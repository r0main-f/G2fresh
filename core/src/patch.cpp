#include "g2/patch.hpp"

#include <algorithm>
#include <map>

namespace g2 {
namespace {

using namespace file;

// Patch-settings pseudo-module index -> module type in the database.
constexpr std::array<std::pair<u8, u8>, 7> kSettingsTypes{{
    {1, 6}, {2, 95}, {3, 135}, {4, 137}, {5, 138}, {6, 136}, {7, 153},
}};

template <class T>
const T& expect(const Section& s, u8 id)
{
    if (s.id != id)
        throw FormatError("unexpected section order");
    if (s.tailPad != 0 || !s.trailing.empty())
        throw FormatError("unsupported data after a section's fields");
    const T* p = std::get_if<T>(&s.payload);
    if (!p)
        throw FormatError("section could not be decoded");
    return *p;
}

void checkVariations(const std::vector<ParamVariation>& vars)
{
    for (std::size_t i = 0; i < vars.size(); ++i)
        if (vars[i].variation != i)
            throw FormatError("parameter variations out of order");
}

std::vector<std::vector<u8>> valuesOf(const ParamModule& m)
{
    checkVariations(m.variations);
    std::vector<std::vector<u8>> out;
    for (const auto& v : m.variations)
        out.push_back(v.values);
    return out;
}

ParamModule paramModule(u8 index, const std::vector<std::vector<u8>>& params)
{
    ParamModule m;
    m.index = index;
    m.paramCount = static_cast<u8>(params.empty() ? 0 : params.front().size());
    for (std::size_t v = 0; v < params.size(); ++v)
        m.variations.push_back({static_cast<u8>(v), params[v]});
    return m;
}

void readArea(Area& a, const ModuleList& list, const CableList& cables, const ParamList& params,
              const CustomData& custom, const ModuleNames& names)
{
    a.modules.clear();
    for (const auto& e : list.modules) {
        Module m;
        m.index = e.index;
        m.type = e.type;
        m.col = e.col;
        m.row = e.row;
        m.color = e.color;
        m.uprate = e.uprate != 0;
        m.locked = e.locked != 0;
        m.reserved = e.reserved;
        m.modes = e.modes;
        a.modules.push_back(std::move(m));
    }
    a.cablePad = cables.pad;
    a.cables.clear();
    for (const auto& c : cables.cables)
        a.cables.push_back({static_cast<CableColor>(std::min<u8>(c.color, 6)), c.fromModule, c.fromConn,
                            c.fromIsOutput != 0, c.toModule, c.toConn});
    for (const auto& pm : params.modules)
        if (Module* m = a.find(pm.index))
            m->params = valuesOf(pm);
    for (const auto& cm : custom.modules)
        if (Module* m = a.find(cm.index))
            m->customData = cm.bytes;
    a.namesReserved = names.reserved;
    for (const auto& n : names.names)
        if (Module* m = a.find(n.index))
            m->name = n.name;
}

template <class T, class F>
std::vector<T> sortedByIndex(const std::vector<Module>& modules, F make)
{
    std::map<u8, T> sorted;
    for (const auto& m : modules)
        if (auto v = make(m))
            sorted.emplace(m.index, std::move(*v));
    std::vector<T> out;
    for (auto& [index, v] : sorted)
        out.push_back(std::move(v));
    return out;
}

Section section(u8 id, Payload p)
{
    return Section{id, std::move(p), 0, {}};
}

std::vector<std::vector<u8>> defaultParams(const db::ModuleDef& def, int variations)
{
    if (def.params.empty())
        return {};
    std::vector<u8> values;
    for (const auto& p : def.params)
        values.push_back(p.defaultValue);
    return std::vector<std::vector<u8>>(static_cast<std::size_t>(variations), values);
}

} // namespace

Module* Area::find(u8 index)
{
    auto it = std::find_if(modules.begin(), modules.end(), [&](const Module& m) { return m.index == index; });
    return it == modules.end() ? nullptr : &*it;
}

const Module* Area::find(u8 index) const
{
    return const_cast<Area*>(this)->find(index);
}

Patch Patch::makeDefault()
{
    Patch p;
    p.header.voiceCount = 2;
    p.header.monoMode = 1;
    for (const auto& [index, type] : kSettingsTypes)
        if (const auto* def = db::find(type))
            p.settings.push_back({index, defaultParams(*def, kFileVariations)});
    // Morph group labels, as stored by the original editor: 7 bytes, NUL-padded.
    static const char* const kMorphLabels[kMorphGroups] = {"Wheel", "Vel", "Keyb", "Aft.Tch",
                                                            "Sust.Pd", "Ctrl.Pd", "P.Stick", "G.Wh 2"};
    file::CustomModule labels{1, {}};
    for (int i = 0; i < kMorphGroups; ++i) {
        std::string label = kMorphLabels[i];
        label.resize(7, '\0');
        labels.bytes.push_back(1);
        labels.bytes.push_back(8);
        labels.bytes.push_back(static_cast<u8>(8 + i));
        labels.bytes.insert(labels.bytes.end(), label.begin(), label.end());
    }
    p.settingsCustomData.push_back(std::move(labels));
    for (int v = 0; v < kFileVariations; ++v)
        p.morphs.push_back({std::vector<u8>(kMorphGroups, 0), {}});
    p.controllers = {{7, 2, 2, 0}, {17, 2, 7, 0}};
    return p;
}

Patch Patch::fromSections(std::span<const Section> s)
{
    if (s.size() != 18)
        throw FormatError("a patch has 18 sections");
    Patch p;
    p.header = expect<PatchHeader>(s[0], kPatchHeader);
    const auto& vaList = expect<ModuleList>(s[1], kModuleList);
    const auto& fxList = expect<ModuleList>(s[2], kModuleList);
    p.currentNotes = expect<CurrentNotes>(s[3], kCurrentNotes);
    const auto& vaCables = expect<CableList>(s[4], kCableList);
    const auto& fxCables = expect<CableList>(s[5], kCableList);
    const auto& settings = expect<ParamList>(s[6], kParamList);
    const auto& vaParams = expect<ParamList>(s[7], kParamList);
    const auto& fxParams = expect<ParamList>(s[8], kParamList);
    const auto& morph = expect<MorphMap>(s[9], kMorphMap);
    const auto& knobs = expect<KnobMap>(s[10], kKnobMap);
    const auto& ctrls = expect<CtrlMap>(s[11], kCtrlMap);
    const auto& settingsCustom = expect<CustomData>(s[12], kCustomData);
    const auto& vaCustom = expect<CustomData>(s[13], kCustomData);
    const auto& fxCustom = expect<CustomData>(s[14], kCustomData);
    const auto& vaNames = expect<ModuleNames>(s[15], kModuleNames);
    const auto& fxNames = expect<ModuleNames>(s[16], kModuleNames);
    p.notes = expect<Textpad>(s[17], kTextpad).text;

    if (vaList.location != 1 || fxList.location != 0 || settings.location != 2 || settingsCustom.location != 2
        || vaCustom.location != 1 || fxCustom.location != 0)
        throw FormatError("unexpected section locations");

    readArea(p.va, vaList, vaCables, vaParams, vaCustom, vaNames);
    readArea(p.fx, fxList, fxCables, fxParams, fxCustom, fxNames);

    p.variationCount = settings.variationCount;
    for (const auto& m : settings.modules)
        p.settings.push_back({m.index, valuesOf(m)});
    p.settingsCustomData = settingsCustom.modules;

    p.morphCount = morph.morphCount;
    p.keyboardMorphAssign = morph.keyboardAssign;
    for (std::size_t i = 0; i < morph.variations.size(); ++i) {
        if (morph.variations[i].variation != i)
            throw FormatError("morph variations out of order");
        p.morphs.push_back({morph.variations[i].legacyDials, morph.variations[i].morphs});
    }
    p.knobs = knobs.knobs;
    p.controllers = ctrls.controllers;
    return p;
}

Patch Patch::fromFile(const File& f)
{
    if (f.type != FileType::Patch)
        throw FormatError("not a patch file");
    if (f.version < 13 || f.version > kCurrentVersion)
        throw FormatError("unsupported patch file version " + std::to_string(f.version));
    if (!f.unparsedTail.empty())
        throw FormatError("unexpected data at the end of the file");
    Patch p = fromSections(f.sections);
    p.textHeader = f.textHeader;
    p.version = f.version;
    return p;
}

std::vector<Section> Patch::toSections() const
{
    auto moduleList = [](const Area& a) {
        ModuleList l;
        l.location = static_cast<u8>(a.location);
        for (const auto& m : a.modules)
            l.modules.push_back({m.type, m.index, m.col, m.row, m.color, static_cast<u8>(m.uprate),
                                 static_cast<u8>(m.locked), m.reserved, m.modes});
        return l;
    };
    auto cableList = [](const Area& a) {
        CableList l;
        l.location = static_cast<u8>(a.location);
        l.pad = a.cablePad;
        for (const auto& c : a.cables)
            l.cables.push_back({static_cast<u8>(c.color), c.fromModule, c.fromConn,
                                static_cast<u8>(c.fromIsOutput), c.toModule, c.toConn});
        return l;
    };
    auto paramList = [this](const Area& a) {
        ParamList l;
        l.location = static_cast<u8>(a.location);
        l.modules = sortedByIndex<ParamModule>(a.modules, [](const Module& m) -> std::optional<ParamModule> {
            if (m.params.empty())
                return std::nullopt;
            return paramModule(m.index, m.params);
        });
        l.variationCount = l.modules.empty() ? 0 : variationCount;
        return l;
    };
    auto customData = [](const Area& a) {
        CustomData d;
        d.location = static_cast<u8>(a.location);
        d.modules = sortedByIndex<CustomModule>(a.modules, [](const Module& m) -> std::optional<CustomModule> {
            if (!m.customData)
                return std::nullopt;
            return CustomModule{m.index, *m.customData};
        });
        return d;
    };
    auto names = [](const Area& a) {
        ModuleNames n;
        n.location = static_cast<u8>(a.location);
        n.reserved = a.namesReserved;
        n.names = sortedByIndex<ModuleName>(a.modules, [](const Module& m) -> std::optional<ModuleName> {
            return ModuleName{m.index, m.name};
        });
        return n;
    };

    ParamList settingsList;
    settingsList.location = static_cast<u8>(Location::Settings);
    std::vector<SettingsModule> sortedSettings = settings;
    std::sort(sortedSettings.begin(), sortedSettings.end(),
              [](const SettingsModule& a, const SettingsModule& b) { return a.index < b.index; });
    for (const auto& m : sortedSettings)
        settingsList.modules.push_back(paramModule(m.index, m.params));
    settingsList.variationCount = settingsList.modules.empty() ? 0 : variationCount;

    MorphMap morph;
    morph.morphCount = morphCount;
    morph.keyboardAssign = keyboardMorphAssign;
    for (std::size_t v = 0; v < morphs.size(); ++v)
        morph.variations.push_back({static_cast<u8>(v), morphs[v].legacyDials, morphs[v].assigns});

    CustomData settingsCustom{static_cast<u8>(Location::Settings), settingsCustomData};
    std::sort(settingsCustom.modules.begin(), settingsCustom.modules.end(),
              [](const CustomModule& a, const CustomModule& b) { return a.index < b.index; });

    std::vector<Section> s;
    s.push_back(section(kPatchHeader, header));
    s.push_back(section(kModuleList, moduleList(va)));
    s.push_back(section(kModuleList, moduleList(fx)));
    s.push_back(section(kCurrentNotes, currentNotes));
    s.push_back(section(kCableList, cableList(va)));
    s.push_back(section(kCableList, cableList(fx)));
    s.push_back(section(kParamList, settingsList));
    s.push_back(section(kParamList, paramList(va)));
    s.push_back(section(kParamList, paramList(fx)));
    s.push_back(section(kMorphMap, morph));
    s.push_back(section(kKnobMap, KnobMap{knobs}));
    s.push_back(section(kCtrlMap, CtrlMap{controllers}));
    s.push_back(section(kCustomData, settingsCustom));
    s.push_back(section(kCustomData, customData(va)));
    s.push_back(section(kCustomData, customData(fx)));
    s.push_back(section(kModuleNames, names(va)));
    s.push_back(section(kModuleNames, names(fx)));
    s.push_back(section(kTextpad, Textpad{notes}));
    return s;
}

File Patch::toFile() const
{
    File f;
    f.textHeader = textHeader;
    f.version = version;
    f.type = FileType::Patch;
    f.sections = toSections();
    return f;
}

Performance Performance::fromFile(const File& f)
{
    if (f.type != FileType::Performance)
        throw FormatError("not a performance file");
    if (f.version < 13 || f.version > kCurrentVersion)
        throw FormatError("unsupported performance file version " + std::to_string(f.version));
    if (f.sections.size() != 1 + 4 * 18 + 1 || !f.unparsedTail.empty())
        throw FormatError("a performance has 74 sections");
    Performance perf;
    perf.header = expect<PerfHeader>(f.sections.front(), kPerfHeader);
    for (std::size_t slot = 0; slot < 4; ++slot)
        perf.slots[slot] = Patch::fromSections(std::span<const Section>(f.sections).subspan(1 + slot * 18, 18));
    perf.globalKnobs = expect<KnobMap>(f.sections.back(), kGlobalKnobMap).knobs;
    perf.textHeader = f.textHeader;
    perf.version = f.version;
    return perf;
}

File Performance::toFile() const
{
    File f;
    f.textHeader = textHeader;
    f.version = version;
    f.type = FileType::Performance;
    f.sections.push_back(section(kPerfHeader, header));
    for (const auto& p : slots) {
        auto s = p.toSections();
        f.sections.insert(f.sections.end(), std::make_move_iterator(s.begin()), std::make_move_iterator(s.end()));
    }
    f.sections.push_back(section(kGlobalKnobMap, KnobMap{globalKnobs}));
    return f;
}

Patch loadPatch(std::span<const u8> bytes)
{
    const File f = read(bytes);
    if (!f.crcValid)
        throw FormatError("checksum error: the file is damaged");
    return Patch::fromFile(f);
}

std::vector<u8> savePatch(const Patch& patch)
{
    return write(patch.toFile());
}

Performance loadPerformance(std::span<const u8> bytes)
{
    const File f = read(bytes);
    if (!f.crcValid)
        throw FormatError("checksum error: the file is damaged");
    return Performance::fromFile(f);
}

std::vector<u8> savePerformance(const Performance& perf)
{
    return write(perf.toFile());
}

} // namespace g2
