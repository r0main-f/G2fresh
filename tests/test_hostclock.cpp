// The host's tempo and transport as MIDI clock (plugin/Source/HostClock.h).
#include "HostClock.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace {

struct Msg {
    int at;
    std::uint8_t status, d1, d2;
    int size;
};

std::vector<Msg> block(HostClock& c, const HostClock::Position& p, int samples, double rate = 48000)
{
    std::vector<Msg> v;
    c.process(p, rate, samples, [&](int at, std::uint8_t s, std::uint8_t a, std::uint8_t b, int n) { v.push_back({at, s, a, b, n}); });
    return v;
}

std::vector<Msg> only(const std::vector<Msg>& v, std::uint8_t status)
{
    std::vector<Msg> r;
    for (const auto& m : v)
        if (m.status == status)
            r.push_back(m);
    return r;
}

} // namespace

TEST_CASE("HostClock: Start from the top, then 24 ticks per quarter at the host's tempo", "[hostclock]")
{
    HostClock c;
    // 120 BPM at 48 kHz: a quarter is 24000 samples, a tick 1000
    auto v = block(c, {120, true, 0.0}, 4096);
    REQUIRE(!v.empty());
    CHECK(v.front().status == 0xFA);  // Start
    const auto ticks = only(v, 0xF8);
    REQUIRE(ticks.size() == 5);  // at 0, 1000, 2000, 3000, 4000
    for (std::size_t i = 0; i < ticks.size(); ++i)
        CHECK(ticks[i].at == static_cast<int>(i) * 1000);
    // the next block, continuing: no Start again, ticks at 904 and 1904 (5000, 6000 from the top)
    v = block(c, {120, true, 4096.0 / 24000.0}, 2048);
    CHECK(only(v, 0xFA).empty());
    const auto next = only(v, 0xF8);
    REQUIRE(next.size() == 2);
    CHECK(next[0].at == 904);
    CHECK(next[1].at == 1904);
}

TEST_CASE("HostClock: starting mid-song sends Song Position and Continue; stopping sends Stop", "[hostclock]")
{
    HostClock c;
    auto v = block(c, {100, true, 4.25}, 512);  // beat 4.25: sixteenth 17
    REQUIRE(v.size() >= 2);
    CHECK(v[0].status == 0xF2);
    CHECK(v[0].d1 == 17);
    CHECK(v[0].d2 == 0);
    CHECK(v[1].status == 0xFB);
    v = block(c, {100, false, 4.3}, 512);
    REQUIRE(!v.empty());
    CHECK(v.front().status == 0xFC);
}

TEST_CASE("HostClock: a jump while playing (a loop) is Stop, Song Position, Continue", "[hostclock]")
{
    HostClock c;
    block(c, {120, true, 7.9}, 4800);  // 0.2 quarter at 120 BPM
    auto v = block(c, {120, true, 0.0}, 4800);  // back to the top
    REQUIRE(v.size() >= 3);
    CHECK(v[0].status == 0xFC);
    CHECK(v[1].status == 0xF2);
    CHECK(v[1].d1 == 0);
    CHECK(v[2].status == 0xFB);
}

TEST_CASE("HostClock: stopped, the ticks go on at the host's tempo, evenly across blocks", "[hostclock]")
{
    HostClock c;
    std::vector<long> at;
    long origin = 0;
    for (int b = 0; b < 40; ++b) {
        for (const auto& m : block(c, {90, false, std::nullopt}, 333))
            if (m.status == 0xF8)
                at.push_back(origin + m.at);
        origin += 333;
    }
    // 90 BPM at 48 kHz: a tick every 1333.33 samples
    REQUIRE(at.size() >= 9);
    for (std::size_t i = 1; i < at.size(); ++i)
        CHECK(std::abs((at[i] - at[i - 1]) - 1333.33) < 1.5);
}

TEST_CASE("HostClock: no tempo, no clock", "[hostclock]")
{
    HostClock c;
    CHECK(block(c, {0, true, 1.0}, 512).empty());
}

TEST_CASE("HostClock: over a minute of blocks, exactly 24 ticks per quarter, none twice", "[hostclock]")
{
    // 100 BPM at 48 kHz in 512-sample blocks: a tick every 1200 samples, so every 19200 samples one falls exactly on
    // a block boundary, where rounding must not send it in both blocks
    for (const double bpm : {100.0, 120.0, 97.3}) {
        HostClock c;
        long count = 0, last = -1;
        bool ordered = true;
        for (long s = 0; s < 48000L * 60; s += 512) {
            const double ppq = static_cast<double>(s) / 48000.0 * bpm / 60.0;
            for (const auto& m : block(c, {bpm, true, ppq}, 512))
                if (m.status == 0xF8) {
                    ordered = ordered && s + m.at > last;
                    last = s + m.at;
                    ++count;
                }
        }
        INFO(bpm << " BPM");
        CHECK(ordered);
        const double expected = 60.0 * bpm / 60.0 * 24.0;
        CHECK(std::abs(static_cast<double>(count) - expected) <= 1.0);
    }
}
