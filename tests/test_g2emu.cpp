// The real-time G2 emulator (emu/g2emu, re/notes/g2-hardware-and-emulation.md §3.9). Everything here runs without
// the user's firmware, except the [firmware] case at the end, which boots the user's own OS when it is there
// (G2_FIRMWARE, or original/firmware) and is skipped otherwise.
#include "dsp.hpp"
#include "hw.hpp"
#include "panel.hpp"
#include "thread.hpp"

#include "coldfire/cfCpu.h"

#include "dsp56kEmu/assembler.h"

#include "g2/edit.hpp"
#include "g2/module_db.hpp"
#include "g2/patch.hpp"
#include "g2/proto/client.hpp"
#include "g2/proto/frame.hpp"
#include "EmulatedMachine.h"
#include "SynthSettingsForm.h"
#include "g2/uprate.hpp"
#include "g2emu/firmware.hpp"
#include "g2emu/machine.hpp"
#include "g2emu/runner.hpp"
#include "g2emu/speedtest.hpp"
#include "g2emu/transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace g2emu;
using Bytes = std::vector<std::uint8_t>;

namespace {

void put16(Bytes& b, std::size_t o, std::uint32_t v) { b[o] = std::uint8_t(v >> 8); b[o + 1] = std::uint8_t(v); }
void put32(Bytes& b, std::size_t o, std::uint32_t v)
{
    for(int k = 0; k < 4; ++k) b[o + std::size_t(k)] = std::uint8_t(v >> (24 - 8 * k));
}

// LZO1X streams written by hand: an initial literal run (first byte 17 + n), then
//   "abc" + a match of 6 at distance 3 (t >= 64: length (t >> 5) + 1, distance 1 + ((t >> 2) & 7) + 8 * next)
//   and the end marker 11 00 00.
const Bytes kLzoAbc = {20, 'a', 'b', 'c', 0xA8, 0x00, 0x11, 0x00, 0x00};

// An OS image as the updater carries it (layout in tools/firmware/g2os.py): a stored SRAM section and an
// LZO-packed CODE section.
Bytes makeOsImage(const Bytes& sram, const Bytes& codePacked, const Bytes& codeUnpacked)
{
    Bytes os(0x2d4, 0);
    put16(os, 0, 162);
    os[2] = 1;
    put16(os, 4, 0x0140);
    os[0x13] = 2;
    std::size_t off = os.size();
    auto section = [&](int i, const char* name, std::uint32_t addr, const Bytes& stored, const Bytes& unpacked, bool packed) {
        const std::size_t e = 0x14 + 0x2c * std::size_t(i);
        std::copy(name, name + 4, os.begin() + std::ptrdiff_t(e));
        put32(os, e + 4, std::uint32_t(off));
        put32(os, e + 8, std::uint32_t(unpacked.size()));
        put32(os, e + 12, addr);
        put32(os, e + 16, onesComplementSum(unpacked));
        put32(os, e + 20, packed ? std::uint32_t(stored.size()) : 0);
        put32(os, e + 24, packed ? onesComplementSum(stored) : 0);
        os.insert(os.end(), stored.begin(), stored.end());
        off = os.size();
    };
    section(0, "SRAM", 0x20000800, sram, sram, false);
    section(1, "CODE", 0x30000400, codePacked, codeUnpacked, true);
    put16(os, 6, onesComplementSum(std::span<const std::uint8_t>(os).subspan(8, 0x2cc)) & 0xffff);
    return os;
}

// A classic resource map holding the given resources (type, id, name, data).
Bytes makeResources(const std::vector<Resource>& res)
{
    Bytes data;
    std::vector<std::uint32_t> offsets;
    for(const auto& r : res)
    {
        offsets.push_back(std::uint32_t(data.size()));
        Bytes len(4);
        put32(len, 0, std::uint32_t(r.data.size()));
        data.insert(data.end(), len.begin(), len.end());
        data.insert(data.end(), r.data.begin(), r.data.end());
    }
    // map: 16-byte header copy, handle, file ref, attributes, type list offset, name list offset, then the type list
    // (count - 1, entries: type, count - 1, reference list offset), the reference lists and the names
    std::vector<std::string> types;
    for(const auto& r : res)
        if(std::find(types.begin(), types.end(), r.type) == types.end()) types.push_back(r.type);
    const std::size_t typeList = 28;
    const std::size_t refs = typeList + 2 + 8 * types.size();
    Bytes map(refs + 12 * res.size(), 0);
    Bytes names;
    put16(map, 24, std::uint32_t(typeList));
    put16(map, typeList, std::uint32_t(types.size() - 1));
    std::size_t refIndex = 0;
    for(std::size_t t = 0; t < types.size(); ++t)
    {
        const std::size_t e = typeList + 2 + 8 * t;
        std::copy(types[t].begin(), types[t].end(), map.begin() + std::ptrdiff_t(e));
        std::uint32_t count = 0;
        const std::size_t first = refIndex;
        for(std::size_t i = 0; i < res.size(); ++i)
        {
            if(res[i].type != types[t]) continue;
            const std::size_t q = refs + 12 * refIndex++;
            put16(map, q, std::uint32_t(std::uint16_t(res[i].id)));
            if(res[i].name.empty())
                put16(map, q + 2, 0xffff);
            else
            {
                put16(map, q + 2, std::uint32_t(names.size()));
                names.push_back(std::uint8_t(res[i].name.size()));
                names.insert(names.end(), res[i].name.begin(), res[i].name.end());
            }
            put32(map, q + 4, offsets[i]);
            ++count;
        }
        put16(map, e + 4, count - 1);
        put16(map, e + 6, std::uint32_t(refs + 12 * first - typeList));
    }
    put16(map, 26, std::uint32_t(map.size()));
    map.insert(map.end(), names.begin(), names.end());
    Bytes file(256, 0);
    put32(file, 0, 256);
    put32(file, 4, std::uint32_t(256 + data.size()));
    put32(file, 8, std::uint32_t(data.size()));
    put32(file, 12, std::uint32_t(map.size()));
    std::copy_n(file.begin(), 16, map.begin());  // the map starts with a copy of the header
    file.insert(file.end(), data.begin(), data.end());
    file.insert(file.end(), map.begin(), map.end());
    return file;
}

} // namespace

TEST_CASE("LZO1X: literals, an overlapping match and the end marker", "[g2emu]")
{
    const auto out = lzo1xDecompress(kLzoAbc);
    CHECK(std::string(out.begin(), out.end()) == "abcabcabc");
    // a far match (16..31) of length 4 needs 0x4000 bytes of history: before the start
    const Bytes bad = {20, 'a', 'b', 'c', 0x12, 0x04, 0x00, 0x11, 0x00, 0x00};
    CHECK_THROWS(lzo1xDecompress(bad));
    const Bytes truncated(kLzoAbc.begin(), kLzoAbc.end() - 2);
    CHECK_THROWS(lzo1xDecompress(truncated));
    const Bytes trailing = {20, 'a', 'b', 'c', 0x11, 0x00, 0x00, 0x00};
    CHECK_THROWS(lzo1xDecompress(trailing));
}

TEST_CASE("OS image: sections, checksums, the updater's resources", "[g2emu]")
{
    const Bytes sram = {1, 2, 3, 4, 5};
    const Bytes code = {'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c'};
    const auto os = makeOsImage(sram, kLzoAbc, code);
    const auto fw = Firmware::fromOsImage(os, Bytes{0x30, 0x40});
    CHECK(fw.version == 162);
    REQUIRE(fw.sections.size() == 2);
    CHECK(fw.section("SRAM")->data == sram);
    CHECK(fw.section("SRAM")->address == 0x20000800);
    REQUIRE(fw.code());
    CHECK(fw.code()->data == code);
    CHECK(fw.code()->address == 0x30000400);
    CHECK(fw.bootLoader == Bytes{0x30, 0x40});

    auto broken = os;
    broken[0x2d4 + 1] ^= 1;  // the stored SRAM section
    CHECK_THROWS(Firmware::fromOsImage(broken));
    broken = os;
    broken[0x10] ^= 1;  // inside the checksummed header
    CHECK_THROWS(Firmware::fromOsImage(broken));

    const auto rsrc = makeResources({{"vers", 1, "", {9, 9}}, {"NMG2", 128, "OS", os}, {"BOOT", 128, "Loader", {7, 7, 7}}});
    const auto res = parseResources(rsrc);
    REQUIRE(res.size() == 3);
    CHECK(res[1].type == "NMG2");
    CHECK(res[1].name == "OS");
    const auto fw2 = Firmware::fromUpdaterResources(rsrc);
    CHECK(fw2.code()->data == code);
    CHECK(fw2.bootLoader == Bytes{7, 7, 7});
    CHECK_THROWS(Firmware::fromUpdaterResources(makeResources({{"vers", 1, "", {9}}})));

    // a directory as g2os.py writes it
    const auto dir = std::filesystem::temp_directory_path() / "g2emu-test-fw";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "NMG2_OS.bin", std::ios::binary).write(reinterpret_cast<const char*>(os.data()), std::streamsize(os.size()));
    const auto fw3 = Firmware::load(dir);
    CHECK(fw3.code()->data == code);
    std::filesystem::remove_all(dir);
}

TEST_CASE("UDIF disk image (.dmg): stored and zero chunks, the updater's resource file found on the disk", "[g2emu]")
{
    const Bytes code = {'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c'};
    const auto os = makeOsImage({1, 2, 3}, kLzoAbc, code);
    auto rsrc = makeResources({{"NMG2", 128, "OS", os}, {"BOOT", 128, "Loader", {7, 7}}});
    rsrc.resize((rsrc.size() + 511) / 512 * 512, 0);
    // a disk of 16 zero sectors, then the resource file from sector 16; the image stores it raw
    const std::uint64_t first = 16, count = rsrc.size() / 512, sectors = first + count;
    Bytes dmg = rsrc;  // the data fork: the raw chunk's bytes
    Bytes mish(204 + 3 * 40, 0);
    std::copy_n("mish", 4, mish.begin());
    put32(mish, 4, 1);
    put32(mish, 8, 0); put32(mish, 12, 0);                               // first sector
    put32(mish, 16, 0); put32(mish, 20, std::uint32_t(sectors));         // sector count
    put32(mish, 200, 3);                                                 // chunks
    auto chunk = [&](int i, std::uint32_t type, std::uint64_t sector, std::uint64_t n, std::uint64_t off, std::uint64_t len) {
        const std::size_t c = 204 + 40 * std::size_t(i);
        put32(mish, c, type);
        put32(mish, c + 12, std::uint32_t(sector));
        put32(mish, c + 20, std::uint32_t(n));
        put32(mish, c + 28, std::uint32_t(off));
        put32(mish, c + 36, std::uint32_t(len));
    };
    chunk(0, 0, 0, first, 0, 0);
    chunk(1, 1, first, count, 0, rsrc.size());
    chunk(2, 0xffffffff, sectors, 0, 0, 0);
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    for(std::size_t i = 0; i < mish.size(); i += 3)
    {
        const std::uint32_t v = std::uint32_t(mish[i]) << 16 | std::uint32_t(i + 1 < mish.size() ? mish[i + 1] : 0) << 8 |
                                (i + 2 < mish.size() ? mish[i + 2] : 0);
        enc += b64[v >> 18]; enc += b64[(v >> 12) & 63];
        enc += i + 1 < mish.size() ? b64[(v >> 6) & 63] : '=';
        enc += i + 2 < mish.size() ? b64[v & 63] : '=';
    }
    const std::string xml = "<plist><dict><key>resource-fork</key><dict><key>blkx</key><array><dict><key>Data</key><data>" +
                            enc + "</data></dict></array></dict></dict></plist>";
    const auto xmlOffset = dmg.size();
    dmg.insert(dmg.end(), xml.begin(), xml.end());
    Bytes koly(512, 0);
    std::copy_n("koly", 4, koly.begin());
    put32(koly, 220, std::uint32_t(xmlOffset));
    put32(koly, 228, std::uint32_t(xml.size()));
    put32(koly, 496, std::uint32_t(sectors));
    dmg.insert(dmg.end(), koly.begin(), koly.end());

    REQUIRE(isUdifImage(dmg));
    const auto disk = udifDisk(dmg);
    REQUIRE(disk.size() == sectors * 512);
    CHECK(std::equal(rsrc.begin(), rsrc.end(), disk.begin() + 16 * 512));
    const auto fw = firmwareFromDisk(disk);
    CHECK(fw.code()->data == code);
    CHECK(fw.bootLoader == Bytes{7, 7});
    CHECK_FALSE(isUdifImage(rsrc));
    CHECK_THROWS(firmwareFromDisk(Bytes(4096, 0)));
}

TEST_CASE("SIM: timer 1 as the OS programs it ticks every 51 x 128 bus clocks", "[g2emu]")
{
    std::uint64_t now = 0;
    Sim sim([&] { return now; });
    // [C] 0x300582F0 called with timer 1, level 1, prescaler $7F, TRR 50: TMR = $7F3B (bus clock / 128, ORI, FRR)
    sim.write(0x180, 2, 1);
    sim.write(0x180, 2, 0);
    sim.write(0x4E, 1, 0x80 | 1 << 2);  // ICR TIMER1: autovector, level 1
    sim.write(0x44, 4, 0xfffffbfe);     // IMR: timer 1 unmasked
    sim.write(0x18C, 2, 0);
    sim.write(0x191, 1, 3);
    sim.write(0x184, 2, 50);
    sim.write(0x180, 2, 0x7F3B);
    CHECK(sim.nextEvent() == 50 * 128);
    now = 50 * 128 - 1;
    sim.advance(now);
    CHECK(sim.pendingLevel() == 0);
    now = 50 * 128;
    sim.advance(now);
    CHECK(sim.pendingLevel() == 1);
    CHECK(sim.acknowledge(1) == 25);
    CHECK(sim.read(0x191, 1) == 2);   // TER: REF
    sim.write(0x191, 1, 2);           // write 1 to clear
    CHECK(sim.pendingLevel() == 0);
    CHECK(sim.nextEvent() == 50 * 128 + 51 * 128);
    now += 51 * 128;
    sim.advance(now);
    CHECK(sim.pendingLevel() == 1);
    // 54 MHz / 6528 = 8272 ticks per second
    CHECK(double(Sim::BusHz) / (51 * 128) > 8270);
}

TEST_CASE("SIM: MIDI bytes reach UART0 at 31250 baud and interrupt at level 4 with UIVR", "[g2emu]")
{
    std::uint64_t now = 0;
    Sim sim([&] { return now; });
    // [C] 0x300583E2 called for UART0 with vector $42, level 4, UBG 54
    sim.write(0x1C0 + 0x18, 1, 0);
    sim.write(0x1C0 + 0x1C, 1, 54);
    sim.write(0x1C0 + 0x30, 1, 0x42);
    sim.write(0x50, 1, 4 << 2);          // ICR UART0: level 4, no autovector
    sim.write(0x44, 4, 0xffffeffe);      // IMR: UART0 unmasked
    sim.write(0x1C0 + 0x14, 1, 2);       // UIMR: RxRDY
    const std::uint8_t note[3] = {0x90, 69, 100};
    sim.midiIn(note, 3);
    const std::uint64_t perByte = 10 * 32 * 54;  // bus clocks
    CHECK(sim.nextEvent() == perByte);
    now = perByte - 1;
    sim.advance(now);
    CHECK((sim.read(0x1C4, 1) & 1) == 0);
    now = perByte;
    sim.advance(now);
    CHECK((sim.read(0x1C4, 1) & 1) == 1);   // USR RxRDY
    CHECK(sim.pendingLevel() == 4);
    CHECK(sim.acknowledge(4) == 0x42);
    CHECK(sim.read(0x1CC, 1) == 0x90);
    CHECK(sim.pendingLevel() == 0);
    now = 3 * perByte;
    sim.advance(now);
    CHECK(sim.read(0x1CC, 1) == 69);
    CHECK(sim.read(0x1CC, 1) == 100);
    CHECK(sim.midiOverruns() == 0);

    // a timed message: its first byte starts on the line at the given bus clock, not before
    const std::uint8_t off[3] = {0x80, 69, 64};
    sim.midiInAt(off, 3, now + 100000);
    CHECK(sim.nextEvent() == now + 100000 + perByte);
    now += 100000 + perByte - 1;
    sim.advance(now);
    CHECK((sim.read(0x1C4, 1) & 1) == 0);
    now += 1;
    sim.advance(now);
    CHECK(sim.read(0x1CC, 1) == 0x80);
    sim.write(0x1CC, 1, 0xF8);
    CHECK(sim.takeMidiOut() == Bytes{0xF8});
}

TEST_CASE("Flash: CFI says AMD command set, programs and erases with unlock bypass", "[g2emu]")
{
    Flash f;
    f.write(0x55 * 2, 2, 0x98);
    CHECK(f.read(0x10 * 2, 2) == 'Q');
    CHECK(f.read(0x13 * 2, 2) == 2);
    CHECK(f.read(0x27 * 2, 2) == 23);
    f.write(0, 2, 0xF0);
    CHECK(f.read(0x1234, 2) == 0xffff);
    // program a word: AA 55 A0, data
    f.write(0x555 * 2, 2, 0xAA);
    f.write(0x2AA * 2, 2, 0x55);
    f.write(0x555 * 2, 2, 0xA0);
    f.write(0x1234, 2, 0x1234);
    CHECK(f.read(0x1234, 2) == 0x1234);
    // unlock bypass: AA 55 20, then A0 + data; sector erase 80 30; exit 90 00 [C] 0x30003DA0
    f.write(0x555 * 2, 2, 0xAA);
    f.write(0x2AA * 2, 2, 0x55);
    f.write(0x555 * 2, 2, 0x20);
    f.write(0, 2, 0xA0);
    f.write(0x20000, 2, 0x5A5A);
    CHECK(f.read(0x20000, 2) == 0x5A5A);
    f.write(0, 2, 0x80);
    f.write(0x10000, 2, 0x30);
    CHECK(f.read(0x1234, 2) == 0x1234);       // another sector
    f.write(0, 2, 0x80);
    f.write(0x20000, 2, 0x30);
    CHECK(f.read(0x20000, 2) == 0xffff);
    f.write(0, 2, 0x90);
    f.write(0, 2, 0x00);
    CHECK(f.programs() == 2);
}

TEST_CASE("EEPROM: the OS's bit-banged I2C on the parallel port writes a page and reads it back", "[g2emu]")
{
    // As the OS drives it [C 0x30053208, 0x3005338E, 0x30053528]: SCL on PADAT bit 14, SDA on bit 8, open drain
    // (PADDR bit 8 set: low; clear: released, read back)
    Sim sim([] { return std::uint64_t(0); });
    std::uint16_t paddr = 0, padat = 0;
    auto put = [&] {
        sim.write(0x244, 2, paddr);
        sim.write(0x248, 2, padat);
    };
    auto scl = [&](bool high) {
        padat = std::uint16_t(high ? padat | 0x4000 : padat & ~0x4000);
        put();
    };
    auto sda = [&](bool high) {
        if(high) paddr = std::uint16_t(paddr & ~0x100);
        else
        {
            padat = std::uint16_t(padat & ~0x100);
            paddr = std::uint16_t(paddr | 0x100);
        }
        put();
    };
    auto line = [&] { return (sim.read(0x248, 2) & 0x100) != 0; };
    auto start = [&] { sda(true); scl(true); sda(false); scl(false); };
    auto stop = [&] { sda(false); scl(true); sda(true); };
    auto out = [&](std::uint8_t b) {
        for(int i = 7; i >= 0; --i) { sda((b >> i) & 1); scl(true); scl(false); }
        sda(true);
        scl(true);
        const bool ack = !line();
        scl(false);
        return ack;
    };
    auto in = [&](bool ack) {
        sda(true);
        std::uint8_t b = 0;
        for(int i = 0; i < 8; ++i) { scl(true); b = std::uint8_t(b << 1 | (line() ? 1 : 0)); scl(false); }
        sda(!ack);
        scl(true);
        scl(false);
        sda(true);
        return b;
    };
    auto read = [&](std::uint16_t addr, int n) {
        std::string s;
        start();
        CHECK(out(std::uint8_t(0xA0 | (addr >> 7 & 0x0E))));
        CHECK(out(std::uint8_t(addr)));
        start();
        CHECK(out(std::uint8_t(0xA1 | (addr >> 7 & 0x0E))));
        for(int i = 0; i < n; ++i) s += char(in(i + 1 < n));
        stop();
        return s;
    };
    scl(false);
    CHECK(read(0x123, 3) == "\xFF\xFF\xFF");  // a new chip
    start();
    CHECK(out(0xA2));  // block 1
    CHECK(out(0x23));
    for(char c : std::string("ABC")) CHECK(out(std::uint8_t(c)));
    stop();
    CHECK(read(0x123, 3) == "ABC");
    CHECK(read(0x122, 5) == "\xFF" "ABC\xFF");  // sequential reads go on
    const auto image = sim.eeprom().snapshot();
    CHECK(std::string(image.begin() + 0x123, image.begin() + 0x126) == "ABC");
    CHECK(sim.eeprom().changes() == 3);
    start();
    CHECK_FALSE(out(0x90));  // another device: no acknowledge
    stop();
}

