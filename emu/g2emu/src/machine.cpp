#include "g2emu/machine.hpp"

#include "dsp.hpp"
#include "hw.hpp"
#include "panel.hpp"
#include "thread.hpp"

#include "coldfire/cfCpu.h"
#include "dsp56kBase/logging.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <thread>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace g2emu {

namespace {

constexpr std::uint32_t SdramBase = 0x30000000, SdramSize = 0x400000;
constexpr std::uint32_t SramBase = 0x20000000, SramSize = 0x1000;
constexpr std::uint32_t BootSize = 0x80000;

inline std::uint16_t be16(const std::uint8_t* p) { return std::uint16_t(p[0] << 8 | p[1]); }
inline std::uint32_t be32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }
inline void put16(std::uint8_t* p, std::uint16_t v) { p[0] = std::uint8_t(v >> 8); p[1] = std::uint8_t(v); }
inline void put32(std::uint8_t* p, std::uint32_t v) { p[0] = std::uint8_t(v >> 24); p[1] = std::uint8_t(v >> 16); p[2] = std::uint8_t(v >> 8); p[3] = std::uint8_t(v); }

// The library logs every ESAI control register write and each transmit underrun; drop those and repeats.
void quietLog(const std::string& s)
{
    // What the log says of the DSP that runs on this thread and should not happen goes to that DSP, which reports it
    // with what it took from its host port before (Dsp::report)
    if(auto* dsp = Dsp::running())
    {
        if(s.find("Empty read") != std::string::npos) dsp->onEmptyRead();
        else if(s.find("Undefined opcode") != std::string::npos && s.find("Undefined opcode 000040") == std::string::npos)
            dsp->report("ran an undefined opcode");
    }
    static std::mutex m;
    static std::string last;
    std::lock_guard lock(m);
    if(s.find("Write ESAI") != std::string::npos || s.find("Write Timer") != std::string::npos ||
       s.find("HPCR") != std::string::npos || s.find("underrun") != std::string::npos || s.find("DSP Boot") != std::string::npos ||
       s.find("Clock speed") != std::string::npos ||
       s.find("Empty read") != std::string::npos ||  // counted and reported by the DSP (above)
       s.find("Undefined opcode 000040") != std::string::npos)  // in the OS's DSP code, harmless (an empty vector)
        return;
    if(s == last) return;
    last = s;
    std::fprintf(stderr, "dsp56300: %s\n", s.c_str());
}

