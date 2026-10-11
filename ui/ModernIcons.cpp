#include "ModernIcons.h"

#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <utility>

namespace g2ui::icons {
namespace {

constexpr float kPi = juce::MathConstants<float>::pi;

// ---- Which controls show which pictures ------------------------------------------

using O = Option;

// Waveforms.
constexpr O kOscAWaves[] = {{Icon::Sine, "Sine"}, {Icon::Tri, "Triangle"}, {Icon::Saw, "Saw"},
                            {Icon::Square, "Square"}, {Icon::Pulse25, "Pulse 25%"}, {Icon::Pulse10, "Pulse 10%"}};
constexpr O kOscBWaves[] = {{Icon::Sine, "Sine"}, {Icon::Tri, "Triangle"}, {Icon::Saw, "Saw"},
                            {Icon::Square, "Square"}, {Icon::DualSaw, "Dual saw"}};
constexpr O kOscShpAWaves[] = {{Icon::Sine, "Sine 1"}, {Icon::Sine2, "Sine 2"}, {Icon::Sine3, "Sine 3"},
                               {Icon::Sine4, "Sine 4"}, {Icon::SawTri, "TriSaw"}, {Icon::SymPulse, "Pulse"}};
constexpr O kOscShpBWaves[] = {{Icon::Sine, "Sine 1"}, {Icon::Sine2, "Sine 2"}, {Icon::Sine3, "Sine 3"},
                               {Icon::Sine4, "Sine 4"}, {Icon::SawTri, "TriSaw"}, {Icon::DoubleSaw, "Double saw"},
                               {Icon::Square, "Pulse"}, {Icon::SymPulse, "Symmetric pulse"}};
constexpr O kOscPMWaves[] = {{Icon::Sine, "Sine"}, {Icon::Tri, "Triangle"}};
constexpr O kLfoAWaves[] = {{Icon::Sine, "Sine"}, {Icon::Tri, "Triangle"}, {Icon::Saw, "Saw"},
                            {Icon::Square, "Square"}, {Icon::RndStep, "Random step"}, {Icon::Rnd, "Random"}};
constexpr O kLfoBWaves[] = {{Icon::Sine, "Sine"}, {Icon::Tri, "Triangle"}, {Icon::Saw, "Saw"}, {Icon::Square, "Square"}};
// LfoC's last two pictures are pulse trains (its help text speaks of six waveforms).
constexpr O kLfoCWaves[] = {{Icon::Sine, "Sine"}, {Icon::Tri, "Triangle"}, {Icon::Saw, "Saw"},
                            {Icon::Square, "Square"}, {Icon::RndStep, "Random step"}, {Icon::Rnd, "Random"},
                            {Icon::RndPulses, "Pulses"}, {Icon::RoundPulses, "Rounded pulses"}};
constexpr O kLfoShpAWaves[] = {{Icon::Sine, "Sine"}, {Icon::CosBell, "CosBell"}, {Icon::TriBell, "TriBell"},
                               {Icon::SawTri, "Saw2Tri"}, {Icon::Trapezoid, "Sqr2Tri"}, {Icon::Square, "Square"}};
constexpr O kRectModes[] = {{Icon::HalfPos, "Half wave, positive"}, {Icon::HalfNeg, "Half wave, negative"},
                            {Icon::FullPos, "Full wave, positive"}, {Icon::FullNeg, "Full wave, negative"}};
constexpr O kShpStaticCurves[] = {{Icon::CurveInv3, "Inv x3"}, {Icon::CurveInv2, "Inv x2"}, {Icon::CurveX2, "x2"},
                                  {Icon::CurveX3, "x3"}};
constexpr O kRndPatternWaves[] = {{Icon::RndStep, "Random steps"}, {Icon::RndPulses, "Random pulses"}};

// Filters.
constexpr O kLpSlopes[] = {{Icon::LowPass, "6 dB/oct", 6},   {Icon::LowPass, "12 dB/oct", 12},
                           {Icon::LowPass, "18 dB/oct", 18}, {Icon::LowPass, "24 dB/oct", 24},
                           {Icon::LowPass, "30 dB/oct", 30}, {Icon::LowPass, "36 dB/oct", 36}};
constexpr O kHpSlopes[] = {{Icon::HighPass, "6 dB/oct", 6},   {Icon::HighPass, "12 dB/oct", 12},
                           {Icon::HighPass, "18 dB/oct", 18}, {Icon::HighPass, "24 dB/oct", 24},
                           {Icon::HighPass, "30 dB/oct", 30}, {Icon::HighPass, "36 dB/oct", 36}};

// Envelopes and sequencers.
constexpr O kEnvShapes[] = {{Icon::EnvLogExp, "LogExp"}, {Icon::EnvLinExp, "LinExp"}, {Icon::EnvExpExp, "ExpExp"},
                            {Icon::EnvLinLin, "LinLin"}};
constexpr O kEnvReset[] = {{Icon::None, "N"}, {Icon::None, "R"}};
constexpr O kSeqCycles[] = {{Icon::OneShot, "1-Cycle"}, {Icon::Loop, "Loop"}};
constexpr O kRndOutTypes[] = {{Icon::Bipolar, "Bipolar"}, {Icon::Positive, "Positive"}, {Icon::Negative, "Negative"}};

// Logic.
constexpr O kPulseModes[] = {{Icon::PosEdge, "Positive edge"}, {Icon::NegEdge, "Negative edge"}};
constexpr O kLogicDelayModes[] = {{Icon::DelayRise, "Delay positive edge"}, {Icon::DelayFall, "Delay negative edge"},
                                  {Icon::DelayBoth, "Delay both edges"}};
constexpr O kClkDivModes[] = {{Icon::ClkGated, "Gated"}, {Icon::ClkToggle, "Toggled"}};
constexpr O kFlipFlopModes[] = {{Icon::FlipFlopD, "D-type"}, {Icon::FlipFlopSR, "Set-Reset"}};
// The selector shows the gate's name; its menu also shows the symbol.
constexpr O kGateModes[] = {{Icon::GateAnd, "AND", 0, false, true},  {Icon::GateNand, "NAND", 0, false, true},
                            {Icon::GateOr, "OR", 0, false, true},    {Icon::GateNor, "NOR", 0, false, true},
                            {Icon::GateXor, "XOR", 0, false, true},  {Icon::GateXnor, "XNOR", 0, false, true}};

// Text pictures.
constexpr O kReverbRooms[] = {{Icon::None, "Small"}, {Icon::None, "Medium"}, {Icon::None, "Large"}, {Icon::None, "Hall"}};
constexpr O kDriverTypes[] = {{Icon::None, "Reed"}, {Icon::None, "Bow"}, {Icon::None, "Lip", 0, true},
                              {Icon::None, "Mallet", 0, true}};
constexpr O kRndCharacters[] = {{Icon::None, "Rnd1"}, {Icon::None, "Rnd2"}};
constexpr O kDelayRange3[] = {{Icon::None, "5m"},   {Icon::None, "25m"},  {Icon::None, "100m"}, {Icon::None, "500m"},
                              {Icon::None, "1.0s"}, {Icon::None, "2.0s"}, {Icon::None, "2.7s"}};
constexpr O kDelayRange2[] = {{Icon::None, "500m"}, {Icon::None, "1.0s"}, {Icon::None, "2.0s"}, {Icon::None, "2.7s"}};
constexpr O kDelayRange1[] = {{Icon::None, "500m"}, {Icon::None, "1.0s"}, {Icon::None, "1.35s"}};

struct Entry {
    std::uint8_t type;
    bool mode; // PartSelector (a module mode), else a parameter
    std::uint8_t ref;
    std::span<const Option> options;
};

const Entry kEntries[] = {
    {7, false, 8, kOscBWaves},          // OscB
    {8, true, 0, kOscShpBWaves},        // OscShpB
    {9, true, 0, kOscAWaves},           // OscC
    {12, true, 0, kReverbRooms},        // Reverb
    {20, false, 0, kEnvShapes},         // EnvADSR
    {20, false, 7, kEnvReset},
    {24, true, 0, kLfoCWaves},          // LfoC
    {25, false, 11, kLfoShpAWaves},     // LfoShpA
    {26, false, 4, kLfoAWaves},         // LfoA
    {35, true, 0, kDriverTypes},        // Driver
    {38, true, 0, kPulseModes},         // Pulse
    {42, true, 0, kLogicDelayModes},    // Delay
    {46, false, 0, kEnvShapes},         // EnvAHD
    {46, false, 3, kEnvReset},
    {52, false, 12, kEnvShapes},        // EnvMulti
    {52, false, 8, kEnvReset},
    {64, true, 0, kGateModes},          // Gate
    {64, true, 1, kGateModes},
    {69, true, 0, kClkDivModes},        // ClkDiv
    {82, false, 0, kRectModes},         // Rect
    {83, false, 0, kShpStaticCurves},   // ShpStatic
    {84, false, 0, kEnvShapes},         // EnvADR
    {84, false, 2, kEnvReset},
    {87, true, 0, kLpSlopes},           // FltLP
    {91, true, 0, kFlipFlopModes},      // FlipFlop
    {96, true, 0, kOscAWaves},          // OscD
    {97, false, 4, kOscAWaves},         // OscA
    {119, false, 1, kEnvShapes},        // EnvADDSR
    {119, false, 10, kEnvReset},
    {121, false, 32, kSeqCycles},       // SeqNote
    {134, true, 0, kHpSlopes},          // FltHP
    {144, false, 32, kSeqCycles},       // SeqEvent
    {145, false, 32, kSeqCycles},       // SeqVal
    {146, false, 32, kSeqCycles},       // SeqLev
    {163, false, 9, kOscShpAWaves},     // OscShpA
    {172, true, 0, kDelayRange3},       // DlySingleA
    {173, true, 0, kDelayRange3},       // DlySingleB
    {174, true, 0, kDelayRange3},       // DelayDual
    {175, true, 0, kDelayRange3},       // DelayQuad
    {176, true, 0, kDelayRange2},       // DelayA
    {177, true, 0, kDelayRange2},       // DelayB
    {181, true, 0, kDelayRange3},       // DlyEight
    {182, true, 0, kDelayRange1},       // DlyStereo
    {183, true, 0, kOscPMWaves},        // OscPM
    {190, false, 4, kLfoBWaves},        // LfoB
    {200, false, 2, kRndOutTypes},      // RandomA
    {202, false, 6, kRndOutTypes},      // RandomB
    {204, false, 3, kRndOutTypes},      // RndClkA
    {206, false, 1, kRndOutTypes},      // RndClkB
    {206, true, 0, kRndCharacters},
    {208, false, 5, kRndOutTypes},      // RndPattern
    {208, true, 0, kRndPatternWaves},
};

// ---- Drawing helpers --------------------------------------------------------------

juce::PathStrokeType stroke(float w = 1.25f)
{
    return {w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded};
}

// Maps (u, y) with u in 0..1 left to right and y in -1..1 bottom to top into `r`.
struct Frame {
    juce::Rectangle<float> r;
    juce::Point<float> at(float u, float y) const
    {
        return {r.getX() + u * r.getWidth(), r.getY() + (1.0f - y) * 0.5f * r.getHeight()};
    }
};

juce::Path lines(const Frame& f, std::initializer_list<std::pair<float, float>> pts)
{
    juce::Path p;
    bool first = true;
    for (const auto& [u, y] : pts) {
        const auto q = f.at(u, y);
        if (first)
            p.startNewSubPath(q);
        else
            p.lineTo(q);
        first = false;
    }
    return p;
}

// y = fn(u) sampled finely; jumps between segments are drawn as verticals.
juce::Path curve(const Frame& f, const std::function<float(float)>& fn, float u0 = 0.0f, float u1 = 1.0f)
{
    juce::Path p;
    const int steps = 48;
    for (int i = 0; i <= steps; ++i) {
        const float u = u0 + (u1 - u0) * static_cast<float>(i) / steps;
        const auto q = f.at(u, fn(u));
        if (i == 0)
            p.startNewSubPath(q);
        else
            p.lineTo(q);
    }
    return p;
}

void strokeIn(juce::Graphics& g, const juce::Path& p, juce::Colour c, float w = 1.25f)
{
    g.setColour(c);
    g.strokePath(p, stroke(w));
}

void arrowHead(juce::Graphics& g, juce::Point<float> tip, float angle, float size, juce::Colour c)
{
    juce::Path p;
    p.addTriangle(0.0f, 0.0f, -size, size * 0.65f, -size, -size * 0.65f);
    g.setColour(c);
    g.fillPath(p, juce::AffineTransform::rotation(angle).translated(tip));
}

void text(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& s, juce::Colour c, float size,
          juce::Justification j = juce::Justification::centred, bool bold = false)
{
    g.setColour(c);
    g.setFont(juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain)));
    g.drawText(s, r, j, false);
}

