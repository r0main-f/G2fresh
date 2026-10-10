// A native audio engine for G2 patches: each module type has a C++
// processor, and the engine runs a patch's modules in cable order at the G2's
// rates, so a patch can sound without the synth.
//
// Clean-room rule: processors are written from the module specification in
// re/notes/dsp-module-catalog.md (behaviour, structure, formulas, measured
// responses) and checked against the emulated G2 as a black box, never from
// Clavia's DSP code. This file holds only the generic machinery.
//
// Rates: the G2 runs at 96 kHz; control-rate ("blue") code runs every 4th
// sample (24 kHz) unless the module is uprated. A processor says which rate
// it needs; the engine holds a control-rate processor's outputs in between.
#pragma once

#include "g2/patch.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace g2::engine {

inline constexpr double kSampleRate = 96000.0;
inline constexpr int kControlDivider = 4;

// Signals are floats in the G2's units: 1.0 is a full-scale signal (64
// "units" on the G2's displays), so the note signal moves 1/64 per semitone.
inline constexpr float kUnitsPerSignal = 64.0f;

// What modules exchange with the outside of the patch, per sample.
struct Io {
    std::array<float, 4> out{}; // Out 1..4 (to the DACs)
    std::array<float, 4> fx{};  // FX 1..4 (VA -> FX area)
    std::array<float, 4> bus{}; // Bus 1..4
    std::array<float, 4> in{};  // audio inputs 1..4 (from the ADCs)
    // Keyboard of the voice: the note signal, 0 at note 64 (E4) and 1/64 per
    // semitone; the gate (0 or 1); velocities 0..1.
    float pitch = 0.0f;
    float gate = 0.0f;
    float velocity = 0.0f;
    float releaseVelocity = 0.0f;
    // True on the samples where control-rate code runs (every
    // kControlDivider-th sample): an audio-rate processor with a control-rate
    // part (an envelope driving a VCA) updates that part on these samples.
    bool controlTick = true;
};

class Processor {
public:
    virtual ~Processor() = default;
    // Parameters or modes changed (a knob, a variation switch). Called once
    // before the first sample too.
    virtual void update(const Module& module, std::uint8_t variation) = 0;
    // One step: `in` has one value per input connector of the module (0 when
    // unconnected), `out` one per output connector.
    virtual void process(const float* in, float* out, Io& io) = 0;
    // Audio-rate processors run every sample; the others every
    // kControlDivider samples. Asked after update().
    virtual bool audioRate(const Module& module) const { return module.uprate; }
    // Which inputs have a cable, once after the patch is built (before
    // update()). An unconnected input reads 0; a module whose unconnected
    // input means something else (an AM input at full level) asks here.
    virtual void connected(const std::vector<bool>& inputs) { (void)inputs; }
};

using Factory = std::function<std::unique_ptr<Processor>()>;
// Registers the processor of a module type. The module files each define a
// register function, called from registerBuiltinProcessors() in
// engine/modules/all.cpp (explicit, so a static library keeps them all).
void registerProcessor(std::uint8_t type, Factory factory);
void registerBuiltinProcessors();
bool hasProcessor(std::uint8_t type);
std::vector<std::uint8_t> supportedTypes();

// A patch: its VA area once per voice, its FX area once (fed by the sum of
// the voices' FX sends).
class PatchEngine {
public:
    // `maxVoices` caps the patch's voice count (Mono and Legato play one).
    explicit PatchEngine(const Patch& patch, int maxVoices = 16);
    ~PatchEngine();

    int voices() const { return static_cast<int>(voices_.size()); }
    // Polyphonic keyboard: a note starts the free voice released longest
    // ago, or steals the voice started longest ago. (G2 note numbers, 60 = C4.)
    void noteOn(int note, int velocity);
    void noteOff(int note, int velocity = 64);
    void allNotesOff();

    // Modules without a processor: they output silence.
    const std::vector<std::string>& unsupported() const { return unsupported_; }

    void setVariation(std::uint8_t variation);
    // Applies a parameter change of the current variation (like a knob).
    void setParam(Location loc, std::uint8_t module, std::uint8_t param, std::uint8_t value);
    // Monophonic keyboard on the first voice (tools and tests): G2 note
    // number (60 = C4), gate on/off, velocity 0..127. The pitch keeps the last
    // note after the gate closes, as on the synth.
    void setKey(int note, bool gate, int velocity = 100);

    // Renders `frames` samples of Out 1..4 into `outs` (each `frames` long;
    // null pointers are skipped). The outputs are the 2-Out/4-Out signals
    // times the patch's Gain setting, in signal units (1.0 = full scale).
    void render(std::array<float*, 4> outs, int frames);

private:
    struct Node;
    struct Voice {
        int note = 64;
        bool gate = false;
        float velocity = 0.0f, releaseVelocity = 0.0f;
        std::uint64_t since = 0; // sample count of the last note on or off
    };
    void build(Location loc, int voice);
    void step();
    void keyIo(const Voice& v);

    Patch patch_;
    std::uint8_t variation_ = 0;
    std::vector<Node> nodes_; // each voice's VA in cable order, then FX in cable order
    std::vector<Voice> voices_;
    int lastVoice_ = 0; // the FX area hears the voice played last
    std::vector<std::string> unsupported_;
    void updateGain();

    Io io_;
    std::uint64_t sampleCount_ = 0;
    float gain_ = 1.0f; // patch Gain setting
};

} // namespace g2::engine
