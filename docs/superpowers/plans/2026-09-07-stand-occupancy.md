# Stand Occupancy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two arrivals get two stands; a third is refused as `NoFreeStand`; a stand deleted under an inbound aircraft retargets it to another free stand, or leaves it waiting (not stranded) until one appears; the stand tool warns before deleting a stand in use.

**Architecture:** A stand is held by the agent whose `GoalNode` is its pose node: a node claim in the existing `FTrafficOccupancy`, re-asserted every tick from agent state by `ClaimAhead` and asserted at dispatch between ticks. `ArrivalPlanner` gains `ChooseStand`, which skips held stands and is reused by the rebuild to retarget a dead goal. `FRoadAgent::bAwaitingStand` (intent data) plus a `bStandsMayHaveFreed` flag on the traffic model drive the re-offer. Facts read the claim holder; the stand tool's remove preview labels a held stand.

**Tech Stack:** UE 5.8.2 C++, UE automation tests, `Tools/Run-AirsideTests.ps1`, `Tools/Check-Architecture.ps1`.

**Spec:** `docs/superpowers/specs/2026-09-07-stand-occupancy-design.md`

## Global Constraints

- Layering: `Model/` never includes `Build/|Tool/|Present/|Entities/`; `Tool/` never includes `Present/`.
- Unity build: anonymous-namespace helpers in tests get a unique prefix (`StandOcc`).
- Full build with the editor CLOSED (or in a worktree with `-NoHotReloadFromIDE`):
  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
  ```
- Tests: `./Tools/Run-AirsideTests.ps1 -Filter <Prefix>`; read `N test(s) run, N failed, N crashed`.
- `UE_LOG` count before/after recorded for the PR; no refactor here, so it may only grow.
- Branch `feature/stand-occupancy`, stacked on `feature/entity-inspector` (PR #61). PR base is that branch until #61 merges, then `main`.
- **Spec amendments this plan makes** (record in the spec's own sections during Task 3):
  - §3: a PARKED aircraft occupies whichever node it parked at, stand pose node or not. The M2 rule was "parked agents claim only the surface their body is on", which left a parked aircraft on a taxiway junction holding nothing. The spec's §5 "holds whatever node it stops at" needs this to be true.
  - §5: a stranded taxi-in (nothing of the route survives) sets `GoalNode` to the exit node it will stop at, so the re-offer has a live node to search from.

---

## File map

| File | Responsibility |
|---|---|
| `Model/GroundTraffic.h` + `Private/Model/GroundTraffic.cpp` | `ClaimGoalNode`, dispatch-time claim, redirect release, `bStandsMayHaveFreed`, re-offer pass in `Advance` |
| `Private/Model/GroundTrafficClaims.cpp` | `ClaimAhead` calls `ClaimGoalNode` after both branches |
| `Private/Model/GroundTrafficRebuild.cpp` | retarget hook in `ReResolvePlan`, awaiting flag, strand sets `GoalNode`, rebuild sets the freed flag |
| `Model/ArrivalPlanner.h/.cpp` | `EArrivalRefusal::NoFreeStand`, `ChooseStand`, planner skips held stands |
| `Model/RoadAgent.h` | `bAwaitingStand` |
| `Model/InspectFacts.h/.cpp` | status `No stand - waiting`; occupant from claim holder, `bOccupantParked` |
| `Source/AirportMgr/InspectorWidget.cpp` | `Reserved for` / `Occupied by` |
| `Tool/StandPlaceTool.cpp` | remove preview label `in use by aircraft N` |
| Tests | `StandClaimTest.cpp`, `StandChoiceTest.cpp`, `StandRetargetTest.cpp` (AirsideTests); edits to `InspectFactsTest.cpp`, `StandPlaceToolTest.cpp` |

Shared test fixture, declared ONCE in `StandClaimTest.cpp`'s anonymous namespace and REPEATED verbatim (renamed `StandOcc2*`, `StandOcc3*`) in the other two files - the tests module is a unity build and cannot share an anonymous-namespace helper:

```cpp
	/** M2TrafficArrivalAirport with TWO stands beside the taxiway, both facing east so their
	 *  lead-ins cast west and meet it. Prefixed StandOcc against the unity build. */
	struct FStandOccAirport
	{
		URoadNetwork* Net = nullptr;
		FVector2D Threshold = FVector2D::ZeroVector;
		FVector2D ExitAt = FVector2D::ZeroVector;
		FVector2D StandAAt, StandBAt;
		FEntityInstanceId StandA, StandB;
	};

	FAirframe StandOccPiper()
	{
		FAirframe A;
		A.Ground = UAircraftType::PiperMeridianGround();
		A.Climb = UAircraftType::PiperMeridianClimb();
		A.Approach = UAircraftType::PiperMeridianApproach();
		A.Engine = UAircraftType::PiperMeridianEngine();
		return A;
	}

	/** Solve, derive guidelines and re-link every stand: what the facade's RebuildMesh does. */
	void StandOccRebuild(URoadNetwork& Net)
	{
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(Net);
		FRoadGuidelineBuilder::Build(Net, Solved);
		FAnchorLink::Build(Net);
	}

	FStandOccAirport StandOccBuild()
	{
		FStandOccAirport Out;
		Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FAirframe Airframe = StandOccPiper();
		const double Needed = FLandingRun::RequiredLandingDistance(
			Airframe.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;
		Out.ExitAt = FVector2D(Needed * 1.2, 0.0);
		const FVector2D FarAt(Needed * 3.0, 0.0);

		URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

		const FRoadNodeId T = Out.Net->AddNode(Out.Threshold);
		const FRoadNodeId X = Out.Net->AddNode(Out.ExitAt);
		const FRoadNodeId F = Out.Net->AddNode(FarAt);
		Out.Net->AddStraightSegment(T, X, Runway);
		Out.Net->AddStraightSegment(X, F, Runway);
		const FRoadNodeId TaxiEnd = Out.Net->AddNode(Out.ExitAt + FVector2D(0.0, -20000.0));
		Out.Net->AddStraightSegment(X, TaxiEnd, Taxiway);

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Out.Net);
		FRoadGuidelineBuilder::Build(*Out.Net, Solved);

		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Out.StandAAt = Out.ExitAt + FVector2D(9000.0, -10000.0);
		Out.StandBAt = Out.ExitAt + FVector2D(9000.0, -16000.0);
		Out.StandA = Out.Net->PlaceEntity(Stand, Stand->Anchors, Out.StandAAt, 0.0);
		Out.StandB = Out.Net->PlaceEntity(Stand, Stand->Anchors, Out.StandBAt, 0.0);
		FAnchorLink::Build(*Out.Net);
		return Out;
	}

	FGuidelineNodeId StandOccPose(const FStandOccAirport& A, FEntityInstanceId Stand)
	{
		const FEntityInstance* E = A.Net->GetEntity(Stand);
		return E != nullptr ? E->PoseNode : FGuidelineNodeId();
	}

	/** Ticks until Pred() or Seconds; returns true when Pred became true. */
	template <typename P>
	bool StandOccRunUntil(UGroundTraffic& Traffic, const URoadNetwork& Net, double Seconds, P Pred, double Dt = 0.05)
	{
		for (double Clock = 0.0; Clock < Seconds; Clock += Dt)
		{
			Traffic.Advance(Dt, &Net);
			if (Pred()) { return true; }
		}
		return Pred();
	}
