// After the toolbar's Perf switch (PerfSwitch.h): the original's windows on the synth's slots show what the synth now
// plays, read again after the 1F it answers with (CSynth::HandlePerformanceRelease @0011d7ac).
#include <catch2/catch_test_macros.hpp>

#include "PerfSwitch.h"

using namespace g2ui;

TEST_CASE("After the Perf switch, a document live on the synth becomes what the synth now plays")
{
    // Round-7 hand tests: after Perf, the document stayed, off the synth, and Get Performance or Get Patch had to be
    // chosen by hand; the user chose that G2fresh opens what the G2 plays.
    CHECK(openAfterPerfSwitch(true, true, 0) == kOpenPerformance);
    CHECK(openAfterPerfSwitch(true, true, 4) == kOpenPerformance);
    CHECK(openAfterPerfSwitch(false, true, 2) == 2); // the slot the G2 has the focus on
    CHECK(openAfterPerfSwitch(false, true, 4) == 0); // no slot focused: A
    // A document not on the synth (the original's Local window) keeps its patch.
    CHECK(openAfterPerfSwitch(true, false, 0) == kOpenNothing);
    CHECK(openAfterPerfSwitch(false, false, 1) == kOpenNothing);
}
