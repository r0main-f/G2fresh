#include "AreaView.h"

#include "g2/edit.hpp"
#include "g2/param_text.hpp"

namespace g2ui {
namespace {

const juce::String kModuleDragPrefix = "g2module:";

bool isEditable(const PanelElement& e)
{
    return ModulePainter::isControl(e);
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
    if (cableAnimation())
        startTimerHz(30);
    else
        stopTimer();
    repaintModules();
}

void AreaView::repaintModules()
{
    modules_->repaint();
    overlay_->repaint();
}

void AreaView::timerCallback()
{
    if (!isShowing() || doc_.patch().area(location_).cables.empty())
        return;
    // Pulses move at a steady 48 px/s, whatever the frame rate.
    flowPhase_ = static_cast<float>(std::fmod(juce::Time::getMillisecondCounterHiRes() * 0.048, 36.0 * 1000.0));
    overlay_->repaint();
}

void AreaView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    if (selected_ && !doc_.patch().area(location_).find(selected_))
        selected_ = 0;
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
    return {doc_.patch(), location_, m, doc_.variation(), ModulePainter::panelFor(m), currentLook()};
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
        if (m.index == selected_) {
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
        if (const auto* m = doc_.patch().area(location_).find(dragHit_.module)) {
            const auto cell = gridCell(dragPos_ - dragOffset_ + juce::Point<int>(kModuleWidth / 2, 0));
            const auto b = ModulePainter::moduleBounds(*m);
            g.setColour(juce::Colours::white.withAlpha(0.3f));
            g.fillRoundedRectangle(b.withPosition(cell.x * kModuleWidth, cell.y * kRowHeight).toFloat(), 5.0f);
        }
    }
    if (dropCell_) {
        g.setColour(juce::Colours::white.withAlpha(0.3f));
        g.fillRoundedRectangle(juce::Rectangle<int>(dropCell_->x * kModuleWidth, dropCell_->y * kRowHeight, kModuleWidth,
                                                    2 * kRowHeight).toFloat(), 5.0f);
    }
    paintCables(g);
}

void AreaView::paintCable(juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, juce::Colour c, bool flowing)
{
    const float distance = a.getDistanceFrom(b);
    juce::Path path;
    path.startNewSubPath(a);
    if (currentLook() == Look::Classic) {
        // The original editor's style: a simple sagging curve with an outline.
        path.quadraticTo((a + b) * 0.5f + juce::Point<float>(0.0f, 10.0f + 0.25f * distance), b);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.strokePath(path, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(c);
        g.strokePath(path, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    } else {
        // Modern: a hanging cable (cubic curve with gravity), soft shadow, a
        // glossy tube and plugs.
        const float sag = std::min(90.0f, 12.0f + 0.22f * distance);
        path.cubicTo(a + juce::Point<float>((b.x - a.x) * 0.15f, sag), b + juce::Point<float>((a.x - b.x) * 0.15f, sag), b);
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
    for (const auto& cable : doc_.patch().area(location_).cables) {
        // Signals flow from the "from" end (an output, or the first input of
        // a link) to the input at the "to" end.
        const auto a = jackCentre({cable.fromModule, cable.fromConn, cable.fromIsOutput});
        const auto b = jackCentre({cable.toModule, cable.toConn, false});
        if (a && b)
            paintCable(g, *a, *b, ModulePainter::cableColour(cable.color), flowing);
    }
    if (drag_ == Drag::Cable && cableFrom_)
        if (const auto a = jackCentre(*cableFrom_))
            paintCable(g, *a, dragPos_.toFloat(), juce::Colours::white, false);
}

void AreaView::mouseMove(const juce::MouseEvent& e)
{
    const auto h = hitAt(e.getPosition());
    if (h.module != hover_.module || h.element != hover_.element) {
        hover_ = h;
        modules_->repaint();
    }
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
    dragPos_ = e.getPosition();
    dragHit_ = hitAt(e.getPosition());
    drag_ = Drag::None;
    if (!dragHit_.module) {
        selected_ = 0;
        repaintModules();
        return;
    }
    if (e.mods.isPopupMenu()) {
        if (dragHit_.element && ModulePainter::isJack(*dragHit_.element)) {
            if (const auto jack = jackAt(e.getPosition()))
                showJackMenu(*jack);
        } else {
            selected_ = dragHit_.module;
            repaintModules();
            showModuleMenu(dragHit_.module);
        }
        return;
    }
    if (dragHit_.element && ModulePainter::isJack(*dragHit_.element)) {
        cableFrom_ = jackAt(e.getPosition());
        drag_ = Drag::Cable;
        return;
    }
    if (dragHit_.element && isEditable(*dragHit_.element)) {
        const auto* m = doc_.patch().area(location_).find(dragHit_.module);
        dragStartValue_ = m ? ModulePainter::value(context(*m), *dragHit_.element).value_or(0) : 0;
        drag_ = dragHit_.element->kind == "Knob" ? Drag::Value : Drag::None;
        return;
    }
    selected_ = dragHit_.module;
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
        const int steps = (e.getMouseDownY() - e.y) / pixelsPerStep(max, e.mods.isShiftDown());
        setValue(dragHit_, juce::jlimit(0, max, dragStartValue_ + steps), true);
        status(describe(dragHit_));
        break;
    }
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
    if (drag == Drag::Module && e.getDistanceFromDragStart() > 3) {
        const auto cell = gridCell(e.getPosition() - dragOffset_ + juce::Point<int>(kModuleWidth / 2, 0));
        const auto index = dragHit_.module;
        const auto loc = location_;
        doc_.perform("Move module", [&](g2::Patch& p) {
            g2::edit::moveModule(p, loc, index, static_cast<std::uint8_t>(cell.x), static_cast<std::uint8_t>(cell.y));
        });
        return;
    }
    if (dragHit_.element && isEditable(*dragHit_.element) && dragHit_.element->kind != "Knob"
        && e.getDistanceFromDragStart() < 4)
        clickControl(dragHit_);
}

void AreaView::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto h = hitAt(e.getPosition());
    if (!h.element || h.element->kind != "Knob")
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
    const auto h = hitAt(e.getPosition());
    const auto* m = doc_.patch().area(location_).find(h.module);
    if (!m || !h.element || h.element->kind != "Knob") {
        Component::mouseWheelMove(e, w); // let the viewport scroll
        return;
    }
    const auto c = context(*m);
    const int step = w.deltaY > 0 ? 1 : w.deltaY < 0 ? -1 : 0;
    const int v = ModulePainter::value(c, *h.element).value_or(0);
    setValue(h, juce::jlimit(0, ModulePainter::maxValue(c, *h.element), v + step), true);
    status(describe(h));
}

bool AreaView::keyPressed(const juce::KeyPress& key)
{
    if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && selected_) {
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
    const auto variation = static_cast<std::uint8_t>(doc_.variation());
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

void AreaView::showModuleMenu(std::uint8_t module)
{
    const auto* m = doc_.patch().area(location_).find(module);
    if (!m)
        return;
    juce::PopupMenu menu;
    menu.addItem(1, "Rename...");
    menu.addItem(2, "Delete");
    menu.showMenuAsync({}, [safe = juce::Component::SafePointer<AreaView>(this), module](int result) {
        if (!safe)
            return;
        if (result == 2) {
            safe->selected_ = module;
            safe->deleteSelected();
        } else if (result == 1) {
            const auto* mod = safe->doc_.patch().area(safe->location_).find(module);
            if (!mod)
                return;
            auto* w = new juce::AlertWindow("Rename module", "Name (up to 16 characters):",
                                            juce::MessageBoxIconType::NoIcon);
            w->addTextEditor("name", juce::String(mod->name));
            w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
            w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            w->enterModalState(true, juce::ModalCallbackFunction::create([safe, module, w](int r) {
                if (!safe || r != 1)
                    return;
                const auto name = w->getTextEditorContents("name").substring(0, 16).toStdString();
                const auto loc = safe->location_;
                safe->doc_.perform("Rename module", [&](g2::Patch& p) { g2::edit::renameModule(p, loc, module, name); });
            }), true);
        }
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

void AreaView::deleteSelected()
{
    if (!selected_)
        return;
    const auto index = std::exchange(selected_, 0);
    const auto loc = location_;
    doc_.perform("Delete module", [&](g2::Patch& p) { g2::edit::removeModule(p, loc, index); });
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
        selected_ = added;
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
