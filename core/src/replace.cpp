#include "g2/replace.hpp"

#include "g2/edit.hpp"
#include "g2/uprate.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace g2::replace {

// ---- Database lookups ---------------------------------------------------------

const Group* groupOf(u8 type)
{
    for (const auto& g : groups())
        for (const auto& m : g.members)
            if (m.type == type)
                return &g;
    return nullptr;
}

namespace {

const Member* memberOf(const Group& g, u8 type)
{
    for (const auto& m : g.members)
        if (m.type == type)
            return &m;
    return nullptr;
}

bool allowedIn(const Member& m, Location loc)
{
    return (m.contexts >> static_cast<int>(loc)) & 1;
}

} // namespace

std::vector<u8> candidates(u8 type)
{
    std::vector<u8> out;
    const Group* g = groupOf(type);
    if (!g || g->members.size() < 2) // CModuleReplaceMenu::CanReplace
        return out;
    for (const auto& m : g->members)
        out.push_back(m.type);
    return out;
}

std::vector<MenuItem> menu(u8 type, Location loc)
{
    std::vector<MenuItem> out;
    for (u8 t : candidates(type)) {
        const auto* def = db::find(t);
        const Member* m = memberOf(*groupOf(type), t);
        out.push_back({t, def && def->longName ? def->longName : "", t != type && allowedIn(*m, loc)});
    }
    return out;
}

bool canReplace(u8 type, u8 newType, Location loc)
{
    if (loc == Location::Settings || type == newType)
        return false;
    const Group* g = groupOf(type);
    if (!g || g->members.size() < 2)
        return false;
    const Member* m = memberOf(*g, newType);
    return m && allowedIn(*m, loc) && db::find(newType);
}

// ---- Mapping (CModuleReplacer::CModuleReplacer @ 0010a6e6) -----------------------

namespace {

// CItemGroup::Find on every class, then CItemGroup::FindAndRemoveFirst.
template <class T, class Pred>
std::optional<std::pair<T, T>> take(std::vector<std::vector<T>>& classes, Pred isOld, u8 newType)
{
    for (auto& c : classes) {
        const auto src = std::find_if(c.begin(), c.end(), isOld);
        if (src == c.end())
            continue;
        const auto dst = std::find_if(c.begin(), c.end(), [&](const T& i) { return i.type == newType; });
        if (dst == c.end())
            return std::nullopt;
        std::pair<T, T> result{*src, *dst};
        c.erase(dst);
        return result;
    }
    return std::nullopt;
}

} // namespace

