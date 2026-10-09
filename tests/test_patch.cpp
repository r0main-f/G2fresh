#include <catch2/catch_test_macros.hpp>

#include "g2/patch.hpp"

#include <algorithm>
#include "test_support.hpp"

using namespace g2;

TEST_CASE("loading and saving an unedited patch reproduces the file byte for byte")
{
    const auto files = g2test::corpusFiles();
    REQUIRE(files.size() >= 11);
    for (const auto& path : files) {
        if (path.extension() != ".pch2")
            continue;
        INFO(path.string());
        const auto bytes = g2test::readBytes(path);
        const Patch p = loadPatch(bytes);
        CHECK(savePatch(p) == bytes);
    }
}

TEST_CASE("the model exposes modules, parameters and cables")
{
    const Patch p = loadPatch(g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2"));
    REQUIRE(p.va.modules.size() == 8);
    REQUIRE(p.fx.modules.size() == 3);
    const Module* m = p.va.find(1);
    REQUIRE(m);
    CHECK(m->def() != nullptr);
    CHECK(m->name.rfind(m->def()->shortName, 0) == 0); // default names start with the short name
    CHECK(m->params.size() == static_cast<std::size_t>(p.variationCount));
    CHECK(m->params.front().size() == m->def()->params.size());
    for (const auto& c : p.va.cables) {
        CHECK(p.va.find(c.fromModule));
        CHECK(p.va.find(c.toModule));
    }
}

TEST_CASE("a new patch matches what the original editor writes")
{
    const Patch fresh = Patch::makeDefault();
    const auto bytes = savePatch(fresh);
    CHECK(savePatch(loadPatch(bytes)) == bytes);

    // Settings values, morph labels and controller defaults are identical to
    // those of patches made with the original editor.
    const file::File reference = file::read(g2test::readBytes(G2_CORPUS_DIR "/pch2csd/Gleb2.pch2"));
    const auto sections = fresh.toSections();
    for (std::size_t i : {std::size_t{6}, std::size_t{11}, std::size_t{12}}) {
        INFO("section " << i);
        CHECK(file::encodeSection(sections[i]) == file::encodeSection(reference.sections[i]));
    }
    CHECK(fresh.header.voiceCount == 2);
    CHECK(fresh.va.modules.empty());
}

TEST_CASE("a performance built from four patches saves and reloads unchanged")
{
    const auto files = g2test::corpusFiles();
    Performance perf;
    for (std::size_t i = 0; i < 4; ++i)
        perf.slots[i] = loadPatch(g2test::readBytes(files[i]));
    perf.header.slots[0].patchName = "Slot A";
    perf.header.slots[1].patchName = "Slot B";
    perf.globalKnobs[0] = KnobAssign{1, 1, 0, 0, 2};

    const auto bytes = savePerformance(perf);
    const file::File raw = file::read(bytes);
    CHECK(raw.crcValid);
    CHECK(raw.sections.size() == 74);
    const Performance again = loadPerformance(bytes);
    CHECK(again.header.slots[1].patchName == "Slot B");
    CHECK(again.globalKnobs[0]->slot == 2);
    CHECK(savePerformance(again) == bytes);
    // Slots carry no text header of their own; restore the original's to
    // compare whole files.
    for (std::size_t i = 0; i < 4; ++i) {
        Patch slot = again.slots[i];
        slot.textHeader = perf.slots[i].textHeader;
        CHECK(savePatch(slot) == g2test::readBytes(files[i]));
    }
}

TEST_CASE("damaged or foreign files are rejected with a clear error")
{
    auto bytes = g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2");
    bytes[bytes.size() - 1] ^= 0x01;
    CHECK_THROWS_AS(loadPatch(bytes), FormatError);
    CHECK_THROWS_AS(loadPerformance(g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2")), FormatError);
}

TEST_CASE("the model keeps file details other writers produce")
{
    // A patch as the synth or other tools write it: non-zero padding bits,
    // names in module-list order, an empty parameter list declaring 9 variations.
    file::File f = file::read(g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2"));
    auto& vaNames = std::get<file::ModuleNames>(f.sections[15].payload);
    std::reverse(vaNames.names.begin(), vaNames.names.end());
    // Set a padding bit on the first section whose last byte has room for one.
    bool padded = false;
    for (auto& s : f.sections) {
        s.tailPad = 1;
        try {
            file::encodeSection(s);
            padded = true;
            break;
        } catch (const std::invalid_argument&) {
            s.tailPad = 0;
        }
    }
    REQUIRE(padded);
    file::File g = f;
    auto& fxParams = std::get<file::ParamList>(g.sections[8].payload);
    fxParams.modules.clear();
    fxParams.variationCount = 9;

    for (const auto* variant : {&f, &g}) {
        const auto bytes = file::write(*variant);
        const Patch p = loadPatch(bytes);
        CHECK(savePatch(p) == bytes);
    }
}

TEST_CASE("load() goes by the file's content, not its name")
{
    const auto patchBytes = g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2");
    CHECK_FALSE(load(patchBytes).isPerformance());
    Performance perf;
    for (auto& slot : perf.slots)
        slot = loadPatch(patchBytes);
    CHECK(load(savePerformance(perf)).isPerformance());

    auto damaged = patchBytes;
    damaged.back() ^= 0x01;
    CHECK_THROWS_AS(load(damaged), ChecksumError);
    CHECK_NOTHROW(load(damaged, {true}));
}
