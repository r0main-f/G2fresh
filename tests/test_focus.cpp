// The parameter focus and its keys, as Clavia's editor v1.62 has them (fidelity audit U2). Every statement below
// comes from the editor's code; the addresses are the Mac editor's.
#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "g2/focus.hpp"

#include <string>

template <>
struct Catch::StringMaker<g2::focus::Focus> {
    static std::string convert(const g2::focus::Focus& f)
    {
        return "{" + std::to_string(static_cast<int>(f.location)) + ", " + std::to_string(f.module) + ", "
            + std::to_string(f.param) + "}";
    }
};

using namespace g2;
using focus::Direction;

namespace {

constexpr u8 kKeyboard = 1; // no parameters
constexpr u8 k2Out = 4;     // Dest 0..5, On/Off 0..1, Pad 0..1; edit order Dest, Pad, On/Off
constexpr u8 kMix4 = 19;    // Level 1-4 0..127, Lin/Exp 0..2
constexpr u8 kEnvAdsr = 20; // Shape 0..3, Attack 1 .. Reset 7; edit order Attack .. Reset, Shape
constexpr u8 kLfoB = 190;   // Mode (5) is not morphable
constexpr auto VA = Location::Va;

struct Fixture {
    Patch p = Patch::makeDefault();
    u8 add(u8 type, u8 col, u8 row) { return edit::addModule(p, VA, type, col, row); }
    static focus::Focus at(u8 module, u8 param = 0) { return {VA, module, param}; }
};

} // namespace

TEST_CASE("the focus: Morph dial 1 at first, one for the whole patch")
{
    // CPatch::CPatch @000d96d4 sets {2, 1, 0}: the patch settings, the Morph module, its dial 1.
    CHECK(focus::kDefault == focus::Focus{Location::Settings, static_cast<u8>(edit::Setting::Morph), 0});
    Fixture f;
    CHECK(focus::exists(f.p, focus::kDefault));
    CHECK_FALSE(focus::exists(f.p, Fixture::at(9)));
}

TEST_CASE("left and right: the module's edit order, wrapping within the module")
{
    // CBuildModule::GetNextParamFrom @000a9f6c / GetPreviousParamFrom @000a9fc6 walk the module type's
    // ParamEditOrder table, not the parameter indices, and never leave the module.
    Fixture f;
    const u8 out = f.add(k2Out, 0, 0), env = f.add(kEnvAdsr, 1, 0);
    CHECK(focus::nextParam(f.p, Fixture::at(out, 0)) == Fixture::at(out, 2)); // Dest -> Pad
    CHECK(focus::nextParam(f.p, Fixture::at(out, 2)) == Fixture::at(out, 1)); // Pad -> On/Off
    CHECK(focus::nextParam(f.p, Fixture::at(out, 1)) == Fixture::at(out, 0)); // On/Off -> Dest
    CHECK(focus::previousParam(f.p, Fixture::at(out, 0)) == Fixture::at(out, 1));
    // EnvADSR: Attack .. Reset, then Shape; a click on the module focuses Shape (index 0), -> goes to Attack.
    CHECK(focus::nextParam(f.p, Fixture::at(env, 0)) == Fixture::at(env, 1));
    CHECK(focus::nextParam(f.p, Fixture::at(env, 7)) == Fixture::at(env, 0));
    CHECK(focus::previousParam(f.p, Fixture::at(env, 1)) == Fixture::at(env, 0));
    // The patch settings too: the Morph module's 16 parameters.
    CHECK(focus::nextParam(f.p, focus::kDefault) == focus::Focus{Location::Settings, 1, 1});
    CHECK(focus::previousParam(f.p, focus::kDefault) == focus::Focus{Location::Settings, 1, 15});
}

TEST_CASE("shift and the arrows: the neighbouring module, its parameter 0")
{
    // CPatchData::CModuleIterator::NavModuleUp/Down/Left/Right (@000e5956, @000e59e6, @000e69d8, @000e4a40) over
    // the area's modules sorted by column, then row (MacModulePositionSort @000e49d0).
    Fixture f;
    const u8 a = f.add(kMix4, 0, 0), b = f.add(kMix4, 0, 2), c = f.add(kMix4, 0, 4);
    const u8 d = f.add(kMix4, 1, 3), e = f.add(kMix4, 1, 5);
    const u8 g = f.add(kMix4, 3, 0); // column 2 is empty
    auto nav = [&](u8 from, Direction dir) { return focus::navModule(f.p, Fixture::at(from, 3), dir); };

    SECTION("up and down: the same column, wrapping")
    {
        CHECK(nav(b, Direction::Up) == Fixture::at(a));
        CHECK(nav(a, Direction::Up) == Fixture::at(c));
        CHECK(nav(a, Direction::Down) == Fixture::at(b));
        CHECK(nav(c, Direction::Down) == Fixture::at(a));
    }
    SECTION("right and left: the nearest column with modules, the nearest top row")
    {
        CHECK(nav(a, Direction::Right) == Fixture::at(d));
        CHECK(nav(d, Direction::Right) == Fixture::at(g)); // past the empty column
        CHECK(nav(g, Direction::Right) == Fixture::at(g, 3)); // at the edge it stays
        CHECK(nav(a, Direction::Left) == Fixture::at(a, 3));
        // A tie: the upper one going right, the lower one going left.
        CHECK(nav(c, Direction::Right) == Fixture::at(d)); // rows 3 and 5 from row 4
        CHECK(nav(d, Direction::Left) == Fixture::at(c));  // rows 2 and 4 from row 3
    }
    SECTION("a module without parameters is passed over")
    {
        const u8 k = f.add(kKeyboard, 0, 7), z = f.add(kMix4, 0, 9);
        CHECK(nav(c, Direction::Down) == Fixture::at(z));
        (void)k;
    }
    SECTION("nothing in the patch settings")
    {
        CHECK(focus::navModule(f.p, focus::kDefault, Direction::Right) == focus::kDefault);
    }
    (void)e;
}

