// Tests for the module graph port (core/src/graphs.cpp).
//
// The expected values were produced by running the original editor's own
// graph classes (G2Editor_i386, Mac v1.62: CCustomObjectFactory::CreateCustom,
// then UpdateGraphics() and Draw()) in a CPU emulator that records the drawing
// primitives; see re/notes/module-graphs.md and tools/graphs/. For every graph
// id, the test re-creates the same value tuples as tools/graphs/cases.py and
// compares a digest of every primitive (integer coordinates, colours, order)
// with the original's, plus a readable spot check of one case.
#include <catch2/catch_test_macros.hpp>

#include "g2/graphs.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gr = g2::graphs;

namespace {

// --- the cases of tools/graphs/cases.py ------------------------------------------

struct Lcg {
    std::uint32_t state;
    int below(int n)
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<int>((state >> 8) % static_cast<std::uint32_t>(n));
    }
};

constexpr int R = 127;

struct Variant {
    int id;
    std::vector<int> max; // inclusive maximum of each dependency (minimum 0)
};

// cases.RANGES
const std::vector<Variant>& ranges()
{
    static const std::vector<Variant> v = {
        {0, {R, R, 3}},
        {1, {R, R, 3, 1, 3}},
        {3, {R, R, R, R, 3, 5}},
        {4, {R, R, R, R, 5}},
        {5, {R, R, 3, 1, 3}},
        {6, {R, 3}},
        {7, {R, 3}},
        {8, std::vector<int>(16, 16)},
        {9, {R, R, R}},
        {10, {R, R, R}},
        {11, {R, R, R}},
        {12, {R}},
        {13, {R, R, 5, 2}},
        {14, {1, R}},
        {15, {1, R}},
        {16, {R}},
        {17, {R, R, R, R, R, R, R, R, 3, 4, 3}},
        {18, {R, 7}},
        {19, {R, 7, R, R, 3}},
        {20, {R, R, 2, 1}},
        {21, {R, R, 3, 1, 1, 1}},
        {23, {R, R, R, R, R, R, 3, 1, 5}},
        {24, {R, R, 5, 9}},
        {28, {R, R, R, 3, 3}},
        {28, {R, R, R, 3}},
        {29, std::vector<int>(8, 99)},
        {30, {R, 1, 5}},
        {31, {R, 1, 5}},
        {32, {R, 5}},
        {33, {3, R, 5}},
        {34, {5, R, R, 5}},
        {35, {R, 2}},
        {36, {R, R}},
        {37, {R, R, R, R}},
        {38, {R, 3}},
        {39, {R, 3}},
        {40, {R, R, 2, 1, 1}},
        {41, {99, 3, 99, 3, 99}},
        {42, {31}},
        {43, {R}},
        {44, {R}},
        {45, {R}},
    };
    return v;
}

std::vector<std::vector<int>> cases(int id, int nRandom = 40)
{
    std::vector<std::vector<int>> out;
    int vi = 0;
    for (const Variant& var : ranges()) {
        if (var.id != id) {
            continue;
        }
        Lcg rng{static_cast<std::uint32_t>(id * 7919 + vi * 131 + 1)};
        ++vi;
        const auto& mx = var.max;
        const std::size_t k = mx.size();
        std::vector<int> mid(k), rnd(k);
        for (std::size_t i = 0; i < k; ++i) {
            mid[i] = mx[i] / 2;
        }
        out.push_back(std::vector<int>(k, 0));
        out.push_back(mx);
        out.push_back(mid);
        for (std::size_t i = 0; i < k; ++i) {
            rnd[i] = rng.below(mx[i] + 1);
        }
        for (const auto* base : {&mid, &rnd}) {
            for (std::size_t i = 0; i < k; ++i) {
                const int step = mx[i] <= 31 ? 1 : 3;
                for (int v = 0; v <= mx[i]; v += step) {
                    auto t = *base;
                    t[i] = v;
                    out.push_back(t);
                }
            }
        }
        for (int n = 0; n < nRandom; ++n) {
            std::vector<int> t(k);
            for (std::size_t i = 0; i < k; ++i) {
                t[i] = rng.below(mx[i] + 1);
            }
            out.push_back(t);
        }
    }
    std::vector<std::vector<int>> res;
    for (auto& t : out) {
        bool dup = false;
        for (const auto& r : res) {
            if (r == t) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            res.push_back(std::move(t));
        }
    }
    return res;
}

// --- primitives in the emulator's text format --------------------------------------

