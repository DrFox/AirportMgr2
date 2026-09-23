#pragma once

#include "CoreMinimal.h"

/**
 * The shape of a dead end's U-turn (spec 2026-09-23 §4). Dependency-free, like the rest of
 * Solve/: the guideline builder turns these pieces into derived edges.
 *
 * A BALLOON, NOT ONE CURVE. A quadratic cannot turn 180 degrees - its end tangents would be
 * parallel and its control point at infinity - and a loop the width of the lane spacing
 * (3 m) is far inside any vehicle's steering lock. So: a reverse curve swinging out to one
 * side, a half circle of radius R centred on the road's axis, and the mirror reverse curve
 * back. Six quadratics, tangent-continuous at every joint.
 */
namespace UTurnGeom
{
	/** One quadratic of the balloon, in travel order: from the previous End (or InEnd) to End. */
	struct FPiece
	{
		FVector2D End = FVector2D::ZeroVector;
		FVector2D Control = FVector2D::ZeroVector;
	};

	/**
	 * How far past the lane ends the circle's centre sits, as a multiple of R. Measured with
	 * a Python prototype on 2026-09-23 for a Narrow road and a 699 uu lock: 1.0 needs R at
	 * 3.8x the lock (the reverse curves are too steep), 2.0 reaches 4.4x the lock past the
	 * road end, 1.5 gives R = 1.56x and a reach of 3.9x - the shortest balloon that clears.
	 */
	constexpr double HeightFactor = 1.5;

	/**
	 * Six quadratics from InEnd (travelling +Axis) to OutEnd (travelling -Axis). R starts at
	 * NeededRadius and grows 3% a step until every piece's GuidelineGeom::TightestRadius is
	 * at least NeededRadius (at most 80 steps; the best found is returned if none clears).
	 * Empty when InEnd == OutEnd or Axis is zero. OutRadius, if given, receives R.
	 */
	AIRSIDE_API TArray<FPiece> Balloon(const FVector2D& InEnd, const FVector2D& OutEnd,
		const FVector2D& Axis, double NeededRadius, double* OutRadius = nullptr);
}