```
Includes each fixture file needs: `Build/AnchorLink.h`, `Build/RoadGuidelineBuilder.h`, `Build/RoadNetworkSolver.h`, `Entities/AircraftType.h`, `Entities/EntityDefinition.h`, `Model/ArrivalPlanner.h`, `Model/GroundTraffic.h`, `Model/LandingRun.h`, `Model/RoadGuideline.h`, `Model/RoadNetwork.h`, `Model/RouteSearch.h`, `Model/TrafficOccupancy.h`, `Profiles/RoadProfile.h`, `Misc/AutomationTest.h`.

---

### Task 1: The claim

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/GroundTraffic.h`, `Private/Model/GroundTraffic.cpp` (DispatchArrival, DispatchAgent, RedirectAgent, RetireAgent, Advance removal), `Private/Model/GroundTrafficClaims.cpp` (ClaimAhead)
- Create: `Plugins/Airside/Source/AirsideTests/Private/StandClaimTest.cpp`

**Interfaces:**
- Produces (private on `UGroundTraffic`): `void ClaimGoalNode(FRoadAgent& Agent, const URoadNetwork& Network);` and member `bool bStandsMayHaveFreed = false;`; public `bool StandsMayHaveFreedForTest() const`.
- Consumes: `FTrafficOccupancy::TryClaim/Release/IsHeld`, `URoadNetwork::FindEntityIndexByPoseNode` (#61), `TraversalPriority`.

- [ ] **Step 1: Failing test**

`StandClaimTest.cpp` (fixture from the file map, prefixed `StandOcc`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandClaimTest,
	"Airside.Model.Traffic.StandClaim",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandClaimTest::RunTest(const FString& Parameters)
{
	// THE CLAIM IS A READING OF THE GOAL. Held from dispatch (between ticks), re-asserted
	// every tick, released when the goal changes or the agent goes. Nothing on the stand.
	FStandOccAirport A = StandOccBuild();
	const FGuidelineNodeId PoseA = StandOccPose(A, A.StandA);
	const FGuidelineNodeId PoseB = StandOccPose(A, A.StandB);
	if (!TestTrue(TEXT("both stands linked"), PoseA.IsSet() && PoseB.IsSet())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Traffic->DispatchArrival(*A.Net, A.Threshold, StandOccPiper(), 1.0);
	if (!TestTrue(TEXT("arrival dispatched"), Id > 0)) { return false; }
	const FGuidelineNodeId Goal = Traffic->FindAgent(Id)->GoalNode;
	TestTrue(TEXT("the goal is one of the two stands"), Goal == PoseA || Goal == PoseB);

	int32 Holder = 0;
	TestTrue(TEXT("the stand is held BEFORE any tick - dispatch claims it"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Goal), 0, &Holder));
	TestEqual(TEXT("by this agent"), Holder, Id);

	// Still held every tick of the approach, roll and taxi; occupied once parked.
	bool bHeldThroughout = true;
	const bool bParked = StandOccRunUntil(*Traffic, *A.Net, 600.0, [&]()
	{
		bHeldThroughout = bHeldThroughout && Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Goal), 0);
		const FRoadAgent* P = Traffic->FindAgent(Id);
		return P == nullptr || P->Phase == EAgentPhase::Parked;
	});
	if (!TestTrue(TEXT("parked"), bParked && Traffic->FindAgent(Id) != nullptr)) { return false; }
	TestTrue(TEXT("held on every tick from dispatch to parking"), bHeldThroughout);
	{
		const FTrafficClaim* Mine = Traffic->GetOccupancy().GetClaims().FindByPredicate(
			[&](const FTrafficClaim& C) { return C.AgentId == Id && C.Resource == FTrafficResource::OfNode(Goal); });
		TestTrue(TEXT("a parked aircraft OCCUPIES its stand node"), Mine != nullptr && Mine->bOccupied);
	}

	// Redirect elsewhere: the stand frees between ticks, and the model says stands may have freed.
	const FGuidelineNodeId Other = (Goal == PoseA) ? PoseB : PoseA;
	FRouteQuery Q; Q.Start = Goal; Q.Goal = Other; Q.Class = ETraversalClass::Aircraft;
	const FRoutePlan ToOther = RouteSearch::Find(*A.Net, Q);
	if (!TestTrue(TEXT("a route between the stands exists"), ToOther.IsValid())) { return false; }
	if (!TestTrue(TEXT("redirected"), Traffic->RedirectAgent(Id, A.Net, ToOther))) { return false; }
	TestFalse(TEXT("the old stand is released at the redirect, not a tick later"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Goal), 0));
	TestTrue(TEXT("the new stand is held at the redirect"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Other), 0));
	TestTrue(TEXT("the model flags that a stand may have freed"), Traffic->StandsMayHaveFreedForTest());

	// Retire: everything goes.
	Traffic->RetireAgent(Id);
	TestFalse(TEXT("retired agents hold nothing"), Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Other), 0));
	return true;
}
```

- [ ] **Step 2: Header**

`Model/GroundTraffic.h`, public, after `GetOccupancy()`:
```cpp
	/** True after a stand claim was released or a rebuild ran, until Advance's re-offer pass
	 *  consumes it. For Airside.Model.Traffic.StandClaim. */
	bool StandsMayHaveFreedForTest() const { return bStandsMayHaveFreed; }
