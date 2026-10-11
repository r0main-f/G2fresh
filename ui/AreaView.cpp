#include "AreaView.h"

#include "ModuleBrowser.h"
#include "SpecialControls.h"

#include "g2/edit.hpp"
#include "g2/param_text.hpp"
#include "g2/replace.hpp"
#include "g2/special.hpp"

namespace g2ui {
namespace {

const juce::String kModuleDragPrefix = "g2module:";

bool isEditable(const PanelElement& e)
{
    return ModulePainter::isControl(e) || SpecialControls::isSpecial(e);
}

// The editor's generator for the special controls' "Rnd" (seeded once, as the
// original seeds Rnd_GetC at start-up).
g2::special::EditorRandom& editorRandom()
{
    static g2::special::EditorRandom rng(static_cast<std::uint32_t>(juce::Time::currentTimeMillis()));
    return rng;
}

const char* cableColourName(g2::CableColor c)
{
    static const char* const names[7] = {"Red", "Blue", "Yellow", "Orange", "Green", "Purple", "White"};
    const auto i = static_cast<std::size_t>(c);
    return i < 7 ? names[i] : "";
}

// The same cable, whatever its colour.
bool sameEnds(const g2::Cable& a, const g2::Cable& b)
{
    return a.fromModule == b.fromModule && a.fromConn == b.fromConn && a.fromIsOutput == b.fromIsOutput
        && a.toModule == b.toModule && a.toConn == b.toConn;
}

int pixelsPerStep(int max, bool fine)
{
    const int base = std::max(1, 200 / (max + 1));
    return fine ? base * 4 : base;
}

} // namespace

class AreaView::ModulesLayer : public juce::Component {
public:
    explicit ModulesLayer(AreaView& owner) : owner_(owner)
    {
        setInterceptsMouseClicks(false, false);
        setBufferedToImage(true);
    }
    void paint(juce::Graphics& g) override { owner_.paintModules(g); }

private:
    AreaView& owner_;
};

class AreaView::Overlay : public juce::Component {
public:
    explicit Overlay(AreaView& owner) : owner_(owner) { setInterceptsMouseClicks(false, false); }
    void paint(juce::Graphics& g) override { owner_.paintOverlay(g); }

private:
    AreaView& owner_;
};

AreaView::AreaView(PatchDocument& doc, g2::Location location)
    : doc_(doc), location_(location),
      modules_(std::make_unique<ModulesLayer>(*this)), overlay_(std::make_unique<Overlay>(*this))
{
    addAndMakeVisible(*modules_);
    addAndMakeVisible(*overlay_);
    setWantsKeyboardFocus(true);
    doc_.addChangeListener(this);
    updateSize();
    settingsChanged();
}

AreaView::~AreaView()
{
    stopTimer();
    doc_.removeChangeListener(this);
}

void AreaView::resized()
{
    modules_->setBounds(getLocalBounds());
    overlay_->setBounds(getLocalBounds());
}

void AreaView::settingsChanged()
{
    updateTimer();
    repaintModules();
}

void AreaView::updateTimer()
{
    // Only while something moves: the flow animation (with cables to animate),
    // the pulsing rings of a highlighted cable, live LEDs.
    const bool animating = cableAnimation() && !doc_.patch().area(location_).cables.empty();
    if (animating || highlighted_ || (liveLeds_ != nullptr && liveLeds_->ledsLive())) {
        if (!isTimerRunning())
            startTimerHz(30);
    } else {
        stopTimer();
    }
}

void AreaView::repaintModules()
{
    modules_->repaint();
    overlay_->repaint();
}

void AreaView::setLiveLeds(const LiveLeds* leds)
{
    liveLeds_ = leds;
    liveLedsChanged();
}

void AreaView::liveLedsChanged()
{
    updateTimer();
    overlay_->repaint();
}

void AreaView::timerCallback()
{
    // Nobody watches a hidden view or an app in the background.
    if (!isShowing() || !juce::Process::isForegroundProcess())
        return;
    if (liveLeds_ != nullptr && liveLeds_->ledGeneration() != ledGeneration_) {
        ledGeneration_ = liveLeds_->ledGeneration();
        overlay_->repaint();
    }
    if (doc_.patch().area(location_).cables.empty() || !(cableAnimation() || highlighted_))
        return;
    // Pulses move at a steady 48 px/s, whatever the frame rate.
    flowPhase_ = static_cast<float>(std::fmod(juce::Time::getMillisecondCounterHiRes() * 0.048, 36.0 * 1000.0));
    // Only where the cables are (a highlighted cable also has labels around it).
    if (highlighted_ || cableBounds_.isEmpty())
        overlay_->repaint();
    else
        overlay_->repaint(cableBounds_);
}

void AreaView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    std::erase_if(selection_, [&](std::uint8_t m) { return !doc_.patch().area(location_).find(m); });
    // Follow the highlighted cable (its colour may have changed); drop it once gone.
    if (highlighted_) {
        const auto& cables = doc_.patch().area(location_).cables;
        const auto it = std::find_if(cables.begin(), cables.end(), [&](const g2::Cable& c) { return sameEnds(c, *highlighted_); });
        highlighted_ = it != cables.end() ? std::optional<g2::Cable>(*it) : std::nullopt;
    }
    updateTimer(); // the first or last cable starts or stops the animation
    updateSize();
    repaintModules();
}

void AreaView::updateSize()
{
    int cols = 6, rows = 40;
    for (const auto& m : doc_.patch().area(location_).modules) {
        const auto b = ModulePainter::moduleBounds(m);
        cols = std::max(cols, b.getRight() / kModuleWidth + 2);
        rows = std::max(rows, b.getBottom() / kRowHeight + 20);
    }
    setSize(std::max(cols * kModuleWidth, minWidth_), std::max(rows * kRowHeight, minHeight_));
}

std::optional<juce::Point<int>> AreaView::focusPoint() const
{
    if (const auto* m = touched_ ? doc_.patch().area(location_).find(touched_) : nullptr)
        return ModulePainter::moduleBounds(*m).getCentre();
    return std::nullopt;
}

void AreaView::setMinimumSize(int width, int height)
{
    if (width == minWidth_ && height == minHeight_)
        return;
    minWidth_ = width;
    minHeight_ = height;
    updateSize();
}

ModuleContext AreaView::context(const g2::Module& m) const
{
    return {doc_.patch(), location_, m, doc_.focusedVariation(), ModulePainter::panelFor(m), currentLook()};
}

juce::Point<int> AreaView::gridCell(juce::Point<int> p) const
{
    return {juce::jlimit(0, 127, p.x / kModuleWidth),
            juce::jlimit(0, 127, juce::roundToInt(static_cast<float>(p.y) / kRowHeight))};
}

AreaView::Hit AreaView::hitAt(juce::Point<int> p) const
{
    const auto& modules = doc_.patch().area(location_).modules;
    for (auto it = modules.rbegin(); it != modules.rend(); ++it) {
        const auto b = ModulePainter::moduleBounds(*it);
        if (!b.contains(p))
            continue;
        Hit h{it->index, nullptr, p - b.getPosition()};
        if (const auto* panel = ModulePainter::panelFor(*it))
            for (const auto& e : panel->elements)
                if ((ModulePainter::isJack(e) || isEditable(e) || e.kind == "TextField")
                    && ModulePainter::elementBounds(e).expanded(1).contains(h.local)) {
                    h.element = &e;
                    break;
                }
        return h;
    }
    return {};
}

std::optional<juce::Point<float>> AreaView::jackCentre(const Jack& j) const
{
    const auto* m = doc_.patch().area(location_).find(j.module);
    if (!m)
        return std::nullopt;
    const auto origin = ModulePainter::moduleBounds(*m).getPosition().toFloat();
    if (const auto* panel = ModulePainter::panelFor(*m))
        for (const auto& e : panel->elements)
            if (e.kind == (j.isOutput ? "Output" : "Input") && e.codeRef == j.conn)
                return origin + ModulePainter::elementBounds(e).toFloat().getCentre();
    return origin + juce::Point<float>(j.isOutput ? kModuleWidth - 8.0f : 8.0f, 8.0f);
}

std::optional<AreaView::Jack> AreaView::jackAt(juce::Point<int> p) const
{
    const auto h = hitAt(p);
    if (!h.element || !ModulePainter::isJack(*h.element) || h.element->codeRef < 0)
        return std::nullopt;
    return Jack{h.module, static_cast<std::uint8_t>(h.element->codeRef), h.element->kind == "Output"};
}

juce::String AreaView::jackName(const Jack& j) const
{
    const auto* m = doc_.patch().area(location_).find(j.module);
    if (!m)
        return {};
    const auto* def = m->def();
    juce::String conn(j.conn);
    if (def) {
        const auto& list = j.isOutput ? def->outputs : def->inputs;
        if (j.conn < list.size())
            conn = list[j.conn].name;
    }
    return juce::String(m->name) + " " + conn;
}

