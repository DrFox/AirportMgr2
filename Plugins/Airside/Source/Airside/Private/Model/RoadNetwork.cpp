#include "Model/RoadNetwork.h"
#include "Model/RoadSlotMap.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"

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

FRoadNodeId URoadNetwork::NodeIdAt(int32 Index) const
{
	return RoadSlot::HandleAt<FRoadNodeId>(Nodes, Index);
}

const URoadProfile* URoadNetwork::ProfileFor(const FRoadSegment& Segment) const
{
	// Its own first, always. DefaultProfile is for segments that never had one or lost it
	// to a save, never an override - a road drawn deliberately narrow must stay narrow.
	return Segment.Profile != nullptr ? Segment.Profile.Get() : DefaultProfile.Get();
}

bool URoadNetwork::IsRunwaySegment(FRoadSegmentId Segment) const
{
	const FRoadSegment* Found = GetSegment(Segment);
	if (Found == nullptr || !Found->bAlive)
	{
		return false;
	}
	const URoadProfile* Profile = ProfileFor(*Found);
	return Profile != nullptr && Profile->bContinuousThroughJunctions;
}

bool URoadNetwork::IsGuidelineNodeOnRunway(FGuidelineNodeId Node, FRoadSegmentId Seed,
	double* OutChainHalfWidth) const
{
	// A NODE IS A POSITION HERE and nothing else, so the geometry lives in one function and
	// the two callers cannot drift apart. An unknown node reports false with the half width
	// still zeroed, which is what IsPointOnRunway does for a chain that is not a runway.
	const FGuidelineNode* Point = GetGuidelineNode(Node);
	if (Point == nullptr)
	{
		if (OutChainHalfWidth != nullptr)
		{
			*OutChainHalfWidth = 0.0;
		}
		return false;
	}
	return IsPointOnRunway(Point->Position, Seed, OutChainHalfWidth);
}

bool URoadNetwork::IsPointOnRunway(const FVector2D& Position, FRoadSegmentId Seed,
	double* OutChainHalfWidth) const
{
	if (OutChainHalfWidth != nullptr)
	{
		*OutChainHalfWidth = 0.0;
	}

	bool bOnStrip = false;
	for (const FRoadSegmentId& Id : RunwayChain(Seed))
	{
		const FRoadSegment* Segment = GetSegment(Id);
		if (Segment == nullptr)
		{
			continue;
		}
		const FRoadNode* A = GetNode(Segment->A);
		const FRoadNode* B = GetNode(Segment->B);
		const URoadProfile* Profile = ProfileFor(*Segment);
		if (A == nullptr || B == nullptr || Profile == nullptr)
		{
			continue;
		}

		// THE SEGMENT'S OWN HALF WIDTH, not a constant: a chain may mix profiles, and the
		// bound has to scale with the strip - the same rule RunwayExitNodes uses.
		const double HalfWidth = Profile->GetTotalWidth() * 0.5;
		if (OutChainHalfWidth != nullptr)
		{
			*OutChainHalfWidth = FMath::Max(*OutChainHalfWidth, HalfWidth);
		}
		if (bOnStrip)
		{
			// Still walking the chain, but only to finish the half-width maximum above.
			continue;
		}

		// THE ROAD NODES' POSITIONS, deliberately, not the sampled ribbon: this asks about
		// the SURFACE model, and the surface's centreline is the segment A..B. A runway is
		// straight in every case the game admits (bContinuousThroughJunctions), so the
		// Bezier control point cannot bend it away from this line.
		const FVector2D Axis = B->Position - A->Position;
		const double Length = Axis.Size();
		if (Length <= KINDA_SMALL_NUMBER)
		{
			continue;
		}
		const FVector2D Along = Axis / Length;
		const FVector2D Offset = Position - A->Position;
		const double Distance = FVector2D::DotProduct(Offset, Along);
		const double Lateral = FMath::Abs(FVector2D::CrossProduct(Along, Offset));

		bOnStrip = Lateral <= HalfWidth
			&& Distance >= -HalfWidth && Distance <= Length + HalfWidth;
	}
	return bOnStrip;
}

