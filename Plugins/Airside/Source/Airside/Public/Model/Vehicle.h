#pragma once

// A SERVICE VEHICLE'S BUNDLE, split out on 2026-09-23. Until then a fuel truck was an
// FAirframe with its climb and approach zeroed by hand (UAirsideSettings::ResolveDefaultVehicle
// called that scaffolding, "M3 replaces this with a vehicle-shaped performance bundle"), so a
// truck carried a gear-retraction cycle, a propeller spool and a wingspan it had no use for,
// and every consumer had to remember which of them were lies. This is that vehicle-shaped
// bundle: what rolls (FChassis) and what the inspector calls it, nothing that flies.
//
// THE SIBLING OF FAirframe, NOT A BASE OF IT. Both hold an FChassis by composition, and
// FRoadAgent holds one of each and answers Chassis() from whichever it was started with - see
// FRoadAgent::Body. An articulated rig's trailer lands HERE, which is the point of the split:
// on FAirframe it would have handed every A380 a kingpin.
//
// ENFORCED BY: Check-Architecture rule 15 - this header may not name FAirframe.

#include "CoreMinimal.h"
#include "Model/Chassis.h"
#include "Vehicle.generated.h"

/**
 * Every fact about one service vehicle that a dispatch needs, bundled - FAirframe's rule
 * ("one struct, not four parameters") applied to the other kind of thing on the apron.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FVehicle
{
	GENERATED_BODY()

	/** How it rolls. See FChassis. */
	UPROPERTY(EditAnywhere) FChassis Chassis;

	/**
	 * What the inspector SAYS this is - "FUEL". The vehicle counterpart of
	 * FAirframe::TypeCode, and here for the same reason: Model/ may not see Entities/, and the
	 * agent carries no pointer to a type. NAME_None for a vehicle assembled by hand.
	 */
	UPROPERTY(EditAnywhere) FName TypeCode;
};
