#include "g2/edit.hpp"

#include <algorithm>
#include <stdexcept>

namespace g2::edit {
namespace {

Area& areaFor(Patch& patch, Location loc)
{
    if (loc == Location::Settings)
        throw std::invalid_argument("modules live in the VA or FX area");
    return patch.area(loc);
}

Module& moduleAt(Patch& patch, Location loc, u8 index)
{
    Module* m = areaFor(patch, loc).find(index);
    if (!m)
        throw std::invalid_argument("no module with this index");
    return *m;
}

const db::ConnectorDef& connector(const Module& m, Endpoint e)
{
    const auto* def = m.def();
    if (!def)
        throw std::invalid_argument("unknown module type");
    const auto& list = e.isOutput ? def->outputs : def->inputs;
    if (e.conn >= list.size())
        throw std::invalid_argument("no such connector");
    return list[e.conn];
}

bool refersTo(u8 location, u8 module, Location loc, u8 index)
{
    return location == static_cast<u8>(loc) && module == index;
}

} // namespace

u8 addModule(Patch& patch, Location loc, u8 type, u8 col, u8 row)
{
    Area& area = areaFor(patch, loc);
    const auto* def = db::find(type);
    if (!def || def->kind != db::ModuleKind::Module)
        throw std::invalid_argument("not a module type");
    if (col > 127 || row > 127)
        throw std::invalid_argument("position out of range");

    u8 index = 1;
    while (area.find(index))
        if (++index == 0)
            throw std::invalid_argument("the area is full");

    int number = 1;
    auto taken = [&](int n) {
        const std::string name = def->shortName + std::to_string(n);
        return std::any_of(area.modules.begin(), area.modules.end(),
                           [&](const Module& m) { return m.name == name; });
    };
    while (taken(number))
        ++number;

    Module m;
    m.index = index;
    m.type = type;
    m.col = col;
    m.row = row;
    m.locked = def->defaultLocked;
    for (const auto& mode : def->modes)
        m.modes.push_back(mode.defaultValue);
    m.name = std::string(def->shortName) + std::to_string(number);
    if (m.name.size() > 16)
        m.name.resize(16);
    if (!def->params.empty()) {
        std::vector<u8> values;
        for (const auto& p : def->params)
            values.push_back(p.defaultValue);
        m.params.assign(patch.variationCount, values);
    }
    area.modules.push_back(std::move(m));
    return index;
}

void removeModule(Patch& patch, Location loc, u8 index)
{
    Area& area = areaFor(patch, loc);
    const auto it = std::find_if(area.modules.begin(), area.modules.end(),
                                 [&](const Module& m) { return m.index == index; });
    if (it == area.modules.end())
        throw std::invalid_argument("no module with this index");
    area.modules.erase(it);
    std::erase_if(area.cables, [&](const Cable& c) { return c.fromModule == index || c.toModule == index; });
    for (auto& v : patch.morphs)
        std::erase_if(v.assigns, [&](const MorphAssign& a) { return refersTo(a.location, a.module, loc, index); });
    for (auto& k : patch.knobs)
        if (k && refersTo(k->location, k->module, loc, index))
            k.reset();
    std::erase_if(patch.controllers, [&](const CtrlAssign& c) { return refersTo(c.location, c.module, loc, index); });
}

void moveModule(Patch& patch, Location loc, u8 index, u8 col, u8 row)
{
    if (col > 127 || row > 127)
        throw std::invalid_argument("position out of range");
    Module& m = moduleAt(patch, loc, index);
    m.col = col;
    m.row = row;
}

void renameModule(Patch& patch, Location loc, u8 index, const std::string& name)
{
    if (name.size() > 16 || name.find('\0') != std::string::npos)
        throw std::invalid_argument("module names have at most 16 characters");
    moduleAt(patch, loc, index).name = name;
}

CableColor cableColor(const Patch& patch, Location loc, Endpoint from)
{
    const Module* m = patch.area(loc).find(from.module);
    if (!m)
        throw std::invalid_argument("no module with this index");
    switch (connector(*m, from).color) {
    case db::ConnColor::Red: return CableColor::Red;
    case db::ConnColor::Blue: return CableColor::Blue;
    case db::ConnColor::Yellow: return CableColor::Yellow;
    case db::ConnColor::BlueRed: return m->uprate ? CableColor::Red : CableColor::Blue;
    case db::ConnColor::YellowOrange: return m->uprate ? CableColor::Orange : CableColor::Yellow;
    }
    return CableColor::Red;
}

Cable connect(Patch& patch, Location loc, Endpoint from, Endpoint to)
{
    Area& area = areaFor(patch, loc);
    if (to.isOutput)
        throw std::invalid_argument("a cable must end at an input");
    const Module& src = moduleAt(patch, loc, from.module);
    const Module& dst = moduleAt(patch, loc, to.module);
    connector(src, from);
    connector(dst, to);
    if (from.module == to.module && from.conn == to.conn && !from.isOutput)
        throw std::invalid_argument("cannot link an input to itself");
    Cable c{cableColor(patch, loc, from), from.module, from.conn, from.isOutput, to.module, to.conn};
    for (const auto& existing : area.cables)
        if (existing.fromModule == c.fromModule && existing.fromConn == c.fromConn
            && existing.fromIsOutput == c.fromIsOutput && existing.toModule == c.toModule
            && existing.toConn == c.toConn)
            throw std::invalid_argument("these connectors are already connected");
    area.cables.push_back(c);
    return c;
}

void disconnect(Patch& patch, Location loc, const Cable& cable)
{
    Area& area = areaFor(patch, loc);
    const auto it = std::find(area.cables.begin(), area.cables.end(), cable);
    if (it == area.cables.end())
        throw std::invalid_argument("no such cable");
    area.cables.erase(it);
}

void setParam(Patch& patch, Location loc, u8 module, u8 param, u8 variation, u8 value)
{
    Module& m = moduleAt(patch, loc, module);
    const auto* def = m.def();
    if (!def || param >= def->params.size() || variation >= m.params.size()
        || param >= m.params[variation].size())
        throw std::invalid_argument("no such parameter");
    const auto& p = def->params[param];
    m.params[variation][param] = std::clamp(value, p.min, p.max);
}

void setMode(Patch& patch, Location loc, u8 module, u8 mode, u8 value)
{
    Module& m = moduleAt(patch, loc, module);
    const auto* def = m.def();
    if (!def || mode >= def->modes.size() || mode >= m.modes.size())
        throw std::invalid_argument("no such mode");
    const auto& md = def->modes[mode];
    m.modes[mode] = std::clamp(value, md.min, md.max);
}

std::string paramText(const Patch& patch, Location loc, u8 module, u8 param, u8 variation)
{
    const Module* m = patch.area(loc).find(module);
    if (!m || !m->def() || variation >= m->params.size())
        return {};
    return db::formatParam(*m->def(), param, m->params[variation], m->modes);
}

} // namespace g2::edit
