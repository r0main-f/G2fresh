#include "ModulePainter.h"

#include "g2/module_db.hpp"
#include "g2/param_text.hpp"

namespace g2ui {
namespace {

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

int morphGroup(const ModuleContext& c, int param)
{
    if (c.variation < 0 || static_cast<std::size_t>(c.variation) >= c.patch.morphs.size())
        return -1;
    for (const auto& a : c.patch.morphs[static_cast<std::size_t>(c.variation)].assigns)
        if (a.location == static_cast<std::uint8_t>(c.location) && a.module == c.module.index && a.param == param)
            return a.morph;
    return -1;
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
            return {e.x, e.y, 11, e.type == "SeqSlider" ? 37 : 51};
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
    if (e.kind == "TextField" && e.dependencies.isNotEmpty()) {
        juce::StringArray deps;
        deps.addTokens(e.dependencies, ",", "");
        std::vector<std::uint8_t> args;
        for (const auto& d : deps) {
            const bool isMode = d.startsWithIgnoreCase("S");
            const int i = (isMode ? d.substring(1) : d).getIntValue();
            const auto& src = isMode ? c.module.modes : *p;
            args.push_back(i >= 0 && static_cast<std::size_t>(i) < src.size() ? src[static_cast<std::size_t>(i)] : 0);
        }
        return g2::paramtext::format(e.textFunc, args);
    }
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

void ModulePainter::paint(juce::Graphics& g, const ModuleContext& c, const PanelElement* highlighted)
{
    const auto* def = c.module.def();
    const auto bounds = moduleBounds(c.module).withZeroOrigin();
    const auto face = def ? Skin::get().cbmp(def->faceResId) : juce::Image();
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
            drawLabel(g, r, e.options.joinIntoString(","), juce::Colours::black);
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
        for (int i = 0; i < n; ++i) {
            const juce::Rectangle<int> b(r.getX() + (i % cols) * w, r.getY() + (i / cols) * h, w, h);
            drawButtonBox(g, b, v.value_or(-1) == i, highlighted && v.value_or(-1) == i);
            if (e.image.isValid() && e.imageWidth > 0)
                drawImageFrame(g, e.image, i, e.imageWidth, b.withSizeKeepingCentre(e.imageWidth, e.image.getHeight()));
            else if (i < e.options.size())
                drawLabel(g, b, e.options[i], juce::Colours::black);
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
    // Text, Line, Symbol, Bitmap are part of the face bitmap; Graph is not
    // drawn yet.
}

} // namespace g2ui
