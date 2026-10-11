#include "g2/proto/client.hpp"

#include "g2/proto/sections.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace g2::proto {

std::uint64_t SteadyClock::nowMs() const
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

const char* statusText(Status s)
{
    switch (s) {
    case Status::NoDevice: return "No synth";
    case Status::Looking: return "Looking...";
    case Status::StillLooking: return "Still looking...";
    case Status::UnsupportedModel: return "Unsupported Model";
    case Status::VersionMismatch: return "Version mismatch";
    case Status::NewDevice: return "New device found";
    case Status::Corrupted: return "Version Corrupted";
    case Status::LostContact: return "Lost Contact / Check USB cables";
    case Status::UpdateMode: return "Update Mode";
    case Status::SynthException: return "Synth Exception";
    case Status::MajorError: return "Major Error";
    case Status::Connected: return "Connected";
    }
    return "";
}

bool isFatal(Status s)
{
    return s != Status::NoDevice && s != Status::Looking && s != Status::StillLooking && s != Status::Connected;
}

const char* exceptionText(u8 code)
{
    switch (code) {
    case 3: return "Error in synth.";
    case 4: return "Down link check sum error.";
    case 5: return "Stream execute error. Synth may be corrupted.";
    case 6: return "Unfinished bubble error.";
    default: return "";
    }
}

const char* flashResultText(u8 result)
{
    // CFlashData::HandleFlashCommandResult 0x11b062: 0, 2 and 5 are done; others say nothing.
    switch (result) {
    case 1: return "Can't load from flash";
    case 3: return "Flash memory full";
    case 4: return "Memory protect on";
    default: return "";
    }
}

Client::Client(Transport& transport, Clock& clock) : Client(transport, clock, Options{}) {}

Client::Client(Transport& transport, Clock& clock, Options options)
    : transport_(transport), clock_(clock), options_(options)
{
    transport_.setSink(this);
}

Client::~Client() { transport_.setSink(nullptr); }

std::string Client::statusLine() const
{
    if (status_ != Status::Connected)
        return statusText(status_);
    char version[16];
    std::snprintf(version, sizeof version, "V%u.%02u", state_.version.firmware / 100u, state_.version.firmware % 100u);
    return std::string(state_.version.modelName()) + " " + version;
}

// ---- Status -----------------------------------------------------------------------

void Client::setStatus(Status s)
{
    if (s == status_)
        return;
    status_ = s;
    if (listener_)
        listener_->statusChanged(s);
}

void Client::fail(Status s)
{
    // Clavia destroys the port: nothing more is sent until the device comes again.
    cleaner_.clear();
    user_.clear();
    outstanding_.reset();
    versionSentAt_.reset();
    synced_ = false;
    setStatus(s);
}

void Client::restart()
{
    cleaner_.clear();
    user_.clear();
    outstanding_.reset();
    expectedBulk_.reset();
    synced_ = false;
    if (!devicePresent_) {
        setStatus(Status::NoDevice);
        return;
    }
    setStatus(Status::Looking);
    sendVersionRequest();
}

void Client::deviceArrived()
{
    devicePresent_ = true;
    restart();
}

void Client::deviceRemoved()
{
    devicePresent_ = false;
    fail(Status::NoDevice);
}

void Client::sendVersionRequest()
{
    sendFrame(versionRequest());
    versionSentAt_ = clock_.nowMs();
}

// ---- Tick and timeouts ------------------------------------------------------------

void Client::tick()
{
    transport_.poll();
    const auto now = clock_.nowMs();
    if (status_ == Status::Looking || status_ == Status::StillLooking) {
        // No reply to the version request: say so and ask again (§5.2).
        if (versionSentAt_ && now - *versionSentAt_ >= options_.versionRetryMs) {
            setStatus(Status::StillLooking);
            sendVersionRequest();
        }
        return;
    }
    if (status_ != Status::Connected)
        return;
    const auto since = std::max(lastSend_, lastReceive_);
    if (outstanding_ && now >= since && now - since >= options_.replyTimeoutMs) {
        if (outstanding_->retriesLeft > 0) {
            --outstanding_->retriesLeft;
            sendFrame(outstanding_->frames[outstanding_->next]);
        } else {
            fail(Status::LostContact);
            return;
        }
    }
    sendNext();
}

// ---- Sending ----------------------------------------------------------------------

void Client::sendFrame(const std::vector<u8>& frame)
{
    lastSend_ = clock_.nowMs();
    transport_.send(frame);
}

void Client::sendRealtime(const Bubble& b)
{
    if (status_ != Status::Connected)
        return;
    if (!user_.empty()) {
        user_.push_back({[b] { return b; }, {}, true});
        return;
    }
    for (const auto& frame : framesFor(b))
        sendFrame(frame);
}

