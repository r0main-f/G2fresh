// Module graphs, ported from the original Clavia G2 editor (Mac v1.62).
// See core/include/g2/graphs.hpp and re/notes/module-graphs.md.
//
// Every graph is ported from the x86 code of its class (UpdateGraphics() +
// Draw()), keeping the original's mix of float / double arithmetic and its
// truncations, so that the integer coordinates match the original exactly.
// The expected output was produced by running the original code in a CPU
// emulator (tools/graphs/emulate.py), see tests/test_graphs.cpp.
#include "g2/graphs.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The original (SSE, no FMA) rounds every product; a fused multiply-add
// changes the last bit and can move a point by one pixel.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace g2::graphs {

namespace {

using Values = std::span<const std::uint8_t>;
using i16 = std::int16_t;
using u8 = std::uint8_t;

constexpr double kPi = 3.141592653589793;

// Value conversions with the x86 semantics of the original.
// cvttsd2si / cvttss2si: truncation, 0x80000000 for NaN / out of range.
inline int trunc(double v)
{
    if (!(v > -2147483649.0 && v < 2147483648.0)) {
        return INT_MIN;
    }
    return static_cast<int>(v);
}
inline int trunc(float v) { return trunc(static_cast<double>(v)); }
// Passing an int as a `short` argument (or storing it in a short field).
inline i16 s16(int v) { return static_cast<i16>(static_cast<std::uint16_t>(static_cast<unsigned>(v) & 0xffffu)); }
inline u8 b8(int v) { return static_cast<u8>(static_cast<unsigned>(v) & 0xffu); }
inline float f32(double v) { return static_cast<float>(v); }

// libm as used by the original, all double precision.
inline double dsin(double x) { return std::sin(x); }
inline double dcos(double x) { return std::cos(x); }
inline double datan(double x) { return std::atan(x); }
inline double dfloor(double x) { return std::floor(x); }
inline double dceil(double x) { return std::ceil(x); }
inline float fceil(float x) { return std::ceil(x); }
// __powidf2 (x^n by repeated squaring, as in libgcc).
inline double powi(double x, int n)
{
    bool neg = n < 0;
    unsigned m = neg ? 0u - static_cast<unsigned>(n) : static_cast<unsigned>(n);
    double r = (m & 1u) ? x : 1.0;
    while ((m >>= 1) != 0) {
        x = x * x;
        if (m & 1u) {
            r = r * x;
        }
    }
    return neg ? 1.0 / r : r;
}

// Dependency value i (CPnlCustom::GetDependencyValue); 0 when missing.
inline u8 dep(Values v, std::size_t i) { return i < v.size() ? v[i] : 0; }

struct Pt {
    i16 x = 0, y = 0;
};

// The drawing state of a CPnlGraphABC: pen position (+0x7c/+0x7e) and the
// shared scratch bitmap (recorded as Ops).
class Canvas {
public:
    Canvas(int w, int h, std::vector<Op>& ops) : W(w), H(h), ops_(ops) {}

    const int W, H; // CView::GetRect() == (0, 0, W, H)
    i16 penX = 0, penY = 0;

    // CGraphGUI::Draw(rect): frame of the view.
    void frame(int l, int t, int r, int b)
    {
        Op o;
        o.kind = Op::Kind::Frame;
        o.ink = Ink::Frame;
        o.x0 = l, o.y0 = t, o.x1 = r, o.y1 = b;
        ops_.push_back(std::move(o));
    }
    void frame() { frame(0, 0, W, H); }
    // DrawFrame(rect, colour) on the view's own canvas (FrameRect, r/b exclusive).
    void viewFrame(int l, int t, int r, int b, Ink ink)
    {
        frame(s16(l), s16(t), s16(r), s16(b));
        ops_.back().ink = ink;
        ops_.back().view = true;
    }
    // DrawRect into the scratch bitmap (viewRect: directly on the view).
    void rect(int l, int t, int r, int b, Ink ink, bool onView = false)
    {
        Op o;
        o.kind = Op::Kind::Rect;
        o.ink = ink;
        o.view = onView;
        o.x0 = s16(l), o.y0 = s16(t), o.x1 = s16(r), o.y1 = s16(b);
        ops_.push_back(std::move(o));
    }
    void viewRect(int l, int t, int r, int b, Ink ink) { rect(l, t, r, b, ink, true); }
    // CABCCanvas::DrawLine on the scratch bitmap (arguments are shorts);
    // viewLine: on the view's own canvas.
    void line(int x0, int y0, int x1, int y1, Ink ink, bool onView = false)
    {
        Op o;
        o.kind = Op::Kind::Line;
        o.ink = ink;
        o.view = onView;
        o.x0 = s16(x0), o.y0 = s16(y0), o.x1 = s16(x1), o.y1 = s16(y1);
        ops_.push_back(std::move(o));
    }
    void viewLine(int x0, int y0, int x1, int y1, Ink ink) { line(x0, y0, x1, y1, ink, true); }
    void moveTo(int x, int y)
    {
        penX = s16(x);
        penY = s16(y);
    }
    // LineTo / SingleLineTo / BorderLineTo / BackLineTo / BaseLineTo.
    void lineTo(int x, int y, Ink ink)
    {
        line(penX, penY, x, y, ink);
        moveTo(x, y);
    }
    void singleLineTo(int x, int y) { lineTo(x, y, Ink::Single); }
    void borderLineTo(int x, int y) { lineTo(x, y, Ink::Border); }
    void backLineTo(int x, int y) { lineTo(x, y, Ink::BackLine); }
    void baseLineTo(int x, int y) { lineTo(x, y, Ink::Base); }
    // DrawHorizontalLine / DrawVerticalLine.
    void hline(int x0, int y, int x1, Ink ink, bool onView = false) { line(x0, y, x1, y, ink, onView); }
    void vline(int x, int y0, int y1, Ink ink, bool onView = false) { line(x, y0, x, y1, ink, onView); }

    // DrawText(x, y, s, kGraphFilterTextFont, colour); GetTextWidth.
    static int textWidth(std::string_view s) { return kTextWidthPerChar * static_cast<int>(s.size()); }
    void text(int x, int y, std::string s, Ink ink = Ink::Text, bool onView = false)
    {
        Op o;
        o.kind = Op::Kind::Text;
        o.ink = ink;
        o.view = onView;
        o.x0 = s16(x), o.y0 = s16(y);
        o.x1 = o.x0 + textWidth(s);
        o.text = std::move(s);
        ops_.push_back(std::move(o));
    }
    void fillUnder(int w, int h) { fill(Op::Kind::FillUnder, w, h); }
    void fillEnvelope(int w, int h) { fill(Op::Kind::FillEnvelope, w, h); }
    // CABCCanvas::DrawBitmap(x, y, bitmap, srcX, srcY, w, h) on the view.
    void bitmap(int resId, int x, int y, int sx, int sy, int w, int h)
    {
        Op o;
        o.kind = Op::Kind::Bitmap;
        o.resId = resId;
        o.x0 = s16(x), o.y0 = s16(y), o.x1 = s16(sx), o.y1 = s16(sy), o.w = s16(w), o.h = s16(h);
        ops_.push_back(std::move(o));
    }
    // DrawBitmap(view, x, y, fScratchPad, 0, 0, w, h).
    // (sx, sy): source origin in the scratch bitmap (stored in x1, y1).
    void blit(int x, int y, int w, int h, int sx = 0, int sy = 0)
    {
        Op o;
        o.kind = Op::Kind::Blit;
        o.x0 = s16(x), o.y0 = s16(y), o.w = s16(w), o.h = s16(h);
        o.x1 = s16(sx), o.y1 = s16(sy);
        ops_.push_back(std::move(o));
    }

    // --- CPnlGraphABC curve helpers -------------------------------------------

    // CPnlGraphABC::Bernstein(t, i) for n = 3 (0x980da): powers in double,
    // 1 - t in float.
    static double bernstein(float t, int i)
    {
        static constexpr float kChoose[4] = {1.0f, 3.0f, 3.0f, 1.0f};
        double a = 1.0;
        for (int k = 0; k < i; ++k) {
            a *= static_cast<double>(t);
        }
        float omt = 1.0f - t;
        double b = 1.0;
        for (int k = 0; k < 3 - i; ++k) {
            b *= static_cast<double>(omt);
        }
        return a * static_cast<double>(kChoose[i]) * b;
    }
    // CPnlGraphABC::Bezier (0x9b4e6): cubic through p[0..3], 10 segments.
    void bezier(const Pt* p, Ink ink)
    {
        moveTo(p[0].x, p[0].y);
        float t = 0.1f;
        for (int step = 0; step < 9; ++step) {
            float fx = 0.0f, fy = 0.0f;
            for (int i = 0; i < 4; ++i) {
                double b = bernstein(t, i);
                fx = f32(static_cast<double>(fx) + static_cast<double>(p[i].x) * b);
                fy = f32(static_cast<double>(fy) + static_cast<double>(p[i].y) * b);
            }
            int x = trunc(static_cast<double>(fx) + 0.5);
            int y = trunc(static_cast<double>(fy) + 0.5);
            lineTo(x, y, ink);
            t = f32(static_cast<double>(t) + 0.1);
        }
        lineTo(p[3].x, p[3].y, ink);
    }
    // PolyBezier (0x9b6ae): pen = p[0]; segments p[3k .. 3k+3], k < n/3.
    void polyBezier(const Pt* p, int n, Ink ink)
    {
        moveTo(p[0].x, p[0].y);
        bezier(p, ink);
        int segs = ((n & 0xff) * 0xab >> 8 & 0xff) >> 1;
        for (int k = 1; k < segs; ++k) {
            bezier(p + 3 * k, ink);
        }
    }
    // PolyBezierTo (0x9b88a): first segment starts at the pen.
    void polyBezierTo(const Pt* p, int n, Ink ink)
    {
        Pt q[4] = {{penX, penY}, p[0], p[1], p[2]};
        bezier(q, ink);
        int segs = ((n & 0xff) * 0xab >> 8 & 0xff) >> 1;
        for (int k = 1; k < segs; ++k) {
            bezier(p + 3 * k - 1, ink);
        }
    }
    // BezierTo (0x9ba56).
    void bezierTo(const Pt* p, Ink ink)
    {
        Pt q[4] = {{penX, penY}, p[0], p[1], p[2]};
        bezier(q, ink);
    }
    void borderPolyBezier(const Pt* p, int n) { polyBezier(p, n, Ink::Border); }
    void singlePolyBezier(const Pt* p, int n) { polyBezier(p, n, Ink::Single); }
    void borderPolyBezierTo(const Pt* p, int n) { polyBezierTo(p, n, Ink::Border); }
    void singlePolyBezierTo(const Pt* p, int n) { polyBezierTo(p, n, Ink::Single); }
    void borderBezierTo(const Pt* p) { bezierTo(p, Ink::Border); }
    void singleBezierTo(const Pt* p) { bezierTo(p, Ink::Single); }
    void borderBezier(const Pt* p) { bezier(p, Ink::Border); }
    void singleBezier(const Pt* p) { bezier(p, Ink::Single); }

    // CPnlGraphABC::ArcTo (0x9daae), in Single ink. The ellipse is the box
    // (a0,a1)-(a2,a3); the arc runs between the angles of (a4,a5) and (a6,a7)
    // as seen from the centre; (a8,a9) is the end point. When the end point
    // lies "before" the pen, the arc is drawn from the end point and closed
    // with a line back to the pen, and the pen is left at the end point.
    void arcTo(double a0, double a1, double a2, double a3, double a4, double a5, double a6, double a7,
               double a8, double a9)
    {
        double cx = a0 + (a2 - a0) * 0.5;
        double cy = a3 + (a1 - a3) * 0.5;
        double rx = std::fabs(a2 - a0) * 0.5;
        double ry = 0.5 * std::fabs(a1 - a3);
        double px = penX, py = penY;
        auto angle = [&](double x, double y) {
            if (x > cx) {
                return datan((y - cy) / (x - cx));
            }
            if (cx > x) {
                return datan((y - cy) / (x - cx)) + kPi;
            }
            return y > cy ? 1.5707963267948966 : -1.5707963267948966;
        };
        double t1 = angle(a4, a5);
        double t2 = angle(a6, a7);
        double tp = angle(px, py);
        double te = angle(a8, a9);
        if (t1 < 0.0 || 0.0 > t2) {
            t1 += 2 * kPi;
            t2 += 2 * kPi;
        }
        double first = tp, second = te; // xmm2, xmm1
        if (t2 > t1) {
            t1 += 2 * kPi;
            first = te;
            second = tp;
        }
        bool reversed = first > second;
        int ex = trunc(a8), ey = trunc(a9);
        if (reversed) {
            moveTo(ex, ey);
        }
        double diff = t1 - t2;
        for (double t = t2; t1 > t; t = diff / 20.0 + t) {
            int y = trunc(dsin(t) * ry + cy + 0.5);
            int x = trunc(dcos(t) * rx + cx + 0.5);
            singleLineTo(s16(x), s16(y));
        }
        if (!reversed) {
            singleLineTo(s16(ex), s16(ey));
        } else {
            singleLineTo(s16(trunc(px)), s16(trunc(py)));
            moveTo(ex, ey);
        }
    }
    // CPnlGraphABC::ExpTo (0x9df9c): quarter ellipse from the pen to (x, y);
    // `flag` picks the bulge. Falls back to a straight line when x <= pen x
    // or y == pen y.
    void expTo(int xi, int yi, bool flag)
    {
        int x = s16(xi), y = s16(yi);
        int x0 = penX, y0 = penY;
        if (x <= x0 || y == y0) {
            singleLineTo(x, y);
            return;
        }
        int dx = s16(x - x0);
        int dy = std::abs(y0 - y);
        dy = s16(dy);
        double X = x, Y = y, X0 = x0, Y0 = y0;
        if (!flag) {
            if (y < y0) {
                arcTo(X0 - dx, Y - dy, X, Y0, X0, Y0, X, Y, X, Y);
            } else {
                arcTo(X0, Y, X + dx, Y0 - dy, X0, Y0, X, Y, X, Y);
            }
        } else {
            if (y < y0) {
                arcTo(X0, Y, X + dx, Y0 + dy, X, Y, X0, Y0, X, Y);
            } else {
                arcTo(X0 - dx, Y0, X, Y0 + 2 * dy, X, Y, X0, Y0, X, Y);
            }
        }
    }
    void logTo(int x, int y, bool flag) { expTo(x, y, !flag); }

