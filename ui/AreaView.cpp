#include "AreaView.h"

#include "AssignMenus.h"
#include "IncDec.h"
#include "ModernIcons.h"
#include "ModuleBrowser.h"
#include "SpecialControls.h"

#include "g2/edit.hpp"
#include "g2/param_text.hpp"
#include "g2/replace.hpp"
#include "g2/special.hpp"

#include <cmath>

namespace g2ui {
namespace {

const juce::String kModuleDragPrefix = "g2module:";

bool isEditable(const PanelElement& e)
{
    return ModulePainter::isControl(e) || SpecialControls::isSpecial(e);
}

// Whether a point of a module is in a knob's control: its sprite, or the inc/dec pair below it.
bool inKnobControl(const PanelElement& e, juce::Point<int> local)
{
    if (e.kind != "Knob")
        return false;
    const auto k = incDecKnob(e.type.toStdString());
    return k && incDecControlSize(*k).contains(local.x - e.x, local.y - e.y);
}

// The parameter a control focuses: knobs, sliders and buttons do (CPnlKnobSliderABC::OnClick @000cb9b0,
// CPnlButtonABC::OnClick @000c3520 -> CPanel::CtrlClick @000bfda8); mode selectors never do.
std::optional<std::uint8_t> focusParam(const PanelElement& e)
{
    if (!isEditable(e) || e.kind == "PartSelector" || e.kind == "TextField" || e.codeRef < 0)
        return std::nullopt;
    return static_cast<std::uint8_t>(e.codeRef);
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

bool listHas(const std::vector<g2::Cable>& list, const g2::Cable& c)
{
    return std::any_of(list.begin(), list.end(), [&](const g2::Cable& x) { return sameEnds(x, c); });
}

// The jack popup's colour names (resource SMNU "Cable Colors"), in
// CableColor order: white is not one, it marks a chain without an output.
constexpr const char* kChainColourNames[6] = {"Audio", "Control", "Logic BG", "Logic FG", "User1", "User2"};

// The original's "drag copy" key, held at the drop, moves the jack's cables
// (CCableDragDropManager::IsMoveOperationRequested): Option on a Mac, Ctrl elsewhere.
bool moveKeyDown(const juce::ModifierKeys& mods)
{
#if JUCE_MAC
    return mods.isAltDown();
#else
    return mods.isCtrlDown();
#endif
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
    // Held, the inc/dec pair steps again after 400 ms, then every 70 ms (CPnlControl::UpDownIdleHandler @000c8ae4).
    incDecRepeat_.onTick = [this] {
        incDecRepeat_.startTimer(kIncDecRepeatMs);
        stepIncDec();
    };
}

AreaView::~AreaView()
{
    incDecRepeat_.stopTimer();
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
        chain_ = highlighted_ ? g2::edit::chainCables(doc_.patch(), location_,
                                                      g2::edit::lowerEnd(doc_.patch(), location_, *highlighted_))
                              : std::vector<g2::Cable>{};
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
    const auto f = doc_.focus();
    return {doc_.patch(), location_, m, doc_.focusedVariation(), ModulePainter::panelFor(m), currentLook(),
            doc_.focusedMorphGroup(), f.location == location_ && f.module == m.index ? f.param : -1,
            incDec_ && incDec_->module == m.index && f.location == location_ && f.module == m.index && f.param == incDec_->param
                ? incDec_->delta
                : 0};
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
                    && (ModulePainter::elementBounds(e).expanded(1).contains(h.local) || inKnobControl(e, h.local))) {
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
    // With a highlighted chain, the other cables are dimmed and it is drawn last.
    const bool dimmed = highlighted_.has_value() || !heldChain_.empty();
    if (dimmed)
        g.beginTransparencyLayer(0.25f);
    for (const auto& cable : doc_.patch().area(location_).cables) {
        if (listHas(heldChain_, cable) || listHas(chain_, cable) || (highlighted_ && sameEnds(cable, *highlighted_)))
            continue; // drawn on top below, even in a hidden colour
        if (!visible[static_cast<std::size_t>(cable.color)])
            continue; // hidden colour (View > Cables)
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
    if (dimmed) {
        g.endTransparencyLayer();
        auto paintGlowing = [&](const g2::Cable& cable, juce::Colour colour) {
            const auto a = jackCentre({cable.fromModule, cable.fromConn, cable.fromIsOutput});
            const auto b = jackCentre({cable.toModule, cable.toConn, false});
            if (!a || !b)
                return;
            g.setColour(colour.withAlpha(0.35f));
            g.strokePath(cablePath(*a, *b, cable.bend),
                         juce::PathStrokeType(11.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            paintCable(g, *a, *b, colour, flowing, cable.bend);
        };
        // A held jack's chain turns light blue (manual p83).
        for (const auto& cable : heldChain_)
            paintGlowing(cable, juce::Colour(0xff8fd3ff));
        if (heldChain_.empty()) {
            for (const auto& cable : chain_)
                if (!(highlighted_ && sameEnds(cable, *highlighted_)))
                    paintGlowing(cable, ModulePainter::cableColour(cable.color));
            if (const auto current = highlightedCable())
                paintHighlight(g, *current);
        }
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
    // Its serial chain: what Delete and the colours act on (as on a jack).
    chain_ = cable ? g2::edit::chainCables(doc_.patch(), location_, g2::edit::lowerEnd(doc_.patch(), location_, *cable))
                   : std::vector<g2::Cable>{};
    updateTimer();
    overlay_->repaint();
    if (cable)
        status(describeCable(*cable) + (chain_.size() > 1 ? "  (Delete removes its chain)" : "  (Delete removes it)"));
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
        // A double click and hold picks up the jack's cables to move them
        // (CPanel::HoleBeginCableMove); a plain press starts a new cable.
        // Either way the jack's chain lights up while it is held.
        moveDrag_ = e.getNumberOfClicks() >= 2;
        if (cableFrom_)
            heldChain_ = g2::edit::chainCables(doc_.patch(), location_,
                                               {cableFrom_->module, cableFrom_->conn, cableFrom_->isOutput});
        overlay_->repaint();
        drag_ = Drag::Cable;
        return;
    }
    if (dragHit_.element && isEditable(*dragHit_.element)) {
        // Mouse-down on a control focuses its parameter, and the focus selects its module; a Command-click (the
        // original's knob assignment) does not (CPanel::CtrlClick @000bfda8, CPatch::HandleParamFocusDump @000dd0fe).
        if (const auto param = focusParam(*dragHit_.element); param && !e.mods.isCommandDown()) {
            doc_.setFocus({location_, dragHit_.module, *param});
            selectFocused(dragHit_.module);
        }
        const auto* m = doc_.patch().area(location_).find(dragHit_.module);
        // The inc/dec pair under a knob: a step on mouse-down, then again while held; Option steps the morph range
        // (CPnlControl::UpDownOnClickHandler @000c8970).
        if (const auto k = dragHit_.element->kind == "Knob" ? incDecKnob(dragHit_.element->type.toStdString()) : std::nullopt;
            m && k && !e.mods.isCommandDown()) {
            const auto part = incDecAt(*k, dragHit_.local.x - dragHit_.element->x, dragHit_.local.y - dragHit_.element->y);
            if (part != IncDecPart::None) {
                startIncDec(dragHit_, part == IncDecPart::Inc ? +1 : -1, e.mods.isAltDown());
                return;
            }
        }
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
        // Alt-drag a knob: set its morph range in the focused morph group of
        // the current variation; so does a double-click and drag, as the
        // original's "Morph w/double click" option (on by default;
        // CPnlControl::ClickDragOnSecondClickHandler). A parameter that cannot
        // be morphed takes no morph drag (CPanel::CtrlAutoAssignMorph).
        const bool morphClick = e.getNumberOfClicks() >= 2 && knobDoubleClick() == KnobDoubleClick::Morph;
        if (drag_ == Drag::Value && (e.mods.isAltDown() || morphClick) && m) {
            const auto param = static_cast<std::uint8_t>(dragHit_.element->codeRef);
            if (!g2::edit::isMorphable(doc_.patch(), location_, dragHit_.module, param)) {
                status("This parameter cannot be morphed");
                drag_ = Drag::None;
                return;
            }
            dragStartRange_ = g2::edit::morphRange(doc_.patch(), static_cast<std::uint8_t>(doc_.focusedVariation()), location_,
                                                   dragHit_.module, param,
                                                   static_cast<std::uint8_t>(doc_.focusedMorphGroup()))
                                  .value_or(0);
            rangeDragDistance_ = 0;
            rangeDragLastY_ = e.y;
            drag_ = Drag::Range;
        }
        return;
    }
    if (e.mods.isShiftDown()) {
        select(dragHit_.module, true);
        return;
    }
    if (!isSelected(dragHit_.module)) {
        select(dragHit_.module, false);
        // A module selected by a click on its body takes the focus to its parameter 0 (CPanel::PanelClick
        // @000bd710 -> CPatch::AddToSelection @000d5d0a).
        doc_.setFocus(g2::focus::onModule(doc_.patch(), location_, dragHit_.module, doc_.focus()));
    }
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
        const auto* m = doc_.patch().area(location_).find(dragHit_.module);
        if (!m)
            return;
        const auto c = context(*m);
        const int max = std::max(1, ModulePainter::maxValue(c, *dragHit_.element));
        const int value = ModulePainter::value(c, *dragHit_.element).value_or(0);
        // CPnlControl::ClickDragIdleHandler @000c8d74 (lock mode 2): 256
        // pixels sweep the full range from the range at the click; value +
        // range stays within [0, max], and at either end the distance is
        // pulled back so that the drag turns at once.
        rangeDragDistance_ += rangeDragLastY_ - e.y;
        rangeDragLastY_ = e.y;
        const float scaled = static_cast<float>(rangeDragDistance_ * max) * 0.00390625f;
        int range = static_cast<short>(static_cast<int>(static_cast<float>(dragStartRange_) + scaled + 0.5f));
        if (value + range < 0) {
            range = -value;
            rangeDragDistance_ = static_cast<int>(
                std::floor(static_cast<float>(value + dragStartRange_) * -256.0f / static_cast<float>(max)) - 1.0f);
        }
        if (value + range > max) {
            range = max - value;
            rangeDragDistance_ = static_cast<int>(
                std::ceil(static_cast<float>(max - (value + dragStartRange_)) * 256.0f / static_cast<float>(max)) + 1.0f);
        }
        const auto loc = location_;
        const auto module = dragHit_.module;
        const auto param = static_cast<std::uint8_t>(dragHit_.element->codeRef);
        const auto variation = static_cast<std::uint8_t>(doc_.focusedVariation());
        const auto group = static_cast<std::uint8_t>(doc_.focusedMorphGroup());
        if (range == g2::edit::morphRange(doc_.patch(), variation, loc, module, param, group).value_or(0))
            break;
        const auto name = juce::String(g2::edit::morphLabel(doc_.patch(), group));
        // A 26th parameter takes no morph (CPanel::CtrlRequestMorphChange).
        if (!g2::edit::canMorph(doc_.patch(), variation, loc, module, param)) {
            status("Morph " + name + ": this variation already morphs " + juce::String(g2::edit::kMaxMorphedParams)
                   + " parameters");
            break;
        }
        touch(module);
        doc_.performCoalesced("morph:" + juce::String(module) + ":" + juce::String(param), [=](g2::Patch& p) {
            g2::edit::setMorph(p, variation, loc, module, param, group, range); // range 0 leaves the group
        });
        status("Morph " + name + " range: " + (range > 0 ? "+" : "") + juce::String(range) + " ("
               + juce::String(juce::roundToInt(range * 100.0 / max)) + "%)");
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
        if (drag_ == Drag::Cable && cableFrom_) {
            // What the drop would do (the original shows it with the cursor).
            const auto target = jackAt(dragPos_);
            const bool move = moveDrag_ || moveKeyDown(e.mods);
            const auto action = g2::edit::dropAction(
                doc_.patch(), location_, {cableFrom_->module, cableFrom_->conn, cableFrom_->isOutput},
                target ? std::optional<g2::edit::Endpoint>({target->module, target->conn, target->isOutput}) : std::nullopt,
                move);
            juce::String what = target ? describe(hitAt(dragPos_)) : juce::String();
            switch (action) {
            case g2::edit::DropAction::Move: what += "  (moves the cables here)"; break;
            case g2::edit::DropAction::Disconnect: what = "Release to disconnect"; break;
            case g2::edit::DropAction::None:
                if (target)
                    what += move ? "  (cannot move the cables here)" : "  (cannot connect)";
                break;
            case g2::edit::DropAction::Connect: break;
            }
            if (what.isNotEmpty())
                status(what);
        }
        break;
    case Drag::None:
        break;
    }
}

void AreaView::startIncDec(const Hit& h, int delta, bool morph)
{
    incDec_ = {h.module, static_cast<std::uint8_t>(h.element->codeRef), delta, morph, ++incDecPresses_};
    stepIncDec();
    incDecRepeat_.startTimer(kIncDecFirstRepeatMs);
}

void AreaView::stepIncDec()
{
    if (!incDec_)
        return;
    const auto press = *incDec_;
    const g2::focus::Focus f{location_, press.module, press.param};
    const auto variation = static_cast<std::uint8_t>(doc_.focusedVariation());
    const auto group = static_cast<std::uint8_t>(doc_.focusedMorphGroup());
    // One press, held or not, is one undo step (CPanel::CtrlClick then CtrlRelease record one pair).
    doc_.performCoalesced("Inc/dec " + juce::String(press.id), [&](g2::Patch& p) {
        if (!(press.morph ? g2::focus::stepMorph(p, f, variation, group, press.delta)
                          : g2::focus::stepValue(p, f, variation, press.delta)))
            throw std::invalid_argument("at the end of its range");
    });
    if (const auto* m = doc_.patch().area(location_).find(press.module); m && dragHit_.element)
        status(describe(dragHit_));
    repaintModules();
}

void AreaView::mouseUp(const juce::MouseEvent& e)
{
    if (incDec_) {
        incDec_.reset();
        incDecRepeat_.stopTimer();
        doc_.endCoalescing();
        repaintModules();
        return;
    }
    const Drag drag = std::exchange(drag_, Drag::None);
    doc_.endCoalescing();
    // A held jack's chain is lit only while it is held (CPanel::HoleRelease).
    const bool move = std::exchange(moveDrag_, false) || moveKeyDown(e.mods);
    if (!heldChain_.empty()) {
        heldChain_.clear();
        overlay_->repaint();
    }
    if (std::exchange(pushed_, false)) {
        pressMomentary(dragHit_, 0); // released, wherever the mouse is now
        return;
    }
    if (e.mods.isPopupMenu())
        return;
    if (drag == Drag::Cable && cableFrom_) {
        // CCableDragDropManager::Drop @00019514: connect, move the jack's
        // cables (double click and hold, or the move key at the drop), or,
        // moving onto nothing, disconnect.
        const auto from = *std::exchange(cableFrom_, std::nullopt);
        const g2::edit::Endpoint src{from.module, from.conn, from.isOutput};
        const auto to = jackAt(e.getPosition());
        const bool sameJack = to && to->module == from.module && to->conn == from.conn && to->isOutput == from.isOutput;
        const auto loc = location_;
        juce::String error;
        if (to && !sameJack) {
            const g2::edit::Endpoint dst{to->module, to->conn, to->isOutput};
            touch(dst.module);
            if (move) {
                if (!doc_.perform("Move cables", [&](g2::Patch& p) { g2::edit::moveCables(p, loc, src, dst); }, &error))
                    status("Cannot move the cables: " + error);
            } else if (!doc_.perform("Connect", [&](g2::Patch& p) { g2::edit::connect(p, loc, src, dst); }, &error)) {
                status("Cannot connect: " + error);
            }
        } else if (!to && move && g2::edit::dropAction(doc_.patch(), loc, src, std::nullopt, true) == g2::edit::DropAction::Disconnect) {
            doc_.perform("Disconnect", [&](g2::Patch& p) { g2::edit::disconnectJack(p, loc, src); });
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
    // The cord's serial chain, as the jack popup's Delete on its lower end.
    const auto loc = location_;
    const auto jack = g2::edit::lowerEnd(doc_.patch(), loc, *cable);
    doc_.perform("Delete cables", [&](g2::Patch& p) { g2::edit::deleteChain(p, loc, jack); });
}

bool AreaView::keyPressed(const juce::KeyPress& key)
{
    if (onArrowKey && (key.isKeyCode(juce::KeyPress::upKey) || key.isKeyCode(juce::KeyPress::downKey)
                       || key.isKeyCode(juce::KeyPress::leftKey) || key.isKeyCode(juce::KeyPress::rightKey)))
        return onArrowKey(key);
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
        // Modern: G2fresh's own icons and names (ModernIcons.h).
        const auto pictures = c.look == Look::Modern && m->def() ? icons::options(*m->def(), e)
                                                                 : std::span<const icons::Option>();
        for (int i = 0; i <= max; ++i) {
            juce::PopupMenu::Item item;
            item.itemID = i + 1;
            item.isTicked = i == value;
            if (i < static_cast<int>(pictures.size())) {
                const auto& o = pictures[static_cast<std::size_t>(i)];
                item.image = icons::menuImage(o, findColour(juce::PopupMenu::textColourId));
                item.text = o.label;
                menu.addItem(std::move(item));
                continue;
            }
            if (c.look == Look::Classic && e.image.isValid() && e.imageWidth > 0 && i < std::max(1, e.imageCount)) {
                auto frame = e.image.getClippedImage({i * e.imageWidth, 0, e.imageWidth, e.image.getHeight()});
                auto drawable = std::make_unique<juce::DrawableImage>();
                drawable->setImage(frame);
                item.image = std::move(drawable);
            }
            item.text = m->def() && e.codeRef >= 0
                            ? g2::db::formatMode(*m->def(), static_cast<std::size_t>(e.codeRef), static_cast<std::uint8_t>(i))
                            : g2::paramtext::formatSingle(0, static_cast<std::uint8_t>(i));
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
                    if (p.area(loc).find(index))
                        g2::edit::setLocked(p, loc, index, lock);
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
    namespace am = assignmenus;
    enum { kMorphAssign = 1, kLabel = 4, kDefault = 5 };

    // Morph assign: in the focused group of the current variation, checked
    // when the parameter is in it; offered for morphable parameters, and
    // only to those already morphed once the variation has 25
    // (CControlMenu::InitMenus @0008fcbe).
    const auto group = static_cast<std::uint8_t>(doc_.focusedMorphGroup());
    const bool morphed = g2::edit::morphRange(patch, variation, loc, module, param, group).has_value();
    const bool morphable = g2::edit::isMorphable(patch, loc, module, param);

    const auto knob = g2::edit::knobOf(patch, loc, module, param);
    const auto cc = g2::edit::midiCcOf(patch, loc, module, param);
    const auto ccs = am::ccMenu(patch, loc, module, param);

    juce::PopupMenu menu;
    menu.addSectionHeader(juce::String(m->name) + "  " + def->params[param].name);
    if (morphable)
        menu.addItem(kMorphAssign, "Morph Assign (" + juce::String(g2::edit::morphLabel(patch, group)) + ")",
                     morphed || g2::edit::canMorph(patch, variation, loc, module, param), morphed);
    menu.addSubMenu("Assign Knob" + (knob ? " (" + juce::String(g2::edit::knobName(*knob)) + ")" : juce::String()),
                    am::knobMenu(patch, loc, module, param));
    if (ccs.getNumItems() > 0)
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
    menu.showMenuAsync({}, [safe = juce::Component::SafePointer<AreaView>(this), h, module, param, variation, loc, group,
                            button](int r) {
        if (!safe || r <= 0)
            return;
        auto& doc = safe->doc_;
        if (r == kMorphAssign) {
            // Joining the group sets the value to 0 and the range to +max;
            // leaving it removes that group only (CControlMenu::HandleMenuSelection).
            doc.perform("Morph assign", [&](g2::Patch& p) { g2::edit::toggleMorphAssign(p, variation, loc, module, param, group); });
        } else if (am::isAssignItem(r)) {
            am::apply(doc, r, loc, module, param);
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

// A G2fresh addition (the original has no popup on a cord): it acts on the
// cord's serial chain as the jack popup does on its lower end, so a chain
// keeps one colour and white stays the mark of a chain without an output.
void AreaView::showCableMenu(const g2::Cable& cable)
{
    enum { kDelete = 1, kBendPoint, kColourBase = 100 };
    const auto jack = g2::edit::lowerEnd(doc_.patch(), location_, cable);
    const bool fed = g2::edit::hasOutput(doc_.patch(), location_, jack);
    juce::PopupMenu colours;
    for (int c = 0; c < 6; ++c) {
        juce::PopupMenu::Item item(kChainColourNames[c]);
        item.itemID = kColourBase + c;
        item.colour = ModulePainter::cableColour(static_cast<g2::CableColor>(c));
        item.isTicked = static_cast<int>(cable.color) == c;
        item.isEnabled = fed;
        colours.addItem(std::move(item));
    }
    juce::PopupMenu menu;
    menu.addSubMenu("Chain Colour", colours);
    menu.addItem(kBendPoint, cable.bend ? "Remove Bend Point" : "Add Bend Point");
    menu.addItem(kDelete, "Delete Chain");
    menu.showMenuAsync({}, [safe = juce::Component::SafePointer<AreaView>(this), cable, jack](int r) {
        if (!safe || r <= 0)
            return;
        const auto loc = safe->location_;
        if (r == kDelete)
            safe->doc_.perform("Delete cables", [&](g2::Patch& p) { g2::edit::deleteChain(p, loc, jack); });
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
                g2::edit::recolorChain(p, loc, jack, static_cast<g2::CableColor>(r - kColourBase));
            });
    });
}

// The original's connector popup (CPanel::HoleDoContextMenu @000c006e, menu
// resource "Connector"; manual p69-70).
void AreaView::showJackMenu(const Jack& jack)
{
    enum { kDisconnect = 1, kBreak, kDelete, kDeleteUnused, kColourBase = 100 };
    const g2::edit::Endpoint here{jack.module, jack.conn, jack.isOutput};
    const auto& patch = doc_.patch();
    const bool cabled = g2::edit::isCabled(patch, location_, here);
    const bool fed = g2::edit::hasOutput(patch, location_, here);
    juce::PopupMenu colours;
    for (int c = 0; c < 6; ++c)
        colours.addItem(kColourBase + c, kChainColourNames[c], fed);
    juce::PopupMenu menu;
    menu.addItem(kDisconnect, "Disconnect", cabled);
    menu.addItem(kBreak, "Break", g2::edit::canBreak(patch, location_, here));
    menu.addSubMenu("Color", colours, cabled);
    menu.addItem(kDelete, "Delete", cabled);
    menu.addItem(kDeleteUnused, "Delete Unused Cables");
    menu.showMenuAsync({}, [safe = juce::Component::SafePointer<AreaView>(this), here](int r) {
        if (!safe || r <= 0)
            return;
        const auto loc = safe->location_;
        auto& doc = safe->doc_;
        // An action that changes nothing is no undo step (the original checks
        // CUndoRedoPair::GetRedoSize).
        auto changed = [](bool any) {
            if (!any)
                throw std::invalid_argument("nothing to change");
        };
        switch (r) {
        case kDisconnect:
            doc.perform("Disconnect", [&](g2::Patch& p) { changed(g2::edit::disconnectJack(p, loc, here)); });
            break;
        case kBreak:
            doc.perform("Break", [&](g2::Patch& p) { changed(g2::edit::breakJack(p, loc, here)); });
            break;
        case kDelete:
            doc.perform("Delete cables", [&](g2::Patch& p) { changed(g2::edit::deleteChain(p, loc, here)); });
            break;
        case kDeleteUnused:
            if (!doc.perform("Delete unused cables", [&](g2::Patch& p) { changed(g2::edit::deleteUnusedCables(p)); }))
                safe->status("No unused cables");
            break;
        default:
            doc.perform("Change cable colour", [&](g2::Patch& p) {
                g2::edit::recolorChain(p, loc, here, static_cast<g2::CableColor>(r - kColourBase));
            });
        }
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

void AreaView::selectFocused(std::uint8_t module)
{
    // When the focus lands on a module that is not selected, the selection becomes that module
    // (CPatch::HandleParamFocusDump @000dd0fe).
    if (!isSelected(module))
        select(module, false);
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
            added = g2::edit::dropModule(p, loc, type, static_cast<std::uint8_t>(cell.x), static_cast<std::uint8_t>(cell.y));
        }, &error)) {
        selection_ = {added};
        touch(added);
        doc_.setFocus(g2::focus::onModule(doc_.patch(), loc, added, doc_.focus())); // CPatch::HandleModuleNew
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
        doc_.setFocus(g2::focus::onModule(doc_.patch(), loc, added, doc_.focus())); // CPatch::HandleModuleNew
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
