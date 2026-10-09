#include "g2/file.hpp"

#include "g2/crc.hpp"

#include <algorithm>
#include <stdexcept>

namespace g2::file {
namespace {

// ---- $21 patch header -------------------------------------------------------

constexpr std::array<unsigned, 12> kHeaderLegacyBits{7, 7, 7, 7, 5, 5, 2, 7, 2, 7, 2, 1};

PatchHeader decodePatchHeader(BitReader& r)
{
    PatchHeader h;
    for (std::size_t i = 0; i < h.legacy.size(); ++i)
        h.legacy[i] = static_cast<u8>(r.read(kHeaderLegacyBits[i]));
    h.voiceCount = static_cast<u8>(r.read(7) + 1);
    const auto hi = r.read(7);
    h.splitterPos = static_cast<std::uint16_t>((hi << 7) | r.read(7));
    h.octaveShift = static_cast<std::int8_t>(static_cast<int>(r.read(3)) - 2);
    for (bool& v : h.cablesVisible)
        v = r.read(1) != 0;
    h.monoMode = static_cast<u8>(r.read(2));
    h.activeVariation = static_cast<u8>(r.read(8));
    h.category = static_cast<u8>(r.read(8));
    h.pad = static_cast<u8>(r.align());
    h.reservedByte = r.u8();
    return h;
}

void encode(BitWriter& w, const PatchHeader& h)
{
    for (std::size_t i = 0; i < h.legacy.size(); ++i)
        w.write(h.legacy[i], kHeaderLegacyBits[i]);
    w.write(h.voiceCount - 1u, 7);
    w.write(h.splitterPos >> 7, 7);
    w.write(h.splitterPos & 0x7Fu, 7);
    w.write(static_cast<std::uint32_t>(h.octaveShift + 2), 3);
    for (bool v : h.cablesVisible)
        w.write(v ? 1 : 0, 1);
    w.write(h.monoMode, 2);
    w.write(h.activeVariation, 8);
    w.write(h.category, 8);
    w.align(h.pad);
    w.u8(h.reservedByte);
}

// ---- $4A module list --------------------------------------------------------

ModuleList decodeModuleList(BitReader& r)
{
    ModuleList l;
    l.location = static_cast<u8>(r.read(2));
    const auto count = r.read(8);
    for (std::uint32_t i = 0; i < count; ++i) {
        ModuleEntry m;
        m.type = static_cast<u8>(r.read(8));
        m.index = static_cast<u8>(r.read(8));
        m.col = static_cast<u8>(r.read(7));
        m.row = static_cast<u8>(r.read(7));
        m.color = static_cast<u8>(r.read(8));
        m.uprate = static_cast<u8>(r.read(1));
        m.locked = static_cast<u8>(r.read(1));
        m.reserved = static_cast<u8>(r.read(6));
        const auto modes = r.read(4);
        for (std::uint32_t k = 0; k < modes; ++k)
            m.modes.push_back(static_cast<u8>(r.read(6)));
        l.modules.push_back(std::move(m));
    }
    return l;
}

void encode(BitWriter& w, const ModuleList& l)
{
    w.write(l.location, 2);
    w.write(static_cast<std::uint32_t>(l.modules.size()), 8);
    for (const auto& m : l.modules) {
        w.write(m.type, 8);
        w.write(m.index, 8);
        w.write(m.col, 7);
        w.write(m.row, 7);
        w.write(m.color, 8);
        w.write(m.uprate, 1);
        w.write(m.locked, 1);
        w.write(m.reserved, 6);
        w.write(static_cast<std::uint32_t>(m.modes.size()), 4);
        for (u8 v : m.modes)
            w.write(v, 6);
    }
}

// ---- $69 current notes ------------------------------------------------------

Voice decodeVoice(BitReader& r)
{
    Voice v;
    v.note = static_cast<u8>(r.read(7));
    v.attackVel = static_cast<u8>(r.read(7));
    v.releaseVel = static_cast<u8>(r.read(7));
    return v;
}

void encode(BitWriter& w, const Voice& v)
{
    w.write(v.note, 7);
    w.write(v.attackVel, 7);
    w.write(v.releaseVel, 7);
}

CurrentNotes decodeCurrentNotes(BitReader& r)
{
    CurrentNotes n;
    n.last = decodeVoice(r);
    n.notes.clear();
    const auto count = r.read(5) + 1;
    for (std::uint32_t i = 0; i < count; ++i)
        n.notes.push_back(decodeVoice(r));
    return n;
}

void encode(BitWriter& w, const CurrentNotes& n)
{
    if (n.notes.empty() || n.notes.size() > 32)
        throw std::invalid_argument("current notes: need 1..32 notes");
    encode(w, n.last);
    w.write(static_cast<std::uint32_t>(n.notes.size() - 1), 5);
    for (const auto& v : n.notes)
        encode(w, v);
}

// ---- $52 cable list ---------------------------------------------------------

CableList decodeCableList(BitReader& r)
{
    CableList l;
    l.location = static_cast<u8>(r.read(2));
    l.pad = static_cast<u8>(r.align());
    const auto count = r.u16();
    for (std::uint32_t i = 0; i < count; ++i) {
        Cable c;
        c.color = static_cast<u8>(r.read(3));
        c.fromModule = static_cast<u8>(r.read(8));
        c.fromConn = static_cast<u8>(r.read(6));
        c.fromIsOutput = static_cast<u8>(r.read(1));
        c.toModule = static_cast<u8>(r.read(8));
        c.toConn = static_cast<u8>(r.read(6));
        l.cables.push_back(c);
    }
    return l;
}

void encode(BitWriter& w, const CableList& l)
{
    w.write(l.location, 2);
    w.align(l.pad);
    w.u16(static_cast<std::uint16_t>(l.cables.size()));
    for (const auto& c : l.cables) {
        w.write(c.color, 3);
        w.write(c.fromModule, 8);
        w.write(c.fromConn, 6);
        w.write(c.fromIsOutput, 1);
        w.write(c.toModule, 8);
        w.write(c.toConn, 6);
    }
}

// ---- $4D parameter values ---------------------------------------------------

ParamList decodeParamList(BitReader& r)
{
    ParamList l;
    l.location = static_cast<u8>(r.read(2));
    const auto modules = r.read(8);
    l.variationCount = static_cast<u8>(r.read(8));
    for (std::uint32_t i = 0; i < modules; ++i) {
        ParamModule m;
        m.index = static_cast<u8>(r.read(8));
        m.paramCount = static_cast<u8>(r.read(7));
        for (unsigned v = 0; v < l.variationCount; ++v) {
            ParamVariation pv;
            pv.variation = static_cast<u8>(r.read(8));
            for (unsigned p = 0; p < m.paramCount; ++p)
                pv.values.push_back(static_cast<u8>(r.read(7)));
            m.variations.push_back(std::move(pv));
        }
        l.modules.push_back(std::move(m));
    }
    return l;
}

void encode(BitWriter& w, const ParamList& l)
{
    w.write(l.location, 2);
    w.write(static_cast<std::uint32_t>(l.modules.size()), 8);
    w.write(l.variationCount, 8);
    for (const auto& m : l.modules) {
        if (m.variations.size() != l.variationCount)
            throw std::invalid_argument("param list: variation count mismatch");
        w.write(m.index, 8);
        w.write(m.paramCount, 7);
        for (const auto& pv : m.variations) {
            if (pv.values.size() != m.paramCount)
                throw std::invalid_argument("param list: parameter count mismatch");
            w.write(pv.variation, 8);
            for (u8 v : pv.values)
                w.write(v, 7);
        }
    }
}

// ---- $65 morph map ----------------------------------------------------------

MorphMap decodeMorphMap(BitReader& r)
{
    MorphMap m;
    const auto variations = r.read(8);
    m.morphCount = static_cast<u8>(r.read(4));
    for (unsigned i = 0; i < m.morphCount; ++i)
        m.keyboardAssign.push_back(static_cast<u8>(r.read(2)));
    for (std::uint32_t v = 0; v < variations; ++v) {
        MorphVariation mv;
        mv.variation = static_cast<u8>(r.read(8));
        for (unsigned i = 0; i < m.morphCount; ++i)
            mv.legacyDials.push_back(static_cast<u8>(r.read(7)));
        const auto count = r.read(8);
        for (std::uint32_t i = 0; i < count; ++i) {
            MorphAssign a;
            a.location = static_cast<u8>(r.read(2));
            a.module = static_cast<u8>(r.read(8));
            a.param = static_cast<u8>(r.read(7));
            a.morph = static_cast<u8>(r.read(4));
            a.range = static_cast<std::int8_t>(r.readSigned(8));
            mv.morphs.push_back(a);
        }
        m.variations.push_back(std::move(mv));
    }
    return m;
}

void encode(BitWriter& w, const MorphMap& m)
{
    if (m.keyboardAssign.size() != m.morphCount)
        throw std::invalid_argument("morph map: keyboard assign count mismatch");
    w.write(static_cast<std::uint32_t>(m.variations.size()), 8);
    w.write(m.morphCount, 4);
    for (u8 k : m.keyboardAssign)
        w.write(k, 2);
    for (const auto& mv : m.variations) {
        if (mv.legacyDials.size() != m.morphCount)
            throw std::invalid_argument("morph map: legacy dial count mismatch");
        w.write(mv.variation, 8);
        for (u8 d : mv.legacyDials)
            w.write(d, 7);
        w.write(static_cast<std::uint32_t>(mv.morphs.size()), 8);
        for (const auto& a : mv.morphs) {
            w.write(a.location, 2);
            w.write(a.module, 8);
            w.write(a.param, 7);
            w.write(a.morph, 4);
            w.writeSigned(a.range, 8);
        }
    }
}

// ---- $62 / $5F knob maps ----------------------------------------------------

KnobMap decodeKnobMap(BitReader& r, bool global)
{
    KnobMap m;
    const auto count = r.u16();
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!r.read(1)) {
            m.knobs.emplace_back();
            continue;
        }
        KnobAssign k;
        k.location = static_cast<u8>(r.read(2));
        k.module = static_cast<u8>(r.read(8));
        k.assignType = static_cast<u8>(r.read(2));
        k.param = static_cast<u8>(r.read(7));
        if (global)
            k.slot = static_cast<u8>(r.read(2));
        m.knobs.emplace_back(k);
    }
    return m;
}

