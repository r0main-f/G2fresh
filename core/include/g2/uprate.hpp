// Module uprate (audio rate vs control rate), as the original editor computes
// it after every cable or module change (CPatchData::GenerateBandwidthChange-
// Molecules @000e5048, see re/notes/uprate.md).
//
// A module is uprated when one of its dynamic ("blue/red", "yellow/orange")
// inputs is fed, directly or through input-to-input links, by an audio-rate
// output: a static red (audio) output, or a dynamic output of an uprated
// module. The rule propagates down chains of dynamic modules until nothing
// changes. When a module's uprate flips, every cable on the nets of its
// connected dynamic outputs is recoloured to the new rate (red/blue or
// orange/yellow); other cables keep their colour, user-chosen ones included.
#pragma once

#include "g2/patch.hpp"

namespace g2::uprate {

// Recomputes the uprate bit of every module of the VA or FX area (a no-op for
// the settings area) and recolours the nets of the modules whose rate
// changed. Like the original, it starts from the current bits and walks the
// module list in creation order until a pass changes nothing, so a loop of
// dynamic modules that is already uprated stays uprated without an audio
// source. Call it after editing cables or adding/replacing modules, not after
// loading a file: files keep the bits they were saved with.
void update(Patch& patch, Location loc);

} // namespace g2::uprate
