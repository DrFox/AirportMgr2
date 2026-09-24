# Two-way roads (PR 1 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Service roads become two one-way lanes whose side follows an airport-wide drive-side setting; junction turns, dead ends, anchor links and centre-line paint all follow the lanes.

**Architecture:** Lanes are declared by the road profile (two `FProfileGuideline`s, mirrored offsets, opposing directions); `FRoadGuidelineBuilder` applies `URoadNetwork::DriveSide` by negating offsets when it derives edges. The graph holds only one-way road edges, so route search is unchanged. Turn paths pair arriving lanes with leaving lanes; a dead end gets a derived balloon U-turn (`Solve/UTurnGeom`); anchors link to both lanes; `FRoadLaneMarkingBuilder` paints the centre line into the white paint layer.

**Tech Stack:** UE 5.8 C++, UE automation tests (`Airside.*`), PowerShell tooling, headless Python for the profile asset.

**Spec:** `docs/superpowers/specs/2026-09-23-road-lanes-and-widths-design.md` (sections 2-5, 7; tests 1-5, 8-10).

## Global Constraints

- Worktree `C:\repos\airportmgr2-road-lanes-and-widths`, branch `feature/road-lanes-and-widths`. Editor closed (checked 2026-09-23) - full `Build.bat` allowed, no `-NoHotReloadFromIDE` needed unless an editor opens on the main checkout.
- Build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-road-lanes-and-widths\AirportMgr.uproject" -WaitMutex`
- Tests: `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\airportmgr2-road-lanes-and-widths\AirportMgr.uproject" -Filter <x>`; read its `N test(s) run, N failed, N crashed` line. A new test .cpp needs TWO builds (memory).
- `Solve/` includes only `CoreMinimal.h` and `Solve/`.
- Taxiway and runway guidelines stay one bidirectional centreline; their turn control points stay bitwise the node.
- Drive side default Right. Right drive = the lane at NEGATIVE `CentreOffset` (right of A->B) runs AToB.
- Narrow lane 300 uu, kerb 60 uu (total 720 uu). The old service road was 600 uu kerb to kerb.
- Every derived edge keeps sample-once: follower, overlay and search read `SampleGuideline`.
- Commits: no `Co-Authored-By` trailer (user CLAUDE.md). Concise messages.
- Every new comment claiming a fact about other code carries `// ENFORCED BY:`.

## Review Focus

1. A one-lane road (bidirectional, legacy/hand-made profile) meeting a two-lane road: must still route both ways - Task 4 test "MixedLaneCounts".
2. Drive side flipped with hand-authored guideline edges on a road: they re-resolve by `(Segment, end, GuidelineIndex)`, which is unchanged by a flip - Task 3 test asserts the spared edge survives a flip.
3. A dead-end stub shorter than the balloon: the balloon is laid anyway (it is off-road by ruling), and a route still exists - Task 5 test on a 20 m stub.
4. A stand beside a two-lane road reached from either direction without a detour - Task 6 test.
5. A straight-through junction between lanes of different offset (Narrow meets a hand-made wider two-lane profile): control falls back to the chord midpoint, never a point behind an end - Task 4 test "ParallelLanes".

---

### Task 1: `EDriveSide` on the network

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadTraffic.h` (beside `EGuidelineDir`)
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h` (UPROPERTY + accessors near `DefaultProfile`)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Profiles/RoadProfile.h` (`FProfileGuideline::OffsetFor`)
- Test: `Plugins/Airside/Source/AirsideTests/Private/DriveSideTest.cpp`

**Interfaces:**
- Produces: `enum class EDriveSide : uint8 { Right, Left }` (UENUM); `EDriveSide URoadNetwork::GetDriveSide() const`; `bool URoadNetwork::SetDriveSide(EDriveSide)` (false when unchanged; bumps edit revision); `double FProfileGuideline::OffsetFor(EDriveSide) const`.

- [ ] **Step 1: Failing test** `Airside.Model.DriveSide.Default`:

```cpp
URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
TestEqual(TEXT("right-hand traffic until the player says otherwise"),
	static_cast<int32>(Net->GetDriveSide()), static_cast<int32>(EDriveSide::Right));
