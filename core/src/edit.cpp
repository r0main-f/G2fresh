#include "g2/edit.hpp"

#include "g2/patch_load.hpp"
#include "g2/special.hpp"
#include "g2/uprate.hpp"

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
    if (!patchload::canAddModule(patch, loc, type))
        throw std::invalid_argument("the patch is too big: the synth has no room for another module here");

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
    // The note sequencer's view (zoom, octave offset), as the original stores it.
    if (type == special::kSeqNoteType)
        m.customData = special::noteSeqCustomData({});
    area.modules.push_back(std::move(m));
    uprate::update(patch, loc);
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
    uprate::update(patch, loc);
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
                    // Rows stop at 127: a module that cannot go lower stays
                    // where it is, overlapping, rather than looping forever.
                    const auto below = static_cast<u8>(std::min(127, p->row + heightOf(*p)));
                    moved = below != m->row;
                    m->row = below;
                    if (!moved)
                        break;
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
    if (!patchload::canAddCable(patch))
        throw std::invalid_argument("the patch is too big: the synth has no room for another cable");
    Cable c{cableColor(patch, loc, from), from.module, from.conn, from.isOutput, to.module, to.conn, std::nullopt};
    for (const auto& existing : area.cables)
        if (existing.fromModule == c.fromModule && existing.fromConn == c.fromConn
            && existing.fromIsOutput == c.fromIsOutput && existing.toModule == c.toModule
            && existing.toConn == c.toConn)
            throw std::invalid_argument("these connectors are already connected");
    area.cables.push_back(c);
    // Rates (and so cable colours) follow the new connection, as in the original.
    uprate::update(patch, loc);
    for (const auto& cable : area.cables)
        if (cable.fromModule == c.fromModule && cable.fromConn == c.fromConn && cable.fromIsOutput == c.fromIsOutput
            && cable.toModule == c.toModule && cable.toConn == c.toConn)
            return cable;
    return c;
}

