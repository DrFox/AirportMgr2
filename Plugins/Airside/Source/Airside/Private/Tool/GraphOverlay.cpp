#include "Tool/GraphOverlay.h"

#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/StandPreview.h"

void GraphOverlay::Describe(const URoadNetwork& Network, IToolPreviewSink& Sink)
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

	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		if (!Entity.bAlive)
		{
			continue;
		}

		// The committed pose itself - context, not a gesture. StandPreview::Describe below
		// marks the same position again as Pending, because that call is shared with an
		// in-progress placement and must not learn it is being asked for a committed one;
		// this ring is what lets a viewer tell "already here" from "about to be placed"
		// apart on screen, which used to be the runtime HUD's double ring and nothing at all
		// in the editor.
		Sink.Marker(Entity.Position, EPreviewStyle::StandPose);

		// The full description - aircraft footprint, service points, fixtures - the SAME
		// call a placement tool's own preview makes, so a placed stand and an aimed one read
		// as one object. See StandPreview.h for why this used to be two.
		StandPreview::Describe(Entity.Definition, Entity.Position, Entity.Heading, Sink);

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
