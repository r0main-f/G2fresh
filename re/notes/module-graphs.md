# Module graphs (`Graph` panel elements)

How the original editor (Mac v1.62, `G2Editor_i386`) draws the small graphs
inside modules (envelope shapes, filter curves, shaper transfer curves,
waveform previews, ...), and how the port reproduces them.

- Port: `core/include/g2/graphs.hpp`, `core/src/graphs.cpp`
  (`g2::graphs::render(id, values)` = the original's primitives,
  `rasterize()` = pixels, `draw()` = polylines / fill polygons for the UI).
- Tests: `tests/test_graphs.cpp` (digest of every primitive over ~9800 value
  tuples, plus spot checks), expected values from the original code run in an
  emulator.
- Tools: `tools/graphs/emulate.py` (runs the original graph classes),
  `cases.py` (value tuples), `compare.py` (port vs original),
  `golden.py` (test table).

Addresses are virtual addresses in `original/mac/G2Editor_i386` (= Ghidra
program `G2Editor_i386`). As in `param-display.md`, the copies at
0x24xxxx/0x5ccxxx are `.eh` data decoded as code; ignore them.

## 1. From PANL element to graph object

A PANL `Graph` element is read by `CPanelCustomObjectData::ReadCustomData`
(0x88fca): `XPos`, `YPos`, `Graph Func` (stored at +0x18) and `Dependencies`
(split at `,`; `S<n>`/`s<n>` = mode n, a number = parameter n; same parser as
TextField dependencies, see `param-display.md` 2.2). `Width`/`Height` are not
used: every graph class sets its own size in its constructor
(`CView::SetSize`), and those sizes equal the PANL values.

`CModuleGeneric::CreateCustom` (0x8493c) calls
`CCustomObjectFactory::CreateCustom(graphFunc, panel, x, y, nDeps)` (0x87736)
and then `AddDependency` for each item, in string order, skipping items whose
parameter / mode does not exist in the module. `CPnlCustom::GetDependencyValue(i)`
(0xc91d6) returns the i-th item's current value (parameter value of the
focused variation, or mode value). So the port's `values` are the values of
the Dependencies items in order.

On a change, `UpdateGraphics()` (virtual) copies the values it needs into
fields of the object (often transformed, e.g. `slope*6+12`), clears the dirty
flag and invalidates; `Draw()` (virtual) paints from those fields. The port
models each class as "fields from values" + "draw from fields".

### Graph Func ids

All 46 PANL Graph elements use 44 distinct ids. `CreateCustom` handles 0..46;
other values give a bare `CPnlGraphABC`.

| id | class (size W x H) | Draw | used by panel(s): Dependencies |
|---:|---|---|---|
| 0 | `CPnlGraphABC` - | - (CGroupView::Draw) | LfoD: `6,8,s0` |
| 1 | `CPnlADEnvGraph` 45x24 | `CPnlADGraphABC::Draw` 0xa6e78 | EnvADR: `1,3,0,7,5` |
| 2 | `CPnlSeqClrRnd` (control) | - | SeqA: `16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,34` |
| 3 | `CPnlADSRGraph` 61x28 | `CPnlADSRGraphABC::Draw` 0xa6656 | EnvADSR: `1,2,3,4,0,5` |
| 4 | `CPnlModEnvGraph` 61x28 | `CPnlADSRGraphABC::Draw` 0xa6656 | ModADSR: `0,1,2,3,8` |
| 5 | `CPnlAREnvGraph` 45x24 | `CPnlADSRGraphABC::Draw` 0xa6656 | AR-Env: `1,3,0,2,4` |
| 6 | `CPnlDEnvGraph` 31x22 | `CPnlADGraphABC::Draw` 0xa6e78 | EnvD: `0,1` |
| 7 | `CPnlHEnvGraph` 31x22 | `CPnlAHDGraphABC::Draw` 0xa3c8c | EnvH: `0,1` |
| 8 | `CPnlVocoderGraph` 194x47 | `CPnlVocoderGraph::Draw` 0x9d92c | Vocoder: `0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15` |
| 9 | `CPnlEqHiLoGraph` 52x28 | `CPnlEqHiLoGraph::Draw` 0xa1880 | ShelvEQ: `0,1,2` |
| 10 | `CPnlEqMidGraph` 52x28 | `CPnlEqMidGraph::Draw` 0xa1c7e | EqPeak: `0,1,2` |
| 11 | `CPnlAmpProcGraph` 56x24 | `CPnlAmpProcGraph::Draw` 0xa208c | LevScaler: `0,1,2` |
| 12 | `CPnlXMuxGraph` 34x22 | `CPnlXMuxGraph::Draw` 0xa0726 | Mux8-1X: `0` |
| 13 | `CPnlPhaserGraph` 52x28 | `CPnlFeedbackFlt::Draw` 0xa4236 | FltPhase: `5,3,4,9` |
| 14 | `CPnlDistAGraph` 34x22 | `CPnlDistAGraph::Draw` 0xa39cc | Clip: `2,1` |
| 15 | `CPnlDistBGraph` 34x22 | `CPnlDistBGraph::Draw` 0xa3080 | Overdrive: `4,1` |
| 16 | `CPnlWrapGraph` 34x22 | `CPnlWrapGraph::Draw` 0xa2a50 | WaveWrap: `1` |
| 17 | `CPnlMultiEnvGraph` 84x28 | `CPnlMultiEnvGraph::Draw` 0xa745a | EnvMulti: `0,1,2,3,4,5,6,7,9,10,12` |
| 18 | `CPnlOscCGraph` 35x22 | `CPnlOscCGraph::Draw` 0x9ffbe | OscShpB: `6,S0` |
| 19 | `CPnlPulseOscGraph` 35x20 | `CPnlPulseOscGraph::Draw` 0x9fa0e | PulseOsc: `5,7,10,11,S0` |
| 20 | `CPnlClassicFilterGraph` 52x28 | `CPnlFilterGraphABC::Draw` 0xa50b0 | FltClassic: `0,3,4,5` |
| 21 | `CPnlNormalFilterGraph` 52x28 | `CPnlFilterGraphABC::Draw` 0xa50b0 | FltNord: `0,4,8,3,5,6` |
| 22 | `CPnlVocoderPreset` (control) | - | Vocoder: `0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15` |
| 23 | `CPnlADBDSREnvGraph` 84x28 | `CPnlADBDSREnvGraph::Draw` 0xa7b94 | EnvADDSR: `2,3,4,5,6,7,1,8,9` |
| 24 | `CPnlLfoGraph` 35x20 | `CPnlLfoGraph::Draw` 0xa053e | - |
| 25 | `CPnlNoteSeqZoom` (control) | - | SeqNote: `0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15` |
| 26 | `CPnlNoteSeqOffset` (control) | - | SeqNote: `0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15` |
| 27 | `CPnlNoteSeqClrRnd` (control) | - | - |
| 28 | `CPnlAHDEnvGraph` 58x28 | `CPnlAHDGraphABC::Draw` 0xa3c8c | EnvAHD: `1,2,4,0,5`; ModAHD: `0,1,2,6` |
| 29 | `CPnlDXEnvGraph` 85x28 | `CPnlDXEnvGraph::Draw` 0xa8a40 | EnvDX: `0,1,2,3,4,5,6,7`; Operator: `8,9,10,11,12,13,14,15` |
| 30 | `CPnlSmallFilterGraph` 32x22 | `CPnlSmallFilterGraph::Draw` 0xa4c4a | FltLP: `0,3,S0` |
| 31 | `CPnlSmallFilterGraph` 32x22 | `CPnlSmallFilterGraph::Draw` 0xa4c4a | FltHP: `0,3,S0` |
| 32 | `CPnlOscSinShapeGraph` 38x22 | `CPnlOscSinShapeGraph::Draw` 0x9fc20 | OscShpA: `7,9` |
| 33 | `CPnlLfoBGraph` 52x28 | `CPnlLfoBGraph::Draw` 0xa03b0 | LfoB: `4,6,8` |
| 34 | `CPnlLfoCGraph` 52x28 | `CPnlLfoCGraph::Draw` 0xa020e | LfoShpA: `11,5,7,10` |
| 35 | `CPnlCombFltGraph` 52x28 | `CPnlFeedbackFlt::Draw` 0xa4236 | FltComb: `3,5` |
| 36 | `CPnlEqHiLoGraph` 52x28 | `CPnlEqHiLoGraph::Draw` 0xa1880 | Eq2Band: `0,1` |
| 37 | `CPnlEqHiMidLoGraph` 52x28 | `CPnlEqHiMidLoGraph::Draw` 0xa1112 | Eq3band: `0,1,2,3` |
| 38 | `CPnlShapeBGraph` 34x22 | `CPnlShapeBGraph::Draw` 0xa26e4 | ShpExp: `0,3` |
| 39 | `CPnlSaturateGraph` 34x22 | `CPnlSaturateGraph::Draw` 0xa2330 | Saturate: `0,3` |
| 40 | `CPnlStaticFilterGraph` 52x28 | `CPnlFilterGraphABC::Draw` 0xa50b0 | FltStatic: `0,1,2,4,3` |
| 41 | `CPnlOperatorGraph` 63x24 | `CPnlOperatorGraph::Draw` 0x9e930 | Operator: `17,18,19,20,21` |
| 42 | `CPnlDXRouterGraph` 139x67 | `CPnlDXRouterGraph::Draw` 0x976fe | DXRouter: `0` |
| 43 | `CPnlTunedNoiseGraph` 31x22 | `CPnlTunedNoiseGraph::Draw` 0x9e558 | OscNoise: `6` |
| 44 | `CPnlRndDistributionGraph` 35x22 | `CPnlRndDistributionGraph::Draw` 0x97894 | - |
| 45 | `CPnlRndTrigGraph` 21x22 | `CPnlRndTrigGraph::Draw` 0x97aa2 | RndTrig: `0` |
| 46 | `CPnlDrumPresetSelector` (control) | - | DrumSynth: `0,1,2,3,4,5,6,7,8,9,10,11,12,13,14` |

