// The emulated G2's MIDI OUT and the rest of its MIDI IN (emu/g2emu, re/notes/g2-hardware-and-emulation.md §3.13):
// the UART's transmitter without the firmware, then what the user's own OS sends ([firmware] cases: G2_FIRMWARE, or
// original/firmware; skipped otherwise).
#include "hw.hpp"

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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace g2emu;
using Bytes = std::vector<std::uint8_t>;

namespace {

std::filesystem::path firmwarePath()
{
    if(const char* e = std::getenv("G2_FIRMWARE")) return e;
    return std::filesystem::path(G2_SOURCE_DIR) / "original" / "firmware";
}

std::optional<Firmware> firmware()
{
    try
    {
        return Firmware::load(firmwarePath());
    }
    catch(const std::exception&)
    {
        return std::nullopt;
    }
}

std::string hex(const Bytes& b)
{
    std::string s;
    char t[4];
    for(auto v : b)
    {
        std::snprintf(t, sizeof t, "%02x ", v);
        s += t;
    }
    return s;
}

// A machine with our client on its USB, and everything it sends on MIDI OUT.
struct Rig {
    Machine m;
    MachineTransport transport{m};
    g2::proto::ManualClock clock;
    g2::proto::Client client{transport, clock};
    std::vector<Machine::MidiOut> sent;
    bool take = true;            // step() takes the MIDI OUT bytes
    std::vector<float>* audio = nullptr;  // step() records the DACs' frames there

