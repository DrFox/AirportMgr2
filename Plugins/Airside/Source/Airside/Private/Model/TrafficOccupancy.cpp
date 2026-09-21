#include "Model/TrafficOccupancy.h"

FTrafficResource FTrafficResource::OfEdge(FGuidelineEdgeId Id)
{
	FTrafficResource R; R.Kind = ETrafficResourceKind::Edge; R.Edge = Id; return R;
}
FTrafficResource FTrafficResource::OfNode(FGuidelineNodeId Id)
{
	FTrafficResource R; R.Kind = ETrafficResourceKind::Node; R.Node = Id; return R;
}
FTrafficResource FTrafficResource::OfSurface(FRoadSegmentId Id)
{
	FTrafficResource R; R.Kind = ETrafficResourceKind::Surface; R.Surface = Id; return R;
}

bool FTrafficResource::operator==(const FTrafficResource& Other) const
{
	if (Kind != Other.Kind) { return false; }
	switch (Kind)
	{
	case ETrafficResourceKind::Edge:    return Edge == Other.Edge;
	case ETrafficResourceKind::Node:    return Node == Other.Node;
	case ETrafficResourceKind::Surface: return Surface == Other.Surface;
	default: return false;
	}
}

FString FTrafficResource::Describe() const
{
	switch (Kind)
	{
	case ETrafficResourceKind::Edge:    return FString::Printf(TEXT("edge %d"), Edge.Index);
	case ETrafficResourceKind::Node:    return FString::Printf(TEXT("node %d"), Node.Index);
	case ETrafficResourceKind::Surface: return FString::Printf(TEXT("runway segment %d"), Surface.Index);
	default: return TEXT("?");
	}
}

bool FTrafficClaim::Conflicts(const FTrafficClaim& Other) const
{
	if (Resource != Other.Resource)
	{
		return false;
	}
	if (Resource.Kind != ETrafficResourceKind::Edge)
	{
		return true;
	}
	// Half-open: a queue packed end to end is not in conflict with itself.
	return From < Other.To && Other.From < To;
}

FTrafficClaim FTrafficClaim::Make(int32 AgentId, const FTrafficResource& Resource, bool bOccupied, int32 Rank)
{
	FTrafficClaim Claim;
	Claim.AgentId = AgentId;
	Claim.Resource = Resource;
	Claim.bOccupied = bOccupied;
	Claim.Rank = Rank;
	return Claim;
}

void FTrafficOccupancy::Assert(const FTrafficClaim& Claim)
{
	FTrafficClaim Blocker;
	TryClaim(Claim, Blocker);
}

void FTrafficOccupancy::IndexClaim(int32 Index)
{
	ByResource.FindOrAdd(Claims[Index].Resource).Add(Index);
	ByAgent.FindOrAdd(Claims[Index].AgentId).Add(Index);
}

void FTrafficOccupancy::UnindexClaim(int32 Index)
{
	const FTrafficClaim& C = Claims[Index];
	if (TArray<int32>* Bucket = ByResource.Find(C.Resource))
	{
		Bucket->RemoveSingleSwap(Index);
		if (Bucket->Num() == 0) { ByResource.Remove(C.Resource); }
	}
	if (TArray<int32>* Bucket = ByAgent.Find(C.AgentId))
	{
		Bucket->RemoveSingleSwap(Index);
		if (Bucket->Num() == 0) { ByAgent.Remove(C.AgentId); }
	}
}

void FTrafficOccupancy::AddClaim(const FTrafficClaim& Claim)
{
	Claims.Add(Claim);
	IndexClaim(Claims.Num() - 1);
}

