#include "g2/bridge/bridge_link.hpp"

#include <array>

namespace g2::bridge {

using namespace g2::proto;

BridgeLink::BridgeLink() : BridgeLink(Options{}) {}

BridgeLink::BridgeLink(Options options) : options_(std::move(options)) {}

BridgeLink::~BridgeLink() = default;

std::string BridgeLink::statusLine() const
{
    if (attached_)
        return statusLine_;
    return spawned_ ? "Starting the G2 bridge" : "No G2 bridge";
}

void BridgeLink::tick()
{
    if (!socket_.valid()) {
        if (std::chrono::steady_clock::now() >= nextAttempt_)
            tryAttach();
        if (!socket_.valid())
            return;
    }
    std::array<std::uint8_t, 64 * 1024> buffer;
    for (int reads = 0; reads < 256; ++reads) {
        const long n = socket_.read(buffer);
        if (n < 0) {
            detach();
            return;
        }
        if (n == 0)
            break;
        in_.append({buffer.data(), static_cast<std::size_t>(n)});
    }
    while (socket_.valid()) {
        auto m = in_.next();
        if (!m)
            break;
        handle(*m);
    }
    if (in_.corrupt()) {
        detach();
        return;
    }
    flush();
}

void BridgeLink::tryAttach()
{
    nextAttempt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.reconnectMs);
    socket_ = Socket::connect(options_.socketPath);
    if (!socket_.valid()) {
        if (!spawned_ && !options_.bridgeExecutable.empty()) {
            spawned_ = spawnBridge(options_.bridgeExecutable, options_.bridgeArguments);
            // Give it a moment before the next attempt.
            nextAttempt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::min(options_.reconnectMs, 200));
        }
        return;
    }
    in_ = MessageBuffer{};
    out_.clear();
    Writer w(MessageType::Hello);
    for (const char c : {'G', '2', 'B', 'R'})
        w.u8(static_cast<std::uint8_t>(c));
    w.u16(kProtocolVersion);
    out_ = w.finish();
    flush();
}

void BridgeLink::detach()
{
    const bool was = attached_;
    socket_.close();
    attached_ = false;
    spawned_ = false;
    synced_ = false;
    clientId_ = 0;
    in_ = MessageBuffer{};
    out_.clear();
    nextAttempt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.reconnectMs);
    if (was && reported_ != Status::NoDevice) {
        reported_ = Status::NoDevice;
        if (listener_)
            listener_->statusChanged(Status::NoDevice);
    }
}

void BridgeLink::flush()
{
    std::size_t sent = 0;
    while (sent < out_.size()) {
        const long n = socket_.write({out_.data() + sent, out_.size() - sent});
        if (n < 0) {
            detach();
            return;
        }
        if (n == 0)
            break;
        sent += static_cast<std::size_t>(n);
    }
    out_.erase(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(sent));
}

void BridgeLink::handle(const MessageBuffer::Message& m)
{
    Reader r(m.payload);
    switch (m.type) {
    case MessageType::Welcome: {
        const auto version = r.u16();
        clientId_ = r.u32();
        if (version != kProtocolVersion || clientId_ == 0) {
            detach(); // another bridge version: try again later (it may be replaced)
            return;
        }
        attached_ = true;
        return;
    }
    case MessageType::Status: {
        status_ = static_cast<Status>(r.u8());
        const bool synced = r.u8() != 0;
        statusLine_ = r.string();
        const bool becameSynced = synced && !synced_;
        synced_ = synced;
        if (status() != reported_) {
            reported_ = status();
            if (listener_)
                listener_->statusChanged(reported_);
        }
        if (becameSynced && listener_)
            listener_->synced();
        return;
    }
    case MessageType::Synth:
        applySynthSnapshot(state_, m.payload);
        return;
    case MessageType::Slot:
        applySlotSnapshot(state_, m.payload);
        return;
    case MessageType::Applied: {
        const auto target = r.u8();
        if (const auto molecule = r.molecule())
            applyMirrored(state_, target, *molecule);
        return;
    }
    case MessageType::Event:
        event(r);
        return;
    default:
        return; // unknown: ignored
    }
}

void BridgeLink::event(Reader& r)
{
    const auto kind = static_cast<EventKind>(r.u8());
    const int slot = r.u8();
    const auto value = r.u8();
    const auto molecule = r.molecule();
    if (!listener_ || r.failed())
        return;
    switch (kind) {
    case EventKind::SlotChanged: listener_->slotChanged(slot); break;
    case EventKind::SynthChanged: listener_->synthChanged(); break;
    case EventKind::PerformanceChanged: listener_->performanceChanged(); break;
    case EventKind::FlashNamesChanged: listener_->flashNamesChanged(value); break;
    case EventKind::ParamChanged:
        if (const auto* p = molecule ? std::get_if<ParamChange>(&*molecule) : nullptr)
            listener_->paramChanged(slot, *p);
        break;
    case EventKind::MorphChanged:
        if (const auto* p = molecule ? std::get_if<MorphChange>(&*molecule) : nullptr)
            listener_->morphChanged(slot, *p);
        break;
    case EventKind::VariationChanged: listener_->variationChanged(slot, value); break;
    case EventKind::ParamFocused:
        if (const auto* p = molecule ? std::get_if<ParamFocus>(&*molecule) : nullptr)
            listener_->paramFocused(slot, *p);
        break;
    case EventKind::PatchEdited:
        if (molecule)
            listener_->patchEdited(slot, *molecule);
        break;
    case EventKind::LedsChanged: listener_->ledsChanged(slot); break;
    case EventKind::MidiLearned:
        if (const auto* p = molecule ? std::get_if<MidiLearn>(&*molecule) : nullptr)
            listener_->midiLearned(*p);
        break;
    case EventKind::Error: listener_->error(value); break;
    default: break; // unknown: ignored
    }
}

