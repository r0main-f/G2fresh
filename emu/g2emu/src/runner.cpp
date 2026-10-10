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
    if(!options_.flash.empty())
    {
        auto& flash = machine_->flash();
        std::copy_n(options_.flash.begin(), std::min(flash.size(), options_.flash.size()), flash.begin());
        options_.flash = {};
    }
    thread_ = std::make_unique<Thread>(Thread{BigStackThread([this] { loop(); })});
}

Runner::~Runner() { stop(); }

void Runner::stop()
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
    // Once full, the machine rests until the reader has taken a quarter of the buffer, then runs until it is full
    // again: a few longer runs rather than one chunk after every short sleep, each of which would start the machine's
    // threads again from their sleep.
    const auto resume = target - std::min(target, std::max<std::size_t>(options_.chunkFrames, target / 4));
    std::vector<float> chunk;
    // the speed while running (the rests left out), over about the last tenth of a second of running
    double busyWall = 0;
    std::uint64_t busyFrames = 0;
    while(!quit_.load(std::memory_order_relaxed))
    {
        if(available() >= target)
        {
            // full: the reader drains it at the audio rate
            while(available() > resume && !quit_.load(std::memory_order_relaxed))
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        const auto chunkStart = Clock::now();
        for(auto t = midiTail_.load(std::memory_order_relaxed); t != midiHead_.load(std::memory_order_acquire); ++t)
        {
            const auto& e = midiRing_[t % midiRing_.size()];
            machine_->midiInAt({e.bytes.data(), e.size}, e.frame);
            midiTail_.store(t + 1, std::memory_order_release);
        }
        chunk.clear();
        machine_->run(options_.chunkFrames, &chunk);
        const std::size_t frames = chunk.size() / 4;
        const auto w = written_.load(std::memory_order_relaxed);
        for(std::size_t i = 0; i < frames; ++i)
            std::memcpy(&ring_[((w + i) % capacity_) * 4], &chunk[4 * i], 4 * sizeof(float));
        written_.store(w + frames, std::memory_order_release);  // room: it only runs below the target level
        // ring frame i was made at machine frame i + offset (the DACs' pipeline is constant)
        frameOffset_.store(std::int64_t(machine_->frame()) - std::int64_t(w + frames), std::memory_order_release);
        busyFrames += frames;
        const double chunkWall = std::chrono::duration<double>(Clock::now() - chunkStart).count();
        busyWall += chunkWall;
        if(chunkWall > longestChunk_.load(std::memory_order_relaxed)) longestChunk_.store(chunkWall, std::memory_order_relaxed);
        if(busyWall > 0.1)
        {
            speed_.store(double(busyFrames) / Machine::FrameRate / busyWall, std::memory_order_relaxed);
            busyWall = 0;
            busyFrames = 0;
        }
    }
}

std::uint32_t Runner::latencyFrames() const
{
    // the reader takes frame R while the machine makes R + bufferMs (it runs in chunks until it is that far ahead)
    return std::uint32_t(options_.bufferMs * Machine::FrameRate / 1000.0) + options_.chunkFrames;
}

void Runner::midiInAt(std::span<const std::uint8_t> bytes, std::uint32_t offset)
{
    // where the next read() starts, plus the offset, plus the fixed latency: the frame its sound appears at is then
    // that many frames after the reader's position, always the same. Lock-free: the runner's thread hands it on.
    const auto at = std::int64_t(read_.load(std::memory_order_relaxed)) + std::int64_t(offset) + std::int64_t(latencyFrames()) +
                    frameOffset_.load(std::memory_order_acquire);
    for(std::size_t k = 0; k < bytes.size(); k += MidiEvent::MaxBytes)
    {
        const auto h = midiHead_.load(std::memory_order_relaxed);
        if(h - midiTail_.load(std::memory_order_acquire) >= midiRing_.size()) return;  // full: dropped
        auto& e = midiRing_[h % midiRing_.size()];
        e.frame = at > 0 ? std::uint64_t(at) : 1;
        e.size = std::uint8_t(std::min<std::size_t>(MidiEvent::MaxBytes, bytes.size() - k));
        std::copy_n(bytes.begin() + std::ptrdiff_t(k), e.size, e.bytes.begin());
        midiHead_.store(h + 1, std::memory_order_release);
    }
}

Runner::Stats Runner::stats() const
{
    Stats s;
    s.framesRead = read_.load();
    s.framesMissing = missing_.load();
    s.speed = speed_.load();
    s.longestChunkMs = longestChunk_.load() * 1e3;
    return s;
}

} // namespace g2emu
