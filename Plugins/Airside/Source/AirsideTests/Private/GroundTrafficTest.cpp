#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "AirsideTestsLog.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeparturePlanner.h"
#include "Model/LandingRun.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/RunwayQuery.h"
#include "Model/TrafficClaims.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// #312: was a hand-built FRouteQuery that skipped AvoidRunways - see TestGraph::Probe's
	// own comment for why that silently answered every errand with the permissive policy.
	FRoutePlan M2TrafficRoute(const URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B, ETraversalClass Class)
	{
		return TestGraph::Probe(Net, A, B, Class);
	}

	/** The two named nodes a rebuilt A-B-C(-D) test graph keeps handles to. */
	struct FRebuiltGraph
	{
		FGuidelineNodeId A;
		FGuidelineNodeId C;
	};
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
	const FGuidelineNodeId W = TestGraph::Node(*Net, -20000.0, 0.0);
	const FGuidelineNodeId E = TestGraph::Node(*Net, 20000.0, 0.0);
	const FGuidelineNodeId S = TestGraph::Node(*Net, 0.0, -20000.0);
	const FGuidelineNodeId N = TestGraph::Node(*Net, 0.0, 20000.0);
	const FGuidelineNodeId J = TestGraph::Node(*Net, 0.0, 0.0);
	TestGraph::Join(*Net, W, J); TestGraph::Join(*Net, J, E);
	TestGraph::Join(*Net, S, J); TestGraph::Join(*Net, J, N);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, W, E, ETraversalClass::Aircraft), TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, S, N, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("both dispatched"), Plane > 0 && Van > 0)) { return false; }

	double VanMinStopWithin = TNumericLimits<double>::Max();
	double PlaneMinStopWithin = TNumericLimits<double>::Max();
	double MinSeparation = TNumericLimits<double>::Max();
	double VanMinSpeedWhileWaiting = TNumericLimits<double>::Max();
	int32 VanBlockedTicks = 0;
	bool bVanWaitedOnPlane = false;
	const int32 Ticks = TickUntil(*Traffic, *Net, 120.0, [&](int32)
	{
		const FRoadAgent* P = Traffic->FindAgent(Plane);
		const FRoadAgent* V = Traffic->FindAgent(Van);
		if (P == nullptr || V == nullptr) { return false; }
		if (P->Phase == EAgentPhase::Taxiing) { PlaneMinStopWithin = FMath::Min(PlaneMinStopWithin, P->GetStopWithin()); }
		if (V->Phase == EAgentPhase::Taxiing) { VanMinStopWithin = FMath::Min(VanMinStopWithin, V->GetStopWithin()); }
		if (V->GetWaitingOn() == Plane)
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
	UE_LOG(LogAirsideTests, Log,
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
	// FOUND BEFORE THEY ARE READ. A run loop can end with an agent removed - a phase change
	// to Gone drops it - and every line below dereferences the result, so an unlucky failure
	// here would be a CRASHED test rather than a failed one, which the runner has to diff the
	// log to notice at all.
	const FRoadAgent* PlaneNow = Traffic->FindAgent(Plane);
	const FRoadAgent* VanNow = Traffic->FindAgent(Van);
	if (!TestNotNull(TEXT("the aircraft is still there to be asked about"), PlaneNow)
		|| !TestNotNull(TEXT("and the van"), VanNow))
	{
		return false;
	}

	TestTrue(TEXT("the van yielded: its speed fell below half its taxi cap while blocked"),
		VanMinSpeedWhileWaiting < 0.5 * VanNow->Chassis().Ground.Taxi.SpeedCap);
	TestTrue(TEXT("the aircraft never was"), PlaneMinStopWithin > 1000.0);
	TestTrue(TEXT("the van's wait named the aircraft"), bVanWaitedOnPlane);
	// SEPARATION AGAINST WHAT THE RULE ACTUALLY RESERVES, which is the gap plus half the
	// BLOCKER's footprint - not, as this line read until 2026-09-15, the van's own length.
	// That was a coincidental yardstick: it happened to sit just under the separation this
	// geometry produces, so it passed at a 620 uu van and failed at 850 by six centimetres,
	// while the arbiter's behaviour had not changed at all (the van still yielded, still
	// named the aircraft, still came down from 1000 to 250 uu/s).
	//
	// A CENTRE-TO-CENTRE DISTANCE IS A PROXY HERE AND ALWAYS WAS. The two cross at ninety
	// degrees, so the van passes behind the tail and their centres are legitimately closer
	// than their combined half-lengths. The real safety property - the yielder stayed off the
	// node while the blocker held it - is what Airside.Model.Traffic.TruckCrossesTaxiway
	// measures directly. This assertion's job is only to catch a van that drove through.
	const double Reserved = Traffic->Rules.GapFor(ETraversalClass::GroundVehicle) + Traffic->Rules.AircraftFootprint * 0.5;
	TestTrue(
		FString::Printf(
			TEXT("never closer than the %.0f uu the crossing rule reserves (measured %.0f)"),
			Reserved, MinSeparation),
		MinSeparation >= Reserved);
	TestEqual(TEXT("both arrive"), PlaneNow->Phase, EAgentPhase::Parked);
	TestEqual(TEXT("both arrive (van)"), VanNow->Phase, EAgentPhase::Parked);
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
	const FGuidelineNodeId W = TestGraph::Node(*Net, -20000.0, 0.0);
	const FGuidelineNodeId E = TestGraph::Node(*Net, 20000.0, 0.0);
	const FGuidelineNodeId S = TestGraph::Node(*Net, 0.0, -20000.0);
	const FGuidelineNodeId N = TestGraph::Node(*Net, 0.0, 20000.0);
	const FGuidelineNodeId J = TestGraph::Node(*Net, 0.0, 0.0);
	TestGraph::Join(*Net, W, J); TestGraph::Join(*Net, J, E);
	TestGraph::Join(*Net, S, J); TestGraph::Join(*Net, J, N);
	// FRoadNetworkTestAccess (#191): no production caller writes PriorityOverride yet - see
	// FGuidelineNode's own comment - so this goes through the one friend struct that may,
	// not the raw GetGuidelineNodeMutable that used to be public for everyone.
	FRoadNetworkTestAccess(*Net).SetGuidelineNodePriorityOverrideForTest(J,
		{ ETraversalClass::GroundVehicle, ETraversalClass::Aircraft });

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, W, E, ETraversalClass::Aircraft), TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, S, N, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);

	bool bPlaneWaitedOnVan = false;
	bool bVanWaitedOnPlane = false;
	TickUntil(*Traffic, *Net, 120.0, [&](int32)
	{
		const FRoadAgent* P = Traffic->FindAgent(Plane);
		const FRoadAgent* V = Traffic->FindAgent(Van);
		if (P == nullptr || V == nullptr) { return false; }
		bPlaneWaitedOnVan |= (P->GetWaitingOn() == Van);
		bVanWaitedOnPlane |= (V->GetWaitingOn() == Plane);
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
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 60000.0, 0.0);
	TestGraph::Join(*Net, A, B);
	const FRoutePlan Plan = M2TrafficRoute(*Net, A, B, ETraversalClass::Aircraft);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	FAirframe Leader = TestAirframes::GroundOnly();
	Leader.Chassis.Ground.Taxi.SpeedCap = 600.0;   // slower, so the follower catches it
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
	const int32 Ticks = TickUntil(*Traffic, *Net, 200.0, [&](int32 Tick)
	{
		if (Tick == 40) { Follow = Traffic->DispatchAgent(Net, Plan, TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0); }
		const FRoadAgent* L = Traffic->FindAgent(Lead);
		const FRoadAgent* Fo = Follow > 0 ? Traffic->FindAgent(Follow) : nullptr;
		if (L == nullptr || Fo == nullptr) { return L != nullptr; }
		if (L->Phase == EAgentPhase::Taxiing && Fo->Phase == EAgentPhase::Taxiing)
		{
			const double Gap = L->Follower.Travelled - Fo->Follower.Travelled;
			MinGap = FMath::Min(MinGap, Gap);
			if (Fo->Follower.Travelled > 0.0) { MinGapUnderWay = FMath::Min(MinGapUnderWay, Gap); }
			if (Fo->GetWaitingOn() == Lead) { bFollowerCaughtUp = true; }
		}
		return !(L->Phase == EAgentPhase::Parked && Fo->Phase == EAgentPhase::Parked);
	});

	UE_LOG(LogAirsideTests, Log,
		TEXT("CarFollowing measured: %d ticks, min centre-to-centre gap %.0f uu (from dispatch), ")
		TEXT("%.0f uu once the follower was under way, floor %.0f uu, caught up %d"),
		Ticks, MinGap, MinGapUnderWay,
		Traffic->Rules.AircraftFootprint * 0.5 + Traffic->Rules.GapFor(ETraversalClass::Aircraft) - 50.0,
		bFollowerCaughtUp ? 1 : 0);

	TestTrue(TEXT("the follower did catch the leader (otherwise this measures nothing)"), bFollowerCaughtUp);
	// The follower's CENTRE stops Gap behind the leader's TAIL (leader centre - Footprint/2),
	// so centre-to-centre is Footprint/2 + Gap: nose to tail is exactly the gap.
	TestTrue(FString::Printf(TEXT("centre-to-centre never below footprint/2 + gap once under way (%.0f)"), MinGapUnderWay),
		MinGapUnderWay >= Traffic->Rules.AircraftFootprint * 0.5 + Traffic->Rules.GapFor(ETraversalClass::Aircraft) - 50.0);
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
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 40000.0, 0.0);
	TestGraph::Join(*Net, A, B);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 P1 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, B, ETraversalClass::Aircraft), TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	const int32 P2 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, B, A, ETraversalClass::Aircraft), TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	double MinSeparation = TNumericLimits<double>::Max();
	TickUntil(*Traffic, *Net, 120.0, [&](int32)
	{
		const FRoadAgent* X = Traffic->FindAgent(P1);
		const FRoadAgent* Y = Traffic->FindAgent(P2);
		MinSeparation = FMath::Min(MinSeparation, FVector2D::Distance(X->LastMotion.Position, Y->LastMotion.Position));
		return true;
	});
	const FRoadAgent* X = Traffic->FindAgent(P1);
	const FRoadAgent* Y = Traffic->FindAgent(P2);

	UE_LOG(LogAirsideTests, Log,
		TEXT("HeadOnStops measured: min separation %.0f uu, travelled %.0f / %.0f, speed %.4f / %.4f, waiting on %d / %d"),
		MinSeparation, X->Follower.Travelled, Y->Follower.Travelled, X->Follower.Speed, Y->Follower.Speed,
		X->GetWaitingOn(), Y->GetWaitingOn());

	TestTrue(TEXT("both are stopped"), X->Follower.Speed < 1e-6 && Y->Follower.Speed < 1e-6);
	TestTrue(TEXT("both are still taxiing, not parked"), X->Phase == EAgentPhase::Taxiing && Y->Phase == EAgentPhase::Taxiing);
	TestTrue(TEXT("each waits on the other"), X->GetWaitingOn() == P2 && Y->GetWaitingOn() == P1);
	TestTrue(FString::Printf(TEXT("never closer than one footprint (%.0f)"), MinSeparation), MinSeparation >= Traffic->Rules.AircraftFootprint - 1.0);

	// AND THE DEADLOCK PASS SEES IT, ONCE. Two aircraft nose to nose on one edge is a
	// two-member cycle, so the detector must find it - but neither is AT the node where its
	// refused edge begins (both are 19 km along it), so neither can turn without reversing,
	// which spec §1 rules out of M2. So it is logged and retried rather than resolved.
	//
	// TWO DIFFERENT MECHANISMS, MEASURED SEPARATELY, because an earlier comment here credited
	// one with the other's job. The COUNT of cycles is one because a cycle is keyed by its
	// lowest member id - re-detecting the same ring is not a new cycle whatever the clock
	// says. The number of LOG LINES is what the LastResolveAttempt stamp governs: without it
	// this ring would report on all ~1900 remaining ticks; with it, once per RetrySeconds.
	TestEqual(TEXT("the all-aircraft cycle was detected once"), Traffic->GetCyclesDetectedForTest(), 1);
	TestEqual(TEXT("nobody could turn, so nobody replanned"), Traffic->GetLastResolvedAgentForTest(), 0);

	// The first report lands as the stall passes StallSeconds and every RetrySeconds after
	// it, so the count is that many windows plus the first. +/-1 because the fire instant is
	// quantised to the 0.05 s tick and the last window may not have closed by the end of the
	// run - not because the cadence is approximate.
	const double StalledFor = FMath::Max(X->GetStalledSeconds(), Y->GetStalledSeconds());
	const double SinceFirst = FMath::Max(0.0, StalledFor - Traffic->Rules.StallSeconds);
	const int32 Expected = FMath::FloorToInt32(SinceFirst / Traffic->Rules.RetrySeconds) + 1;
	UE_LOG(LogAirsideTests, Log,
		TEXT("HeadOnStops cadence measured: stalled %.2f s, %.2f s since the first report, ")
		TEXT("%d line(s) at one per %.0f s, expected %d"),
		StalledFor, SinceFirst, Traffic->GetDeadlockLogLinesForTest(), Traffic->Rules.RetrySeconds, Expected);
	TestTrue(FString::Printf(TEXT("one report per retry window, not per tick (%d against %d)"),
		Traffic->GetDeadlockLogLinesForTest(), Expected),
		FMath::Abs(Traffic->GetDeadlockLogLinesForTest() - Expected) <= 1);
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
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 600.0, 0.0);
	const FGuidelineNodeId C = TestGraph::Node(*Net, 300.0, 519.6);
	TestGraph::Join(*Net, A, B, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, B, C, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, C, A, { EGuidelineDir::AToB });

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 V1 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V2 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, B, A, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V3 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, C, B, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
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
		TestEqual(FString::Printf(TEXT("van %d is stopped where it stands"), Van->Id), Van->GetStopWithin(), 0.0);
		TestEqual(FString::Printf(TEXT("van %d was refused on its first step"), Van->Id), Van->GetBlockedStep(), 0);
	}

	UE_LOG(LogAirsideTests, Log, TEXT("BoxEntry measured: waits %d->%d, %d->%d, %d->%d"),
		V1, Vans[0]->GetWaitingOn(), V2, Vans[1]->GetWaitingOn(), V3, Vans[2]->GetWaitingOn());

	// The cycle, named: van 1 stands on A and wants B, which van 2 is standing on.
	TestEqual(TEXT("van 1 waits on van 2"), Vans[0]->GetWaitingOn(), V2);
	TestEqual(TEXT("van 2 waits on van 3"), Vans[1]->GetWaitingOn(), V3);
	TestEqual(TEXT("van 3 waits on van 1"), Vans[2]->GetWaitingOn(), V1);

	// A second's worth of ticks changes nothing: this is a deadlock, and until Task 8 lands
	// nothing is entitled to resolve it. Anyone who moved has driven into a junction.
	TickUntil(*Traffic, *Net, 1.0, [](int32) { return true; });
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
	const FGuidelineNodeId N0 = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId N1 = TestGraph::Node(*Net, 20000.0, 0.0);
	const FGuidelineNodeId N2 = TestGraph::Node(*Net, 20400.0, 0.0);
	const FGuidelineNodeId N3 = TestGraph::Node(*Net, 20800.0, 0.0);
	const FGuidelineNodeId N4 = TestGraph::Node(*Net, 21200.0, 0.0);
	TestGraph::Join(*Net, N0, N1, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, N1, N2, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, N2, N3, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, N3, N4, { EGuidelineDir::AToB });

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, N0, N4, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
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
	//
	// THE GAP IS ASKED OF THE RULES, NOT TYPED (#455 item 6): those two figures were written for a
	// VehicleGap of 300, and GapFor floors a vehicle's gap at half its footprint (334.75 + 1) so that a
	// refused vehicle stops outside the zone where its own claim is an occupancy. The 400 uu that separates
	// the two rules is the point of the test and does not move; the 36 uu the gap grew by does, so both are
	// measured against GapFor. The default figures below are what they were at a gap of 300, for a reader
	// who has the old log.
	const double VanGap = Traffic->Rules.GapFor(ETraversalClass::GroundVehicle);
	const double HeldNodeAt = 20800.0;
	double EarliestStopPointOffered = TNumericLimits<double>::Max();
	bool bBlockedBeforeTheBoxes = false;
	bool bStoppedBeforeTheBoxes = false;
	TickUntil(*Traffic, *Net, 60.0, [&](int32)
	{
		const FRoadAgent* Agent = Traffic->FindAgent(Van);
		if (Agent == nullptr) { return false; }
		if (Agent->Follower.Travelled < 20000.0 && Agent->GetStopWithin() < 1.0e9)
		{
			EarliestStopPointOffered = FMath::Min(EarliestStopPointOffered, Agent->Follower.Travelled + Agent->GetStopWithin());
			bBlockedBeforeTheBoxes = bBlockedBeforeTheBoxes || Agent->GetWaitingOn() == Phantom;
		}
		if (Agent->Follower.Travelled > 100.0 && Agent->Follower.Travelled < 19999.0 && Agent->Follower.Speed < 1e-6)
		{
			bStoppedBeforeTheBoxes = true;
		}
		return true;
	});

	const FRoadAgent* Agent = Traffic->FindAgent(Van);
	UE_LOG(LogAirsideTests, Log,
		TEXT("BoxEntryFirstOnly measured: earliest stop point offered on the run-up %.0f uu ")
		TEXT("(the chained rule offers 20100), finished at %.0f uu, speed %.4f, waiting on %d, blocked step %d"),
		EarliestStopPointOffered, Agent->Follower.Travelled, Agent->Follower.Speed, Agent->GetWaitingOn(), Agent->GetBlockedStep());

	if (!TestTrue(TEXT("the van WAS refused for the held node while still on the run-up (otherwise this measures nothing)"),
		bBlockedBeforeTheBoxes)) { return false; }

	// THE DISCRIMINATING ASSERTION.
	TestTrue(FString::Printf(
		TEXT("while before the first box the stop point offered is the HELD NODE less the gap (20500 at a gap of 300), ")
		TEXT("never a later box's start (20100 at a gap of 300): %.0f"), EarliestStopPointOffered),
		EarliestStopPointOffered >= HeldNodeAt - VanGap - 1.0);

	TestFalse(TEXT("and it never came to rest before the first box"), bStoppedBeforeTheBoxes);
	TestTrue(FString::Printf(TEXT("it entered the first box (%.0f uu)"), Agent->Follower.Travelled),
		Agent->Follower.Travelled > 20000.0);
	TestTrue(FString::Printf(TEXT("and stopped a gap short of the box whose end is held (%.0f uu, want %.0f)"), Agent->Follower.Travelled, 20400.0 - VanGap),
		Agent->Follower.Travelled <= 20400.0 - VanGap + 1.0);
	TestTrue(TEXT("stopped"), Agent->Follower.Speed < 1e-6);
	TestEqual(TEXT("waiting on the phantom"), Agent->GetWaitingOn(), Phantom);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficHoldingPositionTest,
	"Airside.Model.Traffic.HoldingPosition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficHoldingPositionTest::RunTest(const FString& Parameters)
{
	// A taxiway that CROSSES a runway: S -> H (hold bar) -> X (on the runway centreline)
	// -> N. The guideline edges are hand-built and carry no DerivedFrom, so the ONLY thing
	// protecting the runway here is the holding-position node - which is what this test is about.
	// The edge-derived-from-a-runway route to the same surface is exercised by the
	// arrival dispatch test once landings hold the chain.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FCrossingFixture Crossing = FCrossingFixture::Build(*Net);
	const FRoadSegmentId RunwaySeg = Crossing.Strip;
	const FGuidelineNodeId S = Crossing.S;
	const FGuidelineNodeId H = Crossing.H;
	const FGuidelineNodeId N = Crossing.N;

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	// Someone holds the runway: a claim by a phantom agent 99, as a landing would make.
	{
		FTrafficClaim Hold; Hold.AgentId = 99; Hold.Resource = FTrafficResource::OfSurface(RunwaySeg); Hold.bOccupied = true; Hold.Rank = 2;
		FTrafficClaim Blocker;
		Traffic->OccupancyForTest().TryClaim(Hold, Blocker);
	}
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, S, N, ETraversalClass::Aircraft), TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }

	// A BAR MUST NOT CLOSE THE RUNWAY FOR THE WHOLE TAXI. Sampled while the plane is still
	// more than one window (braking distance + gap, about 4500 uu here) short of the bar:
	// the claim is raised only once the window reaches the node, so before that nothing but
	// the phantom holds the strip. Excluding 99 asks "does anyone ELSE hold it".
	bool bRunwayFreeEarly = false;
	TickUntil(*Traffic, *Net, 60.0, [&](int32)
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
	// THE BODY CENTRE, NOT Follower.Travelled (#449): route distance is the nose gear's, and the claims are measured from the centre FClaimPass::CentreOf puts BodyCentreX - SteerAxleX from it - 316 uu aft on the Meridian. The default airframe was a hand copy with no footprint until #449, so the two coincided and this read one for the other.
	const double CentreAt = FClaimPass::CentreOf(*P);
	UE_LOG(LogAirsideTests, Log,
		TEXT("HoldingPosition measured: centre %.1f uu, nose %.1f uu (bar 17000), speed %.4f, waiting on %d, blocked step %d"),
		CentreAt, CentreAt + Traffic->Rules.AircraftFootprint * 0.5,
		P->Follower.Speed, P->GetWaitingOn(), P->GetBlockedStep());

	TestTrue(TEXT("stopped"), P->Follower.Speed < 1e-6);
	const double NoseAt = CentreAt + Traffic->Rules.AircraftFootprint * 0.5;
	TestTrue(FString::Printf(TEXT("nose within 50 uu of the hold bar and not past it (nose %.0f, bar 17000)"), NoseAt),
		NoseAt <= 17000.0 + 1.0 && NoseAt >= 17000.0 - 50.0);
	TestEqual(TEXT("waiting on the runway's holder"), P->GetWaitingOn(), 99);

	Traffic->OccupancyForTest().ReleaseAll(99);

	// THE BAR'S OWN CLAIM IS A RESERVATION, never an occupancy - nobody is occupied THROUGH
	// a bar, because an occupied claim cannot be preempted and a queue at the bar would then
	// lock the strip against the very landing the bar exists to protect. Sampled after the
	// phantom is gone and before the plane's centre reaches the bar, which is the only window
	// in which the plane holds the surface for the BAR's reason rather than the crossing's.
	bool bSawBarClaim = false;
	bool bBarClaimWasOccupied = false;
	TickUntil(*Traffic, *Net, 5.0, [&](int32)
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

	TickUntil(*Traffic, *Net, 120.0, [&](int32) { return Traffic->FindAgent(Plane)->Phase != EAgentPhase::Parked; });
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
	URoadProfile* Runway = TestProfiles::Runway();
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

	// AND THE SAME REFUSAL WITH NO PHANTOM: TWO REAL ARRIVALS, BACK TO BACK, NO TICK BETWEEN.
	//
	// The block above plants the hold by hand, so it measures ArrivalPlanner reading the
	// table - not that a landing ever PUTS anything in it. DispatchArrival recorded the chain
	// on the agent and left the claim to the next Advance, which means the strip was free to
	// the very next DispatchArrival: two aircraft cleared onto one runway for one frame, the
	// window the Taxiing->Departing handover already raises its claim synchronously to avoid
	// (see UGroundTraffic::Advance). A player pressing 7 twice is exactly that call pattern.
	{
		const FAirframe Piper = TestAirframes::Piper();
		const FTestAirport Fixture = FTestAirport::Build(Piper);
		URoadNetwork* Airport = Fixture.Net;
		const FVector2D Threshold = Fixture.Threshold;
		UGroundTraffic* Two = NewObject<UGroundTraffic>(GetTransientPackage());
		TArray<EArrivalRefusal> Refused;
		Two->OnArrivalRefused.AddLambda([&Refused](EArrivalRefusal Why) { Refused.Add(Why); });

		const FVector2D Approach = Threshold - FVector2D(1000.0, 0.0);
		const int32 First = Two->DispatchArrival(*Airport, Approach, Piper, 1.0);
		if (TestTrue(TEXT("the first arrival onto a free runway is admitted"), First > 0))
		{
			// EVERY SEGMENT OF THE CHAIN, not just the seed: this airport's runway is split
			// at its exit, and a landing that held only the piece it touched down on would
			// leave the rest of the strip free for somebody to line up on.
			const FRoadAgent* P = Two->FindAgent(First);
			if (TestNotNull(TEXT("and the agent is there"), P))
			{
				int32 Held = 0;
				for (const FRoadSegmentId Segment : P->RunwayHeld)
				{
					Held += Two->GetOccupancy().IsHeld(FTrafficResource::OfSurface(Segment), 0) ? 1 : 0;
				}
				UE_LOG(LogAirsideTests, Log,
					TEXT("ArrivalHoldsRunway measured: %d of %d chain segment(s) held before any tick"),
					Held, P->RunwayHeld.Num());
				TestTrue(TEXT("the whole chain is held in the dispatch call itself, before any Advance"),
					P->RunwayHeld.Num() > 0 && Held == P->RunwayHeld.Num());
			}

			// NO Advance BETWEEN THEM. That is the whole point: the second press happens in
			// the same frame as the first, and the refusal must not wait for a tick.
			TestEqual(TEXT("a second arrival in the same frame is refused"),
				Two->DispatchArrival(*Airport, Approach, Piper, 1.0), 0);
			TestTrue(TEXT("with RunwayOccupied, not with a landing-distance reason"),
				Refused.Num() == 1 && Refused[0] == EArrivalRefusal::RunwayOccupied);
			TestEqual(TEXT("and nothing was admitted for it"), Two->GetAgentCount(), 1);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficArrivalHoldSurvivesRunwaySplitTest,
	"Airside.Model.Traffic.ArrivalHoldSurvivesRunwaySplit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficArrivalHoldSurvivesRunwaySplitTest::RunTest(const FString& Parameters)
{
	// PLAYTEST 2026-09-28: an exit built onto the runway while one aircraft was landing let the
	// holding one land behind it. An exit is a SPLIT (URoadNetwork::SplitSegment frees the
	// segment and adds two), and RunwayHeld is the chain as it was at dispatch - so the landing
	// held handles the rebuild had just killed. Two cases: one segment of the chain split (the
	// playtest's multi-exit runway), and every segment split (a strip with no survivor at all).
	for (const bool bSplitAll : { false, true })
	{
		const FAirframe Piper = TestAirframes::Piper();
		const FTestAirport Fixture = FTestAirport::Build(Piper);
		URoadNetwork* Net = Fixture.Net;
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const FVector2D Approach = Fixture.Threshold - FVector2D(1000.0, 0.0);
		const TCHAR* Case = bSplitAll ? TEXT("every segment split") : TEXT("one segment split");

		const int32 First = Traffic->DispatchArrival(*Net, Approach, Piper, 1.0);
		if (!TestTrue(FString::Printf(TEXT("%s: the first arrival is admitted"), Case), First > 0))
		{
			continue;
		}
		Traffic->Advance(0.05, Net);

		// THE PLAYER'S EDIT: a taxiway spur off a new node on the strip. Each split at the
		// segment's own midpoint, so the new node is on the centreline wherever it is.
		const TArray<FRoadSegmentId> Before = Net->RunwayChain(Fixture.ThresholdSegment);
		for (const FRoadSegmentId Segment : Before)
		{
			if (!bSplitAll && Segment != Fixture.ThresholdSegment)
			{
				continue;
			}
			const FRoadSegment* S = Net->GetSegment(Segment);
			const FVector2D Mid = (Net->GetNode(S->A)->Position + Net->GetNode(S->B)->Position) * 0.5;
			const FRoadNodeId Middle = Net->SplitSegment(Segment, Mid);
			const FRoadNodeId Spur = Net->AddNode(Mid + FVector2D(0.0, -8000.0));
			Net->AddStraightSegment(Middle, Spur, TestProfiles::Taxiway());
		}
		TestGraph::Derive(*Net);
		FAnchorLink::Build(*Net, UAirsideSettings::ResolveLargestServiceVehicle());
		Traffic->OnGraphRebuilt(*Net);

		// The chain the split left, walked from anything still on the strip.
		const FRoadSegmentId Live = RunwayQuery::RunwaySegmentAt(*Net, Fixture.Threshold + FVector2D(100.0, 0.0));
		const TArray<FRoadSegmentId> After = Net->RunwayChain(Live);
		const FRoadAgent* Landing = Traffic->FindAgent(First);
		UE_LOG(LogAirsideTests, Log, TEXT("ArrivalHoldSurvivesRunwaySplit (%s): chain %d -> %d segment(s), agent holds %d"),
			Case, Before.Num(), After.Num(), Landing != nullptr ? Landing->RunwayHeld.Num() : -1);

		// BETWEEN THE REBUILD AND THE NEXT TICK, the window a same-frame accept lands in.
		TestTrue(FString::Printf(TEXT("%s: the runway is still busy straight after the rebuild"), Case),
			ArrivalPlanner::IsRunwayBusy(*Net, Approach, &Traffic->GetOccupancy()));

		Traffic->Advance(0.05, Net);
		TestTrue(FString::Printf(TEXT("%s: and still busy a tick later"), Case),
			ArrivalPlanner::IsRunwayBusy(*Net, Approach, &Traffic->GetOccupancy()));
		if (Landing != nullptr && TestEqual(FString::Printf(TEXT("%s: the landing is still landing"), Case),
			Landing->Phase, EAgentPhase::Arriving))
		{
			// THE SHAPE, not only the verdict: the agent holds the strip as it now is.
			for (const FRoadSegmentId Segment : After)
			{
				TestTrue(FString::Printf(TEXT("%s: it holds new segment %d"), Case, Segment.Index),
					Landing->RunwayHeld.Contains(Segment));
			}
		}
		TestEqual(FString::Printf(TEXT("%s: a second arrival is refused while it lands"), Case),
			Traffic->DispatchArrival(*Net, Approach, Piper, 1.0), 0);
	}
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficArrivalReceivesReverseSpeedTest,
	"Airside.Model.Traffic.ArrivalReceivesReverseSpeed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficArrivalReceivesReverseSpeedTest::RunTest(const FString& Parameters)
{
	// ISSUE #295: DispatchArrival stamped ShutdownPause by hand and never ReverseSpeed, so an
	// arrival was admitted holding the struct default (0.0) rather than Rules.ServiceReverseSpeed
	// - "the copy that nobody set" (CLAUDE.md), invisible only because nothing an arrival does
	// today reads the figure. FRoadAgent::StampRules is the fix: one call, from every admit
	// path, that sets both ShutdownPause and ReverseSpeed together.
	const FAirframe Piper = TestAirframes::Piper();
	const FTestAirport Fixture = FTestAirport::Build(Piper);
	URoadNetwork* Net = Fixture.Net;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	const int32 Id = Traffic->DispatchArrival(*Net, Fixture.Threshold - FVector2D(1000.0, 0.0), Piper, 1.0);
	if (!TestTrue(TEXT("the arrival is admitted"), Id > 0))
	{
		return false;
	}
	const FRoadAgent* Agent = Traffic->FindAgent(Id);
	if (!TestNotNull(TEXT("and the agent is there"), Agent))
	{
		return false;
	}
	TestEqual(TEXT("it holds the rules' reverse speed, not the struct default"),
		Agent->GetReverseSpeed(), Traffic->Rules.ServiceReverseSpeed);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficAgentIndexSurvivesRetireTest,
	"Airside.Model.Traffic.AgentIndexSurvivesRetire",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficAgentIndexSurvivesRetireTest::RunTest(const FString& Parameters)
{
	// ISSUE #295: FindIndex/FindAgent moved from an O(N) Agents.IndexOfByPredicate scan to an
	// id->index TMap (UGroundTraffic::AgentIndex), rebuilt WHOLESALE on every Admit/
	// RetireAgent/AdvanceOnce-removal rather than maintained incrementally - see
	// RebuildAgentIndex's own comment for why. This pins the one thing a rebuild has to get
	// right: retiring the MIDDLE agent of three shifts the survivors' array slots, and the
	// index has to move with them, not keep answering with the position an agent USED to hold.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	auto Lane = [&](double Y) { return TArray<FGuidelineNodeId>{
		TestGraph::Node(*Net, -1000.0, Y), TestGraph::Node(*Net, 1000.0, Y) }; };
	const TArray<FGuidelineNodeId> LaneA = Lane(0.0);
	const TArray<FGuidelineNodeId> LaneB = Lane(20000.0);
	const TArray<FGuidelineNodeId> LaneC = Lane(40000.0);
	TestGraph::Join(*Net, LaneA[0], LaneA[1]);
	TestGraph::Join(*Net, LaneB[0], LaneB[1]);
	TestGraph::Join(*Net, LaneC[0], LaneC[1]);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 First = Traffic->DispatchAgent(Net,
		M2TrafficRoute(*Net, LaneA[0], LaneA[1], ETraversalClass::GroundVehicle),
		TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	const int32 Middle = Traffic->DispatchAgent(Net,
		M2TrafficRoute(*Net, LaneB[0], LaneB[1], ETraversalClass::GroundVehicle),
		TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	const int32 Last = Traffic->DispatchAgent(Net,
		M2TrafficRoute(*Net, LaneC[0], LaneC[1], ETraversalClass::GroundVehicle),
		TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("all three admitted"), First > 0 && Middle > 0 && Last > 0))
	{
		return false;
	}

	TestTrue(TEXT("retiring the middle agent succeeds"), Traffic->RetireAgent(Middle));

	const FRoadAgent* FoundFirst = Traffic->FindAgent(First);
	const FRoadAgent* FoundLast = Traffic->FindAgent(Last);
	if (!TestNotNull(TEXT("the first survivor is still findable"), FoundFirst)
		|| !TestNotNull(TEXT("the last survivor is still findable"), FoundLast))
	{
		return false;
	}
	TestEqual(TEXT("the first survivor still answers its own id, not a shifted neighbour's"),
		FoundFirst->Id, First);
	TestEqual(TEXT("the last survivor still answers its own id, not a shifted neighbour's"),
		FoundLast->Id, Last);
	TestNull(TEXT("the retired agent is gone"), Traffic->FindAgent(Middle));
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficCrossingHoldsRunwayTest,
	"Airside.Model.Traffic.CrossingHoldsRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficCrossingHoldsRunwayTest::RunTest(const FString& Parameters)
{
	// SPEC §3.1'S FOURTH ROUTE. The HoldingPosition geometry with NOBODY holding the runway: the
	// plane is granted the bar and crosses, and the question is what happens AFTER the bar.
	// Before this rule the bar node left the window as the plane passed it and the surface
	// claim went with it, leaving an aeroplane standing on the centreline with the table
	// saying the strip was free - so a landing could be cleared onto it. The crossing edges
	// cannot cover the gap: here they are hand-built with no DerivedFrom, and in a DERIVED
	// graph a junction's turn paths carry none either, by design.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	// A SECOND BAR ON THE FAR SIDE, protecting the SAME runway, because that is how a
	// crossing is actually painted - one bar each side - and it is the case that broke the
	// hold. The far bar's claim is a RESERVATION on a chain the crossing block is already
	// holding OCCUPIED, and TryClaim treats a same-agent claim on the same resource as an
	// update written over the old one wholesale: without the skip in ClaimAhead the
	// reservation replaced the occupancy, and a landing could then preempt an aeroplane
	// standing on the centreline.
	const FCrossingFixture Crossing = FCrossingFixture::Build(*Net, /*bFarBar=*/true);
	const FRoadSegmentId RunwaySeg = Crossing.Strip;
	const FGuidelineNodeId S = Crossing.S;
	const FGuidelineNodeId N = Crossing.N;

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
	const FAirframe Airframe = TestAirframes::GroundOnly();
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

	/** One injected graph rebuild, taken while the BODY is on the strip. See below. */
	bool bRebuiltMidCrossing = false;
	bool bStripHeldAcrossRebuild = false;

	TickUntil(*Traffic, *Net, 120.0, [&](int32)
	{
		const FRoadAgent* Q = Traffic->FindAgent(Plane);
		if (Q == nullptr) { return false; }
		// THE BODY CENTRE, NOT Follower.Travelled (#449): route distance is the nose gear's, and the claims are measured from the centre FClaimPass::CentreOf puts BodyCentreX - SteerAxleX from it - 316 uu aft on the Meridian. The default airframe was a hand copy with no footprint until #449, so the two coincided and this read one for the other.
		const double Travelled = FClaimPass::CentreOf(*Q);
		const bool bHeld = Traffic->GetOccupancy().IsHeld(Strip, 0);

		if (Travelled < 10000.0 && !bHeld) { bFreeBeforeTheBar = true; }
		if (bHeld && HoldBegan < 0.0) { HoldBegan = Travelled; }
		if (!bHeld && HoldBegan >= 0.0 && HoldEnded < 0.0) { HoldEnded = Travelled; }

		// THE CROSSING ITSELF, separately from the table. With a bar on each side the strip
		// is HELD continuously from the near bar's reservation to the far bar's, so IsHeld
		// alone can no longer say when the crossing ended - and when the crossing ends is
		// the rule under test. Measured off the agent's own field, which is what the
		// geometric release actually clears.
		if (Q->GetCrossingRunway().IsSet()) { bWasCrossing = true; }
		else if (bWasCrossing && CrossingEnded < 0.0) { CrossingEnded = Travelled; }

		// THE EXIT BAR ARMS NOTHING. Past the far bar at 23000 the aeroplane is leaving the
		// strip, and a rule that cannot tell that from entering it re-arms the hold here and
		// keeps the runway shut for the whole 17000 uu leg to N.
		if (Travelled > 23100.0 && Q->GetCrossingRunway().IsSet()) { bCrossingPastTheFarBar = true; }

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

		// A GRAPH REBUILD MID-CROSSING MUST NOT HAND THE STRIP BACK. Injected ONCE, on the
		// first tick the body is actually on the asphalt, and asserted IMMEDIATELY - before
		// any Advance can re-raise the claim, because "the next tick puts it back" is exactly
		// the answer that is not good enough: ArrivalPlanner::Plan reads this table directly
		// at DispatchArrival, between ticks, for as long as it takes a player to click.
		// Nothing about the graph changes here, so the re-resolution itself is a no-op and
		// what is under test is the RELEASE - Clear() drops this aeroplane's surface claim
		// and ReleaseGuidelineClaims does not.
		//
		// At the END of the lambda so this tick's other measurements were all taken against
		// the table the arbiter actually left behind.
		if (!bRebuiltMidCrossing && Q->GetCrossingPhase() == ECrossingPhase::OnStrip)
		{
			bRebuiltMidCrossing = true;
			Traffic->OnGraphRebuilt(*Net);
			bStripHeldAcrossRebuild = Traffic->GetOccupancy().IsHeld(Strip, 0);
		}

		// Well past the tail's clearance of the FAR BAR (23500, itself 3000 uu clear of the
		// strip) and far short of the next route node (40000), so the two candidate release
		// rules cannot both pass the assertions below. (Travelled holds the CENTRE - see above.)
		return Travelled < 25000.0;
	});

	const FRoadAgent* P = Traffic->FindAgent(Plane);
	UE_LOG(LogAirsideTests, Log,
		TEXT("CrossingHoldsRunway measured (body centre): hold began at route %.0f uu, ended at %.0f uu ")
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
	TestTrue(TEXT("the injected rebuild really did land while the body was on the strip"), bRebuiltMidCrossing);
	TestTrue(TEXT("and it kept the strip held: a guideline rebuild frees guidelines, never surfaces"),
		bStripHeldAcrossRebuild);
	TestFalse(TEXT("and the strip is free afterwards"), Traffic->GetOccupancy().IsHeld(Strip, 0));
	TestFalse(TEXT("with nothing left naming a crossing"), P->GetCrossingRunway().IsSet());
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRunwayChainCacheTest,
	"Airside.Model.Traffic.RunwayChainCache",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRunwayChainCacheTest::RunTest(const FString& Parameters)
{
	// ISSUE #170: HoldRunwayOnly, UpdateCrossing and BuildPending together ask "which
	// segments make up Seed's strip" from a dozen call sites, and before FRunwayChainCache
	// every one of them - direct RunwayChain/RunwayChainOrSeed calls, and the ones hiding
	// inside IsPointOnRunway/IsGuidelineNodeOnRunway's Seed overloads - re-walked the graph
	// and heap-allocated a fresh TArray to answer it, for the SAME seed, on the same
	// substep, and then again on every substep after it for as long as the agent stood near
	// a bar or a crossing.
	//
	// THE FIXTURE IS THE SAME SHAPE Airside.Model.Traffic.CrossingHoldsRunway MEASURES: one
	// runway with a bar each side (so BOTH of BuildPending's bar/crossing branches fire, not
	// just one), so this is pinned against geometry that rule was already written for rather
	// than a shape invented just to make the count small.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FCrossingFixture Crossing = FCrossingFixture::Build(*Net, /*bFarBar=*/true);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Airframe = TestAirframes::GroundOnly();
	const int32 Plane = Traffic->DispatchAgent(Net,
		M2TrafficRoute(*Net, Crossing.S, Crossing.N, ETraversalClass::Aircraft), Airframe, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }

	// THE WHOLE TAXI, near-bar to past-the-far-bar and on to N: every substep of every tick
	// this agent spends taxiing asks the claim pass about Crossing.Strip at least once,
	// through Agent.CrossingRunway (UpdateCrossing, BuildPending's route zero) and through
	// H's and Far's HoldingPositionFor (BuildPending's bar branch) - the SAME seed
	// throughout, since this fixture names exactly one runway.
	TickUntil(*Traffic, *Net, 120.0, [&](int32) { return Traffic->FindAgent(Plane)->Phase != EAgentPhase::Parked; });
	TestEqual(TEXT("the crossing agent reaches N and parks"), Traffic->FindAgent(Plane)->Phase, EAgentPhase::Parked);

	const int32 Walks = Traffic->GetRunwayChainWalksForTest();
	UE_LOG(LogAirsideTests, Log,
		TEXT("RunwayChainCache measured: %d actual RunwayQuery::RunwayChain walk(s) across the whole crossing ")
		TEXT("(one runway strip named throughout by every one of UpdateCrossing's and BuildPending's several call ")
		TEXT("sites, across every substep of every tick)"),
		Walks);

	// O(DISTINCT SEEDS), NOT O(CALLS): one strip is ever named here, so one walk pays for the
	// whole taxi no matter how many times, or from how many call sites, it is asked about.
	// This goes RED the moment TrafficClaims.cpp calls Network.RunwayChain/RunwayChainOrSeed/
	// IsPointOnRunway(Position, Seed)/IsGuidelineNodeOnRunway(Node, Seed) directly again
	// instead of asking FClaimPass::Chains.
	TestEqual(TEXT("exactly one walk for the one runway strip this fixture names"), Walks, 1);
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
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RM = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
	const FRoadSegmentId Near = Net->AddStraightSegment(RA, RM, Runway);
	const FRoadSegmentId Far = Net->AddStraightSegment(RM, RB, Runway);
	if (!TestEqual(TEXT("the split runway is a two-segment chain"), Net->RunwayChain(Far).Num(), 2))
	{
		return false;
	}

	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, -20000.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId C = TestGraph::Node(*Net, 0.0, 20000.0);
	TestGraph::Join(*Net, A, B);
	{
		// Hand-built rather than through TestGraph::Join, because DerivedFrom is the whole
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
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::Aircraft), TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }

	TickUntil(*Traffic, *Net, 60.0, [&](int32)
	{
		const FRoadAgent* Q = Traffic->FindAgent(Plane);
		return Q->Follower.Speed > 1e-6 || Q->Follower.Travelled < 1.0;
	});

	const FRoadAgent* P = Traffic->FindAgent(Plane);
	// THE BODY CENTRE, NOT Follower.Travelled (#449): route distance is the nose gear's, and the claims are measured from the centre FClaimPass::CentreOf puts BodyCentreX - SteerAxleX from it - 316 uu aft on the Meridian. The default airframe was a hand copy with no footprint until #449, so the two coincided and this read one for the other.
	const double CentreAt = FClaimPass::CentreOf(*P);
	UE_LOG(LogAirsideTests, Log,
		TEXT("RunwayEdgeClaim measured: centre stopped at route %.0f uu (want 18500 = the runway edge's ")
		TEXT("start 20000 less the gap 1500), waiting on %d, blocked step %d"),
		CentreAt, P->GetWaitingOn(), P->GetBlockedStep());

	TestTrue(TEXT("stopped"), P->Follower.Speed < 1e-6);
	TestTrue(FString::Printf(TEXT("a gap short of where the runway edge BEGINS, not inside it (%.0f, want 18500)"),
		CentreAt),
		FMath::Abs(CentreAt - 18500.0) < 50.0);
	TestEqual(TEXT("blocked on the step whose edge lies on the runway"), P->GetBlockedStep(), 1);
	TestEqual(TEXT("waiting on the holder of the OTHER segment of the same chain"), P->GetWaitingOn(), 99);

	Traffic->OccupancyForTest().ReleaseAll(99);
	bool bPlaneHeldTheChain = false;
	bool bSomebodyElseHeldIt = false;
	TickUntil(*Traffic, *Net, 180.0, [&](int32)
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
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 20000.0, 0.0);
	const FGuidelineNodeId C = TestGraph::Node(*Net, 40000.0, 0.0);
	const FGuidelineNodeId X = TestGraph::Node(*Net, 30000.0, 15000.0);
	TestGraph::Join(*Net, A, B);
	const FGuidelineEdgeId BC = TestGraph::Join(*Net, B, C);
	TestGraph::Join(*Net, B, X); TestGraph::Join(*Net, X, C);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	TickUntil(*Traffic, *Net, 8.0, [](int32) { return true; });
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
	auto Sample = [&]()
	{
		const FRoadAgent* Now = Traffic->FindAgent(Van);
		MaxJump = FMath::Max(MaxJump, FVector2D::Distance(Now->LastMotion.Position, Last));
		Last = Now->LastMotion.Position;
		MaxY = FMath::Max(MaxY, Now->LastMotion.Position.Y);
	};

	// A SPLICE STEP BEHIND THE AGENT, refused. Travelled survives a splice - that is the
	// whole point of Replace - so re-basing the plan at a node the van has already driven
	// past would map the same route distance onto different geometry and put the van
	// somewhere else on the airport in one frame. Measured here rather than argued: the van
	// is part way along step 1, so step 0 is behind it.
	TickUntil(*Traffic, *Net, 60.0, [&](int32)
	{
		Sample();
		return Traffic->FindAgent(Van)->Follower.Travelled < 21000.0;
	});
	V = Traffic->FindAgent(Van);
	const int32 StepsBehind = V->Follower.Plan.Steps.Num();
	const double LengthBehind = V->Follower.Plan.Length;
	const double TravelledBehind = V->Follower.Travelled;
	const double SpeedBehind = V->Follower.Speed;
	TestTrue(FString::Printf(TEXT("part way along step 1 (%.0f uu)"), TravelledBehind), TravelledBehind > 20000.0);

	TestFalse(TEXT("a splice step BEHIND the agent is refused"), Traffic->ReplanAt(Van, *Net, 0, FGuidelineEdgeId()));
	V = Traffic->FindAgent(Van);
	TestEqual(TEXT("and the plan is the one it had"), V->Follower.Plan.Steps.Num(), StepsBehind);
	TestEqual(TEXT("with the same length"), V->Follower.Plan.Length, LengthBehind, 1e-9);
	TestEqual(TEXT("at the same route distance"), V->Follower.Travelled, TravelledBehind, 1e-9);
	TestEqual(TEXT("at the same speed"), V->Follower.Speed, SpeedBehind, 1e-9);

	TickUntil(*Traffic, *Net, 200.0, [&](int32)
	{
		Sample();
		return Traffic->FindAgent(Van)->Phase != EAgentPhase::Parked;
	});

	// MEASURED, not asserted: a teleport is what a splice that re-based EndDistance wrongly
	// would look like, and the number says how near the bound the drive actually ran.
	UE_LOG(LogAirsideTests, Log,
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
	const FGuidelineNodeId P = TestGraph::Node(*Dead, 0.0, 0.0);
	const FGuidelineNodeId Qn = TestGraph::Node(*Dead, 20000.0, 0.0);
	const FGuidelineEdgeId PQ = TestGraph::Join(*Dead, P, Qn);

	UGroundTraffic* Only = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Stuck = Only->DispatchAgent(Dead, M2TrafficRoute(*Dead, P, Qn, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	TickUntil(*Only, *Dead, 5.0, [](int32) { return true; });
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
	FTrafficDeadlockRingTest,
	"Airside.Model.Traffic.DeadlockRing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficDeadlockRingTest::RunTest(const FString& Parameters)
{
	// One compound junction: a square ring of one-way 750 uu lanes A->B->C->D->A, each
	// shorter than a van's footprint + gap (800), so the box-entry rule applies to every
	// edge. Four vans start ON the corners, each bound for the corner two ahead: V1 A->C via
	// B, V2 B->D via C, V3 C->A via D, V4 D->B via A. Nobody may enter its first edge while
	// the far node is occupied, so all four are stopped at t = 0 waiting on each other - a
	// genuine cycle. One escape: D->X1->X2->X3->B round the outside, longer than D->A->B so
	// V4 does not take it unprompted.
	//
	// A SQUARE, NOT THE TRIANGLE Airside.Model.Traffic.BoxEntry uses, and the difference is
	// the corner angle. A van waiting at the entry of its NEXT box sits a gap (300) short of
	// that box's start, i.e. 450 uu past the corner behind it. Node reach (NodeReach.h) holds
	// a corner for as far as two bodies down its two edges stay within a footprint: at 90
	// degrees that is F/sqrt(2) = 354, so the waiter has cleared the corner behind and the
	// van waiting for THAT corner may take it. At 60 degrees it is F = 500, the waiter still
	// blocks the corner, and the ring is a gridlock nothing can turn out of - which is the
	// truth about a triangle of 600 uu lanes and 500 uu vans: measured on that fixture, the
	// old half-footprint rule "resolved" it by driving one van 278 uu from another.
	//
	// THE ESCAPE MEETS THE RING SQUARE ON, for the same reason. Its last arm arrives at B
	// along the x axis, at right angles to the lane leaving B; drawn to arrive 22 degrees
	// off that lane (measured, first draft of this fixture) B's reach along the lane was the
	// whole 750 uu, the van waiting in it never released B, and the ring re-locked with no
	// member able to turn. An escape that hugs the lane it rejoins is not an escape.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 750.0, 0.0);
	const FGuidelineNodeId C = TestGraph::Node(*Net, 750.0, 750.0);
	const FGuidelineNodeId D = TestGraph::Node(*Net, 0.0, 750.0);
	const FGuidelineNodeId X1 = TestGraph::Node(*Net, -500.0, 1250.0);
	const FGuidelineNodeId X2 = TestGraph::Node(*Net, 1500.0, 1250.0);
	const FGuidelineNodeId X3 = TestGraph::Node(*Net, 1500.0, 0.0);
	TestGraph::Join(*Net, A, B, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, B, C, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, C, D, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, D, A, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, D, X1, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, X1, X2, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, X2, X3, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, X3, B, { EGuidelineDir::AToB });

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	// THE FIXTURE OWNS ITS FOOTPRINT, because every number in the comment above is derived
	// from it: 750 uu lanes shorter than footprint + gap (800), a waiter sitting 450 uu past
	// the corner behind it, and a node holding that corner for F/sqrt(2) = 354 - which is
	// what lets the waiter clear it and the ring turn over.
	//
	// It used to read the default, and on 2026-09-14 that default became fueltruck1's real
	// length: 620, up from 500. F/sqrt(2) goes to 438 against an unchanged 450, so the margin
	// that makes this ring ESCAPABLE - 96 uu - collapses to 12, and the ring gridlocks
	// exactly as this fixture's own comment says a 60-degree one does. The resolver is right
	// to fail to turn it; the geometry no longer has the property the test was built on.
	//
	// Pinned rather than scaling the lanes to suit, because what is under test is the
	// DEADLOCK RESOLVER, not how long a fuel truck is. DeadlockMixedClass pins its aircraft
	// footprint to the van's for the same reason and says so at the assignment.
	Traffic->Rules.VehicleFootprint = 500.0;

	const int32 V1 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V2 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, B, D, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V3 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, C, A, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V4 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, D, B, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("all four routed and dispatched"), V1 > 0 && V2 > 0 && V3 > 0 && V4 > 0)) { return false; }
	TestEqual(TEXT("V4's first plan goes via A (2 steps), not the escape"), Traffic->FindAgent(V4)->Follower.Plan.Steps.Num(), 2);

	// All four stopped, each waiting on the next, before any resolution.
	TickUntil(*Traffic, *Net, 1.0, [](int32) { return true; });
	TestTrue(TEXT("V1 waits on V2"), Traffic->FindAgent(V1)->GetWaitingOn() == V2);
	TestTrue(TEXT("V2 waits on V3"), Traffic->FindAgent(V2)->GetWaitingOn() == V3);
	TestTrue(TEXT("V3 waits on V4"), Traffic->FindAgent(V3)->GetWaitingOn() == V4);
	TestTrue(TEXT("V4 waits on V1"), Traffic->FindAgent(V4)->GetWaitingOn() == V1);
	bool bNobodyMoved = true;
	for (const int32 Id : { V1, V2, V3, V4 }) { bNobodyMoved &= Traffic->FindAgent(Id)->Follower.Travelled < 1.0; }
	TestTrue(TEXT("nobody has moved"), bNobodyMoved);

	double MaxJump = 0.0;
	double MinSeparation = TNumericLimits<double>::Max();
	TMap<int32, FVector2D> Last;
	int32 ResolvedAtTick = -1;
	TickUntil(*Traffic, *Net, 120.0, [&](int32 Tick)
	{
		bool bAllParked = true;
		for (const FRoadAgent& Agent : Traffic->GetAgents())
		{
			if (const FVector2D* Prev = Last.Find(Agent.Id)) { MaxJump = FMath::Max(MaxJump, FVector2D::Distance(*Prev, Agent.LastMotion.Position)); }
			Last.Add(Agent.Id, Agent.LastMotion.Position);
			bAllParked &= (Agent.Phase == EAgentPhase::Parked);
			for (const FRoadAgent& Other : Traffic->GetAgents())
			{
				if (Other.Id > Agent.Id && Agent.Phase == EAgentPhase::Taxiing && Other.Phase == EAgentPhase::Taxiing)
				{
					MinSeparation = FMath::Min(MinSeparation, FVector2D::Distance(Agent.LastMotion.Position, Other.LastMotion.Position));
				}
			}
		}
		if (ResolvedAtTick < 0 && Traffic->GetLastResolvedAgentForTest() != 0) { ResolvedAtTick = Tick; }
		return !bAllParked;
	});

	UE_LOG(LogAirsideTests, Log,
		TEXT("DeadlockRing measured: resolved at tick %d of the second run (bound %d), agent %d replanned, ")
		TEXT("%d cycle(s) detected, max per-tick step %.1f uu, min separation while taxiing %.0f uu"),
		ResolvedAtTick, static_cast<int32>(Traffic->Rules.StallSeconds / 0.05) + 2,
		Traffic->GetLastResolvedAgentForTest(), Traffic->GetCyclesDetectedForTest(), MaxJump, MinSeparation);

	TestEqual(TEXT("exactly one cycle was detected"), Traffic->GetCyclesDetectedForTest(), 1);
	TestTrue(FString::Printf(TEXT("detected within StallSeconds + one tick (tick %d)"), ResolvedAtTick), ResolvedAtTick >= 0 && ResolvedAtTick <= static_cast<int32>(Traffic->Rules.StallSeconds / 0.05) + 2);
	TestEqual(TEXT("the agent that replanned is the highest id"), Traffic->GetLastResolvedAgentForTest(), V4);
	// Spliced at step 0 (V4 never left D), so the new plan IS the tail: D->X1->X2->X3->B.
	TestEqual(TEXT("V4 now has four steps round the outside"), Traffic->FindAgent(V4) ? Traffic->FindAgent(V4)->Follower.Plan.Steps.Num() : 0, 4);
	TestEqual(TEXT("the first of which goes to X1"), Traffic->FindAgent(V4)->Follower.Plan.Steps[0].To, X1);
	for (const int32 Id : { V1, V2, V3, V4 })
	{
		TestEqual(FString::Printf(TEXT("agent %d reached its goal"), Id), Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked);
	}
	TestTrue(FString::Printf(TEXT("no agent moved more than one frame's travel in any tick (%.1f uu)"), MaxJump), MaxJump <= 1000.0 * 0.05 + 1.0);
	// THE RING RESOLVES WITHOUT ANYBODY DRIVING THROUGH ANYBODY, which is the claim the
	// triangle could not make. The bound is CENTRES a gap short of one corner on two
	// perpendicular arms - Gap * sqrt(2) = 424 - not a whole footprint: the nearest two
	// bodies ever get here is both stopped a gap short of the same corner, noses 70 uu
	// apart and pointing at right angles, which is a queue and not a collision. Measured
	// 453 on the first run of this fixture.
	TestTrue(FString::Printf(TEXT("never closer than two vans queued at one corner (%.0f uu)"), MinSeparation),
		MinSeparation >= Traffic->Rules.GapFor(ETraversalClass::GroundVehicle) * FMath::Sqrt(2.0) - 1.0);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficDeadlockMixedClassTest,
	"Airside.Model.Traffic.DeadlockMixedClass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficDeadlockMixedClassTest::RunTest(const FString& Parameters)
{
	// THE CLASS ARM OF THE CANDIDATE ORDERING, which DeadlockRing cannot test because all
	// four of its members are vans and the tie-break decides everything. Same ring, but the
	// agent on C is an AIRCRAFT, and both it and the van on B have an escape available:
	// C->X1->X2->X3->A for the aircraft, B->Y1->Y2->Y3->D for the van, both round the
	// outside, and both leaving and rejoining the ring at right angles - see DeadlockRing
	// for why an escape that rejoins at a shallow angle is no escape at all.
	// Spec §5 says the LOWEST-ranked member that can turn goes round, so the van must be the
	// one that replans and the aeroplane must be left on the route it was cleared for -
	// which is the whole reason the rule is "lowest class priority first" and not "whoever
	// is nearest a turn". The vans on A and D have no escape, so among the vans the one
	// that CAN turn is the one that goes, whatever its id.
	//
	// Every arm is a box (750 uu against footprint + gap = 800), so the gridlock forms at the
	// corners exactly as in DeadlockRing - see the note on the rules below for why the
	// aeroplane is given the vehicle's figures to make that true of it too.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 750.0, 0.0);
	const FGuidelineNodeId C = TestGraph::Node(*Net, 750.0, 750.0);
	const FGuidelineNodeId D = TestGraph::Node(*Net, 0.0, 750.0);
	const FGuidelineNodeId X1 = TestGraph::Node(*Net, 1500.0, 750.0);
	const FGuidelineNodeId X2 = TestGraph::Node(*Net, 1500.0, -1000.0);
	const FGuidelineNodeId X3 = TestGraph::Node(*Net, 0.0, -1000.0);
	const FGuidelineNodeId Y1 = TestGraph::Node(*Net, 750.0, -750.0);
	const FGuidelineNodeId Y2 = TestGraph::Node(*Net, -750.0, -750.0);
	const FGuidelineNodeId Y3 = TestGraph::Node(*Net, -750.0, 750.0);
	TestGraph::Join(*Net, A, B, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, B, C, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, C, D, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, D, A, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, C, X1, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, X1, X2, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, X2, X3, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, X3, A, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, B, Y1, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, Y1, Y2, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, Y2, Y3, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, Y3, D, { EGuidelineDir::AToB });

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	// THE AEROPLANE IS GIVEN THE VEHICLE'S FOOTPRINT AND GAP, and that is the fixture's one
	// deliberate lie. A default airframe is 1000 uu long and keeps 1500 uu ahead of its nose,
	// so on 750 uu arms it stands on two nodes at once and its stop point for anything on the
	// next arm is behind where it already is: it cannot move at all, whatever the arbiter
	// decides. That is the airport-geometry rule ("size ground geometry for the largest
	// aircraft admitted"), not a traffic defect, and a ring scaled up to admit a real
	// aeroplane stops being a box for a van - so the gridlock would form mid-edge and no
	// member could turn, which is a different test.
	//
	// WHAT IS UNDER TEST SURVIVES IT UNTOUCHED: the candidate ordering reads
	// TraversalPriority(Class), never a size. This agent still ranks as an aircraft
	// everywhere it matters.
	// Pinned for the reason DeadlockRing's is, and BEFORE the line below so the aircraft
	// still matches the van: this ring is the same 750 uu square, so it needs the same 500
	// the geometry was drawn around rather than fueltruck1's 620.
	Traffic->Rules.VehicleFootprint = 500.0;
	Traffic->Rules.AircraftFootprint = Traffic->Rules.VehicleFootprint;
	Traffic->Rules.AircraftGap = Traffic->Rules.GapFor(ETraversalClass::GroundVehicle);
	const int32 V1 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V2 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, B, D, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	const int32 P3 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, C, A, ETraversalClass::Aircraft), TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	const int32 V4 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, D, B, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("all four routed and dispatched"), V1 > 0 && V2 > 0 && P3 > 0 && V4 > 0)) { return false; }
	TestEqual(TEXT("the van on B goes via C (2 steps), not round Y"), Traffic->FindAgent(V2)->Follower.Plan.Steps.Num(), 2);
	TestEqual(TEXT("and the aircraft via D, not round X"), Traffic->FindAgent(P3)->Follower.Plan.Steps.Num(), 2);

	int32 FirstResolved = 0;
	bool bAircraftEverReplanned = false;
	double MinSeparation = TNumericLimits<double>::Max();
	TickUntil(*Traffic, *Net, 180.0, [&](int32)
	{
		if (FirstResolved == 0) { FirstResolved = Traffic->GetLastResolvedAgentForTest(); }
		const FRoadAgent* Plane = Traffic->FindAgent(P3);
		if (Plane != nullptr && Plane->Phase == EAgentPhase::Taxiing)
		{
			bAircraftEverReplanned = bAircraftEverReplanned
				|| Plane->Follower.Plan.Steps.Num() != 2
				|| (Plane->Follower.Plan.Steps.Num() == 2 && Plane->Follower.Plan.Steps[0].To == X1);
		}
		bool bAllParked = true;
		for (const FRoadAgent& Agent : Traffic->GetAgents())
		{
			bAllParked &= (Agent.Phase == EAgentPhase::Parked);
			for (const FRoadAgent& Other : Traffic->GetAgents())
			{
				if (Other.Id > Agent.Id && Agent.Phase == EAgentPhase::Taxiing && Other.Phase == EAgentPhase::Taxiing)
				{
					MinSeparation = FMath::Min(MinSeparation, FVector2D::Distance(Agent.LastMotion.Position, Other.LastMotion.Position));
				}
			}
		}
		return !bAllParked;
	});

	UE_LOG(LogAirsideTests, Log,
		TEXT("DeadlockMixedClass measured: first resolved agent %d (van 1 = %d, van 2 = %d, aircraft = %d, van 4 = %d), ")
		TEXT("%d cycle(s), %d line(s), aircraft plan %d step(s), min separation while taxiing %.0f uu"),
		FirstResolved, V1, V2, P3, V4, Traffic->GetCyclesDetectedForTest(),
		Traffic->GetDeadlockLogLinesForTest(),
		Traffic->FindAgent(P3) ? Traffic->FindAgent(P3)->Follower.Plan.Steps.Num() : 0, MinSeparation);
	for (const FRoadAgent& Agent : Traffic->GetAgents())
	{
		UE_LOG(LogAirsideTests, Log,
			TEXT("  agent %d: phase %s, %.0f of %.0f uu, waiting on %d, blocked step %d, stalled %.1f s"),
			Agent.Id, *UEnum::GetValueAsString(Agent.Phase), Agent.Follower.Travelled,
			Agent.Follower.Plan.Length, Agent.GetWaitingOn(), Agent.GetBlockedStep(), Agent.GetStalledSeconds());
	}

	TestEqual(TEXT("the lowest-ranked member that can turn goes round: the van, not the aircraft"), FirstResolved, V2);
	TestFalse(TEXT("and the aircraft keeps the route it was cleared for"), bAircraftEverReplanned);
	for (const int32 Id : { V1, V2, P3, V4 })
	{
		TestEqual(FString::Printf(TEXT("agent %d reached its goal"), Id), Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked);
	}
	// The corner bound, as in DeadlockRing: a gap short of one corner on two arms.
	TestTrue(FString::Printf(TEXT("never closer than two vans queued at one corner (%.0f uu)"), MinSeparation),
		MinSeparation >= Traffic->Rules.GapFor(ETraversalClass::GroundVehicle) * FMath::Sqrt(2.0) - 1.0);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficBarToBarCrossingTest,
	"Airside.Model.Traffic.BarToBarCrossing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficBarToBarCrossingTest::RunTest(const FString& Parameters)
{
	// A CROSSING WITH NO NODE ON THE STRIP: one hand-drawn edge straight from the near bar to
	// the far one, which is what a player draws and what the generated graph does NOT produce
	// (spec R10 guarantees an on-strip node only for GENERATED crossings). The node-based
	// version of spec §3.1's fourth route armed half a footprint late here and released with
	// up to half a footprint of tail still on the asphalt, because there was no node between
	// the bars for it to reason about. This fixture is that graph, and the assertions are the
	// BODY against the surface: nose in, tail out.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
	const FRoadSegmentId RunwaySeg = Net->AddStraightSegment(RA, RB, Runway);

	const FGuidelineNodeId S = TestGraph::Node(*Net, 0.0, -20000.0);
	const FGuidelineNodeId Hn = TestGraph::Node(*Net, 0.0, -3000.0);
	const FGuidelineNodeId Hf = TestGraph::Node(*Net, 0.0, 3000.0);
	const FGuidelineNodeId N = TestGraph::Node(*Net, 0.0, 20000.0);
	TestGraph::Join(*Net, S, Hn);
	TestGraph::Join(*Net, Hn, Hf);   // ONE edge across the runway. No vertex on the strip.
	TestGraph::Join(*Net, Hf, N);
	Net->SetRunwayHoldingPositionForTest(Hn, RunwaySeg);
	Net->SetRunwayHoldingPositionForTest(Hf, RunwaySeg);

	// Route distances: near bar 17000, centreline 20000, far bar 23000, N 40000. Half width
	// 2250, so the strip runs from 17750 to 22250 in route distance. Footprint 1000, so the
	// NOSE is on the asphalt from centre 17250 and the TAIL is off it from centre 22750.
	const double HalfWidth = 2250.0;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Airframe = TestAirframes::GroundOnly();
	const double Half = Traffic->Rules.AircraftFootprint * 0.5;
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, S, N, ETraversalClass::Aircraft), Airframe, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }

	const FTrafficResource Strip = FTrafficResource::OfSurface(RunwaySeg);
	bool bFreeWellBeforeTheBar = false;
	bool bUnheldWhileBodyOnStrip = false;
	bool bReservedWhileBodyOnStrip = false;
	bool bCrossingPastTheFarBar = false;
	double ArmedAt = -1.0;
	double ReleasedAt = -1.0;

	// MEASURED AT THE POSE THE ARBITER SAW, which is the previous tick's: Advance claims
	// first and moves second (see its header), so the table read after a tick answers for the
	// route distance the agent was at when the tick began. Comparing this tick's claims
	// against this tick's position would fail every threshold by one frame's travel and would
	// be measuring the tick order, not the crossing rule.
	//
	// THAT BOOKKEEPING IS ONLY VALID WHILE ONE CALL IS ONE PASS, which is why the tick below
	// is the substep and not the 0.05 the other fixtures use. Advance now splits a long delta
	// into bounded substeps (see FTrafficRules::MaxSubstepSeconds), and a call holding two
	// passes arbitrates twice - at the start pose and half a tick later - while this variable
	// still names only the first. Every threshold here would then be read against a pose the
	// agent had already left, which is measuring the substep count rather than the crossing
	// rule. Ticking at the substep keeps the call a single pass, and has the better property
	// of measuring the crossing at the granularity PRODUCTION now runs it: no frame reaches
	// the model coarser than this, whatever the speed multiplier.
	double SeenByTheArbiter = 0.0;

	TickUntil(*Traffic, *Net, 120.0, [&](int32)
	{
		const FRoadAgent* Q = Traffic->FindAgent(Plane);
		if (Q == nullptr) { return false; }
		const double T = SeenByTheArbiter;
		const bool bHeld = Traffic->GetOccupancy().IsHeld(Strip, 0);
		// THE BODY CENTRE, NOT Follower.Travelled (#449): route distance is the nose gear's, and the claims are measured from the centre FClaimPass::CentreOf puts BodyCentreX - SteerAxleX from it - 316 uu aft on the Meridian. The default airframe was a hand copy with no footprint until #449, so the two coincided and this read one for the other.
		SeenByTheArbiter = FClaimPass::CentreOf(*Q);

		// More than one window (braking distance + gap, about 4000 uu) short of the bar:
		// nothing has any business holding the strip yet.
		if (T < 10000.0 && !bHeld) { bFreeWellBeforeTheBar = true; }

		if (ArmedAt < 0.0 && Q->GetCrossingPhase() != ECrossingPhase::None) { ArmedAt = T; }
		if (ArmedAt >= 0.0 && ReleasedAt < 0.0 && Q->GetCrossingPhase() == ECrossingPhase::None) { ReleasedAt = T; }
		if (T > 23100.0 && Q->GetCrossingPhase() != ECrossingPhase::None) { bCrossingPastTheFarBar = true; }

		// ANY PART OF THE BODY ON THE ASPHALT: nose in at 17750, tail out at 22250. The
		// strip must be held, and held OCCUPIED - a reservation is exactly what a landing
		// may preempt, so a reservation here would be an aeroplane a landing could be
		// cleared onto.
		if (T + Half >= 20000.0 - HalfWidth && T - Half <= 20000.0 + HalfWidth)
		{
			bUnheldWhileBodyOnStrip = bUnheldWhileBodyOnStrip || !bHeld;
			for (const FTrafficClaim& Claim : Traffic->GetOccupancy().GetClaims())
			{
				if (Claim.AgentId == Plane && Claim.Resource == Strip && !Claim.bOccupied)
				{
					bReservedWhileBodyOnStrip = true;
				}
			}
		}
		return Q->Follower.Travelled < 25000.0;
	}, Traffic->Rules.MaxSubstepSeconds);

	const FRoadAgent* P = Traffic->FindAgent(Plane);
	UE_LOG(LogAirsideTests, Log,
		TEXT("BarToBarCrossing measured: armed at route %.0f uu (nose reaches the strip at 17250), ")
		TEXT("released at %.0f uu (tail leaves it at 22750); near bar 17000, far bar 23000, next node 40000; ")
		TEXT("stopped at %.0f"),
		ArmedAt, ReleasedAt, P->Follower.Travelled);

	TestTrue(TEXT("the bars did not close the runway for the whole taxi"), bFreeWellBeforeTheBar);
	TestFalse(TEXT("the strip is never unheld while any part of the body is on it"), bUnheldWhileBodyOnStrip);
	TestFalse(TEXT("and never merely RESERVED there, which a landing could preempt"), bReservedWhileBodyOnStrip);

	// THE TWO DISCRIMINATING NUMBERS. Armed no later than the nose reaching the asphalt, and
	// released no earlier than the tail leaving it - the node rule managed neither on this
	// graph, because there is no node between the bars to hang either answer on.
	// One tick's travel of slack on the arming bound and nothing more: the fixture ticks at
	// the substep and the agent cruises at 1000 uu/s (TestAirframes::GroundOnly()'s taxi cap), so a tick
	// is about 34 uu and "armed on the tick the nose reached the asphalt" cannot be measured
	// tighter than that. The release bound needs no such slack downward - a release before
	// the tail is off would be the defect itself - only the one tick upward.
	//
	// THESE WERE 60 AND 100 while the fixture ticked at 0.05, which is coarser than any frame
	// production now hands the model. Measured at the substep the crossing arms ON 17250 and
	// releases 33 uu past 22750 - a tick, exactly - so the wider bounds were tolerance for the
	// sampling rate and not for the rule. Tightening them is the point of ticking finer: at 60
	// the arming could drift more than a whole tick late and this test would still pass.
	const double OneTick = Traffic->Rules.MaxSubstepSeconds * 1000.0 + 1.0;
	TestTrue(FString::Printf(TEXT("armed no later than the nose entered the strip (%.0f, bound 17250 + one tick)"), ArmedAt),
		ArmedAt >= 0.0 && ArmedAt <= 20000.0 - HalfWidth - Half + OneTick);
	TestTrue(FString::Printf(TEXT("released no earlier than the tail left it, and within one tick after (%.0f, want 22750)"), ReleasedAt),
		ReleasedAt >= 20000.0 + HalfWidth + Half && ReleasedAt <= 20000.0 + HalfWidth + Half + OneTick);
	TestFalse(TEXT("and the bar on the way OUT arms nothing"), bCrossingPastTheFarBar);
	TestEqual(TEXT("nothing is left crossing past the far bar"), P->GetCrossingPhase(), ECrossingPhase::None);
	TestFalse(TEXT("with no chain left named"), P->GetCrossingRunway().IsSet());
	TestFalse(TEXT("and the strip free behind it"), Traffic->GetOccupancy().IsHeld(Strip, 0));
	return true;
}

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
	auto Build = [](URoadNetwork& Net, bool bKeepBC, bool bBypass)
	{
		// Sweep everything (a rebuild removes derived edges, then idle derived nodes).
		TArray<FGuidelineEdgeId> Edges;
		for (int32 I = 0; I < Net.GetGuidelineEdges().Num(); ++I) { if (Net.GetGuidelineEdges()[I].bAlive) { FGuidelineEdgeId Id; Id.Index = I; Id.Generation = Net.GetGuidelineEdges()[I].Generation; Edges.Add(Id); } }
		for (const FGuidelineEdgeId& Id : Edges) { Net.RemoveGuidelineEdge(Id); }
		for (int32 I = 0; I < Net.GetGuidelineNodes().Num(); ++I) { if (Net.GetGuidelineNodes()[I].bAlive) { Net.RemoveGuidelineNode(Net.GuidelineNodeIdAt(I)); } }
		const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(20000.0, 0.0));
		const FGuidelineNodeId C = Net.AddGuidelineNode(FVector2D(40000.0, 0.0));
		TestGraph::Join(Net, A, B);
		if (bKeepBC) { TestGraph::Join(Net, B, C); }
		if (bBypass) { const FGuidelineNodeId D = Net.AddGuidelineNode(FVector2D(30000.0, 8000.0)); TestGraph::Join(Net, B, D); TestGraph::Join(Net, D, C); }
		return FRebuiltGraph{ A, C };
	};

	auto Dispatch = [&](URoadNetwork& Net, UGroundTraffic& Traffic, FGuidelineNodeId A, FGuidelineNodeId C)
	{
		const int32 Id = Traffic.DispatchAgent(&Net, M2TrafficRoute(Net, A, C, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
		TickUntil(Traffic, Net, 5.0, [](int32) { return true; });   // a few thousand uu along A->B
		return Id;
	};

	// Case 1: same geometry.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const auto [A, C] = Build(*Net, true, false);
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Van = Dispatch(*Net, *Traffic, A, C);
		const FVector2D Before = Traffic->FindAgent(Van)->LastMotion.Position;
		const FRoutePlan& Was = Traffic->FindAgent(Van)->Follower.Plan;
		const FGuidelineEdgeId OldEdge = Was.Steps[1].Edge;

		// RE-POINTED, NOT REPLACED - spec §6.1, and the thing every other case here would
		// also pass without. An implementation that simply re-searched Start to Goal on each
		// rebuild would satisfy "it arrives at C"; what it could not do is leave the polyline
		// and the step boundaries bit for bit as they were, because a fresh search re-samples
		// the curve and re-bases every EndDistance. These three numbers are that difference.
		const int32 VertsWas = Was.Polyline.Num();
		const int32 EndVertexWas = Was.Steps[1].EndVertex;
		const double EndDistanceWas = Was.Steps[1].EndDistance;

		Build(*Net, true, false);
		TestNull(TEXT("the old handle is dead after the rebuild"), Net->GetGuidelineEdge(OldEdge));
		Traffic->OnGraphRebuilt(*Net);

		const FGraphRebuildSummary Summary = Traffic->GetLastRebuildSummaryForTest();
		TestEqual(TEXT("the van was re-resolved"), Summary.ReResolved, 1);
		TestEqual(TEXT("and NOT replanned: identical geometry costs no search at all"), Summary.Replanned, 0);
		TestEqual(TEXT("nor truncated"), Summary.Truncated, 0);
		TestEqual(TEXT("nor stranded"), Summary.Stranded, 0);

		Traffic->Advance(0.05, Net);
		const FRoadAgent* V = Traffic->FindAgent(Van);
		TestNotNull(TEXT("step 1's edge handle is live again"), Net->GetGuidelineEdge(V->Follower.Plan.Steps[1].Edge));

		// THE NODE THE CURRENT STEP LEAVES FROM. The van is on step 0 here, so StepFromNode
		// answers Plan.Start - and four things read it every tick: the crossing arm, the
		// tail-node claim, RankAt, and a replan's own Query.Start. A dead handle there is
		// silent in all four.
		TestNotNull(TEXT("and so is the node the current step leaves from (Plan.Start at step 0)"),
			Net->GetGuidelineNode(V->Follower.Plan.Start));

		TestEqual(TEXT("the polyline was not re-sampled: same point count"), V->Follower.Plan.Polyline.Num(), VertsWas);
		TestEqual(TEXT("step 1 still ends at the same polyline vertex"), V->Follower.Plan.Steps[1].EndVertex, EndVertexWas);
		TestEqual(TEXT("and at the same route distance, so Travelled still means what it did"),
			V->Follower.Plan.Steps[1].EndDistance, EndDistanceWas, 1e-9);
		TestTrue(TEXT("position moved by at most one tick across the rebuild"), FVector2D::Distance(V->LastMotion.Position, Before) <= 1000.0 * 0.05 + 1.0);
		TickUntil(*Traffic, *Net, 120.0, [&](int32) { return Traffic->FindAgent(Van)->Phase != EAgentPhase::Parked; });
		TestEqual(TEXT("arrives"), Traffic->FindAgent(Van)->Phase, EAgentPhase::Parked);
		TestTrue(TEXT("at C"), FVector2D::Distance(Traffic->FindAgent(Van)->LastMotion.Position, FVector2D(40000.0, 0.0)) < 10.0);
	}
	// Case 2: B->C deleted, bypass added.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const auto [A, C] = Build(*Net, true, false);
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Van = Dispatch(*Net, *Traffic, A, C);
		Build(*Net, false, true);
		Traffic->OnGraphRebuilt(*Net);
		double MaxY = 0.0;
		TickUntil(*Traffic, *Net, 150.0, [&](int32) { MaxY = FMath::Max(MaxY, Traffic->FindAgent(Van)->LastMotion.Position.Y); return Traffic->FindAgent(Van)->Phase != EAgentPhase::Parked; });
		TestEqual(TEXT("arrives over the bypass"), Traffic->FindAgent(Van)->Phase, EAgentPhase::Parked);
		TestTrue(FString::Printf(TEXT("via D (max Y %.0f)"), MaxY), MaxY > 7000.0);
	}
	// Case 3: B->C deleted, nothing replaces it.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const auto [A, C] = Build(*Net, true, false);
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Van = Dispatch(*Net, *Traffic, A, C);
		Build(*Net, false, false);
		Traffic->OnGraphRebuilt(*Net);
		TickUntil(*Traffic, *Net, 120.0, [&](int32) { return Traffic->FindAgent(Van)->Phase != EAgentPhase::Parked; });
		TestEqual(TEXT("stops at the last live node"), Traffic->FindAgent(Van)->Phase, EAgentPhase::Parked);
		TestTrue(TEXT("which is B"), FVector2D::Distance(Traffic->FindAgent(Van)->LastMotion.Position, FVector2D(20000.0, 0.0)) < 10.0);
	}
	// Case 4: an ARRIVING aircraft's TaxiInPlan, which no follower is on yet, re-resolved from
	// step 0. It is the branch the three cases above cannot reach - they all replan a plan
	// under a moving follower - and it is the one that matters most in a game, because a
	// player builds while an aircraft is on final and the route it will vacate onto is derived
	// geometry the next rebuild frees. Rebuilt by the REAL FRoadGuidelineBuilder here, not by
	// hand, so what is measured is the actual event rather than this test's model of it.
	{
		const FAirframe Piper = TestAirframes::Piper();
		const FTestAirport Fixture = FTestAirport::Build(Piper);
		URoadNetwork* Net = Fixture.Net;
		const FVector2D Threshold = Fixture.Threshold;
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Plane = Traffic->DispatchArrival(*Net, Threshold - FVector2D(1000.0, 0.0), Piper, 1.0);
		if (TestTrue(TEXT("the arrival is admitted"), Plane > 0))
		{
			const FRoadAgent* P = Traffic->FindAgent(Plane);
			const int32 Steps = P->TaxiInPlan.Steps.Num();
			const FGuidelineEdgeId OldFirst = Steps > 0 ? P->TaxiInPlan.Steps[0].Edge : FGuidelineEdgeId();
			TestTrue(TEXT("with a taxi-in route to re-resolve"), Steps > 0);
			TestEqual(TEXT("and it is Arriving, so nothing is following that route yet"), P->Phase, EAgentPhase::Arriving);

			TestGraph::Rebuild(*Net);
			TestNull(TEXT("the builder freed the taxi-in route's first handle"), Net->GetGuidelineEdge(OldFirst));

			Traffic->OnGraphRebuilt(*Net);

			P = Traffic->FindAgent(Plane);
			int32 Live = 0;
			for (const FRouteStep& Step : P->TaxiInPlan.Steps)
			{
				if (Net->GetGuidelineEdge(Step.Edge) != nullptr && Net->GetGuidelineNode(Step.To) != nullptr) { ++Live; }
			}
			UE_LOG(LogAirsideTests, Log,
				TEXT("GraphRebuild arrival measured: %d taxi-in steps before the rebuild, %d live after, ")
				TEXT("goal node %s, %.0f uu"),
				Steps, Live, Net->GetGuidelineNode(P->GoalNode) != nullptr ? TEXT("live") : TEXT("DEAD"),
				P->TaxiInPlan.Length);

			// EVERY step, not merely the first: re-resolution walks forward from one node to the
			// next, so a break anywhere leaves the tail naming freed slots and the aircraft
			// vacates onto nothing.
			TestEqual(TEXT("every taxi-in step names a live edge and a live node again"), Live, P->TaxiInPlan.Steps.Num());
			TestEqual(TEXT("and none was lost: the route is the same length in steps"), P->TaxiInPlan.Steps.Num(), Steps);
			TestEqual(TEXT("the plan is still a route"), P->TaxiInPlan.Result, ERouteResult::Found);
			TestNotNull(TEXT("and the goal it will search back to is live"), Net->GetGuidelineNode(P->GoalNode));
			TestEqual(TEXT("nothing stranded it: it is still Arriving"), P->Phase, EAgentPhase::Arriving);
		}
	}
	// Case 5: THE EDGE THE AGENT IS ON IS DELETED - it is STRANDED IN PLACE, spec §6.2's
	// "only an agent whose current step itself is gone is stranded in place".
	//
	// Cases 2 and 3 both fail at a step AHEAD of the agent, and a bypass round those is
	// something the agent can drive to. This one fails at the step UNDER it, and a replan
	// there is not a re-route: the agent keeps Travelled across the splice, so the same route
	// distance is re-read on different geometry and the van TELEPORTS - measured at 3310 uu
	// sideways before this rule, unbounded in principle, and visible in the game as a vehicle
	// jumping across the apron the instant a player deletes a taxiway. Truncating instead
	// would be no better: there is no node behind the agent on a line that still exists.
	// So the agent stops where it is and the player retires it, which is what Stranded means.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const auto [A, C] = Build(*Net, true, false);
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::GroundVehicle),
			TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);

		// PAST B. 30 s at Accel 100 to a cap of 1000 is 25000 uu - comfortably past B at
		// 20000, so the agent is on step 1, and well short of the braking point for C at
		// 37500, so it is still at cruise.
		TickUntil(*Traffic, *Net, 30.0, [](int32) { return true; });
		const FRoadAgent* Before = Traffic->FindAgent(Van);
		if (!TestNotNull(TEXT("the van survived the run up to the rebuild"), Before)) { return false; }
		const double Travelled = Before->Follower.Travelled;
		const FVector2D WasAt = Before->LastMotion.Position;
		if (!TestTrue(FString::Printf(TEXT("the van is ON step 1, past B (%.0f uu)"), Travelled),
			Travelled > 20000.0 && Travelled < 37000.0)) { return false; }

		Build(*Net, false, true);      // B->C gone, B->D->C in its place
		Traffic->OnGraphRebuilt(*Net);

		const FGraphRebuildSummary Summary = Traffic->GetLastRebuildSummaryForTest();

		// ONE TICK BEFORE THE POSITION IS READ: LastMotion is only written by Advance, so
		// reading it straight after the rebuild would report where the agent was BEFORE. The
		// figure this line exists to record is the discontinuity - which is now the thing
		// being ruled out rather than measured.
		Traffic->Advance(0.05, Net);
		const FRoadAgent* V = Traffic->FindAgent(Van);
		if (!TestNotNull(TEXT("a stranded agent is not removed - it stops, it does not vanish"), V))
		{
			return false;
		}
		const double Jump = FVector2D::Distance(WasAt, V->LastMotion.Position);
		UE_LOG(LogAirsideTests, Log,
			TEXT("GraphRebuild under-the-agent measured: was at %.0f uu (%.0f, %.0f), one tick after the ")
			TEXT("rebuild (%.0f, %.0f) - %.0f uu moved; %d replanned, %d truncated, %d stranded"),
			Travelled, WasAt.X, WasAt.Y, V->LastMotion.Position.X, V->LastMotion.Position.Y, Jump,
			Summary.Replanned, Summary.Truncated, Summary.Stranded);

		TestEqual(TEXT("the pavement under it went, so it was STRANDED IN PLACE"), Summary.Stranded, 1);
		TestEqual(TEXT("not replanned onto geometry its own Travelled no longer means"), Summary.Replanned, 0);
		TestEqual(TEXT("and not truncated back behind itself"), Summary.Truncated, 0);

		// ONE TICK'S TRAVEL IS THE WHOLE BUDGET, at the cap of 1000 uu/s: this is the 3310 uu
		// teleport, asserted away. Measured across the rebuild frame and then over five more
		// seconds, per tick, because a replan that fired one frame late would show as a single
		// large step somewhere in that window rather than at the rebuild itself.
		const double PerTick = 1000.0 * 0.05 + 1.0;
		TestTrue(FString::Printf(TEXT("it did not jump: %.0f uu across the rebuild frame, budget %.0f"), Jump, PerTick),
			Jump <= PerTick);

		double MaxStep = 0.0;
		FVector2D Last = V->LastMotion.Position;
		TickUntil(*Traffic, *Net, 5.0, [&](int32)
		{
			const FRoadAgent* Q = Traffic->FindAgent(Van);
			if (Q == nullptr) { return false; }
			MaxStep = FMath::Max(MaxStep, FVector2D::Distance(Last, Q->LastMotion.Position));
			Last = Q->LastMotion.Position;
			return true;
		});
		UE_LOG(LogAirsideTests, Log,
			TEXT("GraphRebuild under-the-agent: max per-tick displacement over the next 5 s %.1f uu"), MaxStep);
		TestTrue(FString::Printf(TEXT("nor in the five seconds after it (max %.1f uu per tick, budget %.0f)"), MaxStep, PerTick),
			MaxStep <= PerTick);
		TestNotNull(TEXT("and it is still there to be retired"), Traffic->FindAgent(Van));
	}
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRebuildSplitUnderTheAgentTest,
	"Airside.Model.Traffic.RebuildSplitUnderTheAgent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRebuildSplitUnderTheAgentTest::RunTest(const FString& Parameters)
{
	// ISSUE #396, REPORTED FROM PLAY 2026-09-28: linking a new exit to a taxiway puts a junction
	// on it, so the edge an aeroplane is taxiing along becomes two edges with a node between them.
	// Re-resolve asked for ONE edge between the step's two old ends, found none, and stranded the
	// aeroplane for good as if "the pavement under it was deleted" - which it was not. The same
	// ground is still there, under the wheels; only the handles changed.
	//
	// THREE SHAPES OF ONE EDIT, all on the step the aircraft is ON (a split AHEAD of it already
	// replanned): the new node ahead of it, the new node behind it, and the node its step leaves
	// from moved back along the same line - the "no live node holds the position its current
	// step starts from" stranding of the same report.
	struct FCase { const TCHAR* Name; double SplitAt; double MovedB; };
	const FCase Cases[] = {
		{ TEXT("split ahead of it"), 30000.0, 20000.0 },
		{ TEXT("split behind it"), 22000.0, 20000.0 },
		{ TEXT("its step's start moved back along the line"), 0.0, 15000.0 },
	};
	for (const FCase& Case : Cases)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
		const FGuidelineNodeId B = TestGraph::Node(*Net, 20000.0, 0.0);
		const FGuidelineNodeId C = TestGraph::Node(*Net, 40000.0, 0.0);
		TestGraph::Join(*Net, A, B);
		TestGraph::Join(*Net, B, C);

		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::Aircraft),
			TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
		if (!TestTrue(FString::Printf(TEXT("%s: dispatched"), Case.Name), Plane > 0)) { return false; }

		// ON STEP 1 (B -> C), between every case's new node and the end.
		TickUntil(*Traffic, *Net, 300.0, [&](int32)
		{
			const FRoadAgent* P = Traffic->FindAgent(Plane);
			return P != nullptr && P->LastMotion.Position.X < 24000.0;
		});
		const FRoadAgent* Before = Traffic->FindAgent(Plane);
		if (!TestNotNull(FString::Printf(TEXT("%s: still under way"), Case.Name), Before)) { return false; }
		const FVector2D WasAt = Before->LastMotion.Position;
		const double WasSpeed = Before->Follower.Speed;
		if (!TestTrue(FString::Printf(TEXT("%s: taxiing on B -> C at %.0f, %.0f uu/s"), Case.Name, WasAt.X, WasSpeed),
			WasAt.X >= 24000.0 && WasAt.X < 29000.0 && WasSpeed > 0.0)) { return false; }

		// THE BUILDER'S WAY: every node and edge removed and re-added with NEW handles - see
		// Airside.Model.Traffic.GraphRebuild - with the one edit the case names.
		TArray<FGuidelineEdgeId> Edges;
		for (int32 I = 0; I < Net->GetGuidelineEdges().Num(); ++I)
		{
			if (Net->GetGuidelineEdges()[I].bAlive) { Edges.Add(Net->GuidelineEdgeIdAt(I)); }
		}
		for (const FGuidelineEdgeId Id : Edges) { Net->RemoveGuidelineEdge(Id); }
		for (int32 I = 0; I < Net->GetGuidelineNodes().Num(); ++I)
		{
			if (Net->GetGuidelineNodes()[I].bAlive) { Net->RemoveGuidelineNode(Net->GuidelineNodeIdAt(I)); }
		}
		const FGuidelineNodeId A2 = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId B2 = Net->AddGuidelineNode(FVector2D(Case.MovedB, 0.0));
		const FGuidelineNodeId C2 = Net->AddGuidelineNode(FVector2D(40000.0, 0.0));
		TestGraph::Join(*Net, A2, B2);
		if (Case.SplitAt > 0.0)
		{
			const FGuidelineNodeId M = Net->AddGuidelineNode(FVector2D(Case.SplitAt, 0.0));
			TestGraph::Join(*Net, B2, M);
			TestGraph::Join(*Net, M, C2);
		}
		else
		{
			TestGraph::Join(*Net, B2, C2);
		}
		Traffic->OnGraphRebuilt(*Net);
		const FGraphRebuildSummary Summary = Traffic->GetLastRebuildSummaryForTest();
		UE_LOG(LogAirsideTests, Log,
			TEXT("RebuildSplitUnderTheAgent %s: %d re-resolved, %d replanned, %d truncated, %d stranded"),
			Case.Name, Summary.ReResolved, Summary.Replanned, Summary.Truncated, Summary.Stranded);
		TestEqual(FString::Printf(TEXT("%s: NOT stranded - the ground under it is still there"), Case.Name),
			Summary.Stranded, 0);

		// NO JUMP, AND IT KEEPS ROLLING: one tick's travel at twice the speed it had is the budget
		// per frame, and it reaches C and parks there - the stand, in the game.
		const double PerTick = FMath::Max(WasSpeed, 1.0) * 2.0 * 0.05 + 1.0;
		double MaxStep = 0.0;
		double SpeedAfter = -1.0;
		FVector2D Last = WasAt;
		TickUntil(*Traffic, *Net, 300.0, [&](int32 Tick)
		{
			const FRoadAgent* Q = Traffic->FindAgent(Plane);
			if (Q == nullptr) { return false; }
			if (Tick == 0) { SpeedAfter = Q->Follower.Speed; }
			MaxStep = FMath::Max(MaxStep, FVector2D::Distance(Last, Q->LastMotion.Position));
			Last = Q->LastMotion.Position;
			return Q->Phase == EAgentPhase::Taxiing;
		});
		const FRoadAgent* After = Traffic->FindAgent(Plane);
		if (!TestNotNull(FString::Printf(TEXT("%s: still there"), Case.Name), After)) { return false; }
		UE_LOG(LogAirsideTests, Log,
			TEXT("RebuildSplitUnderTheAgent %s: max step %.1f uu (budget %.1f), speed %.0f -> %.0f, ended %s at (%.0f, %.0f)"),
			Case.Name, MaxStep, PerTick, WasSpeed, SpeedAfter, *UEnum::GetValueAsString(After->Phase),
			After->LastMotion.Position.X, After->LastMotion.Position.Y);
		TestTrue(FString::Printf(TEXT("%s: no jump (max %.1f uu per tick, budget %.1f)"), Case.Name, MaxStep, PerTick),
			MaxStep <= PerTick);
		TestTrue(FString::Printf(TEXT("%s: it did not stop dead at the rebuild (%.0f -> %.0f uu/s)"), Case.Name, WasSpeed, SpeedAfter),
			SpeedAfter >= WasSpeed * 0.5);
		TestEqual(FString::Printf(TEXT("%s: it parks"), Case.Name), After->Phase, EAgentPhase::Parked);
		TestTrue(FString::Printf(TEXT("%s: AT C, where it was going (%.0f uu off)"), Case.Name,
			FVector2D::Distance(After->LastMotion.Position, FVector2D(40000.0, 0.0))),
			FVector2D::Distance(After->LastMotion.Position, FVector2D(40000.0, 0.0)) < 100.0);
	}
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficStrandedIsNotParkedTest,
	"Airside.Model.Traffic.StrandedIsNotParked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficStrandedIsNotParkedTest::RunTest(const FString& Parameters)
{
	// ISSUE #396: a stranded plan counts as ARRIVED (FRouteFollower::HasArrived: not drivable), so
	// an aeroplane stranded half way along a taxiway went to Parked on the spot with its GoalNode
	// still the stand - and the ops layer, which reads GoalNode as the stand it parked at, fuelled
	// an empty stand and later pushed the aeroplane back from it: the visible jump. A stranding is
	// its own phase, and nothing hears Parked.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 20000.0, 0.0);
	TestGraph::Join(*Net, A, B);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, B, ETraversalClass::Aircraft),
		TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }
	TArray<EAgentPhase> Heard;
	Traffic->OnAgentPhaseChanged.AddLambda([&](const FAgentTransition& T)
	{
		if (T.AgentId == Plane) { Heard.Add(T.To); }
	});

	TickUntil(*Traffic, *Net, 5.0, [](int32) { return true; });
	const FVector2D WasAt = Traffic->FindAgent(Plane)->LastMotion.Position;
	if (!TestTrue(TEXT("stranded"), FGroundTrafficTestAccess(*Traffic).Strand(Plane))) { return false; }
	TickUntil(*Traffic, *Net, 3.0, [](int32) { return true; });

	const FRoadAgent* P = Traffic->FindAgent(Plane);
	if (!TestNotNull(TEXT("a stranded agent stays until it is retired"), P)) { return false; }
	TestEqual(TEXT("its phase says STRANDED"), P->Phase, EAgentPhase::Stranded);
	TestFalse(TEXT("and nothing ever heard Parked"), Heard.Contains(EAgentPhase::Parked));
	TestTrue(TEXT("and what was heard was Stranded"), Heard.Contains(EAgentPhase::Stranded));
	TestTrue(TEXT("it stands where it stopped, well short of its goal"),
		FVector2D::Distance(P->LastMotion.Position, WasAt) < 100.0
			&& FVector2D::Distance(P->LastMotion.Position, FVector2D(20000.0, 0.0)) > 1000.0);
	TestEqual(TEXT("it cannot be told to depart - it is at no stand"),
		Traffic->DepartAgent(Plane, *Net), EDepartureRefusal::NotParked);
	TestTrue(TEXT("and the player can still retire it"), Traffic->RetireAgent(Plane));
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficDeadPlanReleasesTest,
	"Airside.Model.Traffic.DeadPlanReleases",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficDeadPlanReleasesTest::RunTest(const FString& Parameters)
{
	// A TAXIING AGENT WHOSE PLAN GOES BAD UNDER IT MUST GIVE EVERYTHING BACK, that tick.
	//
	// THE SEAM THIS EXISTS FOR is ClaimAhead's dead-plan branch (UGroundTraffic::
	// ReleaseForDeadPlan). Every other Airside.Model.Traffic fixture drives agents whose
	// plans stay valid, so before this test the branch had NO reader at all: deleting its
	// release, or its `return`, or the branch itself, left 115 tests green while an agent's
	// claims sat in the table for the rest of the session, blocking a junction nobody could
	// ever be told was free.
	//
	// ONE TICK AND NOT TWO, deliberately. An invalid plan makes FRouteFollower::HasArrived
	// true, so the agent becomes Parked at the END of the very tick that strands it, and
	// from the NEXT tick onwards ClaimAhead takes its non-Taxiing branch, which releases
	// everything anyway. Asserting after two ticks would therefore pass with the dead-plan
	// branch deleted - the bug would simply have been one frame long, and one frame is
	// enough for another agent to be refused a node this one no longer wants.
	//
	// WHAT IT DOES NOT CATCH, said out loud: substituting HoldRunwayOnly for
	// ReleaseForDeadPlan. The two differ only in whether RunwayHeld is kept and re-claimed,
	// and RunwayHeld is filled at the arrival/departure HANDOVER in Advance, so it is always
	// empty on a Taxiing agent. For every reachable state the substitution is observationally
	// identical, which makes it a rename rather than a defect.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 0.0, 20000.0);
	const FGuidelineEdgeId AB = TestGraph::Join(*Net, A, B);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, B, ETraversalClass::GroundVehicle),
		TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("dispatched"), Van > 0)) { return false; }

	// Under way, and holding: the edge it is on, plus the node it left while its tail is
	// still within half a footprint of it. Two seconds is well short of the 20 km run.
	TickUntil(*Traffic, *Net, 2.0, [&](int32) { return true; });

	auto ClaimsHeldBy = [&](int32 AgentId)
	{
		int32 Count = 0;
		for (const FTrafficClaim& Claim : Traffic->GetOccupancy().GetClaims())
		{
			Count += Claim.AgentId == AgentId ? 1 : 0;
		}
		return Count;
	};

	const int32 HeldBefore = ClaimsHeldBy(Van);
	UE_LOG(LogAirsideTests, Log,
		TEXT("DeadPlanReleases measured: %d claim(s) held at %.0f uu before the plan died"),
		HeldBefore, Traffic->FindAgent(Van)->Follower.Travelled);

	if (!TestTrue(TEXT("the van is holding something before its plan dies"), HeldBefore > 0))
	{
		return false;
	}
	TestTrue(TEXT("including the edge it is driving on"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfEdge(AB), /*ExcludingAgent=*/0));
	TestTrue(TEXT("and the node it left, while its tail is still in it"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(A), /*ExcludingAgent=*/0));

	// THE PAVEMENT GOES AWAY UNDER IT. Strand sets Result only - the follower keeps
	// its polyline and its distance, so the agent is exactly where it was and the ONLY thing
	// that has changed is that its plan no longer describes the airport.
	if (!TestTrue(TEXT("stranded"), FGroundTrafficTestAccess(*Traffic).Strand(Van))) { return false; }
	Traffic->Advance(0.05, Net);

	const FRoadAgent* V = Traffic->FindAgent(Van);
	if (!TestNotNull(TEXT("a stranded agent is not removed - it stops, it does not vanish"), V))
	{
		return false;
	}

	// NOTHING, and that is still the right answer for a VAN: what it gave back is every
	// GUIDELINE it held, and a van on a taxiway holds nothing else. An aircraft standing on a
	// runway does - see the second half of this test, which is why the release is now
	// ReleaseGuidelineClaimsOf rather than ReleaseAll.
	TestEqual(TEXT("and the table holds NOTHING for it, in the same tick"), ClaimsHeldBy(Van), 0);
	TestFalse(TEXT("the edge it was on is free"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfEdge(AB), /*ExcludingAgent=*/0));
	TestFalse(TEXT("and so is the node behind it"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(A), /*ExcludingAgent=*/0));

	// THE ARBITRATION FIELDS GO WITH THE CLAIMS. A stranded agent still naming a blocker
	// would feed the deadlock resolver's wait-for graph an edge out of an agent that is not
	// waiting for anything - the same reason the non-Taxiing branch clears them.
	TestEqual(TEXT("waiting on nobody"), V->GetWaitingOn(), 0);
	TestEqual(TEXT("blocked on no step"), V->GetBlockedStep(), INDEX_NONE);
	TestTrue(TEXT("and under no cap it could drive against"), V->GetStopWithin() >= TNumericLimits<double>::Max());

	// AND THE HALF A DEAD PLAN SAYS NOTHING ABOUT: AN AIRCRAFT'S BODY.
	//
	// A plan is a route, not a position. An aeroplane whose plan dies while it is standing on
	// the centreline is still standing on the centreline, and ArrivalPlanner::Plan reads this
	// table directly at DispatchArrival, BETWEEN ticks - so releasing its strip claim shows
	// the runway free for as long as it takes the player to press 7, and a landing is cleared
	// onto an aeroplane nobody can see is there. ReleaseAll did exactly that at both stranding
	// sites; ReleaseGuidelineClaimsOf gives back the lines and keeps the surface.
	//
	// NO PHANTOM HOLDER: the strip is held by the crossing aircraft itself and by nothing
	// else, so what the planner refuses on is this agent's own claim.
	{
		URoadNetwork* Cross = NewObject<URoadNetwork>(GetTransientPackage());
		const FCrossingFixture Crossing = FCrossingFixture::Build(*Cross);
		const FRoadSegmentId RunwaySeg = Crossing.Strip;
		const FGuidelineNodeId S = Crossing.S;
		const FGuidelineNodeId N = Crossing.N;

		UGroundTraffic* Air = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Plane = Air->DispatchAgent(Cross, M2TrafficRoute(*Cross, S, N, ETraversalClass::Aircraft),
			TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
		if (TestTrue(TEXT("the crossing aircraft is dispatched"), Plane > 0))
		{
			TickUntil(*Air, *Cross, 120.0, [&](int32)
			{
				const FRoadAgent* Q = Air->FindAgent(Plane);
				return Q != nullptr && Q->GetCrossingPhase() != ECrossingPhase::OnStrip;
			});
			const FRoadAgent* P = Air->FindAgent(Plane);
			if (TestNotNull(TEXT("it is still under way"), P)
				&& TestEqual(TEXT("and its body is ON the strip"), P->GetCrossingPhase(), ECrossingPhase::OnStrip))
			{
				const FTrafficResource Strip = FTrafficResource::OfSurface(RunwaySeg);
				TestTrue(TEXT("so it holds the runway before its plan dies"),
					Air->GetOccupancy().IsHeld(Strip, /*ExcludingAgent=*/0));

				TestTrue(TEXT("stranded mid-crossing"), FGroundTrafficTestAccess(*Air).Strand(Plane));
				Air->Advance(0.05, Cross);

				UE_LOG(LogAirsideTests, Log,
					TEXT("DeadPlanReleases crossing measured: %d claim(s) left in the table after the ")
					TEXT("plan died, strip %s"),
					Air->GetOccupancy().GetClaims().Num(),
					Air->GetOccupancy().IsHeld(Strip, 0) ? TEXT("HELD") : TEXT("free"));

				TestTrue(TEXT("it STILL holds the strip it is standing on, one tick later"),
					Air->GetOccupancy().IsHeld(Strip, /*ExcludingAgent=*/0));

				// THE CONSUMER, not just the table: this is the question a player pressing 7
				// asks between ticks, and the only one that matters.
				const FArrivalPlan Landing = ArrivalPlanner::Plan(*Cross, FVector2D(-51000.0, 0.0),
					TestAirframes::Piper(), &Air->GetOccupancy());
				TestEqual(TEXT("and a landing offered in that same frame is refused: RunwayOccupied"),
					Landing.Why, EArrivalRefusal::RunwayOccupied);

				// AND IT KEEPS IT AFTER IT STOPS, WHICH IS THE FRAME THAT MATTERED.
				//
				// An invalid plan makes FRouteFollower::HasArrived true, so this agent is
				// Stranded (Parked before issue #396) by the end of the very tick that stranded it, and from the next tick
				// on it takes ClaimAhead's non-Taxiing branch. That branch used to hold
				// RunwayHeld and nothing else - and RunwayHeld is empty on an agent that
				// never landed - so the strip it is physically standing on came free one tick
				// after the fix above kept it. Holding for one tick and then letting go is not
				// holding: the player's next press of 7 is not on that frame.
				TickUntil(*Air, *Cross, 3.0, [](int32) { return true; });
				const FRoadAgent* Parked = Air->FindAgent(Plane);
				if (TestNotNull(TEXT("it is still there three seconds later"), Parked))
				{
					UE_LOG(LogAirsideTests, Log,
						TEXT("DeadPlanReleases parked measured: phase %s, crossing phase %d, strip %s"),
						*UEnum::GetValueAsString(Parked->Phase), static_cast<int32>(Parked->GetCrossingPhase()),
						Air->GetOccupancy().IsHeld(Strip, 0) ? TEXT("HELD") : TEXT("free"));

					TestEqual(TEXT("and it is stranded where it stood - not parked, it is at no stand (#396)"), Parked->Phase, EAgentPhase::Stranded);
					TestTrue(TEXT("a STRANDED aircraft still holds the strip its body is on"),
						Air->GetOccupancy().IsHeld(Strip, /*ExcludingAgent=*/0));

					const FArrivalPlan Later = ArrivalPlanner::Plan(*Cross, FVector2D(-51000.0, 0.0),
						TestAirframes::Piper(), &Air->GetOccupancy());
					TestEqual(TEXT("so a landing is still refused: RunwayOccupied, until the player retires it"),
						Later.Why, EArrivalRefusal::RunwayOccupied);
				}

				// AND RETIRING IT IS WHAT GIVES THE RUNWAY BACK - the one path that should,
				// and the reason the hold above is not a leak.
				TestTrue(TEXT("retired"), Air->RetireAgent(Plane));
				TestFalse(TEXT("and the strip is free again"),
					Air->GetOccupancy().IsHeld(Strip, /*ExcludingAgent=*/0));
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficDepartureMeetsArrivalOnTaxiwayTest,
	"Airside.Model.Traffic.DepartureMeetsArrivalOnTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficDepartureMeetsArrivalOnTaxiwayTest::RunTest(const FString& Parameters)
{
	// THE PLAY REPORT OF 2026-09-07: a parked aircraft is sent to depart while the next
	// arrival is taxiing in on the same taxiway, and the two drive through each other. On
	// the DERIVED graph - builder taxiway, anchor-link lead-ins, arrival planner, departure
	// planner - not the hand-joined edge HeadOnStops uses, because that one stops.
	const FAirframe Piper = TestAirframes::Piper();
	// TWO STANDS off the one taxiway, so the second arrival has somewhere to go while the
	// first is parked - the standard shape, not the -24000 taxiway/-12000,-18000 stands this
	// fixture had drifted to before #101: nothing here asserts on those exact figures.
	const FTestAirport Fixture = FTestAirport::Build(Piper, { .StandCount = 2 });
	URoadNetwork* Net = Fixture.Net;
	const FVector2D Threshold = Fixture.Threshold;

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FVector2D Approach = Threshold - FVector2D(1000.0, 0.0);

	const int32 First = Traffic->DispatchArrival(*Net, Approach, Piper, 1.0);
	if (!TestTrue(TEXT("first arrival admitted"), First > 0)) { return false; }
	TickUntil(*Traffic, *Net, 400.0, [&](int32)
	{
		const FRoadAgent* A = Traffic->FindAgent(First);
		return A != nullptr && A->Phase != EAgentPhase::Parked;
	});
	{
		const FRoadAgent* A = Traffic->FindAgent(First);
		if (!TestTrue(TEXT("first arrival parked"), A != nullptr && A->Phase == EAgentPhase::Parked)) { return false; }
	}

	const int32 Second = Traffic->DispatchArrival(*Net, Approach, Piper, 1.0);
	if (!TestTrue(TEXT("second arrival admitted"), Second > 0)) { return false; }
	TickUntil(*Traffic, *Net, 200.0, [&](int32)
	{
		const FRoadAgent* B = Traffic->FindAgent(Second);
		return B != nullptr && B->Phase != EAgentPhase::Taxiing;
	});
	{
		const FRoadAgent* B = Traffic->FindAgent(Second);
		if (!TestTrue(TEXT("second arrival is taxiing in"), B != nullptr && B->Phase == EAgentPhase::Taxiing)) { return false; }
	}

	// THIS DEPARTURE DRIVES STRAIGHT OUT. StraightOutDegrees is the angle within which an
	// aeroplane's way out is already ahead of it and no manoeuvre is needed; 360 makes that
	// true of every stand, so this fixture takes exactly the path it took before pushback
	// existed.
	//
	// NEUTRALISED RATHER THAN RETUNED, and the difference matters. Making the push merely
	// FAST does not restore this test: the aeroplane still physically reverses seventy-six
	// metres down its lead-in, which moves where it meets the arrival however quickly it gets
	// there. Trying to keep the encounter by shortening the manoeuvre chased that for three
	// rounds and never worked.
	//
	// WHAT THIS TEST IS FOR is two aeroplanes meeting on one taxiway and keeping separation.
	// The manoeuvre has its own tests - Airside.Model.PushbackRun, PushbackClearance,
	// PushbackStraightOut, AgentPushbackComposition - and a departure that meets an arrival
	// WITH a push in front of it is still uncovered. That gap is named in the branch's notes
	// rather than papered over here.
	Traffic->Rules.StraightOutDegrees = 360.0;
	const EDepartureRefusal Why = Traffic->DepartAgent(First, *Net);
	if (!TestTrue(FString::Printf(TEXT("departure accepted (%d)"), static_cast<int32>(Why)), Why == EDepartureRefusal::None)) { return false; }

	double MinSeparation = TNumericLimits<double>::Max();
	int32 TicksBothTaxiing = 0;
	int32 TicksFirstWaited = 0;
	int32 TicksSecondWaited = 0;
	TickUntil(*Traffic, *Net, 300.0, [&](int32)
	{
		const FRoadAgent* A = Traffic->FindAgent(First);
		const FRoadAgent* B = Traffic->FindAgent(Second);
		if (A == nullptr || B == nullptr) { return false; }
		// KEEP TICKING WHILE EITHER IS STILL UNDER WAY, and a PUSH is under way. This read
		// "Phase == Taxiing" on both sides, which was the whole truth before a departure
		// manoeuvred off its stand first: the loop saw the departing aeroplane leave Taxiing,
		// concluded it was finished, and stopped before the two ever met - zero ticks both
		// taxiing, however fast the push was made. IsOnRoute is the question that was meant.
		if (A->Phase != EAgentPhase::Taxiing || B->Phase != EAgentPhase::Taxiing) { return B->IsOnRoute() || A->IsOnRoute(); }
		++TicksBothTaxiing;
		TicksFirstWaited += A->GetWaitingOn() != 0 ? 1 : 0;
		TicksSecondWaited += B->GetWaitingOn() != 0 ? 1 : 0;
		MinSeparation = FMath::Min(MinSeparation, FVector2D::Distance(A->LastMotion.Position, B->LastMotion.Position));
		return true;
	});

	UE_LOG(LogAirsideTests, Log,
		TEXT("DepartureMeetsArrivalOnTaxiway measured: min separation %.0f uu over %d ticks both taxiing; ")
		TEXT("departure waited %d tick(s), arrival waited %d tick(s)"),
		MinSeparation, TicksBothTaxiing, TicksFirstWaited, TicksSecondWaited);

	TestTrue(TEXT("they shared the taxiway for a while"), TicksBothTaxiing > 0);
	TestTrue(FString::Printf(TEXT("never closer than one footprint (%.0f)"), MinSeparation),
		MinSeparation >= Traffic->Rules.AircraftFootprint - 1.0);
	TestTrue(TEXT("somebody was made to wait"), TicksFirstWaited + TicksSecondWaited > 0);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReservationCycleYieldsTest,
	"Airside.Model.Traffic.ReservationCycleYields",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReservationCycleYieldsTest::RunTest(const FString& Parameters)
{
	// samples/routing2.png, 2026-09-07. A 392 uu stub of taxiway between two junctions;
	// one aircraft closing on it from each side, both still ~2000 uu short. Green (here:
	// First) holds the far node of the stub - its junction's arms hug each other, so the
	// node's reach asks for it long before the window reaches the stub - and Second holds
	// the stub. Each is refused the other's RESERVATION. Nobody is in anybody's way, yet the
	// wait-for graph is a two-member cycle, and the resolver sent the later aircraft round
	// the whole loop. Here there is NO alternative route at all: the old resolver logged "no
	// member can turn" and both waited for ever. The yield rule lets one of them drop what
	// it has not reached, the other passes, and both arrive without a replan.
	//
	// THE GRAPH. Stub n30 (0,0) - n31 (0,-392). From n31, two arms leave southward as tangent
	// arcs (control (0, -392-R)) and curve apart to E1 and W1 - a junction's turn paths, which
	// share the arm's tangent at the node. Their reach at n31 is what asks for the node
	// early and stages the cycle. R IS SMALL, 600, so the arcs are ~940 uu and the reach is
	// capped there: First's stop point for the node-30 refusal is 1315 short of n31, and with
	// the 2582 uu arcs of NodeReach.TangentArc (reach 2500) it rolled INSIDE n31's zone, its
	// claim became an occupancy, and the resolver rightly said a body was in the way - on
	// those arcs Second's exit really would pass within a footprint of First. The junction in
	// routing2.png has ~1200 uu turn paths and the aircraft stopped 1956 short, outside.
	// From n30: a spur east to NR (1900,0), Second's start, and the line north to NN
	// (0,8000), First's goal.
	const double R = 600.0;
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId N30 = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId N31 = TestGraph::Node(*Net, 0.0, -392.0);
	const FGuidelineNodeId NN = TestGraph::Node(*Net, 0.0, 8000.0);
	const FGuidelineNodeId NR = TestGraph::Node(*Net, 1900.0, 0.0);
	const FGuidelineNodeId E1 = TestGraph::Node(*Net, R, -392.0 - R);
	const FGuidelineNodeId W1 = TestGraph::Node(*Net, -R, -392.0 - R);
	const FGuidelineNodeId E = TestGraph::Node(*Net, R + 8000.0, -392.0 - R);
	const FGuidelineNodeId W = TestGraph::Node(*Net, -R - 8000.0, -392.0 - R);
	const FGuidelineEdgeId Stub = TestGraph::Join(*Net, N30, N31);
	TestGraph::Join(*Net, N30, NN);
	TestGraph::Join(*Net, N30, NR);
	TestGraph::Join(*Net, E1, E);
	TestGraph::Join(*Net, W1, W);
	for (const FGuidelineNodeId Arm : { E1, W1 })
	{
		FGuidelineEdge Arc;
		Arc.A = N31; Arc.B = Arm;
		Arc.Control = FVector2D(0.0, -392.0 - R);
		Arc.AllowedTraffic = FTrafficMask::All();
		Net->AddGuidelineEdge(MoveTemp(Arc));
	}

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	// FIRST goes W -> W1 -> n31 -> stub -> n30 -> NN. Run it until it holds n31 and not yet
	// the stub: the reach has asked for the node, the window has not reached the edge.
	const int32 First = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, W, NN, ETraversalClass::Aircraft), TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("first dispatched"), First > 0)) { return false; }
	const int32 FirstSteps = Traffic->FindAgent(First)->Follower.Plan.Steps.Num();
	bool bStaged = false;
	TickUntil(*Traffic, *Net, 60.0, [&](int32)
	{
		int32 Holder = 0;
		const bool bNodeHeld = Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(N31), 0, &Holder) && Holder == First;
		const bool bStubHeld = Traffic->GetOccupancy().IsHeld(FTrafficResource::OfEdge(Stub), 0);
		bStaged = bNodeHeld && !bStubHeld;
		return !bStaged;
	});
	if (!TestTrue(TEXT("staged: First holds n31 and nobody holds the stub"), bStaged)) { return false; }

	// SECOND starts 1900 uu east of n30, at rest: its window (F/2 + G = 2000) covers n30 and
	// the near end of the stub, and stops 392 short of n31 - so it reserves the stub and
	// never asks for the node First already holds. Route NR -> n30 -> stub -> n31 -> E1 -> E.
	const int32 Second = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, NR, E, ETraversalClass::Aircraft), TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("second dispatched"), Second > 0)) { return false; }
	const int32 SecondSteps = Traffic->FindAgent(Second)->Follower.Plan.Steps.Num();

	bool bCycleFormed = false;
	int32 CycleTick = -1;
	TickUntil(*Traffic, *Net, 200.0, [&](int32 Tick)
	{
		const FRoadAgent* A = Traffic->FindAgent(First);
		const FRoadAgent* B = Traffic->FindAgent(Second);
		if (A == nullptr || B == nullptr) { return false; }
		if (!bCycleFormed && A->GetWaitingOn() == Second && B->GetWaitingOn() == First)
		{
			bCycleFormed = true;
			CycleTick = Tick;
		}
		return A->Phase != EAgentPhase::Parked || B->Phase != EAgentPhase::Parked;
	});

	const FRoadAgent* A = Traffic->FindAgent(First);
	const FRoadAgent* B = Traffic->FindAgent(Second);
	UE_LOG(LogAirsideTests, Log,
		TEXT("ReservationCycleYields measured: cycle formed at tick %d; yields %d (last yielder %d, Second = %d); replans %d; ")
		TEXT("cycles %d; First %s %d step(s) (was %d), Second %s %d step(s) (was %d)"),
		CycleTick, Traffic->GetYieldsForTest(), Traffic->GetLastYieldedAgentForTest(), Second, Traffic->GetLastResolvedAgentForTest(),
		Traffic->GetCyclesDetectedForTest(),
		A ? *UEnum::GetValueAsString(A->Phase) : TEXT("gone"), A ? A->Follower.Plan.Steps.Num() : 0, FirstSteps,
		B ? *UEnum::GetValueAsString(B->Phase) : TEXT("gone"), B ? B->Follower.Plan.Steps.Num() : 0, SecondSteps);

	TestTrue(TEXT("the reservation cycle formed: each waited on the other"), bCycleFormed);
	TestEqual(TEXT("it was settled by exactly one yield"), Traffic->GetYieldsForTest(), 1);
	TestEqual(TEXT("by the later aircraft"), Traffic->GetLastYieldedAgentForTest(), Second);
	TestEqual(TEXT("and nobody replanned"), Traffic->GetLastResolvedAgentForTest(), 0);
	TestTrue(TEXT("both arrived"), A != nullptr && B != nullptr && A->Phase == EAgentPhase::Parked && B->Phase == EAgentPhase::Parked);
	TestTrue(TEXT("on the routes they were cleared for"),
		A != nullptr && B != nullptr && A->Follower.Plan.Steps.Num() == FirstSteps && B->Follower.Plan.Steps.Num() == SecondSteps);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReplanTurnsOverFreeRunwayEndTest,
	"Airside.Model.Traffic.ReplanTurnsOverFreeRunwayEnd",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReplanTurnsOverFreeRunwayEndTest::RunTest(const FString& Parameters)
{
	// samples/routing.png, 2026-09-07: an aircraft sent round the whole taxiway loop when
	// the loop through a free runway threshold beside it was a turnaround it could have
	// taken. The resolver's replan asked the search to avoid every runway edge; it now asks
	// it to avoid a runway somebody ELSE holds. This pins that query, through the same
	// ReplanAt the resolver and the rebuild path call.
	//
	//   W --- A --- B --- E        taxiway; the agent is on it, and A->B is what gets banned
	//         |     |
	//        R1 === R2             the strip, derived from a runway segment
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RoadR1 = Net->AddNode(FVector2D(0.0, -1500.0));
	const FRoadNodeId RoadR2 = Net->AddNode(FVector2D(4000.0, -1500.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(RoadR1, RoadR2, Runway);

	const FGuidelineNodeId W = TestGraph::Node(*Net, -5000.0, 0.0);
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 4000.0, 0.0);
	const FGuidelineNodeId E = TestGraph::Node(*Net, 9000.0, 0.0);
	const FGuidelineNodeId R1 = TestGraph::Node(*Net, 0.0, -1500.0);
	const FGuidelineNodeId R2 = TestGraph::Node(*Net, 4000.0, -1500.0);
	TestGraph::Join(*Net, W, A);
	const FGuidelineEdgeId AB = TestGraph::Join(*Net, A, B);
	TestGraph::Join(*Net, B, E);
	TestGraph::Join(*Net, A, R1);
	TestGraph::Join(*Net, R2, B);
	{
		FGuidelineEdge Along;
		Along.A = R1; Along.B = R2;
		Along.Control = FVector2D(2000.0, -1500.0);
		Along.AllowedTraffic = FTrafficMask::All();
		Along.DerivedFrom = Strip;
		Net->AddGuidelineEdge(MoveTemp(Along));
	}

	auto UsesStrip = [&](const FRoutePlan& Plan)
	{
		for (const FRouteStep& Step : Plan.Steps)
		{
			const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Step.Edge);
			if (Edge != nullptr && Edge->DerivedFrom == Strip) { return true; }
		}
		return false;
	};

	// FREE: the replan from A with A->B banned goes A -> R1 -> R2 -> B -> E.
	{
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, W, E, ETraversalClass::Aircraft), TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
		if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }
		const FRoadAgent* Before = Traffic->FindAgent(Plane);
		TestTrue(TEXT("the cleared route is the taxiway, three steps, no strip"), Before->Follower.Plan.Steps.Num() == 3 && !UsesStrip(Before->Follower.Plan));

		const bool bReplanned = FGroundTrafficTestAccess(*Traffic).ReplanAt(Plane, *Net, /*SpliceStep=*/1, AB);
		const FRoadAgent* After = Traffic->FindAgent(Plane);
		UE_LOG(LogAirsideTests, Log, TEXT("ReplanTurnsOverFreeRunwayEnd measured: free strip - replanned %d, %d step(s), uses strip %d, %.0f uu"),
			bReplanned, After->Follower.Plan.Steps.Num(), UsesStrip(After->Follower.Plan), After->Follower.Plan.Length);
		TestTrue(TEXT("a free runway end is a turnaround: the replan succeeds"), bReplanned);
		TestTrue(TEXT("and goes round the end of the strip"), UsesStrip(After->Follower.Plan));
		TestTrue(TEXT("W -> A -> R1 -> R2 -> B -> E"), After->Follower.Plan.Steps.Num() == 5 && After->Follower.Plan.Steps[1].To == R1);
	}

	// HELD BY SOMEBODY ELSE: a phantom reservation on the strip - a departure at its bar -
	// and the same replan has nowhere to go. The old ban and the new rule agree here; the
	// difference is only the case above.
	{
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		FTrafficClaim Bar; Bar.AgentId = 99; Bar.Resource = FTrafficResource::OfSurface(Strip); Bar.bOccupied = false;
		FTrafficClaim Blocker;
		Traffic->OccupancyForTest().TryClaim(Bar, Blocker);
		const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, W, E, ETraversalClass::Aircraft), TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
		if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }
		const bool bReplanned = FGroundTrafficTestAccess(*Traffic).ReplanAt(Plane, *Net, /*SpliceStep=*/1, AB);
		const FRoadAgent* After = Traffic->FindAgent(Plane);
		UE_LOG(LogAirsideTests, Log, TEXT("ReplanTurnsOverFreeRunwayEnd measured: held strip - replanned %d, %d step(s), uses strip %d"),
			bReplanned, After->Follower.Plan.Steps.Num(), UsesStrip(After->Follower.Plan));
		TestFalse(TEXT("a strip somebody else holds is not a turnaround: the replan fails"), bReplanned);
		TestTrue(TEXT("and the agent keeps the route it had"), After->Follower.Plan.Steps.Num() == 3 && !UsesStrip(After->Follower.Plan));
	}
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A LONG FRAME IS SPLIT INTO BOUNDED STEPS, so what the model does stops depending on how
 * fast the player is running the game.
 *
 * UAirsideTraffic hands UGroundTraffic the frame time MULTIPLIED by the speed multiplier,
 * so at x8 a healthy 16 ms frame arrives as 133 ms of simulation. Taken in one go, an agent
 * covers 133 ms of ground in a single jump - and a jump that overshoots the bend it is
 * turning onto puts it off the guideline, to be pulled back on the next frame. That is the
 * rubber-banding reported from play: absent at x1, visible at x2, worse above it, which is
 * the signature of a step that scales with the multiplier.
 *
 * PINS THE SEAM, NOT THE SYMPTOM. Advance splitting the delta is not observable from
 * outside - there is no counter to read - so this measures the only thing that matters
 * about it: a long frame must land the agent where the short frames would have landed it.
 * Two identical agents are run over identical total time, one fed whole frames and one fed
 * substep-sized ones, and they must agree EXACTLY. 0.09 is chosen because it is three whole
 * substeps at 1/30 and divides exactly, so the two runs take the same sequence of steps;
 * an equality rather than a tolerance is what makes an unwired split fail this.
 *
 * The third agent is the control, and it is why the equality has teeth: with the split
 * disabled it takes the frame in one step and lands somewhere else. Without it, deleting
 * the split would leave the first assertion passing for a fixture too gentle to diverge.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficSubstepTest,
	"Airside.Model.Traffic.LongFrameIsSplitIntoBoundedSteps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficSubstepTest::RunTest(const FString& Parameters)
{
	// A right-angle turn, because a bend is where step size shows. On a straight the error is
	// only the integration of the acceleration; through a corner it is the overshoot as well.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId W = TestGraph::Node(*Net, -20000.0, 0.0);
	const FGuidelineNodeId J = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId N = TestGraph::Node(*Net, 0.0, 20000.0);
	TestGraph::Join(*Net, W, J);
	TestGraph::Join(*Net, J, N);

	// One network for all three: it is read-only while agents advance, and sharing it removes
	// any chance the runs differ because their geometry did.
	int32 Ids[3] = { 0, 0, 0 };
	UGroundTraffic* Runs[3] = { nullptr, nullptr, nullptr };
	const double Longest[3] = { 1.0 / 30.0, 1.0 / 30.0, 1000.0 };
	for (int32 Which = 0; Which < 3; ++Which)
	{
		Runs[Which] = NewObject<UGroundTraffic>(GetTransientPackage());
		Runs[Which]->Rules.MaxSubstepSeconds = Longest[Which];
		Ids[Which] = Runs[Which]->DispatchAgent(Net,
			M2TrafficRoute(*Net, W, N, ETraversalClass::Aircraft),
			TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	}
	if (!TestTrue(TEXT("all three dispatched"), Ids[0] > 0 && Ids[1] > 0 && Ids[2] > 0))
	{
		return false;
	}

	const double Frame = 0.09;        // a 60 Hz frame at x5, and three whole substeps
	const double Substep = Frame / 3.0;
	for (int32 Tick = 0; Tick < 300; ++Tick)
	{
		Runs[0]->Advance(Substep, Net);
		Runs[0]->Advance(Substep, Net);
		Runs[0]->Advance(Substep, Net);
		Runs[1]->Advance(Frame, Net);
		Runs[2]->Advance(Frame, Net);
	}

	const FRoadAgent* Fine = Runs[0]->FindAgent(Ids[0]);
	const FRoadAgent* Split = Runs[1]->FindAgent(Ids[1]);
	const FRoadAgent* Unsplit = Runs[2]->FindAgent(Ids[2]);
	if (!TestTrue(TEXT("all three still exist"), Fine && Split && Unsplit)) { return false; }

	const double SplitGap = FMath::Abs(Split->Follower.Travelled - Fine->Follower.Travelled);
	const double UnsplitGap = FMath::Abs(Unsplit->Follower.Travelled - Fine->Follower.Travelled);
	UE_LOG(LogAirsideTests, Log,
		TEXT("Substep measured: travelled fine %.3f, split %.3f, unsplit %.3f; ")
		TEXT("split gap %.4f uu, unsplit gap %.4f uu"),
		Fine->Follower.Travelled, Split->Follower.Travelled, Unsplit->Follower.Travelled,
		SplitGap, UnsplitGap);

	TestTrue(TEXT("the agent moved at all, so the comparison means something"),
		Fine->Follower.Travelled > 1000.0);
	TestTrue(*FString::Printf(
		TEXT("a whole frame split into substeps lands exactly where the substeps land (%.6f uu apart)"),
		SplitGap), SplitGap <= 0.001);
	TestTrue(*FString::Printf(
		TEXT("and taking the frame in one step does not (%.4f uu apart)"), UnsplitGap),
		UnsplitGap > 1.0);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * THE DEFAULT MaxSubsteps CEILING COVERS THE SPEED LADDER, NOT JUST A HITCH (#107 item 4).
 *
 * CONFIRMED, 2026-09-12 review. MaxSubsteps x MaxSubstepSeconds bounds one Advance call at
 * MaxSubsteps * MaxSubstepSeconds - the OLD default of 8 x 1/30 s gave 267 ms - but
 * USimClock's ladder (AirportOps, SimClock.h) reaches X32, and UAirsideTraffic::Advance's
 * own caller (ARoadNetworkActor::Tick) hands it the real frame time TIMES that multiplier.
 * A 30 fps frame at X32 is 1.067 s of sim time EVERY FRAME, not just on a hitch, which needs
 * 32 steps of MaxSubstepSeconds to stay at the documented target - the old ceiling of 8
 * clamped that to 8 steps of 133 ms each, four times MaxSubstepSeconds' own 33 ms, which is
 * the same rubber-banding the substep split exists to remove in the first place, just moved
 * to a higher speed setting instead of fixed.
 *
 * PINS THE ARITHMETIC DIRECTLY, not a position that a pre-costed speed profile can mask (see
 * FRouteFollower::Advance's Profile.LimitAt - it plans a corner's braking many steps ahead,
 * which makes a coarser step's actual DISPLACEMENT a weak and noisy signal here). What
 * matters is simpler and exact: the number of steps a full ladder-top frame needs at
 * MaxSubstepSeconds must not exceed MaxSubsteps, on a freshly constructed model - i.e. on
 * whatever a level that never touches either figure actually runs.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficSubstepLadderTest,
	"Airside.Model.Traffic.SubstepCeilingCoversTheSpeedLadder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficSubstepLadderTest::RunTest(const FString& Parameters)
{
	const UGroundTraffic* Fresh = NewObject<UGroundTraffic>(GetTransientPackage());

	// X32 IS THE LADDER'S TOP (USimClock::SpeedLadder, AirportOps/SimClock.h) and 30 fps is
	// THIS SUBSTEP CEILING'S OWN floor for ordinary play rather than a hitch (FPropAliasingTest's
	// rate list calls 24 "a hitching" rate and starts bracketing real play at 30) - so this is
	// the busiest EVERY-FRAME delta the ladder can ask for, not a hitch outlier the ceiling is
	// allowed to clamp. A DIFFERENT, HIGHER FLOOR (60 fps) is what UAirsideAgentAnim::
	// PropDisplayCapRPM is picked against - the two are chosen separately, one per feature,
	// not read from one shared "ordinary play" constant.
	const double WorstOrdinaryFrame = 32.0 * (1.0 / 30.0);
	const int32 StepsNeeded = FMath::CeilToInt(WorstOrdinaryFrame / Fresh->Rules.MaxSubstepSeconds);

	UE_LOG(LogAirsideTests, Log,
		TEXT("SubstepCeilingCoversTheSpeedLadder: %.3f s needs %d steps of %.4f s; MaxSubsteps is %d"),
		WorstOrdinaryFrame, StepsNeeded, Fresh->Rules.MaxSubstepSeconds, Fresh->Rules.MaxSubsteps);

	// THE ASSERTION THE BUG WOULD FAIL: the old default of 8 is less than the 32 steps X32 at
	// 30 fps needs, so every frame at that speed - not merely a hitch - was silently taken in
	// steps four times longer than MaxSubstepSeconds documents.
	TestTrue(*FString::Printf(
		TEXT("MaxSubsteps (%d) covers a full ladder-top frame (%d steps needed)"),
		Fresh->Rules.MaxSubsteps, StepsNeeded), Fresh->Rules.MaxSubsteps >= StepsNeeded);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A REDIRECTED AIRCRAFT ALREADY HAS ITS ENGINES RUNNING.
 *
 * REPORTED FROM PLAY: "when departing there is no time for the prop to spin up, it is still
 * coming up to speed when the aircraft is taxiing at full speed."
 *
 * FRoadAgent::StartTaxi deliberately starts COLD, so a propeller winds up as the aircraft
 * first rolls - which is right for a plain dispatch and wrong for everything that reaches
 * RedirectAgent. A departure has spent a turnaround on a stand with its engines started
 * minutes before it moved; the taxi out is not an engine start. Winding up from zero there
 * meant the prop was still accelerating while the aeroplane was already at taxi speed.
 *
 * ASSERTS BOTH HALVES, because the fix is a difference between two paths and an assertion
 * on only the redirect would pass just as well if StartTaxi had been changed to start warm
 * too - which would lose the wind-up on a genuine cold start.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficWarmRedirectTest,
	"Airside.Model.Traffic.RedirectStartsTheEngineAtSpeed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficWarmRedirectTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId W = TestGraph::Node(*Net, -20000.0, 0.0);
	const FGuidelineNodeId J = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId N = TestGraph::Node(*Net, 0.0, 20000.0);
	TestGraph::Join(*Net, W, J);
	TestGraph::Join(*Net, J, N);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Airframe = TestAirframes::GroundOnly();

	// The same fallback StartEngineAtSpeed uses, so an airframe with no authored engine still
	// has an expected figure rather than the test asserting against zero.
	const double AtSpeed = Airframe.Engine.IsSet() ? Airframe.Engine.MaxRPM : 2000.0;

	const int32 Id = Traffic->DispatchAgent(Net,
		M2TrafficRoute(*Net, W, J, ETraversalClass::Aircraft),
		Airframe, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }

	// Measured before any tick: the wind-up is what is under test, so letting it run would
	// be measuring the ramp rate instead of where it started.
	const FRoadAgent* Cold = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("the agent exists"), Cold != nullptr)) { return false; }
	TestTrue(*FString::Printf(
		TEXT("a plain dispatch still starts the engine cold, so a cold start still winds up (%.0f RPM)"),
		Cold->GetEngineRPM()), Cold->GetEngineRPM() < AtSpeed);

	if (!TestTrue(TEXT("the redirect is accepted"),
		Traffic->RedirectAgent(Id, Net, M2TrafficRoute(*Net, W, N, ETraversalClass::Aircraft))))
	{
		return false;
	}

	// Re-fetched: RedirectAgent may have moved the agent array out from under the pointer.
	const FRoadAgent* Warm = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("the agent survived the redirect"), Warm != nullptr)) { return false; }
	TestTrue(*FString::Printf(
		TEXT("but a redirect finds the engines already running at speed (%.0f RPM, want %.0f)"),
		Warm->GetEngineRPM(), AtSpeed), FMath::IsNearlyEqual(Warm->GetEngineRPM(), AtSpeed, 0.01));
	TestTrue(TEXT("and running"), Warm->bEngineRunning);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A REDIRECT DOES NOT WARM-START AN ENGINE THAT HAS ALREADY SHUT DOWN.
 *
 * CONFIRMED, 2026-09-12 review (#107 item 2). RedirectAgent called StartEngineAtSpeed
 * unconditionally after StartTaxi, snapping EngineRPM 0 -> MaxRPM in the very same call for
 * DepartAgent on a parked aircraft that had already run out its post-arrival shutdown pause.
 * StartEngineAtSpeed's own header says what it is FOR - "as it is for an aeroplane that has
 * spent a turnaround ... before it taxied out", which presumes the engine was ALREADY
 * running - so calling it regardless made a stopped propeller jump straight to full power
 * with no spool-up at all, which is the opposite of FTrafficWarmRedirectTest above, whose
 * redirected agent's engine had never stopped.
 *
 * FRoadAgent::StartTaxi already primes a cold start (bEngineRunning=true, EngineRPM=0.0) -
 * the same one a plain DispatchAgent gets - so the fix is to leave that alone unless the
 * engine was already running a moment before the redirect.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficColdRedirectTest,
	"Airside.Model.Traffic.RedirectDoesNotWarmStartAShutDownEngine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficColdRedirectTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 100.0, 0.0);
	const FGuidelineNodeId C = TestGraph::Node(*Net, 100.0, 20000.0);
	TestGraph::Join(*Net, A, B);
	TestGraph::Join(*Net, B, C);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Airframe = TestAirframes::GroundOnly();
	const double AtSpeed = Airframe.Engine.IsSet() ? Airframe.Engine.MaxRPM : 2000.0;

	// A HAIR-TRIGGER SHUTDOWN PAUSE, so the aircraft is sitting with its engine already off
	// well within a handful of ticks - #107 item 2's DepartAgent-on-a-shut-down-aircraft case.
	const int32 Id = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, B, ETraversalClass::Aircraft),
		Airframe, ETraversalClass::Aircraft, /*ShutdownPauseSeconds=*/0.1);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }

	const bool bShutDown = RunUntil(*Traffic, *Net, 10.0, [&]()
	{
		const FRoadAgent* Ag = Traffic->FindAgent(Id);
		return Ag != nullptr && Ag->Phase == EAgentPhase::Parked && !Ag->bEngineRunning;
	});
	if (!TestTrue(TEXT("parked and shut down before the redirect"), bShutDown))
	{
		return false;
	}

	// NOT NECESSARILY ZERO: bEngineRunning flips the moment the pause elapses, but EngineRPM
	// trails it down over SpoolDownSeconds the same way it trails a start up - see
	// AdvanceEngine. CAPTURED, not just logged: this is the mid-decay RPM the fix must not
	// throw away - see the assertion below and PR #134 review item 3.
	const FRoadAgent* ShutDown = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("the agent exists"), ShutDown != nullptr)) { return false; }
	const double PreRedirectRPM = ShutDown->GetEngineRPM();
	AddInfo(*FString::Printf(TEXT("shut down at %.0f RPM, still decaying"), PreRedirectRPM));

	if (!TestTrue(TEXT("the redirect is accepted"),
		Traffic->RedirectAgent(Id, Net, M2TrafficRoute(*Net, B, C, ETraversalClass::Aircraft))))
	{
		return false;
	}

	// THE ASSERTIONS THE BUG WOULD FAIL. Before the original fix this jumped straight to
	// AtSpeed in the very same call, with no spool-up at all. Before the review fix
	// (PR #134), StartTaxi's own EngineRPM=0.0 reset was left standing on the not-already-
	// running branch, so a redirect landing MID-DECAY (bEngineRunning already false, RPM
	// still positive - exactly this test's fixture) snapped the propeller DOWN to zero first
	// and spooled it back UP from there: the same one-frame snap this fix exists to remove,
	// just in the other direction. RedirectAgent's own zero-second Advance (#107 item 3)
	// calls AdvanceEngine(0.0), which makes no change, so immediately after the redirect the
	// RPM must be AT LEAST what it already was.
	const FRoadAgent* Redirected = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("the agent survived the redirect"), Redirected != nullptr)) { return false; }
	TestTrue(*FString::Printf(
		TEXT("a shut-down engine spools up rather than snapping to speed (%.0f RPM, cap %.0f)"),
		Redirected->GetEngineRPM(), AtSpeed), Redirected->GetEngineRPM() < AtSpeed);
	TestTrue(*FString::Printf(
		TEXT("and it never drops below where it already was (%.0f RPM, was %.0f)"),
		Redirected->GetEngineRPM(), PreRedirectRPM), Redirected->GetEngineRPM() >= PreRedirectRPM);
	TestTrue(TEXT("but it is running again, spooling up"), Redirected->bEngineRunning);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A REDIRECT RE-POSES IMMEDIATELY, EVEN ON A PAUSED FRAME.
 *
 * CONFIRMED, 2026-09-12 review (#107 item 3). UGroundTraffic::Advance early-returns on
 * DeltaSeconds <= 0 (the paused-frame guard, by design - see its own comment), which means
 * nothing calls FRoadAgent::Advance for the rest of a paused tick. DispatchAgent covers this
 * for a fresh agent with its own zero-second Agent.Advance(0.0, Motion) right after StartTaxi
 * - see its comment - but RedirectAgent did not, so LastMotion was left exactly as StartTaxi's
 * OWN fallback reset it: FAgentMotion() with only Position set. Heading 0 regardless of the
 * new route's actual direction, EngineRPM 0 regardless of whatever StartEngineAtSpeed had
 * just written into Agent.EngineRPM. UAirsideTraffic::Advance poses the view off exactly this
 * field every tick (AirsideTraffic.cpp:255-268), including a paused one, so a player pressing
 * Depart while paused would see the aeroplane point due east with a stopped propeller until
 * the game resumed - regardless of which way the departure runway actually lies.
 *
 * Fixed the same way DispatchAgent already does it: RedirectAgent now re-poses with its own
 * zero-second Agent.Advance(0.0, Motion) once everything is armed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRedirectPosesImmediatelyTest,
	"Airside.Model.Traffic.RedirectPosesImmediatelyEvenPaused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRedirectPosesImmediatelyTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 20000.0, 0.0);
	const FGuidelineNodeId D = TestGraph::Node(*Net, 20000.0, 20000.0);
	TestGraph::Join(*Net, A, B);
	TestGraph::Join(*Net, B, D);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Airframe = TestAirframes::GroundOnly();
	const double AtSpeed = Airframe.Engine.IsSet() ? Airframe.Engine.MaxRPM : 2000.0;

	// A -> B, due EAST (heading 0) - so a redirect's heading is measured against a real
	// change, not against the same number StartTaxi's fallback would have left anyway.
	const int32 Id = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, B, ETraversalClass::Aircraft),
		Airframe, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }

	// NOT TICKED: the engine is still warm from the dispatch itself (bEngineRunning true), so
	// this redirect takes the "already running" branch (#107 item 2) and StartEngineAtSpeed
	// writes Agent.EngineRPM = AtSpeed - the mismatch this test pins is between THAT field and
	// LastMotion.EngineRPM, which is what the view actually reads.
	//
	// B -> D, due NORTH (heading +90 deg) - unmistakably different from both 0 and from
	// whatever FAgentMotion()'s default would read.
	if (!TestTrue(TEXT("the redirect is accepted"),
		Traffic->RedirectAgent(Id, Net, M2TrafficRoute(*Net, B, D, ETraversalClass::Aircraft))))
	{
		return false;
	}

	// NO Traffic->Advance CALL HERE AT ALL - this is exactly the state a paused frame would
	// show, because UGroundTraffic::Advance(0.0, ...) would not touch the agent either.
	const FRoadAgent* Redirected = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("the agent survived the redirect"), Redirected != nullptr)) { return false; }

	TestTrue(*FString::Printf(
		TEXT("LastMotion already points north (%.1f deg), not the old heading or the FAgentMotion default"),
		FMath::RadiansToDegrees(Redirected->LastMotion.Heading)),
		FMath::IsNearlyEqual(Redirected->LastMotion.Heading, PI * 0.5, 0.01));

	TestTrue(*FString::Printf(
		TEXT("LastMotion already shows the warm-started engine (%.0f RPM, want %.0f)"),
		Redirected->LastMotion.EngineRPM, AtSpeed),
		FMath::IsNearlyEqual(Redirected->LastMotion.EngineRPM, AtSpeed, 0.01));
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * THE POSED AIRCRAFT NEVER REVERSES OR SNAPS, across a whole arrival.
 *
 * Written while chasing judder reported from play (2026-09-12). It turned out to be
 * presentation and not the model - see r.VSync in Config/DefaultEngine.ini - but nothing
 * covered the property the report NAMED, and the investigation took a day partly because
 * there was no test that could answer "is the model smooth?" in one run.
 *
 * MEASURES THE SYMPTOM'S OWN WORDS. "Jerking back and forwards" is the step VECTOR
 * reversing, which is a sign test with no threshold to argue about. An earlier attempt
 * compared distance moved against speed x dt and read 1.00 on every frame forever - the
 * follower computes the position FROM speed x dt, so that assertion was measuring its own
 * input. A test that cannot fail is worse than no test, because it is believed.
 *
 * Walks the phases a plain dispatch never reaches, which is where a discontinuity would
 * live: Arriving -> the vacate handover -> the taxi follower. Frame times jitter, because a
 * constant delta would hide anything that only shows on an uneven one.
 *
 * Yaw is asserted as well as position: a nose that stepped while the body ran smooth would
 * look exactly like judder and would pass every positional check here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficPoseContinuityTest,
	"Airside.Model.Traffic.PoseNeverReversesOrSnaps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficPoseContinuityTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTestAirport Fixture = FTestAirport::Build(Airframe);
	URoadNetwork* Net = Fixture.Net;
	const FVector2D Threshold = Fixture.Threshold;

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Traffic->DispatchArrival(*Net, Threshold, Airframe, 0.0);
	if (!TestTrue(TEXT("arrival dispatched"), Id > 0)) { return false; }

	FRandomStream Frames(4242);
	FVector2D PrevPos = Traffic->FindAgent(Id)->LastMotion.Position;
	double PrevYaw = Traffic->FindAgent(Id)->LastMotion.Heading;
	FVector2D LastStep = FVector2D::ZeroVector;
	double LastLen = 0.0;

	int32 Reversals = 0, Lurches = 0, YawSnaps = 0, Ticks = 0;
	double WorstCos = 1.0, WorstYaw = 0.0;
	FString Where;

	while (Ticks < 20000)
	{
		const double Dt = Frames.FRandRange(0.014, 0.020);
		Traffic->Advance(Dt, Net);
		++Ticks;

		const FRoadAgent* A = Traffic->FindAgent(Id);
		if (A == nullptr) { break; }

		const FVector2D Pos = A->LastMotion.Position;
		const FVector2D Step = Pos - PrevPos;
		const double Len = Step.Size();
		const double Yaw = FMath::RadiansToDegrees(FMath::UnwindRadians(A->LastMotion.Heading - PrevYaw));

		if (Len > KINDA_SMALL_NUMBER && LastLen > KINDA_SMALL_NUMBER)
		{
			const double Cos = FVector2D::DotProduct(Step / Len, LastStep / LastLen);
			if (Cos < 0.0)
			{
				++Reversals;
				if (Cos < WorstCos)
				{
					WorstCos = Cos;
					Where = FString::Printf(TEXT("phase %d at %.0f,%.0f tick %d"),
						static_cast<int32>(A->Phase), Pos.X, Pos.Y, Ticks);
				}
			}
			if (Len > LastLen * 2.0 || Len * 2.0 < LastLen) { ++Lurches; }
		}
		if (FMath::Abs(Yaw) > 5.0)
		{
			++YawSnaps;
			if (FMath::Abs(Yaw) > FMath::Abs(WorstYaw)) { WorstYaw = Yaw; }
		}

		LastStep = Step; LastLen = Len; PrevPos = Pos; PrevYaw = A->LastMotion.Heading;
	}

	AddInfo(FString::Printf(
		TEXT("pose continuity: %d ticks, %d reversals (worst cos %.3f %s), %d lurches, %d yaw snaps (worst %+.2f deg)"),
		Ticks, Reversals, WorstCos, *Where, Lurches, YawSnaps, WorstYaw));

	TestTrue(TEXT("the arrival actually flew, so the walk means something"), Ticks > 100);
	TestEqual(*FString::Printf(
		TEXT("the body never moves backwards (worst cos %.3f, %s)"), WorstCos, *Where),
		Reversals, 0);
	TestEqual(*FString::Printf(
		TEXT("and the nose never steps (worst %+.2f deg in one frame)"), WorstYaw),
		YawSnaps, 0);

	// LURCHES ARE NOT ASSERTED. A frame twice as long as the last one MUST carry twice the
	// distance - that is the delta being honoured, not a defect - and every lurch seen in
	// play paired with a frame time that had genuinely changed. Counted and reported because
	// the number is worth reading when this test is being used to chase something.
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficGraphRebuildNodeVisitsTest,
	"Airside.Model.Traffic.GraphRebuildNodeVisits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficGraphRebuildNodeVisitsTest::RunTest(const FString& Parameters)
{
	// #172: every handle OnGraphRebuilt hands to FPlanReResolver::ReResolvePlan dies on a
	// rebuild - re-added at the same slot count, new generations - so a re-resolving agent
	// calls RouteSearch::FindNearestNode for its from-node, once per remaining step, and for
	// its goal, and that function used to scan every LIVE guideline node to answer each one.
	// A agents x S remaining steps x N nodes node-visits, in ONE rebuild, on the graph this
	// test builds. This is the boundary the fix moves, not the route it produces - the
	// existing GraphRebuild test above already pins that a same-geometry rebuild costs no
	// search at all; this pins that answering it no longer costs the whole graph either.
	constexpr int32 NumNodes = 200;
	constexpr int32 NumAgents = 10;
	constexpr double Spacing = 2000.0;

	// A CHAIN, rebuilt exactly the way FRoadGuidelineBuilder::Build rebuilds one: every
	// derived edge and node freed, then re-added at the SAME positions with NEW handles -
	// same shape as FTrafficGraphRebuildTest's own Build lambda above, sized up from three
	// nodes to two hundred so there is something for a linear scan to be slow over.
	auto BuildChain = [](URoadNetwork& Net, TArray<FGuidelineNodeId>& OutNodes)
	{
		TArray<FGuidelineEdgeId> Edges;
		for (int32 Index = 0; Index < Net.GetGuidelineEdges().Num(); ++Index)
		{
			if (Net.GetGuidelineEdges()[Index].bAlive)
			{
				FGuidelineEdgeId Id;
				Id.Index = Index;
				Id.Generation = Net.GetGuidelineEdges()[Index].Generation;
				Edges.Add(Id);
			}
		}
		for (const FGuidelineEdgeId& Id : Edges) { Net.RemoveGuidelineEdge(Id); }
		for (int32 Index = 0; Index < Net.GetGuidelineNodes().Num(); ++Index)
		{
			if (Net.GetGuidelineNodes()[Index].bAlive) { Net.RemoveGuidelineNode(Net.GuidelineNodeIdAt(Index)); }
		}

		OutNodes.Reset();
		OutNodes.Reserve(NumNodes);
		for (int32 Index = 0; Index < NumNodes; ++Index)
		{
			OutNodes.Add(Net.AddGuidelineNode(FVector2D(Index * Spacing, 0.0)));
		}
		for (int32 Index = 0; Index + 1 < NumNodes; ++Index)
		{
			TestGraph::Join(Net, OutNodes[Index], OutNodes[Index + 1]);
		}
	};

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	TArray<FGuidelineNodeId> Nodes;
	BuildChain(*Net, Nodes);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	for (int32 AgentIndex = 0; AgentIndex < NumAgents; ++AgentIndex)
	{
		// FRESH, NOT TICKED: StartTaxi poses an agent on step 0 with Travelled 0, so every
		// one of the chain's NumNodes-1 steps is still "remaining" - the worst case
		// OnGraphRebuilt's own header names, not a shortened one a few ticks would leave.
		Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, Nodes[0], Nodes.Last(), ETraversalClass::GroundVehicle),
			TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	}

	// REBUILT AT THE SAME POSITIONS: every handle in every agent's plan is now dead, but
	// nothing about where the pavement IS has moved, so every agent is fully re-resolved
	// rather than truncated or stranded early - the case that actually walks every step.
	BuildChain(*Net, Nodes);

	RouteSearch::ResetNodeVisitCountForTest();
	Traffic->OnGraphRebuilt(*Net);
	const int32 Visits = RouteSearch::NodeVisitCountForTest();

	const FGraphRebuildSummary Summary = Traffic->GetLastRebuildSummaryForTest();
	if (!TestEqual(TEXT("every agent survived the rebuild un-stranded, so the walk below covers every step"),
		Summary.Stranded, 0))
	{
		return false;
	}
	if (!TestEqual(TEXT("and none needed a fresh search - identical geometry costs one, not a route search"),
		Summary.Replanned, 0))
	{
		return false;
	}

	// THE BOUND. Worst case is every agent's every remaining step plus its from-node and its
	// goal, each an O(N) scan: NumAgents * (NumNodes - 1 steps + 2) * NumNodes. An indexed
	// lookup instead visits only the handful of nodes in its own grid cell and the eight
	// around it - on this chain, one or two - so the true count is close to NumAgents *
	// (NumNodes + 1) * (a small constant), orders of magnitude under the worst case. The
	// bound below does not pin that constant; it only rules out the O(N)-per-call code this
	// replaces, which would land within a rounding error of WorstCase itself.
	const int64 WorstCase = static_cast<int64>(NumAgents) * (NumNodes + 1) * NumNodes;
	UE_LOG(LogAirsideTests, Log,
		TEXT("GraphRebuildNodeVisits measured: %d node visits (%d agents, %d nodes, worst case %lld)"),
		Visits, NumAgents, NumNodes, WorstCase);
	TestTrue(FString::Printf(TEXT("node visits (%d) are well below A*S*N (%lld)"), Visits, WorstCase),
		static_cast<int64>(Visits) < WorstCase / 10);

	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReofferStandsRetireReentrancyTest,
	"Airside.Model.Traffic.ReofferStandsRetireReentrancy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FReofferStandsRetireReentrancyTest::RunTest(const FString& Parameters)
{
	// #193: ReofferStands used to re-derive its agent's index with FindIndex AFTER calling
	// RedirectAgent, which broadcasts OnAgentPhaseChanged. In play, UJobBoard::OnAgentPhase
	// answered that broadcast by calling UGroundTraffic::RetireAgent SYNCHRONOUSLY - so a
	// listener that retires the very agent being redirected removes it from Agents mid-call,
	// and Agents[FindIndex(Id)] afterwards indexed with INDEX_NONE. Since the ops bus it hears
	// the event a drain later (#436), but a synchronous listener is still legal - UGroundTraffic's
	// re-entrancy contract - and this test IS one: it pins that contract with a plain lambda,
	// without pulling in the AirportOps module: the fix is in Model/, and belongs to a Model/ test.
	const FTestAirport A = FTestAirport::Build(TestAirframes::Piper(), { .StandCount = 2 });
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	const int32 Id = Traffic->DispatchArrival(*A.Net, A.Threshold, TestAirframes::Piper(), 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }
	Traffic->Advance(0.05, A.Net);   // one tick: on final
	const FGuidelineNodeId Goal0 = Traffic->FindAgent(Id)->GoalNode;
	const FEntityInstanceId Target = (Goal0 == A.Pose(A.Stands[0])) ? A.Stands[0] : A.Stands[1];
	const FEntityInstanceId Spare = (Target == A.Stands[0]) ? A.Stands[1] : A.Stands[0];

	// BOTH STANDS GONE, exactly like StandRetargetTest's own step 2: the aircraft lands with
	// nowhere to go, sets bAwaitingStand, and parks at the node it waited at. That is the
	// state ReofferStands' Waiting list is built from.
	A.Net->RemoveEntity(Target);
	TestGraph::Rebuild(*A.Net);
	Traffic->OnGraphRebuilt(*A.Net);
	A.Net->RemoveEntity(Spare);
	TestGraph::Rebuild(*A.Net);
	Traffic->OnGraphRebuilt(*A.Net);
	if (!TestTrue(TEXT("it lands and waits"), RunUntil(*Traffic, *A.Net, 600.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P && P->Phase == EAgentPhase::Parked; })))
	{
		return false;
	}

	// THE LISTENER STANDS IN FOR WHAT UJobBoard::OnAgentPhase WAS: it retires this agent the moment
	// it sees ITS phase change away from Parked - which is the redirect ReofferStands is about
	// to drive - and does so exactly once, so the Gone broadcast RetireAgent itself raises does
	// not recurse.
	bool bRetiredDuringRedirect = false;
	int32 PhaseChangedBroadcasts = 0;
	Traffic->OnAgentPhaseChanged.AddLambda(
		[&](const FAgentTransition& T)
		{
			++PhaseChangedBroadcasts;
			if (T.AgentId == Id && T.From == EAgentPhase::Parked && T.To != EAgentPhase::Gone && !bRetiredDuringRedirect)
			{
				bRetiredDuringRedirect = true;
				Traffic->RetireAgent(Id);
			}
		});

	// A STAND APPEARS. Advance's re-offer pass (ReofferStands) finds the waiter, plans a route
	// to it, and calls RedirectAgent - which is where the listener above fires. On unfixed code
	// this crashes (Agents[INDEX_NONE]) rather than merely failing, which is why this is the
	// regression: a green run here means the re-entrancy is actually safe, not just unassessed.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	A.Net->PlaceEntity(Stand, Stand->Anchors, A.ExitAt + FVector2D(9000.0, -10000.0), 0.0);
	TestGraph::Rebuild(*A.Net);
	Traffic->OnGraphRebuilt(*A.Net);
	Traffic->Advance(0.05, A.Net);   // the re-offer runs at the end of a tick

	TestTrue(TEXT("the listener actually got to retire mid-redirect (the case under test ran)"),
		bRetiredDuringRedirect);
	TestTrue(TEXT("more than one broadcast fired: the redirect's and RetireAgent's Gone"),
		PhaseChangedBroadcasts >= 2);
	TestNull(TEXT("the agent is gone - retired, not left half-redirected"), Traffic->FindAgent(Id));
	TestEqual(TEXT("the table is left with no agents at all, not a dangling slot"),
		Traffic->GetAgentCount(), 0);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalStandCountLogTest,
	"Airside.Model.Traffic.ArrivalStandCountLog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalStandCountLogTest::RunTest(const FString& Parameters)
{
	// #193: DispatchArrival's own "%d stand(s) on the airport" line read
	// Network.GetEntities().Num() - the whole slot array, dead slots and depots included -
	// where every other reader of "how many stands" means live EServiceRole::Aircraft
	// entities. Two ways that number can be wrong, both set up here: a DEAD SLOT (one of the
	// fixture's two stands, removed - RemoveEntity frees the slot but GetEntities() still
	// counts it) and a DEPOT (a live entity whose PoseRole is Fuel, not Aircraft). Both must
	// be excluded, so the true count (1) is the number to look for, not GetEntities().Num()
	// (which would read 3: two stand slots, one of them dead, plus the live depot).
	const FTestAirport A = FTestAirport::Build(TestAirframes::Piper(), { .StandCount = 2 });

	// A DEPOT, placed but not joined to any guideline - joining is irrelevant to a log line
	// that only counts entities, and skipping FAnchorLink::Build keeps this test to the one
	// thing under test.
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }
	A.Net->PlaceEntity(Depot, Depot->Anchors, A.ExitAt + FVector2D(0.0, 20000.0),
		0.0, /*DesignWingspan=*/0.0, Depot->PoseRole, Depot->Trucks);

	// A DEAD SLOT: one of the two stands, removed. The other stays live so the arrival still
	// has somewhere to go - StandRetargetTest's own step 1 confirms one stand's removal
	// leaves the graph routable to the survivor.
	TestTrue(TEXT("a stand removed to free (not erase) its slot"), A.Net->RemoveEntity(A.Stands[0]));

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	// ISSUE #216d: was its own hand-copied FOutputDevice (FLogAirsideTrafficSpy) missing the
	// CanBeUsedOnMultipleThreads override #216 traced the RoadRebuildLogQuietTest flake to -
	// now AirsideTestWorld.h's shared FLogLineSpy, which carries that override once for every
	// spy of this shape instead of leaving each one to remember it by hand.
	FLogLineSpy Spy(FName(TEXT("LogAirsideTraffic")));
	GLog->AddOutputDevice(&Spy);
	const int32 Id = Traffic->DispatchArrival(*A.Net, A.Threshold, TestAirframes::Piper(), 1.0);
	GLog->RemoveOutputDevice(&Spy);
	if (!TestTrue(TEXT("dispatched onto the surviving stand"), Id > 0)) { return false; }

	const FString* Line = Spy.CapturedLines.FindByPredicate(
		[](const FString& L) { return L.Contains(TEXT("stand(s) on the airport")); });
	if (!TestTrue(TEXT("the arrival line was logged"), Line != nullptr)) { return false; }

	// PARSED OUT OF THE ACTUAL LINE, not re-derived from the model, so this fails if the
	// wording ever drifts from "%d stand(s)" without anyone noticing - the same reason
	// RoadRebuildLogQuietTest's spy names its captured line rather than trusting a count alone.
	int32 Reported = -1;
	const int32 Marker = Line->Find(TEXT(" stand(s) on the airport"));
	if (TestTrue(TEXT("the line names a count before 'stand(s)'"), Marker != INDEX_NONE))
	{
		int32 DigitsStart = Marker;
		while (DigitsStart > 0 && FChar::IsDigit((*Line)[DigitsStart - 1])) { --DigitsStart; }
		Reported = FCString::Atoi(*Line->Mid(DigitsStart, Marker - DigitsStart));
	}

	TestEqual(TEXT("only the one LIVE aircraft-role stand is counted - not the dead slot, ")
		TEXT("not the depot"), Reported, 1);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A ROUTE EXTENDED UNDER A MOVING AGENT DOES NOT STOP IT (the rig test course, 2026-09-25):
 * dispatched A->B, then B->C appended while it is still on its way to B, and C->D appended
 * again while it is already BRAKING for C. It must pass B and C without its speed reaching
 * zero, keep the distance it had travelled, and park at D - where RedirectAgent from a parked
 * agent restarts it from rest. A refused extension changes nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficExtendRouteKeepsMovingTest,
	"Airside.Model.Traffic.ExtendRouteKeepsMoving",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficExtendRouteKeepsMovingTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 20000.0, 0.0);
	const FGuidelineNodeId C = TestGraph::Node(*Net, 40000.0, 0.0);
	const FGuidelineNodeId D = TestGraph::Node(*Net, 60000.0, 0.0);
	TestGraph::Join(*Net, A, B);
	TestGraph::Join(*Net, B, C);
	TestGraph::Join(*Net, C, D);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Airframe = TestAirframes::GroundOnly();
	const int32 Id = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, B, ETraversalClass::Aircraft),
		Airframe, ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }
	for (int32 Tick = 0; Tick < 2000 && Traffic->FindAgent(Id)->Follower.Travelled < 5000.0; ++Tick)
	{
		Traffic->Advance(0.05, Net);
	}
	const double Before = Traffic->FindAgent(Id)->Follower.Travelled;
	const double SpeedBefore = Traffic->FindAgent(Id)->Follower.Speed;
	const double LengthBefore = Traffic->FindAgent(Id)->Follower.Plan.Length;
	const uint32 RevisionBefore = Traffic->OccupancyRevision();
	TestTrue(TEXT("under way before the extension"), SpeedBefore > 0.0);

	// REFUSED, NOTHING CHANGED: a tail from A does not start where the route ends (B).
	TestFalse(TEXT("a tail that does not start where the route ends is refused"),
		Traffic->ExtendRoute(Id, Net, M2TrafficRoute(*Net, A, C, ETraversalClass::Aircraft)));
	TestEqual(TEXT("the refusal left the route as it was"), Traffic->FindAgent(Id)->Follower.Plan.Length, LengthBefore);
	TestEqual(TEXT("and the distance travelled"), Traffic->FindAgent(Id)->Follower.Travelled, Before);
	TestTrue(TEXT("and the goal"), Traffic->FindAgent(Id)->GoalNode == B);
	TestEqual(TEXT("and bumped no occupancy revision"), Traffic->OccupancyRevision(), RevisionBefore);

	if (!TestTrue(TEXT("the extension is accepted"), Traffic->ExtendRoute(Id, Net, M2TrafficRoute(*Net, B, C, ETraversalClass::Aircraft)))) { return false; }
	TestEqual(TEXT("the distance travelled survives the splice"), Traffic->FindAgent(Id)->Follower.Travelled, Before);
	TestEqual(TEXT("and so does the speed"), Traffic->FindAgent(Id)->Follower.Speed, SpeedBefore);
	TestTrue(TEXT("the goal moved on to C"), Traffic->FindAgent(Id)->GoalNode == C);
	TestTrue(TEXT("and the occupancy revision was bumped (#169): the goal changed"), Traffic->OccupancyRevision() != RevisionBefore);

	// ON TO C, until it is BRAKING for C - then extend again. The rebuilt profile finds road
	// ahead, so the braking stops and it drives on rather than halting and restarting.
	const FGroundRegime& Taxi = Airframe.Chassis.Ground.Taxi;
	const double Stopping = Taxi.SpeedCap * Taxi.SpeedCap / (2.0 * FMath::Max(Taxi.Decel, 1.0));
	double PrevSpeed = Traffic->FindAgent(Id)->Follower.Speed;
	bool bBraking = false;
	for (int32 Tick = 0; Tick < 4000 && !bBraking; ++Tick)
	{
		Traffic->Advance(0.05, Net);
		const FRoadAgent* Agent = Traffic->FindAgent(Id);
		bBraking = Agent->Follower.Plan.Length - Agent->Follower.Travelled < Stopping && Agent->Follower.Speed < PrevSpeed - 1.0;
		PrevSpeed = Agent->Follower.Speed;
	}
	if (!TestTrue(TEXT("it began braking for C"), bBraking)) { return false; }
	const double SpeedWhileBraking = Traffic->FindAgent(Id)->Follower.Speed;
	if (!TestTrue(TEXT("a second extension, while braking, is accepted"),
		Traffic->ExtendRoute(Id, Net, M2TrafficRoute(*Net, C, D, ETraversalClass::Aircraft)))) { return false; }

	double SlowestPastB = TNumericLimits<double>::Max();
	double SlowestPastC = TNumericLimits<double>::Max();
	for (int32 Tick = 0; Tick < 6000 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++Tick)
	{
		Traffic->Advance(0.05, Net);
		const FRoadAgent* Agent = Traffic->FindAgent(Id);
		if (FMath::Abs(Agent->Follower.Travelled - 20000.0) < 2000.0) { SlowestPastB = FMath::Min(SlowestPastB, Agent->Follower.Speed); }
		if (FMath::Abs(Agent->Follower.Travelled - 40000.0) < 2000.0) { SlowestPastC = FMath::Min(SlowestPastC, Agent->Follower.Speed); }
	}
	TestTrue(*FString::Printf(TEXT("it passed B without stopping (slowest %.0f uu/s within 20 m of it)"), SlowestPastB),
		SlowestPastB > 0.5 * SpeedBefore);
	TestTrue(*FString::Printf(TEXT("and C, though it was braking for it when extended (slowest %.0f uu/s, %.0f when extended)"), SlowestPastC, SpeedWhileBraking),
		SlowestPastC > 0.5 * SpeedWhileBraking);
	TestEqual(TEXT("and parked at D, the extended goal"),
		static_cast<int32>(Traffic->FindAgent(Id)->Phase), static_cast<int32>(EAgentPhase::Parked));
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * AN EXTENSION MOVES THE GOAL THE WAY A REDIRECT DOES (review of ed81410c): through the same
 * ReleaseGoal/TakeGoal. Three consequences, each a way the extended agent would otherwise act
 * on the goal it no longer has: a route whose OLD end was a runway must not take off there; a
 * new goal on a stand is claimed; an agent waiting for a stand is no longer waiting.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficExtendRouteMovesTheGoalTest,
	"Airside.Model.Traffic.ExtendRouteMovesTheGoal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficExtendRouteMovesTheGoalTest::RunTest(const FString& Parameters)
{
	// 1. THE OLD END WAS A RUNWAY. TrafficDepartureReleaseTest's fixture: a taxiway guideline
	// ending ON the runway at its split, which arms a departure; then extended off the runway.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Runway = TestProfiles::Runway();
		const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
		const FRoadNodeId RM = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
		Net->AddStraightSegment(RA, RM, Runway);
		Net->AddStraightSegment(RM, RB, Runway);
		const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, -20000.0);
		const FGuidelineNodeId B = TestGraph::Node(*Net, 0.0, 0.0);
		const FGuidelineNodeId C = TestGraph::Node(*Net, 0.0, 20000.0);
		TestGraph::Join(*Net, A, B);
		TestGraph::Join(*Net, B, C);

		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, B, ETraversalClass::Aircraft),
			TestAirframes::Piper(), ETraversalClass::Aircraft, 1.0);
		if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }
		if (!TestTrue(TEXT("the route ends on the runway, so a departure is armed"), Traffic->FindAgent(Plane)->bDepartureArmed)) { return false; }
		Traffic->Advance(0.05, Net);
		if (!TestTrue(TEXT("extended across the runway to C"),
			Traffic->ExtendRoute(Plane, Net, M2TrafficRoute(*Net, B, C, ETraversalClass::Aircraft)))) { return false; }
		TestFalse(TEXT("the departure is disarmed: the route no longer ends on the runway"), Traffic->FindAgent(Plane)->bDepartureArmed);
		TestEqual(TEXT("and it holds no runway chain for it"), Traffic->FindAgent(Plane)->GetDepartureRunway().Num(), 0);
		bool bDeparted = false;
		RunUntil(*Traffic, *Net, 600.0, [&]()
		{
			const FRoadAgent* P = Traffic->FindAgent(Plane);
			bDeparted = bDeparted || (P != nullptr && P->Phase == EAgentPhase::Departing);
			return P == nullptr || P->Phase == EAgentPhase::Parked;
		});
		TestFalse(TEXT("it never took off from the runway its OLD route ended on"), bDeparted);
		const FRoadAgent* P = Traffic->FindAgent(Plane);
		TestTrue(TEXT("it parked at C instead"), P != nullptr && P->Phase == EAgentPhase::Parked && P->GoalNode == C);
	}

	// 2 AND 3. A WAITER, EXTENDED ONTO A STAND. GroundTrafficTest's own awaiting-stand path: an
	// arrival whose stands are removed lands and taxis on with bAwaitingStand set.
	{
		const FTestAirport Air = FTestAirport::Build(TestAirframes::Piper(), { .StandCount = 2 });
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Id = Traffic->DispatchArrival(*Air.Net, Air.Threshold, TestAirframes::Piper(), 1.0);
		if (!TestTrue(TEXT("the arrival is dispatched"), Id > 0)) { return false; }
		Traffic->Advance(0.05, Air.Net);
		const FGuidelineNodeId Goal0 = Traffic->FindAgent(Id)->GoalNode;
		const FEntityInstanceId Target = (Goal0 == Air.Pose(Air.Stands[0])) ? Air.Stands[0] : Air.Stands[1];
		const FEntityInstanceId Spare = (Target == Air.Stands[0]) ? Air.Stands[1] : Air.Stands[0];
		Air.Net->RemoveEntity(Target);
		TestGraph::Rebuild(*Air.Net);
		Traffic->OnGraphRebuilt(*Air.Net);
		Air.Net->RemoveEntity(Spare);
		TestGraph::Rebuild(*Air.Net);
		Traffic->OnGraphRebuilt(*Air.Net);
		if (!TestTrue(TEXT("it lands and taxis on, waiting for a stand"), RunUntil(*Traffic, *Air.Net, 600.0,
			[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P && P->Phase == EAgentPhase::Taxiing && P->bAwaitingStand; })))
		{
			return false;
		}
		// A STAND APPEARS, and the waiter's route is extended onto it from where its route ends.
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		const FEntityInstanceId Placed = Air.Net->PlaceEntity(Stand, Stand->Anchors, Air.ExitAt + FVector2D(9000.0, -10000.0), 0.0);
		TestGraph::Rebuild(*Air.Net);
		Traffic->OnGraphRebuilt(*Air.Net);
		const FRoadAgent* Waiter = Traffic->FindAgent(Id);
		if (!TestTrue(TEXT("still taxiing and waiting after the rebuild"), Waiter != nullptr && Waiter->Phase == EAgentPhase::Taxiing && Waiter->bAwaitingStand)) { return false; }
		const FGuidelineNodeId From = Waiter->Follower.Plan.Steps.Num() > 0 ? Waiter->Follower.Plan.Steps.Last().To : Waiter->Follower.Plan.Start;
		const FGuidelineNodeId StandPose = Air.Pose(Placed);
		const FRoutePlan Tail = M2TrafficRoute(*Air.Net, From, StandPose, ETraversalClass::Aircraft);
		if (!TestTrue(TEXT("a route from the waiter's route end to the new stand exists"), Tail.IsValid())) { return false; }
		if (!TestTrue(TEXT("the extension onto the stand is accepted"), Traffic->ExtendRoute(Id, Air.Net, Tail))) { return false; }
		TestFalse(TEXT("the waiter is no longer waiting - it has a stand to go to"), Traffic->FindAgent(Id)->bAwaitingStand);
		int32 Holder = 0;
		TestTrue(TEXT("and the new stand is claimed, for it"),
			Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(StandPose), 0, &Holder) && Holder == Id);
	}
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * AN EXTENDED ROUTE DOES NOT GROW FOR EVER (review of ed81410c): KeepBehind trims the steps
 * driven more than that far back, and Travelled is rebased by exactly what was dropped - so the
 * agent is where it was, on the same line, and still parks at the far end.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficExtendRouteTrimsHistoryTest,
	"Airside.Model.Traffic.ExtendRouteTrimsHistory",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficExtendRouteTrimsHistoryTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	TArray<FGuidelineNodeId> N;
	for (int32 I = 0; I < 5; ++I)
	{
		N.Add(TestGraph::Node(*Net, I * 10000.0, 0.0));
		if (I > 0) { TestGraph::Join(*Net, N[I - 1], N[I]); }
	}
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, N[0], N[2], ETraversalClass::Aircraft),
		TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }
	for (int32 Tick = 0; Tick < 4000 && Traffic->FindAgent(Id)->Follower.Travelled < 15000.0; ++Tick)
	{
		Traffic->Advance(0.05, Net);
	}
	const FVector2D At = Traffic->FindAgent(Id)->LastMotion.Position;
	const double Travelled = Traffic->FindAgent(Id)->Follower.Travelled;
	double Dropped = -1.0;
	if (!TestTrue(TEXT("extended, keeping 20 m behind"), Traffic->ExtendRoute(Id, Net,
		M2TrafficRoute(*Net, N[2], N[4], ETraversalClass::Aircraft), 2000.0, &Dropped))) { return false; }
	TestEqual(TEXT("the first step, ending 50 m back, was dropped - and only it"), Dropped, 10000.0, 1.0);
	TestEqual(TEXT("Travelled is rebased by exactly that"), Traffic->FindAgent(Id)->Follower.Travelled, Travelled - Dropped, 0.001);
	TestEqual(TEXT("the route is the rest: 40 m minus 10 m"), Traffic->FindAgent(Id)->Follower.Plan.Length, 30000.0, 1.0);
	Traffic->Advance(0.05, Net);
	TestTrue(*FString::Printf(TEXT("and the agent is where it was, one tick on (moved %.1f uu)"),
		FVector2D::Distance(Traffic->FindAgent(Id)->LastMotion.Position, At)),
		FVector2D::Distance(Traffic->FindAgent(Id)->LastMotion.Position, At) < 100.0);
	RunUntil(*Traffic, *Net, 600.0, [&]() { return Traffic->FindAgent(Id)->Phase == EAgentPhase::Parked; });
	TestTrue(TEXT("and it parks at the extended end"),
		Traffic->FindAgent(Id)->Phase == EAgentPhase::Parked && FVector2D::Distance(Traffic->FindAgent(Id)->LastMotion.Position, FVector2D(40000.0, 0.0)) < 1000.0);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A REDIRECT OFF A RUNWAY DISARMS THE DEPARTURE (re-review of e19b187e). ArmDepartureIfRunway
 * used to clear only the runway chain, leaving bDepartureArmed and its order: an aircraft armed
 * for a take-off and redirected somewhere that is not a runway then took off on arriving there.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRedirectDisarmsDepartureTest,
	"Airside.Model.Traffic.RedirectDisarmsDeparture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRedirectDisarmsDepartureTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RM = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
	Net->AddStraightSegment(RA, RM, Runway);
	Net->AddStraightSegment(RM, RB, Runway);
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, -20000.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId C = TestGraph::Node(*Net, 20000.0, -20000.0);
	TestGraph::Join(*Net, A, B);
	TestGraph::Join(*Net, A, C);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, B, ETraversalClass::Aircraft),
		TestAirframes::Piper(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }
	if (!TestTrue(TEXT("its route ends on the runway, so a departure is armed"), Traffic->FindAgent(Plane)->bDepartureArmed)) { return false; }
	Traffic->Advance(0.05, Net);
	if (!TestTrue(TEXT("redirected to C, off the runway"),
		Traffic->RedirectAgent(Plane, Net, M2TrafficRoute(*Net, A, C, ETraversalClass::Aircraft)))) { return false; }
	TestFalse(TEXT("the departure is disarmed: the new route does not end on a runway"), Traffic->FindAgent(Plane)->bDepartureArmed);
	bool bDeparted = false;
	RunUntil(*Traffic, *Net, 600.0, [&]()
	{
		const FRoadAgent* P = Traffic->FindAgent(Plane);
		bDeparted = bDeparted || (P != nullptr && P->Phase == EAgentPhase::Departing);
		return P == nullptr || P->Phase == EAgentPhase::Parked;
	});
	TestFalse(TEXT("it never took off"), bDeparted);
	const FRoadAgent* P = Traffic->FindAgent(Plane);
	TestTrue(TEXT("it parked at C, its new goal"), P != nullptr && P->Phase == EAgentPhase::Parked && P->GoalNode == C);
	return true;
}

