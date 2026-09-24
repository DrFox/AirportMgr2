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
 * THE STEERED AXLE RIDES THE LINE (FRouteFollower), so Radius is the steered axle's; the
 * fixed axle runs at sqrt(R^2 - L^2) and every body corner is placed from there.
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
}
