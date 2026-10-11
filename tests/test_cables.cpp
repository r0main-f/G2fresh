#include <catch2/catch_test_macros.hpp>

#include "g2/cables.hpp"
#include "g2/edit.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <iostream>

using namespace g2;

namespace {

constexpr u8 kOscB = 7;     // static red output 0
constexpr u8 kLevMult = 44; // inputs In 0, Mod 1; output 0 (dynamic blue/red)
constexpr u8 kOut2 = 4;     // inputs L 0, R 1
constexpr u8 kLfoB = 190;   // static blue output 0
constexpr auto VA = Location::Va;

struct Fixture {
    Patch p = Patch::makeDefault();
    u8 add(u8 type)
    {
        const u8 col = static_cast<u8>(p.va.modules.size() % 6), row = static_cast<u8>(p.va.modules.size() / 6 * 10);
        return edit::addModule(p, VA, type, col, row);
    }
    static edit::Endpoint out(u8 m, u8 c = 0) { return {m, c, true}; }
    static edit::Endpoint in(u8 m, u8 c = 0) { return {m, c, false}; }
    Cable connect(edit::Endpoint a, edit::Endpoint b) { return edit::connect(p, VA, a, b); }
    // The cable between two connectors, whichever way it is stored.
    const Cable* cable(edit::Endpoint a, edit::Endpoint b) const
    {
        for (const auto& c : p.va.cables) {
            const edit::Endpoint f{c.fromModule, c.fromConn, c.fromIsOutput}, t{c.toModule, c.toConn, false};
            auto same = [](edit::Endpoint x, edit::Endpoint y) {
                return x.module == y.module && x.conn == y.conn && x.isOutput == y.isOutput;
            };
            if ((same(f, a) && same(t, b)) || (same(f, b) && same(t, a)))
                return &c;
        }
        return nullptr;
    }
    bool stored(edit::Endpoint from, edit::Endpoint to) const
    {
        for (const auto& c : p.va.cables)
            if (c.fromModule == from.module && c.fromConn == from.conn && c.fromIsOutput == from.isOutput
                && c.toModule == to.module && c.toConn == to.conn)
                return true;
        return false;
    }
    std::size_t count() const { return p.va.cables.size(); }
};

// The cable list as the original's tree writes it back.
bool reproduces(const Area& area)
{
    return cables::Tree(area).toCables() == area.cables;
}

} // namespace

TEST_CASE("cable trees: replaying a saved cable list and walking it gives the list back")
{
    int areas = 0;
    for (const auto& path : g2test::corpusFiles()) {
        Loaded loaded;
        try {
            loaded = load(g2test::readBytes(path));
        } catch (const std::exception&) {
            continue;
        }
        if (loaded.isPerformance())
            continue;
        const auto& patch = std::get<Patch>(loaded.content);
        for (const Area* area : {&patch.va, &patch.fx})
            if (!area->cables.empty()) {
                ++areas;
                INFO(path.filename().string());
                CHECK(reproduces(*area));
            }
    }
    CHECK(areas > 0);
}

TEST_CASE("cable trees: a link is stored from the input farther from the output")
{
    Fixture f;
    const u8 osc = f.add(kOscB), a = f.add(kOut2), b = f.add(kOut2);
    f.connect(f.out(osc), f.in(a));
    // Dragged from the fed input to the free one, or the other way: the same cable.
    const Cable link = f.connect(f.in(a), f.in(b));
    CHECK(f.stored(f.in(b), f.in(a)));
    CHECK(link.fromModule == b);
    CHECK(link.color == CableColor::Red);
    edit::disconnectJack(f.p, VA, f.in(b));
    f.connect(f.in(b), f.in(a));
    CHECK(f.stored(f.in(b), f.in(a)));
    CHECK(reproduces(f.p.va));
}

