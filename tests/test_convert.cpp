// Conversion of old file versions (g2::convert, re/notes/pch2-format.md §7).
//
// The golden digests (tests/golden/convert.tsv) are the original editor's own
// conversions, run under emulation by tools/convert/emulate.py: each corpus
// patch is declared as an older version (12..22) and converted to 23 by
// Clavia's CFileConverter, then written by its CPatchFile_23 writer.
// Declarations that drive the original into undefined behaviour (a parameter
// removed past the end of a module's list, which no real file of that version
// can cause) are listed as comments and not compared.
#include <catch2/catch_test_macros.hpp>

#include "g2/convert.hpp"
#include "g2/patch.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <sstream>

#include "sha256.hpp"
#include "test_support.hpp"

using namespace g2;

namespace {

// The same file declared as `version`: the text header's version line and the
// version word. Before 13 a patch has no settings custom data (the first 5B
// section of each patch).
file::File asVersion(file::File f, std::uint16_t version)
{
    f.textHeader[2] = "Version=" + std::to_string(version);
    f.version = version;
    if (version < 13) {
        std::vector<file::Section> out;
        int customs = 0;
        for (auto& s : f.sections) {
            if (s.id == file::kPatchHeader)
                customs = 0;
            if (s.id == file::kCustomData && customs++ == 0)
                continue;
            out.push_back(std::move(s));
        }
        f.sections = std::move(out);
    }
    return f;
}

std::vector<std::uint8_t> bytesOf(const file::File& f) { return file::write(f); }

// The digest the golden file holds: the converted file from the version word
// on, with the release velocities the v1.62 writer writes (the attack
// velocity, CCurrentVoiceData_11::WriteStream @00064aca).
std::string digest(file::File f)
{
    for (auto& s : f.sections)
        if (auto* n = std::get_if<file::CurrentNotes>(&s.payload)) {
            n->last.releaseVel = n->last.attackVel;
            for (auto& v : n->notes)
                v.releaseVel = v.attackVel;
        }
    f.textHeader.clear();
    const auto bytes = file::write(f);
    g2test::Sha256 h;
    h.update(std::string(bytes.begin(), bytes.end()));
    return h.hex();
}

const Module* findType(const Area& a, u8 type)
{
    auto it = std::find_if(a.modules.begin(), a.modules.end(), [&](const Module& m) { return m.type == type; });
    return it == a.modules.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("conversion reproduces the original editor's (golden digests)")
{
    namespace fs = std::filesystem;
    std::ifstream in(fs::path(G2_GOLDEN_DIR) / "convert.tsv");
    REQUIRE(in);
    std::map<std::string, file::File> files;
    for (const auto& p : g2test::corpusFiles())
        files.emplace(p.filename().string(), file::read(g2test::readBytes(p)));
    int compared = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        std::string name, sha;
        int version = 0;
        row >> name >> version >> sha;
        auto it = files.find(name);
        if (it == files.end())
            continue;
        INFO(name << " as version " << version);
        const auto old = asVersion(it->second, static_cast<std::uint16_t>(version));
        const auto converted = convert::toCurrent(file::read(bytesOf(old)));
        CHECK(converted.version == 23);
        CHECK(digest(converted) == sha);
        ++compared;
    }
    CHECK(compared >= 100);
}

TEST_CASE("an old file loads converted, and saves as version 23 with the v1.62 header")
{
    const auto v23 = file::read(g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2"));
    const auto bytes = bytesOf(asVersion(v23, 19));
    const Loaded loaded = load(bytes);
    CHECK(loaded.convertedFrom == 19);
    const auto saved = savePatch(std::get<Patch>(loaded.content));
    const auto again = file::read(saved);
    CHECK(again.version == 23);
    CHECK(again.textHeader == file::defaultTextHeader(file::FileType::Patch));
    CHECK(again.textHeader[3] == "Info=BUILD 320");
    // Converted once: loading the saved file converts nothing.
    CHECK(load(saved).convertedFrom == 0);
    CHECK(savePatch(loadPatch(saved)) == saved);
    // A current file is not converted and still saves byte for byte.
    const auto current = g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2");
    CHECK(load(current).convertedFrom == 0);
    CHECK(savePatch(loadPatch(current)) == current);
}

TEST_CASE("version 11 and unknown versions are not converted")
{
    const auto v23 = file::read(g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2"));
    // CPatchFile_11::Convert @00052c70.
    auto v11 = asVersion(v23, 12);
    v11.textHeader[2] = "Version=11";
    v11.version = 11;
    CHECK_THROWS_AS(loadPatch(bytesOf(v11)), convert::UnsupportedVersion);
    try {
        loadPatch(bytesOf(v11));
    } catch (const convert::UnsupportedVersion& e) {
        CHECK(std::string(e.what()) == "Unsupported file version");
    }
    CHECK_THROWS_AS(loadPatch(bytesOf(asVersion(v23, 24))), convert::UnsupportedVersion);
    // Version 12 converts (17 sections: no settings custom data, which stays
    // empty: ConvertPatch12to13 @000480c6).
    const Patch p = loadPatch(bytesOf(asVersion(v23, 12)));
    CHECK(p.settingsCustomData.empty());
}

TEST_CASE("the steps' visible effects")
{
    Patch p = Patch::makeDefault();
    auto add = [&](Area& a, u8 type, u8 index, std::vector<u8> modes = {}) {
        Module m;
        m.type = type;
        m.index = index;
        m.modes = std::move(modes);
        if (const auto* def = m.def(); def && !def->params.empty()) {
            std::vector<u8> values;
            for (const auto& d : def->params)
                values.push_back(d.defaultValue);
            m.params.assign(p.variationCount, values);
        }
        m.name = "m" + std::to_string(index);
        a.modules.push_back(m);
    };
    add(p.va, 3, 1);          // 4-Out: locked by default (22->23)
    add(p.va, 0x45, 2, {0});  // gets a second selector value (21->22)
    add(p.va, 0x4D, 3);       // dropped by 21->22 (CopyModules), with its cables
    add(p.va, 0x04, 4);
    p.va.cables.push_back({CableColor::Red, 3, 0, true, 4, 0, std::nullopt});
    p.va.cables.push_back({CableColor::Blue, 2, 0, true, 4, 1, std::nullopt});
    for (auto& m : p.va.modules)
        m.locked = false;
    // A morph on group 4 (Sust.Pd) in variation 0; the morph dials of groups 4 and 5.
    p.morphs[0].assigns.push_back({1, 4, 0, 4, 20});
    for (auto& v : p.settings[0].params) {
        v[4] = 11;
        v[5] = 22;
    }
    const auto v23 = p.toFile();

    SECTION("22 -> 23: lock flags from the type")
    {
        const Patch q = Patch::fromFile(file::read(bytesOf(asVersion(v23, 22))));
        CHECK(findType(q.va, 3)->locked);
        CHECK(findType(q.va, 4)->locked);
        CHECK(findType(q.va, 0x45)->locked);
        REQUIRE(findType(q.va, 0x4D) != nullptr);
        CHECK_FALSE(findType(q.va, 0x4D)->locked);
    }
    SECTION("21 -> 22: selector added, type 77 dropped with its cables")
    {
        const Patch q = Patch::fromFile(file::read(bytesOf(asVersion(v23, 21))));
        CHECK(findType(q.va, 0x45)->modes == std::vector<u8>{0, 0});
        CHECK(findType(q.va, 0x4D) == nullptr);
        REQUIRE(q.va.cables.size() == 1);
        CHECK(q.va.cables[0].fromModule == 2);
    }
    SECTION("20 -> 21: morph groups 4 and 5 swap")
    {
        const Patch q = Patch::fromFile(file::read(bytesOf(asVersion(v23, 20))));
        REQUIRE(q.morphs[0].assigns.size() == 1);
        CHECK(q.morphs[0].assigns[0].morph == 5);
        CHECK(q.settings[0].params[0][4] == 22);
        CHECK(q.settings[0].params[0][5] == 11);
    }
}