juce::String AreaView::describeCable(const g2::Cable& c) const
{
    const auto from = jackName({c.fromModule, c.fromConn, c.fromIsOutput});
    const auto to = jackName({c.toModule, c.toConn, false});
    return juce::String(cableColourName(c.color)) + " cable: " + from + juce::String(juce::CharPointer_UTF8(c.fromIsOutput ? "  \xe2\x86\x92  " : "  \xe2\x86\x94  "))
        + to;
}

juce::String AreaView::describe(const Hit& h) const
{
    const auto* m = doc_.patch().area(location_).find(h.module);
    if (!m)
        return {};
    const auto* def = m->def();
    const juce::String name(m->name);
    if (!h.element)
        return name + (def && def->longName ? "  (" + juce::String(def->longName) + ")" : juce::String());
    const auto& e = *h.element;
    const auto c = context(*m);
    if (SpecialControls::isSpecial(e))
        return SpecialControls::describe(c, e, SpecialControls::partAt(e, h.local - ModulePainter::elementBounds(e).getPosition()));
    if (ModulePainter::isJack(e)) {
        const auto& list = e.kind == "Output" ? def->outputs : def->inputs;
        const juce::String conn = def && juce::isPositiveAndBelow(e.codeRef, static_cast<int>(list.size()))
            ? juce::String(list[static_cast<std::size_t>(e.codeRef)].name) : juce::String(e.codeRef);
        return name + "  " + (e.kind == "Output" ? "output " : "input ") + conn;
    }
    juce::String what;
    if (e.kind == "PartSelector") {
        if (def && juce::isPositiveAndBelow(e.codeRef, static_cast<int>(def->modes.size())))
            what = def->modes[static_cast<std::size_t>(e.codeRef)].name;
    } else {
        const int ref = e.kind == "TextField" ? e.masterRef : e.codeRef;
        if (def && juce::isPositiveAndBelow(ref, static_cast<int>(def->params.size())))
            what = def->params[static_cast<std::size_t>(ref)].name;
    }
    return name + "  " + what + ": " + ModulePainter::valueText(c, e);
}

void AreaView::paint(juce::Graphics& g)
{
    // Slightly different tints tell the voice area and the FX area apart.
    const bool fx = location_ == g2::Location::Fx;
    const auto base = fx ? juce::Colour(0xff38333f) : juce::Colour(0xff313840);
    g.fillAll(base);
    g.setColour(base.brighter(0.08f));
    for (int x = kModuleWidth; x < getWidth(); x += kModuleWidth)
        g.drawVerticalLine(x, 0.0f, static_cast<float>(getHeight()));
}

void AreaView::paintModules(juce::Graphics& g)
{
    for (const auto& m : doc_.patch().area(location_).modules) {
        const auto b = ModulePainter::moduleBounds(m);
        if (!g.clipRegionIntersects(b))
            continue;
        const auto c = context(m);
        {
            juce::Graphics::ScopedSaveState state(g);
            g.setOrigin(b.getPosition());
            g.reduceClipRegion(b.withZeroOrigin());
            ModulePainter::paint(g, c, hover_.module == m.index ? hover_.element : nullptr);
        }
        if (isSelected(m.index)) {
            g.setColour(juce::Colours::white);
            if (c.look == Look::Modern)
                g.drawRoundedRectangle(b.toFloat().reduced(0.5f), 6.0f, 2.0f);
            else
                g.drawRect(b, 2);
        }
    }
}

void AreaView::paintOverlay(juce::Graphics& g)
{
    if (drag_ == Drag::Module) {
        if (const auto* grabbed = doc_.patch().area(location_).find(dragHit_.module)) {
            // Ghosts of the whole selection at the drop position.
            const auto cell = gridCell(dragPos_ - dragOffset_ + juce::Point<int>(kModuleWidth / 2, 0));
            const int dCol = cell.x - grabbed->col, dRow = cell.y - grabbed->row;
            g.setColour(juce::Colours::white.withAlpha(0.3f));
            for (const auto& m : doc_.patch().area(location_).modules)
                if (isSelected(m.index))
                    g.fillRoundedRectangle(ModulePainter::moduleBounds(m).translated(dCol * kModuleWidth, dRow * kRowHeight).toFloat(), 5.0f);
        }
    }
    if (drag_ == Drag::Band) {
        const auto band = juce::Rectangle<int>(dragHit_.local, dragPos_).toFloat();
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.fillRect(band);
        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.drawRect(band, 1.0f);
    }
    if (dropCell_) {
        g.setColour(juce::Colours::white.withAlpha(0.3f));
        g.fillRoundedRectangle(juce::Rectangle<int>(dropCell_->x * kModuleWidth, dropCell_->y * kRowHeight, kModuleWidth,
                                                    2 * kRowHeight).toFloat(), 5.0f);
    }
    paintLiveLeds(g);
    paintCables(g);
}

void AreaView::paintLiveLeds(juce::Graphics& g)
{
    if (liveLeds_ == nullptr)
        return;
    for (const auto& m : doc_.patch().area(location_).modules) {
        const auto* panel = ModulePainter::panelFor(m);
        if (!panel)
            continue;
        const auto origin = ModulePainter::moduleBounds(m).getPosition();
        const auto groups = ledGroups(*panel);
        std::vector<int> nth(groups.size(), 0); // position of each LED in its group
        for (const auto& e : panel->elements) {
            if (e.kind != "Led" && e.kind != "MiniVU")
                continue;
            if (!juce::isPositiveAndBelow(e.groupId, static_cast<int>(groups.size())))
                continue;
            const int index = nth[static_cast<std::size_t>(e.groupId)]++;
            const auto value = liveLeds_->ledValue(location_, m.index, e.groupId);
            if (!value)
                continue;
            const auto r = ModulePainter::elementBounds(e).translated(origin.x, origin.y).toFloat();
            if (e.kind == "MiniVU") {
                // Level 0..0x7E from the bottom (or left), green, then yellow, red when clipping.
                const bool vertical = e.orientation != "Horizontal";
                const float level = juce::jlimit(0.0f, 1.0f, static_cast<float>(*value) / 126.0f);
                const auto bar = vertical ? r.reduced(1.5f).withTrimmedTop(r.reduced(1.5f).getHeight() * (1.0f - level))
                                          : r.reduced(1.5f).withWidth(r.reduced(1.5f).getWidth() * level);
                g.setColour(juce::Colour(0xff1b1d22));
                g.fillRoundedRectangle(r, 2.0f);
                g.setColour(*value > 0x7E ? juce::Colour(0xffff4040)
                            : level > 0.8f ? juce::Colour(0xffffd23c) : juce::Colour(0xff4fe04f));
                g.fillRect(bar);
                continue;
            }
            bool lit = false;
            float brightness = 1.0f;
            if (groups[static_cast<std::size_t>(e.groupId)]) { // a strip
                if (*value == 0xFFF)
                    lit = true;
                else if ((*value & 0x3000) == 0x3000)
                    lit = ((*value >> index) & 1) != 0;
                else
                    lit = *value == index;
            } else { // a single LED: 0 off, 1..3 on [I: what 1 and 2 mean is unverified]
                lit = *value != 0;
                brightness = 0.55f + 0.15f * static_cast<float>(*value);
            }
            const auto led = r.withSizeKeepingCentre(6.0f, 6.0f);
            const auto colour = e.type == "Sequencer" ? juce::Colour(0xffffb020) : juce::Colour(0xff40ff40);
            if (lit) {
                g.setColour(colour.withAlpha(0.35f * brightness));
                g.fillEllipse(led.expanded(3.0f));
                g.setColour(colour.withMultipliedBrightness(brightness));
                g.fillEllipse(led);
            } else {
                g.setColour(colour.darker(2.5f));
                g.fillEllipse(led);
            }
        }
    }
}

juce::Path AreaView::cablePath(juce::Point<float> a, juce::Point<float> b, std::optional<g2::CableBend> bend) const
{
    const float distance = a.getDistanceFrom(b);
    juce::Path path;
    path.startNewSubPath(a);
    if (bend) {
        // Two curves meeting at the bend point, like a cord over a hook: each
        // leaves its jack towards the point with a little sag, and they cross
        // the point along the jack-to-jack direction with a short tangent, so
        // a point pulled far away gives a U, not a loop.
        const auto m = (a + b) * 0.5f + juce::Point<float>(bend->dx, bend->dy);
        const float ha = a.getDistanceFrom(m), hb = b.getDistanceFrom(m);
        const auto t = distance > 0.5f ? (b - a) / distance : juce::Point<float>(1.0f, 0.0f);
        const float tangent = std::min(0.25f * distance, 0.35f * std::min(ha, hb));
        path.cubicTo(a + (m - a) * 0.3f + juce::Point<float>(0.0f, std::min(25.0f, 0.2f * ha)), m - t * tangent, m);
        path.cubicTo(m + t * tangent, b + (m - b) * 0.3f + juce::Point<float>(0.0f, std::min(25.0f, 0.2f * hb)), b);
        return path;
    }
    if (currentLook() == Look::Classic) {
        path.quadraticTo((a + b) * 0.5f + juce::Point<float>(0.0f, 10.0f + 0.25f * distance), b);
    } else {
        const float sag = std::min(90.0f, 12.0f + 0.22f * distance);
        path.cubicTo(a + juce::Point<float>((b.x - a.x) * 0.15f, sag), b + juce::Point<float>((a.x - b.x) * 0.15f, sag), b);
    }
    return path;
}

