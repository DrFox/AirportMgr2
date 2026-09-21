# Editing placed geometry: an Edit mode, snapped like placement

Date: 2026-09-20
Branch: `feature/node-editing`
Status: design, approved in brainstorming; not yet planned

## 1. The report

Three things, from play:

1. **No way to merge close points.** Two nodes a few metres apart cannot be joined.
2. **No definitive edit mode.** Node dragging exists but is undiscoverable, and worse, is
   reachable by accident.
3. **Editing does not snap like placing.** The snap guides landed in #162 and are good; a
   drag gets none of them.

The first has a measured consequence, and it is the one that motivates this work:
**vehicles slow or stop unnecessarily at close point pairs.** A pair of nodes a few metres
apart makes a tight corner; `FSpeedProfile` is the drivability authority and derives a low
speed across it; the agent crawls. There was no way to fix the layout that caused it.

That is the acceptance criterion, and section 9 states it as a test over a whole route
rather than per edge - see `airside-speedprofile-is-the-drivability-authority`, which
records four attempts that shipped green by re-implementing the rule one edge at a time.

## 2. What is already true

Read before designing, so the design is against the code and not against memory of it.

| Fact | Where |
|---|---|
| Node dragging already exists, on the Taxiway and Road entries only | `RoadDrawTool.cpp:541-580` |
| It passes the RAW cursor - not `Snap.Position`, not `GuidedCursor()` | `RoadDrawTool.cpp:567` |
| The guide anchor describes a chain or a free start, never a drag | `RoadDrawTool.cpp:256` |
| `FRoadSnapChain::Resolve` has no exclusion input | `RoadSnap.h:186` |
| Shift, Ctrl and Alt are all spoken for: insert, remove, suspend guides | `RoadBuildTool.h` |
| No merge exists anywhere in either module | `grep -rn Merge` |
| A node carries no kind; kind lives on the segment's `Profile` | `RoadNode.h:35` |
| Apron corners and runway ends are not draggable at all | only `FRoadDrawTool` and `FStandPlaceTool` implement `OnDragBegin` |
| The runway designator is DERIVED from direction, not stored | `RunwayDesignator::ToPairText` |
| `Q`/`E` is camera turn, polled every frame | `RoadBuildController.cpp:110` |

Two of these are defects in their own right:

- **The misclick.** Press-and-travel over any node, under the Taxiway or Road tool,
  reshapes the road. Mid-chain, a slightly-moved click on a junction moves it instead of
  continuing from it, and there is no way to decline. This is what "editing must be a
  deliberate action" was ruled against.
- **The drag cannot snap even if asked.** The snap chain resolves against every live node
  including the one under the cursor, which is the node being dragged. Without an
  exclusion the node rule claims the dragged node itself and it can never move.

## 3. Rulings from brainstorming

| Question | Ruling | Why |
|---|---|---|
| Held modifier, dedicated tool, or mode toggle? | **Mode toggle** | Editing must be deliberate. A held key is not; and Shift/Ctrl/Alt are taken anyway. |
| What does a click do with Edit lit? | **Nothing builds** | Symmetric with the misclick argument: you cannot accidentally build while editing, as you cannot accidentally edit while building. |
| Which tools get an Edit meaning? | **Taxiway, Road, Runway, Apron** | The four named in the report. Each handle is a point on the road plane, so one handle type, one drag path, one snap treatment. |
| Merge gesture? | **Drop a node on a node** | Genre convention: CS1 has no merge verb - drawing within the snap radius reuses the node. `ERoadSnapKind::Node` already says so: *"Clicking reuses it, which is how a junction is closed."* |
| Duplicate arms after a merge? | **Keep the wider, discard the narrower** | Matches `airport-geometry-is-infrastructure-not-per-aircraft`: ground geometry is sized for the largest aircraft admitted, so collapsing a stub must never silently narrow a cleared route. |
| Bulk "weld everything closer than X"? | **No** | One pair at a time is enough. YAGNI. |
| Does Edit survive re-entering the session? | **Resets to Build** | |
| Toggle key | **`M`** | `E` is camera turn. `M` is free, is Move It's key, and is mnemonic for move and merge. |

### On Cities: Skylines

CS1 vanilla has **no node editing at all**. Two things stand in for it: merge is implicit
in the draw snap, and "Upgrade" is a persistent mode button inside the road panel rather
than a held key. Move It - the mod everyone installs because of that gap - went the other
way, a dedicated tool on `M`. This design takes CS1's mode-button shape, Move It's key,
and CS1's implicit merge.

## 4. Where the mode lives

`FBuildSession` gains one enum beside `Selection`, and one tool:

```cpp
enum class EGestureMode : uint8 { Build, Edit };
```

