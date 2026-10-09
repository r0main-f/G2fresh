#include "g2/proto/emulator.hpp"

#include "g2/proto/bubble.hpp"
#include "g2/proto/sections.hpp"

#include "g2/patch_load.hpp"

#include <algorithm>
#include <type_traits>

namespace g2::proto {
namespace {

template <class... F>
struct Overloaded : F... {
    using F::operator()...;
};
template <class... F>
Overloaded(F...) -> Overloaded<F...>;

constexpr std::size_t kNamesPerChunk = 16; // names per 16 reply before "ask again" [I]

const file::Section* findSection(const std::vector<file::Section>& sections, u8 sid, std::optional<u8> loc)
{
    for (const auto& s : sections)
        if (s.id == sid && (!loc || sectionLocation(s) == loc))
            return &s;
    return nullptr;
}

} // namespace

Emulator::Emulator()
{
    state_.settings.name = "G2 Emulator";
    state_.voices = {1, 1, 1, 1};
    state_.perfName = "Emulator";
    state_.perfSession = 5;
    state_.perfSessionKnown = true;
    for (int i = 0; i < kSlots; ++i) {
        auto& slot = state_.slots[static_cast<std::size_t>(i)];
        slot.patch = Patch::makeDefault();
        slot.name = "No name";
        slot.session = static_cast<u8>(1 + i);
        slot.sessionKnown = true;
        state_.perfHeader.slots[static_cast<std::size_t>(i)].patchName = slot.name;
    }
}

// ---- Sending ----------------------------------------------------------------------

void Emulator::send(u8 hdr, u8 session, const std::vector<Molecule>& molecules)
{
    auto message = deviceMessage(hdr, session, encode(molecules));
    if (corruptNext_) {
        message.back() ^= 0xFF;
        corruptNext_ = false;
    }
    outgoing_.push_back(deliver(message, message.size() > maxEmbedded_));
}

void Emulator::sendUnsolicited(u8 slot, std::vector<Molecule> molecules)
{
    // Releases and synth-level news go on hdr 4 with a void session (as in
    // the golden vectors' synthetic captures); slot traffic carries the
    // slot's session.
    const bool slotLevel = slot < kSlots;
    const u8 session = slotLevel ? state_.slots[slot].session : kVoidSession;
    send(slot, session, molecules);
}

// ---- Receiving --------------------------------------------------------------------

void Emulator::receive(std::span<const u8> frame)
{
    const auto f = parseHostFrame(frame);
    if (!f) {
        // "Down link check sum error" (7E 4) [I]: the slot from the header byte if there is one.
        const u8 slot = frame.size() > 3 ? static_cast<u8>(frame[3] & kSlotMask) : kSlotSynth;
        if (responding_)
            send(static_cast<u8>(kResponse | slot), static_cast<u8>(kVoidSession | 1), {Exception{4}});
        return;
    }
    received_.push_back(*f);
    if (f->versionRequest) {
        if (responding_) {
            const auto message = versionMessage(encode(state_.version));
            outgoing_.push_back(deliver(message, true)); // delivered extended [V]
        }
        return;
    }
    if (f->realtime()) {
        // Realtime messages are never answered; edits with a stale session are dropped.
        const u8 slot = f->slot();
        if (slot >= kSlots || f->session != state_.slots[slot].session)
            return;
        for (const auto& m : decode(f->molecules))
            applyToSlot(state_.slots[slot], m);
        return;
    }
    // A bubble split over several messages: every message is answered; the
    // bubble is processed at its last one.
    partial_.insert(partial_.end(), f->molecules.begin(), f->molecules.end());
    if (!(f->hdr & kBubbleEnd)) {
        if (responding_)
            send(static_cast<u8>(kResponse | f->slot()), static_cast<u8>(kVoidSession | 1), {Ack{}});
        return;
    }
    auto molecules = std::move(partial_);
    partial_.clear();
    handleBubble(*f, std::move(molecules));
}

void Emulator::handleBubble(const HostFrame& f, std::vector<u8> bytes)
{
    if (!responding_)
        return;
    const u8 slot = f.slot();
    const bool isVoid = f.voidSession();
    auto replySession = [&](std::size_t count) {
        if (isVoid)
            return static_cast<u8>(kVoidSession | (count & kSessionMask));
        return slot < kSlots ? state_.slots[slot].session : state_.perfSession;
    };
    if (failNext_) {
        const std::vector<Molecule> reply{Exception{*failNext_}};
        failNext_.reset();
        send(static_cast<u8>(kResponse | slot), replySession(1), reply);
        return;
    }

    const auto molecules = decode(bytes);
    std::vector<Molecule> reply;
    std::vector<std::vector<Molecule>> after; // unsolicited messages after the reply (hdr 4)

    const bool upload = !molecules.empty() && std::holds_alternative<DumpDestination>(molecules.front());
    if (upload && slot < kSlots) {
        // Patch upload (§8.1): 37 name + 18 sections.
        PatchAssembler a;
        for (std::size_t i = 1; i < molecules.size(); ++i)
            if (const auto* s = std::get_if<SectionDump>(&molecules[i]))
                a.add(s->section);
        if (a.complete()) {
            auto& target = state_.slots[slot];
            target.patch = a.patch();
            target.name = std::get<DumpDestination>(molecules.front()).name;
            target.session = nextSession(target.session);
            state_.perfHeader.slots[slot].patchName = target.name;
            after.push_back({SessionNumber{id::PatchRelease, slot, target.session}});
        } else {
            reply.push_back(Exception{6}); // "Unfinished bubble error" [I]
        }
    } else if (upload && molecules.size() >= 2 && std::holds_alternative<CompletePerformance>(molecules[1])) {
        const auto& p = std::get<CompletePerformance>(molecules[1]);
        try {
            state_.setPerformance(performanceFromSections(p.sections), p.name);
            state_.perfSession = nextSession(state_.perfSession);
            for (auto& s : state_.slots)
                s.session = nextSession(s.session);
            after.push_back({PerformanceRelease{state_.perfSession}});
        } catch (const std::exception&) {
            reply.push_back(Exception{5}); // "Stream execute error" [I]
        }
    } else {
        const bool sessionOk = isVoid || (slot < kSlots ? f.session == state_.slots[slot].session
                                                        : f.session == state_.perfSession);
        for (const auto& m : molecules)
            handle(slot, sessionOk, m, reply, after);
    }
    if (reply.empty())
        reply.push_back(Ack{});
    send(static_cast<u8>(kResponse | slot), replySession(reply.size()), reply);
    for (auto& m : after)
        sendUnsolicited(kSlotSynth, std::move(m));
}

std::vector<Molecule> Emulator::patchDump(u8 slot) const
{
    const auto& s = state_.slots[slot];
    std::vector<Molecule> out;
    for (auto& section : uploadSections(s.patch)) {
        if (section.id == file::kTextpad)
            out.push_back(NameDump{id::PatchName, s.name});
        out.push_back(SectionDump{std::move(section)});
    }
    return out;
}

std::vector<Molecule> Emulator::flashNames(u8 type, u8 bank, u8 prog) const
{
    FlashData d;
    d.flag = 1;
    d.type = type;
    const auto& entries = flash_[type & 1];
    auto it = entries.lower_bound({bank, prog});
    std::optional<std::pair<u8, u8>> cursor;
    std::size_t names = 0;
    using K = FlashItem::Kind;
    for (; it != entries.end() && names < kNamesPerChunk; ++it, ++names) {
        const auto [b, p] = it->first;
        if (!cursor || cursor->first != b) {
            d.items.push_back({K::SetEntry, b, p, {}, 0});
        } else if (cursor->second != p) {
            if (p > cursor->second && p - cursor->second <= 3)
                for (int k = cursor->second; k < p; ++k)
                    d.items.push_back({K::Empty, 0, 0, {}, 0});
            else
                d.items.push_back({K::SetProg, 0, p, {}, 0});
        }
        d.items.push_back({K::Name, 0, 0, it->second.name, it->second.category});
        cursor = std::make_pair(b, static_cast<u8>(p + 1));
    }
    d.items.push_back({it == entries.end() ? K::EndOfList : K::EndOfChunk, 0, 0, {}, 0});
    return {d};
}

void Emulator::handle(u8 slot, bool sessionOk, const Molecule& molecule, std::vector<Molecule>& reply,
                      std::vector<std::vector<Molecule>>& after)
{
    const bool slotLevel = slot < kSlots;
    SlotState* target = slotLevel ? &state_.slots[slot] : nullptr;
    auto slotSection = [&](u8 sid, std::optional<u8> loc = std::nullopt) {
        if (!target)
            return;
        const auto sections = uploadSections(target->patch);
        if (const auto* s = findSection(sections, sid, loc))
            reply.push_back(SectionDump{*s});
    };
    auto perfHeaderSection = [&] {
        auto header = state_.perfHeader;
        for (int i = 0; i < kSlots; ++i)
            header.slots[static_cast<std::size_t>(i)].patchName = state_.slots[static_cast<std::size_t>(i)].name;
        return SectionDump{file::Section{file::kPerfHeader, header, 0, {}}};
    };

    std::visit(
        Overloaded{
            [&](const Request& m) {
                switch (m.id) {
                case id::SynthDataRequest: reply.push_back(SynthData{state_.settings}); break;
                case id::VoicesRequest: reply.push_back(Voices{state_.voices}); break;
                case id::SlotSelectionRequest: reply.push_back(SlotFlags{id::SlotSelection, state_.slotEnabled}); break;
                case id::SlotFocusRequest: reply.push_back(SlotFocus{state_.slotFocus}); break;
                case id::PerfHeaderRequest:
                    reply.push_back(NameDump{id::PerfName, state_.perfName});
                    reply.push_back(perfHeaderSection());
                    break;
                case id::PerfNameRequest: reply.push_back(NameDump{id::PerfName, state_.perfName}); break;
                case id::GlobalPageFocusRequest: reply.push_back(PageFocus{id::GlobalPageFocus, state_.globalPage}); break;
                case id::GlobalKnobMapRequest:
                    reply.push_back(SectionDump{file::Section{file::kGlobalKnobMap, file::KnobMap{state_.globalKnobs}, 0, {}}});
                    break;
                case id::ClockInfoRequest: reply.push_back(state_.clock); break;
                case id::MidiLearnRequest: reply.push_back(state_.midiLearn.value_or(MidiLearn{})); break;
                case id::CompletePatchRequest:
                    if (target)
                        reply = patchDump(slot);
                    break;
                case id::PatchHeaderRequest: slotSection(file::kPatchHeader); break;
                case id::PatchNameRequest:
                    if (target)
                        reply.push_back(NameDump{id::PatchName, target->name});
                    break;
                case id::ParamFocusRequest:
                    if (target)
                        reply.push_back(target->focus);
                    break;
                case id::PageFocusRequest:
                    if (target)
                        reply.push_back(PageFocus{id::PageFocus, target->page});
                    break;
                case id::CtrlMapRequest: slotSection(file::kCtrlMap); break;
                case id::KnobMapRequest: slotSection(file::kKnobMap); break;
                case id::MorphMapRequest: slotSection(file::kMorphMap); break;
                case id::CurrentNotesRequest: slotSection(file::kCurrentNotes); break;
                case id::TextpadRequest: slotSection(file::kTextpad); break;
                default: break; // 3D, 55, 70: acknowledged
                }
            },
            [&](const LocationRequest& m) {
                switch (m.id) {
                case id::ModuleListRequest: slotSection(file::kModuleList, m.location); break;
                case id::ParamListRequest: slotSection(file::kParamList, m.location); break;
                case id::ModuleNamesRequest: slotSection(file::kModuleNames, m.location); break;
                case id::CustomDataRequest: slotSection(file::kCustomData, m.location); break;
                case id::CableListRequest: slotSection(file::kCableList, m.location); break;
                case id::PatchLoadRequest:
                    if (target) {
                        // The area's resources as the editor computes them (a real synth
                        // reports its own figures).
                        const auto loc = m.location ? Location::Va : Location::Fx;
                        const auto bytes = patchload::encodeReport({loc, patchload::areaTotal(target->patch.area(loc))});
                        PatchLoad load{static_cast<u8>(m.location ? 1 : 0), {}};
                        std::copy(bytes.begin() + 1, bytes.end(), load.counters.begin());
                        reply.push_back(load);
                    }
                    break;
                default: break;
                }
            },
            [&](const SessionRequest& m) {
                const u8 session = m.slot < kSlots ? state_.slots[m.slot].session : state_.perfSession;
                reply.push_back(SessionNumber{id::SessionDump, m.slot, session});
            },
            [&](const EditorSync& m) { editorSync_ = m.begin; },
            [&](const FlashCommand& m) {
                const u8 type = m.slot == kSlotSynth ? 1 : 0;
                switch (m.id) {
                case id::FlashLoad: {
                    const auto it = flash_[type].find({m.bank, m.prog});
                    if (it == flash_[type].end())
                        break;
                    try {
                        if (type == 0 && m.slot < kSlots) {
                            auto& s = state_.slots[m.slot];
                            s.patch = g2::loadPatch(it->second.file, {true});
                            s.name = it->second.name;
                            s.session = nextSession(s.session);
                            after.push_back({SessionNumber{id::PatchRelease, m.slot, s.session}});
                        } else if (type == 1) {
                            state_.setPerformance(g2::loadPerformance(it->second.file, {true}), it->second.name);
                            state_.perfSession = nextSession(state_.perfSession);
                            after.push_back({PerformanceRelease{state_.perfSession}});
                        }
                    } catch (const std::exception&) {
                        reply.push_back(Exception{5});
                    }
                    break;
                }
                case id::FlashStore: {
                    FlashEntry e;
                    if (type == 0 && m.slot < kSlots) {
                        e.name = state_.slots[m.slot].name;
                        e.file = savePatch(state_.slots[m.slot].patch);
                    } else {
                        e.name = state_.perfName;
                        e.file = savePerformance(state_.performance());
                    }
                    flash_[type][{m.bank, m.prog}] = std::move(e);
                    reply.push_back(FlashResult{type, m.bank, m.prog, 0, 0});
                    break;
                }
                case id::FlashDataRequest: {
                    auto names = flashNames(m.slot, m.bank, m.prog);
                    reply.insert(reply.end(), names.begin(), names.end());
                    break;
                }
                case id::FlashDumpRequest: {
                    const u8 t = m.slot & 1;
                    const auto it = flash_[t].find({m.bank, m.prog});
                    if (it == flash_[t].end()) {
                        reply.push_back(FlashDumpMarker{4, t, m.bank, m.prog, {}});
                        break;
                    }
                    // The binary part of the file (§8.3): after the text header's NUL,
                    // the version byte, then the rest (type, sections, CRC).
                    const auto& bytes = it->second.file;
                    const auto start = std::find(bytes.begin(), bytes.end(), u8{0}) - bytes.begin();
                    FlashRawData raw{t, m.bank, m.prog, it->second.name, 0, std::nullopt};
                    if (static_cast<std::size_t>(start) + 2 < bytes.size()) {
                        raw.version = bytes[static_cast<std::size_t>(start) + 1];
                        raw.data = std::vector<u8>(bytes.begin() + start + 2, bytes.end());
                    }
                    reply.push_back(FlashDumpMarker{0, t, m.bank, m.prog, {}});
                    reply.push_back(raw);
                    break;
                }
                default: break;
                }
            },
            [&](const FlashDelete& m) {
                flash_[m.type & 1].erase({m.bank, m.prog});
                reply.push_back(FlashResult{m.type, m.bank, m.prog, m.origin, 0});
            },
            [&](const SynthData& m) { state_.settings = m.settings; },
            [&](const PerformanceMode& m) { state_.settings.perfMode = m.perfMode != 0; },
            [&](const PlayNote&) {},
            [&](const auto& m) {
                // Edits: patch-level ones to the slot, the rest to the synth or
                // performance. Stale sessions make the synth discard them [I].
                if (!sessionOk)
                    return;
                if (target && applyToSlot(*target, m))
                    return;
                applyToSynth(state_, m);
            },
        },
        molecule);
}

// ---- Panel actions ----------------------------------------------------------------

void Emulator::turnKnob(u8 slot, u8 location, u8 module, u8 param, u8 value)
{
    auto& s = state_.slots[slot];
    const ParamChange m{location, module, param, value, s.patch.header.activeVariation};
    applyToSlot(s, m);
    sendUnsolicited(slot, {m});
}

void Emulator::selectVariation(u8 slot, u8 variation)
{
    const VariationSelect m{variation};
    applyToSlot(state_.slots[slot], m);
    sendUnsolicited(slot, {m});
}

void Emulator::selectParam(u8 slot, u8 location, u8 module, u8 param)
{
    const ParamFocus m{1, location, module, param};
    applyToSlot(state_.slots[slot], m);
    sendUnsolicited(slot, {m});
}

void Emulator::midiLearn(u8 slot, u8 cc)
{
    state_.midiLearn = MidiLearn{slot, cc};
    sendUnsolicited(kSlotSynth, {MidiLearn{slot, cc}});
}

void Emulator::loadPatch(u8 slot, const Patch& patch, const std::string& name)
{
    auto& s = state_.slots[slot];
    s.patch = patch;
    s.name = name;
    s.session = nextSession(s.session);
    sendUnsolicited(kSlotSynth, {SessionNumber{id::PatchRelease, slot, s.session}});
}

void Emulator::loadPerformance(const Performance& perf, const std::string& name)
{
    state_.setPerformance(perf, name);
    state_.perfSession = nextSession(state_.perfSession);
    for (auto& s : state_.slots)
        s.session = nextSession(s.session);
    sendUnsolicited(kSlotSynth, {PerformanceRelease{state_.perfSession}});
}

void Emulator::sendLeds(u8 slot, std::span<const u8> leds)
{
    const Blink b{id::Leds, 0, encodeLeds(leds)};
    applyToSlot(state_.slots[slot], b);
    sendUnsolicited(slot, {b});
}

void Emulator::sendMeters(u8 slot, std::span<const std::uint16_t> meters)
{
    const Blink b{id::Meters, 0, encodeMeters(meters)};
    applyToSlot(state_.slots[slot], b);
    sendUnsolicited(slot, {b});
}

// ---- EmulatorTransport ------------------------------------------------------------

void EmulatorTransport::plugIn()
{
    if (plugged_)
        return;
    plugged_ = true;
    events_.push_back(true);
}

void EmulatorTransport::unplug()
{
    if (!plugged_)
        return;
    plugged_ = false;
    emulator_.outgoing().clear();
    events_.push_back(false);
}

bool EmulatorTransport::send(std::span<const std::uint8_t> frame)
{
    if (!plugged_)
        return false;
    emulator_.receive(frame);
    return true;
}

void EmulatorTransport::poll()
{
    while (!events_.empty()) {
        const bool arrived = events_.front();
        events_.pop_front();
        if (sink_)
            arrived ? sink_->deviceArrived() : sink_->deviceRemoved();
    }
    // The client may answer from inside the callbacks; whatever the emulator
    // sends back meanwhile is delivered in the same poll.
    auto& out = emulator_.outgoing();
    while (plugged_ && !out.empty()) {
        const auto d = std::move(out.front());
        out.pop_front();
        if (!sink_)
            continue;
        sink_->interruptPacket(d.interrupt);
        if (!d.bulk.empty())
            sink_->bulkIn(d.bulk);
    }
}

} // namespace g2::proto
