#pragma once

#include "CoreMinimal.h"
#include "Model/Chassis.h"
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
 * and the rig will turn at a road end with a three-point turn once reversing exists (step 2).
 * So FRoadGuidelineBuilder sizes every balloon from Default, and the rig is refused at every
 * dead end on its lock until then.
 *
 * WHICH TIER IS WHICH IS DECIDED IN ONE PLACE, UAirsideSettings::ResolveTierDesignVehicles.
 * This struct only carries the answer, resolved once per rebuild and handed down (issue #190),
 * the way the single FChassis it replaces was.
 */
struct AIRSIDE_API FRoadDesignVehicles
{
	/** Every profile not named in PerProfile - taxiways and runways included, which author their own fillets. */
	FChassis Default;

	/**
	 * The profiles designed for something other than Default. TObjectKey, not a raw pointer: a
	 * key compared after its profile was collected must not match a new object at the same
	 * address.
	 */
	TMap<TObjectKey<URoadProfile>, FChassis> PerProfile;

	FRoadDesignVehicles() = default;

	/**
	 * UNIFORM: every profile designed for one chassis. EXPLICIT (review, 2026-09-25): when it was
	 * implicit, every test that handed the builder ResolveLargestServiceVehicle() quietly laid its
	 * Wide roads for the bowser while production laid them for the rig. A caller that means one
	 * vehicle for every tier now has to say so; one that means production passes
	 * UAirsideSettings::ResolveRoadDesignVehicles().
	 */
	explicit FRoadDesignVehicles(const FChassis& Uniform) : Default(Uniform) {}

	/**
	 * The chassis Profile's fillets are sized for. Null (no profile) is Default. Out of line:
	 * the key needs URoadProfile complete, and this header only forward-declares it.
	 */
	const FChassis& For(const URoadProfile* Profile) const;
};