// Timing diagram: the input (faint, top) over the output (bottom), each a list
// of (rise, fall) times along 0..1.
void timing(juce::Graphics& g, juce::Rectangle<float> r, std::initializer_list<std::pair<float, float>> in,
            std::initializer_list<std::pair<float, float>> out, juce::Colour c)
{
    auto trace = [&](juce::Rectangle<float> row, std::initializer_list<std::pair<float, float>> pulses) {
        juce::Path p;
        const float lo = row.getBottom(), hi = row.getY();
        auto x = [&](float u) { return row.getX() + u * row.getWidth(); };
        p.startNewSubPath(x(0.0f), lo);
        for (const auto& [a, b] : pulses) {
            p.lineTo(x(a), lo);
            p.lineTo(x(a), hi);
            p.lineTo(x(b), hi);
            p.lineTo(x(b), lo);
        }
        p.lineTo(x(1.0f), lo);
        return p;
    };
    const float gap = std::max(2.0f, r.getHeight() * 0.18f);
    const float h = (r.getHeight() - gap) * 0.5f;
    strokeIn(g, trace(r.withHeight(h), in), c.withMultipliedAlpha(0.45f), 1.1f);
    strokeIn(g, trace(r.withTrimmedTop(h + gap), out), c, 1.3f);
}

