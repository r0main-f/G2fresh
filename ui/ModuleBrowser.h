// The module browser: one tab per category, one button per module. As the
// original's module bar, a click focuses a module, a double-click (or Return)
// adds it to the area last clicked, below the selection; or drag it onto
// either area.
#pragma once

#include "ModuleBarKeys.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace g2::db {
struct ModuleDef;
}

namespace g2ui {

// A compact description of a module for tooltips: name and category on the
// first line, then the module's short summary and the connectors.
juce::String moduleTooltip(const g2::db::ModuleDef& def);

class ModuleBrowser : public juce::Component, private juce::ChangeListener {
public:
    ModuleBrowser();
    ~ModuleBrowser() override;

    std::function<void(std::uint8_t type)> onAdd;
    std::function<void(const juce::String&)> onStatus;

    void resized() override;

    // The module bar's keys, which the patch window hands here first (ModuleBarKeys.h): Tab and Shift+Tab, < > , .
    // and Return or Enter. `repeat`: the key's auto-repeat. Returns whether the key was the bar's.
    bool routeKey(const juce::KeyPress& key, bool repeat);
    // The search field (G2fresh's own, ModuleSearch.h): takes the keyboard, its text selected.
    void focusSearch();

private:
    class ModuleButton;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void showCategory(int index);
    // The defs of a category's modules, in the original toolbar's order.
    static std::vector<const g2::db::ModuleDef*> categoryModules(int index);
    void showFocused();
    void showSearch(const juce::String& query);
    bool searching() const { return search_.getText().trim().isNotEmpty(); }
    void focusButton(const ModuleButton* b);

    juce::TabbedButtonBar tabs_{juce::TabbedButtonBar::TabsAtTop};
    // The module buttons scroll horizontally when they don't fit.
    juce::Viewport strip_;
    juce::Component stripContent_;
    juce::OwnedArray<ModuleButton> buttons_;
    ModuleBarKeys keys_;
    juce::TextEditor search_;
};

} // namespace g2ui