TEST_CASE("a click on a module's body focuses its parameter 0; deleting the focused module gives Morph dial 1")
{
    // CPanel::PanelClick @000bd710 -> CPatch::AddToSelection @000d5d0a: the module's parameter 0, if it has
    // parameters and the focus is not already in it. CPatch::RemoveFocusFrom @000da8bc.
    Fixture f;
    const u8 mix = f.add(kMix4, 0, 0), kbd = f.add(kKeyboard, 1, 0);
    CHECK(focus::onModule(f.p, VA, mix, focus::kDefault) == Fixture::at(mix, 0));
    CHECK(focus::onModule(f.p, VA, mix, Fixture::at(mix, 3)) == Fixture::at(mix, 3));
    CHECK(focus::onModule(f.p, VA, kbd, Fixture::at(mix, 3)) == Fixture::at(mix, 3));
    CHECK(focus::afterDelete(Fixture::at(mix, 3), VA, {kbd}) == Fixture::at(mix, 3));
    CHECK(focus::afterDelete(Fixture::at(mix, 3), VA, {kbd, mix}) == focus::kDefault);
    CHECK(focus::afterDelete(Fixture::at(mix, 3), Location::Fx, {mix}) == Fixture::at(mix, 3));
}

TEST_CASE("up and down: one step of the parameter's own range, stopping at its ends")
{
    // CPnlControl::ReqIncParamValue @000c86d4 / ReqDecParamValue @000c872c: +-1, clamped, nothing sent at an end.
    Fixture f;
    const u8 out = f.add(k2Out, 0, 0), mix = f.add(kMix4, 1, 0);
    edit::setParam(f.p, VA, mix, 0, 0, 126);
    CHECK(focus::stepValue(f.p, Fixture::at(mix, 0), 0, +1));
    CHECK(edit::targetValue(f.p, Fixture::at(mix, 0), 0) == 127);
    CHECK_FALSE(focus::stepValue(f.p, Fixture::at(mix, 0), 0, +1));
    CHECK(focus::stepValue(f.p, Fixture::at(mix, 0), 0, -1));
    CHECK(edit::targetValue(f.p, Fixture::at(mix, 0), 0) == 126);
    // On/Off: 0..1.
    edit::setParam(f.p, VA, out, 1, 0, 1);
    CHECK_FALSE(focus::stepValue(f.p, Fixture::at(out, 1), 0, +1));
    CHECK(focus::stepValue(f.p, Fixture::at(out, 1), 0, -1));
    CHECK_FALSE(focus::stepValue(f.p, Fixture::at(out, 1), 0, -1));
    // Only the variation given.
    CHECK(edit::targetValue(f.p, Fixture::at(out, 1), 1) == 1);
    // A patch setting.
    const auto rate = focus::Focus{Location::Settings, static_cast<u8>(edit::Setting::Glide), 1};
    const u8 before = *edit::targetValue(f.p, rate, 0);
    CHECK(focus::stepValue(f.p, rate, 0, before == 0 ? +1 : -1));
}

TEST_CASE("option with up and down: the range in the focused morph group, assigning +-1 first")
{
    // CPatchView::KeyIncMorphDelta @000f8292 / KeyDecMorphDelta @000fa3b4: the toolbar's morph group;
    // ReqIncMorphDelta @000c8600 steps a range the parameter has; CtrlAutoAssignMorph @000c1b9c assigns +-1.
    Fixture f;
    const u8 mix = f.add(kMix4, 0, 0), lfo = f.add(kLfoB, 1, 0);
    const auto level = Fixture::at(mix, 0);
    edit::setParam(f.p, VA, mix, 0, 0, 100);
    CHECK(focus::stepMorph(f.p, level, 0, 2, +1));
    CHECK(edit::morphRange(f.p, 0, VA, mix, 0, 2) == 1);
    CHECK(focus::stepMorph(f.p, level, 0, 2, +1));
    CHECK(edit::morphRange(f.p, 0, VA, mix, 0, 2) == 2);
    CHECK_FALSE(edit::morphRange(f.p, 0, VA, mix, 0, 3)); // other groups untouched
    // value + range stays within 0..127.
    edit::setMorph(f.p, 0, VA, mix, 0, 2, 27);
    CHECK_FALSE(focus::stepMorph(f.p, level, 0, 2, +1));
    CHECK(focus::stepMorph(f.p, level, 0, 2, -1));
    CHECK(edit::morphRange(f.p, 0, VA, mix, 0, 2) == 26);
    // Nothing for a parameter that does not morph, without a focused group, or in the patch settings.
    CHECK_FALSE(focus::stepMorph(f.p, Fixture::at(lfo, 5), 0, 2, +1));
    CHECK_FALSE(focus::stepMorph(f.p, level, 0, 8, +1));
    CHECK_FALSE(focus::stepMorph(f.p, focus::kDefault, 0, 2, +1));
}
