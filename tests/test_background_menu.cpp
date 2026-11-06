// The patch background's pop-up, as Clavia's editor v1.62 builds it (CPatchBackground::CPatchBkgContextMenu
// @000e06b2, AddSendToSlotMenus @000dfa62, DoContextMenu @000e2a4c; manual p68).
#include <catch2/catch_test_macros.hpp>

#include "BackgroundMenu.h"

#include "g2/module_db.hpp"

#include <string>
#include <vector>

using namespace g2ui;

TEST_CASE("background menu: Cut, Copy and Paste, as there is something to act on")
{
    // Cut and Copy want a selection, Paste a clipboard (IsSelectionEmpty, CClipboard::IsEmpty).
    const auto e = editItems(false, false);
    REQUIRE(e.size() == 3);
    CHECK(e[0].label == "Cut");
    CHECK(e[1].label == "Copy");
    CHECK(e[2].label == "Paste");
    CHECK_FALSE(e[0].enabled);
    CHECK_FALSE(e[2].enabled);
    CHECK(editItems(true, true)[0].enabled);
    CHECK(editItems(true, true)[2].enabled);
}

TEST_CASE("background menu: Insert lists every group's modules, in the module bar's order")
{
    // A submenu per category (CModuleFactory::GetNumberOfCategorys), its modules by index; one the area cannot take is
    // greyed (IsModuleAvailable).
    const auto groups = insertGroups(g2::Location::Va);
    REQUIRE(groups.size() == g2::db::categories().size());
    CHECK(groups[0].name == g2::db::categories()[0].name);
    for (const auto& g : groups)
        for (std::size_t i = 1; i < g.modules.size(); ++i)
            CHECK(g2::db::find(g.modules[i - 1].type)->browserOrder <= g2::db::find(g.modules[i].type)->browserOrder);
    bool fxInGreyedInVoice = false, fxInOkInFx = false;
    for (const auto& g : groups)
        for (const auto& m : g.modules)
            if (std::string(g2::db::find(m.type)->shortName) == "Fx-In")
                fxInGreyedInVoice = !m.enabled;
    for (const auto& g : insertGroups(g2::Location::Fx))
        for (const auto& m : g.modules)
            if (std::string(g2::db::find(m.type)->shortName) == "Fx-In")
                fxInOkInFx = m.enabled;
    CHECK(fxInGreyedInVoice);
    CHECK(fxInOkInFx);
}

TEST_CASE("background menu: the synth's slots for a patch, the performance for a performance")
{
    // AddSendToSlotMenus: Slot A..D, ticked where the patch is; Local when it is on none. A performance: Download
    // Performance, or Disconnect Performance when it is on the synth.
    CHECK(synthItems(false, false, false, -1).empty()); // no synth
    const auto patch = synthItems(true, false, false, 2);
    REQUIRE(patch.size() == 5);
    CHECK(patch[0].label == "Slot A");
    CHECK(patch[2].ticked);
    CHECK(patch[4].label == "Local");
    CHECK_FALSE(patch[4].ticked);
    CHECK(synthItems(true, false, false, -1)[4].ticked);
    const auto perf = synthItems(true, true, false, -1);
    REQUIRE(perf.size() == 1);
    CHECK(perf[0].label == "Download Performance");
    CHECK(synthItems(true, true, true, -1)[0].label == "Disconnect Performance");
}