TestTrue(TEXT("a change reports itself"), Net->SetDriveSide(EDriveSide::Left));
TestFalse(TEXT("setting the side it already has is a no-op, so no undo step"), Net->SetDriveSide(EDriveSide::Left));
FProfileGuideline Lane; Lane.CentreOffset = -150.0;
TestEqual(TEXT("right drive keeps the authored offset"), Lane.OffsetFor(EDriveSide::Right), -150.0);
TestEqual(TEXT("left drive mirrors it across the centreline"), Lane.OffsetFor(EDriveSide::Left), 150.0);
```

- [ ] **Step 2: Build twice, run `-Filter Airside.Model.DriveSide`; expect compile failure then FAIL.**
- [ ] **Step 3: Implement.** `RoadTraffic.h`:

```cpp
/**
 * Which side of a two-lane road traffic keeps to, airport-wide (spec 2026-09-23 §2).
 * ONE SETTING, NOT PER ROAD: a junction between roads of opposite sides needs crossover
 * logic no real airside has. Applied when guidelines are derived, never when followed -
 * the lane the player sees is the lane that is driven (sample-once).
 */
UENUM()
enum class EDriveSide : uint8 { Right, Left };
```

`RoadProfile.h`, inside `FProfileGuideline`:

```cpp
/** CentreOffset as laid under Side. Profiles are authored for Right; Left mirrors them. */
double OffsetFor(EDriveSide Side) const { return Side == EDriveSide::Left ? -CentreOffset : CentreOffset; }
```

`RoadNetwork.h`: `UPROPERTY() EDriveSide DriveSide = EDriveSide::Right;` (private, with the other state), public `GetDriveSide()`, `SetDriveSide()`. `.cpp`: return false if equal; assign; `++EditRevision` (whatever the existing revision-bump call is - match `SetIntermediateHoldingPosition`'s); `UE_LOG(LogAirside, Log, TEXT("Drive side -> %s"), Side == EDriveSide::Left ? TEXT("Left") : TEXT("Right"));`
- [ ] **Step 4: Run; expect PASS.**
- [ ] **Step 5: Commit** `feat(model): EDriveSide on URoadNetwork`.

### Task 2: Two-lane service road profile

**Files:**
- Modify: `Public/Profiles/RoadProfile.h`, `Private/Profiles/RoadProfile.cpp` - `FillServiceRoad` becomes `FillTwoWayRoad(Profile, LaneWidth /*per lane*/, KerbWidth, FilletRadius)`; `MakeServiceRoadTransient(LaneWidth = 300.0, KerbWidth = 60.0, FilletRadius = 0.0)`.
- Modify every caller (compiler finds them): `ResolvedContentOncePerRebuildTest.cpp`, `RoadCrossingTest.cpp`, `ServiceRoadFilletTest.cpp`, `ServiceRoadToolTest.cpp`, `TurnPathRadiusTest.cpp`.
- Modify: `Tools/Python/build_road_profiles.py` - fill the EXISTING asset in place (memory `unreal-authoring-uassets-headlessly`: delete-and-recreate dies on references), `save_asset(path, only_if_is_dirty=False)`, `LANE_WIDTH = 300.0`.
- Test: `ServiceRoadProfileTest.cpp` (rewrite).

**Interfaces:** Produces: `URoadProfile::FillTwoWayRoad(URoadProfile*, double LaneWidth, double KerbWidth, double FilletRadius)` (UFUNCTION, script name `fill_two_way_road`). Guideline 0 = right lane (`CentreOffset = -LaneWidth/2`, AToB), guideline 1 = left lane (`+LaneWidth/2`, BToA), both `GroundVehicle`, `Width = LaneWidth`, `MaxWingspan = 0`. Bands kerb | lane | lane | kerb (two Lane bands, so the builder sees two lanes).

- [ ] **Step 1: Rewrite the test** `Airside.Build.RoadProfileGuideline`:

```cpp
URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
if (!TestEqual(TEXT("two lanes: one each way"), Road->Guidelines.Num(), 2)) { return false; }
const FProfileGuideline& Right = Road->Guidelines[0];
const FProfileGuideline& Left  = Road->Guidelines[1];
TestEqual(TEXT("offsets mirror across the centreline"), Right.CentreOffset, -Left.CentreOffset);
TestTrue(TEXT("lane 0 is right of A->B, authored for right-hand traffic"), Right.CentreOffset < 0.0);
TestEqual(TEXT("the right lane runs A to B"), static_cast<int32>(Right.Direction), static_cast<int32>(EGuidelineDir::AToB));
TestEqual(TEXT("the left lane runs B to A"), static_cast<int32>(Left.Direction), static_cast<int32>(EGuidelineDir::BToA));
TestEqual(TEXT("each lane centred in its own half"), Right.CentreOffset, -150.0);
TestEqual(TEXT("lane width is the per-lane figure"), Right.Width, 300.0);
TestEqual(TEXT("kerb to kerb: two 3 m lanes and two 0.6 m kerbs"), Road->GetTotalWidth(), 720.0);
```
Keep the existing class/kerb/continuous/exit-length assertions (class checked on both lanes).
- [ ] **Step 2: Build, run `-Filter Airside.Build.RoadProfileGuideline`; expect FAIL (one guideline).**
- [ ] **Step 3: Implement** `FillTwoWayRoad`: kerb clamp `FMath::Clamp(KerbWidth, 0.0, LaneWidth * 0.45)`; bands Curb/Lane/Lane/Curb (slots Kerb/Asphalt/Asphalt/Kerb); two guidelines as above; `CentrelineOffset = -1`, fillet, not continuous, `ExitLength = 0`. Move the old WHY comments (kerbs vs run-offs; wingspan 0; no exits) with the code; replace the "ONE guideline ... a 6 m lane IS one line both ways" comment with the two-lane rationale and the 2026-09-23 date. Fix callers: an explicit `MakeServiceRoadTransient(600.0, ...)` meant 600 kerb-to-kerb; pass `(240.0, 60.0, ...)` only if the test's geometry depends on total width, otherwise take the default and adjust expectations.
- [ ] **Step 4: Script** - `build_service_road()` loads `/Game/DA_RoadProfile_ServiceRoad` if it exists (create only if missing), calls `unreal.RoadProfile.fill_two_way_road(profile, LANE_WIDTH, KERB_WIDTH, DERIVE_FILLET)`, `unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)`. Update the docstring's width prose.
- [ ] **Step 5: Build, run `-Filter Airside.Build`; expect the rewritten test PASS; note other failures for Task 3/4.**
- [ ] **Step 6: Commit** `feat(profile): service road is two one-way lanes`.

### Task 3: Builder applies drive side

**Files:** Modify `Private/Build/RoadGuidelineBuilder.cpp:335-336` (segment loop). Test: `Plugins/Airside/Source/AirsideTests/Private/TwoWayLaneTest.cpp`.

**Interfaces:** Consumes Task 1-2. Produces nothing new; `AlphaForOffset(Profile, Declared.OffsetFor(Network.GetDriveSide()))`.

- [ ] **Step 1: Failing tests** in `TwoWayLaneTest.cpp` (fixture helper in the file: `static URoadNetwork* StraightRoad(URoadProfile*& OutProfile, FRoadSegmentId& OutSeg)` - two nodes (0,0)->(20000,0), `AddStraightSegment`, `SolveAll`, `Build`):

`Airside.Build.TwoWay.LanesDerived` (spec test 1): two edges with `DerivedFrom == Seg`; one AToB with both endpoints' Y < 0 (right of +X travel), one BToA with Y > 0; `|Y| == 150` within 1e-6.

`Airside.Build.TwoWay.RouteKeepsRight` (spec test 2): route `GraphProbe`, GroundVehicle, from the AToB edge's A node to its B node; every `Plan.Polyline` point has `Y < 0`. Then `Net->SetDriveSide(EDriveSide::Left)`, re-solve + rebuild, route A->B again (fresh nodes: pick the AToB edge again); every point `Y > 0`. Reason strings: "right-hand traffic drives right of its travel", "left-hand traffic drives left of it".

`Airside.Build.TwoWay.SparedEdgeSurvivesFlip` (Review Focus 2): hand-author an edge between the two lane ends at node B (use the existing authoring call the `GuidelineAuthoringTest` uses), flip side, rebuild; the hand edge is alive and both ends are live nodes.
- [ ] **Step 2: Build twice, run `-Filter Airside.Build.TwoWay`; LanesDerived passes already? It must FAIL on RouteKeepsRight's Left half (offset not mirrored).** If LanesDerived passes before Step 3, that is expected (profile does it); the Left half is the red one.
- [ ] **Step 3: Implement** at `:336`:

```cpp
// THE DRIVE SIDE IS APPLIED HERE AND NOWHERE ELSE. Profiles are authored for right-hand
// traffic; OffsetFor mirrors them. Direction stays tied to A/B, so the lane that ran A->B
// on the right now runs A->B on the left - which is what left-hand traffic is.
// ENFORCED BY: Airside.Build.TwoWay.RouteKeepsRight
const double Alpha = AlphaForOffset(Profile, Declared.OffsetFor(Network.GetDriveSide()));
```
- [ ] **Step 4: Run; expect PASS.** Delete the `OffsetFor` call (use `Declared.CentreOffset`), confirm RouteKeepsRight goes red, restore (memory `a-green-test-may-measure-nothing`).
- [ ] **Step 5: Commit** `feat(build): guidelines follow the drive side`.

### Task 4: Junction turns pair arriving lanes with leaving lanes

**Files:** Modify `Private/Build/RoadGuidelineBuilder.cpp:476-627` (turn loop). Modify `Tools/Check-Architecture.ps1` (rule 16). Test: `TwoWayLaneTest.cpp` additions.

**Interfaces:** Consumes `RoadGeom::LineIntersect(const FRay2D&, const FRay2D&, FVector2D&)` (`Public/Solve/RoadGeom.h:55`).

- [ ] **Step 1: Failing tests.**

`Airside.Build.TwoWay.TJunctionTurns` (spec test 4): hub (0,0), arms to (-20000,0), (20000,0), (0,20000), all two-lane. For each ordered arm pair there is exactly ONE turn edge (no `DerivedFrom`) leaving that arm's arriving-lane end, landing on the other arm's leaving-lane end. Right drive: a route from the west arm's far node on its AToB... simpler and stronger: route from a point on the west road to the north road's far end; every polyline sample's signed lateral offset from its local travel direction is negative (right) except within the junction polygon; measure with `GuidelineGeom` samples: for consecutive points P_i, P_{i+1} on straight segments only (skip points with |X|<1500 && |Y|<1500).

`Airside.Build.TwoWay.MixedLaneCounts` (spec test 3 / Review Focus 1): same hub, west arm a hand-built one-guideline bidirectional GroundVehicle profile (700 uu lane, as `RoadGuidelineBuilderTest.cpp:392` builds), others two-lane. Routes west-far -> east-far and east-far -> west-far both `IsValid()`.

`Airside.Build.TwoWay.ParallelLanes` (Review Focus 5): straight road split at a node, west half Narrow (offset 150), east half a hand-built two-lane profile with offset 250. The through turn's `Control` lies on the chord between its ends (distance from segment < 1 uu) - never behind either end.

`Airside.Build.TwoWay.TaxiwayControlUnchanged`: a taxiway X junction (`URoadProfile::MakeTransient(2300, 1500)`); every turn edge's `Control == Node->Position` exactly (`TestTrue(..., Control == Pos)`), because taxiway turns must not move.
- [ ] **Step 2: Build twice; run; expect TJunctionTurns / MixedLaneCounts FAIL.**
- [ ] **Step 3: Implement.** Replace `const int32 Count = ...; for (int32 Which = 0; Which < Count; ++Which)` with a double loop over `FromWhich < FromProfile->Guidelines.Num()` and `ToWhich < ToProfile->Guidelines.Num()`. Every `[Which]` on the From side becomes `[FromWhich]`, on the To side `[ToWhich]`, including the `EndKey`/`Attach` lookups. Comment at the loop:

```cpp
// EVERY ARRIVING LANE TO EVERY LEAVING LANE, subject to class. Until 2026-09-23 this paired
// guideline N with guideline N over min(count) - right while every road was one centreline,
// wrong the day a road had two lanes: offsets are relative to each segment's own A->B, arms
// meet at mixed ends, so index 0 is the arriving lane on one arm and the leaving lane on the
// next, and a one-lane arm meeting a two-lane arm lost a lane outright. With two one-way
// lanes per arm this emits exactly one turn per arm pair; a bidirectional taxiway still
// emits one, because its one guideline both arrives and leaves.
// ENFORCED BY: Airside.Build.TwoWay.TJunctionTurns, .MixedLaneCounts; Check-Architecture rule 16
```
Skip a pair whose mask intersection is only Emergency when BOTH arms are GroundVehicle-only... no: keep existing behaviour (mask intersection, emitted regardless) - do not change semantics beyond pairing.

Control point:

```cpp
// THE NODE IS THE CONTROL ONLY FOR CENTRELINE GUIDELINES, where both arms' tangent lines
// meet at it. An offset lane's line misses the node, and a node-controlled quadratic would
// leave and join the lanes at an angle. So: the two LANE lines' intersection - and exactly
// the node, bitwise, when both are centred, so no taxiway turn moves.
const double FromOffset = FromProfile->Guidelines[FromWhich].OffsetFor(Network.GetDriveSide());
const double ToOffset   = ToProfile->Guidelines[ToWhich].OffsetFor(Network.GetDriveSide());
Turn.Control = Node->Position;
if (FromOffset != 0.0 || ToOffset != 0.0)
{
	const FVector2D PA = Network.GetGuidelineNode(Turn.A)->Position;
	const FVector2D PB = Network.GetGuidelineNode(Turn.B)->Position;
	FRay2D Arrive; Arrive.Origin = PA; Arrive.Dir = -Network.GetOutgoingTangent(FromSeg, NodeId).GetSafeNormal();
	FRay2D Leave;  Leave.Origin  = PB; Leave.Dir  =  Network.GetOutgoingTangent(ToSeg, NodeId).GetSafeNormal();
	FVector2D Hit;
	const bool bAhead = RoadGeom::LineIntersect(Arrive, Leave, Hit)
		&& FVector2D::DotProduct(Hit - PA, Arrive.Dir) > 0.0
		&& FVector2D::DotProduct(PB - Hit, Leave.Dir) > 0.0;
	// Straight through (parallel lines), or an intersection behind an end: the chord
	// midpoint, which is a straight line and never loops back.
	Turn.Control = bAhead ? Hit : (PA + PB) * 0.5;
}
```
Include `Solve/RoadGeom.h`. Leave the radius-warning block unchanged.

- [ ] **Step 4: Lint rule 16** in `Check-Architecture.ps1`, after rule 15:

```powershell
# --- 16. Turn paths never pair guidelines by index across arms --------------------------------
# Until 2026-09-23 FRoadGuidelineBuilder paired guideline N of one arm with guideline N of the
# next over min(count). Offsets are per segment A->B and arms meet at mixed ends, so that pairs
# an arriving lane with an arriving lane on two-lane roads. The shape it had: a Min over two
# profiles' Guidelines.Num().
$builder = Join-Path $plugin 'Private\Build\RoadGuidelineBuilder.cpp'
foreach ($h in (Select-String -Path $builder -Pattern 'Min\(\s*\w+->Guidelines\.Num\(\)')) {
    $failures.Add("turn-index-pairing: $($builder):$($h.LineNumber) pairs turn-path guidelines by index; pair arriving lanes with leaving lanes instead: $($h.Line.Trim())")
}
```
Verify: temporarily restore the `FMath::Min(` line, run `./Tools/Check-Architecture.ps1`, see the failure, revert.
- [ ] **Step 5: Run `-Filter Airside.Build`; expect PASS for the new tests.**
- [ ] **Step 6: Commit** `feat(build): turn paths join arriving to leaving lanes`.

### Task 5: Dead-end balloon U-turn

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Solve/UTurnGeom.h`, `Private/Solve/UTurnGeom.cpp`
- Modify: `Private/Build/RoadGuidelineBuilder.cpp` (turn loop: node with one arm)
- Test: `UTurnGeomTest.cpp`, `TwoWayLaneTest.cpp` (`DeadEnd`)

**Interfaces:**
```cpp
namespace UTurnGeom
{
	/** One quadratic of the balloon, in travel order: from the previous End (or InEnd) to End. */
	struct FPiece { FVector2D End; FVector2D Control; };

	/** Swing-out height as a multiple of R. 1.5 measured best 2026-09-23 (1.0 needs R 3.8x lock, 2.0 reaches 4.4x). */
	constexpr double HeightFactor = 1.5;

	/**
	 * Six tangent-continuous quadratics from InEnd (travelling +Axis) to OutEnd (travelling
	 * -Axis): a reverse curve out to lateral -R, a half circle of radius R centred on the axis
	 * HeightFactor*R past the ends' midpoint, and the mirror reverse curve back. R grows from
	 * NeededRadius in 3% steps until every piece's GuidelineGeom::TightestRadius >= NeededRadius
	 * (at most 80 steps). Empty if InEnd == OutEnd or Axis is zero.
	 */
	AIRSIDE_API TArray<FPiece> Balloon(const FVector2D& InEnd, const FVector2D& OutEnd,
		const FVector2D& Axis, double NeededRadius, double* OutRadius = nullptr);
}
```

- [ ] **Step 1: Failing Solve test** `Airside.Solve.UTurnBalloon`: In (-150, 0), Out (150, 0), Axis (0,1), Needed 699.
  - 6 pieces; last `End == Out` exactly.
  - Every piece's `GuidelineGeom::TightestRadius(Start, Control, End) >= 699` ("followable by the vehicle it was sized for").
  - Tangent continuity at each joint: direction (End - Control) of piece i vs (Control - End_i) of piece i+1, normalised, dot > 0.9999.
  - First piece leaves In along +Axis: `(Control0 - In)` normalised dot Axis > 0.9999; last piece arrives along -Axis.
  - `OutRadius` in [1000, 1200] (prototype measured 1091 on 2026-09-23).
- [ ] **Step 2: Build twice; run; expect FAIL (no symbol).**
- [ ] **Step 3: Implement** (frame: `O = (In+Out)/2`, `u = Axis.GetSafeNormal()`, `v` = `(Out-In)` minus its u-component, normalised; `o` = half lateral spacing; points in (lateral, along) mapped `O + v*x + u*y`):

```cpp
auto Build = [&](double R) {
	const double H = HeightFactor * R, K = H / 4.0;
	auto P = [&](double X, double Y) { return O + V * X + U * Y; };
	const FVector2D C1 = P(-Half, K), C2 = P(-R, H - K), D2 = P(R, H - K), D1 = P(Half, K);
	TArray<FPiece> Out6;
	Out6.Add({ (C1 + C2) * 0.5, C1 });
	Out6.Add({ P(-R, H), C2 });
	Out6.Add({ P(0.0, H + R), P(-R, H + R) });
	Out6.Add({ P(R, H), P(R, H + R) });
	Out6.Add({ (D2 + D1) * 0.5, D2 });
	Out6.Add({ OutEnd, D1 });
	return Out6;
};
```
Loop R from NeededRadius, `*= 1.03`, measuring each piece with `GuidelineGeom::TightestRadius(Prev, Piece.Control, Piece.End)`. Comment the prototype figures and date.
- [ ] **Step 4: Run; expect PASS.**
- [ ] **Step 5: Failing builder test** `Airside.Build.TwoWay.DeadEnd` (spec test 5 bowser half; Review Focus 3): road (0,0)->(2000,0) two-lane, both ends dead. From the AToB lane's A node, route `GraphProbe` GroundVehicle to the BToA lane's A node (i.e. down the road, round the balloon at B, back): `IsValid()`. Its polyline reaches X > 2000 + 1000 ("the balloon lies past the road end, off the tarmac by ruling"). Also a 2000 uu stub counts: it is the Review Focus case.
- [ ] **Step 6: Implement** in the turn loop, before `for (From...)`:

```cpp
// A DEAD END TURNS VEHICLES ROUND (spec 2026-09-23 §4, ruled: an edge, no mesh). One-way
// lanes would otherwise strand anything that drives into a stub. The balloon is sized for
// the largest service vehicle - the same figure the fillets use - and lies over grass.
// ENFORCED BY: Airside.Build.TwoWay.DeadEnd, Airside.Solve.UTurnBalloon
if (ArmSegments->Num() == 1)
{
	AddDeadEndBalloon(Network, NodeId, (*ArmSegments)[0], Ends, EndKey, LargestServiceVehicle);
	continue;
}
```
`AddDeadEndBalloon` (file-local, in the anonymous namespace; `EndKey` passed as `TFunctionRef<uint64(int32,bool,int32)>`): find the arm's arriving guideline (`bMayArrive` rule) and leaving guideline; skip if either missing or either is Bidirectional (a taxiway stub keeps today's behaviour). `InEnd`/`OutEnd` from `Ends`. `Axis = -GetOutgoingTangent(Seg, NodeId)` (out of the road). `Balloon(..., LargestServiceVehicle.TightestFollowableRadius())`; if empty, `UE_LOG(LogAirside, Warning, TEXT("Dead end at (%.0f,%.0f): no U-turn laid"), ...)` and return. Chain: `Prev = *InEnd`; for pieces 0..4 `Next = AddGuidelineNode(Piece.End)`; last piece ends on `*OutEnd`. Each edge: `A=Prev, B=Next, Control, AllowedTraffic` = both lanes' mask intersection (as turns), `Direction = AToB`, `Width = min lane width`, `bDerived = true`, no `DerivedFrom`.
  Census: add `%d dead-end U-turn(s)` to the `Guidelines:` log line (count balloons laid).
