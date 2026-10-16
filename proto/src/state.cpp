#include "g2/proto/state.hpp"

#include "g2/edit.hpp"
#include "g2/proto/sections.hpp"

#include <algorithm>
#include <type_traits>

namespace g2::proto {
namespace {

template <class... F>
struct Overloaded : F... {
    using F::operator()...;
};
template <class... F>
Overloaded(F...) -> Overloaded<F...>;

bool areaLocation(u8 loc) { return loc == 0 || loc == 1; }
Area& areaOf(Patch& p, u8 loc) { return p.area(static_cast<Location>(loc)); }

// A module's parameter values [variation][param], in an area or the patch settings.
std::vector<std::vector<u8>>* paramsOf(Patch& p, u8 loc, u8 module)
{
    if (areaLocation(loc)) {
        auto* m = areaOf(p, loc).find(module);
        return m ? &m->params : nullptr;
    }
    if (loc == 2)
        for (auto& s : p.settings)
            if (s.index == module)
                return &s.params;
    return nullptr;
}

// The cable between the molecule's two connectors, whichever way round it
// names them (the original sends tree links child -> parent, new cables
// origin -> target; a patch stores them output first).
bool sameEnds(const Cable& c, const CableEdit& e)
{
    const bool forward = c.fromModule == e.fromModule && c.fromConn == e.fromConn && c.fromIsOutput == e.fromIsOutput
                      && c.toModule == e.toModule && c.toConn == e.toConn && !e.toIsOutput;
    const bool backward = c.fromModule == e.toModule && c.fromConn == e.toConn && c.fromIsOutput == e.toIsOutput
                       && c.toModule == e.fromModule && c.toConn == e.fromConn && !e.fromIsOutput;
    return forward || backward;
}

Cable cableOf(const CableEdit& e)
{
    if (e.toIsOutput && !e.fromIsOutput)
        return Cable{static_cast<CableColor>(e.color), e.toModule, e.toConn, true, e.fromModule, e.fromConn, std::nullopt};
    return Cable{static_cast<CableColor>(e.color), e.fromModule, e.fromConn, e.fromIsOutput, e.toModule, e.toConn,
                 std::nullopt};
}

// A whole-section replacement through the section codec, keeping the rest.
void replaceSection(Patch& p, const file::Section& s)
{
    PatchAssembler a;
    a.reset(&p);
    if (a.add(fileSections({s}).front()))
        p = a.patch();
}

void mergeParamList(Patch& p, const file::ParamList& list)
{
    // The Mutator's variation (9) in a patch that has none yet.
    for (const auto& pm : list.modules)
        for (const auto& v : pm.variations)
            if (v.variation == kAuditionVariation)
                edit::addAuditionVariation(p);
    for (const auto& pm : list.modules) {
        auto* params = paramsOf(p, list.location, pm.index);
        if (!params)
            continue;
        for (const auto& v : pm.variations) {
            if (v.variation >= params->size())
                continue;
            auto& values = (*params)[v.variation];
            for (std::size_t i = 0; i < v.values.size() && i < values.size(); ++i)
                values[i] = v.values[i];
        }
    }
}

// A morph map's variations replace the patch's (the Mutator sends one).
void mergeMorphMap(Patch& p, const file::MorphMap& map)
{
    for (const auto& v : map.variations) {
        if (v.variation == kAuditionVariation)
            edit::addAuditionVariation(p);
        if (v.variation < p.morphs.size())
            p.morphs[v.variation].assigns = v.morphs;
    }
}

void mergeModuleList(Patch& p, const file::ModuleList& list)
{
    if (!areaLocation(list.location))
        return;
    auto& area = areaOf(p, list.location);
    for (const auto& e : list.modules) {
        Module* m = area.find(e.index);
        if (!m) {
            area.modules.push_back({});
            m = &area.modules.back();
            m->index = e.index;
        }
        m->type = e.type;
        m->col = e.col;
        m->row = e.row;
        m->color = e.color;
        m->uprate = e.uprate != 0;
        m->locked = e.locked != 0;
        m->reserved = e.reserved;
        m->modes = e.modes;
        if (m->params.empty())
            if (const auto* def = m->def(); def && !def->params.empty()) {
                std::vector<u8> values;
                for (const auto& d : def->params)
                    values.push_back(d.defaultValue);
                m->params.assign(p.variationCount, values);
            }
    }
}

void addModule(Patch& p, const ModuleNew& n)
{
    if (!areaLocation(n.location))
        return;
    auto& area = areaOf(p, n.location);
    std::erase_if(area.modules, [&](const Module& m) { return m.index == n.index; });
    Module m;
    m.index = n.index;
    m.type = n.type;
    m.col = n.col;
    m.row = n.row;
    m.color = n.color;
    m.uprate = n.uprate != 0;
    m.locked = n.locked != 0;
    m.modes = n.modes;
    m.name = n.name;
    if (const auto* def = m.def(); def && !def->params.empty()) {
        std::vector<u8> values;
        for (const auto& d : def->params)
            values.push_back(d.defaultValue);
        m.params.assign(edit::variationSlots(p), values);
    }
    area.modules.push_back(std::move(m));
}

void applySection(Patch& p, const file::Section& s)
{
    switch (s.id) {
    case file::kParamList:
        if (const auto* l = std::get_if<file::ParamList>(&s.payload))
            mergeParamList(p, *l);
        return;
    case file::kModuleList:
        if (const auto* l = std::get_if<file::ModuleList>(&s.payload))
            mergeModuleList(p, *l);
        return;
    case file::kCableList:
        if (const auto* l = std::get_if<file::CableList>(&s.payload); l && areaLocation(l->location)) {
            auto& area = areaOf(p, l->location);
            for (const auto& c : l->cables) {
                const Cable cable{static_cast<CableColor>(c.color), c.fromModule, c.fromConn, c.fromIsOutput != 0,
                                  c.toModule, c.toConn, std::nullopt};
                if (std::find(area.cables.begin(), area.cables.end(), cable) == area.cables.end())
                    area.cables.push_back(cable);
            }
        }
        return;
    case file::kModuleNames:
        if (const auto* l = std::get_if<file::ModuleNames>(&s.payload); l && areaLocation(l->location))
            for (const auto& n : l->names)
                if (auto* m = areaOf(p, l->location).find(n.index))
                    m->name = n.name;
        return;
    case file::kCustomData:
        if (const auto* l = std::get_if<file::CustomData>(&s.payload)) {
            for (const auto& c : l->modules) {
                if (areaLocation(l->location)) {
                    if (auto* m = areaOf(p, l->location).find(c.index))
                        m->customData = c.bytes;
                } else if (l->location == 2) {
                    std::erase_if(p.settingsCustomData, [&](const file::CustomModule& x) { return x.index == c.index; });
                    p.settingsCustomData.push_back(c);
                }
            }
        }
        return;
    case file::kMorphMap:
        if (const auto* l = std::get_if<file::MorphMap>(&s.payload); l && l->variations.size() < p.morphs.size())
            mergeMorphMap(p, *l);
        else
            replaceSection(p, s);
        return;
    case file::kPatchHeader:
    case file::kCtrlMap:
    case file::kKnobMap:
    case file::kCurrentNotes:
    case file::kTextpad:
        replaceSection(p, s);
        return;
    default:
        return;
    }
}

} // namespace

Performance SynthState::performance() const
{
    Performance perf;
    perf.header = perfHeader;
    for (int i = 0; i < kSlots; ++i) {
        perf.slots[static_cast<std::size_t>(i)] = slots[static_cast<std::size_t>(i)].patch;
        perf.header.slots[static_cast<std::size_t>(i)].patchName = slots[static_cast<std::size_t>(i)].name;
    }
    perf.globalKnobs = globalKnobs;
    return perf;
}

void SynthState::setPerformance(const Performance& perf, const std::string& name)
{
    perfName = name;
    perfHeader = perf.header;
    for (int i = 0; i < kSlots; ++i) {
        slots[static_cast<std::size_t>(i)].patch = perf.slots[static_cast<std::size_t>(i)];
        slots[static_cast<std::size_t>(i)].name = perf.header.slots[static_cast<std::size_t>(i)].patchName;
    }
    globalKnobs = perf.globalKnobs;
    globalKnobs.resize(kKnobCount);
}

bool applyToPatch(Patch& p, const Molecule& molecule)
{
    // Edits referring to something the patch does not have are ignored, as a
    // synth with a stale view would; the edit layer's checks throw.
    auto guarded = [](auto&& f) {
        try {
            f();
        } catch (const std::exception&) {
        }
        return true;
    };
    return std::visit(
        Overloaded{
            [&](const ParamChange& m) {
                if (auto* params = paramsOf(p, m.location, m.module)) {
                    if (m.variation < params->size() && m.param < (*params)[m.variation].size())
                        (*params)[m.variation][m.param] = m.value;
                    // Variation 0x7F: every variation (the 10 of a slot), clamped
                    // to the parameter's range (CPatch::HandleParamChange @000dc636
                    // -> CModule::SetCommonParamValue @000aa1b4).
                    if (m.variation == 0x7F)
                        guarded([&] {
                            for (std::size_t v = 0; v < params->size(); ++v)
                                edit::setTargetValue(p, {static_cast<Location>(m.location), m.module, m.param},
                                                     static_cast<u8>(v), m.value);
                        });
                }
                return true;
            },
            [&](const MorphChange& m) {
                if (m.variation == 0x7F)
                    return true; // redrawn only (CPatch::HandleMorphChange @000dc3b8)
                return guarded([&] {
                    // One (parameter, group) assignment; range 0 removes it (CPatch::HandleMorphChange).
                    edit::storeMorph(p, m.variation, static_cast<Location>(m.location), m.module, m.param, m.morph, m.range);
                });
            },
            [&](const VariationSelect& m) {
                if (m.variation < edit::variationSlots(p))
                    p.header.activeVariation = m.variation;
                return true;
            },
            [&](const VariationCopy& m) { return guarded([&] { edit::copyVariation(p, m.from, m.to); }); },
            [&](const ModeChange& m) {
                return guarded([&] { edit::setMode(p, static_cast<Location>(m.location), m.module, m.mode, m.value); });
            },
            [&](const Uprate& m) {
                if (areaLocation(m.location))
                    if (auto* mod = areaOf(p, m.location).find(m.module))
                        mod->uprate = m.uprate != 0;
                return true;
            },
            [&](const KnobAssign& m) {
                if (m.knob < p.knobs.size())
                    p.knobs[m.knob] = file::KnobAssign{m.location, m.module, m.assignType, m.param, 0};
                return true;
            },
            [&](const KnobDeassign& m) {
                if (m.id != id::KnobDeassign)
                    return false; // 1D is the performance's
                if (m.knob < p.knobs.size())
                    p.knobs[m.knob].reset();
                return true;
            },
            [&](const CtrlAssign& m) {
                return guarded([&] {
                    // What the synth has, without the editor's rules.
                    edit::storeMidiCc(p, static_cast<u8>(m.cc & 0x7F), static_cast<Location>(m.location), m.module, m.param);
                });
            },
            [&](const CtrlDeassign& m) { return guarded([&] { edit::eraseMidiCc(p, static_cast<u8>(m.cc & 0x7F)); }); },
            [&](const ModuleNew& m) {
                addModule(p, m);
                return true;
            },
            [&](const ModuleRecolor& m) {
                if (areaLocation(m.location))
                    if (auto* mod = areaOf(p, m.location).find(m.module))
                        mod->color = m.color;
                return true;
            },
            [&](const ModuleDelete& m) {
                // The module and whatever still refers to it (the editor sends its
                // cables', knobs', controllers' and morphs' removals first).
                if (areaLocation(m.location)) {
                    auto& area = areaOf(p, m.location);
                    std::erase_if(area.modules, [&](const Module& x) { return x.index == m.module; });
                    std::erase_if(area.cables, [&](const Cable& c) { return c.fromModule == m.module || c.toModule == m.module; });
                    auto refers = [&](u8 loc, u8 module) { return loc == m.location && module == m.module; };
                    for (auto& v : p.morphs)
                        std::erase_if(v.assigns, [&](const MorphAssign& a) { return refers(a.location, a.module); });
                    for (auto& k : p.knobs)
                        if (k && refers(k->location, k->module))
                            k.reset();
                    std::erase_if(p.controllers, [&](const file::CtrlAssign& c) { return refers(c.location, c.module); });
                }
                return true;
            },
            [&](const ModuleRename& m) {
                if (areaLocation(m.location))
                    if (auto* mod = areaOf(p, m.location).find(m.module))
                        mod->name = m.name;
                return true;
            },
            [&](const ModuleMove& m) {
                if (areaLocation(m.location))
                    if (auto* mod = areaOf(p, m.location).find(m.module)) {
                        mod->col = m.col;
                        mod->row = m.row;
                    }
                return true;
            },
            [&](const CableEdit& m) {
                auto& area = p.area(m.va ? Location::Va : Location::Fx);
                if (m.id == id::CableConnect) {
                    if (std::none_of(area.cables.begin(), area.cables.end(), [&](const Cable& c) { return sameEnds(c, m); }))
                        area.cables.push_back(cableOf(m));
                } else if (m.id == id::CableDelete) {
                    std::erase_if(area.cables, [&](const Cable& c) { return sameEnds(c, m); });
                } else {
                    for (auto& c : area.cables)
                        if (sameEnds(c, m))
                            c.color = static_cast<CableColor>(m.color);
                }
                return true;
            },
            [&](const CustomData& m) {
                if (areaLocation(m.location))
                    if (auto* mod = areaOf(p, m.location).find(m.module))
                        mod->customData = m.bytes;
                return true;
            },
            [&](const MutaLock& m) {
                if (areaLocation(m.location))
                    if (auto* mod = areaOf(p, m.location).find(m.module))
                        mod->locked = m.locked != 0;
                return true;
            },
            [&](const SectionDump& m) {
                if (m.section.id == file::kPerfHeader || m.section.id == file::kGlobalKnobMap)
                    return false;
                guarded([&] { applySection(p, m.section); });
                return true;
            },
            [](const auto&) { return false; },
        },
        molecule);
}

bool applyToSlot(SlotState& slot, const Molecule& molecule)
{
    if (applyToPatch(slot.patch, molecule))
        return true;
    return std::visit(Overloaded{
                          [&](const NameDump& m) {
                              if (m.id != id::PatchName)
                                  return false;
                              slot.name = m.name;
                              return true;
                          },
                          [&](const PageFocus& m) {
                              if (m.id != id::PageFocus)
                                  return false;
                              slot.page = m.page;
                              return true;
                          },
                          [&](const ParamFocus& m) {
                              slot.focus = m;
                              return true;
                          },
                          [&](const PatchLoad& m) {
                              slot.load[m.isVA ? 1 : 0] = m;
                              return true;
                          },
                          [&](const Blink& m) {
                              if (m.id == id::Leds)
                                  decodeLeds(m, slot.leds);
                              else
                                  decodeMeters(m, slot.meters);
                              return true;
                          },
                          [](const auto&) { return false; },
                      },
                      molecule);
}

bool applyToSynth(SynthState& s, const Molecule& molecule)
{
    return std::visit(
        Overloaded{
            [&](const SynthData& m) {
                s.settings = m.settings;
                return true;
            },
            [&](const Voices& m) {
                s.voices = m.voices;
                s.voicesReceived = true;
                return true;
            },
            [&](const SlotFlags& m) {
                (m.id == id::SlotSelection ? s.slotEnabled : s.keyboardEnabled) = m.slots;
                return true;
            },
            [&](const SlotFocus& m) {
                s.slotFocus = m.slot;
                return true;
            },
            [&](const SectionDump& m) {
                if (const auto* h = std::get_if<file::PerfHeader>(&m.section.payload)) {
                    s.perfHeader = *h;
                    return true;
                }
                if (m.section.id == file::kGlobalKnobMap)
                    if (const auto* k = std::get_if<file::KnobMap>(&m.section.payload)) {
                        s.globalKnobs = k->knobs;
                        s.globalKnobs.resize(kKnobCount);
                        return true;
                    }
                return false;
            },
            [&](const FlashUsage& m) {
                s.flashUsage = m.value;
                return true;
            },
            [&](const GlobalKnobAssign& m) {
                if (m.knob < s.globalKnobs.size())
                    s.globalKnobs[m.knob] = file::KnobAssign{m.location, m.module, m.assignType, m.param, m.slot};
                return true;
            },
            [&](const KnobDeassign& m) {
                if (m.id != id::GlobalKnobDeassign)
                    return false;
                if (m.knob < s.globalKnobs.size())
                    s.globalKnobs[m.knob].reset();
                return true;
            },
            [&](const PageFocus& m) {
                if (m.id != id::GlobalPageFocus)
                    return false;
                s.globalPage = m.page;
                return true;
            },
            [&](const NameDump& m) {
                if (m.id != id::PerfName)
                    return false;
                s.perfName = m.name;
                return true;
            },
            [&](const PerfHeaderParam& m) {
                // Param 0 clock run, 1 BPM, 2 global knob pages (the knob floater,
                // CKnobFloaterContainer::HandleChangeRequests @00132420); scope
                // 0xFF = global [I].
                if (m.value && m.scope == 0xFF) {
                    if (m.param == 0)
                        s.perfHeader.masterClockRun = *m.value;
                    else if (m.param == 1)
                        s.perfHeader.masterClockBpm = *m.value;
                    else if (m.param == 2)
                        s.perfHeader.globalPages = *m.value;
                }
                return true;
            },
            [&](const ClockInfo& m) {
                s.clock = m;
                return true;
            },
            [&](const MidiLearn& m) {
                s.midiLearn = m;
                return true;
            },
            [](const auto&) { return false; },
        },
        molecule);
}

void decodeLeds(const Blink& b, std::array<u8, kLedsPerSlot>& leds)
{
    for (std::size_t i = b.start, k = 0; i < leds.size(); ++i, ++k) {
        const std::size_t byte = k / 4;
        if (byte >= b.data.size())
            break;
        leds[i] = static_cast<u8>((b.data[byte] >> (2 * (k % 4))) & 3);
    }
}

std::vector<u8> encodeLeds(std::span<const u8> leds)
{
    std::vector<u8> data((leds.size() + 3) / 4, 0);
    for (std::size_t i = 0; i < leds.size(); ++i)
        data[i / 4] = static_cast<u8>(data[i / 4] | ((leds[i] & 3) << (2 * (i % 4))));
    return data;
}

void decodeMeters(const Blink& b, std::array<std::uint16_t, kLedsPerSlot>& meters)
{
    for (std::size_t i = b.start, k = 0; i < meters.size() && 2 * k + 1 < b.data.size(); ++i, ++k)
        meters[i] = static_cast<std::uint16_t>(b.data[2 * k] | (b.data[2 * k + 1] << 8));
}

std::vector<u8> encodeMeters(std::span<const std::uint16_t> meters)
{
    std::vector<u8> data;
    for (std::uint16_t w : meters) {
        data.push_back(static_cast<u8>(w & 0xFF));
        data.push_back(static_cast<u8>(w >> 8));
    }
    return data;
}

} // namespace g2::proto
