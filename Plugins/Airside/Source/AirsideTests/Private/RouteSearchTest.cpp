#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteFollower.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A bidirectional guideline admitting everything, straight unless a control is given. */
	FGuidelineEdgeId Join(
		URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B,
		EGuidelineDir Direction = EGuidelineDir::Bidirectional, double MaxWingspan = 0.0)
	{
		const FGuidelineNode* NodeA = Net.GetGuidelineNode(A);
		const FGuidelineNode* NodeB = Net.GetGuidelineNode(B);

		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = (NodeA->Position + NodeB->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = Direction;
		Edge.MaxWingspan = MaxWingspan;
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

// Named as a child of RouteSearch, not the bare category name: Task 1 added
// Airside.Model.RouteSearch.{EndDistance,Splice} as siblings, and UE's automation report
// tree cannot have "RouteSearch" be both a leaf test and the parent of those - the leaf
// silently stops being run at all, with no error, the moment a child is registered under
// the same full name. Task 7 adds OccupancyCost as another sibling, so this needed a
// specific name regardless of the collision: it exercises Find and FindNearestNode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteSearchTest,
	"Airside.Model.RouteSearch.Find",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteSearchTest::RunTest(const FString& Parameters)
{
	// A diamond. The top way round is deliberately the LONG one, so a search that merely
	// finds *a* route rather than the shortest is caught: both have two edges, so an
	// edge-counting search would be free to pick either and would pass a test that only
	// asserted a route exists.
	//
	//        North (0, 4000)
	//       /                \
	//   West (-1000,0)    East (1000,0)
	//       \                /
	//        South (0, -100)
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	const FGuidelineNodeId West = Net->AddGuidelineNode(FVector2D(-1000.0, 0.0));
	const FGuidelineNodeId East = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
	const FGuidelineNodeId North = Net->AddGuidelineNode(FVector2D(0.0, 4000.0));
	const FGuidelineNodeId South = Net->AddGuidelineNode(FVector2D(0.0, -100.0));

	Join(*Net, West, North);
	Join(*Net, North, East);
	const FGuidelineEdgeId WestSouth = Join(*Net, West, South);
	Join(*Net, South, East);

	FRouteQuery Query;
	Query.Errand = ERouteErrand::GraphProbe;
	Query.Policy = FRoutePolicy::For(Query.Errand);
	Query.Start = West;
	Query.Goal = East;
	Query.Class = ETraversalClass::Aircraft;

	{
		const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
		TestTrue(TEXT("a route is found across the diamond"), Plan.IsValid());
		TestEqual(TEXT("it takes two edges"), Plan.Steps.Num(), 2);

		// The SHORT way, which is the whole point of the heuristic being admissible.
		if (Plan.Steps.Num() == 2)
		{
			TestEqual(TEXT("it goes via south, the shorter side"), Plan.Steps[0].To, South);
		}

		// Geometry, not just topology: the drawn line must actually start and end where
		// the query did, or the route is right and the picture is wrong - which is this
		// project's most expensive category of bug.
		TestTrue(TEXT("the polyline has points"), Plan.Polyline.Num() >= 2);
		TestEqual(TEXT("it starts at the start node"), Plan.Polyline[0], FVector2D(-1000.0, 0.0));
		TestEqual(TEXT("it ends at the goal node"), Plan.Polyline.Last(), FVector2D(1000.0, 0.0));

		// The weld: consecutive edges must contribute ONE shared point, so a route of two
		// straight edges is exactly three points. A duplicate would leave a zero-length
		// step for the follower to walk and would not show up in any length assertion.
		TestEqual(TEXT("two straight edges weld to three points"), Plan.Polyline.Num(), 3);

		TestTrue(TEXT("the length is the polyline's own"),
			FMath::IsNearlyEqual(Plan.Length, GuidelineGeom::PolylineLength(Plan.Polyline), 1e-6));
	}

	// One-way, against the traveller. The edge is still there and still admits aircraft;
	// only its direction refuses - so this catches a search that filtered traffic but
	// forgot direction.
	{
		URoadNetwork* OneWay = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId A = OneWay->AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId B = OneWay->AddGuidelineNode(FVector2D(1000.0, 0.0));
		Join(*OneWay, A, B, EGuidelineDir::AToB);

		FRouteQuery Forward;
		Forward.Errand = ERouteErrand::GraphProbe;
		Forward.Policy = FRoutePolicy::For(Forward.Errand);
		Forward.Start = A;
		Forward.Goal = B;
		Forward.Class = ETraversalClass::Aircraft;
		TestTrue(TEXT("the one-way runs forwards"), RouteSearch::Find(*OneWay, Forward).IsValid());

		FRouteQuery Back;
		Back.Errand = ERouteErrand::GraphProbe;
		Back.Policy = FRoutePolicy::For(Back.Errand);
		Back.Start = B;
		Back.Goal = A;
		Back.Class = ETraversalClass::Aircraft;
		const FRoutePlan Refused = RouteSearch::Find(*OneWay, Back);
		TestFalse(TEXT("and refuses backwards"), Refused.IsValid());
		TestEqual(TEXT("reported as unreachable"), Refused.Result, ERouteResult::Unreachable);
	}

	// Wingspan. The distinction that matters: TooWide, not Unreachable - the taxiways ARE
	// joined up and the aircraft is the problem, and telling those apart is the difference
	// between "connect your airport" and "send a smaller aeroplane".
	{
		URoadNetwork* Narrow = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId A = Narrow->AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId B = Narrow->AddGuidelineNode(FVector2D(1000.0, 0.0));
		Join(*Narrow, A, B, EGuidelineDir::Bidirectional, /*MaxWingspan=*/3600.0);

		FRouteQuery Fits;
		Fits.Errand = ERouteErrand::GraphProbe;
		Fits.Policy = FRoutePolicy::For(Fits.Errand);
		Fits.Start = A;
		Fits.Goal = B;
		Fits.Class = ETraversalClass::Aircraft;
		Fits.Wingspan = 3600.0;
		TestTrue(TEXT("a wingspan equal to the limit fits"), RouteSearch::Find(*Narrow, Fits).IsValid());

		FRouteQuery TooBig = Fits;
		TooBig.Wingspan = 6500.0;
		const FRoutePlan Refused = RouteSearch::Find(*Narrow, TooBig);
		TestFalse(TEXT("a widebody does not"), Refused.IsValid());
		TestEqual(TEXT("and is told why"), Refused.Result, ERouteResult::TooWide);
	}

	// A guideline that admits nobody - the documented default of FTrafficMask - must be a
	// visible dead end rather than a silent free-for-all.
	{
		URoadNetwork* Closed = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId A = Closed->AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId B = Closed->AddGuidelineNode(FVector2D(1000.0, 0.0));

		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = FVector2D(500.0, 0.0);
		Closed->AddGuidelineEdge(MoveTemp(Edge));

		FRouteQuery Query2;
		Query2.Errand = ERouteErrand::GraphProbe;
		Query2.Policy = FRoutePolicy::For(Query2.Errand);
		Query2.Start = A;
		Query2.Goal = B;
		Query2.Class = ETraversalClass::Aircraft;
		TestFalse(TEXT("a mask admitting nobody carries nobody"),
			RouteSearch::Find(*Closed, Query2).IsValid());
	}

	// Typed refusals, so the tool can say something better than "no".
	{
		FGuidelineNodeId Dead;
		Dead.Index = 999;
		Dead.Generation = 1;

		FRouteQuery Bad;
		Bad.Errand = ERouteErrand::GraphProbe;
		Bad.Policy = FRoutePolicy::For(Bad.Errand);
		Bad.Start = Dead;
		Bad.Goal = East;
		TestEqual(TEXT("a dead start is NoStart"),
			RouteSearch::Find(*Net, Bad).Result, ERouteResult::NoStart);

		Bad.Start = West;
		Bad.Goal = Dead;
		TestEqual(TEXT("a dead goal is NoGoal"),
			RouteSearch::Find(*Net, Bad).Result, ERouteResult::NoGoal);

		Bad.Goal = West;
		TestEqual(TEXT("start equal to goal is SameNode"),
			RouteSearch::Find(*Net, Bad).Result, ERouteResult::SameNode);
	}

	// Nearest-node picking, which is how the tool turns a click into a query. A node no
	// edge admits this class on must not be offered: snapping to it and failing afterwards
	// reads as a broken pathfinder rather than as a node that was never usable.
	{
		const FGuidelineNodeId Found =
			RouteSearch::FindNearestNode(*Net, FVector2D(-1050.0, 30.0), ETraversalClass::Aircraft, 200.0);
		TestEqual(TEXT("the nearest usable node is west"), Found, West);

		const FGuidelineNodeId Distant =
			RouteSearch::FindNearestNode(*Net, FVector2D(-9000.0, 0.0), ETraversalClass::Aircraft, 200.0);
		TestFalse(TEXT("nothing is offered beyond the radius"), Distant.IsSet());

		URoadNetwork* Lonely = NewObject<URoadNetwork>(GetTransientPackage());
		Lonely->AddGuidelineNode(FVector2D(0.0, 0.0));
		TestFalse(TEXT("a node with no edges is not offered"),
			RouteSearch::FindNearestNode(*Lonely, FVector2D(0.0, 0.0), ETraversalClass::Aircraft, 500.0).IsSet());
	}

	// The follower walks the very array that was drawn. Asserted by walking it to the end
	// and landing on the goal - if cost and motion ever used different geometry, an agent
	// would stop short of, or overshoot, the line it was shown.
	{
		FRoutePlan Plan = RouteSearch::Find(*Net, Query);

		FRouteFollower Follower;
		// Every limit wide open: this asks whether the follower walks the SAME GEOMETRY the
		// search costed, and an agent slowing for corners or winding up from rest would turn
		// a clean "arrives in N steps" into a question about turn rates and throttle. Those
		// are measured in Airside.Model.TurnRate.
		FAirframe Airframe;
		Airframe.Chassis.Ground.MaxTurnRateDegPerSec = 1.0e6;
		Airframe.Chassis.Ground.Taxi.Accel = 1.0e9;
		Airframe.Chassis.Ground.Taxi.Decel = 1.0e9;
		Follower.Start(Plan, Airframe.Chassis);

		FVector2D At;
		double Heading = 0.0;

		TestTrue(TEXT("the first advance reports a pose"), Follower.Advance(0.0, Airframe.Chassis, At, Heading));
		TestEqual(TEXT("and it is the start"), At, Plan.Polyline[0]);
		TestFalse(TEXT("it has not arrived"), Follower.HasArrived());

		// One second per 1000 uu, plus a generous margin, walked in small steps so this
		// exercises the same accumulation a real tick does.
		for (int32 Step = 0; Step < 1000 && !Follower.HasArrived(); ++Step)
		{
			Follower.Advance(0.05, Airframe.Chassis, At, Heading);
		}

		TestTrue(TEXT("it arrives"), Follower.HasArrived());
		TestTrue(TEXT("at the goal, within a uu"),
			FVector2D::Distance(At, Plan.Polyline.Last()) < 1.0);

		// Past the end it must STAY at the end, not run on and not snap to the origin.
		Follower.Advance(100.0, Airframe.Chassis, At, Heading);
		TestTrue(TEXT("and stays there"), FVector2D::Distance(At, Plan.Polyline.Last()) < 1.0);
	}

	// A plan that never found anything must not move an agent at all. Advance returning
	// false is what leaves the caller's pose untouched instead of writing (0,0) into it.
	{
		FRouteFollower Follower;
		FAirframe Airframe;
		FVector2D At(1234.0, 5678.0);
		double Heading = 42.0;

		TestFalse(TEXT("an empty plan does not advance"), Follower.Advance(1.0, Airframe.Chassis, At, Heading));
		TestEqual(TEXT("and leaves the pose alone"), At, FVector2D(1234.0, 5678.0));
		TestTrue(TEXT("an agent that cannot move counts as arrived"), Follower.HasArrived());
	}

	return true;
}

// Named as a sibling, not folded into .Find above, for the same automation-tree reason the
// file banner gives: a distinct dotted leaf, no bare parent for it to collide with.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteSearchEdgeCostCacheTest,
	"Airside.Model.RouteSearch.EdgeCostCache",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteSearchEdgeCostCacheTest::RunTest(const FString& Parameters)
{
	// #171: EdgeCost must read FGuidelineEdge::Length rather than re-sampling and re-measuring
	// the curve on every relaxation. SampleGuidelineCallCountForTest is the boundary that
	// proves it - see URoadNetwork::SampleGuideline's own comment. A test that only checked
	// the ROUTE returned would pass on the un-fixed code too: the cache is read-only here and
	// changes nothing about which path is chosen, only how many times the curve is measured
	// finding it.

	// UNREACHABLE: the search relaxes every edge in Start's small component before Open
	// drains, then reports Unreachable - so NO step is ever walked to build a polyline, and a
	// fixed EdgeCost must leave the counter exactly where it found it. Before the fix, each of
	// those relaxations (including the reverse direction of an edge back toward the already-
	// closed Start, costed before the closed check discards it) sampled and measured the curve
	// itself, so this is the case the fix must bring to EXACTLY zero, not merely lower.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId Start = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId Mid = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
		const FGuidelineNodeId Branch = Net->AddGuidelineNode(FVector2D(1000.0, 1000.0));
		Join(*Net, Start, Mid);
		Join(*Net, Start, Branch);

		// Disconnected from the rest: no edge reaches it, so the search cannot find it and can
		// only exhaust the reachable component trying.
		const FGuidelineNodeId Goal = Net->AddGuidelineNode(FVector2D(9000.0, 9000.0));

		FRouteQuery Query;
		Query.Errand = ERouteErrand::GraphProbe;
		Query.Policy = FRoutePolicy::For(Query.Errand);
		Query.Start = Start;
		Query.Goal = Goal;
		Query.Class = ETraversalClass::Aircraft;

		const int32 Before = Net->SampleGuidelineCallCountForTest();
		const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
		const int32 After = Net->SampleGuidelineCallCountForTest();

		TestEqual(TEXT("an unreachable goal is reported as such"), Plan.Result, ERouteResult::Unreachable);
		TestEqual(TEXT("no relaxation samples the curve it costs"), After - Before, 0);
	}

	// REACHABLE: the diamond from .Find above, with two candidate ways into East, so the
	// search relaxes more edges than the winning route has steps. SampleGuideline is still
	// called here - the polyline the follower walks and the overlay draws still has to come
	// from somewhere - but ONLY by the post-search reconstruction, exactly once per step of
	// the WINNING route, never once per relaxation during the search itself.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId West = Net->AddGuidelineNode(FVector2D(-1000.0, 0.0));
		const FGuidelineNodeId East = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
		const FGuidelineNodeId North = Net->AddGuidelineNode(FVector2D(0.0, 4000.0));
		const FGuidelineNodeId South = Net->AddGuidelineNode(FVector2D(0.0, -100.0));
		Join(*Net, West, North);
		Join(*Net, North, East);
		Join(*Net, West, South);
		Join(*Net, South, East);

		FRouteQuery Query;
		Query.Errand = ERouteErrand::GraphProbe;
		Query.Policy = FRoutePolicy::For(Query.Errand);
		Query.Start = West;
		Query.Goal = East;
		Query.Class = ETraversalClass::Aircraft;

		const int32 Before = Net->SampleGuidelineCallCountForTest();
		const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
		const int32 After = Net->SampleGuidelineCallCountForTest();

		if (!TestTrue(TEXT("a route is found"), Plan.IsValid())) { return false; }
		TestEqual(TEXT("SampleGuideline runs only to build the winning polyline, once per step"),
			After - Before, Plan.Steps.Num());
	}

	return true;
}

