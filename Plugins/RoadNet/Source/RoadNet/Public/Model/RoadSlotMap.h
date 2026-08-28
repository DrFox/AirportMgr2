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
}
