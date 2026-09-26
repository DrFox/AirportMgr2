# Articulated Reversing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tow vehicles (rig, utility+trailer) reverse straight and on curves, solved once and played back; a reversing yard on M_RigTest demonstrates a straight bay, a 90 degree bay and a hammerhead; reverse spans draw amber.

**Architecture:** A dependency-free solver (`Solve/TowReverse`) simulates the tractor backing with a hitch-angle controller so the rearmost axle tracks a line; `FTowReverseRun` (Model) plays the samples back inside `EAgentPhase::Reversing`; `VehicleFit::JudgePlan` runs the same solver so the router agrees. `FReverseTurn` records on `URoadNetwork` make the guideline builder derive `bReverseLeg` edge chains. `ARigTestCourse::LayYard` lays a separate yard island with its own runners.

**Tech Stack:** UE 5.8 C++, Airside plugin, automation tests (`Run-AirsideTests.ps1`).

**Spec:** `docs/superpowers/specs/2026-09-26-articulated-reverse-design.md`

## Global Constraints

- Worktree `C:\repos\airportmgr2-articulated-reverse`, branch `feature/articulated-reverse`. Build with `-NoHotReloadFromIDE` only if the editor has ANOTHER checkout open; test script takes `-Project`.
- `Solve/` includes `CoreMinimal.h` and other `Solve/` headers only.
- Rigid vehicles keep `FReverseRun` bit for bit: existing reverse tests unchanged and green.
- Loop runners and every existing `AirportMgr.RigCourse.*` test unchanged and green.
- Tolerances (spec): turntable lock 3 deg; line error 30 uu; final heading 3 deg; final position 20 uu; handover steered-axle-on-exit 20 uu.
- Critical hitch angle DERIVED from body + lock, capped at `VehicleSweep::MaxHitchRadians`.
- New log lines: `LogAirside` (plugin), `LogRoadBuild` with `RigYard:` prefix (course). Every refusal logs its figure and place.
- Comments explain WHY; claims about other code carry `// ENFORCED BY:`.
- `Tool/` names meanings (`EPreviewStyle`), colours only in `PreviewPalette.cpp`.
- Commit per task, messages terse; "unbuilt" if a commit could not be built.

## Review Focus