void FTrafficOccupancy::RemoveClaimAtSwap(int32 Index)
{
	UnindexClaim(Index);

	// RemoveAtSwap is about to move the LAST claim into Index - unless Index already IS the
	// last one, in which case nothing moves and there is nothing left to repoint.
	const int32 LastIndex = Claims.Num() - 1;
	if (Index != LastIndex)
	{
		const FTrafficClaim& Moved = Claims[LastIndex];
		if (TArray<int32>* Bucket = ByResource.Find(Moved.Resource))
		{
			const int32 At = Bucket->Find(LastIndex);
			if (At != INDEX_NONE) { (*Bucket)[At] = Index; }
		}
		if (TArray<int32>* Bucket = ByAgent.Find(Moved.AgentId))
		{
			const int32 At = Bucket->Find(LastIndex);
			if (At != INDEX_NONE) { (*Bucket)[At] = Index; }
		}
	}
	Claims.RemoveAtSwap(Index);
}

EClaimResult FTrafficOccupancy::TryClaim(const FTrafficClaim& Claim, FTrafficClaim& OutBlocker)
{
	// A same-agent claim on the same resource is an UPDATE: checked against everyone else
	// like a fresh claim, then written over the old one in place. Unconditional replacement
	// was rejected: a follower whose window grows each tick would re-claim its own edge and
	// be granted straight through the leader's occupied interval - the pass-through defect
	// this table exists to end.
	LastTryClaimComparesForTest = 0;

	// NOBODY HOLDS THIS RESOURCE AT ALL: the common case at an airport's scale (issue #168),
	// and the whole reason ByResource exists - granted with zero comparisons rather than a
	// scan of every claim on every OTHER resource in the table.
	TArray<int32>* Bucket = ByResource.Find(Claim.Resource);
	if (Bucket == nullptr)
	{
		AddClaim(Claim);
		return EClaimResult::Granted;
	}

	// Find an existing claim by the same agent, and conflicts against OTHER agents' claims -
	// BOTH out of the same bucket, which holds every claim on this resource and nothing else.
	int32 ExistingIndex = INDEX_NONE;
	TArray<int32> ToPreempt;
	for (int32 Index : *Bucket)
	{
		const FTrafficClaim& Existing = Claims[Index];
		if (Existing.AgentId == Claim.AgentId)
		{
			ExistingIndex = Index;
			continue;
		}

		++LastTryClaimComparesForTest;
		if (!Existing.Conflicts(Claim))
		{
			continue;
		}

		// OCCUPIED IS ABSOLUTE, from both sides.
		//
		// Nobody is evicted from ground they are standing on, so an existing occupancy
		// refuses everything - including another occupancy, which is two bodies in one place
		// and the caller's problem to report.
		if (Existing.bOccupied)
		{
			OutBlocker = Existing;
			return EClaimResult::Held;
		}

		// And PRESENCE BEATS A RESERVATION, whatever the ranks say: spec §3.3 promises the
		// occupant will never be moved, so a reservation on ground somebody is already
		// standing on was never a claim anyone could act on. Leaving it in the table cost a
		// deadlock its cycle - the occupant was refused its OWN ground, abandoned the rest of
		// its claim pass, and the wait-for graph became a fan into the reserver instead of a
		// ring. See Airside.Model.Traffic.BoxEntry.
		if (!Claim.bOccupied && Existing.Rank >= Claim.Rank)
		{
			// Equal rank keeps the holder - that IS first-to-reserve.
			OutBlocker = Existing;
			return EClaimResult::Held;
		}
		ToPreempt.Add(Index);
	}

	// Update or add the claim BEFORE removing preempted claims, so indices stay valid.
	// The agent's own claim is never in ToPreempt (we skip AgentId), so it won't move.
	if (ExistingIndex != INDEX_NONE)
	{
		// Resource and AgentId are unchanged (that is what made this the existing claim),
		// so neither index needs touching - only the interval/occupied/rank fields differ.
		Claims[ExistingIndex] = Claim;
	}
	else
	{
		AddClaim(Claim);
	}

	// Now preempt lower-ranked reservations, HIGHEST INDEX FIRST: RemoveClaimAtSwap moves the
	// table's last claim into the slot it empties, so removing back-to-front never reshuffles
	// an index this loop has not reached yet out from under it. SORTED explicitly - ToPreempt
	// was built walking ByResource's bucket, whose order is insertion order, not the ascending
	// scan the old flat-array version got for free.
	ToPreempt.Sort();
	for (int32 At = ToPreempt.Num() - 1; At >= 0; --At)
	{
		Preempted.Add(Claims[ToPreempt[At]].AgentId);
		RemoveClaimAtSwap(ToPreempt[At]);
	}

	return EClaimResult::Granted;
}

