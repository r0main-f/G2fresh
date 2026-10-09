# Module Replace (editor v1.62)

How the original editor (Mac, i386) replaces a module by another of the same family,
and how G2fresh reproduces it (`g2::replace`, `core/include/g2/replace.hpp`).

Pipeline (the facts are committed; the build needs neither Ghidra nor Clavia files):

```
ReplaceDisasm.java <dir> 0010ae74 -> <dir>/0010ae74.asm        (CReplaceDataBase::AddData)
tools/moduledb/extract_replace_facts.py <dir>/0010ae74.asm  -> tools/moduledb/replace_facts.json
tools/moduledb/gen_replace.py [--check]  -> data/replace.json, core/src/replace_data.cpp
```

`ReplaceDisasm.java` takes any number of function addresses; the other functions cited
below were read from its output (`000ebf7e`, `000d7a14`, `000f09aa`, `000f12de`, `000f1428`,
`000ff4c4`, `000bf2d8`, `000aa04c`, `00085d9c`, `001176d6`) and from `re/out/decomp/`.

## 1. User interface

* Every module built by `CBuildModule` has a **Replace button**: `CBuildModule+0x60`
  (`HasReplaceButton` @ `000a9f40`) is 1 for all `CMBGeneric` modules, 0 for the name bar
  and the patch-settings pseudo-modules. `CPanel::CPanel` @ `000c2b6a` creates a
  `CReplaceView` at (1,1) in the category colour.
* Click → `CPanel::HandleChangeRequests` @ `000bf2d8`: builds a
  `CModuleReplaceMenu(type, context)` and pops it up at the button
  (`DoReplaceMenu` @ `00117670`, only when `CanReplace`, i.e. the list has more than one
  entry). A choice calls `CPatch::InternalReplaceModule(context, oldType, index, newType)`.
* Menu (`CModuleReplaceMenu::CModuleReplaceMenu` @ `001176d6`):
  `CReplaceDataBase::GetReplaceCandidates` (@ `00109edc`) returns **all types of the
  module's group in the order of `AddData`** (`FindGroup` @ `001097b4`: first group that
  contains the type; no type is in two groups). One item per type, text =
  `CBuildModule::GetToolbarHintText` @ `000aa04c` = builder+4 = the PANL `Tooltip`
  (`longName`, e.g. "LFO B"), item id = `0x10000 | type`. An item is **disabled**
  (`CMenuItemInfo+4`, see `MacSetItemInfo` @ `00162a00`) when it is the module's own type,
  when `IsModuleAvailable` is false (never), or when `CBuildModule::IsValidContext` @
  `000aa00e` refuses the area (only Fx-In, FX only).

## 2. The database (`CReplaceDataBase::AddData` @ `0010ae74`)

Straight-line code (decompiler times out) calling builder methods with immediate records:

| Call | Record |
|------|--------|
| `Module_Begin(name)` @ `0010a160` | new `CModuleGroup` (0x3c bytes) |
| `Add(uchar const&)` @ `0010a20a` | a member type (menu order) |
| `Begin_Input(name)` @ `0010a46e` + `Add(CInput)` @ `00109bca` | input class; item = `{type, connector, amountParam, onParam}` (0xFF none) |
| `Begin_Output(name)` @ `0010a4b8` + `Add(COutput)` @ `00109bee` | output class; item = `{type, connector}` |
| `Begin_Param(name)` @ `0010a230` + `Add(CParam)` @ `00109bb8` | parameter class; item = `{type, param}` |
| `Begin_Value` @ `0010adba` / `Add(CValue)` @ `00109bdc` | never called: **no value mapping tables** |
| `Module_End` @ `001096c4` | |

A "class" (`CItemGroup<T,uchar>`, 0x10 bytes) lists the items of the group's modules that
stand for one another, e.g. LFO Group / "Params Mono" = LfoA Mode, LfoB Mode, LfoShpA Mode,
LfoC Mode. `CModuleGroup` holds the types (+0), input classes (+0xc), output classes
(+0x18), param classes (+0x24) and value classes (+0x30, empty).

Totals: 19 groups, 159 types, 53 input classes (396 items, 103 with an attenuator,
23 with an on/off button: mixers), 26 output classes (267 items), 110 parameter classes
(539 items). Parameter indices are parameters, never modes. All indices were checked
against `data/modules.json` (`gen_replace.py` asserts it).

## 3. The mapping (`CModuleReplacer::CModuleReplacer` @ `0010a6e6`)

Arguments: (newType, oldType, connected inputs, connected outputs of the old module).
`CPatch::InternalReplaceModule` @ `000d7a14` lists the connectors that are in a cable
tree (`CPatchData::IsInTree`), in ascending order.

1. `CReplaceDataBase::CloneGroup` @ `0010a688` → `CModuleGroup::Clone` @ `0010a502`: a
   working copy of the group keeping only items of the two types.
