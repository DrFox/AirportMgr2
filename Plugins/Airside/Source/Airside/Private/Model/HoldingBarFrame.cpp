#include "Model/HoldingBarFrame.h"

#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"

FHoldingBarFrame HoldingBarAt(const URoadNetwork& Network, FGuidelineNodeId NodeId)
{
	FHoldingBarFrame Frame;
	const FGuidelineNode* Node = Network.GetGuidelineNode(NodeId);
	if (Node == nullptr)
	{
		return Frame;
	}

	// A taxiway with no profile to ask, and no incident edge to measure below, still gets a
	// bar this wide - the standard taxiway. Overwritten by either branch that finds something
	// more specific to ask.
	Frame.HalfWidth = URoadProfile::StandardTaxiwayWidth * 0.5;

	// THE NODE'S OWN SEGMENT FIRST, and directly - not by searching Incident for an edge whose
	// DerivedFrom happens to match. Origin names the road this node was derived FOR - the
	// taxiway the aircraft is travelling along - so GetOutgoingTangent on THAT segment, at the
	// end this node sits on, is the taxiway's own direction at this exact point, whatever the
	// guideline graph's edges around it look like. GetOutgoingTangent points from the road's
	// node OUT along the segment, so negating it points from the node into the junction - the
	// side the marking pattern sits on.
	if (Node->Origin.IsSet())
	{
		if (const FRoadSegment* Segment = Network.GetSegment(Node->Origin.Segment))
		{
			const FRoadNodeId RoadNode = Node->Origin.bEndA ? Segment->A : Segment->B;
			Frame.Toward = -Network.GetOutgoingTangent(Node->Origin.Segment, RoadNode).GetSafeNormal();
			if (const URoadProfile* Profile = Network.ProfileFor(*Segment))
			{
				Frame.HalfWidth = Profile->GetTotalWidth() * 0.5;
			}
		}
	}

	// Incident[0] is the fallback for a node with no Origin (an anchor, a hand-placed node):
	// away from whichever neighbour is found first, negated the same way as above so both
	// branches agree on what "into the junction" means for THIS node.
	if (Frame.Toward.IsNearlyZero())
	{
		for (const FGuidelineEdgeId& EdgeId : Node->Incident)
		{
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(EdgeId);
			if (Edge == nullptr)
			{
				continue;
			}
			const FGuidelineNodeId Other = (Edge->A == NodeId) ? Edge->B : Edge->A;
			const FGuidelineNode* OtherNode = Network.GetGuidelineNode(Other);
			if (OtherNode == nullptr)
			{
				continue;
			}
			const FVector2D Away = (OtherNode->Position - Node->Position).GetSafeNormal();
			if (!Away.IsNearlyZero())
			{
				Frame.Toward = -Away;
				if (Edge->Width > 0.0)
				{
					Frame.HalfWidth = Edge->Width * 0.5;
				}
				break;
			}
		}
	}

	// An isolated node (no Origin, no usable incident edge) has no bar to be across: Toward
	// stays zero and IsSet() says so, rather than handing back an arbitrary Across nothing
	// measured.
	if (!Frame.Toward.IsNearlyZero())
	{
		Frame.Across = FVector2D(-Frame.Toward.Y, Frame.Toward.X);
	}

	return Frame;
}
