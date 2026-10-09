#include "g2/bridge/server.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace g2::bridge {

using namespace g2::proto;

BridgeServer::BridgeServer(std::unique_ptr<LocalLink> link, Options options)
    : link_(std::move(link)), options_(std::move(options)), idleSince_(std::chrono::steady_clock::now())
{
    link_->client().setObserver(this);
    link_->setListener(this);
}

BridgeServer::~BridgeServer()
{
    link_->client().setObserver(nullptr);
    link_->setListener(nullptr);
}

bool BridgeServer::start(std::string* error)
{
    listener_ = Socket::listen(options_.socketPath, error);
    idleSince_ = std::chrono::steady_clock::now();
    return listener_.valid();
}

bool BridgeServer::shouldExit() const
{
    if (options_.stay || !clients_.empty())
        return false;
    const auto idle = std::chrono::steady_clock::now() - idleSince_;
    return idle >= std::chrono::milliseconds(options_.idleExitMs);
}

void BridgeServer::pump(int waitMs)
{
    std::vector<SocketHandle> sockets{listener_.handle()};
    for (const auto& c : clients_)
        sockets.push_back(c->socket.handle());
    waitReadable(sockets, waitMs);

    acceptAll();
    readAll();
    link_->tick();
    flushAll();

    const auto now = std::chrono::steady_clock::now();
    for (auto& c : clients_)
        if (!c->hello && now - c->since > std::chrono::milliseconds(options_.helloTimeoutMs))
            c->dead = true; // not an editor
    const auto before = clients_.size();
    std::erase_if(clients_, [](const auto& c) { return c->dead; });
    if (before > 0 && clients_.empty())
        idleSince_ = std::chrono::steady_clock::now();
}

void BridgeServer::acceptAll()
{
    for (;;) {
        Socket s = listener_.accept();
        if (!s.valid())
            return;
        auto c = std::make_unique<Editor>();
        c->id = nextId_++;
        c->socket = std::move(s);
        c->since = std::chrono::steady_clock::now();
        clients_.push_back(std::move(c));
    }
}

void BridgeServer::readAll()
{
    std::array<std::uint8_t, 64 * 1024> buffer;
    for (std::size_t i = 0; i < clients_.size(); ++i) {
        auto& c = *clients_[i];
        for (int reads = 0; reads < 64 && !c.dead; ++reads) {
            const long n = c.socket.read(buffer);
            if (n < 0) {
                c.dead = true;
                break;
            }
            if (n == 0)
                break;
            c.in.append({buffer.data(), static_cast<std::size_t>(n)});
        }
        while (!c.dead) {
            auto m = c.in.next();
            if (!m)
                break;
            handle(c, *m);
        }
        if (c.in.corrupt())
            c.dead = true;
    }
}

void BridgeServer::flushAll()
{
    for (auto& c : clients_) {
        if (c->dead)
            continue;
        while (c->outPos < c->out.size()) {
            const long n = c->socket.write({c->out.data() + c->outPos, c->out.size() - c->outPos});
            if (n < 0) {
                c->dead = true;
                break;
            }
            if (n == 0)
                break;
            c->outPos += static_cast<std::size_t>(n);
        }
        if (c->outPos == c->out.size()) {
            c->out.clear();
            c->outPos = 0;
        } else if (c->outPos > (1u << 20)) {
            c->out.erase(c->out.begin(), c->out.begin() + static_cast<std::ptrdiff_t>(c->outPos));
            c->outPos = 0;
        }
        if (c->out.size() - c->outPos > options_.maxBacklog)
            c->dead = true; // not reading: let it go
    }
}

void BridgeServer::send(Editor& c, std::vector<std::uint8_t> message)
{
    if (c.dead)
        return;
    c.out.insert(c.out.end(), message.begin(), message.end());
}

void BridgeServer::broadcast(const std::vector<std::uint8_t>& message, std::uint32_t except)
{
    for (auto& c : clients_)
        if (c->hello && !c->dead && c->id != except)
            c->out.insert(c->out.end(), message.begin(), message.end());
}

// ---- Requests ------------------------------------------------------------------

void BridgeServer::handle(Editor& c, const MessageBuffer::Message& m)
{
    Reader r(m.payload);
    switch (m.type) {
    case MessageType::Hello: {
        char magic[4];
        for (char& ch : magic)
            ch = static_cast<char>(r.u8());
        const auto version = r.u16();
        if (r.failed() || std::memcmp(magic, "G2BR", 4) != 0 || version != kProtocolVersion) {
            // One attempt to tell the editor our version (client id 0: refused).
            const auto welcome = Writer(MessageType::Welcome).u16(kProtocolVersion).u32(0).finish();
            c.socket.write(welcome);
            c.dead = true;
            return;
        }
        c.hello = true;
        send(c, Writer(MessageType::Welcome).u16(kProtocolVersion).u32(c.id).finish());
        sendSnapshot(c);
        send(c, statusMessage());
        return;
    }
    case MessageType::Op:
        if (!c.hello) {
            c.dead = true;
            return;
        }
        execute(c, r);
        return;
    default:
        return; // unknown: ignored
    }
}

