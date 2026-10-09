// A virtual G2: the device side of the USB protocol, for tests and for working
// without hardware (re/notes/usb-protocol.md). It answers the version request,
// holds a performance, four patch slots, the synth settings and flash banks,
// accepts uploads and edits, answers requests and downloads with the session
// numbers it owns, and produces the synth-initiated traffic of a real unit
// (panel edits, patch releases, LEDs and meters).
//
// It follows the spec's [C] facts and makes the most defensible choice where
// the spec marks [I] (see the hardware checklist in usb-protocol.md):
// - every normal bubble gets one reply message: the requested dumps, or 7F;
// - a patch or performance upload is acknowledged, then released (38 / 1F)
//   with a new session number;
// - edits carrying a stale session number are discarded (and still acked).
#pragma once

#include "g2/proto/frame.hpp"
#include "g2/proto/state.hpp"
#include "g2/proto/transport.hpp"

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace g2::proto {

class Emulator {
public:
    struct FlashEntry {
        std::string name;
        u8 category = 0;
        std::vector<u8> file; // the .pch2 / .prf2 file
    };

    Emulator();

    // ---- The USB link ----
    // One bulk-OUT transfer from the host.
    void receive(std::span<const u8> frame);
    // Device -> host messages, oldest first.
    std::deque<Delivery>& outgoing() { return outgoing_; }

    // ---- State ----
    SynthState& state() { return state_; }
    const SynthState& state() const { return state_; }
    VersionInfo& version() { return state_.version; }
    // Flash banks: [type][(bank, prog)], type 0 patches, 1 performances.
    std::array<std::map<std::pair<u8, u8>, FlashEntry>, 2>& flash() { return flash_; }
    bool editorSyncActive() const { return editorSync_; }
    // Every frame received, decoded (for tests).
    const std::vector<HostFrame>& received() const { return received_; }

    // ---- Panel actions: synth-initiated messages ----
    void turnKnob(u8 slot, u8 location, u8 module, u8 param, u8 value); // 40 in the active variation
    void selectVariation(u8 slot, u8 variation);                       // 6A
    void selectParam(u8 slot, u8 location, u8 module, u8 param);       // 2F
    void midiLearn(u8 slot, u8 cc);                                     // 80
    // A patch loaded on the synth (from its own memory): new session, 38.
    void loadPatch(u8 slot, const Patch& patch, const std::string& name);
    // A new performance: new sessions, 1F.
    void loadPerformance(const Performance& perf, const std::string& name);
    void sendLeds(u8 slot, std::span<const u8> leds);               // 39, from LED 0
    void sendMeters(u8 slot, std::span<const std::uint16_t> meters); // 3A, from entry 0

    // ---- Fault injection ----
    void setResponding(bool on) { responding_ = on; } // off: requests go unanswered
    void failNextRequest(u8 code) { failNext_ = code; } // reply 7E code to the next bubble
    void corruptNextMessage() { corruptNext_ = true; }  // flip the CRC of the next message
    // Messages up to this length go embedded in the interrupt packet (0 sends
    // everything extended).
    void setMaxEmbedded(std::size_t n) { maxEmbedded_ = n; }

private:
    void send(u8 hdr, u8 session, const std::vector<Molecule>& molecules);
    void sendUnsolicited(u8 slot, std::vector<Molecule> molecules);
    void handleBubble(const HostFrame& frame, std::vector<u8> molecules);
    void handle(u8 slot, bool sessionOk, const Molecule& m, std::vector<Molecule>& reply,
                std::vector<std::vector<Molecule>>& after);
    std::vector<Molecule> patchDump(u8 slot) const;
    std::vector<Molecule> flashNames(u8 type, u8 bank, u8 prog) const;
    u8 nextSession(u8 current) const { return static_cast<u8>((current + 1) & kSessionMask); }

    SynthState state_;
    std::array<std::map<std::pair<u8, u8>, FlashEntry>, 2> flash_;
    std::deque<Delivery> outgoing_;
    std::vector<HostFrame> received_;
    std::vector<u8> partial_; // molecules of a bubble split over several frames
    bool editorSync_ = false;
    bool responding_ = true;
    std::optional<u8> failNext_;
    bool corruptNext_ = false;
    std::size_t maxEmbedded_ = kMaxEmbedded;
};

// Connects a Client to an Emulator in the same process: frames the client
// sends go straight to the emulator; the emulator's messages reach the client
// on the next poll().
class EmulatorTransport : public Transport {
public:
    explicit EmulatorTransport(Emulator& emulator) : emulator_(emulator) {}

    void plugIn();  // the client sees a device arrive on the next poll()
    void unplug();  // the client sees it go
    bool pluggedIn() const { return plugged_; }

    bool send(std::span<const std::uint8_t> frame) override;
    void poll() override;

private:
    Emulator& emulator_;
    bool plugged_ = false;
    std::deque<bool> events_; // pending arrivals (true) / removals (false)
};

} // namespace g2::proto
