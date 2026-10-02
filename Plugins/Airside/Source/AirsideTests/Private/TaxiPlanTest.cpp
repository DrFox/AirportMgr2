#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "AirsideTestFixtures.h"
#include "Model/Airframe.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/SpeedProfile.h"
#include "Model/TaxiPlanner.h"
#include "Model/TaxiReservations.h"
#include "Model/TrafficRules.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

// Spec 2026-10-02 (space-time taxi planning) §4, the "reservations" and "planner" bullets. World-free: every test
// NewObjects a network and builds its own guideline graph, so what is measured is the planner, not a layout.

namespace
{
	/** Who else is on the airport in these tests. The planned aircraft is always holder 1. */
	constexpr int32 OurId = 1;
	constexpr int32 TheirId = 2;

	/**
	 * One guideline admitting everything. Lateral bends the edge by moving its control point off the chord's middle,
	 * sideways, so a "parallel" really is a different, longer line. bTurnPath marks it a junction turn path the way
	 * FRoadGuidelineBuilder does - AtJunction set - which is what the planner's hold rule reads.
	 */
	FGuidelineEdgeId Join(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B,
		EGuidelineDir Direction = EGuidelineDir::Bidirectional, double MaxWingspan = 0.0, double Lateral = 0.0,
		bool bTurnPath = false)
	{
		const FVector2D PA = Net.GetGuidelineNode(A)->Position;
		const FVector2D PB = Net.GetGuidelineNode(B)->Position;
		const FVector2D Along = (PB - PA).GetSafeNormal();
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = (PA + PB) * 0.5 + FVector2D(-Along.Y, Along.X) * Lateral;
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = Direction;
		Edge.MaxWingspan = MaxWingspan;
		if (bTurnPath)
		{
			// Any set road node: the planner asks only WHETHER the piece was laid at a junction.
			Edge.AtJunction.Index = 0;
		}
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	FTaxiRequest Request(FGuidelineNodeId Start, FGuidelineNodeId Goal, double DepartAt = 0.0, int32 Holder = OurId)
	{
		FTaxiRequest Out;
		Out.Start = Start;
		Out.Goal = Goal;
		Out.Errand = ERouteErrand::ArrivalTaxiIn;
		Out.Holder = Holder;
		Out.DepartAt = DepartAt;
		return Out;
	}

	bool HoldsAt(const FTaxiPlan& Plan, FGuidelineNodeId Node)
	{
		return Plan.Holds.ContainsByPredicate([Node](const FTaxiHold& Hold) { return Hold.At == Node; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanReservationsBookTest, "Airside.Model.TaxiPlan.ReservationsBookAndRelease",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanReservationsBookTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(5000.0, 0.0));
	const FGuidelineEdgeId AB = Join(*Net, A, B);

	FTaxiReservations Table;
	const FTaxiResource R = FTaxiResource::Node(A);

	TestTrue(TEXT("a window books on an empty resource"), Table.BookWindow(R, { OurId, 10.0, 20.0 }));
	TestTrue(TEXT("a TOUCHING window books - [10,20) and [20,30) share no instant"), Table.BookWindow(R, { TheirId, 20.0, 30.0 }));
	TestFalse(TEXT("an OVERLAPPING window is refused, whoever holds it"), Table.BookWindow(R, { 3, 15.0, 25.0 }));
	TestFalse(TEXT("the holder's OWN overlap is refused too - two windows of one resource with no order"),
		Table.BookWindow(R, { OurId, 12.0, 14.0 }));
	TestEqual(TEXT("and the refusals changed nothing"), Table.WindowsOn(R).Num(), 2);
	TestFalse(TEXT("a window with no length is refused"), Table.BookWindow(R, { 3, 40.0, 40.0 }));
	TestTrue(TEXT("booked out of order"), Table.BookWindow(R, { 3, 0.0, 5.0 }));
	TestTrue(TEXT("windows read back sorted by From"),
		Table.WindowsOn(R).Num() == 3 && Table.WindowsOn(R)[0].Holder == 3 && Table.WindowsOn(R)[2].Holder == TheirId);

	// An edge and a node are different resources even when their slot indices agree - A and AB are both slot 0.
	TestTrue(TEXT("the edge with the node's index is a different resource"),
		Table.BookWindow(FTaxiResource::Edge(AB), { TheirId, 10.0, 20.0 }));
	TestTrue(TEXT("the edge resource has no direction: one key both ways"), FTaxiResource::Edge(AB) == FTaxiResource::Edge(AB));

	// ALL OR NOTHING: a plan half-booked is one other aircraft wait on and this one can never fly.
	const FTaxiResource RB = FTaxiResource::Node(B);
	const TArray<FTaxiPass> Clashing = { { RB, { 4, 0.0, 10.0 } }, { R, { 4, 25.0, 26.0 } } };
	TestFalse(TEXT("passes where one overlaps are refused"), Table.BookPasses(Clashing));
	TestEqual(TEXT("and NONE of them landed"), Table.WindowsOn(RB).Num(), 0);
	const TArray<FTaxiPass> SelfClash = { { RB, { 4, 0.0, 10.0 } }, { RB, { 4, 5.0, 15.0 } } };
	TestFalse(TEXT("passes that overlap EACH OTHER are refused"), Table.BookPasses(SelfClash));
	const TArray<FTaxiPass> Fine = { { RB, { 4, 0.0, 10.0 } }, { R, { 4, 30.0, 31.0 } } };
	TestTrue(TEXT("passes that fit all book"), Table.BookPasses(Fine));
	TestEqual(TEXT("both landed"), Table.WindowsOn(RB).Num() + Table.WindowsOn(R).Num(), 1 + 4);

	TestEqual(TEXT("ReleaseHolderOn takes only that resource's windows"), Table.ReleaseHolderOn(R, TheirId), 1);
	TestEqual(TEXT("the holder's edge window survives it"), Table.WindowsOn(FTaxiResource::Edge(AB)).Num(), 1);
	TestEqual(TEXT("ReleaseHolder takes the rest of its windows"), Table.ReleaseHolder(TheirId), 1);
	TestEqual(TEXT("ReleaseHolder takes every resource"), Table.ReleaseHolder(4), 2);
	TestEqual(TEXT("nothing of 4 is left"), Table.WindowsOn(RB).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanReservationsFreeTest, "Airside.Model.TaxiPlan.ReservationsFreeIntervals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanReservationsFreeTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FTaxiResource R = FTaxiResource::Node(Net->AddGuidelineNode(FVector2D::ZeroVector));

	FTaxiReservations Table;
	TArray<FTaxiInterval> Free;
	Table.FreeIntervals(R, 0, Free);
	TestTrue(TEXT("nobody holds it: free always"), Free.Num() == 1
		&& Free[0].Start == FTaxiReservations::Always && Free[0].End == FTaxiReservations::Forever);

	Table.BookWindow(R, { OurId, 10.0, 20.0 });
	Table.BookWindow(R, { TheirId, 30.0, 40.0 });
	Table.FreeIntervals(R, 0, Free);
	TestEqual(TEXT("two windows leave three gaps"), Free.Num(), 3);
	if (Free.Num() == 3)
	{
		TestEqual(TEXT("before the first"), Free[0].End, 10.0);
		TestTrue(TEXT("between"), Free[1].Start == 20.0 && Free[1].End == 30.0);
		TestTrue(TEXT("after the last, for ever"), Free[2].Start == 40.0 && Free[2].End == FTaxiReservations::Forever);
	}

	Table.BookWindow(R, { 3, 20.0, 30.0 });
	Table.FreeIntervals(R, 0, Free);
	TestEqual(TEXT("TOUCHING windows leave no zero-length gap between them"), Free.Num(), 2);

	Table.FreeIntervals(R, 3, Free);
	TestEqual(TEXT("the holder's OWN windows read as free - a re-plan is not blocked by the plan it replaces"),
		Free.Num(), 3);

	TestFalse(TEXT("IsFree: held by 3"), Table.IsFree(R, 20.0, 30.0, 0));
	TestTrue(TEXT("IsFree: unless 3 is asking"), Table.IsFree(R, 20.0, 30.0, 3));
	TestTrue(TEXT("IsFree: a touching span is free"), Table.IsFree(R, 40.0, 50.0, 0));
	TestFalse(TEXT("IsFree: an overlapping span is not"), Table.IsFree(R, 39.0, 50.0, 0));

	// Open-ended: an aircraft parked on a stand holds it for ever.
	FTaxiReservations Parked;
	Parked.BookWindow(R, { TheirId, 50.0, FTaxiReservations::Forever });
	Parked.FreeIntervals(R, 0, Free);
	TestTrue(TEXT("held for ever from 50: one gap, before it"), Free.Num() == 1 && Free[0].End == 50.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanPieceSecondsTest, "Airside.Model.TaxiPlan.PieceSeconds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanPieceSecondsTest::RunTest(const FString& Parameters)
{
	// A straight line long enough to reach the taxi cap and brake from it, so the closed forms below hold:
	// accelerating from rest takes v/a over v^2/2a, braking v/d over v^2/2d, the rest is cruise at v.
	const FChassis Chassis = TestAirframes::Piper().Chassis;
	const double V = Chassis.Ground.Taxi.SpeedCap;
	const double A = Chassis.Ground.Taxi.Accel;
	const double D = Chassis.Ground.Taxi.Decel;
	const double L = 4.0 * (V * V / (2.0 * A) + V * V / (2.0 * D));
	const TArray<FVector2D> Line = { FVector2D::ZeroVector, FVector2D(L, 0.0) };

	FSpeedProfile Rolls;
	Rolls.BuildPiece(Line, Chassis, EPieceEnd::Rolls);
	FSpeedProfile Stops;
	Stops.BuildPiece(Line, Chassis, EPieceEnd::Stops);

	const double Cruise = Rolls.SecondsToDrive(Rolls.LimitAt(0.0));
	const double FromRest = Rolls.SecondsToDrive(0.0);
	const double RestToRest = Stops.SecondsToDrive(0.0);

	const double ExpectCruise = L / V;
	const double ExpectFromRest = V / A + (L - V * V / (2.0 * A)) / V;
	const double ExpectRestToRest = V / A + V / D + (L - V * V / (2.0 * A) - V * V / (2.0 * D)) / V;
	TestTrue(FString::Printf(TEXT("rolling in at the cap and out: L/v (%.3f s, expected %.3f)"), Cruise, ExpectCruise),
		FMath::IsNearlyEqual(Cruise, ExpectCruise, ExpectCruise * 0.01));
	TestTrue(FString::Printf(TEXT("from rest, rolling out: v/a + cruise (%.3f s, expected %.3f)"), FromRest, ExpectFromRest),
		FMath::IsNearlyEqual(FromRest, ExpectFromRest, ExpectFromRest * 0.01));
	TestTrue(FString::Printf(TEXT("rest to rest: both ramps (%.3f s, expected %.3f)"), RestToRest, ExpectRestToRest),
		FMath::IsNearlyEqual(RestToRest, ExpectRestToRest, ExpectRestToRest * 0.01));
	TestTrue(TEXT("the last vertex of a ROLLING piece keeps its limit"), Rolls.LimitAt(L) > 0.0);
	TestEqual(TEXT("the last vertex of a STOPPING piece is a stop"), Stops.LimitAt(L), 0.0);

	// ONE RULE, TWO ENTRY POINTS: a whole route built by Build is a piece that stops.
	FSpeedProfile Route;
	Route.Build(Line, Chassis);
	TestEqual(TEXT("Build and BuildPiece(Stops) time the same line the same"), Route.SecondsToDrive(0.0), RestToRest);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanEmptyNetworkTest, "Airside.Model.TaxiPlan.EmptyNetworkIsShortestRouteTime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanEmptyNetworkTest::RunTest(const FString& Parameters)
{
	// RouteSearchTest's diamond, at taxiway scale: the north side is the long way round.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId West = Net->AddGuidelineNode(FVector2D(-5000.0, 0.0));
	const FGuidelineNodeId East = Net->AddGuidelineNode(FVector2D(5000.0, 0.0));
	const FGuidelineNodeId North = Net->AddGuidelineNode(FVector2D(0.0, 20000.0));
	const FGuidelineNodeId South = Net->AddGuidelineNode(FVector2D(0.0, -1000.0));
	Join(*Net, West, North);
	Join(*Net, North, East);
	Join(*Net, West, South);
	Join(*Net, South, East);

	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;
	const FTaxiReservations Empty;
	FTaxiPlanner Planner(*Net, Empty, Piper, Rules);

	const double DepartAt = 100.0;
	const FTaxiPlan Plan = Planner.Plan(Request(West, East, DepartAt));
	const FRoutePlan Shortest = TestGraph::Probe(*Net, West, East, ETraversalClass::Aircraft, nullptr, Piper.Wingspan);

	TestTrue(TEXT("planned"), Plan.IsPlanned());
	TestTrue(TEXT("RouteSearch finds the same pair joined"), Shortest.IsValid());
	TestEqual(TEXT("the same number of steps as the shortest route"), Plan.Route.Steps.Num(), Shortest.Steps.Num());
	if (Plan.Route.Steps.Num() == 2 && Shortest.Steps.Num() == 2)
	{
		TestEqual(TEXT("via the same node - the short side"), Plan.Route.Steps[0].To, Shortest.Steps[0].To);
	}
	const double Expected = DepartAt + Planner.RouteSeconds(Shortest);
	TestTrue(FString::Printf(TEXT("earliest arrival on an empty airport is the shortest route's time (%.3f vs %.3f)"),
		Plan.Arrival, Expected), Plan.IsPlanned() && FMath::IsNearlyEqual(Plan.Arrival, Expected, 1e-6));
	TestEqual(TEXT("nothing to wait for"), Plan.Holds.Num(), 0);
	TestEqual(TEXT("one leg per route step"), Plan.Legs.Num(), Plan.Route.Steps.Num());
	TestTrue(TEXT("the route is drivable - welded by RouteSearch"), Plan.Route.IsDrivable());
	TestTrue(TEXT("the route is the polyline RouteSearch would give"),
		Plan.Route.Polyline.Num() == Shortest.Polyline.Num() && FMath::IsNearlyEqual(Plan.Route.Length, Shortest.Length, 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanWholeRouteTest, "Airside.Model.TaxiPlan.EtaAgreesWithWholeRouteProfile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanWholeRouteTest::RunTest(const FString& Parameters)
{
	// The planner times each edge as its own piece; the follower drives one profile over the whole route. The two
	// differ only in braking for a bend across an edge boundary - this measures that the difference is small, by
	// asking the AUTHORITY over the whole route (memory: a test that re-implements FSpeedProfile per edge ships green).
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(8000.0, 0.0));
	const FGuidelineNodeId C = Net->AddGuidelineNode(FVector2D(12000.0, 4000.0));
	const FGuidelineNodeId D = Net->AddGuidelineNode(FVector2D(12000.0, 12000.0));
	Join(*Net, A, B);
	Join(*Net, B, C, EGuidelineDir::Bidirectional, 0.0, 1200.0);
	Join(*Net, C, D);

	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;
	const FTaxiReservations Empty;
	FTaxiPlanner Planner(*Net, Empty, Piper, Rules);
	const FTaxiPlan Plan = Planner.Plan(Request(A, D));

	FSpeedProfile Whole;
	Whole.Build(Plan.Route.Polyline, Piper.Chassis);
	const double Authority = Whole.SecondsToDrive(0.0);
	TestTrue(TEXT("planned"), Plan.IsPlanned());
	TestTrue(FString::Printf(TEXT("the planner's clock is within 10%% of the whole-route profile (%.2f s vs %.2f s)"),
		Plan.Arrival, Authority), Authority > 0.0 && FMath::Abs(Plan.Arrival - Authority) <= 0.1 * Authority);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanWaitsTest, "Airside.Model.TaxiPlan.WaitsAtPlainNodeNotInJunction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanWaitsTest::RunTest(const FString& Parameters)
{
	//   S ===lane=== P --turn-- J1 --turn-- J2 ===lane=== G
	// Another aircraft holds S from 30 s (it is coming onto that stand), the junction's first piece P-J1 from 60 to
	// 200 s, and J2-G until 100 s. We must leave S early, cannot be through the junction before 60, and so must wait
	// for 200. The only legal place is P: J1 and J2 are inside the junction. A planner that let us wait inside it
	// would slip into the junction at once and wait at J2 for 100 s instead - arriving ~100 s earlier, which is what
	// the Arrival assertion catches.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId S = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId P = Net->AddGuidelineNode(FVector2D(5000.0, 0.0));
	const FGuidelineNodeId J1 = Net->AddGuidelineNode(FVector2D(5400.0, 0.0));
	const FGuidelineNodeId J2 = Net->AddGuidelineNode(FVector2D(5800.0, 0.0));
	const FGuidelineNodeId G = Net->AddGuidelineNode(FVector2D(10800.0, 0.0));
	const FGuidelineNodeId Stub = Net->AddGuidelineNode(FVector2D(5000.0, 1000.0));
	const FGuidelineEdgeId SP = Join(*Net, S, P);
	const FGuidelineEdgeId PJ1 = Join(*Net, P, J1, EGuidelineDir::Bidirectional, 0.0, 0.0, /*bTurnPath*/ true);
	const FGuidelineEdgeId J1J2 = Join(*Net, J1, J2, EGuidelineDir::Bidirectional, 0.0, 0.0, /*bTurnPath*/ true);
	const FGuidelineEdgeId J2G = Join(*Net, J2, G);
	const FGuidelineEdgeId Short = Join(*Net, P, Stub);   // a lane, but shorter than an aircraft and its gap

	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;

	// The hold rule on its own: the box is the claim pass's (IsBox), the turn path the builder's (AtJunction).
	TestTrue(TEXT("CanHoldAt: P, reached along a long lane"), FTaxiPlanner::CanHoldAt(*Net, Rules, SP, P));
	TestFalse(TEXT("CanHoldAt: not J1 - reached along a turn path"), FTaxiPlanner::CanHoldAt(*Net, Rules, PJ1, J1));
	TestFalse(TEXT("CanHoldAt: not J2"), FTaxiPlanner::CanHoldAt(*Net, Rules, J1J2, J2));
	TestTrue(TEXT("the stub really is shorter than a body and its gap"), Rules.IsBox(1000.0, ETraversalClass::Aircraft));
	TestFalse(TEXT("CanHoldAt: not at the end of a lane too short to stand on (a box)"),
		FTaxiPlanner::CanHoldAt(*Net, Rules, Short, Stub));

	FTaxiReservations Table;
	Table.BookWindow(FTaxiResource::Node(S), { TheirId, 30.0, FTaxiReservations::Forever });
	Table.BookWindow(FTaxiResource::Edge(PJ1), { TheirId, 60.0, 200.0 });
	Table.BookWindow(FTaxiResource::Edge(J2G), { TheirId, 0.0, 100.0 });

	FTaxiPlanner Planner(*Net, Table, Piper, Rules);
	const FTaxiPlan Plan = Planner.Plan(Request(S, G));
	TestTrue(TEXT("planned"), Plan.IsPlanned());
	TestTrue(TEXT("it waits at P, the plain node before the junction"), HoldsAt(Plan, P));
	TestFalse(TEXT("never at J1, inside the junction"), HoldsAt(Plan, J1));
	TestFalse(TEXT("never at J2, inside the junction"), HoldsAt(Plan, J2));
	TestTrue(FString::Printf(TEXT("so it cannot be through before the junction frees at 200 s (arrives %.1f s)"), Plan.Arrival),
		Plan.Arrival > 200.0);
	TestTrue(TEXT("and its windows fit round the other aircraft's"), Table.BookPasses(Plan.Passes));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanParallelTest, "Airside.Model.TaxiPlan.TakesOtherParallel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanParallelTest::RunTest(const FString& Parameters)
{
	// Two lines X to Y: straight, and bowed - longer. Two edges, so two resources. The straight one is held for
	// 100 s; waiting for it at X costs more than the extra length of the other.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId X = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId Y = Net->AddGuidelineNode(FVector2D(10000.0, 0.0));
	const FGuidelineEdgeId Straight = Join(*Net, X, Y);
	const FGuidelineEdgeId Bowed = Join(*Net, X, Y, EGuidelineDir::Bidirectional, 0.0, 3000.0);

	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;
	FTaxiReservations Table;

	{
		FTaxiPlanner Planner(*Net, Table, Piper, Rules);
		const FTaxiPlan Free = Planner.Plan(Request(X, Y));
		TestTrue(TEXT("unobstructed, it takes the straight line"),
			Free.IsPlanned() && Free.Route.Steps.Num() == 1 && Free.Route.Steps[0].Edge == Straight);
	}

	Table.BookWindow(FTaxiResource::Edge(Straight), { TheirId, 0.0, 100.0 });
	FTaxiPlanner Planner(*Net, Table, Piper, Rules);
	const FTaxiPlan Plan = Planner.Plan(Request(X, Y));
	TestTrue(TEXT("planned"), Plan.IsPlanned());
	TestTrue(TEXT("it takes the other parallel rather than waiting"),
		Plan.Route.Steps.Num() == 1 && Plan.Route.Steps[0].Edge == Bowed);
	TestTrue(FString::Printf(TEXT("and arrives before the straight one would even free (%.1f s)"), Plan.Arrival),
		Plan.IsPlanned() && Plan.Arrival < 100.0);
	TestEqual(TEXT("no wait"), Plan.Holds.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanRefusesTest, "Airside.Model.TaxiPlan.RefusesWhenNothingFree",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanRefusesTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(5000.0, 0.0));
	Join(*Net, A, B);

	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;

	{
		FTaxiReservations Table;
		Table.BookWindow(FTaxiResource::Node(B), { TheirId, 0.0, FTaxiReservations::Forever });
		FTaxiPlanner Planner(*Net, Table, Piper, Rules);
		const FTaxiPlan Plan = Planner.Plan(Request(A, B));
		TestEqual(TEXT("goal held for ever: no free window"), Plan.Result, ETaxiPlanResult::NoFreeWindow);
		TestEqual(TEXT("though the layout has a route - this is traffic, not the layout"), Plan.RouteResult, ERouteResult::Found);
		TestEqual(TEXT("and nothing to book"), Plan.Passes.Num(), 0);
	}
	{
		// Someone is due at the stand at 500 s: we could get there first, but could not STAY - and a plan that ends
		// where the aircraft cannot stay is the one order enforcement cannot keep deadlock-free (spec §2).
		FTaxiReservations Table;
		Table.BookWindow(FTaxiResource::Node(B), { TheirId, 500.0, FTaxiReservations::Forever });
		FTaxiPlanner Planner(*Net, Table, Piper, Rules);
		TestEqual(TEXT("goal due to someone LATER: refused, it could not stay"),
			Planner.Plan(Request(A, B)).Result, ETaxiPlanResult::NoFreeWindow);
	}
	{
		FTaxiReservations Table;
		Table.BookWindow(FTaxiResource::Node(A), { TheirId, 0.0, 1000.0 });
		FTaxiPlanner Planner(*Net, Table, Piper, Rules);
		TestEqual(TEXT("start held by somebody else when we would leave: refused, not crashed"),
			Planner.Plan(Request(A, B, 10.0)).Result, ETaxiPlanResult::NoFreeWindow);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanRulesTest, "Airside.Model.TaxiPlan.RespectsOneWayAndSize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanRulesTest::RunTest(const FString& Parameters)
{
	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;
	const FTaxiReservations Empty;

	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(5000.0, 0.0));
		Join(*Net, A, B, EGuidelineDir::BToA);
		FTaxiPlanner Planner(*Net, Empty, Piper, Rules);
		const FTaxiPlan Plan = Planner.Plan(Request(A, B));
		TestEqual(TEXT("one-way against us: no route"), Plan.Result, ETaxiPlanResult::NoRoute);
		TestEqual(TEXT("and RouteSearch says why"), Plan.RouteResult, ERouteResult::Unreachable);
	}
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(5000.0, 0.0));
		Join(*Net, A, B, EGuidelineDir::Bidirectional, Piper.Wingspan * 0.5);
		FTaxiPlanner Planner(*Net, Empty, Piper, Rules);
		const FTaxiPlan Plan = Planner.Plan(Request(A, B));
		TestEqual(TEXT("too narrow for the wings: no route"), Plan.Result, ETaxiPlanResult::NoRoute);
		TestEqual(TEXT("and RouteSearch says too wide"), Plan.RouteResult, ERouteResult::TooWide);
	}
	{
		// The rules steer the SEARCH, not just the refusal: the short side is one-way against us and the plan
		// goes the long way round.
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId West = Net->AddGuidelineNode(FVector2D(-5000.0, 0.0));
		const FGuidelineNodeId East = Net->AddGuidelineNode(FVector2D(5000.0, 0.0));
		const FGuidelineNodeId North = Net->AddGuidelineNode(FVector2D(0.0, 20000.0));
		const FGuidelineNodeId South = Net->AddGuidelineNode(FVector2D(0.0, -1000.0));
		Join(*Net, West, North);
		Join(*Net, North, East);
		Join(*Net, West, South, EGuidelineDir::BToA);
		Join(*Net, South, East);
		FTaxiPlanner Planner(*Net, Empty, Piper, Rules);
		const FTaxiPlan Plan = Planner.Plan(Request(West, East));
		TestTrue(TEXT("planned the long way"), Plan.IsPlanned() && Plan.Route.Steps.Num() == 2
			&& Plan.Route.Steps[0].To == North);
	}
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(5000.0, 0.0));
		Join(*Net, A, B);
		FTaxiPlanner Planner(*Net, Empty, Piper, Rules);
		FTaxiRequest NoErrand = Request(A, B);
		NoErrand.Errand = ERouteErrand::Unset;
		// RouteSearch's own refusal, reused - logged once, as Find would.
		AddExpectedMessage(TEXT("has no errand"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, 1);
		TestEqual(TEXT("no errand: refused as RouteSearch refuses it"), Planner.Plan(NoErrand).Result, ETaxiPlanResult::NoRoute);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanBooksTest, "Airside.Model.TaxiPlan.PlanBooksCleanly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanBooksTest::RunTest(const FString& Parameters)
{
	//        D
	//        |
	//   A === B === C
	//        |
	//        E
	// We go A to C; another aircraft goes D to E across our path at the same moment, so one of us waits.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(-5000.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId C = Net->AddGuidelineNode(FVector2D(5000.0, 0.0));
	const FGuidelineNodeId D = Net->AddGuidelineNode(FVector2D(0.0, 5000.0));
	const FGuidelineNodeId E = Net->AddGuidelineNode(FVector2D(0.0, -5000.0));
	Join(*Net, A, B);
	Join(*Net, B, C);
	Join(*Net, D, B);
	Join(*Net, B, E);

	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;
	FTaxiReservations Table;
	FTaxiPlanner Planner(*Net, Table, Piper, Rules);

	const FTaxiPlan Ours = Planner.Plan(Request(A, C));
	TestTrue(TEXT("ours planned"), Ours.IsPlanned());
	TestTrue(TEXT("ours books"), Table.BookPasses(Ours.Passes));
	TestTrue(TEXT("every pass is ours"), !Ours.Passes.ContainsByPredicate([](const FTaxiPass& Pass) { return Pass.Window.Holder != OurId; }));
	TestTrue(TEXT("the goal is held for ever - we stay there"), Table.WindowsOn(FTaxiResource::Node(C)).Num() == 1
		&& Table.WindowsOn(FTaxiResource::Node(C))[0].To == FTaxiReservations::Forever);

	// A RE-PLAN by the same holder is not blocked by the plan it is replacing.
	const FTaxiPlan Again = Planner.Plan(Request(A, C));
	TestTrue(TEXT("re-planned over our own windows, same arrival"),
		Again.IsPlanned() && FMath::IsNearlyEqual(Again.Arrival, Ours.Arrival, 1e-6));

	const FTaxiPlan Theirs = Planner.Plan(Request(D, E, 0.0, TheirId));
	TestTrue(TEXT("theirs planned"), Theirs.IsPlanned());
	TestTrue(TEXT("theirs waits - we are through B first"), Theirs.Holds.Num() > 0);
	TestTrue(TEXT("and books round ours, holds and all"), Table.BookPasses(Theirs.Passes));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
