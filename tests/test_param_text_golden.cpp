// Exhaustive check of the ParamText port against digests of the original
// editor's output (tests/golden/param_text_digests.json, produced by
// tools/paramtext/emulate.py; see re/notes/param-display.md, section 4).
// For each (table, id) the port's strings over the whole input domain are
// hashed the same way and must give the same SHA-256.
#include <catch2/catch_test_macros.hpp>

#include "g2/param_text.hpp"
#include "sha256.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Entry {
    int table = 0;
    int id = 0;
    std::string function;
    std::vector<std::pair<int, int>> domain;
    std::string sha256;
};

// The digest file is written one entry per line, e.g.
// {"table": 2, "id": 103, "function": "LFOFreq", "domain": [[0, 127], [0, 7]], "sha256": "..."}
std::vector<Entry> loadEntries(const std::string& path)
{
    std::vector<Entry> out;
    std::ifstream in(path);
    std::string line;
    auto intAfter = [](const std::string& s, const char* key) {
        return std::stoi(s.substr(s.find(key) + std::string(key).size()));
    };
    auto strAfter = [](const std::string& s, const char* key) {
        const std::size_t b = s.find(key) + std::string(key).size();
        return s.substr(b, s.find('"', b) - b);
    };
    while (std::getline(in, line)) {
        if (line.find("\"table\": ") == std::string::npos) continue;
        Entry e;
        e.table = intAfter(line, "\"table\": ");
        e.id = intAfter(line, "\"id\": ");
        e.function = strAfter(line, "\"function\": \"");
        e.sha256 = strAfter(line, "\"sha256\": \"");
        std::size_t p = line.find("\"domain\": [") + 11;
        while (line[p] == '[' || line[p] == ',' || line[p] == ' ') {
            if (line[p] != '[') {
                ++p;
                continue;
            }
            const std::size_t comma = line.find(',', p);
            const std::size_t close = line.find(']', p);
            e.domain.emplace_back(std::stoi(line.substr(p + 1, comma - p - 1)),
                                  std::stoi(line.substr(comma + 1, close - comma - 1)));
            p = close + 1;
        }
        out.push_back(std::move(e));
    }
    return out;
}

std::string digest(const Entry& e)
{
    namespace pt = g2::paramtext;
    g2test::Sha256 h;
    std::vector<int> v(e.domain.size());
    // Odometer over the domain, last argument fastest.
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = e.domain[i].first;
    for (;;) {
        std::string s;
        switch (e.table) {
        case 1: s = pt::formatSingle(e.id, std::uint8_t(v[0])); break;
        case 2: s = pt::formatDual(e.id, std::uint8_t(v[0]), std::uint8_t(v[1])); break;
        default: s = pt::formatTriple(e.id, std::uint8_t(v[0]), std::uint8_t(v[1]), std::uint8_t(v[2])); break;
        }
        h.update(s + "\n");
        std::size_t k = v.size();
        while (k > 0) {
            --k;
            if (v[k] < e.domain[k].second) {
                ++v[k];
                break;
            }
            v[k] = e.domain[k].first;
            if (k == 0) return h.hex();
        }
    }
}

} // namespace

TEST_CASE("sha256 self-test")
{
    g2test::Sha256 a;
    CHECK(a.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    g2test::Sha256 b;
    b.update("abc");
    CHECK(b.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    g2test::Sha256 c;
    c.update("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    CHECK(c.hex() == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("param text: port reproduces the original over the whole golden domain")
{
    const auto entries = loadEntries(std::string(G2_GOLDEN_DIR) + "/param_text_digests.json");
    REQUIRE(entries.size() >= 230);
    for (const Entry& e : entries) {
        INFO("table " << e.table << " id " << e.id << " " << e.function);
        CHECK(std::string(g2::paramtext::functionName(e.id, e.table)) == e.function);
        CHECK(digest(e) == e.sha256);
    }
}
