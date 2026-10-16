#pragma once
// The Patch Mutators of a document: one per patch, as the original's CMutaSynthData belongs to its CPatch. A
// performance has one per slot; a new or loaded document (PatchDocument::generation) starts with fresh ones. Plain C++.

#include "g2/mutate.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace g2ui {

class MutatorSet {
public:
    // The Mutator of `slot` (0 for a patch) of document `generation`, made for `patch` the first time.
    g2::mutate::Mutator& get(int slot, int generation, const g2::Patch& patch, std::uint32_t seed = 1)
    {
        if (generation != generation_) {
            for (auto& m : mutators_)
                m.reset();
            generation_ = generation;
        }
        auto& m = mutators_[static_cast<std::size_t>(slot < 0 || slot > 3 ? 0 : slot)];
        if (!m)
            m.emplace(patch, seed + static_cast<std::uint32_t>(slot));
        return *m;
    }

private:
    std::array<std::optional<g2::mutate::Mutator>, 4> mutators_;
    int generation_ = -1;
};

} // namespace g2ui
