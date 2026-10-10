#include "LiveView.h"

#include <algorithm>
#include <cmath>

namespace g2ui {

const char* panelLabel(PanelButton b)
{
    static const char* const names[] = {
        "System (Shift: Dump One)", "Patch (Shift: Dump CC)", "Store (Shift: Store As...)", "Display Mode (Shift: Sub Func.)",
        "Navigator Up", "Navigator Left (Del)", "Navigator Right (Ins)", "Navigator Down (Sort Mode)",
        "Load Patch", "Perf. Mode (Shift: Perf. Transfer)",
        "Slot A", "Slot B", "Slot C", "Slot D",
        "KB Split (Shift: Split Point)",
        "Octave Shift Down", "Octave Shift Up", "KB Hold (Shift: Panic)", "Focus/Copy (Shift: Assign/Paste)", "Shift (Clear)",
        "Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6", "Button 7", "Button 8",
        "Variation 1", "Variation 2", "Variation 3", "Variation 4", "Variation 5", "Variation 6", "Variation 7",
        "Variation 8", "Morph (Shift: Vari.Init)",
        "Patch Settings (Shift: Global Panel)",
        "Parameter Page A (Osc)", "Parameter Page B (LFO)", "Parameter Page C (Env)", "Parameter Page D (Filter)",
        "Parameter Page E (Effect)",
        "Parameter Column 1", "Parameter Column 2", "Parameter Column 3",
    };
    static_assert(std::size(names) == static_cast<std::size_t>(PanelButton::Count));
    return names[static_cast<std::size_t>(b)];
}

namespace {

// ---- The panel's geometry -------------------------------------------------------------------------------------
// Panel units, measured on the v1.2 manual's drawing of the G2/G2X front panel (the System functions section in its
// own units, the Sound functions section in units 1.3947 times smaller, offset by 1270: sx/sy below convert them).
// The colours and the printed labels follow photos of a production G2X, which differ from the drawing.

constexpr float kSnd = 1.3947f, kSndX = 1270.0f;
constexpr float kPanelW = 3890.0f, kPanelH = 1050.0f; // the navy frame and the red strip below it

juce::Rectangle<float> R(float x1, float y1, float x2, float y2) { return {x1, y1, x2 - x1, y2 - y1}; }
float sx(float x) { return kSndX + x * kSnd; }
float sy(float y) { return y * kSnd; }
juce::Rectangle<float> SR(float x1, float y1, float x2, float y2) { return R(sx(x1), sy(y1), sx(x2), sy(y2)); }
juce::Point<float> SP(float x, float y) { return {sx(x), sy(y)}; }

constexpr float kKnobX[kPanelKnobs] = {107, 288, 509, 690, 912, 1094, 1316, 1497}; // Sound section units
constexpr float kVarX[8] = {105, 213, 320, 428, 537, 645, 753, 861};
constexpr float kPageY[5][2] = {{325, 367}, {398, 440}, {470, 512}, {542, 584}, {616, 658}};
constexpr float kLcdX[4] = {0, 400, 803, 1210};

// G2fresh's own look, not Clavia's: graphite surfaces, one cyan accent for what lights up, dark displays with
// light characters, flat keys and buttons. The layout is the instrument's; the colours are not.
const juce::Colour kBody{0xff17191d},   // around the panel and the keyboard (the red body on the instrument)
    kNavy{0xff0f1114},                   // the panel's frame
    kCheek{0xff1f2227},                  // the controllers' side
    kPanel{0xff2a2e35},                  // the panel
    kInset{0xff23272d},                  // its grouped areas
    kLcdLit{0xff0b0f13}, kLcdOff{0xff14171b}, kLcdInk{0xff9fe8ff}, kBezel{0xff07080a},
    kButton{0xff3a3f47}, kStore{0xffe0604f}, kLedOff{0xff3b4048}, kLedOn{0xff38d6ff},
    kText{0xffd3d7de}, kSubText{0xff7e8795}, kKnobRing{0xff1b1e23}, kSegment{0xff343940}, kKnobCap{0xff464c55},
    kWood{0xff8a929e}, kStone{0xff5c636e}, kGreen{0xff38d6ff};

enum class Style { Normal, Red, Light };

struct ButtonSpec {
    PanelButton id;
    juce::Rectangle<float> r;
    Style style = Style::Normal;
};

std::vector<ButtonSpec> buttons()
{
    using B = PanelButton;
    std::vector<ButtonSpec> v = {
        {B::System, R(585, 152, 712, 210)}, {B::Patch, R(747, 152, 874, 210)}, {B::Store, R(909, 152, 1036, 210), Style::Red},
        {B::DisplayMode, R(1118, 150, 1246, 212)},
        {B::NavUp, R(212, 352, 338, 410)}, {B::NavLeft, R(148, 446, 274, 505)}, {B::NavRight, R(278, 446, 406, 505)},
        {B::NavDown, R(212, 540, 338, 598)},
        {B::LoadPatch, R(444, 560, 504, 684)}, {B::PerformanceMode, R(1166, 324, 1228, 446)},
        {B::SlotA, R(546, 597, 672, 656)}, {B::SlotB, R(681, 597, 808, 656)}, {B::SlotC, R(816, 597, 942, 656)},
        {B::SlotD, R(951, 597, 1078, 656)}, {B::KbSplit, R(1128, 597, 1255, 656)},
        {B::OctaveDown, R(454, 859, 582, 918)}, {B::OctaveUp, R(586, 859, 712, 918)},
        {B::KbHold, R(780, 859, 907, 918)}, {B::FocusCopy, R(975, 859, 1102, 918)},
        {B::Shift, R(1184, 797, 1246, 919), Style::Light},
        {B::Morph, SR(941, 616, 1031, 658)}, {B::PatchSettings, SR(1654, 110, 1744, 152)},
        {B::Column1, SR(1282, 615, 1370, 657)}, {B::Column2, SR(1389, 615, 1477, 657)},
        {B::Column3, SR(1501, 615, 1589, 657)},
    };
    for (int k = 0; k < kPanelKnobs; ++k)
        v.push_back({static_cast<B>(static_cast<int>(B::Knob1) + k), SR(kKnobX[k] - 45, 478, kKnobX[k] + 45, 520)});
    for (int i = 0; i < 8; ++i)
        v.push_back({static_cast<B>(static_cast<int>(B::Var1) + i), SR(kVarX[i] - 45, 616, kVarX[i] + 45, 658)});
    for (int i = 0; i < 5; ++i)
        v.push_back({static_cast<B>(static_cast<int>(B::PageA) + i), SR(1654, kPageY[i][0], 1744, kPageY[i][1])});
    return v;
}

struct LedSpec {
    PanelLed id;
    juce::Point<float> c;
};

std::vector<LedSpec> leds()
{
    using L = PanelLed;
    std::vector<LedSpec> v = {
        {L::Midi, {160, 308}}, {L::MicLow, {412, 309}}, {L::MicMid, {455, 309}}, {L::MicHigh, {497, 309}},
        {L::System, {594, 117}}, {L::Patch, {756, 117}}, {L::Store, {918, 117}}, {L::SubFunc, {1218, 246}},
        {L::PerformanceMode, {1218, 297}}, {L::LoadPatch, {472, 534}},
        {L::SlotA, {598, 561}}, {L::SlotB, {734, 561}}, {L::SlotC, {870, 561}}, {L::SlotD, {1005, 561}},
        {L::FocusA, {598, 687}}, {L::FocusB, {734, 687}}, {L::FocusC, {870, 687}}, {L::FocusD, {1005, 687}},
        {L::KbSplit, {1132, 560}},
        {L::Octave1, {498, 793}}, {L::Octave2, {540, 793}}, {L::Octave3, {582, 793}}, {L::Octave4, {624, 793}},
        {L::Octave5, {665, 793}}, {L::KbHold, {790, 824}},
        {L::Morph, SP(948, 590)}, {L::PatchSettings, SP(1673, 85)}, {L::GlobalPanel, SP(1673, 178)},
        {L::Column1, SP(1325, 590)}, {L::Column2, SP(1432, 590)}, {L::Column3, SP(1540, 590)},
        {L::Split1, {1451, 1022}}, {L::Split2, {2255, 1022}}, {L::Split3, {2848, 1022}}, {L::Split4, {3657, 1022}},
    };
    constexpr float varLedX[8] = {104, 212, 320, 428, 536, 644, 750, 858};
    constexpr float pageLedY[5] = {348, 420, 492, 565, 638};
    for (int k = 0; k < kPanelKnobs; ++k) {
        v.push_back({static_cast<L>(static_cast<int>(L::KnobUpper1) + k), SP(kKnobX[k], 420)});
        v.push_back({static_cast<L>(static_cast<int>(L::KnobLower1) + k), SP(kKnobX[k], 453)});
    }
    for (int i = 0; i < 8; ++i)
        v.push_back({static_cast<L>(static_cast<int>(L::Var1) + i), SP(varLedX[i], 590)});
    for (int i = 0; i < 5; ++i)
        v.push_back({static_cast<L>(static_cast<int>(L::PageA) + i), SP(1634, pageLedY[i])});
    return v;
}

// Text printed on the panel: white headings and blue second functions.
struct LabelSpec {
    const char* text;
    juce::Point<float> c;
    bool blue = false;
    float size = 30.0f;
};

std::vector<LabelSpec> labels()
{
    // As printed on a production G2X; `blue` marks the second functions (Shift), printed lighter.
    std::vector<LabelSpec> v = {
        {"Master Level", {218, 118}}, {"Mic Level", {455, 118}}, {"-20 -12 0dB", {455, 262}, false, 24},
        {"In 1 Level", {455, 352}, true, 24}, {"MIDI", {112, 306}}, {"Navigator", {275, 316}},
        {"System", {660, 118}}, {"Patch", {810, 118}}, {"Store", {972, 118}},
        {"Dump One", {648, 238}, true}, {"Dump CC", {810, 238}, true}, {"Store As...", {974, 238}, true},
        {"Display Mode", {1186, 116}}, {"Sub Func.", {1153, 244}, true},
        {"Perf.Mode", {1162, 298}, false, 22}, {"Perf.", {1195, 472}, true, 26}, {"Transfer", {1195, 496}, true, 26},
        {"Del", {158, 531}, true}, {"Ins", {395, 531}, true}, {"Sort Mode", {275, 625}, true},
        {"Load", {474, 472}, false, 26}, {"Patch", {474, 498}, false, 26},
        {"Keyboard Assign", {814, 510}}, {"A", {622, 563}}, {"B", {760, 563}}, {"C", {896, 563}}, {"D", {1032, 563}},
        {"Active Slots/Focus", {814, 727}, true}, {"KB Split", {1205, 563}}, {"Split Point", {1188, 687}, true},
        {"Octave Shift", {584, 833}}, {"Global", {518, 952}, true}, {"KB Hold", {857, 824}}, {"Panic", {846, 952}, true},
        {"Focus/Copy", {1041, 826}}, {"Assign/Paste", {1041, 952}, true}, {"Shift", {1215, 765}}, {"Clear", {1215, 952}, true},
        {"Master Clock", SP(106, 74)}, {"Voice Mode", SP(289, 74)}, {"Arpeggiator", SP(601, 74)}, {"Vibrato", SP(913, 74)},
        {"Glide", SP(1096, 74)}, {"Pitch Bend", SP(1317, 74)}, {"Patch Level", SP(1499, 74)},
        {"VARIATIONS", SP(485, 566)}, {"Morph", SP(990, 591)}, {"Vari.Init", SP(986, 684), true},
        {"Patch", SP(1714, 74)}, {"Settings", SP(1724, 93)}, {"Global", SP(1716, 172), true}, {"Panel", SP(1712, 190), true},
        {"PARAMETER", SP(1708, 277)}, {"PAGES", SP(1680, 297)},
        {"1", SP(1351, 591)}, {"2", SP(1459, 591)}, {"3", SP(1567, 591)},
        {"Osc", SP(1776, 346)}, {"LFO", SP(1776, 419)}, {"Env", SP(1776, 491)},
        {"Filter", SP(1781, 562)}, {"Effect", SP(1781, 636)},
    };
    const char* const sources[8] = {"Wheel", "Velocity", "Keyboard", "Aft.Touch", "G.Wh1/Sus.P", "Ctrl.Pedal", "Pitch Stick", "G.Wheel 2"};
    for (int i = 0; i < 8; ++i) {
        static const char* const digits[8] = {"1", "2", "3", "4", "5", "6", "7", "8"};
        v.push_back({digits[i], SP(kVarX[i] + 23, 591)});
        v.push_back({sources[i], SP(kVarX[i] + 1, 684), true, 26});
    }
    for (int i = 0; i < 5; ++i) {
        static const char* const letters[5] = {"A", "B", "C", "D", "E"};
        v.push_back({letters[i], SP(1634, kPageY[i][0] + 5)});
    }
    return v;
}

void drawButton(juce::Graphics& g, juce::Rectangle<float> r, Style style, bool down)
{
    const float corner = std::min(r.getWidth(), r.getHeight()) * 0.28f;
    auto face = style == Style::Red ? kStore : style == Style::Light ? juce::Colour(0xff5a616c) : kButton;
    g.setColour(down ? face.darker(0.35f) : face);
    g.fillRoundedRectangle(r, corner);
    g.setColour(down ? kLedOn : juce::Colours::white.withAlpha(0.08f));
    g.drawRoundedRectangle(r.reduced(1.5f), corner, down ? 3.0f : 2.0f);
}

void drawLed(juce::Graphics& g, juce::Point<float> c, float level, float radius)
{
    g.setColour(kLedOff.interpolatedWith(kLedOn, std::clamp(level, 0.0f, 1.0f)));
    g.fillEllipse(juce::Rectangle<float>(radius * 2, radius * 2).withCentre(c));
    if (level > 0.05f) {
        g.setColour(kLedOn.withAlpha(0.35f * level));
        g.fillEllipse(juce::Rectangle<float>(radius * 3.4f, radius * 3.4f).withCentre(c));
    }
    g.setColour(juce::Colours::black.withAlpha(0.45f));
    g.drawEllipse(juce::Rectangle<float>(radius * 2, radius * 2).withCentre(c), 1.5f);
}

// A pot or encoder cap: a flat dark disc with a thin rim, and an accent pointer at `angle` (radians, 0 = up,
// clockwise) for the pots.
void drawCap(juce::Graphics& g, juce::Point<float> c, float radius, std::optional<float> angle)
{
    auto r = juce::Rectangle<float>(radius * 2, radius * 2).withCentre(c);
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillEllipse(r.translated(0, radius * 0.06f));
    g.setGradientFill(juce::ColourGradient(kKnobCap.brighter(0.15f), r.getTopLeft(), kKnobCap.darker(0.25f), r.getBottomRight(), false));
    g.fillEllipse(r);
    g.setColour(juce::Colours::white.withAlpha(0.1f));
    g.drawEllipse(r.reduced(1.5f), 2.0f);
    if (angle) {
        g.setColour(kLedOn);
        g.drawLine({c.getPointOnCircumference(radius * 0.35f, *angle), c.getPointOnCircumference(radius * 0.85f, *angle)}, radius * 0.1f);
    }
}

void drawDisplay(juce::Graphics& g, juce::Rectangle<float> bezel, juce::Rectangle<float> lcd, const PanelDisplay& d, bool lit)
{
    g.setColour(kBezel);
    g.fillRect(bezel);
    g.setColour(lit ? kLcdLit : kLcdOff); // backlit while the G2 runs
    g.fillRect(lcd);
    if (d.width > 0 && d.height > 0 && d.pixels.size() >= static_cast<std::size_t>(d.width * d.height)) {
        const float px = std::min(lcd.getWidth() / static_cast<float>(d.width), lcd.getHeight() / static_cast<float>(d.height));
        const auto origin = lcd.getCentre() - juce::Point<float>(px * static_cast<float>(d.width), px * static_cast<float>(d.height)) / 2;
        g.setColour(kLcdInk);
        for (int y = 0; y < d.height; ++y)
            for (int x = 0; x < d.width; ++x)
                if (d.pixels[static_cast<std::size_t>(y * d.width + x)] != 0)
                    g.fillRect(origin.x + static_cast<float>(x) * px, origin.y + static_cast<float>(y) * px, px * 0.9f, px * 0.9f);
    } else if (d.rows > 0 && d.columns > 0) {
        const float lineH = lcd.getHeight() / static_cast<float>(d.rows);
        juce::Font font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), lineH * 0.82f, juce::Font::bold));
        // fit the columns
        const float charW = juce::GlyphArrangement::getStringWidth(font, "M");
        if (charW * static_cast<float>(d.columns) > lcd.getWidth() * 0.96f)
            font = font.withHorizontalScale(lcd.getWidth() * 0.96f / (charW * static_cast<float>(d.columns)));
        g.setFont(font);
        g.setColour(kLcdInk);
        for (int row = 0; row < d.rows && row < static_cast<int>(d.text.size()); ++row)
            g.drawText(juce::String(d.text[static_cast<std::size_t>(row)]),
                       lcd.withY(lcd.getY() + lineH * static_cast<float>(row)).withHeight(lineH).reduced(lcd.getWidth() * 0.02f, 0),
                       juce::Justification::centredLeft, false);
    }
    // glass
    g.setColour(juce::Colours::white.withAlpha(0.025f));
    g.fillRect(lcd.withHeight(lcd.getHeight() * 0.4f));
}

} // namespace

