#include "panel.hpp"

#include <algorithm>
#include <bit>

namespace g2emu {

// ---------------------------------------------------------------------------------------------------------------
// HD44780 (the subset an 8-bit, write-only host uses; timing is the host's business: the OS waits after each write)

void Hd44780::execute(bool rs, std::uint8_t v)
{
    ++writes;
    if(rs) data(v);
    else command(v);
}

void Hd44780::moveAc(bool up)
{
    if(cgMode)
    {
        ac = std::uint8_t((ac + (up ? 1 : -1)) & 0x3f);
        return;
    }
    // DDRAM: 0x00-0x27 and 0x40-0x67 in two-line mode (each wraps into the other), 0x00-0x4F in one-line mode
    int a = ac + (up ? 1 : -1);
    if(twoLines)
    {
        if(a == 0x28) a = 0x40;
        else if(a == 0x68) a = 0x00;
        else if(a == 0x3f) a = 0x27;
        else if(a == -1) a = 0x67;
    }
    else
        a = (a + 80) % 80;
    ac = std::uint8_t(a);
}

void Hd44780::command(std::uint8_t v)
{
    if(v & 0x80)  // set DDRAM address
    {
        ac = std::uint8_t(v & 0x7f);
        cgMode = false;
    }
    else if(v & 0x40)  // set CGRAM address
    {
        ac = std::uint8_t(v & 0x3f);
        cgMode = true;
    }
    else if(v & 0x20)  // function set: DL, N, F
    {
        eightBit = v & 0x10;
        twoLines = v & 0x08;
    }
    else if(v & 0x10)  // cursor or display shift: S/C, R/L
    {
        if(v & 0x08) shift = (shift + ((v & 0x04) ? 39 : 1)) % 40;  // the display moves right: the window left
        else moveAc(v & 0x04);
    }
    else if(v & 0x08)  // display control: D, C, B
    {
        displayOn = v & 0x04;
        cursorOn = v & 0x02;
        blinkOn = v & 0x01;
    }
    else if(v & 0x04)  // entry mode: I/D, S
    {
        increment = v & 0x02;
        shiftOnWrite = v & 0x01;
    }
    else if(v & 0x02)  // return home
    {
        ac = 0;
        cgMode = false;
        shift = 0;
    }
    else if(v & 0x01)  // clear display
    {
        ddram.fill(0x20);
        ac = 0;
        cgMode = false;
        shift = 0;
        increment = true;
    }
}

void Hd44780::data(std::uint8_t v)
{
    if(cgMode) cgram[ac & 0x3f] = std::uint8_t(v & 0x1f);
    else ddram[std::size_t(ddIndex(ac))] = v;
    moveAc(increment);
    if(shiftOnWrite && !cgMode) shift = (shift + (increment ? 1 : 39)) % 40;
}

void Hd44780::snapshot(PanelDisplay& d) const
{
    d.on = displayOn;
    d.cgram = cgram;
    d.cursor = -1;
    for(int row = 0; row < PanelDisplay::Rows; ++row)
        for(int col = 0; col < PanelDisplay::Columns; ++col)
        {
            // a single-line display shows 80 cells in one row; the G2's are all set to two lines [C]
            const int idx = row * 40 + (shift + col) % 40;
            d.chars[std::size_t(row * PanelDisplay::Columns + col)] = ddram[std::size_t(idx)];
            if(!cgMode && ddIndex(ac) == idx) d.cursor = row * PanelDisplay::Columns + col;
        }
    d.cursorLine = cursorOn && d.cursor >= 0;
    d.cursorBlink = blinkOn && d.cursor >= 0;
}

// ---------------------------------------------------------------------------------------------------------------

Panel::Panel()
{
    latch_.fill(0xff);
    buttons_.fill(0xff);
    quad_.fill(3);  // both contacts open (pulled high): the dial's detent position [C 0x300580fa resyncs there]
}

std::uint8_t Panel::encoderBits(int raw) const
{
    // The OS reads each encoder byte into a word whose pairs (bits 15-14 first) it decodes as quadrature, the
    // byte's pairs being encoders 4-7 of the word [C 0x30057a08]; the state `quad_` is the pair's value.
    return quad_[std::size_t(raw)] & 3;
}

std::uint8_t Panel::readCs5(std::uint32_t off)
{
    if((off & 7) != 0) return 0xff;  // the OS reads +0 only
    ++trace_.cs5Reads;
    const auto sel = std::uint8_t(~latch_[7]);
    std::uint8_t v = 0xff;
    if(sel && (latch_[0] & 0x80))
    {
        for(int s = 0; s < 8; ++s)
        {
            if(!(sel & (1u << s))) continue;
            ++selectReads_[std::size_t(s)];
            ++trace_.selectReads[s];
            if(s < 6) v &= buttons_[std::size_t(s)];
            else
            {
                // encoders (s - 6) * 4 + 0..3 in bits 7-6, 5-4, 3-2, 1-0
                std::uint8_t b = 0;
                for(int e = 0; e < 4; ++e) b = std::uint8_t(b << 2 | encoderBits((s - 6) * 4 + e));
                v &= b;
            }
        }
    }
    else
    {
        ++selectReads_[8];
        ++trace_.selectReads[8];
        // the dial: the OS swaps the two bits [C 0x3005bd74, table 0x300EC758]: bit 6 is the pair's high bit
        const auto q = quad_[8];
        v = std::uint8_t(0x0f | (modelBits_ << 4) | ((q & 2) ? 0x40 : 0) | ((q & 1) ? 0x80 : 0));
    }
    applyPending();
    return v;
}

void Panel::writeCs5(std::uint32_t off, std::uint8_t v)
{
    off &= 7;
    ++trace_.cs5Writes[off];
    const auto old = latch_[off];
    latch_[off] = v;
    switch(off)
    {
    case 3: strobeLeds(v); break;
    case 5: controlLcds(5, std::uint8_t(old & ~v));  break;  // falling edges
    case 6: controlLcds(6, std::uint8_t(old & ~v)); break;
    default: break;
    }
}

void Panel::controlLcds(int latch, std::uint8_t falling)
{
    // E falls: the controller takes the data bus with RS as it is
    const auto c = latch_[std::size_t(latch)];
    if(latch == 6)
    {
        if(falling & 0x01) { lcd_[0].execute(c & 0x02, latch_[4]); ++changes_; }
        return;
    }
    for(int i = 0; i < 4; ++i)
    {
        const int e = 0x40 >> (2 * i), rs = 0x80 >> (2 * i);
        if(falling & e) { lcd_[std::size_t(1 + i)].execute(c & rs, latch_[4]); ++changes_; }
    }
}

void Panel::strobeLeds(std::uint8_t columnsActiveLow)
{
    // Column c (strobe bit 7 - c) shows bytes 0x1E - 4c (rows 0-7, latch +2), 0x1D - 4c (rows 8-14, latch +0) and
    // 0x1C - 4c (rows 16-23, latch +1) of the OS's 32-byte LED buffer, active low [C 0x3005bf3e]. The OS's LED number
    // n is bit n & 7 of byte n >> 3.
    const auto active = std::uint8_t(~columnsActiveLow);
    if(!active) return;
    for(int bit = 0; bit < 8; ++bit)
    {
        if(!(active & (1u << bit))) continue;
        const int c = 7 - bit;
        const int bytes[3] = {0x1e - 4 * c, 0x1d - 4 * c, 0x1c - 4 * c};
        const std::uint8_t data[3] = {latch_[2], std::uint8_t(latch_[0] | 0x80), latch_[1]};
        for(int k = 0; k < 3; ++k)
            for(int b = 0; b < 8; ++b)
            {
                if(k == 1 && b == 7) continue;  // the multiplexer's enable, not an LED
                const auto n = std::size_t(bytes[k] * 8 + b);
                const std::uint32_t lit = (data[k] >> b) & 1 ? 0 : 1;
                const auto h = ledHist_[n] << 1 | lit;
                if((h ^ ledHist_[n]) & 1) ++changes_;
                ledHist_[n] = h;
                ++ledStrobes_[n];
            }
    }
}

void Panel::writeCs4(std::uint16_t columns)
{
    ++trace_.cs4Writes;
    columns_ = columns;
}

std::uint16_t Panel::gpioInputs() const
{
    // key k: columns 2(k >> 3) + 1 (contact A) and 2(k >> 3) (contact B), row bit 7 - (k & 7) [C 0x30029d78]
    std::uint8_t rows = 0;
    const auto driven = std::uint16_t(~columns_);
    if(driven)
        for(int k = 0; k < 64; ++k)
        {
            const auto c = contacts_[std::size_t(k)];
            if(!c) continue;
            const int p = k >> 3;
            if(((c & 1) && (driven & (1u << (2 * p + 1)))) || ((c & 2) && (driven & (1u << (2 * p)))))
                rows |= std::uint8_t(0x80 >> (k & 7));
        }
    // sustain pedal: bit 12 high while no pedal is plugged in, bit 13 the pedal's contact, low when closed
    // [C 0x30027926: with Sust Ped Pol 0, the pedal is down while bit 13 is low]
    std::uint16_t v = std::uint16_t(~rows & 0xff);
    if(!sustainPlugged_) v |= 0x1000;
    if(!sustainDown_) v |= 0x2000;
    return v;
}

void Panel::button(int raw, bool down)
{
    if(raw < 0 || raw >= 48) return;
    buttonQueue_.push_back({raw, down});
    applyPending();
}

void Panel::turn(int raw, int transitions)
{
    if(raw < 0 || raw > 8) return;
    pendingTurn_[std::size_t(raw)] += transitions;
    applyPending();
}

void Panel::key(int raw, bool down, std::uint64_t delayBus, std::uint64_t now)
{
    if(raw < 0 || raw >= 64) return;
    std::uint64_t at = keyQueue_.empty() ? now : std::max(now, keyQueue_.back().at);
    if(down)
    {
        keyQueue_.push_back({at, raw, 1});
        keyQueue_.push_back({at + delayBus, raw, 3});
    }
    else
    {
        keyQueue_.push_back({at, raw, 1});
        keyQueue_.push_back({at + delayBus, raw, 0});
    }
}

bool Panel::advance(std::uint64_t now)
{
    now_ = now;
    bool changed = false;
    while(!keyQueue_.empty() && keyQueue_.front().at <= now)
    {
        contacts_[std::size_t(keyQueue_.front().raw)] = keyQueue_.front().contacts;
        keyQueue_.pop_front();
        changed = true;
    }
    return changed;
}

void Panel::applyPending()
{
    // buttons, in order: a change waits until the OS has scanned the byte since its last change
    while(!buttonQueue_.empty())
    {
        const auto e = buttonQueue_.front();
        const auto s = std::size_t(e.raw >> 3);
        if(selectReads_[s] == changedAtRead_[s]) break;
        const auto bit = std::uint8_t(1u << (e.raw & 7));
        const auto old = buttons_[s];
        buttons_[s] = e.down ? std::uint8_t(old & ~bit) : std::uint8_t(old | bit);
        if(buttons_[s] != old) changedAtRead_[s] = selectReads_[s];
        buttonQueue_.pop_front();
    }
    // encoders: one transition per scan
    for(int e = 0; e < 9; ++e)
    {
        auto& p = pendingTurn_[std::size_t(e)];
        if(!p) continue;
        const auto s = std::size_t(e == 8 ? 8 : 6 + e / 4);
        if(selectReads_[s] == changedAtRead_[s]) continue;
        // the dial: a detent (4 transitions, from rest to rest) at most every DialDetentGap bus clocks, slower than
        // the OS's acceleration window (30 scans) so that one step is one step
        if(e == 8 && quad_[8] == 3 && now_ < dialRestUntil_) continue;
        // clockwise: knobs 0 -> 2 -> 3 -> 1 -> 0 [C table 0x300EC540], the dial 0 -> 1 -> 3 -> 2 -> 0 [0x300EC560]
        static constexpr std::uint8_t knobCw[4] = {2, 0, 3, 1}, knobCcw[4] = {1, 3, 0, 2};
        static constexpr std::uint8_t dialCw[4] = {1, 3, 0, 2}, dialCcw[4] = {2, 0, 3, 1};
        auto& q = quad_[std::size_t(e)];
        if(e == 8) q = p > 0 ? dialCw[q] : dialCcw[q];
        else q = p > 0 ? knobCw[q] : knobCcw[q];
        p += p > 0 ? -1 : 1;
        changedAtRead_[s] = selectReads_[s];
        if(e == 8 && q == 3) dialRestUntil_ = now_ + DialDetentGap;
    }
}

void Panel::snapshot(PanelState& s) const
{
    for(std::size_t i = 0; i < lcd_.size(); ++i) lcd_[i].snapshot(s.displays[i]);
    for(std::size_t n = 0; n < 256; ++n) s.rawLeds[n] = ledHist_[n] & 1;
    for(int l = 0; l < PanelLedCount; ++l) s.leds[std::size_t(l)] = s.rawLeds[panelLedNumber(PanelLed(l))];
    for(std::size_t k = 0; k < 8; ++k)
        for(std::size_t i = 0; i < PanelRingLeds; ++i) s.rings[k][i] = s.rawLeds[32 * k + i];
    s.model = modelBits_ == 3 ? PanelModel::G2X : modelBits_ == 2 ? PanelModel::G2Engine : PanelModel::G2;
}

// ---------------------------------------------------------------------------------------------------------------
// Names and the LED map. LED numbers are the OS's [C: its tables at 0x300ECD26 (knob LEDs), 0x300ECD2E (button
// LEDs), 0x300ECE84/88 (slots), 0x300ECE8C (octave), 0x300ECE9E (mic level), 0x300ECEB5 (variations); emulated:
// each LED lit by its function]; see re/notes §3.10.

std::uint8_t panelLedNumber(PanelLed led)
{
    static constexpr std::uint8_t n[PanelLedCount] = {
        16,                     // Midi
        244, 212, 180,          // Mic -20, -12, 0 dB
        144, 176, 208, 148,     // System, Patch, Store, Load Patch
        177, 209, 241, 18,      // Slot A-D
        49, 81, 113, 145,       // Slot A-D keyboard
        50, 82, 114, 146, 178,  // Octave -2..+2
        210, 112,               // KB Hold, KB Split
        116, 80, 247, 48,       // split points 1-4
        17, 240,                // Perf.Mode, Sub Func.
        23, 55,                 // Patch Settings, Global Panel
        242, 19, 51, 83, 115, 147, 179, 211,  // Variation 1-8
        243,                    // Morph
        87, 119, 151, 183, 215, // Page A-E
        20, 52, 84,             // Page 1-3
        21, 53, 85, 117, 149, 181, 213, 245,  // Knob 1-8 LED
        22, 54, 86, 118, 150, 182, 214, 246,  // Button 1-8 LED
    };
    return n[std::size_t(led) % PanelLedCount];
}

const char* panelName(PanelButton b)
{
    static constexpr const char* names[48] = {
        "Navigator Up", "Navigator Down", "Navigator Right", "Navigator Left", "Load Patch", "KB Hold", "Focus/Copy", "Shift",
        "System", "Patch", "Store", "Display Mode", "Performance", "KB Split", "Patch Settings", "Morph",
        "Page A", "Page B", "Page C", "Page D", "Page E", "Page 1", "Page 2", "Page 3",
        "Variation 1", "Variation 2", "Variation 3", "Variation 4", "Variation 5", "Variation 6", "Variation 7", "Variation 8",
        "Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6", "Button 7", "Button 8",
        "Slot A", "Slot B", "Slot C", "Slot D", "Octave Shift Down", "Octave Shift Up", "", ""};
    return names[std::size_t(b) & 47];
}

const char* panelName(PanelEncoder e)
{
    static constexpr const char* names[PanelEncoderCount] = {"Knob 1", "Knob 2", "Knob 3", "Knob 4", "Knob 5",
                                                             "Knob 6", "Knob 7", "Knob 8", "Rotary Dial"};
    return std::size_t(e) < PanelEncoderCount ? names[std::size_t(e)] : "";
}

const char* panelName(PanelAnalog a)
{
    static constexpr const char* names[PanelAnalogCount] = {"Master Level", "Pitch Stick", "Modwheel", "Ctrl.Pedal",
                                                            "Aftertouch", "Global Wheel 1", "Global Wheel 2"};
    return std::size_t(a) < PanelAnalogCount ? names[std::size_t(a)] : "";
}

const char* panelName(PanelLed l)
{
    static constexpr const char* names[PanelLedCount] = {
        "MIDI", "Mic -20", "Mic -12", "Mic 0 dB", "System", "Patch", "Store", "Load Patch",
        "Slot A", "Slot B", "Slot C", "Slot D", "Slot A Keyboard", "Slot B Keyboard", "Slot C Keyboard", "Slot D Keyboard",
        "Octave -2", "Octave -1", "Octave 0", "Octave +1", "Octave +2", "KB Hold", "KB Split",
        "Split Point 1", "Split Point 2", "Split Point 3", "Split Point 4", "Perf.Mode", "Sub Func.",
        "Patch Settings", "Global Panel",
        "Variation 1", "Variation 2", "Variation 3", "Variation 4", "Variation 5", "Variation 6", "Variation 7", "Variation 8",
        "Morph", "Page A", "Page B", "Page C", "Page D", "Page E", "Page 1", "Page 2", "Page 3",
        "Knob 1 LED", "Knob 2 LED", "Knob 3 LED", "Knob 4 LED", "Knob 5 LED", "Knob 6 LED", "Knob 7 LED", "Knob 8 LED",
        "Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6", "Button 7", "Button 8"};
    return std::size_t(l) < PanelLedCount ? names[std::size_t(l)] : "";
}

// ---------------------------------------------------------------------------------------------------------------

char32_t hd44780Unicode(std::uint8_t c)
{
    if(c < 0x10) return 0;
    if(c < 0x20) return 0;
    if(c == 0x5c) return char32_t(0x00A5);
    if(c == 0x7e) return char32_t(0x2192);
    if(c == 0x7f) return char32_t(0x2190);
    if(c < 0x7e) return c;
    if(c < 0xa0) return 0;
    if(c == 0xa0) return U' ';
    if(c < 0xe0) return char32_t(0xFF61 + (c - 0xa1));  // JIS X 0201 katakana, as half-width forms
    // 0xE0-0xFF: Greek letters and symbols of the A00 set (written as code points: MSVC reads the source in the
    // local code page)
    static constexpr char32_t hi[32] = {
        char32_t(0x03B1), char32_t(0x00E4), char32_t(0x03B2), char32_t(0x03B5), char32_t(0x03BC), char32_t(0x03C3), char32_t(0x03C1), U'g',
        char32_t(0x221A), char32_t(0x207B), U'j', char32_t(0x02E3), char32_t(0x00A2), char32_t(0x00A3), char32_t(0x00F1), char32_t(0x00F6),
        U'p', U'q', char32_t(0x03B8), char32_t(0x221E), char32_t(0x03A9), char32_t(0x00FC), char32_t(0x03A3), char32_t(0x03C0),
        U'x', U'y', char32_t(0x5343), char32_t(0x4E07), char32_t(0x5186), char32_t(0x00F7), U' ', char32_t(0x2588),
    };
    return hi[c - 0xe0];
}

namespace {
void appendUtf8(std::string& s, char32_t c)
{
    if(c < 0x80) s += char(c);
    else if(c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
    else if(c < 0x10000) { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
    else { s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 0x3F)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
}
} // namespace

namespace {
// What a user character stands for: the OS draws the descender letters g, j, p, q, y itself (the A00 set squeezes
// them into 7 rows). Told apart by their shape, not by a copy of the OS's dots.
char32_t userLetter(const std::uint8_t* rows)
{
    if(!(rows[6] | rows[7])) return U'\u2592';  // no descender: a symbol
    if(rows[0]) return U'j';                    // the only one with a dot on top
    if((rows[6] | rows[7]) & 0x10) return U'p';  // the stem goes down on the left
    if((rows[2] & 0x11) == 0x11 && (rows[3] & 0x11) == 0x11) return U'y';  // open at the top
    if(rows[7] == 0x01) return U'q';            // a straight stem on the right
    if(rows[7] & 0x0e) return U'g';             // a hook under it
    return U'\u2592';
}
} // namespace

bool PanelDisplay::userGlyph(std::uint8_t code, std::array<std::uint8_t, 8>& rows) const
{
    if(code >= 16) return false;
    for(int r = 0; r < 8; ++r) rows[std::size_t(r)] = cgram[std::size_t((code & 7) * 8 + r)];
    return true;
}

std::string PanelDisplay::text(int row) const
{
    std::string s;
    for(int c = 0; c < Columns; ++c)
    {
        const auto code = chars[std::size_t(row * Columns + c)];
        char32_t u = hd44780Unicode(code);
        if(code < 16) u = userLetter(&cgram[std::size_t((code & 7) * 8)]);
        appendUtf8(s, u ? u : U' ');
    }
    return s;
}

} // namespace g2emu
