// Patch load (g2::patchload) against the original editor's CPatchLoad, run in an
// emulator over the corpus (tests/golden/patch_load.tsv, produced by
// tools/patchload/emulate.py; see re/notes/patch-load.md).
#include <catch2/catch_test_macros.hpp>

#include "g2/patch_load.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "test_support.hpp"

using namespace g2;
using namespace g2::patchload;

namespace {

struct GoldenRow {
    std::string file;
    int slot = 0;
    std::string area;
    std::vector<std::uint32_t> raw;   // cyclesA cyclesB zp xA yA pA xB yB pB dynRam q r
    std::vector<float> percent;       // Cycles X Y P ZP RAM Q R Critical
    int criticalType = 0;
    long freeDynamicRam = 0;
};

std::vector<GoldenRow> readGolden()
{
    std::ifstream f(G2_GOLDEN_DIR "/patch_load.tsv");
    std::vector<GoldenRow> rows;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in(line);
        std::vector<std::string> cols;
        std::string c;
        while (std::getline(in, c, '\t'))
            cols.push_back(c);
        REQUIRE(cols.size() == 3 + 12 + 9 + 2);
        GoldenRow r;
        r.file = cols[0];
        r.slot = std::stoi(cols[1]);
        r.area = cols[2];
        for (int i = 0; i < 12; ++i)
            r.raw.push_back(static_cast<std::uint32_t>(std::stoul(cols[3 + i])));
        for (int i = 0; i < 9; ++i)
            r.percent.push_back(std::strtof(cols[15 + i].c_str(), nullptr)); // exact hex floats
        r.criticalType = std::stoi(cols[24]);
        r.freeDynamicRam = std::stol(cols[25]);
        rows.push_back(r);
    }
    return rows;
}

std::vector<std::uint32_t> rawOf(const ResourceSpec& s)
{
    return {s.cyclesA, s.cyclesB, s.zpMem, s.xMemA, s.yMemA, s.pMemA,
            s.xMemB, s.yMemB, s.pMemB, s.dynRam, s.qMem, s.rMem};
}

std::vector<float> percentOf(const AreaLoad& a)
{
    return {a.cycles, a.xMem, a.yMem, a.pMem, a.zpMem, a.dynRam, a.qMem, a.rMem, a.criticalPercent};
}

std::vector<Patch> patchesOf(const std::filesystem::path& path)
{
    const auto loaded = g2::load(g2test::readBytes(path), {.ignoreChecksum = true});
    if (!loaded.isPerformance())
        return {std::get<Patch>(loaded.content)};
    const auto& perf = std::get<Performance>(loaded.content);
    return {perf.slots.begin(), perf.slots.end()};
}

Module mod(u8 type, bool uprate = false)
{
    Module m;
    m.type = type;
    m.uprate = uprate;
    return m;
}

} // namespace

TEST_CASE("patch load matches the original editor on the corpus (emulated CPatchLoad)")
{
    const auto golden = readGolden();
    REQUIRE(golden.size() >= 20);
    std::map<std::string, std::filesystem::path> byName;
    for (const auto& p : g2test::corpusFiles())
        byName[p.filename().string()] = p;

    int checked = 0;
    for (const auto& g : golden) {
        auto it = byName.find(g.file);
        if (it == byName.end())
            continue; // e.g. a private patch not present here
        INFO(g.file << " slot " << g.slot << " " << g.area);
        const auto patches = patchesOf(it->second);
        REQUIRE(g.slot < static_cast<int>(patches.size()));
        const Load l = compute(patches[static_cast<std::size_t>(g.slot)]);
        const AreaLoad& a = g.area == "va" ? l.va : l.fx;
        CHECK(rawOf(a.total) == g.raw);
        CHECK(percentOf(a) == g.percent); // bit-exact
        CHECK(static_cast<int>(a.critical) == g.criticalType);
        CHECK(l.freeDynamicRam == g.freeDynamicRam);
        CHECK((g.area == "va" ? l.vaCycles : l.fxCycles) == a.cycles / 100.0f);
        CHECK((g.area == "va" ? l.vaMemory : l.fxMemory) == a.criticalPercent / 100.0f);
        ++checked;
    }
    CHECK(checked >= 20);
}

TEST_CASE("module figures: table lookup and uprate folding")
{
    // FltClassic (92): A 34 cycles, B 40; X/Y/P A 6/4/34, B 4/4/41; 263 words of RAM.
    const ResourceSpec* s = moduleSpec(92);
    REQUIRE(s);
    CHECK(s->cyclesA == 34);
    CHECK(s->cyclesB == 40);
    CHECK(s->dynRam == 263);
    const ResourceSpec up = moduleCost(92, true);
    CHECK(up.cyclesA == 74);
    CHECK(up.cyclesB == 0);
    CHECK(up.xMemA == 10);
    CHECK(up.yMemA == 8);
    CHECK(up.pMemA == 75);
    CHECK(up.xMemB + up.yMemB + up.pMemB == 0);
    CHECK(up.dynRam == 263);
    CHECK(moduleCost(92, false) == *s);
    CHECK(moduleSpec(2) == nullptr);      // no such type
    CHECK(moduleCost(2, false) == ResourceSpec{});
    REQUIRE(moduleSpec(126));             // Name bar: _kNameModuleSize
    CHECK(moduleSpec(126)->dynRam == 71);
    // Morph's table sets byte +5, which the editor never sums.
    CHECK(moduleCost(6, false).unused5 == 0);
}

