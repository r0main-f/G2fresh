// How an editor talks to a G2. The editor codes against SynthLink; behind it
// is either the connection itself (LocalLink: a Client on a transport in this
// process) or the g2bridge process that owns the USB device and serves every
// editor (the stand-alone app and each plugin instance) at once (BridgeLink,
// bridge/include/g2/bridge/bridge_link.hpp, in the g2bridgelib library).
//
// Like Client, a link is single-threaded: the editor calls tick() regularly
// (e.g. 100 times a second) on its own thread, and listener callbacks come
// from there. A LocalLink applies the editor's own edits to state() at once;
// a BridgeLink's state() shows them after the next tick().
#pragma once

#include "g2/proto/client.hpp"

#include <memory>
#include <string>
#include <vector>

namespace g2::proto {

class Emulator;

class SynthLink {
public:
    using Listener = Client::Listener;

    virtual ~SynthLink() = default;
    virtual void setListener(Listener* listener) = 0;
    virtual void tick() = 0;

    virtual Status status() const = 0;
    bool connected() const { return status() == Status::Connected; }
    virtual bool synced() const = 0;
    // "G2 V1.50" when connected, else the status text.
    virtual std::string statusLine() const = 0;
    // The mirror of the synth: performance, slots' patches, settings, LEDs.
    virtual const SynthState& state() const = 0;

    virtual void restart() = 0;
    virtual void resync() = 0;
    virtual void resyncSlot(int slot) = 0;

    // The same calls as Client (see client.hpp).
    virtual void sendPatch(int slot, const Patch& patch, const std::string& name) = 0;
    virtual void sendPerformance(const Performance& perf, const std::string& name) = 0;
    virtual void setParam(int slot, Location loc, u8 module, u8 param, u8 value, u8 variation) = 0;
    virtual void selectParam(int slot, Location loc, u8 module, u8 param) = 0;
    virtual void setMorph(int slot, Location loc, u8 module, u8 param, u8 group, int range, u8 variation,
                          bool dragging) = 0;
    virtual void selectVariation(int slot, u8 variation) = 0;
    virtual void copyVariation(int slot, u8 from, u8 to) = 0;
    virtual void setMode(int slot, Location loc, u8 module, u8 mode, u8 value) = 0;
    virtual void assignKnob(int slot, int knob, Location loc, u8 module, u8 param) = 0;
    virtual void deassignKnob(int slot, int knob) = 0;
    virtual void assignMidiCc(int slot, u8 cc, Location loc, u8 module, u8 param) = 0;
    virtual void deassignMidiCc(int slot, u8 cc) = 0;
    virtual void selectSlot(int slot) = 0;
    virtual void loadFromFlash(int slot, u8 bank, u8 prog) = 0;
    virtual void storeToFlash(int slot, u8 bank, u8 prog) = 0;
    virtual void playNote(u8 note, bool on) = 0;
    virtual void edit(int slot, std::vector<Molecule> molecules) = 0;
};

// A Client on a transport owned by the link, timed by the steady clock.
class LocalLink final : public SynthLink {
public:
    LocalLink(std::unique_ptr<Transport> transport, Client::Options options = {});
    ~LocalLink() override;

    // A link to a virtual G2 (the emulator, in this process): for trying the
    // synth features without hardware, and for tests.
    static std::unique_ptr<LocalLink> virtualG2();
    // The virtual G2 of a link made by virtualG2(), else nullptr.
    Emulator* emulator() { return emulator_.get(); }

    Client& client() { return client_; }

    void setListener(Listener* listener) override { client_.setListener(listener); }
    void tick() override { client_.tick(); }
    Status status() const override { return client_.status(); }
    bool synced() const override { return client_.synced(); }
    std::string statusLine() const override { return client_.statusLine(); }
    const SynthState& state() const override { return client_.state(); }
    void restart() override { client_.restart(); }
    void resync() override { client_.resync(); }
    void resyncSlot(int slot) override { client_.resyncSlot(slot); }

    void sendPatch(int slot, const Patch& patch, const std::string& name) override { client_.sendPatch(slot, patch, name); }
    void sendPerformance(const Performance& perf, const std::string& name) override { client_.sendPerformance(perf, name); }
    void setParam(int slot, Location loc, u8 module, u8 param, u8 value, u8 variation) override
    {
        client_.setParam(slot, loc, module, param, value, variation);
    }
    void selectParam(int slot, Location loc, u8 module, u8 param) override { client_.selectParam(slot, loc, module, param); }
    void setMorph(int slot, Location loc, u8 module, u8 param, u8 group, int range, u8 variation, bool dragging) override
    {
        client_.setMorph(slot, loc, module, param, group, range, variation, dragging);
    }
    void selectVariation(int slot, u8 variation) override { client_.selectVariation(slot, variation); }
    void copyVariation(int slot, u8 from, u8 to) override { client_.copyVariation(slot, from, to); }
    void setMode(int slot, Location loc, u8 module, u8 mode, u8 value) override { client_.setMode(slot, loc, module, mode, value); }
    void assignKnob(int slot, int knob, Location loc, u8 module, u8 param) override
    {
        client_.assignKnob(slot, knob, loc, module, param);
    }
    void deassignKnob(int slot, int knob) override { client_.deassignKnob(slot, knob); }
    void assignMidiCc(int slot, u8 cc, Location loc, u8 module, u8 param) override
    {
        client_.assignMidiCc(slot, cc, loc, module, param);
    }
    void deassignMidiCc(int slot, u8 cc) override { client_.deassignMidiCc(slot, cc); }
    void selectSlot(int slot) override { client_.selectSlot(slot); }
    void loadFromFlash(int slot, u8 bank, u8 prog) override { client_.loadFromFlash(slot, bank, prog); }
    void storeToFlash(int slot, u8 bank, u8 prog) override { client_.storeToFlash(slot, bank, prog); }
    void playNote(u8 note, bool on) override { client_.playNote(note, on); }
    void edit(int slot, std::vector<Molecule> molecules) override { client_.edit(slot, std::move(molecules)); }

private:
    std::unique_ptr<Emulator> emulator_; // virtualG2() only; outlives the transport
    std::unique_ptr<Transport> transport_;
    SteadyClock clock_;
    Client client_;
};

} // namespace g2::proto