Ids 24, 27, 44 are not used by any panel. 2, 22, 25, 26, 27, 46 are not
graphs but interactive custom controls (buttons, selectors) created through the
same factory; the port's `isGraph()` is false for them and `render()` returns
nothing. Id 0 (LfoD, an unmapped panel) is a plain `CPnlGraphABC`, whose
`Draw` is `CGroupView::Draw`: nothing is drawn.

## 2. Drawing model (CPnlGraphABC, CGraphGUI)

Every graph draws the same way (e.g. `CPnlFilterGraphABC::Draw`, 0xa50b0):

1. `rect = GetRect()` = (0, 0, W, H) in view coordinates;
   `CGraphGUI::Draw(rect)` (0x81c94) frames it with `Color::kGraphFrame`
   (100,100,100), 1 px (QuickDraw `FrameRect`).
2. The curve is drawn into a shared offscreen bitmap,
   `CPnlGraphABC::fScratchPad` (a 200 x 100 `CBitmap32`, created by
   `Graph::InitBitmaps`). First a background rectangle in `kGraphBackColor`,
   then lines with `CABCCanvas::DrawLine(x0, y0, x1, y1, colour)`
   (0x161762: QuickDraw `MoveTo` + `LineTo`, 1 x 1 pen, both end pixels set).
   All coordinates are 16-bit integers; points outside the bitmap are
   clipped.
3. Optionally a column fill of the bitmap (below).
4. Optionally text (filter slope) with `DrawText(x, y)`, y = baseline, font
   `kGraphFilterTextFont` = Arial 9, colour `kGraphFilterTextColor`; x is
   usually right-aligned with `GetTextWidth`.
5. `DrawBitmap(view, rect.left+1, rect.top+1, fScratchPad, sx, sy, w, h)`
   copies a w x h part of the bitmap into the view, inside the frame
   (w x h = (W-2) x (H-2); the source origin (sx, sy) is (0, 0) for most
   classes but (1, 1) for the AD/ADSR/AHD/MultiEnv/ADDSR/DX envelopes; the
   port records it as a `Blit` op).

So scratch coordinate (x, y) is view pixel (x+1-sx, y+1-sy); x and y grow
right and down; the visible scratch area is [sx, sx+w) x [sy, sy+h). A few graphs (RndTrig)
draw straight onto the view instead (`Op::view`), DXRouter only copies a
bitmap resource.

### Pen helpers (CPnlGraphABC)

The pen is the short pair at +0x7c/+0x7e.

| helper | address | behaviour |
|---|---|---|
| `MoveTo(x, y)` | 0x9542e | pen = (x, y) |
| `LineTo(x, y, c)`, `ShadedLineTo` | 0x95444, 0x95498 | line pen -> (x, y), pen = (x, y) |
| `SingleLineTo` / `BorderLineTo` / `BackLineTo` / `BaseLineTo` | 0x9bc3a / 0x9e444 / 0x9e4ce / 0xa5dec | LineTo in kGraphSingleLineColor / BorderLineColor / BackLineColor / BaseLineColor |
| `Bernstein(t, i)` | 0x980da | C(3,i) t^i (1-t)^(3-i): powers in double, 1-t in float |
| `Bezier(P[4], c)` | 0x9b4e6 | pen = P0; t = 0.1f, 0.2f, ... (9 steps, t += 0.1 in double then rounded to float); point = sum B_i P_i accumulated in float; line to (int)(x+0.5), (int)(y+0.5); finally line to P3 |
| `PolyBezier(P, n, c)` | 0x9b6ae | Bezier(P[0..3]), then Bezier(P[3k..3k+3]) for 1 <= k < n/3 |
| `PolyBezierTo(P, n, c)` | 0x9b88a | Bezier(pen, P0, P1, P2), then Bezier(P[3k-1..3k+2]) for 1 <= k < n/3 |
| `BezierTo(P[3], c)` | 0x9ba56 | Bezier(pen, P0, P1, P2) |
| `Border/Single` + `PolyBezier`, `PolyBezierTo`, `BezierTo`, `Bezier` | 0x9b7ca.. 0x9bbe2 | colour wrappers |
| `ArcTo(10 doubles)` | 0x9daae | elliptical arc, see below |
| `ExpTo(x, y, flag)` / `LogTo(x, y, flag)` | 0x9df9c / 0x9e1be | quarter ellipse from the pen to (x, y); LogTo = ExpTo with !flag |
| `FillUnderGraph(w, h)` | 0x95572 | column fill, below |
| `FillEnvelopeGraph(w, h)` | 0x95748 | column fill, below |
| `Exp(x, n)`, `Mult`, `Div`, `round`, `Faculty`, `Choose` | 0x954ec.. | trivial helpers (`round(f) = (int)(f + 0.5)`) |