TEST_CASE("ISP1181: OUT packets raise their endpoint's interrupt, IN buffers reach the host", "[g2emu]")
{
    Isp1181 usb;
    auto cmd = [&](std::uint8_t c) { usb.write(0x10, 1, c); };
    auto data = [&](std::uint8_t d) { usb.write(0, 1, d); };
    cmd(0xC2);
    for(std::uint8_t b : {0x07, 0x1F, 0x00, 0x00}) data(b);  // interrupt enable 0x1F07, as the OS sets it [C]
    CHECK_FALSE(usb.irq());
    const std::uint8_t packet[3] = {1, 2, 3};
    usb.out(4, packet, 3);
    CHECK(usb.irq());
    cmd(0xC0);
    const auto intreg = usb.read(0, 4);  // little-endian: bit 12 = endpoint index 4
    CHECK(intreg == 0x00100000u);
    cmd(0x14);  // read buffer 4: LE16 length, data
    CHECK(usb.read(0, 2) == 0x0300);
    CHECK(usb.read(0, 1) == 1);
    CHECK(usb.read(0, 2) == 0x0203);
    cmd(0x54);  // status: clears the interrupt bit
    CHECK(usb.read(0, 1) == 0x20);
    CHECK_FALSE(usb.irq());
    cmd(0x74);  // clear the buffer
    // an interrupt-IN packet (index 2) before a host listens stays; after attach() it is delivered
    cmd(0x02);
    for(std::uint8_t b : {0x02, 0x00, 0xAB, 0xCD}) data(b);
    cmd(0x62);
    CHECK(usb.inPackets().empty());
    usb.attach();
    REQUIRE(usb.inPackets().size() == 1);
    CHECK(usb.inPackets().front().first == 2);
    CHECK(usb.inPackets().front().second == Bytes{0xAB, 0xCD});
    // bulk-IN (index 3) waits until the host polls
    cmd(0x03);
    for(std::uint8_t b : {0x01, 0x00, 0x77}) data(b);
    cmd(0x63);
    std::vector<std::uint8_t> got;
    REQUIRE(usb.pollBulkIn(got));
    CHECK(got == Bytes{0x77});
    CHECK_FALSE(usb.pollBulkIn(got));
}

TEST_CASE("DSP on the JIT: host boot, a subroutine in the vector area, a host command", "[g2emu]")
{
    // A program loaded through the host port as the G2's OS loads stage 1 [§3.6.3]; like stage 1 it keeps a
    // subroutine in the interrupt vector area (P:$B8), which the JIT only runs with dynamicFastInterrupts (§3.9).
    dsp56k::Assembler as;
    std::vector<dsp56k::TWord> p(0x210, 0);
    auto put = [&](dsp56k::TWord at, const char* text) {
        const auto r = as.assemble(text);
        REQUIRE(r.success());
        for(std::uint32_t k = 0; k < r.wordCount; ++k) p[at + k] = r.word[k];
        return at + r.wordCount;
    };
    put(0x00, "jmp $200");
    put(0x80, "movep x:<<$ffffc6,a");    // host command $40: echo HORX to HOTX (a fast interrupt)
    put(0x81, "movep a,x:<<$ffffc7");
    auto a = put(0xB8, "move x0,x:$10");
    put(a, "rts");
    a = put(0x200, "movep #>$4,x:<<$ffffc2");  // HCR: host command interrupts on
    a = put(a, "andi #$fc,mr");
    a = put(a, "move #>$abcdef,x0");
    a = put(a, "jsr $b8");
    const auto wait = a;
    char jmp[32];
    std::snprintf(jmp, sizeof jmp, "jmp $%x", unsigned(wait));
    put(a, jmp);

    Dsp dsp(0, Dsp::Options{true, false});
    auto word = [&](dsp56k::TWord w) {
        dsp.hostWrite(5, std::uint8_t(w >> 16));
        dsp.hostWrite(6, std::uint8_t(w >> 8));
        dsp.hostWrite(7, std::uint8_t(w));
    };
    word(dsp56k::TWord(p.size()));
    word(0);
    for(auto w : p) word(w);
    dsp.runTo(Dsp::CyclesPerFrame);
    CHECK_FALSE(dsp.booting());
    CHECK(dsp.memRead(dsp56k::MemArea_X, 0x10) == 0xabcdef);
    CHECK(dsp.pc() == wait);

    word(0x123456);
    dsp.hostWrite(1, 0x80 | 0x40);
    CHECK((dsp.hostRead(1) & 0x80) != 0);  // pending until the DSP takes it
    dsp.runTo(2 * Dsp::CyclesPerFrame);
    CHECK((dsp.hostRead(1) & 0x80) == 0);
    CHECK((dsp.hostRead(2) & 0x01) != 0);  // RXDF
    std::uint32_t echo = 0;
    for(int r = 5; r <= 7; ++r) echo = echo << 8 | dsp.hostRead(r);
    CHECK(echo == 0x123456);
}


// ---- the front panel (re/notes §3.10) ----

namespace {
// The OS's way of writing to LCD n [C 0x3005c046]: RS on the control latch, then RS | E, the data on +4, E falls.
void lcdWrite(Panel& p, int n, bool rs, std::uint8_t v)
{
    const std::uint32_t ctrl = n == 0 ? 6 : 5;
    const auto e = std::uint8_t(n == 0 ? 0x01 : 0x40 >> (2 * (n - 1))), r = std::uint8_t(n == 0 ? 0x02 : 0x80 >> (2 * (n - 1)));
    p.writeCs5(ctrl, rs ? r : 0);
    p.writeCs5(ctrl, std::uint8_t((rs ? r : 0) | e));
    p.writeCs5(4, v);
    p.writeCs5(ctrl, rs ? r : 0);
}
void lcdText(Panel& p, int n, std::uint8_t address, const char* s)
{
    lcdWrite(p, n, false, std::uint8_t(0x80 | address));
    for(; *s; ++s) lcdWrite(p, n, true, std::uint8_t(*s));
}
// One multiplexer read as the OS does it [C 0x3005bd74]: select (active low) with the enable bit, then read +0.
std::uint8_t muxRead(Panel& p, int select)
{
    if(select < 8)
    {
        p.writeCs5(0, 0x80);
        p.writeCs5(7, std::uint8_t(~(1u << select)));
    }
    else
    {
        p.writeCs5(0, 0x00);
        p.writeCs5(7, 0xff);
    }
    return p.readCs5(0);
}
} // namespace

TEST_CASE("Panel: five HD44780 LCDs on the shared data latch", "[g2emu]")
{
    Panel p;
    for(int n = 0; n < 5; ++n)
        for(std::uint8_t c : {0x30, 0x30, 0x30, 0x38, 0x08, 0x01, 0x06, 0x0c}) lcdWrite(p, n, false, c);  // the OS's init
    lcdText(p, 0, 0x00, "Nord Modular G2");
    lcdText(p, 0, 0x40, "Version 1.62");
    lcdText(p, 3, 0x48, "x");
    PanelState s;
    p.snapshot(s);
    CHECK(s.displays[0].on);
    CHECK(s.displays[0].text(0) == "Nord Modular G2 ");
    CHECK(s.displays[0].text(1) == "Version 1.62    ");
    CHECK(s.displays[3].text(1) == "        x       ");
    CHECK(s.displays[1].text(0) == "                ");  // the other LCDs saw only their init
    CHECK(s.displays[3].cursor == 16 + 9);               // the address counter moved on
    CHECK_FALSE(s.displays[3].cursorLine);

    // a user character (CGRAM 0) drawn as a "g" with its descender, shown as code 0 and code 8
    const std::uint8_t g[8] = {0x00, 0x00, 0x0f, 0x11, 0x11, 0x0f, 0x01, 0x0e};
    lcdWrite(p, 2, false, 0x40);
    for(auto r : g) lcdWrite(p, 2, true, r);
    lcdWrite(p, 2, false, 0x80);
    for(int c : {int('S'), int('t'), int('r'), int('i'), int('n'), 0x08}) lcdWrite(p, 2, true, std::uint8_t(c));
    lcdWrite(p, 2, false, 0x0e);  // underline cursor on
    p.snapshot(s);
    CHECK(s.displays[2].text(0) == "String          ");
    std::array<std::uint8_t, 8> dots{};
    REQUIRE(s.displays[2].userGlyph(8, dots));
    CHECK(dots[7] == 0x0e);
    CHECK_FALSE(s.displays[2].userGlyph('A', dots));
    CHECK(s.displays[2].cursorLine);
    CHECK(s.displays[2].cursor == 6);

    lcdWrite(p, 0, false, 0x01);  // clear
    p.snapshot(s);
    CHECK(s.displays[0].text(0) == "                ");
    CHECK(hd44780Unicode(0x7e) == char32_t(0x2192));  // a right arrow
    CHECK(hd44780Unicode('A') == U'A');
}

TEST_CASE("Panel: the LED matrix, 8 columns of 24 rows, as the OS numbers its LEDs", "[g2emu]")
{
    Panel p;
    // column c (strobe bit 7 - c) shows bytes 0x1E - 4c (+2), 0x1D - 4c (+0, bits 0-6) and 0x1C - 4c (+1) of the OS's
    // buffer, active low: light Variation 2 (19: byte 2 bit 3, column 7), ring LED 0 of knob 1 (0: byte 0 bit 0,
    // column 7) and Morph (243: byte 30 bit 3, column 0)
    auto strobe = [&](int column, std::uint8_t l2, std::uint8_t l0, std::uint8_t l1) {
        p.writeCs5(3, 0xff);
        p.writeCs5(2, l2);
        p.writeCs5(0, std::uint8_t(0x80 | l0));
        p.writeCs5(1, l1);
        p.writeCs5(3, std::uint8_t(~(0x80 >> column)));
    };
    for(int c = 0; c < 8; ++c) strobe(c, 0xff, 0x7f, 0xff);
    strobe(7, std::uint8_t(~0x08), 0x7f, std::uint8_t(~0x01));
    strobe(0, std::uint8_t(~0x08), 0x7f, 0xff);
    PanelState s;
    p.snapshot(s);
    CHECK(s.led(PanelLed::Variation2));
    CHECK(s.led(PanelLed::Morph));
    CHECK(s.rings[0][0]);
    CHECK_FALSE(s.led(PanelLed::Variation1));
    int lit = 0;
    for(bool b : s.rawLeds) lit += b;
    CHECK(lit == 3);
    // every named LED is a distinct position of the matrix, none of them a ring LED
    std::array<int, 256> used{};
    for(int l = 0; l < PanelLedCount; ++l)
    {
        const auto n = panelLedNumber(PanelLed(l));
        CHECK(++used[n] == 1);
        CHECK((n & 31) < 15 + 16);
        CHECK((n & 31) >= 16);
        CHECK(std::string(panelName(PanelLed(l))).size() > 0);
    }
}

TEST_CASE("Panel: buttons and encoders through the input multiplexer", "[g2emu]")
{
    Panel p;
    for(int s = 0; s < 6; ++s) CHECK(muxRead(p, s) == 0xff);
    // nothing selected: the dial at rest (bits 6-7 high), the model in bits 4-5
    CHECK((muxRead(p, 8) & 0xf0) == 0xc0);
    p.setModel(3);
    CHECK((muxRead(p, 8) & 0x30) == 0x30);

    // a press and its release, queued faster than the OS scans: each one is seen by one scan
    p.button(int(PanelButton::Variation3), true);
    p.button(int(PanelButton::Variation3), false);
    CHECK(muxRead(p, 3) == std::uint8_t(~0x04));  // Variation 3: byte 3, bit 2
    CHECK(muxRead(p, 3) == 0xff);
    CHECK(muxRead(p, 3) == 0xff);

    // knob 1 is the pair in bits 1-0 of byte 7 (scan position 7); clockwise in the OS's decoding is 0 -> 2 -> 3 -> 1
    // [C table 0x300EC540], from rest (3)
    p.turn(7, 3);
    std::vector<int> seen;
    for(int i = 0; i < 4; ++i) seen.push_back(muxRead(p, 7) & 3);
    CHECK(seen == std::vector<int>{3, 1, 0, 2});
    p.turn(7, -1);  // the OS has seen state 2: the step back goes out at once
    CHECK((muxRead(p, 7) & 3) == 0);
    CHECK((muxRead(p, 7) & 3) == 0);
    CHECK(muxRead(p, 6) == 0xff);  // the other byte did not move

    // the dial (scanned above): one detent is 4 transitions from rest to rest; the next one waits 8 ms
    p.turn(8, 8);
    std::vector<int> dial;
    for(int i = 0; i < 6; ++i)
    {
        const auto v = muxRead(p, 8);
        dial.push_back((v >> 6 & 1) << 1 | (v >> 7));  // the OS's pair value: bit 6 high, bit 7 low [C 0x300EC758]
    }
    CHECK(dial == std::vector<int>{2, 0, 1, 3, 3, 3});
    p.advance(54000 * 9);
    muxRead(p, 8);
    const auto v = muxRead(p, 8);
    CHECK(((v >> 6 & 1) << 1 | (v >> 7)) == 2);
}

TEST_CASE("Panel: the keyboard matrix's two contacts per key", "[g2emu]")
{
    Panel p;
    CHECK((p.gpioInputs() & 0xff) == 0xff);
    CHECK((p.gpioInputs() & 0x3000) == 0x3000);  // no sustain pedal
    p.key(21, true, 1000, 0);  // key 21: columns 5 (contact A) and 4 (contact B), row bit 7 - 5 = 2
    p.advance(0);
    p.writeCs4(std::uint16_t(~(1u << 5)));
    CHECK((p.gpioInputs() & 0xff) == std::uint8_t(~0x04));
    p.writeCs4(std::uint16_t(~(1u << 4)));
    CHECK((p.gpioInputs() & 0xff) == 0xff);  // B closes 1000 bus clocks later
    p.advance(1000);
    CHECK((p.gpioInputs() & 0xff) == std::uint8_t(~0x04));
    p.key(21, false, 1000, 2000);
    p.advance(2000);
    CHECK((p.gpioInputs() & 0xff) == 0xff);  // B opens first
    p.writeCs4(std::uint16_t(~(1u << 5)));
    CHECK((p.gpioInputs() & 0xff) == std::uint8_t(~0x04));
    p.advance(3000);
    CHECK((p.gpioInputs() & 0xff) == 0xff);
    p.sustain(true, true);
    CHECK((p.gpioInputs() & 0x3000) == 0);
}

namespace {

std::filesystem::path firmwarePath()
{
    if(const char* e = std::getenv("G2_FIRMWARE")) return e;
    return std::filesystem::path(G2_SOURCE_DIR) / "original" / "firmware";
}

// Keyboard -> OscA (sine, Coarse 64, keyboard tracking) -> EnvADSR (gate) -> 2-Out: note 69 is 440 Hz.
g2::Patch keyboardSine()
{
    using namespace g2;
    Patch patch = Patch::makeDefault();
    auto add = [&](const char* name, u8 col) {
        for(const auto& m : db::modules())
            if(m.kind == db::ModuleKind::Module && std::string(m.shortName) == name)
                return edit::addModule(patch, Location::Va, m.typeId, col, 0);
        throw std::runtime_error(name);
    };
    auto param = [&](u8 module, const char* name, u8 value) {
        const auto* def = patch.va.find(module)->def();
        for(std::size_t i = 0; i < def->params.size(); ++i)
            if(std::string(def->params[i].name) == name)
                for(u8 v = 0; v < kFileVariations; ++v) edit::setParam(patch, Location::Va, module, u8(i), v, value);
    };
    const auto kbd = add("Keyboard", 0), osc = add("OscA", 1), env = add("EnvADSR", 2), out = add("2-Out", 3);
    param(osc, "Wave", 0);
    param(osc, "Coarse", 64);
    param(env, "Attack", 0);
    param(env, "Sustain", 127);
    edit::connect(patch, Location::Va, {osc, 0, true}, {env, 0, false});
    edit::connect(patch, Location::Va, {kbd, 1, true}, {env, 1, false});
    edit::connect(patch, Location::Va, {env, 1, true}, {out, 0, false});
    edit::connect(patch, Location::Va, {env, 1, true}, {out, 1, false});
    for(u8 v = 0; v < kFileVariations; ++v) edit::setSetting(patch, edit::Setting::Gain, 0, v, 127);
    uprate::update(patch, Location::Va);
    return patch;
}

} // namespace

namespace {
// RAM from address 0, all of it the core's fast window (as the G2's SDRAM); the rest of the bus reads 0
struct TestBus final : coldfire::Bus {
    static constexpr std::uint32_t Size = 0x40000;
    Bytes mem = Bytes(Size, 0);
    std::uint8_t read8(std::uint32_t a) override { return a < Size ? mem[a] : 0; }
    std::uint16_t read16(std::uint32_t a) override { return std::uint16_t(read8(a) << 8 | read8(a + 1)); }
    std::uint32_t read32(std::uint32_t a) override { return std::uint32_t(read16(a)) << 16 | read16(a + 2); }
    void write8(std::uint32_t a, std::uint8_t v) override { if(a < Size) mem[a] = v; }
    void write16(std::uint32_t a, std::uint16_t v) override { write8(a, std::uint8_t(v >> 8)); write8(a + 1, std::uint8_t(v)); }
    void write32(std::uint32_t a, std::uint32_t v) override { write16(a, std::uint16_t(v >> 16)); write16(a + 2, std::uint16_t(v)); }
    std::uint8_t interruptAcknowledge(std::uint8_t level) override { return std::uint8_t(24 + level); }
};
} // namespace

TEST_CASE("ColdFire core: run()'s dispatch and specialised handlers do what step() does", "[g2emu]")
{
    // Two cores on equal memories from equal states, one stepped through the general handlers (step()), the other
    // through run() (its function table and the specialised MOVE, MOVEA, CLR, TST, CMP, LEA, JSR, PEA, ADDQ/SUBQ and
    // Bcc handlers), one instruction at a time: the registers, flags, cycle counts and memories stay equal, through
    // the exceptions that random code takes (illegal opcodes, odd addresses, privilege)
    std::mt19937 rng(12345);
    auto rnd = [&](std::uint32_t n) { return std::uint32_t(rng() % n); };
    auto opcode = [&]() -> std::uint16_t {
        const std::uint32_t ea = rnd(64), size = rnd(3), reg = rnd(8);
        switch(rnd(12))
        {
        case 0: case 1: return std::uint16_t((1 + rnd(3)) << 12 | reg << 9 | rnd(8) << 6 | ea);  // MOVE, MOVEA
        case 2: return std::uint16_t(0x4200 | size << 6 | ea);                                    // CLR
        case 3: return std::uint16_t(0x4a00 | size << 6 | ea);                                    // TST
        case 4: return std::uint16_t(0xb080 | reg << 9 | ea);                                     // CMP.L
        case 5: return std::uint16_t(0x41c0 | reg << 9 | ea);                                     // LEA
        case 6: return std::uint16_t(0x4e80 | ea);                                                // JSR
        case 7: return std::uint16_t(0x4840 | ea);                                                // PEA
        case 8: return std::uint16_t(0x5080 | reg << 9 | rnd(2) << 8 | ea);                      // ADDQ/SUBQ.L
        case 9: return std::uint16_t(0x6000 | rnd(16) << 8 | (rnd(4) == 0 ? (rnd(2) ? 0xff : 0) : rnd(256)));  // Bcc
        case 10: return std::uint16_t(0x4e75);                                                    // RTS
        default: return std::uint16_t(rng());                                                     // anything
        }
    };
    std::uint64_t instructions = 0;
    for(int program = 0; program < 200; ++program)
    {
        TestBus busA, busB;
        // on the heap: a core's instruction tables take about 1.5 MB (Windows gives a thread 1 MB of stack)
        auto coreA = std::make_unique<coldfire::Cpu>(busA), coreB = std::make_unique<coldfire::Cpu>(busB);
        auto &a = *coreA, &b = *coreB;
        for(auto* c : {&a, &b}) c->setFastMemory((c == &a ? busA : busB).mem.data(), 0, TestBus::Size);
        // vectors to handlers inside RAM (some odd: an address error while taking an exception halts the core)
        for(std::uint32_t v = 0; v < 64; ++v) put32(busA.mem, 4 * v, 0x1000 + 0x40 * v + (rnd(16) == 0 ? 1 : 0));
        for(std::uint32_t o = 0x400; o < TestBus::Size; o += 2)
            put16(busA.mem, o, rnd(3) == 0 ? std::uint16_t(rng()) : opcode());
        busB.mem = busA.mem;
        for(std::uint32_t r = 0; r < 8; ++r)
        {
            const std::uint32_t d = rnd(2) ? std::uint32_t(rng()) : rnd(TestBus::Size);
            const std::uint32_t ad = 0x400 + rnd(TestBus::Size - 0x800) + (rnd(8) == 0 ? 1 : 0);
            a.setD(r, d); b.setD(r, d);
            a.setA(r, ad); b.setA(r, ad);
        }
        const std::uint32_t pc = 0x400 + 2 * rnd((TestBus::Size - 0x800) / 2);
        const std::uint16_t sr = std::uint16_t((rnd(2) ? 0x2700 : 0x0000) | rnd(32));
        for(auto* c : {&a, &b}) { c->setPC(pc); c->setSR(sr); c->setA(7, (a.getA(7) & ~3u) | 0x800); }
        const bool never = false;
        for(int i = 0; i < 400 && !a.isHalted() && !a.isStopped(); ++i)
        {
            a.step();
            b.run(b.getCycles() + 1, never);
            INFO("program " << program << ", instruction " << i << " at " << std::hex << a.getInstructionPC());
            REQUIRE(b.getPC() == a.getPC());
            REQUIRE(b.getSR() == a.getSR());
            REQUIRE(b.getCycles() == a.getCycles());
            REQUIRE(b.getInstructionCount() == a.getInstructionCount());
            REQUIRE(b.isHalted() == a.isHalted());
            REQUIRE(b.isStopped() == a.isStopped());
            for(std::uint32_t r = 0; r < 8; ++r)
            {
                REQUIRE(b.getD(r) == a.getD(r));
                REQUIRE(b.getA(r) == a.getA(r));
            }
            if((i & 15) == 15) REQUIRE(busB.mem == busA.mem);
        }
        REQUIRE(busB.mem == busA.mem);
        instructions += a.getInstructionCount();
    }
    CHECK(instructions > 20000);
}