// ---------------------------------------------------------------------------------------
// COINCIDENT TWINS SURVIVE A REBUILD (2026-09-25). At a straight-through road node whose arms
// share a lane offset, the builder leaves two lane ends at ONE point joined by a zero-length turn
// path. The re-resolve looked each step's end up by position; on a tie FindNearestNode takes the
// later slot, so both steps named the same twin, no edge joined them, and the "failure" replanned
// to the goal - which on the rig course cut three dead ends out of the utility's route. Built by
// hand here as that pair: B1 and B2 at the same point, B2 the later slot, rebuilt in the same
// order so the tie goes the same way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRebuildCoincidentTwinsTest,
	"Airside.Model.Traffic.RebuildCoincidentTwins",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRebuildCoincidentTwinsTest::RunTest(const FString& Parameters)
{
	struct FTwinGraph { FGuidelineNodeId A, B1, B2, C; };
	auto Build = [](URoadNetwork& Net)
	{
		TArray<FGuidelineEdgeId> Edges;
		for (int32 I = 0; I < Net.GetGuidelineEdges().Num(); ++I) { if (Net.GetGuidelineEdges()[I].bAlive) { Edges.Add(Net.GuidelineEdgeIdAt(I)); } }
		for (const FGuidelineEdgeId& Id : Edges) { Net.RemoveGuidelineEdge(Id); }
		for (int32 I = 0; I < Net.GetGuidelineNodes().Num(); ++I) { if (Net.GetGuidelineNodes()[I].bAlive) { Net.RemoveGuidelineNode(Net.GuidelineNodeIdAt(I)); } }
		FTwinGraph G;
		G.A = Net.AddGuidelineNode(FVector2D(0.0, 0.0));
		G.B1 = Net.AddGuidelineNode(FVector2D(20000.0, 0.0));
		G.B2 = Net.AddGuidelineNode(FVector2D(20000.0, 0.0));
		G.C = Net.AddGuidelineNode(FVector2D(40000.0, 0.0));
		TestGraph::Join(Net, G.A, G.B1);
		TestGraph::Join(Net, G.B1, G.B2);   // the zero-length turn path
		TestGraph::Join(Net, G.B2, G.C);
		return G;
	};

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FTwinGraph Was = Build(*Net);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, Was.A, Was.C, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	TickUntil(*Traffic, *Net, 5.0, [](int32) { return true; });
	const FRoadAgent* Agent = Traffic->FindAgent(Van);
	if (!TestNotNull(TEXT("the van is out"), Agent)) { return false; }
	if (!TestEqual(TEXT("the plan steps through BOTH twins: A->B1, B1->B2, B2->C"), Agent->Follower.Plan.Steps.Num(), 3)) { return false; }
	TestTrue(TEXT("and the van is on the first step, ahead of the twins"), Agent->Follower.Travelled < Agent->Follower.Plan.Steps[0].EndDistance);
	const double LengthWas = Agent->Follower.Plan.Length;

	const FTwinGraph Now = Build(*Net);
	TestTrue(TEXT("the rebuild gave every node a new handle"), Now.B1 != Was.B1 && Now.B2 != Was.B2);
	// THE AMBIGUITY ITSELF: one position, one answer, two nodes that both hold it.
	const FGuidelineNodeId Found = RouteSearch::FindNearestNode(*Net, FVector2D(20000.0, 0.0), ETraversalClass::GroundVehicle, 25.0);
	TestTrue(TEXT("position alone names just one of the twins"), Found == Now.B1 || Found == Now.B2);
	Traffic->OnGraphRebuilt(*Net);

	const FGraphRebuildSummary Summary = Traffic->GetLastRebuildSummaryForTest();
	TestEqual(TEXT("the van was re-resolved"), Summary.ReResolved, 1);
	TestEqual(TEXT("and NOT replanned: nothing was lost, so nothing is searched - a replan to the goal drops via points"), Summary.Replanned, 0);
	TestEqual(TEXT("nor truncated"), Summary.Truncated, 0);
	Agent = Traffic->FindAgent(Van);
	const FRoutePlan& Plan = Agent->Follower.Plan;
	TestTrue(TEXT("step 0 ends at the FIRST twin, the one its edge reaches"), Plan.Steps[0].To == Now.B1);
	TestTrue(TEXT("step 1 is the zero-length path to the second"), Plan.Steps[1].To == Now.B2);
	TestTrue(TEXT("step 2 ends at C"), Plan.Steps[2].To == Now.C);
	TestTrue(TEXT("the goal is C's new handle"), Agent->GoalNode == Now.C);
	TestEqual(TEXT("the route's length is unchanged"), Plan.Length, LengthWas, 1e-9);
	return true;
}

