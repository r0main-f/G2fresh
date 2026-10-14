#include "EmulatedSoundEngine.h"

#include "AudioIo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace {

g2emu::Runner::Options runnerOptions(std::vector<std::uint8_t> flash)
{
    g2emu::Runner::Options o;
    o.flash = std::move(flash);
    o.machine.model = g2emu::PanelModel::G2X; // the Live view is a G2X: 61 keys and the two global wheels
    // The DSPs on two threads of their own where the computer has 4 performance cores, else one
    // (Machine::autoThreads): two were faster for every patch measured, at the same CPU time
    o.machine.threads = -1;
    // Its threads at the priority of the host's audio threads (macOS: elsewhere nothing changes), and no less than a
    // stretch of audio per run (AudioIo.h). Beside other busy programs this was the difference between losing all
    // of the audio and losing none (re/notes/audio-path.md §3.4). G2FRESH_EMULATOR_REALTIME=0 leaves them as they
    // were up to 0.10.2 (a user-interactive class), to tell whether it is the cause of a problem.
    o.machine.realtimeThreads = juce::SystemStats::getEnvironmentVariable("G2FRESH_EMULATOR_REALTIME", "1") != "0";
    o.stretchMs = g2io::kStretchMs;
    o.bufferMs = g2io::reserveMs(g2io::kDefaultBuffer);
    return o;
}

} // namespace

std::vector<std::uint8_t> EmulatedSoundEngine::readFlash(const juce::File& file)
{
    juce::MemoryBlock data;
    if (!file.existsAsFile() || !file.loadFileAsData(data))
        return {};
    const auto* p = static_cast<const std::uint8_t*>(data.getData());
    return {p, p + data.getSize()};
}

juce::File EmulatedSoundEngine::defaultFlashFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
#if JUCE_MAC
        .getChildFile("Application Support")
#endif
        .getChildFile("G2fresh")
        .getChildFile("Emulated G2 flash.bin");
}

EmulatedSoundEngine::EmulatedSoundEngine(const g2emu::Firmware& firmware, std::vector<std::uint8_t> flash, juce::File saveTo)
    : saveTo_(std::move(saveTo)), savedFlash_(std::move(flash)),
      runner_(std::make_unique<g2emu::Runner>(firmware, runnerOptions(savedFlash_)))
{
    ++running_;
}

juce::String EmulatedSoundEngine::diagnostics() const
{
    const auto s = runner_->stats();
    return "emulator speed while running " + juce::String(s.speed, 2) + "x, longest chunk "
         + juce::String(s.longestChunkMs, 1) + " ms, DSP threads " + juce::String(runner_->machine().stats().dspThreads)
         + (s.realtime ? " (real-time)" : "") + ", buffer " + juce::String(runner_->bufferMs(), 0) + " ms, "
         + juce::String(static_cast<juce::int64>(dropouts())) + " dropouts"
         + ", buffered " + juce::String(static_cast<int>(runner_->available())) + " frames, input frames late "
         + juce::String(static_cast<juce::int64>(s.inputMissing))
         + "\nMaster Clock display: '" + juce::String(runner_->machine().panel().displays[1].text(0)) + "' / '"
         + juce::String(runner_->machine().panel().displays[1].text(1)) + "'";
}

std::uint64_t EmulatedSoundEngine::dropouts() const
{
    if (!playing_.load(std::memory_order_acquire))
        return 0;
    return runner_->stats().shortReads - shortBefore_.load(std::memory_order_relaxed);
}

void EmulatedSoundEngine::setBuffer(g2io::Buffer buffer)
{
    buffer_ = buffer;
    runner_->setBufferMs(g2io::reserveMs(buffer));
}

int EmulatedSoundEngine::latencySamples(g2io::Buffer buffer) const
{
    const double more = (g2io::reserveMs(buffer) - g2io::reserveMs(buffer_)) * hostRate_ / 1000.0;
    return latencySamples() + static_cast<int>(std::lround(more));
}

