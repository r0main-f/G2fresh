// FltLP (type 87): non-resonant lowpass, 6 to 36 dB/oct.
//
// Spec, clean-room (re/notes/native-engine.md):
// * Cutoff: fc = 440 * 2^((Freq - 60)/12) Hz (the editor's display), the
//   catalog's word 0.00045 * 2^(v/12) = pi*fc/fs.
// * Measured as a black box (the patch's saw before and after the filter,
//   magnitude and phase at 400 harmonics): SlopeMode s is a cascade of s+1
//   identical one-poles y += k (x - y), k = 2 sin(pi*fc/fs) (about twice the
//   catalog word; the sine shows near the top, 0.965 * 2*pi*fc/fs at Freq
//   120), clamped at 1 (Freq 127 passes the input unchanged); no extra
//   delay. Matches within 0.03 dB / 0.15 degree where measurable.
// * KBT (Off/25/50/75/100 %) adds that share of the keyboard's note signal;
//   the Pitch input adds 64 semitones per unit times Freq M (0..1).
// * On/Off: off passes the input (inferred).
#include "common.hpp"

#include <array>

namespace g2::engine {
namespace {

class FltLP final : public Processor {
public:
    void connected(const std::vector<bool>& inputs) override { pitchConnected_ = inputs.size() > 1 && inputs[1]; }
    void update(const Module& m, std::uint8_t var) override
    {
        freq_ = param(m, var, 0);
        mod_ = param(m, var, 1) / 127.0f;
        kbt_ = param(m, var, 2) * 0.25f;
        on_ = param(m, var, 3) != 0;
        stages_ = std::clamp(mode(m, 0), 0, 5) + 1;
        dirty_ = true;
    }
    bool audioRate(const Module&) const override { return true; }

    void process(const float* in, float* out, Io& io) override
    {
        if (io.controlTick) {
            const float key = kbt_ * io.pitch;
            const float pm = pitchConnected_ ? mod_ * in[1] : 0.0f;
            if (dirty_ || key != lastKey_ || pm != lastMod_) {
                lastKey_ = key;
                lastMod_ = pm;
                dirty_ = false;
                const double note = freq_ + kUnitsPerSignal * (key + pm);
                k_ = static_cast<float>(std::min(1.0, 2.0 * std::sin(kPi * noteHz(note + 9.0) / kSampleRate)));
            }
        }
        float x = in[0];
        if (!on_) {
            out[0] = x;
            return;
        }
        for (int i = 0; i < stages_; ++i) {
            y_[static_cast<std::size_t>(i)] += k_ * (x - y_[static_cast<std::size_t>(i)]);
            x = y_[static_cast<std::size_t>(i)];
        }
        out[0] = x;
    }

private:
    int freq_ = 75, stages_ = 1;
    float mod_ = 0.0f, kbt_ = 0.0f;
    bool on_ = true, pitchConnected_ = false, dirty_ = true;
    float k_ = 0.0f, lastKey_ = 0.0f, lastMod_ = 0.0f;
    std::array<float, 6> y_{};
};

} // namespace

void registerFltLP() { registerProcessor(87, [] { return std::make_unique<FltLP>(); }); }

} // namespace g2::engine