void encode(BitWriter& w, const KnobMap& m, bool global)
{
    w.u16(static_cast<std::uint16_t>(m.knobs.size()));
    for (const auto& k : m.knobs) {
        w.write(k ? 1 : 0, 1);
        if (!k)
            continue;
        w.write(k->location, 2);
        w.write(k->module, 8);
        w.write(k->assignType, 2);
        w.write(k->param, 7);
        if (global)
            w.write(k->slot, 2);
    }
}

// ---- $60 controller map -----------------------------------------------------

CtrlMap decodeCtrlMap(BitReader& r)
{
    CtrlMap m;
    const auto count = r.read(7);
    for (std::uint32_t i = 0; i < count; ++i) {
        CtrlAssign c;
        c.cc = static_cast<u8>(r.read(7));
        c.location = static_cast<u8>(r.read(2));
        c.module = static_cast<u8>(r.read(8));
        c.param = static_cast<u8>(r.read(7));
        m.controllers.push_back(c);
    }
    return m;
}

void encode(BitWriter& w, const CtrlMap& m)
{
    w.write(static_cast<std::uint32_t>(m.controllers.size()), 7);
    for (const auto& c : m.controllers) {
        w.write(c.cc, 7);
        w.write(c.location, 2);
        w.write(c.module, 8);
        w.write(c.param, 7);
    }
}

