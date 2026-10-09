#include "g2/proto/molecules.hpp"

#include "g2/bitstream.hpp"
#include "g2/module_db.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace g2::proto {
namespace {

constexpr std::array<u8, 25> kRequestIds{0x02, 0x04, 0x06, 0x08, 0x10, 0x20, 0x28, 0x2C, 0x2E, 0x3B, 0x3C, 0x3D, 0x55,
                                         0x58, 0x59, 0x5E, 0x61, 0x63, 0x66, 0x68, 0x6E, 0x70, 0x81, 0x74, 0x75};
constexpr std::array<u8, 6> kLocationRequestIds{0x4B, 0x4C, 0x4E, 0x4F, 0x53, 0x71};
constexpr std::array<u8, 13> kSectionIds{0x11, 0x21, 0x4A, 0x4D, 0x52, 0x5A, 0x5B, 0x5F, 0x60, 0x62, 0x65, 0x69, 0x6F};

template <std::size_t N>
bool contains(const std::array<u8, N>& ids, u8 v)
{
    return std::find(ids.begin(), ids.end(), v) != ids.end();
}

void str16(BitWriter& w, const std::string& s)
{
    std::string t = s.substr(0, 16);
    t.erase(std::find(t.begin(), t.end(), '\0'), t.end());
    w.string(t, 16);
}

void bit(BitWriter& w, bool b) { w.write(b ? 1u : 0u, 1); }

void section(BitWriter& w, const file::Section& s)
{
    const auto payload = file::encodeSection(s);
    if (payload.size() > 0xFFFF)
        throw std::invalid_argument("section longer than 0xFFFF bytes");
    w.u8(s.id);
    w.u16(static_cast<std::uint16_t>(payload.size()));
    w.bytes(payload);
}

file::Section readSection(BitReader& r, std::span<const u8> body)
{
    const u8 sid = r.u8();
    const std::uint16_t length = r.u16();
    const std::size_t at = r.bitPosition() / 8;
    if (at + length > body.size())
        throw FormatError("section past the end of the message");
    const auto payload = body.subspan(at, length);
    for (std::size_t i = 0; i < length; ++i)
        r.u8();
    return file::decodeSection(sid, payload);
}

// ---- Bodies (after the id byte) --------------------------------------------------

void body(BitWriter&, const Request&) {}
void body(BitWriter& w, const LocationRequest& m) { w.u8(m.location); }

void body(BitWriter& w, const SynthData& d)
{
    const auto& s = d.settings;
    str16(w, s.name);
    bit(w, s.perfMode);
    w.write(s.patchSortMode, 7);
    w.write(s.perfSortMode, 7);
    w.align();
    w.u8(s.focusBank);
    w.u8(s.focusProg);
    bit(w, s.memoryProtect);
    w.align();
    for (u8 c : s.midiChannels)
        w.u8(c);
    w.u8(s.globalChannel);
    w.u8(s.sysExId);
    bit(w, s.localOn);
    w.align();
    w.u8(s.programChangeMode);
    w.u8(s.controllerMode);
    bit(w, s.sendArp);
    bit(w, s.sendClock);
    bit(w, s.ignoreExternalClock);
    w.align();
    w.u8(static_cast<u8>(s.masterTune));
    bit(w, s.octaveShiftEnable);
    w.align();
    w.u8(static_cast<u8>(s.octaveShift));
    w.u8(static_cast<u8>(s.transpose));
    w.u8(s.vibratoRate);
    bit(w, s.sustainPolarity);
    bit(w, true);
    w.align();
    w.u8(s.controlPedalGain);
    for (u8 b : s.reserved)
        w.u8(b);
}

void body(BitWriter& w, const Voices& m)
{
    for (u8 v : m.voices)
        w.u8(v);
}
void body(BitWriter& w, const SlotFlags& m)
{
    w.write(0, 4);
    for (bool b : m.slots)
        bit(w, b);
}
void body(BitWriter& w, const SlotFocus& m) { w.u8(m.slot); }
void body(BitWriter& w, const FlashCommand& m)
{
    w.u8(m.slot);
    w.u8(m.bank);
    w.u8(m.prog);
}
void body(BitWriter& w, const FlashDelete& m)
{
    w.bytes(std::array<u8, 4>{m.type, m.bank, m.prog, m.origin});
}
void body(BitWriter& w, const FlashResult& m)
{
    w.bytes(std::array<u8, 5>{m.type, m.bank, m.prog, m.origin, m.result});
}
void body(BitWriter& w, const FlashDeleteRange& m)
{
    w.bytes(std::array<u8, 5>{m.type, m.bank1, m.prog1, m.bank2, m.prog2});
    if (m.id == id::FlashDeleteRange)
        w.u8(m.origin);
}
void body(BitWriter& w, const FlashUsage& m)
{
    w.u8(static_cast<u8>(m.value & 0x7F));
    w.u8(static_cast<u8>(m.value >> 7));
}
void body(BitWriter& w, const FlashData& m)
{
    w.u8(m.flag);
    w.u8(m.type);
    for (const auto& item : m.items) {
        using K = FlashItem::Kind;
        switch (item.kind) {
        case K::SetEntry:
            w.bytes(std::array<u8, 3>{3, item.bank, item.prog});
            break;
        case K::SetProg:
            w.bytes(std::array<u8, 2>{1, item.prog});
            break;
        case K::Empty:
        case K::EndOfList:
        case K::EndOfChunk:
            w.u8(static_cast<u8>(item.kind));
            break;
        case K::Name:
            if (!item.name.empty() && static_cast<u8>(item.name[0]) >= 1 && static_cast<u8>(item.name[0]) <= 5)
                throw std::invalid_argument("flash name starting with a tag byte");
            str16(w, item.name);
            w.u8(item.category);
            break;
        }
    }
}
void body(BitWriter& w, const FlashDumpMarker& m)
{
    w.bytes(std::array<u8, 4>{m.code, m.type, m.bank, m.prog});
    if (m.code == 2)
        str16(w, m.name);
}
void body(BitWriter& w, const FlashRawData& m)
{
    w.bytes(std::array<u8, 3>{m.type, m.bank, m.prog});
    str16(w, m.name);
    if (m.data && m.data->size() >= 0xFFFF)
        throw std::invalid_argument("flash data too long");
    w.u16(m.data ? static_cast<std::uint16_t>(m.data->size()) : std::uint16_t{0xFFFF});
    w.u8(m.version);
    if (m.data)
        w.bytes(*m.data);
}
void body(BitWriter& w, const GlobalKnobAssign& m)
{
    w.write(m.assignType, 2);
    w.write(m.slot, 2);
    w.write(m.location, 2);
    w.write(0, 2);
    w.u8(m.module);
    w.u8(m.param);
    w.u16(m.knob);
}
void body(BitWriter& w, const KnobDeassign& m) { w.u16(m.knob); }
void body(BitWriter& w, const PageFocus& m) { w.u8(m.page); }
void body(BitWriter& w, const PerformanceRelease& m) { w.u8(m.session); }
void body(BitWriter& w, const CtrlAssign& m)
{
    w.bytes(std::array<u8, 4>{m.location, m.module, m.param, m.cc});
}
void body(BitWriter& w, const CtrlDeassign& m) { w.u8(m.cc); }
void body(BitWriter& w, const KnobAssign& m)
{
    w.u8(m.module);
    w.u8(m.param);
    w.write(m.location, 2);
    w.write(m.assignType, 2);
    w.write(0, 4);
    w.u16(m.knob);
}
void body(BitWriter& w, const NameDump& m) { str16(w, m.name); }
void body(BitWriter& w, const Uprate& m) { w.bytes(std::array<u8, 3>{m.location, m.module, m.uprate}); }
void body(BitWriter& w, const ModeChange& m)
{
    w.bytes(std::array<u8, 4>{m.location, m.module, m.mode, m.value});
}
void body(BitWriter& w, const ParamFocus& m)
{
    w.bytes(std::array<u8, 4>{m.flag, m.location, m.module, m.param});
}
void body(BitWriter& w, const ModuleNew& m)
{
    w.bytes(std::array<u8, 8>{m.type, m.location, m.index, m.col, m.row, m.color, m.uprate, m.locked});
    w.bytes(m.modes);
    str16(w, m.name);
}
void body(BitWriter& w, const ModuleRecolor& m) { w.bytes(std::array<u8, 3>{m.location, m.module, m.color}); }
void body(BitWriter& w, const ModuleDelete& m) { w.bytes(std::array<u8, 2>{m.location, m.module}); }
void body(BitWriter& w, const ModuleRename& m)
{
    w.bytes(std::array<u8, 2>{m.location, m.module});
    str16(w, m.name);
}
void body(BitWriter& w, const ModuleMove& m) { w.bytes(std::array<u8, 4>{m.location, m.module, m.col, m.row}); }
void body(BitWriter& w, const SessionRequest& m) { w.u8(m.slot); }
void body(BitWriter& w, const SessionNumber& m) { w.bytes(std::array<u8, 2>{m.slot, m.session}); }
void body(BitWriter& w, const DumpDestination& m)
{
    w.bytes(std::array<u8, 3>{m.target, m.bank, m.prog});
    str16(w, m.name);
}
void body(BitWriter& w, const Blink& m)
{
    w.u8(m.start);
    w.bytes(m.data);
}
void body(BitWriter& w, const PerformanceMode& m) { w.bytes(std::array<u8, 2>{m.perfMode, m.flag}); }
void body(BitWriter& w, const PerfHeaderParam& m)
{
    w.u8(m.scope);
    w.u8(m.param);
    const bool hasValue = m.scope > 3 && m.param < 3;
    if (hasValue != m.value.has_value())
        throw std::invalid_argument("CMPerformanceHeaderParam: value only with scope > 3 and param < 3");
    if (m.value)
        w.u8(*m.value);
}
void body(BitWriter& w, const ParamChange& m)
{
    w.bytes(std::array<u8, 5>{m.location, m.module, m.param, m.value, m.variation});
}
void body(BitWriter& w, const CustomData& m)
{
    if (m.bytes.size() > 0xFF)
        throw std::invalid_argument("custom data longer than 255 bytes");
    w.bytes(std::array<u8, 3>{m.location, m.module, static_cast<u8>(m.bytes.size())});
    w.bytes(m.bytes);
}
void body(BitWriter& w, const MorphChange& m)
{
    if (m.range < -127 || m.range > 127)
        throw std::invalid_argument("morph range out of -127..127");
    w.bytes(std::array<u8, 7>{m.location, m.module, m.param, m.morph, static_cast<u8>(m.range < 0 ? -m.range : m.range),
                              static_cast<u8>(m.range < 0 ? 1 : 0), m.variation});
}
void body(BitWriter& w, const VariationCopy& m) { w.bytes(std::array<u8, 2>{m.from, m.to}); }
void body(BitWriter& w, const CableEdit& m)
{
    switch (m.id) {
    case id::CableConnect: // pad3, b1 1 (not a dump), b1 isVA, b3 color
        w.write(0, 3);
        bit(w, true);
        bit(w, m.va);
        w.write(m.color, 3);
        break;
    case id::CableDelete: // pad6, b1 1, b1 isVA
        w.write(0, 6);
        bit(w, true);
        bit(w, m.va);
        break;
    default: // 54: pad4, b1 isVA, b3 color
        w.write(0, 4);
        bit(w, m.va);
        w.write(m.color, 3);
        break;
    }
    if (m.fromConn > 0x3F || m.toConn > 0x3F)
        throw std::invalid_argument("cable connector out of 0..63");
    w.u8(m.fromModule);
    w.u8(static_cast<u8>(m.fromConn | (m.fromIsOutput ? 0x40 : 0)));
    w.u8(m.toModule);
    w.u8(static_cast<u8>(m.toConn | (m.toIsOutput ? 0x40 : 0)));
}
void body(BitWriter& w, const PlayNote& m) { w.bytes(std::array<u8, 2>{static_cast<u8>(m.off ? 1 : 0), m.note}); }
void body(BitWriter& w, const ClockInfo& m)
{
    w.u8(m.flag);
    w.u16(m.bpm);
}
void body(BitWriter& w, const VariationSelect& m) { w.u8(m.variation); }
void body(BitWriter& w, const PatchLoad& m)
{
    w.u8(m.isVA);
    w.bytes(m.counters);
}
void body(BitWriter& w, const EditorSync& m) { w.u8(m.begin ? 1 : 0); }
void body(BitWriter& w, const Exception& m) { w.u8(m.code); }
void body(BitWriter&, const Ack&) {}
void body(BitWriter& w, const MidiLearn& m) { w.bytes(std::array<u8, 2>{m.slot, m.cc}); }
void body(BitWriter& w, const MutaLock& m) { w.bytes(std::array<u8, 3>{m.location, m.module, m.locked}); }
void body(BitWriter& w, const CompletePerformance& m)
{
    w.u8(id::PerfName);
    str16(w, m.name);
    for (const auto& s : m.sections)
        section(w, s);
}
void body(BitWriter& w, const Raw& m) { w.bytes(m.body); }

// ---- Ids ----------------------------------------------------------------------

struct IdOf {
    template <class T>
    u8 operator()(const T& m) const
    {
        if constexpr (requires(const T& t) { t.id; })
            return m.id;
        else if constexpr (std::is_same_v<T, SectionDump>)
            return m.section.id;
        else if constexpr (std::is_same_v<T, SynthData>)
            return id::SynthData;
        else if constexpr (std::is_same_v<T, Voices>)
            return id::Voices;
        else if constexpr (std::is_same_v<T, SlotFocus>)
            return id::SlotFocus;
        else if constexpr (std::is_same_v<T, FlashDelete>)
            return id::FlashDelete;
        else if constexpr (std::is_same_v<T, FlashResult>)
            return id::FlashResult;
        else if constexpr (std::is_same_v<T, FlashUsage>)
            return id::FlashUsage;
        else if constexpr (std::is_same_v<T, FlashData>)
            return id::FlashData;
        else if constexpr (std::is_same_v<T, FlashDumpMarker>)
            return id::FlashDumpMarker;
        else if constexpr (std::is_same_v<T, FlashRawData>)
            return id::FlashRawData;
        else if constexpr (std::is_same_v<T, GlobalKnobAssign>)
            return id::GlobalKnobAssign;
        else if constexpr (std::is_same_v<T, PerformanceRelease>)
            return id::PerformanceRelease;
        else if constexpr (std::is_same_v<T, CtrlAssign>)
            return id::CtrlAssign;
        else if constexpr (std::is_same_v<T, CtrlDeassign>)
            return id::CtrlDeassign;
        else if constexpr (std::is_same_v<T, KnobAssign>)
            return id::KnobAssign;
        else if constexpr (std::is_same_v<T, Uprate>)
            return id::Uprate;
        else if constexpr (std::is_same_v<T, ModeChange>)
            return id::ModeChange;
        else if constexpr (std::is_same_v<T, ParamFocus>)
            return id::ParamFocus;
        else if constexpr (std::is_same_v<T, ModuleNew>)
            return id::ModuleNew;
        else if constexpr (std::is_same_v<T, ModuleRecolor>)
            return id::ModuleRecolor;
        else if constexpr (std::is_same_v<T, ModuleDelete>)
            return id::ModuleDelete;
        else if constexpr (std::is_same_v<T, ModuleRename>)
            return id::ModuleName;
        else if constexpr (std::is_same_v<T, ModuleMove>)
            return id::ModuleMove;
        else if constexpr (std::is_same_v<T, SessionRequest>)
            return id::SessionRequest;
        else if constexpr (std::is_same_v<T, DumpDestination>)
            return id::DumpDestination;
        else if constexpr (std::is_same_v<T, PerformanceMode>)
            return id::PerformanceMode;
        else if constexpr (std::is_same_v<T, PerfHeaderParam>)
            return id::PerfHeaderParam;
        else if constexpr (std::is_same_v<T, ParamChange>)
            return id::ParamChange;
        else if constexpr (std::is_same_v<T, CustomData>)
            return id::CustomData;
        else if constexpr (std::is_same_v<T, MorphChange>)
            return id::MorphChange;
        else if constexpr (std::is_same_v<T, VariationCopy>)
            return id::VariationCopy;
        else if constexpr (std::is_same_v<T, PlayNote>)
            return id::PlayNote;
        else if constexpr (std::is_same_v<T, ClockInfo>)
            return id::ClockInfo;
        else if constexpr (std::is_same_v<T, VariationSelect>)
            return id::VariationSelect;
        else if constexpr (std::is_same_v<T, PatchLoad>)
            return id::PatchLoad;
        else if constexpr (std::is_same_v<T, EditorSync>)
            return id::EditorSync;
        else if constexpr (std::is_same_v<T, Exception>)
            return id::Exception;
        else if constexpr (std::is_same_v<T, Ack>)
            return id::Ack;
        else if constexpr (std::is_same_v<T, MidiLearn>)
            return id::MidiLearn;
        else if constexpr (std::is_same_v<T, MutaLock>)
            return id::MutaLock;
        else if constexpr (std::is_same_v<T, CompletePerformance>)
            return id::CompletePerformance;
        else
            static_assert(sizeof(T) == 0, "molecule without an id");
    }
};

// ---- Decoding -------------------------------------------------------------------

std::string readStr16(BitReader& r) { return r.string(16); }

// One molecule starting at r (positioned after the id byte), or nullopt for
// an id this codec does not know. Throws FormatError when the body is cut short.
std::optional<Molecule> read(u8 mid, BitReader& r, std::span<const u8> body)
{
    if (contains(kRequestIds, mid))
        return Request{mid};
    if (contains(kLocationRequestIds, mid))
        return LocationRequest{mid, r.u8()};
    switch (mid) {
    case id::SynthData: {
        SynthSettings s;
        s.name = readStr16(r);
        s.perfMode = r.read(1) != 0;
        s.patchSortMode = static_cast<u8>(r.read(7));
        s.perfSortMode = static_cast<u8>(r.read(7));
        r.align();
        s.focusBank = r.u8();
        s.focusProg = r.u8();
        s.memoryProtect = r.read(1) != 0;
        r.align();
        for (u8& c : s.midiChannels)
            c = r.u8();
        s.globalChannel = r.u8();
        s.sysExId = r.u8();
        s.localOn = r.read(1) != 0;
        r.align();
        s.programChangeMode = r.u8();
        s.controllerMode = r.u8();
        s.sendArp = r.read(1) != 0;
        s.sendClock = r.read(1) != 0;
        s.ignoreExternalClock = r.read(1) != 0;
        r.align();
        s.masterTune = static_cast<std::int8_t>(r.u8());
        s.octaveShiftEnable = r.read(1) != 0;
        r.align();
        s.octaveShift = static_cast<std::int8_t>(r.u8());
        s.transpose = static_cast<std::int8_t>(r.u8());
        s.vibratoRate = r.u8();
        s.sustainPolarity = r.read(1) != 0;
        r.read(1); // always 1
        r.align();
        s.controlPedalGain = r.u8();
        for (u8& b : s.reserved)
            b = r.u8();
        return SynthData{s};
    }
    case id::Voices: {
        Voices v;
        for (u8& x : v.voices)
            x = r.u8();
        return v;
    }
    case id::SlotSelection:
    case id::KeyboardFocus: {
        SlotFlags f{mid, {}};
        r.read(4);
        for (bool& b : f.slots)
            b = r.read(1) != 0;
        return f;
    }
    case id::SlotFocus:
        return SlotFocus{r.u8()};
    case id::FlashLoad:
    case id::FlashStore:
    case id::FlashDataRequest:
    case id::FlashDeleteDump:
    case id::FlashDumpRequest: {
        FlashCommand c{mid, 0, 0, 0};
        c.slot = r.u8();
        c.bank = r.u8();
        c.prog = r.u8();
        return c;
    }
    case id::FlashDelete: {
        FlashDelete d;
        d.type = r.u8();
        d.bank = r.u8();
        d.prog = r.u8();
        d.origin = r.u8();
        return d;
    }
    case id::FlashResult: {
        FlashResult d;
        d.type = r.u8();
        d.bank = r.u8();
        d.prog = r.u8();
        d.origin = r.u8();
        d.result = r.u8();
        return d;
    }
    case id::FlashDeleteRange:
    case id::FlashDeleteRangeDump: {
        FlashDeleteRange d{mid, 0, 0, 0, 0, 0, 0};
        d.type = r.u8();
        d.bank1 = r.u8();
        d.prog1 = r.u8();
        d.bank2 = r.u8();
        d.prog2 = r.u8();
        if (mid == id::FlashDeleteRange)
            d.origin = r.u8();
        return d;
    }
    case id::FlashUsage: {
        const u8 lo = r.u8();
        const u8 hi = r.u8();
        if (lo > 0x7F)
            return std::nullopt; // not a 7-bit pair: keep it raw
        return FlashUsage{static_cast<std::uint16_t>(lo | (hi << 7))};
    }
    case id::FlashData: {
        FlashData d;
        d.flag = r.u8();
        d.type = r.u8();
        using K = FlashItem::Kind;
        while (!r.atEnd()) {
            const u8 tag = r.u8();
            FlashItem item;
            if (tag == 3) {
                item.kind = K::SetEntry;
                item.bank = r.u8();
                item.prog = r.u8();
            } else if (tag == 1) {
                item.kind = K::SetProg;
                item.prog = r.u8();
            } else if (tag == 2 || tag == 4 || tag == 5) {
                item.kind = static_cast<K>(tag);
            } else {
                // The tag is the name's first character (CMFlashDataDump::ReadStream
                // steps back 8 bits and reads the string).
                std::string name(1, static_cast<char>(tag));
                if (tag != 0)
                    name += r.string(15);
                else
                    name.clear();
                item.kind = K::Name;
                item.name = name;
                item.category = r.u8();
            }
            d.items.push_back(item);
            if (tag == 4 || tag == 5)
                break;
        }
        return d;
    }
    case id::FlashDumpMarker: {
        FlashDumpMarker m;
        m.code = r.u8();
        m.type = r.u8();
        m.bank = r.u8();
        m.prog = r.u8();
        if (m.code == 2)
            m.name = readStr16(r);
        return m;
    }
    case id::FlashRawData: {
        FlashRawData m;
        m.type = r.u8();
        m.bank = r.u8();
        m.prog = r.u8();
        m.name = readStr16(r);
        const std::uint16_t size = r.u16();
        m.version = r.u8();
        if (size != 0xFFFF) {
            std::vector<u8> data(size);
            for (u8& b : data)
                b = r.u8();
            m.data = std::move(data);
        }
        return m;
    }
    case id::CompletePerformance: {
        CompletePerformance p;
        if (r.u8() != id::PerfName)
            throw FormatError("CMCompletePerformanceDump without its name");
        p.name = readStr16(r);
        while (!r.atEnd()) {
            p.sections.push_back(readSection(r, body));
            if (p.sections.back().id == id::GlobalKnobMap)
                break;
        }
        return p;
    }
    case id::GlobalKnobAssign: {
        GlobalKnobAssign m;
        m.assignType = static_cast<u8>(r.read(2));
        m.slot = static_cast<u8>(r.read(2));
        m.location = static_cast<u8>(r.read(2));
        r.read(2);
        m.module = r.u8();
        m.param = r.u8();
        m.knob = r.u16();
        return m;
    }
    case id::GlobalKnobDeassign:
    case id::KnobDeassign:
        return KnobDeassign{mid, r.u16()};
    case id::GlobalPageFocus:
    case id::PageFocus:
        return PageFocus{mid, r.u8()};
    case id::PerformanceRelease:
        return PerformanceRelease{r.u8()};
    case id::CtrlAssign: {
        CtrlAssign m;
        m.location = r.u8();
        m.module = r.u8();
        m.param = r.u8();
        m.cc = r.u8();
        return m;
    }
    case id::CtrlDeassign:
        return CtrlDeassign{r.u8()};
    case id::KnobAssign: {
        KnobAssign m;
        m.module = r.u8();
        m.param = r.u8();
        m.location = static_cast<u8>(r.read(2));
        m.assignType = static_cast<u8>(r.read(2));
        r.read(4);
        m.knob = r.u16();
        return m;
    }
    case id::PatchName:
    case id::PerfName:
        return NameDump{mid, readStr16(r)};
    case id::Uprate: {
        Uprate m;
        m.location = r.u8();
        m.module = r.u8();
        m.uprate = r.u8();
        return m;
    }
    case id::ModeChange: {
        ModeChange m;
        m.location = r.u8();
        m.module = r.u8();
        m.mode = r.u8();
        m.value = r.u8();
        return m;
    }
    case id::ParamFocus: {
        ParamFocus m;
        m.flag = r.u8();
        m.location = r.u8();
        m.module = r.u8();
        m.param = r.u8();
        return m;
    }
    case id::ModuleNew: {
        ModuleNew m;
        m.type = r.u8();
        m.location = r.u8();
        m.index = r.u8();
        m.col = r.u8();
        m.row = r.u8();
        m.color = r.u8();
        m.uprate = r.u8();
        m.locked = r.u8();
        const auto* def = db::find(m.type);
        if (!def)
            return std::nullopt; // the mode count is unknown: keep it raw
        for (std::size_t i = 0; i < def->modes.size(); ++i)
            m.modes.push_back(r.u8());
        m.name = readStr16(r);
        return m;
    }
    case id::ModuleRecolor: {
        ModuleRecolor m;
        m.location = r.u8();
        m.module = r.u8();
        m.color = r.u8();
        return m;
    }
    case id::ModuleDelete: {
        ModuleDelete m;
        m.location = r.u8();
        m.module = r.u8();
        return m;
    }
    case id::ModuleName: {
        ModuleRename m;
        m.location = r.u8();
        m.module = r.u8();
        m.name = readStr16(r);
        return m;
    }
    case id::ModuleMove: {
        ModuleMove m;
        m.location = r.u8();
        m.module = r.u8();
        m.col = r.u8();
        m.row = r.u8();
        return m;
    }
    case id::SessionRequest:
        return SessionRequest{r.u8()};
    case id::SessionDump:
    case id::PatchRelease: {
        SessionNumber m{mid, 0, 0};
        m.slot = r.u8();
        m.session = r.u8();
        return m;
    }
    case id::DumpDestination: {
        DumpDestination m;
        m.target = r.u8();
        m.bank = r.u8();
        m.prog = r.u8();
        m.name = readStr16(r);
        return m;
    }
    case id::Leds:
    case id::Meters: {
        Blink m{mid, 0, {}};
        m.start = r.u8();
        while (!r.atEnd())
            m.data.push_back(r.u8());
        return m;
    }
    case id::PerformanceMode: {
        PerformanceMode m;
        m.perfMode = r.u8();
        m.flag = r.u8();
        return m;
    }
    case id::PerfHeaderParam: {
        PerfHeaderParam m;
        m.scope = r.u8();
        m.param = r.u8();
        if (m.scope > 3 && m.param < 3)
            m.value = r.u8();
        return m;
    }
    case id::ParamChange: {
        ParamChange m;
        m.location = r.u8();
        m.module = r.u8();
        m.param = r.u8();
        m.value = r.u8();
        m.variation = r.u8();
        return m;
    }
    case id::CustomData: {
        CustomData m;
        m.location = r.u8();
        m.module = r.u8();
        const u8 n = r.u8();
        for (u8 i = 0; i < n; ++i)
            m.bytes.push_back(r.u8());
        return m;
    }
    case id::MorphChange: {
        MorphChange m;
        m.location = r.u8();
        m.module = r.u8();
        m.param = r.u8();
        m.morph = r.u8();
        const u8 magnitude = r.u8();
        const u8 negative = r.u8();
        if (magnitude > 127 || negative > 1)
            return std::nullopt;
        m.range = negative ? -magnitude : magnitude;
        m.variation = r.u8();
        return m;
    }
    case id::VariationCopy: {
        VariationCopy m;
        m.from = r.u8();
        m.to = r.u8();
        return m;
    }
    case id::CableConnect:
    case id::CableDelete:
    case id::CableRecolor: {
        CableEdit m{mid, true, 0, 0, 0, true, 0, 0, false};
        if (mid == id::CableConnect) {
            r.read(3);
            r.read(1);
            m.va = r.read(1) != 0;
            m.color = static_cast<u8>(r.read(3));
        } else if (mid == id::CableDelete) {
            r.read(6);
            r.read(1);
            m.va = r.read(1) != 0;
        } else {
            r.read(4);
            m.va = r.read(1) != 0;
            m.color = static_cast<u8>(r.read(3));
        }
        m.fromModule = r.u8();
        const u8 from = r.u8();
        m.toModule = r.u8();
        const u8 to = r.u8();
        m.fromConn = from & 0x3F;
        m.fromIsOutput = (from & 0x40) != 0;
        m.toConn = to & 0x3F;
        m.toIsOutput = (to & 0x40) != 0;
        return m;
    }
    case id::PlayNote: {
        const u8 off = r.u8();
        const u8 note = r.u8();
        if (off > 1)
            return std::nullopt;
        return PlayNote{off == 1, note};
    }
    case id::ClockInfo: {
        ClockInfo m;
        m.flag = r.u8();
        m.bpm = r.u16();
        return m;
    }
    case id::VariationSelect:
        return VariationSelect{r.u8()};
    case id::PatchLoad: {
        PatchLoad m;
        m.isVA = r.u8();
        for (u8& b : m.counters)
            b = r.u8();
        return m;
    }
    case id::EditorSync: {
        const u8 v = r.u8();
        if (v > 1)
            return std::nullopt;
        return EditorSync{v == 1};
    }
    case id::Exception:
        return Exception{r.u8()};
    case id::Ack:
        return Ack{};
    case id::MidiLearn: {
        MidiLearn m;
        m.slot = r.u8();
        m.cc = r.u8();
        return m;
    }
    case id::MutaLock: {
        MutaLock m;
        m.location = r.u8();
        m.module = r.u8();
        m.locked = r.u8();
        return m;
    }
    default:
        break;
    }
    if (contains(kSectionIds, mid)) {
        // Back to the id: the section reader takes id, length and payload.
        BitReader sr(body.subspan(r.bitPosition() / 8 - 1));
        auto s = readSection(sr, body.subspan(r.bitPosition() / 8 - 1));
        for (std::size_t i = 1; i < sr.bitPosition() / 8; ++i)
            r.u8();
        return SectionDump{std::move(s)};
    }
    return std::nullopt;
}

} // namespace

