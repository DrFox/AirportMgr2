#include "Build/HoldingPositionMarkingBuilder.h"

#include "AirsideLog.h"

#include "Build/MarkingQuads.h"
#include "Model/HoldingBarFrame.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"

namespace
{
	/**
	 * One bar across the taxiway: solid, or dashed from one edge. Toward is the unit
	 * direction from the node INTO the junction (the side the pattern sits on), Across its
	 * left-hand perpendicular. Near and Far are distances from the node along Toward.
	 */
	void MarkingAddBar(FRoadMeshBuffers& Out, double Z, const FVector2D& Node,
		const FVector2D& Toward, const FVector2D& Across, double HalfWidth,
		double Near, double Far, bool bDashed)
	{
		const FVector2D NearLine = Node + Toward * Near;
		const FVector2D FarLine = Node + Toward * Far;
		if (!bDashed)
		{
			MarkingQuads::AddQuad(Out, Z,
				NearLine - Across * HalfWidth, NearLine + Across * HalfWidth,
				FarLine + Across * HalfWidth, FarLine - Across * HalfWidth);
			return;
		}
		const double Period = FHoldingPositionMarkingBuilder::DashLength + FHoldingPositionMarkingBuilder::DashGap;
		for (double Start = -HalfWidth; Start < HalfWidth; Start += Period)
		{
			const double End = FMath::Min(Start + FHoldingPositionMarkingBuilder::DashLength, HalfWidth);
			MarkingQuads::AddQuad(Out, Z,
				NearLine + Across * Start, NearLine + Across * End,
				FarLine + Across * End, FarLine + Across * Start);
		}
	}
}

int32 FHoldingPositionMarkingBuilder::Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out)
{
	int32 Painted = 0;
	const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FGuidelineNode& Node = Nodes[Index];
		if (!Node.bAlive || Node.HoldingPosition == EHoldingPositionKind::None)
		{
			continue;
		}

		// THE DIRECTION INTO THE JUNCTION, and the taxiway's width - THE ONE EVALUATOR
		// (issue #307). This used to derive Toward/Across itself (Origin's segment tangent
		// first, an incident edge otherwise) and so, separately, did GuidelineOverlay for its
		// cross-mark's Along - a comment here once claimed the two fallbacks were "exactly"
		// the same, which held only for a straight, centred guideline and broke the moment
		// one curved or its end was set back from the node (RoadGuidelineBuilder.cpp's
		// SetBack). HoldingBarAt is now the only place either question is answered.
		// ENFORCED BY: Airside.Model.HoldingBarFrame, which builds this node's bar and asserts
		// its Toward/Across/HalfWidth are square to and no wider than HoldingBarAt's own
		// frame - red the day this builder, or the overlay, answers the question a second way.
		// The LineWidth floor stays HERE, not there: it exists so a quad never degenerates to
		// zero thickness when a profile or edge declares no width, which is this builder's
		// problem alone - the overlay draws no thickness at all (RoadBuildTool.h).
		const FGuidelineNodeId NodeId = Network.GuidelineNodeIdAt(Index);
		const FHoldingBarFrame Frame = HoldingBarAt(Network, NodeId);
		if (!Frame.IsSet())
		{
			continue;   // an isolated node has no bar to be across
		}
		const FVector2D Toward = Frame.Toward;
		const FVector2D Across = Frame.Across;
		const double HalfWidth = FMath::Max(Frame.HalfWidth, LineWidth);

		// WHAT THIS BAR IS ABOUT TO BE, before it is built. Reported from play as "the hold
		// line doesnt cover the whol of the taxiway" (samples/issues.png), and five
		// synthetic fixtures failed to reproduce it - each one put the bar somewhere its own
		// arm really was all the asphalt there was. Rather than invent a sixth, this says
		// where the bar stands and how wide it was told to be, so ONE pass over the level
		// that shows the fault answers it.
		//
		// A LOG LINE IS A FEATURE (CLAUDE.md, "Diagnosing"), and this one is cheap: a
		// handful of holding positions per airport, once per surface rebuild.
		UE_LOG(LogAirside, Log,
			TEXT("HoldBar: node %d at (%.0f, %.0f), %s, half width %.0f, across (%.2f, %.2f), "
				 "origin segment %d"),
			Index, Node.Position.X, Node.Position.Y,
			Node.HoldingPosition == EHoldingPositionKind::Runway ? TEXT("runway") : TEXT("intermediate"),
			HalfWidth, Across.X, Across.Y,
			Node.Origin.IsSet() ? Node.Origin.Segment.Index : -1);

		if (Node.HoldingPosition == EHoldingPositionKind::Runway)
		{
			// Nearest the aircraft first: two solid, then two dashed, each LineWidth deep
			// with LineGap between. The nose stopped at the node is on the first line's edge.
			double At = 0.0;
			for (int32 Line = 0; Line < 4; ++Line)
			{
				MarkingAddBar(Out, Z, Node.Position, Toward, Across, HalfWidth, At, At + LineWidth, /*bDashed=*/Line >= 2);
				At += LineWidth + LineGap;
			}
		}
		else
		{
			MarkingAddBar(Out, Z, Node.Position, Toward, Across, HalfWidth, 0.0, LineWidth, /*bDashed=*/true);
		}
		++Painted;
	}
	return Painted;
}
