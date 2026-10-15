#include "Skin.h"

#include "G2SkinData.h"

#include "g2/clavia/skin.hpp"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace g2ui {
namespace {

const char* findResource(const juce::String& originalName, int& size)
{
    for (int i = 0; i < G2SkinData::namedResourceListSize; ++i) {
        const char* name = G2SkinData::namedResourceList[i];
        if (originalName == G2SkinData::getNamedResourceOriginalFilename(name))
            return G2SkinData::getNamedResource(name, size);
    }
    size = 0;
    return nullptr;
}

// PANL inline images: colon-separated "rrggbb" pixels. A strip of `count`
// frames, each `frameWidth` pixels wide, is stored as the frames one below the
// other; it's returned as a horizontal strip (frame k at x = k * frameWidth).
juce::Image decodeInlineImage(const juce::String& data, int frameWidth, int count)
{
    juce::StringArray px;
    px.addTokens(data, ":", "");
    px.removeEmptyStrings();
    count = std::max(1, count);
    if (frameWidth <= 0 || px.size() < frameWidth * count)
        return {};
    const int frameHeight = px.size() / (frameWidth * count);
    const int framePixels = frameWidth * frameHeight;
    juce::Image img(juce::Image::RGB, frameWidth * count, frameHeight, false);
    for (int i = 0; i < framePixels * count; ++i) {
        const int frame = i / framePixels, j = i % framePixels;
        const auto rgb = static_cast<juce::uint32>(px[i].getHexValue32());
        img.setPixelAt(frame * frameWidth + j % frameWidth, j / frameWidth, juce::Colour(0xff000000u | rgb));
    }
    return img;
}

PanelElement element(const juce::var& v)
{
    PanelElement e;
    e.kind = v["type"].toString();
    e.id = v.getProperty("ID", 0);
    e.x = v.getProperty("XPos", 0);
    e.y = v.getProperty("YPos", 0);
    e.width = v.getProperty("Width", 0);
    e.height = v.getProperty("Height", 0);
    e.codeRef = v.getProperty("CodeRef", -1);
    e.infoFunc = v.getProperty("InfoFunc", 0);
    e.groupId = v.getProperty("GroupId", 0);
    e.textFunc = v.getProperty("Text Func", 0);
    e.graphFunc = v.getProperty("Graph Func", 0);
    e.masterRef = v.getProperty("MasterRef", -1);
    e.type = v.getProperty("Type", {}).toString();
    e.style = v.getProperty("Style", {}).toString();
    e.orientation = v.getProperty("Orientation", {}).toString();
    e.dependencies = v.getProperty("Dependencies", {}).toString();
    e.buttonCount = v.getProperty("ButtonCount", 0);
    e.buttonWidth = v.getProperty("ButtonWidth", 0);
    e.columns = v.getProperty("ButtonColumns", 0);
    e.rows = v.getProperty("ButtonRows", 0);
    e.imageCount = v.getProperty("ImageCount", 0);
    e.imageWidth = v.getProperty("ImageWidth", 0);
    const auto text = v.getProperty("Text", {}).toString();
    if (e.kind != "Text")
        e.options.addTokens(text, ",", "");
    else
        e.options.add(text);
    e.fontSize = static_cast<float>(static_cast<double>(v.getProperty("FontSize", 9)));
    e.length = v.getProperty("Length", 0);
    e.thick = v.getProperty("Width", {}).toString() == "Thick";
    e.zpos = v.getProperty("ZPos", 0);
    return e;
}

// An element's inline image (its pixels from the user's download), as the
// original lays it out: a Bitmap is one frame as wide as the element, a
// button's strip has a frame per state or per button.
juce::Image inlineImage(const PanelElement& e, const std::string& pixels)
{
    const juce::String data(pixels);
    if (e.kind == "Bitmap")
        return decodeInlineImage(data, e.width, 1);
    return decodeInlineImage(data, e.imageWidth, e.imageCount > 0 ? e.imageCount : std::max(1, e.buttonCount));
}

std::filesystem::path toPath(const juce::File& f)
{
    return std::filesystem::path(reinterpret_cast<const char8_t*>(f.getFullPathName().toRawUTF8()));
}

// The cache of the Classic skin's images, in the user's settings folder
// (~/Library/Application Support/G2fresh/ClassicSkin on macOS), or
// G2_CLAVIA_CACHE. Never in the build or the repository.
juce::File classicCacheDir()
{
    if (const char* env = std::getenv("G2_CLAVIA_CACHE"))
        return juce::File(juce::String::fromUTF8(env));
    return userSettings().getFile().getSiblingFile("ClassicSkin");
}

// What the cache was extracted from: the file's path, size and date (a new
// download, or the file replaced, extracts again).
juce::String stampFor(const juce::File& source)
{
    return "G2fresh Classic skin 1\n" + source.getFullPathName() + "\n" + juce::String(source.getSize()) + "\n"
           + juce::String(source.getLastModificationTime().toMilliseconds()) + "\n";
}

bool writePng(const juce::Image& img, const juce::File& f)
{
    f.getParentDirectory().createDirectory();
    juce::FileOutputStream out(f);
    return out.openedOk() && juce::PNGImageFormat().writeImageToStream(img, out);
}

juce::Image toImage(const g2::clavia::Bitmap& b)
{
    juce::Image img(juce::Image::RGB, b.width, b.height, false);
    const juce::Image::BitmapData px(img, juce::Image::BitmapData::writeOnly);
    const auto* p = b.rgb.data();
    for (int y = 0; y < b.height; ++y)
        for (int x = 0; x < b.width; ++x, p += 3)
            px.setPixelColour(x, y, juce::Colour(p[0], p[1], p[2]));
    return img;
}

// Extracts the Classic skin's images from the user's download into `dir`:
// cbmp/<id>.png, smtb/<id>.png, jpeg/<id>.jpg (as stored), the panels' inline
// images as inline/<n>.png (each distinct one once) with inline.txt listing
// "<panel> <element> <n>", and source.txt last. Throws std::runtime_error.
void extractClassicSkin(const juce::File& source, const juce::File& dir, const std::map<int, PanelDef>& panels)
{
    const auto skin = g2::clavia::classicSkin(g2::clavia::editorResources(toPath(source)));
    if (skin.cbmp.empty() || skin.inlineImages.empty())
        throw std::runtime_error("no module graphics in " + source.getFileName().toStdString());
    auto fail = [](const juce::File& f) {
        throw std::runtime_error("cannot write " + f.getFullPathName().toStdString());
    };
    for (const auto& [kind, bitmaps] : {std::pair{"cbmp", &skin.cbmp}, std::pair{"smtb", &skin.smtb}})
        for (const auto& [id, bmp] : *bitmaps)
            if (const auto f = dir.getChildFile(kind).getChildFile(juce::String(id) + ".png");
                !writePng(toImage(bmp), f))
                fail(f);
    for (const auto& [id, bytes] : skin.jpeg) {
        const auto f = dir.getChildFile("jpeg").getChildFile(juce::String(id) + ".jpg");
        f.getParentDirectory().createDirectory();
        if (!f.replaceWithData(bytes.data(), bytes.size()))
            fail(f);
    }
    juce::String index;
    std::map<juce::String, int> distinct;
    for (const auto& [resId, def] : panels) {
        const auto it = skin.inlineImages.find(resId);
        if (it == skin.inlineImages.end() || it->second.size() != def.elements.size())
            continue; // not the same panel as the layout's
        for (std::size_t i = 0; i < def.elements.size(); ++i) {
            const auto& pixels = it->second[i];
            if (pixels.empty())
                continue;
            const auto& e = def.elements[i];
            const auto key = e.kind + ":" + juce::String(e.width) + ":" + juce::String(e.imageWidth) + ":"
                             + juce::String(e.imageCount) + ":" + juce::String(e.buttonCount) + ":" + juce::String(pixels);
            auto [d, added] = distinct.emplace(key, static_cast<int>(distinct.size()));
            if (added) {
                const auto img = inlineImage(e, pixels);
                if (!img.isValid())
                    continue;
                if (const auto f = dir.getChildFile("inline").getChildFile(juce::String(d->second) + ".png");
                    !writePng(img, f))
                    fail(f);
            }
            index << resId << " " << static_cast<int>(i) << " " << d->second << "\n";
        }
    }
    if (!dir.getChildFile("inline.txt").replaceWithText(index))
        fail(dir.getChildFile("inline.txt"));
    if (!dir.getChildFile("source.txt").replaceWithText(stampFor(source)))
        fail(dir.getChildFile("source.txt"));
}

} // namespace

