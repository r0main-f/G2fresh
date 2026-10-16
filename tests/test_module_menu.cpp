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
    // Assign and Global Assign need the knob assignments (not built yet), Help Clavia's text (not shipped).
    CHECK(labels(items) == std::vector<std::string>{"Cut", "Copy", "Paste", "Paste Params", "Rename...",
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
