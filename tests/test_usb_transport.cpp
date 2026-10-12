// LibusbTransport against a scripted G2 (fake_libusb.hpp): the reset at open,
// the empty bulk-IN reads before a message, the read deadline, the scans, the
// length check, the write timeout and the frame-size cap
// (re/notes/usb-crosscheck.md H1, M2, M3, M4).
//
// The transport runs on the fake's clock, which only advance() moves, and each
// check comes after settle() or advance(): the I/O thread has then done all
// that the script and the time so far make it do, and no more. So the checks
// are of exact facts (how many reads by when, which events in which order), and
// none of them waits or depends on the machine.
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
#include <memory>
#include <string>
#include <vector>

using fakeusb::Bytes;
using g2::usb::LibusbTimeouts;
using g2::usb::LibusbTransport;
using namespace std::chrono_literals;

namespace {

using Strings = std::vector<std::string>;

// Not Clavia's, to see that the timings are the ones given.
constexpr LibusbTimeouts kShort{5000, 20, 200};

struct Recorder : g2::proto::Transport::Sink {
    Strings events;
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

// An extended announcement of an n-byte message, and such a message.
Bytes announce(std::size_t n) { return {0x01, std::uint8_t(n >> 8), std::uint8_t(n), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; }
Bytes message(std::size_t n)
{
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i)
        b[i] = std::uint8_t(i * 7 + 1);
    return b;
}

// What opening the G2 calls.
Strings opening()
{
#if defined(_WIN32)
    return {"open", "claim"}; // WinUSB cannot reset a device
#else
    return {"open", "reset", "claim"};
#endif
}
auto opens()
{
    const auto calls = fakeusb::stats().calls;
    return std::count(calls.begin(), calls.end(), "open");
}

// A transport on the fake libusb and its clock, with Clavia's timings unless
// others are given.
struct Rig {
    Recorder sink;
    LibusbTransport transport;
    Rig(bool hotplug, bool plugged, LibusbTimeouts timeouts = {})
        : transport(scripted(hotplug, plugged, timeouts), &fakeusb::now)
    {
        transport.setSink(&sink);
        settle();
    }
    // The I/O thread has done what it had to; the sink has what it queued.
    void settle()
    {
        fakeusb::settle();
        transport.poll();
    }
    void advance(std::chrono::milliseconds by)
    {
        fakeusb::advance(by);
        transport.poll();
    }

private:
    static LibusbTimeouts scripted(bool hotplug, bool plugged, LibusbTimeouts timeouts)
    {
        fakeusb::reset(hotplug);
        if (plugged)
            fakeusb::plug();
        return timeouts;
    }
};

// With the G2 there from the start: opened by the first scan.
struct Connected : Rig {
    explicit Connected(bool hotplug, LibusbTimeouts timeouts = {}) : Rig(hotplug, true, timeouts)
    {
        REQUIRE(sink.events == Strings{"arrived"});
    }
};

} // namespace

TEST_CASE("usb: the G2 is reset when it is opened, before its interface is claimed (M2)", "[usb]")
{
    Connected c(false);
    const auto s = fakeusb::stats();
    CHECK(s.calls == opening());
    CHECK(s.scans == 1);
    CHECK(c.transport.lastError().empty());
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
    c.settle();
    CHECK(c.sink.events == Strings{"arrived", "interrupt", "bulk"});
    REQUIRE(c.sink.bulks.size() == 1);
    CHECK(c.sink.bulks[0] == reply);
    const auto s = fakeusb::stats();
    CHECK(s.bulkReads == 3);
    CHECK(s.readTimeout == 1000);
    CHECK(c.transport.lastError().empty());

    // The interrupt endpoint is read again after the message.
    fakeusb::interrupt({0x32, 1, 2, 3});
    c.settle();
    CHECK(c.sink.events == Strings{"arrived", "interrupt", "bulk", "interrupt"});
    CHECK(fakeusb::stats().bulkReads == 3);
}

TEST_CASE("usb: a bulk-IN read that times out or fails is read again until the deadline (H1, M2)", "[usb]")
{
    Connected c(false);
    const auto m = message(20);
    fakeusb::interrupt(announce(20));
    fakeusb::bulkInFails(fakeusb::Fail::Error);
    c.settle();
    CHECK(fakeusb::stats().bulkReads == 2); // the one that failed and the next
    c.advance(999ms);
    CHECK(fakeusb::stats().bulkReads == 2);
    c.advance(1ms); // it times out
    CHECK(fakeusb::stats().bulkReads == 3);
    fakeusb::bulkIn(m);
    c.settle();
    CHECK(c.sink.events == Strings{"arrived", "interrupt", "bulk"});
    REQUIRE(c.sink.bulks.size() == 1);
    CHECK(c.sink.bulks[0] == m);
    CHECK(fakeusb::stats().bulkReads == 3);
    CHECK(c.transport.lastError().empty());
}

TEST_CASE("usb: a message has 5 s to come, in reads of 1 s (M2)", "[usb]")
{
    // As Clavia's CUSBManager::RxData (0x108a7e): the time is read after each
    // empty read, and the G2 is given up once 5000 ms have passed since the
    // first one started; a read under way is not cut short.
    Connected c(true);
    const auto m = message(20);
    fakeusb::interrupt(announce(20));
    c.settle();
    CHECK(fakeusb::stats().bulkReads == 1);

    SECTION("nothing comes: five reads, then the G2 is given up")
    {
        c.advance(4999ms);
        CHECK(fakeusb::stats().bulkReads == 5);
        CHECK(c.sink.events == Strings{"arrived", "interrupt"});
        c.advance(1ms);
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed"});
        const auto s = fakeusb::stats();
        CHECK(s.bulkReads == 5);
        auto calls = opening();
        calls.push_back("close");
        CHECK(s.calls == calls);
        CHECK(s.transfers == 0);
        CHECK(c.transport.lastError().find("sent nothing for 5000 ms") != std::string::npos);
    }
    SECTION("the message 1 ms before the deadline")
    {
        c.advance(4999ms);
        fakeusb::bulkIn(m);
        c.settle();
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "bulk"});
        c.advance(10s);
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "bulk"});
        CHECK(fakeusb::stats().bulkReads == 5);
        CHECK(c.transport.lastError().empty());
    }
    SECTION("the message at the deadline: too late")
    {
        c.advance(5000ms);
        fakeusb::bulkIn(m);
        c.settle();
        c.advance(10s);
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed"});
        CHECK(fakeusb::stats().bulkReads == 5);
    }
    SECTION("zero-length packets do not move the deadline")
    {
        for (int i = 0; i < 3; ++i)
            fakeusb::bulkIn({});
        c.advance(4999ms);
        CHECK(fakeusb::stats().bulkReads == 8); // three empty at once, then five of 1 s
        CHECK(c.sink.count("removed") == 0);
        c.advance(1ms);
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed"});
        CHECK(fakeusb::stats().bulkReads == 8);
    }
    // The deadline is looked at between two reads: the last one starts before
    // it and has its own second.
    SECTION("a read begun 1 ms before the deadline")
    {
        c.advance(4999ms);
        fakeusb::bulkIn({});
        c.advance(999ms); // 998 ms after the deadline
        CHECK(fakeusb::stats().bulkReads == 6);
        CHECK(c.sink.events == Strings{"arrived", "interrupt"});
        SECTION("brings the message")
        {
            fakeusb::bulkIn(m);
            c.settle();
            CHECK(c.sink.events == Strings{"arrived", "interrupt", "bulk"});
        }
        SECTION("times out: the G2 is given up")
        {
            c.advance(1ms);
            CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed"});
            CHECK(fakeusb::stats().bulkReads == 6);
        }
    }
}

