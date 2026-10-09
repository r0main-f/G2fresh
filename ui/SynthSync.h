// The editor's connection to a G2: a SynthLink (through the g2bridge process,
// or to the virtual G2 in this process), ticked on the message thread, and
// the binding between the document and the synth's slots.
//
// Once a patch is sent to (or read from) a slot, or a performance to (or from)
// the synth, the document is bound: every edit goes to the synth as the
// original editor sends it (parameter, mode, morph, knob, controller and
// variation changes as single messages; anything else, e.g. modules or cables,
// by sending the slot's patch again), and what changes on the synth (panel
// knobs, variations, another editor's edits) comes into the document, outside
// the undo history. DAW automation changes the document, so it reaches the
// synth too. Owned by the plugin processor, so the connection outlives the
// editor window.
#pragma once

#include "LiveLeds.h"
#include "PatchDocument.h"

#include "g2/patch_load.hpp"
#include "g2/proto/link.hpp"

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
                  private g2::proto::SynthLink::Listener {
public:
    explicit SynthSync(PatchDocument& doc);
    ~SynthSync() override;

    enum class Kind { None, G2, Virtual };
    // A G2 on USB, through the g2bridge process (started if needed).
    void connectG2();
    // Whether the bridge's USB log includes patch contents (off: redacted).
    // Saved in the user's settings; applies when the bridge next starts.
    static bool fullUsbLog();
    static void setFullUsbLog(bool on);
    // The virtual G2: try the synth features without hardware.
    void connectVirtual();
    void disconnect();
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
    void sendPerformance();
    // Replaces the document with a slot's patch (or the synth's performance)
    // and binds it.
    void getPatch(int slot);
    void getPerformance();
    void unbind();
    bool bound() const { return binding_ != Binding::None; }
    // The slot a patch document is bound to, or -1 (unbound or performance).
    int boundSlot() const { return binding_ == Binding::Patch ? slot_ : -1; }
    bool performanceBound() const { return binding_ == Binding::Performance; }
    // A slot's patch name on the synth ("" when unknown).
    juce::String slotName(int slot) const;

    // ---- Synth memory (flash banks) --------------------------------------
    // Loads a stored patch into a slot (slot 4: a stored performance); with
    // `open`, the editor then shows it (replacing the document, bound).
    void loadFromBank(int slot, std::uint8_t bank, std::uint8_t prog, bool open);
    // Stores a slot's patch (slot 4: the performance) on the synth.
    void storeToBank(int slot, std::uint8_t bank, std::uint8_t prog);

    // LiveLeds: the LEDs and meters of the slot shown, while bound.
    std::optional<int> ledValue(g2::Location location, std::uint8_t module, int group) const override;
    std::uint32_t ledGeneration() const override { return ledGeneration_; }

    // The patch load the synth reports for the slot shown, while bound.
    std::optional<g2::patchload::Load> reportedLoad() const;

private:
    enum class Binding { None, Patch, Performance };

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void startLink(std::unique_ptr<g2::proto::SynthLink> link, Kind kind);
    // Document slot <-> synth slot for the current binding (-1 if not bound).
    int synthSlotFor(int docSlot) const;
    int docSlotFor(int synthSlot) const;
    // Sends what changed in a document slot since the synth last got it.
    void push(int docSlot);
    // Applies a change made on the synth to the document and to what we
    // know the synth has, so it is not sent back.
    void fromSynth(int synthSlot, const std::function<void(g2::Patch&)>& change);
    void slotReplacedOnSynth(int synthSlot);

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
    void performanceChanged() override;
    // The virtual G2 makes its LEDs and meters move, so they can be seen.
    void animateVirtualLeds();

    PatchDocument& doc_;
    std::unique_ptr<g2::proto::SynthLink> link_;
    Kind kind_ = Kind::None;
    bool wasReady_ = false;
    Binding binding_ = Binding::None;
    int slot_ = 0; // Binding::Patch: the synth slot
    // What the synth has, per synth slot, as the document last sent or got it.
    std::array<std::optional<g2::Patch>, 4> sent_;
    LedMap ledMap_; // of the document slot shown
    std::uint32_t ledGeneration_ = 0;
    double lastVirtualLeds_ = 0.0;
    int pendingOpen_ = -1; // a slot (4: the performance) to open once the synth has loaded it
};

} // namespace g2ui
