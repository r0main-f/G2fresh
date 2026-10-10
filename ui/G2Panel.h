// The front panel of a Nord Modular G2 / G2X as the Live view draws it: its
// controls, named as printed on a production G2X (photos; the v1.2 manual's
// drawing differs in places), and a snapshot of what the panel shows (displays, LEDs).
// The emulated G2's OS drives it (EmulatorHost); the view only draws the
// snapshot and reports the user's input.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace g2ui {

enum class PanelButton {
    // System functions
    System, Patch, Store, DisplayMode,
    NavUp, NavLeft, NavRight, NavDown, // Navigator (Left: Del, Right: Ins, Down: Sort Mode)
    LoadPatch, PerformanceMode, // "Load Patch"; Perf.Mode (second function: Perf. Transfer)
    SlotA, SlotB, SlotC, SlotD, // Keyboard Assign (ActiveSlots/Focus)
    KbSplit,
    OctaveDown, OctaveUp, KbHold, FocusCopy, Shift,
    // Sound functions
    Knob1, Knob2, Knob3, Knob4, Knob5, Knob6, Knob7, Knob8, // the buttons below the knobs
    Var1, Var2, Var3, Var4, Var5, Var6, Var7, Var8, Morph,
    PatchSettings,
    PageA, PageB, PageC, PageD, PageE, // Parameter Pages rows: Osc, LFO, Env, Filter, Effect
    Column1, Column2, Column3,
    Count
};

enum class PanelLed {
    Midi, MicLow, MicMid, MicHigh, // In 1 Level: -20, -12, 0 dB
    System, Patch, Store, SubFunc, PerformanceMode, LoadPatch,
    SlotA, SlotB, SlotC, SlotD,         // above the slot buttons (Keyboard Assign)
    FocusA, FocusB, FocusC, FocusD,     // below them (ActiveSlots/Focus)
    KbSplit,
    Octave1, Octave2, Octave3, Octave4, Octave5,
    KbHold,
    KnobUpper1, KnobUpper2, KnobUpper3, KnobUpper4, KnobUpper5, KnobUpper6, KnobUpper7, KnobUpper8,
    KnobLower1, KnobLower2, KnobLower3, KnobLower4, KnobLower5, KnobLower6, KnobLower7, KnobLower8,
    Var1, Var2, Var3, Var4, Var5, Var6, Var7, Var8, Morph,
    PatchSettings, GlobalPanel,
    PageA, PageB, PageC, PageD, PageE,
    Column1, Column2, Column3,
    Split1, Split2, Split3, Split4, // above the keyboard
    ModWheel, GlobalWheel1, GlobalWheel2, // the green LEDs in the wheels
    Count
};

// Continuous controls the OS reads (through the panel's ADC).
enum class PanelAnalog { MasterLevel, PitchStick, ModWheel, GlobalWheel1, GlobalWheel2, Count };

// The endless encoders: the 8 assignable knobs, then the rotary dial.
constexpr int kPanelKnobs = 8;
constexpr int kPanelDial = 8;

// One display as the OS drives it: characters (rows of text) or pixels.
struct PanelDisplay {
    int columns = 0, rows = 0;      // character display
    std::vector<std::string> text;  // `rows` lines of `columns` characters
    int width = 0, height = 0;      // graphic display
    std::vector<std::uint8_t> pixels; // width * height, row-major, 0 = off
};

struct PanelSnapshot {
    bool live = false;               // the emulated G2 runs and drives the panel
    std::uint64_t generation = 0;    // changes when anything below changes
    // [0] the main display (System functions), [1]-[4] the assignable displays, left to right
    std::array<PanelDisplay, 5> displays{};
    std::array<float, static_cast<std::size_t>(PanelLed::Count)> leds{}; // 0 off .. 1 fully on
    // The LED collar of each knob, segment by segment, clockwise (0 .. 1 each)
    std::array<std::vector<float>, kPanelKnobs> rings{};
};

// Labels printed on the panel, for drawing and tooltips.
const char* panelLabel(PanelButton button);

} // namespace g2ui
