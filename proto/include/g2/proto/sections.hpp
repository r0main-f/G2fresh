// Patches and performances on the wire: the .pch2 sections as molecules
// (re/notes/usb-protocol.md §8). Over USB the parameter lists and the morph
// map carry 10 variations instead of a file's 9 ("10 Variations must be in a
// patch for USB", Verhue Gen2 Mess:4016; what the 10th holds is unknown, it is
// written as zeros) [V/I].
#pragma once

#include "g2/patch.hpp"
#include "g2/proto/bubble.hpp"

#include <optional>
#include <string>
#include <vector>

namespace g2::proto {

inline constexpr u8 kUsbVariations = 10;

// The 18 sections of a patch as an upload carries them: file order, 10
// variations in the non-empty parameter lists and in the morph map.
std::vector<file::Section> uploadSections(const Patch& patch);
// Back to file form: drops a 10th variation (parameter lists, morph map).
std::vector<file::Section> fileSections(std::vector<file::Section> sections);

// The upload of a patch into a slot's edit buffer (§8.1): one T-void bubble,
// 37 00 00 00 <name> followed by the 18 sections (session byte 0x53).
Bubble patchUpload(u8 slot, const Patch& patch, const std::string& name);
// The upload of a performance: one synth bubble, 37 00 00 00 <name> + 1A
// (name, header section, 4 x 18 patch sections, global knob map) (0x42).
Bubble performanceUpload(const Performance& perf, const std::string& name);
// The sections of 1A for a performance (header, 4 x 18 upload sections, 5F).
std::vector<file::Section> performanceSections(const Performance& perf);
Performance performanceFromSections(std::span<const file::Section> sections);

// The location (0 FX, 1 VA, 2 settings) a section is for, for the ids that
// come once per location (4A, 4D, 52, 5A, 5B); nullopt for the others.
std::optional<u8> sectionLocation(const file::Section& s);

// Collects the section molecules of a download (the reply to 3C, or partial
// refreshes) and builds the patch once all 18 are there. A later section for
// the same (id, location) replaces the earlier one.
class PatchAssembler {
public:
    // Starts from a known patch (for partial refreshes).
    void reset(const Patch* from = nullptr);
    // Returns false for a section that is not part of a patch.
    bool add(const file::Section& section);
    bool complete() const;
    // The patch, in file form (9 variations). Throws FormatError if incomplete.
    Patch patch() const;

private:
    std::array<std::optional<file::Section>, 18> sections_;
};

} // namespace g2::proto
