// g2audiopath: measurements of the way between the emulated G2 and the host (re/notes/audio-path.md).
//
//   g2audiopath resample IN.wav OUT.wav RATE
//       IN.wav (32-bit float) through the plugin's sample rate converter (plugin/Source/Resampler.h) to RATE Hz, in
//       blocks of 256 samples as the plugin works. tools/audiopath/aliasing.py measures the result.
//   g2audiopath cpu
//       What the converter costs: four channels each way between 96 kHz and the usual host rates, as a share of
//       one core.
//   g2audiopath response
//       The converter's response each way at the usual rates, measured with tones: the pass band's flatness, the
//       stop band's attenuation, what else comes out (a Markdown table).
//   g2audiopath burn THREADS [rt]
//       Other work for the measurements, until killed: THREADS threads that compute without a pause at an
//       application's priority, or (rt) as a host's audio worker threads, 60 % of every 2.9 ms at the audio priority.
//   g2audiopath runner [--fw PATH] [--patch FILE.pch2] [--slots N] [--chord 57,60,64] [--buffer-ms MS]
//                      [--stretch-ms MS] [--block FRAMES] [--threads N] [--rt 0|1] [--seconds S] [--settle S]
//                      [--inputs] [--name TEXT]
//       (with -DG2_BUILD_EMU=ON) The emulated G2 in real time behind an audio callback, as the plugin runs it
//       (g2emu::Runner): a thread with a host's real-time scheduling takes --block frames (96 kHz) every
//       block/96000 s, while another one is the editor (the link's ticks). --patch goes to slot A (to slots A-D
//       with --slots 4: a performance), --chord is held on MIDI (on channels 1-N). Prints one line of JSON: the
//       frames missing, how far the buffer was drawn down at each callback (the reserve it took), the machine's
//       speed chunk by chunk, how long its thread took to wake, how late the callbacks came.
//       --buffer-ms and --block are the Runner's reserve and the host's block, --stretch-ms the least the machine
//       makes in one run, --rt whether its threads have real-time priority (default: as the plugin runs it, real-time
//       threads and a stretch of plugin/Source/AudioIo.h's kStretchMs; --rt 0 --stretch-ms 0: as 0.10.2 did);
//       --inputs also writes input frames.
//       On macOS run it under `taskpolicy -a` (an application's scheduling, as in a DAW: tools/audiopath/sweep.py).
#include "AudioIo.h"
#include "Resampler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if G2AUDIOPATH_EMU
#include "g2/file.hpp"
#include "g2/patch.hpp"
#include "g2emu/firmware.hpp"
#include "g2emu/runner.hpp"
#include "g2emu/transport.hpp"
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace {

struct Wav {
    int rate = 0, channels = 0;
    std::vector<float> data; // interleaved
};

std::vector<std::uint8_t> readFile(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot read " + p);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// 32-bit float WAV (format 3)
Wav readWav(const std::string& path)
{
    const auto b = readFile(path);
    auto u32 = [&](std::size_t at) { std::uint32_t v; std::memcpy(&v, &b[at], 4); return v; };
    auto u16 = [&](std::size_t at) { std::uint16_t v; std::memcpy(&v, &b[at], 2); return v; };
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(&b[8], "WAVE", 4) != 0)
        throw std::runtime_error(path + " is not a WAV file");
    Wav w;
    for (std::size_t at = 12; at + 8 <= b.size();) {
        const std::size_t size = u32(at + 4);
        if (std::memcmp(&b[at], "fmt ", 4) == 0 && size >= 16) {
            if (u16(at + 8) != 3 || u16(at + 22) != 32)
                throw std::runtime_error(path + ": only 32-bit float WAV files");
            w.channels = u16(at + 10);
            w.rate = static_cast<int>(u32(at + 12));
        } else if (std::memcmp(&b[at], "data", 4) == 0) {
            const std::size_t n = (std::min)(size, b.size() - at - 8) / 4;
            w.data.resize(n);
            std::memcpy(w.data.data(), &b[at + 8], n * 4);
            return w;
        }
        at += 8 + size + (size & 1);
    }
    throw std::runtime_error(path + " has no audio");
}

void writeWav(const std::string& path, const Wav& w)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot write " + path);
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(w.data.size() * 4);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(3);
    u16(static_cast<std::uint16_t>(w.channels));
    u32(static_cast<std::uint32_t>(w.rate));
    u32(static_cast<std::uint32_t>(w.rate * 4 * w.channels));
    u16(static_cast<std::uint16_t>(4 * w.channels));
    u16(32);
    f.write("data", 4);
    u32(bytes);
    f.write(reinterpret_cast<const char*>(w.data.data()), static_cast<std::streamsize>(bytes));
}

