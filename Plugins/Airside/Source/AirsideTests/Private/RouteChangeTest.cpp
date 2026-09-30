#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteChange.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

// ISSUE #429: A ROUTE CHANGE IS ONE STATE CHANGE. Every operation that hands a live agent a new plan used to spell
// its own aftermath - which claims to let go, whether the wait was over, how the goal moved - and each picked a
// different subset. They now name a motion (Model/RouteChange.h) and one function does the steps. These tests hold
// each ENTRY POINT to its steps at the UGroundTraffic level, with real agents on a real claim table: a waiter staged
// on a route (ScriptWait - a refusal, a blocker and 30 s on the stall clock), the entry point called, and the
// table, the arbitration fields, the stall clock and the goal read back. Each asserts the aftermath the entry point
// had before the seam, except where the PR's table says it changed and why: RedirectAgent now ends the wait and lets
// the old line ahead go, as every other change of route did (RestartEndsTheWait, red before the seam).

namespace RouteChangeTest
{
	/** A blocker id no agent has - ScriptWait's refusal names it, and nothing ever clears it but a route change. */
	constexpr int32 PhantomBlocker = 4242;

	/** Seconds on the staged stall clock: past any StallSeconds, so the deadlock pass would count the waiter. */
	constexpr double StagedStall = 30.0;

	/**
	 * A fork: A (0,0) east to B (3000,0), on east to C (6000,0) and E (9000,0), and B north to D (3000,3000) and D on
	 * to C. Short legs, so an agent that has taxied most of the way to B has a claim window reaching past B: it
	 * OCCUPIES the stretch of A -> B it is on and RESERVES B and the line beyond. Authored nodes and edges throughout.
	 */
	struct FFork
	{
		URoadNetwork* Net = nullptr;
		FGuidelineNodeId A, B, C, D, E;
		FGuidelineEdgeId AB, BC, BD, DC, CE;
	};

	FFork Build()
	{
		FFork F;
		F.Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadNetwork& Net = *F.Net;
		F.A = TestGraph::Node(Net, 0.0, 0.0);
		F.B = TestGraph::Node(Net, 3000.0, 0.0);
		F.C = TestGraph::Node(Net, 6000.0, 0.0);
		F.D = TestGraph::Node(Net, 3000.0, 3000.0);
		F.E = TestGraph::Node(Net, 9000.0, 0.0);
		TestGraph::FJoinOptions Authored;
		Authored.bDerived = false;
		F.AB = TestGraph::Join(Net, F.A, F.B, Authored);
		F.BC = TestGraph::Join(Net, F.B, F.C, Authored);
		F.BD = TestGraph::Join(Net, F.B, F.D, Authored);
		F.DC = TestGraph::Join(Net, F.D, F.C, Authored);
		F.CE = TestGraph::Join(Net, F.C, F.E, Authored);
		return F;
	}

	/** Claims AgentId holds, split by what they are. */
	struct FHeld
	{
		int32 Reserved = 0;
		int32 OccupiedGuidelines = 0;
	};

	FHeld HeldBy(const UGroundTraffic& Traffic, int32 AgentId)
	{
		FHeld Held;
		for (const FTrafficClaim& Claim : Traffic.GetOccupancy().GetClaims())
		{
			if (Claim.AgentId != AgentId)
			{
				continue;
			}
			if (!Claim.bOccupied)
			{
				++Held.Reserved;
			}
			else if (Claim.Resource.Kind != ETrafficResourceKind::Surface)
			{
				++Held.OccupiedGuidelines;
			}
		}
		return Held;
	}

