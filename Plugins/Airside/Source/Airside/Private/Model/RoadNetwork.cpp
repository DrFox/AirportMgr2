#include "Model/RoadNetwork.h"
#include "AirsideLog.h"
#include "Model/RoadSlotMap.h"
#include "Model/RunwayQuery.h"
#include "Model/TaxiwayStrip.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/JunctionSolver.h"
#include "Solve/RoadGeom.h"
#include "Solve/RunwayDesignator.h"
#include "Solve/StandBox.h"

bool URoadNetwork::SetDriveSide(EDriveSide Side)
{
	if (DriveSide == Side)
	{
		return false;
	}
	DriveSide = Side;
	// NO ++EditRevision: that clock is scoped to nodes and segments (see GetEditRevision), and
	// a flip moves neither - only the guidelines, which the rebuild re-derives and whose own
	// clock the builder advances.
	// BUT THE GUIDELINE CLOCK MOVES HERE TOO (#446), not only in that rebuild: the side is a fact a
	// planner reads, and "the caller rebuilds" is the caller's promise, not this model's.
	NoteFactChanged();
	UE_LOG(LogAirside, Log, TEXT("Drive side -> %s"), Side == EDriveSide::Left ? TEXT("Left") : TEXT("Right"));
	return true;
}

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

	// A NEW RUNWAY PIECE TAKES A DIRECTION IN USE here, the one site every runway passes
	// through (the tool's PlaceRunway, the test fixtures, the heal's scratch graph): the
	// chain's own if it joined a strip that has one - an extension drawn back toward the
	// threshold must not reverse the runway - else the direction it was DRAWN, A to B
	// (ruling 5, spec 2026-09-28-runway-in-use). SplitSegment overwrites both halves with the
	// doomed segment's facts straight after, so a split keeps the strip's.
	if (IsRunwaySegment(Handle))
	{
		int32 InUse = 0;
		for (const FRoadSegmentId& Member : RunwayChain(Handle))
		{
			const FRoadSegment* Other = GetSegment(Member);
			if (Member != Handle && Other != nullptr && Other->Runway.InUse != 0)
			{
				InUse = Other->Runway.InUse;
				break;
			}
		}
		if (InUse == 0)
		{
			InUse = RunwayDesignator::Designate(
				RoadSlot::Get<FRoadNodeId>(Nodes, B)->Position - RoadSlot::Get<FRoadNodeId>(Nodes, A)->Position);
		}
		GetSegmentMutable(Handle)->Runway.InUse = InUse;
	}
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
	const EPavement KeepSurface = Segment->Surface;
	const int32 KeepTaxiway = Segment->TaxiwayId;
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
	//
	// THE ROAD SURFACE LIKEWISE: an exit cut into a grass taxiway would otherwise pave both
	// halves, and the aircraft the grass kept off it would route straight down them.
	// THE TAXIWAY NAME LIKEWISE (2026-10-02), below.
	for (const FRoadSegmentId& Half : { First, Second })
	{
		if (FRoadSegment* Fresh = GetSegmentMutable(Half))
		{
			Fresh->Runway = KeepFacts;
			Fresh->Surface = KeepSurface;
		}
		// AND THE TAXIWAY (spec: "insert a node / split a segment: both halves keep the taxiway") - through the one
		// writer. ENFORCED BY: Airside.Model.TaxiwayNames.SplitKeepsTheTaxiway, Check-Architecture rule 103
		WriteTaxiwayId(Half, KeepTaxiway);
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
	// Named for the reason DefaultProfile is: the guideline builder reads it, so a scratch
	// copy that dropped it would derive the other side's lanes.
	DriveSide = Source.DriveSide;

	Aprons = Source.Aprons;
	ApronFreeList = Source.ApronFreeList;

	ReverseTurns = Source.ReverseTurns;
	ReverseTurnEnds = Source.ReverseTurnEnds;

	Entities = Source.Entities;
	EntityFreeList = Source.EntityFreeList;

	// THE ONE FIELD THE FIRST VERSION OF THIS FUNCTION ALMOST LEFT OUT: DefaultProfile is
	// how ProfileFor answers for any segment with no profile of its own (see its own
	// comment), and URoadSurfacePresenter::Rebuild sets it on the LIVE network before every
	// solve - a copy that missed it would solve the ghost's arms at zero width the moment
	// a level-loaded segment (profile lost to the transient package, see DefaultProfile's
	// own comment) needed the fallback.
	DefaultProfile = Source.DefaultProfile;

	// LEFT OUT UNTIL ISSUE #318's completeness test walked the class: NextStandNumber only ever
	// advances, so a copy that kept its own would let a restored network re-issue a number a
	// deleted stand had already worn - see the field's comment on why numbers are never reissued.
	NextStandNumber = Source.NextStandNumber;

	// ITS TWIN, FOR THE SAME REASON (#490): NextDepotNumber only ever advances too, so a restored or ghost copy that kept its own would re-issue
	// a number a bulldozed depot had already worn. Written beside NextStandNumber so the next counter added is not left out of one of them.
	// ENFORCED BY: Airside.Model.CopyFromCoversEveryProperty (the counter), Airside.Model.DepotNumbers (a copy keeps each depot's own number)
	NextDepotNumber = Source.NextDepotNumber;
	// THE NAMES (2026-10-02): an undo that restored the roads but not their names would re-letter the airport.
	Taxiways = Source.Taxiways;
	OwnedLand = Source.OwnedLand;
}