void Client::sendNext()
{
    // Stop-and-wait (§4.3): one bubble at a time; sync traffic before edits.
    while (status_ == Status::Connected && !outstanding_) {
        Pending p;
        if (!cleaner_.empty()) {
            p = std::move(cleaner_.front());
            cleaner_.pop_front();
        } else if (auto dirty = synced_ ? dirtyRequest() : std::nullopt) {
            p = std::move(*dirty);
        } else if (!user_.empty()) {
            p = std::move(user_.front());
            user_.pop_front();
        } else {
            break;
        }
        const auto bubble = p.make();
        if (!bubble)
            continue;
        if (p.realtime) {
            for (const auto& frame : framesFor(*bubble))
                sendFrame(frame);
            continue;
        }
        Outstanding o;
        o.frames = framesFor(*bubble);
        o.handler = std::move(p.handler);
        o.retriesLeft = options_.retries;
        if (o.frames.empty())
            continue;
        outstanding_ = std::move(o);
        sendFrame(outstanding_->frames.front());
    }
}

void Client::enqueueCleaner(std::function<std::optional<Bubble>()> make, ReplyHandler handler)
{
    cleaner_.push_back({std::move(make), std::move(handler), false});
}

void Client::enqueueUser(std::function<std::optional<Bubble>()> make, ReplyHandler handler)
{
    user_.push_back({std::move(make), std::move(handler), false});
    sendNext();
}

Bubble Client::slotBubble(int slot, std::vector<Molecule> molecules) const
{
    const auto s = static_cast<u8>(slot);
    return Bubble::patch(s, state_.slots[static_cast<std::size_t>(slot)].session, std::move(molecules));
}

Bubble Client::perfBubble(std::vector<Molecule> molecules) const
{
    return Bubble::performance(state_.perfSession, std::move(molecules));
}

// ---- The sync (§6.3) --------------------------------------------------------------

void Client::enqueueSync(bool withLock)
{
    synced_ = false;
    if (withLock)
        enqueueCleaner([] { return Bubble::synth({EditorSync{true}}); });
    enqueueCleaner([] { return Bubble::synth({SessionRequest{kSlotSynth}}); });
    enqueueCleaner([] { return Bubble::synth({Request{id::SynthDataRequest}}); });
    enqueueCleaner([] { return Bubble::synth({Request{id::MidiLearnRequest}}); });
    enqueueCleaner([this] { return perfBubble({Request{id::PerfHeaderRequest}}); },
                   [this](const auto&) {
                       if (listener_)
                           listener_->performanceChanged();
                   });
    enqueueCleaner([this] { return perfBubble({Request{id::GlobalPageFocusRequest}}); });
    if (options_.askSlotFocus)
        enqueueCleaner([this] { return perfBubble({Request{id::SlotFocusRequest}}); });
    else
        enqueueCleaner([this] { return perfBubble({SlotFocus{4}}); });
    for (int slot = 0; slot < kSlots; ++slot)
        enqueueSlotSync(slot);
    enqueueCleaner([] { return Bubble::synth({Request{id::VoicesRequest}}); });
    enqueueCleaner([] { return Bubble::synth({Request{id::ClockInfoRequest}}); });
    enqueueCleaner([this] { return perfBubble({Request{id::GlobalKnobMapRequest}}); },
                   [this](const auto&) {
                       if (listener_)
                           listener_->synthChanged();
                   });
    if (options_.readFlashNames) {
        flashScratch_[0].clear();
        flashScratch_[1].clear();
        enqueueFlashNames(0, 0, 0, false);
    } else {
        enqueueSyncDone();
    }
}

void Client::enqueueSyncDone()
{
    enqueueCleaner([] { return Bubble::synth({EditorSync{false}}); },
                   [this](const auto&) {
                       synced_ = true;
                       if (listener_)
                           listener_->synced();
                   });
}