// A host port of an absent DSP (A7..A10 without the expansion board; the OS then never selects them, §3.12): INIT
// completes at once, a host command is taken at once, HF0 is echoed as HF2 (as g2hostemu.py's stubs).
struct StubPort {
    std::uint8_t icr = 0, cvr = 0, ivr = 0x0f, hf23 = 0;
    std::uint8_t read(int reg) const
    {
        switch(reg)
        {
        case 0: return icr;
        case 1: return cvr;
        case 2: return std::uint8_t(0x02 | 0x04 | hf23);
        case 3: return ivr;
        default: return 0;
        }
    }
    void write(int reg, std::uint8_t v)
    {
        switch(reg)
        {
        case 0: icr = v & 0x7f; hf23 = (v & 0x08) ? 0x08 : 0; break;
        case 1: cvr = v & 0x7f; break;
        case 3: ivr = v; break;
        default: break;
        }
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------

struct Machine::Impl final : coldfire::Bus {
    Options opt;

    // memories
    std::vector<std::uint8_t> boot = std::vector<std::uint8_t>(BootSize, 0xff);
    std::vector<std::uint8_t> sram = std::vector<std::uint8_t>(SramSize, 0);
    std::vector<std::uint8_t> sdram = std::vector<std::uint8_t>(SdramSize, 0);

    // devices
    coldfire::Cpu cpu{*this};
    Sim sim{[this] { return busNow(); }};
    Flash flashChip;
    Isp1181 usb;
    Panel panelHw;
    // By chip select: A3..A6 the main board's DSPs, A7..A10 the expansion board's (null without it) [§2.6, §3.12]
    std::array<std::unique_ptr<Dsp>, MaxDsps> dsps;
    std::array<StubPort, 4> stubs;
    std::array<std::unique_ptr<Link>, 2 * MaxDsps> links;
    // A6 (clock master, ADCs) -> A5 -> A4 -> A3 (DACs) [§3.7.3]; with the expansion board its four DSPs come between
    // A6 and A5, in the OS's order: A6 -> A10 -> A9 -> A8 -> A7 -> A5 -> A4 -> A3 [§3.12.2]
    const bool expanded;
    const std::vector<int> chain;
    std::vector<std::int32_t> dac;          // the DACs' words, 4 per frame
    std::uint64_t dacSent = 0;              // when the last of them was sent (A3's clock)
    std::uint64_t dacClock = 0;             // the same for the last frame run() handed out
    std::mutex dacMutex;

    // threads = N > 0: N DSP threads, each running a stretch of the chain (in chain order) up to `horizon`; each
    // publishes how far it got in `worker[w].time`, and `dspTime` is the slowest of them
    // (Each group of atomics that one thread writes has a cache line of its own (128 bytes on Apple's cores): on a
    // shared line every store of one thread would evict what the other threads read all the time, the ColdFire's
    // state among it.)
    std::vector<BigStackThread> workers;
    struct alignas(128) Worker {
        std::atomic<std::uint64_t> time{0};
        std::atomic<std::uint64_t> cpuNs{0}, waitNs{0};  // CPU time, time spent waiting
        // The DSP (a bit, by chip select) whose status the ColdFire is about to read while it has yet to take what
        // the ColdFire wrote: the worker runs it on until it has (takeHosts, syncDsps). `asked` numbers the
        // ColdFire's requests; `stuck` is the last one the worker could not serve because its DSPs had reached the
        // horizon.
        std::atomic<std::uint32_t> takeHost{0};
        std::atomic<std::uint64_t> asked{0}, stuck{0};
    };
    std::array<Worker, MaxDsps> worker{};
    std::array<int, MaxDsps> dspWorker{};  // which DSP thread runs DSP n
    struct alignas(128) {
        std::atomic<std::uint64_t> horizon{0};
        std::atomic<bool> quit{false};
    } cf;
    struct alignas(128) {
        std::atomic<std::uint64_t> dspTime{0};
    } dspShared;
    std::atomic<std::uint64_t>& horizon = cf.horizon;
    std::atomic<bool>& quit = cf.quit;
    std::atomic<std::uint64_t>& dspTime = dspShared.dspTime;
    Doorbell progress;  // rung whenever horizon, a worker's time or quit changes
    struct alignas(128) {} endOfShared;

    // time
    std::uint64_t t = 0;         // master time in DSP clocks
    double cfPerDsp = 1.0;       // ColdFire cycles per DSP clock
    double busPerCf = 1.0 / 3;   // bus clocks per ColdFire cycle
    std::uint64_t cfSkipped = 0; // ColdFire cycles skipped (stopped)
    std::uint64_t nextSimEvent = 0;
    bool irqDirty = true;

    // USB host side
    struct UsbHostState {
        bool plugged = false, arrived = false, setupSent = false;
        std::uint64_t plugFrame = 0;
        std::vector<std::uint8_t> bulk;
        std::size_t expect = 0;
        std::deque<std::vector<std::uint8_t>> ints;
    } host;
    mutable std::mutex ioMutex;            // guards the queues below
    std::deque<std::vector<std::uint8_t>> usbOut;  // from the client
    std::deque<UsbIn> usbIn;               // to the client
    bool plugRequest = false;
    // MIDI OUT: the last midiOutKept bytes the OS sent that nobody took yet (a ring: nothing is allocated as it runs)
    std::vector<MidiOut> midiOutRing;
    std::uint64_t midiOutHead = 0, midiOutTail = 0;
    struct MidiIn {
        std::vector<std::uint8_t> bytes;
        std::uint64_t frame = 0;  // 0: as soon as possible
    };
    std::vector<MidiIn> midiInQueue;

    // front panel: the UI's inputs (any thread) are applied at the start of a quantum; a snapshot of what the panel
    // shows is published at most once per emulated millisecond, when something changed
    struct PanelInput {
        enum Kind { Button, Encoder, Adc, Key, Sustain } kind = Button;
        int index = 0, value = 0, extra = 0;
    };
    mutable std::mutex panelMutex;
    std::vector<PanelInput> panelInputs;
    std::atomic<bool> panelInputsPending{false};
    PanelState panelPublished;
    std::array<int, 8> knobDirection{};  // the last direction each knob was turned (panelEncoder)
    std::uint64_t panelChanges = ~0ull, panelPublishAt = 0;

    static bool samePanel(const PanelState& a, const PanelState& b)
    {
        if(a.rawLeds != b.rawLeds || a.model != b.model) return false;
        for(std::size_t i = 0; i < a.displays.size(); ++i)
        {
            const auto &x = a.displays[i], &y = b.displays[i];
            if(x.chars != y.chars || x.cgram != y.cgram || x.on != y.on || x.cursor != y.cursor || x.cursorLine != y.cursorLine ||
               x.cursorBlink != y.cursorBlink)
                return false;
        }
        return true;
    }
    // The parallel port's inputs: the panel's (keyboard rows, sustain pedal) and bit 9, which the expansion board
    // pulls low: the OS reads it once at power-on and takes eight DSPs instead of four [C 0x30037F64, §3.12.1].
    // (Without the board the bit reads high, as the OS's own write to PADAT left it before it was modelled.)
    static constexpr std::uint16_t GpioExpansionAbsent = 0x0200;
    void gpioChanged()
    {
        sim.setGpioInputs(std::uint16_t(panelHw.gpioMask() | GpioExpansionAbsent),
                          std::uint16_t(panelHw.gpioInputs() | (expanded ? 0 : GpioExpansionAbsent)));
    }
    void panelStep()
    {
        if(panelInputsPending.load(std::memory_order_acquire))
        {
            std::vector<PanelInput> in;
            {
                std::lock_guard lock(panelMutex);
                in.swap(panelInputs);
                panelInputsPending.store(false, std::memory_order_relaxed);
            }
            for(const auto& e : in)
                switch(e.kind)
                {
                case PanelInput::Button: panelHw.button(e.index, e.value != 0); break;
                case PanelInput::Encoder: panelHw.turn(e.index, e.value); break;
                case PanelInput::Adc: sim.setPanelAdc(std::size_t(e.index), std::uint8_t(e.value)); break;
                case PanelInput::Key: panelHw.key(e.index, e.value != 0, std::uint64_t(e.extra), busNow()); break;
                case PanelInput::Sustain:
                    panelHw.sustain(true, e.value != 0);
                    gpioChanged();
                    break;
                }
        }
        if(panelHw.advance(busNow())) gpioChanged();
        if(panelHw.changes() != panelChanges && t >= panelPublishAt)
        {
            PanelState s;
            panelHw.snapshot(s);
            panelChanges = panelHw.changes();
            std::lock_guard lock(panelMutex);
            s.generation = panelPublished.generation;
            if(!samePanel(s, panelPublished))
            {
                s.generation = panelPublished.generation + 1;
                panelPublished = s;
            }
            panelPublishAt = t + std::uint64_t(FrameRate / 1000) * Dsp::CyclesPerFrame;
        }
    }
    std::string panelDebug() const
    {
        const auto& tr = panelHw.trace();
        char b[512];
        std::snprintf(b, sizeof b,
                      "cs5 writes %llu %llu %llu %llu %llu %llu %llu %llu, reads %llu (selects %llu %llu %llu %llu %llu %llu %llu %llu, none %llu); "
                      "cs4 writes %llu; lcd writes %llu %llu %llu %llu %llu",
                      (unsigned long long)tr.cs5Writes[0], (unsigned long long)tr.cs5Writes[1], (unsigned long long)tr.cs5Writes[2],
                      (unsigned long long)tr.cs5Writes[3], (unsigned long long)tr.cs5Writes[4], (unsigned long long)tr.cs5Writes[5],
                      (unsigned long long)tr.cs5Writes[6], (unsigned long long)tr.cs5Writes[7], (unsigned long long)tr.cs5Reads,
                      (unsigned long long)tr.selectReads[0], (unsigned long long)tr.selectReads[1], (unsigned long long)tr.selectReads[2],
                      (unsigned long long)tr.selectReads[3], (unsigned long long)tr.selectReads[4], (unsigned long long)tr.selectReads[5],
                      (unsigned long long)tr.selectReads[6], (unsigned long long)tr.selectReads[7], (unsigned long long)tr.selectReads[8],
                      (unsigned long long)tr.cs4Writes, (unsigned long long)panelHw.lcds()[0].writes,
                      (unsigned long long)panelHw.lcds()[1].writes, (unsigned long long)panelHw.lcds()[2].writes,
                      (unsigned long long)panelHw.lcds()[3].writes, (unsigned long long)panelHw.lcds()[4].writes);
        std::string r = b;
        for(int n = 0; n < 256; ++n)
            if(panelHw.ledHistory(n))
            {
                std::snprintf(b, sizeof b, " led%d=%08x/%llu", n, panelHw.ledHistory(n), (unsigned long long)panelHw.ledStrobes(n));
                r += b;
            }
        return r;
    }
    void panelInput(PanelInput e)
    {
        std::lock_guard lock(panelMutex);
        panelInputs.push_back(e);
        panelInputsPending.store(true, std::memory_order_release);
    }

    // statistics
    Stats st;
    std::unique_ptr<std::map<std::uint32_t, std::uint64_t>> profile;  // G2EMU_CFPROFILE: ColdFire PCs, every 61st
    // G2EMU_CFPROFILE_EXACT: by instruction (all of them listed) instead of 64-byte blocks (the top 40)
    const bool profileExact = std::getenv("G2EMU_CFPROFILE_EXACT") != nullptr;
    std::uint64_t profileTick = 0, profileFrom = 0;
    std::uint64_t profileIpl[8] = {};
    ~Impl() override
    {
        stopWorker();
        if(timing && opt.threads)
            std::fprintf(stderr, "ColdFire waits: %llu (<1 us %llu, <10 us %llu, <100 us %llu, more %llu)\n", (unsigned long long)cfWaits,
                         (unsigned long long)cfWaitHist[0], (unsigned long long)cfWaitHist[1], (unsigned long long)cfWaitHist[2], (unsigned long long)cfWaitHist[3]);
        if(timing)
            std::fprintf(stderr, "time: cf %.2f s (of which DSP catch-up %.2f s), DSPs at the end of each slice %.2f s; ColdFire alone %.2f s, %.1f M instructions/s\n",
                         cfNs * 1e-9, syncNs * 1e-9, dspNs * 1e-9, (cfNs - syncNs - dspNs) * 1e-9,
                         double(cpu.getInstructionCount()) / double(std::max<std::uint64_t>(1, cfNs - syncNs - dspNs)) * 1e3);
        if(!profile) return;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> v;
        std::uint64_t total = 0;
        for(auto [pc, n] : *profile) { v.emplace_back(n, pc); total += n; }
        std::sort(v.rbegin(), v.rend());
        std::fprintf(stderr, "ColdFire PC profile (%llu samples):\n", (unsigned long long)total);
        std::fprintf(stderr, "  by interrupt mask: ");
        for(int i = 0; i < 8; ++i) std::fprintf(stderr, "%d: %.1f%% ", i, 100.0 * double(profileIpl[i]) / double(total));
        std::fprintf(stderr, "\n");
        for(std::size_t i = 0; i < (profileExact ? v.size() : std::min<std::size_t>(40, v.size())); ++i)
            std::fprintf(stderr, "  %08x %5.2f%%\n", v[i].second, 100.0 * double(v[i].first) / double(total));
    }

    Impl(const Firmware& fw, Options o)
        : opt(o), expanded(Machine::hasExpansion(o)),
          chain(expanded ? std::vector<int>{3, 7, 6, 5, 4, 2, 1, 0} : std::vector<int>{3, 2, 1, 0})
    {
        if(opt.threads < 0) opt.threads = Machine::autoThreads(int(chain.size()));
        midiOutRing.resize(std::max<std::uint32_t>(1, opt.midiOutKept));
        Logging::setLogFunc(&quietLog);
        cfPerDsp = opt.cfHz / (double(FrameRate) * Dsp::CyclesPerFrame);
        busPerCf = double(Sim::BusHz) / opt.cfHz;
        // The panel's analogue controls at rest (stream positions, §3.10): G.Wheel 2, the master level, the control
        // pedal, the aftertouch sensor (0xFF: no pressure), the pitch stick (centre; the OS's boot calibration needs
        // 0x80 here), the mod wheel, G.Wheel 1
        const std::uint8_t rest[7] = {0, std::uint8_t(std::lround(std::clamp(opt.masterVolume, 0.0, 1.0) * 255)), 0, 0xFF, 0x80, 0, 0};
        for(std::size_t i = 0; i < 7; ++i) sim.setPanelAdc(i, rest[i]);
        panelHw.setModel(opt.model == PanelModel::G2X ? 3 : opt.model == PanelModel::G2Engine ? 2 : 0);
        gpioChanged();
        load(fw);
        // Without this the library's JIT leaves its entry points null and the first DSP step jumps to address 0: in a
        // plugin that takes the host down with it.
        if(!Dsp::canGenerateCode())
            throw std::runtime_error("this program may not generate code at run time, which the emulated DSPs need"
#ifdef __APPLE__
                                     " (macOS: the app, or the plugin's host, is signed with the hardened runtime and"
                                     " without the allow-jit or allow-unsigned-executable-memory entitlement)"
#endif
            );
        Dsp::Options dopt;
        dopt.idleSkip = opt.idleSkip;
        dopt.hostLagFrames = opt.threads ? opt.hostLagFrames : 0;
        for(int n : chain) dsps[std::size_t(n)] = std::make_unique<Dsp>(n, dopt);
        wire();
        cpu.setFastMemory(sdram.data(), SdramBase, SdramSize);
        cpu.setUnimplementedCallback([this](std::uint32_t pc, std::uint16_t op) {
            ++st.exceptions;
            if(opt.trace) std::fprintf(stderr, "g2emu: unimplemented opcode %04x at %08x\n", op, pc);
        });
        // as the boot loader leaves it when it jumps to CODE (g2hostemu.py starts here too)
        cpu.setSR(0x2700);
        cpu.setA(7, 0x30400000);
        cpu.setPC(fw.code()->address);
        if(const char* p = std::getenv("G2EMU_CFPROFILE"))
        {
            profile = std::make_unique<std::map<std::uint32_t, std::uint64_t>>();
            profileFrom = std::uint64_t(std::atof(p) * FrameRate * Dsp::CyclesPerFrame);
        }
    }

    void load(const Firmware& fw)
    {
        std::copy_n(fw.bootLoader.begin(), std::min<std::size_t>(fw.bootLoader.size(), BootSize), boot.begin());
        for(const auto& s : fw.sections)
        {
            if(s.address >= SdramBase && s.address + s.data.size() <= SdramBase + SdramSize)
                std::copy(s.data.begin(), s.data.end(), sdram.begin() + (s.address - SdramBase));
            else if(s.address >= SramBase && s.address + s.data.size() <= SramBase + SramSize)
                std::copy(s.data.begin(), s.data.end(), sram.begin() + (s.address - SramBase));
            else
                throw std::runtime_error("g2emu: section " + s.name + " outside the memory map");
        }
    }

    // Serial audio (§3.7): the chain on ESAI (TX0/TX1 -> RX0/RX1), ending in A3's DACs, and the ring on ESAI_1
    // (TX2/TX3 -> RX0/RX1) back to A6. The converter sides have 2 slots per frame: 4 clock ticks per slot.
    void wire()
    {
        const int first = chain.front(), last = chain.back();
        for(int n : chain)
            dsps[std::size_t(n)]->setDividers(n == last ? 3 : 0, n == first ? 3 : 0, 0, 0);
        std::size_t li = 0;
        auto link = [&](int up, int upEsai, int txBase, int down, int downEsai, std::uint32_t prefill) {
            auto l = std::make_unique<Link>();
            l->up = dsps[std::size_t(up)].get(); l->upEsai = upEsai; l->txBase = txBase;
            l->down = dsps[std::size_t(down)].get(); l->downEsai = downEsai;
            l->prefill = prefill;
            l->up->setLinkOut(upEsai, l.get());
            l->down->setLinkIn(downEsai, l.get());
            links[li++] = std::move(l);
        };
        for(std::size_t k = 0; k + 1 < chain.size(); ++k)
        {
            link(chain[k], 0, 0, chain[k + 1], 0, opt.chainPrefill);
            link(chain[k], 1, 2, chain[k + 1], 1, opt.chainPrefill);
        }
        link(last, 1, 2, first, 1, opt.ringPrefill);
        dsps[std::size_t(last)]->setSink(&dac, &dacMutex, &dacSent);
    }

    // ---- time ----
    std::uint64_t cfNow() const { return cpu.getCycles() + cfSkipped; }
    // The bus clock (54 MHz [C]: the OS's MIDI divider) runs with the master clock, whatever the core's speed.
    std::uint64_t busNow() const { return std::uint64_t(double(cfNow()) * busPerCf); }
    std::uint64_t dspTimeOfCf(std::uint64_t cf) const { return std::uint64_t(double(cf) / cfPerDsp); }
    std::uint64_t cfTimeOfDsp(std::uint64_t d) const { return std::uint64_t(double(d) * cfPerDsp); }

    // Catches the DSPs up to the ColdFire's time, in chain order up to DSP n (each needs its upstream's frames),
    // before the ColdFire reads register `reg` of DSP n's host port or writes to it (reg < 0).
    // How many frames beyond dspLead the ColdFire lets the DSPs run so that one of them takes what the host wrote
    // (with the frame takeHosts() adds and the JIT's overshoot: under the 8 frames of the Runner's input lead)
    static constexpr std::uint64_t HorizonStretch = 5;
    void syncDsps(int n, int reg = -1)
    {
        if(opt.threads)
        {
            // A read of the status register finds the DSP past everything the ColdFire wrote before it: its words
            // in HORX, its host commands run, its flag changes seen. On the board the two chips run at once and the
            // DSP takes a command as soon as it may (at the latest when the frame program it is in ends); one thread
            // runs the DSP up to the ColdFire's time before the read. A DSP thread may be frames behind, or held at
            // the horizon with the writes still in its queue, and the OS cannot tell: an interrupt that wants the
            // host ports saves what the interrupted code left in them, and takes TRDY = 0 for a word the DSP has yet
            // to read [C 0x30050488]. When the command that reads that word was still on its way, the OS fetched the
            // word with $AE after the command had taken it, got nothing, and later wrote that nothing back: one word
            // too many, every word after it went to the command after its own, and the DSP wrote its program's
            // addresses at P:0 and strayed (re/notes §3.9.6). So the DSP's worker runs it on until it has taken them,
            // past the horizon if it must.
            if(reg == 2 && !workers.empty() && dsps[std::size_t(n)]->hostPending())
            {
                auto& d = *dsps[std::size_t(n)];
                ++st.statusBehind;
                if(!opt.hostTakenOnRead) ++st.statusStale;
                else
                {
                    auto& w = worker[std::size_t(dspWorker[std::size_t(n)])];
                    const auto w0 = clockNs();
                    w.takeHost.store(1u << n, std::memory_order_release);
                    for(;;)
                    {
                        const auto ask = w.asked.load(std::memory_order_relaxed) + 1;
                        w.asked.store(ask, std::memory_order_release);
                        progress.ring();
                        progress.wait([&] {
                            return !d.hostPending() || w.stuck.load(std::memory_order_acquire) == ask || quit.load(std::memory_order_relaxed);
                        });
                        if(!d.hostPending() || quit.load(std::memory_order_relaxed)) break;
                        // The DSPs have reached the horizon and this one is not there yet (in its frame program, or
                        // its host port is paced behind a flag change it has yet to see): they may all run a frame
                        // further, a few times. After that the status is read as it is (a DSP that no longer takes
                        // anything: it strayed, or its program does not read the host port).
                        if(horizon.load(std::memory_order_relaxed) >= t + opt.dspLead + HorizonStretch * Dsp::CyclesPerFrame)
                        {
                            ++st.statusStale;
                            break;
                        }
                        horizon.fetch_add(Dsp::CyclesPerFrame, std::memory_order_release);
                    }
                    w.takeHost.store(0, std::memory_order_release);
                    st.cfWaitNs += clockNs() - w0;
                }
            }
            if(!workers.empty() && opt.causalReads)
            {
                const auto target = std::min(dspTimeOfCf(cfNow()), horizon.load(std::memory_order_acquire));
                auto& wt = worker[std::size_t(dspWorker[std::size_t(n)])].time;
                if(wt.load(std::memory_order_acquire) < target)
                {
                    ++st.dspSyncs;
                    progress.wait([&] { return wt.load(std::memory_order_acquire) >= target || quit.load(std::memory_order_relaxed); });
                }
            }
            return;
        }
        const auto target = dspTimeOfCf(cfNow());
        ++st.dspSyncs;
        const auto t0 = timing ? clockNs() : 0;
        std::uint64_t up = target;  // the whole frames the DSP before DSP n has run
        for(int k : chain)
        {
            auto& d = *dsps[std::size_t(k)];
            d.runTo(target);
            if(k == n)
            {
                // The same rule on one thread (above): the DSP is at the ColdFire's time, but what was written just
                // before may still be on its way, because the DSP had already run past the time of the writes (the
                // JIT runs whole blocks, an idle DSP skips to its next serial slot) or is in its frame program, which
                // takes no host command. A MIDI clock made that common: the OS's interrupt for each clock byte saves
                // the host ports, and after a few seconds of clock a DSP went out of step and the notes went silent.
                if(reg == 2 && d.hostPending())
                {
                    ++st.statusBehind;
                    bool taken = opt.hostTakenOnRead && d.takeHost(std::max(up, target) + Dsp::CyclesPerFrame);
                    // not within a frame past the DSP before it: the DSPs up to it run on together, as with threads
                    for(auto to = target; opt.hostTakenOnRead && !taken && to < target + HorizonStretch * Dsp::CyclesPerFrame;)
                    {
                        to += 16;
                        for(int u : chain)
                        {
                            dsps[std::size_t(u)]->runTo(to);
                            if(u == n) break;
                        }
                        taken = !d.hostPending();
                    }
                    if(!taken) ++st.statusStale;
                }
                break;
            }
            up = d.clock() / Dsp::CyclesPerFrame * Dsp::CyclesPerFrame;
        }
        if(timing) syncNs += clockNs() - t0;
    }
    static std::uint64_t threadCpuNs()
    {
#ifdef _WIN32
        FILETIME c, e, k, u;
        if(!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
        const auto t = [](const FILETIME& f) { return (std::uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) * 100; };
        return t(k) + t(u);
#else
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return std::uint64_t(ts.tv_sec) * 1000000000ull + std::uint64_t(ts.tv_nsec);
#endif
    }
    static std::uint64_t clockNs()
    {
        return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    bool timing = std::getenv("G2EMU_TIMING") != nullptr;
    std::vector<std::uint32_t> pcWatch = [] {
        std::vector<std::uint32_t> v;
        if(const char* p = std::getenv("G2EMU_PCWATCH"))
            for(const char* q = p; *q;)
            {
                char* e = nullptr;
                v.push_back(std::uint32_t(std::strtoul(q, &e, 16)));
                q = *e ? e + 1 : e;
            }
        return v;
    }();
    std::uint64_t syncNs = 0, cfNs = 0, dspNs = 0, cfWaits = 0, cfWaitHist[4] = {};

    // ---- the ColdFire bus ----
    std::uint32_t mmioRead(std::uint32_t a, int size)
    {
        switch(a >> 24)
        {
        case 0x10:
            if(a < 0x10001000) { irqDirty = true; cpu.m_g2WaitCycles += 6; return sim.read(a & 0xfff, size); }
            break;
        case 0x11:
            if(a < 0x11001000)
            {
                std::uint32_t v = 0;
                for(int k = 0; k < size; ++k) v = v << 8 | hostRead8((a + std::uint32_t(k)) & 0xfff);
                return v;
            }
            break;
        case 0x12: cpu.m_g2WaitCycles += 15; return flashChip.read(a & (Flash::Size - 1), size);
        case 0x13:
            if(a < 0x13010000)
            {
                cpu.m_g2WaitCycles += 12;
                const auto v = usb.read(a & 0xffff, size);
                usbIrqChanged();
                return v;
            }
            break;
        case 0x15:
            if((a & 0xffffff) < 0x10000)
            {
                cpu.m_g2WaitCycles += 12;
                std::uint32_t v = 0;
                for(int k = 0; k < size; ++k) v = v << 8 | panelHw.readCs5((a + std::uint32_t(k)) & 7);
                return v;
            }
            break;
        case 0x14: case 0x16: case 0x17:
            if((a & 0xffffff) < 0x10000) { cpu.m_g2WaitCycles += 12; return 0; }  // write-only latches (CS4), nothing (CS6/7)
            break;
        default: break;
        }
        unmapped(a, size, false, 0);
        return 0;
    }

    void mmioWrite(std::uint32_t a, int size, std::uint32_t v)
    {
        switch(a >> 24)
        {
        case 0x10:
            if(a < 0x10001000)
            {
                cpu.m_g2WaitCycles += 6;
                sim.write(a & 0xfff, size, v);
                irqDirty = true;
                nextSimEvent = 0;
                return;
            }
            break;
        case 0x11:
            if(a < 0x11001000)
            {
                for(int k = 0; k < size; ++k) hostWrite8((a + std::uint32_t(k)) & 0xfff, std::uint8_t(v >> (8 * (size - 1 - k))));
                return;
            }
            break;
        case 0x12: cpu.m_g2WaitCycles += 15; flashChip.write(a & (Flash::Size - 1), size, v); return;
        case 0x13:
            if(a < 0x13010000) { cpu.m_g2WaitCycles += 12; usb.write(a & 0xffff, size, v); usbIrqChanged(); return; }
            break;
        case 0x14:
            if((a & 0xffffff) < 0x10000)
            {
                // the keyboard matrix's column latch (16 bits) [C 0x30029d78]
                cpu.m_g2WaitCycles += 12;
                panelHw.writeCs4(std::uint16_t(v));
                gpioChanged();
                return;
            }
            break;
        case 0x15:
            if((a & 0xffffff) < 0x10000)
            {
                cpu.m_g2WaitCycles += 12;
                for(int k = 0; k < size; ++k) panelHw.writeCs5((a + std::uint32_t(k)) & 7, std::uint8_t(v >> (8 * (size - 1 - k))));
                return;
            }
            break;
        case 0x16: case 0x17:
            if((a & 0xffffff) < 0x10000) { cpu.m_g2WaitCycles += 12; return; }
            break;
        default: break;
        }
        unmapped(a, size, true, v);
    }

    void unmapped(std::uint32_t a, int size, bool write, std::uint32_t v)
    {
        if(++st.unmapped <= 20 && opt.trace)
            std::fprintf(stderr, "g2emu: unmapped %s %08x/%d = %x at pc %08x\n", write ? "write" : "read", a, size, v,
                         cpu.getInstructionPC());
    }

    void usbIrqChanged()
    {
        sim.setExternalIrq(3, usb.irq());
        irqDirty = true;
    }

    // CS1, 0x11000000-0x110007FF: the DSP host ports. Address lines A3..A10 are one-hot, active-low DSP selects (A3..A6
    // the main board's DSPs, A7..A10 the expansion's), A0-A2 the HDI08 register [C, §2.6]. Several selected: a write
    // goes to all of them, a read ANDs them (inferred). The bus splits word and long accesses into byte cycles.
    std::uint8_t hostRead8(std::uint32_t off)
    {
        cpu.m_g2WaitCycles += 12;
        const unsigned sel = (~off >> 3) & 0xff;
        const int reg = int(off & 7);
        std::uint8_t v = 0xff;
        for(int n = 0; n < 8; ++n)
        {
            if(!(sel & (1u << n))) continue;
            if(auto* d = dsps[std::size_t(n)].get())
            {
                if(d->hostReadDependsOnDsp(reg)) syncDsps(n, reg);
                ++st.hostReads;
                v &= d->hostRead(reg);
            }
            else
                v &= stubs[std::size_t(n - 4)].read(reg);
        }
        return pollSkip(off, sel, reg, v);
    }

    // Poll skipping: the ColdFire reads the same status register of one DSP (CVR: waiting for a host
    // command to be taken; ISR: for data or a host flag) from the same instruction, with nothing else on the host
    // port in between, and finds the same value: it spins until the DSP changes it. Instead of running the loop, the
    // DSPs run ahead until the value changes (at most a frame) and the ColdFire's clock moves on by as much, as if it
    // had spun all that time. Only ColdFire instructions that do nothing but wait are left out.
    struct Poll {
        std::uint32_t pc = 0, off = ~0u;
        std::uint8_t value = 0;
    } poll;
    std::uint8_t pollSkip(std::uint32_t off, unsigned sel, int reg, std::uint8_t v)
    {
        const std::uint32_t pc = cpu.getInstructionPC();
        const bool repeat = pc == poll.pc && off == poll.off && v == poll.value;
        poll = {pc, off, v};
        if(!repeat || !opt.pollSkip || (reg != 1 && reg != 2) || (sel & (sel - 1)) || !(sel & (expanded ? 0xff : 0x0f))) return v;
        if(opt.threads)
        {
            // the DSPs run on their own: the ColdFire's clock moves on a little per repeated poll instead of running
            // the loop (the answer comes as soon as the DSP's thread gets there; the skew bounds how far it may be)
            const std::uint64_t step = 128;
            st.cfPollSkipped += step;
            cfSkipped += step;
            irqDirty = true;
            return v;
        }
        int n = 0;
        while(!(sel & (1u << n))) ++n;
        auto& d = *dsps[std::size_t(n)];
        const std::uint64_t from = dspTimeOfCf(cfNow());
        std::uint64_t to = from;
        std::uint8_t now = v;
        const auto t0 = timing ? clockNs() : 0;
        while(now == v && to < from + Dsp::CyclesPerFrame)
        {
            to += 16;
            for(int k : chain)
            {
                dsps[std::size_t(k)]->runTo(to);
                if(k == n) break;
            }
            now = d.hostRead(reg);
        }
        if(timing) syncNs += clockNs() - t0;
        const auto cfTo = cfTimeOfDsp(to);
        if(cfTo > cfNow())
        {
            st.cfPollSkipped += cfTo - cfNow();
            cfSkipped += cfTo - cfNow();
        }
        irqDirty = true;  // the run loop looks at the clock again
        poll.value = now;
        return now;
    }

    void hostWrite8(std::uint32_t off, std::uint8_t v)
    {
        cpu.m_g2WaitCycles += 12;
        poll.off = ~0u;
        const unsigned sel = (~off >> 3) & 0xff;
        const int reg = int(off & 7);
        for(int n = 0; n < 8; ++n)
        {
            if(!(sel & (1u << n))) continue;
            if(auto* d = dsps[std::size_t(n)].get())
            {
                // single thread: catch the DSPs up first; with threads the writes need no wait (they reach the DSP
                // in order through its host queue)
                if(!opt.threads) syncDsps(n);
                if(reg == 1 && (v & 0x80)) ++st.hostCommands;
                d->hostWrite(reg, v);
            }
            else
                stubs[std::size_t(n - 4)].write(reg, v);
        }
    }

    // coldfire::Bus
    std::uint8_t read8(std::uint32_t a) override
    {
        if(a - SdramBase < SdramSize) return sdram[a - SdramBase];
        if(a - SramBase < SramSize) return sram[a - SramBase];
        if(a < BootSize) return boot[a];
        return std::uint8_t(mmioRead(a, 1));
    }
    std::uint16_t read16(std::uint32_t a) override
    {
        if(a - SdramBase < SdramSize - 1) return be16(&sdram[a - SdramBase]);
        if(a - SramBase < SramSize - 1) return be16(&sram[a - SramBase]);
        if(a < BootSize - 1) return be16(&boot[a]);
        return std::uint16_t(mmioRead(a, 2));
    }
    std::uint32_t read32(std::uint32_t a) override
    {
        if(a - SdramBase < SdramSize - 3)
        {
            // The core fetches exception vectors through the bus (its RAM window is for instructions and data):
            // count the error exceptions (access, address, illegal, divide by zero, privilege, trace, line A/F,
            // format: vectors 2-15) taken through the OS's table at VBR = 0x30000000 [C]
            if(a - SdramBase < 0x40 && a - SdramBase >= 8)
            {
                ++st.exceptions;
                if(opt.trace) std::fprintf(stderr, "g2emu: exception vector %u at pc %08x\n", (a - SdramBase) / 4, cpu.getInstructionPC());
            }
            return be32(&sdram[a - SdramBase]);
        }
        if(a - SramBase < SramSize - 3) return be32(&sram[a - SramBase]);
        if(a < BootSize - 3) return be32(&boot[a]);
        return mmioRead(a, 4);
    }
    void write8(std::uint32_t a, std::uint8_t v) override
    {
        if(a - SdramBase < SdramSize) { sdram[a - SdramBase] = v; return; }
        if(a - SramBase < SramSize) { sram[a - SramBase] = v; return; }
        mmioWrite(a, 1, v);
    }
    void write16(std::uint32_t a, std::uint16_t v) override
    {
        if(a - SdramBase < SdramSize - 1) { put16(&sdram[a - SdramBase], v); return; }
        if(a - SramBase < SramSize - 1) { put16(&sram[a - SramBase], v); return; }
        mmioWrite(a, 2, v);
    }
    void write32(std::uint32_t a, std::uint32_t v) override
    {
        if(a - SdramBase < SdramSize - 3) { put32(&sdram[a - SdramBase], v); return; }
        if(a - SramBase < SramSize - 3) { put32(&sram[a - SramBase], v); return; }
        mmioWrite(a, 4, v);
    }
    std::uint16_t fetch16(const std::uint32_t a) override
    {
        if(a - SdramBase < SdramSize - 1) return be16(&sdram[a - SdramBase]);
        return read16(a);
    }
    std::uint8_t interruptAcknowledge(std::uint8_t level) override { return sim.acknowledge(level); }
    void writeControlRegister(std::uint16_t, std::uint32_t) override
    {
        // CACR, ACR0-3, RAMBAR0/1 ($C04/$C05), MBAR ($C0F): the caches are not modelled, and the OS maps the SRAM at
        // 0x20000000 and MBAR at 0x10000000 as the memory map above has them [C, §2.3, §3.6.1]
    }
    // the bus's wait states go to the core's m_g2WaitCycles, which it adds to the instruction that caused them
    std::uint32_t consumeWaitCycles() override
    {
        const auto w = cpu.m_g2WaitCycles;
        cpu.m_g2WaitCycles = 0;
        return w;
    }

    // ---- scheduling ----
    void runCf(std::uint64_t dspEnd)
    {
        const std::uint64_t target = cfTimeOfDsp(dspEnd);
        for(;;)
        {
            const std::uint64_t now = cfNow();
            if(now >= target) break;
            const std::uint64_t bus = busNow();
            if(bus >= nextSimEvent)
            {
                sim.advance(bus);
                nextSimEvent = sim.nextEvent();
                irqDirty = true;
            }
            if(irqDirty)
            {
                irqDirty = false;
                cpu.setInterruptLevel(std::uint8_t(sim.pendingLevel()));
            }
            // run until the next timed event or the end of the slice; an access to the SIM or the USB chip (which may
            // change an interrupt or the next event) ends the run early (irqDirty)
            const std::uint64_t eventCf = nextSimEvent == ~0ull ? target : std::uint64_t(double(nextSimEvent) / busPerCf) + 1;
            const std::uint64_t limit = std::min(target, std::max(eventCf, now + 1));
            if(cpu.isStopped())
            {
                // STOP: nothing runs until an interrupt; move on to the next event or the end of the slice
                if(sim.pendingLevel() == 0) { cfSkipped += limit - now; continue; }
                cpu.step();
                continue;
            }
            const std::uint64_t stepLimit = limit - cfSkipped;  // in the core's own cycle count
            if(!pcWatch.empty())
            {
                // G2EMU_PCWATCH=ADDR,ADDR,...: report when the ColdFire reaches these addresses (debugging)
                while(cpu.getCycles() < stepLimit && !irqDirty && !cpu.isStopped())
                {
                    const auto pc = cpu.getPC();
                    if(std::find(pcWatch.begin(), pcWatch.end(), pc) != pcWatch.end())
                        std::fprintf(stderr, "pcwatch %08x at %.4f s d0 %08x d1 %08x\n", pc, double(t) / (FrameRate * Dsp::CyclesPerFrame), cpu.getD(0), cpu.getD(1));
                    cpu.step();
                }
            }
            else if(profile)
            {
                while(cpu.getCycles() < stepLimit && !irqDirty)
                {
                    cpu.step();
                    if((++profileTick % 61) == 0 && t >= profileFrom)
                    {
                        ++(*profile)[cpu.getPC() & (profileExact ? ~1u : ~0x3fu)];
                        ++profileIpl[(cpu.getSR() >> 8) & 7];
                    }
                }
            }
            else
                cpu.run(stepLimit, irqDirty);  // while(cycles < stepLimit && !irqDirty && !stopped) step()
            if(cpu.isHalted())
            {
                if(opt.trace) std::fprintf(stderr, "g2emu: the ColdFire halted at %08x\n", cpu.getPC());
                cfSkipped += target - std::min(target, cfNow());
                break;
            }
        }
    }

    void usbStep()
    {
        {
            std::lock_guard lock(ioMutex);
            // a host that connects while the OS still boots loses its first request (the OS initialises its USB
            // chip again at the end of its boot) and retries only after 10 s: the cable goes in once the OS runs
            if(plugRequest && !host.plugged && t >= std::uint64_t(opt.usbAfter * FrameRate * Dsp::CyclesPerFrame))
            {
                plugRequest = false;
                host.plugged = true;
                host.plugFrame = t / Dsp::CyclesPerFrame;
                usb.attach();
                usb.busReset();
                usbIrqChanged();
            }
            if(!midiInQueue.empty())
            {
                for(const auto& m : midiInQueue)
                {
                    // frame f starts at master time f * 1536 DSP clocks: in bus clocks via the ColdFire's clock
                    const auto bus = std::uint64_t(double(m.frame) * Dsp::CyclesPerFrame * cfPerDsp * busPerCf);
                    if(m.frame) sim.midiInAt(m.bytes.data(), m.bytes.size(), bus);
                    else sim.midiIn(m.bytes.data(), m.bytes.size());
                }
                midiInQueue.clear();
                nextSimEvent = 0;
            }
            // MIDI OUT: the bytes whose stop bit has ended, with that time on the master clock (bus clocks to DSP
            // clocks, as above the other way). The oldest goes when nobody took the last midiOutKept.
            Sim::MidiOutByte sent[8];
            while(const auto n = sim.takeMidiOut(sent, std::size(sent), busNow()))
                for(std::size_t k = 0; k < n; ++k)
                {
                    if(midiOutHead - midiOutTail >= midiOutRing.size()) { ++midiOutTail; ++st.midiOutLost; }
                    midiOutRing[midiOutHead++ % midiOutRing.size()] = {std::uint64_t(double(sent[k].done) / (cfPerDsp * busPerCf)), sent[k].byte};
                    ++st.midiOutBytes;
                }
        }
        if(!host.plugged) return;
        const std::uint64_t ms = (t / Dsp::CyclesPerFrame - host.plugFrame) / (FrameRate / 1000);
        if(!host.setupSent && ms >= 5)
        {
            // what a host does before it uses the pipes: SET_ADDRESS 1, SET_CONFIGURATION 1
            const std::uint8_t setAddress[8] = {0x00, 0x05, 0x01, 0, 0, 0, 0, 0};
            const std::uint8_t setConfig[8] = {0x00, 0x09, 0x01, 0, 0, 0, 0, 0};
            usb.out(0, setAddress, 8, true);
            usb.out(0, setConfig, 8, true);
            host.setupSent = true;
            usbIrqChanged();
        }
        if(ms < 15) return;
        // interrupt-IN is polled all the time; bulk-IN only while an extended announcement (b0 & 3 == 1, BE16
        // length) waits for its data
        auto& inp = usb.inPackets();
        while(!inp.empty())
        {
            if(inp.front().first == 2) host.ints.push_back(std::move(inp.front().second));
            inp.pop_front();
        }
        std::vector<UsbIn> delivered;
        for(;;)
        {
            if(host.expect)
            {
                std::vector<std::uint8_t> d;
                if(!usb.pollBulkIn(d)) break;
                usbIrqChanged();
                // a transfer ends on a short packet, a zero-length one included, as libusb completes it. The OS
                // usually sends one or more zero-length packets before a message's data, as a real G2 does (two
                // before most messages in USB captures of a G2X): each is an empty transfer, after which the
                // client's transport reads again (re/notes/usb-crosscheck.md H1)
                const bool shortPacket = d.size() < 64;
                host.bulk.insert(host.bulk.end(), d.begin(), d.end());
                if(host.bulk.size() >= host.expect || shortPacket)
                {
                    const bool empty = host.bulk.empty();
                    delivered.push_back({true, std::move(host.bulk)});
                    host.bulk.clear();
                    if(!empty) host.expect = 0;
                }
                continue;
            }
            if(host.ints.empty()) break;
            auto p = std::move(host.ints.front());
            host.ints.pop_front();
            if(p.size() >= 3 && (p[0] & 3) == 1)
                host.expect = std::size_t(p[1]) << 8 | p[2];
            delivered.push_back({false, std::move(p)});
        }
        std::deque<std::vector<std::uint8_t>> frames;
        {
            std::lock_guard lock(ioMutex);
            host.arrived = true;
            for(auto& d : delivered) usbIn.push_back(std::move(d));
            frames.swap(usbOut);
        }
        for(const auto& f : frames)
        {
            for(std::size_t k = 0; k < f.size(); k += 64)
                usb.out(4, f.data() + k, std::min<std::size_t>(64, f.size() - k));
            if(f.size() % 64 == 0) usb.out(4, nullptr, 0);
            usbIrqChanged();
        }
    }

    void run(std::uint32_t frames, std::vector<float>* out)
    {
        const std::uint64_t end = t + std::uint64_t(frames) * Dsp::CyclesPerFrame;
        if(opt.threads && workers.empty()) startWorkers();
        while(t < end)
        {
            const std::uint64_t q = std::min<std::uint64_t>(end, t + opt.quantum);
            const auto t0 = timing ? clockNs() : 0;
            if(opt.threads)
            {
                // the ColdFire may not run more than `skew` ahead of the DSPs
                if(dspTime.load(std::memory_order_acquire) + opt.skew < q)
                {
                    const auto w0 = clockNs();
                    progress.wait([&] { return dspTime.load(std::memory_order_acquire) + opt.skew >= q; });
                    const auto dw = clockNs() - w0;
                    st.cfWaitNs += dw;
                    if(timing) { ++cfWaits; cfWaitHist[dw < 1000 ? 0 : dw < 10000 ? 1 : dw < 100000 ? 2 : 3]++; }
                }
                runCf(q);
                // (it only moves on: a status read may have moved it past this, see syncDsps)
                if(q + opt.dspLead > horizon.load(std::memory_order_relaxed)) horizon.store(q + opt.dspLead, std::memory_order_release);
                progress.ring();
            }
            else
            {
                runCf(q);
                const auto t1 = timing ? clockNs() : 0;
                for(int k : chain) dsps[std::size_t(k)]->runTo(q);
                if(timing) dspNs += clockNs() - t1;
            }
            if(timing) cfNs += clockNs() - t0;
            t = q;
            usbStep();
            panelStep();
        }
        if(opt.threads && dspTime.load(std::memory_order_acquire) < end)
        {
            const auto w0 = clockNs();
            progress.wait([&] { return dspTime.load(std::memory_order_acquire) >= end; });
            st.cfWaitNs += clockNs() - w0;
        }
        // hand out the DAC frames produced so far
        std::vector<std::int32_t> words;
        {
            std::lock_guard lock(dacMutex);
            words.swap(dac);
            if(!words.empty()) dacClock = dacSent;
        }
        if(out)
            for(auto w : words) out->push_back(float(w) / 8388608.0f);
        st.frames += words.size() / 4;
    }



    // Each worker runs its DSPs one frame at a time. A worker never runs past the one upstream of it (its DSPs need
    // that one's frames; the chain links hold chainPrefill frames of slack), and the first one never more than the
    // ring's prefill past the last one (A6 needs A3's frames from around the ring). The ColdFire's thread moves
    // `horizon`.
    // takeHosts(): a DSP the ColdFire waits for (syncDsps) runs on by itself, a few instructions at a time, until it
    // has taken what the ColdFire wrote, up to a frame further than its upstream: the worker's limit for its first
    // DSP, the whole frames the DSP before it has run for the others. A receiver that far ahead of its sender still
    // finds its frames: it takes the one sent chainPrefill = 2 frames before its own starts.
    void startWorkers()
    {
        const int n = std::clamp(opt.threads, 1, int(chain.size()));
        std::vector<std::vector<int>> parts(static_cast<std::size_t>(n));
        for(std::size_t k = 0; k < chain.size(); ++k)
        {
            parts[k * std::size_t(n) / chain.size()].push_back(chain[k]);
            dspWorker[std::size_t(chain[k])] = int(k * std::size_t(n) / chain.size());
        }
        horizon.store(t + opt.dspLead);
        dspTime.store(t);
        for(int w = 0; w < n; ++w) worker[std::size_t(w)].time.store(t);
        // (2 frames of the ring's prefill for the JIT's overshoot, as on the chain, and one for takeHosts())
        const std::uint64_t ringSlack = opt.ringPrefill > 3 ? std::uint64_t(opt.ringPrefill - 3) * Dsp::CyclesPerFrame : 0;
        for(int w = 0; w < n; ++w)
            workers.emplace_back([this, w, n, ringSlack, mine = parts[std::size_t(w)]] {
                auto& me = worker[std::size_t(w)];
                auto& my = me.time;
                if(opt.realtimeThreads) makeThreadRealtime(0.001, 0.0005);
                RealtimeBreath breath;
                std::uint64_t done = my.load(), frames = 0;
                auto limit = [&] {
                    std::uint64_t l = horizon.load(std::memory_order_acquire);
                    if(w > 0) l = std::min(l, worker[std::size_t(w - 1)].time.load(std::memory_order_acquire));
                    else if(n > 1) l = std::min(l, worker[std::size_t(n - 1)].time.load(std::memory_order_acquire) + ringSlack);
                    return l;
                };
                // The request takeHosts() tried last, with which limit, how far the worker had got and after how
                // many runs of a DSP: it tries again when one of them has moved on.
                std::uint64_t served = 0, triedLimit = 0, triedDone = 0, runs = 0, triedRuns = 0;
                auto takeHosts = [&] {
                    const auto bits = me.takeHost.load(std::memory_order_acquire);
                    if(!bits) return;
                    const auto ask = me.asked.load(std::memory_order_acquire);
                    const auto l = limit();
                    if(ask == served && l <= triedLimit && done == triedDone && runs == triedRuns) return;
                    served = ask;
                    triedLimit = l;
                    triedDone = done;
                    triedRuns = runs;
                    bool taken = true;
                    std::uint64_t up = l;
                    for(int k : mine)
                    {
                        auto& d = *dsps[std::size_t(k)];
                        if(bits & (1u << k)) taken = d.takeHost(up + Dsp::CyclesPerFrame) && taken;
                        up = std::min(l, d.clock() / Dsp::CyclesPerFrame * Dsp::CyclesPerFrame);
                    }
                    // Held back by the horizon (the worker has run up to it): the ColdFire may move it. Otherwise the
                    // worker runs on, or the workers upstream do (they have room), and it tries again then.
                    if(!taken && done >= l && l >= horizon.load(std::memory_order_acquire)) me.stuck.store(ask, std::memory_order_release);
                    progress.ring();
                };
                while(!quit.load(std::memory_order_relaxed))
                {
                    takeHosts();
                    const auto l = limit();
                    if(done >= l)
                    {
                        const auto w0 = clockNs();
                        progress.wait([&] {
                            return quit.load(std::memory_order_relaxed) || limit() > done ||
                                   (me.takeHost.load(std::memory_order_acquire) && me.asked.load(std::memory_order_acquire) != served);
                        });
                        const auto waited = clockNs() - w0;
                        me.waitNs += waited;
                        // (the thread's CPU time is a system call: not after the short waits that end with a host
                        // port to serve, thousands a second)
                        if(waited > 20000) me.cpuNs.store(threadCpuNs(), std::memory_order_relaxed);
                        continue;
                    }
                    const auto target = std::min(l, done + Dsp::CyclesPerFrame);
                    for(int k : mine)
                    {
                        dsps[std::size_t(k)]->runTo(target);
                        ++runs;
                        takeHosts();
                    }
                    done = target;
                    my.store(done, std::memory_order_release);
                    if((++frames & 63) == 0)
                    {
                        me.cpuNs.store(threadCpuNs(), std::memory_order_relaxed);
                        if(opt.realtimeThreads) breath();
                    }
                    std::uint64_t slowest = done;
                    for(int v = 0; v < n; ++v) slowest = std::min(slowest, worker[std::size_t(v)].time.load(std::memory_order_acquire));
                    // dspTime only grows: a lagging worker may publish an older minimum than another just did
                    auto cur = dspTime.load(std::memory_order_relaxed);
                    while(slowest > cur && !dspTime.compare_exchange_weak(cur, slowest)) {}
                    progress.ring();
                }
            });
    }

    void stopWorker()
    {
        quit = true;
        progress.ring();
        for(auto& w : workers) w.join();
        workers.clear();
    }

    Stats stats()
    {
        Stats s = st;
        s.cfInstructions = cpu.getInstructionCount();
        s.cfCycles = cfNow();
        s.cfSkipped = cfSkipped;
        s.dspThreads = opt.threads;
        s.cfPc = cpu.getPC();
        s.dsps = int(chain.size());
        for(int n : chain)
        {
            s.dspExecuted[n] = dsps[std::size_t(n)]->executed();
            s.dspSkipped[n] = dsps[std::size_t(n)]->skipped();
            s.dspPc[n] = dsps[std::size_t(n)]->pc();
            if(dsps[std::size_t(n)]->wild()) ++s.dspsWild;
            s.hostEmptyReads += dsps[std::size_t(n)]->hostEmptyReads();
            s.dspEmptyReads += dsps[std::size_t(n)]->dspEmptyReads();
        }
        for(const auto& l : links)
            if(l) { s.linkUnderruns += l->underruns; s.linkOverruns += l->overruns; }
        s.midiOverruns = sim.midiOverruns();
        s.midiOutLost += sim.midiOutLost();
        for(std::size_t w = 0; w < workers.size(); ++w)
        {
            s.dspThreadCpuNs[w] = worker[w].cpuNs.load(std::memory_order_relaxed);
            s.dspThreadWaitNs[w] = worker[w].waitNs.load(std::memory_order_relaxed);
        }
        return s;
    }
};

// ---------------------------------------------------------------------------------------------------------------

bool Machine::hasExpansion(const Options& options)
{
    return options.expansion == Expansion::With || (options.expansion == Expansion::AsBuilt && options.model == PanelModel::G2X);
}

int Machine::autoThreads(int dsps)
{
    int cores = 0;
#ifdef __APPLE__
    // Apple Silicon: the performance cores (perflevel0); an Intel Mac: its physical cores
    for(const char* name : {"hw.perflevel0.physicalcpu", "hw.physicalcpu"})
    {
        int n = 0;
        std::size_t size = sizeof n;
        if(sysctlbyname(name, &n, &size, nullptr, 0) == 0 && n > 0)
        {
            cores = n;
            break;
        }
    }
#endif
    if(!cores)
    {
        // hardware threads: 6 or more are at least 3 cores, or 4 without SMT on most current processors
        const auto t = int(std::thread::hardware_concurrency());
        cores = t >= 6 ? 4 : (t >= 4 ? 3 : t);
    }
    // Eight DSPs (the expansion board): a third thread where there are 6 performance cores, so that the ColdFire's
    // thread and three DSP threads leave the host's audio thread a core. Provisional: not measured on a quiet
    // computer yet (re/notes §3.12.5).
    if(dsps > 4 && cores >= 6) return 3;
    return cores >= 4 ? 2 : 1;
}

Machine::Machine(const Firmware& firmware) : Machine(firmware, Options{}) {}
Machine::Machine(const Firmware& firmware, Options options) : impl_(std::make_unique<Impl>(firmware, options)) {}
Machine::~Machine() = default;

void Machine::run(std::uint32_t frames, std::vector<float>* out) { impl_->run(frames, out); }
std::uint64_t Machine::frame() const { return impl_->t / Dsp::CyclesPerFrame; }

static_assert(Machine::ClocksPerFrame == Dsp::CyclesPerFrame);

void Machine::setAdcSource(AdcSource* source) { impl_->dsps[std::size_t(impl_->chain.front())]->setSource(source); }
std::uint64_t Machine::dacClock() const { return impl_->dacClock; }

void Machine::plugUsb()
{
    std::lock_guard lock(impl_->ioMutex);
    impl_->plugRequest = true;
}

bool Machine::usbArrived() const
{
    std::lock_guard lock(impl_->ioMutex);
    return impl_->host.arrived;
}

void Machine::usbSend(std::span<const std::uint8_t> frame)
{
    std::lock_guard lock(impl_->ioMutex);
    impl_->usbOut.emplace_back(frame.begin(), frame.end());
}

bool Machine::usbTake(UsbIn& in)
{
    std::lock_guard lock(impl_->ioMutex);
    if(impl_->usbIn.empty()) return false;
    in = std::move(impl_->usbIn.front());
    impl_->usbIn.pop_front();
    return true;
}

void Machine::midiIn(std::span<const std::uint8_t> bytes)
{
    std::lock_guard lock(impl_->ioMutex);
    impl_->midiInQueue.push_back({{bytes.begin(), bytes.end()}, 0});
}

void Machine::midiInAt(std::span<const std::uint8_t> bytes, std::uint64_t frame)
{
    std::lock_guard lock(impl_->ioMutex);
    impl_->midiInQueue.push_back({{bytes.begin(), bytes.end()}, frame});
}

std::size_t Machine::takeMidiOut(std::span<MidiOut> out)
{
    std::lock_guard lock(impl_->ioMutex);
    std::size_t n = 0;
    for(; n < out.size() && impl_->midiOutTail != impl_->midiOutHead; ++n)
        out[n] = impl_->midiOutRing[impl_->midiOutTail++ % impl_->midiOutRing.size()];
    return n;
}

std::vector<std::uint8_t> Machine::takeMidiOut()
{
    std::vector<std::uint8_t> v;
    MidiOut sent[256];
    while(const auto n = takeMidiOut(sent))
        for(std::size_t k = 0; k < n; ++k) v.push_back(sent[k].byte);
    return v;
}

std::vector<std::uint8_t>& Machine::flash() { return impl_->flashChip.data(); }
std::vector<std::uint8_t> Machine::flashSnapshot() const { return impl_->flashChip.snapshot(); }
std::uint64_t Machine::flashChanges() const { return impl_->flashChip.changes(); }

void Machine::loadMemory(std::span<const std::uint8_t> image)
{
    auto& flash = impl_->flashChip.data();
    const auto n = std::min(flash.size(), image.size());
    std::copy_n(image.begin(), n, flash.begin());
    if(image.size() > flash.size()) impl_->sim.eeprom().load(image.data() + flash.size(), image.size() - flash.size());
}

static_assert(Machine::EepromSize == Eeprom::Size);

std::vector<std::uint8_t> Machine::memorySnapshot() const
{
    auto image = impl_->flashChip.snapshot();
    const auto eeprom = impl_->sim.eeprom().snapshot();
    image.insert(image.end(), eeprom.begin(), eeprom.end());
    return image;
}

std::uint64_t Machine::memoryChanges() const { return impl_->flashChip.changes() + impl_->sim.eeprom().changes(); }

PanelState Machine::panel() const
{
    std::lock_guard lock(impl_->panelMutex);
    return impl_->panelPublished;
}

namespace {
// The scan positions of the knobs' quadrature pairs, knob 1 to 8 [emulated: each knob turned changes the parameter
// shown above it]: byte 7 holds knobs 1, 3, 5, 7, byte 6 knobs 2, 4, 6, 8
constexpr int KnobScan[8] = {7, 3, 6, 2, 5, 1, 4, 0};
// The panel ADC's stream positions of the analogue controls [C: the OS's handlers 0x30012588 and the morph groups
// they feed, see re/notes §3.10]
constexpr int AdcPosition[PanelAnalogCount] = {1, 4, 5, 2, 3, 6, 0};
} // namespace

void Machine::panelButton(PanelButton button, bool down)
{
    if(int(button) < PanelButtonCount) panelRawButton(int(button), down);
}

void Machine::panelEncoder(PanelEncoder encoder, int steps)
{
    if(!steps) return;
    if(encoder == PanelEncoder::Dial) panelRawEncoder(8, 4 * steps);
    else if(int(encoder) < 8)
    {
        // The OS ignores the first transition after a knob changes direction [C 0x30057aac: a step whose sign
        // cancels the previous one is dropped]; one more makes a step back a step.
        int extra = 0;
        {
            std::lock_guard lock(impl_->panelMutex);
            auto& dir = impl_->knobDirection[std::size_t(encoder)];
            if(dir && (dir > 0) != (steps > 0)) extra = steps > 0 ? 1 : -1;
            dir = steps > 0 ? 1 : -1;
        }
        panelRawEncoder(KnobScan[int(encoder)], steps + extra);
    }
}

void Machine::panelAnalog(PanelAnalog control, float value)
{
    if(int(control) >= PanelAnalogCount) return;
    auto v = std::uint8_t(std::lround(std::clamp(double(value), 0.0, 1.0) * 255));
    if(control == PanelAnalog::Aftertouch) v = std::uint8_t(255 - v);  // the sensor reads 0xFF at rest [C 0x300125d0]
    panelRawAdc(AdcPosition[int(control)], v);
}

void Machine::panelKey(int key, bool down, int velocity)
{
    // The OS times contact A closing to contact B closing in ticks (8272 Hz), halves it and looks the velocity up
    // in a falling curve [C 0x30029468]; about 127 * 0.97^(i - 6) for i = ticks / 2 (fitted; a per-key offset the OS
    // adds is not modelled)
    const double v = std::clamp(velocity, 1, 127);
    const double i = 6.0 + std::log(127.0 / v) / 0.03046;
    panelRawKey(key, down, 2.0 * i / 8.272);
}

void Machine::panelSustainPedal(bool down) { impl_->panelInput({Impl::PanelInput::Sustain, 1, down ? 1 : 0, 0}); }

void Machine::panelRawButton(int raw, bool down) { impl_->panelInput({Impl::PanelInput::Button, raw, down ? 1 : 0, 0}); }
void Machine::panelRawEncoder(int raw, int transitions) { impl_->panelInput({Impl::PanelInput::Encoder, raw, transitions, 0}); }
void Machine::panelRawAdc(int position, std::uint8_t value)
{
    if(position >= 0 && position < 7) impl_->panelInput({Impl::PanelInput::Adc, position, value, 0});
}
void Machine::panelRawKey(int raw, bool down, double contactMs)
{
    impl_->panelInput({Impl::PanelInput::Key, raw, down ? 1 : 0, int(contactMs * 1e-3 * Sim::BusHz)});
}
std::string Machine::panelDebug() const { return impl_->panelDebug(); }

Machine::Stats Machine::stats() const { return impl_->stats(); }
int Machine::dspThreads() const { return impl_->opt.threads; }

std::uint32_t Machine::dspMemory(int n, int area, std::uint32_t address) const
{
    const auto* d = impl_->dsps[std::size_t(n) % MaxDsps].get();
    return d ? d->memRead(dsp56k::EMemArea(area), address) : 0;
}

std::uint32_t Machine::cfRead32(std::uint32_t address) const
{
    auto& i = *impl_;
    if(address - SdramBase < SdramSize - 3) return be32(&i.sdram[address - SdramBase]);
    if(address - SramBase < SramSize - 3) return be32(&i.sram[address - SramBase]);
    return 0;
}

} // namespace g2emu