const char* inkName(gr::Ink i)
{
    switch (i) {
    case gr::Ink::Frame: return "frame";
    case gr::Ink::Back: return "back";
    case gr::Ink::BackLine: return "backline";
    case gr::Ink::Border: return "border";
    case gr::Ink::Single: return "single";
    case gr::Ink::Base: return "base";
    case gr::Ink::Shade: return "shade";
    case gr::Ink::Fill: return "fill";
    case gr::Ink::EnvFill: return "envfill";
    case gr::Ink::Text: return "base"; // same colour as the base line
    }
    return "?";
}

std::string format(const gr::Op& o)
{
    char buf[160];
    const char* v = o.view ? "V" : "";
    switch (o.kind) {
    case gr::Op::Kind::Frame:
        if (o.view) {
            std::snprintf(buf, sizeof buf, "VFR %d %d %d %d %s", o.x0, o.y0, o.x1, o.y1, inkName(o.ink));
        } else {
            std::snprintf(buf, sizeof buf, "FRAME %d %d %d %d", o.x0, o.y0, o.x1, o.y1);
        }
        break;
    case gr::Op::Kind::Rect:
        std::snprintf(buf, sizeof buf, "%sR %d %d %d %d %s", v, o.x0, o.y0, o.x1, o.y1, inkName(o.ink));
        break;
    case gr::Op::Kind::Line:
        std::snprintf(buf, sizeof buf, "%sL %d %d %d %d %s", v, o.x0, o.y0, o.x1, o.y1, inkName(o.ink));
        break;
    case gr::Op::Kind::Text:
        std::snprintf(buf, sizeof buf, "%sT %d %d %s %s", v, o.x0, o.y0, inkName(o.ink), o.text.c_str());
        break;
    case gr::Op::Kind::FillUnder: std::snprintf(buf, sizeof buf, "FU %d %d", o.x1, o.y1); break;
    case gr::Op::Kind::FillEnvelope: std::snprintf(buf, sizeof buf, "FE %d %d", o.x1, o.y1); break;
    case gr::Op::Kind::Bitmap:
        std::snprintf(buf, sizeof buf, "BMP %d %d %d %d %d %d %d", o.resId, o.x0, o.y0, o.x1, o.y1, o.w, o.h);
        break;
    case gr::Op::Kind::Blit:
        std::snprintf(buf, sizeof buf, "BLIT %d %d %d %d %d %d", o.x0, o.y0, o.x1, o.y1, o.w, o.h);
        break;
    }
    return buf;
}

std::vector<std::string> lines(int id, const std::vector<int>& values)
{
    std::vector<std::uint8_t> v(values.begin(), values.end());
    std::vector<std::string> out;
    for (const gr::Op& o : gr::render(id, v).ops) {
        out.push_back(format(o));
    }
    return out;
}