- [ ] **Step 7: Run `-Filter Airside`; expect DeadEnd PASS.**
- [ ] **Step 8: Commit** `feat(build): dead ends turn vehicles round on a balloon`.

### Task 6: Anchors link to both lanes

**Files:** Modify `Private/Build/AnchorLink.cpp` (`Build` loop after `Join`). Test: `TwoWayLaneTest.cpp` (`AnchorBothLanes`).

**Interfaces:** Consumes `FAnchorLink::Join(Network, Link, Hit, AnchorNodes, LargestServiceVehicle)`; `FLinkHit{Edge, Param}`.

- [ ] **Step 1: Failing test** `Airside.Build.TwoWay.AnchorBothLanes` (spec test 8, Review Focus 4): straight two-lane road (0,0)->(20000,0) with junction-free ends; a fuel depot / vehicle pose placed at (10000, 1500) the way `FuelDepotAnchorTest.cpp:90-101` does; `FAnchorLink::Build`. Then: route GroundVehicle from the AToB lane's A node to the pose: valid, and its length < 10000 + 2500 ("no detour round the balloon"); route from the BToA lane's B-end node (travelling -X) to the pose: valid, length < 10000 + 2500. Route from the pose to both far lane ends: valid.
- [ ] **Step 2: Build twice; run; expect the BToA-side route length or validity FAIL.**
- [ ] **Step 3: Implement** after a successful `Join` for a `Link.Class != Aircraft` link:

