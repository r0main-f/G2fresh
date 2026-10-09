#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "g2/mutate.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace g2;
using namespace g2::mutate;

namespace {

constexpr u8 kOscB = 7, kEnvADSR = 20, kReverb = 12, kSeqNote = 121;

// ---- Golden data from the original editor (tools/mutate/emulate.py) ---------------

struct Golden {
    std::vector<std::vector<std::string>> lines;
    explicit Golden(const std::string& path)
    {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream ss(line);
            std::vector<std::string> w;
            for (std::string t; ss >> t;)
                w.push_back(t);
            lines.push_back(std::move(w));
        }
    }
    std::vector<const std::vector<std::string>*> all(const std::string& tag) const
    {
        std::vector<const std::vector<std::string>*> out;
        for (const auto& l : lines)
            if (l[0] == tag)
                out.push_back(&l);
        return out;
    }
};

const Golden& golden()
{
    static const Golden g(G2_GOLDEN_DIR "/mutate.txt");
    return g;
}

double num(const std::string& s) { return std::strtod(s.c_str(), nullptr); }

bool same(double a, double b)
{
    if (std::isnan(a) || std::isnan(b))
        return std::isnan(a) && std::isnan(b);
    return a == b || std::fabs(a - b) <= 1e-12 * std::max(1.0, std::fabs(b));
}

// The golden scenario as a patch (VA/FX modules with lock flags) and two parents.
struct Scenario {
    Patch patch = Patch::makeDefault();
    Individual mother, father;
};

Scenario scenario()
{
    Scenario sc;
    sc.patch.settings.clear(); // the golden run has no patch-settings modules
    sc.patch.morphs.clear();
    for (const auto* l : golden().all("MODULE")) {
        const auto loc = static_cast<Location>(std::stoi((*l)[1]));
        Module m;
        m.index = static_cast<u8>(std::stoi((*l)[2]));
        m.type = static_cast<u8>(std::stoi((*l)[3]));
        m.locked = (*l)[4] == "1";
        ModuleGenes g{m.index, m.type, {}};
        for (std::size_t i = 5; i < l->size(); ++i)
            g.values.push_back(num((*l)[i]));
        m.params.assign(kFileVariations, std::vector<u8>(g.values.size(), 0));
        sc.patch.area(loc).modules.push_back(m);
        sc.mother.area(loc).push_back(g);
    }
    for (const auto* l : golden().all("FATHER")) {
        const auto loc = static_cast<Location>(std::stoi((*l)[1]));
        const auto idx = static_cast<u8>(std::stoi((*l)[2]));
        ModuleGenes g{idx, sc.mother.find(loc, idx)->type, {}};
        for (std::size_t i = 3; i < l->size(); ++i)
            g.values.push_back(num((*l)[i]));
        sc.father.area(loc).push_back(g);
    }
    auto morphs = [](const char* tag, Individual& ind) {
        for (const auto* l : golden().all(tag))
            ind.morphs.push_back({static_cast<u8>(std::stoi((*l)[1])), static_cast<u8>(std::stoi((*l)[2])),
                                  static_cast<u8>(std::stoi((*l)[3])), static_cast<u8>(std::stoi((*l)[4])),
                                  static_cast<std::int8_t>(std::stoi((*l)[5]))});
    };
    morphs("MMORPH", sc.mother);
    morphs("FMORPH", sc.father);
    return sc;
}

void checkResult(const Individual& child, const std::string& config, const std::string& op)
{
    int checked = 0;
    for (const auto* l : golden().all("RESULT")) {
        if ((*l)[1] != config || (*l)[2] != op)
            continue;
        const auto loc = static_cast<Location>(std::stoi((*l)[3]));
        const auto idx = static_cast<u8>(std::stoi((*l)[4]));
        const ModuleGenes* g = child.find(loc, idx);
        REQUIRE(g);
        REQUIRE(g->values.size() == l->size() - 5);
        for (std::size_t p = 0; p < g->values.size(); ++p) {
            INFO(config << " " << op << " loc " << int(loc) << " module " << int(idx) << " param " << p);
            CHECK(same(g->values[p], num((*l)[5 + p])));
        }
        ++checked;
    }
    CHECK(checked > 0);
}

