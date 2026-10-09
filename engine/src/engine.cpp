#include "g2/engine/engine.hpp"

#include "g2/edit.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace g2::engine {
namespace {

std::map<std::uint8_t, Factory>& registry()
{
    static std::map<std::uint8_t, Factory> r;
    return r;
}

} // namespace

void registerProcessor(std::uint8_t type, Factory factory)
{
    registry()[type] = std::move(factory);
}

bool hasProcessor(std::uint8_t type)
{
    return registry().count(type) != 0;
}

std::vector<std::uint8_t> supportedTypes()
{
    std::vector<std::uint8_t> types;
    for (const auto& [type, f] : registry())
        types.push_back(type);
    return types;
}

// One module in the run order: its processor and where its inputs come from.
struct PatchEngine::Node {
    Location loc = Location::Va;
    std::uint8_t index = 0;
    std::unique_ptr<Processor> proc;
    bool audioRate = true;
    // Per input: the producing node's output slot (node, connector), or -1.
    std::vector<std::pair<int, int>> sources;
    std::vector<float> in, out;
};

PatchEngine::PatchEngine(const Patch& patch) : patch_(patch)
{
    static const bool registered = [] {
        registerBuiltinProcessors();
        return true;
    }();
    (void)registered;
    variation_ = patch_.header.activeVariation;
    build(Location::Va);
    build(Location::Fx);
    for (auto& n : nodes_)
        if (n.proc) {
            const auto* m = patch_.area(n.loc).find(n.index);
            n.proc->update(*m, variation_);
            n.audioRate = n.proc->audioRate(*m);
        }
}

PatchEngine::~PatchEngine() = default;

void PatchEngine::build(Location loc)
{
    const auto& area = patch_.area(loc);
    const auto first = static_cast<int>(nodes_.size());

    // Cable order: a module runs after the modules feeding it; a cycle is
    // broken where it closes (that input reads the previous sample).
    std::map<std::uint8_t, std::vector<std::uint8_t>> feeds; // module -> modules it reads from
    for (const auto& c : area.cables)
        if (c.fromIsOutput)
            feeds[c.toModule].push_back(c.fromModule);
    std::vector<std::uint8_t> order;
    std::map<std::uint8_t, int> state; // 0 new, 1 visiting, 2 done
    std::function<void(std::uint8_t)> visit = [&](std::uint8_t m) {
        if (state[m] != 0)
            return;
        state[m] = 1;
        for (auto src : feeds[m])
            visit(src);
        state[m] = 2;
        order.push_back(m);
    };
    std::vector<std::uint8_t> indices;
    for (const auto& m : area.modules)
        indices.push_back(m.index);
    std::sort(indices.begin(), indices.end());
    for (auto m : indices)
        visit(m);

    std::map<std::uint8_t, int> nodeOf;
    for (auto idx : order) {
        const auto* m = area.find(idx);
        const auto* def = m ? m->def() : nullptr;
        if (def && def->inputs.empty() && def->outputs.empty() && def->params.empty())
            continue; // a label (the Name bar): nothing to play
        Node n;
        n.loc = loc;
        n.index = idx;
        n.in.assign(def ? def->inputs.size() : 0, 0.0f);
        n.out.assign(def ? def->outputs.size() : 0, 0.0f);
        n.sources.assign(n.in.size(), {-1, -1});
        if (const auto it = registry().find(m->type); it != registry().end())
            n.proc = it->second();
        else
            unsupported_.push_back(m->name + (def ? " (" + std::string(def->shortName) + ")" : std::string()));
        nodeOf[idx] = static_cast<int>(nodes_.size());
        nodes_.push_back(std::move(n));
    }
    // Cables: an input reads the output it is connected to. Links between
    // inputs (fromIsOutput false) share the source of the first input.
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& c : area.cables) {
            if (!nodeOf.count(c.toModule) || !nodeOf.count(c.fromModule))
                continue;
            auto& to = nodes_[static_cast<std::size_t>(nodeOf[c.toModule])];
            if (c.toConn >= to.sources.size())
                continue;
            if (c.fromIsOutput && pass == 0) {
                to.sources[c.toConn] = {nodeOf[c.fromModule], c.fromConn};
            } else if (!c.fromIsOutput && pass == 1) {
                const auto& from = nodes_[static_cast<std::size_t>(nodeOf[c.fromModule])];
                if (c.fromConn < from.sources.size())
                    to.sources[c.toConn] = from.sources[c.fromConn];
            }
        }
    (void)first;
}

void PatchEngine::setVariation(std::uint8_t variation)
{
    variation_ = variation;
    for (auto& n : nodes_)
        if (n.proc)
            n.proc->update(*patch_.area(n.loc).find(n.index), variation_);
}

void PatchEngine::setParam(Location loc, std::uint8_t module, std::uint8_t param, std::uint8_t value)
{
    try {
        edit::setTargetValue(patch_, {loc, module, param}, variation_, value);
    } catch (const std::exception&) {
        return;
    }
    for (auto& n : nodes_)
        if (n.proc && n.loc == loc && n.index == module)
            n.proc->update(*patch_.area(loc).find(module), variation_);
}

void PatchEngine::setKey(int note, bool gate)
{
    // Placeholder encoding until the module catalog documents the G2's note
    // signal: 0 at note 64, 1/64 per semitone.
    io_.pitch = static_cast<float>(note - 64) / 64.0f;
    io_.gate = gate ? 1.0f : 0.0f;
}

void PatchEngine::step()
{
    const bool controlTick = sampleCount_ % kControlDivider == 0;
    io_.out = {};
    io_.fx = {};
    for (auto& n : nodes_) {
        if (!n.proc || (!n.audioRate && !controlTick))
            continue;
        for (std::size_t i = 0; i < n.in.size(); ++i) {
            const auto [node, conn] = n.sources[i];
            n.in[i] = node >= 0 ? nodes_[static_cast<std::size_t>(node)].out[static_cast<std::size_t>(conn)] : 0.0f;
        }
        n.proc->process(n.in.data(), n.out.data(), io_);
    }
    ++sampleCount_;
}

void PatchEngine::render(std::array<float*, 4> outs, int frames)
{
    for (int f = 0; f < frames; ++f) {
        step();
        for (int c = 0; c < 4; ++c)
            if (outs[static_cast<std::size_t>(c)])
                outs[static_cast<std::size_t>(c)][f] = io_.out[static_cast<std::size_t>(c)];
    }
}

} // namespace g2::engine
