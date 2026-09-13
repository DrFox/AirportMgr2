#pragma once

#include "CoreMinimal.h"

/**
 * Slot-map algorithms over plain reflected TArrays.
 * TItem must expose: int32 Generation; bool bAlive;
 */
namespace RoadSlot
{
	template<typename THandle, typename TItem>
	THandle Add(TArray<TItem>& Items, TArray<int32>& FreeList, TItem&& NewItem)
	{
		int32 Index;
		if (FreeList.Num() > 0)
		{
			Index = FreeList.Pop();
			const int32 NextGeneration = Items[Index].Generation;
			Items[Index] = MoveTemp(NewItem);
			Items[Index].Generation = NextGeneration;
		}
		else
		{
			Index = Items.Add(MoveTemp(NewItem));
			Items[Index].Generation = 1;
		}
		Items[Index].bAlive = true;

		THandle Handle;
		Handle.Index = Index;
		Handle.Generation = Items[Index].Generation;
		return Handle;
	}

	/**
	 * The handle addressing a live slot by INDEX.
	 *
	 * Here rather than at each call site because the generation counter is the slot map's,
	 * and three separate places had already rebuilt one by hand - see the comment in
	 * URoadNetwork::RunwayExtentAt, which names doing so as the reason it tracks nodes
	 * rather than segment indices. A handle to a dead slot is not a handle, so this returns
	 * a default (unset) one for anything not alive.
	 */
	template<typename THandle, typename TItem>
	THandle HandleAt(const TArray<TItem>& Items, int32 Index)
	{
		if (!Items.IsValidIndex(Index) || !Items[Index].bAlive)
		{
			return THandle();
		}

		THandle Handle;
		Handle.Index = Index;
		Handle.Generation = Items[Index].Generation;
		return Handle;
	}

	template<typename THandle, typename TItem>
	bool IsValid(const TArray<TItem>& Items, THandle Handle)
	{
		return Handle.Index != INDEX_NONE
			&& Items.IsValidIndex(Handle.Index)
			&& Items[Handle.Index].bAlive
			&& Items[Handle.Index].Generation == Handle.Generation;
	}

	template<typename THandle, typename TItem>
	TItem* Get(TArray<TItem>& Items, THandle Handle)
	{
		return IsValid<THandle, TItem>(Items, Handle) ? &Items[Handle.Index] : nullptr;
	}

	template<typename THandle, typename TItem>
	const TItem* Get(const TArray<TItem>& Items, THandle Handle)
	{
		return IsValid<THandle, TItem>(Items, Handle) ? &Items[Handle.Index] : nullptr;
	}

	template<typename THandle, typename TItem>
	bool Remove(TArray<TItem>& Items, TArray<int32>& FreeList, THandle Handle)
	{
		if (!IsValid<THandle, TItem>(Items, Handle))
		{
			return false;
		}
		Items[Handle.Index].bAlive = false;
		++Items[Handle.Index].Generation;
		FreeList.Push(Handle.Index);
		return true;
	}

	/**
	 * Index of the nearest ALIVE item to At within Radius, or INDEX_NONE. PositionOf projects
	 * an item to the FVector2D its distance is measured from - the three call sites this
	 * replaces (URoadEditFacade::FindNodeNear/FindEntityAt, FGuidelineDrawTool's node pick)
	 * differed only in that projection and the item type (#103). Compared squared, so a
	 * caller passing a large radius costs no square roots.
	 */
	template<typename TItem, typename TPositionOf>
	int32 NearestAlive(TConstArrayView<TItem> Items, const FVector2D& At, double Radius, TPositionOf&& PositionOf)
	{
		double BestSquared = Radius * Radius;
		int32 Best = INDEX_NONE;
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			if (!Items[Index].bAlive)
			{
				continue;
			}

			const double DistanceSquared = FVector2D::DistSquared(PositionOf(Items[Index]), At);
			if (DistanceSquared <= BestSquared)
			{
				BestSquared = DistanceSquared;
				Best = Index;
			}
		}
		return Best;
	}
}