void BridgeServer::execute(Editor& c, Reader& r)
{
    origin_ = c.id;
    const auto op = static_cast<Op>(r.u8());
    auto& l = *link_;
    const auto loc = [](u8 v) { return static_cast<Location>(v); };
    switch (op) {
    case Op::Restart: l.restart(); break;
    case Op::Resync: l.resync(); break;
    case Op::ResyncSlot: l.resyncSlot(r.u8()); break;
    case Op::SendPatch: {
        const int slot = r.u8();
        const auto bytes = r.bytes();
        const auto name = r.string();
        if (!r.failed()) {
            try {
                l.sendPatch(slot, loadPatch(bytes), name);
            } catch (const std::exception&) {
                // not a patch: ignored
            }
        }
        break;
    }
    case Op::SendPerformance: {
        const auto bytes = r.bytes();
        const auto name = r.string();
        if (!r.failed()) {
            try {
                l.sendPerformance(loadPerformance(bytes), name);
            } catch (const std::exception&) {
            }
        }
        break;
    }
    case Op::SetParam: {
        const int slot = r.u8();
        const auto location = r.u8(), module = r.u8(), param = r.u8(), value = r.u8(), variation = r.u8();
        if (!r.failed())
            l.setParam(slot, loc(location), module, param, value, variation);
        break;
    }
    case Op::SelectParam: {
        const int slot = r.u8();
        const auto location = r.u8(), module = r.u8(), param = r.u8();
        if (!r.failed())
            l.selectParam(slot, loc(location), module, param);
        break;
    }
    case Op::SetMorph: {
        const int slot = r.u8();
        const auto location = r.u8(), module = r.u8(), param = r.u8(), group = r.u8();
        const auto range = static_cast<std::int16_t>(r.u16());
        const auto variation = r.u8();
        const bool dragging = r.u8() != 0;
        if (!r.failed())
            l.setMorph(slot, loc(location), module, param, group, range, variation, dragging);
        break;
    }
    case Op::SelectVariation: {
        const int slot = r.u8();
        const auto v = r.u8();
        if (!r.failed())
            l.selectVariation(slot, v);
        break;
    }
    case Op::CopyVariation: {
        const int slot = r.u8();
        const auto from = r.u8(), to = r.u8();
        if (!r.failed())
            l.copyVariation(slot, from, to);
        break;
    }
    case Op::SetMode: {
        const int slot = r.u8();
        const auto location = r.u8(), module = r.u8(), mode = r.u8(), value = r.u8();
        if (!r.failed())
            l.setMode(slot, loc(location), module, mode, value);
        break;
    }
    case Op::AssignKnob: {
        const int slot = r.u8();
        const int knob = r.u16();
        const auto location = r.u8(), module = r.u8(), param = r.u8();
        if (!r.failed())
            l.assignKnob(slot, knob, loc(location), module, param);
        break;
    }
    case Op::DeassignKnob: {
        const int slot = r.u8();
        const int knob = r.u16();
        if (!r.failed())
            l.deassignKnob(slot, knob);
        break;
    }
    case Op::AssignMidiCc: {
        const int slot = r.u8();
        const auto cc = r.u8(), location = r.u8(), module = r.u8(), param = r.u8();
        if (!r.failed())
            l.assignMidiCc(slot, cc, loc(location), module, param);
        break;
    }
    case Op::DeassignMidiCc: {
        const int slot = r.u8();
        const auto cc = r.u8();
        if (!r.failed())
            l.deassignMidiCc(slot, cc);
        break;
    }
    case Op::SelectSlot: {
        const int slot = r.u8();
        if (!r.failed())
            l.selectSlot(slot);
        break;
    }
    case Op::LoadFromFlash:
    case Op::StoreToFlash: {
        const int slot = r.u8();
        const auto bank = r.u8(), prog = r.u8();
        if (!r.failed()) {
            if (op == Op::LoadFromFlash)
                l.loadFromFlash(slot, bank, prog);
            else
                l.storeToFlash(slot, bank, prog);
        }
        break;
    }
    case Op::PlayNote: {
        const auto note = r.u8();
        const bool on = r.u8() != 0;
        if (!r.failed())
            l.playNote(note, on);
        break;
    }
    case Op::Edit: {
        const int slot = r.u8();
        const auto bytes = r.bytes();
        if (!r.failed())
            l.edit(slot, decode(bytes));
        break;
    }
    default:
        break; // unknown: ignored
    }
    origin_ = 0;
}

// ---- State to the editors ------------------------------------------------------

std::vector<std::uint8_t> BridgeServer::statusMessage() const
{
    return Writer(MessageType::Status)
        .u8(static_cast<u8>(link_->status()))
        .u8(link_->synced() ? 1 : 0)
        .string(link_->statusLine())
        .finish();
}

