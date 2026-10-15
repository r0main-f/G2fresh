#pragma once
// The original's cable shape (Clavia's editor v1.62, CRenderSnakeABC; fidelity audit C9). A cubic between the two
// jack centres, with no gravity: each arm leaves along the line joining the jacks, turned by up to 60 degrees by a
// twist seeded per area (the Shake button picks a new seed) and by the jacks' coordinates. Plain C++; y points down.

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

namespace g2ui::cablecurve {

struct Pt {
    float x = 0, y = 0;
    bool operator==(const Pt&) const = default;
};

inline float distance(Pt a, Pt b)
{
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// CRenderSnakeABC::GetControlpoints @0001d13a: L / 3 below 45 px, else L / 5 + 9.
inline float armLength(float length)
{
    return length >= 45.0f ? length / 5.0f + 9.0f : length / 3.0f;
}

// CRenderSnakeABC::GetAngels @0001cb86: a0 from P0 towards P1 (atan with the quadrant put right; straight up or down
// when dx is 0), a1 = a0 + pi, wrapped into (-pi, pi].
inline std::pair<float, float> angles(Pt p0, Pt p1)
{
    constexpr float pi = std::numbers::pi_v<float>;
    const float dx = p1.x - p0.x, dy = p1.y - p0.y;
    float a0;
    if (dx == 0.0f) {
        a0 = dy >= 1.0f ? pi / 2 : -pi / 2;
    } else {
        a0 = std::atan(dy / dx);
        if (dx < 0.0f)
            a0 += dy < 0.0f ? -pi : pi;
    }
    float a1 = a0 + pi;
    if (a1 > pi)
        a1 -= 2 * pi;
    return {a0, a1};
}

// CRenderSnakeABC::TwistAngel @0001cc5e: the turn for a twisted angle f in [-pi, pi], piecewise linear through
// (-pi, 0), (-pi/2, -pi/3), (0, pi/3), (pi/2, -pi/3), (pi, 0).
inline float twistOffset(float f)
{
    constexpr float pi = std::numbers::pi_v<float>;
    if (f < -pi / 2)
        return (f + pi) * (-pi / 3) / (pi / 2);
    if (f < 0.0f)
        return (f + pi / 2) * (2 * pi / 3) / (pi / 2) - pi / 3;
    if (f < pi / 2)
        return f * (-2 * pi / 3) / (pi / 2) + pi / 3;
    return (f - pi / 2) * (pi / 3) / (pi / 2) - pi / 3;
}

// An arm's angle `a` turned by the twist `tw`: fully from 100 px on, by (L / 100)^2 (at least half) below.
inline float twisted(float a, float tw, float length)
{
    double f = static_cast<double>(a) + tw;
    while (f < -std::numbers::pi)
        f += 2 * std::numbers::pi;
    while (f > std::numbers::pi)
        f -= 2 * std::numbers::pi;
    const float full = a + twistOffset(static_cast<float>(f));
    if (length >= 100.0f)
        return full;
    float w = (length / 100.0f) * (length / 100.0f);
    if (w < 0.5f)
        w = 0.5f;
    return a * (1.0f - w) + full * w;
}

// The two control points, on whole pixels (CRenderSnakeABC::MovePoint @0001cd5c): C0 off P0, C1 off P1. The twist
// is sin(seed + x0 + y0 + x1 + y1) * 2 pi, in double as the original computes it.
inline std::pair<Pt, Pt> controlPoints(Pt p0, Pt p1, int seed)
{
    const float length = distance(p0, p1);
    const float arm = armLength(length);
    const auto [a0, a1] = angles(p0, p1);
    const float tw = static_cast<float>(
        std::sin(static_cast<double>(seed) + static_cast<double>(p0.x + p0.y + p1.x + p1.y)) * 6.283185307179586);
    auto move = [&](Pt p, float angle) {
        return Pt{std::floor(p.x + std::cos(angle) * arm + 0.5f), std::floor(p.y + std::sin(angle) * arm + 0.5f)};
    };
    return {move(p0, twisted(a0, tw, length)), move(p1, twisted(a1, tw, length))};
}

// BuildSegmentList @0001de3e: one point per 10 px, 5 to 20.
inline int sampleCount(float length)
{
    return std::clamp(static_cast<int>(length / 10.0f), 5, 20);
}

// The cable as the original draws it: points on the cubic from P1 (t = 0) to P0 (t = 1), closer together at the
// ends (t_i = (sin(-pi/2 + i pi / n) + 1) / 2, the last 1), rounded to whole pixels, a point closer than 4 px to the
// one before left out. The jack at the end is always kept. (The original also nudges each inner t towards a whole
// pixel; that is not done here.)
inline std::vector<Pt> polyline(Pt p0, Pt p1, int seed)
{
    const auto [c0, c1] = controlPoints(p0, p1, seed);
    const int n = sampleCount(distance(p0, p1));
    auto at = [&](double t) {
        const double u = 1.0 - t;
        const double b0 = 3 * t * t * u, b1 = 3 * t * u * u, b2 = u * u * u;
        const double x = p0.x + b0 * (c0.x - p0.x) + b1 * (c1.x - p0.x) + b2 * (p1.x - p0.x);
        const double y = p0.y + b0 * (c0.y - p0.y) + b1 * (c1.y - p0.y) + b2 * (p1.y - p0.y);
        return Pt{static_cast<float>(std::floor(x + 0.5)), static_cast<float>(std::floor(y + 0.5))};
    };
    std::vector<Pt> out;
    for (int i = 0; i < n; ++i) {
        const double t = i == n - 1 ? 1.0 : (std::sin(-std::numbers::pi / 2 + i * std::numbers::pi / n) + 1.0) / 2.0;
        const Pt p = at(t);
        if (i == n - 1) {
            if (!out.empty() && distance(out.back(), p) < 4.0f && out.size() > 1)
                out.back() = p;
            else
                out.push_back(p);
        } else if (out.empty() || distance(out.back(), p) >= 4.0f) {
            out.push_back(p);
        }
    }
    return out;
}

} // namespace g2ui::cablecurve
