// The patch window's single keys, as Clavia's editor v1.62 binds them (CPatchView::InitializeShortcutKeys @000fafd8:
// KeySelectVariation1..8 and KeySelectSlotA..D on key down, no modifier; manual p115-116).
#include <catch2/catch_test_macros.hpp>

#include "PatchKeys.h"

using namespace g2ui;
using Kind = PatchKey::Kind;

TEST_CASE("patch keys: 1 to 8 select the variation, alone and once per press")
{
    for (int i = 0; i < 8; ++i) {
        const auto k = patchKey(static_cast<char>('1' + i), false, false, false);
        CHECK(k.kind == Kind::Variation);
        CHECK(k.index == i);
    }
    CHECK(patchKey('9', false, false, false).kind == Kind::None);
    CHECK(patchKey('0', false, false, false).kind == Kind::None);
    CHECK(patchKey('1', true, false, false).kind == Kind::None);  // with a modifier: not this key
    CHECK(patchKey('1', false, true, false).kind == Kind::None);  // the key's auto-repeat
}

TEST_CASE("patch keys: A to D select a performance's slot")
{
    CHECK(patchKey('a', false, false, true).kind == Kind::Slot);
    CHECK(patchKey('a', false, false, true).index == 0);
    CHECK(patchKey('D', false, false, true).index == 3);
    CHECK(patchKey('e', false, false, true).kind == Kind::None);
    CHECK(patchKey('b', false, false, false).kind == Kind::None); // a patch has no slots
    CHECK(patchKey('c', true, false, true).kind == Kind::None);
}
