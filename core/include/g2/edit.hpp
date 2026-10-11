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

// The colour the editor gives a cable leaving `from`: the connector's colour,
// with blue/red and yellow/orange connectors following the module's uprate.
CableColor cableColor(const Patch& patch, Location loc, Endpoint from);

// Connects an output (or, for a link, an input) to an input. Returns the new cable.
Cable connect(Patch& patch, Location loc, Endpoint from, Endpoint to);
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

// The morph assignment of a parameter in a variation: group 0..7 and signed
// range -128..127 (fraction of the parameter's full range), or nullopt.
struct Morph {
    u8 group = 0;
    std::int8_t range = 0;
};
std::optional<Morph> morphOf(const Patch& patch, u8 variation, Location loc, u8 module, u8 param);
// Assigns (or re-assigns) a parameter to a morph group with a range in one
// variation; a parameter belongs to at most one group per variation.
void setMorph(Patch& patch, u8 variation, Location loc, u8 module, u8 param, u8 group, std::int8_t range);
void clearMorph(Patch& patch, u8 variation, Location loc, u8 module, u8 param);

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
void setCableColor(Patch& patch, Location loc, const Cable& cable, CableColor color);
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

// Modules copied with the cables between them, their morph assignments (all
// variations), labels and names. Positions are relative to the group's top-left.
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
