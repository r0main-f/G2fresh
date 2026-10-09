#include "g2/module_db.hpp"

#include "g2/param_text.hpp"

#include <array>
#include <iterator>

namespace g2::db {

extern const ModuleDef kModules[];
extern const CategoryDef kCategories[];
extern const std::size_t kModuleCount;
extern const std::size_t kCategoryCount;

std::span<const ModuleDef> modules() { return {kModules, kModuleCount}; }
std::span<const CategoryDef> categories() { return {kCategories, kCategoryCount}; }

const ModuleDef* find(std::uint8_t typeId)
{
    static const auto index = [] {
        std::array<const ModuleDef*, 256> t{};
        for (const auto& m : modules())
            t[m.typeId] = &m;
        return t;
    }();
    return index[typeId];
}

std::string formatParam(const ModuleDef& def, std::size_t param,
                        std::span<const std::uint8_t> paramValues,
                        std::span<const std::uint8_t> modeValues)
{
    if (param >= def.params.size() || param >= paramValues.size())
        return {};
    const ParamDef& p = def.params[param];
    std::array<std::uint8_t, 3> args{paramValues[param], 0, 0};
    std::size_t n = 1;
    for (std::uint8_t dep : p.deps) {
        if (dep == kNoDep)
            break;
        const auto source = (dep & kModeDep) ? modeValues : paramValues;
        const std::size_t i = dep & ~kModeDep;
        args[n++] = i < source.size() ? source[i] : 0;
    }
    return paramtext::format(p.textFunc, std::span<const std::uint8_t>(args.data(), n));
}

} // namespace g2::db
