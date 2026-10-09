#include <catch2/catch_test_macros.hpp>

#include "g2/bridge/bridge_link.hpp"
#include "g2/bridge/server.hpp"
#include "g2/edit.hpp"
#include "g2/proto/emulator.hpp"

#ifdef G2_HAVE_USB
#include "g2/usb/libusb_transport.hpp"
#endif

#include <atomic>
#include <filesystem>
#include <functional>
#include <thread>

using namespace g2;
using namespace g2::bridge;
using namespace g2::proto;

namespace {

std::string testSocketPath()
{
    static std::atomic<int> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count() % 1000000;
    return (std::filesystem::temp_directory_path()
            / ("g2b-" + std::to_string(stamp) + "-" + std::to_string(counter++) + ".sock"))
        .string();
}

// Records what a listener hears.
struct Recorder : SynthLink::Listener {
    int params = 0, slots = 0, variations = 0, edits = 0, syncs = 0, statuses = 0;
    std::optional<ParamChange> lastParam;
    void paramChanged(int, const ParamChange& p) override
    {
        ++params;
        lastParam = p;
    }
    void slotChanged(int) override { ++slots; }
    void variationChanged(int, u8) override { ++variations; }
    void patchEdited(int, const Molecule&) override { ++edits; }
    void synced() override { ++syncs; }
    void statusChanged(Status) override { ++statuses; }
};

struct Bridge {
    std::string path = testSocketPath();
    BridgeServer server;
    explicit Bridge(int idleExitMs = 60'000) : server(LocalLink::virtualG2(), options(idleExitMs))
    {
        std::string error;
        REQUIRE(server.start(&error));
    }
    BridgeServer::Options options(int idleExitMs) const
    {
        BridgeServer::Options o;
        o.socketPath = path;
        o.idleExitMs = idleExitMs;
        return o;
    }
    std::unique_ptr<BridgeLink> link()
    {
        BridgeLink::Options o;
        o.socketPath = path;
        o.reconnectMs = 20;
        return std::make_unique<BridgeLink>(o);
    }
    Emulator& emulator() { return *server.link().emulator(); }
    // Runs the bridge and the links until `done` (or a few seconds).
    bool run(std::initializer_list<BridgeLink*> links, const std::function<bool()>& done)
    {
        for (int i = 0; i < 5000; ++i) {
            server.pump(0);
            for (auto* l : links)
                l->tick();
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        return false;
    }
    void settle(std::initializer_list<BridgeLink*> links, int rounds = 50)
    {
        run(links, [&, n = 0]() mutable { return ++n >= rounds; });
    }
};

// What the synth holds and an editor shows: the patches, their names, the
// performance and the settings (the emulator keeps device-side details the
// client does not mirror).
bool samePatches(const SynthState& editor, const SynthState& synth)
{
    if (editor.perfName != synth.perfName || encode(std::vector<Molecule>{SynthData{editor.settings}})
                                                 != encode(std::vector<Molecule>{SynthData{synth.settings}}))
        return false;
    for (int slot = 0; slot < kSlots; ++slot) {
        const auto& e = editor.slots[static_cast<std::size_t>(slot)];
        const auto& s = synth.slots[static_cast<std::size_t>(slot)];
        if (e.name != s.name || savePatch(e.patch) != savePatch(s.patch))
            return false;
    }
    return true;
}

bool sameMirror(const SynthState& a, const SynthState& b)
{
    if (synthSnapshot(a) != synthSnapshot(b))
        return false;
    for (int slot = 0; slot < kSlots; ++slot)
        if (slotSnapshot(a, slot) != slotSnapshot(b, slot))
            return false;
    return true;
}

} // namespace

TEST_CASE("bridge: two editors sync and mirror the bridge")
{
    Bridge bridge;
    auto a = bridge.link();
    auto b = bridge.link();
    Recorder ra, rb;
    a->setListener(&ra);
    b->setListener(&rb);
    REQUIRE(bridge.run({a.get(), b.get()}, [&] { return a->synced() && b->synced(); }));
    CHECK(a->connected());
    CHECK(b->connected());
    CHECK(a->statusLine() == "G2 V1.50");
    CHECK(ra.syncs == 1);
    CHECK(ra.statuses >= 1);
    CHECK(sameMirror(a->state(), bridge.server.link().state()));
    CHECK(sameMirror(b->state(), bridge.server.link().state()));
    CHECK(samePatches(a->state(), bridge.emulator().state()));
}

TEST_CASE("bridge: an edit from one editor reaches the synth and the other editor")
{
    Bridge bridge;
    auto a = bridge.link();
    auto b = bridge.link();
    Recorder ra, rb;
    a->setListener(&ra);
    b->setListener(&rb);
    REQUIRE(bridge.run({a.get(), b.get()}, [&] { return a->synced() && b->synced(); }));

    // A patch with an OscB, sent from A.
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, 7, 0, 0);
    a->sendPatch(0, p, "Bridge test");
    REQUIRE(bridge.run({a.get(), b.get()}, [&] {
        return bridge.server.link().client().idle() && b->state().slots[0].patch.va.find(osc) != nullptr;
    }));
    CHECK(bridge.emulator().state().slots[0].patch.va.find(osc) != nullptr);
    CHECK(b->state().slots[0].name == "Bridge test");
    CHECK(rb.slots >= 1); // B hears that slot A changed