// A gate's body: AND (flat back, round front) or OR (curved back, pointed front).
void gate(juce::Graphics& g, juce::Rectangle<float> r, bool isOr, bool negated, bool exclusive, juce::Colour c)
{
    const float bubble = negated ? 2.2f : 0.0f;
    auto body = r.withTrimmedLeft(r.getWidth() * 0.22f).withTrimmedRight(r.getWidth() * 0.18f + bubble * 2.0f);
    body = body.withSizeKeepingCentre(body.getWidth(), std::min(body.getHeight(), body.getWidth() * 1.05f));
    const float x0 = body.getX(), x1 = body.getRight(), y0 = body.getY(), y1 = body.getBottom(), ym = body.getCentreY();
    juce::Path p;
    if (!isOr) {
        p.startNewSubPath(x0, y0);
        p.lineTo(x0 + body.getWidth() * 0.45f, y0);
        p.quadraticTo(x1, y0, x1, ym);
        p.quadraticTo(x1, y1, x0 + body.getWidth() * 0.45f, y1);
        p.lineTo(x0, y1);
        p.closeSubPath();
    } else {
        p.startNewSubPath(x0, y0);
        p.quadraticTo(x0 + body.getWidth() * 0.6f, y0, x1, ym);
        p.quadraticTo(x0 + body.getWidth() * 0.6f, y1, x0, y1);
        p.quadraticTo(x0 + body.getWidth() * 0.3f, ym, x0, y0);
        p.closeSubPath();
    }
    strokeIn(g, p, c, 1.2f);
    if (exclusive) {
        juce::Path back;
        back.startNewSubPath(x0 - 2.5f, y0);
        back.quadraticTo(x0 - 2.5f + body.getWidth() * 0.3f, ym, x0 - 2.5f, y1);
        strokeIn(g, back, c, 1.1f);
    }
    // Inputs and output.
    const float inX = isOr ? x0 + body.getWidth() * 0.12f : x0;
    juce::Path wires;
    for (float y : {y0 + body.getHeight() * 0.28f, y1 - body.getHeight() * 0.28f}) {
        wires.startNewSubPath(r.getX(), y);
        wires.lineTo(inX, y);
    }
    wires.startNewSubPath(x1 + bubble * 2.0f, ym);
    wires.lineTo(r.getRight(), ym);
    strokeIn(g, wires, c, 1.1f);
    if (negated)
        g.drawEllipse(x1 + 0.5f, ym - bubble + 0.5f, bubble * 2.0f - 1.0f, bubble * 2.0f - 1.0f, 1.1f);
}

void flipFlop(juce::Graphics& g, juce::Rectangle<float> r, bool setReset, juce::Colour c)
{
    const auto box = r.reduced(r.getWidth() * 0.13f, 0.5f);
    g.setColour(c);
    g.drawRoundedRectangle(box, 2.0f, 1.2f);
    // Wires: data/set and clock in, Q and not-Q out.
    const float yTop = box.getY() + box.getHeight() * 0.3f, yBottom = box.getBottom() - box.getHeight() * 0.3f;
    juce::Path wires;
    for (float y : {yTop, yBottom}) {
        wires.startNewSubPath(r.getX(), y);
        wires.lineTo(box.getX(), y);
        wires.startNewSubPath(box.getRight(), y);
        wires.lineTo(r.getRight(), y);
    }
    strokeIn(g, wires, c, 1.1f);
    // Clock wedge.
    juce::Path wedge;
    wedge.startNewSubPath(box.getX(), yBottom - 2.5f);
    wedge.lineTo(box.getX() + 3.0f, yBottom);
    wedge.lineTo(box.getX(), yBottom + 2.5f);
    strokeIn(g, wedge, c, 1.0f);
    const float size = std::min(8.0f, box.getHeight() * 0.42f);
    const float half = box.getWidth() * 0.5f;
    text(g, {box.getX() + 1.5f, yTop - size * 0.6f, half, size * 1.2f}, setReset ? "S" : "D", c, size,
         juce::Justification::centredLeft, true);
    const juce::Rectangle<float> q{box.getCentreX(), yTop - size * 0.6f, half - 1.5f, size * 1.2f};
    const juce::Rectangle<float> nq{box.getCentreX(), yBottom - size * 0.6f, half - 1.5f, size * 1.2f};
    text(g, q, "Q", c, size, juce::Justification::centredRight, true);
    text(g, nq, "Q", c, size, juce::Justification::centredRight, true);
    // The bar of not-Q.
    const float barW = size * 0.62f;
    g.fillRect(nq.getRight() - barW, nq.getY() + 0.5f, barW, 0.9f);
    if (setReset)
        text(g, {box.getX() + 1.5f, yBottom - size * 0.6f, half, size * 1.2f}, "R", c, size,
             juce::Justification::centredLeft, true);
}