void URoadNetwork::RestoreFrom(const URoadNetwork& Snapshot)
{
	// READ BEFORE CopyFrom OVERWRITES GuidelineRevision with the snapshot's (zero, for a
	// DuplicateObject clone). See this method's header comment on why the clocks go forward.
	const uint32 LiveEditRevision = EditRevision;
	const uint32 LiveGuidelineRevision = GuidelineRevision;
	const bool bLiveWasDerived = GuidelinesDerivedAt != MAX_uint32;

	CopyFrom(Snapshot);

	EditRevision = FMath::Max(LiveEditRevision, Snapshot.EditRevision) + 1;
	GuidelineRevision = FMath::Max(LiveGuidelineRevision, Snapshot.GuidelineRevision) + 1;
	// NEVER-DERIVED STAYS NEVER-DERIVED: AreGuidelinesBehindRoad is false for MAX_uint32 because a
	// hand-authored graph has no road to be behind, and a stamp here would flip that the moment the
	// next node is added and the planners would refuse the graph.
	GuidelinesDerivedAt = bLiveWasDerived ? EditRevision : MAX_uint32;

	// PoseNodeIndex memoises Entities against GuidelineRevision, which has just moved, so it would
	// rebuild on its own; cleared as well so nothing depends on that being noticed.
	PoseNodeIndex.Reset();
	PoseNodeIndexRevision = MAX_uint32;
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

EPavement URoadNetwork::PavementOf(FRoadSegmentId Segment) const
{
	const FRoadSegment* Found = GetSegment(Segment);
	if (Found == nullptr || !Found->bAlive)
	{
		return EPavement::Tarmac;
	}
	// RUNWAY FIRST: a runway's surface is Runway.Surface, and its own Surface field is the
	// default nothing wrote. Asking the field alone would be right today only because no path
	// writes it on a runway.
	return IsRunwaySegment(Segment) ? RunwayFactsFor(Segment).Surface : Found->Surface;
}

bool URoadNetwork::IsGrassRoad(FRoadSegmentId Segment) const
{
	return !IsRunwaySegment(Segment) && PavementOf(Segment) == EPavement::Grass;
}

bool URoadNetwork::SetSegmentProfile(FRoadSegmentId Segment, URoadProfile* Profile)
{
	FRoadSegment* Found = GetSegmentMutable(Segment);
	if (Found == nullptr || !Found->bAlive || Profile == nullptr || IsRunwaySegment(Segment)
		|| Profile->bContinuousThroughJunctions)
	{
		return false;
	}
	if (TaxiwayStrip::IsAircraftOnlyProfile(ProfileFor(*Found)) != TaxiwayStrip::IsAircraftOnlyProfile(Profile))
	{
		UE_LOG(LogAirside, Warning, TEXT("SetSegmentProfile refused: %s would change segment %d's kind"),
			*Profile->GetName(), Segment.Index);
		return false;
	}
	Found->Profile = Profile;
	++EditRevision;
	return true;
}

bool URoadNetwork::WriteSegmentRestriction(FRoadSegmentId Segment, uint8 Letter)
{
	FRoadSegment* Found = GetSegmentMutable(Segment);
	if (Found == nullptr || !Found->bAlive)
	{
		return false;
	}
	Found->RestrictedLetter = Letter;
	return true;
}

bool URoadNetwork::SetSegmentSurface(FRoadSegmentId Segment, EPavement Surface)
{
	FRoadSegment* Found = GetSegmentMutable(Segment);
	if (Found == nullptr || !Found->bAlive || IsRunwaySegment(Segment))
	{
		return false;
	}
	// THE PROFILE'S LIST, the one the road tool's row offers: a concrete service road is
	// refused here as well as never offered there. Said out loud because a caller that got
	// false has otherwise laid a tarmac road with nothing to say why.
	const URoadProfile* Profile = ProfileFor(*Found);
	if (!Pavement::Offered(Profile != nullptr ? TConstArrayView<EPavement>(Profile->AllowedPavements)
			: TConstArrayView<EPavement>()).Contains(Surface))
	{
		UE_LOG(LogAirside, Warning, TEXT("SetSegmentSurface refused: %s is not offered by profile %s"),
			Pavement::Name(Surface), *GetNameSafe(Profile));
		return false;
	}
	Found->Surface = Surface;
	// A grass road is a fact a planner reads (IsGrassRoad) - see NoteFactChanged (#446).
	NoteFactChanged();
	return true;
}

bool URoadNetwork::IsGuidelineNodeOnRunway(FGuidelineNodeId Node, FRoadSegmentId Seed,
	double* OutChainHalfWidth) const
{
	return RunwayQuery::IsGuidelineNodeOnRunway(*this, Node, Seed, OutChainHalfWidth);
}

bool URoadNetwork::IsGuidelineNodeOnRunway(FGuidelineNodeId Node, const TArray<FRoadSegmentId>& Chain,
	double* OutChainHalfWidth) const
{
	return RunwayQuery::IsGuidelineNodeOnRunway(*this, Node, Chain, OutChainHalfWidth);
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
			// InUse 0 keeps the member's own - see the header for why 0 cannot mean "clear".
			// Use Unset keeps it too, for the same reason (ERunwayUse).
			const int32 Kept = Segment->Runway.InUse;
			const ERunwayUse KeptUse = Segment->Runway.Use;
			Segment->Runway = Facts;
			if (Facts.InUse == 0)
			{
				Segment->Runway.InUse = Kept;
			}
			if (Facts.Use == ERunwayUse::Unset)
			{
				Segment->Runway.Use = KeptUse;
			}
		}
	}
	// THE ONE SIGNAL THE CACHES GET (#446): the facade notifies a runway flip as EChangeKind::Facts, which
	// re-derives no graph, so without this the admission and re-bid gates keep the old runway's answer. See
	// the header. ENFORCED BY: AirportOps.Model.Offers.Generate.RunwayFlipAsksAgain, AirportOps.Service.Rebid.RunwayFlipRebids
	NoteFactChanged();
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

FRunwayEnd URoadNetwork::InUseEnd(const FRunwayEnd& Either) const
{
	return RunwayQuery::InUseEnd(*this, Either);
}

bool URoadNetwork::InUseRunwayAt(const FVector2D& Near, FRunwayEnd& OutEnd) const
{
	return RunwayQuery::InUseRunwayAt(*this, Near, OutEnd);
}