// ---- Calls ---------------------------------------------------------------------

namespace {

Writer opWriter(Op op)
{
    Writer w(MessageType::Op);
    w.u8(static_cast<std::uint8_t>(op));
    return w;
}

std::uint8_t slotByte(int slot) { return slot >= 0 && slot <= 0xFF ? static_cast<std::uint8_t>(slot) : 0xFF; }

} // namespace

void BridgeLink::op(Writer&& w)
{
    if (!attached_)
        return;
    const auto message = w.finish();
    out_.insert(out_.end(), message.begin(), message.end());
    flush();
}

void BridgeLink::restart() { op(opWriter(Op::Restart)); }
void BridgeLink::resync() { op(opWriter(Op::Resync)); }
void BridgeLink::resyncSlot(int slot) { op(std::move(opWriter(Op::ResyncSlot).u8(slotByte(slot)))); }

void BridgeLink::sendPatch(int slot, const Patch& patch, const std::string& name)
{
    std::vector<std::uint8_t> bytes;
    try {
        bytes = savePatch(patch);
    } catch (const std::exception&) {
        return;
    }
    op(std::move(opWriter(Op::SendPatch).u8(slotByte(slot)).bytes(bytes).string(name)));
}

void BridgeLink::sendPerformance(const Performance& perf, const std::string& name)
{
    std::vector<std::uint8_t> bytes;
    try {
        bytes = savePerformance(perf);
    } catch (const std::exception&) {
        return;
    }
    op(std::move(opWriter(Op::SendPerformance).bytes(bytes).string(name)));
}

void BridgeLink::setParam(int slot, Location loc, u8 module, u8 param, u8 value, u8 variation)
{
    op(std::move(opWriter(Op::SetParam).u8(slotByte(slot)).u8(static_cast<u8>(loc)).u8(module).u8(param).u8(value).u8(variation)));
}

void BridgeLink::selectParam(int slot, Location loc, u8 module, u8 param)
{
    op(std::move(opWriter(Op::SelectParam).u8(slotByte(slot)).u8(static_cast<u8>(loc)).u8(module).u8(param)));
}

void BridgeLink::setMorph(int slot, Location loc, u8 module, u8 param, u8 group, int range, u8 variation, bool dragging)
{
    op(std::move(opWriter(Op::SetMorph)
                     .u8(slotByte(slot))
                     .u8(static_cast<u8>(loc))
                     .u8(module)
                     .u8(param)
                     .u8(group)
                     .u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(range, -32768, 32767))))
                     .u8(variation)
                     .u8(dragging ? 1 : 0)));
}

void BridgeLink::selectVariation(int slot, u8 variation)
{
    op(std::move(opWriter(Op::SelectVariation).u8(slotByte(slot)).u8(variation)));
}

void BridgeLink::copyVariation(int slot, u8 from, u8 to)
{
    op(std::move(opWriter(Op::CopyVariation).u8(slotByte(slot)).u8(from).u8(to)));
}

void BridgeLink::setMode(int slot, Location loc, u8 module, u8 mode, u8 value)
{
    op(std::move(opWriter(Op::SetMode).u8(slotByte(slot)).u8(static_cast<u8>(loc)).u8(module).u8(mode).u8(value)));
}

void BridgeLink::assignKnob(int slot, int knob, Location loc, u8 module, u8 param)
{
    if (knob < 0 || knob > 0xFFFF)
        return;
    op(std::move(opWriter(Op::AssignKnob)
                     .u8(slotByte(slot))
                     .u16(static_cast<std::uint16_t>(knob))
                     .u8(static_cast<u8>(loc))
                     .u8(module)
                     .u8(param)));
}

void BridgeLink::deassignKnob(int slot, int knob)
{
    if (knob < 0 || knob > 0xFFFF)
        return;
    op(std::move(opWriter(Op::DeassignKnob).u8(slotByte(slot)).u16(static_cast<std::uint16_t>(knob))));
}

void BridgeLink::assignMidiCc(int slot, u8 cc, Location loc, u8 module, u8 param)
{
    op(std::move(opWriter(Op::AssignMidiCc).u8(slotByte(slot)).u8(cc).u8(static_cast<u8>(loc)).u8(module).u8(param)));
}

void BridgeLink::deassignMidiCc(int slot, u8 cc) { op(std::move(opWriter(Op::DeassignMidiCc).u8(slotByte(slot)).u8(cc))); }
void BridgeLink::selectSlot(int slot) { op(std::move(opWriter(Op::SelectSlot).u8(slotByte(slot)))); }

void BridgeLink::loadFromFlash(int slot, u8 bank, u8 prog)
{
    op(std::move(opWriter(Op::LoadFromFlash).u8(slotByte(slot)).u8(bank).u8(prog)));
}

void BridgeLink::storeToFlash(int slot, u8 bank, u8 prog)
{
    op(std::move(opWriter(Op::StoreToFlash).u8(slotByte(slot)).u8(bank).u8(prog)));
}

void BridgeLink::playNote(u8 note, bool on) { op(std::move(opWriter(Op::PlayNote).u8(note).u8(on ? 1 : 0))); }

void BridgeLink::edit(int slot, std::vector<Molecule> molecules)
{
    std::vector<std::uint8_t> bytes;
    try {
        bytes = encode(molecules);
    } catch (const std::exception&) {
        return;
    }
    op(std::move(opWriter(Op::Edit).u8(slotByte(slot)).bytes(bytes)));
}

} // namespace g2::bridge
