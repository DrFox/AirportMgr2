#include "Tool/GuidelineOverlay.h"

#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"
#include "Tool/RoadBuildTool.h"

void GuidelineOverlay::Draw(const URoadNetwork& Network, IToolPreviewSink& Sink)
{
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

		// The same sampling the search costed and a follower will walk, so what is drawn is
		// what is driven. Do not add a second evaluator here - see the header.
		TArray<FVector2D> Points;
		GuidelineGeom::Sample(A->Position, Edge.Control, B->Position, Points);

		for (int32 At = 1; At < Points.Num(); ++At)
		{
			Sink.Line(Points[At - 1], Points[At], EPreviewStyle::Guideline);
		}
	}

	for (const FGuidelineNode& Node : Network.GetGuidelineNodes())
	{
		if (Node.bAlive)
		{
			Sink.Marker(Node.Position, EPreviewStyle::Guideline);
		}
	}
}
