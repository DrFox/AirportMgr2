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
	return true;
}

#endif
