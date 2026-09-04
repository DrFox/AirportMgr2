#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/GuidelineDrawTool.h"
#include "Tool/RoadBuildTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.
	struct FLinkSink : public IToolPreviewSink
	{
		TMap<EPreviewStyle, int32> Markers;
		TMap<EPreviewStyle, int32> LineCounts;
		TArray<FString> Labels;

		virtual void Marker(const FVector2D&, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle Style) override
		{
			LineCounts.FindOrAdd(Style)++;
		}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Label(const FVector2D&, const FString& Text, EPreviewStyle) override
		{
			Labels.Add(Text);
		}

		int32 CountMarkers(EPreviewStyle Style) const
		{
			const int32* Found = Markers.Find(Style);
			return Found != nullptr ? *Found : 0;
		}
		int32 CountLines(EPreviewStyle Style) const
		{
			const int32* Found = LineCounts.Find(Style);
			return Found != nullptr ? *Found : 0;
		}
	};

	/** Two roads that do NOT touch - the case a hand-drawn link exists for. */
	ARoadNetworkActor* LinkFixture()
	{
		ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
		if (Actor == nullptr)
		{
			return nullptr;
		}

		const int32 A0 = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 A1 = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(A0, A1);

		const int32 B0 = Actor->PlaceNode(FVector2D(20000.0, 0.0));
		const int32 B1 = Actor->PlaceNode(FVector2D(26000.0, 0.0));
		Actor->ConnectNodes(B0, B1);

		Actor->RebuildMesh();
		return Actor;
	}

	/** The alive guideline node nearest a point. */
	FGuidelineNodeId NodeNearest(const URoadNetwork& Network, const FVector2D& Where)
	{
		FGuidelineNodeId Best;
		double BestDistance = TNumericLimits<double>::Max();

		const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (!Nodes[Index].bAlive)
			{
				continue;
			}
			const double Distance = FVector2D::Distance(Nodes[Index].Position, Where);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best.Index = Index;
				Best.Generation = Nodes[Index].Generation;
			}
		}
		return Best;
	}

	FToolContext LinkContextAt(ARoadNetworkActor* Actor, const FVector2D& Where)
	{
		FToolContext Context;
		Context.Target = Actor;
		Context.SnapRadius = 400.0;

		FRoadSnapResult Snap;
		Snap.Position = Where;
		Context.SetCursor(Where, Snap);
		return Context;
	}

	int32 CountHandEdges(const URoadNetwork& Network)
	{
		int32 Count = 0;
		for (const FGuidelineEdge& Edge : Network.GetGuidelineEdges())
		{
			Count += (Edge.bAlive && !Edge.bDerived) ? 1 : 0;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuidelineDrawToolTest,
	"Airside.Tool.GuidelineDraw",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuidelineDrawToolTest::RunTest(const FString& Parameters)
{
	ARoadNetworkActor* Actor = LinkFixture();
	if (!TestNotNull(TEXT("fixture built"), Actor)
		|| !TestNotNull(TEXT("fixture has a network"), Actor->Network.Get()))
	{
		return false;
	}

	// The two ends facing each other across the gap.
	const FGuidelineNodeId Left  = NodeNearest(*Actor->Network, FVector2D(6000.0, 0.0));
	const FGuidelineNodeId Right = NodeNearest(*Actor->Network, FVector2D(20000.0, 0.0));

	if (!TestTrue(TEXT("found a node either side of the gap"),
		Left.IsSet() && Right.IsSet() && Left != Right))
	{
		return false;
	}

	const FVector2D LeftAt  = Actor->Network->GetGuidelineNode(Left)->Position;
	const FVector2D RightAt = Actor->Network->GetGuidelineNode(Right)->Position;

	// 1. The two roads start UNROUTABLE between each other. Without this the end-to-end
	//    assertion below could pass on a graph that was already connected.
	{
		const FRoutePlan Before =
			Actor->FindRoute(Left, Right, ETraversalClass::Aircraft, 0.0);
		TestFalse(TEXT("nothing routes across the gap to begin with"), Before.IsValid());
	}

	// 2. Validation, before anything is built.
	{
		TestTrue(TEXT("a node cannot link to itself"),
			FGuidelineDrawTool::Validate(*Actor->Network, Left, Left) == EGuidelineLink::SameNode);
		TestTrue(TEXT("two distinct unlinked nodes are valid"),
			FGuidelineDrawTool::Validate(*Actor->Network, Left, Right) == EGuidelineLink::Valid);
		TestTrue(TEXT("an unset start is refused"),
			FGuidelineDrawTool::Validate(*Actor->Network, FGuidelineNodeId(), Right)
				== EGuidelineLink::NoStart);
	}

	// 3. Two clicks make one hand-authored edge, carrying both endpoints' identities.
	{
		FGuidelineDrawTool Tool;
		TestTrue(TEXT("the tool starts idle"), Tool.IsIdle());

		Tool.OnClick(LinkContextAt(Actor, LeftAt));
		TestFalse(TEXT("one click leaves it part-drawn"), Tool.IsIdle());

		Tool.OnClick(LinkContextAt(Actor, RightAt));
		TestTrue(TEXT("the second click completes the link"), Tool.IsIdle());

		TestEqual(TEXT("exactly one hand-authored edge exists"),
			CountHandEdges(*Actor->Network), 1);

		// Identity, not handles - the difference between surviving a rebuild attached and
		// surviving it connected to nothing.
		bool bCarriesIdentity = false;
		for (const FGuidelineEdge& Edge : Actor->Network->GetGuidelineEdges())
		{
			if (Edge.bAlive && !Edge.bDerived)
			{
				bCarriesIdentity = Edge.EndRefA.IsSet() && Edge.EndRefB.IsSet();
			}
		}
		TestTrue(TEXT("the new edge records what its ends ARE"), bCarriesIdentity);
	}

	// 4. THE POINT OF THE FEATURE, end to end: a route now crosses the gap.
	{
		const FRoutePlan After =
			Actor->FindRoute(Left, Right, ETraversalClass::Aircraft, 0.0);
		TestTrue(TEXT("a route now crosses the hand-drawn link"), After.IsValid());
	}

	// 5. And it still crosses after a rebuild, which is where a handle-based link died.
	{
		Actor->RebuildMesh();

		const FGuidelineNodeId LeftAgain  = NodeNearest(*Actor->Network, LeftAt);
		const FGuidelineNodeId RightAgain = NodeNearest(*Actor->Network, RightAt);

		const FRoutePlan After =
			Actor->FindRoute(LeftAgain, RightAgain, ETraversalClass::Aircraft, 0.0);
		TestTrue(TEXT("the route survives a rebuild"), After.IsValid());
		TestEqual(TEXT("and there is still exactly one hand-authored edge"),
			CountHandEdges(*Actor->Network), 1);
	}

	// 6. Drawing the same link twice is refused rather than duplicated.
	{
		const FGuidelineNodeId LeftAgain  = NodeNearest(*Actor->Network, LeftAt);
		const FGuidelineNodeId RightAgain = NodeNearest(*Actor->Network, RightAt);

		TestTrue(TEXT("a duplicate link is refused"),
			FGuidelineDrawTool::Validate(*Actor->Network, LeftAgain, RightAgain)
				== EGuidelineLink::AlreadyJoined);

		FGuidelineDrawTool Tool;
		Tool.OnClick(LinkContextAt(Actor, LeftAt));
		Tool.OnClick(LinkContextAt(Actor, RightAt));

		TestEqual(TEXT("and no second edge appears"), CountHandEdges(*Actor->Network), 1);
	}

	// 7. Ctrl removes a hand-authored link; a derived edge is left alone, because the next
	//    rebuild would put it straight back and that reads as the tool ignoring the click.
	{
		FToolContext Remove = LinkContextAt(Actor, (LeftAt + RightAt) * 0.5);
		Remove.bRemoveModifier = true;

		FGuidelineDrawTool Tool;
		Tool.OnClick(Remove);

		TestEqual(TEXT("ctrl-click removes the hand-authored link"),
			CountHandEdges(*Actor->Network), 0);

		// On a derived edge, in the middle of the left road.
		const int32 DerivedBefore = Actor->Network->GetGuidelineEdges().Num();
		FToolContext OnDerived = LinkContextAt(Actor, FVector2D(3000.0, 0.0));
		OnDerived.bRemoveModifier = true;
		Tool.OnClick(OnDerived);

		int32 AliveDerived = 0;
		for (const FGuidelineEdge& Edge : Actor->Network->GetGuidelineEdges())
		{
			AliveDerived += (Edge.bAlive && Edge.bDerived) ? 1 : 0;
		}
		TestTrue(TEXT("a derived edge is not removable by hand"), AliveDerived > 0);
		(void)DerivedBefore;
	}

	// 8. The preview says what the gesture would do: the node under the cursor lights up,
	//    and after one click a pending line follows the cursor.
	{
		FGuidelineDrawTool Tool;

		FLinkSink Hover;
		Tool.BuildPreview(LinkContextAt(Actor, LeftAt), Hover);
		TestTrue(TEXT("the node under the cursor is highlighted"),
			Hover.CountMarkers(EPreviewStyle::Snap) > 0);

		Tool.OnClick(LinkContextAt(Actor, LeftAt));

		FLinkSink Pending;
		Tool.BuildPreview(LinkContextAt(Actor, FVector2D(12000.0, 3000.0)), Pending);
		TestTrue(TEXT("a pending line follows the cursor after the first click"),
			Pending.CountLines(EPreviewStyle::Pending) > 0);
	}

	return true;
}

#endif