Patch onePatch(std::vector<u8>* oscOut = nullptr)
{
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, kOscB, 0, 0);
    edit::addModule(p, Location::Va, kEnvADSR, 0, 4);
    edit::addModule(p, Location::Va, kSeqNote, 1, 0);
    edit::addModule(p, Location::Fx, kReverb, 0, 0);
    if (oscOut)
        oscOut->push_back(osc);
    return p;
}

} // namespace

// ---- Faithfulness to the original ------------------------------------------------------

TEST_CASE("mutate: Random reproduces libc random()")
{
    const auto rows = golden().all("RAND");
    REQUIRE(rows.size() == 1);
    Random r(1);
    for (std::size_t i = 2; i < rows[0]->size(); ++i)
        CHECK(r.next() == std::stol((*rows[0])[i]));
    // Other seeds, as macOS srandom()/random() give them.
    Random r0(0), r12345(12345);
    CHECK(r0.next() == 577655601);
    CHECK(r0.next() == 1248161417);
    CHECK(r12345.next() == 383100999);
    CHECK(r12345.next() == 858300821);
#ifdef __APPLE__
    ::srandom(4242);
    Random mine(4242);
    for (int i = 0; i < 1000; ++i)
        CHECK(mine.next() == ::random());
#endif
}

TEST_CASE("mutate: probability/range link matches the original")
{
    for (const auto* l : golden().all("LINK")) {
        const double x = num((*l)[1]);
        INFO("x = " << x);
        CHECK(same(probabilityForRange(x), num((*l)[2])));
        CHECK(same(rangeForProbability(x), num((*l)[3])));
    }
    Settings s;
    CHECK(std::fabs(probabilityForRange(kDefaultRange) - kDefaultProbability) < 1e-3);
    s.setProbability(1.0);
    CHECK(s.range < 0.03);
    s.setLink(false);
    s.setRange(0.4);
    CHECK(s.probability == 1.0);
}

TEST_CASE("mutate: parameter filter equals CMutaSynthData::IsEnabled for every module")
{
    int rows = 0;
    for (const auto* l : golden().all("ENABLED")) {
        Module m;
        m.type = static_cast<u8>(std::stoi((*l)[1]));
        Settings s;
        s.unlocked = static_cast<u8>(std::stoi((*l)[2]));
        s.solo = static_cast<u8>(std::stoi((*l)[3]));
        m.locked = (*l)[4] == "1";
        const std::string& bits = (*l)[5];
        for (std::size_t p = 0; p < bits.size(); ++p) {
            INFO("type " << int(m.type) << " param " << p << " unlocked " << int(s.unlocked) << " solo " << int(s.solo));
            CHECK(isEnabled(m, p, s) == (bits[p] == '1'));
        }
        ++rows;
    }
    CHECK(rows > 1000);
}

TEST_CASE("mutate: curves match the original ApplyCurve")
{
    for (const auto* l : golden().all("CURVE")) {
        const int kind = std::stoi((*l)[1]), n = std::stoi((*l)[2]);
        Random rng(static_cast<std::uint32_t>(7 + kind + n));
        const double v = applyCurve(static_cast<Curve>(kind), n, num((*l)[3]), num((*l)[4]), rng);
        INFO("kind " << kind << " n " << n << " x " << (*l)[3] << " range " << (*l)[4]);
        CHECK(same(v, num((*l)[5])));
    }
}