```
Private, after `HoldRunwayOnly`:
```cpp
	/**
	 * The one claim that is not about the ground under or ahead of the agent: its DESTINATION.
	 * An Arriving or Taxiing aircraft RESERVES the stand pose node it is heading for, and a
	 * Parked agent OCCUPIES the node it parked at - stand or not; the M2 rule "a parked agent
	 * holds only its surface" left a parked aircraft on a taxiway junction holding nothing
	 * (spec 2026-09-07-stand-occupancy §3, amended).
	 *
	 * DERIVED FROM GoalNode EVERY TICK, after the phase's own claim pass has run its
	 * ReleaseExcept. Threading the stand through BuildPending/ApplyClaims was rejected: those
	 * are route-ordered claims whose first refusal sets StopWithin, and a stand must never
	 * stop an aircraft short - it is a reservation for a place, not a queue for a line. The
	 * drop-and-reclaim inside one agent's pass is invisible: Arbitrate is synchronous and no
	 * other agent has the same goal (the planner and the rebuild see to that).
	 */
	void ClaimGoalNode(FRoadAgent& Agent, const URoadNetwork& Network);

	/** Claims Agent's GoalNode as a stand reservation right now, for the between-ticks
	 *  window DispatchArrival reads the table in. Used by both dispatches. */
	void ClaimGoalNodeAtDispatch(const FRoadAgent& Agent, int32 Id, const URoadNetwork& Network);

	/**
	 * Set when a stand claim is released (redirect, retire, Gone) or the graph is rebuilt;
	 * consumed by Advance's re-offer pass (Task 3). A FLAG rather than an event: the table is
	 * rebuilt per tick and the model is world-free, so one bool checked per frame is the
	 * cheapest correct thing.
	 */
	bool bStandsMayHaveFreed = false;
```

- [ ] **Step 3: `ClaimGoalNode` and `ClaimAhead`**

`GroundTrafficClaims.cpp`: at the end of `ClaimAhead`, restructure so both branches fall through to the goal claim:
```cpp
void UGroundTraffic::ClaimAhead(FRoadAgent& Agent, const URoadNetwork& Network)
{
	if (Agent.Phase != EAgentPhase::Taxiing)
	{
		HoldRunwayOnly(Agent, Network);
		ClaimGoalNode(Agent, Network);
		return;
	}

	const FRoutePlan& Plan = Agent.Follower.Plan;
	if (!Plan.IsValid() || Plan.Steps.Num() == 0)
	{
		ReleaseForDeadPlan(Agent);
		ClaimGoalNode(Agent, Network);
		return;
	}
	... existing body unchanged ...
	ApplyClaims(Agent, Window, Pending);
	ClaimGoalNode(Agent, Network);
}

void UGroundTraffic::ClaimGoalNode(FRoadAgent& Agent, const URoadNetwork& Network)
{
	if (!Agent.GoalNode.IsSet() || Network.GetGuidelineNode(Agent.GoalNode) == nullptr)
	{
		return;
	}
	const bool bParked = Agent.Phase == EAgentPhase::Parked;
	const bool bStandGoal = Network.FindEntityIndexByPoseNode(Agent.GoalNode) != INDEX_NONE;
	// A reservation names a STAND only: an aircraft heading for a runway or a plain node
	// reserves nothing ahead of itself beyond what the route pass already asks for. A parked
	// body occupies wherever it is.
	if (!bParked && (!bStandGoal || Agent.Class != ETraversalClass::Aircraft))
	{
		return;
	}
	FTrafficClaim Claim;
	Claim.AgentId = Agent.Id;
	Claim.Resource = FTrafficResource::OfNode(Agent.GoalNode);
	Claim.bOccupied = bParked;
	Claim.Rank = TraversalPriority(Agent.Class);
	FTrafficClaim Blocker;
	// Not acted on: a stand already held by someone else is a planning failure upstream (the
	// planner and the rebuild both skip held stands), and a table cannot un-plan an aircraft.
	// Logged on the transition only by the overlap machinery, as any other double claim.
	Occupancy.TryClaim(Claim, Blocker);
}
```
Add `#include "Model/RoadNetwork.h"` if not already present in that .cpp.

- [ ] **Step 4: Dispatch, redirect, retire, Gone**

`GroundTraffic.cpp`:

