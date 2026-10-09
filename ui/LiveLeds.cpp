#include "LiveLeds.h"

#include "ModulePainter.h"

#include <algorithm>

namespace g2ui {

std::vector<bool> ledGroups(const PanelDef& panel)
{
    // CPanel::GetLedGroupCnt counts groups 0, 1, 2... in the order the
    // panel's LEDs were created, which is group order: some PANL files list
    // them otherwise (Gate, 2-In, 4-In, Mix4-1S), and Verhue's editor, used
    // with real synths, gives every group its own entry in group order.
    // IsMutliLedGroup: more than one LED, or a VU meter.
    std::vector<const PanelElement*> leds;
    for (const auto& e : panel.elements)
        if (e.kind == "Led" || e.kind == "MiniVU")
            leds.push_back(&e);
    int count = 0;
    for (const auto* e : leds)
        count = std::max(count, e->groupId + 1);
    std::vector<bool> multi(static_cast<std::size_t>(count), false);
    for (int g = 0; g < count; ++g) {
        int weight = 0;
        for (const auto* e : leds)
            if (e->groupId == g)
                weight += e->kind == "MiniVU" ? 0x1267 : 1;
        multi[static_cast<std::size_t>(g)] = weight > 1;
    }
    return multi;
}

LedMap ledMap(const g2::Patch& patch)
{
    LedMap map;
    for (const auto loc : {g2::Location::Va, g2::Location::Fx}) {
        std::vector<const g2::Module*> modules;
        for (const auto& m : patch.area(loc).modules)
            modules.push_back(&m);
        std::sort(modules.begin(), modules.end(), [](auto* a, auto* b) { return a->index < b->index; });
        for (const auto* m : modules) {
            const auto* panel = ModulePainter::panelFor(*m);
            if (!panel)
                continue;
            const auto groups = ledGroups(*panel);
            for (std::size_t g = 0; g < groups.size(); ++g) {
                auto& list = groups[g] ? map.multi : map.single;
                if (list.size() < 40)
                    list.push_back({loc, m->index, static_cast<int>(g)});
            }
        }
    }
    return map;
}

} // namespace g2ui
