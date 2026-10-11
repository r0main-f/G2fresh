// g2benchpack: writes the hardware bench pack's patches (bench/hardware-pack/patches) with the core's edit API.
//
//   g2benchpack PACK_DIR         write the patches and the B08 performance into PACK_DIR/patches, and PACK_DIR/manifest.json
//   g2benchpack --check PACK_DIR exit 1 unless PACK_DIR holds exactly that (the committed pack is current)
//   g2benchpack --texts TYPE     print a module type's parameter texts (for choosing values)
//
// The patches are G2fresh's own: built here module by module, nothing taken from Clavia's or anyone's patch files.
// Each one plays by itself as soon as it is loaded (no key, no MIDI), holds nothing random (except the noise of
// B05, which is the test) and uses a small part of one DSP, so that it loads on any G2, G2X or G2 Engine; B11-B13 are
// the exception: heavy on purpose, to read the voice count a machine gives them.
//
// A patch has no name of its own: a G2 shows the name of the file it was loaded from, so the file names are the
// names (at most 16 characters). What each patch is for, and what we expect from it: bench/hardware-pack/NOTES.md.
#include "g2/edit.hpp"
#include "g2/patch.hpp"
#include "g2/patch_load.hpp"
#include "g2/uprate.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace g2;

namespace {

const db::ModuleDef& defByName(std::string_view name)
{
    for (const auto& m : db::modules())
        if (m.kind == db::ModuleKind::Module && name == m.shortName)
            return m;
    throw std::invalid_argument("unknown module type " + std::string(name));
}

template <class Span>
u8 indexByName(const Span& items, std::string_view name, std::string_view what)
{
    for (std::size_t i = 0; i < items.size(); ++i)
        if (name == items[i].name)
            return u8(i);
    throw std::invalid_argument("unknown " + std::string(what) + " " + std::string(name));
}

std::string trimmed(std::string s)
{
    while (!s.empty() && s.front() == ' ')
        s.erase(s.begin());
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

// A patch under construction. Every value is written to all nine variations, so that the variation selected on the
// synth makes no difference.
class Build {
public:
    Build()
    {
        edit::setVoices(patch, edit::VoiceMode::Mono);
        for (u8 v = 0; v < kFileVariations; ++v) {
            edit::setSetting(patch, edit::Setting::Gain, 0, v, 127); // patch level: full
            edit::setSetting(patch, edit::Setting::Bend, 0, v, 0);   // no pitch bend
        }
    }

    u8 add(std::string_view type, u8 col, std::string_view name = {})
    {
        const u8 m = edit::addModule(patch, area, defByName(type).typeId, col, edit::freeRow(patch, area, col));
        if (!name.empty())
            edit::renameModule(patch, area, m, std::string(name));
        return m;
    }
    void set(u8 m, std::string_view param, int value)
    {
        const u8 p = indexByName(def(m).params, param, "parameter");
        if (value < def(m).params[p].min || value > def(m).params[p].max)
            throw std::invalid_argument("value out of range for " + std::string(param));
        for (u8 v = 0; v < kFileVariations; ++v)
            edit::setParam(patch, area, m, p, v, u8(value));
    }
    // The value the editor shows as `text` (spaces around it ignored).
    void setText(u8 m, std::string_view param, std::string_view text)
    {
        const u8 p = indexByName(def(m).params, param, "parameter");
        for (int value = def(m).params[p].min; value <= def(m).params[p].max; ++value) {
            set(m, param, value);
            if (trimmed(edit::paramText(patch, area, m, p, 0)) == text)
                return;
        }
        throw std::invalid_argument("no value of " + std::string(param) + " reads " + std::string(text));
    }
    void mode(u8 m, std::string_view name, int value) { edit::setMode(patch, area, m, indexByName(def(m).modes, name, "mode"), u8(value)); }
    std::string text(u8 m, std::string_view param) const
    {
        return trimmed(edit::paramText(patch, area, m, indexByName(def(m).params, param, "parameter"), 0));
    }
    void wire(u8 from, std::string_view out, u8 to, std::string_view in)
    {
        edit::connect(patch, area, {from, indexByName(def(from).outputs, out, "output"), true},
                      {to, indexByName(def(to).inputs, in, "input"), false});
    }
    // A sequencer's steps: "Step 1".."Step 16" (`row` = "Step") or its event row ("Evnt").
    void steps(u8 m, std::string_view row, const std::vector<int>& values)
    {
        for (std::size_t i = 0; i < 16; ++i)
            set(m, std::string(row) + " " + std::to_string(i + 1), i < values.size() ? values[i] : 0);
    }
    Patch finish(std::string_view notes)
    {
        uprate::update(patch, Location::Va);
        uprate::update(patch, Location::Fx);
        patch.notes = std::string(notes);
        return patch;
    }

    Patch patch = Patch::makeDefault();
    Location area = Location::Va;

private:
    const db::ModuleDef& def(u8 m) const { return *patch.area(area).find(m)->def(); }
};

// ---- building blocks ---------------------------------------------------------------------------------------

// A sine that ignores the keyboard: MIDI note `note`, Fine at `fine` (64: in tune, then 100/128 cent per step).
u8 sine(Build& b, u8 col, int note, int fine, std::string_view name)
{
    const u8 osc = b.add("OscA", col, name);
    b.setText(osc, "Wave", "Sine");
    b.set(osc, "KBT", 0);
    b.set(osc, "Coarse", note);
    b.set(osc, "Fine", fine);
    return osc;
}

// The steps' clock: ClkGen on its own tempo (never the master clock: the performance on the friend's G2 decides
// that one), its 1/16 output divided. At 120 BPM the 1/16 output ticks 8 times a second: `divide` 8 is one step a
// second. A second divider resets the sequencers once per loop of `loopSteps` steps, so that two sequencers in a
// patch stay on the same step whatever happened before.
struct Clock {
    u8 gen = 0, div = 0, loop = 0;
};
Clock clock(Build& b, u8 col, int divide, int loopSteps)
{
    Clock c;
    c.gen = b.add("ClkGen", col, "Clock 120");
    b.setText(c.gen, "Tempo", "120BPM");
    b.setText(c.gen, "Source", "Intern");
    b.set(c.gen, "Swing", 0);
    c.div = b.add("ClkDiv", col, "Step clock");
    b.set(c.div, "Divider", divide - 1);
    b.wire(c.gen, "1/16", c.div, "Clk");
    if (loopSteps > 0) {
        c.loop = b.add("ClkDiv", col, "Loop reset");
        b.set(c.loop, "Divider", loopSteps - 1);
        b.wire(c.div, "Out", c.loop, "Clk");
    }
    return c;
}

u8 sequencer(Build& b, u8 col, std::string_view type, const Clock& c, int length, std::string_view name)
{
    const u8 seq = b.add(type, col, name);
    b.set(seq, "Length", length - 1);
    b.setText(seq, "Cycles", "Loop");
    b.wire(c.div, "Out", seq, "Clk");
    if (c.loop)
        b.wire(c.loop, "Out", seq, "Rst");
    return seq;
}

u8 output(Build& b, u8 col, bool pad6, std::string_view name = "Out 1/2")
{
    const u8 out = b.add("2-Out", col, name);
    b.setText(out, "Dest", "Out 1/2");
    b.set(out, "Pad", pad6 ? 1 : 0);
    return out;
}

// ---- the patches -------------------------------------------------------------------------------------------

struct Item {
    std::string id, name, measure; // what benchfacts.py measures in it: tone, level_steps, tone_map, pad_steps, noise, ...
    bool performance = false;
    Patch patch;
    double recordSeconds = 0, loopSeconds = 0;
    std::string json; // design facts for the manifest (members of the item's object)
};

constexpr int kSineNote = 83, kSineFine = 91; // B5 +21.1 cents: 1 kHz (999.9 Hz)

Item levelRef()
{
    Build b;
    const u8 osc = sine(b, 0, kSineNote, kSineFine, "Sine 1 kHz");
    const u8 out = output(b, 1, false);
    b.wire(osc, "Out", out, "InL");
    b.wire(osc, "Out", out, "InR");
    return {"B01", "B01 Level Ref", "tone", false,
            b.finish("G2fresh bench pack B01: a 1 kHz sine at full level (64 units) on Out 1 and 2, Pad 0 dB."), 30, 0,
            R"("outputs": [1, 2], "tone_hz": 1000)"};
}

// Level steps in dB relative to a full-level signal (64 units): the sequencer's value scales the sine, two
// amplifiers (x4, x2) lift it 18 dB. Out 1 has Pad 0 dB, Out 2 Pad +6 dB.
const std::vector<std::pair<int, int>> kLevelSteps = { // {sequencer value, nominal dB}
    {1, -24}, {2, -18}, {4, -12}, {8, -6}, {16, 0}, {23, 3}, {32, 6}, {45, 9}, {64, 12}, {90, 15}, {127, 18}};

Item levelSteps()
{
    Build b;
    const u8 osc = sine(b, 0, kSineNote, kSineFine, "Sine 1 kHz");
    const int length = int(kLevelSteps.size()) + 1;
    const Clock c = clock(b, 1, 8, 0);
    const u8 seq = sequencer(b, 2, "SeqLev", c, length, "Levels");
    b.setText(seq, "Pol", "Unipol");
    std::vector<int> values{0};
    for (const auto& s : kLevelSteps)
        values.push_back(s.first);
    b.steps(seq, "Step", values);
    const u8 vca = b.add("LevMult", 3, "Level");
    const u8 amp1 = b.add("LevAmp", 3, "x4");
    const u8 amp2 = b.add("LevAmp", 3, "x2");
    b.setText(amp1, "Type", "Lin");
    b.setText(amp1, "Gain", "x4.00");
    b.setText(amp2, "Type", "Lin");
    b.setText(amp2, "Gain", "x2.00");
    b.wire(osc, "Out", vca, "In");
    b.wire(seq, "Val", vca, "Mod");
    b.wire(vca, "Out", amp1, "In");
    b.wire(amp1, "Out", amp2, "In");
    const u8 out0 = output(b, 4, false, "Out 1 pad 0dB");
    const u8 out6 = output(b, 4, true, "Out 2 pad +6dB");
    b.wire(amp2, "Out", out0, "InL");
    b.wire(amp2, "Out", out6, "InR");
    std::ostringstream j;
    j << R"("outputs": [1, 2], "tone_hz": 1000, "step_seconds": 1, "steps": [{"label": "silence", "nominal_db": null})";
    for (const auto& s : kLevelSteps)
        j << ", {\"label\": \"" << (s.second > 0 ? "+" : "") << s.second << " dB\", \"nominal_db\": " << s.second << ", \"seq_value\": " << s.first
          << "}";
    j << R"(], "pad_db": {"1": 0, "2": 6})";
    return {"B02", "B02 Level Steps", "level_steps", false,
            b.finish("G2fresh bench pack B02: a 1 kHz sine, one second of silence then 11 one-second steps from -24 dB to +18 dB "
                     "relative to full level. Out 1: Pad 0 dB. Out 2: Pad +6 dB."),
            40, double(length), j.str()};
}

Item outputMap()
{
    Build b;
    const int notes[4] = {69, 74, 79, 84}; // A4, D5, G5, C6
    const u8 out = b.add("4-Out", 1, "Out 1-4");
    b.setText(out, "Dest", "Out");
    b.set(out, "Pad", 0);
    const char* ins[4] = {"In1", "In2", "In3", "In4"};
    const char* names[4] = {"Out 1: A4", "Out 2: D5", "Out 3: G5", "Out 4: C6"};
    for (int i = 0; i < 4; ++i)
        b.wire(sine(b, 0, notes[i], 64, names[i]), "Out", out, ins[i]);
    return {"B03", "B03 Output Map", "tone_map", false,
            b.finish("G2fresh bench pack B03: four sines at full level, one per output. Out 1: A4 440 Hz, Out 2: D5 587 Hz, "
                     "Out 3: G5 784 Hz, Out 4: C6 1047 Hz."),
            20, 0, R"("outputs": [1, 2, 3, 4], "tones_hz": {"1": 440.0, "2": 587.33, "3": 783.99, "4": 1046.5})"};
}

Item pad()
{
    Build b;
    const u8 osc = sine(b, 0, kSineNote, kSineFine, "Sine 1 kHz");
    const Clock c = clock(b, 1, 8, 8);
    const u8 seq0 = sequencer(b, 2, "SeqLev", c, 8, "Pad 0dB steps");
    const u8 seq6 = sequencer(b, 2, "SeqLev", c, 8, "Pad +6dB steps");
    b.setText(seq0, "Pol", "Unipol");
    b.setText(seq6, "Pol", "Unipol");
    b.steps(seq0, "Step", {0, 127, 127, 127, 0, 0, 0, 0});
    b.steps(seq6, "Step", {0, 0, 0, 0, 127, 127, 127, 0});
    const u8 vca0 = b.add("LevMult", 3, "Gate 0dB");
    const u8 vca6 = b.add("LevMult", 3, "Gate +6dB");
    const u8 out0 = output(b, 4, false, "Out pad 0dB");
    const u8 out6 = output(b, 4, true, "Out pad +6dB");
    b.wire(osc, "Out", vca0, "In");
    b.wire(osc, "Out", vca6, "In");
    b.wire(seq0, "Val", vca0, "Mod");
    b.wire(seq6, "Val", vca6, "Mod");
    b.wire(vca0, "Out", out0, "InL");
    b.wire(vca0, "Out", out0, "InR");
    b.wire(vca6, "Out", out6, "InL");
    b.wire(vca6, "Out", out6, "InR");
    return {"B04", "B04 Pad", "pad_steps", false,
            b.finish("G2fresh bench pack B04: a 1 kHz sine at full level on Out 1 and 2: one second of silence, three seconds through "
                     "an output module with Pad 0 dB, three seconds through one with Pad +6 dB, one second of silence."),
            30, 8,
            R"("outputs": [1, 2], "tone_hz": 1000, "step_seconds": 1, "steps": [{"label": "silence"}, {"label": "pad 0 dB"}, )"
            R"({"label": "pad 0 dB"}, {"label": "pad 0 dB"}, {"label": "pad +6 dB"}, {"label": "pad +6 dB"}, {"label": "pad +6 dB"}, )"
            R"({"label": "silence"}])"};
}

Item whiteNoise()
{
    Build b;
    const u8 noise = b.add("Noise", 0, "White noise");
    b.set(noise, "Color", 0);
    const u8 out = output(b, 1, false);
    b.wire(noise, "Out", out, "InL");
    b.wire(noise, "Out", out, "InR");
    return {"B05", "B05 White Noise", "noise", false,
            b.finish("G2fresh bench pack B05: the Noise module, white (Color 0), on Out 1 and 2, Pad 0 dB."), 30, 0,
            R"("outputs": [1, 2])"};
}

// C5 C6 C7 G7 C8 G8 C9 E9. Not the sequencer's last notes: at 127 (G9) the oscillator plays 13.28 kHz, not G9's 12.54.
const std::vector<int> kSawNotes = {72, 84, 96, 103, 108, 115, 120, 124};

Item brightSaw()
{
    Build b;
    const int length = int(kSawNotes.size()) + 1;
    const Clock c = clock(b, 0, 8, length);
    const u8 notes = sequencer(b, 1, "SeqNote", c, length, "Notes");
    const u8 gate = sequencer(b, 2, "SeqLev", c, length, "Gate");
    b.setText(gate, "Pol", "Unipol");
    std::vector<int> n{64}, g{0};
    for (const int note : kSawNotes) {
        n.push_back(note);
        g.push_back(127);
    }
    b.steps(notes, "Step", n);
    b.steps(gate, "Step", g);
    const u8 osc = b.add("OscA", 3, "Saw");
    b.setText(osc, "Wave", "Saw");
    b.set(osc, "KBT", 0);
    b.set(osc, "Coarse", 64);
    b.set(osc, "Fine", 64);
    const u8 vca = b.add("LevMult", 3, "Gate");
    const u8 out = output(b, 4, false);
    b.wire(notes, "Note", osc, "Pitch");
    b.wire(osc, "Out", vca, "In");
    b.wire(gate, "Val", vca, "Mod");
    b.wire(vca, "Out", out, "InL");
    b.wire(vca, "Out", out, "InR");
    std::ostringstream j;
    j << R"("outputs": [1, 2], "step_seconds": 1, "steps": [{"label": "silence"})";
    for (std::size_t i = 0; i < kSawNotes.size(); ++i)
        j << ", {\"label\": \"" << b.text(notes, "Step " + std::to_string(i + 2)) << "\", \"midi_note\": " << kSawNotes[i] << "}";
    j << "]";
    return {"B06", "B06 Bright Saw", "saw_steps", false,
            b.finish("G2fresh bench pack B06: an unfiltered sawtooth, one second of silence then one second each of C5, C6, C7, G7, "
                     "C8, G8, C9 and E9, on Out 1 and 2."),
            30, double(length), j.str()};
}

Item silence()
{
    Build b;
    output(b, 0, false);
    return {"B07", "B07 Silence", "silence", false,
            b.finish("G2fresh bench pack B07: an output module with nothing connected. Record the silence."), 15, 0, R"("outputs": [1, 2])"};
}

// One click per beat: the clock's 1/16 output divided by 4 fires a 5 ms decay envelope, sent out as it is (a
// positive pulse: it also shows which way round the outputs are).
u8 clicker(Build& b, u8 col, bool master, std::string_view name)
{
    const u8 gen = b.add("ClkGen", col, name);
    b.setText(gen, "Tempo", "120BPM");
    b.setText(gen, "Source", master ? "Master" : "Intern");
    b.set(gen, "Swing", 0);
    const u8 div = b.add("ClkDiv", col, "Beats");
    b.set(div, "Divider", 3);
    const u8 env = b.add("EnvD", col, "Click");
    b.setText(env, "Decay", "5.5m");
    b.setText(env, "OutType", "Pos");
    b.wire(gen, "1/16", div, "Clk");
    b.wire(div, "Out", env, "Trig");
    return env;
}

Item clock120()
{
    Build b;
    const u8 master = clicker(b, 0, true, "Master clock");
    const u8 intern = clicker(b, 1, false, "Own 120 BPM");
    const u8 out = output(b, 2, false);
    b.wire(master, "Env", out, "InL");
    b.wire(intern, "Env", out, "InR");
    return {"B08", "B08 Clock 120", "clicks", true,
            b.finish("G2fresh bench pack B08 (a performance: master clock 120 BPM, running). Out 1: a click on every beat of the "
                     "master clock. Out 2: a click on every beat of the patch's own 120 BPM clock."),
            60, 0, R"j("outputs": [1, 2], "nominal_bpm": 120, "clicks": {"1": "master clock", "2": "the patch's own clock (ClkGen, Intern)"})j"};
}

// The phrases run at four steps a second (the 1/16 clock divided by 2), 16 steps: 4 seconds, the last second silent.
Item sawFilter()
{
    Build b;
    const Clock c = clock(b, 0, 2, 0);
    const u8 seq = sequencer(b, 1, "SeqNote", c, 16, "Phrase");
    b.steps(seq, "Step", {48, 48, 60, 48, 55, 58, 48, 51, 53, 60, 63, 48, 64, 64, 64, 64}); // C3 ... from step 13: rest
    b.steps(seq, "Evnt", {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0});
    b.setText(seq, "Pulse", "Trig");
    const u8 osc = b.add("OscB", 2, "Saw"); // its Sync input restarts the wave on every note: each loop is the same waveform
    b.setText(osc, "Wave", "Saw");
    b.set(osc, "KBT", 0);
    b.set(osc, "Coarse", 64);
    b.set(osc, "Fine", 64);
    const u8 flt = b.add("FltClassic", 2, "Low-pass 24 dB");
    b.setText(flt, "Freq", "261.6Hz");
    b.setText(flt, "Freq M", "100%");
    b.setText(flt, "KBT", "KBT Off");
    b.set(flt, "Res", 64);
    b.setText(flt, "dB/Oct", "24dB");
    const u8 env = b.add("EnvD", 3, "Sweep and level");
    b.setText(env, "Decay", "186m");
    b.setText(env, "OutType", "Pos");
    const u8 out = output(b, 4, false);
    b.wire(seq, "Note", osc, "Pitch");
    b.wire(seq, "Trig", env, "Trig");
    b.wire(seq, "Trig", osc, "Sync");
    b.wire(osc, "Out", flt, "In");
    b.wire(env, "Env", flt, "PitchVar");
    b.wire(flt, "Out", env, "In");
    b.wire(env, "Out", out, "InL");
    b.wire(env, "Out", out, "InR");
    return {"B09a", "B09a Saw Filter", "phrase", false,
            b.finish("G2fresh bench pack B09a: a 12-note sawtooth phrase through FltClassic (24 dB, resonance 50), swept by a decay "
                     "envelope that also shapes the level. Four seconds, the last one silent."),
            20, 4, R"("outputs": [1, 2])"};
}

Item fmPhrase()
{
    Build b;
    const Clock c = clock(b, 0, 2, 0);
    const u8 seq = sequencer(b, 1, "SeqNote", c, 16, "Phrase");
    b.steps(seq, "Step", {48, 55, 60, 63, 55, 60, 67, 72, 70, 67, 63, 60, 64, 64, 64, 64});
    b.steps(seq, "Evnt", {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0});
    b.setText(seq, "Pulse", "Trig"); // a gate would stay up from one step to the next: no new attack
    auto op = [&](u8 col, std::string_view name, int coarse, int level, int release) {
        const u8 o = b.add("Operator", col, name);
        b.set(o, "KBT", 0);
        b.set(o, "KBG", 0);
        b.set(o, "Sync", 1); // the wave restarts with every note
        b.setText(o, "Tune Md", "Ratio");
        b.set(o, "Coarse", coarse);
        b.set(o, "R1", 99);
        b.set(o, "L1", 99);
        b.set(o, "R2", 99); // full level while the trigger lasts, then the release makes the note
        b.set(o, "L2", 99);
        b.set(o, "R3", 99);
        b.set(o, "L3", 99);
        b.set(o, "R4", release);
        b.set(o, "L4", 0);
        b.set(o, "Level", level);
        b.wire(seq, "Note", o, "Pitch");
        b.wire(seq, "Trig", o, "Gate");
        return o;
    };
    const u8 mod = op(2, "Modulator x2", 2, 82, 60);
    const u8 car = op(3, "Carrier x1", 1, 99, 55);
    const u8 out = output(b, 4, false);
    b.wire(mod, "Out", car, "FM");
    b.wire(car, "Out", out, "InL");
    b.wire(car, "Out", out, "InR");
    return {"B09b", "B09b FM Phrase", "phrase", false,
            b.finish("G2fresh bench pack B09b: a 12-note phrase on two Operator modules, a modulator at twice the pitch into the "
                     "carrier's FM input. Four seconds, the last one silent."),
            20, 4, R"("outputs": [1, 2])"};
}

Item drum()
{
    Build b;
    const Clock c = clock(b, 0, 2, 0);
    const u8 seq = sequencer(b, 1, "SeqEvent", c, 16, "Hits");
    b.steps(seq, "Step", {1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0});
    b.setText(seq, "PulseUp", "Trig");
    const u8 drm = b.add("DrumSynth", 2, "Drum");
    b.set(drm, "Noise", 0); // its noise is random: off
    const u8 out = output(b, 3, false);
    b.wire(seq, "Trig1", drm, "Trig");
    b.wire(drm, "Out", out, "InL");
    b.wire(drm, "Out", out, "InR");
    return {"B09c", "B09c Drum", "phrase", false,
            b.finish("G2fresh bench pack B09c: five hits of the DrumSynth module (its noise part off) in four seconds, the last "
                     "second silent."),
            20, 4, R"("outputs": [1, 2])"};
}

Item reverbDelay()
{
    Build b;
    const Clock c = clock(b, 0, 4, 0); // two steps a second: 8 seconds
    const u8 seq = sequencer(b, 1, "SeqEvent", c, 16, "Clicks");
    b.steps(seq, "Step", {1});                               // step 1: the reverb's click
    b.steps(seq, "Evnt", {0, 0, 0, 0, 0, 0, 0, 0, 1});       // step 9: the delay's click
    b.setText(seq, "PulseUp", "Trig");
    b.setText(seq, "PulseLo", "Trig");
    auto click = [&](std::string_view name, std::string_view trig) {
        const u8 env = b.add("EnvD", 2, name);
        b.setText(env, "Decay", "1.1m");
        b.setText(env, "OutType", "Pos");
        b.wire(seq, trig, env, "Trig");
        return env;
    };
    const u8 click1 = click("Click 1", "Trig1");
    const u8 click2 = click("Click 2", "Trig2");
    const u8 rev = b.add("Reverb", 3, "Hall");
    b.mode(rev, "RoomType", 3);
    b.set(rev, "Time", 16); // 1.5 s
    b.set(rev, "Bright", 64);
    b.set(rev, "Mix", 64);
    const u8 dly = b.add("DlyStereo", 3, "Delay");
    b.setText(dly, "Time Md", "Time");
    b.setText(dly, "Time L", "252m");
    b.setText(dly, "Time R", "378m");
    b.set(dly, "FB L", 64);
    b.set(dly, "FB R", 64);
    b.set(dly, "X-FB L", 0);
    b.set(dly, "X-FB R", 0);
    b.set(dly, "Dry/Wet", 64);
    const u8 out1 = output(b, 4, false, "Reverb out");
    const u8 out2 = output(b, 4, false, "Delay out");
    b.wire(click1, "Env", rev, "InL");
    b.wire(click1, "Env", rev, "InR");
    b.wire(rev, "OutL", out1, "InL");
    b.wire(rev, "OutR", out1, "InR");
    b.wire(click2, "Env", dly, "In");
    b.wire(dly, "OutL", out2, "InL");
    b.wire(dly, "OutR", out2, "InR");
    return {"B09d", "B09d Rev Delay", "phrase", false,
            b.finish("G2fresh bench pack B09d: every eight seconds, a click into the Reverb (Hall, 1.5 s), then four seconds later a "
                     "click into a stereo delay (252 ms left, 378 ms right, feedback 50)."),
            30, 8, R"("outputs": [1, 2])"};
}

Item audioIn()
{
    Build b;
    const u8 in = b.add("2-In", 0, "In 1/2");
    b.setText(in, "Source", "In 1/2");
    b.setText(in, "Pad", "0 dB");
    const u8 out = output(b, 1, false);
    b.wire(in, "OutL", out, "InL");
    b.wire(in, "OutR", out, "InR");
    return {"B10", "B10 Audio In", "input", false,
            b.finish("G2fresh bench pack B10: audio inputs 1 and 2 (Pad 0 dB) straight to Out 1 and 2 (Pad 0 dB)."), 20, 0,
            R"("outputs": [1, 2], "stimulus": "B10 Stimulus.wav")"};
}

// B11-B13: heavy on purpose, 32 voices asked. `stacks` oscillator -> filter -> envelope chains per voice, silent until a
// key is played. With `fx`, the voices go through a Reverb in the FX area (voices plus effects).
Item voices(std::string_view id, std::string_view name, int stacks, bool fx)
{
    Build b;
    edit::setVoices(b.patch, edit::VoiceMode::Poly, 32);
    const u8 kbd = b.add("Keyboard", 0, "Keyboard");
    const u8 out = output(b, 0, false, fx ? "To FX" : "Out 1/2");
    if (fx) {
        b.setText(out, "Dest", "FX 1/2");
        b.area = Location::Fx;
        const u8 in = b.add("Fx-In", 0, "From voices");
        const u8 rev = b.add("Reverb", 1, "Hall");
        b.mode(rev, "RoomType", 3);
        const u8 fxOut = output(b, 2, false);
        b.wire(in, "OutL", rev, "InL");
        b.wire(in, "OutR", rev, "InR");
        b.wire(rev, "OutL", fxOut, "InL");
        b.wire(rev, "OutR", fxOut, "InR");
        b.area = Location::Va;
    }
    u8 chain = 0;
    for (int i = 0; i < stacks; ++i) {
        const u8 col = u8(1 + i / 3);
        const u8 osc = b.add("OscB", col);
        b.set(osc, "Fine", 64 + (i % 5) * 2);
        const u8 flt = b.add("FltNord", col);
        const u8 env = b.add("EnvADSR", col);
        b.set(env, "KBG", 0);
        const u8 mix = b.add("Mix1-1A", col);
        b.set(mix, "Level", 60);
        b.wire(osc, "Out", flt, "In");
        b.wire(flt, "Out", env, "In");
        b.wire(kbd, "Gate", env, "Gate");
        b.wire(env, "Out", mix, "In");
        if (chain)
            b.wire(chain, "Out", mix, "Chain");
        chain = mix;
    }
    b.wire(chain, "Out", out, "InL");
    b.wire(chain, "Out", out, "InR");
    return {std::string(id), std::string(name), "voices", false,
            b.finish("G2fresh bench pack " + std::string(id) + ": " + std::to_string(stacks)
                     + " oscillator, filter and envelope chains per voice" + (fx ? ", then a Hall reverb in the FX area" : "")
                     + ", 32 voices asked. Silent until a key is played: read the voice count on the display."),
            0, 0,
            "\"requested_voices\": 32, \"stacks\": " + std::to_string(stacks) + ", \"fx_reverb\": " + (fx ? "true" : "false")};
}

// B14, for the keyboard models: which way the pitch stick and the G2X's two global wheels count. A steady A4; the
// stick's morph group raises it a fourth, G.Wheel 1's an octave, and G.Wheel 2's brings in a second, higher note.
Item wheels()
{
    Build b;
    const u8 low = sine(b, 0, 69, 64, "A4");
    const u8 high = sine(b, 0, 88, 64, "E6");
    const u8 mix = b.add("Mix2-1A", 1, "Mix");
    b.setText(mix, "Lin/Exp", "Lin");
    b.set(mix, "Level1", 64);
    b.set(mix, "Level2", 0);
    const u8 out = output(b, 2, false);
    b.wire(low, "Out", mix, "In1");
    b.wire(high, "Out", mix, "In2");
    b.wire(mix, "Out", out, "InL");
    b.wire(mix, "Out", out, "InR");
    Patch patch = b.finish("G2fresh bench pack B14 (keyboard models): a steady A4. Pitch stick: up a fourth. G.Wheel 1: up an octave. "
                           "G.Wheel 2: a second, higher note (E6) comes in.");
    const int kStick = 6, kWheel1 = 4, kWheel2 = 7; // morph groups 7 "P.Stick", 5 (source "G.Wh 1") and 8 "G.Wh 2"
    const auto& mixDef = *patch.va.find(mix)->def();
    for (u8 v = 0; v < kFileVariations; ++v) {
        edit::setSetting(patch, edit::Setting::Morph, u8(8 + kWheel1), v, 2); // group 5's source: G.Wh 1 (not the sustain pedal)
        edit::setMorph(patch, v, Location::Va, low, 0, u8(kStick), 5);
        edit::setMorph(patch, v, Location::Va, low, 0, u8(kWheel1), 12);
        edit::setMorph(patch, v, Location::Va, mix, indexByName(mixDef.params, "Level2", "parameter"), u8(kWheel2), 64);
    }
    if (edit::settingText(patch, edit::Setting::Morph, u8(8 + kWheel1), 0) != "G.Wh 1")
        throw std::runtime_error("morph group 5's source is not G.Wh 1");
    return {"B14", "B14 Wheels", "controls", false, patch, 0, 0,
            R"("tone_hz": 440.0, "pitch_stick_semitones": 5, "g_wheel_1_semitones": 12, "g_wheel_2_adds_hz": 1318.51)"};
}

std::vector<Item> items()
{
    return {levelRef(), levelSteps(), outputMap(), pad(), whiteNoise(), brightSaw(), silence(), clock120(), sawFilter(), fmPhrase(),
            drum(), reverbDelay(), audioIn(), voices("B11", "B11 Voices Light", 2, false), voices("B12", "B12 Voices Mid", 3, false),
            voices("B13", "B13 Voices Heavy", 8, true), wheels()};
}

// ---- files -------------------------------------------------------------------------------------------------

std::vector<u8> bytesOf(const Item& item)
{
    if (!item.performance)
        return savePatch(item.patch);
    // The master clock's tempo and whether it runs belong to the performance, not to a patch.
    Performance perf = Performance::playing(item.patch, item.name);
    for (std::size_t i = 1; i < perf.header.slots.size(); ++i)
        perf.header.slots[i].patchName = "No name";
    perf.header.masterClockBpm = 120;
    perf.header.masterClockRun = 1;
    return savePerformance(perf);
}

std::string fileName(const Item& item)
{
    return item.name + (item.performance ? ".prf2" : ".pch2");
}

std::string manifest(const std::vector<Item>& all)
{
    std::ostringstream j;
    j << "{\n \"comment\": \"Written by tools/benchpack/g2benchpack: what each bench patch is designed to do. "
         "What the emulated G2 makes of it is in expected/.\",\n \"patches\": [\n";
    for (std::size_t i = 0; i < all.size(); ++i) {
        const auto& it = all[i];
        const auto load = patchload::compute(it.patch);
        char est[64];
        std::snprintf(est, sizeof est, "%.1f", double(load.vaCycles) * 100.0);
        j << "  {\"id\": \"" << it.id << "\", \"name\": \"" << it.name << "\", \"file\": \"" << fileName(it) << "\", \"measure\": \"" << it.measure
          << "\", \"record_seconds\": " << it.recordSeconds << ", \"loop_seconds\": " << it.loopSeconds << ", \"modules\": "
          << it.patch.va.modules.size() + it.patch.fx.modules.size() << ", \"editor_load_estimate_percent\": " << est << ",\n   "
          << it.json << "}" << (i + 1 < all.size() ? "," : "") << "\n";
    }
    j << " ]\n}\n";
    return j.str();
}

std::vector<u8> readBytes(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

int write(const std::filesystem::path& dir, bool check)
{
    const auto all = items();
    std::map<std::string, std::vector<u8>> files;
    for (const auto& it : all) {
        if (it.name.size() > edit::kNameLength || edit::modularName(it.name) != it.name || edit::fileNameForPatch(it.name) != it.name)
            throw std::runtime_error("not a G2 patch name: " + it.name);
        auto bytes = bytesOf(it);
        // what we wrote loads again, as version 23, and saves to the same bytes
        const auto back = g2::load(bytes);
        if (back.convertedFrom != 0 || back.isPerformance() != it.performance)
            throw std::runtime_error("does not load back: " + it.name);
        const auto again = it.performance ? savePerformance(std::get<Performance>(back.content)) : savePatch(std::get<Patch>(back.content));
        if (again != bytes)
            throw std::runtime_error("does not round-trip: " + it.name);
        files["patches/" + fileName(it)] = std::move(bytes);
    }
    const auto m = manifest(all);
    files["manifest.json"] = std::vector<u8>(m.begin(), m.end());

    if (check) {
        int bad = 0;
        for (const auto& [name, bytes] : files)
            if (readBytes(dir / name) != bytes) {
                std::cerr << "differs or missing: " << (dir / name).string() << "\n";
                ++bad;
            }
        for (const auto& e : std::filesystem::directory_iterator(dir / "patches")) {
            const auto ext = e.path().extension().string();
            if ((ext == ".pch2" || ext == ".prf2") && !files.count("patches/" + e.path().filename().string())) {
                std::cerr << "not part of the pack: " << e.path().string() << "\n";
                ++bad;
            }
        }
        if (bad)
            std::cerr << "run: g2benchpack " << dir.string() << "\n";
        return bad ? 1 : 0;
    }
    std::filesystem::create_directories(dir / "patches");
    for (const auto& [name, bytes] : files) {
        std::ofstream f(dir / name, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f)
            throw std::runtime_error("cannot write " + (dir / name).string());
    }
    std::cout << "wrote " << files.size() << " files to " << dir.string() << "\n";
    return 0;
}

int texts(std::string_view type)
{
    Patch patch = Patch::makeDefault();
    const auto& def = defByName(type);
    const u8 m = edit::addModule(patch, Location::Va, def.typeId, 0, 0);
    for (std::size_t p = 0; p < def.params.size(); ++p) {
        std::printf("%s (%d..%d, default %d):", def.params[p].name, def.params[p].min, def.params[p].max, def.params[p].defaultValue);
        for (int v = def.params[p].min; v <= def.params[p].max; ++v) {
            edit::setParam(patch, Location::Va, m, u8(p), 0, u8(v));
            std::printf(" %d=%s", v, edit::paramText(patch, Location::Va, m, u8(p), 0).c_str());
        }
        edit::setParam(patch, Location::Va, m, u8(p), 0, def.params[p].defaultValue);
        std::printf("\n");
    }
    for (std::size_t p = 0; p < def.modes.size(); ++p) {
        std::printf("mode %s (%d..%d, default %d):", def.modes[p].name, def.modes[p].min, def.modes[p].max, def.modes[p].defaultValue);
        for (int v = def.modes[p].min; v <= def.modes[p].max; ++v)
            std::printf(" %d=%s", v, db::formatMode(def, p, u8(v)).c_str());
        std::printf("\n");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--texts")
            return texts(argv[2]);
        if (argc == 3 && std::string_view(argv[1]) == "--check")
            return write(argv[2], true);
        if (argc == 2 && argv[1][0] != '-')
            return write(argv[1], false);
        std::cerr << "usage: g2benchpack PACK_DIR | --check PACK_DIR | --texts MODULE_TYPE\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
