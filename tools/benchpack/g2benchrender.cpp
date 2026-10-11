// g2benchrender: plays one patch or performance of the hardware bench pack on the emulated G2 and records it.
//
//   g2benchrender [--fw PATH] [--model g2|g2x|engine] [--settle S] [--seconds S] [--wav OUT.wav] [--json OUT.json] FILE
//
// Boots the user's own G2 OS (--fw: Clavia's .dmg as downloaded, the updater app or its .rsrc; default: $G2_FIRMWARE),
// connects our protocol client over the emulated USB and loads FILE as Clavia's editor would: a .pch2 into slot A
// under its file name, a .prf2 as a performance. After --settle seconds it records the four outputs for --seconds.
//
// Everything runs on one thread (Machine::Options::threads = 0), so two runs give the same samples.
//
// The WAV is 96 kHz, 4 channels (Out 1-4), 32-bit float: the DAC words, full scale 1.0, with the sign G2fresh plays
// them with (the words are inverted: a signal of +1.0 inside the G2 is a word of about -0.125, written as +0.125).
// The JSON holds what the synth reports: the voices it assigned, the patch load, the master clock, the main display,
// the MIDI clock bytes it sent while recording.
#include "g2/file.hpp"
#include "g2/patch.hpp"
#include "g2/proto/client.hpp"
#include "g2emu/firmware.hpp"
#include "g2emu/machine.hpp"
#include "g2emu/transport.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <pthread.h>
#endif

using namespace g2;

namespace {

std::vector<std::uint8_t> readFile(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot read " + p);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// 32-bit float WAV (format 3), interleaved
void writeWav(const std::string& path, const std::vector<float>& data, int channels, int rate)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot write " + path);
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = std::uint32_t(data.size() * 4);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(3);
    u16(std::uint16_t(channels));
    u32(std::uint32_t(rate));
    u32(std::uint32_t(rate * 4 * channels));
    u16(std::uint16_t(4 * channels));
    u16(32);
    f.write("data", 4);
    u32(bytes);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(bytes));
}

std::string jsonString(const std::string& s)
{
    std::string o = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += char(c);
        } else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += char(c);
        }
    }
    return o + "\"";
}

// u14: two 7-bit bytes, most significant first (proto::PatchLoad::counters, re/notes/usb-protocol.md)
int u14(const std::array<u8, 27>& c, std::size_t at)
{
    return (c[at] & 0x7F) << 7 | (c[at + 1] & 0x7F);
}

struct Args {
    std::string fw, file, wav, json;
    double settle = 1.5, seconds = 5;
    g2emu::PanelModel model = g2emu::PanelModel::G2;
    std::string modelName = "g2";
    std::vector<std::pair<g2emu::PanelAnalog, float>> analog; // --analog: set before recording
};