std::optional<g2::Cable> AreaView::cableAt(juce::Point<int> p) const
{
    const auto& header = doc_.patch().header;
    for (auto it = doc_.patch().area(location_).cables.rbegin(); it != doc_.patch().area(location_).cables.rend(); ++it) {
        if (!header.cablesVisible[static_cast<std::size_t>(it->color)])
            continue;
        const auto a = jackCentre({it->fromModule, it->fromConn, it->fromIsOutput});
        const auto b = jackCentre({it->toModule, it->toConn, false});
        if (!a || !b)
            continue;
        juce::Point<float> nearest;
        cablePath(*a, *b, it->bend).getNearestPoint(p.toFloat(), nearest);
        if (nearest.getDistanceFrom(p.toFloat()) < 5.0f)
            return *it;
    }
    return std::nullopt;
}

std::optional<std::pair<juce::Point<float>, juce::Point<float>>> AreaView::cableEnds(const g2::Cable& c) const
{
    const auto a = jackCentre({c.fromModule, c.fromConn, c.fromIsOutput});
    const auto b = jackCentre({c.toModule, c.toConn, false});
    if (!a || !b)
        return std::nullopt;
    return std::make_pair(*a, *b);
}

std::optional<juce::Point<float>> AreaView::bendPoint(const g2::Cable& c) const
{
    const auto ends = cableEnds(c);
    if (!ends || !c.bend)
        return std::nullopt;
    return (ends->first + ends->second) * 0.5f + juce::Point<float>(c.bend->dx, c.bend->dy);
}

std::optional<g2::Cable> AreaView::bendPointAt(juce::Point<int> p) const
{
    const auto& visible = doc_.patch().header.cablesVisible;
    const auto& cables = doc_.patch().area(location_).cables;
    // The highlighted cable's point first, then the topmost.
    if (const auto h = highlightedCable())
        if (const auto b = bendPoint(*h); b && b->getDistanceFrom(p.toFloat()) <= 8.0f)
            return h;
    for (auto it = cables.rbegin(); it != cables.rend(); ++it)
        if (visible[static_cast<std::size_t>(it->color)])
            if (const auto b = bendPoint(*it); b && b->getDistanceFrom(p.toFloat()) <= 6.0f)
                return *it;
    return std::nullopt;
}

void AreaView::toggleBendPoint(const g2::Cable& cable, juce::Point<float> where)
{
    const auto ends = cableEnds(cable);
    if (!ends)
        return;
    const auto loc = location_;
    std::optional<g2::CableBend> bend;
    if (!cable.bend) {
        // The curve will pass through the point double-clicked.
        const auto offset = where - (ends->first + ends->second) * 0.5f;
        bend = g2::CableBend{static_cast<std::int16_t>(juce::roundToInt(offset.x)),
                             static_cast<std::int16_t>(juce::roundToInt(offset.y))};
    }
    doc_.perform(bend ? "Add bend point" : "Remove bend point",
                 [&](g2::Patch& p) { g2::edit::setCableBend(p, loc, cable, bend); });
    highlightCable(cable);
}

