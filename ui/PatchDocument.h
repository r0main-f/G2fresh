// The patch or performance being edited, with undo/redo and change
// notifications. Shared by the stand-alone app and the plugin (the processor
// owns it; editors view it).
#pragma once

#include "g2/edit.hpp"
#include "g2/patch.hpp"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <optional>

namespace g2ui {

class PatchDocument : public juce::ChangeBroadcaster {
public:
    PatchDocument();

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

    bool undo();
    bool redo();
    bool canUndo() const { return undo_.canUndo(); }
    bool canRedo() const { return undo_.canRedo(); }

    int variation() const { return variation_; }
    void setVariation(int v);

    // File handling. load* replace the document and clear the undo history.
    void newPatch();
    // Loads a patch or performance, whichever the bytes contain. Throws
    // g2::ChecksumError (unless ignoreChecksum) or g2::FormatError.
    void loadBytes(const std::vector<std::uint8_t>& bytes, bool ignoreChecksum = false);
    std::vector<std::uint8_t> saveBytes() const;
    juce::String fileExtension() const { return perf_ ? ".prf2" : ".pch2"; }
    juce::File file() const { return file_; }
    void setFile(const juce::File& f) { file_ = f; }
    bool isDirty() const { return dirty_; }
    void markSaved() { dirty_ = false; sendChangeMessage(); }

private:
    class Step;
    void replace(int slot, const g2::Patch& p);
    void resetHistory();

    g2::Patch patch_;
    std::optional<g2::Performance> perf_;
    int slot_ = 0;
    juce::UndoManager undo_;
    juce::String coalesceKey_;
    Step* lastStep_ = nullptr; // the step a coalesced edit extends
    int variation_ = 0;
    juce::File file_;
    bool dirty_ = false;
};

} // namespace g2ui
