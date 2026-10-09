// The editor's shared look: one font size for all UI text, and a LookAndFeel
// that applies it to buttons, tabs, toggles and menus.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace g2ui::theme {

inline constexpr float kFontSize = 14.0f;

inline juce::Font font(bool bold = false)
{
    return juce::Font(juce::FontOptions(kFontSize, bold ? juce::Font::bold : juce::Font::plain));
}

class LookAndFeel : public juce::LookAndFeel_V4 {
public:
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return font(); }
    juce::Font getPopupMenuFont() override { return font(); }
    juce::Font getComboBoxFont(juce::ComboBox&) override { return font(); }
    juce::Font getLabelFont(juce::Label& l) override { return l.getFont().withHeight(kFontSize); }
    juce::Font getTabButtonFont(juce::TabBarButton&, float) override { return font(); }
    juce::Font getAlertWindowMessageFont() override { return font(); }
    juce::Font getAlertWindowFont() override { return font(); }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool down) override
    {
        const float tick = kFontSize + 2.0f;
        drawTickBox(g, b, 4.0f, (static_cast<float>(b.getHeight()) - tick) * 0.5f, tick, tick, b.getToggleState(),
                    b.isEnabled(), highlighted, down);
        g.setColour(b.findColour(juce::ToggleButton::textColourId).withMultipliedAlpha(b.isEnabled() ? 1.0f : 0.5f));
        g.setFont(font());
        g.drawFittedText(b.getButtonText(), b.getLocalBounds().withTrimmedLeft(juce::roundToInt(tick) + 10).withTrimmedRight(2),
                         juce::Justification::centredLeft, 1);
    }
};

} // namespace g2ui::theme