`ArcTo(a0..a9)`: ellipse centre c = (a0 + (a2-a0)/2, a3 + (a1-a3)/2), radii
|a2-a0|/2, |a1-a3|/2. Angles (atan of dy/dx, +pi when x < cx, +-pi/2 when
x == cx) of the start reference (a4,a5) = t1, end reference (a6,a7) = t2, the
pen = tp and the end point (a8,a9) = te. If t1 < 0 or t2 < 0 both get +2pi; if
t2 > t1, t1 += 2pi and tp/te swap roles. If the (possibly swapped) "first"
angle is greater than the "second", the arc is drawn reversed: the pen jumps
to the end point first. Then for t = t2; t < t1; t += (t1-t2)/20 a single-
colour line to ((int)(cos t * rx + cx + 0.5), (int)(sin t * ry + cy + 0.5)).
Finally a line to the end point (normal case), or back to the original pen
position and the pen set to the end point (reversed case). All in double.

`ExpTo(x, y, flag)` with pen (x0, y0), dx = x - x0, dy = |y - y0|: if
x <= x0 or y == y0 a straight single line; otherwise `ArcTo` of the quarter
ellipse whose bounding box is chosen by flag and direction:

| flag | direction | box (a0, a1, a2, a3) | refs (a4,a5), (a6,a7) |
|---|---|---|---|
| 0 | up (y < y0) | (x0-dx, y-dy, x, y0) | (x0,y0), (x,y) |
| 0 | down | (x0, y, x+dx, y0-dy) | (x0,y0), (x,y) |
| 1 | up | (x0, y, x+dx, y0+dy) | (x,y), (x0,y0) |
| 1 | down | (x0-dx, y0, x, y0+2dy) | (x,y), (x0,y0) |

### Colours

Static initialiser 0x1fb280 (`Graph` translation unit), 16-bit RGB:

| constant | RGB (8 bit) | port `Ink` | use |
|---|---|---|---|
| `kGraphBackColor` 0x3185c4 | 0,128,128 | Back | background rectangle |
| `kGraphBorderLineColor` 0x3185e2 | 0,0,0 | Border | filter / shaper curves |
| `kGraphSingleLineColor` 0x3185dc | 0,255,0 | Single | envelopes, waveforms, arcs |
| `kGraphBackLineColor` 0x3185d6 | 192,192,192 | BackLine | zero / reference line |
| `kGraphBaseLineColor` 0x3185be | 255,255,0 | Base | base line |
| `kGraphSustainColor` 0x3185b8 | 255,255,0 | Base | sustain segment (same colour) |
| `kGraphShadeColor` 0x3185d0 | 75,99,99 | Shade | |
| `kGraphFillColor` 0x3185ca | 0,255,128 | Fill | FillUnderGraph |
| `kGraphEnvFillColor` 0x3185b2 | 0,164,164 | EnvFill | FillEnvelopeGraph |
| `kGraphFilterTextColor` 0x3185ac | 255,255,0 | Text | slope text |
| `Color::kGraphFrame` 0x352f0c | 100,100,100 | Frame | frame |
| `kGraphFilterTextFont` 0x318560 | Arial 9 | | |

### Column fills

Both scan the scratch bitmap column by column (x < w, y < h), comparing
pixel colours:

- `FillUnderGraph`: from the first Border or Shade pixel of the column
  downwards, every pixel that is not Border/Shade becomes Fill. So the area
  under a black curve is filled down to the bottom.
- `FillEnvelopeGraph`: finds, per column, the span between the first curve
  pixel (Single or Sustain colour) and the last BackLine pixel below it (or, if
  the BackLine comes first, between it and the last curve pixel), and paints the
  pixels of that span that are not Single/Sustain/BackLine with EnvFill. So
  the area between an envelope and its zero line is filled, on either side.

Because they work on pixels, the result depends on the exact pixels of
QuickDraw's line algorithm; `rasterize()` uses Bresenham, which can differ
by a pixel on some slopes (fills may then differ by a pixel per column).

## 3. Port

`render(id, values)` returns the list of primitives (`Op`) in the original's
order and integer coordinates; `Frame` and `Blit`/`Bitmap` are in view
coordinates, `Rect`/`Line`/`Text`/fills in scratch coordinates unless
`Op::view`. Each graph is a port of its class' `UpdateGraphics` + `Draw`,
written from the x86 code (precision of every float/double operation, `int`
truncations, 16-bit wrap of `short` arguments), not from the decompiler's
C, which is often wrong about precision.

`draw(id, values)` returns, in view pixels: the background (filled), the fill
areas as staircase polygons (from `rasterize()`, exact per column), the lines
joined into polylines through pixel centres (x+0.5, y+0.5) and clipped to the
blitted interior, and the frame. Text ops (filter slope) are left to the UI:
see `Op::Text` (x0 = left from the 5 px/char width model, x1 = right edge,
y0 = baseline).

## 4. Verification

`tools/graphs/emulate.py` maps `G2Editor_i386` into unicorn (x86-32), calls
`CCustomObjectFactory::CreateCustom(id, ...)` (the real constructors run, with
`CPnlCustom::CPnlCustom` skipped and `CView::SetSize` recorded), then the
object's virtual `UpdateGraphics()` and `Draw()` found through its vtable. It
intercepts `GetDependencyValue` (returns the test values), the canvas
primitives (`DrawLine`, `DrawRect`, `DrawText`, `DrawBitmap`, `DrawFrame`,
`GetTextWidth` = 5 px per character), `CGraphGUI::Draw`, the two fills,
`Utils::IntToStr`, `operator new` and the libm imports (`sin cos atan floor
ceil floorf ceilf modf exp pow log __powidf2`, host libm, double-precision).
Everything else, including all `CPnlGraphABC` helpers, runs unmodified; a call
to any other function of the binary is an error, so nothing is silently
skipped. The script reads the user's own binary at run time and contains no
bytes or code from it.

`tools/graphs/cases.py` lists, per id, the dependency ranges (from
`data/modules.json`) and generates ~9800 value tuples (corners, a sweep of
every dependency over its range around two bases, 40 pseudo-random tuples).
`compare.py` diffs the port's primitives against the emulator's, line by line;
`golden.py` writes the test table (FNV-1a 64 digest of all primitives per id
plus a spot check). Reproduce:

```sh
python3 -m venv /tmp/g2venv && /tmp/g2venv/bin/pip install unicorn
/tmp/g2venv/bin/python -I tools/graphs/emulate.py original/mac/G2Editor_i386 info
/tmp/g2venv/bin/python -I tools/graphs/emulate.py original/mac/G2Editor_i386 draw 20 64 0 1 1
/tmp/g2venv/bin/python -I tools/graphs/golden.py original/mac/G2Editor_i386   # test table
```

Results: every case of every id matches the original exactly (9778 tuples,
~290,000 primitives), also with larger random samples (300..1500 extra tuples
per id) during development. The port gives identical output on arm64 and
x86_64 and at -O0..-O3 (`#pragma clang fp contract(off)`: the original rounds
every product, an FMA moves points by a pixel). Not covered by the golden
data: wave 10 (random) of id 24, whose generator state is global in the
original (see the oscillator section).

Caveat: `sin`, `cos`, `atan`, `exp`, `pow`, `log` are the host's; a last-ulp
difference from Apple's 2007 libm could move a point by one pixel at an exact
.5 boundary.

## 5. Graphs

### Envelopes: AD, ADSR, AHD families (ids 1, 3, 4, 5, 6, 7, 28)

Three drawing classes, each fed by small subclasses whose `UpdateGraphics`
copies dependency values into fields (constructors set the defaults).