void AreaView::paintCable(juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, juce::Colour c, bool flowing,
                          std::optional<g2::CableBend> bend)
{
    juce::Path path = cablePath(a, b, bend);
    if (currentLook() == Look::Classic) {
        // The original editor's style: a simple sagging curve with an outline.
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.strokePath(path, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(c);
        g.strokePath(path, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    } else {
        // Modern: a hanging cable (cubic curve with gravity), soft shadow, a
        // glossy tube and plugs.
        g.setColour(juce::Colours::black.withAlpha(0.28f));
        g.strokePath(path, juce::PathStrokeType(4.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                     juce::AffineTransform::translation(0.8f, 1.8f));
        g.setColour(c.darker(0.25f));
        g.strokePath(path, juce::PathStrokeType(3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(c.brighter(0.45f).withAlpha(0.55f));
        g.strokePath(path, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                     juce::AffineTransform::translation(-0.4f, -0.6f));
        for (auto p : {a, b}) {
            g.setColour(c.darker(0.5f));
            g.fillEllipse(p.x - 3.0f, p.y - 3.0f, 6.0f, 6.0f);
        }
    }
    // A bent cable shows its bend point, which can be dragged.
    if (bend) {
        const auto m = (a + b) * 0.5f + juce::Point<float>(bend->dx, bend->dy);
        g.setColour(juce::Colours::white);
        g.fillEllipse(m.x - 3.5f, m.y - 3.5f, 7.0f, 7.0f);
        g.setColour(c.darker(0.4f));
        g.drawEllipse(m.x - 3.5f, m.y - 3.5f, 7.0f, 7.0f, 1.5f);
    }
    if (!flowing)
        return;
    // Pulses flowing from the source to the destination; dark on light cables
    // (white, yellow) so they stay visible.
    const float length = path.getLength();
    constexpr float spacing = 36.0f;
    g.setColour(c.getPerceivedBrightness() > 0.7f ? juce::Colours::black.withAlpha(0.6f)
                                                  : juce::Colours::white.withAlpha(0.85f));
    for (float d = std::fmod(flowPhase_, spacing); d < length; d += spacing) {
        const auto p = path.getPointAlongPath(d);
        g.fillEllipse(p.x - 2.0f, p.y - 2.0f, 4.0f, 4.0f);
    }
}

void AreaView::paintCables(juce::Graphics& g)
{
    const bool flowing = cableAnimation();
    const auto& visible = doc_.patch().header.cablesVisible;
    juce::Rectangle<float> covered;
    // With a highlighted cable, the others are dimmed and it is drawn last.
    if (highlighted_)
        g.beginTransparencyLayer(0.25f);
    for (const auto& cable : doc_.patch().area(location_).cables) {
        if (!visible[static_cast<std::size_t>(cable.color)] && !(highlighted_ && sameEnds(cable, *highlighted_)))
            continue; // hidden colour (View > Cables)
        if (highlighted_ && sameEnds(cable, *highlighted_))
            continue;
        // Signals flow from the "from" end (an output, or the first input of
        // a link) to the input at the "to" end.
        const auto a = jackCentre({cable.fromModule, cable.fromConn, cable.fromIsOutput});
        const auto b = jackCentre({cable.toModule, cable.toConn, false});
        if (a && b) {
            paintCable(g, *a, *b, ModulePainter::cableColour(cable.color), flowing, cable.bend);
            // The animation repaints only this area (shadow and plugs included).
            const auto r = cablePath(*a, *b, cable.bend).getBounds().expanded(8.0f);
            covered = covered.isEmpty() ? r : covered.getUnion(r);
        }
    }
    cableBounds_ = covered.getSmallestIntegerContainer();
    if (highlighted_) {
        g.endTransparencyLayer();
        if (const auto current = highlightedCable())
            paintHighlight(g, *current);
    }
    if (drag_ == Drag::Cable && cableFrom_)
        if (const auto a = jackCentre(*cableFrom_))
            paintCable(g, *a, dragPos_.toFloat(), juce::Colours::white, false);
}

void AreaView::paintHighlight(juce::Graphics& g, const g2::Cable& cable)
{
    const auto a = jackCentre({cable.fromModule, cable.fromConn, cable.fromIsOutput});
    const auto b = jackCentre({cable.toModule, cable.toConn, false});
    if (!a || !b)
        return;
    const auto colour = ModulePainter::cableColour(cable.color);
    // A soft glow under the cable, then the cable itself.
    const auto path = cablePath(*a, *b, cable.bend);
    g.setColour(colour.withAlpha(0.35f));
    g.strokePath(path, juce::PathStrokeType(11.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour(juce::Colours::white.withAlpha(0.5f));
    g.strokePath(path, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    paintCable(g, *a, *b, colour, cableAnimation(), cable.bend);

    // The bend point, larger: drag it to move it, double-click to remove it.
    if (const auto h = bendPoint(cable)) {
        juce::Path diamond;
        diamond.addPolygon(*h, 4, 6.5f, 0.0f);
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillPath(diamond, juce::AffineTransform::translation(0.6f, 1.2f));
        g.setColour(juce::Colours::white);
        g.fillPath(diamond);
        g.setColour(colour.darker(0.3f));
        g.strokePath(diamond, juce::PathStrokeType(1.5f));
    }

    // Both ends: a pulsing ring and a label naming the module and connector.
    const auto pulse = static_cast<float>(0.5 + 0.5 * std::sin(juce::Time::getMillisecondCounterHiRes() * 0.006));
    const juce::Font font(juce::FontOptions(12.0f, juce::Font::bold));
    const std::pair<juce::Point<float>, Jack> ends[2] = {
        {*a, Jack{cable.fromModule, cable.fromConn, cable.fromIsOutput}},
        {*b, Jack{cable.toModule, cable.toConn, false}},
    };
    for (const auto& [p, jack] : ends) {
        const float r = 8.0f + 3.0f * pulse;
        g.setColour(juce::Colours::white.withAlpha(0.9f));
        g.drawEllipse(p.x - r, p.y - r, 2 * r, 2 * r, 2.0f);
        g.setColour(colour.withAlpha(0.6f + 0.4f * pulse));
        g.drawEllipse(p.x - r - 2.5f, p.y - r - 2.5f, 2 * r + 5.0f, 2 * r + 5.0f, 1.5f);

        const auto text = jackName(jack);
        const float w = juce::GlyphArrangement::getStringWidth(font, text) + 10.0f, h = 18.0f;
        // Above the jack, kept inside the area.
        auto tag = juce::Rectangle<float>(p.x - w / 2, p.y - r - 6.0f - h, w, h);
        if (tag.getY() < 0)
            tag.setY(p.y + r + 6.0f);
        tag.setX(juce::jlimit(2.0f, std::max(2.0f, static_cast<float>(getWidth()) - w - 2.0f), tag.getX()));
        g.setColour(juce::Colour(0xee1c1f24));
        g.fillRoundedRectangle(tag, 5.0f);
        g.setColour(colour);
        g.drawRoundedRectangle(tag.reduced(0.5f), 5.0f, 1.0f);
        g.setColour(juce::Colours::white);
        g.setFont(font);
        g.drawText(text, tag, juce::Justification::centred, false);
    }
}

std::optional<g2::Cable> AreaView::highlightedCable() const
{
    if (!highlighted_)
        return std::nullopt;
    for (const auto& c : doc_.patch().area(location_).cables)
        if (sameEnds(c, *highlighted_))
            return c;
    return std::nullopt;
}

void AreaView::highlightCable(std::optional<g2::Cable> cable)
{
    if (!cable && !highlighted_)
        return;
    highlighted_ = cable;
    updateTimer();
    overlay_->repaint();
    if (cable)
        status(describeCable(*cable) + "  (Delete removes it)");
}

void AreaView::mouseMove(const juce::MouseEvent& e)
{
    const auto h = hitAt(e.getPosition());
    if (h.module != hover_.module || h.element != hover_.element) {
        hover_ = h;
        modules_->repaint();
    }
    if (const auto bent = bendPointAt(e.getPosition())) {
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        status(describeCable(*bent) + "  (drag the bend point to move it, double-click to remove it)");
        return;
    }
    // Cables are drawn over the modules: over a cord (but not over a jack or
    // a control) the cord is what a click picks.
    const bool onControl = h.element && (ModulePainter::isJack(*h.element) || isEditable(*h.element));
    if (const auto cable = onControl ? std::nullopt : cableAt(e.getPosition())) {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        status(describeCable(*cable) + (cable->bend ? "  (click to select, double-click to remove the bend point)"
                                                    : "  (click to select, double-click to add a bend point)"));
        return;
    }
    setMouseCursor(juce::MouseCursor::NormalCursor);
    status(describe(h));
}

void AreaView::mouseExit(const juce::MouseEvent&)
{
    hover_ = {};
    modules_->repaint();
}

void AreaView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (onActivated)
        onActivated(this);
    dragPos_ = e.getPosition();
    dragHit_ = hitAt(e.getPosition());
    drag_ = Drag::None;
    // Bend points come first: drag one to move it.
    if (const auto bent = bendPointAt(e.getPosition())) {
        highlightCable(bent);
        clearSelection();
        dragHit_ = {};
        if (e.mods.isPopupMenu())
            showCableMenu(*bent);
        else
            drag_ = Drag::Bend;
        return;
    }
    // A click on a cord (cables are drawn over modules, but not over jacks
    // and controls) highlights that cable; a right-click opens its menu.
    const bool onControl = dragHit_.element && (ModulePainter::isJack(*dragHit_.element) || isEditable(*dragHit_.element));
    if (const auto cable = onControl ? std::nullopt : cableAt(e.getPosition())) {
        highlightCable(cable);
        clearSelection(); // the cable is what Delete acts on now
        dragHit_ = {};
        if (e.mods.isPopupMenu())
            showCableMenu(*cable);
        return;
    }
    highlightCable(std::nullopt);
    if (!dragHit_.module) {
        if (e.mods.isPopupMenu())
            return;
        // Empty background: start a rubber-band selection.
        if (!e.mods.isShiftDown())
            clearSelection();
        dragHit_.local = e.getPosition();
        drag_ = Drag::Band;
        return;
    }
    if (e.mods.isPopupMenu()) {
        if (dragHit_.element && ModulePainter::isJack(*dragHit_.element)) {
            if (const auto jack = jackAt(e.getPosition()))
                showJackMenu(*jack);
        } else if (dragHit_.element && isEditable(*dragHit_.element)) {
            showControlMenu(dragHit_);
        } else {
            if (!isSelected(dragHit_.module))
                select(dragHit_.module, false);
            showModuleMenu(dragHit_.module);
        }
        return;
    }
    if (dragHit_.element && ModulePainter::isJack(*dragHit_.element)) {
        cableFrom_ = jackAt(e.getPosition());
        // Alt-drag an input: pick up the cable ending there and re-patch it.
        if (cableFrom_ && !cableFrom_->isOutput && e.mods.isAltDown()) {
            const auto loc = location_;
            const auto here = *cableFrom_;
            for (const auto& c : doc_.patch().area(location_).cables)
                if (c.toModule == here.module && c.toConn == here.conn) {
                    cableFrom_ = Jack{c.fromModule, c.fromConn, c.fromIsOutput};
                    const auto cable = c;
                    doc_.perform("Disconnect", [&](g2::Patch& p) { g2::edit::disconnect(p, loc, cable); });
                    break;
                }
        }
        drag_ = Drag::Cable;
        return;
    }
    if (dragHit_.element && isEditable(*dragHit_.element)) {
        const auto* m = doc_.patch().area(location_).find(dragHit_.module);
        dragStartValue_ = m ? ModulePainter::value(context(*m), *dragHit_.element).value_or(0) : 0;
        drag_ = dragHit_.element->kind == "Knob" ? Drag::Value : Drag::None;
        // A push button (sequencer Clr/Rnd, momentary switches): 1 while
        // held, 0 on release, not an undo step (CPnlPushButton, CPanel::CtrlRelease).
        if (m && isPushButton(dragHit_)) {
            pressMomentary(dragHit_, 1);
            pushed_ = true;
            return;
        }
        // A note sequencer step jumps to the note clicked (CPnlSeqSlider::OnClick),
        // then drags from there.
        if (m && SpecialControls::isSeqSlider(*dragHit_.element) && !e.mods.isAltDown()) {
            const int y = dragHit_.local.y - ModulePainter::elementBounds(*dragHit_.element).getY();
            dragStartValue_ = SpecialControls::seqSliderValueAt(context(*m), y);
            setValue(dragHit_, dragStartValue_, true);
            status(describe(dragHit_));
        }
        // Alt-drag a knob: set its morph range in the current variation; so
        // does a double-click and drag, as the original's "Morph w/double
        // click" option (on by default; CPnlControl::ClickDragOnSecondClickHandler).
        const bool morphClick = e.getNumberOfClicks() >= 2 && knobDoubleClick() == KnobDoubleClick::Morph;
        if (drag_ == Drag::Value && (e.mods.isAltDown() || morphClick) && m) {
            const auto morph = g2::edit::morphOf(doc_.patch(), static_cast<std::uint8_t>(doc_.focusedVariation()), location_,
                                                 dragHit_.module, static_cast<std::uint8_t>(dragHit_.element->codeRef));
            dragStartRange_ = morph ? morph->range : 0;
            if (morph)
                morphGroupForRange_ = morph->group;
            drag_ = Drag::Range;
        }
        return;
    }
    if (e.mods.isShiftDown()) {
        select(dragHit_.module, true);
        return;
    }
    if (!isSelected(dragHit_.module))
        select(dragHit_.module, false);
    touch(dragHit_.module);
    if (const auto* m = doc_.patch().area(location_).find(dragHit_.module))
        dragOffset_ = e.getPosition() - ModulePainter::moduleBounds(*m).getPosition();
    drag_ = Drag::Module;
    repaintModules();
}

void AreaView::mouseDrag(const juce::MouseEvent& e)
{
    dragPos_ = e.getPosition();
    switch (drag_) {
    case Drag::Value: {
        const auto* m = doc_.patch().area(location_).find(dragHit_.module);
        if (!m)
            return;
        const int max = ModulePainter::maxValue(context(*m), *dragHit_.element);
        const int perStep = SpecialControls::isSeqSlider(*dragHit_.element)
            ? SpecialControls::seqSliderPixelsPerValue(context(*m)) * (e.mods.isShiftDown() ? 4 : 1)
            : pixelsPerStep(max, e.mods.isShiftDown());
        const int steps = (e.getMouseDownY() - e.y) / perStep;
        setValue(dragHit_, juce::jlimit(0, max, dragStartValue_ + steps), true);
        status(describe(dragHit_));
        break;
    }
    case Drag::Range: {
        const int range = juce::jlimit(-127, 127, dragStartRange_ + (e.getMouseDownY() - e.y));
        const auto loc = location_;
        const auto module = dragHit_.module;
        const auto param = static_cast<std::uint8_t>(dragHit_.element->codeRef);
        const auto variation = static_cast<std::uint8_t>(doc_.focusedVariation());
        const auto group = morphGroupForRange_;
        touch(module);
        doc_.performCoalesced("morph:" + juce::String(module) + ":" + juce::String(param), [=](g2::Patch& p) {
            if (range == 0)
                g2::edit::clearMorph(p, variation, loc, module, param);
            else
                g2::edit::setMorph(p, variation, loc, module, param, group, static_cast<std::int8_t>(range));
        });
        status("Morph " + juce::String(g2::edit::morphLabel(doc_.patch(), group)) + " range: "
               + juce::String(juce::roundToInt(range * 100.0 / 127.0)) + "%");
        break;
    }
    case Drag::Bend: {
        if (!highlighted_ || e.getDistanceFromDragStart() < 3)
            break;
        const auto ends = cableEnds(*highlighted_);
        if (!ends)
            break;
        // The curve passes through the mouse: the bend is its offset from
        // the middle of the jacks, in unzoomed pixels.
        const auto offset = e.position - (ends->first + ends->second) * 0.5f;
        const g2::CableBend bend{static_cast<std::int16_t>(juce::roundToInt(offset.x)),
                                 static_cast<std::int16_t>(juce::roundToInt(offset.y))};
        const auto cable = *highlighted_;
        const auto loc = location_;
        doc_.performCoalesced("bend:" + juce::String(cable.fromModule) + ":" + juce::String(cable.fromConn) + ":"
                                  + juce::String(cable.toModule) + ":" + juce::String(cable.toConn),
                              [&](g2::Patch& p) { g2::edit::setCableBend(p, loc, cable, bend); });
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        break;
    }
    case Drag::Band:
    case Drag::Module:
    case Drag::Cable:
        overlay_->repaint();
        if (drag_ == Drag::Cable)
            if (const auto target = jackAt(dragPos_))
                status(describe(hitAt(dragPos_)));
        break;
    case Drag::None:
        break;
    }
}

void AreaView::mouseUp(const juce::MouseEvent& e)
{
    const Drag drag = std::exchange(drag_, Drag::None);
    doc_.endCoalescing();
    if (std::exchange(pushed_, false)) {
        pressMomentary(dragHit_, 0); // released, wherever the mouse is now
        return;
    }
    if (e.mods.isPopupMenu())
        return;
    if (drag == Drag::Cable && cableFrom_) {
        const auto from = *std::exchange(cableFrom_, std::nullopt);
        if (const auto to = jackAt(e.getPosition()); to && !(to->module == from.module && to->conn == from.conn
                                                              && to->isOutput == from.isOutput)) {
            // Cables run from an output (or an input, for links) to an input.
            auto src = from, dst = *to;
            if (dst.isOutput)
                std::swap(src, dst);
            juce::String error;
            const auto loc = location_;
            touch(dst.module);
            if (!doc_.perform("Connect", [&](g2::Patch& p) {
                    g2::edit::connect(p, loc, {src.module, src.conn, src.isOutput}, {dst.module, dst.conn, dst.isOutput});
                }, &error))
                status("Cannot connect: " + error);
        }
        overlay_->repaint();
        return;
    }
    if (drag == Drag::Band) {
        const auto band = juce::Rectangle<int>(dragHit_.local, e.getPosition());
        for (const auto& m : doc_.patch().area(location_).modules)
            if (band.intersects(ModulePainter::moduleBounds(m)) && !isSelected(m.index))
                selection_.push_back(m.index);
        overlay_->repaint();
        repaintModules();
        return;
    }
    if (drag == Drag::Module && e.getDistanceFromDragStart() > 3) {
        const auto* grabbed = doc_.patch().area(location_).find(dragHit_.module);
        if (!grabbed)
            return;
        const auto cell = gridCell(e.getPosition() - dragOffset_ + juce::Point<int>(kModuleWidth / 2, 0));
        const int dCol = cell.x - grabbed->col, dRow = cell.y - grabbed->row;
        const auto loc = location_;
        const auto moved = selection_;
        doc_.perform(moved.size() > 1 ? "Move modules" : "Move module",
                     [&](g2::Patch& p) { g2::edit::moveModules(p, loc, moved, dCol, dRow); });
        return;
    }
    if (dragHit_.element && isEditable(*dragHit_.element) && dragHit_.element->kind != "Knob"
        && e.getDistanceFromDragStart() < 4)
        clickControl(dragHit_);
}

void AreaView::mouseDoubleClick(const juce::MouseEvent& e)
{
    // On a bend point: remove it. On a cord: add a bend point there, or
    // remove the cable's one.
    if (const auto bent = bendPointAt(e.getPosition())) {
        toggleBendPoint(*bent, e.position);
        return;
    }
    const auto h = hitAt(e.getPosition());
    const bool onControl = h.element && (ModulePainter::isJack(*h.element) || isEditable(*h.element));
    if (const auto cable = onControl ? std::nullopt : cableAt(e.getPosition())) {
        toggleBendPoint(*cable, e.position);
        return;
    }
    // A knob: by default the double-click drags the morph range (mouseDown);
    // resetting to the default value is G2fresh's opt-in.
    if (!h.element || h.element->kind != "Knob" || knobDoubleClick() != KnobDoubleClick::Reset)
        return;
    const auto* m = doc_.patch().area(location_).find(h.module);
    const auto* def = m ? m->def() : nullptr;
    if (def && juce::isPositiveAndBelow(h.element->codeRef, static_cast<int>(def->params.size())))
        setValue(h, def->params[static_cast<std::size_t>(h.element->codeRef)].defaultValue, false);
}

void AreaView::mouseMagnify(const juce::MouseEvent& e, float scaleFactor)
{
    if (onZoom)
        onZoom(scaleFactor, e.getPosition());
}

void AreaView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown()) {
        if (onZoom && w.deltaY != 0.0f)
            onZoom(w.deltaY > 0 ? 1.1f : 1.0f / 1.1f, e.getPosition());
        return;
    }
    // The original's wheel only scrolls (CScrollViewEx::DoMouseWheel);
    // editing a knob with it is G2fresh's opt-in.
    const auto h = hitAt(e.getPosition());
    const auto* m = doc_.patch().area(location_).find(h.module);
    if (!wheelEditsKnobs() || !m || !h.element || h.element->kind != "Knob") {
        Component::mouseWheelMove(e, w); // let the viewport scroll
        return;
    }
    const auto c = context(*m);
    const int step = w.deltaY > 0 ? 1 : w.deltaY < 0 ? -1 : 0;
    const int v = ModulePainter::value(c, *h.element).value_or(0);
    setValue(h, juce::jlimit(0, ModulePainter::maxValue(c, *h.element), v + step), true);
    status(describe(h));
}

juce::String AreaView::getTooltip()
{
    // Only over the title strip, so tooltips don't get in the way of editing.
    if (!hover_.module || hover_.element || hover_.local.y > 14)
        return {};
    const auto* m = doc_.patch().area(location_).find(hover_.module);
    return m && m->def() ? moduleTooltip(*m->def()) : juce::String();
}

void AreaView::deleteHighlightedCable()
{
    const auto cable = highlightedCable();
    highlightCable(std::nullopt);
    if (!cable)
        return;
    const auto loc = location_;
    doc_.perform("Delete cable", [&](g2::Patch& p) { g2::edit::disconnect(p, loc, *cable); });
}

bool AreaView::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && highlighted_) {
        highlightCable(std::nullopt);
        return true;
    }
    if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && highlighted_) {
        deleteHighlightedCable();
        return true;
    }
    if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && hasSelection()) {
        deleteSelected();
        return true;
    }
    return false;
}

void AreaView::setValue(const Hit& h, int value, bool coalesce)
{
    touch(h.module);
    const auto loc = location_;
    const auto module = h.module;
    const auto ref = static_cast<std::uint8_t>(h.element->codeRef);
    const auto variation = static_cast<std::uint8_t>(doc_.focusedVariation());
    const auto v = static_cast<std::uint8_t>(value);
    const bool isMode = h.element->kind == "PartSelector";
    auto edit = [=](g2::Patch& p) {
        if (isMode)
            g2::edit::setMode(p, loc, module, ref, v);
        else
            g2::edit::setParam(p, loc, module, ref, variation, v);
    };
    if (coalesce)
        doc_.performCoalesced("value:" + juce::String(module) + ":" + juce::String(ref), edit);
    else
        doc_.perform("Change value", edit);
}

bool AreaView::isPushButton(const Hit& h) const
{
    if (!h.element || (h.element->kind != "ButtonText" && h.element->kind != "TextEdit") || h.element->codeRef < 0)
        return false;
    return g2::edit::isMomentary(doc_.patch(), location_, h.module, static_cast<std::uint8_t>(h.element->codeRef));
}

void AreaView::pressMomentary(const Hit& h, int value)
{
    touch(h.module);
    const auto loc = location_;
    const auto module = h.module;
    const auto param = static_cast<std::uint8_t>(h.element->codeRef);
    const auto variation = static_cast<std::uint8_t>(doc_.focusedVariation());
    doc_.performMomentary([=](g2::Patch& p) {
        g2::edit::setParam(p, loc, module, param, variation, static_cast<std::uint8_t>(value));
    });
    status(describe(h));
}

void AreaView::clickSpecial(const Hit& h, SpecialControls::Hit part)
{
    namespace sp = g2::special;
    const auto loc = location_;
    const auto module = h.module;
    const auto variation = static_cast<std::uint8_t>(doc_.focusedVariation());
    touch(module);
    using Part = SpecialControls::Part;
    switch (part.part) {
    case Part::VocoderButton: {
        const auto op = static_cast<sp::VocoderOp>(part.index);
        doc_.perform(juce::String("Vocoder bands ") + sp::vocoderOpLabel(op), [&](g2::Patch& p) {
            sp::vocoderPreset(p, loc, module, variation, op, &editorRandom());
        });
        break;
    }
    case Part::Zoom:
    case Part::OffsetLeft:
    case Part::OffsetRight: {
        auto view = sp::noteSeqView(doc_.patch(), loc, module);
        if (part.part == Part::Zoom)
            view.zoom = sp::nextNoteSeqZoom(view.zoom);
        else
            view.offset = sp::stepNoteSeqOffset(view.offset, part.part == Part::OffsetRight);
        if (view == sp::noteSeqView(doc_.patch(), loc, module))
            break; // already at the end of the range
        doc_.perform(part.part == Part::Zoom ? "Note sequencer zoom" : "Note sequencer octave",
                     [&](g2::Patch& p) { sp::setNoteSeqView(p, loc, module, view); });
        break;
    }
    case Part::DrumUp:
    case Part::DrumDown: {
        auto& selector = drumSelectors_[module];
        selector.update(doc_.patch(), loc, module, variation);
        if (const auto preset = selector.step(part.part == Part::DrumUp))
            doc_.perform(juce::String("Drum preset ") + sp::drumPresetName(*preset),
                         [&](g2::Patch& p) { sp::applyDrumPreset(p, loc, module, variation, *preset); });
        break;
    }
    case Part::DrumName: {
        const auto current = sp::drumPresetIndex(doc_.patch(), loc, module, variation);
        juce::PopupMenu menu;
        for (int i = 0; i < sp::kDrumPresets; ++i)
            menu.addItem(i + 1, sp::drumPresetName(i), true, current && *current == i);
        const auto* m = doc_.patch().area(loc).find(module);
        if (!m)
            break;
        const auto target = localAreaToGlobal(ModulePainter::elementBounds(*h.element)
                                                  .translated(ModulePainter::moduleBounds(*m).getX(),
                                                              ModulePainter::moduleBounds(*m).getY()));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(target),
                           [safe = juce::Component::SafePointer<AreaView>(this), loc, module, variation](int result) {
                               if (!safe || result <= 0)
                                   return;
                               const int preset = result - 1;
                               auto& selector = safe->drumSelectors_[module];
                               selector.index = preset;
                               selector.matched = true;
                               safe->doc_.perform(juce::String("Drum preset ") + sp::drumPresetName(preset),
                                                  [&](g2::Patch& p) { sp::applyDrumPreset(p, loc, module, variation, preset); });
                           });
        break;
    }
    case Part::None:
        break;
    }
}

