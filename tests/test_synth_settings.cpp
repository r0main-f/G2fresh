// The Synth Settings window's fields (ui/SynthSettingsForm.h): the original dialog's values, ranges and texts
// (CDialogSynthSettings, DITL 132) and their mapping to the 03 synth settings.
#include <catch2/catch_test_macros.hpp>

#include "SynthSettingsForm.h"

using g2::proto::SynthSettings;
using g2ui::SettingsEcho;
using g2ui::SynthSettingsForm;

TEST_CASE("synth settings form: the fields from the synth's settings and back")
{
    SynthSettings s;
    s.name = "ModularG2";
    s.perfMode = true;
    s.patchSortMode = 2;
    s.focusBank = 3;
    s.focusProg = 9;
    s.memoryProtect = true;
    s.midiChannels = {0, 16, 9, 15}; // slot B off
    s.globalChannel = 16;            // Off
    s.sysExId = 16;                  // All
    s.localOn = true;
    s.programChangeMode = 2; // receive
    s.controllerMode = 1;    // send
    s.sendArp = true;
    s.sendClock = true;
    s.ignoreExternalClock = false;
    s.masterTune = -50;
    s.transpose = 5;
    s.octaveShiftEnable = true;
    s.octaveShift = -2;
    s.vibratoRate = 7;
    s.sustainPolarity = true;
    s.controlPedalGain = 32;

    SynthSettingsForm f;
    f.take(s);
    CHECK(f.name == "ModularG2");
    CHECK(f.channel == std::array<int, 4>{1, 2, 10, 16}); // slot B keeps the number the dialog starts with
    CHECK(f.active == std::array<bool, 4>{true, false, true, true});
    CHECK(f.globalChannel == 17);
    CHECK(f.sysExId == 17);
    CHECK(f.sendClock);
    CHECK_FALSE(f.ignoreExternalClock);
    CHECK_FALSE(f.controllersReceive);
    CHECK(f.controllersSend);
    CHECK(f.programChangeReceive);
    CHECK_FALSE(f.programChangeSend);
    CHECK(f.semi == 5);
    CHECK(f.cent == -50);
    CHECK(f.octaveShift);
    CHECK(f.octave == -2);
    CHECK(f.pedalGain == 32);
    CHECK(f.memoryProtect);
    CHECK(f.pedalClosed);
    CHECK(f.localOn);
    // unchanged fields give the same settings: what the dialog does not show stays (perf mode, sort, focus, ...)
    CHECK(f.applyTo(s) == s);

    // every field to its place in the settings
    f.name = "Mine";
    f.channel = {3, 7, 11, 16};
    f.active = {true, true, false, true};
    f.globalChannel = 1;
    f.sysExId = 4;
    f.sendClock = false;
    f.ignoreExternalClock = true;
    f.controllersReceive = true;
    f.controllersSend = true;
    f.programChangeReceive = false;
    f.programChangeSend = false;
    f.semi = -6;
    f.cent = 100;
    f.octaveShift = false;
    f.octave = 2;
    f.pedalGain = 16;
    f.memoryProtect = false;
    f.pedalClosed = false;
    f.localOn = false;
    const auto t = f.applyTo(s);
    CHECK(t.name == "Mine");
    CHECK(t.midiChannels == std::array<std::uint8_t, 4>{2, 6, 16, 15});
    CHECK(t.globalChannel == 0);
    CHECK(t.sysExId == 3);
    CHECK_FALSE(t.sendClock);
    CHECK(t.ignoreExternalClock);
    CHECK(t.controllerMode == 3);
    CHECK(t.programChangeMode == 0);
    CHECK(t.transpose == -6);
    CHECK(t.masterTune == 100);
    CHECK_FALSE(t.octaveShiftEnable);
    CHECK(t.octaveShift == 2);
    CHECK(t.controlPedalGain == 16);
    CHECK_FALSE(t.memoryProtect);
    CHECK_FALSE(t.sustainPolarity);
    CHECK_FALSE(t.localOn);
    CHECK(t.perfMode);
    CHECK(t.patchSortMode == 2);
    CHECK(t.focusBank == 3);
    CHECK(t.focusProg == 9);
    CHECK(t.sendArp);
    CHECK(t.vibratoRate == 7);

    // a slot switched back to active sends the number its field kept
    SynthSettingsForm g;
    g.take(t);
    CHECK(g.channel[2] == 3); // never had one from the synth: the slot's own
    g.channel[2] = 12;
    g.active[2] = false;
    CHECK(g.applyTo(t).midiChannels[2] == 16);
    g.active[2] = true;
    CHECK(g.applyTo(t).midiChannels[2] == 11);
}