int resample(const std::string& in, const std::string& out, double rate)
{
    const Wav src = readWav(in);
    constexpr std::size_t block = 256;
    g2io::Resampler r;
    r.prepare(src.rate, rate, src.channels, block);
    Wav dst;
    dst.rate = static_cast<int>(std::lround(rate));
    dst.channels = src.channels;
    const auto ch = static_cast<std::size_t>(src.channels);
    const std::size_t frames = src.data.size() / ch;
    std::vector<float> one(block * ch);
    for (std::size_t at = 0;;) {
        const std::size_t need = r.needs(block);
        if (at + need > frames)
            break;
        for (std::size_t c = 0; c < ch; ++c)
            r.process(static_cast<int>(c), src.data.data() + at * ch + c, static_cast<std::ptrdiff_t>(ch), one.data() + c,
                      static_cast<std::ptrdiff_t>(ch), block);
        r.advance(block);
        at += need;
        dst.data.insert(dst.data.end(), one.begin(), one.end());
    }
    writeWav(out, dst);
    std::printf("%s: %d Hz -> %d Hz, %zu taps, delay %zu input samples (%.3f ms), %zu frames\n", out.c_str(), src.rate,
                dst.rate, r.taps(), r.delay(), 1000.0 * static_cast<double>(r.delay()) / src.rate, dst.data.size() / ch);
    return 0;
}

// A tone of level 1 at `hz` through the conversion: what comes out at `hz` (if the output can hold it) and the rest
// (aliases, images), as levels in dB.
struct ToneOut {
    double wanted = -300, others = -300;
};

