#include "g2/special.hpp"

#include "g2/edit.hpp"

#include <algorithm>
#include <stdexcept>

namespace g2::special {
namespace {

const Module& moduleOf(const Patch& patch, Location loc, u8 index, u8 type)
{
    if (loc == Location::Settings)
        throw std::invalid_argument("modules live in the VA or FX area");
    const Module* m = patch.area(loc).find(index);
    if (!m)
        throw std::invalid_argument("no module with this index");
    if (m->type != type)
        throw std::invalid_argument("wrong module type for this control");
    return *m;
}

Module& moduleOf(Patch& patch, Location loc, u8 index, u8 type)
{
    return const_cast<Module&>(moduleOf(static_cast<const Patch&>(patch), loc, index, type));
}

std::span<const u8> valuesOf(const Module& m, u8 variation)
{
    if (variation >= m.params.size())
        throw std::invalid_argument("no such variation");
    return m.params[variation];
}

// CPnlDrumPresetSelector::fValues (0x1de4a0), 30 x 15 bytes, and fNames
// (0x1de680), 30 x char[8]. Preset 0 is the module's default sound.
constexpr std::array<std::array<u8, kDrumPresetParams>, kDrumPresets> kDrumValues{{
    {42, 15, 46, 50, 120, 102, 57, 32, 39, 49, 1, 68, 61, 79, 115},
    {43, 26, 55, 53, 105, 94, 63, 18, 78, 36, 1, 76, 44, 25, 123},
    {31, 71, 45, 35, 127, 0, 90, 24, 71, 27, 0, 84, 42, 81, 112},
    {32, 61, 58, 42, 113, 110, 90, 0, 0, 27, 1, 37, 55, 90, 127},
    {36, 39, 50, 52, 104, 92, 111, 0, 40, 32, 0, 34, 68, 79, 69},
    {79, 55, 35, 43, 127, 42, 102, 46, 0, 37, 2, 2, 0, 127, 127},
    {68, 3, 48, 42, 84, 127, 55, 66, 63, 40, 2, 63, 37, 126, 122},
    {64, 23, 36, 44, 120, 84, 26, 0, 19, 44, 2, 24, 44, 127, 98},
    {68, 57, 47, 33, 105, 0, 91, 28, 42, 42, 0, 81, 44, 112, 127},
    {85, 107, 32, 23, 127, 94, 0, 0, 63, 45, 2, 39, 55, 102, 98},
    {80, 38, 56, 47, 102, 58, 98, 26, 27, 50, 0, 33, 69, 105, 97},
    {69, 38, 57, 52, 102, 58, 96, 26, 27, 51, 0, 33, 69, 105, 89},
    {56, 38, 58, 52, 102, 58, 93, 26, 27, 53, 0, 33, 69, 105, 89},
    {86, 2, 55, 44, 99, 0, 67, 0, 56, 56, 0, 86, 65, 81, 117},
    {69, 2, 56, 45, 99, 0, 67, 0, 56, 56, 0, 86, 65, 81, 117},
    {55, 2, 58, 49, 99, 0, 67, 0, 56, 57, 0, 86, 65, 81, 117},
    {70, 28, 45, 51, 113, 81, 102, 4, 12, 47, 0, 44, 46, 96, 97},
    {58, 28, 46, 53, 113, 81, 102, 4, 12, 47, 0, 44, 57, 93, 97},
    {48, 28, 52, 54, 108, 81, 102, 4, 12, 47, 0, 44, 66, 96, 103},
    {127, 93, 0, 0, 0, 102, 91, 0, 26, 46, 2, 127, 0, 0, 127},
    {127, 93, 0, 0, 0, 102, 91, 50, 16, 55, 2, 127, 0, 0, 127},
    {127, 127, 19, 0, 28, 58, 112, 60, 0, 50, 2, 0, 0, 81, 107},
    {127, 127, 0, 0, 0, 127, 102, 107, 0, 53, 2, 127, 0, 0, 127},
    {28, 111, 42, 64, 73, 28, 83, 57, 37, 71, 0, 83, 50, 109, 123},
    {77, 32, 31, 45, 117, 93, 32, 32, 40, 45, 2, 110, 126, 116, 30},
    {127, 99, 52, 48, 86, 71, 110, 120, 0, 45, 2, 47, 16, 49, 114},
    {96, 60, 36, 42, 127, 72, 32, 32, 40, 45, 2, 46, 24, 89, 46},
    {87, 60, 36, 42, 127, 72, 32, 32, 40, 45, 2, 46, 24, 89, 46},
    {110, 93, 58, 66, 92, 79, 92, 127, 0, 58, 0, 0, 0, 127, 89},
    {30, 45, 62, 46, 127, 127, 55, 127, 28, 41, 2, 81, 55, 80, 97},
}};

constexpr std::array<const char*, kDrumPresets> kDrumNames{
    "Kick 1",  "Kick 2",  "Kick 3",  "Kick 4",  "Kick 5",  "Snare 1", "Snare 2", "Snare 3",
    "Snare 4", "Snare 5", "Tom1 1",  "Tom1 2",  "Tom1 3",  "Tom2 1",  "Tom2 2",  "Tom2 3",
    "Tom3 1",  "Tom3 2",  "Tom3 3",  "Cymb 1",  "Cymb 2",  "Cymb 3",  "Cymb 4",  "Cymb 5",
    "Perc 1",  "Perc 2",  "Perc 3",  "Perc 4",  "Perc 5",  "Perc 6",
};

void checkPreset(int preset)
{
    if (preset < 0 || preset >= kDrumPresets)
        throw std::invalid_argument("no such drum preset");
}

} // namespace

// ---- Random numbers ------------------------------------------------------------

std::uint32_t EditorRandom::next()
{
    state_ = state_ * 0x0bb38435u + 0x3619636bu;
    if (state_ == 0)
        state_ = 0x3619636bu;
    return state_;
}

// ---- Vocoder -------------------------------------------------------------------

const char* vocoderOpLabel(VocoderOp op)
{
    static constexpr std::array<const char*, kVocoderOps> labels{"-2", "-1", "0", "+1", "+2", "Inv", "Rnd"};
    const auto i = static_cast<std::size_t>(op);
    return i < labels.size() ? labels[i] : "";
}

std::array<u8, 16> vocoderBands(VocoderOp op, EditorRandom* rng)
{
    if (op == VocoderOp::Random && !rng)
        throw std::invalid_argument("the Rnd button needs a random generator");
    std::array<u8, 16> out{};
    for (int i = 0; i < 16; ++i) {
        int v = 0;
        switch (op) {
        case VocoderOp::Minus2: v = i + 3 <= 16 ? i + 3 : 0; break;
        case VocoderOp::Minus1: v = i + 2 <= 16 ? i + 2 : 0; break;
        case VocoderOp::Zero: v = i + 1; break;
        case VocoderOp::Plus1: v = i; break;
        case VocoderOp::Plus2: v = std::max(i - 1, 0); break;
        case VocoderOp::Invert: v = 16 - i; break;
        case VocoderOp::Random:
            // double((x >> 8)) * 17.0 * 2^-24, truncated (cvttsd2si)
            v = static_cast<int>(static_cast<double>(rng->next() >> 8) * 17.0 * (1.0 / 16777216.0));
            break;
        default: throw std::invalid_argument("unknown vocoder button");
        }
        out[static_cast<std::size_t>(i)] = static_cast<u8>(v);
    }
    return out;
}

void vocoderPreset(Patch& patch, Location loc, u8 module, u8 variation, VocoderOp op, EditorRandom* rng)
{
    moduleOf(patch, loc, module, kVocoderType);
    const auto bands = vocoderBands(op, rng);
    for (u8 i = 0; i < 16; ++i)
        edit::setParam(patch, loc, module, i, variation, bands[i]);
}

// ---- Note sequencer view ---------------------------------------------------------

NoteSeqView noteSeqView(const Module& module)
{
    NoteSeqView v;
    if (!module.customData)
        return v;
    // CPanel::SetCustomData hands the stream to each custom control in turn;
    // each reads three bytes and keeps the third.
    const auto& b = *module.customData;
    int kind0 = 0;
    for (std::size_t i = 0; i + 2 <= b.size(); i += 2u + b[i + 1]) {
        if (b[i] != 0 || b[i + 1] < 1 || i + 2 >= b.size())
            continue;
        if (kind0 == 0)
            v.zoom = b[i + 2];
        else if (kind0 == 1)
            v.offset = b[i + 2];
        ++kind0;
    }
    return v;
}

NoteSeqView noteSeqView(const Patch& patch, Location loc, u8 module)
{
    return noteSeqView(moduleOf(patch, loc, module, kSeqNoteType));
}

std::vector<u8> noteSeqCustomData(NoteSeqView view)
{
    return {0, 1, view.zoom, 0, 1, view.offset};
}

void setNoteSeqView(Patch& patch, Location loc, u8 module, NoteSeqView view)
{
    Module& m = moduleOf(patch, loc, module, kSeqNoteType);
    std::vector<u8> out = noteSeqCustomData(view);
    if (m.customData) {
        // Keep any other record (e.g. a label) after the two view records.
        const auto& b = *m.customData;
        int kind0 = 0;
        for (std::size_t i = 0; i + 2 <= b.size();) {
            const std::size_t end = std::min(b.size(), i + 2u + b[i + 1]);
            if (b[i] == 0 && kind0 < 2)
                ++kind0;
            else
                out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(i),
                           b.begin() + static_cast<std::ptrdiff_t>(end));
            i = end;
        }
    }
    m.customData = std::move(out);
}