TEST_CASE("connect: one output per net and no loops")
{
    Fixture f;
    const u8 osc = f.add(kOscB), osc2 = f.add(kOscB), a = f.add(kOut2), b = f.add(kOut2);
    f.connect(f.out(osc), f.in(a));
    f.connect(f.out(osc2), f.in(b));
    // Both nets have an output.
    CHECK_THROWS_AS(f.connect(f.in(a), f.in(b)), std::invalid_argument);
    CHECK_THROWS_AS(f.connect(f.out(osc), f.in(b)), std::invalid_argument);
    CHECK_THROWS_AS(f.connect(f.out(osc), f.out(osc2)), std::invalid_argument);
    // The same net: a duplicate or a loop.
    CHECK_THROWS_AS(f.connect(f.out(osc), f.in(a)), std::invalid_argument);
    f.connect(f.in(a), f.in(a, 1));
    CHECK_THROWS_AS(f.connect(f.in(a, 1), f.out(osc)), std::invalid_argument);
    CHECK_THROWS_AS(f.connect(f.in(a), f.in(a)), std::invalid_argument);
    CHECK(f.count() == 3);
    CHECK(edit::dropAction(f.p, VA, f.in(a), f.in(b), false) == edit::DropAction::None);
    CHECK(edit::dropAction(f.p, VA, f.in(a), f.in(b, 1), false) == edit::DropAction::Connect);
    CHECK(edit::dropAction(f.p, VA, f.in(b, 1), std::nullopt, false) == edit::DropAction::None);
}

TEST_CASE("connect: the fed side's colour; chains without an output are white")
{
    Fixture f;
    const u8 lfo = f.add(kLfoB), a = f.add(kOut2), b = f.add(kOut2), c = f.add(kOut2);
    // Input-to-input links without an output: white.
    CHECK(f.connect(f.in(a), f.in(b)).color == CableColor::White);
    CHECK(f.connect(f.in(b), f.in(c)).color == CableColor::White);
    // Feeding the chain gives it the output's colour, every cable of it.
    CHECK(f.connect(f.out(lfo), f.in(b)).color == CableColor::Blue);
    for (const auto& cable : f.p.va.cables)
        CHECK(cable.color == CableColor::Blue);
    // A user colour on a chain spreads to what joins it.
    edit::recolorChain(f.p, VA, f.in(a), CableColor::Green);
    const u8 d = f.add(kOut2), e = f.add(kOut2);
    f.connect(f.in(d), f.in(e));
    CHECK(f.cable(f.in(d), f.in(e))->color == CableColor::White);
    CHECK(f.connect(f.in(d), f.in(a)).color == CableColor::Green);
    CHECK(f.cable(f.in(d), f.in(e))->color == CableColor::Green);
    CHECK(reproduces(f.p.va));
}

TEST_CASE("deleting a module splices its chains")
{
    Fixture f;
    const u8 osc = f.add(kOscB), a = f.add(kOut2), x = f.add(kLevMult), b = f.add(kOut2), z = f.add(kOut2);
    // Out -> A -> X -> B, and X's own output to Z.
    f.connect(f.out(osc), f.in(a));
    f.connect(f.in(a), f.in(x));
    f.connect(f.in(x), f.in(b));
    f.connect(f.out(x), f.in(z));
    REQUIRE(f.count() == 4);

    SECTION("a module in the middle: the chain closes up")
    {
        edit::removeModule(f.p, VA, x);
        CHECK(f.count() == 2);
        CHECK(f.stored(f.out(osc), f.in(a)));
        CHECK(f.stored(f.in(b), f.in(a))); // B now hangs from A
        CHECK(f.cable(f.in(b), f.in(a))->color == CableColor::Red);
    }
    SECTION("the output's module: the rest stays connected, white")
    {
        edit::removeModule(f.p, VA, osc);
        CHECK(f.count() == 3);
        CHECK(f.cable(f.in(a), f.in(x)));
        CHECK(f.cable(f.in(x), f.in(b)));
        for (const auto& c : f.p.va.cables)
            if (!c.fromIsOutput)
                CHECK(c.color == CableColor::White);
        CHECK(f.cable(f.out(x), f.in(z))->color != CableColor::White);
    }
    SECTION("a selection: what is left is joined to what remains above it")
    {
        edit::removeModules(f.p, VA, {a, x});
        CHECK(f.count() == 1);
        CHECK(f.stored(f.out(osc), f.in(b)));
        CHECK(f.cable(f.out(osc), f.in(b))->color == CableColor::Red);
    }
    SECTION("an output with several branches: the branches are joined, white")
    {
        const u8 c = f.add(kOut2);
        f.connect(f.out(osc), f.in(c));
        edit::removeModule(f.p, VA, osc);
        // A's chain and C are one net again, without an output.
        CHECK(f.cable(f.in(a), f.in(c)));
        CHECK(f.cable(f.in(a), f.in(c))->color == CableColor::White);
    }
    CHECK(reproduces(f.p.va));
}

