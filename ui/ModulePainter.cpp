#include "ModulePainter.h"

#include "PatchDocument.h"
#include "SpecialControls.h"

#include "g2/edit.hpp"
#include "g2/module_db.hpp"
#include "g2/graphs.hpp"
#include "g2/param_text.hpp"

namespace g2ui {
namespace {

// The captions a control shows: a label control's (TextEdit, ButtonRadioEdit)
// come from the patch (custom labels, else the panel's), others from the panel.
// Label bytes from files are shown as Latin-1.
juce::StringArray captions(const ModuleContext& c, const PanelElement& e)
{
    if ((e.kind == "TextEdit" || e.kind == "ButtonRadioEdit") && e.codeRef >= 0) {
        const auto labels = g2::edit::paramLabels(c.patch, c.location, c.module.index, static_cast<std::uint8_t>(e.codeRef));
        if (!labels.empty()) {
            juce::StringArray out;
            for (const auto& l : labels)
                out.add(PatchDocument::fromG2Bytes(l));
            return out;
        }
    }
    return e.options;
}

// Knob sprite strips: 10 frames each (0 = selected, 1 = normal, 2..9 = morph
// groups 1..8).
struct KnobSprite {
    int resId;
    int size;
};

KnobSprite knobSprite(const juce::String& type)
{
    if (type == "Big")
        return {199, 23};
    if (type == "Small" || type == "Reset")
        return {204, 19};
    return {205, 21}; // Medium, Reset/medium
}

bool isSlider(const PanelElement& e) { return e.type == "Slider" || e.type == "SeqSlider"; }

constexpr int kJackSize = 13;
constexpr int kButtonHeight = 12;

juce::Font smallFont(float size = 9.0f)
{
    return juce::Font(juce::FontOptions(size));
}

const g2::MorphAssign* morphOf(const ModuleContext& c, int param)
{
    if (c.variation < 0 || static_cast<std::size_t>(c.variation) >= c.patch.morphs.size())
        return nullptr;
    for (const auto& a : c.patch.morphs[static_cast<std::size_t>(c.variation)].assigns)
        if (a.location == static_cast<std::uint8_t>(c.location) && a.module == c.module.index && a.param == param)
            return &a;
    return nullptr;
}

int morphGroup(const ModuleContext& c, int param)
{
    const auto* a = morphOf(c, param);
    return a ? a->morph : -1;
}

// The arc a morph adds to a knob: from the value to where the morph takes it,
// in the morph group's colour, just outside the knob.
void drawMorphRange(juce::Graphics& g, const ModuleContext& c, int param, juce::Point<float> centre, float radius,
                    int value, int max, float thickness)
{
    const auto* a = morphOf(c, param);
    if (!a || a->range == 0)
        return;
    const float start = juce::degreesToRadians(-135.0f), sweep = juce::degreesToRadians(270.0f);
    const float from = static_cast<float>(value) / static_cast<float>(max);
    const float to = juce::jlimit(0.0f, 1.0f, from + static_cast<float>(a->range) / 127.0f);
    juce::Path arc;
    arc.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, start + sweep * std::min(from, to),
                      start + sweep * std::max(from, to), true);
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.strokePath(arc, juce::PathStrokeType(thickness + 1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    g.setColour(ModulePainter::morphColour(a->morph).brighter(0.1f));
    g.strokePath(arc, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
}

const std::vector<std::uint8_t>* params(const ModuleContext& c);

// Values named by a PANL "Dependencies" string ("0,3,S0"): parameter values
// of the current variation, or mode values for "S<n>"/"s<n>".
std::vector<std::uint8_t> dependencyValues(const ModuleContext& c, const juce::String& dependencies)
{
    juce::StringArray deps;
    deps.addTokens(dependencies, ",", "");
    deps.removeEmptyStrings();
    std::vector<std::uint8_t> out;
    const auto* p = params(c);
    for (const auto& d : deps) {
        const bool isMode = d.startsWithIgnoreCase("S");
        const int i = (isMode ? d.substring(1) : d).getIntValue();
        const std::vector<std::uint8_t>* src = isMode ? &c.module.modes : p;
        out.push_back(src && i >= 0 && static_cast<std::size_t>(i) < src->size() ? (*src)[static_cast<std::size_t>(i)] : 0);
    }
    return out;
}

const std::vector<std::uint8_t>* params(const ModuleContext& c)
{
    if (c.module.params.empty())
        return nullptr;
    const auto v = static_cast<std::size_t>(juce::jlimit(0, static_cast<int>(c.module.params.size()) - 1, c.variation));
    return &c.module.params[v];
}

void drawImageFrame(juce::Graphics& g, const juce::Image& strip, int frame, int frameWidth, juce::Rectangle<int> dest)
{
    if (!strip.isValid() || frameWidth <= 0)
        return;
    g.drawImage(strip, dest.getX(), dest.getY(), dest.getWidth(), dest.getHeight(), frame * frameWidth, 0,
                frameWidth, strip.getHeight());
}

void drawButtonBox(juce::Graphics& g, juce::Rectangle<int> r, bool on, bool highlighted)
{
    g.setColour(on ? juce::Colour(0xff8c8c8c) : juce::Colour(0xffcecbce));
    g.fillRect(r);
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.drawRect(r);
    if (highlighted) {
        g.setColour(juce::Colours::red);
        g.drawRect(r, 1);
    }
}

void drawLabel(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, juce::Colour colour)
{
    g.setColour(colour);
    g.setFont(smallFont());
    g.drawFittedText(text, r, juce::Justification::centred, 1, 0.8f);
}

} // namespace

namespace {

juce::Colour inkColour(g2::graphs::Ink ink)
{
    const auto rgb = g2::graphs::color(ink);
    return juce::Colour(rgb.r, rgb.g, rgb.b);
}

void drawGraphText(juce::Graphics& g, const g2::graphs::Drawing& d, juce::Point<int> origin, juce::Colour colour,
                   float size)
{
    for (const auto& op : d.ops)
        if (op.kind == g2::graphs::Op::Kind::Text) {
            g.setColour(colour);
            g.setFont(juce::Font(juce::FontOptions(size)));
            g.drawSingleLineText(juce::String(op.text), origin.x + op.x0, origin.y + op.y0);
        }
}

} // namespace

// The original's graph, pixel for pixel.
void paintClassicGraph(juce::Graphics& g, const ModuleContext& c, const PanelElement& e, juce::Rectangle<int> r)
{
    if (!g2::graphs::isGraph(e.graphFunc) || r.isEmpty())
        return;
    const auto d = g2::graphs::render(e.graphFunc, dependencyValues(c, e.dependencies), r.getWidth(), r.getHeight());
    const auto pixels = g2::graphs::rasterize(d);
    if (pixels.size() != static_cast<std::size_t>(d.width * d.height))
        return;
    juce::Image img(juce::Image::RGB, d.width, d.height, false);
    for (int y = 0; y < d.height; ++y)
        for (int x = 0; x < d.width; ++x)
            img.setPixelAt(x, y, inkColour(pixels[static_cast<std::size_t>(y * d.width + x)]));
    g.drawImageAt(img, r.getX(), r.getY());
    drawGraphText(g, d, r.getPosition(), inkColour(g2::graphs::Ink::Text), 9.0f);
}

// The graph as vectors, in the modern palette.
void paintModernGraph(juce::Graphics& g, const ModuleContext& c, const PanelElement& e, juce::Rectangle<float> r)
{
    using g2::graphs::Ink;
    if (!g2::graphs::isGraph(e.graphFunc) || r.isEmpty())
        return;
    const auto values = dependencyValues(c, e.dependencies);
    const auto lines = g2::graphs::draw(e.graphFunc, values, r.getWidth(), r.getHeight());
    auto modern = [](Ink ink) {
        switch (ink) {
        case Ink::Back: return juce::Colour(0xff1f262d);
        case Ink::BackLine: return juce::Colour(0xff4c5865);
        case Ink::Border: return juce::Colour(0xff7fd8ff);
        case Ink::Single: return juce::Colour(0xff6ee7a8);
        case Ink::Base: return juce::Colour(0xffffc65c);
        case Ink::Shade: return juce::Colour(0xff34404b);
        case Ink::Fill: return juce::Colour(0x557fd8ff);
        case Ink::EnvFill: return juce::Colour(0x446ee7a8);
        case Ink::Text: return juce::Colour(0xffffc65c);
        case Ink::Frame: return juce::Colour(0x00000000);
        }
        return juce::Colours::grey;
    };
    juce::Graphics::ScopedSaveState state(g);
    juce::Path clip;
    clip.addRoundedRectangle(r, 3.0f);
    g.setColour(modern(Ink::Back));
    g.fillPath(clip);
    g.reduceClipRegion(clip);
    for (const auto& line : lines) {
        if (line.points.empty() || line.ink == Ink::Frame || line.ink == Ink::Back)
            continue;
        juce::Path p;
        p.startNewSubPath(r.getX() + line.points[0].first, r.getY() + line.points[0].second);
        for (std::size_t i = 1; i < line.points.size(); ++i)
            p.lineTo(r.getX() + line.points[i].first, r.getY() + line.points[i].second);
        if (line.closed)
            p.closeSubPath();
        g.setColour(modern(line.ink));
        if (line.filled)
            g.fillPath(p);
        else if (line.points.size() == 1)
            g.fillRect(r.getX() + line.points[0].first - 0.5f, r.getY() + line.points[0].second - 0.5f, 1.0f, 1.0f);
        else
            g.strokePath(p, juce::PathStrokeType(line.ink == Ink::BackLine ? 1.0f : 1.4f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }
    const auto d = g2::graphs::render(e.graphFunc, values, juce::roundToInt(r.getWidth()), juce::roundToInt(r.getHeight()));
    drawGraphText(g, d, r.getPosition().toInt(), modern(Ink::Text), 9.0f);
}

const PanelDef* ModulePainter::panelFor(const g2::Module& m)
{
    const auto* def = m.def();
    return def ? Skin::get().panel(def->panelResId) : nullptr;
}

juce::Rectangle<int> ModulePainter::moduleBounds(const g2::Module& m)
{
    const auto* def = m.def();
    const int height = def && def->height > 0 ? def->height : 2;
    return {m.col * kModuleWidth, m.row * kRowHeight, kModuleWidth, height * kRowHeight};
}

bool ModulePainter::isControl(const PanelElement& e)
{
    static const juce::StringArray kinds{"Knob", "ButtonText", "ButtonFlat", "ButtonRadio", "ButtonIncDec",
                                         "ButtonRadioEdit", "TextEdit", "LevelShift", "PartSelector"};
    return kinds.contains(e.kind) && e.codeRef >= 0;
}

juce::Rectangle<int> ModulePainter::elementBounds(const PanelElement& e)
{
    if (e.kind == "Knob") {
        if (isSlider(e))
            return {e.x, e.y, 11, e.type == "SeqSlider" ? 75 : 51};
        const int s = knobSprite(e.type).size;
        return {e.x, e.y, s, s};
    }
    if (isJack(e))
        return {e.x, e.y, kJackSize, kJackSize};
    if (e.kind == "ButtonRadio") {
        const int n = std::max(1, e.buttonCount);
        const int w = e.buttonWidth > 0 ? e.buttonWidth : 16;
        return e.orientation == "Vertical" ? juce::Rectangle<int>{e.x, e.y, w, n * kButtonHeight}
                                           : juce::Rectangle<int>{e.x, e.y, n * w, kButtonHeight};
    }
    if (e.kind == "ButtonRadioEdit")
        return {e.x, e.y, std::max(1, e.columns) * 14, std::max(1, e.rows) * kButtonHeight};
    if (e.kind == "ButtonIncDec")
        return e.type == "Left/Right" ? juce::Rectangle<int>{e.x, e.y, 26, 10} : juce::Rectangle<int>{e.x, e.y, 13, 16};
    if (e.kind == "PartSelector")
        return {e.x, e.y, std::max(e.width, 8), std::max(e.height, 8)};
    if (e.kind == "Led")
        return {e.x, e.y, 7, 7};
    if (e.kind == "MiniVU")
        return {e.x, e.y, 8, 16};
    if (e.kind == "Graph")
        return {e.x, e.y, e.width, e.height};
    if (e.kind == "LevelShift")
        return {e.x, e.y, 14, 14};
    // ButtonText, ButtonFlat, TextEdit, TextField
    return {e.x, e.y, std::max(e.width, 8), kButtonHeight};
}

std::optional<int> ModulePainter::value(const ModuleContext& c, const PanelElement& e)
{
    if (e.kind == "PartSelector") {
        if (e.codeRef >= 0 && static_cast<std::size_t>(e.codeRef) < c.module.modes.size())
            return c.module.modes[static_cast<std::size_t>(e.codeRef)];
        return std::nullopt;
    }
    const int ref = e.kind == "TextField" ? e.masterRef : e.codeRef;
    const auto* p = params(c);
    if (!p || ref < 0 || static_cast<std::size_t>(ref) >= p->size())
        return std::nullopt;
    return (*p)[static_cast<std::size_t>(ref)];
}

int ModulePainter::maxValue(const ModuleContext& c, const PanelElement& e)
{
    const auto* def = c.module.def();
    if (!def)
        return 127;
    if (e.kind == "PartSelector")
        return e.codeRef >= 0 && static_cast<std::size_t>(e.codeRef) < def->modes.size()
            ? def->modes[static_cast<std::size_t>(e.codeRef)].max : 0;
    const int ref = e.kind == "TextField" ? e.masterRef : e.codeRef;
    return ref >= 0 && static_cast<std::size_t>(ref) < def->params.size() ? def->params[static_cast<std::size_t>(ref)].max : 127;
}

juce::String ModulePainter::valueText(const ModuleContext& c, const PanelElement& e)
{
    const auto* def = c.module.def();
    const auto* p = params(c);
    if (!def)
        return {};
    if (e.kind == "PartSelector") {
        const auto v = value(c, e);
        if (!v || static_cast<std::size_t>(e.codeRef) >= def->modes.size())
            return {};
        return g2::paramtext::formatSingle(def->modes[static_cast<std::size_t>(e.codeRef)].textFunc,
                                           static_cast<std::uint8_t>(*v));
    }
    if (!p)
        return {};
    if (e.kind == "TextField" && e.dependencies.isNotEmpty())
        return g2::paramtext::format(e.textFunc, dependencyValues(c, e.dependencies));
    const int ref = e.kind == "TextField" ? e.masterRef : e.codeRef;
    if (ref < 0)
        return {};
    return g2::db::formatParam(*def, static_cast<std::size_t>(ref), *p, c.module.modes);
}

juce::Colour ModulePainter::jackColour(const ModuleContext& c, const PanelElement& e)
{
    const auto* def = c.module.def();
    const bool out = e.kind == "Output";
    if (def && e.codeRef >= 0) {
        const auto& list = out ? def->outputs : def->inputs;
        if (static_cast<std::size_t>(e.codeRef) < list.size()) {
            using CC = g2::db::ConnColor;
            switch (list[static_cast<std::size_t>(e.codeRef)].color) {
            case CC::Red: return cableColour(g2::CableColor::Red);
            case CC::Blue: return cableColour(g2::CableColor::Blue);
            case CC::Yellow: return cableColour(g2::CableColor::Yellow);
            case CC::BlueRed: return cableColour(c.module.uprate ? g2::CableColor::Red : g2::CableColor::Blue);
            case CC::YellowOrange: return cableColour(c.module.uprate ? g2::CableColor::Orange : g2::CableColor::Yellow);
            }
        }
    }
    return e.type == "Logic" ? cableColour(g2::CableColor::Yellow)
                             : e.type == "Control" ? cableColour(g2::CableColor::Blue) : cableColour(g2::CableColor::Red);
}

juce::Colour ModulePainter::cableColour(g2::CableColor color)
{
    switch (color) {
    case g2::CableColor::Red: return juce::Colour(0xffe00000);
    case g2::CableColor::Blue: return juce::Colour(0xff2040e0);
    case g2::CableColor::Yellow: return juce::Colour(0xffe8d800);
    case g2::CableColor::Orange: return juce::Colour(0xffff9000);
    case g2::CableColor::Green: return juce::Colour(0xff10c010);
    case g2::CableColor::Purple: return juce::Colour(0xffa020c0);
    case g2::CableColor::White: return juce::Colour(0xfff0f0f0);
    }
    return juce::Colours::grey;
}

juce::Colour ModulePainter::moduleColour(int index)
{
    static const juce::uint32 colours[kModuleColours] = {
        0xffc0c0c0, 0xffccbaba, 0xffbaccba, 0xffb0bacc, 0xffd0cbaa, 0xff74a0d4, 0xffe5777a, 0xff7bc1bd, 0xff82b980,
        0xffe7d14b, 0xff93d162, 0xffdec77d, 0xff8f9ac2, 0xffba7d81, 0xffca8d8d, 0xffded1a5, 0xff94cf9c, 0xff69d6c7,
        0xffa0d2c8, 0xffbed2d2, 0xff808cc0, 0xffd673c7, 0xffbe82be, 0xffcda0d2, 0xffd2bed2};
    return juce::Colour(colours[juce::isPositiveAndBelow(index, kModuleColours) ? index : 0]);
}

const std::array<int, ModulePainter::kModuleColours>& ModulePainter::moduleColourMenuOrder()
{
    static const std::array<int, kModuleColours> order{0, 6, 13, 14, 1, 9, 11, 15, 4, 10, 8, 16, 2,
                                                       17, 7, 18, 19, 5, 20, 12, 3, 21, 22, 23, 24};
    return order;
}

juce::Colour ModulePainter::morphColour(int group)
{
    static const juce::uint32 colours[8] = {0xffe5a1a1, 0xffc5dac5, 0xffa1a1e5, 0xffdadac5,
                                            0xffd87093, 0xff758e40, 0xff88cccc, 0xfff29664};
    return juce::Colour(colours[juce::jlimit(0, 7, group)]);
}

void ModulePainter::paint(juce::Graphics& g, const ModuleContext& c, const PanelElement* highlighted)
{
    if (c.look == Look::Modern) {
        paintModern(g, c, highlighted);
        return;
    }
    const auto* def = c.module.def();
    const auto bounds = moduleBounds(c.module).withZeroOrigin();
    const auto face = !def ? juce::Image()
                      : c.module.color == 0 ? Skin::get().cbmp(def->faceResId)
                                            : Skin::get().tintedFace(def->faceResId, moduleColour(c.module.color));
    if (face.isValid()) {
        g.drawImageAt(face, 0, 0);
    } else {
        g.setColour(juce::Colour(0xffc8c8c8));
        g.fillRect(bounds);
        g.setColour(juce::Colours::black);
        g.drawRect(bounds);
    }
    // Module name in the title strip.
    g.setColour(juce::Colours::black);
    g.setFont(smallFont(10.0f));
    g.drawText(juce::String(c.module.name), 3, 0, 120, 13, juce::Justification::centredLeft, true);

    if (c.panel)
        for (const auto& e : c.panel->elements)
            paintElement(g, c, e, &e == highlighted);
}

void ModulePainter::paintElement(juce::Graphics& g, const ModuleContext& c, const PanelElement& e, bool highlighted)
{
    const auto r = elementBounds(e);
    const auto v = value(c, e);
    const int max = std::max(1, maxValue(c, e));

    if (SpecialControls::isSeqSlider(e)) {
        SpecialControls::paintSeqSlider(g, c, e, highlighted);
        return;
    }
    if (e.kind == "Knob") {
        const int val = v.value_or(0);
        if (isSlider(e)) {
            g.setColour(juce::Colour(0xff505050));
            g.fillRect(r.reduced(4, 0));
            const int travel = r.getHeight() - 6;
            const int y = r.getBottom() - 6 - travel * val / max;
            g.setColour(highlighted ? juce::Colours::red : juce::Colour(0xffe0e0e0));
            g.fillRect(r.getX(), y, r.getWidth(), 6);
            g.setColour(juce::Colours::black);
            g.drawRect(r.getX(), y, r.getWidth(), 6);
            return;
        }
        const auto sprite = knobSprite(e.type);
        const int group = morphGroup(c, e.codeRef);
        const int frame = highlighted ? 0 : group >= 0 ? 2 + group : 1;
        drawImageFrame(g, Skin::get().sprite(sprite.resId, sprite.size), frame, sprite.size, r);
        const auto centre = r.toFloat().getCentre();
        const float angle = juce::degreesToRadians(-135.0f + 270.0f * static_cast<float>(val) / static_cast<float>(max));
        const float radius = static_cast<float>(sprite.size) * 0.5f - 2.5f;
        g.setColour(juce::Colours::black);
        g.drawLine(centre.x, centre.y, centre.x + radius * std::sin(angle), centre.y - radius * std::cos(angle), 2.0f);
        drawMorphRange(g, c, e.codeRef, centre, static_cast<float>(sprite.size) * 0.5f + 1.5f, val, max, 2.5f);
        return;
    }
    if (isJack(e)) {
        const auto colour = jackColour(c, e);
        const auto rf = r.toFloat();
        g.setColour(colour);
        if (e.kind == "Output")
            g.fillRect(rf);
        else
            g.fillEllipse(rf);
        g.setColour(juce::Colours::black);
        if (e.kind == "Output")
            g.drawRect(rf, 1.0f);
        else
            g.drawEllipse(rf.reduced(0.5f), 1.0f);
        g.setColour(juce::Colour(0xff202020));
        g.fillEllipse(rf.reduced(4.0f));
        if (highlighted) {
            g.setColour(juce::Colours::white);
            g.drawEllipse(rf.expanded(1.5f), 1.5f);
        }
        return;
    }
    if (e.kind == "TextField") {
        g.setColour(juce::Colour(0xfff4f4f0));
        g.fillRect(r);
        g.setColour(juce::Colour(0xff606060));
        g.drawRect(r);
        drawLabel(g, r.reduced(1, 0), valueText(c, e), juce::Colours::black);
        return;
    }
    if (e.kind == "ButtonText" || e.kind == "TextEdit") {
        const bool on = v.value_or(0) != 0;
        drawButtonBox(g, r, on, highlighted);
        if (e.image.isValid())
            g.drawImageAt(e.image, r.getCentreX() - e.image.getWidth() / 2, r.getCentreY() - e.image.getHeight() / 2);
        else
            drawLabel(g, r, captions(c, e).joinIntoString(","), juce::Colours::black);
        return;
    }
    if (e.kind == "ButtonFlat" || e.kind == "LevelShift") {
        drawButtonBox(g, r, false, highlighted);
        const int val = v.value_or(0);
        if (e.image.isValid() && e.imageCount > 0) {
            const int fh = e.image.getHeight();
            drawImageFrame(g, e.image, juce::jlimit(0, e.imageCount - 1, val), e.imageWidth,
                           r.withSizeKeepingCentre(e.imageWidth, fh));
        } else {
            const auto text = juce::isPositiveAndBelow(val, e.options.size()) ? e.options[val] : juce::String(val);
            drawLabel(g, r, text, juce::Colours::black);
        }
        return;
    }
    if (e.kind == "ButtonRadio" || e.kind == "ButtonRadioEdit") {
        const bool vertical = e.orientation == "Vertical";
        const int n = e.kind == "ButtonRadio" ? std::max(1, e.buttonCount) : std::max(1, e.columns * e.rows);
        const int cols = e.kind == "ButtonRadio" ? (vertical ? 1 : n) : std::max(1, e.columns);
        const int w = r.getWidth() / cols, h = r.getHeight() / std::max(1, (n + cols - 1) / cols);
        const auto texts = captions(c, e);
        for (int i = 0; i < n; ++i) {
            const juce::Rectangle<int> b(r.getX() + (i % cols) * w, r.getY() + (i / cols) * h, w, h);
            drawButtonBox(g, b, v.value_or(-1) == i, highlighted && v.value_or(-1) == i);
            if (e.image.isValid() && e.imageWidth > 0)
                drawImageFrame(g, e.image, i, e.imageWidth, b.withSizeKeepingCentre(e.imageWidth, e.image.getHeight()));
            else if (i < texts.size())
                drawLabel(g, b, texts[i], juce::Colours::black);
        }
        return;
    }
    if (e.kind == "ButtonIncDec") {
        const auto sprite = Skin::get().cbmp(e.type == "Left/Right" ? 202 : 201);
        if (sprite.isValid())
            g.drawImage(sprite, r.toFloat(), juce::RectanglePlacement::stretchToFit);
        else
            drawButtonBox(g, r, false, highlighted);
        return;
    }
    if (e.kind == "PartSelector") {
        g.setColour(juce::Colour(0xffe6e6e6));
        g.fillRect(r);
        if (e.image.isValid() && e.imageCount > 0)
            drawImageFrame(g, e.image, juce::jlimit(0, e.imageCount - 1, v.value_or(0)), e.imageWidth,
                           r.withWidth(e.imageWidth).withHeight(e.image.getHeight()));
        else
            drawLabel(g, r, valueText(c, e), juce::Colours::black);
        g.setColour(highlighted ? juce::Colours::red : juce::Colour(0xff606060));
        g.drawRect(r);
        // Drop-down arrow.
        juce::Path arrow;
        const float ax = static_cast<float>(r.getRight()) - 6.0f, ay = static_cast<float>(r.getCentreY());
        arrow.addTriangle(ax - 3.0f, ay - 2.0f, ax + 3.0f, ay - 2.0f, ax, ay + 2.0f);
        g.setColour(juce::Colours::black);
        g.fillPath(arrow);
        return;
    }
    if (e.kind == "Led") {
        const auto sprite = Skin::get().jpeg(e.type == "Sequencer" ? 273 : 217); // "off" states
        if (sprite.isValid())
            g.drawImageAt(sprite, r.getX(), r.getY());
        return;
    }
    if (e.kind == "MiniVU") {
        g.setColour(juce::Colour(0xff203020));
        g.fillRect(r);
        return;
    }
    if (SpecialControls::isSpecial(e)) {
        SpecialControls::paint(g, c, e, highlighted);
        return;
    }
    if (e.kind == "Graph") {
        paintClassicGraph(g, c, e, r);
        return;
    }
    // Text, Line, Symbol and Bitmap are part of the face bitmap.
}

// ---- Modern look: everything drawn as vectors -------------------------------

namespace {

const juce::Colour kInk(0xff2b2d33);        // labels and pointers
const juce::Colour kSubtle(0xff8a8f99);     // decorative lines
const juce::Colour kControl(0xfffafafb);    // knob caps, buttons
const juce::Colour kControlEdge(0xffa3a8b2);
const juce::Colour kAccent(0xff3478d4);     // values, active buttons
const juce::Colour kHighlight(0xffff8c1a);  // hovered control

juce::Colour categoryColour(const g2::db::ModuleDef* def)
{
    const auto cats = g2::db::categories();
    if (!def || def->category < 0 || static_cast<std::size_t>(def->category) >= cats.size())
        return juce::Colour(0xff9096a0);
    const auto& c = cats[static_cast<std::size_t>(def->category)];
    return juce::Colour(c.r, c.g, c.b);
}

void roundedButton(juce::Graphics& g, juce::Rectangle<float> r, bool on, bool highlighted, float radius = 3.0f)
{
    g.setColour(on ? kAccent : kControl);
    g.fillRoundedRectangle(r, radius);
    g.setColour(highlighted ? kHighlight : on ? kAccent.darker(0.3f) : kControlEdge);
    g.drawRoundedRectangle(r.reduced(0.5f), radius, highlighted ? 1.5f : 1.0f);
}

void centredText(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, juce::Colour colour,
                 float size = 9.5f)
{
    g.setColour(colour);
    g.setFont(juce::Font(juce::FontOptions(size)));
    g.drawFittedText(text, r.toNearestInt(), juce::Justification::centred, 1, 0.75f);
}

// Draws a frame of an original 1-bit-style icon as a glyph: dark pixels in
// `colour`, light background transparent, centred on `centre`.
void drawGlyph(juce::Graphics& g, const juce::Image& strip, int frame, int frameWidth, juce::Point<float> centre,
               juce::Colour colour)
{
    if (!strip.isValid() || frameWidth <= 0)
        return;
    const int h = strip.getHeight();
    const float x0 = std::round(centre.x - static_cast<float>(frameWidth) * 0.5f);
    const float y0 = std::round(centre.y - static_cast<float>(h) * 0.5f);
    g.setColour(colour);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < frameWidth; ++x) {
            const float b = strip.getPixelAt(frame * frameWidth + x, y).getBrightness();
            if (b < 0.6f) {
                g.setOpacity(b < 0.3f ? 1.0f : 0.55f);
                g.fillRect(x0 + static_cast<float>(x), y0 + static_cast<float>(y), 1.0f, 1.0f);
            }
        }
    g.setOpacity(1.0f);
}

void chevron(juce::Graphics& g, juce::Point<float> c, float size, float angle, juce::Colour colour)
{
    juce::Path p;
    p.startNewSubPath(-size, -size * 0.5f);
    p.lineTo(0.0f, size * 0.5f);
    p.lineTo(size, -size * 0.5f);
    g.setColour(colour);
    g.strokePath(p, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                 juce::AffineTransform::rotation(angle).translated(c));
}

} // namespace

void ModulePainter::paintModern(juce::Graphics& g, const ModuleContext& c, const PanelElement* highlighted)
{
    const auto* def = c.module.def();
    const auto body = moduleBounds(c.module).withZeroOrigin().toFloat().reduced(1.5f, 1.0f);
    const auto accent = categoryColour(def);

    // Body: soft vertical gradient, rounded corners, hairline border.
    // Default modules are light grey; a module colour tints the body.
    const auto top = c.module.color == 0 ? juce::Colour(0xffeef0f3)
                                         : moduleColour(c.module.color).interpolatedWith(juce::Colours::white, 0.55f);
    const auto bottom = c.module.color == 0 ? juce::Colour(0xffdfe2e7)
                                            : moduleColour(c.module.color).interpolatedWith(juce::Colours::white, 0.35f);
    g.setGradientFill(juce::ColourGradient(top, 0.0f, body.getY(), bottom, 0.0f, body.getBottom(), false));
    g.fillRoundedRectangle(body, 5.0f);
    // Category accent along the top edge.
    {
        juce::Graphics::ScopedSaveState state(g);
        juce::Path clip;
        clip.addRoundedRectangle(body, 5.0f);
        g.reduceClipRegion(clip);
        g.setColour(accent);
        g.fillRect(body.withHeight(2.5f));
    }
    g.setColour(juce::Colour(0xff9ea3ad));
    g.drawRoundedRectangle(body, 5.0f, 1.0f);

    g.setColour(kInk);
    g.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
    g.drawText(juce::String(c.module.name), 5, 2, 130, 12, juce::Justification::centredLeft, true);

    if (!c.panel)
        return;
    // Decorations first (by ZPos), then live controls on top.
    std::vector<const PanelElement*> decor;
    for (const auto& e : c.panel->elements)
        if (e.kind == "Text" || e.kind == "Line" || e.kind == "Symbol" || e.kind == "Bitmap")
            decor.push_back(&e);
    std::stable_sort(decor.begin(), decor.end(), [](auto* a, auto* b) { return a->zpos < b->zpos; });
    for (const auto* e : decor)
        paintModernElement(g, c, *e, false);
    for (const auto& e : c.panel->elements)
        if (e.kind != "Text" && e.kind != "Line" && e.kind != "Symbol" && e.kind != "Bitmap")
            paintModernElement(g, c, e, &e == highlighted);
}

void ModulePainter::paintModernElement(juce::Graphics& g, const ModuleContext& c, const PanelElement& e,
                                       bool highlighted)
{
    const auto r = elementBounds(e).toFloat();
    const auto v = value(c, e);
    const int max = std::max(1, maxValue(c, e));

    if (e.kind == "Text") {
        const auto text = e.options.isEmpty() ? juce::String() : e.options[0];
        g.setColour(kInk.withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions(e.fontSize + 0.5f)));
        g.drawSingleLineText(text, e.x, e.y + juce::roundToInt(e.fontSize));
        return;
    }
    if (e.kind == "Line") {
        g.setColour(kSubtle);
        const float w = e.thick ? 2.0f : 1.0f;
        if (e.orientation == "Vertical")
            g.fillRoundedRectangle(static_cast<float>(e.x), static_cast<float>(e.y), w, static_cast<float>(e.length), w * 0.5f);
        else
            g.fillRoundedRectangle(static_cast<float>(e.x), static_cast<float>(e.y), static_cast<float>(e.length), w, w * 0.5f);
        return;
    }
    if (e.kind == "Bitmap") {
        if (e.image.isValid())
            drawGlyph(g, e.image, 0, e.image.getWidth(),
                      juce::Rectangle<float>(static_cast<float>(e.x), static_cast<float>(e.y),
                                             static_cast<float>(e.image.getWidth()), static_cast<float>(e.image.getHeight())).getCentre(),
                      kInk.withAlpha(0.85f));
        return;
    }
    if (e.kind == "Symbol") {
        const auto s = juce::Rectangle<float>(static_cast<float>(e.x), static_cast<float>(e.y),
                                              static_cast<float>(std::max(e.width, 3)), static_cast<float>(std::max(e.height, 3)));
        g.setColour(kSubtle);
        if (e.type == "Box") {
            g.drawRoundedRectangle(s, 2.0f, 1.0f);
        } else if (e.type == "Amplifier") {
            juce::Path p;
            p.addTriangle(s.getX(), s.getY(), s.getX(), s.getBottom(), s.getRight(), s.getCentreY());
            g.strokePath(p, juce::PathStrokeType(1.0f));
        } else { // Trig 1 / Trig 2: a small pulse
            juce::Path p;
            p.startNewSubPath(s.getX(), s.getBottom());
            p.lineTo(s.getX(), s.getY());
            p.lineTo(s.getRight(), s.getY());
            p.lineTo(s.getRight(), s.getBottom());
            g.strokePath(p, juce::PathStrokeType(1.0f));
        }
        return;
    }

    if (SpecialControls::isSeqSlider(e)) {
        SpecialControls::paintSeqSlider(g, c, e, highlighted);
        return;
    }
    if (e.kind == "Knob") {
        const int val = v.value_or(0);
        const int group = morphGroup(c, e.codeRef);
        const auto ring = group >= 0 ? morphColour(group).darker(0.15f) : kAccent;
        if (isSlider(e)) {
            const auto track = r.withSizeKeepingCentre(4.0f, r.getHeight());
            g.setColour(juce::Colour(0xffc4c8cf));
            g.fillRoundedRectangle(track, 2.0f);
            const float travel = r.getHeight() - 8.0f;
            const float y = r.getBottom() - 8.0f - travel * static_cast<float>(val) / static_cast<float>(max);
            g.setColour(ring);
            g.fillRoundedRectangle(track.withTop(y + 4.0f), 2.0f);
            roundedButton(g, {r.getX(), y, r.getWidth(), 8.0f}, false, highlighted, 2.5f);
            return;
        }
        const float size = static_cast<float>(knobSprite(e.type).size) - 2.0f;
        const auto area = r.withSizeKeepingCentre(size, size);
        const auto centre = area.getCentre();
        const float radius = size * 0.5f;
        const float start = juce::degreesToRadians(-135.0f);
        const float angle = start + juce::degreesToRadians(270.0f) * static_cast<float>(val) / static_cast<float>(max);
        // Track and value arc.
        juce::Path track, arc;
        track.addCentredArc(centre.x, centre.y, radius - 1.0f, radius - 1.0f, 0.0f, start, -start, true);
        arc.addCentredArc(centre.x, centre.y, radius - 1.0f, radius - 1.0f, 0.0f, start, angle, true);
        g.setColour(juce::Colour(0xffc4c8cf));
        g.strokePath(track, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(highlighted ? kHighlight : ring);
        g.strokePath(arc, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        // Cap and pointer.
        const float cap = radius - 3.5f;
        g.setColour(group >= 0 ? morphColour(group).brighter(0.35f) : kControl);
        g.fillEllipse(centre.x - cap, centre.y - cap, cap * 2.0f, cap * 2.0f);
        g.setColour(kControlEdge);
        g.drawEllipse(centre.x - cap, centre.y - cap, cap * 2.0f, cap * 2.0f, 1.0f);
        g.setColour(kInk);
        g.drawLine(centre.x + (cap * 0.25f) * std::sin(angle), centre.y - (cap * 0.25f) * std::cos(angle),
                   centre.x + (cap - 1.0f) * std::sin(angle), centre.y - (cap - 1.0f) * std::cos(angle), 1.8f);
        drawMorphRange(g, c, e.codeRef, centre, radius + 1.5f, val, max, 2.6f);
        return;
    }
    if (isJack(e)) {
        const auto colour = jackColour(c, e);
        const auto jr = r.reduced(0.5f);
        if (e.kind == "Output") {
            g.setColour(colour);
            g.fillRoundedRectangle(jr, 3.5f);
        } else {
            g.setColour(colour);
            g.fillEllipse(jr);
        }
        g.setColour(juce::Colour(0xff1d1f24));
        g.fillEllipse(jr.reduced(3.5f));
        if (highlighted) {
            g.setColour(kHighlight);
            if (e.kind == "Output")
                g.drawRoundedRectangle(jr.expanded(1.5f), 4.5f, 1.5f);
            else
                g.drawEllipse(jr.expanded(1.5f), 1.5f);
        }
        return;
    }
    if (e.kind == "TextField") {
        g.setColour(juce::Colours::white);
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(juce::Colour(0xffc3c7ce));
        g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
        centredText(g, r.reduced(2.0f, 0.0f), valueText(c, e), kInk);
        return;
    }
    if (e.kind == "ButtonText" || e.kind == "TextEdit") {
        const bool on = v.value_or(0) != 0;
        roundedButton(g, r, on, highlighted);
        if (e.image.isValid()) {
            drawGlyph(g, e.image, 0, e.image.getWidth(), r.getCentre(), on ? juce::Colours::white : kInk);
        } else {
            centredText(g, r, captions(c, e).joinIntoString(","), on ? juce::Colours::white : kInk);
        }
        return;
    }
    if (e.kind == "ButtonFlat" || e.kind == "LevelShift") {
        roundedButton(g, r, false, highlighted);
        const int val = v.value_or(0);
        if (e.image.isValid() && e.imageCount > 0)
            drawGlyph(g, e.image, juce::jlimit(0, e.imageCount - 1, val), e.imageWidth, r.getCentre(), kInk);
        else
            centredText(g, r, juce::isPositiveAndBelow(val, e.options.size()) ? e.options[val] : juce::String(val), kInk);
        return;
    }
    if (e.kind == "ButtonRadio" || e.kind == "ButtonRadioEdit") {
        const bool vertical = e.orientation == "Vertical";
        const int n = e.kind == "ButtonRadio" ? std::max(1, e.buttonCount) : std::max(1, e.columns * e.rows);
        const int cols = e.kind == "ButtonRadio" ? (vertical ? 1 : n) : std::max(1, e.columns);
        const int rows = (n + cols - 1) / cols;
        const float w = r.getWidth() / static_cast<float>(cols), h = r.getHeight() / static_cast<float>(rows);
        // A segmented control: one rounded outline, active segment filled.
        g.setColour(kControl);
        g.fillRoundedRectangle(r, 3.0f);
        const auto texts = captions(c, e);
        for (int i = 0; i < n; ++i) {
            const juce::Rectangle<float> b(r.getX() + static_cast<float>(i % cols) * w,
                                           r.getY() + static_cast<float>(i / cols) * h, w, h);
            const bool on = v.value_or(-1) == i;
            if (on) {
                g.setColour(kAccent);
                g.fillRoundedRectangle(b.reduced(1.0f), 2.5f);
            }
            if (e.image.isValid() && e.imageWidth > 0) {
                drawGlyph(g, e.image, i, e.imageWidth, b.getCentre(), on ? juce::Colours::white : kInk);
            } else if (i < texts.size()) {
                centredText(g, b, texts[i], on ? juce::Colours::white : kInk, 9.0f);
            }
            if (i % cols > 0) {
                g.setColour(kControlEdge.withAlpha(0.6f));
                g.drawVerticalLine(juce::roundToInt(b.getX()), b.getY() + 2.0f, b.getBottom() - 2.0f);
            }
        }
        g.setColour(highlighted ? kHighlight : kControlEdge);
        g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
        return;
    }
    if (e.kind == "ButtonIncDec") {
        const bool lr = e.type == "Left/Right";
        const auto a = lr ? r.withWidth(r.getWidth() * 0.5f) : r.withHeight(r.getHeight() * 0.5f);
        const auto b = lr ? r.withLeft(r.getCentreX()) : r.withTop(r.getCentreY());
        roundedButton(g, a.reduced(0.5f), false, highlighted, 2.5f);
        roundedButton(g, b.reduced(0.5f), false, highlighted, 2.5f);
        const float pi = juce::MathConstants<float>::pi;
        chevron(g, a.getCentre(), 2.5f, lr ? pi * 0.5f : pi, kInk);  // left / up
        chevron(g, b.getCentre(), 2.5f, lr ? -pi * 0.5f : 0.0f, kInk); // right / down
        return;
    }
    if (e.kind == "PartSelector") {
        roundedButton(g, r, false, highlighted);
        if (e.image.isValid() && e.imageCount > 0)
            drawGlyph(g, e.image, juce::jlimit(0, e.imageCount - 1, v.value_or(0)), e.imageWidth,
                      r.withTrimmedRight(8.0f).getCentre(), kInk);
        else
            centredText(g, r.withTrimmedRight(8.0f), valueText(c, e), kInk);
        chevron(g, {r.getRight() - 5.0f, r.getCentreY()}, 2.5f, 0.0f, kInk);
        return;
    }
    if (e.kind == "Led") {
        const auto led = r.withSizeKeepingCentre(6.0f, 6.0f);
        g.setColour(e.type == "Sequencer" ? juce::Colour(0xff6b5a1a) : juce::Colour(0xff2f5a2f));
        g.fillEllipse(led);
        return;
    }
    if (e.kind == "MiniVU") {
        g.setColour(juce::Colour(0xff2a2e35));
        g.fillRoundedRectangle(r, 2.0f);
        return;
    }
    if (SpecialControls::isSpecial(e)) {
        SpecialControls::paint(g, c, e, highlighted);
        return;
    }
    if (e.kind == "Graph") {
        paintModernGraph(g, c, e, r);
        return;
    }
}

} // namespace g2ui
