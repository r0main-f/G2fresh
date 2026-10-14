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
    // ...without its voice expansion board (a deliberate deviation: a G2's polyphony, four DSPs to emulate instead of
    // eight; whether eight hold real time on a typical computer is not measured yet, re/notes §3.12.5)
    o.machine.expansion = g2emu::Expansion::Without;
    // The DSPs on two threads of their own where the computer has 4 performance cores, else one
    // (Machine::autoThreads): two were faster for every patch measured, at the same CPU time
    o.machine.threads = -1;
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
         + ", buffered " + juce::String(static_cast<int>(runner_->available())) + " frames, input frames late "
         + juce::String(static_cast<juce::int64>(s.inputMissing))
         + "\nMaster Clock display: '" + juce::String(runner_->machine().panel().displays[1].text(0)) + "' / '"
         + juce::String(runner_->machine().panel().displays[1].text(1)) + "'";
}

int EmulatedSoundEngine::midiLatencySamples() const
{
    // the runner's fixed delay, plus the samples the resampler holds back
    return static_cast<int>(std::lround(runner_->latencyFrames() * hostRate_ / g2emu::Machine::FrameRate)) + 2;
}

int EmulatedSoundEngine::latencySamples() const
{
    // An input sample waits behind the silence the inputs start with and the two samples the input interpolator holds
    // back; its frame reaches the ADCs Runner::inputLatencyFrames() after the DACs send the frame read with it; then
    // the G2's own way to the DACs, and the two frames the output interpolator holds back.
    const double toHost = hostRate_ / g2emu::Machine::FrameRate;
    return static_cast<int>(std::lround(inHeld_ + 2 + (runner_->inputLatencyFrames() + kAdcToDacFrames + 2) * toHost));
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
    hostRate_ = sampleRate > 0 ? sampleRate : 96000.0;
    const auto room = static_cast<std::size_t>(std::ceil(maxBlock * g2emu::Machine::FrameRate / hostRate_)) + 64;
    // the machine keeps a whole host block ready on top of its buffer (a block takes this many frames at once)
    runner_->setReadSize(static_cast<std::uint32_t>(std::ceil(maxBlock * g2emu::Machine::FrameRate / hostRate_)) + 8);
    frames_.reserve(room * 4);
    for (int c = 0; c < 4; ++c) {
        resamplers_[c].reset();
        render96_[c].clear();
        render96_[c].reserve(room * 2);
    }
    outs_.setSize(4, std::max(1, maxBlock));
    // The inputs start with a little silence: the first block takes 8 frames more than it gives (as render96_ keeps
    // them), and no block may find the inputs short. Part of latencySamples().
    const double toHost = hostRate_ / g2emu::Machine::FrameRate;
    inHeld_ = static_cast<int>(std::ceil(12 * toHost)) + 2;
    for (int c = 0; c < 4; ++c) {
        inResamplers_[c].reset();
        inHost_[c].clear();
        inHost_[c].reserve(static_cast<std::size_t>(std::max(1, maxBlock) + inHeld_) + 64);
        inHost_[c].assign(static_cast<std::size_t>(inHeld_), 0.0f);
    }
    in96_.reserve(room);
    inFrames_.reserve(room * 4);
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
    const double ratio = g2emu::Machine::FrameRate / hostRate_;
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

    // The track's MIDI to the G2's MIDI IN, at its place in the block: 96 kHz frames after the next frame read
    // (the samples already read come first), plus the runner's fixed latency.
    const auto have = static_cast<double>(render96_[0].size());
    for (const auto meta : midi) {
        const auto m = meta.getMessage();
        if (m.isSysEx())
            continue;
        const auto at = std::max(0.0, std::round(meta.samplePosition * ratio - have));
        runner_->midiInAt({m.getRawData(), static_cast<std::size_t>(m.getRawDataSize())}, static_cast<std::uint32_t>(at));
    }

    const auto needed = static_cast<std::size_t>(std::ceil(frames * ratio)) + 8;
    if (render96_[0].size() < needed) {
        const auto more = needed - render96_[0].size();
        if (offline_) {
            // A bounce asks faster than real time: wait for the machine (up to 2 s per block).
            for (int i = 0; i < 2000 && runner_->available() < more; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // As many input frames as output frames, for the same stretch of the runner's time: they reach the ADCs a
        // fixed time after the DACs send the frames read here (Runner::write).
        inFrames_.resize(more * 4); // (these three) within the capacity reserved in prepare() for normal blocks
        in96_.resize(more);
        for (int c = 0; c < 4; ++c) {
            auto& held = inHost_[c];
            const int used = inResamplers_[c].process(1.0 / ratio, held.data(), in96_.data(), static_cast<int>(more),
                                                      static_cast<int>(held.size()), 0);
            held.erase(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(static_cast<std::size_t>(std::max(0, used)), held.size())));
            for (std::size_t i = 0; i < more; ++i)
                inFrames_[4 * i + static_cast<std::size_t>(word[c])] = in96_[i];
        }
        runner_->write(inFrames_.data(), more);
        frames_.resize(more * 4);
        runner_->read(frames_.data(), more); // silence for what is not there yet (booting, too slow)
        // Words: Out 1, Out 3, Out 2, Out 4 (Machine::run). With the volume knob all the way up, a signal of 1.0 into
        // an Out module gives a word of about -0.125: scaled back to signal units.
        constexpr float toSignal = -8.0f;
        for (std::size_t i = 0; i < more; ++i)
            for (int c = 0; c < 4; ++c)
                render96_[c].push_back(toSignal * frames_[4 * i + static_cast<std::size_t>(word[c])]);
    }
    if (outs_.getNumSamples() < frames)
        outs_.setSize(4, frames, false, false, true);
    int used = 0;
    for (int c = 0; c < 4; ++c)
        used = resamplers_[c].process(ratio, render96_[c].data(), outs_.getWritePointer(c), frames);
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
    for (auto& r : render96_)
        r.erase(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(static_cast<std::size_t>(used), r.size())));
}
