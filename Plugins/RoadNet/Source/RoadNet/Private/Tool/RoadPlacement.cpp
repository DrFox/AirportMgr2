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

	// Measured at the START node only. The far end's arms are checked when a segment is
	// drawn FROM there, and a node cannot know about a corner it is not the vertex of.
	const FVector2D Outgoing = (To.Position - Start->Position) / Length;
	for (const FRoadSegmentId& Incident : Start->Incident)
	{
		const FVector2D Existing = Network.GetOutgoingTangent(Incident, From);
		if (Existing.IsNearlyZero())
		{
			continue;
		}

		// Both tangents point AWAY from the shared node, so the angle between them is the
		// angle of the corner itself - small means a hairpin, 180 means straight through.
		const double Cosine = FMath::Clamp(FVector2D::DotProduct(Outgoing, Existing), -1.0, 1.0);
		if (FMath::RadiansToDegrees(FMath::Acos(Cosine)) < Limits.MinTurnDegrees)
		{
			return ERoadPlacement::TooSharp;
		}
	}

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
	default:                            return TEXT("refused");
	}
}