TEST_CASE("Doorbell: no wake-up is lost between threads that wait for each other", "[g2emu]")
{
    // A ping-pong of a counter: each side waits for its turn, takes it and rings. One side dawdles (0.2 ms) every
    // other round, so that the other spins out and sleeps. The bell's timeout (2 ms in the machine, a safety net) is
    // an hour here: a lost ring would leave a waiter asleep, and the rounds would not finish. Finishing is the fact
    // that holds on any machine; counting the 2 ms timeouts did not, as a busy scheduler alone makes many of them
    // (it failed under load).
    for(const int spin : {0, 50})
    {
        Doorbell bell{std::chrono::hours(1)};
        std::atomic<int> turn{0};
        std::atomic<bool> abandon{false};
        constexpr int rounds = 2000;
        std::thread other([&] {
            for(int i = 0; i < rounds && !abandon; ++i)
            {
                bell.wait([&] { return turn.load() == 2 * i + 1 || abandon.load(); }, spin);
                if(i & 1) std::this_thread::sleep_for(std::chrono::microseconds(200));
                turn.store(2 * i + 2);
                bell.ring();
            }
        });
        std::thread self([&] {
            for(int i = 0; i < rounds && !abandon; ++i)
            {
                bell.wait([&] { return turn.load() == 2 * i || abandon.load(); }, spin);
                if(!(i & 1)) std::this_thread::sleep_for(std::chrono::microseconds(200));
                turn.store(2 * i + 1);
                bell.ring();
            }
        });
        // 2000 rounds take about half a second; two minutes is far beyond any machine's slowness, and well short of
        // the hour a lost ring would sleep.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        while(turn.load() < 2 * rounds && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        INFO("spin " << spin << " us: " << turn.load() << " of " << 2 * rounds << " turns taken");
        CHECK(turn.load() == 2 * rounds);
        abandon = true;
        for(int k = 0; k < 4; ++k) bell.ring(); // a stuck waiter, if any, is asleep and counted now: it wakes and leaves
        other.join();
        self.join();
        CHECK(bell.timeouts() == 0);
    }
}

TEST_CASE("Machine::autoThreads: one or two DSP threads, three for eight DSPs on six performance cores", "[g2emu]")
{
    const int n = Machine::autoThreads();
    CHECK(n >= 1);
    CHECK(n <= 2);
    if(std::thread::hardware_concurrency() <= 2) CHECK(n == 1);
    // eight DSPs: a third thread only where there are cores for it
    const int eight = Machine::autoThreads(8);
    CHECK(eight >= n);
    CHECK(eight <= 3);
    if(eight == 3) CHECK(std::thread::hardware_concurrency() >= 6);
}

TEST_CASE("The user's G2 OS: boots, syncs with our client, plays a MIDI note", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    Machine m(fw);
    m.run(Machine::FrameRate * 2);  // boot: DSPs loaded, flash formatted, frame programs running (about 1.6 s)
    MachineTransport transport(m);
    g2::proto::ManualClock clock;
    g2::proto::Client client(transport, clock);
    m.plugUsb();
    auto step = [&] {
        m.run(Machine::FrameRate / 1000);
        clock.set(std::uint64_t(m.seconds() * 1000));
        client.tick();
    };
    while(!client.synced() && m.seconds() < 4) step();
    REQUIRE(client.synced());
    CHECK(client.state().version.firmware == 162);  // OS 1.62
    // the OS sends zero-length packets before its messages, as a real G2 does: the transport read those empty
    // transfers again (taken as the message, they would have lost the version reply)
    CHECK(transport.emptyReads() > 0);

    g2::Performance perf;
    for(auto& p : perf.slots) p = g2::Patch::makeDefault();
    perf.slots[0] = keyboardSine();
    for(int i = 0; i < 4; ++i)
    {
        perf.header.slots[std::size_t(i)].enabled = i == 0;
        perf.header.slots[std::size_t(i)].keyboard = i == 0;
    }
    client.sendPerformance(perf, "Test");
    while(!client.idle() && m.seconds() < 6) step();
    REQUIRE(client.idle());
    for(int i = 0; i < 500; ++i) step();  // the output ramps up after an upload

    const std::uint8_t on[3] = {0x90, 69, 100};
    // timed: the note-on starts on the MIDI line 500 frames after the start of the recording
    m.midiInAt(on, m.frame() + 500);
    std::vector<float> out;
    m.run(Machine::FrameRate / 2, &out);
    // the DACs' frames follow the DSPs' serial clock, not run()'s boundaries: one more or less is possible
    REQUIRE(out.size() / 4 + 2 >= Machine::FrameRate / 2);
    // outputs 1 and 2 are words 0 and 2 of each frame; find the onset and measure the pitch after it
    std::size_t onset = 0;
    while(onset < out.size() / 4 && std::fabs(out[onset * 4]) < 1e-3f) ++onset;
    // not before its three bytes (0.96 ms = 92 frames) are in, within 10 ms after that
    CHECK(onset >= 500 + 92);
    CHECK(onset < 500 + Machine::FrameRate / 100);
    std::size_t crossings = 0;
    double first = -1, last = -1;
    for(std::size_t f = onset + 100; f + 1 < out.size() / 4; ++f)
    {
        const double a = out[f * 4], b = out[(f + 1) * 4];
        if(a < 0 && b >= 0)
        {
            const double tc = double(f) + a / (a - b);
            if(first < 0) first = tc;
            last = tc;
            ++crossings;
        }
    }
    REQUIRE(crossings > 100);
    const double hz = double(crossings - 1) * Machine::FrameRate / (last - first);
    CHECK_THAT(hz, Catch::Matchers::WithinAbs(440.0, 0.1));
    CHECK(m.stats().exceptions == 0);
}

TEST_CASE("The user's G2 OS on the JIT: control-rate code runs once every 4 frames on every DSP", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    // Stage 1's background loop passes once the frame counter X:$43 is past 3, then adds X:$40 to the 48-bit control
    // tick count L:$42 and runs the patch's control-rate code (re/notes/g2-hardware-and-emulation.md §3.8.1 [C]):
    // 0.25 ticks per frame, a 24 kHz control rate at 96 kHz. With the interpreter in a build that has the JIT, the
    // condition codes of the loop's cmp were lost when an interrupt fell before its ble, and it passed every 2 or 3
    // frames (§3.8.2: 0.33 to 0.5). dsp56300 runs one engine per build; ours is the JIT.
    Machine m(fw);
    m.run(Machine::FrameRate * 2);  // booted: the frame programs run (about 1.6 s)
    auto ticks = [&](int n) {
        return (std::uint64_t(m.dspMemory(n, 1, 0x42)) << 24 | m.dspMemory(n, 2, 0x42)) / m.dspMemory(n, 1, 0x40);
    };
    std::uint64_t before[4];
    for(int n = 0; n < 4; ++n)
    {
        REQUIRE(m.dspMemory(n, 1, 0x40) != 0);
        before[n] = ticks(n);
    }
    const auto from = m.frame();
    m.run(Machine::FrameRate);
    const double frames = double(m.frame() - from);
    for(int n = 0; n < 4; ++n)
    {
        INFO("DSP on chip select A" << 3 + n);
        // each DSP keeps its own frames, within a few of the DACs' (Options::skew, the lines' prefill)
        CHECK_THAT(double(ticks(n) - before[n]) / frames, Catch::Matchers::WithinAbs(0.25, 0.001));
    }
    CHECK(m.stats().exceptions == 0);
}

TEST_CASE("The user's G2 OS in real time: the plugin's path (Runner, a link over USB, MIDI)", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    Runner runner(fw);
    auto link = emulatedG2Link(runner.machine());
    // As the audio thread and the editor do: drain the audio, tick the link, 1 ms apart, in real time.
    std::vector<float> heard, block(4 * 96);
    auto step = [&](bool keep) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while(const auto n = runner.read(block.data(), std::min<std::size_t>(96, runner.available())))
            if(keep) heard.insert(heard.end(), block.begin(), block.begin() + std::ptrdiff_t(4 * n));
        link->tick();
    };
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto within = [&](double s) { return std::chrono::duration<double>(Clock::now() - start).count() < s; };
    while(!link->synced() && within(20)) step(false);
    REQUIRE(link->synced());
    link->sendPerformance(g2::Performance::playing(keyboardSine(), "Sine"), "Sine");  // SynthSync::sendPatchAlone
    while(!link->client().idle() && within(30)) step(false);
    REQUIRE(link->client().idle());
    for(int i = 0; i < 1000; ++i) step(false);  // the output ramps up after an upload

    const std::uint8_t on[3] = {0x90, 69, 100};
    runner.midiIn(on);
    while(heard.size() < 4 * Machine::FrameRate / 2 && within(60)) step(true);
    REQUIRE(heard.size() >= 4 * Machine::FrameRate / 2);
    // Out 1 and Out 2 (words 0 and 2): the 440 Hz sine on both
    for(const std::size_t word : {std::size_t(0), std::size_t(2)})
    {
        std::size_t crossings = 0;
        double first = -1, last = -1;
        for(std::size_t f = Machine::FrameRate / 10; f + 1 < heard.size() / 4; ++f)
        {
            const double a = heard[f * 4 + word], b = heard[(f + 1) * 4 + word];
            if(a < 0 && b >= 0)
            {
                const double tc = double(f) + a / (a - b);
                if(first < 0) first = tc;
                last = tc;
                ++crossings;
            }
        }
        REQUIRE(crossings > 100);
        const double hz = double(crossings - 1) * Machine::FrameRate / (last - first);
        CHECK_THAT(hz, Catch::Matchers::WithinAbs(440.0, 0.1));
        // the master volume knob all the way up (Machine::Options::masterVolume): a full sine gives about 0.125
        float peak = 0;
        for(std::size_t f = Machine::FrameRate / 10; f < heard.size() / 4; ++f) peak = std::max(peak, std::fabs(heard[f * 4 + word]));
        CHECK_THAT(peak, Catch::Matchers::WithinAbs(0.125, 0.01));
    }
    INFO("frames the reader missed: " << runner.stats().framesMissing << ", speed " << runner.stats().speed);
    runner.stop();
    CHECK(runner.machine().stats().exceptions == 0);
    CHECK(runner.machine().stats().dspsWild == 0);
}

TEST_CASE("The user's G2 OS drives the emulated front panel", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    Machine m(fw);
    m.run(Machine::FrameRate * 2);  // boot

    // after the boot: the main display shows the (empty) patch, the assignable displays the patch settings
    auto p = m.panel();
    CHECK(p.generation > 0);
    for(const auto& d : p.displays) CHECK(d.on);
    CHECK(p.displays[0].text(0).find("No Cat") != std::string::npos);
    CHECK(p.displays[1].text(0).find("BPM") != std::string::npos);  // Master Clock, above knob 1
    CHECK(p.led(PanelLed::PatchSettings));
    CHECK(p.led(PanelLed::Octave0));

    struct Events : g2::proto::Client::Listener {
        int variation = -1;
        std::vector<g2::proto::ParamChange> params;
        void variationChanged(int slot, std::uint8_t v) override { if(slot == 0) variation = v; }
        void paramChanged(int slot, const g2::proto::ParamChange& c) override { if(slot == 0) params.push_back(c); }
    } events;
    MachineTransport transport(m);
    g2::proto::ManualClock clock;
    g2::proto::Client client(transport, clock);
    client.setListener(&events);
    m.plugUsb();
    auto step = [&] {
        m.run(Machine::FrameRate / 1000);
        clock.set(std::uint64_t(m.seconds() * 1000));
        client.tick();
    };
    auto run = [&](double s) {
        const double end = m.seconds() + s;
        while(m.seconds() < end) step();
    };
    auto press = [&](PanelButton b) {
        m.panelButton(b, true);
        run(0.03);
        m.panelButton(b, false);
        run(0.05);
    };
    while(!client.synced() && m.seconds() < 4) step();
    REQUIRE(client.synced());

    // a patch with knob 1 of page A1 on the oscillator's Coarse
    auto patch = keyboardSine();
    const auto* osc = [&]() -> const g2::Module* {
        for(const auto& mod : patch.va.modules)
            if(std::string(mod.def()->shortName) == "OscA") return &mod;
        return nullptr;
    }();
    REQUIRE(osc);
    std::uint8_t coarse = 0;
    for(std::size_t i = 0; i < osc->def()->params.size(); ++i)
        if(std::string(osc->def()->params[i].name) == "Coarse") coarse = std::uint8_t(i);
    const auto oscIndex = osc->index;
    g2::edit::assignKnob(patch, 0, g2::Location::Va, oscIndex, coarse);
    client.sendPerformance(g2::Performance::playing(patch, "Panel Test"), "Panel Test");  // slot A on the keyboard
    while(!client.idle() && m.seconds() < 6) step();
    REQUIRE(client.idle());
    run(0.2);
    INFO(m.panel().displays[0].text(0) << " / " << m.panel().displays[0].text(1));
    CHECK(m.panel().displays[0].text(1) == "Panel Test      ");

    // The OS's MIDI Local is Off on an erased flash (variations and keys would only go out as MIDI): System, down to
    // "MIDI Local", one detent of the dial, System again
    press(PanelButton::System);
    press(PanelButton::NavDown);
    CHECK(m.panel().displays[0].text(0).rfind("MIDI Local", 0) == 0);
    m.panelEncoder(PanelEncoder::Dial, 1);
    run(0.05);
    CHECK(m.panel().displays[0].text(1).rfind("On", 0) == 0);
    press(PanelButton::System);

    // a variation button: the client hears of it, the LEDs follow
    press(PanelButton::Variation3);
    run(0.05);
    CHECK(events.variation == 2);
    p = m.panel();
    CHECK(p.led(PanelLed::Variation3));
    CHECK_FALSE(p.led(PanelLed::Variation1));

    // page A1 on the assignable displays, then knob 1 five steps clockwise: Coarse 64 -> 69 in variation 3
    press(PanelButton::PageA);
    p = m.panel();
    CHECK(p.led(PanelLed::PageA));
    CHECK(p.led(PanelLed::Page1));
    CHECK_FALSE(p.led(PanelLed::PatchSettings));
    events.params.clear();
    m.panelEncoder(PanelEncoder::Knob1, 5);
    run(0.1);
    REQUIRE_FALSE(events.params.empty());
    CHECK(events.params.back().module == oscIndex);
    CHECK(events.params.back().param == coarse);
    CHECK(events.params.back().value == 69);
    CHECK(events.params.back().variation == 2);
    m.panelEncoder(PanelEncoder::Knob1, -5);
    run(0.1);
    CHECK(events.params.back().value == 64);

    // the keyboard: key 21 of the G2's 37 is A (MIDI 69 with no octave shift): 440 Hz
    m.panelKey(21, true, 100);
    run(0.1);
    std::vector<float> out;
    m.run(Machine::FrameRate / 4, &out);
    m.panelKey(21, false);
    run(0.1);
    std::size_t crossings = 0;
    double first = -1, last = -1;
    for(std::size_t f = 0; f + 1 < out.size() / 4; ++f)
    {
        const double a = out[f * 4], b = out[(f + 1) * 4];
        if(a < 0 && b >= 0)
        {
            const double tc = double(f) + a / (a - b);
            if(first < 0) first = tc;
            last = tc;
            ++crossings;
        }
    }
    REQUIRE(crossings > 50);
    CHECK_THAT(double(crossings - 1) * Machine::FrameRate / (last - first), Catch::Matchers::WithinAbs(440.0, 0.1));

    // the analogue controls reach the OS's copy of the ADC inputs (OS 1.62: bytes at 0x302A0DB0, index = stream
    // position - 1): mod wheel (position 5), pitch stick (4), control pedal (2), aftertouch (3, inverted)
    auto adc = [&](int index) { return std::uint8_t(m.cfRead32(0x302A0DB0 + std::uint32_t(index & ~3)) >> (8 * (3 - (index & 3)))); };
    CHECK(int(adc(3)) == 0x80);  // the pitch stick at rest
    m.panelAnalog(PanelAnalog::ModWheel, 1.0f);
    m.panelAnalog(PanelAnalog::PitchStick, 0.0f);
    m.panelAnalog(PanelAnalog::ControlPedal, 0.5f);
    m.panelAnalog(PanelAnalog::Aftertouch, 1.0f);
    run(0.05);
    CHECK(int(adc(4)) == 0xff);
    CHECK(int(adc(3)) == 0x00);
    CHECK(std::abs(int(adc(1)) - 0x80) <= 1);
    CHECK(int(adc(2)) == 0x00);
    CHECK(m.stats().exceptions == 0);
}

TEST_CASE("The user's G2 OS: MIDI Local switched off and on over USB (synth settings), then the panel's keys play",
          "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    Machine m(fw);
    m.run(Machine::FrameRate * 2);
    MachineTransport transport(m);
    g2::proto::ManualClock clock;
    g2::proto::Client client(transport, clock);
    m.plugUsb();
    auto step = [&] {
        m.run(Machine::FrameRate / 1000);
        clock.set(std::uint64_t(m.seconds() * 1000));
        client.tick();
    };
    auto run = [&](double s) {
        const double end = m.seconds() + s;
        while(m.seconds() < end) step();
    };
    while(!client.synced() && m.seconds() < 4) step();
    REQUIRE(client.synced());
    CHECK(client.state().settings.localOn);  // a new G2: the OS's own default, Local On

    auto press = [&](PanelButton b) {
        m.panelButton(b, true);
        run(0.03);
        m.panelButton(b, false);
        run(0.05);
    };
    auto output = [&](double seconds) {
        std::vector<float> out;
        m.run(std::uint32_t(Machine::FrameRate * seconds), &out);
        return out;
    };
    auto setLocal = [&](bool on) {
        auto settings = client.state().settings;
        settings.localOn = on;
        client.setSynthSettings(settings);
        while(!client.idle() && m.seconds() < 8) step();
        REQUIRE(client.idle());
        run(0.3);
    };
    // Off: the keys go out as MIDI only, the G2 stays silent
    client.sendPerformance(g2::Performance::playing(keyboardSine(), "Local"), "Local");
    setLocal(false);
    m.panelKey(21, true, 100);
    run(0.1);
    const auto silent = output(0.1);
    m.panelKey(21, false);
    run(0.2);
    CHECK(std::all_of(silent.begin(), silent.end(), [](float s) { return std::fabs(s) < 1e-4f; }));

    // On again, as the OS's own System menu shows it
    setLocal(true);
    press(PanelButton::System);
    press(PanelButton::NavDown);
    INFO(m.panel().displays[0].text(0) << " / " << m.panel().displays[0].text(1));
    CHECK(m.panel().displays[0].text(0).rfind("MIDI Local", 0) == 0);
    CHECK(m.panel().displays[0].text(1).rfind("On", 0) == 0);
    press(PanelButton::System);

    // and the keyboard plays: key 21 of the G2's 37 is A, 440 Hz
    m.panelKey(21, true, 100);
    run(0.1);
    std::vector<float> out;
    m.run(Machine::FrameRate / 4, &out);
    m.panelKey(21, false);
    std::size_t crossings = 0;
    double first = -1, last = -1;
    for(std::size_t f = 0; f + 1 < out.size() / 4; ++f)
    {
        const double a = out[f * 4], b = out[(f + 1) * 4];
        if(a < 0 && b >= 0)
        {
            const double tc = double(f) + a / (a - b);
            if(first < 0) first = tc;
            last = tc;
            ++crossings;
        }
    }
    REQUIRE(crossings > 50);
    CHECK_THAT(double(crossings - 1) * Machine::FrameRate / (last - first), Catch::Matchers::WithinAbs(440.0, 0.2));
}