namespace TwinFixture
{
	/** A->B1, B1->B2 (zero length), and B2->C when bToC; B1 and B2 at one point. */
	struct FTwins { FGuidelineNodeId A, B1, B2, C; };

	/**
	 * Swept and rebuilt the way FRoadGuidelineBuilder rebuilds: every node and edge removed and
	 * re-added with new handles. bSwap adds B2's node before B1's, which is what decides the slot
	 * order FindNearestNode breaks the positional tie by - so a test can make the lookup name
	 * whichever twin it needs to be WRONG.
	 */
	FTwins Build(URoadNetwork& Net, bool bSwap, bool bToC)
	{
		TArray<FGuidelineEdgeId> Edges;
		for (int32 I = 0; I < Net.GetGuidelineEdges().Num(); ++I) { if (Net.GetGuidelineEdges()[I].bAlive) { Edges.Add(Net.GuidelineEdgeIdAt(I)); } }
		for (const FGuidelineEdgeId& Id : Edges) { Net.RemoveGuidelineEdge(Id); }
		for (int32 I = 0; I < Net.GetGuidelineNodes().Num(); ++I) { if (Net.GetGuidelineNodes()[I].bAlive) { Net.RemoveGuidelineNode(Net.GuidelineNodeIdAt(I)); } }
		FTwins G;
		G.A = Net.AddGuidelineNode(FVector2D(0.0, 0.0));
		if (bSwap) { G.B2 = Net.AddGuidelineNode(FVector2D(20000.0, 0.0)); G.B1 = Net.AddGuidelineNode(FVector2D(20000.0, 0.0)); }
		else { G.B1 = Net.AddGuidelineNode(FVector2D(20000.0, 0.0)); G.B2 = Net.AddGuidelineNode(FVector2D(20000.0, 0.0)); }
		G.C = Net.AddGuidelineNode(FVector2D(40000.0, 0.0));
		TestGraph::Join(Net, G.A, G.B1);
		TestGraph::Join(Net, G.B1, G.B2);
		if (bToC) { TestGraph::Join(Net, G.B2, G.C); }
		return G;
	}
}

