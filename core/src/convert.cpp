#include "g2/convert.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// A port of Clavia's file converters (editor v1.62). The NSFile_V11 objects
// the converters work on are mirrored here with the same containers and the
// same operations, so that ordering, duplicate handling and the side effects
// of each operation (index shifts, dropped entries) come out as in the
// original. Addresses are those of the Mac editor binary; see
// re/notes/pch2-format.md §7 and convert.hpp.

namespace g2::convert {
namespace {

using namespace file;

// ---- keys ---------------------------------------------------------------------

// NSFile_V7::CParamSpec: context, module, parameter. operator< @0002398a.
struct ParamSpec {
    int ctx = 0;
    u8 module = 0;
    u8 param = 0;
    auto key() const { return std::tie(ctx, module, param); }
    bool operator<(const ParamSpec& o) const { return key() < o.key(); }
    bool operator==(const ParamSpec& o) const { return key() == o.key(); }
};

// CMorphMapData_11::CMorphSpec: a parameter and a morph group. operator< @00064754.
struct MorphSpec {
    ParamSpec p;
    u8 group = 0;
    bool operator<(const MorphSpec& o) const { return std::tie(p, group) < std::tie(o.p, o.group); }
};

// NSFile_V11::CBarbTargetSpec, a knob's target: a parameter, the device
// assign type and the performance slot (4 in a patch's own knob map).
// operator< @00024168 orders by slot, then parameter, then device.
struct BarbSpec {
    ParamSpec p;
    int device = 0;
    u8 slot = 4;
    bool operator<(const BarbSpec& o) const
    {
        return std::tie(slot, p, device) < std::tie(o.slot, o.p, o.device);
    }
};

// ---- the file objects ---------------------------------------------------------

// NSFile_V11::CModuleData_11::CModuleDumpItem.
struct ModuleItem {
    u8 type = 0, index = 0, col = 0, row = 0;
    u8 color = 0;
    u8 bandwidth = 0;
    std::vector<u8> selectors; // "modes"
    u8 locked = 0;
};

struct ModuleData {
    int ctx = 0;
    std::vector<ModuleItem> list;
};

// NSFile_V7::CConnector: module, connector, output flag.
struct Connector {
    u8 module = 0, conn = 0;
    bool output = false;
};

// NSFile_V11::CCableSegmentSpec.
struct Segment {
    u8 color = 0;
    Connector a, b;
};

struct CableData {
    int ctx = 0;
    std::vector<Segment> list;

    // CCableData_11::RemoveConnector @00065e14: removes the cables at the
    // connector and renumbers the module's higher connectors of the same kind.
    void removeConnector(const Connector& c)
    {
        auto renumber = [&](Connector& end) {
            if (end.module == c.module && end.output == c.output && c.conn < end.conn)
                --end.conn;
        };
        auto at = [&](const Connector& end) {
            return end.module == c.module && end.output == c.output && end.conn == c.conn;
        };
        for (auto it = list.begin(); it != list.end();) {
            if (at(it->a) || at(it->b)) {
                it = list.erase(it);
                continue;
            }
            renumber(it->a);
            renumber(it->b);
            ++it;
        }
    }
};

// NSFile_V11::CModuleParamData_11: per module a parameter count and, per
// "param setting" (variation), a list of values.
struct ParamData {
    int ctx = 0;
    u8 settingCount = 0;    // +0x40: the number of AddParamSetting calls
    std::vector<u8> settings; // +0x1c: the variation ids, in the order added
    struct Module {
        u8 count = 0; // +0x24 map
        std::map<u8, std::vector<u8>> lists; // +0x04 map
    };
    std::map<u8, Module> modules;

    bool hasSetting(u8 v) const { return std::find(settings.begin(), settings.end(), v) != settings.end(); }

    // AddParamSettingToModule @000683c0.
    void addSettingToModule(Module& m, u8 v) { m.lists.emplace(v, std::vector<u8>(m.count, 0)); }

    // AddParamSetting @000684f2.
    void addSetting(u8 v)
    {
        ++settingCount;
        settings.push_back(v);
        for (auto& [id, m] : modules)
            if (!m.lists.count(v))
                addSettingToModule(m, v);
    }

    // AddModule @0006856c.
    void addModule(u8 id, u8 count)
    {
        auto& m = modules.emplace(id, Module{count, {}}).first->second;
        for (u8 v : settings)
            addSettingToModule(m, v);
    }

    std::vector<u8>* list(u8 module, u8 v)
    {
        auto m = modules.find(module);
        if (m == modules.end())
            return nullptr;
        auto l = m->second.lists.find(v);
        return l == m->second.lists.end() ? nullptr : &l->second;
    }

    // GetParamValue @00067dc8: 0 past the end of the list.
    u8 get(u8 module, u8 v, u8 p)
    {
        const auto* l = list(module, v);
        return l && p < l->size() ? (*l)[p] : 0;
    }

    // SetParamValue @00067f48: nothing past the end of the list.
    void set(u8 module, u8 v, u8 p, u8 value)
    {
        if (auto* l = list(module, v); l && p < l->size())
            (*l)[p] = value;
    }

    // AddParam @00068274: the value inserted at `pos` in every variation.
    // (std::advance on the list wraps through its end.)
    void addParam(u8 module, u8 pos, u8 value)
    {
        auto m = modules.find(module);
        if (m == modules.end())
            return; // the original writes through an end iterator here
        ++m->second.count;
        for (u8 v : settings)
            if (auto* l = list(module, v)) {
                const std::size_t at = pos % (l->size() + 1);
                l->insert(l->begin() + static_cast<std::ptrdiff_t>(at), value);
            }
    }

    // RemoveParam @0006831e: the value at `pos` removed from every variation.
    void removeParam(u8 module, u8 pos)
    {
        auto m = modules.find(module);
        if (m == modules.end())
            return;
        --m->second.count;
        for (u8 v : settings)
            if (auto* l = list(module, v)) {
                const std::size_t at = pos % (l->size() + 1);
                if (at < l->size()) // the original would erase the list's end node
                    l->erase(l->begin() + static_cast<std::ptrdiff_t>(at));
            }
    }
};

// NSFile_V11::CMorphMapData_11: per variation, a map of morphed parameters
// to their range.
struct MorphData {
    u8 settingCount = 0; // +0x3c
    std::array<u8, 8> keyboard{}; // EKeyboardMorphAssign per group
    std::map<u8, std::map<MorphSpec, std::int8_t>> vars;

    bool hasSetting(u8 v) const { return vars.count(v) != 0; }

    // AddParamSetting @000661ca.
    void addSetting(u8 v)
    {
        ++settingCount;
        vars.emplace(v, std::map<MorphSpec, std::int8_t>{});
    }

    // IsParamMorphed @0006912a.
    bool isMorphed(u8 v, const ParamSpec& p, u8 g) const
    {
        auto it = vars.find(v);
        return it != vars.end() && it->second.count(MorphSpec{p, g}) != 0;
    }

    // GetMorphDelta @000690d4, ChangeMorph @0006755a (called on morphed parameters).
    std::int8_t delta(u8 v, const ParamSpec& p, u8 g) const { return vars.at(v).at(MorphSpec{p, g}); }
    void change(u8 v, const ParamSpec& p, u8 g, std::int8_t d) { vars.at(v).at(MorphSpec{p, g}) = d; }