TEST_CASE("jack popup: Disconnect splices")
{
    Fixture f;
    const u8 osc = f.add(kOscB), a = f.add(kOut2), b = f.add(kOut2), c = f.add(kOut2);
    f.connect(f.out(osc), f.in(a));
    f.connect(f.in(a), f.in(b));
    f.connect(f.in(a), f.in(c));

    SECTION("an input: its children go to its parent")
    {
        CHECK(edit::disconnectJack(f.p, VA, f.in(a)));
        CHECK(f.count() == 2);
        CHECK(f.stored(f.out(osc), f.in(b)));
        CHECK(f.stored(f.out(osc), f.in(c)));
        CHECK_FALSE(edit::disconnectJack(f.p, VA, f.in(a)));
    }
    SECTION("an output: the rest stays together, white")
    {
        CHECK(edit::disconnectJack(f.p, VA, f.out(osc)));
        CHECK(f.count() == 2);
        CHECK(f.cable(f.in(a), f.in(b)));
        CHECK(f.cable(f.in(a), f.in(c)));
        for (const auto& cable : f.p.va.cables)
            CHECK(cable.color == CableColor::White);
    }
    SECTION("a drop on nothing in a move is a Disconnect")
    {
        CHECK(edit::dropAction(f.p, VA, f.in(b), std::nullopt, true) == edit::DropAction::Disconnect);
        CHECK(edit::dropAction(f.p, VA, f.in(b, 1), std::nullopt, true) == edit::DropAction::None);
    }
    CHECK(reproduces(f.p.va));
}

TEST_CASE("jack popup: Break")
{
    Fixture f;
    const u8 osc = f.add(kOscB), a = f.add(kOut2), b = f.add(kOut2), c = f.add(kOut2);
    f.connect(f.out(osc), f.in(a));
    f.connect(f.in(a), f.in(b));
    f.connect(f.out(osc), f.in(c));
    CHECK_FALSE(edit::canBreak(f.p, VA, f.in(b))); // a leaf
    CHECK(edit::canBreak(f.p, VA, f.in(a)));

    SECTION("an input: cut from the output, its links kept, white")
    {
        CHECK(edit::breakJack(f.p, VA, f.in(a)));
        CHECK(f.count() == 2);
        CHECK(f.cable(f.in(a), f.in(b))->color == CableColor::White);
        CHECK(f.cable(f.out(osc), f.in(c))->color == CableColor::Red);
        // Nothing to break from any more.
        CHECK_FALSE(edit::breakJack(f.p, VA, f.in(a)));
    }
    SECTION("an output: every cable from it goes, the rest is white")
    {
        CHECK(edit::breakJack(f.p, VA, f.out(osc)));
        CHECK(f.count() == 1);
        CHECK(f.cable(f.in(a), f.in(b))->color == CableColor::White);
    }
    CHECK(reproduces(f.p.va));
}

TEST_CASE("jack popup: Color and Delete act on the serial chain")
{
    Fixture f;
    const u8 osc = f.add(kOscB), a = f.add(kOut2), b = f.add(kOut2), c = f.add(kOut2);
    f.connect(f.out(osc), f.in(a));
    f.connect(f.in(a), f.in(b));
    f.connect(f.out(osc), f.in(c));

    CHECK(edit::chainCables(f.p, VA, f.in(b)).size() == 2);
    CHECK(edit::chainCables(f.p, VA, f.out(osc)).size() == 3);
    const Cable link = *f.cable(f.in(a), f.in(b));
    CHECK(edit::lowerEnd(f.p, VA, link).module == b);
    CHECK(edit::lowerEnd(f.p, VA, *f.cable(f.out(osc), f.in(c))).module == c);

    edit::recolorChain(f.p, VA, f.in(b), CableColor::Purple);
    CHECK(f.cable(f.out(osc), f.in(a))->color == CableColor::Purple);
    CHECK(f.cable(f.in(a), f.in(b))->color == CableColor::Purple);
    CHECK(f.cable(f.out(osc), f.in(c))->color == CableColor::Red); // another branch
    CHECK_THROWS_AS(edit::recolorChain(f.p, VA, f.in(b), CableColor::White), std::invalid_argument);
    edit::recolorChain(f.p, VA, f.out(osc), CableColor::Green);
    for (const auto& cable : f.p.va.cables)
        CHECK(cable.color == CableColor::Green);

    CHECK(edit::deleteChain(f.p, VA, f.in(a)));
    CHECK(f.count() == 1);
    CHECK(f.cable(f.out(osc), f.in(c)));
    CHECK(edit::deleteChain(f.p, VA, f.out(osc)));
    CHECK(f.count() == 0);
    CHECK_FALSE(edit::deleteChain(f.p, VA, f.out(osc)));

    // An unfed chain cannot change colour.
    f.connect(f.in(a), f.in(b));
    CHECK_THROWS_AS(edit::recolorChain(f.p, VA, f.in(a), CableColor::Red), std::invalid_argument);
}

