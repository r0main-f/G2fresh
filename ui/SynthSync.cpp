#include "SynthSync.h"

#include "g2/bridge/bridge_link.hpp"
#include "g2/edit.hpp"
#include "g2/proto/state.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <tuple>

namespace g2ui {
namespace {

using g2::Location;
using g2::u8;

// The g2bridge executable shipped with the app or plugin: next to the
// stand-alone executable, or in the plugin bundle's Resources folder.
juce::File findBridge()
{
    const auto self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const juce::String name = juce::SystemStats::getOperatingSystemType() & juce::SystemStats::Windows ? "g2bridge.exe"
                                                                                                        : "g2bridge";
    for (const auto& candidate : {self.getSiblingFile(name),
                                  self.getParentDirectory().getSiblingFile("Resources").getChildFile(name),
                                  self.getParentDirectory().getChildFile(name)})
        if (candidate.existsAsFile())
            return candidate;
    return {};
}

// Modules (index and type) and cables of both areas: what cannot change by
// single messages.
bool sameStructure(const g2::Patch& a, const g2::Patch& b)
{
    for (const auto loc : {Location::Va, Location::Fx}) {
        const auto& x = a.area(loc);
        const auto& y = b.area(loc);
        if (x.modules.size() != y.modules.size() || !(x.cables == y.cables))
            return false;
        for (const auto& m : x.modules) {
            const auto* other = y.find(m.index);
            if (!other || other->type != m.type)
                return false;
        }
    }
    return true;
}

using MorphKey = std::tuple<u8, u8, u8>; // location, module, param
std::map<MorphKey, g2::file::MorphAssign> morphsOf(const g2::Patch& p, std::size_t variation)
{
    std::map<MorphKey, g2::file::MorphAssign> out;
    if (variation < p.morphs.size())
        for (const auto& a : p.morphs[variation].assigns)
            out[{a.location, a.module, a.param}] = a;
    return out;
}

bool equalFiles(const g2::Patch& a, const g2::Patch& b)
{
    try {
        return g2::savePatch(a) == g2::savePatch(b);
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

SynthSync::SynthSync(PatchDocument& doc) : doc_(doc)
{
    doc_.addChangeListener(this);
}

SynthSync::~SynthSync()
{
    stopTimer();
    doc_.removeChangeListener(this);
    if (link_)
        link_->setListener(nullptr);
}

void SynthSync::startLink(std::unique_ptr<g2::proto::SynthLink> link, Kind kind)
{
    unbind();
    if (link_)
        link_->setListener(nullptr);
    link_ = std::move(link);
    kind_ = kind;
    wasReady_ = false;
    if (link_) {
        link_->setListener(this);
        startTimerHz(100);
    } else {
        stopTimer();
    }
    sendChangeMessage();
}

void SynthSync::connectG2()
{
    g2::bridge::BridgeLink::Options options;
    if (const auto bridge = findBridge(); bridge.existsAsFile())
        options.bridgeExecutable = bridge.getFullPathName().toStdString();
    startLink(std::make_unique<g2::bridge::BridgeLink>(options), Kind::G2);
}

void SynthSync::connectVirtual()
{
    startLink(g2::proto::LocalLink::virtualG2(), Kind::Virtual);
}

void SynthSync::disconnect()
{
    startLink(nullptr, Kind::None);
}

juce::String SynthSync::statusText() const
{
    if (!link_)
        return "Not connected";
    if (kind_ == Kind::Virtual)
        return link_->synced() ? "Virtual G2" : "Virtual G2: connecting...";
    if (link_->synced())
        return juce::String(link_->statusLine());
    if (link_->connected())
        return "Reading the G2...";
    const auto status = link_->status();
    if (status == g2::proto::Status::NoDevice || status == g2::proto::Status::Looking
        || status == g2::proto::Status::StillLooking)
        return "Looking for a G2...";
    return juce::String(link_->statusLine());
}

juce::String SynthSync::slotName(int slot) const
{
    if (!ready() || !juce::isPositiveAndBelow(slot, 4))
        return {};
    return juce::String(link_->state().slots[static_cast<std::size_t>(slot)].name);
}

// ---- Binding ---------------------------------------------------------------

int SynthSync::synthSlotFor(int docSlot) const
{
    if (binding_ == Binding::Patch)
        return docSlot == 0 ? slot_ : -1;
    if (binding_ == Binding::Performance)
        return docSlot;
    return -1;
}

int SynthSync::docSlotFor(int synthSlot) const
{
    if (binding_ == Binding::Patch)
        return synthSlot == slot_ ? 0 : -1;
    if (binding_ == Binding::Performance)
        return synthSlot;
    return -1;
}

void SynthSync::sendPatch(int slot)
{
    if (!ready() || doc_.isPerformance() || !juce::isPositiveAndBelow(slot, 4))
        return;
    unbind();
    link_->sendPatch(slot, doc_.patch(), doc_.name().toStdString());
    binding_ = Binding::Patch;
    slot_ = slot;
    sent_[static_cast<std::size_t>(slot)] = doc_.patch();
    sendChangeMessage();
}

void SynthSync::sendPerformance()
{
    if (!ready() || !doc_.isPerformance())
        return;
    unbind();
    const auto name = doc_.file() != juce::File() ? doc_.file().getFileNameWithoutExtension() : juce::String("Performance");
    link_->sendPerformance(*doc_.performance(), name.toStdString());
    binding_ = Binding::Performance;
    for (int s = 0; s < 4; ++s)
        sent_[static_cast<std::size_t>(s)] = doc_.slotPatch(s);
    sendChangeMessage();
}

void SynthSync::getPatch(int slot)
{
    if (!ready() || !juce::isPositiveAndBelow(slot, 4))
        return;
    unbind();
    const auto& s = link_->state().slots[static_cast<std::size_t>(slot)];
    doc_.loadPatch(s.patch, juce::String(s.name));
    binding_ = Binding::Patch;
    slot_ = slot;
    sent_[static_cast<std::size_t>(slot)] = doc_.patch();
    sendChangeMessage();
}

void SynthSync::getPerformance()
{
    if (!ready())
        return;
    unbind();
    doc_.loadPerformance(link_->state().performance());
    binding_ = Binding::Performance;
    for (int s = 0; s < 4; ++s)
        sent_[static_cast<std::size_t>(s)] = doc_.slotPatch(s);
    sendChangeMessage();
}

void SynthSync::unbind()
{
    if (binding_ == Binding::None)
        return;
    binding_ = Binding::None;
    for (auto& s : sent_)
        s.reset();
    sendChangeMessage();
}

// ---- Editor -> synth ----------------------------------------------------------

void SynthSync::changeListenerCallback(juce::ChangeBroadcaster*)
{
    if (!bound() || !ready())
        return;
    // A new or loaded document ends the binding: it is not what the synth has.
    if ((binding_ == Binding::Patch) == doc_.isPerformance()) {
        unbind();
        return;
    }
    for (int s = 0; s < (doc_.isPerformance() ? 4 : 1); ++s)
        push(s);
}

void SynthSync::push(int docSlot)
{
    const int slot = synthSlotFor(docSlot);
    if (slot < 0 || !sent_[static_cast<std::size_t>(slot)])
        return;
    auto& before = *sent_[static_cast<std::size_t>(slot)];
    const auto& now = doc_.slotPatch(docSlot);
    if (equalFiles(before, now))
        return;
    auto sendAll = [&] {
        link_->sendPatch(slot, now, doc_.isPerformance() ? link_->state().slots[static_cast<std::size_t>(slot)].name
                                                          : doc_.name().toStdString());
        before = now;
    };
    if (!sameStructure(before, now)) {
        sendAll();
        return;
    }

    // Single messages for what the original editor sends that way, applied to
    // a copy of what the synth has; anything they do not cover sends it all.
    g2::Patch probe = before;
    try {
        const auto variations = std::min<std::size_t>(now.variationCount, probe.variationCount);
        for (const auto loc : {Location::Va, Location::Fx})
            for (const auto& m : now.area(loc).modules) {
                auto* old = probe.area(loc).find(m.index);
                for (std::size_t v = 0; v < variations && v < m.params.size() && v < old->params.size(); ++v)
                    for (std::size_t i = 0; i < m.params[v].size() && i < old->params[v].size(); ++i)
                        if (m.params[v][i] != old->params[v][i]) {
                            link_->setParam(slot, loc, m.index, static_cast<u8>(i), m.params[v][i], static_cast<u8>(v));
                            old->params[v][i] = m.params[v][i];
                        }
                for (std::size_t i = 0; i < m.modes.size() && i < old->modes.size(); ++i)
                    if (m.modes[i] != old->modes[i]) {
                        link_->setMode(slot, loc, m.index, static_cast<u8>(i), m.modes[i]);
                        old->modes[i] = m.modes[i];
                    }
            }
        // Patch settings (morph dials, glide, vibrato...): parameters of location 2.
        for (const auto& m : now.settings)
            for (auto& old : probe.settings)
                if (old.index == m.index)
                    for (std::size_t v = 0; v < variations && v < m.params.size() && v < old.params.size(); ++v)
                        for (std::size_t i = 0; i < m.params[v].size() && i < old.params[v].size(); ++i)
                            if (m.params[v][i] != old.params[v][i]) {
                                link_->setParam(slot, Location::Settings, m.index, static_cast<u8>(i), m.params[v][i],
                                                static_cast<u8>(v));
                                old.params[v][i] = m.params[v][i];
                            }
        // Morph assignments, per variation (range 0 removes one).
        for (std::size_t v = 0; v < variations; ++v) {
            const auto was = morphsOf(probe, v);
            const auto is = morphsOf(now, v);
            for (const auto& [key, a] : was)
                if (!is.count(key)) {
                    link_->setMorph(slot, static_cast<Location>(a.location), a.module, a.param, a.morph, 0,
                                    static_cast<u8>(v), false);
                    g2::edit::clearMorph(probe, static_cast<u8>(v), static_cast<Location>(a.location), a.module, a.param);
                }
            for (const auto& [key, a] : is) {
                const auto it = was.find(key);
                if (it == was.end() || it->second.morph != a.morph || it->second.range != a.range) {
                    link_->setMorph(slot, static_cast<Location>(a.location), a.module, a.param, a.morph, a.range,
                                    static_cast<u8>(v), false);
                    g2::edit::setMorph(probe, static_cast<u8>(v), static_cast<Location>(a.location), a.module, a.param,
                                       a.morph, a.range);
                }
            }
        }
        // Knobs and MIDI controllers.
        for (int k = 0; k < g2::kKnobCount; ++k) {
            const auto& a = now.knobs[static_cast<std::size_t>(k)];
            const auto& b = probe.knobs[static_cast<std::size_t>(k)];
            const bool same = a.has_value() == b.has_value()
                && (!a || (a->location == b->location && a->module == b->module && a->param == b->param));
            if (same)
                continue;
            if (a) {
                link_->assignKnob(slot, k, static_cast<Location>(a->location), a->module, a->param);
                g2::edit::assignKnob(probe, k, static_cast<Location>(a->location), a->module, a->param);
            } else {
                link_->deassignKnob(slot, k);
                g2::edit::clearKnob(probe, k);
            }
        }
        std::map<u8, g2::file::CtrlAssign> wasCc, isCc;
        for (const auto& c : probe.controllers)
            wasCc[c.cc] = c;
        for (const auto& c : now.controllers)
            isCc[c.cc] = c;
        for (const auto& [cc, c] : wasCc)
            if (!isCc.count(cc)) {
                link_->deassignMidiCc(slot, cc);
                g2::edit::clearMidiCc(probe, cc);
            }
        for (const auto& [cc, c] : isCc) {
            const auto it = wasCc.find(cc);
            if (it == wasCc.end() || it->second.location != c.location || it->second.module != c.module
                || it->second.param != c.param) {
                link_->assignMidiCc(slot, cc, static_cast<Location>(c.location), c.module, c.param);
                g2::edit::assignMidiCc(probe, cc, static_cast<Location>(c.location), c.module, c.param);
            }
        }
        if (now.header.activeVariation != probe.header.activeVariation) {
            link_->selectVariation(slot, now.header.activeVariation);
            probe.header.activeVariation = now.header.activeVariation;
        }
    } catch (const std::exception&) {
        sendAll();
        return;
    }
    if (equalFiles(probe, now))
        before = now;
    else
        sendAll(); // names, colours, labels, notes...: the original sends those as edits we do not have yet
}

// ---- Synth -> editor ----------------------------------------------------------

void SynthSync::fromSynth(int synthSlot, const std::function<void(g2::Patch&)>& change)
{
    const int docSlot = docSlotFor(synthSlot);
    if (docSlot < 0 || !sent_[static_cast<std::size_t>(synthSlot)])
        return;
    try {
        change(*sent_[static_cast<std::size_t>(synthSlot)]);
    } catch (const std::exception&) {
    }
    doc_.performLiveOnSlot(docSlot, change);
}

void SynthSync::slotReplacedOnSynth(int synthSlot)
{
    const int docSlot = docSlotFor(synthSlot);
    if (docSlot < 0 || !ready())
        return;
    // A new patch in the slot (loaded on the synth, or sent by another editor).
    // Cable bend points are the editor's own: keep those of cables still there.
    g2::Patch fresh = link_->state().slots[static_cast<std::size_t>(synthSlot)].patch;
    const auto& old = doc_.slotPatch(docSlot);
    for (const auto loc : {Location::Va, Location::Fx})
        for (auto& c : fresh.area(loc).cables)
            for (const auto& o : old.area(loc).cables)
                if (o.bend && o.fromModule == c.fromModule && o.fromConn == c.fromConn
                    && o.fromIsOutput == c.fromIsOutput && o.toModule == c.toModule && o.toConn == c.toConn)
                    c.bend = o.bend;
    sent_[static_cast<std::size_t>(synthSlot)] = fresh;
    doc_.performLiveOnSlot(docSlot, [&](g2::Patch& p) { p = fresh; });
}

void SynthSync::timerCallback()
{
    if (!link_)
        return;
    link_->tick();
    if (ready() != wasReady_) {
        wasReady_ = ready();
        if (!wasReady_)
            unbind(); // the connection went: what the synth has is unknown now
        sendChangeMessage();
    }
}

void SynthSync::statusChanged(g2::proto::Status)
{
    sendChangeMessage();
}

void SynthSync::synced()
{
    sendChangeMessage();
}

void SynthSync::slotChanged(int slot)
{
    slotReplacedOnSynth(slot);
    sendChangeMessage(); // slot names in the menus
}

void SynthSync::paramChanged(int slot, const g2::proto::ParamChange& c)
{
    fromSynth(slot, [c](g2::Patch& p) {
        g2::edit::setTargetValue(p, {static_cast<Location>(c.location), c.module, c.param}, c.variation, c.value);
    });
}

void SynthSync::morphChanged(int slot, const g2::proto::MorphChange& c)
{
    fromSynth(slot, [c](g2::Patch& p) {
        const auto loc = static_cast<Location>(c.location);
        if (c.range == 0)
            g2::edit::clearMorph(p, c.variation, loc, c.module, c.param);
        else
            g2::edit::setMorph(p, c.variation, loc, c.module, c.param, c.morph, static_cast<std::int8_t>(c.range));
    });
}

void SynthSync::variationChanged(int slot, g2::u8 variation)
{
    fromSynth(slot, [variation](g2::Patch& p) { p.header.activeVariation = variation; });
    if (docSlotFor(slot) == doc_.slot() && variation < g2::kUserVariations)
        doc_.setVariation(variation);
}

void SynthSync::patchEdited(int slot, const g2::proto::Molecule& m)
{
    fromSynth(slot, [&m](g2::Patch& p) { g2::proto::applyToPatch(p, m); });
}

} // namespace g2ui
