#include "Model/RoadNetwork.h"
#include "AirsideLog.h"
#include "Model/RoadSlotMap.h"
#include "Model/RunwayQuery.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

FRoadNodeId URoadNetwork::AddNode(const FVector2D& Position)
{
	FRoadNode Node;
	Node.Position = Position;
	++EditRevision;
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
	++EditRevision;
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

	++EditRevision;
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

	++EditRevision;
	return RoadSlot::Remove<FRoadSegmentId>(Segments, SegmentFreeList, Segment);
}

FRoadNodeId URoadNetwork::SplitSegment(FRoadSegmentId Doomed, const FVector2D& At)
{
	const FRoadSegment* Segment = GetSegment(Doomed);
	const FRoadNode* EndA = Segment != nullptr ? GetNode(Segment->A) : nullptr;
	const FRoadNode* EndB = Segment != nullptr ? GetNode(Segment->B) : nullptr;
	if (EndA == nullptr || EndB == nullptr)
	{
		return FRoadNodeId();
	}

	// Copied out before anything mutates. Every pointer above dangles the moment the
	// segment is removed or the arrays reallocate, and the two replacements need all of
	// this after that point.
	const FRoadNodeId KeepA = Segment->A;
	const FRoadNodeId KeepB = Segment->B;
	URoadProfile* KeepProfile = Segment->Profile;
	const FRunwayFacts KeepFacts = Segment->Runway;
	const FVector2D PositionA = EndA->Position;
	const FVector2D PositionB = EndB->Position;

	// A degeneracy floor, NOT a placement policy: how far from an end a split should be
	// allowed is the snap chain's MinSplitFromEndpoint, which is tuned and can be turned
	// down. This is the point below which the result is not a road at all, and no setting
	// may cross it - a zero-length segment has no direction, so the solver cannot derive
	// a bearing for it and the junction at either end loses an arm.
	constexpr double MinSplitOffset = 1.0;
	if (FVector2D::DistSquared(At, PositionA) < MinSplitOffset * MinSplitOffset
		|| FVector2D::DistSquared(At, PositionB) < MinSplitOffset * MinSplitOffset)
	{
		return FRoadNodeId();
	}

	const FRoadNodeId Middle = AddNode(At);
	if (!Middle.IsSet())
	{
		return FRoadNodeId();
	}

	// Removed, not reshaped. A segment's endpoints are its identity and both of them
	// change here, so the handle must die rather than quietly come to mean half a road.
	if (!RemoveSegment(Doomed))
	{
		RemoveNode(Middle);
		return FRoadNodeId();
	}

	const FRoadSegmentId First = AddStraightSegment(KeepA, Middle, KeepProfile);
	const FRoadSegmentId Second = AddStraightSegment(Middle, KeepB, KeepProfile);

	// The runway facts are the STRIP's and both halves are still the strip. Copied here
	// rather than re-derived through SetRunwayFacts on the chain, because at this moment
	// the chain is the two new segments and nothing else remembers what the doomed one
	// said; without this, every exit added to a precision runway demoted the far half to
	// the default and repainted it visual.
	for (const FRoadSegmentId& Half : { First, Second })
	{
		if (FRoadSegment* Fresh = GetSegmentMutable(Half))
		{
			Fresh->Runway = KeepFacts;
		}
	}

	// Both endpoints were checked live and the middle node was just created, so the only
	// way here is a model invariant having changed underneath. Loud rather than silent:
	// the graph is now missing a road the player can still see the ends of.
	if (!First.IsSet() || !Second.IsSet())
	{
		UE_LOG(LogRoadMesh, Error, TEXT("SplitSegment left a segment half-replaced: first=%d second=%d"),
			First.IsSet() ? 1 : 0, Second.IsSet() ? 1 : 0);
	}

	return Middle;
}