// ---- $5B custom data --------------------------------------------------------

CustomData decodeCustomData(BitReader& r)
{
    CustomData d;
    d.location = static_cast<u8>(r.read(2));
    const auto count = r.read(8);
    for (std::uint32_t i = 0; i < count; ++i) {
        CustomModule m;
        m.index = static_cast<u8>(r.read(8));
        const auto n = r.read(8);
        for (std::uint32_t k = 0; k < n; ++k)
            m.bytes.push_back(static_cast<u8>(r.read(8)));
        d.modules.push_back(std::move(m));
    }
    return d;
}

void encode(BitWriter& w, const CustomData& d)
{
    w.write(d.location, 2);
    w.write(static_cast<std::uint32_t>(d.modules.size()), 8);
    for (const auto& m : d.modules) {
        if (m.bytes.size() > 255)
            throw std::invalid_argument("custom data: more than 255 bytes");
        w.write(m.index, 8);
        w.write(static_cast<std::uint32_t>(m.bytes.size()), 8);
        w.bytes(m.bytes);
    }
}

// ---- $5A module names -------------------------------------------------------

ModuleNames decodeModuleNames(BitReader& r)
{
    ModuleNames n;
    n.location = static_cast<u8>(r.read(2));
    n.reserved = static_cast<u8>(r.read(6));
    const auto count = r.read(8);
    for (std::uint32_t i = 0; i < count; ++i) {
        ModuleName m;
        m.index = r.u8();
        m.name = r.string(16);
        n.names.push_back(std::move(m));
    }
    return n;
}