Mapping mapping(u8 oldType, u8 newType, std::span<const u8> connectedInputs,
                std::span<const u8> connectedOutputs)
{
    const auto* oldDef = db::find(oldType);
    const auto* newDef = db::find(newType);
    const Group* g = groupOf(oldType);
    if (!oldDef || !newDef || !g || groupOf(newType) != g)
        throw std::invalid_argument("the modules are not in the same replace group");

    Mapping map;
    map.inputs.assign(oldDef->inputs.size(), kNone);
    map.outputs.assign(oldDef->outputs.size(), kNone);
    map.params.assign(oldDef->params.size(), kNone);

    // CModuleGroup::Clone: a working copy keeping only the two types' items.
    auto keep = [&](u8 t) { return t == oldType || t == newType; };
    std::vector<std::vector<InputItem>> ins;
    std::vector<std::vector<OutputItem>> outs;
    std::vector<std::vector<ParamItem>> params;
    for (const auto& ig : g->inputs) {
        auto& v = ins.emplace_back();
        std::copy_if(ig.items.begin(), ig.items.end(), std::back_inserter(v), [&](const auto& i) { return keep(i.type); });
    }
    for (const auto& og : g->outputs) {
        auto& v = outs.emplace_back();
        std::copy_if(og.items.begin(), og.items.end(), std::back_inserter(v), [&](const auto& i) { return keep(i.type); });
    }
    for (const auto& pg : g->params) {
        auto& v = params.emplace_back();
        std::copy_if(pg.items.begin(), pg.items.end(), std::back_inserter(v), [&](const auto& i) { return keep(i.type); });
    }

    // CModuleGroup::Map{Input,Output,Param}: the first class holding the old
    // item, then the first new-type item of that class, which is used up.
    auto mapIn = [&](auto& classes, auto isOld) { return take(classes, isOld, newType); };

    for (u8 conn : connectedInputs) {
        if (conn >= map.inputs.size())
            continue;
        const auto r = mapIn(ins, [&](const InputItem& i) { return i.type == oldType && i.conn == conn; });
        if (!r)
            continue;
        const auto& [src, dst] = *r;
        map.inputs[conn] = dst.conn;
        if (src.amountParam != kNone && dst.amountParam != kNone)
            map.params[src.amountParam] = dst.amountParam;
        if (src.onParam != kNone && dst.onParam != kNone)
            map.params[src.onParam] = dst.onParam;
    }
    // CModuleGroup::TransferUnMappedAmounts / TransferUnMappedOns: what is
    // left of each input class becomes a parameter class of the attenuators,
    // then one of the on/off buttons.
    for (int which = 0; which < 2; ++which)
        for (const auto& c : ins) {
            std::vector<ParamItem> p;
            for (const auto& i : c) {
                const u8 param = which == 0 ? i.amountParam : i.onParam;
                if (param != kNone)
                    p.push_back({i.type, param});
            }
            if (!p.empty())
                params.push_back(std::move(p));
        }
    for (u8 conn : connectedOutputs) {
        if (conn >= map.outputs.size())
            continue;
        if (const auto r = mapIn(outs, [&](const OutputItem& i) { return i.type == oldType && i.conn == conn; }))
            map.outputs[conn] = r->second.conn;
    }
    for (std::size_t p = 0; p < map.params.size(); ++p) {
        if (map.params[p] != kNone)
            continue;
        if (const auto r = mapIn(params, [&](const ParamItem& i) { return i.type == oldType && i.param == p; }))
            map.params[p] = r->second.param;
    }
    return map;
}

// ---- Replace (CPatchData::GetReplaceModuleMolecules @ 000ebf7e) --------------------

namespace {

struct End {
    u8 module = 0, conn = 0;
    bool isOutput = false;
    bool operator==(const End&) const = default;
};

End fromOf(const Cable& c) { return {c.fromModule, c.fromConn, c.fromIsOutput}; }
End toOf(const Cable& c) { return {c.toModule, c.toConn, false}; }

Cable makeCable(CableColor color, End from, End to)
{
    return {color, from.module, from.conn, from.isOutput, to.module, to.conn};
}

// CPatchData::GetFreeNameIndex @ 000e6cf8: the smallest positive number not
// used by a module named prefix + number (the number read with atoi from the
// three characters after the prefix).
int freeNameIndex(const Area& area, const std::string& prefix)
{
    std::vector<int> used;
    for (const auto& m : area.modules)
        if (m.name.compare(0, prefix.size(), prefix) == 0 && m.name.size() >= prefix.size())
            used.push_back(std::atoi(m.name.substr(prefix.size(), 3).c_str()) & 0xFF);
    std::sort(used.begin(), used.end());
    int n = 1;
    for (int u : used)
        if (u == n)
            ++n;
    return n & 0xFF;
}

} // namespace