void AreaView::clickControl(const Hit& h)
{
    const auto* m = doc_.patch().area(location_).find(h.module);
    if (!m)
        return;
    const auto c = context(*m);
    const auto& e = *h.element;
    const int value = ModulePainter::value(c, e).value_or(0);
    const int max = ModulePainter::maxValue(c, e);
    const auto r = ModulePainter::elementBounds(e);
    const auto local = h.local - r.getPosition();

    if (SpecialControls::isSpecial(e)) {
        clickSpecial(h, SpecialControls::partAt(e, local));
        return;
    }
    if (e.kind == "ButtonText" || e.kind == "TextEdit") {
        setValue(h, value ? 0 : std::min(1, max), false);
    } else if (e.kind == "ButtonFlat" || e.kind == "LevelShift") {
        setValue(h, value >= max ? 0 : value + 1, false);
    } else if (e.kind == "ButtonRadio" || e.kind == "ButtonRadioEdit") {
        const bool vertical = e.orientation == "Vertical";
        const int n = e.kind == "ButtonRadio" ? std::max(1, e.buttonCount) : std::max(1, e.columns * e.rows);
        const int cols = e.kind == "ButtonRadio" ? (vertical ? 1 : n) : std::max(1, e.columns);
        const int rows = (n + cols - 1) / cols;
        const int i = juce::jlimit(0, n - 1, (local.y * rows / std::max(1, r.getHeight())) * cols
                                                 + local.x * cols / std::max(1, r.getWidth()));
        setValue(h, std::min(i, max), false);
    } else if (e.kind == "ButtonIncDec") {
        const bool up = e.type == "Left/Right" ? local.x >= r.getWidth() / 2 : local.y < r.getHeight() / 2;
        setValue(h, juce::jlimit(0, max, value + (up ? 1 : -1)), false);
    } else if (e.kind == "PartSelector") {
        juce::PopupMenu menu;
        for (int i = 0; i <= max; ++i) {
            juce::PopupMenu::Item item;
            item.itemID = i + 1;
            item.isTicked = i == value;
            if (e.image.isValid() && e.imageWidth > 0 && i < std::max(1, e.imageCount)) {
                auto frame = e.image.getClippedImage({i * e.imageWidth, 0, e.imageWidth, e.image.getHeight()});
                auto drawable = std::make_unique<juce::DrawableImage>();
                drawable->setImage(frame);
                item.image = std::move(drawable);
            }
            item.text = g2::paramtext::formatSingle(
                m->def() && juce::isPositiveAndBelow(e.codeRef, static_cast<int>(m->def()->modes.size()))
                    ? m->def()->modes[static_cast<std::size_t>(e.codeRef)].textFunc : 0,
                static_cast<std::uint8_t>(i));
            menu.addItem(std::move(item));
        }
        const auto target = localAreaToGlobal(r.translated(ModulePainter::moduleBounds(*m).getX(),
                                                           ModulePainter::moduleBounds(*m).getY()));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(target),
                           [safe = juce::Component::SafePointer<AreaView>(this), h](int result) {
                               if (safe && result > 0)
                                   safe->setValue(h, result - 1, false);
                           });
    }
}