```cpp
void UGroundTraffic::ClaimGoalNodeAtDispatch(const FRoadAgent& Agent, int32 Id, const URoadNetwork& Network)
{
	// THE SAME RULE AS THE RUNWAY CHAIN TWENTY LINES UP: raised here, not on the next claim
	// pass, because ArrivalPlanner reads the table between ticks and M3's sequencer will
	// dispatch two arrivals in one frame. Class and goal are read off the agent as admitted.
	if (Agent.Class != ETraversalClass::Aircraft || !Agent.GoalNode.IsSet()
		|| Network.FindEntityIndexByPoseNode(Agent.GoalNode) == INDEX_NONE)
	{
		return;
	}
	FTrafficClaim Claim;
	Claim.AgentId = Id;
	Claim.Resource = FTrafficResource::OfNode(Agent.GoalNode);
	Claim.bOccupied = false;
	Claim.Rank = TraversalPriority(ETraversalClass::Aircraft);
	FTrafficClaim Blocker;
	Occupancy.TryClaim(Claim, Blocker);
}
```
In `DispatchArrival`, after the runway-chain claim loop: `ClaimGoalNodeAtDispatch(Agents.Last(), Id, Network);` (the agent was just moved into the array by `Admit`; `Agents.Last()` is it - add a one-line comment saying so). In `DispatchAgent`, after `Admit`: `if (Network != nullptr) { ClaimGoalNodeAtDispatch(Agents.Last(), Id, *Network); }` - read the function to find where `Admit` is called and place it after.

In `RedirectAgent`, before `Agent.StartTaxi(Plan, Own);`:
```cpp
	// THE OLD STAND FREES NOW, between ticks: the next claim pass would drop it anyway
	// (ClaimGoalNode reads the new goal), but a planner asking in this frame must see it
	// free. And the model notes that a stand may have freed, for the re-offer pass.
	if (Agent.GoalNode.IsSet())
	{
		Occupancy.Release(AgentId, FTrafficResource::OfNode(Agent.GoalNode));
		bStandsMayHaveFreed = true;
	}
```
After `Agent.GoalNode = ...; ArmDepartureIfRunway(...)`: `if (Network != nullptr) { ClaimGoalNodeAtDispatch(Agent, AgentId, *Network); }`.

In `RetireAgent` next to `Occupancy.ReleaseAll(AgentId);` and in `Advance`'s removal branch next to `Occupancy.ReleaseAll(Id);`: `bStandsMayHaveFreed = true;`.

- [ ] **Step 5: Build, test, commit**

Build → `Result: Succeeded`. `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.Traffic` → all M2 traffic tests still green plus `StandClaim`; `0 failed, 0 crashed`. If an M2 test that parks an agent at a node and expects another agent to pass through it fails, that is the spec §3 amendment biting: read the test, and if it asserted pass-through of a parked body, update its expectation and say so in the commit.
```
git add Plugins && git commit -m "feat(traffic): a stand is held by the agent whose goal it is

Node claim in FTrafficOccupancy, re-asserted per tick from GoalNode (reservation inbound,
occupied when parked - and a parked body now occupies any node, spec §3 amended), asserted
at dispatch and released at redirect for the between-ticks window the planner reads in.
Airside.Model.Traffic.StandClaim."
```

---

### Task 2: The planner skips held stands

**Files:**
- Modify: `Model/ArrivalPlanner.h`, `Private/Model/ArrivalPlanner.cpp`
- Create: `AirsideTests/Private/StandChoiceTest.cpp`

**Interfaces:**
- Produces:
  ```cpp
  enum class EArrivalRefusal : uint8 { ..., NotAdmitted, NoFreeStand };
  namespace ArrivalPlanner {
    /** Best free stand reachable from From (shortest taxi, runway edges excluded). Unset when none.
     *  bOutSawHeld reports that at least one reachable stand was skipped for being held. */
    AIRSIDE_API FGuidelineNodeId ChooseStand(const URoadNetwork&, FGuidelineNodeId From, const FAirframe&,
        const FTrafficOccupancy* Occupancy, int32 ExcludingAgent, FRoutePlan* OutRoute, bool* bOutSawHeld);
  }
  ```

- [ ] **Step 1: Failing test**

`StandChoiceTest.cpp` (fixture repeated with prefix `StandOcc2`):
```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceTest,
	"Airside.Model.ArrivalPlanner.SkipsHeldStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceTest::RunTest(const FString& Parameters)
{
	FStandOcc2Airport A = StandOcc2Build();
	const FGuidelineNodeId PoseA = StandOcc2Pose(A, A.StandA);
	const FGuidelineNodeId PoseB = StandOcc2Pose(A, A.StandB);
	if (!TestTrue(TEXT("both stands linked"), PoseA.IsSet() && PoseB.IsSet())) { return false; }
	const FAirframe Piper = StandOcc2Piper();

	// Planner level: with A held by agent 7, the plan goes to B; with both held, NoFreeStand.
	FTrafficOccupancy Occ;
	const FArrivalPlan Free = ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ);
	if (!TestTrue(TEXT("plans with both free"), Free.IsValid())) { return false; }
	const FGuidelineNodeId First = Free.TaxiIn.Steps.Last().To;

	auto Hold = [&](FGuidelineNodeId Node, int32 Agent)
	{
		FTrafficClaim C; C.AgentId = Agent; C.Resource = FTrafficResource::OfNode(Node); C.Rank = 10;
		FTrafficClaim B; Occ.TryClaim(C, B);
	};
	Hold(First, 7);
	const FArrivalPlan Other = ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ);
	if (!TestTrue(TEXT("plans with one held"), Other.IsValid())) { return false; }
	TestTrue(TEXT("and goes to the OTHER stand"), Other.TaxiIn.Steps.Last().To != First);
	Hold(Other.TaxiIn.Steps.Last().To, 8);
	const FArrivalPlan None = ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ);
	TestEqual(TEXT("both held is NoFreeStand"), None.Why, EArrivalRefusal::NoFreeStand);
	TestTrue(TEXT("and it has words"), !ArrivalPlanner::DescribeRefusal(None).IsEmpty());

	// Unreachable is still NoRouteToStand: remove both stands' lead-ins by removing the stands.
	A.Net->RemoveEntity(A.StandA);
	A.Net->RemoveEntity(A.StandB);
	StandOcc2Rebuild(*A.Net);
	TestEqual(TEXT("no stands at all is NoRouteToStand, not NoFreeStand"),
		ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ).Why, EArrivalRefusal::NoRouteToStand);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceTwoArrivalsTest,
	"Airside.Model.Traffic.TwoArrivalsTwoStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceTwoArrivalsTest::RunTest(const FString& Parameters)
{
	// THE REPORT: "I called in 2 aircraft, they both went to the same stand." Through the
	// model, end to end: the second is dispatched once the first has vacated the runway (the
	// runway claim would refuse it earlier, for its own reason), and gets the other stand.
	// A third, once the second has vacated too, is refused for want of a stand.
	FStandOcc2Airport A = StandOcc2Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Piper = StandOcc2Piper();

	const int32 First = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	if (!TestTrue(TEXT("first dispatched"), First > 0)) { return false; }
	if (!TestTrue(TEXT("first vacates"), StandOcc2RunUntil(*Traffic, *A.Net, 300.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(First); return P && P->Phase == EAgentPhase::Taxiing; }))) { return false; }

	TArray<EArrivalRefusal> Refusals;
	Traffic->OnArrivalRefused.AddLambda([&](EArrivalRefusal Why) { Refusals.Add(Why); });
	const int32 Second = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	if (!TestTrue(FString::Printf(TEXT("second dispatched (refusals: %d)"), Refusals.Num()), Second > 0)) { return false; }
	TestTrue(TEXT("two aircraft, two stands"),
		Traffic->FindAgent(First)->GoalNode != Traffic->FindAgent(Second)->GoalNode);

	if (!TestTrue(TEXT("second vacates"), StandOcc2RunUntil(*Traffic, *A.Net, 300.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Second); return P && P->Phase == EAgentPhase::Taxiing; }))) { return false; }
	const int32 Third = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	TestEqual(TEXT("a third is refused"), Third, 0);
	TestTrue(TEXT("for want of a free stand"), Refusals.Num() > 0 && Refusals.Last() == EArrivalRefusal::NoFreeStand);
	return true;
}
```

