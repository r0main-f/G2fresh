// The patch window's arrow keys, as Clavia's editor v1.62 binds them (CPatchView::InitializeShortcutKeys
// @000fafd8, modifiers decoded by CShortcutKeys::GetMethod @001c81e2; manual p115).
#include <catch2/catch_test_macros.hpp>

#include "FocusKeys.h"

using namespace g2ui;
using Action = FocusKey::Action;
using g2::focus::Direction;

namespace {

FocusKey key(Arrow a, FocusMods m = {}, bool repeat = false, bool mutator = false)
{
    return focusKey(a, m, repeat, mutator);
}

} // namespace

TEST_CASE("arrow keys: up and down step the value, left and right walk the parameters, also when held")
{
    CHECK(key(Arrow::Up).action == Action::StepValue);
    CHECK(key(Arrow::Up).delta == +1);
    CHECK(key(Arrow::Down).delta == -1);
    CHECK(key(Arrow::Right).action == Action::NextParam);
    CHECK(key(Arrow::Right).delta == +1);
    CHECK(key(Arrow::Left).delta == -1);
    // Bound for the key's auto-repeat too (state 0x0B).
    CHECK(key(Arrow::Up, {}, true).action == Action::StepValue);
    CHECK(key(Arrow::Left, {}, true).action == Action::NextParam);
}

TEST_CASE("arrow keys: shift goes to the neighbouring module, once per press")
{
    const FocusMods shift{.shift = true};
    CHECK(key(Arrow::Up, shift).action == Action::NavModule);
    CHECK(key(Arrow::Up, shift).direction == Direction::Up);
    CHECK(key(Arrow::Down, shift).direction == Direction::Down);
    CHECK(key(Arrow::Left, shift).direction == Direction::Left);
    CHECK(key(Arrow::Right, shift).direction == Direction::Right);
    CHECK(key(Arrow::Right, shift, true).action == Action::None); // key down only
}

TEST_CASE("arrow keys: the morph modifier steps the morph range; left and right once per press")
{
    // The manual's Ctrl (p115) is Option on the Mac (CShortcutKeys::Add turns modifier 5 into 2, Option).
    const FocusMods morph{.morph = true};
    CHECK(key(Arrow::Up, morph).action == Action::StepMorph);
    CHECK(key(Arrow::Up, morph).delta == +1);
    CHECK(key(Arrow::Down, morph).delta == -1);
    CHECK(key(Arrow::Up, morph, true).action == Action::StepMorph);
    CHECK(key(Arrow::Right, morph).action == Action::NextParam);
    CHECK(key(Arrow::Right, morph, true).action == Action::None);
}

TEST_CASE("arrow keys: other modifiers are unbound; shift with the morph modifier counts as none")
{
    CHECK(key(Arrow::Up, {.other = true}).action == Action::None);
    CHECK(key(Arrow::Up, {.shift = true, .other = true}).action == Action::None);
    // GetMethod's parity: Control equal to Shift xor Option xor Command reads as no modifier.
    CHECK(key(Arrow::Up, {.shift = true, .morph = true}).action == Action::StepValue);
}

TEST_CASE("arrow keys: while the Patch Mutator is open the plain arrows are its own")
{
    // KeyIncParamValue and the others check gFloater: the Mutator gets the plain arrows and Option+left/right.
    CHECK(key(Arrow::Up, {}, false, true).action == Action::None);
    CHECK(key(Arrow::Left, {}, false, true).action == Action::None);
    CHECK(key(Arrow::Left, {.morph = true}, false, true).action == Action::None);
    CHECK(key(Arrow::Up, {.morph = true}, false, true).action == Action::StepMorph);
    CHECK(key(Arrow::Up, {.shift = true}, false, true).action == Action::NavModule);
}
