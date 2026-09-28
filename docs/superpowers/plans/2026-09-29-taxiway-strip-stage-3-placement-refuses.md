# Taxiway Strip Stage 3 - Every Placement Refuses Inside a Strip

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Roads, taxiways and depot plots refuse to be placed, moved or healed into a taxiway's clearance strip, except where they meet the taxiway within 30 degrees of square; a new taxiway also refuses if its own strip would swallow something already built.

**Architecture:** Grow `Model/TaxiwayStrip` from "does this footprint intrude" to the one placement judge: a segment-shape footprint, an exempt set (the taxiways a segment meets at an allowed angle), and a reverse query (what a new taxiway's strip would contain). Each placement path asks it ONCE through a single evaluator that both the tool's preview and the facade's commit call - the `WhyStandRefused` pattern, extended.

**Tech Stack:** UE 5.8.2 C++, Airside plugin, UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-28-taxiway-clearance-strip-design.md` (stage 3). Stages 1-2 merged as #390 (main 0b231a69).

## Research this plan stands on (2026-09-29, read-only agents against 0b231a69)

- Registry: `ToolRegistry()` (`Public/Tool/BuildSession.h:152`, `Private/Tool/BuildSession.cpp:18-97`) - Select(4), Taxiway(1, `FRoadDrawTool(Taxiway)`), Apron(2), Stand(3), Guideline(5), Runway(6), HoldingPosition(8), Road(9, `FRoadDrawTool(ServiceRoad)`), FuelDepot(0, `FPlotPlaceTool`). `FEditTool` is outside the registry and moves nodes/apron corners.
- Road placement judge today: `ERoadPlacement RoadPlacement::Validate(const URoadNetwork&, FRoadNodeId From, const FRoadSnapResult& To, const FRoadPlacementLimits&)` (`Public/Tool/RoadPlacement.h`, `.cpp:52`) - no kind, no profile; `Limits.NewRoadHalfWidth` comes from `MakeTunables` using the TAXIWAY default whatever is being laid (`RoadEditFacade.cpp:~484-492`) - a latent bug this plan fixes. Called from the click (`RoadDrawTool.cpp:160`), readout label (`:230-246`), ghost validity (`:655-657`).
- Facade `ConnectNodes` (`RoadEditFacade.cpp:565`) has no geometric check. `MoveNode` (`:1358`), `RoadHeal` (node delete heal) and `PlaceEntityInPlot` (`RoadEditFacadeSurfaces.cpp:413`, depot, no single Why) are the other pavement paths. `PlaceRunway` always makes fresh nodes.
- No segment polygon exists; a segment is `A, Control, B` + profile half-widths (`GuidelineGeom::Eval`).
- A Segment snap becomes `SplitSegment` (two new ids; in preview only the ghost network has split).
- Angles: `RoadGeom::AngleBetween(const FVector2D&, const FVector2D&)` in [0, PI]; rule 18 bans `Acos`.
- **Merged defect**: `TaxiwayStrip::HasStrip/StripWidthOf/WorstIntrusion` read `Segment.Profile`, not `URoadNetwork::ProfileFor` - a segment saved with the actor's transient fallback profile reloads with null and gets no strip (`RoadSurfacePresenter.cpp:626-633`). Task 1.

## Rulings made while planning (user asleep - each is in "Unresolved questions")

1. **Aprons are exempt** (USER agreed 2026-09-29). An apron is aircraft pavement - the same movement area as the taxiway it borders. Refusing aprons in strips would forbid the normal apron-beside-taxiway layout. What stands ON an apron is still judged (stands already are).
2. **Runways are out of scope** (spec: "runways have their own strip rules"). A runway is neither refused by taxiway strips nor given one.
3. **Straight continuation is a meeting.** A segment leaving a taxiway's end node at >= 150 degrees to it (extending it) meets it; exempt from that taxiway, as a right-angle join is. Else the rule would forbid lengthening a taxiway.
4. **Existing placements are not re-judged here.** Roads already inside strips (e.g. in M_Test) stay; stage 6 turns them into taxiway restrictions.
5. **Both directions for a new taxiway.** Its PAVEMENT must be clear of other strips, AND its STRIP must be clear of existing stands, depots, road pavement and other taxiway pavement - except what it meets. One-way checking would let an F taxiway be laid 20 m from a B taxiway because the B's 9 m strip is clear.

## Global Constraints

- `Model/` world-free (NewObject network in tests); `Solve/` CoreMinimal only.
- UENUM values APPENDED (`ERoadPlacement` is a UENUM - add `InsideStrip`, `StripSwallows` at the end).
- One evaluator per placement kind: preview and commit call the same function; a refusal string is written once.
- Comments say WHY; claims about other code carry `// ENFORCED BY:` (rule 12).
- Build (worktree): `Build.bat AirportMgrEditor Win64 Development -Project="<worktree>\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE`. Tests: `Tools/Run-AirsideTests.ps1 -Project <uproject> [-Filter X]`; read `N run, N failed, N crashed`.
- Test world default taxiway: 24 m (Code E), strip 28 m, keep-out edge 40 m off the centreline.

## Review Focus

1. **Preview/commit disagreement on a Segment snap** - preview judges against a ghost split, commit against the real split; the exempt set must name the met taxiway by the ORIGINAL segment id before the split, or by geometry. Task 4 test: snap onto mid-taxiway, preview says valid, commit succeeds.
2. **A road meeting a taxiway at a node where two taxiways join** - it meets both; the 30-degree test applies per met taxiway arm. Task 3 test "T-junction of taxiways".
3. **Moving a node drags every incident segment** - `MoveNode` must judge all of them, and undo must not leave a half-moved node. Task 6.
4. **Heal on node delete** refused silently would leave a gap the player did not ask for - it logs and keeps both stubs. Task 6 test.
5. **Road half-width** - the footprint must use the profile being laid (road 4-6 m), not the taxiway default - else every road near a taxiway is refused at twice its width. Task 2 test.

---

### Task 1: Strip reads the profile through `ProfileFor`

**Files:** Modify `Plugins/Airside/Source/Airside/Private/Model/TaxiwayStrip.cpp` (IsAircraftOnly, StripWidthOf, WorstIntrusion); Test `AirsideTests/Private/RoadNetworkTest.cpp` (append).

- [ ] **Step 1: Failing test** - a taxiway segment with `Profile = nullptr` and `Net->DefaultProfile = URoadProfile::MakeTransient(2400, 1600)`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayStripReloadedProfileTest, "Airside.Model.TaxiwayStrip.ReloadedSegmentHasAStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayStripReloadedProfileTest::RunTest(const FString&)
{
	// A SEGMENT RELOADED FROM A SAVED LEVEL carries a null Profile when it was laid with the
	// actor's transient fallback (RoadSurfacePresenter's DefaultProfile comment); ProfileFor
	// repairs it. The strip must be read through the same accessor, or a reloaded map has none.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	Net->DefaultProfile = URoadProfile::MakeTransient(2400.0, 1600.0);
	const FRoadSegmentId Taxi = Net->AddSegment(Net->AddNode({ -10000.0, 0.0 }), Net->AddNode({ 10000.0, 0.0 }),
		FVector2D::ZeroVector, nullptr);
	TestEqual(TEXT("the reloaded taxiway still has E's strip"), TaxiwayStrip::StripWidthOf(*Net, Taxi), 2800.0, 0.5);
	const TArray<FVector2D> Flush{ { -2000.0, 1200.0 }, { 2000.0, 1200.0 }, { 2000.0, 5200.0 }, { -2000.0, 5200.0 } };
	TestTrue(TEXT("and a stand flush to it intrudes"), TaxiwayStrip::WorstIntrusion(*Net, Flush).IsSet());
	return true;
}
```

Before writing: confirm `AddSegment` accepts a null profile and `DefaultProfile` is assignable from a test (it is set by the presenter at `RoadSurfacePresenter.cpp:633`). If `AddSegment` rejects null, add then null it through `FRoadNetworkTestAccess` (add a `SetSegmentProfileForTest` beside `SetEntityOutlineForTest` if none exists).

- [ ] **Step 2:** Build, run `-Filter Airside.Model.TaxiwayStrip`; expect FAIL (strip 0).
- [ ] **Step 3:** In all three functions replace `Segment->Profile` / `Segment.Profile` with `Network.ProfileFor(*Segment)` (null-check the result). Comment: `// THROUGH ProfileFor, the one accessor that repairs a reloaded segment's null Profile - read raw, a saved map lost every strip laid on the fallback profile (found 2026-09-29).`
- [ ] **Step 4:** Run `-Filter Airside.Model.TaxiwayStrip`, `-Filter Airside.Tool.StandPlot`; expect pass. Commit `fix(strip): read the profile through ProfileFor - reloaded segments had no strip`.

---

### Task 2: A segment's footprint, and the right half-width

**Files:** Modify `Public/Model/TaxiwayStrip.h`, `Private/Model/TaxiwayStrip.cpp`; Test `RoadNetworkTest.cpp`.

**Produces:**
```cpp
namespace TaxiwayStrip
{
	/** A road or taxiway that exists or is about to - enough to know its ground. */
	struct FSegmentShape
	{
		FVector2D A = FVector2D::ZeroVector;
		FVector2D Control = FVector2D::ZeroVector;   // (A+B)/2 for straight
		FVector2D B = FVector2D::ZeroVector;
		double HalfWidth = 0.0;                      // the WIDER half, GetMaxHalfWidth()
	};
	/** The pavement polygon: both edges of the sampled centreline, CCW. */
	AIRSIDE_API TArray<FVector2D> FootprintOf(const FSegmentShape& Shape);
	/** The shape of a live segment through ProfileFor; false if dead or profile-less. */
	AIRSIDE_API bool ShapeOf(const URoadNetwork& Network, FRoadSegmentId Id, FSegmentShape& Out);
}
```

- [ ] **Step 1: Failing test** - `FootprintOf` of a straight 200 m x 6 m shape has 2*(DefaultSamples+1) points, area 200*6 m^2 within 0.1%, and for a curved control every point is exactly HalfWidth from the sampled centreline (measure with `RoadGeom::ClosestPointOnSegment` against the Eval samples). Name `Airside.Model.TaxiwayStrip.Footprint`.
- [ ] **Step 2:** Run, expect compile failure.
- [ ] **Step 3:** Implement with `GuidelineGeom::Eval` at `DefaultSamples` and `GuidelineGeom`'s analytic tangent (the header documents "Unit direction of travel at T. The ANALYTIC derivative of Eval" - use it, do not difference samples). Left edge forward, right edge back, so the polygon is CCW (assert `RoadGeom::PolygonArea > 0` in the test).
- [ ] **Step 4: Half-width bug.** In `URoadEditFacade::MakeTunables` (`RoadEditFacade.cpp:~484-492`) `NewRoadHalfWidth` is taken from the taxiway default. It is used by `Validate`'s corner checks. Leave it (other tests pin it), but Task 4's footprint takes its half-width from `ResolveProfileFor(Kind, WidthIndex)` directly - add a comment at `MakeTunables` naming that the strip judge does NOT read `NewRoadHalfWidth`, and why (it is the taxiway default regardless of kind).
- [ ] **Step 5:** Tests pass; commit `feat(strip): segment footprint and shape`.

---

### Task 3: The segment judge - exempt what it meets at an allowed angle

**Files:** `TaxiwayStrip.h/.cpp`; Test `RoadNetworkTest.cpp` (new test cases in one `Airside.Model.TaxiwayStrip.SegmentJudge`).

**Produces:**
```cpp
namespace TaxiwayStrip
{
	/** Where a new segment's end meets the network: a node it shares, or a point on a segment it
	 *  will split. Unset Node and Segment = a free end. */
	struct FSegmentEnd
	{
		FRoadNodeId Node;
		FRoadSegmentId Segment;   // a Segment snap: the ORIGINAL segment, before any split
		FVector2D At = FVector2D::ZeroVector;
	};

	/** What a placement is refused for, if anything. Written once; tool and facade both show Text. */
	struct FStripVerdict
	{
		bool bRefused = false;
		FString Text;
		double Depth = 0.0;
	};

	/** Meets within 30 degrees of square (60..120 deg), or continues straight on (>= 150 deg). */
	inline constexpr double MeetMinDegrees = 60.0;
	inline constexpr double MeetMaxDegrees = 120.0;
	inline constexpr double ContinueMinDegrees = 150.0;

	/**
	 * May a road or taxiway of this shape be laid with these ends? Refuses when its pavement
	 * enters the strip of a taxiway it does not MEET (share a node / split, at an allowed angle),
	 * or - bIsTaxiway - when its own strip would contain an existing stand, depot, road or other
	 * taxiway's pavement it does not meet. Ignore lists the segments a caller is replacing (a
	 * moved node's own incident segments, a heal's two stubs).
	 */
	AIRSIDE_API FStripVerdict JudgeSegment(const URoadNetwork& Network, const FSegmentShape& Shape,
		bool bIsTaxiway, const FSegmentEnd& AtA, const FSegmentEnd& AtB,
		TConstArrayView<FRoadSegmentId> Ignore = {});
}
```

Also give `WorstIntrusion` an exempt parameter (`TConstArrayView<FRoadSegmentId> Exempt = {}`) - skip those taxiways. Existing callers pass nothing.

- [ ] **Step 1: Failing tests** (one `RunTest`, named blocks):
  - "right-angle road meets taxiway at a node": taxiway W-E at y 0; road from the taxiway's mid node north 100 m. Not refused.
  - "shallow road meets at 20 degrees": refused, Text contains "clearance strip".
  - "road runs alongside, no meeting": road y 2000 from x -5000 to 5000 (6 m road, E taxiway reach 4000): refused.
  - "road crosses without a junction": road N-S across the taxiway, free ends: refused, Text contains "junction".
  - "taxiway continues straight on from the end node": not refused (ruling 3).
  - "T-junction of taxiways": a road leaving the junction node square to one taxiway but 15 degrees off the other: refused for the other (Review Focus 2).
  - "new F taxiway 20 m from an existing B taxiway, parallel": refused - its strip swallows the B's pavement (ruling 5).
  - "new taxiway whose strip would contain a stand": refused, Text names the stand.
  - "service road, no taxiway near": not refused.
  Build shapes via `FSegmentShape` and ends via `FSegmentEnd`; the met node ids from `AddNode`.
- [ ] **Step 2:** Run, expect compile failure.
- [ ] **Step 3: Implement.** Order:
  1. Exempt set: for each of AtA/AtB with a Node, every incident taxiway segment (`HasStrip`) whose angle to the new segment's tangent at that end (use `URoadNetwork::GetOutgoingTangent(Seg, Node)` for the existing arm, the new shape's analytic tangent for the new one) passes `MeetMin..MeetMax` or `>= ContinueMin` -> exempt; failing it -> refused with "meets taxiway at N degrees - within 30 degrees of square, or straight on". For a Segment end, the met segment's tangent at `At` (Eval derivative at the closest T) - exempt if allowed.
  2. Pavement: `WorstIntrusion(Network, FootprintOf(Shape), Exempt ∪ Ignore)`; if set -> refused, Text `inside a Code X taxiway's clearance strip by N m - end it on the taxiway at a junction, or move it N m away`. If the new shape crosses a strip-bearing centreline and neither end meets it -> Text says "crosses a taxiway without a junction".
  3. bIsTaxiway reverse: strip polygon = `FootprintOf(Shape with HalfWidth + IcaoCode::TaxiwayStripForWidth(2*HalfWidth))`; test every live stand/depot `Outline` (`FEntityInstance::IsPlotted`), every live road/taxiway segment's `FootprintOf(ShapeOf(...))` not in Exempt ∪ Ignore and not the met segments, for overlap (reuse the facade's SAT `OutlinesOverlap` logic - MOVE it into `Solve/RoadGeom` as `PolygonsOverlap(A, B, Tolerance)` with its comment, and make the facade call it; refactor contract applies). Refused Text names what: "its clearance strip would contain stand N" / "a service road" / "a fuel depot" / "a taxiway".