void Client::enqueueSlotSync(int slot, bool askSession)
{
    const auto s = static_cast<u8>(slot);
    if (askSession)
        enqueueCleaner([s] { return Bubble::synth({SessionRequest{s}}); });
    // CPatch::CleanDirtyData @000d601e asks only for what is still dirty. A
    // real G2's 3C reply carries the page focus (2D) but not the name (27),
    // the current notes (69) or the textpad (6F), so 28, 68 and 6E follow it
    // and 58 does not (usb-crosscheck H3).
    enqueueCleaner([this, slot] { return slotBubble(slot, {Request{id::CompletePatchRequest}}); },
                   [this, slot](const std::vector<Molecule>& reply) {
                       auto& d = dirty_[static_cast<std::size_t>(slot)];
                       d.name = d.page = d.notes = d.textpad = d.focus = true;
                       for (const auto& m : reply) {
                           const u8 mid = idOf(m);
                           d.name = d.name && mid != id::PatchName;
                           d.page = d.page && mid != id::PageFocus;
                           d.notes = d.notes && mid != id::CurrentNotes;
                           d.textpad = d.textpad && mid != id::Textpad;
                       }
                       if (listener_)
                           listener_->slotChanged(slot);
                   });
    auto ifDirty = [this, slot](bool SlotDirty::*flag, Molecule m) {
        enqueueCleaner([this, slot, flag, m]() -> std::optional<Bubble> {
            auto& d = dirty_[static_cast<std::size_t>(slot)];
            if (!(d.*flag))
                return std::nullopt;
            d.*flag = false;
            return slotBubble(slot, {m});
        });
    };
    ifDirty(&SlotDirty::name, Request{id::PatchNameRequest});
    ifDirty(&SlotDirty::page, Request{id::PageFocusRequest});
    ifDirty(&SlotDirty::notes, Request{id::CurrentNotesRequest});
    ifDirty(&SlotDirty::textpad, Request{id::TextpadRequest});
    // The load report also opens the LED stream (§10): reset it first.
    enqueueCleaner([this, slot] {
        state_.slots[static_cast<std::size_t>(slot)].load = {};
        dirty_[static_cast<std::size_t>(slot)].loadVa = false;
        return slotBubble(slot, {LocationRequest{id::PatchLoadRequest, 1}});
    });
    enqueueCleaner([this, slot] {
        dirty_[static_cast<std::size_t>(slot)].loadFx = false;
        return slotBubble(slot, {LocationRequest{id::PatchLoadRequest, 0}});
    });
    enqueueCleaner([this, slot] {
        dirty_[static_cast<std::size_t>(slot)].flush = false;
        return slotBubble(slot, {Request{id::FlushBlink}});
    });
    enqueueCleaner([this, slot] {
        dirty_[static_cast<std::size_t>(slot)].focus = false;
        return slotBubble(slot, {Request{id::ParamFocusRequest}});
    });
}

void Client::enqueuePerformanceResync()
{
    // CSynth::HandlePerformanceRelease @0011d7ac: the performance and every
    // slot again, without 7D 01. A real G2 sends the slots' sessions with the
    // 1F, so the slots are read from 3C on (usb-crosscheck H2, capture-003/005).
    synced_ = false;
    enqueueCleaner([] { return Bubble::synth({SessionRequest{kSlotSynth}}); });
    enqueueCleaner([this] { return perfBubble({Request{id::PerfHeaderRequest}}); },
                   [this](const auto&) {
                       if (listener_)
                           listener_->performanceChanged();
                   });
    enqueueCleaner([this] { return perfBubble({Request{id::GlobalPageFocusRequest}}); });
    for (int slot = 0; slot < kSlots; ++slot)
        enqueueSlotSync(slot, !state_.slots[static_cast<std::size_t>(slot)].sessionKnown);
    enqueueSyncDone();
}

std::optional<Client::Pending> Client::dirtyRequest()
{
    // Between bubbles, as the original's idle pass (CSynth::CleanDirtyData
    // @0011f394 -> CPatch::CleanDirtyData): a slot whose modules or cables
    // changed asks for its load again (71 VA, else 71 FX; CPatchLoad::
    // SendLoadRequest @000ee866), every load report is followed by a 70, and
    // a patch read anew by one 2E.
    for (int slot = 0; slot < kSlots; ++slot) {
        auto& d = dirty_[static_cast<std::size_t>(slot)];
        std::optional<Molecule> m;
        if (d.loadVa) {
            d.loadVa = false;
            m = LocationRequest{id::PatchLoadRequest, 1};
        } else if (d.loadFx) {
            d.loadFx = false;
            m = LocationRequest{id::PatchLoadRequest, 0};
        } else if (d.flush) {
            d.flush = false;
            m = Request{id::FlushBlink};
        } else if (d.focus && state_.slots[static_cast<std::size_t>(slot)].load[0]
                   && state_.slots[static_cast<std::size_t>(slot)].load[1]) { // after the load report's 70
            d.focus = false;
            m = Request{id::ParamFocusRequest};
        }
        if (m)
            return Pending{[this, slot, m = *m] { return std::optional<Bubble>(slotBubble(slot, {m})); }, {}, false};
    }
    return std::nullopt;
}