bool URoadNetwork::InUseRunwayNearest(const FVector2D& Near, FRunwayEnd& OutEnd) const
{
	return RunwayQuery::InUseRunwayNearest(*this, Near, OutEnd);
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

bool URoadNetwork::WriteSegmentEndSolve(FRoadSegmentId Segment, bool bEndA, const FJunctionArmResult& Solve)
{
	FRoadSegment* Found = GetSegmentMutable(Segment);
	if (Found == nullptr)
	{
		return false;
	}
	if (bEndA)
	{
		Found->TrimA = Solve.CutDistance;
		Found->LeftCutA = Solve.LeftCut;
		Found->RightCutA = Solve.RightCut;
		Found->bSolvedA = true;
	}
	else
	{
		Found->TrimB = Solve.CutDistance;
		Found->LeftCutB = Solve.LeftCut;
		Found->RightCutB = Solve.RightCut;
		Found->bSolvedB = true;
	}
	return true;
}

bool URoadNetwork::ClearSegmentEndSolve(FRoadSegmentId Segment, bool bEndA)
{
	FRoadSegment* Found = GetSegmentMutable(Segment);
	if (Found == nullptr)
	{
		return false;
	}
	if (bEndA)
	{
		Found->bSolvedA = false;
	}
	else
	{
		Found->bSolvedB = false;
	}
	return true;
}

bool URoadNetwork::SegmentEnds(FRoadSegmentId Segment, FVector2D& OutA, FVector2D& OutB) const
{
	const FRoadSegment* Seg = GetSegment(Segment);
	if (Seg == nullptr || !Seg->bAlive)
	{
		return false;
	}
	const FRoadNode* A = GetNode(Seg->A);
	const FRoadNode* B = GetNode(Seg->B);
	if (A == nullptr || B == nullptr)
	{
		return false;
	}
	OutA = A->Position;
	OutB = B->Position;
	return true;
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

	// Cached HERE, once, from the same evaluator SampleGuideline itself calls - see
	// FGuidelineEdge::Length (#171). Both ends are already known live above, so this needs no
	// lookup back through GetGuidelineEdge once the edge is in the array.
	Edge.Length = GuidelineGeom::Length(
		GuidelineNodes[EndA.Index].Position, Edge.Control, GuidelineNodes[EndB.Index].Position);

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

	// The ends just moved, so the cached length would otherwise go on describing the edge's
	// PREVIOUS geometry - see FGuidelineEdge::Length (#171).
	Found->Length = GuidelineGeom::Length(
		GuidelineNodes[NewA.Index].Position, Found->Control, GuidelineNodes[NewB.Index].Position);

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
	// thing it was before. Only the endpoint and control that actually moved, and the PER-HALF
	// fields below (measured off the whole curve, now stale for either piece of it), are
	// overridden.
	FGuidelineEdge Head = Original;
	Head.B = OutNode;
	Head.Control = ControlLeft;

	FGuidelineEdge Tail = Original;
	Tail.A = OutNode;
	Tail.Control = ControlRight;

	// The measured fields (MinRadius, ClearInner/Outer, ClearInnerAt/OuterAt) describe the
	// WHOLE original curve, not either half - and a curved half re-samples to the SAME point
	// count as the original (GuidelineGeom::Sample is a fixed subdivision), so a naive copy
	// passes VehicleFit's Path.Num()-vs-ClearInnerAt.Num() guard while judging a half against
	// the whole curve's numbers (#288). Mark unmeasured rather than re-measure: re-measuring
	// needs FRoadGuidelineBuilder::MeasureTurn and the junction pavement polygon the ORIGINAL
	// numbers were marched against, and this is Model/, which must not depend on Build/ to get
	// either - and a rebuild does not close the gap on its own: it re-derives the turn path
	// (MeasureTurn fills it in) and then FAnchorLink::Build splits it AGAIN, so a lead-in half
	// is unmeasured after EVERY rebuild, not just until the next one. The halves stay
	// unmeasured for as long as the split exists - VehicleFit says nothing for them rather
	// than guessing; re-measuring them belongs to a Build/ pass after AnchorLink (a
	// follow-up), not here. See the "PER-HALF FIELDS" comment beside FGuidelineEdge for the
	// full list this mirrors.
	const auto MarkUnmeasured = [](FGuidelineEdge& Half)
	{
		Half.MinRadius = 0.0;
		Half.ClearInner = -1.0;
		Half.ClearOuter = -1.0;
		Half.ClearInnerAt.Reset();
		Half.ClearOuterAt.Reset();
	};
	MarkUnmeasured(Head);
	MarkUnmeasured(Tail);

	// EndRefA/EndRefB name what a hand-authored end IS, not where it sits - Head's B and
	// Tail's A now point at the new split node rather than at whatever Original's ref named,
	// so those two must clear. The ends that did NOT move (Head's A, Tail's B) keep theirs.
	// Unreachable today only because IsJoinable requires bDerived and EndRef is only ever set
	// on a hand-authored (non-derived) edge - fixed anyway because SplitGuidelineEdge is a
	// public API this project already calls from three sites, and a future fourth should not
	// have to rediscover the same rule.
	Head.EndRefB = FGuidelineEndRef();
	Tail.EndRefA = FGuidelineEndRef();

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
	// Counted for SampleGuidelineCallCountForTest (#171), unconditionally and first: a route
	// search that reads FGuidelineEdge::Length instead of calling this must show zero calls
	// per Find, and a count taken after an early-out below would miss exactly the callers a
	// stale cache would make MORE of.
	++SampleGuidelineCalls;

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
	const FGuidelineNode* Found = GetGuidelineNode(Node);
	if (Found == nullptr)
	{
		return false;
	}
	// Not the player's: a runway-holding position is derived from the junction on every
	// build, so a clear here would come back next rebuild and a set would be a no-op that
	// looked like one. Refusing says so. A road's taxiway-crossing stop line is derived the
	// same way, for the same reason.
	if (Found->HoldingPosition == EHoldingPositionKind::Runway
		|| Found->HoldingPosition == EHoldingPositionKind::TaxiwayCrossing)
	{
		return false;
	}
	// Copied before the write below: SetGuidelineNodeHoldingPosition touches HoldingPosition/
	// HoldingPositionFor only, but Origin is read through the same pointer and a const one
	// is what this function should be holding once it stops writing fields by hand (#191).
	const FGuidelineEndRef At = Found->Origin;
	SetGuidelineNodeHoldingPosition(Node,
		bSet ? EHoldingPositionKind::Intermediate : EHoldingPositionKind::None, FRoadSegmentId());
	// THE PLAYER'S BAR IS A FACT A PLAN READS (where a taxiing aircraft stops) - see NoteFactChanged (#446).
	// Here, not in SetGuidelineNodeHoldingPosition, which the derivation also calls for the derived kinds.
	NoteFactChanged();

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
	// A set Protects must be a live runway. Refusing beats storing it: the arbiter expands
	// whatever a position names through RunwayChain, and a taxiway named there would hand
	// a crossing agent a strip made of the taxiway it is standing on.
	if (GetGuidelineNode(Node) == nullptr || !Protects.IsSet() || !IsRunwaySegment(Protects))
	{
		return false;
	}
	return SetGuidelineNodeHoldingPosition(Node, EHoldingPositionKind::Runway, Protects);
}

bool URoadNetwork::SetGuidelineNodeOrigin(FGuidelineNodeId Node, const FGuidelineEndRef& Origin)
{
	FGuidelineNode* Found = GetGuidelineNodeMutable(Node);
	if (Found == nullptr)
	{
		return false;
	}
	Found->Origin = Origin;
	return true;
}

bool URoadNetwork::SetGuidelineNodeHoldingPosition(FGuidelineNodeId Node, EHoldingPositionKind Kind, FRoadSegmentId For)
{
	FGuidelineNode* Found = GetGuidelineNodeMutable(Node);
	if (Found == nullptr)
	{
		return false;
	}
	Found->HoldingPosition = Kind;
	Found->HoldingPositionFor = For;
	// Only SetGuidelineNodeCrossingHold writes a list; any other write of the kind ends it.
	Found->ProtectsConflicts.Reset();
	return true;
}

bool URoadNetwork::SetGuidelineNodeCrossingConflict(FGuidelineNodeId Node)
{
	FGuidelineNode* Found = GetGuidelineNodeMutable(Node);
	if (Found == nullptr)
	{
		return false;
	}
	Found->bCrossingConflict = true;
	return true;
}

bool URoadNetwork::SetGuidelineEdgeAllowedTraffic(FGuidelineEdgeId Edge, FTrafficMask Allowed)
{
	FGuidelineEdge* Found = GetGuidelineEdgeMutable(Edge);
	if (Found == nullptr)
	{
		return false;
	}
	Found->AllowedTraffic = Allowed;
	return true;
}

bool URoadNetwork::SetGuidelineNodeCrossingHold(FGuidelineNodeId Node, TArray<FGuidelineNodeId> Conflicts)
{
	FGuidelineNode* Found = GetGuidelineNodeMutable(Node);
	if (Found == nullptr)
	{
		return false;
	}
	Found->HoldingPosition = EHoldingPositionKind::TaxiwayCrossing;
	Found->HoldingPositionFor = FRoadSegmentId();
	Found->ProtectsConflicts = MoveTemp(Conflicts);
	return true;
}

bool URoadNetwork::SetGuidelineEdgeMeasurement(FGuidelineEdgeId Edge, double MinRadius, double ClearInner,
	double ClearOuter, TArray<float> ClearInnerAt, TArray<float> ClearOuterAt)
{
	FGuidelineEdge* Found = GetGuidelineEdgeMutable(Edge);
	if (Found == nullptr)
	{
		return false;
	}
	Found->MinRadius = MinRadius;
	Found->ClearInner = ClearInner;
	Found->ClearOuter = ClearOuter;
	Found->ClearInnerAt = MoveTemp(ClearInnerAt);
	Found->ClearOuterAt = MoveTemp(ClearOuterAt);
	return true;
}

bool FRoadNetworkTestAccess::MarkGuidelineEdgeEditedForTest(FGuidelineEdgeId Edge, double MaxWingspan)
{
	FGuidelineEdge* Found = Network.GetGuidelineEdgeMutable(Edge);
	if (Found == nullptr)
	{
		return false;
	}
	Found->bDerived = false;
	Found->MaxWingspan = MaxWingspan;
	return true;
}

bool FRoadNetworkTestAccess::SetGuidelineNodePriorityOverrideForTest(FGuidelineNodeId Node,
	TArray<ETraversalClass> PriorityOverride)
{
	FGuidelineNode* Found = Network.GetGuidelineNodeMutable(Node);
	if (Found == nullptr)
	{
		return false;
	}
	Found->PriorityOverride = MoveTemp(PriorityOverride);
	return true;
}

bool FRoadNetworkTestAccess::SetEntityOutlineForTest(FEntityInstanceId Entity, TArray<FVector2D> Outline)
{
	FEntityInstance* Found = Network.GetEntityMutable(Entity);
	if (Found == nullptr)
	{
		return false;
	}
	Found->Outline = MoveTemp(Outline);
	// THE FRONTAGE FOLLOWS THE OUTLINE IT INDEXES (FEntityInstance::FrontageEdge): a stand's entrance is the one search's answer for the outline just
	// written, or none for an outline of under three points. A DEPOT'S is left alone - nothing here can know which edge a drawn depot faced.
	if (Found->IsStand())
	{
		Found->FrontageEdge = StandBox::EntranceEdgeOf(Found->Outline, Found->Position,
			FVector2D(FMath::Cos(Found->Heading), FMath::Sin(Found->Heading)));
	}
	return true;
}

void FRoadNetworkTestAccess::ClearStandNumbersForTest()
{
	for (FEntityInstance& Instance : Network.Entities)
	{
		Instance.StandNumber = 0;
		Instance.DepotNumber = 0;
	}
	Network.NextStandNumber = 1;
	Network.NextDepotNumber = 1;
}

bool FRoadNetworkTestAccess::SetEntityFrontageForTest(FEntityInstanceId Entity, int32 FrontageEdge)
{
	FEntityInstance* Found = Network.GetEntityMutable(Entity);
	if (Found == nullptr)
	{
		return false;
	}
	Found->FrontageEdge = FrontageEdge;
	return true;
}

bool FRoadNetworkTestAccess::SetEntityPavementForTest(FEntityInstanceId Entity, EPavement Pavement)
{
	FEntityInstance* Found = Network.GetEntityMutable(Entity);
	if (Found == nullptr)
	{
		return false;
	}
	Found->Pavement = Pavement;
	return true;
}

bool FRoadNetworkTestAccess::ClearRunwayInUseForTest(FRoadSegmentId Seed)
{
	const TArray<FRoadSegmentId> Chain = Network.RunwayChain(Seed);
	for (const FRoadSegmentId& Member : Chain)
	{
		if (FRoadSegment* Segment = Network.GetSegmentMutable(Member))
		{
			Segment->Runway.InUse = 0;
		}
	}
	return !Chain.IsEmpty();
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

void URoadNetwork::ForEachOutgoingGuideline(
	FGuidelineNodeId Node, ETraversalClass Class, TFunctionRef<void(FGuidelineEdgeId)> Visit) const
{
	const FGuidelineNode* Found = RoadSlot::Get<FGuidelineNodeId>(GuidelineNodes, Node);
	if (Found == nullptr)
	{
		return;
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
			Visit(Id);
		}
	}
}

TArray<FGuidelineEdgeId> URoadNetwork::GetOutgoingGuidelines(
	FGuidelineNodeId Node, ETraversalClass Class) const
{
	// A thin forwarder onto ForEachOutgoingGuideline (#171), added so RouteSearch's inner loop
	// could stop paying for this array on every node expansion - see that method's own comment.
	// GroundTrafficRebuild was the last production caller still building this array just to
	// walk it looking for one match (#190); it now calls ForEachOutgoingGuideline directly, so
	// this function's only remaining callers are the tests that genuinely want the TArray, from
	// the one incidence-and-direction rule rather than a second copy of it.
	TArray<FGuidelineEdgeId> Out;
	ForEachOutgoingGuideline(Node, Class, [&Out](FGuidelineEdgeId Id) { Out.Add(Id); });
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

void URoadNetwork::RemoveReverseTurnAt(int32 Index)
{
	if (ReverseTurns.IsValidIndex(Index))
	{
		ReverseTurns.RemoveAt(Index);
	}
	if (ReverseTurnEnds.IsValidIndex(Index))
	{
		ReverseTurnEnds.RemoveAt(Index);
	}
}

FApronId URoadNetwork::AddApron(FApronSurface&& Apron)
{
	// Every mutation moves a clock (#446): an apron is where a stand may stand - see NoteFactChanged.
	NoteFactChanged();
	return RoadSlot::Add<FApronId>(Aprons, ApronFreeList, MoveTemp(Apron));
}

bool URoadNetwork::RemoveApron(FApronId Apron)
{
	const bool bRemoved = RoadSlot::Remove<FApronId>(Aprons, ApronFreeList, Apron);
	if (bRemoved)
	{
		NoteFactChanged();
	}
	return bRemoved;
}

const FApronSurface* URoadNetwork::GetApron(FApronId Apron) const
{
	return RoadSlot::Get<FApronId>(Aprons, Apron);
}

FApronId URoadNetwork::ApronIdAt(int32 Index) const
{
	return RoadSlot::HandleAt<FApronId>(Aprons, Index);
}

void URoadNetwork::PostLoad()
{
	Super::PostLoad();
	EnsureStandOutlines();
	EnsureStandNumbers();
	EnsureDepotFrontages();
	// AFTER EnsureStandOutlines, which gives a legacy stand the outline its entrance is an edge of.
	// ENFORCED BY: Airside.Model.StandFrontage.MigrationStoresTheEntranceOnce (the PostLoad half's outline-less stand gets its box and then edge 0)
	EnsureStandFrontages();
}

void URoadNetwork::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	// ONLY A REAL LOAD: a reference collector or a memory count also comes through Serialize and changes nothing.
	// NOR A DUPLICATE (PPF_Duplicate): FDuplicateDataReader loads too, but DuplicateObject makes a NEW object - an undo
	// Memento, a rollback point, a PIE copy - whose clocks start at zero with nothing derived from it alive to fool, and
	// URoadNetwork::RestoreFrom's clock rule (#437) reads a snapshot's clocks as exactly that. Bumping here made every
	// Memento read 1, and a snapshot's clock a lie about what it had seen.
	// ENFORCED BY: Airside.Model.RestoreFromMovesClocksForward ("control: a duplicate starts its clocks at zero")
	if (!Ar.IsLoading() || Ar.IsObjectReferenceCollector() || Ar.IsCountingMemory() || (Ar.GetPortFlags() & PPF_Duplicate) != 0)
	{
		return;
	}

	// THE CLOCKS MOVE, THE "DERIVED FROM" STAMP MOVES WITH THEM - see the header. The road and the guideline graph
	// just arrived together, so whether the one was derived from the other is exactly what it was before the load;
	// a load must not make AreGuidelinesBehindRoad say yes on its own (an editor undo rebuilds nothing after it).
	const bool bWasDerived = GuidelinesDerivedAt == EditRevision;
	++EditRevision;
	++GuidelineRevision;
	if (bWasDerived)
	{
		GuidelinesDerivedAt = EditRevision;
	}
}

URoadProfile* URoadNetwork::TransientDefaultProfile() const
{
	return DefaultProfile != nullptr && DefaultProfile->IsIn(GetTransientPackage()) ? DefaultProfile.Get() : nullptr;
}

int32 URoadNetwork::RepointTransientDefaultProfile(URoadProfile* Default)
{
	// ONLY WHEN THE SAVED DEFAULT WAS A TRANSIENT OBJECT, AND NOT ALREADY THIS ACTOR'S - see the header. A content-asset
	// default resolves to itself in every session, and a segment naming it keeps it; the same session's same actor
	// re-finds its own fallback, which is already right.
	URoadProfile* Saved = TransientDefaultProfile();
	if (Default == nullptr || Saved == nullptr || Saved == Default)
	{
		return 0;
	}
	int32 Repointed = 0;
	for (FRoadSegment& Segment : Segments)
	{
		// BY IDENTITY WITH THE SAVED DEFAULT - see the header.
		if (Segment.bAlive && Segment.Profile == Saved)
		{
			Segment.Profile = Default;
			++Repointed;
		}
	}
	// A NEW PROFILE IS NEW GEOMETRY (SetSegmentProfile's rule), so a cache keyed on EditRevision must hear it - the
	// save game's Serialize has moved the clock already, but this must not depend on its caller having done so.
	if (Repointed > 0)
	{
		++EditRevision;
	}
	DefaultProfile = Default;
	return Repointed;
}

bool URoadNetwork::GiveStandOutlineIfMissing(FEntityInstance& Instance, const FLetterEnvelope& CodeCEnvelope)
{
	if (!Instance.IsStand() || Instance.Outline.Num() >= 3)
	{
		return false;
	}

	// Same Heading -> Facing conversion PlaceEntity's own anchor resolution below uses -
	// see its Cos/Sin two lines down. Position is already the nose-gear stop mark (the pose
	// StandBox::BoxAt wants), captured at placement or loaded straight off the instance.
	StandBox::FStandPose Pose;
	Pose.Position = Instance.Position;
	Pose.Facing = FVector2D(FMath::Cos(Instance.Heading), FMath::Sin(Instance.Heading));
	// CodeCEnvelope IS THE CALLER'S CHOICE (#292 review finding), not always the floor: see
	// this function's own header for why EnsureStandOutlines and PlaceEntity mean different
	// things by "the" Code C envelope, and StandBox::BoxAt's header for why Envelope must be
	// the SAME one the pose (if it came from PlaceEntity's live path) was built against.
	StandBox::BoxAt(Pose, EIcaoCode::C, CodeCEnvelope, Instance.Outline);
	return true;
}

int32 URoadNetwork::EnsureStandOutlines()
{
	// THE FLOOR, DELIBERATELY, NEVER THE FLEET-RESOLVED ENVELOPE (#292 review finding): this
	// runs at PostLoad, on a level saved before a stand's outline was captured at placement at
	// all, and its whole job is a MIGRATION - reproducing EXACTLY the pre-#292 box such a stand
	// has always had. Model/ cannot reach Content/ to ask for the resolved figure anyway (see
	// StandBox::PoseFor's header), but even if it could, using the fleet's CURRENT envelope
	// here would make an old save's stand outline depend on which content happens to be loaded
	// the day it loads, and a migration whose output drifts with content is not a migration.
	//
	// ITS OUTPUT DID MOVE ONCE, ON PURPOSE (2026-09-26, far-side entry): StandBox::BoxAt builds
	// the box off EntranceSetback, so the same saved pose now gets a box whose entrance sits
	// MaxTailAft + wingtip clearance behind the stop mark rather than Depth - MaxNoseFwd - a
	// legacy stand's box moved away from its taxiway by the difference. That is a change of the
	// GEOMETRY RULE, made once in code and the same for every load, which is what this paragraph
	// allows; what it forbids is output that varies with the content loaded. The stop mark is
	// then re-derived from this box on load (UStandDefinitionCache::RebindStandDefinitions).
	const FLetterEnvelope CodeCFloor = IcaoCode::FloorEnvelopeForLetter(EIcaoCode::C);

	int32 Changed = 0;
	for (FEntityInstance& Instance : Entities)
	{
		if (!Instance.bAlive)
		{
			continue;
		}
		if (GiveStandOutlineIfMissing(Instance, CodeCFloor))
		{
			// See this function's own header comment on why the DesignWingspan write lives
			// HERE and not in GiveStandOutlineIfMissing: only a stand old enough to have
			// loaded with no outline at all is being pinned to a letter for the first time.
			if (Instance.DesignWingspan == 0.0)
			{
				Instance.DesignWingspan = IcaoCode::DesignSpanForLetter(EIcaoCode::C);
			}
			++Changed;
		}
	}

	if (Changed > 0)
	{
		UE_LOG(LogAirside, Log,
			TEXT("EnsureStandOutlines: %d legacy stand(s) given a Code C outline"), Changed);
	}
	return Changed;
}

int32 URoadNetwork::EnsureDepotFrontages()
{
	// A MIGRATION, NOT A READER (#450) - see the declaration. The nearest-midpoint search survives HERE because it is the
	// one fact a depot placed before FEntityInstance::FrontageEdge kept: PlaceEntityInPlot stored Position at the midpoint of the
	// frontage, so this recovers exactly the edge it was given. A depot the facade places now stores its own edge and is skipped.
	int32 Changed = 0;
	for (FEntityInstance& Instance : Entities)
	{
		if (!Instance.bAlive || !Instance.IsDepot() || !Instance.IsPlotted() || Instance.FrontageEdge != INDEX_NONE)
		{
			continue;
		}

		double BestDistance = TNumericLimits<double>::Max();
		int32 BestEdge = INDEX_NONE;
		for (int32 Corner = 0; Corner < Instance.Outline.Num(); ++Corner)
		{
			const FVector2D Mid = (Instance.Outline[Corner] + Instance.Outline[(Corner + 1) % Instance.Outline.Num()]) * 0.5;
			const double Distance = FVector2D::Distance(Mid, Instance.Position);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				BestEdge = Corner;
			}
		}
		Instance.FrontageEdge = BestEdge;
		++Changed;
	}

	if (Changed > 0)
	{
		UE_LOG(LogAirside, Log,
			TEXT("EnsureDepotFrontages: %d legacy plotted depot(s) given a stored frontage edge"), Changed);
	}
	return Changed;
}