// ---- Panel ---------------------------------------------------------------------------------------------------

class LiveView::Panel : public juce::Component, public juce::SettableTooltipClient {
public:
    explicit Panel(LiveView& owner) : owner_(owner), buttons_(buttons()), leds_(leds()), labels_(labels()) {}

    float masterLevel = 1.0f; // the OS starts with the knob all the way up (Machine::Options::masterVolume)

    void paint(juce::Graphics& g) override
    {
        const auto& s = owner_.snapshot_;
        g.addTransform(toScreen());
        g.setColour(kBody);
        g.fillRect(R(0, 0, kPanelW, kPanelH));
        g.setColour(kNavy);
        g.fillRoundedRectangle(R(20, 58, 3880, 990), 30);
        g.setColour(kPanel);
        g.fillRoundedRectangle(R(68, 84, 1282, 975), 26);
        g.fillRoundedRectangle(SR(10, 58, 1832, 700), 26);
        // the insets
        g.setColour(kInset);
        for (auto r : {R(380, 92, 528, 332), R(538, 92, 1090, 478), R(538, 486, 1090, 745), R(1126, 272, 1265, 512),
                       SR(58, 552, 1037, 698), SR(1268, 552, 1610, 698), SR(1615, 60, 1820, 135), SR(1615, 255, 1820, 700)})
            g.fillRoundedRectangle(r, 22);
        for (int k = 0; k < kPanelKnobs; ++k)
            g.fillRoundedRectangle(SR(kKnobX[k] - 82, 255, kKnobX[k] + 82, 532), 40);
        g.setColour(kText);
        g.drawLine(sx(445), sy(74), sx(540), sy(74), 2.0f); // the Arpeggiator line
        g.drawLine(sx(662), sy(74), sx(758), sy(74), 2.0f);

        for (const auto& l : labels_) {
            g.setColour(l.blue ? kSubText : kText);
            g.setFont(juce::FontOptions(l.size, juce::Font::bold));
            g.drawText(l.text, juce::Rectangle<float>(400, l.size * 1.4f).withCentre(l.c), juce::Justification::centred, false);
        }

        // displays
        drawDisplay(g, R(576, 297, 1046, 461), R(597, 330, 1027, 430), s.displays[0], s.live);
        for (int i = 0; i < 4; ++i)
            drawDisplay(g, SR(28 + kLcdX[i], 95, 366 + kLcdX[i], 211), SR(43 + kLcdX[i], 116, 351 + kLcdX[i], 189),
                        s.displays[static_cast<std::size_t>(i + 1)], s.live);

        // knobs: dark collar with its LED segments, then the cap
        for (int k = 0; k < kPanelKnobs; ++k) {
            const auto c = SP(kKnobX[k], 338);
            const float ring = 76 * kSnd, cap = 35 * kSnd;
            g.setColour(kKnobRing);
            g.fillEllipse(juce::Rectangle<float>(ring * 2, ring * 2).withCentre(c));
            const auto& segs = s.rings[static_cast<std::size_t>(k)];
            const int n = segs.empty() ? 25 : static_cast<int>(segs.size());
            for (int i = 0; i < n; ++i) {
                const float a = juce::degreesToRadians(-150.0f + 300.0f * static_cast<float>(i) / static_cast<float>(n - 1));
                const float level = segs.empty() ? 0.0f : segs[static_cast<std::size_t>(i)];
                g.setColour(kSegment.interpolatedWith(kLedOn, std::clamp(level, 0.0f, 1.0f)));
                g.drawLine({c.getPointOnCircumference(cap * 1.32f, a), c.getPointOnCircumference(ring * 0.9f, a)}, 9.0f);
            }
            drawCap(g, c, cap, std::nullopt); // endless: no pointer
        }
        const auto master = juce::degreesToRadians(-150.0f + 300.0f * masterLevel);
        drawCap(g, {212, 183}, 50, master);
        drawCap(g, {452, 183}, 50, juce::degreesToRadians(20.0f)); // Mic Level: an analog preamp, not emulated
        drawCap(g, {272, 790}, 100, std::nullopt);                  // the rotary dial

        for (const auto& b : buttons_)
            drawButton(g, b.r, b.style, pressed_ == b.id);
        for (const auto& l : leds_)
            drawLed(g, l.c, s.leds[static_cast<std::size_t>(l.id)], l.c.x > kSndX ? 9 * kSnd : 12);
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const auto p = toPanel(e.position);
        juce::String tip;
        if (const auto* b = buttonAt(p))
            tip = panelLabel(b->id);
        else if (const int k = encoderAt(p); k >= 0)
            tip = k == kPanelDial ? "Rotary dial (drag or scroll)" : "Knob " + juce::String(k + 1) + " (drag or scroll)";
        else if (masterAt(p))
            tip = "Master Level";
        setTooltip(tip);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const auto p = toPanel(e.position);
        dragAccum_ = 0;
        lastDragY_ = e.position.y;
        if (const auto* b = buttonAt(p)) {
            pressed_ = b->id;
            if (auto* h = owner_.host_)
                h->panelButton(b->id, true);
            repaint();
        } else {
            dragEncoder_ = encoderAt(p);
            dragMaster_ = dragEncoder_ < 0 && masterAt(p);
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const float dy = lastDragY_ - e.position.y;
        lastDragY_ = e.position.y;
        if (dragEncoder_ >= 0) {
            dragAccum_ += dy;
            const int steps = static_cast<int>(dragAccum_ / 5.0f); // a step every 5 pixels
            if (steps != 0) {
                dragAccum_ -= static_cast<float>(steps) * 5.0f;
                turn(dragEncoder_, steps);
            }
        } else if (dragMaster_) {
            masterLevel = std::clamp(masterLevel + dy / 150.0f, 0.0f, 1.0f);
            if (auto* h = owner_.host_)
                h->panelAnalog(PanelAnalog::MasterLevel, masterLevel);
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (pressed_) {
            if (auto* h = owner_.host_)
                h->panelButton(*pressed_, false);
            pressed_.reset();
            repaint();
        }
        dragEncoder_ = -1;
        dragMaster_ = false;
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const auto p = toPanel(e.position);
        wheelAccum_ += w.deltaY * (w.isReversed ? -1.0f : 1.0f) * 12.0f;
        const int steps = static_cast<int>(wheelAccum_);
        if (steps == 0)
            return;
        wheelAccum_ -= static_cast<float>(steps);
        if (const int k = encoderAt(p); k >= 0)
            turn(k, steps);
        else if (masterAt(p)) {
            masterLevel = std::clamp(masterLevel + static_cast<float>(steps) / 50.0f, 0.0f, 1.0f);
            if (auto* h = owner_.host_)
                h->panelAnalog(PanelAnalog::MasterLevel, masterLevel);
            repaint();
        }
    }

    static float aspect() { return kPanelW / kPanelH; }

private:
    juce::AffineTransform toScreen() const
    {
        return juce::AffineTransform::scale(static_cast<float>(getWidth()) / kPanelW, static_cast<float>(getHeight()) / kPanelH);
    }
    juce::Point<float> toPanel(juce::Point<float> p) const { return p.transformedBy(toScreen().inverted()); }

    const ButtonSpec* buttonAt(juce::Point<float> p) const
    {
        for (const auto& b : buttons_)
            if (b.r.expanded(6).contains(p))
                return &b;
        return nullptr;
    }
    static int encoderAt(juce::Point<float> p)
    {
        for (int k = 0; k < kPanelKnobs; ++k)
            if (p.getDistanceFrom(SP(kKnobX[k], 338)) < 76 * kSnd)
                return k;
        if (p.getDistanceFrom({272, 790}) < 105)
            return kPanelDial;
        return -1;
    }
    static bool masterAt(juce::Point<float> p) { return p.getDistanceFrom({212, 183}) < 55; }

    void turn(int encoder, int steps)
    {
        if (auto* h = owner_.host_)
            h->panelEncoder(encoder, steps);
    }

    LiveView& owner_;
    const std::vector<ButtonSpec> buttons_;
    const std::vector<LedSpec> leds_;
    const std::vector<LabelSpec> labels_;
    std::optional<PanelButton> pressed_;
    int dragEncoder_ = -1;
    bool dragMaster_ = false;
    float dragAccum_ = 0, lastDragY_ = 0, wheelAccum_ = 0;
};

// ---- Controllers: the G2X's left cheek -------------------------------------------------------------------------
// As on the G2X: the "Global Modulation Wheel" box with wheels 1 and 2 lying across, the wooden pitch stick (sprung
// to the middle) and the mod wheel standing up; the wheels have green LEDs.

class LiveView::Controllers : public juce::Component, public juce::SettableTooltipClient {
public:
    explicit Controllers(LiveView& owner) : owner_(owner) {}

    void paint(juce::Graphics& g) override
    {
        const auto& s = owner_.snapshot_;
        g.setColour(kCheek);
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1), 8);
        // the global wheels' box
        const auto box = globalBox();
        g.setColour(kPanel.darker(0.08f));
        g.fillRoundedRectangle(box, 6);
        g.setColour(kText);
        g.setFont(juce::FontOptions(std::clamp(box.getHeight() * 0.13f, 8.0f, 13.0f), juce::Font::bold));
        g.drawText("Global Modulation Wheel", box.withHeight(box.getHeight() * 0.22f), juce::Justification::centred);
        for (int i = 1; i <= 2; ++i)
            drawWheel(g, wheelArea(i), wheels_[static_cast<std::size_t>(i)], false,
                      s.leds[static_cast<std::size_t>(i == 1 ? PanelLed::GlobalWheel1 : PanelLed::GlobalWheel2)]);
        // the pitch stick: a wooden block in a slot
        const auto stick = stickArea();
        g.setColour(kBezel);
        g.fillRoundedRectangle(stick.expanded(3), 3);
        const float w = stick.getWidth() * 0.45f;
        const auto block = juce::Rectangle<float>(w, stick.getHeight())
                               .withCentre({stick.getCentreX() + pitch_ * (stick.getWidth() - w) / 2, stick.getCentreY()});
        g.setGradientFill(juce::ColourGradient(kWood.brighter(0.35f), block.getTopLeft(), kWood.darker(0.2f), block.getBottomLeft(), false));
        g.fillRoundedRectangle(block, 3);
        g.setColour(juce::Colours::white.withAlpha(0.85f));
        g.setFont(juce::FontOptions(std::clamp(stick.getHeight() * 0.55f, 8.0f, 12.0f), juce::Font::bold));
        g.drawText("Pitch Stick", stick.translated(0, stick.getHeight() + 3).expanded(30, 0), juce::Justification::centredTop);
        // the mod wheel
        drawWheel(g, wheelArea(0), wheels_[0], true, s.leds[static_cast<std::size_t>(PanelLed::ModWheel)]);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragging_ = -2;
        if (stickArea().expanded(6).contains(e.position))
            dragging_ = -1;
        for (int i = 0; i < 3; ++i)
            if (wheelArea(i).expanded(4).contains(e.position))
                dragging_ = i;
        mouseDrag(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging_ == -1) {
            const auto s = stickArea();
            setPitch(std::clamp((e.position.x - s.getCentreX()) / (s.getWidth() / 2), -1.0f, 1.0f));
        } else if (dragging_ == 0) {
            const auto s = wheelArea(0);
            setWheel(0, std::clamp((s.getBottom() - e.position.y) / s.getHeight(), 0.0f, 1.0f));
        } else if (dragging_ > 0) {
            const auto s = wheelArea(dragging_); // lying across: right is up
            setWheel(dragging_, std::clamp((e.position.x - s.getX()) / s.getWidth(), 0.0f, 1.0f));
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragging_ == -1)
            setPitch(0.0f); // the stick springs back
        dragging_ = -2;
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        juce::String tip;
        if (stickArea().expanded(6).contains(e.position))
            tip = "Pitch Stick";
        else if (wheelArea(0).expanded(4).contains(e.position))
            tip = "Mod Wheel";
        else if (wheelArea(1).expanded(4).contains(e.position))
            tip = "Global Modulation Wheel 1 (Morph group 5)";
        else if (wheelArea(2).expanded(4).contains(e.position))
            tip = "Global Modulation Wheel 2 (Morph group 8)";
        setTooltip(tip);
    }

private:
    juce::Rectangle<float> globalBox() const
    {
        auto r = getLocalBounds().toFloat().reduced(8);
        return r.removeFromTop(r.getHeight() * 0.42f);
    }
    juce::Rectangle<float> lower() const
    {
        auto r = getLocalBounds().toFloat().reduced(8);
        r.removeFromTop(r.getHeight() * 0.42f + 8);
        return r;
    }
    juce::Rectangle<float> stickArea() const
    {
        auto l = lower();
        auto left = l.removeFromLeft(l.getWidth() * 0.62f);
        return left.withSizeKeepingCentre(left.getWidth() * 0.8f, std::clamp(left.getHeight() * 0.16f, 10.0f, 20.0f))
            .translated(0, -left.getHeight() * 0.12f);
    }
    // 0 the mod wheel (standing), 1-2 the global wheels (lying across)
    juce::Rectangle<float> wheelArea(int i) const
    {
        if (i == 0) {
            auto l = lower();
            auto right = l.removeFromRight(l.getWidth() * 0.38f);
            return right.withSizeKeepingCentre(std::clamp(right.getWidth() * 0.45f, 10.0f, 22.0f), right.getHeight() * 0.86f);
        }
        auto b = globalBox();
        b.removeFromTop(b.getHeight() * 0.24f);
        const float rowH = b.getHeight() / 2;
        auto row = b.withY(b.getY() + rowH * static_cast<float>(i - 1)).withHeight(rowH);
        return row.withSizeKeepingCentre(row.getWidth() * 0.8f, std::clamp(rowH * 0.42f, 8.0f, 18.0f));
    }

