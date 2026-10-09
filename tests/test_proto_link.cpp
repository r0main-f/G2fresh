#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "g2/proto/emulator.hpp"
#include "g2/proto/link.hpp"

using namespace g2;
using namespace g2::proto;

TEST_CASE("a link to the virtual G2 connects, syncs and carries edits")
{
    auto link = LocalLink::virtualG2();
    REQUIRE(link->emulator() != nullptr);
    for (int i = 0; i < 1000 && !link->synced(); ++i)
        link->tick();
    REQUIRE(link->connected());
    REQUIRE(link->synced());

    // Send a patch with an OscB to slot A, then turn its first knob.
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, 7, 0, 0);
    link->sendPatch(0, p, "Link test");
    for (int i = 0; i < 1000 && !link->client().idle(); ++i)
        link->tick();
    REQUIRE(link->emulator()->state().slots[0].patch.va.find(osc) != nullptr);

    link->setParam(0, Location::Va, osc, 0, 99, 0);
    for (int i = 0; i < 100; ++i)
        link->tick();
    CHECK(link->emulator()->state().slots[0].patch.va.find(osc)->params[0][0] == 99);
    CHECK(link->state().slots[0].patch.va.find(osc)->params[0][0] == 99);
}