	/**
	 * Taxied two thirds of the way to B, still on step 0 (A -> B): near enough that its window reaches past B (a
	 * stopped agent's window is only a gap beyond its nose, and reserves nothing on the next edge), far enough that
	 * a splice at step 1 is ahead of it.
	 */
	void Settle(UGroundTraffic& Traffic, const URoadNetwork& Net, int32 AgentId)
	{
		for (int32 Tick = 0; Tick < 30 * 30; ++Tick)
		{
			const FRoadAgent* Agent = Traffic.FindAgent(AgentId);
			if (Agent == nullptr || Agent->LastMotion.Position.X >= 2000.0)
			{
				return;
			}
			Traffic.Advance(1.0 / 30.0, &Net);
		}
	}

	/** The staged wait - see the file comment. False if the id is unknown. */
	bool StageWait(UGroundTraffic& Traffic, int32 AgentId, FGuidelineNodeId At, int32 BlockedStep = INDEX_NONE)
	{
		return FGroundTrafficTestAccess(Traffic).ScriptWait(AgentId, FTrafficResource::OfNode(At), PhantomBlocker,
			StagedStall, BlockedStep);
	}
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteChangeRestartEndsTheWaitTest,
	"Airside.Model.RouteChange.RestartEndsTheWait",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteChangeRestartEndsTheWaitTest::RunTest(const FString& Parameters)
{
	using namespace RouteChangeTest;
	// REDIRECTAGENT ON A WAITER. Before issue #429 it cleared the arbitration fields (for its reverse-leg gate) and
	// nothing else of the wait: the stall clock ran on into the new route, where the deadlock pass read it as a
	// stalled cycle member on its first refusal, and the old route's line ahead stayed reserved until the next claim
	// pass - for a journey nobody was making. A Restart now ends the wait as a Splice and a Rejoin always did. What
	// it STANDS on stays claimed: a restart does not hop, so its body is where its claims say.
	const FFork F = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = Traffic->DispatchAgent(F.Net, TestGraph::Probe(*F.Net, F.A, F.C, ETraversalClass::Aircraft),
		TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("a plane dispatched A -> C"), Plane > 0)) { return false; }
	Settle(*Traffic, *F.Net, Plane);
	const FHeld Before = HeldBy(*Traffic, Plane);
	AddInfo(FString::Printf(TEXT("before: %d reserved, %d occupied"), Before.Reserved, Before.OccupiedGuidelines));
	if (!TestTrue(TEXT("precondition: the claim pass reserved the line ahead of it"), Before.Reserved > 0)) { return false; }
	if (!TestTrue(TEXT("precondition: and holds the ground it stands on"), Before.OccupiedGuidelines > 0)) { return false; }
	if (!TestTrue(TEXT("the wait is staged"), StageWait(*Traffic, Plane, F.B, 0))) { return false; }

