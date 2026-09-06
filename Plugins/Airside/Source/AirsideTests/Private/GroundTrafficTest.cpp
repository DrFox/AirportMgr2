#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
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
	double VanMinSpeedWhileWaiting = TNumericLimits<double>::Max();
	int32 VanBlockedTicks = 0;
	bool bVanWaitedOnPlane = false;
	const int32 Ticks = M2TrafficRun(*Traffic, *Net, 120.0, [&](int32)
	{
		const FRoadAgent* P = Traffic->FindAgent(Plane);
		const FRoadAgent* V = Traffic->FindAgent(Van);
		if (P == nullptr || V == nullptr) { return false; }
		if (P->Phase == EAgentPhase::Taxiing) { PlaneMinStopWithin = FMath::Min(PlaneMinStopWithin, P->StopWithin); }
		if (V->Phase == EAgentPhase::Taxiing) { VanMinStopWithin = FMath::Min(VanMinStopWithin, V->StopWithin); }
		if (V->WaitingOn == Plane)
		{
			bVanWaitedOnPlane = true;
			++VanBlockedTicks;
			VanMinSpeedWhileWaiting = FMath::Min(VanMinSpeedWhileWaiting, V->Follower.Speed);
		}
		MinSeparation = FMath::Min(MinSeparation, FVector2D::Distance(P->LastMotion.Position, V->LastMotion.Position));
		return !(P->Phase == EAgentPhase::Parked && V->Phase == EAgentPhase::Parked);
	});

	// MEASURED AND LOGGED, so a failure is read off numbers rather than re-derived from the
	// assertion text. The window arithmetic behind every one of these is in spec 3.1-3.2.
	UE_LOG(LogM2TrafficTest, Log,
		TEXT("NodeYield measured: %d ticks, min separation %.0f uu, van min StopWithin %.0f uu, ")
		TEXT("van blocked %d ticks, van min speed while blocked %.0f uu/s, plane min StopWithin %.0f"),
		Ticks, MinSeparation, VanMinStopWithin, VanBlockedTicks, VanMinSpeedWhileWaiting, PlaneMinStopWithin);

	// A YIELD IS A SPEED DROP WHILE BLOCKED, NOT NECESSARILY A DEAD STOP, and this geometry
	// cannot produce a dead stop however the arbiter is written. The yielder's first refusal
	// always lands exactly on its own braking distance - it is refused at
	// End - Gap - Footprint/2 - v^2/2D and told to stop at End - Gap - so it reaches a
	// standstill only if the blocker holds the node for longer than the whole braking time,
	// i.e. only if Gap_yield + Footprint_blocker/2 >= v^2/(2 Decel). Here that is
	// 300 + 500 = 800 uu against 2500 uu, so it cannot be: van and aircraft share the taxi
	// figures (Accel 100, Decel 200, cap 1000) and reach the crossing together by design.
	//
	// MEASURED: blocked for 71 ticks (3.55 s), speed 1000 -> 335 uu/s, StopWithin down to
	// 281 uu, never closer than 711 uu to the aircraft. Half the cap is the threshold
	// because it is well clear of both the 1000 it was doing and the 335 it came down to,
	// so this fails if the van merely dawdles and if it does not slow at all.
	TestTrue(TEXT("the van yielded: its speed fell below half its taxi cap while blocked"),
		VanMinSpeedWhileWaiting < 0.5 * Traffic->FindAgent(Van)->Follower.Ground.Taxi.SpeedCap);
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

	// THE ASSERTED ONE is measured only once the follower has actually moved: the dispatch
	// separation (205 uu - the leader has been accelerating for 2 s at 100 uu/s^2 when the
	// second aircraft is put on the line behind it) is sampled before the arbiter has been
	// consulted once, so MinGap below measures the fixture's initial condition and not
	// anything this class did. Both are logged; the assertion uses the second.
	double MinGapUnderWay = TNumericLimits<double>::Max();
	bool bFollowerCaughtUp = false;
	const int32 Ticks = M2TrafficRun(*Traffic, *Net, 200.0, [&](int32 Tick)
	{
		if (Tick == 40) { Follow = Traffic->DispatchAgent(Net, Plan, M2TrafficPlane(), ETraversalClass::Aircraft, 1.0); }
		const FRoadAgent* L = Traffic->FindAgent(Lead);
		const FRoadAgent* Fo = Follow > 0 ? Traffic->FindAgent(Follow) : nullptr;
		if (L == nullptr || Fo == nullptr) { return L != nullptr; }
		if (L->Phase == EAgentPhase::Taxiing && Fo->Phase == EAgentPhase::Taxiing)
		{
			const double Gap = L->Follower.Travelled - Fo->Follower.Travelled;
			MinGap = FMath::Min(MinGap, Gap);
			if (Fo->Follower.Travelled > 0.0) { MinGapUnderWay = FMath::Min(MinGapUnderWay, Gap); }
			if (Fo->WaitingOn == Lead) { bFollowerCaughtUp = true; }
		}
		return !(L->Phase == EAgentPhase::Parked && Fo->Phase == EAgentPhase::Parked);
	});

	UE_LOG(LogM2TrafficTest, Log,
		TEXT("CarFollowing measured: %d ticks, min centre-to-centre gap %.0f uu (from dispatch), ")
		TEXT("%.0f uu once the follower was under way, floor %.0f uu, caught up %d"),
		Ticks, MinGap, MinGapUnderWay,
		Traffic->Rules.AircraftFootprint * 0.5 + Traffic->Rules.AircraftGap - 50.0,
		bFollowerCaughtUp ? 1 : 0);

	TestTrue(TEXT("the follower did catch the leader (otherwise this measures nothing)"), bFollowerCaughtUp);
	// The follower's CENTRE stops Gap behind the leader's TAIL (leader centre - Footprint/2),
	// so centre-to-centre is Footprint/2 + Gap: nose to tail is exactly the gap.
	TestTrue(FString::Printf(TEXT("centre-to-centre never below footprint/2 + gap once under way (%.0f)"), MinGapUnderWay),
		MinGapUnderWay >= Traffic->Rules.AircraftFootprint * 0.5 + Traffic->Rules.AircraftGap - 50.0);
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

	UE_LOG(LogM2TrafficTest, Log,
		TEXT("HeadOnStops measured: min separation %.0f uu, travelled %.0f / %.0f, speed %.4f / %.4f, waiting on %d / %d"),
		MinSeparation, X->Follower.Travelled, Y->Follower.Travelled, X->Follower.Speed, Y->Follower.Speed,
		X->WaitingOn, Y->WaitingOn);

	TestTrue(TEXT("both are stopped"), X->Follower.Speed < 1e-6 && Y->Follower.Speed < 1e-6);
	TestTrue(TEXT("both are still taxiing, not parked"), X->Phase == EAgentPhase::Taxiing && Y->Phase == EAgentPhase::Taxiing);
	TestTrue(TEXT("each waits on the other"), X->WaitingOn == P2 && Y->WaitingOn == P1);
	TestTrue(FString::Printf(TEXT("never closer than one footprint (%.0f)"), MinSeparation), MinSeparation >= Traffic->Rules.AircraftFootprint - 1.0);

	// AND THE DEADLOCK PASS SEES IT, ONCE. Two aircraft nose to nose on one edge is a
	// two-member cycle, so the detector must find it - but neither is AT the node where its
	// refused edge begins (both are 19 km along it), so neither can turn without reversing,
	// which spec §1 rules out of M2. The cycle is therefore logged and retried on the
	// RetrySeconds cadence rather than resolved, and the count stays at one however many
	// retry windows the 120 s run covers: the members' LastResolveAttempt stamp is what
	// keeps a re-detected cycle from being a re-logged one.
	TestEqual(TEXT("the all-aircraft cycle was detected once"), Traffic->GetCyclesDetectedForTest(), 1);
	TestEqual(TEXT("nobody could turn, so nobody replanned"), Traffic->GetLastResolvedAgentForTest(), 0);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficBoxEntryTest,
	"Airside.Model.Traffic.BoxEntry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficBoxEntryTest::RunTest(const FString& Parameters)
{
	// THE THREE-VEHICLE TRIANGLE spec §3.1 argues the box-entry rule from, and the fixture
	// Task 8's resolver is built on. Three one-way 600 uu arms; every arm is a BOX for a van
	// (600 < Footprint 500 + Gap 300), so no van can stand on one without still blocking the
	// node behind it. Each van stands on a node and wants the node after next, so each needs
	// the arm its neighbour is standing on.
	//
	// What is pinned here is that the gridlock forms AT THE NODES and is VISIBLE: every van
	// stopped dead, every van naming the one it waits for, and the wait-for edges closing
	// into a cycle. Without the entry rule a van drives into an arm and stops inside the
	// junction, where a replan has nowhere to turn; without occupancy beating a stale
	// reservation the wait-for graph is a fan into one van rather than a cycle, and Task 8
	// would find nothing to resolve.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = M2TrafficNode(*Net, 600.0, 0.0);
	const FGuidelineNodeId C = M2TrafficNode(*Net, 300.0, 519.6);
	M2TrafficJoin(*Net, A, B, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, B, C, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, C, A, EGuidelineDir::AToB);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 V1 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V2 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, B, A, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V3 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, C, B, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("all three routed and dispatched"), V1 > 0 && V2 > 0 && V3 > 0)) { return false; }

	// ONE tick. The cycle must be complete after the first arbitration pass and not after a
	// settling period: Task 8 starts its stall clock from here.
	Traffic->Advance(0.05, Net);

	const FRoadAgent* Vans[3] = { Traffic->FindAgent(V1), Traffic->FindAgent(V2), Traffic->FindAgent(V3) };
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const FRoadAgent* Van = Vans[Index];
		if (!TestNotNull(TEXT("van still under way"), Van)) { return false; }
		TestTrue(FString::Printf(TEXT("van %d never moved (speed %.3f)"), Van->Id, Van->Follower.Speed), Van->Follower.Speed < 1e-9);
		TestEqual(FString::Printf(TEXT("van %d is stopped where it stands"), Van->Id), Van->StopWithin, 0.0);
		TestEqual(FString::Printf(TEXT("van %d was refused on its first step"), Van->Id), Van->BlockedStep, 0);
	}

	UE_LOG(LogM2TrafficTest, Log, TEXT("BoxEntry measured: waits %d->%d, %d->%d, %d->%d"),
		V1, Vans[0]->WaitingOn, V2, Vans[1]->WaitingOn, V3, Vans[2]->WaitingOn);

	// The cycle, named: van 1 stands on A and wants B, which van 2 is standing on.
	TestEqual(TEXT("van 1 waits on van 2"), Vans[0]->WaitingOn, V2);
	TestEqual(TEXT("van 2 waits on van 3"), Vans[1]->WaitingOn, V3);
	TestEqual(TEXT("van 3 waits on van 1"), Vans[2]->WaitingOn, V1);

	// A second's worth of ticks changes nothing: this is a deadlock, and until Task 8 lands
	// nothing is entitled to resolve it. Anyone who moved has driven into a junction.
	M2TrafficRun(*Traffic, *Net, 1.0, [](int32) { return true; });
	for (const int32 Id : { V1, V2, V3 })
	{
		const FRoadAgent* Van = Traffic->FindAgent(Id);
		TestTrue(FString::Printf(TEXT("van %d still has not moved after 1 s (%.2f uu)"), Id, Van->Follower.Travelled),
			Van->Follower.Travelled < 1.0);
	}
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficBoxEntryFirstOnlyTest,
	"Airside.Model.Traffic.BoxEntryFirstOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficBoxEntryFirstOnlyTest::RunTest(const FString& Parameters)
{
	// THE ENTRY RULE APPLIES TO THE FIRST BOX THE WINDOW REACHES, NOT TO EVERY CONSECUTIVE
	// ONE - spec §3.1's rejected alternative, the one that "deadlocks HARDER". A 20 km
	// run-up, then three 400 uu boxes (400 < 500 + 300) end to end. The node at the end of
	// the SECOND box is held by a phantom occupant no agent owns.
	//
	// Chained through consecutive boxes, the rule stops the van a gap short of the SECOND
	// box's START - 20100 - while the first box's own end node is free and nothing is inside
	// the first box at all. Applied to the first box only, the van claims the first box's end
	// (free, granted) and stops against the held node itself, 20500, entering the first box
	// as it should. The two rules AGREE once the van is standing before the second box, which
	// is why the discriminating measurement below is taken while the van is still on the
	// run-up rather than at the end.
	//
	// THE PHANTOM SITS ON THE END OF THE SECOND BOX BECAUSE THAT BOX WAS CHOSEN, not because
	// it is the only one that discriminates - which is what this comment claimed until the
	// arithmetic was actually checked. On the THIRD box's end the two rules differ just as
	// plainly (20900 against the chained rule's answer), so that fixture would have worked
	// too. What makes THIS one discriminating is the pair of figures measured below: 20500
	// offered while the van is still on the run-up, where the chained rule offers 20100.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId N0 = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId N1 = M2TrafficNode(*Net, 20000.0, 0.0);
	const FGuidelineNodeId N2 = M2TrafficNode(*Net, 20400.0, 0.0);
	const FGuidelineNodeId N3 = M2TrafficNode(*Net, 20800.0, 0.0);
	const FGuidelineNodeId N4 = M2TrafficNode(*Net, 21200.0, 0.0);
	M2TrafficJoin(*Net, N0, N1, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, N1, N2, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, N2, N3, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, N3, N4, EGuidelineDir::AToB);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, N0, N4, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("dispatched"), Van > 0)) { return false; }

	// The phantom: agent 99 exists only in the table, so nothing ever releases it. STANDING
	// on the node rather than reserving it, so no rank can take it away.
	const int32 Phantom = 99;
	FTrafficClaim Sitting;
	Sitting.AgentId = Phantom;
	Sitting.Resource = FTrafficResource::OfNode(N3);
	Sitting.bOccupied = true;
	FTrafficClaim Blocker;
	Traffic->OccupancyForTest().TryClaim(Sitting, Blocker);

	// The stop point the arbiter offers while the van is still on the run-up, in route
	// distance. 20500 is the held node less the van's gap; 20100 would be the second box's
	// START less the gap, which is the chained rule's answer and 400 uu too early.
	double EarliestStopPointOffered = TNumericLimits<double>::Max();
	bool bBlockedBeforeTheBoxes = false;
	bool bStoppedBeforeTheBoxes = false;
	M2TrafficRun(*Traffic, *Net, 60.0, [&](int32)
	{
		const FRoadAgent* Agent = Traffic->FindAgent(Van);
		if (Agent == nullptr) { return false; }
		if (Agent->Follower.Travelled < 20000.0 && Agent->StopWithin < 1.0e9)
		{
			EarliestStopPointOffered = FMath::Min(EarliestStopPointOffered, Agent->Follower.Travelled + Agent->StopWithin);
			bBlockedBeforeTheBoxes = bBlockedBeforeTheBoxes || Agent->WaitingOn == Phantom;
		}
		if (Agent->Follower.Travelled > 100.0 && Agent->Follower.Travelled < 19999.0 && Agent->Follower.Speed < 1e-6)
		{
			bStoppedBeforeTheBoxes = true;
		}
		return true;
	});

	const FRoadAgent* Agent = Traffic->FindAgent(Van);
	UE_LOG(LogM2TrafficTest, Log,
		TEXT("BoxEntryFirstOnly measured: earliest stop point offered on the run-up %.0f uu ")
		TEXT("(the chained rule offers 20100), finished at %.0f uu, speed %.4f, waiting on %d, blocked step %d"),
		EarliestStopPointOffered, Agent->Follower.Travelled, Agent->Follower.Speed, Agent->WaitingOn, Agent->BlockedStep);

	if (!TestTrue(TEXT("the van WAS refused for the held node while still on the run-up (otherwise this measures nothing)"),
		bBlockedBeforeTheBoxes)) { return false; }

	// THE DISCRIMINATING ASSERTION.
	TestTrue(FString::Printf(
		TEXT("while before the first box the stop point offered is the HELD NODE less the gap (20500), ")
		TEXT("never a later box's start (20100): %.0f"), EarliestStopPointOffered),
		EarliestStopPointOffered >= 20499.0);

	TestFalse(TEXT("and it never came to rest before the first box"), bStoppedBeforeTheBoxes);
	TestTrue(FString::Printf(TEXT("it entered the first box (%.0f uu)"), Agent->Follower.Travelled),
		Agent->Follower.Travelled > 20000.0);
	TestTrue(FString::Printf(TEXT("and stopped a gap short of the box whose end is held (%.0f uu, want 20100)"), Agent->Follower.Travelled),
		Agent->Follower.Travelled <= 20101.0);
	TestTrue(TEXT("stopped"), Agent->Follower.Speed < 1e-6);
	TestEqual(TEXT("waiting on the phantom"), Agent->WaitingOn, Phantom);
	return true;
}

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
		Traffic->OccupancyForTest().TryClaim(Hold, Blocker);
	}
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, S, N, ETraversalClass::Aircraft), M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }

	// A BAR MUST NOT CLOSE THE RUNWAY FOR THE WHOLE TAXI. Sampled while the plane is still
	// more than one window (braking distance + gap, about 4500 uu here) short of the bar:
	// the claim is raised only once the window reaches the node, so before that nothing but
	// the phantom holds the strip. Excluding 99 asks "does anyone ELSE hold it".
	bool bRunwayFreeEarly = false;
	M2TrafficRun(*Traffic, *Net, 60.0, [&](int32)
	{
		const FRoadAgent* Q = Traffic->FindAgent(Plane);
		if (Q->Follower.Travelled < 10000.0
			&& !Traffic->GetOccupancy().IsHeld(FTrafficResource::OfSurface(RunwaySeg), 99))
		{
			bRunwayFreeEarly = true;
		}
		return Q->Follower.Speed > 1e-6 || Q->Follower.Travelled < 1.0;
	});
	const FRoadAgent* P = Traffic->FindAgent(Plane);
	TestTrue(TEXT("the bar did not close the runway for the whole taxi - free while a window short of it"),
		bRunwayFreeEarly);

	// MEASURED AND LOGGED, so a failure is read off the numbers rather than re-derived from
	// the assertion text. The bar is the end of step 0, at 17000 uu of route distance.
	UE_LOG(LogM2TrafficTest, Log,
		TEXT("HoldShort measured: centre %.1f uu, nose %.1f uu (bar 17000), speed %.4f, waiting on %d, blocked step %d"),
		P->Follower.Travelled, P->Follower.Travelled + Traffic->Rules.AircraftFootprint * 0.5,
		P->Follower.Speed, P->WaitingOn, P->BlockedStep);

	TestTrue(TEXT("stopped"), P->Follower.Speed < 1e-6);
	const double NoseAt = P->Follower.Travelled + Traffic->Rules.AircraftFootprint * 0.5;
	TestTrue(FString::Printf(TEXT("nose within 50 uu of the hold bar and not past it (nose %.0f, bar 17000)"), NoseAt),
		NoseAt <= 17000.0 + 1.0 && NoseAt >= 17000.0 - 50.0);
	TestEqual(TEXT("waiting on the runway's holder"), P->WaitingOn, 99);

	Traffic->OccupancyForTest().ReleaseAll(99);

	// THE BAR'S OWN CLAIM IS A RESERVATION, never an occupancy - nobody is occupied THROUGH
	// a bar, because an occupied claim cannot be preempted and a queue at the bar would then
	// lock the strip against the very landing the bar exists to protect. Sampled after the
	// phantom is gone and before the plane's centre reaches the bar, which is the only window
	// in which the plane holds the surface for the BAR's reason rather than the crossing's.
	bool bSawBarClaim = false;
	bool bBarClaimWasOccupied = false;
	M2TrafficRun(*Traffic, *Net, 5.0, [&](int32)
	{
		const FRoadAgent* Q = Traffic->FindAgent(Plane);
		if (Q->Follower.Travelled >= 16999.0)
		{
			return false;
		}
		for (const FTrafficClaim& Claim : Traffic->GetOccupancy().GetClaims())
		{
			if (Claim.AgentId == Plane && Claim.Resource == FTrafficResource::OfSurface(RunwaySeg))
			{
				bSawBarClaim = true;
				bBarClaimWasOccupied = bBarClaimWasOccupied || Claim.bOccupied;
			}
		}
		return true;
	});
	TestTrue(TEXT("the plane claims the strip at the bar"), bSawBarClaim);
	TestFalse(TEXT("and that claim is a RESERVATION: nobody is occupied through a bar"), bBarClaimWasOccupied);

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
		Traffic->OccupancyForTest().TryClaim(Hold, Blocker);
	}
	TestEqual(TEXT("refused"), Traffic->DispatchArrival(*Net, FVector2D(-1000.0, 0.0), UAirsideSettings::ResolveDefaultAirframe(), 1.0), 0);
	TestTrue(TEXT("with RunwayOccupied"), Refusals.Num() == 1 && Refusals[0] == EArrivalRefusal::RunwayOccupied);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficCrossingHoldsRunwayTest,
	"Airside.Model.Traffic.CrossingHoldsRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficCrossingHoldsRunwayTest::RunTest(const FString& Parameters)
{
	// SPEC §3.1'S FOURTH ROUTE. The HoldShort geometry with NOBODY holding the runway: the
	// plane is granted the bar and crosses, and the question is what happens AFTER the bar.
	// Before this rule the bar node left the window as the plane passed it and the surface
	// claim went with it, leaving an aeroplane standing on the centreline with the table
	// saying the strip was free - so a landing could be cleared onto it. The crossing edges
	// cannot cover the gap: here they are hand-built with no DerivedFrom, and in a DERIVED
	// graph a junction's turn paths carry none either, by design.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
	const FRoadSegmentId RunwaySeg = Net->AddStraightSegment(RA, RB, Runway);

	const FGuidelineNodeId S = M2TrafficNode(*Net, 0.0, -20000.0);
	const FGuidelineNodeId H = M2TrafficNode(*Net, 0.0, -3000.0);
	const FGuidelineNodeId X = M2TrafficNode(*Net, 0.0, 0.0);

	// A SECOND BAR ON THE FAR SIDE, protecting the SAME runway, because that is how a
	// crossing is actually painted - one bar each side - and it is the case that broke the
	// hold. The far bar's claim is a RESERVATION on a chain the crossing block is already
	// holding OCCUPIED, and TryClaim treats a same-agent claim on the same resource as an
	// update written over the old one wholesale: without the skip in ClaimAhead the
	// reservation replaced the occupancy, and a landing could then preempt an aeroplane
	// standing on the centreline.
	const FGuidelineNodeId Far = M2TrafficNode(*Net, 0.0, 3000.0);
	const FGuidelineNodeId N = M2TrafficNode(*Net, 0.0, 20000.0);
	M2TrafficJoin(*Net, S, H); M2TrafficJoin(*Net, H, X);
	M2TrafficJoin(*Net, X, Far); M2TrafficJoin(*Net, Far, N);
	Net->GetGuidelineNodeMutable(H)->HoldShortFor = RunwaySeg;
	Net->GetGuidelineNodeMutable(Far)->HoldShortFor = RunwaySeg;

	// Route distances: the near bar at 17000, the centreline crossing X at 20000, the far
	// bar at 23000, the far node N at 40000. The strip's half width is 2250, so a tail clear
	// of it is at 22250 - and the REJECTED "release at the next node" rule would hold on
	// until 40000.
	//
	// THE FAR LEG RUNS STRAIGHT FROM THE BAR TO N - 17000 uu of it - AND THAT IS THE POINT.
	// This fixture used to carry an extra node 500 uu past the far bar, because leaving a bar
	// RE-ARMED the crossing: the rule read "the node this step left carries a bar" and could
	// not tell a strip being entered from one being left, so the release was suppressed for
	// the whole of that step and the runway stayed held for 17000 uu behind an aeroplane
	// already clear of it (measured at 25025 uu, i.e. never inside the run). Spec §3.1's
	// "Refined 2026-09-06 during Task 7" arms the hold only when the step leaving the bar
	// leads ONTO the strip, so the exit bar arms nothing and the extra node is not needed -
	// which is what the long leg here measures.
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Airframe = M2TrafficPlane();
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, S, N, ETraversalClass::Aircraft), Airframe, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }

	const FTrafficResource Strip = FTrafficResource::OfSurface(RunwaySeg);
	bool bFreeBeforeTheBar = false;
	bool bHeldWhileCrossing = false;
	bool bLandingRefusedWhileCrossing = false;
	double HoldBegan = -1.0;
	double HoldEnded = -1.0;
	int32 ReservedOnTheStripTicks = 0;
	double FirstReservedAt = -1.0;
	bool bWasCrossing = false;
	double CrossingEnded = -1.0;

	/** Set if anything re-arms the crossing after the FAR bar - the exit-side defect. */
	bool bCrossingPastTheFarBar = false;

	M2TrafficRun(*Traffic, *Net, 120.0, [&](int32)
	{
		const FRoadAgent* Q = Traffic->FindAgent(Plane);
		if (Q == nullptr) { return false; }
		const double Travelled = Q->Follower.Travelled;
		const bool bHeld = Traffic->GetOccupancy().IsHeld(Strip, 0);

		if (Travelled < 10000.0 && !bHeld) { bFreeBeforeTheBar = true; }
		if (bHeld && HoldBegan < 0.0) { HoldBegan = Travelled; }
		if (!bHeld && HoldBegan >= 0.0 && HoldEnded < 0.0) { HoldEnded = Travelled; }

		// THE CROSSING ITSELF, separately from the table. With a bar on each side the strip
		// is HELD continuously from the near bar's reservation to the far bar's, so IsHeld
		// alone can no longer say when the crossing ended - and when the crossing ends is
		// the rule under test. Measured off the agent's own field, which is what the
		// geometric release actually clears.
		if (Q->CrossingRunway.IsSet()) { bWasCrossing = true; }
		else if (bWasCrossing && CrossingEnded < 0.0) { CrossingEnded = Travelled; }

		// THE EXIT BAR ARMS NOTHING. Past the far bar at 23000 the aeroplane is leaving the
		// strip, and a rule that cannot tell that from entering it re-arms the hold here and
		// keeps the runway shut for the whole 17000 uu leg to N.
		if (Travelled > 23100.0 && Q->CrossingRunway.IsSet()) { bCrossingPastTheFarBar = true; }

		// ON THE STRIP: centre past the bar, not yet at the far side. A landing offered now
		// must be refused, which is the whole point of the rule.
		if (Travelled > 17500.0 && Travelled < 19500.0)
		{
			bHeldWhileCrossing = bHeldWhileCrossing || bHeld;
			bLandingRefusedWhileCrossing = bLandingRefusedWhileCrossing
				|| ArrivalPlanner::Plan(*Net, FVector2D(-60000.0, 0.0), Airframe, &Traffic->GetOccupancy()).Why
					== EArrivalRefusal::RunwayOccupied;
		}

		// WHILE THE PLANE IS ON THE STRIP - past the centreline at 20000, tail not yet clear
		// at 22250 - its claim on the chain must be OCCUPIED, the one state nothing can
		// preempt (spec §3.3). The far bar at 23000 is inside the window throughout this
		// span and asks for the same chain as a RESERVATION; before the skip in ClaimAhead
		// that reservation was written over the occupancy, and a landing could then be
		// cleared onto an aeroplane standing on the centreline. Read off GetClaims and not
		// IsHeld, because IsHeld cannot tell an occupancy from a reservation and that is
		// precisely the distinction being measured.
		if (Travelled > 20500.0 && Travelled < 22000.0)
		{
			for (const FTrafficClaim& Claim : Traffic->GetOccupancy().GetClaims())
			{
				if (Claim.AgentId == Plane && Claim.Resource == Strip && !Claim.bOccupied)
				{
					++ReservedOnTheStripTicks;
					if (FirstReservedAt < 0.0) { FirstReservedAt = Travelled; }
				}
			}
		}

		// Well past the tail's clearance of the FAR BAR (23500, itself 3000 uu clear of the
		// strip) and far short of the next route node (40000), so the two candidate release
		// rules cannot both pass the assertions below.
		return Travelled < 25000.0;
	});

	const FRoadAgent* P = Traffic->FindAgent(Plane);
	UE_LOG(LogM2TrafficTest, Log,
		TEXT("CrossingHoldsRunway measured: hold began at route %.0f uu, ended at %.0f uu ")
		TEXT("(near bar 17000, centreline 20000, tail clear 22250, far bar 23000, next node 40000); ")
		TEXT("stopped at %.0f; crossing ended at %.0f; reserved-not-occupied on the strip for ")
		TEXT("%d ticks, first at %.0f"),
		HoldBegan, HoldEnded, P->Follower.Travelled, CrossingEnded, ReservedOnTheStripTicks, FirstReservedAt);

	TestTrue(TEXT("the bar did not close the runway for the whole taxi"), bFreeBeforeTheBar);
	TestTrue(FString::Printf(TEXT("the hold began before the bar, as the bar's reservation (%.0f)"), HoldBegan),
		HoldBegan > 0.0 && HoldBegan < 17000.0);
	TestTrue(TEXT("the strip is HELD while the plane is on the centreline"), bHeldWhileCrossing);
	TestTrue(TEXT("so a landing offered mid-crossing is refused RunwayOccupied"), bLandingRefusedWhileCrossing);
	TestEqual(FString::Printf(TEXT("the far bar's RESERVATION never overwrote the crossing's OCCUPANCY (%d ticks, first at %.0f)"),
		ReservedOnTheStripTicks, FirstReservedAt), ReservedOnTheStripTicks, 0);

	// THE DISCRIMINATING ASSERTION. "Release at the next route node" would hold to 40000.
	// Read off the CROSSING and not the table, because with a bar on each side the table is
	// held continuously across the whole crossing - the near bar hands over to the crossing
	// and the crossing to the far bar, which is the correct answer and an uninformative one.
	//
	// THE BOUND IS THE GEOMETRY, not a round number: the centreline node is at 20000 and the
	// tail is clear once it is a chain half width (2250) past it, so the CENTRE is clear at
	// 22250 plus half a footprint (500), and 100 uu of slack covers the 50 uu tick.
	const double ClearBy = 20000.0 + 2250.0 + Traffic->Rules.AircraftFootprint * 0.5 + 100.0;
	TestTrue(FString::Printf(TEXT("the crossing ends once the TAIL is clear of the strip (by %.0f), not at the next node (%.0f)"), ClearBy, CrossingEnded),
		CrossingEnded > 20000.0 && CrossingEnded <= ClearBy);
	TestFalse(TEXT("and the bar on the way OUT arms nothing: the crossing is not re-armed past the far bar"),
		bCrossingPastTheFarBar);
	TestTrue(FString::Printf(TEXT("and the hold ends with it, not at the next node 17000 uu away (%.0f)"), HoldEnded),
		HoldEnded > 22000.0 && HoldEnded < 25000.0);
	TestFalse(TEXT("and the strip is free afterwards"), Traffic->GetOccupancy().IsHeld(Strip, 0));
	TestFalse(TEXT("with nothing left naming a crossing"), P->CrossingRunway.IsSet());
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRunwayEdgeClaimTest,
	"Airside.Model.Traffic.RunwayEdgeClaim",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRunwayEdgeClaimTest::RunTest(const FString& Parameters)
{
	// SPEC §3.1'S FIRST ROUTE, and the CHAIN part of it. The runway is split at a road node
	// into two segments; the guideline edge B->C names only the FAR one, and the phantom
	// holds the NEAR one. An implementation that claimed only DerivedFrom would sail past.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RM = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
	const FRoadSegmentId Near = Net->AddStraightSegment(RA, RM, Runway);
	const FRoadSegmentId Far = Net->AddStraightSegment(RM, RB, Runway);
	if (!TestEqual(TEXT("the split runway is a two-segment chain"), Net->RunwayChain(Far).Num(), 2))
	{
		return false;
	}

	const FGuidelineNodeId A = M2TrafficNode(*Net, 0.0, -20000.0);
	const FGuidelineNodeId B = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId C = M2TrafficNode(*Net, 0.0, 20000.0);
	M2TrafficJoin(*Net, A, B);
	{
		// Hand-built rather than through M2TrafficJoin, because DerivedFrom is the whole
		// point of this fixture and the helper does not set it.
		FGuidelineEdge Edge;
		Edge.A = B; Edge.B = C;
		Edge.Control = (Net->GetGuidelineNode(B)->Position + Net->GetGuidelineNode(C)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.DerivedFrom = Far;
		Net->AddGuidelineEdge(MoveTemp(Edge));
	}

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	{
		FTrafficClaim Hold; Hold.AgentId = 99; Hold.Resource = FTrafficResource::OfSurface(Near); Hold.bOccupied = true;
		FTrafficClaim Blocker;
		Traffic->OccupancyForTest().TryClaim(Hold, Blocker);
	}
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::Aircraft), M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }

	M2TrafficRun(*Traffic, *Net, 60.0, [&](int32)
	{
		const FRoadAgent* Q = Traffic->FindAgent(Plane);
		return Q->Follower.Speed > 1e-6 || Q->Follower.Travelled < 1.0;
	});

	const FRoadAgent* P = Traffic->FindAgent(Plane);
	UE_LOG(LogM2TrafficTest, Log,
		TEXT("RunwayEdgeClaim measured: stopped at route %.0f uu (want 18500 = the runway edge's ")
		TEXT("start 20000 less the gap 1500), waiting on %d, blocked step %d"),
		P->Follower.Travelled, P->WaitingOn, P->BlockedStep);

	TestTrue(TEXT("stopped"), P->Follower.Speed < 1e-6);
	TestTrue(FString::Printf(TEXT("a gap short of where the runway edge BEGINS, not inside it (%.0f, want 18500)"),
		P->Follower.Travelled),
		FMath::Abs(P->Follower.Travelled - 18500.0) < 50.0);
	TestEqual(TEXT("blocked on the step whose edge lies on the runway"), P->BlockedStep, 1);
	TestEqual(TEXT("waiting on the holder of the OTHER segment of the same chain"), P->WaitingOn, 99);

	Traffic->OccupancyForTest().ReleaseAll(99);
	bool bPlaneHeldTheChain = false;
	bool bSomebodyElseHeldIt = false;
	M2TrafficRun(*Traffic, *Net, 180.0, [&](int32)
	{
		const FRoadAgent* Q = Traffic->FindAgent(Plane);
		if (Q == nullptr) { return false; }
		if (Q->Follower.Travelled > 21000.0 && Q->Follower.Travelled < 39000.0)
		{
			bPlaneHeldTheChain = bPlaneHeldTheChain || Traffic->GetOccupancy().IsHeld(FTrafficResource::OfSurface(Near), 0);
			bSomebodyElseHeldIt = bSomebodyElseHeldIt || Traffic->GetOccupancy().IsHeld(FTrafficResource::OfSurface(Near), Plane);
		}
		return Q->Phase != EAgentPhase::Parked;
	});

	TestTrue(TEXT("released, the plane holds the whole chain while it is on the runway edge"), bPlaneHeldTheChain);
	TestFalse(TEXT("and nobody else does"), bSomebodyElseHeldIt);
	TestEqual(TEXT("and it reaches its goal"), Traffic->FindAgent(Plane)->Phase, EAgentPhase::Parked);
	return true;
}

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
	const double TravelledBefore = V->Follower.Travelled;
	TestTrue(TEXT("moving, mid-edge"), SpeedBefore > 100.0 && V->Follower.Travelled < 20000.0);

	TestTrue(TEXT("replan accepted"), Traffic->ReplanAt(Van, *Net, 1, BC));
	V = Traffic->FindAgent(Van);

	// THE REPLAN IS NOT A DISPATCH. Start() would put the van back at the polyline's first
	// point at rest; Replace keeps Travelled and Speed, and the line up to the splice is the
	// one it was already driving - so these three are the whole of "no jump" at the instant
	// of the swap, and MaxJump below is the same claim measured over the rest of the drive.
	TestEqual(TEXT("speed kept"), V->Follower.Speed, SpeedBefore, 1e-9);
	TestEqual(TEXT("route distance kept"), V->Follower.Travelled, TravelledBefore, 1e-9);
	TestTrue(FString::Printf(TEXT("and the van did not move (%.3f uu)"), FVector2D::Distance(V->LastMotion.Position, Before)),
		FVector2D::Distance(V->LastMotion.Position, Before) < 1e-6);
	TestEqual(TEXT("three steps now: A->B, B->X, X->C"), V->Follower.Plan.Steps.Num(), 3);
	TestEqual(TEXT("the goal is unchanged"), V->GoalNode, C);

	// A SPLICE STEP OFF THE END OF THE PLAN, refused with the agent still under way - the
	// guard that fires before StepFromNode would index past Steps.
	TestFalse(TEXT("a splice step off the end of the plan refused"), Traffic->ReplanAt(Van, *Net, 99, FGuidelineEdgeId()));
	TestEqual(TEXT("and the three-step plan survived it"), Traffic->FindAgent(Van)->Follower.Plan.Steps.Num(), 3);

	double MaxJump = 0.0; FVector2D Last = Before; double MaxY = 0.0;
	M2TrafficRun(*Traffic, *Net, 200.0, [&](int32)
	{
		const FRoadAgent* Now = Traffic->FindAgent(Van);
		MaxJump = FMath::Max(MaxJump, FVector2D::Distance(Now->LastMotion.Position, Last));
		Last = Now->LastMotion.Position;
		MaxY = FMath::Max(MaxY, Now->LastMotion.Position.Y);
		return Now->Phase != EAgentPhase::Parked;
	});

	// MEASURED, not asserted: a teleport is what a splice that re-based EndDistance wrongly
	// would look like, and the number says how near the bound the drive actually ran.
	UE_LOG(LogM2TrafficTest, Log,
		TEXT("Replan measured: max per-tick step %.1f uu (bound 51 = cruise 1000 uu/s x 0.05 s + 1), max Y %.0f"),
		MaxJump, MaxY);

	TestTrue(FString::Printf(TEXT("no tick moved it more than one frame at cruise (%.1f uu)"), MaxJump), MaxJump <= 1000.0 * 0.05 + 1.0);
	TestTrue(FString::Printf(TEXT("it went via X (max Y %.0f)"), MaxY), MaxY > 14000.0);
	TestEqual(TEXT("and arrived"), Traffic->FindAgent(Van)->Phase, EAgentPhase::Parked);

	// REFUSALS CHANGE NOTHING. Each of these fails at a different guard, and an agent left
	// half-replanned by one of them is a plan spliced onto a follower that never got it.
	const FRoutePlan Was = Traffic->FindAgent(Van)->Follower.Plan;
	TestFalse(TEXT("unknown agent refused"), Traffic->ReplanAt(Van + 500, *Net, 1, FGuidelineEdgeId()));
	TestFalse(TEXT("a parked agent refused"), Traffic->ReplanAt(Van, *Net, 1, FGuidelineEdgeId()));
	TestEqual(TEXT("and the plan it had is the plan it still has"),
		Traffic->FindAgent(Van)->Follower.Plan.Steps.Num(), Was.Steps.Num());
	TestEqual(TEXT("with the same length"), Traffic->FindAgent(Van)->Follower.Plan.Length, Was.Length, 1e-9);

	// THE SEARCH-FAILURE PATH, on its own fixture because it needs a graph with no way
	// round: one edge P->Q, a van under way on it, and that edge banned. The search comes
	// back Unreachable, and the agent must be left driving what it had - the case a deadlock
	// resolver hits every time it bans the only line out of a dead end.
	URoadNetwork* Dead = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId P = M2TrafficNode(*Dead, 0.0, 0.0);
	const FGuidelineNodeId Qn = M2TrafficNode(*Dead, 20000.0, 0.0);
	const FGuidelineEdgeId PQ = M2TrafficJoin(*Dead, P, Qn);

	UGroundTraffic* Only = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Stuck = Only->DispatchAgent(Dead, M2TrafficRoute(*Dead, P, Qn, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	M2TrafficRun(*Only, *Dead, 5.0, [](int32) { return true; });
	const double StuckAt = Only->FindAgent(Stuck)->Follower.Travelled;
	const double StuckSpeed = Only->FindAgent(Stuck)->Follower.Speed;
	TestTrue(TEXT("the dead-end van is under way"), StuckSpeed > 100.0);

	TestFalse(TEXT("no route survives the ban: refused"), Only->ReplanAt(Stuck, *Dead, 0, PQ));
	TestEqual(TEXT("and it is still on the one edge it had"), Only->FindAgent(Stuck)->Follower.Plan.Steps.Num(), 1);
	TestEqual(TEXT("at the same distance"), Only->FindAgent(Stuck)->Follower.Travelled, StuckAt, 1e-9);
	TestEqual(TEXT("at the same speed"), Only->FindAgent(Stuck)->Follower.Speed, StuckSpeed, 1e-9);
	return true;
}

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
	//
	// THE SAME GEOMETRY AS Airside.Model.Traffic.BoxEntry, deliberately: that test pins that
	// the gridlock FORMS with its members at nodes where they can turn, which is the
	// precondition this one's resolver depends on. One fixture, two halves of one claim.
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
	if (!TestTrue(TEXT("all three routed and dispatched"), V1 > 0 && V2 > 0 && V3 > 0)) { return false; }
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

	UE_LOG(LogM2TrafficTest, Log,
		TEXT("DeadlockTriangle measured: resolved at tick %d of the second run (bound %d), agent %d replanned, ")
		TEXT("%d cycle(s) detected, max per-tick step %.1f uu"),
		ResolvedAtTick, static_cast<int32>(Traffic->Rules.StallSeconds / 0.05) + 2,
		Traffic->GetLastResolvedAgentForTest(), Traffic->GetCyclesDetectedForTest(), MaxJump);

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

#endif
