#include "g2/proto/bubble.hpp"

#include <stdexcept>

namespace g2::proto {

Bubble Bubble::synth(std::vector<Molecule> molecules) { return {kSlotSynth, std::nullopt, false, std::move(molecules)}; }

Bubble Bubble::performance(u8 perfSession, std::vector<Molecule> molecules)
{
    return {kSlotSynth, perfSession, false, std::move(molecules)};
}

Bubble Bubble::patch(u8 slot, u8 slotSession, std::vector<Molecule> molecules)
{
    return {slot, slotSession, false, std::move(molecules)};
}

Bubble Bubble::patchVoid(u8 slot, std::vector<Molecule> molecules)
{
    return {slot, std::nullopt, false, std::move(molecules)};
}

Bubble Bubble::realtimeFor(u8 slot, u8 slotSession, std::vector<Molecule> molecules)
{
    return {slot, slotSession, true, std::move(molecules)};
}

std::vector<std::vector<u8>> framesFor(const Bubble& b)
{
    if (b.slot > kSlotSynth)
        throw std::invalid_argument("bubble slot out of 0..4");
    std::vector<std::vector<u8>> frames;
    if (b.realtime) {
        const u8 session = b.session.value_or(0) & kSessionMask;
        for (const auto& m : b.molecules) {
            std::vector<u8> bytes;
            encode(m, bytes);
            frames.push_back(hostFrame(static_cast<u8>(kBubbleBegin | kRealtime | kBubbleEnd | b.slot), session, bytes));
        }
        return frames;
    }

    // Molecules back to back, cut into frames that fit 0xFFFF bytes.
    constexpr std::size_t kMaxPayload = kMaxFrame - 7;
    std::vector<std::vector<u8>> payloads(1);
    std::vector<int> counts(1, 0);
    for (const auto& m : b.molecules) {
        std::vector<u8> bytes;
        encode(m, bytes);
        if (bytes.size() > kMaxPayload)
            throw std::length_error("molecule longer than a USB frame");
        if (payloads.back().size() + bytes.size() > kMaxPayload) {
            payloads.emplace_back();
            counts.push_back(0);
        }
        payloads.back().insert(payloads.back().end(), bytes.begin(), bytes.end());
        ++counts.back();
    }
    for (std::size_t i = 0; i < payloads.size(); ++i) {
        u8 hdr = b.slot;
        if (i == 0)
            hdr |= kBubbleBegin;
        if (i + 1 == payloads.size())
            hdr |= kBubbleEnd;
        const u8 session = b.session ? static_cast<u8>(*b.session & kSessionMask)
                                     : static_cast<u8>(kVoidSession | (counts[i] & kSessionMask));
        frames.push_back(hostFrame(hdr, session, payloads[i]));
    }
    return frames;
}

} // namespace g2::proto
