#pragma once
// A module's pop-up menu, as Clavia's editor v1.62 has it (SMNU "Module"; CPanel::DoContextMenu @000bf466): Cut, Copy,
// Paste, Paste Params, Rename, Assign, Global Assign, Exclude From Mutation (ticked for a module that is,
// CModule::IsLocked), Help, Delete. Assign and Global Assign wait for the knob assignments, and Help for text of
// G2fresh's own (Clavia's cannot ship). G2fresh's own items follow: Replace With (the original's Replace button),
// Colour (its toolbar's colour selector), Duplicate. Plain C++.

#include <string>
#include <vector>

namespace g2ui {

struct ModuleMenuItem {
    enum class Id { None, Cut, Copy, Paste, PasteParams, Rename, Exclude, Delete, Separator, Replace, Colour, Duplicate };
    Id id = Id::None;
    std::string label;
    bool enabled = true;
    bool ticked = false;
};

struct ModuleMenuState {
    bool clipboardFull = false; // modules copied
    bool excluded = false;      // the clicked module is excluded from mutation
    bool several = false;       // several modules selected
};

inline std::vector<ModuleMenuItem> moduleMenu(const ModuleMenuState& s)
{
    using Id = ModuleMenuItem::Id;
    return {
        {Id::Cut, "Cut"},
        {Id::Copy, "Copy"},
        {Id::Paste, "Paste", s.clipboardFull},
        {Id::PasteParams, "Paste Params", s.clipboardFull},
        {Id::Rename, "Rename...", !s.several},
        {Id::Exclude, "Exclude From Mutation", true, s.excluded},
        {Id::Delete, "Delete"},
        {Id::Separator, "-"},
        {Id::Replace, "Replace With", !s.several},
        {Id::Colour, "Colour"},
        {Id::Duplicate, "Duplicate"},
    };
}

} // namespace g2ui