TEST_CASE("mutate: randomize, mutate and cross match the original value for value")
{
    const Scenario sc = scenario();
    for (const auto* c : golden().all("CONFIG")) {
        const std::string name = (*c)[1];
        Settings s;
        s.unlocked = static_cast<u8>(std::stoi((*c)[2]));
        s.solo = static_cast<u8>(std::stoi((*c)[3]));
        s.probability = num((*c)[4]);
        s.range = num((*c)[5]);
        s.crossover = num((*c)[6]);
        Random rng(1);
        const Individual r = randomized(sc.patch, sc.mother, s, rng);
        const Individual u = mutated(sc.patch, sc.mother, s, rng);
        const Individual x = recombined(sc.mother, sc.father, s.crossover, rng);
        SECTION("config " + name)
        {
            checkResult(r, name, "R");
            checkResult(u, name, "U");
            checkResult(x, name, "X");
            std::vector<MorphAssign> expected;
            for (const auto* l : golden().all("XMORPH"))
                if ((*l)[1] == name)
                    expected.push_back({static_cast<u8>(std::stoi((*l)[2])), static_cast<u8>(std::stoi((*l)[3])),
                                        static_cast<u8>(std::stoi((*l)[4])), static_cast<u8>(std::stoi((*l)[5])),
                                        static_cast<std::int8_t>(std::stoi((*l)[6]))});
            REQUIRE(x.morphs.size() == expected.size());
            for (std::size_t i = 0; i < expected.size(); ++i) {
                CHECK(x.morphs[i].location == expected[i].location);
                CHECK(x.morphs[i].module == expected[i].module);
                CHECK(x.morphs[i].param == expected[i].param);
                CHECK(x.morphs[i].morph == expected[i].morph);
                CHECK(x.morphs[i].range == expected[i].range);
            }
            // Same number of random() calls as the original.
            for (const auto* l : golden().all("NEXTRAND"))
                if ((*l)[1] == name)
                    CHECK(rng.next() == std::stol((*l)[2]));
        }
    }
}

// ---- Behaviour -----------------------------------------------------------------------------

TEST_CASE("mutate: locked modules and fixed classes are never changed")
{
    std::vector<u8> osc;
    Patch p = onePatch(&osc);
    p.va.find(osc[0])->locked = true;
    Settings s;
    s.unlocked = 0xFF;
    Random rng(99);
    const Individual parent = capture(p, 0, &rng);
    for (int i = 0; i < 20; ++i) {
        const Individual r = randomized(p, parent, s, rng);
        CHECK(r.find(Location::Va, osc[0])->values == parent.find(Location::Va, osc[0])->values);
        // Envelope: its fixed classes (KBG, Reset, OutType) keep their values.
        const Module& env = p.va.modules[1];
        const auto* g = r.find(Location::Va, env.index);
        const auto* pg = parent.find(Location::Va, env.index);
        for (std::size_t q = 0; q < g->values.size(); ++q)
            if (!isMutableClass(*paramClass(env.type, q)))
                CHECK(g->values[q] == pg->values[q]);
    }
    // Default quick locks: oscillator coarse (OscFreq) is locked.
    p.va.find(osc[0])->locked = false;
    Settings def;
    CHECK_FALSE(isEnabled(*p.va.find(osc[0]), 0, def)); // Coarse
    CHECK(isEnabled(*p.va.find(osc[0]), 1, def));       // Fine
    CHECK_FALSE(isEnabled(*p.va.find(osc[0]), 2, def)); // KBT switch
    def.setQuickLock(OscFine, true);
    CHECK_FALSE(isEnabled(*p.va.find(osc[0]), 1, def));
    def.setSolo(Envelope, true);
    def.setQuickLock(OscFine, false); // ignored while solo is on
    CHECK(def.quickLocked(OscFine));
    CHECK_FALSE(isEnabled(*p.va.find(osc[0]), 1, def));
    CHECK(isEnabled(p.va.modules[1], 1, def)); // envelope attack
    // Default-locked module types (edit::addModule sets the flag).
    for (const auto& d : db::modules())
        if (d.defaultLocked && d.kind == db::ModuleKind::Module && !d.params.empty()) {
            Patch q = Patch::makeDefault();
            const u8 idx = edit::addModule(q, Location::Va, d.typeId, 0, 0);
            for (std::size_t k = 0; k < d.params.size(); ++k)
                CHECK_FALSE(isEnabled(*q.va.find(idx), k, Settings{}));
        }
}

