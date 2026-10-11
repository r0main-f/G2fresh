// The hardware bench pack (bench/hardware-pack, tools/benchpack): what must hold for every patch a friend loads
// into a real G2. That the committed files are what the generator writes is checked by the ctest
// benchpack_patches_are_current; what the emulated G2 makes of them, by benchpack_expected_matches_emulator.
#include <catch2/catch_test_macros.hpp>

#include "g2/edit.hpp"
#include "g2/patch.hpp"
#include "g2/patch_load.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace g2;

namespace {

std::vector<std::filesystem::path> packFiles()
{
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(G2_SOURCE_DIR "/bench/hardware-pack/patches"))
        files.push_back(e.path());
    std::sort(files.begin(), files.end());
    return files;
}

Patch patchOf(const std::filesystem::path& path)
{
    const auto loaded = g2::load(g2test::readBytes(path));
    if (const auto* perf = std::get_if<Performance>(&loaded.content))
        return perf->slots[0];
    return std::get<Patch>(loaded.content);
}

std::string typeName(const Module& m)
{
    return m.def() ? m.def()->shortName : "?";
}

} // namespace

TEST_CASE("Bench pack: every file is a version 23 patch a G2 can name and load")
{
    const auto files = packFiles();
    REQUIRE(files.size() == 17);
    for (const auto& path : files) {
        INFO(path.filename().string());
        const auto ext = path.extension().string();
        REQUIRE((ext == ".pch2" || ext == ".prf2"));
        const auto bytes = g2test::readBytes(path);
        const auto file = file::read(bytes);
        CHECK(file.version == 23);
        CHECK(file.crcValid);
        const auto loaded = g2::load(bytes);
        CHECK(loaded.convertedFrom == 0);
        CHECK(loaded.isPerformance() == (ext == ".prf2"));
        // a G2 shows the file's name: 16 characters of its own character set
        const auto name = path.stem().string();
        CHECK(name.size() <= edit::kNameLength);
        CHECK(edit::modularName(name) == name);
        CHECK(edit::fileNameForPatch(name) == name);
        // saved as the editor saves it
        const auto again = loaded.isPerformance() ? savePerformance(std::get<Performance>(loaded.content))
                                                  : savePatch(std::get<Patch>(loaded.content));
        CHECK(again == bytes);
    }
}

TEST_CASE("Bench pack: the patches play by themselves, hold nothing random and are small")
{
    for (const auto& path : packFiles()) {
        const auto name = path.stem().string();
        INFO(name);
        const Patch patch = patchOf(path);
        const bool voices = name.rfind("B11", 0) == 0 || name.rfind("B12", 0) == 0 || name.rfind("B13", 0) == 0;
        std::map<std::string, int> types;
        for (const auto* area : {&patch.va, &patch.fx})
            for (const auto& m : area->modules) {
                ++types[typeName(m)];
                // every variation holds the same values: the one selected on the synth does not matter
                for (const auto& v : m.params)
                    CHECK(v == m.params.front());
                // nothing follows the keyboard (the voice patches do: they wait for a key)
                if (!voices && m.def())
                    for (std::size_t p = 0; p < m.def()->params.size(); ++p) {
                        const std::string param = m.def()->params[p].name;
                        if (param == "KBT" || param == "KBG")
                            CHECK(m.params.front()[p] == 0);
                    }
            }
        // an output module, and no source of chance: the Noise module only where noise is the test
        CHECK(types["2-Out"] + types["4-Out"] >= 1);
        for (const auto& [type, count] : types) {
            CHECK(type.rfind("Rnd", 0) != 0);
            CHECK(type.rfind("Random", 0) != 0);
            CHECK(type != "OscNoise");
            CHECK(type != "MetNoise");
            if (type == "Noise")
                CHECK(name == "B05 White Noise");
        }
        if (types.count("DrumSynth")) { // its noise part is off
            for (const auto& m : patch.va.modules)
                if (typeName(m) == "DrumSynth")
                    for (std::size_t p = 0; p < m.def()->params.size(); ++p)
                        if (std::string(m.def()->params[p].name) == "Noise")
                            CHECK(m.params.front()[p] == 0);
        }
        CHECK(types["Keyboard"] == (voices ? 1 : 0));
        if (voices) {
            CHECK(patch.header.monoMode == 0);
            CHECK(patch.header.voiceCount == 32);
        } else {
            CHECK(patch.header.monoMode == 1); // one voice
            CHECK(patch.va.modules.size() + patch.fx.modules.size() <= 12);
            // the editor's own estimate (the synth's report is in bench/hardware-pack/expected)
            CHECK(patchload::compute(patch).vaCycles < 0.25f);
        }
    }
}

TEST_CASE("Bench pack: B08 is a performance with the master clock running at 120 BPM")
{
    const auto loaded = g2::load(g2test::readBytes(G2_SOURCE_DIR "/bench/hardware-pack/patches/B08 Clock 120.prf2"));
    REQUIRE(loaded.isPerformance());
    const auto& perf = std::get<Performance>(loaded.content);
    CHECK(perf.header.masterClockBpm == 120);
    CHECK(perf.header.masterClockRun == 1);
    CHECK(perf.header.slots[0].patchName == "B08 Clock 120");
    CHECK(perf.header.slots[0].enabled == 1);
    CHECK(perf.header.slots[1].enabled == 0);
    // one clock follows the master clock, the other runs by itself
    int master = 0, own = 0;
    for (const auto& m : perf.slots[0].va.modules)
        if (typeName(m) == "ClkGen") {
            for (std::size_t p = 0; p < m.def()->params.size(); ++p)
                if (std::string(m.def()->params[p].name) == "Source")
                    ++(m.params.front()[p] == 1 ? master : own);
        }
    CHECK(master == 1);
    CHECK(own == 1);
}