	if (!TestTrue(TEXT("redirected A -> D"),
		Traffic->RedirectAgent(Plane, F.Net, TestGraph::Probe(*F.Net, F.A, F.D, ETraversalClass::Aircraft)))) { return false; }
	const FRoadAgent* P = Traffic->FindAgent(Plane);
	const FHeld After = HeldBy(*Traffic, Plane);
	TestEqual(TEXT("the stall clock restarts: the old wait is not a wait on this route"), P->GetStalledSeconds(), 0.0);
	TestEqual(TEXT("nobody is waited on"), P->GetWaitingOn(), 0);
	TestEqual(TEXT("no step is refused"), P->GetBlockedStep(), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("the old route's line ahead is let go"), After.Reserved, 0);
	TestEqual(TEXT("but what it stands on is kept - a restart does not move it"), After.OccupiedGuidelines, Before.OccupiedGuidelines);
	TestEqual(TEXT("its goal is D"), P->GoalNode, F.D);
	TestTrue(TEXT("the engine carries on - running, as a redirect finds it"), P->bEngineRunning);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteChangeSpliceEndsTheWaitTest,
	"Airside.Model.RouteChange.SpliceEndsTheWait",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteChangeSpliceEndsTheWaitTest::RunTest(const FString& Parameters)
{
	using namespace RouteChangeTest;
	// THE TWO SPLICES, unchanged by the seam: a recalled truck (RerouteAgent - the goal MOVES) and the deadlock
	// resolver's replan (ReplanAt - the goal is KEPT, the search was to it). Both: the reservations for the route
	// that no longer exists let go, the ground under the agent kept, the wait over, the stall clock reset, and the
	// agent still where it was on the unchanged line up to the splice.
	const FFork F = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	// RerouteAgent: a van A -> C, called off at B to D.
	const int32 Van = Traffic->DispatchAgent(F.Net, TestGraph::Probe(*F.Net, F.A, F.C, ETraversalClass::GroundVehicle),
		TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("a van dispatched A -> C"), Van > 0)) { return false; }
	Settle(*Traffic, *F.Net, Van);
	const FHeld VanBefore = HeldBy(*Traffic, Van);
	if (!TestTrue(TEXT("precondition: the van reserved the line ahead"), VanBefore.Reserved > 0)) { return false; }
	const double VanWas = Traffic->FindAgent(Van)->Follower.Travelled;
	if (!TestTrue(TEXT("the van's wait is staged"), StageWait(*Traffic, Van, F.B, 0))) { return false; }
	if (!TestTrue(TEXT("re-routed after step 0 to D"), Traffic->RerouteAgent(Van, F.Net, 1,
		TestGraph::Probe(*F.Net, F.B, F.D, ETraversalClass::GroundVehicle)))) { return false; }
	const FRoadAgent* V = Traffic->FindAgent(Van);
	TestEqual(TEXT("reroute: the stall clock restarts"), V->GetStalledSeconds(), 0.0);
	TestEqual(TEXT("reroute: nobody is waited on"), V->GetWaitingOn(), 0);
	TestEqual(TEXT("reroute: the line ahead is let go"), HeldBy(*Traffic, Van).Reserved, 0);
	TestEqual(TEXT("reroute: the ground under it is kept"), HeldBy(*Traffic, Van).OccupiedGuidelines, VanBefore.OccupiedGuidelines);
	TestEqual(TEXT("reroute: the goal moved to D"), V->GoalNode, F.D);
	TestEqual(TEXT("reroute: Travelled survives the splice - it is on the same line"), V->Follower.Travelled, VanWas);
	TestEqual(TEXT("reroute: still Taxiing"), V->Phase, EAgentPhase::Taxiing);
	Traffic->RetireAgent(Van);