`FBuildSession::GetActiveTool()` returns `&EditTool` when `Mode == Edit`, and the indexed
build tool otherwise.

**That is what makes "Edit suppresses the tool" structural.** Both drivers already route
every input through `GetActiveTool()`, so PIE and `URoadBuildEdMode` get this from one
change and cannot diverge - the failure `FRoadSnapSettings` records from before the two
were merged. The alternative, a `bEditMode` on `FToolContext` that nine tools each branch
on, is nine places that must agree and eight tools that must remember to refuse.

**An enum, not a bool** (CLAUDE.md: a phase is an enum, never a set of bools). Two values
today; it leaves room for CS1's actual third mode - Upgrade, repaint an existing road to
the current width - without a second flag that could be true at the same time.

**Switching to Edit deactivates the build tool** through the existing
`IBuildTool::OnDeactivate`, so a part-drawn chain is abandoned rather than left to
reappear. Switching back re-enters at idle. `Mode` resets to `Build` when the session is
constructed.

## 5. Which handles are live - one list, not two

`FEditTool` must know what the lit tool exposes. That mapping has to agree with
`ToolRegistry()`, so it **goes in** `ToolRegistry()` rather than beside it - CLAUDE.md,
"lists that must agree are ONE list", and "check where a list is CONSUMED".

```cpp
enum class EEditHandleKind : uint8
{
    None,             // Edit greys out for this tool
    AirsideNode,      // a node with an incident taxiway-or-runway segment
    ServiceRoadNode,  // a node with an incident service-road segment
    RunwayThreshold,  // the END node of a runway chain, not its interior nodes
    ApronCorner,      // a corner of an apron outline
};
```

New field on `FToolRegistration`:

```cpp
EEditHandleKind EditHandles = EEditHandleKind::None;
```

| Entry | Key | EditHandles |
|---|---|---|
| Select | 4 | `None` |
| Taxiway | 1 | `AirsideNode` |
| Apron | 2 | `ApronCorner` |
| Stand | 3 | `None` |
| Guideline | 5 | `None` |
| Runway | 6 | `RunwayThreshold` |
| Holding point | 8 | `None` |
| Road | 9 | `ServiceRoadNode` |
| Fuel depot | 0 | `None` |

`None` is what greys the toggle out, and the bar says why.

**A node carries no kind** (`RoadNode.h`), so `AirsideNode` and `ServiceRoadNode` are
questions about a node's *arms*. A node where a service road meets a taxiway is grabbable
under both - correctly, since it is both. `RoadNaming::ReferenceOf` already classifies a
segment into Runway / ServiceRoad / Taxiway and is the one home for that question; the
handle filter calls it rather than asking the profile itself. That header records what
happened the last time four call sites each answered it privately.

## 6. Snapping parity

Three gaps between a drag and a click. All three must close or the feature misses its point.

### 6.1 The drag must use the snapped, guided position

`RoadDrawTool.cpp:567` passes `Context.Cursor`. `FEditTool::OnDrag` passes
`Context.GuidedCursor()`, and honours `Context.Snap` for the drop. This is the whole of
"the same snapping experience we have when placing" for positioning.

### 6.2 The snap chain needs an exclusion

Dragging node A, the cursor is on A, so `FRoadNodeSnapRule` claims A and the node never
moves. `Resolve` currently takes only the network, a cursor and the settings.

```cpp
struct FRoadSnapQuery
{
    FVector2D    Cursor;
    FRoadNodeId  ExcludeNode;   // unset for every existing caller
};
```

`IRoadSnapRule::Resolve` and `FRoadSnapChain::Resolve` take the query. The existing
three-argument `Resolve` stays as a forwarder building `{Cursor}`, so no current caller
changes - CLAUDE.md's rule that every reachable entry point stays reachable at its old name.

A struct rather than a fourth parameter, per "one struct per thing": the cursor and what
it may not claim are one question, and the next exclusion (an apron corner may not snap
to its own neighbours) adds a field rather than a parameter to eleven signatures.

### 6.3 The drag needs a guide anchor

`FRoadDrawTool::DescribeGuideAnchor` describes a chain or a free start. A drag is neither.

The drag's anchor is **free-start-shaped**: the moving thing is the node itself, so there
is no fixed origin, `Origin` is the cursor, and the arbiter drops every angular candidate
measured from a point to itself - exactly the mechanism `FGuideAnchor::bFreeStart`
documents. Angular references come from the dragged node's own arms:

- exactly one incident segment: that arm's direction is the reference, named "this road"
- two or more: no angular reference

which is the same rule and the same reason `FRoadDrawTool::DescribeGuideAnchor:300` gives
for a junction - no arm is "the" one, and picking whichever is stored first would make the
guide change with an edit nobody connected to guides at all.