void BridgeServer::sendSnapshot(Editor& c)
{
    const auto& s = link_->state();
    send(c, synthSnapshot(s));
    for (int slot = 0; slot < kSlots; ++slot)
        send(c, slotSnapshot(s, slot));
}

void BridgeServer::broadcastSnapshot()
{
    const auto& s = link_->state();
    broadcast(synthSnapshot(s));
    for (int slot = 0; slot < kSlots; ++slot)
        broadcast(slotSnapshot(s, slot));
}

std::vector<std::uint8_t> BridgeServer::eventMessage(EventKind kind, int slot, u8 value, const Molecule* molecule) const
{
    Writer w(MessageType::Event);
    w.u8(static_cast<u8>(kind)).u8(static_cast<u8>(slot)).u8(value);
    if (molecule)
        w.molecule(*molecule);
    else
        w.bytes({});
    return w.finish();
}

void BridgeServer::editEvents(u8 target, const Molecule& m)
{
    // What the other editors' listeners would hear had the edit come from
    // the synth's panel.
    if (target >= kSlots) {
        broadcast(eventMessage(EventKind::PerformanceChanged, kSlotSynth), origin_);
        return;
    }
    if (std::holds_alternative<ParamChange>(m))
        broadcast(eventMessage(EventKind::ParamChanged, target, 0, &m), origin_);
    else if (std::holds_alternative<MorphChange>(m))
        broadcast(eventMessage(EventKind::MorphChanged, target, 0, &m), origin_);
    else if (const auto* v = std::get_if<VariationSelect>(&m))
        broadcast(eventMessage(EventKind::VariationChanged, target, v->variation), origin_);
    else if (std::holds_alternative<ParamFocus>(m))
        broadcast(eventMessage(EventKind::ParamFocused, target, 0, &m), origin_);
    else
        broadcast(eventMessage(EventKind::PatchEdited, target, 0, &m), origin_);
}

// Client::Observer

void BridgeServer::applied(u8 target, const Molecule& m)
{
    broadcast(Writer(MessageType::Applied).u8(target).molecule(m).finish());
    if (origin_ != 0)
        editEvents(target, m);
}

void BridgeServer::slotReplaced(int slot)
{
    broadcast(slotSnapshot(link_->state(), slot));
    if (origin_ != 0)
        broadcast(eventMessage(EventKind::SlotChanged, slot), origin_);
}

void BridgeServer::stateReplaced()
{
    broadcastSnapshot();
    if (origin_ != 0) {
        broadcast(eventMessage(EventKind::PerformanceChanged, kSlotSynth), origin_);
        for (int slot = 0; slot < kSlots; ++slot)
            broadcast(eventMessage(EventKind::SlotChanged, slot), origin_);
    }
}

// Client::Listener: synth-initiated news, to every editor.

void BridgeServer::statusChanged(Status) { broadcast(statusMessage()); }
void BridgeServer::synced() { broadcast(statusMessage()); }
void BridgeServer::slotChanged(int slot) { broadcast(eventMessage(EventKind::SlotChanged, slot)); }
void BridgeServer::synthChanged() { broadcast(eventMessage(EventKind::SynthChanged, kSlotSynth)); }
void BridgeServer::performanceChanged() { broadcast(eventMessage(EventKind::PerformanceChanged, kSlotSynth)); }

void BridgeServer::flashNamesChanged(u8 type)
{
    broadcast(eventMessage(EventKind::FlashNamesChanged, kSlotSynth, type));
}

void BridgeServer::paramChanged(int slot, const ParamChange& p)
{
    const Molecule m = p;
    broadcast(eventMessage(EventKind::ParamChanged, slot, 0, &m));
}

void BridgeServer::morphChanged(int slot, const MorphChange& mc)
{
    const Molecule m = mc;
    broadcast(eventMessage(EventKind::MorphChanged, slot, 0, &m));
}

void BridgeServer::variationChanged(int slot, u8 variation)
{
    broadcast(eventMessage(EventKind::VariationChanged, slot, variation));
}

void BridgeServer::paramFocused(int slot, const ParamFocus& f)
{
    const Molecule m = f;
    broadcast(eventMessage(EventKind::ParamFocused, slot, 0, &m));
}

void BridgeServer::patchEdited(int slot, const Molecule& m) { broadcast(eventMessage(EventKind::PatchEdited, slot, 0, &m)); }
void BridgeServer::ledsChanged(int slot) { broadcast(eventMessage(EventKind::LedsChanged, slot)); }

void BridgeServer::midiLearned(const MidiLearn& l)
{
    const Molecule m = l;
    broadcast(eventMessage(EventKind::MidiLearned, kSlotSynth, 0, &m));
}

void BridgeServer::error(u8 code) { broadcast(eventMessage(EventKind::Error, kSlotSynth, code)); }

} // namespace g2::bridge