TEST_CASE("an area's load: B code at a quarter of the cycles, cables in dynamic RAM")
{
    Patch p;
    p.va.modules = {mod(92), mod(92, true), mod(7)};
    p.va.cables.resize(5);
    const Load l = compute(p);
    // cycles: A = 34 + 74 + 6 = 114, B = 40 -> (114 + 10) / 1371
    CHECK(l.va.total.cyclesA == 114);
    CHECK(l.va.total.cyclesB == 40);
    CHECK(l.va.cycles == static_cast<float>(124.0 * 100.0 / 1371.0));
    CHECK(l.va.total.dynRam == 263 + 263 + 423 + 5 * 3);
    CHECK(l.freeDynamicRam == 131072 - static_cast<std::int32_t>(l.va.total.dynRam));
    // P: A 34 + 75 + 7, B 41 -> 157 of 6498 (2.42 %) beats ZP: 3 of 128 (2.34 %).
    CHECK(l.va.zpMem == static_cast<float>(3 * 100.0 / 128.0));
    CHECK(l.va.critical == Resource::PMem);
    CHECK(l.va.criticalPercent == static_cast<float>(157 * 100.0 / 6498.0));
    CHECK(l.vaMemory == l.va.criticalPercent / 100.0f);
    CHECK(l.fx.total == ResourceSpec{});
    CHECK(l.fxCycles == 0);
    CHECK(l.fxMemory == 0);
    CHECK_FALSE(l.overloaded());
}

TEST_CASE("critical resource: X wins ties, later resources must be strictly larger")
{
    ResourceSpec t;
    CHECK(evaluate(t).critical == Resource::XMem);
    t.cyclesA = 5000; // cycles never count as the critical resource
    CHECK(evaluate(t).critical == Resource::XMem);
    CHECK(evaluate(t).cycles > 100.0f);
    t.qMem = 2601; // 1.0000 %
    t.rMem = 2;    // 0.78 %
    CHECK(evaluate(t).critical == Resource::QMem);
    t.rMem = 3; // 1.17 %
    CHECK(evaluate(t).critical == Resource::RMem);
    CHECK(evaluate(t).criticalPercent == evaluate(t).rMem);
}

TEST_CASE("the editor's dynamic-RAM checks")
{
    Patch p;
    CHECK(canAddModule(p, Location::Va, 92));
    CHECK(canAddModule(p, Location::Fx, 92));
    CHECK_FALSE(canAddModule(p, Location::Settings, 92));
    CHECK_FALSE(canAddModule(p, Location::Va, 2)); // unknown type
    CHECK(canAddCable(p));

    // 127 modules is the most an area holds.
    p.va.modules.assign(127, mod(44));
    CHECK_FALSE(canAddModule(p, Location::Va, 44));
    CHECK(canAddModule(p, Location::Fx, 44));

    // Fill the dynamic RAM with cables: 3 words each.
    Patch q;
    q.va.cables.resize(131072 / 3 - 1); // 131067 used, 5 free
    CHECK(compute(q).freeDynamicRam == 5);
    CHECK(canAddCable(q));
    CHECK(canAddModule(q, Location::Va, 44) == false); // LevMult needs 71
    q.va.cables.resize(131072 / 3); // 2 free
    CHECK_FALSE(canAddCable(q));

    const std::vector<Module> clip{mod(92), mod(7)};
    CHECK(dynamicSize(clip, 2) == 263 + 423 + 6);
    CHECK(canPaste(p, dynamicSize(clip, 2)));
    CHECK_FALSE(canPaste(q, dynamicSize(clip, 2)));
}

TEST_CASE("synth reports (molecule 72) decode to the same resource figures")
{
    // VA, cyclesA 300 (2,44), cyclesB 129 (1,1), zp 13, +5 0, X/Y/P A 24/6/46,
    // B 1/1/1, dynRam 1715 (13,51), Q 0x00010002, R 4.
    const std::vector<std::uint8_t> payload{
        0x01, 0x02, 0x2C, 0x01, 0x01, 0x0D, 0x00, 0x00, 0x00, 0x18, 0x00, 0x06, 0x00, 0x2E,
        0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x0D, 0x33, 0x00, 0x01, 0x00, 0x02, 0x00, 0x04};
    const auto r = parseReport(payload);
    REQUIRE(r);
    CHECK(r->area == Location::Va);
    CHECK(r->spec.cyclesA == 300);
    CHECK(r->spec.cyclesB == 129);
    CHECK(r->spec.zpMem == 13);
    CHECK(r->spec.xMemA == 24);
    CHECK(r->spec.pMemA == 46);
    CHECK(r->spec.pMemB == 1);
    CHECK(r->spec.dynRam == 1715);
    CHECK(r->spec.qMem == 0x00010002u);
    CHECK(r->spec.rMem == 4);
    CHECK_FALSE(parseReport(std::span(payload).first(27)));

    const Load l = fromReports(r->spec, ResourceSpec{});
    CHECK(l.va.cycles == static_cast<float>((300 + 129 * 0.25) * 100.0 / 1371.0));
    CHECK(l.vaMemory == l.va.criticalPercent / 100.0f);
}

TEST_CASE("synth load reports encode and decode back")
{
    patchload::Report r;
    r.area = Location::Va;
    r.spec.cyclesA = 1234;
    r.spec.cyclesB = 77;
    r.spec.zpMem = 99;
    r.spec.xMemA = 4000;
    r.spec.pMemB = 12;
    r.spec.dynRam = 9000;
    r.spec.qMem = 200000;
    r.spec.rMem = 33;
    const auto bytes = patchload::encodeReport(r);
    REQUIRE(bytes.size() == 28);
    const auto back = patchload::parseReport(bytes);
    REQUIRE(back);
    CHECK(back->area == Location::Va);
    CHECK(back->spec == r.spec);
}