// Named as a sibling for the same automation-tree reason the other two give: a distinct
// dotted leaf under RouteSearch, no bare parent to collide with.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteSearchNodeIndexTest,
	"Airside.Model.RouteSearch.IndexMatchesLinear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteSearchNodeIndexTest::RunTest(const FString& Parameters)
{
	// #172: FGuidelineNodeIndex exists ONLY to make FindNearestNode cheaper, never to change
	// what it answers. A test that merely checked "the indexed call returns SOMETHING" would
	// pass on an index that silently missed a neighbouring cell; this compares the indexed
	// and linear paths node for node, over a random fixture and many query points and both
	// classes present in it, so a wrong cell radius or a dropped candidate shows up as a
	// mismatch rather than as a plausible-looking wrong answer.
	FRandomStream Rng(172172);

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// A DENSE FIELD, not a sparse scatter: MaxDistance below is 1500 uu, and node spacing
	// has to be small enough next to that for most queries to actually hit something -
	// otherwise this would mostly be comparing two paths that both return "nothing", which
	// proves neither of them right.
	const int32 NumNodes = 250;
	const double Extent = 6000.0;
	TArray<FGuidelineNodeId> Nodes;
	Nodes.Reserve(NumNodes);
	for (int32 Index = 0; Index < NumNodes; ++Index)
	{
		Nodes.Add(Net->AddGuidelineNode(
			FVector2D(Rng.FRandRange(-Extent, Extent), Rng.FRandRange(-Extent, Extent))));
	}

	// EVERY OTHER NODE GETS AN EDGE TO ITS NEIGHBOUR IN THE ARRAY, cycling through THREE
	// traffic masks - Aircraft-only, GroundVehicle-only, both - so "usable for this Class"
	// genuinely partitions the graph and the test cannot pass by admitting everybody.
	for (int32 Index = 0; Index + 1 < NumNodes; Index += 2)
	{
		const FGuidelineNode* A = Net->GetGuidelineNode(Nodes[Index]);
		const FGuidelineNode* B = Net->GetGuidelineNode(Nodes[Index + 1]);
		FGuidelineEdge Edge;
		Edge.A = Nodes[Index];
		Edge.B = Nodes[Index + 1];
		Edge.Control = (A->Position + B->Position) * 0.5;
		const int32 Which = (Index / 2) % 3;
		Edge.AllowedTraffic = Which == 0 ? FTrafficMask::Only(ETraversalClass::Aircraft)
			: Which == 1 ? FTrafficMask::Only(ETraversalClass::GroundVehicle)
			: FTrafficMask::All();
		Net->AddGuidelineEdge(MoveTemp(Edge));
	}

	// A DEAD SLOT: removed after being added, so the linear scan's bAlive check and the
	// index's build-time skip have to agree on refusing it, not merely on both defaulting
	// to the same thing by accident.
	Net->RemoveGuidelineNode(Nodes[3]);

	const double MaxDistance = 1500.0;
	FGuidelineNodeIndex Index(*Net, MaxDistance);

	int32 FoundSomething = 0;
	for (int32 Query = 0; Query < 500; ++Query)
	{
		const FVector2D At(
			Rng.FRandRange(-Extent * 1.2, Extent * 1.2), Rng.FRandRange(-Extent * 1.2, Extent * 1.2));
		const ETraversalClass Class = (Query % 2 == 0) ? ETraversalClass::Aircraft : ETraversalClass::GroundVehicle;

		const FGuidelineNodeId Linear = RouteSearch::FindNearestNode(*Net, At, Class, MaxDistance);
		const FGuidelineNodeId Indexed = RouteSearch::FindNearestNode(*Net, At, Class, MaxDistance, &Index);
		if (Linear.IsSet())
		{
			++FoundSomething;
		}

		if (!TestEqual(FString::Printf(
			TEXT("query %d (%s) at (%.0f, %.0f): the indexed path agrees with the linear one"),
			Query, Class == ETraversalClass::Aircraft ? TEXT("Aircraft") : TEXT("GroundVehicle"), At.X, At.Y),
			Indexed, Linear))
		{
			return false;
		}
	}

	// THE CONTROL: most of the 500 queries actually found a node, or the loop above would
	// have been 500 near-vacuous comparisons of "unset" against "unset".
	TestTrue(FString::Printf(TEXT("most queries found a node within range (%d of 500)"), FoundSomething),
		FoundSomething > 250);

	// THE TIE ITSELF, built by hand rather than hoped for from the random fixture: two
	// usable nodes exactly MaxDistance apart from the query point on opposite sides of it.
	// FindNearestNode's own comment says an exact tie goes to the LATER node visited - which
	// for the unindexed scan is the higher slot index - so Second, added after First, is the
	// one both paths must return. An indexed path that visited its candidates in whatever
	// order the grid's TMap happened to bucket them, rather than sorting back to ascending
	// index first, would agree with the linear scan only by chance.
	{
		URoadNetwork* Tied = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId First = Tied->AddGuidelineNode(FVector2D(1000.0, 0.0));
		const FGuidelineNodeId Second = Tied->AddGuidelineNode(FVector2D(-1000.0, 0.0));
		Join(*Tied, First, Second);

		FGuidelineNodeIndex TiedIndex(*Tied, 1000.0);
		const FGuidelineNodeId LinearTie =
			RouteSearch::FindNearestNode(*Tied, FVector2D(0.0, 0.0), ETraversalClass::Aircraft, 1000.0);
		const FGuidelineNodeId IndexedTie =
			RouteSearch::FindNearestNode(*Tied, FVector2D(0.0, 0.0), ETraversalClass::Aircraft, 1000.0, &TiedIndex);

		TestEqual(TEXT("the linear scan's own tie-break picks the later-added node"), LinearTie, Second);
		TestEqual(TEXT("and the indexed path breaks the same tie the same way"), IndexedTie, LinearTie);
	}

	return true;
}

