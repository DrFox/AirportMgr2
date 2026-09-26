#pragma once

#include "CoreMinimal.h"
#include "Model/Chassis.h"
#include "Model/Vehicle.h"
#include "UObject/ObjectKey.h"

class URoadProfile;

/**
 * Which vehicle each road profile's JUNCTION FILLETS are designed for.
 *
 * PER WIDTH TIER (user ruling 2026-09-25, spec 2026-09-24 §3 REVISED "per-tier design
 * vehicle"): the Wide service-road tier's corners are laid for the articulated rig, Narrow and
 * Standard for the largest RIGID service vehicle (the bowser), as every road was before.
 *
 * FILLETS ONLY, NOT DEAD-END BALLOONS - BY RULING (the same day, "smaller, reverse later"): a
 * balloon a rig can be driven round without folding its trailer reaches ~24 m past the road end,
 * and the rig turns at a road end with a hammerhead - a reverse turn into a stub (FReverseTurn,
 * ruled 2026-09-26) - rather than a three-point turn in the balloon, as first expected.
 * So FRoadGuidelineBuilder sizes every balloon from Default, and the rig is refused at every
 * balloon (on its trailer folding, since the whole-route check).
 *
 * WHICH TIER IS WHICH IS DECIDED IN ONE PLACE, UAirsideSettings::ResolveTierDesignVehicles.
 * This struct only carries the answer, resolved once per rebuild and handed down (issue #190),
 * the way the single FChassis it replaces was.
 */
struct AIRSIDE_API FRoadDesignVehicles
{
	/**
	 * Every profile not named in PerProfile - taxiways and runways included, which author their own fillets.
	 *
	 * A WHOLE VEHICLE, NOT A CHASSIS, since 2026-09-25 (bend widening): a bend's inside is widened
	 * to what its design vehicle's BODY sweeps (FRoadNetworkSolver, BendWidening), and a chassis
	 * has no trailer to trace. For() still answers with the chassis, which is all a fillet needs.
	 */
	FVehicle Default;

	/**
	 * The profiles designed for something other than Default. TObjectKey, not a raw pointer: a
	 * key compared after its profile was collected must not match a new object at the same
	 * address.
	 */
	TMap<TObjectKey<URoadProfile>, FVehicle> PerProfile;

	FRoadDesignVehicles() = default;

	/**
	 * UNIFORM: every profile designed for one chassis. EXPLICIT (review, 2026-09-25): when it was
	 * implicit, every test that handed the builder ResolveLargestServiceVehicle() quietly laid its
	 * Wide roads for the bowser while production laid them for the rig. A caller that means one
	 * vehicle for every tier now has to say so; one that means production passes
	 * UAirsideSettings::ResolveRoadDesignVehicles().
	 */
	explicit FRoadDesignVehicles(const FChassis& Uniform) { Default.Chassis = Uniform; }

	/** UNIFORM with a body: every profile designed for one vehicle, whose sweep widens bends too. */
	explicit FRoadDesignVehicles(const FVehicle& Uniform) : Default(Uniform) {}

	/**
	 * The chassis Profile's fillets are sized for. Null (no profile) is Default. Out of line:
	 * the key needs URoadProfile complete, and this header only forward-declares it.
	 */
	const FChassis& For(const URoadProfile* Profile) const;

	/**
	 * The whole vehicle behind For(): what a bend's inside is widened for. A Default built from
	 * a bare chassis has no body (BodyWidth 0), and a bodiless vehicle widens nothing.
	 */
	const FVehicle& VehicleFor(const URoadProfile* Profile) const;
};