`HalfWidthLeft`/`HalfWidthRight` come from the widest incident segment's profile, through
`IRoadEditTarget::ResolveProfileFor` - the one resolver, so a guide cannot disagree with
the pavement it is guiding.

**The candidate-node loop is then written twice** (`RoadDrawTool.cpp:336-355` and here) and
the two must agree. It is extracted to a shared helper - `RoadGuideAnchor::AddNodeCandidates(
Network, Origin, ExcludeIndex, Out)` - and both tools call it. Both already need the same
exclusion: the chain skips the node it extends from, the drag skips the node it moves, for
the identical reason that a node's own lines pass through the origin.

**Saying yes is half the work.** `IBuildTool::WantsFreeStartGuides` records that a tool
which describes an anchor and never draws it shipped on 2026-09-20 and showed the player
nothing. `FEditTool::BuildPreview` draws the dashed line, and section 9's test measures the
drawing, not the describing.

## 7. The mutations

### 7.1 `MergeNodes`

`URoadNetwork::MergeNodes(FRoadNodeId Keep, FRoadNodeId Absorb)`, surfaced on
`IRoadEditTarget` as `bool MergeNodes(int32 KeepIndex, int32 AbsorbIndex)`.

Judged whole before anything mutates - a move-then-check would need an undo the drag never
asked for, which is the argument `URoadEditFacade::MoveNode:800` already makes for
`NodeCornersFit`:

```
for each arm of Absorb:
    other end is Keep         -> collapse: delete the arm
    Keep already reaches it   -> keep the wider profile, delete the other
    otherwise                 -> repoint the Absorb end to Keep
delete Absorb
re-sort Keep.Incident by outgoing bearing        (FRoadNode::Incident's invariant)

refuse the whole merge if any resulting corner fails RoadPlacement::NodeCornersFit
                       or any resulting arm is under MinSegmentLength
```

**Widest wins** where both nodes already reach the same third node - widest by
`URoadProfile::GetTotalWidth()`, the same measure the width cycle reports. On an exact tie
the arm already incident to `Keep` survives, because that is the edit that touches less.
Deterministic, one line in the log, and it never narrows a route an aircraft was cleared for.

**Control points.** A segment is a quadratic Bezier with `Control`, equal to `(A+B)/2` when
straight. Repointing an endpoint by `d` translates `Control` by `d/2`, which keeps a
straight segment exactly straight and approximately preserves an authored curve.
Recomputing `Control` as the new midpoint instead would silently flatten every curved arm
into the merged node.

**Cost needs nothing new.** The merge lands inside the drag's
`BeginInteractiveEdit`/`EndInteractiveEdit`, and that pair already prices total pavement
before against after and credits the shortfall at scrap (`RoadEditFacade.cpp:746`). The
discarded arm is a disposal like any other.

**Two precedents to read before writing this, not to assume:**

- what `URoadNetwork::SetNodePosition` already does to `Control` - merge must match it
- how `DeleteNode` treats manual `ConnectGuidelines` edges whose guideline nodes derive
  from the absorbed road node. Merge follows that precedent; it does not invent one.

### 7.2 Runway thresholds: one clause on `MoveNode`, not a second mutator

A runway threshold **is** a road node, so `MoveNode` already moves it and already checks
`MinSegmentLength` and `NodeCornersFit`. It gains one clause: a node on a runway chain may
not pull the strip under `MinimumRunwayLength`.

One mutator, one rule set. A `MoveRunwayThreshold` beside `MoveNode` would be a second
answer to "may this node move", and the two would drift.

**The designator needs no work.** `RunwayDesignator::ToPairText` takes a direction and
stores nothing, and `RoadNaming.h:47` was written for this exact case: *"the same strip must
not become '27/09' because a node was dragged."* Dragging a threshold re-derives the
designator, low end first.

*Open, to verify in PIE rather than assert:* whether the designator **painted on the
surface** rebuilds with it. `MoveNode` broadcasts `OnChanged` and the actor rebuilds the
mesh, so it should; that is a prediction, not a measurement.

### 7.3 `MoveApronCorner`

`bool MoveApronCorner(int32 ApronIndex, int32 CornerIndex, FVector2D To)` - genuinely new,
with one rule of its own: refuse a move that makes the outline self-intersecting. The
winding must survive it too (`unreal-triangle-winding-convention`: CCW faces DOWN here), so
the test asserts on an engine-computed normal, not on a 2D signed area.

## 8. Feedback and the bar

- New `EPreviewStyle::Handle` - "a point this mode can grab". **Appended at the end** of the
  UENUM, per that header's own warning that renumbering repoints anything serialised
  against it.