int EmulatedSoundEngine::midiLatencySamples() const
{
    // the runner's fixed delay, then the output converter's
    const double toHost = hostRate_ / g2emu::Machine::FrameRate;
    return static_cast<int>(std::lround(static_cast<double>(runner_->latencyFrames() + down_.delay()) * toHost));
}

int EmulatedSoundEngine::latencySamples() const
{
    // An input sample goes through the input converter (its delay, in host samples); its frame reaches the ADCs
    // Runner::inputLatencyFrames() after the DACs send the frame read with it; then the G2's own way to the DACs,
    // and the output converter's delay (in 96 kHz frames).
    const double toHost = hostRate_ / g2emu::Machine::FrameRate;
    return static_cast<int>(std::lround(static_cast<double>(up_.delay())
                                        + static_cast<double>(runner_->inputLatencyFrames() + kAdcToDacFrames + down_.delay()) * toHost));
}

void EmulatedSoundEngine::setMic(bool in1IsMic, float level)
{
    in1Gain_.store(in1IsMic ? g2io::micGain(level) : 1.0f, std::memory_order_relaxed);
}

EmulatedSoundEngine::~EmulatedSoundEngine()
{
    runner_->stop();
    --running_;
    const auto flash = runner_->machine().memorySnapshot();
    if (saveTo_ == juce::File() || flash == savedFlash_)
        return;
    saveTo_.getParentDirectory().createDirectory();
    if (!saveTo_.replaceWithData(flash.data(), flash.size()))
        DBG("G2fresh: cannot save the emulated G2's flash to " << saveTo_.getFullPathName());
}

void EmulatedSoundEngine::prepare(double sampleRate, int maxBlock)
{
    hostRate_ = sampleRate > 0 ? std::round(sampleRate) : 96000.0;
    const auto block = static_cast<std::size_t>(std::max(1, maxBlock));
    down_.prepare(g2emu::Machine::FrameRate, hostRate_, 4, block);
    // the most 96 kHz frames a block of that size takes (one more than the average, depending on where it starts)
    const auto room = static_cast<std::size_t>(std::ceil(static_cast<double>(block) * g2emu::Machine::FrameRate / hostRate_));
    up_.prepare(hostRate_, g2emu::Machine::FrameRate, 4, room);
    // the machine keeps a whole host block ready on top of its buffer (a block takes this many frames at once)
    runner_->setReadSize(static_cast<std::uint32_t>(room));
    frames_.reserve(room * 4);
    inFrames_.reserve(room * 4);
    outs_.setSize(4, static_cast<int>(block));
    for (auto& held : inHost_) {
        held.clear();
        held.reserve(block + 8);
    }
}

juce::String EmulatedSoundEngine::status() const
{
    const auto stats = runner_->stats();
    if (stats.framesMissing > 0 && stats.speed > 0 && stats.speed < 1.0)
        return "Emulated G2: this computer runs it at " + juce::String(stats.speed, 2) + "x real time (dropouts)";
    return "Emulated G2";
}