std::uint64_t fnv1a(const std::string& s, std::uint64_t h)
{
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

// --- expected values (tools/graphs/golden.py) --------------------------------------

struct Golden {
    int id;
    int cases;
    std::uint64_t digest;
    std::vector<int> spot; // the all-mid case
    int spotOps;
    std::vector<const char*> head; // its first primitives
    std::vector<const char*> tail; // and its last ones
};

const Golden kGolden[] = {
    {0, 220, 0x2d90b07161540ae0ull, {63, 63, 1}, 0,
     {},
     {}},
    {1, 228, 0x4a74ccbd983736b5ull, {63, 63, 1, 0, 1}, 28,
     {"FRAME 0 0 45 24", "R 0 0 44 23 back", "L 1 20 43 20 backline", "L 2 4 12 19 single", "L 12 19 12 19 single", "L 12 19 12 18 single", "L 12 18 12 17 single", "L 12 17 12 15 single"},
     {"L 21 4 22 4 single", "L 22 4 42 4 single", "FE 43 22", "BLIT 1 1 1 1 43 22"}},
    {3, 397, 0xfe56594326576da2ull, {63, 63, 63, 63, 1, 2}, 49,
     {"FRAME 0 0 61 28", "R 0 0 60 27 back", "L 1 3 59 3 backline", "L 2 23 10 4 single", "L 18 14 18 14 single", "L 18 14 17 14 single", "L 17 14 17 14 single", "L 17 14 16 14 single"},
     {"L 50 15 50 15 single", "L 50 15 50 14 single", "FE 59 26", "BLIT 1 1 1 1 59 26"}},
    {4, 392, 0x37c70ee44e80293bull, {63, 63, 63, 63, 2}, 49,
     {"FRAME 0 0 61 28", "R 0 0 60 27 back", "L 1 3 59 3 backline", "L 2 23 10 4 single", "L 18 14 18 14 single", "L 18 14 17 14 single", "L 17 14 17 14 single", "L 17 14 16 14 single"},
     {"L 50 15 50 15 single", "L 50 15 50 14 single", "FE 59 26", "BLIT 1 1 1 1 59 26"}},
    {5, 228, 0x73e7f35b0678d34bull, {63, 63, 1, 0, 1}, 29,
     {"FRAME 0 0 45 24", "R 0 0 44 23 back", "L 1 20 43 20 backline", "L 2 4 7 19 single", "L 7 19 7 19 single", "L 7 19 37 19 base", "L 37 19 37 19 single", "L 37 19 37 18 single"},
     {"L 41 4 42 4 single", "L 42 4 42 4 single", "FE 43 22", "BLIT 1 1 1 1 43 22"}},
    {6, 124, 0x88aa7a5f5e17e54cull, {63, 1}, 28,
     {"FRAME 0 0 31 22", "R 0 0 30 21 back", "L 1 18 29 18 backline", "L 2 4 2 17 single", "L 2 17 2 17 single", "L 2 17 2 16 single", "L 2 16 2 15 single", "L 2 15 2 14 single"},
     {"L 14 4 15 4 single", "L 15 4 28 4 single", "FE 29 20", "BLIT 1 1 1 1 29 20"}},
    {7, 124, 0x64ad31cd9ca6fe16ull, {63, 1}, 9,
     {"FRAME 0 0 31 22", "R 0 0 30 21 back", "L 1 18 29 18 backline", "L 2 4 2 17 single", "L 2 17 15 17 single", "L 15 17 15 4 single", "L 15 4 28 4 single", "FE 29 20"},
     {"L 15 17 15 4 single", "L 15 4 28 4 single", "FE 29 20", "BLIT 1 1 1 1 29 20"}},
    {8, 556, 0x29289560885669faull, {8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8}, 19,
     {"FRAME 0 0 194 47", "R 0 0 192 45 back", "L 6 45 90 -1 single", "L 18 45 90 -1 single", "L 30 45 90 -1 single", "L 42 45 90 -1 single", "L 54 45 90 -1 single", "L 66 45 90 -1 single"},
     {"L 162 45 90 -1 single", "L 174 45 90 -1 single", "L 186 45 90 -1 single", "BLIT 1 1 0 0 192 45"}},
    {9, 298, 0x9dc9848b218f6f7bull, {63, 63, 63}, 28,
     {"FRAME 0 0 52 28", "R 0 0 50 26 back", "L 0 13 52 13 backline", "L -1 13 0 13 border", "L 0 13 2 13 border", "L 2 13 4 13 border", "L 4 13 5 13 border", "L 5 13 7 13 border"},
     {"L 47 13 49 13 border", "L 49 13 52 13 border", "FU 50 26", "BLIT 1 1 0 0 50 26"}},
    {10, 298, 0xf9ba6245f2e5b678ull, {63, 63, 63}, 27,
     {"FRAME 0 0 52 28", "R 0 0 50 26 back", "L 0 13 52 13 backline", "L -1 13 5 13 border", "L 5 13 8 13 border", "L 8 13 10 13 border", "L 10 13 12 13 border", "L 12 13 13 13 border"},
     {"L 42 13 45 13 border", "L 45 13 52 13 border", "FU 50 26", "BLIT 1 1 0 0 50 26"}},
    {11, 296, 0xa2e13aebafadea43ull, {63, 63, 63}, 6,
     {"FRAME 0 0 56 24", "R 0 0 54 22 back", "L 1 11 54 11 backline", "L 27 11 -32 11 single", "L 27 11 82 11 single", "BLIT 1 1 0 0 54 22"},
     {}},
    {12, 68, 0x5e8a1be31cc071adull, {63}, 11,
     {"FRAME 0 0 34 22", "R 0 0 32 20 back", "L -17 19 -1 3 single", "L -1 3 11 3 single", "L 11 3 21 19 single", "L 11 19 21 3 single", "L 21 3 33 3 single", "L 33 3 49 19 single"},
     {"L 33 3 49 19 single", "L 5 1 5 19 backline", "L 27 1 27 19 backline", "BLIT 1 1 0 0 32 20"}},
    {13, 227, 0xec1c3808c3d4353bull, {63, 63, 2, 1}, 65,
     {"FRAME 0 0 52 28", "R 0 0 50 26 back", "L 0 13 52 13 backline", "L -13 13 -8 13 border", "L -8 13 -4 13 border", "L -4 13 0 13 border", "L 0 13 3 13 border", "L 3 13 6 13 border"},
     {"L 55 13 59 13 border", "L 59 13 63 13 border", "FU 50 26", "BLIT 1 1 0 0 50 26"}},
    {14, 112, 0xa69735add88406d4ull, {0, 63}, 8,
     {"FRAME 0 0 34 22", "R 0 0 32 20 back", "L 0 10 34 10 backline", "L 16 0 16 22 backline", "L 34 5 25 5 single", "L 25 5 8 15 single", "L 8 15 -1 21 single", "BLIT 1 1 0 0 32 20"},
     {}},
    {15, 114, 0xc4d855270826dbdeull, {0, 63}, 27,
     {"FRAME 0 0 34 22", "R 0 0 32 20 back", "L 0 10 32 10 backline", "L 16 0 16 20 backline", "L -2 20 8 20 single", "L 8 20 9 20 single", "L 9 20 10 20 single", "L 10 20 10 19 single"},
     {"L 22 1 23 1 single", "L 23 1 24 0 single", "L 24 0 34 0 single", "BLIT 1 1 0 0 32 20"}},
    {16, 68, 0x02f6301224f73e5full, {63}, 23,
     {"FRAME 0 0 34 22", "R 0 0 32 20 back", "L 0 10 32 10 backline", "L 16 0 16 20 backline", "L 16 10 17 0 single", "L 16 9 14 19 single", "L 17 0 19 10 single", "L 14 19 12 9 single"},
     {"L 3 0 2 9 single", "L 29 10 31 1 single", "L 2 9 0 18 single", "BLIT 1 1 0 0 32 20"}},
    {17, 742, 0xaec0bd8bd79b4dd6ull, {63, 63, 63, 63, 63, 63, 63, 63, 1, 2, 1}, 10,
     {"FRAME 0 0 84 28", "R 0 0 83 27 back", "L 1 3 82 3 backline", "L 2 14 10 14 single", "L 10 14 18 14 single", "L 18 14 65 14 base", "L 65 14 73 14 single", "L 73 14 81 14 single"},
     {"L 65 14 73 14 single", "L 73 14 81 14 single", "FE 82 26", "BLIT 1 1 1 1 82 26"}},
    {18, 134, 0xc26d77b457ccb76cull, {63, 3}, 35,
     {"FRAME 0 0 35 22", "R 0 0 33 20 back", "L 0 10 0 10 single", "L 0 10 1 7 single", "L 1 7 2 7 single", "L 2 7 3 7 single", "L 3 7 4 7 single", "L 4 7 5 7 single"},
     {"L 28 12 29 12 single", "L 29 12 30 12 single", "L 30 12 31 10 single", "BLIT 1 1 0 0 33 20"}},
    {19, 317, 0xbddb540839949b86ull, {63, 3, 63, 63, 1}, 7,
     {"FRAME 0 0 35 20", "R 0 0 33 18 back", "L 0 14 0 1 single", "L 0 1 15 1 single", "L 15 1 15 14 single", "L 15 14 31 14 single", "BLIT 1 1 0 0 33 18"},
     {}},
    {20, 220, 0x1e11d8783e3eb865ull, {63, 63, 1, 0}, 7,
     {"FRAME 0 0 52 28", "R 0 0 50 26 back", "L 0 10 53 10 backline", "L 0 10 53 10 border", "FU 50 26", "T 39 7 base 18", "BLIT 1 1 0 0 50 26"},
     {}},
    {21, 225, 0xc04f9cbb7e7e6dd8ull, {63, 63, 1, 0, 0, 0}, 7,
     {"FRAME 0 0 52 28", "R 0 0 50 26 back", "L 0 10 53 10 backline", "L 0 10 53 10 border", "FU 50 26", "T 39 7 base 12", "BLIT 1 1 0 0 50 26"},
     {}},
    {23, 569, 0xe4fd5f3593b32087ull, {63, 63, 63, 63, 63, 63, 1, 0, 2}, 50,
     {"FRAME 0 0 84 28", "R 0 0 83 27 back", "L 1 3 82 3 backline", "L 2 23 10 4 single", "L 18 14 18 14 single", "L 18 14 17 14 single", "L 17 14 17 14 single", "L 17 14 16 14 single"},
     {"L 73 15 73 15 single", "L 73 15 73 14 single", "FE 82 26", "BLIT 1 1 1 1 82 26"}},
    {24, 241, 0x65c88799797b44d4ull, {63, 63, 2, 4}, 10,
     {"FRAME 0 0 35 20", "R 0 0 33 18 back", "L 0 1 32 1 backline", "L -14 8 -5 1 single", "L -5 1 9 16 single", "L 9 16 17 8 single", "L 17 8 25 1 single", "L 25 1 40 16 single"},
     {"L 17 8 25 1 single", "L 25 1 40 16 single", "L 40 16 48 8 single", "BLIT 1 1 0 0 33 18"}},
    {28, 612, 0xa77bd0fad7e5fd7dull, {63, 63, 63, 1, 1}, 29,
     {"FRAME 0 0 58 28", "R 0 0 57 27 back", "L 1 24 56 24 backline", "L 2 4 11 23 single", "L 11 23 20 23 single", "L 20 23 20 23 single", "L 20 23 20 22 single", "L 20 22 20 20 single"},
     {"L 28 4 29 4 single", "L 29 4 55 4 single", "FE 56 26", "BLIT 1 1 1 1 56 26"}},
    {29, 586, 0x94b03387a86e79d2ull, {49, 49, 49, 49, 49, 49, 49, 49}, 10,
     {"FRAME 0 0 85 28", "R 0 0 84 27 back", "L 1 24 83 24 backline", "L 2 21 2 21 single", "L 2 21 2 21 single", "L 2 21 2 21 single", "L 2 21 82 21 base", "L 82 21 82 21 single"},
     {"L 2 21 82 21 base", "L 82 21 82 21 single", "FE 83 26", "BLIT 1 1 1 1 83 26"}},
    {30, 133, 0xbedaf4550389ed6dull, {63, 0, 2}, 7,
     {"FRAME 0 0 32 22", "R 0 0 30 20 back", "L 0 6 32 6 backline", "L 0 6 32 6 border", "FU 30 20", "T 19 7 base 18", "BLIT 1 1 0 0 30 20"},
     {}},
    {31, 136, 0xf356c65122bd5c0dull, {63, 0, 2}, 7,
     {"FRAME 0 0 32 22", "R 0 0 30 20 back", "L 0 6 32 6 backline", "L 0 6 32 6 border", "FU 30 20", "T 19 7 base 18", "BLIT 1 1 0 0 30 20"},
     {}},
    {32, 131, 0x043e4093ad3dd907ull, {63, 2}, 38,
     {"FRAME 0 0 38 22", "R 0 0 36 20 back", "L 0 10 0 10 single", "L 0 10 1 7 single", "L 1 7 2 4 single", "L 2 4 3 4 single", "L 3 4 4 3 single", "L 4 3 5 4 single"},
     {"L 31 15 32 15 single", "L 32 15 33 12 single", "L 33 12 34 10 single", "BLIT 1 1 0 0 36 20"}},
    {33, 140, 0xd40627cc98e30a68ull, {1, 63, 2}, 10,
     {"FRAME 0 0 52 28", "R 0 0 50 26 back", "L 0 1 49 1 backline", "L -23 12 -10 1 single", "L -10 1 13 24 single", "L 13 24 25 12 single", "L 25 12 37 1 single", "L 37 1 61 24 single"},
     {"L 25 12 37 1 single", "L 37 1 61 24 single", "L 61 24 73 12 single", "BLIT 1 1 0 0 50 26"}},
    {34, 233, 0x60d256438b9bb6b6ull, {2, 63, 63, 2}, 51,
     {"FRAME 0 0 52 28", "R 0 0 50 26 back", "L 0 1 49 1 backline", "L 2 2 2 2 single", "L 2 2 3 3 single", "L 3 3 4 5 single", "L 4 5 5 7 single", "L 5 7 6 9 single"},
     {"L 45 8 46 6 single", "L 46 6 47 4 single", "L 47 4 48 2 single", "BLIT 1 1 0 0 50 26"}},
    {35, 116, 0xba1520be3ba232c7ull, {63, 1}, 105,
     {"FRAME 0 0 52 28", "R 0 0 50 26 back", "L 0 13 52 13 backline", "L -18 13 -15 13 border", "L -15 13 -12 13 border", "L -12 13 -9 13 border", "L -9 13 -5 13 border", "L -5 13 -2 13 border"},
     {"L 63 13 66 13 border", "L 66 13 68 13 border", "FU 50 26", "BLIT 1 1 0 0 50 26"}},
    {36, 213, 0x814b0bccee708471ull, {63, 63}, 28,
     {"FRAME 0 0 52 28", "R 0 0 50 26 back", "L 0 13 52 13 backline", "L -1 13 0 13 border", "L 0 13 2 13 border", "L 2 13 4 13 border", "L 4 13 5 13 border", "L 5 13 7 13 border"},
     {"L 47 13 49 13 border", "L 49 13 52 13 border", "FU 50 26", "BLIT 1 1 0 0 50 26"}},
    {37, 382, 0x04c964ffa1a1ce67ull, {63, 63, 63, 63}, 49,
     {"FRAME 0 0 52 28", "R 0 0 50 26 back", "L 0 13 52 13 backline", "L -1 13 2 13 border", "L 2 13 3 13 border", "L 3 13 4 13 border", "L 4 13 5 13 border", "L 5 13 6 13 border"},
     {"L 46 13 47 13 border", "L 47 13 52 13 border", "FU 50 26", "BLIT 1 1 0 0 50 26"}},
    {38, 85, 0x30623e1463c48672ull, {63, 1}, 35,
     {"FRAME 0 0 34 22", "R 0 0 32 20 back", "L 0 10 32 10 backline", "L 16 0 16 20 backline", "L 0 21 1 21 single", "L 1 21 2 21 single", "L 2 21 3 21 single", "L 3 21 4 21 single"},
     {"L 27 0 28 -1 single", "L 28 -1 29 -1 single", "L 29 -1 30 -1 single", "BLIT 1 1 0 0 32 20"}},
    {39, 90, 0xb860e15299d9cb67ull, {63, 1}, 35,
     {"FRAME 0 0 34 22", "R 0 0 32 20 back", "L 0 10 32 10 backline", "L 16 0 16 20 backline", "L 0 17 1 17 single", "L 1 17 2 16 single", "L 2 16 3 16 single", "L 3 16 4 16 single"},
     {"L 27 4 28 3 single", "L 28 3 29 3 single", "L 29 3 30 3 single", "BLIT 1 1 0 0 32 20"}},
    {40, 222, 0xd911d294c9d7628dull, {63, 63, 1, 0, 0}, 7,
     {"FRAME 0 0 52 28", "R 0 0 50 26 back", "L 0 10 53 10 backline", "L 0 10 53 10 border", "FU 50 26", "T 39 7 base 12", "BLIT 1 1 0 0 50 26"},
     {}},
    {41, 260, 0xdd91d433117970b2ull, {49, 1, 49, 1, 49}, 46,
     {"FRAME 0 0 63 24", "R 0 0 61 22 back", "L 1 11 61 11 backline", "L -36 35 -35 35 single", "L -35 35 -35 33 single", "L -35 33 -34 31 single", "L -34 31 -33 29 single", "L -33 29 -32 28 single"},
     {"L 90 29 91 31 single", "L 91 31 92 33 single", "L 92 33 92 35 single", "BLIT 1 1 0 0 61 22"}},
    {42, 32, 0xedba6da31a7d67f3ull, {15}, 2,
     {"FRAME 0 0 139 67", "BMP 993 1 1 0 990 137 65"},
     {}},
    {43, 71, 0x0911cdfdde449735ull, {63}, 27,
     {"FRAME 0 0 31 22", "R 0 0 29 20 back", "L 0 18 31 18 backline", "L -1 18 1 18 border", "L 1 18 3 18 border", "L 3 18 4 18 border", "L 4 18 5 18 border", "L 5 18 6 17 border"},
     {"L 24 18 26 18 border", "L 26 18 31 18 border", "FU 29 20", "BLIT 1 1 0 0 29 20"}},
    {44, 70, 0x688162dadd283b32ull, {63}, 18,
     {"FRAME 0 0 35 22", "VR 1 1 34 21 back", "VFR 1 11 4 21 single", "VFR 3 11 6 21 single", "VFR 5 10 8 21 single", "VFR 7 10 10 21 single", "VFR 9 10 12 21 single", "VFR 11 10 14 21 single"},
     {"VFR 25 10 28 21 single", "VFR 27 10 30 21 single", "VFR 29 11 32 21 single", "VFR 31 11 34 21 single"}},
    {45, 68, 0x383ca9d089a74e1cull, {63}, 8,
     {"FRAME 0 0 21 22", "VR 1 1 20 21 back", "VL 1 20 19 20 single", "VL 5 2 5 20 single", "VL 7 2 7 20 single", "VL 9 2 9 20 single", "VL 11 2 11 20 single", "VL 17 2 17 20 single"},
     {}},
};

const Golden& golden(int id)
{
    for (const Golden& g : kGolden) {
        if (g.id == id) {
            return g;
        }
    }
    FAIL("no golden entry for graph id " << id);
    return kGolden[0];
}

void checkGraph(int id)
{
    const Golden& g = golden(id);
    INFO("graph id " << id << " (" << gr::className(id) << ")");

    // Spot check: the all-mid case.
    const auto spot = lines(id, g.spot);
    CHECK(static_cast<int>(spot.size()) == g.spotOps);
    for (std::size_t i = 0; i < g.head.size() && i < spot.size(); ++i) {
        CHECK(spot[i] == g.head[i]);
    }
    for (std::size_t i = 0; i < g.tail.size() && i < spot.size(); ++i) {
        CHECK(spot[spot.size() - g.tail.size() + i] == g.tail[i]);
    }

    // Every case: digest of all primitives.
    const auto all = cases(id);
    REQUIRE(static_cast<int>(all.size()) == g.cases);
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const auto& t : all) {
        std::string head = std::to_string(id);
        for (int x : t) {
            head += ' ' + std::to_string(x);
        }
        h = fnv1a(head + '\n', h);
        for (const auto& ln : lines(id, t)) {
            h = fnv1a(ln + '\n', h);
        }
    }
    CHECK(h == g.digest);

    // The vector form stays inside the view.
    const auto size = gr::defaultSize(id);
    for (const auto& t : {all.front(), all.back(), g.spot}) {
        std::vector<std::uint8_t> v(t.begin(), t.end());
        for (const auto& p : gr::draw(id, v)) {
            for (auto [x, y] : p.points) {
                CHECK(x >= 0.0f);
                CHECK(y >= 0.0f);
                CHECK(x <= static_cast<float>(size.first));
                CHECK(y <= static_cast<float>(size.second));
            }
        }
    }
}

} // namespace

