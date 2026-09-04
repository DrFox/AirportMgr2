#include "Tool/RoadPlacement.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"

ERoadPlacement RoadPlacement::Validate(const URoadNetwork& Network, FRoadNodeId From,
	const FRoadSnapResult& To, const FRoadPlacementLimits& Limits)
{
	const FRoadNode* Start = Network.GetNode(From);
	if (Start == nullptr)
	{
		return ERoadPlacement::NoStart;
	}

	// Only an existing node can already be joined to something. A Free or Segment snap
	// resolves to a node that does not exist yet, so neither can collide with an arm the
	// start node already has.
	if (To.Kind == ERoadSnapKind::Node)
	{
		if (To.Node == From)
		{
			return ERoadPlacement::SameNode;
		}

		for (const FRoadSegmentId& Incident : Start->Incident)
		{
			if (Network.GetOtherEnd(Incident, From) == To.Node)
			{
				return ERoadPlacement::AlreadyJoined;
			}
		}
	}

	const double Length = FVector2D::Distance(Start->Position, To.Position);
	if (Length < Limits.MinSegmentLength)
	{
		return ERoadPlacement::TooShort;
	}

	// A corner is measured between two tangents that both point AWAY from the node they
	// meet at, so the angle between them IS the corner - small means a hairpin, 180 means
	// straight through.
	const auto IsTooSharp = [&Limits](const FVector2D& A, const FVector2D& B)
	{
		if (A.IsNearlyZero() || B.IsNearlyZero())
		{
			return false;
		}
		const double Cosine = FMath::Clamp(FVector2D::DotProduct(A, B), -1.0, 1.0);
		return FMath::RadiansToDegrees(FMath::Acos(Cosine)) < Limits.MinTurnDegrees;
	};

	const FVector2D Outgoing = (To.Position - Start->Position) / Length;

	// The corner at the START node.
	for (const FRoadSegmentId& Incident : Start->Incident)
	{
		if (IsTooSharp(Outgoing, Network.GetOutgoingTangent(Incident, From)))
		{
			return ERoadPlacement::TooSharp;
		}
	}

	// The corner at the FAR end, which used to go unmeasured entirely.
	//
	// The previous comment here reasoned that the far end "is checked when a segment is
	// drawn FROM there". It is not: that would be a DIFFERENT segment. This one makes a
	// corner at its destination the moment it is built, and nothing else will ever look at
	// it - so a hairpin was reachable simply by drawing towards a junction instead of away
	// from one. Both ends make corners; both ends are measured.
	const FVector2D Incoming = -Outgoing;

	if (To.Kind == ERoadSnapKind::Node)
	{
		const FRoadNode* End = Network.GetNode(To.Node);
		if (End != nullptr)
		{
			for (const FRoadSegmentId& Incident : End->Incident)
			{
				if (IsTooSharp(Incoming, Network.GetOutgoingTangent(Incident, To.Node)))
				{
					return ERoadPlacement::TooSharpAtEnd;
				}
			}
		}
	}
	else if (To.Kind == ERoadSnapKind::Segment)
	{
		// Splitting hands the new node BOTH halves of the segment as arms, pointing at the
		// two endpoints. Drawing back almost along the road just split is exactly as sharp
		// as any other hairpin, and the node does not exist yet to be asked.
		const FRoadSegment* Split = Network.GetSegment(To.Segment);
		const FRoadNode* EndA = Split ? Network.GetNode(Split->A) : nullptr;
		const FRoadNode* EndB = Split ? Network.GetNode(Split->B) : nullptr;

		if (EndA != nullptr && EndB != nullptr)
		{
			const FVector2D TowardsA = (EndA->Position - To.Position).GetSafeNormal();
			const FVector2D TowardsB = (EndB->Position - To.Position).GetSafeNormal();

			if (IsTooSharp(Incoming, TowardsA) || IsTooSharp(Incoming, TowardsB))
			{
				return ERoadPlacement::TooSharpAtEnd;
			}
		}
	}

	// A Free snap lands on a node that does not exist yet and will carry only this arm, so
	// there is no corner at the far end to measure.

	return ERoadPlacement::Valid;
}

const TCHAR* RoadPlacement::Describe(ERoadPlacement Result)
{
	switch (Result)
	{
	case ERoadPlacement::Valid:         return TEXT("");
	case ERoadPlacement::NoStart:       return TEXT("no start node");
	case ERoadPlacement::SameNode:      return TEXT("same node");
	case ERoadPlacement::AlreadyJoined: return TEXT("already joined");
	case ERoadPlacement::TooShort:      return TEXT("too short");
	case ERoadPlacement::TooSharp:      return TEXT("turn too sharp");
	case ERoadPlacement::TooSharpAtEnd: return TEXT("turn too sharp at the far end");
	default:                            return TEXT("refused");
	}
}
