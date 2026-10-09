#include "g2/uprate.hpp"

#include <cstddef>
#include <map>
#include <numeric>
#include <optional>
#include <tuple>
#include <vector>

namespace g2::uprate {
namespace {

// A connector of the area: module index, connector index, output flag.
using Node = std::tuple<u8, u8, bool>;

const db::ConnectorDef* connectorDef(const Area& area, Node n)
{
    const Module* m = area.find(std::get<0>(n));
    const auto* def = m ? m->def() : nullptr;
    if (!def)
        return nullptr;
    const auto& list = std::get<2>(n) ? def->outputs : def->inputs;
    return std::get<1>(n) < list.size() ? &list[std::get<1>(n)] : nullptr;
}

bool isDynamic(const db::ConnectorDef* c)
{
    return c && c->bandwidth == db::Bandwidth::Dynamic;
}

// The cable nets of an area: connected components of connectors, each with
// the output that drives it (CPatchData::GetCableChainID @000e407a).
struct Nets {
    std::map<Node, std::size_t> id; // connector -> union-find slot
    std::vector<std::size_t> parent;
    std::vector<std::optional<Node>> source; // per root: the net's output

    std::size_t slot(Node n)
    {
        auto [it, added] = id.try_emplace(n, parent.size());
        if (added) {
            parent.push_back(parent.size());
            source.emplace_back();
        }
        return it->second;
    }
    std::size_t root(std::size_t s)
    {
        while (parent[s] != s)
            s = parent[s] = parent[parent[s]];
        return s;
    }
    std::optional<std::size_t> rootOf(Node n)
    {
        const auto it = id.find(n);
        if (it == id.end())
            return std::nullopt;
        return root(it->second);
    }

    explicit Nets(const Area& area)
    {
        for (const auto& c : area.cables) {
            const Node from{c.fromModule, c.fromConn, c.fromIsOutput};
            const Node to{c.toModule, c.toConn, false};
            const std::size_t a = root(slot(from)), b = root(slot(to));
            if (a != b) {
                parent[b] = a;
                if (!source[a])
                    source[a] = source[b];
            }
            // A net has at most one output; the first one found wins.
            if (c.fromIsOutput && !source[a])
                source[a] = from;
        }
    }
};

// CPanel::GetBandWidth @000be72e: a dynamic connector runs at its module's
// rate, a static one is audio rate when it is red.
bool isAudioRate(const Area& area, Node out)
{
    const auto* c = connectorDef(area, out);
    if (!c)
        return false;
    if (c->bandwidth == db::Bandwidth::Dynamic) {
        const Module* m = area.find(std::get<0>(out));
        return m && m->uprate;
    }
    return c->color == db::ConnColor::Red;
}

// CPnl*OutHole::GetColor(EModuleBandWidth) mapped by
// CPatchData::MapConnectorColorToCableColor.
CableColor rateColor(const db::ConnectorDef& c, bool uprate)
{
    if (c.color == db::ConnColor::YellowOrange)
        return uprate ? CableColor::Orange : CableColor::Yellow;
    return uprate ? CableColor::Red : CableColor::Blue;
}

} // namespace

void update(Patch& patch, Location loc)
{
    if (loc == Location::Settings)
        return;
    Area& area = patch.area(loc);
    Nets nets(area);

    for (bool changed = true; changed;) {
        changed = false;
        for (Module& m : area.modules) {
            const auto* def = m.def();
            if (!def)
                continue;
            bool rate = false;
            for (std::size_t i = 0; i < def->inputs.size() && !rate; ++i) {
                if (def->inputs[i].bandwidth != db::Bandwidth::Dynamic)
                    continue;
                const auto net = nets.rootOf(Node{m.index, static_cast<u8>(i), false});
                if (net && nets.source[*net])
                    rate = isAudioRate(area, *nets.source[*net]);
            }
            if (rate == m.uprate)
                continue;
            m.uprate = rate;
            changed = true;
            // Recolour the whole net of every connected dynamic output.
            for (std::size_t o = 0; o < def->outputs.size(); ++o) {
                if (!isDynamic(&def->outputs[o]))
                    continue;
                const auto net = nets.rootOf(Node{m.index, static_cast<u8>(o), true});
                if (!net)
                    continue;
                const CableColor color = rateColor(def->outputs[o], rate);
                for (auto& c : area.cables)
                    if (nets.rootOf(Node{c.toModule, c.toConn, false}) == net)
                        c.color = color;
            }
        }
    }
}

} // namespace g2::uprate
