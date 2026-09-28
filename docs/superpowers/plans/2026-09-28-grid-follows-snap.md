# Grid Follows Snap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the snap grid turns to the thing being snapped to (Follow), or stays world-aligned (World), toggled by bar button + H.

**Architecture:** `Solve/GridSnap` gains `FGridFrame` (origin, axis, step); every grid function gets a frame overload that maps into the frame and calls today's world arithmetic. A pure resolver `Tool/GridFrameSource` picks the frame from winner > tool line > anchor reference > road snap > held. `FBuildSession::MakeContext` resolves it after the guide chain, applies the grid, and puts `FToolContext::GridFrame` on the context for every consumer.

**Tech Stack:** UE 5.8 C++, automation tests (`Airside.*`), `Run-AirsideTests.ps1`.

**Spec:** `docs/superpowers/specs/2026-09-28-grid-follows-snap-design.md`

## Global Constraints

- World frame results bitwise equal to the pre-change functions.
- `Solve/` stays `CoreMinimal.h` only.
- A phase is an enum: `EGridOrientation { Follow, World }`, UENUM, default Follow.
- Winner relations that turn the grid: Parallel (label Along/SquareTo only), Collinear, MatchingGap; references Taxiway, ServiceRoad, Runway, Apron, Stand. Never ThisGesture/World, never DegreesTo/AngledFrom/Extending/LevelWith.
- Key H on `snap.gridorient`; logs `LogRoadBuild: Grid orientation -> follow|world`.
- `LogAirside: Grid frame -> <deg> deg through (<x>, <y>) from <source>` once per change.
- Every `UE_LOG` and WHY comment survives; `Check-Architecture.ps1` green.

## Refinement over the spec (found reading the code)

Plot tools (stand, depot) find their taxiway themselves in their first two stages - not a road
snap. New `IBuildTool::DescribeGridLine`: the tool names the centreline it attaches to. Ranked
second, after winners. Spec updated in Task 3.

## Review Focus

1. Grid step changed while Follow holds a frame - the held origin/axis keep, the step updates (Task 3 test "held frame takes the new step").
2. SquareTo winner (Direction perpendicular to the road) must give the road's centreline frame, not a line through ReferenceAt across it (Task 2 test).
3. A diagonal-winner (45 degrees, DegreesTo) must NOT turn the grid (Task 2 test).
4. Reverse-drawn segment (B->A) gives the identical frame (Task 1 test).
5. Alt held then released keeps the Follow frame (Task 3 test).

---

