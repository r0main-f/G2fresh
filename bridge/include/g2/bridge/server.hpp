// The g2bridge server: owns the connection to the G2 (a LocalLink, on USB or
// the virtual G2) and serves any number of editor processes over a local
// socket (wire.hpp). Each editor gets a mirror of the synth kept equal to the
// bridge's, the listener events, and may call every SynthLink operation.
//
// Concurrency policy: every editor may edit. Calls are executed in the order
// they arrive (one editor's calls in its order, editors interleaved as their
// messages come in); everyone sees every change. There is no slot locking:
// two editors editing the same parameter simply see the last value win.
//
// Single-threaded: pump() does everything (accept, read, execute, tick the
// link, send).
#pragma once

#include "g2/bridge/socket.hpp"
#include "g2/bridge/wire.hpp"
#include "g2/proto/link.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace g2::bridge {

class BridgeServer : private proto::Client::Listener, private proto::Client::Observer {
public:
    struct Options {
        std::string socketPath = defaultSocketPath();
        // Exit this long after the last editor left (or after starting, if
        // none came); never when `stay`.
        int idleExitMs = 5000;
        bool stay = false;
        // Editors whose unsent data grows past this are dropped, and
        // connections that do not say Hello in time.
        std::size_t maxBacklog = 128u << 20;
        int helloTimeoutMs = 5000;
    };

    BridgeServer(std::unique_ptr<proto::LocalLink> link, Options options);
    ~BridgeServer() override;

    // Starts listening. False (with the reason) if the socket cannot be
    // opened, e.g. "in use" when another bridge runs.
    bool start(std::string* error = nullptr);
    // One round, waiting up to `waitMs` for socket traffic first.
    void pump(int waitMs);
    // No editor for idleExitMs (and not `stay`).
    bool shouldExit() const;

    std::size_t clientCount() const { return clients_.size(); }
    proto::LocalLink& link() { return *link_; }

private:
    struct Editor {
        std::uint32_t id = 0;
        Socket socket;
        MessageBuffer in;
        std::vector<std::uint8_t> out;
        std::size_t outPos = 0;
        bool hello = false;
        bool dead = false;
        std::chrono::steady_clock::time_point since; // connected
    };

    void acceptAll();
    void readAll();
    void flushAll();
    void handle(Editor& c, const MessageBuffer::Message& m);
    void execute(Editor& c, Reader& r);
    void send(Editor& c, std::vector<std::uint8_t> message);
    // To every editor that said hello, but `except` (an id, 0: none).
    void broadcast(const std::vector<std::uint8_t>& message, std::uint32_t except = 0);
    void sendSnapshot(Editor& c);
    void broadcastSnapshot();
    std::vector<std::uint8_t> statusMessage() const;
    std::vector<std::uint8_t> eventMessage(EventKind kind, int slot, u8 value = 0,
                                           const proto::Molecule* molecule = nullptr) const;
    // Events for an edit an editor made, to the other editors.
    void editEvents(u8 target, const proto::Molecule& m);

    // Client::Listener
    void statusChanged(proto::Status) override;
    void synced() override;
    void slotChanged(int slot) override;
    void synthChanged() override;
    void performanceChanged() override;
    void flashNamesChanged(u8 type) override;
    void paramChanged(int slot, const proto::ParamChange& p) override;
    void morphChanged(int slot, const proto::MorphChange& m) override;
    void variationChanged(int slot, u8 variation) override;
    void paramFocused(int slot, const proto::ParamFocus& f) override;
    void patchEdited(int slot, const proto::Molecule& m) override;
    void ledsChanged(int slot) override;
    void midiLearned(const proto::MidiLearn& l) override;
    void error(u8 code) override;

    // Client::Observer
    void applied(u8 target, const proto::Molecule& m) override;
    void slotReplaced(int slot) override;
    void stateReplaced() override;

    std::unique_ptr<proto::LocalLink> link_;
    Options options_;
    Socket listener_;
    std::vector<std::unique_ptr<Editor>> clients_;
    std::uint32_t nextId_ = 1;
    std::uint32_t origin_ = 0; // the editor whose call is executing
    std::chrono::steady_clock::time_point idleSince_;
};

} // namespace g2::bridge