Skin& Skin::get()
{
    static Skin skin;
    return skin;
}

Skin::Skin()
{
    int size = 0;
    const char* data = findResource("panels.json", size);
    if (!data)
        return;
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(data, size));
    if (const auto* list = parsed.getArray()) {
        for (const auto& p : *list) {
            PanelDef def;
            def.resId = p["resId"];
            def.name = p["Name"].toString();
            def.height = p.getProperty("Height", 1);
            if (const auto* children = p["children"].getArray())
                for (const auto& c : *children)
                    def.elements.push_back(element(c));
            panels_.emplace(def.resId, std::move(def));
        }
    }
}

const PanelDef* Skin::panel(int resId) const
{
    const auto it = panels_.find(resId);
    return it == panels_.end() ? nullptr : &it->second;
}

juce::String Skin::loadClassic(const juce::File& source)
{
    if (!source.exists())
        return source.getFullPathName() + " is missing (moved or deleted?)";
    const auto dir = classicCacheDir();
    const auto stamp = stampFor(source);
    if (dir.getChildFile("source.txt").loadFileAsString() != stamp) {
        // Extracted next to the cache, then put in its place: another G2fresh
        // (a second plugin host) may be doing the same.
        const auto tmp =
            dir.getSiblingFile(dir.getFileName() + "-" + juce::String::toHexString(juce::Random().nextInt64()));
        try {
            extractClassicSkin(source, tmp, panels_);
        } catch (const std::exception& e) {
            tmp.deleteRecursively();
            return juce::String::fromUTF8(e.what());
        }
        dir.deleteRecursively();
        if (!tmp.moveFileTo(dir)) {
            tmp.deleteRecursively();
            if (dir.getChildFile("source.txt").loadFileAsString() != stamp)
                return "cannot write " + dir.getFullPathName();
        }
    }
    classicDir_ = dir;
    images_.clear();
    attachInlineImages();
    return {};
}

