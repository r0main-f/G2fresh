// The editor's connection to a G2: a SynthLink (through the g2bridge process,
// or to the virtual G2 in this process), ticked on the message thread, and
// the binding between the document and the synth's slots.
//
// Once a patch is sent to (or read from) a slot, or a performance to (or from)
// the synth, the document is bound: every edit goes to the synth as the
// original editor sends it (g2/proto/live.hpp: the molecules each edit
// records, undo and redo included, then single messages for the rest; the
// patch name, the performance's settings, name and slot focus), and what
// changes on the synth (panel knobs, variations, another editor's edits, the
// slot focus) comes into the document, outside the undo history. DAW
// automation changes the document, so it reaches the synth too. Owned by the
// plugin processor, so the connection outlives the editor window.
#pragma once

#include "LiveLeds.h"
#include "PatchDocument.h"

#include "g2/patch_load.hpp"
#include "g2/proto/link.hpp"
#include "g2/proto/live.hpp"

#include <juce_events/juce_events.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>

namespace g2ui {

class SynthSync : public juce::ChangeBroadcaster,
                  public LiveLeds,
                  private juce::Timer,
                  private juce::ChangeListener,
                  private PatchDocument::EditListener,
                  private g2::proto::SynthLink::Listener {
public:
    explicit SynthSync(PatchDocument& doc);
    ~SynthSync() override;

    enum class Kind { None, G2, Virtual, Emulated };
    // A G2 on USB, through the g2bridge process (started if needed).
    void connectG2();
    // Whether the bridge's USB log includes patch contents (off: redacted).
    // Saved in the user's settings; applies when the bridge next starts.
    static bool fullUsbLog();
    static void setFullUsbLog(bool on);
    // The virtual G2: try the synth features without hardware.
    void connectVirtual();
    // The emulated G2 (the user's own G2 OS running in G2fresh, which plays
    // its sound): a link made by its owner, which keeps the emulator running
    // while kind() is Emulated.
    void connectEmulated(std::unique_ptr<g2::proto::SynthLink> link);
    void disconnect();
    // The link gave up (no reply in time, an error): handshake and read the synth again, on the same link.
    void restartLink();
    bool linkFailed() const { return link_ != nullptr && g2::proto::isFatal(link_->status()); }
    // The synth's settings (MIDI channels, Local, ...), as the synth reported them, and a change to them.
    void setSynthSettings(const g2::proto::SynthSettings& settings);
    Kind kind() const { return kind_; }
    // Connected and the synth fully read.
    bool ready() const { return link_ != nullptr && link_->synced(); }
    const g2::proto::SynthLink* link() const { return link_.get(); }
    // "G2 V1.50", "Virtual G2", "Looking for a G2...", "Not connected".
    juce::String statusText() const;

    // ---- Binding -------------------------------------------------------
    // Sends the document's patch to a slot (or the performance to the synth)
    // and binds it.
    void sendPatch(int slot);
    // Sends the document's patch in a performance of its own (slot A alone,
    // on the keyboard: g2::Performance::playing) and binds it to slot A: for
    // a synth that should just play the patch, as the emulated G2 does.
    void sendPatchAlone();
    void sendPerformance();
    // Replaces the document with a slot's patch (or the synth's performance)
    // and binds it.
    void getPatch(int slot);
    void getPerformance();
    void unbind();
    bool bound() const { return binding_ != Binding::None; }
    // The slot a patch document is bound to, or -1 (unbound or performance).
    int boundSlot() const { return binding_ == Binding::Patch ? slot_ : -1; }
    // The synth slot the patch was last sent to or got from, bound or not (LiveEditing.h).
    int lastSlot() const { return slot_; }
    bool performanceBound() const { return binding_ == Binding::Performance; }
    // A slot's patch name on the synth ("" when unknown).
    juce::String slotName(int slot) const;

    // ---- Audition (the patch library) ------------------------------------
    // Plays a patch or a performance on the synth without touching the
    // document: a patch goes to the slot the document is live on (else the
    // synth's focused slot), a performance replaces the synth's. While it
    // plays, what it replaced is cut off from the document: edits wait, and
    // nothing the synth says about it comes in. endAudition() puts back what
    // was there: the document as it is then where it was live, else what the
    // synth had. An upload only; the synth's memory (flash) is never written.
    // Sending or loading something else to the same place ends the audition
    // without the way back.
    void audition(const g2::Patch& patch, const std::string& name);
    void audition(const g2::Performance& perf, const std::string& name);
    void endAudition();
    bool auditioning() const { return audition_ != Audition::None; }
    // For a document about to load the auditioned patch into `docSlot`, where
    // it is live: the audition ends with the patch left in the slot, known as
    // what the synth has, so that the document's arrival sends only what
    // differs (nothing: the sound goes on). False, and nothing done, if no
    // patch is auditioned there; endAudition() is the way then.
    bool adoptAudition(int docSlot);

    // ---- Synth memory (flash banks) --------------------------------------
    // Loads a stored patch into a slot (slot 4: a stored performance); with
    // `open`, the editor then shows it (replacing the document, bound).
    void loadFromBank(int slot, std::uint8_t bank, std::uint8_t prog, bool open);
    // Stores a slot's patch (slot 4: the performance) on the synth.
    void storeToBank(int slot, std::uint8_t bank, std::uint8_t prog);
    // A store the synth refused, with the original's message ("Memory protect on", "Flash memory full").
    std::function<void(const juce::String&)> onFlashProblem;

    // LiveLeds: the LEDs and meters of the slot shown, while bound.
    std::optional<int> ledValue(g2::Location location, std::uint8_t module, int group) const override;
    std::uint32_t ledGeneration() const override { return ledGeneration_; }
    bool ledsLive() const override { return ready() && bound(); }

    // The patch load the synth reports for the slot shown, while bound.
    std::optional<g2::patchload::Load> reportedLoad() const;
    // Whether the patch shown plays on the synth, and how many voices the synth gives it (nullopt until it has said:
    // CPatch::GetCurrentNumberOfVoices @000d458c).
    bool patchConnected() const { return ready() && bound() && synthSlotFor(doc_.slot()) >= 0; }
    std::optional<std::uint8_t> currentVoices() const;
    // The last MIDI controller the synth received (80 CMMidiLearn), while the patch is on it
    // (CPatch::GetLastUsedMidiController @000d47e8).
    std::optional<std::uint8_t> lastMidiCc() const;
    // The G2's focused slot (09 CMSlotFocus), or -1 when not connected or none.
    // What the G2 has in a slot (the mirror), with its name; nullopt when not connected.
    std::optional<std::pair<g2::Patch, std::string>> synthSlotPatch(int slot) const
    {
        if (!ready() || slot < 0 || slot > 3)
            return std::nullopt;
        const auto& s = link_->state().slots[static_cast<std::size_t>(slot)];
        return std::pair{s.patch, s.name};
    }
    // The toolbar's Perf: the G2's performance mode, and the switch (3E). The synth then plays other patches (the
    // performance it recalls, or its patch-mode slots again): the document is taken off it, not replaced.
    bool performanceMode() const { return ready() && link_->state().settings.perfMode; }
    void setPerformanceMode(bool on, bool shift);
    int synthFocusedSlot() const { return ready() && link_->state().slotFocus < 4 ? link_->state().slotFocus : -1; }

    // An alert the original shows when the synth reports an exception (7E):
    // title and text. Set by the editor window.
    std::function<void(const juce::String& title, const juce::String& text)> onAlert;

private:
    enum class Binding { None, Patch, Performance };
    enum class Audition { None, Slot, Performance };
    // Everything a synth plays: its performance (the four slots' patches), and whether it is in performance mode.
    struct SynthContent {
        g2::Performance performance;
        std::string name;
        bool performanceMode = false;
    };

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void startLink(std::unique_ptr<g2::proto::SynthLink> link, Kind kind);
    // Document slot <-> synth slot for the current binding (-1 if not bound).
    int synthSlotFor(int docSlot) const;
    int docSlotFor(int synthSlot) const;
    // Sends what changed in a document slot since the synth last got it,
    // `edit` (what the step recorded, or its undo) first.
    void push(int docSlot, const g2::live::Edit* edit = nullptr, bool undo = false);
    void send(int synthSlot, const std::vector<g2::proto::Outgoing>& out);
    // The performance's settings, name and slot focus (P bubbles).
    void pushPerformance();
    std::string nameFor(int docSlot) const;
    void bindPerformance();
    // PatchDocument::EditListener
    void patchEdited(int slot, const g2::live::Edit* edit, bool undo) override;
    void paramFocused(int slot, const g2::focus::Focus& focus) override;
    // Applies a change made on the synth to the document and to what we
    // know the synth has, so it is not sent back.
    void fromSynth(int synthSlot, const std::function<void(g2::Patch&)>& change);
    void slotReplacedOnSynth(int synthSlot);
    // A synth slot that plays an audition: not the document's for now.
    bool auditioned(int synthSlot) const
    {
        return audition_ == Audition::Performance || (audition_ == Audition::Slot && synthSlot == auditionSlot_);
    }
    // The first audition remembers what the synth has.
    void beginAudition();
    // What endAudition() leaves on the synth.
    SynthContent afterAudition() const;
    // Before something else goes to `slot` (4: to every slot): the audition
    // ends, without putting back what that replaces anyway.
    void leaveAuditionFor(int slot);

    // SynthLink::Listener
    void statusChanged(g2::proto::Status) override;
    void synced() override;
    void slotChanged(int slot) override;
    void paramChanged(int slot, const g2::proto::ParamChange&) override;
    void morphChanged(int slot, const g2::proto::MorphChange&) override;
    void variationChanged(int slot, g2::u8 variation) override;
    void patchEdited(int slot, const g2::proto::Molecule&) override;
    void ledsChanged(int slot) override;
    void flashNamesChanged(g2::u8 type) override;
    void flashResult(const g2::proto::FlashResult&) override;
    void performanceChanged() override;
    void error(g2::u8 code) override;
    // The virtual G2 makes its LEDs and meters move, so they can be seen.
    void animateVirtualLeds();

    PatchDocument& doc_;
    std::unique_ptr<g2::proto::SynthLink> link_;
    Kind kind_ = Kind::None;
    bool wasReady_ = false;
    Binding binding_ = Binding::None;
    int slot_ = 0; // Binding::Patch: the synth slot
    // What the synth has, per synth slot, as the document last sent or got it.
    std::array<std::optional<g2::proto::LiveSlot>, 4> live_;
    // The performance as the synth has it (Binding::Performance).
    g2::file::PerfHeader sentPerf_;
    std::string sentPerfName_;
    int sentFocus_ = -1;
    LedMap ledMap_; // of the document slot shown
    std::uint32_t ledGeneration_ = 0;
    double lastVirtualLeds_ = 0.0;
    int pendingOpen_ = -1; // a slot (4: the performance) to open once the synth has loaded it
    Audition audition_ = Audition::None;
    int auditionSlot_ = 0; // Audition::Slot: the synth slot
    g2::Patch auditionedPatch_; // ... and what plays there
    std::string auditionedName_;
    // What the synth had when the audition began.
    SynthContent beforeAudition_;
    bool auditionedPerformance_ = false; // a performance replaced every slot since then
    // What the last audition put back, while the mirror may not show it yet
    // (it takes an upload when it goes out): until the synth's next slot
    // report, a few seconds at most.
    std::optional<SynthContent> putBack_;
    double putBackAt_ = 0.0;
};

} // namespace g2ui
