#include "Model/RoadEntity.h"

ETraversalClass TraversalForRole(EServiceRole Role)
{
	switch (Role)
	{
	case EServiceRole::Aircraft:
		return ETraversalClass::Aircraft;

	// Every one of these is a vehicle that obeys identical movement rules and differs only
	// in the job it arrives to do. That is the whole reason the two enums are separate.
	case EServiceRole::Fuel:
	case EServiceRole::Baggage:
	case EServiceRole::Tug:
	case EServiceRole::GPU:
		return ETraversalClass::GroundVehicle;

	case EServiceRole::Passenger:
	case EServiceRole::Crew:
		return ETraversalClass::Pedestrian;
	}

	// No default above, so adding a role makes the compiler point here rather than letting
	// the new one quietly become a pedestrian.
	return ETraversalClass::GroundVehicle;
}

void FGearPerformance::FractionsAt(double Elapsed, bool bRaising,
	double& OutGearDown, double& OutDoorOpen) const
{
	if (!IsSet())
	{
		// Nothing authored, so there is no shape to sample. AdvanceGear checks IsSet() before
		// it ever gets here; the resting pose is the right answer anyway. Doors OPEN, because
		// open is the bind pose and an unauthored airframe must not have its bones moved.
		OutGearDown = bRaising ? 0.0 : 1.0;
		OutDoorOpen = 1.0;
		return;
	}

	const double Doors = FMath::Max(DoorSeconds, 0.0);
	const double Travel = FMath::Max(TravelSeconds, 0.0);

	// CLAMPED, NOT WRAPPED. A cycle that has run past its end holds the pose it arrived in;
	// wrapping would send the gear back down the moment it finished coming up.
	const double At = FMath::Clamp(Elapsed, 0.0, Doors + Travel);

	if (bRaising)
	{
		// THE GEAR GOES FIRST AND THE DOORS SHUT BEHIND IT. They are already open - the gear
		// was down - so nothing has to happen before the leg can move.
		OutGearDown = Travel > 0.0 ? 1.0 - FMath::Clamp(At / Travel, 0.0, 1.0) : 0.0;

		// Held open across the whole travel, then shut over the last Doors seconds. Holding
		// them open is what stops the wheel passing through a door that seals to 0.0 mm.
		OutDoorOpen = Doors > 0.0
			? 1.0 - FMath::Clamp((At - Travel) / Doors, 0.0, 1.0)
			: 1.0;
		return;
	}

	// LOWERING: THE DOORS LEAD, because they are shut over the stowed wheel and it cannot
	// come down through them. Then the gear travels with them held open, and they STAY open -
	// there is no closing stage at the gear-down end, which is the whole correction of
	// 2026-09-19. See the header.
	OutDoorOpen = Doors > 0.0 ? FMath::Clamp(At / Doors, 0.0, 1.0) : 1.0;
	OutGearDown = Travel > 0.0
		? FMath::Clamp((At - Doors) / Travel, 0.0, 1.0)
		: (At >= Doors ? 1.0 : 0.0);
}
