#pragma once

#include "CoreMinimal.h"

/**
 * The shape of a dead end's U-turn (spec 2026-09-23 §4). Dependency-free, like the rest of
 * Solve/: the guideline builder turns these pieces into derived edges.
 *
 * A BALLOON, NOT ONE CURVE. A quadratic cannot turn 180 degrees - its end tangents would be
 * parallel and its control point at infinity - and a loop the width of the lane spacing
 * (3 m) is far inside any vehicle's steering lock. So: a reverse curve swinging out to one
 * side, a half circle centred on the road's axis, and the mirror reverse curve back, all
 * quadratics, tangent-continuous at every joint.
 *
 * RESHAPED 2026-09-25 WITHIN THE SAME FOOTPRINT (user ruling: balloons stay bowser-sized, but
 * the curves inside that footprint may be as gentle as it allows). The balloon used to be two
 * quadratics per reverse curve and one per QUARTER circle, with R grown until the tightest piece
 * cleared the bowser: a quadratic across a 90 degree arc delivers only cos(45) = 0.707 of the
 * circle's radius at its apex, so the half circle had to be 1.41x the lock to deliver it, and
 * the path turned no gentler than the lock anywhere. Now:
 *
 *   - the FOOTPRINT is the ruled box - how far past the lane ends and how far either side of
 *     the axis today's bowser balloon reached (FootprintFor, which keeps the old construction
 *     only to measure it);
 *   - the half circle is CirclePieces quadratics (45 degrees each, cos(22.5) = 0.924 of R);
 *   - the reverse curves are GuidelineGeom's lane change laid across the height that is left
 *     (GuidelineGeom::LaneChange), the same S a width taper uses;
 *   - and the circle's radius is chosen, inside the box, where the circle and the S deliver
 *     the same radius - the S gets gentler as the circle narrows (less to shift, more height to
 *     shift it in) and the circle tighter, so that crossing IS the gentlest balloon the box holds.
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
	 * How far past the lane ends the OLD balloon's circle centre sat, as a multiple of its R -
	 * part of the footprint's definition now, not of the shape. Measured with a Python prototype
	 * on 2026-09-23 for a Narrow road and a 699 uu lock: 1.0 needs R at 3.8x the lock (the
	 * reverse curves are too steep), 2.0 reaches 4.4x the lock past the road end, 1.5 gives
	 * R = 1.56x and a reach of 3.9x - the shortest balloon that cleared.
	 */
	constexpr double HeightFactor = 1.5;

	/**
	 * Quadratics in the half circle. EVEN, so the circle's top and both its sides are piece
	 * ENDS: a quadratic across an arc bulges just outside the circle at its middle (by 1% of R
	 * at 60 degrees), and an extreme mid-piece would break the footprint. Four: 45 degrees each,
	 * 0.924 of R - measured 2026-09-25 against the bowser footprint on the three tiers, 4 pieces
	 * deliver 642-666 uu; 8 would add ~27 uu for twice the edges.
	 */
	constexpr int32 CirclePieces = 4;

	/**
	 * The ruled box a balloon may occupy, in the balloon's own frame: Reach past the line of the
	 * two lane ends along the axis, HalfWidth either side of the axis.
	 */
	struct FFootprint
	{
		double Reach = 0.0;
		double HalfWidth = 0.0;
	};

	/**
	 * THE RULED FOOTPRINT for lane ends HalfSpacing either side of the axis: the box of the
	 * balloon this namespace used to lay - six quadratics, R grown 3% a step from NeededRadius
	 * until every piece's TightestRadius cleared it - which the ruling of 2026-09-25 froze as the
	 * size of a dead end. Reach = (HeightFactor + 1) R, HalfWidth = R.
	 * ENFORCED BY: Airside.Solve.UTurnBalloonFootprint (the box, measured, per tier)
	 */
	AIRSIDE_API FFootprint FootprintFor(double HalfSpacing, double NeededRadius);

	/**
	 * The balloon from InEnd (travelling +Axis) to OutEnd (travelling -Axis), reshaped for the
	 * gentlest tightest piece inside FootprintFor(its lane spacing, NeededRadius). 4 +
	 * CirclePieces quadratics. Empty when InEnd == OutEnd or Axis is zero. OutRadius, if given,
	 * receives the circle's radius.
	 */
	AIRSIDE_API TArray<FPiece> Balloon(const FVector2D& InEnd, const FVector2D& OutEnd,
		const FVector2D& Axis, double NeededRadius, double* OutRadius = nullptr);

	/** What a balloon's pieces measure, on GuidelineGeom's samples - the line that is driven. */
	struct FMeasured
	{
		/** Furthest sample past the lane ends' line, along the axis. */
		double Reach = 0.0;
		/** Furthest sample from the axis, either side. */
		double HalfWidth = 0.0;
		/** GuidelineGeom::TightestRadius of the tightest piece. */
		double Tightest = 0.0;
	};

	/** Measures Pieces laid from InEnd, in the frame InEnd, OutEnd and Axis define. */
	AIRSIDE_API FMeasured Measure(const TArray<FPiece>& Pieces, const FVector2D& InEnd, const FVector2D& OutEnd,
		const FVector2D& Axis);
}
