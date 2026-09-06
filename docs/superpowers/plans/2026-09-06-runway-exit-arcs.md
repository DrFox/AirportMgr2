# Runway Exit Arcs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A taxiway meeting a runway gets a tangent exit/entry arc instead of a straight stub into the node, and the landing hands over to the taxi without a speed or heading step.

**Architecture:** Guideline builder only for geometry (§3 of the spec): runway halves split `ExitLength` from a mixed node, taxiway ends set back the same, turn paths attach there. Follower gains an initial speed; the Vacated handover passes the rollout's. Solver, mesh, planner untouched.

**Tech Stack:** UE 5.8.2 C++, Airside plugin, `Run-AirsideTests.ps1`.

**Spec:** `docs/superpowers/specs/2026-09-06-runway-exit-arcs-design.md`

## Global Constraints

- `Model/` never includes Build/Tool/Present; `Build/` may include Model/ and Solve/. `Check-Architecture.ps1` enforces.
- The surface model's bitwise weld is untouched: no solver or mesh-builder edit.
- `GuidelineGeom::Sample` is the one evaluator; tests measure on it.
- Header changes (T1, T4) need a full build with the editor closed; batch them.
- Every `UE_LOG` survives; comments explain WHY; tests assert with a named reason.
- Commits: concise, no Co-Authored-By trailer.

---

### Task 1: `ExitLength` on the profile (header, batched with T4's header)

**Files:** Modify `Plugins/Airside/Source/Airside/Public/Profiles/RoadProfile.h` (after `bContinuousThroughJunctions`).

- [ ] Add, with the spec §2 rationale as its doc comment:
```cpp
	UPROPERTY(EditAnywhere) double ExitLength = 6000.0;
```

### Task 2: Builder arcs (red → green)

**Files:** Create `Plugins/Airside/Source/AirsideTests/Private/RunwayExitArcTest.cpp`; modify `Private/Build/RoadGuidelineBuilder.cpp`.