void Client::markLoadDirty(int slot, const Molecule& m)
{
    // The molecules whose local handlers add or remove a module or cable from
    // the patch load, or change its DSP code (CPatchLoad::UpdateCriticalResource
    // @000ee488): the area's load is no longer reported.
    if (slot < 0 || slot >= kSlots)
        return;
    std::optional<u8> loc;
    if (const auto* n = std::get_if<ModuleNew>(&m))
        loc = n->location;
    else if (const auto* d = std::get_if<ModuleDelete>(&m))
        loc = d->location;
    else if (const auto* u = std::get_if<Uprate>(&m))
        loc = u->location;
    else if (const auto* c = std::get_if<ModeChange>(&m))
        loc = c->location;
    else if (const auto* e = std::get_if<CableEdit>(&m); e && e->id != id::CableRecolor)
        loc = static_cast<u8>(e->va ? 1 : 0);
    else if (const auto* sd = std::get_if<SectionDump>(&m); sd && sd->section.id == file::kCableList)
        if (const auto* l = std::get_if<file::CableList>(&sd->section.payload))
            loc = l->location;
    auto& d = dirty_[static_cast<std::size_t>(slot)];
    if (loc == 1)
        d.loadVa = true;
    else if (loc == 0)
        d.loadFx = true;
}

void Client::enqueueFlashNames(u8 type, u8 bank, u8 prog, bool front)
{
    Pending p{[type, bank, prog] { return Bubble::synth({FlashCommand{id::FlashDataRequest, type, bank, prog}}); },
              [this, type](const std::vector<Molecule>& reply) {
                  // Follow the list (§7, 16): 03 sets the entry, 01 the prog, 02 skips
                  // one, a name takes the current entry; 05 asks again from the next.
                  std::pair<u8, u8> cursor{0, 0};
                  bool more = false;
                  for (const auto& m : reply) {
                      const auto* d = std::get_if<FlashData>(&m);
                      if (!d)
                          continue;
                      for (const auto& item : d->items) {
                          using K = FlashItem::Kind;
                          switch (item.kind) {
                          case K::SetEntry: cursor = {item.bank, item.prog}; break;
                          case K::SetProg: cursor.second = item.prog; break;
                          case K::Empty: ++cursor.second; break;
                          case K::Name:
                              flashScratch_[type].push_back({cursor.first, cursor.second, item.name, item.category});
                              ++cursor.second;
                              break;
                          case K::EndOfChunk: more = true; break;
                          case K::EndOfList: break;
                          }
                      }
                  }
                  // A chunk is followed at once by the next request, before the rest of the sync.
                  if (more) {
                      const u8 bank = cursor.second > 127 ? static_cast<u8>(cursor.first + 1) : cursor.first;
                      const u8 prog = cursor.second > 127 ? u8{0} : cursor.second;
                      enqueueFlashNames(type, bank, prog, true);
                      return;
                  }
                  state_.flash[type] = std::move(flashScratch_[type]);
                  flashScratch_[type].clear();
                  if (observer_)
                      observer_->stateReplaced();
                  if (listener_)
                      listener_->flashNamesChanged(type);
                  // The performance list follows, the last step before 7D 00.
                  if (type == 0)
                      enqueueFlashNames(1, 0, 0, true);
              },
              false};
    if (front)
        cleaner_.push_front(std::move(p));
    else
        cleaner_.push_back(std::move(p));
    // 7D 00 follows the performance list: queue it once, after the first request.
    if (!front && type == 0)
        enqueueSyncDone();
}

void Client::resync()
{
    if (status_ != Status::Connected)
        return;
    enqueueSync(true);
    sendNext();
}

void Client::resyncSlot(int slot)
{
    if (status_ != Status::Connected || slot < 0 || slot >= kSlots)
        return;
    enqueueSlotSync(slot);
    enqueueSyncDone(); // "7D 00 is still sent at the end" (§6.3)
    sendNext();
}

// ---- Receiving --------------------------------------------------------------------

void Client::interruptPacket(std::span<const u8> packet)
{
    lastReceive_ = clock_.nowMs();
    const auto i = parseInterrupt(packet);
    switch (i.kind) {
    case Interrupt::Kind::Embedded:
        handleMessage(i.message);
        break;
    case Interrupt::Kind::Extended:
        expectedBulk_ = i.length;
        break;
    case Interrupt::Kind::Ignored:
        break;
    }
}

void Client::bulkIn(std::span<const u8> data)
{
    lastReceive_ = clock_.nowMs();
    if (!expectedBulk_)
        return; // not announced
    expectedBulk_.reset();
    // Clavia takes the first non-empty read as the whole message.
    handleMessage(data);
}

void Client::handleMessage(std::span<const u8> bytes)
{
    const auto m = parseDeviceMessage(bytes);
    if (!m) {
        ++dropped_;
        return;
    }
    if (m->kind == DeviceMessage::Kind::Version) {
        handleVersion(*m);
        return;
    }
    if (status_ != Status::Connected)
        return;
    if (m->isResponse() && outstanding_ && !m->isBlink())
        handleReply(*m);
    else
        handleUnsolicited(*m);
}