u8 nextNoteSeqZoom(u8 zoom)
{
    return static_cast<u8>(zoom + 1u) < 3 ? static_cast<u8>(zoom + 1u) : 0;
}

u8 stepNoteSeqOffset(u8 offset, bool right)
{
    if (right)
        return offset < 9 ? static_cast<u8>(offset + 1) : offset;
    return offset != 0 ? static_cast<u8>(offset - 1) : offset;
}

std::string noteSeqOffsetText(u8 offset)
{
    return offset < kNoteSeqOffsets ? "C" + std::to_string(offset) : std::string{};
}

int noteSeqZoomBitmapX(u8 zoom)
{
    return (2 - static_cast<int>(zoom)) * 11;
}

NoteSeqWindow noteSeqWindow(NoteSeqView view)
{
    NoteSeqWindow w;
    const int z = view.zoom <= 1 ? view.zoom : 2;
    const int o = std::min<int>(view.offset, 9);
    w.zoom = z;
    w.high = o * 12 + 12;
    w.low = o * 12 - (z == 0 ? 24 : z == 1 ? 12 : 0);
    w.range = z == 0 ? 37 : z == 1 ? 25 : 13;
    w.pixels = z == 0 ? 2 : z == 1 ? 3 : 6;
    w.topCorr = z == 0 ? 1 : z == 1 ? 0 : -2;
    return w;
}

