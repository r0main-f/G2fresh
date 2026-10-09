// g2audio: renders a G2 patch with the native engine (g2engine) to a WAV, and
// reports the modules it cannot play yet and the time it took.
//
//   g2audio PATCH.pch2 OUT.wav [SECONDS] [note=N] [key=N@ON-OFF]... [repeat=R]
//
// note=N holds note N from the start; key=N@ON-OFF plays note N from ON to
// OFF seconds (repeatable). repeat=R renders R times (for timing).
#include "g2/engine/engine.hpp"
#include "g2/patch.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: g2audio PATCH.pch2 OUT.wav [SECONDS] [note=N]\n";
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    const double seconds = argc > 3 ? std::stod(argv[3]) : 1.0;
    int note = -1, repeat = 1;
    struct Key { int note; double on, off; };
    std::vector<Key> keys;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("note=", 0) == 0)
            note = std::stoi(a.substr(5));
        else if (a.rfind("repeat=", 0) == 0)
            repeat = std::max(1, std::stoi(a.substr(7)));
        else if (a.rfind("key=", 0) == 0) {
            const auto at = a.find('@'), dash = a.find('-', a.find('@'));
            keys.push_back({std::stoi(a.substr(4, at - 4)), std::stod(a.substr(at + 1, dash - at - 1)),
                            std::stod(a.substr(dash + 1))});
        }
    }
    try {
        const auto patch = g2::loadPatch(bytes, {true});
        const int frames = static_cast<int>(seconds * g2::engine::kSampleRate);
        std::vector<float> outs[4];
        for (auto& o : outs)
            o.assign(static_cast<std::size_t>(frames), 0.0f);
        // Events (sample, note, on) in time order.
        struct Ev { int at, note; bool on; };
        std::vector<Ev> evs;
        for (const auto& k : keys) {
            evs.push_back({static_cast<int>(k.on * g2::engine::kSampleRate), k.note, true});
            evs.push_back({static_cast<int>(k.off * g2::engine::kSampleRate), k.note, false});
        }
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
        double best = 1e30;
        for (int r = 0; r < repeat; ++r) {
            g2::engine::PatchEngine engine(patch);
            if (r == 0)
                for (const auto& u : engine.unsupported())
                    std::cerr << "no processor yet: " << u << "\n";
            if (note >= 0)
                engine.setKey(note, true);
            const auto t0 = std::chrono::steady_clock::now();
            int pos = 0;
            for (const auto& e : evs) {
                const int at = std::clamp(e.at, pos, frames);
                engine.render({outs[0].data() + pos, outs[1].data() + pos, outs[2].data() + pos, outs[3].data() + pos},
                              at - pos);
                pos = at;
                engine.setKey(e.note, e.on);
            }
            engine.render({outs[0].data() + pos, outs[1].data() + pos, outs[2].data() + pos, outs[3].data() + pos},
                          frames - pos);
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        const double wall = best;
        std::printf("%.2f s rendered in %.4f s: %.1fx real time (%.2f%% of one core)\n", seconds, wall, seconds / wall,
                    100.0 * wall / seconds);

        // 4-channel 32-bit float WAV.
        std::ofstream w(argv[2], std::ios::binary);
        auto u32 = [&](std::uint32_t v) { w.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&](std::uint16_t v) { w.write(reinterpret_cast<const char*>(&v), 2); };
        const std::uint32_t data = static_cast<std::uint32_t>(frames) * 4 * 4;
        w.write("RIFF", 4); u32(36 + data); w.write("WAVEfmt ", 8); u32(16); u16(3); u16(4); u32(96000);
        u32(96000 * 16); u16(16); u16(32); w.write("data", 4); u32(data);
        for (int f = 0; f < frames; ++f)
            for (auto& o : outs)
                w.write(reinterpret_cast<const char*>(&o[static_cast<std::size_t>(f)]), 4);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
