#include "SynthSync.h"

#include "PerfSwitch.h"
#include "PerfSync.h"

#include "ModulePainter.h"
#include "Skin.h"

#include "g2/bridge/bridge_link.hpp"
#include "g2/proto/emulator.hpp"
#include "g2/edit.hpp"
#include "g2/proto/state.hpp"

#include <algorithm>
#include <cmath>
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

std::uint64_t nowMs()
{
    return static_cast<std::uint64_t>(juce::Time::getMillisecondCounterHiRes());
}

// The performance header as a 11 carries it, without what other messages
// set: the slots' patch names (27 to each slot) and the focused slot (09).
} // namespace

SynthSync::SynthSync(PatchDocument& doc) : doc_(doc)
{
    doc_.addChangeListener(this);
    doc_.setEditListener(this);
}

SynthSync::~SynthSync()
{
    stopTimer();
    doc_.setEditListener(nullptr);
    doc_.removeChangeListener(this);
    if (link_)
        link_->setListener(nullptr);
}

void SynthSync::startLink(std::unique_ptr<g2::proto::SynthLink> link, Kind kind)
{
    unbind();
    audition_ = Audition::None; // nothing to put back on a synth that is gone
    putBack_.reset();
    perfSwitchWasLive_.reset();
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

bool SynthSync::fullUsbLog()
{
    return userSettings().getBoolValue("fullUsbLog", false);
}

void SynthSync::setFullUsbLog(bool on)
{
    userSettings().setValue("fullUsbLog", on);
}

void SynthSync::connectG2()
{
    g2::bridge::BridgeLink::Options options;
    if (const auto bridge = findBridge(); bridge.existsAsFile())
        options.bridgeExecutable = bridge.getFullPathName().toStdString();
    if (fullUsbLog())
        options.bridgeArguments.push_back("--log-full");
    startLink(std::make_unique<g2::bridge::BridgeLink>(options), Kind::G2);
}

void SynthSync::connectVirtual()
{
    startLink(g2::proto::LocalLink::virtualG2(), Kind::Virtual);
}

void SynthSync::connectEmulated(std::unique_ptr<g2::proto::SynthLink> link)
{
    startLink(std::move(link), Kind::Emulated);
}

void SynthSync::setPerformanceMode(bool on, bool shift)
{
    if (!ready() || performanceMode() == on)
        return;
    leaveAuditionFor(4);
    perfSwitchWasLive_ = bound();
    unbind();
    link_->setPerformanceMode(on, shift);
    sendChangeMessage();
}

void SynthSync::setSynthSettings(const g2::proto::SynthSettings& settings)
{
    if (ready())
        link_->setSynthSettings(settings);
}

void SynthSync::restartLink()
{
    if (link_ == nullptr)
        return;
    unbind();
    audition_ = Audition::None;
    putBack_.reset();
    perfSwitchWasLive_.reset();
    wasReady_ = false;
    link_->restart();
    sendChangeMessage();
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
    if (kind_ == Kind::Emulated)
        return link_->synced() ? "Emulated " + juce::String(link_->statusLine()) : "Emulated G2: starting...";
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
    const int slot = binding_ == Binding::Patch ? (docSlot == 0 ? slot_ : -1) : binding_ == Binding::Performance ? docSlot : -1;
    return auditioned(slot) ? -1 : slot;
}

int SynthSync::docSlotFor(int synthSlot) const
{
    if (auditioned(synthSlot))
        return -1;
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
    leaveAuditionFor(slot);
    unbind();
    link_->sendPatch(slot, doc_.patch(), doc_.name().toStdString());
    // The slot the document is in has the focus (CSynth::DownloadPatch
    // @0011dc62 -> CPortManager::SetFocusedSlot; 09).
    link_->selectSlot(slot);
    binding_ = Binding::Patch;
    slot_ = slot;
    live_[static_cast<std::size_t>(slot)] = g2::proto::LiveSlot::uploaded(doc_.patch(), doc_.name().toStdString());
    ledMap_ = g2ui::ledMap(doc_.patch());
    sendChangeMessage();
}

void SynthSync::sendPatchAlone()
{
    if (!ready() || doc_.isPerformance())
        return;
    leaveAuditionFor(4);
    unbind();
    // The original's download of a patch to a synth without a performance
    // (CreateDownloadPerformanceFromPatch @000fe478): a "void" performance,
    // played in patch mode.
    link_->sendPerformance(g2::Performance::playing(doc_.patch(), doc_.name().toStdString()), doc_.name().toStdString(),
                           false);
    binding_ = Binding::Patch;
    slot_ = 0;
    live_[0] = g2::proto::LiveSlot::uploaded(doc_.patch(), doc_.name().toStdString());
    ledMap_ = g2ui::ledMap(doc_.patch());
    sendChangeMessage();
}

void SynthSync::bindPerformance()
{
    binding_ = Binding::Performance;
    for (int s = 0; s < 4; ++s)
        live_[static_cast<std::size_t>(s)] = g2::proto::LiveSlot::uploaded(doc_.slotPatch(s), nameFor(s));
    sentPerf_ = doc_.performance()->header;
    sentPerfName_ = doc_.performanceName().toStdString();
    sentFocus_ = doc_.slot();
    ledMap_ = g2ui::ledMap(doc_.patch());
}

void SynthSync::sendPerformance()
{
    if (!ready() || !doc_.isPerformance())
        return;
    leaveAuditionFor(4);
    unbind();
    link_->sendPerformance(*doc_.performance(), doc_.performanceName().toStdString(), true);
    bindPerformance();
    sendChangeMessage();
}

void SynthSync::getPatch(int slot)
{
    if (!ready() || !juce::isPositiveAndBelow(slot, 4))
        return;
    if (auditioned(slot)) {
        // The slot as it is once the audition is over (the mirror still has
        // the auditioned patch until the way back went out).
        const auto after = afterAudition().performance;
        endAudition();
        unbind();
        doc_.loadPatch(after.slots[static_cast<std::size_t>(slot)],
                       juce::String(after.header.slots[static_cast<std::size_t>(slot)].patchName));
    } else {
        unbind();
        const auto& s = link_->state().slots[static_cast<std::size_t>(slot)];
        doc_.loadPatch(s.patch, juce::String(s.name));
    }
    if (link_->state().slotFocus != slot)
        link_->selectSlot(slot); // the window shown is the focused slot (CPatch::SetTrulyFocused @000d4a18)
    binding_ = Binding::Patch;
    slot_ = slot;
    live_[static_cast<std::size_t>(slot)] = g2::proto::LiveSlot(doc_.patch(), doc_.name().toStdString());
    sendChangeMessage();
}

void SynthSync::getPerformance()
{
    if (!ready())
        return;
    if (auditioning()) {
        const auto after = afterAudition();
        endAudition();
        unbind();
        doc_.loadPerformance(after.performance, juce::String(after.name));
    } else {
        unbind();
        doc_.loadPerformance(link_->state().performance(), juce::String(link_->state().perfName));
    }
    bindPerformance();
    sendChangeMessage();
}

void SynthSync::unbind()
{
    if (binding_ == Binding::None)
        return;
    binding_ = Binding::None;
    for (auto& s : live_)
        s.reset();
    sendChangeMessage();
}

// ---- Audition ------------------------------------------------------------------

void SynthSync::beginAudition()
{
    // What the synth has: what an audition just put back, if the mirror may
    // not have it yet, else the mirror.
    if (putBack_ && juce::Time::getMillisecondCounterHiRes() - putBackAt_ < 5000.0) {
        beforeAudition_ = *putBack_;
    } else {
        const auto& state = link_->state();
        beforeAudition_ = {state.performance(), state.perfName, state.settings.perfMode};
    }
    auditionedPerformance_ = false;
}

void SynthSync::audition(const g2::Patch& patch, const std::string& name)
{
    if (!ready())
        return;
    if (audition_ == Audition::None)
        beginAudition();
    if (audition_ != Audition::Slot) {
        // After a performance, the slots first: the patch replaces one of them.
        if (audition_ == Audition::Performance) {
            const auto after = afterAudition();
            link_->sendPerformance(after.performance, after.name, after.performanceMode);
            if (binding_ == Binding::Performance && doc_.isPerformance())
                bindPerformance(); // the other slots are the document's again, as it is now
        }
        // Where the document plays, so that what plays the document plays this; else the synth's focused slot.
        const int focus = link_->state().slotFocus;
        auditionSlot_ = binding_ == Binding::Patch ? slot_ : binding_ == Binding::Performance ? doc_.slot() : focus < 4 ? focus : 0;
        audition_ = Audition::Slot;
    }
    link_->sendPatch(auditionSlot_, patch, name);
    auditionedPatch_ = patch;
    auditionedName_ = name;
    sendChangeMessage();
}

bool SynthSync::adoptAudition(int docSlot)
{
    if (audition_ != Audition::Slot || !ready())
        return false;
    audition_ = Audition::None; // the slot is the document's again
    if (synthSlotFor(docSlot) != auditionSlot_) {
        audition_ = Audition::Slot;
        return false;
    }
    live_[static_cast<std::size_t>(auditionSlot_)] = g2::proto::LiveSlot::uploaded(auditionedPatch_, auditionedName_);
    sendChangeMessage();
    return true;
}

void SynthSync::audition(const g2::Performance& perf, const std::string& name)
{
    if (!ready())
        return;
    if (audition_ == Audition::None)
        beginAudition();
    audition_ = Audition::Performance;
    auditionedPerformance_ = true;
    link_->sendPerformance(perf, name, true);
    sendChangeMessage();
}

SynthSync::SynthContent SynthSync::afterAudition() const
{
    // The document where it is live. Around it, what the synth had when the
    // audition began; or, if only one slot was ever replaced, what it has now
    // beside that slot.
    if (binding_ == Binding::Performance && doc_.isPerformance())
        return {*doc_.performance(), doc_.performanceName().toStdString(), true};
    SynthContent after = beforeAudition_;
    if (!auditionedPerformance_) {
        const auto& state = link_->state();
        after = {state.performance(), state.perfName, state.settings.perfMode};
        const auto s = static_cast<std::size_t>(auditionSlot_);
        after.performance.slots[s] = beforeAudition_.performance.slots[s];
        after.performance.header.slots[s].patchName = beforeAudition_.performance.header.slots[s].patchName;
    }
    if (binding_ == Binding::Patch && !doc_.isPerformance()) {
        after.performance.slots[static_cast<std::size_t>(slot_)] = doc_.patch();
        after.performance.header.slots[static_cast<std::size_t>(slot_)].patchName = doc_.name().toStdString();
    }
    return after;
}

void SynthSync::endAudition()
{
    if (audition_ == Audition::None)
        return;
    if (!ready()) {
        audition_ = Audition::None;
        sendChangeMessage();
        return;
    }
    const auto after = afterAudition();
    const auto was = audition_;
    audition_ = Audition::None;
    if (was == Audition::Slot) {
        const auto s = static_cast<std::size_t>(auditionSlot_);
        const auto& name = after.performance.header.slots[s].patchName;
        link_->sendPatch(auditionSlot_, after.performance.slots[s], name);
        if (docSlotFor(auditionSlot_) >= 0)
            live_[s] = g2::proto::LiveSlot::uploaded(after.performance.slots[s], name);
    } else {
        link_->sendPerformance(after.performance, after.name, after.performanceMode);
        if (binding_ == Binding::Performance && doc_.isPerformance())
            bindPerformance();
        else if (binding_ == Binding::Patch && !doc_.isPerformance())
            live_[static_cast<std::size_t>(slot_)] = g2::proto::LiveSlot::uploaded(doc_.patch(), doc_.name().toStdString());
    }
    putBack_ = after;
    putBackAt_ = juce::Time::getMillisecondCounterHiRes();
    sendChangeMessage();
}

void SynthSync::leaveAuditionFor(int slot)
{
    if (slot == 4 || (audition_ == Audition::Slot && slot == auditionSlot_)) {
        if (audition_ != Audition::None)
            sendChangeMessage();
        audition_ = Audition::None;
    } else {
        endAudition();
    }
    putBack_.reset(); // something else goes to the synth now: the mirror is what it has
}

// ---- Editor -> synth ----------------------------------------------------------

std::string SynthSync::nameFor(int docSlot) const
{
    if (const auto* perf = doc_.performance())
        return perf->header.slots[static_cast<std::size_t>(juce::jlimit(0, 3, docSlot))].patchName;
    return doc_.name().toStdString();
}

void SynthSync::changeListenerCallback(juce::ChangeBroadcaster*)
{
    ledMap_ = g2ui::ledMap(doc_.patch());
    ++ledGeneration_;
    if (!bound() || !ready())
        return;
    // A new or loaded document ends the binding: it is not what the synth has.
    if ((binding_ == Binding::Patch) == doc_.isPerformance()) {
        unbind();
        return;
    }
    // Edits arrive one by one through patchEdited(); this catches anything else.
    for (int s = 0; s < (doc_.isPerformance() ? 4 : 1); ++s)
        push(s);
    pushPerformance();
}

void SynthSync::patchEdited(int slot, const g2::live::Edit* edit, bool undo)
{
    if (!bound() || !ready() || (binding_ == Binding::Patch) == doc_.isPerformance())
        return;
    push(slot, edit, undo);
}

void SynthSync::paramFocused(int slot, const g2::focus::Focus& focus)
{
    // A realtime 2F (CPatch::InternalSetFocus @000d5bfa), only when the focus moves.
    if (!bound() || !ready() || (binding_ == Binding::Patch) == doc_.isPerformance())
        return;
    const int synthSlot = synthSlotFor(slot);
    if (synthSlot < 0 || !live_[static_cast<std::size_t>(synthSlot)])
        return;
    link_->selectParam(synthSlot, focus.location, focus.module, focus.param);
}

void SynthSync::send(int synthSlot, const std::vector<g2::proto::Outgoing>& out)
{
    using namespace g2::proto;
    for (const auto& o : out) {
        if (!o.realtime) {
            link_->edit(synthSlot, o.molecules);
            continue;
        }
        for (const auto& m : o.molecules) {
            if (const auto* p = std::get_if<ParamChange>(&m))
                link_->setParam(synthSlot, static_cast<Location>(p->location), p->module, p->param, p->value, p->variation);
            else if (const auto* mc = std::get_if<MorphChange>(&m))
                link_->setMorph(synthSlot, static_cast<Location>(mc->location), mc->module, mc->param, mc->morph, mc->range,
                                mc->variation, true);
        }
    }
}

void SynthSync::push(int docSlot, const g2::live::Edit* edit, bool undo)
{
    const int slot = synthSlotFor(docSlot);
    if (slot < 0 || !live_[static_cast<std::size_t>(slot)])
        return;
    auto& live = *live_[static_cast<std::size_t>(slot)];
    const auto& now = doc_.slotPatch(docSlot);
    const auto name = nameFor(docSlot);
    const auto out = live.update(now, name, edit, undo, nowMs());
    if (live.needsUpload()) {
        // Modules or cables changed without the original's messages (none of
        // the editor's own edits does that): the whole patch again.
        link_->sendPatch(slot, now, name);
        live = g2::proto::LiveSlot::uploaded(now, name);
        return;
    }
    send(slot, out);
}

void SynthSync::pushPerformance()
{
    const auto* perf = doc_.performance();
    if (binding_ != Binding::Performance || perf == nullptr || audition_ == Audition::Performance)
        return;
    // CDialogPerformance::TrySendData @00039b36: the name (29) if it changed,
    // then the whole header (11) unless only the name did. P bubbles, no undo.
    const auto name = doc_.performanceName().toStdString();
    std::vector<g2::proto::Molecule> molecules;
    if (name != sentPerfName_) {
        sentPerfName_ = name;
        link_->edit(g2::proto::kSlotSynth, {g2::proto::NameDump{g2::proto::id::PerfName, name}});
    }
    if (!sameSettings(perf->header, sentPerf_)) {
        auto header = perf->header;
        // The synth's own slot names stay (27 renames a slot's patch).
        for (int s = 0; s < 4; ++s)
            header.slots[static_cast<std::size_t>(s)].patchName = sentPerf_.slots[static_cast<std::size_t>(s)].patchName;
        sentPerf_ = header;
        link_->edit(g2::proto::kSlotSynth, {g2::proto::SectionDump{g2::file::Section{g2::file::kPerfHeader, header, 0, {}}}});
    }
    // The slot shown is the synth's focused slot (09; the original's slot
    // buttons, CPerformance::CleanDirtyData @00100e5a).
    if (doc_.slot() != sentFocus_) {
        sentFocus_ = doc_.slot();
        link_->selectSlot(sentFocus_);
    }
    for (int s = 0; s < 4; ++s)
        sentPerf_.slots[static_cast<std::size_t>(s)].patchName = perf->header.slots[static_cast<std::size_t>(s)].patchName;
}

// ---- Synth -> editor ----------------------------------------------------------

void SynthSync::fromSynth(int synthSlot, const std::function<void(g2::Patch&)>& change)
{
    const int docSlot = docSlotFor(synthSlot);
    if (docSlot < 0 || !live_[static_cast<std::size_t>(synthSlot)])
        return;
    try {
        change(live_[static_cast<std::size_t>(synthSlot)]->synth());
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
    live_[static_cast<std::size_t>(synthSlot)] =
        g2::proto::LiveSlot(fresh, link_->state().slots[static_cast<std::size_t>(synthSlot)].name);
    doc_.performLiveOnSlot(docSlot, [&](g2::Patch& p) { p = fresh; });
}

void SynthSync::timerCallback()
{
    if (!link_)
        return;
    animateVirtualLeds();
    link_->tick();
    // A change of the voice count goes out 600 ms after the last one.
    if (ready() && bound())
        for (int docSlot = 0; docSlot < (doc_.isPerformance() ? 4 : 1); ++docSlot) {
            const int slot = synthSlotFor(docSlot);
            if (slot >= 0 && live_[static_cast<std::size_t>(slot)] && live_[static_cast<std::size_t>(slot)]->headerPending())
                send(slot, live_[static_cast<std::size_t>(slot)]->tick(doc_.slotPatch(docSlot), nowMs()));
        }
    if (ready() != wasReady_) {
        wasReady_ = ready();
        if (!wasReady_) {
            unbind(); // the connection went: what the synth has is unknown now
            audition_ = Audition::None;
            putBack_.reset();
        }
        sendChangeMessage();
    }
}

void SynthSync::statusChanged(g2::proto::Status)
{
    sendChangeMessage();
}

void SynthSync::synced()
{
    putBack_.reset(); // everything was read again: the mirror is the synth
    // A performance loaded from the synth's memory: the synth released it and
    // everything was read again; open it now that the slots are read too.
    if (pendingOpen_ == 4) {
        pendingOpen_ = -1;
        getPerformance();
    }
    // After a Perf switch the OS released the performance (1F) and everything was read again: a document that was live
    // becomes what the synth now plays (PerfSwitch.h).
    if (perfSwitchWasLive_) {
        const int open = openAfterPerfSwitch(performanceMode(), *perfSwitchWasLive_, link_->state().slotFocus);
        perfSwitchWasLive_.reset();
        if (open == kOpenPerformance)
            getPerformance();
        else if (open != kOpenNothing)
            getPatch(open);
    }
    sendChangeMessage();
}

void SynthSync::slotChanged(int slot)
{
    putBack_.reset(); // the synth reported after an upload: the mirror has what an audition put back
    if (slot == pendingOpen_) {
        pendingOpen_ = -1;
        getPatch(slot); // the patch just loaded from the synth's memory
        return;
    }
    slotReplacedOnSynth(slot);
    sendChangeMessage(); // slot names in the menus
}

void SynthSync::paramChanged(int slot, const g2::proto::ParamChange& c)
{
    // (Variation 0x7F: every variation, CPatch::HandleParamChange @000dc636.)
    fromSynth(slot, [c](g2::Patch& p) { g2::proto::applyToPatch(p, c); });
}

void SynthSync::morphChanged(int slot, const g2::proto::MorphChange& c)
{
    // One (parameter, group) assignment; range 0 removes it (CPatch::HandleMorphChange).
    fromSynth(slot, [c](g2::Patch& p) { g2::proto::applyToPatch(p, c); });
}

void SynthSync::variationChanged(int slot, g2::u8 variation)
{
    fromSynth(slot, [variation](g2::Patch& p) { p.header.activeVariation = variation; });
    if (docSlotFor(slot) == doc_.slot() && variation < g2::kUserVariations)
        doc_.setVariation(variation);
}

void SynthSync::loadFromBank(int slot, std::uint8_t bank, std::uint8_t prog, bool open)
{
    if (!ready() || !juce::isPositiveAndBelow(slot, 5))
        return;
    leaveAuditionFor(slot);
    pendingOpen_ = open ? slot : -1;
    link_->loadFromFlash(slot, bank, prog);
}

void SynthSync::storeToBank(int slot, std::uint8_t bank, std::uint8_t prog)
{
    if (!ready() || !juce::isPositiveAndBelow(slot, 5))
        return;
    endAudition(); // what is stored is the slot's own patch, never an auditioned one
    link_->storeToFlash(slot, bank, prog);
}

void SynthSync::flashNamesChanged(g2::u8)
{
    sendChangeMessage();
}

void SynthSync::flashResult(const g2::proto::FlashResult& r)
{
    const juce::String text = g2::proto::flashResultText(r.result);
    if (text.isNotEmpty() && onFlashProblem)
        onFlashProblem(text);
}

void SynthSync::performanceChanged()
{
    // The synth focused another slot (09 from its panel): the original opens
    // and shows that slot's patch (CPerformance @00100582).
    // (Not the focus of a performance that is only auditioned.)
    if (ready() && binding_ == Binding::Performance && doc_.isPerformance() && audition_ != Audition::Performance) {
        // Its settings (11) and name (29), changed on the synth, come into the document. What G2fresh knows the synth has
        // is updated first, so the document's change is not sent back to it (PerfSync.h).
        const auto& state = link_->state();
        if (state.perfName != sentPerfName_) {
            sentPerfName_ = state.perfName;
            doc_.setPerformanceName(juce::String(state.perfName));
        }
        if (!sameSettings(state.perfHeader, sentPerf_)) {
            takeSettings(sentPerf_, state.perfHeader);
            const auto synthHeader = state.perfHeader;
            doc_.editPerformance([&](g2::file::PerfHeader& h) { takeSettings(h, synthHeader); });
        }
        const int focus = link_->state().slotFocus;
        if (focus < 4 && focus != sentFocus_) {
            sentFocus_ = focus;
            doc_.setSlot(focus);
        }
    }
    sendChangeMessage();
}

void SynthSync::error(g2::u8 code)
{
    // The synth answered 7E: the original drops the connection and says why
    // (status 11 "Synth Exception", XMHostMidi's text).
    if (onAlert)
        onAlert(g2::proto::statusText(g2::proto::Status::SynthException), g2::proto::exceptionText(code));
}

void SynthSync::ledsChanged(int slot)
{
    if (docSlotFor(slot) == doc_.slot())
        ++ledGeneration_;
}

std::optional<int> SynthSync::ledValue(g2::Location location, std::uint8_t module, int group) const
{
    if (!ready() || !bound())
        return std::nullopt;
    const int slot = synthSlotFor(doc_.slot());
    if (slot < 0)
        return std::nullopt;
    const auto& s = link_->state().slots[static_cast<std::size_t>(slot)];
    for (std::size_t i = 0; i < ledMap_.single.size(); ++i) {
        const auto& g = ledMap_.single[i];
        if (g.location == location && g.module == module && g.group == group)
            return s.leds[i];
    }
    for (std::size_t i = 0; i < ledMap_.multi.size(); ++i) {
        const auto& g = ledMap_.multi[i];
        if (g.location == location && g.module == module && g.group == group)
            return s.meters[i];
    }
    return std::nullopt;
}

std::optional<std::uint8_t> SynthSync::lastMidiCc() const
{
    if (!patchConnected() || !link_->state().midiLearn)
        return std::nullopt;
    return link_->state().midiLearn->cc;
}

std::optional<std::uint8_t> SynthSync::currentVoices() const
{
    if (!patchConnected() || !link_->state().voicesReceived)
        return std::nullopt;
    return link_->state().voices[static_cast<std::size_t>(synthSlotFor(doc_.slot()))];
}

std::optional<g2::patchload::Load> SynthSync::reportedLoad() const
{
    if (!ready() || !bound())
        return std::nullopt;
    const int slot = synthSlotFor(doc_.slot());
    if (slot < 0)
        return std::nullopt;
    const auto& load = link_->state().slots[static_cast<std::size_t>(slot)].load;
    if (!load[0] || !load[1])
        return std::nullopt;
    std::optional<g2::patchload::Report> areas[2];
    for (int i = 0; i < 2; ++i) {
        std::vector<std::uint8_t> payload{load[static_cast<std::size_t>(i)]->isVA};
        payload.insert(payload.end(), load[static_cast<std::size_t>(i)]->counters.begin(),
                       load[static_cast<std::size_t>(i)]->counters.end());
        areas[i] = g2::patchload::parseReport(payload);
    }
    if (!areas[0] || !areas[1])
        return std::nullopt;
    const auto& va = areas[0]->area == g2::Location::Va ? areas[0] : areas[1];
    const auto& fx = areas[0]->area == g2::Location::Va ? areas[1] : areas[0];
    return g2::patchload::fromReports(va->spec, fx->spec);
}

void SynthSync::animateVirtualLeds()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (kind_ != Kind::Virtual || !ready() || !bound() || now - lastVirtualLeds_ < 50.0)
        return;
    lastVirtualLeds_ = now;
    auto* local = dynamic_cast<g2::proto::LocalLink*>(link_.get());
    auto* emulator = local != nullptr ? local->emulator() : nullptr;
    const int slot = synthSlotFor(doc_.slot());
    if (emulator == nullptr || slot < 0)
        return;
    const double t = now / 1000.0;
    std::vector<std::uint8_t> leds(ledMap_.single.size());
    for (std::size_t i = 0; i < leds.size(); ++i)
        leds[i] = static_cast<std::uint8_t>(std::fmod(t * 2.0 + static_cast<double>(i) * 0.37, 1.0) < 0.5 ? 3 : 0);
    std::vector<std::uint16_t> meters(ledMap_.multi.size());
    for (std::size_t i = 0; i < meters.size(); ++i) {
        const auto& g = ledMap_.multi[i];
        const auto* m = doc_.patch().area(g.location).find(g.module);
        const auto* panel = m ? ModulePainter::panelFor(*m) : nullptr;
        int count = 0;
        bool vu = false;
        if (panel)
            for (const auto& e : panel->elements)
                if ((e.kind == "Led" || e.kind == "MiniVU") && e.groupId == g.group) {
                    ++count;
                    vu |= e.kind == "MiniVU";
                }
        if (vu) // a level swinging up to clip now and then
            meters[i] = static_cast<std::uint16_t>(juce::jlimit(0.0, 130.0, 64.0 + 66.0 * std::sin(t * 2.2 + static_cast<double>(i))));
        else    // a strip: the lit LED walks along
            meters[i] = static_cast<std::uint16_t>(count > 0 ? static_cast<int>(t * 8.0) % count : 0);
    }
    if (!leds.empty())
        emulator->sendLeds(static_cast<g2::u8>(slot), leds);
    if (!meters.empty())
        emulator->sendMeters(static_cast<g2::u8>(slot), meters);
}

void SynthSync::patchEdited(int slot, const g2::proto::Molecule& m)
{
    // Only what edits a patch: the load report the synth sends after each upload (72) is not a change of the
    // document (round-6 hand tests: an untouched new patch asked to be saved).
    if (!g2::proto::isPatchEdit(m))
        return;
    fromSynth(slot, [&m](g2::Patch& p) { g2::proto::applyToPatch(p, m); });
}

} // namespace g2ui
