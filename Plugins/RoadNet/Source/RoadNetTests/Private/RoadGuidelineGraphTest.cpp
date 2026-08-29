#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadGuidelineGraphTest,
	"RoadNet.Model.GuidelineGraph",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadGuidelineGraphTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));

	TestTrue(TEXT("a new guideline node handle is set"), A.IsSet());
	TestNotNull(TEXT("a new guideline node resolves"), Net->GetGuidelineNode(A));

	FGuidelineEdge Edge;
	Edge.A = A;
	Edge.B = B;
	Edge.Control = FVector2D(500.0, 0.0);
	Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::Aircraft);
	Edge.Direction = EGuidelineDir::Bidirectional;
	Edge.Width = 0.0;

	const FGuidelineEdgeId Id = Net->AddGuidelineEdge(MoveTemp(Edge));
	TestTrue(TEXT("a new guideline edge handle is set"), Id.IsSet());

	// Incidence is maintained by the network, not by the caller. A caller-maintained
	// adjacency list is the classic way for a graph to go quietly inconsistent.
	{
		const FGuidelineNode* NodeA = Net->GetGuidelineNode(A);
		const FGuidelineNode* NodeB = Net->GetGuidelineNode(B);
		if (TestNotNull(TEXT("node A resolves"), NodeA) && TestNotNull(TEXT("node B resolves"), NodeB))
		{
			TestTrue(TEXT("the edge is incident to A"), NodeA->Incident.Contains(Id));
			TestTrue(TEXT("the edge is incident to B"), NodeB->Incident.Contains(Id));
		}
	}

	// Removing an edge must retract it from BOTH endpoints, or a later traversal walks a
	// dead handle.
	{
		TestTrue(TEXT("the edge removes"), Net->RemoveGuidelineEdge(Id));
		TestNull(TEXT("a removed edge no longer resolves"), Net->GetGuidelineEdge(Id));

		const FGuidelineNode* NodeA = Net->GetGuidelineNode(A);
		const FGuidelineNode* NodeB = Net->GetGuidelineNode(B);
		if (TestNotNull(TEXT("node A still resolves"), NodeA) && TestNotNull(TEXT("node B still resolves"), NodeB))
		{
			TestFalse(TEXT("A no longer lists the edge"), NodeA->Incident.Contains(Id));
			TestFalse(TEXT("B no longer lists the edge"), NodeB->Incident.Contains(Id));
		}
	}

	// Generation checking, which is the whole point of the handle. A recycled slot must
	// NOT resolve through the old handle - the failure it prevents is an edit silently
	// landing on whatever object took the slot over.
	{
		const FGuidelineNodeId Doomed = Net->AddGuidelineNode(FVector2D(50.0, 50.0));
		TestTrue(TEXT("the doomed node removes"), Net->RemoveGuidelineNode(Doomed));

		const FGuidelineNodeId Recycled = Net->AddGuidelineNode(FVector2D(60.0, 60.0));
		TestEqual(TEXT("the slot was reused"), Recycled.Index, Doomed.Index);
		TestNotEqual(TEXT("but the generation moved on"), Recycled.Generation, Doomed.Generation);
		TestNull(TEXT("the stale handle does not resolve"), Net->GetGuidelineNode(Doomed));
		TestNotNull(TEXT("the fresh handle does"), Net->GetGuidelineNode(Recycled));
	}

	// Removing a node takes its edges with it. Leaving them would strand edges pointing at
	// a dead node, which reads as a graph with a hole rather than as a removal.
	{
		const FGuidelineNodeId L = Net->AddGuidelineNode(FVector2D(0.0, 500.0));
		const FGuidelineNodeId R = Net->AddGuidelineNode(FVector2D(0.0, 900.0));

		FGuidelineEdge Span;
		Span.A = L;
		Span.B = R;
		const FGuidelineEdgeId SpanId = Net->AddGuidelineEdge(MoveTemp(Span));

		TestTrue(TEXT("the endpoint node removes"), Net->RemoveGuidelineNode(L));
		TestNull(TEXT("its edge went with it"), Net->GetGuidelineEdge(SpanId));

		const FGuidelineNode* Survivor = Net->GetGuidelineNode(R);
		if (TestNotNull(TEXT("the far node survives"), Survivor))
		{
			TestFalse(TEXT("and no longer lists the edge"), Survivor->Incident.Contains(SpanId));
		}
	}

	// A self-loop must appear in Incident ONCE. Without the guard in AddGuidelineEdge the
	// node lists it twice and a single Remove leaves one behind - an edge handle in an
	// incidence list resolving to nothing, which is the exact inconsistency the handle
	// discipline exists to prevent.
	{
		const FGuidelineNodeId Loop = Net->AddGuidelineNode(FVector2D(700.0, 700.0));

		FGuidelineEdge Self;
		Self.A = Loop;
		Self.B = Loop;
		const FGuidelineEdgeId SelfId = Net->AddGuidelineEdge(MoveTemp(Self));
		TestTrue(TEXT("a self-loop is accepted"), SelfId.IsSet());

		const FGuidelineNode* LoopNode = Net->GetGuidelineNode(Loop);
		if (TestNotNull(TEXT("the self-loop's node resolves"), LoopNode))
		{
			int32 Occurrences = 0;
			for (const FGuidelineEdgeId IncidentId : LoopNode->Incident)
			{
				if (IncidentId == SelfId)
				{
					++Occurrences;
				}
			}
			TestEqual(TEXT("a self-loop is listed once, not twice"), Occurrences, 1);
		}

		TestTrue(TEXT("the self-loop removes"), Net->RemoveGuidelineEdge(SelfId));

		const FGuidelineNode* AfterRemoval = Net->GetGuidelineNode(Loop);
		if (TestNotNull(TEXT("the node survives its self-loop's removal"), AfterRemoval))
		{
			TestFalse(TEXT("leaving nothing dangling"), AfterRemoval->Incident.Contains(SelfId));
		}
	}

	// The documented rejection path: "A and B must both be live, or this returns an unset
	// handle and adds nothing." Both halves matter - a version that returned unset AFTER
	// appending would leave a dead edge occupying a slot and half-linked incidence.
	{
		const FGuidelineNodeId Alive = Net->AddGuidelineNode(FVector2D(1500.0,   0.0));
		const FGuidelineNodeId Dead  = Net->AddGuidelineNode(FVector2D(1500.0, 100.0));
		TestTrue(TEXT("the doomed endpoint removes"), Net->RemoveGuidelineNode(Dead));

		int32 LiveBefore = 0;
		for (const FGuidelineEdge& E : Net->GetGuidelineEdges())
		{
			if (E.bAlive)
			{
				++LiveBefore;
			}
		}

		FGuidelineEdge Rejected;
		Rejected.A = Alive;
		Rejected.B = Dead;
		const FGuidelineEdgeId RejectedId = Net->AddGuidelineEdge(MoveTemp(Rejected));

		TestFalse(TEXT("an edge to a dead node is refused"), RejectedId.IsSet());

		int32 LiveAfter = 0;
		for (const FGuidelineEdge& E : Net->GetGuidelineEdges())
		{
			if (E.bAlive)
			{
				++LiveAfter;
			}
		}
		TestEqual(TEXT("and nothing was added"), LiveAfter, LiveBefore);

		const FGuidelineNode* AliveNode = Net->GetGuidelineNode(Alive);
		if (TestNotNull(TEXT("the live endpoint resolves"), AliveNode))
		{
			TestEqual(TEXT("and gained no incidence"), AliveNode->Incident.Num(), 0);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
