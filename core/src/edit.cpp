#include "g2/edit.hpp"

#include "g2/cables.hpp"
#include "g2/live.hpp"
#include "g2/patch_load.hpp"
#include "g2/special.hpp"
#include "g2/uprate.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <tuple>

namespace g2::edit {
namespace {

Area& areaFor(Patch& patch, Location loc)
{
    if (loc == Location::Settings)
        throw std::invalid_argument("modules live in the VA or FX area");
    return patch.area(loc);
}

Module& moduleAt(Patch& patch, Location loc, u8 index)
{
    Module* m = areaFor(patch, loc).find(index);
    if (!m)
        throw std::invalid_argument("no module with this index");
    return *m;
}

const db::ConnectorDef& connector(const Module& m, Endpoint e)
{
    const auto* def = m.def();
    if (!def)
        throw std::invalid_argument("unknown module type");
    const auto& list = e.isOutput ? def->outputs : def->inputs;
    if (e.conn >= list.size())
        throw std::invalid_argument("no such connector");
    return list[e.conn];
}

bool refersTo(u8 location, u8 module, Location loc, u8 index)
{
    return location == static_cast<u8>(loc) && module == index;
}

// ---- Cable trees ----------------------------------------------------------------
//
// The original's cable edits are lists of molecules computed from the
// current trees, then applied in order: CMCableDelete, CMCableConnect and
// CMCableRecolor (each one CPatchData::DeleteCable, ConnectCable or
// RecolorCable), followed by the uprate changes (GenerateBandwidthChange-
// Molecules, here uprate::update).

using cables::Connector;
using Segment = cables::Tree::Segment;

Connector connectorOf(Endpoint e)
{
    return {e.module, e.conn, e.isOutput};
}

Endpoint endpointOf(Connector c)
{
    return {c.module, c.conn, c.isOutput};
}

struct Molecule {
    enum Kind { Connect, Delete, Recolor } kind;
    Connector a, b;
    CableColor color = CableColor::White;
};
using Molecules = std::vector<Molecule>;

void recolorAll(Molecules& out, const std::vector<Segment>& segments, CableColor color)
{
    for (const auto& s : segments)
        out.push_back({Molecule::Recolor, s.node, s.parent, color});
}

// Applies the molecules in order and records each with its inverse (a
// deleted cable comes back with its colour). `dump`: they belong to a module
// deletion or replacement (the original's EPartOfDump).
void applyMolecules(cables::Tree& tree, const Molecules& molecules, Location loc, bool dump = false)
{
    // The inverses come from the trees as they were when the molecules were
    // computed, as the original builds both lists before applying either.
    const cables::Tree was = tree;
    auto colourOf = [&](Connector a, Connector b) {
        const auto parent = was.parentOf(a);
        return parent == b ? was.colorOf(a) : was.colorOf(b);
    };
    for (const auto& m : molecules)
        switch (m.kind) {
        case Molecule::Connect:
            live::record(live::CableConnect{loc, m.a, m.b, m.color, dump}, live::CableDelete{loc, m.a, m.b, dump});
            tree.connect(m.a, m.b, m.color);
            break;
        case Molecule::Delete: {
            const auto color = colourOf(m.a, m.b);
            live::record(live::CableDelete{loc, m.a, m.b, dump},
                         live::CableConnect{loc, m.a, m.b, color.value_or(CableColor::White), dump});
            tree.erase(m.a, m.b);
            break;
        }
        case Molecule::Recolor: {
            const auto color = colourOf(m.a, m.b);
            live::record(live::CableRecolor{loc, m.a, m.b, m.color},
                         live::CableRecolor{loc, m.a, m.b, color.value_or(m.color)});
            tree.recolor(m.a, m.b, m.color);
            break;
        }
        }
}

// Writes the trees back as the cable list (bends kept) and updates the rates.
void store(Patch& patch, Location loc, const cables::Tree& tree, bool updateRates = true)
{
    Area& area = patch.area(loc);
    area.cables = tree.toCables(area.cables);
    if (updateRates)
        uprate::update(patch, loc);
}

// CPatchData::GenerateDeleteSelectionMolecules @000e7fe4 (with splicing):
// the cables of the modules about to go. Per tree: a copy loses the
// selection's connectors (CTree::erase: children move up), the real cables to
// them are deleted, and every connector whose parent changed in the copy is
// connected to its new parent; a root left over (its own root gone) is
// connected to the next one. A tree whose output goes turns white.
void spliceOut(Patch& patch, Location loc, const std::vector<u8>& indices, bool updateRates = true)
{
    auto selected = [&](u8 module) { return std::find(indices.begin(), indices.end(), module) != indices.end(); };
    if (loc == Location::Settings)
        return;
    cables::Tree tree(patch.area(loc));
    Molecules molecules;
    for (const Connector root : tree.roots()) {
        cables::Tree copy = tree.treeOf(root);
        const Connector chain = tree.chainId(root);
        const bool white = chain.isOutput && selected(chain.module);
        if (white)
            recolorAll(molecules, tree.completeBranch(chain), CableColor::White);
        bool touched = false;
        for (const auto& s : tree.completeTree(root)) {
            const bool node = selected(s.node.module), parent = selected(s.parent.module);
            if (!node && !parent)
                continue;
            touched = true;
            molecules.push_back({Molecule::Delete, s.node, s.parent});
            if (node)
                copy.eraseConnector(s.node);
            if (parent)
                copy.eraseConnector(s.parent);
        }
        if (!touched)
            continue;
        for (const Connector n : copy.nodes()) {
            const auto parent = copy.parentOf(n);
            if (parent == tree.parentOf(n))
                continue;
            const CableColor color = white ? CableColor::White : *copy.colorOf(n);
            if (parent)
                molecules.push_back({Molecule::Connect, n, *parent, color});
            else if (const auto next = copy.nextRoot(n))
                molecules.push_back({Molecule::Connect, n, *next, color});
        }
    }
    if (molecules.empty())
        return;
    applyMolecules(tree, molecules, loc, true);
    store(patch, loc, tree, updateRates);
}

// The modules' places, to record the moves an edit makes.
std::vector<std::tuple<u8, u8, u8>> placesOf(const Area& area)
{
    std::vector<std::tuple<u8, u8, u8>> out;
    for (const auto& m : area.modules)
        out.emplace_back(m.index, m.col, m.row);
    return out;
}

// Records the 34s of the modules an edit moved, in the area's module order:
// first the others (the original's make-room moves, CPatchData::InsertSpace
// @000e54c2), then `moved` (GetMoveModulesMolecules @000e5398).
void recordMoves(const Area& area, Location loc, const std::vector<std::tuple<u8, u8, u8>>& before,
                 const std::vector<u8>& moved = {})
{
    if (!live::recording())
        return;
    for (const bool ofMoved : {false, true})
        for (const auto& m : area.modules) {
            if ((std::find(moved.begin(), moved.end(), m.index) != moved.end()) != ofMoved)
                continue;
            for (const auto& [index, col, row] : before)
                if (index == m.index && (col != m.col || row != m.row))
                    live::record(live::ModuleMove{loc, m.index, m.col, m.row}, live::ModuleMove{loc, m.index, col, row});
        }
}

// What re-creates a deleted module (the undo of CPatchSelection::
// GetDeleteModuleMolecule): 30, 33, 42, 4D.
std::vector<live::Op> recreate(const Patch& patch, Location loc, const Module& m)
{
    std::vector<live::Op> ops{live::ModuleNew{loc, m}, live::ModuleName{loc, m.index, m.name},
                              live::CustomData{loc, m.index, m.customData.value_or(std::vector<u8>{})}};
    if (!m.params.empty())
        ops.push_back(live::Section{live::paramSection(patch, loc, {m.index})});
    return ops;
}

void placeBelow(Patch& patch, Location loc, u8 keep);

} // namespace

// ---- Names ------------------------------------------------------------------------

bool isModularChar(char c)
{
    return c != '\0' && kModularChars.find(c) != std::string_view::npos;
}

std::string modularName(std::string_view text, std::size_t maxLength)
{
    std::string out;
    for (std::size_t i = 0; i < text.size() && out.size() < maxLength;) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out.push_back(isModularChar(static_cast<char>(c)) ? static_cast<char>(c) : ' ');
            ++i;
            continue;
        }
        // A UTF-8 sequence counts as one character; a stray byte as one.
        std::size_t n = c >= 0xF0 && c < 0xF8 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (n > 1) {
            bool valid = i + n <= text.size();
            for (std::size_t k = 1; valid && k < n; ++k)
                valid = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
            if (!valid)
                n = 1;
        }
        out.push_back(' ');
        i += n;
    }
    return out;
}

std::string modularBytes(std::string_view bytes)
{
    std::string out(bytes);
    for (char& c : out)
        if (!isModularChar(c))
            c = ' ';
    return out;
}

std::string fileNameForPatch(std::string_view name)
{
    static constexpr std::string_view kFileChars =
        " !#$%&'()+,-.0123456789;=@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_`abcdefghijklmnopqrstuvwxyz{}";
    std::string out = modularName(name, name.size());
    for (char& c : out)
        if (kFileChars.find(c) == std::string_view::npos)
            c = ' ';
    return out;
}