void Client::handleVersion(const DeviceMessage& m)
{
    const auto v = decodeVersionInfo(m.body);
    if (status_ == Status::Connected) {
        // Identical: ignored. Different: another device (§5.2).
        if (!v || !(*v == state_.version))
            fail(Status::NewDevice);
        return;
    }
    if (status_ != Status::Looking && status_ != Status::StillLooking)
        return;
    versionSentAt_.reset();
    if (!v || !v->supportedModel()) {
        if (v && v->marker == 0x0A && (v->model == VersionInfo::Rack || v->model == VersionInfo::Native))
            fail(Status::VersionMismatch);
        else
            fail(Status::UnsupportedModel);
        return;
    }
    if (v->mode != 0) {
        fail(Status::UpdateMode);
        return;
    }
    if (v->protocol != 0x0012) {
        fail(Status::VersionMismatch);
        return;
    }
    state_ = SynthState{};
    state_.version = *v;
    if (observer_)
        observer_->stateReplaced();
    setStatus(Status::Connected);
    enqueueSync(true);
    sendNext();
}

void Client::handleReply(const DeviceMessage& m)
{
    auto& o = *outstanding_;
    auto molecules = decode(m.body);
    // A reply ends at 7F / 7E (§4.1).
    bool exception = false;
    u8 code = 0;
    for (std::size_t i = 0; i < molecules.size(); ++i) {
        if (std::holds_alternative<Ack>(molecules[i])) {
            molecules.resize(i);
            break;
        }
        if (const auto* e = std::get_if<Exception>(&molecules[i])) {
            exception = true;
            code = e->code;
            molecules.resize(i + 1);
            break;
        }
    }
    o.reply.insert(o.reply.end(), molecules.begin(), molecules.end());
    if (exception) {
        if (listener_)
            listener_->error(code);
        if (options_.exceptionsAreFatal) {
            fail(Status::SynthException);
            return;
        }
    }
    // A request bubble of several messages: each message is answered.
    if (!exception && ++o.next < o.frames.size()) {
        sendFrame(o.frames[o.next]);
        return;
    }
    // Session numbers in replies to the slot's own requests are adopted.
    if (m.slot() < kSlots && !m.voidSession()) {
        auto& slot = state_.slots[m.slot()];
        slot.session = m.session & kSessionMask;
        slot.sessionKnown = true;
        applied(kSlotSynth, SessionNumber{id::SessionDump, m.slot(), slot.session});
    }
    auto done = std::move(o);
    outstanding_.reset();
    route(m.slot(), done.reply, true);
    if (done.handler)
        done.handler(done.reply);
    sendNext();
}

void Client::handleUnsolicited(const DeviceMessage& m)
{
    const u8 slot = m.slot();
    const auto molecules = decode(m.body);
    if (slot < kSlots) {
        auto& s = state_.slots[slot];
        if (m.isBlink()) {
            // LEDs only once the slot's patch is read and both load reports
            // arrived (CSynthPort::InterruptHandleBlinkData 0x1041ea).
            if (!s.load[0] || !s.load[1])
                return;
            for (const auto& b : molecules) {
                applyToSlot(s, b);
                applied(slot, b);
            }
            if (listener_)
                listener_->ledsChanged(slot);
            return;
        }
        // A patch bubble with another session: the slot changed on the synth (§5.3).
        if (!m.voidSession() && (m.session & kSessionMask) != s.session) {
            s.session = m.session & kSessionMask;
            s.sessionKnown = true;
            applied(kSlotSynth, SessionNumber{id::SessionDump, slot, s.session});
            resyncSlot(slot);
            return;
        }
    }
    route(slot, molecules, false);
}