// THE FIRST STEP'S START MAY BE THE WRONG TWIN (review of 0277a642). An agent on step B2->C:
// the node its step leaves from is found by position, and the lookup names B1. B1->C has no
// edge, so the re-resolver must take B2 from B1's twins AND re-point Steps[FromStep-1].To at
// it - every tick's StepFromNode reads that handle (crossing arm, tail claim, RankAt, replan
// start). Run under both slot orders; the one where the lookup names B1 is the case.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRebuildTwinAtCurrentStepTest,
	"Airside.Model.Traffic.RebuildTwinAtCurrentStep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRebuildTwinAtCurrentStepTest::RunTest(const FString& Parameters)
{
	int32 Exercised = 0;
	for (const bool bSwap : { false, true })
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const TwinFixture::FTwins Was = TwinFixture::Build(*Net, bSwap, true);
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, Was.A, Was.C, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
		TickUntil(*Traffic, *Net, 120.0, [&](int32)
		{
			const FRoadAgent* A = Traffic->FindAgent(Van);
			return A != nullptr && A->Follower.Plan.Steps.Num() == 3 && A->Follower.Travelled <= A->Follower.Plan.Steps[1].EndDistance + 500.0;
		});
		const FRoadAgent* Agent = Traffic->FindAgent(Van);
		if (!TestTrue(TEXT("the van is out on a three-step plan"), Agent != nullptr && Agent->Follower.Plan.Steps.Num() == 3)) { return false; }
		if (!TestEqual(TEXT("and on step 2, B2->C"), UGroundTraffic::CurrentStep(Agent->Follower.Plan, Agent->Follower.Travelled), 2)) { return false; }

		const TwinFixture::FTwins Now = TwinFixture::Build(*Net, bSwap, true);
		if (RouteSearch::FindNearestNode(*Net, FVector2D(20000.0, 0.0), ETraversalClass::GroundVehicle, 25.0) != Now.B1)
		{
			continue;   // the lookup names the right twin under this order - not the case under test
		}
		++Exercised;
		Traffic->OnGraphRebuilt(*Net);
		const FGraphRebuildSummary Summary = Traffic->GetLastRebuildSummaryForTest();
		TestEqual(TEXT("re-resolved, NOT replanned: nothing was lost"), Summary.Replanned, 0);
		TestEqual(TEXT("nor truncated"), Summary.Truncated, 0);
		Agent = Traffic->FindAgent(Van);
		TestTrue(TEXT("the node the current step leaves from is re-pointed at B2, the twin its edge leaves - not the B1 the lookup named"),
			UGroundTraffic::StepFromNode(Agent->Follower.Plan, 2) == Now.B2);
		TestTrue(TEXT("and the step itself ends at C"), Agent->Follower.Plan.Steps[2].To == Now.C);
	}
	TestTrue(TEXT("one slot order made the lookup name the wrong twin - the case was exercised"), Exercised > 0);
	return true;
}

