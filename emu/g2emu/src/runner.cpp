#include "g2emu/runner.hpp"

#include "thread.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace g2emu {

struct Runner::Thread {
    BigStackThread thread;
};

Runner::Runner(const Firmware& firmware) : Runner(firmware, Options{}) {}

namespace {
// room for the largest reserve (setBufferMs), the largest read size (setReadSize) or stretch, and a few chunks
std::size_t ringFrames(const Runner::Options& o)
{
    return std::size_t((Runner::MaxBufferMs + o.stretchMs) * Machine::FrameRate / 1000.0) + Runner::MaxReadSize + 8 * o.chunkFrames + 1;
}
} // namespace

Runner::Runner(const Firmware& firmware, Options options)
    : options_(options),
      // the inputs wait for the ADCs over the same stretch, the input latency and one more read
      input_(ringFrames(options) + MaxReadSize + inputLead() + 64, MaxReadSize),
      machine_(std::make_unique<Machine>(firmware, options.machine))
{
    capacity_ = ringFrames(options_);
    ring_.assign(capacity_ * 4, 0.0f);
    stretch_ = std::uint32_t(std::max(0.0, options_.stretchMs) * Machine::FrameRate / 1000.0);
    setBufferMs(options_.bufferMs);
    machine_->setAdcSource(&input_);
    if(!options_.flash.empty())
    {
        machine_->loadMemory(options_.flash);
        options_.flash = {};
    }
    thread_ = std::make_unique<Thread>(Thread{BigStackThread([this] { loop(); })});
}

Runner::~Runner() { stop(); }

void Runner::stop()
{
    quit_ = true;
    consumed_.fetch_add(1, std::memory_order_release);
    consumed_.notify_one();
    thread_.reset();
}

std::size_t Runner::available() const
{
    const auto w = written_.load(std::memory_order_acquire), r = read_.load(std::memory_order_relaxed);
    return w > r ? std::size_t(w - r) : 0;  // the reader may be ahead after a gap
}

std::size_t Runner::read(float* out, std::size_t frames)
{
    const auto r = read_.load(std::memory_order_relaxed);
    const auto w = written_.load(std::memory_order_acquire);
    const auto n = std::min<std::size_t>(frames, w > r ? std::size_t(w - r) : 0);
    for(std::size_t i = 0; i < n; ++i)
        std::memcpy(out + 4 * i, &ring_[((r + i) % capacity_) * 4], 4 * sizeof(float));
    if(n < frames)
    {
        std::memset(out + 4 * n, 0, (frames - n) * 4 * sizeof(float));
        missing_.fetch_add(frames - n, std::memory_order_relaxed);
        shortReads_.fetch_add(1, std::memory_order_relaxed);
    }
    // The ring's timeline is the reader's: frames the machine had not made in time are skipped (heard as a gap) and
    // dropped when they come, so that the frames after the gap, and the MIDI scheduled against them (midiInAt), keep
    // their place in the reader's time. (Counting only the frames read would shift everything after a gap, and a
    // MIDI clock would then reach the G2 too fast.)
    read_.store(r + frames, std::memory_order_release);
    lastRead_ = frames;
    consumed_.fetch_add(1, std::memory_order_release);
    consumed_.notify_one();  // the machine's thread, if it waits for room (a wake-up, no lock)
    return n;
}

InputQueue::InputQueue(std::size_t capacity, std::size_t maxWrite)
    : ring_(capacity * 4, 0.0f), capacity_(capacity), maxWrite_(maxWrite)
{
}

void InputQueue::write(std::uint64_t at, const float* in, std::size_t frames)
{
    frames = std::min(frames, maxWrite_);
    if(end_.load(std::memory_order_relaxed) != at) start_.store(at, std::memory_order_release);  // after a pause
    for(std::size_t i = 0; i < frames; ++i)
        std::memcpy(&ring_[((at + i) % capacity_) * 4], in + 4 * i, 4 * sizeof(float));
    end_.store(at + frames, std::memory_order_release);
}

