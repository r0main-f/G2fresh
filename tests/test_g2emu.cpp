// The real-time G2 emulator (emu/g2emu, re/notes/g2-hardware-and-emulation.md §3.9). Everything here runs without
// the user's firmware, except the [firmware] case at the end, which boots the user's own OS when it is there
// (G2_FIRMWARE, or original/firmware) and is skipped otherwise.
#include "dsp.hpp"
#include "hw.hpp"

#include "dsp56kEmu/assembler.h"

#include "g2/edit.hpp"
#include "g2/module_db.hpp"
#include "g2/patch.hpp"
#include "g2/proto/client.hpp"
#include "g2/uprate.hpp"
#include "g2emu/firmware.hpp"
#include "g2emu/machine.hpp"
#include "g2emu/transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
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
    m.midiIn(on);
    std::vector<float> out;
    m.run(Machine::FrameRate / 2, &out);
    REQUIRE(out.size() == Machine::FrameRate / 2 * 4);
    // outputs 1 and 2 are words 0 and 2 of each frame; find the onset and measure the pitch after it
    std::size_t onset = 0;
    while(onset < out.size() / 4 && std::fabs(out[onset * 4]) < 1e-3f) ++onset;
    CHECK(onset < Machine::FrameRate / 100);  // within 10 ms
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