bool filterLoadedNames(Patch& patch)
{
    bool changed = false;
    for (auto* area : {&patch.va, &patch.fx})
        for (auto& m : area->modules) {
            auto filtered = modularBytes(m.name);
            if (filtered != m.name) {
                m.name = std::move(filtered);
                changed = true;
            }
        }
    return changed;
}

u8 addModule(Patch& patch, Location loc, u8 type, u8 col, u8 row)
{
    Area& area = areaFor(patch, loc);
    const auto* def = db::find(type);
    if (!def || def->kind != db::ModuleKind::Module)
        throw std::invalid_argument("not a module type");
    if (col > 127 || row > 127)
        throw std::invalid_argument("position out of range");
    if (!patchload::canAddModule(patch, loc, type))
        throw std::invalid_argument("the patch is too big: the synth has no room for another module here");

    u8 index = 1;
    while (area.find(index))
        if (++index == 0)
            throw std::invalid_argument("the area is full");

    int number = 1;
    auto taken = [&](int n) {
        const std::string name = def->shortName + std::to_string(n);
        return std::any_of(area.modules.begin(), area.modules.end(),
                           [&](const Module& m) { return m.name == name; });
    };
    while (taken(number))
        ++number;

    Module m;
    m.index = index;
    m.type = type;
    m.col = col;
    m.row = row;
    m.locked = def->defaultLocked;
    for (const auto& mode : def->modes)
        m.modes.push_back(mode.defaultValue);
    m.name = std::string(def->shortName) + std::to_string(number);
    if (m.name.size() > 16)
        m.name.resize(16);
    if (!def->params.empty()) {
        std::vector<u8> values;
        for (const auto& p : def->params)
            values.push_back(p.defaultValue);
        m.params.assign(variationSlots(patch), values);
    }
    // The note sequencer's view (zoom, octave offset), as the original stores it.
    if (type == special::kSeqNoteType)
        m.customData = special::noteSeqCustomData({});
    // Label controls write their captions (CPanel::GetCustomData).
    if (auto labels = defaultLabelData(type))
        m.customData = std::move(labels);
    area.modules.push_back(std::move(m));
    // CPatchData::GetNewModulesMolecules @000eba8e: 30, then the new data's
    // 52 (empty), 4D (the defaults in every variation), 5B (default custom
    // data), 5A; the undo deletes it.
    if (live::recording()) {
        live::record(live::ModuleNew{loc, area.modules.back()}, live::ModuleDelete{loc, index});
        for (auto& section : live::moduleSections(patch, loc, {index}))
            live::record(live::Section{std::move(section)}, std::vector<live::Op>{});
    }
    uprate::update(patch, loc);
    return index;
}

u8 dropModule(Patch& patch, Location loc, u8 type, u8 col, u8 row)
{
    // A module dragged in (CClipboard::Drop @0014d882): the original makes
    // room before it creates the module, so its 34s come first.
    auto* recorded = live::recording();
    const std::size_t start = recorded ? recorded->size() : 0;
    const u8 index = addModule(patch, loc, type, col, row);
    const std::size_t moves = recorded ? recorded->size() : 0;
    resolveOverlaps(patch, loc, index);
    if (recorded)
        recorded->moveTail(moves, start);
    return index;
}

void removeModule(Patch& patch, Location loc, u8 index)
{
    removeModules(patch, loc, {index});
}

void moveModule(Patch& patch, Location loc, u8 index, u8 col, u8 row)
{
    if (col > 127 || row > 127)
        throw std::invalid_argument("position out of range");
    const auto before = placesOf(areaFor(patch, loc));
    Module& m = moduleAt(patch, loc, index);
    m.col = col;
    m.row = row;
    placeBelow(patch, loc, index);
    recordMoves(patch.area(loc), loc, before, {index});
}

namespace {
int heightOf(const Module& m)
{
    const auto* def = m.def();
    return def && def->height > 0 ? def->height : 1;
}
} // namespace

void resolveOverlaps(Patch& patch, Location loc, u8 keep)
{
    const auto before = placesOf(areaFor(patch, loc));
    placeBelow(patch, loc, keep);
    recordMoves(patch.area(loc), loc, before);
}

namespace {
void placeBelow(Patch& patch, Location loc, u8 keep)
{
    Area& area = areaFor(patch, loc);
    // Per column: the kept module first, then the others top to bottom; each
    // one goes no higher than it was, below anything placed that it overlaps.
    std::vector<Module*> order;
    for (auto& m : area.modules)
        order.push_back(&m);
    std::stable_sort(order.begin(), order.end(), [&](const Module* a, const Module* b) {
        if (a->col != b->col)
            return a->col < b->col;
        if ((a->index == keep) != (b->index == keep))
            return a->index == keep;
        return a->row < b->row;
    });
    std::vector<const Module*> placed;
    for (Module* m : order) {
        bool moved = true;
        while (moved) {
            moved = false;
            for (const Module* p : placed)
                if (p->col == m->col && m->row < p->row + heightOf(*p) && p->row < m->row + heightOf(*m)) {
                    // Rows stop at 127: a module that cannot go lower stays
                    // where it is, overlapping, rather than looping forever.
                    const auto below = static_cast<u8>(std::min(127, p->row + heightOf(*p)));
                    moved = below != m->row;
                    m->row = below;
                    if (!moved)
                        break;
                }
        }
        placed.push_back(m);
    }
}
} // namespace

u8 freeRow(const Patch& patch, Location loc, u8 col)
{
    int row = 0;
    for (const auto& m : patch.area(loc).modules)
        if (m.col == col)
            row = std::max(row, m.row + heightOf(m));
    return static_cast<u8>(std::min(row, 127));
}

void renameModule(Patch& patch, Location loc, u8 index, const std::string& name)
{
    const std::string filtered = modularName(name, kNameLength + 1);
    if (filtered.size() > kNameLength)
        throw std::invalid_argument("module names have at most 16 characters");
    Module& m = moduleAt(patch, loc, index);
    // CPanel::ChangePanelNameDialog @000bf08a: 33, only for a new name.
    if (m.name != filtered)
        live::record(live::ModuleName{loc, index, filtered}, live::ModuleName{loc, index, m.name});
    m.name = filtered;
}

CableColor cableColor(const Patch& patch, Location loc, Endpoint from)
{
    const Module* m = patch.area(loc).find(from.module);
    if (!m)
        throw std::invalid_argument("no module with this index");
    switch (connector(*m, from).color) {
    case db::ConnColor::Red: return CableColor::Red;
    case db::ConnColor::Blue: return CableColor::Blue;
    case db::ConnColor::Yellow: return CableColor::Yellow;
    case db::ConnColor::BlueRed: return m->uprate ? CableColor::Red : CableColor::Blue;
    case db::ConnColor::YellowOrange: return m->uprate ? CableColor::Orange : CableColor::Yellow;
    }
    return CableColor::Red;
}

namespace {

void checkJack(Patch& patch, Location loc, Endpoint e)
{
    connector(moduleAt(patch, loc, e.module), e);
}

bool sameEnds(const Cable& c, Connector a, Connector b)
{
    const Connector from{c.fromModule, c.fromConn, c.fromIsOutput}, to{c.toModule, c.toConn, false};
    return (from == a && to == b) || (from == b && to == a);
}

} // namespace

DropAction dropAction(const Patch& patch, Location loc, Endpoint source, std::optional<Endpoint> target, bool move)
{
    if (loc == Location::Settings)
        return DropAction::None;
    const cables::Tree tree(patch.area(loc));
    const Connector s = connectorOf(source);
    if (!move) {
        if (target) {
            const Connector t = connectorOf(*target);
            if (tree.chainId(s) != tree.chainId(t) && (!tree.hasSignal(s) || !tree.hasSignal(t)))
                return DropAction::Connect;
        }
        return DropAction::None;
    }
    if (!target)
        return tree.contains(s) ? DropAction::Disconnect : DropAction::None;
    const Connector t = connectorOf(*target);
    if (tree.chainId(s) != tree.chainId(t) && tree.contains(s)
        && (s.isOutput || !tree.hasSignal(s) || !tree.hasSignal(t)))
        return DropAction::Move;
    return DropAction::None;
}

Cable connect(Patch& patch, Location loc, Endpoint from, Endpoint to)
{
    checkJack(patch, loc, from);
    checkJack(patch, loc, to);
    cables::Tree tree(patch.area(loc));
    const Connector a = connectorOf(from), b = connectorOf(to);
    if (tree.chainId(a) == tree.chainId(b))
        throw std::invalid_argument(a == b ? "cannot connect a connector to itself"
                                           : "these connectors are already connected (one cable chain)");
    if (tree.hasSignal(a) && tree.hasSignal(b))
        throw std::invalid_argument("both connectors are already fed by an output");
    if (!patchload::canAddCable(patch))
        throw std::invalid_argument("the patch is too big: the synth has no room for another cable");

    // CPatchData::GetConnectRecolorMolecules @000e4c32: the colour of the fed
    // side (an output's own colour, or an input's cable colour), which the
    // other side's whole tree takes too.
    Molecules molecules;
    auto fedColour = [&](Connector fed, Connector other) {
        const CableColor color = fed.isOutput ? cableColor(patch, loc, endpointOf(fed)) : *tree.colorOf(fed);
        recolorAll(molecules, tree.completeTree(other), color);
        return color;
    };
    CableColor color = CableColor::White;
    if (tree.hasSignal(a))
        color = fedColour(a, b);
    else if (tree.hasSignal(b))
        color = fedColour(b, a);
    molecules.push_back({Molecule::Connect, a, b, color});
    applyMolecules(tree, molecules, loc);
    store(patch, loc, tree);
    for (const auto& c : patch.area(loc).cables)
        if (sameEnds(c, a, b))
            return c;
    throw std::logic_error("the new cable is missing");
}