    static constexpr int kTextWidthPerChar = 5; // Arial 9: digits are 5 px wide

private:
    void fill(Op::Kind k, int w, int h)
    {
        Op o;
        o.kind = k;
        o.ink = k == Op::Kind::FillUnder ? Ink::Fill : Ink::EnvFill;
        o.x1 = w & 0xffff;
        o.y1 = h & 0xffff;
        ops_.push_back(std::move(o));
    }
    std::vector<Op>& ops_;
};

// Graph ports: one explicit specialisation per Graph Func id. The primary
// template draws nothing (CPnlGraphABC without its own Draw()).
template <int Id>
void drawGraph(Canvas&, Values)
{
}

// --- AD / ADSR / AHD envelopes (ids 1, 3, 4, 5, 6, 7, 28) -------------------------

namespace env_a {

// Envelope OutType values 0..3 (Pos, PosInv, Neg, NegInv) and 0..5 for the
// ADSR family: even = curve rises from the bottom, odd = inverted (starts at
// the top). The graph's inner rectangle (the view shrunk by 1 px) is drawn
// into the scratch bitmap and copied with a source offset of (1, 1).

// The original copies the scratch bitmap from source (1, 1) for these graphs.
void blitFromOneOne(Canvas& c, int x, int y, int w, int h)
{
    c.blit(x, y, w, h, 1, 1);
}

// Fields of CPnlADGraphABC (constructor 0x9a680).
struct ADFields {
    u8 attack = 0;     // +0x84
    u8 decay = 0;      // +0x85
    int attackCurve = 0; // +0x88: 0 exp, 1 linear, 2 log (Shape LogExp)
    int decayLinear = 0; // +0x8c: 1 = straight decay line
    bool sustain = false; // +0x90: "AR" mode, decay is a release after a sustain line
    int outType = 0;   // +0x94
    int segments = 2;  // +0x98: number of time segments sharing the width
};

// CPnlADGraphABC::Draw (0xa6e78).
void drawAD(Canvas& c, const ADFields& f)
{
    c.frame();
    const int w = c.W - 2, h = c.H - 2; // GetRect().Shrink(1)
    c.rect(0, 0, w + 1, h + 1, Ink::Back);
    const float fh = static_cast<float>(h);
    const float fw = static_cast<float>(w);
    const float yBase = fh - 2.0f;
    if (f.outType >= 0 && f.outType <= 1) {
        c.moveTo(1, trunc(yBase));
        c.backLineTo(trunc(fw), s16(trunc(yBase)));
    } else if (f.outType >= 2 && f.outType <= 3) {
        c.moveTo(1, 3);
        c.backLineTo(trunc(fw), 3);
    }
    const float yBottom = yBase - 1.0f;
    const float xRight = fw - 1.0f;
    float scale = xRight - 2.0f;
    if (f.sustain) {
        scale -= 5.0f;
    }
    scale /= static_cast<float>(f.segments * 127);
    const int yb = s16(trunc(yBottom));

    const bool rising = f.outType == 0 || f.outType == 2;
    const bool falling = f.outType == 1 || f.outType == 3;
    if (!rising && !falling) {
        c.fillEnvelope(w, h);
        blitFromOneOne(c, 1, 1, w, h);
        return;
    }
    // y of the peak and of the rest level
    const int yPeak = rising ? 4 : yb;
    const int yRest = rising ? yb : 4;
    c.moveTo(2, yRest);
    const int xa = trunc(static_cast<double>(static_cast<float>(f.attack) * scale + 2.0f) + 0.5);
    const double dxa = xa;
    switch (f.attackCurve) {
    case 1:
        c.singleLineTo(trunc(dxa), yPeak);
        break;
    case 0:
        c.expTo(trunc(dxa), yPeak, !rising);
        break;
    case 2:
        c.expTo(trunc(dxa), yPeak, rising);
        break;
    default:
        break;
    }
    const float d = static_cast<float>(f.decay) * scale;
    const double dd = trunc(static_cast<double>(d) + 0.5);
    float xEnd = f32(static_cast<double>(static_cast<float>(dxa)) + dd);
    if (f.sustain) {
        const int xs = trunc(static_cast<double>(xRight) - dd);
        c.lineTo(s16(xs), yPeak, Ink::Base);
        xEnd = xRight;
    }
    if (f.decayLinear == 1) {
        c.singleLineTo(trunc(xEnd), yRest);
    } else {
        c.expTo(trunc(xEnd), yRest, !rising);
    }
    c.singleLineTo(trunc(xRight), yRest);
    c.fillEnvelope(w, h);
    blitFromOneOne(c, 1, 1, w, h);
}

// Envelope "Shape" (0..3: LogExp, LinExp, ExpExp, LinLin) -> attack curve
// and linear decay, as in CPnlADEnvGraph::UpdateGraphics (0x9602c). Other
// values leave the fields unchanged.
void applyShape(int shape, int& attackCurve, int& decayLinear)
{
    switch (shape) {
    case 0: attackCurve = 2, decayLinear = 0; break;
    case 1: attackCurve = 1, decayLinear = 0; break;
    case 2: attackCurve = 0, decayLinear = 0; break;
    case 3: attackCurve = 1, decayLinear = 1; break;
    default: break;
    }
}


// Fields of CPnlADSRGraphABC (constructor 0x9a970).
struct ADSRFields {
    u8 attack = 0, decay = 0, sustain = 0, release = 0; // +0x84..+0x87
    int attackCurve = 0; // +0x88: 0 exp, 1 linear, 2 log
    int decayCurve = 0;  // +0x8c: 0 curved, 1 linear, other: decay and release not drawn
    int outType = 0;     // +0x90: 0..5
};

// CPnlADSRGraphABC::Draw (0xa6656). The width holds 461 time units:
// attack + decay + release at most 3 x 127, plus 80 for the sustain segment.
void drawADSR(Canvas& c, const ADSRFields& f)
{
    c.frame();
    const int w = c.W - 2, h = c.H - 2;
    c.rect(0, 0, w + 1, h + 1, Ink::Back);
    const float fh = static_cast<float>(h);
    const float fw = static_cast<float>(w);
    const float yBase = fh - 2.0f;
    // back line: bottom, top, or the zero level of the bipolar types 4/5
    auto backLine = [&](int y) {
        c.moveTo(1, y);
        c.backLineTo(trunc(fw), y);
    };
    switch (f.outType) {
    case 0:
    case 1:
        backLine(s16(trunc(yBase)));
        break;
    case 2:
    case 3:
        backLine(3);
        break;
    case 4:
    case 5: {
        const float span = yBase - 3.0f;
        const float level = f.outType == 4 ? span + static_cast<float>(f.sustain) * span / -127.0f
                                           : static_cast<float>(f.sustain) * span / 127.0f;
        const int y = trunc(static_cast<float>(trunc(static_cast<double>(level) + 0.5)) + 3.0f);
        backLine(s16(y));
        break;
    }
    default:
        break;
    }
    const float yBottom = yBase - 1.0f;
    const float range = yBottom - 4.0f;
    const float vScale = range / 127.0f;
    const float xRight = fw - 1.0f;
    const float hScale = (xRight - 2.0f) / 461.0f;
    if (f.outType >= 0 && f.outType <= 5) {
        const bool rising = (f.outType & 1) == 0;
        const int yb = s16(trunc(yBottom));
        const int yPeak = rising ? 4 : yb;
        const int yRest = rising ? yb : 4;
        const double xa = trunc(static_cast<double>(static_cast<float>(f.attack) * hScale + 2.0f) + 0.5);
        c.moveTo(2, yRest);
        switch (f.attackCurve) {
        case 1: c.singleLineTo(trunc(xa), yPeak); break;
        case 0: c.expTo(trunc(xa), yPeak, !rising); break;
        case 2: c.expTo(trunc(xa), yPeak, rising); break;
        default: break;
        }
        const float sv = static_cast<float>(f.sustain) * vScale;
        const float sLevel = rising ? range - sv : sv;
        const double ys = static_cast<float>(trunc(static_cast<double>(sLevel) + 0.5)) + 4.0f;
        const double xdSum = static_cast<double>(static_cast<float>(f.decay) * hScale) + xa;
        const double xd = trunc(static_cast<double>(static_cast<float>(xdSum)) + 0.5);
        const int yS = s16(trunc(ys));
        if (f.decayCurve == 0) {
            c.expTo(trunc(xd), yS, !rising);
        } else if (f.decayCurve == 1) {
            c.singleLineTo(trunc(xd), yS);
        }
        if (xd < 2.0) {
            c.moveTo(trunc(xd + 1.0), yS);
        }
        const float r = static_cast<float>(f.release) * hScale;
        const double rr = trunc(static_cast<double>(r) + 0.5);
        const int xs = trunc(static_cast<double>(xRight) - rr);
        c.lineTo(s16(xs), yS, Ink::Base);
        if (f.decayCurve == 0) {
            c.expTo(trunc(xRight), yRest, !rising);
        } else if (f.decayCurve == 1) {
            c.singleLineTo(trunc(xRight), yRest);
        }
    }
    c.fillEnvelope(w, h);
    blitFromOneOne(c, 1, 1, w, h);
}

// Fields of CPnlAHDGraphABC (constructor 0x9a370).
struct AHDFields {
    u8 attack = 0, hold = 127, decay = 0; // +0x84..+0x86
    int attackCurve = 2; // +0x88: 0 exp, 1 linear, 2 log
    int decayCurve = 0;  // +0x8c: 0 curved, 1 linear, other: no decay
    int outType = 0;     // +0x90: 0..3
    int segments = 1;    // +0x94
};

// CPnlAHDGraphABC::Draw (0xa3c8c).
void drawAHD(Canvas& c, const AHDFields& f)
{
    c.frame();
    const int w = c.W - 2, h = c.H - 2;
    c.rect(0, 0, w + 1, h + 1, Ink::Back);
    const float fh = static_cast<float>(h);
    const float fw = static_cast<float>(w);
    const float yBase = fh - 2.0f;
    if (f.outType >= 0 && f.outType <= 1) {
        c.moveTo(1, trunc(yBase));
        c.backLineTo(trunc(fw), s16(trunc(yBase)));
    } else if (f.outType >= 2 && f.outType <= 3) {
        c.moveTo(1, 3);
        c.backLineTo(trunc(fw), 3);
    }
    const float yBottom = yBase - 1.0f;
    const float xRight = fw - 1.0f;
    const float scale = (xRight - 2.0f) / (static_cast<float>(f.segments) * 127.0f);
    if (f.outType >= 0 && f.outType <= 3) {
        const bool rising = (f.outType & 1) == 0;
        const int yb = s16(trunc(yBottom));
        const int yPeak = rising ? 4 : yb;
        const int yRest = rising ? yb : 4;
        const float a = scale * static_cast<float>(f.attack) + 2.0f;
        const double xa = trunc(static_cast<double>(a) + 0.5);
        c.moveTo(2, yRest);
        switch (f.attackCurve) {
        case 1: c.singleLineTo(trunc(xa), yPeak); break;
        case 0: c.expTo(trunc(xa), yPeak, !rising); break;
        case 2: c.expTo(trunc(xa), yPeak, rising); break;
        default: break;
        }
        const double xh = trunc(static_cast<double>(static_cast<float>(f.hold) * scale + a) + 0.5);
        c.singleLineTo(trunc(xh), yPeak);
        const float d = static_cast<float>(f.decay) * scale;
        const int xd = trunc(xh + static_cast<double>(trunc(static_cast<double>(d) + 0.5)));
        if (f.decayCurve == 0) {
            c.expTo(xd, yRest, !rising);
        } else if (f.decayCurve == 1) {
            c.singleLineTo(xd, yRest);
        }
        c.singleLineTo(trunc(xRight), yRest);
    }
    c.fillEnvelope(w, h);
    blitFromOneOne(c, 1, 1, w, h);
}

} // namespace env_a

// 1 CPnlADEnvGraph (EnvADR): attack, decay/release, shape, AD/AR, out type.
template <>
void drawGraph<1>(Canvas& c, Values v)
{
    env_a::ADFields f;
    f.segments = 2;
    f.attack = dep(v, 0);
    f.decay = dep(v, 1);
    env_a::applyShape(dep(v, 2), f.attackCurve, f.decayLinear);
    f.sustain = dep(v, 3) != 0;
    f.outType = dep(v, 4);
    env_a::drawAD(c, f);
}

// 6 CPnlDEnvGraph (EnvD): decay, out type; no attack, linear-attack jump.
template <>
void drawGraph<6>(Canvas& c, Values v)
{
    env_a::ADFields f;
    f.segments = 1;
    f.attack = 0;
    f.attackCurve = 1;
    f.decayLinear = 0;
    f.decay = dep(v, 0);
    f.outType = dep(v, 1);
    env_a::drawAD(c, f);
}

// 3 CPnlADSRGraph (EnvADSR): A, D, S, R, shape, out type.
template <>
void drawGraph<3>(Canvas& c, Values v)
{
    env_a::ADSRFields f;
    f.attack = dep(v, 0);
    f.decay = dep(v, 1);
    f.sustain = dep(v, 2);
    f.release = dep(v, 3);
    env_a::applyShape(dep(v, 4), f.attackCurve, f.decayCurve);
    f.outType = dep(v, 5);
    env_a::drawADSR(c, f);
}

// 4 CPnlModEnvGraph (ModADSR): A, D, S, R, out type; linear attack, curved decay.
template <>
void drawGraph<4>(Canvas& c, Values v)
{
    env_a::ADSRFields f;
    f.attackCurve = 1;
    f.decayCurve = 0;
    f.attack = dep(v, 0);
    f.decay = dep(v, 1);
    f.sustain = dep(v, 2);
    f.release = dep(v, 3);
    f.outType = dep(v, 4);
    env_a::drawADSR(c, f);
}

// 5 CPnlAREnvGraph (AR-Env panel, not used by any module): attack, release,
// attack curve and decay curve as raw values, out type; no decay, full sustain.
template <>
void drawGraph<5>(Canvas& c, Values v)
{
    env_a::ADSRFields f;
    f.decay = 0;
    f.sustain = 127;
    f.attack = dep(v, 0);
    f.release = dep(v, 1);
    f.attackCurve = dep(v, 2);
    f.decayCurve = dep(v, 3);
    f.outType = dep(v, 4);
    env_a::drawADSR(c, f);
}

// 7 CPnlHEnvGraph (EnvH): hold time, out type; a gate whose length is the hold time.
template <>
void drawGraph<7>(Canvas& c, Values v)
{
    env_a::AHDFields f;
    f.segments = 1;
    f.hold = dep(v, 0);
    f.outType = dep(v, 1);
    env_a::drawAHD(c, f);
}

// 28 CPnlAHDEnvGraph (EnvAHD: A, H, D, shape, out type; ModAHD: A, H, D,
// out type with a linear attack and curved decay). The shape is only read
// when there are at least 5 dependencies.
template <>
void drawGraph<28>(Canvas& c, Values v)
{
    env_a::AHDFields f;
    f.segments = 3;
    f.attackCurve = 1;
    f.attack = dep(v, 0);
    f.hold = dep(v, 1);
    f.decay = dep(v, 2);
    if (v.size() < 5) {
        f.outType = dep(v, 3);
    } else {
        env_a::applyShape(dep(v, 3), f.attackCurve, f.decayCurve);
        f.outType = dep(v, 4);
    }
    env_a::drawAHD(c, f);
}

// --- multi-segment envelopes, DX envelope and DX keyboard scaling --------------

namespace env_b {

// 1.0471^level, the DX output level curve used by CPnlDXEnvGraph (0..99 ->
// 0..95), truncated to an int and then to its low byte.
inline int dxLevel(int level)
{
    if (level == 0) {
        return 0;
    }
    return b8(trunc(powi(1.0471, level)));
}

} // namespace env_b

// 29 CPnlDXEnvGraph (UpdateGraphics 0x97554, Draw 0xa8a40): DX-style 4-rate /
// 4-level envelope. Values R1 L1 R2 L2 R3 L3 R4 L4 (0..99). Starts at L4,
// three ExpTo segments to L1, L2, L3, the sustain (Base ink) at L3, then the
// release ExpTo back to L4 at the right edge. Segment widths are proportional
// to |level change| / 99 * (99 - rate), 480 such units across the graph.
template <>
void drawGraph<29>(Canvas& c, Values v)
{
    const int r1 = dep(v, 0), l1 = dep(v, 1), r2 = dep(v, 2), l2 = dep(v, 3);
    const int r3 = dep(v, 4), l3 = dep(v, 5), r4 = dep(v, 6), l4 = dep(v, 7);
    c.frame();
    const int w = c.W - 2, h = c.H - 2; // shrunk view rect
    c.rect(0, 0, w + 1, h + 1, Ink::Back);
    const float hf = static_cast<float>(h), wf = static_cast<float>(w);
    const float base = hf - 2.0f;
    c.moveTo(1, s16(trunc(base)));
    c.backLineTo(s16(trunc(wf)), s16(trunc(base)));
    const float right = wf - 1.0f;
    const float bottom = base - 1.0f;
    const float xs = (right - 2.0f) / 480.0f;
    const float ys = (bottom - 4.0f) / 100.0f;
    auto yOf = [&](int level) { return trunc(bottom - static_cast<float>(env_b::dxLevel(level)) * ys); };
    auto segment = [&](int from, int to, int rate) {
        return static_cast<float>(std::abs(from - to)) / 99.0f * (static_cast<float>(99 - rate) * xs);
    };
    c.moveTo(2, yOf(l4));
    float x = segment(l4, l1, r1) + 2.0f;
    c.expTo(s16(trunc(x)), s16(yOf(l1)), false);
    x = segment(l1, l2, r2) + x;
    c.expTo(s16(trunc(x)), s16(yOf(l2)), false);
    x = segment(l2, l3, r3) + x;
    const i16 sustainY = s16(yOf(l3));
    c.expTo(s16(trunc(x)), sustainY, false);
    const i16 releaseX = s16(trunc(right - segment(l3, l4, r4)));
    c.lineTo(releaseX, sustainY, Ink::Base);
    c.expTo(s16(trunc(right)), s16(yOf(l4)), false);
    c.fillEnvelope(w, h);
    c.blit(1, 1, w, h, 1, 1);
}

// 41 CPnlOperatorGraph (UpdateGraphics 0x97646, Draw 0x9e930): DX keyboard
// level scaling. Values BreakP (0..99), L-Curve, L-Depth, R-Curve, R-Depth
// (curves 0 -Lin 1 -Exp 2 +Exp 3 +Lin, depths 0..99). A grey centre line; the
// left curve rises/falls from off-screen left to the break point, the right
// one from the break point to off-screen right. Depth/2 pixels per side.
template <>
void drawGraph<41>(Canvas& c, Values v)
{
    const int breakPoint = dep(v, 0), lCurve = dep(v, 1), lDepth = dep(v, 2);
    const int rCurve = dep(v, 3), rDepth = dep(v, 4);
    c.frame();
    const float W = static_cast<float>(c.W), H = static_cast<float>(c.H);
    c.rect(0, 0, c.W - 2, c.H - 2, Ink::Back);
    const float inner = W - 2.0f;
    const double lHalf = static_cast<double>(lDepth) * 0.5;
    const double rHalf = static_cast<double>(rDepth) * 0.5;
    const double bp = trunc(static_cast<double>(static_cast<float>(breakPoint) * inner / 99.0f) + 0.5);
    const double mid = static_cast<double>((H - 2.0f) * 0.5f);
    const i16 midY = s16(trunc(mid));
    c.moveTo(1, midY);
    c.backLineTo(s16(trunc(inner)), midY);
    const double w = static_cast<double>(W);
    // left side: starts one view width left of the break point
    c.moveTo(trunc(bp - w - 3.0), lCurve <= 1 ? trunc(lHalf + mid) : trunc(mid - lHalf));
    const i16 bx = s16(trunc(bp));
    if (lCurve == 0 || lCurve == 3) {
        c.singleLineTo(bx, midY);
    } else {
        c.expTo(bx, midY, lCurve == 1);
    }
    c.moveTo(bx, midY);
    // right side: ends one view width right of the break point
    const int ex = s16(trunc(bp + w - 1.0));
    switch (rCurve) {
    case 0: c.singleLineTo(ex, s16(trunc(rHalf + mid))); break;
    case 1: c.expTo(ex, s16(trunc(rHalf + mid)), true); break;
    case 2: c.expTo(ex, s16(trunc(mid - rHalf)), false); break;
    default: c.singleLineTo(ex, s16(trunc(mid - rHalf))); break;
    }
    c.blit(1, 1, c.W - 2, c.H - 2);
}

// 17 CPnlMultiEnvGraph (UpdateGraphics 0x96212, Draw 0xa745a): four-level
// envelope. Values L1 L2 L3 L4 T1 T2 T3 T4 SusPlac OutType Shape.
// Starts and ends at L4; segment k goes to level k (times scaled by 588
// units across), the sustain segment (Base ink) after level SusPlac+1 has the
// width left over by the four times (SusPlac 3 = "Trg": a line to the right
// edge after the last segment). OutType 1/3/5 draws upside down (0 at the
// top), and moves the zero line to the top (2, 3) or middle (4).
namespace env_b {

struct MultiEnv {
    int level[4] = {0, 0, 0, 0};         // +0x84..0x87
    int time[4] = {0, 29, 29, 29};       // +0x88..0x8b (ctor defaults)
    int susPlace = 3;                    // +0x8c
    unsigned outType = 0;                // +0x90
    int riseShape = 1, fallShape = 1;    // +0x94, +0x98: 0 exp, 1 line, 2 log