void disconnect(Patch& patch, Location loc, const Cable& cable)
{
    Area& area = areaFor(patch, loc);
    const auto it = std::find(area.cables.begin(), area.cables.end(), cable);
    if (it == area.cables.end())
        throw std::invalid_argument("no such cable");
    area.cables.erase(it);
    uprate::update(patch, loc);
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

// ---- Morphs, knob and MIDI assignments, labels ---------------------------------

namespace {

bool sameTarget(u8 location, u8 module, u8 param, Location loc, u8 m, u8 p)
{
    return location == static_cast<u8>(loc) && module == m && param == p;
}

void checkParam(const Patch& patch, Location loc, u8 module, u8 param)
{
    if (loc == Location::Settings) {
        for (const auto& m : patch.settings)
            if (m.index == module && !m.params.empty() && param < m.params.front().size())
                return;
        throw std::invalid_argument("no such setting");
    }
    const Module* m = patch.area(loc).find(module);
    if (!m || !m->def() || param >= m->def()->params.size())
        throw std::invalid_argument("no such parameter");
}

// Custom-data records of a module: [kind, len, payload] with kind 1 =
// parameter label: payload [param, 7 label bytes, ...].
std::vector<u8>* customOf(Patch& patch, Location loc, u8 module)
{
    if (loc == Location::Settings) {
        for (auto& m : patch.settingsCustomData)
            if (m.index == module)
                return &m.bytes;
        patch.settingsCustomData.push_back({module, {}});
        return &patch.settingsCustomData.back().bytes;
    }
    Module* m = patch.area(loc).find(module);
    if (!m)
        throw std::invalid_argument("no module with this index");
    if (!m->customData)
        m->customData.emplace();
    return &*m->customData;
}

const std::vector<u8>* customOf(const Patch& patch, Location loc, u8 module)
{
    if (loc == Location::Settings) {
        for (const auto& m : patch.settingsCustomData)
            if (m.index == module)
                return &m.bytes;
        return nullptr;
    }
    const Module* m = patch.area(loc).find(module);
    return m && m->customData ? &*m->customData : nullptr;
}

} // namespace

std::optional<Morph> morphOf(const Patch& patch, u8 variation, Location loc, u8 module, u8 param)
{
    if (variation >= patch.morphs.size())
        return std::nullopt;
    for (const auto& a : patch.morphs[variation].assigns)
        if (sameTarget(a.location, a.module, a.param, loc, module, param))
            return Morph{a.morph, a.range};
    return std::nullopt;
}

void setMorph(Patch& patch, u8 variation, Location loc, u8 module, u8 param, u8 group, std::int8_t range)
{
    checkParam(patch, loc, module, param);
    if (variation >= patch.morphs.size() || group >= kMorphGroups)
        throw std::invalid_argument("no such variation or morph group");
    auto& list = patch.morphs[variation].assigns;
    for (auto& a : list)
        if (sameTarget(a.location, a.module, a.param, loc, module, param)) {
            a.morph = group;
            a.range = range;
            return;
        }
    list.push_back({static_cast<u8>(loc), module, param, group, range});
}

void clearMorph(Patch& patch, u8 variation, Location loc, u8 module, u8 param)
{
    if (variation >= patch.morphs.size())
        return;
    std::erase_if(patch.morphs[variation].assigns,
                  [&](const MorphAssign& a) { return sameTarget(a.location, a.module, a.param, loc, module, param); });
}

std::optional<int> knobOf(const Patch& patch, Location loc, u8 module, u8 param)
{
    for (std::size_t i = 0; i < patch.knobs.size(); ++i)
        if (const auto& k = patch.knobs[i]; k && sameTarget(k->location, k->module, k->param, loc, module, param))
            return static_cast<int>(i);
    return std::nullopt;
}

void assignKnob(Patch& patch, int knob, Location loc, u8 module, u8 param)
{
    checkParam(patch, loc, module, param);
    if (knob < 0 || static_cast<std::size_t>(knob) >= patch.knobs.size())
        throw std::invalid_argument("no such knob");
    // A parameter sits on one knob only.
    if (const auto old = knobOf(patch, loc, module, param))
        patch.knobs[static_cast<std::size_t>(*old)].reset();
    patch.knobs[static_cast<std::size_t>(knob)] = KnobAssign{static_cast<u8>(loc), module, 0, param, 0};
}

void clearKnob(Patch& patch, int knob)
{
    if (knob >= 0 && static_cast<std::size_t>(knob) < patch.knobs.size())
        patch.knobs[static_cast<std::size_t>(knob)].reset();
}

std::string knobName(int knob)
{
    const int group = knob / kKnobsPerPage;
    return std::to_string(group / 3 + 1) + static_cast<char>('A' + group % 3) + "-" + std::to_string(knob % kKnobsPerPage + 1);
}

std::optional<u8> midiCcOf(const Patch& patch, Location loc, u8 module, u8 param)
{
    for (const auto& c : patch.controllers)
        if (sameTarget(c.location, c.module, c.param, loc, module, param))
            return c.cc;
    return std::nullopt;
}

void assignMidiCc(Patch& patch, u8 cc, Location loc, u8 module, u8 param)
{
    checkParam(patch, loc, module, param);
    if (cc > 127)
        throw std::invalid_argument("MIDI controllers are 0..127");
    std::erase_if(patch.controllers, [&](const CtrlAssign& c) {
        return c.cc == cc || sameTarget(c.location, c.module, c.param, loc, module, param);
    });
    patch.controllers.push_back({cc, static_cast<u8>(loc), module, param});
}

void clearMidiCc(Patch& patch, u8 cc)
{
    std::erase_if(patch.controllers, [&](const CtrlAssign& c) { return c.cc == cc; });
}

std::string paramLabel(const Patch& patch, Location loc, u8 module, u8 param)
{
    const auto* bytes = customOf(patch, loc, module);
    if (!bytes)
        return {};
    for (std::size_t i = 0; i + 2 <= bytes->size(); i += 2u + (*bytes)[i + 1]) {
        const u8 kind = (*bytes)[i], len = (*bytes)[i + 1];
        if (i + 2 + len > bytes->size())
            break;
        if (kind == 1 && len >= 1 && (*bytes)[i + 2] == param) {
            std::string s(bytes->begin() + static_cast<std::ptrdiff_t>(i + 3),
                          bytes->begin() + static_cast<std::ptrdiff_t>(i + 3 + std::min<std::size_t>(7, len - 1u)));
            return s.substr(0, s.find('\0'));
        }
    }
    return {};
}

void setParamLabel(Patch& patch, Location loc, u8 module, u8 param, const std::string& label)
{
    checkParam(patch, loc, module, param);
    if (label.size() > 7 || label.find('\0') != std::string::npos)
        throw std::invalid_argument("labels have at most 7 characters");
    auto* bytes = customOf(patch, loc, module);
    std::size_t at = bytes->size();
    for (std::size_t i = 0; i + 2 <= bytes->size(); i += 2u + (*bytes)[i + 1])
        if ((*bytes)[i] == 1 && (*bytes)[i + 1] >= 1 && i + 2 < bytes->size() && (*bytes)[i + 2] == param) {
            at = i;
            bytes->erase(bytes->begin() + static_cast<std::ptrdiff_t>(i),
                         bytes->begin() + static_cast<std::ptrdiff_t>(i + 2 + (*bytes)[i + 1]));
            break;
        }
    if (!label.empty()) {
        std::string padded = label;
        padded.resize(7, '\0');
        std::vector<u8> record{1, 8, param};
        record.insert(record.end(), padded.begin(), padded.end());
        bytes->insert(bytes->begin() + static_cast<std::ptrdiff_t>(std::min(at, bytes->size())), record.begin(), record.end());
    }
    if (loc != Location::Settings && bytes->empty())
        patch.area(loc).find(module)->customData.reset();
}

void setModuleColor(Patch& patch, Location loc, u8 module, u8 color)
{
    moduleAt(patch, loc, module).color = color;
}

void setCableBend(Patch& patch, Location loc, const Cable& cable, std::optional<CableBend> bend)
{
    Area& area = areaFor(patch, loc);
    const auto it = std::find_if(area.cables.begin(), area.cables.end(), [&](const Cable& c) {
        return c.fromModule == cable.fromModule && c.fromConn == cable.fromConn && c.fromIsOutput == cable.fromIsOutput
            && c.toModule == cable.toModule && c.toConn == cable.toConn;
    });
    if (it == area.cables.end())
        throw std::invalid_argument("no such cable");
    it->bend = bend;
}

void clearCableBends(Patch& patch)
{
    for (Area* area : {&patch.va, &patch.fx})
        for (auto& c : area->cables)
            c.bend.reset();
}

bool hasCableBends(const Patch& patch)
{
    for (const Area* area : {&patch.va, &patch.fx})
        for (const auto& c : area->cables)
            if (c.bend)
                return true;
    return false;
}

void setCableColor(Patch& patch, Location loc, const Cable& cable, CableColor color)
{
    Area& area = areaFor(patch, loc);
    const auto it = std::find(area.cables.begin(), area.cables.end(), cable);
    if (it == area.cables.end())
        throw std::invalid_argument("no such cable");
    it->color = color;
}

void setCablesVisible(Patch& patch, CableColor color, bool visible)
{
    patch.header.cablesVisible[static_cast<std::size_t>(color)] = visible;
}

void copyVariation(Patch& patch, u8 from, u8 to)
{
    if (from >= patch.variationCount || to >= patch.variationCount)
        throw std::invalid_argument("no such variation");
    if (from == to)
        return;
    for (auto* area : {&patch.va, &patch.fx})
        for (auto& m : area->modules)
            if (from < m.params.size() && to < m.params.size())
                m.params[to] = m.params[from];
    for (auto& m : patch.settings)
        if (from < m.params.size() && to < m.params.size())
            m.params[to] = m.params[from];
    if (from < patch.morphs.size() && to < patch.morphs.size())
        patch.morphs[to].assigns = patch.morphs[from].assigns;
}

// ---- Clipboard -----------------------------------------------------------------

Clipboard copyModules(const Patch& patch, Location loc, const std::vector<u8>& indices)
{
    Clipboard clip;
    clip.from = loc;
    const Area& area = patch.area(loc);
    int minCol = 127, minRow = 127;
    for (const auto& m : area.modules)
        if (std::find(indices.begin(), indices.end(), m.index) != indices.end()) {
            clip.modules.push_back(m);
            minCol = std::min<int>(minCol, m.col);
            minRow = std::min<int>(minRow, m.row);
        }
    for (auto& m : clip.modules) {
        m.col = static_cast<u8>(m.col - minCol);
        m.row = static_cast<u8>(m.row - minRow);
    }
    auto inside = [&](u8 index) { return std::find(indices.begin(), indices.end(), index) != indices.end(); };
    for (const auto& c : area.cables)
        if (inside(c.fromModule) && inside(c.toModule))
            clip.cables.push_back(c);
    for (std::size_t v = 0; v < patch.morphs.size(); ++v)
        for (const auto& a : patch.morphs[v].assigns)
            if (a.location == static_cast<u8>(loc) && inside(a.module))
                clip.morphs.emplace_back(static_cast<u8>(v), a);
    return clip;
}

std::vector<u8> pasteModules(Patch& patch, Location loc, const Clipboard& clip, u8 col, u8 row)
{
    Area& area = areaFor(patch, loc);
    if (!patchload::canPaste(patch, patchload::dynamicSize(clip.modules, clip.cables.size()))
        || area.modules.size() + clip.modules.size() > 127)
        throw std::invalid_argument("the patch is too big: the synth has no room for these modules");
    std::vector<std::pair<u8, u8>> remap; // old index -> new index
    std::vector<u8> added;
    for (const auto& src : clip.modules) {
        u8 index = 1;
        while (area.find(index) || std::find(added.begin(), added.end(), index) != added.end())
            if (++index == 0)
                throw std::invalid_argument("the area is full");
        Module m = src;
        m.index = index;
        m.col = static_cast<u8>(std::min(127, col + src.col));
        m.row = static_cast<u8>(std::min(127, row + src.row));
        // Keep the variation count of the patch.
        if (!m.params.empty())
            m.params.resize(patch.variationCount, m.params.front());
        // A default-style name gets the next free number ("OscB1" -> "OscB2").
        if (const auto* def = m.def(); def && m.name.rfind(def->shortName, 0) == 0) {
            int number = 1;
            auto taken = [&](int n) {
                const std::string name = def->shortName + std::to_string(n);
                return std::any_of(area.modules.begin(), area.modules.end(), [&](const Module& x) { return x.name == name; });
            };
            while (taken(number))
                ++number;
            m.name = std::string(def->shortName) + std::to_string(number);
        }
        area.modules.push_back(std::move(m));
        added.push_back(index);
        remap.emplace_back(src.index, index);
    }
    auto mapped = [&](u8 old) {
        for (const auto& [o, n] : remap)
            if (o == old)
                return n;
        return u8{0};
    };
    for (Cable c : clip.cables) {
        c.fromModule = mapped(c.fromModule);
        c.toModule = mapped(c.toModule);
        if (loc != clip.from)
            c.color = cableColor(patch, loc, {c.fromModule, c.fromConn, c.fromIsOutput});
        area.cables.push_back(c);
    }
    for (auto [v, a] : clip.morphs) {
        if (v >= patch.morphs.size())
            continue;
        a.location = static_cast<u8>(loc);
        a.module = mapped(a.module);
        patch.morphs[v].assigns.push_back(a);
    }
    for (u8 index : added)
        resolveOverlaps(patch, loc, index);
    uprate::update(patch, loc);
    return added;
}

void removeModules(Patch& patch, Location loc, const std::vector<u8>& indices)
{
    for (u8 index : indices)
        if (patch.area(loc).find(index))
            removeModule(patch, loc, index);
}

void moveModules(Patch& patch, Location loc, const std::vector<u8>& indices, int dCol, int dRow)
{
    Area& area = areaFor(patch, loc);
    for (auto& m : area.modules)
        if (std::find(indices.begin(), indices.end(), m.index) != indices.end()) {
            m.col = static_cast<u8>(std::clamp(m.col + dCol, 0, 127));
            m.row = static_cast<u8>(std::clamp(m.row + dRow, 0, 127));
        }
    // The moved group keeps its place; others make room below.
    for (u8 index : indices)
        resolveOverlaps(patch, loc, index);
}

} // namespace g2::edit
