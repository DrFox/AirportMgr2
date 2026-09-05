#include "Model/RoadNetwork.h"
#include "Model/RoadSlotMap.h"
#include "Profiles/RoadProfile.h"
#include "Entities/EntityDefinition.h"

// Named apart from AirsideModule.cpp's LogAirside on purpose: both are file-static, and a
// unity build merges the translation units into one where the two definitions collide.
DEFINE_LOG_CATEGORY_STATIC(LogRoadModel, Log, All);

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

bool URoadNetwork::SetNodePosition(FRoadNodeId Node, const FVector2D& To)
{
	if (!RoadSlot::IsValid<FRoadNodeId, FRoadNode>(Nodes, Node))
	{
		return false;
	}

	const FVector2D Was = Nodes[Node.Index].Position;
	Nodes[Node.Index].Position = To;

	// Every incident segment's CONTROL POINT has to come with it. GetOutgoingTangent - and
	// therefore SortIncident, and therefore the solver - derives direction from Control,
	// not from the endpoints, so a node moved without it keeps pointing at where it used
	// to be: the roads do not follow the node, and the incidence order is sorted on stale
	// geometry. Found by instrumenting; nothing in the graph reports it.
	//
	// Moved by half the node's own displacement, which is how far the chord's midpoint
	// travels. That leaves a straight segment straight and preserves a curve's bend
	// relative to its chord, rather than flattening it.
	const FVector2D ControlShift = (To - Was) * 0.5;
	for (const FRoadSegmentId& Incident : Nodes[Node.Index].Incident)
	{
		if (FRoadSegment* Segment = GetSegmentMutable(Incident))
		{
			Segment->Control += ControlShift;
		}
	}

	// Copied before sorting: SortIncident reorders the very array being walked.
	const TArray<FRoadSegmentId> Touching = Nodes[Node.Index].Incident;

	SortIncident(Node);
	for (const FRoadSegmentId& Incident : Touching)
	{
		// The neighbour's bearing towards this node changed too, so its own list is now
		// out of order as well. Missing this is invisible until a junction is solved.
		const FRoadNodeId Other = GetOtherEnd(Incident, Node);
		if (Other.IsSet())
		{
			SortIncident(Other);
		}
	}

	return true;
}

const FRoadNode* URoadNetwork::GetNode(FRoadNodeId Node) const
{
	return RoadSlot::Get<FRoadNodeId>(Nodes, Node);
}

const URoadProfile* URoadNetwork::ProfileFor(const FRoadSegment& Segment) const
{
	// Its own first, always. DefaultProfile is for segments that never had one or lost it
	// to a save, never an override - a road drawn deliberately narrow must stay narrow.
	return Segment.Profile != nullptr ? Segment.Profile.Get() : DefaultProfile.Get();
}

bool URoadNetwork::RunwayExtentAt(const FVector2D& Near, FVector2D& OutThreshold,
	FVector2D& OutDirection, double& OutLength) const
{
	return RunwayExtentInternal(Near, true, OutThreshold, OutDirection, OutLength);
}

bool URoadNetwork::NearestRunwayThreshold(const FVector2D& Near, FVector2D& OutThreshold,
	FVector2D& OutDirection, double& OutLength) const
{
	// NO PROXIMITY TEST, and that is the difference between the two. RunwayExtentAt answers
	// "is this point ON a runway", which a departure asks of the place its taxi ended and
	// which must say no for the rest of the airport. This answers "which runway would you
	// land on", which is asked of a click that is deliberately nowhere near one.
	return RunwayExtentInternal(Near, false, OutThreshold, OutDirection, OutLength);
}