    explicit MultiEnv(Values v)
    {
        for (int i = 0; i < 4; ++i) {
            level[i] = dep(v, static_cast<std::size_t>(i));
            time[i] = dep(v, static_cast<std::size_t>(4 + i));
        }
        susPlace = dep(v, 8);
        outType = dep(v, 9);
        switch (dep(v, 10)) { // "Shape"
        case 0: riseShape = 2, fallShape = 0; break;
        case 1: riseShape = 1, fallShape = 0; break;
        case 2: riseShape = 0, fallShape = 0; break;
        case 3: riseShape = 1, fallShape = 1; break;
        default: break;
        }
    }
};

} // namespace env_b

template <>
void drawGraph<17>(Canvas& c, Values v)
{
    const env_b::MultiEnv e(v);
    c.frame();
    const int w = c.W - 2, h = c.H - 2;
    c.rect(0, 0, w + 1, h + 1, Ink::Back);
    const float hf = static_cast<float>(h), wf = static_cast<float>(w);
    const float base = hf - 2.0f;
    int zeroY = 0;
    bool zeroLine = true;
    switch (e.outType) {
    case 0:
    case 1: zeroY = s16(trunc(base)); break;
    case 2:
    case 3: zeroY = 3; break;
    case 4: zeroY = s16(trunc(static_cast<float>(trunc(static_cast<double>(base - 3.0f) + 0.5) / 2) + 3.0f)); break;
    default: zeroLine = false; break;
    }
    if (zeroLine) {
        c.moveTo(1, zeroY);
        c.backLineTo(s16(trunc(wf)), zeroY);
    }
    const bool inverted = e.outType < 6 && ((1u << e.outType) & 0x2au) != 0;
    const float right = wf - 1.0f;
    const float bottom = base - 1.0f;
    const float inner = right - 2.0f;
    const float xs = (inner - 5.0f) / 588.0f;
    const float ys = (bottom - 4.0f) / 127.0f;
    auto round = [](float f) { return trunc(static_cast<double>(f) + 0.5); };
    const int timeSum = e.time[0] + e.time[1] + e.time[2] + e.time[3];
    const double sustain = round(inner - static_cast<float>(timeSum) * xs);
    i16 x[5];
    x[0] = 2;
    int acc = 2;
    for (int i = 0; i < 4; ++i) {
        acc += round(static_cast<float>(e.time[i]) * xs);
        x[i + 1] = s16(acc);
    }
    std::uint16_t y[5];
    y[0] = static_cast<std::uint16_t>(round(static_cast<float>(e.level[3]) * ys));
    for (int i = 0; i < 3; ++i) {
        y[i + 1] = static_cast<std::uint16_t>(round(static_cast<float>(e.level[i]) * ys));
    }
    y[4] = y[0];
    auto toY = [&](std::uint16_t level) {
        return s16(trunc(inverted ? static_cast<float>(level) + 4.0f : bottom - static_cast<float>(level)));
    };
    c.moveTo(2, toY(y[0]));
    for (int i = 0; i < 4; ++i) {
        const std::uint16_t yn = y[i + 1];
        const int shape = yn > y[i] ? e.riseShape : e.fallShape;
        // exp/log as seen on screen: the bulge flips with the drawing direction
        switch (shape) {
        case 1: c.singleLineTo(x[i + 1], toY(yn)); break;
        case 0: c.expTo(x[i + 1], toY(yn), inverted); break;
        case 2:
            if (yn > y[i]) {
                c.expTo(x[i + 1], toY(yn), !inverted);
            }
            break;
        default: break;
        }
        if (e.susPlace == i) {
            if (e.susPlace == 3) {
                c.singleLineTo(s16(trunc(right)), toY(yn));
            } else {
                const i16 sy = s16(trunc(inverted ? static_cast<float>(yn) + 4.0f : bottom - static_cast<float>(yn)));
                const i16 sx = s16(trunc(static_cast<double>(static_cast<std::uint16_t>(x[i + 1])) + sustain));
                c.lineTo(sx, sy, Ink::Base);
                for (int k = i + 1; k < 5; ++k) {
                    x[k] = s16(trunc(static_cast<double>(static_cast<std::uint16_t>(x[k])) + sustain));
                }
            }
        }
    }
    c.fillEnvelope(w, h);
    c.blit(1, 1, w, h, 1, 1);
}

// 23 CPnlADBDSREnvGraph (UpdateGraphics 0x973f6, Draw 0xa7b94): attack,
// decay 1 to break 1, decay 2 to break 2, sustain, release. Values Attack
// Decay1 Break1 Decay2 Break2 Release Shape SusPlac OutType. Times are scaled
// by 588 units across the graph; the sustain segment (Base ink) after break
// 1 (SusPlac 0) or break 2 (SusPlac 1) fills the width left over. The
// release is a quarter ellipse (ArcTo) down to the base, or a straight line
// to the right edge when it is shorter than 3 px. OutType 1/3/5 draws upside
// down; OutType 4 (5 inverted) puts the zero line at the sustain level.
namespace env_b {

struct AdbdsrEnv {
    int attack = 0, decay1 = 0, break1 = 0, decay2 = 0, break2 = 0, release = 0; // +0x84..0x89
    int riseShape = 2, fallShape = 0; // +0x8c, +0x90: 0 exp, 1 line, 2 log
    int susPlace = 0;                 // +0x94
    unsigned outType = 0;             // +0x98
    // +0x9c is cleared by the constructor and never set; when non-zero the
    // release would always be a straight line.