// Envelope: attack to the peak, then decay; each segment log, lin or exp.
enum class Seg { Log, Lin, Exp };
void envelope(juce::Graphics& g, juce::Rectangle<float> r, Seg attack, Seg decay, juce::Colour c)
{
    const Frame f{r};
    const float peak = 0.38f;
    auto shape = [](Seg s, float t) {
        switch (s) {
        case Seg::Log: return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        case Seg::Exp: return t * t * t;
        case Seg::Lin: break;
        }
        return t;
    };
    juce::Path p = curve(f, [&](float u) {
        if (u <= peak)
            return -1.0f + 2.0f * shape(attack, u / peak);
        // Decay: an Exp decay falls fast first (mirror of a log rise).
        const float t = (u - peak) / (1.0f - peak);
        const Seg d = decay == Seg::Exp ? Seg::Log : decay;
        return 1.0f - 2.0f * shape(d, t);
    });
    juce::Path filled(p);
    filled.lineTo(f.at(0.0f, -1.0f));
    filled.closeSubPath();
    g.setColour(c.withMultipliedAlpha(0.4f));
    g.fillPath(filled);
    strokeIn(g, p, c, 1.2f);
}

// Filter response: flat pass band, then a slope as steep as `dB` per octave.
void filter(juce::Graphics& g, juce::Rectangle<float> r, bool highPass, int dB, juce::Colour c)
{
    const float steep = juce::jlimit(0.0f, 1.0f, static_cast<float>(dB - 6) / 30.0f);
    const float knee = 0.42f;
    const float end = knee + 0.12f + 0.46f * (1.0f - steep); // where the slope reaches the floor
    const Frame f{r.reduced(0.0f, 0.5f)};
    juce::Path p;
    auto mirror = [&](float u) { return highPass ? 1.0f - u : u; };
    p.startNewSubPath(f.at(mirror(0.0f), 0.7f));
    p.lineTo(f.at(mirror(knee - 0.1f), 0.7f));
    p.cubicTo(f.at(mirror(knee + 0.04f), 0.7f), f.at(mirror(knee + 0.04f), 0.5f),
              f.at(mirror(end), -1.0f));
    strokeIn(g, p, c, 1.3f);
}

void power(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    const float s = std::min(r.getWidth(), r.getHeight());
    const auto b = r.withSizeKeepingCentre(s, s).reduced(0.5f);
    const float rad = b.getWidth() * 0.42f;
    const auto centre = b.getCentre().translated(0.0f, 0.4f);
    juce::Path arc;
    arc.addCentredArc(centre.x, centre.y, rad, rad, 0.0f, juce::degreesToRadians(35.0f),
                      juce::degreesToRadians(325.0f), true);
    arc.startNewSubPath(centre.x, b.getY());
    arc.lineTo(centre.x, centre.y);
    strokeIn(g, arc, c, 1.3f);
}

// A zero line with arrows: up from it (positive), down (negative) or both ways.
void polarity(juce::Graphics& g, juce::Rectangle<float> r, bool up, bool down, juce::Colour c)
{
    const auto b = r.withSizeKeepingCentre(std::min(r.getWidth(), r.getHeight() * 1.2f), r.getHeight());
    const float cx = b.getCentreX();
    const float zero = up && down ? b.getCentreY() : up ? b.getBottom() - 0.5f : b.getY() + 0.5f;
    g.setColour(c);
    g.fillRect(b.getX(), zero - 0.6f, b.getWidth(), 1.2f);
    const float head = std::max(2.5f, b.getHeight() * 0.36f);
    juce::Path stem;
    stem.startNewSubPath(cx, up ? b.getY() + head * 0.8f : zero);
    stem.lineTo(cx, down ? b.getBottom() - head * 0.8f : zero);
    strokeIn(g, stem, c, 1.2f);
    if (up)
        arrowHead(g, {cx, b.getY()}, -kPi * 0.5f, head, c);
    if (down)
        arrowHead(g, {cx, b.getBottom()}, kPi * 0.5f, head, c);
}

void oneShot(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    const float cy = r.getCentreY();
    juce::Path p;
    p.startNewSubPath(r.getX(), cy);
    p.lineTo(r.getRight() - 3.0f, cy);
    strokeIn(g, p, c, 1.3f);
    arrowHead(g, {r.getRight() - 1.5f, cy}, 0.0f, 3.5f, c);
    g.setColour(c);
    g.fillRoundedRectangle(r.getRight() - 1.2f, r.getY(), 1.4f, r.getHeight(), 0.7f);
}

void loop(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    const auto b = r.reduced(0.5f, 0.75f);
    juce::Path p;
    p.addRoundedRectangle(b.getX(), b.getY(), b.getWidth(), b.getHeight(), b.getHeight() * 0.45f);
    strokeIn(g, p, c, 1.2f);
    // A gap and an arrow on the top run, pointing right.
    const float x = b.getCentreX() + b.getWidth() * 0.1f;
    g.setColour(c);
    arrowHead(g, {x + 1.5f, b.getY()}, 0.0f, 3.2f, c);
}

// Random values for the random icons (fixed so the icons don't change).
constexpr float kRandom[] = {0.2f, 0.8f, 0.4f, -0.6f, -0.15f, 0.95f, -0.85f};

} // namespace

std::span<const Option> options(const g2::db::ModuleDef& def, const PanelElement& e)
{
    const bool mode = e.kind == "PartSelector";
    if (e.codeRef < 0 || !(mode || e.kind == "ButtonRadio" || e.kind == "ButtonFlat"))
        return {};
    for (const auto& entry : kEntries)
        if (entry.type == def.typeId && entry.mode == mode && entry.ref == e.codeRef)
            return entry.options;
    return {};
}

Icon buttonIcon(const g2::db::ModuleDef& def, const PanelElement& e)
{
    if (e.kind != "ButtonText" || e.codeRef < 0 || static_cast<std::size_t>(e.codeRef) >= def.params.size())
        return Icon::None;
    const juce::String name(def.params[static_cast<std::size_t>(e.codeRef)].name);
    // The module's bypass / mute button.
    if (name == "On/Off" || name == "Active")
        return Icon::Power;
    // KeyQuant's keyboard ("Note C" .. "Note B"): white keys are plain buttons.
    if (def.typeId == 152 && name.startsWith("Note "))
        return name.containsAnyOf("#b") ? Icon::BlackKey : Icon::WhiteKey;
    return Icon::None;
}