- [ ] **Step 4:** Run `-Filter Airside.Model.TaxiwayStrip`; expect all pass. Run the full suite (the `OutlinesOverlap` move). Commit `feat(strip): one segment judge - meets within 30 degrees, both directions for taxiways`.

---

### Task 4: Road and taxiway tools refuse; facade refuses the same

**Files:** `Public/Tool/RoadPlacement.h` (append `InsideStrip` to `ERoadPlacement`; `Describe`), `Private/Tool/RoadPlacement.cpp`, `Private/Tool/RoadDrawTool.cpp` (readout label shows the verdict Text), `Private/Present/RoadEditFacade.cpp` (`ConnectNodes`), `Public/Tool/RoadEditTarget.h` (new `virtual FString WhySegmentRefused(int32 FromIndex, const FRoadSnapResult& To, ERoadKind Kind, int32 WidthIndex) const = 0;`), test `AirsideTests/Private/RoadDrawToolTest.cpp` or the file holding `Airside.Tool.TaxiwayWidth` (read it; append there).

- [ ] **Step 1: Failing composition tests** (spawn the actor - `FAirsideTestWorld`):
  - lay a taxiway through the facade; with the Road tool, click a start 20 m off the taxiway and a second point parallel: the readout's refusal label says "clearance strip", the click lays nothing (`Network->GetSegments()` count unchanged), and `ConnectNodes` called directly on those two nodes returns false and logs `ConnectNodes refused: inside`.
  - the same road ending ON the taxiway at a right angle (Segment snap mid-taxiway): preview valid, click lays it (Review Focus 1).
  - Taxiway tool: a parallel taxiway 20 m away is refused.
