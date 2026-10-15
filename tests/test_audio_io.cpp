// The plugin's audio buses and its inputs' levels (plugin/Source/AudioIo.h).
#include "AudioIo.h"
#include "StandaloneAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <initializer_list>
#include <string>
#include <vector>

namespace {

bool supported(std::initializer_list<int> inputs, std::initializer_list<int> outputs)
{
    const std::vector<int> in(inputs), out(outputs);
    return g2io::layoutSupported(in, out);
}

} // namespace

TEST_CASE("Buses in a host: In 1/2 and In 3/4, Out 1/2 and Out 3/4", "[audioio]")
{
    // as the plugin starts: In 1/2 and Out 1/2 on, the second pairs off
    CHECK(supported({2, 0}, {2, 0}));
    // an instrument without inputs, as before there were any (a host that routes nothing to it)
    CHECK(supported({0, 0}, {2, 0}));
    CHECK(supported({0, 0}, {1, 0}));
    CHECK(supported({0, 0}, {2, 2}));
    // everything routed
    CHECK(supported({2, 2}, {2, 2}));
    CHECK(supported({2, 2}, {2, 0}));
    CHECK(supported({2, 0}, {2, 2}));
    // In 3/4 without In 1/2 (a host that routes only its second side-chain)
    CHECK(supported({0, 2}, {2, 0}));
    // mono: IN 1 alone; a mono Out 1/2 is Out 1 + Out 2
    CHECK(supported({1, 0}, {2, 0}));
    CHECK(supported({1, 0}, {1, 0}));
    CHECK(supported({2, 0}, {1, 0}));

    // Out 1/2 is always there
    CHECK_FALSE(supported({2, 0}, {0, 0}));
    CHECK_FALSE(supported({2, 0}, {0, 2}));
    // Out 3/4 is a stereo pair next to a stereo Out 1/2
    CHECK_FALSE(supported({0, 0}, {1, 2}));
    CHECK_FALSE(supported({0, 0}, {2, 1}));
    // In 3/4 is a stereo pair; no pair has more than two channels
    CHECK_FALSE(supported({2, 1}, {2, 0}));
    CHECK_FALSE(supported({4, 0}, {2, 0}));
    CHECK_FALSE(supported({2, 4}, {2, 0}));
    CHECK_FALSE(supported({2, 0}, {4, 0}));
    // a channel set the plugin does not know (negative), more buses than it has
    CHECK_FALSE(supported({-1, 0}, {2, 0}));
    CHECK_FALSE(supported({2, 0}, {-1, 0}));
    CHECK_FALSE(supported({2, 0, 2}, {2, 0}));
    CHECK_FALSE(supported({2, 0}, {2, 0, 2}));
    CHECK_FALSE(supported({2, 0}, {}));
}

TEST_CASE("Buses of the stand-alone app: the audio device's inputs 1-4 on one bus, Out 1/2", "[audioio]")
{
    for (int inputs = 0; inputs <= 4; ++inputs) // as many as the device has
        CHECK(supported({inputs}, {2}));
    CHECK(supported({4}, {1}));
    CHECK_FALSE(supported({5}, {2}));
    CHECK_FALSE(supported({-1}, {2}));
    // before there were inputs: outputs only
    CHECK(supported({}, {2}));
    CHECK(supported({}, {2, 2}));
}

TEST_CASE("Which of IN 1-4 a bus's channel is", "[audioio]")
{
    // In 1/2, then In 3/4
    CHECK(g2io::inputOf(0, 0) == 0);
    CHECK(g2io::inputOf(0, 1) == 1);
    CHECK(g2io::inputOf(1, 0) == 2);
    CHECK(g2io::inputOf(1, 1) == 3);
    // the stand-alone app's one bus of four
    CHECK(g2io::inputOf(0, 2) == 2);
    CHECK(g2io::inputOf(0, 3) == 3);
    // nothing beyond IN 4
    CHECK(g2io::inputOf(0, 4) == -1);
    CHECK(g2io::inputOf(1, 2) == -1);
    CHECK(g2io::inputOf(2, 0) == -1);
    CHECK(g2io::inputOf(-1, 0) == -1);
}

TEST_CASE("Input level: 1.0 is a signal of 1.0, a quarter of the ADCs' full scale", "[audioio]")
{
    // [emulated, tests/test_g2emu.cpp] an ADC word w comes out of an In module (Pad 0 dB) and an Out module as a
    // DAC word of -0.4987 w, which the plugin plays as 8 * 0.4987 w: the level it went in, to 0.02 dB
    CHECK(g2io::kAdcPerUnit == 0.25f);
    CHECK_THAT(8.0 * 0.4987 * g2io::kAdcPerUnit, Catch::Matchers::WithinAbs(1.0, 0.003));
}

