#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Tool/GuidelineOverlay.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RouteTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.
	struct FOverlaySink : public IToolPreviewSink
	{
		TMap<EPreviewStyle, int32> Markers;
		TMap<EPreviewStyle, int32> LineCounts;

		/** Every guideline line, in the order drawn, as (from, to) pairs. */
		TArray<TPair<FVector2D, FVector2D>> GuidelineLines;

		virtual void Marker(const FVector2D&, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) override
		{
			LineCounts.FindOrAdd(Style)++;
			if (Style == EPreviewStyle::Guideline)
			{
				GuidelineLines.Emplace(From, To);
			}
		}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle Style) override
		{
			Markers.FindOrAdd(Style)++;
		}
		virtual void Label(const FVector2D&, const FString&, EPreviewStyle) override {}

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

	/** A corner with its guidelines derived, as RebuildMesh would leave it. */
	ARoadNetworkActor* OverlayFixture()
	{
		ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
		if (Actor == nullptr)
		{
			return nullptr;
		}

		const int32 Centre = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 East   = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		const int32 North  = Actor->PlaceNode(FVector2D(0.0, 6000.0));
		Actor->ConnectNodes(Centre, East);
		Actor->ConnectNodes(Centre, North);

		if (Actor->Network == nullptr)
		{
			return Actor;
		}

		const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Actor->Network);
		FRoadGuidelineBuilder::Build(*Actor->Network, Solved);
		return Actor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuidelineOverlayTest,
	"RoadNet.Tool.GuidelineOverlay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuidelineOverlayTest::RunTest(const FString& Parameters)
{
	ARoadNetworkActor* Actor = OverlayFixture();
	if (!TestNotNull(TEXT("fixture actor built"), Actor)
		|| !TestNotNull(TEXT("fixture has a network"), Actor->Network.Get()))
	{
		return false;
	}

	const URoadNetwork& Network = *Actor->Network;

	// Count what the graph actually holds, so every assertion below is against the model
	// rather than against a number typed here that would drift the first time the builder
	// changed.
	int32 AliveNodes = 0;
	for (const FGuidelineNode& Node : Network.GetGuidelineNodes())
	{
		AliveNodes += Node.bAlive ? 1 : 0;
	}

	int32 AliveEdges = 0;
	for (const FGuidelineEdge& Edge : Network.GetGuidelineEdges())
	{
		AliveEdges += Edge.bAlive ? 1 : 0;
	}

	if (!TestTrue(TEXT("the fixture derived a graph to draw"), AliveNodes > 0 && AliveEdges > 0))
	{
		return false;
	}

	// 1. Every alive node gets a marker, and every alive edge gets drawn.
	{
		FOverlaySink Sink;
		GuidelineOverlay::Draw(Network, Sink);

		TestEqual(TEXT("one marker per alive guideline node"),
			Sink.CountMarkers(EPreviewStyle::Guideline), AliveNodes);
		TestTrue(TEXT("the edges are drawn as guideline lines"),
			Sink.CountLines(EPreviewStyle::Guideline) > 0);
	}

	// 2. THE SINGLE-SAMPLING CONTRACT, MEASURED. The polyline drawn for an edge must be
	//    vertex-for-vertex what GuidelineGeom::Sample returns.
	//
	//    "Some lines were drawn" would pass on an overlay that had grown its own sampler,
	//    which is precisely the failure the contract exists to prevent: a second evaluator
	//    diverges only on bends, so a cube leaves the line the player was shown and
	//    everything looks right on the straights.
	{
		FOverlaySink Sink;
		GuidelineOverlay::Draw(Network, Sink);

		TArray<TPair<FVector2D, FVector2D>> Expected;
		for (const FGuidelineEdge& Edge : Network.GetGuidelineEdges())
		{
			if (!Edge.bAlive)
			{
				continue;
			}

			const FGuidelineNode* A = Network.GetGuidelineNode(Edge.A);
			const FGuidelineNode* B = Network.GetGuidelineNode(Edge.B);
			if (A == nullptr || B == nullptr)
			{
				continue;
			}

			TArray<FVector2D> Points;
			GuidelineGeom::Sample(A->Position, Edge.Control, B->Position, Points);
			for (int32 At = 1; At < Points.Num(); ++At)
			{
				Expected.Emplace(Points[At - 1], Points[At]);
			}
		}

		if (TestEqual(TEXT("one drawn line per sampled span, and no more"),
			Sink.GuidelineLines.Num(), Expected.Num()))
		{
			int32 Mismatched = 0;
			for (int32 At = 0; At < Expected.Num(); ++At)
			{
				if (Sink.GuidelineLines[At].Key != Expected[At].Key
					|| Sink.GuidelineLines[At].Value != Expected[At].Value)
				{
					++Mismatched;
				}
			}

			TestEqual(TEXT("every drawn vertex is Sample's own, not a second sampler's"),
				Mismatched, 0);
		}
	}

	// 3. A dead edge is not drawn. Deleting a road must take its guideline off the screen,
	//    or the overlay shows routes that no longer exist.
	{
		const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
		FGuidelineEdgeId Doomed;
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			if (Edges[Index].bAlive)
			{
				Doomed.Index = Index;
				Doomed.Generation = Edges[Index].Generation;
				break;
			}
		}

		FOverlaySink Before;
		GuidelineOverlay::Draw(Network, Before);

		Actor->Network->RemoveGuidelineEdge(Doomed);

		FOverlaySink After;
		GuidelineOverlay::Draw(Network, After);

		TestTrue(TEXT("removing an edge draws fewer guideline lines"),
			After.CountLines(EPreviewStyle::Guideline)
				< Before.CountLines(EPreviewStyle::Guideline));
	}

	// 4. ONE EMITTER. The route tool must no longer draw the graph itself, or selecting it
	//    draws every edge twice - and the overlay stops being the single place that decides
	//    what the routing graph looks like.
	{
		ARoadNetworkActor* Fresh = OverlayFixture();
		if (TestNotNull(TEXT("second fixture built"), Fresh))
		{
			FToolContext Context;
			Context.Target = Fresh;
			Context.SnapRadius = 150.0;

			FRoadSnapResult NoSnap;
			NoSnap.Position = FVector2D(50000.0, 50000.0);   // far from anything
			Context.SetCursor(NoSnap.Position, NoSnap);

			FRouteTool Tool;
			FOverlaySink Sink;
			Tool.BuildPreview(Context, Sink);

			TestEqual(TEXT("the route tool draws no guideline lines of its own"),
				Sink.CountLines(EPreviewStyle::Guideline), 0);
			TestEqual(TEXT("nor guideline node markers"),
				Sink.CountMarkers(EPreviewStyle::Guideline), 0);
		}
	}

	// 5. But the route tool keeps its OWN preview. Extracting the graph must not have taken
	//    the hover highlight with it - that is the thing the player aims at.
	{
		ARoadNetworkActor* Fresh = OverlayFixture();
		if (TestNotNull(TEXT("third fixture built"), Fresh) && Fresh->Network != nullptr)
		{
			FVector2D Hover = FVector2D::ZeroVector;
			double Nearest = TNumericLimits<double>::Max();
			for (const FGuidelineNode& Node : Fresh->Network->GetGuidelineNodes())
			{
				if (Node.bAlive && Node.Position.Size() > 1.0 && Node.Position.Size() < Nearest)
				{
					Nearest = Node.Position.Size();
					Hover = Node.Position;
				}
			}

			FToolContext Context;
			Context.Target = Fresh;
			Context.SnapRadius = 150.0;

			FRoadSnapResult AtNode;
			AtNode.Position = Hover;
			Context.SetCursor(Hover, AtNode);

			FRouteTool Tool;
			FOverlaySink Sink;
			Tool.BuildPreview(Context, Sink);

			TestTrue(TEXT("hovering a guideline node still highlights it"),
				Sink.CountMarkers(EPreviewStyle::Snap) > 0);
		}
	}

	return true;
}

#endif