TEST_CASE("usb: the read timeout and the deadline are the ones given", "[usb]")
{
    Connected c(false, kShort);
    fakeusb::interrupt(announce(20));
    for (int i = 0; i < 3; ++i)
        fakeusb::bulkIn({});
    c.advance(199ms);
    const auto s = fakeusb::stats();
    CHECK(s.bulkReads == 13); // three empty at once, then ten of 20 ms
    CHECK(s.readTimeout == 20);
    CHECK(c.sink.events == Strings{"arrived", "interrupt"});
    c.advance(1ms);
    CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed"});
    CHECK(fakeusb::stats().bulkReads == 13);
    CHECK(c.transport.lastError().find("sent nothing for 200 ms") != std::string::npos);
}

TEST_CASE("usb: without data by the deadline the G2 is given up until it leaves and comes back (M2)", "[usb]")
{
    SECTION("with hotplug")
    {
        Connected c(true);
        fakeusb::interrupt(announce(20));
        c.advance(5s);
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed"});
        CHECK(c.transport.lastError().find("sent nothing") != std::string::npos);
        CHECK(fakeusb::stats().scans == 2); // at the start, and once closed: still there

        // Still there: not opened again, on an arrival or by the scans of a minute.
        fakeusb::arrivalAgain();
        c.settle();
        CHECK(fakeusb::stats().scans == 3);
        c.advance(60s);
        CHECK(fakeusb::stats().scans == 15);
        CHECK(opens() == 1);
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed"});
        CHECK(c.transport.lastError().find("sent nothing") != std::string::npos);

        // Switched off and on.
        fakeusb::unplug();
        c.settle();
        CHECK(opens() == 1);
        fakeusb::plug();
        c.settle();
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed", "arrived"});
        CHECK(opens() == 2);
        CHECK(c.transport.lastError().empty());
    }
    SECTION("found by scanning (Windows)")
    {
        Connected c(false);
        fakeusb::interrupt(announce(20));
        c.advance(5s);
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed"});
        CHECK(fakeusb::stats().scans == 2);
        c.advance(10s); // ten scans find it there
        CHECK(fakeusb::stats().scans == 12);
        CHECK(opens() == 1);
        CHECK(c.transport.lastError().find("sent nothing") != std::string::npos);

        fakeusb::unplug();
        c.advance(999ms);
        CHECK(fakeusb::stats().scans == 12);
        c.advance(1ms); // a scan finds it gone
        CHECK(fakeusb::stats().scans == 13);
        CHECK(c.transport.lastError().empty());
        fakeusb::plug();
        c.advance(999ms);
        CHECK(opens() == 1);
        c.advance(1ms);
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed", "arrived"});
        CHECK(opens() == 2);
        CHECK(fakeusb::stats().scans == 14);
    }
}

