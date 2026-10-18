// When the Emulated G2 starts and what it plays (EmulatorFollow.h): G2fresh's own, as the emulator is.
#include <catch2/catch_test_macros.hpp>

#include "EmulatorFollow.h"

using namespace g2io;

TEST_CASE("Emulated G2: the stand-alone app starts it at every launch, a plugin as its project says")
{
    // Round-5 decision (2026-10-18): after Synth > Disconnect, the stand-alone app still connects at the next launch.
    CHECK(emulatorAtLaunch(true, false));
    CHECK(emulatorAtLaunch(true, true));
    CHECK_FALSE(emulatorAtLaunch(false, false));
    CHECK(emulatorAtLaunch(false, true));
}

TEST_CASE("Emulated G2: it plays each document opened or made")
{
    // Round-5 hand tests: after a performance, a patch opened stayed off the Emulated G2 (the binding ended with the
    // kind of document). It sends each new document once; a document unbound by the user (Stop Live Editing) stays off.
    CHECK(emulatorSendsDocument(true, true, false, 3, -1));  // just started
    CHECK(emulatorSendsDocument(true, true, false, 4, 3));   // a new document since
    CHECK_FALSE(emulatorSendsDocument(true, true, false, 3, 3)); // the same document, unbound by the user
    CHECK_FALSE(emulatorSendsDocument(true, true, true, 4, 3));  // bound: live editing carries it
    CHECK_FALSE(emulatorSendsDocument(true, false, false, 4, 3)); // not up yet
    CHECK_FALSE(emulatorSendsDocument(false, true, false, 4, 3)); // another synth
}
