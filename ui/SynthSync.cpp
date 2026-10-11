#include "SynthSync.h"

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
g2::file::PerfHeader settingsOf(g2::file::PerfHeader h)
{
    for (auto& slot : h.slots)
        slot.patchName.clear();
    h.focusedSlot = 0;
    return h;
}

bool sameSettings(const g2::file::PerfHeader& a, const g2::file::PerfHeader& b)
{
    auto bytes = [](const g2::file::PerfHeader& h) {
        return g2::file::encodeSection(g2::file::Section{g2::file::kPerfHeader, settingsOf(h), 0, {}});
    };
    return bytes(a) == bytes(b);
}

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
    unbind();
    link_->sendPerformance(*doc_.performance(), doc_.performanceName().toStdString(), true);
    bindPerformance();
    sendChangeMessage();
}

void SynthSync::getPatch(int slot)
{
    if (!ready() || !juce::isPositiveAndBelow(slot, 4))
        return;
    unbind();
    const auto& s = link_->state().slots[static_cast<std::size_t>(slot)];
    doc_.loadPatch(s.patch, juce::String(s.name));
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
    unbind();
    doc_.loadPerformance(link_->state().performance(), juce::String(link_->state().perfName));
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
    if (binding_ != Binding::Performance || perf == nullptr)
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
    // A performance loaded from the synth's memory: the synth released it and
    // everything was read again; open it now that the slots are read too.
    if (pendingOpen_ == 4) {
        pendingOpen_ = -1;
        getPerformance();
    }
    sendChangeMessage();
}

void SynthSync::slotChanged(int slot)
{
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
    pendingOpen_ = open ? slot : -1;
    link_->loadFromFlash(slot, bank, prog);
}

void SynthSync::storeToBank(int slot, std::uint8_t bank, std::uint8_t prog)
{
    if (!ready() || !juce::isPositiveAndBelow(slot, 5))
        return;
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
    if (ready() && binding_ == Binding::Performance && doc_.isPerformance()) {
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
    fromSynth(slot, [&m](g2::Patch& p) { g2::proto::applyToPatch(p, m); });
}

} // namespace g2ui
