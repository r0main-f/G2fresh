// A module's pop-up menu, as Clavia's editor v1.62 has it (SMNU "Module": Cut 202, Copy 203, Paste 204, Paste Params
// 207, Rename 1010, Assign 1019, Global Assign 1009, Exclude From Mutation 1013, Help 1011, Delete 1012;
// CPanel::DoContextMenu @000bf466 ticks Exclude From Mutation from CModule::IsLocked).
#include <catch2/catch_test_macros.hpp>

#include "ModuleMenu.h"

#include <string>
#include <vector>

using namespace g2ui;

namespace {
std::vector<std::string> labels(const std::vector<ModuleMenuItem>& items)
{
    std::vector<std::string> out;
    for (const auto& i : items)
        out.push_back(i.label);
    return out;
}
} // namespace

TEST_CASE("module menu: the original's items in its order, then G2fresh's own")
{
    const auto items = moduleMenu({});
    // Global Assign needs the performance's knobs (not built yet), Help Clavia's text (not shipped).
    CHECK(labels(items) == std::vector<std::string>{"Cut", "Copy", "Paste", "Paste Params", "Rename...", "Assign",
                                                    "Exclude From Mutation", "Delete", "-", "Replace With",
                                                    "Colour", "Duplicate"});
}

TEST_CASE("module menu: what is enabled and ticked")
{
    ModuleMenuState s;
    s.clipboardFull = false;
    s.excluded = true;
    s.several = true;
    const auto items = moduleMenu(s);
    auto find = [&](ModuleMenuItem::Id id) {
        for (const auto& i : items)
            if (i.id == id)
                return i;
        return ModuleMenuItem{};
    };
    CHECK_FALSE(find(ModuleMenuItem::Id::Paste).enabled);
    CHECK_FALSE(find(ModuleMenuItem::Id::PasteParams).enabled);
    CHECK(find(ModuleMenuItem::Id::Exclude).ticked);
    CHECK_FALSE(find(ModuleMenuItem::Id::Rename).enabled); // one module at a time
    CHECK(find(ModuleMenuItem::Id::Cut).label == "Cut"); // the same labels for several modules
    s.clipboardFull = true;
    CHECK(moduleMenu(s)[2].enabled);
}

TEST_CASE("module menu: Assign's pages and columns")
{
    // CKnobAssignMenu::CreateNormalKnobAssignMenu @000bbc8e: "Page A".."Page E" ("(used)" if one of its three columns
    // is), each with "Multi Column (n knobs)" when the module has more than 8, then "Column 1".."Column 3" ("(used)").
    auto p = g2::Patch::makeDefault();
    const auto seq = g2::edit::addModule(p, g2::Location::Va, 121, 0, 0);
    g2::edit::assignKnob(p, 4 * 8 + 2, g2::Location::Va, seq, 0); // page B2
    const auto pages = assignMenu(p, *g2::db::find(121));
    REQUIRE(pages.size() == 5);
    CHECK(pages[0].label == "Page A");
    CHECK(pages[1].label == "Page B (used)");
    REQUIRE(pages[1].items.size() == 4);
    CHECK(pages[1].items[0].label == "Multi Column (21 knobs)");
    CHECK(pages[1].items[0].page == 3);
    CHECK(pages[1].items[0].multi);
    CHECK(pages[1].items[1].label == "Column 1");
    CHECK(pages[1].items[2].label == "Column 2 (used)");
    CHECK(pages[1].items[2].page == 4);
    CHECK_FALSE(pages[1].items[2].multi);
    CHECK(assignMenu(p, *g2::db::find(7))[0].items.size() == 3); // OscB: 8 parameters, no Multi Column
}
