#pragma once
// After the toolbar's Perf switch the G2 plays other patches: the performance it recalls, or its patch-mode slots again
// (manual p46). The OS answers the switch (3E) with a performance release (1F), after which the original's editor reads
// the performance and every slot again (CSynth::HandlePerformanceRelease @0011d7ac), so its windows on the synth's slots
// show what the synth now plays; a Local window keeps its patch. G2fresh's document live on the synth does the same:
// once that read is done (SynthSync::synced) it opens the performance, or the patch of the slot the G2 has the focus
// on. A document not live on the synth stays. Plain C++.

namespace g2ui {

constexpr int kOpenPerformance = 4;
constexpr int kOpenNothing = -1;

// What to open after a switch to `perfMode`: kOpenPerformance, a slot (0-3) or kOpenNothing. `wasLive`: the document
// was live on the synth before the switch; `slotFocus`: the slot the synth has the focus on (4 or more: none).
inline int openAfterPerfSwitch(bool perfMode, bool wasLive, int slotFocus)
{
    if (!wasLive)
        return kOpenNothing;
    if (perfMode)
        return kOpenPerformance;
    return slotFocus >= 0 && slotFocus < 4 ? slotFocus : 0;
}

} // namespace g2ui
