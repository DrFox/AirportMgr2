# Holding Positions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the player-placed "hold-short mark" with derived runway-holding positions at every taxiway end on a runway, a player-placed intermediate holding position, and painted markings for both.

**Architecture:** Model gets `EHoldingPositionKind` on the node and a mark struct for intermediate positions only; the guideline builder derives runway positions at mixed nodes beside the exit-arc bookkeeping; traffic reads the renamed field unchanged; the tool toggles intermediate positions; a new marking mesh builder paints both kinds through `M_RoadSurface` with `UV1 = 0`.

**Tech Stack:** UE 5.8.2 C++, Airside plugin, `Run-AirsideTests.ps1`.

**Spec:** `docs/superpowers/specs/2026-09-07-holding-positions-design.md`

## Global Constraints

- `Model/` never includes Build/Tool/Present; `Tool/` never Present/. `Check-Architecture.ps1` enforces.
- `UE_LOG` count survives every rename; log text may change, count may not.
- Registry label, `UI_COMMAND` label and tool display name are ONE string.
- Header changes need the editor closed: batch T1 + T2's header edits into one build.
- Commits: concise, no Co-Authored-By trailer.

---

### Task 1: Mechanical rename (one commit, no behaviour change)

**Files:** every file `grep -rl "HoldShort\|Hold short\|hold short\|hold-short"` finds under `Plugins/Airside/Source`, `Source`, `Tools`, `docs` (spec §8 map). `git mv` `Tool/HoldShortTool.{h,cpp}` → `Tool/HoldingPointTool.{h,cpp}`; `AirsideTests/Private/HoldShortMarkTest.cpp` → `HoldingPositionMarkTest.cpp`; `HoldShortToolTest.cpp` → `HoldingPointToolTest.cpp`.

- [ ] Script (Python, word-boundary): `FHoldShortMark`→`FHoldingPositionMark`, `HoldShortMarks`→`HoldingPositionMarks`, `PruneHoldShortMarks`→`PruneHoldingPositionMarks`, `HoldShortFor`→`HoldingPositionFor`, `SetHoldShort`→`SetIntermediateHoldingPosition` (except the actor's UFUNCTION forwarder, added by hand), `FHoldShortTool`→`FHoldingPointTool`, `HoldShortTool.h`→`HoldingPointTool.h`, `EPreviewStyle::HoldShort`→`EPreviewStyle::RunwayHoldingPosition`, `HoldShortColour`→`RunwayHoldingPositionColour`, `PlaceHoldShort`→`PlaceHoldingPoint`, `"Hold short"`→`"Holding point"`, test names `HoldShort`→`HoldingPosition`. Then `grep -rn "HoldShort\|hold short" ...` must return only the actor's forwarder and dated comments that quote the old name.
- [ ] Actor: add `UFUNCTION(BlueprintCallable) bool SetHoldShort(int32 NodeIndex, int32 SegmentIndex)` forwarding to `SetIntermediateHoldingPosition(NodeIndex)` with a comment: deprecated name kept for Blueprint.
- [ ] Build (editor closed), full suite: same count as before (127), 0 failed. `UE_LOG` count unchanged.
- [ ] Commit: `refactor(airside): hold-short is holding position; the tool is Holding point`.

### Task 2: Kind on the node, intermediate-only marks, derived runway positions

**Files:** `Public/Model/RoadGuideline.h`, `Public/Model/RoadNetwork.h`, `Private/Model/RoadNetwork.cpp`, `Private/Build/RoadGuidelineBuilder.cpp`, `Public/Model/GroundTraffic.h` (test setter), tests `RunwayExitArcTest.cpp` (new test 1), `HoldingPositionMarkTest.cpp` (test 2), M2 traffic tests (setter).

- [ ] `EHoldingPositionKind { None, Runway, Intermediate }` in `RoadGuideline.h`; `FGuidelineNode::HoldingPosition`; `FHoldingPositionMark` loses `Protects`.
- [ ] `URoadNetwork::SetIntermediateHoldingPosition(FGuidelineNodeId, bool bSet)`: refuses `Runway`, dead, non-aircraft nodes; sets/clears `Intermediate`; records/removes the mark by `Origin` when set. `SetRunwayHoldingPositionForTest(FGuidelineNodeId, FRoadSegmentId)` for hand-built traffic fixtures (Model/, no world).
- [ ] Builder: after the turn-path loop, for each `SetBack` key not in `ContinuousEnds`: node = `Ends[EndKey(seg, end, 0)]`; `HoldingPosition = Runway`, `HoldingPositionFor` = first continuous arm's segment at that node (collect per node in the pre-pass: `TMap<int32, FRoadSegmentId> RunwayAtNode`). The clear/re-apply pass: clear `Runway` flags on every derived node before deriving (they are re-derived); clear/re-apply `Intermediate` exactly as hold-short today, from the marks.
- [ ] Red test 1 (`RunwayHoldingPositionsAreDerived`) on the exit-arc fixture incl. `ExitLength = 0`; test 2 rename+intermediate; traffic tests use the test setter. `TrafficHeadOnReplanTest`: drop the four `SetHoldShort` calls, assert the four nodes are `Runway` after the build.
- [ ] Build, suite green. Commit: `feat(airside): runway holding positions are derived; the player places intermediate ones`.

### Task 3: Tool, overlay, HUD

**Files:** `Private/Tool/HoldingPointTool.cpp`, `Private/Tool/GuidelineOverlay.cpp`, `Public/Tool/RoadBuildTool.h` (style), `Source/AirportMgr/RoadBuildHUD.{h,cpp}`, `AirsideEditor/Private/RoadBuildEditorTool.cpp`, `RoadBuildEdModeCommands.cpp` (tooltip), `HoldingPointToolTest.cpp`.

- [ ] Tool: pick any aircraft node; if `Runway` → refuse "Runway holding positions are derived from the runway" (logged, `LogAirside`); else toggle via `SetIntermediateHoldingPosition`. Preview: Doomed on a set intermediate node, Refused marker on a runway one, Snap otherwise.
- [ ] `EPreviewStyle::IntermediateHoldingPosition`; overlay emits per kind; HUD and editor tool map both colours (intermediate at half alpha).
- [ ] Test 5 red→green. Build (header: style enum + HUD), suite. Commit: `feat(airside): Holding point tool places intermediate holding positions`.

### Task 4: Surface marking

**Files:** create `Public/Build/HoldingPositionMarkingBuilder.h`, `Private/Build/HoldingPositionMarkingBuilder.cpp`; modify `Present/RoadSurfacePresenter.{h,cpp}` (fourth component, rebuilt with the surface), `Present/RoadNetworkActor.cpp` (create the component, same material as the road), test `HoldingPositionMarkingTest.cpp`.

- [ ] Builder: `static void Build(const URoadNetwork&, FRoadMeshBuffers& Out, double SurfaceZ)`. Per flagged node: tangent = the node's own segment (Origin) tangent, fallback Incident[0]; width = that segment's profile total width; quads per spec §6; `UV1 = (0,0)`, `UV2 = (0,0)`; normals up (winding per `unreal-triangle-winding-convention`).
- [ ] Presenter: `MarkingComponent`, rebuilt in `Rebuild` after the road mesh, road material.
- [ ] Test 4 red→green (counts, plane, width, orientation, UV1 zero). Build, suite. Commit: `feat(airside): painted holding-position markings`.

### Task 5: Docs, suite, PR

- [ ] Amend the two older specs with dated notes (done in the branch's first commit alongside the spec). Update `airportmgr-roadnet-state` memory. PR body: build line, test line, `UE_LOG` delta, rename map. `gh pr create`.