TEST_CASE("The user's G2 OS on a G2X: the lowest key, the pitch stick's direction, the mod wheel", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    Machine::Options o;
    o.model = PanelModel::G2X;
    Machine m(fw, o);
    m.run(Machine::FrameRate * 2);
    MachineTransport transport(m);
    g2::proto::ManualClock clock;
    g2::proto::Client client(transport, clock);
    m.plugUsb();
    auto step = [&] {
        m.run(Machine::FrameRate / 1000);
        clock.set(std::uint64_t(m.seconds() * 1000));
        client.tick();
    };
    auto run = [&](double s) {
        const double end = m.seconds() + s;
        while(m.seconds() < end) step();
    };
    while(!client.synced() && m.seconds() < 4) step();
    REQUIRE(client.synced());

    // the sine on the keyboard, pitch bend on (2 semitones), vibrato from the wheel at full depth
    auto patch = keyboardSine();
    using g2::edit::Setting;
    for(std::uint8_t v = 0; v < g2::kFileVariations; ++v)
    {
        g2::edit::setSetting(patch, Setting::Bend, 0, v, 1);
        g2::edit::setSetting(patch, Setting::Bend, 1, v, 2);
        g2::edit::setSetting(patch, Setting::Vibrato, 0, v, 2);    // source: Wheel
        g2::edit::setSetting(patch, Setting::Vibrato, 1, v, 127);  // depth: 127 cents
    }
    client.sendPerformance(g2::Performance::playing(patch, "Probe"), "Probe");
    auto settings = client.state().settings;
    settings.localOn = true;
    client.setSynthSettings(settings);
    while(!client.idle() && m.seconds() < 6) step();
    REQUIRE(client.idle());
    run(0.3);

    // zero crossings of output 1 over `seconds`: the mean frequency and the spread of the per-cycle frequency
    auto measure = [&](double seconds, double& mean, double& spread) {
        std::vector<float> out;
        m.run(std::uint32_t(Machine::FrameRate * seconds), &out);
        std::vector<double> t;
        for(std::size_t f = 0; f + 1 < out.size() / 4; ++f)
        {
            const double a = out[f * 4], b = out[(f + 1) * 4];
            if(a < 0 && b >= 0) t.push_back(double(f) + a / (a - b));
        }
        mean = spread = 0;
        if(t.size() < 3) return;
        mean = double(t.size() - 1) * Machine::FrameRate / (t.back() - t.front());
        double lo = 1e9, hi = 0;
        for(std::size_t i = 1; i < t.size(); ++i)
        {
            const double f = Machine::FrameRate / (t[i] - t[i - 1]);
            lo = std::min(lo, f);
            hi = std::max(hi, f);
        }
        spread = hi - lo;
    };
    auto note = [](double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); };
    double hz = 0, spread = 0;

    // the lowest key
    m.panelKey(0, true, 100);
    run(0.1);
    measure(0.5, hz, spread);
    m.panelKey(0, false);
    run(0.2);
    INFO("key 0 plays " << hz << " Hz, MIDI note " << note(hz));
    REQUIRE(hz > 0);
    CHECK(std::lround(note(hz)) == 36);  // C1: the G2X's 61 keys are C1-C6

    // the pitch stick on key 33 (A4, 440 Hz): 1.0 bends up, 0.0 down, by the patch's bend range (setting value 2:
    // 3 semitones, as measured)
    m.panelKey(33, true, 100);
    run(0.1);
    double rest = 0, up = 0, down = 0;
    measure(0.3, rest, spread);
    m.panelAnalog(PanelAnalog::PitchStick, 1.0f);
    run(0.15);
    measure(0.3, up, spread);
    m.panelAnalog(PanelAnalog::PitchStick, 0.0f);
    run(0.15);
    measure(0.3, down, spread);
    m.panelAnalog(PanelAnalog::PitchStick, 0.5f);
    run(0.15);
    INFO("stick at rest " << rest << " Hz, at 1.0 " << up << " Hz (" << note(up) - note(rest) << " semitones), at 0.0 "
                          << down << " Hz (" << note(down) - note(rest) << ")");
    CHECK_THAT(rest, Catch::Matchers::WithinAbs(440.0, 0.5));
    CHECK_THAT(note(up) - note(rest), Catch::Matchers::WithinAbs(3.0, 0.1));
    CHECK_THAT(note(down) - note(rest), Catch::Matchers::WithinAbs(-3.0, 0.1));

    // the mod wheel: no vibrato at 0, vibrato at 1
    double still = 0, wobble = 0;
    m.panelAnalog(PanelAnalog::ModWheel, 0.0f);
    run(0.2);
    measure(0.6, hz, still);
    m.panelAnalog(PanelAnalog::ModWheel, 1.0f);
    run(0.3);
    measure(0.6, hz, wobble);
    m.panelKey(33, false);
    INFO("per-cycle frequency spread: wheel at 0 " << still << " Hz, at 1 " << wobble << " Hz");
    CHECK(still < 1.0);
    CHECK(wobble > 5.0);
}

TEST_CASE("The user's G2 OS: after the client lost contact (the machine ran too slowly), a restart syncs again",
          "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    Machine m(fw);
    m.run(Machine::FrameRate * 2);
    MachineTransport transport(m);
    g2::proto::ManualClock clock;
    g2::proto::Client client(transport, clock);
    m.plugUsb();
    std::uint64_t extraMs = 0;  // wall time the machine did not get
    auto step = [&] {
        m.run(Machine::FrameRate / 1000);
        clock.set(std::uint64_t(m.seconds() * 1000) + extraMs);
        client.tick();
    };
    while(!client.synced() && m.seconds() < 4) step();
    REQUIRE(client.synced());

    // a request, then the machine stalls for 25 s of wall time: the client gives up
    client.sendPerformance(g2::Performance::playing(keyboardSine(), "Stall"), "Stall");
    client.tick();
    for(int i = 0; i < 25 && client.status() == g2::proto::Status::Connected; ++i)
    {
        extraMs += 1000;
        clock.set(std::uint64_t(m.seconds() * 1000) + extraMs);
        client.tick();
    }
    REQUIRE(client.status() == g2::proto::Status::LostContact);

    // what the plugin does then: restart the handshake on the same link, the machine running again
    client.restart();
    const double until = m.seconds() + 6;
    while(!client.synced() && m.seconds() < until) step();
    REQUIRE(client.synced());
    client.sendPerformance(g2::Performance::playing(keyboardSine(), "Again"), "Again");
    while(!client.idle() && m.seconds() < until + 4) step();
    CHECK(client.idle());
    CHECK(client.state().slots[0].name == "Again");
}

TEST_CASE("The user's G2 OS follows an external MIDI clock (its Master Clock display shows the tempo)", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    Machine::Options o;
    o.model = PanelModel::G2X;
    if(const char* e = std::getenv("G2TEST_THREADS")) o.threads = std::atoi(e);
    Machine m(fw, o);
    m.run(Machine::FrameRate * 3);  // boot; the assignable displays show the patch settings, Master Clock first
    const auto before = m.panel().displays[1].text(0) + "|" + m.panel().displays[1].text(1);
    // A new G2's master clock runs at power-on (120 BPM, Run): a MIDI Stop first, so that the clock alone is seen
    // not to start it
    const std::uint8_t start[1] = {0xFA}, stop[1] = {0xFC};
    m.midiInAt(stop, m.frame());
    m.run(Machine::FrameRate / 10);
    // MIDI clock at 100 BPM: a tick every 2400 frames (96 kHz)
    const std::uint8_t tick[1] = {0xF8};
    const int quarters = std::getenv("G2TEST_QUARTERS") ? std::atoi(std::getenv("G2TEST_QUARTERS")) : 8;
    for(int i = 0; i < 24 * quarters; ++i)  // 8 quarter notes (G2TEST_QUARTERS: more)
    {
        m.midiInAt(tick, m.frame());
        m.run(2400);
    }
    const auto after = m.panel().displays[1].text(0) + "|" + m.panel().displays[1].text(1);
    INFO("Master Clock display before: '" << before << "', after 8 quarters of clock at 100 BPM: '" << after << "'");
    CHECK(after.find("Ext 100") != std::string::npos);
    CHECK(after.find("Stop") != std::string::npos);
    // Start, then the clock goes on: the master clock runs; Stop stops it
    m.midiInAt(start, m.frame());
    for(int i = 0; i < 24 * 2; ++i)
    {
        m.midiInAt(tick, m.frame());
        m.run(2400);
    }
    const auto running = m.panel().displays[1].text(1);
    m.midiInAt(stop, m.frame());
    for(int i = 0; i < 24; ++i)
    {
        m.midiInAt(tick, m.frame());
        m.run(2400);
    }
    const auto stopped = m.panel().displays[1].text(1);
    INFO("after Start: '" << running << "', after Stop: '" << stopped << "'");
    CHECK(running.find("Run") != std::string::npos);
    CHECK(stopped.find("Stop") != std::string::npos);
}

TEST_CASE("The user's G2 OS: a new emulated G2 gets a factory G2's system settings, a used one keeps its own",
          "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    // A G2 and our client, as the plugin has them; booted from `flash` (null: erased)
    struct Synth {
        Machine m;
        MachineTransport transport{m};
        g2::proto::ManualClock clock;
        g2::proto::Client client{transport, clock};
        explicit Synth(const Firmware& fw, const std::vector<std::uint8_t>* flash = nullptr) : m(fw)
        {
            if(flash) m.loadMemory(*flash);
            m.run(Machine::FrameRate * 2);
            m.plugUsb();
            while(!client.synced() && m.seconds() < 4) step();
        }
        void step()
        {
            m.run(Machine::FrameRate / 1000);
            clock.set(std::uint64_t(m.seconds() * 1000));
            client.tick();
        }
        void run(double s)
        {
            const double end = m.seconds() + s;
            while(m.seconds() < end) step();
        }
        bool settle()
        {
            const double end = m.seconds() + 4;
            while(!client.idle() && m.seconds() < end) step();
            run(0.3);
            return client.idle();
        }
        void press(PanelButton b)
        {
            m.panelButton(b, true);
            run(0.03);
            m.panelButton(b, false);
            run(0.05);
        }
        // The OS's System menu as it shows it, title -> value (trimmed), down to Synth Name, its last page
        std::map<std::string, std::string> systemMenu()
        {
            auto trim = [](std::string s) {
                s.erase(0, s.find_first_not_of(' '));
                s.erase(s.find_last_not_of(' ') + 1);
                return s;
            };
            std::map<std::string, std::string> pages;
            press(PanelButton::System);
            for(int i = 0; i < 20; ++i)
            {
                const auto d = m.panel().displays[0];
                const auto title = trim(d.text(0).substr(0, d.text(0).find('|')));
                pages[title] = trim(d.text(1));
                if(title == "Synth Name") break;
                press(PanelButton::NavDown);
            }
            press(PanelButton::System);
            return pages;
        }
        void midi(std::initializer_list<std::uint8_t> bytes)
        {
            const std::vector<std::uint8_t> b(bytes);
            m.midiIn(b);
            run(0.1);
        }
        // output 1's level (RMS) over a fifth of a second
        double level()
        {
            std::vector<float> out;
            m.run(Machine::FrameRate / 5, &out);
            double sum = 0;
            const std::size_t n = std::max<std::size_t>(1, out.size() / 4);
            for(std::size_t f = 0; f < out.size() / 4; ++f) sum += double(out[f * 4]) * out[f * 4];
            return std::sqrt(sum / double(n));
        }
        bool stored(std::uint8_t bank, std::uint8_t prog) const
        {
            for(const auto& n : client.state().flash[0])
                if(n.bank == bank && n.prog == prog) return true;
            return false;
        }
    };

    // A new G2 (a new EEPROM, an erased flash): the OS formats the flash and takes its own built-in settings
    CHECK(newMemory({}));
    std::vector<std::uint8_t> saved;
    {
        Synth g2(fw);
        REQUIRE(g2.client.synced());
        const auto own = g2.client.state().settings;
        CHECK(own.name == "ModularG2");
        CHECK(own.localOn);
        CHECK(own.controllerMode == 3);
        CHECK(own.programChangeMode == 3);
        CHECK(own.midiChannels == std::array<std::uint8_t, 4>{0, 1, 2, 3});
        CHECK(own.globalChannel == 15);  // 16
        CHECK_FALSE(own.memoryProtect);
        CHECK(newMemory(g2.m.memorySnapshot()) == false);  // the OS wrote its EEPROM at power-on

        // slot B plays a sine on the keyboard, slots A, C and D are empty; all four active
        g2::Performance perf;
        for(auto& p : perf.slots) p = g2::Patch::makeDefault();
        perf.slots[1] = keyboardSine();
        for(int i = 0; i < 4; ++i)
        {
            perf.header.slots[std::size_t(i)].enabled = true;
            perf.header.slots[std::size_t(i)].keyboard = i == 1;
        }
        g2.client.sendPerformance(perf, "Factory");
        REQUIRE(g2.settle());
        // the OS's Global channel 16 plays as the keyboard does
        g2.midi({0x9F, 69, 100});
        const double global16 = g2.level();
        g2.midi({0x8F, 69, 0});
        g2.run(0.3);
        CHECK(global16 > 0.01);

        // What the plugin sends a new G2, and the OS's System menu then
        g2.client.setSynthSettings(factorySettings(own));
        REQUIRE(g2.settle());
        const auto menu = g2.systemMenu();
        std::string pages;
        for(const auto& [title, value] : menu) pages += title + ": '" + value + "'; ";
        INFO(pages);
        CHECK(menu.at("MIDI Glob Ch") == "Off");
        CHECK(menu.at("Memory Prot") == "On");
        // and the OS's own, as the manual's pictures show them [M p.30-31]
        CHECK(menu.at("MIDI Slot Ch") == "1   2   3   4");
        CHECK(menu.at("MIDI Local") == "On");
        CHECK(menu.at("MIDI Prg Chng") == "Send & Receive");
        CHECK(menu.at("MIDI Ctrl") == "Send & Receive");
        CHECK(menu.at("MIDI Clk Send") == "Off");
        CHECK(menu.at("Sust Ped Pol") == "Open");
        CHECK(menu.at("Ctrl Ped Gain") == "x1.00");
        CHECK(menu.at("Master Tune") == "0 semi   0 cent");
        CHECK(menu.at("Synth Name") == "ModularG2");
        // or not (the pictures: SysEx ID 16, Clk Recv Off)
        CHECK(menu.at("MIDI SysEx Id") == "All");
        CHECK(menu.at("MIDI Clk Recv") == "On");

        // Global Off: channel 16 plays nothing. The slots answer on their own channels: channel 2 plays slot B's
        // sine, channel 1 the empty slot A
        g2.midi({0x9F, 69, 100});
        const double off16 = g2.level();
        g2.midi({0x8F, 69, 0});
        g2.run(0.3);
        g2.midi({0x91, 69, 100});
        const double b = g2.level();
        g2.midi({0x81, 69, 0});
        g2.run(0.3);
        g2.midi({0x90, 69, 100});
        const double a = g2.level();
        g2.midi({0x80, 69, 0});
        g2.run(0.3);
        INFO("channel 16 with Global 16: " << global16 << ", with Global Off: " << off16 << "; channel 2: " << b
                                           << ", channel 1: " << a);
        CHECK(off16 < global16 / 100);
        CHECK(b > 0.01);
        CHECK(a < b / 100);

        // MIDI Ctrl received: CC7 (patch volume) on slot B's channel turns it down
        g2.midi({0x91, 69, 100});
        const double loud = g2.level();
        g2.midi({0xB1, 7, 0});
        const double quiet = g2.level();
        g2.midi({0xB1, 7, 127});
        const double back = g2.level();
        g2.midi({0x81, 69, 0});
        g2.run(0.3);
        INFO("CC7 127 -> 0 -> 127: " << loud << " -> " << quiet << " -> " << back);
        CHECK(quiet < loud / 4);
        CHECK(back > loud / 2);

        // Memory Protect On: the OS refuses to store
        g2.client.storeToFlash(1, 0, 0);
        REQUIRE(g2.settle());
        CHECK_FALSE(g2.stored(0, 0));
        // Switched off (the user's choice), it stores: slot B's sine in 1:1, the empty slot A in 1:2. Slot A loaded
        // from 1:2, a Program Change 1 on channel 1 loads 1:1 into it (a slot never loaded from the memory ignores
        // Program Changes)
        auto open = g2.client.state().settings;
        open.memoryProtect = false;
        g2.client.setSynthSettings(open);
        g2.client.storeToFlash(1, 0, 0);
        g2.client.storeToFlash(0, 0, 1);
        REQUIRE(g2.settle());
        CHECK(g2.stored(0, 0));
        CHECK(g2.stored(0, 1));
        g2.client.loadFromFlash(0, 0, 1);
        REQUIRE(g2.settle());
        g2.midi({0x90, 69, 100});
        const double before = g2.level();
        g2.midi({0x80, 69, 0});
        g2.midi({0xC0, 0});
        g2.run(1.0);
        g2.midi({0x90, 69, 100});
        const double after = g2.level();
        g2.midi({0x80, 69, 0});
        INFO("channel 1 before Program Change 1: " << before << ", after: " << after);
        CHECK(before < 0.001);
        CHECK(after > 0.01);

        // the user's own settings, then the memory as the plugin keeps it
        auto mine = g2.client.state().settings;
        mine.midiChannels = {4, 5, 6, 7};
        mine.localOn = false;
        mine.name = "Mine";
        g2.client.setSynthSettings(mine);
        REQUIRE(g2.settle());
        saved = g2.m.memorySnapshot();
    }

    // A G2 in use: not new, so the plugin sends nothing; the OS keeps the user's settings and patches
    CHECK_FALSE(newMemory(saved));
    Synth again(fw, &saved);
    REQUIRE(again.client.synced());
    const auto s = again.client.state().settings;
    CHECK(s.name == "Mine");
    CHECK(s.midiChannels == std::array<std::uint8_t, 4>{4, 5, 6, 7});
    CHECK_FALSE(s.localOn);
    CHECK_FALSE(s.memoryProtect);
    CHECK(s.globalChannel == 16);
    CHECK(again.stored(0, 0));
    CHECK(again.stored(0, 1));

    // An image of the flash alone (as G2fresh kept it before it had the EEPROM): the OS keeps its patches and takes
    // its own settings, as with a new EEPROM; the plugin treats it as new
    saved.resize(saved.size() - Machine::EepromSize);
    CHECK(newMemory(saved));
    Synth old(fw, &saved);
    REQUIRE(old.client.synced());
    CHECK(old.client.state().settings.name == "ModularG2");
    CHECK(old.client.state().settings.globalChannel == 15);
    CHECK(old.stored(0, 0));
}

