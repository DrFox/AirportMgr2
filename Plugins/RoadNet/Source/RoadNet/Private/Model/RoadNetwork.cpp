#include "Model/RoadNetwork.h"
#include "Model/RoadSlotMap.h"
#include "Profiles/RoadProfile.h"
#include "Entities/EntityDefinition.h"

FRoadNodeId URoadNetwork::AddNode(const FVector2D& Position)
{
	FRoadNode Node;
	Node.Position = Position;
	return RoadSlot::Add<FRoadNodeId>(Nodes, NodeFreeList, MoveTemp(Node));
}

bool URoadNetwork::RemoveNode(FRoadNodeId Node)
{
	const FRoadNode* Existing = RoadSlot::Get<FRoadNodeId>(Nodes, Node);
	if (Existing == nullptr)
	{
		return false;
	}

	// Copy: removing segments mutates the incident array we would otherwise iterate.
	TArray<FRoadSegmentId> ToRemove = Existing->Incident;
	for (const FRoadSegmentId Segment : ToRemove)
	{
		RemoveSegment(Segment);
	}
	return RoadSlot::Remove<FRoadNodeId>(Nodes, NodeFreeList, Node);
}

FRoadSegmentId URoadNetwork::AddSegment(FRoadNodeId A, FRoadNodeId B, const FVector2D& Control, URoadProfile* Profile)
{
	if (!RoadSlot::IsValid<FRoadNodeId>(Nodes, A) || !RoadSlot::IsValid<FRoadNodeId>(Nodes, B) || A == B)
	{
		return FRoadSegmentId();
	}

	FRoadSegment Segment;
	Segment.A = A;
	Segment.B = B;
	Segment.Control = Control;
	Segment.Profile = Profile;

	const FRoadSegmentId Handle = RoadSlot::Add<FRoadSegmentId>(Segments, SegmentFreeList, MoveTemp(Segment));

	RoadSlot::Get<FRoadNodeId>(Nodes, A)->Incident.Add(Handle);
	RoadSlot::Get<FRoadNodeId>(Nodes, B)->Incident.Add(Handle);
	SortIncident(A);
	SortIncident(B);

	return Handle;
}

FRoadSegmentId URoadNetwork::AddStraightSegment(FRoadNodeId A, FRoadNodeId B, URoadProfile* Profile)
{
	const FRoadNode* NodeA = RoadSlot::Get<FRoadNodeId>(Nodes, A);
	const FRoadNode* NodeB = RoadSlot::Get<FRoadNodeId>(Nodes, B);
	if (NodeA == nullptr || NodeB == nullptr)
	{
		return FRoadSegmentId();
	}
	return AddSegment(A, B, (NodeA->Position + NodeB->Position) * 0.5, Profile);
}

bool URoadNetwork::RemoveSegment(FRoadSegmentId Segment)
{
	const FRoadSegment* Existing = RoadSlot::Get<FRoadSegmentId>(Segments, Segment);
	if (Existing == nullptr)
	{
		return false;
	}

	const FRoadNodeId EndA = Existing->A;
	const FRoadNodeId EndB = Existing->B;

	if (FRoadNode* NodeA = RoadSlot::Get<FRoadNodeId>(Nodes, EndA))
	{
		NodeA->Incident.Remove(Segment);
	}
	if (FRoadNode* NodeB = RoadSlot::Get<FRoadNodeId>(Nodes, EndB))
	{
		NodeB->Incident.Remove(Segment);
	}

	return RoadSlot::Remove<FRoadSegmentId>(Segments, SegmentFreeList, Segment);
}

const FRoadNode* URoadNetwork::GetNode(FRoadNodeId Node) const
{
	return RoadSlot::Get<FRoadNodeId>(Nodes, Node);
}

const FRoadSegment* URoadNetwork::GetSegment(FRoadSegmentId Segment) const
{
	return RoadSlot::Get<FRoadSegmentId>(Segments, Segment);
}