ToneOut tone(double from, double to, double hz)
{
    constexpr double pi = 3.14159265358979323846;
    g2io::Resampler r;
    r.prepare(from, to, 1, 256);
    const auto n = static_cast<std::size_t>(from * 0.2);
    std::vector<float> in(n), out, one(256);
    for (std::size_t i = 0; i < n; ++i)
        in[i] = static_cast<float>(std::sin(2 * pi * hz * static_cast<double>(i) / from));
    for (std::size_t at = 0; at + r.needs(256) <= n;) {
        const std::size_t need = r.needs(256);
        r.process(0, in.data() + at, 1, one.data(), 1, 256);
        r.advance(256);
        at += need;
        out.insert(out.end(), one.begin(), one.end());
    }
    const std::size_t skip = 2000;
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
    const bool fits = hz < to / 2;
    for (std::size_t i = skip; i < out.size() && fits; ++i) {
        const double w = 2 * pi * hz * static_cast<double>(i) / to, s = std::sin(w), c = std::cos(w);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        xs += out[i] * s;
        xc += out[i] * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = fits && det != 0 ? (xs * cc - xc * sc) / det : 0, b = fits && det != 0 ? (xc * ss - xs * sc) / det : 0;
    double rest = 0;
    for (std::size_t i = skip; i < out.size(); ++i) {
        const double w = 2 * pi * hz * static_cast<double>(i) / to;
        const double e = out[i] - a * std::sin(w) - b * std::cos(w);
        rest += e * e;
    }
    auto dB = [](double v) { return 20.0 * std::log10((std::max)(v, 1e-15)); };
    return {dB(std::hypot(a, b)), dB(std::sqrt(2.0 * rest / static_cast<double>(out.size() - skip)))};
}

// The converter's measured response each way at the usual rates: 400 tones per band. Going down, a tone in the stop
// band would fold into the pass band: what is left of it. Going up, a tone in the pass band comes with images of
// itself from the stop band on: what is left of them.
int response()
{
    std::printf("| Conversion | Taps | Delay | Pass band | Deviation there | Level 1 Hz below the lower Nyquist | Stop band from | Left of a tone in the stop band | Left of a pass-band tone's images and aliases |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|\n");
    for (const double host : {44100.0, 48000.0, 88200.0, 176400.0, 192000.0}) {
        for (const bool toHost : {true, false}) {
            const double from = toHost ? 96000.0 : host, to = toHost ? host : 96000.0;
            const double lower = (std::min)(from, to), edge = g2io::Resampler::passband(lower), stop = lower - edge;
            g2io::Resampler r;
            r.prepare(from, to, 1, 256);
            double ripple = 0, images = -300, least = -300;
            constexpr int steps = 400;
            for (int k = 0; k <= steps; ++k) {
                const auto t = tone(from, to, (std::max)(10.0, edge * k / steps));
                ripple = (std::max)(ripple, std::abs(t.wanted));
                images = (std::max)(images, t.others);
            }
            const bool folds = stop < from / 2; // the input can hold tones of the stop band
            for (int k = 0; k < steps && folds; ++k) {
                const auto t = tone(from, to, stop + (from / 2 - stop) * k / steps);
                least = (std::max)(least, (std::max)(t.wanted, t.others));
            }
            const auto nyquist = tone(from, to, lower / 2 - 1.0);
            char folded[32] = "(none in the input)";
            if (folds)
                std::snprintf(folded, sizeof folded, "%.1f dB", least);
            std::printf("| %.0f Hz to %.0f Hz | %zu | %zu samples at %.0f Hz (%.3f ms) | to %.0f Hz | %.4f dB | %.1f dB | %.0f Hz | %s | %.1f dB |\n",
                        from, to, r.taps(), r.delay(), from, 1000.0 * static_cast<double>(r.delay()) / from, edge, ripple,
                        (std::max)(nyquist.wanted, nyquist.others), stop, folded, images);
        }
    }
    return 0;
}

// Four channels of noise each way between 96 kHz and each host rate, 256-sample host blocks, 20 s of audio. The
// time is this thread's CPU time, not the wall's: other programs beside it do not count.
int cpu()
{
    struct Clock {
        static double now() { return static_cast<double>(std::clock()) / CLOCKS_PER_SEC; }
    };
    std::printf("%-10s %-22s %-22s %s\n", "host rate", "96 kHz -> host", "host -> 96 kHz", "both, 4 channels each");
    for (const double host : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 47999.0}) {
        constexpr std::size_t block = 256;
        g2io::Resampler down, up;
        down.prepare(96000.0, host, 4, block);
        up.prepare(host, 96000.0, 4, static_cast<std::size_t>(std::ceil(block * 96000.0 / host)) + 2);
        std::vector<float> g2(4 * (static_cast<std::size_t>(std::ceil(block * 96000.0 / host)) + 4)), hostIn(4 * (block + 4)), out(4 * block), toG2(g2.size());
        std::uint32_t seed = 1;
        auto noise = [&](std::vector<float>& v) {
            for (auto& x : v) {
                seed = seed * 1664525u + 1013904223u;
                x = static_cast<float>(static_cast<std::int32_t>(seed)) / 2147483648.0f;
            }
        };
        noise(g2);
        noise(hostIn);
        const auto blocks = static_cast<std::size_t>(20.0 * host / block);
        double seconds[2] = {0, 0};
        float sink = 0;
        for (int pass = 0; pass < 2; ++pass) {
            const auto t0 = Clock::now();
            for (std::size_t b = 0; b < blocks; ++b) {
                if (pass == 0) {
                    for (int c = 0; c < 4; ++c)
                        down.process(c, g2.data() + c, 4, out.data() + c, 4, block);
                    down.advance(block);
                    sink += out[0];
                } else {
                    // as the plugin: as many 96 kHz frames as the other converter took (here: the average)
                    const std::size_t frames = static_cast<std::size_t>(block * 96000.0 / host);
                    for (int c = 0; c < 4; ++c)
                        up.process(c, hostIn.data() + c, 4, toG2.data() + c, 4, frames);
                    up.advance(frames);
                    sink += toG2[0];
                }
            }
            seconds[pass] = Clock::now() - t0;
        }
        std::printf("%-10.0f %5zu taps %6.2f %%      %5zu taps %6.2f %%      %6.2f %% of one core%s\n", host, down.taps(),
                    100.0 * seconds[0] / 20.0, up.taps(), 100.0 * seconds[1] / 20.0, 100.0 * (seconds[0] + seconds[1]) / 20.0,
                    sink == 12345.0f ? "." : "");
    }
    return 0;
}

// This thread as a host's audio thread (as tools/plugintest): real-time scheduling on macOS.
void makeRealtime(double period)
{
#ifdef __APPLE__
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const auto toAbs = [&](double s) { return static_cast<uint32_t>(s * 1e9 * tb.denom / tb.numer); };
    thread_time_constraint_policy_data_t policy;
    policy.period = toAbs(period);
    policy.computation = toAbs(period * 0.5);
    policy.constraint = toAbs(period * 0.9);
    policy.preemptible = 1;
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#else
    (void) period;
#endif
}

