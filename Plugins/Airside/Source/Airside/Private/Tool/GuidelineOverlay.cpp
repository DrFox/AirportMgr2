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

	// Hold bars, AFTER the node loop so a bar draws over the dot it sits on rather than
	// under it. Drawn HERE and not in FHoldShortTool for the same reason the graph is: a
	// bar is a standing fact about the airport, so it must be visible under every tool -
	// one you can see only while the hold-short tool is selected is one you forget you
	// placed, and a stop nobody expected then looks like a bug in the traffic model.
	const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FGuidelineNode& Node = Nodes[Index];
		if (!Node.bAlive || !Node.HoldShortFor.IsSet())
		{
			continue;
		}

		// By HANDLE, so "which end of this edge am I" is answered by identity rather than
		// by comparing positions - two coincident nodes are legal in this graph.
		const FGuidelineNodeId Self = Network.GuidelineNodeIdAt(Index);

		// The direction of TRAVEL, not of the bar: CrossMark draws perpendicular to what it
		// is given (see ARoadBuildHUD::CrossMark), which is what puts a hold bar across the
		// taxiway rather than along it. Taken from the first incident edge, since a bar sits
		// at a taxiway end and every edge there runs the way the aircraft is going.
		FVector2D Along(1.0, 0.0);
		for (const FGuidelineEdgeId& Incident : Node.Incident)
		{
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Incident);
			if (Edge == nullptr)
			{
				continue;
			}

			const FGuidelineNode* Other = Network.GetGuidelineNode(Edge->A == Self ? Edge->B : Edge->A);
			if (Other == nullptr)
			{
				continue;
			}

			// Normalize leaves Along untouched and returns false on a degenerate edge, so
			// the (1,0) fallback survives rather than becoming a zero vector the HUD would
			// then discard - honour the return, per CLAUDE.md.
			FVector2D Direction = Other->Position - Node.Position;
			if (Direction.Normalize())
			{
				Along = Direction;
				break;
			}
		}

		Sink.CrossMark(Node.Position, Along, EPreviewStyle::HoldShort);
	}
}