| id | class (UpdateGraphics) | ctor | Draw | size | deps -> fields |
|---:|---|---|---|---|---|
| 1 | CPnlADEnvGraph (0x9602c) | 0x9a890 | CPnlADGraphABC::Draw 0xa6e78 | 45x24 | attack, decay, shape, AD/AR (`!= 0` = sustain), out type; 2 time segments |
| 6 | CPnlDEnvGraph (0x96136) | 0x9a77c | CPnlADGraphABC::Draw | 31x22 | decay, out type; attack 0, linear attack, curved decay, 1 segment |
| 3 | CPnlADSRGraph (0x95da2) | 0x9ac64 | CPnlADSRGraphABC::Draw 0xa6656 | 61x28 | A, D, S, R, shape, out type |
| 4 | CPnlModEnvGraph (0x95ebe) | 0x9ab60 | CPnlADSRGraphABC::Draw | 61x28 | A, D, S, R, out type; linear attack, curved decay |
| 5 | CPnlAREnvGraph (0x95f72) | 0x9aa68 | CPnlADSRGraphABC::Draw | 45x24 | A, R, attack curve (raw), decay curve (raw), out type; D = 0, S = 127 |
| 7 | CPnlHEnvGraph (0x961a4) | 0x9a46c | CPnlAHDGraphABC::Draw 0xa3c8c | 31x22 | hold, out type; A = D = 0, log attack (unused), 1 segment |
| 28 | CPnlAHDEnvGraph (0x97f0a) | 0x9a564 | CPnlAHDGraphABC::Draw | 58x28 | A, H, D, [shape], out type; 3 segments |

Shape (EnvAttackDecayShape 0..3: LogExp, LinExp, ExpExp, LinLin) is mapped
the same way in ADEnv/ADSR/AHDEnv: attack curve +0x88 = 2, 1, 0, 1 and linear
decay +0x8c = 0, 0, 0, 1 (other values leave the fields unchanged). Curve 1 is
`SingleLineTo`, 0/2 are `ExpTo` quarter ellipses whose flag depends on the
direction (rising: 0 -> false, 2 -> true; inverted: opposite). Decay/release
curves use `ExpTo(..., inverted)`. CPnlAHDEnvGraph reads the shape only when
the dependency vector has at least 5 items (EnvAHD); with 4 (ModAHD) dep 3 is
the out type and the constructor's linear attack / curved decay stay.

Common layout (all three Draws): frame the view; inner rect = view shrunk by
1 (w = W-2, h = H-2); DrawRect (0,0,w+1,h+1) Back into the scratch; back
line (BackLine ink) from x=1 to x=w at y = h-4 (out types 0/1) or y=3
(2/3); the ADSR family also has bipolar out types 4/5 whose back line sits at
the sustain level (`trunc(trunc(S*(h-5)/127 [or (h-5) - S*(h-5)/127] + 0.5) + 3)`).
Even out types rise from the bottom (rest y = trunc(h-5), peak y = 4), odd
ones are inverted. The pen starts at (2, rest); x positions are
`2 + value * scale`, rounded with `trunc(x + 0.5)` in double, where
`scale = (w-3) / (segments*127)` (AD: minus 5 px when the AR sustain stub
is shown; ADSR: `(w-3) / 461`, i.e. 3x127 + 80 for the sustain plateau).
Each segment is chained from the previous rounded x: attack, (hold,) decay,
then a straight line to x = w-1. ADSR draws the sustain plateau in Base ink
(kGraphSustainColor) from the decay end to `w-1 - round(R*scale)`, then the
release; if the decay end x is < 2 the pen is first moved to x+1. AD in AR
mode draws a Base-ink plateau at the peak level up to `w-1 - round(D*scale)`.
Out-of-range out types draw only the back line (if any). Finally
FillEnvelopeGraph(w, h) fills between the curve and the back line with
EnvFill, and the scratch is copied with DrawBitmap(view, 1, 1, scratch,
**src 1, 1**, w, h) - i.e. scratch pixel (1,1) lands at view (1,1).

Arithmetic: sizes, scales and products in float; each "+ 0.5" rounding is done
in double (`cvtss2sd`, `addsd 0.5`, `cvttsd2si`); the decay end of ADSR sums
`(double)(D*scale) + xAttack` in double, rounds to float, then +0.5.

Verified against the emulator: 100% of cases for all seven ids (e.g. 3965
cases with `--random 300`).


### Multi-stage envelopes and DX graphs (ids 17, 23, 29, 41)

Common frame for all four: `CGraphGUI::Draw` frame of the view, the view
rect shrunk by 1 (w = W-2, h = H-2), background `DrawRect` into the scratch
bitmap, curves in scratch coordinates, then `DrawBitmap(view, 1, 1, scratch,
src, w, h)`. 17/23/29 clear (0,0,w+1,h+1) and blit from scratch (1,1) (the
first scratch row/column is not shown); 41 clears (0,0,W-2,H-2) and blits from
(0,0). Envelopes (17, 23, 29) end with `FillEnvelopeGraph(w, h)` (EnvFill
between curve and BackLine). All arithmetic is single precision except where
noted; "round(f)" below is `(int)((double)f + 0.5)`.

Common envelope geometry (hf, wf = shrunk height/width as float):
`base = hf-2` (zero line y), `bottom = base-1` (level 0), `right = wf-1`,
levels from y = bottom (level 0) up to y = 4 (full scale).

| id | class | UpdateGraphics | Draw | ctor | size |
|---|---|---|---|---|---|
| 17 | CPnlMultiEnvGraph | 0x96212 | 0xa745a | 0x9a1d4 | 84x28 |
| 23 | CPnlADBDSREnvGraph | 0x973f6 | 0xa7b94 | 0x98824 | 84x28 |
| 29 | CPnlDXEnvGraph | 0x97554 | 0xa8a40 | 0x986d4 | 85x28 |
| 41 | CPnlOperatorGraph | 0x97646 | 0x9e930 | 0x985f4 | 63x24 |

The "Shape" mode of 17 and 23 maps to (rise, fall) segment types, 0 ExpTo
"exp", 1 straight line, 2 ExpTo "log": 0 -> (log, exp), 1 -> (line, exp),
2 -> (exp, exp), 3 -> (line, line) (other values keep the ctor default;
17: (line, line), 23: (log, exp)). ExpTo's flag is chosen per segment and is
negated when the graph is drawn upside down.

**17 EnvMulti** — deps L1 L2 L3 L4 T1 T2 T3 T4 SusPlac OutType Shape (+0x84..
0x8c, +0x90, Shape -> +0x94/+0x98; ctor: T2..T4 = 29, SusPlac 3, Shape (1,1)).
- Zero line (BackLine) from x 1 to wf: OutType 0/1 at y = base, 2/3 at y = 3,
  4 at `(float)(round(base-3)/2) + 3`, none for other values.
- OutType 1, 3, 5: upside down, y = level + 4; otherwise y = bottom - level.
- xs = (right - 2 - 5)/588, ys = (bottom-4)/127. Node x: x0 = 2, xk = x(k-1) +
  round(Tk*xs) (integer sums). Node levels round(L*ys): start and end at L4,
  nodes 1..3 at L1..L3, node 4 at L4.
- Segment k (pen at node k-1, starting at (2, y(L4))): rising (level grows)
  uses the rise shape, otherwise the fall shape; ExpTo flag = inverted for
  "exp", !inverted for "log" (a "log" fall draws nothing: unreachable).
