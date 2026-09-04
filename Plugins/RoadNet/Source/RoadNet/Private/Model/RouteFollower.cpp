#include "Model/RouteFollower.h"

#include "Solve/GuidelineGeom.h"

void FRouteFollower::Start(const FRoutePlan& InPlan, const FTaxiPerformance& InTaxi)
{
	Plan = InPlan;
	Travelled = 0.0;
	Taxi = InTaxi;
	Speed = InTaxi.TaxiSpeed;

	// Seeded from the line, not left at zero. An agent that starts facing due east and
	// slews to its actual heading pirouettes on the stand the instant it is dispatched -
	// which reads as a routing bug rather than as an uninitialised field.
	FVector2D Unused;
	if (!GuidelineGeom::PointAtDistance(Plan.Polyline, 0.0, Unused, Heading))
	{
		Heading = 0.0;
	}
}

bool FRouteFollower::Advance(double DeltaSeconds, FVector2D& OutPosition, double& OutHeading)
{
	if (!Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		return false;
	}

	// Clamped rather than allowed to run on, so a long frame - a hitch, or a breakpoint -
	// leaves the agent at its destination instead of somewhere past the end of the world.
	//
	// Speed is LAST frame's, decided at the bottom of this function. One frame of lag, 16 ms
	// at the rate this is watched at, and it buys the whole loop a single PointAtDistance
	// call: reading the line, deciding a speed and then moving would need two, one before
	// the move and one after, on every agent on the airport.
	Travelled = FMath::Clamp(Travelled + Speed * DeltaSeconds, 0.0, Plan.Length);

	// Where the LINE points here. Not where the aircraft points - those are now two
	// different things, and that gap is the whole of this function.
	double LineHeading = 0.0;
	if (!GuidelineGeom::PointAtDistance(Plan.Polyline, Travelled, OutPosition, LineHeading))
	{
		return false;
	}

	const double Error = FMath::UnwindRadians(LineHeading - Heading);

	// Unwound first, so a turn across the +/-PI seam is taken the short way round rather
	// than very nearly all the way about.
	const double MaxStep = FMath::DegreesToRadians(Taxi.MaxTurnRateDegPerSec) * DeltaSeconds;
	const double Step = FMath::Clamp(Error, -MaxStep, MaxStep);
	Heading = FMath::UnwindRadians(Heading + Step);

	// WHAT COULD NOT BE TAKEN OUT THIS FRAME, which is the crab the player is now looking
	// at. Measured after the slew rather than before it: an airframe that CAN make the turn
	// has no reason to slow for it, and taking the pre-slew error instead would have shaved
	// a few percent off the speed of every agent on every gentle bend for nothing.
	const double Crab = FMath::RadiansToDegrees(FMath::Abs(Error - Step));

	// SPEED IS WHAT GIVES. The aircraft is on the painted line by construction - position
	// comes from the polyline and nothing here touches it - so when the geometry demands a
	// turn the airframe cannot make, the only remaining freedom is how fast it takes it.
	// Slowing is also what a pilot does, and it settles: the yaw a curve demands is v times
	// curvature, so losing speed lowers the demand until the two meet.
	//
	// The floor is NOT zero, and that is physics rather than taste. A prop or a fan makes
	// thrust along the airframe and the nosewheel only steers where that thrust is taken;
	// there is no way to pivot standing still. At zero this law would park an agent at a
	// sharp corner for ever - a deadlock, and a lie about what an aeroplane can do.
	const double Slowing = 1.0 - FMath::Clamp(Crab / CrabAtMinSpeedDegrees, 0.0, 1.0);
	Speed = FMath::Max(Taxi.MinTaxiSpeed, Taxi.TaxiSpeed * Slowing);

	OutHeading = Heading;
	return true;
}

bool FRouteFollower::HasArrived() const
{
	if (!Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		// An agent that cannot move has, for every purpose the caller has, finished. The
		// alternative is a cube that never despawns because it never got a route.
		return true;
	}

	return Travelled >= Plan.Length;
}
