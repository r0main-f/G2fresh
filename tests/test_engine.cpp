// Native engine (g2engine) module processors: properties measured on the
// emulated G2 as a black box (re/notes/native-engine.md), checked here without
// the emulator.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "g2/engine/engine.hpp"
#include "g2/patch.hpp"
#include "g2/uprate.hpp"

#include <cmath>
#include <numbers>
#include <complex>
#include <map>
#include <string>
#include <vector>

using namespace g2;
using Catch::Approx;

namespace {

constexpr double kRate = engine::kSampleRate;
constexpr double kPi = 3.14159265358979323846;

const db::ModuleDef& def(const std::string& name)
{
    for (const auto& m : db::modules())
        if (m.kind == db::ModuleKind::Module && name == m.shortName)
            return m;
    FAIL("no module " << name);
    throw;
}

// A VA patch built by name: modules, parameters (all variations), cables.
struct Builder {
    Patch patch = Patch::makeDefault();
    std::map<std::string, u8> mods;
    u8 col = 0;

    Builder() { setGain(127); }
    Builder& add(const std::string& name, const std::string& type)
    {
        mods[name] = edit::addModule(patch, Location::Va, def(type).typeId, col++, 0);
        return *this;
    }
    Builder& set(const std::string& name, const std::string& param, int value)
    {
        const auto* d = patch.va.find(mods.at(name))->def();
        for (std::size_t p = 0; p < d->params.size(); ++p)
            if (param == d->params[p].name) {
                for (u8 v = 0; v < kFileVariations; ++v)
                    edit::setParam(patch, Location::Va, mods.at(name), u8(p), v, u8(value));
                return *this;
            }
        for (std::size_t m = 0; m < d->modes.size(); ++m)
            if (param == d->modes[m].name) {
                edit::setMode(patch, Location::Va, mods.at(name), u8(m), u8(value));
                return *this;
            }
        FAIL("no parameter " << param);
        return *this;
    }
    Builder& cable(const std::string& from, int out, const std::string& to, int in)
    {
        edit::connect(patch, Location::Va, {mods.at(from), u8(out), true}, {mods.at(to), u8(in), false});
        uprate::update(patch, Location::Va);
        return *this;
    }
    Builder& setGain(int v)
    {
        for (u8 var = 0; var < kFileVariations; ++var)
            edit::setSetting(patch, edit::Setting::Gain, 0, var, u8(v));
        return *this;
    }
};

std::vector<float> render(const Patch& p, double seconds, int channel = 0,
                          const std::vector<std::pair<double, int>>& keys = {}, double keyOff = -1)
{
    engine::PatchEngine e(p);
    REQUIRE(e.unsupported().empty());
    const int n = int(seconds * kRate);
    std::vector<float> out[4];
    for (auto& o : out)
        o.assign(std::size_t(n), 0.0f);
    int pos = 0;
    auto renderTo = [&](int at) {
        at = std::min(at, n);
        if (at > pos)
            e.render({out[0].data() + pos, out[1].data() + pos, out[2].data() + pos, out[3].data() + pos}, at - pos);
        pos = std::max(pos, at);
    };
    for (const auto& [t, note] : keys) {
        renderTo(int(t * kRate));
        e.setKey(note, true);
    }
    if (keyOff >= 0) {
        renderTo(int(keyOff * kRate));
        e.setKey(keys.empty() ? 64 : keys.back().second, false);
    }
    renderTo(n);
    return out[channel];
}

// Frequency from rising zero crossings (linear interpolation).
double frequency(const std::vector<float>& x, std::size_t from = 4800)
{
    double first = -1, last = -1;
    int count = 0;
    for (std::size_t i = from + 1; i < x.size(); ++i)
        if (x[i - 1] < 0 && x[i] >= 0) {
            const double t = double(i - 1) + x[i - 1] / double(x[i - 1] - x[i]);
            if (first < 0)
                first = t;
            else
                ++count;
            last = t;
        }
    return count / ((last - first) / kRate);
}

// Amplitude of the component at f (Hann-windowed single-bin DFT).
double amplitude(const std::vector<float>& x, double f, std::size_t from, std::size_t len)
{
    std::complex<double> s = 0;
    double wsum = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * double(i) / double(len));
        s += w * double(x[from + i]) * std::polar(1.0, -2 * kPi * f * double(i) / kRate);
        wsum += w;
    }
    return 2 * std::abs(s) / wsum;
}