TEST_CASE("The emulated G2's buffer: three reserves, named in the settings", "[audioio]")
{
    using g2io::Buffer;
    // smallest first; Safe keeps at least what G2fresh kept before there was a choice (20 ms, and up to 1 ms more:
    // the machine ran in 1 ms chunks past its mark). The default is Normal: in the sweep of 2026-10-14 on an M1
    // (re/notes/audio-path.md §3.6) no run that started quiet, nor any beside programs at an application's priority,
    // took more than 2.67 ms of its reserve; the runs that lost audio lost it with Safe's 21 ms too.
    CHECK(g2io::reserveMs(Buffer::Low) < g2io::reserveMs(Buffer::Normal));
    CHECK(g2io::reserveMs(Buffer::Normal) < g2io::reserveMs(Buffer::Safe));
    CHECK(g2io::reserveMs(Buffer::Safe) >= 20.0 + 1.0);
    CHECK(g2io::reserveMs(Buffer::Normal) >= 2 * 2.67);  // twice the most a quiet run took
    CHECK(g2io::kDefaultBuffer == Buffer::Normal);
    // the menu's index and the settings file's word
    for (int i = 0; i < g2io::kBuffers; ++i)
        CHECK(g2io::bufferFromKey(g2io::bufferKey(g2io::bufferOf(i)), Buffer::Safe) == g2io::bufferOf(i));
    CHECK(g2io::bufferKey(Buffer::Low) == "low");
    CHECK(g2io::bufferOf(-1) == Buffer::Low);
    CHECK(g2io::bufferOf(7) == Buffer::Safe);
    // a settings file without the word, or with another: the default
    CHECK(g2io::bufferFromKey("", Buffer::Normal) == Buffer::Normal);
    CHECK(g2io::bufferFromKey("fast", Buffer::Safe) == Buffer::Safe);
}

TEST_CASE("Mic Level: the preamp's gain from 0 to +40 dB", "[audioio]")
{
    CHECK_THAT(g2io::micGain(0.0f), Catch::Matchers::WithinRel(1.0f, 1e-5f));
    CHECK_THAT(g2io::micGain(0.5f), Catch::Matchers::WithinRel(10.0f, 1e-4f));
    CHECK_THAT(g2io::micGain(1.0f), Catch::Matchers::WithinRel(100.0f, 1e-4f));
    // the knob has ends
    CHECK_THAT(g2io::micGain(-1.0f), Catch::Matchers::WithinRel(1.0f, 1e-5f));
    CHECK_THAT(g2io::micGain(2.0f), Catch::Matchers::WithinRel(100.0f, 1e-4f));
}

TEST_CASE("The stand-alone app opens no audio input until the user chooses one", "[audioio]")
{
    // Opening an input is not free: a Bluetooth headset's microphone switches the headset to its call profile, which
    // changes the sound of every application (heard 2026-10-15: AirPods, YouTube's level jumped when G2fresh started).
    // A G2 plays its IN 1-4 only where a patch has an In module, so the app starts on its output alone.
    const std::vector<std::string> outputs = {"AirPods", "MacBook Pro Speakers"};
    // never set up: the system's output, no input
    auto start = g2io::standaloneStart(nullptr, outputs, "AirPods");
    REQUIRE(start.has_value());
    CHECK(start->output == "AirPods");
    // set up with an input: the user's choice stays as saved
    const g2io::SavedAudioSetup withInput{"MacBook Pro Microphone", "MacBook Pro Speakers"};
    CHECK_FALSE(g2io::standaloneStart(&withInput, outputs, "AirPods").has_value());
    // set up without an input: its output, still no input
    const g2io::SavedAudioSetup outputOnly{"", "MacBook Pro Speakers"};
    start = g2io::standaloneStart(&outputOnly, outputs, "AirPods");
    REQUIRE(start.has_value());
    CHECK(start->output == "MacBook Pro Speakers");
    // ...whose output has gone (the headphones are off): the system's output, not JUCE's fallback, which would open
    // the default input again
    const g2io::SavedAudioSetup gone{"", "Studio Display Speakers"};
    start = g2io::standaloneStart(&gone, outputs, "AirPods");
    REQUIRE(start.has_value());
    CHECK(start->output == "AirPods");
    // ...written by G2fresh itself at an earlier start, not chosen: it follows the system's output, as JUCE does when
    // nothing was chosen
    const g2io::SavedAudioSetup ours{"", "MacBook Pro Speakers", true};
    start = g2io::standaloneStart(&ours, outputs, "AirPods");
    REQUIRE(start.has_value());
    CHECK(start->output == "AirPods");
}