- [ ] **Step 2:** Run, expect FAIL.
- [ ] **Step 3: Implement one evaluator.** `URoadEditFacade::WhySegmentRefused` builds the `FSegmentShape` (straight: `Control = (A+B)/2`; half-width from `ResolveProfileFor(Kind, WidthIndex)->GetMaxHalfWidth()`), the two `FSegmentEnd`s (From node; To = snap Node / Segment + Position / free), calls `TaxiwayStrip::JudgeSegment(Network, Shape, Kind == ERoadKind::Taxiway, AtA, AtB)` and returns `Verdict.Text` (empty = allowed). `RoadPlacement::Validate` keeps its geometric checks; the road tool asks `Target->WhySegmentRefused(...)` after `Validate` returns `Valid`, and treats a non-empty string as refused: readout label (Refused style), ghost `bValid = false`, click lays nothing. `ConnectNodes` asks the same function before its price check and refuses with `UE_LOG(LogRoadMesh, Log, TEXT("ConnectNodes refused: %s"), *Why)`. Mirror every non-test implementer of `IRoadEditTarget` (grep `: public IRoadEditTarget` and test doubles; the compiler lists them).
- [ ] **Step 4:** Full suite. Expected fallout from research: `RoadColumnSplitTest` (`LaySplit` service road 2 m and 40 m off a taxiway), `GuideGridTest:193-207` (road between two taxiways 30 m apart), `NetworkGuideSourceTest` (road at 50 m - clear; apron corner - aprons exempt, ruling 1). For each: move the fixture road outside the keep-out (E: > 40 m + road half-width from the centreline), with a comment `// clear of the taxiway's clearance strip (stage 3, 2026-09-29)`. If a test's POINT is a road near a taxiway, keep the geometry and assert the refusal instead. Commit `feat(strip): roads and taxiways refuse inside a strip`, listing every fixture moved.

