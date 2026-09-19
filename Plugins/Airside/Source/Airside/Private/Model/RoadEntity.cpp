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
		// it ever gets here, but the resting pose is the right answer anyway: a gear stuck
		// half-retracted is a worse failure than one that never moves.
		OutGearDown = bRaising ? 0.0 : 1.0;
		OutDoorOpen = 0.0;
		return;
	}

	const double Doors = FMath::Max(DoorSeconds, 0.0);
	const double Travel = FMath::Max(TravelSeconds, 0.0);

	// CLAMPED, NOT WRAPPED. A cycle that has run past its end holds the pose it arrived in;
	// wrapping would send the gear back down the moment it finished coming up.
	const double At = FMath::Clamp(Elapsed, 0.0, Doors + Travel + Doors);

	// STAGE 2 IS THE ONE THAT MOVES THE GEAR, bracketed by the two door stages. THAT interval
	// is the whole sequencing decision - expressed as a pair of bounds rather than as four
	// extra enum states. Doors = 0 collapses both brackets to nothing and the gear simply
	// travels, which is what an airframe with no bay doors wants.
	const double GearStart = Doors;
	const double GearEnd = Doors + Travel;

	const double Progress = Travel > 0.0
		? FMath::Clamp((At - GearStart) / Travel, 0.0, 1.0)
		: (At >= GearStart ? 1.0 : 0.0);

	// RAISING COUNTS DOWN FROM 1, LOWERING COUNTS UP TO IT. One timeline and one direction
	// flag, which is what makes the extend half nearly free - and free is the argument that
	// justified building a half nothing can currently see.
	OutGearDown = bRaising ? 1.0 - Progress : Progress;

	if (Doors <= 0.0)
	{
		// NO DOORS, and this branch is also what keeps the divisions below away from zero.
		OutDoorOpen = 0.0;
		return;
	}

	// THE TRAPEZOID: open across the first Doors seconds, HELD open for the whole of the
	// gear's travel, shut across the last Doors seconds. Holding it open for the travel is
	// what stops the wheel passing through a door that seals to 0.0 mm on plane4.
	if (At < GearStart)
	{
		OutDoorOpen = FMath::Clamp(At / Doors, 0.0, 1.0);
	}
	else if (At < GearEnd)
	{
		OutDoorOpen = 1.0;
	}
	else
	{
		OutDoorOpen = FMath::Clamp(1.0 - (At - GearEnd) / Doors, 0.0, 1.0);
	}
}