int NoteSeqWindow::markerTop(int value) const
{
    return topCorr + pixels * (range - 1 - (value - low));
}

u8 NoteSeqWindow::valueAt(int y) const
{
    // Byte arithmetic of CPnlSeqSlider::OnClick (0xccf66).
    const int q = (y - topCorr) / pixels; // idiv: truncates towards zero
    int d = static_cast<std::int8_t>(static_cast<u8>(range - q - 1));
    if (d < 0)
        d = 0;
    if (d + low < 0)
        d = -low;
    return static_cast<u8>(d + low);
}

int NoteSeqWindow::bitmapX() const
{
    return zoom == 0 ? 22 : zoom == 1 ? 11 : 0;
}

// ---- Note sequencer Clr / Rnd ------------------------------------------------------

void noteSeqClear(Patch& patch, Location loc, u8 module, u8 variation)
{
    moduleOf(patch, loc, module, kSeqNoteType);
    for (u8 i = 0; i < 16; ++i)
        edit::setParam(patch, loc, module, i, variation, 64);
}

u8 noteSeqRandomValue(const NoteSeqWindow& window, std::uint32_t r31)
{
    const int lo = std::max(window.low, 0);
    const int n = window.range + std::min(window.low, 0);
    const auto v = lo + static_cast<int>((static_cast<std::uint64_t>(n) * (r31 & 0x7fffffffu)) >> 31);
    return static_cast<u8>(std::min(v, 127));
}

