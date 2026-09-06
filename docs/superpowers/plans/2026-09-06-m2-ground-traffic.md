# Milestone 2: Ground Traffic — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two aircraft on one field no longer pass through each other; an aircraft holds at a player-placed hold-short bar while the runway is in use; vehicles queue, yield to aircraft, route round jams, and a gridlock resolves or clears when the player builds a way out.

**Architecture:** One reservation table (`FTrafficOccupancy`) in Airside `Model/`, driven by a new `Model/` Mediator `UGroundTraffic` that owns the agents and a priority-ordered two-pass tick (claim, then advance). `UAirsideTraffic` (`Present/`) shrinks to a view registry and forwarders. `RouteSearch` gains an occupancy cost and a banned edge; `FRouteFollower` gains a stop-within cap. Hold-short is player-placed with a new tool and persisted by identity so it survives the guideline rebuild.

**Tech Stack:** UE 5.8.2 C++, UnrealBuildTool, automation tests (`IMPLEMENT_SIMPLE_AUTOMATION_TEST`), `Tools/Run-AirsideTests.ps1`, `Tools/Check-Architecture.ps1`.

**Spec:** `docs/superpowers/specs/2026-09-06-ground-traffic-design.md` (all sections). Systems map `2026-09-05-game-systems-map-design.md` §3.8, already amended.

## Global Constraints