	// ReplanAt: a plane A -> C by B, replanned at step 1 with B -> C banned: by D instead, to the same C.
	const int32 Plane = Traffic->DispatchAgent(F.Net, TestGraph::Probe(*F.Net, F.A, F.C, ETraversalClass::Aircraft),
		TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("a plane dispatched A -> C"), Plane > 0)) { return false; }
	Settle(*Traffic, *F.Net, Plane);
	const FHeld PlaneBefore = HeldBy(*Traffic, Plane);
	if (!TestTrue(TEXT("precondition: the plane reserved the line ahead"), PlaneBefore.Reserved > 0)) { return false; }
	if (!TestTrue(TEXT("the plane's wait is staged"), StageWait(*Traffic, Plane, F.C, 1))) { return false; }
	if (!TestTrue(TEXT("replanned at step 1 round the banned B -> C"),
		FGroundTrafficTestAccess(*Traffic).ReplanAt(Plane, *F.Net, 1, F.BC))) { return false; }
	const FRoadAgent* P = Traffic->FindAgent(Plane);
	TestEqual(TEXT("replan: the stall clock restarts, or the resolver would ask again next tick"), P->GetStalledSeconds(), 0.0);
	TestEqual(TEXT("replan: nobody is waited on"), P->GetWaitingOn(), 0);
	TestEqual(TEXT("replan: the line ahead is let go"), HeldBy(*Traffic, Plane).Reserved, 0);
	TestEqual(TEXT("replan: the ground under it is kept"), HeldBy(*Traffic, Plane).OccupiedGuidelines, PlaneBefore.OccupiedGuidelines);
	TestEqual(TEXT("replan: the goal is KEPT - the search was to it"), P->GoalNode, F.C);
	TestTrue(TEXT("replan: and the route now goes by D"),
		P->Follower.Plan.Steps.ContainsByPredicate([&](const FRouteStep& Step) { return Step.To == F.D; }));
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteChangeExtendKeepsTheWaitTest,
	"Airside.Model.RouteChange.ExtendKeepsTheWait",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteChangeExtendKeepsTheWaitTest::RunTest(const FString& Parameters)
{
	using namespace RouteChangeTest;
	// AN EXTENSION KEEPS EVERY STEP THE AGENT HAD, so its wait on them stands - the one motion (with a rebuild's
	// truncation) that does not end it. Nothing is let go, the refusal and the stall clock carry on, and the goal
	// still moves to the extended end through ReleaseGoal/TakeGoal. Unchanged by the seam.
	const FFork F = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = Traffic->DispatchAgent(F.Net, TestGraph::Probe(*F.Net, F.A, F.C, ETraversalClass::Aircraft),
		TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("a plane dispatched A -> C"), Plane > 0)) { return false; }
	Settle(*Traffic, *F.Net, Plane);
	const FHeld Before = HeldBy(*Traffic, Plane);
	if (!TestTrue(TEXT("precondition: it reserved the line ahead (B and beyond, short of its goal C)"), Before.Reserved > 0)) { return false; }
	if (!TestTrue(TEXT("the wait is staged"), StageWait(*Traffic, Plane, F.B, 0))) { return false; }

	if (!TestTrue(TEXT("extended C -> E"),
		Traffic->ExtendRoute(Plane, F.Net, TestGraph::Probe(*F.Net, F.C, F.E, ETraversalClass::Aircraft)))) { return false; }
	const FRoadAgent* P = Traffic->FindAgent(Plane);
	TestEqual(TEXT("the stall clock carries on: it is the same wait on the same steps"), P->GetStalledSeconds(), StagedStall);
	TestEqual(TEXT("the refusal stands"), P->GetWaitingOn(), PhantomBlocker);
	TestEqual(TEXT("on the same step"), P->GetBlockedStep(), 0);
	TestEqual(TEXT("nothing is let go: every reservation still describes its route"), HeldBy(*Traffic, Plane).Reserved, Before.Reserved);
	TestEqual(TEXT("nor the ground under it"), HeldBy(*Traffic, Plane).OccupiedGuidelines, Before.OccupiedGuidelines);
	TestEqual(TEXT("the goal moved to the extended end"), P->GoalNode, F.E);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteChangeTruncateKeepsTheWaitTest,
	"Airside.Model.RouteChange.TruncateKeepsTheWait",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteChangeTruncateKeepsTheWaitTest::RunTest(const FString& Parameters)
{
	using namespace RouteChangeTest;
	// A REBUILD THAT CUTS THE ROUTE SHORT: the step ahead of the agent lost its edge and no route replaces it, so the
	// agent keeps the prefix of its own line and brakes to a stop at its new end. Its speed and Travelled survive (it
	// is on the same line), the goal is re-pointed at the new end, and the stall clock is NOT reset - the rebuild
	// re-arbitrates everybody in its own claim pass straight after, and a reset clock would be the one thing that
	// pass did not put back. Unchanged by the seam.
	const FFork F = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = Traffic->DispatchAgent(F.Net, TestGraph::Probe(*F.Net, F.A, F.C, ETraversalClass::Aircraft),
		TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("a plane dispatched A -> C"), Plane > 0)) { return false; }
	Settle(*Traffic, *F.Net, Plane);
	if (!TestTrue(TEXT("its route is A -> B -> C"), Traffic->FindAgent(Plane)->Follower.Plan.Steps.Num() == 2)) { return false; }
	const double SpeedWas = Traffic->FindAgent(Plane)->Follower.Speed;
	const double TravelledWas = Traffic->FindAgent(Plane)->Follower.Travelled;
	if (!TestTrue(TEXT("the wait is staged"), StageWait(*Traffic, Plane, F.B, 0))) { return false; }

	// B -> C AND D -> C GO: C is left with no way in, so the step ahead cannot be replanned - only cut.
	F.Net->RemoveGuidelineEdge(F.BC);
	F.Net->RemoveGuidelineEdge(F.DC);
	Traffic->OnGraphRebuilt(*F.Net);

	const FRoadAgent* P = Traffic->FindAgent(Plane);
	TestEqual(TEXT("truncated to its first step"), P->Follower.Plan.Steps.Num(), 1);
	TestEqual(TEXT("the goal re-pointed at the new end, B"), P->GoalNode, F.B);
	TestEqual(TEXT("the stall clock is left for the rebuild's own claim pass: not reset"), P->GetStalledSeconds(), StagedStall);
	TestEqual(TEXT("its speed survives - the same line"), P->Follower.Speed, SpeedWas);
	TestEqual(TEXT("and so does Travelled"), P->Follower.Travelled, TravelledWas);
	TestEqual(TEXT("still Taxiing"), P->Phase, EAgentPhase::Taxiing);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteChangeRejoinEndsTheWaitTest,
	"Airside.Model.RouteChange.RejoinEndsTheWait",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteChangeRejoinEndsTheWaitTest::RunTest(const FString& Parameters)
{
	using namespace RouteChangeTest;
	// THE PLAYER'S RESCUE, a Rejoin: every guideline claim let go, the wait over (a stall clock that ran while
	// stranded is not a stall on this line), the goal moved to where the player sent it - and the OLD goal's claim
	// freed with it, which is what RescueStranded's SetGoal(WasGoal) existed to arrange before RejoinNearby stopped
	// writing the goal itself. Unchanged by the seam.
	const FFork F = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = Traffic->DispatchAgent(F.Net, TestGraph::Probe(*F.Net, F.A, F.C, ETraversalClass::Aircraft),
		TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("a plane dispatched A -> C"), Plane > 0)) { return false; }
	TickUntil(*Traffic, *F.Net, 2.0, [](int32) { return true; });
	if (!TestTrue(TEXT("stranded where it stands"), FGroundTrafficTestAccess(*Traffic).Strand(Plane))) { return false; }
	TickUntil(*Traffic, *F.Net, 0.5, [](int32) { return true; });
	if (!TestEqual(TEXT("precondition: stranded"), Traffic->FindAgent(Plane)->Phase, EAgentPhase::Stranded)) { return false; }
	if (!TestTrue(TEXT("the wait is staged"), StageWait(*Traffic, Plane, F.B))) { return false; }

	if (!TestTrue(TEXT("rescued toward D"), Traffic->RescueStranded(Plane, *F.Net, F.D))) { return false; }
	const FRoadAgent* P = Traffic->FindAgent(Plane);
	TestEqual(TEXT("taxiing again"), P->Phase, EAgentPhase::Taxiing);
	TestEqual(TEXT("the stall clock restarts"), P->GetStalledSeconds(), 0.0);
	TestEqual(TEXT("nobody is waited on"), P->GetWaitingOn(), 0);
	TestEqual(TEXT("its goal is D"), P->GoalNode, F.D);
	// NOT ASSERTED HERE: the claims a Rejoin lets go. A stranded agent already gave its guideline claims back when
	// it was stranded, so a count here would pass whatever the rescue did; the old goal's claim going is pinned by
	// Airside.Model.Traffic.RescueStranded.NewGoal, and what each motion lets go, on a table holding all three kinds
	// of claim, by Airside.Model.RouteChange.ReleaseByMotion.
	return true;
}

