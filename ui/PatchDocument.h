// The patch being edited, with undo/redo and change notifications. Shared by
// the stand-alone app and the plugin (the processor owns it; editors view it).
#pragma once

#include "g2/edit.hpp"
#include "g2/patch.hpp"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <functional>

namespace g2ui {

class PatchDocument : public juce::ChangeBroadcaster {
public:
    PatchDocument();

    const g2::Patch& patch() const { return patch_; }

    // Applies an edit as one undoable step. Returns false (and changes
    // nothing) if the edit throws std::invalid_argument; `error` gets the reason.
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

    // File handling. load* replace the patch and clear the undo history.
    void newPatch();
    void loadBytes(const std::vector<std::uint8_t>& bytes); // throws g2::FormatError
    std::vector<std::uint8_t> saveBytes() const { return g2::savePatch(patch_); }
    juce::File file() const { return file_; }
    void setFile(const juce::File& f) { file_ = f; }
    bool isDirty() const { return dirty_; }
    void markSaved() { dirty_ = false; sendChangeMessage(); }

private:
    class Step;
    void replace(const g2::Patch& p);

    g2::Patch patch_;
    juce::UndoManager undo_;
    juce::String coalesceKey_;
    Step* lastStep_ = nullptr; // the step a coalesced edit extends
    int variation_ = 0;
    juce::File file_;
    bool dirty_ = false;
};

} // namespace g2ui