- Layering (`Check-Architecture.ps1`): `Model/` never includes `Build/|Tool/|Present/|Entities/`; `Tool/` never includes `Present/`; `Build/` never includes `Present/|Tool/`. Tests (`AirsideTests`) may include anything.
- Airside never references AirportOps.
- One log category per name per module. Airside `Model/` traffic logs use `LogAirsideTraffic` (declared in `Public/AirsideLog.h`). Test files that log define their own category.
- Every existing `UE_LOG(` in `Plugins/Airside/Source/Airside` survives. Baseline count: **71** (`grep -rc "UE_LOG(" Plugins/Airside/Source/Airside/Private Plugins/Airside/Source/Airside/Public | awk -F: '{s+=$2} END {print s}'`). It may rise, never fall.
- Comment-line count in touched files may not fall. Comments say WHY and name the rejected alternative.
- Every new model field is a `UPROPERTY()`. Runtime-only state on `UGroundTraffic`/`UAirsideTraffic` is `UPROPERTY(Transient)`.
- Every public name on `UAirsideTraffic` and every `IRoadEditTarget` virtual stays reachable at its old signature (a forwarder if the body moved).
- Unity build: anonymous-namespace helpers in test files are prefixed per file (`M2Occ`, `M2Traffic`, …). No two test files define the same helper name.
- Tests assert behaviour with a reason string, and MEASURE (a distance, a count, a tick) rather than narrate.
- Red first: run each new test on the unfixed tree and quote the failure in the commit message or the task notes.
- **The editor must be CLOSED for every build in this plan** (new headers, UPROPERTYs, UCLASSes). Build line:
  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
  ```
  If the editor is open and nothing is unsaved (check `git status Content/` and the log for save lines), close it yourself (`CloseMainWindow()` on the `UnrealEditor` process, wait) and say so.
- Test line: `./Tools/Run-AirsideTests.ps1` (default filter `Airside+AirportOps+AirportMgr`). Read its `N test(s) run, N failed, N crashed` line; crashed must be 0. Narrow with `-Filter Airside.Model.Traffic` while iterating; the full run is mandatory before each commit's push and before the PR.
- Baseline before Task 1: **88 tests, 0 failed, 0 crashed**.
- Commit messages: concise, no `Co-Authored-By` trailer. Branch `feature/m2-ground-traffic` (exists; spec is committed on it).
- Nothing parallelises: one task at a time, each ending in a full build and a test run.

---

## File map

**New**

| File | Responsibility |
|---|---|
| `Plugins/Airside/Source/Airside/Public/Model/TrafficOccupancy.h` / `Private/Model/TrafficOccupancy.cpp` | `FTrafficResource`, `FTrafficClaim`, `FTrafficOccupancy` — the table |
| `Plugins/Airside/Source/Airside/Public/Model/GroundTraffic.h` / `Private/Model/GroundTraffic.cpp` | `FTrafficRules`, `UGroundTraffic` — agents, tick, claims, deadlock, rebuild |
| `Plugins/Airside/Source/Airside/Public/Tool/HoldShortTool.h` / `Private/Tool/HoldShortTool.cpp` | `FHoldShortTool` |
| `Plugins/Airside/Source/AirsideTests/Private/TrafficOccupancyTest.cpp` | `Airside.Model.Occupancy.*` |
| `Plugins/Airside/Source/AirsideTests/Private/RouteStepDistanceTest.cpp` | `Airside.Model.RouteSearch.EndDistance`, `.Splice` |
| `Plugins/Airside/Source/AirsideTests/Private/FollowerStopWithinTest.cpp` | `Airside.Model.Follower.StopWithin` |
| `Plugins/Airside/Source/AirsideTests/Private/RunwayChainTest.cpp` | `Airside.Model.RunwayChain`, `Airside.Model.ArrivalPlanner.RunwayOccupied` |
| `Plugins/Airside/Source/AirsideTests/Private/GroundTrafficTest.cpp` | `Airside.Model.Traffic.*` (NodeYield, PriorityOverride, CarFollowing, HeadOn, HoldShort, DeadlockTriangle, GraphRebuild, ArrivalRefusedRunwayOccupied) |
| `Plugins/Airside/Source/AirsideTests/Private/TrafficForwardersTest.cpp` | `Airside.Present.TrafficForwarders` |
| `Plugins/Airside/Source/AirsideTests/Private/HoldShortMarkTest.cpp` | `Airside.Build.HoldShortSurvivesRebuild` |
| `Plugins/Airside/Source/AirsideTests/Private/HoldShortToolTest.cpp` | `Airside.Tool.HoldShort` |

**Modified**

| File | Change |
|---|---|
| `Public/Model/RouteSearch.h` / `Private/Model/RouteSearch.cpp` | `FRouteStep::EndDistance/EndVertex`; `FRouteQuery::Occupancy/QueryingAgent/CongestionWeight/BannedEdge`; cost term; `RouteSearch::Splice` |
| `Public/Model/RouteFollower.h` / `Private/Model/RouteFollower.cpp` | `Advance(Delta, StopWithin, …)`; old overload forwards; `Replace(Plan)` |
| `Public/Model/RoadAgent.h` / `Private/Model/RoadAgent.cpp` | new fields; `Advance` passes `StopWithin` |
| `Public/Model/RoadNetwork.h` / `Private/Model/RoadNetwork.cpp` | `IsRunwaySegment`, `RunwayChain`, `OutSegment` on runway queries, `RunwayNearGuidelineNode`, `HoldShortMarks`, `SetHoldShort` |
| `Public/Model/RoadGuideline.h` | `FHoldShortMark` |
| `Public/Model/ArrivalPlanner.h` / `Private/Model/ArrivalPlanner.cpp` | `RunwaySegment`, `RunwayChain`, `RunwayOccupied`, optional occupancy |
| `Public/Present/AirsideTraffic.h` / `Private/Present/AirsideTraffic.cpp` | registry + forwarders over `UGroundTraffic` |
| `Public/Present/RoadNetworkActor.h` / `Private/Present/RoadNetworkActor.cpp` | `Tick` passes network; `RebuildMesh` notifies traffic; `DispatchAgent` class param; `SetHoldShort` forwarder |
| `Public/Tool/RoadEditTarget.h`, `Public/Present/RoadEditFacade.h` / `Private/Present/RoadEditFacade.cpp` | `DispatchAgent(…, Class)`; `SetHoldShort` |
| `Public/Tool/RoadBuildTool.h` | `EPreviewStyle::HoldShort` |
| `Private/Tool/GuidelineOverlay.cpp` | hold bars |
| `Private/Tool/BuildSession.cpp` | registry entry, key Eight |
| `Private/Tool/RouteTool.cpp` | passes `Class` to `DispatchAgent` |
| `Private/Build/RoadGuidelineBuilder.cpp` | re-applies hold-short marks |
| `Source/AirportMgr/RoadBuildHUD.h` / `.cpp` | `HoldShortColour` |
| `Plugins/Airside/Source/AirsideEditor/Public/RoadBuildEdModeCommands.h` / `Private/RoadBuildEdModeCommands.cpp`, `Private/RoadBuildEditorTool.cpp` | `PlaceHoldShort` command; colour |
| `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp` | none expected (binds `GetTraffic()->OnAgentPhaseChanged`, which stays) — verify it compiles |

All Airside paths below are relative to `Plugins/Airside/Source/Airside/` unless they start with `AirsideTests/`, `Source/`, or `Plugins/`.

---

### Task 1: Route step distances, follower stop-within, plan splice

**Files:**
- Modify: `Public/Model/RouteSearch.h` (`FRouteStep`, `RouteSearch` namespace)
- Modify: `Private/Model/RouteSearch.cpp` (`RunSearch` polyline loop, lines ~195-228; new `Splice`)
- Modify: `Public/Model/RouteFollower.h`, `Private/Model/RouteFollower.cpp`
- Create: `AirsideTests/Private/RouteStepDistanceTest.cpp`, `AirsideTests/Private/FollowerStopWithinTest.cpp`

**Interfaces:**
- Produces `FRouteStep::EndDistance` (double, cumulative route distance where the step's edge ends) and `FRouteStep::EndVertex` (int32, index into `FRoutePlan::Polyline` of that point). For step 0, start is distance 0 / vertex 0.
- Produces `FRoutePlan RouteSearch::Splice(const FRoutePlan& Head, int32 KeepSteps, const FRoutePlan& Tail)`.
- Produces `bool FRouteFollower::Advance(double DeltaSeconds, double StopWithin, FVector2D& OutPosition, double& OutHeading)`; the existing 3-arg `Advance` forwards with `TNumericLimits<double>::Max()`. Produces `void FRouteFollower::Replace(const FRoutePlan& NewPlan)`.

- [ ] **Step 1: Write the failing tests**

`AirsideTests/Private/RouteStepDistanceTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the unity build: RouteSearchTest already owns Join().
	// (TrafficOccupancy.h is Task 2's; until then the include is absent and Task 7 adds it.)
	FGuidelineEdgeId M2StepJoin(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B,
		const FVector2D* Control = nullptr)
	{
		const FGuidelineNode* NodeA = Net.GetGuidelineNode(A);
		const FGuidelineNode* NodeB = Net.GetGuidelineNode(B);
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = Control ? *Control : (NodeA->Position + NodeB->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::All();
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteStepEndDistanceTest,
	"Airside.Model.RouteSearch.EndDistance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteStepEndDistanceTest::RunTest(const FString& Parameters)
{
	// A straight, a BEND, a straight - the bend is what makes EndDistance a measurement
	// rather than a sum of chords: it must equal the sampled polyline's length up to the
	// step's end vertex, because that is the array the follower walks.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
	const FGuidelineNodeId C = Net->AddGuidelineNode(FVector2D(2000.0, 1000.0));
	const FGuidelineNodeId D = Net->AddGuidelineNode(FVector2D(2000.0, 3000.0));
	M2StepJoin(*Net, A, B);
	const FVector2D Bend(2000.0, 0.0);
	M2StepJoin(*Net, B, C, &Bend);
	// Reversed on purpose: D->C is the stored direction, the route walks C->D.
	M2StepJoin(*Net, D, C);

	FRouteQuery Query;
	Query.Start = A;
	Query.Goal = D;
	Query.Class = ETraversalClass::Aircraft;
	const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
	if (!TestTrue(TEXT("route found"), Plan.IsValid())) { return false; }
	if (!TestEqual(TEXT("three steps"), Plan.Steps.Num(), 3)) { return false; }

	TestEqual(TEXT("step 0 ends at vertex 1 (a straight is two points)"), Plan.Steps[0].EndVertex, 1);
	TestEqual(TEXT("step 0 ends at 1000 uu"), Plan.Steps[0].EndDistance, 1000.0, 1e-6);

	// The bend's EndDistance must be the polyline length to its end vertex, EXACTLY the
	// sum the follower will walk - not the Bezier's true arc length, not the chord.
	TArray<FVector2D> UpToC;
	for (int32 At = 0; At <= Plan.Steps[1].EndVertex; ++At) { UpToC.Add(Plan.Polyline[At]); }
	TestEqual(TEXT("step 1 EndDistance is the sampled polyline length to its end vertex"),
		Plan.Steps[1].EndDistance, GuidelineGeom::PolylineLength(UpToC), 1e-6);
	TestTrue(TEXT("the bend is longer than its chord, so the measurement is not the chord"),
		Plan.Steps[1].EndDistance > 1000.0 + FVector2D::Distance(FVector2D(1000.0, 0.0), FVector2D(2000.0, 1000.0)) + 1.0);

	TestEqual(TEXT("the last step ends at the route's own length"), Plan.Steps[2].EndDistance, Plan.Length, 1e-6);
	TestEqual(TEXT("and at the last vertex"), Plan.Steps[2].EndVertex, Plan.Polyline.Num() - 1);
	TestTrue(TEXT("a reversed step still ends where the route arrives"),
		FVector2D::Distance(Plan.Polyline[Plan.Steps[2].EndVertex], FVector2D(2000.0, 3000.0)) < 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteSpliceTest,
	"Airside.Model.RouteSearch.Splice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteSpliceTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
	const FGuidelineNodeId C = Net->AddGuidelineNode(FVector2D(2000.0, 0.0));
	const FGuidelineNodeId X = Net->AddGuidelineNode(FVector2D(1000.0, 1500.0));
	M2StepJoin(*Net, A, B);
	M2StepJoin(*Net, B, C);
	M2StepJoin(*Net, B, X);
	M2StepJoin(*Net, X, C);

	FRouteQuery Q; Q.Start = A; Q.Goal = C; Q.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Head = RouteSearch::Find(*Net, Q);
	FRouteQuery T; T.Start = B; T.Goal = C; T.Class = ETraversalClass::GroundVehicle;
	T.BannedEdge = Head.Steps[1].Edge;   // forbid B->C, so the tail goes via X
	const FRoutePlan Tail = RouteSearch::Find(*Net, T);
	if (!TestTrue(TEXT("tail routes round the ban"), Tail.IsValid() && Tail.Steps.Num() == 2)) { return false; }

	const FRoutePlan Spliced = RouteSearch::Splice(Head, 1, Tail);
	TestTrue(TEXT("spliced plan is valid"), Spliced.IsValid());
	TestEqual(TEXT("keeps one head step and both tail steps"), Spliced.Steps.Num(), 3);
	TestEqual(TEXT("starts where the head started"), Spliced.Start, Head.Start);
	TestEqual(TEXT("length is head-to-B plus the tail"), Spliced.Length, 1000.0 + Tail.Length, 1e-6);
	// EndDistance stays monotone and consistent with the polyline: the follower and the
	// claims read the same numbers, so a splice that shifted one and not the other would
	// stop an agent for a node it had already passed.
	for (int32 Index = 0; Index < Spliced.Steps.Num(); ++Index)
	{
		TArray<FVector2D> Prefix;
		for (int32 At = 0; At <= Spliced.Steps[Index].EndVertex; ++At) { Prefix.Add(Spliced.Polyline[At]); }
		TestEqual(FString::Printf(TEXT("step %d EndDistance matches its polyline prefix"), Index),
			Spliced.Steps[Index].EndDistance, GuidelineGeom::PolylineLength(Prefix), 1e-6);
	}
	TestTrue(TEXT("no duplicated weld point at the splice"),
		FVector2D::Distance(Spliced.Polyline[1], Spliced.Polyline[2]) > 1.0);
	return true;
}

#endif
```

`AirsideTests/Private/FollowerStopWithinTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RouteFollower.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFollowerStopWithinTest,
	"Airside.Model.Follower.StopWithin",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFollowerStopWithinTest::RunTest(const FString& Parameters)
{
	FRoutePlan Plan;
	Plan.Result = ERouteResult::Found;
	Plan.Polyline = { FVector2D(0.0, 0.0), FVector2D(20000.0, 0.0) };
	Plan.Length = 20000.0;

	FGroundPerformance Ground;   // Accel 100, Decel 200, SpeedCap 1000 - the struct defaults
	FRouteFollower Follower;
	Follower.Start(Plan, Ground);

	// Run up to cruise with nothing ahead.
	FVector2D At; double Heading = 0.0;
	for (int32 Tick = 0; Tick < 400; ++Tick) { Follower.Advance(0.05, At, Heading); }
	if (!TestTrue(TEXT("at cruise before the stop is asked for"), Follower.Speed > 990.0)) { return false; }

	// A stop point 3000 uu ahead, held fixed in ROUTE distance across ticks the way the
	// arbiter will hold it: the follower must come to rest short of it, never past it, and
	// never braking harder than the airframe has.
	const double StopAt = Follower.Travelled + 3000.0;
	double MaxDecelSeen = 0.0;
	double LastSpeed = Follower.Speed;
	for (int32 Tick = 0; Tick < 400; ++Tick)
	{
		const double StopWithin = FMath::Max(0.0, StopAt - Follower.Travelled);
		Follower.Advance(0.05, StopWithin, At, Heading);
		MaxDecelSeen = FMath::Max(MaxDecelSeen, (LastSpeed - Follower.Speed) / 0.05);
		LastSpeed = Follower.Speed;
		TestTrue(TEXT("never passes the stop point"), Follower.Travelled <= StopAt + 1e-6);
	}
	TestTrue(TEXT("has stopped"), Follower.Speed < 1e-6);
	TestTrue(FString::Printf(TEXT("stopped within 1 uu of the stop point (at %.1f of %.1f)"), Follower.Travelled, StopAt),
		StopAt - Follower.Travelled < 1.0);
	TestTrue(FString::Printf(TEXT("braked no harder than Decel (%.1f <= 200)"), MaxDecelSeen), MaxDecelSeen <= 200.0 + 1e-6);

	// Released: it goes again, from rest, at the airframe's acceleration - not at cruise.
	Follower.Advance(0.05, TNumericLimits<double>::Max(), At, Heading);
	TestTrue(TEXT("resumes from rest at Accel, not by snapping to cruise"), Follower.Speed > 0.0 && Follower.Speed <= 100.0 * 0.05 + 1e-6);
	return true;
}

#endif
```

- [ ] **Step 2: Build to see them fail**

Expected: `EndVertex`, `EndDistance`, `BannedEdge`, `Splice` and the 4-arg `Advance` undeclared.

- [ ] **Step 3: `FRouteStep` and `FRouteQuery`**

In `Public/Model/RouteSearch.h`, inside `FRouteStep` after `bReversed`:
```cpp
	/**
	 * Cumulative route distance at which this step's edge ends, and the index of that point
	 * in FRoutePlan::Polyline. Filled by RunSearch from the SAME polyline it appends - never
	 * from the Bezier - so "which edge am I on at Travelled" is answered off the array the
	 * follower walks. A step map derived from the curve would disagree on every bend, and
	 * the arbiter would then stop an agent for a node it had already crossed.
	 */
	UPROPERTY() double EndDistance = 0.0;
	UPROPERTY() int32 EndVertex = 0;
```
Add `struct FTrafficOccupancy;` forward declaration above `FRouteQuery` (defined in Task 2; the forward declaration keeps this task building). Inside `FRouteQuery` after `Wingspan`:
```cpp
	/**
	 * An edge the search may not use. Set by a deadlock replan to forbid the edge the agent
	 * was refused. One edge, not a set: the resolver bans exactly the thing it is stuck on
	 * and lets the occupancy cost steer round the rest.
	 */
	UPROPERTY() FGuidelineEdgeId BannedEdge;

	/**
	 * Who holds what, for the congestion cost term - or null for a plain shortest route,
	 * which is bitwise the search this class ran before occupancy existed. A raw pointer
	 * rather than a UPROPERTY: a query lives on the stack for one call and the table it
	 * reads outlives it.
	 */
	const FTrafficOccupancy* Occupancy = nullptr;

	/** The agent asking, so its own claims do not cost it. 0 when nobody is. */
	UPROPERTY() int32 QueryingAgent = 0;

	/** Weight on held length. Ignored when Occupancy is null. */
	UPROPERTY() double CongestionWeight = 2.0;
```
In the `RouteSearch` namespace after `Find`:
```cpp
	/**
	 * Head's first KeepSteps steps followed by all of Tail, welded at the node Tail starts
	 * from. Precondition: Tail.Start is the node Head's KeepSteps-th step arrives at (or
	 * Head.Start when KeepSteps is 0). EndDistance and EndVertex are re-based so the
	 * follower and the arbiter keep reading one set of numbers. Returns an invalid plan
	 * when the precondition fails, because splicing two lines that do not meet would put a
	 * jump in the polyline the agent would then drive across.
	 */
	AIRSIDE_API FRoutePlan Splice(const FRoutePlan& Head, int32 KeepSteps, const FRoutePlan& Tail);
```

- [ ] **Step 4: Fill `EndDistance`/`EndVertex`, honour `BannedEdge`, write `Splice`**

In `Private/Model/RouteSearch.cpp`, in `RunSearch`'s outgoing-edge loop, immediately after the `Edge == nullptr || Edge->A == Edge->B` check:
```cpp
				if (Query.BannedEdge.IsSet() && EdgeId == Query.BannedEdge)
				{
					continue;
				}
```
Replace the polyline loop (`for (const FRouteStep& Step : Plan.Steps)`) with an index loop that writes back:
```cpp
		Plan.Polyline.Add(StartNode->Position);
		for (int32 Index = 0; Index < Plan.Steps.Num(); ++Index)
		{
			FRouteStep& Step = Plan.Steps[Index];
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Step.Edge);
			if (Edge == nullptr)
			{
				continue;
			}

			TArray<FVector2D> Points;
			if (!EdgePoints(Network, *Edge, Points))
			{
				continue;
			}

			if (Step.bReversed)
			{
				Algo::Reverse(Points);
			}

			for (int32 At = 1; At < Points.Num(); ++At)
			{
				Plan.Polyline.Add(Points[At]);
			}

			// Measured off the array just appended, not off Points: the two are the same
			// numbers today, and reading the plan's own polyline is what keeps them the same
			// if the weld rule above ever changes.
			Step.EndVertex = Plan.Polyline.Num() - 1;
			Step.EndDistance = GuidelineGeom::PolylineLength(Plan.Polyline);
		}
```
Add to the `RouteSearch` namespace:
```cpp
	FRoutePlan Splice(const FRoutePlan& Head, int32 KeepSteps, const FRoutePlan& Tail)
	{
		FRoutePlan Out;
		Out.Result = ERouteResult::Unreachable;
		if (!Head.IsValid() || !Tail.IsValid() || KeepSteps < 0 || KeepSteps > Head.Steps.Num()
			|| Tail.Polyline.Num() < 2)
		{
			return Out;
		}

		const FGuidelineNodeId JoinNode = KeepSteps == 0 ? Head.Start : Head.Steps[KeepSteps - 1].To;
		if (JoinNode != Tail.Start)
		{
			return Out;
		}

		const int32 JoinVertex = KeepSteps == 0 ? 0 : Head.Steps[KeepSteps - 1].EndVertex;
		const double JoinDistance = KeepSteps == 0 ? 0.0 : Head.Steps[KeepSteps - 1].EndDistance;

		Out.Result = ERouteResult::Found;
		Out.Start = Head.Start;
		for (int32 At = 0; At <= JoinVertex; ++At)
		{
			Out.Polyline.Add(Head.Polyline[At]);
		}
		for (int32 Index = 0; Index < KeepSteps; ++Index)
		{
			Out.Steps.Add(Head.Steps[Index]);
		}

		// The tail's first point IS the join node, so it is dropped - the same weld rule
		// RunSearch applies between consecutive edges.
		for (int32 At = 1; At < Tail.Polyline.Num(); ++At)
		{
			Out.Polyline.Add(Tail.Polyline[At]);
		}
		for (const FRouteStep& Step : Tail.Steps)
		{
			FRouteStep Rebased = Step;
			Rebased.EndVertex += JoinVertex;
			Rebased.EndDistance += JoinDistance;
			Out.Steps.Add(Rebased);
		}
		Out.Length = GuidelineGeom::PolylineLength(Out.Polyline);
		return Out;
	}
```

- [ ] **Step 5: Follower `StopWithin` and `Replace`**

In `Public/Model/RouteFollower.h`, replace the `Advance` declaration with:
```cpp
	/**
	 * Moves forward by DeltaSeconds and reports where that leaves the agent.
	 *
	 * StopWithin is the ONE input the traffic model adds (spec 3.8): the distance, from
	 * where the agent is at the start of this tick, beyond which it may not go. It becomes
	 * a third cap on the target speed - sqrt(2 a s), the same shape the profile's own
	 * braking curve has - and a clamp on Travelled, so a long frame cannot carry the agent
	 * through a node it was told to hold at. Unbounded means today's behaviour exactly.
	 *
	 * False when there is no valid route to walk, leaving the outputs untouched - so a
	 * caller that ignores the return value leaves its agent where it was rather than
	 * teleporting it to the origin, which is this project's most-repeated bug.
	 */
	bool Advance(double DeltaSeconds, double StopWithin, FVector2D& OutPosition, double& OutHeading);

	/** Advance with nothing ahead. Kept so every caller and test from before the traffic
	 *  model reads exactly as it did. */
	bool Advance(double DeltaSeconds, FVector2D& OutPosition, double& OutHeading)
	{
		return Advance(DeltaSeconds, TNumericLimits<double>::Max(), OutPosition, OutHeading);
	}

	/**
	 * Swaps the plan under a MOVING agent, keeping Travelled, Speed and Heading.
	 *
	 * Start() is for a dispatch: it resets to rest at the polyline's first point. A replan
	 * spliced at a node the agent has not reached yet must not do that - the agent is part
	 * way along a line that is unchanged up to the splice, so only the profile is rebuilt.
	 */
	void Replace(const FRoutePlan& NewPlan);
```
In `Private/Model/RouteFollower.cpp`, change the definition to the 4-arg form and edit the body:
```cpp
bool FRouteFollower::Advance(double DeltaSeconds, double StopWithin, FVector2D& OutPosition, double& OutHeading)
{
	if (!Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		return false;
	}

	// The stop point in route distance, fixed BEFORE the move: StopWithin was measured from
	// where the agent was when the arbiter looked, and re-measuring it after moving would
	// let the agent creep past it one frame at a time.
	const double StopAt = FMath::Min(Plan.Length, Travelled + FMath::Max(0.0, StopWithin));

	// Clamped rather than allowed to run on, so a long frame - a hitch, or a breakpoint -
	// leaves the agent at its destination instead of somewhere past the end of the world.
	//
	// Speed is LAST frame's, decided at the bottom of this function. One frame of lag, 16 ms
	// at the rate this is watched at, and it buys the whole loop a single PointAtDistance
	// call: reading the line, deciding a speed and then moving would need two, one before
	// the move and one after, on every agent on the airport.
	Travelled = FMath::Clamp(Travelled + Speed * DeltaSeconds, 0.0, StopAt);
```
Keep everything from `double LineHeading = 0.0;` through `CrabLimit` unchanged, then replace the `Target` line with:
```cpp
	// The THIRD cap: what the stop point permits. sqrt(2 a s), the braking curve, so the
	// agent arrives at the stop at rest having braked at the rate it actually has - the
	// profile already does this for corners and the destination; this does it for whatever
	// the arbiter put in the way this tick. Zero distance is zero speed, which is a stop.
	const double StopCap = FMath::Sqrt(2.0 * Ground.Taxi.Decel * FMath::Max(0.0, StopAt - Travelled));

	const double Target = FMath::Min3(Profile.LimitAt(Travelled), CrabLimit, StopCap);
```
Add:
```cpp
void FRouteFollower::Replace(const FRoutePlan& NewPlan)
{
	Plan = NewPlan;
	Travelled = FMath::Clamp(Travelled, 0.0, Plan.Length);
	Profile.Build(Plan.Polyline, Ground);
}
```
Update the class comment's "WHAT IS STILL NOT HERE" paragraph: it now reads that the follower takes one input from the traffic model (`StopWithin`) and still knows nothing about WHO is ahead - that is `UGroundTraffic`'s job. Do not delete the paragraph; rewrite it.

- [ ] **Step 6: Build, run, commit**

Build. Run `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model`. Expected: the three new tests pass; `Airside.Model.RouteSearch`, `Airside.Model.TurnRate`, `Airside.Model.RoadAgent.*` unchanged. Then the full run: 91 tests, 0 failed, 0 crashed.
```bash
git add -A Plugins/Airside && git commit -m "feat(airside): route step distances, follower stop-within cap, plan splice"
```

---

### Task 2: `FTrafficOccupancy` — the table

**Files:**
- Create: `Public/Model/TrafficOccupancy.h`, `Private/Model/TrafficOccupancy.cpp`
- Create: `AirsideTests/Private/TrafficOccupancyTest.cpp`

**Interfaces:**
- Produces (all in `Model/TrafficOccupancy.h`):
```cpp
UENUM() enum class ETrafficResourceKind : uint8 { Edge, Node, Surface };
USTRUCT() struct AIRSIDE_API FTrafficResource {
    UPROPERTY() ETrafficResourceKind Kind = ETrafficResourceKind::Node;
    UPROPERTY() FGuidelineEdgeId Edge; UPROPERTY() FGuidelineNodeId Node; UPROPERTY() FRoadSegmentId Surface;
    static FTrafficResource OfEdge(FGuidelineEdgeId); static FTrafficResource OfNode(FGuidelineNodeId); static FTrafficResource OfSurface(FRoadSegmentId);
    bool operator==(const FTrafficResource&) const; FString Describe() const; };
USTRUCT() struct AIRSIDE_API FTrafficClaim {
    UPROPERTY() int32 AgentId = 0; UPROPERTY() FTrafficResource Resource;
    UPROPERTY() double From = 0.0; UPROPERTY() double To = 0.0;   // edge distance, Edge kind only
    UPROPERTY() bool bOccupied = false; UPROPERTY() int32 Rank = 0;
    bool Conflicts(const FTrafficClaim& Other) const; };
UENUM() enum class EClaimResult : uint8 { Granted, Held };
USTRUCT() struct AIRSIDE_API FTrafficOccupancy {
    EClaimResult TryClaim(const FTrafficClaim& Claim, FTrafficClaim& OutBlocker);
    void ReleaseAll(int32 AgentId);
    void ReleaseExcept(int32 AgentId, const TArray<FTrafficResource>& Keep);
    double HeldLengthOn(FGuidelineEdgeId Edge, int32 ExcludingAgent) const;
    bool IsHeld(const FTrafficResource& Resource, int32 ExcludingAgent, int32* OutHolder = nullptr) const;
    const TArray<FTrafficClaim>& GetClaims() const { return Claims; }
    TSet<int32> TakePreempted();
    void Clear();
private: UPROPERTY() TArray<FTrafficClaim> Claims; TSet<int32> Preempted; };
```
- Semantics of `TryClaim`: a claim by the same agent on the same resource REPLACES its old claim (Granted). Otherwise every conflicting claim is examined: if any is `bOccupied`, or has `Rank >= Claim.Rank`, the result is `Held` with that claim in `OutBlocker` and nothing changes. Else all conflicting claims are removed, their agents added to `Preempted`, the claim is added, `Granted`. Two edge claims conflict when `From < Other.To && Other.From < To` (half-open, so touching intervals do not conflict). Node and Surface claims on the same id always conflict.

- [ ] **Step 1: Write the failing test**

`AirsideTests/Private/TrafficOccupancyTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/TrafficOccupancy.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FGuidelineEdgeId M2OccEdge(int32 Index) { FGuidelineEdgeId Id; Id.Index = Index; Id.Generation = 1; return Id; }
	FGuidelineNodeId M2OccNode(int32 Index) { FGuidelineNodeId Id; Id.Index = Index; Id.Generation = 1; return Id; }

	FTrafficClaim M2OccEdgeClaim(int32 Agent, int32 Edge, double From, double To, bool bOccupied, int32 Rank)
	{
		FTrafficClaim C;
		C.AgentId = Agent; C.Resource = FTrafficResource::OfEdge(M2OccEdge(Edge));
		C.From = From; C.To = To; C.bOccupied = bOccupied; C.Rank = Rank;
		return C;
	}
	FTrafficClaim M2OccNodeClaim(int32 Agent, int32 Node, bool bOccupied, int32 Rank)
	{
		FTrafficClaim C;
		C.AgentId = Agent; C.Resource = FTrafficResource::OfNode(M2OccNode(Node));
		C.bOccupied = bOccupied; C.Rank = Rank;
		return C;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficOccupancyClaimsTest,
	"Airside.Model.Occupancy.Claims",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficOccupancyClaimsTest::RunTest(const FString& Parameters)
{
	FTrafficOccupancy Table;
	FTrafficClaim Blocker;

	// 1. Disjoint intervals on one edge coexist - that is a queue.
	TestEqual(TEXT("agent 1 reserves [0,1000)"), Table.TryClaim(M2OccEdgeClaim(1, 7, 0.0, 1000.0, true, 2), Blocker), EClaimResult::Granted);
	TestEqual(TEXT("agent 2 reserves [1000,2000) behind it - touching is not overlapping"), Table.TryClaim(M2OccEdgeClaim(2, 7, 1000.0, 2000.0, false, 2), Blocker), EClaimResult::Granted);
	TestEqual(TEXT("two claims held"), Table.GetClaims().Num(), 2);

	// 2. Overlap is refused and names the holder.
	TestEqual(TEXT("agent 3 wants [500,1500) - held"), Table.TryClaim(M2OccEdgeClaim(3, 7, 500.0, 1500.0, false, 2), Blocker), EClaimResult::Held);
	TestEqual(TEXT("the blocker named is agent 1, the occupied one found first"), Blocker.AgentId, 1);
	TestEqual(TEXT("a refused claim adds nothing"), Table.GetClaims().Num(), 2);

	// 3. Rank preempts a RESERVATION, never an OCCUPANCY.
	TestEqual(TEXT("rank 3 preempts agent 2's reservation"), Table.TryClaim(M2OccEdgeClaim(4, 7, 1200.0, 1800.0, false, 3), Blocker), EClaimResult::Granted);
	TestEqual(TEXT("rank 3 cannot preempt agent 1, who is standing there"), Table.TryClaim(M2OccEdgeClaim(4, 7, 200.0, 400.0, false, 3), Blocker), EClaimResult::Held);
	{
		const TSet<int32> Preempted = Table.TakePreempted();
		TestTrue(TEXT("agent 2 is reported preempted, once"), Preempted.Num() == 1 && Preempted.Contains(2));
		TestEqual(TEXT("and the report clears on read"), Table.TakePreempted().Num(), 0);
	}

	// 4. Equal rank: the holder keeps it (first-to-reserve).
	TestEqual(TEXT("agent 5 at rank 3 cannot take agent 4's rank-3 reservation"), Table.TryClaim(M2OccEdgeClaim(5, 7, 1200.0, 1300.0, false, 3), Blocker), EClaimResult::Held);
	TestEqual(TEXT("blocker is agent 4"), Blocker.AgentId, 4);

	// 5. Re-claiming your own resource updates it rather than conflicting with yourself.
	TestEqual(TEXT("agent 1 extends its own interval"), Table.TryClaim(M2OccEdgeClaim(1, 7, 0.0, 1100.0, true, 2), Blocker), EClaimResult::Granted);
	TestEqual(TEXT("still one claim for agent 1 on that edge"), Table.GetClaims().FilterByPredicate([](const FTrafficClaim& C) { return C.AgentId == 1; }).Num(), 1);

	// 6. Nodes are exclusive whatever the numbers say.
	TestEqual(TEXT("agent 1 takes node 9"), Table.TryClaim(M2OccNodeClaim(1, 9, false, 2), Blocker), EClaimResult::Granted);
	TestEqual(TEXT("agent 2 cannot"), Table.TryClaim(M2OccNodeClaim(2, 9, false, 2), Blocker), EClaimResult::Held);
	int32 Holder = 0;
	TestTrue(TEXT("IsHeld sees node 9 held by someone other than agent 2"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(9)), 2, &Holder) && Holder == 1);
	TestFalse(TEXT("but not held by anyone other than agent 1"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(9)), 1));

	// 7. HeldLengthOn sums OTHER agents' intervals - the routing cost's input.
	TestEqual(TEXT("edge 7 as seen by a stranger: 1100 + 600"), Table.HeldLengthOn(M2OccEdge(7), 0), 1700.0, 1e-9);
	TestEqual(TEXT("edge 7 as seen by agent 1 excludes its own 1100"), Table.HeldLengthOn(M2OccEdge(7), 1), 600.0, 1e-9);

	// 8. Release.
	Table.ReleaseExcept(1, { FTrafficResource::OfNode(M2OccNode(9)) });
	TestEqual(TEXT("agent 1 keeps only the node"), Table.GetClaims().FilterByPredicate([](const FTrafficClaim& C) { return C.AgentId == 1; }).Num(), 1);
	Table.ReleaseAll(1);
	TestFalse(TEXT("after ReleaseAll node 9 is free"), Table.IsHeld(FTrafficResource::OfNode(M2OccNode(9)), 0));
	Table.Clear();
	TestEqual(TEXT("Clear empties the table"), Table.GetClaims().Num(), 0);
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail** — `Model/TrafficOccupancy.h` not found.

- [ ] **Step 3: Write the header**

`Public/Model/TrafficOccupancy.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "TrafficOccupancy.generated.h"

/** What kind of thing an agent can hold. See FTrafficResource. */
UENUM()
enum class ETrafficResourceKind : uint8
{
	/** A guideline edge, held as a distance INTERVAL along it - a queue, not a block. */
	Edge,
	/** A guideline node. Exclusive: junctions and crossings are the conflict points. */
	Node,
	/** One segment of a runway. A landing holds every segment of the chain; a hold-short
	 *  node names one and the arbiter expands it. */
	Surface,
};

/**
 * One thing an agent can hold: an edge, a node, or a runway segment.
 *
 * A tagged struct rather than three tables, so the runway grant M3's sequencer will ask
 * for (spec 3.8: "runway occupancy is one more surface in the same table") is the same
 * TryClaim a junction uses - one rule for a taxiway crossing a runway and for a landing.
 * TVariant was rejected: it is not UHT-reflectable, and every claim has to be a UPROPERTY
 * so the table can live on a UObject the collector traces.
 */
USTRUCT()
struct AIRSIDE_API FTrafficResource
{
	GENERATED_BODY()

	UPROPERTY() ETrafficResourceKind Kind = ETrafficResourceKind::Node;
	UPROPERTY() FGuidelineEdgeId Edge;
	UPROPERTY() FGuidelineNodeId Node;
	UPROPERTY() FRoadSegmentId Surface;

	static FTrafficResource OfEdge(FGuidelineEdgeId Id);
	static FTrafficResource OfNode(FGuidelineNodeId Id);
	static FTrafficResource OfSurface(FRoadSegmentId Id);

	bool operator==(const FTrafficResource& Other) const;
	bool operator!=(const FTrafficResource& Other) const { return !(*this == Other); }

	/** "edge 12", "node 4", "runway segment 2" - for the log lines. */
	FString Describe() const;
};

/**
 * One agent's hold on one resource.
 *
 * OCCUPIED versus RESERVED is the whole arbitration rule (spec 3.3). A claim that contains
 * the agent's own position is occupied and can never be taken away - nobody is evicted from
 * a node they are standing in. Anything ahead is a reservation, and a higher rank preempts
 * it. Rank is decided by the caller (class priority, or the node's PriorityOverride) so the
 * table never has to know what a class is.
 */
USTRUCT()
struct AIRSIDE_API FTrafficClaim
{
	GENERATED_BODY()

	UPROPERTY() int32 AgentId = 0;
	UPROPERTY() FTrafficResource Resource;

	/** Edge kind only: the interval held, in edge distance from A. Half-open [From, To). */
	UPROPERTY() double From = 0.0;
	UPROPERTY() double To = 0.0;

	UPROPERTY() bool bOccupied = false;
	UPROPERTY() int32 Rank = 0;

	/**
	 * True when the two cannot both stand. Half-open on edges, so a queue that packs
	 * intervals end to end is not told it is colliding with itself.
	 */
	bool Conflicts(const FTrafficClaim& Other) const;
};

UENUM()
enum class EClaimResult : uint8
{
	Granted,
	/** Refused; the blocking claim is handed back so the caller can stop short of it. */
	Held,
};

/**
 * Who holds which guideline edge interval, node and runway surface, and what each has
 * reserved a short way ahead. Spec 3.8's FTrafficOccupancy; spec 2026-09-06 §2.1.
 *
 * MECHANISM, NOT POLICY. It grants and refuses by the one rule in TryClaim and knows
 * nothing about braking distances, classes or routes - UGroundTraffic decides what to ask
 * for and in what order, and URunwaySequencer (M3, AirportOps) will decide who is next
 * before asking here for the runway. A table that knew about aircraft would have to grow
 * with every rule anyone added to traffic.
 *
 * A flat array searched linearly rather than a map per kind: an airport has tens of agents
 * holding a handful of claims each, and the whole table is rebuilt in one pass per tick.
 * Measured before it is indexed.
 */
USTRUCT()
struct AIRSIDE_API FTrafficOccupancy
{
	GENERATED_BODY()

	/**
	 * Grant if nothing occupied or of equal-or-higher rank conflicts; else Held, with the
	 * first blocker in OutBlocker. A same-agent claim on the same resource is an UPDATE,
	 * never a conflict. Preempted agents are recorded for TakePreempted, so the caller can
	 * re-run their claim pass in the same tick rather than let them drive one frame on a
	 * reservation they no longer have.
	 */
	EClaimResult TryClaim(const FTrafficClaim& Claim, FTrafficClaim& OutBlocker);

	void ReleaseAll(int32 AgentId);

	/** Drops every claim by AgentId whose resource is not in Keep. The per-tick "release
	 *  what is behind me" in one call, so first-to-reserve survives across ticks: an agent
	 *  that released everything and re-claimed would be a stranger to its own queue. */
	void ReleaseExcept(int32 AgentId, const TArray<FTrafficResource>& Keep);

	/** Sum of (To - From) over claims on Edge by agents other than ExcludingAgent. */
	double HeldLengthOn(FGuidelineEdgeId Edge, int32 ExcludingAgent) const;

	/** True when someone other than ExcludingAgent holds Resource (any interval). */
	bool IsHeld(const FTrafficResource& Resource, int32 ExcludingAgent, int32* OutHolder = nullptr) const;

	const TArray<FTrafficClaim>& GetClaims() const { return Claims; }

	/** Agents whose reservation was removed by a preemption since the last call; clears. */
	TSet<int32> TakePreempted();

	void Clear();

private:
	UPROPERTY() TArray<FTrafficClaim> Claims;

	/** Not a UPROPERTY: consumed within the tick that produced it. */
	TSet<int32> Preempted;
};
```

- [ ] **Step 4: Write the body**

`Private/Model/TrafficOccupancy.cpp`:
```cpp
#include "Model/TrafficOccupancy.h"

FTrafficResource FTrafficResource::OfEdge(FGuidelineEdgeId Id)
{
	FTrafficResource R; R.Kind = ETrafficResourceKind::Edge; R.Edge = Id; return R;
}
FTrafficResource FTrafficResource::OfNode(FGuidelineNodeId Id)
{
	FTrafficResource R; R.Kind = ETrafficResourceKind::Node; R.Node = Id; return R;
}
FTrafficResource FTrafficResource::OfSurface(FRoadSegmentId Id)
{
	FTrafficResource R; R.Kind = ETrafficResourceKind::Surface; R.Surface = Id; return R;
}

bool FTrafficResource::operator==(const FTrafficResource& Other) const
{
	if (Kind != Other.Kind) { return false; }
	switch (Kind)
	{
	case ETrafficResourceKind::Edge:    return Edge == Other.Edge;
	case ETrafficResourceKind::Node:    return Node == Other.Node;
	case ETrafficResourceKind::Surface: return Surface == Other.Surface;
	default: return false;
	}
}

FString FTrafficResource::Describe() const
{
	switch (Kind)
	{
	case ETrafficResourceKind::Edge:    return FString::Printf(TEXT("edge %d"), Edge.Index);
	case ETrafficResourceKind::Node:    return FString::Printf(TEXT("node %d"), Node.Index);
	case ETrafficResourceKind::Surface: return FString::Printf(TEXT("runway segment %d"), Surface.Index);
	default: return TEXT("?");
	}
}

bool FTrafficClaim::Conflicts(const FTrafficClaim& Other) const
{
	if (Resource != Other.Resource)
	{
		return false;
	}
	if (Resource.Kind != ETrafficResourceKind::Edge)
	{
		return true;
	}
	// Half-open: a queue packed end to end is not in conflict with itself.
	return From < Other.To && Other.From < To;
}

EClaimResult FTrafficOccupancy::TryClaim(const FTrafficClaim& Claim, FTrafficClaim& OutBlocker)
{
	// An update of my own hold on this resource. Found first so a re-claim can never be
	// refused by the claim it is replacing.
	for (FTrafficClaim& Existing : Claims)
	{
		if (Existing.AgentId == Claim.AgentId && Existing.Resource == Claim.Resource)
		{
			Existing = Claim;
			return EClaimResult::Granted;
		}
	}

	TArray<int32> ToPreempt;
	for (int32 Index = 0; Index < Claims.Num(); ++Index)
	{
		const FTrafficClaim& Existing = Claims[Index];
		if (Existing.AgentId == Claim.AgentId || !Existing.Conflicts(Claim))
		{
			continue;
		}
		// Occupied is absolute. Equal rank keeps the holder - that IS first-to-reserve.
		if (Existing.bOccupied || Existing.Rank >= Claim.Rank)
		{
			OutBlocker = Existing;
			return EClaimResult::Held;
		}
		ToPreempt.Add(Index);
	}

	// Every conflict was a lower-ranked reservation: take them all, and say whose.
	for (int32 At = ToPreempt.Num() - 1; At >= 0; --At)
	{
		Preempted.Add(Claims[ToPreempt[At]].AgentId);
		Claims.RemoveAtSwap(ToPreempt[At]);
	}
	Claims.Add(Claim);
	return EClaimResult::Granted;
}

void FTrafficOccupancy::ReleaseAll(int32 AgentId)
{
	Claims.RemoveAllSwap([AgentId](const FTrafficClaim& C) { return C.AgentId == AgentId; });
}

void FTrafficOccupancy::ReleaseExcept(int32 AgentId, const TArray<FTrafficResource>& Keep)
{
	Claims.RemoveAllSwap([AgentId, &Keep](const FTrafficClaim& C)
	{
		return C.AgentId == AgentId && !Keep.Contains(C.Resource);
	});
}

double FTrafficOccupancy::HeldLengthOn(FGuidelineEdgeId Edge, int32 ExcludingAgent) const
{
	double Sum = 0.0;
	for (const FTrafficClaim& C : Claims)
	{
		if (C.AgentId != ExcludingAgent && C.Resource.Kind == ETrafficResourceKind::Edge && C.Resource.Edge == Edge)
		{
			Sum += FMath::Max(0.0, C.To - C.From);
		}
	}
	return Sum;
}

bool FTrafficOccupancy::IsHeld(const FTrafficResource& Resource, int32 ExcludingAgent, int32* OutHolder) const
{
	for (const FTrafficClaim& C : Claims)
	{
		if (C.AgentId != ExcludingAgent && C.Resource == Resource)
		{
			if (OutHolder != nullptr) { *OutHolder = C.AgentId; }
			return true;
		}
	}
	return false;
}

TSet<int32> FTrafficOccupancy::TakePreempted()
{
	TSet<int32> Out = MoveTemp(Preempted);
	Preempted.Reset();
	return Out;
}

void FTrafficOccupancy::Clear()
{
	Claims.Reset();
	Preempted.Reset();
}
```
Note: `TryClaim`'s preempt loop removes higher indices first with `RemoveAtSwap`; because `ToPreempt` is ascending, iterating it backwards keeps the remaining indices valid. `TArray::Contains` on `FTrafficResource` uses `operator==`.

- [ ] **Step 5: Build, run `-Filter Airside.Model.Occupancy`, full run (92 tests), commit**

```bash
git add -A Plugins/Airside && git commit -m "feat(airside): FTrafficOccupancy - the reservation table"
```

---

### Task 3: Runway identity — `IsRunwaySegment`, `RunwayChain`, `OutSegment`, `RunwayOccupied`

**Files:**
- Modify: `Public/Model/RoadNetwork.h` (runway queries, ~lines 85-128), `Private/Model/RoadNetwork.cpp` (`RunwayExtentInternal` ~160-300)
- Modify: `Public/Model/ArrivalPlanner.h`, `Private/Model/ArrivalPlanner.cpp`
- Create: `AirsideTests/Private/RunwayChainTest.cpp`

**Interfaces:**
- Produces on `URoadNetwork`: `bool IsRunwaySegment(FRoadSegmentId Segment) const`; `TArray<FRoadSegmentId> RunwayChain(FRoadSegmentId Seed) const` (empty when Seed is not a live runway segment; includes Seed); `RunwayExtentAt(..., FRoadSegmentId* OutSegment = nullptr)` and `NearestRunwayThreshold(..., FRoadSegmentId* OutSegment = nullptr)` write the SEED segment (the one whose end was nearest).
- Produces on `FArrivalPlan`: `UPROPERTY() FRoadSegmentId RunwaySegment; UPROPERTY() TArray<FRoadSegmentId> RunwayChain;`. `EArrivalRefusal::RunwayOccupied` (appended LAST; existing values keep their order). `ArrivalPlanner::Plan(const URoadNetwork&, const FVector2D&, const FAirframe&, const FTrafficOccupancy* Occupancy = nullptr)` refuses `RunwayOccupied` when any chain segment `IsHeld(..., 0)`. `DescribeRefusal` gains `"Arrival refused: the runway is in use. Wait for it to clear."`.

- [ ] **Step 1: Write the failing test**

`AirsideTests/Private/RunwayChainTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayChainTest,
	"Airside.Model.RunwayChain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayChainTest::RunTest(const FString& Parameters)
{
	// A runway split at an exit into two segments, a taxiway off the exit, and a SECOND
	// runway elsewhere that must not be swept into the chain.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net->AddNode(FVector2D(100000.0, 0.0));
	const FRoadSegmentId R1 = Net->AddStraightSegment(T, E, Runway);
	const FRoadSegmentId R2 = Net->AddStraightSegment(E, F, Runway);
	const FRoadNodeId X = Net->AddNode(FVector2D(60000.0, -20000.0));
	const FRoadSegmentId Tx = Net->AddStraightSegment(E, X, Taxiway);

	const FRoadNodeId T2 = Net->AddNode(FVector2D(0.0, 500000.0));
	const FRoadNodeId F2 = Net->AddNode(FVector2D(100000.0, 500000.0));
	const FRoadSegmentId Other = Net->AddStraightSegment(T2, F2, Runway);

	TestTrue(TEXT("a runway-profiled segment is a runway"), Net->IsRunwaySegment(R1));
	TestFalse(TEXT("a taxiway is not"), Net->IsRunwaySegment(Tx));

	const TArray<FRoadSegmentId> Chain = Net->RunwayChain(R2);
	TestEqual(TEXT("the chain crosses the exit split: two segments"), Chain.Num(), 2);
	TestTrue(TEXT("it contains both halves"), Chain.Contains(R1) && Chain.Contains(R2));
	TestFalse(TEXT("and not the other runway"), Chain.Contains(Other));
	TestFalse(TEXT("and not the taxiway"), Chain.Contains(Tx));
	TestEqual(TEXT("seeded from a taxiway the chain is empty"), Net->RunwayChain(Tx).Num(), 0);

	FVector2D Threshold, Direction; double Length = 0.0; FRoadSegmentId Seed;
	TestTrue(TEXT("extent query answers near the far end"), Net->RunwayExtentAt(FVector2D(99000.0, 100.0), Threshold, Direction, Length, &Seed));
	TestEqual(TEXT("and names the segment whose end was nearest"), Seed, R2);
	TestEqual(TEXT("length is the whole strip, not the seed"), Length, 100000.0, 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalPlannerRunwayOccupiedTest,
	"Airside.Model.ArrivalPlanner.RunwayOccupied",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalPlannerRunwayOccupiedTest::RunTest(const FString& Parameters)
{
	// Only the runway step is under test, so the plan is allowed to fail LATER (no exits,
	// no stands): the assertion is that a held runway is refused BEFORE any of that, with
	// its own reason, and that the same graph unheld gets past the runway step.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net->AddNode(FVector2D(120000.0, 0.0));
	const FRoadSegmentId R1 = Net->AddStraightSegment(T, E, Runway);
	Net->AddStraightSegment(E, F, Runway);

	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();

	FTrafficOccupancy Table;
	FTrafficClaim Hold; Hold.AgentId = 42; Hold.Resource = FTrafficResource::OfSurface(R1); Hold.bOccupied = true;
	FTrafficClaim Blocker;
	Table.TryClaim(Hold, Blocker);

	const FArrivalPlan Held = ArrivalPlanner::Plan(*Net, FVector2D(-1000.0, 0.0), Airframe, &Table);
	TestEqual(TEXT("a runway held on ANY segment of its chain refuses the arrival"), Held.Why, EArrivalRefusal::RunwayOccupied);
	TestEqual(TEXT("the plan still names the chain it was refused for"), Held.RunwayChain.Num(), 2);
	TestTrue(TEXT("DescribeRefusal has words for it"), ArrivalPlanner::DescribeRefusal(Held).Contains(TEXT("in use")));

	Table.Clear();
	const FArrivalPlan Free = ArrivalPlanner::Plan(*Net, FVector2D(-1000.0, 0.0), Airframe, &Table);
	TestNotEqual(TEXT("unheld, the refusal (if any) is a later step's"), Free.Why, EArrivalRefusal::RunwayOccupied);
	TestNotEqual(TEXT("and the runway was found"), Free.Why, EArrivalRefusal::NoRunway);
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail** — `IsRunwaySegment`, `RunwayChain`, five-arg `RunwayExtentAt`, `RunwayOccupied` undeclared.

- [ ] **Step 3: `URoadNetwork` additions**

In `Public/Model/RoadNetwork.h`, before `RunwayExtentAt`:
```cpp
	/**
	 * The profile rule, in one place: a runway is a segment whose profile is continuous
	 * through junctions. Every runway query below asked this inline; the traffic model asks
	 * it per claim, and two spellings of one rule is how a taxiway ends up a runway.
	 */
	bool IsRunwaySegment(FRoadSegmentId Segment) const;

	/**
	 * Every segment continuous with Seed through nodes joining exactly two runway segments -
	 * the same walk RunwayExtentAt makes to find the thresholds, returning the segments it
	 * walked rather than the ends. Empty when Seed is not a live runway. Includes Seed.
	 *
	 * This is what a runway IS to the occupancy table: a landing holds every segment of the
	 * chain, a hold-short names one, and the arbiter expands it here - so an exit added to a
	 * runway after the hold bar was placed still protects the whole strip.
	 */
	TArray<FRoadSegmentId> RunwayChain(FRoadSegmentId Seed) const;
```
Change both `RunwayExtentAt` and `NearestRunwayThreshold` declarations to end `double& OutLength, FRoadSegmentId* OutSegment = nullptr) const;` and add to each doc comment: `OutSegment, when given, receives the seed segment - the runway segment whose end was nearest Near.` Change the private `RunwayExtentInternal` declaration to take `FRoadSegmentId* OutSegment` (no default).

In `Private/Model/RoadNetwork.cpp`:
```cpp
bool URoadNetwork::IsRunwaySegment(FRoadSegmentId Segment) const
{
	const FRoadSegment* Found = GetSegment(Segment);
	if (Found == nullptr || !Found->bAlive)
	{
		return false;
	}
	const URoadProfile* Profile = ProfileFor(*Found);
	return Profile != nullptr && Profile->bContinuousThroughJunctions;
}

TArray<FRoadSegmentId> URoadNetwork::RunwayChain(FRoadSegmentId Seed) const
{
	TArray<FRoadSegmentId> Out;
	if (!IsRunwaySegment(Seed))
	{
		return Out;
	}
	Out.Add(Seed);

	// The same walk RunwayExtentInternal makes, collecting segments instead of stopping
	// at the ends: from each end of Seed, step through nodes that join exactly two runway
	// segments, and stop at a threshold (one arm) or anything stranger (a fork).
	auto WalkFrom = [this, &Out](FRoadNodeId At, FRoadSegmentId Along)
	{
		for (int32 Guard = 0; Guard < 1024; ++Guard)
		{
			const FRoadNode* Node = GetNode(At);
			if (Node == nullptr)
			{
				return;
			}
			FRoadSegmentId Next;
			int32 RunwayArms = 0;
			for (const FRoadSegmentId& Incident : Node->Incident)
			{
				if (!IsRunwaySegment(Incident))
				{
					continue;
				}
				++RunwayArms;
				if (Incident != Along)
				{
					Next = Incident;
				}
			}
			if (RunwayArms != 2 || !Next.IsSet() || Out.Contains(Next))
			{
				return;
			}
			Out.Add(Next);
			At = GetOtherEnd(Next, At);
			Along = Next;
		}
	};

	const FRoadSegment* SeedSegment = GetSegment(Seed);
	WalkFrom(SeedSegment->A, Seed);
	WalkFrom(SeedSegment->B, Seed);
	return Out;
}
```
In `RunwayExtentInternal`: delete the local `IsRunway` lambda; in the first loop build `FRoadSegmentId Id{Index, Segment.Generation}` and test `IsRunwaySegment(Id)`; inside `WalkFrom` test `IsRunwaySegment(Incident)`. After the proximity test passes (just before `WalkFrom` is defined), add:
```cpp
	if (OutSegment != nullptr)
	{
		OutSegment->Index = Best;
		OutSegment->Generation = Segments[Best].Generation;
	}
```
Thread `OutSegment` through `RunwayExtentAt` and `NearestRunwayThreshold` into `RunwayExtentInternal`.

- [ ] **Step 4: `FArrivalPlan` and the planner**

In `Public/Model/ArrivalPlanner.h`: add `#include "Model/RoadHandles.h"` and `struct FTrafficOccupancy;`. Append to `EArrivalRefusal` (last):
```cpp
	/**
	 * The runway exists and would do, but someone holds it - a landing rolling out, a
	 * departure lining up, or an aircraft crossing at a hold-short. The one refusal that
	 * clears on its own; M3's sequencer queues on it.
	 */
	RunwayOccupied,
```
In `FArrivalPlan` after `RunwayLength`:
```cpp
	/** The runway segment nearest the query, and every segment continuous with it. What the
	 *  landing holds in the occupancy table from StartArrival until Vacated. */
	UPROPERTY() FRoadSegmentId RunwaySegment;
	UPROPERTY() TArray<FRoadSegmentId> RunwayChain;
```
Change `Plan` to `Plan(const URoadNetwork& Network, const FVector2D& Near, const FAirframe& Airframe, const FTrafficOccupancy* Occupancy = nullptr);` and add to its doc comment: `Occupancy, when given, refuses RunwayOccupied while any segment of the chain is held. Null is the pre-traffic answer, which is what a tool that only asks "could this land here" still wants.`

In `Private/Model/ArrivalPlanner.cpp`: `#include "Model/TrafficOccupancy.h"`. Pass `&Out.RunwaySegment` to `NearestRunwayThreshold`, then right after the `NoRunway` return:
```cpp
		Out.RunwayChain = Network.RunwayChain(Out.RunwaySegment);

		// Asked before the length and exit steps, because those cannot change while the
		// runway is busy and this can: a refusal that clears on its own is reported as
		// itself, not as whichever later step happened to fail too.
		if (Occupancy != nullptr)
		{
			for (const FRoadSegmentId& Segment : Out.RunwayChain)
			{
				if (Occupancy->IsHeld(FTrafficResource::OfSurface(Segment), 0))
				{
					Out.Why = EArrivalRefusal::RunwayOccupied;
					return Out;
				}
			}
		}
```
In `DescribeRefusal`: `case EArrivalRefusal::RunwayOccupied: return TEXT("Arrival refused: the runway is in use. Wait for it to clear.");`

- [ ] **Step 5: Build, run `-Filter Airside.Model`, full run (94 tests), commit**

`grep -rn "RunwayExtentAt\|NearestRunwayThreshold" Plugins Source` — every caller compiles unchanged thanks to the default. Confirm `Airside.Model.RunwayExtent*`, `Airside.Model.ArrivalPlanner*` and `Airside.Present.ArrivalDispatch` still pass.
```bash
git add -A Plugins/Airside && git commit -m "feat(airside): runway chain identity; ArrivalPlanner refuses an occupied runway"
```

---

### Task 4: `UGroundTraffic` — the Mediator moves down to `Model/`

Pure refactor plus new fields. No arbitration yet: `Advance` ticks agents with `StopWithin` unbounded, so every existing behaviour is preserved and measurable. Refactor contract applies: count `UE_LOG(` and comment lines in `AirsideTraffic.cpp` before and after; both may only rise across the pair `AirsideTraffic.cpp + GroundTraffic.cpp`.

**Files:**
- Create: `Public/Model/GroundTraffic.h`, `Private/Model/GroundTraffic.cpp`
- Modify: `Public/Model/RoadAgent.h` (new fields), `Private/Model/RoadAgent.cpp` (`Advance` passes `StopWithin`)
- Modify: `Public/Present/AirsideTraffic.h`, `Private/Present/AirsideTraffic.cpp` (rewrite as registry + forwarders)
- Modify: `Public/Present/RoadNetworkActor.h`, `Private/Present/RoadNetworkActor.cpp` (`Tick`, `DispatchAgent` class overload)
- Modify: `Public/Tool/RoadEditTarget.h`, `Public/Present/RoadEditFacade.h`, `Private/Present/RoadEditFacade.cpp` (`DispatchAgent(…, Class)`)
- Modify: `Private/Tool/RouteTool.cpp` line ~134 (pass `Class`)
- Create: `AirsideTests/Private/TrafficForwardersTest.cpp`

**Interfaces:**
- Produces `FTrafficRules` (see Step 3) and `UGroundTraffic` (see Step 3). Later tasks add private members to `UGroundTraffic`; the public surface below is final.
- Produces on `FRoadAgent`: `int32 Id`, `ETraversalClass Class`, `FGuidelineNodeId GoalNode`, `double StopWithin`, `int32 WaitingOn`, `double StalledSeconds`, `double LastResolveAttempt`, `int32 BlockedStep`, `TArray<FRoadSegmentId> RunwayHeld`, `TArray<FRoadSegmentId> DepartureRunway`.
- Produces on `UAirsideTraffic`: `UGroundTraffic* GetModel() const`; `Advance(float DeltaSeconds, double SurfaceZ, const URoadNetwork* Network)`; `DispatchAgent(const URoadNetwork*, const FRoutePlan&, const FAirframe&, double SurfaceZ, double ShutdownPauseSeconds, ETraversalClass Class = ETraversalClass::Aircraft)`. Every other public name unchanged.
- Produces on `IRoadEditTarget`: `virtual bool DispatchAgent(const FRoutePlan&, const FAirframe&, ETraversalClass Class) = 0;` plus a non-virtual two-argument `DispatchAgent` that forwards with `Aircraft`. `ARoadNetworkActor` and `URoadEditFacade` override the three-argument one and carry `using IRoadEditTarget::DispatchAgent;`.

- [ ] **Step 1: Count the baseline**

```bash
grep -c "UE_LOG(" Plugins/Airside/Source/Airside/Private/Present/AirsideTraffic.cpp
grep -cE "^\s*(//|\*|/\*)" Plugins/Airside/Source/Airside/Private/Present/AirsideTraffic.cpp Plugins/Airside/Source/Airside/Public/Present/AirsideTraffic.h
```
Record both. Expected today: 9 log lines.

- [ ] **Step 2: Write the failing test**

`AirsideTests/Private/TrafficForwardersTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadAgentActor.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FGuidelineEdgeId M2FwdJoin(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B)
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = (Net.GetGuidelineNode(A)->Position + Net.GetGuidelineNode(B)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::All();
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficForwardersTest,
	"Airside.Present.TrafficForwarders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficForwardersTest::RunTest(const FString& Parameters)
{
	// THE SEAM TEST for the Mediator split: every name on UAirsideTraffic must reach
	// UGroundTraffic, the view must appear and vanish on the model's own phase events, and
	// both delegates must re-broadcast - because AirportOps binds to UAirsideTraffic's,
	// and a relay that was never wired would leave the flight board deaf without any
	// compile error to say so.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world to spawn into"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;

	UAirsideTraffic* Traffic = Actor->GetTraffic();
	if (!TestNotNull(TEXT("the actor has a traffic object"), Traffic)) { return false; }
	UGroundTraffic* Model = Traffic->GetModel();
	if (!TestNotNull(TEXT("and it owns a model"), Model)) { return false; }
	TestEqual(TEXT("the model's outer is THIS traffic object, not the CDO's (duplication rule)"), Model->GetOuter(), static_cast<UObject*>(Traffic));

	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(30000.0, 0.0), false);
	M2FwdJoin(Net, A, B);
	FRouteQuery Q; Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Plan = RouteSearch::Find(Net, Q);
	if (!TestTrue(TEXT("route found"), Plan.IsValid())) { return false; }

	TArray<TPair<EAgentPhase, EAgentPhase>> Relayed;
	Traffic->OnAgentPhaseChanged.AddLambda([&Relayed](int32, EAgentPhase From, EAgentPhase To) { Relayed.Emplace(From, To); });

	FAirframe Van = UAirsideSettings::ResolveDefaultAirframe();
	Van.Climb = FClimbPerformance();   // a van does not fly; unset Climb means no departure can arm
	if (!TestTrue(TEXT("dispatch through the actor is accepted"), Actor->DispatchAgent(Plan, Van, ETraversalClass::GroundVehicle))) { return false; }

	const int32 Id = Traffic->GetNewestAgentId();
	TestTrue(TEXT("the id is assigned by the model"), Id >= 1 && Model->GetNewestAgentId() == Id);
	TestEqual(TEXT("the model has the agent"), Model->GetAgentCount(), 1);
	TestEqual(TEXT("GetAgentCount forwards"), Traffic->GetAgentCount(), 1);
	const FRoadAgent* Agent = Model->FindAgent(Id);
	if (!TestNotNull(TEXT("FindAgent"), Agent)) { return false; }
	TestEqual(TEXT("the class the tool asked for reached the model"), Agent->Class, ETraversalClass::GroundVehicle);
	TestEqual(TEXT("the goal node is recorded for replans"), Agent->GoalNode, B);
	TestEqual(TEXT("the spawn was relayed as Gone -> Taxiing"), Relayed.Num(), 1);
	if (Relayed.Num() == 1) { TestTrue(TEXT("with the right phases"), Relayed[0].Key == EAgentPhase::Gone && Relayed[0].Value == EAgentPhase::Taxiing); }

	ARoadAgentActor* View = Traffic->GetNewestAgent();
	if (!TestNotNull(TEXT("a view was spawned on the phase event"), View)) { return false; }
	const FVector Before = View->GetActorLocation();

	for (int32 Tick = 0; Tick < 40; ++Tick) { Traffic->Advance(0.05f, 0.0, &Net); }
	TestTrue(TEXT("the view moved: Advance forwards and pushes LastMotion to the view"),
		FVector::Dist(View->GetActorLocation(), Before) > 10.0);
	TestTrue(TEXT("and the model's own position agrees with the view"),
		FVector2D::Distance(Traffic->LastAgentPositionForTest(), FVector2D(View->GetActorLocation().X, View->GetActorLocation().Y)) < 1.0);

	TestTrue(TEXT("RetireAgent forwards"), Traffic->RetireAgent(Id));
	TestEqual(TEXT("the model dropped it"), Model->GetAgentCount(), 0);
	TestNull(TEXT("and the view is gone"), Traffic->GetNewestAgent());
	TestEqual(TEXT("removal was relayed as Taxiing -> Gone"), Relayed.Num(), 2);

	TArray<EArrivalRefusal> Refusals;
	Traffic->OnArrivalRefused.AddLambda([&Refusals](EArrivalRefusal Why) { Refusals.Add(Why); });
	TestFalse(TEXT("no runway: arrival refused"), Actor->DispatchArrival(FVector2D::ZeroVector, UAirsideSettings::ResolveDefaultAirframe()));
	TestEqual(TEXT("the refusal relayed"), Refusals.Num(), 1);
	return true;
}

#endif
```

- [ ] **Step 3: `FTrafficRules` and `UGroundTraffic` header**

`Public/Model/GroundTraffic.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadAgent.h"
#include "Model/RoadTraffic.h"
#include "Model/TrafficOccupancy.h"
#include "GroundTraffic.generated.h"

class URoadNetwork;

/**
 * The numbers the arbiter works with. Spec 2026-09-06 §2.3.
 *
 * Footprint and gap live HERE, per class, and not on FAirframe: the airframe has no
 * length figure today, and a second copy of a performance number is the drift this
 * codebase's "one struct per thing" rule exists to prevent. A per-type length is a later
 * refinement with one owner.
 */
USTRUCT()
struct AIRSIDE_API FTrafficRules
{
	GENERATED_BODY()

	/** How much of the line an agent's body covers, uu. Half ahead of Travelled, half behind. */
	UPROPERTY(EditAnywhere) double AircraftFootprint = 1000.0;
	UPROPERTY(EditAnywhere) double VehicleFootprint = 500.0;

	/** Clear line kept ahead of the nose, beyond the braking distance, uu. */
	UPROPERTY(EditAnywhere) double AircraftGap = 1500.0;
	UPROPERTY(EditAnywhere) double VehicleGap = 300.0;

	/** Weight on held length in the routing cost. See FRouteQuery::CongestionWeight. */
	UPROPERTY(EditAnywhere) double CongestionWeight = 2.0;

	/** Stopped-and-waiting this long before deadlock detection looks. A normal junction
	 *  wait must never trip it. */
	UPROPERTY(EditAnywhere) double StallSeconds = 3.0;

	/** An unresolvable waiter re-tries its replan this often, sim seconds. */
	UPROPERTY(EditAnywhere) double RetrySeconds = 5.0;

	/** After a graph rebuild, how near a live node must be to a step's end to be it. */
	UPROPERTY(EditAnywhere) double ResolveRadius = 25.0;

	double FootprintFor(ETraversalClass Class) const;
	double GapFor(ETraversalClass Class) const;
};

/**
 * Every agent under way, the reservation table, and the tick that arbitrates between
 * them. Spec 2026-09-06 §2.2, §3.
 *
 * Pattern: Mediator - the one UAirsideTraffic was, moved down a layer. It moved because the
 * milestone's three tests ("two agents converge on a node; one yields", and the rest) must
 * run with NewObject and no world, and every multi-agent test before this one had to
 * UWorld::CreateWorld because the agent list lived in Present/ beside its view pointers.
 * Free functions over a hand-built agent array were considered and rejected: the ORDER of
 * claims is what makes aircraft-over-vehicle true, and a test that built its own order
 * would not be testing the tick.
 *
 * WORLD-FREE. The network is passed to Advance per call and never held, so this object
 * cannot outlive the graph it arbitrates over and needs nothing from an actor.
 *
 * A UObject rather than a USTRUCT so the table and the agents are GC-visible UPROPERTYs
 * on something UAirsideTraffic can own by CreateDefaultSubobject, and so a test can
 * NewObject one. Transient throughout: agents never reach disk (see Agents).
 */
UCLASS()
class AIRSIDE_API UGroundTraffic : public UObject
{
	GENERATED_BODY()

public:
	/** See UAirsideTraffic::OnAgentPhaseChanged, which relays this one layer up. */
	DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentPhaseChanged, int32 /*AgentId*/, EAgentPhase /*From*/, EAgentPhase /*To*/);
	FOnAgentPhaseChanged OnAgentPhaseChanged;

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnArrivalRefused, EArrivalRefusal);
	FOnArrivalRefused OnArrivalRefused;

	UPROPERTY(EditAnywhere, Category = "Airside|Traffic") FTrafficRules Rules;

	/**
	 * Lands an aircraft on the runway nearest Near and taxis it to a stand. Returns the new
	 * agent's id, or 0 - having broadcast OnArrivalRefused with the planner's reason. The
	 * runway chain is held from here until Vacated. Occupied runway: RunwayOccupied.
	 */
	int32 DispatchArrival(const URoadNetwork& Network, const FVector2D& Near, const FAirframe& Airframe,
		double ShutdownPauseSeconds);

	/** Sends a new agent of Class along Plan. Returns its id, or 0 when the plan is unusable. */
	int32 DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan, const FAirframe& Airframe,
		ETraversalClass Class, double ShutdownPauseSeconds);

	bool RedirectAgent(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Plan);
	bool RetireAgent(int32 AgentId);
	void ClearAgents();

	/**
	 * One tick: arbitrate, advance every agent, resolve deadlocks, announce phase changes.
	 * Network null means no arbitration - every agent drives as if alone, which is the
	 * pre-M2 behaviour and what a caller with no graph yet gets.
	 */
	void Advance(double DeltaSeconds, const URoadNetwork* Network);

	int32 GetAgentCount() const { return Agents.Num(); }
	int32 GetNewestAgentId() const { return Agents.Num() > 0 ? Agents.Last().Id : 0; }
	const TArray<FRoadAgent>& GetAgents() const { return Agents; }
	const FRoadAgent* FindAgent(int32 AgentId) const;
	const FTrafficOccupancy& GetOccupancy() const { return Occupancy; }
	double GetSimSeconds() const { return SimSeconds; }

private:
	/**
	 * Runtime only, and deliberately not part of URoadNetwork. An agent is a thing part way
	 * through a journey, not a fact about the airport: putting them in the network would
	 * snapshot them into every undo Memento and serialise them into the saved level, so
	 * re-opening a map would restore half-driven cubes that no longer have a route.
	 */
	UPROPERTY(Transient) TArray<FRoadAgent> Agents;

	/** Next id to hand out. Ids are per-session; 0 is never issued. */
	UPROPERTY(Transient) int32 NextAgentId = 1;

	UPROPERTY(Transient) FTrafficOccupancy Occupancy;

	/** Sim seconds elapsed through Advance. The deadlock resolver's retry clock. */
	UPROPERTY(Transient) double SimSeconds = 0.0;

	/** Assigns the id, stores the agent, announces Gone -> its phase. The one place all three happen. */
	int32 Admit(FRoadAgent&& Agent);
	int32 FindIndex(int32 AgentId) const;

	/** Arms a departure when Plan ends on a runway. One helper for dispatch and redirect. */
	void ArmDepartureIfRunway(FRoadAgent& Agent, const URoadNetwork* Network, const FRoutePlan& Plan) const;
};
```

- [ ] **Step 4: `FRoadAgent` fields and `Advance`**

In `Public/Model/RoadAgent.h`, add `#include "Model/RoadHandles.h"` and `#include "Model/RoadTraffic.h"`. After `LastMotion`:
```cpp
	/** Stable identity for the agent's lifetime, assigned by UGroundTraffic::Admit. 0 means
	 *  unassigned and is never handed out. Was FAgentSlot::Id before the Mediator moved to
	 *  Model/ and the slot struct went with the view pointer it existed to carry. */
	UPROPERTY() int32 Id = 0;

	/** How this agent moves. Vehicles were dispatched with no class at all before M2, which
	 *  is why priority could not be applied to them. */
	UPROPERTY() ETraversalClass Class = ETraversalClass::Aircraft;

	/** Where the current route is going, so a replan can aim at the same place. */
	UPROPERTY() FGuidelineNodeId GoalNode;

	// --- Written by UGroundTraffic's arbitration each tick; read by Advance ------------
	//
	// Arbitration writes, motion reads: there is no second evaluator of where the agent
	// may go, only one input into the one follower.

	/** Distance beyond which the follower may not go this tick. See FRouteFollower::Advance. */
	UPROPERTY() double StopWithin = TNumericLimits<double>::Max();

	/** Id of the agent holding what this one was refused, or 0. The wait-for graph's edge. */
	UPROPERTY() int32 WaitingOn = 0;

	/** Index into Follower.Plan.Steps of the step whose resource refused this agent, or -1.
	 *  Names the node a deadlock replan starts from (the step's FROM node). */
	UPROPERTY() int32 BlockedStep = -1;

	/** Seconds stopped with WaitingOn set. Deadlock detection looks once this passes the rule. */
	UPROPERTY() double StalledSeconds = 0.0;

	/** SimSeconds of the last replan attempt by the deadlock resolver; -1e9 = never. */
	UPROPERTY() double LastResolveAttempt = -1.0e9;

	/** Runway segments this agent occupies in a phase that is not a taxi: an arrival from
	 *  StartArrival until Vacated, a departure from the handover until Gone. */
	UPROPERTY() TArray<FRoadSegmentId> RunwayHeld;

	/** The chain a taxi ending on a runway will hold once it becomes a departure. */
	UPROPERTY() TArray<FRoadSegmentId> DepartureRunway;
```
In `Private/Model/RoadAgent.cpp`, `case EAgentPhase::Taxiing`: change `Follower.Advance(DeltaSeconds, FollowAt, FollowHeading)` to `Follower.Advance(DeltaSeconds, StopWithin, FollowAt, FollowHeading)`.

- [ ] **Step 5: `UGroundTraffic` body**

`Private/Model/GroundTraffic.cpp`. Move the bodies of `DispatchArrival`, `DispatchAgent`, `ArmDepartureIfRunway`, `Admit`, `FindSlot`→`FindIndex`, `RedirectAgent`, `RetireAgent`, `ClearAgents`, `Advance` from `AirsideTraffic.cpp`, dropping every `World`/`SpawnActor`/`View` line, keeping every `UE_LOG` and every WHY comment. Differences from the moved code, exactly:
```cpp
#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"
#include "Solve/RunwayDesignator.h"

double FTrafficRules::FootprintFor(ETraversalClass Class) const
{
	// Pedestrians and emergency vehicles take the vehicle figures: nothing authored says
	// otherwise yet, and a fire truck is nearer a van than an aeroplane.
	return Class == ETraversalClass::Aircraft ? AircraftFootprint : VehicleFootprint;
}

double FTrafficRules::GapFor(ETraversalClass Class) const
{
	return Class == ETraversalClass::Aircraft ? AircraftGap : VehicleGap;
}

int32 UGroundTraffic::DispatchArrival(const URoadNetwork& Network, const FVector2D& Near,
	const FAirframe& Airframe, double ShutdownPauseSeconds)
{
	// WHICH RUNWAY, WHICH EXIT, WHICH STAND - none of that needs a world, so issue #29 moved
	// it to Model/ArrivalPlanner. This is left with arming and logging the plan. The table
	// is handed in so a runway someone holds is refused HERE, with its own reason, rather
	// than by an aircraft that lands through another one.
	const FArrivalPlan Plan = ArrivalPlanner::Plan(Network, Near, Airframe, &Occupancy);

	// ... (the two existing UE_LOGs and the refusal broadcast, verbatim) ...

	FRoadAgent Agent;
	if (!Agent.StartArrival(Plan.Threshold, Plan.Direction, Plan.RunwayLength, Airframe, Plan.VacateAt, Plan.TaxiIn))
	{
		return 0;
	}
	Agent.ShutdownPause = ShutdownPauseSeconds;
	Agent.Class = ETraversalClass::Aircraft;
	Agent.GoalNode = Plan.TaxiIn.Steps.Num() > 0 ? Plan.TaxiIn.Steps.Last().To : FGuidelineNodeId();

	// THE RUNWAY IS HELD FROM NOW. Claimed as occupied every tick by Advance while the phase
	// is Arriving; released at the Vacated handover. Held on the agent rather than looked
	// up again at release, because by then the graph may have been rebuilt.
	Agent.RunwayHeld = Plan.RunwayChain;

	// ... (the existing "Arrival on runway" UE_LOG, verbatim) ...
	return Admit(MoveTemp(Agent));
}

int32 UGroundTraffic::DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan,
	const FAirframe& Airframe, ETraversalClass Class, double ShutdownPauseSeconds)
{
	if (!Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		return 0;
	}
	FRoadAgent Agent;
	Agent.StartTaxi(Plan, Airframe);
	Agent.ShutdownPause = ShutdownPauseSeconds;
	Agent.Class = Class;
	Agent.GoalNode = Plan.Steps.Num() > 0 ? Plan.Steps.Last().To : FGuidelineNodeId();
	ArmDepartureIfRunway(Agent, Network, Plan);

	// Posed before its first tick: a zero-second Advance asks where the taxi starts without
	// moving it, so LastMotion is a full pose - heading included - by the time the view is
	// spawned off the Admit broadcast below. StartTaxi's fallback covers a plan too short.
	FAgentMotion Motion;
	Agent.Advance(0.0, Motion);
	return Admit(MoveTemp(Agent));
}
```
`ArmDepartureIfRunway`: pass a `FRoadSegmentId Seed` to `RunwayExtentAt` and set `Agent.DepartureRunway = Network->RunwayChain(Seed);` beside `ArmDeparture`. `Admit` returns the id. `RedirectAgent` sets `GoalNode` from the new plan's last step and keeps `Class`. `Advance`:
```cpp
void UGroundTraffic::Advance(double DeltaSeconds, const URoadNetwork* Network)
{
	SimSeconds += DeltaSeconds;

	// Every handover (arrive -> taxi -> depart -> gone, or arrive -> taxi -> park) is owned
	// by FRoadAgent::Advance - see its own comment. This loop is left with: advance, watch
	// the phase, drop an agent once it says Gone. Arbitration slots in ahead of it (Task 5)
	// and the deadlock pass behind it (Task 8); neither touches this order.
	for (int32 Index = Agents.Num() - 1; Index >= 0; --Index)
	{
		FRoadAgent& Agent = Agents[Index];
		const EAgentPhase Before = Agent.Phase;
		const int32 Id = Agent.Id;

		FAgentMotion Motion;
		if (!Agent.Advance(DeltaSeconds, Motion))
		{
			Occupancy.ReleaseAll(Id);
			Agents.RemoveAt(Index);
			// Broadcast AFTER the removal so a listener that asks GetAgentCount sees the
			// agent already gone, which is what "To == Gone" promises.
			OnAgentPhaseChanged.Broadcast(Id, Before, EAgentPhase::Gone);
			continue;
		}

		if (Agent.Phase != Before)
		{
			OnAgentPhaseChanged.Broadcast(Id, Before, Agent.Phase);
		}
	}
}
```
`FindAgent` returns `&Agents[FindIndex(Id)]` or null.

- [ ] **Step 6: `UAirsideTraffic` becomes the registry**

Rewrite `Public/Present/AirsideTraffic.h`. Delete `FAgentSlot` (its Id comment moved to `FRoadAgent::Id`; its "view pointer lives here, not on FRoadAgent" paragraph moves onto `Views` below). Class comment: keep the Mediator paragraph but rewrite its first sentence to say the mediation now lives in `UGroundTraffic` and this class is the VIEW REGISTRY and the relay across the Present/Model boundary - the same shape AirportOps's `UOpsRuntime` has above it. Members:
```cpp
	UAirsideTraffic();
	virtual void PostInitProperties() override;

	DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentPhaseChanged, int32, EAgentPhase, EAgentPhase);
	FOnAgentPhaseChanged OnAgentPhaseChanged;   // relayed from Model; AirportOps binds here
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnArrivalRefused, EArrivalRefusal);
	FOnArrivalRefused OnArrivalRefused;

	UGroundTraffic* GetModel() const { return Model; }
	int32 GetNewestAgentId() const;

	bool DispatchArrival(const URoadNetwork& Network, const FVector2D& Near, const FAirframe& Airframe, double SurfaceZ, double ShutdownPauseSeconds);
	bool DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan, const FAirframe& Airframe, double SurfaceZ, double ShutdownPauseSeconds, ETraversalClass Class = ETraversalClass::Aircraft);
	bool RedirectAgent(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Plan);
	bool RetireAgent(int32 AgentId);
	void ClearAgents();
	int32 GetAgentCount() const;
	ARoadAgentActor* GetNewestAgent() const;
	void Advance(float DeltaSeconds, double SurfaceZ, const URoadNetwork* Network);
	EAgentPhase LastAgentPhaseForTest() const;
	double LastAgentTaxiSpeedCapForTest() const;
	FVector2D LastAgentPositionForTest() const;

private:
	/** The Mediator. CreateDefaultSubobject in the constructor, re-pointed by name in
	 *  PostInitProperties - the duplication rule ARoadNetworkActor already follows. */
	UPROPERTY(Transient) TObjectPtr<UGroundTraffic> Model;

	/**
	 * Agent id -> the cube standing where it is. THE VIEW POINTER LIVES HERE, NOT ON
	 * FRoadAgent: the agent is Model/, world-free and testable with no actor, and the view
	 * is a level-resident actor, so pairing them is a Present-layer job. A map keyed by id
	 * rather than a parallel array, because the model's array reorders on removal.
	 */
	UPROPERTY(Transient) TMap<int32, TObjectPtr<ARoadAgentActor>> Views;

	/** The last SurfaceZ a caller gave, so a view spawned off a phase event can be posed. */
	UPROPERTY(Transient) double SurfaceZ = 0.0;

	void OnModelPhaseChanged(int32 AgentId, EAgentPhase From, EAgentPhase To);
	void OnModelArrivalRefused(EArrivalRefusal Why);
	void SpawnView(int32 AgentId);
	void DestroyView(int32 AgentId);
```
Body highlights in `Private/Present/AirsideTraffic.cpp`:
```cpp
UAirsideTraffic::UAirsideTraffic()
{
	Model = CreateDefaultSubobject<UGroundTraffic>(TEXT("GroundTraffic"));
}

void UAirsideTraffic::PostInitProperties()
{
	Super::PostInitProperties();
	// See ARoadNetworkActor::PostInitProperties: a duplicate arrives holding the CDO's
	// subobject. Re-pointed by name, and the relay bound HERE rather than in the
	// constructor, so it is bound to whichever model this object actually ends up with.
	Model = Cast<UGroundTraffic>(GetDefaultSubobjectByName(TEXT("GroundTraffic")));
	if (Model != nullptr && !Model->OnAgentPhaseChanged.IsBoundToObject(this))
	{
		Model->OnAgentPhaseChanged.AddUObject(this, &UAirsideTraffic::OnModelPhaseChanged);
		Model->OnArrivalRefused.AddUObject(this, &UAirsideTraffic::OnModelArrivalRefused);
	}
}

void UAirsideTraffic::OnModelPhaseChanged(int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	// The view follows the agent's LIFE, which the phase events already describe: born on
	// Gone -> anything, dead on anything -> Gone. Spawning inline in Dispatch* would be a
	// second place that knows when an agent exists.
	if (From == EAgentPhase::Gone) { SpawnView(AgentId); }
	if (To == EAgentPhase::Gone) { DestroyView(AgentId); }
	OnAgentPhaseChanged.Broadcast(AgentId, From, To);
}
```
`SpawnView`: `GetWorld()`; if null, `UE_LOG(LogAirsideTraffic, Warning, TEXT("Agent %d has no world to show in; model only."), AgentId)` and return (the old code refused the dispatch here; now the model already admitted it, and a model-only agent is a truer picture than a refused dispatch - say so in the comment). Else the existing `SpawnActor` + `SetAirframe` + `SetMotion(Agent->LastMotion, SurfaceZ)` lines, moved verbatim with their comments, into `Views.Add(AgentId, View)`. `DestroyView`: `Destroy()` + `Views.Remove`. `Advance`: `SurfaceZ = InSurfaceZ; Model->Advance(DeltaSeconds, Network); for (const FRoadAgent& Agent : Model->GetAgents()) { if (TObjectPtr<ARoadAgentActor>* View = Views.Find(Agent.Id)) { (*View)->SetMotion(Agent.LastMotion, SurfaceZ); } }`. `ClearAgents`: `Model->ClearAgents()` (views die via the events). Forwarders are one line each. `GetNewestAgent`: `Views.FindRef(Model->GetNewestAgentId())`.

- [ ] **Step 7: Actor, facade, interface, route tool**

`Public/Tool/RoadEditTarget.h`: replace the `DispatchAgent` virtual with the three-argument pure virtual and add:
```cpp
	/** Aircraft by default - what every caller before M2 meant. A non-virtual overload,
	 *  so implementers override one signature; they carry `using IRoadEditTarget::DispatchAgent;`
	 *  so this one stays visible on the concrete type. */
	bool DispatchAgent(const FRoutePlan& Plan, const FAirframe& Airframe)
	{
		return DispatchAgent(Plan, Airframe, ETraversalClass::Aircraft);
	}
```
`ARoadNetworkActor` and `URoadEditFacade`: override the three-argument form, add `using IRoadEditTarget::DispatchAgent;` in the public section. Actor body: `return Traffic->DispatchAgent(Network, Plan, Airframe, SurfaceZ, ShutdownPauseSeconds, Class);`. Facade body forwards to `Actor()`. `Tick`: `Traffic->Advance(static_cast<float>(DeltaSeconds * SimTimeScale), SurfaceZ, Network);`. `RouteTool.cpp` ~134: `Context.Target->DispatchAgent(LastPlan, Airframe, Class);`.

`grep -rn "Traffic->Advance\|->DispatchAgent(" Plugins Source` — fix any other caller (expected: none beyond the above; `AgentRedirectTest` and `SimTimeScaleTest` go through the actor's two-argument overload).

- [ ] **Step 8: Build, run everything, count, commit**

Full build (new UCLASS). Full test run: 95 tests, 0 failed, 0 crashed - every `Airside.Present.*` and `AirportOps.*` test unchanged. Counts:
```bash
grep -c "UE_LOG(" Plugins/Airside/Source/Airside/Private/Present/AirsideTraffic.cpp Plugins/Airside/Source/Airside/Private/Model/GroundTraffic.cpp
```
Sum ≥ Step 1's figure; comment lines likewise. `Check-Architecture.ps1` passes (`Model/GroundTraffic.h` includes nothing above `Model/`).
```bash
git add -A Plugins/Airside && git commit -m "refactor(airside): UGroundTraffic owns agents in Model/; UAirsideTraffic is the view registry (logs N->M, comments N->M)"
```

---

### Task 5: Claims and the arbitration tick

**Files:**
- Modify: `Public/Model/GroundTraffic.h` (private members), `Private/Model/GroundTraffic.cpp`
- Create: `AirsideTests/Private/GroundTrafficTest.cpp` (fixture + NodeYield, PriorityOverride, CarFollowing, HeadOn-stops)

**Interfaces:**
- Adds private to `UGroundTraffic`:
```cpp
	void Arbitrate(const URoadNetwork& Network);
	void ClaimAhead(FRoadAgent& Agent, const URoadNetwork& Network);
	int32 RankAt(const URoadNetwork& Network, FGuidelineNodeId Node, ETraversalClass Class) const;
	static double StepStart(const FRoutePlan& Plan, int32 Step);
	static FGuidelineNodeId StepFromNode(const FRoutePlan& Plan, int32 Step);
	static int32 CurrentStep(const FRoutePlan& Plan, double Travelled);
```
- `Advance` calls `Arbitrate(*Network)` before the agent loop when `Network != nullptr`, and after the loop accrues `StalledSeconds`.
- Test fixture (in `GroundTrafficTest.cpp`, anonymous namespace, `M2Traffic` prefix): `M2TrafficNode(Net, x, y)`, `M2TrafficJoin(Net, A, B, Direction = Bidirectional)`, `M2TrafficRoute(Net, A, B, Class)`, `M2TrafficVan()` (an `FAirframe` with `Ground` defaults but `MaxTurnRateDegPerSec = 90`, `Climb`/`Approach`/`Engine` unset, `Wingspan 0`), `M2TrafficPlane()` (`ResolveDefaultAirframe()` with `Climb` unset so nothing arms a departure), `M2TrafficRun(Traffic, Net, Seconds, Dt = 0.05, Callback)`.

**Algorithm (`ClaimAhead`), spec §3.1–3.3.** For a `Taxiing` agent with a valid plan and at least one step; `T = Follower.Travelled`, `F = Rules.FootprintFor(Class)`, `G = Rules.GapFor(Class)`, `W = Speed²/(2·Ground.Taxi.Decel) + G`, `Head = T + W`, `Tail = T − F/2`, `s = CurrentStep(Plan, T)`. Build `Wanted` (resources) and claims in route order:
1. From-node of step `s` if `T − StepStart(s) < F/2`: node claim, occupied.
2. For `i = s` while `i < Steps.Num()` and `StepStart(i) < Head`:
   - `Lo = max(Tail, StepStart(i))`, `Hi = min(Head, End(i))`; map to edge distance: `L = End(i) − StepStart(i)`; forward: `[Lo−Start, Hi−Start]`; reversed: `[L − (Hi−Start), L − (Lo−Start)]`. Occupied iff `i == s`. Rank = `RankAt(Network, Steps[i].To, Class)` (node rank governs the edge leading into it; for step `s` use the from-node's rank). Edge claim.
   - If the edge's `DerivedFrom` is a runway segment (Task 6).
   - Box entry: `bBox = L < F + G`. Node `Steps[i].To` is wanted if `End(i) < Head`, OR (`bBox` and `Head ≥ StepStart(i)`). Node claim, occupied iff `|End(i) − T| < F/2`.
   - If node has `HoldShortFor` set (Task 6).
3. Each claim in order: `TryClaim`. On `Held`: `WaitingOn = Blocker.AgentId`, `BlockedStep = i`, and `StopWithin`:
   - blocked EDGE interval: the blocker's nearest boundary ahead in edge distance mapped back to route distance (`Boundary = bReversed ? StepStart + (L − Blocker.To) : StepStart + Blocker.From`), `StopWithin = max(0, Boundary − T − G)`.
   - blocked NODE at end of step `i`: if `bBox` and `T ≤ StepStart(i)`: `StopWithin = max(0, StepStart(i) − T − G)`; else `StopWithin = max(0, End(i) − T − G)`.
   - blocked SURFACE via a runway edge at step `i`: `StopWithin = max(0, StepStart(i) − T − G)`; via a hold-short node at end of step `i`: `StopWithin = max(0, End(i) − T − F/2)` (nose on the bar).
   Stop claiming further. On all granted: `StopWithin = Max`, `WaitingOn = 0`, `BlockedStep = −1`. Then `Occupancy.ReleaseExcept(Id, Wanted)` — called BEFORE the claims so stale holds do not block the agent's own re-claim... no: `ReleaseExcept` runs first with the full `Wanted` list (which is computed before any `TryClaim`), so what is behind is dropped and what is still wanted is updated by `TryClaim`.
4. Non-`Taxiing` agents with `RunwayHeld`: claim each segment as Surface, occupied, rank `TraversalPriority(Class)`. `ReleaseExcept` with just those.

**`Arbitrate`:** order indices by `TraversalPriority(Class)` descending, then `Id` ascending; `ClaimAhead` each; then `for Id in Occupancy.TakePreempted(): ClaimAhead(that agent)` once.

**`RankAt`:** if the node exists and `PriorityOverride.Num() > 0`: `Index = PriorityOverride.Find(Class)`; rank = `Index == INDEX_NONE ? 0 : 10 · (PriorityOverride.Num() − Index)`; else `TraversalPriority(Class)`. Override ranks are scaled by 10 so an overridden node's order can never tie with a default one.

- [ ] **Step 1: Write the failing tests**

`AirsideTests/Private/GroundTrafficTest.cpp` — fixture plus four tests:
```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

DEFINE_LOG_CATEGORY_STATIC(LogM2TrafficTest, Log, All);

namespace
{
	FGuidelineNodeId M2TrafficNode(URoadNetwork& Net, double X, double Y)
	{
		return Net.AddGuidelineNode(FVector2D(X, Y), /*bDerived=*/false);
	}

	FGuidelineEdgeId M2TrafficJoin(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B,
		EGuidelineDir Direction = EGuidelineDir::Bidirectional)
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = (Net.GetGuidelineNode(A)->Position + Net.GetGuidelineNode(B)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = Direction;
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	FRoutePlan M2TrafficRoute(const URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B, ETraversalClass Class)
	{
		FRouteQuery Q; Q.Start = A; Q.Goal = B; Q.Class = Class;
		return RouteSearch::Find(Net, Q);
	}

	/** Ground defaults (Accel 100, Decel 200, cap 1000), a nimble nosewheel so corners do
	 *  not dominate the clock, and nothing that could arm a departure. */
	FAirframe M2TrafficVan()
	{
		FAirframe A;
		A.Ground.MaxTurnRateDegPerSec = 90.0;
		return A;
	}

	FAirframe M2TrafficPlane()
	{
		FAirframe A = UAirsideSettings::ResolveDefaultAirframe();
		A.Climb = FClimbPerformance();
		return A;
	}

	/** Ticks until Seconds elapse or Callback returns false. Returns ticks run. */
	template <typename F>
	int32 M2TrafficRun(UGroundTraffic& Traffic, const URoadNetwork& Net, double Seconds, F Callback, double Dt = 0.05)
	{
		int32 Ticks = 0;
		for (double Clock = 0.0; Clock < Seconds; Clock += Dt, ++Ticks)
		{
			Traffic.Advance(Dt, &Net);
			if (!Callback(Ticks)) { break; }
		}
		return Ticks;
	}
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficNodeYieldTest,
	"Airside.Model.Traffic.NodeYield",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficNodeYieldTest::RunTest(const FString& Parameters)
{
	// A crossing: aircraft west->east through J, van south->north through J, both 20 km
	// out so they reach J in the same second. Spec 3.8: the vehicle yields by class.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId W = M2TrafficNode(*Net, -20000.0, 0.0);
	const FGuidelineNodeId E = M2TrafficNode(*Net, 20000.0, 0.0);
	const FGuidelineNodeId S = M2TrafficNode(*Net, 0.0, -20000.0);
	const FGuidelineNodeId N = M2TrafficNode(*Net, 0.0, 20000.0);
	const FGuidelineNodeId J = M2TrafficNode(*Net, 0.0, 0.0);
	M2TrafficJoin(*Net, W, J); M2TrafficJoin(*Net, J, E);
	M2TrafficJoin(*Net, S, J); M2TrafficJoin(*Net, J, N);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, W, E, ETraversalClass::Aircraft), M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
	const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, S, N, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("both dispatched"), Plane > 0 && Van > 0)) { return false; }

	double VanMinStopWithin = TNumericLimits<double>::Max();
	double PlaneMinStopWithin = TNumericLimits<double>::Max();
	double MinSeparation = TNumericLimits<double>::Max();
	bool bVanWaitedOnPlane = false;
	M2TrafficRun(*Traffic, *Net, 120.0, [&](int32)
	{
		const FRoadAgent* P = Traffic->FindAgent(Plane);
		const FRoadAgent* V = Traffic->FindAgent(Van);
		if (P == nullptr || V == nullptr) { return false; }
		if (P->Phase == EAgentPhase::Taxiing) { PlaneMinStopWithin = FMath::Min(PlaneMinStopWithin, P->StopWithin); }
		if (V->Phase == EAgentPhase::Taxiing) { VanMinStopWithin = FMath::Min(VanMinStopWithin, V->StopWithin); }
		if (V->WaitingOn == Plane) { bVanWaitedOnPlane = true; }
		MinSeparation = FMath::Min(MinSeparation, FVector2D::Distance(P->LastMotion.Position, V->LastMotion.Position));
		return !(P->Phase == EAgentPhase::Parked && V->Phase == EAgentPhase::Parked);
	});

	TestTrue(TEXT("the van was stopped short of the node (StopWithin reached 0)"), VanMinStopWithin < 1.0);
	TestTrue(TEXT("the aircraft never was"), PlaneMinStopWithin > 1000.0);
	TestTrue(TEXT("the van's wait named the aircraft"), bVanWaitedOnPlane);
	TestTrue(FString::Printf(TEXT("never closer than the van's own footprint (%.0f uu)"), MinSeparation), MinSeparation >= Traffic->Rules.VehicleFootprint - 1.0);
	TestEqual(TEXT("both arrive"), Traffic->FindAgent(Plane)->Phase, EAgentPhase::Parked);
	TestEqual(TEXT("both arrive (van)"), Traffic->FindAgent(Van)->Phase, EAgentPhase::Parked);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficPriorityOverrideTest,
	"Airside.Model.Traffic.PriorityOverride",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficPriorityOverrideTest::RunTest(const FString& Parameters)
{
	// Same crossing, but J says vehicles first (spec 5.4's per-node exception).
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId W = M2TrafficNode(*Net, -20000.0, 0.0);
	const FGuidelineNodeId E = M2TrafficNode(*Net, 20000.0, 0.0);
	const FGuidelineNodeId S = M2TrafficNode(*Net, 0.0, -20000.0);
	const FGuidelineNodeId N = M2TrafficNode(*Net, 0.0, 20000.0);
	const FGuidelineNodeId J = M2TrafficNode(*Net, 0.0, 0.0);
	M2TrafficJoin(*Net, W, J); M2TrafficJoin(*Net, J, E);
	M2TrafficJoin(*Net, S, J); M2TrafficJoin(*Net, J, N);
	Net->GetGuidelineNodeMutable(J)->PriorityOverride = { ETraversalClass::GroundVehicle, ETraversalClass::Aircraft };

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, W, E, ETraversalClass::Aircraft), M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
	const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, S, N, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);

	bool bPlaneWaitedOnVan = false;
	bool bVanWaitedOnPlane = false;
	M2TrafficRun(*Traffic, *Net, 120.0, [&](int32)
	{
		const FRoadAgent* P = Traffic->FindAgent(Plane);
		const FRoadAgent* V = Traffic->FindAgent(Van);
		if (P == nullptr || V == nullptr) { return false; }
		bPlaneWaitedOnVan |= (P->WaitingOn == Van);
		bVanWaitedOnPlane |= (V->WaitingOn == Plane);
		return !(P->Phase == EAgentPhase::Parked && V->Phase == EAgentPhase::Parked);
	});
	TestTrue(TEXT("with the override the aircraft yields"), bPlaneWaitedOnVan);
	TestFalse(TEXT("and the van never does"), bVanWaitedOnPlane);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficCarFollowingTest,
	"Airside.Model.Traffic.CarFollowing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficCarFollowingTest::RunTest(const FString& Parameters)
{
	// Two aircraft on one long edge, the second dispatched from the same node 2 s later.
	// Spec 3.8: "the agent ahead's reservation is the stop point" - the follower must never
	// close inside footprint + gap, and must never pass.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = M2TrafficNode(*Net, 60000.0, 0.0);
	M2TrafficJoin(*Net, A, B);
	const FRoutePlan Plan = M2TrafficRoute(*Net, A, B, ETraversalClass::Aircraft);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	FAirframe Leader = M2TrafficPlane();
	Leader.Ground.Taxi.SpeedCap = 600.0;   // slower, so the follower catches it
	const int32 Lead = Traffic->DispatchAgent(Net, Plan, Leader, ETraversalClass::Aircraft, 1.0);
	int32 Follow = 0;
	double MinGap = TNumericLimits<double>::Max();
	bool bFollowerCaughtUp = false;
	M2TrafficRun(*Traffic, *Net, 200.0, [&](int32 Tick)
	{
		if (Tick == 40) { Follow = Traffic->DispatchAgent(Net, Plan, M2TrafficPlane(), ETraversalClass::Aircraft, 1.0); }
		const FRoadAgent* L = Traffic->FindAgent(Lead);
		const FRoadAgent* Fo = Follow > 0 ? Traffic->FindAgent(Follow) : nullptr;
		if (L == nullptr || Fo == nullptr) { return L != nullptr; }
		if (L->Phase == EAgentPhase::Taxiing && Fo->Phase == EAgentPhase::Taxiing)
		{
			const double Gap = L->Follower.Travelled - Fo->Follower.Travelled;
			MinGap = FMath::Min(MinGap, Gap);
			if (Fo->WaitingOn == Lead) { bFollowerCaughtUp = true; }
		}
		return !(L->Phase == EAgentPhase::Parked && Fo->Phase == EAgentPhase::Parked);
	});
	TestTrue(TEXT("the follower did catch the leader (otherwise this measures nothing)"), bFollowerCaughtUp);
	// The follower's CENTRE stops Gap behind the leader's TAIL (leader centre - Footprint/2),
	// so centre-to-centre is Footprint/2 + Gap: nose to tail is exactly the gap.
	TestTrue(FString::Printf(TEXT("centre-to-centre never below footprint/2 + gap (%.0f)"), MinGap),
		MinGap >= Traffic->Rules.AircraftFootprint * 0.5 + Traffic->Rules.AircraftGap - 50.0);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficHeadOnStopsTest,
	"Airside.Model.Traffic.HeadOnStops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficHeadOnStopsTest::RunTest(const FString& Parameters)
{
	// Nose to nose on a bidirectional taxiway with nowhere to turn. Both must STOP - the
	// pass-through-each-other defect the follower's header names. Resolution is Task 8's.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = M2TrafficNode(*Net, 40000.0, 0.0);
	M2TrafficJoin(*Net, A, B);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 P1 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, B, ETraversalClass::Aircraft), M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
	const int32 P2 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, B, A, ETraversalClass::Aircraft), M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
	double MinSeparation = TNumericLimits<double>::Max();
	M2TrafficRun(*Traffic, *Net, 120.0, [&](int32)
	{
		const FRoadAgent* X = Traffic->FindAgent(P1);
		const FRoadAgent* Y = Traffic->FindAgent(P2);
		MinSeparation = FMath::Min(MinSeparation, FVector2D::Distance(X->LastMotion.Position, Y->LastMotion.Position));
		return true;
	});
	const FRoadAgent* X = Traffic->FindAgent(P1);
	const FRoadAgent* Y = Traffic->FindAgent(P2);
	TestTrue(TEXT("both are stopped"), X->Follower.Speed < 1e-6 && Y->Follower.Speed < 1e-6);
	TestTrue(TEXT("both are still taxiing, not parked"), X->Phase == EAgentPhase::Taxiing && Y->Phase == EAgentPhase::Taxiing);
	TestTrue(TEXT("each waits on the other"), X->WaitingOn == P2 && Y->WaitingOn == P1);
	TestTrue(FString::Printf(TEXT("never closer than one footprint (%.0f)"), MinSeparation), MinSeparation >= Traffic->Rules.AircraftFootprint - 1.0);
	return true;
}

#endif
```

- [ ] **Step 2: Build and run `-Filter Airside.Model.Traffic`** — expect all four to FAIL on the unfixed tree (NodeYield: aircraft and van pass through, `VanMinStopWithin` stays Max; HeadOn: both park). Quote the failure lines.

- [ ] **Step 3: Implement**

Add the private members to `GroundTraffic.h` (Interfaces above), with a doc comment on `ClaimAhead` that states the algorithm in prose as written above and names the two rejected alternatives (release-all-then-reclaim, chain box extension - see spec §3.1). Implement in `GroundTraffic.cpp`:
```cpp
int32 UGroundTraffic::CurrentStep(const FRoutePlan& Plan, double Travelled)
{
	for (int32 Index = 0; Index < Plan.Steps.Num(); ++Index)
	{
		if (Travelled < Plan.Steps[Index].EndDistance) { return Index; }
	}
	return FMath::Max(0, Plan.Steps.Num() - 1);
}
double UGroundTraffic::StepStart(const FRoutePlan& Plan, int32 Step)
{
	return Step <= 0 ? 0.0 : Plan.Steps[Step - 1].EndDistance;
}
FGuidelineNodeId UGroundTraffic::StepFromNode(const FRoutePlan& Plan, int32 Step)
{
	return Step <= 0 ? Plan.Start : Plan.Steps[Step - 1].To;
}
int32 UGroundTraffic::RankAt(const URoadNetwork& Network, FGuidelineNodeId Node, ETraversalClass Class) const
{
	const FGuidelineNode* Found = Network.GetGuidelineNode(Node);
	if (Found != nullptr && Found->PriorityOverride.Num() > 0)
	{
		// Scaled by ten so an authored order can never tie with a default one - a tie keeps
		// the holder, and an authored "vehicles first" that tied would mean nothing.
		const int32 Index = Found->PriorityOverride.Find(Class);
		return Index == INDEX_NONE ? 0 : 10 * (Found->PriorityOverride.Num() - Index);
	}
	return TraversalPriority(Class);
}
```
`ClaimAhead` per the algorithm; log on the transition into waiting and out of it:
```cpp
	if (Held && Agent.WaitingOn != Blocker.AgentId)
	{
		UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d stops %.0f uu short of %s held by agent %d"),
			Agent.Id, Agent.StopWithin, *Blocker.Resource.Describe(), Blocker.AgentId);
	}
	if (!Held && Agent.WaitingOn != 0)
	{
		UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d resumes"), Agent.Id);
	}
```
`Arbitrate` as specified. In `Advance`, after `SimSeconds += DeltaSeconds`: `if (Network != nullptr) { Arbitrate(*Network); }`. In the agent loop after a successful `Advance`: `Agent.StalledSeconds = (Agent.Phase == EAgentPhase::Taxiing && Agent.WaitingOn != 0 && Agent.Follower.Speed < KINDA_SMALL_NUMBER) ? Agent.StalledSeconds + DeltaSeconds : 0.0;`.

- [ ] **Step 4: Build, run `-Filter Airside.Model.Traffic` green, full run (99 tests), commit**

```bash
git add -A Plugins/Airside && git commit -m "feat(airside): reservation window, node arbitration by rank, car-following, head-on stop"
```

---

### Task 6: Runway surface and hold-short honouring

**Files:**
- Modify: `Private/Model/GroundTraffic.cpp` (`ClaimAhead` runway/hold-short branches; surface release on handovers)
- Modify: `AirsideTests/Private/GroundTrafficTest.cpp` (+ HoldShort, ArrivalRefusedRunwayOccupied)

**Interfaces:** no new public names. Behaviour:
- In `ClaimAhead` step 2, after the edge claim: `if (Edge->DerivedFrom.IsSet() && Network.IsRunwaySegment(Edge->DerivedFrom))`: for each segment of `Network.RunwayChain(Edge->DerivedFrom)`: Surface claim, occupied iff `i == s`. A refusal here: `StopWithin = max(0, StepStart(i) − T − G)`.
- After the node claim: `if (Node->HoldShortFor.IsSet())`: for each segment of `Network.RunwayChain(Node->HoldShortFor)` (or just `HoldShortFor` if the chain is empty because the segment is no longer a runway): Surface claim, reserved (never occupied through a hold-short). A refusal: `StopWithin = max(0, End(i) − T − F/2)`.
- In `Advance`'s agent loop: on `Before == Arriving && Phase == Taxiing`: `for seg in RunwayHeld: Occupancy.Release(Id, OfSurface(seg))` — add `void Release(int32 AgentId, const FTrafficResource&)` to `FTrafficOccupancy` (one-liner beside `ReleaseAll`); `RunwayHeld.Reset()`; `UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d released the runway"), Id)`. On `Before == Taxiing && Phase == Departing`: `RunwayHeld = DepartureRunway`.
- Log at the hold-short: when `BlockedStep` changes to a hold-short refusal, `UE_LOG(... "Agent %d holding short at node %d for runway segment %d held by agent %d")`.

- [ ] **Step 1: Write the failing tests** (append to `GroundTrafficTest.cpp`)

```cpp
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficHoldShortTest,
	"Airside.Model.Traffic.HoldShort",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficHoldShortTest::RunTest(const FString& Parameters)
{
	// A taxiway that CROSSES a runway: S -> H (hold bar) -> X (on the runway centreline)
	// -> N. The guideline edges are hand-built and carry no DerivedFrom, so the ONLY thing
	// protecting the runway here is the hold-short node - which is what this test is about.
	// The edge-derived-from-a-runway route to the same surface is exercised by the
	// arrival dispatch test once landings hold the chain.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
	const FRoadSegmentId RunwaySeg = Net->AddStraightSegment(RA, RB, Runway);

	const FGuidelineNodeId S = M2TrafficNode(*Net, 0.0, -20000.0);
	const FGuidelineNodeId H = M2TrafficNode(*Net, 0.0, -3000.0);
	const FGuidelineNodeId X = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId N = M2TrafficNode(*Net, 0.0, 20000.0);
	M2TrafficJoin(*Net, S, H); M2TrafficJoin(*Net, H, X); M2TrafficJoin(*Net, X, N);
	Net->GetGuidelineNodeMutable(H)->HoldShortFor = RunwaySeg;

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	// Someone holds the runway: a claim by a phantom agent 99, as a landing would make.
	{
		FTrafficClaim Hold; Hold.AgentId = 99; Hold.Resource = FTrafficResource::OfSurface(RunwaySeg); Hold.bOccupied = true; Hold.Rank = 2;
		FTrafficClaim Blocker;
		const_cast<FTrafficOccupancy&>(Traffic->GetOccupancy()).TryClaim(Hold, Blocker);
	}
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, S, N, ETraversalClass::Aircraft), M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);

	M2TrafficRun(*Traffic, *Net, 60.0, [&](int32) { return Traffic->FindAgent(Plane)->Follower.Speed > 1e-6 || Traffic->FindAgent(Plane)->Follower.Travelled < 1.0; });
	const FRoadAgent* P = Traffic->FindAgent(Plane);
	TestTrue(TEXT("stopped"), P->Follower.Speed < 1e-6);
	const double NoseAt = P->Follower.Travelled + Traffic->Rules.AircraftFootprint * 0.5;
	TestTrue(FString::Printf(TEXT("nose within 50 uu of the hold bar and not past it (nose %.0f, bar 17000)"), NoseAt),
		NoseAt <= 17000.0 + 1.0 && NoseAt >= 17000.0 - 50.0);
	TestEqual(TEXT("waiting on the runway's holder"), P->WaitingOn, 99);

	const_cast<FTrafficOccupancy&>(Traffic->GetOccupancy()).ReleaseAll(99);
	M2TrafficRun(*Traffic, *Net, 120.0, [&](int32) { return Traffic->FindAgent(Plane)->Phase != EAgentPhase::Parked; });
	TestEqual(TEXT("released, it crosses and arrives"), Traffic->FindAgent(Plane)->Phase, EAgentPhase::Parked);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficArrivalRefusedRunwayOccupiedTest,
	"Airside.Model.Traffic.ArrivalRefusedRunwayOccupied",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficArrivalRefusedRunwayOccupiedTest::RunTest(const FString& Parameters)
{
	// The dispatch path, not just the planner: a runway held in the traffic's OWN table
	// refuses through UGroundTraffic and fires the delegate with the new reason.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	const FRoadNodeId RA = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(120000.0, 0.0));
	const FRoadSegmentId RunwaySeg = Net->AddStraightSegment(RA, RB, Runway);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	TArray<EArrivalRefusal> Refusals;
	Traffic->OnArrivalRefused.AddLambda([&Refusals](EArrivalRefusal Why) { Refusals.Add(Why); });
	{
		FTrafficClaim Hold; Hold.AgentId = 99; Hold.Resource = FTrafficResource::OfSurface(RunwaySeg); Hold.bOccupied = true;
		FTrafficClaim Blocker;
		const_cast<FTrafficOccupancy&>(Traffic->GetOccupancy()).TryClaim(Hold, Blocker);
	}
	TestEqual(TEXT("refused"), Traffic->DispatchArrival(*Net, FVector2D(-1000.0, 0.0), UAirsideSettings::ResolveDefaultAirframe(), 1.0), 0);
	TestTrue(TEXT("with RunwayOccupied"), Refusals.Num() == 1 && Refusals[0] == EArrivalRefusal::RunwayOccupied);
	return true;
}
```
(The `const_cast` is confined to tests and stands in for the landing that Task 6 also makes claim the chain; a `FTrafficOccupancy& MutableOccupancyForTest()` accessor on `UGroundTraffic` is acceptable instead - add it and use it if preferred.)

- [ ] **Step 2: Run, see them fail** (plane drives through the bar and parks; arrival accepted).

- [ ] **Step 3: Implement** the four behaviours listed under Interfaces.

- [ ] **Step 4: Build, `-Filter Airside.Model.Traffic` green, full run (101 tests), commit**

Also confirm `Airside.Present.ArrivalDispatch` still passes: the arrival now holds the chain and releases it on Vacated; the log shows `released the runway`.
```bash
git add -A Plugins/Airside && git commit -m "feat(airside): runway surface held by landings, departures, runway edges and hold-short nodes"
```

---

### Task 7: Occupancy routing cost and `ReplanAt`

**Files:**
- Modify: `Private/Model/RouteSearch.cpp` (`EdgeCost` takes the query), `AirsideTests/Private/RouteStepDistanceTest.cpp` (+ OccupancyCost test)
- Modify: `Public/Model/GroundTraffic.h`, `Private/Model/GroundTraffic.cpp` (`ReplanAt`)
- Modify: `AirsideTests/Private/GroundTrafficTest.cpp` (+ Replan test)

**Interfaces:**
- `RouteSearch`: `EdgeCost(Network, Edge, EdgeId, Query)` = length + `Query.CongestionWeight · Query.Occupancy->HeldLengthOn(EdgeId, Query.QueryingAgent)` when `Occupancy` is non-null.
- `UGroundTraffic` (public, so tests and the rebuild can call it): `bool ReplanAt(int32 AgentId, const URoadNetwork& Network, int32 SpliceStep, FGuidelineEdgeId BannedEdge);` — searches from `StepFromNode(Plan, SpliceStep)` to `GoalNode` with the ban and the occupancy cost, splices with `RouteSearch::Splice(Plan, SpliceStep, Tail)`, `Follower.Replace`, `Occupancy.ReleaseAll(Id)`, resets `WaitingOn/BlockedStep/StalledSeconds`, logs `"Agent %d replanned at step %d: %.0f uu remaining -> %.0f"`. False (and nothing changed) when the search fails, the splice precondition fails, or the agent is not `Taxiing`. Vehicles and aircraft alike: the caller decides WHEN (deadlock, rebuild); this only does HOW.

- [ ] **Step 1: Write the failing tests**

Append to `RouteStepDistanceTest.cpp`:
```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteOccupancyCostTest,
	"Airside.Model.RouteSearch.OccupancyCost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteOccupancyCostTest::RunTest(const FString& Parameters)
{
	// The diamond again: south is shorter by 3900 uu. A queue of 2500 uu on the south edge at
	// weight 2 costs 5000, so the search must go north; with no table, or with the querier's
	// OWN claim, it must go south exactly as before.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId West = Net->AddGuidelineNode(FVector2D(-1000.0, 0.0));
	const FGuidelineNodeId East = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
	const FGuidelineNodeId North = Net->AddGuidelineNode(FVector2D(0.0, 4000.0));
	const FGuidelineNodeId South = Net->AddGuidelineNode(FVector2D(0.0, -100.0));
	M2StepJoin(*Net, West, North); M2StepJoin(*Net, North, East);
	const FGuidelineEdgeId WestSouth = M2StepJoin(*Net, West, South);
	M2StepJoin(*Net, South, East);

	FTrafficOccupancy Table;
	FTrafficClaim Queue; Queue.AgentId = 7; Queue.Resource = FTrafficResource::OfEdge(WestSouth); Queue.From = 0.0; Queue.To = 2500.0;
	FTrafficClaim Blocker;
	Table.TryClaim(Queue, Blocker);

	FRouteQuery Q; Q.Start = West; Q.Goal = East; Q.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Plain = RouteSearch::Find(*Net, Q);
	TestEqual(TEXT("no table: south"), Plain.Steps[0].To, South);

	Q.Occupancy = &Table; Q.QueryingAgent = 1; Q.CongestionWeight = 2.0;
	const FRoutePlan Costed = RouteSearch::Find(*Net, Q);
	TestEqual(TEXT("a queue on the south edge sends a stranger north"), Costed.Steps[0].To, North);

	Q.QueryingAgent = 7;
	const FRoutePlan Own = RouteSearch::Find(*Net, Q);
	TestEqual(TEXT("the queue's own agent is not charged for itself: south"), Own.Steps[0].To, South);
	TestEqual(TEXT("the null-table plan is the old plan to the byte"), Plain.Length, RouteSearch::Find(*Net, FRouteQuery(Q.Start, Q.Goal)).Length);
	return true;
}
```
(`FRouteQuery` has no two-argument constructor; write the last assertion with a fresh `FRouteQuery Bare; Bare.Start = West; Bare.Goal = East; Bare.Class = ETraversalClass::GroundVehicle;` and compare `Plain.Polyline == RouteSearch::Find(*Net, Bare).Polyline`.)

Append to `GroundTrafficTest.cpp`:
```cpp
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReplanTest,
	"Airside.Model.Traffic.Replan",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReplanTest::RunTest(const FString& Parameters)
{
	// A -> B -> C with a bypass B -> X -> C. A van under way on A->B is replanned at B with
	// B->C banned: it must keep driving the SAME line to B (no jump), then take X.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = M2TrafficNode(*Net, 20000.0, 0.0);
	const FGuidelineNodeId C = M2TrafficNode(*Net, 40000.0, 0.0);
	const FGuidelineNodeId X = M2TrafficNode(*Net, 30000.0, 15000.0);
	M2TrafficJoin(*Net, A, B);
	const FGuidelineEdgeId BC = M2TrafficJoin(*Net, B, C);
	M2TrafficJoin(*Net, B, X); M2TrafficJoin(*Net, X, C);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	M2TrafficRun(*Traffic, *Net, 8.0, [](int32) { return true; });
	const FRoadAgent* V = Traffic->FindAgent(Van);
	const FVector2D Before = V->LastMotion.Position;
	const double SpeedBefore = V->Follower.Speed;
	TestTrue(TEXT("moving, mid-edge"), SpeedBefore > 100.0 && V->Follower.Travelled < 20000.0);

	TestTrue(TEXT("replan accepted"), Traffic->ReplanAt(Van, *Net, 1, BC));
	V = Traffic->FindAgent(Van);
	TestEqual(TEXT("speed kept"), V->Follower.Speed, SpeedBefore, 1e-9);
	TestEqual(TEXT("three steps now: A->B, B->X, X->C"), V->Follower.Plan.Steps.Num(), 3);
	TestEqual(TEXT("the goal is unchanged"), V->GoalNode, C);

	double MaxJump = 0.0; FVector2D Last = Before; double MaxY = 0.0;
	M2TrafficRun(*Traffic, *Net, 200.0, [&](int32)
	{
		const FRoadAgent* Now = Traffic->FindAgent(Van);
		MaxJump = FMath::Max(MaxJump, FVector2D::Distance(Now->LastMotion.Position, Last));
		Last = Now->LastMotion.Position;
		MaxY = FMath::Max(MaxY, Now->LastMotion.Position.Y);
		return Now->Phase != EAgentPhase::Parked;
	});
	TestTrue(FString::Printf(TEXT("no tick moved it more than one frame at cruise (%.1f uu)"), MaxJump), MaxJump <= 1000.0 * 0.05 + 1.0);
	TestTrue(FString::Printf(TEXT("it went via X (max Y %.0f)"), MaxY), MaxY > 14000.0);
	TestEqual(TEXT("and arrived"), Traffic->FindAgent(Van)->Phase, EAgentPhase::Parked);
	return true;
}
```

- [ ] **Step 2: Build, run, see both fail.**

- [ ] **Step 3: Implement**

`RouteSearch.cpp`: change `EdgeCost` to `EdgeCost(const URoadNetwork& Network, const FGuidelineEdge& Edge, FGuidelineEdgeId EdgeId, const FRouteQuery& Query)`; after computing the length:
```cpp
		// Congestion: what others hold on this edge, weighted. Additive and non-negative,
		// so the straight-line heuristic stays admissible and the first pop stays optimal.
		// Nodes are not costed - a held node is a moment, a held edge is a queue.
		if (Query.Occupancy != nullptr)
		{
			Length += Query.CongestionWeight * Query.Occupancy->HeldLengthOn(EdgeId, Query.QueryingAgent);
		}
```
and `#include "Model/TrafficOccupancy.h"`. `GroundTraffic.cpp`:
```cpp
bool UGroundTraffic::ReplanAt(int32 AgentId, const URoadNetwork& Network, int32 SpliceStep, FGuidelineEdgeId BannedEdge)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE) { return false; }
	FRoadAgent& Agent = Agents[Index];
	const FRoutePlan& Plan = Agent.Follower.Plan;
	if (Agent.Phase != EAgentPhase::Taxiing || !Plan.IsValid() || SpliceStep < 0 || SpliceStep > Plan.Steps.Num()) { return false; }

	FRouteQuery Query;
	Query.Start = StepFromNode(Plan, SpliceStep);
	Query.Goal = Agent.GoalNode;
	Query.Class = Agent.Class;
	Query.Wingspan = Agent.Airframe.Wingspan;
	Query.BannedEdge = BannedEdge;
	Query.Occupancy = &Occupancy;
	Query.QueryingAgent = AgentId;
	Query.CongestionWeight = Rules.CongestionWeight;
	const FRoutePlan Tail = RouteSearch::Find(Network, Query);
	if (!Tail.IsValid()) { return false; }

	const FRoutePlan Spliced = RouteSearch::Splice(Plan, SpliceStep, Tail);
	if (!Spliced.IsValid()) { return false; }

	const double WasRemaining = Plan.Length - Agent.Follower.Travelled;
	Agent.Follower.Replace(Spliced);
	Occupancy.ReleaseAll(AgentId);
	Agent.WaitingOn = 0;
	Agent.BlockedStep = -1;
	Agent.StalledSeconds = 0.0;
	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d replanned at step %d: %.0f uu remaining -> %.0f"),
		AgentId, SpliceStep, WasRemaining, Spliced.Length - Agent.Follower.Travelled);
	return true;
}
```

- [ ] **Step 4: Build, `-Filter Airside.Model` green, full run (103), commit**

```bash
git add -A Plugins/Airside && git commit -m "feat(airside): occupancy-costed routing; ReplanAt splices a new tail onto a moving agent"
```

---

### Task 8: Deadlock detection and resolution

**Files:**
- Modify: `Public/Model/GroundTraffic.h`, `Private/Model/GroundTraffic.cpp` (`ResolveDeadlocks`)
- Modify: `AirsideTests/Private/GroundTrafficTest.cpp` (+ DeadlockTriangle; extend HeadOnStops to assert the log-once-and-replan-if-a-turn-exists case)

**Interfaces:**
- Private `void ResolveDeadlocks(const URoadNetwork& Network);` called at the end of `Advance` when `Network != nullptr`.
- Private `bool CanReplanAtBlockedStep(const FRoadAgent& Agent) const`: `Taxiing`, `Follower.Speed < KINDA_SMALL_NUMBER`, `BlockedStep >= 0`, and `StepStart(Plan, BlockedStep) − Travelled` in `[−KINDA_SMALL_NUMBER, GapFor(Class) + FootprintFor(Class)/2]`.
- Public read-only for tests: `int32 GetCyclesDetectedForTest() const` (count of distinct cycles logged this session) and `int32 GetLastResolvedAgentForTest() const`.

**Algorithm.** Build `Waiting: TMap<int32,int32>` from agents with `StalledSeconds > Rules.StallSeconds` and `WaitingOn != 0`. For each such agent not yet visited: walk `WaitingOn` links collecting the path; if the walk reaches an agent already on the current path, the cycle is the path from that agent onward. Cycle key = min id; `TSet<int32> Seen` per tick prevents double handling. For a cycle: if every member has `LastResolveAttempt > SimSeconds − Rules.RetrySeconds`, skip (already tried recently). Else: candidates = members with `CanReplanAtBlockedStep`; pick min by (`TraversalPriority(Class)` asc, `Id` desc). Stamp `LastResolveAttempt = SimSeconds` on EVERY member. If a candidate exists: `ReplanAt(candidate, Network, BlockedStep, Steps[BlockedStep].Edge)`; on success log `"Deadlock among agents [%s] resolved: agent %d replans"`; else log `"Deadlock among agents [%s]: no member can turn; retrying in %.0f s"` at Warning. If all members are `Aircraft`, the log line is prefixed `"All-aircraft "` and is at Warning regardless (the build-tool warning's input). Log once per cycle per retry window - the `LastResolveAttempt` stamp is what guarantees it.

- [ ] **Step 1: Write the failing test** (append to `GroundTrafficTest.cpp`)

```cpp
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficDeadlockTriangleTest,
	"Airside.Model.Traffic.DeadlockTriangle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficDeadlockTriangleTest::RunTest(const FString& Parameters)
{
	// One compound junction: a triangle of one-way 600 uu lanes A->B->C->A, shorter than a
	// van's footprint + gap (800), so the box-entry rule applies to every edge. Three vans
	// start ON the nodes, each bound for the node the next van stands on: V1 A->C via B,
	// V2 B->A via C, V3 C->B via A. Nobody may enter its first edge while the far node is
	// occupied, so all three are stopped at t = 0 waiting on each other - a genuine cycle.
	// One escape: C->X->B, longer than C->A->B so V3 does not take it unprompted.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = M2TrafficNode(*Net, 600.0, 0.0);
	const FGuidelineNodeId C = M2TrafficNode(*Net, 300.0, 519.6);
	const FGuidelineNodeId X = M2TrafficNode(*Net, 1400.0, 519.6);
	M2TrafficJoin(*Net, A, B, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, B, C, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, C, A, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, C, X, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, X, B, EGuidelineDir::AToB);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 V1 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V2 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, B, A, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V3 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, C, B, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	TestEqual(TEXT("V3's first plan goes via A (2 steps), not the escape"), Traffic->FindAgent(V3)->Follower.Plan.Steps.Num(), 2);

	// All three stopped, each waiting on the next, before any resolution.
	M2TrafficRun(*Traffic, *Net, 1.0, [](int32) { return true; });
	TestTrue(TEXT("V1 waits on V2"), Traffic->FindAgent(V1)->WaitingOn == V2);
	TestTrue(TEXT("V2 waits on V3"), Traffic->FindAgent(V2)->WaitingOn == V3);
	TestTrue(TEXT("V3 waits on V1"), Traffic->FindAgent(V3)->WaitingOn == V1);
	TestTrue(TEXT("nobody has moved"), Traffic->FindAgent(V1)->Follower.Travelled < 1.0 && Traffic->FindAgent(V2)->Follower.Travelled < 1.0 && Traffic->FindAgent(V3)->Follower.Travelled < 1.0);

	double MaxJump = 0.0;
	TMap<int32, FVector2D> Last;
	int32 ResolvedAtTick = -1;
	M2TrafficRun(*Traffic, *Net, 120.0, [&](int32 Tick)
	{
		bool bAllParked = true;
		for (const FRoadAgent& Agent : Traffic->GetAgents())
		{
			if (const FVector2D* Prev = Last.Find(Agent.Id)) { MaxJump = FMath::Max(MaxJump, FVector2D::Distance(*Prev, Agent.LastMotion.Position)); }
			Last.Add(Agent.Id, Agent.LastMotion.Position);
			bAllParked &= (Agent.Phase == EAgentPhase::Parked);
		}
		if (ResolvedAtTick < 0 && Traffic->GetLastResolvedAgentForTest() != 0) { ResolvedAtTick = Tick; }
		return !bAllParked;
	});
	TestEqual(TEXT("exactly one cycle was detected"), Traffic->GetCyclesDetectedForTest(), 1);
	TestTrue(FString::Printf(TEXT("detected within StallSeconds + one tick (tick %d)"), ResolvedAtTick), ResolvedAtTick >= 0 && ResolvedAtTick <= static_cast<int32>(Traffic->Rules.StallSeconds / 0.05) + 2);
	TestEqual(TEXT("the agent that replanned is the highest id"), Traffic->GetLastResolvedAgentForTest(), V3);
	// Spliced at step 0 (V3 never left C), so the new plan IS the tail: C->X, X->B.
	TestEqual(TEXT("V3 now has two steps via X"), Traffic->FindAgent(V3) ? Traffic->FindAgent(V3)->Follower.Plan.Steps.Num() : 0, 2);
	TestEqual(TEXT("the first of which goes to X"), Traffic->FindAgent(V3)->Follower.Plan.Steps[0].To, X);
	for (const int32 Id : { V1, V2, V3 })
	{
		TestEqual(FString::Printf(TEXT("agent %d reached its goal"), Id), Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked);
	}
	TestTrue(FString::Printf(TEXT("no agent moved more than one frame's travel in any tick (%.1f uu)"), MaxJump), MaxJump <= 1000.0 * 0.05 + 1.0);
	return true;
}
```
Extend `FTrafficHeadOnStopsTest`: after the run, `TestEqual(TEXT("the all-aircraft cycle was detected once"), Traffic->GetCyclesDetectedForTest(), 1);` and `TestEqual(TEXT("nobody could turn, so nobody replanned"), Traffic->GetLastResolvedAgentForTest(), 0);`.

- [ ] **Step 2: Build, run, see them fail** (no cycle count; the triangle never resolves).

- [ ] **Step 3: Implement** per the algorithm; `GetCyclesDetectedForTest` counts distinct cycle keys ever logged (a `TSet<int32> CyclesSeen` keyed by min id, `Transient`, not a UPROPERTY since it is test-facing bookkeeping - say so).

- [ ] **Step 4: Build, `-Filter Airside.Model.Traffic` green, full run (104), commit**

```bash
git add -A Plugins/Airside && git commit -m "feat(airside): wait-for cycle detection; lowest-ranked waiter at a node replans round the block"
```

---

### Task 9: Graph rebuild re-resolution

**Files:**
- Modify: `Public/Model/GroundTraffic.h`, `Private/Model/GroundTraffic.cpp` (`OnGraphRebuilt`, `ReResolvePlan`)
- Modify: `Public/Present/AirsideTraffic.h` / `.cpp` (`OnGraphRebuilt` forwarder), `Private/Present/RoadNetworkActor.cpp` (`RebuildMesh` calls it)
- Modify: `AirsideTests/Private/GroundTrafficTest.cpp` (+ GraphRebuild)

**Interfaces:**
- Public `void UGroundTraffic::OnGraphRebuilt(const URoadNetwork& Network);`
- Private `enum class EReResolve { Intact, Replanned, Truncated, Stranded }; EReResolve ReResolvePlan(FRoadAgent& Agent, FRoutePlan& Plan, int32 FromStep, const URoadNetwork& Network);` applied to `Follower.Plan` (from `CurrentStep`) for `Taxiing` agents and to `TaxiInPlan` (from 0) for `Arriving` ones.
- `UAirsideTraffic::OnGraphRebuilt(const URoadNetwork&)` forwards. `ARoadNetworkActor::RebuildMesh` calls `Traffic->OnGraphRebuilt(*Network)` after `Presenter->Rebuild(...)` when both exist.

**Algorithm (`ReResolvePlan`).** `Prev = FindNearestNode(Network, Polyline[FromStep == 0 ? 0 : Steps[FromStep-1].EndVertex], Class, ResolveRadius)`; if unset → `Stranded` (the pavement under the agent is gone: `Plan.Result = Unreachable`, `Occupancy.ReleaseAll`). Else for `i = FromStep..`: `Next = FindNearestNode(Network, Polyline[Steps[i].EndVertex], Class, ResolveRadius)`; edge = an id in `Network.GetOutgoingGuidelines(Prev, Class)` whose other end is `Next` (set `bReversed` from `Edge->B == Prev`). If found: write `Steps[i].Edge/To/bReversed`; `Prev = Next`; continue. If not: `k = i`; break. `GoalNode = FindNearestNode(Polyline.Last())` if it resolved, else unchanged. Write `Plan.Start = FindNearestNode(Polyline[0])` when `FromStep == 0` and it resolves. If all resolved → `Intact`. Else if `ReplanAt(Id, Network, k, unset)` succeeds → `Replanned` (for `TaxiInPlan`, do the search + `Splice` directly since `ReplanAt` works on `Follower.Plan`; write a small shared helper `SpliceReplan(Plan, k, Query)` both use). Else truncate: `Steps.SetNum(k)`, `Polyline.SetNum(Steps.Last().EndVertex + 1)` (or 2 points when `k == 0` and the agent is on step 0 — then `Stranded` instead), `Length = Steps.Last().EndDistance`, `GoalNode = Steps.Last().To`, `Follower.Replace` → `Truncated`. `OnGraphRebuilt` ends with `Occupancy.Clear()` and one log: `"Graph rebuilt: %d agents re-resolved, %d replanned, %d truncated, %d stranded"`.

- [ ] **Step 1: Write the failing test** (append to `GroundTrafficTest.cpp`)

```cpp
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficGraphRebuildTest,
	"Airside.Model.Traffic.GraphRebuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficGraphRebuildTest::RunTest(const FString& Parameters)
{
	// What FRoadGuidelineBuilder::Build does to the graph, done by hand: every derived node
	// and edge removed and re-added at the same positions with NEW handles. Three cases:
	//   1. same geometry -> the agent's step handles are re-pointed and it never notices;
	//   2. the edge ahead is gone but a bypass exists -> replanned over the bypass;
	//   3. the edge ahead is gone and nothing replaces it -> stops at the last live node.
	auto Build = [](URoadNetwork& Net, bool bKeepBC, bool bBypass, FGuidelineNodeId* OutA, FGuidelineNodeId* OutC)
	{
		// Sweep everything (a rebuild removes derived edges, then idle derived nodes).
		TArray<FGuidelineEdgeId> Edges;
		for (int32 I = 0; I < Net.GetGuidelineEdges().Num(); ++I) { if (Net.GetGuidelineEdges()[I].bAlive) { FGuidelineEdgeId Id; Id.Index = I; Id.Generation = Net.GetGuidelineEdges()[I].Generation; Edges.Add(Id); } }
		for (const FGuidelineEdgeId& Id : Edges) { Net.RemoveGuidelineEdge(Id); }
		for (int32 I = 0; I < Net.GetGuidelineNodes().Num(); ++I) { if (Net.GetGuidelineNodes()[I].bAlive) { Net.RemoveGuidelineNode(Net.GuidelineNodeIdAt(I)); } }
		const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0));
		const FGuidelineNodeId C = Net.AddGuidelineNode(FVector2D(40000.0, 0.0));
		M2TrafficJoin(Net, A, B);
		if (bKeepBC) { M2TrafficJoin(Net, B, C); }
		if (bBypass) { const FGuidelineNodeId D = Net.AddGuidelineNode(FVector2D(30000.0, 8000.0)); M2TrafficJoin(Net, B, D); M2TrafficJoin(Net, D, C); }
		if (OutA) { *OutA = A; } if (OutC) { *OutC = C; }
	};

	auto Dispatch = [&](URoadNetwork& Net, UGroundTraffic& Traffic, FGuidelineNodeId A, FGuidelineNodeId C)
	{
		const int32 Id = Traffic.DispatchAgent(&Net, M2TrafficRoute(Net, A, C, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
		M2TrafficRun(Traffic, Net, 5.0, [](int32) { return true; });   // a few thousand uu along A->B
		return Id;
	};

	// Case 1: same geometry.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId A, C; Build(*Net, true, false, &A, &C);
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Van = Dispatch(*Net, *Traffic, A, C);
		const FVector2D Before = Traffic->FindAgent(Van)->LastMotion.Position;
		const FGuidelineEdgeId OldEdge = Traffic->FindAgent(Van)->Follower.Plan.Steps[1].Edge;
		Build(*Net, true, false, nullptr, nullptr);
		TestNull(TEXT("the old handle is dead after the rebuild"), Net->GetGuidelineEdge(OldEdge));
		Traffic->OnGraphRebuilt(*Net);
		Traffic->Advance(0.05, Net);
		const FRoadAgent* V = Traffic->FindAgent(Van);
		TestNotNull(TEXT("step 1's edge handle is live again"), Net->GetGuidelineEdge(V->Follower.Plan.Steps[1].Edge));
		TestTrue(TEXT("position moved by at most one tick across the rebuild"), FVector2D::Distance(V->LastMotion.Position, Before) <= 1000.0 * 0.05 + 1.0);
		M2TrafficRun(*Traffic, *Net, 120.0, [&](int32) { return Traffic->FindAgent(Van)->Phase != EAgentPhase::Parked; });
		TestEqual(TEXT("arrives"), Traffic->FindAgent(Van)->Phase, EAgentPhase::Parked);
		TestTrue(TEXT("at C"), FVector2D::Distance(Traffic->FindAgent(Van)->LastMotion.Position, FVector2D(40000.0, 0.0)) < 10.0);
	}
	// Case 2: B->C deleted, bypass added.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId A, C; Build(*Net, true, false, &A, &C);
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Van = Dispatch(*Net, *Traffic, A, C);
		Build(*Net, false, true, nullptr, nullptr);
		Traffic->OnGraphRebuilt(*Net);
		double MaxY = 0.0;
		M2TrafficRun(*Traffic, *Net, 150.0, [&](int32) { MaxY = FMath::Max(MaxY, Traffic->FindAgent(Van)->LastMotion.Position.Y); return Traffic->FindAgent(Van)->Phase != EAgentPhase::Parked; });
		TestEqual(TEXT("arrives over the bypass"), Traffic->FindAgent(Van)->Phase, EAgentPhase::Parked);
		TestTrue(FString::Printf(TEXT("via D (max Y %.0f)"), MaxY), MaxY > 7000.0);
	}
	// Case 3: B->C deleted, nothing replaces it.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		FGuidelineNodeId A, C; Build(*Net, true, false, &A, &C);
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Van = Dispatch(*Net, *Traffic, A, C);
		Build(*Net, false, false, nullptr, nullptr);
		Traffic->OnGraphRebuilt(*Net);
		M2TrafficRun(*Traffic, *Net, 120.0, [&](int32) { return Traffic->FindAgent(Van)->Phase != EAgentPhase::Parked; });
		TestEqual(TEXT("stops at the last live node"), Traffic->FindAgent(Van)->Phase, EAgentPhase::Parked);
		TestTrue(TEXT("which is B"), FVector2D::Distance(Traffic->FindAgent(Van)->LastMotion.Position, FVector2D(20000.0, 0.0)) < 10.0);
	}
	return true;
}
```
(`RemoveGuidelineNode` cascades to incident edges, so the edge loop may already have emptied it; the node loop is what matters. `GuidelineNodeIdAt` exists on `URoadNetwork`.)

- [ ] **Step 2: Build, run, see it fail** (`OnGraphRebuilt` undeclared).

- [ ] **Step 3: Implement** per the algorithm. Actor: in `RebuildMesh`, after `Presenter->Rebuild(*Network, MakeSurfaceSettings());`:
```cpp
	// The guideline graph was just regenerated with new handles. Every agent's route must be
	// re-pointed at the nodes that now hold its positions, or the occupancy table would be
	// keyed on slots the builder has already freed - see UGroundTraffic::OnGraphRebuilt.
	if (Traffic != nullptr)
	{
		Traffic->OnGraphRebuilt(*Network);
	}
```

- [ ] **Step 4: Build, `-Filter Airside` full (105), commit**

`Airside.Present.MeshIsFreshAfterLoad` and `ArrivalDispatch` call `RebuildMesh` with agents present: confirm they still pass and the log shows `Graph rebuilt: ...`.
```bash
git add -A Plugins/Airside && git commit -m "feat(airside): agents survive a guideline rebuild - steps re-resolved by position, replanned or truncated"
```

---

### Task 10: Hold-short marks — model, builder, facade

**Files:**
- Modify: `Public/Model/RoadGuideline.h` (`FHoldShortMark`), `Public/Model/RoadNetwork.h`, `Private/Model/RoadNetwork.cpp` (`HoldShortMarks`, `SetHoldShort`, `RunwayNearGuidelineNode`)
- Modify: `Private/Build/RoadGuidelineBuilder.cpp` (re-apply pass before the sweep)
- Modify: `Public/Tool/RoadEditTarget.h`, `Public/Present/RoadEditFacade.h`, `Private/Present/RoadEditFacade.cpp`, `Public/Present/RoadNetworkActor.h`, `Private/Present/RoadNetworkActor.cpp` (`SetHoldShort`)
- Create: `AirsideTests/Private/HoldShortMarkTest.cpp`

**Interfaces:**
- `RoadGuideline.h`:
```cpp
/** A player-placed hold bar, stored by IDENTITY so it survives the rebuild. Spec §6. */
USTRUCT() struct AIRSIDE_API FHoldShortMark {
    GENERATED_BODY()
    UPROPERTY() FGuidelineEndRef At;        // which derived node: the same key the builder's Ends map uses
    UPROPERTY() FRoadSegmentId Protects;    // the runway segment (any of its chain)
};
```
- `URoadNetwork`: `UPROPERTY() TArray<FHoldShortMark> HoldShortMarks;` (private) with `const TArray<FHoldShortMark>& GetHoldShortMarks() const`; `bool SetHoldShort(FGuidelineNodeId Node, FRoadSegmentId Protects)` — writes `HoldShortFor` on the node; if the node's `Origin.IsSet()`, upserts (or removes, when `Protects` is unset) the mark keyed by `Origin`; false when the node is dead. `FRoadSegmentId RunwayNearGuidelineNode(FGuidelineNodeId Node) const` — the `DerivedFrom` of the first incident edge that is a runway segment, else the same test over each neighbour node's incident edges, else unset.
- `FRoadGuidelineBuilder::Build`: after the turn-path loop and hand-authored edge re-resolution, before the orphan sweep: for each mark, `Ends.Find(EndKey(At.Segment.Index, At.bEndA, At.GuidelineIndex))` → set that node's `HoldShortFor = Protects` if `Network.IsRunwaySegment(Protects)`; marks whose `At.Segment` is dead or whose `Protects` is no longer a runway are removed (`URoadNetwork::PruneHoldShortMarks()` — a public helper the builder calls; the network owns its own invariant).
- `IRoadEditTarget`: `virtual bool SetHoldShort(int32 NodeIndex, int32 SegmentIndex) = 0;` (`SegmentIndex == INDEX_NONE` clears). Facade: `FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("hold short"))`, validates both indices are live, calls `Network->SetHoldShort`, `Commit()`, logs `"Hold short %s at guideline node %d for segment %d"`; no `OnChanged` broadcast (the overlay reads the flag; nothing in the mesh changes). Actor: `UFUNCTION(BlueprintCallable, Category = "Airside") virtual bool SetHoldShort(int32 NodeIndex, int32 SegmentIndex) override;` forwarding to the facade.

- [ ] **Step 1: Write the failing test**

`AirsideTests/Private/HoldShortMarkTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The guideline node derived for one end of Segment, or unset. */
	FGuidelineNodeId M2HoldNodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA)
	{
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].Origin.Segment == Segment && Nodes[Index].Origin.bEndA == bEndA)
			{
				return Net.GuidelineNodeIdAt(Index);
			}
		}
		return FGuidelineNodeId();
	}

	void M2HoldRebuild(URoadNetwork& Net)
	{
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(Net);
		FRoadGuidelineBuilder::Build(Net, Solved);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoldShortSurvivesRebuildTest,
	"Airside.Build.HoldShortSurvivesRebuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHoldShortSurvivesRebuildTest::RunTest(const FString& Parameters)
{
	// A runway with a taxiway joining it at E. The taxiway's end node at E is the hold-short
	// candidate. The claim under test is spec §6: the flag is stored by identity and the
	// builder re-applies it, so a rebuild - which allocates FRESH nodes - keeps the bar.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net->AddNode(FVector2D(100000.0, 0.0));
	const FRoadSegmentId R1 = Net->AddStraightSegment(T, E, Runway);
	Net->AddStraightSegment(E, F, Runway);
	const FRoadNodeId X = Net->AddNode(FVector2D(60000.0, -20000.0));
	const FRoadSegmentId Tx = Net->AddStraightSegment(E, X, Taxiway);
	M2HoldRebuild(*Net);

	const FGuidelineNodeId Bar = M2HoldNodeFor(*Net, Tx, /*bEndA=*/true);
	if (!TestTrue(TEXT("the taxiway's E-end guideline node exists"), Bar.IsSet())) { return false; }

	TestEqual(TEXT("the runway near that node is the one it joins"), Net->RunwayNearGuidelineNode(Bar).Index, R1.Index);
	TestFalse(TEXT("the far end of the taxiway is near no runway"), Net->RunwayNearGuidelineNode(M2HoldNodeFor(*Net, Tx, false)).IsSet());

	TestTrue(TEXT("set"), Net->SetHoldShort(Bar, R1));
	TestEqual(TEXT("the flag is on the node"), Net->GetGuidelineNode(Bar)->HoldShortFor, R1);
	TestEqual(TEXT("one mark recorded"), Net->GetHoldShortMarks().Num(), 1);

	M2HoldRebuild(*Net);
	TestNull(TEXT("the old node handle is dead - the builder made a fresh one"), Net->GetGuidelineNode(Bar));
	const FGuidelineNodeId BarAfter = M2HoldNodeFor(*Net, Tx, true);
	if (!TestTrue(TEXT("the fresh node exists"), BarAfter.IsSet())) { return false; }
	TestEqual(TEXT("and carries the flag"), Net->GetGuidelineNode(BarAfter)->HoldShortFor, R1);
	TestTrue(TEXT("and has edges - it is connected, not an orphan kept alive"), Net->GetGuidelineNode(BarAfter)->Incident.Num() > 0);

	TestTrue(TEXT("clear"), Net->SetHoldShort(BarAfter, FRoadSegmentId()));
	TestFalse(TEXT("flag gone"), Net->GetGuidelineNode(BarAfter)->HoldShortFor.IsSet());
	TestEqual(TEXT("mark gone"), Net->GetHoldShortMarks().Num(), 0);

	// A mark whose runway was deleted is pruned rather than left pointing at nothing.
	Net->SetHoldShort(BarAfter, R1);
	Net->RemoveSegment(R1);
	M2HoldRebuild(*Net);
	TestEqual(TEXT("mark pruned with its runway"), Net->GetHoldShortMarks().Num(), 0);
	return true;
}

#endif
```

- [ ] **Step 2: Build, see it fail.**

- [ ] **Step 3: Implement** the model, builder and facade pieces listed under Interfaces. In the builder the re-apply block sits immediately before the `// LAST, for the reason given where this used to live` sweep, with this comment:
```cpp
	// --- Re-apply hold-short marks ------------------------------------------------------
	//
	// The flag lives on a node and every derived node above is FRESH, so a bar the player
	// placed would vanish on the next road edit. The mark is stored by the same identity
	// a hand-authored edge stores its ends by, and resolved through the same Ends map -
	// one source (the mark), one cache (the flag), rebuilt together. Spec 2026-09-06 §6.
```

- [ ] **Step 4: Build, `-Filter Airside` full (106), commit**

```bash
git add -A Plugins/Airside && git commit -m "feat(airside): hold-short marks stored by identity; builder re-applies them; facade SetHoldShort"
```

---

### Task 11: Hold-short tool, overlay, drivers

**Files:**
- Create: `Public/Tool/HoldShortTool.h`, `Private/Tool/HoldShortTool.cpp`
- Modify: `Public/Tool/RoadBuildTool.h` (`EPreviewStyle::HoldShort`), `Private/Tool/GuidelineOverlay.cpp`, `Private/Tool/BuildSession.cpp` (registry)
- Modify: `Source/AirportMgr/RoadBuildHUD.h` / `.cpp` (`HoldShortColour`), `Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEditorTool.cpp` (colour), `Plugins/Airside/Source/AirsideEditor/Public/RoadBuildEdModeCommands.h` / `Private/RoadBuildEdModeCommands.cpp` (`PlaceHoldShort`)
- Create: `AirsideTests/Private/HoldShortToolTest.cpp`

**Interfaces:**
```cpp
class AIRSIDE_API FHoldShortTool : public IBuildTool {
public:
    virtual FText GetDisplayName() const override;          // "Hold short"
    virtual void OnClick(const FToolContext& Context) override;
    virtual void OnCancel(const FToolContext& Context) override {}
    virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
    virtual bool IsIdle() const override { return true; }
    /** Why the last click was refused, or empty. Shown by the preview at the cursor. */
    FString LastRefusal;
private:
    FGuidelineNodeId PickNode(const FToolContext& Context) const;   // FindNearestNode, Aircraft, Context.SnapRadius
};
```
- `OnClick`: pick; none → return silently. Node already flagged → `SetHoldShort(index, INDEX_NONE)`, `LastRefusal.Empty()`. Else `RunwayNearGuidelineNode(node)`; unset → `LastRefusal = "No runway within one edge of this node"` and return; else `SetHoldShort(node.Index, runway.Index)`.
- `BuildPreview`: hovered pickable node → `Marker(Snap)`; if `LastRefusal` non-empty → `Label(Cursor, LastRefusal, Refused)`.
- `EPreviewStyle::HoldShort` appended with comment `/** A hold bar: a player-placed line an aircraft stops at. Context, like Guideline, but authored. */`.
- `GuidelineOverlay::Draw`: after the node loop, for each alive node with `HoldShortFor.IsSet()`: `Along` = direction from the node to the other end of its first incident edge (fall back to `(1,0)`); `Sink.CrossMark(Node.Position, Along, EPreviewStyle::HoldShort)`.
- Registry: `{ EKeys::Eight, LOCTEXT("HoldShort", "Hold short"), [] { return MakeUnique<FHoldShortTool>(); } }` appended (key order is the table order). `BuildActions()` picks it up unchanged; the banner and the bar follow.
- HUD: `UPROPERTY(EditAnywhere, Category = "Airside|Preview") FLinearColor HoldShortColour = FLinearColor(1.0f, 0.8f, 0.1f);` and a `case EPreviewStyle::HoldShort: return HoldShortColour;`. Editor colour map: the same value. Editor commands: `UI_COMMAND(PlaceHoldShort, "Hold short", "Click a taxiway node beside a runway to place a hold bar; click it again to remove it.", EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::Eight));` after `PlaceRunways`, the member after `PlaceRunways`, and `ToolCommandsInOrder` returns `{ ..., PlaceRunways, PlaceHoldShort }`.

- [ ] **Step 1: Write the failing test**

`AirsideTests/Private/HoldShortToolTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/BuildSession.h"
#include "Tool/GuidelineOverlay.h"
#include "Tool/HoldShortTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FM2HoldSink : IToolPreviewSink
	{
		int32 HoldBars = 0; int32 Refused = 0; FString LastLabel;
		virtual void Marker(const FVector2D&, EPreviewStyle) override {}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle Style) override { if (Style == EPreviewStyle::HoldShort) { ++HoldBars; } }
		virtual void Label(const FVector2D&, const FString& Text, EPreviewStyle Style) override { if (Style == EPreviewStyle::Refused) { ++Refused; LastLabel = Text; } }
	};

	FGuidelineNodeId M2HoldToolNodeFor(const URoadNetwork& Net, FRoadSegmentId Segment, bool bEndA)
	{
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].Origin.Segment == Segment && Nodes[Index].Origin.bEndA == bEndA) { return Net.GuidelineNodeIdAt(Index); }
		}
		return FGuidelineNodeId();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoldShortToolTest,
	"Airside.Tool.HoldShort",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHoldShortToolTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;

	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
	const FRoadNodeId T = Net.AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId E = Net.AddNode(FVector2D(60000.0, 0.0));
	const FRoadNodeId F = Net.AddNode(FVector2D(100000.0, 0.0));
	const FRoadSegmentId R1 = Net.AddStraightSegment(T, E, Runway);
	Net.AddStraightSegment(E, F, Runway);
	const FRoadNodeId X = Net.AddNode(FVector2D(60000.0, -20000.0));
	const FRoadSegmentId Tx = Net.AddStraightSegment(E, X, Taxiway);
	Actor->RebuildMesh();

	const FGuidelineNodeId Bar = M2HoldToolNodeFor(Net, Tx, true);
	const FGuidelineNodeId Far = M2HoldToolNodeFor(Net, Tx, false);
	if (!TestTrue(TEXT("both taxiway end nodes exist"), Bar.IsSet() && Far.IsSet())) { return false; }

	// The registry knows the tool, under key 8, and the session builds it.
	TestTrue(TEXT("registered under Eight"), ToolRegistry().Last().Key == EKeys::Eight);
	FBuildSession Session;
	TestEqual(TEXT("the session holds every registered tool"), Session.NumTools(), ToolRegistry().Num());

	FHoldShortTool Tool;
	FToolContext Ctx;
	Ctx.Target = Actor;
	Ctx.SnapRadius = 400.0;

	Ctx.Cursor = Net.GetGuidelineNode(Bar)->Position;
	Tool.OnClick(Ctx);
	TestEqual(TEXT("click sets the bar for the runway it joins"), Net.GetGuidelineNode(Bar)->HoldShortFor.Index, R1.Index);
	FM2HoldSink Sink;
	GuidelineOverlay::Draw(Net, Sink);
	TestEqual(TEXT("the overlay draws one hold bar"), Sink.HoldBars, 1);

	TestTrue(TEXT("undoable"), Actor->Undo());
	TestFalse(TEXT("undo clears it"), Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, true))->HoldShortFor.IsSet());
	TestTrue(TEXT("redo"), Actor->Redo());
	TestTrue(TEXT("redo restores it"), Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, true))->HoldShortFor.IsSet());

	Ctx.Cursor = Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, true))->Position;
	Tool.OnClick(Ctx);
	TestFalse(TEXT("second click clears"), Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, true))->HoldShortFor.IsSet());

	Ctx.Cursor = Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, false))->Position;
	Tool.OnClick(Ctx);
	TestFalse(TEXT("a node with no runway near is refused"), Actor->Network->GetGuidelineNode(M2HoldToolNodeFor(*Actor->Network, Tx, false))->HoldShortFor.IsSet());
	TestFalse(TEXT("with a reason"), Tool.LastRefusal.IsEmpty());
	FM2HoldSink Sink2;
	Tool.BuildPreview(Ctx, Sink2);
	TestEqual(TEXT("the preview shows the refusal"), Sink2.Refused, 1);
	return true;
}

#endif
```
(Undo/Redo return fresh network objects; the test re-finds the node through `Actor->Network` each time, deliberately.)

- [ ] **Step 2: Build, see it fail.**

- [ ] **Step 3: Implement** everything under Interfaces. `HoldShortTool.h` class comment: what it is (one click, no state, the only tool that edits a NODE rather than pavement), why the runway is found by the network and not the tool (`RunwayNearGuidelineNode` is a fact about the graph, testable without a tool), and why a second click clears rather than a modifier (one gesture, no sticky mode to forget).

- [ ] **Step 4: Editor mode list agrees**

`RoadBuildEdMode.cpp` compares `ToolCommandsInOrder()` against `ToolRegistry()` by name and logs a mismatch. Confirm by grep that the names match: registry `"Hold short"`, command `"Hold short"`.

- [ ] **Step 5: Build (all three modules), `Run-AirsideTests.ps1` full (107), commit**

`AirportMgr.Actions.*` tests must still pass (the registry grew by one).
```bash
git add -A Plugins Source && git commit -m "feat(tool): hold-short tool on key 8; overlay draws hold bars; both drivers list it"
```

---

### Task 12: Runtime evidence, docs, PR

**Files:**
- Modify: `docs/superpowers/specs/2026-09-06-ground-traffic-design.md` (status line → implemented; any amendment discovered during execution recorded in §1 with a date)
- Modify: `docs/superpowers/handovers/2026-09-06-m2-ground-traffic.md` (append "Outcome" section: what shipped, test count, deviations)

- [ ] **Step 1: Counts**

```bash
grep -rc "UE_LOG(" Plugins/Airside/Source/Airside/Private Plugins/Airside/Source/Airside/Public | awk -F: '{s+=$2} END {print s}'
```
Must be ≥ 71 plus every line this plan added. Comment-line delta for `AirsideTraffic.h/.cpp` + `GroundTraffic.h/.cpp` versus the Task 4 Step 1 figure: not lower.

- [ ] **Step 2: Full cold run**

`./Tools/Run-AirsideTests.ps1` → expect `107 test(s) run, 0 failed, 0 crashed`. Quote the line.

- [ ] **Step 3: Runtime evidence in PIE**

Open the editor, PIE, draw a runway, a taxiway crossing it, two stands. Press 8 and click the taxiway node beside the runway: a hold bar appears. Press 7 twice. Expected in `Saved/Logs/AirportMgr.log`:
- `Arrival refused: the runway is in use` for the second 7 while the first is rolling, OR both land in sequence if the first has vacated.
- `Agent N holding short at node ...` if an aircraft reaches the bar while the other is on the runway.
- `Agent N stops ... short of node ... held by agent M` and `Agent N resumes` at a junction.
Take `python Tools/Mcp.py shot m2.png` with two aircraft on the field. If the editor was not up at session start, use `Tools/Mcp.py` as CLAUDE.md says. If no runtime evidence can be gathered, the PR says "builds, tests green, unverified at runtime" in those words.

- [ ] **Step 4: Docs and PR**

Spec status line: `**Status:** implemented 2026-09-XX (PR #NN).` Handover outcome section. Commit `docs: M2 outcome`. Then:
```bash
git push -u origin feature/m2-ground-traffic
gh pr create --title "M2: ground traffic occupancy, arbitration, hold-short, deadlock" --body-file <filled template>
```
The PR body follows `.github/PULL_REQUEST_TEMPLATE.md`: build line, test line (quoted), `UE_LOG` delta (71 → N), comment-line delta for the refactored pair, the seams and the test that fails if each is unwired (`TrafficForwarders` for the registry/relay, `GraphRebuild` for the rebuild hook, `HoldShortSurvivesRebuild` for the builder re-apply, `HoldShort` tool test for the registry entry), and the runtime evidence from Step 3.

---

## Self-review

**Spec coverage.** §2.1 table → Task 2. §2.2 `UGroundTraffic` → Tasks 4–9. §2.3 rules → Task 4. §2.4 agent fields → Task 4 (all listed, including `BlockedStep`, `RunwayHeld`, `DepartureRunway`). §2.5 follower → Task 1. §2.6 step distances → Task 1. §2.7 network additions → Tasks 3 and 10. §3 window/tick/box rule → Task 5; runway surface and hold-short → Task 6. §4 routing cost and replan → Task 7. §5 deadlock → Task 8. §6 rebuild → Task 9 (traffic) and Task 10 (marks). §7 tool → Task 11. §8 registry → Task 4. §9 tests: every named test has a task (`Occupancy.Claims` T2; `NodeYield`, `PriorityOverride`, `CarFollowing`, `HeadOn` T5/T8; `HoldShort`, `ArrivalRefusedRunwayOccupied` T6; `DeadlockTriangle` T8; `GraphRebuild` T9; `RouteSearch.OccupancyCost` T7; `Present.TrafficForwarders` T4; `Tool.HoldShort` T11; plus `HoldShortSurvivesRebuild` T10, `EndDistance`/`Splice`/`StopWithin` T1, `RunwayChain`/`RunwayOccupied` T3, `Replan` T7). §10 logs: stop/resume T5, hold-short T6, cycle detected/resolved/unresolvable T8, rebuild T9, arrival refused T3/T6, replanned T7. §11 out of scope: nothing in the plan touches reversing, pushback, time windows, the tool warning, turn bans, `MinSegmentLength`, a bar Blueprint.

**Type consistency.** `FRouteStep::EndDistance/EndVertex` (T1) used by T5/T7/T9. `RouteSearch::Splice(Head, KeepSteps, Tail)` (T1) used by T7/T9. `FRouteFollower::Advance(Delta, StopWithin, Pos, Heading)` and `Replace` (T1) used by T4/T7. `FTrafficOccupancy::TryClaim(Claim, OutBlocker)`, `ReleaseExcept`, `Release` (T2, T6), `HeldLengthOn`, `IsHeld`, `TakePreempted` (T2) used by T3/T5/T6/T7. `URoadNetwork::IsRunwaySegment`, `RunwayChain`, `OutSegment` (T3) used by T4/T6/T10. `UGroundTraffic::DispatchAgent(Network*, Plan, Airframe, Class, Pause)` returns id (T4) used by every test; `FindAgent`, `GetAgents`, `GetOccupancy`, `Rules` (T4); `ReplanAt(Id, Network, SpliceStep, Banned)` (T7) used by T8/T9; `GetCyclesDetectedForTest`, `GetLastResolvedAgentForTest` (T8); `OnGraphRebuilt` (T9). `IRoadEditTarget::DispatchAgent(Plan, Airframe, Class)` + two-arg forwarder (T4); `SetHoldShort(NodeIndex, SegmentIndex)` (T10) used by T11. `FHoldShortMark{At, Protects}`, `URoadNetwork::SetHoldShort(NodeId, SegmentId)`, `RunwayNearGuidelineNode` (T10) used by T11. `EPreviewStyle::HoldShort` (T11).

**Placeholders.** None: every step names files, code, the run line and the expected result. The one deliberate delegation is Task 4 Step 5 ("move verbatim") for bodies that already exist in `AirsideTraffic.cpp`, with the differences spelled out.

**Test counts** assume one test per `IMPLEMENT_SIMPLE_AUTOMATION_TEST`: 88 + 3 (T1) + 1 (T2) + 2 (T3) + 1 (T4) + 4 (T5) + 2 (T6) + 2 (T7) + 1 (T8) + 1 (T9) + 1 (T10) + 1 (T11) = 107.
