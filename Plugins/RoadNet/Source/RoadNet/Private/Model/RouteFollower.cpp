#include "Model/RouteFollower.h"

#include "Solve/GuidelineGeom.h"

void FRouteFollower::Start(const FRoutePlan& InPlan, const FGroundPerformance& InGround)
{
	Plan = InPlan;
	Travelled = 0.0;
	Ground = InGround;

	// FROM REST. An aeroplane on a stand is stopped, and snapping to taxi speed on the first
	// frame is the same defect as the corner this class was just taught about - an
	// acceleration no airframe has - only at the one moment the player is certain to be
	// looking, because they just dispatched it.
	Speed = 0.0;

	// The whole route costed before the first frame. See FSpeedProfile: once braking is
	// limited, a corner discovered by arriving at it is already twenty-five metres too late.
	Profile.Build(Plan.Polyline, Ground);

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
	const double MaxStep = FMath::DegreesToRadians(Ground.MaxTurnRateDegPerSec) * DeltaSeconds;
	const double Step = FMath::Clamp(Error, -MaxStep, MaxStep);
	Heading = FMath::UnwindRadians(Heading + Step);

	// WHAT COULD NOT BE TAKEN OUT THIS FRAME, which is the crab the player is now looking
	// at. Measured after the slew rather than before it: an airframe that CAN make the turn
	// has no reason to slow for it, and taking the pre-slew error instead would have shaved
	// a few percent off the speed of every agent on every gentle bend for nothing.
	const double Crab = FMath::RadiansToDegrees(FMath::Abs(Error - Step));

	// TWO THINGS DECIDE THE TARGET SPEED, and they have different jobs.
	//
	// The PROFILE is the plan: it knows what is coming and is the only reason the aircraft
	// is ever slow BEFORE a corner rather than after it. It is also the only one that can
	// bring the aircraft to a stop, at the destination.
	//
	// The CRAB TERM is the feedback: it reacts to the line the aircraft is actually being
	// given. It should almost never bind - if the profile has done its job the crab stays
	// near zero on anything but a genuine corner - but it is what makes "the nose stays
	// within CrabAtMinSpeedDegrees of the line" a property of this loop rather than a
	// prediction that happens to come true. A plan alone would have nothing to notice with.
	//
	// It floors at MinTaxiSpeed and the profile does not, which is what lets the aircraft
	// creep through a turn but still stop when it has arrived.
	const double Slowing = 1.0 - FMath::Clamp(Crab / CrabAtMinSpeedDegrees, 0.0, 1.0);
	const double CrabLimit = FMath::Max(Ground.MinTaxiSpeed, Ground.Taxi.SpeedCap * Slowing);

	const double Target = FMath::Min(Profile.LimitAt(Travelled), CrabLimit);

	// AND THE TARGET IS APPROACHED, NOT TAKEN. Speed used to be assigned outright, so
	// meeting a corner cost 920 uu/s in a single frame - 552 m/s2, fifty-six g. Thrust and
	// brakes are separate figures because they are not equal: wheel brakes beat a propeller.
	Speed = Target > Speed
		? FMath::Min(Target, Speed + Ground.Taxi.Accel * DeltaSeconds)
		: FMath::Max(Target, Speed - Ground.Taxi.Decel * DeltaSeconds);

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