---

### Task 5: Depot plots refuse, through one evaluator

**Files:** `Public/Tool/RoadEditTarget.h` (`virtual FString WhyPlotRefused(TArrayView<const FVector2D> Outline) const = 0;`), `Private/Present/RoadEditFacadeSurfaces.cpp` (`PlaceEntityInPlot` asks it first; move its existing refusals - not simple, overlaps a stand - into it), `Private/Tool/PlotPlaceTool.cpp` (`DescribeReadout` shows it and gates `Committable`), test `AirsideTests/Private/PlotPlaceToolTest.cpp`.

- [ ] **Step 1: Failing test** `Airside.Tool.PlotPlace.RefusedInsideStrip`: a service road 50 m from a taxiway (clear); a depot plot drawn off the road's taxiway side deep enough to reach within 40 m of the taxiway centreline: readout warning contains "clearance strip", Build not committable, `PlaceEntityInPlot` returns INDEX_NONE. Drawn on the far side: places.
- [ ] **Step 2:** Run, FAIL.
- [ ] **Step 3:** `WhyPlotRefused` = today's two refusals (in their order, same wording) + `TaxiwayStrip::WorstIntrusion(Network, Outline)` -> "inside a Code X taxiway's clearance strip by N m" (the stand wording). `PlaceEntityInPlot` returns INDEX_NONE and logs `PlaceEntityInPlot refused: %s` when non-empty. The readout's existing post-commit "Build failed" stays for the reasons only the commit knows (ReserveForPlot fits nothing, afford).
- [ ] **Step 4:** Tests (`-Filter Airside.Tool.Plot`, full suite); commit `feat(strip): depot plots refuse inside a strip`.