TEST_CASE("The user's G2 OS: the Synth Settings window's fields reach the OS and come back, from its System menu too; "
          "a store under Memory Protect is refused with the original's message",
          "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    using g2ui::SettingsEcho;
    using g2ui::SynthSettingsForm;
    struct Heard : g2::proto::Client::Listener {
        std::vector<g2::proto::FlashResult> flashResults;
        int synthChanges = 0;
        void flashResult(const g2::proto::FlashResult& r) override { flashResults.push_back(r); }
        void synthChanged() override { ++synthChanges; }
    } heard;
    Machine m(fw);
    m.run(Machine::FrameRate * 2);
    MachineTransport transport(m);
    g2::proto::ManualClock clock;
    g2::proto::Client client(transport, clock);
    client.setListener(&heard);
    m.plugUsb();
    auto step = [&] {
        m.run(Machine::FrameRate / 1000);
        clock.set(std::uint64_t(m.seconds() * 1000));
        client.tick();
    };
    auto run = [&](double s) {
        const double end = m.seconds() + s;
        while(m.seconds() < end) step();
    };
    auto settle = [&] {
        const double end = m.seconds() + 4;
        while(!client.idle() && m.seconds() < end) step();
        run(0.3);
        return client.idle();
    };
    auto press = [&](PanelButton b) {
        m.panelButton(b, true);
        run(0.03);
        m.panelButton(b, false);
        run(0.05);
    };
    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(' '));
        s.erase(s.find_last_not_of(' ') + 1);
        return s;
    };
    auto title = [&] {
        const auto line = m.panel().displays[0].text(0);
        return trim(line.substr(0, line.find('|')));
    };
    auto value = [&] { return trim(m.panel().displays[0].text(1)); };
    // The OS's System menu, title -> value, down to Synth Name (its last page)
    auto systemMenu = [&] {
        std::map<std::string, std::string> pages;
        press(PanelButton::System);
        for(int i = 0; i < 20; ++i)
        {
            pages[title()] = value();
            if(title() == "Synth Name") break;
            press(PanelButton::NavDown);
        }
        press(PanelButton::System);
        return pages;
    };
    auto stored = [&](std::uint8_t bank, std::uint8_t prog) {
        for(const auto& n : client.state().flash[0])
            if(n.bank == bank && n.prog == prog) return true;
        return false;
    };
    while(!client.synced() && m.seconds() < 4) step();
    REQUIRE(client.synced());

    // The window opens with the synth's settings (a new G2: the OS's own) ...
    auto sent = client.state().settings;
    SettingsEcho echo;
    echo.start(sent);
    SynthSettingsForm form;
    form.take(sent);
    CHECK(form.name == "ModularG2");
    CHECK(form.channel == std::array<int, 4>{1, 2, 3, 4});
    CHECK(form.active == std::array<bool, 4>{true, true, true, true});
    CHECK(form.globalChannel == 16);
    CHECK_FALSE(form.memoryProtect);
    CHECK(SynthSettingsForm::freeMemoryText(client.state().flashUsage) == "100.0%");
    // ... and sends each change as the original's dialog does: the settings last sent with the fields, in one 03
    auto change = [&] {
        sent = form.applyTo(sent);
        echo.sent(sent);
        client.setSynthSettings(sent);
        REQUIRE(settle());
        CHECK_FALSE(echo.changedElsewhere(client.state().settings));  // its own change, not the synth's
    };
    form.name = "Fresh G2";
    form.channel = {5, 6, 7, 8};
    form.active = {true, true, false, true};
    form.globalChannel = SynthSettingsForm::wrap(form.globalChannel + 1);  // Off
    form.sysExId = 3;
    form.sendClock = true;
    form.ignoreExternalClock = true;
    form.controllersReceive = true;
    form.controllersSend = false;
    form.programChangeReceive = false;
    form.programChangeSend = true;
    form.semi = -3;
    form.cent = 12;
    form.octaveShift = true;
    form.octave = 1;
    form.pedalGain = 16;
    form.pedalClosed = true;
    form.memoryProtect = true;
    change();

    // The OS took them: read again from the synth, they are the window's fields ...
    client.resync();
    REQUIRE(settle());
    SynthSettingsForm back;
    back.channel = form.channel;  // an inactive slot's number is the window's own
    back.take(client.state().settings);
    CHECK(back == form);
    CHECK(client.state().settings.name == "Fresh G2");
    CHECK(client.state().settings.midiChannels == std::array<std::uint8_t, 4>{4, 5, 16, 7});
    CHECK(client.state().settings.globalChannel == 16);
    CHECK(client.state().settings.transpose == -3);
    CHECK(client.state().settings.masterTune == 12);
    // ... and its own System menu shows them
    {
        const auto menu = systemMenu();
        std::string pages;
        for(const auto& [t, v] : menu) pages += t + ": '" + v + "'; ";
        INFO(pages);
        CHECK(menu.at("Synth Name") == "Fresh G2");
        CHECK(menu.at("MIDI Glob Ch") == "Off");
        CHECK(menu.at("MIDI SysEx Id") == "3");
        CHECK(menu.at("MIDI Clk Send") == "On");
        CHECK(menu.at("MIDI Clk Recv") == "Off");
        CHECK(menu.at("MIDI Ctrl") == "Receive");
        CHECK(menu.at("MIDI Prg Chng") == "Send");
        CHECK(menu.at("MIDI Local") == "On");
        CHECK(menu.at("Master Tune") == "-3 semi  12 cent");
        CHECK(menu.at("Ctrl Ped Gain") == "x1.25");
        CHECK(menu.at("Sust Ped Pol") == "Closed");
        CHECK(menu.at("Memory Prot") == "On");
    }

    // Memory Protect on: the OS answers a store with result 4, the original's "Memory protect on", and stores nothing
    client.storeToFlash(0, 0, 0);
    REQUIRE(settle());
    REQUIRE(heard.flashResults.size() == 1);
    CHECK(heard.flashResults[0].result == 4);
    CHECK(std::string(g2::proto::flashResultText(heard.flashResults[0].result)) == "Memory protect on");
    CHECK_FALSE(stored(0, 0));

    // Switched off on the G2 itself (System, down to Memory Prot, the dial): the OS tells the editor, and the window
    // takes it as the synth's change
    const int before = heard.synthChanges;
    press(PanelButton::System);
    // (the menu opens on the page it was left on, Synth Name, its last)
    for(int i = 0; i < 20 && title() != "Memory Prot"; ++i) press(PanelButton::NavUp);
    REQUIRE(title() == "Memory Prot");
    m.panelEncoder(PanelEncoder::Dial, -1);
    run(0.3);
    if(value() != "Off")
    {
        m.panelEncoder(PanelEncoder::Dial, 2);
        run(0.3);
    }
    CHECK(value() == "Off");
    press(PanelButton::System);
    run(0.3);
    CHECK(heard.synthChanges > before);
    CHECK_FALSE(client.state().settings.memoryProtect);
    REQUIRE(echo.changedElsewhere(client.state().settings));
    sent = client.state().settings;
    form.take(sent);
    CHECK_FALSE(form.memoryProtect);
    CHECK(form.name == "Fresh G2");  // the rest as it was
    CHECK(form.semi == -3);

    // and the store goes through: result 2 (with the memory's usage and the new name), no message
    client.storeToFlash(0, 0, 0);
    REQUIRE(settle());
    REQUIRE(heard.flashResults.size() == 2);
    CHECK(heard.flashResults[1].result == 2);
    CHECK(std::string(g2::proto::flashResultText(heard.flashResults[1].result)).empty());
    CHECK(stored(0, 0));
    CHECK(client.state().flashUsage.has_value());

    // Cancel: the settings the window opened with go back
    const auto opened = sent;
    form.localOn = false;
    form.cent = -40;
    change();
    form.take(opened);
    change();
    client.resync();
    REQUIRE(settle());
    back.channel = form.channel;
    back.take(client.state().settings);
    CHECK(back == form);
    CHECK(back.localOn);
    CHECK(back.cent == 12);
    CHECK(m.stats().exceptions == 0);
}

// ---------------------------------------------------------------------------------------------------------------
// The audio inputs (re/notes §3.11)

namespace {

constexpr double kTwoPi = 2 * 3.14159265358979323846;

// A patch put together module by module (voice area), by the names of the module database.
struct PatchBuilder {
    g2::Patch patch = g2::Patch::makeDefault();
    g2::u8 add(const char* name, g2::u8 col, g2::u8 row = 0)
    {
        for(const auto& m : g2::db::modules())
            if(m.kind == g2::db::ModuleKind::Module && std::string(m.shortName) == name)
                return g2::edit::addModule(patch, g2::Location::Va, m.typeId, col, row);
        throw std::runtime_error(name);
    }
    g2::u8 paramIndex(g2::u8 module, const char* name) const
    {
        const auto* def = patch.va.find(module)->def();
        for(std::size_t i = 0; i < def->params.size(); ++i)
            if(std::string(def->params[i].name) == name) return g2::u8(i);
        throw std::runtime_error(name);
    }
    void param(g2::u8 module, const char* name, g2::u8 value)
    {
        for(g2::u8 v = 0; v < g2::kFileVariations; ++v)
            g2::edit::setParam(patch, g2::Location::Va, module, paramIndex(module, name), v, value);
    }
    void cable(g2::u8 from, g2::u8 output, g2::u8 to, g2::u8 input)
    {
        g2::edit::connect(patch, g2::Location::Va, {from, output, true}, {to, input, false});
    }
    g2::Patch done()
    {
        for(g2::u8 v = 0; v < g2::kFileVariations; ++v) g2::edit::setSetting(patch, g2::edit::Setting::Gain, 0, v, 127);
        g2::uprate::update(patch, g2::Location::Va);
        return patch;
    }
};

// What the ADCs give: a level and a sine per word (inputs 1, 3, 2, 4; 1 = the ADCs' full scale), the sine by each
// frame's own time, so that the delay to the DACs can be read off the phase.
struct ToneSource final : AdcSource {
    double hz[4] = {0, 0, 0, 0}, amplitude[4] = {0, 0, 0, 0}, level[4] = {0, 0, 0, 0};
    std::uint64_t frames = 0, last = 0, irregular = 0;  // irregular: frames not one frame after the one before
    void adcFrame(std::uint64_t clock, std::int32_t words[4]) override
    {
        if(frames && clock != last + Machine::ClocksPerFrame) ++irregular;
        last = clock;
        ++frames;
        const double t = double(clock) / Machine::ClocksPerFrame / Machine::FrameRate;
        for(int k = 0; k < 4; ++k)
            words[k] = std::int32_t(std::lround(8388607.0 * (level[k] + amplitude[k] * std::sin(kTwoPi * hz[k] * t))));
    }
    void silence()
    {
        for(int k = 0; k < 4; ++k) hz[k] = amplitude[k] = level[k] = 0;
    }
};

// The amplitude and phase (radians, of a sine) of `hz` in word `word` of the frames `out`, the first of which the
// DACs sent at time `clock0` (DSP clocks).
struct Tone {
    double amplitude = 0, phase = 0;
};
Tone tone(const std::vector<float>& out, std::size_t word, double hz, std::uint64_t clock0)
{
    double re = 0, im = 0;
    const std::size_t n = out.size() / 4;
    for(std::size_t i = 0; i < n; ++i)
    {
        const double w = kTwoPi * hz * (double(clock0) / Machine::ClocksPerFrame + double(i)) / Machine::FrameRate;
        re += out[4 * i + word] * std::sin(w);
        im += out[4 * i + word] * std::cos(w);
    }
    return {2 * std::hypot(re, im) / double(n), std::atan2(im, re)};
}

// What is left of word `word` without that sine, relative to it (power): 0 for a clean sine without gaps.
double residue(const std::vector<float>& out, std::size_t word, double hz, std::uint64_t clock0)
{
    const auto t = tone(out, word, hz, clock0);
    double rest = 0, all = 0;
    for(std::size_t i = 0; i < out.size() / 4; ++i)
    {
        const double w = kTwoPi * hz * (double(clock0) / Machine::ClocksPerFrame + double(i)) / Machine::FrameRate;
        const double d = out[4 * i + word] - t.amplitude * std::sin(w + t.phase);
        rest += d * d;
        all += double(out[4 * i + word]) * out[4 * i + word];
    }
    return all > 0 ? rest / all : 1.0;
}

// The power of word `word` between two frequencies (a plain Fourier sum every 50 Hz over the first 9600 frames).
double band(const std::vector<float>& out, std::size_t word, double from, double to)
{
    const std::size_t n = std::min<std::size_t>(out.size() / 4, 9600);
    double power = 0;
    for(double hz = from; hz <= to; hz += 50)
    {
        double re = 0, im = 0;
        for(std::size_t i = 0; i < n; ++i)
        {
            const double w = kTwoPi * hz * double(i) / Machine::FrameRate;
            re += out[4 * i + word] * std::cos(w);
            im += out[4 * i + word] * std::sin(w);
        }
        power += (re * re + im * im) / (double(n) * double(n));
    }
    return power;
}

// A G2 and our client, its ADCs fed by `source`; everything on this thread (deterministic).
struct InputRig {
    ToneSource source;
    Machine m;
    MachineTransport transport{m};
    g2::proto::ManualClock clock;
    g2::proto::Client client{transport, clock};
    explicit InputRig(const Firmware& fw, Machine::Options options = {}) : m(fw, options)
    {
        m.setAdcSource(&source);
        m.run(Machine::FrameRate * 2);
        m.plugUsb();
        while(!client.synced() && m.seconds() < options.usbAfter + 2) step();
    }
    void step()
    {
        m.run(Machine::FrameRate / 1000);
        clock.set(std::uint64_t(m.seconds() * 1000));
        client.tick();
    }
    void run(double s)
    {
        const double end = m.seconds() + s;
        while(m.seconds() < end) step();
    }
    bool upload(const g2::Patch& patch)
    {
        client.sendPerformance(g2::Performance::playing(patch, "In"), "In");
        const double end = m.seconds() + 4;
        while(!client.idle() && m.seconds() < end) step();
        run(0.5);  // the output ramps up after an upload
        return client.idle();
    }
    void param(g2::u8 module, g2::u8 index, g2::u8 value)
    {
        client.setParam(0, g2::Location::Va, module, index, value, 0);
        run(0.1);
    }
    // `seconds` of the DACs' frames; `clock0`: when they sent the first one.
    std::vector<float> record(double seconds, std::uint64_t& clock0)
    {
        std::vector<float> out;
        m.run(std::uint32_t(seconds * Machine::FrameRate), &out);
        clock0 = m.dacClock() - (out.size() / 4 - 1) * Machine::ClocksPerFrame;
        return out;
    }
    // The delay in frames of a tone that comes out inverted with phase `phase` (as tone() gives it).
    static double delayOf(double phase, double hz)
    {
        const double period = Machine::FrameRate / hz;
        double d = (kTwoPi / 2 - phase) / kTwoPi * period;
        while(d < 0) d += period;
        while(d >= period) d -= period;
        return d;
    }
};

} // namespace

TEST_CASE("InputQueue: an input frame reaches the ADCs a fixed time after the DACs send the frame it was written with", "[g2emu]")
{
    InputQueue q(1024, 256);
    const std::int64_t per = Machine::ClocksPerFrame;
    std::int32_t w[4];
    auto take = [&](std::int64_t clock) {
        std::fill(w, w + 4, -1);
        std::int32_t words[4] = {0, 0, 0, 0};  // as Dsp::readRx hands them over
        q.adcFrame(std::uint64_t(clock), words);
        std::copy(words, words + 4, w);
        return w[0];
    };
    // frames that say where they were written: word k of ring frame p is 4 p + k + 1 (of 2^23 - 1)
    auto write = [&](std::uint64_t at, std::size_t frames) {
        std::vector<float> in(4 * frames);
        for(std::size_t i = 0; i < frames; ++i)
            for(std::size_t k = 0; k < 4; ++k) in[4 * i + k] = float(4 * (at + i) + k + 1) / 8388607.0f;
        q.write(at, in.data(), frames);
    };
    // nothing written, and the DACs do not run yet: silence, and nothing is missing
    CHECK(take(100 * per) == 0);
    write(0, 256);
    CHECK(take(100 * per) == 0);
    CHECK(q.missing() == 0);

    // The DACs send ring frame 0 at `zero` (before the machine's time 0 here); the ADCs take ring frame p's input
    // 100 frames after the DACs send ring frame p, wherever in that frame the ADCs' own frame starts
    const std::int64_t zero = -5 * per + 200;
    q.setDacClock(zero);
    q.setLatency(100);
    CHECK(q.latency() == 100);
    CHECK(take(zero + 100 * per) == 1);
    CHECK(w[1] == 2);
    CHECK(w[2] == 3);
    CHECK(w[3] == 4);
    CHECK(take(zero + 100 * per + per - 1) == 1);
    CHECK(take(zero + 101 * per) == 5);
    CHECK(take(zero + 355 * per) == 4 * 255 + 1);
    // before the first frame written: silence
    CHECK(take(zero + 100 * per - 1) == 0);
    CHECK(take(zero) == 0);
    CHECK(q.missing() == 0);
    // a frame that is not written yet (the latency is too short for this reader): silence, counted
    CHECK(take(zero + 356 * per) == 0);
    CHECK(q.missing() == 1);
    write(256, 100);
    CHECK(take(zero + 356 * per) == 4 * 256 + 1);
    // a longer latency takes an earlier frame at the same time
    q.setLatency(150);
    CHECK(take(zero + 356 * per) == 4 * 206 + 1);
    q.setLatency(100);

    // after a pause (a write that does not follow the last one) nothing older is taken: the ring holds old frames there
    write(1000, 256);
    CHECK(take(zero + 1099 * per) == 0);
    CHECK(take(zero + 400 * per) == 0);
    CHECK(take(zero + 1100 * per) == 4 * 1000 + 1);
    CHECK(q.missing() == 1);
    // a frame so old that the next write may write over it is not taken (the machine fell behind the reader)
    write(1256, 256);
    write(1512, 256);
    write(1768, 232);  // written: ring frames 1000 to 1999, in a ring of 1024 whose next write brings up to 256
    CHECK(take(zero + (1300 + 100) * per) == 4 * 1300 + 1);
    CHECK(take(zero + (1232 + 100) * per) == 4 * 1232 + 1);
    CHECK(take(zero + (1231 + 100) * per) == 0);
    CHECK(q.missing() == 1);

    // the converters clip at their full scale
    const float loud[8] = {2.0f, -3.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.0f, 1e-9f};
    q.write(2000, loud, 2);
    CHECK(take(zero + 2100 * per) == 8388607);
    CHECK(w[1] == -8388607);
    CHECK(w[2] == 8388607);
    CHECK(w[3] == -8388607);
    CHECK(take(zero + 2101 * per) == 4194304);  // 0.5 * (2^23 - 1), rounded to even
    CHECK(w[1] == -4194304);
    CHECK(w[2] == 0);
    CHECK(w[3] == 0);
}

