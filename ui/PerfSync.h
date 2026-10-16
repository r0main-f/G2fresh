#pragma once
// A performance's settings between the editor and the synth (SynthSync). The settings are the header (11) without the
// slots' patch names (27 renames those) and the focused slot (09). Plain C++.

#include "g2/file.hpp"
#include "g2/patch.hpp"

namespace g2ui {

inline g2::file::PerfHeader settingsOf(g2::file::PerfHeader h)
{
    for (auto& slot : h.slots)
        slot.patchName.clear();
    h.focusedSlot = 0;
    return h;
}

// Whether two headers hold the same settings, compared as the bytes the synth gets.
inline bool sameSettings(const g2::file::PerfHeader& a, const g2::file::PerfHeader& b)
{
    auto bytes = [](const g2::file::PerfHeader& h) {
        return g2::file::encodeSection(g2::file::Section{g2::file::kPerfHeader, settingsOf(h), 0, {}});
    };
    return bytes(a) == bytes(b);
}

// The document takes the synth's settings, keeping its own slot names and focused slot.
inline void takeSettings(g2::file::PerfHeader& doc, const g2::file::PerfHeader& synth)
{
    auto h = synth;
    for (std::size_t s = 0; s < h.slots.size(); ++s)
        h.slots[s].patchName = doc.slots[s].patchName;
    h.focusedSlot = doc.focusedSlot;
    doc = h;
}

} // namespace g2ui
