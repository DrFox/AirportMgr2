#pragma once

#include "CoreMinimal.h"

/** Flat triangle soup in world space. Slice 2b adds UV channels here. */
struct FRoadMeshBuffers
{
	TArray<FVector3d> Positions;
	TArray<int32>     Indices;

	void Reset()
	{
		Positions.Reset();
		Indices.Reset();
	}
};

/**
 * Where finished geometry goes. Strategy: the builder does not know whether its output
 * becomes a UDynamicMeshComponent, a preview ghost, or a test counter.
 */
struct IRoadMeshSink
{
	virtual ~IRoadMeshSink() = default;
	virtual void Accept(const FRoadMeshBuffers& Buffers) = 0;
};
