#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FRoutePlan PreemptionRoute(const URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B, ETraversalClass Class)
	{
		FRouteQuery Query;
		Query.Start = A;
		Query.Goal = B;
		Query.Class = Class;
		return RouteSearch::Find(Net, Query);
	}
}

// Issue #194 (2026-09-21 test-suite review). UGroundTraffic::Arbitrate's own comment on its
// re-pass loop - "a preempted agent drives a whole frame on a reservation it no longer holds
// - into the very node that was just taken from it" - is measured only at the TABLE level:
// TrafficOccupancyTest's own preemption scenarios (#3 and #9 in its RunTest) prove
// FTrafficOccupancy::TakePreempted reports the right agent, but nothing composed at the
// UGroundTraffic level proves the RE-PASS LOOP ITSELF - the
// `for (const int32 AgentId : Occupancy.TakePreempted())` block in GroundTraffic.cpp's
// Arbitrate - is what re-runs FClaimPass::Run for the loser and so restores ITS OWN
// WaitingOn/StopWithin inside the SAME tick.
//
// WHY THE PLANE IS THE ONE PREEMPTED HERE, not the van the issue's own wording leads with -
// measured, not assumed. Arbitrate's Order sorts by TraversalPriority(Class) ALONE
// (Aircraft=2, GroundVehicle=0 - RoadTraffic.cpp), never by which agent got where first, so
// a plane and a van with no override ALWAYS run plane-then-van in the SAME call: if the
// plane's own turn preempts the van's stale reservation, the van's TURN STILL HAS NOT
// HAPPENED YET this tick and re-derives its claim fresh regardless of the re-pass - proven by
// running exactly that construction with the re-pass loop commented out and watching it stay
// green (see the PR body). The re-pass only ever matters for whichever agent is preempted
// AFTER its own turn in Order has already run, and Order never puts a GroundVehicle before an
// Aircraft on class alone. FTrafficPriorityOverrideTest (this file's neighbour) already
// proves FGuidelineNode::PriorityOverride can flip who WINS a contested resource at one node
// without touching Order at all ("vehicles first" at J, spec 5.4) - so a plane reserves J
// first here, and a van dispatched after it takes J away at THIS node only, preempting the
// plane's already-completed turn. Commenting out the re-pass loop and re-running this test
// reproduced the exact failure the issue describes: WaitingOn stuck at 0 and StopWithin stuck
// at TNumericLimits<double>::Max() (verified for this PR - see the PR body).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficPreemptedAgentGetsARePassTest,
	"Airside.Model.Traffic.PreemptedAgentGetsARePass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficPreemptedAgentGetsARePassTest::RunTest(const FString& Parameters)
{
	// A SHORT FIRST STEP FOR BOTH APPROACHES, but not TOO short - it has to sit inside
	// [Footprint/2, Footprint+Gap) at rest. Above Footprint/2 so the claim on J is a
	// RESERVATION rather than an OCCUPANCY (FClaimPass::BuildPending: "Occupied only while
	// the CENTRE is within half a footprint of the node" - a shorter step there made J
	// un-preemptable outright, physical presence beating any rank - measured the hard way,
	// see the PR body). Below Footprint+Gap so the "box junction" entry rule still claims it
	// on the very first tick, with no need to run either agent up to speed first over a long
	// approach the way FTrafficNodeYieldTest's crossing does. 700 uu clears both bars for the
	// plane (500 / 2500) and the van (425 / 1150) - TrafficRules.h's own AircraftFootprint/Gap
	// and VehicleFootprint/Gap defaults.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId PlaneStart = TestGraph::Node(*Net, 0.0, -700.0);
	const FGuidelineNodeId J = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId VanStart = TestGraph::Node(*Net, -700.0, 0.0);
	TestGraph::Join(*Net, PlaneStart, J);
	TestGraph::Join(*Net, VanStart, J);

	// VEHICLES FIRST AT J, and only at J - the same override FTrafficPriorityOverrideTest
	// authors on its own four-way crossing (spec 5.4's per-node exception). This is the ONLY
	// thing that lets the van's turn - second in Order regardless - outrank a reservation the
	// plane's own, earlier turn already holds there.
	//
	// THROUGH FRoadNetworkTestAccess (#191), not a raw GetGuidelineNodeMutable - the same
	// friend struct FTrafficPriorityOverrideTest goes through, since no production caller
	// writes PriorityOverride yet (see FGuidelineNode's own comment).
	FRoadNetworkTestAccess(*Net).SetGuidelineNodePriorityOverrideForTest(J,
		{ ETraversalClass::GroundVehicle, ETraversalClass::Aircraft });

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	// THE PLANE RESERVES J FIRST, alone: nothing else exists yet to contest it.
	const int32 Plane = Traffic->DispatchAgent(Net, PreemptionRoute(*Net, PlaneStart, J, ETraversalClass::Aircraft),
		TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("the plane dispatches"), Plane > 0)) { return false; }

	// ONE Advance CALL, ONE Arbitrate CALL: 0.03s is under FTrafficRules::MaxSubstepSeconds
	// (1/30 s, the default), so Advance's own Steps arithmetic (CeilToInt(Delta/Longest))
	// clamps to exactly one AdvanceOnce - unlike TickUntil's default 0.05s step, which would
	// run two.
	Traffic->Advance(0.03, Net);

	const FRoadAgent* PlaneAgent = Traffic->FindAgent(Plane);
	if (!TestNotNull(TEXT("the plane agent exists"), PlaneAgent)) { return false; }
	if (!TestEqual(TEXT("the plane holds J outright - nothing contests it yet"),
		PlaneAgent->GetWaitingOn(), 0)) { return false; }

	// THE VAN DISPATCHES LATER, on a route that also ends at J. Order still runs the plane's
	// OWN turn first this tick (Order sorts on TraversalPriority(Class) alone, never on the
	// override) - the van's turn, second, is where J's override actually bites.
	const int32 Van = Traffic->DispatchAgent(Net, PreemptionRoute(*Net, VanStart, J, ETraversalClass::GroundVehicle),
		TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the van dispatches"), Van > 0)) { return false; }

	// ONE MORE Advance, ONE MORE Arbitrate call. The plane's OWN turn (first in Order) has
	// already run and finished BEFORE the van's turn takes J away from it - so whether the
	// PLANE's WaitingOn/StopWithin ever catch up inside this SAME tick is exactly what the
	// re-pass loop over TakePreempted is for; nothing else in this Arbitrate call revisits it.
	Traffic->Advance(0.03, Net);

	PlaneAgent = Traffic->FindAgent(Plane);
	if (!TestNotNull(TEXT("the plane agent still exists"), PlaneAgent)) { return false; }

	TestEqual(FString::Printf(TEXT("the re-pass names the van as who the plane is now waiting on ")
		TEXT("(plane WaitingOn %d, want %d)"), PlaneAgent->GetWaitingOn(), Van),
		PlaneAgent->GetWaitingOn(), Van);
	TestTrue(FString::Printf(TEXT("and StopWithin is a real distance, not the Max() a stale grant ")
		TEXT("would leave it at (measured %.1f)"), PlaneAgent->GetStopWithin()),
		PlaneAgent->GetStopWithin() < TNumericLimits<double>::Max());
	return true;
}

#endif