int32 URoadNetwork::EnsureStandFrontages()
{
	// A MIGRATION, NOT A READER (#450's leftover) - see the declaration, and EnsureDepotFrontages above for its sibling. The search survives HERE
	// because it is the one fact a stand saved before FEntityInstance::FrontageEdge kept: the stop mark's facing and the outline. It is the rule the
	// stand cache (rearmost midpoint) used to run on every load, run once, so the pose repair and the paint read what they always found.
	int32 Changed = 0;
	for (FEntityInstance& Instance : Entities)
	{
		if (!Instance.bAlive || !Instance.IsStand() || !Instance.IsPlotted() || Instance.FrontageEdge != INDEX_NONE)
		{
			continue;
		}
		Instance.FrontageEdge = StandBox::EntranceEdgeOf(Instance.Outline, Instance.Position,
			FVector2D(FMath::Cos(Instance.Heading), FMath::Sin(Instance.Heading)));
		++Changed;
	}

	if (Changed > 0)
	{
		UE_LOG(LogAirside, Log,
			TEXT("EnsureStandFrontages: %d legacy drawn stand(s) given a stored entrance edge"), Changed);
	}
	return Changed;
}

int32 URoadNetwork::EnsureStandNumbers()
{
	// ONE BACKFILL, TWO KINDS (#490): the stand's rule and the depot's are the same rule over a different predicate, number field and counter, so it is
	// written once and run for each - a copy per kind is the shape that lets the second kind's never-reuse guard drift from the first's.
	const auto Backfill = [this](bool (FEntityInstance::*IsKind)() const, int32 FEntityInstance::* Number, int32& Counter) -> int32
	{
		// PAST EVERY NUMBER ALREADY HELD, not merely the counter: a counter that somehow sits
		// behind a live one (hand-edited data, a half-migrated save) must not issue that
		// number a second time. The never-reuse rule outranks the counter's value.
		int32 Next = FMath::Max(Counter, 1);
		for (const FEntityInstance& Instance : Entities)
		{
			if (Instance.bAlive && (Instance.*IsKind)())
			{
				Next = FMath::Max(Next, Instance.*Number + 1);
			}
		}

		int32 Numbered = 0;
		for (FEntityInstance& Instance : Entities)
		{
			if (Instance.bAlive && (Instance.*IsKind)() && Instance.*Number == 0)
			{
				Instance.*Number = Next++;
				++Numbered;
			}
		}
		Counter = Next;
		return Numbered;
	};

	const int32 Stands = Backfill(&FEntityInstance::IsStand, &FEntityInstance::StandNumber, NextStandNumber);
	if (Stands > 0)
	{
		UE_LOG(LogAirside, Log,
			TEXT("EnsureStandNumbers: %d legacy stand(s) numbered; next stand is %d"), Stands, NextStandNumber);
	}

	// THE DEPOTS, a level saved before 2026-10-01 (#490): every one loads at 0 and the counter at its default.
	const int32 Depots = Backfill(&FEntityInstance::IsDepot, &FEntityInstance::DepotNumber, NextDepotNumber);
	if (Depots > 0)
	{
		UE_LOG(LogAirside, Log,
			TEXT("EnsureStandNumbers: %d legacy depot(s) numbered; next depot is %d"), Depots, NextDepotNumber);
	}
	return Stands + Depots;
}

