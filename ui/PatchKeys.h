#pragma once
// The patch window's single keys, as Clavia's editor v1.62 binds them (CPatchView::InitializeShortcutKeys @000fafd8;
// manual p115-116): 1 to 8 select the variation, A to D a performance's slot, on key down and with no modifier.
// Plain C++.

#include <cctype>

namespace g2ui {

struct PatchKey {
    enum class Kind { None, Variation, Slot };
    Kind kind = Kind::None;
    int index = 0;
};

// `c`: the character typed; `modifier`: any modifier held; `repeat`: the key's auto-repeat; `performance`: the
// document is a performance (only then are there slots to select).
inline PatchKey patchKey(char c, bool modifier, bool repeat, bool performance)
{
    if (modifier || repeat)
        return {};
    if (c >= '1' && c <= '8')
        return {PatchKey::Kind::Variation, c - '1'};
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (performance && l >= 'a' && l <= 'd')
        return {PatchKey::Kind::Slot, l - 'a'};
    return {};
}

} // namespace g2ui