```cpp
// THE OTHER LANE TOO (spec 2026-09-23 §5). Joining only the nearest guideline was right
// while a road was one line; on a two-lane road it makes the anchor reachable from one
// direction only, and traffic the other way would drive to the next dead end and back. A
// driveway serves both lanes; so does this.
// ENFORCED BY: Airside.Build.TwoWay.AnchorBothLanes
if (Link.Class != ETraversalClass::Aircraft)
{
	const FLinkHit Sibling = FindSiblingLane(Network, Link, Hit, AnchorNodes);
	if (Sibling.IsSet())
	{
		FPendingLink Again = Link;
		Join(Network, Again, Sibling, AnchorNodes, LargestServiceVehicle);
	}
}
```
`FindSiblingLane` (file-local): the hit edge's `DerivedFrom` and `DerivedGuidelineIndex` were copied BEFORE `Join` split it (capture them before calling `Join`); scan live derived edges with the same `DerivedFrom`, a different `DerivedGuidelineIndex`, allowing `Link.Class`, not incident to `AnchorNodes`; nearest by `GuidelineGeom::NearestOnPolyline` over `SampleGuideline`; `Param` via `GuidelineGeom::ParamAtSample`. Read `Join` fully before wiring this: if Join adds the anchor node to `AnchorNodes` or treats `Link.LaneOwner` specially, pass the copy through the same path and keep the count (`++Joined`) unchanged so the census line keeps its meaning.
- [ ] **Step 4: Run `-Filter Airside`; expect PASS.**
- [ ] **Step 5: Commit** `feat(build): anchors join both lanes of a two-way road`.

