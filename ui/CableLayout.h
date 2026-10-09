// Cable shapes (g2::Cable::bend) saved beside a patch: the .pch2/.prf2 format
// has no place for them, and the synth and the original editor must keep
// reading our files, so they go to a small JSON "layout" file next to the
// patch ("Pad.pch2" -> "Pad.pch2.g2layout") and into the plugin's state.
//
//   {"format": "G2fresh cable layout", "version": 1,
//    "patches": [{"slot": 0, "cables": [
//        {"area": "va", "from": [module, connector, isOutput], "to": [module, connector],
//         "bend": [dx, dy]}]}]}
//
// "slot" is 0 for a patch, 0..3 (A..D) in a performance. Entries whose cable
// no longer exists are ignored.
#pragma once

#include "g2/patch.hpp"

#include <juce_core/juce_core.h>

#include <functional>
#include <utility>
#include <vector>

namespace g2ui::cablelayout {

// The layout of these patches (slot, patch), or "" when no cable is bent.
juce::String toJson(const std::vector<std::pair<int, const g2::Patch*>>& patches);
// Applies a layout: `patchFor(slot)` returns the patch of a slot, or nullptr.
// Returns the number of cables shaped.
int apply(const juce::String& json, const std::function<g2::Patch*(int slot)>& patchFor);

// The layout file of a patch or performance file.
inline juce::File fileFor(const juce::File& patchFile)
{
    return patchFile.getSiblingFile(patchFile.getFileName() + ".g2layout");
}

} // namespace g2ui::cablelayout