TEST_CASE("The user's G2 OS: the audio inputs through the In modules, and the In 1 level LEDs", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    InputRig g2(fw);
    REQUIRE(g2.client.synced());
    auto& src = g2.source;
    // the DACs' words in the order of Machine::run(): Out 1, 3, 2, 4; the ADCs' the same way: In 1, 3, 2, 4
    constexpr std::size_t Out1 = 0, Out2 = 2, Out3 = 1, Out4 = 3;
    constexpr int In1 = 0, In2 = 2, In3 = 1, In4 = 3;
    // An In module (Pad 0 dB) into an Out module (0 dB), the Master Level knob all the way up: the DACs' word is
    // minus half the ADCs' (0.4987: the OS's level table ends at 0.997). In signal units (an oscillator: 1.0) the
    // ADCs' full scale is 4.0 and the DACs' 8.0.
    constexpr double Through = 0.4987;

    // ---- 2-In (In 1/2) -> 2-Out (Out 1/2) ----
    PatchBuilder two;
    const auto in2 = two.add("2-In", 0), out2 = two.add("2-Out", 1);
    two.cable(in2, 0, out2, 0);
    two.cable(in2, 1, out2, 1);
    const auto source = two.paramIndex(in2, "Source"), pad = two.paramIndex(in2, "Pad"), onOff = two.paramIndex(in2, "On/Off");
    REQUIRE(g2.upload(two.done()));
    const double hz[4] = {1000, 3000, 2000, 4000};  // by word: In 1 1 kHz, In 2 2 kHz, In 3 3 kHz, In 4 4 kHz
    for(int k = 0; k < 4; ++k) src.hz[k] = hz[k];
    src.amplitude[In1] = 0.125;
    src.amplitude[In2] = 0.0625;
    src.amplitude[In3] = src.amplitude[In4] = 0.125;
    g2.run(0.1);
    const auto irregular = src.irregular;
    const auto underruns = g2.m.stats().linkUnderruns;
    std::uint64_t clock0 = 0;
    auto out = g2.record(0.25, clock0);
    REQUIRE(out.size() / 4 + 2 >= Machine::FrameRate / 4);
    // no frame of the ADCs or between the DSPs was late or lost meanwhile
    CHECK(src.irregular == irregular);
    CHECK(g2.m.stats().linkUnderruns == underruns);
    const auto left = tone(out, Out1, 1000, clock0), right = tone(out, Out2, 2000, clock0);
    CHECK_THAT(left.amplitude, Catch::Matchers::WithinRel(0.125 * Through, 0.01));
    CHECK_THAT(right.amplitude, Catch::Matchers::WithinRel(0.0625 * Through, 0.01));
    CHECK(tone(out, Out1, 2000, clock0).amplitude < 1e-5);
    CHECK(tone(out, Out1, 3000, clock0).amplitude < 1e-5);
    CHECK(tone(out, Out2, 1000, clock0).amplitude < 1e-5);
    CHECK(tone(out, Out2, 4000, clock0).amplitude < 1e-5);
    // a clean sine, without gaps
    CHECK(residue(out, Out1, 1000, clock0) < 1e-6);
    CHECK(residue(out, Out2, 2000, clock0) < 1e-6);
    // Out 3/4 stay silent
    for(std::size_t f = 0; f < out.size() / 4; ++f)
    {
        REQUIRE(std::fabs(out[4 * f + Out3]) < 1e-6f);
        REQUIRE(std::fabs(out[4 * f + Out4]) < 1e-6f);
    }
    // The way from the ADCs to the DACs: inverted (the DACs' words are, §3.9) and about 20 frames late, the same on
    // both channels: 2 frames in each DSP and the 3 lines between them (the board's own figure is not known).
    const double delay = InputRig::delayOf(left.phase, 1000);
    INFO("ADCs to DACs: " << delay << " frames");
    CHECK(delay > 17);
    CHECK(delay < 23);
    CHECK_THAT(std::fmod(delay, 48.0), Catch::Matchers::WithinAbs(InputRig::delayOf(right.phase, 2000), 0.01));

    // ---- Source In 3/4 ----
    g2.param(in2, source, 1);
    out = g2.record(0.1, clock0);
    CHECK_THAT(tone(out, Out1, 3000, clock0).amplitude, Catch::Matchers::WithinRel(0.125 * Through, 0.01));
    CHECK_THAT(tone(out, Out2, 4000, clock0).amplitude, Catch::Matchers::WithinRel(0.125 * Through, 0.01));
    CHECK(tone(out, Out1, 1000, clock0).amplitude < 1e-5);
    g2.param(in2, source, 0);

    // ---- Pad: +6 dB, 0 dB, -6 dB, -12 dB ----
    const double gains[4] = {2, 1, 0.5, 0.25};
    for(g2::u8 p = 0; p < 4; ++p)
    {
        g2.param(in2, pad, p);
        out = g2.record(0.1, clock0);
        INFO("Pad " << int(p));
        CHECK_THAT(tone(out, Out1, 1000, clock0).amplitude, Catch::Matchers::WithinRel(0.125 * Through * gains[p], 0.01));
    }
    g2.param(in2, pad, 1);

    // ---- the ADCs' full scale passes unclipped (it is a signal of 4.0; the Out module has room up to 8.0) ----
    src.amplitude[In1] = 1.0;
    g2.run(0.05);
    out = g2.record(0.1, clock0);
    CHECK_THAT(tone(out, Out1, 1000, clock0).amplitude, Catch::Matchers::WithinRel(Through, 0.01));
    CHECK(residue(out, Out1, 1000, clock0) < 1e-6);
    src.amplitude[In1] = 0.125;

    // ---- the module switched off ----
    g2.param(in2, onOff, 0);
    out = g2.record(0.1, clock0);
    CHECK(tone(out, Out1, 1000, clock0).amplitude < 1e-5);

    // ---- 4-In -> 4-Out: each input on its own output, as late as before (the lines' delay is fixed) ----
    PatchBuilder four;
    const auto in4 = four.add("4-In", 0), out4 = four.add("4-Out", 1);
    for(g2::u8 k = 0; k < 4; ++k) four.cable(in4, k, out4, k);
    REQUIRE(g2.upload(four.done()));
    src.amplitude[In2] = 0.125;
    g2.run(0.1);
    out = g2.record(0.25, clock0);
    const std::size_t outs[4] = {Out1, Out3, Out2, Out4};  // the output of the input on word k
    for(int k = 0; k < 4; ++k)
        for(int j = 0; j < 4; ++j)
        {
            INFO("the input at " << hz[j] << " Hz on the output whose own is " << hz[k] << " Hz");
            const double a = tone(out, outs[k], hz[j], clock0).amplitude;
            if(j == k) CHECK_THAT(a, Catch::Matchers::WithinRel(0.125 * Through, 0.01));
            else CHECK(a < 1e-5);
        }
    CHECK_THAT(InputRig::delayOf(tone(out, Out1, 1000, clock0).phase, 1000), Catch::Matchers::WithinAbs(delay, 0.01));

    // ---- the In 1 level LEDs: -20, -12 and 0 dB at 0.1, 0.25 and 0.89 of the ADC's full scale, either sign ----
    // (the OS reads one sample of In 1 at a time, takes its magnitude and lets the LEDs fall back one by one; the
    // 0 dB LED stays on for 128 of its readings [C 0x3006bd98])
    src.silence();
    g2.run(2.5);
    auto leds = [&](double level) {
        src.level[In1] = level;
        g2.run(level == 0 ? 2.5 : 0.3);
        const auto p = g2.m.panel();
        return std::array<bool, 3>{p.led(PanelLed::Mic20), p.led(PanelLed::Mic12), p.led(PanelLed::Mic0)};
    };
    CHECK(leds(0.05) == std::array<bool, 3>{false, false, false});
    CHECK(leds(0.15) == std::array<bool, 3>{true, false, false});
    CHECK(leds(0.4) == std::array<bool, 3>{true, true, false});
    CHECK(leds(0.95) == std::array<bool, 3>{true, true, true});
    CHECK(leds(0) == std::array<bool, 3>{false, false, false});
    CHECK(leds(-0.15) == std::array<bool, 3>{true, false, false});
    // the other inputs do not show
    src.silence();
    src.level[In2] = src.level[In3] = src.level[In4] = 0.95;
    g2.run(2.5);
    CHECK_FALSE(g2.m.panel().led(PanelLed::Mic20));
    CHECK(g2.m.stats().exceptions == 0);
}

TEST_CASE("The user's G2 OS: a Vocoder driven by an audio input", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    InputRig g2(fw);
    REQUIRE(g2.client.synced());
    // Noise (the synthesizer input) through a Vocoder whose analysis input (Ctrl) is In 1: the noise comes out in
    // the bands where the input has sound, and not at all while the input is silent.
    PatchBuilder b;
    const auto in = b.add("2-In", 0), noise = b.add("Noise", 1), vocoder = b.add("Vocoder", 2), out = b.add("2-Out", 3);
    b.cable(in, 0, vocoder, 0);     // Ctrl
    b.cable(noise, 0, vocoder, 1);  // In
    b.cable(vocoder, 0, out, 0);
    b.cable(vocoder, 0, out, 1);
    REQUIRE(g2.upload(b.done()));
    auto listen = [&](double hz, double amplitude) {
        g2.source.hz[0] = hz;
        g2.source.amplitude[0] = amplitude;
        g2.run(0.3);  // the envelope followers settle
        std::uint64_t clock0 = 0;
        return g2.record(0.25, clock0);
    };
    auto power = [](const std::vector<float>& o) {
        double sum = 0;
        for(std::size_t f = 0; f < o.size() / 4; ++f) sum += double(o[4 * f]) * o[4 * f];
        return sum / double(std::max<std::size_t>(1, o.size() / 4));
    };
    const auto silent = listen(0, 0);
    // In 1 at a quarter of the ADC's full scale: a signal of 1.0, as an oscillator's
    const auto low = listen(500, 0.25);
    const auto high = listen(4000, 0.25);
    INFO("output power: input silent " << power(silent) << ", 500 Hz " << power(low) << ", 4 kHz " << power(high));
    CHECK(power(low) > 1e-7);
    CHECK(power(high) > 1e-7);
    CHECK(power(silent) < 0.001 * power(low));
    const double lowLow = band(low, 0, 300, 800), lowHigh = band(low, 0, 3000, 5500);
    const double highLow = band(high, 0, 300, 800), highHigh = band(high, 0, 3000, 5500);
    INFO("with 500 Hz: 300-800 Hz " << lowLow << ", 3-5.5 kHz " << lowHigh << "; with 4 kHz: " << highLow << ", " << highHigh);
    CHECK(lowLow > 10 * lowHigh);
    CHECK(highHigh > 10 * highLow);
    CHECK(g2.m.stats().exceptions == 0);
}

TEST_CASE("The user's G2 OS: a held note keeps five envelopes up in a voice of 21 modules", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    // The bigger voice of re/notes/native-engine.md §3.3/§4 (that patch was not kept; this one is put together the
    // same way): with the interpreter in a build that has the JIT, its held note's gate dropped for 55 to 504 ms, or
    // for good (g2-hardware-and-emulation.md §3.8.2). An envelope holds its sustain while the gate is up, so the sum
    // of five envelopes' Env outputs is flat during a held note, and one that drops takes a fifth of it away.
    PatchBuilder b;
    const auto kbd = b.add("Keyboard", 0);
    g2::u8 env[5];
    for(int k = 0; k < 5; ++k)
    {
        env[k] = b.add("EnvADSR", 1, g2::u8(k * 8));
        b.param(env[k], "Attack", 0);
        b.param(env[k], "Decay", 0);
        b.param(env[k], "Sustain", 16);
        b.cable(kbd, 1, env[k], 1);
    }
    const auto sum4 = b.add("Mix4-1A", 2, 0), sum = b.add("Mix2-1A", 2, 10), out = b.add("2-Out", 7, 0);
    for(int k = 0; k < 4; ++k) b.cable(env[k], 0, sum4, g2::u8(k));
    b.cable(sum4, 0, sum, 0);
    b.cable(env[4], 0, sum, 1);
    b.param(sum, "Level1", 127);
    b.param(sum, "Level2", 127);
    b.cable(sum, 0, out, 0);
    // the rest of that voice's load: six oscillators, four of them through lowpass filters, into Out 2
    const auto load4 = b.add("Mix4-1A", 6, 0), load = b.add("Mix2-1A", 6, 10);
    for(int k = 0; k < 6; ++k)
    {
        const auto osc = b.add("OscA", g2::u8(3 + k / 3), g2::u8(k % 3 * 10));
        b.param(osc, "Wave", 2);
        if(k < 4)
        {
            const auto f = b.add("FltLP", 5, g2::u8(k * 8));
            b.cable(osc, 0, f, 0);
            b.cable(f, 0, load4, g2::u8(k));
        }
        else
            b.cable(osc, 0, load, g2::u8(k - 4));
    }
    b.cable(load4, 0, load, 2);
    b.cable(load, 0, out, 1);
    REQUIRE(b.patch.va.modules.size() == 21);

    InputRig g2(fw);
    REQUIRE(g2.client.synced());
    REQUIRE(g2.upload(b.done()));
    g2.run(1.5);  // the output's ramp after an upload is done by 1.7 s (g2-hardware-and-emulation.md §3.8.4)

    const std::uint8_t on[3] = {0x90, 60, 100};
    g2.m.midiInAt(on, g2.m.frame() + 500);
    std::uint64_t clock0 = 0;
    const auto rec = g2.record(3.2, clock0);
    // Out 1 (word 0) in 50 ms windows, from 0.2 s after the note to the end; the outputs come out inverted
    constexpr std::size_t Window = Machine::FrameRate / 20;
    auto mean = [&](std::size_t f, std::size_t n) {
        double s = 0;
        for(std::size_t i = f; i < f + n; ++i) s -= rec[i * 4];
        return s / double(n);
    };
    CHECK(std::fabs(mean(0, 400)) < 0.001);  // before the note: the envelopes are down
    std::vector<double> level;
    for(std::size_t f = 500 + Machine::FrameRate / 5; f + Window <= rec.size() / 4; f += Window) level.push_back(mean(f, Window));
    REQUIRE(level.size() >= 55);
    auto sorted = level;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    CHECK(median > 0.05);
    for(std::size_t w = 0; w < level.size(); ++w)
    {
        INFO("window " << w << " (" << 0.2 + 0.05 * double(w) << " s after the note): " << level[w] << ", median "
                       << median);
        CHECK(std::fabs(level[w] - median) < median * 0.03);
    }
    CHECK(g2.m.stats().exceptions == 0);
}

TEST_CASE("The user's G2 OS: a 3 ms envelope steps once every 4 frames in every note", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    // Control-rate code runs once every 4 frames (re/notes/g2-hardware-and-emulation.md §3.8.1 [C]), so an envelope
    // changes on every 4th frame and holds in between. LinLin attacks rise and releases fall by the same step each
    // time, full scale in the displayed time (re/notes/native-engine.md §2.3). Its 0.5-3 ms stages once showed
    // errors of up to 0.14 around note events (§2.3), measured with the interpreter in a build that has the JIT.
    PatchBuilder b;
    const auto kbd = b.add("Keyboard", 0), env = b.add("EnvADSR", 1), out = b.add("2-Out", 2);
    b.param(env, "Shape", 3);    // LinLin
    b.param(env, "Attack", 10);  // 3.0 ms
    b.param(env, "Decay", 10);   // 3.0 ms
    b.param(env, "Sustain", 64);
    b.param(env, "Release", 10); // 3.0 ms
    b.cable(kbd, 1, env, 1);
    b.cable(env, 0, out, 0);
    constexpr double StageFrames = 3.0e-3 * Machine::FrameRate;  // 288 frames, 72 control ticks

    InputRig g2(fw);
    REQUIRE(g2.client.synced());
    REQUIRE(g2.upload(b.done()));
    g2.run(1.5);  // the output's ramp after an upload

    // eight notes of 30 ms, 70 ms apart, starting at a different frame within a tick each time
    constexpr int Notes = 8;
    constexpr std::uint32_t Spacing = Machine::FrameRate / 10 + 1, Held = Machine::FrameRate * 3 / 100;
    const auto start = g2.m.frame() + 500;
    for(int k = 0; k < Notes; ++k)
    {
        const std::uint8_t on[3] = {0x90, 60, 100}, off[3] = {0x80, 60, 0};
        g2.m.midiInAt(on, start + std::uint64_t(k) * Spacing);
        g2.m.midiInAt(off, start + std::uint64_t(k) * Spacing + Held);
    }
    std::uint64_t clock0 = 0;
    const auto rec = g2.record(double(Notes * Spacing + 1000) / Machine::FrameRate, clock0);
    std::vector<double> v(rec.size() / 4);
    for(std::size_t i = 0; i < v.size(); ++i) v[i] = -rec[i * 4];  // Out 1; the outputs come out inverted

    // The changes of the envelope, frame by frame: (frame, new value)
    std::vector<std::pair<std::size_t, double>> changes;
    for(std::size_t i = 1; i < v.size(); ++i)
        if(v[i] != v[i - 1]) changes.push_back({i, v[i]});
    REQUIRE(!changes.empty());
    const double peak = *std::max_element(v.begin(), v.end());
    REQUIRE(peak > 0.1);

    // Cut the changes into stages: a run of changes in one direction, without a pause (the sustain, the time between
    // notes: more than 64 frames). Within a stage the envelope must change every 4 frames, no more often and no less.
    struct Stage {
        std::size_t first = 0, last = 0;  // the frames of the first and last change
        double from = 0, to = 0;
        std::vector<double> steps;
    };
    std::vector<Stage> stages;
    for(std::size_t c = 0; c < changes.size();)
    {
        Stage s;
        s.from = c ? changes[c - 1].second : 0.0;
        s.first = changes[c].first;
        const bool rising = changes[c].second > s.from;
        double prev = s.from;
        std::size_t e = c;
        for(; e < changes.size() && (changes[e].second > prev) == rising && (e == c || changes[e].first - changes[e - 1].first <= 64);
            ++e)
        {
            s.steps.push_back(changes[e].second - prev);
            prev = changes[e].second;
            s.last = changes[e].first;
        }
        s.to = prev;
        INFO("stage from frame " << s.first << " to " << s.last << ", " << s.steps.size() << " steps, from " << s.from
                                 << " to " << s.to);
        for(std::size_t k = c + 1; k < e; ++k) CHECK(changes[k].first - changes[k - 1].first == 4);
        stages.push_back(s);
        c = e;
    }
    // Each note: an attack from 0 to the peak, a decay to the sustain (half the peak), a release to 0; each at full
    // scale per displayed time, so 288, 144 and 144 frames, by steps of one size (the last may be cut short).
    REQUIRE(stages.size() == 3 * Notes);
    for(std::size_t k = 0; k < stages.size(); ++k)
    {
        const auto& s = stages[k];
        INFO("note " << k / 3 << ", stage " << k % 3 << ": frames " << s.first << " to " << s.last << ", from " << s.from
                     << " to " << s.to);
        const double expectFrom[3] = {0, peak, peak / 2}, expectTo[3] = {peak, peak / 2, 0};
        CHECK(std::fabs(s.from - expectFrom[k % 3]) < peak * 0.01);
        CHECK(std::fabs(s.to - expectTo[k % 3]) < peak * 0.01);
        const double frames = double(s.last - s.first + 4);
        CHECK(std::fabs(frames - (k % 3 == 0 ? StageFrames : StageFrames / 2)) <= 8);
        const double step = s.steps[s.steps.size() / 2];
        for(std::size_t n = 0; n + 1 < s.steps.size(); ++n) CHECK(std::fabs(s.steps[n] - step) < std::fabs(step) * 0.02);
    }
    CHECK(g2.m.stats().exceptions == 0);
}

TEST_CASE("The user's G2 OS: no frame repeated or lost while notes and parameter changes come in", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    // A sine plays on Out 1 throughout; notes over MIDI and parameter changes over USB go to a second voice on
    // Out 2. Each frame of a sine is 2 cos(w) times the one before it minus the one before that, so a frame that
    // comes twice, or one that is missing, stands out. re/notes/native-engine.md §3.3 found about 0.1 % of frames
    // repeated around note events, with the interpreter in a build that has the JIT.
    PatchBuilder b;
    const auto sine = b.add("OscA", 0), kbd = b.add("Keyboard", 1), osc = b.add("OscA", 2), env = b.add("EnvADSR", 3),
               out = b.add("2-Out", 4);
    b.param(sine, "Wave", 0);
    b.param(sine, "KBT", 0);
    b.param(sine, "Coarse", 76);  // a fixed pitch, without the keyboard
    b.param(osc, "Wave", 2);
    b.param(env, "Attack", 0);
    b.cable(sine, 0, out, 0);
    b.cable(osc, 0, env, 0);
    b.cable(kbd, 1, env, 1);
    b.cable(env, 1, out, 1);
    const auto sustain = b.paramIndex(env, "Sustain");

    InputRig g2(fw);
    REQUIRE(g2.client.synced());
    REQUIRE(g2.upload(b.done()));
    g2.run(1.5);  // the output's ramp after an upload

    // 3 s: a note every 50 ms (held 25 ms), a Sustain change over USB every 20 ms, the client served every ms
    std::vector<float> rec;
    const auto start = g2.m.frame();
    for(int ms = 0; ms < 3000; ++ms)
    {
        if(ms % 50 == 0)
        {
            const std::uint8_t on[3] = {0x90, std::uint8_t(48 + ms / 50 % 24), 100};
            const std::uint8_t off[3] = {0x80, on[1], 0};
            g2.m.midiInAt(on, g2.m.frame() + 10);
            g2.m.midiInAt(off, g2.m.frame() + 10 + Machine::FrameRate / 40);
        }
        if(ms % 20 == 0) g2.client.setParam(0, g2::Location::Va, env, sustain, std::uint8_t(40 + ms / 20 % 80), 0);
        g2.m.run(Machine::FrameRate / 1000, &rec);
        g2.clock.set(std::uint64_t(g2.m.seconds() * 1000));
        g2.client.tick();
    }
    REQUIRE(g2.client.synced());
    REQUIRE(g2.m.frame() - start >= 3 * Machine::FrameRate - 2);
    std::vector<double> x(rec.size() / 4);
    for(std::size_t i = 0; i < x.size(); ++i) x[i] = rec[i * 4];

    // the sine's 2 cos(w) by least squares, and its amplitude
    double num = 0, den = 0, peak = 0;
    for(std::size_t i = 1; i + 1 < x.size(); ++i)
    {
        num += (x[i + 1] + x[i - 1]) * x[i];
        den += x[i] * x[i];
        peak = (std::max)(peak, std::fabs(x[i]));
    }
    const double c2 = num / den;
    REQUIRE(peak > 0.1);
    CHECK(c2 < 2.0);  // a tone, not DC
    auto outliers = [&](const std::vector<double>& s) {
        std::vector<std::size_t> at;
        for(std::size_t i = 1; i + 1 < s.size(); ++i)
            if(std::fabs(s[i + 1] - c2 * s[i] + s[i - 1]) > peak * 3e-4) at.push_back(i);
        return at;
    };
    const auto bad = outliers(x);
    INFO(bad.size() << " frames off the sine, the first at " << (bad.empty() ? 0 : bad[0]) << " of " << x.size());
    CHECK(bad.empty());

    // The check sees one: the same recording with a frame repeated, and one with a frame left out, at the sine's top
    // where they change it least (w^2 / 2 of the amplitude: 9e-4 here). The oscillator's own sine is off a pure one
    // by up to about 1.2e-4 of its amplitude, at its tops; the check's limit is 3e-4.
    std::size_t top = 50000;
    for(std::size_t i = top; i < top + 200; ++i)
        if(x[i] > x[top]) top = i;
    auto repeated = x;
    repeated.insert(repeated.begin() + std::ptrdiff_t(top), repeated[top]);
    CHECK(!outliers(repeated).empty());
    auto lost = x;
    lost.erase(lost.begin() + std::ptrdiff_t(top));
    CHECK(!outliers(lost).empty());
    CHECK(g2.m.stats().exceptions == 0);
}