### Task 7: Drive-side edit, undo and the bar button

**Files:**
- Modify: `Public/Present/RoadEditFacade.h/.cpp` - `bool SetDriveSide(EDriveSide)`.
- Modify: `Public/Present/RoadNetworkActor.h/.cpp` - `UFUNCTION(BlueprintCallable) bool SetDriveSide(EDriveSide)` forwarder, `EDriveSide GetDriveSide() const`.
- Modify: `Source/AirportMgr/BuildActions.cpp` - `game.driveside` action.
- Test: `Plugins/Airside/Source/AirsideTests/Private/DriveSideTest.cpp` (`Composition`), and the BuildActions test if it pins the action count.

- [ ] **Step 1: Failing composition test** `Airside.Present.DriveSide.Composition` (spec test 9): spawn `ARoadNetworkActor` in a test world as `ServiceRoadToolTest.cpp` does; lay a two-lane road with the road tool/ConnectNodes; dispatch a truck along the AToB lane (as `RoadAgentTest`/`AgentRedirectTest` do); `Actor->SetDriveSide(EDriveSide::Left)`; tick until arrival or 60 s; the agent's final position has Y > 0 ("re-planned onto the new side"). `Undo()` returns the side to Right.
- [ ] **Step 2: Build twice; run; expect FAIL.**
- [ ] **Step 3: Implement facade** - pattern of `SetIntermediateHoldingPosition`: guard `Network->GetDriveSide() == Side` -> return false BEFORE the scope; `FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("drive side"));` `Network->SetDriveSide(Side);` `CommitAndNotify(Edit, EChangeKind::Topology);` log `UE_LOG(LogRoadMesh, Log, TEXT("Drive side -> %s, %d road lane(s) re-derived"), ...)` counting live derived edges whose `DerivedFrom` profile has two guidelines. Actor forwarder only (composition root).
- [ ] **Step 4: Bar action** in the Game section beside save/load:

