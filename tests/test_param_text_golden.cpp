// Exhaustive check of the ParamText port against digests of the original
// editor's output (tests/golden/param_text_digests.json, produced by
// tools/paramtext/emulate.py; see re/notes/param-display.md, section 4).
// For each (table, id) the port's strings over the whole input domain are
// hashed the same way and must give the same SHA-256.
#include <catch2/catch_test_macros.hpp>

#include "g2/param_text.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Minimal SHA-256 (FIPS 180-4).
class Sha256 {
public:
    void update(const std::string& s)
    {
        for (unsigned char c : s) {
            block_[used_++] = c;
            ++length_;
            if (used_ == 64) {
                compress();
                used_ = 0;
            }
        }
    }

    std::string hex()
    {
        const std::uint64_t bits = length_ * 8;
        block_[used_++] = 0x80;
        if (used_ > 56) {
            while (used_ < 64) block_[used_++] = 0;
            compress();
            used_ = 0;
        }
        while (used_ < 56) block_[used_++] = 0;
        for (int i = 7; i >= 0; --i) block_[used_++] = static_cast<std::uint8_t>(bits >> (8 * i));
        compress();
        std::string out;
        char buf[9];
        for (std::uint32_t h : h_) {
            std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(h));
            out += buf;
        }
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress()
    {
        static constexpr std::array<std::uint32_t, 64> k = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i)
            w[i] = std::uint32_t(block_[4 * i]) << 24 | std::uint32_t(block_[4 * i + 1]) << 16 |
                   std::uint32_t(block_[4 * i + 2]) << 8 | std::uint32_t(block_[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }

    std::array<std::uint32_t, 8> h_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t length_ = 0;
};

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
    Sha256 h;
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
    Sha256 a;
    CHECK(a.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Sha256 b;
    b.update("abc");
    CHECK(b.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Sha256 c;
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
