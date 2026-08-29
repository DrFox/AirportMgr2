#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadGuidelineBuilderTest,
	"RoadNet.Build.GuidelineBuilder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadGuidelineBuilderTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(800.0, 200.0);

	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(12000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 12000.0));
	const FRoadSegmentId ToEast = Net->AddStraightSegment(Centre, East,  Profile);
	Net->AddStraightSegment(Centre, North, Profile);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node solved"), Solved.FailedNodes, 0);

	FRoadGuidelineBuilder::Build(*Net, Solved);

	// One edge per segment per declared guideline. The taxiway profile declares one, and
	// there are two segments, so exactly two edges carry a DerivedFrom naming a segment.
	{
		int32 SegmentEdges = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.DerivedFrom.IsSet())
			{
				++SegmentEdges;
			}
		}
		TestEqual(TEXT("one guideline edge per segment"), SegmentEdges, 2);
	}

	// The edge must inherit the profile's access and direction, or the guideline exists
	// and admits nobody - which looks like a pathfinding bug, not a derivation bug.
	{
		bool bFound = false;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (!Edge.bAlive || Edge.DerivedFrom != ToEast)
			{
				continue;
			}
			bFound = true;
			TestTrue(TEXT("the derived edge admits aircraft"),
				Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft));
			TestFalse(TEXT("and not pedestrians"),
				Edge.AllowedTraffic.Allows(ETraversalClass::Pedestrian));
			TestEqual(TEXT("bidirectional, as the profile declares"),
				Edge.Direction, EGuidelineDir::Bidirectional);
			TestTrue(TEXT("and is marked derived"), Edge.bDerived);
		}
		TestTrue(TEXT("the east segment produced an edge"), bFound);
	}

	// Endpoints sit ON the segment's stored cut lines, not at its node positions. A
	// guideline that ran node-to-node would overlap the junction it should hand off to.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);
		if (!TestNotNull(TEXT("east segment resolves"), Seg))
		{
			return false;
		}

		const FVector2D ExpectedA = FMath::Lerp(Seg->RightCutA, Seg->LeftCutA, 0.5);

		bool bOnCutLine = false;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (!Edge.bAlive || Edge.DerivedFrom != ToEast)
			{
				continue;
			}
			const FGuidelineNode* NodeA = Net->GetGuidelineNode(Edge.A);
			const FGuidelineNode* NodeB = Net->GetGuidelineNode(Edge.B);
			if (NodeA != nullptr && NodeB != nullptr)
			{
				bOnCutLine =
					NodeA->Position.Equals(ExpectedA, 0.01) ||
					NodeB->Position.Equals(ExpectedA, 0.01);
			}
		}
		TestTrue(TEXT("an endpoint sits on the A-end cut line"), bOnCutLine);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
