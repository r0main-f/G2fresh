#include "PatchDocument.h"

#include "CableLayout.h"

#include <cstring>
#include <stdexcept>

namespace g2ui {

// An undo step holds the edited patch before and after the edit (and, in a
// performance, its slot): patches are small, and snapshots can't get out of
// sync with the model. It also holds what the edit recorded (the original's
// molecules and their inverses), which a synth gets on undo and redo.
class PatchDocument::Step : public juce::UndoableAction {
public:
    Step(PatchDocument& doc, int slot, g2::Patch before, g2::Patch after, g2::live::Edit edit)
        : doc_(doc), slot_(slot), before_(std::move(before)), after_(std::move(after)), edit_(std::move(edit)) {}

    bool perform() override
    {
        doc_.replace(slot_, after_, &edit_, false);
        return true;
    }
    bool undo() override
    {
        doc_.replace(slot_, before_, &edit_, true);
        return true;
    }
    int getSizeInUnits() override { return 1; }

    void setAfter(g2::Patch after) { after_ = std::move(after); }
    void addEdit(const g2::live::Edit& more) { edit_.append(more); }

private:
    PatchDocument& doc_;
    int slot_;
    g2::Patch before_, after_;
    g2::live::Edit edit_;
};

PatchDocument::PatchDocument()
    : patch_(g2::Patch::makeDefault()), undo_(30000, 200)
{
}

void PatchDocument::replace(int slot, const g2::Patch& p, const g2::live::Edit* edit, bool undo)
{
    if (perf_) {
        perf_->slots[static_cast<std::size_t>(slot)] = p;
        slot_ = slot; // show the slot an undo/redo touched
    } else {
        patch_ = p;
    }
    dirty_ = true;
    edited(slot, edit, undo);
    sendChangeMessage();
}

void PatchDocument::resetHistory()
{
    undo_.clearUndoHistory();
    endCoalescing();
    lastStep_ = nullptr;
}

void PatchDocument::setSlot(int slot)
{
    if (!perf_)
        return;
    endCoalescing();
    lastStep_ = nullptr;
    slot_ = juce::jlimit(0, 3, slot);
    perf_->header.focusedSlot = static_cast<std::uint8_t>(slot_);
    variation_ = juce::jlimit(0, g2::kUserVariations - 1, static_cast<int>(patch().header.activeVariation));
    sendChangeMessage();
}

bool PatchDocument::perform(const juce::String& name, const std::function<void(g2::Patch&)>& edit,
                            juce::String* error)
{
    endCoalescing();
    g2::Patch next = patch();
    // The edit works on a copy: whatever it throws leaves the document as it
    // was. It records what a synth gets.
    g2::live::Recorder recorder;
    try {
        edit(next);
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
        return false;
    }
    undo_.beginNewTransaction(name);
    auto* step = new Step(*this, slot_, patch(), std::move(next), recorder.edit());
    if (!undo_.perform(step, name))
        return false;
    lastStep_ = step;
    return true;
}

bool PatchDocument::performCoalesced(const juce::String& key, const std::function<void(g2::Patch&)>& edit)
{
    if (key != coalesceKey_ || !lastStep_ || !undo_.canUndo()) {
        const bool ok = perform(key, edit);
        coalesceKey_ = key;
        return ok;
    }
    // Extend the last step: keep its "before", replace its "after".
    g2::Patch next = patch();
    g2::live::Recorder recorder;
    try {
        edit(next);
    } catch (const std::exception&) {
        return false;
    }
    lastStep_->setAfter(next);
    lastStep_->addEdit(recorder.edit());
    replace(slot_, next, &recorder.edit());
    return true;
}

bool PatchDocument::performLive(const std::function<void(g2::Patch&)>& edit)
{
    g2::Patch next = patch();
    try {
        edit(next);
    } catch (const std::exception&) {
        return false;
    }
    replace(slot_, next);
    return true;
}

bool PatchDocument::performMomentary(const std::function<void(g2::Patch&)>& edit)
{
    g2::Patch next = patch();
    try {
        edit(next);
    } catch (const std::exception&) {
        return false;
    }
    const bool wasDirty = dirty_;
    replace(slot_, next);
    dirty_ = wasDirty;
    // The press must reach the synth before the release: no coalescing.
    sendSynchronousChangeMessage();
    return true;
}

bool PatchDocument::performLiveOnSlot(int slot, const std::function<void(g2::Patch&)>& edit)
{
    if (!perf_ || slot == slot_)
        return performLive(edit);
    if (!juce::isPositiveAndBelow(slot, 4))
        return false;
    g2::Patch next = perf_->slots[static_cast<std::size_t>(slot)];
    try {
        edit(next);
    } catch (const std::exception&) {
        return false;
    }
    perf_->slots[static_cast<std::size_t>(slot)] = std::move(next);
    dirty_ = true;
    edited(slot);
    sendChangeMessage();
    return true;
}

void PatchDocument::loadPatch(const g2::Patch& patch, const juce::String& name)
{
    resetHistory();
    perf_.reset();
    patch_ = patch;
    slot_ = 0;
    file_ = juce::File();
    name_ = juce::String(g2::edit::modularName(name.toStdString()));
    variation_ = juce::jlimit(0, g2::kUserVariations - 1, static_cast<int>(patch_.header.activeVariation));
    dirty_ = false;
    loadReport_.clear();
    convertedFrom_ = 0;
    sendChangeMessage();
}

void PatchDocument::loadPerformance(const g2::Performance& perf, const juce::String& name)
{
    resetHistory();
    perf_ = perf;
    perfName_ = juce::String(g2::edit::modularName(name.toStdString()));
    slot_ = juce::jlimit(0, 3, static_cast<int>(perf_->header.focusedSlot));
    file_ = juce::File();
    variation_ = juce::jlimit(0, g2::kUserVariations - 1, static_cast<int>(patch().header.activeVariation));
    dirty_ = false;
    loadReport_.clear();
    convertedFrom_ = 0;
    sendChangeMessage();
}

bool PatchDocument::undo()
{
    endCoalescing();
    lastStep_ = nullptr;
    return undo_.undo();
}

bool PatchDocument::redo()
{
    endCoalescing();
    lastStep_ = nullptr;
    return undo_.redo();
}

void PatchDocument::setVariation(int v)
{
    // Choosing a variation also leaves the Mutator's audition variation.
    variation_ = juce::jlimit(0, g2::kUserVariations - 1, v);
    // The selected variation is saved with the patch, like in the original editor.
    auto& header = perf_ ? perf_->slots[static_cast<std::size_t>(slot_)].header : patch_.header;
    header.activeVariation = static_cast<std::uint8_t>(variation_);
    edited(slot_);
    sendChangeMessage();
}

void PatchDocument::setFocusedMorphGroup(int group)
{
    group = juce::jlimit(0, g2::kMorphGroups - 1, group);
    if (group == morphGroup_)
        return;
    morphGroup_ = group;
    sendChangeMessage();
}

int PatchDocument::focusedVariation() const
{
    const auto& p = patch();
    if (p.header.activeVariation == g2::kAuditionVariation && g2::edit::hasAuditionVariation(p))
        return g2::kAuditionVariation;
    return variation_;
}

void PatchDocument::setFile(const juce::File& f)
{
    file_ = f;
    if (f != juce::File()) {
        // As the original names a patch or performance after its file
        // (Utils::MakePatchNameFromFileName).
        const auto name = juce::String(g2::edit::modularName(f.getFileNameWithoutExtension().toStdString()));
        (perf_ ? perfName_ : name_) = name;
    }
    edited(slot_);
    sendChangeMessage();
}

void PatchDocument::setPerformanceName(const juce::String& name)
{
    const auto trimmed = juce::String(g2::edit::modularName(name.trim().toStdString())).trim();
    if (trimmed.isEmpty() || trimmed == perfName_)
        return;
    perfName_ = trimmed;
    dirty_ = true;
    sendChangeMessage();
}

void PatchDocument::markSaved()
{
    bool differs = false;
    if (file_ != juce::File()) {
        juce::MemoryBlock onDisk;
        try {
            const auto bytes = saveBytes();
            differs = !file_.loadFileAsData(onDisk) || onDisk.getSize() != bytes.size()
                   || std::memcmp(onDisk.getData(), bytes.data(), bytes.size()) != 0;
            // An old file not saved since it was converted: the document is
            // what the file converts to.
            if (differs && convertedFrom_ != 0 && onDisk.getSize() > 0) {
                const auto* p = static_cast<const std::uint8_t*>(onDisk.getData());
                const auto converted = g2::load(std::vector<std::uint8_t>(p, p + onDisk.getSize()), {true});
                const auto* perf = std::get_if<g2::Performance>(&converted.content);
                differs = (perf ? g2::savePerformance(*perf) : g2::savePatch(std::get<g2::Patch>(converted.content))) != bytes;
            }
        } catch (const std::exception&) {
            differs = true;
        }
    }
    dirty_ = differs;
    sendChangeMessage();
}

juce::String PatchDocument::name() const
{
    if (perf_)
        return fromG2Bytes(perf_->header.slots[static_cast<std::size_t>(slot_)].patchName);
    return name_;
}

void PatchDocument::setName(const juce::String& name)
{
    const auto trimmed = juce::String(g2::edit::modularName(name.trim().toStdString())).trim();
    if (trimmed.isEmpty() || trimmed == this->name())
        return;
    if (perf_)
        perf_->header.slots[static_cast<std::size_t>(slot_)].patchName = trimmed.toStdString();
    else
        name_ = trimmed;
    dirty_ = true;
    edited(slot_);
    sendChangeMessage();
}

void PatchDocument::newPatch()
{
    resetHistory();
    patch_ = g2::Patch::makeDefault();
    perf_.reset();
    slot_ = 0;
    variation_ = 0;
    file_ = juce::File();
    name_ = "New patch";
    dirty_ = false;
    loadReport_.clear();
    convertedFrom_ = 0;
    sendChangeMessage();
}

void PatchDocument::newPerformance()
{
    resetHistory();
    g2::Performance perf;
    for (std::size_t i = 0; i < perf.slots.size(); ++i) {
        perf.slots[i] = g2::Patch::makeDefault();
        auto& slot = perf.header.slots[i];
        slot.patchName = "New patch";
        slot.enabled = 1;
        slot.keyboard = i == 0 ? 1 : 0;
        slot.midiChannel = static_cast<std::uint8_t>(i);
    }
    perf_ = std::move(perf);
    perfName_ = "Performance";
    slot_ = 0;
    variation_ = 0;
    file_ = juce::File();
    dirty_ = false;
    convertedFrom_ = 0;
    sendChangeMessage();
}

void PatchDocument::editPerformance(const std::function<void(g2::file::PerfHeader&)>& edit)
{
    if (!perf_)
        return;
    edit(perf_->header);
    dirty_ = true;
    sendChangeMessage();
}

void PatchDocument::loadBytes(const std::vector<std::uint8_t>& bytes, bool ignoreChecksum)
{
    auto loaded = g2::load(bytes, {ignoreChecksum});
    resetHistory();
    embeddedNameFiltered_ = false;
    convertedFrom_ = loaded.convertedFrom;
    if (auto* perf = std::get_if<g2::Performance>(&loaded.content)) {
        perf_ = std::move(*perf);
        slot_ = juce::jlimit(0, 3, static_cast<int>(perf_->header.focusedSlot));
    } else {
        perf_.reset();
        patch_ = std::move(std::get<g2::Patch>(loaded.content));
        slot_ = 0;
        if (!loaded.embeddedName.empty()) {
            const auto filtered = g2::edit::modularBytes(loaded.embeddedName.substr(0, kMaxNameLength));
            if (filtered != loaded.embeddedName.substr(0, kMaxNameLength))
                embeddedNameFiltered_ = true;
            name_ = juce::String(filtered);
        }
    }
    variation_ = juce::jlimit(0, g2::kUserVariations - 1, static_cast<int>(patch().header.activeVariation));
    dirty_ = false;
    repairLoaded();
    sendChangeMessage();
}

void PatchDocument::repairLoaded()
{
    // As the original loads a file: the controller and morph maps are
    // validated and repaired (CCtrlMap/CMorphMap::ValidateAndRepairMap, "Ctrl
    // assignment problem", "Morph assignment problem") and module names are
    // filtered to the G2 characters (CModule::SetName).
    bool ctrl = false, morph = false, names = false;
    auto fix = [&](g2::Patch& p) {
        ctrl = g2::edit::repairMidiCcs(p) || ctrl;
        morph = g2::edit::repairMorphs(p) || morph;
        names = g2::edit::filterLoadedNames(p) || names;
    };
    if (perf_)
        for (auto& slot : perf_->slots)
            fix(slot);
    else
        fix(patch_);
    if (embeddedNameFiltered_)
        names = true;
    juce::StringArray report;
    if (ctrl)
        report.add("Ctrl assignment problem: invalid MIDI controller assignments were removed");
    if (morph)
        report.add("Morph assignment problem: morphs of parameters the patch does not have were removed");
    if (names)
        report.add("characters a G2 cannot show were replaced by spaces in names");
    loadReport_ = report.joinIntoString("; ");
}

juce::String PatchDocument::layoutJson() const
{
    std::vector<std::pair<int, const g2::Patch*>> patches;
    if (perf_)
        for (int i = 0; i < 4; ++i)
            patches.emplace_back(i, &perf_->slots[static_cast<std::size_t>(i)]);
    else
        patches.emplace_back(0, &patch_);
    return cablelayout::toJson(patches);
}

void PatchDocument::applyLayoutJson(const juce::String& json)
{
    const int shaped = cablelayout::apply(json, [this](int slot) -> g2::Patch* {
        if (perf_)
            return juce::isPositiveAndBelow(slot, 4) ? &perf_->slots[static_cast<std::size_t>(slot)] : nullptr;
        return slot == 0 ? &patch_ : nullptr;
    });
    if (shaped > 0)
        sendChangeMessage();
}

std::vector<std::uint8_t> PatchDocument::saveBytes() const
{
    return perf_ ? g2::savePerformance(*perf_) : g2::savePatch(patch_);
}

} // namespace g2ui
