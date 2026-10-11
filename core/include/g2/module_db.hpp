// The G2 module database: every module type the original editor knows, with
// its connectors, parameters, modes and display-function bindings. Generated
// from data/modules.json and data/param_text.json (tools/moduledb/gen_cpp.py).
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace g2::db {

enum class ModuleKind : std::uint8_t { Module, PatchSettings, NameBar };
enum class SignalType : std::uint8_t { Audio, Control, Logic };
// Jack colours as drawn by the editor; the two-colour kinds switch with the
// module's uprate (red/blue for audio/control, yellow/orange for logic).
enum class ConnColor : std::uint8_t { Red, Blue, Yellow, BlueRed, YellowOrange };
enum class Bandwidth : std::uint8_t { Static, Dynamic };

struct ConnectorDef {
    const char* name;
    SignalType type;
    ConnColor color;
    Bandwidth bandwidth;
};

// A dependency is the index of another parameter of the same module, or
// 0x80 | mode index for a module mode; 0xFF means none.
inline constexpr std::uint8_t kNoDep = 0xFF;
inline constexpr std::uint8_t kModeDep = 0x80;

struct ParamDef {
    const char* name;
    std::uint8_t min;
    std::uint8_t max;
    std::uint8_t defaultValue;
    bool morphable;
    std::uint8_t textFunc; // display function id, see g2::paramtext
    std::uint8_t deps[2];
    const char* rangeType;
    // A push button's parameter (param spec +0x0F, CPanel::CtrlRelease): 1
    // while held, 0 on release, and not an undo step. Sequencer Clear/Random,
    // the momentary switches, RndClkA Dice.
    bool momentary;
    // Can take a MIDI controller (param spec +0x10, CPanel::CtrlIsMidiAssignable);
    // false for the controller-number parameters of Automate, CtrlSend, CtrlRcv.
    bool midiAssignable;
};

struct ModeDef {
    const char* name;
    std::uint8_t min;
    std::uint8_t max;
    std::uint8_t defaultValue;
    std::uint8_t textFunc;
    const char* rangeType;
};

// A label control of a module panel (PANL `TextEdit`: CPnlLabelButton, one
// button; `ButtonRadioEdit`: CPnlLabelRadioButton, one caption per button):
// the user can rename its captions, which the patch stores as custom data.
struct LabelDef {
    std::uint8_t param;   // the parameter the control edits
    std::uint8_t buttons; // number of captions
    const char* defaults; // the panel's captions, comma-separated
};

struct ModuleDef {
    std::uint8_t typeId;
    ModuleKind kind;
    const char* shortName;
    const char* longName;
    std::uint8_t height;      // in 15 px grid rows
    std::int8_t category;     // index into categories(), -1 for none
    bool selectable;          // offered in the module browser
    bool defaultLocked;       // stored as the module's lock flag in patches
    std::uint16_t panelResId; // PANL resource of the original editor
    std::uint16_t faceResId;  // CBMP face bitmap of the original editor
    std::span<const ConnectorDef> inputs;
    std::span<const ConnectorDef> outputs;
    std::span<const ParamDef> params;
    std::span<const ModeDef> modes;
    const char* description; // short summary from the original help, or nullptr
    std::uint8_t browserOrder; // position in its category's toolbar in the original editor (255: none)
    std::span<const LabelDef> labels; // label controls, in panel order
};

struct CategoryDef {
    const char* name;
    std::uint8_t r, g, b;
};

std::span<const ModuleDef> modules();
std::span<const CategoryDef> categories();
// nullptr if the type is unknown.
const ModuleDef* find(std::uint8_t typeId);

// Display text of parameter `param` given the module's current parameter and
// mode values, exactly as the original editor shows it.
std::string formatParam(const ModuleDef& def, std::size_t param,
                        std::span<const std::uint8_t> paramValues,
                        std::span<const std::uint8_t> modeValues);

} // namespace g2::db