// =======================================================================================
// THE SEAM ITSELF, below the entry points: FRoadAgent::ApplyRouteChange on a bare agent and a bare table, so each
// choice a FRouteChange names is measured on its own - including the ones no entry point above can show (a stranded
// agent has no guideline claims left to let go; a rebuild re-arbitrates straight after a truncation).
namespace RouteChangeTest
{
	/** A taxiing agent on A -> C with a claim of each kind and a staged wait. See ReleaseByMotion. */
	struct FBareAgent
	{
		FRoadAgent Agent;
		FTrafficOccupancy Occupancy;
		FRoadSegmentId Strip;
	};

	constexpr int32 BareId = 7;

	void Stage(FBareAgent& Out, const FFork& F, const FRoutePlan& Live, FRoadSegmentId Strip)
	{
		Out.Agent = FRoadAgent();
		Out.Agent.Id = BareId;
		Out.Agent.StartTaxi(Live, TestAirframes::GroundOnly());
		Out.Agent.SetGoalFrom(Live);
		Out.Occupancy = FTrafficOccupancy();
		Out.Strip = Strip;
		// THE GROUND UNDER IT (occupied guideline), THE LINE AHEAD (reserved guideline) AND A RUNWAY ITS BODY IS ON
		// (occupied surface) - the three things the three ERouteRelease values tell apart.
		Out.Occupancy.Assert(FTrafficClaim::Make(BareId, FTrafficResource::OfNode(F.A), /*bOccupied*/ true));
		Out.Occupancy.Assert(FTrafficClaim::Make(BareId, FTrafficResource::OfNode(F.B), /*bOccupied*/ false));
		Out.Occupancy.Assert(FTrafficClaim::Make(BareId, FTrafficResource::OfSurface(Strip), /*bOccupied*/ true));
		Out.Agent.Refuse(1, FTrafficResource::OfNode(F.C), 500.0, PhantomBlocker);
		Out.Agent.AccrueStall(StagedStall);
	}