const FTrafficClaim* FTrafficOccupancy::FindClaim(int32 AgentId, const FTrafficResource& Resource) const
{
	const TArray<int32>* Bucket = ByResource.Find(Resource);
	if (Bucket == nullptr)
	{
		return nullptr;
	}
	for (int32 Index : *Bucket)
	{
		if (Claims[Index].AgentId == AgentId)
		{
			return &Claims[Index];
		}
	}
	return nullptr;
}

void FTrafficOccupancy::ReleaseWhere(TFunctionRef<bool(const FTrafficClaim&)> Predicate)
{
	// Collect first, remove highest-index-first after: the same reasoning as TryClaim's
	// ToPreempt loop above, generalised to an arbitrary predicate over the whole table.
	TArray<int32> ToRemove;
	for (int32 Index = 0; Index < Claims.Num(); ++Index)
	{
		if (Predicate(Claims[Index]))
		{
			ToRemove.Add(Index);
		}
	}
	for (int32 At = ToRemove.Num() - 1; At >= 0; --At)
	{
		RemoveClaimAtSwap(ToRemove[At]);
	}
}

void FTrafficOccupancy::ReleaseAgentWhere(int32 AgentId, TFunctionRef<bool(const FTrafficClaim&)> Predicate)
{
	const TArray<int32>* Bucket = ByAgent.Find(AgentId);
	if (Bucket == nullptr)
	{
		return;
	}

	// Copied out of the bucket before anything is removed: RemoveClaimAtSwap mutates this
	// very TArray (it is ByAgent[AgentId]), and the same highest-index-first rule as
	// ReleaseWhere applies to whatever indices survive the predicate.
	TArray<int32> ToRemove;
	for (int32 Index : *Bucket)
	{
		if (Predicate(Claims[Index]))
		{
			ToRemove.Add(Index);
		}
	}
	ToRemove.Sort([](int32 A, int32 B) { return A > B; });
	for (int32 Index : ToRemove)
	{
		RemoveClaimAtSwap(Index);
	}
}

void FTrafficOccupancy::ReleaseAll(int32 AgentId)
{
	ReleaseAgentWhere(AgentId, [](const FTrafficClaim&) { return true; });
}

void FTrafficOccupancy::Release(int32 AgentId, const FTrafficResource& Resource)
{
	ReleaseAgentWhere(AgentId, [&Resource](const FTrafficClaim& C)
	{
		return C.Resource == Resource;
	});
}

void FTrafficOccupancy::ReleaseReservations(int32 AgentId)
{
	// bOccupied IS THE WHOLE TEST, and it is the same one TryClaim arbitrates on: a claim
	// that contains the agent's own position is where its body is, and nothing a caller does
	// to its PLAN can move a body. See the header for the landing this cost.
	ReleaseAgentWhere(AgentId, [](const FTrafficClaim& C) { return !C.bOccupied; });
}

void FTrafficOccupancy::ReleaseExcept(int32 AgentId, const TArray<FTrafficResource>& Keep)
{
	ReleaseAgentWhere(AgentId, [&Keep](const FTrafficClaim& C)
	{
		return !Keep.Contains(C.Resource);
	});
}

