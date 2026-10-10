// The editor's side of a G2 connection (re/notes/usb-protocol.md §4-§11):
// version handshake, the sync that reads everything from the synth (Clavia's
// order, §6.3), session numbers, stop-and-wait flow control with timeouts, the
// editor's edits, and the synth-initiated traffic (panel edits, releases,
// LEDs). It keeps a mirror of the synth (SynthState) up to date.
//
// Single-threaded and deterministic: everything happens in tick(), which polls
// the transport, handles timeouts against the injected clock and sends what is
// due. Edits apply to the mirror at once and go out in order.
#pragma once

#include "g2/proto/bubble.hpp"
#include "g2/proto/state.hpp"
#include "g2/proto/transport.hpp"

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace g2::proto {

// CSynthPort statuses (§11.2). Every status but NoDevice, Looking,
// StillLooking and Connected ends the connection until the device comes again
// (or restart()).
enum class Status : u8 {
    NoDevice = 0,
    Looking = 1,
    StillLooking = 2,     // no version reply within 10 s; the request is repeated
    UnsupportedModel = 3,
    VersionMismatch = 4,  // G2 Rack / Native, or protocol != 0x0012
    NewDevice = 5,        // the version info changed while connected
    Corrupted = 6,
    LostContact = 7,      // no reply within the timeout
    UpdateMode = 8,
    SynthException = 11,  // the synth sent 7E
    MajorError = 12,
    Connected = 13,
};
const char* statusText(Status s);
bool isFatal(Status s);

// 7E codes (XMHostMidi): 3 error in synth, 4 down-link checksum error, 5 stream
// execute error, 6 unfinished bubble.
const char* exceptionText(u8 code);

class Client : private Transport::Sink {
public:
    struct Options {
        std::uint64_t replyTimeoutMs = 10'000; // since max(last send, last receive), §11.1
        std::uint64_t versionRetryMs = 10'000; // §5.2
        // Resends of an unanswered message before Lost Contact. Clavia's
        // kErrorRetry is 0: the first timeout is fatal.
        int retries = 0;
        // A 7E reply ends the connection (Clavia's MajorError). Off: it fails
        // only the request (listener error(), handler sees the Exception).
        bool exceptionsAreFatal = true;
        bool readFlashNames = true; // sync step 13
        // The lowest-numbered connected port asks the synth for its slot focus
        // (08); others tell it there is none (09 04), §6.3 step 8.
        bool askSlotFocus = true;
    };

    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void statusChanged(Status) {}
        virtual void synced() {}                 // the full sync finished (7D 00 answered)
        virtual void slotChanged(int /*slot*/) {} // a slot's patch was (re)read from the synth
        virtual void synthChanged() {}           // synth settings, voices, clock, flash usage
        virtual void performanceChanged() {}     // performance name, header, global knobs, focus
        virtual void flashNamesChanged(u8 /*type*/) {}
        // Synth-initiated panel edits.
        virtual void paramChanged(int /*slot*/, const ParamChange&) {}
        virtual void morphChanged(int /*slot*/, const MorphChange&) {}
        virtual void variationChanged(int /*slot*/, u8 /*variation*/) {}
        virtual void paramFocused(int /*slot*/, const ParamFocus&) {}
        virtual void patchEdited(int /*slot*/, const Molecule&) {} // other edits made on the synth
        virtual void ledsChanged(int /*slot*/) {}
        virtual void midiLearned(const MidiLearn&) {}
        virtual void error(u8 /*exceptionCode*/) {}
    };

    // Told about every change to state(), in order: what g2bridge needs to keep
    // its editors' mirrors equal to its own. Calls come from tick() and from
    // the edit methods.
    class Observer {
    public:
        virtual ~Observer() = default;
        // A molecule applied to slot `target` (0..3) with applyToSlot, or to the
        // synth and performance (kSlotSynth) with applyToSynth; at synth level
        // also SessionNumber (a slot's or the performance's session),
        // PerformanceRelease and MidiLearn, which the client applies itself.
        virtual void applied(u8 target, const Molecule& m) = 0;
        // A slot's patch (and name) replaced: a download, or sendPatch().
        virtual void slotReplaced(int slot) = 0;
        // Anything else may have changed (a new connection, a performance
        // upload, new flash name lists): mirror the whole state again.
        virtual void stateReplaced() = 0;
    };

    using ReplyHandler = std::function<void(const std::vector<Molecule>&)>;

    Client(Transport& transport, Clock& clock);
    Client(Transport& transport, Clock& clock, Options options);
    ~Client() override;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void setListener(Listener* listener) { listener_ = listener; }
    void setObserver(Observer* observer) { observer_ = observer; }
    // Polls the transport, handles timeouts, sends what is due.
    void tick();

    Status status() const { return status_; }
    bool connected() const { return status_ == Status::Connected; }
    bool synced() const { return synced_; }
    // "G2 V1.50" when connected, else the status text.
    std::string statusLine() const;
    const SynthState& state() const { return state_; }
    // Messages dropped for a bad CRC (Clavia drops them silently).
    std::size_t droppedMessages() const { return dropped_; }
    // A request is waiting for its reply.
    bool waiting() const { return outstanding_.has_value(); }
    // Nothing queued and nothing outstanding.
    bool idle() const { return !outstanding_ && cleaner_.empty() && user_.empty(); }

    // Starts the handshake again after a fatal status (or while looking).
    void restart();
    // Reads everything from the synth again (CSynth::RequestEditorSyncLock).
    void resync();
    void resyncSlot(int slot);

    // ---- Uploads ----
    void sendPatch(int slot, const Patch& patch, const std::string& name); // §8.1, then the synth releases the slot
    void sendPerformance(const Performance& perf, const std::string& name);

    // ---- Live edits (§9). Each applies to the mirror at once. ----
    // Realtime messages go out at once, like Clavia's, unless edits are still
    // queued: then they keep their place behind them, so that the synth sees
    // the edits in the order they were made.
    void setParam(int slot, Location loc, u8 module, u8 param, u8 value, u8 variation); // realtime 40
    void selectParam(int slot, Location loc, u8 module, u8 param);                    // realtime 2F
    // Realtime while `dragging`, a normal (acknowledged) 43 on release.
    void setMorph(int slot, Location loc, u8 module, u8 param, u8 group, int range, u8 variation, bool dragging);
    void selectVariation(int slot, u8 variation);
    void copyVariation(int slot, u8 from, u8 to);
    void setMode(int slot, Location loc, u8 module, u8 mode, u8 value);
    void assignKnob(int slot, int knob, Location loc, u8 module, u8 param);
    void deassignKnob(int slot, int knob);
    void assignMidiCc(int slot, u8 cc, Location loc, u8 module, u8 param);
    void deassignMidiCc(int slot, u8 cc);
    void selectSlot(int slot);                       // 09 (performance bubble)
    void loadFromFlash(int slot, u8 bank, u8 prog);  // slot 4 = a performance; the synth releases
    void storeToFlash(int slot, u8 bank, u8 prog);
    void playNote(u8 note, bool on);                  // 56: no velocity, no channel (§12)
    // 03 CMSynthDataDump: the synth settings (MIDI channels, Local, ...). Clavia's editor sends it only to change
    // perfMode; the OS reads every field (CSynthMap::ReadStream).
    void setSynthSettings(const SynthSettings& settings);
    // Any patch edit: applied to the slot's mirror, sent as one T bubble.
    void edit(int slot, std::vector<Molecule> molecules);
    // Any request; `handler` gets the reply's molecules.
    void request(Bubble bubble, ReplyHandler handler = {});

private:
    // A bubble built when it is sent (so that it carries the current session).
    struct Pending {
        std::function<std::optional<Bubble>()> make;
        ReplyHandler handler;
        bool realtime = false; // sent without waiting for a reply
    };
    struct Outstanding {
        std::vector<std::vector<u8>> frames; // the bubble's messages
        std::size_t next = 0;                // index of the frame answered next
        std::vector<Molecule> reply;
        ReplyHandler handler;
        int retriesLeft = 0;
    };

    // Transport::Sink
    void deviceArrived() override;
    void deviceRemoved() override;
    void interruptPacket(std::span<const u8> packet) override;
    void bulkIn(std::span<const u8> data) override;

    void setStatus(Status s);
    void fail(Status s);
    void sendVersionRequest();
    void handleMessage(std::span<const u8> bytes);
    void handleVersion(const DeviceMessage& m);
    void handleReply(const DeviceMessage& m);
    void handleUnsolicited(const DeviceMessage& m);
    void route(u8 slot, const std::vector<Molecule>& molecules, bool fromReply);
    void sendNext();
    void sendFrame(const std::vector<u8>& frame);
    void sendRealtime(const Bubble& b);

    void enqueueCleaner(std::function<std::optional<Bubble>()> make, ReplyHandler handler = {});
    void enqueueUser(std::function<std::optional<Bubble>()> make, ReplyHandler handler = {});
    void enqueueSync(bool withLock);
    void enqueueSlotSync(int slot);
    void enqueueFlashNames(u8 type, u8 bank, u8 prog, bool front);
    void enqueueSyncDone();
    Bubble slotBubble(int slot, std::vector<Molecule> molecules) const;
    Bubble perfBubble(std::vector<Molecule> molecules) const;
    void applyEdit(int slot, const Molecule& m);
    void editBubble(int slot, std::vector<Molecule> molecules);

    Transport& transport_;
    Clock& clock_;
    Options options_;
    Listener* listener_ = nullptr;
    Observer* observer_ = nullptr;
    void applied(u8 target, const Molecule& m)
    {
        if (observer_)
            observer_->applied(target, m);
    }

    Status status_ = Status::NoDevice;
    bool devicePresent_ = false;
    bool synced_ = false;
    SynthState state_;
    std::optional<std::uint64_t> versionSentAt_;
    std::uint64_t lastSend_ = 0, lastReceive_ = 0;
    std::optional<std::size_t> expectedBulk_;
    std::size_t dropped_ = 0;

    std::deque<Pending> cleaner_; // sync traffic
    std::deque<Pending> user_;    // the editor's edits and requests
    std::optional<Outstanding> outstanding_;
    std::vector<FlashName> flashScratch_[2];
};

} // namespace g2::proto