    static void drawWheel(juce::Graphics& g, juce::Rectangle<float> slot, float value, bool standing, float led)
    {
        g.setColour(kBezel);
        g.fillRoundedRectangle(slot.expanded(3), 4);
        // the stone wheel shows a ridge where it is turned to
        g.setGradientFill(juce::ColourGradient(kStone.brighter(0.2f), slot.getTopLeft(), kStone.darker(0.35f),
                                               standing ? slot.getTopRight() : slot.getBottomLeft(), false));
        g.fillRoundedRectangle(slot, std::min(slot.getWidth(), slot.getHeight()) * 0.5f);
        const auto mark = standing ? juce::Point<float>(slot.getCentreX(), slot.getBottom() - value * slot.getHeight())
                                   : juce::Point<float>(slot.getX() + value * slot.getWidth(), slot.getCentreY());
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        if (standing)
            g.drawLine(slot.getX() + 2, mark.y, slot.getRight() - 2, mark.y, 2.0f);
        else
            g.drawLine(mark.x, slot.getY() + 2, mark.x, slot.getBottom() - 2, 2.0f);
        // the green LED in the middle of the wheel
        const float d = std::min(slot.getWidth(), slot.getHeight()) * 0.42f;
        g.setColour(kLedOff.interpolatedWith(kGreen, std::clamp(led, 0.0f, 1.0f)));
        g.fillEllipse(juce::Rectangle<float>(d, d).withCentre(slot.getCentre()));
    }

