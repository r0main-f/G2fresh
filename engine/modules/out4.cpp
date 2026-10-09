// 4-Out (type 3): four inputs to Out 1..4, FX 1..4 or Bus 1..4.
//
// Spec: module database (Dest Out/FX/Bus, On/Off, Pad 0 dB / +6 dB from the
// editor's OutPad display); checked on the emulated G2 as a black box.
#include "common.hpp"

namespace g2::engine {
namespace {

class Out4 final : public Processor {
public:
    void update(const Module& m, std::uint8_t var) override
    {
        dest_ = param(m, var, 0);
        gain_ = param(m, var, 1) ? (param(m, var, 2) ? 2.0f : 1.0f) : 0.0f;
    }
    bool audioRate(const Module&) const override { return true; }
    void process(const float* in, float*, Io& io) override
    {
        auto& bank = dest_ == 0 ? io.out : dest_ == 1 ? io.fx : io.bus;
        for (std::size_t i = 0; i < 4; ++i)
            bank[i] += in[i] * gain_;
    }

private:
    int dest_ = 0;
    float gain_ = 1.0f;
};

} // namespace

void registerOut4() { registerProcessor(3, [] { return std::make_unique<Out4>(); }); }

} // namespace g2::engine