void encode(BitWriter& w, const ModuleNames& n)
{
    w.write(n.location, 2);
    w.write(n.reserved, 6);
    w.write(static_cast<std::uint32_t>(n.names.size()), 8);
    for (const auto& m : n.names) {
        w.u8(m.index);
        w.string(m.name, 16);
    }
}

// ---- $6F textpad ------------------------------------------------------------

Textpad decodeTextpad(BitReader& r)
{
    Textpad t;
    while (r.bitsLeft() >= 8)
        t.text.push_back(static_cast<char>(r.u8()));
    return t;
}

void encode(BitWriter& w, const Textpad& t)
{
    for (char c : t.text)
        w.u8(static_cast<u8>(c));
}

// ---- $11 performance header -------------------------------------------------

PerfHeader decodePerfHeader(BitReader& r)
{
    PerfHeader h;
    h.unknown08 = r.u8();
    h.focusedSlot = static_cast<u8>(r.read(6));
    h.globalPages = static_cast<u8>(r.read(2));
    h.kbdRangeEnabled = r.u8();
    h.masterClockBpm = r.u8();
    h.unknown18 = r.u8();
    h.masterClockRun = r.u8();
    h.reserved1 = r.u8();
    h.reserved2 = r.u8();
    for (auto& s : h.slots) {
        s.patchName = r.string(16);
        for (u8* f : {&s.enabled, &s.keyboard, &s.hold, &s.bank, &s.program, &s.kbdRangeLower,
                      &s.kbdRangeUpper, &s.midiChannel, &s.reserved1, &s.reserved2})
            *f = r.u8();
    }
    return h;
}

void encode(BitWriter& w, const PerfHeader& h)
{
    w.u8(h.unknown08);
    w.write(h.focusedSlot, 6);
    w.write(h.globalPages, 2);
    for (u8 f : {h.kbdRangeEnabled, h.masterClockBpm, h.unknown18, h.masterClockRun,
                 h.reserved1, h.reserved2})
        w.u8(f);
    for (const auto& s : h.slots) {
        w.string(s.patchName, 16);
        for (u8 f : {s.enabled, s.keyboard, s.hold, s.bank, s.program, s.kbdRangeLower,
                     s.kbdRangeUpper, s.midiChannel, s.reserved1, s.reserved2})
            w.u8(f);
    }
}

// ---- dispatch -----------------------------------------------------------------

Payload decodePayload(u8 id, BitReader& r)
{
    switch (id) {
    case kPatchHeader: return decodePatchHeader(r);
    case kModuleList: return decodeModuleList(r);
    case kCurrentNotes: return decodeCurrentNotes(r);
    case kCableList: return decodeCableList(r);
    case kParamList: return decodeParamList(r);
    case kMorphMap: return decodeMorphMap(r);
    case kKnobMap: return decodeKnobMap(r, false);
    case kGlobalKnobMap: return decodeKnobMap(r, true);
    case kCtrlMap: return decodeCtrlMap(r);
    case kCustomData: return decodeCustomData(r);
    case kModuleNames: return decodeModuleNames(r);
    case kTextpad: return decodeTextpad(r);
    case kPerfHeader: return decodePerfHeader(r);
    default: throw FormatError("unknown section id");
    }
}

void encodePayload(BitWriter& w, u8 id, const Payload& payload)
{
    std::visit([&](const auto& p) {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, RawSection>)
            w.bytes(p.payload);
        else if constexpr (std::is_same_v<T, KnobMap>)
            encode(w, p, id == kGlobalKnobMap);
        else
            encode(w, p);
    }, payload);
}

std::uint16_t readU16(std::span<const u8> d, std::size_t at)
{
    return static_cast<std::uint16_t>((d[at] << 8) | d[at + 1]);
}

} // namespace