	/** Which of the three staged claims survive: occupied node A, reserved node B, occupied strip. */
	FString Survivors(const FBareAgent& Bare, const FFork& F)
	{
		const bool bGround = Bare.Occupancy.IsHeld(FTrafficResource::OfNode(F.A), 0);
		const bool bAhead = Bare.Occupancy.IsHeld(FTrafficResource::OfNode(F.B), 0);
		const bool bStrip = Bare.Occupancy.IsHeld(FTrafficResource::OfSurface(Bare.Strip), 0);
		return FString::Printf(TEXT("%s%s%s"), bGround ? TEXT("ground ") : TEXT(""), bAhead ? TEXT("ahead ") : TEXT(""),
			bStrip ? TEXT("strip") : TEXT("")).TrimEnd();
	}
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteChangeReleaseByMotionTest,
	"Airside.Model.RouteChange.ReleaseByMotion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteChangeReleaseByMotionTest::RunTest(const FString& Parameters)
{
	using namespace RouteChangeTest;
	// ONE ROW PER MOTION: what each lets go of a table holding all three kinds of claim, whether it ends the wait,
	// and where the goal ends up. The row IS the contract Model/RouteChange.h states in prose; a change to a
	// factory's defaults, or to ApplyRouteChange's switches, turns exactly one row red.
	const FFork F = Build();
	const FRoadNodeId RA = F.Net->AddNode(FVector2D(-20000.0, -5000.0));
	const FRoadNodeId RB = F.Net->AddNode(FVector2D(20000.0, -5000.0));
	const FRoadSegmentId Strip = F.Net->AddStraightSegment(RA, RB, TestProfiles::Runway());
	const FRoutePlan Live = TestGraph::Probe(*F.Net, F.A, F.C, ETraversalClass::Aircraft);
	const FRoutePlan ToD = TestGraph::Probe(*F.Net, F.A, F.D, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("two routes to change between"), Live.IsValid() && ToD.IsValid())) { return false; }

