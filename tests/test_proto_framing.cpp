#include <catch2/catch_test_macros.hpp>

#include "g2/crc.hpp"
#include "g2/proto/frame.hpp"
#include "g2/proto/molecules.hpp"
#include "proto_vectors.hpp"

#include <algorithm>
#include <map>
#include <string>

using namespace g2::proto;
using g2test::bytesOf;
using g2test::hexOf;

TEST_CASE("protocol vectors: CRC-16/XMODEM check values")
{
    for (const auto& c : g2test::kCrcVectors) {
        const auto data = c.ascii ? std::vector<u8>(c.data, c.data + std::string(c.data).size()) : bytesOf(c.data);
        INFO(c.data);
        CHECK(g2::crc16(data) == c.crc);
    }
}

TEST_CASE("protocol vectors: every host->device frame parses and rebuilds byte for byte")
{
    int frames = 0;
    for (const auto& v : g2test::kProtoVectors) {
        if (!v.hostToDevice() || !v.hex)
            continue;
        INFO(v.scenario << " #" << v.step << ": " << v.annotation);
        const auto bytes = bytesOf(v.hex);
        const auto f = parseHostFrame(bytes);
        REQUIRE(f);
        ++frames;
        if (f->versionRequest) {
            CHECK(versionRequest() == bytes);
            continue;
        }
        CHECK(hostFrame(f->hdr, f->session, f->molecules) == bytes);
        // The molecules decode to known, typed molecules and encode back.
        const auto molecules = decode(f->molecules);
        REQUIRE_FALSE(molecules.empty());
        for (const auto& m : molecules) {
            INFO("molecule 0x" << std::hex << int(idOf(m)));
            CHECK_FALSE(std::holds_alternative<Raw>(m));
        }
        CHECK(encode(molecules) == f->molecules);
    }
    CHECK(frames > 60);
}

TEST_CASE("protocol vectors: every device->host message is received, decoded and re-sent identically")
{
    // Per scenario, an extended announcement is followed by its bulk-IN transfer.
    std::map<std::string, std::size_t> pending;
    int messages = 0;
    for (const auto& v : g2test::kProtoVectors) {
        if (v.hostToDevice() || !v.hex)
            continue;
        INFO(v.scenario << " #" << v.step << ": " << v.annotation);
        const auto bytes = bytesOf(v.hex);
        std::vector<u8> message;
        if (v.interrupt()) {
            REQUIRE(bytes.size() == kInterruptPacketSize);
            const auto i = parseInterrupt(bytes);
            if (i.kind == Interrupt::Kind::Extended) {
                pending[v.scenario] = i.length;
                continue;
            }
            REQUIRE(i.kind == Interrupt::Kind::Embedded);
            message = i.message;
            // The device side announces it the same way.
            const auto d = deliver(message);
            CHECK(hexOf(d.interrupt) == hexOf(bytes));
            CHECK(d.bulk.empty());
        } else {
            REQUIRE(pending.count(v.scenario));
            CHECK(pending[v.scenario] == bytes.size());
            pending.erase(v.scenario);
            message = bytes;
            const auto d = deliver(message, true);
            CHECK(d.bulk == message);
            CHECK(d.interrupt[0] == 0x01);
            CHECK(std::size_t((d.interrupt[1] << 8) | d.interrupt[2]) == message.size());
        }
        const auto m = parseDeviceMessage(message);
        if (!v.valid) {
            CHECK_FALSE(m); // a bad CRC is dropped
            continue;
        }
        REQUIRE(m);
        ++messages;
        if (m->kind == DeviceMessage::Kind::Version) {
            const auto info = decodeVersionInfo(m->body);
            REQUIRE(info);
            CHECK(versionMessage(m->body) == message);
            continue;
        }
        CHECK(deviceMessage(m->hdr, m->session, m->body) == message);
        const auto molecules = decode(m->body);
        REQUIRE_FALSE(molecules.empty());
        for (const auto& x : molecules) {
            INFO("molecule 0x" << std::hex << int(idOf(x)));
            CHECK_FALSE(std::holds_alternative<Raw>(x));
        }
        CHECK(encode(molecules) == m->body);
    }
    CHECK(pending.empty());
    CHECK(messages > 20);
}

TEST_CASE("framing: version request, frame layout and header bits")
{
    CHECK(hexOf(versionRequest()) == "00 05 80 91 88");
    const std::vector<u8> molecules{0x35, 0x04};
    const auto frame = hostFrame(0x2C, 0x41, molecules);
    CHECK(hexOf(frame) == "00 09 01 2C 41 35 04 42 54");
    const auto f = parseHostFrame(frame);
    REQUIRE(f);
    CHECK(f->slot() == kSlotSynth);
    CHECK(f->voidSession());
    CHECK_FALSE(f->realtime());

    auto broken = frame;
    broken.back() ^= 1;
    CHECK_FALSE(parseHostFrame(broken)); // CRC
    broken = frame;
    broken[1] = 0x0A;
    CHECK_FALSE(parseHostFrame(broken)); // length
}

TEST_CASE("framing: interrupt packets")
{
    // Inline-long (byte0 & 3 == 0) is accepted when the message fits.
    std::vector<u8> packet(16, 0);
    const auto ack = deviceMessage(0x0C, 0x41, std::vector<u8>{0x7F});
    packet[0] = 0x00;
    packet[1] = 0x00;
    packet[2] = static_cast<u8>(ack.size());
    std::copy(ack.begin(), ack.end(), packet.begin() + 3);
    auto i = parseInterrupt(packet);
    REQUIRE(i.kind == Interrupt::Kind::Embedded);
    CHECK(i.message == ack);
    // byte0 & 3 == 3 is ignored, as is an embedded length past the packet.
    packet[0] = 0x03;
    CHECK(parseInterrupt(packet).kind == Interrupt::Kind::Ignored);
    packet[0] = 0xF2;
    CHECK(parseInterrupt(packet).kind == Interrupt::Kind::Embedded); // 15 bytes fit
    CHECK(parseInterrupt(std::vector<u8>{0xF2, 1, 2}).kind == Interrupt::Kind::Ignored);
    // A message longer than 15 bytes is always extended.
    const std::vector<u8> long16(16, 0x55);
    CHECK(deliver(long16).bulk == long16);
}

TEST_CASE("framing: the version reply's fields")
{
    VersionInfo v;
    v.model = VersionInfo::Engine;
    v.firmware = 155;
    v.unknown26 = 0x01020304;
    const auto info = encode(v);
    const auto back = decodeVersionInfo(info);
    REQUIRE(back);
    CHECK(*back == v);
    CHECK(std::string(back->modelName()) == "G2 Engine");
    CHECK(back->supportedModel());
    v.model = VersionInfo::Rack;
    CHECK_FALSE(v.supportedModel());
    CHECK_FALSE(decodeVersionInfo(std::vector<u8>(20, 0)));
    // The golden vector's reply: G2, OS 1.50, protocol 0x0012.
    const auto reply = parseDeviceMessage(g2test::vectorBytes("init_sequence", 2));
    REQUIRE(reply);
    const auto g2 = decodeVersionInfo(reply->body);
    REQUIRE(g2);
    CHECK(g2->model == VersionInfo::G2);
    CHECK(g2->firmware == 150);
    CHECK(g2->protocol == 0x0012);
    CHECK(encode(*g2) == reply->body); // the emulator's reply is the vector's
}
