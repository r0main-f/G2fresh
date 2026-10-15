// Edit operations on a Patch. Each one validates its arguments against the
// module database and keeps the patch consistent (e.g. removing a module also
// removes its cables, morph, knob and controller assignments). They throw
// std::invalid_argument on a request the original editor would refuse.
#pragma once

#include "g2/patch.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace g2::edit {

// ---- Names ------------------------------------------------------------------------

// G2 names (patches, performance slots, modules: 16 characters; parameter
// and morph labels: 7) use one byte per character, from a fixed set:
// space, a-z, A-Z, 0-9 and -!"#$%&'()*+,./:;<=>?@[\]^_`{|} (no ~), as
// NameUtils::IsModularChar @0002989c checks.
inline constexpr std::size_t kNameLength = 16;
inline constexpr std::size_t kLabelLength = 7;
inline constexpr std::string_view kModularChars =
    " abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-!\"#$%&'()*+,./:;<=>?@[\\]^_`{|}";
bool isModularChar(char c);
// Utils::RemoveNonModularChars @001465e8 on typed (UTF-8) text: every
// character outside the set becomes a space (one per character, whatever its
// UTF-8 length; bytes that are not UTF-8 count one each), then the first
// `maxLength` characters. The result is plain ASCII.
std::string modularName(std::string_view text, std::size_t maxLength = kNameLength);
// The same on raw bytes, as the original filters names it reads from a file
// (CModule::SetName): each byte outside the set becomes a space.
std::string modularBytes(std::string_view bytes);
// Utils::MakeFileNameFromPatchName @00146806 / RemoveNonFileChars @0014663e:
// the file name the editor suggests for a patch or performance name; every
// character outside space !#$%&'()+,-.0-9;=@A-Z[]^_`a-z{} becomes a space.
std::string fileNameForPatch(std::string_view name);
// Names a loaded patch carries, filtered as the original reads them: module
// names (CModule::SetName). Returns whether anything changed.
bool filterLoadedNames(Patch& patch);

struct Endpoint {
    u8 module = 0;
    u8 conn = 0;
    bool isOutput = true;
};

// Adds a module with default parameters and modes, the lowest free index and
// a default name ("OscB1", "OscB2", ...). Returns the new module's index
// (references into the module list are invalidated by later additions).
u8 addModule(Patch& patch, Location loc, u8 type, u8 col, u8 row);
// addModule, then resolveOverlaps: a module dropped at (col, row). (The
// original makes room before it creates the module.)
u8 dropModule(Patch& patch, Location loc, u8 type, u8 col, u8 row);
// Deletes a module (Edit > Clear, Delete). Its cables go and the chains
// through it are spliced back together (Out->A->X->B minus X: Out->A->B);
// a chain whose output goes keeps its cables, white
// (CPatchData::GenerateDeleteSelectionMolecules @000e7fe4; manual p69, p81).
void removeModule(Patch& patch, Location loc, u8 index);
// Moves a module, then pushes any module it now overlaps down its column (as
// the original editor does).
void moveModule(Patch& patch, Location loc, u8 index, u8 col, u8 row);
// Pushes modules down so none overlap; `keep` (if non-zero) stays in place.
void resolveOverlaps(Patch& patch, Location loc, u8 keep = 0);
// The first free row in a column, below every module already there.
u8 freeRow(const Patch& patch, Location loc, u8 col);
// Renames a module (at most 16 characters, filtered by modularName as
// CModule::SetName does).
void renameModule(Patch& patch, Location loc, u8 index, const std::string& name);

// ---- Cables -------------------------------------------------------------------
//
// The cables of a net form a tree rooted at its output (g2/cables.hpp). The
// operations below are the original editor's (CPatchData::Get*Molecules):
// each one is a single edit (one undo step), keeps every net to at most one
// output and no loop, and leaves the cable list as the original would save
// it. A serial chain (the branch of a net's root holding a connector) has one
// colour; a chain without an output is white.

// The colour of an output's cables (CPatchData::MapConnectorColorToCableColor):
// the connector's colour, blue/red and yellow/orange following the module's uprate.
CableColor cableColor(const Patch& patch, Location loc, Endpoint from);