    // AddMorph @000674a8.
    void add(u8 v, const ParamSpec& p, u8 g, std::int8_t d)
    {
        if (auto it = vars.find(v); it != vars.end())
            it->second.emplace(MorphSpec{p, g}, d);
    }

    // RemoveParam @00066836: the parameter's morphs removed, those of the
    // module's higher parameters moved down by one.
    void removeParam(const ParamSpec& spec)
    {
        for (auto& [v, morphs] : vars) {
            std::map<MorphSpec, std::int8_t> moved;
            for (auto it = morphs.begin(); it != morphs.end();) {
                const auto& k = it->first;
                if (k.p.ctx == spec.ctx && k.p.module == spec.module && spec.param <= k.p.param) {
                    if (spec.param < k.p.param)
                        moved[MorphSpec{ParamSpec{k.p.ctx, k.p.module, static_cast<u8>(k.p.param - 1)}, k.group}] = it->second;
                    it = morphs.erase(it);
                    continue;
                }
                ++it;
            }
            for (const auto& [k, d] : moved)
                morphs[k] = d;
        }
    }
};

// NSFile_V7::CMap<Key, Item>: a map whose items are unique too.
template <class Key, class Item>
struct UniqueMap {
    std::map<Key, Item> map;

    // FindItemFromKey.
    std::optional<Item> find(const Key& k) const
    {
        auto it = map.find(k);
        return it == map.end() ? std::nullopt : std::optional<Item>(it->second);
    }
    // AddItem (@001b0822, @001ae244): refused if the item is used or the key exists.
    bool add(const Key& k, Item item)
    {
        for (const auto& [key, i] : map)
            if (i == item)
                return false;
        return map.emplace(k, item).second;
    }
    // RemoveItem: the first key (in key order) holding the item.
    void removeItem(Item item)
    {
        for (auto it = map.begin(); it != map.end(); ++it)
            if (it->second == item) {
                map.erase(it);
                return;
            }
    }
};

// NSFile_V11::CKnobMapData_11 (type 0: a patch's knobs, type 1: a
// performance's global knobs).
struct KnobData : UniqueMap<BarbSpec, std::uint16_t> {
    // RemoveParam @00067b88 (patch) and @00067c8c (one slot of the global
    // knobs): the parameter's knob freed, the module's higher parameters'
    // knobs moved down by one parameter. A moved target gets device type 0
    // (and in a patch slot 4: CBarbTargetSpec(CParamSpec) @0002427a).
    void removeParam(const ParamSpec& spec, std::optional<u8> slot = std::nullopt)
    {
        std::map<BarbSpec, std::uint16_t> moved;
        for (auto it = map.begin(); it != map.end();) {
            const auto& k = it->first;
            if ((!slot || k.slot == *slot) && k.p.ctx == spec.ctx && k.p.module == spec.module
                && spec.param <= k.p.param) {
                if (spec.param < k.p.param)
                    moved[BarbSpec{ParamSpec{k.p.ctx, k.p.module, static_cast<u8>(k.p.param - 1)}, 0,
                                   slot ? *slot : u8{4}}] = it->second;
                it = map.erase(it);
                continue;
            }
            ++it;
        }
        for (const auto& [k, knob] : moved)
            map[k] = knob;
    }
};

// NSFile_V11::CCtrlMapData_11: MIDI controller per parameter.
struct CtrlData : UniqueMap<ParamSpec, u8> {
    // RemoveParam @00066746, like the knobs'.
    void removeParam(const ParamSpec& spec)
    {
        std::map<ParamSpec, u8> moved;
        for (auto it = map.begin(); it != map.end();) {
            const auto& k = it->first;
            if (k.ctx == spec.ctx && k.module == spec.module && spec.param <= k.param) {
                if (spec.param < k.param)
                    moved[ParamSpec{k.ctx, k.module, static_cast<u8>(k.param - 1)}] = it->second;
                it = map.erase(it);
                continue;
            }
            ++it;
        }
        for (const auto& [k, cc] : moved)
            map[k] = cc;
    }
};

// NSFile_V11::CModuleCustomData_11: per module, [kind, len, payload] records.
struct CustomData {
    int ctx = 0;
    std::map<u8, std::vector<u8>> map;

    // operator= @000698ca (CopyMap @000697f0, AddCustomData @00065d24): a
    // module without data is not copied.
    void dropEmpty()
    {
        for (auto it = map.begin(); it != map.end();)
            it = it->second.empty() ? map.erase(it) : std::next(it);
    }