bool disconnectJack(Patch& patch, Location loc, Endpoint jack)
{
    cables::Tree tree(areaFor(patch, loc));
    const Connector x = connectorOf(jack);
    if (!tree.contains(x))
        return false;
    Molecules molecules;
    // An output's chain stays connected without it: white.
    if (x.isOutput)
        recolorAll(molecules, tree.completeBranch(x), CableColor::White);
    // The connector its neighbours move to: its parent, or for a root its
    // first child (or the child holding the net's output, in a tree not
    // rooted at its output).
    const auto parent = tree.parentOf(x);
    const auto children = tree.childrenOf(x);
    Connector hub = parent ? *parent : children.front();
    if (const Connector chain = tree.chainId(x); chain.isOutput)
        for (const Connector c : children)
            if (tree.inSubtree(c, chain)) {
                hub = c;
                break;
            }
    const CableColor own = *tree.colorOf(x);
    if (parent) {
        molecules.push_back({Molecule::Delete, x, *parent});
        if (hub != *parent)
            molecules.push_back({Molecule::Connect, hub, *parent, x.isOutput ? CableColor::White : own});
    }
    for (const Connector c : children) {
        molecules.push_back({Molecule::Delete, x, c});
        if (hub != c)
            molecules.push_back({Molecule::Connect, hub, c, x.isOutput ? CableColor::White : *tree.colorOf(c)});
    }
    applyMolecules(tree, molecules, loc);
    store(patch, loc, tree);
    return true;
}

bool breakJack(Patch& patch, Location loc, Endpoint jack)
{
    cables::Tree tree(areaFor(patch, loc));
    const Connector x = connectorOf(jack);
    if (!tree.contains(x))
        return false;
    const Connector output = tree.chainId(x);
    if (!output.isOutput)
        return false; // nothing to break from
    tree.makeBase(output);
    Molecules molecules;
    if (x != output) {
        // What hangs below x stays linked, white; x loses its way to the output.
        recolorAll(molecules, tree.partialTree(x), CableColor::White);
        molecules.push_back({Molecule::Delete, x, *tree.parentOf(x)});
    } else {
        recolorAll(molecules, tree.completeBranch(x), CableColor::White);
        for (const Connector c : tree.childrenOf(x))
            molecules.push_back({Molecule::Delete, c, x});
    }
    applyMolecules(tree, molecules, loc);
    store(patch, loc, tree);
    return true;
}

bool deleteChain(Patch& patch, Location loc, Endpoint jack)
{
    cables::Tree tree(areaFor(patch, loc));
    const auto branch = tree.completeBranch(connectorOf(jack));
    if (branch.empty())
        return false;
    Molecules molecules;
    for (const auto& s : branch)
        molecules.push_back({Molecule::Delete, s.node, s.parent});
    applyMolecules(tree, molecules, loc);
    store(patch, loc, tree, false); // no uprate pass here in the original
    return true;
}

void recolorChain(Patch& patch, Location loc, Endpoint jack, CableColor color)
{
    cables::Tree tree(areaFor(patch, loc));
    const Connector x = connectorOf(jack);
    if (color == CableColor::White)
        throw std::invalid_argument("white is the colour of cables without an output");
    if (!tree.contains(x) || !tree.hasSignal(x))
        throw std::invalid_argument("only a chain fed by an output can change colour");
    Molecules molecules;
    recolorAll(molecules, tree.completeBranch(x), color);
    applyMolecules(tree, molecules, loc);
    store(patch, loc, tree, false); // no uprate pass here in the original
}

bool deleteUnusedCables(Patch& patch)
{
    bool deleted = false;
    for (const Location loc : {Location::Fx, Location::Va}) {
        cables::Tree tree(patch.area(loc));
        Molecules molecules;
        std::vector<Connector> done;
        // Every net without an output, found from its inputs, module by module.
        for (const auto& m : patch.area(loc).modules) {
            const auto* def = m.def();
            for (std::size_t i = 0; def && i < def->inputs.size(); ++i) {
                const Connector in{m.index, static_cast<u8>(i), false};
                if (!tree.contains(in) || tree.hasSignal(in))
                    continue;
                const Connector chain = tree.chainId(in);
                if (std::find(done.begin(), done.end(), chain) != done.end())
                    continue;
                done.push_back(chain);
                for (const auto& s : tree.completeBranch(chain))
                    molecules.push_back({Molecule::Delete, s.node, s.parent});
            }
        }
        if (molecules.empty())
            continue;
        applyMolecules(tree, molecules, loc);
        store(patch, loc, tree, false); // no uprate pass here in the original
        deleted = true;
    }
    return deleted;
}

void moveCables(Patch& patch, Location loc, Endpoint from, Endpoint to)
{
    checkJack(patch, loc, from);
    checkJack(patch, loc, to);
    if (dropAction(patch, loc, from, to, true) != DropAction::Move)
        throw std::invalid_argument("these cables cannot move there");
    cables::Tree tree(patch.area(loc));
    const Connector x = connectorOf(from), y = connectorOf(to);
    const Connector chainX = tree.chainId(x), chainY = tree.chainId(y);
    Molecules molecules;
    // The moved cables' colour: like a new connection, from the side that
    // has an output; with none, each cable keeps its own.
    CableColor color = *tree.colorOf(x);
    bool uniform = true;
    if (y.isOutput) {
        color = cableColor(patch, loc, to);
        recolorAll(molecules, tree.completeBranch(chainX), color);
    } else if (chainY.isOutput) {
        color = *tree.colorOf(y);
        recolorAll(molecules, tree.completeBranch(chainX), color);
    } else if (x.isOutput) {
        color = CableColor::White;
        recolorAll(molecules, tree.completeBranch(chainX), color);
    } else if (chainX.isOutput && tree.contains(y)) {
        recolorAll(molecules, tree.completeBranch(chainY), color);
    } else {
        uniform = false;
    }
    if (const auto parent = tree.parentOf(x)) {
        molecules.push_back({Molecule::Delete, x, *parent});
        molecules.push_back({Molecule::Connect, y, *parent, uniform ? color : *tree.colorOf(x)});
    }
    for (const Connector c : tree.childrenOf(x)) {
        molecules.push_back({Molecule::Delete, x, c});
        molecules.push_back({Molecule::Connect, y, c, uniform ? color : *tree.colorOf(c)});
    }
    applyMolecules(tree, molecules, loc);
    store(patch, loc, tree);
}

bool isCabled(const Patch& patch, Location loc, Endpoint jack)
{
    return loc != Location::Settings && cables::Tree(patch.area(loc)).contains(connectorOf(jack));
}

bool hasOutput(const Patch& patch, Location loc, Endpoint jack)
{
    if (loc == Location::Settings)
        return false;
    const cables::Tree tree(patch.area(loc));
    return tree.contains(connectorOf(jack)) && tree.hasSignal(connectorOf(jack));
}

bool canBreak(const Patch& patch, Location loc, Endpoint jack)
{
    if (loc == Location::Settings)
        return false;
    const cables::Tree tree(patch.area(loc));
    return tree.contains(connectorOf(jack)) && !tree.isLeaf(connectorOf(jack));
}

std::vector<Cable> chainCables(const Patch& patch, Location loc, Endpoint jack)
{
    std::vector<Cable> out;
    if (loc == Location::Settings)
        return out;
    const Area& area = patch.area(loc);
    for (const auto& s : cables::Tree(area).completeBranch(connectorOf(jack)))
        for (const auto& c : area.cables)
            if (sameEnds(c, s.node, s.parent)) {
                out.push_back(c);
                break;
            }
    return out;
}

Endpoint lowerEnd(const Patch& patch, Location loc, const Cable& cable)
{
    const Connector from{cable.fromModule, cable.fromConn, cable.fromIsOutput}, to{cable.toModule, cable.toConn, false};
    if (loc != Location::Settings && cables::Tree(patch.area(loc)).parentOf(from) == to)
        return endpointOf(from);
    return endpointOf(to);
}

void disconnect(Patch& patch, Location loc, const Cable& cable)
{
    Area& area = areaFor(patch, loc);
    const auto it = std::find(area.cables.begin(), area.cables.end(), cable);
    if (it == area.cables.end())
        throw std::invalid_argument("no such cable");
    area.cables.erase(it);
    uprate::update(patch, loc);
}

void setParam(Patch& patch, Location loc, u8 module, u8 param, u8 variation, u8 value)
{
    Module& m = moduleAt(patch, loc, module);
    const auto* def = m.def();
    if (!def || param >= def->params.size() || variation >= m.params.size()
        || param >= m.params[variation].size())
        throw std::invalid_argument("no such parameter");
    const auto& p = def->params[param];
    m.params[variation][param] = std::clamp(value, p.min, p.max);
}