FEntityInstanceId URoadNetwork::PlaceEntity(
	UEntityDefinition* Definition, TConstArrayView<FEntityAnchor> Anchors,
	const FVector2D& Position, double Heading, double DesignWingspan, EServiceRole PoseRole,
	int32 Trucks, const FLetterEnvelope& CodeCEnvelope)
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
	return PlaceEntity(Placement, CodeCEnvelope);
}

FEntityInstanceId URoadNetwork::PlaceEntity(const FEntityPlacement& Placement, const FLetterEnvelope& CodeCEnvelope)
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
	Instance.Pavement = Placement.Pavement;

	// Captured for the same Model/-must-not-see-Entities/ reason as DesignWingspan, and read
	// by FAnchorLink to decide which class of guideline the pose's lead-in may join.
	Instance.PoseRole = Placement.PoseRole;

	Instance.Outline = Placement.Outline;

	// THE FRONTAGE RIDES WITH THE OUTLINE it indexes, copied in the same breath so a placement path cannot
	// store one without the other - see FEntityInstance::FrontageEdge.
	Instance.FrontageEdge = Placement.FrontageEdge;
	Instance.Modules = Placement.Modules;

	// ISSUED HERE, THE ONE FUNCTION EVERY PLACEMENT PATH ENDS IN (the facade's point and plot
	// gestures and the test fixtures all forward to it), and never reused - see
	// FEntityInstance::StandNumber. After PoseRole, which IsStand() reads.
	// ENFORCED BY: Airside.Model.StandNumbers (point placement) and
	// Airside.Present.StandPlot.RefusesOverlap (a plot-placed stand reads back as stand 1).
	if (Instance.IsStand())
	{
		Instance.StandNumber = NextStandNumber++;
	}

	// A DEPOT'S NUMBER, ISSUED IN THE SAME BREATH FOR THE SAME REASON (#490): from its own saved counter, never reused, so "depot 3" names one
	// depot for the life of the airport and not whichever one the recycled slot holds. A separate counter - the stand's is painted on the ground.
	// ENFORCED BY: Airside.Model.DepotNumbers (point placement) and Airside.Entities.DepotNumberSurvivesUndoAndRedo (a plot-placed depot)
	if (Instance.IsDepot())
	{
		Instance.DepotNumber = NextDepotNumber++;
	}

	// TASK 6 / RULING 6: a stand placed with no drawn plot (Placement.Outline empty) gets its
	// Code C box RIGHT HERE, not only the next time EnsureStandOutlines runs at load. This is
	// the legacy point path's own placement, not just PlaceStand's - the facade's PlaceEntity
	// forwards through the other overload into this one, so covering this one function covers
	// both. A drawn stand's own Outline (>= 3 points already) and a depot (never IsStand())
	// are both left exactly as given - see GiveStandOutlineIfMissing's guard.
	//
	// CodeCEnvelope IS THE CALLER'S: this overload's own default is the floor, purely so the
	// ~thirty existing callers keep compiling (see the header) - URoadEditFacade::PlaceEntity,
	// the one LIVE point-placement gesture, passes UAirsideSettings::ResolveLetterEnvelope
	// explicitly instead, so this box matches the ghost preview and the drawn-stand commit
	// path, which read the same resolved figure. See PlaceEntity's own header comment.
	GiveStandOutlineIfMissing(Instance, CodeCEnvelope);

	// A STAND THAT WAS GIVEN NO ENTRANCE GETS THE ONE SEARCH'S ANSWER, HERE AT THE DOOR (#450's leftover): the facade's PlaceStandInPlot states the edge it
	// was GIVEN, but a point-placed stand (whose Code C box was just made above) and a fixture that hands in an outline say none, and the stand
	// readers (UStandDefinitionCache::PoseFromOutline, FStandMarkingBuilder::FrameFor) read the stored edge and no longer search. After the box, which is what
	// the entrance is an edge OF. A depot is not asked: its frontage is the gesture's to state, and with none it is unsolvable, as it always was.
	// ENFORCED BY: Airside.Model.StandFrontage.StoredEdgeIsTodaysHeuristicAnswer (the point-placed and fixture stands)
	if (Instance.IsStand() && Instance.IsPlotted() && Instance.FrontageEdge == INDEX_NONE)
	{
		Instance.FrontageEdge = StandBox::EntranceEdgeOf(Instance.Outline, Instance.Position,
			FVector2D(FMath::Cos(Instance.Heading), FMath::Sin(Instance.Heading)));
	}

	// THE STARTER FLEET, AS STATED - never derived from the sheds since 2026-09-29 (facility-upgrades
	// spec §6): a shed is a BAY the player fills by buying a vehicle (R2), so a count derived from the
	// sheds would hand out the vehicles the player is meant to buy. The player's plotted depot states 0
	// (URoadEditFacade::PlaceEntityInPlot, R3); plotless callers state their definition's count, which
	// UJobBoard's placeholder seeding turns into vehicles once.
	// ENFORCED BY: Airside.Entities.StarterTrucksAreStated, Airside.Present.PlayerDepotStartsWithNoTrucks
	Instance.Trucks = Placement.Trucks;

	Instance.ResolvedAnchors.Reserve(Placement.Anchors.Num());

	// The stop position itself, as a node an aircraft can be routed to. NON-DERIVED for
	// the same reason the anchor nodes are: it carries no edge until a lead-in is cast to
	// it, and a derived one would be swept by the next rebuild.
	//
	// ONE OF THEM, however many bays the plot holds. BuildFuelDepot already ruled that two
	// lead-ins from one small building into one road is a duplicate painted line, so a
	// depot's sheds are capacity and visuals - the yard's single gate is the pose.
	const double Cos = FMath::Cos(Placement.Heading);
	const double Sin = FMath::Sin(Placement.Heading);

	// SET BACK ALONG THE HEADING when the placement asks - a drawn depot's trucks live inside
	// its gate, not on it. See FEntityPlacement::PoseSetbackUu.
	Instance.PoseNode = AddGuidelineNode(
		Placement.Position + FVector2D(Cos, Sin) * Placement.PoseSetbackUu, /*bDerived=*/false);

	for (const FEntityAnchor& Anchor : Placement.Anchors)
	{
		// Local to world. Rotating by the entity's heading is what makes an anchor mean
		// "off the aircraft's left wing" rather than "somewhere north of here".
		const FVector2D World = Anchor.WorldAt(Placement.Position, Placement.Heading);

		// NON-DERIVED. See the header: an anchor node has no incident edges until a
		// guideline is drawn to it, so a derived one would be swept by the next rebuild
		// and this handle would dangle.
		FResolvedAnchor Resolved;
		Resolved.Id = Anchor.Id;
		Resolved.Node = AddGuidelineNode(World, /*bDerived=*/false);

		// Captured rather than left on the definition - see FResolvedAnchor's comment.
		// GetAnchorWorldHeading and FirstAnchorIdForRole read these back instead of
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

FEntityInstance* URoadNetwork::GetEntityMutable(FEntityInstanceId Entity)
{
	return RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
}

int32 URoadNetwork::FindEntityIndexByPoseNode(FGuidelineNodeId Node) const
{
	if (!Node.IsSet())
	{
		return INDEX_NONE;
	}

	// REBUILT ONCE PER GuidelineRevision, not per call - see PoseNodeIndex's own comment.
	// A revision match means Entities has not moved since the last rebuild, so the map is
	// exactly as current as a fresh scan would be.
	if (PoseNodeIndexRevision != GuidelineRevision)
	{
		PoseNodeIndex.Reset();
		PoseNodeIndex.Reserve(Entities.Num());
		for (int32 Index = 0; Index < Entities.Num(); ++Index)
		{
			if (Entities[Index].bAlive)
			{
				PoseNodeIndex.Add(Entities[Index].PoseNode, Index);
			}
		}
		PoseNodeIndexRevision = GuidelineRevision;
	}

	const int32* Found = PoseNodeIndex.Find(Node);
	return Found != nullptr ? *Found : INDEX_NONE;
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

FName URoadNetwork::FirstAnchorIdForRole(FEntityInstanceId Entity, EServiceRole Role) const
{
	// Reads FResolvedAnchor::Role rather than filtering the definition's own anchors and
	// checking each one against ResolvedAnchors - ResolvedAnchors already holds only ids
	// this INSTANCE actually resolved, so iterating it directly cannot hand back an id a
	// definition edited after placement would leave leading nowhere.
	const FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr)
	{
		return NAME_None;
	}

	for (const FResolvedAnchor& Resolved : Instance->ResolvedAnchors)
	{
		if (Resolved.Role == Role)
		{
			return Resolved.Id;
		}
	}
	return NAME_None;
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

bool URoadNetwork::SetEntityDefinition(FEntityInstanceId Entity, UEntityDefinition* Definition)
{
	FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr)
	{
		return false;
	}

	Instance->Definition = Definition;
	// What a stand admits is read from its definition - see NoteFactChanged (#446).
	NoteFactChanged();
	return true;
}

bool URoadNetwork::SetStandDesignWingspan(FEntityInstanceId Entity, double DesignWingspan)
{
	FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr || !Instance->IsStand())
	{
		return false;
	}
	Instance->DesignWingspan = DesignWingspan;
	// The widest aeroplane a stand takes - admission reads it; see NoteFactChanged (#446).
	NoteFactChanged();
	return true;
}

