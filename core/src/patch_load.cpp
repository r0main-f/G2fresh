// Port of the original editor's CPatchLoad (v1.62, 000ed144..000eeaae).
// See re/notes/patch-load.md for addresses and the reasoning.
#include "g2/patch_load.hpp"

namespace g2::patchload {

ResourceSpec& ResourceSpec::operator+=(const ResourceSpec& o)
{
    // SModuleResourceSpec::operator+= @000ed18c; byte +5 is not summed.
    cyclesA = static_cast<std::uint16_t>(cyclesA + o.cyclesA);
    cyclesB = static_cast<std::uint16_t>(cyclesB + o.cyclesB);
    zpMem = static_cast<std::uint8_t>(zpMem + o.zpMem);
    xMemA = static_cast<std::uint16_t>(xMemA + o.xMemA);
    yMemA = static_cast<std::uint16_t>(yMemA + o.yMemA);
    pMemA = static_cast<std::uint16_t>(pMemA + o.pMemA);
    xMemB = static_cast<std::uint16_t>(xMemB + o.xMemB);
    yMemB = static_cast<std::uint16_t>(yMemB + o.yMemB);
    pMemB = static_cast<std::uint16_t>(pMemB + o.pMemB);
    dynRam += o.dynRam;
    qMem += o.qMem;
    rMem += o.rMem;
    return *this;
}

ResourceSpec& ResourceSpec::operator-=(const ResourceSpec& o)
{
    cyclesA = static_cast<std::uint16_t>(cyclesA - o.cyclesA);
    cyclesB = static_cast<std::uint16_t>(cyclesB - o.cyclesB);
    zpMem = static_cast<std::uint8_t>(zpMem - o.zpMem);
    xMemA = static_cast<std::uint16_t>(xMemA - o.xMemA);
    yMemA = static_cast<std::uint16_t>(yMemA - o.yMemA);
    pMemA = static_cast<std::uint16_t>(pMemA - o.pMemA);
    xMemB = static_cast<std::uint16_t>(xMemB - o.xMemB);
    yMemB = static_cast<std::uint16_t>(yMemB - o.yMemB);
    pMemB = static_cast<std::uint16_t>(pMemB - o.pMemB);
    dynRam -= o.dynRam;
    qMem -= o.qMem;
    rMem -= o.rMem;
    return *this;
}

ResourceSpec moduleCost(std::uint8_t type, bool uprate)
{
    const ResourceSpec* spec = moduleSpec(type);
    if (!spec)
        return {};
    ResourceSpec c = *spec;
    c.unused5 = 0; // AddModule only takes the low byte of the ZP word
    if (uprate) {
        // CPatchLoad::AddModule @000ee98a (same as B2AConvertResourceSpec @000ed306):
        // an uprated module runs its B code at the audio rate.
        c.cyclesA = static_cast<std::uint16_t>(c.cyclesA + c.cyclesB);
        c.pMemA = static_cast<std::uint16_t>(c.pMemA + c.pMemB);
        c.xMemA = static_cast<std::uint16_t>(c.xMemA + c.xMemB);
        c.yMemA = static_cast<std::uint16_t>(c.yMemA + c.yMemB);
        c.cyclesB = c.xMemB = c.yMemB = c.pMemB = 0;
    }
    return c;
}

namespace {

// The getters (000ed522..000ed924) compute in double precision (SSE2) and
// round to float; RAM, Q and R finish in single precision. None of the
// expressions has a multiply-add that contraction could change (B * 0.25 is
// exact), so the results are bit-identical to the original's.
float cyclesPercent(const ResourceSpec& s)
{
    const double a = static_cast<float>(s.cyclesA);
    const double b = static_cast<float>(s.cyclesB);
    return static_cast<float>((a + b * 0.25) * 100.0 / static_cast<double>(kMaxCycles));
}

float memPercent(std::uint16_t a, std::uint16_t b, std::uint32_t limit)
{
    const int sum = static_cast<int>(a) + static_cast<int>(b);
    return static_cast<float>(static_cast<double>(sum) * 100.0 / static_cast<double>(limit));
}

float zpPercent(const ResourceSpec& s)
{
    return static_cast<float>(static_cast<double>(s.zpMem) * 100.0 * 0.0078125); // 1/128
}

float ramPercent(const ResourceSpec& s)
{
    return static_cast<float>(static_cast<double>(s.dynRam) * 100.0) * 7.62939453125e-06f; // 1/131072
}

float qPercent(const ResourceSpec& s)
{
    return static_cast<float>(static_cast<double>(s.qMem) * 100.0) / 260096.0f;
}

float rPercent(const ResourceSpec& s)
{
    return static_cast<float>(static_cast<double>(s.rMem) * 100.0) * 0.00390625f; // 1/256
}

} // namespace

float AreaLoad::percent(Resource r) const
{
    switch (r) {
    case Resource::Cycles: return cycles;
    case Resource::XMem: return xMem;
    case Resource::YMem: return yMem;
    case Resource::PMem: return pMem;
    case Resource::ZpMem: return zpMem;
    case Resource::DynRam: return dynRam;
    case Resource::QMem: return qMem;
    case Resource::RMem: return rMem;
    }
    return 0;
}

AreaLoad evaluate(const ResourceSpec& total)
{
    AreaLoad a;
    a.total = total;
    a.cycles = cyclesPercent(total);
    a.xMem = memPercent(total.xMemA, total.xMemB, kMaxXMem);
    a.yMem = memPercent(total.yMemA, total.yMemB, kMaxYMem);
    a.pMem = memPercent(total.pMemA, total.pMemB, kMaxPMem);
    a.zpMem = zpPercent(total);
    a.dynRam = ramPercent(total);
    a.qMem = qPercent(total);
    a.rMem = rPercent(total);

    // CPatchLoad::FindCriticalResourceType @000ede6c: cycles are not candidates;
    // X wins ties with Y, and later resources must be strictly larger.
    Resource crit = Resource::XMem;
    float best = a.xMem;
    if (best < a.yMem) {
        crit = Resource::YMem;
        best = a.yMem;
    }
    for (Resource r : {Resource::PMem, Resource::ZpMem, Resource::DynRam, Resource::QMem, Resource::RMem}) {
        const float v = a.percent(r);
        if (best < v) {
            crit = r;
            best = v;
        }
    }
    a.critical = crit;
    a.criticalPercent = best; // CalculateCriticalResource @000ee0c8
    return a;
}

ResourceSpec areaTotal(const Area& area)
{
    ResourceSpec t;
    for (const Module& m : area.modules)
        t += moduleCost(m.type, m.uprate);
    t.dynRam += static_cast<std::uint32_t>(kCableDynRam * area.cables.size());
    return t;
}

namespace {

Load assemble(const AreaLoad& va, const AreaLoad& fx, std::int32_t freeRam)
{
    Load l;
    l.va = va;
    l.fx = fx;
    l.vaCycles = va.cycles / 100.0f;
    l.vaMemory = va.criticalPercent / 100.0f;
    l.fxCycles = fx.cycles / 100.0f;
    l.fxMemory = fx.criticalPercent / 100.0f;
    l.freeDynamicRam = freeRam;
    return l;
}

std::int32_t freeDynamicRam(const ResourceSpec& va, const ResourceSpec& fx)
{
    // CPatchLoad::GetFreeDynamicRam @000ed144: 0x20000 - FX - VA - settings
    // (the settings area never receives modules: CPatchToolbarData::AddModule
    // does not call AddModuleToPatchLoad).
    return static_cast<std::int32_t>(kMaxDynRam - fx.dynRam - va.dynRam);
}

std::uint32_t freeAsUnsigned(const Patch& patch)
{
    return static_cast<std::uint32_t>(compute(patch).freeDynamicRam);
}

} // namespace

namespace {

AreaLoad evaluateArea(const Area& area)
{
    AreaLoad a = evaluate(areaTotal(area));
    if (area.modules.empty() && area.cables.empty()) {
        // The editor only picks the critical resource when a module or a cable
        // is added; an area that never had any keeps the constructor's
        // values (type 0, 0 %).
        a.critical = Resource::Cycles;
        a.criticalPercent = 0;
    }
    return a;
}

} // namespace

Load compute(const Patch& patch)
{
    const AreaLoad va = evaluateArea(patch.va);
    const AreaLoad fx = evaluateArea(patch.fx);
    return assemble(va, fx, freeDynamicRam(va.total, fx.total));
}

Load fromReports(const ResourceSpec& va, const ResourceSpec& fx)
{
    return assemble(evaluate(va), evaluate(fx), freeDynamicRam(va, fx));
}

bool canAddModule(const Patch& patch, Location area, std::uint8_t type)
{
    if (area == Location::Settings)
        return false;
    if (patch.area(area).modules.size() >= static_cast<std::size_t>(kMaxModulesPerArea))
        return false; // InternalNewModule: count + 1 must stay below 0x80
    const ResourceSpec* spec = moduleSpec(type);
    if (!spec)
        return false;
    return spec->dynRam <= freeAsUnsigned(patch);
}

bool canAddCable(const Patch& patch)
{
    return freeAsUnsigned(patch) > 2;
}

std::uint16_t dynamicSize(std::span<const Module> modules, std::size_t cableCount)
{
    // CPatchData::GetDynamicSize @000e56a0: a 16-bit sum of 3 per cable segment
    // and the low 16 bits of each module's dynamic RAM (uprate does not matter).
    std::uint16_t size = 0;
    for (std::size_t i = 0; i < cableCount; ++i)
        size = static_cast<std::uint16_t>(size + kCableDynRam);
    for (const Module& m : modules)
        if (const ResourceSpec* spec = moduleSpec(m.type))
            size = static_cast<std::uint16_t>(size + spec->dynRam);
    return size;
}

bool canPaste(const Patch& patch, std::uint16_t size)
{
    return !(freeAsUnsigned(patch) < size);
}

std::optional<Report> parseReport(std::span<const std::uint8_t> p)
{
    // 1 + 2 + 2 + 1 + 2 + 12 + 2 + 4 + 2 bytes.
    if (p.size() < 28)
        return std::nullopt;
    std::size_t i = 0;
    auto u8 = [&] { return p[i++]; };
    auto u14 = [&] {
        const unsigned v = (static_cast<unsigned>(p[i] & 0x7F) << 7) | (p[i + 1] & 0x7F);
        i += 2;
        return static_cast<std::uint16_t>(v);
    };
    auto u16 = [&] {
        const unsigned v = (static_cast<unsigned>(p[i]) << 8) | p[i + 1];
        i += 2;
        return static_cast<std::uint16_t>(v);
    };
    Report r;
    r.area = u8() != 0 ? Location::Va : Location::Fx;
    ResourceSpec& s = r.spec;
    s.cyclesA = u14();
    s.cyclesB = u14();
    s.zpMem = u8();
    s.unused5 = static_cast<std::uint8_t>(u14());
    s.xMemA = u14();
    s.yMemA = u14();
    s.pMemA = u14();
    s.xMemB = u14();
    s.yMemB = u14();
    s.pMemB = u14();
    s.dynRam = u14();
    const std::uint32_t hi = u16();
    s.qMem = (hi << 16) | u16();
    s.rMem = u14();
    return r;
}

} // namespace g2::patchload
