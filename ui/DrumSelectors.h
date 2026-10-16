#pragma once
// The DrumSynth preset selectors' state (g2::special::DrumPresetSelector, one per module), kept as the original keeps
// it in each CPnlDrumPresetSelector (id 46): it lives with its module's panel, so it goes with the module, and with
// every panel when a patch is loaded or made. Plain C++.

#include "g2/patch.hpp"
#include "g2/special.hpp"

#include <map>

namespace g2ui {

// `replaced`: the document was loaded or made since the last call; then nothing is kept.
template <class Selector>
void pruneDrumSelectors(std::map<g2::u8, Selector>& selectors, const g2::Area& area, bool replaced)
{
    if (replaced) {
        selectors.clear();
        return;
    }
    for (auto it = selectors.begin(); it != selectors.end();) {
        const auto* m = area.find(it->first);
        if (m == nullptr || m->type != g2::special::kDrumSynthType)
            it = selectors.erase(it);
        else
            ++it;
    }
}

} // namespace g2ui
