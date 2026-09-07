#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/NodeReach.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

DEFINE_LOG_CATEGORY_STATIC(LogNodeReachTest, Log, All);

namespace
{
	FGuidelineNodeId NRNode(URoadNetwork& Net, double X, double Y)
	{
		return Net.AddGuidelineNode(FVector2D(X, Y), /*bDerived=*/false);
	}

	/** Straight unless a control point is given. */
	FGuidelineEdgeId NRJoin(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B,
		TOptional<FVector2D> Control = TOptional<FVector2D>())
	{
		FGuidelineEdge Edge;
		Edge.A = A; Edge.B = B;
		Edge.Control = Control.IsSet() ? *Control
			: (Net.GetGuidelineNode(A)->Position + Net.GetGuidelineNode(B)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::All();
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	constexpr double NRFootprint = 1000.0;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNodeReachStraightTest,
	"Airside.Model.NodeReach.StraightContinuation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNodeReachStraightTest::RunTest(const FString& Parameters)
{
	// THE FLOOR, AND THE PROOF THAT NOTHING ORDINARY CHANGES. Two bodies the same distance
	// either side of a node on a straight line are 2s apart, so they part at F/2 - which is
	// exactly what the claim pass held before reach existed. A split taxiway must not make
	// a follower brake for its leader's node.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId W = NRNode(*Net, -20000.0, 0.0);
	const FGuidelineNodeId J = NRNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId E = NRNode(*Net, 20000.0, 0.0);
	NRJoin(*Net, W, J);
	const FGuidelineEdgeId Out = NRJoin(*Net, J, E);

	const double Reach = NodeReach::Compute(*Net, J, Out, NRFootprint);
	UE_LOG(LogNodeReachTest, Log, TEXT("StraightContinuation measured: reach %.1f uu (want %.0f)"), Reach, NRFootprint * 0.5);
	TestTrue(FString::Printf(TEXT("a straight continuation reaches exactly half a footprint (%.1f)"), Reach),
		FMath::IsNearlyEqual(Reach, NRFootprint * 0.5, 1.0));

	// A node with one edge, or an edge that does not meet the node, is the floor too.
	const FGuidelineNodeId Lone = NRNode(*Net, 0.0, 50000.0);
	const FGuidelineEdgeId Stub = NRJoin(*Net, Lone, NRNode(*Net, 0.0, 60000.0));
	TestTrue(TEXT("a node with nothing else at it is the floor"),
		FMath::IsNearlyEqual(NodeReach::Compute(*Net, Lone, Stub, NRFootprint), NRFootprint * 0.5, 1.0));
	TestTrue(TEXT("an edge that does not meet the node is the floor"),
		FMath::IsNearlyEqual(NodeReach::Compute(*Net, J, Stub, NRFootprint), NRFootprint * 0.5, 1.0));
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNodeReachRightAngleTest,
	"Airside.Model.NodeReach.RightAngle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNodeReachRightAngleTest::RunTest(const FString& Parameters)
{
	// Two bodies s down two perpendicular edges are s*sqrt(2) apart: they part at F/sqrt(2)
	// = 707 uu, and the sampler's one-step-long answer lands within a sixteenth of a
	// footprint above that. Pinned so a change to the sampling step is a change here.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId J = NRNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId E = NRNode(*Net, 20000.0, 0.0);
	const FGuidelineNodeId N = NRNode(*Net, 0.0, 20000.0);
	const FGuidelineEdgeId ToE = NRJoin(*Net, J, E);
	NRJoin(*Net, J, N);

	const double Reach = NodeReach::Compute(*Net, J, ToE, NRFootprint);
	const double Exact = NRFootprint / FMath::Sqrt(2.0);
	UE_LOG(LogNodeReachTest, Log, TEXT("RightAngle measured: reach %.1f uu (exact parting %.1f, step %.1f)"),
		Reach, Exact, NRFootprint / 16.0);
	TestTrue(FString::Printf(TEXT("never shorter than where the bodies actually part (%.1f vs %.1f)"), Reach, Exact),
		Reach >= Exact);
	TestTrue(FString::Printf(TEXT("and no more than one sample longer (%.1f)"), Reach),
		Reach <= Exact + NRFootprint / 16.0 + 1.0);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNodeReachTangentArcTest,
	"Airside.Model.NodeReach.TangentArc",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNodeReachTangentArcTest::RunTest(const FString& Parameters)
{
	// THE CASE THE RULE EXISTS FOR: a stand's sweep arc leaving its taxiway tangentially.
	// Bezier from the join, control point straight down the taxiway, so the arc and the
	// line share a tangent at the node and part only as the curve bends away - at roughly
	// sqrt(2 R F) for a circle of that radius, 2272 uu here, four and a half times the
	// half footprint the claim pass used to hold. The measured figure on the two-stand
	// fixture was 2454 of a 4058 uu arc (2026-09-07); this pins the same order.
	const double R = 2582.0;
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId J = NRNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId S = NRNode(*Net, 0.0, -20000.0);
	const FGuidelineNodeId Stand = NRNode(*Net, R, -R);
	const FGuidelineEdgeId Line = NRJoin(*Net, J, S);
	const FGuidelineEdgeId Arc = NRJoin(*Net, J, Stand, FVector2D(0.0, -R));

	const double AlongArc = NodeReach::Compute(*Net, J, Arc, NRFootprint);
	const double AlongLine = NodeReach::Compute(*Net, J, Line, NRFootprint);
	UE_LOG(LogNodeReachTest, Log, TEXT("TangentArc measured: reach %.0f uu along the arc, %.0f along the line (circle estimate %.0f)"),
		AlongArc, AlongLine, FMath::Sqrt(2.0 * R * NRFootprint));

	TestTrue(FString::Printf(TEXT("the arc's reach is far past half a footprint (%.0f)"), AlongArc), AlongArc > 1700.0);
	TestTrue(FString::Printf(TEXT("but short of the whole arc (%.0f)"), AlongArc), AlongArc < 3500.0);
	TestTrue(FString::Printf(TEXT("and the taxiway's reach at the same node matches it (%.0f vs %.0f)"), AlongLine, AlongArc),
		FMath::IsNearlyEqual(AlongLine, AlongArc, NRFootprint / 16.0 + 1.0));

	// Seen from the FAR end of the arc, where it meets nothing else, it is an ordinary node.
	TestTrue(TEXT("the stand end is the floor"),
		FMath::IsNearlyEqual(NodeReach::Compute(*Net, Stand, Arc, NRFootprint), NRFootprint * 0.5, 1.0));
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNodeReachCacheTest,
	"Airside.Model.NodeReach.CacheFollowsRevision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNodeReachCacheTest::RunTest(const FString& Parameters)
{
	// A hand-drawn arc added at a node AFTER the table was filled must change that node's
	// answer - this is why the reach is derived from the graph and not stored by the
	// builder. The revision is what tells the cache; a cache that missed it would hold the
	// claim pass to a half footprint on a node that has just grown a hugging edge.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId J = NRNode(*Net, 0.0, 0.0);
	const FGuidelineNodeId S = NRNode(*Net, 0.0, -20000.0);
	const FGuidelineNodeId N = NRNode(*Net, 0.0, 20000.0);
	const FGuidelineEdgeId Line = NRJoin(*Net, J, S);
	NRJoin(*Net, J, N);

	FNodeReachCache Cache;
	const double Before = Cache.Get(*Net, J, Line, NRFootprint);
	const uint32 RevisionBefore = Net->GetGuidelineRevision();
	TestTrue(TEXT("a straight line through the node is the floor"), FMath::IsNearlyEqual(Before, NRFootprint * 0.5, 1.0));
	TestEqual(TEXT("one entry held"), Cache.NumForTest(), 1);
	TestTrue(TEXT("a second ask is served from the table"),
		FMath::IsNearlyEqual(Cache.Get(*Net, J, Line, NRFootprint), Before, 0.0) && Cache.NumForTest() == 1);

	const double R = 2582.0;
	NRJoin(*Net, J, NRNode(*Net, R, -R), FVector2D(0.0, -R));
	TestTrue(TEXT("adding an edge bumped the revision"), Net->GetGuidelineRevision() > RevisionBefore);

	const double After = Cache.Get(*Net, J, Line, NRFootprint);
	UE_LOG(LogNodeReachTest, Log, TEXT("CacheFollowsRevision measured: %.0f uu before the arc, %.0f after"), Before, After);
	TestTrue(FString::Printf(TEXT("the same node's reach grew with the arc (%.0f -> %.0f)"), Before, After), After > 1700.0);

	Cache.Invalidate();
	TestEqual(TEXT("Invalidate empties the table"), Cache.NumForTest(), 0);
	return true;
}

#endif
