#pragma once

#include "CoreMinimal.h"

/** Flat triangle soup in world space. Slice 2b adds UV channels here. */
struct FRoadMeshBuffers
{
	TArray<FVector3d> Positions;
	TArray<int32>     Indices;

	/**
	 * World-aligned XY, divided by the texel scale. A pure function of Positions, which
	 * is exactly why design spec 6.3 chose it: asphalt is continuous across a
	 * segment/junction boundary by construction, because neither side can disagree about
	 * a value that depends only on where the vertex is.
	 */
	TArray<FVector2f> UV0;

	/** X = lateral offset across the profile in uu, Y = distance along the centreline in uu. */
	TArray<FVector2f> UV1;

	/** A = ground blend (unused until 2b-ii), G = junction blend, R and B reserved. */
	TArray<FColor> Colors;
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
