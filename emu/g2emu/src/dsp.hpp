// One emulated DSP56367 of the G2 (Gearmulator's dsp56300 library) as the machine drives it: its HDI08 host port
// seen from the ColdFire, its two ESAIs linked to the other DSPs, and stepping on the caller's thread, so that the
// machine decides when each DSP runs and for how long (re/notes/g2-hardware-and-emulation.md §3.6.4, §3.7, §3.9).
//
// Threads: the host-port functions run on the ColdFire's thread, runTo() on the DSP's. What the host writes goes
// through one ordered queue to the DSP's thread (pumpHost); the host reads only the library's thread-safe parts of
// the HDI08 (its transmit queue, the host flags HF2/HF3).
//
// Time: a DSP's instruction counter is its clock. The ESAI clock ticks once per slot every 192 counts, 1536 per
// frame (§3.7.1); the machine runs every DSP to the same count, so their frames stay aligned. While a DSP waits in
// its boot ROM, or idles in stage 1's background loop, its counter is moved on without running anything.
#pragma once

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/dspBootCode.h"
#include "dsp56kEmu/esaiclock.h"
#include "dsp56kEmu/hdi08.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace g2emu {

class AdcSource;  // g2emu/machine.hpp
class Dsp;

// Serial line pair between two DSPs: the slots of transmitters txBase, txBase+1 of `up`'s ESAI `upEsai` arrive in
// receivers 0 and 1 of `down`'s ESAI `downEsai`, whole frames at a time (the library's ESAI hands a frame over at
// the end of its last transmit slot and takes one at the start of its first receive slot). A single-producer
// single-consumer queue; the machine runs the DSPs in an order that keeps it filled.
//
// The line has a fixed delay: the receiver takes the frame the sender finished `prefill` frames earlier, by the two
// DSPs' clocks (both on the machine's time). A frame that is not there in time is lost, as on a wire. (Until
// 2026-10-11 the receiver took the frames in their order of arrival: every frame a sender missed while the OS
// reprogrammed it made its line one frame longer for good, and the way from the ADCs to the DACs grew from 50 to
// 150 frames over four patch uploads.)
struct Link {
    static constexpr std::uint32_t Capacity = 256;  // frames
    static constexpr std::uint32_t Slots = 8;
    struct Frame {
        std::uint32_t slots = 0;
        std::uint64_t number = 0;  // the sender's clock at the end of the frame, in frames
        dsp56k::TWord w[Slots][2];
    };

    Dsp* up = nullptr;
    int upEsai = 0, txBase = 0;
    Dsp* down = nullptr;
    int downEsai = 0;
    std::uint32_t prefill = 0;  // the line's delay in frames (the receiver's head start over the sender)

    std::array<Frame, Capacity> ring{};
    std::atomic<std::uint32_t> head{0}, tail{0};
    std::atomic<bool> active{false};

    // statistics
    std::atomic<std::uint64_t> sent{0}, received{0}, dropped{0}, late{0}, underruns{0}, overruns{0};

    std::uint32_t size() const { return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire); }
};

class Dsp {
public:
    static constexpr std::uint32_t CyclesPerSlot = 192;   // 1536 DSP clocks per 8-slot frame [C]
    static constexpr std::uint32_t CyclesPerFrame = 1536;

    struct Options {
        bool jit = true;
        bool idleSkip = true;
        std::uint32_t hostLagFrames = 0;  // for tests: see Machine::Options
    };

    // Stage 1's background loop [C]: P:$222..$22A spins while the frame counter X:$43 <= 3.
    struct IdleLoop {
        dsp56k::TWord first = 0x222, last = 0x22A, counter = 0x43, limit = 3;
    };

    // Whether this process may generate code at run time, as the library's JIT does (for the DSPs' interrupts too
    // when they run on the interpreter). Not in a macOS program signed with the hardened runtime and neither the
    // allow-jit nor the allow-unsigned-executable-memory entitlement; a plugin lives by its host's entitlements.
    static bool canGenerateCode();

    Dsp(int index, Options options);
    ~Dsp();
    Dsp(const Dsp&) = delete;
    Dsp& operator=(const Dsp&) = delete;

    int index() const { return index_; }

