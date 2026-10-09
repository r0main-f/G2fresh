// Keyboard (type 1): the voice's keyboard signals.
//
// Spec: module database (outputs Pitch, Gate, Lin, Release, Note, Exp) and the
// G2's documented signal convention (note signal: 0 at E4 = note 64, one unit
// = 1/64 per semitone). Lin is the note-on velocity 0..1, Release the release
// velocity, Exp a squared velocity curve (inferred), Note the note signal
// without glide (no glide yet: equal to Pitch).
#include "common.hpp"

namespace g2::engine {
namespace {

class Keyboard final : public Processor {
public:
    void update(const Module&, std::uint8_t) override {}
    void process(const float*, float* out, Io& io) override
    {
        out[0] = io.pitch;
        out[1] = io.gate;
        out[2] = io.velocity;
        out[3] = io.releaseVelocity;
        out[4] = io.pitch;
        out[5] = io.velocity * io.velocity;
    }
};

} // namespace

void registerKeyboard() { registerProcessor(1, [] { return std::make_unique<Keyboard>(); }); }

} // namespace g2::engine
