// LibusbTransport against a scripted G2 (fake_libusb.hpp): the reset at open,
// the empty bulk-IN reads before a message, the read deadline, the length check,
// the write timeout and the frame-size cap (re/notes/usb-crosscheck.md H1, M2,
// M3, M4).
#include "fake_libusb.hpp"

#include "g2/patch.hpp"
#include "g2/proto/bubble.hpp"
#include "g2/proto/sections.hpp"
#include "g2/usb/libusb_transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using fakeusb::Bytes;
using g2::usb::LibusbTimeouts;
using g2::usb::LibusbTransport;

namespace {

// Short reads and deadline, so that the tests wait little.
constexpr LibusbTimeouts kFast{5000, 20, 200};

struct Recorder : g2::proto::Transport::Sink {
    std::vector<std::string> events;
    std::vector<Bytes> bulks;
    void deviceArrived() override { events.push_back("arrived"); }
    void deviceRemoved() override { events.push_back("removed"); }
    void interruptPacket(std::span<const std::uint8_t>) override { events.push_back("interrupt"); }
    void bulkIn(std::span<const std::uint8_t> data) override
    {
        events.push_back("bulk");
        bulks.emplace_back(data.begin(), data.end());
    }
    auto count(const std::string& e) const { return std::count(events.begin(), events.end(), e); }
};

// Polls the transport until `done` or `ms` have passed; returns done().
template <class F> bool until(LibusbTransport& t, F done, int ms = 3000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        t.poll();
        if (done())
            return true;
        if (std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void pollFor(LibusbTransport& t, int ms)
{
    until(t, [] { return false; }, ms);
}

// An extended announcement of an n-byte message, and such a message.
Bytes announce(std::size_t n) { return {0x01, std::uint8_t(n >> 8), std::uint8_t(n), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; }
Bytes message(std::size_t n)
{
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i)
        b[i] = std::uint8_t(i * 7 + 1);
    return b;
}

struct Connected {
    Recorder sink;
    LibusbTransport transport;
    explicit Connected(bool hotplug, LibusbTimeouts timeouts = kFast)
        : transport((fakeusb::reset(hotplug), fakeusb::plug(), timeouts))
    {
        transport.setSink(&sink);
        REQUIRE(until(transport, [&] { return sink.count("arrived") == 1; }));
    }
};

} // namespace

TEST_CASE("usb: the G2 is reset when it is opened, before its interface is claimed (M2)", "[usb]")
{
    Connected c(false);
    const auto calls = fakeusb::stats().calls;
#if defined(_WIN32)
    CHECK(calls == std::vector<std::string>{"open", "claim"}); // WinUSB cannot reset a device
#else
    CHECK(calls == std::vector<std::string>{"open", "reset", "claim"});
#endif
}

TEST_CASE("usb: empty bulk-IN reads before a message are read again, the first non-empty one is the message (H1)", "[usb]")
{
    Connected c(false);
    // As a real G2 sends the version reply: two zero-length packets, then the 66 bytes.
    const auto reply = message(66);
    fakeusb::interrupt(announce(66));
    fakeusb::bulkIn({});
    fakeusb::bulkIn({});
    fakeusb::bulkIn(reply);
    REQUIRE(until(c.transport, [&] { return c.sink.bulks.size() == 1; }));
    CHECK(c.sink.bulks[0] == reply);
    CHECK(c.sink.events == std::vector<std::string>{"arrived", "interrupt", "bulk"});
    const auto s = fakeusb::stats();
    CHECK(s.bulkReads == 3);
    CHECK(s.readTimeout == kFast.readMs);
    CHECK(c.transport.lastError().empty());

    // The interrupt endpoint is read again after the message.
    fakeusb::interrupt({0x32, 1, 2, 3});
    CHECK(until(c.transport, [&] { return c.sink.count("interrupt") == 2; }));
    CHECK(fakeusb::stats().bulkReads == 3);
}

TEST_CASE("usb: a bulk-IN read that times out or fails is read again until the deadline (H1, M2)", "[usb]")
{
    Connected c(false);
    const auto m = message(20);
    fakeusb::interrupt(announce(20));
    fakeusb::bulkInFails(fakeusb::Fail::Error);
    // a failed read, one that times out, and the next one waiting, however slow the machine
    REQUIRE(until(c.transport, [&] { return fakeusb::stats().bulkReads >= 3; }));
    fakeusb::bulkIn(m);
    REQUIRE(until(c.transport, [&] { return c.sink.bulks.size() == 1; }));
    CHECK(c.sink.bulks[0] == m);
    CHECK(fakeusb::stats().bulkReads >= 3);
    CHECK(c.sink.count("removed") == 0);
}

TEST_CASE("usb: without data by the deadline the G2 is given up until it leaves and comes back (M2)", "[usb]")
{
    SECTION("with hotplug")
    {
        Connected c(true);
        fakeusb::interrupt(announce(20));
        for (int i = 0; i < 3; ++i)
            fakeusb::bulkIn({});
        REQUIRE(until(c.transport, [&] { return c.sink.count("removed") == 1; }));
        CHECK(c.transport.lastError().find("sent nothing") != std::string::npos);
        CHECK(fakeusb::stats().bulkReads >= 4); // three empty, then reads that time out (how many depends on the machine)

        // Still there: not opened again, even on an arrival.
        fakeusb::arrivalAgain();
        pollFor(c.transport, 100);
        CHECK(c.sink.count("arrived") == 1);

        // Switched off and on.
        fakeusb::unplug();
        pollFor(c.transport, 50);
        fakeusb::plug();
        REQUIRE(until(c.transport, [&] { return c.sink.count("arrived") == 2; }));
        CHECK(c.transport.lastError().empty());
    }
    SECTION("found by scanning (Windows)")
    {
        Connected c(false);
        fakeusb::interrupt(announce(20));
        REQUIRE(until(c.transport, [&] { return c.sink.count("removed") == 1; }));
        pollFor(c.transport, 1200); // a scan finds it there
        CHECK(c.sink.count("arrived") == 1);
        fakeusb::unplug();
        pollFor(c.transport, 1200); // a scan finds it gone
        fakeusb::plug();
        REQUIRE(until(c.transport, [&] { return c.sink.count("arrived") == 2; }));
    }
}

TEST_CASE("usb: a stalled bulk-IN read gives the G2 up, an unplugged one is a removal (M2)", "[usb]")
{
    Connected c(true);
    fakeusb::interrupt(announce(20));
    fakeusb::bulkInFails(fakeusb::Fail::Stall);
    REQUIRE(until(c.transport, [&] { return c.sink.count("removed") == 1; }));
    CHECK(c.transport.lastError().find("stall") != std::string::npos);
    fakeusb::arrivalAgain();
    pollFor(c.transport, 100);
    CHECK(c.sink.count("arrived") == 1);

    fakeusb::unplug();
    fakeusb::plug();
    REQUIRE(until(c.transport, [&] { return c.sink.count("arrived") == 2; }));
    fakeusb::interrupt(announce(20));
    REQUIRE(until(c.transport, [&] { return fakeusb::stats().bulkReads > 1; }));
    fakeusb::unplug(); // while the read is pending
    REQUIRE(until(c.transport, [&] { return c.sink.count("removed") == 2; }));
    fakeusb::plug(); // opened again at once: it left
    CHECK(until(c.transport, [&] { return c.sink.count("arrived") == 3; }));
}

TEST_CASE("usb: a read of another length than announced is passed on and noted (M2)", "[usb]")
{
    Connected c(false);
    auto read = [&](std::size_t announced, std::size_t got) {
        const auto before = c.sink.bulks.size();
        fakeusb::interrupt(announce(announced));
        fakeusb::bulkIn(message(got));
        REQUIRE(until(c.transport, [&] { return c.sink.bulks.size() == before + 1; }));
        CHECK(c.sink.bulks.back().size() == got);
        return c.transport.lastError();
    };
    auto e = read(20, 10);
    CHECK(e.find("10 bytes") != std::string::npos);
    CHECK(e.find("announced as 20") != std::string::npos);
    CHECK(e.find("out of step") == std::string::npos);
    CHECK(read(30, 29).find("out of step") != std::string::npos);
    CHECK(read(20, 20).find("out of step") != std::string::npos); // unchanged by a good read
    CHECK(read(40, 12).find("out of step") == std::string::npos); // the count starts again
    CHECK(c.sink.count("removed") == 0);
}

TEST_CASE("usb: bulk-OUT writes wait up to 5 s; a frame above 32 KB is sent and noted (M3, M4)", "[usb]")
{
    CHECK(LibusbTimeouts{}.sendMs == 5000);
    CHECK(LibusbTimeouts{}.readMs == 1000);
    CHECK(LibusbTimeouts{}.readTotalMs == 5000);
    Connected c(false, LibusbTimeouts{});
    const auto largest = message(g2::usb::kMaxSendFrame);
    CHECK(c.transport.send(largest));
    auto s = fakeusb::stats();
    REQUIRE(s.sent.size() == 1);
    CHECK(s.sent[0] == largest);
    CHECK(s.sendTimeout == 5000);
    CHECK(c.transport.lastError().empty());

    CHECK(c.transport.send(message(g2::usb::kMaxSendFrame + 1)));
    CHECK(fakeusb::stats().sent.size() == 2);
    CHECK(c.transport.lastError().find("32769 bytes") != std::string::npos);
}

TEST_CASE("usb: the corpus's patches upload in one frame below the cap (M4)", "[usb]")
{
    // Measured over 4 500 patches from Clavia's banks and the users' archives:
    // the largest upload is 19 359 bytes. The largest factory performance's is
    // 28 086; 44 of 1 097 user performances go above the cap (up to 60 850).
    std::size_t largest = 0, files = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(G2_CORPUS_DIR)) {
        if (e.path().extension() != ".pch2")
            continue;
        std::ifstream f(e.path(), std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
        const auto bubble = g2::proto::patchUpload(0, g2::loadPatch(bytes, {true}), "0123456789abcdef");
        const auto frames = g2::proto::framesFor(bubble);
        REQUIRE(frames.size() == 1);
        largest = (std::max)(largest, frames[0].size());
        ++files;
    }
    REQUIRE(files > 0);
    CHECK(largest <= g2::usb::kMaxSendFrame);
}
