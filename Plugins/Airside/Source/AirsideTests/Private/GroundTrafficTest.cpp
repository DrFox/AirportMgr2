#include "CoreMinimal.h"
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

	/** A Piper, which unlike M2TrafficPlane still has its Climb figures and so can land.
	 *  Only Airside.Model.Traffic.GraphRebuild's arrival case needs one. */
	FAirframe M2TrafficPiper()
	{
		FAirframe A;
		A.Ground = UAircraftType::PiperMeridianGround();
		A.Climb = UAircraftType::PiperMeridianClimb();
		A.Approach = UAircraftType::PiperMeridianApproach();
		A.Engine = UAircraftType::PiperMeridianEngine();
		return A;
	}

	/**
	 * The smallest airport an arrival can be dispatched into: a runway split at ONE exit (the
	 * split is what puts a guideline node on the centreline for RunwayExitNodes to find), a
	 * taxiway south from it, and a stand beside that taxiway facing east so its lead-in casts
	 * west and meets the taxiway. Modelled on ArrivalPlannerTest's two-exit fixture, cut down
	 * to the one exit this test needs, and M2-prefixed against the unity build.
	 *
	 * DERIVED THROUGHOUT, deliberately: every guideline in it comes from FRoadGuidelineBuilder,
	 * so re-running the builder frees the whole graph and hands back fresh handles - which is
	 * the event OnGraphRebuilt exists for, done by the real thing rather than by hand.
	 */
	URoadNetwork* M2TrafficArrivalAirport(const FAirframe& Airframe, FVector2D& OutThreshold)
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		OutThreshold = FVector2D(0.0, 0.0);

		const double Needed = FLandingRun::RequiredLandingDistance(
			Airframe.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;
		const FVector2D ExitAt(Needed * 1.2, 0.0);
		const FVector2D FarAt(Needed * 3.0, 0.0);

		URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

		const FRoadNodeId ThresholdNode = Net->AddNode(OutThreshold);
		const FRoadNodeId ExitNode = Net->AddNode(ExitAt);
		const FRoadNodeId FarNode = Net->AddNode(FarAt);
		Net->AddStraightSegment(ThresholdNode, ExitNode, Runway);
		Net->AddStraightSegment(ExitNode, FarNode, Runway);

		const FRoadNodeId TaxiEnd = Net->AddNode(ExitAt + FVector2D(0.0, -20000.0));
		Net->AddStraightSegment(ExitNode, TaxiEnd, Taxiway);

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		FRoadGuidelineBuilder::Build(*Net, Solved);

		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Net->PlaceEntity(Stand, Stand->Anchors, ExitAt + FVector2D(9000.0, -10000.0), 0.0);
		FAnchorLink::Build(*Net);
		return Net;
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
		VanMinSpeedWhileWaiting < 0.5 * VanNow->Follower.Ground.Taxi.SpeedCap);
	TestTrue(TEXT("the aircraft never was"), PlaneMinStopWithin > 1000.0);
	TestTrue(TEXT("the van's wait named the aircraft"), bVanWaitedOnPlane);
	TestTrue(FString::Printf(TEXT("never closer than the van's own footprint (%.0f uu)"), MinSeparation), MinSeparation >= Traffic->Rules.VehicleFootprint - 1.0);
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
	const double StalledFor = FMath::Max(X->StalledSeconds, Y->StalledSeconds);
	const double SinceFirst = FMath::Max(0.0, StalledFor - Traffic->Rules.StallSeconds);
	const int32 Expected = FMath::FloorToInt32(SinceFirst / Traffic->Rules.RetrySeconds) + 1;
	UE_LOG(LogM2TrafficTest, Log,
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
	Net->SetRunwayHoldingPositionForTest(H, RunwaySeg);

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
		TEXT("HoldingPosition measured: centre %.1f uu, nose %.1f uu (bar 17000), speed %.4f, waiting on %d, blocked step %d"),
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

	// AND THE SAME REFUSAL WITH NO PHANTOM: TWO REAL ARRIVALS, BACK TO BACK, NO TICK BETWEEN.
	//
	// The block above plants the hold by hand, so it measures ArrivalPlanner reading the
	// table - not that a landing ever PUTS anything in it. DispatchArrival recorded the chain
	// on the agent and left the claim to the next Advance, which means the strip was free to
	// the very next DispatchArrival: two aircraft cleared onto one runway for one frame, the
	// window the Taxiing->Departing handover already raises its claim synchronously to avoid
	// (see UGroundTraffic::Advance). A player pressing 7 twice is exactly that call pattern.
	{
		FVector2D Threshold;
		const FAirframe Piper = M2TrafficPiper();
		URoadNetwork* Airport = M2TrafficArrivalAirport(Piper, Threshold);
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
				UE_LOG(LogM2TrafficTest, Log,
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
	Net->SetRunwayHoldingPositionForTest(H, RunwaySeg);
	Net->SetRunwayHoldingPositionForTest(Far, RunwaySeg);

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

	/** One injected graph rebuild, taken while the BODY is on the strip. See below. */
	bool bRebuiltMidCrossing = false;
	bool bStripHeldAcrossRebuild = false;

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
		if (!bRebuiltMidCrossing && Q->CrossingPhase == ECrossingPhase::OnStrip)
		{
			bRebuiltMidCrossing = true;
			Traffic->OnGraphRebuilt(*Net);
			bStripHeldAcrossRebuild = Traffic->GetOccupancy().IsHeld(Strip, 0);
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
	TestTrue(TEXT("the injected rebuild really did land while the body was on the strip"), bRebuiltMidCrossing);
	TestTrue(TEXT("and it kept the strip held: a guideline rebuild frees guidelines, never surfaces"),
		bStripHeldAcrossRebuild);
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
	M2TrafficRun(*Traffic, *Net, 60.0, [&](int32)
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

	M2TrafficRun(*Traffic, *Net, 200.0, [&](int32)
	{
		Sample();
		return Traffic->FindAgent(Van)->Phase != EAgentPhase::Parked;
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
	const FGuidelineNodeId A = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = M2TrafficNode(*Net, 750.0, 0.0);
	const FGuidelineNodeId C = M2TrafficNode(*Net, 750.0, 750.0);
	const FGuidelineNodeId D = M2TrafficNode(*Net, 0.0, 750.0);
	const FGuidelineNodeId X1 = M2TrafficNode(*Net, -500.0, 1250.0);
	const FGuidelineNodeId X2 = M2TrafficNode(*Net, 1500.0, 1250.0);
	const FGuidelineNodeId X3 = M2TrafficNode(*Net, 1500.0, 0.0);
	M2TrafficJoin(*Net, A, B, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, B, C, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, C, D, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, D, A, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, D, X1, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, X1, X2, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, X2, X3, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, X3, B, EGuidelineDir::AToB);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 V1 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V2 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, B, D, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V3 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, C, A, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V4 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, D, B, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("all four routed and dispatched"), V1 > 0 && V2 > 0 && V3 > 0 && V4 > 0)) { return false; }
	TestEqual(TEXT("V4's first plan goes via A (2 steps), not the escape"), Traffic->FindAgent(V4)->Follower.Plan.Steps.Num(), 2);

	// All four stopped, each waiting on the next, before any resolution.
	M2TrafficRun(*Traffic, *Net, 1.0, [](int32) { return true; });
	TestTrue(TEXT("V1 waits on V2"), Traffic->FindAgent(V1)->WaitingOn == V2);
	TestTrue(TEXT("V2 waits on V3"), Traffic->FindAgent(V2)->WaitingOn == V3);
	TestTrue(TEXT("V3 waits on V4"), Traffic->FindAgent(V3)->WaitingOn == V4);
	TestTrue(TEXT("V4 waits on V1"), Traffic->FindAgent(V4)->WaitingOn == V1);
	bool bNobodyMoved = true;
	for (const int32 Id : { V1, V2, V3, V4 }) { bNobodyMoved &= Traffic->FindAgent(Id)->Follower.Travelled < 1.0; }
	TestTrue(TEXT("nobody has moved"), bNobodyMoved);

	double MaxJump = 0.0;
	double MinSeparation = TNumericLimits<double>::Max();
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

	UE_LOG(LogM2TrafficTest, Log,
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
		MinSeparation >= Traffic->Rules.VehicleGap * FMath::Sqrt(2.0) - 1.0);
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
	const FGuidelineNodeId A = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = M2TrafficNode(*Net, 750.0, 0.0);
	const FGuidelineNodeId C = M2TrafficNode(*Net, 750.0, 750.0);
	const FGuidelineNodeId D = M2TrafficNode(*Net, 0.0, 750.0);
	const FGuidelineNodeId X1 = M2TrafficNode(*Net, 1500.0, 750.0);
	const FGuidelineNodeId X2 = M2TrafficNode(*Net, 1500.0, -1000.0);
	const FGuidelineNodeId X3 = M2TrafficNode(*Net, 0.0, -1000.0);
	const FGuidelineNodeId Y1 = M2TrafficNode(*Net, 750.0, -750.0);
	const FGuidelineNodeId Y2 = M2TrafficNode(*Net, -750.0, -750.0);
	const FGuidelineNodeId Y3 = M2TrafficNode(*Net, -750.0, 750.0);
	M2TrafficJoin(*Net, A, B, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, B, C, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, C, D, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, D, A, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, C, X1, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, X1, X2, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, X2, X3, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, X3, A, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, B, Y1, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, Y1, Y2, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, Y2, Y3, EGuidelineDir::AToB);
	M2TrafficJoin(*Net, Y3, D, EGuidelineDir::AToB);

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
	Traffic->Rules.AircraftFootprint = Traffic->Rules.VehicleFootprint;
	Traffic->Rules.AircraftGap = Traffic->Rules.VehicleGap;
	const int32 V1 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	const int32 V2 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, B, D, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	const int32 P3 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, C, A, ETraversalClass::Aircraft), M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
	const int32 V4 = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, D, B, ETraversalClass::GroundVehicle), M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("all four routed and dispatched"), V1 > 0 && V2 > 0 && P3 > 0 && V4 > 0)) { return false; }
	TestEqual(TEXT("the van on B goes via C (2 steps), not round Y"), Traffic->FindAgent(V2)->Follower.Plan.Steps.Num(), 2);
	TestEqual(TEXT("and the aircraft via D, not round X"), Traffic->FindAgent(P3)->Follower.Plan.Steps.Num(), 2);

	int32 FirstResolved = 0;
	bool bAircraftEverReplanned = false;
	double MinSeparation = TNumericLimits<double>::Max();
	M2TrafficRun(*Traffic, *Net, 180.0, [&](int32)
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

	UE_LOG(LogM2TrafficTest, Log,
		TEXT("DeadlockMixedClass measured: first resolved agent %d (van 1 = %d, van 2 = %d, aircraft = %d, van 4 = %d), ")
		TEXT("%d cycle(s), %d line(s), aircraft plan %d step(s), min separation while taxiing %.0f uu"),
		FirstResolved, V1, V2, P3, V4, Traffic->GetCyclesDetectedForTest(),
		Traffic->GetDeadlockLogLinesForTest(),
		Traffic->FindAgent(P3) ? Traffic->FindAgent(P3)->Follower.Plan.Steps.Num() : 0, MinSeparation);
	for (const FRoadAgent& Agent : Traffic->GetAgents())
	{
		UE_LOG(LogM2TrafficTest, Log,
			TEXT("  agent %d: phase %s, %.0f of %.0f uu, waiting on %d, blocked step %d, stalled %.1f s"),
			Agent.Id, *UEnum::GetValueAsString(Agent.Phase), Agent.Follower.Travelled,
			Agent.Follower.Plan.Length, Agent.WaitingOn, Agent.BlockedStep, Agent.StalledSeconds);
	}

	TestEqual(TEXT("the lowest-ranked member that can turn goes round: the van, not the aircraft"), FirstResolved, V2);
	TestFalse(TEXT("and the aircraft keeps the route it was cleared for"), bAircraftEverReplanned);
	for (const int32 Id : { V1, V2, P3, V4 })
	{
		TestEqual(FString::Printf(TEXT("agent %d reached its goal"), Id), Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked);
	}
	// The corner bound, as in DeadlockRing: a gap short of one corner on two arms.
	TestTrue(FString::Printf(TEXT("never closer than two vans queued at one corner (%.0f uu)"), MinSeparation),
		MinSeparation >= Traffic->Rules.VehicleGap * FMath::Sqrt(2.0) - 1.0);
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
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
	const FRoadSegmentId RunwaySeg = Net->AddStraightSegment(RA, RB, Runway);

	const FGuidelineNodeId S = M2TrafficNode(*Net, 0.0, -20000.0);
	const FGuidelineNodeId Hn = M2TrafficNode(*Net, 0.0, -3000.0);
	const FGuidelineNodeId Hf = M2TrafficNode(*Net, 0.0, 3000.0);
	const FGuidelineNodeId N = M2TrafficNode(*Net, 0.0, 20000.0);
	M2TrafficJoin(*Net, S, Hn);
	M2TrafficJoin(*Net, Hn, Hf);   // ONE edge across the runway. No vertex on the strip.
	M2TrafficJoin(*Net, Hf, N);
	Net->SetRunwayHoldingPositionForTest(Hn, RunwaySeg);
	Net->SetRunwayHoldingPositionForTest(Hf, RunwaySeg);

	// Route distances: near bar 17000, centreline 20000, far bar 23000, N 40000. Half width
	// 2250, so the strip runs from 17750 to 22250 in route distance. Footprint 1000, so the
	// NOSE is on the asphalt from centre 17250 and the TAIL is off it from centre 22750.
	const double HalfWidth = 2250.0;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Airframe = M2TrafficPlane();
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
	// into bounded substeps (see UGroundTraffic::MaxSubstepSeconds), and a call holding two
	// passes arbitrates twice - at the start pose and half a tick later - while this variable
	// still names only the first. Every threshold here would then be read against a pose the
	// agent had already left, which is measuring the substep count rather than the crossing
	// rule. Ticking at the substep keeps the call a single pass, and has the better property
	// of measuring the crossing at the granularity PRODUCTION now runs it: no frame reaches
	// the model coarser than this, whatever the speed multiplier.
	double SeenByTheArbiter = 0.0;

	M2TrafficRun(*Traffic, *Net, 120.0, [&](int32)
	{
		const FRoadAgent* Q = Traffic->FindAgent(Plane);
		if (Q == nullptr) { return false; }
		const double T = SeenByTheArbiter;
		const bool bHeld = Traffic->GetOccupancy().IsHeld(Strip, 0);
		SeenByTheArbiter = Q->Follower.Travelled;

		// More than one window (braking distance + gap, about 4000 uu) short of the bar:
		// nothing has any business holding the strip yet.
		if (T < 10000.0 && !bHeld) { bFreeWellBeforeTheBar = true; }

		if (ArmedAt < 0.0 && Q->CrossingPhase != ECrossingPhase::None) { ArmedAt = T; }
		if (ArmedAt >= 0.0 && ReleasedAt < 0.0 && Q->CrossingPhase == ECrossingPhase::None) { ReleasedAt = T; }
		if (T > 23100.0 && Q->CrossingPhase != ECrossingPhase::None) { bCrossingPastTheFarBar = true; }

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
	}, Traffic->MaxSubstepSeconds);

	const FRoadAgent* P = Traffic->FindAgent(Plane);
	UE_LOG(LogM2TrafficTest, Log,
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
	// the substep and the agent cruises at 1000 uu/s (M2TrafficPlane's taxi cap), so a tick
	// is about 34 uu and "armed on the tick the nose reached the asphalt" cannot be measured
	// tighter than that. The release bound needs no such slack downward - a release before
	// the tail is off would be the defect itself - only the one tick upward.
	//
	// THESE WERE 60 AND 100 while the fixture ticked at 0.05, which is coarser than any frame
	// production now hands the model. Measured at the substep the crossing arms ON 17250 and
	// releases 33 uu past 22750 - a tick, exactly - so the wider bounds were tolerance for the
	// sampling rate and not for the rule. Tightening them is the point of ticking finer: at 60
	// the arming could drift more than a whole tick late and this test would still pass.
	const double OneTick = Traffic->MaxSubstepSeconds * 1000.0 + 1.0;
	TestTrue(FString::Printf(TEXT("armed no later than the nose entered the strip (%.0f, bound 17250 + one tick)"), ArmedAt),
		ArmedAt >= 0.0 && ArmedAt <= 20000.0 - HalfWidth - Half + OneTick);
	TestTrue(FString::Printf(TEXT("released no earlier than the tail left it, and within one tick after (%.0f, want 22750)"), ReleasedAt),
		ReleasedAt >= 20000.0 + HalfWidth + Half && ReleasedAt <= 20000.0 + HalfWidth + Half + OneTick);
	TestFalse(TEXT("and the bar on the way OUT arms nothing"), bCrossingPastTheFarBar);
	TestEqual(TEXT("nothing is left crossing past the far bar"), P->CrossingPhase, ECrossingPhase::None);
	TestFalse(TEXT("with no chain left named"), P->CrossingRunway.IsSet());
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

		Build(*Net, true, false, nullptr, nullptr);
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
	// Case 4: an ARRIVING aircraft's TaxiInPlan, which no follower is on yet, re-resolved from
	// step 0. It is the branch the three cases above cannot reach - they all replan a plan
	// under a moving follower - and it is the one that matters most in a game, because a
	// player builds while an aircraft is on final and the route it will vacate onto is derived
	// geometry the next rebuild frees. Rebuilt by the REAL FRoadGuidelineBuilder here, not by
	// hand, so what is measured is the actual event rather than this test's model of it.
	{
		FVector2D Threshold;
		const FAirframe Piper = M2TrafficPiper();
		URoadNetwork* Net = M2TrafficArrivalAirport(Piper, Threshold);
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Plane = Traffic->DispatchArrival(*Net, Threshold - FVector2D(1000.0, 0.0), Piper, 1.0);
		if (TestTrue(TEXT("the arrival is admitted"), Plane > 0))
		{
			const FRoadAgent* P = Traffic->FindAgent(Plane);
			const int32 Steps = P->TaxiInPlan.Steps.Num();
			const FGuidelineEdgeId OldFirst = Steps > 0 ? P->TaxiInPlan.Steps[0].Edge : FGuidelineEdgeId();
			TestTrue(TEXT("with a taxi-in route to re-resolve"), Steps > 0);
			TestEqual(TEXT("and it is Arriving, so nothing is following that route yet"), P->Phase, EAgentPhase::Arriving);

			const FRoadSolveResult Again = FRoadNetworkSolver::SolveAll(*Net);
			FRoadGuidelineBuilder::Build(*Net, Again);
			FAnchorLink::Build(*Net);
			TestNull(TEXT("the builder freed the taxi-in route's first handle"), Net->GetGuidelineEdge(OldFirst));

			Traffic->OnGraphRebuilt(*Net);

			P = Traffic->FindAgent(Plane);
			int32 Live = 0;
			for (const FRouteStep& Step : P->TaxiInPlan.Steps)
			{
				if (Net->GetGuidelineEdge(Step.Edge) != nullptr && Net->GetGuidelineNode(Step.To) != nullptr) { ++Live; }
			}
			UE_LOG(LogM2TrafficTest, Log,
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
		FGuidelineNodeId A, C; Build(*Net, true, false, &A, &C);
		UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, C, ETraversalClass::GroundVehicle),
			M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);

		// PAST B. 30 s at Accel 100 to a cap of 1000 is 25000 uu - comfortably past B at
		// 20000, so the agent is on step 1, and well short of the braking point for C at
		// 37500, so it is still at cruise.
		M2TrafficRun(*Traffic, *Net, 30.0, [](int32) { return true; });
		const FRoadAgent* Before = Traffic->FindAgent(Van);
		if (!TestNotNull(TEXT("the van survived the run up to the rebuild"), Before)) { return false; }
		const double Travelled = Before->Follower.Travelled;
		const FVector2D WasAt = Before->LastMotion.Position;
		if (!TestTrue(FString::Printf(TEXT("the van is ON step 1, past B (%.0f uu)"), Travelled),
			Travelled > 20000.0 && Travelled < 37000.0)) { return false; }

		Build(*Net, false, true, nullptr, nullptr);      // B->C gone, B->D->C in its place
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
		UE_LOG(LogM2TrafficTest, Log,
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
		M2TrafficRun(*Traffic, *Net, 5.0, [&](int32)
		{
			const FRoadAgent* Q = Traffic->FindAgent(Van);
			if (Q == nullptr) { return false; }
			MaxStep = FMath::Max(MaxStep, FVector2D::Distance(Last, Q->LastMotion.Position));
			Last = Q->LastMotion.Position;
			return true;
		});
		UE_LOG(LogM2TrafficTest, Log,
			TEXT("GraphRebuild under-the-agent: max per-tick displacement over the next 5 s %.1f uu"), MaxStep);
		TestTrue(FString::Printf(TEXT("nor in the five seconds after it (max %.1f uu per tick, budget %.0f)"), MaxStep, PerTick),
			MaxStep <= PerTick);
		TestNotNull(TEXT("and it is still there to be retired"), Traffic->FindAgent(Van));
	}
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
	const FGuidelineNodeId A = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = M2TrafficNode(*Net, 0.0, 20000.0);
	const FGuidelineEdgeId AB = M2TrafficJoin(*Net, A, B);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Van = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, A, B, ETraversalClass::GroundVehicle),
		M2TrafficVan(), ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("dispatched"), Van > 0)) { return false; }

	// Under way, and holding: the edge it is on, plus the node it left while its tail is
	// still within half a footprint of it. Two seconds is well short of the 20 km run.
	M2TrafficRun(*Traffic, *Net, 2.0, [&](int32) { return true; });

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
	UE_LOG(LogM2TrafficTest, Log,
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

	// THE PAVEMENT GOES AWAY UNDER IT. StrandForTest sets Result only - the follower keeps
	// its polyline and its distance, so the agent is exactly where it was and the ONLY thing
	// that has changed is that its plan no longer describes the airport.
	if (!TestTrue(TEXT("stranded"), Traffic->StrandForTest(Van))) { return false; }
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
	TestEqual(TEXT("waiting on nobody"), V->WaitingOn, 0);
	TestEqual(TEXT("blocked on no step"), V->BlockedStep, INDEX_NONE);
	TestTrue(TEXT("and under no cap it could drive against"), V->StopWithin >= TNumericLimits<double>::Max());

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
		URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		const FRoadNodeId RA = Cross->AddNode(FVector2D(-50000.0, 0.0));
		const FRoadNodeId RB = Cross->AddNode(FVector2D(50000.0, 0.0));
		const FRoadSegmentId RunwaySeg = Cross->AddStraightSegment(RA, RB, Runway);

		const FGuidelineNodeId S = M2TrafficNode(*Cross, 0.0, -20000.0);
		const FGuidelineNodeId H = M2TrafficNode(*Cross, 0.0, -3000.0);
		const FGuidelineNodeId X = M2TrafficNode(*Cross, 0.0, 0.0);
		const FGuidelineNodeId N = M2TrafficNode(*Cross, 0.0, 20000.0);
		M2TrafficJoin(*Cross, S, H); M2TrafficJoin(*Cross, H, X); M2TrafficJoin(*Cross, X, N);
		Cross->SetRunwayHoldingPositionForTest(H, RunwaySeg);

		UGroundTraffic* Air = NewObject<UGroundTraffic>(GetTransientPackage());
		const int32 Plane = Air->DispatchAgent(Cross, M2TrafficRoute(*Cross, S, N, ETraversalClass::Aircraft),
			M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
		if (TestTrue(TEXT("the crossing aircraft is dispatched"), Plane > 0))
		{
			M2TrafficRun(*Air, *Cross, 120.0, [&](int32)
			{
				const FRoadAgent* Q = Air->FindAgent(Plane);
				return Q != nullptr && Q->CrossingPhase != ECrossingPhase::OnStrip;
			});
			const FRoadAgent* P = Air->FindAgent(Plane);
			if (TestNotNull(TEXT("it is still under way"), P)
				&& TestEqual(TEXT("and its body is ON the strip"), P->CrossingPhase, ECrossingPhase::OnStrip))
			{
				const FTrafficResource Strip = FTrafficResource::OfSurface(RunwaySeg);
				TestTrue(TEXT("so it holds the runway before its plan dies"),
					Air->GetOccupancy().IsHeld(Strip, /*ExcludingAgent=*/0));

				TestTrue(TEXT("stranded mid-crossing"), Air->StrandForTest(Plane));
				Air->Advance(0.05, Cross);

				UE_LOG(LogM2TrafficTest, Log,
					TEXT("DeadPlanReleases crossing measured: %d claim(s) left in the table after the ")
					TEXT("plan died, strip %s"),
					Air->GetOccupancy().GetClaims().Num(),
					Air->GetOccupancy().IsHeld(Strip, 0) ? TEXT("HELD") : TEXT("free"));

				TestTrue(TEXT("it STILL holds the strip it is standing on, one tick later"),
					Air->GetOccupancy().IsHeld(Strip, /*ExcludingAgent=*/0));

				// THE CONSUMER, not just the table: this is the question a player pressing 7
				// asks between ticks, and the only one that matters.
				const FArrivalPlan Landing = ArrivalPlanner::Plan(*Cross, FVector2D(-51000.0, 0.0),
					M2TrafficPiper(), &Air->GetOccupancy());
				TestEqual(TEXT("and a landing offered in that same frame is refused: RunwayOccupied"),
					Landing.Why, EArrivalRefusal::RunwayOccupied);

				// AND IT KEEPS IT AFTER IT PARKS, WHICH IS THE FRAME THAT MATTERED.
				//
				// An invalid plan makes FRouteFollower::HasArrived true, so this agent is
				// Parked by the end of the very tick that stranded it, and from the next tick
				// on it takes ClaimAhead's non-Taxiing branch. That branch used to hold
				// RunwayHeld and nothing else - and RunwayHeld is empty on an agent that
				// never landed - so the strip it is physically standing on came free one tick
				// after the fix above kept it. Holding for one tick and then letting go is not
				// holding: the player's next press of 7 is not on that frame.
				M2TrafficRun(*Air, *Cross, 3.0, [](int32) { return true; });
				const FRoadAgent* Parked = Air->FindAgent(Plane);
				if (TestNotNull(TEXT("it is still there three seconds later"), Parked))
				{
					UE_LOG(LogM2TrafficTest, Log,
						TEXT("DeadPlanReleases parked measured: phase %s, crossing phase %d, strip %s"),
						*UEnum::GetValueAsString(Parked->Phase), static_cast<int32>(Parked->CrossingPhase),
						Air->GetOccupancy().IsHeld(Strip, 0) ? TEXT("HELD") : TEXT("free"));

					TestEqual(TEXT("and it has parked where it stood"), Parked->Phase, EAgentPhase::Parked);
					TestTrue(TEXT("a PARKED aircraft still holds the strip its body is on"),
						Air->GetOccupancy().IsHeld(Strip, /*ExcludingAgent=*/0));

					const FArrivalPlan Later = ArrivalPlanner::Plan(*Cross, FVector2D(-51000.0, 0.0),
						M2TrafficPiper(), &Air->GetOccupancy());
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
	FVector2D Threshold(0.0, 0.0);
	const FAirframe Piper = M2TrafficPiper();
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	{
		const double Needed = FLandingRun::RequiredLandingDistance(
			Piper.Ground, Piper.Climb, Piper.Approach) * FLandingRun::LandingMargin;
		const FVector2D ExitAt(Needed * 1.2, 0.0);
		const FVector2D FarAt(Needed * 3.0, 0.0);
		URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
		const FRoadNodeId ThresholdNode = Net->AddNode(Threshold);
		const FRoadNodeId ExitNode = Net->AddNode(ExitAt);
		const FRoadNodeId FarNode = Net->AddNode(FarAt);
		Net->AddStraightSegment(ThresholdNode, ExitNode, Runway);
		Net->AddStraightSegment(ExitNode, FarNode, Runway);
		const FRoadNodeId TaxiEnd = Net->AddNode(ExitAt + FVector2D(0.0, -24000.0));
		Net->AddStraightSegment(ExitNode, TaxiEnd, Taxiway);
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		FRoadGuidelineBuilder::Build(*Net, Solved);
		// Two stands off the one taxiway, so the second arrival has somewhere to go while
		// the first is parked.
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Net->PlaceEntity(Stand, Stand->Anchors, ExitAt + FVector2D(9000.0, -12000.0), 0.0);
		Net->PlaceEntity(Stand, Stand->Anchors, ExitAt + FVector2D(9000.0, -18000.0), 0.0);
		FAnchorLink::Build(*Net);
	}

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FVector2D Approach = Threshold - FVector2D(1000.0, 0.0);

	const int32 First = Traffic->DispatchArrival(*Net, Approach, Piper, 1.0);
	if (!TestTrue(TEXT("first arrival admitted"), First > 0)) { return false; }
	M2TrafficRun(*Traffic, *Net, 400.0, [&](int32)
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
	M2TrafficRun(*Traffic, *Net, 200.0, [&](int32)
	{
		const FRoadAgent* B = Traffic->FindAgent(Second);
		return B != nullptr && B->Phase != EAgentPhase::Taxiing;
	});
	{
		const FRoadAgent* B = Traffic->FindAgent(Second);
		if (!TestTrue(TEXT("second arrival is taxiing in"), B != nullptr && B->Phase == EAgentPhase::Taxiing)) { return false; }
	}

	const EDepartureRefusal Why = Traffic->DepartAgent(First, *Net);
	if (!TestTrue(FString::Printf(TEXT("departure accepted (%d)"), static_cast<int32>(Why)), Why == EDepartureRefusal::None)) { return false; }

	double MinSeparation = TNumericLimits<double>::Max();
	int32 TicksBothTaxiing = 0;
	int32 TicksFirstWaited = 0;
	int32 TicksSecondWaited = 0;
	M2TrafficRun(*Traffic, *Net, 300.0, [&](int32)
	{
		const FRoadAgent* A = Traffic->FindAgent(First);
		const FRoadAgent* B = Traffic->FindAgent(Second);
		if (A == nullptr || B == nullptr) { return false; }
		if (A->Phase != EAgentPhase::Taxiing || B->Phase != EAgentPhase::Taxiing) { return B->Phase == EAgentPhase::Taxiing || A->Phase == EAgentPhase::Taxiing; }
		++TicksBothTaxiing;
		TicksFirstWaited += A->WaitingOn != 0 ? 1 : 0;
		TicksSecondWaited += B->WaitingOn != 0 ? 1 : 0;
		MinSeparation = FMath::Min(MinSeparation, FVector2D::Distance(A->LastMotion.Position, B->LastMotion.Position));
		return true;
	});

	UE_LOG(LogM2TrafficTest, Log,
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
	const FGuidelineNodeId N30 = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId N31 = M2TrafficNode(*Net, 0.0, -392.0);
	const FGuidelineNodeId NN = M2TrafficNode(*Net, 0.0, 8000.0);
	const FGuidelineNodeId NR = M2TrafficNode(*Net, 1900.0, 0.0);
	const FGuidelineNodeId E1 = M2TrafficNode(*Net, R, -392.0 - R);
	const FGuidelineNodeId W1 = M2TrafficNode(*Net, -R, -392.0 - R);
	const FGuidelineNodeId E = M2TrafficNode(*Net, R + 8000.0, -392.0 - R);
	const FGuidelineNodeId W = M2TrafficNode(*Net, -R - 8000.0, -392.0 - R);
	const FGuidelineEdgeId Stub = M2TrafficJoin(*Net, N30, N31);
	M2TrafficJoin(*Net, N30, NN);
	M2TrafficJoin(*Net, N30, NR);
	M2TrafficJoin(*Net, E1, E);
	M2TrafficJoin(*Net, W1, W);
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
	const int32 First = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, W, NN, ETraversalClass::Aircraft), M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("first dispatched"), First > 0)) { return false; }
	const int32 FirstSteps = Traffic->FindAgent(First)->Follower.Plan.Steps.Num();
	bool bStaged = false;
	M2TrafficRun(*Traffic, *Net, 60.0, [&](int32)
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
	const int32 Second = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, NR, E, ETraversalClass::Aircraft), M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("second dispatched"), Second > 0)) { return false; }
	const int32 SecondSteps = Traffic->FindAgent(Second)->Follower.Plan.Steps.Num();

	bool bCycleFormed = false;
	int32 CycleTick = -1;
	M2TrafficRun(*Traffic, *Net, 200.0, [&](int32 Tick)
	{
		const FRoadAgent* A = Traffic->FindAgent(First);
		const FRoadAgent* B = Traffic->FindAgent(Second);
		if (A == nullptr || B == nullptr) { return false; }
		if (!bCycleFormed && A->WaitingOn == Second && B->WaitingOn == First)
		{
			bCycleFormed = true;
			CycleTick = Tick;
		}
		return A->Phase != EAgentPhase::Parked || B->Phase != EAgentPhase::Parked;
	});

	const FRoadAgent* A = Traffic->FindAgent(First);
	const FRoadAgent* B = Traffic->FindAgent(Second);
	UE_LOG(LogM2TrafficTest, Log,
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
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	const FRoadNodeId RoadR1 = Net->AddNode(FVector2D(0.0, -1500.0));
	const FRoadNodeId RoadR2 = Net->AddNode(FVector2D(4000.0, -1500.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(RoadR1, RoadR2, Runway);

	const FGuidelineNodeId W = M2TrafficNode(*Net, -5000.0, 0.0);
	const FGuidelineNodeId A = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = M2TrafficNode(*Net, 4000.0, 0.0);
	const FGuidelineNodeId E = M2TrafficNode(*Net, 9000.0, 0.0);
	const FGuidelineNodeId R1 = M2TrafficNode(*Net, 0.0, -1500.0);
	const FGuidelineNodeId R2 = M2TrafficNode(*Net, 4000.0, -1500.0);
	M2TrafficJoin(*Net, W, A);
	const FGuidelineEdgeId AB = M2TrafficJoin(*Net, A, B);
	M2TrafficJoin(*Net, B, E);
	M2TrafficJoin(*Net, A, R1);
	M2TrafficJoin(*Net, R2, B);
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
		const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, W, E, ETraversalClass::Aircraft), M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
		if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }
		const FRoadAgent* Before = Traffic->FindAgent(Plane);
		TestTrue(TEXT("the cleared route is the taxiway, three steps, no strip"), Before->Follower.Plan.Steps.Num() == 3 && !UsesStrip(Before->Follower.Plan));

		const bool bReplanned = Traffic->ReplanAtForTest(Plane, *Net, /*SpliceStep=*/1, AB);
		const FRoadAgent* After = Traffic->FindAgent(Plane);
		UE_LOG(LogM2TrafficTest, Log, TEXT("ReplanTurnsOverFreeRunwayEnd measured: free strip - replanned %d, %d step(s), uses strip %d, %.0f uu"),
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
		const int32 Plane = Traffic->DispatchAgent(Net, M2TrafficRoute(*Net, W, E, ETraversalClass::Aircraft), M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
		if (!TestTrue(TEXT("dispatched"), Plane > 0)) { return false; }
		const bool bReplanned = Traffic->ReplanAtForTest(Plane, *Net, /*SpliceStep=*/1, AB);
		const FRoadAgent* After = Traffic->FindAgent(Plane);
		UE_LOG(LogM2TrafficTest, Log, TEXT("ReplanTurnsOverFreeRunwayEnd measured: held strip - replanned %d, %d step(s), uses strip %d"),
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
	const FGuidelineNodeId W = M2TrafficNode(*Net, -20000.0, 0.0);
	const FGuidelineNodeId J = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId N = M2TrafficNode(*Net, 0.0, 20000.0);
	M2TrafficJoin(*Net, W, J);
	M2TrafficJoin(*Net, J, N);

	// One network for all three: it is read-only while agents advance, and sharing it removes
	// any chance the runs differ because their geometry did.
	int32 Ids[3] = { 0, 0, 0 };
	UGroundTraffic* Runs[3] = { nullptr, nullptr, nullptr };
	const double Longest[3] = { 1.0 / 30.0, 1.0 / 30.0, 1000.0 };
	for (int32 Which = 0; Which < 3; ++Which)
	{
		Runs[Which] = NewObject<UGroundTraffic>(GetTransientPackage());
		Runs[Which]->MaxSubstepSeconds = Longest[Which];
		Ids[Which] = Runs[Which]->DispatchAgent(Net,
			M2TrafficRoute(*Net, W, N, ETraversalClass::Aircraft),
			M2TrafficPlane(), ETraversalClass::Aircraft, 1.0);
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
	UE_LOG(LogM2TrafficTest, Log,
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
	const FGuidelineNodeId W = M2TrafficNode(*Net, -20000.0, 0.0);
	const FGuidelineNodeId J = M2TrafficNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId N = M2TrafficNode(*Net, 0.0, 20000.0);
	M2TrafficJoin(*Net, W, J);
	M2TrafficJoin(*Net, J, N);

	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Airframe = M2TrafficPlane();

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
		Cold->EngineRPM), Cold->EngineRPM < AtSpeed);

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
		Warm->EngineRPM, AtSpeed), FMath::IsNearlyEqual(Warm->EngineRPM, AtSpeed, 0.01));
	TestTrue(TEXT("and running"), Warm->bEngineRunning);
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
	const FAirframe Airframe = M2TrafficPiper();
	FVector2D Threshold;
	URoadNetwork* Net = M2TrafficArrivalAirport(Airframe, Threshold);

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

#endif
