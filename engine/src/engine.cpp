#include "g2/engine/engine.hpp"

#include "g2/edit.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
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
            std::vector<bool> connected(n.sources.size());
            for (std::size_t i = 0; i < n.sources.size(); ++i)
                connected[i] = n.sources[i].first >= 0;
            n.proc->connected(connected);
            n.proc->update(*m, variation_);
            n.audioRate = n.proc->audioRate(*m);
        }
    updateGain();
}

// The patch's Gain setting (Level, On/Off) scales the voice's outputs. Its
// curve, measured on the emulated G2 as a black box, is the mixers' "Exp"
// level curve: 0.01 x + 0.99 x^3 with x = v/127 (Level 100 = -6.2 dB).
void PatchEngine::updateGain()
{
    const auto level = edit::settingValue(patch_, edit::Setting::Gain, 0, variation_);
    const auto on = edit::settingValue(patch_, edit::Setting::Gain, 1, variation_);
    const float x = static_cast<float>(level) / 127.0f;
    gain_ = on ? 0.01f * x + 0.99f * x * x * x : 0.0f;
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
    // Cables: an input reads the output it is connected to. Inputs joined by
    // links (fromIsOutput false) form one net with one source, whichever of
    // them the output cable reaches: a link has no direction for the signal.
    std::map<std::pair<int, int>, std::pair<int, int>> parent; // (node, input) -> its net's representative
    std::function<std::pair<int, int>(std::pair<int, int>)> root = [&](std::pair<int, int> x) {
        auto it = parent.find(x);
        if (it == parent.end() || it->second == x)
            return x;
        return it->second = root(it->second);
    };
    for (const auto& c : area.cables)
        if (!c.fromIsOutput && nodeOf.count(c.fromModule) && nodeOf.count(c.toModule)) {
            const auto a = root({nodeOf[c.fromModule], c.fromConn});
            const auto b = root({nodeOf[c.toModule], c.toConn});
            if (a != b)
                parent[a] = b;
        }
    std::map<std::pair<int, int>, std::pair<int, int>> netSource; // representative -> (node, output)
    for (const auto& c : area.cables)
        if (c.fromIsOutput && nodeOf.count(c.fromModule) && nodeOf.count(c.toModule))
            netSource[root({nodeOf[c.toModule], c.toConn})] = {nodeOf[c.fromModule], c.fromConn};
    for (int i = first; i < static_cast<int>(nodes_.size()); ++i)
        for (std::size_t in = 0; in < nodes_[static_cast<std::size_t>(i)].sources.size(); ++in)
            if (const auto it = netSource.find(root({i, static_cast<int>(in)})); it != netSource.end())
                nodes_[static_cast<std::size_t>(i)].sources[in] = it->second;
}

void PatchEngine::setVariation(std::uint8_t variation)
{
    variation_ = variation;
    for (auto& n : nodes_)
        if (n.proc)
            n.proc->update(*patch_.area(n.loc).find(n.index), variation_);
    updateGain();
}

void PatchEngine::setParam(Location loc, std::uint8_t module, std::uint8_t param, std::uint8_t value)
{
    if (loc == Location::Settings) {
        try {
            edit::setTargetValue(patch_, {loc, module, param}, variation_, value);
        } catch (const std::exception&) {
        }
        updateGain();
        return;
    }
    try {
        edit::setTargetValue(patch_, {loc, module, param}, variation_, value);
    } catch (const std::exception&) {
        return;
    }
    for (auto& n : nodes_)
        if (n.proc && n.loc == loc && n.index == module)
            n.proc->update(*patch_.area(loc).find(module), variation_);
}

void PatchEngine::setKey(int note, bool gate, int velocity)
{
    // The G2's note signal: 0 at note 64 (E4), one unit (1/64) per semitone
    // (the Keyboard module's Pitch output, measured on the emulated G2).
    io_.pitch = static_cast<float>(note - 64) / kUnitsPerSignal;
    io_.gate = gate ? 1.0f : 0.0f;
    const float v = static_cast<float>(std::clamp(velocity, 0, 127)) / 127.0f;
    if (gate)
        io_.velocity = v;
    else
        io_.releaseVelocity = v;
}

void PatchEngine::step()
{
    const bool controlTick = sampleCount_ % kControlDivider == 0;
    io_.controlTick = controlTick;
    io_.out = {};
    io_.fx = {};
    io_.bus = {};
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
                outs[static_cast<std::size_t>(c)][f] = io_.out[static_cast<std::size_t>(c)] * gain_;
    }
}

} // namespace g2::engine