void Skin::attachInlineImages()
{
    for (auto& [resId, def] : panels_)
        for (auto& e : def.elements)
            e.image = {};
    juce::StringArray lines;
    lines.addLines(classicDir_.getChildFile("inline.txt").loadFileAsString());
    for (const auto& line : lines) {
        juce::StringArray f;
        f.addTokens(line, " ", "");
        if (f.size() != 3)
            continue;
        const auto it = panels_.find(f[0].getIntValue());
        const int i = f[1].getIntValue();
        if (it != panels_.end() && juce::isPositiveAndBelow(i, static_cast<int>(it->second.elements.size())))
            it->second.elements[static_cast<std::size_t>(i)].image = load("inline/" + f[2] + ".png");
    }
}

juce::Image Skin::load(const juce::String& name) const
{
    if (const auto it = images_.find(name); it != images_.end())
        return it->second;
    juce::Image img;
    if (classicLoaded())
        img = juce::ImageFileFormat::loadFrom(classicDir_.getChildFile(name));
    images_.emplace(name, img);
    return img;
}

juce::Image Skin::cbmp(int resId) const { return load("cbmp/" + juce::String(resId) + ".png"); }

juce::Image Skin::tintedFace(int resId, juce::Colour colour) const
{
    const juce::String key = "tint:" + juce::String(resId) + ":" + colour.toString();
    if (const auto it = images_.find(key); it != images_.end())
        return it->second;
    juce::Image out = cbmp(resId).createCopy();
    const auto grey = juce::Colour(0xffbdbdbd); // the faces' background
    for (int y = 0; y < out.getHeight(); ++y)
        for (int x = 0; x < out.getWidth(); ++x)
            if (out.getPixelAt(x, y) == grey)
                out.setPixelAt(x, y, colour);
    images_.emplace(key, out);
    return out;
}

juce::Image Skin::sprite(int resId, int frameWidth) const
{
    const juce::String key = "keyed:" + juce::String(resId);
    if (const auto it = images_.find(key); it != images_.end())
        return it->second;
    // Sprite strips have an opaque background; make each frame's background
    // colour (its top-left pixel) transparent so it blends with the face.
    const juce::Image src = cbmp(resId);
    juce::Image out;
    if (src.isValid() && frameWidth > 0) {
        out = juce::Image(juce::Image::ARGB, src.getWidth(), src.getHeight(), true);
        for (int fx = 0; fx < src.getWidth(); fx += frameWidth) {
            const auto bg = src.getPixelAt(fx, 0);
            for (int y = 0; y < src.getHeight(); ++y)
                for (int x = fx; x < std::min(fx + frameWidth, src.getWidth()); ++x) {
                    const auto c = src.getPixelAt(x, y);
                    out.setPixelAt(x, y, c == bg ? juce::Colours::transparentBlack : c);
                }
        }
    }
    images_.emplace(key, out);
    return out;
}
juce::Image Skin::jpeg(int resId) const { return load("jpeg/" + juce::String(resId) + ".jpg"); }

