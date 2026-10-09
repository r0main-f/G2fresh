# Module uprate (audio rate vs control rate)

How the original editor (Mac v1.62, `G2Editor_i386`) decides each module's
uprate bit (`CModule+0x40`, `NSFile_V11::EModuleBandWidth`, 0 = control rate,
1 = audio rate), and how cable colours follow it. Implemented in
`core/src/uprate.cpp` (`g2::uprate::update`), tested in `tests/test_uprate.cpp`.

## Where it is computed

`CPatchData::GenerateBandwidthChangeMolecules(CUndoRedoPair&)` @000e5048. It
runs on a clone of the area's patch data with the edit already applied, from:

| caller | address |
|---|---|
| `GetNewCableMolecules` | @000eb87c |
| `GetDisconnectCableNodeMolecules` | @000ea48a |
| `GetMoveCableNodeMolecules` | @000eab6c |
| `GetBreakCableNodeMolecules` | @000eb27a |
| `GetDeleteSelectionMolecules` | @000e8c72 |
| `GetNewModulesMolecules` (add, paste) | @000eba8e |
| `GetReplaceModuleMolecules` | @000ebf7e |

Nothing recomputes on load: a file keeps the bits it was saved with. New
modules start at 0 (`CBuildModule::GetBandWidth` @000a9d0c returns 0; set by
`CPatch::InternalNewModule`). The only other writer of the bit is
`CPatchData::SetABCode` (@000e911e), which applies an incoming
`CMModuleABCode` (message `2A`, S_SET_UPRATE).

## The rule

The cable network is a forest (`CTree` at `CPatchData+0xc`). Each tree is
one net; a net holds at most one output and, when it has one, the tree is
rooted at it (`ConnectCable` @000e7352 joins the tree without an output under
the one with an output). `GetCableChainID` @000e407a returns a connector's
net output, or the tree root (an input) when the net has none.

```
do {
  changed = false
  for each module M in the area's module list (creation order):
    rate = 0
    for each tree edge (parent P -> child C) with C an input of M      // CConnectedCablesIterator
      if C is dynamic (CPanel::IsMultiBandWidth @000bed94, hole+0x47):
        S = P if P is an output else GetCableChainID(P)               // links
        if S is an output and GetBandWidth(S) == 1: rate = 1; break
    if rate != M.uprate:
      push CMModuleABCode(M, rate) (redo) / (M, old) (undo)           // message 2A
      for each dynamic output O of M that is in a tree:
        recolour every cable of O's net to O's colour at `rate`
      M.uprate = rate                                                  // SetABCodeState @000a9d18
      changed = true
} while (changed)
```

`CPanel::GetBandWidth(connector)` @000be72e: a dynamic connector reports its
module's uprate; a static one reports 1 iff its colour (`CPnlHole` vfunc
0xd0, `GetColor()`) is red (`EConnectorColor` 1). So:

* **Triggers**: only *dynamic* inputs (blue/red, yellow/orange). Static
  inputs never uprate a module, even when fed with audio (OscB Sync, FmMod).
* **Audio-rate sources**: static red (audio) outputs, and dynamic outputs of
  an uprated module (red or orange). Static blue/yellow outputs never uprate.
* **Links**: an input-to-input link takes the rate of its net's output; a net
  without an output carries nothing.
* **Propagation**: the outer loop repeats until a pass changes nothing; a
  module changed earlier in a pass is seen by the later ones.
* **Fixed-rate modules**: a module with no fed dynamic input gets 0, so a
  stale 1 on any module is cleared at the next recomputation.
* **Cycles / hysteresis**: the loop starts from the current bits, not from
  zero. A feedback loop of dynamic modules that is already uprated keeps
  itself uprated after its audio source is removed (each module still sees an
  uprated output on its input). Starting from all zeros would give the least
  fixed point; the original does not do that. With a mixed initial state the
  result can depend on the module order (the list is `push_back`ed in
  creation order, i.e. the file's module-list order).

## Cable colours

Rate change (above): every cable of the net of each connected **dynamic**
output of a module whose rate flipped is recoloured (`GetRecolorCableMolecules`
@000e43f8, `CMCableRecolor`) to `GetColor(bw)` of the output hole, mapped by
`MapConnectorColorToCableColor` @000e3a24 (connector colour 2→blue,
3→orange, 0→yellow, else red):

* blue/red output (`CPnlControlOutHole::GetColor` @000ca268, cf. InHole
  @000ca1ea: `bw == 1 ? red : blue`): red when uprated, blue otherwise;
* yellow/orange output (`CPnlLogicOutHole::GetColor` @000ca43c:
  `bw == 1 ? orange : yellow`).

Static outputs are never recoloured by a rate change, and nets of modules
whose rate did not change keep their colour. Users can recolour a net
from a connector's context menu (`CPanel::HoleDoContextMenu` →
`CPatch::InternalRecolorCable` @000d6fb6, any `ECableColor`); that colour
lasts until the source module's rate flips, which overwrites it.

Related rules outside the uprate pass (in the cable edits, not in
`uprate::update`):

* New cable (`GetNewCableMolecules` @000eb87c → `GetConnectRecolorMolecules`
  @000e4c32): the end whose net has an output gives the colour: the output
  connector's colour at its module's current rate when that end is the
  output itself, else the colour already stored on that end's net (so a
  user colour spreads). The other end's whole net is recoloured to it. If
  neither end's net has an output, the cable is **white** (6).
* A cable delete / move / break that leaves a part of a net without its
  output recolours that part white (`GetRecolorCableMolecules(…, 6)` in
  @000ea48a, @000eab6c, @000eb27a, @000e7fe4).

## Corpus agreement

All 5,589 readable files of `corpus-external/` (3 unreadable) plus
`tests/corpus/pch2csd/`, every VA and FX area with modules: 12,320 areas,
355,551 modules (201,756 with dynamic connectors, 80,875 stored uprated).

| check | areas | module bits |
|---|---|---|
| stored bits are a fixed point (`update` changes nothing) | 12,301 / 12,320 (99.85%) | 355,417 / 355,551 (99.96%) |
| recomputed from cleared bits (least fixed point) | 12,023 / 12,320 (97.6%) | 352,660 / 355,551 (99.19%) |

The from-zero mismatches in stable areas are all stored 1 / recomputed 0,
i.e. modules held up by an uprated feedback loop: this is the hysteresis
above (a least-fixed-point rule would never save them). The 19 non-stable
areas are older or odd files: patches from factory banks of older editors
(`Crazy Noise`, file version 19, references a FreqShift output that no
longer exists; `ClaviNeck` FX, an unconnected SwOnOffT stored uprated),
hidden legacy modules (Resonator with static inputs stored uprated), and a
dozen datanoisetv patches where an uprated module's output cable is still
blue and its downstream modules are at control rate, i.e. saved without the
recomputation running. The committed corpus (pch2csd + Verhue's
`hi_hat_machine.pch2`) matches exactly, from stored and from cleared bits.

Cable colours, 500,384 cables: in nets with an output, 487,938 match the
output's colour at its module's rate (`edit::cableColor`); 11,019 others are
user colours (green, purple, or another hue chosen by hand) and 299 (dynamic
sources only: blue/red or yellow/orange swapped) contradict the source
module's rate (stale, as above); 18 reference connectors missing from the
module database. Of 1,110 cables in nets without an
output, 1,108 are white.