void AreaView::rename(std::uint8_t module)
{
    const auto* mod = doc_.patch().area(location_).find(module);
    if (!mod)
        return;
    auto* w = new juce::AlertWindow("Rename module", "Name (up to 16 characters):", juce::MessageBoxIconType::NoIcon);
    w->addTextEditor("name", juce::String(mod->name));
    // As the original's name dialog: keys outside the G2 set are refused (CNameDialog).
    if (auto* editor = w->getTextEditor("name"))
        editor->setInputRestrictions(PatchDocument::kMaxNameLength, PatchDocument::allowedNameCharacters());
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    w->enterModalState(true, juce::ModalCallbackFunction::create(
                                 [safe = juce::Component::SafePointer<AreaView>(this), module, w](int r) {
                                     if (!safe || r != 1)
                                         return;
                                     const auto name = w->getTextEditorContents("name").substring(0, 16).toStdString();
                                     const auto loc = safe->location_;
                                     safe->doc_.perform("Rename module", [&](g2::Patch& p) {
                                         g2::edit::renameModule(p, loc, module, name);
                                     });
                                 }),
                       true);
}

void AreaView::showModuleMenu(std::uint8_t module)
{
    const auto* m = doc_.patch().area(location_).find(module);
    if (!m)
        return;
    enum { kRename = 1, kDuplicate, kCopy, kCut, kDelete, kLock, kColourBase = 100, kReplaceBase = 1000 };
    const bool many = selection_.size() > 1;
    juce::PopupMenu colours;
    for (int c : ModulePainter::moduleColourMenuOrder()) {
        juce::PopupMenu::Item item(c == 0 ? juce::String("Default") : juce::String("Colour ") + juce::String(c));
        item.itemID = kColourBase + c;
        item.colour = ModulePainter::moduleColour(c);
        item.isTicked = m->color == c;
        colours.addItem(std::move(item));
    }
    // Replace: the modules of the same group, as the original's Replace button offers.
    juce::PopupMenu replace;
    for (const auto& item : g2::replace::menu(m->type, location_))
        replace.addItem(kReplaceBase + item.type, item.label, item.enabled, item.type == m->type);
    juce::PopupMenu menu;
    menu.addItem(kRename, "Rename...", !many);
    menu.addSubMenu("Replace With", replace, !many && replace.getNumItems() > 1);
    menu.addSubMenu(many ? "Colour (selection)" : "Colour", colours);
    menu.addItem(kLock, "Lock (keep when randomizing)", true, m->locked);
    menu.addSeparator();
    menu.addItem(kCopy, many ? "Copy Modules" : "Copy");
    menu.addItem(kCut, many ? "Cut Modules" : "Cut");
    menu.addItem(kDuplicate, many ? "Duplicate Modules" : "Duplicate");
    menu.addSeparator();
    menu.addItem(kDelete, many ? "Delete Modules" : "Delete");
    menu.showMenuAsync({}, [safe = juce::Component::SafePointer<AreaView>(this), module](int result) {
        if (!safe || result <= 0)
            return;
        auto& self = *safe;
        const auto loc = self.location_;
        if (result >= kReplaceBase) {
            const auto newType = static_cast<std::uint8_t>(result - kReplaceBase);
            std::uint8_t replaced = 0;
            juce::String error;
            if (self.doc_.perform("Replace module", [&](g2::Patch& p) {
                    replaced = g2::replace::replaceModule(p, loc, module, newType);
                }, &error)) {
                self.selection_ = {replaced};
                self.touch(replaced);
            } else {
                self.status("Cannot replace: " + error);
            }
        } else if (result >= kColourBase) {
            const auto colour = static_cast<std::uint8_t>(result - kColourBase);
            const auto targets = self.selection_.empty() ? std::vector<std::uint8_t>{module} : self.selection_;
            self.doc_.perform("Change module colour", [&](g2::Patch& p) {
                for (auto index : targets)
                    g2::edit::setModuleColor(p, loc, index, colour);
            });
        } else if (result == kLock) {
            const auto targets = self.selection_.empty() ? std::vector<std::uint8_t>{module} : self.selection_;
            const auto* first = self.doc_.patch().area(loc).find(module);
            const bool lock = first && !first->locked;
            self.doc_.perform(lock ? "Lock modules" : "Unlock modules", [&](g2::Patch& p) {
                for (auto index : targets)
                    if (auto* mod = p.area(loc).find(index))
                        mod->locked = lock;
            });
        } else if (result == kRename) {
            self.rename(module);
        } else if (result == kCopy || result == kCut) {
            clipboard() = self.copySelection();
            if (result == kCut)
                self.deleteSelected();
        } else if (result == kDuplicate) {
            self.duplicateSelection();
        } else if (result == kDelete) {
            self.deleteSelected();
        }
    });
}