    void setPitch(float v)
    {
        pitch_ = v;
        if (owner_.snapshot_.live && owner_.host_ != nullptr) {
            owner_.host_->panelAnalog(PanelAnalog::PitchStick, (v + 1) / 2); // the panel's own stick
        } else { // MIDI pitch bend into the G2's MIDI IN
            const int bend = std::clamp(static_cast<int>(8192 + v * 8191), 0, 16383);
            owner_.midi({0xE0, static_cast<std::uint8_t>(bend & 0x7f), static_cast<std::uint8_t>(bend >> 7)});
        }
        repaint();
    }

    void setWheel(int i, float v)
    {
        wheels_[static_cast<std::size_t>(i)] = v;
        if (owner_.snapshot_.live && owner_.host_ != nullptr)
            owner_.host_->panelAnalog(i == 0 ? PanelAnalog::ModWheel : i == 1 ? PanelAnalog::GlobalWheel1 : PanelAnalog::GlobalWheel2, v);
        else if (i == 0) // the mod wheel as MIDI CC 1
            owner_.midi({0xB0, 1, static_cast<std::uint8_t>(std::lround(v * 127))});
        repaint();
    }

    LiveView& owner_;
    float pitch_ = 0.0f;
    std::array<float, 3> wheels_{};
    int dragging_ = -2; // -1 the stick, 0 the mod wheel, 1-2 the global wheels
};