- After segment SusPlac+1 (SusPlac 0..2): a Base line of width
  `round(inner - (T1+..+T4)*xs)` (inner = right-2) at that level, and all later
  nodes move right by that width. SusPlac 3 ("Trg"): after the last segment a
  Single line to x = right.

**23 EnvADDSR** — deps Attack Decay1 Break1 Decay2 Break2 Release Shape
SusPlac OutType (+0x84..0x89, Shape -> +0x8c/+0x90, +0x94, +0x98). +0x9c is
cleared by the ctor and never written (if set, the release would be a line).
- Zero line: OutType 0/1 at y = base, 2/3 at y = 3; OutType 4 (5 when inverted)
  draws it later at the sustain level, from x 1 to right+1. OutType > 5 draws
  no curve.
- xs = (right-2)/588, ys = (bottom-4)/127; break y = bottom - round(B*ys)
  (inverted: round(B*ys) + 4).
- Attack from (2, bottom) to (round(A*xs + 2), 4) with the rise shape
  (inverted: from (2,4) to bottom). Decay 1 to Break1 with the fall shape;
  decay 2 to Break2 with the rise shape if Break2 > Break1 else the fall shape.
  The running x is a float: non-inverted it restarts from the rounded attack
  x, inverted from the unrounded one (two slightly different code paths).
- Sustain (Base) at Break1 (SusPlac 0) or Break2 (SusPlac 1), ending at
  `right - round(D2*xs) - round(R*xs)` (SusPlac 0) or `right - round(R*xs)`;
  decay 2 continues from the sustain end.
- Release: when rel = round(R*xs) > 2 and Break2 is above the base, an
  `ArcTo` quarter ellipse from the sustain end to (right, bottom) (inverted: to
  (right, 4)); otherwise a Single line to (right, bottom) (inverted (right, 4)).

**29 EnvDX / Operator envelope** — deps R1 L1 R2 L2 R3 L3 R4 L4 (0..99,
+0x84..0x8b, ctor all 0).
- Zero line at y = base from x 1 to wf.
- Level curve: `lvl(L) = (int)(1.0471^L)` (`__powidf2`, low byte), 0 -> 0
  (0..95); y = bottom - lvl*ys with ys = (bottom-4)/100.
- Segment widths `|L(prev) - L(next)|/99 * (99 - R) * xs`, xs = (right-2)/480,
  accumulated in float from x = 2.
- Starts at (2, y(L4)); ExpTo(flag false) to L1, L2, L3; Base (sustain) line at
  L3 to `right - |L3-L4|/99*(99-R4)*xs`; ExpTo(false) to (right, y(L4)).

**41 Operator keyboard scaling** — deps BreakP L-Curve L-Depth R-Curve R-Depth
(+0x84..0x88). Curves 0 -Lin, 1 -Exp, 2 +Exp, 3 +Lin; depths 0..99.
- mid = (H-2)*0.5 (float, then double); grey BackLine from (1, mid) to W-2.
- bp = round(BreakP*(W-2)/99) (double).
- Left side: from (bp - W - 3, mid ± LDepth/2) (+ for curves 0/1, - for 2/3)
  to (bp, mid): a line for 0/3, ExpTo(flag = curve 1) for 1/2.
- Right side: from (bp, mid) to (bp + W - 1, mid ± RDepth/2) (+ for 0/1):
  0 line, 1 ExpTo(true), 2 ExpTo(false), 3 line.
- Both ends lie outside the view on purpose (clipped by the blit); no fill.


### Filter graphs (ids 20, 21, 40, 30, 31, 13, 35) and vocoder (8)

All draw into the scratch bitmap: background rect (0,0,W-2,H-2) in Back,
curve in Border, `FillUnderGraph(W-2,H-2)`, then blit to (1,1). Coordinates
below are scratch coordinates (view = scratch + 1). `W`,`H` = view size.

#### CPnlFilterGraphABC::Draw (0xa50b0) — ids 20, 21, 40 (52x28)

Fields (+0x84 gc, +0x85 res, +0x86 freq, +0x87 type, +0x88 slope dB,
+0x89 classic knee, +0x8a on):

| id | class | UpdateGraphics | deps -> fields | ctor |
|---|---|---|---|---|
| 20 | CPnlClassicFilterGraph | 0x95bf4 | freq, res, slope = 6 v+12, on | type 1 (LP), classic 1, gc 0 |
| 21 | CPnlNormalFilterGraph | 0x95a64 | freq, res, type = v+1, gc (GComp), slope = 12 v+12, on | classic 0 |
| 40 | CPnlStaticFilterGraph | 0x95b3a | freq, res, type = v+1, gc, on; slope 12 | classic 0 |

