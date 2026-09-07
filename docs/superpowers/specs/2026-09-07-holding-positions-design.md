# Holding positions — design

Date: 2026-09-07. Status: approved in conversation, binding for the plan.
Branch: `feature/holding-positions`. Follows the runway exit arcs (PR #55).
Amends: `2026-08-29-ground-movement-model-design.md` §5.5 and
`2026-09-06-ground-traffic-design.md` (hold-short authoring row, §5.5 references).

## 1. The correction

M2 shipped a "hold-short mark": a player-placed bar at a taxiway end, keyed by
`FGuidelineEndRef`, that both marks a place and implies a permanent rule. Real airports
separate the two:

- A **holding position** is a place, a painted marking across a taxiway. There are three
  kinds. A **runway-holding position** exists at EVERY taxiway that meets a runway: two
  solid and two dashed yellow lines, solid side toward the taxiway, at a fixed distance
  from the runway centreline (30 to 90 m by runway code). An **intermediate holding
  position** is a single dashed line at a taxiway-to-taxiway junction where ATC may hold
  traffic. An **ILS critical-area** position sits further back on some runway entries
  (out of scope until there is an ILS).
- **"Hold short"** is an instruction. ATC says "hold short of 27"; "cross" and "line up"
  release it. The marking exists whether or not anyone is told to hold at it.

The M2 decision "hold-short is player-placed, never derived" was made about the marking
while thinking about the instruction. The marking is infrastructure; the instruction is
M3's sequencer. Both older specs are amended by dated notes pointing here.

## 2. The rule

1. **Every taxiway end at a runway carries a runway-holding position, derived by the
   builder.** It sits on the taxiway's end node - since the exit arcs that node is
   `ExitLength` down the taxiway, 60 m from the centreline, a code 3 distance - and names
   the runway segment it protects. The player neither places nor removes it.
2. **The player places intermediate holding positions**, on a taxiway node that is NOT a
   runway end, with the same tool and key (8, "Holding point"). They protect nothing and,
   until M3 issues instructions, hold nothing: the tooltip says so.
3. **No node means "hold" by itself.** The runway rule in traffic (claim the strip's chain
   at a runway-holding position; arm the crossing when committed past one) is unchanged in
   behaviour and reads the same field under its new name. "Hold short" as an instruction
   arrives with M3's sequencer and is written against holding positions.
4. **The surface shows the marking.** The runway pattern (two solid, two dashed) across the
   taxiway at a runway-holding position; a single dashed line at an intermediate one.

## 3. Model

```cpp
enum class EHoldingPositionKind : uint8 { None, Runway, Intermediate };

struct FGuidelineNode {
    // replaces HoldShortFor. One enum, not two flags: Runway iff Protects is set.
    EHoldingPositionKind HoldingPosition = None;
    FRoadSegmentId HoldingPositionFor;     // the runway protected; set iff Runway
};

// replaces FHoldShortMark. INTERMEDIATE positions only: runway ones are derived on every
// build and need no source of truth beyond the graph.
struct FHoldingPositionMark { FGuidelineEndRef At; };
```

`URoadNetwork`:
- `bool SetIntermediateHoldingPosition(FGuidelineNodeId Node, bool bSet)` - replaces
  `SetHoldShort`. Refuses a node whose `HoldingPosition == Runway` (derived, not the
  player's), a node no aircraft edge admits, and a dead slot. Records/removes the mark by
  the node's `Origin` when it has one (so it survives rebuilds), by the flag alone when it
  has none (pose and anchor nodes, as today).
- `GetHoldingPositionMarks()`, `PruneHoldingPositionMarks()` - renamed.
- `RunwayNearGuidelineNode` stays (the builder uses it to name the protected segment; the
  tool no longer needs it).

`FRoadGuidelineBuilder::Build`, after the turn paths: for every non-continuous arm end at
a mixed node (the same `SetBack` / `ContinuousEnds` bookkeeping the arcs use), set the end
node to `Runway`, protecting one of that node's continuous arms (any; `RunwayChain` expands
it). With `ExitLength = 0` the end sits at its cut line and still gets the position, so the
head-on replay keeps its geometry AND its bars without calling anything. The re-apply pass
then rewrites intermediate marks exactly as it rewrites hold-short marks today, with the
same "unsolved end leaves the mark" rule.

## 4. Traffic

`GroundTrafficClaims.cpp` reads `HoldingPositionFor` where it read `HoldShortFor`; the two
sites (the bar's chain claim at `~618`, the committed-past-a-bar arming at `~265`) are
unchanged in logic. An intermediate position (`Kind == Intermediate`, `For` unset) is
inert: the chain loop finds nothing to claim and the arming test finds no strip. This is
deliberate and documented at both sites: instructions are M3's.

## 5. Tool, overlay, HUD

- `FHoldShortTool` → `FHoldingPointTool`, display name "Holding point", key 8, registry and
  `UI_COMMAND` labels renamed together (the consumer checks names, not counts).
  Click on a plain taxiway node: toggles an intermediate position. Click on a
  runway-holding position: refused, "Runway holding positions are derived from the
  runway", logged.
- `EPreviewStyle::HoldShort` → `RunwayHoldingPosition`, plus `IntermediateHoldingPosition`.
  `GuidelineOverlay` draws the runway kind as it draws the bar today (across the taxiway,
  square to the node's own segment) and the intermediate kind the same way in the second
  style. `ARoadBuildHUD` maps both to colours (runway: the current bar colour;
  intermediate: the same at half alpha).

## 6. Surface marking

`FHoldingPositionMarkingBuilder` (Build/) emits, per holding position, quads in the road
plane across the taxiway's full width at the node, oriented by the node's own segment
tangent: runway kind = two solid bars (0.3 m each, 0.3 m apart) on the taxiway side and two
dashed bars (dashes 0.9 m, gaps 0.9 m) on the runway side; intermediate kind = one dashed
bar. Vertices carry `UV1 = (0, 0)` and go to a fourth `UDynamicMeshComponent` on the
presenter (`MarkingComponent`) using `M_RoadSurface`: with lateral offset 0 everywhere the
material's centreline mask is 1 across the whole quad and paints it `MarkingColor` - the
"solid yellow slab" that `ProfileFallback` §4 documents as a bug is exactly the paint a
marking wants, and it costs no new material asset (authoring one needs the editor closed).
Z is `SurfaceZ + 0.5` so it draws over the pavement like the apron offset does.

The overlay bar stays: it is the design-time symbol; the mesh is the paint.

## 7. Tests (measured)

1. `Airside.Build.RunwayHoldingPositionsAreDerived` - the exit-arc fixture: every taxiway
   end at the runway is `Runway` and protects a segment of the strip; the runway's own
   nodes and the split nodes are `None`; a rebuild leaves them identical; `ExitLength = 0`
   still derives them (at the cut line).
2. `Airside.Model.IntermediateHoldingPositionByThePlayer` - set on a taxiway-taxiway node,
   survives a rebuild by identity (the `HoldShortSurvivesRebuild` test, renamed and pointed
   at an intermediate node); refused on a runway-holding position; cleared on a second
   call.
3. The M2 traffic tests that hand-flag nodes call the new API with `Runway` semantics via
   a test-only setter (`SetRunwayHoldingPositionForTest`), because their guidelines are
   hand-built with no runway junction to derive from. Their assertions do not change.
4. `Airside.Build.HoldingPositionMarking` - the marking builder emits the right number of
   quads per kind, in the road plane, across the taxiway width, square to the taxiway, at
   `UV1 == 0`.
5. `Airside.Tool.HoldingPointTool` - the renamed tool test: places/clears an intermediate
   position; refuses a runway one with the message.

## 8. Rename map (mechanical, one commit, log lines and test names carried)

| Old | New |
|---|---|
| `FHoldShortMark`, `HoldShortMarks`, `GetHoldShortMarks`, `PruneHoldShortMarks` | `FHoldingPositionMark`, `HoldingPositionMarks`, `GetHoldingPositionMarks`, `PruneHoldingPositionMarks` |
| `FGuidelineNode::HoldShortFor` | `HoldingPositionFor` (+ `HoldingPosition` kind) |
| `SetHoldShort` (network, facade, actor, `IRoadEditTarget`) | `SetIntermediateHoldingPosition` (actor keeps a `SetHoldShort` UFUNCTION forwarder for Blueprint, marked deprecated in its comment) |
| `FHoldShortTool`, `HoldShortTool.h/.cpp` | `FHoldingPointTool`, `HoldingPointTool.h/.cpp` |
| `EPreviewStyle::HoldShort`, `HoldShortColour` | `RunwayHoldingPosition` / `IntermediateHoldingPosition`, `RunwayHoldingPositionColour` / `IntermediateHoldingPositionColour` |
| key label "Hold short" | "Holding point" |
| tests `HoldShort*` | `HoldingPosition*` / `HoldingPointTool*` |
| log text "Hold short refused ..." | "Holding point refused ..." (same category, same count) |

## 9. Out of scope

ILS critical-area positions; enhanced centreline dashes; the sequencer and its
instructions (M3); mandatory red signs; any change to occupancy semantics.

## 10. Outcome (2026-09-07, same day)

Four commits on `feature/holding-positions`: the rename (no behaviour change, 127 tests),
derived runway positions + intermediate tool (127), painted markings (128). Measured:

- Every taxiway end at a runway is `Runway` after a build, arcs on or off; the runway's
  own nodes and the split nodes are `None`; a rebuild reproduces them; the player's set and
  clear are refused there and record no mark.
- An intermediate position at a taxiway junction survives two rebuilds by identity, clears
  and stays clear, lives on an Origin-less node without a mark, is pruned with its segment,
  and is out-ranked by the derivation when its end becomes a runway end.
- The runway pattern: 210 uu deep on the junction side of the node starting AT the node,
  the taxiway's full width, in the road plane, UV1 = 0, every engine-computed vertex normal
  up. The first cut had all 112 normals down - the quad helper now measures its winding.
- The head-on deadlock replay no longer places bars: the four it relied on are derived.
- `UE_LOG` 98 -> 99. Unverified in PIE at the time of writing.
