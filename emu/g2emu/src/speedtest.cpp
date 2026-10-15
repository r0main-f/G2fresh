#include "g2emu/speedtest.hpp"

#include "g2/proto/client.hpp"
#include "g2emu/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace g2emu {

SpeedTest measureSpeed(const Firmware& firmware, const Machine::Options& options, const g2::Patch& patch,
                       std::span<const int> notes, double seconds, const std::atomic<bool>* stop)
{
    SpeedTest r;
    auto stopped = [&] { return stop != nullptr && stop->load(std::memory_order_relaxed); };
    if(stopped())
    {
        r.error = "stopped";
        return r;
    }
    Machine m(firmware, options);
    MachineTransport transport{m};
    g2::proto::ManualClock clock;
    g2::proto::Client client{transport, clock};
    constexpr std::uint32_t Chunk = Machine::FrameRate / 1000;  // 1 ms, as the plugin's link ticks
    auto step = [&] {
        m.run(Chunk);
        clock.set(std::uint64_t(m.seconds() * 1000.0));
        client.tick();
    };
    // boot, then the editor connects (Options::usbAfter) and sends the patch as the plugin does
    m.plugUsb();
    while(!client.synced())
    {
        if(stopped()) { r.error = "stopped"; return r; }
        if(m.seconds() > options.usbAfter + 4 || g2::proto::isFatal(client.status())) { r.error = "the G2 OS did not answer"; return r; }
        step();
    }
    client.sendPerformance(g2::Performance::playing(patch, "Speed Test"), "Speed Test");
    for(const double end = m.seconds() + 4; !client.idle();)
    {
        if(stopped()) { r.error = "stopped"; return r; }
        if(m.seconds() > end || g2::proto::isFatal(client.status())) { r.error = "the patch did not load"; return r; }
        step();
    }
    for(const int n : notes)
    {
        const std::uint8_t on[3] = {0x90, std::uint8_t((std::clamp)(n, 0, 127)), 100};
        m.midiIn(on);
    }
    // the voices start and the output ramps up
    for(const double end = m.seconds() + 0.5; m.seconds() < end;)
        step();

    const auto s1 = m.stats();
    std::vector<float> out;
    out.reserve(std::size_t(Chunk) * 4);
    const auto frames = std::uint64_t(seconds * Machine::FrameRate);
    const std::uint64_t f0 = m.frame();
    const auto w0 = std::chrono::steady_clock::now();
    while(m.frame() - f0 < frames)
    {
        if(stopped()) { r.error = "stopped"; return r; }
        out.clear();
        m.run(Chunk, &out);
        clock.set(std::uint64_t(m.seconds() * 1000.0));
        client.tick();
        for(const float v : out)
            r.peak = (std::max)(r.peak, std::fabs(v));
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    const double emulated = double(m.frame() - f0) / Machine::FrameRate;
    const auto s2 = m.stats();
    r.speed = wall > 0 ? emulated / wall : 0.0;
    for(int w = 0; w < Machine::MaxDsps; ++w)
    {
        const double busy = double((s2.dspThreadCpuNs[w] - s1.dspThreadCpuNs[w]) - (s2.dspThreadWaitNs[w] - s1.dspThreadWaitNs[w]));
        r.busiestDspThread = (std::max)(r.busiestDspThread, busy * 1e-9 / emulated);
    }
    r.dsps = s2.dsps;
    r.threads = m.dspThreads();
    r.voices = client.state().voices[0];
    return r;
}

} // namespace g2emu
