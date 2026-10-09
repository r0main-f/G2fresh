// Mix4-1A (type 193): the sum of four inputs, no controls.
//
// Spec: module database (four dynamic inputs, no parameters); catalog response
// 0.0 dB at 100 Hz..10 kHz, DC 0.25 -> 0.25. Runs at audio rate when uprated
// (fed by an audio signal), at control rate otherwise.
#include "common.hpp"

namespace g2::engine {
namespace {

class Mix4_1A final : public Processor {
public:
    void update(const Module&, std::uint8_t) override {}
    void process(const float* in, float* out, Io&) override { out[0] = in[0] + in[1] + in[2] + in[3]; }
};

} // namespace

void registerMix4_1A() { registerProcessor(193, [] { return std::make_unique<Mix4_1A>(); }); }

} // namespace g2::engine
