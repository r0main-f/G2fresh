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
        onClick = [this] {
            if (owner_.onAdd)
                owner_.onAdd(type_);
        };
    }

    void mouseEnter(const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseEnter(e);
        if (owner_.onStatus)
            owner_.onStatus(getButtonText() + ": click to add it to the voice area, or drag it onto the patch");
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
    ModuleBrowser& owner_;
    std::uint8_t type_;
};

ModuleBrowser::ModuleBrowser()
{
    for (const auto& c : g2::db::categories())
        tabs_.addTab(c.name, juce::Colour(c.r, c.g, c.b).withMultipliedSaturation(0.6f), -1);
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

void ModuleBrowser::showCategory(int index)
{
    buttons_.clear();
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
    for (const auto* def : defs)
        stripContent_.addAndMakeVisible(buttons_.add(new ModuleButton(*this, *def)));
    strip_.setViewPosition(0, 0);
    resized();
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
