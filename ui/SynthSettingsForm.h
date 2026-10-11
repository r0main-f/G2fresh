// The original editor's Synth Settings dialog (CDialogSynthSettings, DLOG/DITL 132) as values, without a window: the
// fields it shows, their ranges and texts, and how they map to the synth settings (03, CSynthMap). Plain C++, so the
// tests check it; SynthSettingsWindow shows it.
//
// The dialog keeps its own copy of each field (SetValues 0x42b84) and builds the settings from them over the last ones
// sent (GetValues 0x40be8), so what it does not show (perf mode, sort modes, the bits the OS keeps for itself) goes
// back unchanged.
#pragma once

#include "g2/param_text.hpp"
#include "g2/proto/molecules.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace g2ui {

struct SynthSettingsForm {
    // The name field: 16 characters of this set (HandleChangeRequests 0x45dc0: no '\' and no '~'), taken when typing
    // ends (Return, or the field losing focus).
    static constexpr std::size_t kNameLength = 16;
    static constexpr std::string_view kNameChars =
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_`abcdefghijklmnopqrstuvwxyz{|}";
    static constexpr int kOff = 17; // Global Channel "Off", SysEx ID "All"

    std::string name;
    // MIDI channel: 1..16 per slot (clipped), and "active". An inactive slot is sent as 16 (off); its field keeps the
    // number shown, from the dialog's start (the slot's own, 1..4) if the synth never had one.
    std::array<int, 4> channel{1, 2, 3, 4};
    std::array<bool, 4> active{true, true, true, true};
    // MIDI Settings
    int globalChannel = 1; // 1..16, 17 "Off"; the arrows wrap around
    int sysExId = 1;       // 1..16, 17 "All"; the arrows wrap around
    bool sendClock = true, ignoreExternalClock = true;
    bool controllersReceive = true, controllersSend = true;
    bool programChangeReceive = true, programChangeSend = true;
    // Tune: Master Tune in semitones (-6..6, the settings' transpose) and cents (-100..100, their master tune);
    // Global Octave Shift -2..+2, its five buttons enabled with the check box.
    int semi = 0, cent = 0;
    bool octaveShift = false;
    int octave = 0;
    int pedalGain = 0; // Control Pedal Gain 0..32: x1.00..x1.50
    bool localOn = true;
    bool pedalClosed = false; // Pedal polarity: Open, Closed
    bool memoryProtect = true; // File

    bool operator==(const SynthSettingsForm&) const = default;

    static int clip(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; } // NSSmuggler::ClipValue
    // NSSmuggler::WrapValue(v, 1, 18): 1..17, past either end to the other.
    static int wrap(int v)
    {
        int i = v - 1;
        while (i < 0)
            i += kOff;
        return 1 + i % kOff;
    }

    // SetValues: the fields from the synth's settings.
    void take(const g2::proto::SynthSettings& s)
    {
        name = s.name;
        for (std::size_t i = 0; i < 4; ++i) {
            active[i] = s.midiChannels[i] < 16;
            if (active[i])
                channel[i] = s.midiChannels[i] + 1;
        }
        globalChannel = s.globalChannel + 1;
        sysExId = s.sysExId + 1;
        sendClock = s.sendClock;
        ignoreExternalClock = s.ignoreExternalClock;
        controllersReceive = s.controllerMode == 2 || s.controllerMode == 3;
        controllersSend = s.controllerMode == 1 || s.controllerMode == 3;
        programChangeReceive = s.programChangeMode == 2 || s.programChangeMode == 3;
        programChangeSend = s.programChangeMode == 1 || s.programChangeMode == 3;
        semi = clip(s.transpose, -6, 6);
        cent = clip(s.masterTune, -100, 100);
        octaveShift = s.octaveShiftEnable;
        octave = clip(s.octaveShift, -2, 2);
        pedalGain = s.controlPedalGain;
        memoryProtect = s.memoryProtect;
        pedalClosed = s.sustainPolarity;
        localOn = s.localOn;
    }

    // GetValues: `s` (the settings last sent) with the fields.
    g2::proto::SynthSettings applyTo(g2::proto::SynthSettings s) const
    {
        using u8 = std::uint8_t;
        s.name = name;
        for (std::size_t i = 0; i < 4; ++i)
            s.midiChannels[i] = active[i] ? static_cast<u8>(channel[i] - 1) : u8{16};
        s.globalChannel = static_cast<u8>(globalChannel - 1);
        s.sysExId = static_cast<u8>(sysExId - 1);
        s.sendClock = sendClock;
        s.ignoreExternalClock = ignoreExternalClock;
        s.controllerMode = static_cast<u8>((controllersReceive ? 2 : 0) | (controllersSend ? 1 : 0));
        s.programChangeMode = static_cast<u8>((programChangeReceive ? 2 : 0) | (programChangeSend ? 1 : 0));
        s.transpose = static_cast<std::int8_t>(semi);
        s.masterTune = static_cast<std::int8_t>(cent);
        s.octaveShiftEnable = octaveShift;
        s.octaveShift = static_cast<std::int8_t>(octave);
        s.controlPedalGain = static_cast<u8>(pedalGain);
        s.memoryProtect = memoryProtect;
        s.sustainPolarity = pedalClosed;
        s.localOn = localOn;
        return s;
    }

    // ---- Texts ----
    static std::string globalChannelText(int v) { return v < kOff ? std::to_string(v) : "Off"; }
    static std::string sysExIdText(int v) { return v < kOff ? std::to_string(v) : "All"; }
    // SetMasterTune 0x429c4: the pitch of A, "440.00Hz".
    static std::string masterTuneText(int semi, int cent)
    {
        const double ln2 = static_cast<double>(std::log(2.0f));
        const double hz = 440.0 * std::exp(semi * ln2 / 12.0) * std::exp(ln2 * cent / 1200.0);
        char text[32];
        std::snprintf(text, sizeof text, "%.2fHz", hz);
        return text;
    }
    static std::string pedalGainText(int v) { return g2::paramtext::ControlPedalGain(static_cast<std::uint8_t>(v)); }
    // SetupState 0x45a8e: the free part of the memory, from the synth's usage in per mille (unknown: none used).
    static std::string freeMemoryText(std::optional<std::uint16_t> usage)
    {
        char text[32];
        std::snprintf(text, sizeof text, "%.1f%%", (1000 - static_cast<int>(usage.value_or(0))) / 10.0);
        return text;
    }
};

// Tells the window's own changes, as the synth's mirror shows them (at once on a local link, a tick later through
// g2bridge), from changes made on the synth (its System menu), which the window takes (CDialogSynthSettings::Idle
// 0x45cd0 compares the time of the synth's last settings with its own last send).
class SettingsEcho {
public:
    // A change sent is expected back within this many looks at the mirror (the window looks 20 times a second).
    static constexpr int kPatience = 40;

    void start(const g2::proto::SynthSettings& s)
    {
        seen_ = s;
        pending_.clear();
    }
    void sent(const g2::proto::SynthSettings& s) { pending_.push_back({s, 0}); }
    // The mirror holds `now`: whether it changed, other than by what was sent.
    bool changedElsewhere(const g2::proto::SynthSettings& now)
    {
        std::erase_if(pending_, [](Sent& p) { return ++p.looks > kPatience; });
        if (now == seen_)
            return false;
        seen_ = now;
        for (std::size_t i = 0; i < pending_.size(); ++i)
            if (pending_[i].settings == now) {
                pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                return false;
            }
        pending_.clear();
        return true;
    }

private:
    struct Sent {
        g2::proto::SynthSettings settings;
        int looks = 0;
    };
    g2::proto::SynthSettings seen_;
    std::vector<Sent> pending_;
};

} // namespace g2ui
