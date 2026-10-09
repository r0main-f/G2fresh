// Parameter randomizer and Patch Mutator. Addresses refer to the Mac editor
// v1.62 (G2Editor_i386); see re/notes/randomize-mutate.md.
#include "g2/mutate.hpp"

#include "g2/edit.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>

namespace g2::mutate {
namespace {

#include "mutate_classes.inc"

constexpr double kRandMax = 2147483647.0;

// ---- BSD random() seeding --------------------------------------------------------

std::int32_t goodRand(std::int32_t x)
{
    // Park-Miller "minimal standard" (Schrage's method), as libc's good_rand.
    if (x == 0)
        x = 123459876;
    const std::int32_t hi = x / 127773, lo = x % 127773;
    x = 16807 * lo - 2836 * hi;
    if (x < 0)
        x += 0x7fffffff;
    return x;
}

// ---- Curves (ApplySingleCurve @001d11ba, ApplyDualCurve @001d1278) ---------------

double powCurve(double t, double p)
{
    return p >= 0.0 ? std::pow(t, p + 1.0) : std::pow(t, -1.0 / (p - 1.0));
}

double singleCurve(double p, double x, int n)
{
    if (x < 0.0)
        return -singleCurve(p, -x, n);
    const double span = n + 0.99;
    return powCurve(x / span, p) * span;
}

double dualCurve(double pLow, double pHigh, double x, int n)
{
    const double span = n + 0.99;
    double t = x - span * 0.5;
    t = (t + t) / span;
    double r;
    if (t >= 0.0)
        r = t > 0.0 ? powCurve(t, pHigh) + 1.0 : 1.0;
    else
        r = 1.0 - powCurve(-t, pLow);
    return r * span * 0.5;
}

} // namespace

// ApplyCurve @001d199c and ApplyCurve1..4. `x < 0` asks for a fresh random
// value (Randomize, x = range = -1), otherwise x moves by up to `range`.
double applyCurve(Curve curve, int n, double x, double range, Random& rng)
{
    const bool fresh = x < 0.0;
    switch (curve) {
    case Curve::Low:    // ApplyCurve2 @001d189e
    case Curve::High: { // ApplyCurve3 @001d17a0
        const double warp = curve == Curve::Low ? -1.5 : 1.5;
        double y;
        if (fresh) {
            y = rng.unit() * (n + 0.999);
        } else {
            const double lo = singleCurve(warp, x - n * range, n);
            const double hi = singleCurve(warp, x + n * range, n);
            y = (hi - lo) * rng.unit() + lo;
        }
        return singleCurve(-warp, y, n);
    }
    case Curve::Centre: { // ApplyCurve4 @001d1690
        double y;
        if (fresh) {
            y = rng.unit() * (n + 0.999);
        } else {
            const double lo = dualCurve(-2.0, -2.0, x - n * range, n);
            const double hi = dualCurve(-2.0, -2.0, x + n * range, n);
            y = (hi - lo) * rng.unit() + lo;
        }
        return dualCurve(2.0, 2.0, y, n);
    }
    case Curve::Linear: // ApplyCurve1 @001d15ee
    default:
        if (fresh)
            return rng.unit() * (n + 0.999);
        return (range + range) * (n + 0.999) * (rng.unit() - 0.5) + x;
    }
}

namespace {

const ClassRow* classRow(u8 type)
{
    for (const auto& row : kClassRows)
        if (row.type == type)
            return &row;
    return nullptr;
}

// Param classes CMutaSynthData::IsEnabled @00141796 refuses (and the curve
// switch of CopyRandomizeContext/CopyMutateContext maps to curve 0).
constexpr std::uint8_t kFixedClasses[] = {
    0x00, 0x08, 0x09, 0x0a, 0x14, 0x15, 0x18, 0x19, 0x1e, 0x1f, 0x20, 0x24, 0x2c,
    0x31, 0x32, 0x34, 0x37, 0x3b, 0x3d, 0x3e, 0x40, 0x45, 0x46, 0x48, 0x4a, 0x4b,
    0x55, 0x5b, 0x5d, 0x5e, 0x5f, 0x60, 0x63, 0x67, 0x6a, 0x6f, 0x70, 0x72, 0x74,
    0x75, 0x7b, 0x7c, 0x7d, 0x7e, 0x80, 0x81, 0x84, 0x85, 0x90, 0x91, 0x9f,
};

constexpr u8 kClassFreqCoarse = 0x02;
constexpr u8 kClassTuneMode = 0x90;
constexpr u8 kTuneModePartial = 3;

int paramMax(const Module& m, std::size_t p)
{
    const auto* def = m.def();
    return def && p < def->params.size() ? def->params[p].max : 0;
}

ModuleGenes genesOf(u8 index, u8 type, const std::vector<std::vector<u8>>& params, u8 variation,
                    Random* dither)
{
    ModuleGenes g;
    g.index = index;
    g.type = type;
    if (variation < params.size())
        for (u8 v : params[variation])
            g.values.push_back(v + (dither ? dither->unit() : 0.5));
    return g;
}

bool byIndex(const ModuleGenes& a, const ModuleGenes& b) { return a.index < b.index; }

// CopyRandomizeContext @001425e8 (fresh) / CopyMutateContext @00142984.
void varyArea(const Patch& patch, Location loc, std::vector<ModuleGenes>& genes, const Settings& s,
              Random& rng, const ModuleFilter& filter, bool fresh)
{
    const Area& area = patch.area(loc);
    for (auto& g : genes) {
        const Module* m = area.find(g.index);
        if (!m)
            continue; // the original would dereference a null module
        if (filter && !filter(loc, g.index))
            continue;
        const std::vector<double> source = g.values;
        for (std::size_t p = 0; p < g.values.size(); ++p) {
            if (!isEnabled(*m, p, s))
                continue;
            const int n = paramMax(*m, p);
            const u8 cls = paramClass(m->type, p).value_or(0);
            const double top = n + 0.999;
            if (fresh) {
                double r;
                std::int8_t tuneMode = 0;
                if (cls == kClassFreqCoarse)
                    for (std::size_t q = 0; q < source.size(); ++q)
                        if (paramClass(m->type, q) == kClassTuneMode)
                            tuneMode = static_cast<std::int8_t>(static_cast<int>(source[q]));
                if (cls == kClassFreqCoarse && tuneMode == kTuneModePartial) {
                    // Partials: skew away from the lowest ratios.
                    static const double minInput = dualCurve(-5.0, -2.0, 49.0, 127);
                    r = dualCurve(5.0, 2.0, minInput + rng.unit() * ((n - minInput) + 0.999), n);
                } else {
                    r = applyCurve(curveOf(cls), n, -1.0, -1.0, rng);
                }
                g.values[p] = r >= 0.0 || std::isnan(r) ? (r <= top ? r : top) : 0.0;
            } else {
                if (!(rng.next() < s.probability * kRandMax))
                    continue;
                double r = applyCurve(curveOf(cls), n, g.values[p], s.range, rng);
                if (r < 0.0)
                    r = -r;
                else if (r > top)
                    r = top - (r - top);
                g.values[p] = r;
            }
        }
    }
}

const MorphAssign* morphFor(const std::vector<MorphAssign>& morphs, Location loc, u8 module, u8 param,
                            u8 group)
{
    for (const auto& a : morphs)
        if (a.location == static_cast<u8>(loc) && a.module == module && a.param == param && a.morph == group)
            return &a;
    return nullptr;
}

// CopyMorphs @00141456: the copy stops once both maps exceed 25 assignments.
std::vector<MorphAssign> copyMorphs(const std::vector<MorphAssign>& from)
{
    std::vector<MorphAssign> out;
    for (const auto& a : from) {
        out.push_back(a);
        if (from.size() > 25 && out.size() > 25) {
            out.pop_back();
            break;
        }
    }
    return out;
}

constexpr Location kAreas[] = {Location::Settings, Location::Va, Location::Fx};

} // namespace

// ---- Random ---------------------------------------------------------------------------

void Random::reseed(std::uint32_t seed)
{
    state_[0] = seed;
    for (int i = 1; i < 31; ++i)
        state_[i] = static_cast<std::uint32_t>(goodRand(static_cast<std::int32_t>(state_[i - 1])));
    front_ = 3;
    rear_ = 0;
    for (int i = 0; i < 310; ++i)
        next();
}

std::int32_t Random::next()
{
    state_[front_] += state_[rear_];
    const auto r = static_cast<std::int32_t>((state_[front_] >> 1) & 0x7fffffff);
    if (++front_ >= 31) {
        front_ = 0;
        ++rear_;
    } else if (++rear_ >= 31) {
        rear_ = 0;
    }
    return r;
}

// ---- Classes and groups ------------------------------------------------------------------

std::optional<u8> paramClass(u8 moduleType, std::size_t param)
{
    const ClassRow* row = classRow(moduleType);
    if (!row || param >= row->count)
        return std::nullopt;
    return row->classes[param];
}

bool isMutableClass(u8 cls)
{
    return std::find(std::begin(kFixedClasses), std::end(kFixedClasses), cls) == std::end(kFixedClasses);
}

Curve curveOf(u8 cls)
{
    switch (cls) {
    case 0x02: case 0x03: case 0x0c: case 0x10: case 0x25: case 0x2d:
    case 0x3f: case 0x58: case 0x79: case 0x8a: case 0x9b: case 0x9c:
        return Curve::Centre;
    case 0x05: case 0x0b: case 0x0e: case 0x27: case 0x36: case 0x49:
    case 0x51: case 0x52: case 0x65: case 0x76: case 0x99:
        return Curve::Low;
    case 0x0f:
        return Curve::High;
    default:
        return Curve::Linear;
    }
}

std::optional<Group> groupOf(u8 type, u8 cls)
{
    switch (type) {
    case 12: case 63: case 89: case 94: case 98: case 118: case 150: case 167: case 192:
        return Effects;
    case 172: case 173: case 174: case 175: case 176: case 177: case 178: case 179: case 181: case 182:
        return Delays;
    default:
        break;
    }
    switch (cls) {
    case 0x02: case 0x04: case 0x4f: case 0x50: case 0x56: case 0x57:
        return OscFreq;
    case 0x03: case 0x58:
        return OscFine;
    case 0x3a: case 0x3c: case 0x3f: case 0x64:
        return Mixer;
    case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x10: case 0x47: case 0x49: case 0x52: case 0x9d: case 0x9e:
        return Envelope;
    case 0x30: case 0x33:
        return SeqValue;
    case 0x2f:
        return SeqEvent;
    default:
        return std::nullopt;
    }
}

double rangeForProbability(double p)
{
    // CMutaSynthData::SetMutationRangeFromProb @00141050
    double r = ((std::sqrt(p) - 0.22) / 0.78) * -0.53 + 0.7;
    r *= r;
    return r >= 0.0 || std::isnan(r) ? (r > 0.5 ? 0.5 : r) : 0.0;
}

double probabilityForRange(double r)
{
    // CMutaSynthData::SetMutationProbFromRange @00140fe4
    double p = ((std::sqrt(r) - 0.7) / 0.53) * -0.78 + 0.22;
    p *= p;
    return p >= 0.0 || std::isnan(p) ? (p > 1.0 ? 1.0 : p) : 0.0;
}

void Settings::setProbability(double p)
{
    probability = p;
    if (link)
        range = rangeForProbability(p);
}

void Settings::setRange(double r)
{
    range = r;
    if (link)
        probability = probabilityForRange(r);
}

void Settings::setLink(bool on)
{
    link = on;
    if (on)
        range = rangeForProbability(probability);
}

void Settings::setQuickLock(Group g, bool locked)
{
    if (solo)
        return;
    unlocked = locked ? static_cast<u8>(unlocked & ~g) : static_cast<u8>(unlocked | g);
}

void Settings::setSolo(Group g, bool on)
{
    solo = on ? static_cast<u8>(solo | g) : static_cast<u8>(solo & ~g);
}

bool isEnabled(const Module& module, std::size_t param, const Settings& s)
{
    if (module.locked)
        return false;
    const auto cls = paramClass(module.type, param);
    if (!cls || !isMutableClass(*cls))
        return false;
    const u8 mask = s.solo ? s.solo : s.unlocked;
    if (const auto g = groupOf(module.type, *cls))
        return (mask & *g) != 0;
    return s.solo == 0;
}

// ---- Individuals -------------------------------------------------------------------

bool Individual::operator==(const Individual& o) const
{
    auto same = [](const MorphAssign& a, const MorphAssign& b) {
        return a.location == b.location && a.module == b.module && a.param == b.param && a.morph == b.morph &&
               a.range == b.range;
    };
    return settings == o.settings && va == o.va && fx == o.fx &&
           std::equal(morphs.begin(), morphs.end(), o.morphs.begin(), o.morphs.end(), same);
}

std::vector<ModuleGenes>& Individual::area(Location loc)
{
    return loc == Location::Settings ? settings : loc == Location::Fx ? fx : va;
}

const std::vector<ModuleGenes>& Individual::area(Location loc) const
{
    return loc == Location::Settings ? settings : loc == Location::Fx ? fx : va;
}

const ModuleGenes* Individual::find(Location loc, u8 index) const
{
    for (const auto& g : area(loc))
        if (g.index == index)
            return &g;
    return nullptr;
}

std::vector<u8> Individual::quantized(Location loc, u8 index) const
{
    std::vector<u8> out;
    const ModuleGenes* g = find(loc, index);
    if (!g)
        return out;
    const auto* def = db::find(g->type);
    for (std::size_t p = 0; p < g->values.size(); ++p) {
        const int max = def && p < def->params.size() ? def->params[p].max : 255;
        const double f = std::floor(g->values[p]);
        out.push_back(static_cast<u8>(std::isnan(f) ? 0 : std::clamp(f, 0.0, static_cast<double>(max))));
    }
    return out;
}

Individual capture(const Patch& patch, u8 variation, Random* dither)
{
    Individual ind;
    for (const auto& m : patch.settings) {
        const auto* def = edit::settingDef(static_cast<edit::Setting>(m.index));
        ind.settings.push_back(genesOf(m.index, def ? def->typeId : 0, m.params, variation, dither));
    }
    for (Location loc : {Location::Va, Location::Fx})
        for (const auto& m : patch.area(loc).modules)
            ind.area(loc).push_back(genesOf(m.index, m.type, m.params, variation, dither));
    for (Location loc : kAreas)
        std::stable_sort(ind.area(loc).begin(), ind.area(loc).end(), byIndex);
    if (variation < patch.morphs.size())
        ind.morphs = patch.morphs[variation].assigns;
    return ind;
}

void apply(Patch& patch, u8 variation, const Individual& ind)
{
    auto write = [&](std::vector<std::vector<u8>>& params, const ModuleGenes& g, Location loc, u8 index) {
        if (variation >= params.size())
            return;
        const auto q = ind.quantized(loc, index);
        auto& dst = params[variation];
        for (std::size_t p = 0; p < q.size() && p < dst.size(); ++p)
            dst[p] = q[p];
        (void)g;
    };
    for (const auto& g : ind.settings)
        for (auto& m : patch.settings)
            if (m.index == g.index)
                write(m.params, g, Location::Settings, g.index);
    for (Location loc : {Location::Va, Location::Fx})
        for (const auto& g : ind.area(loc))
            if (Module* m = patch.area(loc).find(g.index))
                write(m->params, g, loc, g.index);

    if (patch.morphs.size() <= variation)
        patch.morphs.resize(variation + 1, {std::vector<u8>(kMorphGroups, 0), {}});
    auto& dst = patch.morphs[variation].assigns;
    dst.clear();
    std::set<std::tuple<u8, u8, u8>> seen; // one group per parameter in our model
    for (const auto& a : ind.morphs) {
        const auto loc = static_cast<Location>(a.location);
        std::size_t count = 0;
        if (loc == Location::Settings) {
            for (const auto& m : patch.settings)
                if (m.index == a.module && variation < m.params.size())
                    count = m.params[variation].size();
        } else if (loc == Location::Va || loc == Location::Fx) {
            if (const Module* m = patch.area(loc).find(a.module); m && variation < m->params.size())
                count = m->params[variation].size();
        }
        if (a.param < count && seen.insert({a.location, a.module, a.param}).second)
            dst.push_back(a);
    }
}

// ---- Operators -----------------------------------------------------------------------------

Individual randomized(const Patch& patch, const Individual& parent, const Settings& s, Random& rng,
                      const ModuleFilter& filter)
{
    Individual child = parent;
    child.morphs = copyMorphs(parent.morphs);
    varyArea(patch, Location::Va, child.va, s, rng, filter, true);
    varyArea(patch, Location::Fx, child.fx, s, rng, filter, true);
    return child;
}

Individual mutated(const Patch& patch, const Individual& parent, const Settings& s, Random& rng,
                   const ModuleFilter& filter)
{
    Individual child = parent;
    child.morphs = copyMorphs(parent.morphs);
    varyArea(patch, Location::Va, child.va, s, rng, filter, false);
    varyArea(patch, Location::Fx, child.fx, s, rng, filter, false);
    return child;
}

Individual recombined(const Individual& mother, const Individual& father, double crossover, Random& rng)
{
    // RecombineContext @00142288, once per area (settings, VA, FX).
    Individual child;
    for (Location loc : kAreas) {
        unsigned useFather = static_cast<unsigned>((rng.next() * 1.9999) / kRandMax);
        for (const auto& mg : mother.area(loc)) {
            ModuleGenes g = mg;
            const ModuleGenes* fg = father.find(loc, mg.index);
            for (std::size_t p = 0; p < g.values.size(); ++p) {
                const bool fromFather = useFather != 0 && fg && p < fg->values.size();
                const Individual& src = fromFather ? father : mother;
                if (fromFather)
                    g.values[p] = fg->values[p];
                for (u8 group = 0; group < kMorphGroups; ++group)
                    if (const auto* a = morphFor(src.morphs, loc, mg.index, static_cast<u8>(p), group))
                        child.morphs.push_back(*a);
                if (rng.next() / kRandMax < crossover)
                    useFather = useFather == 0;
            }
            child.area(loc).push_back(std::move(g));
        }
    }
    return child;
}

Individual interpolated(const Individual& mother, const Individual& father, double t)
{
    // InterpolateContext @00141f84.
    Individual child;
    for (Location loc : kAreas) {
        for (const auto& mg : mother.area(loc)) {
            ModuleGenes g = mg;
            const ModuleGenes* fg = father.find(loc, mg.index);
            for (std::size_t p = 0; p < g.values.size(); ++p) {
                const double a = mg.values[p];
                const double b = fg && p < fg->values.size() ? fg->values[p] : a;
                g.values[p] = a + (b - a) * t;
                for (u8 group = 0; group < kMorphGroups; ++group) {
                    const auto* ma = morphFor(mother.morphs, loc, mg.index, static_cast<u8>(p), group);
                    const auto* fa = morphFor(father.morphs, loc, mg.index, static_cast<u8>(p), group);
                    const int da = ma ? ma->range : 0, db = fa ? fa->range : 0;
                    if (da == 0 && db == 0)
                        continue;
                    const auto d = static_cast<std::int8_t>(static_cast<int>(da + (db - da) * t));
                    if (d != 0)
                        child.morphs.push_back({static_cast<u8>(loc), mg.index, static_cast<u8>(p), group, d});
                }
            }
            child.area(loc).push_back(std::move(g));
        }
    }
    return child;
}

// ---- In-place convenience ------------------------------------------------------------------

namespace {

void inPlace(Patch& patch, const Scope& scope, const Settings& s, Random& rng, bool fresh)
{
    ModuleFilter filter;
    if (!scope.modules.empty())
        filter = [&](Location loc, u8 index) {
            return std::find(scope.modules.begin(), scope.modules.end(), std::pair{loc, index}) != scope.modules.end();
        };
    std::vector<u8> variations;
    if (scope.variation)
        variations.push_back(*scope.variation);
    else
        for (u8 v = 0; v < kUserVariations; ++v)
            variations.push_back(v);
    for (u8 v : variations) {
        const Individual parent = capture(patch, v, &rng);
        apply(patch, v, fresh ? randomized(patch, parent, s, rng, filter) : mutated(patch, parent, s, rng, filter));
    }
}

} // namespace

void randomize(Patch& patch, const Scope& scope, const Settings& s, Random& rng)
{
    inPlace(patch, scope, s, rng, true);
}

void mutate(Patch& patch, const Scope& scope, const Settings& s, Random& rng)
{
    inPlace(patch, scope, s, rng, false);
}

// ---- Mutator ----------------------------------------------------------------------------------

Mutator::Mutator(const Patch& patch, std::uint32_t seed) : rng_(seed)
{
    for (u8 v = 0; v < kFileVariations; ++v)
        mirror_.push_back(capture(patch, v, &rng_));
}

void Mutator::sync(const Patch& patch)
{
    for (u8 v = 0; v < mirror_.size(); ++v) {
        Individual fresh = capture(patch, v, nullptr); // v + 0.5
        Individual& old = mirror_[v];
        for (Location loc : kAreas) {
            for (auto& g : fresh.area(loc)) {
                const ModuleGenes* o = old.find(loc, g.index);
                const auto q = old.quantized(loc, g.index);
                for (std::size_t p = 0; p < g.values.size(); ++p) {
                    const u8 now = static_cast<u8>(g.values[p] - 0.5);
                    if (o && o->type == g.type && p < q.size() && q[p] == now)
                        g.values[p] = o->values[p]; // unchanged: keep the fraction
                    else if (!o || o->type != g.type)
                        g.values[p] = now + rng_.unit(); // new module (CMutaSynthData::AddModule)
                }
            }
        }
        old = std::move(fresh);
    }
    // Removed modules disappear from every box (CMutaSynthData::RemoveModule).
    auto prune = [&](std::optional<Individual>& box) {
        if (!box)
            return;
        for (Location loc : {Location::Va, Location::Fx}) {
            auto& genes = box->area(loc);
            std::erase_if(genes, [&](const ModuleGenes& g) {
                const Module* m = patch.area(loc).find(g.index);
                return !m || m->type != g.type;
            });
        }
    };
    for (auto& b : population)
        prune(b);
    for (auto& b : geneBank)
        prune(b);
}

std::optional<Individual>* Mutator::slot(Box b)
{
    if (b.kind == BoxKind::Population && b.index >= 0 && b.index < static_cast<int>(population.size()))
        return &population[b.index];
    if (b.kind == BoxKind::GeneBank && b.index >= 0 && b.index < kGeneBankSize)
        return &geneBank[b.index];
    return nullptr;
}

const std::optional<Individual>* Mutator::slot(Box b) const
{
    return const_cast<Mutator*>(this)->slot(b);
}

bool Mutator::hasData(Box b) const
{
    if (b.kind == BoxKind::Variation)
        return b.index >= 0 && b.index < static_cast<int>(mirror_.size());
    const auto* s = slot(b);
    return s && s->has_value();
}

std::optional<Individual> Mutator::get(Box b) const
{
    if (b.kind == BoxKind::Variation)
        return hasData(b) ? std::optional<Individual>(mirror_[b.index]) : std::nullopt;
    const auto* s = slot(b);
    return s ? *s : std::nullopt;
}

void Mutator::setChildren(std::array<Individual, kChildren>&& kids, int parents)
{
    for (int i = 0; i < kChildren; ++i)
        population[i] = std::move(kids[i]);
    parentCount = parents;
    focus = Box{BoxKind::Population, 0};
}

void Mutator::randomize(const Patch& patch, u8 variation)
{
    // GetRandomizedChildrenMolecules @00143ae8: from the focused variation.
    if (variation >= mirror_.size())
        throw std::invalid_argument("no such variation");
    const Individual parent = mirror_[variation];
    std::array<Individual, kChildren> kids;
    for (auto& k : kids)
        k = randomized(patch, parent, settings, rng_);
    setChildren(std::move(kids), 1);
}

bool Mutator::mutate(const Patch& patch, Box parentBox)
{
    // GetMutatedChildrenMolecules @00145100.
    const auto parent = get(parentBox);
    if (!parent)
        return false;
    if (parentBox != kMother)
        population[kMotherBox] = *parent;
    std::array<Individual, kChildren> kids;
    for (auto& k : kids)
        k = mutated(patch, *parent, settings, rng_);
    setChildren(std::move(kids), 1);
    return true;
}

bool Mutator::cross(Box motherBox, Box fatherBox)
{
    // GetRecombinedChildrenMolecules @00144c3a.
    const auto mother = get(motherBox), father = get(fatherBox);
    if (!mother || !father)
        return false;
    if (motherBox != kMother)
        population[kMotherBox] = *mother;
    if (fatherBox != kFather)
        population[kFatherBox] = *father;
    std::array<Individual, kChildren> kids;
    for (auto& k : kids)
        k = recombined(*mother, *father, settings.crossover, rng_);
    setChildren(std::move(kids), 2);
    return true;
}

bool Mutator::interpolate(Box motherBox, Box fatherBox)
{
    // GetInterpolatedChildrenMolecules @00144748.
    const auto mother = get(motherBox), father = get(fatherBox);
    if (!mother || !father)
        return false;
    if (motherBox != kMother)
        population[kMotherBox] = *mother;
    if (fatherBox != kFather)
        population[kFatherBox] = *father;
    std::array<Individual, kChildren> kids;
    for (int i = 0; i < kChildren; ++i)
        kids[i] = interpolated(*mother, *father, (i + 1.0) / 7.0);
    setChildren(std::move(kids), 2);
    return true;
}

void Mutator::copy(Patch& patch, Box from, Box to)
{
    // GetCopyIndividMolecules @00144528 (with focus).
    const auto ind = get(from);
    if (!ind)
        return;
    if (to.kind == BoxKind::Variation) {
        if (to.index < 0 || to.index >= static_cast<int>(mirror_.size()))
            return;
        apply(patch, static_cast<u8>(to.index), *ind);
        mirror_[to.index] = *ind;
        focus.reset();
        return;
    }
    if (auto* s = slot(to)) {
        *s = *ind;
        focus = to;
    }
}

void Mutator::move(Patch& patch, Box from, Box to)
{
    // EndDrag @0013f11a: a plain drag between gene bank boxes moves
    // (GetMoveIndividMolecules @00144412); any other drag copies.
    if (from.kind != BoxKind::GeneBank || to.kind != BoxKind::GeneBank) {
        copy(patch, from, to);
        return;
    }
    if (from == to || !hasData(from))
        return;
    auto* src = slot(from);
    *slot(to) = std::move(*src);
    src->reset();
    focus = to;
}

void Mutator::clear(Box b)
{
    // GetClearIndividMolecules @00143190: gene bank, or population boxes > 5.
    const bool allowed = (b.kind == BoxKind::GeneBank && b.index >= 0) ||
                         (b.kind == BoxKind::Population && b.index > 5);
    auto* s = allowed ? slot(b) : nullptr;
    if (!s || !s->has_value())
        return;
    s->reset();
    if (focus == b)
        focus.reset();
}

void Mutator::copyRowToVariations(Patch& patch, int row)
{
    // HandleChangeRequests @0013f7b2, "Copy to Variations": slot row*8+i to variation i.
    if (row < 0 || row > 2)
        return;
    bool any = false;
    for (int i = 0; i < 8; ++i)
        if (const auto& ind = geneBank[row * 8 + i]) {
            apply(patch, static_cast<u8>(i), *ind);
            mirror_[i] = *ind;
            any = true;
        }
    if (any)
        focus.reset();
}

void Mutator::clearRow(int row)
{
    if (row < 0 || row > 2)
        return;
    bool any = false;
    for (int i = 0; i < 8; ++i)
        if (geneBank[row * 8 + i]) {
            geneBank[row * 8 + i].reset();
            any = true;
        }
    if (any)
        focus.reset();
}

bool Mutator::storeFocused()
{
    // CPatchView::KeyMutaStoreChild @000f89e0.
    if (!focus || focus->kind != BoxKind::Population)
        return false;
    const auto ind = get(*focus);
    if (!ind)
        return false;
    for (auto& s : geneBank)
        if (!s) {
            s = *ind;
            return true;
        }
    return false;
}

} // namespace g2::mutate