u8 replaceModule(Patch& patch, Location loc, u8 index, u8 newType)
{
    if (loc == Location::Settings)
        throw std::invalid_argument("modules live in the VA or FX area");
    Area& area = patch.area(loc);
    const Module* oldPtr = area.find(index);
    if (!oldPtr)
        throw std::invalid_argument("no module with this index");
    const Module old = *oldPtr;
    if (!canReplace(old.type, newType, loc))
        throw std::invalid_argument("this module cannot be replaced by that type");
    const auto* oldDef = db::find(old.type);
    const auto* newDef = db::find(newType);

    // CPatch::InternalReplaceModule @ 000d7a14: the connectors in a cable tree.
    std::vector<u8> connIns, connOuts;
    for (u8 c = 0; c < oldDef->inputs.size(); ++c)
        if (std::any_of(area.cables.begin(), area.cables.end(), [&](const Cable& k) {
                return toOf(k) == End{index, c, false} || fromOf(k) == End{index, c, false};
            }))
            connIns.push_back(c);
    for (u8 c = 0; c < oldDef->outputs.size(); ++c)
        if (std::any_of(area.cables.begin(), area.cables.end(),
                        [&](const Cable& k) { return fromOf(k) == End{index, c, true}; }))
            connOuts.push_back(c);
    const Mapping map = mapping(old.type, newType, connIns, connOuts);

    // The new module: CPatchData::GetUniqueID (lowest index not in use, the
    // old module still counting), old position and colour, defaults otherwise.
    u8 newIndex = 1;
    while (area.find(newIndex))
        if (++newIndex == 0)
            throw std::invalid_argument("the area is full");

    Module m;
    m.index = newIndex;
    m.type = newType;
    m.col = old.col;
    m.row = old.row;
    m.color = old.color;
    m.locked = newDef->defaultLocked;
    for (const auto& mode : newDef->modes)
        m.modes.push_back(mode.defaultValue);
    if (!newDef->params.empty()) {
        std::vector<u8> values;
        for (const auto& p : newDef->params)
            values.push_back(p.defaultValue);
        m.params.assign(patch.variationCount, values);
    }
    // Values follow the mapping when both parameters have the same range.
    for (std::size_t p = 0; p < map.params.size(); ++p) {
        const u8 q = map.params[p];
        if (q == kNone || q >= newDef->params.size() || newDef->params[q].max != oldDef->params[p].max)
            continue;
        for (std::size_t v = 0; v < m.params.size() && v < old.params.size(); ++v)
            if (p < old.params[v].size())
                m.params[v][q] = old.params[v][p];
    }
    // Name: a name starting with the old type's default name becomes the new
    // type's default name with the first free number (at most 15 characters).
    const std::string oldPrefix = oldDef->shortName;
    if (old.name.size() >= oldPrefix.size() && old.name.compare(0, oldPrefix.size(), oldPrefix) == 0) {
        m.name = newDef->shortName + std::to_string(freeNameIndex(area, newDef->shortName));
        if (m.name.size() > 15)
            m.name.resize(15);
    } else {
        m.name = old.name;
    }

    // ---- Cables ----
    auto mapEnd = [&](End e) -> std::optional<End> {
        if (e.module != index)
            return e;
        const auto& table = e.isOutput ? map.outputs : map.inputs;
        if (e.conn >= table.size() || table[e.conn] == kNone)
            return std::nullopt;
        return End{newIndex, table[e.conn], e.isOutput};
    };
    // Parent of each input in the cable trees, to find a tree's source.
    auto parentOf = [&](End e) -> std::optional<End> {
        for (const auto& k : area.cables)
            if (toOf(k) == e)
                return fromOf(k);
        return std::nullopt;
    };
    auto rootOf = [&](End e) {
        for (std::size_t guard = 0; guard <= area.cables.size(); ++guard) {
            const auto p = parentOf(e);
            if (!p)
                break;
            e = *p;
        }
        return e;
    };

    const std::vector<Cable> before = area.cables;
    std::vector<Cable> cables;
    auto emit = [&](const Cable& c) {
        if (c.fromModule == c.toModule && c.fromConn == c.toConn && !c.fromIsOutput)
            return;
        if (std::find(cables.begin(), cables.end(), c) == cables.end())
            cables.push_back(c);
    };
    auto newOutputColor = [&](u8 conn) {
        const auto& d = newDef->outputs[conn];
        switch (d.color) {
        case db::ConnColor::Red: return CableColor::Red;
        case db::ConnColor::Blue: case db::ConnColor::BlueRed: return CableColor::Blue; // not uprated yet
        case db::ConnColor::Yellow: case db::ConnColor::YellowOrange: return CableColor::Yellow;
        }
        return CableColor::Red;
    };

    for (const auto& c : before) {
        const End from = fromOf(c), to = toOf(c);
        if (from.module != index && to.module != index) {
            emit(c);
        } else if (to.module == index) {
            // An input of the old module is the child (first loop of the original).
            if (const auto to2 = mapEnd(to)) {
                std::optional<End> from2 = mapEnd(from);
                if (!from2 && from.module == index) {
                    // The parent has no counterpart: link to the tree's source instead.
                    const End root = rootOf(from);
                    if (!(root == from))
                        from2 = mapEnd(root);
                }
                if (from2)
                    emit(makeCable(c.color, *from2, *to2));
            } else {
                // No counterpart: bridge the input's own links to its parent.
                const auto parent = mapEnd(from);
                if (!parent)
                    continue;
                for (const auto& d : before)
                    if (fromOf(d) == to && d.toModule != index)
                        emit(makeCable(c.color, *parent, toOf(d)));
            }
        } else if (const auto from2 = mapEnd(from)) {
            // An output (or a linked input) of the old module is the parent.
            emit(makeCable(from2->isOutput ? newOutputColor(from2->conn) : c.color, *from2, to));
        }
    }
    // Nets fed by an old output: the new output's colour, or white when the
    // output has no counterpart (CPatchData::GenerateDeleteSelectionMolecules
    // recolours a tree whose source is deleted with colour 6).
    for (auto& c : cables) {
        if (c.fromModule == newIndex || c.toModule == newIndex)
            continue;
        const End root = rootOf(fromOf(c));
        if (root.module != index || !root.isOutput)
            continue;
        const auto mapped = mapEnd(root);
        c.color = mapped ? newOutputColor(mapped->conn) : CableColor::White;
    }
    area.cables = std::move(cables);

    // ---- Module list ----
    std::erase_if(area.modules, [&](const Module& x) { return x.index == index; });
    for (auto* order : {&area.nameOrder, &area.paramOrder, &area.customOrder})
        std::erase(*order, index);
    area.modules.push_back(std::move(m));

    // ---- Morphs, knobs, controllers (CMorphMap/CKnobMap/CCtrlMap::GetReplaceModuleMolecules) ----
    const u8 l = static_cast<u8>(loc);
    auto mapParam = [&](u8 p) -> u8 { return p < map.params.size() ? map.params[p] : kNone; };
    for (auto& v : patch.morphs) {
        std::vector<MorphAssign> out;
        for (const auto& a : v.assigns) {
            if (a.location != l || a.module != index) {
                out.push_back(a);
                continue;
            }
            const u8 q = mapParam(a.param);
            if (q == kNone)
                continue;
            std::erase_if(out, [&](const MorphAssign& x) { return x.location == l && x.module == newIndex && x.param == q; });
            out.push_back({l, newIndex, q, a.morph, a.range});
        }
        v.assigns = std::move(out);
    }
    for (auto& k : patch.knobs) {
        if (!k || k->location != l || k->module != index)
            continue;
        const u8 q = mapParam(k->param);
        if (q == kNone)
            k.reset();
        else
            *k = KnobAssign{l, newIndex, 0, q, k->slot};
    }
    std::vector<CtrlAssign> ctrls;
    for (const auto& c : patch.controllers) {
        if (c.location != l || c.module != index) {
            ctrls.push_back(c);
            continue;
        }
        if (const u8 q = mapParam(c.param); q != kNone)
            ctrls.push_back({c.cc, l, newIndex, q});
    }
    patch.controllers = std::move(ctrls);

    // CPatchData::GetMakeRoomForMolecules, then GenerateBandwidthChangeMolecules.
    edit::resolveOverlaps(patch, loc, newIndex);
    uprate::update(patch, loc);
    return newIndex;
}

} // namespace g2::replace