bool URoadNetwork::RePoseStand(FEntityInstanceId Entity, const FVector2D& Position, double Heading,
	TConstArrayView<FEntityAnchor> Anchors)
{
	FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr || !Instance->IsStand())
	{
		return false;
	}

	Instance->Position = Position;
	Instance->Heading = Heading;

	// THE POSE NODE MOVES AND KEEPS ITS HANDLE - see the header. Its edges go: they were the
	// lead-in to where the stop mark used to be, and the rebuild casts a new one.
	if (FGuidelineNode* Pose = GetGuidelineNodeMutable(Instance->PoseNode))
	{
		const TArray<FGuidelineEdgeId> Doomed = Pose->Incident;
		for (const FGuidelineEdgeId Edge : Doomed)
		{
			RemoveGuidelineEdge(Edge);
		}
		// RE-READ after the removals, which touch the node array's incidence lists.
		if (FGuidelineNode* Moved = GetGuidelineNodeMutable(Instance->PoseNode))
		{
			Moved->Position = Position;
		}
	}

	// THE ANCHORS, RE-CAPTURED WHOLE, by the same local-to-world rule PlaceEntity uses. Copied out
	// first: removing a node does not touch Entities, but the list is about to be rebuilt.
	const TArray<FResolvedAnchor> Old = MoveTemp(Instance->ResolvedAnchors);
	Instance->ResolvedAnchors.Reset();
	for (const FResolvedAnchor& Gone : Old)
	{
		RemoveGuidelineNode(Gone.Node);
	}

	TArray<FResolvedAnchor> Fresh;
	Fresh.Reserve(Anchors.Num());
	for (const FEntityAnchor& Anchor : Anchors)
	{
		const FVector2D World = Anchor.WorldAt(Position, Heading);
		FResolvedAnchor Resolved;
		Resolved.Id = Anchor.Id;
		Resolved.Node = AddGuidelineNode(World, /*bDerived=*/false);
		Resolved.LocalHeading = Anchor.LocalHeading;
		Resolved.Role = Anchor.Role;
		Fresh.Add(Resolved);
	}

	// RE-FOUND, NOT HELD: AddGuidelineNode grows the node array, not Entities, but the instance is
	// looked up again rather than trusting a pointer across four kinds of mutation.
	if (FEntityInstance* Again = RoadSlot::Get<FEntityInstanceId>(Entities, Entity))
	{
		Again->ResolvedAnchors = MoveTemp(Fresh);
	}
	++GuidelineRevision;
	return true;
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