Geometry (float unless noted, the original's float/double mix is kept):
- `mid = round(0.4 (H-2))` is the 0 dB line; `xscale = (W-2)/128`,
  `yscale = (H-2)/128`; `freqf = freq/127*107 + 10` (cutoff on a 0..128
  axis); resonance peak `R = round(res/127 * gc * mid)` — so the peak only
  shows when the "gc" field is set (GComp on Nord/Static; never for Classic).
- BackLine from (0,mid) to (W+1,mid). Off: a Border line on the same row.
- LP: flat Border line at `y = mid + R/2` from x = -1 to the knee, then a
  6-point `BorderPolyBezierTo` (2 cubic segments) through the resonance bump
  (`y = mid + R/2 - res/12`) down to the bottom (H-2 + R/2) at
  `x ≈ xc + W+1 - slope`, i.e. the steeper the slope, the closer the drop.
  The knee differs: normal `xc - slope/2 + 14`, classic `xc + 7`.
- HP: the LP mirrored (x -> W - x) with cutoff `90 - freqf`.
- BP: 7-point `BorderPolyBezier`: left skirt from the bottom, peak, right
  skirt; the -10 / +10 offsets keep the skirts outside the box.
- BR: 13-point `BorderPolyBezier` (4 segments) at `mid + R/4`, notch of width
  `res/4 + 30` going down to `H + (2 slope - 24) yscale`.
- Text: slope ("12", "18", "24") in Arial 9, kGraphFilterTextColor (yellow),
  right-aligned: `x = W - GetTextWidth - 3`, baseline y = 7, drawn after the fill.

#### CPnlSmallFilterGraph::Draw (0xa4c4a) — ids 30 (LP), 31 (HP), 32x22

UpdateGraphics 0x959da: deps freq, on, slope mode (0..5) -> slope = 6 v + 6
(6..36 dB). Constructor 0x9b24c: type = 1 (id 30) or 3 (id 31), res/gc 0.
BackLine at y = 6 from 0 to W. Cutoff `xc = trunc(freq/127 (W-2))`. LP: flat
Border from (0,6) to (xc,6), one Bezier (xc,6) -> (xc+32-0.85 slope, 11) ->
(xc+32-0.8 slope, H-2). HP: mirrored, flat part from (W-2,6), control
points at xc-32+... Slope text right-aligned at W-3, baseline 7.

#### CPnlFeedbackFlt::Draw (0xa4236) — ids 13 (phaser), 35 (comb), 52x28

| id | class | UpdateGraphics | deps -> fields |
|---|---|---|---|
| 13 | CPnlPhaserGraph | 0x95d0c | spread (+0x85), FB (+0x86), notches 0..5 (+0x84), type (+0x87) |
| 35 | CPnlCombFltGraph | 0x95c94 | FB (+0x86), type (+0x87); notches 4, spread 120 |

`hm = (H-2)/2` is the reference line (BackLine). Per notch count n a table
gives (a, b, start x): 0 (6,10,-11), 1 (5,9,-11), 2 (4,8,-13), 3 (3,7,-16),
4 (2,6,-18), 5 (1,5,-21). `step = b spread/127` is half the notch spacing,
`amp = 0.8125 a spread/127` the control-point inset, the first notch starts
at `x0 = round(((W-5) - (n+1) step)/2)` so the comb is centred. The feedback
(64 = 0) moves the start/end level, the dip of the skirts and the notch depth
depending on type (0, 1: positive/negative feedback halves; 2: both; other:
a fixed table of notch shapes with `fb <= 76` / `> 76`). Drawing: left skirt
(3-point PolyBezierTo from (start x, startY)), one 6-point PolyBezierTo per
notch (down to `hm + depth`, back up), right skirt to (W-2 - start x,
startY), then FillUnder. No text.

#### CPnlVocoderGraph::Draw (0x9d92c) — id 8, 194x47

UpdateGraphics 0x965cc copies the 16 band values (0 = off, 1..16 = target
band). For each band i with value v != 0: Single line from (6 + 12 i, 45)
to (12 v - 6, -1) — analysis band at the bottom, synthesis band at the
top edge (y = -1 is clipped, so lines run off the top). No fill.


### Equalisers, level scaler, crossfade (ids 9, 36, 10, 37, 11, 12)

Common frame for all six: `CGraphGUI::Draw` frame of the view (0,0,W,H); scratch
background `DrawRect (0,0,W-2,H-2)` in Back; at the end the scratch interior
(W-2)x(H-2) is blitted to (1,1). All coordinates below are scratch coordinates.
`H`, `W` are the view height/width converted to float.

EQ vertical scale (9/36, 10, 37): `mid = (double)(int)((H-2)*0.5f + 0.5)` (0 dB
line, BackLine from (0,mid) to ((int)W, mid)); `scale = (float)((H-2) * 0.45 *
0.015625)` px per step; a gain g (64 = 0 dB) is at
`(int)((double)(float)(mid - (double)((float)(g-64)*scale)) + 0.5)`, i.e. ±0.45 of
the interior height for ±64. The curve is Border ink, then `FillUnderGraph(W-2,H-2)`
fills everything below it (Fill ink). Constructors set all fields to 64, but
UpdateGraphics always overwrites them.

| id | panel | class | UpdateGraphics | Draw | deps -> fields |
|---|---|---|---|---|---|
| 9, 36 | ShelvEQ (unmapped), Eq2Band | `CPnlEqHiLoGraph` 52x28 | 0x9691a | 0xa1880 | v0 lo gain +0x84, v1 hi gain +0x85 (ShelvEQ's 3rd value unused) |
| 10 | EqPeak | `CPnlEqMidGraph` 52x28 | 0x96890 | 0xa1c7e | v0 freq +0x84, v1 gain +0x85, +0x86 = abs(v2 - 127) |
| 37 | Eq3band | `CPnlEqHiMidLoGraph` 52x28 | 0x96984 | 0xa1112 | v0 lo, v1 mid gain, v2 mid freq, v3 hi (+0x84..+0x87) |
| 11 | LevScaler | `CPnlAmpProcGraph` 56x24 | 0x9682c | 0xa208c | v0 L.Gain, v1 BrkPnt, v2 R.Gain (+0x84..+0x86) |
| 12 | Mux8-1X | `CPnlXMuxGraph` 34x22 | 0x96efa | 0xa0726 | v0 X-Fade +0x84 |

- **EqHiLo**: `xs = ((W-2)-20)/128`. Low shelf: pen (-1, y(lo)), line to
  x(25)-16 where x(k) = xs*k+10, then `BorderPolyBezierTo` (3 points: corner
  ((int)(x(25)+0.5)-8, y(lo)), (same x, mid), (x(25), mid)). High shelf: line along
  0 dB to x(100), Bezier through (x(100)+8, mid), (x(100)+8, y(hi)) to
  (x(100)+16, y(hi)), line to ((int)W, y(hi)).
- **EqMid**: `xs = (W-6)/128`; centre `cx = freq*xs + 2`, half width
  `0.7*xs*abs(bw-127) + 4` (narrower for higher BWidth). Line along 0 dB to the left
  foot, 6-point `BorderPolyBezierTo`: (mid-foot, mid) x2, (cx, y(gain)), (mid-foot
  right, mid) x2, right foot; then 0 dB to (int)W. Integer halves use C division.
- **EqHiMidLo**: `xs = ((W-2)-20)/128`; low shelf corner at x(10), high at x(117)
  (x(k) = xs*k+10); mid peak at `mx = (int)(lx + (hx-lx)*f/127)` (float). For f < 40
  the low corner sits at `(1 - f/40)*` mid gain, for f > 96 the high corner at
  `(f-97)/30 *` mid gain (so the peak merges into the shelves at the ends). Each
  half of the peak is a 3-point Bezier; when its corner is at 0 dB and the half is
  wider than 12 px, the half is a 0 dB line plus a 6 px Bezier (quirk: the corner
  comparison is `mid == (double)corner_y`).
- **AmpProc** (LevScaler): no fill. BackLine (1,c)-((int)(W-2), c), c =
  (int)((H-2)*0.5). Break point `bx = (int)(brk*(W-2)/127 + 0.5)`; two Single lines
  from (bx, c): to (bx-W-3, c-(lg-64)/2) and to (bx+W-1, c-(rg-64)/2) (the far ends
  lie outside and are clipped, so they read as slopes of 1/2 px per step per W).
- **XMux**: interior w,h; `e = (int)(w/6)`, `r = (int)(w-e)`, `m = (int)(w/2)`,
  `off = (int)(fade/127 * (r-e)/2)` (double). Single lines: (e-2(m-e), h-1) ->
  (e-(m-e)+off, 3) -> (m-off, 3) -> (m+off, h-1); (m-off, h-1) -> (m+off, 3) ->
  (m-e+r-off, 3) -> (2(m-e)+r, h-1): two crossing trapezoids. BackLine verticals at
  x = e and x = r from y 1 to h-1.


### Shapers (ids 14, 15, 16, 38, 39)

All five are 34x22 transfer-curve graphs. Common structure: `CGraphGUI::Draw`
frame, scratch `DrawRect(0,0,W-2,H-2, Back)`, a centre cross of BackLines,
the curve in Single ink (no fill), `Blit(1,1,W-2,H-2)`. Curves run left to
right, input on x, output upwards. Port: `namespace shapers`. Verified against the emulator on every case of
tools/graphs/cases.py plus 400 random tuples each: all match.

| id | class | UpdateGraphics | ctor | Draw | deps -> fields |
|---|---|---|---|---|---|
| 14 Clip | CPnlDistAGraph | 0x9639c | 0x9a0f4 | 0xa39cc | Shape -> +0x84, Clip -> +0x85 |
| 15 Overdrive | CPnlDistBGraph | 0x96406 | 0x9a014 | 0xa3080 | Shape -> +0x84, Drive -> +0x88 (double, `(float)v/127`) |
| 16 WaveWrap | CPnlWrapGraph | 0x96486 | 0x99f34 | 0xa2a50 | WrpGain -> +0x84 (double, `v*0.125f+1`) |
| 38 ShpExp | CPnlShapeBGraph | 0x964f8 | 0x99e38 | 0xa26e4 | Shape -> +0x85, Curve -> +0x84 (ctor: 0x85=64, 0x84=0) |
| 39 Saturate | CPnlSaturateGraph | 0x96562 | 0x99d3c | 0xa2330 | Sat -> +0x85, Curve -> +0x84 (ctor: 0x85=64, 0x84=0) |

- **14 Clip**: cross at (W/2-1, H/2-1) (float, truncated; the lines run to
  W and H, clipped by the blit). With r = Clip/127, dx = round((W/2-1) r),
  dy = round((H/2-1) r): line from (W, dy) to (W-dx-1, dy) (flat top), diagonal
  to (dx, H-dy-2), then to x = -1 at the same height (Shape 1, symmetric) or
  at H-1 (Shape 0, asymmetric: bottom not clipped).
- **15 Overdrive**: g = Drive/127, inner size w2 = W-2, h2 = H-2, centre
  (w2/2, h2/2), cross at their floor. Curve y(t) = t - 2048/3 g^3 t^3, x =
  2 w2 t + centre, y = centre - 2 h2 y(t). g <= 1/16: start at t = -1/4, 20
  points t = (-9..10) * 0.025; Shape 0 clamps the lower half at
  limit = round(H - 7g). g > 1/16: t range scaled to 1/(16 g) (step
  1/(160 g)), starts with a flat line from x = -2, uses ceil() for y, ends with
  a line to x = W; Shape 0 then has no clamp but Shape 1 does (the two
  branches swap which shape is clamped, faithfully kept).
- **16 WaveWrap**: gain = 1 + Gain/8. Half width mx = (W-3)/2, amplitude
  a = (H-3)/2. floor(gain) segments of width mx/gain, the k-th (1-based) going
  down by a when k&3 is 0/1 and up when 2/3, then a partial segment of
  frac(gain) of a step (down for count&3 in {0,3}, up for {1,2}). Each segment
  is drawn from x+mx and mirrored (mx-x, H-3-y): a triangle wave folded around
  the centre. Integer y of the mirror uses the already-rounded y.
- **38 ShpExp**: inner w,h (shorts), hx = w/2, hy = h/2. n = Curve+2,
  s = Shape/127. Per column x in [0, w-1): u = (x-hx)/hx,
  val = ((2u)^n s (sign forced odd) + 2(1-s) u) / 2 * hy + hy + 1, truncated
  to short, clamped to [-1, h+1], y = h - val. First point is a MoveTo.
- **39 Saturate**: same plotting; u = 2(x-hx)/hx; u >= 1: (1-s)(u-1)+1;
  u <= -1: (1-s)(u+1)-1; otherwise z = u+1 wrapped to [-1,1),
  val = z^n (odd) s + z(1-s) + 1, wrapped again (>= 1 -> -2). s is fabs'd.

Precision: all of these mix float and double; e.g. Overdrive computes the
polynomial in float and the centre offset in double. FMA contraction must be
off (clang on arm64 fuses `a*b+c` by default and moved Overdrive points by
1 px); head.cpp now has `#pragma clang fp contract(off)`.


### Oscillator / LFO waveform graphs (CPnlWaveformGraphABC)

Port: `namespace osc` in graphs.cpp.

All five classes derive from `CPnlWaveformGraphABC` (ctor 0x98a6c). Object
fields: `+0x84` wave (byte), `+0x86` shape (short), `+0x88` phase (byte,
0..127 = one period), `+0x8c` LFO output type (uint). Ctor defaults: 0, 0, 0,
**4**. Every `Draw()` is: frame; shrink the view rect by 1 (w x h = (W-2) x
(H-2)); `DrawRect(0,0,w,h, Back)` into the scratch bitmap; [`DrawBackLine`];
one wave helper; blit to (1,1,w,h). No fills, no text. All curves are
`SingleLineTo` (green), except `DrawBlank` / `DrawBackLine` (BackLine grey).

| id | class | UpdateGraphics | Draw | size | Dependencies -> fields |
|---|---|---|---|---|---|
| 18 | CPnlOscCGraph (OscShpB) | 0x970ec | 0x9ffbe | 35x22 | v0 Shape -> shape, v1 Wave (S0) -> wave |
| 32 | CPnlOscSinShapeGraph (OscShpA) | 0x97158 | 0x9fc20 | 38x22 | v0 Shape -> shape, v1 Wave -> wave |
| 33 | CPnlLfoBGraph (LfoB) | 0x972cc | 0xa03b0 | 52x28 | v0 Wave, v1 Phase, v2 OutType; shape = 0 |
| 34 | CPnlLfoCGraph (LfoShpA) | 0x97358 | 0xa020e | 52x28 | v0 Wave, v1 Shape -> shape = 2*v-128, v2 Phase, v3 OutType |
| 24 | CPnlLfoGraph (no panel) | 0x9722e | 0xa053e | 35x20 | v3 Wave, v0 Shape -> 2*v-128, v1 Phase, v2 OutType |

Oscillators (18/32) never set phase/out type, so they draw with phase 0 and
out type 4 (the default). Wave -> helper:

- 18: 0 DualSine(0.25), 1 DualSine(0), 2 Dsf(1), 3 Dsf(2), 4 TweekTri,
  5 DoubleSaw, 6 Square, 7 Pulse, else Blank.
- 32: 0..4 as 18, 5 Pulse, else Blank.
- 33: BackLine, then 0 Sine, 1 Tri, 2 Saw, 3 Square, else Blank.
- 34: BackLine, then 0 Sin2Saw, 1 CosBell, 2 TriBell, 3 Tri2Saw (all through
  DrawNormalizedGraph), 4 SqrTri, 5 Square, else Blank.
- 24: BackLine, then 0 Sine, 1 DualSine(0.25), 2 DualSine(0.5), 3 Tri,
  4 TweekTri, 5 Square, 6 Pulse, 7 Saw, 8 SqrTri, 9 Blank, 10 Random, else
  nothing.

Output type: the helpers test `(1 << type) & 0x15` (types 0, 2, 4: curve
upright / unipolar-positive style) and `& 0x2a` (1, 3, 5: inverted); DrawBackLine
puts the reference line at y = h-2 for types 0/1, y = 1 for 2/3, y = (h-1)/2
for 4/5, y = 0 for >= 6. Saw, Pulse and SqrTri draw nothing for types >= 6.

Helpers (CPnlWaveformGraphABC, coordinates in the scratch bitmap, x from about
0 to w-2, mostly float arithmetic, `+0.5` then truncation for pixel positions):

| helper | addr | what |
|---|---|---|
| DrawBackLine | 0xa017c | BackLine across (0..w-1) at the type's zero level |
| DrawBlank | 0x9fba8 | BackLine (1..w-3) at h/2-1 |
| DrawSine | 0x9d74a | one sine period over w-1 px, amplitude round((h-3)/2), phase shift |
| DrawTri | 0x9cf04 | triangle, peaks at 1/4 and 3/4 period |
| DrawSaw | 0x9c524 | vertical-edge saw, one period of w-2 px |
| DrawSquare | 0x9cb2e | square, duty = (shape/128+1)/2 |
| DrawPulse | 0x9c65a | pulse from the middle line; width from shape, mirrored for shape < 0 |
| DrawTweekTri | 0x9ccf2 | triangle with movable apex (shape) |
| DrawSqrTri | 0x9c21c | trapezoid; period rounded down to a multiple of 4; slope from shape |
| DrawNormalizedGraph | 0x9d0cc | x = 2..w-2: y = (h-2) - f(t, s)*(h-4), f inverted for types 1/3/5; t = (x-2)/(w-4) + phase/127, s = (shape+128)/256*0.9+0.05 |
| DrawDsf | 0x9c980 | sin(t)/(1 - 2a cos(n t) + a^2), a = shape/128, n = 1 or 2, clamped to [2, h-1] |
| DrawDualSine | 0x9d30e | two half sines of lengths r*(w-2) and (2-r)*(w-2), r = clamp(shape/127+1, 0.1, 1.9); the argument (0, 0.25, 0.5) offsets the start |
| DrawDoubleSaw | 0x9fdc2 | BackLine at mid, then a falling saw with a second wrap set by shape |
| DrawRandom | 0x9c0ce | 5 px random steps from the global LCG `_gRandom` (0x3189c8, x*0xbb38435+0x3619636b) |

Shape functions (global, `float f(double t, double s)`): Sin2Saw 0x9b364
((1-cos(pi*Tri2Saw))/2), CosBell 0x96f4e, TriBell 0x96fec, Tri2Saw 0x9708e
(rise t/s, fall (1-t)/(1-s)).

Quirks kept: DrawRandom's state is global and persistent in the original (the
picture changes on every redraw); the port restarts it at 0 (the value after
launch) on each render. Pulse/Saw/SqrTri draw nothing for out types >= 6.
Ghidra's C is wrong about precision in several places (DualSine, Pulse,
NormalizedGraph mix float and double); the port follows the x86 code.

