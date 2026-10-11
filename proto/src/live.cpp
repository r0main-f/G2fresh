#include "g2/proto/live.hpp"

#include "g2/edit.hpp"
#include "g2/proto/state.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <type_traits>

namespace g2::proto {
namespace {

template <class... F>
struct Overloaded : F... {
    using F::operator()...;
};
template <class... F>
Overloaded(F...) -> Overloaded<F...>;

u8 locOf(Location loc) { return static_cast<u8>(loc); }

CableEdit cableEdit(u8 id, Location loc, const cables::Connector& from, const cables::Connector& to, CableColor color,
                    bool dump)
{
    CableEdit e;
    e.id = id;
    e.va = loc == Location::Va;
    e.color = static_cast<u8>(color);
    e.fromModule = from.module;
    e.fromConn = from.conn;
    e.fromIsOutput = from.isOutput;
    e.toModule = to.module;
    e.toConn = to.conn;
    e.toIsOutput = to.isOutput;
    e.dump = dump;
    return e;
}

// A cable's two ends, the output (or else the lower connector) first, and its colour.
using CableKey = std::tuple<u8, u8, bool, u8, u8, bool, u8>;
std::vector<CableKey> cableSet(const Area& area)
{
    std::vector<CableKey> out;
    for (const auto& c : area.cables) {
        auto a = std::make_tuple(c.fromModule, c.fromConn, c.fromIsOutput);
        auto b = std::make_tuple(c.toModule, c.toConn, false);
        if (!c.fromIsOutput && b < a)
            std::swap(a, b);
        out.emplace_back(std::get<0>(a), std::get<1>(a), std::get<2>(a), std::get<0>(b), std::get<1>(b), std::get<2>(b),
                         static_cast<u8>(c.color));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<u8> headerBytes(file::PatchHeader h)
{
    h.activeVariation = 0; // a variation change is a 6A (CPatchHeader::operator== @000ed05a ignores it)
    return file::encodeSection(file::Section{file::kPatchHeader, h, 0, {}});
}

using MorphKey = std::tuple<u8, u8, u8, u8>; // location, module, param, group
std::map<MorphKey, int> morphMap(const Patch& p, std::size_t variation)
{
    std::map<MorphKey, int> out;
    if (variation < p.morphs.size())
        for (const auto& a : p.morphs[variation].assigns)
            out.try_emplace({a.location, a.module, a.param, a.morph}, a.range); // the first of repeats, as the original reads them
    return out;
}

} // namespace

Molecule toMolecule(const live::Op& op)
{
    return std::visit(
        Overloaded{
            [](const live::Param& o) -> Molecule { return ParamChange{locOf(o.loc), o.module, o.param, o.value, o.variation}; },
            [](const live::Morph& o) -> Molecule {
                return MorphChange{locOf(o.loc), o.module, o.param, o.group, o.range, o.variation};
            },
            [](const live::Mode& o) -> Molecule { return ModeChange{locOf(o.loc), o.module, o.mode, o.value}; },
            [](const live::KnobAssign& o) -> Molecule {
                return KnobAssign{o.module, o.param, locOf(o.loc), o.assignType, static_cast<std::uint16_t>(o.knob)};
            },
            [](const live::KnobDeassign& o) -> Molecule { return KnobDeassign{id::KnobDeassign, static_cast<std::uint16_t>(o.knob)}; },
            [](const live::PageFocus& o) -> Molecule { return PageFocus{id::PageFocus, o.page}; },
            [](const live::CtrlAssign& o) -> Molecule { return CtrlAssign{locOf(o.loc), o.module, o.param, o.cc}; },
            [](const live::CtrlDeassign& o) -> Molecule { return CtrlDeassign{o.cc}; },
            [](const live::VariationSelect& o) -> Molecule { return VariationSelect{o.variation}; },
            [](const live::VariationCopy& o) -> Molecule { return VariationCopy{o.from, o.to}; },
            [](const live::ModuleNew& o) -> Molecule {
                const auto& m = o.module;
                return ModuleNew{m.type, locOf(o.loc), m.index, m.col, m.row, m.color, static_cast<u8>(m.uprate),
                                 static_cast<u8>(m.locked), m.modes, m.name};
            },
            [](const live::ModuleDelete& o) -> Molecule { return ModuleDelete{locOf(o.loc), o.module}; },
            [](const live::ModuleMove& o) -> Molecule { return ModuleMove{locOf(o.loc), o.module, o.col, o.row}; },
            [](const live::ModuleColor& o) -> Molecule { return ModuleRecolor{locOf(o.loc), o.module, o.color}; },
            [](const live::ModuleName& o) -> Molecule { return ModuleRename{locOf(o.loc), o.module, o.name}; },
            [](const live::Uprate& o) -> Molecule { return Uprate{locOf(o.loc), o.module, static_cast<u8>(o.uprate)}; },
            [](const live::CustomData& o) -> Molecule { return CustomData{locOf(o.loc), o.module, o.bytes}; },
            [](const live::MutaLock& o) -> Molecule { return MutaLock{locOf(o.loc), o.module, static_cast<u8>(o.locked)}; },
            [](const live::CableConnect& o) -> Molecule {
                return cableEdit(id::CableConnect, o.loc, o.from, o.to, o.color, o.dump);
            },
            [](const live::CableDelete& o) -> Molecule {
                return cableEdit(id::CableDelete, o.loc, o.from, o.to, CableColor::Red, o.dump);
            },
            [](const live::CableRecolor& o) -> Molecule {
                return cableEdit(id::CableRecolor, o.loc, o.from, o.to, o.color, false);
            },
            [](const live::Section& o) -> Molecule { return SectionDump{o.section}; },
        },
        op);
}

std::vector<Molecule> toMolecules(const std::vector<live::Op>& ops)
{
    std::vector<Molecule> out;
    out.reserve(ops.size());
    for (const auto& op : ops)
        out.push_back(toMolecule(op));
    return out;
}

LiveSlot::LiveSlot(Patch synth, std::string name) : synth_(std::move(synth)), name_(std::move(name)) {}

LiveSlot LiveSlot::uploaded(const Patch& patch, std::string name)
{
    try {
        return LiveSlot(Patch::fromSections(patch.toSections()), std::move(name));
    } catch (const std::exception&) {
        return LiveSlot(patch, std::move(name));
    }
}

void LiveSlot::apply(const Molecule& m)
{
    applyToPatch(synth_, m);
}

void LiveSlot::applyAll(const std::vector<Molecule>& molecules)
{
    for (const auto& m : molecules)
        apply(m);
}

std::vector<Outgoing> LiveSlot::update(const Patch& now, const std::string& name, const live::Edit* edit, bool undo,
                                       std::uint64_t ms)
{
    std::vector<Outgoing> out;
    // What the edit recorded, as the original's bubble (or realtime pair).
    if (edit && !edit->empty()) {
        auto molecules = toMolecules(undo ? edit->undo() : edit->redo());
        applyAll(molecules);
        if (edit->realtime)
            for (auto& m : molecules)
                out.push_back({true, {std::move(m)}});
        else if (!molecules.empty())
            out.push_back({false, std::move(molecules)});
    }

    // The rest, by comparison: one bubble for what is acknowledged...
    std::vector<Molecule> bubble;
    auto add = [&](Molecule m) {
        apply(m);
        bubble.push_back(std::move(m));
    };
    // ... realtime messages for values and morph ranges.
    std::vector<Molecule> realtime;
    auto addRealtime = [&](Molecule m) {
        apply(m);
        realtime.push_back(std::move(m));
    };

    // Modules and cables: what a recorded edit did not bring over needs the
    // whole patch.
    for (const auto loc : {Location::Va, Location::Fx}) {
        const auto& a = now.area(loc);
        const auto& b = synth_.area(loc);
        if (a.modules.size() != b.modules.size() || cableSet(a) != cableSet(b))
            needsUpload_ = true;
        for (const auto& m : a.modules) {
            const auto* s = b.find(m.index);
            if (!s || s->type != m.type)
                needsUpload_ = true;
        }
    }
    if (needsUpload_)
        return out;

    // The variations the synth has: the Mutator's (9) appears with the first
    // edit that uses it; send all of it then.
    if (edit::hasAuditionVariation(now) && !edit::hasAuditionVariation(synth_)) {
        edit::addAuditionVariation(synth_);
        constexpr auto v = static_cast<std::size_t>(kAuditionVariation);
        for (const auto loc : {Location::Va, Location::Fx})
            for (auto& m : synth_.area(loc).modules)
                if (v < m.params.size())
                    std::fill(m.params[v].begin(), m.params[v].end(), u8{0xFF});
        for (auto& m : synth_.settings)
            if (v < m.params.size())
                std::fill(m.params[v].begin(), m.params[v].end(), u8{0xFF});
        synth_.morphs[v].assigns.clear();
    }

    for (const auto loc : {Location::Va, Location::Fx}) {
        const u8 l = locOf(loc);
        for (const auto& m : now.area(loc).modules) {
            const Module s = *synth_.area(loc).find(m.index);
            if (s.col != m.col || s.row != m.row)
                add(ModuleMove{l, m.index, m.col, m.row});
            if (s.color != m.color)
                add(ModuleRecolor{l, m.index, m.color});
            if (s.name != m.name)
                add(ModuleRename{l, m.index, m.name});
            if (s.uprate != m.uprate)
                add(Uprate{l, m.index, static_cast<u8>(m.uprate)});
            if (s.locked != m.locked)
                add(MutaLock{l, m.index, static_cast<u8>(m.locked)});
            for (std::size_t i = 0; i < m.modes.size() && i < s.modes.size(); ++i)
                if (s.modes[i] != m.modes[i])
                    add(ModeChange{l, m.index, static_cast<u8>(i), m.modes[i]});
            if (s.customData.value_or(std::vector<u8>{}) != m.customData.value_or(std::vector<u8>{}))
                add(CustomData{l, m.index, m.customData.value_or(std::vector<u8>{})});
        }
    }
    // The patch settings' custom data (the morph groups' names).
    for (const auto& c : now.settingsCustomData) {
        const auto it = std::find_if(synth_.settingsCustomData.begin(), synth_.settingsCustomData.end(),
                                     [&](const file::CustomModule& x) { return x.index == c.index; });
        if (it == synth_.settingsCustomData.end() || it->bytes != c.bytes) {
            add(CustomData{2, c.index, c.bytes});
            if (it == synth_.settingsCustomData.end())
                synth_.settingsCustomData.push_back(c);
            else
                it->bytes = c.bytes;
        }
    }
    // Knobs: deassignments, then each new assignment (26 for the knob's
    // other target, 25), then the last one's page (2D).
    std::optional<int> lastKnob;
    for (int k = 0; k < kKnobCount; ++k) {
        const auto& a = now.knobs[static_cast<std::size_t>(k)];
        const auto& b = synth_.knobs[static_cast<std::size_t>(k)];
        if (b && !a)
            add(KnobDeassign{id::KnobDeassign, static_cast<std::uint16_t>(k)});
    }
    for (int k = 0; k < kKnobCount; ++k) {
        const auto& a = now.knobs[static_cast<std::size_t>(k)];
        const auto& b = synth_.knobs[static_cast<std::size_t>(k)];
        if (!a || (b && a->location == b->location && a->module == b->module && a->param == b->param))
            continue;
        if (b)
            add(KnobDeassign{id::KnobDeassign, static_cast<std::uint16_t>(k)});
        add(KnobAssign{a->module, a->param, a->location, a->assignType, static_cast<std::uint16_t>(k)});
        lastKnob = k;
    }
    if (lastKnob)
        bubble.push_back(PageFocus{id::PageFocus, static_cast<u8>(*lastKnob >> 3)});
    // MIDI controllers.
    std::map<u8, file::CtrlAssign> wasCc, isCc;
    for (const auto& c : synth_.controllers)
        wasCc[c.cc] = c;
    for (const auto& c : now.controllers)
        isCc[c.cc] = c;
    for (const auto& [cc, c] : wasCc)
        if (!isCc.count(cc))
            add(CtrlDeassign{cc});
    for (const auto& [cc, c] : isCc) {
        const auto it = wasCc.find(cc);
        if (it == wasCc.end() || it->second.location != c.location || it->second.module != c.module
            || it->second.param != c.param)
            add(CtrlAssign{c.location, c.module, c.param, cc});
    }

    // Values and morph ranges, every variation both have.
    const auto variations = std::min(edit::variationSlots(now), edit::variationSlots(synth_));
    for (const auto loc : {Location::Va, Location::Fx})
        for (const auto& m : now.area(loc).modules) {
            const auto* s = synth_.area(loc).find(m.index);
            for (std::size_t v = 0; v < variations && v < m.params.size() && v < s->params.size(); ++v)
                for (std::size_t i = 0; i < m.params[v].size() && i < s->params[v].size(); ++i)
                    if (m.params[v][i] != s->params[v][i])
                        addRealtime(ParamChange{locOf(loc), m.index, static_cast<u8>(i), m.params[v][i], static_cast<u8>(v)});
        }
    for (const auto& m : now.settings)
        for (const auto& s : synth_.settings)
            if (s.index == m.index)
                for (std::size_t v = 0; v < variations && v < m.params.size() && v < s.params.size(); ++v)
                    for (std::size_t i = 0; i < m.params[v].size() && i < s.params[v].size(); ++i)
                        if (m.params[v][i] != s.params[v][i])
                            addRealtime(ParamChange{2, m.index, static_cast<u8>(i), m.params[v][i], static_cast<u8>(v)});
    for (std::size_t v = 0; v < variations; ++v) {
        const auto was = morphMap(synth_, v);
        const auto is = morphMap(now, v);
        for (const auto& [key, range] : was)
            if (!is.count(key))
                addRealtime(MorphChange{std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key), 0,
                                        static_cast<u8>(v)});
        for (const auto& [key, range] : is) {
            const auto it = was.find(key);
            if (it == was.end() || it->second != range)
                addRealtime(MorphChange{std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key), range,
                                        static_cast<u8>(v)});
        }
    }

