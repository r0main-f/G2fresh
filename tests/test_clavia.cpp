#include <catch2/catch_test_macros.hpp>

#include "g2/clavia/download.hpp"
#include "g2/clavia/skin.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <cstdio>
#include <string>

using namespace g2::clavia;

namespace {

void put16(Bytes& b, std::size_t o, std::uint32_t v)
{
    b[o] = std::uint8_t(v >> 8);
    b[o + 1] = std::uint8_t(v);
}

void put32(Bytes& b, std::size_t o, std::uint32_t v)
{
    for (int k = 0; k < 4; ++k)
        b[o + std::size_t(k)] = std::uint8_t(v >> (24 - 8 * k));
}

// A resource file holding `res` (one resource per type, enough here).
Bytes makeResources(const std::vector<Resource>& res)
{
    Bytes data;
    const std::size_t typeList = 28, refs = typeList + 2 + 8 * res.size();
    Bytes map(refs + 12 * res.size(), 0);
    put16(map, 24, std::uint32_t(typeList));
    put16(map, 26, std::uint32_t(map.size()));
    put16(map, typeList, std::uint32_t(res.size() - 1));
    for (std::size_t i = 0; i < res.size(); ++i) {
        const std::size_t e = typeList + 2 + 8 * i, q = refs + 12 * i;
        std::copy(res[i].type.begin(), res[i].type.end(), map.begin() + std::ptrdiff_t(e));
        put16(map, e + 4, 0);
        put16(map, e + 6, std::uint32_t(q - typeList));
        put16(map, q, std::uint32_t(std::uint16_t(res[i].id)));
        put16(map, q + 2, 0xffff);
        put32(map, q + 4, std::uint32_t(data.size()));
        Bytes len(4);
        put32(len, 0, std::uint32_t(res[i].data.size()));
        data.insert(data.end(), len.begin(), len.end());
        data.insert(data.end(), res[i].data.begin(), res[i].data.end());
    }
    Bytes file(256, 0);
    put32(file, 0, 256);
    put32(file, 4, std::uint32_t(256 + data.size()));
    put32(file, 8, std::uint32_t(data.size()));
    put32(file, 12, std::uint32_t(map.size()));
    std::copy_n(file.begin(), 16, map.begin()); // the map starts with a copy of the header
    file.insert(file.end(), data.begin(), data.end());
    file.insert(file.end(), map.begin(), map.end());
    return file;
}

Bytes bytesOf(std::string_view s)
{
    return {s.begin(), s.end()};
}

// A 24-bit BMP, bottom-up, of `w` x `h` pixels whose blue is x, green y, red 9.
Bytes makeBmp(int w, int h)
{
    const std::size_t stride = (std::size_t(w) * 3 + 3) & ~std::size_t(3);
    Bytes b(54 + stride * std::size_t(h), 0);
    auto le32 = [&](std::size_t o, std::uint32_t v) {
        for (int k = 0; k < 4; ++k)
            b[o + std::size_t(k)] = std::uint8_t(v >> (8 * k));
    };
    b[0] = 'B';
    b[1] = 'M';
    le32(10, 54);
    le32(14, 40);
    le32(18, std::uint32_t(w));
    le32(22, std::uint32_t(h));
    b[26] = 1;
    b[28] = 24;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto* p = b.data() + 54 + stride * std::size_t(h - 1 - y) + std::size_t(x) * 3;
            p[0] = std::uint8_t(x);
            p[1] = std::uint8_t(y);
            p[2] = 9;
        }
    return b;
}

