#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/Airframe.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteStepEndDistanceTest,
	"Airside.Model.RouteSearch.EndDistance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteStepEndDistanceTest::RunTest(const FString& Parameters)
{
	// A straight, a BEND, a straight - the bend is what makes EndDistance a measurement
	// rather than a sum of chords: it must equal the sampled polyline's length up to the
	// step's end vertex, because that is the array the follower walks.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
	const FGuidelineNodeId C = Net->AddGuidelineNode(FVector2D(2000.0, 1000.0));
	const FGuidelineNodeId D = Net->AddGuidelineNode(FVector2D(2000.0, 3000.0));
	TestGraph::Join(*Net, A, B);
	const FVector2D Bend(2000.0, 0.0);
	TestGraph::Join(*Net, B, C, { EGuidelineDir::Bidirectional, &Bend });
	// Reversed on purpose: D->C is the stored direction, the route walks C->D.
	TestGraph::Join(*Net, D, C);

	FRouteQuery Query;
	Query.Errand = ERouteErrand::GraphProbe;
	Query.Policy = FRoutePolicy::For(Query.Errand);
	Query.Start = A;
	Query.Goal = D;
	Query.Class = ETraversalClass::Aircraft;
	const FRoutePlan Plan = RouteSearch::Find(*Net, Query);
	if (!TestTrue(TEXT("route found"), Plan.IsValid())) { return false; }
	if (!TestEqual(TEXT("three steps"), Plan.Steps.Num(), 3)) { return false; }

	TestEqual(TEXT("step 0 ends at vertex 1 (a straight is two points)"), Plan.Steps[0].EndVertex, 1);
	TestEqual(TEXT("step 0 ends at 1000 uu"), Plan.Steps[0].EndDistance, 1000.0, 1e-6);

	// The bend's EndDistance must be the polyline length to its end vertex, EXACTLY the
	// sum the follower will walk - not the Bezier's true arc length, not the chord.
	TArray<FVector2D> UpToC;
	for (int32 At = 0; At <= Plan.Steps[1].EndVertex; ++At) { UpToC.Add(Plan.Polyline[At]); }
	TestEqual(TEXT("step 1 EndDistance is the sampled polyline length to its end vertex"),
		Plan.Steps[1].EndDistance, GuidelineGeom::PolylineLength(UpToC), 1e-6);
	TestTrue(TEXT("the bend is longer than its chord, so the measurement is not the chord"),
		Plan.Steps[1].EndDistance > 1000.0 + FVector2D::Distance(FVector2D(1000.0, 0.0), FVector2D(2000.0, 1000.0)) + 1.0);

	TestEqual(TEXT("the last step ends at the route's own length"), Plan.Steps[2].EndDistance, Plan.Length, 1e-6);
	TestEqual(TEXT("and at the last vertex"), Plan.Steps[2].EndVertex, Plan.Polyline.Num() - 1);
	TestTrue(TEXT("a reversed step still ends where the route arrives"),
		FVector2D::Distance(Plan.Polyline[Plan.Steps[2].EndVertex], FVector2D(2000.0, 3000.0)) < 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteSpliceTest,
	"Airside.Model.RouteSearch.Splice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteSpliceTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
	const FGuidelineNodeId C = Net->AddGuidelineNode(FVector2D(2000.0, 0.0));
	const FGuidelineNodeId X = Net->AddGuidelineNode(FVector2D(1000.0, 1500.0));
	TestGraph::Join(*Net, A, B);
	TestGraph::Join(*Net, B, C);
	TestGraph::Join(*Net, B, X);
	TestGraph::Join(*Net, X, C);

	FRouteQuery Q; Q.Errand = ERouteErrand::GraphProbe; Q.Policy = FRoutePolicy::For(Q.Errand); Q.Start = A; Q.Goal = C; Q.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Head = RouteSearch::Find(*Net, Q);
	FRouteQuery T; T.Errand = ERouteErrand::GraphProbe; T.Policy = FRoutePolicy::For(T.Errand); T.Start = B; T.Goal = C; T.Class = ETraversalClass::GroundVehicle;
	T.BannedEdge = Head.Steps[1].Edge;   // forbid B->C, so the tail goes via X
	const FRoutePlan Tail = RouteSearch::Find(*Net, T);
	if (!TestTrue(TEXT("tail routes round the ban"), Tail.IsValid() && Tail.Steps.Num() == 2)) { return false; }

	const FRoutePlan Spliced = RouteSearch::Splice(Head, 1, Tail);
	TestTrue(TEXT("spliced plan is valid"), Spliced.IsValid());
	TestEqual(TEXT("keeps one head step and both tail steps"), Spliced.Steps.Num(), 3);
	TestEqual(TEXT("starts where the head started"), Spliced.Start, Head.Start);
	TestEqual(TEXT("length is head-to-B plus the tail"), Spliced.Length, 1000.0 + Tail.Length, 1e-6);
	// EndDistance stays monotone and consistent with the polyline: the follower and the
	// claims read the same numbers, so a splice that shifted one and not the other would
	// stop an agent for a node it had already passed.
	for (int32 Index = 0; Index < Spliced.Steps.Num(); ++Index)
	{
		TArray<FVector2D> Prefix;
		for (int32 At = 0; At <= Spliced.Steps[Index].EndVertex; ++At) { Prefix.Add(Spliced.Polyline[At]); }
		TestEqual(FString::Printf(TEXT("step %d EndDistance matches its polyline prefix"), Index),
			Spliced.Steps[Index].EndDistance, GuidelineGeom::PolylineLength(Prefix), 1e-6);
	}
	TestTrue(TEXT("no duplicated weld point at the splice"),
		FVector2D::Distance(Spliced.Polyline[1], Spliced.Polyline[2]) > 1.0);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteOccupancyCostTest,
	"Airside.Model.RouteSearch.OccupancyCost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteOccupancyCostTest::RunTest(const FString& Parameters)
{
	// The diamond again, with the geometry stated because every assertion turns on it: the
	// control point defaults to the midpoint, which makes each guideline a straight line, so
	// west->south->east is 2 * sqrt(1000^2 + 100^2) = 2010 uu and west->north->east is
	// 2 * sqrt(1000^2 + 2000^2) = 4472 uu. South is shorter by 2462. A queue of 2500 uu on
	// the south edge at weight 2 costs 5000, which is MORE than the detour, so the search
	// must go north; with no table, or with the querier's OWN claim, it must go south
	// exactly as before.
	//
	// (The brief's fixture put north at (0, 4000), where the detour costs 6236 and a 5000
	// congestion charge does not cover it - the search stayed south and the test could not
	// have passed. North moved to (0, 2000) to keep the brief's queue and weight, which are
	// the numbers under test, rather than inflating the queue to fit the geometry.)
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId West = Net->AddGuidelineNode(FVector2D(-1000.0, 0.0));
	const FGuidelineNodeId East = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
	const FGuidelineNodeId North = Net->AddGuidelineNode(FVector2D(0.0, 2000.0));
	const FGuidelineNodeId South = Net->AddGuidelineNode(FVector2D(0.0, -100.0));
	TestGraph::Join(*Net, West, North); TestGraph::Join(*Net, North, East);
	const FGuidelineEdgeId WestSouth = TestGraph::Join(*Net, West, South);
	TestGraph::Join(*Net, South, East);

	FTrafficOccupancy Table;
	FTrafficClaim Queue; Queue.AgentId = 7; Queue.Resource = FTrafficResource::OfEdge(WestSouth); Queue.From = 0.0; Queue.To = 2500.0;
	FTrafficClaim Blocker;
	Table.TryClaim(Queue, Blocker);

	// THE ERRAND CHANGES PART WAY THROUGH, and it has to: this test's whole point is to
	// compare a route costed WITHOUT the table against the same route costed WITH it, and
	// the search refuses a mismatch in either direction. GraphProbe for the plain pass,
	// VehicleToJob (requires the table, carries no runway penalty to disturb the costs) from
	// the moment it is attached. That the errand differs across the two is exactly the
	// distinction being measured, so naming it here is not a workaround.
	FRouteQuery Q; Q.Errand = ERouteErrand::GraphProbe; Q.Policy = FRoutePolicy::For(Q.Errand); Q.Start = West; Q.Goal = East; Q.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Plain = RouteSearch::Find(*Net, Q);
	if (!TestTrue(TEXT("the plain route exists"), Plain.IsValid() && Plain.Steps.Num() == 2)) { return false; }
	TestEqual(TEXT("no table: south"), Plain.Steps[0].To, South);

	Q.Occupancy = &Table; Q.QueryingAgent = 1; Q.CongestionWeight = 2.0;
	Q.Errand = ERouteErrand::VehicleToJob; Q.Policy = FRoutePolicy::For(Q.Errand);
	const FRoutePlan Costed = RouteSearch::Find(*Net, Q);
	if (!TestTrue(TEXT("the costed route exists"), Costed.IsValid())) { return false; }
	TestEqual(TEXT("a queue on the south edge sends a stranger north"), Costed.Steps[0].To, North);

	Q.QueryingAgent = 7;
	const FRoutePlan Own = RouteSearch::Find(*Net, Q);
	if (!TestTrue(TEXT("the owner's route exists"), Own.IsValid())) { return false; }
	TestEqual(TEXT("the queue's own agent is not charged for itself: south"), Own.Steps[0].To, South);

	// A NULL TABLE IS THE OLD SEARCH, POINT FOR POINT. The cost term is the one thing added
	// to EdgeCost, and every caller that never heard of occupancy passes a bare query; a
	// change that moved those routes by a metre would move every ghost the player is shown.
	FRouteQuery Bare; Bare.Errand = ERouteErrand::GraphProbe; Bare.Policy = FRoutePolicy::For(Bare.Errand); Bare.Start = West; Bare.Goal = East; Bare.Class = ETraversalClass::GroundVehicle;
	TestTrue(TEXT("the null-table plan is the old plan to the point"), Plain.Polyline == RouteSearch::Find(*Net, Bare).Polyline);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteRunwayAvoidanceTest,
	"Airside.Model.RouteSearch.RunwayAvoidance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteRunwayAvoidanceTest::RunTest(const FString& Parameters)
{
	// A(0,0) to B(20000,0). The short way is round a runway's end: A -> R1 (1000 south),
	// along the strip R1 -> R2 (20000, an edge DERIVED FROM a runway segment), R2 -> B
	// (1000): 22000 uu. The long way is the taxiway detour over D(10000, 30000): 63246 uu.
	// Which one the search takes is exactly what ERunwayAvoidance decides, and the phantom
	// holder on the runway chain is what Held reads.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RoadR1 = Net->AddNode(FVector2D(0.0, -1000.0));
	const FRoadNodeId RoadR2 = Net->AddNode(FVector2D(20000.0, -1000.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(RoadR1, RoadR2, Runway);
	if (!TestTrue(TEXT("the strip is a runway segment"), Net->IsRunwaySegment(Strip))) { return false; }

	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(20000.0, 0.0));
	const FGuidelineNodeId D = Net->AddGuidelineNode(FVector2D(10000.0, 30000.0));
	const FGuidelineNodeId R1 = Net->AddGuidelineNode(FVector2D(0.0, -1000.0));
	const FGuidelineNodeId R2 = Net->AddGuidelineNode(FVector2D(20000.0, -1000.0));
	TestGraph::Join(*Net, A, D); TestGraph::Join(*Net, D, B);
	TestGraph::Join(*Net, A, R1); TestGraph::Join(*Net, R2, B);
	{
		FGuidelineEdge Along;
		Along.A = R1; Along.B = R2;
		Along.Control = FVector2D(10000.0, -1000.0);
		Along.AllowedTraffic = FTrafficMask::All();
		Along.DerivedFrom = Strip;
		Net->AddGuidelineEdge(MoveTemp(Along));
	}

	auto ViaRunway = [&](const FRoutePlan& Plan) { return Plan.IsValid() && Plan.Steps.Num() == 3 && Plan.Steps[0].To == R1; };
	auto ViaDetour = [&](const FRoutePlan& Plan) { return Plan.IsValid() && Plan.Steps.Num() == 2 && Plan.Steps[0].To == D; };

	// THE ERRAND CHANGES PART WAY THROUGH THIS TEST, and it has to. The first three cases
	// run with NO occupancy table and the rest run with one, and the search refuses a
	// mismatch either way round - so a single errand cannot cover both halves. GraphProbe
	// (reads no table) here, VehicleToJob (requires one, and carries no runway penalty to
	// disturb the costs) from the moment the table is attached.
	//
	// AvoidRunways is swept by hand below: this test is about ERunwayAvoidance itself, one
	// layer under the policy that normally chooses it.
	FRouteQuery Q; Q.Errand = ERouteErrand::GraphProbe; Q.Policy = FRoutePolicy::For(Q.Errand); Q.Start = A; Q.Goal = B; Q.Class = ETraversalClass::Aircraft;
	TestTrue(TEXT("None: the runway end is ordinary line, the short way"), ViaRunway(RouteSearch::Find(*Net, Q)));

	Q.AvoidRunways = ERunwayAvoidance::All;
	TestTrue(TEXT("All: never along the strip, whatever the table - the detour"), ViaDetour(RouteSearch::Find(*Net, Q)));

	Q.AvoidRunways = ERunwayAvoidance::Held;
	TestTrue(TEXT("Held with no table: every runway is free - the short way"), ViaRunway(RouteSearch::Find(*Net, Q)));

	FTrafficOccupancy Table;
	Q.Occupancy = &Table; Q.QueryingAgent = 1;
	Q.Errand = ERouteErrand::VehicleToJob; Q.Policy = FRoutePolicy::For(Q.Errand);
	TestTrue(TEXT("Held, empty table: the short way"), ViaRunway(RouteSearch::Find(*Net, Q)));

	// A RESERVATION IS ENOUGH: a departure at the bar holds the strip reserved, not occupied,
	// and it is exactly the agent the ban was written to protect.
	FTrafficClaim Bar; Bar.AgentId = 7; Bar.Resource = FTrafficResource::OfSurface(Strip); Bar.bOccupied = false;
	FTrafficClaim Blocker;
	Table.TryClaim(Bar, Blocker);
	TestTrue(TEXT("Held, a stranger's reservation on the chain: the detour"), ViaDetour(RouteSearch::Find(*Net, Q)));

	Q.QueryingAgent = 7;
	TestTrue(TEXT("Held, the querier's OWN reservation does not count: the short way"), ViaRunway(RouteSearch::Find(*Net, Q)));

	// BUT ITS OWN BODY DOES. An aircraft standing on the strip (its claim occupied - the
	// crossing rule, or a roll-out) must not be routed along it: the jam it is replanning
	// out of is a queue for that strip, and the waiter at the bar holds nothing the table
	// can show. Traffic.HeadOnReplansRoundBarHolder is the case in full.
	FTrafficClaim Body; Body.AgentId = 7; Body.Resource = FTrafficResource::OfSurface(Strip); Body.bOccupied = true;
	Table.TryClaim(Body, Blocker);
	TestTrue(TEXT("Held, the querier is standing on the strip: the detour"), ViaDetour(RouteSearch::Find(*Net, Q)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteRunwaySeedMemoTest,
	"Airside.Model.RouteSearch.RunwaySeedMemo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteRunwaySeedMemoTest::RunTest(const FString& Parameters)
{
	// A LADDER ALONG ONE STRIP: five guideline edges all DerivedFrom the SAME runway segment,
	// so a search that walks them asks "is this a runway" five times and must resolve the
	// seed exactly once.
	//
	// MEASURES THE MEMO, does not name it. The search returns the same route with or without
	// one, which is precisely why a correctness assertion here would stay green on a memo
	// that had been deleted - only the count can tell.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RoadA = Net->AddNode(FVector2D(0.0, -1000.0));
	const FRoadNodeId RoadB = Net->AddNode(FVector2D(50000.0, -1000.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(RoadA, RoadB, Runway);
	if (!TestTrue(TEXT("the strip is a runway segment"), Net->IsRunwaySegment(Strip))) { return false; }

	TArray<FGuidelineNodeId> Chain;
	for (int32 Index = 0; Index <= 5; ++Index)
	{
		Chain.Add(Net->AddGuidelineNode(FVector2D(Index * 10000.0, -1000.0)));
	}
	for (int32 Index = 0; Index < 5; ++Index)
	{
		FGuidelineEdge Along;
		Along.A = Chain[Index];
		Along.B = Chain[Index + 1];
		Along.Control = FVector2D((Index * 10000.0) + 5000.0, -1000.0);
		Along.AllowedTraffic = FTrafficMask::All();
		Along.DerivedFrom = Strip;
		Net->AddGuidelineEdge(MoveTemp(Along));
	}

	RouteSearch::ResetRunwaySeedResolveCountForTest();

	FRouteQuery Q;
	Q.Errand = ERouteErrand::GraphProbe;
	Q.Policy = FRoutePolicy::For(Q.Errand);
	Q.Start = Chain[0];
	Q.Goal = Chain.Last();
	Q.Class = ETraversalClass::Aircraft;
	// Held, not All: All would delete every edge, the search would never reach the cost, and
	// the ladder would be walked once - making the memo look unnecessary.
	Q.AvoidRunways = ERunwayAvoidance::Held;

	const FRoutePlan Plan = RouteSearch::Find(*Net, Q);
	TestTrue(TEXT("the ladder is routable, or the count below measures an empty search"), Plan.IsValid());

	TestEqual(TEXT("one seed resolved once, however many of its edges the search relaxed"),
		RouteSearch::RunwaySeedResolveCountForTest(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteRunwayPenaltyTest,
	"Airside.Model.RouteSearch.RunwayPenalty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteRunwayPenaltyTest::RunTest(const FString& Parameters)
{
	// A(0,0) to B(20000,0). Along the strip is SHORT; the detour via D is longer but legal.
	// The penalty is the only thing that can make the longer way win, which is what makes
	// this measure the rule rather than name it: at a multiplier of 1.0 it must go red.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RoadR1 = Net->AddNode(FVector2D(0.0, -1000.0));
	const FRoadNodeId RoadR2 = Net->AddNode(FVector2D(20000.0, -1000.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(RoadR1, RoadR2, Runway);
	if (!TestTrue(TEXT("the strip is a runway segment"), Net->IsRunwaySegment(Strip))) { return false; }

	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(20000.0, 0.0));
	// D at 8000 north: the detour is roughly 34000 uu against the strip's ~22000, so it
	// loses on plain length and wins at a penalty of ten. A margin, not a hair - a detour
	// that only just won would be measuring floating-point noise.
	const FGuidelineNodeId D = Net->AddGuidelineNode(FVector2D(10000.0, 8000.0));
	const FGuidelineNodeId R1 = Net->AddGuidelineNode(FVector2D(0.0, -1000.0));
	const FGuidelineNodeId R2 = Net->AddGuidelineNode(FVector2D(20000.0, -1000.0));
	TestGraph::Join(*Net, A, D); TestGraph::Join(*Net, D, B);
	TestGraph::Join(*Net, A, R1); TestGraph::Join(*Net, R2, B);
	{
		FGuidelineEdge Along;
		Along.A = R1; Along.B = R2;
		Along.Control = FVector2D(10000.0, -1000.0);
		Along.AllowedTraffic = FTrafficMask::All();
		Along.DerivedFrom = Strip;
		Net->AddGuidelineEdge(MoveTemp(Along));
	}

	auto ViaRunway = [&](const FRoutePlan& Plan) { return Plan.IsValid() && Plan.Steps.Num() == 3 && Plan.Steps[0].To == R1; };
	auto ViaDetour = [&](const FRoutePlan& Plan) { return Plan.IsValid() && Plan.Steps.Num() == 2 && Plan.Steps[0].To == D; };

	const FAirframe Airframe;

	// GraphProbe: no filter, no penalty. The strip is ordinary line and the short way wins.
	const FRouteQuery Probe = FRouteQuery::For(ERouteErrand::GraphProbe, A, B, Airframe.Wingspan, ETraversalClass::Aircraft);
	TestTrue(TEXT("with no policy at all the strip is ordinary line - the short way"),
		ViaRunway(RouteSearch::Find(*Net, Probe)));

	// PlayerIssued: no filter, but a penalty. Same graph, opposite answer.
	FRouteQuery Player = FRouteQuery::For(ERouteErrand::PlayerIssued, A, B, Airframe.Wingspan, ETraversalClass::Aircraft);
	TestTrue(TEXT("the penalty alone sends a player-issued route round the strip"),
		ViaDetour(RouteSearch::Find(*Net, Player)));

	// AND THE PENALTY IS THE THING DOING IT, not the errand: turn the multiplier off and the
	// same errand takes the strip again. Without this the test would still pass on a build
	// where PlayerIssued had quietly been given an All filter instead.
	Player.RunwayPenalty = 1.0;
	TestTrue(TEXT("at a multiplier of one the same errand takes the strip"),
		ViaRunway(RouteSearch::Find(*Net, Player)));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteRunwayOnlyWayTest,
	"Airside.Model.RouteSearch.RunwayPenaltyStillRoutesWhenItIsTheOnlyWay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteRunwayOnlyWayTest::RunTest(const FString& Parameters)
{
	// THE DEGRADATION PlayerIssued EXISTS FOR. No detour at all: a filter would report
	// Unreachable and read as a broken tool, where a penalty takes the only way there is.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RoadR1 = Net->AddNode(FVector2D(0.0, -1000.0));
	const FRoadNodeId RoadR2 = Net->AddNode(FVector2D(20000.0, -1000.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(RoadR1, RoadR2, Runway);

	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(20000.0, 0.0));
	const FGuidelineNodeId R1 = Net->AddGuidelineNode(FVector2D(0.0, -1000.0));
	const FGuidelineNodeId R2 = Net->AddGuidelineNode(FVector2D(20000.0, -1000.0));
	TestGraph::Join(*Net, A, R1); TestGraph::Join(*Net, R2, B);
	{
		FGuidelineEdge Along;
		Along.A = R1; Along.B = R2;
		Along.Control = FVector2D(10000.0, -1000.0);
		Along.AllowedTraffic = FTrafficMask::All();
		Along.DerivedFrom = Strip;
		Net->AddGuidelineEdge(MoveTemp(Along));
	}

	const FAirframe Airframe;

	const FRoutePlan Player = RouteSearch::Find(*Net,
		FRouteQuery::For(ERouteErrand::PlayerIssued, A, B, Airframe.Wingspan, ETraversalClass::Aircraft));
	TestTrue(TEXT("a penalty is expensive, not impossible: the only way through is still found"),
		Player.IsValid());

	// THE CONTRAST THAT GIVES THAT ITS MEANING. An errand with a filter reports Unreachable
	// on the very same graph - so the assertion above is about the penalty, not about the
	// graph happening to be routable.
	const FRoutePlan TaxiIn = RouteSearch::Find(*Net,
		FRouteQuery::For(ERouteErrand::ArrivalTaxiIn, A, B, Airframe.Wingspan, ETraversalClass::Aircraft));
	TestEqual(TEXT("a filtered errand refuses the same graph outright"),
		TaxiIn.Result, ERouteResult::Unreachable);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteChangedErrandsTest,
	"Airside.Model.RouteSearch.ErrandsThatGainedAFilter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRouteChangedErrandsTest::RunTest(const FString& Parameters)
{
	// THE 2026-09-21 REPORT, PINNED: "aircraft see the runway as a taxi route".
	//
	// Four call sites declared no runway policy and got the permissive one by omission.
	// Nothing in the suite moved when they were given errands - every existing fixture's
	// edges are hand-authored and carry no DerivedFrom, so none of them could ever have
	// routed along a strip. A change nothing measures is a change nobody can defend, so
	// this builds the graph those sites were missing and names each errand that gained a
	// rule.
	//
	// AT THE SEARCH LAYER, NOT END TO END, and that is a real limit: PushbackPlanner is
	// private to the plugin, so the taxi-out route cannot be asked for by name from a test
	// module. This pins the ROW those planners now resolve. The route an actual pushback
	// produces is still only shown by the in-editor repro.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RoadR1 = Net->AddNode(FVector2D(0.0, -1000.0));
	const FRoadNodeId RoadR2 = Net->AddNode(FVector2D(20000.0, -1000.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(RoadR1, RoadR2, Runway);
	if (!TestTrue(TEXT("the strip is a runway segment"), Net->IsRunwaySegment(Strip))) { return false; }

	// The strip is the SHORT way; the detour via D exists and is longer. An errand with no
	// rule takes the strip, which is precisely the reported behaviour.
	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(20000.0, 0.0));
	const FGuidelineNodeId D = Net->AddGuidelineNode(FVector2D(10000.0, 8000.0));
	const FGuidelineNodeId R1 = Net->AddGuidelineNode(FVector2D(0.0, -1000.0));
	const FGuidelineNodeId R2 = Net->AddGuidelineNode(FVector2D(20000.0, -1000.0));
	TestGraph::Join(*Net, A, D); TestGraph::Join(*Net, D, B);
	TestGraph::Join(*Net, A, R1); TestGraph::Join(*Net, R2, B);
	{
		FGuidelineEdge Along;
		Along.A = R1; Along.B = R2;
		Along.Control = FVector2D(10000.0, -1000.0);
		Along.AllowedTraffic = FTrafficMask::All();
		Along.DerivedFrom = Strip;
		Net->AddGuidelineEdge(MoveTemp(Along));
	}

	const FAirframe Airframe;
	auto ViaRunway = [&](const FRoutePlan& Plan) { return Plan.IsValid() && Plan.Steps.Num() == 3 && Plan.Steps[0].To == R1; };
	auto ViaDetour = [&](const FRoutePlan& Plan) { return Plan.IsValid() && Plan.Steps.Num() == 2 && Plan.Steps[0].To == D; };

	// THE CONTROL FIRST. Without a rule the strip wins - so every assertion below is about
	// the errand's row, not about the graph preferring the detour anyway.
	TestTrue(TEXT("an errand with no rule takes the strip - the behaviour reported"),
		ViaRunway(RouteSearch::Find(*Net,
			FRouteQuery::For(ERouteErrand::GraphProbe, A, B, Airframe.Wingspan, ETraversalClass::Aircraft))));

	// The two pushback errands. PushbackTaxiOut is the long taxi from where a push ends to
	// the runway entry, and is the site that reproduces the report.
	TestTrue(TEXT("PushbackTaxiOut no longer taxis down the strip"),
		ViaDetour(RouteSearch::Find(*Net,
			FRouteQuery::For(ERouteErrand::PushbackTaxiOut, A, B, Airframe.Wingspan, ETraversalClass::Aircraft))));
	TestTrue(TEXT("PushbackClear no longer taxis down the strip"),
		ViaDetour(RouteSearch::Find(*Net,
			FRouteQuery::For(ERouteErrand::PushbackClear, A, B, Airframe.Wingspan, ETraversalClass::Aircraft))));

	// And the errands that already had a rule, so a future edit to the table cannot quietly
	// swap two rows and leave this file green.
	TestTrue(TEXT("ArrivalTaxiIn still refuses the strip"),
		ViaDetour(RouteSearch::Find(*Net,
			FRouteQuery::For(ERouteErrand::ArrivalTaxiIn, A, B, Airframe.Wingspan, ETraversalClass::Aircraft))));
	TestTrue(TEXT("DepartureToEntry still refuses the strip"),
		ViaDetour(RouteSearch::Find(*Net,
			FRouteQuery::For(ERouteErrand::DepartureToEntry, A, B, Airframe.Wingspan, ETraversalClass::Aircraft))));

	// THE EXCEPTION, asserted rather than assumed. A backtrack exists to use the strip, and
	// an over-eager ban would silently strand every intersection departure.
	TestTrue(TEXT("DepartureBacktrack still MAY use the strip - the one errand that must"),
		ViaRunway(RouteSearch::Find(*Net,
			FRouteQuery::For(ERouteErrand::DepartureBacktrack, A, B, Airframe.Wingspan, ETraversalClass::Aircraft))));

	return true;
}

#endif