bool URoadNetwork::AddEntityModule(FEntityInstanceId Entity, EDepotModule Module)
{
	FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr || !Instance->bAlive || !Instance->IsDepot())
	{
		return false;
	}
	Instance->Modules.Add(Module);
	// A purchase re-derives nothing now (the facade's Facts notify), so this is what tells the job board and the
	// flight board's fuel verdict the depot changed - see the header and NoteFactChanged (#446).
	NoteFactChanged();
	return true;
}

int32 URoadNetwork::RemoveEntityModules(FEntityInstanceId Entity, EDepotModule Module, int32 Count)
{
	FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr || !Instance->bAlive || !Instance->IsDepot())
	{
		return 0;
	}
	// FROM THE END, so the modules bought last go first and the start kit is the last of a kind to leave - the order the
	// list was written in, unwound. Which one goes changes nothing drawn: the presenter lights a run from its count.
	int32 Removed = 0;
	for (int32 Index = Instance->Modules.Num() - 1; Index >= 0 && Removed < Count; --Index)
	{
		if (Instance->Modules[Index] == Module)
		{
			Instance->Modules.RemoveAt(Index);
			++Removed;
		}
	}
	// AddEntityModule's reason (#446); nothing removed, nothing changed, no stamp.
	if (Removed > 0)
	{
		NoteFactChanged();
	}
	return Removed;
}