// A ROUTE THAT ENDS AT A TWIN (review of 0277a642): A->B1->B2, goal B2. Every step re-resolves,
// so the goal must be the LAST STEP'S END, chosen by the edge that reaches it - a position lookup
// names B1, and every later replan would search to the wrong node.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRebuildGoalIsATwinTest,
	"Airside.Model.Traffic.RebuildGoalIsATwin",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRebuildGoalIsATwinTest::RunTest(const FString& Parameters)
{
	int32 Exercised = 0;
	for (const bool bSwap : { false, true })
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const TwinFixture::FTwins Was = TwinFixture::Build(*Net, bSwap, false);
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, Was.A, Was.B2, ETraversalClass::GroundVehicle), TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
		TickUntil(*Traffic, *Net, 5.0, [](int32) { return true; });
		const FRoadAgent* Agent = Traffic->FindAgent(Van);
		if (!TestTrue(TEXT("the van is out, A->B1->B2"), Agent != nullptr && Agent->Follower.Plan.Steps.Num() == 2)) { return false; }

		const TwinFixture::FTwins Now = TwinFixture::Build(*Net, bSwap, false);
		if (RouteSearch::FindNearestNode(*Net, FVector2D(20000.0, 0.0), ETraversalClass::GroundVehicle, 25.0) != Now.B1)
		{
			continue;
		}
		++Exercised;
		Traffic->OnGraphRebuilt(*Net);
		TestEqual(TEXT("re-resolved, NOT replanned"), Traffic->GetLastRebuildSummaryForTest().Replanned, 0);
		Agent = Traffic->FindAgent(Van);
		TestTrue(TEXT("the last step ends at B2"), Agent->Follower.Plan.Steps.Last().To == Now.B2);
		TestTrue(TEXT("and the goal is B2, the last step's end - not the B1 a position lookup names"), Agent->GoalNode == Now.B2);
	}
	TestTrue(TEXT("one slot order made the lookup name the wrong twin - the case was exercised"), Exercised > 0);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * FindAgent SURVIVES A STALE AgentIndex.
 *
 * PR review on issue #295: AgentIndex is not a UPROPERTY (see its own comment in
 * GroundTraffic.h) - a duplicated UGroundTraffic (PIE's level duplication) copies Agents, a
 * reflected UPROPERTY, but this plain C++ member is invisible to DuplicateObject's property
 * walk, so a duplicate starts with it default-constructed empty regardless of how many agents
 * came along. Every FindAgent on the duplicate would silently miss until the next
 * Admit/RetireAgent/AdvanceOnce-removal happened to rebuild it - which, for a level that admits
 * nothing new, could be never.
 *
 * ClearAgentIndexForTest recreates exactly that shape (Agents keeps its entry, AgentIndex is
 * emptied) without staging an actual PIE duplication, which nothing in this test module can
 * drive headlessly. FindIndex's count-mismatch guard is what this pins.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficFindAgentSurvivesAStaleIndexTest,
	"Airside.Model.Traffic.FindAgentSurvivesAStaleIndex",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficFindAgentSurvivesAStaleIndexTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 20000.0, 0.0);
	TestGraph::Join(*Net, A, B);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, B, ETraversalClass::GroundVehicle),
		TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }
	if (!TestTrue(TEXT("found before the index is cleared"), Traffic->FindAgent(Id) != nullptr)) { return false; }

	// THE FAULT INJECTED: Agents keeps its one entry, AgentIndex is emptied - exactly what a
	// duplicate that skipped this plain C++ member looks like on its first lookup.
	Traffic->ClearAgentIndexForTest();

	const FRoadAgent* Found = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("FindAgent still resolves: the count mismatch rebuilds the index"), Found != nullptr))
	{
		return false;
	}
	TestEqual(TEXT("and the id it finds is the one dispatched"), Found->Id, Id);
	return true;
}


