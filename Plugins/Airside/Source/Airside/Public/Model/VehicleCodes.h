#pragma once

#include "CoreMinimal.h"

/**
 * THE VEHICLE KINDS' CODES, TYPED ONCE (#430): FVehicle::TypeCode for each chassis Content builds, and the key a
 * scenario's figures are joined to that chassis by (UAirsideSettings::ResolveVehicle).
 *
 * WHY A HEADER OF ITS OWN: the join had no owner. "FUEL" was typed in AirsideSettings.cpp (the bowser's chassis) and
 * again in AirportOps' OpsDefinition.h (its figures), "UTILITY" in both, and a code typed in two modules that must
 * agree between them is a join that fails silently - a scenario row whose code matched no chassis became a zero-size
 * vehicle that passed every fit gate. IN Model/, NOT Content/, because the scenario that names these codes is AirportOps
 * Model/, which may not include Content/ (Check-Architecture's include-direction rule).
 * ENFORCED BY: Check-Architecture rule 4's 'vehicle kind codes' row (the three literals appear in this file alone)
 *
 * NO LONGER A LOOK-UP LADDER (#308), and still not one: nothing branches on which of these a TypeCode equals - rule 21
 * bans the comparison. Each resolver names its own vehicle with one of them, and ResolveVehicle finds a chassis by the
 * code the vehicle itself carries, so a vehicle's look comes from its own FVehicle::Mesh/Tow[].Mesh as #308 left it.
 * CONSTANTS, NOT AN ENUM: FVehicle::TypeCode is an FName the inspector prints and a save holds, and a code a scenario
 * names with no chassis behind it must still be spellable, to be refused by name.
 */
namespace AirsideVehicleCodes
{
	/** The bowser - ResolveDefaultVehicle's chassis. FUEL rather than VAN: the inspector names the job the player sees. */
	inline constexpr const TCHAR* Fuel = TEXT("FUEL");

	/** utility1 towing fuelTrailer1 - ResolveUtilityTowVehicle's. */
	inline constexpr const TCHAR* UtilityTow = TEXT("UTILITY");

	/** The articulated tanker, truckCab1 + tankTrailer1 - ResolveRigVehicle's. Built; on no scenario's list yet. */
	inline constexpr const TCHAR* Rig = TEXT("RIG");
}
