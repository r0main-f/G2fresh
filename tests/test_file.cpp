#include <catch2/catch_test_macros.hpp>

#include "g2/file.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <sstream>

using namespace g2::file;

namespace {

std::string hex(const std::string& s)
{
    if (s.empty())
        return "-";
    std::string out;
    char buf[3];
    for (unsigned char c : s) {
        std::snprintf(buf, sizeof buf, "%02x", c);
        out += buf;
    }
    return out;
}

std::string hex(const std::vector<std::uint8_t>& v)
{
    return hex(std::string(v.begin(), v.end()));
}

template <class Seq>
std::string join(const Seq& seq)
{
    if (seq.empty())
        return "-";
    std::string out;
    for (auto v : seq) {
        if (!out.empty())
            out += ' ';
        out += std::to_string(static_cast<int>(v));
    }
    return out;
}

// Same format as tools/pch2/summary.py.
std::string listing(const File& f)
{
    std::ostringstream o;
    o << "F " << f.version << ' ' << static_cast<int>(f.type) << ' ' << (f.crcValid ? 1 : 0);
    for (const auto& l : f.textHeader)
        o << ' ' << hex(l);
    o << '\n';
    for (const auto& s : f.sections) {
        char id[3];
        std::snprintf(id, sizeof id, "%02X", s.id);
        if (const auto* raw = std::get_if<RawSection>(&s.payload)) {
            o << "RAW " << id << ' ' << hex(raw->payload) << '\n';
            continue;
        }
        o << "S " << id << ' ' << int(s.tailPad) << ' ' << hex(s.trailing) << '\n';
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, PatchHeader>) {
                o << "H";
                for (auto v : p.legacy)
                    o << ' ' << int(v);
                o << ' ' << int(p.voiceCount) << ' ' << p.splitterPos << ' ' << int(p.octaveShift) << ' ';
                for (bool b : p.cablesVisible)
                    o << (b ? '1' : '0');
                o << ' ' << int(p.monoMode) << ' ' << int(p.activeVariation) << ' ' << int(p.category)
                  << ' ' << int(p.pad) << ' ' << int(p.reservedByte) << '\n';
            } else if constexpr (std::is_same_v<T, ModuleList>) {
                for (const auto& m : p.modules)
                    o << "M " << int(p.location) << ' ' << int(m.type) << ' ' << int(m.index) << ' '
                      << int(m.col) << ' ' << int(m.row) << ' ' << int(m.color) << ' ' << int(m.uprate)
                      << ' ' << int(m.locked) << ' ' << int(m.reserved) << ' ' << join(m.modes) << '\n';
            } else if constexpr (std::is_same_v<T, CurrentNotes>) {
                o << "V " << int(p.last.note) << ' ' << int(p.last.attackVel) << ' ' << int(p.last.releaseVel) << '\n';
                for (const auto& v : p.notes)
                    o << "V " << int(v.note) << ' ' << int(v.attackVel) << ' ' << int(v.releaseVel) << '\n';
            } else if constexpr (std::is_same_v<T, CableList>) {
                o << "CL " << int(p.location) << ' ' << int(p.pad) << ' ' << p.cables.size() << '\n';
                for (const auto& c : p.cables)
                    o << "C " << int(c.color) << ' ' << int(c.fromModule) << ' ' << int(c.fromConn) << ' '
                      << int(c.fromIsOutput) << ' ' << int(c.toModule) << ' ' << int(c.toConn) << '\n';
            } else if constexpr (std::is_same_v<T, ParamList>) {
                o << "PL " << int(p.location) << ' ' << int(p.variationCount) << ' ' << p.modules.size() << '\n';
                for (const auto& m : p.modules)
                    for (const auto& v : m.variations)
                        o << "P " << int(m.index) << ' ' << int(m.paramCount) << ' ' << int(v.variation)
                          << ' ' << join(v.values) << '\n';
            } else if constexpr (std::is_same_v<T, MorphMap>) {
                o << "ML " << int(p.morphCount) << ' ' << join(p.keyboardAssign) << '\n';
                for (const auto& v : p.variations) {
                    o << "MV " << int(v.variation) << ' ' << join(v.legacyDials) << '\n';
                    for (const auto& a : v.morphs)
                        o << "MA " << int(a.location) << ' ' << int(a.module) << ' ' << int(a.param) << ' '
                          << int(a.morph) << ' ' << int(a.range) << '\n';
                }
            } else if constexpr (std::is_same_v<T, KnobMap>) {
                for (std::size_t i = 0; i < p.knobs.size(); ++i) {
                    o << "K " << i;
                    if (const auto& k = p.knobs[i])
                        o << ' ' << int(k->location) << ' ' << int(k->module) << ' ' << int(k->assignType)
                          << ' ' << int(k->param) << ' ' << int(k->slot) << '\n';
                    else
                        o << " -\n";
                }
            } else if constexpr (std::is_same_v<T, CtrlMap>) {
                for (const auto& c : p.controllers)
                    o << "R " << int(c.cc) << ' ' << int(c.location) << ' ' << int(c.module) << ' '
                      << int(c.param) << '\n';
            } else if constexpr (std::is_same_v<T, CustomData>) {
                o << "DL " << int(p.location) << '\n';
                for (const auto& m : p.modules)
                    o << "D " << int(m.index) << ' ' << hex(m.bytes) << '\n';
            } else if constexpr (std::is_same_v<T, ModuleNames>) {
                o << "NL " << int(p.location) << ' ' << int(p.reserved) << '\n';
                for (const auto& n : p.names)
                    o << "N " << int(n.index) << ' ' << hex(n.name) << '\n';
            } else if constexpr (std::is_same_v<T, Textpad>) {
                o << "T " << hex(p.text) << '\n';
            } else if constexpr (std::is_same_v<T, PerfHeader>) {
                o << "PH " << int(p.unknown08) << ' ' << int(p.focusedSlot) << ' ' << int(p.globalPages) << ' '
                  << int(p.kbdRangeEnabled) << ' ' << int(p.masterClockBpm) << ' ' << int(p.unknown18) << ' '
                  << int(p.masterClockRun) << ' ' << int(p.reserved1) << ' ' << int(p.reserved2) << '\n';
                for (const auto& s : p.slots)
                    o << "PS " << hex(s.patchName) << ' ' << int(s.enabled) << ' ' << int(s.keyboard) << ' '
                      << int(s.hold) << ' ' << int(s.bank) << ' ' << int(s.program) << ' '
                      << int(s.kbdRangeLower) << ' ' << int(s.kbdRangeUpper) << ' ' << int(s.midiChannel)
                      << ' ' << int(s.reserved1) << ' ' << int(s.reserved2) << '\n';
            }
        }, s.payload);
    }
    return o.str();
}

} // namespace