void draw(juce::Graphics& g, Icon icon, juce::Rectangle<float> box, juce::Colour c, int amount)
{
    if (box.isEmpty())
        return;
    const Frame f{box.reduced(0.6f)};
    auto sq = [&](std::initializer_list<std::pair<float, float>> pts) { strokeIn(g, lines(f, pts), c); };
    auto fn = [&](const std::function<float(float)>& y) { strokeIn(g, curve(f, y), c); };
    switch (icon) {
    case Icon::None: return;
    case Icon::Sine: fn([](float u) { return std::sin(2.0f * kPi * u); }); return;
    case Icon::Tri: sq({{0.0f, -1.0f}, {0.5f, 1.0f}, {1.0f, -1.0f}}); return;
    case Icon::Saw: sq({{0.0f, -1.0f}, {0.0f, 1.0f}, {1.0f, -1.0f}}); return;
    case Icon::Square: sq({{0.0f, -1.0f}, {0.0f, 1.0f}, {0.5f, 1.0f}, {0.5f, -1.0f}, {1.0f, -1.0f}}); return;
    case Icon::Pulse25: sq({{0.0f, -1.0f}, {0.0f, 1.0f}, {0.25f, 1.0f}, {0.25f, -1.0f}, {1.0f, -1.0f}}); return;
    case Icon::Pulse10: sq({{0.0f, -1.0f}, {0.0f, 1.0f}, {0.12f, 1.0f}, {0.12f, -1.0f}, {1.0f, -1.0f}}); return;
    case Icon::DualSaw: sq({{0.0f, -1.0f}, {0.0f, 1.0f}, {0.5f, -0.25f}, {0.5f, 0.55f}, {1.0f, -1.0f}}); return;
    case Icon::DoubleSaw: sq({{0.0f, -1.0f}, {0.0f, 1.0f}, {0.5f, -1.0f}, {0.5f, 1.0f}, {1.0f, -1.0f}}); return;
    case Icon::SymPulse:
        sq({{0.0f, 0.0f}, {0.22f, 0.0f}, {0.22f, -1.0f}, {0.5f, -1.0f}, {0.5f, 1.0f}, {0.78f, 1.0f}, {0.78f, 0.0f},
            {1.0f, 0.0f}});
        return;
    case Icon::RndStep: {
        juce::Path p;
        const int n = 7;
        for (int i = 0; i < n; ++i) {
            const float u0 = static_cast<float>(i) / n, u1 = static_cast<float>(i + 1) / n;
            if (i == 0)
                p.startNewSubPath(f.at(u0, kRandom[i]));
            else
                p.lineTo(f.at(u0, kRandom[i]));
            p.lineTo(f.at(u1, kRandom[i]));
        }
        strokeIn(g, p, c);
        return;
    }
    case Icon::Rnd: {
        juce::Path p;
        const int n = 5;
        p.startNewSubPath(f.at(0.0f, kRandom[0]));
        for (int i = 1; i < n; ++i) {
            const float u = static_cast<float>(i) / (n - 1);
            const float um = (static_cast<float>(i) - 0.5f) / (n - 1);
            p.cubicTo(f.at(um, kRandom[i - 1]), f.at(um, kRandom[i]), f.at(u, kRandom[i]));
        }
        strokeIn(g, p, c);
        return;
    }
    case Icon::RndPulses:
        sq({{0.0f, -1.0f}, {0.06f, -1.0f}, {0.06f, 1.0f}, {0.3f, 1.0f}, {0.3f, -1.0f}, {0.42f, -1.0f}, {0.42f, 1.0f},
            {0.52f, 1.0f}, {0.52f, -1.0f}, {0.66f, -1.0f}, {0.66f, 1.0f}, {0.94f, 1.0f}, {0.94f, -1.0f}, {1.0f, -1.0f}});
        return;
    case Icon::RoundPulses: {
        // Pulses whose falls round off into the next rise.
        juce::Path p;
        p.startNewSubPath(f.at(0.0f, -1.0f));
        const std::pair<float, float> pulses[] = {{0.06f, 0.32f}, {0.44f, 0.6f}, {0.72f, 0.94f}};
        for (const auto& [a, b] : pulses) {
            p.lineTo(f.at(a, -0.55f));
            p.lineTo(f.at(a, 1.0f));
            p.lineTo(f.at(b, 1.0f));
            p.lineTo(f.at(b, -0.55f));
            p.quadraticTo(f.at(b + 0.05f, -1.2f), f.at(b + 0.1f, -0.55f));
        }
        strokeIn(g, p, c);
        return;
    }
    case Icon::CosBell:
        fn([](float u) { return u < 0.6f ? -1.0f + 2.0f * (0.5f - 0.5f * std::cos(2.0f * kPi * u / 0.6f)) : -1.0f; });
        return;
    case Icon::TriBell: sq({{0.0f, -1.0f}, {0.3f, 1.0f}, {0.6f, -1.0f}, {1.0f, -1.0f}}); return;
    case Icon::SawTri: sq({{0.0f, -1.0f}, {0.22f, 1.0f}, {1.0f, -1.0f}}); return;
    case Icon::Trapezoid:
        sq({{0.0f, -1.0f}, {0.12f, 1.0f}, {0.38f, 1.0f}, {0.62f, -1.0f}, {0.88f, -1.0f}, {1.0f, 1.0f}});
        return;
    case Icon::Sine2: // a sine whose positive half is shorter
        fn([](float u) { return u < 0.38f ? std::sin(kPi * u / 0.38f) : -std::sin(kPi * (u - 0.38f) / 0.62f); });
        return;
    case Icon::Sine3: // one squeezed sine period between flat ends
        fn([](float u) { return u < 0.25f || u > 0.75f ? 0.0f : std::sin(2.0f * kPi * (u - 0.25f) / 0.5f); });
        return;
    case Icon::Sine4: // each half folded back from its peak
        fn([](float u) {
            const float h = u < 0.5f ? u / 0.5f : (u - 0.5f) / 0.5f;
            const float s = std::sin(kPi * h);
            const float y = s < 0.6f ? s / 0.6f : 1.0f - (s - 0.6f) / 0.4f * 0.55f;
            return u < 0.5f ? y : -y;
        });
        return;
    case Icon::HalfPos: fn([](float u) { return std::max(0.0f, std::sin(2.0f * kPi * u)) * 2.0f - 1.0f; }); return;
    case Icon::HalfNeg: fn([](float u) { return std::min(0.0f, std::sin(2.0f * kPi * u)) * 2.0f + 1.0f; }); return;
    case Icon::FullPos: fn([](float u) { return std::abs(std::sin(2.0f * kPi * u)) * 2.0f - 1.0f; }); return;
    case Icon::FullNeg: fn([](float u) { return 1.0f - std::abs(std::sin(2.0f * kPi * u)) * 2.0f; }); return;
    case Icon::CurveInv3: fn([](float u) { return std::cbrt(2.0f * u - 1.0f); }); return;
    case Icon::CurveInv2:
        fn([](float u) {
            const float x = 2.0f * u - 1.0f;
            return std::copysign(std::sqrt(std::abs(x)), x);
        });
        return;
    case Icon::CurveX2:
        fn([](float u) {
            const float x = 2.0f * u - 1.0f;
            return x * std::abs(x);
        });
        return;
    case Icon::CurveX3:
        fn([](float u) {
            const float x = 2.0f * u - 1.0f;
            return x * x * x;
        });
        return;
    case Icon::LowPass: filter(g, box, false, amount, c); return;
    case Icon::HighPass: filter(g, box, true, amount, c); return;
    case Icon::EnvLogExp: envelope(g, box, Seg::Log, Seg::Exp, c); return;
    case Icon::EnvLinExp: envelope(g, box, Seg::Lin, Seg::Exp, c); return;
    case Icon::EnvExpExp: envelope(g, box, Seg::Exp, Seg::Exp, c); return;
    case Icon::EnvLinLin: envelope(g, box, Seg::Lin, Seg::Lin, c); return;
    case Icon::PosEdge: timing(g, box, {{0.12f, 0.6f}}, {{0.12f, 0.36f}}, c); return;
    case Icon::NegEdge: timing(g, box, {{0.12f, 0.6f}}, {{0.6f, 0.84f}}, c); return;
    case Icon::DelayRise: timing(g, box, {{0.1f, 0.55f}}, {{0.32f, 0.55f}}, c); return;
    case Icon::DelayFall: timing(g, box, {{0.1f, 0.55f}}, {{0.1f, 0.77f}}, c); return;
    case Icon::DelayBoth: timing(g, box, {{0.1f, 0.55f}}, {{0.32f, 0.77f}}, c); return;
    case Icon::ClkGated:
        timing(g, box, {{0.06f, 0.16f}, {0.3f, 0.4f}, {0.54f, 0.64f}, {0.78f, 0.88f}}, {{0.06f, 0.16f}, {0.54f, 0.64f}}, c);
        return;
    case Icon::ClkToggle:
        timing(g, box, {{0.06f, 0.16f}, {0.3f, 0.4f}, {0.54f, 0.64f}, {0.78f, 0.88f}}, {{0.06f, 0.54f}}, c);
        return;
    case Icon::FlipFlopD: flipFlop(g, box, false, c); return;
    case Icon::FlipFlopSR: flipFlop(g, box, true, c); return;
    case Icon::GateAnd: gate(g, box, false, false, false, c); return;
    case Icon::GateNand: gate(g, box, false, true, false, c); return;
    case Icon::GateOr: gate(g, box, true, false, false, c); return;
    case Icon::GateNor: gate(g, box, true, true, false, c); return;
    case Icon::GateXor: gate(g, box, true, false, true, c); return;
    case Icon::GateXnor: gate(g, box, true, true, true, c); return;
    case Icon::Power: power(g, box, c); return;
    case Icon::OneShot: oneShot(g, box, c); return;
    case Icon::Loop: loop(g, box, c); return;
    case Icon::Bipolar: polarity(g, box, true, true, c); return;
    case Icon::Positive: polarity(g, box, true, false, c); return;
    case Icon::Negative: polarity(g, box, false, true, c); return;
    case Icon::BlackKey:
        g.setColour(c);
        g.fillRoundedRectangle(box, 1.5f);
        return;
    case Icon::WhiteKey: return;
    }
}

