#include "Model/Chassis.h"

#include "AirsideLog.h"

void WarnIfSteerLawUnsupported(const FChassis& Chassis)
{
	if (!Chassis.NeedsSteerLawWarning())
	{
		return;
	}

	// THE SAME MESSAGE FAirframe::EffectiveSteerLaw (now FChassis's) USED TO LOG INLINE,
	// moved here by #176 so the pure query on FRouteFollower::Advance's hot path does not pay for AirsideLog.h, and
	// so the Error is emitted once per CALLER of this function - once per dispatch or replan -
	// rather than once per frame.
	UE_LOG(LogAirside, Error,
		TEXT("Chassis declares RollingSteer with no wheelbase (steer axle %.1f, "
		     "fixed axle %.1f). Falling back to the pivot law - it will turn about "
		     "itself rather than steer."),
		Chassis.SteerAxleX, Chassis.FixedAxleX);
}
