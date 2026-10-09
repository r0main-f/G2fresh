#include <catch2/catch_test_macros.hpp>

#include "g2/patch.hpp"
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
