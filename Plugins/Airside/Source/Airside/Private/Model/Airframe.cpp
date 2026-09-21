#include "Model/Airframe.h"

#include "AirsideLog.h"

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

void WarnIfSteerLawUnsupported(const FAirframe& Airframe)
{
	if (!Airframe.NeedsSteerLawWarning())
	{
		return;
	}

	// THE SAME MESSAGE FAirframe::EffectiveSteerLaw USED TO LOG INLINE, moved here by #176 so
	// the pure query on FRouteFollower::Advance's hot path does not pay for AirsideLog.h, and
	// so the Error is emitted once per CALLER of this function - once per dispatch or replan -
	// rather than once per frame.
	UE_LOG(LogAirside, Error,
		TEXT("Airframe '%s' declares RollingSteer with no wheelbase (steer axle %.1f, "
		     "fixed axle %.1f). Falling back to the pivot law - it will turn about "
		     "itself rather than steer."),
		*Airframe.TypeCode.ToString(), Airframe.SteerAxleX, Airframe.FixedAxleX);
}
