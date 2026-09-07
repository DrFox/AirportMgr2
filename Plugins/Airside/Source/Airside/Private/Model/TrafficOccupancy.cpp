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

EClaimResult FTrafficOccupancy::TryClaim(const FTrafficClaim& Claim, FTrafficClaim& OutBlocker)
{
	// A same-agent claim on the same resource is an UPDATE: checked against everyone else
	// like a fresh claim, then written over the old one in place. Unconditional replacement
	// was rejected: a follower whose window grows each tick would re-claim its own edge and
	// be granted straight through the leader's occupied interval - the pass-through defect
	// this table exists to end.

	// Find existing claim by same agent on same resource.
	int32 ExistingIndex = INDEX_NONE;
	for (int32 i = 0; i < Claims.Num(); ++i)
	{
		if (Claims[i].AgentId == Claim.AgentId && Claims[i].Resource == Claim.Resource)
		{
			ExistingIndex = i;
			break;
		}
	}

	// Check for conflicts against OTHER agents' claims (skip any by Claim.AgentId).
	TArray<int32> ToPreempt;
	for (int32 Index = 0; Index < Claims.Num(); ++Index)
	{
		// Skip claims by the same agent (including the existing claim we'll update).
		if (Claims[Index].AgentId == Claim.AgentId)
		{
			continue;
		}

		if (!Claims[Index].Conflicts(Claim))
		{
			continue;
		}

		// OCCUPIED IS ABSOLUTE, from both sides.
		//
		// Nobody is evicted from ground they are standing on, so an existing occupancy
		// refuses everything - including another occupancy, which is two bodies in one place
		// and the caller's problem to report.
		if (Claims[Index].bOccupied)
		{
			OutBlocker = Claims[Index];
			return EClaimResult::Held;
		}

		// And PRESENCE BEATS A RESERVATION, whatever the ranks say: spec §3.3 promises the
		// occupant will never be moved, so a reservation on ground somebody is already
		// standing on was never a claim anyone could act on. Leaving it in the table cost a
		// deadlock its cycle - the occupant was refused its OWN ground, abandoned the rest of
		// its claim pass, and the wait-for graph became a fan into the reserver instead of a
		// ring. See Airside.Model.Traffic.BoxEntry.
		if (!Claim.bOccupied && Claims[Index].Rank >= Claim.Rank)
		{
			// Equal rank keeps the holder - that IS first-to-reserve.
			OutBlocker = Claims[Index];
			return EClaimResult::Held;
		}
		ToPreempt.Add(Index);
	}

	// Update or add the claim BEFORE removing preempted claims, so indices stay valid.
	// The agent's own claim is never in ToPreempt (we skip AgentId), so it won't move.
	if (ExistingIndex != INDEX_NONE)
	{
		Claims[ExistingIndex] = Claim;
	}
	else
	{
		Claims.Add(Claim);
	}

	// Now preempt lower-ranked reservations.
	for (int32 At = ToPreempt.Num() - 1; At >= 0; --At)
	{
		Preempted.Add(Claims[ToPreempt[At]].AgentId);
		Claims.RemoveAtSwap(ToPreempt[At]);
	}

	return EClaimResult::Granted;
}

const FTrafficClaim* FTrafficOccupancy::FindClaim(int32 AgentId, const FTrafficResource& Resource) const
{
	for (const FTrafficClaim& Claim : Claims)
	{
		if (Claim.AgentId == AgentId && Claim.Resource == Resource)
		{
			return &Claim;
		}
	}
	return nullptr;
}

void FTrafficOccupancy::ReleaseAll(int32 AgentId)
{
	Claims.RemoveAllSwap([AgentId](const FTrafficClaim& C) { return C.AgentId == AgentId; });
}

void FTrafficOccupancy::Release(int32 AgentId, const FTrafficResource& Resource)
{
	Claims.RemoveAllSwap([AgentId, &Resource](const FTrafficClaim& C)
	{
		return C.AgentId == AgentId && C.Resource == Resource;
	});
}

void FTrafficOccupancy::ReleaseReservations(int32 AgentId)
{
	// bOccupied IS THE WHOLE TEST, and it is the same one TryClaim arbitrates on: a claim
	// that contains the agent's own position is where its body is, and nothing a caller does
	// to its PLAN can move a body. See the header for the landing this cost.
	Claims.RemoveAllSwap([AgentId](const FTrafficClaim& C) { return C.AgentId == AgentId && !C.bOccupied; });
}

void FTrafficOccupancy::ReleaseExcept(int32 AgentId, const TArray<FTrafficResource>& Keep)
{
	Claims.RemoveAllSwap([AgentId, &Keep](const FTrafficClaim& C)
	{
		return C.AgentId == AgentId && !Keep.Contains(C.Resource);
	});
}

double FTrafficOccupancy::HeldLengthOn(FGuidelineEdgeId Edge, int32 ExcludingAgent) const
{
	double Sum = 0.0;
	for (const FTrafficClaim& C : Claims)
	{
		if (C.AgentId != ExcludingAgent && C.Resource.Kind == ETrafficResourceKind::Edge && C.Resource.Edge == Edge)
		{
			Sum += FMath::Max(0.0, C.To - C.From);
		}
	}
	return Sum;
}

bool FTrafficOccupancy::IsHeld(const FTrafficResource& Resource, int32 ExcludingAgent, int32* OutHolder) const
{
	for (const FTrafficClaim& C : Claims)
	{
		if (C.AgentId != ExcludingAgent && C.Resource == Resource)
		{
			if (OutHolder != nullptr) { *OutHolder = C.AgentId; }
			return true;
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

void FTrafficOccupancy::ReleaseGuidelineClaims()
{
	// THE KIND IS THE WHOLE TEST, and it is asked of the RESOURCE rather than of the holder:
	// a rebuild frees guideline slots for everybody at once, so this is not one agent giving
	// something back - it is a set of resources ceasing to exist. See the header for why
	// Surface is not one of them.
	Claims.RemoveAllSwap([](const FTrafficClaim& C)
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
	Claims.RemoveAllSwap([AgentId](const FTrafficClaim& C)
	{
		return C.AgentId == AgentId
			&& (C.Resource.Kind == ETrafficResourceKind::Edge
				|| C.Resource.Kind == ETrafficResourceKind::Node);
	});
}

void FTrafficOccupancy::Clear()
{
	Claims.Reset();
	Preempted.Reset();
}