double FTrafficOccupancy::HeldLengthOn(FGuidelineEdgeId Edge, int32 ExcludingAgent) const
{
	const TArray<int32>* Bucket = ByResource.Find(FTrafficResource::OfEdge(Edge));
	if (Bucket == nullptr)
	{
		return 0.0;
	}
	double Sum = 0.0;
	for (int32 Index : *Bucket)
	{
		const FTrafficClaim& C = Claims[Index];
		if (C.AgentId != ExcludingAgent)
		{
			Sum += FMath::Max(0.0, C.To - C.From);
		}
	}
	return Sum;
}

bool FTrafficOccupancy::IsHeld(const FTrafficResource& Resource, int32 ExcludingAgent, int32* OutHolder) const
{
	const TArray<int32>* Bucket = ByResource.Find(Resource);
	if (Bucket == nullptr)
	{
		return false;
	}
	for (int32 Index : *Bucket)
	{
		const FTrafficClaim& C = Claims[Index];
		if (C.AgentId != ExcludingAgent)
		{
			if (OutHolder != nullptr) { *OutHolder = C.AgentId; }
			return true;
		}
	}
	return false;
}

bool FTrafficOccupancy::IsAnyHeld(TConstArrayView<FTrafficResource> Resources, int32 Excluding, bool bCountOwnOccupied) const
{
	for (const FTrafficResource& Resource : Resources)
	{
		if (IsHeld(Resource, Excluding))
		{
			return true;
		}
		if (bCountOwnOccupied)
		{
			const FTrafficClaim* Own = FindClaim(Excluding, Resource);
			if (Own != nullptr && Own->bOccupied)
			{
				return true;
			}
		}
	}
	return false;
}

TSet<int32> FTrafficOccupancy::TakePreempted()
{
	TSet<int32> Out = MoveTemp(Preempted);
	Preempted.Reset();
	return Out;
}

void FTrafficOccupancy::TakePreempted(TSet<int32>& OutPreempted)
{
	// APPENDED, NOT MOVED (issue #190): a move would hand Preempted's own allocation to the
	// caller and leave this table to rebuild one from scratch the next time a preemption
	// happens - see the header. Reset() below empties Preempted without freeing it, so this
	// table keeps its allocation exactly as OutPreempted keeps its caller's.
	OutPreempted.Append(Preempted);
	Preempted.Reset();
}

void FTrafficOccupancy::ReleaseGuidelineClaims()
{
	// THE KIND IS THE WHOLE TEST, and it is asked of the RESOURCE rather than of the holder:
	// a rebuild frees guideline slots for everybody at once, so this is not one agent giving
	// something back - it is a set of resources ceasing to exist. See the header for why
	// Surface is not one of them.
	ReleaseWhere([](const FTrafficClaim& C)
	{
		return C.Resource.Kind == ETrafficResourceKind::Edge
			|| C.Resource.Kind == ETrafficResourceKind::Node;
	});

	// The preemption list goes with them. It names agents that must re-claim THIS tick
	// because a rival took a reservation; nothing here was taken by a rival, and every agent
	// re-claims on the next pass regardless, so a leftover entry would only buy a redundant
	// second claim pass.
	Preempted.Reset();
}

void FTrafficOccupancy::ReleaseGuidelineClaimsOf(int32 AgentId)
{
	// THE AGENT AND THE KIND, both: the guidelines this one agent will never drive go, and
	// its runway surfaces stay because its BODY has not moved. Nothing is done to the
	// preemption list here, unlike ReleaseGuidelineClaims: this is one agent giving ground
	// back on a graph everybody else is still claiming over, and an agent that lost a
	// reservation to a rival this tick still has to hear about it.
	ReleaseAgentWhere(AgentId, [](const FTrafficClaim& C)
	{
		return C.Resource.Kind == ETrafficResourceKind::Edge
			|| C.Resource.Kind == ETrafficResourceKind::Node;
	});
}

void FTrafficOccupancy::Clear()
{
	Claims.Reset();
	ByResource.Reset();
	ByAgent.Reset();
	Preempted.Reset();
}
