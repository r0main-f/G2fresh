// Module graphs (envelope shapes, filter curves, waveform previews, ...),
// ported from the original Clavia G2 editor (Mac v1.62, classes CPnl*Graph).
// See re/notes/module-graphs.md.
//
// A PANL `Graph` element has a "Graph Func" id that selects the graph class
// (CCustomObjectFactory::CreateCustom) and a "Dependencies" list of parameter
// indices / mode references ("S0") whose current values it shows. `values`
// below are those values, in the order of the Dependencies string.
//
// The original draws with integer QuickDraw lines into a 200x100 scratch
// bitmap, fills columns under/inside the curve, then copies a (Width-2) x
// (Height-2) part of it into the view, inside a 1 px frame. `render()` returns
// that sequence of primitives exactly (integer coordinates, same order);
// `rasterize()` replays it into pixels; `draw()` turns it into polylines and
// fill polygons in view pixels for a vector UI.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace g2::graphs {

// Colours of the original (kGraph*Color, Color::kGraphFrame).
enum class Ink : std::uint8_t {
    Frame,    // 1 px frame around the view            (100,100,100)
    Back,     // background                            (  0,128,128)
    BackLine, // reference line (0 dB, zero level)     (192,192,192)
    Border,   // curve outline of filter/shaper graphs (  0,  0,  0)
    Single,   // envelope / waveform curve             (  0,255,  0)
    Base,     // base line and sustain segment         (255,255,  0)
    Shade,    //                                       ( 75, 99, 99)
    Fill,     // area under a Border curve             (  0,255,128)
    EnvFill,  // area between an envelope and its BackLine (0,164,164)
    Text,     // filter slope text, Arial 9            (255,255,  0)
};

struct Rgb {
    std::uint8_t r, g, b;
};
Rgb color(Ink ink);

// One drawing primitive of the original, in drawing order.
struct Op {
    enum class Kind : std::uint8_t {
        Frame,        // 1 px rectangle outline on the view, l,t,r,b (r,b exclusive):
                      // the CGraphGUI frame (Ink::Frame), or with `view` set a
                      // DrawFrame of the graph itself
        Rect,         // DrawRect l,t,r,b (r,b exclusive), scratch coords unless `view`
        Line,         // DrawLine (MoveTo x0,y0; LineTo x1,y1), both ends drawn
        Text,         // DrawText at x0 (left), y0 (baseline); x1 = right edge
        FillUnder,    // FillUnderGraph(x1 = w, y1 = h)
        FillEnvelope, // FillEnvelopeGraph(x1 = w, y1 = h)
        Bitmap,       // resource resId, src (x1,y1) size (w,h) -> view (x0,y0)
        Blit,         // scratch (x1,y1) size (w,h) -> view (x0,y0)
    };
    Kind kind = Kind::Line;
    Ink ink = Ink::Border;
    bool view = false; // Rect/Line drawn directly on the view (view coords)
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int w = 0, h = 0;
    int resId = 0;
    std::string text;
};

struct Drawing {
    int width = 0, height = 0; // view size
    std::vector<Op> ops;
};

// Original view size (set by each graph class' constructor); {0,0} for ids
// that are not graphs.
std::pair<int, int> defaultSize(int graphFunc);

// True when graphFunc is a graph (draws something). The other ids handled by
// CCustomObjectFactory are interactive controls (2 sequencer clear/random,
// 22 vocoder presets, 25/26 note sequencer zoom/offset, 46 drum presets) or
// the empty CPnlGraphABC (0 and unknown ids).
bool isGraph(int graphFunc);

// Original class name, e.g. "CPnlClassicFilterGraph"; "" if unknown.
const char* className(int graphFunc);

// The original's primitives. width/height <= 0 use defaultSize(). Missing
// values read as 0.
Drawing render(int graphFunc, std::span<const std::uint8_t> values, int width = 0, int height = 0);

// Pixel image (row major, width*height) of a Drawing, background Ink::Back;
// text is not rendered (draw it from the Text ops) and Bitmap areas are left
// as background. Lines use Bresenham, which may differ from QuickDraw by a
// pixel on some slopes.
std::vector<Ink> rasterize(const Drawing& d);

// Vector form for the UI, in view pixels (0..width, 0..height): first the
// filled background, then fill polygons (filled = true, closed = true, built
// from the column fills of rasterize()), then the curves as polylines through
// pixel centres (x + 0.5), clipped to the visible interior, then the frame
// (closed polyline on pixel centres). A polyline with a single point is one
// pixel.
struct Polyline {
    std::vector<std::pair<float, float>> points;
    bool closed = false;
    bool filled = false;
    Ink ink = Ink::Border;
};
std::vector<Polyline> draw(int graphFunc, std::span<const std::uint8_t> values, float width = 0,
                           float height = 0);
std::vector<Polyline> toPolylines(const Drawing& d);

} // namespace g2::graphs
