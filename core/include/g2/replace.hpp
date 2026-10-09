// The module Replace feature of the original editor (v1.62): the small button
// at the top-left of a module opens a menu of the modules of the same "replace
// group" (e.g. the LFO group); choosing one swaps the module in place. See
// re/notes/module-replace.md for the reverse-engineered algorithm.
//
// The groups and their correspondence tables (which inputs, outputs and
// parameters of the group's modules stand for one another) come from
// CReplaceDataBase::AddData @ 0010ae74 and are generated into
// core/src/replace_data.cpp by tools/moduledb/gen_replace.py.
#pragma once

#include "g2/patch.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace g2::replace {

inline constexpr u8 kNone = 0xFF;

// ---- Database (generated) ----------------------------------------------------

struct Member {
    u8 type;
    u8 contexts; // bit 0: FX area, bit 1: VA area, bit 2: patch settings (bit = Location)
};
// Items of one correspondence class: the connectors or parameters of the
// group's modules that stand for one another.
struct InputItem {
    u8 type, conn;
    u8 amountParam; // the input's attenuator ("modulation amount") parameter, or kNone
    u8 onParam;     // the input's on/off parameter (mixer channels), or kNone
};
struct OutputItem {
    u8 type, conn;
};
struct ParamItem {
    u8 type, param;
};
template <class T>
struct ItemGroup {
    const char* name; // the editor's label, e.g. "Pitch Variable"
    std::span<const T> items;
};
struct Group {
    const char* name; // e.g. "LFO Group"
    std::span<const Member> members; // menu order
    std::span<const ItemGroup<InputItem>> inputs;
    std::span<const ItemGroup<OutputItem>> outputs;
    std::span<const ItemGroup<ParamItem>> params;
};

std::span<const Group> groups();
// The group of a module type, or nullptr (CReplaceDataBase::FindGroup).
const Group* groupOf(u8 type);

// ---- Menu --------------------------------------------------------------------

// The entries of the Replace menu of a module of this type, in menu order. The
// list includes `type` itself (shown greyed); it is empty when the module has
// no menu (no group, or a group of one).
std::vector<u8> candidates(u8 type);

struct MenuItem {
    u8 type;
    const char* label; // the module's long name ("LFO B"), as the menu shows it
    bool enabled;      // false for the module's own type and types not allowed in the area
};
std::vector<MenuItem> menu(u8 type, Location loc);

// Whether a module of type `type` in area `loc` may be replaced by `newType`.
bool canReplace(u8 type, u8 newType, Location loc);

// ---- Mapping -----------------------------------------------------------------

// What CModuleReplacer computes: for every input, output and parameter of the
// old module, the corresponding one of the new module, or kNone.
struct Mapping {
    std::vector<u8> inputs, outputs, params;
};
// `connectedInputs` / `connectedOutputs` are the old module's connectors that
// carry a cable, in ascending order: only those are matched (a connected input
// also carries its attenuator parameter along).
Mapping mapping(u8 oldType, u8 newType, std::span<const u8> connectedInputs,
                std::span<const u8> connectedOutputs);

// ---- Edit --------------------------------------------------------------------

// Replaces module `index` of area `loc` by a new module of type `newType`, as
// the original editor does (CPatch::InternalReplaceModule @ 000d7a14):
// - the new module takes the next free index, the old one's position and
//   colour, and default modes, lock flag and parameter labels;
// - parameter values (all variations) are copied through the mapping when
//   both parameters have the same range, otherwise the default stays;
// - cables are moved to the corresponding connectors; a cable through an
//   input that has no counterpart is bridged to that input's links; cables on
//   other unmatched connectors are dropped; nets fed by the old module's
//   outputs take the new output's colour (white when the output is gone);
// - morph, knob and MIDI controller assignments follow mapped parameters and
//   are dropped otherwise;
// - a default name ("OscA2") becomes the new type's default name, a custom
//   name is kept; modules the new one overlaps are pushed down, and uprates
//   are recomputed.
// Returns the new module's index. Throws std::invalid_argument if the module
// does not exist or canReplace() is false.
u8 replaceModule(Patch& patch, Location loc, u8 index, u8 newType);

} // namespace g2::replace