	struct FRow
	{
		const TCHAR* Name;
		FRouteChange Change;
		const TCHAR* Survive;
		bool bWaitStands;
		FGuidelineNodeId Goal;
	};
	FRouteChange ExtendKept = FRouteChange::Extend(ToD, 0.0);
	ExtendKept.Goal = ERouteGoal::Keep;   // Extend's own factory MOVES the goal, which the agent alone refuses.
	const FRow Rows[] = {
		{ TEXT("Extend"), ExtendKept, TEXT("ground ahead strip"), true, F.C },
		{ TEXT("Truncate"), FRouteChange::Truncate(ToD), TEXT("ground ahead strip"), true, F.D },
		{ TEXT("Splice"), FRouteChange::Splice(ToD, ERouteGoal::Keep), TEXT("ground strip"), false, F.C },
		{ TEXT("Restart, reservations"), FRouteChange::Restart(ToD, ERouteRelease::Reservations, ERouteGoal::Keep, 0.0, TOptional<double>()),
			TEXT("ground strip"), false, F.C },
		{ TEXT("Restart, reservations and guidelines"), FRouteChange::Restart(ToD, ERouteRelease::ReservationsAndGuidelines, ERouteGoal::Repoint, 0.0, TOptional<double>()),
			TEXT("strip"), false, F.D },
		{ TEXT("Rejoin"), FRouteChange::Rejoin(ToD, ERouteGoal::Repoint, 0.0, FVector2D(10.0, 0.0)), TEXT("strip"), false, F.D },
	};
	for (const FRow& Row : Rows)
	{
		FBareAgent Bare;
		Stage(Bare, F, Live, Strip);
		if (!TestTrue(FString::Printf(TEXT("%s: applied"), Row.Name), Bare.Agent.ApplyRouteChange(Row.Change, Bare.Occupancy)))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s: lets go of the right claims"), Row.Name), Survivors(Bare, F), FString(Row.Survive));
		TestEqual(FString::Printf(TEXT("%s: the wait %s"), Row.Name, Row.bWaitStands ? TEXT("stands") : TEXT("is over")),
			Bare.Agent.GetWaitingOn(), Row.bWaitStands ? PhantomBlocker : 0);
		TestEqual(FString::Printf(TEXT("%s: the stall clock %s"), Row.Name, Row.bWaitStands ? TEXT("runs on") : TEXT("restarts")),
			Bare.Agent.GetStalledSeconds(), Row.bWaitStands ? StagedStall : 0.0);
		TestEqual(FString::Printf(TEXT("%s: the goal"), Row.Name), Bare.Agent.GoalNode, Row.Goal);
		TestEqual(FString::Printf(TEXT("%s: the follower took the new plan"), Row.Name),
			Bare.Agent.Follower.Plan.Steps.Num(), ToD.Steps.Num());
		TestEqual(FString::Printf(TEXT("%s: taxiing"), Row.Name), Bare.Agent.Phase, EAgentPhase::Taxiing);
	}