### Task 1: FGridFrame in Solve/GridSnap

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/GridSnap.h`, `Private/Solve/GridSnap.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/GridSnapTest.cpp`

**Interfaces — produces:**
```cpp
namespace GridSnap {
struct FGridFrame {
	FVector2D Origin = FVector2D::ZeroVector;
	FVector2D Axis = FVector2D(1.0, 0.0);   // unit, folded into [0, 90) degrees
	double StepUu = 0.0;
	static FGridFrame World(double StepUu);
	static FGridFrame Along(const FVector2D& Point, const FVector2D& Direction, double StepUu);
	bool IsOn() const { return StepUu > 0.0; }
	bool IsWorldAligned() const;           // Origin == 0 && Axis == (1,0)
	bool SameLines(const FGridFrame& Other) const; // exact Origin/Axis/Step equality
	double AxisDegrees() const;
};
AIRSIDE_API FVector2D Quantise(const FVector2D& Point, const FGridFrame& Frame);
AIRSIDE_API bool NearestCrossingAlong(const FVector2D& Origin, const FVector2D& Direction, const FVector2D& Near, const FGridFrame& Frame, FVector2D& Out);
AIRSIDE_API bool NearestCrossingInRange(const FVector2D& Origin, const FVector2D& Direction, const FVector2D& Near, const FGridFrame& Frame, double TMin, double TMax, FVector2D& Out);
AIRSIDE_API void PiecesInDisc(const FVector2D& Centre, double RadiusUu, const FGridFrame& Frame, TArray<FPiece>& Out);
}
```
Existing `double StepUu` overloads stay as the world arithmetic.

- [ ] Step 1: tests (new `IMPLEMENT_SIMPLE_AUTOMATION_TEST`s in GridSnapTest.cpp):
  - `Airside.Solve.GridSnap.WorldFrameIsBitwiseToday` - for each input in the existing four tests, frame overload with `World(step)` returns bitwise the double overload's result (`==` on components plus `FMath::IsNegativeOrNegativeZero`-free: compare with `memcmp` of the two FVector2D).
  - `Airside.Solve.GridSnap.AlongFolds` - `Along(P,(cos30,sin30))`, `Along(P,-dir)`, `Along(P, dir rotated 90)` all `SameLines`; zero direction gives World.
  - `Airside.Solve.GridSnap.AlongPutsALineOnTheThing` - `Along((1234,777), 30deg, 500)`: `Quantise` of a point 40 uu off the line, near it, lands with local v == 0 (within 1e-6); a point on the line's foot from the origin has local u == 0.
  - `Airside.Solve.GridSnap.CollinearAndParallelShareLines` - `Along(A,d)` and `Along(A + d*777, d)` SameLines within 1e-9 (collinear); `Along(A,d)` and `Along(A + perp*500, d)` quantise a test point to the same result within 1e-6 (parallel, one step apart).
  - `Airside.Solve.GridSnap.RotatedPiecesFollowAxis` - every piece of `PiecesInDisc(C, 6000, Along(...30deg, 500))` is parallel or perpendicular to Axis (|cross| or |dot| < 1e-9 of unit piece dir), within the disc (+1e-6), and at least one major.
- [ ] Step 2: build + run `-Filter Airside.Solve.GridSnap`, expect compile fail.
- [ ] Step 3: implement: `ToLocal(Q) = ((Q-O)·Axis, (Q-O)·Perp)`, `Perp = (-Axis.Y, Axis.X)`; `ToWorld(L) = O + Axis*L.X + Perp*L.Y`; directions rotate without origin. Each frame overload: `if (Frame.IsWorldAligned()) return <double overload>(..., Frame.StepUu, ...)` (fast path - the bitwise contract, not an optimisation); else map in, call, map out. `Along`: `Unit = Direction.GetSafeNormal()`; zero -> World; fold by `(x,y)->(y,-x)` until `X > 0 && Y >= 0` (swaps and negations only, exact); `Origin = Point - Unit * (Point·Unit)` (foot of world origin on the line). Header comment: why centreline + world-phased cross lines, why the fold.
- [ ] Step 4: run, pass.
- [ ] Step 5: commit `feat(grid): grid frame - rotated grid arithmetic in Solve`.

### Task 2: Frame resolver `Tool/GridFrameSource`

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/GridFrameSource.h`, `Private/Tool/GridFrameSource.cpp`
- Modify: `Public/Tool/SnapGuideSettings.h` (+ `EGridOrientation` UENUM, `GridOrientation` UPROPERTY, `ToggleGridOrientation()`), `Private/Tool/SnapGuideSettings.cpp`, `Public/Tool/BuildSession.h` (tunables `operator==`)
- Test: create `Plugins/Airside/Source/AirsideTests/Private/GridFrameSourceTest.cpp`

**Interfaces — produces:**
```cpp
UENUM(BlueprintType) enum class EGridOrientation : uint8 { Follow, World };
// FSnapGuideSettings: UPROPERTY(EditAnywhere, Category="Grid") EGridOrientation GridOrientation = EGridOrientation::Follow;
//                     void ToggleGridOrientation();
enum class EGridFrameSource : uint8 { World, Winner, ToolLine, Anchor, RoadSnap, Held };
struct FGridFrameInputs {
	EGridOrientation Orientation = EGridOrientation::Follow;
	double StepUu = 0.0;
	const SnapGuide::FResult* Guide = nullptr;
	bool bToolLine = false; FVector2D ToolThrough, ToolDirection;
	bool bAnchor = false;   FVector2D AnchorOrigin, AnchorReference;
	bool bRoadSnap = false; FVector2D RoadA, RoadB;
};
namespace GridFrameSource {
	AIRSIDE_API bool LineOfWinner(const SnapGuide::FCandidate& Winner, FVector2D& OutThrough, FVector2D& OutDirection);
	AIRSIDE_API EGridFrameSource Resolve(const FGridFrameInputs& In, GridSnap::FGridFrame& InOutHeld, GridSnap::FGridFrame& Out);
	AIRSIDE_API const TCHAR* Name(EGridFrameSource Source);
}
```
Rules in `LineOfWinner`: reference in {Taxiway, ServiceRoad, Runway, Apron, Stand}; Collinear -> (Through, Direction); Parallel+Along -> (ReferenceAt, Direction); Parallel+SquareTo -> (ReferenceAt, PerpCCW(Direction)); MatchingGap -> (ReferenceAt, Direction); else false. `Resolve`: `Out.StepUu = In.StepUu`; step <= 0 -> Out = World(0), return World; World orientation -> Out = World(step), held untouched, return World; else first of winner (Guide winners in order) / tool line / anchor (Reference non-zero) / road snap (A != B) builds `Along(...)`, written to held (origin+axis) and Out; none -> Out = held with the new step, return Held.