TEST_CASE("jack popup: Delete Unused Cables removes the chains without an output, in both areas")
{
    Fixture f;
    const u8 osc = f.add(kOscB), a = f.add(kOut2), b = f.add(kOut2), c = f.add(kOut2);
    f.connect(f.out(osc), f.in(a));
    f.connect(f.in(b), f.in(c));
    f.connect(f.in(b, 1), f.in(c, 1));
    const u8 fx1 = edit::addModule(f.p, Location::Fx, kOut2, 0, 0), fx2 = edit::addModule(f.p, Location::Fx, kOut2, 1, 0);
    edit::connect(f.p, Location::Fx, {fx1, 0, false}, {fx2, 0, false});

    CHECK(edit::deleteUnusedCables(f.p));
    CHECK(f.count() == 1);
    CHECK(f.cable(f.out(osc), f.in(a)));
    CHECK(f.p.fx.cables.empty());
    CHECK_FALSE(edit::deleteUnusedCables(f.p));
}

TEST_CASE("moving a plug moves every cable at the jack")
{
    Fixture f;
    const u8 osc = f.add(kOscB), lfo = f.add(kLfoB), a = f.add(kOut2), b = f.add(kOut2), c = f.add(kOut2);
    f.connect(f.out(osc), f.in(a));
    f.connect(f.out(osc), f.in(b));

    SECTION("an output's cables to another output, which gives its colour")
    {
        REQUIRE(edit::dropAction(f.p, VA, f.out(osc), f.out(lfo), true) == edit::DropAction::Move);
        edit::moveCables(f.p, VA, f.out(osc), f.out(lfo));
        CHECK(f.count() == 2);
        CHECK(f.stored(f.out(lfo), f.in(a)));
        CHECK(f.stored(f.out(lfo), f.in(b)));
        for (const auto& cable : f.p.va.cables)
            CHECK(cable.color == CableColor::Blue);
    }
    SECTION("an input's cables to a free input")
    {
        f.connect(f.in(a), f.in(a, 1));
        edit::moveCables(f.p, VA, f.in(a), f.in(c));
        CHECK(f.count() == 3);
        CHECK(f.stored(f.out(osc), f.in(c)));
        CHECK(f.stored(f.in(a, 1), f.in(c)));
        CHECK_FALSE(edit::isCabled(f.p, VA, f.in(a)));
    }
    SECTION("refused: into the same net, or a fed input onto another output")
    {
        CHECK(edit::dropAction(f.p, VA, f.in(a), f.in(b), true) == edit::DropAction::None);
        CHECK_THROWS_AS(edit::moveCables(f.p, VA, f.in(a), f.out(lfo)), std::invalid_argument);
        CHECK_THROWS_AS(edit::moveCables(f.p, VA, f.in(c), f.in(a)), std::invalid_argument); // no cable at C
    }
    CHECK(reproduces(f.p.va));
}

TEST_CASE("cable edits keep the bends of the cables that stay")
{
    Fixture f;
    const u8 osc = f.add(kOscB), a = f.add(kOut2), x = f.add(kOut2), b = f.add(kOut2);
    const Cable kept = f.connect(f.out(osc), f.in(a));
    f.connect(f.in(a), f.in(x));
    f.connect(f.in(x), f.in(b));
    edit::setCableBend(f.p, VA, kept, CableBend{12, -7});
    edit::removeModule(f.p, VA, x);
    REQUIRE(f.cable(f.out(osc), f.in(a)));
    CHECK(f.cable(f.out(osc), f.in(a))->bend == CableBend{12, -7});
    edit::recolorChain(f.p, VA, f.in(a), CableColor::Green);
    CHECK(f.cable(f.out(osc), f.in(a))->bend == CableBend{12, -7});
}