// Clavia routes every molecule by its destination, not by the header of the
// message it came in (CSynth::RouteBubble @0011f940, CMoleculeFactory::
// GetDestination): a patch upload is answered by 36 on the slot's header, a
// load from flash by 38 or 1F (and 0D) inside the reply to 0A, and these reach
// the synth's handlers all the same (usb-crosscheck M1, H2).
void Client::route(u8 slot, const std::vector<Molecule>& molecules, bool fromReply)
{
    if (slot >= kSlots) {
        routeSynth(molecules, fromReply);
        return;
    }
    auto& s = state_.slots[slot];
    std::vector<Molecule> synthLevel;
    // Sections replace the slot's (a download, or a partial refresh).
    PatchAssembler sections;
    bool anySection = false;
    for (const auto& m : molecules) {
        if (const auto* d = std::get_if<SectionDump>(&m)) {
            if (d->section.id == file::kPerfHeader || d->section.id == file::kGlobalKnobMap) {
                synthLevel.push_back(m);
                continue;
            }
            if (!anySection)
                sections.reset(&s.patch);
            anySection = sections.add(d->section) || anySection;
            continue;
        }
        if (const auto* r = std::get_if<Request>(&m); r && r->id == id::KnobMapChanged) {
            // CPatch::HandleBarbMapChangeNotify @000d3918: the slot's knob map
            // changed on the synth; ask for it again (63).
            enqueueCleaner([this, slot] { return slotBubble(slot, {Request{id::KnobMapRequest}}); },
                           [this, slot](const std::vector<Molecule>& reply) {
                               if (listener_)
                                   for (const auto& k : reply)
                                       if (const auto* d = std::get_if<SectionDump>(&k); d && d->section.id == file::kKnobMap)
                                           listener_->patchEdited(slot, k);
                           });
            sendNext();
            continue;
        }
        if (!applyToSlot(s, m)) {
            synthLevel.push_back(m);
            continue;
        }
        applied(slot, m);
        // Every load report is followed by a 70 (CPatch::SetDspReportedLoad @000d4a74).
        if (std::holds_alternative<PatchLoad>(m))
            dirty_[slot].flush = true;
        if (fromReply)
            continue;
        markLoadDirty(slot, m);
        if (!listener_)
            continue;
        if (const auto* p = std::get_if<ParamChange>(&m))
            listener_->paramChanged(slot, *p);
        else if (const auto* v = std::get_if<VariationSelect>(&m))
            listener_->variationChanged(slot, v->variation);
        else if (const auto* f = std::get_if<ParamFocus>(&m))
            listener_->paramFocused(slot, *f);
        else if (const auto* mc = std::get_if<MorphChange>(&m))
            listener_->morphChanged(slot, *mc);
        else
            listener_->patchEdited(slot, m);
    }
    if (anySection) {
        try {
            s.patch = sections.patch();
        } catch (const std::exception&) {
            fail(Status::Corrupted);
            return;
        }
        if (observer_)
            observer_->slotReplaced(slot);
        if (!fromReply && listener_)
            listener_->slotChanged(slot);
    }
    if (!synthLevel.empty())
        routeSynth(synthLevel, fromReply);
}

void Client::routeSynth(const std::vector<Molecule>& molecules, bool fromReply)
{
    bool synthNews = false, perfNews = false, resyncAll = false;
    std::vector<int> released;
    for (const auto& m : molecules) {
        if (const auto* r = std::get_if<SessionNumber>(&m)) {
            applied(kSlotSynth, m);
            if (r->slot < kSlots) {
                auto& s = state_.slots[r->slot];
                s.session = r->session & kSessionMask;
                s.sessionKnown = true;
                // 38: the synth replaced the patch in that slot (a load from its
                // memory, the panel): read it again, whether the release came
                // in a reply or on its own (CSynth::HandleSessionPatchRelease
                // @0011df1c). A 36 only gives the session: after an upload the
                // synth has what the editor sent.
                if (r->id == id::PatchRelease)
                    released.push_back(r->slot);
            } else {
                state_.perfSession = r->session & kSessionMask;
                state_.perfSessionKnown = true;
            }
            continue;
        }
        if (const auto* r = std::get_if<PerformanceRelease>(&m)) {
            applied(kSlotSynth, m);
            state_.perfSession = r->session & kSessionMask;
            state_.perfSessionKnown = true;
            resyncAll = true; // CSynth::HandlePerformanceRelease @0011d7ac
            continue;
        }
        if (const auto* l = std::get_if<MidiLearn>(&m)) {
            state_.midiLearn = *l;
            applied(kSlotSynth, m);
            if (!fromReply && listener_)
                listener_->midiLearned(*l);
            continue;
        }
        if (const auto* r = std::get_if<Request>(&m); r && r->id == id::GlobalKnobMapChanged) {
            // CPerformance::HandlePerfBarbMapChangeNotify @000ffeda: the global
            // knob map changed; ask for it again (5E).
            enqueueCleaner([this] { return perfBubble({Request{id::GlobalKnobMapRequest}}); },
                           [this](const auto&) {
                               if (listener_)
                                   listener_->performanceChanged();
                           });
            sendNext();
            continue;
        }
        if (applyToSynth(state_, m)) {
            applied(kSlotSynth, m);
            const auto mid = idOf(m);
            (mid == id::SynthData || mid == id::Voices || mid == id::ClockInfo || mid == id::FlashUsage ? synthNews
                                                                                                        : perfNews) = true;
        }
    }
    if (resyncAll) {
        enqueuePerformanceResync();
        sendNext();
    } else {
        for (const int slot : released)
            resyncSlot(slot);
    }
    if (!fromReply && listener_) {
        if (synthNews)
            listener_->synthChanged();
        if (perfNews)
            listener_->performanceChanged();
    }
}