// ---- Keyboard: 61 keys, C1-C6 ----------------------------------------------------------------------------------

class LiveView::Keyboard : public juce::MidiKeyboardComponent {
public:
    explicit Keyboard(juce::MidiKeyboardState& state) : MidiKeyboardComponent(state, horizontalKeyboard)
    {
        setAvailableRange(36, 96);
        setScrollButtonsVisible(false);
        setMidiChannel(1);
        setVelocity(0.8f, true); // velocity from where the key is hit
        setKeyPressBaseOctave(octave_);
        setOctaveForMiddleC(4);
        setBlackNoteLengthProportion(0.62f);
        setWantsKeyboardFocus(true);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        // Z / X: the computer keyboard's octave (as in most soft synths)
        if (!key.getModifiers().isAnyModifierKeyDown() && (key.getTextCharacter() == 'z' || key.getTextCharacter() == 'x')) {
            octave_ = std::clamp(octave_ + (key.getTextCharacter() == 'x' ? 1 : -1), 3, 7);
            setKeyPressBaseOctave(octave_);
            return true;
        }
        return MidiKeyboardComponent::keyPressed(key);
    }

    void drawWhiteNote(int, juce::Graphics& g, juce::Rectangle<float> area, bool isDown, bool isOver, juce::Colour,
                       juce::Colour) override
    {
        auto key = area.reduced(1, 0);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xffeef0f3), key.getTopLeft(), isDown ? kLedOn.withAlpha(0.55f).withBrightness(0.95f) : juce::Colour(0xffdfe2e7),
                                               key.getBottomLeft(), false));
        g.fillRoundedRectangle(key.withTrimmedTop(-6), 4);
        if (isOver && !isDown) {
            g.setColour(juce::Colours::black.withAlpha(0.05f));
            g.fillRect(key);
        }
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.drawLine(area.getRight(), area.getY(), area.getRight(), area.getBottom(), 1.0f);
    }

    void drawBlackNote(int, juce::Graphics& g, juce::Rectangle<float> area, bool isDown, bool isOver, juce::Colour) override
    {
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRoundedRectangle(area.translated(1.5f, 2), 3);
        g.setGradientFill(juce::ColourGradient(isDown ? kLedOn.darker(0.6f) : juce::Colour(0xff2b2f35), area.getTopLeft(),
                                               juce::Colour(0xff121417), area.getBottomLeft(), false));
        g.fillRoundedRectangle(area, 3);
        if (!isDown) {
            g.setColour(juce::Colours::white.withAlpha(isOver ? 0.22f : 0.14f));
            g.fillRoundedRectangle(area.reduced(area.getWidth() * 0.18f, 0).withTrimmedBottom(area.getHeight() * 0.12f).withHeight(area.getHeight() * 0.8f), 2);
        }
    }