TEST_CASE("usb: a stalled bulk-IN read gives the G2 up, an unplugged one is a removal (M2)", "[usb]")
{
    SECTION("with hotplug")
    {
        Connected c(true);
        fakeusb::interrupt(announce(20));
        fakeusb::bulkInFails(fakeusb::Fail::Stall);
        c.settle();
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed"});
        CHECK(c.transport.lastError().find("stall") != std::string::npos);
        fakeusb::arrivalAgain();
        c.advance(60s);
        CHECK(opens() == 1);
        CHECK(c.sink.count("arrived") == 1);

        fakeusb::unplug();
        fakeusb::plug();
        c.settle();
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed", "arrived"});
        fakeusb::interrupt(announce(20));
        c.settle();
        CHECK(fakeusb::stats().bulkReads == 2);
        fakeusb::unplug(); // while the read is pending: told by hotplug, the read is cancelled
        c.settle();
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed", "arrived", "interrupt", "removed"});
        CHECK(fakeusb::stats().cancelled == 1);
        CHECK(c.transport.lastError().empty());
        fakeusb::plug(); // opened again at once: it left
        c.settle();
        CHECK(c.sink.count("arrived") == 3);
        CHECK(fakeusb::stats().transfers == 2);
    }
    SECTION("found by scanning (Windows)")
    {
        Connected c(false);
        fakeusb::interrupt(announce(20));
        c.settle();
        CHECK(fakeusb::stats().bulkReads == 1);
        fakeusb::unplug(); // while the read is pending: it fails with NO_DEVICE
        c.settle();
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed"});
        CHECK(fakeusb::stats().cancelled == 0);
        fakeusb::plug();
        c.advance(999ms);
        CHECK(c.sink.count("arrived") == 1);
        c.advance(1ms); // the scan one second after the one that opened it
        CHECK(c.sink.events == Strings{"arrived", "interrupt", "removed", "arrived"});
        CHECK(fakeusb::stats().scans == 2);
    }
}

