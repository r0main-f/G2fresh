#include "g2/focus.hpp"

#include <algorithm>
#include <cstdlib>

namespace g2::focus {

namespace {

const db::ModuleDef* defOf(const Patch& patch, Focus f)
{
    if (f.location == Location::Settings)
        return f.module >= static_cast<u8>(edit::Setting::Morph) && f.module <= static_cast<u8>(edit::Setting::Misc)
            ? edit::settingDef(static_cast<edit::Setting>(f.module))
            : nullptr;
    const Module* m = patch.area(f.location).find(f.module);
    return m ? m->def() : nullptr;
}

bool hasParams(const Module& m)
{
    return m.def() && !m.def()->params.empty();
}

Focus walk(const Patch& patch, Focus f, int step)
{
    const auto* def = defOf(patch, f);
    if (!def || def->editOrder.empty())
        return f;
    const auto& order = def->editOrder;
    const auto at = std::find(order.begin(), order.end(), f.param);
    if (at == order.end())
        return f;
    const auto n = static_cast<int>(order.size());
    const int i = (static_cast<int>(at - order.begin()) + step + n) % n;
    return {f.location, f.module, order[static_cast<std::size_t>(i)]};
}

} // namespace

bool exists(const Patch& patch, Focus f)
{
    const auto* def = defOf(patch, f);
    return def && f.param < def->params.size();
}

Focus nextParam(const Patch& patch, Focus f)
{
    return walk(patch, f, +1);
}

Focus previousParam(const Patch& patch, Focus f)
{
    return walk(patch, f, -1);
}

Focus navModule(const Patch& patch, Focus f, Direction dir)
{
    if (f.location == Location::Settings)
        return f;
    std::vector<const Module*> mods;
    for (const auto& m : patch.area(f.location).modules)
        mods.push_back(&m);
    std::stable_sort(mods.begin(), mods.end(),
                     [](const Module* a, const Module* b) { return std::pair(a->col, a->row) < std::pair(b->col, b->row); });
    const auto here = std::find_if(mods.begin(), mods.end(), [&](const Module* m) { return m->index == f.module; });
    if (here == mods.end())
        return f;
    const Module& cur = **here;
    const auto column = [&](int col) {
        std::vector<std::size_t> in;
        for (std::size_t i = 0; i < mods.size(); ++i)
            if (mods[i]->col == col)
                in.push_back(i);
        return in;
    };
    std::size_t target = static_cast<std::size_t>(here - mods.begin());
    switch (dir) {
    case Direction::Up:
    case Direction::Down: {
        const auto in = column(cur.col);
        const auto at = std::find(in.begin(), in.end(), target) - in.begin();
        const auto n = static_cast<std::ptrdiff_t>(in.size());
        target = in[static_cast<std::size_t>((at + (dir == Direction::Down ? 1 : n - 1)) % n)];
        break;
    }
    case Direction::Right:
    case Direction::Left: {
        const bool right = dir == Direction::Right;
        int col = -1;
        for (const Module* m : mods)
            if (right ? m->col > cur.col && (col < 0 || m->col < col) : m->col < cur.col && (col < 0 || m->col > col))
                col = m->col;
        if (col < 0)
            return f; // at the edge
        std::size_t best = mods.size();
        for (const std::size_t i : column(col)) {
            if (best == mods.size()) {
                best = i;
                continue;
            }
            const int d = std::abs(mods[i]->row - cur.row), bestD = std::abs(mods[best]->row - cur.row);
            // The column is in row order: going right a tie keeps the upper one, going left it takes the lower one.
            if (d < bestD || (d == bestD && !right))
                best = i;
        }
        target = best;
        break;
    }
    }
    while (target < mods.size() && !hasParams(*mods[target]))
        ++target;
    if (target == mods.size())
        return f; // (the original reads past its list here)
    return {f.location, mods[target]->index, 0};
}

Focus onModule(const Patch& patch, Location loc, u8 module, Focus f)
{
    const Module* m = patch.area(loc).find(module);
    if (!m || !hasParams(*m) || (f.location == loc && f.module == module))
        return f;
    return {loc, module, 0};
}

Focus afterDelete(Focus f, Location loc, const std::vector<u8>& removed)
{
    if (f.location == loc && std::find(removed.begin(), removed.end(), f.module) != removed.end())
        return kDefault;
    return f;
}

bool stepValue(Patch& patch, Focus f, u8 variation, int delta)
{
    const auto* def = defOf(patch, f);
    const auto value = edit::targetValue(patch, f, variation);
    if (!def || !value || f.param >= def->params.size())
        return false;
    const auto& p = def->params[f.param];
    const int next = std::clamp(*value + delta, static_cast<int>(p.min), static_cast<int>(p.max));
    if (next == *value)
        return false;
    edit::setTargetValue(patch, f, variation, static_cast<u8>(next));
    return true;
}

bool stepMorph(Patch& patch, Focus f, u8 variation, u8 group, int delta)
{
    if (f.location == Location::Settings || group >= kMorphGroups
        || !edit::canMorph(patch, variation, f.location, f.module, f.param))
        return false;
    const auto range = edit::morphRange(patch, variation, f.location, f.module, f.param, group);
    const int from = range ? *range : 0;
    const int to = edit::clampMorphRange(patch, variation, f.location, f.module, f.param, from + delta);
    if (to == from)
        return false;
    edit::setMorph(patch, variation, f.location, f.module, f.param, group, to);
    return true;
}

} // namespace g2::focus