bool URoadNetwork::RunwayExtentInternal(const FVector2D& Near, bool bRequireOnRunway,
	FVector2D& OutThreshold, FVector2D& OutDirection, double& OutLength) const
{
	auto IsRunway = [this](const FRoadSegment& Segment)
	{
		const URoadProfile* Profile = ProfileFor(Segment);
		return Profile != nullptr && Profile->bContinuousThroughJunctions;
	};

	// The runway segment with an END nearest the query. Ends rather than centres: a threshold
	// is an end, and a long runway's midpoint can be closer to a query than the end that
	// actually matters.
	int32 Best = INDEX_NONE;
	double BestDistance = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegment& Segment = Segments[Index];
		if (!Segment.bAlive || !IsRunway(Segment))
		{
			continue;
		}

		const FRoadNode* A = GetNode(Segment.A);
		const FRoadNode* B = GetNode(Segment.B);
		if (A == nullptr || B == nullptr)
		{
			continue;
		}

		const double Distance = FMath::Min(
			FVector2D::Distance(Near, A->Position), FVector2D::Distance(Near, B->Position));
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = Index;
		}
	}

	if (Best == INDEX_NONE)
	{
		return false;
	}

	// AND IT HAS TO BE NEAR. Without this the search kept the nearest threshold and never
	// asked how near, so it answered "yes, a runway" for every point on the airport as soon
	// as one runway existed - and every dispatched route armed a departure at it. An aircraft
	// would taxi correctly to a stand on the far side and then jump to the runway and roll.
	//
	// The tolerance is the RUNWAY'S OWN WIDTH, so it scales with the strip rather than being
	// a number chosen to make one airport work: a wider runway is correspondingly more
	// forgiving about where its threshold is considered to begin, and a taxiway a hundred
	// metres away is never mistaken for one.
	if (bRequireOnRunway)
	{
		const URoadProfile* SeedProfile = ProfileFor(Segments[Best]);
		const double Reach = SeedProfile != nullptr ? SeedProfile->GetTotalWidth() : 0.0;
		if (BestDistance > Reach)
		{
			return false;
		}
	}

	// Walk out to both extremes through nodes that join exactly two runway segments. Anything
	// else - a threshold, or a node with a taxiway on it - ends the walk in that direction.
	//
	// Tracked by the node WALKED FROM rather than the segment walked along, because a node's
	// Incident list already holds segment handles and building one from an index would mean
	// reconstructing a generation counter that the slot map owns.
	auto WalkFrom = [this, &IsRunway](FRoadNodeId At, FRoadNodeId CameFrom)
	{
		for (int32 Guard = 0; Guard < 1024; ++Guard)
		{
			const FRoadNode* Node = GetNode(At);
			if (Node == nullptr)
			{
				break;
			}

			FRoadSegmentId Next;
			FRoadNodeId Beyond;
			int32 RunwayArms = 0;

			for (const FRoadSegmentId& Incident : Node->Incident)
			{
				const FRoadSegment* Other = GetSegment(Incident);
				if (Other == nullptr || !Other->bAlive || !IsRunway(*Other))
				{
					continue;
				}
				++RunwayArms;

				const FRoadNodeId Far = GetOtherEnd(Incident, At);
				if (Far != CameFrom)
				{
					Next = Incident;
					Beyond = Far;
				}
			}

			// One runway arm means this node is a threshold. More than two would be a fork,
			// which a runway does not have - stopping there is the safe reading.
			if (RunwayArms != 2 || !Next.IsSet() || !Beyond.IsSet())
			{
				break;
			}

			CameFrom = At;
			At = Beyond;
		}

		return At;
	};

	const FRoadSegment& Seed = Segments[Best];
	const FRoadNodeId EndA = WalkFrom(Seed.A, Seed.B);
	const FRoadNodeId EndB = WalkFrom(Seed.B, Seed.A);

	const FRoadNode* NodeA = GetNode(EndA);
	const FRoadNode* NodeB = GetNode(EndB);
	if (NodeA == nullptr || NodeB == nullptr)
	{
		return false;
	}

	// The threshold is the end you are AT; you depart away from it.
	const bool bNearA = FVector2D::Distance(Near, NodeA->Position)
		<= FVector2D::Distance(Near, NodeB->Position);

	OutThreshold = bNearA ? NodeA->Position : NodeB->Position;
	const FVector2D Far = bNearA ? NodeB->Position : NodeA->Position;

	const FVector2D Along = Far - OutThreshold;
	OutLength = Along.Size();
	if (OutLength <= 0.0)
	{
		return false;
	}

	OutDirection = Along / OutLength;
	return true;
}

