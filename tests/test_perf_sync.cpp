// The performance settings the synth changes (its front panel, another editor) come into the document, as the
// original's window follows 11/29 (audit: synth -> editor performance changes reached the mirror only).
#include <catch2/catch_test_macros.hpp>

#include "PerfSync.h"

using namespace g2;

TEST_CASE("performance sync: what counts as a change, and what the document takes")
{
    auto doc = Performance::makeNew().header;
    auto synth = doc;
    CHECK(g2ui::sameSettings(doc, synth));
    // Slot names and the focus are not settings (27 and 09 carry them).
    synth.slots[1].patchName = "Other";
    synth.focusedSlot = 2;
    CHECK(g2ui::sameSettings(doc, synth));
    synth.masterClockBpm = 133;
    synth.slots[2].enabled = 0;
    synth.slots[1].kbdRangeLower = 40;
    CHECK_FALSE(g2ui::sameSettings(doc, synth));
    // The document takes the synth's settings and keeps its own slot names and focus.
    g2ui::takeSettings(doc, synth);
    CHECK(int(doc.masterClockBpm) == 133);
    CHECK(int(doc.slots[2].enabled) == 0);
    CHECK(int(doc.slots[1].kbdRangeLower) == 40);
    CHECK(doc.slots[1].patchName == "No name");
    CHECK(int(doc.focusedSlot) == 0);
    CHECK(g2ui::sameSettings(doc, synth));
}
