// Reading Clavia's updater straight from the user's "Nord Modular G2 OS v1.62 Update.dmg": an Apple UDIF disk image
// (the "koly" trailer at the end, an XML property list of "blkx" tables, one per partition, whose chunks are stored,
// zero-filled or zlib-compressed). The disk is put back together in memory; the updater's resource file is then the
// resource map in it that holds NMG2 128 and BOOT 128 (Firmware::fromUpdaterResources checks the OS image's own
// checksums, so a false match cannot pass). That assumes the file is stored in one piece on the HFS volume, as it is
// in Clavia's image; a fragmented file would need the volume's catalog.
#include "g2emu/firmware.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#if G2EMU_HAVE_ZLIB
#include <zlib.h>
#endif

namespace g2emu {

namespace {

std::uint32_t be32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }
std::uint64_t be64(const std::uint8_t* p) { return std::uint64_t(be32(p)) << 32 | be32(p + 4); }

std::vector<std::uint8_t> base64(const std::string& s)
{
    std::vector<std::uint8_t> out;
    std::uint32_t acc = 0;
    int bits = 0;
    for(char c : s)
    {
        int v;
        if(c >= 'A' && c <= 'Z') v = c - 'A';
        else if(c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if(c >= '0' && c <= '9') v = c - '0' + 52;
        else if(c == '+') v = 62;
        else if(c == '/') v = 63;
        else continue;  // whitespace, padding
        acc = acc << 6 | std::uint32_t(v);
        bits += 6;
        if(bits >= 8)
        {
            bits -= 8;
            out.push_back(std::uint8_t(acc >> bits));
        }
    }
    return out;
}

} // namespace

bool isUdifImage(std::span<const std::uint8_t> file)
{
    return file.size() >= 512 && std::memcmp(file.data() + file.size() - 512, "koly", 4) == 0;
}

std::vector<std::uint8_t> udifDisk(std::span<const std::uint8_t> file)
{
    if(!isUdifImage(file)) throw std::runtime_error("dmg: not a UDIF disk image (no koly trailer)");
    const std::uint8_t* koly = file.data() + file.size() - 512;
    const auto dataForkOffset = be64(koly + 24);
    const auto xmlOffset = be64(koly + 216), xmlLength = be64(koly + 224);
    const auto sectors = be64(koly + 492);
    if(xmlOffset + xmlLength > file.size() || sectors > (std::uint64_t(1) << 22))  // 2 GB at most
        throw std::runtime_error("dmg: bad trailer");
    const std::string xml(reinterpret_cast<const char*>(file.data() + xmlOffset), std::size_t(xmlLength));

    std::vector<std::uint8_t> disk(std::size_t(sectors) * 512, 0);
    const auto blkx = xml.find("<key>blkx</key>");
    if(blkx == std::string::npos) throw std::runtime_error("dmg: no blkx tables");
    for(std::size_t p = xml.find("<data>", blkx); p != std::string::npos; p = xml.find("<data>", p))
    {
        const auto e = xml.find("</data>", p);
        if(e == std::string::npos) break;
        const auto mish = base64(xml.substr(p + 6, e - p - 6));
        p = e;
        if(mish.size() < 204 || std::memcmp(mish.data(), "mish", 4) != 0) continue;
        const auto firstSector = be64(mish.data() + 8);
        const auto dataOffset = be64(mish.data() + 24);
        const auto chunks = be32(mish.data() + 200);
        if(204 + std::uint64_t(chunks) * 40 > mish.size()) throw std::runtime_error("dmg: bad blkx table");
        for(std::uint32_t c = 0; c < chunks; ++c)
        {
            const std::uint8_t* k = mish.data() + 204 + 40 * c;
            const auto type = be32(k);
            const auto sector = firstSector + be64(k + 8), count = be64(k + 16);
            const auto offset = dataForkOffset + dataOffset + be64(k + 24), length = be64(k + 32);
            if(type == 0xffffffff) break;  // end of the table
            if(type == 0 || type == 2 || type == 0x7ffffffe) continue;  // zeros, ignored, comment
            if((sector + count) * 512 > disk.size() || offset + length > file.size())
                throw std::runtime_error("dmg: chunk outside the image");
            std::uint8_t* out = disk.data() + sector * 512;
            if(type == 1)
                std::memcpy(out, file.data() + offset, std::size_t(std::min(length, count * 512)));
            else if(type == 0x80000005)
            {
#if G2EMU_HAVE_ZLIB
                uLongf n = uLongf(count * 512);
                if(uncompress(out, &n, file.data() + offset, uLong(length)) != Z_OK)
                    throw std::runtime_error("dmg: a zlib chunk does not inflate");
#else
                throw std::runtime_error("dmg: zlib-compressed image, and this build has no zlib");
#endif
            }
            else
                throw std::runtime_error("dmg: chunk type " + std::to_string(type) + " (only stored and zlib images are read; "
                                         "convert it with hdiutil, or open it and choose the updater app)");
        }
    }
    return disk;
}

Firmware firmwareFromDisk(std::span<const std::uint8_t> disk)
{
    // The resource file: a 256-byte header (data offset 256, map offset, data length, map length; the map starts
    // with a copy of it) at the start of an allocation block (512-byte aligned on HFS and HFS+).
    for(std::size_t o = 0; o + 256 <= disk.size(); o += 512)
    {
        const std::uint8_t* h = disk.data() + o;
        if(be32(h) != 256) continue;
        const auto mapOff = be32(h + 4), dataLen = be32(h + 8), mapLen = be32(h + 12);
        if(mapOff != 256 + dataLen || mapLen < 30 || mapLen > (1u << 24) || o + mapOff + mapLen > disk.size()) continue;
        if(std::memcmp(h, h + mapOff, 16) != 0) continue;
        const auto rsrc = disk.subspan(o, mapOff + mapLen);
        try
        {
            bool hasOs = false;
            for(const auto& r : parseResources(rsrc))
                if(r.type == "NMG2" && r.id == 128) hasOs = true;
            if(hasOs) return Firmware::fromUpdaterResources(rsrc);
        }
        catch(const std::exception&)
        {
        }
    }
    throw std::runtime_error("dmg: no G2 updater resources in this disk image");
}

} // namespace g2emu
