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
	// under it. Drawn HERE and not in FHoldingPointTool for the same reason the graph is: a
	// bar is a standing fact about the airport, so it must be visible under every tool -
	// one you can see only while the holding-position tool is selected is one you forget you
	// placed, and a stop nobody expected then looks like a bug in the traffic model.
	const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FGuidelineNode& Node = Nodes[Index];
		if (!Node.bAlive || Node.HoldingPosition == EHoldingPositionKind::None)
		{
			continue;
		}

		// By HANDLE, so "which end of this edge am I" is answered by identity rather than
		// by comparing positions - two coincident nodes are legal in this graph.
		const FGuidelineNodeId Self = Network.GuidelineNodeIdAt(Index);

		// The direction of TRAVEL, not of the bar: CrossMark draws perpendicular to what it
		// is given (see ARoadBuildHUD::CrossMark), which is what puts a hold bar across the
		// taxiway rather than along it.
		//
		// THE NODE'S OWN SEGMENT FIRST, and only then whatever else is incident. A bar sits
		// at a taxiway end AT A JUNCTION, so the node is usually joined to the runway's
		// centreline by turn paths as well - and those run every which way. Taking
		// Incident[0] would let the bar's angle depend on the order edges happened to be
		// added, which is an implementation detail of the derivation and not a contract:
		// the same airport rebuilt could rotate the bar. Origin.Segment names the road this
		// node was derived FOR, which is the taxiway the aircraft is actually travelling
		// along, so matching DerivedFrom against it picks the one edge whose direction the
		// bar should be square to. Incident[0] remains the fallback for a node with no
		// Origin (an anchor or pose node), which has no such segment to prefer.
		FVector2D Along(1.0, 0.0);

		// An explicit flag, not "Along still equals (1,0)": a real edge can point exactly
		// along +X, and a sentinel a legitimate value can collide with is not a sentinel.
		bool bHaveAny = false;
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

			// Normalize leaves Direction untouched and returns false on a degenerate edge,
			// so a zero-length edge is skipped rather than adopted as a zero vector the HUD
			// would then discard - honour the return, per CLAUDE.md.
			FVector2D Direction = Other->Position - Node.Position;
			if (!Direction.Normalize())
			{
				continue;
			}

			if (Node.Origin.IsSet() && Edge->DerivedFrom == Node.Origin.Segment)
			{
				Along = Direction;
				break;
			}

			// Keep the FIRST usable edge as the fallback, but go on looking for the origin
			// one - breaking here is what would have made insertion order the answer.
			if (!bHaveAny)
			{
				Along = Direction;
				bHaveAny = true;
			}
		}

		// The style names the KIND: the runway pattern (two solid, two dashed) and the
		// intermediate one (a single dashed line) are different markings on the ground.
		Sink.CrossMark(Node.Position, Along,
			Node.HoldingPosition == EHoldingPositionKind::Runway
				? EPreviewStyle::RunwayHoldingPosition
				: EPreviewStyle::IntermediateHoldingPosition);
	}
}
