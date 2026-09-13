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

	/**
	 * Below this, in uu², a triangle cannot cover a pixel at any sane texel density.
	 *
	 * A square 0.001 uu on a side. Absolute rather than texel-derived: spec section 12 (K3)
	 * assigns the drop to the mesh builder precisely because the solver has no texel scale
	 * to judge "too small" against, and neither does this - what it has is the knowledge
	 * that nothing this small is ever rasterised, whatever the scale.
	 */
	static constexpr double MinSliverArea = 1e-6;

	/**
	 * One triangle from three ALREADY-APPENDED vertex indices A, B, C - guards degenerate
	 * and sliver triangles, then emits with Unreal's winding. Shared by every builder that
	 * emits a triangle (FRoadMeshBuilder::AddTriangle, MarkingQuads::AddQuad), so the flip
	 * and the guards live in one place rather than being re-derived per builder (#103).
	 *
	 * Emitted B and C SWAPPED, because Unreal's front face is the opposite winding to the
	 * mathematical convention every caller here uses.
	 *
	 * Callers build counter-clockwise as seen from +Z, which is correct maths and is what
	 * the solver's polygons are. Unreal is left-handed: VectorUtil::Normal computes
	 * cross(C-A, B-A) - the negation of the standard cross product, with a comment in the
	 * engine saying exactly why - so a counter-clockwise triangle faces DOWN and is
	 * backface-culled from above.
	 *
	 * This went unnoticed from slice 2a until the first genuinely lit material, because
	 * the placeholder colour override substitutes Unreal's vertex-colour debug material,
	 * which is two-sided. Every winding check - the tests, the hand-derivations, the
	 * review - measured the maths convention and agreed with each other while disagreeing
	 * with the rasteriser.
	 */
	void AppendTriangleUp(int32 A, int32 B, int32 C, int32 MaterialID)
	{
		// A degenerate triangle contributes nothing and upsets downstream normal
		// computation, so drop it rather than emit it.
		if (A == B || B == C || A == C)
		{
			return;
		}

		// Zero-area slivers. A pass-through node a hair off collinear emits a fan whose corner
		// has collapsed: the triangles are correctly wound and have distinct indices, so every
		// check above passes them, but they carry ~2.6e-07 uu² of area into FDynamicMesh3 and
		// its normal computation. Spec section 12 (K3) assigns this to the mesh builder rather
		// than the solver. Note an exactly collinear node never reaches here - the solver finds
		// no apex that sees its rim and declines to emit a fan at all.
		{
			const FVector3d& PA = Positions[A];
			const FVector3d& PB = Positions[B];
			const FVector3d& PC = Positions[C];
			const double Area = FMath::Abs(
				0.5 * ((PB.X - PA.X) * (PC.Y - PA.Y) - (PB.Y - PA.Y) * (PC.X - PA.X)));
			if (Area < MinSliverArea)
			{
				return;
			}
		}

		Indices.Add(A);
		Indices.Add(C);
		Indices.Add(B);

		// Added HERE, after every early return above, so the array stays exactly one entry per
		// emitted triangle. Pushing it at the top would leave an id for each degenerate and
		// sliver triangle this function drops, and MaterialIDs would silently run one ahead of
		// Indices for the rest of the mesh.
		MaterialIDs.Add(MaterialID);
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