// The DACs send ring frame q at dacClock + q frames; the ADCs sample this frame while the DACs send ring frame
// `now`, and take the input written `latency` frames before it. In the Runner that one is there: the machine runs
// no further than latencyFrames() ahead of the reader, and A6 no more than inputLead() ahead of the machine.
void InputQueue::adcFrame(std::uint64_t clock, std::int32_t words[4])
{
    const auto e = end_.load(std::memory_order_acquire);
    const auto s = start_.load(std::memory_order_acquire);
    const auto zero = dacClock_.load(std::memory_order_acquire);
    if(e == s || zero == NoClock) return;  // write() is not in use, or the DACs do not run yet
    const std::int64_t ticks = std::int64_t(clock) - zero;
    const std::int64_t per = Machine::ClocksPerFrame;
    const std::int64_t now = ticks >= 0 ? ticks / per : -((-ticks + per - 1) / per);
    const std::int64_t at = now - std::int64_t(latency_.load(std::memory_order_relaxed));
    if(at < std::int64_t(s)) return;  // from before the first write()
    if(at >= std::int64_t(e))
    {
        missing_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if(e - std::uint64_t(at) + maxWrite_ > capacity_) return;  // the machine fell far behind: written over since
    const float* f = &ring_[(std::uint64_t(at) % capacity_) * 4];
    for(int k = 0; k < 4; ++k)
    {
        // 24-bit words; the converters clip at their full scale
        const float v = std::clamp(f[k], -1.0f, 1.0f) * 8388607.0f;
        words[k] = std::int32_t(std::lrintf(v));
    }
}

void Runner::loop()
{
    using Clock = std::chrono::steady_clock;
    if(options_.machine.realtimeThreads) realtime_.store(makeThreadRealtime(0.001, 0.0005), std::memory_order_relaxed);
    std::vector<float> chunk;
    // the speed while running (the rests left out), over about the last tenth of a second of running
    double busyWall = 0;
    std::uint64_t busyFrames = 0;
    bool woke = false, filling = true;
    RealtimeBreath breath;
    while(!quit_.load(std::memory_order_relaxed))
    {
        if(options_.machine.realtimeThreads) breath();
        // The machine fills the ring up to `high` and rests; it starts again when the reader's next block would
        // reach into the reserve (`low`: at once after a block that is longer than the stretch). A few frames short
        // of a mark count as there: the machine does not start for a frame or two (the DACs hand a run's last frame
        // out with the next one). So at rest the ring may hold `slack` frames (0.08 ms) less than the reserve asks.
        constexpr std::size_t slack = 8;
        const std::size_t reserve = buffer_.load(std::memory_order_relaxed), block = readSize_.load(std::memory_order_relaxed);
        const std::size_t low = reserve + block, high = reserve + std::max<std::size_t>(block, stretch_);
        const auto level = available();
        if(level + slack >= high) filling = false;
        else if(level + slack < low) filling = true;
        if(!filling)
        {
            // wait until the reader takes some (it wakes this thread)
            const auto seen = consumed_.load(std::memory_order_acquire);
            if(available() + slack >= low && !quit_.load(std::memory_order_relaxed))
            {
                consumed_.wait(seen, std::memory_order_acquire);
                woke = true;
            }
            continue;
        }
        // No further than `high` ahead of the reader as it was when `level` was taken: MIDI the audio thread puts at
        // latencyFrames() after its place is still ahead of the machine, and is taken below, before the machine runs.
        const auto wanted = std::uint32_t(std::min<std::size_t>(options_.chunkFrames, high - level));
        const auto chunkStart = Clock::now();
        for(auto t = midiTail_.load(std::memory_order_relaxed); t != midiHead_.load(std::memory_order_acquire); ++t)
        {
            const auto& e = midiRing_[t % midiRing_.size()];
            machine_->midiInAt({e.bytes.data(), e.size}, e.frame);
            midiTail_.store(t + 1, std::memory_order_release);
        }
        chunk.clear();
        machine_->run(wanted, &chunk);
        const std::size_t frames = chunk.size() / 4;
        auto w = written_.load(std::memory_order_relaxed);
        // frames the reader has gone past (it read silence there): dropped, the timeline stays the reader's
        const auto r = read_.load(std::memory_order_acquire);
        const std::size_t skip = w < r ? std::min<std::size_t>(frames, std::size_t(r - w)) : 0;
        w += skip;
        for(std::size_t i = skip; i < frames; ++i)
            std::memcpy(&ring_[((w + i - skip) % capacity_) * 4], &chunk[4 * i], 4 * sizeof(float));
        w += frames - skip;
        // MIDI OUT, before the reader may take these frames: each byte to the ring frame the DACs sent as its stop
        // bit ended (ring frame w - 1, the last one handed out, at dacClock(); while the DACs do not run yet, by the
        // machine's frames), plus the lead. What does not fit stays in the machine's queue.
        {
            const std::int64_t per = Machine::ClocksPerFrame;
            const std::int64_t zero = machine_->dacClock() ? std::int64_t(machine_->dacClock()) - (std::int64_t(w) - 1) * per
                                                            : (std::int64_t(machine_->frame()) - std::int64_t(w)) * per;
            Machine::MidiOut sent[32];
            for(;;)
            {
                const auto h = midiOutHead_.load(std::memory_order_relaxed);
                const auto room = midiOutRing_.size() - std::size_t(h - midiOutTail_.load(std::memory_order_acquire));
                const auto n = machine_->takeMidiOut({sent, std::min(room, std::size(sent))});
                if(!n) break;
                for(std::size_t k = 0; k < n; ++k)
                {
                    const std::int64_t at = (std::int64_t(sent[k].clock) - zero) / per + std::int64_t(midiOutLeadFrames());
                    midiOutRing_[(h + k) % midiOutRing_.size()] = {std::uint64_t(std::max<std::int64_t>(0, at)), sent[k].byte};
                }
                midiOutHead_.store(h + n, std::memory_order_release);
            }
        }
        written_.store(w, std::memory_order_release);  // room: it only runs below the target level
        // ring frame i was made at machine frame i + offset (the DACs' pipeline is constant)
        frameOffset_.store(std::int64_t(machine_->frame()) - std::int64_t(w), std::memory_order_release);
        // for the inputs, to the DSP clock: the last frame handed out, ring frame w - 1, left the DACs at dacClock()
        if(frames) input_.setDacClock(std::int64_t(machine_->dacClock()) - std::int64_t(w - 1) * std::int64_t(Machine::ClocksPerFrame));
        busyFrames += frames;
        const auto chunkEnd = Clock::now();
        const double chunkWall = std::chrono::duration<double>(chunkEnd - chunkStart).count();
        if(options_.onChunk) options_.onChunk({chunkStart, chunkEnd, std::uint32_t(frames), woke});
        woke = false;
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
    // the reader takes frame R while the machine makes at most R + the reserve + the read size (or the stretch): it
    // runs until it is exactly that far ahead, and its DACs may be a little further (dacLead)
    return buffer_.load(std::memory_order_relaxed) + std::max(readSize_.load(std::memory_order_relaxed), stretch_) + dacLead();
}

void Runner::midiInAt(std::span<const std::uint8_t> bytes, std::uint32_t offset)
{
    // where the next read() starts, plus the offset, plus the fixed latency: the frame its sound appears at is then
    // that many frames after the reader's position, always the same. Lock-free: the runner's thread hands it on.
    const auto at = std::int64_t(read_.load(std::memory_order_relaxed)) + std::int64_t(offset) + std::int64_t(latencyFrames()) +
                    frameOffset_.load(std::memory_order_acquire);
    // A message goes in whole or not at all: the G2 never gets the first part of a System Exclusive block.
    const std::size_t pieces = (bytes.size() + MidiEvent::MaxBytes - 1) / MidiEvent::MaxBytes;
    if(midiHead_.load(std::memory_order_relaxed) - midiTail_.load(std::memory_order_acquire) + pieces > midiRing_.size())
        return;  // full: dropped
    for(std::size_t k = 0; k < bytes.size(); k += MidiEvent::MaxBytes)
    {
        const auto h = midiHead_.load(std::memory_order_relaxed);
        auto& e = midiRing_[h % midiRing_.size()];
        e.frame = at > 0 ? std::uint64_t(at) : 1;
        e.size = std::uint8_t(std::min<std::size_t>(MidiEvent::MaxBytes, bytes.size() - k));
        std::copy_n(bytes.begin() + std::ptrdiff_t(k), e.size, e.bytes.begin());
        midiHead_.store(h + 1, std::memory_order_release);
    }
}

std::size_t Runner::midiOut(std::span<MidiOut> out)
{
    // the frames of the last read(): [end - lastRead_, end)
    const auto end = read_.load(std::memory_order_relaxed);
    const auto start = end - std::min<std::uint64_t>(end, lastRead_);
    auto t = midiOutTail_.load(std::memory_order_relaxed);
    const auto h = midiOutHead_.load(std::memory_order_acquire);
    std::size_t n = 0;
    for(; n < out.size() && t != h; ++n, ++t)
    {
        const auto& e = midiOutRing_[t % midiOutRing_.size()];
        if(e.frame >= end) break;  // a later read()'s
        if(e.frame < start) midiOutLate_.fetch_add(1, std::memory_order_relaxed);
        out[n] = {std::uint32_t(e.frame > start ? e.frame - start : 0), e.byte};
    }
    midiOutTail_.store(t, std::memory_order_release);
    return n;
}

Runner::Stats Runner::stats() const
{
    Stats s;
    s.framesRead = read_.load();
    s.framesMissing = missing_.load();
    s.shortReads = shortReads_.load();
    s.inputMissing = input_.missing();
    s.midiOutLate = midiOutLate_.load();
    s.speed = speed_.load();
    s.longestChunkMs = longestChunk_.load() * 1e3;
    s.realtime = realtime_.load();
    return s;
}

} // namespace g2emu
