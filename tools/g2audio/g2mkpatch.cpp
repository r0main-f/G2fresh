// g2mkpatch: builds a small test patch with the core's edit API, for
// comparing the native engine (g2audio) with the emulated G2.
//
//   g2mkpatch OUT.pch2 ITEM...
//
// Items (applied in order, all in the VA area):
//   name=Type              add a module by short name (e.g. osc=OscA, out=2-Out)
//   name.Param=V           set a parameter (all variations) or a mode, by name
//   a.Out>b.In             cable from an output to an input (names or indices)
//   gain=V                 patch Gain level (patch settings, 0..127)
//   voices=N | mono        voice mode
//
// Example: g2mkpatch sine.pch2 osc=OscA osc.Wave=0 osc.Coarse=69 out=2-Out osc.0>out.0 osc.0>out.1
#include "g2/edit.hpp"
#include "g2/patch.hpp"
#include "g2/uprate.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace g2;

namespace {

const db::ModuleDef* defByName(const std::string& name)
{
    for (const auto& m : db::modules())
        if (m.kind == db::ModuleKind::Module && name == m.shortName)
            return &m;
    throw std::invalid_argument("unknown module type " + name);
}

template <class Span>
int indexByName(const Span& items, const std::string& name)
{
    for (std::size_t i = 0; i < items.size(); ++i)
        if (name == items[i].name)
            return static_cast<int>(i);
    if (!name.empty() && name.find_first_not_of("0123456789") == std::string::npos)
        return std::stoi(name);
    return -1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: g2mkpatch OUT.pch2 ITEM... (see the source header)\n";
        return 2;
    }
    try {
        Patch patch = Patch::makeDefault();
        std::map<std::string, u8> mods;
        u8 col = 0;
        const auto area = Location::Va;
        auto module = [&](const std::string& n) {
            const auto it = mods.find(n);
            if (it == mods.end())
                throw std::invalid_argument("unknown module " + n);
            return it->second;
        };
        for (int i = 2; i < argc; ++i) {
            const std::string item = argv[i];
            if (const auto gt = item.find('>'); gt != std::string::npos) {
                const auto a = item.substr(0, gt), b = item.substr(gt + 1);
                const auto da = a.find('.'), dbp = b.find('.');
                const u8 ma = module(a.substr(0, da)), mb = module(b.substr(0, dbp));
                const auto* defA = patch.va.find(ma)->def();
                const auto* defB = patch.va.find(mb)->def();
                const int o = indexByName(defA->outputs, a.substr(da + 1));
                const int in = indexByName(defB->inputs, b.substr(dbp + 1));
                if (o < 0 || in < 0)
                    throw std::invalid_argument("bad cable " + item);
                edit::connect(patch, area, {ma, u8(o), true}, {mb, u8(in), false});
                continue;
            }
            const auto eq = item.find('=');
            if (eq == std::string::npos) {
                if (item == "mono") {
                    edit::setVoices(patch, edit::VoiceMode::Mono);
                    continue;
                }
                throw std::invalid_argument("bad item " + item);
            }
            const auto key = item.substr(0, eq), val = item.substr(eq + 1);
            if (key == "gain") {
                for (u8 v = 0; v < kFileVariations; ++v)
                    edit::setSetting(patch, edit::Setting::Gain, 0, v, u8(std::stoi(val)));
            } else if (key == "voices") {
                edit::setVoices(patch, edit::VoiceMode::Poly, u8(std::stoi(val)));
            } else if (const auto dot = key.find('.'); dot != std::string::npos) {
                const u8 m = module(key.substr(0, dot));
                const auto pname = key.substr(dot + 1);
                const auto* def = patch.va.find(m)->def();
                if (const int p = indexByName(def->params, pname); p >= 0) {
                    for (u8 v = 0; v < kFileVariations; ++v)
                        edit::setParam(patch, area, m, u8(p), v, u8(std::stoi(val)));
                } else if (const int md = indexByName(def->modes, pname); md >= 0) {
                    edit::setMode(patch, area, m, u8(md), u8(std::stoi(val)));
                } else {
                    throw std::invalid_argument("unknown parameter " + item);
                }
            } else {
                const auto* def = defByName(val);
                mods[key] = edit::addModule(patch, area, def->typeId, col, 0);
                col = u8(col + 1);
            }
        }
        uprate::update(patch, area);
        const auto bytes = savePatch(patch);
        std::ofstream f(argv[1], std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