TArray<FGuidelineNodeId> URoadNetwork::RunwayExitNodes(const FVector2D& Threshold,
	const FVector2D& Direction, double Length, double HalfWidth, double MinDistance) const
{
	TArray<FGuidelineNodeId> Out;
	if (Direction.IsNearlyZero() || Length <= 0.0)
	{
		return Out;
	}

	const FVector2D Along = Direction.GetSafeNormal();

	// Sorted by distance down the runway, because the CALLER's rule is "the first exit I can
	// take". Collected with the distance and sorted at the end rather than inserted in order:
	// the guideline node array is in creation order, which has nothing to do with geometry.
	TArray<TPair<double, FGuidelineNodeId>> Found;

	for (int32 Index = 0; Index < GuidelineNodes.Num(); ++Index)
	{
		const FGuidelineNode& Node = GuidelineNodes[Index];
		if (!Node.bAlive)
		{
			continue;
		}

		const FVector2D Offset = Node.Position - Threshold;
		const double Distance = FVector2D::DotProduct(Offset, Along);

		// Beyond the point the aircraft could have slowed to taxi speed, and still on the
		// strip. An exit before that is one it cannot take, which is the whole reason
		// MinDistance is a parameter rather than zero.
		// The far end is INCLUDED, with the runway's own half width of slack past it. The
		// commonest airport anyone draws has its taxiway joined to the END of the runway, and
		// the guideline node there sits wherever the junction cut put it - which can be a
		// little beyond the road node the length was measured to. Excluding it leaves that
		// airport with no exits at all.
		if (Distance < MinDistance || Distance > Length + HalfWidth)
		{
			continue;
		}

		// LATERAL, so a node on a parallel taxiway is not mistaken for one on the runway.
		// The runway's own half width is the bound, so it scales with the strip.
		const double Lateral = FMath::Abs(FVector2D::CrossProduct(Along, Offset));
		if (Lateral > HalfWidth)
		{
			continue;
		}

		Found.Add(TPair<double, FGuidelineNodeId>(
			Distance, RoadSlot::HandleAt<FGuidelineNodeId>(GuidelineNodes, Index)));
	}

	Found.Sort([](const TPair<double, FGuidelineNodeId>& A, const TPair<double, FGuidelineNodeId>& B)
	{
		return A.Key < B.Key;
	});

	Out.Reserve(Found.Num());
	for (const TPair<double, FGuidelineNodeId>& Entry : Found)
	{
		Out.Add(Entry.Value);
	}
	return Out;
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

bool URoadNetwork::RelinkGuidelineEdge(FGuidelineEdgeId Edge, FGuidelineNodeId NewA,
	FGuidelineNodeId NewB)
{
	FGuidelineEdge* Found = RoadSlot::Get<FGuidelineEdgeId>(GuidelineEdges, Edge);
	if (Found == nullptr)
	{
		return false;
	}

	// Both new ends must be live BEFORE anything moves, for the same reason AddGuidelineEdge
	// checks first: a half-applied relink leaves the graph inconsistent in a way nothing
	// downstream can detect.
	if (!RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, NewA) ||
		!RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, NewB))
	{
		return false;
	}

	const FGuidelineNodeId OldA = Found->A;
	const FGuidelineNodeId OldB = Found->B;

	if (RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, OldA))
	{
		GuidelineNodes[OldA.Index].Incident.Remove(Edge);
	}
	if (RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, OldB))
	{
		GuidelineNodes[OldB.Index].Incident.Remove(Edge);
	}

	Found->A = NewA;
	Found->B = NewB;

	GuidelineNodes[NewA.Index].Incident.AddUnique(Edge);
	if (NewB != NewA)
	{
		GuidelineNodes[NewB.Index].Incident.AddUnique(Edge);
	}

	return true;
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

	// Complained about, not refused: a half-authored definition should be visible in the
	// log rather than fatal at the call site. But it IS a real fault - lookup is by id, so
	// two anchors sharing one are indistinguishable and a query for either returns the
	// first, which sends the fuel truck to the belt loader and reports success.
	if (!UEntityDefinition::HasUsableAnchorIds(Definition))
	{
		UE_LOG(LogRoadModel, Error,
			TEXT("PlaceEntity: %s has anchors with empty or duplicate ids. Anchor lookups on "
				 "this entity will be ambiguous."),
			*Definition->GetName());
	}

	FEntityInstance Instance;
	Instance.Position = Position;
	Instance.Heading = Heading;
	Instance.Definition = Definition;
	Instance.ResolvedAnchors.Reserve(Definition->Anchors.Num());

	// The stop position itself, as a node an aircraft can be routed to. NON-DERIVED for
	// the same reason the anchor nodes are: it carries no edge until a lead-in is cast to
	// it, and a derived one would be swept by the next rebuild.
	Instance.PoseNode = AddGuidelineNode(Position, /*bDerived=*/false);

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
		FResolvedAnchor Resolved;
		Resolved.Id = Anchor.Id;
		Resolved.Node = AddGuidelineNode(World, /*bDerived=*/false);
		Instance.ResolvedAnchors.Add(Resolved);
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
	const TArray<FResolvedAnchor> Owned = Found->ResolvedAnchors;
	const FGuidelineNodeId OwnedPose = Found->PoseNode;
	for (const FResolvedAnchor& Anchor : Owned)
	{
		RemoveGuidelineNode(Anchor.Node);
	}

	// The stop position goes with the stand. Left behind it would be a node in the middle
	// of the apron that routes still lead to and nothing explains.
	RemoveGuidelineNode(OwnedPose);

	return RoadSlot::Remove<FEntityInstanceId>(Entities, EntityFreeList, Entity);
}

