#include "CableLayout.h"

namespace g2ui::cablelayout {
namespace {

const juce::String kFormat = "G2fresh cable layout";

juce::var array(std::initializer_list<int> values)
{
    juce::Array<juce::var> a;
    for (int v : values)
        a.add(v);
    return a;
}

int item(const juce::var& a, int i)
{
    return a.isArray() && i < a.size() ? static_cast<int>(a[i]) : -1;
}

} // namespace

juce::String toJson(const std::vector<std::pair<int, const g2::Patch*>>& patches)
{
    juce::Array<juce::var> list;
    for (const auto& [slot, patch] : patches) {
        juce::Array<juce::var> cables;
        for (const g2::Area* area : {&patch->va, &patch->fx})
            for (const auto& c : area->cables) {
                if (!c.bend)
                    continue;
                auto* o = new juce::DynamicObject();
                o->setProperty("area", area == &patch->va ? "va" : "fx");
                o->setProperty("from", array({c.fromModule, c.fromConn, c.fromIsOutput ? 1 : 0}));
                o->setProperty("to", array({c.toModule, c.toConn}));
                o->setProperty("bend", array({c.bend->dx, c.bend->dy}));
                cables.add(juce::var(o));
            }
        if (cables.isEmpty())
            continue;
        auto* p = new juce::DynamicObject();
        p->setProperty("slot", slot);
        p->setProperty("cables", cables);
        list.add(juce::var(p));
    }
    if (list.isEmpty())
        return {};
    auto* root = new juce::DynamicObject();
    root->setProperty("format", kFormat);
    root->setProperty("version", 1);
    root->setProperty("patches", list);
    return juce::JSON::toString(juce::var(root));
}

int apply(const juce::String& json, const std::function<g2::Patch*(int slot)>& patchFor)
{
    const auto root = juce::JSON::parse(json);
    if (root["format"].toString() != kFormat || !root["patches"].isArray())
        return 0;
    int shaped = 0;
    for (const auto& p : *root["patches"].getArray()) {
        auto* patch = patchFor(static_cast<int>(p["slot"]));
        if (!patch || !p["cables"].isArray())
            continue;
        for (const auto& c : *p["cables"].getArray()) {
            auto& area = c["area"].toString() == "fx" ? patch->fx : patch->va;
            const auto& from = c["from"];
            const auto& to = c["to"];
            const auto& bend = c["bend"];
            for (auto& cable : area.cables)
                if (cable.fromModule == item(from, 0) && cable.fromConn == item(from, 1)
                    && (cable.fromIsOutput ? 1 : 0) == item(from, 2) && cable.toModule == item(to, 0)
                    && cable.toConn == item(to, 1) && bend.isArray() && bend.size() == 2) {
                    cable.bend = g2::CableBend{static_cast<std::int16_t>(juce::jlimit(-30000, 30000, static_cast<int>(bend[0]))),
                                               static_cast<std::int16_t>(juce::jlimit(-30000, 30000, static_cast<int>(bend[1])))};
                    ++shaped;
                    break;
                }
        }
    }
    return shaped;
}

} // namespace g2ui::cablelayout
