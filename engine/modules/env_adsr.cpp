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

namespace g2::engine {
namespace {

// The editor's envelope time display (kEnvelopeTime), in milliseconds.
constexpr float kEnvTimeMs[128] = {
    0.5f, 0.6f, 0.7f, 0.9f, 1.1f, 1.3f, 1.5f, 1.8f, 2.1f, 2.5f, 3, 3.5f, 4, 4.7f, 5.5f, 6.3f, 7.3f, 8.4f, 9.7f,
    11.1f, 12.7f, 14.5f, 16.5f, 18.7f, 21.2f, 24, 27.1f, 30.6f, 34.4f, 38.7f, 43.4f, 48.6f, 54.3f, 60.6f, 67.6f,
    75.2f, 83.6f, 92.8f, 103, 114, 126, 139, 153, 169, 186, 204, 224, 246, 269, 295, 322, 352, 384, 419, 456, 496,
    540, 586, 636, 690, 748, 810, 876, 947, 1020, 1100, 1190, 1280, 1380, 1490, 1600, 1720, 1850, 1990, 2130, 2280,
    2450, 2620, 2810, 3000, 3210, 3430, 3660, 3910, 4170, 4450, 4740, 5050, 5370, 5720, 6080, 6470, 6870, 7300,
    7750, 8220, 8720, 9250, 9800, 10400, 11000, 11600, 12300, 13000, 13800, 14600, 15400, 16200, 17100, 18100,
    19100, 20100, 21200, 22400, 23500, 24800, 26100, 27500, 28900, 30400, 32000, 33600, 35300, 37100, 38900,
    40900, 42900, 45000,
};

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
        auto ticksOf = [&](std::size_t p) { return kEnvTimeMs[std::clamp(param(m, var, p), 0, 127)] * 1e-3 * kControlRate; };
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