    explicit AdbdsrEnv(Values v)
    {
        attack = dep(v, 0);
        decay1 = dep(v, 1);
        break1 = dep(v, 2);
        decay2 = dep(v, 3);
        break2 = dep(v, 4);
        release = dep(v, 5);
        switch (dep(v, 6)) { // "Shape"
        case 0: riseShape = 2, fallShape = 0; break;
        case 1: riseShape = 1, fallShape = 0; break;
        case 2: riseShape = 0, fallShape = 0; break;
        case 3: riseShape = 1, fallShape = 1; break;
        default: break;
        }
        susPlace = dep(v, 7);
        outType = dep(v, 8);
    }
};

} // namespace env_b

template <>
void drawGraph<23>(Canvas& c, Values v)
{
    const env_b::AdbdsrEnv e(v);
    c.frame();
    const int w = c.W - 2, h = c.H - 2;
    c.rect(0, 0, w + 1, h + 1, Ink::Back);
    const float hf = static_cast<float>(h), wf = static_cast<float>(w);
    const float base = hf - 2.0f;
    if (e.outType <= 1) {
        c.moveTo(1, s16(trunc(base)));
        c.backLineTo(s16(trunc(wf)), s16(trunc(base)));
    } else if (e.outType <= 3) {
        c.moveTo(1, 3);
        c.backLineTo(s16(trunc(wf)), 3);
    }
    const float right = wf - 1.0f;
    const float bottom = base - 1.0f;
    const float xs = (right - 2.0f) / 588.0f;
    const float ys = (bottom - 4.0f) / 127.0f;
    if (e.outType > 5) {
        c.fillEnvelope(w, h);
        c.blit(1, 1, w, h, 1, 1);
        return;
    }
    const bool inv = ((1u << e.outType) & 0x2au) != 0; // 1, 3, 5: upside down
    const unsigned zeroAtSustain = inv ? 5u : 4u;
    auto round = [](float f) { return trunc(static_cast<double>(f) + 0.5); };
    // y of a break level, as a double
    auto levelY = [&](int level) {
        const float l = static_cast<float>(round(static_cast<float>(level) * ys));
        return static_cast<double>(inv ? l + 4.0f : bottom - l);
    };
    // a curve segment: line, or ExpTo whose flag flips with the orientation
    auto segment = [&](int shape, int x, int y, bool flag) {
        if (shape == 1) {
            c.singleLineTo(s16(x), s16(y));
        } else if (shape == 0 || shape == 2) {
            c.expTo(s16(x), s16(y), flag != inv);
        }
    };
    // sustain at level y from the pen to sx (Base ink); OutType 4/5 first
    // draws the zero line at this level.
    auto sustainTo = [&](float sx, i16 sy, float penXNow) {
        if (e.outType == zeroAtSustain) {
            c.moveTo(1, sy);
            c.backLineTo(s16(trunc(right + 1.0f)), sy);
            c.moveTo(trunc(penXNow), sy);
        }
        c.lineTo(s16(trunc(sx)), sy, Ink::Base);
    };

    // attack
    const i16 lowY = s16(trunc(bottom));
    float x;
    if (!inv) {
        const double ax = round(static_cast<float>(e.attack) * xs + 2.0f);
        c.moveTo(2, lowY);
        segment(e.riseShape, trunc(ax), 4, e.riseShape == 2);
        x = f32(static_cast<double>(static_cast<float>(e.decay1) * xs) + ax);
    } else {
        const float ax = 2.0f + xs * static_cast<float>(e.attack);
        c.moveTo(2, 4);
        segment(e.riseShape, round(ax), lowY, e.riseShape == 2);
        x = ax + xs * static_cast<float>(e.decay1);
    }
    // decay 1
    const double y1 = levelY(e.break1);
    if (e.fallShape == 0 || e.fallShape == 1) {
        segment(e.fallShape, round(x), trunc(y1), false);
    }
    if (e.susPlace == 0) {
        const i16 sy = s16(trunc(y1));
        const int d2 = round(static_cast<float>(e.decay2) * xs);
        const int r = round(static_cast<float>(e.release) * xs);
        const float sx = f32(static_cast<double>(right) - static_cast<double>(d2 + r));
        sustainTo(sx, sy, x);
        x = sx;
    }
    // decay 2
    x = x + xs * static_cast<float>(e.decay2);
    const double y2 = levelY(e.break2);
    if (e.break2 > e.break1) {
        segment(e.riseShape, round(x), trunc(y2), e.riseShape == 2);
    } else if (e.fallShape == 0 || e.fallShape == 1) {
        segment(e.fallShape, round(x), trunc(y2), false);
    }
    const double rel = round(static_cast<float>(e.release) * xs);
    if (e.susPlace == 1) {
        sustainTo(f32(static_cast<double>(right) - rel), s16(trunc(y2)), x);
    }
    // release
    const double r = right;
    if (!inv) {
        const double b = bottom;
        if (rel > 2.0 && b > y2) {
            c.arcTo(r - rel, b, rel + r, y2 + y2 - b, 0.0, y2, r, 100.0, r, b);
        } else {
            c.singleLineTo(s16(trunc(right)), lowY);
        }
    } else {
        if (rel > 2.0 && y2 > 1.0) {
            c.arcTo(r - rel, 4.0, rel + r, y2 + y2 - 4.0, r, 4.0, 0.0, y2, r, 4.0);
        } else {
            c.singleLineTo(s16(trunc(right)), 4);
        }
    }
    c.fillEnvelope(w, h);
    c.blit(1, 1, w, h, 1, 1);
}

// --- filters: frequency responses (FilterGraphABC, SmallFilter, FeedbackFlt), vocoder

namespace filters {

// Fields of CPnlFilterGraphABC (+0x84..+0x8a), set by UpdateGraphics / ctor.
struct FilterFields {
    u8 gc = 0;       // +0x84 resonance peak enable (gain compensation / classic: 0)
    u8 res = 0;      // +0x85 resonance 0..127
    u8 freq = 0;     // +0x86 cutoff 0..127
    u8 type = 1;     // +0x87 1 LP, 2 BP, 3 HP, 4 BR
    u8 slope = 12;   // +0x88 dB/oct (also printed)
    u8 classic = 0;  // +0x89 LP knee variant (1 = classic)
    u8 on = 1;       // +0x8a
};

// CPnlFilterGraphABC::Draw (0xa50b0).
void drawFilter(Canvas& c, const FilterFields& f)
{
    c.frame();
    const float Hf = static_cast<float>(c.H), Wf = static_cast<float>(c.W);
    c.rect(0, 0, c.W - 2, c.H - 2, Ink::Back);

    const float h2 = Hf - 2.0f;
    const int mid = trunc(static_cast<double>(f32(static_cast<double>(h2) * 0.4)) + 0.5); // 0 line
    const double M = mid;
    const float yscale = f32(static_cast<double>(h2) * 0.0078125);
    const float xscale = f32(static_cast<double>(Wf - 2.0f) * 0.0078125);
    // resonance peak height
    const int R = trunc(static_cast<double>(f32(static_cast<double>(f.res) / 127.0 * static_cast<double>(f.gc) * M)) + 0.5);
    const double Rd = R;
    const float freqf = f32(static_cast<double>(f.freq) / 127.0 * 107.0 + 10.0);
    const float wp1 = Wf + 1.0f;
    const int right = s16(trunc(wp1));

    c.moveTo(0, mid);
    c.backLineTo(right, mid);
    if (f.on == 0) {
        c.moveTo(0, mid);
        c.borderLineTo(right, mid);
    } else {
        const int k = trunc(Wf - 3.0f);
        const int slope = f.slope;
        const int res12 = (f.res * 171) >> 11; // res / 12
        auto halfR = [&] { return trunc(static_cast<double>(f32(Rd * 0.5)) + 0.5); };
        Pt p[13];
        auto set = [&](int i, int x, int y) { p[i] = {s16(x), s16(y)}; };
        switch (f.type) {
        case 1: { // low pass
            const int hr = halfR();
            const double hrd = hr;
            const int y = trunc(hrd + M);
            c.moveTo(-1, y);
            const float fr = freqf - static_cast<float>(k);
            const int xc = trunc(static_cast<double>(fr * xscale) + 0.5);
            if (f.classic == 0) {
                c.borderLineTo(s16(xc - (slope >> 1) + 14), s16(y));
                set(0, xc - (slope >> 1) + 24, y);
                set(1, xc - (slope * 171 >> 9) + 20, y);
            } else {
                c.borderLineTo(s16(xc + 7), s16(y));
                set(0, xc + 15, y);
                set(1, xc + 14, y);
            }
            set(2, xc + 20, trunc(M - static_cast<double>(res12) + hrd));
            const int xc2 = trunc(static_cast<double>(f32(static_cast<double>(xscale) * static_cast<double>(fr))) + 0.5);
            set(3, xc2 + trunc(static_cast<double>(f32(slope * 0.25)) + 0.5) + 19, y);
            set(4, xc2 + 29, trunc(M + 5.0 + hrd));
            set(5, trunc(static_cast<float>(xc2) + (wp1 - static_cast<float>(slope))),
                trunc(static_cast<float>(hr) + h2));
            c.borderPolyBezierTo(p, 6);
            break;
        }
        case 3: { // high pass: the low pass mirrored
            const int hr = halfR();
            const double hrd = hr;
            const int y = trunc(M + hrd);
            c.moveTo(right, y);
            const int xc = trunc(static_cast<double>(xscale * (90.0f - freqf)) + 0.5);
            auto mx = [&](int v) { return trunc(Wf - static_cast<float>(v)); };
            c.borderLineTo(s16(mx(xc - (slope >> 1) + 14)), s16(y));
            set(0, mx(xc - (slope >> 1) + 24), y);
            set(1, mx(xc - (slope * 171 >> 9) + 20), y);
            set(2, mx(xc + 20), trunc(M - static_cast<double>(res12) + hrd));
            set(3, mx(xc + (slope >> 2) + 19), y);
            set(4, mx(xc + 29), trunc(hrd + (M + 5.0)));
            set(5, trunc(Wf - (static_cast<float>(xc) + (wp1 - static_cast<float>(slope)))),
                trunc(static_cast<float>(hr) + h2));
            c.borderPolyBezierTo(p, 6);
            break;
        }
        case 2: { // band pass
            const int xc = trunc(static_cast<double>((90.0f - freqf) * xscale) + 0.5);
            const float ws = wp1 - static_cast<float>(slope);
            const int hr = halfR();
            const int bottom = trunc(static_cast<float>(hr) + h2);
            const int low = trunc(M + 5.0 + static_cast<double>(hr));
            const int q = slope >> 2;
            set(0, trunc((Wf - (static_cast<float>(xc) + ws)) - 10.0f), bottom);
            set(1, trunc((Wf - static_cast<float>(xc + 29)) - 10.0f), low);
            const int y = trunc(M + static_cast<double>(hr));
            set(2, trunc(Wf - static_cast<float>(xc + q + 19)), y);
            const int xc2 = trunc(static_cast<double>(xscale * (freqf - static_cast<float>(k))) + 0.5);
            set(3, xc2 + 20, trunc(static_cast<double>(hr) + (M - static_cast<double>(res12))));
            set(4, xc2 + q + 19, y);
            set(5, xc2 + 39, low);
            set(6, trunc((ws + static_cast<float>(xc2)) + 10.0f), bottom);
            c.borderPolyBezier(p, 7);
            break;
        }
        case 4: { // band reject
            const int x0 = trunc(static_cast<double>(xscale * -30.0f) + 0.5);
            const int yb = trunc(M + static_cast<double>(trunc(static_cast<double>(f32(Rd * 0.25)) + 0.5)));
            set(0, x0, yb);
            p[1] = p[0];
            p[2] = p[0];
            const int xa = trunc(static_cast<double>((freqf - (3.0f + Wf)) * xscale) + 0.5);
            set(3, xa, yb);
            const int w = trunc(static_cast<double>(static_cast<float>((f.res >> 2) + 30) * xscale) + 0.5);
            set(4, xa + w, yb);
            const int xm = trunc(static_cast<double>(xscale * freqf) + 0.5);
            const int dip = trunc(static_cast<float>(trunc(static_cast<double>(static_cast<float>(slope * 2 - 24) * yscale) + 0.5)) + Hf);
            set(6, xm, dip);
            set(5, xm, dip - trunc(static_cast<double>(yscale * 30.0f) + 0.5));
            p[7] = p[5];
            const int xr = trunc(static_cast<double>((freqf + 60.0f) * xscale) + 0.5);
            set(9, xr, yb);
            set(8, xr - w, yb);
            p[10] = p[9];
            p[11] = p[9];
            set(12, trunc(static_cast<double>(xscale * 158.0f) + 0.5), yb);
            c.borderPolyBezier(p, 13);
            break;
        }
        default:
            break;
        }
    }
    c.fillUnder(c.W - 2, c.H - 2);
    std::string s = std::to_string(f.slope);
    const int tw = Canvas::textWidth(s);
    c.text(s16(trunc((Wf - static_cast<float>(tw)) - 3.0f)), 7, s);
    c.blit(1, 1, c.W - 2, c.H - 2);
}

} // namespace filters

// 20 CPnlClassicFilterGraph (UpdateGraphics 0x95bf4): deps freq, res, dB/Oct
// (0..2 -> 12/18/24), on; always low pass with the "classic" knee.
template <>
void drawGraph<20>(Canvas& c, Values v)
{
    filters::FilterFields f;
    f.freq = dep(v, 0);
    f.res = dep(v, 1);
    f.slope = b8(dep(v, 2) * 6 + 12);
    f.on = dep(v, 3);
    f.type = 1;
    f.classic = 1;
    f.gc = 0;
    filters::drawFilter(c, f);
}

// 21 CPnlNormalFilterGraph (UpdateGraphics 0x95a64): deps freq, res, type
// (0..3 -> LP/BP/HP/BR), GComp, dB/Oct (0..1 -> 12/24), on.
template <>
void drawGraph<21>(Canvas& c, Values v)
{
    filters::FilterFields f;
    f.freq = dep(v, 0);
    f.res = dep(v, 1);
    f.type = b8(dep(v, 2) + 1);
    f.gc = dep(v, 3);
    f.slope = b8(dep(v, 4) * 12 + 12);
    f.on = dep(v, 5);
    f.classic = 0;
    filters::drawFilter(c, f);
}

// 40 CPnlStaticFilterGraph (UpdateGraphics 0x95b3a): deps freq, res, type
// (0..2 -> LP/BP/HP), GComp, on; slope fixed at 12.
template <>
void drawGraph<40>(Canvas& c, Values v)
{
    filters::FilterFields f;
    f.freq = dep(v, 0);
    f.res = dep(v, 1);
    f.type = b8(dep(v, 2) + 1);
    f.gc = dep(v, 3);
    f.slope = 12;
    f.on = dep(v, 4);
    f.classic = 0;
    filters::drawFilter(c, f);
}

namespace filters {

// CPnlSmallFilterGraph::Draw (0xa4c4a): 12..36 dB slope sketch of FltLP/FltHP.
// type 1 = low pass, 3 = high pass (constructor argument).
void drawSmallFilter(Canvas& c, int type, u8 freq, u8 on, u8 slope)
{
    c.frame();
    const float Hf = static_cast<float>(c.H), Wf = static_cast<float>(c.W);
    c.rect(0, 0, c.W - 2, c.H - 2, Ink::Back);
    const int right = s16(trunc(Wf));
    c.moveTo(0, 6);
    c.backLineTo(right, 6);
    float textRight = Wf;
    if (on == 0) {
        c.moveTo(0, 6);
        c.borderLineTo(right, 6);
    } else {
        const float w2 = Wf - 2.0f;
        const int xc = trunc(static_cast<double>(freq) / 127.0 * static_cast<double>(w2));
        // horizontal offsets of the two Bezier control points of the skirt
        const int a = trunc(static_cast<double>(f32(slope * 0.85)) + 0.5);
        const int b = trunc(static_cast<double>(f32(slope * 0.8)) + 0.5);
        const int bottom = trunc(Hf - 2.0f);
        Pt p[3];
        if (type == 1) {
            c.moveTo(0, 6);
            c.borderLineTo(s16(xc), 6);
            const int x = static_cast<std::uint16_t>(xc) + 32;
            p[0] = {s16(xc), 6};
            p[1] = {s16(x - a), 11};
            p[2] = {s16(x - b), s16(bottom)};
            c.borderPolyBezierTo(p, 3);
        } else if (type == 3) {
            c.moveTo(trunc(w2), 6);
            c.borderLineTo(s16(xc), 6);
            const int x = static_cast<std::uint16_t>(xc) - 32;
            p[0] = {s16(xc), 6};
            p[1] = {s16(x + a), 11};
            p[2] = {s16(x + b), s16(bottom)};
            c.borderPolyBezierTo(p, 3);
            textRight = w2 + 2.0f;
        }
    }
    c.fillUnder(c.W - 2, c.H - 2);
    std::string s = std::to_string(slope);
    const int tw = Canvas::textWidth(s);
    c.text(s16(trunc((textRight - static_cast<float>(tw)) - 3.0f)), 7, s);
    c.blit(1, 1, c.W - 2, c.H - 2);
}

} // namespace filters

// 30 / 31 CPnlSmallFilterGraph (UpdateGraphics 0x959da), FltLP / FltHP: deps
// freq, on, slope mode (0..5 -> 6..36 dB).
template <>
void drawGraph<30>(Canvas& c, Values v)
{
    filters::drawSmallFilter(c, 1, dep(v, 0), dep(v, 1), b8(dep(v, 2) * 6 + 6));
}
template <>
void drawGraph<31>(Canvas& c, Values v)
{
    filters::drawSmallFilter(c, 3, dep(v, 0), dep(v, 1), b8(dep(v, 2) * 6 + 6));
}

namespace filters {

// Fields of CPnlFeedbackFlt (+0x84..+0x87).
struct FeedbackFields {
    u8 notches = 0; // +0x84 number of notches (0..5)
    u8 spread = 0;  // +0x85 notch spacing 0..127
    u8 fb = 64;     // +0x86 feedback, 64 = none
    u8 type = 0;    // +0x87 response type
};

// CPnlFeedbackFlt::Draw (0xa4236): comb / phaser response. A left skirt, one
// notch (two Bezier segments) per notch, a right skirt; filled under.
void drawFeedback(Canvas& c, const FeedbackFields& f)
{
    c.frame();
    const float Hf = static_cast<float>(c.H), Wf = static_cast<float>(c.W);
    c.rect(0, 0, c.W - 2, c.H - 2, Ink::Back);
    const float hm = (Hf - 2.0f) * 0.5f; // mid line

    // per notch count: depth factor a, width factor b, skirt start x (double)
    float a = 0.0f, b = 0.0f;
    double cx = -11.0;
    switch (f.notches) {
    case 0: a = 6.0f, b = 10.0f, cx = -11.0; break;
    case 1: a = 5.0f, b = 9.0f, cx = -11.0; break;
    case 2: a = 4.0f, b = 8.0f, cx = -13.0; break;
    case 3: a = 3.0f, b = 7.0f, cx = -16.0; break;
    case 4: a = 2.0f, b = 6.0f, cx = -18.0; break;
    case 5: a = 1.0f, b = 5.0f, cx = -21.0; break;
    default: break;
    }
    const float rf = static_cast<float>(f.spread);
    const float step = (b * rf) / 127.0f;            // half notch spacing
    const float amp = ((a * rf) / 127.0f) * 0.8125f; // control point inset
    const float x0 = static_cast<float>(
        trunc(static_cast<double>(((Wf - 5.0f) - static_cast<float>(f.notches + 1) * step) * 0.5f) + 0.5));
    const float q = 0.8125f * static_cast<float>(25 - (f.spread * 20) / 127);

    // feedback shapes: start/end level, dip, notch levels
    const int base = trunc(static_cast<double>(hm) + 0.5);
    int startY = base;
    float dip = 0.0f, top = 0.0f, notchLow = 0.0f, notchMid = 0.0f;
    const u8 fb = f.fb;
    auto fbf = [&] { return f32(static_cast<double>(fb - 64) * 0.015625); };
    auto lifted = [&](float off) { return trunc(static_cast<double>(hm + off) + 0.5); };
    switch (f.type) {
    case 0:
        if (fb > 63) {
            dip = fbf() * 15.0f;
        } else {
            startY = trunc(static_cast<double>(fbf() * -15.0f + hm) + 0.5);
            notchLow = f32(static_cast<double>(fb) * -0.015625 + 1.0) * 15.0f;
        }
        break;
    case 1:
        if (fb > 63) {
            notchLow = fbf() * -15.0f;
            startY = lifted(notchLow);
        } else {
            dip = fbf() * 15.0f;
        }
        break;
    case 2: {
        const float t = fbf();
        notchLow = t * -15.0f;
        dip = t * 15.0f;
        if (fb > 63) {
            startY = lifted(notchLow);
        }
        break;
    }
    default:
        if (fb <= 76) {
            top = static_cast<float>(1 - fb / 38) * 0.0f;
            notchLow = static_cast<float>((fb * 15) / 76);
            notchMid = static_cast<float>(5 - (fb * 10) / 76);
        } else {
            dip = static_cast<float>((760 - 10 * fb) / 50);
            top = -0.0f;
            notchLow = 15.0f;
            notchMid = -5.0f;
        }
        break;
    }

    c.moveTo(0, base);
    c.backLineTo(s16(trunc(Wf)), base);
    c.moveTo(trunc(cx), startY);
    const double qd = q;
    auto r = [](float v) { return trunc(static_cast<double>(v) + 0.5); };
    Pt p[6];
    p[0] = {s16(r(f32(cx + qd))), s16(base)};
    p[1] = {s16(r(x0 - 3.0f)), s16(r(dip * 0.5f + hm))};
    const int yDip = r(dip + hm);
    p[2] = {s16(r(x0)), s16(yDip)};
    c.borderPolyBezierTo(p, 3);

    const float step2 = step + step;
    const int yTop = r(top + hm);
    const int yLow = r(notchMid + (notchLow + hm));
    const int yLow2 = r(notchLow + hm);
    for (int i = 0; i < f.notches; ++i) {
        const float xs = static_cast<float>(2 * i) * step + x0;
        const int xn = r(step + xs);
        const float xe = xs + step2;
        p[0] = {s16(r(amp + xs)), s16(yTop)};
        p[1] = {s16(xn), s16(yLow)};
        p[2] = {s16(xn), s16(yLow2)};
        p[3] = {s16(xn), s16(yLow)};
        p[4] = {s16(r(xe - amp)), s16(yTop)};
        p[5] = {s16(r(xe)), s16(yDip)};
        c.borderPolyBezierTo(p, 6);
    }

    const float w2 = Wf - 2.0f;
    const double xr = static_cast<double>(w2) - cx;
    p[0] = {s16(r((w2 - x0) + 3.0f)), s16(r(hm + dip / 3.0f))};
    p[1] = {s16(r(f32(xr - qd))), s16(base)};
    p[2] = {s16(r(f32(xr))), s16(r(static_cast<float>(startY)))};
    c.borderPolyBezierTo(p, 3);

    c.fillUnder(c.W - 2, c.H - 2);
    c.blit(1, 1, c.W - 2, c.H - 2);
}

} // namespace filters

// 13 CPnlPhaserGraph (UpdateGraphics 0x95d0c), FltPhase: deps spread, FB,
// notches (0..5), type.
template <>
void drawGraph<13>(Canvas& c, Values v)
{
    filters::FeedbackFields f;
    f.spread = dep(v, 0);
    f.fb = dep(v, 1);
    f.notches = dep(v, 2);
    f.type = dep(v, 3);
    filters::drawFeedback(c, f);
}

// 35 CPnlCombFltGraph (UpdateGraphics 0x95c94), FltComb: deps FB, type; four
// notches at spacing 120.
template <>
void drawGraph<35>(Canvas& c, Values v)
{
    filters::FeedbackFields f;
    f.spread = 120;
    f.fb = dep(v, 0);
    f.notches = 4;
    f.type = dep(v, 1);
    filters::drawFeedback(c, f);
}

// 8 CPnlVocoderGraph (UpdateGraphics 0x965cc, Draw 0x9d92c): the 16 band
// routings. Band i (analysis band at the bottom, x = 6 + 12 i, y = 45) is
// connected by a straight line to the synthesis band v (1..16, x = 12 v - 6,
// y = -1, i.e. the top edge); 0 = off, no line.
template <>
void drawGraph<8>(Canvas& c, Values v)
{
    c.frame();
    c.rect(0, 0, c.W - 2, c.H - 2, Ink::Back);
    for (int i = 0; i < 16; ++i) {
        const int band = dep(v, static_cast<std::size_t>(i));
        if (band != 0) {
            c.moveTo(6 + 12 * i, 45);
            c.singleLineTo(s16(band * 12 - 6), -1);
        }
    }
    c.blit(1, 1, c.W - 2, c.H - 2);
}

// --- equalisers, level scaler, crossfade mux -----------------------------------

namespace eq {

// Common start of these Draw()s: frame, background of the interior (scratch
// (0,0)-(W-2,H-2)); common end: blit of the interior at (1,1).
inline void begin(Canvas& c)
{
    c.frame();
    c.rect(0, 0, c.W - 2, c.H - 2, Ink::Back);
}
inline void end(Canvas& c) { c.blit(1, 1, c.W - 2, c.H - 2); }

// Vertical scale shared by the EQ graphs: the 0 dB line in the middle of the
// interior, gain g (0..127, 64 = 0 dB) moves the curve by (g-64) * 0.45 h/64.
struct GainScale {
    double mid;  // 0 dB line (integer, kept as double)
    float scale; // pixels per gain step
    explicit GainScale(const Canvas& c)
    {
        const float hh = static_cast<float>(c.H) - 2.0f;
        mid = static_cast<double>(trunc(static_cast<double>(hh * 0.5f) + 0.5));
        scale = f32(static_cast<double>(hh) * 0.45 * 0.015625);
    }
    // y of a gain value; the float product is subtracted in double, then
    // rounded through float.
    int y(int gain) const
    {
        const float d = static_cast<float>(gain - 64) * scale;
        return trunc(static_cast<double>(f32(mid - static_cast<double>(d))) + 0.5);
    }
    int y(float factor) const // factor already multiplied in
    {
        return trunc(static_cast<double>(f32(mid - static_cast<double>(factor))) + 0.5);
    }
};

inline int roundD(double v) { return trunc(v + 0.5); }
inline int roundF(float v) { return trunc(static_cast<double>(v) + 0.5); }

// 0 dB back line across the interior; returns its y and the right end x.
inline std::pair<int, int> zeroLine(Canvas& c, const GainScale& g)
{
    const int by = s16(trunc(g.mid));
    const int right = s16(trunc(static_cast<float>(c.W)));
    c.moveTo(0, by);
    c.backLineTo(right, by);
    return {by, right};
}

// CPnlEqHiLoGraph (UpdateGraphics 0x9691a, Draw 0xa1880): low shelf from
// value 0, high shelf from value 1.
void drawHiLo(Canvas& c, int lo, int hi)
{
    begin(c);
    const GainScale g(c);
    const float xs = ((static_cast<float>(c.W) - 2.0f) - 20.0f) * 0.0078125f;
    auto [by, right] = zeroLine(c, g);

    // low shelf: flat at its gain up to x(25) - 16, Bezier down to the 0 dB line at x(25)
    const float xl = xs * 25.0f + 10.0f;
    const int yl = g.y(lo);
    const int lx = roundF(xl);
    const int l0 = roundD(static_cast<double>(s16(lx) - 8));
    Pt pl[3] = {{s16(l0), s16(yl)}, {s16(l0), s16(by)}, {s16(lx), s16(by)}};
    c.moveTo(-1, yl);
    c.borderLineTo(roundF(xl - 16.0f), yl);
    c.borderPolyBezierTo(pl, 3);

    // high shelf: 0 dB up to x(100), Bezier to its gain at x(100) + 16
    const float xh = xs * 100.0f + 10.0f;
    const int yh = g.y(hi);
    const int hx = s16(roundF(xh));
    const int h0 = roundD(static_cast<double>(hx + 8));
    Pt ph[3] = {{s16(h0), s16(by)}, {s16(h0), s16(yh)}, {s16(roundF(xh + 16.0f)), s16(yh)}};
    c.borderLineTo(hx, by);
    c.borderPolyBezierTo(ph, 3);
    c.borderLineTo(right, s16(yh));

    c.fillUnder(c.W - 2, c.H - 2);
    end(c);
}

// CPnlEqMidGraph (UpdateGraphics 0x96890, Draw 0xa1c7e): peak at frequency
// value 0 with gain value 1; field 0x86 = |bandwidth - 127| sets the width.
void drawMid(Canvas& c, int freq, int gain, int bandwidth)
{
    const int narrow = b8(std::abs(bandwidth - 127));
    begin(c);
    const GainScale g(c);
    const float xs = (static_cast<float>(c.W) - 6.0f) * 0.0078125f;
    auto [by, right] = zeroLine(c, g);

    const double cxF = static_cast<double>(static_cast<float>(freq) * xs + 2.0f);
    const double bw = static_cast<double>(xs * static_cast<float>(narrow));
    const int cx = s16(trunc(cxF + 0.5));
    const int yg = g.y(gain);
    const int rx = s16(roundD(static_cast<double>(f32(bw * 0.7 + cxF + 4.0))));
    const int lx = s16(roundD(static_cast<double>(f32(cxF + bw * -0.7 - 4.0))));
    const int l0 = roundD(static_cast<double>(lx + (cx - lx) / 2));
    const int r0 = roundD(static_cast<double>(cx + (rx - cx) / 2));
    Pt p[6] = {{s16(l0), s16(by)}, {s16(l0), s16(by)}, {s16(cx), s16(yg)},
               {s16(r0), s16(by)}, {s16(r0), s16(by)}, {s16(rx), s16(by)}};
    c.moveTo(-1, by);
    c.borderLineTo(lx, by);
    c.borderPolyBezierTo(p, 6);
    c.borderLineTo(right, by);

    c.fillUnder(c.W - 2, c.H - 2);
    end(c);
}

// CPnlEqHiMidLoGraph (UpdateGraphics 0x96984, Draw 0xa1112): low shelf
// (value 0), mid peak (gain value 1 at frequency value 2), high shelf (value 3).
// The mid band is drawn as two halves meeting at x = lerp(x(10), x(117), f/127);
// near the ends (f < 40 or f > 96) the mid gain also bends the shelf corner.
void drawHiMidLo(Canvas& c, int lo, int midGain, int midFreq, int hi)
{
    begin(c);
    const GainScale g(c);
    const float xs = ((static_cast<float>(c.W) - 2.0f) - 20.0f) * 0.0078125f;
    auto [by, right] = zeroLine(c, g);
    const float midD = static_cast<float>(midGain - 64) * g.scale;

    const float xl = xs * 10.0f + 10.0f;
    const int lx = s16(roundF(xl));
    int yMidL = by; // mid gain seen at the low corner
    if (midFreq < 40) {
        yMidL = s16(g.y((static_cast<float>(midFreq) / -40.0f + 1.0f) * midD));
    }
    const float xhF = xs * 117.0f + 10.0f;
    const int hx = s16(roundF(xhF));
    int yMidH = by; // mid gain seen at the high corner
    if (midFreq > 96) {
        yMidH = s16(g.y((static_cast<float>(midFreq - 97) / 30.0f) * midD));
    }

    // low shelf
    const int yl = g.y(lo);
    const int l0 = roundD(static_cast<double>(lx - 5));
    Pt pl[3] = {{s16(l0), s16(yl)}, {s16(l0), s16(by)}, {s16(lx), s16(yMidL)}};
    c.moveTo(-1, yl);
    c.borderLineTo(roundF(xl - 10.0f), yl);
    c.borderPolyBezierTo(pl, 3);

    // mid peak
    const float t = static_cast<float>(midFreq) / 127.0f;
    const int mx = s16(trunc(static_cast<float>(lx) + static_cast<float>(hx - lx) * t));
    const int ym = g.y(midGain);
    if (lx < mx) {
        int q0;
        if (g.mid == static_cast<double>(yMidL) && lx < mx - 12) {
            c.borderLineTo(s16(mx - 12), by);
            q0 = mx - 6;
        } else {
            q0 = roundD(static_cast<double>(lx + (mx - lx) / 2));
        }
        Pt q[3] = {{s16(q0), s16(yMidL)}, {s16(q0), s16(yMidL)}, {s16(mx), s16(ym)}};
        c.borderPolyBezierTo(q, 3);
    }
    if (hx > mx) {
        const bool flat = g.mid == static_cast<double>(yMidH) && hx > mx + 12;
        int q0, q2;
        if (flat) {
            q0 = mx + 6;
            q2 = mx + 12;
        } else {
            q0 = roundD(static_cast<double>(mx + (hx - mx) / 2));
            q2 = hx;
        }
        Pt q[3] = {{s16(q0), s16(yMidH)}, {s16(q0), s16(yMidH)}, {s16(q2), s16(yMidH)}};
        c.borderPolyBezierTo(q, 3);
        if (flat) {
            c.borderLineTo(s16(mx + 12), by);
        }
    }

    // high shelf
    const int yh = g.y(hi);
    const int h0 = roundD(static_cast<double>(hx + 5));
    Pt ph[3] = {{s16(h0), s16(by)}, {s16(h0), s16(yh)}, {s16(roundF(xhF + 10.0f)), s16(yh)}};
    c.borderLineTo(hx, yMidH);
    c.borderPolyBezierTo(ph, 3);
    c.borderLineTo(right, s16(yh));

    c.fillUnder(c.W - 2, c.H - 2);
    end(c);
}

// CPnlAmpProcGraph (UpdateGraphics 0x9682c, Draw 0xa208c): LevScaler. Two
// lines through the break point (value 1) on the centre line, sloped by the
// left gain (value 0) and right gain (value 2); no fill.
void drawAmpProc(Canvas& c, int leftGain, int breakPoint, int rightGain)
{
    begin(c);
    const float wf = static_cast<float>(c.W);
    const float ww = wf - 2.0f;
    const double bx = static_cast<double>(roundD(static_cast<double>(static_cast<float>(breakPoint) * ww / 127.0f)));
    const double mid = static_cast<double>((static_cast<float>(c.H) - 2.0f) * 0.5f);
    const double dl = static_cast<double>(leftGain - 64) * 0.5;
    const double dr = static_cast<double>(rightGain - 64) * 0.5;
    const int by = s16(trunc(mid));
    c.moveTo(1, by);
    c.backLineTo(s16(trunc(ww)), by);
    c.moveTo(trunc(bx), by);
    c.singleLineTo(s16(trunc(bx - static_cast<double>(wf) - 3.0)), s16(trunc(mid - dl)));
    c.moveTo(trunc(bx), by);
    c.singleLineTo(s16(trunc(bx + static_cast<double>(wf) - 1.0)), s16(trunc(mid - dr)));
    end(c);
}

// CPnlXMuxGraph (UpdateGraphics 0x96efa, Draw 0xa0726): crossfade picture,
// two trapezoid ramps meeting in the middle, between two grey verticals at
// w/6 and w - w/6; the fade value 0 (0..127) moves the ramp corners.
void drawXMux(Canvas& c, int fade)
{
    begin(c);
    const int w = c.W - 2, h = c.H - 2;
    const float wf = static_cast<float>(w);
    const int e = trunc(wf / 6.0f);
    const int r = trunc(wf - static_cast<float>(e));
    const int m = trunc(wf * 0.5f);
    const int off = trunc(static_cast<double>(fade) / 127.0 * (static_cast<double>(r - e) * 0.5));
    const int bot = s16(trunc(static_cast<float>(h) - 1.0f));
    const int a = s16((m - e) * 2);
    const int b = s16(m - e);
    c.moveTo(e - a, bot);
    c.singleLineTo(s16(e - b + off), 3);
    c.singleLineTo(s16(m - off), 3);
    c.singleLineTo(s16(m + off), bot);
    c.moveTo(m - off, bot);
    c.singleLineTo(s16(m + off), 3);
    c.singleLineTo(s16(b + r - off), 3);
    c.singleLineTo(s16(a + r), bot);
    c.moveTo(e, 1);
    c.backLineTo(s16(e), bot);
    c.moveTo(r, 1);
    c.backLineTo(s16(r), bot);
    end(c);
}

} // namespace eq

// 9 (ShelvEQ, unmapped) and 36 (Eq2Band): CPnlEqHiLoGraph, values lo, hi.
template <>
void drawGraph<9>(Canvas& c, Values v)
{
    eq::drawHiLo(c, dep(v, 0), dep(v, 1));
}
template <>
void drawGraph<36>(Canvas& c, Values v)
{
    eq::drawHiLo(c, dep(v, 0), dep(v, 1));
}
// 10 EqPeak: freq, gain, bandwidth.
template <>
void drawGraph<10>(Canvas& c, Values v)
{
    eq::drawMid(c, dep(v, 0), dep(v, 1), dep(v, 2));
}
// 37 Eq3band: lo, mid gain, mid freq, hi.
template <>
void drawGraph<37>(Canvas& c, Values v)
{
    eq::drawHiMidLo(c, dep(v, 0), dep(v, 1), dep(v, 2), dep(v, 3));
}
// 11 LevScaler: left gain, break point, right gain.
template <>
void drawGraph<11>(Canvas& c, Values v)
{
    eq::drawAmpProc(c, dep(v, 0), dep(v, 1), dep(v, 2));
}
// 12 Mux8-1X: crossfade.
template <>
void drawGraph<12>(Canvas& c, Values v)
{
    eq::drawXMux(c, dep(v, 0));
}

// --- shapers: Clip, Overdrive, WaveWrap, ShpExp, Saturate -----------------------

namespace shapers {

// Common start of the shaper Draw()s: frame, background of the scratch
// bitmap over the interior (W-2) x (H-2).
inline void begin(Canvas& c)
{
    c.frame();
    c.rect(0, 0, c.W - 2, c.H - 2, Ink::Back);
}
inline void end(Canvas& c) { c.blit(1, 1, c.W - 2, c.H - 2); }

} // namespace shapers

// 14 CPnlDistAGraph (Clip; UpdateGraphics 0x9639c, Draw 0xa39cc).
// values: Shape (0 asymmetric, 1 symmetric), Clip level. A transfer curve on
// a cross of BackLines: flat at the clip level on the right, a diagonal, then
// flat at -level (symmetric) or down to the bottom edge (asymmetric).
template <>
void drawGraph<14>(Canvas& c, Values v)
{
    const u8 shape = dep(v, 0);
    const u8 clip = dep(v, 1);
    shapers::begin(c);
    const float fh = static_cast<float>(c.H), fw = static_cast<float>(c.W);
    const double hx = static_cast<double>(fw * 0.5f - 1.0f);
    const double hy = static_cast<double>(fh * 0.5f - 1.0f);
    const double r = static_cast<double>(static_cast<float>(clip) / 127.0f);
    const double dx = trunc(static_cast<double>(f32(hx * r)) + 0.5);
    const double dy = trunc(static_cast<double>(f32(r * hy)) + 0.5);
    const i16 right = s16(trunc(fw));
    c.moveTo(0, s16(trunc(hy)));
    c.backLineTo(right, s16(trunc(hy)));
    c.moveTo(s16(trunc(hx)), 0);
    c.backLineTo(s16(trunc(hx)), s16(trunc(fh)));
    c.moveTo(right, s16(trunc(dy)));
    c.singleLineTo(s16(trunc(static_cast<double>(fw) - dx - 1.0)), s16(trunc(dy)));
    i16 low = s16(trunc(static_cast<double>(fh) - dy - 2.0));
    c.singleLineTo(s16(trunc(dx)), low);
    if (shape == 0) {
        low = s16(trunc(fh - 1.0f));
    }
    c.singleLineTo(-1, low);
    shapers::end(c);
}

// 15 CPnlDistBGraph (Overdrive; UpdateGraphics 0x96406, Draw 0xa3080).
// values: Shape (0 asymmetric, 1 symmetric), Drive. The curve is the cubic
// soft clipper y = t - 2048/3 * (g t)^3 (g = Drive/127), plotted for 20
// points t = -9..10 times a step, scaled to the interior; above g = 1/16 the
// t range shrinks to 1/(16 g) so the curve reaches its flat top. Asymmetric
// shapes are cut at y = H - 7 g (the positive half is flattened).
template <>
void drawGraph<15>(Canvas& c, Values v)
{
    const u8 shape = dep(v, 0);
    const float g = f32(static_cast<double>(static_cast<float>(dep(v, 1)) / 127.0f));
    shapers::begin(c);
    const float fh = static_cast<float>(c.H), fw = static_cast<float>(c.W);
    const float w2 = fw - 2.0f, h2 = fh - 2.0f;
    const double cx = static_cast<double>(w2 * 0.5f);
    const double cy = static_cast<double>(0.5f * h2);
    const float ww = w2 + w2, hh = h2 + h2;
    const i16 midY = s16(trunc(dfloor(cy)));
    c.moveTo(0, midY);
    c.backLineTo(s16(trunc(w2)), midY);
    const i16 midX = s16(trunc(dfloor(cx)));
    c.moveTo(midX, 0);
    c.backLineTo(midX, s16(trunc(h2)));

    auto px = [&](float t) { return s16(trunc(static_cast<double>(f32(static_cast<double>(t * ww) + cx)) + 0.5)); };
    // The flat bottom of the asymmetric curve.
    const int limit = trunc(static_cast<double>(-7.0f * g + fh) + 0.5);
    if (g > 0.0625f || std::isnan(g)) {
        const float inv = f32(1.0 / (static_cast<double>(g) * 16.0));
        const float step = inv / 10.0f;
        const float ninv = -inv;
        const float scale = std::max(0.375f, inv) * hh;
        const float g16 = g * 16.0f;
        auto curve = [&](float t) { return (t * 4096.0f * t * t * g * g * g) / -3.0f + g16 * t; };
        i16 y;
        if (shape == 0) {
            float a = g16 * ninv + (ninv * 4096.0f * ninv * ninv * g * g * g) / -3.0f;
            y = s16(trunc(static_cast<double>(f32(cy - static_cast<double>(a * scale))) + 0.5));
        } else {
            y = s16(trunc(g * -7.0f + fh));
        }
        c.moveTo(-2, y);
        c.singleLineTo(px(ninv), y);
        float t = 0.0f;
        double k = -9.0;
        for (int i = 0; i < 20; ++i, k += 1.0) {
            t = f32(static_cast<double>(step) * k);
            const double cl = dceil(cy - static_cast<double>(curve(t) * scale));
            int yy;
            if (shape == 0) {
                yy = s16(trunc(cl));
            } else {
                yy = s16(std::min(limit, trunc(static_cast<double>(f32(cl)) + 0.5)));
            }
            c.singleLineTo(px(t), yy);
        }
        c.singleLineTo(s16(trunc(fw)), s16(trunc(dceil(cy - static_cast<double>(curve(t) * scale)))));
    } else {
        i16 y;
        if (shape == 0) {
            y = s16(trunc(static_cast<double>(
                              f32(cy - static_cast<double>(((g * -64.0f * g * g) / -3.0f - 0.25f) * hh))) +
                          0.5));
        } else {
            y = s16(trunc(g * -7.0f + fh));
        }
        c.moveTo(px(-0.25f), y);
        double k = -9.0;
        for (int i = 0; i < 20; ++i, k += 1.0) {
            const float t = f32(k * 0.02500000037252903);
            float e = t * t * t * 2048.0f;
            e = ((e + e) * g * g * g) / -3.0f + t;
            int yy = trunc(static_cast<double>(f32(cy - static_cast<double>(e * hh))) + 0.5);
            if (shape == 0) {
                yy = std::min(limit, yy);
            }
            c.singleLineTo(px(t), s16(yy));
        }
    }
    shapers::end(c);
}

// 16 CPnlWrapGraph (WaveWrap; UpdateGraphics 0x96486, Draw 0xa2a50).
// value: Wrap gain. gain = 1 + Gain/8 (a double field). The transfer curve
// is a triangle wave through the centre: floor(gain) full segments of slope
// gain (each spans 1/gain of the half width and the whole half height),
// alternating up/down in groups of two, then a partial segment for the
// fraction of gain; each segment is drawn twice, mirrored about the centre.
template <>
void drawGraph<16>(Canvas& c, Values v)
{
    const double gain = static_cast<double>(static_cast<float>(dep(v, 0)) * 0.125f + 1.0f);
    shapers::begin(c);
    const float fh = static_cast<float>(c.H), fw = static_cast<float>(c.W);
    const float w2 = fw - 2.0f, h2 = fh - 2.0f;
    const double cxd = static_cast<double>(w2 * 0.5f);
    const i16 midY = s16(trunc(static_cast<double>(h2 * 0.5f)));
    c.moveTo(0, midY);
    c.backLineTo(s16(trunc(w2)), midY);
    const i16 midX = s16(trunc(cxd));
    c.moveTo(midX, 0);
    c.backLineTo(midX, s16(trunc(h2)));

    const float h3 = fh - 3.0f;
    const float amp = 0.5f * h3;          // half height
    const float mx = 0.5f * (fw - 3.0f);  // centre x
    const float seg = f32(static_cast<double>(mx) / gain);
    const u8 count = b8(trunc(gain));
    // One segment from (x, y) to (nx, ny) and its mirror image.
    auto segment = [&](float x, float y, float nx, float ny) {
        const int yi = trunc(static_cast<double>(y) + 0.5);
        const int nyi = trunc(static_cast<double>(ny) + 0.5);
        c.moveTo(s16(trunc(static_cast<double>(x + mx) + 0.5)), s16(yi));
        c.singleLineTo(s16(trunc(static_cast<double>(mx + nx) + 0.5)), s16(nyi));
        c.moveTo(s16(trunc(static_cast<double>(mx - x) + 0.5)), s16(trunc(h3 - static_cast<float>(yi))));
        c.singleLineTo(s16(trunc(static_cast<double>(mx - nx) + 0.5)), s16(trunc(h3 - static_cast<float>(nyi))));
    };
    float x = 0.0f, y = amp;
    for (int i = 1; static_cast<u8>(i) <= count; ++i) {
        const float nx = x + seg;
        const float ny = (i & 3) <= 1 ? y - amp : amp + y;
        segment(x, y, nx, ny);
        x = nx;
        y = ny;
    }
    double whole = 0.0;
    const double frac = std::modf(gain, &whole);
    const float fx = f32(static_cast<double>(x) + static_cast<double>(seg) * frac);
    const int m = count & 3;
    const float fy = (m == 1 || m == 2) ? f32(static_cast<double>(y) + static_cast<double>(amp) * frac)
                                        : f32(static_cast<double>(y) - static_cast<double>(amp) * frac);
    segment(x, y, fx, fy);
    shapers::end(c);
}

namespace shapers {

// Inner rect of the view (CRect::Shrink(1)) and its half sizes, as shorts.
struct Inner {
    i16 w, h, hx, hy;
    explicit Inner(const Canvas& c)
        : w(s16(c.W - 2)), h(s16(c.H - 2)), hx(s16(w / 2)), hy(s16(h / 2))
    {
    }
};

// Centre cross shared by ShapeB / Saturate.
inline void cross(Canvas& c, const Inner& r)
{
    c.moveTo(0, r.hy);
    c.backLineTo(r.w, r.hy);
    c.moveTo(r.hx, 0);
    c.backLineTo(r.hx, r.h);
}

// y of a curve value `val` (pixels from the bottom, already offset), clamped
// to [-1, h + 1] as the original does with shorts.
inline i16 fromBottom(const Inner& r, double val)
{
    i16 d = s16(trunc(val));
    const i16 lim = s16(r.h + 1);
    i16 pick = lim < d ? lim : d;
    if (pick <= -1) {
        pick = -1;
    }
    return s16(r.h - pick);
}

} // namespace shapers

// 38 CPnlShapeBGraph (ShpExp; UpdateGraphics 0x964f8, Draw 0xa26e4).
// values: Shape amount (s = Shape/127), Curve (0..3, exponent n = Curve + 2).
// For each column x, u = (x - hx) / hx in [-1, 1):
//   y = ((2u)^n * s (odd-symmetric) + 2 (1 - s) u) / 2 * hy + hy + 1,
// i.e. a blend of the line and the power curve x^2..x^5.
template <>
void drawGraph<38>(Canvas& c, Values v)
{
    const u8 amount = dep(v, 0);
    const u8 curve = dep(v, 1);
    shapers::begin(c);
    const shapers::Inner r(c);
    shapers::cross(c, r);
    const double s = static_cast<double>(static_cast<float>(amount) / 127.0f);
    const int n = curve + 2;
    const double lin = (1.0 - s) + (1.0 - s);
    const double hxd = r.hx, hy1 = r.hy + 1;
    for (int x = 0; x < r.w - 1; ++x) {
        const double u = (static_cast<double>(x) - hxd) / hxd;
        double p = powi(u, n);
        if ((n & 1) == 0 && u < 0.0) {
            p = -p;
        }
        const double val = (p * (powi(2.0, n) * s) + u * lin) * 0.5 * static_cast<double>(r.hy) + hy1;
        const i16 y = shapers::fromBottom(r, val);
        if (x == 0) {
            c.moveTo(0, y);
        } else {
            c.singleLineTo(x, y);
        }
    }
    shapers::end(c);
}

// 39 CPnlSaturateGraph (Saturate; UpdateGraphics 0x96562, Draw 0xa2330).
// values: Saturation amount (s = Sat/127), Curve (0..3, exponent n = Curve
// + 2). Per column, u = 2 (x - hx) / hx; outside [-1, 1] the curve is a
// line of slope (1 - s) continuing from +-1; inside, with z = u + 1 wrapped
// into [-1, 1): y = z^n (odd-symmetric) * s + z (1 - s) + 1, wrapped the same
// way. Plotted like ShpExp: y * hy / 2 + hy + 1 from the bottom.
template <>
void drawGraph<39>(Canvas& c, Values v)
{
    const u8 amount = dep(v, 0);
    const u8 curve = dep(v, 1);
    shapers::begin(c);
    const shapers::Inner r(c);
    shapers::cross(c, r);
    const double s = std::fabs(static_cast<double>(static_cast<float>(amount) / 127.0f));
    const int n = curve + 2;
    const double hxd = r.hx, hy1 = r.hy + 1;
    for (int x = 0; x < r.w - 1; ++x) {
        double u = (static_cast<double>(x) - hxd) / hxd;
        u = u + u;
        double val;
        if (u >= 1.0) {
            val = (1.0 - s) * (u - 1.0) + 1.0;
        } else if (!(u > -1.0) && !std::isnan(u)) {
            val = (1.0 - s) * (u + 1.0) - 1.0;
        } else {
            double z = u + 1.0;
            if (z >= 1.0) {
                z -= 2.0;
            }
            double p = powi(z, n);
            if ((n & 1) == 0 && z < 0.0) {
                p = -p;
            }
            val = p * s + z * (1.0 - s) + 1.0;
            if (val >= 1.0) {
                val -= 2.0;
            }
        }
        const i16 y = shapers::fromBottom(r, val * 0.5 * static_cast<double>(r.hy) + hy1);
        if (x == 0) {
            c.moveTo(0, y);
        } else {
            c.singleLineTo(x, y);
        }
    }
    shapers::end(c);
}

// --- oscillator and LFO waveforms (CPnlWaveformGraphABC) --------------------------

namespace osc {

// Fields of CPnlWaveformGraphABC (constructor 0x98a6c: all 0, out type 4).
struct Wave {
    u8 wave = 0;          // +0x84 waveform selector
    i16 shape = 0;        // +0x86 shape / symmetry, -128..126 or 0..127
    u8 phase = 0;         // +0x88 start phase 0..127
    unsigned outType = 4; // +0x8c LFO output type (0..5)
};

// Output type classes used by the waveform helpers:
// bit set 0x15 (types 0, 2, 4) and 0x2a (types 1, 3, 5) pick the polarity of
// the drawing, other values (>= 6) draw nothing for some waves.
inline bool typeIn(unsigned outType, unsigned mask) { return outType < 6 && ((1u << outType) & mask) != 0; }

// The helpers draw into the scratch bitmap over the view rect shrunk by one
// pixel: w x h = (W - 2) x (H - 2).
class Painter {
public:
    Painter(Canvas& c, const Wave& f) : c_(c), f_(f), w_(c.W - 2), h_(c.H - 2) {}