bool isMomentary(const Patch& patch, Location loc, u8 module, u8 param)
{
    if (loc == Location::Settings)
        return false;
    const Module* m = patch.area(loc).find(module);
    const auto* def = m ? m->def() : nullptr;
    return def && param < def->params.size() && def->params[param].momentary;
}

void setMode(Patch& patch, Location loc, u8 module, u8 mode, u8 value)
{
    Module& m = moduleAt(patch, loc, module);
    const auto* def = m.def();
    if (!def || mode >= def->modes.size() || mode >= m.modes.size())
        throw std::invalid_argument("no such mode");
    const auto& md = def->modes[mode];
    const u8 v = std::clamp(value, md.min, md.max);
    // CPanel::SelectorRequestChange @000c060a: 2B, undone by the old value.
    if (m.modes[mode] != v)
        live::record(live::Mode{loc, module, mode, v}, live::Mode{loc, module, mode, m.modes[mode]});
    m.modes[mode] = v;
}

std::string paramText(const Patch& patch, Location loc, u8 module, u8 param, u8 variation)
{
    const Module* m = patch.area(loc).find(module);
    if (!m || !m->def() || variation >= m->params.size())
        return {};
    return db::formatParam(*m->def(), param, m->params[variation], m->modes);
}

// ---- Knob and controller targets ---------------------------------------------------

std::optional<u8> targetValue(const Patch& patch, Target t, u8 variation)
{
    if (t.location == Location::Settings) {
        const auto* def = settingDef(static_cast<Setting>(t.module));
        if (!def || t.param >= def->params.size())
            return std::nullopt;
        return settingValue(patch, static_cast<Setting>(t.module), t.param, variation);
    }
    const Module* m = patch.area(t.location).find(t.module);
    if (!m || variation >= m->params.size() || t.param >= m->params[variation].size())
        return std::nullopt;
    return m->params[variation][t.param];
}

int targetMax(const Patch& patch, Target t)
{
    const db::ModuleDef* def = nullptr;
    if (t.location == Location::Settings) {
        def = settingDef(static_cast<Setting>(t.module));
    } else if (const Module* m = patch.area(t.location).find(t.module)) {
        def = m->def();
    }
    return def && t.param < def->params.size() ? def->params[t.param].max : 0;
}

std::string targetText(const Patch& patch, Target t, u8 variation)
{
    if (t.location == Location::Settings)
        return settingDef(static_cast<Setting>(t.module)) ? settingText(patch, static_cast<Setting>(t.module), t.param, variation)
                                                          : std::string();
    return paramText(patch, t.location, t.module, t.param, variation);
}

std::string targetName(const Patch& patch, Target t)
{
    if (t.location == Location::Settings) {
        const auto* def = settingDef(static_cast<Setting>(t.module));
        if (!def || t.param >= def->params.size())
            return {};
        if (static_cast<Setting>(t.module) == Setting::Morph && t.param < kMorphGroups)
            return "Morph " + morphLabel(patch, t.param);
        return std::string(def->longName) + " " + def->params[t.param].name;
    }
    const Module* m = patch.area(t.location).find(t.module);
    if (!m || !m->def() || t.param >= m->def()->params.size())
        return {};
    return m->name + " " + m->def()->params[t.param].name;
}

void setTargetValue(Patch& patch, Target t, u8 variation, u8 value)
{
    if (t.location == Location::Settings)
        setSetting(patch, static_cast<Setting>(t.module), t.param, variation, value);
    else
        setParam(patch, t.location, t.module, t.param, variation, value);
}

std::optional<Target> knobTarget(const Patch& patch, int knob)
{
    if (knob < 0 || static_cast<std::size_t>(knob) >= patch.knobs.size() || !patch.knobs[static_cast<std::size_t>(knob)])
        return std::nullopt;
    const auto& k = *patch.knobs[static_cast<std::size_t>(knob)];
    return Target{static_cast<Location>(k.location), k.module, k.param};
}

// ---- Patch settings -----------------------------------------------------------

namespace {

constexpr std::array<std::pair<u8, u8>, 7> kSettingTypes{{
    {1, 6}, {2, 95}, {3, 135}, {4, 137}, {5, 138}, {6, 136}, {7, 153},
}};

SettingsModule& settingsModule(Patch& patch, Setting s)
{
    const auto index = static_cast<u8>(s);
    for (auto& m : patch.settings)
        if (m.index == index)
            return m;
    throw std::invalid_argument("the patch has no such settings module");
}

// Morph group labels live in the settings custom data, module 1, as
// [kind 1, len 8, param 8+group, 7 NUL-padded characters] records.
std::vector<u8>* morphLabels(Patch& patch)
{
    for (auto& m : patch.settingsCustomData)
        if (m.index == 1)
            return &m.bytes;
    return nullptr;
}

} // namespace

const db::ModuleDef* settingDef(Setting s)
{
    for (const auto& [index, type] : kSettingTypes)
        if (index == static_cast<u8>(s))
            return db::find(type);
    return nullptr;
}

u8 settingValue(const Patch& patch, Setting s, u8 param, u8 variation)
{
    for (const auto& m : patch.settings)
        if (m.index == static_cast<u8>(s) && variation < m.params.size() && param < m.params[variation].size())
            return m.params[variation][param];
    return 0;
}

void setSetting(Patch& patch, Setting s, u8 param, u8 variation, u8 value)
{
    auto& m = settingsModule(patch, s);
    const auto* def = settingDef(s);
    if (!def || param >= def->params.size() || variation >= m.params.size() || param >= m.params[variation].size())
        throw std::invalid_argument("no such setting");
    const auto& p = def->params[param];
    m.params[variation][param] = std::clamp(value, p.min, p.max);
}

std::string settingText(const Patch& patch, Setting s, u8 param, u8 variation)
{
    const auto* def = settingDef(s);
    if (!def)
        return {};
    for (const auto& m : patch.settings)
        if (m.index == static_cast<u8>(s) && variation < m.params.size())
            return db::formatParam(*def, param, m.params[variation], {});
    return {};
}

void setVoices(Patch& patch, VoiceMode mode, u8 polyVoices)
{
    // As the original editor's voice menu: choosing a count makes the patch
    // poly; Mono and Legato keep the stored count.
    patch.header.monoMode = static_cast<u8>(mode);
    if (mode == VoiceMode::Poly) {
        if (polyVoices < 1 || polyVoices > 32)
            throw std::invalid_argument("a patch has 1 to 32 voices");
        patch.header.voiceCount = polyVoices;
    }
}

std::string voicesText(const Patch& patch)
{
    switch (patch.header.monoMode) {
    case 1: return "Mono";
    case 2: return "Legato";
    default: return std::to_string(patch.header.voiceCount);
    }
}

void setCategory(Patch& patch, u8 category)
{
    if (category > 15)
        throw std::invalid_argument("no such category");
    patch.header.category = category;
}

const char* categoryName(u8 category)
{
    static const char* const kNames[16] = {"No Cat", "Acoustic", "Sequencer", "Bass", "Classic", "Drum",
                                           "Fantasy", "FX", "Lead", "Organ", "Pad", "Piano", "Synth",
                                           "Audio In", "User 1", "User 2"};
    return category < 16 ? kNames[category] : "";
}

std::string morphLabel(const Patch& patch, int group)
{
    for (const auto& m : patch.settingsCustomData) {
        if (m.index != 1)
            continue;
        for (std::size_t i = 0; i + 2 <= m.bytes.size(); i += 2u + m.bytes[i + 1]) {
            const u8 kind = m.bytes[i], len = m.bytes[i + 1];
            if (i + 2 + len > m.bytes.size())
                break;
            if (kind == 1 && len >= 1 && m.bytes[i + 2] == 8 + group) {
                std::string s(m.bytes.begin() + static_cast<std::ptrdiff_t>(i + 3),
                              m.bytes.begin() + static_cast<std::ptrdiff_t>(i + 2 + len));
                return s.substr(0, s.find('\0'));
            }
        }
    }
    const auto* def = settingDef(Setting::Morph);
    return def && group >= 0 && group < 8 ? def->params[static_cast<std::size_t>(group)].name : std::string();
}

void setMorphLabel(Patch& patch, int group, const std::string& text)
{
    const std::string label = modularName(text, kLabelLength + 1);
    if (group < 0 || group > 7 || label.size() > kLabelLength)
        throw std::invalid_argument("morph labels have at most 7 characters");
    auto* bytes = morphLabels(patch);
    if (!bytes) {
        patch.settingsCustomData.push_back({1, {}});
        bytes = &patch.settingsCustomData.back().bytes;
    }
    std::string padded = label;
    padded.resize(7, '\0');
    std::vector<u8> record{1, 8, static_cast<u8>(8 + group)};
    record.insert(record.end(), padded.begin(), padded.end());
    for (std::size_t i = 0; i + 2 <= bytes->size(); i += 2u + (*bytes)[i + 1]) {
        if ((*bytes)[i] == 1 && (*bytes)[i + 1] >= 1 && i + 2 < bytes->size() && (*bytes)[i + 2] == 8 + group) {
            const auto first = bytes->begin() + static_cast<std::ptrdiff_t>(i);
            const auto last = first + 2 + (*bytes)[i + 1];
            bytes->erase(first, last);
            bytes->insert(bytes->begin() + static_cast<std::ptrdiff_t>(i), record.begin(), record.end());
            return;
        }
    }
    bytes->insert(bytes->end(), record.begin(), record.end());
}

