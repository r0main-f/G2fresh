#pragma once
// When a panel control acts on a click: a button on mouse-down, as Clavia's editor v1.62 (CPnlButtonABC::OnClick
// @000c3520 -> OnClickHandler @000c341a requests the new value at once; Release @000c355e only ends the press).
// Plain C++.

#include <string_view>

namespace g2ui {

// The PANL kinds of the buttons: text and flat buttons, label buttons, level shifters, radio buttons, inc/dec
// buttons. Not knobs (they act by dragging) nor mode selectors (a click opens their menu).
inline bool actsOnPress(std::string_view kind)
{
    return kind == "ButtonText" || kind == "TextEdit" || kind == "ButtonFlat" || kind == "LevelShift"
        || kind == "ButtonRadio" || kind == "ButtonRadioEdit" || kind == "ButtonIncDec";
}

} // namespace g2ui