- [ ] **Step 2: Header**

`ArrivalPlanner.h`: add to the enum after `NotAdmitted`:
```cpp
	/** Stands are reachable, but every one of them is held by another aircraft. Distinct from
	 *  NoRouteToStand because the player's fix differs: wait (or build a stand), not a taxiway. */
	NoFreeStand,
```
Add to the namespace, before `Plan`:
```cpp
	/**
	 * The best FREE stand reachable from From: shortest taxi with runway edges excluded, skipping
	 * any stand whose pose node Occupancy says is held by an agent other than ExcludingAgent.
	 * Unset when none. OutRoute receives the winning route; bOutSawHeld reports that at least one
	 * reachable stand was skipped for being held, which is how Plan tells NoFreeStand from
	 * NoRouteToStand. Factored out of Plan so the rebuild can ask it from a node that is not a
	 * runway exit (UGroundTraffic::ReResolvePlan).
	 */
	AIRSIDE_API FGuidelineNodeId ChooseStand(const URoadNetwork& Network, FGuidelineNodeId From,
		const FAirframe& Airframe, const FTrafficOccupancy* Occupancy, int32 ExcludingAgent,
		FRoutePlan* OutRoute = nullptr, bool* bOutSawHeld = nullptr);
```

- [ ] **Step 3: Implementation**

`ArrivalPlanner.cpp`: add `ChooseStand` before `Plan`:
```cpp
	FGuidelineNodeId ChooseStand(const URoadNetwork& Network, FGuidelineNodeId From,
		const FAirframe& Airframe, const FTrafficOccupancy* Occupancy, int32 ExcludingAgent,
		FRoutePlan* OutRoute, bool* bOutSawHeld)
	{
		FGuidelineNodeId Best;
		FRoutePlan BestRoute;
		double BestLength = TNumericLimits<double>::Max();
		bool bSawHeld = false;
		for (const FEntityInstance& Stand : Network.GetEntities())
		{
			if (!Stand.bAlive || !Stand.PoseNode.IsSet())
			{
				continue;
			}
			FRouteQuery Query;
			Query.Start = From;
			Query.Goal = Stand.PoseNode;
			Query.Class = ETraversalClass::Aircraft;
			Query.Wingspan = Airframe.Wingspan;
			Query.bAvoidRunways = true;
			const FRoutePlan Route = RouteSearch::Find(Network, Query);
			if (!Route.IsValid() || Route.Polyline.Num() < 2 || Route.Steps.Num() == 0)
			{
				continue;
			}
			// Held is asked AFTER reachability, so bSawHeld means "a stand this aircraft could
			// have used" - the only reading under which NoFreeStand is the right word.
			if (Occupancy != nullptr && Occupancy->IsHeld(FTrafficResource::OfNode(Stand.PoseNode), ExcludingAgent))
			{
				bSawHeld = true;
				continue;
			}
			if (Route.Length < BestLength)
			{
				BestLength = Route.Length;
				BestRoute = Route;
				Best = Stand.PoseNode;
			}
		}
		if (OutRoute != nullptr) { *OutRoute = BestRoute; }
		if (bOutSawHeld != nullptr) { *bOutSawHeld = bSawHeld; }
		return Best;
	}
```
In `Plan`, replace the inner `for (const FEntityInstance& Stand : ...)` loop body (from `double BestLength = ...` through the closing of that loop) with:
```cpp
			FRoutePlan BestForExit;
			bool bHeldHere = false;
			ChooseStand(Network, Candidate, Airframe, Occupancy, 0, &BestForExit, &bHeldHere);
			bSawHeldStand = bSawHeldStand || bHeldHere;
```
declaring `bool bSawHeldStand = false;` beside `FGuidelineNodeId FirstForward, FirstBacktrack;`. Then change the refusal:
```cpp
		if (!Out.TaxiIn.IsValid())
		{
			Out.Why = bSawHeldStand ? EArrivalRefusal::NoFreeStand : EArrivalRefusal::NoRouteToStand;
			return Out;
		}
```
`DescribeRefusal`: add
```cpp
		case EArrivalRefusal::NoFreeStand:
			return TEXT("Arrival refused: every stand it could reach is taken. Wait for one to free, or build another.");
```

