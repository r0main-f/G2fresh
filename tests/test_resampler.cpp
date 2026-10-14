// The plugin's sample rate converter between the emulated G2 (96 kHz) and the host (plugin/Source/Resampler.h):
// its pass band, its stop band (what would fold or mirror), its delay, and that it is exact about its input.
#include "Resampler.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// `in` from `from` Hz to `to` Hz, in blocks of `block` output samples: as much output as the input gives.
std::vector<float> convert(double from, double to, const std::vector<float>& in, std::size_t block, std::size_t maxOut = 256)
{
    g2io::Resampler r;
    r.prepare(from, to, 1, maxOut);
    std::vector<float> out;
    std::vector<float> one(block);
    for (std::size_t at = 0;;) {
        const std::size_t need = r.needs(block);
        if (at + need > in.size())
            break;
        r.process(0, in.data() + at, 1, one.data(), 1, block);
        r.advance(block);
        at += need;
        out.insert(out.end(), one.begin(), one.end());
    }
    return out;
}

std::vector<float> sine(double hz, double rate, std::size_t n, double level = 1.0)
{
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i)
        v[i] = static_cast<float>(level * std::sin(2 * kPi * hz * static_cast<double>(i) / rate));
    return v;
}

struct Fit {
    double level = 0;    // the amplitude of the tone at `hz` in the signal
    double residue = 0;  // the rms of everything else, as the amplitude of a sine of that power
};

// The tone at `hz` in v[from, to) by least squares, and what is left without it.
Fit fit(const std::vector<float>& v, std::size_t from, std::size_t to, double rate, double hz)
{
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double w = 2 * kPi * hz * static_cast<double>(i) / rate, s = std::sin(w), c = std::cos(w);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        xs += v[i] * s;
        xc += v[i] * c;
    }
    const double det = ss * cc - sc * sc;
    const double a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
    double rest = 0;
    for (std::size_t i = from; i < to; ++i) {
        const double w = 2 * kPi * hz * static_cast<double>(i) / rate;
        const double e = v[i] - a * std::sin(w) - b * std::cos(w);
        rest += e * e;
    }
    return {std::hypot(a, b), std::sqrt(2.0 * rest / static_cast<double>(to - from))};
}

double rms(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double sum = 0;
    for (std::size_t i = from; i < to; ++i)
        sum += static_cast<double>(v[i]) * v[i];
    return std::sqrt(sum / static_cast<double>(to - from));
}

double dB(double ratio) { return 20.0 * std::log10((std::max)(ratio, 1e-12)); }

// A tone of level 1 at `hz` through the conversion: the level it comes out with at `hz` (below the output's Nyquist
// frequency) and the level of everything else (aliases, images), in dB.
struct Tone {
    double wanted = -240, others = -240;
};

Tone tone(double from, double to, double hz)
{
    const auto n = static_cast<std::size_t>(from * 0.25);
    const auto out = convert(from, to, sine(hz, from, n), 61);
    const std::size_t skip = 2000;
    Tone t;
    if (hz < to / 2) {
        const Fit f = fit(out, skip, out.size(), to, hz);
        t.wanted = dB(f.level);
        t.others = dB(f.residue);
    } else {
        t.others = dB(rms(out, skip, out.size()) * std::sqrt(2.0));
    }
    return t;
}

} // namespace

TEST_CASE("Resampler: the pass band is flat and the stop band 100 dB down, each way at each rate", "[resampler]")
{
    for (const double host : {44100.0, 48000.0, 88200.0, 176400.0, 192000.0}) {
        const double lower = (std::min)(host, 96000.0);
        const double edge = g2io::Resampler::passband(lower), stop = lower - edge;
        CAPTURE(host, edge, stop);
        for (const bool toHost : {true, false}) {
            const double from = toHost ? 96000.0 : host, to = toHost ? host : 96000.0;
            CAPTURE(from, to);
            // the pass band: within 0.01 dB, and nothing else comes with it
            for (const double share : {0.001, 0.05, 0.25, 0.5, 0.75, 0.9, 1.0}) {
                const double hz = share * edge;
                const Tone t = tone(from, to, hz);
                CAPTURE(hz, t.wanted, t.others);
                CHECK(std::abs(t.wanted) < 0.01);
                CHECK(t.others < -98.0);
            }
            // the stop band: what would fold into the pass band (going down) or mirror above it (going up)
            for (double hz = stop; hz < from / 2; hz += (from / 2 - stop) / 7.3) {
                const Tone t = tone(from, to, hz);
                CAPTURE(hz, t.wanted, t.others);
                CHECK(t.wanted < -98.0);
                CHECK(t.others < -98.0);
            }
        }
    }
}

