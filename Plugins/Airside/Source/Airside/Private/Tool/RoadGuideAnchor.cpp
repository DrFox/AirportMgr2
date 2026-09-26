#include "Tool/RoadGuideAnchor.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Solve/GuideArbiter.h"
#include "Tool/RoadNaming.h"

void RoadGuideAnchor::DescribeIncomingArm(const URoadNetwork& Network, const FRoadNode& Node,
	FRoadNodeId NodeId, FGuideAnchor& Out)
{
	// EXACTLY ONE ARM GIVES A DIRECTION TO HOLD - see this function's own header comment for
	// why more or fewer leaves Out untouched.
	if (Node.Incident.Num() != 1)
	{
		return;
	}

	const FRoadNodeId Far = Network.GetOtherEnd(Node.Incident[0], NodeId);
	const FRoadNode* Other = Network.GetNode(Far);
	if (Other == nullptr)
	{
		return;
	}

	const FVector2D Along = (Node.Position - Other->Position).GetSafeNormal();
	if (Along.IsNearlyZero())
	{
		return;
	}

	Out.Reference = Along;
	Out.ReferenceAt = Other->Position;
	Out.ReferenceName = TEXT("this road");
}

void RoadGuideAnchor::AddNodeCandidates(const URoadNetwork& InNetwork, const FVector2D& Origin,
	int32 ExcludeIndex, FGuideAnchor& Out)
{
	// Named so the body lifted from FRoadDrawTool::DescribeGuideAnchor reads unchanged; every
	// comment below came with it, because each explains a rule rather than a call site.
	const URoadNetwork* Network = &InNetwork;

	// EVERY LIVE NODE IN REACH IS SOMETHING TO LINE UP WITH - "level with that junction" is what
	// a player squinting at a taxiway layout actually wants. A node has no name, so the label
	// cannot say WHICH; the dashed line drawn to it is what does.
	//
	// A DELETED NODE KEEPS ITS SLOT, so bAlive is checked here as the segment loop above checks
	// its own: offering one would draw a guide to a junction the player has removed.
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;
	const TArray<FRoadNode>& Nodes = Network->GetNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FRoadNode& Node = Nodes[Index];

		// NOT THE NODE THE GESTURE IS HOLDING - the one being extended from, or the one being
		// dragged. Its own lines pass through the origin, so both would always be in tolerance
		// and the guide would say "you are level with yourself".
		if (Index == ExcludeIndex || !Node.bAlive
			|| FVector2D::DistSquared(Node.Position, Origin) > Reach * Reach)
		{
			continue;
		}
		// A NODE BELONGS TO EVERY COLUMN THAT MEETS IT - ruled 2026-09-20, when Road became
		// Taxiway and ServiceRoad. A node is a place where segments end, and where a taxiway
		// meets a service road it is honestly both; picking a winner would make one of the two
		// buttons lie about a junction the player can see. So: one FGuidePoint per DISTINCT
		// kind of incident segment, and the node's position repeated under each.
		//
		// AND A BARE NODE OFFERS NOTHING. With no live segment on it there is no kind to tag,
		// and the tool's own Kind would be a guess about what the player will attach to it -
		// which is exactly the second opinion about the gesture that FGuidePoint::Reference
		// exists to avoid. In practice the only bare node is the one being extended from, and
		// that is excluded above.
		SnapGuide::EReference Columns[] = {
			SnapGuide::EReference::Taxiway,
			SnapGuide::EReference::ServiceRoad,
			SnapGuide::EReference::Runway };

		for (const SnapGuide::EReference Column : Columns)
		{
			bool bIncident = false;
			for (const FRoadSegmentId Meeting : Node.Incident)
			{
				SnapGuide::EReference Of = SnapGuide::EReference::Taxiway;
				if (RoadNaming::ReferenceOf(*Network, Meeting, Of) && Of == Column)
				{
					bIncident = true;
					break;
				}
			}
			if (!bIncident)
			{
				continue;
			}

			// SPELT OUT, not braced: a third member arrived on FGuidePoint in 2026-09-20 and a
			// braced initialiser would have taken the default for it in silence.
			FGuidePoint Point;
			Point.At = Node.Position;
			Point.Name = TEXT("that node");

			// THE ONE PLACE A NETWORK FACT REACHES A TOOL-FED SOURCE, and the tag is what lets
			// the matching button switch it off.
			Point.Reference = Column;
			Out.AlignTo.Add(Point);
		}
	}
}
