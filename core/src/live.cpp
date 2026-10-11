#include "g2/live.hpp"

#include "g2/edit.hpp"

#include <algorithm>
#include <iterator>

namespace g2::live {
namespace {

thread_local Recorder* gCurrent = nullptr;

// The variations a module's values go out in: 10 over USB (0..9, the
// Mutator's included; CPatch::CPatch @000d96d4 makes 10 parameter settings).
constexpr std::size_t kVariations = static_cast<std::size_t>(kAuditionVariation) + 1;

bool inList(const std::vector<u8>& list, u8 index)
{
    return std::find(list.begin(), list.end(), index) != list.end();
}

file::Section section(u8 id, file::Payload payload)
{
    file::Section s;
    s.id = id;
    s.payload = std::move(payload);
    return s;
}

file::ParamModule paramModule(u8 index, const std::vector<std::vector<u8>>& params, const std::vector<std::size_t>& variations)
{
    file::ParamModule m;
    m.index = index;
    m.paramCount = params.empty() ? u8{0} : static_cast<u8>(params.front().size());
    for (const std::size_t v : variations) {
        // A patch without the audition variation has the init variation's
        // values there, as a new module has (CModule::Init @000ab63a).
        const std::size_t from = v < params.size() ? v : std::min<std::size_t>(params.size() - 1, kFileVariations - 1);
        m.variations.push_back({static_cast<u8>(v), params[from]});
    }
    return m;
}

std::vector<std::size_t> allVariations()
{
    std::vector<std::size_t> v(kVariations);
    for (std::size_t i = 0; i < v.size(); ++i)
        v[i] = i;
    return v;
}

} // namespace

std::vector<Op> Edit::redo() const
{
    std::vector<Op> out;
    for (const auto& e : entries_)
        out.push_back(e.redo);
    return out;
}

std::vector<Op> Edit::undo() const
{
    std::vector<Op> out;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        out.insert(out.end(), it->undo.begin(), it->undo.end());
    return out;
}

void Edit::add(Op redo, std::vector<Op> undo)
{
    entries_.push_back({std::move(redo), std::move(undo)});
}

void Edit::moveTail(std::size_t from, std::size_t to)
{
    if (from >= entries_.size() || to >= from)
        return;
    std::rotate(entries_.begin() + static_cast<std::ptrdiff_t>(to), entries_.begin() + static_cast<std::ptrdiff_t>(from),
                entries_.end());
}

void Edit::append(const Edit& other)
{
    if (other.entries_.empty())
        return;
    realtime = entries_.empty() ? other.realtime : realtime && other.realtime;
    entries_.insert(entries_.end(), other.entries_.begin(), other.entries_.end());
}

Recorder::Recorder() : previous_(gCurrent)
{
    gCurrent = this;
}

Recorder::~Recorder()
{
    gCurrent = previous_;
}

Edit* recording()
{
    return gCurrent ? &gCurrent->edit() : nullptr;
}

void record(Op redo, Op undo)
{
    if (auto* e = recording())
        e->add(std::move(redo), {std::move(undo)});
}

void record(Op redo, std::vector<Op> undo)
{
    if (auto* e = recording())
        e->add(std::move(redo), std::move(undo));
}

void markRealtime()
{
    if (auto* e = recording())
        e->realtime = true;
}

file::Section paramSection(const Patch& patch, Location loc, const std::vector<u8>& modules)
{
    file::ParamList l;
    l.location = static_cast<u8>(loc);
    const auto variations = allVariations();
    std::vector<const Module*> sorted; // ascending index, as CModuleParamData_11 writes them
    for (const auto& m : patch.area(loc).modules)
        if (inList(modules, m.index) && !m.params.empty())
            sorted.push_back(&m);
    std::sort(sorted.begin(), sorted.end(), [](const Module* a, const Module* b) { return a->index < b->index; });
    for (const Module* m : sorted)
        l.modules.push_back(paramModule(m->index, m->params, variations));
    l.variationCount = l.modules.empty() ? u8{0} : static_cast<u8>(variations.size());
    return section(file::kParamList, l);
}

std::vector<file::Section> moduleSections(const Patch& patch, Location loc, const std::vector<u8>& modules)
{
    const Area& area = patch.area(loc);
    std::vector<const Module*> sorted;
    for (const auto& m : area.modules)
        if (inList(modules, m.index))
            sorted.push_back(&m);
    std::sort(sorted.begin(), sorted.end(), [](const Module* a, const Module* b) { return a->index < b->index; });

    std::vector<file::Section> out;
    file::CableList cables;
    cables.location = static_cast<u8>(loc);
    for (const auto& c : area.cables)
        if (inList(modules, c.fromModule) && inList(modules, c.toModule))
            cables.cables.push_back({static_cast<u8>(c.color), c.fromModule, c.fromConn, static_cast<u8>(c.fromIsOutput),
                                     c.toModule, c.toConn});
    out.push_back(section(file::kCableList, cables));
    out.push_back(paramSection(patch, loc, modules));
    file::CustomData custom;
    custom.location = static_cast<u8>(loc);
    for (const Module* m : sorted)
        if (m->customData && !m->customData->empty())
            custom.modules.push_back({m->index, *m->customData});
    if (!custom.modules.empty())
        out.push_back(section(file::kCustomData, custom));
    file::ModuleNames names;
    names.location = static_cast<u8>(loc);
    for (const Module* m : sorted)
        names.names.push_back({m->index, m->name});
    out.push_back(section(file::kModuleNames, names));
    return out;
}

file::Section variationParams(const Patch& patch, Location loc, u8 variation)
{
    file::ParamList l;
    l.location = static_cast<u8>(loc);
    const std::vector<std::size_t> one{variation};
    if (loc == Location::Settings) {
        std::vector<const SettingsModule*> sorted;
        for (const auto& m : patch.settings)
            if (!m.params.empty())
                sorted.push_back(&m);
        std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->index < b->index; });
        for (const auto* m : sorted)
            l.modules.push_back(paramModule(m->index, m->params, one));
    } else {
        std::vector<const Module*> sorted;
        for (const auto& m : patch.area(loc).modules)
            if (!m.params.empty())
                sorted.push_back(&m);
        std::sort(sorted.begin(), sorted.end(), [](const Module* a, const Module* b) { return a->index < b->index; });
        for (const Module* m : sorted)
            l.modules.push_back(paramModule(m->index, m->params, one));
    }
    l.variationCount = l.modules.empty() ? u8{0} : u8{1};
    return section(file::kParamList, l);
}

file::Section variationMorphs(const Patch& patch, u8 variation)
{
    file::MorphMap morph;
    morph.morphCount = patch.morphCount;
    morph.keyboardAssign = patch.keyboardMorphAssign;
    if (variation < patch.morphs.size())
        morph.variations.push_back({variation, patch.morphs[variation].legacyDials, patch.morphs[variation].assigns});
    else
        morph.variations.push_back({variation, std::vector<u8>(patch.morphCount, 0), {}});
    return section(file::kMorphMap, morph);
}

void recordVariationDump(const Patch& before, const Patch& after, u8 variation)
{
    if (!recording())
        return;
    Patch was = before;
    if (variation == kAuditionVariation)
        edit::addAuditionVariation(was);
    for (const auto loc : {Location::Settings, Location::Va, Location::Fx})
        record(Section{variationParams(after, loc, variation)}, Section{variationParams(was, loc, variation)});
    record(Section{variationMorphs(after, variation)}, Section{variationMorphs(was, variation)});
}

} // namespace g2::live
