// Patch load: the DSP resources a patch uses, computed the way the original
// editor (v1.62, class CPatchLoad) does. See re/notes/patch-load.md.
//
// The editor keeps a running sum of the per-module figures of each area
// (module type table `_k<Name>ModuleSize`, plus 3 words of dynamic RAM per
// cable) and turns it into percentages of fixed limits. Its load meters only
// show figures the synth reports (USB molecule 72, "--" otherwise); the local
// sum is what it falls back to, and its dynamic-RAM part is what it checks
// before adding a module, a cable or pasted modules.
//
// The VA figures are those of ONE voice: the editor never multiplies by the
// voice count. How many voices fit is decided by the synth (molecule 05).
#pragma once

#include "g2/patch.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace g2::patchload {

// SModuleResourceSpec: the figures of one module type, or the sum over an area.
// Sums wrap like the original's 16-bit, 8-bit and 32-bit fields.
struct ResourceSpec {
    std::uint16_t cyclesA = 0; // DSP cycles per sample of the audio-rate (A) code
    std::uint16_t cyclesB = 0; // cycles of the control-rate (B) code, run every 4th sample
    std::uint8_t zpMem = 0;    // "ZP" memory (kMaxZmem)
    std::uint8_t unused5 = 0;  // byte +5: set in some tables and in reports, never read by the editor
    std::uint16_t xMemA = 0, yMemA = 0, pMemA = 0; // DSP X/Y/P memory of the A code
    std::uint16_t xMemB = 0, yMemB = 0, pMemB = 0; // ... of the B code
    std::uint32_t dynRam = 0; // dynamic RAM (patch data words); cables add kCableDynRam each
    std::uint32_t qMem = 0;   // Q memory (kMaxQmem)
    std::uint32_t rMem = 0;   // R memory (kMaxRmem)

    ResourceSpec& operator+=(const ResourceSpec& o);
    ResourceSpec& operator-=(const ResourceSpec& o);
    bool operator==(const ResourceSpec&) const = default;
};

// CPatchLoad::EResource. The "memory" meter shows the largest of XMem..RMem.
enum class Resource : std::uint8_t { Cycles, XMem, YMem, PMem, ZpMem, DynRam, QMem, RMem };

// CPatchLoad::kMax* (100 % of each resource).
inline constexpr std::uint32_t kMaxCycles = 1371;
inline constexpr std::uint32_t kMaxXMem = 4336;
inline constexpr std::uint32_t kMaxYMem = 2992;
inline constexpr std::uint32_t kMaxPMem = 6498;
inline constexpr std::uint32_t kMaxZpMem = 128;
inline constexpr std::uint32_t kMaxDynRam = 131072; // kMaxDynMem, shared by FX + VA
inline constexpr std::uint32_t kMaxQMem = 260096;
inline constexpr std::uint32_t kMaxRMem = 256;
inline constexpr std::uint32_t kCableDynRam = 3;    // per cable, CPatchLoad::AddCable
inline constexpr int kMaxModulesPerArea = 127;      // CPatch::InternalNewModule

// The type's table, nullptr for a type the editor does not know (it would
// refuse such a patch). Generated: core/src/patch_load_data.cpp.
const ResourceSpec* moduleSpec(std::uint8_t type);

// What a module adds to its area: the type's figures, with the B code folded
// into the A code when the module is uprated (CPatchLoad::AddModule). An
// unknown type costs nothing.
ResourceSpec moduleCost(std::uint8_t type, bool uprate);

// One area, as percentages of the limits (100 = full; may exceed 100),
// computed exactly like CPatchLoad::Get*.
struct AreaLoad {
    ResourceSpec total;
    float cycles = 0;  // (A + B / 4) / kMaxCycles
    float xMem = 0, yMem = 0, pMem = 0; // (A + B) / limit
    float zpMem = 0, dynRam = 0, qMem = 0, rMem = 0;
    // Largest memory resource (cycles are never candidates). Cycles, with 0 %,
    // only for an area that is empty in compute() (the editor never evaluated it).
    Resource critical = Resource::XMem;
    float criticalPercent = 0;          // its percentage: the "memory" meter

    float percent(Resource r) const;
};

// Percentages of an area total: the local sum, or a synth report.
AreaLoad evaluate(const ResourceSpec& total);

// Sum of an area's modules and cables.
ResourceSpec areaTotal(const Area& area);

struct Load {
    // The four meters, as fractions of the limit (0..1, may exceed 1):
    // cycles and "memory" (critical resource) of the VA and FX areas.
    float vaCycles = 0, vaMemory = 0, fxCycles = 0, fxMemory = 0;
    AreaLoad va, fx;
    // kMaxDynRam minus the dynamic RAM of both areas (the patch-settings area
    // never contributes). Signed like the original; negative only for a patch
    // that is already too big.
    std::int32_t freeDynamicRam = static_cast<std::int32_t>(kMaxDynRam);

    bool overloaded() const { return vaCycles > 1 || vaMemory > 1 || fxCycles > 1 || fxMemory > 1; }
};

Load compute(const Patch& patch);
// From two synth reports (molecule 72), as the editor shows them.
Load fromReports(const ResourceSpec& va, const ResourceSpec& fx);

// The editor's "patch too big" checks, all on dynamic RAM. They compare the
// free RAM as an unsigned number, like the original.
//  - adding a module from the toolbar (CTabButton::Action): its RAM <= free;
//    InternalNewModule also refuses a 128th module in an area;
//  - connecting a cable (CCableDragDropManager::Drop): free > 2;
//  - pasting (CClipboard::AdaptForNewModules): the clipboard's size (modules'
//    RAM + 3 per cable, summed in 16 bits) <= free, else the paste is dropped.
bool canAddModule(const Patch& patch, Location area, std::uint8_t type);
bool canAddCable(const Patch& patch);
std::uint16_t dynamicSize(std::span<const Module> modules, std::size_t cableCount);
bool canPaste(const Patch& patch, std::uint16_t dynamicSize);

// Decodes a synth report, the payload of molecule 72 after its id
// (CMPatchLoad::ReadStream @000163da): u8 location (non-zero = VA), u14 cyclesA,
// u14 cyclesB, u8 zp, u14 (byte +5), 6 x u14 X/Y/P A/B, u14 dynRam,
// u16 + u16 qMem (high word first), u14 rMem. u14 = two 7-bit bytes, MSB first.
struct Report {
    Location area = Location::Fx;
    ResourceSpec spec;
};
std::optional<Report> parseReport(std::span<const std::uint8_t> payload);
// The reverse, for the virtual G2: fields wider than their encoding are
// clipped (u14 to 0x3FFF; whether the synth scales dynamic RAM or Q memory
// to fit is not known).
std::vector<std::uint8_t> encodeReport(const Report& report);

} // namespace g2::patchload
