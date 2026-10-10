// The real-time G2 emulator (emu/g2emu, re/notes/g2-hardware-and-emulation.md §3.9). Everything here runs without
// the user's firmware, except the [firmware] case at the end, which boots the user's own OS when it is there
// (G2_FIRMWARE, or original/firmware) and is skipped otherwise.
#include "dsp.hpp"
#include "hw.hpp"
#include "panel.hpp"

#include "dsp56kEmu/assembler.h"

#include "g2/edit.hpp"
#include "g2/module_db.hpp"
#include "g2/patch.hpp"
#include "g2/proto/client.hpp"
#include "g2/uprate.hpp"
#include "g2emu/firmware.hpp"
#include "g2emu/machine.hpp"
#include "g2emu/runner.hpp"
#include "g2emu/transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
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

TEST_CASE("The user's G2 OS: MIDI Local switched on over USB (synth settings), then the panel's keys play",
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
    CHECK_FALSE(client.state().settings.localOn);  // an erased flash: Local Off

    client.sendPerformance(g2::Performance::playing(keyboardSine(), "Local"), "Local");
    auto settings = client.state().settings;
    settings.localOn = true;
    client.setSynthSettings(settings);
    while(!client.idle() && m.seconds() < 6) step();
    REQUIRE(client.idle());
    run(0.3);

    // the OS's own System menu says so too
    auto press = [&](PanelButton b) {
        m.panelButton(b, true);
        run(0.03);
        m.panelButton(b, false);
        run(0.05);
    };
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