// ---- Uploads and edits ------------------------------------------------------------

void Client::sendPatch(int slot, const Patch& patch, const std::string& name)
{
    if (slot < 0 || slot >= kSlots)
        return;
    const auto bubble = patchUpload(static_cast<u8>(slot), patch, name);
    // The mirror takes the patch when it goes out, as the original applies a
    // bubble when it sends it (CPatch::UserBubbleOutput @000d5e74): a read of
    // the slot still queued before it must not undo it. Then the synth
    // reports the load, and the editor sends 70 and, once, 2E (as after a
    // real G2's upload; CPatch::CleanDirtyData).
    enqueueUser([this, slot, bubble, patch, name] {
        auto& s = state_.slots[static_cast<std::size_t>(slot)];
        s.patch = patch;
        s.name = name;
        s.load = {};
        dirty_[static_cast<std::size_t>(slot)].focus = true;
        if (observer_)
            observer_->slotReplaced(slot);
        return bubble;
    });
}

void Client::sendPerformance(const Performance& perf, const std::string& name, std::optional<bool> performanceMode)
{
    // CSynth::CleanDirtyData @0011f394: when the synth's performance mode is
    // not the one the upload wants (off for a patch sent alone, the original's
    // "void" performance), a 03 with only that changed goes first.
    enqueueUser([this, performanceMode]() -> std::optional<Bubble> {
        if (!performanceMode || state_.settings.perfMode == *performanceMode)
            return std::nullopt;
        auto settings = state_.settings;
        settings.perfMode = *performanceMode;
        state_.settings.perfMode = *performanceMode;
        applied(kSlotSynth, SynthData{settings});
        return Bubble::synth({SynthData{settings}});
    });
    const auto bubble = performanceUpload(perf, name);
    enqueueUser([this, bubble, perf, name] {
        state_.setPerformance(perf, name);
        if (observer_)
            observer_->stateReplaced();
        for (int slot = 0; slot < kSlots; ++slot) {
            state_.slots[static_cast<std::size_t>(slot)].load = {};
            dirty_[static_cast<std::size_t>(slot)].focus = true;
        }
        return bubble;
    });
}

void Client::applyEdit(int slot, const Molecule& m)
{
    if (slot >= 0 && slot < kSlots) {
        applyToSlot(state_.slots[static_cast<std::size_t>(slot)], m);
        applied(static_cast<u8>(slot), m);
    }
}

void Client::editBubble(int slot, std::vector<Molecule> molecules)
{
    if (slot == kSlotSynth) {
        // A performance edit (11, 29, 07, 0F, 3F, 1C...): one P bubble
        // (CPerformance::UserBubbleOutput @0010040c), applied at once.
        for (const auto& m : molecules)
            if (applyToSynth(state_, m))
                applied(kSlotSynth, m);
        enqueueUser([this, molecules = std::move(molecules)] { return perfBubble(molecules); });
        return;
    }
    if (slot < 0 || slot >= kSlots)
        return;
    for (const auto& m : molecules)
        applyEdit(slot, m);
    enqueueUser([this, slot, molecules = std::move(molecules)] {
        for (const auto& m : molecules)
            markLoadDirty(slot, m);
        return slotBubble(slot, molecules);
    });
}

void Client::edit(int slot, std::vector<Molecule> molecules) { editBubble(slot, std::move(molecules)); }

void Client::request(Bubble bubble, ReplyHandler handler)
{
    enqueueUser([bubble = std::move(bubble)] { return bubble; }, std::move(handler));
}

void Client::setParam(int slot, Location loc, u8 module, u8 param, u8 value, u8 variation)
{
    if (slot < 0 || slot >= kSlots)
        return;
    const ParamChange m{static_cast<u8>(loc), module, param, value, variation};
    applyEdit(slot, m);
    sendRealtime(Bubble::realtimeFor(static_cast<u8>(slot), state_.slots[static_cast<std::size_t>(slot)].session, {m}));
}

void Client::selectParam(int slot, Location loc, u8 module, u8 param)
{
    if (slot < 0 || slot >= kSlots)
        return;
    const ParamFocus m{0, static_cast<u8>(loc), module, param};
    applyEdit(slot, m);
    sendRealtime(Bubble::realtimeFor(static_cast<u8>(slot), state_.slots[static_cast<std::size_t>(slot)].session, {m}));
}