2. Tables `params[oldParamCount]`, `inputs[oldInputCount]`, `outputs[oldOutputCount]`,
   all 0xFF.
3. For each connected input `c`: `MapInput` @ `00109b00` = `GetCompatibleInputGroup`
   (first class containing `(old, c)`) then `FindAndRemoveFirst(new)` (first item of the
   new type in that class, **removed** so it is used once). If found: `inputs[c] =
   newItem.conn`, and when both items have an attenuator `params[old.amount] =
   new.amount` (same for the on/off button).
4. `TransferUnMappedAmounts` @ `0010a27a`, then `TransferUnMappedOns` @ `0010a374`: each
   input class (what is left of it: all old items, unused new items) with attenuators
   becomes an extra param class `{type, amountParam}`, appended after the real ones; then
   the same for on/off buttons. So attenuators of unconnected inputs still map.
5. For each connected output: `MapOutput` @ `00109b5c`, same rule.
6. For each old param `p` still 0xFF, in index order: `MapParam` @ `00109aa4`, same rule
   (first class containing `(old, p)`, first remaining new item).

`MapSrcParam/Input/Output` @ `001099d0/0010999a/00109964` read the tables;
`MapConnector` @ `00109a02` maps a `CConnector {module, conn, isOutput}`.

## 4. The replace (`CPatchData::GetReplaceModuleMolecules` @ `000ebf7e`)

* `GenerateDeleteSelectionMolecules(…, relink = false)` @ `000e7fe4` on a clone: removes
  the old module and every cable segment touching it, **without** re-linking; a tree whose
  source (`GetCableChainID` @ `000e407a`: the tree's output, else its base) is on the old
  module is recoloured with colour 6 (white).
* `CModuleFactory::CreateModuleFromType(newType)`: defaults for parameters, modes, lock
  flag, uprate and custom data (**parameter labels are not carried over**).
  `SetQuantPos(old col,row)`, copies `CModule+0x34` (area) and the colour (`+0x3c`).
* Index: `CPatchData::GetUniqueID(1)` @ `000e4ff6` on the patch that still holds the old
  module: lowest index ≥ 1 not in use, so never the old index. The module is appended to
  the module list.
* Name @ `000ec0d6`: if the old name starts with the old type's name (builder+0x38 =
  PANL `Module.Name`, `shortName`), the new name is `newShortName + GetFreeNameIndex`
  (`000e6cf8`: for every module whose name starts with the prefix, `atoi` of the next 3
  characters; the result is the smallest positive number not among them), cut to 15
  characters (`substr(0, 0xf)`). Otherwise the old name is kept.
* Parameters: for each old param with a mapping, if `GetMaxParam` is equal on both sides
  the 10 variation values are copied; **no scaling**, a different range keeps the default
  (e.g. OscA Wave 0..5 → OscB Wave 0..4).
* Cables, walking every segment `(child, parent)` of the original tree:
  * child on the old module (an input): if it maps, connect `new input ← parent`
    (parent mapped if it is on the old module; if that fails, the tree's chain ID is used,
    mapped if needed; nothing if it is the parent itself). If the input does not map,
    every link leaving that input towards another module is reconnected to the input's
    parent (the chain is bridged), with the segment's colour.
  * parent on the old module, child elsewhere: if the parent maps, connect it to the child.
    For an output, the cable takes the new connector's colour (`CPanel::GetHole` kind
    2 → blue, 3 → orange, 0 → yellow, else red; the new module is not uprated yet) and the
    child's whole subtree is recoloured (`CMCableRecolor`). Unmapped: dropped.
* `GetMakeRoomForMolecules` pushes overlapped modules down, `GenerateBandwidthChangeMolecules`
  recomputes uprates (and recolours).
* `CMorphMap::GetReplaceModuleMolecules` @ `000f09aa`: every morph spec of the old module
  in every variation is re-created on the mapped parameter (same group and range);
  `CKnobMap` @ `000f1428` (same knob, assign type 0), `CPerfKnobMap` @ `000ff4c4` (global
  knobs of a performance), `CCtrlMap` @ `000f12de` (same CC). Assignments of unmapped
  parameters disappear with the delete molecules. `CMutaSynthData` is updated the same way.
* Modes are not mapped: the new module's modes are their defaults.

## 5. G2fresh

`g2::replace::replaceModule` follows section 4 on `Patch`'s cable list (each cable is a
tree segment `from` = parent → `to` = child). Known differences:

* An unmapped input whose parent is also on the old module: the original would issue a
  `CMCableConnect` to the deleted module (`CheckConnectorAndThrow` in `ConnectCable`); we
  connect to the parent's counterpart, or skip.
* Performance-wide (global) knob assignments are not touched (`Patch` does not hold them).
* Recolouring of nets fed by an old output is done for the whole tree under that output.

## Open questions

* Exact cable order the editor writes after a replace (we keep each new segment at the
  position of the segment it replaces).
* Whether the synth-side molecule order matters for patches sent live (not modelled).
