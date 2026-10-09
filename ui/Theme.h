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

    // Tabs (module categories): unselected tabs are dark with a strip of their
    // colour; the selected tab is filled with its colour, bold, outlined, and
    // joins the panel below.
    void drawTabButton(juce::TabBarButton& b, juce::Graphics& g, bool over, bool down) override
    {
        const auto colour = b.getTabBackgroundColour();
        const bool front = b.isFrontTab();
        auto r = b.getLocalBounds().toFloat().reduced(1.0f, 0.0f);
        if (front) {
            juce::Path tab;
            tab.addRoundedRectangle(r.getX(), r.getY() + 1.0f, r.getWidth(), r.getHeight() - 1.0f, 5.0f, 5.0f,
                                    true, true, false, false);
            g.setColour(colour.withSaturation(std::min(1.0f, colour.getSaturation() * 1.25f)));
            g.fillPath(tab);
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.strokePath(tab, juce::PathStrokeType(1.5f));
        } else {
            auto body = r.reduced(0.0f, 2.0f).withTrimmedTop(1.0f);
            g.setColour(over ? juce::Colour(0xff40444c) : juce::Colour(0xff32353b));
            g.fillRoundedRectangle(body, 4.0f);
            g.setColour(colour.withAlpha(down ? 1.0f : 0.9f));
            g.fillRoundedRectangle(body.removeFromBottom(over ? 5.0f : 3.0f), 1.5f);
        }
        const auto ink = front ? (colour.getPerceivedBrightness() > 0.55f ? juce::Colour(0xff15171a) : juce::Colours::white)
                               : (over ? juce::Colours::white : juce::Colour(0xffb8bdc6));
        g.setColour(ink);
        g.setFont(font(front));
        g.drawFittedText(b.getButtonText(), b.getLocalBounds().reduced(6, 0).withTrimmedBottom(front ? 0 : 3),
                         juce::Justification::centred, 1);
    }

    void drawTabbedButtonBarBackground(juce::TabbedButtonBar& bar, juce::Graphics& g) override
    {
        g.setColour(juce::Colour(0xff26282c));
        g.fillRect(bar.getLocalBounds());
    }

    void drawTabAreaBehindFrontButton(juce::TabbedButtonBar& bar, juce::Graphics& g, int w, int h) override
    {
        // A line under the tabs, in the selected tab's colour: the tab looks
        // attached to the module row below it.
        if (auto* front = bar.getTabButton(bar.getCurrentTabIndex())) {
            g.setColour(front->getTabBackgroundColour());
            g.fillRect(0, h - 2, w, 2);
        }
    }

    int getTabButtonBestWidth(juce::TabBarButton& b, int) override
    {
        return juce::GlyphArrangement::getStringWidthInt(font(true), b.getButtonText()) + 22;
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
