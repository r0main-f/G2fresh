#include "g2emu/firmware.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace g2emu {

namespace {

std::uint32_t be16(std::span<const std::uint8_t> b, std::size_t o)
{
    if(o + 2 > b.size()) throw std::runtime_error("firmware: read past the end");
    return std::uint32_t(b[o]) << 8 | b[o + 1];
}

std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t o)
{
    if(o + 4 > b.size()) throw std::runtime_error("firmware: read past the end");
    return std::uint32_t(b[o]) << 24 | std::uint32_t(b[o + 1]) << 16 | std::uint32_t(b[o + 2]) << 8 | b[o + 3];
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if(!f) throw std::runtime_error("firmware: cannot read " + p.string());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

} // namespace

std::uint32_t onesComplementSum(std::span<const std::uint8_t> data)
{
    std::uint32_t s = 0;
    for(auto b : data) s += b;
    return ~s;
}

std::vector<std::uint8_t> lzo1xDecompress(std::span<const std::uint8_t> src)
{
    std::vector<std::uint8_t> out;
    out.reserve(src.size() * 3);
    std::size_t ip = 0;

    auto in = [&](std::size_t i) -> std::uint32_t {
        if(i >= src.size()) throw std::runtime_error("LZO: input overrun");
        return src[i];
    };
    auto literals = [&](std::size_t n) {
        if(ip + n > src.size()) throw std::runtime_error("LZO: input overrun");
        out.insert(out.end(), src.begin() + std::ptrdiff_t(ip), src.begin() + std::ptrdiff_t(ip + n));
        ip += n;
    };
    auto runLength = [&](std::uint32_t base) {
        std::uint32_t n = 0;
        while(in(ip) == 0) { n += 255; ++ip; }
        n += base + in(ip);
        ++ip;
        return n;
    };
    auto copyMatch = [&](std::size_t dist, std::size_t len) {
        if(dist > out.size()) throw std::runtime_error("LZO: match before the start of the output");
        const std::size_t p = out.size() - dist;
        for(std::size_t k = 0; k < len; ++k) out.push_back(out[p + k]);  // may overlap: byte by byte
    };

    unsigned state = 0;  // literals copied after the last match: 0..3, or 4 after a literal run
    std::uint32_t t = in(ip);
    if(t > 17)
    {
        ++ip;
        t -= 17;
        literals(t);
        state = t >= 4 ? 4 : t;
    }
    for(;;)
    {
        t = in(ip++);
        if(t < 16)
        {
            if(state == 0)
            {
                if(t == 0) t = runLength(15);
                literals(t + 3);
                state = 4;
                continue;
            }
            if(state < 4)
                copyMatch(1 + (t >> 2) + (in(ip) << 2), 2);
            else
                copyMatch(1 + 0x800 + (t >> 2) + (in(ip) << 2), 3);
            ++ip;
            state = src[ip - 2] & 3;
        }
        else if(t >= 64)
        {
            copyMatch(1 + ((t >> 2) & 7) + (in(ip) << 3), (t >> 5) + 1);
            ++ip;
            state = src[ip - 2] & 3;
        }
        else if(t >= 32)
        {
            std::uint32_t len = t & 31;
            if(len == 0) len = runLength(31);
            const std::uint32_t d = in(ip) | in(ip + 1) << 8;
            ip += 2;
            copyMatch(1 + (d >> 2), len + 2);
            state = d & 3;
        }
        else  // 16..31: a far match, or the end of the stream
        {
            std::uint32_t len = t & 7;
            if(len == 0) len = runLength(7);
            const std::uint32_t d = in(ip) | in(ip + 1) << 8;
            ip += 2;
            const std::uint32_t dist = ((t & 8) << 11) + (d >> 2);
            if(dist == 0)
            {
                if(ip != src.size()) throw std::runtime_error("LZO: data after the end marker");
                return out;
            }
            copyMatch(dist + 0x4000, len + 2);
            state = d & 3;
        }
        if(state) literals(state);
    }
}

std::vector<Resource> parseResources(std::span<const std::uint8_t> blob)
{
    const auto dataOff = be32(blob, 0), mapOff = be32(blob, 4), mapLen = be32(blob, 12);
    if(std::size_t(mapOff) + mapLen > blob.size()) throw std::runtime_error("resources: map past the end");
    const auto m = blob.subspan(mapOff, mapLen);
    const auto typeList = be16(m, 24), nameList = be16(m, 26);
    const auto types = be16(m, typeList) + 1;
    std::vector<Resource> out;
    for(std::uint32_t i = 0; i < types; ++i)
    {
        const std::size_t p = typeList + 2 + 8 * i;
        if(p + 8 > m.size()) throw std::runtime_error("resources: type list past the end");
        const std::string type(reinterpret_cast<const char*>(&m[p]), 4);
        const auto count = be16(m, p + 4) + 1, refOff = be16(m, p + 6);
        for(std::uint32_t j = 0; j < count; ++j)
        {
            const std::size_t q = typeList + refOff + 12 * j;
            Resource r;
            r.type = type;
            r.id = std::int16_t(be16(m, q));
            const auto nameOff = std::int16_t(be16(m, q + 2));
            const auto d = dataOff + (be32(m, q + 4) & 0xffffff);
            const auto size = be32(blob, d);
            if(std::size_t(d) + 4 + size > blob.size()) throw std::runtime_error("resources: data past the end");
            r.data.assign(blob.begin() + d + 4, blob.begin() + d + 4 + size);
            if(nameOff != -1)
            {
                const std::size_t n = nameList + std::size_t(nameOff);
                if(n >= m.size() || n + 1 + m[n] > m.size()) throw std::runtime_error("resources: name past the end");
                r.name.assign(reinterpret_cast<const char*>(&m[n + 1]), m[n]);  // Mac Roman, ASCII here
            }
            out.push_back(std::move(r));
        }
    }
    return out;
}

