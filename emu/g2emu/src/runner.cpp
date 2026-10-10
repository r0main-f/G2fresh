#include "g2emu/runner.hpp"

#include "thread.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace g2emu {

struct Runner::Thread {
    BigStackThread thread;
};

Runner::Runner(const Firmware& firmware) : Runner(firmware, Options{}) {}

Runner::Runner(const Firmware& firmware, Options options)
    : options_(options), machine_(std::make_unique<Machine>(firmware, options.machine))
{
    // room for the buffer plus a few chunks
    capacity_ = std::size_t(options_.bufferMs * Machine::FrameRate / 1000.0) + 8 * options_.chunkFrames + 1;
    ring_.assign(capacity_ * 4, 0.0f);
    thread_ = std::make_unique<Thread>(Thread{BigStackThread([this] { loop(); })});
}

Runner::~Runner()
{
    quit_ = true;
    thread_.reset();
}

std::size_t Runner::available() const
{
    return std::size_t(written_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed));
}

std::size_t Runner::read(float* out, std::size_t frames)
{
    const auto r = read_.load(std::memory_order_relaxed);
    const auto n = std::min<std::size_t>(frames, std::size_t(written_.load(std::memory_order_acquire) - r));
    for(std::size_t i = 0; i < n; ++i)
        std::memcpy(out + 4 * i, &ring_[((r + i) % capacity_) * 4], 4 * sizeof(float));
    if(n < frames)
    {
        std::memset(out + 4 * n, 0, (frames - n) * 4 * sizeof(float));
        missing_.fetch_add(frames - n, std::memory_order_relaxed);
    }
    read_.store(r + n, std::memory_order_release);
    return n;
}

void Runner::loop()
{
    using Clock = std::chrono::steady_clock;
    const auto target = std::size_t(options_.bufferMs * Machine::FrameRate / 1000.0);
    std::vector<float> chunk;
    auto busyStart = Clock::now();
    std::uint64_t busyFrames = 0;
    while(!quit_.load(std::memory_order_relaxed))
    {
        if(available() >= target)
        {
            // full: the reader drains it at the audio rate
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            busyStart = Clock::now();
            busyFrames = 0;
            continue;
        }
        chunk.clear();
        machine_->run(options_.chunkFrames, &chunk);
        const std::size_t frames = chunk.size() / 4;
        const auto w = written_.load(std::memory_order_relaxed);
        for(std::size_t i = 0; i < frames; ++i)
            std::memcpy(&ring_[((w + i) % capacity_) * 4], &chunk[4 * i], 4 * sizeof(float));
        written_.store(w + frames, std::memory_order_release);  // room: it only runs below the target level
        busyFrames += frames;
        const double wall = std::chrono::duration<double>(Clock::now() - busyStart).count();
        if(wall > 0.05) speed_.store(double(busyFrames) / Machine::FrameRate / wall, std::memory_order_relaxed);
    }
}

Runner::Stats Runner::stats() const
{
    Stats s;
    s.framesRead = read_.load();
    s.framesMissing = missing_.load();
    s.speed = speed_.load();
    return s;
}

} // namespace g2emu