// What dropping a cable dragged from `source` does
// (CCableDragDropManager::GetCurrentDropAction @0001939c). A plain drag
// connects when the two connectors are in different nets and at most one of
// them has an output. A move (Option at the drop, or a drag started with a
// double click) moves every cable of `source` onto `target` when they are in
// different nets, `source` has cables, and `source` is an output or at most
// one net has an output; dropped on nothing, it disconnects `source`.
enum class DropAction { None, Connect, Move, Disconnect };
DropAction dropAction(const Patch& patch, Location loc, Endpoint source, std::optional<Endpoint> target, bool move);

// Connects two connectors, either of which may be an output (`from` is where
// the drag started; CPatchData::GetNewCableMolecules @000eb87c). Throws when
// dropAction refuses it. The cable takes the colour of the side with an
// output (the output's colour, or the input's cable colour) and the other
// net takes it too; between two nets without an output it is white. Returns
// the new cable as stored.
Cable connect(Patch& patch, Location loc, Endpoint from, Endpoint to);
// Jack popup "Disconnect" (GetDisconnectCableNodeMolecules @000ea48a):
// removes the connector's cables and splices its neighbours together (the
// parent takes its children; a root's first child takes the others). The
// rest of an output's chain stays connected, white. False if it has no cable.
bool disconnectJack(Patch& patch, Location loc, Endpoint jack);
// Jack popup "Break" (GetBreakCableNodeMolecules @000eb27a): on an input,
// removes the cable towards the output and turns what hangs below it white;
// on an output, removes its cables and turns the rest white. Only in a net
// with an output; false when nothing changes.
bool breakJack(Patch& patch, Location loc, Endpoint jack);
// Jack popup "Delete" (GetDeleteCableChainMolecules @000e41d6): deletes the
// connector's serial chain, or every cable of the net from its output. Like
// Color and Delete Unused Cables, and unlike the other cable edits, it does
// not recompute the modules' rates (the original has no uprate pass there:
// a module the chain made audio-rate stays so until the next cable edit).
bool deleteChain(Patch& patch, Location loc, Endpoint jack);
// Jack popup "Color" (GetRecolorCableMolecules @000e43f8): recolours the
// connector's serial chain (the whole net from its output). Only in a net
// with an output and not white (the popup's six colours): throws otherwise.
void recolorChain(Patch& patch, Location loc, Endpoint jack, CableColor color);
// Jack popup "Delete Unused Cables" (CPatch::InternalDeleteUnusedCables
// @000d7168): deletes every net without an output, in both areas. False if
// there was none.
bool deleteUnusedCables(Patch& patch);
// Moves every cable of `from` onto `to` (GetMoveCableNodeMolecules
// @000eab6c), recolouring as a connection would. Throws when dropAction
// refuses the move.
void moveCables(Patch& patch, Location loc, Endpoint from, Endpoint to);

// For the jack popup and highlighting.
bool isCabled(const Patch& patch, Location loc, Endpoint jack);  // in a net
bool hasOutput(const Patch& patch, Location loc, Endpoint jack); // its net has an output
// "Break" is offered on a cabled connector that has cables below it
// (CPanel::HoleDoContextMenu @000c006e: not on a leaf).
bool canBreak(const Patch& patch, Location loc, Endpoint jack);
// The cables of a connector's serial chain (what Color and Delete act on,
// and what holding a jack highlights: CPatchBackground::CableHighLight @000def5e).
std::vector<Cable> chainCables(const Patch& patch, Location loc, Endpoint jack);
// The end of a cable farther from the net's root: the connector whose chain
// holds the cable (cord actions go through it).
Endpoint lowerEnd(const Patch& patch, Location loc, const Cable& cable);

// Removes one cable, splicing nothing and recolouring nothing. Not an action
// of the original editor (which only has the ones above); for tools and tests.
void disconnect(Patch& patch, Location loc, const Cable& cable);

// Sets a parameter in one variation, clamped to the parameter's range.
void setParam(Patch& patch, Location loc, u8 module, u8 param, u8 variation, u8 value);
// Whether a parameter is a push button's (db::ParamDef::momentary): 1 while
// held, 0 on release, never left at 1 and not an undo step (CPanel::CtrlRelease).
bool isMomentary(const Patch& patch, Location loc, u8 module, u8 param);
void setMode(Patch& patch, Location loc, u8 module, u8 mode, u8 value);

// Display text of a parameter, as the original editor shows it.
std::string paramText(const Patch& patch, Location loc, u8 module, u8 param, u8 variation);

// ---- Patch settings -------------------------------------------------------