// Other work beside the emulated G2, for the measurements: `threads` threads that compute until the process is
// killed. Plain: without a pause, at the priority of an application's own threads (macOS: the user-interactive
// class, as another emulator's or a busy program's threads). Real-time: as a host's audio worker threads, 60 % of
// every 2.9 ms (128 samples at 44.1 kHz) at the audio priority.
int burn(int threads, bool realtime)
{
    std::vector<std::thread> all;
    for (int t = 0; t < threads; ++t)
        all.emplace_back([realtime] {
            using Clock = std::chrono::steady_clock;
            constexpr double period = 128.0 / 44100.0;
#ifdef __APPLE__
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
            if (realtime)
                makeRealtime(period);
            volatile double x = 1.0;
            auto next = Clock::now();
            for (;;) {
                const auto until = next + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(0.6 * period));
                while (!realtime || Clock::now() < until)
                    for (int i = 0; i < 1000; ++i)
                        x = x * 1.0000001 + 1e-9;
                next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(period));
                std::this_thread::sleep_until(next);
            }
        });
    for (auto& t : all)
        t.join();
    return 0;
}

#if G2AUDIOPATH_EMU

// The value below which `share` of the sorted values lie.
double quantile(const std::vector<double>& sorted, double share)
{
    if (sorted.empty())
        return 0;
    const double at = share * static_cast<double>(sorted.size() - 1);
    return sorted[static_cast<std::size_t>(std::lround(at))];
}

std::string spread(std::vector<double> v, const char* format = "%.3f")
{
    std::sort(v.begin(), v.end());
    std::string s = "{";
    const std::pair<const char*, double> points[] = {{"min", 0.0}, {"p0.1", 0.001}, {"p1", 0.01}, {"p50", 0.5}, {"p99", 0.99},
                                                     {"p99.9", 0.999}, {"max", 1.0}};
    for (const auto& [name, share] : points) {
        char text[64];
        std::snprintf(text, sizeof text, format, quantile(v, share));
        s += std::string(s.size() > 1 ? ", " : "") + "\"" + name + "\": " + text;
    }
    return s + "}";
}

struct RunnerArgs {
    std::string fw, patch, name;
    std::vector<int> chord;
    int slots = 1, threads = -1;
    double bufferMs = 20, stretchMs = g2io::kStretchMs, seconds = 20, settle = 3;  // the plugin's stretch
    std::uint32_t block = 512;
    int realtime = 1;  // as the plugin runs it
    bool inputs = false;
};

