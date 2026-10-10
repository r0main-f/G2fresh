#include "g2emu/machine.hpp"

#include "dsp.hpp"
#include "hw.hpp"
#include "thread.hpp"

#include "coldfire/cfCpu.h"
#include "dsp56kBase/logging.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <thread>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>

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
    static std::mutex m;
    static std::string last;
    std::lock_guard lock(m);
    if(s.find("Write ESAI") != std::string::npos || s.find("Write Timer") != std::string::npos ||
       s.find("HPCR") != std::string::npos || s.find("underrun") != std::string::npos || s.find("DSP Boot") != std::string::npos ||
       s.find("Clock speed") != std::string::npos ||
       s.find("Empty read") != std::string::npos ||  // the OS's $AE pings read the host port without a word first
       s.find("Undefined opcode 000040") != std::string::npos)  // in the OS's DSP code, harmless (an empty vector)
        return;
    if(s == last) return;
    last = s;
    std::fprintf(stderr, "dsp56300: %s\n", s.c_str());
}

// A host port of an absent DSP (the expansion board's A7..A10): INIT completes at once, a host command is taken at
// once, HF0 is echoed as HF2 (as g2hostemu.py's stubs).
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
    std::array<std::unique_ptr<Dsp>, 4> dsps;
    std::array<StubPort, 4> stubs;
    std::array<std::unique_ptr<Link>, 8> links;
    std::array<int, 4> chain{3, 2, 1, 0};  // A6 (clock master, ADCs) -> A5 -> A4 -> A3 (DACs) [§3.7.3]
    std::vector<std::int32_t> dac;          // the DACs' words, 4 per frame
    std::mutex dacMutex;

    // threads = N > 0: N DSP threads, each running a stretch of the chain (in chain order) up to `horizon`; each
    // publishes how far it got in `workerTime`, and `dspTime` is the slowest of them
    std::vector<BigStackThread> workers;
    std::array<std::atomic<std::uint64_t>, 4> workerTime{};
    std::array<std::atomic<std::uint64_t>, 4> workerCpuNs{}, workerWaitNs{};  // per DSP thread: CPU time, time waiting
    std::atomic<std::uint64_t> horizon{0}, dspTime{0};
    std::atomic<bool> quit{false};

    // time
    std::uint64_t t = 0;         // master time in DSP clocks
    double cfPerDsp = 1.0;       // ColdFire cycles per DSP clock
    double busPerCf = 1.0 / 3;   // bus clocks per ColdFire cycle
    std::uint64_t cfSkipped = 0; // ColdFire cycles skipped (stopped)
    std::uint64_t nextSimEvent = 0;
    bool irqDirty = true;
    std::uint32_t waitCycles = 0;

    // USB host side
    struct UsbHostState {
        bool plugged = false, arrived = false, setupSent = false;
        std::uint64_t plugFrame = 0;
        std::vector<std::uint8_t> announce, bulk;
        std::size_t expect = 0;
        std::deque<std::vector<std::uint8_t>> ints;
    } host;
    mutable std::mutex ioMutex;            // guards the queues below
    std::deque<std::vector<std::uint8_t>> usbOut;  // from the client
    std::deque<UsbIn> usbIn;               // to the client
    bool plugRequest = false;
    std::vector<std::uint8_t> midiInQueue, midiOutQueue;

    // statistics
    Stats st;
    std::unique_ptr<std::map<std::uint32_t, std::uint64_t>> profile;  // G2EMU_CFPROFILE: ColdFire PCs, every 64th
    std::uint64_t profileTick = 0, profileFrom = 0;
    std::uint64_t profileIpl[8] = {};
    ~Impl() override
    {
        stopWorker();
        if(timing)
            std::fprintf(stderr, "time: cf %.2f s (of which DSP catch-up %.2f s), DSPs at the end of each slice %.2f s\n", cfNs * 1e-9,
                         syncNs * 1e-9, dspNs * 1e-9);
        if(!profile) return;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> v;
        std::uint64_t total = 0;
        for(auto [pc, n] : *profile) { v.emplace_back(n, pc); total += n; }
        std::sort(v.rbegin(), v.rend());
        std::fprintf(stderr, "ColdFire PC profile (%llu samples):\n", (unsigned long long)total);
        std::fprintf(stderr, "  by interrupt mask: ");
        for(int i = 0; i < 8; ++i) std::fprintf(stderr, "%d: %.1f%% ", i, 100.0 * double(profileIpl[i]) / double(total));
        std::fprintf(stderr, "\n");
        for(std::size_t i = 0; i < std::min<std::size_t>(40, v.size()); ++i)
            std::fprintf(stderr, "  %08x %5.2f%%\n", v[i].second, 100.0 * double(v[i].first) / double(total));
    }

    Impl(const Firmware& fw, Options o) : opt(o)
    {
        Logging::setLogFunc(&quietLog);
        cfPerDsp = opt.cfHz / (double(FrameRate) * Dsp::CyclesPerFrame);
        busPerCf = double(Sim::BusHz) / opt.cfHz;
        load(fw);
        Dsp::Options dopt;
        dopt.jit = opt.jit;
        dopt.idleSkip = opt.idleSkip;
        for(int n = 0; n < 4; ++n) dsps[std::size_t(n)] = std::make_unique<Dsp>(n, dopt);
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
        for(int n = 0; n < 4; ++n)
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
        dsps[std::size_t(last)]->setSink(&dac, &dacMutex);
    }

    // ---- time ----
    std::uint64_t cfNow() const { return cpu.getCycles() + cfSkipped; }
    // The bus clock (54 MHz [C]: the OS's MIDI divider) runs with the master clock, whatever the core's speed.
    std::uint64_t busNow() const { return std::uint64_t(double(cfNow()) * busPerCf); }
    std::uint64_t dspTimeOfCf(std::uint64_t cf) const { return std::uint64_t(double(cf) / cfPerDsp); }
    std::uint64_t cfTimeOfDsp(std::uint64_t d) const { return std::uint64_t(double(d) * cfPerDsp); }

    // Catches the DSPs up to the ColdFire's time, in chain order up to DSP n (each needs its upstream's frames).
    void syncDsps(int n)
    {
        if(opt.threads) return;  // the DSPs run on their own thread
        const auto target = dspTimeOfCf(cfNow());
        ++st.dspSyncs;
        const auto t0 = timing ? clockNs() : 0;
        for(int k : chain)
        {
            dsps[std::size_t(k)]->runTo(target);
            if(k == n) break;
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
    std::uint64_t syncNs = 0, cfNs = 0, dspNs = 0;

    // ---- the ColdFire bus ----
    std::uint32_t mmioRead(std::uint32_t a, int size)
    {
        switch(a >> 24)
        {
        case 0x10:
            if(a < 0x10001000) { irqDirty = true; waitCycles += 6; return sim.read(a & 0xfff, size); }
            break;
        case 0x11:
            if(a < 0x11001000)
            {
                std::uint32_t v = 0;
                for(int k = 0; k < size; ++k) v = v << 8 | hostRead8((a + std::uint32_t(k)) & 0xfff);
                return v;
            }
            break;
        case 0x12: waitCycles += 15; return flashChip.read(a & (Flash::Size - 1), size);
        case 0x13:
            if(a < 0x13010000)
            {
                waitCycles += 12;
                const auto v = usb.read(a & 0xffff, size);
                usbIrqChanged();
                return v;
            }
            break;
        case 0x14: case 0x15: case 0x16: case 0x17:
            if((a & 0xffffff) < 0x10000) { waitCycles += 12; return 0; }  // panel latches: read 0 (stub)
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
                waitCycles += 6;
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
        case 0x12: waitCycles += 15; flashChip.write(a & (Flash::Size - 1), size, v); return;
        case 0x13:
            if(a < 0x13010000) { waitCycles += 12; usb.write(a & 0xffff, size, v); usbIrqChanged(); return; }
            break;
        case 0x14: case 0x15: case 0x16: case 0x17:
            if((a & 0xffffff) < 0x10000) { waitCycles += 12; return; }  // panel latches, LEDs/LCD (not modelled)
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
        waitCycles += 12;
        const unsigned sel = (~off >> 3) & 0xff;
        const int reg = int(off & 7);
        std::uint8_t v = 0xff;
        for(int n = 0; n < 8; ++n)
        {
            if(!(sel & (1u << n))) continue;
            if(n < 4)
            {
                auto& d = *dsps[std::size_t(n)];
                if(d.hostReadDependsOnDsp(reg)) syncDsps(n);
                ++st.hostReads;
                v &= d.hostRead(reg);
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
        if(!repeat || !opt.pollSkip || (reg != 1 && reg != 2) || (sel & (sel - 1)) || !(sel & 0x0f)) return v;
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
        waitCycles += 12;
        poll.off = ~0u;
        const unsigned sel = (~off >> 3) & 0xff;
        const int reg = int(off & 7);
        for(int n = 0; n < 8; ++n)
        {
            if(!(sel & (1u << n))) continue;
            if(n < 4)
            {
                syncDsps(n);
                if(reg == 1 && (v & 0x80)) ++st.hostCommands;
                dsps[std::size_t(n)]->hostWrite(reg, v);
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
    std::uint32_t consumeWaitCycles() override
    {
        const auto w = waitCycles;
        waitCycles = 0;
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
            if(profile)
            {
                while(cpu.getCycles() < stepLimit && !irqDirty)
                {
                    cpu.step();
                    if((++profileTick % 61) == 0 && t >= profileFrom)
                    {
                        ++(*profile)[cpu.getPC() & ~0x3fu];
                        ++profileIpl[(cpu.getSR() >> 8) & 7];
                    }
                }
            }
            else
                while(cpu.getCycles() < stepLimit && !irqDirty && !cpu.isStopped()) cpu.step();
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
                sim.midiIn(midiInQueue.data(), midiInQueue.size());
                midiInQueue.clear();
                nextSimEvent = 0;
            }
            auto mo = sim.takeMidiOut();
            midiOutQueue.insert(midiOutQueue.end(), mo.begin(), mo.end());
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
                const bool shortPacket = !d.empty() && d.size() < 64;
                host.bulk.insert(host.bulk.end(), d.begin(), d.end());
                if(host.bulk.size() >= host.expect || shortPacket)
                {
                    delivered.push_back({false, host.announce});
                    delivered.push_back({true, std::move(host.bulk)});
                    host.bulk.clear();
                    host.expect = 0;
                }
                continue;
            }
            if(host.ints.empty()) break;
            auto p = std::move(host.ints.front());
            host.ints.pop_front();
            if(p.size() >= 3 && (p[0] & 3) == 1)
            {
                host.announce = p;
                host.expect = std::size_t(p[1]) << 8 | p[2];
            }
            else
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
                waitFor([&] { return dspTime.load(std::memory_order_acquire) + opt.skew >= q; });
                runCf(q);
                horizon.store(q + opt.skew, std::memory_order_release);
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
        }
        if(opt.threads)
            waitFor([&] { return dspTime.load(std::memory_order_acquire) >= end; });
        // hand out the DAC frames produced so far
        std::vector<std::int32_t> words;
        {
            std::lock_guard lock(dacMutex);
            words.swap(dac);
        }
        if(out)
            for(auto w : words) out->push_back(float(w) / 8388608.0f);
        st.frames += words.size() / 4;
    }

    // spins briefly, then yields
    template<typename F> static void waitFor(F ready)
    {
        for(int i = 0; !ready(); ++i)
            if(i > 64) std::this_thread::yield();
    }

    // Each worker runs its DSPs one frame at a time. A worker never runs past the one upstream of it (its DSPs need
    // that one's frames; the chain links hold chainPrefill frames of slack), and the first one never more than the ring's
    // prefill past the last one (A6 needs A3's frames from around the ring). The ColdFire's thread moves `horizon`.
    void startWorkers()
    {
        const int n = std::clamp(opt.threads, 1, 4);
        std::vector<std::vector<int>> parts(static_cast<std::size_t>(n));
        for(std::size_t k = 0; k < chain.size(); ++k) parts[k * std::size_t(n) / chain.size()].push_back(chain[k]);
        horizon.store(t + opt.skew);
        dspTime.store(t);
        for(int w = 0; w < n; ++w) workerTime[std::size_t(w)].store(t);
        const std::uint64_t ringSlack = opt.ringPrefill > 2 ? std::uint64_t(opt.ringPrefill - 2) * Dsp::CyclesPerFrame : 0;
        for(int w = 0; w < n; ++w)
            workers.emplace_back([this, w, n, ringSlack, mine = parts[std::size_t(w)]] {
                auto& my = workerTime[std::size_t(w)];
                std::uint64_t done = my.load();
                auto limit = [&] {
                    std::uint64_t l = horizon.load(std::memory_order_acquire);
                    if(w > 0) l = std::min(l, workerTime[std::size_t(w - 1)].load(std::memory_order_acquire));
                    else if(n > 1) l = std::min(l, workerTime[std::size_t(n - 1)].load(std::memory_order_acquire) + ringSlack);
                    return l;
                };
                while(!quit.load(std::memory_order_relaxed))
                {
                    const auto l = limit();
                    if(done >= l)
                    {
                        const auto w0 = clockNs();
                        waitFor([&] { return quit.load(std::memory_order_relaxed) || limit() > done; });
                        workerWaitNs[std::size_t(w)] += clockNs() - w0;
                        workerCpuNs[std::size_t(w)].store(threadCpuNs(), std::memory_order_relaxed);
                        continue;
                    }
                    const auto target = std::min(l, done + Dsp::CyclesPerFrame);
                    for(int k : mine) dsps[std::size_t(k)]->runTo(target);
                    done = target;
                    my.store(done, std::memory_order_release);
                    std::uint64_t slowest = done;
                    for(int v = 0; v < n; ++v) slowest = std::min(slowest, workerTime[std::size_t(v)].load(std::memory_order_acquire));
                    // dspTime only grows: a lagging worker may publish an older minimum than another just did
                    auto cur = dspTime.load(std::memory_order_relaxed);
                    while(slowest > cur && !dspTime.compare_exchange_weak(cur, slowest)) {}
                }
            });
    }

    void stopWorker()
    {
        quit = true;
        for(auto& w : workers) w.join();
        workers.clear();
    }

    Stats stats()
    {
        Stats s = st;
        s.cfInstructions = cpu.getInstructionCount();
        s.cfCycles = cfNow();
        s.cfSkipped = cfSkipped;
        s.cfPc = cpu.getPC();
        for(int n = 0; n < 4; ++n)
        {
            s.dspExecuted[n] = dsps[std::size_t(n)]->executed();
            s.dspSkipped[n] = dsps[std::size_t(n)]->skipped();
            s.dspPc[n] = dsps[std::size_t(n)]->pc();
            if(dsps[std::size_t(n)]->wild()) ++s.dspsWild;
        }
        for(const auto& l : links)
            if(l) { s.linkUnderruns += l->underruns; s.linkOverruns += l->overruns; }
        s.midiOverruns = sim.midiOverruns();
        for(std::size_t w = 0; w < workers.size(); ++w)
        {
            s.dspThreadCpuNs[w] = workerCpuNs[w].load(std::memory_order_relaxed);
            s.dspThreadWaitNs[w] = workerWaitNs[w].load(std::memory_order_relaxed);
        }
        return s;
    }
};

// ---------------------------------------------------------------------------------------------------------------

Machine::Machine(const Firmware& firmware) : Machine(firmware, Options{}) {}
Machine::Machine(const Firmware& firmware, Options options) : impl_(std::make_unique<Impl>(firmware, options)) {}
Machine::~Machine() = default;

void Machine::run(std::uint32_t frames, std::vector<float>* out) { impl_->run(frames, out); }
std::uint64_t Machine::frame() const { return impl_->t / Dsp::CyclesPerFrame; }

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
    impl_->midiInQueue.insert(impl_->midiInQueue.end(), bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> Machine::takeMidiOut()
{
    std::lock_guard lock(impl_->ioMutex);
    std::vector<std::uint8_t> v;
    v.swap(impl_->midiOutQueue);
    return v;
}

std::vector<std::uint8_t>& Machine::flash() { return impl_->flashChip.data(); }

Machine::Stats Machine::stats() const { return impl_->stats(); }

std::uint32_t Machine::dspMemory(int n, int area, std::uint32_t address) const
{
    return impl_->dsps[std::size_t(n & 3)]->memRead(dsp56k::EMemArea(area), address);
}

std::uint32_t Machine::cfRead32(std::uint32_t address) const
{
    auto& i = *impl_;
    if(address - SdramBase < SdramSize - 3) return be32(&i.sdram[address - SdramBase]);
    if(address - SramBase < SramSize - 3) return be32(&i.sram[address - SramBase]);
    return 0;
}

} // namespace g2emu
