#pragma once

#include "CoreMinimal.h"
#include "RoadHandles.generated.h"

/** Generation-checked handle to a node in URoadNetwork. */
USTRUCT()
struct ROADNET_API FRoadNodeId
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() int32 Generation = 0;

	/**
	 * Reports only that this handle was ever assigned - NOT that its referent is still
	 * alive. A stale handle to a recycled slot still returns true here, because this
	 * deliberately ignores Generation.
	 *
	 * For liveness always use RoadSlot::IsValid(Items, Handle), which is the only check
	 * that compares Generation and so the only one the recycling scheme is safe under.
	 */
	bool IsSet() const { return Index != INDEX_NONE; }

	bool operator==(const FRoadNodeId& Other) const
	{
		return Index == Other.Index && Generation == Other.Generation;
	}
	bool operator!=(const FRoadNodeId& Other) const { return !(*this == Other); }
};

FORCEINLINE uint32 GetTypeHash(const FRoadNodeId& Id)
{
	return HashCombine(::GetTypeHash(Id.Index), ::GetTypeHash(Id.Generation));
}

/** Generation-checked handle to a segment in URoadNetwork. */
USTRUCT()
struct ROADNET_API FRoadSegmentId
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() int32 Generation = 0;

	/**
	 * Reports only that this handle was ever assigned - NOT that its referent is still
	 * alive. A stale handle to a recycled slot still returns true here, because this
	 * deliberately ignores Generation.
	 *
	 * For liveness always use RoadSlot::IsValid(Items, Handle), which is the only check
	 * that compares Generation and so the only one the recycling scheme is safe under.
	 */
	bool IsSet() const { return Index != INDEX_NONE; }

	bool operator==(const FRoadSegmentId& Other) const
	{
		return Index == Other.Index && Generation == Other.Generation;
	}
	bool operator!=(const FRoadSegmentId& Other) const { return !(*this == Other); }
};

FORCEINLINE uint32 GetTypeHash(const FRoadSegmentId& Id)
{
	return HashCombine(::GetTypeHash(Id.Index), ::GetTypeHash(Id.Generation));
}
