#pragma once
// Synth > Live Editing: G2fresh's own item for its binding (SynthSync). Bound, it stops live editing; not bound, with a
// synth, it sends the document again, which starts it: a performance as a performance, a patch to the Emulated G2 as
// it plays at launch (SynthSync::sendPatchAlone), a patch to a G2 to the slot it was last in. The original has no such
// item: its patch windows are on a slot or local (the background menu's Slot A-D and Local). Plain C++.

#include <string>

namespace g2ui {

enum class LiveEditingAction { None, Stop, SendPatch, SendPatchAlone, SendPerformance };

struct LiveEditingItem {
    std::string label;
    bool enabled = true;
    bool ticked = false;
    LiveEditingAction action = LiveEditingAction::None;
    int slot = 0;
};

// `ready`: a synth is connected and synced; `bound`: the document is live on it; `emulated`: it is the Emulated G2;
// `lastSlot`: the synth slot the patch was last on (0..3).
inline LiveEditingItem liveEditingItem(bool ready, bool bound, bool performance, bool emulated, int lastSlot)
{
    if (bound)
        return {"Stop Live Editing", true, true, LiveEditingAction::Stop};
    if (!ready)
        return {"Live Editing", false};
    if (performance)
        return {"Live Editing (sends the performance)", true, false, LiveEditingAction::SendPerformance};
    if (emulated)
        return {"Live Editing (sends the patch)", true, false, LiveEditingAction::SendPatchAlone};
    return {std::string("Live Editing (sends the patch to slot ") + static_cast<char>('A' + lastSlot) + ")", true, false,
            LiveEditingAction::SendPatch, lastSlot};
}

} // namespace g2ui