bool URoadNetwork::SetNodePosition(FRoadNodeId Node, const FVector2D& To)
{
	if (!RoadSlot::IsValid<FRoadNodeId, FRoadNode>(Nodes, Node))
	{
		return false;
	}

	const FVector2D Was = Nodes[Node.Index].Position;
	Nodes[Node.Index].Position = To;
	++EditRevision;

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

bool URoadNetwork::SetApronCorner(FApronId Apron, int32 CornerIndex, const FVector2D& To)
{
	FApronSurface* Live = RoadSlot::Get<FApronId>(Aprons, Apron);
	if (Live == nullptr || !Live->Outline.IsValidIndex(CornerIndex))
	{
		return false;
	}

	Live->Outline[CornerIndex] = To;
	return true;
}

bool URoadNetwork::MergeNodes(FRoadNodeId Keep, FRoadNodeId Absorb)
{
	if (!RoadSlot::IsValid<FRoadNodeId, FRoadNode>(Nodes, Keep)
		|| !RoadSlot::IsValid<FRoadNodeId, FRoadNode>(Nodes, Absorb)
		|| Keep == Absorb)
	{
		return false;
	}

	const FVector2D KeepAt = Nodes[Keep.Index].Position;
	// Bumped up front, not per case below: every branch of this function - collapse,
	// narrower-arm removal, and the plain repoint case that touches neither AddSegment nor
	// RemoveSegment - is graph surgery, and RemoveNode's own bump at the end would still
	// miss the repoint case (Segment->A/B and Incident are written directly, in CASE 3
	// below, with no primitive mutator between here and there to carry it).
	++EditRevision;

	// COPIED BEFORE ANY SURGERY: RemoveSegment mutates the very array this walks, and a
	// repoint below edits it too. The same copy RemoveNode takes, for the same reason.
	const TArray<FRoadSegmentId> Arms = Nodes[Absorb.Index].Incident;

	for (const FRoadSegmentId Arm : Arms)
	{
		FRoadSegment* Segment = RoadSlot::Get<FRoadSegmentId>(Segments, Arm);
		if (Segment == nullptr)
		{
			continue;
		}

		const bool bAbsorbIsA = (Segment->A == Absorb);
		const FRoadNodeId Far = bAbsorbIsA ? Segment->B : Segment->A;

		// CASE 1: the arm runs BETWEEN the two nodes being merged. Once they are one node it
		// is a self-loop - no length, no direction, and nothing the junction solver can make
		// a surface from. Collapsing it is the whole point of merging a stubby pair.
		if (Far == Keep)
		{
			RemoveSegment(Arm);
			continue;
		}

		// CASE 2: Keep already reaches that far end, so repointing would leave two segments
		// between one pair of nodes - coincident pavement at one Z, which is a z-fight and
		// not a surface. One of them has to go, and it is the NARROWER: see the header.
		FRoadSegmentId Rival;
		for (const FRoadSegmentId Existing : Nodes[Keep.Index].Incident)
		{
			if (GetOtherEnd(Existing, Keep) == Far)
			{
				Rival = Existing;
				break;
			}
		}

		if (Rival.IsSet())
		{
			const FRoadSegment* RivalSegment = RoadSlot::Get<FRoadSegmentId>(Segments, Rival);
			const URoadProfile* RivalProfile = RivalSegment != nullptr ? ProfileFor(*RivalSegment) : nullptr;
			const URoadProfile* ArmProfile = ProfileFor(*Segment);

			const double RivalWidth = RivalProfile != nullptr ? RivalProfile->GetTotalWidth() : 0.0;
			const double ArmWidth = ArmProfile != nullptr ? ArmProfile->GetTotalWidth() : 0.0;

			// STRICTLY GREATER, so an exact tie keeps the arm already on Keep - the edit that
			// touches less, and a rule that does not depend on which node the player happened
			// to drag onto which.
			if (ArmWidth > RivalWidth)
			{
				RemoveSegment(Rival);
				// Falls through to the repoint below: the wider arm is the survivor and still
				// has to be moved onto Keep.
			}
			else
			{
				RemoveSegment(Arm);
				continue;
			}

			// Re-fetched: RemoveSegment above may have moved this slot's neighbours about, and
			// the pointer taken before it is not one to trust afterwards.
			Segment = RoadSlot::Get<FRoadSegmentId>(Segments, Arm);
			if (Segment == nullptr)
			{
				continue;
			}
		}

		// CASE 3: repoint. The Control point travels by HALF the endpoint's displacement,
		// which is how far the chord's midpoint moves - so a straight segment stays exactly
		// straight and a curve keeps its bend relative to its chord. SetNodePosition applies
		// the identical rule; getting it wrong leaves the arm aiming at where its end used
		// to be, because GetOutgoingTangent derives direction from Control and not from the
		// endpoints.
		const FVector2D Was = Nodes[Absorb.Index].Position;
		Segment->Control += (KeepAt - Was) * 0.5;

		if (bAbsorbIsA)
		{
			Segment->A = Keep;
		}
		else
		{
			Segment->B = Keep;
		}

		Nodes[Absorb.Index].Incident.Remove(Arm);
		Nodes[Keep.Index].Incident.AddUnique(Arm);
	}

	// Absorb is bare by now - every arm was either removed or repointed - so this takes no
	// segments with it. Going through RemoveNode rather than the slot directly keeps the one
	// cascade, in case a future arm kind is missed above.
	RemoveNode(Absorb);

	// THE INCIDENCE ORDER, at Keep and at every neighbour. URoadNetwork's contract is that
	// Incident stays sorted by outgoing bearing and the junction solver walks it assuming
	// so - an arm in the wrong slot puts one road's geometry on another road's cut line.
	// Every repointed arm changed its bearing at BOTH ends, which is the half SetNodePosition
	// records being invisible until a junction is solved.
	SortIncident(Keep);
	const TArray<FRoadSegmentId> Touching = Nodes[Keep.Index].Incident;
	for (const FRoadSegmentId Arm : Touching)
	{
		const FRoadNodeId Other = GetOtherEnd(Arm, Keep);
		if (Other.IsSet())
		{
			SortIncident(Other);
		}
	}

	return true;
}

void URoadNetwork::CopyFrom(const URoadNetwork& Source)
{
	// EVERY ARRAY DuplicateObject's reflection walk used to clone, assigned by hand instead
	// (#166): TArray::operator= is a deep copy, so handles (index and generation, both
	// plain data inside the element structs) come across identical to what DuplicateObject
	// gave the ghost preview - the property the whole call site depends on (see
	// URoadSurfacePresenter::BuildGhostBuffers's own comment).
	//
	// WHOLESALE, not just Nodes/Segments: a caller that reasoned "the ghost only ever reads
	// Nodes and Segments" would be a second place deciding what the ghost is allowed to
	// need, and the day it grows a use for the guideline graph or an apron, this function
	// would silently hand it stale data instead of a copy error loud enough to find. The
	// cost of copying the rest is a few more TArray assignments, not a UObject allocation -
	// the whole point of this function existing.
	Nodes = Source.Nodes;
	NodeFreeList = Source.NodeFreeList;
	Segments = Source.Segments;
	SegmentFreeList = Source.SegmentFreeList;

	GuidelineNodes = Source.GuidelineNodes;
	GuidelineNodeFreeList = Source.GuidelineNodeFreeList;
	GuidelineEdges = Source.GuidelineEdges;
	GuidelineEdgeFreeList = Source.GuidelineEdgeFreeList;
	GuidelineRevision = Source.GuidelineRevision;

	HoldingPositionMarks = Source.HoldingPositionMarks;

	Aprons = Source.Aprons;
	ApronFreeList = Source.ApronFreeList;

	Entities = Source.Entities;
	EntityFreeList = Source.EntityFreeList;

	// THE ONE FIELD THE FIRST VERSION OF THIS FUNCTION ALMOST LEFT OUT: DefaultProfile is
	// how ProfileFor answers for any segment with no profile of its own (see its own
	// comment), and URoadSurfacePresenter::Rebuild sets it on the LIVE network before every
	// solve - a copy that missed it would solve the ghost's arms at zero width the moment
	// a level-loaded segment (profile lost to the transient package, see DefaultProfile's
	// own comment) needed the fallback.
	DefaultProfile = Source.DefaultProfile;
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
	return RunwayQuery::IsGuidelineNodeOnRunway(*this, Node, Seed, OutChainHalfWidth);
}

bool URoadNetwork::IsPointOnRunway(const FVector2D& Position, FRoadSegmentId Seed,
	double* OutChainHalfWidth) const
{
	return RunwayQuery::IsPointOnRunway(*this, Position, Seed, OutChainHalfWidth);
}

bool URoadNetwork::IsPointOnRunway(const FVector2D& Position, const TArray<FRoadSegmentId>& Chain,
	double* OutChainHalfWidth) const
{
	return RunwayQuery::IsPointOnRunway(*this, Position, Chain, OutChainHalfWidth);
}

TArray<FRoadSegmentId> URoadNetwork::RunwayChain(FRoadSegmentId Seed) const
{
	return RunwayQuery::RunwayChain(*this, Seed);
}

TArray<FRoadSegmentId> URoadNetwork::RunwayChainOrSeed(FRoadSegmentId Seed) const
{
	return RunwayQuery::RunwayChainOrSeed(*this, Seed);
}

TArray<FTrafficResource> URoadNetwork::RunwaySurfaces(FRoadSegmentId Seed) const
{
	TArray<FTrafficResource> Surfaces;
	for (const FRoadSegmentId& Segment : RunwayChainOrSeed(Seed))
	{
		Surfaces.Add(FTrafficResource::OfSurface(Segment));
	}
	return Surfaces;
}

FRunwayFacts URoadNetwork::RunwayFactsFor(FRoadSegmentId Seed) const
{
	return RunwayQuery::RunwayFactsFor(*this, Seed);
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

bool URoadNetwork::RunwayExtentAt(const FVector2D& Near, FRunwayEnd& OutEnd) const
{
	return RunwayQuery::RunwayExtentAt(*this, Near, OutEnd);
}

bool URoadNetwork::NearestRunwayThreshold(const FVector2D& Near, FRunwayEnd& OutEnd) const
{
	return RunwayQuery::NearestRunwayThreshold(*this, Near, OutEnd);
}

TArray<FGuidelineNodeId> URoadNetwork::RunwayExitNodes(FRoadSegmentId Seed, const FVector2D& Threshold,
	const FVector2D& Direction, double MinDistance) const
{
	return RunwayQuery::RunwayExitNodes(*this, Seed, Threshold, Direction, MinDistance);
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
		return RoadGeom::Bearing(DirL) < RoadGeom::Bearing(DirR);
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

	// Both halves inherit every field of Original - identity (DerivedFrom, StandGeometryOwner,
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

bool URoadNetwork::SampleGuideline(FGuidelineEdgeId Edge, TArray<FVector2D>& Out, bool bFromB) const
{
	const FGuidelineEdge* Found = GetGuidelineEdge(Edge);
	if (Found == nullptr)
	{
		return false;
	}
	const FGuidelineNode* A = GetGuidelineNode(Found->A);
	const FGuidelineNode* B = GetGuidelineNode(Found->B);
	if (A == nullptr || B == nullptr)
	{
		return false;
	}
	if (bFromB)
	{
		GuidelineGeom::Sample(B->Position, Found->Control, A->Position, Out);
	}
	else
	{
		GuidelineGeom::Sample(A->Position, Found->Control, B->Position, Out);
	}
	return true;
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

			if (!Edge->StandGeometryOwner.IsSet())
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

bool URoadNetwork::IsDepotJoined(const FEntityInstance& Entity) const
{
	const FGuidelineNode* Pose = GetGuidelineNode(Entity.PoseNode);
	return Pose != nullptr && Pose->Incident.Num() > 0;
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
	// A FORWARDER. The logic moved to the overload below when the plot and its modules
	// became the fourth and fifth things a placement carries - see FEntityPlacement. Every
	// caller written before plots existed keeps meaning exactly what it meant.
	FEntityPlacement Placement;
	Placement.Definition = Definition;
	Placement.Anchors = Anchors;
	Placement.Position = Position;
	Placement.Heading = Heading;
	Placement.DesignWingspan = DesignWingspan;
	Placement.PoseRole = PoseRole;
	Placement.Trucks = Trucks;
	return PlaceEntity(Placement);
}

FEntityInstanceId URoadNetwork::PlaceEntity(const FEntityPlacement& Placement)
{
	if (Placement.Definition == nullptr)
	{
		return FEntityInstanceId();
	}

	// HasUsableAnchorIds' complaint moved to the caller along with it: that check, like
	// Anchors itself, is a UEntityDefinition method Model/ cannot call - see the header.

	FEntityInstance Instance;
	Instance.Position = Placement.Position;
	Instance.Heading = Placement.Heading;
	Instance.Definition = Placement.Definition;
	Instance.DesignWingspan = Placement.DesignWingspan;

	// Captured for the same Model/-must-not-see-Entities/ reason as DesignWingspan, and read
	// by FAnchorLink to decide which class of guideline the pose's lead-in may join.
	Instance.PoseRole = Placement.PoseRole;

	Instance.Outline = Placement.Outline;
	Instance.Modules = Placement.Modules;

	// DERIVED FROM THE SHEDS, not captured, whenever there are modules at all. A shed is a
	// truck: the player's mix IS the fleet size, so a separately-stated count could only
	// ever disagree with the sheds they actually built.
	//
	// UFuelService is untouched by this and always will be - it reads Instance.Trucks, and
	// that a module system landed without its consumer changing is the sign the seam was
	// already in the right place.
	//
	// A PLOTLESS caller keeps its own number: it has no modules to derive one from. Exactly
	// one of the two branches applies to any placement, so the two cannot both be true.
	if (Placement.Modules.Num() > 0)
	{
		int32 Sheds = 0;
		for (const EDepotModule Module : Placement.Modules)
		{
			if (Module == EDepotModule::Shed)
			{
				++Sheds;
			}
		}
		Instance.Trucks = Sheds;
	}
	else
	{
		Instance.Trucks = Placement.Trucks;
	}

	Instance.ResolvedAnchors.Reserve(Placement.Anchors.Num());

	// The stop position itself, as a node an aircraft can be routed to. NON-DERIVED for
	// the same reason the anchor nodes are: it carries no edge until a lead-in is cast to
	// it, and a derived one would be swept by the next rebuild.
	//
	// ONE OF THEM, however many bays the plot holds. BuildFuelDepot already ruled that two
	// lead-ins from one small building into one road is a duplicate painted line, so a
	// depot's sheds are capacity and visuals - the yard's single gate is the pose.
	Instance.PoseNode = AddGuidelineNode(Placement.Position, /*bDerived=*/false);

	const double Cos = FMath::Cos(Placement.Heading);
	const double Sin = FMath::Sin(Placement.Heading);

	for (const FEntityAnchor& Anchor : Placement.Anchors)
	{
		// Local to world. Rotating by the entity's heading is what makes an anchor mean
		// "off the aircraft's left wing" rather than "somewhere north of here".
		const FVector2D World(
			Placement.Position.X + Anchor.LocalPosition.X * Cos - Anchor.LocalPosition.Y * Sin,
			Placement.Position.Y + Anchor.LocalPosition.X * Sin + Anchor.LocalPosition.Y * Cos);

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