    // DrawBackLine (0xa017c): reference line for the output type.
    void backLine()
    {
        int y = (h_ - 1) / 2;
        if (typeIn(f_.outType, 0x03)) {
            y = h_ - 2;
        } else if (typeIn(f_.outType, 0x0c)) {
            y = 1;
        } else if (!typeIn(f_.outType, 0x30)) {
            y = 0;
        }
        c_.moveTo(0, y);
        c_.backLineTo(s16(w_ - 1), y);
    }

    // DrawBlank (0x9fba8): a flat BackLine in the middle.
    void blank()
    {
        const float H = static_cast<float>(h_), W = static_cast<float>(w_);
        int y = s16(trunc(static_cast<double>(H * 0.5f - 1.0f)));
        c_.moveTo(1, y);
        c_.backLineTo(s16(trunc(W - 3.0f)), y);
    }

    // DrawSine (0x9d74a).
    void sine()
    {
        const float H = static_cast<float>(h_), W = static_cast<float>(w_);
        int offs = 0;
        if (typeIn(f_.outType, 0x15)) {
            offs = 1;
        } else if (typeIn(f_.outType, 0x2a)) {
            offs = trunc(-(H - 1.0f));
        }
        const float w1 = W - 1.0f;
        const double x0 = trunc(static_cast<double>(-w1 * static_cast<float>(f_.phase) * 0.0078125f) + 0.5);
        const double amp = trunc(static_cast<double>((H - 3.0f) * 0.5f) + 0.5);
        const double k = static_cast<double>(f32(2 * kPi / static_cast<double>(w1)));
        int y0 = trunc(static_cast<double>(f32((dsin(x0 * k) + 1.0) * amp)) + 0.5) + offs;
        c_.moveTo(0, std::abs(y0));
        for (int x = 1; static_cast<float>(x) < W; ++x) {
            double y = std::fabs((1.0 - dsin((static_cast<double>(x) - x0) * k)) * amp + offs);
            c_.singleLineTo(s16(x), s16(trunc(y)));
        }
    }

