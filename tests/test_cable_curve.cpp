// The original's cable shape (Clavia's editor v1.62, CRenderSnakeABC): a cubic between the two jack centres whose arms
// leave along the line joining them, twisted by a seeded angle, with no gravity. Statements from the decompiled code
// and the float constants in its disassembly.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "CableCurve.h"

#include <numbers>

using namespace g2ui::cablecurve;
using Catch::Approx;

namespace {
constexpr float kPi = std::numbers::pi_v<float>;
}

TEST_CASE("cable curve: the arms, a third of the distance up to 45 px, then a fifth plus 9")
{
    // CRenderSnakeABC::GetControlpoints @0001d13a; constants 45.0 @2899ac, 5 @2899b0, 9 @2899b4, 3 @289988.
    CHECK(armLength(30.0f) == Approx(10.0f));
    CHECK(armLength(44.9f) == Approx(44.9f / 3.0f));
    CHECK(armLength(45.0f) == Approx(18.0f));
    CHECK(armLength(200.0f) == Approx(49.0f));
}

TEST_CASE("cable curve: each arm leaves along the line joining the jacks")
{
    // GetAngels @0001cb86: a0 from P0 towards P1, a1 = a0 + pi wrapped into (-pi, pi]; dx = 0 gives +-pi/2.
    auto [a0, a1] = angles({0, 0}, {10, 0});
    CHECK(a0 == Approx(0.0f).margin(1e-6));
    CHECK(a1 == Approx(kPi));
    std::tie(a0, a1) = angles({0, 0}, {0, 10});
    CHECK(a0 == Approx(kPi / 2));
    CHECK(a1 == Approx(-kPi / 2));
    std::tie(a0, a1) = angles({0, 0}, {-10, -10});
    CHECK(a0 == Approx(-3 * kPi / 4));
    CHECK(a1 == Approx(kPi / 4));
}

TEST_CASE("cable curve: the twist is at most 60 degrees, piecewise linear in the twisted angle")
{
    // TwistAngel @0001cc5e: through (-pi, 0), (-pi/2, -pi/3), (0, pi/3), (pi/2, -pi/3), (pi, 0).
    CHECK(twistOffset(-kPi) == Approx(0.0f).margin(1e-6));
    CHECK(twistOffset(-kPi / 2) == Approx(-kPi / 3));
    CHECK(twistOffset(0.0f) == Approx(kPi / 3));
    CHECK(twistOffset(kPi / 2) == Approx(-kPi / 3));
    CHECK(twistOffset(kPi) == Approx(0.0f).margin(1e-6));
    CHECK(twistOffset(kPi / 4) == Approx(0.0f).margin(1e-6));
}

TEST_CASE("cable curve: the control points, as an independent port of the same code computes them")
{
    // Vectors from a second float32 port of GetControlpoints written for the fidelity check (not from the original,
    // which does not run here): (x0, y0, x1, y1, seed) -> C0, C1.
    struct Case {
        Pt p0, p1;
        int seed;
        Pt c0, c1;
    };
    const Case cases[] = {
        {{100, 100}, {300, 150}, 0, {146, 80}, {250, 151}},  {{100, 100}, {300, 150}, 1, {150, 92}, {255, 127}},
        {{100, 100}, {300, 150}, 128, {150, 106}, {253, 167}}, {{50, 40}, {80, 60}, 7, {60, 47}, {69, 56}},
        {{10, 10}, {10, 200}, 255, {31, 52}, {-22, 166}},    {{200, 50}, {20, 50}, 42, {157, 62}, {56, 22}},
    };
    for (const auto& c : cases) {
        const auto [c0, c1] = controlPoints(c.p0, c.p1, c.seed);
        INFO(c.p0.x << "," << c.p0.y << " -> " << c.p1.x << "," << c.p1.y << " seed " << c.seed);
        CHECK(c0 == c.c0);
        CHECK(c1 == c.c1);
    }
}

TEST_CASE("cable curve: sampled into 5 to 20 points, one per 10 px, none closer than 4 px")
{
    // BuildSegmentList @0001de3e: n = clamp(int(L / 10), 5, 20); points closer than 4.0 px to the last are dropped.
    CHECK(sampleCount(30.0f) == 5);
    CHECK(sampleCount(120.0f) == 12);
    CHECK(sampleCount(900.0f) == 20);
    const auto line = polyline({100, 100}, {300, 150}, 0);
    REQUIRE(line.size() >= 2);
    CHECK(line.size() <= 20);
    // From one jack to the other.
    CHECK(((line.front() == Pt{100, 100} && line.back() == Pt{300, 150}) || (line.front() == Pt{300, 150} && line.back() == Pt{100, 100})));
    for (std::size_t i = 1; i + 1 < line.size(); ++i) {
        const float dx = line[i].x - line[i - 1].x, dy = line[i].y - line[i - 1].y;
        CHECK(dx * dx + dy * dy >= 16.0f);
    }
}