// ---- Morphs, knob and MIDI assignments, labels ---------------------------------

namespace {

bool sameTarget(u8 location, u8 module, u8 param, Location loc, u8 m, u8 p)
{
    return location == static_cast<u8>(loc) && module == m && param == p;
}

void checkParam(const Patch& patch, Location loc, u8 module, u8 param)
{
    if (loc == Location::Settings) {
        for (const auto& m : patch.settings)
            if (m.index == module && !m.params.empty() && param < m.params.front().size())
                return;
        throw std::invalid_argument("no such setting");
    }
    const Module* m = patch.area(loc).find(module);
    if (!m || !m->def() || param >= m->def()->params.size())
        throw std::invalid_argument("no such parameter");
}

// The definition of a module parameter or patch setting, or nullptr.
const db::ParamDef* paramDefOf(const Patch& patch, Location loc, u8 module, u8 param)
{
    if (loc == Location::Settings) {
        const auto* def = settingDef(static_cast<Setting>(module));
        return def && param < def->params.size() ? &def->params[param] : nullptr;
    }
    if (loc != Location::Va && loc != Location::Fx)
        return nullptr;
    const Module* m = patch.area(loc).find(module);
    const auto* def = m ? m->def() : nullptr;
    return def && param < def->params.size() ? &def->params[param] : nullptr;
}

u8 valueOf(const Patch& patch, Location loc, u8 module, u8 param, u8 variation)
{
    if (loc == Location::Settings)
        return settingValue(patch, static_cast<Setting>(module), param, variation);
    const Module* m = patch.area(loc).find(module);
    return m && variation < m->params.size() && param < m->params[variation].size() ? m->params[variation][param] : 0;
}

// The order of the original's map, which is also the order it writes the
// assignments in: location, module, parameter, group (CMorphSpec::operator<).
auto morphKey(const MorphAssign& a)
{
    return std::tuple(a.location, a.module, a.param, a.morph);
}

std::vector<MorphAssign>& morphList(Patch& patch, u8 variation)
{
    if (variation >= patch.morphs.size())
        throw std::invalid_argument("no such variation");
    return patch.morphs[variation].assigns;
}

// Sets the range of an assignment, or adds it where the original's map has
// it (lists loaded from files are in that order already); range 0 removes it.
void putMorph(std::vector<MorphAssign>& list, MorphAssign a)
{
    const auto key = morphKey(a);
    const auto same = std::find_if(list.begin(), list.end(), [&](const MorphAssign& x) { return morphKey(x) == key; });
    if (a.range == 0) {
        if (same != list.end())
            list.erase(same);
        return;
    }
    if (same != list.end()) {
        same->range = a.range;
        return;
    }
    const auto after = std::find_if(list.begin(), list.end(), [&](const MorphAssign& x) { return key < morphKey(x); });
    list.insert(after, a);
}

bool inAnyGroup(const std::vector<MorphAssign>& list, Location loc, u8 module, u8 param)
{
    return std::any_of(list.begin(), list.end(),
                       [&](const MorphAssign& a) { return sameTarget(a.location, a.module, a.param, loc, module, param); });
}

} // namespace

std::vector<Morph> morphsOf(const Patch& patch, u8 variation, Location loc, u8 module, u8 param)
{
    std::vector<Morph> out;
    if (variation < patch.morphs.size())
        for (const auto& a : patch.morphs[variation].assigns)
            if (sameTarget(a.location, a.module, a.param, loc, module, param)
                && std::none_of(out.begin(), out.end(), [&](const Morph& m) { return m.group == a.morph; }))
                out.push_back({a.morph, a.range});
    std::sort(out.begin(), out.end(), [](const Morph& a, const Morph& b) { return a.group < b.group; });
    return out;
}

std::optional<std::int8_t> morphRange(const Patch& patch, u8 variation, Location loc, u8 module, u8 param, u8 group)
{
    if (variation < patch.morphs.size())
        for (const auto& a : patch.morphs[variation].assigns)
            if (sameTarget(a.location, a.module, a.param, loc, module, param) && a.morph == group)
                return a.range;
    return std::nullopt;
}

bool isMorphable(const Patch& patch, Location loc, u8 module, u8 param)
{
    const auto* def = paramDefOf(patch, loc, module, param);
    return def && def->morphable;
}

int morphedParamCount(const Patch& patch, u8 variation)
{
    if (variation >= patch.morphs.size())
        return 0;
    std::vector<std::tuple<u8, u8, u8>> params;
    for (const auto& a : patch.morphs[variation].assigns)
        params.emplace_back(a.location, a.module, a.param);
    std::sort(params.begin(), params.end());
    return static_cast<int>(std::unique(params.begin(), params.end()) - params.begin());
}

bool canMorph(const Patch& patch, u8 variation, Location loc, u8 module, u8 param)
{
    if (variation >= patch.morphs.size() || !isMorphable(patch, loc, module, param))
        return false;
    return inAnyGroup(patch.morphs[variation].assigns, loc, module, param)
        || morphedParamCount(patch, variation) < kMaxMorphedParams;
}

std::int8_t clampMorphRange(const Patch& patch, u8 variation, Location loc, u8 module, u8 param, int range)
{
    const auto* def = paramDefOf(patch, loc, module, param);
    const int max = def ? std::min<int>(def->max, 127) : 127;
    const int value = std::min<int>(valueOf(patch, loc, module, param, variation), max);
    return static_cast<std::int8_t>(std::clamp(range, -value, max - value));
}

void setMorph(Patch& patch, u8 variation, Location loc, u8 module, u8 param, u8 group, int range)
{
    checkParam(patch, loc, module, param);
    if (variation >= patch.morphs.size() || group >= kMorphGroups)
        throw std::invalid_argument("no such variation or morph group");
    if (!isMorphable(patch, loc, module, param))
        throw std::invalid_argument("this parameter cannot be morphed");
    if (!canMorph(patch, variation, loc, module, param))
        throw std::invalid_argument("a variation morphs at most 25 parameters");
    const auto r = clampMorphRange(patch, variation, loc, module, param, range);
    putMorph(patch.morphs[variation].assigns, {static_cast<u8>(loc), module, param, group, r});
}

void clearMorph(Patch& patch, u8 variation, Location loc, u8 module, u8 param, u8 group)
{
    if (variation >= patch.morphs.size())
        return;
    std::erase_if(patch.morphs[variation].assigns, [&](const MorphAssign& a) {
        return sameTarget(a.location, a.module, a.param, loc, module, param) && a.morph == group;
    });
}

bool toggleMorphAssign(Patch& patch, u8 variation, Location loc, u8 module, u8 param, u8 group)
{
    // CControlMenu::HandleMenuSelection @0008f5c6, a realtime pair: leaving
    // the group is a 43 with range 0 (CMorphMap::GetDeassignMorphMolecules
    // @000f1d58); joining it, a 40 to value 0 (if it was not) then a 43 with
    // the full range (GetAssignMorphToParamMolecules @000f1e62).
    live::markRealtime();
    if (const auto range = morphRange(patch, variation, loc, module, param, group)) {
        live::record(live::Morph{loc, module, param, group, 0, variation},
                     live::Morph{loc, module, param, group, *range, variation});
        clearMorph(patch, variation, loc, module, param, group);
        return false;
    }
    if (!canMorph(patch, variation, loc, module, param))
        throw std::invalid_argument(isMorphable(patch, loc, module, param) ? "a variation morphs at most 25 parameters"
                                                                          : "this parameter cannot be morphed");
    // The value goes to 0 and the range to the full +max (the control's
    // maximum, CPnlControl+0x45).
    if (const u8 old = valueOf(patch, loc, module, param, variation); old != 0)
        live::record(live::Param{loc, module, param, 0, variation}, live::Param{loc, module, param, old, variation});
    if (loc == Location::Settings)
        setSetting(patch, static_cast<Setting>(module), param, variation, 0);
    else
        setParam(patch, loc, module, param, variation, 0);
    setMorph(patch, variation, loc, module, param, group, paramDefOf(patch, loc, module, param)->max);
    live::record(live::Morph{loc, module, param, group, *morphRange(patch, variation, loc, module, param, group), variation},
                 live::Morph{loc, module, param, group, 0, variation});
    return true;
}

void clearMorphGroup(Patch& patch, u8 variation, u8 group)
{
    std::erase_if(morphList(patch, variation), [&](const MorphAssign& a) { return a.morph == group; });
}

void copyMorphGroup(Patch& patch, u8 variation, u8 from, u8 to)
{
    if (from >= kMorphGroups || to >= kMorphGroups)
        throw std::invalid_argument("no such morph group");
    auto& list = morphList(patch, variation);
    std::vector<MorphAssign> copies;
    for (const auto& a : list)
        if (a.morph == from)
            copies.push_back({a.location, a.module, a.param, to, a.range});
    // CMorphMap::GetAssignMorphToParamMolecules: the old assignment in `to`
    // goes, the copy comes (the parameter is morphed, so the limit allows it).
    for (const auto& a : copies)
        putMorph(list, a);
}

