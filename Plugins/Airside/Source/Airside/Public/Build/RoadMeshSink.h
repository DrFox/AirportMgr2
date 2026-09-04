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

	/**
	 * Masks, NOT colour. X = junction blend. Y is reserved and always 1.
	 *
	 * Y briefly carried a ground blend driving a shoulder fade into the terrain. That was
	 * removed: an airport's surfaces meet at hard material lines - concrete slab, asphalt
	 * run-off, grass - and a road's edge is a kerb, so the fade solved a problem this game
	 * does not have while costing a masked material to do it. Edge treatment is a per-band
	 * material choice instead.
	 *
	 * These began life in a vertex-colour overlay and must not go back there. A
	 * UDynamicMeshComponent only ignores its colour overlay while ColorOverrideMode is
	 * Constant; assigning any material flips it to None, at which point the converter
	 * reads the overlay and the whole surface stops rendering - with any material, ours
	 * or a stock one. Beyond that specific fault, vertex colour multiplies through in
	 * anything that samples it, so a mask stored there tints the surface as a side
	 * effect. A UV channel carries the same two floats with neither coupling.
	 */
	TArray<FVector2f> UV2;

	/**
	 * Material id per TRIANGLE, so exactly Indices.Num() / 3 entries.
	 *
	 * Per triangle, not per vertex, and that is the whole reason this slice does not touch
	 * slice 2a's contract. A vertex on the boundary between two bands of different
	 * materials is shared by triangles of both; because the id lives on the face, that
	 * vertex stays ONE welded vertex and nothing is ever tempted to split it in order to
	 * carry a material. Material is a per-face property; the weld is a per-vertex one.
	 */
	TArray<int32> MaterialIDs;
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