TEST_CASE("every corpus file decodes fully and round-trips byte for byte")
{
    const auto files = g2test::corpusFiles();
    REQUIRE(files.size() >= 11);
    for (const auto& path : files) {
        INFO(path.string());
        const auto bytes = g2test::readBytes(path);
        const File f = read(bytes);
        CHECK(f.crcValid);
        CHECK(f.unparsedTail.empty());
        for (const auto& s : f.sections)
            CHECK_FALSE(std::holds_alternative<RawSection>(s.payload));
        if (f.type == FileType::Patch)
            CHECK(f.sections.size() == 18);
        CHECK(write(f) == bytes);
    }
}

TEST_CASE("the C++ codec decodes the same fields as the reference decoder")
{
    namespace fs = std::filesystem;
    int compared = 0;
    for (const auto& path : g2test::corpusFiles()) {
        const fs::path golden = fs::path(G2_GOLDEN_DIR) / "corpus" / (path.stem().string() + ".txt");
        if (!fs::exists(golden))
            continue;
        INFO(path.string());
        CHECK(listing(read(g2test::readBytes(path))) == g2test::readText(golden));
        ++compared;
    }
    CHECK(compared == 11);
}

TEST_CASE("a damaged file is reported, not silently accepted")
{
    auto bytes = g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2");
    SECTION("bad checksum")
    {
        bytes[bytes.size() - 1] ^= 0x01;
        const File f = read(bytes);
        CHECK_FALSE(f.crcValid);
        CHECK(write(f) == bytes); // kept as found
    }
    SECTION("not a G2 file")
    {
        const std::string junk = "Hello\r\nworld\r\n";
        CHECK_THROWS_AS(read(std::vector<std::uint8_t>(junk.begin(), junk.end())), g2::FormatError);
    }
}

TEST_CASE("container variants found in real-world files load and re-save exactly")
{
    const auto plain = g2test::readBytes(G2_CORPUS_DIR "/pch2csd/test_3osc.pch2");
    const File reference = read(plain);
    std::ptrdiff_t bodyStart = 0; // length of the text header
    for (const auto& line : reference.textHeader)
        bodyStart += static_cast<std::ptrdiff_t>(line.size() + 2);

    SECTION("MacBinary-wrapped")
    {
        std::vector<std::uint8_t> mb(128, 0);
        const std::string name = "test_3osc.pch2";
        mb[1] = static_cast<std::uint8_t>(name.size());
        std::copy(name.begin(), name.end(), mb.begin() + 2);
        const auto n = plain.size();
        mb[83] = static_cast<std::uint8_t>(n >> 24);
        mb[84] = static_cast<std::uint8_t>(n >> 16);
        mb[85] = static_cast<std::uint8_t>(n >> 8);
        mb[86] = static_cast<std::uint8_t>(n);
        mb.insert(mb.end(), plain.begin(), plain.end());
        mb.resize(mb.size() + 37, 0); // padding to the 128-byte boundary
        const File f = read(mb);
        CHECK(f.crcValid);
        CHECK(f.macBinaryHeader.size() == 128);
        CHECK(f.sections.size() == 18);
        CHECK(write(f) == mb);
    }
    SECTION("zero-padded after the checksum")
    {
        auto padded = plain;
        padded.resize(padded.size() + 91, 0);
        const File f = read(padded);
        CHECK(f.crcValid);
        CHECK(f.zeroPadding == 91);
        CHECK(f.unparsedTail.empty());
        CHECK(write(f) == padded);
    }
    SECTION("patch name instead of the text header")
    {
        const std::string name = "Three Oscs";
        std::vector<std::uint8_t> headerless(name.begin(), name.end());
        headerless.push_back(0);
        headerless.insert(headerless.end(), plain.begin() + bodyStart, plain.end());
        const File f = read(headerless);
        CHECK(f.crcValid);
        REQUIRE(f.embeddedName);
        CHECK(*f.embeddedName == name);
        CHECK(f.sections.size() == 18);
        CHECK(write(f) == headerless);
    }
    SECTION("files that are not G2 files get a clear message")
    {
        const std::string ableton = "\xab\x1e\x56\x78\x03\x28\x00\x00LiveDocument";
        try {
            read(std::vector<std::uint8_t>(ableton.begin(), ableton.end()));
            FAIL("accepted a foreign file");
        } catch (const g2::FormatError& e) {
            CHECK(std::string(e.what()).find("Ableton") != std::string::npos);
        }
    }
}