    explicit Rig(const Firmware& fw, Machine::Options o = {}) : m(fw, o)
    {
        m.run(Machine::FrameRate * 2);  // boot
        m.plugUsb();
        while(!client.synced() && m.seconds() < 5) step();
        REQUIRE(client.synced());
    }
    void step()
    {
        m.run(Machine::FrameRate / 1000, audio);
        clock.set(std::uint64_t(m.seconds() * 1000));
        client.tick();
        Machine::MidiOut b[64];
        while(take)
        {
            const auto n = m.takeMidiOut(b);
            if(!n) break;
            sent.insert(sent.end(), b, b + n);
        }
    }
    void run(double s)
    {
        const double end = m.seconds() + s;
        while(m.seconds() < end) step();
    }
    void idle()
    {
        const double end = m.seconds() + 10;
        while(!client.idle() && m.seconds() < end) step();
        REQUIRE(client.idle());
    }
    void press(PanelButton b)
    {
        m.panelButton(b, true);
        run(0.03);
        m.panelButton(b, false);
        run(0.05);
    }
    Bytes bytes() const
    {
        Bytes b;
        for(const auto& e : sent) b.push_back(e.byte);
        return b;
    }
    std::uint64_t now() const { return m.frame() * Machine::ClocksPerFrame; }  // on the clock of the bytes' times
    // A performance that plays `patch` in slot A from the keyboard.
    void load(const g2::Patch& patch, const std::string& name)
    {
        client.sendPerformance(g2::Performance::playing(patch, name), name);
        idle();
        run(0.3);
        sent.clear();
    }
    void settings(const std::function<void(g2::proto::SynthSettings&)>& change)
    {
        auto s = client.state().settings;
        change(s);
        client.setSynthSettings(s);
        idle();
        run(0.2);
        sent.clear();
    }
    std::string display(int d, int row) const { return m.panel().displays[std::size_t(d)].text(row); }
};

// One byte on a MIDI cable: 10 bits at 31250 bit/s, 320 us, in DSP clocks (96000 x 1536 a second) and in seconds.
constexpr double kByteClocks = 0.00032 * 96000 * 1536;  // 47185.92
constexpr double kClocksPerSecond = 96000.0 * 1536;
double seconds(std::uint64_t from, std::uint64_t to) { return (double(to) - double(from)) / kClocksPerSecond; }

// The line never carries bytes closer than one byte time; returns the largest gap in byte times.
double checkLineRate(const std::vector<Machine::MidiOut>& sent)
{
    double longest = 0;
    for(std::size_t i = 1; i < sent.size(); ++i)
    {
        const double gap = (double(sent[i].clock) - double(sent[i - 1].clock)) / kByteClocks;
        CHECK(gap > 0.9999);
        longest = std::max(longest, gap);
    }
    return longest;
}

g2::u8 addModule(g2::Patch& patch, const char* name, g2::u8 col, g2::u8 row = 0)
{
    for(const auto& m : g2::db::modules())
        if(m.kind == g2::db::ModuleKind::Module && std::string(m.shortName) == name)
            return g2::edit::addModule(patch, g2::Location::Va, m.typeId, col, row);
    throw std::runtime_error(name);
}

g2::u8 paramIndex(const g2::Patch& patch, g2::u8 module, const char* name)
{
    const auto* def = patch.va.find(module)->def();
    for(std::size_t i = 0; i < def->params.size(); ++i)
        if(std::string(def->params[i].name) == name) return g2::u8(i);
    throw std::runtime_error(name);
}

void setParam(g2::Patch& patch, g2::u8 module, const char* name, g2::u8 value)
{
    const auto i = paramIndex(patch, module, name);
    for(g2::u8 v = 0; v < g2::kFileVariations; ++v) g2::edit::setParam(patch, g2::Location::Va, module, i, v, value);
}

// Keyboard -> OscA (sine, keyboard tracking) -> EnvADSR (gate) -> 2-Out, knob 1 and MIDI CC 20 on the oscillator's
// Coarse: note 69 is 440 Hz.
g2::Patch keyboardSine()
{
    using namespace g2;
    Patch patch = Patch::makeDefault();
    const auto kbd = addModule(patch, "Keyboard", 0), osc = addModule(patch, "OscA", 1), env = addModule(patch, "EnvADSR", 2),
               out = addModule(patch, "2-Out", 3);
    setParam(patch, osc, "Wave", 0);
    setParam(patch, osc, "Coarse", 64);
    setParam(patch, env, "Attack", 0);
    setParam(patch, env, "Sustain", 127);
    edit::connect(patch, Location::Va, {osc, 0, true}, {env, 0, false});
    edit::connect(patch, Location::Va, {kbd, 1, true}, {env, 1, false});
    edit::connect(patch, Location::Va, {env, 1, true}, {out, 0, false});
    edit::connect(patch, Location::Va, {env, 1, true}, {out, 1, false});
    edit::assignKnob(patch, 0, Location::Va, osc, paramIndex(patch, osc, "Coarse"));
    edit::assignMidiCc(patch, 20, Location::Va, osc, paramIndex(patch, osc, "Coarse"));
    for(u8 v = 0; v < kFileVariations; ++v) edit::setSetting(patch, edit::Setting::Gain, 0, v, 127);
    uprate::update(patch, Location::Va);
    return patch;
}

// The System Exclusive blocks of a patch or performance dump [M p.133-135]: F0 33 <id> 0A <20 patch | 28
// performance> 00 <slot> 00 <block MSB LSB> <count MSB LSB> <name: 16> 00 <data> <checksum> F7, the checksum the low
// 7 bits of the sum of all bytes before it. Returns the number of blocks, 0 if `dump` is not a whole dump.
std::size_t dumpBlocks(const Bytes& dump, std::uint8_t message, std::string* name = nullptr)
{
    std::size_t at = 0, blocks = 0, count = 0;
    while(at < dump.size())
    {
        if(dump[at] != 0xF0) return 0;
        std::size_t end = at + 1;
        while(end < dump.size() && dump[end] != 0xF7)
        {
            if(dump[end] & 0x80) return 0;  // a status byte inside the block
            ++end;
        }
        if(end == dump.size() || end - at < 31) return 0;
        const auto* b = &dump[at];
        if(b[1] != 0x33 || b[3] != 0x0A || b[4] != message || b[5] != 0x00) return 0;
        const std::size_t id = std::size_t(b[8]) << 7 | b[9], n = std::size_t(b[10]) << 7 | b[11];
        if(id != blocks || (blocks && n != count)) return 0;
        count = n;
        unsigned sum = 0;
        for(std::size_t k = at; k + 1 < end; ++k) sum += dump[k];
        if((sum & 0x7f) != dump[end - 1]) return 0;
        if(name && blocks == 0)
        {
            name->assign(reinterpret_cast<const char*>(b + 12), 16);
            name->erase(std::min(name->find('\0'), name->size()));
        }
        ++blocks;
        at = end + 1;
    }
    return blocks == count ? blocks : 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// without the firmware

TEST_CASE("SIM: UART0's transmitter sends a byte in 10 bits at 31250 baud, one more waiting in its holding register",
          "[g2emu][midi]")
{
    std::uint64_t now = 1000;
    Sim sim([&] { return now; });
    sim.write(0x1C0 + 0x18, 1, 0);
    sim.write(0x1C0 + 0x1C, 1, 54);  // UBG = 54, as the OS sets it [C 0x30038004]
    const std::uint64_t perByte = 10 * 32 * 54;  // bus clocks: 320 us at 54 MHz
    auto usr = [&] { return sim.read(0x1C4, 1); };
    CHECK((usr() & 0x0C) == 0x0C);  // TxRDY and TxEMP
    sim.write(0x1CC, 1, 0x90);
    // the shifter took it at once: the holding register is free again, the transmitter is not empty
    CHECK((usr() & 0x0C) == 0x04);
    now += 100;
    sim.write(0x1CC, 1, 0x45);
    CHECK((usr() & 0x0C) == 0x00);  // one in the shifter, one waiting
    sim.write(0x1CC, 1, 0x7F);      // lost, as on the chip: the OS waits for TxRDY [C 0x30002C04]
    CHECK(sim.midiOutLost() == 1);
    Sim::MidiOutByte out[4];
    CHECK(sim.takeMidiOut(out, 4, now) == 0);  // nothing has left the line yet
    now = 1000 + perByte - 1;
    CHECK((usr() & 0x0C) == 0x00);
    now = 1000 + perByte;
    CHECK((usr() & 0x0C) == 0x04);  // the second byte moved into the shifter
    REQUIRE(sim.takeMidiOut(out, 4, now) == 1);
    CHECK(out[0].byte == 0x90);
    CHECK(out[0].done == 1000 + perByte);
    sim.write(0x1CC, 1, 0x66);
    now = 1000 + 3 * perByte - 1;
    CHECK((usr() & 0x0C) == 0x04);
    REQUIRE(sim.takeMidiOut(out, 4, now) == 1);
    CHECK(out[0].byte == 0x45);
    CHECK(out[0].done == 1000 + 2 * perByte);
    now = 1000 + 3 * perByte;
    CHECK((usr() & 0x0C) == 0x0C);
    REQUIRE(sim.takeMidiOut(out, 4, now) == 1);
    CHECK(out[0].byte == 0x66);
    CHECK(out[0].done == 1000 + 3 * perByte);
    // a byte written to an idle transmitter starts at once
    now += 12345;
    sim.write(0x1CC, 1, 0xF8);
    now += perByte;
    REQUIRE(sim.takeMidiOut(out, 4, now) == 1);
    CHECK(out[0].done == now);
    // the transmitter switched off (UCR): not ready, and what is written is lost
    sim.write(0x1C8, 1, 0x08);
    CHECK((usr() & 0x0C) == 0x00);
    sim.write(0x1CC, 1, 0x01);
    CHECK(sim.midiOutLost() == 2);
    sim.write(0x1C8, 1, 0x04);
    CHECK((usr() & 0x0C) == 0x0C);

    // nobody listens: the transmitter keeps the last 65536 bytes it sent and no more
    for(int i = 0; i < 70000; ++i)
    {
        sim.write(0x1CC, 1, std::uint8_t(i & 0x7f));
        now += perByte;
    }
    CHECK(sim.midiOutLost() == 2 + 70000 - 65536);
    const auto kept = sim.takeMidiOut();
    REQUIRE(kept.size() == 65536);
    CHECK(kept.back() == std::uint8_t(69999 & 0x7f));
    CHECK(kept.front() == std::uint8_t((70000 - 65536) & 0x7f));
}

TEST_CASE("SIM: a real-time byte goes between the bytes of a System Exclusive message on the MIDI IN line, behind anything else",
          "[g2emu][midi]")
{
    std::uint64_t now = 0;
    Sim sim([&] { return now; });
    sim.write(0x1C0 + 0x1C, 1, 54);
    const std::uint64_t perByte = 10 * 32 * 54;
    // what the receiver gets, byte by byte, with the bus clock at which each is complete
    std::vector<std::pair<std::uint8_t, std::uint64_t>> got;
    auto drain = [&](std::uint64_t until) {
        while(now < until)
        {
            now = std::min(until, sim.nextEvent());
            sim.advance(now);
            while(sim.read(0x1C4, 1) & 1) got.push_back({std::uint8_t(sim.read(0x1CC, 1)), now});
        }
    };
    auto order = [&] {
        Bytes b;
        for(const auto& g : got) b.push_back(g.first);
        return b;
    };
    // a message of 12 bytes from clock 0, and a clock tick due while its 5th byte is on the line: the tick follows
    // that byte, the message goes on after it
    const Bytes sysex = {0xF0, 0x33, 0x7F, 0x0A, 0x30, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0xF7};
    const std::uint8_t tick[1] = {0xF8}, note[3] = {0x90, 60, 100}, stop[1] = {0xFC};
    sim.midiInAt(sysex.data(), sysex.size(), 0);
    sim.midiInAt(tick, 1, 4 * perByte + 100);
    drain(20 * perByte);
    REQUIRE(got.size() == 13);
    CHECK(order() == Bytes{0xF0, 0x33, 0x7F, 0x0A, 0x30, 0xF8, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0xF7});
    CHECK(got[5].second == 6 * perByte);
    CHECK(got[12].second == 13 * perByte);

    // behind a note that waits behind a message, a real-time byte keeps its turn (Song Position then Continue, a
    // note then Stop: their order matters)
    got.clear();
    const std::uint64_t t0 = now;
    sim.midiInAt(sysex.data(), sysex.size(), t0);
    sim.midiInAt(note, 3, t0);
    sim.midiInAt(stop, 1, t0);
    drain(t0 + 20 * perByte);
    Bytes expected = sysex;
    expected.insert(expected.end(), {0x90, 60, 100, 0xFC});
    CHECK(order() == expected);
    // it never goes in front of the F0: due before the message starts but sent after it, it goes right after
    got.clear();
    const std::uint64_t t1 = now;
    sim.midiInAt(sysex.data(), sysex.size(), t1 + 1000);
    sim.midiInAt(tick, 1, t1);
    drain(t1 + 20 * perByte);
    REQUIRE(got.size() == 13);
    CHECK(got[0].first == 0xF0);
    CHECK(got[1].first == 0xF8);
    // two of them inside one message keep their order and their times (the first one moved the bytes behind it)
    got.clear();
    const std::uint64_t t2 = now;
    sim.midiInAt(sysex.data(), sysex.size(), t2);
    sim.midiInAt(tick, 1, t2 + 2 * perByte + 1);
    sim.midiInAt(stop, 1, t2 + 7 * perByte + 1);
    drain(t2 + 20 * perByte);
    CHECK(order() == Bytes{0xF0, 0x33, 0x7F, 0xF8, 0x0A, 0x30, 0x00, 0x00, 0xFC, 0x01, 0x02, 0x03, 0x04, 0xF7});
    // a message of three bytes is not a System Exclusive one: nothing goes between its bytes
    got.clear();
    const std::uint64_t t3 = now;
    sim.midiInAt(note, 3, t3);
    sim.midiInAt(tick, 1, t3 + perByte + 1);
    drain(t3 + 20 * perByte);
    CHECK(order() == Bytes{0x90, 60, 100, 0xF8});
    CHECK(sim.midiOverruns() == 0);
}

// ---------------------------------------------------------------------------------------------------------------
// with the user's OS

TEST_CASE("The user's G2 OS sends the panel's keys, wheels, pedal and knobs on MIDI OUT, at the cable's rate", "[g2emu][firmware][midi]")
{
    const auto fw = firmware();
    if(!fw) SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    Machine::Options o;
    o.model = PanelModel::G2X;  // 61 keys from C1 (MIDI 36), the two global wheels
    Rig r(*fw, o);
    // a new G2: MIDI Local On, slot A on channel 1, controllers Send & Receive (the OS's own settings)
    REQUIRE(r.client.state().settings.localOn);
    REQUIRE(r.client.state().settings.midiChannels[0] == 0);
    REQUIRE((r.client.state().settings.controllerMode & 1) == 1);
    CHECK(r.bytes().empty());  // nothing at power-on: no Active Sensing, no clock (MIDI Clk Send is Off)
    r.load(keyboardSine(), "Midi Test");

    // A key: key 33 of the G2X is A (MIDI 69). Note On with its velocity, three bytes one byte time apart, a few
    // milliseconds after the key went down; the sound starts with it (Local On)
    std::vector<float> audio;
    r.audio = &audio;
    const auto down = r.now();
    r.m.panelKey(33, true, 100);
    r.run(0.1);
    r.audio = nullptr;
    const auto audioEnd = r.m.dacClock();
    REQUIRE(r.sent.size() == 3);
    CHECK(r.sent[0].byte == 0x90);
    CHECK(r.sent[1].byte == 69);
    CHECK(std::abs(int(r.sent[2].byte) - 100) <= 4);  // the velocity the OS made of the contacts' time
    CHECK_THAT(double(r.sent[1].clock - r.sent[0].clock), Catch::Matchers::WithinAbs(kByteClocks, 1.0));
    CHECK_THAT(double(r.sent[2].clock - r.sent[1].clock), Catch::Matchers::WithinAbs(kByteClocks, 1.0));
    const double noteOnMs = seconds(down, r.sent[2].clock) * 1e3;
    INFO("Note On complete " << noteOnMs << " ms after the key went down");
    CHECK(noteOnMs > 0.96);  // its three bytes take that long
    CHECK(noteOnMs < 10.0);
    // the sound: the first DAC frame that is not silence, on the bytes' clock (the last frame recorded left the DACs
    // at dacClock(), the others a frame apart before it)
    std::size_t onset = 0;
    while(onset < audio.size() / 4 && std::fabs(audio[onset * 4]) < 1e-3f) ++onset;
    REQUIRE(onset < audio.size() / 4);
    const auto onsetClock = audioEnd - (audio.size() / 4 - 1 - onset) * Machine::ClocksPerFrame;
    const double soundAfterMidiMs = (double(onsetClock) - double(r.sent[2].clock)) / kClocksPerSecond * 1e3;
    INFO("the sound starts " << soundAfterMidiMs << " ms after the Note On is complete on MIDI OUT");
    CHECK(std::abs(soundAfterMidiMs) < 5.0);
    r.sent.clear();
    r.m.panelKey(33, false);
    r.run(0.1);
    REQUIRE(r.sent.size() == 3);
    CHECK(r.sent[0].byte == 0x80);  // Note Off, with a release velocity
    CHECK(r.sent[1].byte == 69);
    CHECK(r.sent[2].byte > 0);
    r.sent.clear();

    // the wheels, the stick, aftertouch and the sustain pedal [M p.126-127]
    auto moved = [&](const char* what, const Bytes& expected) {
        r.run(0.1);
        INFO(what << ": " << hex(r.bytes()));
        const auto b = r.bytes();
        // the last message is the control's end position (the OS may send values on the way)
        REQUIRE(b.size() >= expected.size());
        CHECK(Bytes(b.end() - std::ptrdiff_t(expected.size()), b.end()) == expected);
        checkLineRate(r.sent);
        r.sent.clear();
    };
    r.m.panelAnalog(PanelAnalog::ModWheel, 1.0f);
    moved("mod wheel", {0xB0, 1, 127});
    r.m.panelAnalog(PanelAnalog::PitchStick, 1.0f);
    moved("pitch stick", {0xE0, 0x7F, 0x7F});
    r.m.panelAnalog(PanelAnalog::GlobalWheel1, 1.0f);
    moved("global wheel 1", {0xB0, 96, 127});
    r.m.panelAnalog(PanelAnalog::Aftertouch, 1.0f);
    moved("aftertouch", {0xD0, 127});
    r.m.panelSustainPedal(true);
    moved("sustain pedal down", {0xB0, 64, 127});
    r.m.panelSustainPedal(false);
    moved("sustain pedal up", {0xB0, 64, 0});

    // a knob whose parameter has a MIDI controller: page A1's knob 1 is the oscillator's Coarse, CC 20
    r.press(PanelButton::PageA);
    r.sent.clear();
    r.m.panelEncoder(PanelEncoder::Knob1, 3);
    moved("knob 1", {0xB0, 20, 67});
    // a variation button: CC 70 [M p.127]
    r.press(PanelButton::Variation2);
    {
        const auto b = r.bytes();
        INFO("variation 2: " << hex(b));
        REQUIRE(b.size() == 3);
        CHECK(b[0] == 0xB0);
        CHECK(b[1] == 70);
    }
    r.sent.clear();

    // MIDI Ctrl set to receive only: the wheel is not sent any more; MIDI Local Off: the key still is, and is not played
    r.settings([](g2::proto::SynthSettings& s) { s.controllerMode = 2; });
    r.m.panelAnalog(PanelAnalog::ModWheel, 0.0f);
    r.run(0.1);
    CHECK(r.bytes().empty());
    r.settings([](g2::proto::SynthSettings& s) { s.localOn = false; });
    audio.clear();
    r.audio = &audio;
    r.m.panelKey(33, true, 100);
    r.run(0.1);
    r.audio = nullptr;
    REQUIRE(r.sent.size() == 3);
    CHECK(r.sent[0].byte == 0x90);
    float peak = 0;
    for(std::size_t f = 0; f < audio.size() / 4; ++f) peak = std::max(peak, std::fabs(audio[f * 4]));
    CHECK(peak < 1e-3f);
    CHECK(r.m.stats().midiOutLost == 0);
    CHECK(r.m.stats().exceptions == 0);
}

TEST_CASE("The user's G2 OS sends MIDI clock at the master clock's tempo when MIDI Clk Send is On, and Start and Stop",
          "[g2emu][firmware][midi]")
{
    const auto fw = firmware();
    if(!fw) SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    Rig r(*fw);
    r.load(keyboardSine(), "Clock Test");
    r.run(1.0);
    CHECK(r.bytes().empty());  // MIDI Clk Send is Off on a new G2 [M p.31]
    r.settings([](g2::proto::SynthSettings& s) { s.sendClock = true; });
    REQUIRE(r.display(1, 0).find("120 BPM") != std::string::npos);  // the Master Clock, above knob 1

    // 24 clocks per quarter note: 48 a second at 120 BPM, evenly spaced, whether the clock runs or not
    auto rate = [&](double over) {
        r.sent.clear();
        r.run(over);
        std::vector<std::uint64_t> ticks;
        for(const auto& e : r.sent)
        {
            CHECK(e.byte == 0xF8);
            ticks.push_back(e.clock);
        }
        REQUIRE(ticks.size() > 10);
        double shortest = 1e9, longest = 0;
        for(std::size_t i = 1; i < ticks.size(); ++i)
        {
            shortest = std::min(shortest, seconds(ticks[i - 1], ticks[i]));
            longest = std::max(longest, seconds(ticks[i - 1], ticks[i]));
        }
        const double hz = double(ticks.size() - 1) / seconds(ticks.front(), ticks.back());
        INFO(hz << " clocks a second, " << shortest * 1e3 << " to " << longest * 1e3 << " ms apart");
        CHECK(longest - shortest < 0.0005);  // the OS sends them from its tick (8272 a second)
        return hz;
    };
    const double at120 = rate(4.0);
    INFO("120 BPM: " << at120 << " clocks a second (48 nominal)");
    CHECK_THAT(at120, Catch::Matchers::WithinRel(48.0, 0.02));

    // Run: Start, and controller 80 = 127 on the Global channel (16 on a new G2); Stop: Stop and controller 80 = 0
    auto without = [&](std::uint8_t byte) {
        Bytes b;
        for(auto v : r.bytes())
            if(v != byte) b.push_back(v);
        return b;
    };
    r.sent.clear();
    r.press(PanelButton::Button1);  // the button under the Master Clock: Run / Stop
    r.run(0.2);
    CHECK(r.display(1, 1).find("Run") != std::string::npos);
    INFO("Run: " << hex(without(0xF8)));
    CHECK(without(0xF8) == Bytes{0xFA, 0xBF, 80, 127});
    r.sent.clear();
    r.press(PanelButton::Button1);
    r.run(0.2);
    CHECK(r.display(1, 1).find("Stop") != std::string::npos);
    CHECK(without(0xF8) == Bytes{0xFC, 0xBF, 80, 0});

    // knob 1 turns the tempo down by 30 BPM or so (the OS drops a knob's first step in a new direction): 24 clocks
    // per quarter note at the tempo its display shows
    r.m.panelEncoder(PanelEncoder::Knob1, -30);
    r.run(1.0);
    INFO("master clock: '" << r.display(1, 0) << "' / '" << r.display(1, 1) << "'");
    REQUIRE(r.display(1, 0).find(" BPM") == 3);
    const int bpm = std::atoi(r.display(1, 0).c_str());
    REQUIRE(bpm >= 85);
    REQUIRE(bpm <= 95);
    const double slower = rate(4.0);
    INFO(bpm << " BPM: " << slower << " clocks a second (" << bpm * 0.4 << " nominal)");
    CHECK_THAT(slower, Catch::Matchers::WithinRel(bpm * 0.4, 0.02));
    CHECK(r.m.stats().exceptions == 0);
}

TEST_CASE("The user's G2 OS: the MIDI modules of a patch send a note, a controller and a program change", "[g2emu][firmware][midi]")
{
    const auto fw = firmware();
    if(!fw) SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    Rig r(*fw);
    // The Keyboard's gate into NoteSend (note 72, velocity 90, channel 2), CtrlSend (controller 21 = 99, channel 3)
    // and PCSend (program 6, channel 4): a note on MIDI IN (channel 1, slot A) sets them off. MIDI IN itself is not
    // passed on to MIDI OUT.
    using namespace g2;
    Patch patch = Patch::makeDefault();
    const auto kbd = addModule(patch, "Keyboard", 0), note = addModule(patch, "NoteSend", 1), ctrl = addModule(patch, "CtrlSend", 2),
               prog = addModule(patch, "PCSend", 3);
    setParam(patch, note, "Note", 72);
    setParam(patch, note, "Vel", 90);
    setParam(patch, note, "Channel", 1);
    setParam(patch, ctrl, "Control", 21);
    setParam(patch, ctrl, "Value", 99);
    setParam(patch, ctrl, "Channel", 2);
    setParam(patch, prog, "Program", 5);
    setParam(patch, prog, "Channel", 3);
    for(const auto to : {note, ctrl, prog}) edit::connect(patch, Location::Va, {kbd, 1, true}, {to, 0, false});
    uprate::update(patch, Location::Va);
    r.load(patch, "Modules");

    const std::uint8_t on[3] = {0x90, 60, 100}, off[3] = {0x80, 60, 64};
    const auto t0 = r.now();
    r.m.midiInAt(on, r.m.frame());
    r.run(0.1);
    INFO("note on in: " << hex(r.bytes()));
    CHECK(r.bytes() == Bytes{0x91, 72, 90, 0xB2, 21, 99, 0xC3, 5});
    REQUIRE(!r.sent.empty());
    const double ms = seconds(t0, r.sent.front().clock) * 1e3;
    INFO("NoteSend's first byte is out " << ms << " ms after the note's first byte went in");
    CHECK(ms > 0.96 + 0.32);  // the note's three bytes in, one byte out
    CHECK(ms < 10.0);
    checkLineRate(r.sent);
    r.sent.clear();
    r.m.midiInAt(off, r.m.frame());
    r.run(0.1);
    INFO("note off in: " << hex(r.bytes()));
    const auto b = r.bytes();
    REQUIRE(b.size() == 3);  // the gate closes: NoteSend ends its note; the other two send on a rising edge only
    CHECK(b[0] == 0x81);
    CHECK(b[1] == 72);
    CHECK(r.m.stats().exceptions == 0);
}

TEST_CASE("The user's G2 OS: Dump One sends the patch as System Exclusive at the cable's rate; sent back in, it loads; the "
          "requests are answered; Active Sensing is ignored",
          "[g2emu][firmware][midi]")
{
    const auto fw = firmware();
    if(!fw) SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    Rig r(*fw);
    r.load(keyboardSine(), "Dump Test");

    // Shift + System [M p.29]
    const auto t0 = r.now();
    r.m.panelButton(PanelButton::Shift, true);
    r.run(0.03);
    r.press(PanelButton::System);
    r.m.panelButton(PanelButton::Shift, false);
    r.run(1.0);
    const auto dump = r.bytes();
    REQUIRE(!dump.empty());
    std::string name;
    const auto blocks = dumpBlocks(dump, 0x20, &name);
    INFO("Dump One: " << dump.size() << " bytes in " << blocks << " blocks, from " << seconds(t0, r.sent.front().clock) * 1e3
                      << " ms after the button, " << seconds(r.sent.front().clock, r.sent.back().clock) * 1e3 << " ms on the line");
    REQUIRE(blocks > 0);  // whole blocks, numbered, each with its checksum
    CHECK(name == "Dump Test");
    CHECK(dump[2] == 0x7F);  // SysEx ID All, a new G2's
    CHECK(dump.size() > 1000);
    // the line never rests during the dump and carries 3125 bytes a second: "about one second to transfer 3 kB"
    // [M p.29]
    CHECK_THAT(checkLineRate(r.sent), Catch::Matchers::WithinAbs(1.0, 0.001));
    CHECK_THAT(seconds(r.sent.front().clock, r.sent.back().clock), Catch::Matchers::WithinRel(double(dump.size() - 1) * 0.00032, 1e-4));
    r.sent.clear();

    // the same dump on request (patch request by slot: slot A) [M p.131]
    const std::uint8_t request[] = {0xF0, 0x33, 0x7F, 0x0A, 0x30, 0x00, 0x00, 0xF7};
    r.m.midiIn(request);
    r.run(1.0);
    CHECK(r.bytes() == dump);
    r.sent.clear();

    // another patch in the slot, then the dump on MIDI IN: the patch is back
    r.load(g2::Patch::makeDefault(), "Other");
    REQUIRE(r.display(0, 1).rfind("Other", 0) == 0);
    r.m.midiIn(dump);
    r.run(1.5);
    INFO("main display: '" << r.display(0, 0) << "' / '" << r.display(0, 1) << "'");
    CHECK(r.display(0, 1).rfind("Dump Test", 0) == 0);
    CHECK(r.m.stats().midiOverruns == 0);
    CHECK(r.bytes().empty());  // a dump is not answered
    // and it plays: note 69 on MIDI IN is 440 Hz
    {
        const std::uint8_t on[3] = {0x90, 69, 100};
        r.m.midiIn(on);
        std::vector<float> audio;
        r.run(0.1);
        r.audio = &audio;
        r.run(0.25);
        r.audio = nullptr;
        std::size_t crossings = 0;
        double first = -1, last = -1;
        for(std::size_t f = 0; f + 1 < audio.size() / 4; ++f)
        {
            const double a = audio[f * 4], b = audio[(f + 1) * 4];
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
        r.sent.clear();
    }

    // the other requests [M p.132-133]: the performance, and all controllers of slot A (as Dump CC sends them)
    const std::uint8_t perfRequest[] = {0xF0, 0x33, 0x7F, 0x0A, 0x38, 0x00, 0x00, 0xF7};
    r.m.midiIn(perfRequest);
    r.run(2.5);
    INFO("performance dump on request: " << r.sent.size() << " bytes");
    CHECK(dumpBlocks(r.bytes(), 0x28) > 0);
    CHECK_THAT(checkLineRate(r.sent), Catch::Matchers::WithinAbs(1.0, 0.001));
    r.sent.clear();
    const std::uint8_t ccRequest[] = {0xF0, 0x33, 0x7F, 0x0A, 0x40, 0x00, 0xF7};
    r.m.midiIn(ccRequest);
    r.run(0.3);
    {
        const auto b = r.bytes();
        INFO("all controllers: " << hex(b));
        REQUIRE(b.size() >= 6);
        REQUIRE(b.size() % 3 == 0);
        bool volume = false, coarse = false;
        for(std::size_t i = 0; i < b.size(); i += 3)
        {
            CHECK(b[i] == 0xB0);
            volume = volume || b[i + 1] == 7;
            coarse = coarse || (b[i + 1] == 20 && b[i + 2] == 64);  // the patch's own controller, on Coarse
        }
        CHECK(volume);
        CHECK(coarse);
        r.sent.clear();
    }
    // another maker's message is ignored, and so are Active Sensing and System Reset [C 0x30011F5E: of the real-time
    // bytes the OS takes F8, FA, FB and FC]
    const std::uint8_t foreign[] = {0xF0, 0x43, 0x7F, 0x0A, 0x30, 0x00, 0x00, 0xF7}, sensing[] = {0xFE, 0xFE, 0xFF};
    r.m.midiIn(foreign);
    r.m.midiIn(sensing);
    r.run(0.5);
    CHECK(r.bytes().empty());
    CHECK(r.display(0, 1).rfind("Dump Test", 0) == 0);
    CHECK(r.m.stats().exceptions == 0);
}

TEST_CASE("The user's G2 OS: MIDI OUT that nobody takes stays bounded (the last bytes are kept)", "[g2emu][firmware][midi]")
{
    const auto fw = firmware();
    if(!fw) SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    Machine::Options o;
    o.midiOutKept = 256;
    Rig r(*fw, o);
    r.take = false;
    r.settings([](g2::proto::SynthSettings& s) { s.sendClock = true; });
    r.run(10.0);  // 48 clocks a second
    const auto stats = r.m.stats();
    CHECK(stats.midiOutBytes > 450);
    std::vector<Machine::MidiOut> kept(1024);
    kept.resize(r.m.takeMidiOut(kept));
    CHECK(kept.size() == 256);
    CHECK(stats.midiOutLost + 256 == stats.midiOutBytes);
    // the last ones, in order: the newest was sent just now
    for(std::size_t i = 1; i < kept.size(); ++i) CHECK(kept[i].clock > kept[i - 1].clock);
    CHECK(seconds(kept.back().clock, r.now()) < 0.03);
    CHECK(seconds(kept.front().clock, r.now()) < 256 / 48.0 * 1.05);
    CHECK(r.m.takeMidiOut(kept) == 0);
}

TEST_CASE("The user's G2 OS in real time: MIDI OUT reaches the audio thread with the frames it belongs to (Runner::midiOut)",
          "[g2emu][firmware][midi]")
{
    const auto fw = firmware();
    if(!fw) SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    Runner::Options o;
    o.machine.model = PanelModel::G2X;
    Runner runner(*fw, o);
    auto link = emulatedG2Link(runner.machine());
    // As the audio thread does: a block of 96 frames every millisecond, then the MIDI OUT bytes of that block, each
    // at its frame on the reader's timeline
    struct Event {
        std::uint64_t frame;
        std::uint8_t byte;
    };
    std::vector<Event> events;
    std::vector<float> heard, block(4 * 96);
    std::uint64_t read = 0;
    auto step = [&](bool keep) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while(runner.available() >= 96)
        {
            runner.read(block.data(), 96);
            Runner::MidiOut out[16];
            std::size_t n;
            while((n = runner.midiOut(out)) > 0)
                for(std::size_t k = 0; k < n; ++k)
                {
                    CHECK(out[k].offset < 96);
                    events.push_back({read + out[k].offset, out[k].byte});
                }
            if(keep) heard.insert(heard.end(), block.begin(), block.end());
            read += 96;
        }
        link->tick();
    };
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto within = [&](double s) { return std::chrono::duration<double>(Clock::now() - start).count() < s; };
    while(!link->synced() && within(30)) step(false);
    REQUIRE(link->synced());
    link->sendPerformance(g2::Performance::playing(keyboardSine(), "Sine"), "Sine");
    while(!link->client().idle() && within(40)) step(false);
    REQUIRE(link->client().idle());
    for(int i = 0; i < 1000; ++i) step(false);
    events.clear();
    const auto late0 = runner.stats().midiOutLate;
    const auto missing0 = runner.stats().framesMissing;

    const auto from = read;
    runner.panelKey(33, true, 100);
    while(read < from + Machine::FrameRate / 4 && within(80)) step(true);
    runner.panelKey(33, false);
    for(int i = 0; i < 200; ++i) step(false);
    const auto stats = runner.stats();
    INFO("frames the reader missed: " << stats.framesMissing - missing0 << ", MIDI OUT bytes late: " << stats.midiOutLate - late0);
    REQUIRE(events.size() == 6);
    CHECK(events[0].byte == 0x90);
    CHECK(events[1].byte == 69);
    CHECK(events[3].byte == 0x80);
    if(stats.framesMissing == missing0)
    {
        // the machine kept up (it may not on a busy computer): no byte came late, the Note On's three bytes are a
        // byte time apart (30.72 frames), and the sound starts with it as on the machine's own clock (the test
        // above: within 5 ms)
        CHECK(stats.midiOutLate == late0);
        CHECK(std::abs(int(events[1].frame - events[0].frame) - 31) <= 1);
        CHECK(std::abs(int(events[2].frame - events[1].frame) - 31) <= 1);
        std::size_t onset = 0;
        while(onset < heard.size() / 4 && std::fabs(heard[onset * 4]) < 1e-3f) ++onset;
        REQUIRE(onset < heard.size() / 4);
        const double ms = (double(from + onset) - double(events[2].frame)) / Machine::FrameRate * 1e3;
        INFO("the sound starts " << ms << " ms after the Note On's last byte, on the reader's timeline");
        CHECK(std::abs(ms) < 5.0);
    }
    else
        WARN("the machine did not keep up with real time: MIDI OUT timing not checked");
    runner.stop();
    CHECK(runner.machine().stats().exceptions == 0);
}

// Found while testing MIDI clock through a dump: once the OS had followed an external MIDI clock, new notes were
// silent (a note held from before went on sounding), and the next patch load could send a DSP into P memory without
// code, after which the OS hung; with one thread or two, G2 or G2X, with or without a MIDI Start. A G2 slaved to a
// sequencer's clock plays. The cause was the host ports: a status read before the DSP had taken what the OS wrote
// (re/notes §3.9.6; the fix's own test, with the rule off, is in test_g2emu.cpp). This one fails on f7c5667 and
// adds what comes after the clock: the OS's own clock again, then a patch load.
TEST_CASE("The user's G2 OS: notes still sound after it followed an external MIDI clock, and a patch loads after it",
          "[g2emu][firmware]")
{
    const auto fw = firmware();
    if(!fw) SKIP("no G2 firmware (set G2_FIRMWARE to the updater's .rsrc, or unpack it into original/firmware)");
    Machine::Options o;
    if(const char* e = std::getenv("G2TEST_THREADS")) o.threads = std::atoi(e);
    if(std::getenv("G2TEST_NO_POLL_SKIP")) o.pollSkip = false;
    if(std::getenv("G2TEST_NO_IDLE_SKIP")) o.idleSkip = false;
    Rig r(*fw, o);
    r.load(keyboardSine(), "Clock");
    auto peak = [&] {
        const std::uint8_t on[3] = {0x90, 69, 100}, off[3] = {0x80, 69, 64};
        std::vector<float> audio;
        r.m.midiIn(on);
        r.run(0.1);
        r.audio = &audio;
        r.run(0.2);
        r.audio = nullptr;
        r.m.midiIn(off);
        r.run(0.2);
        float p = 0;
        for(std::size_t f = 0; f < audio.size() / 4; ++f) p = std::max(p, std::fabs(audio[f * 4]));
        return p;
    };
    CHECK(peak() > 0.1f);
    // 4 quarter notes of clock at 100 BPM
    const std::uint8_t tick[1] = {0xF8};
    const auto from = r.m.frame();
    for(int i = 0; i < 96; ++i)
    {
        r.m.midiInAt(tick, from + std::uint64_t(i) * 2400);
        while(r.m.frame() < from + std::uint64_t(i + 1) * 2400) r.step();
    }
    CHECK(r.display(1, 0).find("Ext") != std::string::npos);
    CHECK(peak() > 0.1f);  // 0 on f7c5667
    r.run(3.0);
    CHECK(peak() > 0.1f);  // also once the OS is back on its own clock
    r.client.sendPerformance(g2::Performance::playing(keyboardSine(), "Again"), "Again");
    r.run(2.0);
    CHECK(r.m.stats().dspsWild == 0);  // 1 in some runs on f7c5667
    CHECK(r.display(0, 1).rfind("Again", 0) == 0);
}