int runner(const RunnerArgs& a)
{
    using Clock = std::chrono::steady_clock;
    using namespace g2;
    const auto fw = g2emu::Firmware::load(a.fw);
    g2emu::Runner::Options options;
    options.machine.threads = a.threads;
    options.machine.model = g2emu::PanelModel::G2X;
    options.bufferMs = a.bufferMs;
    if (a.stretchMs >= 0)
        options.stretchMs = a.stretchMs;
    if (a.realtime >= 0)
        options.machine.realtimeThreads = a.realtime != 0;
    const double stretchMs = options.stretchMs;
    const bool realtime = options.machine.realtimeThreads;
    // the machine's chunks, kept on its thread (room for twice the run; no allocation while it runs)
    std::vector<g2emu::Runner::Chunk> chunks;
    chunks.reserve(static_cast<std::size_t>((a.seconds + a.settle + 60) * 2200));
    std::atomic<bool> keep{false};
    options.onChunk = [&](const g2emu::Runner::Chunk& c) {
        if (keep.load(std::memory_order_relaxed) && chunks.size() < chunks.capacity())
            chunks.push_back(c);
    };
    g2emu::Runner r(fw, options);
    r.setReadSize(a.block);
    auto link = g2emu::emulatedG2Link(r.machine());

    // the host's audio thread
    const double period = a.block / 96000.0;
    std::atomic<bool> quit{false};
    struct Callback {
        Clock::time_point at;
        std::uint32_t level, got;
        float lateUs;
    };
    std::vector<Callback> callbacks;
    callbacks.reserve(static_cast<std::size_t>((a.seconds + 5) / period) + 16);
    std::thread audio([&] {
        makeRealtime(period);
        std::vector<float> out(a.block * 4), in(a.block * 4, 0.0f);
        const auto step = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(period));
        auto next = Clock::now();
        std::uint64_t n = 0;
        while (!quit.load(std::memory_order_relaxed)) {
            const auto now = Clock::now();
            const auto level = static_cast<std::uint32_t>(r.available());
            if (a.inputs) {
                for (std::uint32_t i = 0; i < a.block; ++i)
                    in[4 * i] = 0.1f * static_cast<float>(std::sin(2 * 3.14159265358979 * 440.0 * static_cast<double>(n + i) / 96000.0));
                r.write(in.data(), a.block);
            }
            const auto got = static_cast<std::uint32_t>(r.read(out.data(), a.block));
            n += a.block;
            if (keep.load(std::memory_order_relaxed) && callbacks.size() < callbacks.capacity())
                callbacks.push_back({now, level, got, static_cast<float>(std::chrono::duration<double, std::micro>(now - next).count())});
            next += step;
            std::this_thread::sleep_until(next);
        }
    });

    // this thread is the editor
    auto run = [&](double seconds, const auto& until) {
        const auto end = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        while (Clock::now() < end && !until()) {
            link->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return until();
    };
    int result = 0;
    if (!run(20, [&] { return link->synced(); })) {
        std::fprintf(stderr, "the emulated G2 did not answer\n");
        result = 1;
    } else {
        if (!a.patch.empty()) {
            const auto patch = Patch::fromFile(file::read(readFile(a.patch)));
            Performance perf;
            for (auto& p : perf.slots)
                p = Patch::makeDefault();
            for (int i = 0; i < 4; ++i) {
                if (i < a.slots)
                    perf.slots[static_cast<std::size_t>(i)] = patch;
                perf.header.slots[static_cast<std::size_t>(i)].enabled = i < a.slots;
                perf.header.slots[static_cast<std::size_t>(i)].keyboard = i < a.slots;
            }
            link->sendPerformance(perf, "Test");
        }
        run(a.settle, [] { return false; });
        for (int slot = 0; slot < a.slots; ++slot)
            for (const int note : a.chord) {
                const std::uint8_t on[3] = {static_cast<std::uint8_t>(0x90 | slot), static_cast<std::uint8_t>(note), 100};
                r.midiIn(on);
            }
        run(1.0, [] { return false; });
        const auto before = r.stats();
        r.resetLongestChunk();
        keep = true;
        const auto cpu0 = std::clock();
        const auto wall0 = Clock::now();
        run(a.seconds, [] { return false; });
        // the CPU time of all the threads of this process, in cores
        const double cores = static_cast<double>(std::clock() - cpu0) / CLOCKS_PER_SEC / std::chrono::duration<double>(Clock::now() - wall0).count();
        keep = false;
        const auto after = r.stats();
        const auto machine = r.machine().stats();
        quit = true;
        audio.join();

        // the reserve taken at each callback: how far below (the buffer + a block) the level was when it came
        const double target = a.bufferMs * 96.0 + a.block;
        std::vector<double> dip, late, interval;
        std::size_t shortReads = 0;
        for (std::size_t i = 0; i < callbacks.size(); ++i) {
            dip.push_back((target - callbacks[i].level) / 96.0);
            late.push_back(callbacks[i].lateUs / 1000.0);
            if (i > 0)
                interval.push_back(std::chrono::duration<double, std::milli>(callbacks[i].at - callbacks[i - 1].at).count());
            shortReads += callbacks[i].got < a.block;
        }
        // the machine: each chunk's speed, the speed over each refill (a wake-up to the next sleep), the wake-ups
        std::vector<double> speed, refill, refillMs, wake, chunkMs;
        std::size_t c = 0;
        double burstFrames = 0, burstWall = 0;
        Clock::time_point burstStart{};
        bool inBurst = false;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            const double wall = std::chrono::duration<double>(chunks[i].end - chunks[i].start).count();
            if (wall > 0)
                speed.push_back(chunks[i].frames / 96000.0 / wall);
            chunkMs.push_back(wall * 1000.0);
            if (chunks[i].woke) {
                if (inBurst && burstWall > 0) {
                    refill.push_back(burstFrames / 96000.0 / burstWall);
                    refillMs.push_back(burstWall * 1000.0);
                }
                burstFrames = 0;
                burstStart = chunks[i].start;
                inBurst = true;
                // the callback that woke it: the last one before this chunk started
                while (c + 1 < callbacks.size() && callbacks[c + 1].at <= chunks[i].start)
                    ++c;
                if (c < callbacks.size() && callbacks[c].at <= chunks[i].start)
                    wake.push_back(std::chrono::duration<double, std::milli>(chunks[i].start - callbacks[c].at).count());
            }
            burstFrames += chunks[i].frames;
            burstWall = std::chrono::duration<double>(chunks[i].end - burstStart).count();
        }
        std::printf("{\"name\": \"%s\", \"buffer_ms\": %.2f, \"stretch_ms\": %.2f, \"realtime\": %s, \"latency_ms\": %.3f, "
                    "\"block\": %u, \"block_ms\": %.3f, \"threads\": %d, \"slots\": %d, "
                    "\"inputs\": %s, \"seconds\": %.1f, \"callbacks\": %zu, \"frames_missing\": %llu, \"short_reads\": %zu, "
                    "\"input_missing\": %llu, \"speed_now\": %.2f, \"longest_chunk_ms\": %.2f, \"link_underruns\": %llu, \"cpu_cores\": %.2f,\n"
                    " \"reserve_taken_ms\": %s,\n \"callback_late_ms\": %s,\n \"callback_interval_ms\": %s,\n"
                    " \"chunk_speed\": %s,\n \"chunk_ms\": %s,\n \"refill_speed\": %s,\n \"refill_ms\": %s,\n \"wake_ms\": %s}\n",
                    a.name.c_str(), a.bufferMs, stretchMs, realtime && after.realtime ? "true" : realtime ? "\"refused\"" : "false", r.latencyFrames() / 96.0, a.block,
                    period * 1000.0, r.machine().dspThreads(), a.slots, a.inputs ? "true" : "false",
                    a.seconds, callbacks.size(), static_cast<unsigned long long>(after.framesMissing - before.framesMissing), shortReads,
                    static_cast<unsigned long long>(after.inputMissing - before.inputMissing), after.speed, after.longestChunkMs,
                    static_cast<unsigned long long>(machine.linkUnderruns), cores,
                    spread(dip).c_str(), spread(late).c_str(), spread(interval).c_str(), spread(speed, "%.2f").c_str(),
                    spread(chunkMs).c_str(), spread(refill, "%.2f").c_str(), spread(refillMs).c_str(), spread(wake).c_str());
        return 0;
    }
    quit = true;
    audio.join();
    return result;
}