1. A reverse armed while the forward follower still has speed (arrives at the cusp crawling) - playback must start from rest without a pose jump. Pinned in Task 2 (`TowReverseArmsWithoutJump`).
2. A locked turntable whose towbar is bent 2.9 deg (inside tolerance) - the lock snaps it straight; the view must not show a 3 deg snap as a jump larger than the lock tolerance. Pinned in Task 1 (`LockSnapsWithinTolerance`).
3. A graph rebuild mid-reverse (course `RebuildKeepsTheCourse` pattern) - the playback holds its own samples; the resume must re-resolve the remainder, not re-arm. Pinned in Task 6 (`YardSurvivesRebuild` runs a rebuild while a vehicle is Reversing).
4. The router using a reverse chain as a U-turn shortcut for a vehicle that did not ask for the bay - yard is an island and only yard runners go there; noted, no penalty term this step. Pinned in Task 6 (loop runners' `RouteStaysBounded` unchanged).
5. `JudgePlan` on a plan whose reverse is the LAST span (goal is a bay end) - must judge the reverse and return, not index past the end. Pinned in Task 3 (`WholeRouteJudgesATrailingReverse`).

---

### Task 1: `Solve/TowReverse` - the solver

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Solve/TowReverse.h`
- Create: `Plugins/Airside/Source/Airside/Private/Solve/TowReverse.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/TowReverseSolveTest.cpp`

**Interfaces - Produces:**

```cpp
namespace TowReverse
{
	/** Locked-turntable body: link 0 keeps its joint, links 1.. merged rigidly. */
	AIRSIDE_API VehicleSweep::FBody ReverseBody(const VehicleSweep::FBody& Body);
	/** Largest hitch angle (rad) still recoverable at full lock, <= MaxHitchRadians. */
	AIRSIDE_API double CriticalHitchRadians(const VehicleSweep::FBody& ReverseBody, double MaxSteerRadians);
	/** Steady-state hitch angle for trailer-axle forward curvature Kappa (signed, 1/uu). */
	AIRSIDE_API double SteadyHitchRadians(const VehicleSweep::FBody& ReverseBody, double Kappa);

	enum class ERefusal : uint8 { None, BadLine, OffLine, TurntableBent, Jackknife, MissedEnd };

	struct FSample
	{
		FVector2D Fixed;            // tractor fixed axle
		double Heading = 0.0;       // tractor heading, rad
		TArray<FVector2D, TInlineAllocator<2>> Axles;   // FULL chain (towbar posed on the locked line)
		double SteerDegrees = 0.0;  // atan(W * kappa_tractor), forward sense
		double HitchRadians = 0.0;  // signed, link 0
		double Along = 0.0;         // leading axle distance along the line
	};

	struct FInput
	{
		VehicleSweep::FBody Body;           // FULL chain, as VehicleFit::BodyOf gives it
		double MaxSteerRadians = 0.0;
		TArray<FVector2D> Line;             // leading-axle path, travel order
		FVector2D Fixed; double Heading = 0.0;          // tractor now
		TArray<FVector2D> Axles;            // FULL chain now
	};

	struct FSolution
	{
		ERefusal Refusal = ERefusal::None;
		double Figure = 0.0;                // the number the refusal names (uu or deg)
		FVector2D Where = FVector2D::ZeroVector;
		TArray<FSample> Samples;
		double WorstHitchRadians = 0.0;
		double EndPositionError = 0.0, EndHeadingErrorDegrees = 0.0;
		bool IsValid() const { return Refusal == ERefusal::None && Samples.Num() >= 2; }
		FString Describe() const;
	};

	AIRSIDE_API FSolution Solve(const FInput& In);

	inline constexpr double TurntableLockDegrees = 3.0;
	inline constexpr double MaxLineError = 30.0;
	inline constexpr double MaxEndHeadingDegrees = 3.0;
	inline constexpr double MaxEndPositionError = 20.0;
}
```

**Algorithm** (write it with these equations, unit vectors via `VehicleSweep`'s `Perp` convention):

- `ReverseBody`: link0 kept; `Length += sum_{k>=1}(Length_k - HitchX_k)`; `BodyRear` = last link's; `BodyFront` = link0's (towbar is a bar, so take max(link0.BodyFront, link1.BodyFront - (Len0 - HitchX1)) ); `Width` = max.
- Hitch angle `phi = unwind(theta - psi)`, `psi` = atan2 of `normalize(Hitch - Axle)`.
- Tractor model per step of fixed-axle travel `ds < 0`: `F += h*ds`, `theta += ds * kappaF`. Chain: `VehicleSweep::StepChain(ReverseBody, F, h, Axle0)` (projection is direction-agnostic).
- Controller: reverse travel sigma = -s. `dphi/dsigma = -kappaF (1 - a cos(phi)/L) + sin(phi)/L` (a = HitchX, L = Length of reverse link). Want `dphi/dsigma = -K (phi - phiRef)`, `K = 1/(0.5 L)`. So `kappaF = (K (phi - phiRef) + sin(phi)/L) / (1 - a cos(phi)/L)`, clamp `|kappaF| <= tan(lock)/W`.
- `phiRef`: pure pursuit of the trailer axle in its travel direction `d = -g`: look-ahead point on the line (extended straight past its end by `Ld`), `Ld = max(1.0 * L, 300)`; `kappaD = 2 sin(alpha)/Ld` signed by cross(d, Q - T); forward trailer curvature `kappaT = -kappaD`; `phiRef = SteadyHitchRadians(body, kappaT)` clamped to `0.8 * critical`.
- `SteadyHitchRadians`: `Rt = 1/|k|`, `Rf = sqrt(Rt^2 + L^2 - a^2)`, `phi = sign(k) (atan(L/Rt) - atan(a/Rf))`; k = 0 -> 0.
- `CriticalHitchRadians`: largest phi in [0, MaxHitchRadians] with `sin(phi) + a*kMax*cos(phi) <= L*kMax` (scan 0.1 deg), where `kMax = tan(lock)/W`.
- Start: refuse `BadLine` if line < 2 points or length < 1; `TurntableBent` if any link k >= 1 is more than 3 deg to its puller; project the leading axle onto the line (`GuidelineGeom::NearestOnPolyline`): `OffLine` if > 30.
- Loop, step 10 uu fixed-axle travel, max steps `4 * (lineLength + 2 * chain) / 10`: jackknife if `|phi| > critical` (refuse `Jackknife`, Where = hitch); line error > 30 -> `OffLine`; stop when leading-axle projection `Along >= lineLength`. End errors vs line end and reversed end tangent; refuse `MissedEnd` past tolerance.
- Samples: FULL chain axles - link 0 axle on the locked line from the merged axle: towbar axle = `Hitch - g * Len0`, body axle = merged axle.

- [ ] **Step 1: Write failing tests** in `TowReverseSolveTest.cpp` (`Airside.Solve.TowReverse.*`), bodies from `FVehicle` figures copied literally with a comment naming `ResolveRigVehicle`/`ResolveUtilityTowVehicle` (rig: W 370, lock 40, link HitchX 57.3 L 1029.5; utility: W 149.3, lock 45, links (-90.7,114 bar),(0,221)):
  - `ReverseBodyMergesTheTurntable`: utility -> 1 link, HitchX -90.7, Length 335; rig unchanged.
  - `SteadyStateHolds`: for kappaT = 1/2000, integrate forward 2000 uu at phi = Steady; phi drift < 0.1 deg.
  - `StraightIsExact`: both bodies straight on a straight 3000 uu line: every sample's axles on the line within 0.01 uu, steer 0, valid.
  - `ArcTrackedWithinTolerance`: line = straight 2000 retracing + 90 deg arc R 1500 + straight 2500 (rig), R 800 (utility): valid, max line error < 30, end errors inside tolerance.
  - `TooTightIsRefused`: rig, arc R 300 -> refusal Jackknife or OffLine, never a valid solution; Describe() names a figure.
  - `BentTurntableIsRefused`: utility towbar at 5 deg -> TurntableBent, Figure ~5.
  - `LockSnapsWithinTolerance`: towbar at 2.9 deg -> valid; first sample's towbar axle within `Len0 * sin(3 deg)` of the input.
  - `NeverJackknives`: every valid solution above keeps |HitchRadians| < critical.
- [ ] **Step 2: Build (twice - new test file), run `-Filter Airside.Solve.TowReverse`**, expect link/compile failure then FAIL.
- [ ] **Step 3: Implement** header + cpp per the algorithm.
- [ ] **Step 4: Run tests; tune `K`, `Ld` only if ArcTracked fails - record final values with date in the header comment.**
- [ ] **Step 5: Commit** `feat(solve): TowReverse - solved reverse for a tow chain, turntable locked`.

### Task 2: `FTowReverseRun` + `FRoadAgent` integration

**Files:**
- Create: `Public/Model/TowReverseRun.h`, `Private/Model/TowReverseRun.cpp`
- Modify: `Public/Model/RoadAgent.h` (member `FTowReverseRun TowReverse;`), `Private/Model/RoadAgent.cpp` (`TryArmReverseLeg`, `Reversing` branch)
- Test: `Plugins/Airside/Source/AirsideTests/Private/TowReverseAgentTest.cpp`

**Interfaces:**
- Consumes: `TowReverse::Solve`, `VehicleFit::BodyOf`, `VehicleFit::FixedAxleAt`.
- Produces:
```cpp
USTRUCT() struct AIRSIDE_API FTowReverseRun
{
	GENERATED_BODY()
	UPROPERTY() FRoutePlan Plan;
	UPROPERTY() double ReverseSpeed = 0.0;
	UPROPERTY() double Speed = 0.0;
	UPROPERTY() double SteerDegrees = 0.0;
	double Along = 0.0;                               // leading axle, uu
	TArray<TowReverse::FSample> Samples;              // not reflected: rebuilt by Start
	bool Start(const FRoutePlan& InPlan, const FVehicle& Vehicle, const FVector2D& Origin, double Heading,
		TArrayView<const FVector2D> Axles, double InReverseSpeed, FString* OutReason = nullptr);
	bool Advance(double DeltaSeconds, double StopWithin, FVector2D& OutOrigin, double& OutHeading,
		TArray<FVector2D>& OutAxles);
	bool HasArrived() const;
	const TowReverse::FSample* Last() const;
	bool IsArmed() const { return Samples.Num() >= 2; }
	void Reset();
};
```
- `Advance` contract = `FReverseRun::Advance` post-#297: false (and nothing moved) once arrived; true on the frame that reaches the end.
- `FRoadAgent`: `Vehicle.HasTrailer()` selects `TowReverse` over `Reverse` in arm, branch and `DescribeMotion` (`GroundSpeed = -TowReverse.Speed`, steer from `TowReverse.SteerDegrees`). Handover: `Follower.Start(Remainder, Chassis(), 0.0, Last()->Heading)`; `Follower.Travelled` = projection of the final steered axle (`Fixed + h * Wheelbase`) onto `Remainder.Polyline` (NearestOnPolyline -> distance), clamped. `TowAxles` = `Last()->Axles`. Log `Backing (tow): %.0f uu at %.0f uu/s, worst hitch %.0f deg.` and refusal `Reverse leg refused - %s` with `Describe()`.

- [ ] **Step 1: Failing tests** (`Airside.Model.TowReverse.*`), world-free `FRoadAgent` driven by hand-built `FRoutePlan` (pattern: `RoadAgentTest.cpp` `ReverseLastLegParksAtRest`): straight approach 4000 east, reverse leg back 2500 west then 90 deg arc into a stub, forward exit.
  - `TowReverseMovesTheChain`: during Reversing, `TowAxles[last]` moves every frame (> 0.5 uu at 0.05 s).
  - `TowReverseArmsWithoutJump`: pose (origin, heading, every axle) on the arming frame within 1 uu / 0.5 deg of the previous frame.
  - `TowReverseHandoverHasNoJump`: same across the reverse->forward frame; `JackknifedLink` stays INDEX_NONE for the whole run.
  - `TowReverseRefusalStalls`: an arc R 300 -> stays Taxiing, speed 0, warning logged once per attempt window (spy `Containing("Reverse leg refused")` >= 1).
  - `RigidStillUsesReverseRun`: bowser on the same plan -> `Reverse.IsArmed`-equivalent (Reverse.Plan valid), TowReverse not armed.
- [ ] **Step 2: Build twice, run, FAIL.**
- [ ] **Step 3: Implement `FTowReverseRun`** (interpolate samples by `Along`, linear on positions, `Lerp` of unwound headings; `Speed = dAlong/dt`).
- [ ] **Step 4: Wire `FRoadAgent`.** Keep every existing comment; add WHY comments at the fork ("chosen by the vehicle, not a flag").
- [ ] **Step 5: Run `Airside.Model` filter - all existing reverse tests still green.**
- [ ] **Step 6: Commit** `feat(model): FTowReverseRun - tow chains reverse by playback; chain no longer freezes`.

### Task 3: `VehicleFit` judges reverses

**Files:**
- Modify: `Public/Model/VehicleFit.h` (`EFitRefusal::ReverseUnsolvable`, `FFitVerdict::Reason` FString, header comment at h:192), `Private/Model/VehicleFit.cpp` (`Judge`: return width verdict only for `Edge.bReverseLeg`; `JudgePlan`: segment loop)
- Test: `Plugins/Airside/Source/AirsideTests/Private/WholeRouteTowTest.cpp` (append)

**Shape of `JudgePlan`:** split the existing drive body into `static FFitVerdict JudgeForwardSection(const FRoutePlan&, const FVehicle&, const URoadNetwork&, FTowSeed-like state in/out)` returning the final follower pose, speed and axles. Loop: forward section -> if reverse run follows: `TowReverse::Solve` from that state on the reverse run's polyline; invalid -> `ReverseUnsolvable` (Reason = Describe, At = Where, Edge = first reverse step); valid -> trace every sample's `BodyCorners` against the reverse span's measured clearances when present (skip when none - derived reverse edges carry none); then next forward section seeded from `Last()` (Travelled = projected steered axle, as Task 2). `Describe()` covers the new refusal.

- [ ] **Step 1: Failing tests** (`Airside.Model.Tow.*`, plans built on a tiny `URoadNetwork` via `TestGraph` helpers used by neighbours in the file):
  - `WholeRouteJudgesTheReverse`: rig, reverse into R 1500 bay -> fits; same layout R 300 -> `ReverseUnsolvable`.
  - `WholeRouteJudgesATrailingReverse`: plan ends on the reverse span -> judged, no crash.
  - `WholeRouteReverseAgreesWithAgent`: run the agent on the admitted plan to the end; no refusal logged, no jackknife.
  - `JudgeSaysNothingForAReverseEdge`: per-edge `Judge` on a `bReverseLeg` edge with MinRadius 50 -> None.
- [ ] **Step 2: Build, run, FAIL.** **Step 3: Implement.** **Step 4: Run `Airside.Model.Tow` + `Airside.Model` - green.**
- [ ] **Step 5: Commit** `feat(model): VehicleFit judges tow reverses with the playback solver`.

### Task 4: `FReverseTurn` - builder-derived reverse legs

**Files:**
- Create: `Public/Model/ReverseTurn.h` (USTRUCT `FReverseTurn { UPROPERTY FRoadNodeId Node; FRoadSegmentId FromArm; FRoadSegmentId IntoArm; }`)
- Modify: `Public/Model/RoadNetwork.h/.cpp` (`AddReverseTurn`, `GetReverseTurns`, `RemoveReverseTurn(int32)`, transient `ReverseTurnEnds` + `GetReverseTurnEnd(int32)`, `CopyFrom` copies the array), `Public/Tool/RoadEditTarget.h` (`virtual bool AddReverseTurn(FRoadNodeId Node, FRoadSegmentId FromArm, FRoadSegmentId IntoArm) = 0;`) + every implementer (`URoadEditFacade`, `ARoadNetworkActor` forwarder, test doubles - grep `: public IRoadEditTarget`), `Private/Build/RoadGuidelineBuilder.cpp` (new pass after turn paths: `BuildReverseTurns`), `Private/Model/VehicleFit.cpp` untouched.
- Test: `Plugins/Airside/Source/AirsideTests/Private/ReverseTurnBuildTest.cpp`

**Derivation (per record; drop + Warning `Reverse turn at (%.0f,%.0f) dropped: %s` if Node/arm dead or arms not both at Node):**
- `P` = FromArm's far node. Lane `LA` = FromArm guideline travelling N->P; start node = its lane end at P (Ends map, `EndKey(FromArm.Index, FromArm.A == P, Which)`).
- Pull-past check: FromArm length >= `DesignVehicles.ForTier(tier).ChainLength() + FilletTangent + 200`; else drop with the figure (`pull-past %.0f < %.0f`). `ChainLength` = steered axle to rearmost axle straight (add to `FRoadDesignVehicles` entry if absent, computed from `FVehicle`).
- Target lane `LB` = IntoArm guideline travelling far->N. Reverse line: from P along LA's line back toward N; fillet `ReverseFilletRadius = 1500` (dated constant, clamped to the room both lines give) onto LB's line (`GuidelineGeom::Arc`, `BendArcPieceSweep`); straight along LB's line to `E` = IntoArm far end - (design vehicle rearmost overhang + 50) along LB. Opposite arms: LA and LB collinear -> one straight.
- Edges: straight pieces control = midpoint; arc pieces from `Arc`. Each `bReverseLeg = true`, `bDerived = true`, `Direction = AToB`, `Width = min lane widths`, `MinRadius = 0` (reverse is judged by the solver, not the forward lock), traffic = GroundVehicle + Emergency. Interior nodes new.
- Exit edge `E` -> LB's lane end at N, straight, forward, derived.
- `ReverseTurnEnds[i] = E`.

- [ ] **Step 1: Failing tests** (`Airside.Build.ReverseTurn.*`, fixture `FAirsideTestWorld`, Wide service roads):
  - `EdgeIsFlaggedAndStartsAtTheStop` (90 deg stub): chain from P's lane end to E, all `bReverseLeg`, route P->E exists and uses them.
  - `StraightBayIsOneLine`: opposite arms -> every reverse vertex on LA's line within 0.5 uu.
  - `PullPastHoldsTheChain`: FromArm 800 uu -> dropped, Warning names both figures.
  - `SurvivesRebuild`: rebuild twice -> same end position, fresh handles resolve.
  - `DeadArmDropsTheRecord`: delete IntoArm -> record gone, warning.
  - `UndoCopiesTheRecord`: `CopyFrom` copies `ReverseTurns`.
- [ ] **Step 2: Build twice, run, FAIL.** **Step 3: Implement.** **Step 4: Run `Airside.Build` - green, incl. rename `WideDeadEndRefusesRigUntilReversing` -> `WideDeadEndRefusesRig` (update the ENFORCED BY lines naming it).**
- [ ] **Step 5: Commit** `feat(build): FReverseTurn - reverse legs derived at junctions`.

### Task 5: Reverse spans in their own colour

**Files:**
- Modify: `Public/Tool/RoadBuildTool.h` (`EPreviewStyle::ReverseRoute`, `ReverseGuideline`), `Private/Present/PreviewPalette.cpp` (colour amber-orange (1.0, 0.55, 0.1) at Route weight; dim (0.6, 0.4, 0.15) at Guideline weight), `Public/Present/AirsideTraffic.h/.cpp` (`RemainingRouteSpans`), `Private/Tool/SelectTool.cpp:176`, `Private/Tool/GuidelineOverlay.cpp` (style `bReverseLeg` edges), `Source/AirportMgr/RoadBuildHUD.cpp:24`.
- Test: `Plugins/Airside/Source/AirsideTests/Private/RouteSpansTest.cpp`

**Interfaces:**
```cpp
struct FRouteSpan { TArray<FVector2D> Points; bool bReverse = false; };
TArray<FRouteSpan> UAirsideTraffic::RemainingRouteSpans(int32 AgentId) const;
```
During Reversing (tow) the reverse span's points = remaining samples' leading axles; rigid = remaining reverse polyline.

- [ ] **Step 1: Failing tests:** `Airside.Tool.RouteSpansSplitAtReverse` (forward/reverse/forward, ends meet within 0.01); `Airside.Present.EveryPreviewStyleIsDrawn` (loop `StaticEnum<EPreviewStyle>()`, each has a palette entry distinct from the fallback - the HUD half lives in the game module, so add `AirportMgr.HUD.EveryPreviewStyleListed` beside `RoadBuildHUD`'s list, comparing names).
- [ ] **Step 2: Build, FAIL. Step 3: Implement. Step 4: Green.**
- [ ] **Step 5: Commit** `feat(ui): reverse spans draw amber in the route view and the guideline overlay`.

### Task 6: Reversing yard on M_RigTest

**Files:**
- Modify: `Source/AirportMgr/RigTestCourse.h/.cpp` (`FRigYardLayout` constants, `LayYard`, `FRigYardRunner`, `TickYardRunner`, logs), `Source/AirportMgr/RigTestCourseTest.cpp` (yard tests; confirm existing counts unchanged)
- Create: `docs/superpowers/plans/2026-09-26-rig-yard.png` (to-scale drawing, from `Tools/Python/draw_rig_yard.py`)

**Layout** (uu, road-plane; all Wide service roads; an island - no link to the loop):
```
 W stub  NW(-2000,-7000)---P1(3000)---J(7000)---P2(10500)---NE(13000,-7000)
 (-5000)  |                           |bay90 (7000,-9500)        |
          |                                                      S(13000,-10000)---H(16000)---D(19500,-10000)
          |                                                      |                 |hammer (16000,-12500)
         SW(-2000,-13000)--------------------------------------SE(13000,-13000)
```
- Reverse turns: straight `(NW, NW->P1, NW->W)`; 90 `(J, J->P2, J->bay90)`; hammer `(H, H->D, H->hammer)`.
- Runner loop per vehicle, goals = reverse-turn ends: straight -> 90 -> hammer -> straight; each leg `RouteSearch::Find(PlayerIssued, from, GetReverseTurnEnd(i), GroundVehicle).WithVehicle(V)`; dispatch at SW's northbound lane end; each goal reached = agent Parked after reverse -> `RedirectAgent` to the next goal. Utility starts 20 s after the rig.
- Logs: `RigYard: %s %s - armed, worst hitch %.0f deg` on entering Reversing; `RigYard: %s %s - in, end error %.0f uu / %.1f deg` on Parked; `RigYard: %s %s - REFUSED: %s`.
- Yard legs expect Reversing; loop-runner warning unchanged.

- [ ] **Step 1: Draw the yard to scale** with balloon reach (27 m) and check nothing crosses the return road (Y -4000) or the floor (Y -17000). Show it in the PR.
- [ ] **Step 2: Failing tests** `AirportMgr.RigCourse.YardHeadless` (every reverse armed, 0 jack-knifed, end error <= 20 uu / 3 deg, both vehicles 3 features, chain continuous across handovers: axle jump < 1 uu per tick beyond travel), `AirportMgr.RigCourse.YardSurvivesRebuild` (rebuild while rig Reversing; completes).
- [ ] **Step 3: Implement.** **Step 4: Run `AirportMgr.RigCourse` - yard green, every existing test unchanged.**
- [ ] **Step 5: Commit** `feat(rigtest): reversing yard - straight bay, 90 degree bay, hammerhead`.

### Task 7: Whole branch

- [ ] Full `Build.bat` (worktree), `Check-Architecture.ps1`, `Run-AirsideTests.ps1 -Project <worktree>` - quote `N test(s) run, N failed, N crashed`.
- [ ] PIE on M_RigTest (worktree editor, MCP port 8001 per memory): `Mcp.py shot` of each feature, `RigYard:` lines from the log.
- [ ] Update spec "Out of scope/Open" with anything found. Push, PR with build/test lines, UE_LOG and comment deltas.
