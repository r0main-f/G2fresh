// An editor's link to the G2 through the g2bridge process (server.hpp), which
// owns the USB device and serves every editor at once: the stand-alone app and
// each plugin instance.
//
// The mirror (state()) follows the bridge's: changes arrive in tick(), the
// editor's own edits included, so they show in state() after the next tick()
// (a LocalLink applies them at once). Listener callbacks come from tick():
// synth-initiated news, and edits made by other editors, which a LocalLink
// would never report.
//
// Without a bridge the status is NoDevice; tick() keeps trying to connect
// (every reconnectMs) and, given the bridge executable, starts it. Calls made
// while not attached are dropped, as a Client drops them when not connected.
#pragma once

#include "g2/bridge/socket.hpp"
#include "g2/bridge/wire.hpp"
#include "g2/proto/link.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace g2::bridge {

class BridgeLink final : public proto::SynthLink {
public:
    struct Options {
        std::string socketPath = defaultSocketPath();
        // When set, a missing bridge is started from this executable.
        std::string bridgeExecutable;
        std::vector<std::string> bridgeArguments;
        int reconnectMs = 1000;
    };

    BridgeLink();
    explicit BridgeLink(Options options);
    ~BridgeLink() override;

    // Attached to a bridge (it said Welcome).
    bool attached() const { return attached_; }
    std::uint32_t clientId() const { return clientId_; }

    void setListener(Listener* listener) override { listener_ = listener; }
    void tick() override;

    proto::Status status() const override { return attached_ ? status_ : proto::Status::NoDevice; }
    bool synced() const override { return attached_ && synced_; }
    std::string statusLine() const override;
    const proto::SynthState& state() const override { return state_; }

    void restart() override;
    void resync() override;
    void resyncSlot(int slot) override;
    void sendPatch(int slot, const Patch& patch, const std::string& name) override;
    void sendPerformance(const Performance& perf, const std::string& name) override;
    void setParam(int slot, Location loc, u8 module, u8 param, u8 value, u8 variation) override;
    void selectParam(int slot, Location loc, u8 module, u8 param) override;
    void setMorph(int slot, Location loc, u8 module, u8 param, u8 group, int range, u8 variation, bool dragging) override;
    void selectVariation(int slot, u8 variation) override;
    void copyVariation(int slot, u8 from, u8 to) override;
    void setMode(int slot, Location loc, u8 module, u8 mode, u8 value) override;
    void assignKnob(int slot, int knob, Location loc, u8 module, u8 param) override;
    void deassignKnob(int slot, int knob) override;
    void assignMidiCc(int slot, u8 cc, Location loc, u8 module, u8 param) override;
    void deassignMidiCc(int slot, u8 cc) override;
    void selectSlot(int slot) override;
    void loadFromFlash(int slot, u8 bank, u8 prog) override;
    void storeToFlash(int slot, u8 bank, u8 prog) override;
    void playNote(u8 note, bool on) override;
    void edit(int slot, std::vector<proto::Molecule> molecules) override;

private:
    void tryAttach();
    void detach();
    void handle(const MessageBuffer::Message& m);
    void event(Reader& r);
    void op(Writer&& w);
    void flush();

    Options options_;
    Listener* listener_ = nullptr;
    Socket socket_;
    MessageBuffer in_;
    std::vector<std::uint8_t> out_;
    bool attached_ = false;      // Welcome received
    bool spawned_ = false;       // we started a bridge (once per detachment)
    std::uint32_t clientId_ = 0;
    std::chrono::steady_clock::time_point nextAttempt_{};
    proto::Status status_ = proto::Status::NoDevice;
    proto::Status reported_ = proto::Status::NoDevice; // the status the listener last heard
    bool synced_ = false;
    std::string statusLine_;
    proto::SynthState state_;
};

// Starts the bridge executable detached from this process (it outlives it).
// Returns false if it could not be started.
bool spawnBridge(const std::string& executable, const std::vector<std::string>& arguments = {});

} // namespace g2::bridge