void noteSeqRandom(Patch& patch, Location loc, u8 module, u8 variation, EditorRandom& rng)
{
    const NoteSeqWindow w = noteSeqWindow(noteSeqView(patch, loc, module));
    for (u8 i = 0; i < 16; ++i)
        edit::setParam(patch, loc, module, i, variation, noteSeqRandomValue(w, rng.next() >> 1));
}

u8 noteSeqRandomOriginal(int low, int range, std::int32_t rand)
{
    int lo = static_cast<std::int8_t>(static_cast<u8>(low));
    u8 n = static_cast<u8>(range);
    if (lo < 0) {
        n = static_cast<u8>(n + lo);
        lo = 0;
    }
    const auto product = static_cast<std::int32_t>(static_cast<std::uint32_t>(n) * static_cast<std::uint32_t>(rand));
    return static_cast<u8>(lo + product / 0x7fffffff);
}

// ---- Drum presets ----------------------------------------------------------------

const char* drumPresetName(int preset)
{
    checkPreset(preset);
    return kDrumNames[static_cast<std::size_t>(preset)];
}

const std::array<u8, kDrumPresetParams>& drumPresetValues(int preset)
{
    checkPreset(preset);
    return kDrumValues[static_cast<std::size_t>(preset)];
}

std::optional<int> drumPresetIndex(std::span<const u8> values)
{
    if (values.size() < kDrumPresetParams)
        return std::nullopt;
    for (int p = 0; p < kDrumPresets; ++p)
        if (std::equal(kDrumValues[static_cast<std::size_t>(p)].begin(), kDrumValues[static_cast<std::size_t>(p)].end(),
                       values.begin()))
            return p;
    return std::nullopt;
}

std::optional<int> drumPresetIndex(const Patch& patch, Location loc, u8 module, u8 variation)
{
    return drumPresetIndex(valuesOf(moduleOf(patch, loc, module, kDrumSynthType), variation));
}

void applyDrumPreset(Patch& patch, Location loc, u8 module, u8 variation, int preset)
{
    moduleOf(patch, loc, module, kDrumSynthType);
    const auto& v = drumPresetValues(preset);
    for (u8 i = 0; i < kDrumPresetParams; ++i)
        edit::setParam(patch, loc, module, i, variation, v[i]);
}

void DrumPresetSelector::update(std::span<const u8> values)
{
    const auto p = drumPresetIndex(values);
    matched = p.has_value();
    if (p)
        index = *p;
}

void DrumPresetSelector::update(const Patch& patch, Location loc, u8 module, u8 variation)
{
    update(valuesOf(moduleOf(patch, loc, module, kDrumSynthType), variation));
}

std::string DrumPresetSelector::text() const
{
    return matched && index >= 0 && index < kDrumPresets ? kDrumNames[static_cast<std::size_t>(index)] : "none";
}

std::optional<int> DrumPresetSelector::step(bool up)
{
    if (up) {
        if (index >= kDrumPresets - 1)
            return std::nullopt;
        if (matched)
            ++index;
    } else {
        if (index <= 0)
            return std::nullopt;
        --index;
    }
    matched = true;
    return index;
}

std::optional<int> DrumPresetSelector::step(Patch& patch, Location loc, u8 module, u8 variation, bool up)
{
    moduleOf(patch, loc, module, kDrumSynthType);
    const auto p = step(up);
    if (p)
        applyDrumPreset(patch, loc, module, variation, *p);
    return p;
}

} // namespace g2::special
