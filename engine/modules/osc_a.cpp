// OscA (type 97): Sine, Triangle, Saw, Sqr50, Sqr25, Sqr10 with keyboard
// tracking and two pitch inputs.
//
// Spec, clean-room (re/notes/native-engine.md):
// * Coarse is a note number: f = 440 * 2^((Coarse + (Fine-64)/128 - 69)/12)
//   [catalog §3; measured on the emulated G2: 440.003 Hz at Coarse 69].
// * KBT On adds the keyboard's note signal (64 * pitch semitones); the Pitch
//   input adds 64 semitones per unit, PitchVar the same scaled by Pitch M.
// * Amplitude 1.0 for every waveform, measured as a black box. The saw
//   rises; the pulse waves are DC-free: Sqr25 is +1.5 / -0.5, "Sqr10" is a
//   1/16 pulse (+1.875 / -0.125), measured. (Polarities as inside the patch:
//   the emulated machine's output inverts, see the Env output measurement.)
// * Anti-aliasing, measured as a black box (residual vs the naive wave):
//   the saw and pulse edges are the ideal step convolved with a triangle
//   kernel of +-2 samples (a 4-sample band-limited step, polyBLEP form); the
//   triangle's corners are raised by one phase increment on the samples
//   within one sample of the corner. The sine is computed directly (flat to
//   12.5 kHz).
// The oscillator output is delayed by two samples so that the band-limited
// step can be spread over the samples on both sides of a discontinuity.
#include "common.hpp"

namespace g2::engine {
namespace {

// Residual of a unit step at t = 0 seen at sample time t (|t| < 2): the
// triangle-kernel band-limited step minus the ideal step.
inline float blep(double t)
{
    if (t <= -2.0 || t >= 2.0)
        return 0.0f;
    if (t <= 0.0)
        return static_cast<float>((t + 2.0) * (t + 2.0) * 0.125);
    return static_cast<float>(-(2.0 - t) * (2.0 - t) * 0.125);
}

class OscA final : public Processor {
public:
    void connected(const std::vector<bool>& inputs) override
    {
        pitchConnected_ = inputs.size() > 0 && inputs[0];
        varConnected_ = inputs.size() > 1 && inputs[1];
    }
    void update(const Module& m, std::uint8_t var) override
    {
        baseNote_ = param(m, var, 0) + (param(m, var, 1) - 64) / 128.0;
        kbt_ = param(m, var, 2) != 0;
        pitchMod_ = param(m, var, 3) / 127.0f;
        wave_ = param(m, var, 4);
        on_ = param(m, var, 5) != 0;
        duty_ = wave_ == 4 ? 0.25 : wave_ == 5 ? 1.0 / 16.0 : 0.5;
        lastKey_ = -1e9f; // recompute the increment
    }
    bool audioRate(const Module&) const override { return true; }

    void process(const float* in, float* out, Io& io) override
    {
        // Pitch: recomputed only when an input it depends on changes.
        const float key = kbt_ ? io.pitch : 0.0f;
        const float pin = pitchConnected_ ? in[0] : 0.0f;
        const float pvar = varConnected_ ? in[1] : 0.0f;
        if (key != lastKey_ || pin != lastPin_ || pvar != lastVar_) {
            lastKey_ = key;
            lastPin_ = pin;
            lastVar_ = pvar;
            const double note = baseNote_ + kUnitsPerSignal * (key + pin + pitchMod_ * pvar);
            inc_ = std::min(noteHz(note) / kSampleRate, 0.5);
        }
        const double prev = phase_;
        phase_ += inc_;
        const bool wrapped = phase_ >= 1.0;
        if (wrapped)
            phase_ -= 1.0;
        const double ph = phase_;

        // The slots hold samples n-2, n-1, n (current) and n+1.
        float naive = 0.0f;
        switch (wave_) {
        case 0:
            naive = static_cast<float>(std::sin(2.0 * kPi * ph));
            break;
        case 1: { // triangle: +1 at phase 0, -1 at phase 1/2
            naive = static_cast<float>(ph < 0.5 ? 1.0 - 4.0 * ph : 4.0 * ph - 3.0);
            // A corner in (n-1, n]: the samples less than one sample away
            // from it (n-1 unless the corner falls on n, and n) move toward
            // the inside by one increment.
            const float c = static_cast<float>(inc_);
            if (wrapped) {
                if (ph > 0.0)
                    slot_[1] -= c;
                naive -= c;
            } else if (prev < 0.5 && ph >= 0.5) {
                if (ph > 0.5)
                    slot_[1] += c;
                naive += c;
            }
            break;
        }
        case 2: // rising saw, -2 step at the wrap
            naive = static_cast<float>(2.0 * ph - 1.0);
            if (wrapped)
                step(ph / inc_, -2.0f);
            break;
        default: { // pulse: high 2(1-d) for phase < d, low -2d after
            const double d = duty_;
            naive = static_cast<float>(ph < d ? 2.0 * (1.0 - d) : -2.0 * d);
            if (wrapped)
                step(ph / inc_, 2.0f);
            const bool crossed = wrapped ? ph >= d : (prev < d && ph >= d);
            if (crossed)
                step((ph - d) / inc_, -2.0f);
            break;
        }
        }
        slot_[2] += naive;
        out[0] = on_ ? slot_[0] : 0.0f;
        slot_[0] = slot_[1];
        slot_[1] = slot_[2];
        slot_[2] = slot_[3];
        slot_[3] = 0.0f;
    }

private:
    // A step of `height` that happened `since` samples before the current
    // sample (0 <= since < 1): spread its residual over n-2 .. n+1.
    void step(double since, float height)
    {
        slot_[0] += height * blep(-2.0 + since);
        slot_[1] += height * blep(-1.0 + since);
        slot_[2] += height * blep(since);
        slot_[3] += height * blep(1.0 + since);
    }

    double baseNote_ = 64.0;
    bool kbt_ = true, on_ = true;
    bool pitchConnected_ = false, varConnected_ = false;
    float pitchMod_ = 0.0f;
    int wave_ = 0;
    double duty_ = 0.5;
    double phase_ = 0.0, inc_ = 0.0;
    float lastKey_ = -1e9f, lastPin_ = 0.0f, lastVar_ = 0.0f;
    float slot_[4] = {};
};

} // namespace

void registerOscA() { registerProcessor(97, [] { return std::make_unique<OscA>(); }); }

} // namespace g2::engine