    // DrawTri (0x9cf04).
    void tri()
    {
        const float H = static_cast<float>(h_), W = static_cast<float>(w_);
        const int mid = s16(trunc(static_cast<double>(H * 0.5f - 1.0f)));
        const float w2 = W - 2.0f;
        const double x0 = trunc(static_cast<double>(-w2 * static_cast<float>(f_.phase) * 0.0078125f) + 0.5);
        const float q = w2 * 0.25f;
        float x = f32(x0);
        int e = 0;
        if (typeIn(f_.outType, 0x15)) {
            e = -1;
        } else if (typeIn(f_.outType, 0x2a)) {
            e = trunc(-(H - 2.0f));
        }
        c_.moveTo(trunc(x0), mid);
        const int far = trunc(std::fabs((H - 1.0f) + static_cast<float>(e)));
        while (w2 > x) {
            c_.singleLineTo(s16(trunc(static_cast<double>(x + q) + 0.5)), s16(std::abs(e)));
            x = x + w2;
            c_.singleLineTo(s16(trunc(static_cast<double>(x - q) + 0.5)), s16(far));
            c_.singleLineTo(s16(trunc(static_cast<double>(x) + 0.5)), mid);
        }
    }

    // DrawSaw (0x9c524): nothing for out types >= 6.
    void saw()
    {
        const float H = static_cast<float>(h_), W = static_cast<float>(w_);
        const float w2 = W - 2.0f;
        float x = static_cast<float>(trunc(static_cast<double>((-w2 * static_cast<float>(f_.phase)) / 127.0f) + 0.5));
        int low, high;
        if (typeIn(f_.outType, 0x15)) {
            low = trunc(H - 2.0f);
            high = 1;
        } else if (typeIn(f_.outType, 0x2a)) {
            high = s16(trunc(H - 2.0f));
            low = 1;
        } else {
            return;
        }
        c_.moveTo(trunc(x), high);
        while (w2 > x) {
            x = x + w2;
            int px = s16(trunc(static_cast<double>(x) + 0.5));
            c_.singleLineTo(px, s16(low));
            c_.singleLineTo(px, s16(high));
        }
    }

    // DrawSquare (0x9cb2e): shape sets the duty cycle.
    void square()
    {
        const float H = static_cast<float>(h_), W = static_cast<float>(w_);
        const float w2 = W - 2.0f;
        const double x0 = trunc(static_cast<double>(-w2 * static_cast<float>(f_.phase) * 0.0078125f) + 0.5);
        const float hw = w2 * (static_cast<float>(f_.shape) * 0.0078125f + 1.0f) * 0.5f;
        float x = f32(x0);
        float lo = H - 4.0f;
        int top = 1;
        if (typeIn(f_.outType, 0x2a)) {
            top = s16(trunc(lo));
            lo = 1.0f;
        }
        const int bottom = s16(trunc(lo));
        c_.moveTo(trunc(x0), top);
        while (w2 > x) {
            c_.singleLineTo(s16(trunc(static_cast<double>(x) + 0.5)), top);
            int a = s16(trunc(static_cast<double>(x + hw) + 0.5));
            c_.singleLineTo(a, top);
            c_.singleLineTo(a, bottom);
            x = x + w2;
            int b = s16(trunc(static_cast<double>(x) + 0.5));
            c_.singleLineTo(b, bottom);
            c_.singleLineTo(b, top);
        }
    }

    // DrawPulse (0x9c65a): a pulse starting from the middle line; nothing for
    // out types >= 6.
    void pulse()
    {
        const float H = static_cast<float>(h_), W = static_cast<float>(w_);
        float yLow, yHigh; // xmm4, xmm5 of the original
        if (typeIn(f_.outType, 0x15)) {
            yLow = H - 2.0f;
            yHigh = 1.0f;
        } else if (typeIn(f_.outType, 0x2a)) {
            yHigh = H - 2.0f;
            yLow = 1.0f;
        } else {
            return;
        }
        const float w2 = W - 2.0f;
        const double x0 = trunc(static_cast<double>(-w2 * static_cast<float>(f_.phase) * 0.0078125f) + 0.5);
        const float pw = static_cast<float>(f_.shape) * -0.00390625f * w2 + w2 * 0.5f;
        float x = f32(x0);
        const int mid = s16(trunc(static_cast<double>(H * 0.5f - 1.0f)));
        c_.moveTo(trunc(x0), mid);
        const float back = (w2 - pw) * -2.0f;
        const float pw2 = pw + pw;
        const int a = s16(trunc(yHigh));
        const int b = s16(trunc(yLow));
        while (w2 > x) {
            const float next = x + w2;
            if (f_.shape >= 0) {
                c_.singleLineTo(s16(trunc(static_cast<double>(x) + 0.5)), a);
                int p = s16(trunc(static_cast<double>(x + pw) + 0.5));
                c_.singleLineTo(p, a);
                c_.singleLineTo(p, b);
                p = s16(trunc(static_cast<double>(x + pw2) + 0.5));
                c_.singleLineTo(p, b);
                c_.singleLineTo(p, mid);
                c_.singleLineTo(s16(trunc(next)), mid);
            } else {
                int p = s16(trunc(static_cast<double>(next + back) + 0.5));
                c_.singleLineTo(p, mid);
                c_.singleLineTo(p, a);
                p = s16(trunc(static_cast<double>(x + pw) + 0.5));
                c_.singleLineTo(p, a);
                c_.singleLineTo(p, b);
                int q = s16(trunc(next));
                c_.singleLineTo(q, b);
                c_.singleLineTo(q, mid);
            }
            x = next;
        }
    }

    // DrawTweekTri (0x9ccf2): triangle with movable peak (shape).
    void tweakTri()
    {
        const float H = static_cast<float>(h_), W = static_cast<float>(w_);
        const float k = static_cast<float>(f_.shape) * 0.0078125f + 1.0f;
        const float w2 = W - 2.0f;
        const float q = w2 * 0.25f;
        float x = static_cast<float>(trunc(static_cast<double>((-w2 * static_cast<float>(f_.phase)) / 127.0f) + 0.5));
        const int mid = s16(trunc(static_cast<double>(H * 0.5f - 1.0f)));
        c_.moveTo(trunc(x), mid);
        const float half = q + q;
        const float qk = q * k;
        const int bottom = s16(trunc(H - 2.0f));
        while (w2 > x) {
            bool up = typeIn(f_.outType, 0x15), down = typeIn(f_.outType, 0x2a);
            if (up || down) {
                const float a = x + half;
                c_.singleLineTo(s16(trunc(static_cast<double>(a - qk) + 0.5)), up ? 1 : bottom);
                c_.singleLineTo(s16(trunc(static_cast<double>(a + qk) + 0.5)), up ? bottom : 1);
            }
            x = x + w2;
            c_.singleLineTo(s16(trunc(static_cast<double>(x) + 0.5)), mid);
        }
    }

    // DrawSqrTri (0x9c21c): trapezoid, shape moves the slopes; period rounded
    // down to a multiple of 4 pixels. Nothing for out types >= 6.
    void sqrTri()
    {
        const float H = static_cast<float>(h_), W = static_cast<float>(w_);
        const int mid = s16(trunc(static_cast<double>(H * 0.5f - 1.0f)));
        const float w2 = W - 2.0f;
        int period = trunc(w2);
        period -= period % 4;
        const int q = period / 4;
        int x = trunc(static_cast<double>((static_cast<float>(-period) * static_cast<float>(f_.phase)) / 127.0f) + 0.5);
        int yA, yB;
        if (typeIn(f_.outType, 0x15)) {
            yB = trunc(H - 2.0f);
            yA = 1;
        } else if (typeIn(f_.outType, 0x2a)) {
            yA = trunc(H - 2.0f);
            yB = 1;
        } else {
            return;
        }
        yA = s16(yA);
        yB = s16(yB);
        c_.moveTo(x, mid);
        const float k = (((static_cast<float>(f_.shape) + 128.0f) * 0.5f + 127.0f) / 127.0f) * 0.9f + 0.1f;
        const float a = static_cast<float>(q) * k;
        const int s1 = s16(trunc(static_cast<double>(a) + 0.5));
        const int s2 = s16(trunc(static_cast<double>(static_cast<float>(2 * q) - a) + 0.5));
        const int s3 = s16(trunc(static_cast<double>(static_cast<float>(2 * q) + a) + 0.5));
        const int s4 = s16(trunc(static_cast<double>(static_cast<float>(4 * q) - a) + 0.5));
        float xf = static_cast<float>(x);
        while (w2 > xf) {
            int last;
            if (k > 1.0f) {
                c_.singleLineTo(s16(x + s2), yA);
                c_.singleLineTo(s16(x + s1), yA);
                c_.singleLineTo(s16(x + s4), yB);
                last = x + s3;
            } else {
                c_.singleLineTo(s16(x + s1), yA);
                c_.singleLineTo(s16(x + s2), yA);
                c_.singleLineTo(s16(x + s3), yB);
                last = x + s4;
            }
            c_.singleLineTo(s16(last), yB);
            x += period;
            xf = static_cast<float>(x);
            c_.singleLineTo(s16(trunc(static_cast<double>(xf) + 0.5)), mid);
        }
    }

    // DrawNormalizedGraph (0x9d0cc): y = fn(t, shape) over one period.
    template <class Fn>
    void normalized(Fn fn)
    {
        const float H = static_cast<float>(h_), W = static_cast<float>(w_);
        const bool invert = typeIn(f_.outType, 0x2a);
        const double sh = static_cast<double>(f_.shape + 128) * 0.00390625 * 0.9 + 0.05;
        const double ph = static_cast<double>(f_.phase) / 127.0;
        const int hi = trunc(H);
        const double top = hi - 2, range = hi - 4;
        auto yOf = [&](float v) {
            double d = static_cast<double>(v);
            if (invert) {
                d = 1.0 - d;
            }
            return s16(trunc(f32(top - d * range)));
        };
        c_.moveTo(2, yOf(fn(ph, sh)));
        const float last = W - 2.0f, span = W - 4.0f;
        for (int x = 2; last >= static_cast<float>(x); ++x) {
            float v = fn(static_cast<double>(static_cast<float>(x - 2) / span) + ph, sh);
            c_.singleLineTo(s16(x), yOf(v));
        }
    }

    // DrawDsf (0x9c980): discrete summation formula wave, `mult` 1 or 2.
    void dsf(int mult)
    {
        const int h = h_;
        const double a = static_cast<double>(static_cast<float>(f_.shape) * 0.0078125f);
        const int half = s16(h / 2);
        c_.moveTo(0, half);
        const double na = a * -2.0, a2 = a * a;
        const double hh = static_cast<double>(static_cast<float>(half) * 0.5f);
        const float span = static_cast<float>(w_) - 2.0f;
        const int limit = s16(h - 1);
        for (int x = 0; x < w_ - 1; x = s16(x + 1)) {
            const double t = static_cast<double>(static_cast<float>(x) / span) * (2 * kPi);
            const double s = dsin(t);
            const double cm = dcos(t * static_cast<double>(mult));
            int v = s16(half + trunc(static_cast<double>(f32(s / (cm * na + 1.0 + a2) * hh)) + 0.5));
            if (v > limit) {
                v = limit;
            }
            if (v <= 2) {
                v = 2;
            }
            c_.singleLineTo(x, s16(h - v));
        }
    }

    // DrawDualSine (0x9d30e): two half sines of different widths (shape),
    // `offset` 0 or 0.25 of a period shifts the start.
    void dualSine(float offset)
    {
        const float H = static_cast<float>(h_), W = static_cast<float>(w_);
        int offs = 0;
        if (typeIn(f_.outType, 0x15)) {
            offs = 1;
        } else if (typeIn(f_.outType, 0x2a)) {
            offs = trunc(-(H - 1.0f));
        }
        double r = static_cast<double>(f_.shape) / 127.0 + 1.0;
        r = r > 0.1 ? r : 0.1;
        if (1.9 <= r) {
            r = 1.9;
        }
        const double w2 = static_cast<double>(W - 2.0f);
        const float len1 = f32(r * w2);
        const float len2 = f32(w2 * (2.0 - r));
        const double k1 = static_cast<double>(f32(2 * kPi / static_cast<double>(len1)));
        const double k2 = static_cast<double>(f32(2 * kPi / static_cast<double>(len2)));
        const double ph1 = trunc(static_cast<double>(-len1 * offset) + 0.5);
        const double ph2 =
            trunc(static_cast<double>(f32(static_cast<double>(-len2) * (static_cast<double>(offset) + 0.5))) + 0.5);
        const double amp = trunc(static_cast<double>((H - 3.0f) * 0.5f) + 0.5);
        const float phase0 =
            static_cast<float>(trunc(static_cast<double>((-W * static_cast<float>(f_.phase)) / 127.0f) + 0.5));
        const float c = fceil(offset * len2);
        const int start = trunc(static_cast<float>(s16(trunc(phase0 + c))) - W);
        const float n1 = static_cast<float>(trunc(static_cast<double>(len1 * 0.5f) + 0.5));
        const float n2 = static_cast<float>(trunc(static_cast<double>(len2 * 0.5f) + 0.5));
        c_.moveTo(start, trunc((H - 1.0f) * 0.5f));
        int next = start + 1;
        int x = s16(next);
        const float limit = W - 1.0f;
        while (static_cast<float>(x) < limit) {
            for (int i = 1; static_cast<float>(i) <= n1; ++i) {
                ++next;
                double y = std::fabs((1.0 - dsin((static_cast<double>(i) - ph1) * k1)) * amp + offs);
                c_.singleLineTo(x, s16(trunc(y)));
                x = s16(next);
            }
            for (int i = 1; static_cast<float>(i) <= n2; ++i) {
                ++next;
                double y = std::fabs((1.0 - dsin((static_cast<double>(i) - ph2) * k2)) * amp + offs);
                c_.singleLineTo(x, s16(trunc(y)));
                x = s16(next);
            }
        }
    }

    // DrawDoubleSaw (0x9fdc2): saw with a second, phase-shifted saw (shape),
    // drawn around a BackLine in the middle.
    void doubleSaw()
    {
        const float H = static_cast<float>(h_), W = static_cast<float>(w_);
        float t = (H - 3.0f) * 0.25f;
        const int amp = trunc(t + t);
        const int half = amp / 2;
        const float fhalf = static_cast<float>(half);
        float off = (static_cast<float>(f_.shape) / 127.0f - 1.0f) * fhalf;
        const float famp = static_cast<float>(amp);
        const double step = static_cast<double>(famp / W);
        const int base = amp + 1;
        c_.moveTo(1, s16(base));
        const float last = W - 2.0f;
        c_.backLineTo(s16(trunc(last)), s16(base));
        const float fbase = static_cast<float>(base);
        const float s = static_cast<float>(f_.shape) / 127.0f;
        float y0 = famp * 0.5f + fbase + (famp * std::fabs(s + s - 1.0f)) / 3.0f;
        c_.moveTo(1, trunc(y0));
        float cur = fhalf;
        const float wrap = static_cast<float>(-half);
        for (int x = 1; static_cast<float>(x) < last; ++x) {
            c_.singleLineTo(s16(x), s16(trunc(fbase - (cur + off))));
            if (wrap >= off) {
                c_.singleLineTo(s16(x), s16(trunc(fbase - (cur + fhalf))));
                off = fhalf;
            }
            cur = f32(static_cast<double>(cur) - step);
            off = f32(static_cast<double>(off) - step);
        }
    }

