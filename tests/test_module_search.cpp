// Finding a module by typing (G2fresh's own: the original's module bar has groups and buttons only, manual p59).
#include <catch2/catch_test_macros.hpp>

#include "ModuleSearch.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace g2ui;

namespace {

std::vector<std::string> names(const std::string& query)
{
    std::vector<std::string> out;
    for (const auto* def : searchModules(query))
        out.emplace_back(def->shortName);
    return out;
}

bool before(const std::vector<std::string>& v, const std::string& a, const std::string& b)
{
    const auto ia = std::find(v.begin(), v.end(), a), ib = std::find(v.begin(), v.end(), b);
    return ia != v.end() && ib != v.end() && ia < ib;
}

} // namespace

TEST_CASE("module search: names first, in the module bar's order, whatever the case")
{
    const auto lfo = names("lfo");
    REQUIRE(lfo.size() >= 4);
    CHECK(std::vector<std::string>(lfo.begin(), lfo.begin() + 4) == std::vector<std::string>{"LfoA", "LfoB", "LfoC", "LfoShpA"});
    CHECK(names("LFO") == lfo);
    // A name that starts with the word before one that only contains it; the long name after both.
    const auto env = names("env");
    CHECK(env.front() == "EnvADSR");
    CHECK(before(env, "EnvADDSR", "ModADSR")); // ModADSR: "Envelope" only in its long name
}

TEST_CASE("module search: every word must be found, in the names or the description")
{
    const auto ring = names("ring mod");
    CHECK(std::find(ring.begin(), ring.end(), "LevMod") != ring.end());
    CHECK(std::find(ring.begin(), ring.end(), "OscString") != ring.end());
    CHECK(std::find(ring.begin(), ring.end(), "OscA") == ring.end());
    CHECK(names("env adsr").front() == "EnvADSR");
}

TEST_CASE("module search: only the modules the bar offers; nothing for nothing")
{
    // The Glide module once, first; not the patch settings' Glide (Keyboard follows: its description has the word).
    const auto glide = names("glide");
    CHECK(glide.front() == "Glide");
    CHECK(std::count(glide.begin(), glide.end(), "Glide") == 1);
    CHECK(names("").empty());
    CHECK(names("   ").empty());
    CHECK(names("zzzz").empty());
}