Verification: `tools/graphs/compare.py` all cases match for 18 (134), 32 (131),
33 (140), 34 (233), 24 (298); plus 1,000 extra random tuples over wider
ranges (out types 0..7, waves 0..9, shape 0..255) and DrawRandom from a fresh
emulator: identical.


### DX algorithm, noise, random graphs, pulse oscillator (ids 19, 42, 43, 44, 45)

#### 19 CPnlPulseOscGraph (PulseOsc, unmapped panel)

UpdateGraphics 0x971c4, ctor 0x98954 (35x20; +0x84 wave = 0, +0x85 shape = 64),
Draw 0x9fa0e. Deps "5,7,10,11,S0": only dep 0 (shape, +0x85) and dep 1 (wave,
+0x84) are read. Draw: frame, background rect (0,0,W-2,H-2), then by wave:
0 DrawSine 0x9f81e, 1 DrawModSine 0x9f512, 2 DrawTri 0x9f07a, 3 DrawSquare
0x9bf66, 4 DrawTweekSaw 0x9ece2, 5 DrawSaw 0x9bcc4, 6 DrawTweekTri 0x9ee9c,
>6 nothing; then blit (1,1,W-2,H-2). The helpers get the shrunk rect
(w = W-2, h = H-2) and draw one period in Single ink, most with a BackLine
zero line first:
- Sine: period w-1, amplitude round((h-3)/2), y = (1 - sin) * amp per pixel.
- ModSine: two half periods, lengths (2 - s)*(w-2) and max(3, s*(w-2)),
  s = shape/64; ends with a line back to the zero line.
