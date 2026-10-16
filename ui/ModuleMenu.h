#pragma once
// A module's pop-up menu, as Clavia's editor v1.62 has it (SMNU "Module"; CPanel::DoContextMenu @000bf466): Cut, Copy,
// Paste, Paste Params, Rename, Assign, Global Assign, Exclude From Mutation (ticked for a module that is,
// CModule::IsLocked), Help, Delete. Global Assign waits for the performance's knobs, and Help for text of G2fresh's
// own (Clavia's cannot ship). G2fresh's own items follow: Replace With (the original's Replace button),
// Colour (its toolbar's colour selector), Duplicate. Plain C++.

#include "g2/edit.hpp"
#include "g2/module_db.hpp"
#include "g2/patch.hpp"

#include <string>
#include <vector>

namespace g2ui {

struct ModuleMenuItem {
    enum class Id { None, Cut, Copy, Paste, PasteParams, Rename, Assign, Exclude, Delete, Separator, Replace, Colour, Duplicate };
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
        {Id::Assign, "Assign", !s.several},
        {Id::Exclude, "Exclude From Mutation", true, s.excluded},
        {Id::Delete, "Delete"},
        {Id::Separator, "-"},
        {Id::Replace, "Replace With", !s.several},
        {Id::Colour, "Colour"},
        {Id::Duplicate, "Duplicate"},
    };
}

// Assign's submenu (CKnobAssignMenu::CreateNormalKnobAssignMenu @000bbc8e): a page per letter, "(used)" when one of
// its three columns holds an assignment (CKnobMap::IsPageUsed); in it "Multi Column (n knobs)" when the module has
// more than 8 parameters to assign, then its three columns. `page` is the knob page an item assigns from (0..14).
struct AssignItem {
    std::string label;
    int page = 0;
    bool multi = false;
};
struct AssignPage {
    std::string label;
    std::vector<AssignItem> items;
};
inline std::vector<AssignPage> assignMenu(const g2::Patch& patch, const g2::db::ModuleDef& def)
{
    const auto count = g2::edit::assignableParams(def).size();
    std::vector<AssignPage> pages;
    for (int letter = 0; letter < 5; ++letter) {
        const int first = letter * 3;
        bool used = false;
        for (int c = 0; c < 3; ++c)
            used = used || g2::edit::isKnobPageUsed(patch, first + c);
        AssignPage page{std::string("Page ") + static_cast<char>('A' + letter) + (used ? " (used)" : ""), {}};
        if (count > static_cast<std::size_t>(g2::edit::kKnobsPerPage))
            page.items.push_back({"Multi Column (" + std::to_string(count) + " knobs)", first, true});
        for (int c = 0; c < 3; ++c)
            page.items.push_back({"Column " + std::to_string(c + 1) + (g2::edit::isKnobPageUsed(patch, first + c) ? " (used)" : ""),
                                  first + c, false});
        pages.push_back(std::move(page));
    }
    return pages;
}

} // namespace g2ui