double dB(double x) { return 20 * std::log10(x); }

Patch oscPatch(int wave, int coarse, int kbt = 0)
{
    Builder b;
    b.add("osc", "OscA").set("osc", "Wave", wave).set("osc", "Coarse", coarse).set("osc", "KBT", kbt);
    b.add("out", "2-Out").cable("osc", 0, "out", 0);
    return b.patch;
}

} // namespace

TEST_CASE("OscA: Coarse is the note's frequency, 440 Hz at 69", "[engine]")
{
    CHECK(frequency(render(oscPatch(0, 69), 1.0)) == Approx(440.0).epsilon(1e-5));
    CHECK(frequency(render(oscPatch(0, 45), 1.0)) == Approx(110.0).epsilon(1e-5));
    CHECK(frequency(render(oscPatch(0, 100), 1.0)) == Approx(440.0 * std::exp2(31.0 / 12)).epsilon(1e-5));
}

TEST_CASE("OscA: keyboard tracking follows the note signal", "[engine]")
{
    // Coarse 64 (E4) tracks the key: note 76 is an octave up.
    const auto x = render(oscPatch(0, 64, 1), 1.0, 0, {{0.0, 76}});
    CHECK(frequency(x) == Approx(329.6276 * 2).epsilon(1e-5));
}

TEST_CASE("OscA: amplitude 1 and DC-free pulses", "[engine]")
{
    for (int wave = 0; wave < 6; ++wave) {
        const auto x = render(oscPatch(wave, 45), 0.5);
        double peak = 0, sum = 0;
        for (std::size_t i = 4800; i < 4800 + 38400; ++i) { // 44 whole periods of 110 Hz
            peak = std::max(peak, double(std::abs(x[i])));
            sum += x[i];
        }
        INFO("wave " << wave);
        const double expectedPeak = wave == 4 ? 1.5 : wave == 5 ? 1.875 : 1.0;
        CHECK(peak == Approx(expectedPeak).margin(0.01));
        CHECK(std::abs(sum / 38400.0) < 1e-4);
    }
}

TEST_CASE("OscA: the saw has 1/n harmonics and is band-limited", "[engine]")
{
    const auto x = render(oscPatch(2, 45), 1.0); // 110 Hz
    const double h1 = amplitude(x, 110, 9600, 76800);
    CHECK(dB(amplitude(x, 220, 9600, 76800) / h1) == Approx(-6.02).margin(0.05));
    CHECK(dB(amplitude(x, 550, 9600, 76800) / h1) == Approx(-13.98).margin(0.05));
    // At 2.6 kHz the highest harmonics roll off (measured on the G2: H16 at
    // 42 kHz is -61 dB re H1 instead of -24 dB), so aliases stay low.
    const auto y = render(oscPatch(2, 100), 1.0);
    const double f = 440.0 * std::exp2(31.0 / 12);
    const double g1 = amplitude(y, f, 9600, 76800);
    CHECK(dB(amplitude(y, 16 * f, 9600, 76800) / g1) < -50);
    CHECK(dB(amplitude(y, 2 * f, 9600, 76800) / g1) == Approx(-6.3).margin(0.3));
}