// The patch-settings pseudo modules (file location 2), by index.
enum class Setting : u8 { Morph = 1, Gain = 2, Glide = 3, Bend = 4, Vibrato = 5, Arpeggiator = 6, Misc = 7 };
const db::ModuleDef* settingDef(Setting s);
u8 settingValue(const Patch& patch, Setting s, u8 param, u8 variation);
void setSetting(Patch& patch, Setting s, u8 param, u8 variation, u8 value);
std::string settingText(const Patch& patch, Setting s, u8 param, u8 variation);

// Voices: poly with 1..32 voices, or mono / legato (one voice).
enum class VoiceMode : u8 { Poly = 0, Mono = 1, Legato = 2 };
void setVoices(Patch& patch, VoiceMode mode, u8 polyVoices = 0); // count used for Poly
std::string voicesText(const Patch& patch);                   // "Mono", "Legato" or the count
void setCategory(Patch& patch, u8 category);                   // 0..15
const char* categoryName(u8 category);

// ---- Morphs, knob and MIDI assignments, labels -----------------------------

// Morph assignments are keyed by (location, module, parameter, group), as
// the original's morph map (CMorphMapData_11, CMorphSpec::operator<
// @00064754): a parameter can be in several groups of a variation, each with
// its own range (manual p91). The range is signed, in parameter units: the
// morph takes the value to value + range, shown clipped to [0, max]
// (CModule::GetMorphAbsValue @000aa276). A range of 0 is no assignment (the
// original's morph change with range 0 removes it, CPatch::HandleMorphChange).
struct Morph {
    u8 group = 0;
    std::int8_t range = 0;
};
// Every group a parameter is in, in group order.
std::vector<Morph> morphsOf(const Patch& patch, u8 variation, Location loc, u8 module, u8 param);
// The parameter's range in one group, or nullopt when it is not in it.
std::optional<std::int8_t> morphRange(const Patch& patch, u8 variation, Location loc, u8 module, u8 param, u8 group);
// The parameter definition's flag (CBuildModule::IsParamMorphable @000a9f4c,
// db::ParamDef::morphable): the editor morphs no other parameter.
bool isMorphable(const Patch& patch, Location loc, u8 module, u8 param);
// At most 25 parameters per variation take morphs, whatever the number of
// groups each is in (manual p91; GetNumberOfMorphedParams @00069246 counts
// parameters, not assignments).
inline constexpr int kMaxMorphedParams = 25;
int morphedParamCount(const Patch& patch, u8 variation);
// Whether the editor lets a parameter take a morph in a variation: it is
// morphable, and already morphed there or fewer than 25 parameters are
// (CControlMenu::InitMenus @0008fcbe, CPanel::CtrlRequestMorphChange @000c19b2).
bool canMorph(const Patch& patch, u8 variation, Location loc, u8 module, u8 param);
// The range a morph drag can give: value + range within [0, max] for the
// parameter's value in the variation (CPnlControl::ClickDragIdleHandler
// @000c8d74, CPnlKnobABC::CircularIdleHandler @000cbcf8).
std::int8_t clampMorphRange(const Patch& patch, u8 variation, Location loc, u8 module, u8 param, int range);
// Sets a parameter's range in one group of a variation, adding the
// assignment if needed (CPanel::CtrlRequestMorphChange); the range is clamped
// by clampMorphRange, and 0 removes the assignment. Throws for a parameter
// canMorph refuses. Other groups are unchanged.
void setMorph(Patch& patch, u8 variation, Location loc, u8 module, u8 param, u8 group, int range);
// Removes a parameter from one group (CMorphMap::GetDeassignMorphMolecules @000f1d58).
void clearMorph(Patch& patch, u8 variation, Location loc, u8 module, u8 param, u8 group);
// The parameter popup's "Morph assign" (CControlMenu::HandleMenuSelection
// @0008f5c6), for the focused group: a parameter not in it joins it with
// value 0 and range +max; one in it leaves it. Returns whether the parameter
// is now in the group. Throws for a parameter canMorph refuses.
bool toggleMorphAssign(Patch& patch, u8 variation, Location loc, u8 module, u8 param, u8 group);
// The morph dial's popup (CMorphControlMenu::HandleMenuSelection @0012523c),
// in one variation. Clear Group: every parameter leaves `group`. Copy To:
// every parameter of `from` joins `to` with the same range (replacing its
// range there); `from` keeps them.
void clearMorphGroup(Patch& patch, u8 variation, u8 group);
void copyMorphGroup(Patch& patch, u8 variation, u8 from, u8 to);
// The same as setMorph without the editor's rules (morphable flag, limit,
// clamp), for mirrors of what a synth has (CPatch::HandleMorphChange): range
// 0 removes. Throws only for a parameter the patch does not have.
void storeMorph(Patch& patch, u8 variation, Location loc, u8 module, u8 param, u8 group, int range);
// What the original does to a morph map it loads (file or synth): a repeated
// (parameter, group) entry is skipped, the first kept
// (CMorphMapData_11::ReadStream @00069326), and assignments to parameters
// the patch does not have are dropped (CMorphMap::ValidateAndRepairMap
// @000f1c7c, reported as "Morph assignment problem"). Returns whether
// anything of the second kind was dropped.
bool repairMorphs(Patch& patch);

