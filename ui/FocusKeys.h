#pragma once
// What the patch window's arrow keys do with the parameter focus (g2/focus.hpp), as Clavia's editor v1.62 binds
// them (CPatchView::InitializeShortcutKeys @000fafd8; manual p115). Plain C++.

#include "g2/focus.hpp"

namespace g2ui {

enum class Arrow { Up, Down, Left, Right };

// The modifiers held. `morph` is the manual's Ctrl, Option on the Mac (CShortcutKeys::Add @001c86e0 turns
// modifier 5 into 2, Option); `other` is any other modifier but Shift (Command on the Mac).
struct FocusMods {
    bool shift = false;
    bool morph = false;
    bool other = false;
};

struct FocusKey {
    enum class Action {
        None,      // not bound here: the key goes on (to the Mutator, or the window's own handling)
        StepValue, // focus::stepValue by `delta`
        NextParam, // focus::nextParam (+1) or previousParam (-1)
        NavModule, // focus::navModule to `direction`
        StepMorph, // focus::stepMorph by `delta` in the focused morph group
    };
    Action action = Action::None;
    int delta = 0;
    g2::focus::Direction direction = g2::focus::Direction::Up;
};

// `repeat`: the key's auto-repeat while held (the original binds some keys for its key-down state only);
// `mutatorOpen`: the Patch Mutator's window is open, and takes the plain arrows and the morph modifier's
// Left/Right (the key handlers check gFloater).
inline FocusKey focusKey(Arrow arrow, FocusMods mods, bool repeat, bool mutatorOpen)
{
    using Action = FocusKey::Action;
    const bool vertical = arrow == Arrow::Up || arrow == Arrow::Down;
    const int delta = arrow == Arrow::Up || arrow == Arrow::Right ? +1 : -1;
    // CShortcutKeys::GetMethod @001c81e2 reads no modifier when Control equals Shift xor Option xor Command:
    // Shift with Option counts as none. Otherwise one modifier is bound, or nothing.
    if (mods.shift && mods.morph && !mods.other)
        mods = {};
    const int held = int(mods.shift) + int(mods.morph) + int(mods.other);
    if (held > 1 || mods.other)
        return {};
    if (mods.shift) {
        if (repeat)
            return {};
        const auto dir = arrow == Arrow::Up     ? g2::focus::Direction::Up
                       : arrow == Arrow::Down   ? g2::focus::Direction::Down
                       : arrow == Arrow::Left   ? g2::focus::Direction::Left
                                                : g2::focus::Direction::Right;
        return {Action::NavModule, 0, dir};
    }
    if (mods.morph) {
        if (vertical)
            return {Action::StepMorph, delta};
        if (repeat || mutatorOpen)
            return {};
        return {Action::NextParam, delta};
    }
    if (mutatorOpen)
        return {};
    return {vertical ? Action::StepValue : Action::NextParam, delta};
}

} // namespace g2ui
