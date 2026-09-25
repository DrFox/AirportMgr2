#pragma once

#include "CoreMinimal.h"
#include "Model/Chassis.h"
#include "Model/Vehicle.h"
#include "UObject/ObjectKey.h"

class URoadProfile;

/**
 * Which vehicle each road profile's TURNING geometry - its junction fillets and its dead-end
 * U-turn balloon - is designed for.
 *
 * PER WIDTH TIER (user ruling 2026-09-25, spec 2026-09-24 §3 REVISED "per-tier design
 * vehicle"): the Wide service-road tier is designed for the articulated rig, Narrow and
 * Standard for the largest RIGID service vehicle (the bowser), as every road was before. One
 * chassis for every road forced a choice between oversizing every dead end for the rig and
 * never letting the rig turn at one; a tier is how the player already chooses how much road to
 * lay, so it is where the design vehicle is chosen too.
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
	 * The profiles designed for something other than Default, as a whole VEHICLE: a tow's dead
	 * end is sized by its chain, not only its cab (VehicleFit::BalloonRadiusFor). TObjectKey, not
	 * a raw pointer: a key compared after its profile was collected must not match a new object
	 * at the same address.
	 */
	TMap<TObjectKey<URoadProfile>, FVehicle> PerProfile;

	FRoadDesignVehicles() = default;

	/**
	 * UNIFORM: every profile designed for Chassis. IMPLICIT on purpose - it is what a caller that
	 * names ONE vehicle has always meant (every test that hands the builder
	 * UAirsideSettings::ResolveLargestServiceVehicle()), and those keep meaning it. Production
	 * rebuilds pass UAirsideSettings::ResolveRoadDesignVehicles() instead.
	 */
	FRoadDesignVehicles(const FChassis& Uniform) : Default(Uniform) {}

	/** The chassis Profile's turns are sized for. Null (no profile) is Default. */
	const FChassis& For(const URoadProfile* Profile) const
	{
		const FVehicle* Found = VehicleFor(Profile);
		return Found != nullptr ? Found->Chassis : Default;
	}

	/** The whole design vehicle when Profile names one of its own, else null (Default is a chassis only). */
	const FVehicle* VehicleFor(const URoadProfile* Profile) const
	{
		return Profile != nullptr ? PerProfile.Find(TObjectKey<URoadProfile>(Profile)) : nullptr;
	}
};