juce::Image Skin::smtb(int resId) const { return load("smtb/" + juce::String(resId) + ".png"); }

juce::StringArray classicSkinSources()
{
    juce::StringArray paths;
    if (const char* env = std::getenv("G2_CLAVIA_DMG"))
        paths.add(juce::String::fromUTF8(env));
    paths.add(userSettings().getValue("classicLookSource"));
    paths.add(userSettings().getValue("emulatorFirmware"));
    paths.removeEmptyStrings();
    paths.removeDuplicates(false);
    return paths;
}

juce::String ensureClassicSkin()
{
    if (Skin::get().classicLoaded())
        return {};
    juce::String error; // the first source's
    for (const auto& path : classicSkinSources()) {
        const auto e = Skin::get().loadClassic(juce::File(path));
        if (e.isEmpty())
            return {};
        if (error.isEmpty())
            error = "Classic look: " + e;
    }
    return error.isNotEmpty() ? error
                              : "Classic look: it is drawn with the original editor's graphics, from your copy of "
                                "Clavia's free download \"Nord Modular G2 OS v1.62 Update.dmg\"; none is chosen yet";
}

void rememberClassicSkinSource(const juce::File& source)
{
    userSettings().setValue("classicLookSource", source.getFullPathName());
    if (source.hasFileExtension("dmg") && !juce::File(userSettings().getValue("emulatorFirmware")).exists())
        userSettings().setValue("emulatorFirmware", source.getFullPathName());
}

juce::PropertiesFile& userSettings()
{
    static juce::PropertiesFile file([] {
        juce::PropertiesFile::Options o;
        o.applicationName = "G2fresh";
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        o.folderName = "G2fresh";
        return o;
    }());
    return file;
}

// The look starts at its default (modern) on every launch; the View menu
// changes it for the session.
namespace {
Look sessionLook = Look::Modern;
} // namespace

Look currentLook()
{
    if (const char* env = std::getenv("G2_LOOK")) { // for g2render snapshots
        if (!juce::String(env).equalsIgnoreCase("classic"))
            return Look::Modern;
        static const bool classic = [] {
            const auto error = ensureClassicSkin();
            if (error.isNotEmpty())
                std::cerr << "G2_LOOK=classic: " << error << " (set G2_CLAVIA_DMG); drawing the Modern look\n";
            return error.isEmpty();
        }();
        return classic ? Look::Classic : Look::Modern;
    }
    // Classic only with its images (MainView asks for the download).
    return sessionLook == Look::Classic && Skin::get().classicLoaded() ? Look::Classic : Look::Modern;
}

void setCurrentLook(Look look)
{
    sessionLook = look;
}

// The cable flow animation (a G2fresh addition: the original's cables do not
// move). On unless the user turned it off; the choice is kept between launches.
bool cableAnimation()
{
    return userSettings().getBoolValue("cableAnimation", true);
}

void setCableAnimation(bool on)
{
    userSettings().setValue("cableAnimation", on);
}

CableStyle cableStyle()
{
    return cableStyleFromSetting(userSettings().getIntValue("cableStyle", static_cast<int>(kDefaultCableStyle)));
}

void setCableStyle(CableStyle style)
{
    userSettings().setValue("cableStyle", static_cast<int>(style));
}

bool wheelEditsKnobs()
{
    return userSettings().getBoolValue("wheelEditsKnobs", false);
}

void setWheelEditsKnobs(bool on)
{
    userSettings().setValue("wheelEditsKnobs", on);
}

KnobDoubleClick knobDoubleClick()
{
    const auto v = userSettings().getValue("knobDoubleClick", "morph");
    return v == "reset" ? KnobDoubleClick::Reset : v == "nothing" ? KnobDoubleClick::Nothing : KnobDoubleClick::Morph;
}

void setKnobDoubleClick(KnobDoubleClick action)
{
    userSettings().setValue("knobDoubleClick", action == KnobDoubleClick::Reset     ? "reset"
                                               : action == KnobDoubleClick::Nothing ? "nothing"
                                                                                    : "morph");
}

bool morphGroupColours()
{
    return userSettings().getBoolValue("morphGroupColours", false);
}

void setMorphGroupColours(bool on)
{
    userSettings().setValue("morphGroupColours", on);
}

bool showLoadEstimate()
{
    return userSettings().getBoolValue("showLoadEstimate", false);
}

void setShowLoadEstimate(bool on)
{
    userSettings().setValue("showLoadEstimate", on);
}

} // namespace g2ui