- [ ] **Step 4: Build, test, commit**

Build. `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.ArrivalPlanner+Airside.Model.Traffic.TwoArrivals` → the existing planner tests plus the two new ones, `0 failed, 0 crashed`.
```
git add Plugins && git commit -m "feat(model): the arrival planner skips held stands; NoFreeStand names the refusal

ChooseStand factored out of Plan (the rebuild reuses it); held is asked after reachability
so NoFreeStand means a stand this aircraft could have used. Two arrivals now get two stands."
```

---

### Task 3: Retarget on goal death, wait not strand, re-offer

**Files:**
- Modify: `Model/RoadAgent.h` (`bAwaitingStand`), `Private/Model/GroundTrafficRebuild.cpp`, `Private/Model/GroundTraffic.cpp` (Advance re-offer), `Model/GroundTraffic.h` (`ReofferStands` private, `bStandsMayHaveFreed` set in `OnGraphRebuilt`), `Model/InspectFacts.cpp` (status), spec §3/§5 amendments
- Create: `AirsideTests/Private/StandRetargetTest.cpp`

**Interfaces:**
- Produces: `UPROPERTY() bool FRoadAgent::bAwaitingStand = false;`, private `void UGroundTraffic::ReofferStands(const URoadNetwork& Network);`.

- [ ] **Step 1: Failing test**

`StandRetargetTest.cpp` (fixture prefixed `StandOcc3`; note `StandOcc3Rebuild` and a Traffic pointer):
```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandRetargetTest,
	"Airside.Model.Traffic.StandRetarget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandRetargetTest::RunTest(const FString& Parameters)
{
	FStandOcc3Airport A = StandOcc3Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Piper = StandOcc3Piper();

	const int32 Id = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }
	Traffic->Advance(0.05, A.Net);   // one tick: on final
	const FGuidelineNodeId Goal0 = Traffic->FindAgent(Id)->GoalNode;
	const FEntityInstanceId Target = (Goal0 == StandOcc3Pose(A, A.StandA)) ? A.StandA : A.StandB;
	const FEntityInstanceId Spare = (Target == A.StandA) ? A.StandB : A.StandA;

	// 1. DELETE THE STAND IT IS HEADING FOR while it is on final. It retargets to the other.
	A.Net->RemoveEntity(Target);
	StandOcc3Rebuild(*A.Net);
	Traffic->OnGraphRebuilt(*A.Net);
	{
		const FRoadAgent* P = Traffic->FindAgent(Id);
		TestEqual(TEXT("still arriving"), P->Phase, EAgentPhase::Arriving);
		TestTrue(TEXT("its goal is now the spare stand"), P->GoalNode == StandOcc3Pose(A, Spare));
		TestFalse(TEXT("and it is not waiting"), P->bAwaitingStand);
		TestTrue(TEXT("the spare stand is held for it"), Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(P->GoalNode), 0));
	}

	// 2. DELETE THE SPARE TOO. Nothing to retarget to: it waits, and lands anyway (v1).
	A.Net->RemoveEntity(Spare);
	StandOcc3Rebuild(*A.Net);
	Traffic->OnGraphRebuilt(*A.Net);
	{
		const FRoadAgent* P = Traffic->FindAgent(Id);
		TestTrue(TEXT("awaiting a stand"), P->bAwaitingStand);
		TestTrue(TEXT("with a live node to wait at"), A.Net->GetGuidelineNode(P->GoalNode) != nullptr);
		TestEqual(TEXT("status says so"), InspectFacts::StatusOf(*P), FString(TEXT("No stand - waiting")));
	}
	if (!TestTrue(TEXT("it lands and stops"), StandOcc3RunUntil(*Traffic, *A.Net, 600.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P && P->Phase == EAgentPhase::Parked; }))) { return false; }
	TestTrue(TEXT("a parked waiter occupies the node it stopped at"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Traffic->FindAgent(Id)->GoalNode), 0));

	// 3. BUILD A STAND. The rebuild re-offers it; the aircraft goes.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId NewStand = A.Net->PlaceEntity(Stand, Stand->Anchors, A.StandAAt, 0.0);
	StandOcc3Rebuild(*A.Net);
	Traffic->OnGraphRebuilt(*A.Net);
	Traffic->Advance(0.05, A.Net);   // the re-offer runs at the end of a tick
	{
		const FRoadAgent* P = Traffic->FindAgent(Id);
		TestFalse(TEXT("no longer waiting"), P->bAwaitingStand);
		TestTrue(TEXT("heading for the new stand"), P->GoalNode == StandOcc3Pose(A, NewStand));
		TestEqual(TEXT("taxiing again"), P->Phase, EAgentPhase::Taxiing);
	}
	TestTrue(TEXT("and it parks there"), StandOcc3RunUntil(*Traffic, *A.Net, 600.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P && P->Phase == EAgentPhase::Parked && P->GoalNode == StandOcc3Pose(A, NewStand); }));
	return true;
}
```
Add `#include "Model/InspectFacts.h"` to that file.

- [ ] **Step 2: `bAwaitingStand`**

`Model/RoadAgent.h`, after `bDepartureArmed`:
```cpp
	/**
	 * No stand could be found for this aircraft: it stops at the end of what remains of its
	 * route and is re-offered one whenever a stand may have freed. INTENT DATA, like
	 * FDepartureOrder, not a phase - a waiting aircraft is Taxiing to its prefix's end and
	 * then Parked there, and either is true while it waits. Set only by the rebuild path
	 * (UGroundTraffic::ReResolvePlan) in v1; cleared by the re-offer.
	 */
	UPROPERTY() bool bAwaitingStand = false;
```

- [ ] **Step 3: Retarget hook and strand goal**