TEST_CASE("FltLP: -3 dB at the displayed cutoff, cascaded slopes", "[engine]")
{
    // A sine at the cutoff of Freq 75 (1046.5 Hz) through the filter.
    auto through = [](int freq, int slope, double f) {
        Builder b;
        b.add("osc", "OscA").set("osc", "Wave", 0).set("osc", "KBT", 0).set("osc", "Coarse", 69);
        // Coarse/Fine give 440 Hz; scale to f with Fine is not enough, so pick the note.
        const double note = 69 + 12 * std::log2(f / 440.0);
        b.set("osc", "Coarse", int(std::lround(note)));
        b.add("flt", "FltLP").set("flt", "Freq", freq).set("flt", "KBT", 0).set("flt", "SlopeMode", slope);
        b.add("out", "2-Out").cable("osc", 0, "flt", 0).cable("flt", 0, "out", 0);
        const auto x = render(b.patch, 0.5);
        return dB(amplitude(x, 440.0 * std::exp2((std::lround(note) - 69) / 12.0), 9600, 38400));
    };
    // One pole y += k (x - y), k = 2 pi fc / fs: -2.86 dB at fc (the
    // emulated G2: -3.07 dB at 1.1 kHz), stages cascade.
    CHECK(through(75, 0, 1046.5) == Approx(-2.86).margin(0.05));
    CHECK(through(75, 1, 1046.5) == Approx(-5.72).margin(0.1));
    CHECK(through(75, 5, 1046.5) == Approx(-17.15).margin(0.3));
    // An octave above the cutoff: -6.74 dB per pole.
    CHECK(through(75, 0, 2093.0) == Approx(-6.74).margin(0.05));
    CHECK(through(75, 5, 2093.0) == Approx(-40.44).margin(0.3));
    // Fully open at 127.
    CHECK(through(127, 0, 8372.0) == Approx(0.0).margin(0.01));
}

TEST_CASE("EnvADSR: displayed attack time, decay/release to 1 %, sustain v/128", "[engine]")
{
    Builder b;
    b.add("env", "EnvADSR")
        .set("env", "Attack", 40)  // 126 ms
        .set("env", "Decay", 50)   // 322 ms
        .set("env", "Sustain", 64) // 0.5
        .set("env", "Release", 45) // 204 ms
        .add("out", "2-Out")
        .cable("env", 0, "out", 0);
    const auto x = render(b.patch, 2.0, 0, {{0.1, 64}}, 1.2);
    auto at = [&](double t) { return double(x[std::size_t(t * kRate)]); };
    // Attack: reaches 1 at 126 ms (LogExp: concave, 0.80 at half time).
    std::size_t peak = 0;
    for (std::size_t i = 0; i < x.size(); ++i)
        if (x[i] >= 0.99999f) {
            peak = i;
            break;
        }
    CHECK((double(peak) / kRate - 0.1) * 1000 == Approx(126.0).margin(0.2));
    CHECK(at(0.1 + 0.063) == Approx(0.803).margin(0.01));
    // Decay: 1 % of the way left after 322 ms.
    CHECK((at(0.226 + 0.322) - 0.5) / 0.5 == Approx(0.01).margin(0.001));
    CHECK(at(1.1) == Approx(0.5).margin(1e-4));
    // Release: 1 % after 204 ms.
    CHECK(at(1.2 + 0.204) / 0.5 == Approx(0.01).margin(0.001));
}

