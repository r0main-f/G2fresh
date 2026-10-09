#include "SpecialControls.h"

#include "g2/special.hpp"

#include <array>

namespace g2ui {
namespace {

namespace sp = g2::special;

constexpr int kVocoder = 22, kZoom = 25, kOffset = 26, kDrumPreset = 46;

// The modern palette (as in ModulePainter).
const juce::Colour kInk(0xff2b2d33);
const juce::Colour kControl(0xfffafafb);
const juce::Colour kControlEdge(0xffa3a8b2);
const juce::Colour kAccent(0xff3478d4);
const juce::Colour kHighlight(0xffff8c1a);
const juce::Colour kKeyStripe(0xffd9dde3);

// Original layouts (constructors of the CPnl* classes, see special-controls.md).
constexpr int kVocoderPitch = 22;             // seven 20 px buttons, 22 px apart
constexpr int kZoomIcon = 16;                 // magnifier rows above the key ruler
constexpr int kSliderHeight = 75;             // CSeqSliderGUI view
const juce::Rectangle<int> kOffsetText{0, 0, 21, 13};
const juce::Rectangle<int> kOffsetButtons{0, 14, 21, 11};
const juce::Rectangle<int> kDrumText{0, 3, 50, 14};
const juce::Rectangle<int> kDrumButtons{53, 0, 11, 21};

const char* const kVocoderHelp[sp::kVocoderOps] = {
    "each band follows the analysis band two above (top two bands off)",
    "each band follows the analysis band above (top band off)",
    "default routing: each band follows its own analysis band",
    "each band follows the analysis band below (bottom band off)",
    "each band follows the analysis band two below (bottom two bands off)",
    "reversed: the lowest band follows the highest analysis band",
    "random routing (some bands may be off)",
};

const std::vector<std::uint8_t>* paramsOf(const ModuleContext& c)
{
    if (c.module.params.empty())
        return nullptr;
    const auto v = static_cast<std::size_t>(juce::jlimit(0, static_cast<int>(c.module.params.size()) - 1, c.variation));
    return &c.module.params[v];
}

sp::NoteSeqWindow windowOf(const ModuleContext& c)
{
    return sp::noteSeqWindow(sp::noteSeqView(c.module));
}

bool isBlackKey(int note)
{
    const int n = ((note % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

juce::String drumText(const ModuleContext& c)
{
    const auto* p = paramsOf(c);
    const auto preset = p ? sp::drumPresetIndex(*p) : std::nullopt;
    return preset ? juce::String(sp::drumPresetName(*preset)) : juce::String("none");
}

// ---- Classic look ----------------------------------------------------------------

void classicButton(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, bool highlighted)
{
    g.setColour(juce::Colour(0xffcecbce));
    g.fillRect(r);
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.drawRect(r);
    if (highlighted) {
        g.setColour(juce::Colours::red);
        g.drawRect(r, 1);
    }
    g.setColour(juce::Colours::black);
    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    g.drawFittedText(text, r, juce::Justification::centred, 1, 0.8f);
}

void classicTextView(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text)
{
    g.setColour(juce::Colour(0xfff4f4f0));
    g.fillRect(r);
    g.setColour(juce::Colour(0xff606060));
    g.drawRect(r);
    g.setColour(juce::Colours::black);
    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    g.drawFittedText(text, r.reduced(1, 0), juce::Justification::centred, 1, 0.8f);
}

// The original's arrow button sprites (CBMP 201 up/down, 202 left/right).
void classicArrows(juce::Graphics& g, juce::Rectangle<int> r, bool leftRight)
{
    const auto sprite = Skin::get().cbmp(leftRight ? 202 : 201);
    if (sprite.isValid())
        g.drawImage(sprite, r.toFloat(), juce::RectanglePlacement::stretchToFit);
    else
        classicButton(g, r, {}, false);
}

// ---- Modern look -----------------------------------------------------------------

void modernButton(juce::Graphics& g, juce::Rectangle<float> r, bool highlighted)
{
    g.setColour(kControl);
    g.fillRoundedRectangle(r, 2.5f);
    g.setColour(highlighted ? kHighlight : kControlEdge);
    g.drawRoundedRectangle(r.reduced(0.5f), 2.5f, highlighted ? 1.5f : 1.0f);
}

void modernText(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, float size = 9.0f)
{
    g.setColour(kInk);
    g.setFont(juce::Font(juce::FontOptions(size)));
    g.drawFittedText(text, r.toNearestInt(), juce::Justification::centred, 1, 0.75f);
}

void modernField(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& text, bool highlighted)
{
    g.setColour(juce::Colours::white);
    g.fillRoundedRectangle(r, 3.0f);
    g.setColour(highlighted ? kHighlight : juce::Colour(0xffc3c7ce));
    g.drawRoundedRectangle(r.reduced(0.5f), 3.0f, 1.0f);
    modernText(g, r.reduced(2.0f, 0.0f), text, 9.5f);
}

void chevron(juce::Graphics& g, juce::Point<float> c, float size, float angle)
{
    juce::Path p;
    p.startNewSubPath(-size, -size * 0.5f);
    p.lineTo(0.0f, size * 0.5f);
    p.lineTo(size, -size * 0.5f);
    g.setColour(kInk);
    g.strokePath(p, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                 juce::AffineTransform::rotation(angle).translated(c));
}

// Two arrow buttons side by side (left/right) or stacked (up/down).
void modernArrows(juce::Graphics& g, juce::Rectangle<float> r, bool leftRight, bool highlighted)
{
    const auto a = leftRight ? r.withWidth(r.getWidth() * 0.5f) : r.withHeight(r.getHeight() * 0.5f);
    const auto b = leftRight ? r.withLeft(r.getCentreX()) : r.withTop(r.getCentreY());
    modernButton(g, a.reduced(0.5f), highlighted);
    modernButton(g, b.reduced(0.5f), highlighted);
    const float pi = juce::MathConstants<float>::pi;
    chevron(g, a.getCentre(), 2.2f, leftRight ? pi * 0.5f : pi);
    chevron(g, b.getCentre(), 2.2f, leftRight ? -pi * 0.5f : 0.0f);
}

// The rows of the visible notes, top (high) to bottom, as the sliders draw them.
template <class F>
void forEachVisibleNote(const sp::NoteSeqWindow& w, F&& f)
{
    for (int v = std::max(w.low, 0); v <= w.high; ++v)
        f(v, w.markerTop(v));
}

} // namespace

bool SpecialControls::isSpecial(const PanelElement& e)
{
    return e.kind == "Graph"
        && (e.graphFunc == kVocoder || e.graphFunc == kZoom || e.graphFunc == kOffset || e.graphFunc == kDrumPreset);
}

juce::Rectangle<int> SpecialControls::vocoderButton(int index)
{
    return {index * kVocoderPitch, 0, kVocoderPitch - 2, 12};
}

SpecialControls::Hit SpecialControls::partAt(const PanelElement& e, juce::Point<int> local)
{
    switch (e.graphFunc) {
    case kVocoder:
        return {Part::VocoderButton, juce::jlimit(0, sp::kVocoderOps - 1, local.x / kVocoderPitch)};
    case kZoom:
        return {Part::Zoom};
    case kOffset: // CLeftRightButton::Click: right of the middle steps up
        return {local.x > 10 ? Part::OffsetRight : Part::OffsetLeft};
    case kDrumPreset:
        if (local.x >= kDrumButtons.getX() - 1) // CUpDownButton::Click: top 10 rows step up
            return {local.y < 10 ? Part::DrumUp : Part::DrumDown};
        return {Part::DrumName};
    default:
        return {};
    }
}

juce::String SpecialControls::describe(const ModuleContext& c, const PanelElement& e, Hit hit)
{
    const juce::String name(c.module.name);
    const auto view = sp::noteSeqView(c.module);
    switch (hit.part) {
    case Part::VocoderButton:
        return name + "  band preset \"" + sp::vocoderOpLabel(static_cast<sp::VocoderOp>(hit.index)) + "\": "
            + kVocoderHelp[hit.index];
    case Part::Zoom: {
        const auto w = sp::noteSeqWindow(view);
        const int octaves = (w.range - 1) / 12;
        return name + "  zoom: " + juce::String(octaves) + (octaves == 1 ? " octave" : " octaves")
            + " visible (click to change)";
    }
    case Part::OffsetLeft:
    case Part::OffsetRight:
        return name + "  octave range up to " + juce::String(sp::noteSeqOffsetText(view.offset))
            + " (click the arrows to scroll)";
    case Part::DrumName:
        return name + "  preset: " + drumText(c) + " (click for the list)";
    case Part::DrumUp:
    case Part::DrumDown:
        return name + "  preset: " + drumText(c) + " (arrows: next / previous)";
    case Part::None:
        break;
    }
    juce::ignoreUnused(e);
    return name;
}

void SpecialControls::paint(juce::Graphics& g, const ModuleContext& c, const PanelElement& e, bool highlighted)
{
    const juce::Rectangle<int> r(e.x, e.y, e.width, e.height);
    const bool modern = c.look == Look::Modern;
    switch (e.graphFunc) {
    case kVocoder:
        for (int i = 0; i < sp::kVocoderOps; ++i) {
            const auto b = vocoderButton(i).translated(r.getX(), r.getY());
            const juce::String label(sp::vocoderOpLabel(static_cast<sp::VocoderOp>(i)));
            if (modern) {
                modernButton(g, b.toFloat(), highlighted);
                modernText(g, b.toFloat(), label);
            } else {
                classicButton(g, b, label, highlighted);
            }
        }
        break;

    case kZoom: {
        const auto view = sp::noteSeqView(c.module);
        if (!modern) {
            // Bitmap 877: one 11 x 91 cell per zoom (magnifier, then the key ruler).
            const auto strip = Skin::get().jpeg(877);
            if (strip.isValid())
                g.drawImage(strip, r.getX(), r.getY(), 11, 91, sp::noteSeqZoomBitmapX(view.zoom), 0, 11, 91);
            if (highlighted) {
                g.setColour(juce::Colours::red);
                g.drawRect(r.withHeight(kZoomIcon));
            }
            break;
        }
        // Magnifier button; the ruler marks the black keys of the visible notes.
        const auto icon = r.withHeight(kZoomIcon - 2).toFloat();
        modernButton(g, icon, highlighted);
        const float size = 1.5f + static_cast<float>(std::min<int>(view.zoom, 2)); // bigger = more zoom
        const auto lens = juce::Point<float>(icon.getCentreX() - 0.5f, icon.getY() + 5.0f);
        g.setColour(kInk);
        g.drawEllipse(lens.x - size, lens.y - size, size * 2.0f, size * 2.0f, 1.2f);
        g.drawLine(lens.x + size * 0.6f, lens.y + size * 0.6f, lens.x + size + 2.0f, lens.y + size + 2.5f, 1.5f);
        const auto w = sp::noteSeqWindow(view);
        const float top = static_cast<float>(r.getY() + kZoomIcon);
        forEachVisibleNote(w, [&](int note, int row) {
            if (isBlackKey(note)) {
                g.setColour(kInk.withAlpha(0.8f));
                g.fillRect(static_cast<float>(r.getX()) + 1.0f, top + static_cast<float>(row), 7.0f,
                           static_cast<float>(std::max(1, w.pixels - (w.pixels > 2 ? 1 : 0))));
            } else if (note % 12 == 0) {
                g.setColour(kAccent);
                g.fillRect(static_cast<float>(r.getX()) + 1.0f, top + static_cast<float>(row + w.pixels) - 1.0f,
                           9.0f, 1.0f);
            }
        });
        break;
    }

    case kOffset: {
        const auto text = juce::String(sp::noteSeqOffsetText(sp::noteSeqView(c.module).offset));
        if (modern) {
            modernField(g, kOffsetText.translated(r.getX(), r.getY()).toFloat(), text, false);
            modernArrows(g, kOffsetButtons.translated(r.getX(), r.getY()).toFloat(), true, highlighted);
        } else {
            classicTextView(g, kOffsetText.translated(r.getX(), r.getY()), text);
            classicArrows(g, kOffsetButtons.translated(r.getX(), r.getY()), true);
        }
        break;
    }

    case kDrumPreset:
        if (modern) {
            modernField(g, kDrumText.translated(r.getX(), r.getY()).toFloat(), drumText(c), highlighted);
            modernArrows(g, kDrumButtons.translated(r.getX(), r.getY()).toFloat(), false, highlighted);
        } else {
            classicTextView(g, kDrumText.translated(r.getX(), r.getY()), drumText(c));
            classicArrows(g, kDrumButtons.translated(r.getX(), r.getY()), false);
        }
        break;

    default:
        break;
    }
}

void SpecialControls::paintSeqSlider(juce::Graphics& g, const ModuleContext& c, const PanelElement& e,
                                     bool highlighted)
{
    const juce::Rectangle<int> r(e.x, e.y, 11, kSliderHeight);
    const auto w = windowOf(c);
    const auto* p = paramsOf(c);
    const int value = p && e.codeRef >= 0 && static_cast<std::size_t>(e.codeRef) < p->size()
        ? (*p)[static_cast<std::size_t>(e.codeRef)] : 0;

    if (c.look == Look::Classic) {
        // CSeqSliderGUI::Draw: bitmap 874's column for the zoom (key stripes,
        // arrows below row 75) and a black marker on the value.
        const auto strip = Skin::get().jpeg(874);
        const int sx = w.bitmapX();
        if (strip.isValid()) {
            if (w.below(value)) {
                g.drawImage(strip, r.getX(), r.getY(), 11, 70, sx, 0, 11, 70);
                g.drawImage(strip, r.getX(), r.getY() + 70, 11, 5, sx, 82, 11, 5);
            } else if (w.above(value)) {
                g.drawImage(strip, r.getX(), r.getY(), 11, 5, sx, 75, 11, 5);
                g.drawImage(strip, r.getX(), r.getY() + 5, 11, 70, sx, 5, 11, 70);
            } else {
                g.drawImage(strip, r.getX(), r.getY(), 11, kSliderHeight, sx, 0, 11, kSliderHeight);
            }
        }
        if (!w.below(value) && !w.above(value)) {
            g.setColour(juce::Colours::black);
            g.fillRect(r.getX(), r.getY() + w.markerTop(value), 11, w.pixels);
        }
        if (highlighted) {
            g.setColour(juce::Colours::red);
            g.drawRect(r);
        }
        return;
    }

    // Modern: a light column with the black keys shaded, C lines, and the
    // step's note as an accent bar (an arrow when outside the window).
    const auto rf = r.toFloat();
    g.setColour(juce::Colours::white);
    g.fillRoundedRectangle(rf, 2.0f);
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(r.reduced(1));
        forEachVisibleNote(w, [&](int note, int row) {
            const float y = rf.getY() + static_cast<float>(row);
            if (isBlackKey(note)) {
                g.setColour(kKeyStripe);
                g.fillRect(rf.getX(), y, rf.getWidth(), static_cast<float>(w.pixels));
            } else if (note % 12 == 0) {
                g.setColour(kControlEdge.withAlpha(0.7f));
                g.fillRect(rf.getX(), y + static_cast<float>(w.pixels) - 0.5f, rf.getWidth(), 0.5f);
            }
        });
    }
    g.setColour(highlighted ? kHighlight : kControlEdge);
    g.drawRoundedRectangle(rf.reduced(0.5f), 2.0f, highlighted ? 1.5f : 1.0f);

    // Morph assignment: the bar takes the morph group's colour.
    juce::Colour bar = kAccent;
    if (c.variation >= 0 && static_cast<std::size_t>(c.variation) < c.patch.morphs.size())
        for (const auto& a : c.patch.morphs[static_cast<std::size_t>(c.variation)].assigns)
            if (a.location == static_cast<std::uint8_t>(c.location) && a.module == c.module.index && a.param == e.codeRef)
                bar = ModulePainter::morphColour(a.morph).darker(0.15f);
    g.setColour(bar);
    if (w.below(value) || w.above(value)) {
        const bool up = w.above(value);
        const float cx = rf.getCentreX(), y = up ? rf.getY() + 2.0f : rf.getBottom() - 2.0f;
        juce::Path arrow;
        arrow.addTriangle(cx - 3.5f, up ? y + 4.0f : y - 4.0f, cx + 3.5f, up ? y + 4.0f : y - 4.0f, cx, y);
        g.fillPath(arrow);
    } else {
        const float h = static_cast<float>(std::max(w.pixels, 2));
        g.fillRoundedRectangle(rf.getX() + 1.0f, rf.getY() + static_cast<float>(w.markerTop(value)), rf.getWidth() - 2.0f,
                               h, 1.0f);
    }
}

int SpecialControls::seqSliderValueAt(const ModuleContext& c, int localY)
{
    return windowOf(c).valueAt(localY);
}

int SpecialControls::seqSliderPixelsPerValue(const ModuleContext& c)
{
    return std::max(1, windowOf(c).pixels);
}

} // namespace g2ui