    // A knob turned in A's editor.
    a->setParam(0, Location::Va, osc, 0, 99, 0);
    REQUIRE(bridge.run({a.get(), b.get()}, [&] {
        const auto* m = bridge.emulator().state().slots[0].patch.va.find(osc);
        return m && m->params[0][0] == 99 && rb.params > 0;
    }));
    CHECK(b->state().slots[0].patch.va.find(osc)->params[0][0] == 99);
    CHECK(a->state().slots[0].patch.va.find(osc)->params[0][0] == 99); // A's mirror follows the bridge
    REQUIRE(rb.lastParam);
    CHECK(rb.lastParam->value == 99);
    CHECK(ra.params == 0);

    // A variation selected in B.
    b->selectVariation(0, 3);
    REQUIRE(bridge.run({a.get(), b.get()}, [&] { return ra.variations > 0; }));
    bridge.settle({a.get(), b.get()});
    CHECK(sameMirror(a->state(), bridge.server.link().state()));
    CHECK(sameMirror(b->state(), bridge.server.link().state()));
}

TEST_CASE("bridge: a knob turned on the synth reaches every editor")
{
    Bridge bridge;
    auto a = bridge.link();
    auto b = bridge.link();
    Recorder ra, rb;
    a->setListener(&ra);
    b->setListener(&rb);
    REQUIRE(bridge.run({a.get(), b.get()}, [&] { return a->synced() && b->synced(); }));
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, 7, 0, 0);
    a->sendPatch(1, p, "Panel");
    REQUIRE(bridge.run({a.get(), b.get()}, [&] { return bridge.server.link().client().idle() && b->state().slots[1].patch.va.find(osc); }));

    bridge.emulator().turnKnob(1, 1, osc, 0, 17);
    REQUIRE(bridge.run({a.get(), b.get()}, [&] { return ra.params > 0 && rb.params > 0; }));
    CHECK(a->state().slots[1].patch.va.find(osc)->params[0][0] == 17);
    CHECK(b->state().slots[1].patch.va.find(osc)->params[0][0] == 17);
}

TEST_CASE("bridge: an editor leaving does not disturb the others, and the bridge exits when idle")
{
    Bridge bridge(100);
    auto a = bridge.link();
    auto b = bridge.link();
    REQUIRE(bridge.run({a.get(), b.get()}, [&] { return a->synced() && b->synced(); }));
    a.reset();
    bridge.settle({b.get()});
    CHECK(bridge.server.clientCount() == 1);
    CHECK(b->connected());
    Patch p = Patch::makeDefault();
    const u8 osc = edit::addModule(p, Location::Va, 7, 0, 0);
    b->sendPatch(2, p, "Still here");
    REQUIRE(bridge.run({b.get()}, [&] { return bridge.emulator().state().slots[2].patch.va.find(osc) != nullptr; }));

    CHECK_FALSE(bridge.server.shouldExit());
    b.reset();
    REQUIRE(bridge.run({}, [&] { return bridge.server.clientCount() == 0; }));
    CHECK_FALSE(bridge.server.shouldExit()); // not yet: 100 ms of grace
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(bridge.server.shouldExit());
}

TEST_CASE("bridge: malformed input is dropped without harm")
{
    Bridge bridge;
    auto good = bridge.link();
    REQUIRE(bridge.run({good.get()}, [&] { return good->synced(); }));

    // A call before Hello, garbage lengths, an unknown message type.
    Socket noHello = Socket::connect(bridge.path);
    REQUIRE(noHello.valid());
    noHello.write(Writer(MessageType::Op).u8(static_cast<u8>(Op::Restart)).finish());
    Socket garbage = Socket::connect(bridge.path);
    REQUIRE(garbage.valid());
    const std::vector<u8> junk{0xFF, 0xFF, 0xFF, 0x7F, 1, 2, 3};
    garbage.write(junk);
    Socket zero = Socket::connect(bridge.path);
    REQUIRE(zero.valid());
    zero.write(std::vector<u8>{0, 0, 0, 0});
    bridge.settle({good.get()}, 100);
    CHECK(bridge.server.clientCount() == 1); // only the good editor is left

    // After Hello: unknown types and truncated calls are ignored.
    Socket odd = Socket::connect(bridge.path);
    Writer hello(MessageType::Hello);
    for (const char c : {'G', '2', 'B', 'R'})
        hello.u8(static_cast<u8>(c));
    odd.write(hello.u16(kProtocolVersion).finish());
    odd.write(Writer(static_cast<MessageType>(0x55)).u32(1234).finish());
    odd.write(Writer(MessageType::Op).u8(static_cast<u8>(Op::SetParam)).u8(0).finish());
    odd.write(Writer(MessageType::Op).u8(static_cast<u8>(Op::SendPatch)).u8(0).bytes(junk).string("x").finish());
    odd.write(Writer(MessageType::Op).u8(200).finish());
    bridge.settle({good.get()}, 100);
    CHECK(bridge.server.clientCount() == 2);
    CHECK(good->connected());
    CHECK(bridge.server.link().connected());
}