    // ---- host port (the ColdFire's side, 8-bit registers A0-A2) ----
    std::uint8_t hostRead(int reg);
    void hostWrite(int reg, std::uint8_t value);
    // Whether a read of `reg` could see something new only after the DSP has run: the machine then catches the DSP
    // up to the ColdFire's time first.
    bool hostReadDependsOnDsp(int reg) const { return reg != 3 && reg != 0; }

    // Whether the DSP has yet to take something the host wrote: a word or a flag change it was not given yet, a host
    // command it has not run. (A word it was given and has not read is not pending: it is in HORX, where the host
    // sees it as TRDY = 0.) The host's thread.
    bool hostPending() const
    {
        return hostHead_.load(std::memory_order_relaxed) != hostTail_.load(std::memory_order_acquire) ||
               commandsOutstanding_.load(std::memory_order_acquire) != 0;
    }

    // ---- running ----
    // Runs until the instruction counter reaches `target` (or a little beyond: the JIT runs whole blocks).
    void runTo(std::uint64_t target);
    // Runs on, a few instructions at a time, until nothing of the host's is pending (true) or the counter reaches
    // `limit` (false). The DSP's thread.
    bool takeHost(std::uint64_t limit);
    std::uint64_t clock() const { return dsp_.getInstructionCounter(); }
    bool booting() const { return booting_; }

    // ---- serial audio ----
    dsp56k::Esai& esai(int i) { return i ? periphY_.getEsai() : periphX_.getEsai(); }
    void setLinkIn(int esaiIndex, Link* l) { in_[esaiIndex & 1] = l; }
    void setLinkOut(int esaiIndex, Link* l) { out_[esaiIndex & 1] = l; }
    // ESAI ticks per slot - 1 for transmit / receive of ESAI 0 and ESAI_1 (3: a 2-slot converter side).
    void setDividers(std::uint32_t tx0, std::uint32_t rx0, std::uint32_t tx1, std::uint32_t rx1);
    // An unlinked ESAI transmitter (the DACs on the output DSP): its frames go here, 4 words per frame
    // (slot 0 TX0, slot 0 TX1, slot 1 TX0, slot 1 TX1).
    // `clock` gets the time of the last frame's last slot (this DSP's clock), under the same mutex.
    void setSink(std::vector<std::int32_t>* sink, std::mutex* mutex, std::uint64_t* clock = nullptr)
    {
        sink_ = sink; sinkMutex_ = mutex; sinkClock_ = clock;
    }
    // An unlinked ESAI receiver (the ADCs on the first DSP): each of its frames is asked from `source`, 4 words in
    // the sink's order, with the time of the frame's first slot (this DSP's clock). Without one: silence.
    void setSource(AdcSource* source) { source_.store(source, std::memory_order_release); }

    // ---- statistics and debugging ----
    // The two sides of the host port out of step (any thread): the host read the receive registers while the DSP
    // had sent nothing (it gets the word before again), or the DSP read HORX while the host had sent nothing (it
    // gets 0; the boot ROM's emulation, which asks before it looks, is not counted). Neither happens while all is
    // well: the OS's $AE, once taken for a ping that reads without a word (§3.9.4), fetches a word the OS knows is
    // there. The second is how a DSP came to write at P:0 with DSP threads (§3.9.6).
    std::uint64_t hostEmptyReads() const { return hostEmptyReads_.load(std::memory_order_relaxed); }
    std::uint64_t dspEmptyReads() const { return dspEmptyReads_.load(std::memory_order_relaxed); }
    // The DSP the calling thread runs now (inside runTo()), for the library's log, which does not say whose it is.
    static Dsp* running();
    // The library logged an empty read of HORX / something that should not happen (an undefined opcode): the
    // DSP's thread. `what` goes to stderr with the last host events the DSP took (once per DSP and kind).
    void onEmptyRead();
    void report(const char* what);
    std::uint64_t txFrames(int esaiIndex) const { return txFrames_[esaiIndex & 1]; }
    std::uint64_t rxFrames(int esaiIndex) const { return rxFrames_[esaiIndex & 1]; }
    std::uint64_t skipped() const { return skipped_; }
    bool wild() const { return wild_; }  // the DSP has run into P memory without code
    std::uint64_t executed() const { return clock() - skipped_; }
    std::uint32_t pc() const { return dsp_.getPC().toWord(); }
    std::uint32_t memRead(dsp56k::EMemArea area, std::uint32_t addr) const { return mem_.get(area, addr); }
    dsp56k::DSP& core() { return dsp_; }

private:
    void readRx(int i, dsp56k::Audio::RxFrame& f);
    void writeTx(int i, const dsp56k::Audio::TxFrame& f);
    std::uint32_t onPeripherals();
    void feedBootRom();
    struct HostEvent {
        enum Kind : std::uint8_t { Data, Command, Flags } kind = Data;
        dsp56k::TWord value = 0;
    };
    void pushHost(HostEvent::Kind kind, dsp56k::TWord value);  // the host's thread
    void pumpHost();                                             // the DSP's thread
    std::uint8_t isr();

