// The emulated G2's front panel as a UI sees it (re/notes/g2-hardware-and-emulation.md §3.10): what Clavia's OS
// shows on the five LCDs and the LEDs, and the controls a user works. The names are the labels printed on a
// G2 / G2X panel (as in the user manual v1.4 and photos of a G2X); the enum values are the hardware's scan positions
// and are stable: a UI may store them.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace g2emu {

// Which instrument the panel board says it is (two bits the OS reads at boot [C 0x30050864]). The G2X adds the two
// global modulation wheels (the OS ignores ADC inputs 0 and 6 otherwise) and a 61-key keyboard.
enum class PanelModel : std::uint8_t { G2 = 0, G2X = 1, G2Engine = 2 };

// ---- buttons: value = scan position (multiplexer byte * 8 + bit) ----
enum class PanelButton : std::uint8_t {
    // system functions, left of the main display
    NavUp = 0, NavDown = 1, NavRight = 2, NavLeft = 3,  // the four NAVIGATOR buttons
    LoadPatch = 4,                                      // "Load Patch"
    KbHold = 5,                                         // "KB Hold" (Shift: Panic)
    FocusCopy = 6,                                      // "Focus/Copy" (Shift: Assign/Paste)
    Shift = 7,                                          // "Shift/Clear"
    System = 8,                                         // "System" (Shift: Dump One)
    Patch = 9,                                          // "Patch" (Shift: Dump CC)
    Store = 10,                                         // "Store" (Shift: Store As...)
    DisplayMode = 11,                                   // "Display Mode" (Shift: Sub Func.)
    PerfMode = 12,                                      // "Performance" (Shift: Perf. Transfer)
    KbSplit = 13,                                       // "KB Split" (Shift: set split point)
    PatchSettings = 14,                                 // "Patch Settings" (Shift or double press: Global Panel)
    Morph = 15,                                         // "Morph" (Shift: Vari.Init)
    // parameter pages: 5 rows A-E (Osc, LFO, Env, Filter, Effect) and 3 columns 1-3
    PageA = 16, PageB, PageC, PageD, PageE,
    Page1 = 21, Page2, Page3,
    Variation1 = 24, Variation2, Variation3, Variation4, Variation5, Variation6, Variation7, Variation8,
    // the assignable buttons under the 8 knobs
    Button1 = 32, Button2, Button3, Button4, Button5, Button6, Button7, Button8,
    SlotA = 40, SlotB, SlotC, SlotD,
    OctaveDown = 44, OctaveUp = 45,
};
constexpr int PanelButtonCount = 46;  // scan positions 46 and 47 are not connected (the OS ignores them [C])

// ---- endless encoders ----
enum class PanelEncoder : std::uint8_t {
    Knob1 = 0, Knob2, Knob3, Knob4, Knob5, Knob6, Knob7, Knob8,  // the assignable knobs, left to right
    Dial = 8,                                                     // the rotary dial under the navigator buttons
};
constexpr int PanelEncoderCount = 9;

// ---- analogue controls (the panel's ADC) ----
enum class PanelAnalog : std::uint8_t {
    MasterLevel = 0,   // "Master Level" knob, 0..1
    PitchStick = 1,    // 0..1, 0.5 = at rest, 1.0 bends up (as the OS reads it: tests/test_g2emu.cpp)
    ModWheel = 2,      // 0..1
    ControlPedal = 3,  // "Ctrl.Pedal" jack, 0..1 (the OS applies its Ctrl Ped Gain)
    Aftertouch = 4,    // keyboard pressure, 0 = none .. 1
    GlobalWheel1 = 5,  // G2X only
    GlobalWheel2 = 6,  // G2X only
};
constexpr int PanelAnalogCount = 7;

