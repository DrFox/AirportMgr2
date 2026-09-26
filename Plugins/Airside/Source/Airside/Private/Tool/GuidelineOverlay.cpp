#include "Tool/GuidelineOverlay.h"

#include "Model/HoldingBarFrame.h"
#include "Model/RoadNetwork.h"
#include "Tool/RoadBuildTool.h"

void GuidelineOverlay::Draw(const URoadNetwork& Network, IToolPreviewSink& Sink)
{
	// HOISTED OUT OF THE LOOP - #183. This ran once per DrawHUD/Render call before, but a fresh
	// TArray PER EDGE meant a heap allocation per edge every time; Reset() below keeps whatever
	// capacity the first edge grew and just drops the count, so only the first edge on the field
	// ever grows the buffer.
	TArray<FVector2D> Points;

	const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
	for (int32 Index = 0; Index < Edges.Num(); ++Index)
	{
		if (!Edges[Index].bAlive)
		{
			continue;
		}

		// GuidelineGeom::Sample APPENDS rather than clearing - see its own header - so this
		// Reset is what stops one edge's points bleeding into the next's polyline.
		Points.Reset();

		// The same sampling the search costed and a follower will walk, so what is drawn is
		// what is driven. Do not add a second evaluator here - see the header.
		if (!Network.SampleGuideline(Network.GuidelineEdgeIdAt(Index), Points))
		{
			continue;
		}

		// A REVERSE LEG IN ITS OWN CONTEXT STYLE, so the graph shows where reverse turns are
		// before anything backs along one (spec 2026-09-26 §5).
		Sink.Polyline(Points, Edges[Index].bReverseLeg ? EPreviewStyle::ReverseGuideline : EPreviewStyle::Guideline);
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

		// THE ONE EVALUATOR (issue #307). This used to derive its own "Along" from the
		// guideline EDGE's chord to whichever node sat on the far end - hunting Node.Incident
		// for one whose DerivedFrom matched Origin.Segment, falling back to the first usable
		// edge otherwise - while FHoldingPositionMarkingBuilder derived its own "Toward" from
		// the ROAD SEGMENT's tangent at Origin. A comment on the builder once claimed the two
		// were "exactly" the same fallback. They were not: a chord and a tangent agree only
		// on a straight, centred guideline, which is what every fixture drawn before #307
		// happened to be, and diverge the moment the taxiway curves or its end is set back
		// from the node along its own tangent (RoadGuidelineBuilder.cpp's SetBack, ~line
		// 446). HoldingBarAt is now the only place either question is answered - see its own
		// comment for the two branches (Origin's segment first, an incident edge otherwise)
		// this loop used to duplicate, differently, from the builder.
		// ENFORCED BY: Airside.Model.HoldingBarFrame, which draws this node's cross mark and
		// asserts its Along is bitwise HoldingBarAt's Toward - red the day this loop, or the
		// builder, answers the question a second way again.
		const FHoldingBarFrame Frame = HoldingBarAt(Network, Self);
		if (!Frame.IsSet())
		{
			// An isolated node has no bar to be across - the same skip
			// FHoldingPositionMarkingBuilder::Build takes. This IS a behaviour change: before
			// #307, a flagged node with no Origin and no usable incident edge still drew a
			// cross mark facing an arbitrary default (1, 0), because Along only ever grew a
			// value, never a "nothing to draw" signal. Nothing reachable through the tools
			// that flag a node (FRoadGuidelineBuilder always gives a derived one an Origin;
			// FHoldingPointTool only flags a node already on the graph) can produce one with
			// zero incident edges, so this is believed unobservable in practice - noted here
			// because CLAUDE.md's refactor contract asks for the diff to be owned, not missed.
			continue;
		}

		// The style names the KIND: the runway pattern (two solid, two dashed) and the
		// intermediate one (a single dashed line) are different markings on the ground.
		// CrossMark draws perpendicular to what it is given (see ARoadBuildHUD::CrossMark),
		// which is what puts a hold bar across the taxiway rather than along it - Frame.Toward
		// is the direction of TRAVEL, not of the bar itself.
		Sink.CrossMark(Node.Position, Frame.Toward,
			Node.HoldingPosition == EHoldingPositionKind::Runway
				? EPreviewStyle::RunwayHoldingPosition
				: EPreviewStyle::IntermediateHoldingPosition);
	}
}
