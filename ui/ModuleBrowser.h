// The module browser: one tab per category, one button per module. Click a
// button to add the module to the voice area, or drag it onto either area.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace g2ui {

class ModuleBrowser : public juce::Component, private juce::ChangeListener {
public:
    ModuleBrowser();
    ~ModuleBrowser() override;

    std::function<void(std::uint8_t type)> onAdd;
    std::function<void(const juce::String&)> onStatus;

    void resized() override;

private:
    class ModuleButton;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void showCategory(int index);

    juce::TabbedButtonBar tabs_{juce::TabbedButtonBar::TabsAtTop};
    juce::OwnedArray<ModuleButton> buttons_;
};

} // namespace g2ui
