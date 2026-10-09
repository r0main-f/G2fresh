// Tests for the ParamText port (core/src/param_text.cpp).
//
// The table at the end was produced by running the original editor's own
// ParamText functions (G2Editor_i386, Mac v1.62) in a CPU emulator, with the
// libc/libm imports (sprintf, exp, log, ...) supplied by the host; see
// re/notes/param-display.md. The hand-written cases above it are worked
// through from the decompiled formulas.
#include <catch2/catch_test_macros.hpp>

#include "g2/param_text.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pt = g2::paramtext;

TEST_CASE("param text: worked examples")
{
    // Default: 0..127 shown as 0..100, exact at the quarter points.
    CHECK(pt::Default(0) == "0");
    CHECK(pt::Default(64) == "50");
    CHECK(pt::Default(127) == "100");
    CHECK(pt::Default(1) == "0.8"); // 1 * 100 / 128 = 0.78
    // Semitone: MIDI note names, C4 = 60.
    CHECK(pt::Semitone(60) == "C4");
    CHECK(pt::Semitone(69) == "A4");
    CHECK(pt::Semitone(0) == "C-1");
    CHECK(pt::SemitoneKey(64) == "E4 Key");
    // FilterCutOff: 440 * 2^((v - 65) / 12) Hz, 3-4 significant digits.
    CHECK(pt::FilterCutOff(64) == "415.3Hz");
    CHECK(pt::FilterCutOff(0) == "10.30Hz");
    CHECK(pt::FilterCutOff(127) == "15.8kHz");
    // OscFreqDep(coarse, fine, tune mode).
    CHECK(pt::OscFreqDep(69, 64, 1) == "440.00Hz"); // Freq
    CHECK(pt::OscFreqDep(69, 64, 0) == " +5   +0"); // Semi: "%+3d %+4.0f"
    CHECK(pt::OscFreqDep(69, 64, 2) == "x1.3348");  // Factor: 440 / 329.6275569
    CHECK(pt::OscFreqDep(69, 64, 3) == "6:1   +0"); // Partial
    CHECK(pt::OscFreqDep(69, 64, 4) == "0.2148Hz"); // Sub: 0.21484375 * ratio
    // dB: 20 log10(0.01 x + 0.99 x^3), x = v / 127, always one decimal.
    CHECK(pt::dB(0) == "-oo");
    CHECK(pt::dB(64) == "-17.6");
    CHECK(pt::dB(127) == "-0");
    // "±" is emitted as UTF-8 (the original wrote Mac Roman 0xB1).
    CHECK(pt::PlusMinusHalfSteps(3) == "\xC2\xB1" "1.5");
    CHECK(pt::NoteScaler(24) == "\xC2\xB1" "12.0-Oct");
    CHECK(pt::NoteScaler(14) == "\xC2\xB1" "7.0-5th");
    // Table lookups.
    CHECK(pt::EnvelopeTime(0) == " 0.5m");
    CHECK(pt::EnvelopeTime(127) == "45.0s");
    CHECK(pt::LfoAWave(4) == "RndStep");
    CHECK(pt::LfoAWave(6).empty()); // out of range -> nothing written
    // Dependent displays.
    CHECK(pt::ReverbTime(127, 2) == "9.000s");             // (3 * 2 + 3) s at full time
    CHECK(pt::ClkGenTempo(64, 1, 0) == "120BPM");
    CHECK(pt::ClkGenTempo(64, 0, 0) == "--");
    CHECK(pt::ClkGenTempo(64, 1, 1) == "Master");
    CHECK(pt::DelayTimeTap(127, 0, 4) == "1.000s");        // (127 * 756 + 1) / 96000 s
    CHECK(pt::DelayTimeTap(64, 1, 0) == "1/4T");           // clock synced
    CHECK(pt::GlideTimeRate(64, 1) == "480m/oct");
    CHECK(pt::AmpGainNew(64, 0) == "x1.00");
    CHECK(pt::AmpGainNew(127, 1) == "12.0");
    CHECK(pt::DXOscFreq(3, 50, 0) == "x4.50");
    CHECK(pt::DXOscFreq(3, 50, 1) == "3162 Hz");
    CHECK(pt::CompressorRatio(0) == "1.0:1");
    CHECK(pt::CompressorRatio(60) == " 50:1");
    CHECK(pt::LfoRateMult(64) == "1:1");
    CHECK(pt::LfoRateMult(69) == "x1.335");
    CHECK(pt::LfoRateMult(100) == "8:1");
}

TEST_CASE("param text: dispatch by id and number of values")
{
    const std::array<std::uint8_t, 1> one{64};
    const std::array<std::uint8_t, 2> two{69, 64};
    const std::array<std::uint8_t, 3> three{69, 64, 1};
    CHECK(pt::format(21, one) == "415.3Hz");        // single 21 = FilterCutOff
    CHECK(pt::format(60, three) == "440.00Hz");     // triple 60 = OscFreqDep
    CHECK(pt::format(0, two) == "0.2148Hz");        // dual 0 = OscSubFreq (single 0 = Default)
    CHECK(pt::format(0, one) == "50");
    CHECK(pt::format(56, one) == "50");             // gap in the single table -> Default
    CHECK(pt::format(21, two) == "69");             // not a dual id -> DualDefault
    CHECK(pt::format(21, three) == "69");           // not a triple id -> TripleDefault
    CHECK(pt::format(21, std::span<const std::uint8_t>{}).empty());
    CHECK(std::string(pt::functionName(21, 1)) == "FilterCutOff");
    CHECK(std::string(pt::functionName(97, 2)) == "OscSyncTimbre");
    CHECK(std::string(pt::functionName(60, 3)) == "OscFreqDep");
    CHECK(std::string(pt::functionName(255, 1)) == "Default");
    CHECK(std::string(pt::functionName(255, 2)) == "DualDefault");
}