Section decodeSection(u8 id, std::span<const u8> payload)
{
    try {
        BitReader r(payload);
        Section s{id, decodePayload(id, r), 0, {}};
        s.tailPad = static_cast<u8>(r.align());
        const std::size_t used = r.bitPosition() / 8;
        s.trailing.assign(payload.begin() + static_cast<std::ptrdiff_t>(used), payload.end());
        const auto again = encodeSection(s);
        if (std::equal(again.begin(), again.end(), payload.begin(), payload.end()))
            return s;
    } catch (const std::exception&) {
        // fall through to the raw representation
    }
    return Section{id, RawSection{{payload.begin(), payload.end()}}, 0, {}};
}

std::vector<u8> encodeSection(const Section& section)
{
    BitWriter w;
    encodePayload(w, section.id, section.payload);
    w.align(section.tailPad);
    std::vector<u8> out = w.data();
    out.insert(out.end(), section.trailing.begin(), section.trailing.end());
    return out;
}

File read(std::span<const u8> data)
{
    File f;
    std::size_t p = 0;
    // Text header: CRLF-terminated lines; the binary part starts with the
    // version word, whose high byte is 0.
    while (p < data.size() && data[p] != 0) {
        std::size_t e = p;
        while (e + 1 < data.size() && !(data[e] == '\r' && data[e + 1] == '\n'))
            ++e;
        if (e + 1 >= data.size())
            throw FormatError("not a G2 file: unterminated text header");
        f.textHeader.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(p),
                                  data.begin() + static_cast<std::ptrdiff_t>(e));
        p = e + 2;
    }
    if (f.textHeader.empty() || f.textHeader[0] != "Version=Nord Modular G2 File Format 1")
        throw FormatError("not a Nord Modular G2 patch or performance file");
    if (p + 5 > data.size())
        throw FormatError("file truncated");
    const std::size_t bodyStart = p;
    f.version = readU16(data, p);
    const u8 type = data[p + 2];
    if (type > 1)
        throw FormatError("unknown file type");
    f.type = static_cast<FileType>(type);
    p += 3;
    const std::size_t end = data.size() - 2;
    while (p + 3 <= end) {
        const u8 id = data[p];
        const std::size_t len = readU16(data, p + 1);
        if (p + 3 + len > end)
            break;
        f.sections.push_back(decodeSection(id, data.subspan(p + 3, len)));
        p += 3 + len;
    }
    f.unparsedTail.assign(data.begin() + static_cast<std::ptrdiff_t>(p),
                          data.begin() + static_cast<std::ptrdiff_t>(end));
    f.storedCrc = readU16(data, end);
    f.crcValid = crc16(data.subspan(bodyStart, end - bodyStart)) == f.storedCrc;
    return f;
}

std::vector<u8> write(const File& file)
{
    std::vector<u8> out;
    for (const auto& line : file.textHeader) {
        out.insert(out.end(), line.begin(), line.end());
        out.push_back('\r');
        out.push_back('\n');
    }
    const std::size_t bodyStart = out.size();
    out.push_back(static_cast<u8>(file.version >> 8));
    out.push_back(static_cast<u8>(file.version & 0xFF));
    out.push_back(static_cast<u8>(file.type));
    for (const auto& s : file.sections) {
        const auto payload = encodeSection(s);
        if (payload.size() > 0xFFFF)
            throw std::length_error("section too long");
        out.push_back(s.id);
        out.push_back(static_cast<u8>(payload.size() >> 8));
        out.push_back(static_cast<u8>(payload.size() & 0xFF));
        out.insert(out.end(), payload.begin(), payload.end());
    }
    out.insert(out.end(), file.unparsedTail.begin(), file.unparsedTail.end());
    const std::uint16_t crc = file.crcValid
        ? crc16(std::span<const u8>(out).subspan(bodyStart))
        : file.storedCrc;
    out.push_back(static_cast<u8>(crc >> 8));
    out.push_back(static_cast<u8>(crc & 0xFF));
    return out;
}

std::vector<std::string> defaultTextHeader(FileType type)
{
    return {"Version=Nord Modular G2 File Format 1",
            type == FileType::Patch ? "Type=Patch" : "Type=Performance",
            "Version=" + std::to_string(kCurrentVersion), "Info=BUILD 320"};
}

} // namespace g2::file
