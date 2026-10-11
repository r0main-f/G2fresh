// Live editing: what goes to a synth slot when the editor's patch changes, as
// the original editor sends it (re/notes/usb-protocol.md §9).
//
// An edit operation records the original's molecules as it changes the patch
// (g2/live.hpp): a cable edit's deletes, connects and recolours, a module's
// creation with its sections, the make-room moves, the rates, and what undoes
// all of it. Those go out first, as one bubble (or, for the original's
// realtime pairs, as realtime messages). Whatever an edit did not record is
// found by comparing the patch with what the synth has, and goes out as the
// original sends each kind: parameter values (40) and morph ranges (43) as
// realtime messages; modes (2B), module names (33), colours (31), custom
// data (42), locks (90), knobs (26/25/2D), controllers (23/22), the variation
// (6A), the patch header (21), the textpad (6F) and the name (27) in one
// bubble. Only a change of modules or cables that nothing recorded needs the
// whole patch again.
#pragma once

#include "g2/live.hpp"
#include "g2/proto/molecules.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace g2::proto {

// The molecule the original writes for a recorded edit.
Molecule toMolecule(const live::Op& op);
std::vector<Molecule> toMolecules(const std::vector<live::Op>& ops);

// One message to send to a slot: realtime (a 40 or 43, never answered), or a
// bubble (answered).
struct Outgoing {
    bool realtime = false;
    std::vector<Molecule> molecules;
};

// A slot as the synth has it, and what brings it to the editor's patch.
class LiveSlot {
public:
    // The delay before a change of the voice count goes out, restarted by
    // each further change (CPatch::RequestPatchHeaderChange @000d62ae).
    static constexpr std::uint64_t kVoicesDelayMs = 600;

    LiveSlot() = default;
    LiveSlot(Patch synth, std::string name);

    const Patch& synth() const { return synth_; }
    Patch& synth() { return synth_; } // a change made on the synth
    const std::string& name() const { return name_; }

    // One change of the editor's patch (`now`, named `name`): the step's
    // recorded edit first (its undo when `undo`), then what it left out.
    std::vector<Outgoing> update(const Patch& now, const std::string& name, const live::Edit* edit, bool undo,
                                 std::uint64_t ms);
    // A delayed header that is due.
    std::vector<Outgoing> tick(const Patch& now, std::uint64_t ms);
    // The modules or cables differ in a way no message sent reached: the
    // whole patch has to go again (reset() after).
    bool needsUpload() const { return needsUpload_; }
    bool headerPending() const { return headerDue_.has_value(); }

private:
    void apply(const Molecule& m);
    void applyAll(const std::vector<Molecule>& molecules);

    Patch synth_;
    std::string name_;
    bool needsUpload_ = false;
    std::optional<std::uint64_t> headerDue_;
};

} // namespace g2::proto
