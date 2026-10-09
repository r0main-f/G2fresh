// Parameter value -> display text, ported from the original Clavia G2 editor
// (Mac v1.62, namespace ParamText). See re/notes/param-display.md.
//
// The editor keeps three tables of text functions, indexed by the same 8-bit
// "text function id" (panel `InfoFunc`, TextField `Text Func`, or the id stored
// in the module definition). Which table is used depends on how many values
// the display depends on:
//   1 value  -> single table, f(v)
//   2 values -> dual table,   f(v, dep1)
//   3 values -> triple table, f(v, dep1, dep2)
// An id that is not in the chosen table falls back to Default (single:
// 0..100 scale) or DualDefault/TripleDefault (first value as an integer).
//
// Function names below are the original ParamText:: names. Output is UTF-8;
// the original wrote Mac Roman, which only matters for "±" (0xB1).
// Values outside the documented range of a table-driven function give "".
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace g2::paramtext {

using u8 = std::uint8_t;

// Formats with the table chosen by values.size(): 1 single, 2 dual, 3 triple
// (extra values are ignored, an empty span gives "").
std::string format(int infoFunc, std::span<const u8> values);
std::string formatSingle(int id, u8 v);
std::string formatDual(int id, u8 v, u8 dep1);
std::string formatTriple(int id, u8 v, u8 dep1, u8 dep2);

// Original function name registered under `id` for `arity` (1..3), e.g.
// functionName(21, 1) == "FilterCutOff". Unknown ids give the default name.
const char* functionName(int id, int arity);

// --- single-value functions -------------------------------------------------
std::string From0to100(u8 v);
std::string Default(u8 v);
std::string From0to200Percent(u8 v);
std::string OnOff(u8 v);
std::string InactiveActive(u8 v);
std::string Mono(u8 v);
std::string Flip(u8 v);
std::string GainCompensation(u8 v);
std::string GCOnOff(u8 v);
std::string Mute(u8 v);
std::string Bypass(u8 v);
std::string EnvReset(u8 v);
std::string EnvADRSust(u8 v);
std::string Loop(u8 v);
std::string Rec(u8 v);
std::string Phase(u8 v);
std::string OffNum(u8 v);
std::string Semitone(u8 v);
std::string SemitoneKey(u8 v);
std::string SemitoneFilter(u8 v);
std::string Enum(u8 v);
std::string Negative(u8 v);
std::string UniPol(u8 v);
std::string UniPolCompact(u8 v);
std::string BiPol(u8 v);
std::string UniPolShort(u8 v);
std::string Sym(u8 v);
std::string DelayTime(u8 v);
std::string FltVariant(u8 v);
std::string BPM(u8 v);
std::string ClkGenSource(u8 v);
std::string ClkGenSync(u8 v);
std::string OscCoarse(u8 v);
std::string KBTOnOff(u8 v);
std::string PulseWidth(u8 v);
std::string PulseWidthHalf(u8 v);
std::string EnvelopeTime(u8 v);
std::string EnvAttackType(u8 v);
std::string EnvAttackDecayShape(u8 v);
std::string LFOWaveFull(u8 v);
std::string Smooth(u8 v);
std::string EqFreqHi(u8 v);
std::string EqFreqLo(u8 v);
std::string SampleRate(u8 v);
std::string OscWaveFull(u8 v);
std::string OscWaveSine(u8 v);
std::string Polarity(u8 v);
std::string TrigGate(u8 v);
std::string GoStop(u8 v);
std::string LevelShift(u8 v);
std::string RndLevelShift(u8 v);
std::string SignalType(u8 v);
std::string Fade2to1(u8 v);
std::string Fade1to2(u8 v);
std::string XFade(u8 v);
std::string OscAWave(u8 v);
std::string OscBWave(u8 v);
std::string OscSinShpWave(u8 v);
std::string OscFine(u8 v);
std::string FmType(u8 v);
std::string OscTuneMode(u8 v);
std::string DelayTimeMode(u8 v);
std::string FilterCdB(u8 v);
std::string EnvMultiType(u8 v);
std::string AmRm(u8 v);
std::string dB_0_6_12(u8 v);
std::string OutBusBDest(u8 v);
std::string Out2Dest(u8 v);
std::string Out4Dest(u8 v);
std::string In2Src(u8 v);
std::string In4Src(u8 v);
std::string InCVA(u8 v);
std::string InputPad(u8 v);
std::string OutPad(u8 v);
std::string PartialGen(u8 v);
std::string FltKBT(u8 v);
std::string FmKBT(u8 v);
std::string dB_6_12(u8 v);
std::string dB_12_24(u8 v);
std::string FilterType(u8 v);
std::string HiLo(u8 v);
std::string Vowels(u8 v);
std::string Emphasis(u8 v);
std::string ClipSym(u8 v);
std::string VocoderMon(u8 v);
std::string DiodeModes(u8 v);
std::string ShaperModes(u8 v);
std::string ShapeExpCurve(u8 v);
std::string ReverbType(u8 v);
std::string SwitchCtrl(u8 v);
std::string EnvMultiSusPlace(u8 v);
std::string EnvADDSRSusPlace(u8 v);
std::string EnvFollowerAttack(u8 v);
std::string EnvFollowerRelease(u8 v);
std::string LFORange(u8 v);
std::string LFOPhase(u8 v);
std::string LfoAWave(u8 v);
std::string LfoBWave(u8 v);
std::string LfoCWave(u8 v);
std::string RndSmooth(u8 v);
std::string RndStep(u8 v);
std::string RndStepSwitch(u8 v);
std::string RndLogic(u8 v);
std::string RndDistribution(u8 v);
std::string RndDensity(u8 v);
std::string PatchMIDIOutChannel(u8 v);
std::string PatchMIDIInChannel(u8 v);
std::string PosNeg(u8 v);
std::string AmpType(u8 v);
std::string ExpLin(u8 v);
std::string LogLin(u8 v);
std::string PortamentoType(u8 v);
std::string NoiseGateAtkTime(u8 v);
std::string NoiseGateRelTime(u8 v);
std::string OverDriveType(u8 v);
std::string OverDriveSym(u8 v);
std::string PhaserType(u8 v);
std::string CompressorAttack(u8 v);
std::string CompressorRelease(u8 v);
std::string CompressorThreshold(u8 v);
std::string CompressorRatio(u8 v);
std::string CompressorLevel(u8 v);
std::string MIDIValue(u8 v);
std::string PTrackAlgorithm(u8 v);
std::string PitchShiftDelay(u8 v);
std::string MIDIFilter(u8 v);
std::string MIDIEcho(u8 v);
std::string DXDetune(u8 v);
std::string DXBreakPoint(u8 v);
std::string DXCurve(u8 v);
std::string DXTuneMode(u8 v);
std::string FlangerRate(u8 v);
std::string PhaserRate(u8 v);
std::string DrumFilterType(u8 v);
std::string OffOnOn(u8 v);
std::string MorphGroupSource1(u8 v);
std::string MorphGroupSource2(u8 v);
std::string MorphGroupSource3(u8 v);
std::string MorphGroupSource4(u8 v);
std::string MorphGroupSource5(u8 v);
std::string MorphGroupSource6(u8 v);
std::string MorphGroupSource7(u8 v);
std::string MorphGroupSource8(u8 v);
std::string MIDIChannel(u8 v);
std::string LfoKBT(u8 v);
std::string NotePlusMinus(u8 v);
std::string Pad(u8 v);
std::string VibratoAmount(u8 v);
std::string VibratoSource(u8 v);
std::string VibratoRate(u8 v);
std::string PortamentoTime(u8 v);
std::string PortamentoMode(u8 v);
std::string ArpeggiatorRange(u8 v);
std::string ArpeggiatorRate(u8 v);
std::string ArpeggiatorMode(u8 v);
std::string OscKBT(u8 v);
std::string OscPhase(u8 v);
std::string LfoShape(u8 v);
std::string VolumeDb(u8 v);
std::string FilterCType(u8 v);
std::string FilterCSlope(u8 v);
std::string BendOnOff(u8 v);
std::string BendRange(u8 v);
std::string OctShift(u8 v);
std::string ControlPedalGain(u8 v);
std::string MonoKeybPrio(u8 v);
std::string ModAmountMode(u8 v);
std::string ClkGenSwing(u8 v);
std::string KeyQuantCaptureRange(u8 v);
std::string FilterResonance(u8 v);
std::string KBT(u8 v);
std::string Slide(u8 v);
std::string PlusMinusHalfSteps(u8 v);
std::string NoteScaler(u8 v);
std::string EqGain(u8 v);
std::string AmpGain(u8 v);
std::string NoteVelScale(u8 v);
std::string EQParBW(u8 v);
std::string dB(u8 v);
std::string NoiseGateLevel(u8 v);
std::string PitchShuttle(u8 v);
std::string FilterCutOff(u8 v);
std::string FilterCutOff2(u8 v);
std::string DrumMstFreq(u8 v);
std::string DrumSlvMult(u8 v);
std::string OnePoleFreq(u8 v);
std::string LfoRateMult(u8 v);
std::string LFOFreq1(u8 v);
std::string PhaserFreq(u8 v);
std::string OscFreqSingle(u8 v);
std::string EqFreq(u8 v);
std::string EqFreq100To8k(u8 v);
std::string VocoderLevel(u8 v);
std::string DigitizerBits(u8 v);
std::string MIDIClockKeyboardTrig(u8 v);

// --- two-value functions: f(value, dependency) ------------------------------
std::string DualDefault(u8 a, u8 b);
std::string ReverbTime(u8 time, u8 roomType);
std::string OscSemi2(u8 coarse, u8 fine);
std::string PitchShift(u8 semi, u8 fine);
std::string BiUniPol(u8 v, u8 polarity);
std::string BiUniPolCompact(u8 v, u8 polarity);
std::string MultiEnvBiUni(u8 v, u8 outType);
std::string DelayTimeTap8(u8 time, u8 range);
std::string DelayTime2(u8 time, u8 range);
std::string LogicTime(u8 time, u8 range);
std::string GlideTimeRate(u8 v, u8 shape);
std::string AmpGainNew(u8 v, u8 type);
std::string LFOFreq(u8 rate, u8 range);
std::string BodeFreq(u8 v, u8 range);
std::string OscFactor(u8 coarse, u8 fine);
std::string OscSubFreq(u8 coarse, u8 fine);
std::string OscFreq(u8 coarse, u8 fine);
std::string OscSyncTimbre(u8 a, u8 b);
std::string OscPulseTimbre(u8 a, u8 b);
std::string OscPartials(u8 coarse, u8 fine);
std::string LevMult(u8 mode, u8 v);
std::string dBLin(u8 v, u8 mode);

// --- three-value functions: f(value, dep1, dep2) ----------------------------
std::string TripleDefault(u8 a, u8 b, u8 c);
std::string ClkGenTempo(u8 rate, u8 active, u8 source);
std::string DelayTimeStereo(u8 time, u8 sync, u8 range);
std::string DelayTimeFx(u8 time, u8 sync, u8 range);
std::string DelayTimeTap(u8 time, u8 sync, u8 range);
std::string DXOscFreq(u8 coarse, u8 fine, u8 fixedMode);
std::string OscFreqDep(u8 coarse, u8 fine, u8 tuneMode);

} // namespace g2::paramtext