TEST_CASE("graphs: dispatch table", "[graphs]")
{
    CHECK(std::string(gr::className(20)) == "CPnlClassicFilterGraph");
    CHECK(std::string(gr::className(42)) == "CPnlDXRouterGraph");
    CHECK(gr::defaultSize(3) == std::pair<int, int>{61, 28});
    CHECK(gr::defaultSize(8) == std::pair<int, int>{194, 47});
    CHECK(gr::defaultSize(42) == std::pair<int, int>{139, 67});
    // Interactive controls and the bare CPnlGraphABC draw nothing.
    for (int id : {0, 2, 22, 25, 26, 27, 46, 47, -1}) {
        CHECK_FALSE(gr::isGraph(id));
        CHECK(gr::render(id, {}).ops.empty());
    }
    CHECK(gr::isGraph(1));
    CHECK(gr::isGraph(45));
}

TEST_CASE("graphs: id 0 (CPnlGraphABC, LfoD) draws nothing", "[graphs]") { checkGraph(0); }
TEST_CASE("graphs: id 1 (CPnlADEnvGraph, EnvADR)", "[graphs]") { checkGraph(1); }
TEST_CASE("graphs: id 3 (CPnlADSRGraph, EnvADSR)", "[graphs]") { checkGraph(3); }
TEST_CASE("graphs: id 4 (CPnlModEnvGraph, ModADSR)", "[graphs]") { checkGraph(4); }
TEST_CASE("graphs: id 5 (CPnlAREnvGraph, AR-Env)", "[graphs]") { checkGraph(5); }
TEST_CASE("graphs: id 6 (CPnlDEnvGraph, EnvD)", "[graphs]") { checkGraph(6); }
TEST_CASE("graphs: id 7 (CPnlHEnvGraph, EnvH)", "[graphs]") { checkGraph(7); }
TEST_CASE("graphs: id 8 (CPnlVocoderGraph, Vocoder)", "[graphs]") { checkGraph(8); }
TEST_CASE("graphs: id 9 (CPnlEqHiLoGraph, ShelvEQ)", "[graphs]") { checkGraph(9); }
TEST_CASE("graphs: id 10 (CPnlEqMidGraph, EqPeak)", "[graphs]") { checkGraph(10); }
TEST_CASE("graphs: id 11 (CPnlAmpProcGraph, LevScaler)", "[graphs]") { checkGraph(11); }
TEST_CASE("graphs: id 12 (CPnlXMuxGraph, Mux8-1X)", "[graphs]") { checkGraph(12); }
TEST_CASE("graphs: id 13 (CPnlPhaserGraph, FltPhase)", "[graphs]") { checkGraph(13); }
TEST_CASE("graphs: id 14 (CPnlDistAGraph, Clip)", "[graphs]") { checkGraph(14); }
TEST_CASE("graphs: id 15 (CPnlDistBGraph, Overdrive)", "[graphs]") { checkGraph(15); }
TEST_CASE("graphs: id 16 (CPnlWrapGraph, WaveWrap)", "[graphs]") { checkGraph(16); }
TEST_CASE("graphs: id 17 (CPnlMultiEnvGraph, EnvMulti)", "[graphs]") { checkGraph(17); }
TEST_CASE("graphs: id 18 (CPnlOscCGraph, OscShpB)", "[graphs]") { checkGraph(18); }
TEST_CASE("graphs: id 19 (CPnlPulseOscGraph, PulseOsc)", "[graphs]") { checkGraph(19); }
TEST_CASE("graphs: id 20 (CPnlClassicFilterGraph, FltClassic)", "[graphs]") { checkGraph(20); }
TEST_CASE("graphs: id 21 (CPnlNormalFilterGraph, FltNord)", "[graphs]") { checkGraph(21); }
TEST_CASE("graphs: id 23 (CPnlADBDSREnvGraph, EnvADDSR)", "[graphs]") { checkGraph(23); }
TEST_CASE("graphs: id 24 (CPnlLfoGraph, no panel)", "[graphs]") { checkGraph(24); }
TEST_CASE("graphs: id 28 (CPnlAHDEnvGraph, EnvAHD / ModAHD)", "[graphs]") { checkGraph(28); }
TEST_CASE("graphs: id 29 (CPnlDXEnvGraph, Operator / EnvDX)", "[graphs]") { checkGraph(29); }
TEST_CASE("graphs: id 30 (CPnlSmallFilterGraph, FltLP)", "[graphs]") { checkGraph(30); }
TEST_CASE("graphs: id 31 (CPnlSmallFilterGraph, FltHP)", "[graphs]") { checkGraph(31); }
TEST_CASE("graphs: id 32 (CPnlOscSinShapeGraph, OscShpA)", "[graphs]") { checkGraph(32); }
TEST_CASE("graphs: id 33 (CPnlLfoBGraph, LfoB)", "[graphs]") { checkGraph(33); }
TEST_CASE("graphs: id 34 (CPnlLfoCGraph, LfoShpA)", "[graphs]") { checkGraph(34); }
TEST_CASE("graphs: id 35 (CPnlCombFltGraph, FltComb)", "[graphs]") { checkGraph(35); }
TEST_CASE("graphs: id 36 (CPnlEqHiLoGraph, Eq2Band)", "[graphs]") { checkGraph(36); }
TEST_CASE("graphs: id 37 (CPnlEqHiMidLoGraph, Eq3band)", "[graphs]") { checkGraph(37); }
TEST_CASE("graphs: id 38 (CPnlShapeBGraph, ShpExp)", "[graphs]") { checkGraph(38); }
TEST_CASE("graphs: id 39 (CPnlSaturateGraph, Saturate)", "[graphs]") { checkGraph(39); }
TEST_CASE("graphs: id 40 (CPnlStaticFilterGraph, FltStatic)", "[graphs]") { checkGraph(40); }
TEST_CASE("graphs: id 41 (CPnlOperatorGraph, Operator)", "[graphs]") { checkGraph(41); }
TEST_CASE("graphs: id 42 (CPnlDXRouterGraph, DXRouter)", "[graphs]")
{
    checkGraph(42);
    // Algorithm n is the 137x65 cell at y = 66 n of bitmap 993.
    const std::uint8_t algo[] = {31};
    const auto d = gr::render(42, algo);
    REQUIRE(d.ops.size() == 2);
    CHECK(d.ops[1].kind == gr::Op::Kind::Bitmap);
    CHECK(d.ops[1].resId == 993);
    CHECK(d.ops[1].y1 == 31 * 66);
}
TEST_CASE("graphs: id 43 (CPnlTunedNoiseGraph, OscNoise)", "[graphs]") { checkGraph(43); }
TEST_CASE("graphs: id 44 (CPnlRndDistributionGraph, no panel)", "[graphs]") { checkGraph(44); }
TEST_CASE("graphs: id 45 (CPnlRndTrigGraph, RndTrig)", "[graphs]") { checkGraph(45); }

TEST_CASE("graphs: rasterize and vector form", "[graphs]")
{
    // FltClassic at Freq 64, Res 0, 12 dB, on: the curve's fill and the slope text.
    const std::uint8_t v[] = {64, 0, 0, 1};
    const auto d = gr::render(20, v);
    const auto px = gr::rasterize(d);
    REQUIRE(px.size() == static_cast<std::size_t>(d.width * d.height));
    CHECK(px[0] == gr::Ink::Frame);
    bool hasFill = false;
    for (auto p : px) {
        hasFill = hasFill || p == gr::Ink::Fill;
    }
    CHECK(hasFill);
    bool text = false;
    for (const auto& o : d.ops) {
        if (o.kind == gr::Op::Kind::Text) {
            text = true;
            CHECK(o.text == "12");
        }
    }
    CHECK(text);
    const auto polys = gr::draw(20, v);
    REQUIRE(!polys.empty());
    CHECK(polys.front().ink == gr::Ink::Back);
    CHECK(polys.front().filled);
    CHECK(polys.back().ink == gr::Ink::Frame);
}