const FEntityInstance* URoadNetwork::GetEntity(FEntityInstanceId Entity) const
{
	return RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
}

const FResolvedAnchor* URoadNetwork::FindResolvedAnchor(FEntityInstanceId Entity, FName AnchorId) const
{
	const FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr)
	{
		return nullptr;
	}

	// A linear scan over a handful of anchors. A map keyed by id would be faster and would
	// have to be kept in step with the array, which is the class of duplication this change
	// exists to remove.
	for (const FResolvedAnchor& Resolved : Instance->ResolvedAnchors)
	{
		if (Resolved.Id == AnchorId)
		{
			return &Resolved;
		}
	}
	return nullptr;
}

bool URoadNetwork::GetAnchorWorldHeading(
	FEntityInstanceId Entity, FName AnchorId, double& OutHeading) const
{
	const FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr || Instance->Definition == nullptr)
	{
		return false;
	}

	// Read from the DEFINITION, by id: LocalHeading lives on the anchor rather than on the
	// resolved node, and reading it from the asset means an anchor re-aimed there is picked
	// up by instances already placed instead of them keeping a stale copy.
	for (const FEntityAnchor& Anchor : Instance->Definition->Anchors)
	{
		if (Anchor.Id == AnchorId)
		{
			// Composed, never stored. A stored world heading would go stale the moment the
			// instance is turned, and nothing here would notice.
			OutHeading = Instance->Heading + Anchor.LocalHeading;
			return true;
		}
	}

	return false;
}

const FGuidelineNode* URoadNetwork::GetAnchorNode(FEntityInstanceId Entity, FName AnchorId) const
{
	const FResolvedAnchor* Resolved = FindResolvedAnchor(Entity, AnchorId);
	return Resolved != nullptr ? GetGuidelineNode(Resolved->Node) : nullptr;
}

TArray<FName> URoadNetwork::GetAnchorIdsForRole(FEntityInstanceId Entity, EServiceRole Role) const
{
	TArray<FName> Found;

	const FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr || Instance->Definition == nullptr)
	{
		return Found;
	}

	for (const FEntityAnchor& Anchor : Instance->Definition->Anchors)
	{
		// Only ids this INSTANCE actually resolved. A definition that gained an anchor
		// after placement would otherwise hand back an id leading nowhere, which is the
		// same stale-lookup failure in a politer disguise.
		if (Anchor.Role == Role && FindResolvedAnchor(Entity, Anchor.Id) != nullptr)
		{
			Found.Add(Anchor.Id);
		}
	}
	return Found;
}