private:
    int octave_ = 5; // A on the computer keyboard plays C4 (middle C)
};

// ---- LiveView ----------------------------------------------------------------------------------------------------

LiveView::LiveView(EmulatorHost* host, std::function<void()> startEmulator) : host_(host)
{
    panel_ = std::make_unique<Panel>(*this);
    controllers_ = std::make_unique<Controllers>(*this);
    keyboard_ = std::make_unique<Keyboard>(keyState_);
    addAndMakeVisible(*panel_);
    addAndMakeVisible(*controllers_);
    addAndMakeVisible(*keyboard_);
    keyState_.addListener(this);
    start_.onClick = [startEmulator] {
        if (startEmulator)
            startEmulator();
    };
    hint_.setJustificationType(juce::Justification::centred);
    hint_.setColour(juce::Label::textColourId, juce::Colours::white);
    hint_.setFont(juce::FontOptions(15.0f));
    backdrop_.setInterceptsMouseClicks(false, false);
    addChildComponent(backdrop_); // under the message, over the panel
    addChildComponent(start_);
    addChildComponent(hint_);
    startTimerHz(30);
}

LiveView::~LiveView()
{
    keyState_.removeListener(this);
}

void LiveView::setHost(EmulatorHost* host)
{
    host_ = host;
    timerCallback();
}

