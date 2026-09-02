#include "Model/RouteFollower.h"

#include "Solve/GuidelineGeom.h"

void FRouteFollower::Start(const FRoutePlan& InPlan, double InSpeed)
{
	Plan = InPlan;
	Travelled = 0.0;
	Speed = InSpeed;
}

bool FRouteFollower::Advance(double DeltaSeconds, FVector2D& OutPosition, double& OutHeading)
{
	if (!Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		return false;
	}

	// Clamped rather than allowed to run on, so a long frame - a hitch, or a breakpoint -
	// leaves the agent at its destination instead of somewhere past the end of the world.
	Travelled = FMath::Clamp(Travelled + Speed * DeltaSeconds, 0.0, Plan.Length);

	return GuidelineGeom::PointAtDistance(Plan.Polyline, Travelled, OutPosition, OutHeading);
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
