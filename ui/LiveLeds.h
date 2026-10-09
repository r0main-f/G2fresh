// The synth's LED and meter streams (re/notes/usb-protocol.md §10) mapped
// onto the modules, as the original editor does (CPatchBackground::
// AddBlinkModule / UpdateBlink, CPanel::GetLedGroupCnt / IsMutliLedGroup):
// - a module's LEDs (PANL Led and MiniVU elements) form groups by GroupId,
//   taken in group order;
// - a group of one plain LED is a single LED, fed by the 0x39 stream; a group
//   of several LEDs (a strip) or with a VU meter is fed by the 0x3A stream;
// - entries run over the VA modules then the FX modules, each by module
//   index, one entry per group of the module, at most 40 per stream.
#pragma once

#include "Skin.h"

#include "g2/patch.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace g2ui {

struct LedGroup {
    g2::Location location = g2::Location::Va;
    std::uint8_t module = 0;
    int group = 0;
};

struct LedMap {
    std::vector<LedGroup> single; // 0x39 entries
    std::vector<LedGroup> multi;  // 0x3A entries
};
LedMap ledMap(const g2::Patch& patch);

// A module's LED groups: whether each one is a multi group.
std::vector<bool> ledGroups(const PanelDef& panel);

// Live LED values for the area views (SynthSync while connected).
class LiveLeds {
public:
    virtual ~LiveLeds() = default;
    // The value of a module's LED group in the patch shown, or nullopt when
    // not live: a single LED 0..3; a strip: the lit LED, a bit mask when
    // (v & 0x3000) == 0x3000, all lit for 0xFFF; a VU meter: level 0..0x7E,
    // above = clip.
    virtual std::optional<int> ledValue(g2::Location location, std::uint8_t module, int group) const = 0;
    // Changes whenever values may have changed (the views repaint then).
    virtual std::uint32_t ledGeneration() const = 0;
    // Values are coming in (connected and bound): the views watch for changes
    // only then.
    virtual bool ledsLive() const = 0;
};

} // namespace g2ui
