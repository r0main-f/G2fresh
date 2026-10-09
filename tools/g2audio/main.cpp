// g2audio: renders a G2 patch with the native engine (g2engine) to a WAV, and
// reports the modules it cannot play yet and the time it took.
//
//   g2audio PATCH.pch2 OUT.wav [SECONDS] [note=N]
#include "g2/engine/engine.hpp"
#include "g2/patch.hpp"

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
    int note = -1;
    for (int i = 4; i < argc; ++i)
        if (std::string(argv[i]).rfind("note=", 0) == 0)
            note = std::stoi(std::string(argv[i]).substr(5));
    try {
        const auto patch = g2::loadPatch(bytes, {true});
        g2::engine::PatchEngine engine(patch);
        for (const auto& u : engine.unsupported())
            std::cerr << "no processor yet: " << u << "\n";
        if (note >= 0)
            engine.setKey(note, true);
        const int frames = static_cast<int>(seconds * g2::engine::kSampleRate);
        std::vector<float> outs[4];
        for (auto& o : outs)
            o.assign(static_cast<std::size_t>(frames), 0.0f);
        const auto t0 = std::chrono::steady_clock::now();
        engine.render({outs[0].data(), outs[1].data(), outs[2].data(), outs[3].data()}, frames);
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
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
