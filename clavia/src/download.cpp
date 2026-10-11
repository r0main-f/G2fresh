#include "g2/clavia/download.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace g2::clavia {
namespace {

std::uint32_t be16(std::span<const std::uint8_t> b, std::size_t o)
{
    if (o + 2 > b.size())
        throw std::runtime_error("resources: read past the end");
    return std::uint32_t(b[o]) << 8 | b[o + 1];
}

std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t o)
{
    if (o + 4 > b.size())
        throw std::runtime_error("resources: read past the end");
    return std::uint32_t(b[o]) << 24 | std::uint32_t(b[o + 1]) << 16 | std::uint32_t(b[o + 2]) << 8 | b[o + 3];
}

std::uint32_t be32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}

std::uint64_t be64(const std::uint8_t* p)
{
    return std::uint64_t(be32(p)) << 32 | be32(p + 4);
}

Bytes base64(const std::string& s)
{
    Bytes out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+')
            v = 62;
        else if (c == '/')
            v = 63;
        else
            continue; // whitespace, padding
        acc = acc << 6 | std::uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(std::uint8_t(acc >> bits));
        }
    }
    return out;
}

const char* const kEditorRsrc = "Nord Modular G2 Editor.rsrc";

} // namespace

std::vector<Resource> parseResources(std::span<const std::uint8_t> blob)
{
    const auto dataOff = be32(blob, 0), mapOff = be32(blob, 4), mapLen = be32(blob, 12);
    if (std::size_t(mapOff) + mapLen > blob.size())
        throw std::runtime_error("resources: map past the end");
    const auto m = blob.subspan(mapOff, mapLen);
    const auto typeList = be16(m, 24), nameList = be16(m, 26);
    const auto types = be16(m, typeList) + 1;
    std::vector<Resource> out;
    for (std::uint32_t i = 0; i < types; ++i) {
        const std::size_t p = typeList + 2 + 8 * i;
        if (p + 8 > m.size())
            throw std::runtime_error("resources: type list past the end");
        const std::string type(reinterpret_cast<const char*>(&m[p]), 4);
        const auto count = be16(m, p + 4) + 1, refOff = be16(m, p + 6);
        for (std::uint32_t j = 0; j < count; ++j) {
            const std::size_t q = typeList + refOff + 12 * j;
            Resource r;
            r.type = type;
            r.id = std::int16_t(be16(m, q));
            const auto nameOff = std::int16_t(be16(m, q + 2));
            const std::size_t d = std::size_t(dataOff) + (be32(m, q + 4) & 0xffffff);
            const auto size = be32(blob, d);
            if (d + 4 + size > blob.size())
                throw std::runtime_error("resources: data past the end");
            r.data.assign(blob.begin() + std::ptrdiff_t(d + 4), blob.begin() + std::ptrdiff_t(d + 4 + size));
            if (nameOff != -1) {
                const std::size_t n = nameList + std::size_t(nameOff);
                if (n >= m.size() || n + 1 + m[n] > m.size())
                    throw std::runtime_error("resources: name past the end");
                r.name.assign(reinterpret_cast<const char*>(&m[n + 1]), m[n]); // Mac Roman, ASCII here
            }
            out.push_back(std::move(r));
        }
    }
    return out;
}

// The "koly" trailer at the end, an XML property list of "blkx" tables, one per partition, whose chunks are stored,
// zero-filled or zlib-compressed. The disk is put back together in memory (40 MB for Clavia's image).
bool isUdifImage(std::span<const std::uint8_t> file)
{
    return file.size() >= 512 && std::memcmp(file.data() + file.size() - 512, "koly", 4) == 0;
}

