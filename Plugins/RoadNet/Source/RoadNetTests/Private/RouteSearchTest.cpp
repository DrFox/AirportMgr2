#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteFollower.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteSearchTest,
	"RoadNet.Model.RouteSearch",
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
		Forward.Start = A;
		Forward.Goal = B;
		Forward.Class = ETraversalClass::Aircraft;
		TestTrue(TEXT("the one-way runs forwards"), RouteSearch::Find(*OneWay, Forward).IsValid());

		FRouteQuery Back;
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
		// are measured in RoadNet.Model.TurnRate.
		FGroundPerformance Ground;
		Ground.MaxTurnRateDegPerSec = 1.0e6;
		Ground.Taxi.Accel = 1.0e9;
		Ground.Taxi.Decel = 1.0e9;
		Follower.Start(Plan, Ground);

		FVector2D At;
		double Heading = 0.0;

		TestTrue(TEXT("the first advance reports a pose"), Follower.Advance(0.0, At, Heading));
		TestEqual(TEXT("and it is the start"), At, Plan.Polyline[0]);
		TestFalse(TEXT("it has not arrived"), Follower.HasArrived());

		// One second per 1000 uu, plus a generous margin, walked in small steps so this
		// exercises the same accumulation a real tick does.
		for (int32 Step = 0; Step < 1000 && !Follower.HasArrived(); ++Step)
		{
			Follower.Advance(0.05, At, Heading);
		}

		TestTrue(TEXT("it arrives"), Follower.HasArrived());
		TestTrue(TEXT("at the goal, within a uu"),
			FVector2D::Distance(At, Plan.Polyline.Last()) < 1.0);

		// Past the end it must STAY at the end, not run on and not snap to the origin.
		Follower.Advance(100.0, At, Heading);
		TestTrue(TEXT("and stays there"), FVector2D::Distance(At, Plan.Polyline.Last()) < 1.0);
	}

	// A plan that never found anything must not move an agent at all. Advance returning
	// false is what leaves the caller's pose untouched instead of writing (0,0) into it.
	{
		FRouteFollower Follower;
		FVector2D At(1234.0, 5678.0);
		double Heading = 42.0;

		TestFalse(TEXT("an empty plan does not advance"), Follower.Advance(1.0, At, Heading));
		TestEqual(TEXT("and leaves the pose alone"), At, FVector2D(1234.0, 5678.0));
		TestTrue(TEXT("an agent that cannot move counts as arrived"), Follower.HasArrived());
	}

	return true;
}

#endif