FRoadSegment* URoadNetwork::GetSegmentMutable(FRoadSegmentId Segment)
{
	return RoadSlot::Get<FRoadSegmentId>(Segments, Segment);
}

FRoadNodeId URoadNetwork::GetOtherEnd(FRoadSegmentId Segment, FRoadNodeId AtNode) const
{
	const FRoadSegment* Seg = GetSegment(Segment);
	if (Seg == nullptr)
	{
		return FRoadNodeId();
	}
	return (Seg->A == AtNode) ? Seg->B : Seg->A;
}

FVector2D URoadNetwork::GetOutgoingTangent(FRoadSegmentId Segment, FRoadNodeId AtNode) const
{
	const FRoadSegment* Seg = GetSegment(Segment);
	const FRoadNode* Node = GetNode(AtNode);
	if (Seg == nullptr || Node == nullptr)
	{
		return FVector2D(1.0, 0.0);
	}

	// Both ends: the outgoing tangent points toward the control point.
	FVector2D Dir = Seg->Control - Node->Position;

	if (Dir.IsNearlyZero())
	{
		// Degenerate control point; fall back to the straight chord.
		const FRoadNode* Other = GetNode(GetOtherEnd(Segment, AtNode));
		Dir = (Other != nullptr) ? (Other->Position - Node->Position) : FVector2D(1.0, 0.0);
	}

	// The chord can be zero too: only A == B is rejected, so two DISTINCT nodes may
	// legitimately sit at the same position. GetSafeNormal would then hand back (0,0),
	// which collapses every edge ray and makes the node silently fail to solve. Always
	// return a unit vector; an arbitrary direction is recoverable, a zero one is not.
	const FVector2D Normalised = Dir.GetSafeNormal();
	return Normalised.IsNearlyZero() ? FVector2D(1.0, 0.0) : Normalised;
}

void URoadNetwork::SortIncident(FRoadNodeId NodeId)
{
	FRoadNode* Node = RoadSlot::Get<FRoadNodeId>(Nodes, NodeId);
	if (Node == nullptr)
	{
		return;
	}

	Node->Incident.Sort([this, NodeId](const FRoadSegmentId& L, const FRoadSegmentId& R)
	{
		const FVector2D DirL = GetOutgoingTangent(L, NodeId);
		const FVector2D DirR = GetOutgoingTangent(R, NodeId);
		return FMath::Atan2(DirL.Y, DirL.X) < FMath::Atan2(DirR.Y, DirR.X);
	});
}

FGuidelineNodeId URoadNetwork::AddGuidelineNode(const FVector2D& Position, bool bDerived)
{
	FGuidelineNode Node;
	Node.Position = Position;
	Node.bDerived = bDerived;
	return RoadSlot::Add<FGuidelineNodeId>(GuidelineNodes, GuidelineNodeFreeList, MoveTemp(Node));
}

FGuidelineEdgeId URoadNetwork::AddGuidelineEdge(FGuidelineEdge&& Edge)
{
	// Both endpoints must be live BEFORE anything is added, or a rejected edge leaves a
	// half-linked graph behind.
	if (!RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, Edge.A) ||
		!RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, Edge.B))
	{
		return FGuidelineEdgeId();
	}

	const FGuidelineNodeId EndA = Edge.A;
	const FGuidelineNodeId EndB = Edge.B;

	const FGuidelineEdgeId Handle =
		RoadSlot::Add<FGuidelineEdgeId>(GuidelineEdges, GuidelineEdgeFreeList, MoveTemp(Edge));

	GuidelineNodes[EndA.Index].Incident.Add(Handle);
	if (EndB != EndA)
	{
		GuidelineNodes[EndB.Index].Incident.Add(Handle);
	}

	return Handle;
}