void LiveView::focusKeyboard()
{
    if (isShowing())
        keyboard_->grabKeyboardFocus();
}

void LiveView::visibilityChanged()
{
    if (isVisible())
        startTimerHz(30);
    else
        stopTimer(); // no polling while the patch is edited
}

void LiveView::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1f22));
    // the instrument's red body around the cheek and the keyboard
    const auto body = controllers_->getBounds().getUnion(keyboard_->getBounds()).expanded(6).withTop(panel_->getBottom());
    g.setColour(kBody);
    g.fillRoundedRectangle(body.toFloat(), 6);
}

void LiveView::Backdrop::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xe0202024));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 10);
}

void LiveView::resized()
{
    auto r = getLocalBounds().reduced(8);
    // the panel keeps its proportions; the keyboard gets what is left (at most a fifth of the width)
    const int bottomH = std::clamp(r.getWidth() / 6, 120, std::max(120, r.getHeight() / 2));
    int panelH = std::min(static_cast<int>(static_cast<float>(r.getWidth()) / Panel::aspect()), r.getHeight() - bottomH);
    panelH = std::max(panelH, 100);
    const int panelW = static_cast<int>(static_cast<float>(panelH) * Panel::aspect());
    auto top = r.removeFromTop(panelH);
    panel_->setBounds(top.withSizeKeepingCentre(panelW, panelH));
    // the cheek and the keyboard right below the panel, in the red body
    const int h = std::min(r.getHeight(), bottomH);
    auto bottom = r.removeFromTop(h).withSizeKeepingCentre(panelW, h).reduced(6, 0).withTrimmedBottom(6);
    controllers_->setBounds(bottom.removeFromLeft(std::max(120, bottom.getHeight())));
    keyboard_->setBounds(bottom);
    keyboard_->setKeyWidth(static_cast<float>(bottom.getWidth()) / 36.0f); // 36 white keys
    const auto p = panel_->getBounds();
    start_.setBounds(p.getCentreX() - 110, p.getCentreY() - 4, 220, 32);
    hint_.setBounds(p.getCentreX() - 300, p.getCentreY() - 34, 600, 24);
    backdrop_.setBounds(hint_.getBounds().getUnion(start_.getBounds()).expanded(14, 10));
}