// The 120 assignable knobs: 5 pages x 3 sub-pages (A-C) x 8 knobs.
inline constexpr int kKnobsPerPage = 8;
std::optional<int> knobOf(const Patch& patch, Location loc, u8 module, u8 param);
void assignKnob(Patch& patch, int knob, Location loc, u8 module, u8 param); // replaces the knob's previous target
void clearKnob(Patch& patch, int knob);
std::string knobName(int knob); // "1A-3": page 1, sub-page A, knob 3

// MIDI controllers a patch can assign: 0..119 except 0, 1, 11, 18, 32, 64,
// 70, 96, 97 (MIDICtrl::IsValid @00148754). CC 7 and 17 are pre-assigned to
// the patch volume and octave shift (MIDICtrl::IsPreAssigned @0014872e,
// manual p90, p126-127): a parameter holding one of them shows no MIDI item,
// and no other parameter can take them.
bool isValidMidiCc(u8 cc);
bool isPreAssignedMidiCc(u8 cc);
// Whether the editor offers a parameter a MIDI controller: the parameter is
// MIDI-assignable (db::ParamDef::midiAssignable) and does not hold a
// pre-assigned CC (CControlMenu removes the MIDI item otherwise).
bool canAssignMidiCc(const Patch& patch, Location loc, u8 module, u8 param);

std::optional<u8> midiCcOf(const Patch& patch, Location loc, u8 module, u8 param);
// One parameter per CC and one CC per parameter. Throws on an invalid or
// pre-assigned CC, or a parameter canAssignMidiCc refuses.
void assignMidiCc(Patch& patch, u8 cc, Location loc, u8 module, u8 param);
// Removes an assignment (not a pre-assigned one: throws).
void clearMidiCc(Patch& patch, u8 cc);
// The same without the editor's rules, for mirrors of what a synth has.
void storeMidiCc(Patch& patch, u8 cc, Location loc, u8 module, u8 param);
void eraseMidiCc(Patch& patch, u8 cc);
// CCtrlMap::ValidateAndRepairMap @000f2392, run on every patch the original
// loads (file or synth): drops duplicate entries, entries whose parameter does
// not exist, and CCs that are neither valid nor pre-assigned. Returns whether
// anything was dropped (the original then reports "Ctrl assignment problem").
bool repairMidiCcs(Patch& patch);

// Parameter labels. Only label controls can be renamed (CanChangeName: the
// panels' CPnlLabelButton and CPnlLabelRadioButton, and the patch-settings
// morph knobs CPnlMorphKnob, whose labels are the morph group names).
bool canRenameParam(const Patch& patch, Location loc, u8 module, u8 param);
// The label control of a module parameter (nullptr if it has none).
const db::LabelDef* labelControl(const Patch& patch, Location loc, u8 module, u8 param);
// A label control's captions (one per button): the patch's custom labels, or
// the panel's defaults. Empty for a parameter without a label control.
std::vector<std::string> paramLabels(const Patch& patch, Location loc, u8 module, u8 param);
// The first caption, or "" without a label control (morph knobs: the group name).
std::string paramLabel(const Patch& patch, Location loc, u8 module, u8 param);
// Renames caption `button` of a label control (at most 7 characters,
// filtered by modularName; "" restores the panel's caption). The module's
// custom data is written as the original's CPanel::GetCustomData does: one
// record per label control in panel order, [1, 8, param, 7 chars] for a
// button and [1, n*7+1, param, n x 7 chars] for n radio buttons. Throws for a
// parameter canRenameParam refuses.
void setParamLabel(Patch& patch, Location loc, u8 module, u8 param, const std::string& label, int button = 0);
// The custom data the original gives a new module with label controls (its
// panel captions), or nullopt for a module without.
std::optional<std::vector<u8>> defaultLabelData(u8 moduleType);