TEST_CASE("usb: without a G2 the bus is scanned every second, every 5 s with hotplug", "[usb]")
{
    SECTION("by scanning (Windows)")
    {
        Rig r(false, false);
        CHECK(fakeusb::stats().scans == 1);
        r.advance(999ms);
        CHECK(fakeusb::stats().scans == 1);
        r.advance(1ms);
        CHECK(fakeusb::stats().scans == 2);
        r.advance(10s);
        CHECK(fakeusb::stats().scans == 12);

        r.advance(300ms);
        fakeusb::plug();
        r.advance(699ms);
        CHECK(r.sink.events.empty());
        r.advance(1ms);
        CHECK(r.sink.events == Strings{"arrived"});
        CHECK(fakeusb::stats().scans == 13);
        r.advance(10s); // none while it is open
        CHECK(fakeusb::stats().scans == 13);
    }
    SECTION("with hotplug")
    {
        Rig r(true, false);
        CHECK(fakeusb::stats().scans == 1);
        r.advance(4999ms);
        CHECK(fakeusb::stats().scans == 1);
        r.advance(1ms);
        CHECK(fakeusb::stats().scans == 2);

        r.advance(1300ms);
        fakeusb::plug(); // opened at its arrival
        r.settle();
        CHECK(r.sink.events == Strings{"arrived"});
        CHECK(fakeusb::stats().scans == 3);
        r.advance(10s);
        CHECK(fakeusb::stats().scans == 3);
    }
}

TEST_CASE("usb: a G2 that cannot be opened is tried again at the next scan", "[usb]")
{
    bool hotplug = false;
    SECTION("by scanning (Windows)") {}
    SECTION("with hotplug") { hotplug = true; }
    Rig r(hotplug, false);
    const auto interval = hotplug ? 5000ms : 1000ms;
    fakeusb::openFails(1);
    fakeusb::plug();
    if (hotplug)
        r.settle(); // tried at its arrival
    else
        r.advance(interval); // at the next scan
    CHECK(fakeusb::stats().calls == Strings{"open"});
    CHECK(r.sink.events.empty());
    CHECK(r.transport.lastError().find("cannot open the G2") != std::string::npos);
    r.advance(interval - 1ms);
    CHECK(fakeusb::stats().calls == Strings{"open"});
    r.advance(1ms);
    auto calls = Strings{"open"};
    for (const auto& call : opening())
        calls.push_back(call);
    CHECK(fakeusb::stats().calls == calls);
    CHECK(r.sink.events == Strings{"arrived"});
    CHECK(r.transport.lastError().empty());
}

