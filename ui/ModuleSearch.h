#pragma once
// Finding a module by typing: G2fresh's own addition to the module bar (the original has its groups and buttons
// only, manual p59). Plain C++.
//
// Every word of the query must be found, case aside, in the module's short name, long name or description. The
// modules whose short name starts with the first word and holds every word come first, then those whose short name
// holds them all, then the long name, then the description; within each, the module bar's order (group, then the
// original toolbar's slot, then the name).

#include "g2/module_db.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace g2ui {

inline std::vector<const g2::db::ModuleDef*> searchModules(const std::string& query)
{
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    std::vector<std::string> words;
    {
        std::istringstream in(lower(query));
        for (std::string w; in >> w;)
            words.push_back(w);
    }
    if (words.empty())
        return {};
    auto all = [&](const std::string& text) {
        return std::all_of(words.begin(), words.end(), [&](const std::string& w) { return text.find(w) != std::string::npos; });
    };
    struct Hit {
        int tier;
        const g2::db::ModuleDef* def;
    };
    std::vector<Hit> hits;
    for (const auto& def : g2::db::modules()) {
        if (!def.selectable || def.kind != g2::db::ModuleKind::Module)
            continue;
        const auto shortName = lower(def.shortName ? def.shortName : "");
        const auto names = shortName + " " + lower(def.longName ? def.longName : "");
        const auto everything = names + " " + lower(def.description ? def.description : "");
        int tier = -1;
        if (all(shortName))
            tier = shortName.rfind(words.front(), 0) == 0 ? 0 : 1;
        else if (all(names))
            tier = 2;
        else if (all(everything))
            tier = 3;
        if (tier >= 0)
            hits.push_back({tier, &def});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        return std::tuple(a.tier, a.def->category, a.def->browserOrder, std::string(a.def->shortName))
             < std::tuple(b.tier, b.def->category, b.def->browserOrder, std::string(b.def->shortName));
    });
    std::vector<const g2::db::ModuleDef*> out;
    for (const auto& h : hits)
        out.push_back(h.def);
    return out;
}

} // namespace g2ui