TArray<FRoadSegmentId> URoadNetwork::RunwayChain(FRoadSegmentId Seed) const
{
	TArray<FRoadSegmentId> Out;
	if (!IsRunwaySegment(Seed))
	{
		return Out;
	}
	Out.Add(Seed);

	// RunwayExtentAt reads its thresholds off this chain (#86) rather than walking a second
	// time: from each end of Seed, step through nodes that join exactly two runway segments,
	// and stop at a threshold (one arm) or anything stranger (a fork).
	auto WalkFrom = [this, &Out](FRoadNodeId At, FRoadSegmentId Along)
	{
		for (int32 Guard = 0; Guard < 1024; ++Guard)
		{
			const FRoadNode* Node = GetNode(At);
			if (Node == nullptr)
			{
				return;
			}
			FRoadSegmentId Next;
			int32 RunwayArms = 0;
			for (const FRoadSegmentId& Incident : Node->Incident)
			{
				if (!IsRunwaySegment(Incident))
				{
					continue;
				}
				++RunwayArms;
				if (Incident != Along)
				{
					Next = Incident;
				}
			}
			if (RunwayArms != 2 || !Next.IsSet() || Out.Contains(Next))
			{
				return;
			}
			Out.Add(Next);
			At = GetOtherEnd(Next, At);
			Along = Next;
		}
	};

	const FRoadSegment* SeedSegment = GetSegment(Seed);
	WalkFrom(SeedSegment->A, Seed);
	WalkFrom(SeedSegment->B, Seed);
	return Out;
}

TArray<FRoadSegmentId> URoadNetwork::RunwayChainOrSeed(FRoadSegmentId Seed) const
{
	// The idiom four call sites spelled out separately (#86): RunwayChain is empty when
	// Seed is not a live runway (a rebuild dropped it to a taxiway under a stored claim,
	// say), and dropping the claim entirely reads as "nothing to protect" rather than
	// "protect the one segment I still know about" - so the seed itself stands in.
	TArray<FRoadSegmentId> Chain = RunwayChain(Seed);
	if (Chain.Num() == 0)
	{
		Chain.Add(Seed);
	}
	return Chain;
}

FRunwayFacts URoadNetwork::RunwayFactsFor(FRoadSegmentId Seed) const
{
	// The seed's own, not a walk: SetRunwayFacts and the split keep every member of a
	// chain equal, so the first member is as good as any and cheaper than the chain walk
	// the marking builder would otherwise make per runway per rebuild.
	const FRoadSegment* Segment = GetSegment(Seed);
	return Segment != nullptr && Segment->bAlive ? Segment->Runway : FRunwayFacts();
}

bool URoadNetwork::SetRunwayFacts(FRoadSegmentId Seed, const FRunwayFacts& Facts)
{
	const TArray<FRoadSegmentId> Chain = RunwayChain(Seed);
	if (Chain.IsEmpty())
	{
		return false;
	}
	for (const FRoadSegmentId& Member : Chain)
	{
		if (FRoadSegment* Segment = GetSegmentMutable(Member))
		{
			Segment->Runway = Facts;
		}
	}
	return true;
}

bool URoadNetwork::RunwayExtentAt(const FVector2D& Near, FVector2D& OutThreshold,
	FVector2D& OutDirection, double& OutLength, FRoadSegmentId* OutSegment) const
{
	return RunwayExtentInternal(Near, true, OutThreshold, OutDirection, OutLength, OutSegment);
}