// ---- LEDs ----
enum class PanelLed : std::uint8_t {
    Midi,                                   // "MIDI" (incoming MIDI)
    Mic20, Mic12, Mic0,                     // mic/In 1 level: "-20", "-12", "0 dB"
    System, Patch, Store, LoadPatch,
    SlotA, SlotB, SlotC, SlotD,             // slot active (blinks on the focused one when several are)
    SlotKbA, SlotKbB, SlotKbC, SlotKbD,     // keyboard assigned to the slot (label not known, see §3.10)
    OctaveMinus2, OctaveMinus1, Octave0, OctavePlus1, OctavePlus2,  // the five octave shift LEDs
    KbHold, KbSplit,
    KbSplit1, KbSplit2, KbSplit3, KbSplit4,  // the split point LEDs above the keyboard (left to right: inferred)
    PerfMode,                               // "Perf.Mode"
    SubFunc,                                // "Sub Func." (Display Mode)
    PatchSettings, GlobalPanel,
    Variation1, Variation2, Variation3, Variation4, Variation5, Variation6, Variation7, Variation8,
    Morph,
    PageA, PageB, PageC, PageD, PageE,
    Page1, Page2, Page3,
    Knob1, Knob2, Knob3, Knob4, Knob5, Knob6, Knob7, Knob8,  // the LED above each assignable button (a module LED)
    Button1, Button2, Button3, Button4, Button5, Button6, Button7, Button8,  // each assignable button's LED
    Count
};
constexpr int PanelLedCount = int(PanelLed::Count);
constexpr int PanelRingLeds = 15;  // the LED graph around each knob, counterclockwise end first

// The OS's own number of an LED (0-255: bit n & 7 of byte n >> 3 of its LED buffer), and of ring LED i of knob k
// (0-based): 32k + i.
std::uint8_t panelLedNumber(PanelLed led);

// Labels as printed on the panel, for a UI or debugging ("KB Hold", "Page A", "Variation 1", ...).
const char* panelName(PanelButton b);
const char* panelName(PanelEncoder e);
const char* panelName(PanelAnalog a);
const char* panelName(PanelLed l);

// ---- displays ----
// Five HD44780-type character LCDs, 2 lines of 16 characters (5x8 dots, character set "A00"): the main display
// (index 0) and the four assignable displays above knobs 1-2, 3-4, 5-6, 7-8 (indices 1-4). The OS defines up to 8
// characters of its own (CGRAM, codes 0-7, and 8-15 the same): the letters with descenders g, j, p, q, y.
enum class PanelDisplayId : std::uint8_t { Main = 0, Assign1 = 1, Assign2 = 2, Assign3 = 3, Assign4 = 4 };
constexpr int PanelDisplayCount = 5;

struct PanelDisplay {
    static constexpr int Columns = 16, Rows = 2;
    // the character codes shown, row by row (after the controller's display shift)
    std::array<std::uint8_t, Columns * Rows> chars{};
    // the user characters: 8 x 8 rows; bits 4-0 of each row are its 5 dots, left to right (row 7 is the cursor line)
    std::array<std::uint8_t, 64> cgram{};
    bool on = false;          // display on (off: blank)
    int cursor = -1;          // the cell under the cursor (row * 16 + column), -1 if outside the display
    bool cursorLine = false;  // an underline cursor is shown at `cursor`
    bool cursorBlink = false; // the cell at `cursor` blinks

    // A row as UTF-8: the A00 character set mapped to Unicode; a user character as the descender letter its dots
    // look like (g, j, p, q, y), else U+2592.
    std::string text(int row) const;
    // The 5x8 dots of user character `code` (0-15), rows top to bottom, bits 4-0 left to right; false for the
    // built-in characters (a UI draws those with an HD44780 A00 font).
    bool userGlyph(std::uint8_t code, std::array<std::uint8_t, 8>& rows) const;
};

// The A00 character set's code `c` as a Unicode code point (0x20-0x7D ASCII except 0x5C yen; 0x7E/0x7F arrows;
// 0xA1-0xDF half-width katakana; 0xE0-0xFF Greek and symbols); 0 for the user characters (0-15) and unused codes.
char32_t hd44780Unicode(std::uint8_t c);

// ---- a snapshot of everything the panel shows ----
struct PanelState {
    std::array<PanelDisplay, PanelDisplayCount> displays{};
    std::array<bool, PanelLedCount> leds{};                                  // by PanelLed
    std::array<std::array<bool, PanelRingLeds>, 8> rings{};                  // knob 1-8, LED 0 (lowest) .. 14
    std::array<bool, 256> rawLeds{};                                         // by the OS's LED number
    PanelModel model = PanelModel::G2;
    std::uint64_t generation = 0;  // grows whenever any of the above changed

    bool led(PanelLed l) const { return leds[std::size_t(l)]; }
    const PanelDisplay& display(PanelDisplayId d) const { return displays[std::size_t(d)]; }
};

} // namespace g2emu
