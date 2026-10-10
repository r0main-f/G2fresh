// g2emurun: the whole emulated G2 from the command line (re/notes/g2-hardware-and-emulation.md §3.9).
//
//   g2emurun [--fw PATH] [--patch FILE | --kbd FILE] [--to B:FILE ...] [--settle S] [--seconds S]
//            [--note N@ON-OFF ...] [--midi N@ON-OFF[:CH] ...] [--wav OUT.wav] [--json OUT.json]
//            [--boot S] [--quantum N] [--no-jit] [--no-idle-skip] [--cf-mhz F] [--trace]
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
#include "g2emu/firmware.hpp"
#include "g2emu/machine.hpp"
#include "g2emu/transport.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
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
    void statusChanged(proto::Status s) override { std::fprintf(stderr, "proto: status %s\n", proto::statusText(s)); }
    void synced() override { std::fprintf(stderr, "proto: synced\n"); }
    void error(std::uint8_t code) override { std::fprintf(stderr, "proto: synth exception %02x\n", code); }
};

} // namespace

int main(int argc, char** argv)
{
    std::string fwPath = "original/firmware", wav, json;
    std::optional<std::string> patch, kbd;
    std::vector<std::pair<int, std::string>> more;
    double boot = 2.0, settle = 1.0, seconds = 1.0, maxSync = 30;
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
        else if(a == "--json") json = next();
        else if(a == "--quantum") opt.quantum = std::uint32_t(std::stoul(next()));
        else if(a == "--ring-prefill") opt.ringPrefill = std::uint32_t(std::stoul(next()));
        else if(a == "--chain-prefill") opt.chainPrefill = std::uint32_t(std::stoul(next()));
        else if(a == "--cf-mhz") opt.cfHz = std::stod(next()) * 1e6;
        else if(a == "--no-jit") opt.jit = false;
        else if(a == "--no-idle-skip") opt.idleSkip = false;
        else if(a == "--trace") opt.trace = true;
        else
        {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    std::sort(notes.begin(), notes.end(), [](const NoteEvent& x, const NoteEvent& y) { return x.at < y.at; });

    const auto fw = g2emu::Firmware::load(fwPath);
    std::fprintf(stderr, "firmware: OS %d.%02d, CODE %zu bytes at %08x\n", fw.version / 100, fw.version % 100,
                 fw.code()->data.size(), fw.code()->address);
    g2emu::Machine m(fw, opt);

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
                     "[%7.3f s emulated, %6.1f s wall] %s: cf pc %08x, %llu instr; dsp pc %06x %06x %06x %06x; "
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
    g2emu::MachineTransport transport(m);
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
    const auto w1 = std::chrono::steady_clock::now();
    const double c1 = cpuSeconds();
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
    const double recCpu = cpuSeconds() - c1;
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
    std::printf("whole run: %.1f emulated s, %.1f wall s, %.1f CPU s; cf %llu instr (%.1f M/s emulated); midi overruns %llu\n",
                m.seconds(), std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count(),
                cpuSeconds() - cpu0, (unsigned long long)s.cfInstructions, double(s.cfInstructions) / m.seconds() / 1e6,
                (unsigned long long)s.midiOverruns);
    if(!wav.empty()) writeWav(wav, wavData, 4, int(g2emu::Machine::FrameRate));
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
