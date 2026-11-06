// Synth > Live Editing (LiveEditing.h): G2fresh's own menu item, as its binding is (the original's patch windows are
// on a slot or local: the background menu's Slot A-D and Local).
#include <catch2/catch_test_macros.hpp>

#include "LiveEditing.h"

using namespace g2ui;

TEST_CASE("Synth > Live Editing: stops it, or sends the document again to start it")
{
    // Round-6 report: after Stop Live Editing, the item said "send or get a patch first" and could not start it again.
    auto i = liveEditingItem(true, true, false, false, 0);
    CHECK(i.label == "Stop Live Editing");
    CHECK(i.ticked);
    CHECK(i.action == LiveEditingAction::Stop);
    i = liveEditingItem(true, false, false, false, 2);
    CHECK(i.label == "Live Editing (sends the patch to slot C)");
    CHECK(i.enabled);
    CHECK(i.action == LiveEditingAction::SendPatch);
    CHECK(i.slot == 2);
    i = liveEditingItem(true, false, false, true, 0);
    CHECK(i.action == LiveEditingAction::SendPatchAlone); // the Emulated G2 plays it as at launch
    CHECK(i.label == "Live Editing (sends the patch)");
    i = liveEditingItem(true, false, true, false, 0);
    CHECK(i.action == LiveEditingAction::SendPerformance);
    CHECK(i.label == "Live Editing (sends the performance)");
    i = liveEditingItem(false, false, false, false, 0);
    CHECK_FALSE(i.enabled);
}