- Tri / TweekTri: peaks at x = q / 3q (q = (w-2)/4); TweekTri moves them to
  t and 4q - t with t = q*(shape+1)/64.
- Square: pulse width shape/128 of the period, top y = 1, bottom h-4.
- TweekSaw: falling saw of height 2*floor((h-3)/4), reset point moved by shape.
- Saw: one ramp 0 -> h-3.
The starting x of most helpers is `(int)(-w * 0.0 / 128 + 0.5)` = 0 (a phase
term multiplied by 0). Mixed float/double: positions are float, sin arguments
double with float-rounded 2*pi/period.

#### 42 CPnlDXRouterGraph (DXRouter)

UpdateGraphics 0x976aa (+0x84 = dep 0, algorithm 0..31), Draw 0x976fe,
size 139x67. Frame, then DrawBitmap of resource 993 (assets/clavia/cbmp/993.png,
32 cells of 137x65 stacked every 66 px): source (0, algorithm*66), size
137x65, to the view at (1,1). No scratch bitmap.

#### 43 CPnlTunedNoiseGraph (OscNoise)

UpdateGraphics 0x977ec (+0x84 = dep 0, Width), Draw 0x9e558, size 31x22.
Background (0,0,W-2,H-2); base line y0 = round(H-4) as BackLine across
(0..W). k = (W-6)/128, centre c = 64k+1, spread s = k*width. Border path:
pen (-1,y0) -> (round(c - 0.7s - 4), y0) -> PolyBezierTo of 6 points
(two cubic halves meeting at the peak (round(c), round(y0 - 0.9*(H-4)))),
-> (round(c + 0.7s + 4), y0) -> (W, y0). FillUnderGraph(W-2,H-2), blit.
So the bump is a bell whose base widens with Width.

#### 44 CPnlRndDistributionGraph (no panel)

UpdateGraphics 0x97840 (+0x84 = dep 0), Draw 0x97894, size 35x22. Draws
directly on the view (no scratch bitmap): frame, background rect of the
shrunk rect (1,1,W-1,H-1), then W/2-1 bars as DrawFrame rects (Single ink),
3 px wide, every 2 px from x = 1 (they overlap by one column), bottom H-1,
top = (H-1) - level*(H-2), level = p*d^2 + (1-d^2)*(1-p), p = v/128,
d = (x+1 - centre)/halfwidth. p = 0: a hump in the middle; p = 1: a U.
In the port: Op::Kind::Frame with view = true and Ink::Single
(`Canvas::viewFrame`).

#### 45 CPnlRndTrigGraph (RndTrig)

UpdateGraphics 0x97a4e (+0x84 = dep 0, Prob), Draw 0x97aa2, size 21x22.
On the view: frame, background (1,1,W-1,H-1), a Single base line
(1,H-2)-(W-2,H-2), then vertical Single lines x = 1+2k from y = 2 to H-2 for
k = 0..9 when Prob > threshold[k] = {120, 96, 24, 60, 36, 0, 72, 108, 48, 84}
(lines appear in a scattered order; Prob 0 shows none, 127 all ten). No
randomness is involved.


### Non-graph ids (custom controls)

Created by the same factory but they are interactive `CPnlCustom` controls,
not `CPnlGraphABC`; not ported (`isGraph()` false, `render()` empty):

| id | class | panel | what it is |
|---:|---|---|---|
| 2 | `CPnlSeqClrRnd` (ctor 0x94732, 21x30) | SeqA (unmapped) | two `CTextPushButton`s "Clr" / "Rnd" acting on the 16 step values (+ param 34) |
| 22 | `CPnlVocoderPreset` (ctor 0x9354a) | Vocoder | push buttons "-2" "-1" "0" "+1" "+2" "Inv" "Rnd" that rewrite the 16 band assignments |
| 25 | `CPnlNoteSeqZoom` (ctor 0x93342, 11x91, Draw 0x926f8) | SeqNote | zoom switch: draws cell (2-zoom)*11 of bitmap 877 (0x36d), 11x91, and rescales the step sliders (`UpdateDependentSliders`) |
| 26 | `CPnlNoteSeqOffset` (ctor 0x930be, 21x25) | SeqNote | `CLeftRightButton`s that scroll the visible note range |
| 27 | `CPnlNoteSeqClrRnd` (ctor 0x9440e, 21x30) | (none) | "Clr" / "Rnd" for the note sequencer |
| 46 | `CPnlDrumPresetSelector` (ctor 0x94fa2, 64x21) | DrumSynth | `CUpDownButton`s choosing a drum preset; `PresetComparison` (0x90f7c) matches the 15 parameters against the preset table |