TEST_CASE("mutate: values stay in range and patches get valid parameters")
{
    Patch p = onePatch();
    Settings s;
    s.unlocked = 0xFF;
    s.probability = 1.0;
    s.range = kMaxRange;
    Random rng(5);
    for (int round = 0; round < 50; ++round) {
        const Individual parent = capture(p, 0, &rng);
        const Individual kids[] = {randomized(p, parent, s, rng), mutated(p, parent, s, rng)};
        for (const auto& k : kids)
            for (Location loc : {Location::Va, Location::Fx})
                for (const auto& g : k.area(loc)) {
                    const auto* def = db::find(g.type);
                    for (std::size_t q = 0; q < g.values.size(); ++q) {
                        CHECK(g.values[q] >= 0.0);
                        CHECK(g.values[q] <= def->params[q].max + 0.999);
                    }
                }
        apply(p, 0, kids[round % 2]);
        for (Location loc : {Location::Va, Location::Fx})
            for (const auto& m : p.area(loc).modules)
                for (std::size_t q = 0; q < m.params[0].size(); ++q)
                    CHECK(m.params[0][q] <= m.def()->params[q].max);
    }
}

TEST_CASE("mutate: a seed makes every operation reproducible")
{
    auto run = [](std::uint32_t seed) {
        Patch p = onePatch();
        Settings s;
        s.unlocked = 0xFF;
        Random rng(seed);
        randomize(p, {}, s, rng);
        g2::mutate::mutate(p, {std::optional<u8>(2), {}}, s, rng);
        Mutator mu(p, seed);
        mu.randomize(p, 0);
        mu.mutate(p, Box{BoxKind::Population, 3});
        mu.cross(kMother, Box{BoxKind::Population, 1});
        return std::pair{savePatch(p), *mu.population[2]};
    };
    CHECK(run(7) == run(7));
    CHECK(run(7) != run(8));
}

TEST_CASE("mutate: in-place randomize respects the scope")
{
    Patch p = onePatch();
    const Patch before = p;
    Settings s;
    s.unlocked = 0xFF;
    Random rng(3);
    const u8 env = p.va.modules[1].index;
    randomize(p, {std::optional<u8>(4), {{Location::Va, env}}}, s, rng);
    for (Location loc : {Location::Va, Location::Fx})
        for (std::size_t i = 0; i < p.area(loc).modules.size(); ++i) {
            const auto& m = p.area(loc).modules[i];
            const auto& o = before.area(loc).modules[i];
            for (u8 v = 0; v < kFileVariations; ++v)
                if (v != 4 || loc != Location::Va || m.index != env)
                    CHECK(m.params[v] == o.params[v]);
        }
    CHECK(p.va.modules[1].params[4] != before.va.modules[1].params[4]);
    REQUIRE(p.settings.size() == before.settings.size()); // patch settings are never randomized
    for (std::size_t i = 0; i < p.settings.size(); ++i)
        CHECK(p.settings[i].params == before.settings[i].params);
}