```cpp
// THE DRIVE SIDE, through this table for the reason the fee lever is (CLAUDE.md "Check where
// a list is CONSUMED"). Lit while left-hand. No key: a mis-hit re-lanes the whole airport.
Out.Add(Make(TEXT("game.driveside"), EActionSection::Game, LOCTEXT("DriveLeft", "Drive left"),
	EKeys::Invalid, false,
	[](FBuildActionContext& Ctx)
	{
		if (Ctx.Target != nullptr)
		{
			Ctx.Target->SetDriveSide(Ctx.Target->GetDriveSide() == EDriveSide::Left ? EDriveSide::Right : EDriveSide::Left);
		}
	},
	[](const FBuildActionContext& Ctx) { return Ctx.Target != nullptr && Ctx.Target->GetDriveSide() == EDriveSide::Left; },
	[](const FBuildActionContext& Ctx) { return Ctx.Target != nullptr; }));
```
Match `Make`'s real parameter order and the existing `Never`/`HasRuntime` helper style when writing it.
- [ ] **Step 5: Full build; run `-Filter Airside` and `-Filter AirportMgr`; expect PASS (update any action-count test with the reason).**
- [ ] **Step 6: Commit** `feat(ui): drive side toggle with undo`.

### Task 8: Centre-line paint

