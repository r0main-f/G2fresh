// Edit operations on a Patch. Each one validates its arguments against the
// module database and keeps the patch consistent (e.g. removing a module also
// removes its cables, morph, knob and controller assignments). They throw
// std::invalid_argument on a request the original editor would refuse.
#pragma once

#include "g2/patch.hpp"

#include <string>

namespace g2::edit {

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
void renameModule(Patch& patch, Location loc, u8 index, const std::string& name);

// The colour the editor gives a cable leaving `from`: the connector's colour,
// with blue/red and yellow/orange connectors following the module's uprate.
CableColor cableColor(const Patch& patch, Location loc, Endpoint from);

// Connects an output (or, for a link, an input) to an input. Returns the new cable.
Cable connect(Patch& patch, Location loc, Endpoint from, Endpoint to);
void disconnect(Patch& patch, Location loc, const Cable& cable);

// Sets a parameter in one variation, clamped to the parameter's range.
void setParam(Patch& patch, Location loc, u8 module, u8 param, u8 variation, u8 value);
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

// The 8 morph groups' names (at most 7 characters), e.g. "Wheel".
std::string morphLabel(const Patch& patch, int group);
void setMorphLabel(Patch& patch, int group, const std::string& label);

} // namespace g2::edit
