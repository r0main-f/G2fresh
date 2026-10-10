#include "NativeSoundEngine.h"

#include <algorithm>
#include <cmath>

namespace {

using g2::Location;

// Same modules (index, type, modes), cables, voices and variation count: a
// change elsewhere is a parameter change.
bool sameStructure(const g2::Patch& a, const g2::Patch& b)
{
    if (a.header.voiceCount != b.header.voiceCount || a.header.monoMode != b.header.monoMode
        || a.variationCount != b.variationCount)
        return false;
    for (const auto loc : {Location::Va, Location::Fx}) {
        const auto& x = a.area(loc);
        const auto& y = b.area(loc);
        if (x.modules.size() != y.modules.size() || !(x.cables == y.cables))
            return false;
        for (const auto& m : x.modules) {
            const auto* o = y.find(m.index);
            if (!o || o->type != m.type || o->modes != m.modes || o->params.size() != m.params.size())
                return false;
        }
    }
    return a.settings.size() == b.settings.size();
}

} // namespace

NativeSoundEngine::NativeSoundEngine()
{
    held_.reserve(128); // no allocation on the audio thread for normal playing
}

NativeSoundEngine::~NativeSoundEngine()
{
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
}

void NativeSoundEngine::prepare(double sampleRate, int maxBlock)
{
    hostRate_ = sampleRate > 0 ? sampleRate : 96000.0;
    const auto room = static_cast<std::size_t>(std::ceil(maxBlock * g2::engine::kSampleRate / hostRate_)) + 64;
    for (int c = 0; c < 2; ++c) {
        resamplers_[c].reset();
        render96_[c].clear();
        render96_[c].reserve(room * 2);
    }
    stereo_.setSize(2, std::max(1, maxBlock));
}

void NativeSoundEngine::collectRetired()
{
    delete retired_.exchange(nullptr);
}

void NativeSoundEngine::setPatch(const g2::Patch& patch, int variation)
{
    collectRetired();
    if (current_ && sameStructure(*current_, patch)) {
        // Parameter changes of the current variation, and variation switches,
        // go to the running engine. An edit in another variation (copy,
        // randomize) is rare: rebuild then, so every variation stays right.
        std::vector<Change> diffs;
        bool otherVariation = false;
        auto diffArea = [&](Location loc, const g2::Area& before, const g2::Area& now) {
            for (const auto& m : now.modules) {
                const auto* o = before.find(m.index);
                for (std::size_t v = 0; v < m.params.size(); ++v)
                    for (std::size_t p = 0; p < m.params[v].size(); ++p)
                        if (m.params[v][p] != o->params[v][p]) {
                            if (static_cast<int>(v) == variation)
                                diffs.push_back({loc, m.index, static_cast<std::uint8_t>(p), m.params[v][p], -1});
                            else
                                otherVariation = true;
                        }
            }
        };
        diffArea(Location::Va, current_->va, patch.va);
        diffArea(Location::Fx, current_->fx, patch.fx);
        for (const auto& s : patch.settings)
            for (const auto& o : current_->settings)
                if (o.index == s.index)
                    for (std::size_t v = 0; v < s.params.size() && v < o.params.size(); ++v)
                        for (std::size_t p = 0; p < s.params[v].size() && p < o.params[v].size(); ++p)
                            if (s.params[v][p] != o.params[v][p]) {
                                if (static_cast<int>(v) == variation)
                                    diffs.push_back({Location::Settings, s.index, static_cast<std::uint8_t>(p), s.params[v][p], -1});
                                else
                                    otherVariation = true;
                            }
        if (!otherVariation && changeFifo_.getFreeSpace() > static_cast<int>(diffs.size()) + 1) {
            if (variation != currentVariation_)
                diffs.insert(diffs.begin(), Change{Location::Va, 0, 0, 0, variation});
            const auto scope = changeFifo_.write(static_cast<int>(diffs.size()));
            for (int i = 0; i < scope.blockSize1; ++i)
                changes_[static_cast<std::size_t>(scope.startIndex1 + i)] = diffs[static_cast<std::size_t>(i)];
            for (int i = 0; i < scope.blockSize2; ++i)
                changes_[static_cast<std::size_t>(scope.startIndex2 + i)] = diffs[static_cast<std::size_t>(scope.blockSize1 + i)];
            current_ = patch;
            currentVariation_ = variation;
            return;
        }
    }
    // A new engine, built here (not on the audio thread).
    g2::Patch copy = patch;
    copy.header.activeVariation = static_cast<std::uint8_t>(std::clamp(variation, 0, g2::kUserVariations - 1));
    auto* built = new g2::engine::PatchEngine(copy);
    unsupported_.clear();
    for (const auto& u : built->unsupported())
        unsupported_.add(juce::String(u));
    delete pending_.exchange(built); // an engine the audio thread never took
    current_ = patch;
    currentVariation_ = variation;
}

