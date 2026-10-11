// The cable trees of an area, as Clavia's editor keeps them (CCableTree, a
// CTree of connectors, CPatchData+0xc).
//
// Each net is a tree whose root is its output when it has one; every other
// connector hangs from the connector it is cabled to on the way to the root,
// so a cable is the link between a node and its parent. The file lists the
// cables by walking the trees depth-first (CPatchData::CCableSegmentIterator):
// a cable from an output is written output -> input, an input-to-input link
// child -> parent, i.e. pointing towards the output (CCableData_11::
// WriteStream @00064d9a). Loading replays the list through
// CPatchData::ConnectCable @000e7352, which rebuilds the same trees, so walking
// them again gives the file's cable list back (true for 97% of the corpus; the
// rest was written by older editors or other programs).
//
// The editing operations of the original are sequences of three primitives
// on these trees (the "molecules" CMCableConnect, CMCableDelete and
// CMCableRecolor): connect(), erase() and recolor() below. g2::edit builds
// those sequences as the original does and writes the trees back with
// toCables().
#pragma once

#include "g2/patch.hpp"

#include <optional>
#include <vector>

namespace g2::cables {

struct Connector {
    u8 module = 0, conn = 0;
    bool isOutput = false;
    bool operator==(const Connector&) const = default;
};

class Tree {
public:
    // A cable between `node` and its `parent`, with the node's colour (the
    // colour of the cable to its parent).
    struct Segment {
        Connector node, parent;
        CableColor color = CableColor::Red;
    };

    Tree();
    // Replays the area's cables in list order, as the original loads them.
    explicit Tree(const Area& area);

    bool contains(Connector c) const; // CPatchData::IsInTree
    bool isLeaf(Connector c) const;   // CPatchData::IsLeafNode (false if absent)
    bool isRoot(Connector c) const;   // the base of its tree
    // CPatchData::GetCableChainID @000e407a: the net's output, or else the
    // root of its tree; a connector without cables is its own chain.
    Connector chainId(Connector c) const;
    // CPatchData::HasSignal: the net has an output.
    bool hasSignal(Connector c) const { return chainId(c).isOutput; }
    std::optional<CableColor> colorOf(Connector c) const;
    std::optional<Connector> parentOf(Connector c) const; // nullopt for a root or an absent connector
    std::vector<Connector> childrenOf(Connector c) const;
    // Whether `c` is `in` or one of its descendants (CTree::find_partial).
    bool inSubtree(Connector in, Connector c) const;

    // The cables, depth-first, as the original iterates them.
    std::vector<Segment> segments() const;                  // CCableSegmentIterator: every tree
    std::vector<Segment> completeTree(Connector c) const;   // CCompleteTreeIterator: c's whole tree
    // CCompleteBranchIterator @0001b8d0: the branch of the root that holds c
    // (the cable from the root and everything below it); the whole tree when
    // c is the root. What the jack popup's Color and Delete act on.
    std::vector<Segment> completeBranch(Connector c) const;
    std::vector<Segment> partialTree(Connector c) const;    // CPartialTreeIterator: the cables below c

    // CPatchData::ConnectCable @000e7352: adds both connectors if needed (both
    // take `color`) and hangs one tree from the other: `to`'s tree below
    // `from` when `from`'s net has an output, else `from`'s tree (re-rooted
    // at `from`) below `to`. Two connectors already in one tree are left
    // alone (the original would corrupt its tree).
    void connect(Connector from, Connector to, CableColor color);
    // CPatchData::DeleteCable @000e7180: removes the cable between a node and
    // its parent; the node's subtree becomes a tree of its own, connectors
    // left without cables leave the tree. Nothing happens if a and b are not
    // cabled together.
    void erase(Connector a, Connector b);
    // CPatchData::RecolorCable @000e465c: the cable between a and b (either
    // way round) takes `color`, set on both of its connectors as the
    // original's CCableSegmentIterator::SetColor does.
    void recolor(Connector a, Connector b, CableColor color);
    // CTree::make_base: re-roots c's tree at c, in the same place among the trees.
    void makeBase(Connector c);
    // CTree::erase: removes a connector, its children taking its place.
    void eraseConnector(Connector c);

    std::vector<Connector> roots() const; // the trees' roots, in order
    std::vector<Connector> nodes() const; // every connector, depth-first (CTree::CIterator)
    // The next tree's root after root `c` (CTree::CIterator::get_sibling), if any.
    std::optional<Connector> nextRoot(Connector c) const;
    // A copy of the tree that holds c (CTree::copy).
    Tree treeOf(Connector c) const;

    // The cables as the original writes them (see above), keeping the bend
    // of each cable of `previous` that has the same two ends.
    std::vector<Cable> toCables(const std::vector<Cable>& previous = {}) const;

private:
    struct Node {
        Connector c;
        CableColor color = CableColor::Red;
        int parent = -1; // 0 for a root (node 0 is the virtual root above them)
        std::vector<int> children;
    };
    std::vector<Node> nodes_;

    int find(Connector c) const; // -1 if absent
    int insertRoot(Connector c, CableColor color);
    int reorder(int n, int newParent);
    void join(int a, int b);
    void split(int n);
    void eraseNode(int n);
    int base(int n) const;
    void preorder(int n, std::vector<int>& out) const;
    std::vector<Segment> segmentsBelow(int n, bool includeSelf) const;
};

// The cable between a segment's two connectors, oriented as the file stores it.
Cable cableOf(const Tree::Segment& s);

} // namespace g2::cables
