// MidiOutAssembler (plugin/Source): the emulated G2's MIDI OUT bytes put together into the messages a host takes.
#include "MidiOutAssembler.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
using Message = std::pair<int, Bytes>; // its time, its bytes

// The messages of `bytes`, one byte per time unit from `from`.
std::vector<Message> feed(MidiOutAssembler& a, const Bytes& bytes, int from = 0)
{
    std::vector<Message> out;
    int at = from;
    for (const auto b : bytes)
        a.push(b, at++, [&](int t, const std::uint8_t* data, std::size_t size) { out.push_back({t, Bytes(data, data + size)}); });
    return out;
}

} // namespace

TEST_CASE("MidiOutAssembler: channel messages, each at the time of its last byte", "[midiout]")
{
    MidiOutAssembler a;
    // as the G2 sends a key: Note On and Note Off, a status byte each [emulated: tests/test_midi_out.cpp]
    const auto m = feed(a, {0x90, 0x45, 0x66, 0x80, 0x45, 0x72, 0xB0, 0x01, 0x7F, 0xE0, 0x7F, 0x7F, 0xC3, 0x05, 0xD0, 0x7F}, 100);
    REQUIRE(m.size() == 6);
    CHECK(m[0] == Message{102, {0x90, 0x45, 0x66}});
    CHECK(m[1] == Message{105, {0x80, 0x45, 0x72}});
    CHECK(m[2] == Message{108, {0xB0, 0x01, 0x7F}});
    CHECK(m[3] == Message{111, {0xE0, 0x7F, 0x7F}});
    CHECK(m[4] == Message{113, {0xC3, 0x05}}); // Program Change: one data byte
    CHECK(m[5] == Message{115, {0xD0, 0x7F}}); // Channel Pressure
}

TEST_CASE("MidiOutAssembler: running status, ended by a System Common message", "[midiout]")
{
    MidiOutAssembler a;
    auto m = feed(a, {0x91, 60, 100, 62, 101, 64, 0});
    REQUIRE(m.size() == 3);
    CHECK(m[0] == Message{2, {0x91, 60, 100}});
    CHECK(m[1] == Message{4, {0x91, 62, 101}});
    CHECK(m[2] == Message{6, {0x91, 64, 0}});
    // one-byte running status
    m = feed(a, {0xC2, 1, 2, 3});
    REQUIRE(m.size() == 3);
    CHECK(m[2] == Message{3, {0xC2, 3}});
    // Song Position (three bytes), Song Select (two), Tune Request (one); the data bytes after them have no status
    // any more
    m = feed(a, {0xF2, 0x10, 0x02, 0x40, 0x41, 0xF3, 0x05, 0x07, 0xF6, 0x33});
    REQUIRE(m.size() == 3);
    CHECK(m[0] == Message{2, {0xF2, 0x10, 0x02}});
    CHECK(m[1] == Message{6, {0xF3, 0x05}});
    CHECK(m[2] == Message{8, {0xF6}});
    // data bytes before any status, a lone F7, the undefined F4 and F5: nothing
    MidiOutAssembler b;
    CHECK(feed(b, {0x40, 0x41, 0xF7, 0xF4, 0x01, 0xF5, 0x02}).empty());
    // a message cut short by another status byte is dropped, the new one taken
    m = feed(b, {0x90, 60, 0xB0, 7, 100});
    REQUIRE(m.size() == 1);
    CHECK(m[0] == Message{4, {0xB0, 7, 100}});
}

TEST_CASE("MidiOutAssembler: real-time bytes go out at once, between the bytes of any message", "[midiout]")
{
    MidiOutAssembler a;
    // a clock tick inside a Note On, Start after its status byte; running status goes on behind them
    const auto m = feed(a, {0x90, 0xFA, 60, 0xF8, 100, 62, 0xFC, 90});
    REQUIRE(m.size() == 5);
    CHECK(m[0] == Message{1, {0xFA}});
    CHECK(m[1] == Message{3, {0xF8}});
    CHECK(m[2] == Message{4, {0x90, 60, 100}});
    CHECK(m[3] == Message{6, {0xFC}});
    CHECK(m[4] == Message{7, {0x90, 62, 90}});
    // every real-time byte is passed on, also the ones the G2 does not send (Active Sensing, System Reset)
    const auto r = feed(a, {0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF});
    CHECK(r.size() == 8);
}

