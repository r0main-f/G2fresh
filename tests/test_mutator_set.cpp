// One Patch Mutator per patch, as the original's CMutaSynthData belongs to its CPatch (audit R4): each slot of a
// performance has its own, and a new or loaded document starts with fresh ones. G2fresh kept one per window, across
// loads, so a population could outlive its patch.
#include <catch2/catch_test_macros.hpp>

#include "MutatorSet.h"

#include "g2/edit.hpp"

using namespace g2;

TEST_CASE("mutator set: one per slot, fresh after a load")
{
    Patch p = Patch::makeDefault();
    edit::addModule(p, Location::Va, 7, 0, 0);
    g2ui::MutatorSet set;
    auto& a = set.get(0, 1, p);
    a.randomize(p, 0);
    REQUIRE(a.population[2].has_value());
    CHECK(&set.get(0, 1, p) == &a);                  // the same patch: the same Mutator
    CHECK_FALSE(set.get(1, 1, p).population[2]);     // another slot: its own
    CHECK(set.get(0, 1, p).population[2].has_value());
    CHECK_FALSE(set.get(0, 2, p).population[2]);     // another document: fresh
}