// A UDIF image of a disk of `lead` zero sectors, then `payload` (stored raw).
Bytes makeDmg(const Bytes& payload, std::uint64_t lead)
{
    Bytes raw = payload;
    raw.resize((raw.size() + 511) / 512 * 512, 0);
    const std::uint64_t count = raw.size() / 512, sectors = lead + count;
    Bytes mish(204 + 3 * 40, 0);
    std::copy_n("mish", 4, mish.begin());
    put32(mish, 200, 3);
    auto chunk = [&](int i, std::uint32_t type, std::uint64_t sector, std::uint64_t n, std::uint64_t len) {
        const std::size_t c = 204 + 40 * std::size_t(i);
        put32(mish, c, type);
        put32(mish, c + 12, std::uint32_t(sector));
        put32(mish, c + 20, std::uint32_t(n));
        put32(mish, c + 36, std::uint32_t(len));
    };
    chunk(0, 0, 0, lead, 0);
    chunk(1, 1, lead, count, raw.size());
    chunk(2, 0xffffffff, sectors, 0, 0);
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    for (std::size_t i = 0; i < mish.size(); i += 3) {
        const std::uint32_t v = std::uint32_t(mish[i]) << 16
                                | std::uint32_t(i + 1 < mish.size() ? mish[i + 1] : 0) << 8
                                | (i + 2 < mish.size() ? mish[i + 2] : 0);
        enc += b64[v >> 18];
        enc += b64[(v >> 12) & 63];
        enc += i + 1 < mish.size() ? b64[(v >> 6) & 63] : '=';
        enc += i + 2 < mish.size() ? b64[v & 63] : '=';
    }
    const std::string xml = "<plist><dict><key>resource-fork</key><dict><key>blkx</key><array><dict><key>Data</key>"
                            "<data>" + enc + "</data></dict></array></dict></dict></plist>";
    Bytes dmg = raw;
    const auto xmlOffset = dmg.size();
    dmg.insert(dmg.end(), xml.begin(), xml.end());
    Bytes koly(512, 0);
    std::copy_n("koly", 4, koly.begin());
    put32(koly, 220, std::uint32_t(xmlOffset));
    put32(koly, 228, std::uint32_t(xml.size()));
    put32(koly, 496, std::uint32_t(sectors));
    dmg.insert(dmg.end(), koly.begin(), koly.end());
    return dmg;
}

void writeFile(const std::filesystem::path& p, const Bytes& b)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

std::string fnv1a64(const std::uint8_t* p, std::size_t n)
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    char s[17];
    std::snprintf(s, sizeof s, "%016llx", static_cast<unsigned long long>(h));
    return s;
}

const char* const kPanl = "<#Module\r\n"
                          "  Name:\"Test\"\r\n"
                          "  <#Bitmap\r\n"
                          "    Width:2\r\n"
                          "    Data:\"ff0000:00ff00\"\r\n"
                          "  #>\r\n"
                          "  <#ButtonFlat\r\n"
                          "    Text Func:-3\r\n"
                          "    Image:\"000000:ffffff\"\r\n"
                          "  #>\r\n"
                          "  <#Knob\r\n"
                          "    CodeRef:0\r\n"
                          "  #>\r\n"
                          "#>\r\n";

} // namespace

TEST_CASE("Clavia BMP: bottom-up BGR rows padded to 4 bytes, as top-down RGB")
{
    const auto b = decodeBmp(makeBmp(3, 2));
    REQUIRE(b.width == 3);
    REQUIRE(b.height == 2);
    REQUIRE(b.rgb.size() == 18);
    CHECK(b.rgb[0] == 9);                 // red
    CHECK(b.rgb[1] == 0);                 // green = y
    CHECK(b.rgb[2] == 0);                 // blue = x
    CHECK(b.rgb[(1 * 3 + 2) * 3 + 1] == 1); // (2, 1)
    CHECK(b.rgb[(1 * 3 + 2) * 3 + 2] == 2);
    auto bad = makeBmp(3, 2);
    bad[28] = 8;
    CHECK_THROWS(decodeBmp(bad));
    CHECK_THROWS(decodeBmp(bytesOf("XX")));
}

TEST_CASE("Clavia PANL: blocks, integer and string values, keys with spaces")
{
    const auto p = parsePanl(kPanl);
    CHECK(p.type == "Module");
    REQUIRE(p.field("Name"));
    CHECK(*p.field("Name") == "Test");
    REQUIRE(p.children.size() == 3);
    CHECK(*p.children[0].field("Data") == "ff0000:00ff00");
    CHECK(*p.children[1].field("Text Func") == "-3");
    CHECK(p.children[2].field("Image") == nullptr);
    CHECK_THROWS(parsePanl("<#Module\n  Name:\"x\"\n"));
    CHECK_THROWS(parsePanl("<#Module\n  what\n#>\n"));
}