- [ ] Step 1: tests `Airside.Tool.GridFrame.*`: `ParallelWinnerTurns`, `SquareToMatchesAlong` (same SameLines as the Along candidate of the same road), `DiagonalWinnerIgnored` (DegreesTo -> falls to held), `GestureWinnerIgnored` (ThisGesture), `ToolLineBeatsAnchor`, `AnchorWhenNoWinner`, `RoadSnapLast`, `NothingHoldsLast` (held from an earlier call survives; step taken from input), `WorldIgnoresEverything` (held untouched), `OffIsOff`, `OrientationTogglesAndDefaultsFollow`, `TunablesCompareOrientation` (`operator==` false when only GridOrientation differs). Hand-built `SnapGuide::FCandidate`s - no world.
- [ ] Step 2: build; fail.
- [ ] Step 3: implement; add `&& GuideSources.GridOrientation == Other.GuideSources.GridOrientation` beside `GridStep` in `FBuildSessionTunables::operator==`.
- [ ] Step 4: full build (UPROPERTY; worktree build `-NoHotReloadFromIDE`), run `-Filter Airside.Tool.GridFrame`; pass.
- [ ] Step 5: commit `feat(grid): grid frame source - what the grid follows`.

### Task 3: Session wiring, consumers, tool grid line

**Files:**
- Modify: `Public/Tool/RoadBuildTool.h` (`FToolContext::GridStepUu` -> `GridSnap::FGridFrame GridFrame`; `IBuildTool::DescribeGridLine`), `Private/Tool/BuildSession.cpp`, `Public/Tool/BuildSession.h` (`mutable GridSnap::FGridFrame HeldGridFrame; mutable GridSnap::FGridFrame LoggedGridFrame;`), `Public/Tool/SnapGuideChain.h`/`.cpp` (Resolve stops applying the grid; `ApplyGrid(FResult&, const FVector2D&, const GridSnap::FGridFrame&)`), `Public|Private/Tool/PlotGesture.*` (`double GridStepUu` -> `const GridSnap::FGridFrame& Grid` defaulted `GridSnap::FGridFrame()`), `StagedPlotTool.h/.cpp` (store `RoadA` at pin; `DescribeGridLine`), `StandPlotTool.cpp`, `PlotPlaceTool.cpp`, `GridOverlay.h/.cpp`, `Source/AirportMgr/RoadBuildController.h/.cpp` (readout key: frame instead of step)
- Test: `GridSnapChainTest.cpp`, `GridOverlayTest.cpp`, `StandGridTest.cpp` (update `.GridStepUu` reads to `.GridFrame.StepUu`, `AnchorAt(..., 500.0)` to `GridSnap::FGridFrame::World(500.0)`)

**Interfaces — consumes** Task 1/2. **Produces:**
```cpp
virtual bool DescribeGridLine(const URoadNetwork* Network, const FVector2D& Cursor, FVector2D& OutThrough, FVector2D& OutDirection) const { return false; }
```
MakeContext order: road snap -> guide chain (Resolve, no grid) -> `bGridApplies = !bSuspendGuides && Tool && (bGuideChainRan || Tool->SnapsToGrid())` -> if applies and step > 0: fill `FGridFrameInputs` (Guide if chain ran; `Tool->DescribeGridLine(Network, PlaneHit, ...)`; anchor if chain ran; road snap if `Snapped.Kind == Segment && Network->SegmentEnds`), `GridFrameSource::Resolve(In, HeldGridFrame, Frame)`, log on `!Frame.SameLines(LoggedGridFrame)`, `if (bGuideChainRan) ApplyGrid(Guide, PlaneHit, Frame)`, `Context.GridFrame = Frame`, radius as today.