TEST_CASE("Resampler: at 48 kHz the pass band reaches 20 kHz and the stop band starts at 28 kHz", "[resampler]")
{
    CHECK(g2io::Resampler::passband(48000.0) == 20000.0);
    CHECK_THAT(g2io::Resampler::passband(44100.0), Catch::Matchers::WithinAbs(20000.0, 1.0));
    CHECK(g2io::Resampler::passband(96000.0) == 40000.0);
    // a partial of the G2 at 30 kHz, which plain decimation plays at 18 kHz at full level
    const Tone folded = tone(96000.0, 48000.0, 30000.0);
    CHECK(folded.others < -98.0);
    // between the two bands the filter rolls off: 24 kHz is half-way (-6 dB)
    const auto out = convert(96000.0, 48000.0, sine(23000.0, 96000.0, 24000), 64);
    CHECK_THAT(dB(fit(out, 2000, out.size(), 48000.0, 23000.0).level), Catch::Matchers::WithinAbs(-2.5, 1.5));
}

TEST_CASE("Resampler: linear phase, delayed by half its length", "[resampler]")
{
    struct Case {
        double from, to;
    };
    for (const Case c : {Case{96000.0, 48000.0}, Case{48000.0, 96000.0}, Case{96000.0, 44100.0}, Case{44100.0, 96000.0}}) {
        CAPTURE(c.from, c.to);
        g2io::Resampler r;
        r.prepare(c.from, c.to, 1, 256);
        CHECK(r.taps() % 4 == 0);
        CHECK(r.delay() * 2 == r.taps());
        // an impulse at input sample 400 comes out centred on (400 + delay) * to / from, the same on both sides
        std::vector<float> in(2000, 0.0f);
        in[400] = 1.0f;
        const auto out = convert(c.from, c.to, in, 50);
        const double centre = (400.0 + static_cast<double>(r.delay())) * c.to / c.from;
        const auto peak = static_cast<std::size_t>(std::max_element(out.begin(), out.end()) - out.begin());
        CHECK(std::abs(static_cast<double>(peak) - centre) <= 0.5);
        if (centre == std::floor(centre)) {
            const auto at = static_cast<std::size_t>(centre);
            float worst = 0;
            for (std::size_t k = 1; k < 200; ++k)
                worst = (std::max)(worst, std::abs(out[at - k] - out[at + k]));
            CHECK(worst < 1e-7f);
        }
    }
    // the delays the plugin reports with: 0.42 ms each way at 48 kHz, 0.8 ms at 44.1 kHz
    g2io::Resampler r;
    r.prepare(96000.0, 48000.0, 1, 256);
    CHECK(r.delay() == 40);
    r.prepare(48000.0, 96000.0, 1, 256);
    CHECK(r.delay() == 20);
    r.prepare(96000.0, 44100.0, 1, 256);
    CHECK(r.delay() == 76);
    r.prepare(44100.0, 96000.0, 1, 256);
    CHECK(r.delay() == 36);
}

TEST_CASE("Resampler: to 96 kHz and back gives the host its signal, later", "[resampler]")
{
    for (const double host : {44100.0, 48000.0, 88200.0, 192000.0}) {
        CAPTURE(host);
        // tones up to the pass band's edge, at levels that add up to less than 1
        const double edge = g2io::Resampler::passband((std::min)(host, 96000.0));
        const auto n = static_cast<std::size_t>(host * 0.5);
        std::vector<float> in(n, 0.0f);
        const double tones[] = {0.011, 0.13, 0.37, 0.71, 0.97};
        for (const double share : tones) {
            const auto one = sine(share * edge, host, n, 0.18);
            for (std::size_t i = 0; i < n; ++i)
                in[i] += one[i];
        }
        g2io::Resampler up, down;
        up.prepare(host, 96000.0, 1, 256);
        down.prepare(96000.0, host, 1, 256);
        const auto back = convert(96000.0, host, convert(host, 96000.0, in, 113), 77);
        const double delay = static_cast<double>(up.delay()) + static_cast<double>(down.delay()) * host / 96000.0;
        CAPTURE(delay);
        // each tone comes back at its level, at the delay's phase; the rest is 90 dB down
        double worst = 0;
        std::vector<float> rest(back);
        for (const double share : tones) {
            const double hz = share * edge;
            for (std::size_t i = 0; i < rest.size(); ++i)
                rest[i] -= static_cast<float>(0.18 * std::sin(2 * kPi * hz * (static_cast<double>(i) - delay) / host));
        }
        for (std::size_t i = 4000; i < rest.size(); ++i)
            worst = (std::max)(worst, static_cast<double>(std::abs(rest[i])));
        CHECK(dB(worst / 0.18) < -70.0);
        CHECK(dB(rms(rest, 4000, rest.size()) / (0.18 / std::sqrt(2.0))) < -80.0);
    }
}

