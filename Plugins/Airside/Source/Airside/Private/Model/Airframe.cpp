#include "Model/Airframe.h"

FGearPose FGearPerformance::FractionsAt(double Elapsed, bool bRaising) const
{
	FGearPose Pose;

	if (!IsSet())
	{
		// Nothing authored, so there is no shape to sample. AdvanceGear checks IsSet() before
		// it ever gets here; the resting pose is the right answer anyway. Doors OPEN and
		// truck LEVEL, because those are the bind pose and an unauthored airframe must not
		// have its bones moved - which is what FGearPose's defaults already say, so only the
		// gear needs a word here.
		Pose.GearDownFraction = bRaising ? 0.0 : 1.0;
		return Pose;
	}

	const double Tilt = FMath::Max(TruckTiltSeconds, 0.0);
	const double Doors = FMath::Max(DoorSeconds, 0.0);
	const double Travel = FMath::Max(TravelSeconds, 0.0);

	// CLAMPED, NOT WRAPPED. A cycle that has run past its end holds the pose it arrived in;
	// wrapping would send the gear back down the moment it finished coming up.
	const double At = FMath::Clamp(Elapsed, 0.0, CycleSeconds());

	// EVERY STAGE IS THE SAME SHAPE: a ramp over its own duration, offset by the stages ahead
	// of it, clamped at both ends so it holds its value before it starts and after it
	// finishes. The three differ only in WHERE they sit and which way the ramp runs, and the
	// zero-duration guard on each is what makes "this airframe has no such part" express
	// itself as a fraction that never leaves the bind pose rather than as a division by zero.
	if (bRaising)
	{
		// THE TRUCK GOES FIRST AND NOTHING ELSE MOVES UNTIL IT HAS FINISHED. A bogie beam has
		// to be swung before the leg can carry it into a well that will not take it lying
		// flat, so this is a precondition for the travel and not a decoration on it.
		Pose.TruckLevelFraction = Tilt > 0.0 ? 1.0 - FMath::Clamp(At / Tilt, 0.0, 1.0) : 1.0;

		// THEN THE GEAR, AND THE DOORS SHUT BEHIND IT. They are already open - the gear was
		// down - so nothing has to happen before the leg can move.
		Pose.GearDownFraction = Travel > 0.0
			? 1.0 - FMath::Clamp((At - Tilt) / Travel, 0.0, 1.0)
			: 0.0;

		// Held open across the whole travel, then shut over the last Doors seconds. Holding
		// them open is what stops the wheel passing through a door that seals to 0.0 mm.
		Pose.BayDoorOpenFraction = Doors > 0.0
			? 1.0 - FMath::Clamp((At - Tilt - Travel) / Doors, 0.0, 1.0)
			: 1.0;
		return Pose;
	}

	// LOWERING: THE DOORS LEAD, because they are shut over the stowed wheel and it cannot
	// come down through them. Then the gear travels with them held open, and they STAY open -
	// there is no closing stage at the gear-down end, which is the whole correction of
	// 2026-09-19. See the header.
	Pose.BayDoorOpenFraction = Doors > 0.0 ? FMath::Clamp(At / Doors, 0.0, 1.0) : 1.0;
	Pose.GearDownFraction = Travel > 0.0
		? FMath::Clamp((At - Doors) / Travel, 0.0, 1.0)
		: (At >= Doors ? 1.0 : 0.0);

	// AND THE TRUCK LEVELS LAST, after the leg is down and locked - which is the time-reverse
	// of leading the retraction, and also what a real one does: the beam meets the tarmac at
	// the end of the extension, not part way through it.
	Pose.TruckLevelFraction = Tilt > 0.0
		? FMath::Clamp((At - Doors - Travel) / Tilt, 0.0, 1.0)
		: 1.0;
	return Pose;
}
