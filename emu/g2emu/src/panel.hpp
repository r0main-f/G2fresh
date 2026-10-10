// The G2's front panel board as the OS drives it (re/notes/g2-hardware-and-emulation.md §3.10). The board has no
// microcontroller: the OS strobes and reads it through byte latches on CS5 (0x15000000-07), drives the keyboard
// matrix's columns through a 16-bit latch on CS4 (0x14000000) and reads the rows on the ColdFire's parallel port
// (PADAT). The analogue controls are on the panel ADC (I2C, in Sim).
//
// CS5, as the OS uses it [C]:
//   write +0  bits 0-6: LED rows 8-14; bit 7: the input multiplexer's enable (set while +7 selects a byte)
//   write +1  LED rows 16-23
//   write +2  LED rows 0-7
//   write +3  LED column strobe, active low (8 columns; the OS strobes one at a time, 0x80 first)
//   write +4  LCD data bus (8 bits, shared by the five LCDs)
//   write +5  LCD 1-4 control: RS/E pairs 0x80/0x40 (LCD 1), 0x20/0x10, 0x08/0x04, 0x02/0x01 (LCD 4)
//   write +6  LCD 0 control: RS 0x02, E 0x01
//   write +7  input multiplexer select, active low: 0-5 button bytes, 6-7 encoder bytes
//   read  +0  the selected byte; with nothing selected: bits 6-7 the dial's quadrature, bits 4-5 the model
//
// Everything here runs on the machine's thread; Machine queues the UI's inputs and publishes snapshots.
#pragma once

#include "g2emu/panel.hpp"

#include <array>
#include <cstdint>
#include <deque>

namespace g2emu {

// An HD44780-compatible character LCD controller in 8-bit mode, write-only (R/W is not used by the OS).
struct Hd44780 {
    std::array<std::uint8_t, 80> ddram{};  // 0x00-0x27 line 1, 0x40-0x67 line 2 (stored at 40-79)
    std::array<std::uint8_t, 64> cgram{};
    std::uint8_t ac = 0;      // address counter (DDRAM address or CGRAM address)
    bool cgMode = false;      // the last address set was a CGRAM address
    bool increment = true, shiftOnWrite = false;
    bool displayOn = false, cursorOn = false, blinkOn = false;
    bool twoLines = false, eightBit = true;
    int shift = 0;            // display shift, 0..39
    std::uint64_t writes = 0;

    Hd44780() { ddram.fill(0x20); }
    void execute(bool rs, std::uint8_t v);
    void snapshot(PanelDisplay& d) const;

private:
    void command(std::uint8_t v);
    void data(std::uint8_t v);
    void moveAc(bool up);
    static int ddIndex(std::uint8_t addr) { return addr >= 0x40 ? 40 + (addr - 0x40) % 40 : addr % 40; }
};

class Panel {
public:
    Panel();

    // ---- the bus (machine thread) ----
    std::uint8_t readCs5(std::uint32_t off);
    void writeCs5(std::uint32_t off, std::uint8_t v);
    void writeCs4(std::uint16_t columns);
    // PADAT bits the panel drives (inputs) and their levels: the keyboard rows (bits 0-7, active low) for the columns
    // driven now, the sustain pedal (bit 13; bit 12 low: a pedal is plugged in)
    std::uint16_t gpioMask() const { return 0x30ff; }
    std::uint16_t gpioInputs() const;

    // ---- inputs (machine thread; Machine queues them from other threads) ----
    // A button by its scan position (mux byte 0-5, bit 0-7: index byte * 8 + bit). Presses and releases are queued
    // and each one shown to the OS only after it has scanned the previous state, so none is lost however short.
    void button(int raw, bool down);
    // An encoder by its scan position (0-7: byte 6 bits 7-6, 5-4, 3-2, 1-0, then byte 7 the same; 8: the dial),
    // turned by `transitions` quadrature steps (positive: clockwise). One step each time the OS has scanned it.
    void turn(int raw, int transitions);
    // A key of the keyboard matrix (0-63) pressed (first contact, then the second `delayBus` bus clocks later) or
    // released (in the opposite order)
    void key(int raw, bool down, std::uint64_t delayBus, std::uint64_t now);
    void sustain(bool plugged, bool down) { sustainPlugged_ = plugged; sustainDown_ = down; }
    void setModel(int bits) { modelBits_ = std::uint8_t(bits & 3); }
    // Moves timed inputs (key contacts) on to bus clock `now`; true if the GPIO inputs changed.
    bool advance(std::uint64_t now);

    // ---- state ----
    void snapshot(PanelState& s) const;
    std::uint64_t changes() const { return changes_; }  // counts writes that changed what is visible
    const std::array<Hd44780, 5>& lcds() const { return lcd_; }
    // LED history by the OS's LED number (0-255: byte n >> 3 of its LED buffer, bit n & 7): bit 0 the latest strobe
    std::uint32_t ledHistory(int n) const { return ledHist_[std::size_t(n & 255)]; }
    std::uint64_t ledStrobes(int n) const { return ledStrobes_[std::size_t(n & 255)]; }

    struct Trace {
        std::uint64_t cs5Writes[8] = {}, cs5Reads = 0, cs4Writes = 0;
        std::uint64_t selectReads[9] = {};
    };
    const Trace& trace() const { return trace_; }

private:
    void strobeLeds(std::uint8_t columnsActiveLow);
    void controlLcds(int latch, std::uint8_t v);
    void applyPending();
    std::uint8_t encoderBits(int raw) const;  // the pair of bits as the board presents them (bit 1 = A, bit 0 = B)

    std::array<std::uint8_t, 8> latch_{};
    std::array<Hd44780, 5> lcd_;
    std::uint64_t changes_ = 0;

    // LEDs
    std::array<std::uint32_t, 256> ledHist_{};
    std::array<std::uint64_t, 256> ledStrobes_{};

    // buttons: 6 bytes, active low
    std::array<std::uint8_t, 6> buttons_{};
    struct ButtonEvent { int raw; bool down; };
    std::deque<ButtonEvent> buttonQueue_;
    std::array<std::uint64_t, 9> selectReads_{};    // the OS's scans of each multiplexer byte (8: nothing selected)
    std::array<std::uint64_t, 9> changedAtRead_{};  // selectReads_ when the byte last changed

    // encoders 0-7 and the dial (8): the quadrature state in the OS's convention (clockwise 0 -> 2 -> 3 -> 1 for
    // the knobs, 0 -> 1 -> 3 -> 2 for the dial) and the transitions still to make
    std::array<std::uint8_t, 9> quad_{};
    std::array<int, 9> pendingTurn_{};
    static constexpr std::uint64_t DialDetentGap = 54000 * 8;  // 8 ms of bus clocks
    std::uint64_t now_ = 0, dialRestUntil_ = 0;

    // keyboard: 64 keys, two contacts each (A closes first)
    std::uint16_t columns_ = 0xffff;
    std::array<std::uint8_t, 64> contacts_{};  // bit 0 contact A, bit 1 contact B
    struct KeyEvent { std::uint64_t at; int raw; std::uint8_t contacts; };
    std::deque<KeyEvent> keyQueue_;
    bool sustainPlugged_ = false, sustainDown_ = false;
    std::uint8_t modelBits_ = 0;  // 0 G2, 3 G2X, 2 G2 Engine (rack) [C]

    Trace trace_;
};

} // namespace g2emu
