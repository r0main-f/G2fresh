#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "g2/uprate.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <iostream>

using namespace g2;

namespace {

// Module types (data/modules.json).
constexpr u8 kInvert = 5;    // logic, all connectors dynamic yellow/orange
constexpr u8 kOscB = 7;      // static red output; Pitch inputs dynamic, Sync static red
constexpr u8 kLevMult = 44;  // In, Mod, Out all dynamic blue/red
constexpr u8 kClkDiv = 69;   // logic, dynamic yellow/orange
constexpr u8 kLfoB = 190;    // static blue output

constexpr auto VA = Location::Va;

struct Fixture {
    Patch p = Patch::makeDefault();
    u8 add(u8 type)
    {
        const u8 col = static_cast<u8>(p.va.modules.size());
        return edit::addModule(p, VA, type, col, 0);
    }
    Cable connect(u8 from, u8 fromConn, u8 to, u8 toConn, bool fromIsOutput = true)
    {
        Cable c = edit::connect(p, VA, {from, fromConn, fromIsOutput}, {to, toConn, false});
        uprate::update(p, VA);
        return c;
    }
    void disconnect(const Cable& c)
    {
        edit::disconnect(p, VA, c);
        uprate::update(p, VA);
    }
    bool up(u8 m) const { return p.va.find(m)->uprate; }
    // Colour of the (unique) cable ending at an input.
    CableColor colorInto(u8 m, u8 conn) const
    {
        for (const auto& c : p.va.cables)
            if (c.toModule == m && c.toConn == conn)
                return c.color;
        FAIL("no cable into this input");
        return CableColor::White;
    }
    Cable cableInto(u8 m, u8 conn) const
    {
        for (const auto& c : p.va.cables)
            if (c.toModule == m && c.toConn == conn)
                return c;
        FAIL("no cable into this input");
        return {};
    }
};

} // namespace

TEST_CASE("an audio output uprates a dynamic module and recolours its output cables")
{
    Fixture f;
    const u8 osc = f.add(kOscB), mult = f.add(kLevMult), next = f.add(kLevMult);
    f.connect(mult, 0, next, 0); // blue: both at control rate
    CHECK_FALSE(f.up(mult));
    CHECK(f.colorInto(next, 0) == CableColor::Blue);

    const Cable audio = f.connect(osc, 0, mult, 0);
    CHECK(audio.color == CableColor::Red);
    CHECK(f.up(mult));
    CHECK(f.up(next)); // propagates down the chain
    CHECK(f.colorInto(next, 0) == CableColor::Red);

    f.disconnect(audio);
    CHECK_FALSE(f.up(mult));
    CHECK_FALSE(f.up(next));
    CHECK(f.colorInto(next, 0) == CableColor::Blue);
}

TEST_CASE("control-rate sources and static inputs do not uprate")
{
    Fixture f;
    const u8 lfo = f.add(kLfoB), mult = f.add(kLevMult), osc = f.add(kOscB), osc2 = f.add(kOscB);
    f.connect(lfo, 0, mult, 1);
    CHECK_FALSE(f.up(mult));

    // Audio into OscB's static Sync input: no uprate.
    f.connect(osc, 0, osc2, 2);
    CHECK_FALSE(f.up(osc2));
    // Audio into its dynamic Pitch input: uprated (its output is static, so
    // nothing else changes).
    f.connect(osc, 0, osc2, 0);
    CHECK(f.up(osc2));
}

TEST_CASE("a long chain of dynamic modules is uprated end to end")
{
    Fixture f;
    const u8 osc = f.add(kOscB);
    std::vector<u8> chain;
    for (int i = 0; i < 6; ++i)
        chain.push_back(f.add(kLevMult));
    // Wire the chain back to front so the recomputation needs several passes.
    for (std::size_t i = chain.size() - 1; i > 0; --i)
        f.connect(chain[i - 1], 0, chain[i], 0);
    for (u8 m : chain)
        CHECK_FALSE(f.up(m));
    const Cable src = f.connect(osc, 0, chain.front(), 1);
    for (std::size_t i = 0; i < chain.size(); ++i) {
        CHECK(f.up(chain[i]));
        if (i > 0)
            CHECK(f.colorInto(chain[i], 0) == CableColor::Red);
    }
    f.disconnect(src);
    for (std::size_t i = 0; i < chain.size(); ++i) {
        CHECK_FALSE(f.up(chain[i]));
        if (i > 0)
            CHECK(f.colorInto(chain[i], 0) == CableColor::Blue);
    }
}

TEST_CASE("input-to-input links carry the rate of their net's output")
{
    Fixture f;
    const u8 osc = f.add(kOscB), a = f.add(kLevMult), b = f.add(kLevMult), c = f.add(kLevMult);
    // Links first: a.In -> b.In -> c.Mod, no source yet.
    f.connect(a, 0, b, 0, false);
    f.connect(b, 0, c, 1, false);
    CHECK_FALSE(f.up(b));
    CHECK_FALSE(f.up(c));
    // Feeding the first input of the link chain uprates every module on it.
    f.connect(osc, 0, a, 0);
    CHECK(f.up(a));
    CHECK(f.up(b));
    CHECK(f.up(c));
}

TEST_CASE("logic modules: audio into a dynamic logic input turns its outputs orange")
{
    Fixture f;
    const u8 osc = f.add(kOscB), inv = f.add(kInvert), div = f.add(kClkDiv), div2 = f.add(kClkDiv);
    f.connect(inv, 0, div, 0);
    f.connect(div, 0, div2, 0);
    CHECK(f.colorInto(div, 0) == CableColor::Yellow);
    f.connect(osc, 0, inv, 0);
    CHECK(f.up(inv));
    CHECK(f.colorInto(div, 0) == CableColor::Orange);
    // An orange (uprated logic) output is audio rate too.
    CHECK(f.up(div));
    CHECK(f.up(div2));
    CHECK(f.colorInto(div2, 0) == CableColor::Orange);
}