namespace {

struct Ref {
    int id;
    std::vector<int> values;
    const char* text;
};

// {id, values (own value first, then dependencies), original output}
const Ref kReference[] = {
    {0, {0}, "0"}, // Default
    {0, {1}, "0.8"}, // Default
    {0, {64}, "50"}, // Default
    {0, {100}, "78.1"}, // Default
    {0, {127}, "100"}, // Default
    {1, {0}, "0"}, // Negative
    {1, {1}, "-1"}, // Negative
    {1, {64}, "-64"}, // Negative
    {1, {100}, "-100"}, // Negative
    {1, {127}, "-127"}, // Negative
    {2, {0}, "1"}, // Enum
    {2, {1}, "2"}, // Enum
    {2, {64}, "65"}, // Enum
    {2, {100}, "101"}, // Enum
    {2, {127}, "128"}, // Enum
    {3, {0}, "Off"}, // OnOff
    {3, {1}, "On"}, // OnOff
    {3, {2}, ""}, // OnOff
    {4, {0}, "Poly"}, // Mono
    {4, {1}, "Mono"}, // Mono
    {4, {2}, ""}, // Mono
    {5, {0}, "Normal"}, // Flip
    {5, {1}, "Inverted"}, // Flip
    {5, {2}, ""}, // Flip
    {6, {0}, "GC Off"}, // GainCompensation
    {6, {1}, "GC On"}, // GainCompensation
    {6, {2}, ""}, // GainCompensation
    {7, {0}, "Muted"}, // Mute
    {7, {1}, "Active"}, // Mute
    {7, {2}, ""}, // Mute
    {8, {0}, "Bypass"}, // Bypass
    {8, {1}, "Active"}, // Bypass
    {8, {2}, ""}, // Bypass
    {9, {0}, "1-Cycle"}, // Loop
    {9, {1}, "Loop"}, // Loop
    {9, {2}, ""}, // Loop
    {10, {0}, "Off"}, // Rec
    {10, {1}, "Rec On"}, // Rec
    {10, {2}, ""}, // Rec
    {11, {0}, "Off"}, // OffNum
    {11, {1}, "1"}, // OffNum
    {11, {64}, "64"}, // OffNum
    {11, {100}, "100"}, // OffNum
    {11, {127}, "127"}, // OffNum
    {12, {0}, "-180"}, // Phase
    {12, {1}, "-178"}, // Phase
    {12, {64}, "0"}, // Phase
    {12, {100}, "101"}, // Phase
    {12, {127}, "177"}, // Phase
    {13, {0}, "C-1"}, // Semitone
    {13, {1}, "C#-1"}, // Semitone
    {13, {64}, "E4"}, // Semitone
    {13, {100}, "E7"}, // Semitone
    {13, {127}, "G9"}, // Semitone
    {14, {0}, "C-1"}, // SemitoneKey
    {14, {1}, "C#-1"}, // SemitoneKey
    {14, {64}, "E4 Key"}, // SemitoneKey
    {14, {100}, "E7"}, // SemitoneKey
    {14, {127}, "G9"}, // SemitoneKey
    {15, {0}, "E-1"}, // SemitoneFilter
    {15, {1}, "F-1"}, // SemitoneFilter
    {15, {64}, "G#4"}, // SemitoneFilter
    {15, {100}, "G#7"}, // SemitoneFilter
    {15, {127}, "B9"}, // SemitoneFilter
    {16, {0}, "0.0"}, // UniPol
    {16, {1}, "0.5"}, // UniPol
    {16, {64}, "32.0"}, // UniPol
    {16, {100}, "50.0"}, // UniPol
    {16, {127}, "64.0"}, // UniPol
    {17, {0}, "-64"}, // BiPol
    {17, {1}, "-63"}, // BiPol
    {17, {64}, "0"}, // BiPol
    {17, {100}, "36"}, // BiPol
    {17, {127}, "64"}, // BiPol
    {18, {0}, "Bipol"}, // Polarity
    {18, {1}, "Unipol"}, // Polarity
    {18, {2}, ""}, // Polarity
    {19, {0}, "-64"}, // Sym
    {19, {1}, "-63"}, // Sym
    {19, {64}, "0"}, // Sym
    {19, {100}, "+36"}, // Sym
    {19, {127}, "+63"}, // Sym
    {20, {0}, "0.0ms"}, // DelayTime
    {20, {1}, "19.7ms"}, // DelayTime
    {20, {64}, "1.260s"}, // DelayTime
    {20, {100}, "1.969s"}, // DelayTime
    {20, {127}, "2.500s"}, // DelayTime
    {21, {0}, "10.30Hz"}, // FilterCutOff
    {21, {1}, "10.91Hz"}, // FilterCutOff
    {21, {64}, "415.3Hz"}, // FilterCutOff
    {21, {100}, "3.32kHz"}, // FilterCutOff
    {21, {127}, "15.8kHz"}, // FilterCutOff
    {22, {0}, "20.0Hz"}, // DrumMstFreq
    {22, {1}, "20.6Hz"}, // DrumMstFreq
    {22, {64}, "127Hz"}, // DrumMstFreq
    {22, {100}, "359Hz"}, // DrumMstFreq
    {22, {127}, "784Hz"}, // DrumMstFreq
    {23, {0}, "1:1"}, // DrumSlvMult
    {23, {1}, "x1.01"}, // DrumSlvMult
    {23, {64}, "x2.52"}, // DrumSlvMult
    {23, {100}, "x4.24"}, // DrumSlvMult
    {23, {127}, "x6.26"}, // DrumSlvMult
    {24, {0}, "12.00Hz"}, // OnePoleFreq
    {24, {1}, "12.72Hz"}, // OnePoleFreq
    {24, {64}, "504.4Hz"}, // OnePoleFreq
    {24, {100}, "4.13kHz"}, // OnePoleFreq
    {24, {127}, "20.0kHz"}, // OnePoleFreq
    {25, {0}, "Off"}, // KBT
    {25, {1}, "x0.02"}, // KBT
    {25, {64}, "Key"}, // KBT
    {25, {100}, "x1.56"}, // KBT
    {25, {127}, "x2"}, // KBT
    {26, {0}, "KBT Off"}, // KBTOnOff
    {26, {1}, "KBT On"}, // KBTOnOff
    {26, {2}, ""}, // KBTOnOff
    {27, {0}, "1%"}, // PulseWidth
    {27, {1}, "1%"}, // PulseWidth
    {27, {64}, "50%"}, // PulseWidth
    {27, {100}, "78%"}, // PulseWidth
    {27, {127}, "99%"}, // PulseWidth
    {28, {0}, " 0.5m"}, // EnvelopeTime
    {28, {1}, " 0.6m"}, // EnvelopeTime
    {28, {64}, "1.02s"}, // EnvelopeTime
    {28, {100}, "11.0s"}, // EnvelopeTime
    {28, {127}, "45.0s"}, // EnvelopeTime
    {29, {0}, "Exp"}, // EnvAttackType
    {29, {2}, "Log"}, // EnvAttackType
    {29, {3}, ""}, // EnvAttackType
    {30, {0}, "Sub"}, // LFORange
    {30, {4}, "ClkSync"}, // LFORange
    {30, {5}, ""}, // LFORange
    {31, {0}, "Sine"}, // LFOWaveFull
    {31, {4}, "Sqr"}, // LFOWaveFull
    {31, {5}, ""}, // LFOWaveFull
    {32, {0}, "x0.025"}, // LfoRateMult
    {32, {1}, "x0.026"}, // LfoRateMult
    {32, {64}, "1:1"}, // LfoRateMult
    {32, {100}, "8:1"}, // LfoRateMult
    {32, {127}, "x38.05"}, // LfoRateMult
    {33, {0}, "5ms"}, // Slide
    {33, {1}, "6ms"}, // Slide
    {33, {64}, "87ms"}, // Slide
    {33, {100}, "417ms"}, // Slide
    {33, {127}, "1355ms"}, // Slide
    {34, {0}, "0.3ms"}, // Smooth
    {34, {1}, "0.3ms"}, // Smooth
    {34, {64}, "10.3ms"}, // Smooth
    {34, {100}, "73.3ms"}, // Smooth
    {34, {127}, "318ms"}, // Smooth
    {35, {0}, "0"}, // PlusMinusHalfSteps
    {35, {1}, "\xC2\xB1""0.5"}, // PlusMinusHalfSteps
    {35, {64}, "\xC2\xB1""32.0"}, // PlusMinusHalfSteps
    {35, {100}, "\xC2\xB1""50.0"}, // PlusMinusHalfSteps
    {35, {127}, "\xC2\xB1""64"}, // PlusMinusHalfSteps
    {36, {0}, "-18.0dB"}, // EqGain
    {36, {1}, "-17.7dB"}, // EqGain
    {36, {64}, "0.0dB"}, // EqGain
    {36, {100}, "10.1dB"}, // EqGain
    {36, {127}, "18.0dB"}, // EqGain
    {37, {0}, "x0.25"}, // AmpGain
    {37, {1}, "x0.26"}, // AmpGain
    {37, {64}, "x1.00"}, // AmpGain
    {37, {100}, "x2.18"}, // AmpGain
    {37, {127}, "x4.00"}, // AmpGain
    {38, {0}, "20.00Hz"}, // EqFreq
    {38, {1}, "21.08Hz"}, // EqFreq
    {38, {64}, "580.8Hz"}, // EqFreq
    {38, {100}, "3.86kHz"}, // EqFreq
    {38, {127}, "16.0kHz"}, // EqFreq
    {39, {0}, "100.0Hz"}, // PhaserFreq
    {39, {1}, "104.1Hz"}, // PhaserFreq
    {39, {64}, "1.29kHz"}, // PhaserFreq
    {39, {100}, "5.44kHz"}, // PhaserFreq
    {39, {127}, "16.0kHz"}, // PhaserFreq
    {40, {0}, "32.70Hz"}, // SampleRate
    {40, {1}, "34.65Hz"}, // SampleRate
    {40, {64}, "1.32kHz"}, // SampleRate
    {40, {100}, "10.5kHz"}, // SampleRate
    {40, {127}, "50.2kHz"}, // SampleRate
    {41, {0}, "Sqr"}, // OscWaveFull
    {41, {3}, "Sine"}, // OscWaveFull
    {41, {4}, ""}, // OscWaveFull
    {42, {0}, "Off"}, // OscWaveSine
    {42, {1}, "Sine"}, // OscWaveSine
    {42, {2}, ""}, // OscWaveSine
    {43, {0}, "Trig"}, // TrigGate
    {43, {1}, "Gate"}, // TrigGate
    {43, {2}, ""}, // TrigGate
    {44, {0}, "Go"}, // GoStop
    {44, {1}, "Stop"}, // GoStop
    {44, {2}, ""}, // GoStop
    {45, {0}, "24BPM"}, // BPM
    {45, {1}, "26BPM"}, // BPM
    {45, {64}, "120BPM"}, // BPM
    {45, {100}, "160BPM"}, // BPM
    {45, {127}, "214BPM"}, // BPM
    {46, {0}, "Pos"}, // LevelShift
    {46, {7}, "BipInv"}, // LevelShift
    {46, {8}, ""}, // LevelShift
    {49, {0}, "L1"}, // EnvADDSRSusPlace
    {49, {1}, "L2"}, // EnvADDSRSusPlace
    {49, {2}, ""}, // EnvADDSRSusPlace
    {50, {0}, "L1"}, // EnvMultiSusPlace
    {50, {3}, "Trg"}, // EnvMultiSusPlace
    {50, {4}, ""}, // EnvMultiSusPlace
    {51, {0}, "Bipolar"}, // EnvMultiType
    {51, {2}, "Uni/Lin"}, // EnvMultiType
    {51, {3}, ""}, // EnvMultiType
    {52, {0}, "I1:127"}, // Fade2to1
    {52, {1}, "I1:126"}, // Fade2to1
    {52, {64}, "Mute"}, // Fade2to1
    {52, {100}, "I2:72"}, // Fade2to1
    {52, {127}, "I2:127"}, // Fade2to1
    {53, {0}, "O1:127"}, // Fade1to2
    {53, {1}, "O1:126"}, // Fade1to2
    {53, {64}, "Mute"}, // Fade1to2
    {53, {100}, "O2:72"}, // Fade1to2
    {53, {127}, "O2:127"}, // Fade1to2
    {55, {0}, "None"}, // AmRm
    {55, {1}, "1"}, // AmRm
    {55, {64}, "Am"}, // AmRm
    {55, {100}, "100"}, // AmRm
    {55, {127}, "Rm"}, // AmRm
    {57, {0}, "0dB"}, // dB_0_6_12
    {57, {2}, "-12dB"}, // dB_0_6_12
    {57, {3}, ""}, // dB_0_6_12
    {58, {0}, "Sine"}, // OscAWave
    {58, {5}, "Sqr10"}, // OscAWave
    {58, {6}, ""}, // OscAWave
    {59, {0}, "-50.0"}, // OscFine
    {59, {1}, "-49.2"}, // OscFine
    {59, {64}, " +0.0"}, // OscFine
    {59, {100}, "+28.1"}, // OscFine
    {59, {127}, "+49.2"}, // OscFine
    {62, {0}, "FM A"}, // FmType
    {62, {2}, "FM C"}, // FmType
    {62, {3}, ""}, // FmType
    {63, {0}, "Semi"}, // OscTuneMode
    {63, {4}, "Sub"}, // OscTuneMode
    {63, {5}, ""}, // OscTuneMode
    {67, {0}, "1/2"}, // OutBusBDest
    {67, {2}, "CVA"}, // OutBusBDest
    {67, {3}, ""}, // OutBusBDest
    {68, {0}, "\xC2\xB1""0"}, // PartialGen
    {68, {1}, "\xC2\xB1""0"}, // PartialGen
    {68, {64}, "\xC2\xB1""32"}, // PartialGen
    {68, {100}, "\xC2\xB1""50*"}, // PartialGen
    {68, {127}, "\xC2\xB1""63*"}, // PartialGen
    {69, {0}, "0-Oct"}, // NoteScaler
    {69, {1}, "\xC2\xB1""0.5"}, // NoteScaler
    {69, {64}, "\xC2\xB1""32.0"}, // NoteScaler
    {69, {100}, "\xC2\xB1""50.0"}, // NoteScaler
    {69, {127}, "\xC2\xB1""64"}, // NoteScaler
    {70, {0}, "-8.0dB"}, // NoteVelScale
    {70, {1}, "-7.9dB"}, // NoteVelScale
    {70, {64}, "0.0dB"}, // NoteVelScale
    {70, {100}, "4.5dB"}, // NoteVelScale
    {70, {127}, "8.0dB"}, // NoteVelScale
    {71, {0}, "KBT Off"}, // FltKBT
    {71, {4}, "KBT 100"}, // FltKBT
    {71, {5}, ""}, // FltKBT
    {72, {0}, "GC Off"}, // GCOnOff
    {72, {1}, "GC On"}, // GCOnOff
    {72, {2}, ""}, // GCOnOff
    {73, {0}, "6dB"}, // dB_6_12
    {73, {1}, "12dB"}, // dB_6_12
    {73, {2}, ""}, // dB_6_12
    {74, {0}, "12dB"}, // dB_12_24
    {74, {1}, "24dB"}, // dB_12_24
    {74, {2}, ""}, // dB_12_24
    {75, {0}, "LP"}, // FilterCType
    {75, {3}, "BR"}, // FilterCType
    {75, {4}, ""}, // FilterCType
    {76, {0}, "2.00Oct"}, // EQParBW
    {76, {1}, "1.98Oct"}, // EQParBW
    {76, {64}, "1.00Oct"}, // EQParBW
    {76, {100}, "0.44Oct"}, // EQParBW
    {76, {127}, "0.02Oct"}, // EQParBW
    {77, {0}, "Low"}, // HiLo
    {77, {1}, "High"}, // HiLo
    {77, {2}, ""}, // HiLo
    {78, {0}, "A"}, // Vowels
    {78, {8}, "OE"}, // Vowels
    {78, {9}, ""}, // Vowels
    {80, {0}, "Off"}, // Emphasis
    {80, {1}, "On"}, // Emphasis
    {80, {2}, ""}, // Emphasis
    {81, {0}, "Active"}, // VocoderMon
    {81, {1}, "Monitor"}, // VocoderMon
    {81, {2}, ""}, // VocoderMon
    {82, {0}, "Asym"}, // ClipSym
    {82, {1}, "Sym"}, // ClipSym
    {82, {2}, ""}, // ClipSym
    {83, {0}, "HalfPos"}, // DiodeModes
    {83, {3}, "FullNeg"}, // DiodeModes
    {83, {4}, ""}, // DiodeModes
    {84, {0}, "Inv x3"}, // ShaperModes
    {84, {3}, "x3"}, // ShaperModes
    {84, {4}, ""}, // ShaperModes
    {90, {0}, "Room"}, // ReverbType
    {90, {3}, "Gate"}, // ReverbType
    {90, {4}, ""}, // ReverbType
    {93, {0}, "12dB"}, // FilterCdB
    {93, {2}, "24dB"}, // FilterCdB
    {93, {3}, ""}, // FilterCdB
    {99, {0}, "0"}, // SwitchCtrl
    {99, {1}, "4"}, // SwitchCtrl
    {99, {64}, "256"}, // SwitchCtrl
    {99, {100}, "400"}, // SwitchCtrl
    {99, {127}, "508"}, // SwitchCtrl
    {105, {0}, "KBT Off"}, // LfoKBT
    {105, {4}, "KBT 100"}, // LfoKBT
    {105, {5}, ""}, // LfoKBT
    {106, {0}, "Sine"}, // LfoAWave
    {106, {5}, "Rnd"}, // LfoAWave
    {106, {6}, ""}, // LfoAWave
    {108, {0}, "1"}, // PatchMIDIOutChannel
    {108, {1}, "2"}, // PatchMIDIOutChannel
    {108, {64}, ""}, // PatchMIDIOutChannel
    {108, {100}, ""}, // PatchMIDIOutChannel
    {108, {127}, ""}, // PatchMIDIOutChannel
    {109, {0}, "1"}, // PatchMIDIInChannel
    {109, {1}, "2"}, // PatchMIDIInChannel
    {109, {64}, ""}, // PatchMIDIInChannel
    {109, {100}, ""}, // PatchMIDIInChannel
    {109, {127}, ""}, // PatchMIDIInChannel
    {111, {0}, "1 Oct"}, // ArpeggiatorRange
    {111, {3}, "4 Oct"}, // ArpeggiatorRange
    {111, {4}, ""}, // ArpeggiatorRange
    {112, {0}, "1/8"}, // ArpeggiatorRate
    {112, {3}, "1/16T"}, // ArpeggiatorRate
    {112, {4}, ""}, // ArpeggiatorRate
    {113, {0}, "Up"}, // ArpeggiatorMode
    {113, {3}, "Rnd"}, // ArpeggiatorMode
    {113, {4}, ""}, // ArpeggiatorMode
    {114, {0}, "0"}, // UniPolShort
    {114, {1}, "1"}, // UniPolShort
    {114, {64}, "64"}, // UniPolShort
    {114, {100}, "100"}, // UniPolShort
    {114, {127}, "127"}, // UniPolShort
    {115, {0}, "19m"}, // PortamentoTime
    {115, {1}, "20m"}, // PortamentoTime
    {115, {64}, "605m"}, // PortamentoTime
    {115, {100}, "1.39s"}, // PortamentoTime
    {115, {127}, "6.27s"}, // PortamentoTime
    {116, {0}, "Off"}, // PortamentoMode
    {116, {2}, "Auto"}, // PortamentoMode
    {116, {3}, ""}, // PortamentoMode
    {117, {0}, "Off"}, // BendOnOff
    {117, {1}, "On"}, // BendOnOff
    {117, {2}, ""}, // BendOnOff
    {118, {0}, "-78 dB"}, // VolumeDb
    {118, {1}, "-77 dB"}, // VolumeDb
    {118, {64}, "-23 dB"}, // VolumeDb
    {118, {100}, "-7.7 dB"}, // VolumeDb
    {118, {127}, "-0.0 dB"}, // VolumeDb
    {119, {0}, "  0 cnt"}, // VibratoAmount
    {119, {1}, "  1 cnt"}, // VibratoAmount
    {119, {64}, " 64 cnt"}, // VibratoAmount
    {119, {100}, "100 cnt"}, // VibratoAmount
    {119, {127}, "127 cnt"}, // VibratoAmount
    {120, {0}, "Off"}, // VibratoSource
    {120, {2}, "Wheel"}, // VibratoSource
    {120, {3}, ""}, // VibratoSource
    {121, {0}, "-2 oct"}, // OctShift
    {121, {1}, "-1 oct"}, // OctShift
    {121, {64}, "+62 oct"}, // OctShift
    {121, {100}, "+98 oct"}, // OctShift
    {121, {127}, "+125 oct"}, // OctShift
    {123, {0}, "13.75Hz"}, // FilterCutOff2
    {123, {1}, "14.57Hz"}, // FilterCutOff2
    {123, {64}, "554.4Hz"}, // FilterCutOff2
    {123, {100}, "4.43kHz"}, // FilterCutOff2
    {123, {127}, "21.1kHz"}, // FilterCutOff2
    {124, {0}, "0.50"}, // FilterResonance
    {124, {1}, "0.51"}, // FilterResonance
    {124, {64}, "1.67"}, // FilterResonance
    {124, {100}, "5.89"}, // FilterResonance
    {124, {127}, "50"}, // FilterResonance
    {125, {0}, "FM Lin"}, // FmKBT
    {125, {1}, "FM Trk"}, // FmKBT
    {125, {2}, ""}, // FmKBT
    {126, {0}, "50%"}, // PulseWidthHalf
    {126, {1}, "50%"}, // PulseWidthHalf
    {126, {64}, "75%"}, // PulseWidthHalf
    {126, {100}, "89%"}, // PulseWidthHalf
    {126, {127}, "99%"}, // PulseWidthHalf
    {127, {0}, "Bipol"}, // SignalType
    {127, {2}, "Neg"}, // SignalType
    {127, {3}, ""}, // SignalType
    {128, {0}, "1%"}, // LfoShape
    {128, {1}, "1%"}, // LfoShape
    {128, {64}, "50%"}, // LfoShape
    {128, {100}, "78%"}, // LfoShape
    {128, {127}, "98%"}, // LfoShape
    {129, {0}, "Fast"}, // EnvFollowerAttack
    {129, {1}, "0.53m"}, // EnvFollowerAttack
    {129, {64}, "23.0m"}, // EnvFollowerAttack
    {129, {100}, "199m"}, // EnvFollowerAttack
    {129, {127}, "1.00s"}, // EnvFollowerAttack
    {130, {0}, "10.0m"}, // EnvFollowerRelease
    {130, {1}, "10.5m"}, // EnvFollowerRelease
    {130, {64}, "177m"}, // EnvFollowerRelease
    {130, {100}, "894m"}, // EnvFollowerRelease
    {130, {127}, "3.00s"}, // EnvFollowerRelease
    {131, {0}, "62.9s"}, // LFOFreq1
    {131, {1}, "59.4s"}, // LFOFreq1
    {131, {64}, "0.64Hz"}, // LFOFreq1
    {131, {100}, "5.13Hz"}, // LFOFreq1
    {131, {127}, "24.4Hz"}, // LFOFreq1
    {132, {0}, "\xC2\xB1""0.0"}, // NotePlusMinus
    {132, {1}, "\xC2\xB1""0.5"}, // NotePlusMinus
    {132, {64}, "\xC2\xB1""32.0"}, // NotePlusMinus
    {132, {100}, "\xC2\xB1""50.0"}, // NotePlusMinus
    {132, {127}, "\xC2\xB1""64.0"}, // NotePlusMinus
    {134, {0}, "Off"}, // XFade
    {134, {3}, "100%"}, // XFade
    {134, {4}, ""}, // XFade
    {135, {0}, "Pos"}, // PosNeg
    {135, {1}, "Neg"}, // PosNeg
    {135, {2}, ""}, // PosNeg
    {136, {0}, "LogExp"}, // EnvAttackDecayShape
    {136, {3}, "LinLin"}, // EnvAttackDecayShape
    {136, {4}, ""}, // EnvAttackDecayShape
    {138, {0}, "Normal"}, // EnvReset
    {138, {1}, "Reset"}, // EnvReset
    {138, {2}, ""}, // EnvReset
    {139, {0}, "AD"}, // EnvADRSust
    {139, {1}, "AR"}, // EnvADRSust
    {139, {2}, ""}, // EnvADRSust
    {144, {0}, "Time"}, // DelayTimeMode
    {144, {1}, "ClkSync"}, // DelayTimeMode
    {144, {2}, ""}, // DelayTimeMode
    {148, {0}, "Lin"}, // AmpType
    {148, {1}, "dB"}, // AmpType
    {148, {2}, ""}, // AmpType
    {149, {0}, "+6 dB"}, // InputPad
    {149, {3}, "-12dB"}, // InputPad
    {149, {4}, ""}, // InputPad
    {150, {0}, "In 1/2"}, // In2Src
    {150, {3}, "Bus 3/4"}, // In2Src
    {150, {4}, ""}, // In2Src
    {151, {0}, "In"}, // In4Src
    {151, {1}, "Bus"}, // In4Src
    {151, {2}, ""}, // In4Src
    {152, {0}, "FX 1/2"}, // InCVA
    {152, {1}, "FX 3/4"}, // InCVA
    {152, {2}, ""}, // InCVA
    {153, {0}, "-oo"}, // dB
    {153, {1}, "-99.9"}, // dB
    {153, {64}, "-17.6"}, // dB
    {153, {100}, "-6.2"}, // dB
    {153, {127}, "-0"}, // dB
    {155, {0}, "Sine1"}, // OscSinShpWave
    {155, {5}, "Pulse"}, // OscSinShpWave
    {155, {6}, ""}, // OscSinShpWave
    {156, {0}, "Sine"}, // OscBWave
    {156, {4}, "DualSaw"}, // OscBWave
    {156, {5}, ""}, // OscBWave
    {157, {0}, "Log"}, // LogLin
    {157, {1}, "Lin"}, // LogLin
    {157, {2}, ""}, // LogLin
    {158, {0}, "C-Rate"}, // PortamentoType
    {158, {1}, "C-Time"}, // PortamentoType
    {158, {2}, ""}, // PortamentoType
    {159, {0}, " 0.2m"}, // NoiseGateAtkTime
    {159, {1}, " 0.3m"}, // NoiseGateAtkTime
    {159, {64}, "28.0m"}, // NoiseGateAtkTime
    {159, {100}, "63.7m"}, // NoiseGateAtkTime
    {159, {127}, " 100m"}, // NoiseGateAtkTime
    {160, {0}, "0.50m"}, // NoiseGateRelTime
    {160, {1}, "0.59m"}, // NoiseGateRelTime
    {160, {64}, "86.4m"}, // NoiseGateRelTime
    {160, {100}, "404m"}, // NoiseGateRelTime
    {160, {127}, "1.00s"}, // NoiseGateRelTime
    {162, {0}, "1 semi"}, // BendRange
    {162, {1}, "2 semi"}, // BendRange
    {162, {64}, "65 semi"}, // BendRange
    {162, {100}, "101 semi"}, // BendRange
    {162, {127}, "128 semi"}, // BendRange
    {163, {0}, "0"}, // LFOPhase
    {163, {1}, "3"}, // LFOPhase
    {163, {64}, "180"}, // LFOPhase
    {163, {100}, "281"}, // LFOPhase
    {163, {127}, "357"}, // LFOPhase
    {164, {0}, "Sine"}, // LfoBWave
    {164, {3}, "Sqr"}, // LfoBWave
    {164, {4}, ""}, // LfoBWave
    {165, {0}, "Sine"}, // LfoCWave
    {165, {5}, "Sqr"}, // LfoCWave
    {165, {6}, ""}, // LfoCWave
    {166, {0}, "1"}, // ClkGenSync
    {166, {5}, "32"}, // ClkGenSync
    {166, {6}, ""}, // ClkGenSync
    {167, {0}, "Soft"}, // OverDriveType
    {167, {3}, "Heavy"}, // OverDriveType
    {167, {4}, ""}, // OverDriveType
    {168, {0}, "Asym"}, // OverDriveSym
    {168, {1}, "Sym"}, // OverDriveSym
    {168, {2}, ""}, // OverDriveSym
    {169, {0}, "Exp"}, // ExpLin
    {169, {2}, "dB"}, // ExpLin
    {169, {3}, ""}, // ExpLin
    {170, {0}, "Type I"}, // PhaserType
    {170, {1}, "Type II"}, // PhaserType
    {170, {2}, ""}, // PhaserType
    {171, {0}, "4.00 Hz"}, // VibratoRate
    {171, {1}, "4.03 Hz"}, // VibratoRate
    {171, {64}, "6.02 Hz"}, // VibratoRate
    {171, {100}, "7.15 Hz"}, // VibratoRate
    {171, {127}, "8.00 Hz"}, // VibratoRate
    {172, {0}, "Notch"}, // FltVariant
    {172, {2}, "Deep"}, // FltVariant
    {172, {3}, ""}, // FltVariant
    {173, {0}, "8.1758Hz"}, // OscFreqSingle
    {173, {1}, "8.6620Hz"}, // OscFreqSingle
    {173, {64}, "329.63Hz"}, // OscFreqSingle
    {173, {100}, "2.637kHz"}, // OscFreqSingle
    {173, {127}, "12.55kHz"}, // OscFreqSingle
    {174, {0}, "Fast"}, // CompressorAttack
    {174, {1}, "0.53m"}, // CompressorAttack
    {174, {64}, "20.2m"}, // CompressorAttack
    {174, {100}, " 161m"}, // CompressorAttack
    {174, {127}, " 767m"}, // CompressorAttack
    {175, {0}, " 125m"}, // CompressorRelease
    {175, {1}, " 129m"}, // CompressorRelease
    {175, {64}, "1.15s"}, // CompressorRelease
    {175, {100}, "4.00s"}, // CompressorRelease
    {175, {127}, "10.2s"}, // CompressorRelease
    {176, {0}, "-30dB"}, // CompressorThreshold
    {176, {1}, "-29dB"}, // CompressorThreshold
    {176, {64}, " 34dB"}, // CompressorThreshold
    {176, {100}, " 70dB"}, // CompressorThreshold
    {176, {127}, " 97dB"}, // CompressorThreshold
    {177, {0}, "1.0:1"}, // CompressorRatio
    {177, {1}, "1.1:1"}, // CompressorRatio
    {177, {64}, " 70:1"}, // CompressorRatio
    {177, {100}, " 250:1"}, // CompressorRatio
    {177, {127}, " 385:1"}, // CompressorRatio
    {178, {0}, "-30dB"}, // CompressorLevel
    {178, {1}, "-29dB"}, // CompressorLevel
    {178, {64}, " 34dB"}, // CompressorLevel
    {178, {100}, " 70dB"}, // CompressorLevel
    {178, {127}, " 97dB"}, // CompressorLevel
    {179, {0}, "0"}, // MIDIValue
    {179, {1}, "1"}, // MIDIValue
    {179, {64}, "64"}, // MIDIValue
    {179, {100}, "100"}, // MIDIValue
    {179, {127}, "127"}, // MIDIValue
    {180, {0}, " 0 dB"}, // OutPad
    {180, {3}, "+18dB"}, // OutPad
    {180, {4}, ""}, // OutPad
    {181, {0}, "Last"}, // MonoKeybPrio
    {181, {2}, "High"}, // MonoKeybPrio
    {181, {3}, ""}, // MonoKeybPrio
    {182, {0}, "Out 1/2"}, // Out2Dest
    {182, {5}, "Bus 3/4"}, // Out2Dest
    {182, {6}, ""}, // Out2Dest
    {183, {0}, "Out"}, // Out4Dest
    {183, {2}, "Bus"}, // Out4Dest
    {183, {3}, ""}, // Out4Dest
    {184, {0}, "100Hz"}, // EqFreq100To8k
    {184, {1}, "104Hz"}, // EqFreq100To8k
    {184, {64}, "910Hz"}, // EqFreq100To8k
    {184, {100}, "3.15kHz"}, // EqFreq100To8k
    {184, {127}, "8.00kHz"}, // EqFreq100To8k
    {185, {0}, "0"}, // OscPhase
    {185, {1}, "3"}, // OscPhase
    {185, {64}, "180"}, // OscPhase
    {185, {100}, "281"}, // OscPhase
    {185, {127}, "357"}, // OscPhase
    {186, {0}, "Inact"}, // InactiveActive
    {186, {1}, "Active"}, // InactiveActive
    {186, {2}, ""}, // InactiveActive
    {187, {0}, "Intern"}, // ClkGenSource
    {187, {127}, "Master"}, // ClkGenSource
    {188, {0}, "0dB"}, // Pad
    {188, {2}, "-12dB"}, // Pad
    {188, {3}, ""}, // Pad
    {189, {0}, "m"}, // ModAmountMode
    {189, {1}, "1-m"}, // ModAmountMode
    {189, {2}, ""}, // ModAmountMode
    {190, {0}, "50.0%"}, // ClkGenSwing
    {190, {1}, "50.2%"}, // ClkGenSwing
    {190, {64}, "62.6%"}, // ClkGenSwing
    {190, {100}, "69.7%"}, // ClkGenSwing
    {190, {127}, "75.0%"}, // ClkGenSwing
    {191, {0}, "0%"}, // From0to200Percent
    {191, {1}, "1.6%"}, // From0to200Percent
    {191, {64}, "100%"}, // From0to200Percent
    {191, {100}, "156.2%"}, // From0to200Percent
    {191, {127}, "200%"}, // From0to200Percent
    {192, {0}, "x2"}, // ShapeExpCurve
    {192, {3}, "x5"}, // ShapeExpCurve
    {192, {4}, ""}, // ShapeExpCurve
    {193, {0}, "Normal"}, // PTrackAlgorithm
    {193, {1}, "HiPitch"}, // PTrackAlgorithm
    {193, {2}, ""}, // PTrackAlgorithm
    {194, {0}, "Closest"}, // KeyQuantCaptureRange
    {194, {1}, "Evenly"}, // KeyQuantCaptureRange
    {194, {2}, ""}, // KeyQuantCaptureRange
    {195, {0}, "1"}, // DigitizerBits
    {195, {1}, "2"}, // DigitizerBits
    {195, {64}, "65"}, // DigitizerBits
    {195, {100}, "101"}, // DigitizerBits
    {195, {127}, "128"}, // DigitizerBits
    {196, {0}, "-7"}, // DXDetune
    {196, {1}, "-6"}, // DXDetune
    {196, {64}, "57"}, // DXDetune
    {196, {100}, "93"}, // DXDetune
    {196, {127}, "120"}, // DXDetune
    {197, {0}, "A-1"}, // DXBreakPoint
    {197, {1}, "Bb-1"}, // DXBreakPoint
    {197, {64}, "C#5"}, // DXBreakPoint
    {197, {100}, "C#8"}, // DXBreakPoint
    {197, {127}, "E10"}, // DXBreakPoint
    {199, {0}, "-Lin"}, // DXCurve
    {199, {3}, "+Lin"}, // DXCurve
    {199, {4}, ""}, // DXCurve
    {200, {0}, "Ratio"}, // DXTuneMode
    {200, {1}, "Fixed"}, // DXTuneMode
    {200, {2}, ""}, // DXTuneMode
    {202, {0}, "12.5m"}, // PitchShiftDelay
    {202, {3}, "100m"}, // PitchShiftDelay
    {202, {4}, ""}, // PitchShiftDelay
    {203, {0}, "0%"}, // RndSmooth
    {203, {4}, "100%"}, // RndSmooth
    {203, {5}, ""}, // RndSmooth
    {204, {0}, "Bip"}, // RndLevelShift
    {204, {2}, "Neg"}, // RndLevelShift
    {204, {3}, ""}, // RndLevelShift
    {205, {0}, "0%"}, // RndStep
    {205, {1}, "0%"}, // RndStep
    {205, {64}, "50%"}, // RndStep
    {205, {100}, "78%"}, // RndStep
    {205, {127}, "100%"}, // RndStep
    {206, {0}, "0%"}, // RndDistribution
    {206, {1}, "0%"}, // RndDistribution
    {206, {64}, "50%"}, // RndDistribution
    {206, {100}, "78%"}, // RndDistribution
    {206, {127}, "100%"}, // RndDistribution
    {207, {0}, "25%"}, // RndStepSwitch
    {207, {3}, "100%"}, // RndStepSwitch
    {207, {4}, ""}, // RndStepSwitch
    {208, {0}, "10%"}, // RndLogic
    {208, {8}, "90%"}, // RndLogic
    {208, {9}, ""}, // RndLogic
    {209, {0}, "- x4.00"}, // PitchShuttle
    {209, {1}, "- x3.94"}, // PitchShuttle
    {209, {64}, "x0"}, // PitchShuttle
    {209, {100}, "x2.25"}, // PitchShuttle
    {209, {127}, "x4.00"}, // PitchShuttle
    {210, {0}, "Notes"}, // MIDIFilter
    {210, {1}, "Note+CC"}, // MIDIFilter
    {210, {2}, ""}, // MIDIFilter
    {211, {0}, "EchoOff"}, // MIDIEcho
    {211, {1}, "EchoOn"}, // MIDIEcho
    {211, {2}, ""}, // MIDIEcho
    {212, {0}, "0%"}, // RndDensity
    {212, {1}, "0%"}, // RndDensity
    {212, {64}, "50%"}, // RndDensity
    {212, {100}, "78%"}, // RndDensity
    {212, {127}, "100%"}, // RndDensity
    {213, {0}, "80 Hz"}, // EqFreqLo
    {213, {2}, "160 Hz"}, // EqFreqLo
    {213, {3}, ""}, // EqFreqLo
    {214, {0}, "6 kHz"}, // EqFreqHi
    {214, {2}, "12 kHz"}, // EqFreqHi
    {214, {3}, ""}, // EqFreqHi
    {215, {0}, "0.01Hz"}, // FlangerRate
    {215, {1}, "0.02Hz"}, // FlangerRate
    {215, {64}, "1.46Hz"}, // FlangerRate
    {215, {100}, "2.29Hz"}, // FlangerRate
    {215, {127}, "2.91Hz"}, // FlangerRate
    {216, {0}, "0.05Hz"}, // PhaserRate
    {216, {1}, "0.05Hz"}, // PhaserRate
    {216, {64}, "2.98Hz"}, // PhaserRate
    {216, {100}, "7.20Hz"}, // PhaserRate
    {216, {127}, "11.6Hz"}, // PhaserRate
    {218, {0}, "LP"}, // DrumFilterType
    {218, {2}, "HP"}, // DrumFilterType
    {218, {3}, ""}, // DrumFilterType
    {219, {0}, "Off"}, // OffOnOn
    {219, {2}, "On"}, // OffOnOn
    {219, {3}, ""}, // OffOnOn
    {220, {0}, "-oo"}, // NoiseGateLevel
    {220, {1}, "-42.1dB"}, // NoiseGateLevel
    {220, {64}, "-6.0dB"}, // NoiseGateLevel
    {220, {100}, "-2.1dB"}, // NoiseGateLevel
    {220, {127}, "-0dB"}, // NoiseGateLevel
    {221, {0}, "Knob"}, // MorphGroupSource1
    {221, {1}, "Wheel"}, // MorphGroupSource1
    {221, {2}, ""}, // MorphGroupSource1
    {222, {0}, "Knob"}, // MorphGroupSource2
    {222, {1}, "Vel"}, // MorphGroupSource2
    {222, {2}, ""}, // MorphGroupSource2
    {223, {0}, "Knob"}, // MorphGroupSource3
    {223, {1}, "Keyb"}, // MorphGroupSource3
    {223, {2}, ""}, // MorphGroupSource3
    {224, {0}, "Knob"}, // MorphGroupSource4
    {224, {1}, "Aft.Tch"}, // MorphGroupSource4
    {224, {2}, ""}, // MorphGroupSource4
    {225, {0}, "Knob"}, // MorphGroupSource5
    {225, {2}, "G.Wh 1"}, // MorphGroupSource5
    {225, {3}, ""}, // MorphGroupSource5
    {226, {0}, "Knob"}, // MorphGroupSource6
    {226, {1}, "Ctrl.Pd"}, // MorphGroupSource6
    {226, {2}, ""}, // MorphGroupSource6
    {227, {0}, "Knob"}, // MorphGroupSource7
    {227, {1}, "P.Stick"}, // MorphGroupSource7
    {227, {2}, ""}, // MorphGroupSource7
    {228, {0}, "Knob"}, // MorphGroupSource8
    {228, {1}, "G.Wh 2"}, // MorphGroupSource8
    {228, {2}, ""}, // MorphGroupSource8
    {0, {0, 0}, "0 Hz"}, // OscSubFreq
    {0, {64, 0}, "0.1564Hz"}, // OscSubFreq
    {0, {127, 0}, "5.9506Hz"}, // OscSubFreq
    {0, {0, 1}, "0 Hz"}, // OscSubFreq
    {0, {64, 1}, "0.1564Hz"}, // OscSubFreq
    {0, {127, 1}, "5.9533Hz"}, // OscSubFreq
    {0, {0, 2}, "0 Hz"}, // OscSubFreq
    {0, {64, 2}, "0.1565Hz"}, // OscSubFreq
    {0, {127, 2}, "5.9559Hz"}, // OscSubFreq
    {0, {20, 64}, "0.0127Hz"}, // OscSubFreq
    {0, {69, 64}, "0.2148Hz"}, // OscSubFreq
    {0, {100, 0}, "1.2510Hz"}, // OscSubFreq
    {0, {127, 127}, "6.3016Hz"}, // OscSubFreq
    {1, {0, 0}, "0"}, // DualDefault
    {1, {127, 0}, "127"}, // DualDefault
    {1, {0, 3}, "0"}, // DualDefault
    {1, {127, 3}, "127"}, // DualDefault
    {65, {0, 0}, "-64  -50"}, // OscSemi2
    {65, {64, 0}, " +0  -50"}, // OscSemi2
    {65, {127, 0}, "+63  -50"}, // OscSemi2
    {65, {0, 1}, "-64  -49"}, // OscSemi2
    {65, {64, 1}, " +0  -49"}, // OscSemi2
    {65, {127, 1}, "+63  -49"}, // OscSemi2
    {65, {0, 2}, "-64  -48"}, // OscSemi2
    {65, {64, 2}, " +0  -48"}, // OscSemi2
    {65, {127, 2}, "+63  -48"}, // OscSemi2
    {65, {20, 64}, "-44   +0"}, // OscSemi2
    {65, {69, 64}, " +5   +0"}, // OscSemi2
    {65, {100, 0}, "+36  -50"}, // OscSemi2
    {65, {127, 127}, "+63  +49"}, // OscSemi2
    {95, {0, 0}, "-128"}, // LevMult
    {95, {64, 0}, "0"}, // LevMult
    {95, {127, 0}, "0"}, // LevMult
    {95, {0, 1}, "-126"}, // LevMult
    {95, {64, 1}, "0.8"}, // LevMult
    {95, {127, 1}, "0.8"}, // LevMult
    {95, {0, 2}, "-124"}, // LevMult
    {95, {64, 2}, "1.6"}, // LevMult
    {95, {127, 2}, "1.6"}, // LevMult
    {95, {20, 64}, "50"}, // LevMult
    {95, {69, 64}, "50"}, // LevMult
    {95, {100, 0}, "0"}, // LevMult
    {95, {127, 127}, "100"}, // LevMult
    {96, {0, 0}, "-64"}, // BiUniPol
    {96, {64, 0}, "0"}, // BiUniPol
    {96, {127, 0}, "64"}, // BiUniPol
    {96, {0, 1}, "0.0"}, // BiUniPol
    {96, {64, 1}, "32.0"}, // BiUniPol
    {96, {127, 1}, "64.0"}, // BiUniPol
    {96, {0, 2}, "0.0"}, // BiUniPol
    {96, {64, 2}, "32.0"}, // BiUniPol
    {96, {127, 2}, "64.0"}, // BiUniPol
    {96, {20, 64}, "10.0"}, // BiUniPol
    {96, {69, 64}, "34.5"}, // BiUniPol
    {96, {100, 0}, "36"}, // BiUniPol
    {96, {127, 127}, "64.0"}, // BiUniPol
    {97, {0, 0}, "8.1758Hz"}, // OscSyncTimbre
    {97, {64, 0}, "+0"}, // OscSyncTimbre
    {97, {127, 0}, "+0"}, // OscSyncTimbre
    {97, {0, 1}, "8.6620Hz"}, // OscSyncTimbre
    {97, {64, 1}, "+1"}, // OscSyncTimbre
    {97, {127, 1}, "+1"}, // OscSyncTimbre
    {97, {0, 2}, "9.1770Hz"}, // OscSyncTimbre
    {97, {64, 2}, "+2"}, // OscSyncTimbre
    {97, {127, 2}, "+2"}, // OscSyncTimbre
    {97, {20, 64}, "+64"}, // OscSyncTimbre
    {97, {69, 64}, "+64"}, // OscSyncTimbre
    {97, {100, 0}, "+0"}, // OscSyncTimbre
    {97, {127, 127}, "+127"}, // OscSyncTimbre
    {98, {0, 0}, "8.1758Hz"}, // OscPulseTimbre
    {98, {64, 0}, "+0"}, // OscPulseTimbre
    {98, {127, 0}, "+0"}, // OscPulseTimbre
    {98, {0, 1}, "8.6620Hz"}, // OscPulseTimbre
    {98, {64, 1}, "+1"}, // OscPulseTimbre
    {98, {127, 1}, "+1"}, // OscPulseTimbre
    {98, {0, 2}, "9.1770Hz"}, // OscPulseTimbre
    {98, {64, 2}, "+2"}, // OscPulseTimbre
    {98, {127, 2}, "+2"}, // OscPulseTimbre
    {98, {20, 64}, "+64"}, // OscPulseTimbre
    {98, {69, 64}, "+64"}, // OscPulseTimbre
    {98, {100, 0}, "+0"}, // OscPulseTimbre
    {98, {127, 127}, "+127"}, // OscPulseTimbre
    {102, {0, 0}, "0"}, // dBLin
    {102, {64, 0}, "50"}, // dBLin
    {102, {127, 0}, "100"}, // dBLin
    {102, {0, 1}, "0"}, // dBLin
    {102, {64, 1}, "50"}, // dBLin
    {102, {127, 1}, "100"}, // dBLin
    {102, {0, 2}, "-oo"}, // dBLin
    {102, {64, 2}, "-17.6"}, // dBLin
    {102, {127, 2}, "-0"}, // dBLin
    {102, {20, 64}, "15.6"}, // dBLin
    {102, {69, 64}, "53.9"}, // dBLin
    {102, {100, 0}, "78.1"}, // dBLin
    {102, {127, 127}, "100"}, // dBLin
    {103, {0, 0}, "699s"}, // LFOFreq
    {103, {64, 0}, "10.8s"}, // LFOFreq
    {103, {127, 0}, "5.46s"}, // LFOFreq
    {103, {0, 1}, "62.9s"}, // LFOFreq
    {103, {64, 1}, "0.64Hz"}, // LFOFreq
    {103, {127, 1}, "24.4Hz"}, // LFOFreq
    {103, {0, 2}, "0.26Hz"}, // LFOFreq
    {103, {64, 2}, "10.3Hz"}, // LFOFreq
    {103, {127, 2}, "392Hz"}, // LFOFreq
    {103, {0, 3}, "24"}, // LFOFreq
    {103, {64, 3}, "120"}, // LFOFreq
    {103, {127, 3}, "214"}, // LFOFreq
    {103, {0, 4}, "64/1"}, // LFOFreq
    {103, {64, 4}, "1/4D"}, // LFOFreq
    {103, {127, 4}, "1/64T"}, // LFOFreq
    {107, {0, 0}, "0.0ms"}, // ReverbTime
    {107, {64, 0}, "1.512s"}, // ReverbTime
    {107, {127, 0}, "3.000s"}, // ReverbTime
    {107, {0, 1}, "0.0ms"}, // ReverbTime
    {107, {64, 1}, "3.024s"}, // ReverbTime
    {107, {127, 1}, "6.000s"}, // ReverbTime
    {107, {0, 2}, "0.0ms"}, // ReverbTime
    {107, {64, 2}, "4.535s"}, // ReverbTime
    {107, {127, 2}, "9.000s"}, // ReverbTime
    {107, {0, 3}, "0.0ms"}, // ReverbTime
    {107, {64, 3}, "6.047s"}, // ReverbTime
    {107, {127, 3}, "12.00s"}, // ReverbTime
    {107, {0, 4}, "0.0ms"}, // ReverbTime
    {107, {64, 4}, "7.559s"}, // ReverbTime
    {107, {127, 4}, "15.00s"}, // ReverbTime
    {122, {0, 0}, "0.10m"}, // LogicTime
    {122, {64, 0}, "9.52m"}, // LogicTime
    {122, {127, 0}, "1.00s"}, // LogicTime
    {122, {0, 1}, "1.04m"}, // LogicTime
    {122, {64, 1}, "95.2m"}, // LogicTime
    {122, {127, 1}, "10.00s"}, // LogicTime
    {122, {0, 2}, "10.4m"}, // LogicTime
    {122, {64, 2}, "952m"}, // LogicTime
    {122, {127, 2}, "100.0s"}, // LogicTime
    {122, {20, 64}, "41.7m"}, // LogicTime
    {122, {69, 64}, "1.37s"}, // LogicTime
    {122, {100, 0}, "133m"}, // LogicTime
    {122, {127, 127}, "100.0s"}, // LogicTime
    {133, {0, 0}, "-64"}, // BiUniPolCompact
    {133, {64, 0}, "0"}, // BiUniPolCompact
    {133, {127, 0}, "64"}, // BiUniPolCompact
    {133, {0, 1}, "0"}, // BiUniPolCompact
    {133, {64, 1}, "32"}, // BiUniPolCompact
    {133, {127, 1}, "64"}, // BiUniPolCompact
    {133, {0, 2}, "0"}, // BiUniPolCompact
    {133, {64, 2}, "32"}, // BiUniPolCompact
    {133, {127, 2}, "64"}, // BiUniPolCompact
    {133, {20, 64}, "10"}, // BiUniPolCompact
    {133, {69, 64}, "34."}, // BiUniPolCompact
    {133, {100, 0}, "36"}, // BiUniPolCompact
    {133, {127, 127}, "64"}, // BiUniPolCompact
    {137, {0, 0}, "0.0"}, // MultiEnvBiUni
    {137, {64, 0}, "32.0"}, // MultiEnvBiUni
    {137, {127, 0}, "64.0"}, // MultiEnvBiUni
    {137, {0, 1}, "0.0"}, // MultiEnvBiUni
    {137, {64, 1}, "32.0"}, // MultiEnvBiUni
    {137, {127, 1}, "64.0"}, // MultiEnvBiUni
    {137, {0, 2}, "0.0"}, // MultiEnvBiUni
    {137, {64, 2}, "32.0"}, // MultiEnvBiUni
    {137, {127, 2}, "64.0"}, // MultiEnvBiUni
    {137, {20, 64}, "-44"}, // MultiEnvBiUni
    {137, {69, 64}, "5"}, // MultiEnvBiUni
    {137, {100, 0}, "50.0"}, // MultiEnvBiUni
    {137, {127, 127}, "64"}, // MultiEnvBiUni
    {141, {0, 0}, "0.01m"}, // DelayTime2
    {141, {64, 0}, "2.68m"}, // DelayTime2
    {141, {127, 0}, "5.30m"}, // DelayTime2
    {141, {0, 1}, "0.01m"}, // DelayTime2
    {141, {64, 1}, "12.7m"}, // DelayTime2
    {141, {127, 1}, "25.1m"}, // DelayTime2
    {141, {0, 2}, "0.01m"}, // DelayTime2
    {141, {64, 2}, "50.7m"}, // DelayTime2
    {141, {127, 2}, "101m"}, // DelayTime2
    {141, {0, 3}, "0.01m"}, // DelayTime2
    {141, {64, 3}, "252m"}, // DelayTime2
    {141, {127, 3}, "500m"}, // DelayTime2
    {141, {0, 4}, "0.01m"}, // DelayTime2
    {141, {64, 4}, "504m"}, // DelayTime2
    {141, {127, 4}, "1.000s"}, // DelayTime2
    {142, {0, 0}, "0.000Hz"}, // BodeFreq
    {142, {64, 0}, "1.12Hz"}, // BodeFreq
    {142, {127, 0}, "8.78Hz"}, // BodeFreq
    {142, {0, 1}, "0.000Hz"}, // BodeFreq
    {142, {64, 1}, "12.5Hz"}, // BodeFreq
    {142, {127, 1}, "97.6Hz"}, // BodeFreq
    {142, {0, 2}, "0.000Hz"}, // BodeFreq
    {142, {64, 2}, "201Hz"}, // BodeFreq
    {142, {127, 2}, "1568Hz"}, // BodeFreq
    {142, {20, 64}, "0.004Hz"}, // BodeFreq
    {142, {69, 64}, "0.16Hz"}, // BodeFreq
    {142, {100, 0}, "4.29Hz"}, // BodeFreq
    {142, {127, 127}, "1.00Hz"}, // BodeFreq
    {145, {0, 0}, "0.00m"}, // DelayTimeTap8
    {145, {64, 0}, "0.33m"}, // DelayTimeTap8
    {145, {127, 0}, "0.66m"}, // DelayTimeTap8
    {145, {0, 1}, "0.00m"}, // DelayTimeTap8
    {145, {64, 1}, "1.58m"}, // DelayTimeTap8
    {145, {127, 1}, "3.14m"}, // DelayTimeTap8
    {145, {0, 2}, "0.00m"}, // DelayTimeTap8
    {145, {64, 2}, "6.33m"}, // DelayTimeTap8
    {145, {127, 2}, "12.6m"}, // DelayTimeTap8
    {145, {0, 3}, "0.00m"}, // DelayTimeTap8
    {145, {64, 3}, "31.5m"}, // DelayTimeTap8
    {145, {127, 3}, "62.5m"}, // DelayTimeTap8
    {145, {0, 4}, "0.00m"}, // DelayTimeTap8
    {145, {64, 4}, "63.0m"}, // DelayTimeTap8
    {145, {127, 4}, "125m"}, // DelayTimeTap8
    {147, {0, 0}, "x0.00"}, // AmpGainNew
    {147, {64, 0}, "x1.00"}, // AmpGainNew
    {147, {127, 0}, "x4.00"}, // AmpGainNew
    {147, {0, 1}, "-oo"}, // AmpGainNew
    {147, {64, 1}, "0.0"}, // AmpGainNew
    {147, {127, 1}, "12.0"}, // AmpGainNew
    {147, {0, 2}, "-oo"}, // AmpGainNew
    {147, {64, 2}, "0.0"}, // AmpGainNew
    {147, {127, 2}, "12.0"}, // AmpGainNew
    {147, {20, 64}, "-13.6"}, // AmpGainNew
    {147, {69, 64}, "0.9"}, // AmpGainNew
    {147, {100, 0}, "x2.19"}, // AmpGainNew
    {147, {127, 127}, "12.0"}, // AmpGainNew
    {201, {0, 0}, "-16.0  -50"}, // PitchShift
    {201, {64, 0}, "+0.0  -50"}, // PitchShift
    {201, {127, 0}, "+15.8  -50"}, // PitchShift
    {201, {0, 1}, "-16.0  -49"}, // PitchShift
    {201, {64, 1}, "+0.0  -49"}, // PitchShift
    {201, {127, 1}, "+15.8  -49"}, // PitchShift
    {201, {0, 2}, "-16.0  -48"}, // PitchShift
    {201, {64, 2}, "+0.0  -48"}, // PitchShift
    {201, {127, 2}, "+15.8  -48"}, // PitchShift
    {201, {20, 64}, "-11.0   +0"}, // PitchShift
    {201, {69, 64}, "+1.2   +0"}, // PitchShift
    {201, {100, 0}, "+9.0  -50"}, // PitchShift
    {201, {127, 127}, "+15.8  +49"}, // PitchShift
    {217, {0, 0}, "0.2m"}, // GlideTimeRate
    {217, {64, 0}, "511m"}, // GlideTimeRate
    {217, {127, 0}, "22.4s"}, // GlideTimeRate
    {217, {0, 1}, "0.2m/oct"}, // GlideTimeRate
    {217, {64, 1}, "480m/oct"}, // GlideTimeRate
    {217, {127, 1}, "23.5s/oct"}, // GlideTimeRate
    {217, {0, 2}, "0.2m/oct"}, // GlideTimeRate
    {217, {64, 2}, "480m/oct"}, // GlideTimeRate
    {217, {127, 2}, "23.5s/oct"}, // GlideTimeRate
    {217, {20, 64}, "5.9m/oct"}, // GlideTimeRate
    {217, {69, 64}, "700m/oct"}, // GlideTimeRate
    {217, {100, 0}, "5.5s"}, // GlideTimeRate
    {217, {127, 127}, "23.5s/oct"}, // GlideTimeRate
    {0, {0, 0, 1}, "0"}, // TripleDefault
    {0, {127, 0, 1}, "127"}, // TripleDefault
    {0, {0, 3, 1}, "0"}, // TripleDefault
    {0, {127, 3, 1}, "127"}, // TripleDefault
    {60, {0, 0, 0}, "-64  -50"}, // OscFreqDep
    {60, {69, 0, 0}, " +5  -50"}, // OscFreqDep
    {60, {127, 0, 0}, "+63  -50"}, // OscFreqDep
    {60, {0, 64, 0}, "-64   +0"}, // OscFreqDep
    {60, {69, 64, 0}, " +5   +0"}, // OscFreqDep
    {60, {127, 64, 0}, "+63   +0"}, // OscFreqDep
    {60, {0, 0, 1}, "7.9431Hz"}, // OscFreqDep
    {60, {69, 0, 1}, "427.47Hz"}, // OscFreqDep
    {60, {127, 0, 1}, "12.19kHz"}, // OscFreqDep
    {60, {0, 64, 1}, "8.1758Hz"}, // OscFreqDep
    {60, {69, 64, 1}, "440.00Hz"}, // OscFreqDep
    {60, {127, 64, 1}, "12.55kHz"}, // OscFreqDep
    {60, {0, 0, 2}, "x0.0241"}, // OscFreqDep
    {60, {69, 0, 2}, "x1.2968"}, // OscFreqDep
    {60, {127, 0, 2}, "x36.971"}, // OscFreqDep
    {60, {0, 64, 2}, "x0.0248"}, // OscFreqDep
    {60, {69, 64, 2}, "x1.3348"}, // OscFreqDep
    {60, {127, 64, 2}, "x38.055"}, // OscFreqDep
    {60, {0, 0, 3}, "0 Hz"}, // OscFreqDep
    {60, {69, 0, 3}, "6:1  -50"}, // OscFreqDep
    {60, {127, 0, 3}, "64:1  -50"}, // OscFreqDep
    {60, {0, 64, 3}, "0 Hz"}, // OscFreqDep
    {60, {69, 64, 3}, "6:1   +0"}, // OscFreqDep
    {60, {127, 64, 3}, "64:1   +0"}, // OscFreqDep
    {60, {0, 0, 4}, "0 Hz"}, // OscFreqDep
    {60, {69, 0, 4}, "0.2087Hz"}, // OscFreqDep
    {60, {127, 0, 4}, "5.9506Hz"}, // OscFreqDep
    {60, {0, 64, 4}, "0 Hz"}, // OscFreqDep
    {60, {69, 64, 4}, "0.2148Hz"}, // OscFreqDep
    {60, {127, 64, 4}, "6.1249Hz"}, // OscFreqDep
    {110, {0, 0, 0}, "--"}, // ClkGenTempo
    {110, {69, 0, 0}, "--"}, // ClkGenTempo
    {110, {127, 0, 0}, "--"}, // ClkGenTempo
    {110, {0, 1, 0}, "24BPM"}, // ClkGenTempo
    {110, {69, 1, 0}, "125BPM"}, // ClkGenTempo
    {110, {127, 1, 0}, "214BPM"}, // ClkGenTempo
    {110, {0, 0, 1}, "--"}, // ClkGenTempo
    {110, {69, 0, 1}, "--"}, // ClkGenTempo
    {110, {127, 0, 1}, "--"}, // ClkGenTempo
    {110, {0, 1, 1}, "Master"}, // ClkGenTempo
    {110, {69, 1, 1}, "Master"}, // ClkGenTempo
    {110, {127, 1, 1}, "Master"}, // ClkGenTempo
    {110, {0, 0, 2}, "--"}, // ClkGenTempo
    {110, {69, 0, 2}, "--"}, // ClkGenTempo
    {110, {127, 0, 2}, "--"}, // ClkGenTempo
    {110, {0, 1, 2}, "Master"}, // ClkGenTempo
    {110, {69, 1, 2}, "Master"}, // ClkGenTempo
    {110, {127, 1, 2}, "Master"}, // ClkGenTempo
    {140, {0, 0, 0}, "0.01m"}, // DelayTimeTap
    {140, {69, 0, 0}, "2.89m"}, // DelayTimeTap
    {140, {127, 0, 0}, "5.30m"}, // DelayTimeTap
    {140, {0, 1, 0}, "1/64T"}, // DelayTimeTap
    {140, {69, 1, 0}, "1/8D"}, // DelayTimeTap
    {140, {127, 1, 0}, "2/1"}, // DelayTimeTap
    {140, {0, 0, 1}, "0.01m"}, // DelayTimeTap
    {140, {69, 0, 1}, "13.7m"}, // DelayTimeTap
    {140, {127, 0, 1}, "25.1m"}, // DelayTimeTap
    {140, {0, 1, 1}, "1/64T"}, // DelayTimeTap
    {140, {69, 1, 1}, "1/8D"}, // DelayTimeTap
    {140, {127, 1, 1}, "2/1"}, // DelayTimeTap
    {140, {0, 0, 2}, "0.01m"}, // DelayTimeTap
    {140, {69, 0, 2}, "54.6m"}, // DelayTimeTap
    {140, {127, 0, 2}, "101m"}, // DelayTimeTap
    {140, {0, 1, 2}, "1/64T"}, // DelayTimeTap
    {140, {69, 1, 2}, "1/8D"}, // DelayTimeTap
    {140, {127, 1, 2}, "2/1"}, // DelayTimeTap
    {140, {0, 0, 3}, "0.01m"}, // DelayTimeTap
    {140, {69, 0, 3}, "272m"}, // DelayTimeTap
    {140, {127, 0, 3}, "500m"}, // DelayTimeTap
    {140, {0, 1, 3}, "1/64T"}, // DelayTimeTap
    {140, {69, 1, 3}, "1/8D"}, // DelayTimeTap
    {140, {127, 1, 3}, "2/1"}, // DelayTimeTap
    {140, {0, 0, 4}, "0.01m"}, // DelayTimeTap
    {140, {69, 0, 4}, "543m"}, // DelayTimeTap
    {140, {127, 0, 4}, "1.000s"}, // DelayTimeTap
    {140, {0, 1, 4}, "1/64T"}, // DelayTimeTap
    {140, {69, 1, 4}, "1/8D"}, // DelayTimeTap
    {140, {127, 1, 4}, "2/1"}, // DelayTimeTap
    {143, {0, 0, 0}, "0.01m"}, // DelayTimeFx
    {143, {69, 0, 0}, "272m"}, // DelayTimeFx
    {143, {127, 0, 0}, "500m"}, // DelayTimeFx
    {143, {0, 1, 0}, "1/64T"}, // DelayTimeFx
    {143, {69, 1, 0}, "1/8D"}, // DelayTimeFx
    {143, {127, 1, 0}, "2/1"}, // DelayTimeFx
    {143, {0, 0, 1}, "0.01m"}, // DelayTimeFx
    {143, {69, 0, 1}, "543m"}, // DelayTimeFx
    {143, {127, 0, 1}, "1.000s"}, // DelayTimeFx
    {143, {0, 1, 1}, "1/64T"}, // DelayTimeFx
    {143, {69, 1, 1}, "1/8D"}, // DelayTimeFx
    {143, {127, 1, 1}, "2/1"}, // DelayTimeFx
    {143, {0, 0, 2}, "0.01m"}, // DelayTimeFx
    {143, {69, 0, 2}, "1.087s"}, // DelayTimeFx
    {143, {127, 0, 2}, "2.000s"}, // DelayTimeFx
    {143, {0, 1, 2}, "1/64T"}, // DelayTimeFx
    {143, {69, 1, 2}, "1/8D"}, // DelayTimeFx
    {143, {127, 1, 2}, "2/1"}, // DelayTimeFx
    {143, {0, 0, 3}, "0.01m"}, // DelayTimeFx
    {143, {69, 0, 3}, "1.467s"}, // DelayTimeFx
    {143, {127, 0, 3}, "2.700s"}, // DelayTimeFx
    {143, {0, 1, 3}, "1/64T"}, // DelayTimeFx
    {143, {69, 1, 3}, "1/8D"}, // DelayTimeFx
    {143, {127, 1, 3}, "2/1"}, // DelayTimeFx
    {146, {0, 0, 0}, "0.01m"}, // DelayTimeStereo
    {146, {69, 0, 0}, "272m"}, // DelayTimeStereo
    {146, {127, 0, 0}, "500m"}, // DelayTimeStereo
    {146, {0, 1, 0}, "1/64T"}, // DelayTimeStereo
    {146, {69, 1, 0}, "1/8D"}, // DelayTimeStereo
    {146, {127, 1, 0}, "2/1"}, // DelayTimeStereo
    {146, {0, 0, 1}, "0.01m"}, // DelayTimeStereo
    {146, {69, 0, 1}, "543m"}, // DelayTimeStereo
    {146, {127, 0, 1}, "1.000s"}, // DelayTimeStereo
    {146, {0, 1, 1}, "1/64T"}, // DelayTimeStereo
    {146, {69, 1, 1}, "1/8D"}, // DelayTimeStereo
    {146, {127, 1, 1}, "2/1"}, // DelayTimeStereo
    {146, {0, 0, 2}, "0.01m"}, // DelayTimeStereo
    {146, {69, 0, 2}, "734m"}, // DelayTimeStereo
    {146, {127, 0, 2}, "1.351s"}, // DelayTimeStereo
    {146, {0, 1, 2}, "1/64T"}, // DelayTimeStereo
    {146, {69, 1, 2}, "1/8D"}, // DelayTimeStereo
    {146, {127, 1, 2}, "2/1"}, // DelayTimeStereo
    {198, {0, 0, 0}, "x0.50"}, // DXOscFreq
    {198, {69, 0, 0}, "x69.00"}, // DXOscFreq
    {198, {127, 0, 0}, "x127.00"}, // DXOscFreq
    {198, {0, 64, 0}, "x0.82"}, // DXOscFreq
    {198, {69, 64, 0}, "x113.16"}, // DXOscFreq
    {198, {127, 64, 0}, "x208.28"}, // DXOscFreq
    {198, {0, 0, 1}, "1.000Hz"}, // DXOscFreq
    {198, {69, 0, 1}, "10.00Hz"}, // DXOscFreq
    {198, {127, 0, 1}, "1000 Hz"}, // DXOscFreq
    {198, {0, 64, 1}, "4.365Hz"}, // DXOscFreq
    {198, {69, 64, 1}, "43.65Hz"}, // DXOscFreq
    {198, {127, 64, 1}, "4365 Hz"}, // DXOscFreq
};

} // namespace

TEST_CASE("param text: matches the emulated original editor")
{
    for (const Ref& r : kReference) {
        std::vector<std::uint8_t> v(r.values.begin(), r.values.end());
        INFO(pt::functionName(r.id, int(v.size())) << " id " << r.id);
        CHECK(pt::format(r.id, v) == r.text);
    }
}
