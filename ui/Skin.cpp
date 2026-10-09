#include "Skin.h"

#include "G2SkinData.h"

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
    e.textFunc = v.getProperty("Text Func", 0);
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
    const auto image = v.getProperty("Image", {}).toString();
    if (image.isNotEmpty())
        e.image = decodeInlineImage(image, e.imageWidth,
                                    e.imageCount > 0 ? e.imageCount : std::max(1, e.buttonCount));
    return e;
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

juce::Image Skin::load(const juce::String& name) const
{
    if (const auto it = images_.find(name); it != images_.end())
        return it->second;
    int size = 0;
    juce::Image img;
    if (const char* data = findResource(name, size))
        img = juce::ImageFileFormat::loadFrom(data, static_cast<size_t>(size));
    images_.emplace(name, img);
    return img;
}

juce::Image Skin::cbmp(int resId) const { return load(juce::String(resId) + ".png"); }

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
juce::Image Skin::jpeg(int resId) const { return load(juce::String(resId) + ".jpg"); }

} // namespace g2ui