int run(const Args& args)
{
    const auto fw = g2emu::Firmware::load(args.fw);
    const auto loaded = g2::load(readFile(args.file));
    const auto name = std::filesystem::path(args.file).stem().string().substr(0, 16);

    g2emu::Machine::Options opt;
    opt.threads = 0;
    opt.model = args.model;
    g2emu::Machine m(fw, opt);
    const std::uint32_t chunk = g2emu::Machine::FrameRate / 1000; // 1 ms
    while (m.seconds() < 2.0)
        m.run(chunk);

    g2emu::MachineTransport transport(m);
    proto::ManualClock clock;
    proto::Client client(transport, clock);
    m.plugUsb();
    auto step = [&](std::vector<float>* out = nullptr) {
        m.run(chunk, out);
        clock.set(std::uint64_t(m.seconds() * 1000.0));
        client.tick();
    };
    auto wait = [&](auto done, const char* what) {
        const double t0 = m.seconds();
        while (!done()) {
            step();
            if (m.seconds() - t0 > 30 || proto::isFatal(client.status()))
                throw std::runtime_error(std::string("the emulated G2 did not answer: ") + what);
        }
    };
    wait([&] { return client.synced(); }, "sync");
    if (loaded.isPerformance())
        client.sendPerformance(std::get<Performance>(loaded.content), name);
    else
        client.sendPatch(0, std::get<Patch>(loaded.content), name);
    wait([&] { return client.idle(); }, "upload");
    const double settleEnd = m.seconds() + args.settle;
    while (m.seconds() < settleEnd)
        step();
    (void)m.takeMidiOut();
    const auto knobs = m.panel().displays[1].text(0); // after a load: the Patch Settings page, voices above knob 2
    for (const auto& [control, value] : args.analog) {
        m.panelAnalog(control, value);
        for (int i = 0; i < 300; ++i) // the OS reads and smooths the panel's ADC
            step();
    }

    std::vector<float> out;
    const std::uint64_t f0 = m.frame();
    const auto total = std::uint64_t(args.seconds * g2emu::Machine::FrameRate);
    out.reserve(std::size_t(total) * 4 + 64);
    while (m.frame() - f0 < total)
        step(&out);
    const double recorded = double(m.frame() - f0) / g2emu::Machine::FrameRate;
    std::size_t midiClocks = 0, midiBytes = 0;
    for (const auto b : m.takeMidiOut()) {
        ++midiBytes;
        if (b == 0xF8)
            ++midiClocks;
    }

    // outputs 1-4: the words per frame are DAC 1 L, DAC 2 L, DAC 1 R, DAC 2 R
    const std::size_t frames = std::min<std::size_t>(out.size() / 4, std::size_t(total));
    std::vector<float> wav(frames * 4);
    const int order[4] = {0, 2, 1, 3};
    double peak[4] = {}, sum2[4] = {};
    for (std::size_t f = 0; f < frames; ++f)
        for (int c = 0; c < 4; ++c) {
            const float v = -out[f * 4 + std::size_t(order[c])];
            wav[f * 4 + std::size_t(c)] = v;
            peak[c] = std::max(peak[c], double(std::fabs(v)));
            sum2[c] += double(v) * v;
        }
    if (!args.wav.empty())
        writeWav(args.wav, wav, 4, int(g2emu::Machine::FrameRate));

    const auto& st = client.state();
    const auto stats = m.stats();
    const auto panel = m.panel();
    std::string j = "{\n";
    j += " \"file\": " + jsonString(std::filesystem::path(args.file).filename().string()) + ",\n";
    j += " \"kind\": " + std::string(loaded.isPerformance() ? "\"performance\"" : "\"patch\"") + ",\n";
    j += " \"model\": " + jsonString(args.modelName) + ",\n";
    j += " \"os_version\": " + std::to_string(st.version.firmware) + ",\n";
    j += " \"sample_rate\": " + std::to_string(g2emu::Machine::FrameRate) + ",\n";
    j += " \"settle_s\": " + std::to_string(args.settle) + ",\n";
    j += " \"recorded_s\": " + std::to_string(recorded) + ",\n";
    j += " \"frames\": " + std::to_string(frames) + ",\n";
    j += " \"voices\": [" + std::to_string(st.voices[0]) + ", " + std::to_string(st.voices[1]) + ", " + std::to_string(st.voices[2]) + ", "
        + std::to_string(st.voices[3]) + "],\n";
    j += " \"requested_voices\": " + std::to_string(st.slots[0].patch.header.monoMode ? 1 : st.slots[0].patch.header.voiceCount) + ",\n";
    j += " \"master_clock\": {\"bpm\": " + std::to_string(st.clock.bpm) + ", \"flag\": " + std::to_string(st.clock.flag) + "},\n";
    j += " \"midi_out\": {\"bytes\": " + std::to_string(midiBytes) + ", \"clocks\": " + std::to_string(midiClocks) + "},\n";
    j += " \"display\": [" + jsonString(panel.displays[0].text(0)) + ", " + jsonString(panel.displays[0].text(1)) + "],\n";
    j += " \"display_knobs_1_2\": " + jsonString(knobs) + ",\n";
    // 72: cycles of the audio-rate and the control-rate code (u14 each), as the synth reports them for one voice
    for (int area = 0; area < 2; ++area) {
        j += std::string(" \"load_") + (area ? "va" : "fx") + "\": ";
        if (const auto& l = st.slots[0].load[std::size_t(area)]) {
            const int a = u14(l->counters, 0), b = u14(l->counters, 2);
            char buf[160];
            std::snprintf(buf, sizeof buf, "{\"cycles_a\": %d, \"cycles_b\": %d, \"cycles_percent\": %.2f}", a, b, (a + b * 0.25) * 100.0 / 1371.0);
            j += buf;
        } else {
            j += "null";
        }
        j += ",\n";
    }
    j += " \"outputs\": [";
    for (int c = 0; c < 4; ++c) {
        char buf[120];
        std::snprintf(buf, sizeof buf, "%s{\"peak\": %.7f, \"rms\": %.7f}", c ? ", " : "", peak[c], frames ? std::sqrt(sum2[c] / double(frames)) : 0.0);
        j += buf;
    }
    j += "],\n";
    j += " \"emulator\": {\"exceptions\": " + std::to_string(stats.exceptions) + ", \"dsps_wild\": " + std::to_string(stats.dspsWild)
        + ", \"link_underruns\": " + std::to_string(stats.linkUnderruns) + "}\n}\n";
    if (args.json.empty())
        std::fputs(j.c_str(), stdout);
    else
        std::ofstream(args.json) << j;
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Args args;
    if (const char* e = std::getenv("G2_FIRMWARE"))
        args.fw = e;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc)
                    throw std::runtime_error(a + " needs a value");
                return argv[++i];
            };
            if (a == "--fw")
                args.fw = next();
            else if (a == "--settle")
                args.settle = std::stod(next());
            else if (a == "--seconds")
                args.seconds = std::stod(next());
            else if (a == "--wav")
                args.wav = next();
            else if (a == "--json")
                args.json = next();
            else if (a == "--model") {
                args.modelName = next();
                args.model = args.modelName == "g2x" ? g2emu::PanelModel::G2X
                    : args.modelName == "engine" ? g2emu::PanelModel::G2Engine : g2emu::PanelModel::G2;
            } else if (a == "--analog") {
                const auto v = next();
                const auto eq = v.find('=');
                const auto name = v.substr(0, eq);
                const auto control = name == "stick" ? g2emu::PanelAnalog::PitchStick
                    : name == "wheel1" ? g2emu::PanelAnalog::GlobalWheel1
                    : name == "wheel2" ? g2emu::PanelAnalog::GlobalWheel2
                    : name == "mod" ? g2emu::PanelAnalog::ModWheel : g2emu::PanelAnalog::MasterLevel;
                if (eq == std::string::npos || (control == g2emu::PanelAnalog::MasterLevel && name != "master"))
                    throw std::runtime_error("--analog takes stick|wheel1|wheel2|mod|master=VALUE (0..1)");
                args.analog.emplace_back(control, std::stof(v.substr(eq + 1)));
            } else if (!a.empty() && a[0] != '-')
                args.file = a;
            else
                throw std::runtime_error("unknown option " + a);
        }
        if (args.file.empty() || args.fw.empty()) {
            std::fprintf(stderr, "usage: g2benchrender [--fw PATH] [--model g2|g2x|engine] [--settle S] [--seconds S] [--wav OUT.wav] "
                                 "[--json OUT.json] FILE.pch2|FILE.prf2   (--fw or $G2_FIRMWARE: Clavia's OS update .dmg)\n");
            return 2;
        }
#ifdef _WIN32
        return run(args);
#else
        // the DSPs' JIT compiles on this thread and needs a big stack (Machine::Options::threads)
        int result = 1;
        struct Call {
            const Args* args;
            int* result;
        } call{&args, &result};
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64u << 20);
        pthread_t thread;
        const auto entry = [](void* p) -> void* {
            auto* c = static_cast<Call*>(p);
            try {
                *c->result = run(*c->args);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "error: %s\n", e.what());
            }
            return nullptr;
        };
        if (pthread_create(&thread, &attr, entry, &call) != 0)
            return run(args);
        pthread_join(thread, nullptr);
        return result;
#endif
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