juce::String NativeSoundEngine::status() const
{
    if (unsupported_.isEmpty())
        return "Built-in sound";
    return "Built-in sound: " + juce::String(unsupported_.size()) + " module" + (unsupported_.size() > 1 ? "s" : "")
         + " not supported yet (" + unsupported_.joinIntoString(", ") + ")";
}

void NativeSoundEngine::applyMidi(g2::engine::PatchEngine& e, const juce::MidiMessage& m)
{
    if (m.isNoteOn()) {
        e.noteOn(m.getNoteNumber(), m.getVelocity());
        held_.emplace_back(m.getNoteNumber(), m.getVelocity());
    } else if (m.isNoteOff()) {
        e.noteOff(m.getNoteNumber(), m.getVelocity());
        std::erase_if(held_, [&](const auto& h) { return h.first == m.getNoteNumber(); });
    } else if (m.isAllNotesOff() || m.isAllSoundOff()) {
        e.allNotesOff();
        held_.clear();
    }
}

void NativeSoundEngine::render(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi)
{
    // A new engine: take it, give the old one back, replay the held notes.
    if (auto* fresh = pending_.exchange(nullptr)) {
        auto* old = engine_.release();
        engine_.reset(fresh);
        if (old != nullptr)
            delete retired_.exchange(old); // normally empty: the message thread collects it
        changeFifo_.reset(); // changes queued for the old engine are in the new one's patch
        for (const auto& [note, velocity] : held_)
            engine_->noteOn(note, velocity);
    }
    out.clear();
    if (!engine_)
        return;

    {
        const auto scope = changeFifo_.read(changeFifo_.getNumReady());
        auto apply = [&](const Change& c) {
            if (c.variation >= 0)
                engine_->setVariation(static_cast<std::uint8_t>(c.variation));
            else
                engine_->setParam(c.loc, c.module, c.param, c.value);
        };
        for (int i = 0; i < scope.blockSize1; ++i)
            apply(changes_[static_cast<std::size_t>(scope.startIndex1 + i)]);
        for (int i = 0; i < scope.blockSize2; ++i)
            apply(changes_[static_cast<std::size_t>(scope.startIndex2 + i)]);
    }
    // MIDI at the start of the block (up to one block of timing jitter).
    for (const auto meta : midi)
        applyMidi(*engine_, meta.getMessage());

    // Render enough 96 kHz samples, then resample to the host's rate.
    const int frames = out.getNumSamples();
    const double ratio = g2::engine::kSampleRate / hostRate_;
    const auto needed = static_cast<std::size_t>(std::ceil(frames * ratio)) + 8;
    if (render96_[0].size() < needed) {
        const auto have = render96_[0].size();
        const auto more = needed - have;
        for (auto& r : render96_)
            r.resize(needed); // within the capacity reserved in prepare() for normal blocks
        engine_->render({render96_[0].data() + have, render96_[1].data() + have, nullptr, nullptr},
                        static_cast<int>(more));
    }
    // Both channels have the same ratio and input, so the resamplers consume
    // the same number of samples.
    if (stereo_.getNumSamples() < frames)
        stereo_.setSize(2, frames, false, false, true);
    int used = 0;
    for (int c = 0; c < 2; ++c)
        used = resamplers_[c].process(ratio, render96_[c].data(), stereo_.getWritePointer(c), frames);
    if (out.getNumChannels() >= 2) {
        out.copyFrom(0, 0, stereo_, 0, 0, frames);
        out.copyFrom(1, 0, stereo_, 1, 0, frames);
    } else if (out.getNumChannels() == 1) { // mono host: (Out 1 + Out 2) / 2
        out.copyFrom(0, 0, stereo_.getReadPointer(0), frames, 0.5f);
        out.addFrom(0, 0, stereo_.getReadPointer(1), frames, 0.5f);
    }
    // Drop what the resamplers consumed.
    for (auto& r : render96_)
        r.erase(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(static_cast<std::size_t>(used), r.size())));
}