void EmulatedSoundEngine::render(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi, const juce::AudioBuffer<float>* in)
{
    const int frames = out.getNumSamples();
    constexpr int word[4] = {0, 2, 1, 3}; // the runner's frames: Out (and In) 1, 3, 2, 4

    // The inputs wait at the host's rate, as ADC words (AudioIo.h); In 1 through the microphone preamp if it is the
    // microphone. Taken before `out` is written: a host may give the same memory for both.
    for (int c = 0; c < 4; ++c) {
        const float gain = g2io::kAdcPerUnit * (c == 0 ? in1Gain_.load(std::memory_order_relaxed) : 1.0f);
        auto& held = inHost_[c];
        const auto at = held.size();
        held.resize(at + static_cast<std::size_t>(frames)); // within the capacity reserved in prepare() for normal blocks
        if (in != nullptr && c < in->getNumChannels() && in->getNumSamples() >= frames) {
            const float* from = in->getReadPointer(c);
            for (int i = 0; i < frames; ++i)
                held[at + static_cast<std::size_t>(i)] = gain * from[i];
        } else {
            std::fill(held.begin() + static_cast<std::ptrdiff_t>(at), held.end(), 0.0f);
        }
    }

    // The track's MIDI to the G2's MIDI IN, at its place in the block: where the output converter is at that sample,
    // in 96 kHz frames after the next frame read, plus the runner's fixed latency.
    for (const auto meta : midi) {
        const auto m = meta.getMessage();
        if (m.isSysEx())
            continue;
        const auto at = std::max(0.0, std::round(down_.position(static_cast<std::size_t>(std::max(0, meta.samplePosition)))));
        runner_->midiInAt({m.getRawData(), static_cast<std::size_t>(m.getRawDataSize())}, static_cast<std::uint32_t>(at));
    }

    // The 96 kHz frames this block's samples take: exactly, from where the output converter is.
    const auto more = down_.needs(static_cast<std::size_t>(frames));
    if (more > 0) {
        if (offline_) {
            // A bounce asks faster than real time: wait for the machine (up to 2 s per block).
            for (int i = 0; i < 2000 && runner_->available() < more; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // As many input frames as output frames, for the same stretch of the runner's time: they reach the ADCs a
        // fixed time after the DACs send the frames read here (Runner::write).
        inFrames_.resize(more * 4); // (and frames_) within the capacity reserved in prepare() for normal blocks
        const auto taken = up_.needs(more);
        for (int c = 0; c < 4; ++c) {
            auto& held = inHost_[c];
            if (held.size() < taken) // (never: the two converters keep step)
                held.resize(taken, 0.0f);
            up_.process(c, held.data(), 1, inFrames_.data() + word[c], 4, more);
            held.erase(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(taken));
        }
        up_.advance(more);
        runner_->write(inFrames_.data(), more);
        frames_.resize(more * 4);
        const auto got = runner_->read(frames_.data(), more); // silence for what is not there yet (booting, too slow)
        if (got == more && !playing_.load(std::memory_order_relaxed)) {
            shortBefore_.store(runner_->stats().shortReads, std::memory_order_relaxed);
            playing_.store(true, std::memory_order_release);
        }
    }
    if (outs_.getNumSamples() < frames)
        outs_.setSize(4, frames, false, false, true);
    // Words: Out 1, Out 3, Out 2, Out 4 (Machine::run). With the volume knob all the way up, a signal of 1.0 into
    // an Out module gives a word of about -0.125: scaled back to signal units.
    constexpr float toSignal = -8.0f;
    for (int c = 0; c < 4; ++c) {
        down_.process(c, frames_.data() + word[c], 4, outs_.getWritePointer(c), 1, static_cast<std::size_t>(frames));
        juce::FloatVectorOperations::multiply(outs_.getWritePointer(c), toSignal, frames);
    }
    down_.advance(static_cast<std::size_t>(frames));
    out.clear();
    if (out.getNumChannels() >= 4) { // the second output bus: Out 3/4 on their own
        for (int c = 0; c < 4; ++c)
            out.copyFrom(c, 0, outs_, c, 0, frames);
    } else if (out.getNumChannels() >= 2) { // Out 1/2 (as the G2's headphones), Out 3/4 mixed in if asked
        const bool mix = mixOut34_.load(std::memory_order_relaxed);
        for (int c = 0; c < 2; ++c) {
            out.copyFrom(c, 0, outs_, c, 0, frames);
            if (mix)
                out.addFrom(c, 0, outs_, c + 2, 0, frames);
        }
    } else if (out.getNumChannels() == 1) { // mono host: (Out 1 + Out 2) / 2, and Out 3/4 likewise if asked
        const int last = mixOut34_.load(std::memory_order_relaxed) ? 4 : 2;
        for (int c = 0; c < last; ++c)
            out.addFrom(0, 0, outs_.getReadPointer(c), frames, 0.5f);
    }
}