TEST_CASE("synth settings form: ranges and texts")
{
    // Global Channel and SysEx ID wrap through 1..16 and Off / All (WrapValue 1..17)
    CHECK(SynthSettingsForm::wrap(17 + 1) == 1);
    CHECK(SynthSettingsForm::wrap(1 - 1) == 17);
    CHECK(SynthSettingsForm::wrap(16 + 1) == 17);
    CHECK(SynthSettingsForm::wrap(9) == 9);
    CHECK(SynthSettingsForm::wrap(40) == 6); // typed
    CHECK(SynthSettingsForm::globalChannelText(1) == "1");
    CHECK(SynthSettingsForm::globalChannelText(16) == "16");
    CHECK(SynthSettingsForm::globalChannelText(17) == "Off");
    CHECK(SynthSettingsForm::sysExIdText(16) == "16");
    CHECK(SynthSettingsForm::sysExIdText(17) == "All");
    // the others stop at their ends (ClipValue)
    CHECK(SynthSettingsForm::clip(0, 1, 16) == 1);
    CHECK(SynthSettingsForm::clip(17, 1, 16) == 16);
    CHECK(SynthSettingsForm::clip(-7, -6, 6) == -6);
    CHECK(SynthSettingsForm::clip(101, -100, 100) == 100);
    CHECK(SynthSettingsForm::clip(33, 0, 32) == 32);

    // Master Tune as the pitch of A
    CHECK(SynthSettingsForm::masterTuneText(0, 0) == "440.00Hz");
    CHECK(SynthSettingsForm::masterTuneText(0, 100) == "466.16Hz");
    CHECK(SynthSettingsForm::masterTuneText(-6, 0) == "311.13Hz");
    CHECK(SynthSettingsForm::masterTuneText(6, 100) == "659.26Hz");
    CHECK(SynthSettingsForm::masterTuneText(0, -1) == "439.75Hz");
    // Control Pedal Gain x1.00..x1.50
    CHECK(SynthSettingsForm::pedalGainText(0) == "x1.00");
    CHECK(SynthSettingsForm::pedalGainText(16) == "x1.25");
    CHECK(SynthSettingsForm::pedalGainText(32) == "x1.50");
    // Free Mem: from the usage in per mille
    CHECK(SynthSettingsForm::freeMemoryText(std::nullopt) == "100.0%");
    CHECK(SynthSettingsForm::freeMemoryText(std::uint16_t{0}) == "100.0%");
    CHECK(SynthSettingsForm::freeMemoryText(std::uint16_t{123}) == "87.7%");
    CHECK(SynthSettingsForm::freeMemoryText(std::uint16_t{1000}) == "0.0%");

    // the name's characters: the G2's, without '\' and '~'
    CHECK(SynthSettingsForm::kNameChars.size() == 93);
    CHECK(SynthSettingsForm::kNameChars.find('\\') == std::string_view::npos);
    CHECK(SynthSettingsForm::kNameChars.find('~') == std::string_view::npos);
    CHECK(SynthSettingsForm::kNameChars.find(' ') != std::string_view::npos);

    // out-of-range settings from a synth show clipped
    SynthSettings s;
    s.transpose = 12;
    s.masterTune = -128;
    s.octaveShift = 5;
    SynthSettingsForm f;
    f.take(s);
    CHECK(f.semi == 6);
    CHECK(f.cent == -100);
    CHECK(f.octave == 2);
}

TEST_CASE("synth settings: the window's own changes are told from the synth's")
{
    SynthSettings a;
    a.name = "A";
    SynthSettings b = a, c = a, d = a;
    b.localOn = false;
    c.memoryProtect = true;
    d.masterTune = 5;

    SettingsEcho echo;
    echo.start(a);
    CHECK_FALSE(echo.changedElsewhere(a));
    // a local link: the mirror holds what was sent at once
    echo.sent(b);
    CHECK_FALSE(echo.changedElsewhere(b));
    CHECK_FALSE(echo.changedElsewhere(b));
    // through g2bridge: two changes sent, the mirror follows one by one
    echo.sent(c);
    echo.sent(d);
    CHECK_FALSE(echo.changedElsewhere(b)); // nothing yet
    CHECK_FALSE(echo.changedElsewhere(c));
    CHECK_FALSE(echo.changedElsewhere(d));
    // the synth's System menu (or another editor) changed something
    CHECK(echo.changedElsewhere(a));
    CHECK_FALSE(echo.changedElsewhere(a));
    // sent, then back to what it was before the mirror was looked at: both seen
    echo.sent(b);
    echo.sent(a);
    CHECK_FALSE(echo.changedElsewhere(a));
    // a change from the synth while ours is on its way: taken, ours forgotten
    echo.sent(c);
    CHECK(echo.changedElsewhere(d));
    CHECK(echo.changedElsewhere(c));
    // a change sent that never showed (it was sent back before the mirror was looked at) is not waited for for ever:
    // the same settings coming from the synth later are the synth's
    echo.sent(b);
    echo.sent(c);
    for (int i = 0; i <= SettingsEcho::kPatience; ++i)
        CHECK_FALSE(echo.changedElsewhere(c));
    CHECK(echo.changedElsewhere(b));
}