TEST_CASE("The user's G2 OS in real time: audio inputs on the reader's timeline (Runner::write)", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    Runner runner(fw);  // as the plugin runs it: the DSPs on threads of their own
    auto link = emulatedG2Link(runner.machine());
    // As an audio thread: blocks of N frames in, N frames out. It waits for the machine instead of the clock (as the
    // plugin does offline), so that no frame is missing and every frame can be checked.
    constexpr std::size_t N = 256;
    std::vector<float> in(4 * N, 0.0f), out(4 * N), heard;
    std::uint64_t position = 0;
    double amplitude = 0;
    bool keep = false;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto within = [&](double s) { return std::chrono::duration<double>(Clock::now() - start).count() < s; };
    auto block = [&] {
        while(runner.available() < N && within(120)) std::this_thread::sleep_for(std::chrono::microseconds(200));
        for(std::size_t i = 0; i < N; ++i)
        {
            // In 1 (word 0): 1 kHz; In 2 (word 2): 1.5 kHz at half the level
            in[4 * i] = float(amplitude * std::sin(kTwoPi * 1000.0 * double(position + i) / Machine::FrameRate));
            in[4 * i + 2] = float(0.5 * amplitude * std::sin(kTwoPi * 1500.0 * double(position + i) / Machine::FrameRate));
        }
        runner.write(in.data(), N);
        runner.read(out.data(), N);
        position += N;
        if(keep) heard.insert(heard.end(), out.begin(), out.end());
        link->tick();
    };
    while(!link->synced() && within(20)) block();
    REQUIRE(link->synced());
    PatchBuilder b;
    const auto in2 = b.add("2-In", 0), out2 = b.add("2-Out", 1);
    b.cable(in2, 0, out2, 0);
    b.cable(in2, 1, out2, 1);
    link->sendPerformance(g2::Performance::playing(b.done(), "In"), "In");
    while(!link->client().idle() && within(30)) block();
    REQUIRE(link->client().idle());
    for(int i = 0; i < 200; ++i) block();  // the output ramps up after an upload

    // The tone starts with a block: it comes out inputLatencyFrames() later, plus the G2's own way from its ADCs
    // to its DACs (about 20 frames).
    keep = true;
    amplitude = 0.25;
    const std::uint64_t started = position;
    for(int i = 0; i < 150; ++i) block();
    REQUIRE(runner.stats().framesMissing == 0);
    CHECK(runner.stats().inputMissing == 0);  // every frame's input was there when the ADCs took it
    std::size_t onset = 0;
    while(onset < heard.size() / 4 && std::fabs(heard[4 * onset]) < 1e-5f) ++onset;
    const double way = double(onset) - double(runner.inputLatencyFrames());
    INFO("the tone came out " << onset << " frames after it went in; input latency " << runner.inputLatencyFrames()
                              << " frames (of which the reader's " << runner.latencyFrames() << ")");
    CHECK(runner.inputLatencyFrames() > runner.latencyFrames());
    CHECK(way > 16);
    CHECK(way < 24);
    // steady from there on: both tones at their levels (the DACs' word is minus 0.4987 of the ADCs'), without gaps,
    // and as late as the start was
    const std::vector<float> steady(heard.begin() + std::ptrdiff_t(4 * (onset + 2000)), heard.end());
    const std::uint64_t at = (started + onset + 2000) * std::uint64_t(Machine::ClocksPerFrame);  // its first frame, on the input's time
    const auto left = tone(steady, 0, 1000, at), right = tone(steady, 2, 1500, at);
    CHECK_THAT(left.amplitude, Catch::Matchers::WithinRel(0.25 * 0.4987, 0.01));
    CHECK_THAT(right.amplitude, Catch::Matchers::WithinRel(0.125 * 0.4987, 0.01));
    CHECK(residue(steady, 0, 1000, at) < 1e-6);
    CHECK(residue(steady, 2, 1500, at) < 1e-6);
    const double delay = InputRig::delayOf(left.phase, 1000);  // modulo the tone's period, 96 frames
    INFO("by the phase: " << delay << " frames (modulo 96)");
    CHECK(std::fabs(std::remainder(delay - double(onset), 96.0)) < 1.5);

    // Beyond the ADCs' full scale the converters clip: an input of twice that comes out flat at 0.4987
    heard.clear();
    amplitude = 2.0;
    for(int i = 0; i < 40; ++i) block();
    float peak = 0;
    std::size_t flat = 0;
    for(std::size_t f = heard.size() / 8; f < heard.size() / 4; ++f)
    {
        peak = std::max(peak, std::fabs(heard[4 * f]));
        if(std::fabs(heard[4 * f]) > 0.4987f * 0.999f) ++flat;
    }
    CHECK_THAT(peak, Catch::Matchers::WithinRel(0.4987, 0.005));
    CHECK(flat > heard.size() / 8 / 2);  // a sine of twice the full scale is beyond it two thirds of the time
    CHECK(runner.stats().inputMissing == 0);
    runner.stop();
    CHECK(runner.machine().stats().exceptions == 0);
    CHECK(runner.machine().stats().dspsWild == 0);
}

TEST_CASE("The user's G2 OS in real time: the reserve and the reader's block are the latency, and MIDI lands by it", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    // A small reserve, and blocks shorter than the machine's stretch: as a host with a 16-sample buffer at 48 kHz
    Runner::Options options;
    options.bufferMs = 2;
    options.stretchMs = 1;
    options.machine.realtimeThreads = true;
    Runner runner(fw, options);
    constexpr std::size_t N = 32;
    runner.setReadSize(N);
    // the reserve and the stretch (longer than the block), and the 10 frames the DACs may be ahead of the machine
    CHECK(runner.latencyFrames() == 192 + 96 + 10);
    auto link = emulatedG2Link(runner.machine());
    // As an audio thread that waits for the machine instead of the clock, so that no frame is missing.
    std::vector<float> out(4 * N), heard;
    std::size_t most = 0;
    bool keep = false;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto within = [&](double s) { return std::chrono::duration<double>(Clock::now() - start).count() < s; };
    auto block = [&] {
        while(runner.available() < N && within(120)) std::this_thread::sleep_for(std::chrono::microseconds(100));
        most = std::max(most, runner.available());
        runner.read(out.data(), N);
        if(keep) heard.insert(heard.end(), out.begin(), out.end());
        link->tick();
    };
    while(!link->synced() && within(20)) block();
    REQUIRE(link->synced());
    link->sendPerformance(g2::Performance::playing(keyboardSine(), "Sine"), "Sine");
    while(!link->client().idle() && within(30)) block();
    REQUIRE(link->client().idle());
    for(int i = 0; i < 3000; ++i) block();  // the output ramps up after an upload

    // A note 5 frames into a block sounds latencyFrames() after that place, plus the G2's own time for a note: its
    // three bytes on the MIDI line (92 frames) and the OS's reaction. `at` frames (in blocks) are recorded.
    const std::uint8_t on[3] = {0x90, 69, 100}, off[3] = {0x80, 69, 64};
    auto note = [&](std::size_t frames) {
        heard.clear();
        keep = true;
        runner.midiInAt(on, 5);
        for(std::size_t f = 0; f < frames; f += N) block();
        keep = false;
        std::size_t onset = 0;
        while(onset < heard.size() / 4 && std::fabs(heard[onset * 4]) < 1e-3f) ++onset;
        runner.midiIn(off);
        for(int i = 0; i < 3000; ++i) block();
        return double(onset) - 5.0;
    };
    const double first = note(4800) - double(runner.latencyFrames());
    INFO("with a reserve of 2 ms a note sounds " << first << " frames after the latency (" << runner.latencyFrames() << " frames)");
    CHECK(first >= 92);
    CHECK(first < 92 + 960);
    // the machine was never further ahead of the reader than the latency says (what MIDI and the inputs rely on)
    INFO("the machine was at most " << most << " frames ahead of the reader");
    CHECK(most <= runner.latencyFrames());

    // The reserve changed while it runs: the latency follows, and the note's own time stays what it was
    runner.setBufferMs(10);
    CHECK(runner.latencyFrames() == 960 + 96 + 10);
    CHECK(runner.inputLatencyFrames() > runner.latencyFrames());
    for(int i = 0; i < 300; ++i) block();
    most = 0;
    const double second = note(4800) - double(runner.latencyFrames());
    INFO("with a reserve of 10 ms: " << second << " frames after the latency");
    CHECK(std::fabs(second - first) < 48);  // (the OS takes its MIDI bytes within half a millisecond)
    INFO("then at most " << most << " frames ahead; " << runner.stats().framesMissing << " frames missing, "
                         << runner.stats().shortReads << " short reads");
    CHECK(most <= runner.latencyFrames());
    CHECK(runner.stats().framesMissing == 0);
    CHECK(runner.stats().shortReads == 0);
    runner.stop();
    CHECK(runner.machine().stats().exceptions == 0);
}

namespace {

// A 440 Hz sine to Bus 1/2 and a 2-In (Bus 1/2) to Out 1/2: once around the ring (§3.7.2). An 880 Hz sine to
// FX 1/2 and, in the FX area, an Fx-In to Out 3/4: down the chain.
g2::Patch busAndFxPatch()
{
    PatchBuilder b;
    const auto osc = b.add("OscA", 0), toBus = b.add("2-Out", 1), fromBus = b.add("2-In", 2), out = b.add("2-Out", 3);
    const auto osc2 = b.add("OscA", 0, 10), toFx = b.add("2-Out", 1, 10);
    for(const auto o : {osc, osc2})
    {
        b.param(o, "Wave", 0);
        b.param(o, "KBT", 0);
    }
    b.param(osc, "Coarse", 69);
    b.param(osc2, "Coarse", 81);
    b.param(toBus, "Dest", 4);
    b.param(fromBus, "Source", 2);
    b.param(toFx, "Dest", 2);
    b.cable(osc, 0, toBus, 0);
    b.cable(fromBus, 0, out, 0);
    b.cable(osc2, 0, toFx, 0);
    auto patch = b.done();
    g2::u8 fxIn = 0, fxOut = 0;
    for(const auto& d : g2::db::modules())
        if(std::string(d.shortName) == "Fx-In") fxIn = g2::edit::addModule(patch, g2::Location::Fx, d.typeId, 0, 0);
    for(const auto& d : g2::db::modules())
        if(std::string(d.shortName) == "2-Out") fxOut = g2::edit::addModule(patch, g2::Location::Fx, d.typeId, 1, 0);
    const auto* def = patch.fx.find(fxOut)->def();
    for(std::size_t i = 0; i < def->params.size(); ++i)
        if(std::string(def->params[i].name) == "Dest")
            for(g2::u8 v = 0; v < g2::kFileVariations; ++v) g2::edit::setParam(patch, g2::Location::Fx, fxOut, g2::u8(i), v, 1);
    g2::edit::connect(patch, g2::Location::Fx, {fxIn, 0, true}, {fxOut, 0, false});
    g2::uprate::update(patch, g2::Location::Fx);
    return patch;
}

} // namespace