    // DrawRandom (0x9c0ce): random steps 5 px wide, from the global LCG
    // _gRandom (0x3189c8, starts at 0 and advances on every draw).
    void random(std::uint32_t& state)
    {
        const float H = static_cast<float>(h_), W = static_cast<float>(w_);
        const int mid = s16(trunc(static_cast<double>(H * 0.5f - 1.0f)));
        c_.moveTo(0, mid);
        c_.singleLineTo(1, mid);
        const float limit = W - 5.0f;
        const unsigned range = static_cast<unsigned>(trunc(H - 3.0f));
        int x = 1, step = 1;
        while (limit > static_cast<float>(x)) {
            state = state * 0xbb38435u + 0x3619636bu;
            const int y = s16(static_cast<int>(state % range) + 1);
            c_.singleLineTo(x, y);
            step += 5;
            x = s16(step);
            c_.singleLineTo(x, y);
        }
        c_.singleLineTo(x, mid);
        c_.singleLineTo(s16(trunc(W - 1.0f)), mid);
    }

private:
    Canvas& c_;
    const Wave& f_;
    const int w_, h_;
};

// Shape functions of LfoShpA (float (*)(double t, double shape)).
// Sin2Saw (0x9b364)
inline float sin2Saw(double t, double s)
{
    if (t > 1.0) {
        t -= 1.0;
    }
    const float v = f32(s > t ? t / s : 1.0 - (t - s) / (1.0 - s));
    return f32((1.0 - dcos(static_cast<double>(v) * kPi)) * 0.5);
}
// CosBell (0x96f4e)
inline float cosBell(double t, double s)
{
    double d = s * 0.5 + 0.5 + t;
    if (d > 2.0) {
        d -= 1.0;
    }
    if (d > 1.0) {
        d -= 1.0;
    }
    if (!(s > d)) {
        return 0.0f;
    }
    return f32(1.0 + (dcos(d * (2 * kPi) / s) + 1.0) * -0.5);
}
// TriBell (0x96fec)
inline float triBell(double t, double s)
{
    const double h = s * 0.5;
    double d = 0.5 + h + t;
    if (d > 2.0) {
        d -= 1.0;
    }
    if (d > 1.0) {
        d -= 1.0;
    }
    if (h > d) {
        return f32(d / s + d / s);
    }
    if (s > d) {
        return f32((d + s * -0.5) / s * -2.0 + 1.0);
    }
    return 0.0f;
}
// Tri2Saw (0x9708e)
inline float tri2Saw(double t, double s)
{
    if (t > 1.0) {
        t -= 1.0;
    }
    return f32(s > t ? t / s : 1.0 - (t - s) / (1.0 - s));
}

// Common frame of the waveform graphs: frame, background, `body`, blit.
template <class Body>
void paint(Canvas& c, const Wave& f, Body body)
{
    c.frame();
    const int w = c.W - 2, h = c.H - 2;
    c.rect(0, 0, w, h, Ink::Back);
    Painter p(c, f);
    body(p);
    c.blit(1, 1, w, h);
}

// OscShpB / OscShpA wave selector (CPnlOscCGraph::Draw 0x9ffbe,
// CPnlOscSinShapeGraph::Draw 0x9fc20).
inline void oscWave(Painter& p, const Wave& f, bool withSaw)
{
    switch (f.wave) {
    case 0: p.dualSine(0.25f); break;
    case 1: p.dualSine(0.0f); break;
    case 2: p.dsf(1); break;
    case 3: p.dsf(2); break;
    case 4: p.tweakTri(); break;
    case 5:
        if (withSaw) {
            p.doubleSaw();
        } else {
            p.pulse();
        }
        break;
    case 6:
        if (withSaw) {
            p.square();
            break;
        }
        p.blank();
        break;
    case 7:
        if (withSaw) {
            p.pulse();
            break;
        }
        p.blank();
        break;
    default: p.blank(); break;
    }
}

} // namespace osc

// 18 CPnlOscCGraph (OscShpB): UpdateGraphics 0x970ec, Draw 0x9ffbe.
// values: Shape, Wave (mode).
template <>
void drawGraph<18>(Canvas& c, Values v)
{
    osc::Wave f;
    f.wave = dep(v, 1);
    f.shape = dep(v, 0);
    osc::paint(c, f, [&](osc::Painter& p) { osc::oscWave(p, f, true); });
}

// 32 CPnlOscSinShapeGraph (OscShpA): UpdateGraphics 0x97158, Draw 0x9fc20.
// values: Shape, Wave.
template <>
void drawGraph<32>(Canvas& c, Values v)
{
    osc::Wave f;
    f.wave = dep(v, 1);
    f.shape = dep(v, 0);
    osc::paint(c, f, [&](osc::Painter& p) { osc::oscWave(p, f, false); });
}

// 33 CPnlLfoBGraph (LfoB): UpdateGraphics 0x972cc, Draw 0xa03b0.
// values: Wave, Phase, OutType.
template <>
void drawGraph<33>(Canvas& c, Values v)
{
    osc::Wave f;
    f.wave = dep(v, 0);
    f.shape = 0;
    f.phase = dep(v, 1);
    f.outType = dep(v, 2);
    osc::paint(c, f, [&](osc::Painter& p) {
        p.backLine();
        switch (f.wave) {
        case 0: p.sine(); break;
        case 1: p.tri(); break;
        case 2: p.saw(); break;
        case 3: p.square(); break;
        default: p.blank(); break;
        }
    });
}

// 34 CPnlLfoCGraph (LfoShpA): UpdateGraphics 0x97358, Draw 0xa020e.
// values: Wave, Shape, Phase, OutType.
template <>
void drawGraph<34>(Canvas& c, Values v)
{
    osc::Wave f;
    f.wave = dep(v, 0);
    f.shape = s16(dep(v, 1) * 2 - 128);
    f.phase = dep(v, 2);
    f.outType = dep(v, 3);
    osc::paint(c, f, [&](osc::Painter& p) {
        p.backLine();
        switch (f.wave) {
        case 0: p.normalized(osc::sin2Saw); break;
        case 1: p.normalized(osc::cosBell); break;
        case 2: p.normalized(osc::triBell); break;
        case 3: p.normalized(osc::tri2Saw); break;
        case 4: p.sqrTri(); break;
        case 5: p.square(); break;
        default: p.blank(); break;
        }
    });
}

// 24 CPnlLfoGraph (no panel uses it): UpdateGraphics 0x9722e, Draw 0xa053e.
// values: Shape, Phase, OutType, Wave.
template <>
void drawGraph<24>(Canvas& c, Values v)
{
    osc::Wave f;
    f.wave = dep(v, 3);
    f.shape = s16(dep(v, 0) * 2 - 128);
    f.phase = dep(v, 1);
    f.outType = dep(v, 2);
    std::uint32_t seed = 0; // _gRandom as after start-up
    osc::paint(c, f, [&](osc::Painter& p) {
        p.backLine();
        switch (f.wave) {
        case 0: p.sine(); break;
        case 1: p.dualSine(0.25f); break;
        case 2: p.dualSine(0.5f); break;
        case 3: p.tri(); break;
        case 4: p.tweakTri(); break;
        case 5: p.square(); break;
        case 6: p.pulse(); break;
        case 7: p.saw(); break;
        case 8: p.sqrTri(); break;
        case 9: p.blank(); break;
        case 10: p.random(seed); break;
        default: break;
        }
    });
}

// --- pulse oscillator, DX algorithm, noise width, random graphs -------------------

// 42 CPnlDXRouterGraph (Draw 0x976fe): the algorithm picture, one 137x65 cell
// per algorithm stacked vertically (66 px apart) in bitmap resource 993.
template <>
void drawGraph<42>(Canvas& c, Values v)
{
    const int algorithm = dep(v, 0);
    c.frame();
    c.bitmap(993, 1, 1, 0, algorithm * 66, 137, 65);
}

// 43 CPnlTunedNoiseGraph (UpdateGraphics 0x977ec, Draw 0x9e558): a band-pass
// bump on the base line whose width follows OscNoise "Width" (dep 0).
template <>
void drawGraph<43>(Canvas& c, Values v)
{
    const u8 width = dep(v, 0); // +0x84
    const float hf = static_cast<float>(c.H), wf = static_cast<float>(c.W);
    const int iw = c.W - 2, ih = c.H - 2; // interior
    c.frame();
    c.rect(0, 0, iw, ih, Ink::Back);

    const double base = static_cast<double>(hf - 4.0f);
    const int y0 = trunc(base + 0.5);            // base line
    const float peak = f32(base * 0.9);          // bump height
    const float k = (wf - 6.0f) * 0.0078125f;    // px per value step
    const i16 y = s16(y0);
    c.moveTo(0, y);
    const int right = trunc(wf);
    c.backLineTo(right, y);

    const double centre = static_cast<double>(k * 64.0f + 1.0f);
    const double spread = static_cast<double>(k * static_cast<float>(width));
    const int mid = s16(trunc(centre + 0.5));
    const int hi = trunc(static_cast<double>(f32(spread * 0.7 + centre + 4.0)) + 0.5);
    const int lo = s16(trunc(static_cast<double>(f32(centre + spread * -0.7 - 4.0)) + 0.5));
    const int p0 = trunc(static_cast<double>(lo + (mid - lo) / 2) + 0.5);
    const int top = trunc(static_cast<double>(f32(static_cast<double>(y0) - static_cast<double>(peak))) + 0.5);
    const int p3 = trunc(static_cast<double>(mid + (s16(hi) - mid) / 2) + 0.5);
    const Pt pts[6] = {{s16(p0), y}, {s16(p0), y}, {s16(mid), s16(top)},
                       {s16(p3), y}, {s16(p3), y}, {s16(hi), y}};
    c.moveTo(-1, y);
    c.borderLineTo(lo, y);
    c.borderPolyBezierTo(pts, 6);
    c.borderLineTo(right, y);
    c.fillUnder(iw, ih);
    c.blit(1, 1, iw, ih);
}

// 44 CPnlRndDistributionGraph (UpdateGraphics 0x97840, Draw 0x97894; no panel
// uses it): bars 3 px wide every 2 px, drawn as frames on the view. Bar
// height mixes a parabola (d^2) and its complement by the value p = v/128.
template <>
void drawGraph<44>(Canvas& c, Values v)
{
    const float p = static_cast<float>(dep(v, 0)) * 0.0078125f;
    const float q = 1.0f - p;
    c.frame();
    const int l = 1, t = 1, r = c.W - 1, b = c.H - 1; // shrunk view rect
    c.viewRect(l, t, r, b, Ink::Back);
    const int height = s16(b - t);
    const int bars = c.W / 2 - 1;
    int x = l;
    for (int i = 0; i < bars; ++i) {
        float d = (static_cast<float>(x + 1) - static_cast<float>((l + r) / 2)) / static_cast<float>((r - l) / 2);
        d = d * d;
        const float level = p * d + (1.0f - d) * q;
        const int top = s16(trunc(static_cast<float>(b) - level * static_cast<float>(height)));
        c.viewFrame(x, top, x + 3, b, Ink::Single);
        x += 2;
    }
}

// 45 CPnlRndTrigGraph (UpdateGraphics 0x97a4e, Draw 0x97aa2): a base line and
// up to ten vertical "trigger" lines, every 2 px, appearing one by one (in a
// scattered order) as Prob (dep 0) passes fixed thresholds. Drawn on the view.
template <>
void drawGraph<45>(Canvas& c, Values v)
{
    // value must exceed threshold[k] for the line at x = left + 2k
    static constexpr int kThreshold[10] = {120, 96, 24, 60, 36, 0, 72, 108, 48, 84};
    const u8 prob = dep(v, 0);
    c.frame();
    const int l = 1, t = 1, r = c.W - 1, b = c.H - 1;
    c.viewRect(l, t, r, b, Ink::Back);
    c.hline(l, b - 1, r - 1, Ink::Single, true);
    for (int k = 0; k < 10; ++k) {
        if (prob > kThreshold[k]) {
            c.vline(l + 2 * k, t + 1, b - 1, Ink::Single, true);
        }
    }
}

// 19 CPnlPulseOscGraph (UpdateGraphics 0x971c4, ctor 0x98954, Draw 0x9fa0e):
// one period of the selected waveform; dep 1 = wave (+0x84), dep 0 = shape
// (+0x85, default 64). Unmapped panel (PulseOsc), never shown by the editor.
namespace misc {

struct PulseOsc {
    Canvas& c;
    u8 shape;
    int w, h; // shrunk view rect

    static constexpr double kTwoPi = 6.283185307179586;

    // DrawSine (0x9f81e)
    void sine()
    {
        const float wf = static_cast<float>(w), hf = static_cast<float>(h);
        const float period = wf - 1.0f;
        const double phase = trunc(static_cast<double>(period * -32.0f * 0.0078125f) + 0.5);
        const double amp = trunc(static_cast<double>((hf - 3.0f) * 0.5f) + 0.5);
        const double k = static_cast<double>(f32(kTwoPi / static_cast<double>(period)));
        const int y0 = s16(trunc(static_cast<double>(f32((dsin(phase * k) + 1.0) * amp)) + 0.5));
        c.moveTo(0, y0);
        c.backLineTo(trunc(period), y0);
        c.moveTo(0, y0);
        for (int i = 1; wf > static_cast<float>(i); ++i) {
            c.singleLineTo(i, trunc((1.0 - dsin((static_cast<double>(i) - phase) * k)) * amp));
        }
    }
    // DrawModSine (0x9f512): two half periods of different lengths.
    void modSine()
    {
        const float wf = static_cast<float>(w), hf = static_cast<float>(h);
        const double s = static_cast<double>(shape) * 0.015625;
        const double span = static_cast<double>(wf - 2.0f);
        const float p1 = f32((2.0 - s) * span);
        const double ph1 = trunc(static_cast<double>(p1 * -32.0f * 0.0078125f) + 0.5);
        const double amp = trunc(static_cast<double>((hf - 3.0f) * 0.5f) + 0.5);
        const double k1 = static_cast<double>(f32(kTwoPi / static_cast<double>(p1)));
        const int y0 = s16(trunc(static_cast<double>(f32((dsin(ph1 * k1) + 1.0) * amp)) + 0.5));
        c.moveTo(0, y0);
        c.backLineTo(trunc(wf - 1.0f), y0);
        c.moveTo(0, y0);
        int i = 1;
        while (0.5f * p1 > static_cast<float>(i)) {
            ++i;
            c.singleLineTo(i - 1, trunc((1.0 - dsin((static_cast<double>(i) - ph1) * k1)) * amp));
        }
        const float p2 = std::max(f32(s * span), 3.0f);
        const double ph2 = trunc(static_cast<double>(p2 * -96.0f * 0.0078125f) + 0.5);
        const double k2 = static_cast<double>(f32(kTwoPi / static_cast<double>(p2)));
        int x = i, end = i;
        for (int j = 1; 0.5f * p2 > static_cast<float>(j); ++j, ++x) {
            ++end;
            c.singleLineTo(x, trunc((1.0 - dsin((static_cast<double>(j) - ph2) * k2)) * amp));
        }
        c.singleLineTo(end, y0);
    }
    // DrawTri (0x9f07a) and DrawTweekTri (0x9ee9c, peaks moved by the shape).
    void tri(bool tweak)
    {
        const float wf = static_cast<float>(w), hf = static_cast<float>(h);
        const float period = wf - 2.0f;
        float x = 0.0f;
        const float q = period * 0.25f;
        const int mid = s16(trunc(static_cast<double>(hf * 0.5f - 1.0f)));
        c.moveTo(0, mid);
        c.backLineTo(trunc(wf - 1.0f), mid);
        c.moveTo(0, mid);
        const int bottom = s16(trunc(hf - 3.0f));
        if (tweak) {
            const float t = q * f32(static_cast<double>(shape + 1) * 0.015625);
            const float q4 = q * 4.0f;
            while (period > x) {
                c.singleLineTo(trunc(static_cast<double>(x + t) + 0.5), 0);
                c.singleLineTo(trunc(static_cast<double>(x + q4 - t) + 0.5), bottom);
                x += period;
                c.singleLineTo(trunc(static_cast<double>(x) + 0.5), mid);
            }
        } else {
            const float q3 = q * 3.0f;
            while (period > x) {
                c.singleLineTo(trunc(static_cast<double>(x + q) + 0.5), 0);
                c.singleLineTo(trunc(static_cast<double>(x + q3) + 0.5), bottom);
                x += period;
                c.singleLineTo(trunc(static_cast<double>(x) + 0.5), mid);
            }
        }
    }
    // DrawSquare (0x9bf66): pulse width = shape/128 of the period.
    void square()
    {
        const float hf = static_cast<float>(h);
        const float period = static_cast<float>(w) - 2.0f;
        float x = 0.0f;
        const float pw = 0.0078125f * (period * static_cast<float>(shape));
        const int bottom = s16(trunc(hf - 4.0f));
        c.moveTo(0, bottom);
        while (period > x) {
            c.singleLineTo(trunc(static_cast<double>(x) + 0.5), 1);
            const int e = s16(trunc(static_cast<double>(x + pw) + 0.5));
            c.singleLineTo(e, 1);
            c.singleLineTo(e, bottom);
            x += period;
            c.singleLineTo(trunc(static_cast<double>(x) + 0.5), bottom);
        }
    }
    // DrawTweekSaw (0x9ece2): a saw whose reset point moves with the shape.
    void tweakSaw()
    {
        const float wf = static_cast<float>(w), hf = static_cast<float>(h);
        const float h3 = hf - 3.0f;
        const int range = trunc(h3 * 0.25f + h3 * 0.25f);
        const int mid = trunc(h3 * 0.5f);
        const int half = range / 2;
        const float halfF = static_cast<float>(half);
        float b = f32((static_cast<double>(shape) * 0.015625 - 1.0) * static_cast<double>(halfF));
        const double step = static_cast<double>(static_cast<float>(range) / wf);
        const int y = s16(mid);
        c.moveTo(1, y);
        const float end = wf - 2.0f;
        c.backLineTo(trunc(end), y);
        c.moveTo(1, y);
        float a = halfF;
        const float midF = static_cast<float>(mid);
        const float negHalf = static_cast<float>(-half);
        for (int i = 1; end > static_cast<float>(i); ++i) {
            c.singleLineTo(i, trunc(midF - (a + b)));
            double bd = b;
            if (negHalf >= b) {
                c.singleLineTo(i, trunc(midF - (a + halfF)));
                bd = halfF;
            }
            a = f32(static_cast<double>(a) - step);
            b = f32(bd - step);
        }
    }
    // DrawSaw (0x9bcc4)
    void saw()
    {
        const float period = static_cast<float>(w) - 2.0f;
        float x = 0.0f;
        const int bottom = s16(trunc(static_cast<float>(h) - 3.0f));
        c.moveTo(0, bottom);
        while (period > x) {
            c.singleLineTo(trunc(static_cast<double>(x) + 0.5), 0);
            x += period;
            c.singleLineTo(trunc(static_cast<double>(x) + 0.5), bottom);
        }
    }
};

} // namespace misc

template <>
void drawGraph<19>(Canvas& c, Values v)
{
    const u8 shape = dep(v, 0); // +0x85 (ctor default 64, always overwritten)
    const u8 wave = dep(v, 1);                    // +0x84
    c.frame();
    const int iw = c.W - 2, ih = c.H - 2;
    c.rect(0, 0, iw, ih, Ink::Back);
    misc::PulseOsc g{c, shape, iw, ih};
    switch (wave) {
    case 0: g.sine(); break;
    case 1: g.modSine(); break;
    case 2: g.tri(false); break;
    case 3: g.square(); break;
    case 4: g.tweakSaw(); break;
    case 5: g.saw(); break;
    case 6: g.tri(true); break;
    default: break;
    }
    c.blit(1, 1, iw, ih);
}


// --- dispatch -------------------------------------------------------------------

struct GraphInfo {
    const char* name;
    short w, h; // constructor size (CView::SetSize)
    bool graph;
};

// CCustomObjectFactory::CreateCustom (0x87736), ids 0..46.
constexpr GraphInfo kGraphs[47] = {
    {"CPnlGraphABC", 0, 0, false},
    {"CPnlADEnvGraph", 45, 24, true},
    {"CPnlSeqClrRnd", 0, 0, false},
    {"CPnlADSRGraph", 61, 28, true},
    {"CPnlModEnvGraph", 61, 28, true},
    {"CPnlAREnvGraph", 45, 24, true},
    {"CPnlDEnvGraph", 31, 22, true},
    {"CPnlHEnvGraph", 31, 22, true},
    {"CPnlVocoderGraph", 194, 47, true},
    {"CPnlEqHiLoGraph", 52, 28, true},
    {"CPnlEqMidGraph", 52, 28, true},
    {"CPnlAmpProcGraph", 56, 24, true},
    {"CPnlXMuxGraph", 34, 22, true},
    {"CPnlPhaserGraph", 52, 28, true},
    {"CPnlDistAGraph", 34, 22, true},
    {"CPnlDistBGraph", 34, 22, true},
    {"CPnlWrapGraph", 34, 22, true},
    {"CPnlMultiEnvGraph", 84, 28, true},
    {"CPnlOscCGraph", 35, 22, true},
    {"CPnlPulseOscGraph", 35, 20, true},
    {"CPnlClassicFilterGraph", 52, 28, true},
    {"CPnlNormalFilterGraph", 52, 28, true},
    {"CPnlVocoderPreset", 0, 0, false},
    {"CPnlADBDSREnvGraph", 84, 28, true},
    {"CPnlLfoGraph", 35, 20, true},
    {"CPnlNoteSeqZoom", 0, 0, false},
    {"CPnlNoteSeqOffset", 0, 0, false},
    {"CPnlNoteSeqClrRnd", 0, 0, false},
    {"CPnlAHDEnvGraph", 58, 28, true},
    {"CPnlDXEnvGraph", 85, 28, true},
    {"CPnlSmallFilterGraph", 32, 22, true},
    {"CPnlSmallFilterGraph", 32, 22, true},
    {"CPnlOscSinShapeGraph", 38, 22, true},
    {"CPnlLfoBGraph", 52, 28, true},
    {"CPnlLfoCGraph", 52, 28, true},
    {"CPnlCombFltGraph", 52, 28, true},
    {"CPnlEqHiLoGraph", 52, 28, true},
    {"CPnlEqHiMidLoGraph", 52, 28, true},
    {"CPnlShapeBGraph", 34, 22, true},
    {"CPnlSaturateGraph", 34, 22, true},
    {"CPnlStaticFilterGraph", 52, 28, true},
    {"CPnlOperatorGraph", 63, 24, true},
    {"CPnlDXRouterGraph", 139, 67, true},
    {"CPnlTunedNoiseGraph", 31, 22, true},
    {"CPnlRndDistributionGraph", 35, 22, true},
    {"CPnlRndTrigGraph", 21, 22, true},
    {"CPnlDrumPresetSelector", 0, 0, false},
};

using DrawFn = void (*)(Canvas&, Values);

template <std::size_t... I>
constexpr std::array<DrawFn, sizeof...(I)> makeTable(std::index_sequence<I...>)
{
    return {&drawGraph<static_cast<int>(I)>...};
}
constexpr auto kDraw = makeTable(std::make_index_sequence<47>{});

} // namespace

