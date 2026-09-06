#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the unity build: RouteSearchTest already owns Join().
	// (TrafficOccupancy.h is Task 2's; until then the include is absent and Task 7 adds it.)
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

#endif
