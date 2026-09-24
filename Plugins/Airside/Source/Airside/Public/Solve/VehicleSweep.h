#pragma once

#include "CoreMinimal.h"

/**
 * How much road a vehicle sweeps holding a turn (spec 2026-09-23 §6). Dependency-free, like
 * the rest of Solve/: Model/VehicleFit maps an FVehicle onto FBody.
 *
 * STEADY STATE, which is CONSERVATIVE: a 90 degree turn is over before the trailer settles to
 * the off-tracking computed here, so the real sweep is smaller. Chosen because it is a closed
 * form with no dependence on the path before the corner, and because the cost of being wrong
 * in this direction is a corner sized a little generously rather than a trailer over a kerb.
 *
 * THE STEERED AXLE RIDES THE LINE, as it does in FRouteFollower, so Radius is the steered
 * axle's; the fixed axle runs at sqrt(R^2 - L^2) and every body corner is placed from there.
 * ENFORCED BY: Airside.Model.FollowerMatchesSweep (follower and Trace agree on the cut-in)
 */
namespace VehicleSweep
{
	/** A body in the fixed axle's frame, uu. KingpinToAxle 0 means no trailer. */
	struct FBody
	{
		double Wheelbase = 0.0;
		double Width = 0.0;
		double FrontX = 0.0;
		double RearX = 0.0;
		double KingpinX = 0.0;
		double KingpinToAxle = 0.0;
		double TrailerFront = 0.0;
		double TrailerRear = 0.0;
		double TrailerWidth = 0.0;
	};

	/**
	 * Offsets from the steered axle's path, uu: Inner toward the turn centre, Outer away from
	 * it. bHolds false when the turn cannot be held at all - tighter than the wheelbase, or a
	 * kingpin circle smaller than the trailer (it would jack-knife) - and then the offsets are
	 * meaningless.
	 */
	struct FEnvelope
	{
		double Inner = 0.0;
		double Outer = 0.0;
		bool bHolds = false;
	};

	AIRSIDE_API FEnvelope Envelope(const FBody& Body, double SteerRadius);

	/**
	 * THE TURN AS DRIVEN, not the steady state (review of 2026-09-24). The steered axle walks
	 * Path - lead in straight along its first tangent, out straight along its last, far enough
	 * for the whole vehicle - the fixed axle pursues it at the wheelbase, the kingpin rides the
	 * tractor, and the trailer axle pursues the kingpin at KingpinToAxle. At every step each
	 * body corner (and each axle end) is projected onto Path; its reach toward the turn's
	 * centre and away from it is kept against the NEAREST SAMPLE, so OutInner[i]/OutOuter[i]
	 * say how far the body reached either side of Path[i]. Points that project beyond either
	 * end are in the straight lanes and are not recorded - lane Width gates those.
	 *
	 * WHY: a 90 degree corner ends before the trailer settles, so it cuts in less than
	 * Envelope says - the steady-state model made the first Wide corner 30 m, where real rigs
	 * turn in yards. It also cannot say WHERE the cut-in happens, which Trace does.
	 *
	 * False when the trailer folds past square to the tractor (a jack-knife): not drivable.
	 * Measured on the SAME samples the follower walks, per the sample-once rule.
	 */
	AIRSIDE_API bool Trace(const FBody& Body, TArrayView<const FVector2D> Path,
		TArray<double>& OutInner, TArray<double>& OutOuter);
}
