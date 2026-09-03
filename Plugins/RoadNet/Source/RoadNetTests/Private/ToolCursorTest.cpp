#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadSnap.h"
#include "Tool/RouteTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build: these test files share one translation unit, so a
	// second anonymous-namespace FCountingSink collides with RoadDrawToolTest's.
	struct FCursorPreviewSink : public IToolPreviewSink
	{
		TMap<EPreviewStyle, int32> Markers;

		virtual void Marker(const FVector2D& At, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void Label(const FVector2D&, const FString&, EPreviewStyle) override {}

		int32 Count(EPreviewStyle Style) const
		{
			const int32* Found = Markers.Find(Style);
			return Found != nullptr ? *Found : 0;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToolCursorTest,
	"RoadNet.Tool.CursorContract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToolCursorTest::RunTest(const FString& Parameters)
{
	// 1. SetCursor keeps the two answers apart. A driver that assigned them by hand could
	//    fold one into the other, and one of the two drivers did.
	{
		FRoadSnapResult Snapped;
		Snapped.Kind = ERoadSnapKind::Node;
		Snapped.Position = FVector2D(1000.0, 0.0);

		FToolContext Context;
		Context.SetCursor(FVector2D(0.0, 0.0), Snapped);

		TestTrue(TEXT("the cursor stays where the mouse is"),
			Context.Cursor == FVector2D(0.0, 0.0));
		TestTrue(TEXT("and the snap keeps its own, different answer"),
			Context.Snap.Position == FVector2D(1000.0, 0.0));
	}

	// 2. THE REGRESSION. Hovering a guideline node beside a junction must highlight it,
	//    even though the road snap claims that whole neighbourhood for the junction.
	//
	//    A guideline node sits on the segment's CUT LINE, CutDistance from the road node.
	//    A junction's snap reach is CutDistance + HalfWidth, strictly further - so the
	//    guideline node is ALWAYS inside the road node's reach, and a cursor folded
	//    through the snap always lands CutDistance away from the thing being hovered.
	//    With SnapRadius smaller than CutDistance that is never a hit, which is why every
	//    junction-adjacent node went dead at once rather than intermittently.
	{
		ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
		if (!TestNotNull(TEXT("actor constructed"), Actor))
		{
			return false;
		}

		// A corner, through the facade, so the network is built the way the tool builds it.
		const int32 Centre = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 East   = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		const int32 North  = Actor->PlaceNode(FVector2D(0.0, 6000.0));
		Actor->ConnectNodes(Centre, East);
		Actor->ConnectNodes(Centre, North);

		if (!TestNotNull(TEXT("the facade made a network"), Actor->Network.Get()))
		{
			return false;
		}

		// Solve and derive the guidelines, which is what RebuildMesh does before any of
		// this is hoverable. Done directly so the test needs no world or component.
		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Actor->Network);
		FRoadGuidelineBuilder::Build(*Actor->Network, Solved);

		// The guideline node nearest the junction, which is the one the player reaches for.
		const TArray<FGuidelineNode>& Guidelines = Actor->Network->GetGuidelineNodes();
		FVector2D Hover = FVector2D::ZeroVector;
		double Nearest = TNumericLimits<double>::Max();
		for (const FGuidelineNode& Node : Guidelines)
		{
			if (!Node.bAlive)
			{
				continue;
			}
			const double Distance = Node.Position.Size();
			if (Distance > 1.0 && Distance < Nearest)
			{
				Nearest = Distance;
				Hover = Node.Position;
			}
		}

		if (!TestTrue(TEXT("the network derived some guideline nodes to hover"), Nearest < 1e8))
		{
			return false;
		}

		FRoadSnapSettings Settings;
		Settings.NodeRadius = 150.0;
		Settings.SegmentRadius = 150.0;
		Settings.JunctionSnapFactor = 1.0;

		const FRoadSnapChain Chain;
		const FRoadSnapResult Snapped = Chain.Resolve(*Actor->Network, Hover, Settings);

		// The fixture is only meaningful if the road snap really does claim this point -
		// otherwise the assertion below could pass without exercising anything.
		TestTrue(TEXT("the junction really does claim the guideline node's neighbourhood"),
			Snapped.Kind == ERoadSnapKind::Node);

		FRouteTool Tool;

		// Built the correct way: the raw hover point, with the snap carried beside it.
		{
			FToolContext Context;
			Context.Target = Actor;
			Context.SnapRadius = 150.0;
			Context.SetCursor(Hover, Snapped);

			FCursorPreviewSink Sink;
			Tool.BuildPreview(Context, Sink);

			TestTrue(TEXT("hovering a guideline node highlights it"),
				Sink.Count(EPreviewStyle::Snap) > 0);
		}

		// Built the way the runtime driver used to: cursor folded through the snap. This
		// is the bug, asserted so the fix cannot be undone without a test going red.
		{
			FToolContext Context;
			Context.Target = Actor;
			Context.SnapRadius = 150.0;
			Context.Cursor = Snapped.Position;
			Context.Snap = Snapped;

			FCursorPreviewSink Sink;
			Tool.BuildPreview(Context, Sink);

			TestEqual(TEXT("folding the cursor through the snap highlights nothing at all"),
				Sink.Count(EPreviewStyle::Snap), 0);
		}
	}

	return true;
}

#endif
