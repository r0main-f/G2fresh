// Editing model of a G2 patch and performance, independent of the file layout.
//
// Patch::fromFile / toFile convert to and from the section codec (file.hpp).
// toFile writes sections the way Clavia's editor does (fixed section order,
// names/parameters/custom data sorted by module index, module list in creation
// order), so loading and saving an unedited v1.62 file reproduces it byte for
// byte.
#pragma once

#include "g2/file.hpp"
#include "g2/module_db.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace g2 {

using u8 = std::uint8_t;

enum class Location : u8 { Fx = 0, Va = 1, Settings = 2 };

enum class CableColor : u8 { Red, Blue, Yellow, Orange, Green, Purple, White };

inline constexpr int kUserVariations = 8;   // variations 1..8 (0..7)
inline constexpr int kFileVariations = 9;   // plus the "init" variation (8)
inline constexpr int kKnobCount = 120;
inline constexpr int kMorphGroups = 8;

struct Module {
    u8 index = 0;     // 1-based id within its area
    u8 type = 0;
    u8 col = 0, row = 0;
    u8 color = 0;
    bool uprate = false;
    bool locked = false; // excluded from randomizing
    u8 reserved = 0;
    std::vector<u8> modes;
    std::string name;
    // Parameter values per variation: params[variation][param]. Empty for
    // modules without parameters.
    std::vector<std::vector<u8>> params;
    // Raw [kind, len, payload] records (parameter labels, editor values).
    std::optional<std::vector<u8>> customData;

    const db::ModuleDef* def() const { return db::find(type); }
};

struct Cable {
    CableColor color = CableColor::Red;
    u8 fromModule = 0, fromConn = 0;
    bool fromIsOutput = true; // false: an input-to-input link
    u8 toModule = 0, toConn = 0;

    bool operator==(const Cable&) const = default;
};

struct Area {
    Location location = Location::Va;
    std::vector<Module> modules; // creation order
    std::vector<Cable> cables;
    u8 cablePad = 0;      // file detail: padding bits of the cable list
    u8 namesReserved = 0; // file detail: 6 bits Clavia's writer leaves unset

    Module* find(u8 index);
    const Module* find(u8 index) const;
};

// A module of the patch-settings area (location 2): 1 morph, 2 gain,
// 3 glide, 4 bend, 5 vibrato, 6 arpeggiator, 7 misc.
struct SettingsModule {
    u8 index = 0;
    std::vector<std::vector<u8>> params; // [variation][param]
};

using MorphAssign = file::MorphAssign;
using KnobAssign = file::KnobAssign;
using CtrlAssign = file::CtrlAssign;

struct MorphVariation {
    std::vector<u8> legacyDials; // always zero in v1.62 files
    std::vector<MorphAssign> assigns;
};

struct Patch {
    file::PatchHeader header;
    Area va{Location::Va, {}, {}, 0, 0};
    Area fx{Location::Fx, {}, {}, 0, 0};
    file::CurrentNotes currentNotes;
    u8 variationCount = kFileVariations;
    std::vector<SettingsModule> settings;
    std::vector<file::CustomModule> settingsCustomData; // morph group labels
    u8 morphCount = kMorphGroups;
    std::vector<u8> keyboardMorphAssign = std::vector<u8>(kMorphGroups, 0);
    std::vector<MorphVariation> morphs; // per variation
    std::vector<std::optional<KnobAssign>> knobs = std::vector<std::optional<KnobAssign>>(kKnobCount);
    std::vector<CtrlAssign> controllers;
    std::string notes; // the patch's textpad

    // File-level details kept for faithful saving.
    std::vector<std::string> textHeader = file::defaultTextHeader(file::FileType::Patch);
    std::uint16_t version = file::kCurrentVersion;

    Area& area(Location loc) { return loc == Location::Fx ? fx : va; }
    const Area& area(Location loc) const { return loc == Location::Fx ? fx : va; }

    // An empty patch as the original editor creates it.
    static Patch makeDefault();

    // Converts from / to the section codec. fromFile accepts the 18-section
    // patch layout (versions 13..23); it throws FormatError otherwise.
    static Patch fromFile(const file::File& file);
    static Patch fromSections(std::span<const file::Section> sections); // the 18 sections
    file::File toFile() const;
    std::vector<file::Section> toSections() const;
};

struct Performance {
    file::PerfHeader header;
    std::array<Patch, 4> slots;
    std::vector<std::optional<KnobAssign>> globalKnobs = std::vector<std::optional<KnobAssign>>(kKnobCount);
    std::vector<std::string> textHeader = file::defaultTextHeader(file::FileType::Performance);
    std::uint16_t version = file::kCurrentVersion;

    static Performance fromFile(const file::File& file);
    file::File toFile() const;
};

// Convenience: bytes <-> model.
Patch loadPatch(std::span<const u8> bytes);
std::vector<u8> savePatch(const Patch& patch);
Performance loadPerformance(std::span<const u8> bytes);
std::vector<u8> savePerformance(const Performance& perf);

} // namespace g2
