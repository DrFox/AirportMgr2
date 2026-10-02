#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "AirsideTestFixtures.h"
#include "Model/Airframe.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/PassingOrder.h"
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

	/**
	 * Whether Plan's passes book into a COPY of Table - the table it was planned against. A plan the planner says fits and
	 * the table refuses is a plan nobody can fly (review of #527: search and booking computed one bound in two float
	 * orders and could disagree on a boundary). Every successful plan in these tests is asked this.
	 */
	bool BooksInto(const FTaxiReservations& Table, const FTaxiPlan& Plan)
	{
		FTaxiReservations Copy = Table;
		return Plan.IsPlanned() && Copy.BookPasses(Plan.Passes);
	}

	/** Whether any step drives straight back along the edge the step before it drove - a U-turn on a centreline. */
	bool HasUTurn(const FTaxiPlan& Plan)
	{
		for (int32 Index = 1; Index < Plan.Route.Steps.Num(); ++Index)
		{
			if (Plan.Route.Steps[Index].Edge == Plan.Route.Steps[Index - 1].Edge)
			{
				return true;
			}
		}
		return false;
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
	TestTrue(TEXT("and it books"), BooksInto(Empty, Plan));
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
	TestTrue(TEXT("and it books"), BooksInto(Empty, Plan));
	// TWO BOUNDS, NOT ONE TOLERANCE, because the two errors are not alike. OPTIMISTIC is the dangerous one - a clock
	// that runs fast books windows the aircraft will overrun - and the per-piece split can only be optimistic by the
	// braking it skips across a smooth edge boundary: held to 5%. PESSIMISTIC is the price of timing an instant
	// corner as a full stop where the follower crawls through at its steering floor: this route has two such corners
	// (68 degrees at B and at C), measured 2026-10-02 at +11% (44.2 s against 39.8 s); held to 15%. Real junctions are
	// tangent-continuous (FRoadGuidelineBuilder's turn paths), so the pessimistic case is a hand-drawn line's.
	TestTrue(TEXT("the authority timed the route"), Authority > 0.0);
	TestTrue(FString::Printf(TEXT("the planner's clock is not optimistic by more than 5%% (%.2f s vs %.2f s)"),
		Plan.Arrival, Authority), Plan.Arrival >= 0.95 * Authority);
	TestTrue(FString::Printf(TEXT("nor pessimistic by more than 15%% (%.2f s vs %.2f s)"),
		Plan.Arrival, Authority), Plan.Arrival <= 1.15 * Authority);

	// And on a SMOOTH route - every joint tangent - the two agree closely: what is left is cross-boundary braking.
	URoadNetwork* Smooth = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId P = Smooth->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId Q = Smooth->AddGuidelineNode(FVector2D(8000.0, 0.0));
	const FGuidelineNodeId R = Smooth->AddGuidelineNode(FVector2D(16000.0, 0.0));
	const FGuidelineNodeId T = Smooth->AddGuidelineNode(FVector2D(24000.0, 0.0));
	Join(*Smooth, P, Q);
	Join(*Smooth, Q, R);
	Join(*Smooth, R, T);
	FTaxiPlanner SmoothPlanner(*Smooth, Empty, Piper, Rules);
	const FTaxiPlan Straight = SmoothPlanner.Plan(Request(P, T));
	FSpeedProfile StraightWhole;
	StraightWhole.Build(Straight.Route.Polyline, Piper.Chassis);
	const double StraightAuthority = StraightWhole.SecondsToDrive(0.0);
	TestTrue(FString::Printf(TEXT("a smooth route's clock is within 1%% of the authority's (%.2f s vs %.2f s)"),
		Straight.Arrival, StraightAuthority), Straight.IsPlanned()
		&& FMath::Abs(Straight.Arrival - StraightAuthority) <= 0.01 * StraightAuthority);
	TestTrue(TEXT("and the smooth one books"), BooksInto(Empty, Straight));
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
		TestTrue(TEXT("and it books"), BooksInto(Table, Free));
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
	TestTrue(TEXT("and it books"), BooksInto(Table, Plan));
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
		TestTrue(TEXT("and it books"), BooksInto(Empty, Plan));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanLaterIntervalTest, "Airside.Model.TaxiPlan.LaterIntervalSuccessor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanLaterIntervalTest::RunTest(const FString& Parameters)
{
	//   S ===== A ===== B ===== G      every node a plain lane node - the aircraft may wait at any of them
	// Another aircraft passes B at [100, 150) and holds B-G until 300. Reaching B EARLY (~40 s) is a dead end: B must
	// be left by 95 and B-G is shut till 300. Waiting at A and reaching B in its LATER free interval [150, inf) works.
	// Textbook SIPP makes one successor per reachable safe interval of the destination (review of #527): a planner
	// that tries only the earliest departure per move never generates that later arrival and refuses.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId S = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(5000.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(10000.0, 0.0));
	const FGuidelineNodeId G = Net->AddGuidelineNode(FVector2D(15000.0, 0.0));
	Join(*Net, S, A);
	Join(*Net, A, B);
	const FGuidelineEdgeId BG = Join(*Net, B, G);

	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;
	FTaxiReservations Table;
	Table.BookWindow(FTaxiResource::Node(B), { TheirId, 100.0, 150.0 });
	Table.BookWindow(FTaxiResource::Edge(BG), { TheirId, 0.0, 300.0 });

	FTaxiPlanner Planner(*Net, Table, Piper, Rules);
	const FTaxiPlan Plan = Planner.Plan(Request(S, G));
	TestEqual(TEXT("planned - via B's later free interval"), Plan.Result, ETaxiPlanResult::Planned);
	TestTrue(FString::Printf(TEXT("arriving after B-G frees at 300 s (%.1f s)"), Plan.Arrival), Plan.Arrival > 300.0);
	TestFalse(TEXT("without driving back on itself"), HasUTurn(Plan));
	TestTrue(TEXT("and it books"), BooksInto(Table, Plan));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanNoUTurnTest, "Airside.Model.TaxiPlan.NoUTurnOnArrivingEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanNoUTurnTest::RunTest(const FString& Parameters)
{
	//   S ===== A ===== B ===== G
	// B is someone else's at [60, 100) and B-G until 200. An aircraft that reached B early must leave it by 55 with the
	// way ahead shut: the edge filter admits A-B reversed from B, so "back to A, wait, come again" - a 180 on a
	// centreline, booking A-B twice in overlapping windows (review of #527) - was a move the search could make.
	// Waiting at A (or S) and going once is the plan.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId S = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(5000.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(10000.0, 0.0));
	const FGuidelineNodeId G = Net->AddGuidelineNode(FVector2D(15000.0, 0.0));
	Join(*Net, S, A);
	Join(*Net, A, B);
	const FGuidelineEdgeId BG = Join(*Net, B, G);

	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;
	FTaxiReservations Table;
	Table.BookWindow(FTaxiResource::Node(B), { TheirId, 60.0, 100.0 });
	Table.BookWindow(FTaxiResource::Edge(BG), { TheirId, 0.0, 200.0 });

	FTaxiPlanner Planner(*Net, Table, Piper, Rules);
	const FTaxiPlan Plan = Planner.Plan(Request(S, G));
	TestTrue(TEXT("planned"), Plan.IsPlanned());
	TestFalse(TEXT("never back along the edge it arrived on"), HasUTurn(Plan));
	TestEqual(TEXT("S, A, B, G - each edge once"), Plan.Route.Steps.Num(), 3);
	TestTrue(TEXT("and it books"), BooksInto(Table, Plan));

	// WHERE THE U-TURN IS THE ONLY WAY, there is no plan. Timed off the unobstructed plan's own legs (A reached at a, B
	// at b): S must be left at once, A is somebody's from just after a, B from 30 s after b and B-G until long after.
	// So the aircraft reaches B with nowhere to wait and the way ahead shut; only "back to A (free again a moment
	// later), wait, come again" fits the table - and an aircraft cannot turn on a centreline. Without the ban the
	// search returns exactly that plan (the first version of this test passed with the ban removed; this half did not).
	// On LONG lanes (20 km apart), so the way back to A takes far longer than A's brief busy spot plus the margins.
	URoadNetwork* Long = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId S2 = Long->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId A2 = Long->AddGuidelineNode(FVector2D(20000.0, 0.0));
	const FGuidelineNodeId B2 = Long->AddGuidelineNode(FVector2D(40000.0, 0.0));
	const FGuidelineNodeId G2 = Long->AddGuidelineNode(FVector2D(60000.0, 0.0));
	Join(*Long, S2, A2);
	Join(*Long, A2, B2);
	const FGuidelineEdgeId B2G2 = Join(*Long, B2, G2);
	FTaxiReservations Empty;
	FTaxiPlanner Clear(*Long, Empty, Piper, Rules);
	const FTaxiPlan Free = Clear.Plan(Request(S2, G2));
	if (!TestTrue(TEXT("unobstructed, it plans S-A-B-G"), Free.IsPlanned() && Free.Legs.Num() == 3))
	{
		return false;
	}
	const double M = Rules.TaxiPlanMargin;
	const double ReachA = Free.Legs[0].Reach;
	const double ReachB = Free.Legs[1].Reach;
	FTaxiReservations Trap;
	Trap.BookWindow(FTaxiResource::Node(S2), { TheirId, M + 0.5, FTaxiReservations::Forever });
	Trap.BookWindow(FTaxiResource::Node(A2), { TheirId, ReachA + M + 1.0, ReachA + M + 2.0 });
	// B's busy spot starts 30 s after reaching it - time enough to STOP there and turn, so the U-turn is a move the
	// search can make (a rolling 180 is a sharp joint and never rolls), but not to wait out B-G.
	Trap.BookWindow(FTaxiResource::Node(B2), { TheirId, ReachB + 30.0, ReachB + 100.0 });
	Trap.BookWindow(FTaxiResource::Edge(B2G2), { TheirId, 0.0, ReachB + 300.0 });
	FTaxiPlanner Trapped(*Long, Trap, Piper, Rules);
	const FTaxiPlan Turned = Trapped.Plan(Request(S2, G2));
	TestFalse(TEXT("no plan turns back on the edge it arrived on"), HasUTurn(Turned));
	TestEqual(TEXT("so with the U-turn the only fit, it is refused"), Turned.Result, ETaxiPlanResult::NoFreeWindow);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanTouchingBoundaryTest, "Airside.Model.TaxiPlan.TouchingBoundaryFractionalReach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanTouchingBoundaryTest::RunTest(const FString& Parameters)
{
	// THE SEARCH AND THE BOOKING MUST AGREE TO THE LAST BIT (review of #527). Another aircraft holds the middle node B
	// until a fractional instant; the earliest plan reaches B just as that window closes, so its own window TOUCHES the
	// other's - and a bound the search summed as Tau + (Reach - M) and the booking as (Tau + Reach) - M can land one
	// ulp inside it: a plan Plan() calls fitting that BookPasses refuses. Swept over many fractional boundaries and
	// departures; one of them hitting the rounding is enough to fail.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId S = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(5123.4567, 0.0));
	const FGuidelineNodeId G = Net->AddGuidelineNode(FVector2D(10987.654, 0.0));
	Join(*Net, S, B);
	Join(*Net, B, G);

	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;
	int32 Planned = 0;
	int32 Booked = 0;
	constexpr int32 Sweeps = 200;
	for (int32 Index = 0; Index < Sweeps; ++Index)
	{
		const double Boundary = 3.0 + Index * 0.731 + 0.0001234 * Index * Index;
		FTaxiReservations Table;
		Table.BookWindow(FTaxiResource::Node(B), { TheirId, 0.0, Boundary });
		FTaxiPlanner Planner(*Net, Table, Piper, Rules);
		const FTaxiPlan Plan = Planner.Plan(Request(S, G, 0.1 * Index));
		Planned += Plan.IsPlanned() ? 1 : 0;
		Booked += BooksInto(Table, Plan) ? 1 : 0;
	}
	TestEqual(TEXT("every sweep planned"), Planned, Sweeps);
	TestEqual(TEXT("and every plan the planner called fitting books"), Booked, Planned);
	return true;
}

// ---- PR 2: one-way FIFO sharing, the push prefix, the passing order ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanShareOneWayTest, "Airside.Model.TaxiPlan.ReservationsShareOneWayInOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanShareOneWayTest::RunTest(const FString& Parameters)
{
	// PR 2's correction to PR 1: an edge was one aircraft's at a time, so nobody could follow anybody down a taxiway.
	// Now two windows may overlap when they go THE SAME WAY and in FIFO order - entry order is exit order, a headway
	// apart at both ends. Opposite ways (the head-on) and Any (nodes, pushes) never share.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(40000.0, 0.0));
	const FTaxiResource R = FTaxiResource::Edge(Join(*Net, A, B));
	const FTaxiResource N = FTaxiResource::Node(A);

	FTaxiReservations Table;
	Table.SetHeadway(5.0);
	TestTrue(TEXT("a window going A to B books"), Table.BookWindow(R, { 1, 0.0, 30.0, ETaxiWay::AToB }));
	TestTrue(TEXT("a follower the same way, in and out 5 s or more later, books ALONGSIDE it"),
		Table.BookWindow(R, { 2, 6.0, 40.0, ETaxiWay::AToB }));
	TestFalse(TEXT("one that would OVERTAKE (in later, out earlier) is refused"), Table.BookWindow(R, { 3, 12.0, 32.0, ETaxiWay::AToB }));
	TestFalse(TEXT("one closer than the headway behind is refused"), Table.BookWindow(R, { 3, 8.0, 50.0, ETaxiWay::AToB }));
	TestFalse(TEXT("the OTHER way, overlapping, is refused - the head-on the table exists for"),
		Table.BookWindow(R, { 3, 20.0, 60.0, ETaxiWay::BToA }));
	TestFalse(TEXT("Any, overlapping, is refused"), Table.BookWindow(R, { 3, 20.0, 60.0, ETaxiWay::Any }));
	TestTrue(TEXT("the other way once both have left books"), Table.BookWindow(R, { 3, 40.0, 60.0, ETaxiWay::BToA }));
	TestTrue(TEXT("a node holds one aircraft at a time"), Table.BookWindow(N, { 1, 0.0, 10.0 }));
	TestFalse(TEXT("so a second overlapping it is refused"), Table.BookWindow(N, { 2, 5.0, 15.0 }));

	// The planner's queries.
	FTaxiReservations One;
	One.SetHeadway(5.0);
	One.BookWindow(R, { 1, 0.0, 30.0, ETaxiWay::AToB });
	double Shift = -1.0;
	TestTrue(TEXT("EarliestFit: the same way fits"), One.EarliestFit(R, ETaxiWay::AToB, 2.0, 20.0, 0, Shift));
	TestEqual(TEXT("as a follower - in 5 s after, and out 5 s after: shifted 15"), Shift, 15.0);
	TestTrue(TEXT("EarliestFit: the other way fits"), One.EarliestFit(R, ETaxiWay::BToA, 2.0, 20.0, 0, Shift));
	TestEqual(TEXT("only once it has left: shifted 28"), Shift, 28.0);
	TestTrue(TEXT("EarliestFit: the holder's own window is ignored"), One.EarliestFit(R, ETaxiWay::BToA, 2.0, 20.0, 1, Shift) && Shift == 0.0);
	FTaxiReservations Parked;
	Parked.BookWindow(N, { 1, 0.0, FTaxiReservations::Forever });
	TestFalse(TEXT("EarliestFit: held for ever - no shift will do"), Parked.EarliestFit(N, ETaxiWay::Any, 2.0, 20.0, 0, Shift));

	FTaxiReservations Ahead;
	Ahead.SetHeadway(5.0);
	Ahead.BookWindow(R, { 2, 50.0, 80.0, ETaxiWay::AToB });
	Ahead.BookWindow(R, { 3, 100.0, 120.0, ETaxiWay::BToA });
	TestEqual(TEXT("LatestEnd: going A to B, out a headway before the follower behind leaves"),
		Ahead.LatestEnd(R, ETaxiWay::AToB, 0.0, 0), 75.0);
	TestEqual(TEXT("LatestEnd: going B to A, out before the next one in"), Ahead.LatestEnd(R, ETaxiWay::BToA, 0.0, 0), 50.0);
	TestEqual(TEXT("PlaceAt: one window starts before 60"), Ahead.PlaceAt(R, 60.0, 0), 1);
	TestEqual(TEXT("NextPlaceAfter: the same way, a headway behind the next one's entry"),
		Ahead.NextPlaceAfter(R, ETaxiWay::AToB, 0.0, 0), 55.0);
	TestEqual(TEXT("NextPlaceAfter: the other way, once it has left"), Ahead.NextPlaceAfter(R, ETaxiWay::BToA, 0.0, 0), 80.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanFollowersTest, "Airside.Model.TaxiPlan.FollowersShareOneEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanFollowersTest::RunTest(const FString& Parameters)
{
	//   S1 ====\                    /==== G1
	//           X ================ Y
	//   S2 ====/                    \==== G2
	// Two aircraft set off together and share X-Y, 40 km of it, one behind the other. They meet at X (a node: one at a
	// time) - but along X-Y the second must FOLLOW, not wait for the first to have left: an edge one aircraft's at a
	// time is the throughput PR 1 lost.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId S1 = Net->AddGuidelineNode(FVector2D(-10000.0, 3000.0));
	const FGuidelineNodeId S2 = Net->AddGuidelineNode(FVector2D(-10000.0, -3000.0));
	const FGuidelineNodeId X = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId Y = Net->AddGuidelineNode(FVector2D(40000.0, 0.0));
	const FGuidelineNodeId G1 = Net->AddGuidelineNode(FVector2D(50000.0, 3000.0));
	const FGuidelineNodeId G2 = Net->AddGuidelineNode(FVector2D(50000.0, -3000.0));
	Join(*Net, S1, X);
	Join(*Net, S2, X);
	const FGuidelineEdgeId XY = Join(*Net, X, Y);
	Join(*Net, Y, G1);
	Join(*Net, Y, G2);

	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;
	FTaxiReservations Table;
	Table.SetHeadway(Rules.TaxiPlanMargin);

	FTaxiPlanner First(*Net, Table, Piper, Rules);
	const FTaxiPlan Leader = First.Plan(Request(S1, G1, 0.0, OurId));
	TestTrue(TEXT("the leader plans and books"), Leader.IsPlanned() && Table.BookPasses(Leader.Passes));

	FTaxiPlanner Second(*Net, Table, Piper, Rules);
	const FTaxiPlan Follower = Second.Plan(Request(S2, G2, 0.0, TheirId));
	TestTrue(TEXT("the follower plans"), Follower.IsPlanned());
	TestTrue(TEXT("and books alongside"), BooksInto(Table, Follower));

	const TConstArrayView<FTaxiWindow> OnXY = Table.WindowsOn(FTaxiResource::Edge(XY));
	double LeaderOut = 0.0;
	for (const FTaxiWindow& Window : OnXY)
	{
		LeaderOut = Window.Holder == OurId ? Window.To : LeaderOut;
	}
	double FollowerIn = FTaxiReservations::Forever;
	for (const FTaxiPass& Pass : Follower.Passes)
	{
		FollowerIn = Pass.Resource == FTaxiResource::Edge(XY) ? Pass.Window.From : FollowerIn;
	}
	TestTrue(FString::Printf(TEXT("the follower is on X-Y (from %.1f s) before the leader has left it (%.1f s)"), FollowerIn, LeaderOut),
		FollowerIn < LeaderOut);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanHeadOnTest, "Airside.Model.TaxiPlan.OppositeDirectionWaits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanHeadOnTest::RunTest(const FString& Parameters)
{
	//   A0 ==\                  /== B0
	//         A ============== B
	//   A1 ==/                  \== B1
	// One aircraft A0 -> B0, another B1 -> A1: head-on along A-B, the 2026-10-02 jam. The second may not be on A-B
	// while the first is - it waits (at its start, where a plan may wait) until the first has gone.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A0 = Net->AddGuidelineNode(FVector2D(-10000.0, 3000.0));
	const FGuidelineNodeId A1 = Net->AddGuidelineNode(FVector2D(-10000.0, -3000.0));
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(20000.0, 0.0));
	const FGuidelineNodeId B0 = Net->AddGuidelineNode(FVector2D(30000.0, 3000.0));
	const FGuidelineNodeId B1 = Net->AddGuidelineNode(FVector2D(30000.0, -3000.0));
	Join(*Net, A0, A);
	Join(*Net, A1, A);
	const FGuidelineEdgeId AB = Join(*Net, A, B);
	Join(*Net, B, B0);
	Join(*Net, B, B1);

	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;
	FTaxiReservations Table;
	Table.SetHeadway(Rules.TaxiPlanMargin);
	FTaxiPlanner First(*Net, Table, Piper, Rules);
	const FTaxiPlan East = First.Plan(Request(A0, B0, 0.0, OurId));
	TestTrue(TEXT("east-bound plans and books"), East.IsPlanned() && Table.BookPasses(East.Passes));

	FTaxiPlanner Second(*Net, Table, Piper, Rules);
	const FTaxiPlan West = Second.Plan(Request(B1, A1, 0.0, TheirId));
	TestTrue(TEXT("west-bound plans"), West.IsPlanned());
	TestTrue(TEXT("it waits for the other to clear A-B"), HoldsAt(West, B1));
	TestTrue(TEXT("and books - never overlapping the head-on"), BooksInto(Table, West));
	double EastOut = 0.0;
	for (const FTaxiWindow& Window : Table.WindowsOn(FTaxiResource::Edge(AB)))
	{
		EastOut = FMath::Max(EastOut, Window.To);
	}
	for (const FTaxiPass& Pass : West.Passes)
	{
		if (Pass.Resource == FTaxiResource::Edge(AB))
		{
			TestTrue(FString::Printf(TEXT("its A-B window (from %.1f s) starts once the other's has ended (%.1f s)"),
				Pass.Window.From, EastOut), Pass.Window.From >= EastOut);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanPushPrefixTest, "Airside.Model.TaxiPlan.PushPrefixHoldsAtStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanPushPrefixTest::RunTest(const FString& Parameters)
{
	//   P (stand) ---- J ==== E      the push: P to J, back onto the arm J-E
	//                  ||
	//                  R             the taxi out: E, J, R
	// The push is the plan's first move, booked whole (both ways) for its duration. Someone holds J until 100 s, so the
	// push itself must wait - ON THE STAND, the departure's hold - never half-way onto the taxiway.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId P = Net->AddGuidelineNode(FVector2D(0.0, -8000.0));
	const FGuidelineNodeId J = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId E = Net->AddGuidelineNode(FVector2D(15000.0, 0.0));
	const FGuidelineNodeId R = Net->AddGuidelineNode(FVector2D(-30000.0, 0.0));
	const FGuidelineEdgeId PJ = Join(*Net, P, J);
	const FGuidelineEdgeId JE = Join(*Net, J, E);
	Join(*Net, J, R);

	const FAirframe Piper = TestAirframes::Piper();
	const FTrafficRules Rules;
	FTaxiReservations Table;
	Table.SetHeadway(Rules.TaxiPlanMargin);
	Table.BookWindow(FTaxiResource::Node(J), { TheirId, 0.0, 100.0 });

	FTaxiRequest Push = Request(P, R);
	FRouteStep Lead;
	Lead.Edge = PJ;
	Lead.To = J;
	FRouteStep Back;
	Back.Edge = JE;
	Back.To = E;
	Push.PushSteps = { Lead, Back };
	Push.PushSeconds = 30.0;

	FTaxiPlanner Planner(*Net, Table, Piper, Rules);
	const FTaxiPlan Plan = Planner.Plan(Push);
	TestTrue(TEXT("planned"), Plan.IsPlanned());
	TestTrue(FString::Printf(TEXT("the push waits for J (pushes at %.1f s)"), Plan.PushAt),
		Plan.PushAt >= 100.0 + Rules.TaxiPlanMargin);
	TestTrue(TEXT("on the stand"), HoldsAt(Plan, P));
	TestTrue(TEXT("the taxi route starts where the push ends"),
		Plan.Route.Steps.Num() > 0 && Plan.Route.Steps[0].Edge == JE && Plan.Route.Steps.Last().To == R);
	TestTrue(TEXT("moves are named"), Plan.MoveStarts.Num() > 0 && Plan.MoveStarts[0] == 0);
	bool bPushGroundAny = false;
	for (const FTaxiPass& Pass : Plan.Passes)
	{
		bPushGroundAny |= Pass.Resource == FTaxiResource::Edge(PJ) && Pass.Window.Way == ETaxiWay::Any
			&& Pass.Window.From <= Plan.PushAt;
	}
	TestTrue(TEXT("the push ground is booked both ways from the push"), bPushGroundAny);
	TestTrue(TEXT("and the plan books"), BooksInto(Table, Plan));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanOrderTest, "Airside.Model.TaxiPlan.OrderWaitsForWhoIsAhead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanOrderTest::RunTest(const FString& Parameters)
{
	// FPassingOrder: an aircraft may enter a resource when everyone booked through it AHEAD of it has left - or, the
	// same way along an edge, has entered (they may follow each other down it).
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(40000.0, 0.0));
	const FTaxiResource E = FTaxiResource::Edge(Join(*Net, A, B));
	const FTaxiResource N = FTaxiResource::Node(A);

	FTaxiReservations Table;
	Table.SetHeadway(5.0);
	Table.BookWindow(N, { 1, 0.0, 10.0 });
	Table.BookWindow(N, { 2, 20.0, 30.0 });
	TArray<int32> Entered;
	auto HasEntered = [&Entered](int32 Other) { return Entered.Contains(Other); };

	TestEqual(TEXT("the first through a node waits for nobody"), FPassingOrder::WaitingFor(Table, 1, N, HasEntered), 0);
	TestEqual(TEXT("the second waits for the first"), FPassingOrder::WaitingFor(Table, 2, N, HasEntered), 1);
	Entered.Add(1);
	TestEqual(TEXT("entering a NODE is not leaving it - still waits"), FPassingOrder::WaitingFor(Table, 2, N, HasEntered), 1);
	FTaxiReservations Left = Table;
	Left.ReleaseHolderOn(N, 1);
	TestEqual(TEXT("once the first's window is released (its tail cleared), the second goes"),
		FPassingOrder::WaitingFor(Left, 2, N, HasEntered), 0);
	TestEqual(TEXT("an aircraft with no window there is not this order's to hold"), FPassingOrder::WaitingFor(Table, 3, N, HasEntered), 0);

	Entered.Reset();
	Table.BookWindow(E, { 1, 0.0, 30.0, ETaxiWay::AToB });
	Table.BookWindow(E, { 2, 6.0, 40.0, ETaxiWay::AToB });
	Table.BookWindow(E, { 3, 50.0, 60.0, ETaxiWay::BToA });
	TestEqual(TEXT("a follower the same way waits for its leader to ENTER"), FPassingOrder::WaitingFor(Table, 2, E, HasEntered), 1);
	Entered.Add(1);
	TestEqual(TEXT("and no longer once it has"), FPassingOrder::WaitingFor(Table, 2, E, HasEntered), 0);
	Entered.Add(2);
	TestEqual(TEXT("the other way waits for them to LEAVE, entered or not"), FPassingOrder::WaitingFor(Table, 3, E, HasEntered), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanOnePassTest, "Airside.Model.TaxiPlan.ReleasesOnePassAtATime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanOnePassTest::RunTest(const FString& Parameters)
{
	// A ROUTE MAY PASS ONE RESOURCE TWICE - a turnaround loop at a dead end, out along an edge and back. The tail clearing
	// it the first time releases THAT pass only; the way back is still booked, and still ordered. Releasing every window
	// of the holder on the first pass (ReleaseHolderOn) let a departure back onto the edge with no window - unordered -
	// ahead of the one booked before it (measured on M_ScaleGatwick at 80 mov/h, 2026-10-02: a jam of a dozen aircraft).
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(40000.0, 0.0));
	const FTaxiResource E = FTaxiResource::Edge(Join(*Net, A, B));

	FTaxiReservations Table;
	Table.BookWindow(E, { 1, 0.0, 20.0, ETaxiWay::AToB });   // out
	Table.BookWindow(E, { 2, 30.0, 50.0, ETaxiWay::AToB });  // another, out
	Table.BookWindow(E, { 1, 60.0, 80.0, ETaxiWay::BToA });  // the first, back
	auto Never = [](int32) { return false; };

	TestTrue(TEXT("the first pass released"), Table.ReleaseFirstOn(E, 1));
	TestEqual(TEXT("and only it: the way back is still booked"), Table.WindowsOn(E).Num(), 2);
	TestEqual(TEXT("so on its way back it waits for the one booked between"), FPassingOrder::WaitingFor(Table, 1, E, Never), 2);
	TestTrue(TEXT("which passes"), Table.ReleaseFirstOn(E, 2));
	TestEqual(TEXT("and then it is its turn"), FPassingOrder::WaitingFor(Table, 1, E, Never), 0);
	TestFalse(TEXT("nothing of a holder with no window to release"), Table.ReleaseFirstOn(E, 3));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
