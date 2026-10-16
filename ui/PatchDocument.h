// The patch or performance being edited, with undo/redo and change
// notifications. Shared by the stand-alone app and the plugin (the processor
// owns it; editors view it).
#pragma once

#include "g2/edit.hpp"
#include "g2/focus.hpp"
#include "g2/live.hpp"
#include "g2/patch.hpp"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <array>
#include <functional>
#include <optional>

namespace g2ui {

class PatchDocument : public juce::ChangeBroadcaster {
public:
    PatchDocument();

    // Told of every change of a slot's patch at once, in order (change
    // messages come later and coalesce): what a synth needs to send each edit
    // as the original does. `edit` is what an undoable step recorded (the
    // original's molecules, g2/live.hpp) and `undo` whether the step is being
    // undone; nullptr for other changes.
    class EditListener {
    public:
        virtual ~EditListener() = default;
        virtual void patchEdited(int slot, const g2::live::Edit* edit, bool undo) = 0;
        // The slot's parameter focus moved (the original sends it as a realtime 2F, CMParamFocusDump).
        virtual void paramFocused(int /*slot*/, const g2::focus::Focus& /*focus*/) {}
    };
    void setEditListener(EditListener* l) { editListener_ = l; }

    // The patch being edited: the whole patch, or the selected slot of a
    // performance.
    const g2::Patch& patch() const { return perf_ ? perf_->slots[static_cast<std::size_t>(slot_)] : patch_; }

    bool isPerformance() const { return perf_.has_value(); }
    const g2::Performance* performance() const { return perf_ ? &*perf_ : nullptr; }
    int slot() const { return slot_; }
    void setSlot(int slot);

    // Applies an edit to the current patch as one undoable step. Returns
    // false (and changes nothing) if the edit throws std::invalid_argument;
    // `error` gets the reason.
    bool perform(const juce::String& name, const std::function<void(g2::Patch&)>& edit,
                 juce::String* error = nullptr);
    // Like perform(), but consecutive calls with the same `coalesceKey`
    // (e.g. one knob drag) merge into a single undo step.
    bool performCoalesced(const juce::String& coalesceKey, const std::function<void(g2::Patch&)>& edit);
    void endCoalescing() { coalesceKey_.clear(); }
    // Applies a change that does not go into the undo history: a value moved
    // by DAW automation, or (later) a knob turned on the synth. Returns false
    // if the edit throws.
    bool performLive(const std::function<void(g2::Patch&)>& edit);
    // The same on a given slot of a performance (the slot shown is unchanged).
    bool performLiveOnSlot(int slot, const std::function<void(g2::Patch&)>& edit);
    // A push button's press or release (g2::edit::isMomentary): not an undo
    // step and not an edit that needs saving (the value goes 1 then back to
    // 0), as the original's CPanel::CtrlRelease. Listeners hear it at once,
    // so the synth gets both the press and the release.
    bool performMomentary(const std::function<void(g2::Patch&)>& edit);
    // The patch of a slot (a patch document has only slot 0).
    const g2::Patch& slotPatch(int slot) const
    {
        return perf_ ? perf_->slots[static_cast<std::size_t>(juce::jlimit(0, 3, slot))] : patch_;
    }

    bool undo();
    bool redo();
    bool canUndo() const { return undo_.canUndo(); }
    bool canRedo() const { return undo_.canRedo(); }

    // The user variation (0..7) last chosen.
    int variation() const { return variation_; }
    void setVariation(int v);
    // The variation shown and edited: variation(), or the Patch Mutator's
    // audition variation (9) while it plays an individual there, as the
    // original focuses it (CMutaSynthData::GetFocusIndividMolecules).
    int focusedVariation() const;
    bool auditioning() const { return focusedVariation() == g2::kAuditionVariation; }
    // The focused morph group (0..7): the one a click on a toolbar morph dial
    // selects (CMorphContainer::Click @00123a84), that Morph assign and the
    // morph drag work on, and whose ranges the knobs show (manual p92). Not
    // saved; group 1 at start.
    int focusedMorphGroup() const { return morphGroup_; }
    void setFocusedMorphGroup(int group);
    // The parameter focus of the patch being edited (g2/focus.hpp): one per patch, shared by its areas and
    // settings, Morph dial 1 at first and after a load. Not an undo step and not saved. A focus whose module
    // goes back to Morph dial 1 without a message, as the original's CPatch::RemoveFocusFrom @000da8bc.
    g2::focus::Focus focus() const { return focus_[static_cast<std::size_t>(perf_ ? slot_ : 0)]; }
    void setFocus(const g2::focus::Focus& f);
    // Hide All (the H button, Space): whether the patch's cables are shown, over its colours' own flags. Per patch, in
    // memory only: not saved, not an undo step, not an edit, shown again after a load (CPatchHeaderData_11 byte 0xd,
    // CPatch::ToggleShowCables @000d6382).
    bool cablesShown() const { return cablesShown_[static_cast<std::size_t>(perf_ ? slot_ : 0)]; }
    void toggleCablesShown();