TEST_CASE("a feedback loop keeps its rate once the source is gone")
{
    Fixture f;
    const u8 osc = f.add(kOscB), a = f.add(kLevMult), b = f.add(kLevMult);
    f.connect(a, 0, b, 0);
    f.connect(b, 0, a, 0);
    CHECK_FALSE(f.up(a));
    CHECK_FALSE(f.up(b));
    const Cable src = f.connect(osc, 0, a, 1);
    CHECK(f.up(a));
    CHECK(f.up(b));
    // The original recomputes from the current bits, so the loop sustains itself.
    f.disconnect(src);
    CHECK(f.up(a));
    CHECK(f.up(b));
    // From cleared bits the loop settles at control rate.
    for (auto& m : f.p.va.modules)
        m.uprate = false;
    uprate::update(f.p, VA);
    CHECK_FALSE(f.up(a));
    CHECK_FALSE(f.up(b));
}

TEST_CASE("modules without a fed dynamic input are reset; user colours survive until the rate flips")
{
    Fixture f;
    const u8 lfo = f.add(kLfoB), osc = f.add(kOscB), a = f.add(kLevMult), b = f.add(kLevMult);
    f.p.va.find(lfo)->uprate = true; // stale bit on a fixed-rate module
    f.connect(a, 0, b, 0);
    CHECK_FALSE(f.up(lfo));

    edit::setCableColor(f.p, VA, f.cableInto(b, 0), CableColor::Green);
    f.connect(lfo, 0, a, 1); // a stays at control rate: colour kept
    CHECK(f.colorInto(b, 0) == CableColor::Green);
    f.connect(osc, 0, a, 0); // a flips to audio rate: net recoloured
    CHECK(f.colorInto(b, 0) == CableColor::Red);
}

TEST_CASE("the settings area is left alone")
{
    Patch p = Patch::makeDefault();
    const Patch before = p;
    uprate::update(p, Location::Settings);
    CHECK(p.va.modules.size() == before.va.modules.size());
}

namespace {

// Recomputing must leave a patch saved by the original editor unchanged.
// Returns false (and reports) when it does not.
bool stableUnderUpdate(const Patch& p, const std::string& what, bool fromScratch, bool report = true)
{
    bool ok = true;
    for (Location loc : {Location::Va, Location::Fx}) {
        Patch q = p;
        if (fromScratch)
            for (auto& m : q.area(loc).modules)
                m.uprate = false;
        uprate::update(q, loc);
        const Area& a = p.area(loc);
        const Area& b = q.area(loc);
        for (std::size_t i = 0; i < a.modules.size(); ++i)
            if (a.modules[i].uprate != b.modules[i].uprate) {
                if (report)
                    UNSCOPED_INFO(what << (loc == Location::Fx ? " FX" : " VA") << " module "
                                   << int(a.modules[i].index) << " stored " << a.modules[i].uprate);
                ok = false;
            }
        if (!fromScratch)
            for (std::size_t i = 0; i < a.cables.size(); ++i)
                if (a.cables[i].color != b.cables[i].color)
                    ok = false;
    }
    return ok;
}

std::vector<Patch> patchesOf(const std::filesystem::path& path)
{
    const auto bytes = g2test::readBytes(path);
    const auto f = file::read(bytes);
    if (f.type == file::FileType::Patch)
        return {Patch::fromFile(f)};
    const auto perf = Performance::fromFile(f);
    return {perf.slots.begin(), perf.slots.end()};
}

} // namespace

TEST_CASE("recomputing reproduces the stored uprate bits of the committed corpus")
{
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(G2_CORPUS_DIR "/pch2csd"))
        if (e.path().extension() == ".pch2" || e.path().extension() == ".prf2")
            files.push_back(e.path());
    const fs::path verhue = G2_SOURCE_DIR "/third_party/nord_g2_editor/Gen3/Patch/hi_hat_machine.pch2";
    if (fs::exists(verhue))
        files.push_back(verhue);
    REQUIRE_FALSE(files.empty());
    int uprated = 0;
    for (const auto& path : files)
        for (const auto& p : patchesOf(path)) {
            CHECK(stableUnderUpdate(p, path.filename().string(), false));
            // These patches have no self-sustaining loops: the bits also
            // follow from the cables alone.
            CHECK(stableUnderUpdate(p, path.filename().string(), true));
            for (const auto& m : p.va.modules)
                uprated += m.uprate;
        }
    CHECK(uprated > 0);
}

TEST_CASE("recomputing agrees with the private corpus (G2_EXTRA_CORPUS)")
{
    namespace fs = std::filesystem;
    const char* extra = std::getenv("G2_EXTRA_CORPUS");
    if (!extra || !fs::is_directory(extra))
        SKIP("G2_EXTRA_CORPUS not set");
    long areas = 0, stable = 0;
    for (const auto& e : fs::recursive_directory_iterator(extra)) {
        const auto ext = e.path().extension();
        if (ext != ".pch2" && ext != ".prf2")
            continue;
        std::vector<Patch> patches;
        try {
            patches = patchesOf(e.path());
        } catch (const std::exception&) {
            continue;
        }
        for (const auto& p : patches) {
            ++areas;
            stable += stableUnderUpdate(p, e.path().filename().string(), false, false);
        }
    }
    std::cout << "uprate: " << stable << "/" << areas << " patches unchanged by recomputation\n";
    // A few patches saved by older editors carry stale bits (19 areas in
    // 8,869 patches of the reference corpus, see re/notes/uprate.md).
    CHECK(stable >= areas * 995 / 1000);
}