---

### Task 6: Moves and heals are judged too

**Files:** `Private/Present/RoadEditFacade.cpp` (`MoveNode` `:1358`; the heal path - find it from `DeleteNode` / `RoadHeal`), test file for `Airside.Tool.Edit*` or `RoadHealTest.cpp` (grep; append).

- [ ] **Step 1: Failing tests:**
  - `Airside.Present.MoveNodeRefusedIntoStrip`: a service road whose end node is dragged to 20 m from a taxiway's centreline (not onto it): `MoveNode` returns false, the node is where it was, undo stack unchanged.
  - `Airside.Present.HealRefusedAcrossStrip`: delete a node whose heal segment would run inside a strip: both stubs remain, log `Heal skipped: <verdict>`, the delete itself still happens.
- [ ] **Step 2:** Run, FAIL.
- [ ] **Step 3:** `MoveNode`: before its edit scope, for each incident segment build the shape with the node at `To` and judge with `Ignore = the node's incident segments` (they are being replaced). Any refusal -> return false with `UE_LOG(LogRoadMesh, Log, TEXT("MoveNode refused: %s"), ...)`. Heal: judge the heal shape with `Ignore = the two stubs`; refused -> skip the heal only.
- [ ] **Step 4:** Tests + full suite; commit `feat(strip): node moves and heals refuse inside a strip`.