    // File handling. load* replace the document and clear the undo history.
    void newPatch();
    // A new performance: four empty patches in slots A-D.
    void newPerformance();
    // Changes the performance's header (slot settings, master clock, ...).
    // Not part of the undo history (the original has none for it).
    void editPerformance(const std::function<void(g2::file::PerfHeader&)>& edit);
    // The performance's name (a .prf2 has none: from the file name, or the
    // synth's), at most 16 G2 characters.
    juce::String performanceName() const { return perfName_; }
    void setPerformanceName(const juce::String& name);
    // Loads a patch or performance, whichever the bytes contain. Throws
    // g2::ChecksumError (unless ignoreChecksum) or g2::FormatError.
    void loadBytes(const std::vector<std::uint8_t>& bytes, bool ignoreChecksum = false);
    std::vector<std::uint8_t> saveBytes() const;
    // Replace the document with a patch or performance received from the
    // synth (no file; clears the undo history).
    void loadPatch(const g2::Patch& patch, const juce::String& name);
    void loadPerformance(const g2::Performance& perf, const juce::String& name = "Performance");
    juce::String fileExtension() const { return perf_ ? ".prf2" : ".pch2"; }
    // The cable shapes of every patch (see CableLayout.h), or "" if none.
    juce::String layoutJson() const;
    // Applies saved cable shapes after loading; not an undoable edit.
    void applyLayoutJson(const juce::String& json);
    juce::File file() const { return file_; }
    // Remembers the file the document was loaded from or saved to; a patch
    // takes its name from the file name (that's how the G2 names patches),
    // filtered to the G2 characters (Utils::MakePatchNameFromFileName).
    void setFile(const juce::File& f);
    // What the last load repaired or filtered as the original does (an
    // invalid MIDI controller map, characters outside the G2 set in module
    // names), for a message; empty if nothing.
    juce::String loadReport() const { return loadReport_; }
    // The version an old file was converted from when it was loaded (the
    // original converts it on load: CFileConverter::Convert @00051224), until
    // it is saved; 0 for a current file.
    int convertedFrom() const { return convertedFrom_; }
    // A patch converted on load and not saved since: the original asks before
    // writing it (CPatch::SaveToFile @000da3f4 -> CFilePortUtils::ConvertAlert
    // @000308c4). A performance is saved without asking
    // (CPerformance::SaveToFile @0010190c).
    bool conversionPending() const { return !perf_ && convertedFrom_ != 0; }
    // Saved in the current format: no more question.
    void conversionDone() { convertedFrom_ = 0; }
    // A restored session's document that was converted and not saved yet.
    void setConvertedFrom(int version) { convertedFrom_ = version; }

    // The name shown and edited in the toolbar: the patch's name, or in a
    // performance the current slot's patch name (stored in the file). G2 names
    // have at most 16 characters, from the G2 set (g2::edit::kModularChars:
    // others become spaces, Utils::RemoveNonModularChars).
    static constexpr int kMaxNameLength = 16;
    juce::String name() const;
    void setName(const juce::String& name);
    // A name or label as stored (one byte per character; bytes from files
    // that are not ASCII shown as Latin-1).
    static juce::String fromG2Bytes(const std::string& bytes)
    {
        juce::String s;
        for (const char c : bytes)
            s += static_cast<juce::juce_wchar>(static_cast<unsigned char>(c));
        return s;
    }
    // The characters a G2 name may contain, for text editors' input restrictions.
    static juce::String allowedNameCharacters() { return juce::String(g2::edit::kModularChars.data(), g2::edit::kModularChars.size()); }
    bool isDirty() const { return dirty_; }
    // The document is what its file holds: after a save, or a restored
    // session. A document that differs from its file on disk (a session
    // restored with edits that were never saved) stays marked edited, so
    // unsaved edits never show as saved. Without a file: not edited.
    void markSaved();
    // A restored session that was edited and not saved.
    void markEdited()
    {
        dirty_ = true;
        sendChangeMessage();
    }

private:
    class Step;
    void replace(int slot, const g2::Patch& p, const g2::live::Edit* edit = nullptr, bool undo = false);
    void edited(int slot, const g2::live::Edit* edit = nullptr, bool undo = false)
    {
        if (editListener_)
            editListener_->patchEdited(slot, edit, undo);
    }
    void resetHistory();
    // The original's load-time repairs (CCtrlMap::ValidateAndRepairMap,
    // CModule::SetName) on the loaded patch(es); fills loadReport_.
    void repairLoaded();

    g2::Patch patch_;
    std::optional<g2::Performance> perf_;
    int slot_ = 0;
    juce::UndoManager undo_;
    juce::String coalesceKey_;
    Step* lastStep_ = nullptr; // the step a coalesced edit extends
    int variation_ = 0;
    int morphGroup_ = 0;
    std::array<bool, 4> cablesShown_{true, true, true, true};
    std::array<g2::focus::Focus, 4> focus_{g2::focus::kDefault, g2::focus::kDefault, g2::focus::kDefault,
                                           g2::focus::kDefault};
    juce::File file_;
    juce::String name_ = g2::kNewPatchName; // patch name (performances use slot names)
    juce::String perfName_ = g2::kNewPerformanceName;
    EditListener* editListener_ = nullptr;
    bool dirty_ = false;
    juce::String loadReport_;
    bool embeddedNameFiltered_ = false;
    int convertedFrom_ = 0;
};

} // namespace g2ui
