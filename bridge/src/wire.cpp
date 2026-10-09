#include "g2/bridge/wire.hpp"

#include <algorithm>
#include <cstring>

namespace g2::bridge {

using namespace g2::proto;

// ---- Writer / Reader ---------------------------------------------------------

Writer::Writer(MessageType type)
{
    out_.resize(4); // the length, filled by finish()
    out_.push_back(static_cast<std::uint8_t>(type));
}

Writer& Writer::u8(std::uint8_t v)
{
    out_.push_back(v);
    return *this;
}

Writer& Writer::u16(std::uint16_t v)
{
    out_.push_back(static_cast<std::uint8_t>(v));
    out_.push_back(static_cast<std::uint8_t>(v >> 8));
    return *this;
}

Writer& Writer::u32(std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    return *this;
}

Writer& Writer::bytes(std::span<const std::uint8_t> data)
{
    u32(static_cast<std::uint32_t>(data.size()));
    out_.insert(out_.end(), data.begin(), data.end());
    return *this;
}

Writer& Writer::string(const std::string& s)
{
    return bytes({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
}

Writer& Writer::molecule(const Molecule& m)
{
    std::vector<std::uint8_t> encoded;
    try {
        encode(m, encoded);
    } catch (const std::exception&) {
        encoded.clear(); // a value the USB encoding cannot carry: sent as nothing
    }
    return bytes(encoded);
}

std::vector<std::uint8_t> Writer::finish()
{
    const auto n = static_cast<std::uint32_t>(out_.size() - 4);
    for (int i = 0; i < 4; ++i)
        out_[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(n >> (8 * i));
    return std::move(out_);
}

std::uint8_t Reader::u8()
{
    if (pos_ + 1 > data_.size()) {
        failed_ = true;
        return 0;
    }
    return data_[pos_++];
}

std::uint16_t Reader::u16()
{
    const auto lo = u8();
    const auto hi = u8();
    return static_cast<std::uint16_t>(lo | (hi << 8));
}

std::uint32_t Reader::u32()
{
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(u8()) << (8 * i);
    return v;
}

std::span<const std::uint8_t> Reader::bytes()
{
    const auto n = u32();
    if (failed_ || n > data_.size() - pos_) {
        failed_ = true;
        return {};
    }
    const auto out = data_.subspan(pos_, n);
    pos_ += n;
    return out;
}

std::string Reader::string()
{
    const auto b = bytes();
    return std::string(b.begin(), b.end());
}

std::optional<Molecule> Reader::molecule()
{
    const auto b = bytes();
    if (b.empty())
        return std::nullopt;
    auto molecules = decode(b);
    if (molecules.empty())
        return std::nullopt;
    return std::move(molecules.front());
}

// ---- MessageBuffer -----------------------------------------------------------

void MessageBuffer::append(std::span<const std::uint8_t> data)
{
    if (corrupt_)
        return;
    // Drop what was consumed before growing.
    if (pos_ > 0 && pos_ == data_.size()) {
        data_.clear();
        pos_ = 0;
    } else if (pos_ > (1u << 20)) {
        data_.erase(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
    data_.insert(data_.end(), data.begin(), data.end());
}

std::optional<MessageBuffer::Message> MessageBuffer::next()
{
    if (corrupt_ || data_.size() - pos_ < 4)
        return std::nullopt;
    std::uint32_t n = 0;
    for (int i = 0; i < 4; ++i)
        n |= static_cast<std::uint32_t>(data_[pos_ + static_cast<std::size_t>(i)]) << (8 * i);
    if (n == 0 || n > kMaxMessage) {
        corrupt_ = true;
        return std::nullopt;
    }
    if (data_.size() - pos_ - 4 < n)
        return std::nullopt;
    const auto* p = data_.data() + pos_ + 4;
    Message m{static_cast<MessageType>(p[0]), std::vector<std::uint8_t>(p + 1, p + n)};
    pos_ += 4 + n;
    return m;
}

// ---- Snapshots ---------------------------------------------------------------

namespace {

std::vector<Molecule> synthMolecules(const SynthState& s)
{
    std::vector<Molecule> out;
    out.push_back(SynthData{s.settings});
    out.push_back(Voices{s.voices});
    out.push_back(SlotFlags{id::SlotSelection, s.slotEnabled});
    out.push_back(SlotFlags{id::KeyboardFocus, s.keyboardEnabled});
    out.push_back(SlotFocus{s.slotFocus});
    out.push_back(NameDump{id::PerfName, s.perfName});
    out.push_back(SectionDump{file::Section{file::kPerfHeader, s.perfHeader, 0, {}}});
    out.push_back(SectionDump{file::Section{file::kGlobalKnobMap, file::KnobMap{s.globalKnobs}, 0, {}}});
    out.push_back(PageFocus{id::GlobalPageFocus, s.globalPage});
    out.push_back(s.clock);
    return out;
}

} // namespace

std::vector<std::uint8_t> synthSnapshot(const SynthState& s)
{
    Writer w(MessageType::Synth);
    w.bytes(encode(s.version));
    const auto molecules = synthMolecules(s);
    w.u32(static_cast<std::uint32_t>(molecules.size()));
    for (const auto& m : molecules)
        w.molecule(m);
    w.u8(s.midiLearn ? 1 : 0);
    if (s.midiLearn)
        w.molecule(*s.midiLearn);
    w.u8(s.flashUsage ? 1 : 0);
    w.u16(s.flashUsage.value_or(0));
    w.u8(s.perfSession).u8(s.perfSessionKnown ? 1 : 0);
    for (const auto& list : s.flash) {
        w.u32(static_cast<std::uint32_t>(list.size()));
        for (const auto& n : list)
            w.u8(n.bank).u8(n.prog).string(n.name).u8(n.category);
    }
    return w.finish();
}

bool applySynthSnapshot(SynthState& s, std::span<const std::uint8_t> payload)
{
    Reader r(payload);
    SynthState next;
    next.slots = s.slots;
    if (const auto v = decodeVersionInfo(r.bytes()))
        next.version = *v;
    const auto count = r.u32();
    for (std::uint32_t i = 0; i < count && !r.failed(); ++i)
        if (const auto m = r.molecule())
            applyToSynth(next, *m);
    if (r.u8())
        if (const auto m = r.molecule())
            if (const auto* l = std::get_if<MidiLearn>(&*m))
                next.midiLearn = *l;
    const bool hasUsage = r.u8() != 0;
    const auto usage = r.u16();
    if (hasUsage)
        next.flashUsage = usage;
    next.perfSession = r.u8();
    next.perfSessionKnown = r.u8() != 0;
    for (auto& list : next.flash) {
        const auto n = r.u32();
        for (std::uint32_t i = 0; i < n && !r.failed(); ++i) {
            FlashName f;
            f.bank = r.u8();
            f.prog = r.u8();
            f.name = r.string();
            f.category = r.u8();
            list.push_back(std::move(f));
        }
    }
    if (r.failed())
        return false;
    s = std::move(next);
    return true;
}

std::vector<std::uint8_t> slotSnapshot(const SynthState& s, int slot)
{
    const auto& st = s.slots[static_cast<std::size_t>(slot)];
    Writer w(MessageType::Slot);
    w.u8(static_cast<std::uint8_t>(slot));
    std::vector<std::uint8_t> patch;
    try {
        patch = savePatch(st.patch);
    } catch (const std::exception&) {
        patch.clear(); // cannot happen for a patch the client built; sent empty
    }
    w.bytes(patch).string(st.name).u8(st.session).u8(st.sessionKnown ? 1 : 0).u8(st.page);
    w.molecule(st.focus);
    for (const auto& load : st.load) {
        w.u8(load ? 1 : 0);
        if (load)
            w.molecule(*load);
    }
    w.bytes(st.leds);
    for (const auto m : st.meters)
        w.u16(m);
    return w.finish();
}

bool applySlotSnapshot(SynthState& s, std::span<const std::uint8_t> payload)
{
    Reader r(payload);
    const auto slot = r.u8();
    if (slot >= kSlots)
        return false;
    SlotState st;
    if (const auto patch = r.bytes(); !patch.empty()) {
        try {
            st.patch = loadPatch(patch);
        } catch (const std::exception&) {
            return false;
        }
    }
    st.name = r.string();
    st.session = r.u8();
    st.sessionKnown = r.u8() != 0;
    st.page = r.u8();
    if (const auto m = r.molecule())
        if (const auto* f = std::get_if<ParamFocus>(&*m))
            st.focus = *f;
    for (auto& load : st.load)
        if (r.u8())
            if (const auto m = r.molecule())
                if (const auto* l = std::get_if<PatchLoad>(&*m))
                    load = *l;
    const auto leds = r.bytes();
    std::copy_n(leds.begin(), std::min(leds.size(), st.leds.size()), st.leds.begin());
    for (auto& m : st.meters)
        m = r.u16();
    if (r.failed())
        return false;
    s.slots[slot] = std::move(st);
    return true;
}

void applyMirrored(SynthState& s, u8 target, const Molecule& m)
{
    if (target < kSlots) {
        applyToSlot(s.slots[target], m);
        return;
    }
    if (const auto* n = std::get_if<SessionNumber>(&m)) {
        if (n->slot < kSlots) {
            s.slots[n->slot].session = n->session & kSessionMask;
            s.slots[n->slot].sessionKnown = true;
        } else {
            s.perfSession = n->session & kSessionMask;
            s.perfSessionKnown = true;
        }
        return;
    }
    if (const auto* r = std::get_if<PerformanceRelease>(&m)) {
        s.perfSession = r->session & kSessionMask;
        s.perfSessionKnown = true;
        return;
    }
    applyToSynth(s, m); // MidiLearn included
}

} // namespace g2::bridge