#endif // G2AUDIOPATH_EMU

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "resample" && argc == 5)
            return resample(argv[2], argv[3], std::stod(argv[4]));
        if (mode == "cpu")
            return cpu();
        if (mode == "response")
            return response();
        if (mode == "burn" && argc >= 3)
            return burn(std::atoi(argv[2]), argc > 3 && std::string(argv[3]) == "rt");
#if G2AUDIOPATH_EMU
        if (mode == "runner") {
            RunnerArgs a;
            if (const char* e = std::getenv("G2_FIRMWARE"))
                a.fw = e;
            for (int i = 2; i < argc; ++i) {
                const std::string o = argv[i];
                auto next = [&]() -> std::string {
                    if (i + 1 >= argc)
                        throw std::runtime_error(o + " needs a value");
                    return argv[++i];
                };
                if (o == "--fw")
                    a.fw = next();
                else if (o == "--patch")
                    a.patch = next();
                else if (o == "--name")
                    a.name = next();
                else if (o == "--slots")
                    a.slots = std::clamp(std::stoi(next()), 1, 4);
                else if (o == "--threads")
                    a.threads = std::stoi(next());
                else if (o == "--buffer-ms")
                    a.bufferMs = std::stod(next());
                else if (o == "--stretch-ms")
                    a.stretchMs = std::stod(next());
                else if (o == "--rt")
                    a.realtime = std::stoi(next());
                else if (o == "--block")
                    a.block = static_cast<std::uint32_t>(std::clamp(std::stoi(next()), 8, 16384));
                else if (o == "--seconds")
                    a.seconds = std::stod(next());
                else if (o == "--settle")
                    a.settle = std::stod(next());
                else if (o == "--inputs")
                    a.inputs = true;
                else if (o == "--chord") {
                    const std::string list = next();
                    for (std::size_t at = 0; at < list.size();) {
                        const auto comma = list.find(',', at);
                        a.chord.push_back(std::stoi(list.substr(at, comma - at)));
                        at = comma == std::string::npos ? list.size() : comma + 1;
                    }
                } else
                    throw std::runtime_error("unknown option " + o);
            }
            if (a.fw.empty())
                throw std::runtime_error("no firmware: --fw PATH or G2_FIRMWARE");
            return runner(a);
        }
#endif
        std::fprintf(stderr, "usage: g2audiopath resample IN.wav OUT.wav RATE | cpu | response | burn THREADS [rt] | runner [options]   (see the source header)\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