void LiveView::timerCallback()
{
    const auto s = host_ != nullptr ? host_->panel() : PanelSnapshot{};
    const bool changed = s.live != snapshot_.live || s.generation != snapshot_.generation;
    const bool running = host_ != nullptr && host_->emulatorRunning();
    snapshot_ = s;
    if (changed)
        panel_->repaint();
    // Offer to start the emulated G2 while it does not run.
    const bool available = host_ != nullptr && host_->emulatorAvailable();
    start_.setVisible(available && !running);
    hint_.setVisible(!running || !s.live);
    backdrop_.setVisible(hint_.isVisible());
    hint_.setText(!available ? "This build of G2fresh has no emulated G2."
                  : !running ? "The Live panel plays the Emulated G2, running Clavia's G2 OS."
                             : "The Emulated G2 is starting...",
                  juce::dontSendNotification);
}

void LiveView::midi(std::initializer_list<std::uint8_t> bytes)
{
    if (host_ != nullptr && host_->emulatorRunning()) {
        const std::vector<std::uint8_t> v(bytes);
        host_->emulatorMidi(v);
    }
}

void LiveView::handleNoteOn(juce::MidiKeyboardState*, int channel, int note, float velocity)
{
    // the emulated panel's own keyboard (the OS applies focus, octave shift, KB Hold and split), else MIDI
    if (host_ != nullptr && snapshot_.live && host_->panelKey(note, true, velocity))
        return;
    midi({static_cast<std::uint8_t>(0x90 | ((channel - 1) & 15)), static_cast<std::uint8_t>(note),
          static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(velocity * 127)), 1, 127))});
}

void LiveView::handleNoteOff(juce::MidiKeyboardState*, int channel, int note, float)
{
    if (host_ != nullptr && snapshot_.live && host_->panelKey(note, false, 0.0f))
        return;
    midi({static_cast<std::uint8_t>(0x80 | ((channel - 1) & 15)), static_cast<std::uint8_t>(note), 64});
}

} // namespace g2ui