void storeMorph(Patch& patch, u8 variation, Location loc, u8 module, u8 param, u8 group, int range)
{
    checkParam(patch, loc, module, param);
    if (group >= kMorphGroups)
        throw std::invalid_argument("no such morph group");
    putMorph(morphList(patch, variation),
             {static_cast<u8>(loc), module, param, group, static_cast<std::int8_t>(std::clamp(range, -128, 127))});
}

bool repairMorphs(Patch& patch)
{
    bool dropped = false;
    for (auto& v : patch.morphs) {
        std::vector<MorphAssign> kept;
        for (const auto& a : v.assigns) {
            const auto key = morphKey(a);
            if (std::any_of(kept.begin(), kept.end(), [&](const MorphAssign& x) { return morphKey(x) == key; }))
                continue;
            // CPatch::IsParamValid @000d4288: the module exists and has the parameter.
            const auto loc = static_cast<Location>(a.location);
            bool valid = false;
            if (loc == Location::Settings) {
                for (const auto& m : patch.settings)
                    valid = valid || (m.index == a.module && !m.params.empty() && a.param < m.params.front().size());
            } else if (loc == Location::Va || loc == Location::Fx) {
                const Module* m = patch.area(loc).find(a.module);
                valid = m && m->def() && a.param < m->def()->params.size();
            }
            if (!valid) {
                dropped = true;
                continue;
            }
            kept.push_back(a);
        }
        v.assigns = std::move(kept);
    }
    return dropped;
}

std::optional<int> knobOf(const Patch& patch, Location loc, u8 module, u8 param)
{
    for (std::size_t i = 0; i < patch.knobs.size(); ++i)
        if (const auto& k = patch.knobs[i]; k && sameTarget(k->location, k->module, k->param, loc, module, param))
            return static_cast<int>(i);
    return std::nullopt;
}

void assignKnob(Patch& patch, int knob, Location loc, u8 module, u8 param)
{
    checkParam(patch, loc, module, param);
    if (knob < 0 || static_cast<std::size_t>(knob) >= patch.knobs.size())
        throw std::invalid_argument("no such knob");
    // CKnobMap::GetAssignKnobToTargetMolecules @000f1580: 26 for the knob the
    // parameter was on, 26 for the parameter the knob held, 25, then 2D (the
    // knob's page); each undone by its inverse.
    const KnobAssign target{static_cast<u8>(loc), module, 0, param, 0};
    // A parameter sits on one knob only.
    if (const auto old = knobOf(patch, loc, module, param)) {
        const auto& was = *patch.knobs[static_cast<std::size_t>(*old)];
        live::record(live::KnobDeassign{*old}, live::KnobAssign{*old, loc, module, param, was.assignType});
        patch.knobs[static_cast<std::size_t>(*old)].reset();
    }
    if (const auto& held = patch.knobs[static_cast<std::size_t>(knob)])
        live::record(live::KnobDeassign{knob}, live::KnobAssign{knob, static_cast<Location>(held->location), held->module,
                                                                held->param, held->assignType});
    live::record(live::KnobAssign{knob, loc, module, param, 0}, live::KnobDeassign{knob});
    live::record(live::PageFocus{static_cast<u8>(knob >> 3)}, live::PageFocus{static_cast<u8>(knob >> 3)});
    patch.knobs[static_cast<std::size_t>(knob)] = target;
}

void clearKnob(Patch& patch, int knob)
{
    if (knob >= 0 && static_cast<std::size_t>(knob) < patch.knobs.size()) {
        if (const auto& held = patch.knobs[static_cast<std::size_t>(knob)])
            live::record(live::KnobDeassign{knob}, live::KnobAssign{knob, static_cast<Location>(held->location),
                                                                    held->module, held->param, held->assignType});
        patch.knobs[static_cast<std::size_t>(knob)].reset();
    }
}

std::string knobName(int knob)
{
    const int group = knob / kKnobsPerPage;
    return std::to_string(group / 3 + 1) + static_cast<char>('A' + group % 3) + "-" + std::to_string(knob % kKnobsPerPage + 1);
}

std::optional<u8> midiCcOf(const Patch& patch, Location loc, u8 module, u8 param)
{
    for (const auto& c : patch.controllers)
        if (sameTarget(c.location, c.module, c.param, loc, module, param))
            return c.cc;
    return std::nullopt;
}

