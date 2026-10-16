// The DrumSynth preset selectors' state lives with the module, as the original's CPnlDrumPresetSelector (id 46) lives
// with its panel: gone with the module or when the document is replaced. Audit item S2: it was keyed by module
// index and never cleared, so a selector could carry its last preset to another patch's DrumSynth.
#include <catch2/catch_test_macros.hpp>

#include "DrumSelectors.h"

#include "g2/edit.hpp"
#include "g2/special.hpp"

using namespace g2;

TEST_CASE("drum selectors: kept for the DrumSynths still there, dropped otherwise")
{
    Patch p = Patch::makeDefault();
    const u8 drum = edit::addModule(p, Location::Va, special::kDrumSynthType, 0, 0);
    const u8 other = edit::addModule(p, Location::Va, 19, 1, 0); // a Mix4-1B
    std::map<u8, int> selectors{{drum, 3}, {other, 5}, {99, 7}};
    g2ui::pruneDrumSelectors(selectors, p.va, false);
    CHECK(selectors == std::map<u8, int>{{drum, 3}}); // not a DrumSynth, not there: gone
    // A new or loaded document: every selector starts again.
    g2ui::pruneDrumSelectors(selectors, p.va, true);
    CHECK(selectors.empty());
}