- [ ] Step 1: tests:
  - GridSnapChainTest `Airside.Tool.GridSnap.FollowTurnsToTheRoad` - taxiway at 30 degrees, Collinear winner near its extension, Follow: `Context.GridFrame.AxisDegrees()` == 30 (1e-6) and the guided point on the extension; World: axis (1,0).
  - `Airside.Tool.GridSnap.FollowHoldsThroughAlt` - after a follow frame, an Alt frame has no grid, the next non-Alt frame in the open still has the 30-degree frame.
  - `Airside.Tool.GridSnap.HeldFrameTakesNewStep` - follow frame at 5 m, set 10 m, cursor in the open: axis still 30, step 1000.
  - GridOverlayTest `Airside.Tool.GridOverlay.TurnsWithTheFrame` - stand tool idle beside a 30-degree taxiway, 5 m: every piece parallel/perpendicular to the taxiway; World: every piece axis-aligned.
  - StandGridTest `Airside.Tool.StandGrid.DiagonalTaxiwayFollows` - 30-degree taxiway, 5 m, Follow: anchor and frontage end on cross lines (local u multiple of 500 in the taxiway frame), back corner a whole number of 500 from the centreline (distance to the line), back edge parallel to the taxiway.
  - Existing tests fixture: leave Follow default; they are axis-aligned roads through y = 0 or on world lines - run and read what breaks before touching any.
- [ ] Step 2: build; fail.
- [ ] Step 3: implement per the interfaces; `FStagedPlotTool::DescribeGridLine`: `Pinned == 0` -> `PlotGesture::NearestRoad(*Network, Cursor, Accept, Id, T)` + `SegmentEnds` -> (A, B-A unit); `Pinned >= 1` -> (RoadA, Along). Plot `GridStepUu > 0.0` tests become `Grid.IsOn()`. GridOverlay comment: "world grid" -> "grid frame". Spec: add the ToolLine source (Refinement section).
- [ ] Step 4: full build, run `-Filter Airside.Tool.Grid` and `Airside.Tool.StandGrid`, `Airside.Tool.StandPlot`, `Airside.Tool.SnapGuide`; pass. `grep -rn "GridStepUu" --include=*.h --include=*.cpp` shows only `FSnapGuideSettings::GridStepUu` and its callers.
- [ ] Step 5: commit `feat(grid): grid follows the snap - session, plots, overlay`.

### Task 4: Toggle, key, controller

**Files:**
- Modify: `Source/AirportMgr/BuildActions.cpp` (action + NO KEYS comment exception), `Source/AirportMgr/RoadBuildController.h/.cpp` (`ToggleGridOrientation`, `IsGridFollowing`), `Source/AirportMgr/BuildActionsTest.cpp`

- [ ] Step 1: test `snap.gridorient` registered in Snap with `Key == EKeys::H`, not Ctrl; `FindAction(EKeys::H, false)` returns it (the existing duplicate-chord test covers clashes).
- [ ] Step 2: fail.
- [ ] Step 3: implement:
```cpp
{
	FBuildAction Orient = Make(TEXT("snap.gridorient"), EActionSection::Snap, LOCTEXT("SnapGridOrient", "Grid follows"),
		EKeys::H, false,
		[](FBuildActionContext& Ctx) { Ctx.Controller.ToggleGridOrientation(); },
		[](const FBuildActionContext& Ctx) { return Ctx.Controller.IsGridFollowing(); },
		Always);
	Orient.DynamicLabel = [](const FBuildActionContext& Ctx)
	{
		return Ctx.Controller.IsGridFollowing() ? LOCTEXT("SnapGridFollow", "Grid: follow") : LOCTEXT("SnapGridWorld", "Grid: world");
	};
	Out.Add(MoveTemp(Orient));
}
```
Controller: toggle on `Actor->GuideSources`, `UE_LOG(LogRoadBuild, Log, TEXT("Grid orientation -> %s"), ...)`.
- [ ] Step 4: build (game module), `-Filter AirportMgr` build-actions tests; pass.
- [ ] Step 5: commit `feat(grid): Grid follow/world toggle on the bar and H`.

### Task 5: Verify and PR

- [ ] `./Tools/Run-AirsideTests.ps1 -Project <worktree uproject>` - read the `N test(s) run, N failed, N crashed` line.
- [ ] `./Tools/Check-Architecture.ps1` green; UE_LOG count not lower than main in touched files.
- [ ] Push `feature/grid-follows-snap`, PR to main with build line, test line, behaviour-change note (default Follow turns saved levels' grids), PIE verification step.