Bytes udifDisk(std::span<const std::uint8_t> file)
{
    if (!isUdifImage(file))
        throw std::runtime_error("dmg: not a UDIF disk image (no koly trailer)");
    const std::uint8_t* koly = file.data() + file.size() - 512;
    const auto dataForkOffset = be64(koly + 24);
    const auto xmlOffset = be64(koly + 216), xmlLength = be64(koly + 224);
    const auto sectors = be64(koly + 492);
    if (xmlOffset + xmlLength > file.size() || sectors > (std::uint64_t(1) << 22)) // 2 GB at most
        throw std::runtime_error("dmg: bad trailer");
    const std::string xml(reinterpret_cast<const char*>(file.data() + xmlOffset), std::size_t(xmlLength));

    Bytes disk(std::size_t(sectors) * 512, 0);
    const auto blkx = xml.find("<key>blkx</key>");
    if (blkx == std::string::npos)
        throw std::runtime_error("dmg: no blkx tables");
    for (std::size_t p = xml.find("<data>", blkx); p != std::string::npos; p = xml.find("<data>", p)) {
        const auto e = xml.find("</data>", p);
        if (e == std::string::npos)
            break;
        const auto mish = base64(xml.substr(p + 6, e - p - 6));
        p = e;
        if (mish.size() < 204 || std::memcmp(mish.data(), "mish", 4) != 0)
            continue;
        const auto firstSector = be64(mish.data() + 8);
        const auto dataOffset = be64(mish.data() + 24);
        const auto chunks = be32(mish.data() + 200);
        if (204 + std::uint64_t(chunks) * 40 > mish.size())
            throw std::runtime_error("dmg: bad blkx table");
        for (std::uint32_t c = 0; c < chunks; ++c) {
            const std::uint8_t* k = mish.data() + 204 + 40 * c;
            const auto type = be32(k);
            const auto sector = firstSector + be64(k + 8), count = be64(k + 16);
            const auto offset = dataForkOffset + dataOffset + be64(k + 24), length = be64(k + 32);
            if (type == 0xffffffff)
                break; // end of the table
            if (type == 0 || type == 2 || type == 0x7ffffffe)
                continue; // zeros, ignored, comment
            if ((sector + count) * 512 > disk.size() || offset + length > file.size())
                throw std::runtime_error("dmg: chunk outside the image");
            std::uint8_t* out = disk.data() + sector * 512;
            if (type == 1) {
                std::memcpy(out, file.data() + offset, std::size_t(std::min(length, count * 512)));
            } else if (type == 0x80000005) {
                uLongf n = uLongf(count * 512);
                if (uncompress(out, &n, file.data() + offset, uLong(length)) != Z_OK)
                    throw std::runtime_error("dmg: a zlib chunk does not inflate");
            } else {
                throw std::runtime_error("dmg: chunk type " + std::to_string(type)
                                         + " (only stored and zlib images are read; convert it with hdiutil, or open "
                                           "it and choose the app in it)");
            }
        }
    }
    return disk;
}

std::vector<std::span<const std::uint8_t>> resourceFilesOnDisk(std::span<const std::uint8_t> disk)
{
    // A resource file starts with a 16-byte header (data offset 256, map offset, data length, map length) that the
    // map starts with a copy of.
    std::vector<std::span<const std::uint8_t>> out;
    for (std::size_t o = 0; o + 256 <= disk.size(); o += 512) {
        const std::uint8_t* h = disk.data() + o;
        if (be32(h) != 256)
            continue;
        const auto mapOff = be32(h + 4), dataLen = be32(h + 8), mapLen = be32(h + 12);
        if (mapOff != 256 + std::uint64_t(dataLen) || mapLen < 30 || mapLen > (1u << 24)
            || o + mapOff + mapLen > disk.size())
            continue;
        if (std::memcmp(h, h + mapOff, 16) != 0)
            continue;
        out.push_back(disk.subspan(o, std::size_t(mapOff) + mapLen));
    }
    return out;
}

Bytes readFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool isEditorResources(const std::vector<Resource>& resources)
{
    bool panl = false, cbmp = false;
    for (const auto& r : resources) {
        panl = panl || r.type == "PANL";
        cbmp = cbmp || r.type == "CBMP";
    }
    return panl && cbmp;
}

Bytes editorResources(const std::filesystem::path& path)
{
    namespace fs = std::filesystem;
    const auto name = path.filename().string();
    if (fs::is_directory(path)) {
        // the editor app, or the volume (the mounted .dmg) that holds it
        for (const auto& p : {path / "Contents" / "Resources" / kEditorRsrc,
                              path / "Nord Modular G2 Editor.app" / "Contents" / "Resources" / kEditorRsrc})
            if (fs::exists(p))
                return editorResources(p);
        throw std::runtime_error(name + " is not the G2 editor app (no Contents/Resources/" + kEditorRsrc + " in it)");
    }
    const auto bytes = readFile(path);
    if (isUdifImage(bytes)) {
        const auto disk = udifDisk(bytes);
        for (const auto rsrc : resourceFilesOnDisk(disk)) {
            try {
                if (isEditorResources(parseResources(rsrc)))
                    return Bytes(rsrc.begin(), rsrc.end());
            } catch (const std::exception&) {
            }
        }
        throw std::runtime_error(name + " holds no G2 editor (Clavia's \"Nord Modular G2 OS v1.62 Update.dmg\" does)");
    }
    std::vector<Resource> res;
    try {
        res = parseResources(bytes);
    } catch (const std::exception&) {
    }
    if (!isEditorResources(res))
        throw std::runtime_error(name + " is neither Clavia's \"Nord Modular G2 OS v1.62 Update.dmg\" nor the G2 "
                                        "editor app or its " + kEditorRsrc);
    return bytes;
}

} // namespace g2::clavia