// ---------------------------------------------------------------------------------------
namespace
{
	/**
	 * A SERVICE TRUCK'S BAY, HAND-AUTHORED (issue #434): the four-leg cycle's arrive, reverse and
	 * depart legs as three one-way edges, on one straight line so the geometry is not the thing under
	 * test. Approach -> Service is the serve leg, Service -> Cleared is the REVERSE leg (backed up
	 * the line the truck came in on, so the body never turns), Cleared -> Exit drives on. Side is a
	 * second road joining the reverse leg's far end, where another vehicle can be refused.
	 *
	 * HAND-AUTHORED, NOT A SOLVE, for the reason the fuel fixtures give: what is under test is what
	 * the traffic model does with a Reversing agent, and a fixture that had to lay pavement first
	 * would fail for reasons that have nothing to do with it. The edges are laid as
	 * FStandLayoutBuild lays a layout's - GroundVehicle only, one-way, the reverse leg marked.
	 */
	struct FBackingBay
	{
		URoadNetwork* Net = nullptr;
		FGuidelineNodeId Approach;
		FGuidelineNodeId Service;
		FGuidelineNodeId Cleared;
		FGuidelineNodeId Exit;
		FGuidelineNodeId Side;
		FGuidelineEdgeId ReverseEdge;
		FGuidelineEdgeId ExitEdge;
		FRoutePlan Route;
		FVehicle Truck;

		static constexpr double ExitX = 8000.0;
		static constexpr double ExitY = 4500.0;

		FGuidelineEdgeId Lay(FGuidelineNodeId From, FGuidelineNodeId To, bool bReverse, EGuidelineDir Direction = EGuidelineDir::AToB) const
		{
			FGuidelineEdge Edge;
			Edge.A = From;
			Edge.B = To;
			Edge.Control = (Net->GetGuidelineNode(From)->Position + Net->GetGuidelineNode(To)->Position) * 0.5;
			Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
			Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
			Edge.Direction = Direction;
			Edge.Width = 400.0;
			Edge.bDerived = false;
			Edge.bReverseLeg = bReverse;
			return Net->AddGuidelineEdge(MoveTemp(Edge));
		}

		static FBackingBay Build()
		{
			FBackingBay Bay;
			Bay.Net = NewObject<URoadNetwork>(GetTransientPackage());
			URoadNetwork& N = *Bay.Net;
			Bay.Approach = TestGraph::Node(N, 0.0, 6000.0);
			Bay.Service = TestGraph::Node(N, 0.0, 2000.0);
			Bay.Cleared = TestGraph::Node(N, 0.0, 4500.0);
			Bay.Exit = TestGraph::Node(N, ExitX, ExitY);
			Bay.Side = TestGraph::Node(N, -8000.0, 4500.0);
			Bay.Lay(Bay.Approach, Bay.Service, false);
			Bay.ReverseEdge = Bay.Lay(Bay.Service, Bay.Cleared, true);
			Bay.ExitEdge = Bay.Lay(Bay.Cleared, Bay.Exit, false);
			Bay.Lay(Bay.Side, Bay.Cleared, false, EGuidelineDir::Bidirectional);

			Bay.Route = TestGraph::Probe(N, Bay.Approach, Bay.Exit, ETraversalClass::GroundVehicle);
			// THE LARGEST VEHICLE'S CHASSIS IN THE DEFAULT VEHICLE'S BUNDLE, as the reverse tests in
			// ServiceLinkTest and RoadAgentTest do - StartDrive takes a whole FVehicle.
			Bay.Truck = UAirsideSettings::ResolveDefaultVehicle();
			Bay.Truck.Chassis = UAirsideSettings::ResolveLargestServiceVehicle();
			return Bay;
		}

		/** The route is the three legs, the middle one marked as a reverse - or the fixture is wrong. */
		bool RouteIsTheThreeLegs() const
		{
			return Route.IsValid() && Route.Steps.Num() == 3 && !Route.Steps[0].bReverseLeg
				&& Route.Steps[1].bReverseLeg && !Route.Steps[2].bReverseLeg;
		}
	};

	/** Ticks until the agent is Reversing (or gives up), so a test starts from the leg it is about. */
	bool TickUntilReversing(UGroundTraffic& Traffic, const URoadNetwork& Net, int32 AgentId)
	{
		return RunUntil(Traffic, Net, 90.0, [&]()
		{
			const FRoadAgent* Agent = Traffic.FindAgent(AgentId);
			return Agent != nullptr && Agent->Phase == EAgentPhase::Reversing;
		});
	}
}

// ---------------------------------------------------------------------------------------
/**
 * A REVERSING VEHICLE HOLDS ITS WHOLE SPAN AND MAKES OTHERS WAIT FOR IT (issue #434).
 *
 * EAgentPhase::Reversing was never added to FRoadAgent::IsOnRoute, so every claim pass gave a
 * backing truck HoldRunwayOnly: every node and edge claim dropped, StopWithin unbounded. It held
 * no ground and never yielded for the whole 25 m leg - and the claim pass holds a PUSH's whole
 * manoeuvre precisely because "a manoeuvre with no replan is a jam the resolver cannot break".
 *
 * Measured against the table, not the pass's own bookkeeping: an asker that is nobody
 * (ExcludingAgent 999) is refused the reverse leg's edge and its far node, and the holder it is
 * told about is the truck. And then behaviourally: a van sent to that far node stops short of it,
 * waiting on the truck.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReversingHoldsItsSpanTest,
	"Airside.Model.Traffic.ReversingHoldsItsSpan",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReversingHoldsItsSpanTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	if (!TestTrue(TEXT("the bay routes as arrive, reverse, depart"), Bay.RouteIsTheThreeLegs())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Truck = Traffic->DispatchAgent(Bay.Net, Bay.Route, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the truck is dispatched"), Truck > 0)) { return false; }
	if (!TestTrue(TEXT("and backs off the service point"), TickUntilReversing(*Traffic, *Bay.Net, Truck))) { return false; }

	// A HANDFUL OF TICKS MORE: the claim pass that saw the truck Taxiing ran at the top of the tick
	// that armed the reverse. The passes after it are the ones that see Reversing.
	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	if (!TestEqual(TEXT("still reversing"), Traffic->FindAgent(Truck)->Phase, EAgentPhase::Reversing)) { return false; }

	constexpr int32 Nobody = 999;
	int32 Holder = 0;
	TestTrue(TEXT("a second agent is refused the reverse leg's edge"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfEdge(Bay.ReverseEdge), Nobody, &Holder));
	TestEqual(TEXT("and the holder is the truck"), Holder, Truck);
	Holder = 0;
	TestTrue(TEXT("and the node the reverse leg ends on"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Bay.Cleared), Nobody, &Holder));
	TestEqual(TEXT("held by the truck too"), Holder, Truck);

	// AND A REVERSE IS NOT REPLANNABLE, which is why holding it matters: widening IsOnRoute must not
	// have made ReplanAt (Taxiing only, by name) take it.
	TestFalse(TEXT("ReplanAt refuses a reversing agent"),
		Traffic->ReplanAt(Truck, *Bay.Net, 2, FGuidelineEdgeId()));

	// AND THE REFUSAL BITES: a van driven at that node, from the side, while the truck is still
	// backing toward it is refused by the truck and waits on it. Before the fix the truck held
	// nothing, so the van was never refused at all.
	//
	// ASSERTED AS A REFUSAL, and that is still all this test is about. It used to have to say why it
	// did NOT assert "the van never gets the node", because it did get it: the far node is held as a
	// RESERVATION (the truck's centre is still metres away), and a vehicle refused at a node stopped
	// VehicleGap (300) short of it - inside the half footprint (335) at which its own claim turns
	// OCCUPIED, which the table lets take a reservation. So the van, being nearer the node, won it a
	// few seconds later and the truck was the one held (measured 2026-09-30, tick 288 of this loop).
	// That is fixed (#455 item 6: GapFor floors a gap at half the footprint, so the refused van waits
	// OUTSIDE that zone and the reserver keeps the node) - pinned by
	// Airside.Model.Traffic.ReservedNodeKeepsItsReserver and, for this very pair, by the truck never
	// waiting on the van in ReversingTruckAndVanMeetingAtTheSpanEnd.
	const int32 Van = Traffic->DispatchAgent(Bay.Net,
		TestGraph::Probe(*Bay.Net, Bay.Side, Bay.Cleared, ETraversalClass::GroundVehicle),
		TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("a van is dispatched at the far node"), Van > 0)) { return false; }
	bool bVanWaitedOnTruck = false;
	RunUntil(*Traffic, *Bay.Net, 40.0, [&]()
	{
		const FRoadAgent* V = Traffic->FindAgent(Van);
		const FRoadAgent* T = Traffic->FindAgent(Truck);
		if (V == nullptr || T == nullptr) { return true; }
		bVanWaitedOnTruck |= T->Phase == EAgentPhase::Reversing && V->GetWaitingOn() == Truck;
		return bVanWaitedOnTruck;
	});
	TestTrue(TEXT("the van was refused by, and waited on, the reversing truck"), bVanWaitedOnTruck);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * AN EDIT DURING A REVERSE LEAVES THE TRUCK A LIVE ROUTE AND A LIVE CLAIM (issue #434, item 3).
 *
 * OnGraphRebuilt had an arm for Taxiing, Manoeuvring, Arriving and Parked/Stranded - none for
 * Reversing, so Follower.Plan (the remainder the truck resumes on once it has backed out, cut from
 * ResumeStep) and GoalNode kept naming handles the rebuild had freed. After backing out the truck
 * drove a route whose claims matched nothing - invisible to arbitration - to a dead goal, until the
 * next edit.
 *
 * THE WAY BACK IS REDRAWN while the truck is halfway along the reverse: the reverse leg's far node
 * and the exit's end node removed (which takes their edges with them) and laid again on the same
 * spots, so every handle is new and every position is unchanged - what a solve does to derived
 * lines. After the truck has backed out, the route it drives must name only live handles, its goal
 * must be live, and its claim must be on the LIVE edge, where a second agent can see it. The
 * rebuild itself runs a claim pass over the reversing truck's route with the span's own handles
 * dead, so that it survives that is asserted too (it is a rebuild, not a crash).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRebuildDuringReverseTest,
	"Airside.Model.Traffic.RebuildDuringReverse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRebuildDuringReverseTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	if (!TestTrue(TEXT("the bay routes as arrive, reverse, depart"), Bay.RouteIsTheThreeLegs())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Truck = Traffic->DispatchAgent(Bay.Net, Bay.Route, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the truck is dispatched"), Truck > 0)) { return false; }
	if (!TestTrue(TEXT("and backs off the service point"), TickUntilReversing(*Traffic, *Bay.Net, Truck))) { return false; }
	// HALFWAY BACK, not at the start: the reverse is played from a snapshot of its span, and an
	// edit that lands mid-leg is the case the player can make.
	for (int32 Tick = 0; Tick < 200; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	if (!TestEqual(TEXT("still reversing when the edit lands"), Traffic->FindAgent(Truck)->Phase, EAgentPhase::Reversing)) { return false; }

	Bay.Net->RemoveGuidelineNode(Bay.Cleared);
	Bay.Net->RemoveGuidelineNode(Bay.Exit);
	const FGuidelineNodeId Cleared = TestGraph::Node(*Bay.Net, 0.0, 4500.0);
	const FGuidelineNodeId Exit = TestGraph::Node(*Bay.Net, FBackingBay::ExitX, FBackingBay::ExitY);
	Bay.Lay(Bay.Service, Cleared, true);
	const FGuidelineEdgeId ExitEdge = Bay.Lay(Cleared, Exit, false);
	Traffic->OnGraphRebuilt(*Bay.Net);

	const FRoadAgent* Agent = Traffic->FindAgent(Truck);
	if (!TestNotNull(TEXT("the truck survives the rebuild"), Agent)) { return false; }
	TestEqual(TEXT("still reversing: the rebuild does not end the back-out"), Agent->Phase, EAgentPhase::Reversing);
	TestTrue(TEXT("its goal was re-pointed at the redrawn exit"), Agent->GoalNode == Exit);

	// BACKED OUT, and driving on.
	if (!TestTrue(TEXT("it backs out and picks the taxi up"), RunUntil(*Traffic, *Bay.Net, 60.0, [&]()
		{
			const FRoadAgent* T = Traffic->FindAgent(Truck);
			return T != nullptr && T->Phase != EAgentPhase::Reversing;
		}))) { return false; }
	Agent = Traffic->FindAgent(Truck);
	if (!TestNotNull(TEXT("the truck is still there"), Agent)) { return false; }
	TestEqual(TEXT("and drives on"), Agent->Phase, EAgentPhase::Taxiing);

	int32 Dead = 0;
	for (const FRouteStep& Step : Agent->Follower.Plan.Steps)
	{
		Dead += (Bay.Net->GetGuidelineEdge(Step.Edge) == nullptr || Bay.Net->GetGuidelineNode(Step.To) == nullptr) ? 1 : 0;
	}
	TestEqual(TEXT("every step of the route it drives names a live edge and node"), Dead, 0);
	TestTrue(TEXT("and its goal is live"), Bay.Net->GetGuidelineNode(Agent->GoalNode) != nullptr);

	// A CLAIM ON THE LIVE EDGE, which is what "visible to arbitration" means. A few ticks on so a
	// claim pass has run over the route it is now driving.
	for (int32 Tick = 0; Tick < 5; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	int32 Holder = 0;
	TestTrue(TEXT("the truck's claim is on the redrawn exit, where another agent would meet it"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfEdge(ExitEdge), 999, &Holder));
	TestEqual(TEXT("held by the truck"), Holder, Truck);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A REVERSING TRUCK AND A VAN THAT MEET AT ITS SPAN'S END BOTH GET THROUGH (issue #434, review of #453).
 *
 * THE QUESTION: holding a reversing vehicle's ground makes a refused reverse reachable for the
 * first time, and the deadlock resolver looks only at waiters whose stall clock has run. Can two
 * vehicles that meet at a reverse's far end jam for good, out of the resolver's sight? The scenario
 * ReversingHoldsItsSpan measured - the van, nearer the far node, wins it from the truck's
 * reservation and the truck stops short - is run to the END, with the van bound for the SAME exit
 * as the truck, so that once it stands on the node it needs the edge the truck has reserved just
 * past the span, while the truck needs the node the van is standing on.
 *
 * THE ANSWER, measured 2026-09-30: NO. The van drove on across the node, the truck resumed ("Agent 1
 * resumes") and finished backing out, both inside 60 s of game time. What happened was that both
 * bodies stood on the node at once (the pass logged "overlaps"): the gap-below-half-footprint
 * behaviour the span test's comment describes, in which the refused van, nearer the node, took it from
 * the truck's reservation and the truck was the one held.
 *
 * AND SINCE #455 (item 6) THE RESERVER KEEPS ITS NODE: GapFor floors a gap at half the footprint, so
 * the refused van waits outside the zone where its own claim is an occupancy and the truck is never
 * refused by it. The order is the other way round - the truck finishes backing out and drives on, THEN
 * the van crosses. The truck waiting on the van is now asserted FALSE, where it used to be the proof that they met.
 *
 * ASSERTED ON AN END STATE (review of #466): the truck backs out, drives the exit and PARKS; the van crosses the far
 * node and is either parked too or stands waiting on that parked truck, because the exit node is both vehicles' goal
 * and a goal somebody stands on is not one anyone parks on. A permanent hold of the van after it crossed is neither,
 * and fails. MEASURED 2026-09-30: 66.2 s of game time after the van's dispatch, the van waiting on the parked truck,
 * so the bound is 100 s (about 1.5x), not a round guess - the 60 s it used to be was the old order, where the van
 * won the race for the node.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReversingTruckAndVanMeetingAtTheSpanEndTest,
	"Airside.Model.Traffic.ReversingTruckAndVanMeetingAtTheSpanEnd",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReversingTruckAndVanMeetingAtTheSpanEndTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	if (!TestTrue(TEXT("the bay routes as arrive, reverse, depart"), Bay.RouteIsTheThreeLegs())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Truck = Traffic->DispatchAgent(Bay.Net, Bay.Route, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the truck is dispatched"), Truck > 0)) { return false; }
	if (!TestTrue(TEXT("and backs off the service point"), TickUntilReversing(*Traffic, *Bay.Net, Truck))) { return false; }
	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}

	// THE SAME EXIT AS THE TRUCK: side road, across the span's far node, out along the exit edge.
	const FRoutePlan VanRoute = TestGraph::Probe(*Bay.Net, Bay.Side, Bay.Exit, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the van routes across the far node and out the exit"), VanRoute.IsValid() && VanRoute.Steps.Num() == 2)) { return false; }
	const int32 Van = Traffic->DispatchAgent(Bay.Net, VanRoute, TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the van is dispatched"), Van > 0)) { return false; }

	// THE VAN HAS CROSSED THE FAR NODE once it is a little way along the exit step: its first step ends there.
	const double VanCrosses = VanRoute.Steps[0].EndDistance + 100.0;
	// MEASURED (see the comment above the test): the bound is about 1.5x the time this takes, not a round guess.
	constexpr double MeetingBoundSeconds = 100.0;
	bool bVanWaitedOnTruck = false;
	bool bTruckWaitedOnVan = false;
	int32 Ticks = 0;
	// THE END STATE (#455, review of #466): the truck has finished - backed out, driven the exit and PARKED - and the van
	// has crossed the far node and is either parked too or stands waiting on that parked truck for the exit node they
	// share as a goal. A permanent hold of the van after it crossed is neither, and fails.
	const bool bBothDone = RunUntil(*Traffic, *Bay.Net, MeetingBoundSeconds, [&]()
	{
		++Ticks;
		const FRoadAgent* V = Traffic->FindAgent(Van);
		const FRoadAgent* T = Traffic->FindAgent(Truck);
		if (V == nullptr || T == nullptr) { return true; }
		bVanWaitedOnTruck |= T->Phase == EAgentPhase::Reversing && V->GetWaitingOn() == Truck;
		bTruckWaitedOnVan |= T->Phase == EAgentPhase::Reversing && T->GetWaitingOn() == Van;
		return T->Phase == EAgentPhase::Parked && V->Follower.Travelled > VanCrosses
			&& (V->Phase == EAgentPhase::Parked || V->GetWaitingOn() == Truck);
	});

	const FRoadAgent* V = Traffic->FindAgent(Van);
	const FRoadAgent* T = Traffic->FindAgent(Truck);
	if (!TestTrue(TEXT("both agents are still there"), V != nullptr && T != nullptr)) { return false; }
	AddInfo(FString::Printf(TEXT("done in %.2f s of game time after the van's dispatch; van waited on truck: %d, truck waited on van: %d; truck phase %d at reverse travelled %.0f, van phase %d waiting on %d, %d cycle(s) seen, %d yield(s)"),
		Ticks * 0.05, bVanWaitedOnTruck ? 1 : 0, bTruckWaitedOnVan ? 1 : 0, static_cast<int32>(T->Phase), T->Reverse.Travelled, static_cast<int32>(V->Phase),
		V->GetWaitingOn(), Traffic->GetCyclesDetectedForTest(), Traffic->GetYieldsForTest()));
	// THE TWO MET, or the outcome below proves nothing: the van was refused by the truck while the truck
	// was still backing. A van that arrives after the truck has gone would pass this vacuously.
	TestTrue(TEXT("the van was refused by the reversing truck"), bVanWaitedOnTruck);
	TestFalse(TEXT("and the truck was NOT refused by the van: it reserved the node first and keeps it (#455 item 6; red while the refused van stopped inside the occupied zone)"),
		bTruckWaitedOnVan);
	TestTrue(TEXT("the truck finished: backed out, drove the exit and parked (it is not held for good by the van)"), T->Phase == EAgentPhase::Parked);
	TestTrue(TEXT("and the van crossed the far node and drove on (it is not held for good by the truck)"), V->Follower.Travelled > VanCrosses);
	TestTrue(TEXT("and it is parked, or waits on the truck now parked on their shared exit - not held by anything else"),
		V->Phase == EAgentPhase::Parked || (V->GetWaitingOn() == Truck && T->Phase == EAgentPhase::Parked));
	TestTrue(TEXT("all of it inside the bound"), bBothDone);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * IsOnRoute NAMES EVERY PHASE THAT WALKS A ROUTE, AND THE TABLE HERE IS EXHAUSTIVE (issue #434).
 *
 * The same omission has now happened twice: the push (which is why IsOnRoute exists) and then
 * Reversing, added on 2026-09-17 and never classified. This lists every EAgentPhase and what the
 * claim pass must do with it, and FAILS on a phase it does not name - so the next one added has
 * to be decided here rather than falling into HoldRunwayOnly by default.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficIsOnRouteClassifiesEveryPhaseTest,
	"Airside.Model.Traffic.IsOnRouteClassifiesEveryPhase",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficIsOnRouteClassifiesEveryPhaseTest::RunTest(const FString& Parameters)
{
	// bReplannable IS THE SECOND COLUMN (#455): the deadlock resolver, ReplanAt and the alert's filter all ask
	// IsReplannable, and the answer is a taxi and nothing else - a push and a reverse have no other line to be sent
	// along. Named per phase here so a phase added is decided for both questions, not just the first.
	struct FPhaseAnswer
	{
		EAgentPhase Phase;
		bool bOnRoute;
		bool bReplannable;
	};
	const FPhaseAnswer Table[] = {
		{ EAgentPhase::Arriving,    false, false },   // on the runway: FLandingRun, the strip's claims only
		{ EAgentPhase::Taxiing,     true,  true },
		{ EAgentPhase::Departing,   false, false },   // on the runway: FTakeoffRun
		{ EAgentPhase::Parked,      false, false },   // stands on its own node
		{ EAgentPhase::Manoeuvring, true,  false },   // a push walks its lead-in: the only line off the stand
		{ EAgentPhase::Reversing,   true,  false },   // a back-out walks a span of its route: the bay's only leg
		{ EAgentPhase::Gone,        false, false },
		{ EAgentPhase::Stranded,    false, false },   // its route died
	};

	const UEnum* Enum = StaticEnum<EAgentPhase>();
	if (!TestNotNull(TEXT("EAgentPhase reflects"), Enum)) { return false; }
	for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
	{
		const FString Name = Enum->GetNameStringByIndex(Index);
		if (Name.EndsWith(TEXT("_MAX"))) { continue; }
		const EAgentPhase Phase = static_cast<EAgentPhase>(Enum->GetValueByIndex(Index));
		const FPhaseAnswer* Answer = nullptr;
		for (const FPhaseAnswer& Row : Table)
		{
			if (Row.Phase == Phase) { Answer = &Row; }
		}
		if (!TestNotNull(*FString::Printf(TEXT("phase %s is classified by this test"), *Name), Answer))
		{
			continue;
		}
		FRoadAgent Agent;
		Agent.Phase = Phase;
		TestEqual(*FString::Printf(TEXT("IsOnRoute for %s"), *Name), Agent.IsOnRoute(), Answer->bOnRoute);
		TestEqual(*FString::Printf(TEXT("IsReplannable for %s"), *Name), Agent.IsReplannable(), Answer->bReplannable);
	}
	return true;
}

// ---------------------------------------------------------------------------------------
namespace
{
	/**
	 * A TAXIING WAITER WITH A NEW STAND JUST PLACED (issue #435) - the fixture ExtendRouteMovesTheGoal's
	 * cases 2 and 3 use, built up to the frame a stand has appeared and the graph been rebuilt, and no
	 * further: the re-offer that runs at the end of the next Advance has not run yet.
	 *
	 * An arrival whose stands are all removed lands and taxis on with bAwaitingStand set; then one
	 * stand appears beside the exit. Shared by the test that the re-offer extends it without a jump and
	 * the one that a refused extension leaves it waiting, which are the two halves of one decision.
	 */
	struct FTaxiingWaiter
	{
		FAirframe Piper = TestAirframes::Piper();
		FTestAirport Air;
		UGroundTraffic* Traffic = nullptr;
		int32 Id = 0;
		FEntityInstanceId Placed;

		/** False, with the failed step told to Test, when the staging did not happen as described. */
		bool Build(FAutomationTestBase& Test)
		{
			Air = FTestAirport::Build(Piper, { .StandCount = 2 });
			Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
			Id = Traffic->DispatchArrival(*Air.Net, Air.Threshold, Piper, 1.0);
			if (!Test.TestTrue(TEXT("the arrival is dispatched"), Id > 0)) { return false; }
			Traffic->Advance(0.05, Air.Net);
			const FGuidelineNodeId Goal0 = Traffic->FindAgent(Id)->GoalNode;
			const FEntityInstanceId Target = (Goal0 == Air.Pose(Air.Stands[0])) ? Air.Stands[0] : Air.Stands[1];
			const FEntityInstanceId Spare = (Target == Air.Stands[0]) ? Air.Stands[1] : Air.Stands[0];
			Air.Net->RemoveEntity(Target);
			TestGraph::Rebuild(*Air.Net);
			Traffic->OnGraphRebuilt(*Air.Net);
			Air.Net->RemoveEntity(Spare);
			TestGraph::Rebuild(*Air.Net);
			Traffic->OnGraphRebuilt(*Air.Net);
			if (!Test.TestTrue(TEXT("it lands and taxis on, waiting for a stand"), RunUntil(*Traffic, *Air.Net, 600.0,
				[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P && P->Phase == EAgentPhase::Taxiing && P->bAwaitingStand; })))
			{
				return false;
			}

			// A STAND APPEARS beside the same exit, while the aircraft is still short of its route's end.
			UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
			Placed = Air.Net->PlaceEntity(Stand, Stand->Anchors, Air.ExitAt + FVector2D(9000.0, -10000.0), 0.0);
			TestGraph::Rebuild(*Air.Net);
			Traffic->OnGraphRebuilt(*Air.Net);
			const FRoadAgent* Waiter = Traffic->FindAgent(Id);
			if (!Test.TestTrue(TEXT("still taxiing and waiting after the rebuild"), Waiter != nullptr && Waiter->Phase == EAgentPhase::Taxiing && Waiter->bAwaitingStand)) { return false; }
			const FGuidelineNode* GoalNode = Air.Net->GetGuidelineNode(Waiter->GoalNode);
			if (!Test.TestNotNull(TEXT("its goal, the end of its truncated route, is live"), GoalNode)) { return false; }

			// THE PRECONDITION THAT MAKES ANY ASSERTION ABOUT A JUMP MEAN SOMETHING: a redirect from here
			// would move it by this much in one frame.
			const double ToGoal = FVector2D::Distance(Waiter->LastMotion.Position, GoalNode->Position);
			return Test.TestTrue(*FString::Printf(TEXT("the aircraft is well short of the end of its route (%.0f uu)"), ToGoal), ToGoal > 2000.0);
		}
	};
}