**Files:**
- Create: `Public/Build/RoadLaneMarkingBuilder.h`, `Private/Build/RoadLaneMarkingBuilder.cpp`
- Modify: `Private/Present/RoadSurfacePresenter.cpp:224-229` (white paint layer lambda)
- Test: `RoadLaneMarkingTest.cpp`

**Interfaces:**
```cpp
struct AIRSIDE_API FRoadLaneMarkingBuilder
{
	/** Exaggerated from 0.1 m like the holding bars, so it reads from the build camera. Unjudged. */
	static constexpr double LineWidth = 20.0;
	static constexpr double DashLength = 300.0;
	static constexpr double DashGap = 600.0;
	/** Appends a dashed centre line for every live segment whose profile has two opposing lanes. Returns dashes painted. */
	static int32 Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out, int32* OutSegments = nullptr);
};
```

- [ ] **Step 1: Failing test** `Airside.Build.LaneMarking` (spec test 10): straight two-lane road (0,0)->(20000,0), solved. `Build` returns N; quads = `Positions.Num()/4 == N`; each quad's centroid |Y| < 1e-6 ("on the centreline, which the drive side does not move"); all X within [TrimA, 20000 - TrimB]; `N == floor(usable / 900) + (remainder >= 300 ? 1 : 0)`. A taxiway segment paints 0. After `SetDriveSide(Left)` + rebuild, the same N and the same centroids.
- [ ] **Step 2: Build twice; run; expect FAIL.**
- [ ] **Step 3: Implement.** Per live, solved segment with `ProfileFor` having >= 2 guidelines of which one AToB and one BToA: from `A + dir*TrimA` to `B - dir*TrimB`, dashes of `DashLength` every `DashLength+DashGap`, the last clipped to the end; `MarkingQuads::AddRect(Out, Z, Start, Dir, Perp, a0, a1, -LineWidth/2, LineWidth/2)`. Straight segments only (production only lays straight - `RoadNetwork.cpp:60-68`). WHY comment: separate builder, like the runway/holding builders, so paint never welds to the road surface.
- [ ] **Step 4: Presenter** - in the RunwayPaint lambda: `return FRunwayMarkingBuilder::Build(...) + FRoadLaneMarkingBuilder::Build(Network, MarkingZ, OutBuffers, &LaneSegments);` with the comment: "THE WHITE PAINT LAYER, not a component of its own: lane lines are white road paint like the runway's, and a new default subobject would need a level resave (memory: a removed default subobject still renders)." Log `Lane markings: %d dashes on %d segment(s)` beside the runway census (skip when quiet).
- [ ] **Step 5: Run `-Filter Airside`; expect PASS.**
- [ ] **Step 6: Commit** `feat(build): dashed centre line on two-lane roads`.

### Task 9: Whole-suite, asset, look at the map, PR

- [ ] **Step 1:** Full build; `./Tools/Run-AirsideTests.ps1 -Project <worktree>` (all). Fix every fallout in tests that assumed one road guideline; each changed expectation gets a reason naming two lanes.
- [ ] **Step 2:** Run `build_road_profiles.py` headless against the worktree project; grep `MARKER:` for "4 bands, 2 guideline(s)". Confirm the `.uasset` mtime changed (memory `unreal-headless-delete-reports-success`).
- [ ] **Step 3:** Ask the user to open the worktree project, PIE, and screenshot a T-junction with the guideline overlay on, Right then Left; or take it with `Tools/Mcp.py shot x editor` if the editor is up.
- [ ] **Step 4:** Count `UE_LOG(` and comment lines in touched files before/after; push, `gh pr create` with the template filled (build line, test line); body notes: road widens 6.0 -> 7.2 m; no player saves.