- Drop target: the existing `Snap` on the node that would absorb, with a `Label` saying so.
- An arm that would be discarded: `Doomed`. An illegal drop: `Refused`, with the reason.
- The guide's dashed line: the existing `Guide`.
- `BuildReadout`: what the merge costs or credits.
- `BuildBarWidget` gains an **Edit** toggle on `M`, drawn apart from the numbered tool row
  because it is a different axis. Greyed with a reason when the lit tool's `EditHandles` is
  `None`. Per `ui-bottom-bar-cities-skylines-style`, the toggle lives on the bar; the
  merge's own confirmation reads at the cursor.

### What comes out

`FRoadDrawTool` loses `OnDragBegin`, `OnDrag`, `OnDragEnd`, `DragNode`, the `DragNode`
clause in `Tick`, and the Taxiway tooltip's "drag a node to move it". That deletion **is**
the misclick fix.

Refactor contract: the `UE_LOG` lines and the WHY comments move to `FEditTool`. They do not
evaporate. Count `UE_LOG(` before and after.

## 9. Tests

One per seam that would otherwise be silently unwired, at the level of the composition.

**The acceptance test, and the reason for the feature:**

- `Airside.Model.MergingClosePointsRemovesTheSpuriousSlowdown` - lay a route with two
  nodes a few metres apart, run `FSpeedProfile` **over the whole route**, merge, run it
  again, assert the minimum speed rose. Over the whole route and not per edge: see
  `airside-speedprofile-is-the-drivability-authority`.

**The mode:**

- `Airside.Tool.EditModeSuppressesTheBuildTool` - Edit lit, click empty ground, node count
  unchanged.
- `Airside.Tool.RoadDrawToolNoLongerDragsNodes` - pins the misclick fix. Delete the rule and
  it must go red (`a-green-test-may-measure-nothing`).
- `Airside.Tool.EditHandlesAreConsumedForEveryRegistryEntry` - names, not counts, and logs
  both on mismatch.
- `Airside.Tool.EditModeResetsToBuildOnConstruction`

**The snapping:**

- `Airside.Tool.EditModeDragSnapsExactlyToANode` - the dragged node's position is bitwise
  equal to the target's stored position, not within a tolerance.
- `Airside.Tool.SnapChainExcludesTheDraggedNode` - without the exclusion the node cannot
  move at all; this measures that it does.
- `Airside.Tool.EditModeDragOffersGuides` - measures the DRAWING, per
  `WantsFreeStartGuides`.

**The merge:**

- `Airside.Model.MergeCollapsesASharedSegment`
- `Airside.Model.MergeKeepsTheWiderDuplicateArm`
- `Airside.Model.MergeRefusesWhenCornersDoNotFit` - and the graph is unchanged afterwards.
- `Airside.Model.MergeSortsIncidentByBearing` - the `FRoadNode::Incident` invariant.
- `Airside.Model.MergeKeepsAStraightSegmentStraight` - the `Control` rule.
- `Airside.Model.MergeIsOneUndoStep`

**The new geometry:**

- `Airside.Model.MoveApronCornerRefusesSelfIntersection`
- `Airside.Model.MoveRunwayThresholdRefusesUnderMinimumLength`
- `Airside.Model.DraggingAThresholdRedesignatesTheRunway`

Test names are distinct leaves - `unreal-automation-test-tree-drops-bare-parent`.

## 10. Verification

Tests are necessary and not sufficient. `graph-changes-need-a-look-at-the-map` records a
graph change that passed 348 tests and put kilometre-wide arcs across the apron. Before
claiming this works:

1. `./Tools/Run-AirsideTests.ps1 -Project C:\repos\airportmgr2-editing\AirportMgr.uproject` -
   read the `N run, N failed, N crashed` line, never the exit code.
2. PIE: press `M` with Taxiway lit, drag a node, confirm the dashed guide appears and the
   node lands on the guide.
3. PIE: drag one node onto another; confirm the merge in `Saved/Logs/AirportMgr.log` and
   that the pavement has no seam at the merged junction.
4. `python Tools/Mcp.py shot out.png` of the merged junction. The surface model welds
   bitwise; a merge that breaks it shows as a crack.
5. Drag a runway threshold; confirm the designator on the surface.

## 11. Out of scope

- Bulk weld by radius. Ruled out; one pair at a time.
- Stand and depot poses, and guideline nodes. A pose is a point AND a heading, so the
  gesture set stops being uniform; revisit once the point case is judged in PIE.
- Upgrade mode (repaint an existing road to the current width). `EGestureMode` leaves room
  for it; nothing here builds it.
- Multi-select and box-select.
