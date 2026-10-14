// The plugin's audio buses and the level of its inputs, as rules the tests can check.
//
// Buses, as the G2's jacks. Outputs: "Out 1/2" (stereo, or mono) and "Out 3/4" (off, or stereo next to a stereo
// Out 1/2). Inputs: "In 1/2" (off, stereo, or mono: IN 1 alone) and "In 3/4" (off or stereo). A host that gives no
// inputs gets the instrument it had before there were any. The stand-alone app has one input bus instead, the audio
// device's inputs 1-4 (as many as the device has, up to 4), and Out 1/2.
//
// Levels. The plugin's outputs are in signal units (a signal of 1.0 into an Out module is 1.0:
// EmulatedSoundEngine.h), and so are its inputs: 1.0 comes out of an In module (Pad 0 dB) as a signal of 1.0, the
// level of an oscillator. The In modules give a signal of 4.0 for the ADCs' full scale [emulated: tests/
// test_g2emu.cpp], so 1.0 is a quarter of it: the inputs clip at 4.0 (+12 dBFS), where the G2's ADCs do. Which
// voltage that is on a G2 (its jacks are -10 dBV nominal) is not known, as for the outputs.
//
// The microphone. On a G2 the XLR input's preamp feeds IN 1 while nothing is plugged into the IN 1 jack, and the
// Mic Level knob sets its gain; the knob does nothing to the jack. Here In 1 is the jack unless the user says it
// is the microphone; then the knob's gain applies to it, before the ADC. The gain's range is not in the manual:
// 0 to +40 dB is an assumption (what brings a dynamic microphone to line level).
//
// The buffer. The emulated G2 runs on threads of its own, ahead of the host: it keeps a reserve of audio ready, so
// that a moment in which the computer gives it no time is not heard (re/notes/audio-path.md). The reserve is part
// of the latency, and how much it takes depends on the computer and on what else runs: the user chooses (Synth >
// Emulated G2 Buffer), for the computer, not for the project.
//
// Plain C++ (no JUCE).
#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>

namespace g2io {

// Whether the plugin plays with these buses: the channels of each input and output bus (0: off; negative: a
// channel set that is none of mono, stereo or plain channels).
inline bool layoutSupported(std::span<const int> inputs, std::span<const int> outputs)
{
    if (outputs.empty() || outputs.size() > 2 || inputs.size() > 2)
        return false;
    if (outputs[0] != 1 && outputs[0] != 2)
        return false;
    if (outputs.size() == 2 && outputs[1] != 0 && (outputs[1] != 2 || outputs[0] != 2))
        return false;
    if (inputs.size() == 1) // the stand-alone app's "In 1-4"
        return inputs[0] >= 0 && inputs[0] <= 4;
    if (inputs.size() == 2)
        return inputs[0] >= 0 && inputs[0] <= 2 && (inputs[1] == 0 || inputs[1] == 2);
    return true;
}

// Which of IN 1-4 (0-3) channel `channel` of input bus `bus` is, or -1: the first bus starts at IN 1, the second
// ("In 3/4") at IN 3.
inline int inputOf(int bus, int channel)
{
    const int input = bus * 2 + channel;
    return bus >= 0 && channel >= 0 && input < 4 ? input : -1;
}

// The ADC word (1.0: full scale) of an input sample of 1.0.
inline constexpr float kAdcPerUnit = 0.25f;

// The emulated G2's buffer, smallest first. Safe keeps at least what G2fresh kept before there was a choice: up to
// 0.10.2 the reserve was 20 ms and the machine ran up to a millisecond past it (it ran in whole chunks); it now
// stops exactly at its mark, so Safe's mark is 21 ms. Its latency is then no lower than 0.10.2's (27.6 ms against
// 27.0 at 48 kHz with 256-sample blocks).
enum class Buffer { Low = 0, Normal = 1, Safe = 2 };
inline constexpr int kBuffers = 3;
inline constexpr double kReserveLowMs = 2.0, kReserveNormalMs = 6.0, kReserveSafeMs = 21.0;
// Where nothing else is chosen: Safe. PROVISIONAL, as are Low's and Normal's reserves: they were only measured on a
// computer that other work kept busy (re/notes/audio-path.md, part 3); the default and the reserves follow from
// tools/audiopath/sweep.py on a quiet one.
inline constexpr Buffer kDefaultBuffer = Buffer::Safe;
// The least the emulated G2 computes in one run, in milliseconds: with a host block shorter than this it takes the
// block's place in the latency (g2emu::Runner::Options::stretchMs). One millisecond: only blocks of fewer than 48
// samples at 48 kHz are shorter.
inline constexpr double kStretchMs = 1.0;

inline Buffer bufferOf(int index) { return static_cast<Buffer>(std::clamp(index, 0, kBuffers - 1)); }

// The reserve in milliseconds: the audio kept ready beyond the host's next block.
inline constexpr double reserveMs(Buffer b)
{
    return b == Buffer::Low ? kReserveLowMs : b == Buffer::Normal ? kReserveNormalMs : kReserveSafeMs;
}

// The setting's name in the user's settings file, and back (anything else: `fallback`).
inline constexpr std::string_view bufferKey(Buffer b)
{
    return b == Buffer::Low ? "low" : b == Buffer::Normal ? "normal" : "safe";
}

inline Buffer bufferFromKey(std::string_view key, Buffer fallback)
{
    for (int i = 0; i < kBuffers; ++i)
        if (key == bufferKey(bufferOf(i)))
            return bufferOf(i);
    return fallback;
}

// The microphone preamp's gain with the Mic Level knob at `level` (0 .. 1).
inline constexpr float kMicGainDb = 40.0f;
inline float micGain(float level)
{
    return std::pow(10.0f, kMicGainDb * std::clamp(level, 0.0f, 1.0f) / 20.0f);
}

} // namespace g2io