`GroundTrafficRebuild.cpp`, in `ReResolvePlan`, immediately after the `Goal` lookup block (`if (Goal.IsSet()) { Agent.GoalNode = Goal; }`) and before `if (Failed == Plan.Steps.Num()) return Intact;`:
```cpp
	// THE GOAL WAS A STAND AND THE STAND IS GONE - or a taxiway to it. An aircraft whose goal
	// no longer resolves, and that is not lined up for a runway, is retargeted at whichever
	// FREE stand is nearest the node it will replan from, BEFORE the replan below runs - so
	// the replan searches to a live stand rather than to a freed handle and truncates. No
	// stand: it is marked awaiting, and the truncation that follows gives it a node to wait
	// at. Spec 2026-09-07-stand-occupancy §5. Vehicles and departures keep M2's rules.
	if (!Goal.IsSet() && Agent.Class == ETraversalClass::Aircraft && !Agent.bDepartureArmed
		&& Failed < Plan.Steps.Num())
	{
		const FGuidelineNodeId ReplanFrom = StepFromNode(Plan, Failed);
		const FGuidelineNodeId NewStand = ArrivalPlanner::ChooseStand(
			Network, ReplanFrom, Agent.Airframe, &Occupancy, Agent.Id);
		if (NewStand.IsSet())
		{
			Agent.GoalNode = NewStand;
			Agent.bAwaitingStand = false;
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d: its stand is gone; retargeting to stand at node %d"),
				Agent.Id, NewStand.Index);
		}
		else
		{
			Agent.bAwaitingStand = true;
			UE_LOG(LogAirsideTraffic, Warning, TEXT("Agent %d: its stand is gone and no free stand is reachable; it will wait"),
				Agent.Id);
		}
	}
```
Add `#include "Model/ArrivalPlanner.h"` at the top. Check `StepFromNode(Plan, Failed)` is the existing helper (it is used in the taxi-in replan branch); if its signature differs, use the same call the replan uses.

In the `Strand` lambda, before `return EReResolve::Stranded;`, give a waiting taxi-in a live node:
```cpp
		// A STRANDED TAXI-IN STILL HAS A PLACE: the exit node the landing hands over at,
		// which Plan.Start was re-pointed to above when it could be. The re-offer searches
		// from GoalNode, so a dead handle here would leave the aircraft waiting for ever.
		if (!bDriving && Agent.bAwaitingStand && Plan.Start.IsSet())
		{
			Agent.GoalNode = Plan.Start;
		}
```
(`Plan.Start` is re-pointed to `Prev` only when the strand happens after that line; the "malformed"/"no live node" strands leave it as it was. Acceptable: the Warning already names the case.)

`OnGraphRebuilt`, after `Occupancy.ReleaseGuidelineClaims();`: `bStandsMayHaveFreed = true;` with the comment `// A rebuild may have ADDED a stand: the re-offer pass asks for every waiter.`

- [ ] **Step 4: Re-offer pass**

`GroundTraffic.h` private: `void ReofferStands(const URoadNetwork& Network);`. `GroundTraffic.cpp`, at the end of `Advance` (after the deadlock pass, before the closing brace):
```cpp
	if (bStandsMayHaveFreed && Network != nullptr)
	{
		ReofferStands(*Network);
	}
```
and:
```cpp
void UGroundTraffic::ReofferStands(const URoadNetwork& Network)
{
	// ONE PASS, then the flag clears whether or not anyone was placed: a waiter that still
	// found nothing will be asked again the next time something frees, not every frame.
	bStandsMayHaveFreed = false;
	TArray<int32> Waiting;
	for (const FRoadAgent& Agent : Agents)
	{
		if (Agent.bAwaitingStand && Agent.GoalNode.IsSet()
			&& (Agent.Phase == EAgentPhase::Parked || Agent.Phase == EAgentPhase::Taxiing))
		{
			Waiting.Add(Agent.Id);
		}
	}
	// By id, not by reference: RedirectAgent writes into Agents and may broadcast.
	for (const int32 Id : Waiting)
	{
		const FRoadAgent* Agent = FindAgent(Id);
		if (Agent == nullptr) { continue; }
		FRoutePlan Route;
		const FGuidelineNodeId Stand = ArrivalPlanner::ChooseStand(Network, Agent->GoalNode, Agent->Airframe, &Occupancy, Id, &Route);
		if (!Stand.IsSet() || !Route.IsValid())
		{
			continue;
		}
		if (RedirectAgent(Id, &Network, Route))
		{
			Agents[FindIndex(Id)].bAwaitingStand = false;
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d: a stand freed; sent to node %d"), Id, Stand.Index);
		}
	}
}
```
Note `RedirectAgent` sets `bStandsMayHaveFreed = true` when it releases the old goal; that is harmless (one extra pass next tick finds no waiters). Add `#include "Model/ArrivalPlanner.h"` to `GroundTraffic.cpp`.

- [ ] **Step 5: Status line**

`InspectFacts.cpp` `StatusOf`, first check before `bDepartureArmed`:
```cpp
		if (Agent.bAwaitingStand)
		{
			return TEXT("No stand - waiting");
		}
```
Update the precedence sentence in `InspectFacts.h`'s `StatusOf` doc to start with `No stand - waiting;`.

- [ ] **Step 6: Spec amendments**

In the spec §3 add, after the rank bullet: `*Amended (Task 3):* a PARKED aircraft occupies whichever node it parked at, stand or not - the M2 "surface only" rule left a parked aircraft on a taxiway junction holding nothing, and §5's "holds whatever node it stops at" needs this to be true.` In §5 after "Awaiting a stand" paragraph: `*Amended (Task 3):* a stranded taxi-in sets GoalNode to the exit node it will stop at, so the re-offer has a live node to search from.`

- [ ] **Step 7: Build, test, commit**