void setModuleColor(Patch& patch, Location loc, u8 module, u8 color);
// The Patch Mutator's lock (excluded from randomizing).
void setLocked(Patch& patch, Location loc, u8 module, bool locked);
void setCablesVisible(Patch& patch, CableColor color, bool visible);
// The editor-only shape of a cable (the cable with the same two ends);
// nullopt returns it to the automatic curve.
void setCableBend(Patch& patch, Location loc, const Cable& cable, std::optional<CableBend> bend);
// Returns every cable of both areas to the automatic curve.
void clearCableBends(Patch& patch);
bool hasCableBends(const Patch& patch);

// Copies every value of a variation (modules, settings, morphs) to another.
// Variation 8 is the "init" variation.
void copyVariation(Patch& patch, u8 from, u8 to);

// The audition variation: the original's patches have a tenth variation
// (index 9), never saved in files, where the Patch Mutator plays the
// individual the user clicks (CMutaSynthData::GetFocusIndividMolecules
// @00142ef8), leaving variations 1-8 untouched. Adds it to every module,
// patch setting and the morph map (a copy of the init variation) if the patch
// does not have it yet. Patch::toSections leaves it out.
void addAuditionVariation(Patch& patch);
bool hasAuditionVariation(const Patch& patch);
// The number of variations the patch holds in memory (9, or 10 with the
// audition variation): what new modules get.
std::size_t variationSlots(const Patch& patch);

// Module bar insertion (CTabButton::Action @0012698e): a module added from the
// module bar goes into the rightmost column of the selection, below the lowest
// selected module in that column; without a selection, at (0, 0).
std::pair<u8, u8> insertPosition(const Patch& patch, Location loc, const std::vector<u8>& selection);

// ---- Clipboard -----------------------------------------------------------------

// Modules copied with the cables between them (a chain through a module left out closes up, one that lost its
// output stays linked, white: CPatchData::AdaptForMergingWith @000e9cfc), their morph assignments (all
// variations), labels and names. Positions are relative to the group's top-left.
// Pasting adds the morphs while the variation has room: each pasted
// assignment counts one, and none is added once the count passes 25
// (CPatchData::GetNewModulesMolecules @000eba8e).
struct Clipboard {
    Location from = Location::Va;
    std::vector<Module> modules;
    std::vector<Cable> cables;
    std::vector<std::pair<u8, MorphAssign>> morphs; // (variation, assignment)
    bool empty() const { return modules.empty(); }
};
Clipboard copyModules(const Patch& patch, Location loc, const std::vector<u8>& indices);
// Pastes at (col, row), giving the modules new indices; returns them.
std::vector<u8> pasteModules(Patch& patch, Location loc, const Clipboard& clip, u8 col, u8 row);
// Deletes several modules as one selection (delete, cut), splicing as removeModule.
void removeModules(Patch& patch, Location loc, const std::vector<u8>& indices);
void moveModules(Patch& patch, Location loc, const std::vector<u8>& indices, int dCol, int dRow);

// The 8 morph groups' names (at most 7 characters), e.g. "Wheel".
std::string morphLabel(const Patch& patch, int group);

// ---- Knob and controller targets ----------------------------------------------

// What a knob or MIDI controller can control: a module parameter (VA or FX
// area) or a patch setting (location Settings, module = Setting, e.g. the
// morph dials are Setting::Morph parameters 0..7).
struct Target {
    Location location = Location::Va;
    u8 module = 0, param = 0;
    bool operator==(const Target&) const = default;
};
// The target's range (0..max), value in a variation and display text, or
// nullopt / 0 / "" when it does not exist.
std::optional<u8> targetValue(const Patch& patch, Target t, u8 variation);
int targetMax(const Patch& patch, Target t);
std::string targetText(const Patch& patch, Target t, u8 variation);
// "Osc1 Pitch", "Morph Wheel", "Glide Rate".
std::string targetName(const Patch& patch, Target t);
void setTargetValue(Patch& patch, Target t, u8 variation, u8 value); // clamped to the range
// The target of one of the 120 knobs, or nullopt when unassigned.
std::optional<Target> knobTarget(const Patch& patch, int knob);
// Renames a morph group (at most 7 characters, filtered by modularName).
void setMorphLabel(Patch& patch, int group, const std::string& label);

} // namespace g2::edit
