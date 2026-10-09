#include "ModuleBrowser.h"

#include "g2/module_db.hpp"

namespace g2ui {

class ModuleBrowser::ModuleButton : public juce::TextButton {
public:
    ModuleButton(ModuleBrowser& owner, const g2::db::ModuleDef& def)
        : juce::TextButton(def.shortName), owner_(owner), type_(def.typeId)
    {
        setTooltip(def.longName ? def.longName : def.shortName);
        onClick = [this] {
            if (owner_.onAdd)
                owner_.onAdd(type_);
        };
    }

    void mouseEnter(const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseEnter(e);
        if (owner_.onStatus)
            owner_.onStatus(getTooltip() + "  (click to add, or drag onto the patch)");
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
    for (const auto& def : g2::db::modules())
        if (def.selectable && def.kind == g2::db::ModuleKind::Module && def.category == index)
            addAndMakeVisible(buttons_.add(new ModuleButton(*this, def)));
    resized();
}

void ModuleBrowser::resized()
{
    auto r = getLocalBounds();
    tabs_.setBounds(r.removeFromTop(24));
    r.reduce(2, 2);
    int x = r.getX();
    for (auto* b : buttons_) {
        const int w = std::max(48, b->getBestWidthForHeight(r.getHeight()) + 8);
        b->setBounds(x, r.getY(), w, r.getHeight());
        x += w + 2;
    }
}

} // namespace g2ui