// ---------------------------------------------------------------------------------------
/**
 * A TAXIING WAITER IS EXTENDED ONTO A FREED STAND, NEVER TELEPORTED TO IT (issue #435).
 *
 * A rebuild that removes an arrival's stand with no free alternative truncates its route and marks
 * it awaiting WHILE IT IS STILL TAXIING; GoalNode is then the END of the truncated route, ahead of
 * the moving aircraft. ReofferStands gathered Taxiing waiters and gave them to RedirectAgent, which
 * restarts an aircraft from REST at the new route's first point - the end of the route it had not
 * yet driven. The aircraft jumped to it.
 *
 * THE EXISTING WAITER FIXTURE (ExtendRouteMovesTheGoal, cases 2 and 3) with its hand-made
 * ExtendRoute replaced by the Advance that runs the re-offer - which is what that test never drove.
 * Asserted on every frame: no tick moves the aircraft further than the taxi speed allows.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReofferTaxiingWaiterDoesNotJumpTest,
	"Airside.Model.Traffic.ReofferTaxiingWaiterDoesNotJump",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReofferTaxiingWaiterDoesNotJumpTest::RunTest(const FString& Parameters)
{
	FTaxiingWaiter W;
	if (!W.Build(*this)) { return false; }
	UGroundTraffic* Traffic = W.Traffic;
	const FTestAirport& Air = W.Air;
	const FAirframe& Piper = W.Piper;
	const int32 Id = W.Id;
	const FEntityInstanceId Placed = W.Placed;
	const FRoadAgent* Waiter = Traffic->FindAgent(Id);

	// NO HAND-MADE EXTEND: the tick's own re-offer does it. Every frame's displacement is measured,
	// from the frame the stand appeared to the frame the aircraft parks.
	const double Dt = 0.05;
	const double PerFrame = Piper.Chassis.Ground.Taxi.SpeedCap * Dt + 1.0;
	FVector2D Last = Waiter->LastMotion.Position;
	double MaxStep = 0.0;
	int32 WorstTick = 0;
	int32 Tick = 0;
	bool bParked = false;
	for (; Tick < 20000 && !bParked; ++Tick)
	{
		Traffic->Advance(Dt, Air.Net);
		const FRoadAgent* P = Traffic->FindAgent(Id);
		if (P == nullptr) { break; }
		const double Step = FVector2D::Distance(Last, P->LastMotion.Position);
		if (Step > MaxStep) { MaxStep = Step; WorstTick = Tick; }
		Last = P->LastMotion.Position;
		bParked = P->Phase == EAgentPhase::Parked;
	}
	TestTrue(*FString::Printf(TEXT("no frame moved it more than one frame of taxi speed (worst %.1f uu at tick %d, budget %.1f)"), MaxStep, WorstTick, PerFrame),
		MaxStep <= PerFrame);

	// AND IT GOT THE STAND, by driving there.
	const FRoadAgent* Done = Traffic->FindAgent(Id);
	if (!TestNotNull(TEXT("the aircraft is still there"), Done)) { return false; }
	TestFalse(TEXT("it is no longer waiting"), Done->bAwaitingStand);
	TestTrue(TEXT("it parked on the stand that appeared"), bParked && Done->GoalNode == Air.Pose(Placed));
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A TAXIING WAITER WHOSE EXTENSION IS REFUSED KEEPS WAITING - IT IS NEVER REDIRECTED (issue #435, review of #453).
 *
 * ReofferStands extends a Taxiing waiter and never falls back to RedirectAgent, because the fallback
 * is the teleport again in the one case nothing had measured. Only the success path was tested.
 * The refusal is staged the one way an aircraft's extension can be refused: its goal is pointed
 * somewhere its live plan does not end (the plan's own start, where the taxi-in began), so the
 * route the re-offer plans from the goal does not start where Splice wants it to. The stand IS
 * reachable from there, so a redirect WOULD have been offered it.
 *
 * ASSERTED: the aircraft is still Taxiing and still waiting, no frame moves it further than taxi
 * speed allows, the "keeps waiting" line is logged, no "redirected" line is, and the pass consumed
 * the freed flag (it asks again only when something frees).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReofferRefusedExtensionKeepsWaitingTest,
	"Airside.Model.Traffic.ReofferRefusedExtensionKeepsWaiting",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReofferRefusedExtensionKeepsWaitingTest::RunTest(const FString& Parameters)
{
	FTaxiingWaiter W;
	if (!W.Build(*this)) { return false; }
	const FRoadAgent* Waiter = W.Traffic->FindAgent(W.Id);
	const FGuidelineNodeId LiveEnd = Waiter->GoalNode;
	const FGuidelineNodeId Start = Waiter->Follower.Plan.Start;
	if (!TestTrue(TEXT("the plan's start is a live node other than where it ends"),
		Start.IsSet() && Start != LiveEnd && W.Air.Net->GetGuidelineNode(Start) != nullptr)) { return false; }
	if (!TestTrue(TEXT("the goal is moved off the plan's end"), FGroundTrafficTestAccess(*W.Traffic).SetGoal(W.Id, Start))) { return false; }
	if (!TestTrue(TEXT("and the rebuild left the re-offer pending"), W.Traffic->StandsMayHaveFreedForTest())) { return false; }

	struct FSpy : public FOutputDevice
	{
		int32 KeepsWaiting = 0;
		int32 Redirected = 0;
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category != FName(TEXT("LogAirsideTraffic"))) { return; }
			const FString Line(V);
			KeepsWaiting += Line.Contains(TEXT("keeps waiting")) ? 1 : 0;
			Redirected += Line.Contains(TEXT(" redirected: ")) ? 1 : 0;
		}
	} Spy;

	const double Dt = 0.05;
	const double PerFrame = W.Piper.Chassis.Ground.Taxi.SpeedCap * Dt + 1.0;
	FVector2D Last = Waiter->LastMotion.Position;
	double MaxStep = 0.0;
	GLog->AddOutputDevice(&Spy);
	for (int32 Tick = 0; Tick < 60; ++Tick)
	{
		W.Traffic->Advance(Dt, W.Air.Net);
		const FRoadAgent* P = W.Traffic->FindAgent(W.Id);
		if (P == nullptr) { break; }
		MaxStep = FMath::Max(MaxStep, FVector2D::Distance(Last, P->LastMotion.Position));
		Last = P->LastMotion.Position;
	}
	GLog->RemoveOutputDevice(&Spy);

	const FRoadAgent* After = W.Traffic->FindAgent(W.Id);
	if (!TestNotNull(TEXT("the aircraft is still there"), After)) { return false; }
	TestEqual(TEXT("still Taxiing: the refused extension did not restart it"), After->Phase, EAgentPhase::Taxiing);
	TestTrue(TEXT("and still waiting for a stand"), After->bAwaitingStand);
	TestTrue(*FString::Printf(TEXT("no frame moved it more than one frame of taxi speed (worst %.1f uu, budget %.1f)"), MaxStep, PerFrame), MaxStep <= PerFrame);
	TestTrue(TEXT("the refusal was said: a keeps-waiting line"), Spy.KeepsWaiting >= 1);
	TestEqual(TEXT("and no redirect was made in its place"), Spy.Redirected, 0);
	TestFalse(TEXT("the pass consumed the freed flag - it asks again only when something frees"), W.Traffic->StandsMayHaveFreedForTest());
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A STRANDED WAITER IS OFFERED ITS STAND FROM WHERE IT STANDS, NEVER RESTARTED AT ITS GOAL NODE (#429 review).
 *
 * TWO REBUILDS. The first removes the arrival's stand while the only other one is held: no free stand, so its
 * route is truncated to a LIVE node ahead (W) and it taxis on, waiting. The second deletes the very edge it is
 * driving: stranded in place, still waiting - and its goal is still W, live, which a rebuild re-points only when it is
 * dead. Then the other stand frees. The re-offer used to treat a stranded waiter as one standing at its goal node (a
 * taxi-in stranded at its exit, #396) and redirect it from rest there: RedirectAgent restarts an aircraft at the new
 * route's first point, so it appeared at W in one frame - #435's teleport by another door. It is now given a stand
 * the way the player's Unstick gives one (chosen from the node ahead of where it stands, reached by RescueStranded's
 * hop onto pavement within RescueRejoinRadius), or it keeps waiting where it is.
 *
 * ASSERTED: no frame moves it further than a rescue's hop allows, and it is never put at W.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReofferStrandedWaiterDoesNotJumpTest,
	"Airside.Model.Traffic.ReofferStrandedWaiterDoesNotJump",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReofferStrandedWaiterDoesNotJumpTest::RunTest(const FString& Parameters)
{
	const FAirframe Piper = TestAirframes::Piper();
	FTestAirport Air = FTestAirport::Build(Piper, { .StandCount = 2 });
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Traffic->DispatchArrival(*Air.Net, Air.Threshold, Piper, 1.0);
	if (!TestTrue(TEXT("the arrival is dispatched"), Id > 0)) { return false; }
	Traffic->Advance(0.05, Air.Net);
	const FGuidelineNodeId Goal0 = Traffic->FindAgent(Id)->GoalNode;
	const FEntityInstanceId Target = (Goal0 == Air.Pose(Air.Stands[0])) ? Air.Stands[0] : Air.Stands[1];
	const FEntityInstanceId Spare = (Target == Air.Stands[0]) ? Air.Stands[1] : Air.Stands[0];

	// THE SPARE IS HELD, NOT REMOVED - an ops reservation, given back later without re-deriving the graph, which
	// would free W's handle and hide the case (a dead goal is re-pointed at where the aircraft stands).
	constexpr int32 Holder = 4242;
	if (!TestTrue(TEXT("the spare stand is held"), Traffic->HoldStand(Holder, Air.Pose(Spare)))) { return false; }

	// REBUILD ONE: its stand goes, the other is held - truncated to a live node ahead, and waiting.
	Air.Net->RemoveEntity(Target);
	TestGraph::Rebuild(*Air.Net);
	Traffic->OnGraphRebuilt(*Air.Net);
	if (!TestTrue(TEXT("it lands and taxis on, waiting for a stand"), RunUntil(*Traffic, *Air.Net, 600.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P && P->Phase == EAgentPhase::Taxiing && P->bAwaitingStand; })))
	{
		return false;
	}
	const FRoadAgent* Waiter = Traffic->FindAgent(Id);
	const FGuidelineNodeId W = Waiter->GoalNode;
	const FGuidelineNode* WNode = Air.Net->GetGuidelineNode(W);
	if (!TestNotNull(TEXT("its goal, the end of its truncated route, is live"), WNode)) { return false; }
	const FVector2D WAt = WNode->Position;

	// REBUILD TWO: the edge it is driving is deleted, the graph otherwise as it was - stranded in place. The hold is
	// given back in the same breath, so the re-offer this rebuild schedules finds the spare free.
	const int32 OnStep = UGroundTraffic::CurrentStep(Waiter->Follower.Plan, Waiter->Follower.Travelled);
	const FGuidelineEdgeId Under = Waiter->Follower.Plan.Steps[OnStep].Edge;
	const double Dt = 0.05;
	const double PerFrame = Piper.Chassis.Ground.Taxi.SpeedCap * Dt + 1.0;
	// How far a rescue may hop an agent onto pavement.
	constexpr double RescueHop = UGroundTraffic::RescueRejoinRadius;
	const double ToGoal = FVector2D::Distance(Waiter->LastMotion.Position, WAt);
	AddInfo(FString::Printf(TEXT("on step %d, %.0f uu short of W"), OnStep, ToGoal));
	if (!TestTrue(*FString::Printf(TEXT("W is further than a rescue's hop (%.0f uu): a restart there would show"), ToGoal),
		ToGoal > RescueHop + 2.0 * PerFrame)) { return false; }
	if (!TestTrue(TEXT("the edge under it is deleted"), Air.Net->RemoveGuidelineEdge(Under))) { return false; }
	Traffic->ReleaseHold(Holder);
	Traffic->OnGraphRebuilt(*Air.Net);
	// STRANDED IN PLACE: the rebuild marks its route dead (the phase follows on the next tick, whose re-offer then runs
	// in the same Advance - so the summary is the witness, not a Stranded phase seen between ticks).
	if (!TestEqual(TEXT("precondition: the second rebuild stranded it in place"), Traffic->GetLastRebuildSummaryForTest().Stranded, 1)) { return false; }
	if (!TestTrue(TEXT("precondition: still waiting, its goal still W"), Traffic->FindAgent(Id)->bAwaitingStand && Traffic->FindAgent(Id)->GoalNode == W)) { return false; }

	FVector2D Last = Traffic->FindAgent(Id)->LastMotion.Position;
	double MaxStep = 0.0;
	double ClosestToW = TNumericLimits<double>::Max();
	for (int32 Tick = 0; Tick < 60; ++Tick)
	{
		Traffic->Advance(Dt, Air.Net);
		const FRoadAgent* P = Traffic->FindAgent(Id);
		if (P == nullptr) { break; }
		MaxStep = FMath::Max(MaxStep, FVector2D::Distance(Last, P->LastMotion.Position));
		ClosestToW = FMath::Min(ClosestToW, FVector2D::Distance(P->LastMotion.Position, WAt));
		Last = P->LastMotion.Position;
	}
	const FRoadAgent* After = Traffic->FindAgent(Id);
	if (!TestNotNull(TEXT("the aircraft is still there"), After)) { return false; }
	AddInfo(FString::Printf(TEXT("after the re-offer: %s, waiting %s, worst frame %.0f uu, closest to W %.0f uu"),
		*UEnum::GetValueAsString(After->Phase), After->bAwaitingStand ? TEXT("yes") : TEXT("no"), MaxStep, ClosestToW));
	TestTrue(*FString::Printf(TEXT("no frame moved it further than a rescue's hop (worst %.0f uu, budget %.0f)"), MaxStep, RescueHop + PerFrame),
		MaxStep <= RescueHop + PerFrame);
	TestTrue(*FString::Printf(TEXT("and it was never put at W, the goal node it was not at (closest %.0f uu)"), ClosestToW),
		ClosestToW > RescueHop);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A STRANDED WAITER WITH PAVEMENT BESIDE IT IS RESCUED ONTO ITS STAND WHEN ONE FREES (#429 review round 2).
 *
 * The positive half of ReofferStrandedWaiterDoesNotJump, which passes as well for a re-offer that ignores stranded
 * waiters altogether: there the edge under the aircraft is deleted, so the rescue finds no pavement and nothing moves.
 * Here the route dies but the ground stays (FGroundTrafficTestAccess::Strand - a plan invalidated, the agent left
 * where it stood), so the pavement under it runs its way within RescueRejoinRadius. When the spare stand frees, the
 * re-offer must hop it onto that pavement and send it to the stand: Taxiing, its goal the stand, no longer waiting,
 * no frame moving it further than a hop - announced ReOffered, and driven by the taxi-in's own policy (the rescue's
 * line names the errand).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReofferStrandedWaiterIsRescuedTest,
	"Airside.Model.Traffic.ReofferStrandedWaiterIsRescued",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReofferStrandedWaiterIsRescuedTest::RunTest(const FString& Parameters)
{
	const FAirframe Piper = TestAirframes::Piper();
	FTestAirport Air = FTestAirport::Build(Piper, { .StandCount = 2 });
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Traffic->DispatchArrival(*Air.Net, Air.Threshold, Piper, 1.0);
	if (!TestTrue(TEXT("the arrival is dispatched"), Id > 0)) { return false; }
	Traffic->Advance(0.05, Air.Net);
	const FGuidelineNodeId Goal0 = Traffic->FindAgent(Id)->GoalNode;
	const FEntityInstanceId Target = (Goal0 == Air.Pose(Air.Stands[0])) ? Air.Stands[0] : Air.Stands[1];
	const FEntityInstanceId Spare = (Target == Air.Stands[0]) ? Air.Stands[1] : Air.Stands[0];
	constexpr int32 Holder = 4242;
	if (!TestTrue(TEXT("the spare stand is held"), Traffic->HoldStand(Holder, Air.Pose(Spare)))) { return false; }
	Air.Net->RemoveEntity(Target);
	TestGraph::Rebuild(*Air.Net);
	Traffic->OnGraphRebuilt(*Air.Net);
	if (!TestTrue(TEXT("it lands and taxis on, waiting for a stand"), RunUntil(*Traffic, *Air.Net, 600.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P && P->Phase == EAgentPhase::Taxiing && P->bAwaitingStand; })))
	{
		return false;
	}

	// STRANDED WITH THE GROUND UNDER IT, then the spare given back and a re-offer scheduled (a rebuild that changes
	// nothing - the flag every rebuild raises).
	if (!TestTrue(TEXT("its route dies where it stands"), FGroundTrafficTestAccess(*Traffic).Strand(Id))) { return false; }
	Traffic->ReleaseHold(Holder);
	Traffic->OnGraphRebuilt(*Air.Net);

	TArray<EAgentEvent> Causes;
	Traffic->OnAgentPhaseChanged.AddLambda([&Causes, Id](const FAgentTransition& T)
	{
		if (T.AgentId == Id && T.From == EAgentPhase::Stranded) { Causes.Add(T.Cause); }
	});
	struct FSpy : public FOutputDevice
	{
		FString Rescued;
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			const FString Line(V);
			if (Category == FName(TEXT("LogAirsideTraffic")) && Line.Contains(TEXT(" rescued: "))) { Rescued = Line; }
		}
	} Spy;

	const double Dt = 0.05;
	const double PerFrame = Piper.Chassis.Ground.Taxi.SpeedCap * Dt + 1.0;
	FVector2D Last = Traffic->FindAgent(Id)->LastMotion.Position;
	double MaxStep = 0.0;
	GLog->AddOutputDevice(&Spy);
	for (int32 Tick = 0; Tick < 60; ++Tick)
	{
		Traffic->Advance(Dt, Air.Net);
		const FRoadAgent* P = Traffic->FindAgent(Id);
		if (P == nullptr) { break; }
		MaxStep = FMath::Max(MaxStep, FVector2D::Distance(Last, P->LastMotion.Position));
		Last = P->LastMotion.Position;
	}
	GLog->RemoveOutputDevice(&Spy);

	const FRoadAgent* After = Traffic->FindAgent(Id);
	if (!TestNotNull(TEXT("the aircraft is still there"), After)) { return false; }
	AddInfo(FString::Printf(TEXT("after the re-offer: %s, worst frame %.0f uu; rescue line: %s"),
		*UEnum::GetValueAsString(After->Phase), MaxStep, *Spy.Rescued));
	TestEqual(TEXT("rescued: taxiing again"), After->Phase, EAgentPhase::Taxiing);
	TestEqual(TEXT("to the stand that freed"), After->GoalNode, Air.Pose(Spare));
	TestFalse(TEXT("and no longer waiting"), After->bAwaitingStand);
	TestTrue(*FString::Printf(TEXT("no frame moved it further than a rescue's hop (worst %.0f uu, budget %.0f)"), MaxStep,
		UGroundTraffic::RescueRejoinRadius + PerFrame), MaxStep <= UGroundTraffic::RescueRejoinRadius + PerFrame);
	TestTrue(TEXT("announced as the re-offer's, not the player's"), Causes.Num() == 1 && Causes[0] == EAgentEvent::ReOffered);
	TestTrue(TEXT("and driven by the taxi-in's policy - the rescue names its errand"), Spy.Rescued.Contains(TEXT("ArrivalTaxiIn")));
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A WAITER STRANDED AT ITS EXIT IS SENT FROM THERE WHEN A STAND FREES (#396, #429 review round 2).
 *
 * An arrival whose taxi-in dies whole while it is still on the runway is stranded BEFORE the handover, and its goal
 * becomes the exit node it hands over at (ReResolvePlan's Strand: "a stranded taxi-in still has a place"). It stands
 * there - the one stranded waiter that measurably IS at its goal node - so a restart from rest at that node is no jump,
 * and it is what the re-offer always did for it. The rescue that round 1 sent every stranded waiter to hops only onto
 * pavement running within 60 degrees of its heading - along the runway, where a taxi-in may not go - and could refuse.
 *
 * THREE REBUILDS, all while it rolls out: its stand goes (the spare held), so it waits; the first edge of its taxi-in
 * goes, so nothing of the taxi-in survives and it is stranded with its exit as its goal. After the handover the edge
 * comes back with the graph and the spare is given back. ASSERTED: it is sent to the spare, from where it stands - and
 * by a RESTART at its exit, not a rescue's hop: on this fixture a rescue happens to find the exit's turn path within its
 * 60 degrees, so arriving at the stand alone would not show which verb the re-offer chose (measured 2026-09-30: with
 * the at-goal-node branch removed this stayed green until the verb was asserted).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReofferWaiterStrandedAtItsExitTest,
	"Airside.Model.Traffic.ReofferWaiterStrandedAtItsExit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReofferWaiterStrandedAtItsExitTest::RunTest(const FString& Parameters)
{
	const FAirframe Piper = TestAirframes::Piper();
	FTestAirport Air = FTestAirport::Build(Piper, { .StandCount = 2 });
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = Traffic->DispatchArrival(*Air.Net, Air.Threshold, Piper, 1.0);
	if (!TestTrue(TEXT("the arrival is dispatched"), Id > 0)) { return false; }
	Traffic->Advance(0.05, Air.Net);
	const FGuidelineNodeId Goal0 = Traffic->FindAgent(Id)->GoalNode;
	const FEntityInstanceId Target = (Goal0 == Air.Pose(Air.Stands[0])) ? Air.Stands[0] : Air.Stands[1];
	const FEntityInstanceId Spare = (Target == Air.Stands[0]) ? Air.Stands[1] : Air.Stands[0];
	constexpr int32 Holder = 4242;
	if (!TestTrue(TEXT("the spare stand is held"), Traffic->HoldStand(Holder, Air.Pose(Spare)))) { return false; }

	// ONE: its stand goes; it will wait.
	Air.Net->RemoveEntity(Target);
	TestGraph::Rebuild(*Air.Net);
	Traffic->OnGraphRebuilt(*Air.Net);
	const FRoadAgent* Rolling = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("still on the runway, now waiting"), Rolling != nullptr && Rolling->Phase == EAgentPhase::Arriving
		&& Rolling->bAwaitingStand && Rolling->TaxiInPlan.Steps.Num() > 0)) { return false; }

	// TWO: the taxi-in's first edge goes - nothing of it survives.
	const FGuidelineEdgeId First = Rolling->TaxiInPlan.Steps[0].Edge;
	const FGuidelineNodeId Exit = Rolling->TaxiInPlan.Start;
	if (!TestTrue(TEXT("its taxi-in's first edge is deleted"), Air.Net->RemoveGuidelineEdge(First))) { return false; }
	Traffic->OnGraphRebuilt(*Air.Net);
	if (!TestEqual(TEXT("precondition: stranded by the rebuild before the handover"), Traffic->GetLastRebuildSummaryForTest().Stranded, 1)) { return false; }
	if (!TestEqual(TEXT("precondition: its goal is now its exit"), Traffic->FindAgent(Id)->GoalNode, Exit)) { return false; }

	// It lands and hands over: stranded, at its exit.
	if (!TestTrue(TEXT("it lands and is stranded at the handover"), RunUntil(*Traffic, *Air.Net, 600.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P && P->Phase == EAgentPhase::Stranded; })))
	{
		return false;
	}

	// THREE: the graph re-derived (the edge is back) and the spare given back - a stand frees.
	TestGraph::Rebuild(*Air.Net);
	Traffic->ReleaseHold(Holder);
	Traffic->OnGraphRebuilt(*Air.Net);
	const FRoadAgent* Waiting = Traffic->FindAgent(Id);
	const FGuidelineNode* GoalNode = Waiting != nullptr ? Air.Net->GetGuidelineNode(Waiting->GoalNode) : nullptr;
	if (!TestTrue(TEXT("precondition: still stranded and waiting, its goal a live node"),
		Waiting != nullptr && Waiting->Phase == EAgentPhase::Stranded && Waiting->bAwaitingStand && GoalNode != nullptr)) { return false; }
	AddInfo(FString::Printf(TEXT("stranded %.1f uu from its goal node, heading %.0f deg"),
		FVector2D::Distance(Waiting->LastMotion.Position, GoalNode->Position), FMath::RadiansToDegrees(Waiting->LastMotion.Heading)));

	// WHICH VERB, heard: RedirectAgent says "redirected:", RescueStranded "rescued:".
	struct FVerbSpy : public FOutputDevice
	{
		int32 Redirected = 0;
		int32 Rescued = 0;
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category != FName(TEXT("LogAirsideTraffic"))) { return; }
			const FString Line(V);
			Redirected += Line.Contains(TEXT(" redirected: ")) ? 1 : 0;
			Rescued += Line.Contains(TEXT(" rescued: ")) ? 1 : 0;
		}
	} Spy;

	const double Dt = 0.05;
	const double PerFrame = Piper.Chassis.Ground.Taxi.SpeedCap * Dt + 1.0;
	FVector2D Last = Waiting->LastMotion.Position;
	double MaxStep = 0.0;
	GLog->AddOutputDevice(&Spy);
	for (int32 Tick = 0; Tick < 60; ++Tick)
	{
		Traffic->Advance(Dt, Air.Net);
		const FRoadAgent* P = Traffic->FindAgent(Id);
		if (P == nullptr) { break; }
		MaxStep = FMath::Max(MaxStep, FVector2D::Distance(Last, P->LastMotion.Position));
		Last = P->LastMotion.Position;
	}
	GLog->RemoveOutputDevice(&Spy);
	const FRoadAgent* After = Traffic->FindAgent(Id);
	if (!TestNotNull(TEXT("the aircraft is still there"), After)) { return false; }
	AddInfo(FString::Printf(TEXT("after the re-offer: %s, worst frame %.0f uu, %d redirect(s), %d rescue(s)"),
		*UEnum::GetValueAsString(After->Phase), MaxStep, Spy.Redirected, Spy.Rescued));
	TestTrue(TEXT("RESTARTED at its exit, where it stands - the goal node it is measured at - not hopped by a rescue"),
		Spy.Redirected == 1 && Spy.Rescued == 0);
	TestEqual(TEXT("sent: taxiing"), After->Phase, EAgentPhase::Taxiing);
	TestEqual(TEXT("to the stand that freed"), After->GoalNode, Air.Pose(Spare));
	TestFalse(TEXT("and no longer waiting"), After->bAwaitingStand);
	TestTrue(*FString::Printf(TEXT("from where it stood - no frame moved it further than taxi speed allows (worst %.0f uu, budget %.0f)"),
		MaxStep, PerFrame), MaxStep <= PerFrame);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A REBUILD MID-REVERSE THAT KILLS THE ROUTE AFTER THE SPAN LEAVES THE REVERSE ALONE, AND THE TRUCK PARKS AT
 * THE SPAN'S END (issue #434, review of #453).
 *
 * The exit is removed with nothing laid in its place: the first step at or after ResumeStep does not
 * re-resolve, no route reaches the goal, and the remainder is TRUNCATED to nothing (the span's end
 * is the longest live prefix). Three things are pinned:
 *
 *  - the reverse is not ended by the rebuild. ReResolvePlan's driving branches - rejoin the pavement
 *    under the agent, restart on a drive-side flip - end in RejoinTaxi / RestartTaxi, which put the
 *    agent back in Taxiing, and stranding in place marks the whole route dead. bDriving is false for
 *    a reversing agent so none of them is taken; without it this fails on the phase or on the route.
 *  - the summary says one route was truncated, not stranded or replanned.
 *  - after backing out, with nothing left to drive, the truck parks at the span's end on a live goal.
 *
 * A rebuild that also frees the span's END node takes the Strand path instead, and is pinned by
 * RebuildDuringReverseStrandsWhenTheSpanEndIsGone (#455): the truck stops where it stands and is
 * Stranded, rather than backing out and driving the remainder on the handles the rebuild freed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRebuildDuringReverseTruncatesTheRemainderTest,
	"Airside.Model.Traffic.RebuildDuringReverseTruncatesTheRemainder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRebuildDuringReverseTruncatesTheRemainderTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	if (!TestTrue(TEXT("the bay routes as arrive, reverse, depart"), Bay.RouteIsTheThreeLegs())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Truck = Traffic->DispatchAgent(Bay.Net, Bay.Route, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the truck is dispatched"), Truck > 0)) { return false; }
	if (!TestTrue(TEXT("and backs off the service point"), TickUntilReversing(*Traffic, *Bay.Net, Truck))) { return false; }
	for (int32 Tick = 0; Tick < 200; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	if (!TestEqual(TEXT("still reversing when the edit lands"), Traffic->FindAgent(Truck)->Phase, EAgentPhase::Reversing)) { return false; }

	// THE EXIT GOES, and nothing replaces it: no route reaches where the truck was heading.
	Bay.Net->RemoveGuidelineNode(Bay.Exit);
	Traffic->OnGraphRebuilt(*Bay.Net);

	const FRoadAgent* Agent = Traffic->FindAgent(Truck);
	if (!TestNotNull(TEXT("the truck survives the rebuild"), Agent)) { return false; }
	TestEqual(TEXT("still reversing: no rejoin, restart or strand ended the back-out"), Agent->Phase, EAgentPhase::Reversing);
	TestTrue(TEXT("and its route is still a route: it was truncated, not marked dead"), Agent->Follower.Plan.IsValid());
	TestEqual(TEXT("one route truncated by the rebuild"), Traffic->GetLastRebuildSummaryForTest().Truncated, 1);
	TestEqual(TEXT("and none stranded"), Traffic->GetLastRebuildSummaryForTest().Stranded, 0);

	if (!TestTrue(TEXT("it finishes backing out"), RunUntil(*Traffic, *Bay.Net, 60.0, [&]()
		{
			const FRoadAgent* T = Traffic->FindAgent(Truck);
			return T != nullptr && T->Phase != EAgentPhase::Reversing;
		}))) { return false; }
	Agent = Traffic->FindAgent(Truck);
	if (!TestNotNull(TEXT("the truck is still there"), Agent)) { return false; }
	TestEqual(TEXT("and with nothing left to drive it parks"), Agent->Phase, EAgentPhase::Parked);
	TestTrue(TEXT("on a live goal: the end of the span"), Agent->GoalNode == Bay.Cleared && Bay.Net->GetGuidelineNode(Agent->GoalNode) != nullptr);
	const FGuidelineNode* SpanEnd = Bay.Net->GetGuidelineNode(Bay.Cleared);
	const double FromEnd = SpanEnd != nullptr ? FVector2D::Distance(Agent->LastMotion.Position, SpanEnd->Position) : 1.0e9;
	TestTrue(*FString::Printf(TEXT("at the span's end, not where it began (%.0f uu from it; the span is 2500)"), FromEnd), FromEnd < 700.0);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A REBUILD DURING A REVERSE THAT ENDS THE ROUTE RE-POINTS THE GOAL (issue #434, review of #453).
 *
 * With nothing after the span there is no remainder for the rebuild to re-resolve, so the Reversing
 * arm does not take the agent; its goal is the span's end node, a derived one the rebuild frees,
 * and it is where the vehicle will park. That goal is re-pointed by position instead. A rig backing
 * into a bay is this route. The far node is redrawn on the same spot mid-reverse: the goal must be the
 * NEW node, and the truck must still park on it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRebuildDuringAReverseThatEndsTheRouteTest,
	"Airside.Model.Traffic.RebuildDuringAReverseThatEndsTheRoute",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRebuildDuringAReverseThatEndsTheRouteTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	const FRoutePlan Route = TestGraph::Probe(*Bay.Net, Bay.Approach, Bay.Cleared, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the route is the serve leg then a reverse that is its last step"),
		Route.IsValid() && Route.Steps.Num() == 2 && !Route.Steps[0].bReverseLeg && Route.Steps[1].bReverseLeg)) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Truck = Traffic->DispatchAgent(Bay.Net, Route, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the truck is dispatched"), Truck > 0)) { return false; }
	if (!TestTrue(TEXT("and backs off the service point"), TickUntilReversing(*Traffic, *Bay.Net, Truck))) { return false; }
	for (int32 Tick = 0; Tick < 200; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	if (!TestEqual(TEXT("still reversing when the edit lands"), Traffic->FindAgent(Truck)->Phase, EAgentPhase::Reversing)) { return false; }
	if (!TestTrue(TEXT("its goal is the far node, as dispatched"), Traffic->FindAgent(Truck)->GoalNode == Bay.Cleared)) { return false; }

	Bay.Net->RemoveGuidelineNode(Bay.Cleared);
	const FGuidelineNodeId Cleared = TestGraph::Node(*Bay.Net, 0.0, 4500.0);
	const FGuidelineEdgeId ReverseEdge = Bay.Lay(Bay.Service, Cleared, true);
	Traffic->OnGraphRebuilt(*Bay.Net);

	const FRoadAgent* Agent = Traffic->FindAgent(Truck);
	if (!TestNotNull(TEXT("the truck survives the rebuild"), Agent)) { return false; }
	TestEqual(TEXT("still reversing"), Agent->Phase, EAgentPhase::Reversing);
	TestTrue(TEXT("its goal is live, and is the redrawn node"), Agent->GoalNode == Cleared && Bay.Net->GetGuidelineNode(Agent->GoalNode) != nullptr);

	// THE SPAN IS THE WHOLE ROUTE HERE, AND NOTHING BUT ReResolveSpan TOUCHES IT (#455, review of #466): with no
	// remainder there is no arm to re-resolve one, so without the span pass every step of the route the truck holds
	// keeps its freed handles for the rest of the leg. Asked while it is STILL reversing, before any handover.
	int32 Dead = 0;
	for (const FRouteStep& Step : Agent->Follower.Plan.Steps)
	{
		Dead += (Bay.Net->GetGuidelineEdge(Step.Edge) == nullptr || Bay.Net->GetGuidelineNode(Step.To) == nullptr) ? 1 : 0;
	}
	TestEqual(TEXT("every step of the route names a live edge and node while it is still reversing (red without ReResolveSpan)"), Dead, 0);
	TestTrue(TEXT("the span is the redrawn reverse edge, ending at the redrawn node"),
		Agent->Follower.Plan.Steps.Last().Edge == ReverseEdge && Agent->Follower.Plan.Steps.Last().To == Cleared);

	if (!TestTrue(TEXT("it finishes backing out"), RunUntil(*Traffic, *Bay.Net, 60.0, [&]()
		{
			const FRoadAgent* T = Traffic->FindAgent(Truck);
			return T != nullptr && T->Phase != EAgentPhase::Reversing;
		}))) { return false; }
	Agent = Traffic->FindAgent(Truck);
	if (!TestNotNull(TEXT("the truck is still there"), Agent)) { return false; }
	TestEqual(TEXT("and parks"), Agent->Phase, EAgentPhase::Parked);
	TestTrue(TEXT("on the live goal"), Agent->GoalNode == Cleared);
	return true;
}

// ---------------------------------------------------------------------------------------
// THE REVERSE-LEG FOLLOW-UPS #453 LEFT (issue #455): stall accounting, the ground-free gate before a reverse arms,
// the span's handles across a mid-reverse edit, the strand case, and the waiter whose extension was refused.
namespace
{
	/**
	 * A CLAIM BY AN AGENT THAT EXISTS ONLY IN THE TABLE, so nothing ever releases it: STANDING on Node (occupied), so
	 * no rank can take it away - the phantom the BoxEntry tests hold a node with. What a test needs when it wants
	 * ground another vehicle holds for as long as it likes, without staging a second vehicle to sit there.
	 */
	void HoldNodeAsPhantom(UGroundTraffic& Traffic, int32 PhantomId, FGuidelineNodeId Node)
	{
		FTrafficClaim Sitting;
		Sitting.AgentId = PhantomId;
		Sitting.Resource = FTrafficResource::OfNode(Node);
		Sitting.bOccupied = true;
		FTrafficClaim Blocker;
		Traffic.OccupancyForTest().TryClaim(Sitting, Blocker);
	}

	/** Ticks until the agent has stopped at SpanStart (the serve leg's end), or is no longer Taxiing - armed anyway. */
	bool TickUntilAtSpanStart(UGroundTraffic& Traffic, const URoadNetwork& Net, int32 AgentId, double SpanStart)
	{
		return RunUntil(Traffic, Net, 90.0, [&]()
		{
			const FRoadAgent* Agent = Traffic.FindAgent(AgentId);
			return Agent == nullptr || Agent->Phase != EAgentPhase::Taxiing
				|| (Agent->Follower.Travelled >= SpanStart - 1.0 && Agent->Follower.Speed < 1.0);
		});
	}
}

