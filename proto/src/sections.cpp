#include "g2/proto/sections.hpp"

#include <algorithm>

namespace g2::proto {
namespace {

using file::Section;

// The 18 patch sections in file order, as (id, location); location -1 for
// sections that come once (pch2-format §2, CPatchFile_13::WriteMolecules).
constexpr std::array<std::pair<u8, int>, 18> kPatchOrder{{
    {file::kPatchHeader, -1}, {file::kModuleList, 1}, {file::kModuleList, 0}, {file::kCurrentNotes, -1},
    {file::kCableList, 1},    {file::kCableList, 0},  {file::kParamList, 2},  {file::kParamList, 1},
    {file::kParamList, 0},    {file::kMorphMap, -1},  {file::kKnobMap, -1},   {file::kCtrlMap, -1},
    {file::kCustomData, 2},   {file::kCustomData, 1}, {file::kCustomData, 0}, {file::kModuleNames, 1},
    {file::kModuleNames, 0},  {file::kTextpad, -1},
}};

void toUsbVariations(Section& s)
{
    if (auto* list = std::get_if<file::ParamList>(&s.payload)) {
        if (list->modules.empty() || list->variationCount >= kUsbVariations)
            return;
        for (auto& m : list->modules)
            for (u8 v = list->variationCount; v < kUsbVariations; ++v)
                m.variations.push_back({v, std::vector<u8>(m.paramCount, 0)});
        list->variationCount = kUsbVariations;
    } else if (auto* morph = std::get_if<file::MorphMap>(&s.payload)) {
        while (morph->variations.size() < kUsbVariations)
            morph->variations.push_back(
                {static_cast<u8>(morph->variations.size()), std::vector<u8>(morph->morphCount, 0), {}});
    }
}

void toFileVariations(Section& s)
{
    constexpr u8 kFile = static_cast<u8>(kFileVariations);
    if (auto* list = std::get_if<file::ParamList>(&s.payload)) {
        if (list->variationCount <= kFile)
            return;
        for (auto& m : list->modules)
            std::erase_if(m.variations, [](const file::ParamVariation& v) { return v.variation >= kFile; });
        list->variationCount = kFile;
    } else if (auto* morph = std::get_if<file::MorphMap>(&s.payload)) {
        if (morph->variations.size() > kFile)
            morph->variations.resize(kFile);
    }
}

int slotOf(const Section& s)
{
    const auto loc = sectionLocation(s);
    for (std::size_t i = 0; i < kPatchOrder.size(); ++i)
        if (kPatchOrder[i].first == s.id && kPatchOrder[i].second == (loc ? int{*loc} : -1))
            return static_cast<int>(i);
    return -1;
}

} // namespace

std::vector<Section> uploadSections(const Patch& patch)
{
    auto sections = patch.toSections();
    for (auto& s : sections)
        toUsbVariations(s);
    return sections;
}

std::vector<Section> fileSections(std::vector<Section> sections)
{
    for (auto& s : sections)
        toFileVariations(s);
    return sections;
}

Bubble patchUpload(u8 slot, const Patch& patch, const std::string& name)
{
    std::vector<Molecule> molecules{DumpDestination{0, 0, 0, name}};
    for (auto& s : uploadSections(patch))
        molecules.push_back(SectionDump{std::move(s)});
    return Bubble::patchVoid(slot, std::move(molecules));
}

std::vector<Section> performanceSections(const Performance& perf)
{
    auto sections = perf.toFile().sections;
    for (auto& s : sections)
        toUsbVariations(s);
    return sections;
}

Performance performanceFromSections(std::span<const Section> sections)
{
    file::File f;
    f.type = file::FileType::Performance;
    f.textHeader = file::defaultTextHeader(file::FileType::Performance);
    f.sections = fileSections(std::vector<Section>(sections.begin(), sections.end()));
    return Performance::fromFile(f);
}

Bubble performanceUpload(const Performance& perf, const std::string& name)
{
    std::vector<Molecule> molecules{DumpDestination{0, 0, 0, name}, CompletePerformance{name, performanceSections(perf)}};
    return Bubble::synth(std::move(molecules));
}

std::optional<u8> sectionLocation(const Section& s)
{
    switch (s.id) {
    case file::kModuleList:
    case file::kParamList:
    case file::kCableList:
    case file::kModuleNames:
    case file::kCustomData: {
        // Every one of these starts with the 2-bit location.
        const auto payload = file::encodeSection(s);
        if (payload.empty())
            return std::nullopt;
        return static_cast<u8>(payload[0] >> 6);
    }
    default:
        return std::nullopt;
    }
}

void PatchAssembler::reset(const Patch* from)
{
    sections_ = {};
    if (!from)
        return;
    auto sections = from->toSections();
    for (std::size_t i = 0; i < sections.size() && i < sections_.size(); ++i)
        sections_[i] = std::move(sections[i]);
}

bool PatchAssembler::add(const Section& section)
{
    const int i = slotOf(section);
    if (i < 0)
        return false;
    sections_[static_cast<std::size_t>(i)] = section;
    return true;
}

bool PatchAssembler::complete() const
{
    return std::all_of(sections_.begin(), sections_.end(), [](const auto& s) { return s.has_value(); });
}

Patch PatchAssembler::patch() const
{
    if (!complete())
        throw FormatError("incomplete patch download");
    std::vector<Section> sections;
    for (const auto& s : sections_)
        sections.push_back(*s);
    return Patch::fromSections(fileSections(std::move(sections)));
}

} // namespace g2::proto