TEST_CASE("MidiOutAssembler: a System Exclusive message across blocks, with real-time bytes inside", "[midiout]")
{
    MidiOutAssembler a;
    // a block of a G2 patch dump, as it comes over several audio blocks (each feed() is one, with its own times)
    Bytes block = {0xF0, 0x33, 0x7F, 0x0A, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
    for (int i = 0; i < 300; ++i)
        block.push_back(static_cast<std::uint8_t>(i & 0x7F));
    block.push_back(0xF7);
    std::vector<Message> all;
    std::size_t at = 0;
    int blocks = 0;
    while (at < block.size()) {
        // 100 bytes per audio block, a clock tick in the middle of each
        Bytes part(block.begin() + static_cast<std::ptrdiff_t>(at), block.begin() + static_cast<std::ptrdiff_t>(std::min(block.size(), at + 100)));
        part.insert(part.begin() + static_cast<std::ptrdiff_t>(part.size() / 2), 0xF8);
        const auto m = feed(a, part);
        all.insert(all.end(), m.begin(), m.end());
        at += 100;
        ++blocks;
    }
    REQUIRE(blocks == 4);
    // four ticks, each at its place in its block, and the whole message once, at the time of its F7 in the last one
    REQUIRE(all.size() == 5);
    for (int i = 0; i < 4; ++i)
        CHECK(all[static_cast<std::size_t>(i)].second == Bytes{0xF8});
    CHECK(all[0].first == 50);
    CHECK(all[4].second == block);
    CHECK(all[4].first == 13); // 13 bytes in the last block and the tick before the F7
    CHECK(a.dropped() == 0);

    // two messages back to back, then a channel message
    const auto m = feed(a, {0xF0, 0x33, 0x01, 0xF7, 0xF0, 0x33, 0x02, 0xF7, 0x90, 60, 100});
    REQUIRE(m.size() == 3);
    CHECK(m[0] == Message{3, {0xF0, 0x33, 0x01, 0xF7}});
    CHECK(m[1] == Message{7, {0xF0, 0x33, 0x02, 0xF7}});
    CHECK(m[2] == Message{10, {0x90, 60, 100}});
}

TEST_CASE("MidiOutAssembler: a System Exclusive message cut short or too long is dropped, not passed on in part", "[midiout]")
{
    MidiOutAssembler a(16);
    // cut short by a Note On: the note is taken, the message is not
    auto m = feed(a, {0xF0, 0x33, 0x01, 0x02, 0x90, 60, 100});
    REQUIRE(m.size() == 1);
    CHECK(m[0] == Message{6, {0x90, 60, 100}});
    CHECK(a.dropped() == 1);
    // one that fits exactly (16 bytes with F0 and F7), one byte more does not
    Bytes fits = {0xF0};
    for (int i = 0; i < 14; ++i)
        fits.push_back(static_cast<std::uint8_t>(i));
    fits.push_back(0xF7);
    m = feed(a, fits);
    REQUIRE(m.size() == 1);
    CHECK(m[0].second == fits);
    Bytes longer = fits;
    longer.insert(longer.end() - 1, 0x55);
    CHECK(feed(a, longer).empty());
    CHECK(a.dropped() == 2);
    // and the next one is taken again
    m = feed(a, {0xF0, 0x01, 0xF7});
    REQUIRE(m.size() == 1);
    CHECK(m[0].second == Bytes{0xF0, 0x01, 0xF7});
    // reset() forgets a message on its way
    feed(a, {0xF0, 0x01, 0x02});
    a.reset();
    CHECK(feed(a, {0x03, 0xF7}).empty());
    CHECK(a.dropped() == 2);
}