bool URoadNetwork::RemoveGuidelineEdge(FGuidelineEdgeId Edge)
{
	const FGuidelineEdge* Found =
		RoadSlot::Get<FGuidelineEdgeId>(GuidelineEdges, Edge);
	if (Found == nullptr)
	{
		return false;
	}

	// Retract from BOTH endpoints before freeing the slot - after Remove the payload is
	// still there but the generation has moved on, so read the endpoints now.
	const FGuidelineNodeId EndA = Found->A;
	const FGuidelineNodeId EndB = Found->B;

	if (RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, EndA))
	{
		GuidelineNodes[EndA.Index].Incident.Remove(Edge);
	}
	if (RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, EndB))
	{
		GuidelineNodes[EndB.Index].Incident.Remove(Edge);
	}

	return RoadSlot::Remove<FGuidelineEdgeId>(GuidelineEdges, GuidelineEdgeFreeList, Edge);
}

bool URoadNetwork::RemoveGuidelineNode(FGuidelineNodeId Node)
{
	if (!RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, Node))
	{
		return false;
	}

	// Copy the incidence list before removing anything: RemoveGuidelineEdge mutates it.
	const TArray<FGuidelineEdgeId> Doomed = GuidelineNodes[Node.Index].Incident;
	for (const FGuidelineEdgeId Edge : Doomed)
	{
		RemoveGuidelineEdge(Edge);
	}

	return RoadSlot::Remove<FGuidelineNodeId>(GuidelineNodes, GuidelineNodeFreeList, Node);
}

const FGuidelineNode* URoadNetwork::GetGuidelineNode(FGuidelineNodeId Node) const
{
	return RoadSlot::Get<FGuidelineNodeId>(GuidelineNodes, Node);
}

const FGuidelineEdge* URoadNetwork::GetGuidelineEdge(FGuidelineEdgeId Edge) const
{
	return RoadSlot::Get<FGuidelineEdgeId>(GuidelineEdges, Edge);
}

FGuidelineEdge* URoadNetwork::GetGuidelineEdgeMutable(FGuidelineEdgeId Edge)
{
	return RoadSlot::Get<FGuidelineEdgeId>(GuidelineEdges, Edge);
}

FGuidelineNode* URoadNetwork::GetGuidelineNodeMutable(FGuidelineNodeId Node)
{
	return RoadSlot::Get<FGuidelineNodeId>(GuidelineNodes, Node);
}

TArray<FGuidelineEdgeId> URoadNetwork::GetOutgoingGuidelines(
	FGuidelineNodeId Node, ETraversalClass Class) const
{
	TArray<FGuidelineEdgeId> Out;

	const FGuidelineNode* Found = RoadSlot::Get<FGuidelineNodeId>(GuidelineNodes, Node);
	if (Found == nullptr)
	{
		return Out;
	}

	for (const FGuidelineEdgeId Id : Found->Incident)
	{
		const FGuidelineEdge* Edge = RoadSlot::Get<FGuidelineEdgeId>(GuidelineEdges, Id);
		if (Edge == nullptr || !Edge->AllowedTraffic.Allows(Class))
		{
			continue;
		}

		// A self-loop's two ends are the SAME node, so leaving it leaves both ends at once
		// and every direction permits it. Without this, bLeavingA is unconditionally true
		// for a self-loop and a BToA one can never satisfy !bLeavingA - it becomes
		// untraversable from its own node, silently, with nothing to report it.
		const bool bSelfLoop = (Edge->A == Edge->B);
		const bool bLeavingA = (Edge->A == Node);
		const bool bPermitted =
			bSelfLoop ||
			Edge->Direction == EGuidelineDir::Bidirectional ||
			(bLeavingA && Edge->Direction == EGuidelineDir::AToB) ||
			(!bLeavingA && Edge->Direction == EGuidelineDir::BToA);

		if (bPermitted)
		{
			Out.Add(Id);
		}
	}

	return Out;
}

FApronId URoadNetwork::AddApron(FApronSurface&& Apron)
{
	return RoadSlot::Add<FApronId>(Aprons, ApronFreeList, MoveTemp(Apron));
}