TEST_CASE("Mixers and outputs: measured level curves", "[engine]")
{
    auto level = [](int lev, int curve) {
        Builder b;
        b.add("osc", "OscA").set("osc", "Wave", 0).set("osc", "KBT", 0).set("osc", "Coarse", 69);
        b.add("mix", "Mix2-1A").set("mix", "Level1", lev).set("mix", "Lin/Exp", curve);
        b.add("out", "2-Out").cable("osc", 0, "mix", 0).cable("mix", 0, "out", 0);
        return amplitude(render(b.patch, 0.3), 440, 4800, 19200);
    };
    CHECK(level(100, 0) == Approx(0.4912).margin(5e-4));
    CHECK(level(64, 0) == Approx(0.1317).margin(5e-4));
    CHECK(level(100, 1) == Approx(0.78125).margin(5e-4));
    CHECK(level(127, 1) == Approx(1.0).margin(5e-4));
    CHECK(level(100, 2) == Approx(0.4912).margin(5e-4));

    // Mix4-1A sums; the patch Gain (100 = -6.2 dB) and the 2-Out Pad (+6 dB) scale the output.
    Builder b;
    b.add("o1", "OscA").set("o1", "Wave", 0).set("o1", "KBT", 0).set("o1", "Coarse", 69);
    b.add("o2", "OscA").set("o2", "Wave", 0).set("o2", "KBT", 0).set("o2", "Coarse", 81);
    b.add("mix", "Mix4-1A").add("out", "2-Out").set("out", "Pad", 1).setGain(100);
    b.cable("o1", 0, "mix", 0).cable("o2", 0, "mix", 3).cable("mix", 0, "out", 0);
    const auto x = render(b.patch, 0.3);
    CHECK(amplitude(x, 440, 4800, 19200) == Approx(2 * 0.4912).margin(1e-3));
    CHECK(amplitude(x, 880, 4800, 19200) == Approx(2 * 0.4912).margin(1e-3));
}

TEST_CASE("Linked inputs share their net's source, whichever end the cable reaches", "[engine]")
{
    // OscA -> 4-Out input 1, and the 4-Out's inputs linked 2->1, 3->2, 4->3
    // (as stored in a real patch): all four outputs carry the oscillator.
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, 97, 0, 0);   // OscA
    const u8 out = edit::addModule(p, Location::Va, 3, 0, 4);    // 4-Out
    edit::connect(p, Location::Va, {osc, 0, true}, {out, 0, false});
    for (u8 i = 1; i < 4; ++i)
        p.va.cables.push_back(Cable{CableColor::Red, out, i, false, out, static_cast<u8>(i - 1), std::nullopt});
    engine::PatchEngine e(p);
    std::vector<float> outs[4];
    for (auto& o : outs)
        o.assign(4800, 0.0f);
    e.render({outs[0].data(), outs[1].data(), outs[2].data(), outs[3].data()}, 4800);
    for (const auto& o : outs) {
        float peak = 0;
        for (float v : o)
            peak = std::max(peak, std::abs(v));
        CHECK(peak > 0.1f);
    }
}

TEST_CASE("Polyphony: each note gets its own voice; a full patch steals the oldest", "[engine]")
{
    Patch p = oscPatch(0, 64, 1); // a sine tracking the keyboard
    p.header.monoMode = 0;
    p.header.voiceCount = 2;
    engine::PatchEngine e(p);
    REQUIRE(e.voices() == 2);
    auto level = [](const std::vector<float>& x, double f) { // Goertzel, normalised
        double re = 0, im = 0;
        for (std::size_t k = 0; k < x.size(); ++k) {
            re += x[k] * std::cos(2 * std::numbers::pi * f * double(k) / 96000.0);
            im += x[k] * std::sin(2 * std::numbers::pi * f * double(k) / 96000.0);
        }
        return std::hypot(re, im) / double(x.size());
    };
    auto block = [&] {
        std::vector<float> l(9600);
        e.render({l.data(), nullptr, nullptr, nullptr}, 9600);
        return l;
    };
    const double e4 = 329.6276, e5 = e4 * 2, b4 = e4 * std::exp2(7.0 / 12);
    e.noteOn(64, 100);
    e.noteOn(76, 100);
    auto x = block();
    CHECK(level(x, e4) > 0.2);
    CHECK(level(x, e5) > 0.2);
    // A third note steals the voice started first (E4).
    e.noteOn(71, 100);
    x = block();
    CHECK(level(x, e4) < 0.05); // only leakage from the other two tones (short, unwindowed block)
    CHECK(level(x, e5) > 0.2);
    CHECK(level(x, b4) > 0.2);
    // Mono patches play one voice.
    p.header.monoMode = 1;
    CHECK(engine::PatchEngine(p).voices() == 1);
}