void drawOption(juce::Graphics& g, const Option& o, juce::Rectangle<float> box, juce::Colour colour)
{
    if (o.icon != Icon::None && !o.textOnFace) {
        draw(g, o.icon, box, colour, o.amount);
        return;
    }
    g.setColour(colour);
    g.setFont(juce::Font(juce::FontOptions(9.5f)));
    g.drawFittedText(o.label, box.toNearestInt(), juce::Justification::centred, 1, 0.75f);
    if (o.unavailable) {
        const float w = std::min(box.getWidth(), juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), o.label) + 2.0f);
        const auto line = box.withSizeKeepingCentre(w, 1.0f);
        g.setColour(colour.withMultipliedAlpha(0.8f));
        g.fillRect(line);
    }
}

std::unique_ptr<juce::Drawable> menuImage(const Option& o, juce::Colour colour)
{
    if (o.icon == Icon::None)
        return nullptr;
    // Drawn at 3x so the menu's scaling keeps it sharp.
    constexpr int w = 24, h = 16, scale = 3;
    juce::Image img(juce::Image::ARGB, w * scale, h * scale, true);
    {
        juce::Graphics g(img);
        g.addTransform(juce::AffineTransform::scale(static_cast<float>(scale)));
        draw(g, o.icon, juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)).reduced(1.5f, 1.5f),
             colour, o.amount);
    }
    auto d = std::make_unique<juce::DrawableImage>();
    d->setImage(img);
    d->setBoundingBox(juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)));
    return d;
}

// ---- Panel decorations ---------------------------------------------------------------