    int index_;
    Options options_;
    IdleLoop idle_;

    dsp56k::DefaultMemoryValidator validator_;
    // Internal memories, plus the external SRAM that stage 1 maps with AAR0 = $800031 [C]: base $800000, X and Y
    // both enabled with no address bits compared, so X and Y reach the same RAM (bridged from $800000).
    dsp56k::Memory mem_{validator_, 0x080000, 0x840000, 0x800000};
    dsp56k::Peripherals56367 periphY_;
    dsp56k::Peripherals56362 periphX_{&periphY_};
    dsp56k::DSP dsp_{mem_, &periphX_, &periphY_};
    dsp56k::HDI08& hdi_ = periphX_.getHDI08();

    std::unique_ptr<dsp56k::DspBoot> boot_;
    bool booting_ = true;

    // host side registers
    std::uint8_t icr_ = 0, cvr_ = 0, ivr_ = 0x0f;
    std::uint8_t tx_[3] = {0, 0, 0};
    std::uint8_t rx_[3] = {0, 0, 0};
    bool rxLatched_ = false;
    // what the host writes, on its way to the DSP's thread (single producer, single consumer), in order
    // (the host's and the DSP's ends on cache lines of their own: see Machine's DSP threads)
    static constexpr std::uint32_t HostQueueSize = 8192;
    std::array<HostEvent, HostQueueSize> hostQueue_{};
    alignas(128) std::atomic<std::uint32_t> hostHead_{0};  // the host's thread writes
    alignas(128) std::atomic<std::uint32_t> hostTail_{0};  // the DSP's thread writes
    std::atomic<int> commandsOutstanding_{0};  // host commands written and not yet taken by the DSP
    bool awaitingCommand_ = false, awaitingFlags_ = false;  // the DSP's thread
    struct alignas(128) {} endOfHostQueue_;

    bool takingHost_ = false;  // inside takeHost(): the idle loop runs, so that the clock moves no further than it must
    Link* in_[2] = {nullptr, nullptr};
    Link* out_[2] = {nullptr, nullptr};
    std::vector<std::int32_t>* sink_ = nullptr;
    std::mutex* sinkMutex_ = nullptr;
    std::uint64_t* sinkClock_ = nullptr;
    std::atomic<AdcSource*> source_{nullptr};
    std::uint64_t txFrames_[2] = {0, 0}, rxFrames_[2] = {0, 0};

    std::uint64_t skipped_ = 0;
    static constexpr dsp56k::TWord WildPcFrom = 0x4000;  // no code above this
    bool wild_ = false;

    // The last host events the DSP took (its thread), for report()
    struct Traced {
        enum Kind : std::uint8_t { Data, Command, Flags, Taken, FlagsSeen, EmptyRead } kind = Data;
        dsp56k::TWord value = 0;
        std::uint64_t clock = 0;
    };
    void trace(Traced::Kind kind, dsp56k::TWord value) { traced_[tracedCount_++ % traced_.size()] = {kind, value, clock()}; }
    std::array<Traced, 256> traced_{};
    std::uint32_t tracedCount_ = 0;
    dsp56k::TWord lastCommand_ = 0;
    int reports_ = 0;
    std::atomic<std::uint64_t> hostEmptyReads_{0}, dspEmptyReads_{0};
};

} // namespace g2emu