// #190, deferred from #171/#201: ArrivalPlanner::ChooseStand's old per-stand loop called
// Find() once per candidate stand - O(exits x stands) searches per dispatch even after #201
// made each individual one cheap. FindToGoals replaces that with ONE search whose goal set
// is every candidate at once. RunSearch and FindToGoals share their neighbour expansion and
// their backtrace (ExpandNode and BuildPlanFromArrival in RouteSearch.cpp), so this compares
// FindToGoals' answer for every goal in ONE call against Find()'s own answer for that SAME
// goal, one call each - a fixture where the two disagreed is exactly what a refactor with no
// behaviour change must not ship.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteSearchFindToGoalsTest,
	"Airside.Model.RouteSearch.FindToGoals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteSearchFindToGoalsTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	const FGuidelineNodeId Start = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId Near = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
	const FGuidelineNodeId Tied = Net->AddGuidelineNode(FVector2D(0.0, 1000.0));   // same length as Near
	const FGuidelineNodeId Far = Net->AddGuidelineNode(FVector2D(0.0, -3000.0));
	const FGuidelineNodeId Marooned = Net->AddGuidelineNode(FVector2D(5000.0, 5000.0)); // never joined
	const FGuidelineNodeId Narrow = Net->AddGuidelineNode(FVector2D(-1000.0, 0.0));

	Join(*Net, Start, Near);
	Join(*Net, Start, Tied);
	Join(*Net, Start, Far);
	Join(*Net, Start, Narrow, EGuidelineDir::Bidirectional, /*MaxWingspan=*/3600.0);

	FRouteQuery Query;
	Query.Errand = ERouteErrand::GraphProbe;
	Query.Policy = FRoutePolicy::For(Query.Errand);
	Query.Start = Start;
	Query.Class = ETraversalClass::Aircraft;
	Query.Wingspan = 6500.0; // wider than Narrow's own edge admits

	FGuidelineNodeId Dead;
	Dead.Index = 999;
	Dead.Generation = 1;

	// Near TWICE, Start itself, and a dead handle thrown in - every exclusion FindToGoals
	// must apply on its own (SameNode, NoGoal, and simply not being asked about a node
	// twice) rather than relying on the caller to have de-duplicated first.
	const TArray<FGuidelineNodeId> Goals = { Near, Tied, Far, Marooned, Narrow, Start, Dead, Near };

	TArray<FGoalReach> Reach;
	const FMultiGoalSearch Search = RouteSearch::FindToGoals(*Net, Query, Goals, Reach);

	if (!TestEqual(TEXT("one entry per goal, order preserved"), Reach.Num(), Goals.Num()))
	{
		return false;
	}

	auto CheckAgainstFind = [&](int32 GoalIndex, const TCHAR* Name)
	{
		FRouteQuery Single = Query;
		Single.Goal = Goals[GoalIndex];
		const FRoutePlan Oracle = RouteSearch::Find(*Net, Single);

		TestEqual(FString::Printf(TEXT("%s: reachability matches Find()"), Name),
			Reach[GoalIndex].bReachable, Oracle.IsValid());
		if (Oracle.IsValid())
		{
			TestTrue(FString::Printf(TEXT("%s: length matches Find() (%.3f vs %.3f)"),
				Name, Reach[GoalIndex].Length, Oracle.Length),
				FMath::IsNearlyEqual(Reach[GoalIndex].Length, Oracle.Length, 1e-6));

			const FRoutePlan Rebuilt = Search.BuildPlan(*Net, Goals[GoalIndex]);
			TestTrue(FString::Printf(TEXT("%s: BuildPlan reconstructs a valid route from the shared search"), Name),
				Rebuilt.IsValid());
			TestEqual(FString::Printf(TEXT("%s: BuildPlan takes the same number of steps as Find()"), Name),
				Rebuilt.Steps.Num(), Oracle.Steps.Num());
			TestTrue(FString::Printf(TEXT("%s: BuildPlan's length matches Find()'s (%.3f vs %.3f)"),
				Name, Rebuilt.Length, Oracle.Length),
				FMath::IsNearlyEqual(Rebuilt.Length, Oracle.Length, 1e-6));
		}
	};

	CheckAgainstFind(0, TEXT("Near"));
	CheckAgainstFind(1, TEXT("Tied"));
	CheckAgainstFind(2, TEXT("Far"));
	CheckAgainstFind(3, TEXT("Marooned"));
	CheckAgainstFind(4, TEXT("Narrow (wingspan-blocked)"));
	CheckAgainstFind(7, TEXT("Near (repeated)"));

	TestFalse(TEXT("Start itself is never reachable from itself"), Reach[5].bReachable);
	TestFalse(TEXT("a dead handle is never reachable"), Reach[6].bReachable);

	// THE TIE ITSELF: Near and Tied cost exactly the same from Start, so both must come back
	// reachable at the same length - FindToGoals settles every goal actually asked for,
	// never stopping at the first one popped the way a single-goal search would.
	TestTrue(TEXT("Near and Tied are both reachable"), Reach[0].bReachable && Reach[1].bReachable);
	TestTrue(TEXT("and cost exactly the same, not merely both non-zero"),
		FMath::IsNearlyEqual(Reach[0].Length, Reach[1].Length, 1e-6));

	return true;
}

#endif