void Client::setMorph(int slot, Location loc, u8 module, u8 param, u8 group, int range, u8 variation, bool dragging)
{
    if (slot < 0 || slot >= kSlots)
        return;
    const MorphChange m{static_cast<u8>(loc), module, param, group, std::clamp(range, -127, 127), variation};
    if (dragging) {
        applyEdit(slot, m);
        sendRealtime(Bubble::realtimeFor(static_cast<u8>(slot), state_.slots[static_cast<std::size_t>(slot)].session, {m}));
    } else {
        editBubble(slot, {m});
    }
}

void Client::selectVariation(int slot, u8 variation) { editBubble(slot, {VariationSelect{variation}}); }
void Client::copyVariation(int slot, u8 from, u8 to) { editBubble(slot, {VariationCopy{from, to}}); }

void Client::setMode(int slot, Location loc, u8 module, u8 mode, u8 value)
{
    editBubble(slot, {ModeChange{static_cast<u8>(loc), module, mode, value}});
}

void Client::assignKnob(int slot, int knob, Location loc, u8 module, u8 param)
{
    if (knob < 0 || knob >= kKnobCount)
        return;
    editBubble(slot, {KnobAssign{module, param, static_cast<u8>(loc), 0, static_cast<std::uint16_t>(knob)}});
}

void Client::deassignKnob(int slot, int knob)
{
    if (knob < 0 || knob >= kKnobCount)
        return;
    editBubble(slot, {KnobDeassign{id::KnobDeassign, static_cast<std::uint16_t>(knob)}});
}

void Client::assignMidiCc(int slot, u8 cc, Location loc, u8 module, u8 param)
{
    editBubble(slot, {CtrlAssign{static_cast<u8>(loc), module, param, static_cast<u8>(cc & 0x7F)}});
}

void Client::deassignMidiCc(int slot, u8 cc) { editBubble(slot, {CtrlDeassign{static_cast<u8>(cc & 0x7F)}}); }

void Client::selectSlot(int slot)
{
    if (slot < 0 || slot >= kSlots)
        return;
    state_.slotFocus = static_cast<u8>(slot);
    applied(kSlotSynth, SlotFocus{static_cast<u8>(slot)});
    enqueueUser([this, slot] { return perfBubble({SlotFocus{static_cast<u8>(slot)}}); });
}

void Client::loadFromFlash(int slot, u8 bank, u8 prog)
{
    if (slot < 0 || slot > kSlotSynth)
        return;
    // The answer carries the release (38, or 1F), which reads the slot or the
    // performance again, and the result (0D), e.g. 1 "Can't load from flash".
    enqueueUser([slot, bank, prog] { return Bubble::synth({FlashCommand{id::FlashLoad, static_cast<u8>(slot), bank, prog}}); },
                [this](const std::vector<Molecule>& reply) {
                    for (const auto& m : reply)
                        if (const auto* r = std::get_if<FlashResult>(&m); r && listener_)
                            listener_->flashResult(*r);
                });
}

void Client::storeToFlash(int slot, u8 bank, u8 prog)
{
    if (slot < 0 || slot > kSlotSynth)
        return;
    // The synth does not send the new name list by itself: read it again once
    // the store is answered, so editors see the new entry. The answer carries
    // the result (0D), e.g. 4 when Memory Protect is on.
    enqueueUser([slot, bank, prog] { return Bubble::synth({FlashCommand{id::FlashStore, static_cast<u8>(slot), bank, prog}}); },
                [this, slot](const std::vector<Molecule>& reply) {
                    for (const auto& m : reply)
                        if (const auto* r = std::get_if<FlashResult>(&m); r && listener_)
                            listener_->flashResult(*r);
                    if (options_.readFlashNames)
                        enqueueFlashNames(slot == kSlotSynth ? 1 : 0, 0, 0, false);
                });
}

void Client::playNote(u8 note, bool on)
{
    enqueueUser([note, on] { return Bubble::synth({PlayNote{!on, static_cast<u8>(note & 0x7F)}}); });
}

void Client::setSynthSettings(const SynthSettings& settings)
{
    // The performance mode stays what it is when the bubble goes out (an
    // upload queued before may have switched it), unless this call changes it.
    const bool setsMode = settings.perfMode != state_.settings.perfMode;
    const bool mode = state_.settings.perfMode;
    state_.settings = settings;
    state_.settings.perfMode = setsMode ? settings.perfMode : mode;
    applied(kSlotSynth, SynthData{state_.settings});
    enqueueUser([this, settings, setsMode] {
        auto s = settings;
        if (!setsMode)
            s.perfMode = state_.settings.perfMode;
        return Bubble::synth({SynthData{s}});
    });
}

} // namespace g2::proto
