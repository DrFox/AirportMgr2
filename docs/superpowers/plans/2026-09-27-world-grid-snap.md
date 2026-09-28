# World Grid Snap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** World-aligned 1/5/10 m grid every guided builder snaps to, a faint local overlay, and
stands that line up with their neighbours' back edges.

**Architecture:** Pure maths in `Solve/GridSnap`; applied LAST in `FSnapGuideChain::Resolve`
(road snap > guides > grid); the step rides `FSnapGuideSettings` -> tunables -> `FToolContext`.
Plot tools apply it to their own anchor/frontage steps; the stand tool gains a guide anchor for
depth. Overlay is a plugin emitter both drivers call before the tool preview.

**Tech Stack:** UE 5.8 C++, automation tests (`Airside.*`), `Run-AirsideTests.ps1`.

**Spec:** `docs/superpowers/specs/2026-09-27-world-grid-snap-design.md`

## Global Constraints

- Worktree `C:\repos\airportmgr2-world-grid-snap`, branch `feature/world-grid-snap`. Build with
  `-NoHotReloadFromIDE`; tests with `-Project` pointing at the worktree .uproject.
- `Solve/` includes `CoreMinimal.h` only.
- Grid step figures 100 / 500 / 1000 uu and overlay radii 2000 / 6000 / 12000 uu live in ONE
  place (`FSnapGuideSettings`).
- Grid off = bitwise-identical behaviour to today.
- New `EPreviewStyle` values appended at the END of the UENUM.
- Round to nearest with `FMath::RoundToDouble`, never an int cast.
- Every new comment claiming a fact about other code carries `// ENFORCED BY:`.

## Review Focus

1. Negative coordinates (airport west/south of origin) - must round to nearest, not toward zero.
   Pinned in Task 1.
2. Alt-hold (suspend) with grid on - must free the point AND hide the overlay. Pinned in Task 2.
3. A 10 m crossing that falls past a segment end on the anchor click - must refuse the anchor,
   not place it off the road. Pinned in Task 3.
4. Grid on, tool that never consults it (Select, Guideline, Holding point) - no overlay.
   Pinned in Task 4.
5. Changing step mid-gesture - frame cache must not replay the old snap. Pinned in Task 2.

---