u8 idOf(const Molecule& m) { return std::visit(IdOf{}, m); }

Molecule sectionMolecule(file::Section section) { return SectionDump{std::move(section)}; }

void encode(const Molecule& m, std::vector<u8>& out)
{
    BitWriter w;
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, SectionDump>) {
                section(w, x.section);
            } else {
                w.u8(IdOf{}(x));
                body(w, x);
                w.align();
            }
        },
        m);
    out.insert(out.end(), w.data().begin(), w.data().end());
}

std::vector<u8> encode(std::span<const Molecule> molecules)
{
    std::vector<u8> out;
    for (const auto& m : molecules)
        encode(m, out);
    return out;
}

std::vector<Molecule> decode(std::span<const u8> body)
{
    std::vector<Molecule> out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        const auto rest = body.subspan(pos);
        const u8 mid = rest[0];
        std::optional<Molecule> m;
        std::size_t used = 0;
        try {
            BitReader r(rest);
            r.u8();
            m = read(mid, r, rest);
            used = (r.bitPosition() + 7) / 8;
            if (m) {
                // Keep a typed molecule only if it re-encodes to the same bytes
                // (non-zero padding or reserved bits stay raw).
                std::vector<u8> again;
                encode(*m, again);
                if (!std::equal(again.begin(), again.end(), rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(used))
                    || again.size() != used)
                    m.reset();
            }
        } catch (const std::exception&) {
            m.reset();
        }
        if (!m) {
            out.push_back(Raw{mid, std::vector<u8>(rest.begin() + 1, rest.end())});
            break;
        }
        out.push_back(std::move(*m));
        pos += used;
    }
    return out;
}

} // namespace g2::proto