TEST_CASE("Clavia download: the editor's resource file from the .dmg, the app or the .rsrc")
{
    const auto editor = makeResources({{"PANL", 360, "", bytesOf(kPanl)},
                                       {"CBMP", 199, "", makeBmp(4, 2)},
                                       {"JPEG", 115, "", {0xff, 0xd8, 1, 2}},
                                       {"SMTB", 0xD01, "", makeBmp(2, 2)}});
    const auto updater = makeResources({{"NMG2", 128, "", {1, 2, 3}}});
    // both apps' resource files on the disk, the updater's first
    Bytes disk = updater;
    disk.resize((disk.size() + 511) / 512 * 512, 0);
    disk.insert(disk.end(), editor.begin(), editor.end());
    const auto dmg = makeDmg(disk, 8);
    REQUIRE(isUdifImage(dmg));
    const auto onDisk = udifDisk(dmg);
    const auto files = resourceFilesOnDisk(onDisk); // spans into onDisk
    REQUIRE(files.size() == 2);
    CHECK(Bytes(files[1].begin(), files[1].end()) == editor);

    const auto dir = std::filesystem::temp_directory_path() / "g2-test-clavia";
    std::filesystem::remove_all(dir);
    const auto app = dir / "Nord Modular G2 Editor.app";
    writeFile(app / "Contents" / "Resources" / "Nord Modular G2 Editor.rsrc", editor);
    writeFile(dir / "download.dmg", dmg);
    writeFile(dir / "updater.rsrc", updater);
    CHECK(editorResources(dir / "download.dmg") == editor);
    CHECK(editorResources(app) == editor);
    CHECK(editorResources(dir) == editor); // the volume
    CHECK(editorResources(app / "Contents" / "Resources" / "Nord Modular G2 Editor.rsrc") == editor);
    CHECK_THROWS(editorResources(dir / "updater.rsrc"));
    CHECK_THROWS(editorResources(dir / "missing.dmg"));
    writeFile(dir / "updater.dmg", makeDmg(updater, 2));
    CHECK_THROWS(editorResources(dir / "updater.dmg"));
    std::filesystem::remove_all(dir);

    const auto skin = classicSkin(editor);
    CHECK(skin.cbmp.at(199).width == 4);
    CHECK(skin.smtb.at(0xD01).height == 2);
    CHECK(skin.jpeg.at(115) == Bytes{0xff, 0xd8, 1, 2});
    REQUIRE(skin.inlineImages.at(360).size() == 3);
    CHECK(skin.inlineImages.at(360)[0] == "ff0000:00ff00");
    CHECK(skin.inlineImages.at(360)[1] == "000000:ffffff");
    CHECK(skin.inlineImages.at(360)[2].empty());
}

// The user's own download, when G2_CLAVIA_DMG names it (the .dmg, the editor app or its .rsrc): the run-time
// extraction gives, pixel for pixel, the images the reference decoders give (tests/clavia_skin_fingerprints.txt).
TEST_CASE("Clavia download: the Classic look's images from the user's file", "[clavia-download]")
{
    const char* path = std::getenv("G2_CLAVIA_DMG");
    if (!path || !std::filesystem::exists(path))
        SKIP("set G2_CLAVIA_DMG to your copy of \"Nord Modular G2 OS v1.62 Update.dmg\"");
    const auto skin = classicSkin(editorResources(path));

    std::set<std::string> expected;
    std::ifstream f(std::string(G2_SOURCE_DIR) + "/tests/clavia_skin_fingerprints.txt");
    for (std::string line; std::getline(f, line);)
        if (!line.empty() && line[0] != '#')
            expected.insert(line);
    REQUIRE(expected.size() == 405);

    std::set<std::string> got;
    auto add = [&](const std::string& what, const std::uint8_t* p, std::size_t n) { got.insert(what + " " + fnv1a64(p, n)); };
    for (const auto& [kind, bitmaps] : {std::pair{"cbmp", &skin.cbmp}, std::pair{"smtb", &skin.smtb}})
        for (const auto& [id, b] : *bitmaps)
            add(std::string(kind) + " " + std::to_string(id) + " " + std::to_string(b.width) + " "
                    + std::to_string(b.height),
                b.rgb.data(), b.rgb.size());
    for (const auto& [id, bytes] : skin.jpeg)
        add("jpeg " + std::to_string(id) + " " + std::to_string(bytes.size()), bytes.data(), bytes.size());
    for (const auto& [panel, images] : skin.inlineImages)
        for (std::size_t i = 0; i < images.size(); ++i)
            if (!images[i].empty())
                add("inline " + std::to_string(panel) + " " + std::to_string(i),
                    reinterpret_cast<const std::uint8_t*>(images[i].data()), images[i].size());

    for (const auto& line : expected) {
        INFO(line);
        CHECK(got.count(line) == 1);
    }
    CHECK(got.size() == expected.size());
}
