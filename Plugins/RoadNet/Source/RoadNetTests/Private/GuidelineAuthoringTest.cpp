#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.
	URoadNetwork* AuthoringFixture()
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Profile = URoadProfile::MakeTransient(200.0, 100.0);

		// Two roads meeting at a corner, plus a separate road with no connection to them.
		// The second is what a hand-drawn link would exist to join.
		const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East   = Net->AddNode(FVector2D(6000.0, 0.0));
		const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 6000.0));
		Net->AddStraightSegment(Centre, East,  Profile);
		Net->AddStraightSegment(Centre, North, Profile);

		const FRoadNodeId FarA = Net->AddNode(FVector2D(20000.0, 0.0));
		const FRoadNodeId FarB = Net->AddNode(FVector2D(26000.0, 0.0));
		Net->AddStraightSegment(FarA, FarB, Profile);

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
		FRoadGuidelineBuilder::Build(*Net, Solved);
		return Net;
	}

	/** Is this node incident to any edge the BUILDER owns? */
	bool TouchesDerivedEdge(const URoadNetwork& Network, FGuidelineNodeId Node)
	{
		const FGuidelineNode* Found = Network.GetGuidelineNode(Node);
		if (Found == nullptr)
		{
			return false;
		}

		for (const FGuidelineEdgeId& Incident : Found->Incident)
		{
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Incident);
			if (Edge != nullptr && Edge->bAlive && Edge->bDerived)
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuidelineAuthoringTest,
	"RoadNet.Model.GuidelineAuthoring",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuidelineAuthoringTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = AuthoringFixture();
	if (!TestNotNull(TEXT("fixture built"), Net))
	{
		return false;
	}

	// Two derived nodes on OPPOSITE sides of the gap - the pair a player would draw between
	// to join a stranded road to the network.
	FGuidelineNodeId Near;
	FGuidelineNodeId Far;
	{
		double BestNear = TNumericLimits<double>::Max();
		double BestFar = TNumericLimits<double>::Max();

		const TArray<FGuidelineNode>& Nodes = Net->GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (!Nodes[Index].bAlive)
			{
				continue;
			}

			FGuidelineNodeId Id;
			Id.Index = Index;
			Id.Generation = Nodes[Index].Generation;

			// Nearest to the corner, and nearest to the stranded road's near end.
			const double ToCorner = FVector2D::Distance(Nodes[Index].Position, FVector2D(6000.0, 0.0));
			const double ToFar = FVector2D::Distance(Nodes[Index].Position, FVector2D(20000.0, 0.0));

			if (ToCorner < BestNear) { BestNear = ToCorner; Near = Id; }
			if (ToFar < BestFar)     { BestFar = ToFar;     Far = Id; }
		}
	}

	if (!TestTrue(TEXT("found two derived nodes to join"), Near.IsSet() && Far.IsSet() && Near != Far))
	{
		return false;
	}

	// Both must start out attached to the graph, or the assertion after the rebuild proves
	// nothing - it could pass on a pair that was never connected.
	TestTrue(TEXT("the near node starts attached to derived geometry"),
		TouchesDerivedEdge(*Net, Near));
	TestTrue(TEXT("the far node starts attached to derived geometry"),
		TouchesDerivedEdge(*Net, Far));

	// The hand-authored link, exactly as a tool would make it.
	FGuidelineEdge Hand;
	Hand.A = Near;
	Hand.B = Far;
	Hand.Control = (Net->GetGuidelineNode(Near)->Position
		+ Net->GetGuidelineNode(Far)->Position) * 0.5;
	Hand.AllowedTraffic = FTrafficMask::Only(ETraversalClass::Aircraft);
	Hand.Direction = EGuidelineDir::Bidirectional;
	Hand.bDerived = false;

	// What the endpoints ARE, copied from the clicked nodes exactly as the tool does. A
	// handle alone would not survive the rebuild below - that is the whole defect.
	Hand.EndRefA = Net->GetGuidelineNode(Near)->Origin;
	Hand.EndRefB = Net->GetGuidelineNode(Far)->Origin;

	TestTrue(TEXT("the clicked nodes know which segment end they are"),
		Hand.EndRefA.IsSet() && Hand.EndRefB.IsSet());

	const FGuidelineEdgeId HandId = Net->AddGuidelineEdge(MoveTemp(Hand));
	if (!TestTrue(TEXT("the hand-authored edge was added"), HandId.IsSet()))
	{
		return false;
	}

	// Now the thing that happens on every subsequent road edit - TWICE.
	//
	// Twice, because a re-resolution that worked once and then left a stale EndRef behind
	// would pass a single-rebuild test and fail on the edit after that.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const FRoadSolveResult Again = FRoadNetworkSolver::SolveAll(*Net);
		FRoadGuidelineBuilder::Build(*Net, Again);
	}

	// The edge itself survives - this much the builder does promise.
	const FGuidelineEdge* Survivor = Net->GetGuidelineEdge(HandId);
	TestTrue(TEXT("the player's edge survives the rebuild"),
		Survivor != nullptr && Survivor->bAlive);

	if (Survivor == nullptr || !Survivor->bAlive)
	{
		return false;
	}

	// THE QUESTION. Surviving is not the same as staying CONNECTED. If re-derivation added
	// fresh coincident nodes and attached its edges to those instead, this link is a stub
	// joined to nothing - it draws correctly, routes nothing, and breaks on an edit that
	// had nothing to do with it.
	TestTrue(TEXT("the link's near end is still attached to the derived graph"),
		TouchesDerivedEdge(*Net, Survivor->A));
	TestTrue(TEXT("the link's far end is still attached to the derived graph"),
		TouchesDerivedEdge(*Net, Survivor->B));

	// How many alive nodes now sit on top of each other. Coincident duplicates are the
	// mechanism, so measure it rather than inferring it from the failure above.
	{
		int32 Coincident = 0;
		const TArray<FGuidelineNode>& Nodes = Net->GetGuidelineNodes();
		for (int32 A = 0; A < Nodes.Num(); ++A)
		{
			if (!Nodes[A].bAlive)
			{
				continue;
			}
			for (int32 B = A + 1; B < Nodes.Num(); ++B)
			{
				if (Nodes[B].bAlive && Nodes[A].Position == Nodes[B].Position)
				{
					++Coincident;
				}
			}
		}

		TestEqual(TEXT("no two alive guideline nodes share a position"), Coincident, 0);
	}

	return true;
}

#endif
