#include "PatchDocument.h"

#include <stdexcept>

namespace g2ui {

// An undo step holds the whole patch before and after the edit: patches are
// small, and snapshots can't get out of sync with the model.
class PatchDocument::Step : public juce::UndoableAction {
public:
    Step(PatchDocument& doc, g2::Patch before, g2::Patch after)
        : doc_(doc), before_(std::move(before)), after_(std::move(after)) {}

    bool perform() override { doc_.replace(after_); return true; }
    bool undo() override { doc_.replace(before_); return true; }
    int getSizeInUnits() override { return 1; }

    void setAfter(g2::Patch after) { after_ = std::move(after); }

private:
    PatchDocument& doc_;
    g2::Patch before_, after_;
};

PatchDocument::PatchDocument()
    : patch_(g2::Patch::makeDefault()), undo_(30000, 200)
{
}

void PatchDocument::replace(const g2::Patch& p)
{
    patch_ = p;
    dirty_ = true;
    sendChangeMessage();
}

bool PatchDocument::perform(const juce::String& name, const std::function<void(g2::Patch&)>& edit,
                            juce::String* error)
{
    endCoalescing();
    g2::Patch next = patch_;
    try {
        edit(next);
    } catch (const std::invalid_argument& e) {
        if (error)
            *error = e.what();
        return false;
    }
    undo_.beginNewTransaction(name);
    auto* step = new Step(*this, patch_, std::move(next));
    if (!undo_.perform(step, name))
        return false;
    lastStep_ = step;
    return true;
}

bool PatchDocument::performCoalesced(const juce::String& key, const std::function<void(g2::Patch&)>& edit)
{
    if (key != coalesceKey_ || !lastStep_ || !undo_.canUndo())
        return [&] {
            const bool ok = perform(key, edit);
            coalesceKey_ = key;
            return ok;
        }();
    // Extend the last step: keep its "before", replace its "after".
    g2::Patch next = patch_;
    try {
        edit(next);
    } catch (const std::invalid_argument&) {
        return false;
    }
    if (lastStep_)
        lastStep_->setAfter(next);
    replace(next);
    return true;
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
    variation_ = juce::jlimit(0, g2::kUserVariations - 1, v);
    // The selected variation is saved with the patch, like in the original editor.
    patch_.header.activeVariation = static_cast<std::uint8_t>(variation_);
    sendChangeMessage();
}

void PatchDocument::newPatch()
{
    undo_.clearUndoHistory();
    endCoalescing();
    lastStep_ = nullptr;
    patch_ = g2::Patch::makeDefault();
    variation_ = 0;
    file_ = juce::File();
    dirty_ = false;
    sendChangeMessage();
}

void PatchDocument::loadBytes(const std::vector<std::uint8_t>& bytes)
{
    g2::Patch p = g2::loadPatch(bytes);
    undo_.clearUndoHistory();
    endCoalescing();
    lastStep_ = nullptr;
    patch_ = std::move(p);
    variation_ = juce::jlimit(0, g2::kUserVariations - 1, static_cast<int>(patch_.header.activeVariation));
    dirty_ = false;
    sendChangeMessage();
}

} // namespace g2ui
