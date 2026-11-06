#pragma once
// The patch background's pop-up, as Clavia's editor v1.62 builds it (CPatchBackground::CPatchBkgContextMenu
// @000e06b2; manual p68): Cut, Copy, Paste; Insert, a submenu per module group with its modules (one the area cannot
// take greyed); then the synth: a patch's slots, Slot A to D and Local (AddSendToSlotMenus @000dfa62: sent to that
// slot, or taken off the synth), or a performance's Download / Disconnect Performance. Plain C++.

#include "g2/module_db.hpp"
#include "g2/patch.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace g2ui {

struct BackgroundItem {
    std::string label;
    bool enabled = true;
    bool ticked = false;
};

inline std::vector<BackgroundItem> editItems(bool selection, bool clipboardFull)
{
    return {{"Cut", selection}, {"Copy", selection}, {"Paste", clipboardFull}};
}

struct InsertModule {
    std::uint8_t type = 0;
    std::string label;
    bool enabled = true;
};
struct InsertGroup {
    std::string name;
    std::vector<InsertModule> modules;
};

// The module bar's groups and modules, in its order (ModuleDef::browserOrder); greyed where the area cannot take them
// (ModuleDef::contexts, the original's IsValidContext). The label is the module's long name (not checked against the
// original's menu).
inline std::vector<InsertGroup> insertGroups(g2::Location loc)
{
    std::vector<InsertGroup> groups;
    const auto cats = g2::db::categories();
    for (std::size_t c = 0; c < cats.size(); ++c) {
        InsertGroup g{cats[c].name, {}};
        std::vector<const g2::db::ModuleDef*> defs;
        for (const auto& def : g2::db::modules())
            if (def.selectable && def.kind == g2::db::ModuleKind::Module && def.category == static_cast<int>(c))
                defs.push_back(&def);
        std::stable_sort(defs.begin(), defs.end(), [](auto* a, auto* b) { return a->browserOrder < b->browserOrder; });
        for (const auto* def : defs)
            g.modules.push_back({def->typeId, def->longName, (def->contexts & (1u << static_cast<unsigned>(loc))) != 0});
        groups.push_back(std::move(g));
    }
    return groups;
}

// `synth`: a synth is connected; `performance`: the document is one; `performanceOn`: it is on the synth;
// `slot`: the synth slot the patch is on (-1: none).
inline std::vector<BackgroundItem> synthItems(bool synth, bool performance, bool performanceOn, int slot)
{
    if (!synth)
        return {};
    if (performance)
        return {{performanceOn ? "Disconnect Performance" : "Download Performance"}};
    std::vector<BackgroundItem> items;
    for (int s = 0; s < 4; ++s)
        items.push_back({std::string("Slot ") + static_cast<char>('A' + s), true, s == slot});
    items.push_back({"Local", true, slot < 0});
    return items;
}

} // namespace g2ui