TEST_CASE("mutate: interpolation and crossover mix the parents")
{
    Patch p = onePatch();
    Individual a = capture(p, 0), b = capture(p, 0);
    for (auto& g : b.va)
        for (auto& v : g.values)
            v = 0.25;
    a.morphs = {{1, 1, 3, 0, 40}};
    b.morphs = {{1, 1, 3, 0, -20}, {1, 1, 4, 2, 10}};
    const Individual mid = interpolated(a, b, 0.5);
    const auto& g = mid.va[0];
    for (std::size_t q = 0; q < g.values.size(); ++q)
        CHECK(g.values[q] == a.va[0].values[q] + (0.25 - a.va[0].values[q]) * 0.5);
    REQUIRE(mid.morphs.size() == 2);
    CHECK(mid.morphs[0].range == 10); // 40 + (-20 - 40) / 2
    CHECK(mid.morphs[1].range == 5);
    CHECK(interpolated(a, b, 1.0 / 7).morphs.size() == 2); // 10 * 1/7 truncates to 1

    Random rng(11);
    const Individual none = recombined(a, b, 0.0, rng);   // one parent for each whole area
    const Individual all = recombined(a, b, 1.0, rng);    // alternate after every parameter
    const bool fromA = none.va[0].values == a.va[0].values, fromB = none.va[0].values == b.va[0].values;
    CHECK(fromA != fromB);
    for (std::size_t q = 1; q < all.va[0].values.size(); ++q)
        CHECK((all.va[0].values[q] == a.va[0].values[q]) != (all.va[0].values[q - 1] == a.va[0].values[q - 1]));
}

TEST_CASE("mutate: Mutator boxes behave as in the dialog")
{
    Patch p = onePatch();
    Mutator mu(p, 1);
    CHECK_FALSE(mu.hasData(kMother));
    CHECK_FALSE(mu.mutate(p, kMother)); // Mutate needs a mother
    mu.randomize(p, 0);
    for (int i = 0; i < kChildren; ++i)
        CHECK(mu.population[i].has_value());
    CHECK(mu.parentCount == 1);
    CHECK(mu.focus == Box{BoxKind::Population, 0});

    // Double-click a child: it becomes the mother, six mutants follow.
    const Individual child3 = *mu.population[3];
    CHECK(mu.mutate(p, Box{BoxKind::Population, 3}));
    CHECK(*mu.population[kMotherBox] == child3);

    // Store children, cross them, copy a row to the variations.
    mu.focus = Box{BoxKind::Population, 1};
    CHECK(mu.storeFocused());
    mu.focus = Box{BoxKind::Population, 2};
    CHECK(mu.storeFocused());
    CHECK(mu.geneBank[0].has_value());
    CHECK(mu.geneBank[1].has_value());
    CHECK(mu.cross(Box{BoxKind::GeneBank, 0}, Box{BoxKind::GeneBank, 1}));
    CHECK(mu.parentCount == 2);
    CHECK(*mu.population[kFatherBox] == *mu.geneBank[1]);
    CHECK(mu.interpolate(kMother, kFather));

    mu.move(p, Box{BoxKind::GeneBank, 1}, Box{BoxKind::GeneBank, 9});
    CHECK_FALSE(mu.geneBank[1].has_value());
    CHECK(mu.geneBank[9].has_value());
    mu.copyRowToVariations(p, 0);
    const auto q = mu.geneBank[0]->quantized(Location::Va, p.va.modules[0].index);
    CHECK(p.va.modules[0].params[0] == q);
    CHECK(*mu.get(Box{BoxKind::Variation, 0}) == *mu.geneBank[0]);

    mu.clear(Box{BoxKind::Population, 2}); // children can't be deleted
    CHECK(mu.population[2].has_value());
    mu.clear(kFather);
    CHECK_FALSE(mu.population[kFatherBox].has_value());
    mu.clearRow(1);
    CHECK_FALSE(mu.geneBank[9].has_value());

    // A copy to a variation box writes the patch.
    mu.copy(p, Box{BoxKind::Population, 4}, Box{BoxKind::Variation, 5});
    CHECK(p.va.modules[0].params[5] == mu.population[4]->quantized(Location::Va, p.va.modules[0].index));

    // Removing a module from the patch removes it from the boxes.
    edit::removeModule(p, Location::Va, p.va.modules[0].index);
    mu.sync(p);
    CHECK(mu.population[4]->va.size() == p.va.modules.size());
}
