// 2-Out (type 4): two inputs to Out 1/2, 3/4, FX 1/2, 3/4 or Bus 1/2, 3/4.
//
// Spec: module database (Dest 0..5, On/Off, Pad 0 dB / +6 dB from the editor's
// OutPad display); the catalog shows Dest and On/Off as code/selector words,
// Pad changes no DSP word (inferred: applied after the voice). Levels checked
// on the emulated G2 as a black box (re/notes/native-engine.md).
#include "common.hpp"

namespace g2::engine {
namespace {

class Out2 final : public Processor {
public:
    void update(const Module& m, std::uint8_t var) override
    {
        dest_ = param(m, var, 0);
        gain_ = param(m, var, 1) ? (param(m, var, 2) ? 2.0f : 1.0f) : 0.0f;
    }
    bool audioRate(const Module&) const override { return true; }
    void process(const float* in, float*, Io& io) override
    {
        const float l = in[0] * gain_, r = in[1] * gain_;
        auto& bank = dest_ < 2 ? io.out : dest_ < 4 ? io.fx : io.bus;
        const std::size_t base = (dest_ & 1) ? 2 : 0;
        bank[base] += l;
        bank[base + 1] += r;
    }

private:
    int dest_ = 0;
    float gain_ = 1.0f;
};

} // namespace

void registerOut2() { registerProcessor(4, [] { return std::make_unique<Out2>(); }); }

} // namespace g2::engine
