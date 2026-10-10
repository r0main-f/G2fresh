#include "EmulatedSoundEngine.h"

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
}

int EmulatedSoundEngine::latencySamples() const
{
    // the runner's fixed delay, plus the samples the resampler holds back
    return static_cast<int>(std::lround(runner_->latencyFrames() * hostRate_ / g2emu::Machine::FrameRate)) + 2;
}

EmulatedSoundEngine::~EmulatedSoundEngine()
{
    runner_->stop();
    const auto& flash = runner_->machine().flash();
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
    frames_.reserve(room * 4);
    for (int c = 0; c < 2; ++c) {
        resamplers_[c].reset();
        render96_[c].clear();
        render96_[c].reserve(room * 2);
    }
    stereo_.setSize(2, std::max(1, maxBlock));
}

juce::String EmulatedSoundEngine::status() const
{
    const auto stats = runner_->stats();
    if (stats.framesMissing > 0 && stats.speed > 0 && stats.speed < 1.0)
        return "Emulated G2: this computer runs it at " + juce::String(stats.speed, 2) + "x real time (dropouts)";
    return "Emulated G2";
}

void EmulatedSoundEngine::render(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi)
{
    const int frames = out.getNumSamples();
    const double ratio = g2emu::Machine::FrameRate / hostRate_;

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
        frames_.resize(more * 4); // within the capacity reserved in prepare() for normal blocks
        runner_->read(frames_.data(), more); // silence for what is not there yet (booting, too slow)
        // Words: Out 1, Out 3, Out 2, Out 4 (Machine::run). With the volume knob all the way up, a signal of 1.0 into
        // an Out module gives a word of about -0.125: scaled back to signal units.
        constexpr float toSignal = -8.0f;
        for (std::size_t i = 0; i < more; ++i) {
            render96_[0].push_back(toSignal * frames_[4 * i]);
            render96_[1].push_back(toSignal * frames_[4 * i + 2]);
        }
    }
    if (stereo_.getNumSamples() < frames)
        stereo_.setSize(2, frames, false, false, true);
    int used = 0;
    for (int c = 0; c < 2; ++c)
        used = resamplers_[c].process(ratio, render96_[c].data(), stereo_.getWritePointer(c), frames);
    out.clear();
    if (out.getNumChannels() >= 2) {
        out.copyFrom(0, 0, stereo_, 0, 0, frames);
        out.copyFrom(1, 0, stereo_, 1, 0, frames);
    } else if (out.getNumChannels() == 1) { // mono host: (Out 1 + Out 2) / 2
        out.copyFrom(0, 0, stereo_.getReadPointer(0), frames, 0.5f);
        out.addFrom(0, 0, stereo_.getReadPointer(1), frames, 0.5f);
    }
    for (auto& r : render96_)
        r.erase(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(static_cast<std::size_t>(used), r.size())));
}