    // RemoveParam @00065012: the parameter's label record removed, the
    // module's higher labels renumbered.
    void removeParam(const ParamSpec& spec)
    {
        auto it = map.find(spec.module);
        if (it == map.end())
            return;
        const auto& in = it->second;
        std::vector<u8> out;
        std::size_t i = 0;
        // ReadCustomUBYTE @00023a4e reads on past the end of the data; a
        // record cut short ends the rewrite here.
        auto next = [&](u8& b) {
            if (i >= in.size())
                return false;
            b = in[i++];
            return true;
        };
        u8 kind = 0, len = 0, b = 0;
        while (i < in.size()) {
            if (!next(kind) || !next(len))
                break;
            if (kind == 1) {
                u8 p = 0;
                if (!next(p))
                    break;
                if (p == spec.param) {
                    for (int k = 1; k < len; ++k)
                        if (!next(b))
                            break;
                } else {
                    out.push_back(1);
                    out.push_back(len);
                    out.push_back(spec.param <= p ? static_cast<u8>(p - 1) : p);
                    for (int k = 1; k < len && next(b); ++k)
                        out.push_back(b);
                }
            } else {
                out.push_back(kind);
                out.push_back(len);
                for (int k = 0; k < len && next(b); ++k)
                    out.push_back(b);
            }
        }
        it->second = std::move(out);
    }
};

struct NameData {
    int ctx = 0;
    std::map<u8, std::string> map;
};

// CPatchFile_13 (and _12, which has no settings custom data).
struct PatchData {
    PatchHeader header;
    ModuleData va, fx;
    CurrentNotes voices;
    CableData vaCables, fxCables;
    ParamData settings, vaParams, fxParams;
    MorphData morph;
    KnobData knobs;
    CtrlData ctrls;
    CustomData settingsCustom, vaCustom, fxCustom;
    NameData vaNames, fxNames;
    std::string text;
};

// CPerformanceData: the slot being converted and the performance's global
// knob map, passed to the 20->21 and 21->22 steps.
struct PerfRef {
    u8 slot = 0;
    KnobData* globalKnobs = nullptr;
};

// ---- reading (the NSFile_V11 ReadStream functions) ------------------------------

int clampContext(u8 raw) { return std::min<int>(raw, 2); } // gLimitCnt clamps

template <class T>
const T& expect(const Section& s, u8 id)
{
    if (s.id != id)
        throw FormatError("unexpected section order");
    const T* p = std::get_if<T>(&s.payload);
    if (!p)
        throw FormatError("section could not be decoded");
    return *p;
}

// CPatchHeaderData_11::ReadStream @000664ae, as WriteStream @00065564 writes it back.
PatchHeader readHeader(const PatchHeader& h)
{
    PatchHeader out;
    out.voiceCount = static_cast<u8>(h.voiceCount - 1u < 0x20 ? h.voiceCount : 1);
    out.splitterPos = std::min<std::uint16_t>(h.splitterPos, 0x3FFF);
    out.octaveShift = static_cast<std::int8_t>(h.octaveShift + 2 < 5 ? h.octaveShift : 0);
    out.cablesVisible = h.cablesVisible;
    out.monoMode = h.monoMode;
    out.activeVariation = h.activeVariation;
    out.category = h.category < 0x10 ? h.category : 0;
    return out;
}

// CModuleData_11::ReadStream @0006774a.
ModuleData readModules(const ModuleList& l)
{
    ModuleData d;
    d.ctx = clampContext(l.location);
    for (const auto& m : l.modules)
        d.list.push_back({m.type, m.index, m.col, m.row, m.color, m.uprate, m.modes, m.locked});
    return d;
}

// CCableData_11::ReadStream @00065eb8.
CableData readCables(const CableList& l)
{
    CableData d;
    d.ctx = clampContext(l.location);
    for (const auto& c : l.cables)
        d.list.push_back({std::min<u8>(c.color, 6), {c.fromModule, c.fromConn, c.fromIsOutput != 0},
                          {c.toModule, c.toConn, false}});
    return d;
}

// CModuleParamData_11::ReadStream @00068620.
ParamData readParams(const ParamList& l)
{
    ParamData d;
    d.ctx = clampContext(l.location);
    for (const auto& m : l.modules) {
        if (!d.modules.count(m.index))
            d.addModule(m.index, m.paramCount);
        for (const auto& v : m.variations) {
            if (!d.hasSetting(v.variation))
                d.addSetting(v.variation);
            for (std::size_t p = 0; p < v.values.size(); ++p)
                d.set(m.index, v.variation, static_cast<u8>(p), v.values[p]);
        }
    }
    return d;
}

// CMorphMapData_11::ReadStream @00069326.
MorphData readMorph(const MorphMap& m)
{
    MorphData d;
    for (std::size_t g = 0; g < m.keyboardAssign.size() && g < 8; ++g)
        d.keyboard[g] = m.keyboardAssign[g] == 1 ? 1 : (m.keyboardAssign[g] == 2 ? 2 : 0);
    for (const auto& v : m.variations) {
        if (!d.hasSetting(v.variation))
            d.addSetting(v.variation);
        for (const auto& a : v.morphs) {
            const ParamSpec p{clampContext(a.location), a.module, a.param};
            const u8 group = std::min<u8>(a.morph, 8); // CRangeABC<0,8>::AssignFromType @0019ec02
            if (!d.isMorphed(v.variation, p, group))
                d.add(v.variation, p, group, a.range);
        }
    }
    return d;
}

// CKnobMapData_11::ReadStream @00069ef8.
KnobData readKnobs(const KnobMap& m, bool global)
{
    KnobData d;
    for (std::size_t k = 0; k < m.knobs.size() && k < 120; ++k)
        if (const auto& a = m.knobs[k]) {
            BarbSpec t{ParamSpec{clampContext(a->location), a->module, a->param}, std::min<u8>(a->assignType, 1),
                       global ? std::min<u8>(a->slot, 4) : u8{4}};
            d.add(t, static_cast<std::uint16_t>(k));
        }
    return d;
}

// CCtrlMapData_11::ReadStream @0006a0f6.
CtrlData readCtrls(const CtrlMap& m)
{
    CtrlData d;
    for (const auto& c : m.controllers)
        d.add(ParamSpec{clampContext(c.location), c.module, c.param}, c.cc);
    return d;
}

// CModuleCustomData_11::ReadStream @00066b3a (a module listed twice keeps the last data).
CustomData readCustom(const file::CustomData& c)
{
    CustomData d;
    d.ctx = clampContext(c.location);
    for (const auto& m : c.modules)
        d.map[m.index] = m.bytes;
    return d;
}

// CModuleNameData_11::ReadStream @00068b3e.
NameData readNames(const ModuleNames& n)
{
    NameData d;
    d.ctx = clampContext(n.location);
    for (const auto& m : n.names)
        d.map[m.index] = m.name;
    return d;
}

// CTextpadData_11::ReadStream @00068d80 (GetString stops at a NUL).
std::string readText(const Textpad& t)
{
    return t.text.substr(0, t.text.find('\0'));
}

// CPatchFile_13::ReadMolecules @00052d3c; version 12 has no settings custom
// data (CPatchFile_11::ReadMolecules @0005196c).
PatchData readPatch(std::span<const Section> s, std::uint16_t version)
{
    const bool v12 = version < 13;
    if (s.size() != (v12 ? 17u : 18u))
        throw FormatError(v12 ? "a version 12 patch has 17 sections" : "a patch has 18 sections");
    PatchData p;
    p.header = readHeader(expect<PatchHeader>(s[0], kPatchHeader));
    p.va = readModules(expect<ModuleList>(s[1], kModuleList));
    p.fx = readModules(expect<ModuleList>(s[2], kModuleList));
    p.voices = expect<CurrentNotes>(s[3], kCurrentNotes);
    p.vaCables = readCables(expect<CableList>(s[4], kCableList));
    p.fxCables = readCables(expect<CableList>(s[5], kCableList));
    p.settings = readParams(expect<ParamList>(s[6], kParamList));
    p.vaParams = readParams(expect<ParamList>(s[7], kParamList));
    p.fxParams = readParams(expect<ParamList>(s[8], kParamList));
    p.morph = readMorph(expect<MorphMap>(s[9], kMorphMap));
    p.knobs = readKnobs(expect<KnobMap>(s[10], kKnobMap), false);
    p.ctrls = readCtrls(expect<CtrlMap>(s[11], kCtrlMap));
    std::size_t i = 12;
    if (v12) {
        // ConvertPatch12to13 copies these into the VA and FX custom data;
        // the settings custom data stays empty (location 2).
        p.settingsCustom.ctx = 2;
    } else {
        p.settingsCustom = readCustom(expect<file::CustomData>(s[i++], kCustomData));
    }
    p.vaCustom = readCustom(expect<file::CustomData>(s[i++], kCustomData));
    p.fxCustom = readCustom(expect<file::CustomData>(s[i++], kCustomData));
    if (!v12 && (p.settingsCustom.ctx != 2 || p.vaCustom.ctx != 1 || p.fxCustom.ctx != 0))
        throw FormatError("File corrupt."); // CPatchFile_13::ReadMolecules
    p.vaNames = readNames(expect<ModuleNames>(s[i++], kModuleNames));
    p.fxNames = readNames(expect<ModuleNames>(s[i++], kModuleNames));
    p.text = readText(expect<Textpad>(s[i], kTextpad));
    return p;
}

// CPerformanceHeader_11::ReadStream @00068e38, as WriteStream @00064254 writes it back.
PerfHeader readPerfHeader(const PerfHeader& h)
{
    PerfHeader out;
    out.unknown08 = std::min<u8>(h.unknown08, 15);
    out.focusedSlot = std::min<u8>(h.focusedSlot, 3);
    out.globalPages = h.globalPages != 0;
    out.kbdRangeEnabled = h.kbdRangeEnabled != 0;
    out.masterClockBpm = h.masterClockBpm;
    out.unknown18 = h.unknown18 != 0;
    out.masterClockRun = std::min<u8>(h.masterClockRun, 1);
    for (std::size_t i = 0; i < out.slots.size(); ++i) {
        const auto& s = h.slots[i];
        auto& o = out.slots[i];
        o.patchName = s.patchName;
        o.enabled = s.enabled != 0;
        o.keyboard = s.keyboard != 0;
        o.hold = s.hold != 0;
        o.bank = std::min<u8>(s.bank, 31);
        o.program = std::min<u8>(s.program, 127);
        o.kbdRangeLower = s.kbdRangeLower;
        o.kbdRangeUpper = s.kbdRangeUpper;
        o.midiChannel = std::min<u8>(s.midiChannel, 16);
    }
    return out;
}

// ---- writing (the NSFile_V11 WriteStream functions) -----------------------------

Section section(u8 id, Payload p) { return Section{id, std::move(p), 0, {}}; }

ModuleList writeModules(const ModuleData& d)
{
    ModuleList l;
    l.location = static_cast<u8>(d.ctx);
    for (const auto& m : d.list)
        l.modules.push_back({m.type, m.index, m.col, m.row, m.color, static_cast<u8>(m.bandwidth == 1), m.locked, 0,
                             m.selectors});
    return l;
}

CableList writeCables(const CableData& d)
{
    CableList l;
    l.location = static_cast<u8>(d.ctx);
    for (const auto& c : d.list) {
        // WriteStream @00064d9a puts the output end first.
        const bool swap = !c.a.output && c.b.output;
        const auto& from = swap ? c.b : c.a;
        const auto& to = swap ? c.a : c.b;
        l.cables.push_back({c.color, from.module, from.conn, static_cast<u8>(from.output), to.module, to.conn});
    }
    return l;
}

ParamList writeParams(ParamData& d)
{
    ParamList l;
    l.location = static_cast<u8>(d.ctx);
    l.variationCount = d.settingCount;
    for (auto& [id, m] : d.modules) {
        if (m.count == 0)
            continue;
        ParamModule pm;
        pm.index = id;
        pm.paramCount = m.count;
        for (u8 v : d.settings) {
            ParamVariation pv;
            pv.variation = v;
            for (u8 p = 0; p < m.count; ++p)
                pv.values.push_back(d.get(id, v, p));
            pm.variations.push_back(std::move(pv));
        }
        l.modules.push_back(std::move(pm));
    }
    return l;
}

MorphMap writeMorph(const MorphData& d)
{
    MorphMap m;
    m.morphCount = 8;
    m.keyboardAssign.assign(d.keyboard.begin(), d.keyboard.end());
    for (const auto& [v, morphs] : d.vars) {
        file::MorphVariation mv;
        mv.variation = v;
        mv.legacyDials.assign(8, 0);
        for (const auto& [k, range] : morphs)
            mv.morphs.push_back({static_cast<u8>(k.p.ctx), k.p.module, k.p.param, k.group, range});
        m.variations.push_back(std::move(mv));
    }
    return m;
}

// WriteStream @00069642: 120 knobs; a target on module 0 is written as unassigned.
KnobMap writeKnobs(const KnobData& d)
{
    std::vector<BarbSpec> targets(120, BarbSpec{ParamSpec{1, 0, 0}, 0, 4});
    for (const auto& [k, knob] : d.map)
        if (knob < 120)
            targets[knob] = k;
    KnobMap m;
    for (const auto& t : targets) {
        if (t.p.module == 0) {
            m.knobs.emplace_back();
            continue;
        }
        m.knobs.push_back(KnobAssign{static_cast<u8>(t.p.ctx), t.p.module, static_cast<u8>(t.device), t.p.param, t.slot});
    }
    return m;
}

CtrlMap writeCtrls(const CtrlData& d)
{
    CtrlMap m;
    for (const auto& [k, cc] : d.map)
        m.controllers.push_back({cc, static_cast<u8>(k.ctx), k.module, k.param});
    return m;
}

file::CustomData writeCustom(const CustomData& d)
{
    file::CustomData c;
    c.location = static_cast<u8>(d.ctx);
    for (const auto& [id, bytes] : d.map)
        c.modules.push_back({id, bytes});
    return c;
}

ModuleNames writeNames(const NameData& d)
{
    ModuleNames n;
    n.location = static_cast<u8>(d.ctx);
    for (const auto& [id, name] : d.map)
        n.names.push_back({id, name});
    return n;
}

std::vector<Section> writePatch(PatchData& p)
{
    std::vector<Section> s;
    s.push_back(section(kPatchHeader, p.header));
    s.push_back(section(kModuleList, writeModules(p.va)));
    s.push_back(section(kModuleList, writeModules(p.fx)));
    s.push_back(section(kCurrentNotes, p.voices));
    s.push_back(section(kCableList, writeCables(p.vaCables)));
    s.push_back(section(kCableList, writeCables(p.fxCables)));
    s.push_back(section(kParamList, writeParams(p.settings)));
    s.push_back(section(kParamList, writeParams(p.vaParams)));
    s.push_back(section(kParamList, writeParams(p.fxParams)));
    s.push_back(section(kMorphMap, writeMorph(p.morph)));
    s.push_back(section(kKnobMap, writeKnobs(p.knobs)));
    s.push_back(section(kCtrlMap, writeCtrls(p.ctrls)));
    s.push_back(section(kCustomData, writeCustom(p.settingsCustom)));
    s.push_back(section(kCustomData, writeCustom(p.vaCustom)));
    s.push_back(section(kCustomData, writeCustom(p.fxCustom)));
    s.push_back(section(kModuleNames, writeNames(p.vaNames)));
    s.push_back(section(kModuleNames, writeNames(p.fxNames)));
    // CTextpadData_11::WriteStream @00064632 writes at most 1024 bytes.
    s.push_back(section(kTextpad, Textpad{p.text.substr(0, 1024)}));
    return s;
}

// ---- the conversion steps -----------------------------------------------------------

// The area a module update works on: its custom data and parameters (the
// patch settings' for the seven settings pseudo-modules).
struct Target {
    u8 type = 0, id = 0;
    CustomData& custom;
    ParamData& params;
    std::vector<u8>* selectors = nullptr; // the module's selector values (21->22)
};

ParamSpec spec(const Target& t, u8 param) { return ParamSpec{t.params.ctx, t.id, param}; }

// RemoveParam (@000486a0, @000496d4, @0004b146 and the inline copies): the
// parameter removed from the custom data, values, morphs, knobs and controllers.
void removeParam(PatchData& p, Target& t, u8 param, const PerfRef* perf = nullptr)
{
    const ParamSpec s = spec(t, param);
    t.custom.removeParam(s);
    t.params.removeParam(t.id, param);
    p.morph.removeParam(s);
    p.knobs.removeParam(s);
    p.ctrls.removeParam(s);
    // RemoveParam @0004ca0c also frees the slot's global knob.
    if (perf && perf->globalKnobs)
        perf->globalKnobs->removeParam(s, perf->slot);
}

// The morph-range rule shared by HandleCubifyParam @000497f8 and ScaleParam
// @0004e470: the morph's end value, clamped to 0..127.
u8 morphEnd(u8 value, std::int8_t delta)
{
    return static_cast<u8>(std::clamp(int{delta} + int{value}, 0, 127));
}

// _gConvertTable @001dcce0, used by HandleCubifyParam.
constexpr std::array<u8, 128> kCubify{
    0,   2,   2,   2,   3,   3,   3,   4,   4,   5,   5,   6,   6,   7,   8,   8,   9,   9,   10,  11,  11,  12,
    13,  14,  14,  15,  16,  16,  17,  18,  19,  20,  20,  21,  22,  23,  24,  24,  25,  26,  27,  28,  29,  30,
    31,  32,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  45,  46,  47,  48,  49,  50,  51,  52,
    53,  54,  56,  57,  58,  59,  60,  61,  63,  64,  65,  66,  67,  68,  70,  71,  72,  73,  75,  76,  77,  78,
    79,  81,  82,  83,  84,  86,  87,  88,  89,  91,  92,  93,  94,  96,  97,  98,  99,  100, 102, 103, 104, 105,
    107, 108, 109, 110, 112, 113, 114, 115, 116, 118, 119, 120, 121, 122, 123, 125, 126, 127};

// ScaleGlide(unsigned char)::convTable @001dd060.
constexpr std::array<u8, 128> kScaleGlide{
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  3,  3,  4,  5,  6,  6,  7,  8,  9,
    10, 10, 11, 12, 12, 13, 14, 15, 16, 16, 17, 18, 19, 19, 20, 21, 22, 22, 23, 24, 25, 25, 26, 27, 28, 28,
    29, 30, 30, 31, 32, 33, 33, 34, 35, 36, 37, 37, 38, 39, 40, 40, 41, 42, 43, 43, 44, 45, 45, 46, 47, 48,
    48, 49, 50, 51, 51, 52, 53, 54, 54, 55, 56, 57, 57, 58, 59, 60, 61, 61, 62, 63, 64, 65, 65, 66, 67, 68,
    68, 69, 70, 71, 71, 72, 73, 73, 74, 75, 76, 76, 77, 78, 79, 80, 81, 81, 82, 83, 84, 85, 85, 87};

// HandleCubifyParam @000497f8: the value through _gConvertTable, each morph
// range so that the morph's end moves the same way.
void cubify(PatchData& p, Target& t, u8 v, u8 param)
{
    const ParamSpec s = spec(t, param);
    const u8 value = t.params.get(t.id, v, param);
    const u8 cubed = kCubify[value & 0x7F];
    t.params.set(t.id, v, param, cubed);
    for (u8 g = 0; g < 8; ++g)
        if (p.morph.isMorphed(v, s, g)) {
            const u8 end = morphEnd(value, p.morph.delta(v, s, g));
            p.morph.change(v, s, g, static_cast<std::int8_t>(kCubify[end] - cubed));
        }
}

// CubifyParam @000499e8: in every variation.
void cubifyParam(PatchData& p, Target& t, u8 param)
{
    for (u8 v : t.params.settings)
        cubify(p, t, v, param);
}

// CubifyParamIfNot1 @0004994a: in the variations where parameter `unless`
// is not 1.
void cubifyParamIfNot1(PatchData& p, Target& t, u8 param, u8 unless)
{
    for (u8 v : t.params.settings)
        if (t.params.get(t.id, v, unless) != 1)
            cubify(p, t, v, param);
}

// ScaleParam @0004e470: the value through `fn`, each morph range so that the
// morph's end moves the same way; a range that would become 0 stays ±1.
template <class F>
void scaleParam(PatchData& p, Target& t, u8 v, u8 param, F fn)
{
    const ParamSpec s = spec(t, param);
    const u8 value = t.params.get(t.id, v, param);
    const u8 scaled = fn(value);
    t.params.set(t.id, v, param, scaled);
    for (u8 g = 0; g < 8; ++g)
        if (p.morph.isMorphed(v, s, g)) {
            const std::int8_t d = p.morph.delta(v, s, g);
            auto nd = static_cast<std::int8_t>(fn(morphEnd(value, d)) - scaled);
            if (nd == 0)
                nd = d > 0 ? 1 : -1;
            p.morph.change(v, s, g, nd);
        }
}

// The remap of an LFO/rate-like parameter 0 (15->16 type 87, 19->20 type 134):
// values 0..100 move down by 5 (not below 0), 101..110 by 3.
void shiftRate(Target& t)
{
    for (u8 v : t.params.settings) {
        int x = t.params.get(t.id, v, 0);
        if (x < 0x65)
            x = std::max(x - 5, 0);
        else if (x < 0x6F)
            x = std::max(x - 3, 0);
        t.params.set(t.id, v, 0, static_cast<u8>(x));
    }
}

// The 12->13 per-module update, UpdateModule @00047fa6.
void update12to13(PatchData& p, Target& t, const PerfRef*)
{
    if (t.type == 0x58)
        removeParam(p, t, 0);
    else if (t.type == 0x8A)
        t.params.addParam(t.id, 2, 0x40);
}

// UpdateModule @00048780.
void update13to14(PatchData& p, Target& t, const PerfRef*)
{
    switch (t.type) {
    case 0x66:
        removeParam(p, t, 9);
        removeParam(p, t, 1);
        removeParam(p, t, 0);
        t.params.addParam(t.id, 8, 0);
        t.params.addParam(t.id, 9, 0);
        t.params.addParam(t.id, 10, 0);
        break;
    case 0x03:
    case 0x04:
        t.params.addParam(t.id, 2, 0);
        break;
    case 0x7B:
        t.params.addParam(t.id, 9, 0);
        break;
    case 0xC3:
        t.params.addParam(t.id, 3, 1);
        break;
    default:
        break;
    }
}

// UpdateModule @00048f9c: InvertParam @00048efa (0 <-> nonzero) on one
// parameter per type.
void update14to15(PatchData&, Target& t, const PerfRef*)
{
    int param = -1;
    switch (t.type) {
    case 0x03: case 0x04: case 0x1F: case 0x52: case 0x5F: case 0x7F: case 0xAA: case 0xAB:
        param = 1; break;
    case 0x3E: case 0x4A: case 0x5E: case 0x7D:
        param = 2; break;
    case 0x0C: case 0x20: case 0x3D: case 0x57: case 0x59: case 0x67: case 0x86: case 0xBD: case 0xC0:
        param = 3; break;
    case 0x18: case 0x19: case 0x60: case 0xB0:
        param = 4; break;
    case 0x09: case 0x1A: case 0x21: case 0x61: case 0x76: case 0xB7:
        param = 5; break;
    case 0x31: case 0x33: case 0x66: case 0x96:
        param = 6; break;
    case 0x0D: case 0xA2: case 0xBE:
        param = 7; break;
    case 0x08: case 0xB1: case 0xC4:
        param = 8; break;
    case 0x07: case 0x2D: case 0xB6:
        param = 9; break;
    case 0xA3: case 0xA4:
        param = 10; break;
    case 0x3A:
        param = 15; break;
    default:
        return;
    }
    const u8 pi = static_cast<u8>(param);
    for (u8 v : t.params.settings)
        t.params.set(t.id, v, pi, t.params.get(t.id, v, pi) == 0 ? 1 : 0);
}

// UpdateModule @00049a50: levels and times moved to the cubic curve.
void update15to16(PatchData& p, Target& t, const PerfRef*)
{
    auto cube = [&](std::initializer_list<u8> params) {
        for (u8 q : params)
            cubifyParam(p, t, q);
    };
    auto cubeUnless1 = [&](std::initializer_list<u8> params, u8 unless) {
        for (u8 q : params)
            cubifyParamIfNot1(p, t, q, unless);
    };
    switch (t.type) {
    case 0x07: case 0x08: case 0x0D: case 0xA3: cube({3, 5}); break;
    case 0x09: cube({7, 4}); break;
    case 0x13: cubeUnless1({0, 1, 2, 3}, 4); break;
    case 0x19: case 0x1A: case 0x2D: case 0x61: case 0xA4: cube({3}); break;
    case 0x1B: case 0x21: case 0x67: case 0xC4: cube({4}); break;
    case 0x20: cube({2}); break;
    case 0x28: cubeUnless1({0, 1, 2, 3, 4, 5, 6, 7}, 8); break;
    case 0x30: cube({0, 1, 2, 3, 4, 5}); break;
    case 0x3A: cube({4, 5, 13, 14, 11}); break;
    case 0x42: cubeUnless1({1, 3}, 4); break;
    case 0x57: shiftRate(t); break;
    case 0x5F: cube({0}); break;
    case 0x66: cube({7}); break;
    case 0x6C: removeParam(p, t, 0x10); break;
    case 0x7B: cubeUnless1({0, 1, 2, 3}, 9); break;
    case 0x8C: cube({0, 1, 2, 3}); break;
    case 0xA1: cubeUnless1({0, 1, 2, 3, 4, 5, 6, 7}, 0x10); break;
    case 0xA2: cube({1, 6}); break;
    case 0xB1: removeParam(p, t, 5); break;
    case 0xB7: cube({4, 6}); break;
    case 0xB8: case 0xB9: case 0xC3: cubeUnless1({0}, 2); break;
    case 0xBE: cube({1}); break;
    case 0xC2: cubeUnless1({0, 2}, 4); break;
    default: break;
    }
}

// UpdateModule @0004a7b6: TruncParam @0004a716 limits one parameter; type 94
// halves parameter 0 and its morph ranges.
void update16to17(PatchData& p, Target& t, const PerfRef*)
{
    auto trunc = [&](u8 param, u8 max) {
        for (u8 v : t.params.settings)
            if (t.params.get(t.id, v, param) > max)
                t.params.set(t.id, v, param, max);
    };
    switch (t.type) {
    case 0x9E: removeParam(p, t, 2); break;
    case 0x18: trunc(3, 3); break;
    case 0x19: trunc(1, 3); break;
    case 0x1A: trunc(7, 3); break;
    case 0xBE: trunc(2, 3); break;
    case 0xAF: trunc(8, 1); break;
    case 0xB0: trunc(5, 1); break;
    case 0xB1: trunc(4, 1); break;
    case 0xB6: trunc(6, 1); break;
    case 0x5E: {
        const ParamSpec s = spec(t, 0);
        for (u8 v : t.params.settings) {
            t.params.set(t.id, v, 0, static_cast<u8>((t.params.get(t.id, v, 0) + 1) >> 1));
            for (u8 g = 0; g < 8; ++g)
                if (p.morph.isMorphed(v, s, g)) {
                    const int d = p.morph.delta(v, s, g);
                    p.morph.change(v, s, g, static_cast<std::int8_t>((d < 1 ? d - 1 : d + 1) / 2));
                }
        }
        break;
    }
    default: break;
    }
}

// UpdateModule @0004b226.
void update17to18(PatchData& p, Target& t, const PerfRef*)
{
    if (t.type == 0x76) {
        removeParam(p, t, 4);
        removeParam(p, t, 3);
    } else if (t.type == 0xC6) {
        removeParam(p, t, 0);
    }
}

// UpdateModule @0004bb60: ZeroParam @0004b8f8 and LimitParam @0004b9d6.
void update18to19(PatchData& p, Target& t, const PerfRef*)
{
    auto zero = [&](u8 param) {
        for (u8 v : t.params.settings)
            t.params.set(t.id, v, param, 0);
    };
    switch (t.type) {
    case 0xAF: zero(8); break;
    case 0xB0: zero(5); break;
    case 0xB1: zero(4); break;
    case 0xB6: zero(6); break;
    case 0x03:
    case 0x04: {
        // LimitParam(param 2, max 1): the value and its morph ranges limited.
        const u8 max = 1;
        const ParamSpec s = spec(t, 2);
        for (u8 v : t.params.settings) {
            if (t.params.get(t.id, v, 2) <= max)
                continue;
            t.params.set(t.id, v, 2, max);
            for (u8 g = 0; g < 8; ++g)
                if (p.morph.isMorphed(v, s, g)) {
                    const int d = p.morph.delta(v, s, g);
                    if (d > max)
                        p.morph.change(v, s, g, static_cast<std::int8_t>(max));
                    else if (d < -int{max})
                        p.morph.change(v, s, g, static_cast<std::int8_t>(-int{max}));
                }
        }
        break;
    }
    default: break;
    }
}

// UpdateModule @0004c2ec.
void update19to20(PatchData&, Target& t, const PerfRef*)
{
    if (t.type == 0x86)
        shiftRate(t);
}

// SwapParamsWithoutMorph @0004cc10: two parameters' values swapped in every
// variation, and their knobs, controllers and global knobs
// (CPerformanceData::SwapParam @001ab0dc); morphs stay.
void swapParams(PatchData& p, Target& t, u8 a, u8 b, const PerfRef* perf)
{
    for (u8 v : t.params.settings) {
        const u8 va = t.params.get(t.id, v, a);
        const u8 vb = t.params.get(t.id, v, b);
        t.params.set(t.id, v, a, vb);
        t.params.set(t.id, v, b, va);
    }
    const ParamSpec sa = spec(t, a), sb = spec(t, b);
    const auto ka = p.knobs.find(BarbSpec{sa}); // FindKnobFromParam: device 0, slot 4
    if (ka)
        p.knobs.removeItem(*ka);
    const auto kb = p.knobs.find(BarbSpec{sb});
    if (kb)
        p.knobs.removeItem(*kb);
    if (ka)
        p.knobs.add(BarbSpec{sb}, *ka);
    if (kb)
        p.knobs.add(BarbSpec{sa}, *kb);
    const auto ca = p.ctrls.find(sa);
    if (ca)
        p.ctrls.removeItem(*ca);
    const auto cb = p.ctrls.find(sb);
    if (cb)
        p.ctrls.removeItem(*cb);
    if (ca)
        p.ctrls.add(sb, *ca);
    if (cb)
        p.ctrls.add(sa, *cb);
    if (perf && perf->globalKnobs) {
        auto& gk = *perf->globalKnobs;
        const BarbSpec ga{sa, 0, perf->slot}, gb{sb, 0, perf->slot};
        const auto k1 = gk.find(ga);
        if (k1)
            gk.removeItem(*k1);
        const auto k2 = gk.find(gb);
        if (k2)
            gk.removeItem(*k2);
        if (k1)
            gk.add(gb, *k1);
        if (k2)
            gk.add(ga, *k2);
    }
}

// UpdateModule @0004d066 (20->21).
void update20to21(PatchData& p, Target& t, const PerfRef* perf)
{
    auto unmorph = [&](std::initializer_list<u8> params) {
        for (u8 q : params)
            p.morph.removeParam(spec(t, q));
    };
    auto unctrl = [&](std::initializer_list<u8> params) {
        for (u8 q : params)
            p.ctrls.removeParam(spec(t, q));
    };
    switch (t.type) {
    case 0x06: // the morph settings: groups 4 and 5 swap (dials, then modes)
        swapParams(p, t, 4, 5, perf);
        swapParams(p, t, 12, 13, perf);
        break;
    case 0x62:
        removeParam(p, t, 3, perf);
        removeParam(p, t, 1, perf);
        t.params.addParam(t.id, 3, 1);
        break;
    case 0x8C: t.params.addParam(t.id, 8, 0); break;
    case 0x2B: unmorph({1}); break;
    case 0x32: unmorph({2}); break;
    case 0x54: unmorph({4}); break;
    case 0x8D:
        unctrl({1, 0});
        unmorph({2, 0});
        break;
    case 0x8E: unmorph({1}); break;
    case 0x8F: unmorph({2}); break;
    case 0x92: unmorph({0x22}); break;
    case 0x93:
        unctrl({0});
        unmorph({1, 0});
        break;
    case 0x94: unmorph({1, 0}); break;
    case 0x95: unmorph({5, 4, 3, 2, 1, 0}); break;
    case 0x98: unmorph({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}); break;
    case 0x9C: unmorph({0}); break;
    case 0x9D: unmorph({1}); break;
    case 0xBC: unmorph({2}); break;
    case 0xCA: removeParam(p, t, 8, perf); break;
    case 0xCE:
        removeParam(p, t, 5, perf);
        removeParam(p, t, 1, perf);
        break;
    default: break;
    }
}

// The LFO rate conversions of 21->22 (Convert_LFORate* @0004e1f0..@0004e27a),
// chosen by the rate mode (GetRandomConvertFunction @0004e29c).
u8 lfoRate(u8 mode, u8 x)
{
    switch (mode) {
    case 0: return x < 0x40 ? static_cast<u8>(x * 2 + 1) : 0x7F;  // Sub
    case 1:                                                        // Lo
    case 2: return x < 0x74 ? static_cast<u8>(x + 12) : 0x7F;      // Hi
    case 3:                                                        // BPM
        if (x < 0x0B) return static_cast<u8>(x * 2 + 12);
        if (x < 0x1B) return static_cast<u8>(x * 4 - 8);
        if (x < 0x21) return static_cast<u8>(x * 2 + 44);
        if (x < 0x34) return static_cast<u8>(x + 76);
        return 0x7F;
    case 4:                                                        // Clk
        if (x < 0x30) return static_cast<u8>(x + 8);
        if (x < 0x74) return static_cast<u8>(x + 12);
        return 0x7F;
    default: return x;
    }
}

// UpdateModule @0004e646 (21->22).
void update21to22(PatchData& p, Target& t, const PerfRef*)
{
    // The function pointer is not reset per variation: a rate mode above 4
    // keeps the previous variation's conversion (first time: none, here).
    std::optional<u8> mode;
    auto rate = [&](u8 v, u8 m) {
        if (m <= 4)
            mode = m;
        if (mode)
            scaleParam(p, t, v, 0, [&](u8 x) { return lfoRate(*mode, x); });
    };
    if (t.type == 0x1A) {
        for (u8 v : t.params.settings)
            if (static_cast<u8>(t.params.get(t.id, v, 4) - 4) < 2)
                rate(v, t.params.get(t.id, v, 7));
    } else if (t.type == 0x9E) {
        for (u8 v : t.params.settings)
            if (t.params.get(t.id, v, 2) == 1)
                scaleParam(p, t, v, 0, [](u8 x) { return kScaleGlide[x & 0x7F]; });
    } else if (t.type == 0x18 && t.selectors && !t.selectors->empty()
               && static_cast<u8>((*t.selectors)[0] - 4) < 2) {
        for (u8 v : t.params.settings)
            rate(v, t.params.get(t.id, v, 3));
    }
}

// GetDefaultMutationLock @0004f008, the list ConvertPatch22to23 @0004f09c
// sets the lock flag from.
bool defaultLocked(u8 type)
{
    switch (type) {
    case 0x03: case 0x04: case 0x11: case 0x26: case 0x2A: case 0x30: case 0x32: case 0x39: case 0x3B:
    case 0x3C: case 0x44: case 0x45: case 0x47: case 0x4B: case 0x4C: case 0x51: case 0x52: case 0x53:
    case 0x55: case 0x69: case 0x6C: case 0x70: case 0x73: case 0x7F: case 0x8D: case 0x8E: case 0x8F:
    case 0x93: case 0x94: case 0x95: case 0x96: case 0x9C: case 0x9D: case 0xAA: case 0xAB: case 0xB2:
    case 0xBC: case 0xBD: case 0xC6: case 0xC7: case 0xD0:
        return true;
    default:
        return false;
    }
}

using Update = void (*)(PatchData&, Target&, const PerfRef*);

// The seven patch-settings pseudo-modules, as each step updates them:
// (type, index) for morph, gain, glide, bend, vibrato, arpeggiator, misc.
constexpr std::array<std::pair<u8, u8>, 7> kSettings{{
    {0x06, 1}, {0x5F, 2}, {0x87, 3}, {0x89, 4}, {0x8A, 5}, {0x88, 6}, {0x99, 7},
}};

void updateSettings(PatchData& p, Update update, const PerfRef* perf)
{
    for (const auto& [type, id] : kSettings) {
        Target t{type, id, p.settingsCustom, p.settings};
        update(p, t, perf);
    }
}

// The steps' extra per-module work, after the module update.
enum class Extra { None, Cables16, Selectors17, Cables21AndSelectors21, Selector22 };

// ValidateCables @00049744 (15->16) and @0004cb22 (20->21).
void validateCables(u8 step, const ModuleItem& m, CableData& cables)
{
    auto input = [&](u8 conn) { cables.removeConnector({m.index, conn, false}); };
    auto output = [&](u8 conn) { cables.removeConnector({m.index, conn, true}); };
    if (step == 16) {
        if (m.type == 0xB1)
            input(1);
    } else if (m.type == 0x62) {
        output(4);
        output(3);
        output(0);
        input(2);
    } else if (m.type == 0xCE) {
        input(3);
    }
}

// ValidatePartSelectors @0004aaba (16->17) and @0004cbec (20->21), on the
// first selector value.
void validateSelectors(u8 step, ModuleItem& m)
{
    if (m.selectors.empty())
        return; // the original reads element 0 of an empty vector
    u8& s = m.selectors[0];
    if (step == 17) {
        if ((m.type == 0x09 || m.type == 0x60) && s > 1)
            s = s == 5 ? 2 : static_cast<u8>(s + 1);
    } else if (m.type == 0xCE && s > 1) {
        s = 1;
    }
}

// The modules of one area, in list order (both areas: VA, then FX).
void updateModules(PatchData& p, u8 step, Update update, const PerfRef* perf)
{
    struct AreaRef {
        ModuleData& modules;
        CustomData& custom;
        ParamData& params;
        CableData& cables;
    };
    for (AreaRef a : {AreaRef{p.va, p.vaCustom, p.vaParams, p.vaCables}, AreaRef{p.fx, p.fxCustom, p.fxParams, p.fxCables}})
        for (auto& m : a.modules.list) {
            Target t{m.type, m.index, a.custom, a.params, &m.selectors};
            update(p, t, perf);
            if (step == 16 || step == 21)
                validateCables(step, m, a.cables);
            if (step == 17 || step == 21)
                validateSelectors(step, m);
            if (step == 22 && m.type == 0x45)
                m.selectors.push_back(0); // ConvertPatch21to22: one more selector value
        }
}

// The copy every step starts with (CPatchFile_N+1 = CPatchFile_N, molecule by
// molecule): only the custom data and, in some steps, the morph map and the
// module lists change on the way.
void copyStep(PatchData& p)
{
    p.settingsCustom.dropEmpty();
    p.vaCustom.dropEmpty();
    p.fxCustom.dropEmpty();
}

// ConvertPatch20to21 @0004d910 rebuilds the morph map with groups 4 and 5
// swapped (also in the performance's global knobs, through UpdateModule's
// SwapParam). The keyboard morph assignments are not copied.
void swapMorphGroups(PatchData& p)
{
    MorphData out;
    for (const auto& [v, morphs] : p.morph.vars) {
        out.addSetting(v);
        for (const auto& [k, d] : morphs) {
            const u8 g = k.group == 4 ? 5 : (k.group == 5 ? 4 : k.group);
            out.add(v, k.p, g, d);
        }
    }
    p.morph = std::move(out);
}

// CopyModules @0004e350 (21->22): every module but the type-77 ones is
// copied with its name; a type-77 module's cables are removed (inputs 1, 0,
// outputs 2, 1, 0). Its parameters and custom data stay in the file.
void copyModules(ModuleData& modules, NameData& names, CableData& cables)
{
    ModuleData out;
    out.ctx = modules.ctx;
    NameData outNames;
    outNames.ctx = names.ctx;
    for (const auto& m : modules.list) {
        if (m.type == 0x4D) {
            cables.removeConnector({m.index, 1, false});
            cables.removeConnector({m.index, 0, false});
            cables.removeConnector({m.index, 2, true});
            cables.removeConnector({m.index, 1, true});
            cables.removeConnector({m.index, 0, true});
            continue;
        }
        out.list.push_back(m);
        if (auto n = names.map.find(m.index); n != names.map.end())
            outNames.map.emplace(n->first, n->second); // AddName @00065d98
    }
    modules = std::move(out);
    names = std::move(outNames);
}

// One step, version `from` to `from + 1`, on one patch.
void convertStep(PatchData& p, std::uint16_t from, const PerfRef* perf)
{
    copyStep(p);
    const auto to = static_cast<u8>(from + 1);
    if (from == 20)
        swapMorphGroups(p);
    if (from == 21) {
        copyModules(p.va, p.vaNames, p.vaCables);
        copyModules(p.fx, p.fxNames, p.fxCables);
    }
    // ConvertPatch14to15 ... 20to21 add the morph map's variation 8 (the
    // ninth, "init" variation) if it is missing.
    if (from >= 14 && from <= 20 && !p.morph.hasSetting(8))
        p.morph.addSetting(8);
    Update update = nullptr;
    switch (from) {
    case 12: update = update12to13; break; // ConvertPatch12to13 @000480c6
    case 13: update = update13to14; break; // @000488f0
    case 14: update = update14to15; break; // @000490a0
    case 15: update = update15to16; break; // @0004a042
    case 16: update = update16to17; break; // @0004aaf0
    case 17: update = update17to18; break; // @0004b2c4
    case 18: update = update18to19; break; // @0004bc48
    case 19: update = update19to20; break; // @0004c3d8
    case 20: update = update20to21; break; // @0004d910
    case 21: update = update21to22; break; // @0004e8ae
    case 22: break;                        // @0004f09c
    default: throw UnsupportedVersion();
    }
    if (update) {
        updateSettings(p, update, perf);
        updateModules(p, to, update, perf);
    } else {
        for (ModuleData* m : {&p.va, &p.fx})
            for (auto& item : m->list)
                item.locked = defaultLocked(item.type) ? 1 : 0;
    }
}

void checkVersion(std::uint16_t v)
{
    if (v < kOldestConvertible || v > kCurrentVersion)
        throw UnsupportedVersion();
}

} // namespace

std::uint16_t sourceVersion(const File& f)
{
    if (f.textHeader.size() >= 3 && !f.embeddedName) {
        const auto& line = f.textHeader[2];
        const auto eq = line.find('=');
        return static_cast<std::uint16_t>(eq == std::string::npos ? 0 : std::atoi(line.c_str() + eq + 1));
    }
    return f.version;
}

File toCurrent(const File& f)
{
    const auto version = sourceVersion(f);
    checkVersion(version);
    if (version == kCurrentVersion)
        return f;
    if (!f.unparsedTail.empty())
        throw FormatError("unexpected data at the end of the file");
    const std::size_t perPatch = version < 13 ? 17 : 18;

    File out;
    out.type = f.type;
    out.version = kCurrentVersion;
    out.textHeader = defaultTextHeader(f.type);
    std::span<const Section> s(f.sections);
    if (f.type == FileType::Patch) {
        PatchData p = readPatch(s, version);
        for (auto v = version; v < kCurrentVersion; ++v)
            convertStep(p, v, nullptr);
        out.sections = writePatch(p);
        return out;
    }
    if (s.size() != 2 + 4 * perPatch)
        throw FormatError("unexpected number of sections in a performance");
    const PerfHeader header = readPerfHeader(expect<PerfHeader>(s.front(), kPerfHeader));
    std::array<PatchData, 4> slots;
    for (std::size_t i = 0; i < 4; ++i)
        slots[i] = readPatch(s.subspan(1 + i * perPatch, perPatch), version);
    KnobData global = readKnobs(expect<KnobMap>(s.back(), kGlobalKnobMap), true);
    // CFileConverter::operator()(CPerformanceFile_N) (@00048586 ... @0004f45c):
    // the header and global knobs copied, each slot converted; 20->21 and
    // 21->22 get the slot and the global knobs.
    for (auto v = version; v < kCurrentVersion; ++v)
        for (std::size_t i = 0; i < 4; ++i) {
            const PerfRef perf{static_cast<u8>(i), &global};
            convertStep(slots[i], v, v == 20 || v == 21 ? &perf : nullptr);
        }
    out.sections.push_back(section(kPerfHeader, header));
    for (auto& p : slots) {
        auto ps = writePatch(p);
        out.sections.insert(out.sections.end(), std::make_move_iterator(ps.begin()), std::make_move_iterator(ps.end()));
    }
    out.sections.push_back(section(kGlobalKnobMap, writeKnobs(global)));
    return out;
}

} // namespace g2::convert
