// What showing the Live view does about the emulated G2 (ui/LiveShow.h).
#include "LiveShow.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Showing the Live view starts the emulated G2, unless something else should play", "[live]")
{
    using g2ui::LiveShow;
    // The panel is the emulated G2's: shown, it starts, as a G2 is on when you sit at it (no button to press)
    CHECK(g2ui::liveShow(true, false, false, false, 0) == LiveShow::Start);
    CHECK(g2ui::liveShow(true, true, true, false, 0) == LiveShow::Nothing);
    // a build without the emulator
    CHECK(g2ui::liveShow(false, false, false, false, 0) == LiveShow::NoEmulator);
    // the editor talks to a G2 on USB or the virtual one: starting would take the editor from it
    CHECK(g2ui::liveShow(true, false, false, true, 0) == LiveShow::OtherSynth);
    // another instance in the host runs one: two at once lost all of their audio on an M1 (re/notes/audio-path.md
    // §3.6), so a second starts only when asked (Synth > Emulated G2), as at a project's opening
    CHECK(g2ui::liveShow(true, false, false, false, 1) == LiveShow::OtherInstance);
    // Found by hand testing: after Synth > Disconnect the emulated G2 still runs but the editor is off it, and the Live
    // panel stayed dead; it connects the editor again, as Synth > Emulated G2 does
    CHECK(g2ui::liveShow(true, true, false, false, 0) == LiveShow::Start);
    // its own emulated G2 counts in the host: still this one, not another instance's
    CHECK(g2ui::liveShow(true, true, false, false, 1) == LiveShow::Start);
}