namespace {

enum class Deco : std::uint8_t {
    Switch,        // on/off switch, input left, output right
    SwitchControl, // the same with its control coming from above
    Changeover,    // two-way switch, control from above
    Memory,        // the sample memory of S&H / T&H
    UpDown,        // momentary / note on and off
    AmpRight,      // amplifier (VCA), signal left to right
    AmpDown,       // amplifier, signal top to bottom
    Inverter,
    Adder,
    Modulator,
    SendBox,       // CtrlSend / PCSend / Automate: the Send trigger
    FaderRight,    // a crossfade bar on the right, knob on the left (Pan)
    FaderLeft,     // mirrored (X-Fade)
    SplitRight,    // two outputs faded (Fade1-2)
    SplitLeft,     // two inputs faded (Fade2-1)
    WaveSquare,
    WaveSaw,
    Title,         // a section title
};

struct DecoEntry {
    std::uint8_t type;
    std::uint8_t id;
    Deco deco;
    const char* title = nullptr;
};

const DecoEntry kDecos[] = {
    {5, 7, Deco::Inverter},          {5, 10, Deco::Inverter},         // Invert
    {17, 2, Deco::Changeover},                                        // ValSw1-2
    {18, 12, Deco::FaderLeft},                                        // X-Fade
    {20, 2, Deco::AmpDown},                                           // EnvADSR
    {23, 15, Deco::AmpDown},                                          // ModADSR
    {36, 6, Deco::SwitchControl},    {36, 9, Deco::UpDown},           // SwOnOffM
    {41, 11, Deco::AmpRight},                                         // EnvH
    {46, 11, Deco::AmpDown},                                          // EnvAHD
    {47, 12, Deco::FaderRight},                                       // Pan
    {50, 4, Deco::Switch},                                            // ConstSwT
    {52, 3, Deco::AmpRight},                                          // EnvMulti
    {53, 2, Deco::SwitchControl},    {53, 7, Deco::Memory},           // S&H
    {55, 10, Deco::AmpRight},                                         // EnvD
    {57, 16, Deco::SendBox},                                          // Automate
    {58, 35, Deco::Title, "MasterOsc"},                               // DrumSynth
    {58, 43, Deco::Title, "SlaveOsc"},
    {58, 33, Deco::Title, "NoiseFilter"},
    {58, 46, Deco::Title, "Bend"},
    {76, 8, Deco::SwitchControl},                                     // SwOnOffT
    {84, 2, Deco::AmpDown},                                           // EnvADR
    {85, 10, Deco::SwitchControl},                                    // WindSw
    {90, 6, Deco::Changeover},                                        // Sw1-2
    {100, 7, Deco::Changeover},                                       // Sw2-1
    {105, 3, Deco::Changeover},                                       // ValSw2-1
    {112, 2, Deco::Adder},                                            // LevAdd
    {113, 12, Deco::SplitRight},                                      // Fade1-2
    {114, 11, Deco::SplitLeft},                                       // Fade2-1
    {115, 15, Deco::AmpDown},                                         // LevScaler
    {117, 4, Deco::Modulator},                                        // LevMod
    {119, 9, Deco::AmpRight},                                         // EnvADDSR
    {139, 3, Deco::SwitchControl},   {139, 6, Deco::Memory},          // T&H
    {141, 16, Deco::SendBox},                                         // CtrlSend
    {142, 15, Deco::SendBox},                                         // PCSend
    {143, 14, Deco::UpDown},                                          // NoteSend
    {164, 34, Deco::WaveSquare},     {164, 35, Deco::WaveSquare},     // OscDual
    {164, 33, Deco::WaveSaw},
    {169, 11, Deco::AmpDown},                                         // ModAHD
    {180, 82, Deco::Title, "Envelope"},                               // Operator
    {180, 25, Deco::Title, "KBLevScale"},
    {186, 6, Deco::Changeover},      {186, 11, Deco::UpDown},         // Sw1-2M
    {187, 7, Deco::Changeover},      {187, 11, Deco::UpDown},         // Sw2-1M
    {188, 4, Deco::Switch},          {188, 6, Deco::UpDown},          // ConstSwM
};

void wire(juce::Graphics& g, std::initializer_list<juce::Point<float>> pts, juce::Colour c)
{
    juce::Path p;
    bool first = true;
    for (auto q : pts) {
        if (first)
            p.startNewSubPath(q);
        else
            p.lineTo(q);
        first = false;
    }
    strokeIn(g, p, c, 1.0f);
}

void contact(juce::Graphics& g, juce::Point<float> at, juce::Colour c)
{
    g.setColour(c);
    g.fillEllipse(at.x - 1.6f, at.y - 1.6f, 3.2f, 3.2f);
}

// The wires of the original's symbols meet the panel's own lines and jacks,
// so they end where the element's box ends.
void paint(juce::Graphics& g, Deco d, juce::Rectangle<float> r, juce::Colour line, juce::Colour ink,
           const char* title)
{
    const float x0 = r.getX(), x1 = r.getRight(), y0 = r.getY(), y1 = r.getBottom();
    const float w = r.getWidth(), h = r.getHeight();
    switch (d) {
    case Deco::Switch:
    case Deco::SwitchControl: {
        // Wire level as in the original (just above the bottom); the lever
        // rests open, from the left contact up towards the right one.
        const float y = d == Deco::Switch ? y0 + h * 0.65f : y0 + h * 0.78f;
        const juce::Point<float> a{x0 + w * 0.32f, y}, b{x0 + w * 0.68f, y};
        wire(g, {{x0, y}, a}, line);
        wire(g, {b, {x1, y}}, line);
        contact(g, a, line);
        contact(g, b, line);
        wire(g, {a, {b.x - 0.5f, y - h * (d == Deco::Switch ? 0.5f : 0.3f)}}, line);
        if (d == Deco::SwitchControl) {
            const float mx = x0 + w * 0.46f;
            g.setColour(line.withMultipliedAlpha(0.8f));
            const float dash[] = {2.0f, 1.5f};
            juce::Path ctl, dashed;
            ctl.startNewSubPath(mx, y0);
            ctl.lineTo(mx, y - h * 0.2f);
            juce::PathStrokeType(1.0f).createDashedStroke(dashed, ctl, dash, 2);
            g.fillPath(dashed);
        }
        return;
    }
    case Deco::Changeover: {
        const float y = y0 + h * 0.78f;
        const juce::Point<float> a{x0 + w * 0.34f, y}, b{x0 + w * 0.7f, y};
        wire(g, {{x0, y}, a}, line);
        wire(g, {b, {x1, y}}, line);
        contact(g, a, line);
        contact(g, b, line);
        // The lever hangs from the control above and touches the left contact.
        const juce::Point<float> pivot{x0 + w * 0.52f, y0 + h * 0.42f};
        wire(g, {{pivot.x, y0}, pivot, a}, line);
        return;
    }
    case Deco::Memory: {
        const auto b = r.withSizeKeepingCentre(std::min(w, h) - 1.0f, std::min(w, h) - 1.0f);
        g.setColour(line);
        g.drawRoundedRectangle(b, 2.0f, 1.0f);
        text(g, b, "M", ink, b.getHeight() * 0.72f, juce::Justification::centred, true);
        return;
    }
    case Deco::UpDown: {
        const float cx = r.getCentreX();
        wire(g, {{cx, y0 + 3.0f}, {cx, y1 - 3.0f}}, ink);
        arrowHead(g, {cx, y0 + 0.5f}, -kPi * 0.5f, 3.5f, ink);
        arrowHead(g, {cx, y1 - 0.5f}, kPi * 0.5f, 3.5f, ink);
        return;
    }
    case Deco::AmpRight:
    case Deco::Inverter: {
        const float cy = r.getCentreY();
        const bool inv = d == Deco::Inverter;
        const float tx0 = x0 + w * (inv ? 0.14f : 0.26f), tx1 = x0 + w * (inv ? 0.6f : 0.72f);
        const float th = std::min(h - 1.0f, (tx1 - tx0) * 1.1f);
        juce::Path tri;
        tri.addTriangle(tx0, cy - th * 0.5f, tx0, cy + th * 0.5f, tx1, cy);
        g.setColour(line);
        g.strokePath(tri, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        wire(g, {{x0, cy}, {tx0, cy}}, line);
        if (inv) {
            g.drawEllipse(tx1 + 0.5f, cy - 1.8f, 3.6f, 3.6f, 1.0f);
            wire(g, {{tx1 + 4.1f, cy}, {x1, cy}}, line);
        } else {
            wire(g, {{tx1, cy}, {x1, cy}}, line);
        }
        return;
    }
    case Deco::AmpDown: {
        const float cx = r.getCentreX();
        const float tw = w - 1.0f, th = std::min(h * 0.5f, tw);
        const float ty0 = r.getCentreY() - th * 0.5f, ty1 = ty0 + th;
        juce::Path tri;
        tri.addTriangle(cx - tw * 0.5f, ty0, cx + tw * 0.5f, ty0, cx, ty1);
        g.setColour(line);
        g.strokePath(tri, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        wire(g, {{cx, y0}, {cx, ty0}}, line);
        wire(g, {{cx, ty1}, {cx, y1}}, line);
        return;
    }
    case Deco::Adder: {
        const float cy = y0 + h * 0.62f, cx = r.getCentreX();
        const float rad = std::min(w, h) * 0.28f;
        g.setColour(line);
        g.drawEllipse(cx - rad, cy - rad, rad * 2.0f, rad * 2.0f, 1.0f);
        wire(g, {{cx - rad * 0.55f, cy}, {cx + rad * 0.55f, cy}}, ink);
        wire(g, {{cx, cy - rad * 0.55f}, {cx, cy + rad * 0.55f}}, ink);
        wire(g, {{x0, cy}, {cx - rad, cy}}, line);
        wire(g, {{cx + rad, cy}, {x1, cy}}, line);
        wire(g, {{cx, y0}, {cx, cy - rad - 2.0f}}, line);
        arrowHead(g, {cx, cy - rad - 0.3f}, kPi * 0.5f, 2.8f, line);
        return;
    }
    case Deco::Modulator: {
        // Input into a box (the multiplier), output on to the right and down.
        const auto box = juce::Rectangle<float>(x0 + w * 0.24f, y0 + 0.5f, w * 0.5f, h - 1.0f);
        g.setColour(line);
        g.drawRoundedRectangle(box, 1.5f, 1.0f);
        wire(g, {{x0, box.getCentreY()}, {box.getX(), box.getCentreY()}}, line);
        wire(g, {{box.getRight(), box.getCentreY()}, {x1 - 0.5f, box.getCentreY()}, {x1 - 0.5f, y1}}, line);
        return;
    }
    case Deco::SendBox: {
        const float cy = r.getCentreY();
        const auto box = r.withSizeKeepingCentre(std::min(w * 0.42f, h - 2.0f), std::min(w * 0.42f, h - 2.0f));
        g.setColour(line);
        g.drawRoundedRectangle(box, 1.5f, 1.0f);
        text(g, box, "D", ink, box.getHeight() * 0.75f, juce::Justification::centred, true);
        wire(g, {{x0, cy}, {box.getX(), cy}}, line);
        wire(g, {{box.getRight(), cy}, {x1, cy}}, line);
        return;
    }
    case Deco::FaderRight:
    case Deco::FaderLeft:
    case Deco::SplitRight:
    case Deco::SplitLeft: {
        // A line from the knob over to a marker on a bar spanning the two
        // jacks it fades between; split when the fade has two ends.
        const bool left = d == Deco::FaderLeft || d == Deco::SplitLeft;
        const bool split = d == Deco::SplitRight || d == Deco::SplitLeft;
        auto x = [&](float u) { return left ? x1 - u : x0 + u; };
        const float knobX = x(1.0f), markerX = x(w * 0.735f), barA = x(w * 0.5f), barB = x(w - 1.0f);
        const float top = y0 + 1.0f, barY = y0 + h * 0.6f;
        wire(g, {{knobX, y1}, {knobX, top}, {markerX, top}}, line);
        juce::Path m;
        m.addTriangle(markerX - 2.5f, top, markerX + 2.5f, top, markerX, barY - 1.5f);
        g.setColour(ink);
        g.fillPath(m);
        juce::Path bar;
        const float gap = split ? 2.0f : 0.0f;
        bar.startNewSubPath(barA, y1);
        bar.lineTo(barA, barY);
        bar.lineTo(left ? markerX + gap : markerX - gap, barY);
        bar.startNewSubPath(left ? markerX - gap : markerX + gap, barY);
        bar.lineTo(barB, barY);
        bar.lineTo(barB, y1);
        g.setColour(line);
        g.strokePath(bar, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }
    case Deco::WaveSquare: draw(g, Icon::Square, r.reduced(2.5f, 2.5f), ink); return;
    case Deco::WaveSaw: draw(g, Icon::Saw, r.reduced(2.5f, 2.5f), ink); return;
    case Deco::Title: {
        // The title, then a hairline to the end of the section.
        const juce::String s(title);
        const juce::Font font(juce::FontOptions(9.0f, juce::Font::bold));
        g.setFont(font);
        g.setColour(ink);
        g.drawText(s, r.withWidth(w), juce::Justification::centredLeft, false);
        const float tx = x0 + juce::GlyphArrangement::getStringWidth(font, s) + 4.0f;
        if (tx < x1) {
            g.setColour(line.withMultipliedAlpha(0.7f));
            g.fillRect(tx, r.getCentreY() - 0.5f, x1 - tx, 1.0f);
        }
        return;
    }
    }
}

} // namespace

bool paintDecoration(juce::Graphics& g, const g2::db::ModuleDef& def, const PanelElement& e, juce::Colour line,
                     juce::Colour ink)
{
    for (const auto& d : kDecos)
        if (d.type == def.typeId && d.id == e.id) {
            const juce::Rectangle<float> r(static_cast<float>(e.x), static_cast<float>(e.y),
                                           static_cast<float>(e.width), static_cast<float>(e.height));
            paint(g, d.deco, r, line, ink, d.title);
            return true;
        }
    return false;
}

} // namespace g2ui::icons