TEST_CASE("The user's G2 OS: a bus and an FX send cross the lines between the DSPs", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    InputRig g2(fw);
    REQUIRE(g2.client.synced());
    const auto patch = busAndFxPatch();
    // twice: the lines are as long after another upload (they grew with each one before they had a fixed delay)
    for(int upload = 0; upload < 2; ++upload)
    {
        REQUIRE(g2.upload(patch));
        const auto underruns = g2.m.stats().linkUnderruns;
        std::uint64_t clock0 = 0;
        const auto o = g2.record(0.25, clock0);
        INFO("upload " << upload);
        // a sine of 1.0 through an Out module (and a bus, or the FX area): a DAC word of 0.125 (x 0.997)
        CHECK_THAT(tone(o, 0, 440.0, clock0).amplitude, Catch::Matchers::WithinRel(0.1247, 0.01));  // Out 1
        CHECK_THAT(tone(o, 1, 880.0, clock0).amplitude, Catch::Matchers::WithinRel(0.1247, 0.01));  // Out 3
        CHECK(tone(o, 0, 880.0, clock0).amplitude < 1e-4);
        CHECK(tone(o, 1, 440.0, clock0).amplitude < 1e-4);
        // no frame is lost on any line while the patch plays
        CHECK(g2.m.stats().linkUnderruns == underruns);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The voice expansion board (re/notes §3.12)

namespace {

// A G2 keyboard with the board fitted.
Machine::Options expandedG2()
{
    Machine::Options o;
    o.expansion = Expansion::With;
    return o;
}

// Keyboard -> OscA (sine, note 69 = 440 Hz) -> EnvADSR -> 2-Out per voice, as keyboardSine(), with `voices` voices
// asked. Beside it `stacks` oscillator -> filter pairs that nothing listens to: they only cost DSP time, so that the
// OS fits few voices on a DSP and has to spread a chord over several.
g2::Patch heavyPolySine(int voices, int stacks)
{
    PatchBuilder b;
    const auto kbd = b.add("Keyboard", 0), osc = b.add("OscA", 1), env = b.add("EnvADSR", 2), out = b.add("2-Out", 3);
    b.param(osc, "Wave", 0);
    b.param(osc, "Coarse", 64);
    b.param(env, "Attack", 0);
    b.param(env, "Sustain", 127);
    b.cable(osc, 0, env, 0);
    b.cable(kbd, 1, env, 1);
    b.cable(env, 1, out, 0);
    for(int k = 0; k < stacks; ++k)
    {
        const auto o = b.add("OscB", g2::u8(4 + k), 0), f = b.add("FltNord", g2::u8(4 + k), 10);
        b.cable(o, 0, f, 0);
    }
    g2::edit::setVoices(b.patch, g2::edit::VoiceMode::Poly, g2::u8(voices));
    return b.done();
}

double noteHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// What each DSP executes per frame (of 1536) over the next `seconds`, by chip select (A3..A6, then the expansion
// board's A7..A10): where the OS put the patch.
std::array<double, Machine::MaxDsps> dspLoads(InputRig& g2, double seconds)
{
    const auto before = g2.m.stats();
    const auto f0 = g2.m.frame();
    g2.run(seconds);
    const auto after = g2.m.stats();
    std::array<double, Machine::MaxDsps> load{};
    for(std::size_t n = 0; n < load.size(); ++n)
        load[n] = double(after.dspExecuted[n] - before.dspExecuted[n]) / double(g2.m.frame() - f0);
    return load;
}

std::string loadsText(const std::array<double, Machine::MaxDsps>& load, int dsps)
{
    std::string s;
    for(int n = 0; n < dsps; ++n) s += (n ? " " : "") + std::to_string(int(load[std::size_t(n)]));
    return s;
}

// The amplitude of `hz` in word `word` under a Hann window: the other notes of a chord, tens of hertz away, do
// not leak into it (into tone()'s plain sum they do, by a few percent over a quarter of a second).
double chordTone(const std::vector<float>& out, std::size_t word, double hz)
{
    double re = 0, im = 0, sum = 0;
    const std::size_t n = out.size() / 4;
    for(std::size_t i = 0; i < n; ++i)
    {
        const double window = 0.5 - 0.5 * std::cos(kTwoPi * (double(i) + 0.5) / double(n));
        const double w = kTwoPi * hz * double(i) / Machine::FrameRate;
        re += window * out[4 * i + word] * std::sin(w);
        im += window * out[4 * i + word] * std::cos(w);
        sum += window;
    }
    return 2 * std::hypot(re, im) / sum;
}

// The largest third difference of word `word`: a sum of low sines has almost none ((2 pi f / fs)^3 of each sine's
// amplitude: under 1e-5 here, 2e-5 measured), a frame lost, repeated or out of place between two DSPs a hundred
// times more (a step of a sine's slope: 1.8e-3 for a full 220 Hz).
double roughness(const std::vector<float>& out, std::size_t word)
{
    double worst = 0;
    for(std::size_t f = 3; f < out.size() / 4; ++f)
        worst = std::max(worst, std::fabs(double(out[4 * f + word]) - 3.0 * out[4 * (f - 1) + word] + 3.0 * out[4 * (f - 2) + word]
                                          - out[4 * (f - 3) + word]));
    return worst;
}

// The chord `notes` held on MIDI channel 1: each note's amplitude on Out 1, and the roughness of what was recorded
// against what the tones themselves account for.
struct Chord {
    std::vector<double> amplitude;
    double roughness = 0, smooth = 0;
};
Chord playChord(InputRig& g2, const std::vector<int>& notes)
{
    for(const int n : notes)
    {
        const std::uint8_t on[3] = {0x90, std::uint8_t(n), 100};
        g2.m.midiIn(on);
        g2.run(0.01);
    }
    g2.run(0.2);
    std::uint64_t clock0 = 0;
    const auto out = g2.record(0.25, clock0);
    Chord c;
    for(const int n : notes)
    {
        const double a = chordTone(out, 0, noteHz(n));
        c.amplitude.push_back(a);
        c.smooth += a * std::pow(kTwoPi * noteHz(n) / Machine::FrameRate, 3.0);
    }
    c.roughness = roughness(out, 0);
    for(const int n : notes)
    {
        const std::uint8_t off[3] = {0x80, std::uint8_t(n), 0};
        g2.m.midiIn(off);
    }
    g2.run(0.1);
    return c;
}

// The version reply as the OS sends it over USB (the bytes from the 0x80 on).
Bytes versionReply(Machine& m)
{
    m.plugUsb();
    while(!m.usbArrived() && m.seconds() < 4) m.run(Machine::FrameRate / 1000);
    m.usbSend(g2::proto::versionRequest());
    Bytes reply;
    for(int ms = 0; ms < 500 && reply.empty(); ++ms)
    {
        m.run(Machine::FrameRate / 1000);
        Machine::UsbIn in;
        while(m.usbTake(in))
            if(in.bulk && in.data.size() > 2 && in.data[0] == 0x80 && in.data[1] == 0x0A) reply = in.data;
    }
    return reply;
}

// The line of the main display that says "Version" while the OS boots (it comes and goes within the first second).
std::string bootVersionLine(Machine& m)
{
    std::string line;
    while(m.seconds() < 2)
    {
        m.run(Machine::FrameRate / 100);
        if(!line.empty()) continue;
        const auto p = m.panel();
        for(int l = 0; l < 2; ++l)
            if(p.displays[0].text(l).find("Version") != std::string::npos) line = p.displays[0].text(l);
    }
    while(!line.empty() && line.back() == ' ') line.pop_back();
    return line;
}

} // namespace

TEST_CASE("Expansion board: a G2X has it, a G2 and a G2 Engine when fitted", "[g2emu]")
{
    Machine::Options o;
    CHECK_FALSE(Machine::hasExpansion(o));  // a G2 keyboard as built
    o.model = PanelModel::G2Engine;
    CHECK_FALSE(Machine::hasExpansion(o));
    o.expansion = Expansion::With;
    CHECK(Machine::hasExpansion(o));
    o.model = PanelModel::G2;
    CHECK(Machine::hasExpansion(o));
    o.model = PanelModel::G2X;
    o.expansion = Expansion::AsBuilt;
    CHECK(Machine::hasExpansion(o));
    o.expansion = Expansion::Without;  // no such instrument: for a computer that cannot run eight DSPs
    CHECK_FALSE(Machine::hasExpansion(o));
}

TEST_CASE("The plugin's G2X: without its expansion board unless the user fits it", "[g2emu]")
{
    // Every G2X has the board (re/notes §3.12); the plugin leaves it out by default because a heavy patch on eight
    // DSPs runs below real time on an M1 (§3.12.5), a deliberate deviation listed in the README.
    Machine::Options o;
    g2io::pluginMachine(o);
    CHECK(o.model == PanelModel::G2X);
    CHECK_FALSE(Machine::hasExpansion(o));
    g2io::pluginMachine(o, true);
    CHECK(o.model == PanelModel::G2X);
    CHECK(Machine::hasExpansion(o));
    g2io::pluginMachine(o, false);
    CHECK_FALSE(Machine::hasExpansion(o));

    // The user's choice in the settings file, and back; anything else is the default, without.
    CHECK(g2io::expansionKey(true) == "on");
    CHECK(g2io::expansionKey(false) == "off");
    CHECK(g2io::expansionFromKey("on"));
    CHECK_FALSE(g2io::expansionFromKey("off"));
    CHECK_FALSE(g2io::expansionFromKey(""));
    CHECK_FALSE(g2io::expansionFromKey("yes"));
}

TEST_CASE("The computer test's advice from the speed of a heavy patch on eight DSPs", "[g2emu]")
{
    using g2io::ExpansionAdvice;
    // Real time in a host wants about 1.5 x off-line (re/notes §3.9.7); below 1 it cannot keep up at all: on 2026-10-14
    // eight DSPs at 0.82 x lost all of the audio in real time, four at 2.0 x lost none (§3.12.5, audio-path.md §3).
    CHECK(g2io::expansionAdvice(0.82) == ExpansionAdvice::TooSlow);
    CHECK(g2io::expansionAdvice(0.99) == ExpansionAdvice::TooSlow);
    CHECK(g2io::expansionAdvice(1.0) == ExpansionAdvice::QuietOnly);
    CHECK(g2io::expansionAdvice(1.2) == ExpansionAdvice::QuietOnly);
    CHECK(g2io::expansionAdvice(1.49) == ExpansionAdvice::QuietOnly);
    CHECK(g2io::expansionAdvice(1.5) == ExpansionAdvice::Holds);
    CHECK(g2io::expansionAdvice(2.0) == ExpansionAdvice::Holds);
    // The threads to keep: the faster of those tried
    const std::vector<std::pair<int, double>> tried = {{2, 0.82}, {3, 1.20}};
    CHECK(g2io::fastestThreads(tried) == 3);
    const std::vector<std::pair<int, double>> twoFaster = {{2, 1.6}, {3, 1.4}};
    CHECK(g2io::fastestThreads(twoFaster) == 2);
    // ...which the machine then runs with the board, and only with it (without: as before, Machine::autoThreads)
    Machine::Options o;
    g2io::pluginMachine(o, true, 3);
    CHECK(o.threads == 3);
    g2io::pluginMachine(o, true);
    CHECK(o.threads == -1);
    g2io::pluginMachine(o, false, 3);
    CHECK(o.threads == -1);
}

TEST_CASE("The user's G2 OS finds the expansion board: eight DSPs booted, Exp on its display and in its version reply",
          "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    constexpr std::uint32_t DspCount = 0x302AA5D4;  // the OS's number of DSPs (a byte) [C 0x30038416]
    constexpr int A3 = 0, A4 = 1, A5 = 2, A6 = 3;
    auto program = [](const Machine& m, int n) {
        // stage 1 (P:$0-$234) and the frame program of the empty patch behind it (§3.6.3)
        std::vector<std::uint32_t> p;
        for(std::uint32_t a = 0; a < 0x2C0; ++a) p.push_back(m.dspMemory(n, 0, a));
        return p;
    };

    Machine base(fw);
    CHECK(bootVersionLine(base) == "Version 1.62");
    CHECK(base.stats().dsps == 4);
    CHECK((base.cfRead32(DspCount) >> 24) == 4);
    const auto baseReply = versionReply(base);
    REQUIRE(baseReply.size() > 34);
    CHECK(baseReply[34] == 2);  // [C 0x3001C606]: 1 with the board, 2 without

    Machine m(fw, expandedG2());
    CHECK(bootVersionLine(m) == "Version 1.62 Exp");  // [C 0x3001B882]
    const auto s = m.stats();
    CHECK(s.dsps == 8);
    CHECK((m.cfRead32(DspCount) >> 24) == 8);
    CHECK(s.exceptions == 0);
    CHECK(s.dspsWild == 0);
    for(int n = 0; n < 8; ++n)
    {
        INFO("DSP on chip select A" << 3 + n);
        CHECK(s.dspExecuted[n] > 0);
        // each runs its frame program: the frame counter of stage 1's background loop moves (X:$43, §3.8.1) and the
        // control ticks count up (L:$42)
        CHECK(m.dspMemory(n, 1, 0x42) + m.dspMemory(n, 2, 0x42) != 0);
    }
    const auto reply = versionReply(m);
    REQUIRE(reply.size() > 34);
    CHECK(reply[34] == 1);
    CHECK(reply.size() == baseReply.size());
    CHECK(reply[2] == 0);  // still a G2 keyboard

    // The OS loads the eight DSPs as it loads four: its first (A6) and its last (A3) get the programs they get
    // without the board, the six between them the program of A5 and A4. So the expansion board's DSPs are four more
    // DSPs in the middle of the serial chain (§3.12.2).
    CHECK(program(m, A6) == program(base, A6));
    CHECK(program(m, A3) == program(base, A3));
    CHECK(program(base, A5) == program(base, A4));
    CHECK(program(base, A6) != program(base, A5));
    CHECK(program(base, A3) != program(base, A5));
    for(const int n : {A4, A5, 4, 5, 6, 7})
    {
        INFO("DSP on chip select A" << 3 + n);
        CHECK(program(m, n) == program(base, A5));
    }
}

TEST_CASE("The user's G2 OS: with the expansion board a heavy patch gets a G2X's voices", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    // The bench pack's voice patches, 32 voices asked: two and three oscillator-filter-envelope chains per voice
    // (21 % and 31 % of a DSP), and eight (79 %) with a reverb in the FX area. The manual: 31 % "would allow 12 voices
    // or 24 voices on an expanded system or on the G2X model"; with the FX area on a DSP of its own "the minimum
    // polyphony is three voices plus effects or seven voices plus effects in an expanded or G2X model" [M p.121].
    const char* files[3] = {"B11 Voices Light.pch2", "B12 Voices Mid.pch2", "B13 Voices Heavy.pch2"};
    const int unexpanded[3] = {16, 12, 3}, expanded[3] = {32, 24, 7};
    const char* shownExpanded[3] = {"32 (32)", "24 (32)", "07 (32)"};
    std::array<std::array<g2::u8, 27>, 3> unexpandedLoad{};
    for(const bool board : {false, true})
    {
        Machine::Options o;
        o.model = board ? PanelModel::G2X : PanelModel::G2;  // as built
        InputRig g2(fw, o);
        REQUIRE(g2.client.synced());
        CHECK(g2.client.state().version.model == (board ? g2::proto::VersionInfo::G2X : g2::proto::VersionInfo::G2));
        for(int k = 0; k < 3; ++k)
        {
            INFO(files[k] << (board ? " on a G2X" : " on a G2"));
            std::ifstream f(std::filesystem::path(G2_SOURCE_DIR) / "bench" / "hardware-pack" / "patches" / files[k], std::ios::binary);
            REQUIRE(f);
            const Bytes bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
            const auto loaded = g2::load(bytes);
            g2.client.sendPatch(0, std::get<g2::Patch>(loaded.content), "Voices");
            const double end = g2.m.seconds() + 4;
            while(!g2.client.idle() && g2.m.seconds() < end) g2.step();
            REQUIRE(g2.client.idle());
            g2.run(0.3);
            CHECK(int(g2.client.state().voices[0]) == (board ? expanded[k] : unexpanded[k]));
            // what the editor shows as the patch load is one voice's share of a DSP: the same with the board
            REQUIRE(g2.client.state().slots[0].load[1]);
            const auto& load = g2.client.state().slots[0].load[1]->counters;
            if(!board) unexpandedLoad[std::size_t(k)] = load;
            else
            {
                CHECK(load == unexpandedLoad[std::size_t(k)]);
                // the Patch Settings page shows the voices above knob 2 (§3.10.2)
                CHECK(g2.m.panel().displays[1].text(0).find(shownExpanded[k]) != std::string::npos);
            }
        }
        CHECK(g2.m.stats().exceptions == 0);
        CHECK(g2.m.stats().dspsWild == 0);
    }
}

TEST_CASE("The speed test: how fast this computer runs a heavy patch, on four DSPs and on eight", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    std::ifstream f(std::filesystem::path(G2_SOURCE_DIR) / "bench" / "hardware-pack" / "patches" / "B13 Voices Heavy.pch2", std::ios::binary);
    REQUIRE(f);
    const Bytes bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    const auto patch = std::get<g2::Patch>(g2::load(bytes).content);
    const std::vector<int> chord = {48, 55, 60, 64, 67, 72, 76};
    // What it reports holds on any computer: the machine it ran, the voices the OS gave (the manual's three and seven
    // plus effects, as in the test above), that the patch sounded; the speed itself is only above zero.
    for(const bool board : {false, true})
    {
        INFO((board ? "eight DSPs" : "four DSPs"));
        Machine::Options o;
        g2io::pluginMachine(o, board);
        o.threads = 2;
        const auto r = measureSpeed(fw, o, patch, chord, 0.3);
        CHECK(r.error.empty());
        CHECK(r.dsps == (board ? 8 : 4));
        CHECK(r.threads == 2);
        CHECK(r.voices == (board ? 7 : 3));
        CHECK(r.peak > 0.01f);
        CHECK(r.speed > 0.0);
        CHECK(r.busiestDspThread > 0.0);
    }
    // Asked to stop (the user closed the window), it stops before it boots
    std::atomic<bool> stop{true};
    Machine::Options o;
    g2io::pluginMachine(o, true);
    CHECK(measureSpeed(fw, o, patch, chord, 0.3, &stop).error == "stopped");
}

TEST_CASE("The user's G2 OS: a chord spread over the eight DSPs sounds as on four, without a click", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    // A sine per voice and enough beside it that a DSP holds one voice. Three voices asked: both machines give them,
    // on three DSPs. Seven asked: the expanded one alone gives them all, one on each of seven DSPs.
    const std::vector<int> three = {57, 64, 72}, seven = {57, 60, 64, 67, 71, 74, 77};
    const int stacks = 8;
    constexpr double Full = 0.1247;  // a sine of 1.0 at the DACs (§3.9.8)
    auto heavyDsps = [](const std::array<double, Machine::MaxDsps>& load) {
        int n = 0;
        for(const double l : load) n += l > 600;  // the empty patch's frame and control code: under 250
        return n;
    };

    InputRig base(fw);
    REQUIRE(base.client.synced());
    REQUIRE(base.upload(heavyPolySine(3, stacks)));
    CHECK(int(base.client.state().voices[0]) == 3);
    const auto baseLoad = dspLoads(base, 0.05);
    INFO("four DSPs, instructions per frame: " << loadsText(baseLoad, 4));
    CHECK(heavyDsps(baseLoad) == 3);
    const auto baseChord = playChord(base, three);

    InputRig g2(fw, expandedG2());
    REQUIRE(g2.client.synced());
    REQUIRE(g2.upload(heavyPolySine(3, stacks)));
    CHECK(int(g2.client.state().voices[0]) == 3);
    const auto load3 = dspLoads(g2, 0.05);
    INFO("eight DSPs, three voices, instructions per frame: " << loadsText(load3, 8));
    CHECK(heavyDsps(load3) == 3);
    auto underruns = g2.m.stats().linkUnderruns;
    const auto chord3 = playChord(g2, three);
    CHECK(g2.m.stats().linkUnderruns == underruns);
    for(std::size_t k = 0; k < three.size(); ++k)
    {
        INFO("note " << three[k]);
        CHECK_THAT(baseChord.amplitude[k], Catch::Matchers::WithinRel(Full, 0.01));
        CHECK_THAT(chord3.amplitude[k], Catch::Matchers::WithinRel(baseChord.amplitude[k], 1e-4));
    }
    INFO("roughness: four DSPs " << baseChord.roughness << " (the tones': " << baseChord.smooth << "), eight " << chord3.roughness
                                 << " (" << chord3.smooth << ")");
    CHECK(baseChord.roughness < 2 * baseChord.smooth + 1e-4);
    CHECK(chord3.roughness < 2 * chord3.smooth + 1e-4);

    REQUIRE(g2.upload(heavyPolySine(7, stacks)));
    CHECK(int(g2.client.state().voices[0]) == 7);
    const auto load7 = dspLoads(g2, 0.05);
    INFO("eight DSPs, seven voices, instructions per frame: " << loadsText(load7, 8));
    CHECK(heavyDsps(load7) == 7);
    int onTheBoard = 0;
    for(std::size_t n = 4; n < 8; ++n) onTheBoard += load7[n] > 600;
    CHECK(onTheBoard >= 3);
    underruns = g2.m.stats().linkUnderruns;
    const auto chord7 = playChord(g2, seven);
    CHECK(g2.m.stats().linkUnderruns == underruns);
    for(std::size_t k = 0; k < seven.size(); ++k)
    {
        INFO("note " << seven[k]);
        CHECK_THAT(chord7.amplitude[k], Catch::Matchers::WithinRel(Full, 0.01));
    }
    INFO("seven voices: roughness " << chord7.roughness << " (the tones': " << chord7.smooth << ")");
    CHECK(chord7.roughness < 2 * chord7.smooth + 1e-4);
    CHECK(g2.m.stats().exceptions == 0);
    CHECK(g2.m.stats().dspsWild == 0);
}

TEST_CASE("The user's G2 OS with the expansion board: buses, the FX area and the audio inputs cross eight DSPs",
          "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    InputRig g2(fw, expandedG2());
    REQUIRE(g2.client.synced());
    // the bus once around the ring of eight, the FX send down the chain: as on four (the test above this section)
    REQUIRE(g2.upload(busAndFxPatch()));
    auto underruns = g2.m.stats().linkUnderruns;
    std::uint64_t clock0 = 0;
    auto o = g2.record(0.25, clock0);
    CHECK_THAT(tone(o, 0, 440.0, clock0).amplitude, Catch::Matchers::WithinRel(0.1247, 0.01));  // Out 1
    CHECK_THAT(tone(o, 1, 880.0, clock0).amplitude, Catch::Matchers::WithinRel(0.1247, 0.01));  // Out 3
    CHECK(tone(o, 0, 880.0, clock0).amplitude < 1e-4);
    CHECK(tone(o, 1, 440.0, clock0).amplitude < 1e-4);
    CHECK(g2.m.stats().linkUnderruns == underruns);

    // In 1/2 -> Out 1/2: the ADCs' words pass A6, the expansion board's four DSPs, A5 and A4 on their way to A3's DACs
    PatchBuilder two;
    const auto in2 = two.add("2-In", 0), out2 = two.add("2-Out", 1);
    two.cable(in2, 0, out2, 0);
    two.cable(in2, 1, out2, 1);
    REQUIRE(g2.upload(two.done()));
    constexpr int In1 = 0, In2 = 2;
    constexpr double Through = 0.4987;
    g2.source.hz[In1] = 1000;
    g2.source.hz[In2] = 2000;
    g2.source.amplitude[In1] = 0.125;
    g2.source.amplitude[In2] = 0.0625;
    g2.run(0.1);
    const auto irregular = g2.source.irregular;
    underruns = g2.m.stats().linkUnderruns;
    o = g2.record(0.25, clock0);
    CHECK(g2.source.irregular == irregular);
    CHECK(g2.m.stats().linkUnderruns == underruns);
    const auto left = tone(o, 0, 1000, clock0), right = tone(o, 2, 2000, clock0);
    CHECK_THAT(left.amplitude, Catch::Matchers::WithinRel(0.125 * Through, 0.01));
    CHECK_THAT(right.amplitude, Catch::Matchers::WithinRel(0.0625 * Through, 0.01));
    CHECK(residue(o, 0, 1000, clock0) < 1e-6);
    CHECK(residue(o, 2, 2000, clock0) < 1e-6);
    // Four more DSPs on the way, each 5.5 frames: a frame to take the buffer in, one to work on it, one to send it
    // (§3.7.1), and the emulator's line of 2 frames (the board's lines take none). 42.2 frames against the 20.4 of
    // four DSPs.
    const double delay = InputRig::delayOf(left.phase, 1000);
    INFO("ADCs to DACs: " << delay << " frames");
    CHECK(delay > 39);
    CHECK(delay < 45);
    CHECK_THAT(std::fmod(delay, 48.0), Catch::Matchers::WithinAbs(InputRig::delayOf(right.phase, 2000), 0.01));
    CHECK(g2.m.stats().exceptions == 0);
    CHECK(g2.m.stats().dspsWild == 0);
}

TEST_CASE("The user's G2 OS with DSP threads: a status read finds the DSP past what the host wrote before it",
          "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    // The DSPs take what the OS writes only in every 4th frame of theirs, as when their threads get little time: the
    // OS then reads their status while its writes are still on their way, thousands of times over a boot and an
    // upload (statusBehind). With Options::hostTakenOnRead the DSP takes them first (statusStale stays 0), the host
    // port stays in step, and the chord plays. Without it each of those reads saw a DSP behind the host, the state
    // in which the OS's interrupts save and restore the host ports wrongly (re/notes §3.9.6): whether that then
    // happens depends on the threads' timing (it did in some runs: a DSP read its host port without a word, strayed,
    // the upload failed), so only the reads are checked there.
    struct Case {
        const char* name;
        int threads;
        bool expanded, taken;
    };
    for(const Case c : {Case{"two threads", 2, false, true}, Case{"two threads, without the wait", 2, false, false},
                        Case{"eight DSPs on three threads", 3, true, true}})
    {
        INFO(c.name);
        Machine::Options o;
        o.threads = c.threads;
        o.hostLagFrames = 4;
        o.hostTakenOnRead = c.taken;
        o.expansion = c.expanded ? Expansion::With : Expansion::Without;
        InputRig g2(fw, o);
        if(!c.taken)
        {
            if(g2.client.synced()) g2.upload(heavyPolySine(3, 8));
            const auto s = g2.m.stats();
            INFO("out of step: " << s.dspEmptyReads << " empty reads by a DSP, " << s.dspsWild << " DSP(s) strayed");
            CHECK(s.statusBehind > 0);
            CHECK(s.statusStale == s.statusBehind);
            continue;
        }
        REQUIRE(g2.client.synced());
        REQUIRE(g2.upload(heavyPolySine(3, 8)));
        CHECK(int(g2.client.state().voices[0]) == 3);
        const auto chord = playChord(g2, {57, 64, 72});
        const auto s = g2.m.stats();
        CHECK(s.statusBehind > 0);
        CHECK(s.statusStale == 0);
        CHECK(s.dspEmptyReads == 0);
        CHECK(s.hostEmptyReads == 0);
        CHECK(s.dspsWild == 0);
        CHECK(s.exceptions == 0);
        for(const double a : chord.amplitude) CHECK_THAT(a, Catch::Matchers::WithinRel(0.1247, 0.01));
    }
}


TEST_CASE("The user's G2 OS: notes still sound after it has followed an external MIDI clock", "[g2emu][firmware]")
{
    Firmware fw;
    try
    {
        fw = Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    }
    // Every clock byte (F8) on MIDI IN has the OS's interrupt save and restore the host ports (re/notes §3.9.6).
    // Before a status read waited for the DSP to take what the OS had written, one of those saves went wrong within
    // a second of clock, on one thread as with DSP threads: a DSP read its host port without a word, and from then on
    // the notes were silent (found by the MIDI OUT work). One thread is deterministic: without the rule the fault
    // comes at the same place every time, which shows the test bites.
    struct Case {
        const char* name;
        int threads;
        bool taken;
    };
    for(const Case c : {Case{"one thread", 0, true}, Case{"one thread, without the rule", 0, false}, Case{"two DSP threads", 2, true}})
    {
        INFO(c.name);
        Machine::Options o;
        o.threads = c.threads;
        o.hostTakenOnRead = c.taken;
        InputRig g2(fw, o);
        REQUIRE(g2.client.synced());
        REQUIRE(g2.upload(keyboardSine()));
        auto peak = [&] {
            const std::uint8_t on[3] = {0x90, 69, 100}, off[3] = {0x80, 69, 64};
            g2.m.midiIn(on);
            g2.run(0.1);
            std::uint64_t clock0 = 0;
            const auto out = g2.record(0.2, clock0);
            g2.m.midiIn(off);
            g2.run(0.2);
            float p = 0;
            for(std::size_t f = 0; f < out.size() / 4; ++f) p = std::max(p, std::fabs(out[f * 4]));
            return p;
        };
        CHECK(peak() > 0.1f);
        // 96 clock bytes at 120 BPM (one every 2400 frames)
        const std::uint8_t tick[1] = {0xF8};
        const auto from = g2.m.frame();
        for(std::uint64_t i = 0; i < 96; ++i)
        {
            g2.m.midiInAt(tick, from + i * 2400);
            while(g2.m.frame() < from + (i + 1) * 2400) g2.step();
        }
        const float after = peak();
        const auto s = g2.m.stats();
        CHECK(s.statusBehind > 0);
        if(!c.taken)
        {
            CHECK(s.dspEmptyReads > 0);
            continue;
        }
        CHECK(after > 0.1f);
        g2.run(1.0);
        CHECK(peak() > 0.1f);
        CHECK(g2.m.stats().statusStale == 0);
        CHECK(g2.m.stats().dspEmptyReads == 0);
        CHECK(g2.m.stats().dspsWild == 0);
    }
}