TEST_CASE("usb: a read of another length than announced is passed on and noted (M2)", "[usb]")
{
    Connected c(false);
    auto read = [&](std::size_t announced, std::size_t got) {
        const auto before = c.sink.bulks.size();
        fakeusb::interrupt(announce(announced));
        fakeusb::bulkIn(message(got));
        c.settle();
        REQUIRE(c.sink.bulks.size() == before + 1);
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
    CHECK(fakeusb::stats().bulkReads == 4);
}

TEST_CASE("usb: bulk-OUT writes wait up to 5 s; a frame above 32 KB is sent and noted (M3, M4)", "[usb]")
{
    CHECK(LibusbTimeouts{}.sendMs == 5000);
    CHECK(LibusbTimeouts{}.readMs == 1000);
    CHECK(LibusbTimeouts{}.readTotalMs == 5000);
    Connected c(false);
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

TEST_CASE("usb: closing the G2 waits for its cancelled transfers", "[usb]")
{
    Connected c(true);
    fakeusb::cancelTakes(25ms);
    fakeusb::unplug();
    // The I/O thread is in its closing, waiting for the cancelled interrupt read.
    c.advance(24ms);
    CHECK(c.sink.events == Strings{"arrived"});
    CHECK(fakeusb::stats().calls == opening());
    c.advance(1ms);
    CHECK(c.sink.events == Strings{"arrived", "removed"});
    const auto s = fakeusb::stats();
    CHECK(s.calls.back() == "close");
    CHECK(s.cancelled == 1);
    CHECK(s.transfers == 0);
}

TEST_CASE("usb: a transport destroyed with a read pending cancels it and waits for nothing", "[usb]")
{
    auto c = std::make_unique<Connected>(false);
    int waits = 0;
    SECTION("the interrupt read") {}
    SECTION("a bulk-IN read")
    {
        fakeusb::interrupt(announce(20));
        c->settle();
        CHECK(fakeusb::stats().bulkReads == 1);
    }
    SECTION("the cancellation takes 25 ms: three waits of the closing, 10 ms each at most")
    {
        fakeusb::cancelTakes(25ms);
        waits = 3;
    }
    c.reset();
    const auto s = fakeusb::stats();
    CHECK(s.calls.back() == "close");
    CHECK(s.cancelled == 1);
    CHECK(s.transfers == 0);
    CHECK(s.waitsWhileStopping == waits);
}

TEST_CASE("usb: a transport destroyed while it closes its device does not wait for a timeout", "[usb]")
{
    auto c = std::make_unique<Connected>(true);
    // The wake-up of ~LibusbTransport comes while the I/O thread, told that the
    // G2 left, cancels the interrupt read: the events that the closing runs
    // take it, and the thread must not go on to wait for more.
    fakeusb::holdCancelUntilStopped();
    fakeusb::unplug();
    fakeusb::settle(); // the I/O thread is held in libusb_cancel_transfer
    c.reset();
    const auto s = fakeusb::stats();
    CHECK(s.calls.back() == "close");
    CHECK(s.cancelled == 1);
    CHECK(s.transfers == 0);
    CHECK(s.waitsWhileStopping == 0);
}

TEST_CASE("usb: poll(), send() and lastError() while the I/O thread works", "[usb]")
{
    // Called without settling first, so that they meet the I/O thread at its
    // work (what a thread sanitizer looks at); the checks come once it is idle.
    Connected c(false);
    const auto frame = message(64);
    const int n = 200;
    int sent = 0;
    for (int i = 0; i < n; ++i) {
        fakeusb::interrupt(announce(20));
        fakeusb::bulkIn(message(i % 2 ? 20 : 19)); // every other one of another length: noted in lastError()
        c.transport.poll();
        if (c.transport.send(frame))
            ++sent;
        (void)c.transport.lastError();
    }
    c.settle();
    CHECK(sent == n);
    CHECK(c.sink.count("interrupt") == n);
    CHECK(c.sink.bulks.size() == std::size_t(n));
    CHECK(c.sink.count("removed") == 0);
    const auto s = fakeusb::stats();
    CHECK(s.bulkReads == n);
    CHECK(s.sent.size() == std::size_t(n));

    fakeusb::unplug();
    CHECK_FALSE(c.transport.send(frame)); // being closed or closed: not sent either way
    c.settle();
    CHECK(c.sink.count("removed") == 1);
    CHECK(fakeusb::stats().sent.size() == std::size_t(n));
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