void AreaView::showControlMenu(const Hit& h)
{
    const auto* m = doc_.patch().area(location_).find(h.module);
    const auto* def = m ? m->def() : nullptr;
    if (!def || h.element->kind == "PartSelector" || !juce::isPositiveAndBelow(h.element->codeRef, static_cast<int>(def->params.size())))
        return;
    const auto module = h.module;
    const auto param = static_cast<std::uint8_t>(h.element->codeRef);
    const auto variation = static_cast<std::uint8_t>(doc_.focusedVariation());
    const auto& patch = doc_.patch();
    const auto loc = location_;
    enum { kNoMorph = 1, kClearKnob, kClearCc, kLabel, kDefault, kMorphBase = 100, kKnobBase = 200, kCcBase = 400 };

    juce::PopupMenu morph;
    const auto current = g2::edit::morphOf(patch, variation, loc, module, param);
    morph.addItem(kNoMorph, "None", true, !current);
    for (int g = 0; g < g2::kMorphGroups; ++g) {
        juce::PopupMenu::Item item(juce::String(g + 1) + ": " + juce::String(g2::edit::morphLabel(patch, g)));
        item.itemID = kMorphBase + g;
        item.colour = ModulePainter::morphColour(g);
        item.isTicked = current && current->group == g;
        morph.addItem(std::move(item));
    }

    juce::PopupMenu knobs;
    const auto knob = g2::edit::knobOf(patch, loc, module, param);
    for (int page = 0; page < 5; ++page) {
        juce::PopupMenu pageMenu;
        for (int sub = 0; sub < 3; ++sub) {
            juce::PopupMenu subMenu;
            for (int k = 0; k < g2::edit::kKnobsPerPage; ++k) {
                const int index = (page * 3 + sub) * g2::edit::kKnobsPerPage + k;
                juce::String text = "Knob " + juce::String(k + 1);
                if (const auto& a = patch.knobs[static_cast<std::size_t>(index)]; a && index != knob.value_or(-1))
                    text << "  (in use)";
                subMenu.addItem(kKnobBase + index, text, true, knob == index);
            }
            pageMenu.addSubMenu(juce::String::charToString(static_cast<juce::juce_wchar>('A' + sub)), subMenu);
        }
        knobs.addSubMenu("Page " + juce::String(page + 1), pageMenu);
    }
    knobs.addSeparator();
    knobs.addItem(kClearKnob, "No knob", knob.has_value());

    // The controllers the original offers (MIDICtrl::IsValid, not
    // pre-assigned); none for a parameter holding CC 7 or 17 or not
    // MIDI-assignable (CControlMenu removes the item).
    juce::PopupMenu ccs;
    const auto cc = g2::edit::midiCcOf(patch, loc, module, param);
    const bool midiItem = g2::edit::canAssignMidiCc(patch, loc, module, param);
    for (int group = 0; group < 8 && midiItem; ++group) {
        juce::PopupMenu groupMenu;
        for (int n = group * 16; n < std::min(group * 16 + 16, 120); ++n) {
            if (!g2::edit::isValidMidiCc(static_cast<std::uint8_t>(n)) || g2::edit::isPreAssignedMidiCc(static_cast<std::uint8_t>(n)))
                continue;
            const bool used = std::any_of(patch.controllers.begin(), patch.controllers.end(),
                                          [&](const g2::CtrlAssign& a) { return a.cc == n; });
            groupMenu.addItem(kCcBase + n, "CC " + juce::String(n) + (used && cc != n ? "  (in use)" : ""), true, cc == n);
        }
        ccs.addSubMenu("CC " + juce::String(group * 16) + "-" + juce::String(std::min(group * 16 + 15, 119)), groupMenu);
    }
    ccs.addSeparator();
    ccs.addItem(kClearCc, "No MIDI controller", cc.has_value());

    juce::PopupMenu menu;
    menu.addSectionHeader(juce::String(m->name) + "  " + def->params[param].name);
    menu.addSubMenu("Morph" + (current ? " (" + juce::String(g2::edit::morphLabel(patch, current->group)) + ")" : juce::String()), morph);
    menu.addSubMenu("Assign Knob" + (knob ? " (" + juce::String(g2::edit::knobName(*knob)) + ")" : juce::String()), knobs);
    if (midiItem)
        menu.addSubMenu("MIDI Controller" + (cc ? " (CC " + juce::String(*cc) + ")" : juce::String()), ccs);
    menu.addSeparator();
    // Only label buttons and label radio buttons have a name to change (CanChangeName).
    const auto labels = g2::edit::paramLabels(patch, loc, module, param);
    if (!labels.empty())
        menu.addItem(kLabel, labels.size() > 1 ? "Rename Button..." : "Rename Button Label...");
    menu.addItem(kDefault, "Default Value");
    // The radio button under the mouse, for its caption.
    int button = 0;
    if (labels.size() > 1) {
        const auto r = ModulePainter::elementBounds(*h.element);
        const auto local = h.local - r.getPosition();
        const int cols = std::max(1, h.element->columns);
        const int rows = std::max(1, (static_cast<int>(labels.size()) + cols - 1) / cols);
        button = juce::jlimit(0, static_cast<int>(labels.size()) - 1,
                              (local.y * rows / std::max(1, r.getHeight())) * cols + local.x * cols / std::max(1, r.getWidth()));
    }
    menu.showMenuAsync({}, [safe = juce::Component::SafePointer<AreaView>(this), h, module, param, variation, loc,
                            current, button](int r) {
        if (!safe || r <= 0)
            return;
        auto& doc = safe->doc_;
        if (r == kNoMorph) {
            doc.perform("Remove morph", [&](g2::Patch& p) { g2::edit::clearMorph(p, variation, loc, module, param); });
        } else if (r >= kMorphBase && r < kMorphBase + g2::kMorphGroups) {
            const auto group = static_cast<std::uint8_t>(r - kMorphBase);
            const auto range = static_cast<std::int8_t>(current ? current->range : 32);
            safe->morphGroupForRange_ = group;
            doc.perform("Assign morph", [&](g2::Patch& p) { g2::edit::setMorph(p, variation, loc, module, param, group, range); });
        } else if (r >= kKnobBase && r < kKnobBase + g2::kKnobCount) {
            doc.perform("Assign knob", [&](g2::Patch& p) { g2::edit::assignKnob(p, r - kKnobBase, loc, module, param); });
        } else if (r == kClearKnob) {
            doc.perform("Remove knob", [&](g2::Patch& p) {
                if (const auto k = g2::edit::knobOf(p, loc, module, param))
                    g2::edit::clearKnob(p, *k);
            });
        } else if (r >= kCcBase && r < kCcBase + 128) {
            const auto n = static_cast<std::uint8_t>(r - kCcBase);
            doc.perform("Assign MIDI controller", [&](g2::Patch& p) { g2::edit::assignMidiCc(p, n, loc, module, param); });
        } else if (r == kClearCc) {
            doc.perform("Remove MIDI controller", [&](g2::Patch& p) {
                if (const auto c = g2::edit::midiCcOf(p, loc, module, param))
                    g2::edit::clearMidiCc(p, *c);
            });
        } else if (r == kDefault) {
            const auto* mod = doc.patch().area(loc).find(module);
            if (mod && mod->def())
                safe->setValue(h, mod->def()->params[param].defaultValue, false);
        } else if (r == kLabel) {
            const auto captions = g2::edit::paramLabels(doc.patch(), loc, module, param);
            if (captions.empty())
                return;
            // The original's "Param name" dialog: 7 characters of the G2 set.
            auto* w = new juce::AlertWindow("Param name", "Label (up to 7 characters, empty for the panel's):",
                                            juce::MessageBoxIconType::NoIcon);
            w->addTextEditor("label", PatchDocument::fromG2Bytes(
                                          captions[static_cast<std::size_t>(juce::jlimit(0, static_cast<int>(captions.size()) - 1, button))]));
            if (auto* editor = w->getTextEditor("label"))
                editor->setInputRestrictions(static_cast<int>(g2::edit::kLabelLength), PatchDocument::allowedNameCharacters());
            w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            w->enterModalState(true, juce::ModalCallbackFunction::create([safe, module, param, loc, w, button](int res) {
                if (!safe || res != 1)
                    return;
                const auto label = w->getTextEditorContents("label").toStdString();
                safe->doc_.perform("Rename parameter",
                                   [&](g2::Patch& p) { g2::edit::setParamLabel(p, loc, module, param, label, button); });
            }), true);
        }
    });
}