namespace {
// MIDICtrl::IsValid's table (_gNonValid) and MIDICtrl::IsPreAssigned's (_gPreAssigned).
constexpr std::array<u8, 9> kNonValidCcs{0, 1, 11, 18, 32, 64, 70, 96, 97};
constexpr std::array<u8, 2> kPreAssignedCcs{7, 17};

bool paramExists(const Patch& patch, Location loc, u8 module, u8 param)
{
    try {
        checkParam(patch, loc, module, param);
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}
} // namespace

bool isValidMidiCc(u8 cc)
{
    return cc < 120 && std::find(kNonValidCcs.begin(), kNonValidCcs.end(), cc) == kNonValidCcs.end();
}

bool isPreAssignedMidiCc(u8 cc)
{
    return std::find(kPreAssignedCcs.begin(), kPreAssignedCcs.end(), cc) != kPreAssignedCcs.end();
}

bool canAssignMidiCc(const Patch& patch, Location loc, u8 module, u8 param)
{
    if (!paramExists(patch, loc, module, param))
        return false;
    if (const auto cc = midiCcOf(patch, loc, module, param); cc && isPreAssignedMidiCc(*cc))
        return false;
    if (loc == Location::Settings)
        return true; // every patch setting is MIDI-assignable
    const auto* def = patch.area(loc).find(module)->def();
    return def->params[param].midiAssignable;
}

void storeMidiCc(Patch& patch, u8 cc, Location loc, u8 module, u8 param)
{
    std::erase_if(patch.controllers, [&](const CtrlAssign& c) {
        return c.cc == cc || sameTarget(c.location, c.module, c.param, loc, module, param);
    });
    patch.controllers.push_back({cc, static_cast<u8>(loc), module, param});
}

void eraseMidiCc(Patch& patch, u8 cc)
{
    std::erase_if(patch.controllers, [&](const CtrlAssign& c) { return c.cc == cc; });
}

void assignMidiCc(Patch& patch, u8 cc, Location loc, u8 module, u8 param)
{
    checkParam(patch, loc, module, param);
    if (!isValidMidiCc(cc))
        throw std::invalid_argument("this MIDI controller cannot be assigned");
    if (isPreAssignedMidiCc(cc))
        throw std::invalid_argument("CC 7 and CC 17 are reserved for the patch volume and octave shift");
    if (!canAssignMidiCc(patch, loc, module, param))
        throw std::invalid_argument("this parameter cannot take a MIDI controller");
    // CCtrlMap::GetAssignCtrlToParamMolecules @000f10ae: 23 for the CC the
    // parameter had, 23 for the parameter the CC had, then 22.
    if (live::recording()) {
        for (const auto& c : patch.controllers)
            if (sameTarget(c.location, c.module, c.param, loc, module, param))
                live::record(live::CtrlDeassign{c.cc}, live::CtrlAssign{c.cc, loc, module, param});
        for (const auto& c : patch.controllers)
            if (c.cc == cc && !sameTarget(c.location, c.module, c.param, loc, module, param))
                live::record(live::CtrlDeassign{cc}, live::CtrlAssign{cc, static_cast<Location>(c.location), c.module, c.param});
        live::record(live::CtrlAssign{cc, loc, module, param}, live::CtrlDeassign{cc});
    }
    storeMidiCc(patch, cc, loc, module, param);
}

void clearMidiCc(Patch& patch, u8 cc)
{
    if (isPreAssignedMidiCc(cc))
        throw std::invalid_argument("CC 7 and CC 17 are reserved for the patch volume and octave shift");
    for (const auto& c : patch.controllers)
        if (c.cc == cc)
            live::record(live::CtrlDeassign{cc}, live::CtrlAssign{cc, static_cast<Location>(c.location), c.module, c.param});
    eraseMidiCc(patch, cc);
}

bool repairMidiCcs(Patch& patch)
{
    // CMap::RemoveDuplicateItems, then the parameter and CC checks.
    const std::size_t before = patch.controllers.size();
    std::vector<CtrlAssign> kept;
    for (const auto& c : patch.controllers) {
        const bool duplicate = std::any_of(kept.begin(), kept.end(), [&](const CtrlAssign& k) {
            return k.cc == c.cc || (k.location == c.location && k.module == c.module && k.param == c.param);
        });
        if (duplicate || c.location > 2 || !paramExists(patch, static_cast<Location>(c.location), c.module, c.param))
            continue;
        if (!isValidMidiCc(c.cc) && !isPreAssignedMidiCc(c.cc))
            continue;
        kept.push_back(c);
    }
    patch.controllers = std::move(kept);
    return patch.controllers.size() != before;
}

namespace {

// The captions of a label control from the panel's comma-separated defaults.
std::vector<std::string> defaultCaptions(const db::LabelDef& l)
{
    std::vector<std::string> out;
    std::string_view rest = l.defaults;
    while (out.size() < l.buttons) {
        const auto comma = rest.find(',');
        out.emplace_back(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    }
    return out;
}

// The [kind 1, len, param, ...] record of a parameter in custom data, or nullopt.
std::optional<std::pair<std::size_t, std::size_t>> labelRecord(const std::vector<u8>& bytes, u8 param)
{
    for (std::size_t i = 0; i + 2 <= bytes.size(); i += 2u + bytes[i + 1]) {
        const u8 kind = bytes[i], len = bytes[i + 1];
        if (i + 2 + len > bytes.size())
            break;
        if (kind == 1 && len >= 1 && bytes[i + 2] == param)
            return std::pair{i, std::size_t{len}};
    }
    return std::nullopt;
}

std::string fixedString(const std::vector<u8>& bytes, std::size_t at, std::size_t length)
{
    std::string s;
    for (std::size_t i = at; i < at + length && i < bytes.size() && bytes[i] != 0; ++i)
        s.push_back(static_cast<char>(bytes[i]));
    return s;
}

// CPnlLabelButton / CPnlLabelRadioButton::GetCustomData for every label
// control of the module, in panel order.
std::vector<u8> labelData(const db::ModuleDef& def, const std::vector<std::vector<std::string>>& captions)
{
    std::vector<u8> out;
    for (std::size_t c = 0; c < def.labels.size(); ++c) {
        const auto& l = def.labels[c];
        out.push_back(1);
        out.push_back(static_cast<u8>(l.buttons * kLabelLength + 1));
        out.push_back(l.param);
        for (std::size_t b = 0; b < l.buttons; ++b) {
            std::string text = b < captions[c].size() ? captions[c][b] : std::string();
            text.resize(kLabelLength, '\0');
            out.insert(out.end(), text.begin(), text.end());
        }
    }
    return out;
}

} // namespace

const db::LabelDef* labelControl(const Patch& patch, Location loc, u8 module, u8 param)
{
    if (loc == Location::Settings)
        return nullptr;
    const Module* m = patch.area(loc).find(module);
    const auto* def = m ? m->def() : nullptr;
    if (!def)
        return nullptr;
    for (const auto& l : def->labels)
        if (l.param == param)
            return &l;
    return nullptr;
}

bool canRenameParam(const Patch& patch, Location loc, u8 module, u8 param)
{
    if (loc == Location::Settings) // the morph knobs (CPnlMorphKnob)
        return module == static_cast<u8>(Setting::Morph) && param < kMorphGroups;
    return labelControl(patch, loc, module, param) != nullptr;
}

std::vector<std::string> paramLabels(const Patch& patch, Location loc, u8 module, u8 param)
{
    if (loc == Location::Settings)
        return canRenameParam(patch, loc, module, param) ? std::vector<std::string>{morphLabel(patch, param)}
                                                         : std::vector<std::string>{};
    const auto* l = labelControl(patch, loc, module, param);
    if (!l)
        return {};
    auto captions = defaultCaptions(*l);
    const Module* m = patch.area(loc).find(module);
    if (m->customData)
        if (const auto rec = labelRecord(*m->customData, param))
            for (std::size_t b = 0; b < captions.size() && 1 + (b + 1) * kLabelLength <= rec->second; ++b)
                captions[b] = fixedString(*m->customData, rec->first + 3 + b * kLabelLength, kLabelLength);
    return captions;
}

std::string paramLabel(const Patch& patch, Location loc, u8 module, u8 param)
{
    const auto labels = paramLabels(patch, loc, module, param);
    return labels.empty() ? std::string() : labels.front();
}

void setParamLabel(Patch& patch, Location loc, u8 module, u8 param, const std::string& text, int button)
{
    checkParam(patch, loc, module, param);
    if (!canRenameParam(patch, loc, module, param))
        throw std::invalid_argument("this parameter has no label to rename");
    if (loc == Location::Settings) {
        setMorphLabel(patch, param, text);
        return;
    }
    const std::string label = modularName(text, kLabelLength + 1);
    if (label.size() > kLabelLength)
        throw std::invalid_argument("labels have at most 7 characters");
    const auto* l = labelControl(patch, loc, module, param);
    if (button < 0 || button >= l->buttons)
        throw std::invalid_argument("no such button");
    Module& m = moduleAt(patch, loc, module);
    const auto& def = *m.def();
    std::vector<std::vector<std::string>> captions;
    for (const auto& other : def.labels)
        captions.push_back(paramLabels(patch, loc, module, other.param));
    for (std::size_t c = 0; c < def.labels.size(); ++c)
        if (def.labels[c].param == param)
            captions[c][static_cast<std::size_t>(button)] =
                label.empty() ? defaultCaptions(def.labels[c])[static_cast<std::size_t>(button)] : label;
    m.customData = labelData(def, captions);
}

std::optional<std::vector<u8>> defaultLabelData(u8 moduleType)
{
    const auto* def = db::find(moduleType);
    if (!def || def->labels.empty())
        return std::nullopt;
    std::vector<std::vector<std::string>> captions;
    for (const auto& l : def->labels)
        captions.push_back(defaultCaptions(l));
    return labelData(*def, captions);
}

void setModuleColor(Patch& patch, Location loc, u8 module, u8 color)
{
    Module& m = moduleAt(patch, loc, module);
    // CPatch::InternalRecolorSelection @000d7880: a 31 for every module of
    // the selection, its colour changed or not.
    live::record(live::ModuleColor{loc, module, color}, live::ModuleColor{loc, module, m.color});
    m.color = color;
}

void setLocked(Patch& patch, Location loc, u8 module, bool locked)
{
    Module& m = moduleAt(patch, loc, module);
    // CPatch::InternalLockSelection @000d7fca: a 90 for a module whose lock changes.
    if (m.locked != locked)
        live::record(live::MutaLock{loc, module, locked}, live::MutaLock{loc, module, m.locked});
    m.locked = locked;
}

void setCableBend(Patch& patch, Location loc, const Cable& cable, std::optional<CableBend> bend)
{
    Area& area = areaFor(patch, loc);
    const auto it = std::find_if(area.cables.begin(), area.cables.end(), [&](const Cable& c) {
        return c.fromModule == cable.fromModule && c.fromConn == cable.fromConn && c.fromIsOutput == cable.fromIsOutput
            && c.toModule == cable.toModule && c.toConn == cable.toConn;
    });
    if (it == area.cables.end())
        throw std::invalid_argument("no such cable");
    it->bend = bend;
}

void clearCableBends(Patch& patch)
{
    for (Area* area : {&patch.va, &patch.fx})
        for (auto& c : area->cables)
            c.bend.reset();
}

bool hasCableBends(const Patch& patch)
{
    for (const Area* area : {&patch.va, &patch.fx})
        for (const auto& c : area->cables)
            if (c.bend)
                return true;
    return false;
}

void setCablesVisible(Patch& patch, CableColor color, bool visible)
{
    patch.header.cablesVisible[static_cast<std::size_t>(color)] = visible;
}

void copyVariation(Patch& patch, u8 from, u8 to)
{
    if (from >= patch.variationCount || to >= patch.variationCount)
        throw std::invalid_argument("no such variation");
    if (from == to)
        return;
    // One 44 (CPatchToolbar: Copy To, and Init with from = 8); the original
    // makes no undo of it.
    live::record(live::VariationCopy{from, to}, std::vector<live::Op>{});
    for (auto* area : {&patch.va, &patch.fx})
        for (auto& m : area->modules)
            if (from < m.params.size() && to < m.params.size())
                m.params[to] = m.params[from];
    for (auto& m : patch.settings)
        if (from < m.params.size() && to < m.params.size())
            m.params[to] = m.params[from];
    if (from < patch.morphs.size() && to < patch.morphs.size())
        patch.morphs[to].assigns = patch.morphs[from].assigns;
}

std::size_t variationSlots(const Patch& patch)
{
    return std::max<std::size_t>(patch.variationCount, patch.morphs.size());
}

bool hasAuditionVariation(const Patch& patch)
{
    return patch.morphs.size() > static_cast<std::size_t>(kAuditionVariation);
}

void addAuditionVariation(Patch& patch)
{
    if (hasAuditionVariation(patch))
        return;
    constexpr auto slots = static_cast<std::size_t>(kAuditionVariation) + 1;
    auto extend = [](std::vector<std::vector<u8>>& params) {
        if (params.empty())
            return;
        const auto init = params.size() > 8 ? params[8] : params.back();
        params.resize(slots, init);
    };
    for (auto* area : {&patch.va, &patch.fx})
        for (auto& m : area->modules)
            extend(m.params);
    for (auto& m : patch.settings)
        extend(m.params);
    while (patch.morphs.size() < slots) {
        MorphVariation v{std::vector<u8>(kMorphGroups, 0), {}};
        if (patch.morphs.size() > 8)
            v.assigns = patch.morphs[8].assigns;
        patch.morphs.push_back(std::move(v));
    }
}

std::pair<u8, u8> insertPosition(const Patch& patch, Location loc, const std::vector<u8>& selection)
{
    // The rightmost column with a selected module; in it, the bottom of the
    // lowest selected one (row + height).
    int col = 0, row = 0;
    for (const auto& m : patch.area(loc).modules) {
        if (std::find(selection.begin(), selection.end(), m.index) == selection.end())
            continue;
        const int bottom = m.row + heightOf(m);
        if (m.col > col || (m.col == col && bottom > row)) {
            col = m.col;
            row = bottom;
        }
    }
    return {static_cast<u8>(std::min(col, 127)), static_cast<u8>(std::min(row, 127))};
}

// ---- Clipboard -----------------------------------------------------------------

Clipboard copyModules(const Patch& patch, Location loc, const std::vector<u8>& indices)
{
    Clipboard clip;
    clip.from = loc;
    const Area& area = patch.area(loc);
    int minCol = 127, minRow = 127;
    for (const auto& m : area.modules)
        if (std::find(indices.begin(), indices.end(), m.index) != indices.end()) {
            clip.modules.push_back(m);
            minCol = std::min<int>(minCol, m.col);
            minRow = std::min<int>(minRow, m.row);
        }
    for (auto& m : clip.modules) {
        m.col = static_cast<u8>(m.col - minCol);
        m.row = static_cast<u8>(m.row - minRow);
    }
    auto inside = [&](u8 index) { return std::find(indices.begin(), indices.end(), index) != indices.end(); };
    for (const auto& c : area.cables)
        if (inside(c.fromModule) && inside(c.toModule))
            clip.cables.push_back(c);
    for (std::size_t v = 0; v < patch.morphs.size(); ++v)
        for (const auto& a : patch.morphs[v].assigns)
            if (a.location == static_cast<u8>(loc) && inside(a.module))
                clip.morphs.emplace_back(static_cast<u8>(v), a);
    return clip;
}

std::vector<u8> pasteModules(Patch& patch, Location loc, const Clipboard& clip, u8 col, u8 row)
{
    Area& area = areaFor(patch, loc);
    if (!patchload::canPaste(patch, patchload::dynamicSize(clip.modules, clip.cables.size()))
        || area.modules.size() + clip.modules.size() > 127)
        throw std::invalid_argument("the patch is too big: the synth has no room for these modules");
    const auto before = placesOf(area);
    std::vector<std::pair<u8, u8>> remap; // old index -> new index
    std::vector<u8> added;
    for (const auto& src : clip.modules) {
        u8 index = 1;
        while (area.find(index) || std::find(added.begin(), added.end(), index) != added.end())
            if (++index == 0)
                throw std::invalid_argument("the area is full");
        Module m = src;
        m.index = index;
        m.col = static_cast<u8>(std::min(127, col + src.col));
        m.row = static_cast<u8>(std::min(127, row + src.row));
        // Keep the variation count of the patch.
        if (!m.params.empty())
            m.params.resize(variationSlots(patch), m.params.front());
        // A default-style name gets the next free number ("OscB1" -> "OscB2").
        if (const auto* def = m.def(); def && m.name.rfind(def->shortName, 0) == 0) {
            int number = 1;
            auto taken = [&](int n) {
                const std::string name = def->shortName + std::to_string(n);
                return std::any_of(area.modules.begin(), area.modules.end(), [&](const Module& x) { return x.name == name; });
            };
            while (taken(number))
                ++number;
            m.name = std::string(def->shortName) + std::to_string(number);
        }
        area.modules.push_back(std::move(m));
        added.push_back(index);
        remap.emplace_back(src.index, index);
    }
    auto mapped = [&](u8 old) {
        for (const auto& [o, n] : remap)
            if (o == old)
                return n;
        return u8{0};
    };
    Area pasted; // the new modules' cables, for the undo's 51s
    for (Cable c : clip.cables) {
        c.fromModule = mapped(c.fromModule);
        c.toModule = mapped(c.toModule);
        if (loc != clip.from)
            c.color = cableColor(patch, loc, {c.fromModule, c.fromConn, c.fromIsOutput});
        area.cables.push_back(c);
        pasted.cables.push_back(c);
    }
    std::vector<std::pair<std::size_t, MorphAssign>> morphs; // as added
    // Morphs, per variation, module by module, parameter by parameter, group
    // by group; each counts one, and none is added past 25
    // (CPatchData::GetNewModulesMolecules @000eba8e).
    for (std::size_t v = 0; v < patch.morphs.size(); ++v) {
        int count = morphedParamCount(patch, static_cast<u8>(v));
        for (const auto& src : clip.modules) {
            std::vector<MorphAssign> own;
            for (const auto& [var, a] : clip.morphs)
                if (var == v && a.module == src.index && a.range != 0)
                    own.push_back(a);
            std::sort(own.begin(), own.end(),
                      [](const MorphAssign& a, const MorphAssign& b) { return std::pair(a.param, a.morph) < std::pair(b.param, b.morph); });
            for (auto a : own) {
                if (++count > kMaxMorphedParams)
                    break;
                a.location = static_cast<u8>(loc);
                a.module = mapped(a.module);
                putMorph(patch.morphs[v].assigns, a);
                morphs.emplace_back(v, a);
            }
        }
    }
    for (u8 index : added)
        placeBelow(patch, loc, index);
    // What the original sends (CClipboard::Drop @0014d882): the make-room
    // 34s, then GetNewModulesMolecules @000eba8e: a 30 per module, the new
    // data's 52, 4D, the morphs (43), 5B, 5A; then the rates. The undo takes
    // the morphs back (43 range 0), deletes the cables (51, as parts of a
    // dump) and the modules (32), and moves the others back.
    recordMoves(area, loc, before);
    if (live::recording()) {
        for (const auto& m : area.modules)
            if (std::find(added.begin(), added.end(), m.index) != added.end())
                live::record(live::ModuleNew{loc, m}, live::ModuleDelete{loc, m.index});
        std::vector<live::Op> unlink;
        for (const auto& seg : cables::Tree(pasted).segments())
            unlink.insert(unlink.begin(), live::CableDelete{loc, seg.node, seg.parent, true});
        auto sections = live::moduleSections(patch, loc, added);
        for (auto& section : sections) {
            const u8 id = section.id;
            live::record(live::Section{std::move(section)}, id == file::kCableList ? unlink : std::vector<live::Op>{});
            if (id == file::kParamList)
                for (const auto& [v, a] : morphs)
                    live::record(live::Morph{loc, a.module, a.param, a.morph, a.range, static_cast<u8>(v)},
                                 live::Morph{loc, a.module, a.param, a.morph, 0, static_cast<u8>(v)});
        }
    }
    uprate::update(patch, loc);
    return added;
}

void removeModules(Patch& patch, Location loc, const std::vector<u8>& indices)
{
    // CPatch::InternalDeleteSelection @000d7e16, in the original's order:
    // per module (selection order) its knobs (26), controllers (23) and morphs
    // (43 range 0, every variation; CKnobMap / CCtrlMap / CMorphMap::
    // GetDeleteModuleMolecules); then the selection's cables as one
    // (GenerateDeleteSelectionMolecules: 54 white, 51, 50 relinks); then the
    // modules (32, CPatchSelection::GetDeleteModuleMolecule); then the rates.
    Area& area = areaFor(patch, loc);
    for (u8 index : indices)
        if (!area.find(index))
            throw std::invalid_argument("no module with this index");
    for (u8 index : indices) {
        for (std::size_t k = 0; k < patch.knobs.size(); ++k)
            if (auto& a = patch.knobs[k]; a && refersTo(a->location, a->module, loc, index)) {
                live::record(live::KnobDeassign{static_cast<int>(k)},
                             live::KnobAssign{static_cast<int>(k), loc, a->module, a->param, a->assignType});
                a.reset();
            }
        std::erase_if(patch.controllers, [&](const CtrlAssign& c) {
            if (!refersTo(c.location, c.module, loc, index))
                return false;
            live::record(live::CtrlDeassign{c.cc}, live::CtrlAssign{c.cc, loc, c.module, c.param});
            return true;
        });
        for (std::size_t v = 0; v < patch.morphs.size(); ++v)
            std::erase_if(patch.morphs[v].assigns, [&](const MorphAssign& a) {
                if (!refersTo(a.location, a.module, loc, index))
                    return false;
                live::record(live::Morph{loc, a.module, a.param, a.morph, 0, static_cast<u8>(v)},
                             live::Morph{loc, a.module, a.param, a.morph, a.range, static_cast<u8>(v)});
                return true;
            });
    }
    spliceOut(patch, loc, indices, false);
    for (u8 index : indices) {
        const auto it = std::find_if(area.modules.begin(), area.modules.end(), [&](const Module& m) { return m.index == index; });
        if (it == area.modules.end())
            continue;
        if (live::recording())
            live::record(live::ModuleDelete{loc, index}, recreate(patch, loc, *it));
        area.modules.erase(it);
        std::erase_if(area.cables, [&](const Cable& c) { return c.fromModule == index || c.toModule == index; });
    }
    uprate::update(patch, loc);
}

void moveModules(Patch& patch, Location loc, const std::vector<u8>& indices, int dCol, int dRow)
{
    Area& area = areaFor(patch, loc);
    const auto before = placesOf(area);
    for (auto& m : area.modules)
        if (std::find(indices.begin(), indices.end(), m.index) != indices.end()) {
            m.col = static_cast<u8>(std::clamp(m.col + dCol, 0, 127));
            m.row = static_cast<u8>(std::clamp(m.row + dRow, 0, 127));
        }
    // The moved group keeps its place; others make room below: the
    // original sends those 34s first, then the moved modules'.
    for (u8 index : indices)
        placeBelow(patch, loc, index);
    recordMoves(area, loc, before, indices);
}

} // namespace g2::edit
