// A Nord Modular G2 in software: the user's own firmware (OS 1.62) on an emulated ColdFire (Gearmulator's ColdFire
// core) with the board's hardware, driving four emulated DSP56367s (Gearmulator's dsp56300, JIT) through their host
// ports, their serial audio links between them, the USB chip our protocol client talks to, and MIDI IN on the
// ColdFire's UART (re/notes/g2-hardware-and-emulation.md §3.9).
//
// Time: one master clock, the DSPs' frame clock. A frame is one 96 kHz sample, 1536 DSP clocks [C]; per frame the
// ColdFire runs 1687.5 of its cycles (162 MHz) and its timers 562.5 bus clocks (54 MHz) (§3.9.2).
//
// Threads: by default everything runs on the thread that calls run() (deterministic); Options::threads = N moves the
// DSPs to N threads of their own. The USB and MIDI queues may be fed from other threads.
#pragma once

#include "g2emu/firmware.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace g2emu {

class Machine {
public:
    static constexpr std::uint32_t FrameRate = 96000;  // as the OS's pitch tables assume (§3.7.1)

    struct Options {
        bool jit = true;               // the DSPs on dsp56300's JIT (the interpreter cannot be stepped: debugging)
        bool idleSkip = true;          // skip the DSPs' idle background loop (no state changes)
        bool pollSkip = true;          // skip the ColdFire's loops that wait on a DSP's host port
        double cfHz = 162e6;           // ColdFire core clock: cycles of the core's (V2) timing per second
        std::uint32_t ringPrefill = 16;  // frames of head start on the ESAI_1 line that closes the ring (A3 -> A6)
        std::uint32_t chainPrefill = 2;  // the same on each hop of the chain (absorbs the JIT's overshoot)
        std::uint32_t quantum = 1536;  // DSP clocks the ColdFire runs ahead of the DSPs at most (1 frame)
        // 0: everything on the caller's thread, deterministic. 1..4: the DSPs on that many threads of their own (each
        // a stretch of the chain), running alongside the ColdFire at most `skew` DSP clocks apart (not deterministic:
        // when a host-port access meets the DSPs depends on the threads' timing, as on the board). 2..4 are
        // experimental: the dsp56300 JIT sometimes recursed without end while the OS uploaded a patch (§3.9.6).
        // The caller's thread needs a big stack (8 MB or more): the JIT compiles there in single-thread mode.
        int threads = 0;
        std::uint32_t skew = 2 * 1536;
        // threads > 0: a status read of a DSP's host port waits (wall time) until that DSP has reached the ColdFire's
        // time. Off: it cost 15-25 % of speed and no failure needed it once the host port was ordered (§3.9.6)
        bool causalReads = false;
        double usbAfter = 2.0;         // plugUsb() takes effect once the OS has run this long (seconds): it boots in 1.6
        bool trace = false;            // log unusual events (unmapped accesses, exceptions) to stderr
    };

    struct Stats {
        std::uint64_t frames = 0;              // DAC frames produced
        std::uint64_t cfInstructions = 0, cfCycles = 0, cfSkipped = 0, cfPollSkipped = 0;
        std::uint64_t dspExecuted[4] = {}, dspSkipped[4] = {};
        std::uint64_t linkUnderruns = 0, linkOverruns = 0;
        std::uint64_t unmapped = 0, exceptions = 0, midiOverruns = 0;
        int dspsWild = 0;  // DSPs that jumped into P memory without code (stopped there: the machine needs a restart)
        std::uint32_t cfPc = 0;
        std::uint32_t dspPc[4] = {};
        std::uint64_t hostCommands = 0, hostReads = 0, dspSyncs = 0;
        // threads > 0: each DSP thread's CPU time (as of its last wait) and the time it spent waiting (spinning or
        // yielding: the waits count as CPU time too)
        std::uint64_t dspThreadCpuNs[4] = {}, dspThreadWaitNs[4] = {};
    };

    explicit Machine(const Firmware& firmware);
    Machine(const Firmware& firmware, Options options);
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // Runs `frames` frames. The DACs' output, 4 words per frame as 24-bit words scaled to [-1, 1), is appended to
    // `out` when it is not null. The words are in the DACs' order: DAC 1 L, DAC 2 L, DAC 1 R, DAC 2 R, i.e. outputs
    // 1, 3, 2, 4.
    void run(std::uint32_t frames, std::vector<float>* out = nullptr);
    std::uint64_t frame() const;
    double seconds() const { return double(frame()) / FrameRate; }

    // ---- USB: our protocol client on the other end of the cable (thread-safe) ----
    // Plug the cable in (at the earliest Options::usbAfter after power-on): bus reset, SET_ADDRESS/SET_CONFIGURATION
    // 5 ms later, the client sees the device 15 ms later.
    void plugUsb();
    bool usbArrived() const;
    // One bulk-OUT transfer (a protocol frame).
    void usbSend(std::span<const std::uint8_t> frame);
    // Device -> host traffic, oldest first: interrupt packets (bulk = false) and the bulk-IN transfers they announce.
    struct UsbIn {
        bool bulk = false;
        std::vector<std::uint8_t> data;
    };
    bool usbTake(UsbIn& in);

    // ---- MIDI IN (UART0, 31250 baud) and OUT (thread-safe) ----
    void midiIn(std::span<const std::uint8_t> bytes);
    std::vector<std::uint8_t> takeMidiOut();

    // ---- flash (patches and settings the OS stores) ----
    std::vector<std::uint8_t>& flash();

    Stats stats() const;
    // A word of DSP n's memory (n: chip select A(3+n); area 0 P, 1 X, 2 Y), for tests and debugging.
    std::uint32_t dspMemory(int n, int area, std::uint32_t address) const;
    std::uint32_t cfRead32(std::uint32_t address) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace g2emu