### Task 1: `Solve/GridSnap` maths

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Solve/GridSnap.h`
- Create: `Plugins/Airside/Source/Airside/Private/Solve/GridSnap.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/GridSnapTest.cpp`

**Interfaces:**
- Produces:
  - `FVector2D GridSnap::Quantise(const FVector2D& Point, double StepUu)` - StepUu <= 0 returns
    Point unchanged.
  - `bool GridSnap::NearestCrossingAlong(const FVector2D& Origin, const FVector2D& Direction,
    const FVector2D& Near, double StepUu, FVector2D& Out)` - point on line Origin+t*Dir, on an
    X=k*Step or Y=k*Step line, nearest to Near's projection. False (Out untouched) for Step<=0 or
    zero Direction.
  - `struct GridSnap::FPiece { FVector2D From, To; bool bMajor; }`
  - `void GridSnap::PiecesInDisc(const FVector2D& Centre, double RadiusUu, double StepUu,
    TArray<FPiece>& Out)` - every grid line crossing the disc, clipped to it, split into pieces
    at most one step long; bMajor when k % 5 == 0.

- [ ] Step 1: tests - Quantise (1/5/10 m; (-149,-151) at 100 -> (-100,-200); Step 0 bitwise
  equal); NearestCrossingAlong (X-axis line crosses only X=k lines; 45 deg line nearest of both
  families; zero dir false); PiecesInDisc (every piece end within Radius+eps, no piece longer than
  Step+eps, majors on multiples of 5*Step, a line exactly tangent contributes nothing, Step 0 ->
  empty).
- [ ] Step 2: build + run `Airside.Solve.GridSnap` - FAIL (not compiled: new file needs the
  second build, memory note).
- [ ] Step 3: implement. NearestCrossingAlong: t0 = dot(Near-Origin, D̂); for each axis a with
  |D̂a| > 1e-9, k = Round((Origin.a + t0*D̂a)/Step), t = (k*Step - Origin.a)/D̂a; keep t nearest t0.
  PiecesInDisc: for each family, k from Ceil((c-R)/S) to Floor((c+R)/S); half-chord
  h = sqrt(R²-d²) (skip h<=0); split [-h,h] into Ceil(2h/S) equal pieces.
- [ ] Step 4: build, run - PASS.
- [ ] Step 5: commit `feat(grid): GridSnap solve maths`.

### Task 2: Setting, tunables, context, chain

**Files:**
- Modify: `Public/Tool/SnapGuideSettings.h` (+ `.cpp`) - UENUM `EGridStep { Off, OneMetre,
  FiveMetres, TenMetres }`; `UPROPERTY EGridStep GridStep = EGridStep::Off`;
  `double GridStepUu() const`; `double GridOverlayRadiusUu() const`; `void CycleGridStep()`.
- Modify: `Public/Tool/BuildSession.h` - tunables `operator==` compares `GuideSources.GridStep`.
- Modify: `Public/Tool/RoadBuildTool.h` - `FToolContext::GridStepUu = 0.0`;
  `virtual bool IBuildTool::SnapsToGrid() const { return false; }`.
- Modify: `Private/Tool/BuildSession.cpp` `MakeContext` - set `Context.GridStepUu`.
- Modify: `Private/Tool/SnapGuideChain.cpp` `Resolve` - apply grid after Arbitrate.
- Test: `AirsideTests/Private/GridSnapChainTest.cpp`

**Interfaces:**
- Consumes: Task 1.
- Produces: `FToolContext::GridStepUu` (0 when grid does not apply this frame);
  `FSnapGuideSettings::CycleGridStep/GridStepUu/GridOverlayRadiusUu`; `IBuildTool::SnapsToGrid`.

Resolve, after Arbitrate:
```cpp
const double Step = Enabled.GridStepUu();
if (Step > 0.0)
{
	if (Result.Winners.Num() == 0)
	{
		Result.bActive = true;
		Result.Point = GridSnap::Quantise(Cursor, Step);
	}
	else if (Result.Winners.Num() == 1)
	{
		FVector2D OnGrid;
		if (GridSnap::NearestCrossingAlong(Result.Winners[0].Through, Result.Winners[0].Direction,
			Result.Point, Step, OnGrid))
		{
			Result.Point = OnGrid;
		}
	}
}
```
MakeContext: `bool bChainRan` set inside the existing guide branch;
`Context.GridStepUu = (!bSuspendGuides && Tool != nullptr && (bChainRan || Tool->SnapsToGrid()))
? Tunables.GuideSources.GridStepUu() : 0.0;`

- [ ] Step 1: tests via `FBuildSession::MakeContext` + Taxiway tool (FreeStartGuideTest's
  shape; own namespace): grid 5 m, cursor far from any guide -> GuidedCursor on (k*500, j*500);
  cursor near the taxiway extension (1 winner) -> point on the extension AND X a multiple of 500;
  grid off -> Guide identical to a run with GridStep unset; Alt (bSuspendGuides) -> GuidedCursor
  == raw and GridStepUu == 0; tunables differing only in GridStep compare unequal; Select tool ->
  GridStepUu 0.
- [ ] Step 2: run - FAIL.
- [ ] Step 3: implement as above.
- [ ] Step 4: build, run new + `Airside.Tool` - PASS.
- [ ] Step 5: commit `feat(grid): grid step applied last in the guide chain`.

### Task 3: Plots and stands

**Files:**
- Modify: `Public/Tool/PlotGesture.h` / `Private/Tool/PlotGesture.cpp` - `AnchorAt` and
  `DescribeAnchors` gain `double GridStepUu = 0.0`.
- Modify: `Private/Tool/StagedPlotTool.cpp` - pass `Context.GridStepUu` at all three call sites;
  `Public/Tool/StagedPlotTool.h` - `SnapsToGrid() const override { return Pinned < 2; }`.
- Modify: `StandPlotTool.h/.cpp` - `DescribeGuideAnchor` (Pinned == 2); frontage + depth rules;
  draw `Sink.Guides` in Describe while Pinned == 2.
- Modify: `PlotPlaceTool.cpp` - frontage on grid (same rule as stand: consistency of the shared
  gesture; spec named the stand, the depot shares AnchorAt).
- Test: extend `StandPlotToolTest.cpp`, `FuelDepotAnchorTest.cpp`.

**Interfaces:**
- Consumes: Task 1, `FToolContext::GridStepUu`, `IBuildTool::SnapsToGrid`.

Rules:
- AnchorAt, Step > 0: after the kerb offset, `NearestCrossingAlong(KerbStart, Along, Cursor,
  Step, Corner)` where KerbStart = RoadA + Inward*Kerb; refuse (return false) if the crossing's
  t is outside [0, Length].
- DescribeAnchors, Step > 0: one Pending marker at AnchorAt's corner, no per-segment dots.
- Frontage (Pinned == 1), Step > 0: Far = NearestCrossingAlong(Anchor, Along, Cursor). No
  MinFrontage clamp - the existing readout/refusal already judges a too-small stand.
- Stand DescribeGuideAnchor (Pinned == 2): Origin = Corners[1], ReferenceAt = Corners[0],
  Reference = Along, ReferenceName "the entrance", Point = EDragPoint::Boundary, AlignTo empty.
- Depth: if Guide has winners -> Raw from GuidedCursor (as now), quantised only when grid off;
  else if Step > 0 -> NearestCrossingAlong(Far, Inward, Cursor) then Raw = dot(that - Far,
  Inward); else today's 0.5 m step. Depth never negative (existing Max).

- [ ] Step 1: tests - (a) THE REPRO: two parallel taxiways, Y=0 (X -10000..0) and Y=-730
  (X 0..10000, different A-end phase AND kerb line), stand 1 north of the first drawn to depth
  D, stand 2 north of the second, third click at the depth that would put its back edge 150 uu
  short of stand 1's, via session MakeContext -> stand 2's back edge Y bitwise-equal to stand 1's
  (grid off and 5 m); (b) grid 5 m, no neighbour: back corner Y == k*500 (world, not kerb); (c) anchors on both segments with grid 5 m have X
  multiples of 500; (d) anchor click within half a step of a segment end on 10 m, crossing
  beyond -> no anchor; (e) all existing StandPlot/FuelDepot tests unchanged (grid off).
- [ ] Step 2: run - FAIL.
- [ ] Step 3: implement.
- [ ] Step 4: build, run `Airside.Tool` - PASS.
- [ ] Step 5: commit `feat(grid): stands and plots on the world grid; stand depth guides`.

### Task 4: Overlay

**Files:**
- Modify: `RoadBuildTool.h` - append `GridMinor`, `GridMajor` to `EPreviewStyle`.
- Modify: `Present/PreviewPalette.cpp` - colours (minor white a=0.12, major white a=0.25), look
  (ThicknessScale 0.5).
- Create: `Public/Tool/GridOverlay.h`, `Private/Tool/GridOverlay.cpp` -
  `void GridOverlay::Describe(const FToolContext& Context, double RadiusUu, IToolPreviewSink& Sink)`
  emits nothing when `Context.GridStepUu <= 0`, else PiecesInDisc around GuidedCursor().
- Modify: `Source/AirportMgr/RoadBuildHUD.cpp` - style list + call before `Tool->BuildPreview`.
- Modify: `AirsideEditor/Private/RoadBuildEditorTool.cpp` - call before `Tool->BuildPreview`.
- Test: `AirsideTests/Private/GridOverlayTest.cpp`.

Radius: callers pass `Tunables.GuideSources.GridOverlayRadiusUu()` - the HUD and editor tool
already hold tunables; if not, carry `GridOverlayRadiusUu` on FToolContext beside GridStepUu
(decide on reading the call site; prefer the context field - one door).

- [ ] Step 1: tests - spy sink via session context: grid 5 m + Taxiway tool -> >0 GridMinor
  lines, all within radius of GuidedCursor; grid off -> 0; Select tool with grid on -> 0;
  palette/HUD looks test already iterates styles (check it goes red without the HUD entry).
- [ ] Step 2-4: fail, implement, pass.
- [ ] Step 5: commit `feat(grid): local grid overlay`.

### Task 5: Bar button

**Files:**
- Modify: `Source/AirportMgr/BuildActions.h` - `FBuildAction::DynamicLabel`
  (`TFunction<FText(const FBuildActionContext&)>`, optional).
- Modify: `Source/AirportMgr/BuildBarWidget.cpp` - tick loop sets label text when DynamicLabel.
- Modify: `Source/AirportMgr/BuildActions.cpp` - `snap.grid` in Snap section, no key.
- Modify: `RoadBuildController.h/.cpp` - `CycleGridStep()`, `GetGridStep()`; log
  `LogRoadBuild: Grid step -> %s`.
- Test: `BuildActionsTest` (AirportMgr tests) - `snap.grid` exists, Snap section, no key.

- [ ] Steps: test, fail, implement, pass, commit `feat(grid): Grid button cycles off/1/5/10 m`.

### Task 6: Verify and PR

- [ ] Full build (worktree, `-NoHotReloadFromIDE`).
- [ ] `./Tools/Run-AirsideTests.ps1 -Project <worktree>` - quote `N run, N failed, N crashed`.
- [ ] Check-Architecture clean; UE_LOG / comment counts not down in touched files.
- [ ] Code review of the branch.
- [ ] Push, PR with template (build line, test line); PIE verification steps in body.
