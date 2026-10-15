#include "g2/cables.hpp"

#include <algorithm>

namespace g2::cables {
namespace {

constexpr int kVirtualRoot = 0; // the CTree's own node, parent of every tree's root
constexpr int kErased = -2;

void removeAll(std::vector<int>& list, int n)
{
    std::erase(list, n);
}

} // namespace

Tree::Tree()
{
    nodes_.push_back(Node{});
}

Tree::Tree(const Area& area) : Tree()
{
    // CPatch::HandleCableDump @000ddd2c: ConnectCable per cable, in file order.
    for (const auto& c : area.cables)
        connect({c.fromModule, c.fromConn, c.fromIsOutput}, {c.toModule, c.toConn, false}, c.color);
}

int Tree::find(Connector c) const
{
    for (std::size_t i = 1; i < nodes_.size(); ++i)
        if (nodes_[i].parent != kErased && nodes_[i].c == c)
            return static_cast<int>(i);
    return -1;
}

int Tree::insertRoot(Connector c, CableColor color)
{
    const int n = static_cast<int>(nodes_.size());
    nodes_.push_back(Node{c, color, kVirtualRoot, {}});
    nodes_[kVirtualRoot].children.push_back(n);
    return n;
}

// CTree::CNode::reorder @0001b320: makes n a child of newParent, turning the
// path from n up to its root upside down (n's ancestors become its
// descendants, each added last to its new parent's children).
int Tree::reorder(int n, int newParent)
{
    if (nodes_[n].parent == -1) {
        removeAll(nodes_[n].children, newParent);
        return n;
    }
    const int r = reorder(nodes_[n].parent, n);
    if (r != nodes_[n].parent)
        nodes_[n].children.push_back(nodes_[n].parent);
    removeAll(nodes_[n].children, newParent);
    nodes_[n].parent = newParent;
    return r;
}

// CTree::join @0001b392: a's tree, re-rooted at a, hangs last below b.
void Tree::join(int a, int b)
{
    reorder(a, b);
    nodes_[b].children.push_back(a);
}

// CTree::split (CNode::move @0001b7f0): n and its subtree become a tree of their own.
void Tree::split(int n)
{
    removeAll(nodes_[nodes_[n].parent].children, n);
    nodes_[n].parent = kVirtualRoot;
    nodes_[kVirtualRoot].children.push_back(n);
}

// CTree::CNode::erase @0001b642: n's children take its place, in order.
void Tree::eraseNode(int n)
{
    const int p = nodes_[n].parent;
    auto& siblings = nodes_[p].children;
    const auto at = std::find(siblings.begin(), siblings.end(), n);
    const auto children = std::move(nodes_[n].children);
    for (int c : children)
        nodes_[c].parent = p;
    siblings.insert(siblings.erase(at), children.begin(), children.end());
    nodes_[n].children.clear();
    nodes_[n].parent = kErased;
}

int Tree::base(int n) const
{
    while (nodes_[n].parent != kVirtualRoot)
        n = nodes_[n].parent;
    return n;
}

void Tree::preorder(int n, std::vector<int>& out) const
{
    out.push_back(n);
    for (int c : nodes_[n].children)
        preorder(c, out);
}

std::vector<Tree::Segment> Tree::segmentsBelow(int n, bool includeSelf) const
{
    std::vector<int> order;
    preorder(n, order);
    std::vector<Segment> out;
    for (int m : order)
        if (m != n || includeSelf)
            out.push_back({nodes_[m].c, nodes_[nodes_[m].parent].c, nodes_[m].color});
    return out;
}

bool Tree::contains(Connector c) const
{
    return find(c) >= 0;
}

bool Tree::isLeaf(Connector c) const
{
    const int n = find(c);
    return n >= 0 && nodes_[n].children.empty();
}

bool Tree::isRoot(Connector c) const
{
    const int n = find(c);
    return n >= 0 && nodes_[n].parent == kVirtualRoot;
}

Connector Tree::chainId(Connector c) const
{
    const int n = find(c);
    if (n < 0)
        return c;
    std::vector<int> order;
    preorder(base(n), order);
    for (int m : order)
        if (nodes_[m].c.isOutput)
            return nodes_[m].c;
    return nodes_[order.front()].c;
}

std::optional<CableColor> Tree::colorOf(Connector c) const
{
    const int n = find(c);
    return n >= 0 ? std::optional<CableColor>(nodes_[n].color) : std::nullopt;
}

std::optional<Connector> Tree::parentOf(Connector c) const
{
    const int n = find(c);
    if (n < 0 || nodes_[n].parent == kVirtualRoot)
        return std::nullopt;
    return nodes_[nodes_[n].parent].c;
}

std::vector<Connector> Tree::childrenOf(Connector c) const
{
    std::vector<Connector> out;
    if (const int n = find(c); n >= 0)
        for (int m : nodes_[n].children)
            out.push_back(nodes_[m].c);
    return out;
}

bool Tree::inSubtree(Connector in, Connector c) const
{
    const int n = find(in), m = find(c);
    if (n < 0 || m < 0)
        return false;
    for (int x = m; x > kVirtualRoot; x = nodes_[x].parent)
        if (x == n)
            return true;
    return false;
}

std::vector<Tree::Segment> Tree::segments() const
{
    std::vector<Segment> out;
    for (int root : nodes_[kVirtualRoot].children) {
        auto below = segmentsBelow(root, false);
        out.insert(out.end(), below.begin(), below.end());
    }
    return out;
}

std::vector<Tree::Segment> Tree::completeTree(Connector c) const
{
    const int n = find(c);
    return n < 0 ? std::vector<Segment>{} : segmentsBelow(base(n), false);
}

std::vector<Tree::Segment> Tree::completeBranch(Connector c) const
{
    int n = find(c);
    if (n < 0)
        return {};
    if (nodes_[n].parent == kVirtualRoot)
        return segmentsBelow(n, false);
    const int root = base(n);
    while (nodes_[n].parent != root)
        n = nodes_[n].parent;
    return segmentsBelow(n, true);
}

std::vector<Tree::Segment> Tree::partialTree(Connector c) const
{
    const int n = find(c);
    return n < 0 ? std::vector<Segment>{} : segmentsBelow(n, false);
}

void Tree::connect(Connector from, Connector to, CableColor color)
{
    int a = find(from);
    if (a < 0)
        a = insertRoot(from, color);
    else
        nodes_[a].color = color;
    int b = find(to);
    if (b < 0)
        b = insertRoot(to, color);
    else
        nodes_[b].color = color;
    if (base(a) == base(b))
        return;
    if (chainId(from).isOutput)
        join(b, a);
    else
        join(a, b);
}

void Tree::erase(Connector a, Connector b)
{
    const int na = find(a), nb = find(b);
    if (na < 0 || nb < 0)
        return;
    int child, parent;
    if (nodes_[na].parent == nb)
        child = na, parent = nb;
    else if (nodes_[nb].parent == na)
        child = nb, parent = na;
    else
        return;
    split(child);
    if (nodes_[child].children.empty())
        eraseNode(child);
    if (nodes_[parent].parent == kVirtualRoot && nodes_[parent].children.empty())
        eraseNode(parent);
}

void Tree::recolor(Connector a, Connector b, CableColor color)
{
    const int na = find(a), nb = find(b);
    if (na < 0 || nb < 0)
        return;
    if (nodes_[na].parent != nb && nodes_[nb].parent != na)
        return;
    nodes_[na].color = color;
    nodes_[nb].color = color;
}

void Tree::makeBase(Connector c)
{
    const int n = find(c);
    if (n < 0 || nodes_[n].parent == kVirtualRoot)
        return;
    auto& roots = nodes_[kVirtualRoot].children;
    const auto at = std::find(roots.begin(), roots.end(), base(n)) - roots.begin();
    reorder(n, kVirtualRoot);
    roots.insert(roots.begin() + std::min<std::ptrdiff_t>(at, static_cast<std::ptrdiff_t>(roots.size())), n);
}

void Tree::eraseConnector(Connector c)
{
    if (const int n = find(c); n >= 0)
        eraseNode(n);
}

void Tree::keepTreeIntactErase(Connector c)
{
    const int n = find(c);
    if (n < 0)
        return;
    if (nodes_[n].parent == kVirtualRoot && nodes_[n].children.size() > 1) {
        const int first = nodes_[n].children.front();
        const std::vector<int> others(nodes_[n].children.begin() + 1, nodes_[n].children.end());
        for (const int o : others) {
            split(o);
            join(o, first);
        }
    }
    eraseNode(n);
}

std::vector<Connector> Tree::roots() const
{
    std::vector<Connector> out;
    for (int n : nodes_[kVirtualRoot].children)
        out.push_back(nodes_[n].c);
    return out;
}

std::vector<Connector> Tree::nodes() const
{
    std::vector<int> order;
    preorder(kVirtualRoot, order);
    std::vector<Connector> out;
    for (std::size_t i = 1; i < order.size(); ++i)
        out.push_back(nodes_[order[i]].c);
    return out;
}

std::optional<Connector> Tree::nextRoot(Connector c) const
{
    const auto& roots = nodes_[kVirtualRoot].children;
    const auto at = std::find(roots.begin(), roots.end(), find(c));
    if (at == roots.end() || at + 1 == roots.end())
        return std::nullopt;
    return nodes_[*(at + 1)].c;
}

Tree Tree::treeOf(Connector c) const
{
    Tree copy;
    const int n = find(c);
    if (n < 0)
        return copy;
    // The same connectors, colours and children order (depth-first, every
    // parent is copied before its children).
    const int root = base(n);
    copy.insertRoot(nodes_[root].c, nodes_[root].color);
    std::vector<int> order;
    preorder(root, order);
    for (std::size_t i = 1; i < order.size(); ++i) {
        const int m = order[i];
        const int parent = copy.find(nodes_[nodes_[m].parent].c);
        const int added = static_cast<int>(copy.nodes_.size());
        copy.nodes_.push_back(Node{nodes_[m].c, nodes_[m].color, parent, {}});
        copy.nodes_[parent].children.push_back(added);
    }
    return copy;
}

Cable cableOf(const Tree::Segment& s)
{
    // CCableData_11::WriteStream @00064d9a: from the output when either end
    // is one, else from the node to its parent.
    Connector from = s.node, to = s.parent;
    if (!s.node.isOutput && s.parent.isOutput)
        std::swap(from, to);
    return Cable{s.color, from.module, from.conn, from.isOutput, to.module, to.conn, std::nullopt};
}

std::vector<Cable> Tree::toCables(const std::vector<Cable>& previous) const
{
    auto same = [](const Cable& c, Connector a, Connector b) {
        const Connector from{c.fromModule, c.fromConn, c.fromIsOutput}, to{c.toModule, c.toConn, false};
        return (from == a && to == b) || (from == b && to == a);
    };
    std::vector<Cable> out;
    for (const auto& s : segments()) {
        Cable c = cableOf(s);
        for (const auto& p : previous)
            if (p.bend && same(p, s.node, s.parent)) {
                c.bend = p.bend;
                break;
            }
        out.push_back(c);
    }
    return out;
}

} // namespace g2::cables