    // The variation shown.
    if (now.header.activeVariation != synth_.header.activeVariation)
        add(VariationSelect{now.header.activeVariation});
    // The header: at once, or 600 ms after the last change of the voice count.
    if (headerBytes(now.header) != headerBytes(synth_.header)) {
        if (now.header.voiceCount != synth_.header.voiceCount || headerDue_) {
            headerDue_ = ms + kVoicesDelayMs;
        } else {
            auto h = now.header;
            synth_.header = h;
            bubble.push_back(SectionDump{file::Section{file::kPatchHeader, h, 0, {}}});
        }
    } else {
        headerDue_.reset();
    }
    // The textpad and the name.
    if (now.notes != synth_.notes)
        add(SectionDump{file::Section{file::kTextpad, file::Textpad{now.notes}, 0, {}}});
    if (!name.empty() && name != name_) {
        name_ = name;
        bubble.push_back(NameDump{id::PatchName, name});
    }

    if (!bubble.empty())
        out.push_back({false, std::move(bubble)});
    for (auto& m : realtime)
        out.push_back({true, {std::move(m)}});
    return out;
}

std::vector<Outgoing> LiveSlot::tick(const Patch& now, std::uint64_t ms)
{
    if (!headerDue_ || ms < *headerDue_ || needsUpload_)
        return {};
    headerDue_.reset();
    if (headerBytes(now.header) == headerBytes(synth_.header))
        return {};
    synth_.header = now.header;
    return {{false, {SectionDump{file::Section{file::kPatchHeader, now.header, 0, {}}}}}};
}

} // namespace g2::proto
