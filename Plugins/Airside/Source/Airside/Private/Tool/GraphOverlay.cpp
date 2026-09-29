#include "Tool/GraphOverlay.h"

#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/StandAdmission.h"
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

		// The body FIRST - the SAME call a placement tool's own preview makes, so a placed
		// stand and an aimed one read as one object. See StandPreview.h for why this used to
		// be two, and why the stop mark, service points and fixtures are Describe's alone.
		//
		// The footprint matters because a stand aimed 180 degrees out looks identical to a
		// correct one until something tries to taxi onto it - the heading has to be
		// unmistakable, which is the whole reason StandPreview draws the aircraft rather
		// than a single point at the stop. It is recomputed on every call rather than
		// stored, because it belongs to whatever is PARKED here - today the type the stand
		// was sized for, tomorrow whatever actually occupies it - and a stored copy would be
		// a claim about an aircraft that has not arrived.
		//
		// NOT FOR A PLOTTED INSTALLATION. A depot's body is only its FootprintExtent box - 4 x
		// 8 m CENTRED ON THE POSE, which is its road connection - so on a drawn depot it was an
		// amber rectangle straddling the gate (reported 2026-09-27). The plot the player drew
		// IS the footprint; the box is a second opinion from before plots existed. A
		// point-placed depot keeps it, because there it is the only geometry the thing has.
		if (Entity.IsStand() || !Entity.IsPlotted())
		{
			StandPreview::DescribeBody(Entity.Definition, Entity.Position, Entity.Heading, Sink);
		}

		// A STAND A STRIP COVERS IS CLOSED to new arrivals (strip stage 6), and says so HERE - its
		// drawn outline in Refused - rather than in paint, which went red once and was removed
		// on purpose (RoadSurfacePresenter's stand paint). StandAdmission::StripClosure, the one
		// rule admission refuses by, so the flag and the refusal cannot disagree.
		// ENFORCED BY: Airside.Tool.StandInStripFlagged
		if (Entity.IsStand() && StandAdmission::StripClosure(Network, Entity).IsSet())
		{
			Sink.Polygon(Entity.Outline, EPreviewStyle::Refused);
		}

		// NO STOP MARK AND NO POSE RING, for any kind, since 2026-09-27. They were an
		// AIRCRAFT's - where its nose gear stops (Pending) and a second ring at a different
		// radius saying which pose was committed (StandPose) - and a depot lost both first, as
		// unexplained circles on the road. A stand lost them when its stop became PAINT
		// (FStandMarkingBuilder's stop bar, 6 m across): the ground now says where to stop, at
		// every zoom and with the overlay off, so two rings on top of it only said it again.
		// The heading they helped read is still unmistakable from the footprint above.
		// ENFORCED BY: Airside.Tool.StandOverlayMarkers, Airside.Tool.DepotOverlayMarkers

		// ONE small ServiceAnchor ring per resolved anchor and nothing else there - the
		// fixture and service-point marks that used to sit concentric with it are
		// StandPreview::Describe's (aiming only) now; see StandPreview.h.
		//
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