bool URoadNetwork::NearestRunwayThreshold(const FVector2D& Near, FVector2D& OutThreshold,
	FVector2D& OutDirection, double& OutLength, FRoadSegmentId* OutSegment) const
{
	// NO PROXIMITY TEST, and that is the difference between the two. RunwayExtentAt answers
	// "is this point ON a runway", which a departure asks of the place its taxi ended and
	// which must say no for the rest of the airport. This answers "which runway would you
	// land on", which is asked of a click that is deliberately nowhere near one.
	return RunwayExtentInternal(Near, false, OutThreshold, OutDirection, OutLength, OutSegment);
}

bool URoadNetwork::RunwayExtentInternal(const FVector2D& Near, bool bRequireOnRunway,
	FVector2D& OutThreshold, FVector2D& OutDirection, double& OutLength,
	FRoadSegmentId* OutSegment) const
{
	// The runway segment with an END nearest the query. Ends rather than centres: a threshold
	// is an end, and a long runway's midpoint can be closer to a query than the end that
	// actually matters.
	int32 Best = INDEX_NONE;
	double BestDistance = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegment& Segment = Segments[Index];
		const FRoadSegmentId Id{Index, Segment.Generation};
		if (!IsRunwaySegment(Id))
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
	//
	// ON THE STRIP, not merely near an end (2026-09-07): since the exit arcs a taxi joins the
	// runway at a split node ExitLength down the centreline, sixty metres from any runway
	// node, and the old "within a width of an end" test refused it - so an intersection
	// departure never armed and the aircraft parked on the runway. IsPointOnRunway is the
	// one implementation of "is this on the strip" that occupancy and the planners share.
	// Either test admits the point: on the strip anywhere along it, OR within a width of an
	// end - the original rule, kept because a route drawn to a threshold ends at the strip's
	// dead-end cut, a half width short of the road node the extent is measured from.
	if (bRequireOnRunway)
	{
		const FRoadSegmentId Seed{Best, Segments[Best].Generation};
		const URoadProfile* SeedProfile = ProfileFor(Segments[Best]);
		const double Reach = SeedProfile != nullptr ? SeedProfile->GetTotalWidth() : 0.0;
		if (!IsPointOnRunway(Near, Seed) && BestDistance > Reach)
		{
			return false;
		}
	}

	if (OutSegment != nullptr)
	{
		OutSegment->Index = Best;
		OutSegment->Generation = Segments[Best].Generation;
	}

	// THE CHAIN IS THE WALK (#86): RunwayChain already walks out through nodes that join
	// exactly two runway segments, stopping at a threshold or a fork - the same rule this
	// used to walk a second time, node by node, to find the very same two ends. The ends
	// are simply the chain's own nodes touched by exactly one of its segments; a second
	// walk could only ever agree with the first or silently stop doing so.
	//
	// Plain RunwayChain, not RunwayChainOrSeed: Best was found by IsRunwaySegment in the
	// search above, so the chain is never empty here.
	const FRoadSegmentId SeedId{Best, Segments[Best].Generation};
	const TArray<FRoadSegmentId> Chain = RunwayChain(SeedId);

	TMap<FRoadNodeId, int32> ChainArms;
	ChainArms.Reserve(Chain.Num() * 2);
	for (const FRoadSegmentId& Member : Chain)
	{
		if (const FRoadSegment* MemberSegment = GetSegment(Member))
		{
			++ChainArms.FindOrAdd(MemberSegment->A);
			++ChainArms.FindOrAdd(MemberSegment->B);
		}
	}

	// The two nodes touched by exactly one chain segment - order from TMap iteration is
	// NOT deterministic, so collect both before choosing which is "EndA".
	TArray<FRoadNodeId, TInlineAllocator<2>> Thresholds;
	for (const TPair<FRoadNodeId, int32>& Arm : ChainArms)
	{
		if (Arm.Value == 1)
		{
			Thresholds.Add(Arm.Key);
		}
	}

	const FRoadNode* NodeA = nullptr;
	const FRoadNode* NodeB = nullptr;
	if (Thresholds.Num() == 2)
	{
		// EndA is explicitly whichever threshold is nearer Segments[Best].A - not "whichever
		// the map iterated first", which a hash reshuffle could change - so a query exactly
		// equidistant from both ends (bNearA below, on <=) resolves the same way every run.
		const FRoadNode* SeedNodeA = GetNode(Segments[Best].A);
		const FRoadNode* First = GetNode(Thresholds[0]);
		const FRoadNode* Second = GetNode(Thresholds[1]);
		const bool bFirstIsA = SeedNodeA != nullptr && First != nullptr && Second != nullptr
			&& FVector2D::DistSquared(SeedNodeA->Position, First->Position)
				<= FVector2D::DistSquared(SeedNodeA->Position, Second->Position);
		NodeA = bFirstIsA ? First : Second;
		NodeB = bFirstIsA ? Second : First;
	}
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

FRoadSegmentId URoadNetwork::SegmentIdAt(int32 Index) const
{
	return RoadSlot::HandleAt<FRoadSegmentId>(Segments, Index);
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
	++GuidelineRevision;
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

	++GuidelineRevision;
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

	++GuidelineRevision;
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

	++GuidelineRevision;
	return true;
}

bool URoadNetwork::SplitGuidelineEdge(FGuidelineEdgeId Edge, double T, double WeldTolerance,
	FGuidelineNodeId& OutNode, FGuidelineEdgeId& OutHead, FGuidelineEdgeId& OutTail)
{
	OutNode = FGuidelineNodeId();
	OutHead = FGuidelineEdgeId();
	OutTail = FGuidelineEdgeId();

	const FGuidelineEdge* Found = GetGuidelineEdge(Edge);
	if (Found == nullptr)
	{
		return false;
	}

	// Copied before anything is removed: Found points into the slot array, and adding the
	// halves below can reallocate it - the same trap every one of the five former call
	// sites guarded against individually.
	const FGuidelineEdge Original = *Found;
	const FGuidelineNode* NodeA = GetGuidelineNode(Original.A);
	const FGuidelineNode* NodeB = GetGuidelineNode(Original.B);
	if (NodeA == nullptr || NodeB == nullptr)
	{
		return false;
	}
	const FVector2D PositionA = NodeA->Position;
	const FVector2D PositionB = NodeB->Position;

	FVector2D Mid, ControlLeft, ControlRight;
	GuidelineGeom::Split(PositionA, Original.Control, PositionB, T, Mid, ControlLeft, ControlRight);

	// Within tolerance of an existing endpoint: reuse it rather than splitting off a stub
	// nobody can see. Edge is untouched, and is handed back as whichever side of the
	// (un-made) split still has it - OutTail welding to A, OutHead welding to B - so a
	// caller chaining splits (the three-way sweep) can keep walking from the right piece.
	if (FVector2D::Distance(Mid, PositionA) <= WeldTolerance)
	{
		OutNode = Original.A;
		OutTail = Edge;
		return true;
	}
	if (FVector2D::Distance(Mid, PositionB) <= WeldTolerance)
	{
		OutNode = Original.B;
		OutHead = Edge;
		return true;
	}

	OutNode = AddGuidelineNode(Mid, /*bDerived=*/true);

	// Both halves inherit every field of Original - identity (DerivedFrom, ServiceLoopOwner,
	// ...) included - which is what keeps a split lane or taxiway recognisable as the same
	// thing it was before. Only the endpoint and control that actually moved are overridden.
	FGuidelineEdge Head = Original;
	Head.B = OutNode;
	Head.Control = ControlLeft;

	FGuidelineEdge Tail = Original;
	Tail.A = OutNode;
	Tail.Control = ControlRight;

	RemoveGuidelineEdge(Edge);
	OutHead = AddGuidelineEdge(MoveTemp(Head));
	OutTail = AddGuidelineEdge(MoveTemp(Tail));
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

	++GuidelineRevision;
	return RoadSlot::Remove<FGuidelineNodeId>(GuidelineNodes, GuidelineNodeFreeList, Node);
}

const FGuidelineNode* URoadNetwork::GetGuidelineNode(FGuidelineNodeId Node) const
{
	return RoadSlot::Get<FGuidelineNodeId>(GuidelineNodes, Node);
}

FGuidelineNodeId URoadNetwork::GuidelineNodeIdAt(int32 Index) const
{
	return RoadSlot::HandleAt<FGuidelineNodeId>(GuidelineNodes, Index);
}

const FGuidelineEdge* URoadNetwork::GetGuidelineEdge(FGuidelineEdgeId Edge) const
{
	return RoadSlot::Get<FGuidelineEdgeId>(GuidelineEdges, Edge);
}

FGuidelineEdgeId URoadNetwork::GuidelineEdgeIdAt(int32 Index) const
{
	return RoadSlot::HandleAt<FGuidelineEdgeId>(GuidelineEdges, Index);
}

FGuidelineEdge* URoadNetwork::GetGuidelineEdgeMutable(FGuidelineEdgeId Edge)
{
	return RoadSlot::Get<FGuidelineEdgeId>(GuidelineEdges, Edge);
}

FGuidelineNode* URoadNetwork::GetGuidelineNodeMutable(FGuidelineNodeId Node)
{
	return RoadSlot::Get<FGuidelineNodeId>(GuidelineNodes, Node);
}

bool URoadNetwork::SetIntermediateHoldingPosition(FGuidelineNodeId Node, bool bSet)
{
	FGuidelineNode* Found = GetGuidelineNodeMutable(Node);
	if (Found == nullptr)
	{
		return false;
	}
	// Not the player's: a runway-holding position is derived from the junction on every
	// build, so a clear here would come back next rebuild and a set would be a no-op that
	// looked like one. Refusing says so.
	if (Found->HoldingPosition == EHoldingPositionKind::Runway)
	{
		return false;
	}
	Found->HoldingPosition = bSet ? EHoldingPositionKind::Intermediate : EHoldingPositionKind::None;
	Found->HoldingPositionFor = FRoadSegmentId();

	const FGuidelineEndRef At = Found->Origin;
	if (!At.IsSet())
	{
		// An ANCHOR or hand-placed node - not derived, never swept, and its handle survives
		// every rebuild already (see FGuidelineNode::Origin). The flag on it is therefore
		// durable by itself, and a mark would be a second source for the same fact - which
		// is precisely the drift this pair of writes exists to avoid everywhere else.
		return true;
	}
	for (int32 Index = 0; Index < HoldingPositionMarks.Num(); ++Index)
	{
		if (HoldingPositionMarks[Index].At == At)
		{
			if (!bSet)
			{
				HoldingPositionMarks.RemoveAt(Index);
			}
			return true;
		}
	}
	if (bSet)
	{
		FHoldingPositionMark Mark;
		Mark.At = At;
		HoldingPositionMarks.Add(MoveTemp(Mark));
	}
	return true;
}

bool URoadNetwork::SetRunwayHoldingPositionForTest(FGuidelineNodeId Node, FRoadSegmentId Protects)
{
	FGuidelineNode* Found = GetGuidelineNodeMutable(Node);
	// A set Protects must be a live runway. Refusing beats storing it: the arbiter expands
	// whatever a position names through RunwayChain, and a taxiway named there would hand
	// a crossing agent a strip made of the taxiway it is standing on.
	if (Found == nullptr || !Protects.IsSet() || !IsRunwaySegment(Protects))
	{
		return false;
	}
	Found->HoldingPosition = EHoldingPositionKind::Runway;
	Found->HoldingPositionFor = Protects;
	return true;
}

void URoadNetwork::PruneHoldingPositionMarks()
{
	HoldingPositionMarks.RemoveAll([this](const FHoldingPositionMark& Mark)
	{
		// GetSegment is the generation-checked read, so a recycled slot fails it - which is
		// the whole point, because the builder's Ends map is keyed on the segment INDEX
		// alone and would happily re-apply a stale mark onto the road that took the index.
		return GetSegment(Mark.At.Segment) == nullptr;
	});
}

FRoadSegmentId URoadNetwork::RunwayNearGuidelineNode(FGuidelineNodeId Node) const
{
	const FGuidelineNode* Found = GetGuidelineNode(Node);
	if (Found == nullptr)
	{
		return FRoadSegmentId();
	}

	auto RunwayAmongIncident = [this](const FGuidelineNode& At) -> FRoadSegmentId
	{
		for (const FGuidelineEdgeId& Incident : At.Incident)
		{
			const FGuidelineEdge* Edge = GetGuidelineEdge(Incident);

			// DerivedFrom unset means a turn path or a hand-drawn link, neither of which
			// belongs to a surface at all - so neither can answer which runway is here.
			if (Edge != nullptr && Edge->DerivedFrom.IsSet() && IsRunwaySegment(Edge->DerivedFrom))
			{
				return Edge->DerivedFrom;
			}
		}
		return FRoadSegmentId();
	};

	const FRoadSegmentId Own = RunwayAmongIncident(*Found);
	if (Own.IsSet())
	{
		return Own;
	}

	// ONE HOP - see the header. The turn paths out of a junction are what stand between a
	// taxiway's end node and the runway's centreline nodes.
	for (const FGuidelineEdgeId& Incident : Found->Incident)
	{
		const FGuidelineEdge* Edge = GetGuidelineEdge(Incident);
		if (Edge == nullptr)
		{
			continue;
		}
		const FGuidelineNode* Neighbour = GetGuidelineNode(Edge->A == Node ? Edge->B : Edge->A);
		if (Neighbour == nullptr)
		{
			continue;
		}
		const FRoadSegmentId Near = RunwayAmongIncident(*Neighbour);
		if (Near.IsSet())
		{
			return Near;
		}
	}
	return FRoadSegmentId();
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

bool URoadNetwork::IsServiceNodeConnected(FGuidelineNodeId Node) const
{
	// Breadth-first over OWNED edges only. The frontier is tiny - a lane is four sides and
	// five spurs - so a plain array of node ids costs nothing, and the visited set is what
	// terminates it on a lane that is by construction a ring.
	TSet<FGuidelineNodeId> Seen;
	TArray<FGuidelineNodeId> Frontier;
	Seen.Add(Node);
	Frontier.Add(Node);

	while (Frontier.Num() > 0)
	{
		const FGuidelineNodeId At = Frontier.Pop();
		const FGuidelineNode* Found = RoadSlot::Get<FGuidelineNodeId>(GuidelineNodes, At);
		if (Found == nullptr)
		{
			continue;
		}

		for (const FGuidelineEdgeId Id : Found->Incident)
		{
			const FGuidelineEdge* Edge = RoadSlot::Get<FGuidelineEdgeId>(GuidelineEdges, Id);
			if (Edge == nullptr)
			{
				continue;
			}

			if (!Edge->ServiceLoopOwner.IsSet())
			{
				// Something that is not this stand's own lane. That is the whole question,
				// and it is why the link from a lane to a road deliberately carries no owner.
				return true;
			}

			const FGuidelineNodeId Other = Edge->A == At ? Edge->B : Edge->A;
			if (!Seen.Contains(Other))
			{
				Seen.Add(Other);
				Frontier.Add(Other);
			}
		}
	}
	return false;
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

FApronId URoadNetwork::ApronIdAt(int32 Index) const
{
	return RoadSlot::HandleAt<FApronId>(Aprons, Index);
}

FEntityInstanceId URoadNetwork::PlaceEntity(
	UEntityDefinition* Definition, TConstArrayView<FEntityAnchor> Anchors,
	const FVector2D& Position, double Heading, double DesignWingspan, EServiceRole PoseRole,
	int32 Trucks)
{
	if (Definition == nullptr)
	{
		return FEntityInstanceId();
	}

	// HasUsableAnchorIds' complaint moved to the caller along with it: that check, like
	// Anchors itself, is a UEntityDefinition method Model/ cannot call - see the header.

	FEntityInstance Instance;
	Instance.Position = Position;
	Instance.Heading = Heading;
	Instance.Definition = Definition;
	Instance.DesignWingspan = DesignWingspan;

	// Captured for the same Model/-must-not-see-Entities/ reason as DesignWingspan, and read
	// by FAnchorLink to decide which class of guideline the pose's lead-in may join.
	Instance.PoseRole = PoseRole;
	Instance.Trucks = Trucks;

	Instance.ResolvedAnchors.Reserve(Anchors.Num());

	// The stop position itself, as a node an aircraft can be routed to. NON-DERIVED for
	// the same reason the anchor nodes are: it carries no edge until a lead-in is cast to
	// it, and a derived one would be swept by the next rebuild.
	Instance.PoseNode = AddGuidelineNode(Position, /*bDerived=*/false);

	const double Cos = FMath::Cos(Heading);
	const double Sin = FMath::Sin(Heading);

	for (const FEntityAnchor& Anchor : Anchors)
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

		// Captured rather than left on the definition - see FResolvedAnchor's comment.
		// GetAnchorWorldHeading and GetAnchorIdsForRole read these back instead of
		// Definition->Anchors, which is the whole reason this struct grew them.
		Resolved.LocalHeading = Anchor.LocalHeading;
		Resolved.Role = Anchor.Role;
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

FEntityInstanceId URoadNetwork::EntityIdAt(int32 Index) const
{
	return RoadSlot::HandleAt<FEntityInstanceId>(Entities, Index);
}

int32 URoadNetwork::FindEntityIndexByPoseNode(FGuidelineNodeId Node) const
{
	if (!Node.IsSet())
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		if (Entities[Index].bAlive && Entities[Index].PoseNode == Node)
		{
			return Index;
		}
	}
	return INDEX_NONE;
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
	if (Instance == nullptr)
	{
		return false;
	}

	// Read from the RESOLVED anchor, by id: LocalHeading was captured here at placement -
	// see FResolvedAnchor's comment - because Model/ cannot read it back from the
	// definition live the way this used to.
	for (const FResolvedAnchor& Resolved : Instance->ResolvedAnchors)
	{
		if (Resolved.Id == AnchorId)
		{
			// Composed, never stored. A stored world heading would go stale the moment the
			// instance is turned, and nothing here would notice.
			OutHeading = Instance->Heading + Resolved.LocalHeading;
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
	if (Instance == nullptr)
	{
		return Found;
	}

	// Reads FResolvedAnchor::Role rather than filtering the definition's own anchors and
	// checking each one against ResolvedAnchors - ResolvedAnchors already holds only ids
	// this INSTANCE actually resolved, so iterating it directly cannot hand back an id a
	// definition edited after placement would leave leading nowhere.
	for (const FResolvedAnchor& Resolved : Instance->ResolvedAnchors)
	{
		if (Resolved.Role == Role)
		{
			Found.Add(Resolved.Id);
		}
	}
	return Found;
}

bool URoadNetwork::RefreshResolvedAnchor(
	FEntityInstanceId Entity, FName AnchorId, double LocalHeading, EServiceRole Role)
{
	FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr)
	{
		return false;
	}

	for (FResolvedAnchor& Resolved : Instance->ResolvedAnchors)
	{
		if (Resolved.Id == AnchorId)
		{
			Resolved.LocalHeading = LocalHeading;
			Resolved.Role = Role;
			return true;
		}
	}
	return false;
}

bool URoadNetwork::SetEntityPoseRole(FEntityInstanceId Entity, EServiceRole PoseRole)
{
	FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr)
	{
		return false;
	}

	Instance->PoseRole = PoseRole;
	return true;
}
