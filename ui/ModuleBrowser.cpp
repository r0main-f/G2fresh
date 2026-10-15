#include "ModuleBrowser.h"

#include "g2/module_db.hpp"

namespace g2ui {

juce::String moduleTooltip(const g2::db::ModuleDef& def)
{
    const auto cats = g2::db::categories();
    juce::String title = def.longName ? def.longName : def.shortName;
    if (def.category >= 0 && static_cast<std::size_t>(def.category) < cats.size())
        title << "  \u00b7  " << cats[static_cast<std::size_t>(def.category)].name;
    juce::String body = def.description ? juce::String(def.description) : juce::String();
    auto count = [](std::size_t n, const char* what) {
        return juce::String(static_cast<int>(n)) + " " + what + (n == 1 ? "" : "s");
    };
    juce::String facts = count(def.inputs.size(), "input") + ", " + count(def.outputs.size(), "output");
    if (!def.params.empty())
        facts << ", " << count(def.params.size(), "control");
    return title + "\n" + (body.isNotEmpty() ? body + "\n" : juce::String()) + facts;
}

class ModuleBrowser::ModuleButton : public juce::TextButton {
public:
    ModuleButton(ModuleBrowser& owner, const g2::db::ModuleDef& def)
        : juce::TextButton(def.shortName), owner_(owner), type_(def.typeId)
    {
        setTooltip(moduleTooltip(def));
        setWantsKeyboardFocus(true);
        setClickingTogglesState(true);
        setRadioGroupId(1);
        // As the original's module bar (CTabButton::Click @00126c04): a
        // click only focuses the module; a double-click or Return inserts it.
        onClick = [this] {
            owner_.focusButton(this);
            grabKeyboardFocus();
        };
    }

    void mouseDoubleClick(const juce::MouseEvent&) override { insert(); }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::returnKey) {
            insert();
            return true;
        }
        return juce::TextButton::keyPressed(key);
    }

    void mouseEnter(const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseEnter(e);
        if (owner_.onStatus)
            owner_.onStatus(getButtonText() + ": double-click (or click, then Return) to add it below the selection, "
                                              "or drag it onto the patch");
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() < 5)
            return;
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this)) {
            container->startDragging("g2module:" + juce::String(type_), this);
            setState(juce::Button::buttonNormal);
        }
    }

private:
    void insert()
    {
        setToggleState(true, juce::dontSendNotification);
        if (owner_.onAdd)
            owner_.onAdd(type_);
    }

    ModuleBrowser& owner_;
public:
    std::uint8_t type() const { return type_; }

private:
    std::uint8_t type_;
};

namespace {

std::vector<int> moduleCounts()
{
    std::vector<int> counts;
    for (std::size_t c = 0; c < g2::db::categories().size(); ++c) {
        int n = 0;
        for (const auto& def : g2::db::modules())
            if (def.selectable && def.kind == g2::db::ModuleKind::Module && def.category == static_cast<int>(c))
                ++n;
        counts.push_back(n);
    }
    return counts;
}

} // namespace

ModuleBrowser::ModuleBrowser() : keys_(moduleCounts())
{
    for (const auto& c : g2::db::categories())
        tabs_.addTab(c.name, juce::Colour(c.r, c.g, c.b), -1);
    addAndMakeVisible(tabs_);
    strip_.setViewedComponent(&stripContent_, false);
    strip_.setScrollBarsShown(false, true); // the wheel scrolls horizontally
    strip_.setScrollBarThickness(6);
    addAndMakeVisible(strip_);
    tabs_.setCurrentTabIndex(2); // Osc
    tabs_.addChangeListener(this);
    showCategory(tabs_.getCurrentTabIndex());
}

ModuleBrowser::~ModuleBrowser()
{
    tabs_.removeChangeListener(this);
}

void ModuleBrowser::changeListenerCallback(juce::ChangeBroadcaster*)
{
    showCategory(tabs_.getCurrentTabIndex());
}

bool ModuleBrowser::routeKey(const juce::KeyPress& key, bool repeat)
{
    const auto m = key.getModifiers();
    ModuleBarKey k;
    k.tab = key.isKeyCode(juce::KeyPress::tabKey);
    k.insert = key.isKeyCode(juce::KeyPress::returnKey);
    const auto c = key.getTextCharacter();
    k.character = c > 0 && c < 128 ? static_cast<char>(c) : 0;
    k.shift = m.isShiftDown();
    k.other = m.isAltDown() || m.isCommandDown() || m.isCtrlDown();
    k.repeat = repeat;
    switch (keys_.press(k)) {
    case ModuleBarKeys::Result::None:
        return false;
    case ModuleBarKeys::Result::Insert:
        if (auto* b = buttons_[keys_.focused(keys_.group())]; b && onAdd)
            onAdd(b->type());
        return true;
    case ModuleBarKeys::Result::Handled:
        break;
    }
    if (tabs_.getCurrentTabIndex() != keys_.group())
        tabs_.setCurrentTabIndex(keys_.group()); // shows the group, its focused module lit
    else
        showFocused();
    if (auto* b = buttons_[keys_.focused(keys_.group())]; b && onStatus)
        onStatus(b->getButtonText() + ": Return adds it below the selection");
    return true;
}

void ModuleBrowser::focusButton(const ModuleButton* b)
{
    if (const int i = buttons_.indexOf(b); i >= 0)
        keys_.setFocused(keys_.group(), i);
}

void ModuleBrowser::showFocused()
{
    // The group's focused module lit, and scrolled into view.
    if (auto* b = buttons_[keys_.focused(keys_.group())]) {
        b->setToggleState(true, juce::dontSendNotification);
        const auto r = b->getBounds();
        const auto view = strip_.getViewArea();
        if (r.getX() < view.getX())
            strip_.setViewPosition(r.getX() - 2, 0);
        else if (r.getRight() > view.getRight())
            strip_.setViewPosition(r.getRight() + 2 - view.getWidth(), 0);
    }
}

std::vector<const g2::db::ModuleDef*> ModuleBrowser::categoryModules(int index)
{
    // In the original editor's toolbar order, which groups families together
    // (OscA, OscB, OscC, OscD, ...); by name for any module without a slot.
    std::vector<const g2::db::ModuleDef*> defs;
    for (const auto& def : g2::db::modules())
        if (def.selectable && def.kind == g2::db::ModuleKind::Module && def.category == index)
            defs.push_back(&def);
    std::sort(defs.begin(), defs.end(), [](const auto* a, const auto* b) {
        if (a->browserOrder != b->browserOrder)
            return a->browserOrder < b->browserOrder;
        return juce::String(a->shortName).compareNatural(b->shortName) < 0;
    });
    return defs;
}

void ModuleBrowser::showCategory(int index)
{
    buttons_.clear();
    for (const auto* def : categoryModules(index))
        stripContent_.addAndMakeVisible(buttons_.add(new ModuleButton(*this, *def)));
    keys_.setGroup(index);
    strip_.setViewPosition(0, 0);
    resized();
    showFocused();
}

void ModuleBrowser::resized()
{
    auto r = getLocalBounds();
    tabs_.setBounds(r.removeFromTop(28));
    strip_.setBounds(r);
    const int h = r.getHeight() - strip_.getScrollBarThickness() - 4;
    int x = 2;
    for (auto* b : buttons_) {
        const int w = std::max(48, b->getBestWidthForHeight(h) + 8);
        b->setBounds(x, 2, w, h);
        x += w + 2;
    }
    stripContent_.setSize(std::max(x, strip_.getWidth()), r.getHeight() - strip_.getScrollBarThickness());
}

} // namespace g2ui