---

### Task 7: Every placement tool is accounted for

**Files:** Test `AirsideTests/Private/BuildSessionTest.cpp` or a new file `StripCoverageTest.cpp` (a new file needs two builds - see memory; prefer appending to the file that tests `ToolRegistry`).

- [ ] **Step 1: The test** `Airside.Tool.EveryPlacementToolHonoursTheStrip`: a table `{ ToolId, EStripCoverage::{Judged, ExemptApron, ExemptRunway, PlacesNoPavement} }` - Taxiway/Road Judged (Task 4), Stand Judged (#390), FuelDepot Judged (Task 5), Apron ExemptApron (ruling 1), Runway ExemptRunway (ruling 2), Select/Guideline/HoldingPosition PlacesNoPavement. The test iterates `ToolRegistry()` and FAILS for any registration whose `Id` is not in the table, naming it - so a new tool cannot ship without a decision. For each Judged row it runs that tool's own refusal case (call the Task 4/5 helpers).
- [ ] **Step 2:** Run - passes (all rows covered). Then delete the FuelDepot row locally and watch it fail with the name; restore. (A green test may measure nothing - memory.)
- [ ] **Step 3:** Commit `test(strip): every registered tool has a strip decision`.

---

### Task 8: Look at it

- [ ] Launch the worktree editor on port 8002 (check for other editors first), open M_Test. Ask the user to: draw a road parallel to a taxiway (refused, label at cursor), end it on the taxiway at a right angle (allowed), try a 20-degree join (refused), draw a depot reaching toward a taxiway, drag a road node into a strip. Evidence: `ConnectNodes refused:` / `MoveNode refused:` log lines and `python Tools/Mcp.py shot x.png editor`.

## Unresolved questions (for the user)

ANSWERED 2026-09-29: aprons exempt (ruling 1).

2. Straight-on continuation (>= 150 degrees) counts as meeting (ruling 3) - agree? Or only same-kind continuation (a road may not continue a taxiway's line)?
3. Existing roads already inside strips stay until stage 6 restricts their taxiway (ruling 4) - agree?
4. Refusal wording tells the player the fix ("end it on the taxiway at a junction, or move it N m away") - OK, or shorter?
