// Mix2-1A (type 194): two inputs with level knobs and on/off buttons, plus a
// chain input.
//
// Spec: module database (Level1/2, On1/2, Lin/Exp 0 Exp / 1 Lin / 2 dB);
// catalog: levels are a table (about (v/127)^2.9, slewed by the OS), response
// -6.18 dB at the default level 100. The curves are in common.hpp (mixLevel),
// checked on the emulated G2 as a black box.
#include "common.hpp"

namespace g2::engine {
namespace {

class Mix2_1A final : public Processor {
public:
    void update(const Module& m, std::uint8_t var) override
    {
        const int curve = param(m, var, 4);
        l1_ = param(m, var, 1) ? mixLevel(param(m, var, 0), curve) : 0.0f;
        l2_ = param(m, var, 3) ? mixLevel(param(m, var, 2), curve) : 0.0f;
    }
    void process(const float* in, float* out, Io&) override { out[0] = in[2] + l1_ * in[0] + l2_ * in[1]; }

private:
    float l1_ = 0.0f, l2_ = 0.0f;
};

} // namespace

void registerMix2_1A() { registerProcessor(194, [] { return std::make_unique<Mix2_1A>(); }); }

} // namespace g2::engine
