// The focused parameter of a patch and what the patch window's keys do with it, as Clavia's editor v1.62 has them
// (CPatch+0x40, CPatchView::InitializeShortcutKeys @000fafd8; manual p83-84, p115).
//
// A patch has one focus, shared by the VA and FX areas and the patch settings, and always has one: it starts on
// Morph dial 1 and nothing clears it. A click on a control or on a module's body moves it; Up/Down step its value,
// Left/Right walk the module's parameters, Shift+arrows go to a neighbouring module, Option+Up/Down step its morph
// range. The functions below are those rules; the patch view decides when to call them.
#pragma once

#include "g2/edit.hpp"

#include <vector>

namespace g2::focus {

using Focus = edit::Target;

// kDefaultFocus, set by CPatch::CPatch @000d96d4: the patch settings' Morph module, dial 1.
inline constexpr Focus kDefault{Location::Settings, static_cast<u8>(edit::Setting::Morph), 0};

// Whether the focus names a parameter the patch has.
bool exists(const Patch& patch, Focus f);

// Right and Left (CPatchView::KeyNavNextParam @000f84e4, KeyNavPrevParam @000f857c): the next or previous
// parameter in the module type's edit order (db::ModuleDef::editOrder), wrapping within the module.
Focus nextParam(const Patch& patch, Focus f);
Focus previousParam(const Patch& patch, Focus f);

// Shift+arrows (CPatchView::KeyNavModule*, CPatchData::CModuleIterator::NavModule*), over the area's modules
// sorted by column, then row (MacModulePositionSort @000e49d0). Up and Down: the module above or below in the
// same column, wrapping within it. Right and Left: in the nearest column on that side that has modules, the
// module whose top row is nearest (a tie: the upper one going right, the lower one going left); at the edge the
// focus stays. The focus goes to the module's parameter 0; a module without parameters is passed over for the
// next one in column order. In the patch settings nothing moves.
enum class Direction : u8 { Up, Down, Left, Right };
Focus navModule(const Patch& patch, Focus f, Direction dir);

// A click on the body of a module that is not selected, or a module added (CPatch::AddToSelection @000d5d0a):
// its parameter 0, if it has parameters and the focus is not already in it; otherwise the focus as it is.
Focus onModule(const Patch& patch, Location loc, u8 module, Focus f);
// The focused module deleted: back to Morph dial 1 (CPatch::RemoveFocusFrom @000da8bc).
Focus afterDelete(Focus f, Location loc, const std::vector<u8>& removed);

// Up and Down (CPnlControl::ReqIncParamValue @000c86d4, ReqDecParamValue @000c872c): the value in `variation`
// one step of the parameter's own range up or down, stopping at its ends. Returns whether it changed (the
// original sends nothing at an end).
bool stepValue(Patch& patch, Focus f, u8 variation, int delta);

// Option+Up/Down (the manual's Ctrl, p115; CPatchView::KeyIncMorphDelta @000f8292, KeyDecMorphDelta @000fa3b4):
// the parameter's range in morph `group` of `variation` one step up or down, value + range within the
// parameter's range (CPnlControl::ReqIncMorphDelta @000c8600); a parameter not in the group joins it with +-1
// (CPanel::CtrlAutoAssignMorph @000c1b9c). Nothing for a parameter the editor does not morph (edit::canMorph), a
// group outside 0..7 (no group focused) or the patch settings. Returns whether it changed.
bool stepMorph(Patch& patch, Focus f, u8 variation, u8 group, int delta);

} // namespace g2::focus