bool URoadNetwork::RemoveApron(FApronId Apron)
{
	return RoadSlot::Remove<FApronId>(Aprons, ApronFreeList, Apron);
}

const FApronSurface* URoadNetwork::GetApron(FApronId Apron) const
{
	return RoadSlot::Get<FApronId>(Aprons, Apron);
}

FEntityInstanceId URoadNetwork::PlaceEntity(
	UEntityDefinition* Definition, const FVector2D& Position, double Heading)
{
	if (Definition == nullptr)
	{
		return FEntityInstanceId();
	}

	FEntityInstance Instance;
	Instance.Position = Position;
	Instance.Heading = Heading;
	Instance.Definition = Definition;
	Instance.ResolvedAnchors.Reserve(Definition->Anchors.Num());

	const double Cos = FMath::Cos(Heading);
	const double Sin = FMath::Sin(Heading);

	for (const FEntityAnchor& Anchor : Definition->Anchors)
	{
		// Local to world. Rotating by the entity's heading is what makes an anchor mean
		// "off the aircraft's left wing" rather than "somewhere north of here".
		const FVector2D World(
			Position.X + Anchor.LocalPosition.X * Cos - Anchor.LocalPosition.Y * Sin,
			Position.Y + Anchor.LocalPosition.X * Sin + Anchor.LocalPosition.Y * Cos);

		// NON-DERIVED. See the header: an anchor node has no incident edges until a
		// guideline is drawn to it, so a derived one would be swept by the next rebuild
		// and this handle would dangle.
		Instance.ResolvedAnchors.Add(AddGuidelineNode(World, /*bDerived=*/false));
	}

	return RoadSlot::Add<FEntityInstanceId>(Entities, EntityFreeList, MoveTemp(Instance));
}

bool URoadNetwork::RemoveEntity(FEntityInstanceId Entity)
{
	const FEntityInstance* Found = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Found == nullptr)
	{
		return false;
	}

	// Copy before removing anything: RemoveGuidelineNode does not touch this array, but
	// the slot's payload is not ours to read once the entity is freed.
	const TArray<FGuidelineNodeId> Owned = Found->ResolvedAnchors;
	for (const FGuidelineNodeId NodeId : Owned)
	{
		RemoveGuidelineNode(NodeId);
	}

	return RoadSlot::Remove<FEntityInstanceId>(Entities, EntityFreeList, Entity);
}

const FEntityInstance* URoadNetwork::GetEntity(FEntityInstanceId Entity) const
{
	return RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
}

bool URoadNetwork::GetAnchorWorldHeading(
	FEntityInstanceId Entity, int32 AnchorIndex, double& OutHeading) const
{
	const FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr || Instance->Definition == nullptr)
	{
		return false;
	}

	// Bounds-checked against the DEFINITION: LocalHeading lives on the anchor, not on the
	// resolved node, so this is the array that has to be in range.
	if (!Instance->Definition->Anchors.IsValidIndex(AnchorIndex))
	{
		return false;
	}

	// Composed, never stored. A stored world heading would go stale the moment the
	// instance is turned, and nothing here would notice.
	const FEntityAnchor& Anchor = Instance->Definition->Anchors[AnchorIndex];
	OutHeading = Instance->Heading + Anchor.LocalHeading;
	return true;
}

const FGuidelineNode* URoadNetwork::GetAnchorNode(FEntityInstanceId Entity, int32 AnchorIndex) const
{
	const FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr || !Instance->ResolvedAnchors.IsValidIndex(AnchorIndex))
	{
		// Deliberately the INSTANCE's array, not the definition's: a definition that gained
		// an anchor after this instance was placed is longer, and it is this array the
		// caller is about to read.
		return nullptr;
	}

	return GetGuidelineNode(Instance->ResolvedAnchors[AnchorIndex]);
}
