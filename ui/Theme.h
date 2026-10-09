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

    // Tooltips: rounded and dark, at most kTooltipWidth wide; the first line
    // (up to the first newline) is bold.
    static constexpr int kTooltipWidth = 380;

    static juce::TextLayout tooltipLayout(const juce::String& text)
    {
        juce::AttributedString s;
        s.setWordWrap(juce::AttributedString::byWord);
        const auto title = text.upToFirstOccurrenceOf("\n", false, false);
        const auto body = text.fromFirstOccurrenceOf("\n", false, false);
        s.append(title, font(true), juce::Colours::white);
        if (body.isNotEmpty())
            s.append("\n" + body, font(), juce::Colour(0xffc9ced6));
        juce::TextLayout layout;
        layout.createLayout(s, static_cast<float>(kTooltipWidth));
        return layout;
    }

    juce::Rectangle<int> getTooltipBounds(const juce::String& text, juce::Point<int> screenPos,
                                          juce::Rectangle<int> parentArea) override
    {
        const auto layout = tooltipLayout(text);
        const int w = juce::roundToInt(layout.getWidth()) + 20, h = juce::roundToInt(layout.getHeight()) + 14;
        return juce::Rectangle<int>(screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 16,
                                    screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 18, w, h)
            .constrainedWithin(parentArea);
    }

    void drawTooltip(juce::Graphics& g, const juce::String& text, int width, int height) override
    {
        const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        g.setColour(juce::Colour(0xf2202328));
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(juce::Colour(0xff4a4f58));
        g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.0f);
        tooltipLayout(text).draw(g, r.reduced(10.0f, 7.0f));
    }

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
