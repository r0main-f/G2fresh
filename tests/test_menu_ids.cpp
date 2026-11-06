// The patch window's menu item ids (MenuIds.h): each range is its own, so no command is taken for another.
#include <catch2/catch_test_macros.hpp>

#include "MenuIds.h"

using namespace g2ui::menuid;

TEST_CASE("menu ids: only the Recent Files entries open a recent file")
{
    // Round-4 hand tests: New To, Open To, Save From and the MIDI selection items did nothing, taken for recent files.
    CHECK(isRecentFileItem(kRecentBase));
    CHECK(isRecentFileItem(kRecentBase + kRecentMax - 1));
    CHECK_FALSE(isRecentFileItem(kRecentBase + kRecentMax));
    for (const int id : {int(kNewToBase), int(kOpenToBase) + 2, int(kSaveFromBase) + 3, int(kAssignMidiSelection),
                         int(kDeassignMidiSelection), int(kDeassignAllMidi), int(kCopyVariationBase), int(kCablesBase),
                         int(kMidiChannelBase), int(kSynthG2), int(kWheelEditsKnobs)})
        CHECK_FALSE(isRecentFileItem(id));
}