// ---------------------------------------------------------------------------------------
/**
 * A REFUSED REVERSING TRUCK ACCRUES THE STALL CLOCK, AND ONE THAT IS MOVING DOES NOT (issue #455, item 1).
 *
 * The clock was fed for a Taxiing agent alone: `Phase == Taxiing && WaitingOn && Follower.Speed == 0`. A truck
 * backing along a bay's leg was refused since #453 made the claim pass hold its whole span, but it never stalled -
 * the clock could not run - so a cycle through it was unrepresentable however long it stood. And Follower.Speed is
 * ZERO for the whole leg (the follower is parked while FReverseRun plays the span), so the obvious widening, "any
 * on-route phase", would have counted a truck backing at full speed as stopped. The clock reads SpeedAlongPlan.
 *
 * A phantom stands on the span's far node a moment after the truck arms: it is refused by it (WaitingOn) while still
 * backing toward the stop - and the clock must NOT run then. Once it has stopped short, it must.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReversingTruckStallsWhenItsSpanIsRefusedTest,
	"Airside.Model.Traffic.ReversingTruckStallsWhenItsSpanIsRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReversingTruckStallsWhenItsSpanIsRefusedTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	if (!TestTrue(TEXT("the bay routes as arrive, reverse, depart"), Bay.RouteIsTheThreeLegs())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Truck = Traffic->DispatchAgent(Bay.Net, Bay.Route, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the truck is dispatched"), Truck > 0)) { return false; }
	if (!TestTrue(TEXT("and backs off the service point"), TickUntilReversing(*Traffic, *Bay.Net, Truck))) { return false; }
	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}

	constexpr int32 Phantom = 900;
	HoldNodeAsPhantom(*Traffic, Phantom, Bay.Cleared);

	// REFUSED AND STILL MOVING: two seconds on, it has been told it cannot have the far node and is backing toward
	// the stop it was given. A stalled waiter is one that has STOPPED; this one has not.
	for (int32 Tick = 0; Tick < 40; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	const FRoadAgent* T = Traffic->FindAgent(Truck);
	if (!TestNotNull(TEXT("the truck is still there"), T)) { return false; }
	TestEqual(TEXT("still reversing"), T->Phase, EAgentPhase::Reversing);
	TestEqual(TEXT("refused the far node by the phantom"), T->GetWaitingOn(), Phantom);
	TestTrue(TEXT("and still backing toward its stop (speed along the plan is not zero)"), T->SpeedAlongPlan() > 1.0);
	TestEqual(TEXT("so the stall clock has not run: a moving agent is not stalled, whatever it waits on"), T->GetStalledSeconds(), 0.0);

	// STOPPED SHORT: the reverse plays at 100 uu/s toward a stop well inside its 2500 uu span.
	const bool bStalled = RunUntil(*Traffic, *Bay.Net, 60.0, [&]()
	{
		const FRoadAgent* P = Traffic->FindAgent(Truck);
		return P == nullptr || P->GetStalledSeconds() > Traffic->Rules.StallSeconds;
	});
	TestTrue(TEXT("once it has stopped short the stall clock runs past StallSeconds (red while only a Taxiing agent accrued)"), bStalled);
	T = Traffic->FindAgent(Truck);
	if (!TestNotNull(TEXT("the truck is still there after the wait"), T)) { return false; }
	TestEqual(TEXT("still reversing"), T->Phase, EAgentPhase::Reversing);
	TestTrue(TEXT("and stopped"), T->SpeedAlongPlan() < 1.0e-3);
	TestEqual(TEXT("still refused by the phantom"), T->GetWaitingOn(), Phantom);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * WHICH PHASES ARE STALLED WAITERS, NAMED FOR EVERY PHASE (issue #455, item 1).
 *
 * FRoadAgent::IsStoppedAndWaiting is what AdvanceOnce accrues the stall clock on: on a route, refused by somebody, and
 * not moving ALONG THE PLAN. A taxi, a push and a reverse qualify when stopped; none of them does while moving,
 * because each is measured by the struct that is driving it (Follower, Pushback, Reverse) - reading the follower for
 * a reverse says "stopped" for the whole leg. Every other phase is off a route and cannot be one. Exhaustive over the
 * enum, so a phase added is decided here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficStalledWaiterClassifiesEveryPhaseTest,
	"Airside.Model.Traffic.StalledWaiterClassifiesEveryPhase",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficStalledWaiterClassifiesEveryPhaseTest::RunTest(const FString& Parameters)
{
	FGuidelineNodeId Node;
	Node.Index = 7;

	const UEnum* Enum = StaticEnum<EAgentPhase>();
	if (!TestNotNull(TEXT("EAgentPhase reflects"), Enum)) { return false; }
	for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
	{
		const FString Name = Enum->GetNameStringByIndex(Index);
		if (Name.EndsWith(TEXT("_MAX"))) { continue; }
		const EAgentPhase Phase = static_cast<EAgentPhase>(Enum->GetValueByIndex(Index));

		FRoadAgent Agent;
		Agent.Phase = Phase;
		Agent.Refuse(INDEX_NONE, FTrafficResource::OfNode(Node), TNumericLimits<double>::Max(), 5);
		const bool bOnRoute = Phase == EAgentPhase::Taxiing || Phase == EAgentPhase::Manoeuvring || Phase == EAgentPhase::Reversing;
		TestEqual(*FString::Printf(TEXT("a %s that is stopped and refused is a stalled waiter exactly when it is on a route"), *Name),
			Agent.IsStoppedAndWaiting(), bOnRoute);

		// NOT WAITING: nothing refuses it.
		Agent.ClearArbitration();
		TestFalse(*FString::Printf(TEXT("a %s that nobody refuses is not one"), *Name), Agent.IsStoppedAndWaiting());
	}

	// MOVING, each by its own driver: the speed that says so is the driving struct's, not the follower's.
	struct FMoving
	{
		EAgentPhase Phase;
		const TCHAR* Driver;
	};
	for (const FMoving& Case : { FMoving{ EAgentPhase::Taxiing, TEXT("the follower") },
		FMoving{ EAgentPhase::Manoeuvring, TEXT("the push") }, FMoving{ EAgentPhase::Reversing, TEXT("the reverse run") } })
	{
		FRoadAgent Agent;
		Agent.Phase = Case.Phase;
		Agent.Refuse(INDEX_NONE, FTrafficResource::OfNode(Node), TNumericLimits<double>::Max(), 5);
		switch (Case.Phase)
		{
		case EAgentPhase::Taxiing:     Agent.Follower.Speed = 50.0; break;
		case EAgentPhase::Manoeuvring: Agent.Pushback.Speed = 50.0; break;
		default:                       Agent.Reverse.Speed = 50.0; break;
		}
		TestFalse(*FString::Printf(TEXT("refused but still moving under %s is not stalled"), Case.Driver), Agent.IsStoppedAndWaiting());
	}
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A REVERSE WAITS FOR HELD GROUND; IT DOES NOT START INTO IT AND SIT MID-SPAN (issue #455, item 2).
 *
 * DepartAgent grants a push whole or withholds it (IsPushGroundFree) because a manoeuvre with no second way out that
 * can be stopped half way blocks a taxiway with nothing able to act on it. A reverse leg is the same kind of
 * manoeuvre and had no such gate: TryArmReverseLeg armed whenever the steered axle reached the span's start, the claim
 * pass then held the whole span and refused the truck a few metres on, and it sat in the span across the lane it was
 * meant to clear.
 *
 * A phantom stands on the span's FAR node before the truck is even dispatched, where the truck's ordinary window (a
 * gap and a half footprint ahead) cannot see it - so only a gate over the WHOLE span can refuse it. The truck
 * drives its serve leg to the service point and must stop there: still Taxiing, waiting on the phantom (visible to
 * the inspector and the wait-for graph as any waiter is), not moved by a centimetre over ten seconds. Then the
 * phantom goes and it arms.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReverseWaitsForHeldGroundTest,
	"Airside.Model.Traffic.ReverseWaitsForHeldGround",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReverseWaitsForHeldGroundTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	if (!TestTrue(TEXT("the bay routes as arrive, reverse, depart"), Bay.RouteIsTheThreeLegs())) { return false; }
	const double SpanStart = Bay.Route.Steps[0].EndDistance;

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	constexpr int32 Phantom = 900;
	HoldNodeAsPhantom(*Traffic, Phantom, Bay.Cleared);
	const int32 Truck = Traffic->DispatchAgent(Bay.Net, Bay.Route, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the truck is dispatched"), Truck > 0)) { return false; }
	TestTrue(TEXT("it drives the serve leg to the service point"), TickUntilAtSpanStart(*Traffic, *Bay.Net, Truck, SpanStart));

	const FRoadAgent* T = Traffic->FindAgent(Truck);
	if (!TestNotNull(TEXT("the truck is still there"), T)) { return false; }
	TestEqual(TEXT("it has NOT armed the reverse: still Taxiing at the span's start (red while nothing gated the arm)"),
		T->Phase, EAgentPhase::Taxiing);
	for (int32 Tick = 0; Tick < 20; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	T = Traffic->FindAgent(Truck);
	TestEqual(TEXT("and waits on whoever holds the span, which is what the inspector and the wait-for graph read"),
		T->GetWaitingOn(), Phantom);
	TestEqual(TEXT("refused at the reverse step, the second of the route's three"), T->GetBlockedStep(), 1);
	TestTrue(TEXT("on the span's far node - the one thing in the span the ordinary window could not see"),
		T->GetBlockedResource().Kind == ETrafficResourceKind::Node && T->GetBlockedResource().Node == Bay.Cleared);

	// IT WAITS, IT DOES NOT CREEP.
	const FVector2D Held = T->LastMotion.Position;
	for (int32 Tick = 0; Tick < 200; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	T = Traffic->FindAgent(Truck);
	TestEqual(TEXT("ten seconds on it is still Taxiing"), T->Phase, EAgentPhase::Taxiing);
	TestTrue(*FString::Printf(TEXT("and has not moved (%.2f uu)"), FVector2D::Distance(Held, T->LastMotion.Position)),
		FVector2D::Distance(Held, T->LastMotion.Position) < 1.0);
	TestEqual(TEXT("no reverse was played"), T->Reverse.Travelled, 0.0);
	TestTrue(TEXT("and a wait that lasts is a stalled waiter, on the clock like any other"), T->GetStalledSeconds() > Traffic->Rules.StallSeconds);

	// LET GO: the phantom leaves and the very next frames arm it.
	Traffic->OccupancyForTest().ReleaseAll(Phantom);
	TestTrue(TEXT("it arms the reverse once the span is free"), RunUntil(*Traffic, *Bay.Net, 5.0, [&]()
	{
		const FRoadAgent* P = Traffic->FindAgent(Truck);
		return P != nullptr && P->Phase == EAgentPhase::Reversing;
	}));
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A ROUTE THAT OPENS WITH A REVERSE, DISPATCHED INTO HELD GROUND, WAITS (issue #455, item 2 - the dispatch's own site).
 *
 * The gate before a reverse arms has three call sites, because a reverse arms in three places: the tick, a dispatch's
 * zero-second pose and a redirect's. This is the dispatch: AdmitDispatched runs Advance(0) before the agent is in the
 * table, before any claim pass, so without the gate a route opening with its reverse leg armed it into held ground
 * at once. The CONTROL is the same dispatch with the ground free, which arms in that same pose - the premise that
 * makes "it did not" mean something.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReverseFromDispatchWaitsForHeldGroundTest,
	"Airside.Model.Traffic.ReverseFromDispatchWaitsForHeldGround",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReverseFromDispatchWaitsForHeldGroundTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	const FRoutePlan Home = TestGraph::Probe(*Bay.Net, Bay.Service, Bay.Exit, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the route from the service point opens with the reverse leg and then drives out"),
		Home.IsValid() && Home.Steps.Num() == 2 && Home.Steps[0].bReverseLeg && !Home.Steps[1].bReverseLeg)) { return false; }

	UGroundTraffic* Free = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Control = Free->DispatchAgent(Bay.Net, Home, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("control: the dispatch is admitted"), Control > 0)) { return false; }
	if (!TestEqual(TEXT("control: into FREE ground it arms the reverse in the dispatch's own pose"),
		Free->FindAgent(Control)->Phase, EAgentPhase::Reversing)) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	constexpr int32 Phantom = 900;
	HoldNodeAsPhantom(*Traffic, Phantom, Bay.Cleared);
	const int32 Truck = Traffic->DispatchAgent(Bay.Net, Home, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the same dispatch is admitted with the span's far node held"), Truck > 0)) { return false; }
	const FRoadAgent* T = Traffic->FindAgent(Truck);
	TestEqual(TEXT("but it did NOT arm: Taxiing at the service point (red while the dispatch's own Advance armed unasked)"),
		T->Phase, EAgentPhase::Taxiing);
	TestEqual(TEXT("waiting on the phantom that holds the span"), T->GetWaitingOn(), Phantom);
	for (int32 Tick = 0; Tick < 20; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	TestEqual(TEXT("and it keeps waiting through the ticks that follow"), Traffic->FindAgent(Truck)->Phase, EAgentPhase::Taxiing);

	Traffic->OccupancyForTest().ReleaseAll(Phantom);
	TestTrue(TEXT("it arms the moment the span is free"), RunUntil(*Traffic, *Bay.Net, 5.0, [&]()
	{
		const FRoadAgent* P = Traffic->FindAgent(Truck);
		return P != nullptr && P->Phase == EAgentPhase::Reversing;
	}));
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A REDIRECT INTO A REVERSE, INTO HELD GROUND, WAITS (issue #455, item 2 - the redirect's own site).
 *
 * The stand service cycle's route home OPENS with its reverse leg, and a parked truck is sent along it by
 * RedirectAgent, whose zero-second Advance is where the back-out arms - with no claim pass run and so no notion that
 * anything else already held the leg. This is THE production path of the gate. Same shape as the dispatch's test:
 * a control that arms into free ground, then held ground that does not, then released.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReverseFromRedirectWaitsForHeldGroundTest,
	"Airside.Model.Traffic.ReverseFromRedirectWaitsForHeldGround",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReverseFromRedirectWaitsForHeldGroundTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	const FRoutePlan ToService = TestGraph::Probe(*Bay.Net, Bay.Approach, Bay.Service, ETraversalClass::GroundVehicle);
	const FRoutePlan Home = TestGraph::Probe(*Bay.Net, Bay.Service, Bay.Exit, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the routes to the service point, and home from it (reverse first), exist"),
		ToService.IsValid() && Home.IsValid() && Home.Steps.Num() == 2 && Home.Steps[0].bReverseLeg)) { return false; }

	// A TRUCK PARKED AT THE SERVICE POINT, in each of two models: one where the ground is free and one where it is not.
	auto ParkAtService = [&](UGroundTraffic& Traffic, int32& OutTruck) -> bool
	{
		OutTruck = Traffic.DispatchAgent(Bay.Net, ToService, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
		return OutTruck > 0 && RunUntil(Traffic, *Bay.Net, 90.0, [&]()
		{
			const FRoadAgent* P = Traffic.FindAgent(OutTruck);
			return P != nullptr && P->Phase == EAgentPhase::Parked;
		});
	};

	UGroundTraffic* Free = NewObject<UGroundTraffic>(GetTransientPackage());
	int32 Control = 0;
	if (!TestTrue(TEXT("control: the truck parks at the service point"), ParkAtService(*Free, Control))) { return false; }
	if (!TestTrue(TEXT("control: it is sent home"), Free->RedirectAgent(Control, Bay.Net, Home))) { return false; }
	if (!TestEqual(TEXT("control: into FREE ground the redirect arms the reverse in its own pose"),
		Free->FindAgent(Control)->Phase, EAgentPhase::Reversing)) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	int32 Truck = 0;
	if (!TestTrue(TEXT("the truck parks at the service point"), ParkAtService(*Traffic, Truck))) { return false; }
	constexpr int32 Phantom = 900;
	HoldNodeAsPhantom(*Traffic, Phantom, Bay.Cleared);
	if (!TestTrue(TEXT("it is sent home with the span's far node held"), Traffic->RedirectAgent(Truck, Bay.Net, Home))) { return false; }
	const FRoadAgent* T = Traffic->FindAgent(Truck);
	TestEqual(TEXT("but it did NOT arm: Taxiing at the service point (red while the redirect's own Advance armed unasked)"),
		T->Phase, EAgentPhase::Taxiing);
	TestEqual(TEXT("waiting on the phantom that holds the span"), T->GetWaitingOn(), Phantom);
	for (int32 Tick = 0; Tick < 20; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	TestEqual(TEXT("and it keeps waiting through the ticks that follow"), Traffic->FindAgent(Truck)->Phase, EAgentPhase::Taxiing);

	Traffic->OccupancyForTest().ReleaseAll(Phantom);
	TestTrue(TEXT("it arms the moment the span is free"), RunUntil(*Traffic, *Bay.Net, 5.0, [&]()
	{
		const FRoadAgent* P = Traffic->FindAgent(Truck);
		return P != nullptr && P->Phase == EAgentPhase::Reversing;
	}));
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * AN EDIT MID-REVERSE LEAVES EVERY STEP OF THE SPAN LIVE, AND THE TRUCK'S HOLD ON IT (issue #455, item 3).
 *
 * #453's Reversing arm re-resolved the route from ResumeStep - the remainder - and deliberately not the span being
 * backed along, because a re-resolve that could replan or truncate would move the route from under the step index
 * the handover cuts the remainder by. So after an edit that freed the span's handles, the claim pass (which holds a
 * reversing vehicle's whole span through those very steps) held nothing for the rest of the leg. ReResolveSpan
 * re-points them: handles only, no replan, no step moved.
 *
 * The far node and the exit are redrawn on the same spots mid-reverse, as RebuildDuringReverse does, and this asks the
 * question that test could not - at the moment of the edit, while the truck is STILL REVERSING: every step of the
 * route names a live edge and node, the span is the redrawn edge, the step count and every EndDistance are what they
 * were, and after a few claim passes the truck holds the LIVE reverse edge and far node, where another agent would
 * meet it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRebuildDuringReverseKeepsTheSpanLiveTest,
	"Airside.Model.Traffic.RebuildDuringReverseKeepsTheSpanLive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRebuildDuringReverseKeepsTheSpanLiveTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	if (!TestTrue(TEXT("the bay routes as arrive, reverse, depart"), Bay.RouteIsTheThreeLegs())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Truck = Traffic->DispatchAgent(Bay.Net, Bay.Route, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the truck is dispatched"), Truck > 0)) { return false; }
	if (!TestTrue(TEXT("and backs off the service point"), TickUntilReversing(*Traffic, *Bay.Net, Truck))) { return false; }
	for (int32 Tick = 0; Tick < 200; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	if (!TestEqual(TEXT("still reversing when the edit lands"), Traffic->FindAgent(Truck)->Phase, EAgentPhase::Reversing)) { return false; }
	const TArray<FRouteStep> Before = Traffic->FindAgent(Truck)->Follower.Plan.Steps;

	Bay.Net->RemoveGuidelineNode(Bay.Cleared);
	Bay.Net->RemoveGuidelineNode(Bay.Exit);
	const FGuidelineNodeId Cleared = TestGraph::Node(*Bay.Net, 0.0, 4500.0);
	const FGuidelineNodeId Exit = TestGraph::Node(*Bay.Net, FBackingBay::ExitX, FBackingBay::ExitY);
	const FGuidelineEdgeId ReverseEdge = Bay.Lay(Bay.Service, Cleared, true);
	const FGuidelineEdgeId ExitEdge = Bay.Lay(Cleared, Exit, false);
	Traffic->OnGraphRebuilt(*Bay.Net);

	const FRoadAgent* Agent = Traffic->FindAgent(Truck);
	if (!TestNotNull(TEXT("the truck survives the rebuild"), Agent)) { return false; }
	TestEqual(TEXT("still reversing: this is the state BEFORE the handover the older test waits for"), Agent->Phase, EAgentPhase::Reversing);
	const FRoutePlan& Plan = Agent->Follower.Plan;
	if (!TestEqual(TEXT("the route has the steps it had"), Plan.Steps.Num(), Before.Num())) { return false; }

	int32 Dead = 0;
	int32 Moved = 0;
	for (int32 Step = 0; Step < Plan.Steps.Num(); ++Step)
	{
		Dead += (Bay.Net->GetGuidelineEdge(Plan.Steps[Step].Edge) == nullptr || Bay.Net->GetGuidelineNode(Plan.Steps[Step].To) == nullptr) ? 1 : 0;
		Moved += FMath::IsNearlyEqual(Plan.Steps[Step].EndDistance, Before[Step].EndDistance, 1.0e-6) ? 0 : 1;
	}
	TestEqual(TEXT("EVERY step of the route names a live edge and node, the span's own included (red while the span kept its freed handles)"), Dead, 0);
	TestEqual(TEXT("and no step moved: the handover cuts the remainder by these indices and distances"), Moved, 0);
	TestTrue(TEXT("the span is the redrawn reverse edge"), Plan.Steps[1].Edge == ReverseEdge);
	TestTrue(TEXT("and ends at the redrawn far node"), Plan.Steps[1].To == Cleared);
	TestTrue(TEXT("where the remainder, on the redrawn exit edge, leaves from"), Plan.Steps[2].Edge == ExitEdge);

	// AND THE CLAIM IS ON THE LIVE GROUND: a few passes on, another agent asking for the reverse edge or the far
	// node is told the truck holds it.
	for (int32 Tick = 0; Tick < 5; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	TestEqual(TEXT("still reversing after the passes"), Traffic->FindAgent(Truck)->Phase, EAgentPhase::Reversing);
	int32 Holder = 0;
	TestTrue(TEXT("a second agent is refused the LIVE reverse edge"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfEdge(ReverseEdge), 999, &Holder));
	TestEqual(TEXT("held by the truck"), Holder, Truck);
	Holder = 0;
	TestTrue(TEXT("and the live far node"), Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Cleared), 999, &Holder));
	TestEqual(TEXT("held by the truck too"), Holder, Truck);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A REBUILD THAT DELETES THE SPAN'S END NODE MID-REVERSE STRANDS THE TRUCK WHERE IT STANDS (issue #455, item 4).
 *
 * When no live node holds the position the span's own end is at, the Reversing arm's ReResolvePlan strands: it marks
 * Follower.Plan unreachable, and the agent stayed Reversing - so it finished the span holding nothing, the handover cut
 * the remainder with RouteSearch::Section (which writes Result = Found over the dead marker) and the truck drove that
 * remainder on the handles the rebuild had freed. What it does now is what a Taxiing agent does when its plan dies
 * under it: stops where it stands and is Stranded, until it is retired or rescued.
 *
 * ASSERTED: the rebuild reports one route stranded; a frame later the phase is Stranded, the phase change was
 * broadcast (the ops layer's jobs are freed by it, not by polling); it has not moved more than a frame's worth of
 * back-out; and in the thirty seconds after it never drives (never Taxiing).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRebuildDuringReverseStrandsWhenTheSpanEndIsGoneTest,
	"Airside.Model.Traffic.RebuildDuringReverseStrandsWhenTheSpanEndIsGone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRebuildDuringReverseStrandsWhenTheSpanEndIsGoneTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	if (!TestTrue(TEXT("the bay routes as arrive, reverse, depart"), Bay.RouteIsTheThreeLegs())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Truck = Traffic->DispatchAgent(Bay.Net, Bay.Route, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the truck is dispatched"), Truck > 0)) { return false; }
	if (!TestTrue(TEXT("and backs off the service point"), TickUntilReversing(*Traffic, *Bay.Net, Truck))) { return false; }
	for (int32 Tick = 0; Tick < 200; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
	}
	if (!TestEqual(TEXT("still reversing when the edit lands"), Traffic->FindAgent(Truck)->Phase, EAgentPhase::Reversing)) { return false; }
	const FVector2D StoppedAt = Traffic->FindAgent(Truck)->LastMotion.Position;

	int32 HeardStranded = 0;
	const FDelegateHandle Listener = Traffic->OnAgentPhaseChanged.AddLambda([&](const FAgentTransition& T)
	{
		// AND NAMED (#436): the Stranded cause travels with it, so ops need not ask the agent why it stopped.
		HeardStranded += (T.AgentId == Truck && T.From == EAgentPhase::Reversing && T.To == EAgentPhase::Stranded
			&& T.Cause == EAgentEvent::Stranded) ? 1 : 0;
	});

	// THE SPAN'S END GOES, and nothing replaces it: no live node holds where the reverse leg ends.
	Bay.Net->RemoveGuidelineNode(Bay.Cleared);
	Traffic->OnGraphRebuilt(*Bay.Net);
	TestEqual(TEXT("the rebuild strands the route: no node holds where the span ends"), Traffic->GetLastRebuildSummaryForTest().Stranded, 1);

	bool bDrove = false;
	for (int32 Tick = 0; Tick < 600; ++Tick)
	{
		Traffic->Advance(0.05, Bay.Net);
		const FRoadAgent* P = Traffic->FindAgent(Truck);
		if (P == nullptr) { break; }
		bDrove |= P->Phase == EAgentPhase::Taxiing;
	}
	Traffic->OnAgentPhaseChanged.Remove(Listener);

	const FRoadAgent* After = Traffic->FindAgent(Truck);
	if (!TestNotNull(TEXT("the truck is still there"), After)) { return false; }
	TestEqual(TEXT("it is Stranded (red while it went on reversing and then drove the dead remainder)"), After->Phase, EAgentPhase::Stranded);
	TestFalse(TEXT("and it never drove on, across thirty seconds"), bDrove);
	TestEqual(TEXT("the phase change Reversing -> Stranded was broadcast once"), HeardStranded, 1);
	const double Wandered = FVector2D::Distance(StoppedAt, After->LastMotion.Position);
	TestTrue(*FString::Printf(TEXT("and it stopped where it stood, not at the span's end (%.0f uu on; a frame of back-out is 5)"), Wandered),
		Wandered < 20.0);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A WAITER WHOSE EXTENSION WAS REFUSED IS OFFERED A STAND AGAIN WHEN IT STOPS (issue #455, item 5).
 *
 * ReofferStands consumes bStandsMayHaveFreed whether or not it placed anyone, and a Taxiing waiter whose
 * ExtendRoute was refused was told it keeps waiting and is asked again "when something frees". The stand it was
 * offered was already free, so nothing freed: the aircraft taxied to the end of its truncated route, parked there
 * still waiting, and sat beside a free stand for good. Its own stop is the retry.
 *
 * The refusal is staged as ReofferRefusedExtensionKeepsWaiting stages it (the goal pointed off the plan's end), and
 * then the cause CLEARS - the goal goes back where the plan ends, as a tail that joins on the next look would - which
 * is the transient refusal the issue describes. Nothing frees a stand from then on. It must get the stand anyway.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReofferRefusedExtensionRetriesWhenItStopsTest,
	"Airside.Model.Traffic.ReofferRefusedExtensionRetriesWhenItStops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReofferRefusedExtensionRetriesWhenItStopsTest::RunTest(const FString& Parameters)
{
	FTaxiingWaiter W;
	if (!W.Build(*this)) { return false; }
	const FRoadAgent* Waiter = W.Traffic->FindAgent(W.Id);
	const FGuidelineNodeId LiveEnd = Waiter->GoalNode;
	const FGuidelineNodeId Start = Waiter->Follower.Plan.Start;
	if (!TestTrue(TEXT("the plan's start is a live node other than where it ends"),
		Start.IsSet() && Start != LiveEnd && W.Air.Net->GetGuidelineNode(Start) != nullptr)) { return false; }
	FGroundTrafficTestAccess Access(*W.Traffic);
	if (!TestTrue(TEXT("the goal is moved off the plan's end"), Access.SetGoal(W.Id, Start))) { return false; }

	// THE REFUSAL: the tick's re-offer cannot extend it, and consumes the freed flag.
	W.Traffic->Advance(0.05, W.Air.Net);
	TestFalse(TEXT("the refused pass consumed the freed flag: nothing asks again unless something frees"), W.Traffic->StandsMayHaveFreedForTest());
	Waiter = W.Traffic->FindAgent(W.Id);
	if (!TestNotNull(TEXT("the aircraft is still there"), Waiter)) { return false; }
	if (!TestTrue(TEXT("still Taxiing and still waiting: it was not redirected"), Waiter->Phase == EAgentPhase::Taxiing && Waiter->bAwaitingStand)) { return false; }

	// THE CAUSE CLEARS: the plan's end is its goal again. No stand frees from here on.
	if (!TestTrue(TEXT("the goal is put back at the plan's end"), Access.SetGoal(W.Id, LiveEnd))) { return false; }

	const bool bPlaced = RunUntil(*W.Traffic, *W.Air.Net, 600.0, [&]()
	{
		const FRoadAgent* P = W.Traffic->FindAgent(W.Id);
		return P != nullptr && P->Phase == EAgentPhase::Parked && !P->bAwaitingStand;
	});
	const FRoadAgent* Done = W.Traffic->FindAgent(W.Id);
	if (!TestNotNull(TEXT("the aircraft is still there at the end"), Done)) { return false; }
	TestTrue(TEXT("it stopped at the end of its route and was offered the stand that had appeared, and parked on it (red: it sat waiting beside a free stand)"),
		bPlaced && Done->GoalNode == W.Air.Pose(W.Placed));
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A NODE ONE VEHICLE HOLDS AS A RESERVATION STAYS ITS RESERVER'S AGAINST A NEARER CLAIMANT (issue #455, item 6).
 *
 * A vehicle refused a node stops VehicleGap short of it, measured from its CENTRE - and the claim it then makes on
 * that node turns OCCUPIED once the centre is within half a footprint of it, plus the node's reach (FClaimPass:
 * `|End - T| < F * 0.5 + ExcessTo`, the reach cancelling against StopWithinFor's own). With
 * VehicleGap 300 and half a VehicleFootprint 334.75 the refused vehicle stops INSIDE the zone where its own claim is an
 * occupancy, and the table's rule that presence beats a reservation (a body already standing there is never moved)
 * hands it the node the other vehicle reserved first. Measured on #453's span test: the van, being nearer, took the
 * truck's span-end node and the truck stopped 188 uu short; this is the same defect with neither vehicle reversing.
 *
 * TWO VANS ARE SENT TO THE SAME NODE FROM TWO ROADS, AT EQUAL DISTANCE, the lower id first in the arbitration order
 * and so first to reserve it. The one that reserved it must be the one that gets it: it parks there, and the other
 * is left waiting on it. An END node and not a crossing one on purpose - the box-entry rule stops a vehicle well short
 * of a junction, and it is the plain end-node rule (a gap short of the node itself) that reaches the threshold.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReservedNodeKeepsItsReserverTest,
	"Airside.Model.Traffic.ReservedNodeKeepsItsReserver",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReservedNodeKeepsItsReserverTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId West = TestGraph::Node(*Net, -3000.0, 0.0);
	const FGuidelineNodeId South = TestGraph::Node(*Net, 0.0, -3000.0);
	const FGuidelineNodeId Goal = TestGraph::Node(*Net, 0.0, 0.0);
	TestGraph::Join(*Net, West, Goal, { EGuidelineDir::AToB });
	TestGraph::Join(*Net, South, Goal, { EGuidelineDir::AToB });

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Reserver = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, West, Goal, ETraversalClass::GroundVehicle),
		TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	const int32 Claimant = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, South, Goal, ETraversalClass::GroundVehicle),
		TestAirframes::Van(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("both vans are dispatched, the reserver first (it is asked first, and reserves the node first)"),
		Reserver > 0 && Claimant > Reserver)) { return false; }

	// THE PASS SAYS WHEN TWO BODIES ARE BOTH STANDING ON ONE NODE ("overlaps ... both are standing on it"): the
	// refused van stopping inside the occupied zone is exactly that, a body 300 uu from a node another is parked on.
	struct FSpy : public FOutputDevice
	{
		int32 Overlaps = 0;
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category != FName(TEXT("LogAirsideTraffic"))) { return; }
			Overlaps += FString(V).Contains(TEXT("both are standing on it")) ? 1 : 0;
		}
	} Spy;

	bool bClaimantWaitedOnReserver = false;
	bool bReserverWaitedOnClaimant = false;
	GLog->AddOutputDevice(&Spy);
	RunUntil(*Traffic, *Net, 60.0, [&]()
	{
		const FRoadAgent* R = Traffic->FindAgent(Reserver);
		const FRoadAgent* C = Traffic->FindAgent(Claimant);
		if (R == nullptr || C == nullptr) { return true; }
		bClaimantWaitedOnReserver |= C->GetWaitingOn() == Reserver;
		bReserverWaitedOnClaimant |= R->GetWaitingOn() == Claimant;
		return R->Phase == EAgentPhase::Parked || C->Phase == EAgentPhase::Parked;
	});
	// A FEW SECONDS MORE, so the loser has stopped wherever it is going to stop.
	for (int32 Tick = 0; Tick < 100; ++Tick)
	{
		Traffic->Advance(0.05, Net);
	}
	GLog->RemoveOutputDevice(&Spy);

	const FRoadAgent* R = Traffic->FindAgent(Reserver);
	const FRoadAgent* C = Traffic->FindAgent(Claimant);
	if (!TestTrue(TEXT("both vans are still there"), R != nullptr && C != nullptr)) { return false; }
	TestTrue(TEXT("they met: the claimant was refused the node the reserver held (or this proves nothing)"), bClaimantWaitedOnReserver);
	TestEqual(TEXT("the RESERVER got the node: it is the one that parked on it (red while the refused van stopped inside the occupied zone and took it)"),
		R->Phase, EAgentPhase::Parked);
	TestFalse(TEXT("and the reserver was never refused by the claimant"), bReserverWaitedOnClaimant);
	TestEqual(TEXT("the claimant is still Taxiing, waiting for a node that is taken"), C->Phase, EAgentPhase::Taxiing);
	TestEqual(TEXT("on the reserver"), C->GetWaitingOn(), Reserver);
	TestEqual(TEXT("and it waits OUTSIDE the zone where its own claim would be an occupancy: no two bodies on the node at once"),
		Spy.Overlaps, 0);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * THE RESOLVER DOES NOT REPLAN A TRUCK OUT OF ITS BAY (issue #455, review of #466).
 *
 * A truck refused the ground of the reverse leg it is about to arm (UGroundTraffic::GateReverseLeg) is Taxiing and
 * stopped, with BlockedStep on the reverse step and ToNode ~0: it passed each of CanReplanAtBlockedStep's checks, and
 * ReplanAt has no reverse-leg handling, so a ring it was in could have it spliced out of the bay - forwards, along
 * whatever road leaves the service point - instead of waited out. DECIDED: a refusal at a reverse step is not a turn.
 *
 * THE BAY GETS A FORWARD ROAD OUT (Service -> Alt -> Exit), so a replan around the banned reverse edge EXISTS - without
 * that the resolver fails to replan the truck for want of a road and this proves nothing. The CONTROL is the same
 * truck with the step not marked as a reverse leg: the resolver replans it, which is what makes "did not" mean
 * something. Staged bare, as DeadlockResolverStandalone stages a ring: Refuse + AccrueStall on hand-built agents, and
 * OCCUPIED claims, so the cycle is a deadlock and not a yield of reservations.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficResolverDoesNotReplanATruckOutOfItsBayTest,
	"Airside.Model.Traffic.Deadlock.ResolverDoesNotReplanATruckOutOfItsBay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficResolverDoesNotReplanATruckOutOfItsBayTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	if (!TestTrue(TEXT("the bay routes as arrive, reverse, depart"), Bay.RouteIsTheThreeLegs())) { return false; }
	const FGuidelineNodeId Alt = TestGraph::Node(*Bay.Net, 4000.0, 2000.0);
	Bay.Lay(Bay.Service, Alt, false);
	Bay.Lay(Alt, Bay.Exit, false);

	// Returns the resolver after one Resolve over a truck waiting at the start of its reverse leg (refused the far
	// node by an aircraft) and that aircraft (refused the service point by the truck).
	auto Resolve = [&](bool bMarkedReverse, FRoadAgent& OutTruck, FDeadlockResolver& OutResolver)
	{
		FTrafficRules Rules;
		FTrafficOccupancy Occupancy;
		auto Stand = [&Occupancy](int32 Agent, FGuidelineNodeId Node)
		{
			FTrafficClaim Claim;
			Claim.AgentId = Agent;
			Claim.Resource = FTrafficResource::OfNode(Node);
			Claim.bOccupied = true;
			FTrafficClaim Blocker;
			Occupancy.TryClaim(Claim, Blocker);
		};
		Stand(2, Bay.Cleared);
		Stand(1, Bay.Service);

		TArray<FRoadAgent> Agents;
		Agents.SetNum(2);
		FRoadAgent& Truck = Agents[0];
		Truck.Id = 1;
		Truck.StampRules(Rules, 1.0);
		Truck.StartDrive(Bay.Route, Bay.Truck);
		Truck.Class = ETraversalClass::GroundVehicle;
		Truck.SetGoalFrom(Bay.Route);
		Truck.Follower.Travelled = Bay.Route.Steps[0].EndDistance;
		Truck.Follower.Speed = 0.0;
		if (!bMarkedReverse)
		{
			Truck.Follower.Plan.Steps[1].bReverseLeg = false;
		}
		Truck.Refuse(1, FTrafficResource::OfNode(Bay.Cleared), TNumericLimits<double>::Max(), 2);
		Truck.AccrueStall(10.0);
		FRoadAgent& Plane = Agents[1];
		Plane.Id = 2;
		Plane.Refuse(INDEX_NONE, FTrafficResource::OfNode(Bay.Service), TNumericLimits<double>::Max(), 1);
		Plane.AccrueStall(10.0);

		TMap<int32, int32> AgentIndex;
		AgentIndex.Add(1, 0);
		AgentIndex.Add(2, 1);
		FNodeReachCache Reach;
		FRunwayChainCache Chains;
		FPlanReResolver PlanReResolver;
		OutResolver.Resolve(Agents, AgentIndex, FTrafficContext{ *Bay.Net, Rules, Occupancy, Reach, Chains, 100.0 }, PlanReResolver);
		OutTruck = Agents[0];
	};

	FRoadAgent Control;
	FDeadlockResolver ControlResolver;
	Resolve(false, Control, ControlResolver);
	if (!TestEqual(TEXT("control: the same truck, its step not a reverse leg, IS replanned - the forward road out exists and the resolver takes it"),
		ControlResolver.LastResolvedAgent, 1)) { return false; }
	TestTrue(TEXT("control: and its route changed"), Control.Follower.Plan.Steps.Num() != Bay.Route.Steps.Num()
		|| Control.Follower.Plan.Steps[1].Edge != Bay.Route.Steps[1].Edge);

	FRoadAgent Truck;
	FDeadlockResolver Resolver;
	Resolve(true, Truck, Resolver);
	TestEqual(TEXT("refused at the reverse step, the truck is NOT replanned (red while CanReplanAtBlockedStep let a reverse step through)"),
		Resolver.LastResolvedAgent, 0);
	bool bSameRoute = Truck.Follower.Plan.Steps.Num() == Bay.Route.Steps.Num();
	for (int32 Step = 0; bSameRoute && Step < Bay.Route.Steps.Num(); ++Step)
	{
		bSameRoute = Truck.Follower.Plan.Steps[Step].Edge == Bay.Route.Steps[Step].Edge;
	}
	TestTrue(TEXT("and it keeps the route it had: still backing out of its bay"), bSameRoute);
	TestEqual(TEXT("the jam was seen and left, on the resolver's cadence"), Resolver.CyclesSeen.Num(), 1);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * THE WAIT BEFORE A REVERSE IS A REFUSAL AT THE SPAN, NOT ANY REFUSAL (issue #455, review of #466).
 *
 * TryArmReverseLeg waited on `WaitingOn != 0`, which says nothing about WHERE the refusal was: one left on the agent
 * by another part of the route - or by the route it has just been redirected off - held the arm, and named a holder
 * that was not in the way. It is keyed on a refusal at a step of the span now, and RedirectAgent clears the agent's
 * arbitration before the gate.
 *
 * TWO HALVES. BARE: an agent at the span's start refused at step 0 (outside the span) arms; refused at step 1 (the
 * span) does not. STALE: a parked truck carrying a refusal AT the span's step - what a Taxiing agent redirected
 * mid-wait would carry, staged through ScriptWait - is redirected into free ground and arms, because the redirect
 * cleared what the old route left.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficReverseIgnoresARefusalElsewhereOnTheRouteTest,
	"Airside.Model.Traffic.ReverseIgnoresARefusalElsewhereOnTheRoute",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficReverseIgnoresARefusalElsewhereOnTheRouteTest::RunTest(const FString& Parameters)
{
	FBackingBay Bay = FBackingBay::Build();
	if (!TestTrue(TEXT("the bay routes as arrive, reverse, depart"), Bay.RouteIsTheThreeLegs())) { return false; }
	const double SpanStart = Bay.Route.Steps[0].EndDistance;

	// BARE: the agent stands at the span's start and is asked to arm, with a refusal at the step named.
	auto Armed = [&](int32 RefusedStep) -> bool
	{
		FTrafficRules Rules;
		FRoadAgent Truck;
		Truck.Id = 1;
		Truck.StampRules(Rules, 1.0);
		Truck.StartDrive(Bay.Route, Bay.Truck);
		Truck.Class = ETraversalClass::GroundVehicle;
		Truck.Follower.Travelled = SpanStart;
		Truck.Follower.Speed = 0.0;
		Truck.Refuse(RefusedStep, FTrafficResource::OfNode(Bay.Cleared), TNumericLimits<double>::Max(), 900);
		FAgentMotion Motion;
		EAgentEvent Event;
		Truck.Advance(0.0, Motion, Event);
		return Truck.Phase == EAgentPhase::Reversing;
	};
	TestTrue(TEXT("a refusal at step 0 - the serve leg, off the span - does not hold the arm (red while any WaitingOn did)"), Armed(0));
	TestTrue(TEXT("and one at INDEX_NONE, which names no step, does not either"), Armed(INDEX_NONE));
	TestFalse(TEXT("control: a refusal at step 1, the reverse step itself, holds it"), Armed(1));

	// STALE: a parked truck with a refusal at the span's step, sent home into FREE ground.
	const FRoutePlan ToService = TestGraph::Probe(*Bay.Net, Bay.Approach, Bay.Service, ETraversalClass::GroundVehicle);
	const FRoutePlan Home = TestGraph::Probe(*Bay.Net, Bay.Service, Bay.Exit, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the routes to the service point, and home from it (reverse first), exist"),
		ToService.IsValid() && Home.IsValid() && Home.Steps.Num() == 2 && Home.Steps[0].bReverseLeg)) { return false; }
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Truck = Traffic->DispatchAgent(Bay.Net, ToService, Bay.Truck, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the truck is dispatched"), Truck > 0)) { return false; }
	if (!TestTrue(TEXT("and parks at the service point"), RunUntil(*Traffic, *Bay.Net, 90.0, [&]()
		{
			const FRoadAgent* P = Traffic->FindAgent(Truck);
			return P != nullptr && P->Phase == EAgentPhase::Parked;
		}))) { return false; }
	// A REFUSAL LEFT AT STEP 0 OF THE NEW ROUTE, which is the reverse step: exactly the index the old route's refusal
	// could coincide with. Nobody holds anything - the holder is a name in a stale field.
	if (!TestTrue(TEXT("a stale refusal is left on it"),
		FGroundTrafficTestAccess(*Traffic).ScriptWait(Truck, FTrafficResource::OfNode(Bay.Cleared), 900, 0.0, 0))) { return false; }
	if (!TestTrue(TEXT("it is sent home"), Traffic->RedirectAgent(Truck, Bay.Net, Home))) { return false; }
	TestEqual(TEXT("and arms the reverse at once: the redirect cleared the old route's refusal (red while a stale WaitingOn held the arm)"),
		Traffic->FindAgent(Truck)->Phase, EAgentPhase::Reversing);
	return true;
}

// ---------------------------------------------------------------------------------------
// EVERY OPERATION ANNOUNCES ITS CAUSE (#436), through the one Announce: an admit is Dispatched, a park Parked on the
// node it parked on, a redirect the caller's cause with its new goal, a retire Retired and a clear Cleared - each To
// Gone. The seam is the Cause each door writes and GoalAtEvent: unwire either (announce None, or the live goal later)
// and a line below goes red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficOperationsAnnounceTheirCauseTest,
	"Airside.Model.Traffic.OperationsAnnounceTheirCause",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficOperationsAnnounceTheirCauseTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(20000.0, 0.0), false);
	TestGraph::Join(*Net, A, B, { EGuidelineDir::Bidirectional, nullptr, false });
	const FRoutePlan Out = TestGraph::Probe(*Net, A, B, ETraversalClass::GroundVehicle);
	const FRoutePlan Back = TestGraph::Probe(*Net, B, A, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the line routes both ways"), Out.IsValid() && Back.IsValid())) { return false; }

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	TArray<FAgentTransition> Heard;
	Traffic->OnAgentPhaseChanged.AddLambda([&Heard](const FAgentTransition& T) { Heard.Add(T); });
	const auto Last = [&Heard]() { return Heard.Num() > 0 ? Heard.Last() : FAgentTransition(); };

	const FVehicle Van = UAirsideSettings::ResolveDefaultVehicle();
	const int32 Id = Traffic->DispatchAgent(Net, Out, Van, ETraversalClass::GroundVehicle, 0.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }
	TestTrue(TEXT("the admit is Dispatched, Gone -> Taxiing"), Last().AgentId == Id && Last().Cause == EAgentEvent::Dispatched
		&& Last().From == EAgentPhase::Gone && Last().To == EAgentPhase::Taxiing);

	if (!TestTrue(TEXT("it parks at the far end"), RunUntil(*Traffic, *Net, 120.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P != nullptr && P->Phase == EAgentPhase::Parked; })))
	{
		return false;
	}
	TestTrue(TEXT("the park is Parked, on the node it parked on"), Last().Cause == EAgentEvent::Parked
		&& Last().To == EAgentPhase::Parked && Last().GoalAtEvent == B);

	TestTrue(TEXT("redirected"), Traffic->RedirectAgent(Id, Net, Back, EAgentEvent::ReOffered));
	TestTrue(TEXT("the redirect carries the CALLER's cause and the NEW goal"), Last().Cause == EAgentEvent::ReOffered
		&& Last().From == EAgentPhase::Parked && Last().To == EAgentPhase::Taxiing && Last().GoalAtEvent == A);

	TestTrue(TEXT("retired"), Traffic->RetireAgent(Id));
	TestTrue(TEXT("the retire is Retired, to Gone"), Last().AgentId == Id && Last().Cause == EAgentEvent::Retired
		&& Last().From == EAgentPhase::Taxiing && Last().To == EAgentPhase::Gone);

	const int32 Second = Traffic->DispatchAgent(Net, Out, Van, ETraversalClass::GroundVehicle, 0.0);
	Traffic->ClearAgents();
	TestTrue(TEXT("a clear is Cleared, to Gone"), Last().AgentId == Second && Last().Cause == EAgentEvent::Cleared
		&& Last().To == EAgentPhase::Gone);

	for (const FAgentTransition& T : Heard)
	{
		TestNotEqual(*FString::Printf(TEXT("agent %d %s -> %s names a cause"), T.AgentId, *UEnum::GetValueAsString(T.From),
			*UEnum::GetValueAsString(T.To)), T.Cause, EAgentEvent::None);
		TestNotEqual(TEXT("and a phase change is a change"), T.From, T.To);
	}
	return true;
}

#endif