const OsSection* Firmware::section(const std::string& name) const
{
    for(const auto& s : sections)
        if(s.name == name) return &s;
    return nullptr;
}

const OsSection* Firmware::code() const { return section("CODE"); }

Firmware Firmware::fromOsImage(std::span<const std::uint8_t> os, std::span<const std::uint8_t> boot)
{
    // Layout (big-endian), see tools/firmware/g2os.py: u16 version, u8 format (1), u16 checksum of the header at 6,
    // the section count at 0x13 and 0x2C-byte section entries from 0x14.
    if(os.size() < 0x2d4) throw std::runtime_error("firmware: OS image too short");
    if(os[2] != 1) throw std::runtime_error("firmware: OS header format " + std::to_string(os[2]) + ", expected 1");
    if((onesComplementSum(os.subspan(8, 0x2cc)) & 0xffff) != be16(os, 6))
        throw std::runtime_error("firmware: OS header checksum mismatch");
    Firmware fw;
    fw.version = int(be16(os, 0));
    const unsigned n = os[0x13];
    for(unsigned i = 0; i < n; ++i)
    {
        const std::size_t e = 0x14 + 0x2c * i;
        OsSection s;
        s.name.assign(reinterpret_cast<const char*>(&os[e]), 4);
        const auto off = be32(os, e + 4), usize = be32(os, e + 8);
        s.address = be32(os, e + 12);
        const auto usum = be32(os, e + 16), csize = be32(os, e + 20), csum = be32(os, e + 24);
        const auto stored = csize ? csize : usize;
        if(std::size_t(off) + stored > os.size()) throw std::runtime_error("firmware: section " + s.name + " past the end");
        const auto raw = os.subspan(off, stored);
        if(onesComplementSum(raw) != (csize ? csum : usum))
            throw std::runtime_error("firmware: section " + s.name + ": stored checksum mismatch");
        if(csize)
            s.data = lzo1xDecompress(raw);
        else
            s.data.assign(raw.begin(), raw.end());
        if(s.data.size() != usize || onesComplementSum(s.data) != usum)
            throw std::runtime_error("firmware: section " + s.name + ": unpacked size or checksum mismatch");
        fw.sections.push_back(std::move(s));
    }
    if(!fw.code()) throw std::runtime_error("firmware: no CODE section");
    fw.bootLoader.assign(boot.begin(), boot.end());
    return fw;
}

Firmware Firmware::fromUpdaterResources(std::span<const std::uint8_t> rsrc)
{
    const Resource* os = nullptr;
    const Resource* boot = nullptr;
    const auto res = parseResources(rsrc);
    for(const auto& r : res)
    {
        if(r.type == "NMG2" && r.id == 128) os = &r;
        if(r.type == "BOOT" && r.id == 128) boot = &r;
    }
    if(!os) throw std::runtime_error("firmware: no NMG2 128 (\"OS\") resource: not the G2 updater's resource file");
    return fromOsImage(os->data, boot ? std::span<const std::uint8_t>(boot->data) : std::span<const std::uint8_t>());
}

Firmware Firmware::load(const std::filesystem::path& path)
{
    namespace fs = std::filesystem;
    if(fs::is_directory(path))
    {
        const fs::path rsrcInApp = path / "Contents" / "Resources" / "Nord Modular G2 Updater.rsrc";
        if(fs::exists(rsrcInApp)) return fromUpdaterResources(readFile(rsrcInApp));
        for(const auto& [os, boot] : {std::pair{path / "NMG2_OS.bin", path / "BOOT_Loader.bin"},
                                      std::pair{path / "NMG2" / "128_OS.bin", path / "BOOT" / "128_Loader.bin"},
                                      std::pair{path / "mac-updater-rsrc" / "NMG2" / "128_OS.bin",
                                                path / "mac-updater-rsrc" / "BOOT" / "128_Loader.bin"}})
        {
            if(!fs::exists(os)) continue;
            const auto osBytes = readFile(os);
            const auto bootBytes = fs::exists(boot) ? readFile(boot) : std::vector<std::uint8_t>();
            return fromOsImage(osBytes, bootBytes);
        }
        throw std::runtime_error("firmware: no G2 OS image found in " + path.string());
    }
    const auto bytes = readFile(path);
    if(bytes.size() >= 3 && bytes[2] == 1 && bytes.size() > 0x2d4 && bytes[0] == 0)
    {
        try { return fromOsImage(bytes); } catch(const std::exception&) {}
    }
    return fromUpdaterResources(bytes);
}

} // namespace g2emu