TEST_CASE("Resampler: the same samples whatever the block size, and exactly the input it says", "[resampler]")
{
    std::vector<float> in(30000);
    std::uint32_t seed = 7;
    for (auto& x : in) {
        seed = seed * 1664525u + 1013904223u;
        x = static_cast<float>(static_cast<std::int32_t>(seed)) / 2147483648.0f;
    }
    for (const double host : {44100.0, 48000.0, 88200.0, 192000.0, 47999.0}) {
        for (const bool toHost : {true, false}) {
            const double from = toHost ? 96000.0 : host, to = toHost ? host : 96000.0;
            CAPTURE(from, to);
            const auto whole = convert(from, to, in, 1);
            for (const std::size_t block : {std::size_t{7}, std::size_t{64}, std::size_t{256}, std::size_t{1000}}) {
                const auto blocks = convert(from, to, in, block, 256); // 1000: more than prepare() was told
                REQUIRE(blocks.size() <= whole.size());
                CHECK(std::equal(blocks.begin(), blocks.end(), whole.begin()));
            }
            // n output samples take the input up to the last one's place: sample j is at j * from / to
            g2io::Resampler r;
            r.prepare(from, to, 1, 256);
            std::size_t taken = 0, made = 0;
            std::vector<float> out(300);
            for (const std::size_t n : {std::size_t{1}, std::size_t{0}, std::size_t{255}, std::size_t{3}, std::size_t{256}, std::size_t{100}}) {
                const auto need = r.needs(n);
                r.process(0, in.data() + taken, 1, out.data(), 1, n);
                r.advance(n);
                taken += need;
                made += n;
                CHECK(taken == static_cast<std::size_t>(std::floor(static_cast<double>(made - 1) * std::round(from) / std::round(to))) + 1);
                CHECK_THAT(r.position(0), Catch::Matchers::WithinAbs(static_cast<double>(made) * std::round(from) / std::round(to) - static_cast<double>(taken), 1e-9));
            }
        }
    }
}

TEST_CASE("Resampler: the plugin's two conversions never want more of the host's input than a block brought", "[resampler]")
{
    // per block of `n` host samples: the 96 kHz frames the outputs take, and for as many frames to the G2 the host
    // samples the inputs take: never more than the host has given so far
    for (const double host : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0, 47999.0, 22050.0}) {
        CAPTURE(host);
        g2io::Resampler down, up;
        down.prepare(96000.0, host, 1, 512);
        up.prepare(host, 96000.0, 1, 2048);
        std::size_t given = 0, used = 0, most = 0;
        for (int block = 0; block < 4000; ++block) {
            const std::size_t n = 1 + static_cast<std::size_t>((block * 37) % 512);
            given += n;
            const std::size_t frames = down.needs(n);
            down.advance(n);
            used += up.needs(frames);
            up.advance(frames);
            REQUIRE(used <= given);
            most = (std::max)(most, given - used);
        }
        CHECK(most <= 1); // and it keeps at most one sample waiting
    }
}

TEST_CASE("Resampler: equal rates pass the samples as they are; silence gives silence", "[resampler]")
{
    g2io::Resampler same;
    same.prepare(96000.0, 96000.0, 2, 64);
    CHECK(same.transparent());
    CHECK(same.delay() == 0);
    CHECK(same.needs(100) == 100);
    std::vector<float> in(200), out(100, 0.0f);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(i) * 0.37f - 20.0f;
    same.process(1, in.data() + 1, 2, out.data(), 1, 100); // the second channel of interleaved frames
    same.advance(100);
    for (std::size_t i = 0; i < 100; ++i)
        REQUIRE(out[i] == in[2 * i + 1]);

    // DC comes out as it went in
    g2io::Resampler r;
    r.prepare(96000.0, 44100.0, 1, 256);
    CHECK_FALSE(r.transparent());
    const auto dc = convert(96000.0, 44100.0, std::vector<float>(4000, 0.5f), 100);
    for (std::size_t i = 500; i < dc.size(); ++i)
        REQUIRE_THAT(dc[i], Catch::Matchers::WithinAbs(0.5, 2e-6));

    // a sound, then silence: exact zeros once the filter has run out, and a sound again after it
    std::vector<float> burst(6000, 0.0f);
    for (std::size_t i = 100; i < 200; ++i)
        burst[i] = 0.9f;
    for (std::size_t i = 5000; i < 5100; ++i)
        burst[i] = -0.9f;
    const auto played = convert(96000.0, 44100.0, burst, 64);
    const auto at = [](std::size_t in96) { return static_cast<std::size_t>(static_cast<double>(in96) * 44100.0 / 96000.0); };
    CHECK(*std::max_element(played.begin() + static_cast<std::ptrdiff_t>(at(100)), played.begin() + static_cast<std::ptrdiff_t>(at(400))) > 0.8f);
    for (std::size_t i = at(200 + 152) + 2; i < at(5000); ++i)
        REQUIRE(played[i] == 0.0f);
    CHECK(*std::min_element(played.begin() + static_cast<std::ptrdiff_t>(at(5000)), played.end()) < -0.8f);
}