TEST_CASE("bridge: an editor waits for a missing bridge and attaches when it comes")
{
    const auto path = testSocketPath();
    BridgeLink::Options o;
    o.socketPath = path;
    o.reconnectMs = 20;
    BridgeLink link(o);
    Recorder r;
    link.setListener(&r);
    link.tick();
    CHECK(link.status() == Status::NoDevice);
    CHECK(link.statusLine() == "No G2 bridge");
    link.setParam(0, Location::Va, 1, 0, 1, 0); // dropped, no crash

    BridgeServer::Options so;
    so.socketPath = path;
    BridgeServer server(LocalLink::virtualG2(), so);
    REQUIRE(server.start());
    for (int i = 0; i < 5000 && !link.synced(); ++i) {
        server.pump(0);
        link.tick();
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    CHECK(link.synced());
    CHECK(r.syncs == 1);

    // A second bridge on the same socket refuses to start.
    BridgeServer second(LocalLink::virtualG2(), so);
    std::string error;
    CHECK_FALSE(second.start(&error));
    CHECK(error == "in use");
}

TEST_CASE("bridge wire: messages split anywhere are reassembled; bad lengths are corrupt")
{
    const auto m1 = Writer(MessageType::Status).u8(13).u8(1).string("G2 V1.50").finish();
    const auto m2 = Writer(MessageType::Event).u8(1).u8(2).u8(3).bytes({}).finish();
    std::vector<u8> stream(m1);
    stream.insert(stream.end(), m2.begin(), m2.end());
    MessageBuffer buffer;
    for (const u8 b : stream)
        buffer.append({&b, 1});
    const auto first = buffer.next();
    REQUIRE(first);
    CHECK(first->type == MessageType::Status);
    Reader r(first->payload);
    CHECK(r.u8() == 13);
    CHECK(r.u8() == 1);
    CHECK(r.string() == "G2 V1.50");
    CHECK_FALSE(r.failed());
    CHECK(r.u32() == 0);
    CHECK(r.failed()); // read past the end
    REQUIRE(buffer.next());
    CHECK_FALSE(buffer.next());

    MessageBuffer bad;
    const std::vector<u8> huge{0xFF, 0xFF, 0xFF, 0xFF, 1};
    bad.append(huge);
    CHECK_FALSE(bad.next());
    CHECK(bad.corrupt());
}

#ifdef G2_HAVE_USB
TEST_CASE("usb: without a G2 the link waits quietly")
{
    auto transport = std::make_unique<usb::LibusbTransport>();
    if (!transport->available()) {
        WARN("libusb could not start here; skipped");
        return;
    }
    LocalLink link(std::move(transport));
    for (int i = 0; i < 20; ++i) {
        link.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // A G2 plugged into the machine running the tests would connect instead.
    CHECK((link.status() == Status::NoDevice || link.status() == Status::Looking || link.connected()));
}
#endif

#ifdef G2_BRIDGE_EXECUTABLE
TEST_CASE("bridge: an editor starts the bridge executable on demand")
{
    BridgeLink::Options o;
    o.socketPath = testSocketPath();
    o.reconnectMs = 50;
    o.bridgeExecutable = G2_BRIDGE_EXECUTABLE;
    o.bridgeArguments = {"--emulator", "--idle-exit", "300", "--socket", o.socketPath};
    auto link = std::make_unique<BridgeLink>(o);
    for (int i = 0; i < 4000 && !link->synced(); ++i) {
        link->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(link->synced());
    CHECK(link->statusLine() == "G2 V1.50");

    // It exits once the last editor left, removing its socket file (probing
    // with connections would count as editors and keep it alive).
    link.reset();
    bool gone = false;
    for (int i = 0; i < 400 && !gone; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        gone = !std::filesystem::exists(o.socketPath);
    }
    CHECK(gone);
}
#endif
