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

		// Occupied is absolute. Equal rank keeps the holder - that IS first-to-reserve.
		if (Claims[Index].bOccupied || Claims[Index].Rank >= Claim.Rank)
		{
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

void FTrafficOccupancy::ReleaseAll(int32 AgentId)
{
	Claims.RemoveAllSwap([AgentId](const FTrafficClaim& C) { return C.AgentId == AgentId; });
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

void FTrafficOccupancy::Clear()
{
	Claims.Reset();
	Preempted.Reset();
}
