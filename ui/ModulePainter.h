// Draws a module the way the original editor does: the face bitmap, then the
// live state of every control from the panel definition (PANL).
#pragma once

#include "Skin.h"

#include "g2/patch.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <optional>

namespace g2ui {

inline constexpr int kModuleWidth = 255; // grid column pitch in pixels
inline constexpr int kRowHeight = 15;    // grid row pitch in pixels

// Everything needed to draw one module.
struct ModuleContext {
    const g2::Patch& patch;
    g2::Location location;
    const g2::Module& module;
    int variation = 0;
    const PanelDef* panel = nullptr; // nullptr: draw a plain box
    Look look = Look::Modern;
};

class ModulePainter {
public:
    static const PanelDef* panelFor(const g2::Module& m);
    static juce::Rectangle<int> moduleBounds(const g2::Module& m);

    // Bounds of an element inside the module, used for drawing and hit testing.
    static juce::Rectangle<int> elementBounds(const PanelElement& e);
    static bool isJack(const PanelElement& e) { return e.kind == "Input" || e.kind == "Output"; }
    static bool isControl(const PanelElement& e);
    // The value an element shows or edits (parameter, or mode for PartSelector).
    static std::optional<int> value(const ModuleContext& c, const PanelElement& e);
    static int maxValue(const ModuleContext& c, const PanelElement& e);
    // Text shown for an element's value (tooltips, status bar, text fields).
    static juce::String valueText(const ModuleContext& c, const PanelElement& e);
    static juce::Colour jackColour(const ModuleContext& c, const PanelElement& e);

    static void paint(juce::Graphics& g, const ModuleContext& c, const PanelElement* highlighted = nullptr);

    static juce::Colour cableColour(g2::CableColor color);

    // The 8 morph-group colours, as in the original editor's knob sprites.
    static juce::Colour morphColour(int group);

    // The 25 module colours of the original (Color::kModuleBackColorNN,
    // CPanel::MapPanelColorToRBGColor @0xbda68); 0 is the default grey.
    static constexpr int kModuleColours = 25;
    static juce::Colour moduleColour(int index);
    // The order of the original's colour menu (CPaletteView).
    static const std::array<int, kModuleColours>& moduleColourMenuOrder();

private:
    static void paintElement(juce::Graphics& g, const ModuleContext& c, const PanelElement& e, bool highlighted);
    static void paintModern(juce::Graphics& g, const ModuleContext& c, const PanelElement* highlighted);
    static void paintModernElement(juce::Graphics& g, const ModuleContext& c, const PanelElement& e, bool highlighted);
};

void paintClassicGraph(juce::Graphics& g, const ModuleContext& c, const PanelElement& e, juce::Rectangle<int> r);
void paintModernGraph(juce::Graphics& g, const ModuleContext& c, const PanelElement& e, juce::Rectangle<float> r);

} // namespace g2ui
