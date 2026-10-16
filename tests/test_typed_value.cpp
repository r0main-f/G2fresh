// Typing a parameter's value (G2fresh's own: the original has no text entry for module parameters, only steps,
// manual p83-84). The text is matched against what the parameter displays, so the value is always one of its steps.
#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"

using namespace g2;

namespace {

struct Fixture {
    Patch p = Patch::makeDefault();
    u8 add(u8 type) { return edit::addModule(p, Location::Va, type, 0, static_cast<u8>(p.va.modules.size() * 6)); }
    std::optional<u8> typed(u8 module, u8 param, const std::string& text) const
    {
        return edit::targetFromText(p, {Location::Va, module, param}, 0, text);
    }
};

constexpr u8 k2Out = 4, kOscB = 7, kMix4 = 19, kEnvAdsr = 20;

} // namespace

TEST_CASE("typed value: a name as displayed, or the start of one, case aside")
{
    Fixture f;
    const u8 env = f.add(kEnvAdsr), osc = f.add(kOscB), out = f.add(k2Out);
    CHECK(f.typed(env, 0, "LinLin") == 3);
    CHECK(f.typed(env, 0, "linexp") == 1);
    CHECK(f.typed(osc, 8, "saw") == 2);
    CHECK(f.typed(osc, 8, "Sq") == 3);
    CHECK(f.typed(out, 0, "fx 1/2") == 2);
    CHECK_FALSE(f.typed(osc, 8, "zzz"));
}

TEST_CASE("typed value: the nearest number displayed")
{
    Fixture f;
    const u8 mix = f.add(kMix4), osc = f.add(kOscB);
    CHECK(f.typed(mix, 0, "50") == 64);   // displays 50
    CHECK(f.typed(mix, 0, "31") == 40);   // 31.2
    CHECK(f.typed(mix, 0, "1000") == 127); // past the end: the end
    CHECK(f.typed(osc, 1, "+28.1") == 100);
    CHECK(f.typed(osc, 1, "-50") == 0);
    CHECK(f.typed(osc, 0, "-24") == 40); // Coarse displays "-24   +0": its first number
}

TEST_CASE("typed value: units are compared as quantities")
{
    Fixture f;
    const u8 env = f.add(kEnvAdsr);
    CHECK(f.typed(env, 1, "1.02s") == 64);
    CHECK(f.typed(env, 1, "126m") == 40);
    CHECK(f.typed(env, 1, "126 ms") == 40);
    CHECK(f.typed(env, 1, "0.126s") == 40);
    CHECK(f.typed(env, 1, "45s") == 127);
}

TEST_CASE("typed value: #n sets the raw value; a patch setting works too")
{
    Fixture f;
    const u8 mix = f.add(kMix4);
    CHECK(f.typed(mix, 0, "#64") == 64);
    CHECK(f.typed(mix, 0, "#300") == 127);
    const auto glideTime = edit::Target{Location::Settings, static_cast<u8>(edit::Setting::Glide), 1};
    const auto text = edit::targetText(f.p, glideTime, 0);
    REQUIRE_FALSE(text.empty());
    CHECK(edit::targetFromText(f.p, glideTime, 0, text) == edit::targetValue(f.p, glideTime, 0));
}

TEST_CASE("typed value: the dialog starts with the parameter's own value, not a neighbour's")
{
    // Found by hand testing: OscB's Coarse displays "-24   +0", its semitones then its Fine; the dialog showed both, and
    // changing the second did nothing (it is Fine's). It starts with the first number only, with its unit.
    Fixture f;
    const u8 osc = f.add(kOscB), env = f.add(kEnvAdsr), out = f.add(k2Out);
    edit::setParam(f.p, Location::Va, osc, 0, 0, 40);
    CHECK(edit::targetEditText(f.p, {Location::Va, osc, 0}, 0) == "-24");
    CHECK(edit::targetEditText(f.p, {Location::Va, env, 1}, 0) == "0.5m"); // " 0.5m": trimmed, the unit kept
    CHECK(edit::targetEditText(f.p, {Location::Va, osc, 8}, 0) == "Saw"); // a name as it is (OscB starts on Saw)
    CHECK(edit::targetEditText(f.p, {Location::Va, out, 0}, 0) == "Out 1/2"); // a name with digits, whole
    // What it gives reads back as the same value.
    CHECK(f.typed(osc, 0, edit::targetEditText(f.p, {Location::Va, osc, 0}, 0)) == 40);
    // Typing both numbers: the first counts.
    CHECK(f.typed(osc, 0, "-12   +0") == 52);
}
