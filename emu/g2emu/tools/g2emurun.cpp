// g2emurun: the whole emulated G2 from the command line (re/notes/g2-hardware-and-emulation.md §3.9).
//
//   g2emurun [--fw PATH] [--patch FILE | --kbd FILE] [--to B:FILE ...] [--settle S] [--seconds S]
//            [--note N@ON-OFF ...] [--midi N@ON-OFF[:CH] ...] [--wav OUT.wav] [--json OUT.json]
//            [--realtime S] [--flash FILE] [--boot S] [--quantum N] [--threads 0|1] [--skew N] [--no-jit] [--no-idle-skip] [--cf-mhz F] [--trace]
//
// Boots the user's own firmware (--fw: the updater's .rsrc, the updater app, or G2fresh's original/firmware),
// plugs in our protocol client (proto::Client over the emulated USB chip) after --boot emulated seconds, waits for
// its sync, uploads a patch (--patch: into slot A; --kbd: a performance with the patch in slot A and the keyboard on
// slot A, so that notes play it), lets it settle, then records the DACs for --seconds while playing notes: --note
// through the protocol (PlayNote), --midi as MIDI note on/off bytes into the OS's MIDI IN (UART0). Times are seconds
// from the start of the recording. Prints the output's level and frequency per channel and the speed.
#include "g2/file.hpp"
#include "g2/patch.hpp"
#include "g2/proto/client.hpp"
#include "g2/proto/logging_transport.hpp"
#include "g2emu/firmware.hpp"
#include "g2emu/machine.hpp"
#include "g2emu/runner.hpp"
#include "g2/proto/link.hpp"
#include "g2emu/transport.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <sys/resource.h>

using namespace g2;

namespace {

struct NoteEvent {
    double at = 0;
    int note = 60;
    bool on = true;
    bool midi = false;
    int channel = 0;
};

double threadCpuSeconds()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}

double cpuSeconds()
{
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return double(r.ru_utime.tv_sec + r.ru_stime.tv_sec) + 1e-6 * double(r.ru_utime.tv_usec + r.ru_stime.tv_usec);
}

std::vector<std::uint8_t> readFile(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    if(!f) throw std::runtime_error("cannot read " + p);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// float WAV (format 3), interleaved
void writeWav(const std::string& path, const std::vector<float>& data, int channels, int rate)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = std::uint32_t(data.size() * 4);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(3); u16(std::uint16_t(channels));
    u32(std::uint32_t(rate)); u32(std::uint32_t(rate * 4 * channels)); u16(std::uint16_t(4 * channels)); u16(32);
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(bytes));
}

struct Analysis {
    double peak = 0, rms = 0, dc = 0, freq = 0;
    double onset = -1, end = -1;  // seconds: first and last sample above 10 % of the peak
};

// Frequency from the interpolated upward zero crossings of the DC-free signal (enough for a steady tone).
Analysis analyse(const std::vector<float>& x)
{
    Analysis a;
    if(x.empty()) return a;
    double s = 0, s2 = 0;
    for(float v : x) { s += v; s2 += double(v) * v; a.peak = std::max(a.peak, double(std::fabs(v))); }
    a.dc = s / double(x.size());
    a.rms = std::sqrt(std::max(0.0, s2 / double(x.size()) - a.dc * a.dc));
    double first = -1, last = -1;
    std::size_t n = 0;
    for(std::size_t i = 1; i < x.size(); ++i)
    {
        const double p = x[i - 1] - a.dc, q = x[i] - a.dc;
        if(p < 0 && q >= 0)
        {
            const double tc = double(i - 1) + p / (p - q);
            if(first < 0) first = tc;
            last = tc;
            ++n;
        }
    }
    if(n >= 2 && a.rms > 1e-7) a.freq = double(n - 1) * g2emu::Machine::FrameRate / (last - first);
    if(a.peak > 1e-6)
        for(std::size_t i = 0; i < x.size(); ++i)
            if(std::fabs(x[i] - a.dc) > 0.1 * a.peak)
            {
                if(a.onset < 0) a.onset = double(i) / g2emu::Machine::FrameRate;
                a.end = double(i) / g2emu::Machine::FrameRate;
            }
    return a;
}

