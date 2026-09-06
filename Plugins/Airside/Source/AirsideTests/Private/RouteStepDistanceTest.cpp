#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the unity build: RouteSearchTest already owns Join().
	FGuidelineEdgeId M2StepJoin(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B,
		const FVector2D* Control = nullptr)
	{
		const FGuidelineNode* NodeA = Net.GetGuidelineNode(A);
		const FGuidelineNode* NodeB = Net.GetGuidelineNode(B);
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		Edge.Control = Control ? *Control : (NodeA->Position + NodeB->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::All();
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

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
	M2StepJoin(*Net, A, B);
	const FVector2D Bend(2000.0, 0.0);
	M2StepJoin(*Net, B, C, &Bend);
	// Reversed on purpose: D->C is the stored direction, the route walks C->D.
	M2StepJoin(*Net, D, C);

	FRouteQuery Query;
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
	M2StepJoin(*Net, A, B);
	M2StepJoin(*Net, B, C);
	M2StepJoin(*Net, B, X);
	M2StepJoin(*Net, X, C);

	FRouteQuery Q; Q.Start = A; Q.Goal = C; Q.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Head = RouteSearch::Find(*Net, Q);
	FRouteQuery T; T.Start = B; T.Goal = C; T.Class = ETraversalClass::GroundVehicle;
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
	M2StepJoin(*Net, West, North); M2StepJoin(*Net, North, East);
	const FGuidelineEdgeId WestSouth = M2StepJoin(*Net, West, South);
	M2StepJoin(*Net, South, East);

	FTrafficOccupancy Table;
	FTrafficClaim Queue; Queue.AgentId = 7; Queue.Resource = FTrafficResource::OfEdge(WestSouth); Queue.From = 0.0; Queue.To = 2500.0;
	FTrafficClaim Blocker;
	Table.TryClaim(Queue, Blocker);

	FRouteQuery Q; Q.Start = West; Q.Goal = East; Q.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Plain = RouteSearch::Find(*Net, Q);
	if (!TestTrue(TEXT("the plain route exists"), Plain.IsValid() && Plain.Steps.Num() == 2)) { return false; }
	TestEqual(TEXT("no table: south"), Plain.Steps[0].To, South);

	Q.Occupancy = &Table; Q.QueryingAgent = 1; Q.CongestionWeight = 2.0;
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
	FRouteQuery Bare; Bare.Start = West; Bare.Goal = East; Bare.Class = ETraversalClass::GroundVehicle;
	TestTrue(TEXT("the null-table plan is the old plan to the point"), Plain.Polyline == RouteSearch::Find(*Net, Bare).Polyline);
	return true;
}

#endif