void AreaView::showCableMenu(const g2::Cable& cable)
{
    enum { kDelete = 1, kBendPoint, kColourBase = 100 };
    juce::PopupMenu colours;
    for (int c = 0; c < 7; ++c) {
        juce::PopupMenu::Item item(cableColourName(static_cast<g2::CableColor>(c)));
        item.itemID = kColourBase + c;
        item.colour = ModulePainter::cableColour(static_cast<g2::CableColor>(c));
        item.isTicked = static_cast<int>(cable.color) == c;
        colours.addItem(std::move(item));
    }
    juce::PopupMenu menu;
    menu.addSubMenu("Cable Colour", colours);
    menu.addItem(kBendPoint, cable.bend ? "Remove Bend Point" : "Add Bend Point");
    menu.addItem(kDelete, "Delete Cable");
    menu.showMenuAsync({}, [safe = juce::Component::SafePointer<AreaView>(this), cable](int r) {
        if (!safe || r <= 0)
            return;
        const auto loc = safe->location_;
        if (r == kDelete)
            safe->doc_.perform("Delete cable", [&](g2::Patch& p) { g2::edit::disconnect(p, loc, cable); });
        else if (r == kBendPoint) {
            // Added from the menu: at the middle of the current curve.
            juce::Point<float> where;
            if (const auto ends = safe->cableEnds(cable)) {
                const auto path = safe->cablePath(ends->first, ends->second, cable.bend);
                where = path.getPointAlongPath(path.getLength() * 0.5f);
            }
            safe->toggleBendPoint(cable, where);
        }
        else
            safe->doc_.perform("Change cable colour", [&](g2::Patch& p) {
                g2::edit::setCableColor(p, loc, cable, static_cast<g2::CableColor>(r - kColourBase));
            });
    });
}

void AreaView::showJackMenu(const Jack& jack)
{
    std::vector<g2::Cable> cables;
    for (const auto& c : doc_.patch().area(location_).cables)
        if ((c.fromModule == jack.module && c.fromConn == jack.conn && c.fromIsOutput == jack.isOutput)
            || (!jack.isOutput && c.toModule == jack.module && c.toConn == jack.conn))
            cables.push_back(c);
    juce::PopupMenu menu;
    menu.addItem(1, cables.size() == 1 ? "Disconnect" : "Disconnect all", !cables.empty());
    menu.showMenuAsync({}, [safe = juce::Component::SafePointer<AreaView>(this), cables](int result) {
        if (!safe || result != 1)
            return;
        const auto loc = safe->location_;
        safe->doc_.perform("Disconnect", [&](g2::Patch& p) {
            for (const auto& c : cables)
                g2::edit::disconnect(p, loc, c);
        });
    });
}

bool AreaView::isSelected(std::uint8_t module) const
{
    return std::find(selection_.begin(), selection_.end(), module) != selection_.end();
}

void AreaView::select(std::uint8_t module, bool toggle)
{
    if (!toggle)
        selection_.clear();
    if (const auto it = std::find(selection_.begin(), selection_.end(), module); it != selection_.end())
        selection_.erase(it);
    else
        selection_.push_back(module);
    repaintModules();
}

void AreaView::selectAll()
{
    selection_.clear();
    for (const auto& m : doc_.patch().area(location_).modules)
        selection_.push_back(m.index);
    repaintModules();
}

void AreaView::clearSelection()
{
    selection_.clear();
    repaintModules();
}

void AreaView::deleteSelected()
{
    if (selection_.empty())
        return;
    const auto indices = std::exchange(selection_, {});
    const auto loc = location_;
    doc_.perform(indices.size() > 1 ? "Delete modules" : "Delete module",
                 [&](g2::Patch& p) { g2::edit::removeModules(p, loc, indices); });
}

g2::edit::Clipboard AreaView::copySelection() const
{
    return g2::edit::copyModules(doc_.patch(), location_, selection_);
}

void AreaView::paste(const g2::edit::Clipboard& clip)
{
    if (clip.empty())
        return;
    // Below the selection, or at the first free row of the first column.
    int col = 0, row = g2::edit::freeRow(doc_.patch(), location_, 0);
    if (!selection_.empty()) {
        col = 127;
        int bottom = 0;
        for (const auto& m : doc_.patch().area(location_).modules)
            if (isSelected(m.index)) {
                col = std::min<int>(col, m.col);
                bottom = std::max(bottom, ModulePainter::moduleBounds(m).getBottom() / kRowHeight);
            }
        row = bottom;
    }
    const auto loc = location_;
    std::vector<std::uint8_t> added;
    juce::String error;
    if (doc_.perform("Paste", [&](g2::Patch& p) {
            added = g2::edit::pasteModules(p, loc, clip, static_cast<std::uint8_t>(col), static_cast<std::uint8_t>(row));
        }, &error)) {
        selection_ = added;
        if (!added.empty())
            touch(added.front());
        repaintModules();
    } else {
        status("Cannot paste: " + error);
    }
}

void AreaView::duplicateSelection()
{
    paste(copySelection());
}

void AreaView::addModule(std::uint8_t type, std::optional<juce::Point<int>> where)
{
    const auto loc = location_;
    const auto cell = where ? gridCell(*where) : juce::Point<int>(0, g2::edit::freeRow(doc_.patch(), loc, 0));
    std::uint8_t added = 0;
    juce::String error;
    if (doc_.perform("Add module", [&](g2::Patch& p) {
            added = g2::edit::addModule(p, loc, type, static_cast<std::uint8_t>(cell.x), static_cast<std::uint8_t>(cell.y));
            g2::edit::resolveOverlaps(p, loc, added);
        }, &error)) {
        selection_ = {added};
        touch(added);
    } else
        status("Cannot add module: " + error);
}

void AreaView::insertModule(std::uint8_t type)
{
    // CTabButton::Action: below the selection's lowest module in its
    // rightmost column (else at 0,0), make room, clear the selection.
    const auto loc = location_;
    const auto [col, row] = g2::edit::insertPosition(doc_.patch(), loc, selection_);
    std::uint8_t added = 0;
    juce::String error;
    if (doc_.perform("Add module", [&](g2::Patch& p) {
            added = g2::edit::addModule(p, loc, type, col, row);
            g2::edit::resolveOverlaps(p, loc, added);
        }, &error)) {
        clearSelection();
        touch(added);
    } else
        status("Cannot add module: " + error);
}

bool AreaView::isInterestedInDragSource(const SourceDetails& d)
{
    return d.description.toString().startsWith(kModuleDragPrefix);
}

void AreaView::itemDragMove(const SourceDetails& d)
{
    dropCell_ = gridCell(d.localPosition);
    overlay_->repaint();
}

void AreaView::itemDragExit(const SourceDetails&)
{
    dropCell_.reset();
    overlay_->repaint();
}

void AreaView::itemDropped(const SourceDetails& d)
{
    dropCell_.reset();
    const int type = d.description.toString().fromFirstOccurrenceOf(kModuleDragPrefix, false, false).getIntValue();
    addModule(static_cast<std::uint8_t>(type), d.localPosition);
}

} // namespace g2ui
