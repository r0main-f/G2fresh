// The G2's hardware around its ColdFire, as the OS uses it (re/notes/g2-hardware-and-emulation.md §2.3, §3.6.2,
// §3.9): the MCF5407's on-chip modules at MBAR (interrupt controller, two timers, UART0 = MIDI, UART1, I2C, GPIO),
// the 8 MB flash on CS2, the ISP1181 USB device controller on CS3. A C++ port of the models of
// tools/firmware/g2hostemu.py (the reference), with the timers and the UART driven by the bus clock.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace g2emu {

// ---------------------------------------------------------------------------------------------------------------
// MCF5407 SIM and the modules at MBAR = 0x10000000 (offsets per the MCF5307/5407 user's manuals).
class Sim {
public:
    // time source: the bus clock now (54 MHz bus clocks since power-on)
    explicit Sim(std::function<std::uint64_t()> busNow);

    std::uint32_t read(std::uint32_t off, int size);
    void write(std::uint32_t off, int size, std::uint32_t value);

    // Moves the timers and the UART receiver to the bus clock `now`.
    void advance(std::uint64_t now);
    // The bus clock at which something next happens by itself (a timer reference, a MIDI byte), for the scheduler.
    std::uint64_t nextEvent() const;

    // External interrupt request lines (IRQ1..7) as the board drives them.
    void setExternalIrq(int level, bool on);
    // Highest pending unmasked interrupt level (0: none), and the vector for an acknowledge at `level`.
    int pendingLevel() const;
    std::uint8_t acknowledge(int level) const;

    // MIDI on UART0 (31250 baud: the OS programs UBG = 54, so the bus clock is 54 MHz [C]).
    void midiIn(const std::uint8_t* data, std::size_t n);
    std::vector<std::uint8_t> takeMidiOut();
    std::uint64_t midiOverruns() const { return uartOverruns_; }

    static constexpr std::uint32_t BusHz = 54000000;

private:
    struct Timer {
        std::uint16_t tmr = 0, trr = 0xffff, tcr = 0;
        std::uint8_t ter = 0;
        std::uint64_t base = 0;  // bus clock at which TCN was 0 (counting runs from there)
    };
    struct Uart {
        std::uint8_t umr[2] = {0, 0};
        int umrPtr = 0;
        std::uint8_t uimr = 0, uivr = 0x0f, ubg1 = 0, ubg2 = 0;
        bool rxEnabled = true, txEnabled = true;
        std::deque<std::uint8_t> fifo;  // the receiver's FIFO (3 bytes)
        std::vector<std::uint8_t> tx;
        bool overrun = false;
    };

    std::uint32_t timerPeriod(const Timer& t) const;  // bus clocks per count
    std::uint32_t timerCount(const Timer& t, std::uint64_t now) const;
    void timerUpdate(Timer& t, std::uint64_t now);
    std::uint8_t usr(const Uart& u) const;
    std::uint8_t uisr(const Uart& u) const;
    std::uint32_t readUart(Uart& u, std::uint32_t off);
    void writeUart(Uart& u, std::uint32_t off, std::uint8_t v);
    std::uint8_t readI2c(std::uint32_t off);
    void writeI2c(std::uint32_t off, std::uint8_t v);
    std::uint32_t busClocksPerMidiByte() const;

    std::function<std::uint64_t()> busNow_;
    std::array<std::uint8_t, 0x1000> r_{};  // registers without a model: read back what was written
    std::array<Timer, 2> timer_{};
    std::uint64_t timerChecked_[2] = {0, 0};
    std::array<Uart, 2> uart_{};
    std::uint8_t externalIrqs_ = 0;  // bit n: IRQn asserted

    // MIDI bytes waiting to be shifted in, and when the next one completes
    std::deque<std::uint8_t> midiPending_;
    std::uint64_t midiNext_ = 0;
    std::uint64_t uartOverruns_ = 0;

    // I2C master: every addressed slave acknowledges; reads return 0x80 (the OS servoes DSP timer 0 until a
    // histogram of these ADC readings peaks at 0x80, 0x30055F28 [C]; a constant 0x80 satisfies it: stub)
    std::uint8_t i2cMsr_ = 0x81, i2cMcr_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// CS2: 8 MB, 16-bit AMD-style flash with CFI and unlock bypass (§3.6.2). The OS identifies it by CFI and accepts
// command set 2 (AMD, 128 sectors of 64 KB). Erased at power-on unless an image is given.
class Flash {
public:
    static constexpr std::uint32_t Size = 0x800000;
    Flash();
    std::uint32_t read(std::uint32_t off, int size);
    void write(std::uint32_t off, int size, std::uint32_t value);
    std::vector<std::uint8_t>& data() { return mem_; }
    std::uint64_t programs() const { return programs_; }
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

private:
    enum class Mode { Read, Cfi, Id, Status };
    enum class State { Idle, Unlock1, Unlock2, Program, Erase1, Erase2, Erase3, BypassErase, BypassExit };
    void erase(std::uint32_t off, bool chip);

    std::vector<std::uint8_t> mem_;
    Mode mode_ = Mode::Read;
    State state_ = State::Idle;
    bool bypass_ = false;
    std::uint64_t programs_ = 0;
    bool dirty_ = false;
};

// ---------------------------------------------------------------------------------------------------------------
// CS3: the Philips ISP1181 USB device controller (command port +0x10, data port +0), on IRQ3, as the OS drives it
// [C] (handler 0x30053C38). The host side (UsbHost in machine.cpp) queues OUT packets and takes IN packets.
class Isp1181 {
public:
    std::uint32_t read(std::uint32_t off, int size);
    void write(std::uint32_t off, int size, std::uint32_t value);
    bool irq() const { return (intreg_ & inten_) != 0; }

    // host side
    void busReset() { intreg_ |= 1; }
    void out(int ep, const std::uint8_t* data, std::size_t n, bool setup = false);
    // An IN token on bulk-IN: the oldest validated packet (then its completion interrupt), or false.
    bool pollBulkIn(std::vector<std::uint8_t>& packet);
    // Validated interrupt-IN (and other non-bulk) packets, oldest first.
    std::deque<std::pair<int, std::vector<std::uint8_t>>>& inPackets() { return inPackets_; }
    // A host starts polling: IN buffers validated so far are taken.
    void attach();

private:
    void command(std::uint8_t c);
    void data(std::uint8_t b);
    void load(int i);
    std::vector<std::uint8_t> takeInBuffer(int i);

    std::uint8_t cmd_ = 0;
    bool haveCmd_ = false;
    std::vector<std::uint8_t> wbuf_;
    std::deque<std::uint8_t> rbuf_;
    std::uint32_t intreg_ = 0, inten_ = 0;
    std::array<std::uint8_t, 16> epcfg_{};
    struct Packet { std::vector<std::uint8_t> data; bool setup = false; };
    std::array<std::deque<Packet>, 16> outq_;
    std::array<bool, 16> outFull_{};
    std::array<Packet, 16> outbuf_;
    std::array<std::vector<std::uint8_t>, 16> inbuf_;
    std::deque<std::vector<std::uint8_t>> bulkIn_;
    std::deque<std::pair<int, std::vector<std::uint8_t>>> inPackets_;
    std::array<std::deque<std::vector<std::uint8_t>>, 16> held_;
    bool attached_ = false;
};

} // namespace g2emu
