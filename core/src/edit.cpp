#include "g2/edit.hpp"

#include <algorithm>
#include <array>
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
    resolveOverlaps(patch, loc, index);
}

namespace {
int heightOf(const Module& m)
{
    const auto* def = m.def();
    return def && def->height > 0 ? def->height : 1;
}
} // namespace

void resolveOverlaps(Patch& patch, Location loc, u8 keep)
{
    Area& area = areaFor(patch, loc);
    // Per column: the kept module first, then the others top to bottom; each
    // one goes no higher than it was, below anything placed that it overlaps.
    std::vector<Module*> order;
    for (auto& m : area.modules)
        order.push_back(&m);
    std::stable_sort(order.begin(), order.end(), [&](const Module* a, const Module* b) {
        if (a->col != b->col)
            return a->col < b->col;
        if ((a->index == keep) != (b->index == keep))
            return a->index == keep;
        return a->row < b->row;
    });
    std::vector<const Module*> placed;
    for (Module* m : order) {
        bool moved = true;
        while (moved) {
            moved = false;
            for (const Module* p : placed)
                if (p->col == m->col && m->row < p->row + heightOf(*p) && p->row < m->row + heightOf(*m)) {
                    m->row = static_cast<u8>(std::min(127, p->row + heightOf(*p)));
                    moved = true;
                }
        }
        placed.push_back(m);
    }
}

u8 freeRow(const Patch& patch, Location loc, u8 col)
{
    int row = 0;
    for (const auto& m : patch.area(loc).modules)
        if (m.col == col)
            row = std::max(row, m.row + heightOf(m));
    return static_cast<u8>(std::min(row, 127));
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

// ---- Patch settings -----------------------------------------------------------

namespace {

constexpr std::array<std::pair<u8, u8>, 7> kSettingTypes{{
    {1, 6}, {2, 95}, {3, 135}, {4, 137}, {5, 138}, {6, 136}, {7, 153},
}};

SettingsModule& settingsModule(Patch& patch, Setting s)
{
    const auto index = static_cast<u8>(s);
    for (auto& m : patch.settings)
        if (m.index == index)
            return m;
    throw std::invalid_argument("the patch has no such settings module");
}

// Morph group labels live in the settings custom data, module 1, as
// [kind 1, len 8, param 8+group, 7 NUL-padded characters] records.
std::vector<u8>* morphLabels(Patch& patch)
{
    for (auto& m : patch.settingsCustomData)
        if (m.index == 1)
            return &m.bytes;
    return nullptr;
}

} // namespace

const db::ModuleDef* settingDef(Setting s)
{
    for (const auto& [index, type] : kSettingTypes)
        if (index == static_cast<u8>(s))
            return db::find(type);
    return nullptr;
}

u8 settingValue(const Patch& patch, Setting s, u8 param, u8 variation)
{
    for (const auto& m : patch.settings)
        if (m.index == static_cast<u8>(s) && variation < m.params.size() && param < m.params[variation].size())
            return m.params[variation][param];
    return 0;
}

void setSetting(Patch& patch, Setting s, u8 param, u8 variation, u8 value)
{
    auto& m = settingsModule(patch, s);
    const auto* def = settingDef(s);
    if (!def || param >= def->params.size() || variation >= m.params.size() || param >= m.params[variation].size())
        throw std::invalid_argument("no such setting");
    const auto& p = def->params[param];
    m.params[variation][param] = std::clamp(value, p.min, p.max);
}

std::string settingText(const Patch& patch, Setting s, u8 param, u8 variation)
{
    const auto* def = settingDef(s);
    if (!def)
        return {};
    for (const auto& m : patch.settings)
        if (m.index == static_cast<u8>(s) && variation < m.params.size())
            return db::formatParam(*def, param, m.params[variation], {});
    return {};
}

void setVoices(Patch& patch, VoiceMode mode, u8 polyVoices)
{
    // As the original editor's voice menu: choosing a count makes the patch
    // poly; Mono and Legato keep the stored count.
    patch.header.monoMode = static_cast<u8>(mode);
    if (mode == VoiceMode::Poly) {
        if (polyVoices < 1 || polyVoices > 32)
            throw std::invalid_argument("a patch has 1 to 32 voices");
        patch.header.voiceCount = polyVoices;
    }
}

std::string voicesText(const Patch& patch)
{
    switch (patch.header.monoMode) {
    case 1: return "Mono";
    case 2: return "Legato";
    default: return std::to_string(patch.header.voiceCount);
    }
}

void setCategory(Patch& patch, u8 category)
{
    if (category > 15)
        throw std::invalid_argument("no such category");
    patch.header.category = category;
}

const char* categoryName(u8 category)
{
    static const char* const kNames[16] = {"No Cat", "Acoustic", "Sequencer", "Bass", "Classic", "Drum",
                                           "Fantasy", "FX", "Lead", "Organ", "Pad", "Piano", "Synth",
                                           "Audio In", "User 1", "User 2"};
    return category < 16 ? kNames[category] : "";
}

std::string morphLabel(const Patch& patch, int group)
{
    for (const auto& m : patch.settingsCustomData) {
        if (m.index != 1)
            continue;
        for (std::size_t i = 0; i + 2 <= m.bytes.size(); i += 2u + m.bytes[i + 1]) {
            const u8 kind = m.bytes[i], len = m.bytes[i + 1];
            if (i + 2 + len > m.bytes.size())
                break;
            if (kind == 1 && len >= 1 && m.bytes[i + 2] == 8 + group) {
                std::string s(m.bytes.begin() + static_cast<std::ptrdiff_t>(i + 3),
                              m.bytes.begin() + static_cast<std::ptrdiff_t>(i + 2 + len));
                return s.substr(0, s.find('\0'));
            }
        }
    }
    const auto* def = settingDef(Setting::Morph);
    return def && group >= 0 && group < 8 ? def->params[static_cast<std::size_t>(group)].name : std::string();
}

void setMorphLabel(Patch& patch, int group, const std::string& label)
{
    if (group < 0 || group > 7 || label.size() > 7 || label.find('\0') != std::string::npos)
        throw std::invalid_argument("morph labels have at most 7 characters");
    auto* bytes = morphLabels(patch);
    if (!bytes) {
        patch.settingsCustomData.push_back({1, {}});
        bytes = &patch.settingsCustomData.back().bytes;
    }
    std::string padded = label;
    padded.resize(7, '\0');
    std::vector<u8> record{1, 8, static_cast<u8>(8 + group)};
    record.insert(record.end(), padded.begin(), padded.end());
    for (std::size_t i = 0; i + 2 <= bytes->size(); i += 2u + (*bytes)[i + 1]) {
        if ((*bytes)[i] == 1 && (*bytes)[i + 1] >= 1 && i + 2 < bytes->size() && (*bytes)[i + 2] == 8 + group) {
            const auto first = bytes->begin() + static_cast<std::ptrdiff_t>(i);
            const auto last = first + 2 + (*bytes)[i + 1];
            bytes->erase(first, last);
            bytes->insert(bytes->begin() + static_cast<std::ptrdiff_t>(i), record.begin(), record.end());
            return;
        }
    }
    bytes->insert(bytes->end(), record.begin(), record.end());
}

} // namespace g2::edit