Rgb color(Ink ink)
{
    switch (ink) {
    case Ink::Frame: return {100, 100, 100};
    case Ink::Back: return {0, 128, 128};
    case Ink::BackLine: return {192, 192, 192};
    case Ink::Border: return {0, 0, 0};
    case Ink::Single: return {0, 255, 0};
    case Ink::Base: return {255, 255, 0};
    case Ink::Shade: return {75, 99, 99};
    case Ink::Fill: return {0, 255, 128};
    case Ink::EnvFill: return {0, 164, 164};
    case Ink::Text: return {255, 255, 0};
    }
    return {0, 0, 0};
}

std::pair<int, int> defaultSize(int graphFunc)
{
    if (graphFunc < 0 || graphFunc > 46) {
        return {0, 0};
    }
    return {kGraphs[graphFunc].w, kGraphs[graphFunc].h};
}

bool isGraph(int graphFunc) { return graphFunc >= 0 && graphFunc <= 46 && kGraphs[graphFunc].graph; }

const char* className(int graphFunc)
{
    if (graphFunc < 0 || graphFunc > 46) {
        return "";
    }
    return kGraphs[graphFunc].name;
}

Drawing render(int graphFunc, std::span<const std::uint8_t> values, int width, int height)
{
    Drawing d;
    auto [w0, h0] = defaultSize(graphFunc);
    d.width = width > 0 ? width : w0;
    d.height = height > 0 ? height : h0;
    if (!isGraph(graphFunc)) {
        return d;
    }
    Canvas c(d.width, d.height, d.ops);
    kDraw[static_cast<std::size_t>(graphFunc)](c, values);
    return d;
}

// --- rasterisation --------------------------------------------------------------

namespace {

struct Bitmap {
    int w, h;
    std::vector<Ink> px;
    Bitmap(int w_, int h_, Ink fillInk) : w(w_), h(h_), px(static_cast<std::size_t>(w_ * h_), fillInk) {}
    bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    Ink& at(int x, int y) { return px[static_cast<std::size_t>(y * w + x)]; }
    void set(int x, int y, Ink ink)
    {
        if (in(x, y)) {
            at(x, y) = ink;
        }
    }
};

void rasterLine(Bitmap& b, int x0, int y0, int x1, int y1, Ink ink)
{
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        b.set(x0, y0, ink);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// CPnlGraphABC::FillUnderGraph (0x95572): in each column, everything below the
// first Border/Shade pixel that is not Border/Shade becomes Fill.
void rasterFillUnder(Bitmap& b, int w, int h)
{
    for (int x = 0; x < std::min(w, b.w); ++x) {
        bool seen = false;
        for (int y = 0; y < std::min(h, b.h); ++y) {
            Ink& p = b.at(x, y);
            bool edge = p == Ink::Border || p == Ink::Shade;
            if (seen && !edge) {
                p = Ink::Fill;
            }
            if (edge) {
                seen = true;
            }
        }
    }
}

// CPnlGraphABC::FillEnvelopeGraph (0x95748): in each column, the pixels between
// the curve (Single, or Base = kGraphSustainColor) and the BackLine.
void rasterFillEnvelope(Bitmap& b, int w, int h)
{
    for (int x = 0; x < std::min(w, b.w); ++x) {
        int state = 0, start = 0, end = 0;
        for (int y = 0; y < std::min(h, b.h); ++y) {
            Ink p = b.at(x, y);
            if (p == Ink::Single || p == Ink::Base || p == Ink::Text) {
                if (state == 0) {
                    state = 1;
                    start = end = y;
                } else if (state == 2) {
                    end = y;
                }
            }
            if (p == Ink::BackLine) {
                if (state == 0) {
                    state = 2;
                    start = end = y;
                } else if (state == 1) {
                    end = y;
                }
            }
        }
        for (int y = start; y < end; ++y) {
            Ink& p = b.at(x, y);
            if (p != Ink::Single && p != Ink::Base && p != Ink::Text && p != Ink::BackLine) {
                p = Ink::EnvFill;
            }
        }
    }
}

} // namespace

std::vector<Ink> rasterize(const Drawing& d)
{
    Bitmap scratch(200, 100, Ink::Back);
    Bitmap view(std::max(d.width, 0), std::max(d.height, 0), Ink::Back);
    for (const Op& o : d.ops) {
        switch (o.kind) {
        case Op::Kind::Frame:
            for (int x = o.x0; x < o.x1; ++x) {
                view.set(x, o.y0, o.ink);
                view.set(x, o.y1 - 1, o.ink);
            }
            for (int y = o.y0; y < o.y1; ++y) {
                view.set(o.x0, y, o.ink);
                view.set(o.x1 - 1, y, o.ink);
            }
            break;
        case Op::Kind::Rect:
            for (int y = o.y0; y < o.y1; ++y) {
                for (int x = o.x0; x < o.x1; ++x) {
                    (o.view ? view : scratch).set(x, y, o.ink);
                }
            }
            break;
        case Op::Kind::Line:
            rasterLine(o.view ? view : scratch, o.x0, o.y0, o.x1, o.y1, o.ink);
            break;
        case Op::Kind::FillUnder:
            rasterFillUnder(scratch, o.x1, o.y1);
            break;
        case Op::Kind::FillEnvelope:
            rasterFillEnvelope(scratch, o.x1, o.y1);
            break;
        case Op::Kind::Blit:
            for (int y = 0; y < o.h; ++y) {
                for (int x = 0; x < o.w; ++x) {
                    if (scratch.in(o.x1 + x, o.y1 + y)) {
                        view.set(o.x0 + x, o.y0 + y, scratch.at(o.x1 + x, o.y1 + y));
                    }
                }
            }
            break;
        case Op::Kind::Text:
        case Op::Kind::Bitmap:
            break;
        }
    }
    return std::move(view.px);
}

// --- vector form ------------------------------------------------------------------

namespace {

using PointF = std::pair<float, float>;

// Liang-Barsky clip of a segment to [x0,x1] x [y0,y1].
bool clipSegment(PointF& a, PointF& b, float x0, float y0, float x1, float y1)
{
    float t0 = 0.0f, t1 = 1.0f;
    float dx = b.first - a.first, dy = b.second - a.second;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {a.first - x0, x1 - a.first, a.second - y0, y1 - a.second};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) {
                return false;
            }
            continue;
        }
        float r = q[i] / p[i];
        if (p[i] < 0.0f) {
            t0 = std::max(t0, r);
        } else {
            t1 = std::min(t1, r);
        }
        if (t0 > t1) {
            return false;
        }
    }
    PointF na{a.first + t0 * dx, a.second + t0 * dy};
    PointF nb{a.first + t1 * dx, a.second + t1 * dy};
    a = na;
    b = nb;
    return true;
}

// Column spans of `ink` merged into staircase polygons.
void fillPolygons(const std::vector<Ink>& px, int w, int h, Ink ink, std::vector<Polyline>& out)
{
    struct Strip {
        std::vector<PointF> top, bottom;
        int lastTop, lastBottom, lastX;
    };
    std::vector<Strip> open, done;
    for (int x = 0; x < w; ++x) {
        std::vector<std::pair<int, int>> spans;
        for (int y = 0; y < h;) {
            if (px[static_cast<std::size_t>(y * w + x)] != ink) {
                ++y;
                continue;
            }
            int y0 = y;
            while (y < h && px[static_cast<std::size_t>(y * w + x)] == ink) {
                ++y;
            }
            spans.emplace_back(y0, y);
        }
        std::vector<Strip> next;
        std::vector<bool> used(open.size(), false);
        for (auto [t, b] : spans) {
            int found = -1;
            for (std::size_t i = 0; i < open.size(); ++i) {
                if (!used[i] && open[i].lastX == x - 1 && t < open[i].lastBottom && b > open[i].lastTop) {
                    found = static_cast<int>(i);
                    break;
                }
            }
            Strip s;
            if (found >= 0) {
                used[static_cast<std::size_t>(found)] = true;
                s = std::move(open[static_cast<std::size_t>(found)]);
            }
            const float fx = static_cast<float>(x);
            s.top.emplace_back(fx, static_cast<float>(t));
            s.top.emplace_back(fx + 1.0f, static_cast<float>(t));
            s.bottom.emplace_back(fx, static_cast<float>(b));
            s.bottom.emplace_back(fx + 1.0f, static_cast<float>(b));
            s.lastTop = t;
            s.lastBottom = b;
            s.lastX = x;
            next.push_back(std::move(s));
        }
        for (std::size_t i = 0; i < open.size(); ++i) {
            if (!used[i]) {
                done.push_back(std::move(open[i]));
            }
        }
        open = std::move(next);
    }
    for (auto& s : open) {
        done.push_back(std::move(s));
    }
    for (auto& s : done) {
        Polyline p;
        p.ink = ink;
        p.closed = true;
        p.filled = true;
        p.points = std::move(s.top);
        p.points.insert(p.points.end(), s.bottom.rbegin(), s.bottom.rend());
        // drop collinear duplicates on horizontal runs
        std::vector<PointF> simple;
        for (const auto& q : p.points) {
            if (simple.size() >= 2) {
                const auto& a = simple[simple.size() - 2];
                const auto& b = simple.back();
                if ((a.second == b.second && b.second == q.second) || (a.first == b.first && b.first == q.first)) {
                    simple.back() = q;
                    continue;
                }
            }
            if (simple.empty() || simple.back() != q) {
                simple.push_back(q);
            }
        }
        p.points = std::move(simple);
        out.push_back(std::move(p));
    }
}

} // namespace

std::vector<Polyline> toPolylines(const Drawing& d)
{
    std::vector<Polyline> out;
    const float W = static_cast<float>(d.width), H = static_cast<float>(d.height);
    // background
    {
        Polyline bg;
        bg.ink = Ink::Back;
        bg.closed = bg.filled = true;
        bg.points = {{0.0f, 0.0f}, {W, 0.0f}, {W, H}, {0.0f, H}};
        out.push_back(std::move(bg));
    }
    // visible part of the scratch bitmap: view (bx, by, bw, bh) shows scratch
    // pixels shifted by (dx, dy)
    int bx = 1, by = 1, bw = d.width - 2, bh = d.height - 2, dx = 1, dy = 1;
    for (const Op& o : d.ops) {
        if (o.kind == Op::Kind::Blit) {
            bx = o.x0, by = o.y0, bw = o.w, bh = o.h;
            dx = o.x0 - o.x1, dy = o.y0 - o.y1;
        }
    }
    // fills, from the raster (exact column spans)
    {
        bool fills = false;
        for (const Op& o : d.ops) {
            fills = fills || o.kind == Op::Kind::FillUnder || o.kind == Op::Kind::FillEnvelope ||
                    o.kind == Op::Kind::Rect;
        }
        if (fills) {
            std::vector<Ink> px = rasterize(d);
            for (Ink ink : {Ink::Shade, Ink::Fill, Ink::EnvFill}) {
                fillPolygons(px, d.width, d.height, ink, out);
            }
        }
    }
    // lines, joined into polylines, in view coordinates through pixel centres
    const float cx0 = static_cast<float>(bx), cy0 = static_cast<float>(by);
    const float cx1 = static_cast<float>(bx + bw), cy1 = static_cast<float>(by + bh);
    Polyline cur;
    auto flush = [&] {
        if (!cur.points.empty()) {
            out.push_back(std::move(cur));
        }
        cur = Polyline{};
    };
    for (const Op& o : d.ops) {
        if (o.kind != Op::Kind::Line) {
            continue;
        }
        const int ox = o.view ? 0 : dx, oy = o.view ? 0 : dy;
        PointF a{static_cast<float>(o.x0 + ox) + 0.5f, static_cast<float>(o.y0 + oy) + 0.5f};
        PointF b{static_cast<float>(o.x1 + ox) + 0.5f, static_cast<float>(o.y1 + oy) + 0.5f};
        PointF ca = a, cb = b;
        const bool visible = o.view ? clipSegment(ca, cb, 0.0f, 0.0f, W, H) : clipSegment(ca, cb, cx0, cy0, cx1, cy1);
        if (!visible) {
            flush();
            continue;
        }
        bool joins = !cur.points.empty() && cur.ink == o.ink && cur.points.back() == ca;
        if (!joins) {
            flush();
            cur.ink = o.ink;
            cur.points.push_back(ca);
        }
        if (cur.points.back() != cb) {
            cur.points.push_back(cb);
        }
        if (cb != b) {
            flush(); // clipped: the path leaves the visible area
        }
    }
    flush();
    // frames (the outer frame, and rectangle outlines drawn on the view)
    for (const Op& o : d.ops) {
        if (o.kind == Op::Kind::Frame) {
            Polyline f;
            f.ink = o.ink;
            f.closed = true;
            const float l = static_cast<float>(o.x0) + 0.5f, t = static_cast<float>(o.y0) + 0.5f;
            const float r = static_cast<float>(o.x1) - 0.5f, b = static_cast<float>(o.y1) - 0.5f;
            f.points = {{l, t}, {r, t}, {r, b}, {l, b}};
            out.push_back(std::move(f));
        }
    }
    return out;
}

std::vector<Polyline> draw(int graphFunc, std::span<const std::uint8_t> values, float width, float height)
{
    return toPolylines(render(graphFunc, values, static_cast<int>(std::lround(width)),
                              static_cast<int>(std::lround(height))));
}

} // namespace g2::graphs
