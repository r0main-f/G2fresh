// The patch library (g2::library): what the indexer takes from a file, the cache, damaged files, the search and the
// filters; and the library window's logic without a window (ui/PatchLibraryModel.h). On the committed corpus and on
// files made here; G2_LIBRARY_BENCH=<folder> times a scan of a real library ([.bench], not run by ctest).
#include <catch2/catch_test_macros.hpp>

#include "PatchLibraryModel.h"

#include "g2/convert.hpp"
#include "g2/edit.hpp"
#include "g2/library.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>

#include "test_support.hpp"

using namespace g2;
using namespace g2::library;
namespace fs = std::filesystem;

namespace {

const fs::path kCorpus = G2_CORPUS_DIR "/pch2csd";

u8 typeNamed(const std::string& shortName)
{
    for (const auto& def : db::modules())
        if (shortName == def.shortName)
            return def.typeId;
    FAIL("no module named " << shortName);
    return 0;
}

int countOf(const Entry& e, const std::string& shortName)
{
    const u8 type = typeNamed(shortName);
    for (const auto& m : e.modules)
        if (m.type == type)
            return m.count;
    return 0;
}

// A patch with the given modules in its voice area.
Patch patchWith(std::initializer_list<const char*> modules)
{
    Patch p = Patch::makeDefault();
    u8 row = 0;
    for (const char* name : modules) {
        edit::addModule(p, Location::Va, typeNamed(name), 0, row);
        row = static_cast<u8>(row + 12);
    }
    return p;
}

void writeBytes(const fs::path& file, const std::vector<u8>& bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// An empty folder of its own for a test (ctest runs the cases side by side, each in its process), removed when the
// test ends.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name)
        : path(fs::temp_directory_path() / ("g2fresh-library-" + name + "-" + std::to_string(std::random_device{}())))
    {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct Collect : ScanSink {
    Index index;
    std::vector<std::string> found_, removed_;
    void found(std::vector<Entry> entries) override
    {
        for (auto& e : entries) {
            found_.push_back(e.path);
            index.upsert(std::move(e));
        }
    }
    void removed(std::vector<std::string> paths) override
    {
        for (auto& p : paths) {
            index.remove(p);
            removed_.push_back(std::move(p));
        }
    }
};

ScanStats scanInto(const fs::path& folder, Cache& cache, Collect& sink)
{
    const std::vector<fs::path> folders{folder};
    return scan(folders, cache, sink);
}

std::string pathText(const fs::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// A small library: patches in two folders, a performance, an old file, a damaged one.
struct Library {
    TempDir dir{"lib"};
    fs::path root = dir.path / "My Patches";
    Library()
    {
        Patch bass = patchWith({"OscA", "FltClassic", "EnvADSR", "2-Out"});
        edit::setCategory(bass, 3); // Bass
        edit::setVoices(bass, edit::VoiceMode::Poly, 6);
        bass.notes = "Fat and round. By Anna.";
        writeBytes(root / "Bass" / "Fat Bass 2.pch2", savePatch(bass));

        Patch bass10 = patchWith({"OscA", "2-Out"});
        edit::setCategory(bass10, 3);
        writeBytes(root / "Bass" / "Fat Bass 10.pch2", savePatch(bass10));

        Patch robot = patchWith({"2-In", "Vocoder", "OscB", "2-Out"});
        edit::setCategory(robot, 7); // FX
        edit::setVoices(robot, edit::VoiceMode::Mono);
        edit::addModule(robot, Location::Fx, typeNamed("Reverb"), 0, 0);
        writeBytes(root / "Voices" / "Robot.PCH2", savePatch(robot));

        Performance perf = Performance::playing(bass, "Fat Bass 2");
        perf.slots[1] = robot;
        perf.header.slots[1].enabled = 1;
        perf.header.slots[1].patchName = "Robot";
        writeBytes(root / "Live Set.prf2", savePerformance(perf));

        auto old = file::read(g2test::readBytes(kCorpus / "test_3osc.pch2"));
        old.textHeader[2] = "Version=19";
        old.version = 19;
        writeBytes(root / "Old" / "Three Osc.pch2", file::write(old));

        writeBytes(root / "Broken.pch2", {'n', 'o', 't', ' ', 'a', ' ', 'p', 'a', 't', 'c', 'h'});
        writeBytes(root / "notes.txt", {'h', 'i'});                 // not a patch file: not listed
        writeBytes(root / "Bass" / "._Fat Bass 2.pch2", {0, 5, 22}); // a macOS resource-fork file: not listed
    }
};

} // namespace

TEST_CASE("library: what a patch file tells")
{
    const Entry osc = describe(g2test::readBytes(kCorpus / "test_3osc.pch2"));
    CHECK(osc.readable());
    CHECK_FALSE(osc.performance);
    CHECK(osc.version == 23);
    CHECK_FALSE(osc.converted());
    CHECK_FALSE(osc.checksumBad);
    CHECK(osc.vaModules == 8);
    CHECK(osc.fxModules == 3);
    CHECK(osc.usesFx());
    CHECK(osc.cables == 12);
    CHECK(countOf(osc, "OscA") == 3);
    CHECK(countOf(osc, "2-Out") == 2);
    CHECK(countOf(osc, "Vocoder") == 0);
    CHECK(osc.monoMode == 1);
    CHECK(voicesText(osc) == "Mono");
    CHECK(osc.voices == 1);
    CHECK_FALSE(osc.audioIn); // an Fx-In takes the voice area's signal, not the G2's inputs
    CHECK(osc.vaCycles > 0);
    CHECK(osc.vaMemory > 0);
    CHECK(osc.fxCycles > 0);
    CHECK(osc.fitsUnexpanded());
    CHECK(moduleSummary(osc).find("OscA x3") != std::string::npos);

    // The types are sorted, each once.
    for (std::size_t i = 1; i < osc.modules.size(); ++i)
        CHECK(osc.modules[i - 1].type < osc.modules[i].type);

    CHECK(describe(g2test::readBytes(kCorpus / "test_in2in.pch2")).audioIn);
    CHECK_FALSE(describe(g2test::readBytes(kCorpus / "manyOSCA.pch2")).usesFx());
}

TEST_CASE("library: category, voices, notes and the audio inputs")
{
    Patch p = patchWith({"2-In", "OscA"});
    edit::setCategory(p, 10); // Pad
    edit::setVoices(p, edit::VoiceMode::Poly, 12);
    p.notes = "Caf\xe9 pad"; // one byte per character, as in the file
    Entry e = describe(savePatch(p));
    REQUIRE(e.readable());
    CHECK(e.category == 10);
    CHECK(e.voices == 12);
    CHECK(voicesText(e) == "12");
    CHECK(e.notes == "Caf\xc3\xa9 pad"); // UTF-8 in the index
    CHECK(e.audioIn);

    // A 2-In that listens to a bus in every variation takes nothing from the G2's inputs.
    for (auto& m : p.va.modules)
        if (m.type == typeNamed("2-In"))
            for (auto& variation : m.params)
                variation[0] = 2; // Bus 1/2
    CHECK_FALSE(describe(savePatch(p)).audioIn);

    edit::setVoices(p, edit::VoiceMode::Legato);
    e = describe(savePatch(p));
    CHECK(voicesText(e) == "Legato");
    CHECK(e.voices == 1);
}

TEST_CASE("library: a performance is its slots")
{
    Patch a = patchWith({"OscA", "2-Out"});
    edit::setVoices(a, edit::VoiceMode::Poly, 4);
    a.notes = "first";
    Patch b = patchWith({"OscA", "Vocoder", "4-In"});
    edit::setVoices(b, edit::VoiceMode::Poly, 3);
    b.notes = "second";
    Performance perf = Performance::playing(a, "Lead A");
    perf.slots[1] = b;
    perf.header.slots[1].patchName = "Talk";

    Entry e = describe(savePerformance(perf));
    REQUIRE(e.readable());
    CHECK(e.performance);
    CHECK(e.category == kNoCategory);
    CHECK(e.slotNames[0] == "Lead A");
    CHECK(e.slotNames[1] == "Talk");
    CHECK(countOf(e, "OscA") == 2);
    CHECK(countOf(e, "Vocoder") == 1);
    CHECK(e.vaModules == 5);
    CHECK(e.audioIn);
    CHECK(e.notes == "first\n\nsecond");
    CHECK(e.voices == 4); // slot B is off: it asks for nothing

    perf.header.slots[1].enabled = 1;
    e = describe(savePerformance(perf));
    CHECK(e.voices == 7);
}

TEST_CASE("library: old, damaged and foreign files")
{
    auto old = file::read(g2test::readBytes(kCorpus / "test_3osc.pch2"));
    old.textHeader[2] = "Version=19";
    old.version = 19;
    const Entry converted = describe(file::write(old));
    CHECK(converted.readable());
    CHECK(converted.version == 19);
    CHECK(converted.converted());
    CHECK(converted.vaModules == 8);

    // A wrong checksum: still described (Open asks before loading it).
    auto bytes = g2test::readBytes(kCorpus / "test_3osc.pch2");
    bytes.back() ^= 0x55;
    const Entry badCrc = describe(bytes);
    CHECK(badCrc.readable());
    CHECK(badCrc.checksumBad);
    CHECK(badCrc.vaModules == 8);

    // Nothing a file can hold makes describe() throw: every prefix of a real file, and the file with a byte changed.
    const auto good = g2test::readBytes(kCorpus / "test_3osc.pch2");
    std::size_t unreadable = 0;
    for (std::size_t n = 0; n < good.size(); n += 7)
        unreadable += describe(std::span(good).first(n)).readable() ? 0 : 1;
    CHECK(unreadable > 0);
    for (std::size_t i = 0; i < good.size(); i += 5) {
        auto damaged = good;
        damaged[i] = static_cast<u8>(damaged[i] ^ 0xFF);
        const Entry e = describe(damaged);
        CHECK((e.readable() || !e.error.empty()));
    }
    const Entry empty = describe({});
    CHECK_FALSE(empty.readable());
    CHECK_FALSE(empty.error.empty());
    CHECK(empty.modules.empty());

    const Entry missing = describeFile(fs::temp_directory_path() / "g2fresh-library-no-such-file.pch2");
    CHECK_FALSE(missing.readable());
    CHECK(missing.name() == "g2fresh-library-no-such-file");
}

TEST_CASE("library: voices by the manual's rule")
{
    auto one = [](int voices, int voiceLoad, int fxLoad, int dsps) {
        const SlotDemand d{voices, voiceLoad, fxLoad};
        return estimateVoices(std::span(&d, 1), dsps);
    };
    // Manual p121: at 33 % three voices fit a DSP (12 on a G2), at 34 % two (8).
    CHECK(one(32, 330, 0, 4) == 12);
    CHECK(one(32, 340, 0, 4) == 8);
    CHECK(one(32, 330, 0, 8) == 24);
    // A voice that takes a whole DSP: 4, or 8 with the expansion; with an FX area that takes one too, 3 or 7.
    CHECK(one(32, 1000, 0, 4) == 4);
    CHECK(one(32, 1000, 0, 8) == 8);
    CHECK(one(32, 1000, 1000, 4) == 3);
    CHECK(one(32, 1000, 1000, 8) == 7);
    // Never more than asked; the FX area shares its DSP with voices.
    CHECK(one(5, 100, 0, 4) == 5);
    CHECK(one(32, 250, 500, 4) == 14);
    // Too large for one DSP: nothing plays.
    CHECK(one(8, 1001, 0, 4) == 0);
    CHECK(one(8, 100, 1001, 4) == 0);
    // An empty voice area costs nothing.
    CHECK(one(8, 0, 300, 4) == 8);
    // Slots share the DSPs, a voice each in turn.
    const SlotDemand slots[] = {{4, 1000, 0}, {4, 500, 0}};
    CHECK(estimateVoices(slots, 4) == 5); // 3 + 2: three DSPs for the heavy slot, one for two light voices
}

TEST_CASE("library: scanning folders, and the cache")
{
    Library lib;
    const fs::path cacheFile = lib.dir.path / "cache" / "PatchLibrary.cache";

    Cache cache;
    CHECK_FALSE(cache.load(cacheFile)); // none yet
    Collect first;
    ScanStats stats = scanInto(lib.root, cache, first);
    CHECK(stats.files == 6);
    CHECK(stats.read == 6);
    CHECK(stats.reused == 0);
    CHECK(stats.unreadable == 1);
    CHECK(first.index.size() == 6);
    CHECK(first.index.unreadableCount() == 1);
    CHECK(cache.dirty());

    const Entry* robot = first.index.find(pathText(lib.root / "Voices" / "Robot.PCH2"));
    REQUIRE(robot != nullptr);
    CHECK(robot->name() == "Robot");
    CHECK(robot->folder == "My Patches/Voices");
    CHECK(robot->category == 7);
    CHECK(robot->fxModules == 1);
    CHECK(robot->audioIn);
    CHECK(robot->size == fs::file_size(lib.root / "Voices" / "Robot.PCH2"));
    CHECK(robot->mtime > 1'000'000'000);
    const Entry* top = first.index.find(pathText(lib.root / "Live Set.prf2"));
    REQUIRE(top != nullptr);
    CHECK(top->folder == "My Patches");
    CHECK(top->performance);
    const Entry* broken = first.index.find(pathText(lib.root / "Broken.pch2"));
    REQUIRE(broken != nullptr);
    CHECK_FALSE(broken->readable());
    CHECK_FALSE(broken->error.empty());

    // The cache on disk gives the same entries back.
    REQUIRE(cache.save(cacheFile));
    CHECK_FALSE(cache.dirty());
    Cache loaded;
    REQUIRE(loaded.load(cacheFile));
    CHECK(loaded.size() == 6);
    for (const auto& e : first.index.entries()) {
        const Entry* c = loaded.find(e.path, e.size, e.mtime);
        REQUIRE(c != nullptr);
        CHECK(*c == e);
    }

    // Nothing changed: everything comes from the cache (shown at once), nothing is read.
    Collect second;
    stats = scanInto(lib.root, loaded, second);
    CHECK(stats.files == 6);
    CHECK(stats.reused == 6);
    CHECK(stats.read == 0);
    CHECK(stats.unreadable == 1);
    CHECK(second.index.size() == 6);
    CHECK_FALSE(loaded.dirty());

    // A file changed (its date), one added, one gone: only those are read or dropped.
    const auto changed = lib.root / "Bass" / "Fat Bass 10.pch2";
    Patch pad = patchWith({"OscA", "OscB", "2-Out"});
    writeBytes(changed, savePatch(pad));
    fs::last_write_time(changed, fs::last_write_time(changed) + std::chrono::seconds(10));
    writeBytes(lib.root / "New" / "Fresh.pch2", savePatch(pad));
    fs::remove(lib.root / "Old" / "Three Osc.pch2");
    Collect third;
    stats = scanInto(lib.root, loaded, third);
    CHECK(stats.files == 6);
    CHECK(stats.reused == 4);
    CHECK(stats.read == 2);
    CHECK(stats.removed == 1);
    CHECK(third.removed_ == std::vector<std::string>{pathText(lib.root / "Old" / "Three Osc.pch2")});
    CHECK(third.index.size() == 6);
    CHECK(third.index.find(pathText(lib.root / "Old" / "Three Osc.pch2")) == nullptr);
    REQUIRE(third.index.find(pathText(changed)) != nullptr);
    CHECK(countOf(*third.index.find(pathText(changed)), "OscB") == 1);
    CHECK(third.index.find(pathText(lib.root / "New" / "Fresh.pch2")) != nullptr);
    CHECK(loaded.dirty());

    // A file of the same date but another size is read again too.
    auto bigger = savePatch(patchWith({"OscA", "OscB", "OscC", "2-Out"}));
    const auto date = fs::last_write_time(changed);
    writeBytes(changed, bigger);
    fs::last_write_time(changed, date);
    Collect fourth;
    stats = scanInto(lib.root, loaded, fourth);
    CHECK(stats.read == 1);
    CHECK(countOf(*fourth.index.find(pathText(changed)), "OscC") == 1);

    // A folder that is not there now (a disk not plugged in) keeps what the cache knows of it...
    const auto away = lib.dir.path / "Away";
    fs::rename(lib.root, away);
    Collect fifth;
    stats = scanInto(lib.root, loaded, fifth);
    CHECK(stats.files == 0);
    CHECK(stats.removed == 0);
    CHECK(fifth.index.size() == 6);
    CHECK(loaded.size() == 6);
    // ... and a folder no longer chosen is forgotten.
    Collect sixth;
    stats = scanInto(away, loaded, sixth);
    CHECK(stats.read == 6);
    CHECK(loaded.size() == 6);
    CHECK(sixth.index.find(pathText(away / "Live Set.prf2")) != nullptr);
    CHECK(sixth.index.find(pathText(away / "Live Set.prf2"))->folder == "Away");
}

TEST_CASE("library: a scan never changes the files, and can be stopped")
{
    Library lib;
    std::map<fs::path, std::pair<std::vector<u8>, fs::file_time_type>> before;
    for (const auto& e : fs::recursive_directory_iterator(lib.root))
        if (e.is_regular_file())
            before[e.path()] = {g2test::readBytes(e.path()), e.last_write_time()};
    Cache cache;
    Collect sink;
    scanInto(lib.root, cache, sink);
    std::size_t files = 0;
    for (const auto& e : fs::recursive_directory_iterator(lib.root))
        if (e.is_regular_file()) {
            ++files;
            REQUIRE(before.count(e.path()) == 1);
            CHECK(before[e.path()].first == g2test::readBytes(e.path()));
            CHECK((before[e.path()].second == e.last_write_time()));
        }
    CHECK(files == before.size());

    struct Stop : Collect {
        int asked = 0;
        bool cancelled() override { return ++asked > 2; }
    } stop;
    Cache fresh;
    const ScanStats stats = scanInto(lib.root, fresh, stop);
    CHECK(stats.cancelled);
    CHECK(stats.files < 6);
    CHECK(stats.removed == 0);

    // Folders chosen twice, or one inside another: each file once.
    Cache twice;
    Collect both;
    const std::vector<fs::path> folders{lib.root, lib.root / "Bass", lib.root};
    CHECK(scan(folders, twice, both).files == 6);
    CHECK(both.index.size() == 6);
}

TEST_CASE("library: a damaged cache is no cache")
{
    TempDir dir("cache");
    const auto file = dir.path / "PatchLibrary.cache";
    Cache cache;
    Entry e = describe(g2test::readBytes(kCorpus / "test_3osc.pch2"));
    e.path = "/somewhere/test_3osc.pch2";
    e.folder = "somewhere";
    e.size = 123;
    e.mtime = 456;
    cache.put(e);
    Entry bad;
    bad.path = "/somewhere/bad.pch2";
    bad.error = "not a patch";
    cache.put(bad);
    REQUIRE(cache.save(file));
    const auto bytes = g2test::readBytes(file);

    Cache again;
    REQUIRE(again.load(file));
    REQUIRE(again.find(e.path, 123, 456) != nullptr);
    CHECK(*again.find(e.path, 123, 456) == e);
    CHECK(again.find(e.path, 124, 456) == nullptr); // another size: the file changed
    CHECK(again.find(e.path, 123, 457) == nullptr);
    CHECK(*again.find(bad.path, 0, 0) == bad);

    // Cut short anywhere, or not a cache at all: load() says no and holds nothing.
    for (std::size_t n = 0; n < bytes.size(); n += 3) {
        writeBytes(file, std::vector<u8>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n)));
        Cache cut;
        CHECK_FALSE(cut.load(file));
        CHECK(cut.size() == 0);
    }
    auto other = bytes;
    other[8] = static_cast<u8>(other[8] + 1); // another index version
    writeBytes(file, other);
    CHECK_FALSE(again.load(file));
    CHECK(again.size() == 0);
}

TEST_CASE("library: search and filters")
{
    Library lib;
    Cache cache;
    Collect sink;
    scanInto(lib.root, cache, sink);
    const Index& index = sink.index;
    Marks marks;
    auto names = [&](const Query& q, SortKey key = SortKey::Name, bool descending = false) {
        std::vector<std::string> out;
        for (const auto i : index.search(q, marks, key, descending))
            out.push_back(index.entries()[i].name());
        return out;
    };
    using Names = std::vector<std::string>;

    // Everything readable, by name: "Fat Bass 2" before "Fat Bass 10".
    CHECK(names({}) == Names{"Fat Bass 2", "Fat Bass 10", "Live Set", "Robot", "Three Osc"});
    CHECK(names({}, SortKey::Name, true) == Names{"Three Osc", "Robot", "Live Set", "Fat Bass 10", "Fat Bass 2"});

    Query q;
    q.text = "vocoder"; // a module's name finds the patches (and performances) that contain it
    CHECK(names(q) == Names{"Live Set", "Robot"});
    q.text = "VOCODER robot";
    CHECK(names(q) == Names{"Live Set", "Robot"}); // the performance has a slot named Robot
    q.text = "fat bass";
    CHECK(names(q) == Names{"Fat Bass 2", "Fat Bass 10", "Live Set"}); // file names, and a slot's patch name
    q.text = "anna";
    CHECK(names(q) == Names{"Fat Bass 2", "Live Set"}); // the notes
    q.text = "voices/";
    CHECK(names(q).empty());
    q.text = "voices";
    CHECK(names(q) == Names{"Robot"}); // the folder
    q.text = "fltclassic";
    CHECK(names(q) == Names{"Fat Bass 2", "Live Set"});
    q.text = "filter classic"; // long names too
    CHECK(names(q) == Names{"Fat Bass 2", "Live Set"});
    q.text = "no such thing";
    CHECK(names(q).empty());

    q = {};
    q.type = TypeFilter::Performances;
    CHECK(names(q) == Names{"Live Set"});
    q.type = TypeFilter::Patches;
    CHECK(names(q).size() == 4);

    q = {};
    q.category = 3;
    CHECK(names(q) == Names{"Fat Bass 2", "Fat Bass 10"});
    q.text = "bass"; // the category's name is searched too, with the rest
    CHECK(names(q).size() == 2);

    q = {};
    q.modules = {typeNamed("OscA"), typeNamed("EnvADSR")};
    CHECK(names(q) == Names{"Fat Bass 2", "Live Set"});
    q.modules = {typeNamed("OscA"), typeNamed("Vocoder")};
    CHECK(names(q) == Names{"Live Set"});

    q = {};
    q.audioIn = true;
    CHECK(names(q) == Names{"Live Set", "Robot"});

    q = {};
    q.unreadable = true;
    CHECK(names(q) == Names{"Broken"});
    q.text = "broken";
    CHECK(names(q) == Names{"Broken"});

    // Stars and recently opened files.
    q = {};
    q.favourites = true;
    CHECK(names(q).empty());
    marks.setFavourite(pathText(lib.root / "Voices" / "Robot.PCH2"), true);
    CHECK(names(q) == Names{"Robot"});
    CHECK(names({}, SortKey::Favourite).front() == "Robot");
    q = {};
    q.recent = true;
    CHECK(names(q).empty());
    marks.opened(pathText(lib.root / "Live Set.prf2"));
    marks.opened(pathText(lib.root / "Bass" / "Fat Bass 2.pch2"));
    CHECK(names(q, SortKey::Recent) == Names{"Fat Bass 2", "Live Set"});

    // The other columns.
    CHECK(names({}, SortKey::Type).back() == "Live Set");
    CHECK(names({}, SortKey::Category) == Names{"Fat Bass 2", "Fat Bass 10", "Robot", "Three Osc", "Live Set"});
    CHECK(names({}, SortKey::Modules, true).front() == "Three Osc");
    CHECK(names({}, SortKey::Version).front() == "Three Osc");
    CHECK(names({}, SortKey::Folder).front() == "Live Set");
    CHECK(names({}, SortKey::Voices, true).front() == "Live Set");
}

TEST_CASE("library: the estimate filter and natural order")
{
    Index index;
    Entry fits;
    fits.path = "/lib/fits.pch2";
    fits.voices = 8;
    fits.voicesOn4 = 8;
    Entry tooMany = fits;
    tooMany.path = "/lib/too many.pch2";
    tooMany.voicesOn4 = 5;
    index.upsert(fits);
    index.upsert(tooMany);
    Query q;
    q.fitsUnexpanded = true;
    const Marks marks;
    REQUIRE(index.search(q, marks).size() == 1);
    CHECK(index.entries()[index.search(q, marks)[0]].name() == "fits");

    // upsert replaces by path; remove keeps the others findable.
    tooMany.voicesOn4 = 8;
    index.upsert(tooMany);
    CHECK(index.size() == 2);
    CHECK(index.search(q, marks).size() == 2);
    index.remove("/lib/fits.pch2");
    CHECK(index.size() == 1);
    CHECK(index.find("/lib/fits.pch2") == nullptr);
    REQUIRE(index.find("/lib/too many.pch2") != nullptr);
    index.remove("/lib/not there.pch2");
    CHECK(index.size() == 1);

    CHECK(compareNatural("Bass 2", "bass 10") < 0);
    CHECK(compareNatural("bass 10", "Bass 2") > 0);
    CHECK(compareNatural("Pad", "pad") == 0);
    CHECK(compareNatural("Pad 007", "Pad 7") == 0);
    CHECK(compareNatural("Pad", "Pad 1") < 0);
    CHECK(compareNatural("a100", "a99") > 0);
    CHECK(compareNatural("", "a") < 0);
}

TEST_CASE("library: stars and recent files as text")
{
    Marks m;
    m.setFavourite("/a/one.pch2", true);
    m.setFavourite("/a/two with spaces.pch2", true);
    m.setFavourite("/a/one.pch2", true);
    CHECK(m.favouritesText() == "/a/one.pch2\n/a/two with spaces.pch2");
    m.setFavourite("/a/one.pch2", false);
    CHECK_FALSE(m.favourite("/a/one.pch2"));
    CHECK(m.favourite("/a/two with spaces.pch2"));

    for (int i = 0; i < 60; ++i)
        m.opened("/a/" + std::to_string(i) + ".pch2");
    m.opened("/a/30.pch2"); // opened again: first, and once
    CHECK(m.recent.size() == Marks::kMaxRecent);
    CHECK(m.recentRank("/a/30.pch2") == 0);
    CHECK(m.recentRank("/a/59.pch2") == 1);
    CHECK(m.recentRank("/a/0.pch2") == -1);

    Marks again;
    again.setFavouritesText(m.favouritesText() + "\r\n\n");
    again.setRecentText(m.recentText());
    CHECK(again.favourites == m.favourites);
    CHECK(again.recent == m.recent);
}

TEST_CASE("library window: rows, selection and sorting")
{
    using g2ui::PatchLibraryModel;
    using Column = PatchLibraryModel::Column;
    Library lib;
    Cache cache;
    Collect sink;
    scanInto(lib.root, cache, sink);
    PatchLibraryModel model;
    model.index = sink.index;
    model.refresh();
    REQUIRE(model.rowCount() == 5);
    CHECK(model.selectedRow() == 0); // as the original's list: the first item after a scan
    CHECK(model.status(false) == "5 files, 1 unreadable");
    CHECK(model.status(true) == "5 files, 1 unreadable (reading the folders...)");

    // Up and Down stop at the ends.
    model.step(-1);
    CHECK(model.selectedRow() == 0);
    model.step(3);
    CHECK(model.selected()->name() == "Robot");
    model.step(10);
    CHECK(model.selectedRow() == 4);

    // The selected file stays selected when the rows change order or number.
    model.select(3);
    model.sortBy(Column::Name, false); // Z to A
    CHECK(model.descending());
    CHECK(model.selected()->name() == "Robot");
    CHECK(model.selectedRow() == 1);
    model.sortBy(Column::Date, true);
    CHECK(model.sortKey() == g2::library::SortKey::Date);
    CHECK(model.descending()); // a date's usual direction: the newest first
    model.sortBy(Column::Modules, false);
    CHECK_FALSE(model.descending());
    CHECK(model.entry(model.rowCount() - 1).name() == "Three Osc"); // the most modules last
    model.sortBy(Column::Name, true);
    CHECK_FALSE(model.descending());
    model.query.text = "robot";
    model.refresh();
    CHECK(model.rowCount() == 2);
    CHECK(model.selected()->name() == "Robot");
    CHECK(model.status(false) == "2 of 5 files, 1 unreadable");
    // Filtered away: the row at its place, not nothing.
    model.query.text = "bass";
    model.refresh();
    REQUIRE(model.selected() != nullptr);
    CHECK(model.selectedRow() == 1);
    model.query.text = "zzz";
    model.refresh();
    CHECK(model.selected() == nullptr);
    model.step(1); // nothing to select
    CHECK(model.selectedRow() == -1);

    // A star, and the "favourites" list losing its row when the star goes.
    model.query = {};
    model.refresh();
    model.toggleFavourite(model.rowOf(pathText(lib.root / "Voices" / "Robot.PCH2")));
    model.query.favourites = true;
    model.refresh();
    REQUIRE(model.rowCount() == 1);
    model.toggleFavourite(0);
    CHECK(model.rowCount() == 0);
    CHECK(model.marks.favourites.empty());

    model.query = {};
    model.query.unreadable = true;
    model.refresh();
    CHECK(model.status(false) == "1 file that cannot be read");
}

TEST_CASE("library window: what the list and the detail pane say")
{
    using g2ui::PatchLibraryModel;
    using Column = PatchLibraryModel::Column;
    Library lib;
    const Entry bass = describeFile(lib.root / "Bass" / "Fat Bass 2.pch2");
    CHECK(PatchLibraryModel::cell(bass, Column::Name) == "Fat Bass 2");
    CHECK(PatchLibraryModel::cell(bass, Column::Type) == "Patch");
    CHECK(PatchLibraryModel::cell(bass, Column::Category) == "Bass");
    CHECK(PatchLibraryModel::cell(bass, Column::Voices) == "6");
    CHECK(PatchLibraryModel::cell(bass, Column::Modules) == "4");
    CHECK(PatchLibraryModel::cell(bass, Column::Version) == "23");
    CHECK(PatchLibraryModel::cell(bass, Column::Load).back() == '%');
    CHECK(PatchLibraryModel::cell(bass, Column::Date).size() == 10);
    auto facts = PatchLibraryModel::facts(bass);
    REQUIRE(facts.size() >= 4);
    CHECK(facts[0] == "Patch, Bass, 6 voices");
    CHECK(facts[1] == "4 modules (4 in the voice area, 0 in the FX area), 0 cables");
    CHECK(facts[2].rfind("Load, estimated: one voice ", 0) == 0);
    CHECK(facts[3] == "Its 6 voices fit a G2 without expansion (4 DSPs), by this estimate");

    const Entry perf = describeFile(lib.root / "Live Set.prf2");
    CHECK(PatchLibraryModel::cell(perf, Column::Type) == "Perf");
    CHECK(PatchLibraryModel::cell(perf, Column::Category).empty());
    facts = PatchLibraryModel::facts(perf);
    CHECK(facts[0] == "Performance: A Fat Bass 2, B Robot");
    CHECK(std::find(facts.begin(), facts.end(), "Listens to the G2's audio inputs") != facts.end());

    const Entry old = describeFile(lib.root / "Old" / "Three Osc.pch2");
    facts = PatchLibraryModel::facts(old);
    CHECK(facts[0] == "Patch, No Cat, Mono");
    CHECK(std::find(facts.begin(), facts.end(), "Format version 19: converted when it is opened") != facts.end());

    const Entry broken = describeFile(lib.root / "Broken.pch2");
    CHECK(PatchLibraryModel::cell(broken, Column::Name) == "Broken");
    CHECK(PatchLibraryModel::cell(broken, Column::Type) == "Unreadable");
    CHECK(PatchLibraryModel::cell(broken, Column::Voices).empty());
    facts = PatchLibraryModel::facts(broken);
    CHECK(facts[0].rfind("Cannot be read: ", 0) == 0);

    Entry heavy = bass;
    heavy.voices = 12;
    heavy.voicesOn4 = 8;
    heavy.voicesOn8 = 12;
    facts = PatchLibraryModel::facts(heavy);
    CHECK(facts[3] == "A G2 without expansion (4 DSPs) would play about 8 of its 12 voices, an expanded one 12");
    CHECK(PatchLibraryModel::percent(334) == "33%");
    CHECK(PatchLibraryModel::percent(335) == "34%");
}

// Not run by ctest: G2_LIBRARY_BENCH=<a folder of patches> ./g2tests "[.bench]" times a scan without a cache, then
// with it.
TEST_CASE("library: scan timing", "[.bench]")
{
    const char* folder = std::getenv("G2_LIBRARY_BENCH");
    if (folder == nullptr)
        SKIP("set G2_LIBRARY_BENCH to a folder of patches");
    TempDir dir("bench");
    const auto cacheFile = dir.path / "PatchLibrary.cache";
    auto ms = [](auto from) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
    };
    for (const char* pass : {"without a cache", "with the cache", "with the cache again"}) {
        const auto start = std::chrono::steady_clock::now();
        Cache cache;
        cache.load(cacheFile);
        const double loaded = ms(start);
        Collect sink;
        const ScanStats stats = scanInto(folder, cache, sink);
        const double scanned = ms(start);
        if (cache.dirty())
            cache.save(cacheFile);
        std::cout << pass << ": " << stats.files << " files (" << stats.read << " read, " << stats.reused << " from the cache, "
                  << stats.unreadable << " unreadable) in " << scanned << " ms (cache load " << loaded << " ms, save "
                  << ms(start) - scanned << " ms)\n";
        CHECK(sink.index.size() == stats.files);
    }
}
