#include "AutomationBank.h"

namespace {

constexpr int kKnobs = g2::kKnobCount;
constexpr int kMorphs = g2::kMorphGroups;

int toValue(float normalised, int max)
{
    return juce::roundToInt(juce::jlimit(0.0f, 1.0f, normalised) * static_cast<float>(max));
}

float toNormalised(int value, int max)
{
    return max > 0 ? static_cast<float>(value) / static_cast<float>(max) : 0.0f;
}

} // namespace

AutomationBank::AutomationBank(juce::AudioProcessor& processor, g2ui::PatchDocument& doc)
    : processor_(processor), doc_(doc), slots_(kKnobs + kMorphs), info_(kKnobs + kMorphs)
{
    for (int i = 0; i < kKnobs + kMorphs; ++i) {
        const bool knob = i < kKnobs;
        const juce::String id = knob ? "knob" + juce::String(i) : "morph" + juce::String(i - kKnobs);
        const juce::String name = knob ? "Knob " + juce::String(g2::edit::knobName(i)) : "Morph " + juce::String(i - kKnobs + 1);
        auto param = std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{id, 1}, name, juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction(
                [this, i](float v, int) { return textFor(i, v); }));
        slots_[static_cast<std::size_t>(i)].param = param.get();
        processor_.addParameter(param.release());
    }
    juce::StringArray variations;
    for (int v = 1; v <= g2::kUserVariations; ++v)
        variations.add(juce::String(v));
    auto variation = std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{"variation", 1}, "Variation",
                                                                  variations, 0);
    variation_ = variation.get();
    processor_.addParameter(variation.release());

    doc_.addChangeListener(this);
    changeListenerCallback(nullptr);
    startTimerHz(30);
}

AutomationBank::~AutomationBank()
{
    stopTimer();
    doc_.removeChangeListener(this);
}

std::optional<g2::edit::Target> AutomationBank::targetOf(int index) const
{
    if (index < kKnobs)
        return g2::edit::knobTarget(doc_.patch(), index);
    return g2::edit::Target{g2::Location::Settings, static_cast<std::uint8_t>(g2::edit::Setting::Morph),
                            static_cast<std::uint8_t>(index - kKnobs)};
}

juce::String AutomationBank::textFor(int index, float normalised) const
{
    // Called from any thread: only the snapshot taken on the message thread.
    const juce::SpinLock::ScopedLockType lock(infoLock_);
    const auto& info = info_[static_cast<std::size_t>(index)];
    if (info.name.isEmpty() || info.max <= 0)
        return "unassigned";
    return info.name + ": " + juce::String(toValue(normalised, info.max));
}

void AutomationBank::refreshInfo()
{
    std::vector<Info> next(info_.size());
    for (int i = 0; i < kKnobs + kMorphs; ++i)
        if (const auto t = targetOf(i))
            next[static_cast<std::size_t>(i)] = {juce::String(g2::edit::targetName(doc_.patch(), *t)),
                                                 g2::edit::targetMax(doc_.patch(), *t)};
    bool changed = false;
    {
        const juce::SpinLock::ScopedLockType lock(infoLock_);
        for (std::size_t i = 0; i < next.size(); ++i)
            changed |= next[i].name != info_[i].name || next[i].max != info_[i].max;
        info_ = std::move(next);
    }
    if (changed)
        processor_.updateHostDisplay(juce::AudioProcessor::ChangeDetails().withParameterInfoChanged(true));
}

void AutomationBank::timerCallback()
{
    // Host -> patch: every parameter the host moved since the last look, in
    // one change of the current variation.
    struct Change {
        g2::edit::Target target;
        int value;
    };
    std::vector<Change> changes;
    for (int i = 0; i < kKnobs + kMorphs; ++i) {
        auto& slot = slots_[static_cast<std::size_t>(i)];
        const float v = slot.param->getValue();
        if (std::abs(v - slot.lastHost) < 1.0e-6f)
            continue;
        slot.lastHost = v;
        if (const auto t = targetOf(i)) {
            const int max = g2::edit::targetMax(doc_.patch(), *t);
            const auto current = g2::edit::targetValue(doc_.patch(), *t, static_cast<std::uint8_t>(doc_.variation()));
            if (max > 0 && current && *current != toValue(v, max))
                changes.push_back({*t, toValue(v, max)});
        }
    }
    if (!changes.empty()) {
        const auto variation = static_cast<std::uint8_t>(doc_.variation());
        doc_.performLive([&](g2::Patch& p) {
            for (const auto& c : changes)
                g2::edit::setTargetValue(p, c.target, variation, static_cast<std::uint8_t>(c.value));
        });
    }
    const int variation = variation_->getIndex();
    if (variation != lastVariation_) {
        lastVariation_ = variation;
        if (variation != doc_.variation())
            doc_.setVariation(variation);
    }
}

void AutomationBank::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // Patch -> host: parameters whose patch value no longer matches what the
    // host has (an edit in the editor, a new patch, another variation).
    refreshInfo();
    const auto variation = static_cast<std::uint8_t>(doc_.variation());
    for (int i = 0; i < kKnobs + kMorphs; ++i) {
        auto& slot = slots_[static_cast<std::size_t>(i)];
        const auto t = targetOf(i);
        if (!t)
            continue;
        const int max = g2::edit::targetMax(doc_.patch(), *t);
        const auto value = g2::edit::targetValue(doc_.patch(), *t, variation);
        if (max <= 0 || !value || toValue(slot.param->getValue(), max) == *value)
            continue; // the host's value already stands for the patch value
        const float n = toNormalised(*value, max);
        slot.lastHost = n;
        slot.param->setValueNotifyingHost(n);
    }
    if (doc_.variation() != variation_->getIndex()) {
        lastVariation_ = doc_.variation();
        *variation_ = doc_.variation();
    }
}
