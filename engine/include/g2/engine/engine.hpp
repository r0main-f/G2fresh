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

// What modules exchange with the outside of the patch, per sample.
struct Io {
    std::array<float, 4> out{}; // Out 1..4 (to the DACs)
    std::array<float, 2> fx{};  // FX send (VA -> FX area)
    std::array<float, 2> bus{}; // Bus 1/2
    std::array<float, 4> in{};  // audio inputs 1..4 (from the ADCs)
    float pitch = 0.0f;         // keyboard pitch (the G2's note signal), for keyboard-driven modules
    float gate = 0.0f;          // keyboard gate
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
};

using Factory = std::function<std::unique_ptr<Processor>()>;
// Registers the processor of a module type. The module files each define a
// register function, called from registerBuiltinProcessors() in
// engine/modules/all.cpp (explicit, so a static library keeps them all).
void registerProcessor(std::uint8_t type, Factory factory);
void registerBuiltinProcessors();
bool hasProcessor(std::uint8_t type);
std::vector<std::uint8_t> supportedTypes();

// One voice of a patch (VA area) plus its FX area.
class PatchEngine {
public:
    explicit PatchEngine(const Patch& patch);
    ~PatchEngine();

    // Modules without a processor: they output silence.
    const std::vector<std::string>& unsupported() const { return unsupported_; }

    void setVariation(std::uint8_t variation);
    // Applies a parameter change of the current variation (like a knob).
    void setParam(Location loc, std::uint8_t module, std::uint8_t param, std::uint8_t value);
    // Keyboard (for keyboard-driven modules): G2 note number, gate on/off.
    void setKey(int note, bool gate);

    // Renders `frames` samples of Out 1..4 into `outs` (each `frames` long;
    // null pointers are skipped).
    void render(std::array<float*, 4> outs, int frames);

private:
    struct Node;
    void build(Location loc);
    void step();

    Patch patch_;
    std::uint8_t variation_ = 0;
    std::vector<Node> nodes_; // VA in cable order, then FX in cable order
    std::vector<std::string> unsupported_;
    Io io_;
    int sampleCount_ = 0;
};

} // namespace g2::engine