	// THE POSE EACH MOTION KEEPS, on an agent that is rolling and pointed somewhere the new line does not start.
	FBareAgent Rolling;
	Stage(Rolling, F, Live, Strip);
	FAgentMotion Motion;
	EAgentEvent Event = EAgentEvent::None;
	for (int32 Tick = 0; Tick < 60; ++Tick) { Rolling.Agent.Advance(1.0 / 30.0, Motion, Event); }
	const double Speed = Rolling.Agent.Follower.Speed;
	const double Travelled = Rolling.Agent.Follower.Travelled;
	if (!TestTrue(TEXT("rolling"), Speed > 0.0 && Travelled > 0.0)) { return false; }
	{
		FBareAgent Splice = Rolling;
		Splice.Agent.ApplyRouteChange(FRouteChange::Splice(ToD, ERouteGoal::Keep), Splice.Occupancy);
		TestEqual(TEXT("Splice: Travelled survives - the same line up to the splice"), Splice.Agent.Follower.Travelled, Travelled);
		TestEqual(TEXT("Splice: and the speed"), Splice.Agent.Follower.Speed, Speed);
	}
	{
		FBareAgent Rejoin = Rolling;
		const FVector2D At(Rolling.Agent.LastMotion.Position.X, 5.0);
		Rejoin.Agent.ApplyRouteChange(FRouteChange::Rejoin(ToD, ERouteGoal::Keep, 42.0, At), Rejoin.Occupancy);
		TestEqual(TEXT("Rejoin: the speed is kept - still rolling"), Rejoin.Agent.Follower.Speed, Speed);
		TestEqual(TEXT("Rejoin: seated At"), Rejoin.Agent.LastMotion.Position, At);
		TestEqual(TEXT("Rejoin: Travelled along the new line's first step"), Rejoin.Agent.Follower.Travelled, 42.0);
		TestEqual(TEXT("Rejoin: the heading kept"), Rejoin.Agent.LastMotion.Heading, Rolling.Agent.LastMotion.Heading);
	}
	{
		FBareAgent Restart = Rolling;
		const double Heading = 1.25;
		Restart.Agent.ApplyRouteChange(FRouteChange::Restart(ToD, ERouteRelease::Reservations, ERouteGoal::Keep, 0.0, Heading),
			Restart.Occupancy);
		TestEqual(TEXT("Restart: from rest"), Restart.Agent.Follower.Speed, 0.0);
		TestEqual(TEXT("Restart: the kept heading is posed"), Restart.Agent.LastMotion.Heading, Heading);
		TestEqual(TEXT("Restart: where it stood"), Restart.Agent.LastMotion.Position, Rolling.Agent.LastMotion.Position);
		TestTrue(TEXT("Restart: the engine carried on, not started cold"), Restart.Agent.bEngineRunning && Restart.Agent.GetEngineRPM() > 0.0);
	}
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteChangeAgentAloneRefusesAGoalMoveTest,
	"Airside.Model.RouteChange.AgentAloneRefusesAGoalMove",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteChangeAgentAloneRefusesAGoalMoveTest::RunTest(const FString& Parameters)
{
	using namespace RouteChangeTest;
	// A GOAL THAT MOVES HAS CLAIMS ONLY THE TRAFFIC CAN MOVE - the old goal's, the stand-freed flag, the new goal's
	// departure arming and claim (UGroundTraffic::ReleaseGoal / TakeGoal). The agent alone refuses the change whole,
	// before anything is written, and says so as an Error: a goal moved without its claim is a stand held for ever by
	// an agent going somewhere else. Not an ensure - a test must never raise one on purpose.
	const FFork F = Build();
	const FRoadNodeId RA = F.Net->AddNode(FVector2D(-20000.0, -5000.0));
	const FRoadNodeId RB = F.Net->AddNode(FVector2D(20000.0, -5000.0));
	const FRoadSegmentId Strip = F.Net->AddStraightSegment(RA, RB, TestProfiles::Runway());
	const FRoutePlan Live = TestGraph::Probe(*F.Net, F.A, F.C, ETraversalClass::Aircraft);
	const FRoutePlan ToD = TestGraph::Probe(*F.Net, F.A, F.D, ETraversalClass::Aircraft);
	FBareAgent Bare;
	Stage(Bare, F, Live, Strip);

	AddExpectedError(TEXT("MOVES its goal was applied to the agent alone"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("refused"), Bare.Agent.ApplyRouteChange(FRouteChange::Splice(ToD, ERouteGoal::Move), Bare.Occupancy));
	TestEqual(TEXT("the follower still has its own route"), Bare.Agent.Follower.Plan.Steps.Num(), Live.Steps.Num());
	TestEqual(TEXT("the goal is untouched"), Bare.Agent.GoalNode, F.C);
	TestEqual(TEXT("no claim was let go"), Survivors(Bare, F), FString(TEXT("ground ahead strip")));
	TestEqual(TEXT("and the wait stands"), Bare.Agent.GetStalledSeconds(), StagedStall);
	return true;
}

#endif