Build. `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.Traffic+Airside.Model.InspectFacts` → `0 failed, 0 crashed`, `StandRetarget` listed. The M2 `GraphRebuild` tests must still pass: their vehicles and departure-armed aircraft are excluded from the hook by class and flag.
```
git add Plugins docs && git commit -m "feat(traffic): a dead stand goal retargets to a free stand, else the aircraft waits

ReResolvePlan asks ChooseStand before the same-goal replan; no stand sets bAwaitingStand and
the truncation/strand leaves a live node to wait at. Advance re-offers waiters when a stand
may have freed (redirect, retire, Gone, rebuild). Status 'No stand - waiting'. Spec §3/§5
amended: a parked body occupies its node; a stranded taxi-in keeps the exit as its goal."
```

---

### Task 4: Facts and panel wording

**Files:**
- Modify: `Model/InspectFacts.h/.cpp`, `Source/AirportMgr/InspectorWidget.cpp`, `AirsideTests/Private/InspectFactsTest.cpp`

**Interfaces:**
- Produces: `FStandFacts::bOccupantParked`; `OccupantAgent` read from the claim holder.

- [ ] **Step 1: Test edits**

In `InspectFactsTest.cpp`, after `TestEqual(TEXT("occupant is the inbound agent"), SF.OccupantAgent, Id);` add `TestFalse(TEXT("inbound, not parked"), SF.bOccupantParked);`. After the parked re-describe add `TestTrue(TEXT("parked occupant"), SF.bOccupantParked);`. These fail until Step 2.

- [ ] **Step 2: Facts from the claim**

`InspectFacts.h` `FStandFacts`: add `/** The occupant is Parked (else inbound: reserved). */ bool bOccupantParked = false;` and reword `OccupantAgent`'s comment: "The agent holding this stand's pose node in the traffic occupancy table, 0 when free. Inbound holders have reserved it; a parked one occupies it."

`InspectFacts.cpp` `DescribeStand`, replace the occupant loop with:
```cpp
		Out.OccupantAgent = 0;
		Out.bOccupantParked = false;
		if (Traffic != nullptr && E.PoseNode.IsSet())
		{
			int32 Holder = 0;
			if (Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(E.PoseNode), 0, &Holder))
			{
				Out.OccupantAgent = Holder;
				const FRoadAgent* Agent = Traffic->FindAgent(Holder);
				Out.bOccupantParked = Agent != nullptr && Agent->Phase == EAgentPhase::Parked;
			}
		}
```
Add `#include "Model/TrafficOccupancy.h"`.

`InspectorWidget.cpp`: `Status = S.OccupantAgent != 0 ? FString::Printf(S.bOccupantParked ? TEXT("Occupied by aircraft #%d") : TEXT("Reserved for aircraft #%d"), S.OccupantAgent) : FString(TEXT("Empty"));`

- [ ] **Step 3: Build, test, commit**

Build. `-Filter Airside.Model.InspectFacts+AirportMgr` → `0 failed, 0 crashed`.
```
git add Plugins Source && git commit -m "feat(ui): stand panel reads the claim - Reserved for / Occupied by"
```

---

### Task 5: Warn at the delete

**Files:**
- Modify: `Private/Tool/StandPlaceTool.cpp`, `AirsideTests/Private/StandPlaceToolTest.cpp`

- [ ] **Step 1: Test**

Read `StandPlaceToolTest.cpp` for its sink and fixture (it places a stand through an `ARoadNetworkActor`). Add a case at the end of its test body: dispatch a plain taxi to the stand's pose node through the actor (`Actor->DispatchAgent(RouteSearch::Find(...), UAirsideSettings::ResolveDefaultAirframe())` from an authored node joined to the pose node with `FGuidelineEdge` as in `InspectFactsTest`), then build a remove-modifier context over the stand and assert the preview sink saw a `Label` with style `Refused` whose text contains `in use by aircraft`. Then retire the agent and assert no such label. If the sink there does not record label text, extend it (a `TArray<FString> Labels` filled in `Label`).

- [ ] **Step 2: Implementation**

`StandPlaceTool.cpp`, in `BuildPreview`'s remove branch, after the Doomed label:
```cpp
				// IN USE. The claim holder, read from the traffic table through the edit target
				// (Model/, so a tool may see it). The click still deletes - the player owns the
				// infrastructure - but not without being told who is about to lose a stand.
				if (const UGroundTraffic* Traffic = Context.Target->GetGroundTraffic())
				{
					int32 Holder = 0;
					if (Entities[Under].PoseNode.IsSet()
						&& Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Entities[Under].PoseNode), 0, &Holder))
					{
						Sink.Label(Entities[Under].Position + FVector2D(0.0, 600.0),
							FString::Printf(TEXT("in use by aircraft %d"), Holder), EPreviewStyle::Refused);
					}
				}
```
Add `#include "Model/GroundTraffic.h"` and `#include "Model/TrafficOccupancy.h"`.

- [ ] **Step 3: Build, test, commit**

Build. `-Filter Airside.Tool.StandPlace` → `0 failed, 0 crashed`.
```
git add Plugins && git commit -m "feat(tool): the stand remove preview says who is using the stand"
```

---

### Task 6: Full run and PR

- [ ] **Step 1:** `./Tools/Check-Architecture.ps1`; `./Tools/Run-AirsideTests.ps1` → record `N test(s) run, 0 failed, 0 crashed` (expect 155 + 5 = 160).
- [ ] **Step 2:** `UE_LOG` count vs 142 after #61.
- [ ] **Step 3:** Push; `gh pr create --base feature/entity-inspector` (or `main` if #61 has merged) with the template: build line, test line, `UE_LOG` delta, seams and their tests (dispatch claim → `StandClaim`; planner skip → `SkipsHeldStand`; retarget/re-offer → `StandRetarget`; panel → `InspectFacts`; preview → `StandPlace`), and the PIE checklist: land twice, two stands; land a third, notification "every stand it could reach is taken"; delete the stand an inbound aircraft is heading for (Ctrl-click with the Stand tool shows `in use by aircraft N` first), it goes to the other; delete both, panel says `No stand - waiting`, place a stand, it goes.
- [ ] **Step 4:** Update memory `airportmgr-roadnet-state.md`.