// a note spec N@ON-OFF[:CH]
void parseNotes(const std::string& s, bool midi, std::vector<NoteEvent>& ev)
{
    int note = 0, ch = 1;
    double on = 0, off = 0;
    const auto colon = s.find(':');
    if(std::sscanf(s.c_str(), "%d@%lf-%lf", &note, &on, &off) != 3) throw std::runtime_error("bad note " + s);
    if(colon != std::string::npos) ch = std::atoi(s.c_str() + colon + 1);
    ev.push_back({on, note, true, midi, ch - 1});
    ev.push_back({off, note, false, midi, ch - 1});
}

struct Listener : proto::Client::Listener {
    int leds = 0;
    void ledsChanged(int) override { ++leds; }
    void statusChanged(proto::Status s) override { std::fprintf(stderr, "proto: status %s\n", proto::statusText(s)); }
    void synced() override { std::fprintf(stderr, "proto: synced\n"); }
    void error(std::uint8_t code) override { std::fprintf(stderr, "proto: synth exception %02x\n", code); }
};

// --realtime S: the Runner as a sound engine would use it. The machine runs on its own threads; this thread plays
// the audio callback (10 ms blocks at the wall clock) and the editor (a LocalLink on the emulated USB, ticked every
// millisecond, with real timeouts). Boot and sync first, upload, then S seconds of audio with the notes.
int runRealtime(const g2emu::Firmware& fw, g2emu::Machine::Options opt, const std::string& patchPath, bool keyboard,
                std::vector<NoteEvent> notes, double seconds, const std::string& wav)
{
    using Clock = std::chrono::steady_clock;
    g2emu::Runner::Options ro;
    ro.machine = opt;
    if(!ro.machine.threads) ro.machine.threads = 1;
    g2emu::Runner runner(fw, ro);
    proto::LocalLink link(std::make_unique<g2emu::MachineTransport>(runner.machine()));
    Listener listener;
    link.setListener(&listener);
    std::vector<float> block(960 * 4), all;
    auto next = Clock::now();
    const auto t0 = next;
    std::uint64_t audioFrames = 0;
    auto audio = [&](bool keep) {
        // one 10 ms callback, then the editor's ticks until the next one
        runner.read(block.data(), 960);
        audioFrames += 960;
        if(keep) all.insert(all.end(), block.begin(), block.end());
        for(int k = 0; k < 10; ++k)
        {
            link.tick();
            next += std::chrono::milliseconds(1);
            std::this_thread::sleep_until(next);
        }
    };
    runner.machine().plugUsb();
    while(!link.synced() && Clock::now() - t0 < std::chrono::seconds(20)) audio(false);
    if(!link.synced()) { std::fprintf(stderr, "no sync\n"); return 1; }
    std::fprintf(stderr, "synced after %.2f s wall (machine at %.2f s)\n",
                 std::chrono::duration<double>(Clock::now() - t0).count(), runner.machine().seconds());
    if(!patchPath.empty())
    {
        const auto patch = Patch::fromFile(file::read(readFile(patchPath)));
        if(keyboard)
        {
            Performance perf;
            for(auto& p : perf.slots) p = Patch::makeDefault();
            perf.slots[0] = patch;
            for(int i = 0; i < 4; ++i)
            {
                perf.header.slots[std::size_t(i)].enabled = i == 0;
                perf.header.slots[std::size_t(i)].keyboard = i == 0;
            }
            link.sendPerformance(perf, "Test");
        }
        else
            link.sendPatch(0, patch, "Test");
        for(int i = 0; i < 100; ++i) audio(false);  // the upload, then a second of settling
    }
    const auto missing0 = runner.stats().framesMissing;
    std::size_t ni = 0;
    const std::uint64_t f0 = audioFrames;
    while(double(audioFrames - f0) / 96000.0 < seconds)
    {
        const double t = double(audioFrames - f0) / 96000.0;
        while(ni < notes.size() && notes[ni].at <= t)
        {
            const auto& e = notes[ni++];
            if(e.midi)
            {
                const std::uint8_t b[3] = {std::uint8_t((e.on ? 0x90 : 0x80) | e.channel), std::uint8_t(e.note), std::uint8_t(e.on ? 100 : 64)};
                runner.midiIn(b);
            }
            else
                link.playNote(std::uint8_t(e.note), e.on);
        }
        audio(true);
    }
    const auto st = runner.stats();
    std::vector<float> ch(all.size() / 4);
    for(std::size_t f = 0; f < ch.size(); ++f) ch[f] = all[f * 4];
    const auto a = analyse(ch);
    std::printf("realtime: %.2f s of audio, %llu frames missing (%.2f%%), emulator speed while catching up %.2fx\n",
                double(ch.size()) / 96000.0, (unsigned long long)(st.framesMissing - missing0),
                100.0 * double(st.framesMissing - missing0) / double(ch.size() ? ch.size() : 1), st.speed);
    std::printf("out 1: peak %.6f rms %.6f freq %.3f Hz, above 10%% of the peak %.4f-%.4f s\n", a.peak, a.rms, a.freq, a.onset, a.end);
    if(!wav.empty())
    {
        std::vector<float> w(all.size());
        const int order[4] = {0, 2, 1, 3};
        for(std::size_t f = 0; f < all.size() / 4; ++f)
            for(int c = 0; c < 4; ++c) w[f * 4 + std::size_t(c)] = all[f * 4 + std::size_t(order[c])];
        writeWav(wav, w, 4, int(g2emu::Machine::FrameRate));
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string fwPath = "original/firmware", wav, json, flashPath;
    std::optional<std::string> patch, kbd;
    std::vector<std::pair<int, std::string>> more;
    double boot = 2.0, settle = 1.0, seconds = 1.0, maxSync = 30, realtime = 0;
    std::vector<NoteEvent> notes;
    g2emu::Machine::Options opt;
    for(int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if(i + 1 >= argc) throw std::runtime_error(a + " needs a value");
            return argv[++i];
        };
        if(a == "--fw") fwPath = next();
        else if(a == "--patch") patch = next();
        else if(a == "--kbd") kbd = next();
        else if(a == "--to")
        {
            const auto v = next();
            more.emplace_back(std::toupper(v[0]) - 'A', v.substr(2));
        }
        else if(a == "--boot") boot = std::stod(next());
        else if(a == "--settle") settle = std::stod(next());
        else if(a == "--seconds") seconds = std::stod(next());
        else if(a == "--max-sync") maxSync = std::stod(next());
        else if(a == "--note") parseNotes(next(), false, notes);
        else if(a == "--midi") parseNotes(next(), true, notes);
        else if(a == "--wav") wav = next();
        else if(a == "--flash") flashPath = next();
        else if(a == "--realtime") realtime = std::stod(next());
        else if(a == "--json") json = next();
        else if(a == "--quantum") opt.quantum = std::uint32_t(std::stoul(next()));
        else if(a == "--ring-prefill") opt.ringPrefill = std::uint32_t(std::stoul(next()));
        else if(a == "--chain-prefill") opt.chainPrefill = std::uint32_t(std::stoul(next()));
        else if(a == "--cf-mhz") opt.cfHz = std::stod(next()) * 1e6;
        else if(a == "--threads") opt.threads = std::stoi(next());
        else if(a == "--skew") opt.skew = std::uint32_t(std::stoul(next()));
        else if(a == "--no-jit") opt.jit = false;
        else if(a == "--no-idle-skip") opt.idleSkip = false;
        else if(a == "--no-poll-skip") opt.pollSkip = false;
        else if(a == "--trace") opt.trace = true;
        else
        {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    std::sort(notes.begin(), notes.end(), [](const NoteEvent& x, const NoteEvent& y) { return x.at < y.at; });

    const auto fw = g2emu::Firmware::load(fwPath);
    if(realtime > 0)
        return runRealtime(fw, opt, kbd ? *kbd : (patch ? *patch : std::string()), bool(kbd), notes, realtime, wav);
    std::fprintf(stderr, "firmware: OS %d.%02d, CODE %zu bytes at %08x\n", fw.version / 100, fw.version % 100,
                 fw.code()->data.size(), fw.code()->address);
    g2emu::Machine m(fw, opt);
    // --flash FILE: the synth's flash (patches, settings) from a previous run, saved again at the end
    if(!flashPath.empty() && std::filesystem::exists(flashPath))
    {
        const auto img = readFile(flashPath);
        if(img.size() == m.flash().size()) std::copy(img.begin(), img.end(), m.flash().begin());
        std::fprintf(stderr, "flash: loaded %s\n", flashPath.c_str());
    }

    std::atomic<bool> done{false};
    std::thread watch;
    if(std::getenv("G2EMU_WATCH"))
        watch = std::thread([&] {
            while(!done)
            {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                const auto s = m.stats();  // racy: debugging only
                std::fprintf(stderr, "watch: cf pc %08x cycles %llu; dsp clocks %llu %llu %llu %llu pcs %06x %06x %06x %06x\n", s.cfPc,
                             (unsigned long long)s.cfCycles, (unsigned long long)(s.dspExecuted[0] + s.dspSkipped[0]),
                             (unsigned long long)(s.dspExecuted[1] + s.dspSkipped[1]), (unsigned long long)(s.dspExecuted[2] + s.dspSkipped[2]),
                             (unsigned long long)(s.dspExecuted[3] + s.dspSkipped[3]), s.dspPc[0], s.dspPc[1], s.dspPc[2], s.dspPc[3]);
            }
        });
    struct Join { std::atomic<bool>& d; std::thread& t; ~Join() { d = true; if(t.joinable()) t.join(); } } join{done, watch};
    const auto wall0 = std::chrono::steady_clock::now();
    const double cpu0 = cpuSeconds();
    auto report = [&](const char* what) {
        const auto s = m.stats();
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        std::fprintf(stderr,
                     "[%7.3f s emulated, %7.3f s wall] %s: cf pc %08x, %llu instr; dsp pc %06x %06x %06x %06x; "
                     "executed/frame %.0f %.0f %.0f %.0f; host cmds %llu; underruns %llu\n",
                     m.seconds(), wall, what, s.cfPc, (unsigned long long)s.cfInstructions, s.dspPc[0], s.dspPc[1],
                     s.dspPc[2], s.dspPc[3], double(s.dspExecuted[0]) / double(m.frame() + 1),
                     double(s.dspExecuted[1]) / double(m.frame() + 1), double(s.dspExecuted[2]) / double(m.frame() + 1),
                     double(s.dspExecuted[3]) / double(m.frame() + 1), (unsigned long long)s.hostCommands,
                     (unsigned long long)s.linkUnderruns);
    };

    // boot
    const std::uint32_t chunk = g2emu::Machine::FrameRate / 1000;  // 1 ms
    while(m.seconds() < boot)
    {
        m.run(chunk);
        static const int progressMs = std::getenv("G2EMU_PROGRESS_MS") ? std::atoi(std::getenv("G2EMU_PROGRESS_MS")) : 100;
        if(m.frame() % (g2emu::Machine::FrameRate / 1000 * std::uint64_t(progressMs)) < chunk) report("boot");
    }
    report("boot done");

    // USB and our protocol client, clocked by emulated time
    std::unique_ptr<proto::Transport> transportPtr = std::make_unique<g2emu::MachineTransport>(m);
    if(const char* log = std::getenv("G2EMU_USBLOG"))  // the protocol traffic, as Synth > Full USB Log writes it
        transportPtr = std::make_unique<proto::LoggingTransport>(std::move(transportPtr), log, proto::LogData::Full);
    auto& transport = *transportPtr;
    proto::ManualClock clock;
    proto::Client client(transport, clock);
    Listener listener;
    client.setListener(&listener);
    m.plugUsb();
    auto step = [&](std::uint32_t frames) {
        m.run(frames);
        clock.set(std::uint64_t(m.seconds() * 1000.0));
        client.tick();
    };
    const double syncStart = m.seconds();
    while(!client.synced())
    {
        step(chunk);
        if(m.seconds() - syncStart > maxSync) { report("no sync"); return 1; }
        if(proto::isFatal(client.status())) { report("connection failed"); return 1; }
    }
    report("synced");

    // uploads
    auto loadPatch = [](const std::string& p) { return Patch::fromFile(file::read(readFile(p))); };
    std::vector<std::pair<int, std::string>> uploads;
    if(kbd)
    {
        Performance perf;
        for(auto& p : perf.slots) p = Patch::makeDefault();
        perf.slots[0] = loadPatch(*kbd);
        perf.header.focusedSlot = 0;
        for(int i = 0; i < 4; ++i)
        {
            perf.header.slots[std::size_t(i)].patchName = i == 0 ? "Test" : "Init";
            perf.header.slots[std::size_t(i)].enabled = i == 0 ? 1 : 0;
            perf.header.slots[std::size_t(i)].keyboard = i == 0 ? 1 : 0;
        }
        client.sendPerformance(perf, "KbdTest");
        std::fprintf(stderr, "sending a performance with %s in slot A, keyboard on\n", kbd->c_str());
        while(!client.idle()) step(chunk);
        report("performance sent");
    }
    if(patch) uploads.emplace_back(0, *patch);
    for(auto& u : more) uploads.push_back(u);
    for(const auto& [slot, path] : uploads)
    {
        const auto name = std::filesystem::path(path).stem().string().substr(0, 16);
        client.sendPatch(slot, loadPatch(path), name);
        while(!client.idle()) step(chunk);
        std::fprintf(stderr, "uploaded %s to slot %c\n", path.c_str(), 'A' + slot);
    }
    report("uploads done");

    const double settleEnd = m.seconds() + settle;
    while(m.seconds() < settleEnd) step(chunk);

    // record
    std::vector<float> out;
    const std::uint64_t f0 = m.frame();
    const auto total = std::uint64_t(seconds * g2emu::Machine::FrameRate);
    std::size_t ni = 0;
    // the OS's tick counter, incremented by its timer 1 handler [C] (0x30001894 adds 1 to 0x3010A13C)
    const std::uint32_t ticks0 = m.cfRead32(0x3010A13C);
    const int leds0 = listener.leds;
    const auto w1 = std::chrono::steady_clock::now();
    const double c1 = cpuSeconds(), t1 = threadCpuSeconds();
    const auto s1 = m.stats();
    while(m.frame() - f0 < total)
    {
        const double tRec = double(m.frame() - f0) / g2emu::Machine::FrameRate;
        while(ni < notes.size() && notes[ni].at <= tRec)
        {
            const auto& e = notes[ni++];
            if(e.midi)
            {
                const std::uint8_t b[3] = {std::uint8_t((e.on ? 0x90 : 0x80) | e.channel), std::uint8_t(e.note), std::uint8_t(e.on ? 100 : 64)};
                m.midiIn(b);
            }
            else
                client.playNote(std::uint8_t(e.note), e.on);
        }
        const auto n = std::uint32_t(std::min<std::uint64_t>(chunk, total - (m.frame() - f0)));
        m.run(n, &out);
        clock.set(std::uint64_t(m.seconds() * 1000.0));
        client.tick();
    }
    const double recWall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w1).count();
    const double recEmulated = double(m.frame() - f0) / g2emu::Machine::FrameRate;
    std::printf("OS time: %.1f timer ticks per emulated second, %.1f LED messages per second\n",
                double(m.cfRead32(0x3010A13C) - ticks0) / recEmulated, double(listener.leds - leds0) / recEmulated);
    const double recCpu = cpuSeconds() - c1, recCfCpu = threadCpuSeconds() - t1;
    const auto s2 = m.stats();
    report("recorded");

    // outputs 1-4: the words per frame are DAC 1 L, DAC 2 L, DAC 1 R, DAC 2 R
    const std::size_t frames = out.size() / 4;
    std::vector<float> wavData(frames * 4);
    std::vector<std::vector<float>> ch(4, std::vector<float>(frames));
    const int order[4] = {0, 2, 1, 3};
    for(std::size_t f = 0; f < frames; ++f)
        for(int c = 0; c < 4; ++c)
        {
            ch[std::size_t(c)][f] = out[f * 4 + std::size_t(order[c])];
            wavData[f * 4 + std::size_t(c)] = ch[std::size_t(c)][f];
        }
    for(int c = 0; c < 4; ++c)
    {
        const auto a = analyse(ch[std::size_t(c)]);
        std::printf("out %d: peak %.6f rms %.6f dc %.7f freq %.3f Hz, above 10%% of the peak %.4f-%.4f s\n", c + 1, a.peak,
                    a.rms, a.dc, a.freq, a.onset, a.end);
    }
    const auto s = m.stats();
    const double emulated = double(frames) / g2emu::Machine::FrameRate;
    std::printf("speed: %.3f emulated s in %.3f wall s = %.2fx real time, %.2f CPU s per emulated s\n", emulated,
                recWall, emulated / recWall, recCpu / emulated);
    std::printf("per thread: ColdFire (and the caller) %.2f CPU s per emulated s", recCfCpu / emulated);
    for(int w = 0; w < 4; ++w)
        if(s2.dspThreadCpuNs[w])
            std::printf("; DSP thread %d %.2f CPU s, of which waiting %.2f s", w,
                        double(s2.dspThreadCpuNs[w] - s1.dspThreadCpuNs[w]) * 1e-9 / emulated,
                        double(s2.dspThreadWaitNs[w] - s1.dspThreadWaitNs[w]) * 1e-9 / emulated);
    std::printf("\n");
    std::printf("host port: %.0f reads/s, %.0f commands/s, %.0f DSP syncs/s; ColdFire time in skipped polls %.1f%%\n",
                double(s.hostReads) / m.seconds(), double(s.hostCommands) / m.seconds(), double(s.dspSyncs) / m.seconds(),
                100.0 * double(s.cfPollSkipped) / double(s.cfCycles));
    std::printf("whole run: %.1f emulated s, %.1f wall s, %.1f CPU s; cf %llu instr (%.1f M/s emulated); midi overruns %llu\n",
                m.seconds(), std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count(),
                cpuSeconds() - cpu0, (unsigned long long)s.cfInstructions, double(s.cfInstructions) / m.seconds() / 1e6,
                (unsigned long long)s.midiOverruns);
    if(!wav.empty()) writeWav(wav, wavData, 4, int(g2emu::Machine::FrameRate));
    if(!flashPath.empty())
    {
        std::ofstream f(flashPath, std::ios::binary);
        f.write(reinterpret_cast<const char*>(m.flash().data()), std::streamsize(m.flash().size()));
    }
    if(!json.empty())
    {
        std::ofstream j(json);
        j << "{\"emulated_s\": " << emulated << ", \"wall_s\": " << recWall << ", \"cpu_s\": " << recCpu << ", \"outputs\": [";
        for(int c = 0; c < 4; ++c)
        {
            const auto a = analyse(ch[std::size_t(c)]);
            j << (c ? ", " : "") << "{\"peak\": " << a.peak << ", \"rms\": " << a.rms << ", \"dc\": " << a.dc
              << ", \"freq_hz\": " << a.freq << "}";
        }
        j << "]}\n";
    }
    return 0;
}
