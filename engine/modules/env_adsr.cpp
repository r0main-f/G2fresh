// EnvADSR (type 20): ADSR envelope with a VCA (Out = In * Env).
//
// Spec, clean-room (re/notes/native-engine.md):
// * Stage times are the editor's EnvTime display (core/param_text,
//   kEnvelopeTime), in ms below.
// * Measured as a black box on the emulated G2 (Env output to a 2-Out, so
//   the envelope is read sample by sample; times scaled by the emulator's
//   control-rate factor, see the note):
//   - the attack reaches 1.0 in the displayed time; LogExp/ExpExp shapes
//     are exponential segments with k = 2.795 time constants over the
//     attack (Log: toward 1/(1-e^-k) = 1.065, clipped at 1; Exp: the
//     mirror image, growth of (y + 1/(e^k - 1))); Lin shapes are straight;
//   - decay and release are exponential with the displayed time = a fall
//     to 1 % (time constant T / ln 100); LinLin decays and releases linearly
//     at full scale per displayed time;
//   - Sustain = v/128, 127 = 1.0.
// * Runs its envelope at control rate (every 4th sample, catalog: 31 of its
//   36 cycles are control-rate) and the VCA at the module's rate.
// * KBG: the keyboard gate, ORed with the Gate input. OutType (Pos, PosInv,
//   Neg, NegInv, Bip, BipInv) applies to the Env output; the VCA uses the
//   positive envelope (inferred, not measured). AM scales the envelope
//   (1 when unconnected). Reset restarts the attack from 0 (inferred).
#include "common.hpp"

#include "g2/param_text.hpp"

#include <array>
#include <string>

namespace g2::engine {
namespace {

// The editor's envelope time display, in milliseconds: the core's display
// table (g2::paramtext::EnvelopeTime, " 0.5m" .. "45.0s"), so the times are
// the ones the editor shows.
float envTimeMs(int v)
{
    static const auto table = [] {
        std::array<float, 128> t{};
        for (int i = 0; i < 128; ++i) {
            const std::string s = paramtext::EnvelopeTime(static_cast<std::uint8_t>(i));
            const float n = std::stof(s);
            t[static_cast<std::size_t>(i)] = s.back() == 's' ? n * 1000.0f : n;
        }
        return t;
    }();
    return table[static_cast<std::size_t>(std::clamp(v, 0, 127))];
}

constexpr double kControlRate = kSampleRate / kControlDivider;
constexpr double kShapeK = 2.795; // exponential attack: time constants per attack time

class EnvADSR final : public Processor {
public:
    void connected(const std::vector<bool>& inputs) override
    {
        gateConnected_ = inputs.size() > 1 && inputs[1];
        amConnected_ = inputs.size() > 2 && inputs[2];
    }
    void update(const Module& m, std::uint8_t var) override
    {
        shape_ = param(m, var, 0);
        auto ticksOf = [&](std::size_t p) { return envTimeMs(param(m, var, p)) * 1e-3 * kControlRate; };
        const double ta = ticksOf(1), td = ticksOf(2), tr = ticksOf(4);
        const int s = param(m, var, 3);
        sustain_ = s >= 127 ? 1.0 : s / 128.0;
        // Attack per tick.
        attackLin_ = 1.0 / ta;
        attackK_ = 1.0 - std::exp(-kShapeK / ta);           // Log: y += (target - y) k
        attackTarget_ = 1.0 / (1.0 - std::exp(-kShapeK));
        attackGrow_ = std::exp(kShapeK / ta) - 1.0;          // Exp: y += (y + offset) g
        attackOffset_ = 1.0 / (std::exp(kShapeK) - 1.0);
        decayMul_ = std::exp(-std::log(100.0) / td);
        releaseMul_ = std::exp(-std::log(100.0) / tr);
        decayLin_ = 1.0 / td;
        releaseLin_ = 1.0 / tr;
        outType_ = param(m, var, 5);
        kbg_ = param(m, var, 6) != 0;
        reset_ = param(m, var, 7) != 0;
        audio_ = m.uprate;
    }

    void process(const float* in, float* out, Io& io) override
    {
        if (!audio_ || io.controlTick)
            tick(io, in);
        const float am = amConnected_ ? in[2] : 1.0f;
        const float e = static_cast<float>(y_) * am;
        out[1] = in[0] * e;
        switch (outType_) {
        case 0: out[0] = e; break;
        case 1: out[0] = 1.0f - e; break;
        case 2: out[0] = -e; break;
        case 3: out[0] = e - 1.0f; break;
        case 4: out[0] = 2.0f * e - 1.0f; break;
        default: out[0] = 1.0f - 2.0f * e; break;
        }
    }

private:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    void tick(const Io& io, const float* in)
    {
        const bool gate = (kbg_ && io.gate > 0.0f) || (gateConnected_ && in[1] > 0.0f);
        if (gate && !gate_) {
            stage_ = Stage::Attack;
            if (reset_)
                y_ = 0.0;
        } else if (!gate && gate_) {
            stage_ = Stage::Release;
        }
        gate_ = gate;
        const bool linDecay = shape_ == 3;
        switch (stage_) {
        case Stage::Idle:
        case Stage::Sustain:
            if (stage_ == Stage::Sustain)
                y_ = sustain_; // follows Sustain changes
            break;
        case Stage::Attack:
            if (shape_ == 1 || shape_ == 3)
                y_ += attackLin_;
            else if (shape_ == 0)
                y_ += (attackTarget_ - y_) * attackK_;
            else
                y_ += (y_ + attackOffset_) * attackGrow_;
            if (y_ >= 1.0) {
                y_ = 1.0;
                stage_ = Stage::Decay;
            }
            break;
        case Stage::Decay:
            if (linDecay) {
                y_ -= decayLin_;
                if (y_ <= sustain_) {
                    y_ = sustain_;
                    stage_ = Stage::Sustain;
                }
            } else {
                y_ = sustain_ + (y_ - sustain_) * decayMul_;
            }
            break;
        case Stage::Release:
            if (linDecay) {
                y_ = std::max(0.0, y_ - releaseLin_);
            } else {
                y_ *= releaseMul_;
            }
            if (y_ <= 1e-7) {
                y_ = 0.0;
                stage_ = Stage::Idle;
            }
            break;
        }
    }

    int shape_ = 0, outType_ = 0;
    bool kbg_ = true, reset_ = false, audio_ = false;
    bool gateConnected_ = false, amConnected_ = false;
    double sustain_ = 0.0;
    double attackLin_ = 0, attackK_ = 0, attackTarget_ = 1, attackGrow_ = 0, attackOffset_ = 0;
    double decayMul_ = 0, releaseMul_ = 0, decayLin_ = 0, releaseLin_ = 0;
    Stage stage_ = Stage::Idle;
    bool gate_ = false;
    double y_ = 0.0;
};

} // namespace

void registerEnvADSR() { registerProcessor(20, [] { return std::make_unique<EnvADSR>(); }); }

} // namespace g2::engine
