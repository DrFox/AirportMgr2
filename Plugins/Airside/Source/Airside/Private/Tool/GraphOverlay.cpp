#include "Tool/GraphOverlay.h"

#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/StandPreview.h"

void GraphOverlay::DescribeNodes(const URoadNetwork& Network, IToolPreviewSink& Sink)
{
	// Degree is the whole point of drawing these: it is what separates a junction from a
	// straight-through node, and the pavement looks identical either way.
	for (const FRoadNode& Node : Network.GetNodes())
	{
		if (!Node.bAlive)
		{
			continue;
		}

		const int32 Degree = Node.Incident.Num();
		const EPreviewStyle Style = (Degree == 0) ? EPreviewStyle::NodeStub
			: (Degree >= 3) ? EPreviewStyle::NodeJunction
			: EPreviewStyle::NodeThrough;

		Sink.Marker(Node.Position, Style);
	}
}

void GraphOverlay::DescribeStands(const URoadNetwork& Network, IToolPreviewSink& Sink)
{
	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		if (!Entity.bAlive)
		{
			continue;
		}

		// The full description FIRST - the SAME call a placement tool's own preview makes,
		// so a placed stand and an aimed one read as one object. See StandPreview.h for why
		// this used to be two.
		//
		// The footprint matters because a stand aimed 180 degrees out looks identical to a
		// correct one until something tries to taxi onto it - the heading has to be
		// unmistakable, which is the whole reason StandPreview draws the aircraft rather
		// than a single point at the stop. Its service points are recomputed on every call
		// rather than stored, because they belong to whatever is PARKED here - today the
		// type the stand was sized for, tomorrow whatever actually occupies it - and a
		// stored copy would be a claim about an aircraft that has not arrived.
		StandPreview::Describe(Entity.Definition, Entity.Position, Entity.Heading, Sink);

		// THEN the committed-pose marker, AFTER StandPreview rather than before it, and at a
		// DIFFERENT radius from its own (see ARoadBuildHUD::Marker's StandPose case).
		// StandPreview's own last call is Sink.Marker(Entity.Position, Pending) at this
		// EXACT position - two rings at the same radius drawn there would simply overdraw
		// one another regardless of which runs second, which is what made this marker
		// invisible against the stop mark it was meant to be told apart from. Order still
		// matters for a sink with no radius-per-style logic of its own - the editor
		// viewport draws every style at one size (see FViewportPreviewSink::Marker) - and
		// there this is the one that legitimately wins, because it is the one drawn last.
		Sink.Marker(Entity.Position, EPreviewStyle::StandPose);

		// The RESOLVED anchors - guideline nodes a vehicle will actually route to - read
		// from the INSTANCE rather than recomputed from the definition, same as the HUD
		// always did: the definition's local positions transformed again would be a second
		// opinion about where they are, and the two could disagree without anything
		// reporting it.
		for (const FResolvedAnchor& Anchor : Entity.ResolvedAnchors)
		{
			const FGuidelineNode* Node = Network.GetGuidelineNode(Anchor.Node);
			if (Node != nullptr)
			{
				Sink.Marker(Node->Position, EPreviewStyle::ServiceAnchor);
			}
		}
	}
}

void GraphOverlay::Describe(const URoadNetwork& Network, IToolPreviewSink& Sink)
{
	DescribeNodes(Network, Sink);
	DescribeStands(Network, Sink);
}
