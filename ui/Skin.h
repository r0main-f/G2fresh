// The bundled Clavia skin: module panel layouts (PANL) and graphics, compiled
// into the binary from assets/clavia/ (see tools/assets/build_assets.py).
#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>
#include <vector>

namespace g2ui {

// One element of a module panel, flattened from the PANL tree.
struct PanelElement {
    juce::String kind;   // Knob, Input, Output, ButtonFlat, TextField, ...
    int id = 0;
    int x = 0, y = 0;
    int width = 0, height = 0;
    int codeRef = -1;    // parameter index (controls), connector index (jacks), mode index (PartSelector)
    int infoFunc = 0;
    int graphFunc = 0;   // Graph "Graph Func"
    int textFunc = 0;    // TextField "Text Func"
    int masterRef = -1;  // TextField: parameter shown
    juce::String type;   // knob size, jack signal, button behaviour, ...
    juce::String style;
    juce::String orientation;
    juce::StringArray options; // comma-separated labels
    juce::String dependencies;
    int buttonCount = 0, buttonWidth = 0, columns = 0, rows = 0;
    int imageCount = 0, imageWidth = 0;
    juce::Image image;   // decoded inline "rrggbb:..." image strip, if any
    float fontSize = 9.0f;  // Text
    int length = 0;         // Line
    bool thick = false;     // Line
    int zpos = 0;
};

enum class Look { Modern, Classic };
// The look used to draw modules: Modern at every launch, Classic on request
// for the session.
Look currentLook();
void setCurrentLook(Look look);
// The user's saved settings (look, cable animation, zoom...).
juce::PropertiesFile& userSettings();

// Whether cables show their signal flow with moving pulses (on at every launch).
bool cableAnimation();
void setCableAnimation(bool on);

struct PanelDef {
    int resId = 0;
    juce::String name;
    int height = 1;
    std::vector<PanelElement> elements;
};

class Skin {
public:
    static Skin& get();

    const PanelDef* panel(int resId) const;
    // A CBMP bitmap (module face or control sprite); invalid if missing.
    juce::Image cbmp(int resId) const;
    // A CBMP sprite strip with each frame's background made transparent.
    juce::Image sprite(int resId, int frameWidth) const;
    // A module face with its grey background replaced by `colour`.
    juce::Image tintedFace(int resId, juce::Colour colour) const;
    // A small JPEG sprite; invalid if missing.
    juce::Image jpeg(int resId) const;

private:
    Skin();
    std::map<int, PanelDef> panels_;
    mutable std::map<juce::String, juce::Image> images_;
    juce::Image load(const juce::String& name) const;
};

} // namespace g2ui
