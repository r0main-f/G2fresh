// What an edit sends to a synth: the original editor's molecules, recorded by
// the edit operations as they change the patch.
//
// The original editor makes every user action a pair of bubbles (CUndoRedoPair
// @00018bdc): the redo bubble, which it sends to the synth and applies to its
// own patch (CPatch::UndoUserBubbleOutput @000d6492), and the undo bubble, the
// inverse of each molecule in reverse order (CUndoRedoPair::PushUndoMolecule
// @00018e20 pushes to the front), which Undo sends (CPatch::Undo @000d6442).
// While a Recorder lives on a thread, g2::edit's operations record those
// molecules here, protocol-neutral (g2::proto turns them into its molecules),
// each with its inverse: a cable edit's deletes, connects and recolours in
// the original's order, the uprate changes after them, a deleted module's
// re-creation for the undo, and so on.
#pragma once

#include "g2/cables.hpp"
#include "g2/file.hpp"
#include "g2/patch.hpp"

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace g2::live {

using cables::Connector;

struct Param { Location loc = Location::Va; u8 module = 0, param = 0, value = 0, variation = 0; };            // 40
struct Morph { Location loc = Location::Va; u8 module = 0, param = 0, group = 0; int range = 0; u8 variation = 0; }; // 43
struct Mode { Location loc = Location::Va; u8 module = 0, mode = 0, value = 0; };                              // 2B
struct KnobAssign { int knob = 0; Location loc = Location::Va; u8 module = 0, param = 0, assignType = 0; };    // 25
struct KnobDeassign { int knob = 0; };                                                          // 26
struct PageFocus { u8 page = 0; };                                                              // 2D
struct CtrlAssign { u8 cc = 0; Location loc = Location::Va; u8 module = 0, param = 0; };                       // 22
struct CtrlDeassign { u8 cc = 0; };                                                             // 23
struct VariationSelect { u8 variation = 0; };                                                   // 6A
struct VariationCopy { u8 from = 0, to = 0; };                                                  // 44
struct ModuleNew { Location loc = Location::Va; Module module; };                                              // 30
struct ModuleDelete { Location loc = Location::Va; u8 module = 0; };                                           // 32
struct ModuleMove { Location loc = Location::Va; u8 module = 0, col = 0, row = 0; };                           // 34
struct ModuleColor { Location loc = Location::Va; u8 module = 0, color = 0; };                                 // 31
struct ModuleName { Location loc = Location::Va; u8 module = 0; std::string name; };                           // 33
struct Uprate { Location loc = Location::Va; u8 module = 0; bool uprate = false; };                            // 2A
struct CustomData { Location loc = Location::Va; u8 module = 0; std::vector<u8> bytes; };                      // 42
struct MutaLock { Location loc = Location::Va; u8 module = 0; bool locked = false; };                          // 90
// 50 / 51 / 54. `dump`: the molecule is part of a module deletion, paste or
// replacement (the original's EPartOfDump, which clears the "not a dump" bit).
struct CableConnect { Location loc = Location::Va; Connector from, to; CableColor color = CableColor::Red; bool dump = false; };
struct CableDelete { Location loc = Location::Va; Connector from, to; bool dump = false; };
struct CableRecolor { Location loc = Location::Va; Connector from, to; CableColor color = CableColor::Red; };
// A patch section sent as an edit (52, 4D, 5A, 5B, 65), e.g. the new modules
// of a paste (CPatchData::GetNewModulesMolecules @000eba8e).
struct Section { file::Section section; };

using Op = std::variant<Param, Morph, Mode, KnobAssign, KnobDeassign, PageFocus, CtrlAssign, CtrlDeassign,
                        VariationSelect, VariationCopy, ModuleNew, ModuleDelete, ModuleMove, ModuleColor, ModuleName,
                        Uprate, CustomData, MutaLock, CableConnect, CableDelete, CableRecolor, Section>;

// One action: its molecules and their inverses.
class Edit {
public:
    // The molecules sent, in order.
    std::vector<Op> redo() const;
    // What undoes them: each molecule's inverse, the last molecule's first.
    std::vector<Op> undo() const;
    bool empty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    // The original's realtime pairs (CUndoRedoPair(true)): sent as realtime
    // messages, never answered.
    bool realtime = false;

    void add(Op redo, std::vector<Op> undo);
    // Moves the entries from `from` on to position `to` (as when the original
    // builds the make-room moves before a dropped module's).
    void moveTail(std::size_t from, std::size_t to);
    // Appends another action's entries (one undo step made of several).
    void append(const Edit& other);

private:
    struct Entry {
        Op redo;
        std::vector<Op> undo; // in the order sent
    };
    std::vector<Entry> entries_;
};

// Records what the edit operations do on this thread while it lives (nested
// recorders: the innermost records).
class Recorder {
public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    Edit& edit() { return edit_; }

private:
    Recorder* previous_;
    Edit edit_;
};

// The current thread's recording, or nullptr.
Edit* recording();
// Records a molecule and what undoes it (nothing when not recording).
void record(Op redo, Op undo);
void record(Op redo, std::vector<Op> undo);
void markRealtime();

// The sections of some modules of an area, as the original sends new modules
// (52 for the cables between them, 4D with their values in every variation
// the patch has, 5B for those with custom data, 5A), in that order; 5B is left
// out when none has custom data. The patch must hold the modules.
std::vector<file::Section> moduleSections(const Patch& patch, Location loc, const std::vector<u8>& modules);
// One module's parameter section (4D) in every variation.
file::Section paramSection(const Patch& patch, Location loc, const std::vector<u8>& modules);
// Every module's values in one variation, of an area or of the patch
// settings (4D), and the morph map of one variation (65), as the Patch
// Mutator sends an individual (CMMutaParamDump::WriteStream @00013a18).
file::Section variationParams(const Patch& patch, Location loc, u8 variation);
file::Section variationMorphs(const Patch& patch, u8 variation);
// Records the Mutator's dump of one variation as the original sends it
// (CMutaSynthData::GetFocusIndividMolecules @00142ef8, GetCopyIndividMolecules
// @00144528): 4D for the patch settings, VA and FX, then 65, with `after`'s
// values, undone by `before`'s.
void recordVariationDump(const Patch& before, const Patch& after, u8 variation);

} // namespace g2::live