- [ ] Test `Airside.Build.RunwayExitArc`: runway W(-40000,0)–X(0,0)–E(40000,0), profile `MakeTransient(1800,1500,180)` continuous, `ExitLength = 6000`; taxiway X→T(20000,-20000) at 45° (`MakeTransient(2300,1500,230)`); a second node Y(0,0)... use X only. Solve, build. Find nodes by scanning alive guideline nodes: `S_up` = node at (-6000, 0) ±1, `S_down` at (6000, 0) ±1, `T_end` = node with `Origin == {XT, bEndA=true, 0}` and assert its distance from X is 6000 ±1 along (1,-1)/√2. Find the exit edge: alive, `bDerived`, `!DerivedFrom.IsSet()`, joins `S_up` and `T_end`. Sample it (`GuidelineGeom::Sample`, 16), assert: first tangent within 1° of (1,0) walking from `S_up`; last tangent within 1° of the taxiway direction; max `|VertexHeadings leaving - arriving|` at interior vertices ≤ 12° (16 samples over ~45° means ~3° per vertex; 12° is four times slack and still fails the 45° stub). Assert an entry arc joins `S_down`–`T_end`. Assert the runway through-route: `RouteSearch::Find` from RW1's W-end node to RW2's E-end node is valid and every step's edge has `DerivedFrom` on RW1/RW2 or is a zero-length turn. Second junction at 90°: taxiway X2(-20000,0)... simpler: node P(20000,0) is not a node; add taxiway from E(40000,0)? Use W: taxiway W→Q(-40000,-20000) (90°): assert its arc is tangent too (same measurement). Short arm: taxiway X→Z(4000,-4000) (length ~5657 < 12000): assert `T_end` is 0.45×length ±1 from X and the arc still tangent within 1°. Runway↔runway-only node: build a second network with just W–X–E and assert no node other than the ends and X exist on the strip (no set-back nodes).
- [ ] Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.RunwayExitArc` → FAIL (no `S_up`).
- [ ] Implement in `FRoadGuidelineBuilder::Build`:
  1. Pre-pass over `Solved.NodeResults`: for each valid node, `bContinuous[i]` per arm from `Network.ProfileFor(seg)->bContinuousThroughJunctions`; `bMixed` = any && !all. If mixed: `ExitLength` = max over continuous arms' profiles. For each arm compute `SetBack` = clamp per §3.4 (guideline length = `|CutLinePoint(A) - CutLinePoint(B)|` of the segment at alpha 0.5; taxiway lower bound `Result.Arms[i].CutDistance`). Store `TMap<uint64, double> SetBack` keyed by `EndKey(SegIndex, bEndA, 0)` (guideline index 0; every guideline index of that end uses the same L) and `TSet<uint64> ContinuousEnd`.
  2. Segment loop: for a NON-continuous end with a `SetBack` entry, `AtA`/`AtB` = `NodePos + GetOutgoingTangent(SegmentId, NodeId) * L` instead of the cut-line point.
  3. Segment loop, continuous segment with a `SetBack` at an end: after adding the edge, split it at `T = L / chord` from that end with `GuidelineGeom::Split` (exact on the straight chord): remove, add two edges (both copies of the original with the same `DerivedFrom`), record `Attach[EndKey(Index, bEndA, Which)] = S`. Do end A first, then end B on whichever piece is now adjacent to B.
  4. Junction loop: attachment for an arm at this node = `Attach` entry if the OTHER arm is non-continuous, else `Ends`. (Both continuous → `Ends`, the through-turn as today.)
- [ ] Run → PASS. Run `Airside.Build` and `Airside.Solve` filters → note movers.
- [ ] Commit: `feat(airside): tangent exit and entry arcs where a taxiway meets a runway`.

### Task 3: Hold-short identity and the planner

**Files:** add to `RunwayExitArcTest.cpp`.

- [ ] `Airside.Build.RunwayExitArcHoldShort`: on the T2 fixture, `SetHoldShort(T_end, RW1)`, rebuild (solve+build), find `T_end` again by `Origin`, assert `HoldShortFor == RW1` and position unchanged ±1.
- [ ] `Airside.Model.ArrivalExitAtArcStart`: T2 fixture plus a stand entity reachable from T (use `ArrivalPlannerTest.cpp`'s stand helper or a pose node hand-joined at T's far end). `ArrivalPlanner::Plan` from W: `Exit == S_up`... first exit reaching the stand is `S_up` at 34000 from W; `VacateAt == 34000 ±1`; `TaxiIn.Polyline[0]` within 1 uu of `S_up`.
- [ ] Commit: `test(airside): exit arcs keep the bar and start the taxi-in`.

### Task 4: Follower initial speed and the handover (header batched with T1)

**Files:** `Public/Model/RouteFollower.h`, `Private/Model/RouteFollower.cpp`, `Private/Model/RoadAgent.cpp`, new test in `Plugins/Airside/Source/AirsideTests/Private/TrafficHandoverTest.cpp`.

- [ ] `void Start(const FRoutePlan& InPlan, const FGroundPerformance& InGround, double InitialSpeed = 0.0);` — body: `Speed = FMath::Clamp(InitialSpeed, 0.0, Profile.LimitAt(0.0))` AFTER `Profile.Build`. Doc: why a default of rest (dispatch) and why clamped (a handover never starts above what the route permits).
- [ ] `RoadAgent.cpp` Vacated branch: `Follower.Start(TaxiInPlan, Airframe.Ground, Arrival.Speed);` and update the comment above the `OutMotion = LastMotion` hand-back (it is now conservative, not a glitch hide).
- [ ] Test `Airside.Model.Traffic.VacatedHandoverIsContinuous`: T2 fixture + stand; `UGroundTraffic::DispatchArrival(Net, Near=W-ish, Piper airframe, ...)`; tick at dt = 1/60 until Parked or 600 s; per tick read `FindAgent(Id)->LastMotion` (Position, Heading); derive speed = |ΔPosition|/dt; assert max Δspeed ≤ `max(Landing.Decel, Taxi.Accel, FlareDecel) * dt * 1.5 + 1.0` over ground phases (skip ticks where Altitude > 0) and max Δheading ≤ `MaxTurnRateDegPerSec * dt * 1.5 + 0.01°`; log the worst tick. Red on main (heading step 45°, speed step 800 uu/s).
- [ ] Commit: `feat(airside): the landing hands its speed to the taxi; no step at Vacated`.

### Task 5: Movers, suite, docs, PR

- [ ] Full suite; fix every mover with a one-line justification in the test. Expected: `HeadOnReplansRoundBarHolder` (`Steps[0].To` is now `S`, H is `Steps[1]`), `RunwayExtent`, `ArrivalPlanner` exit counts (+2 strip nodes per junction).
- [ ] Update the M2 handover's follow-ups (bar-to-bar window note), memory, PR body with build line, test line, `UE_LOG` delta (expected +0).
- [ ] `gh pr create` to main.
