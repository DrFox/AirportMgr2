#pragma once

#include "CoreMinimal.h"
#include "Model/RouteSearch.h"
#include "PushbackRun.generated.h"

/**
 * One aeroplane being pushed or reversed off a stand: the fourth motion phase, beside
 * FLandingRun, FRouteFollower and FTakeoffRun. FRoadAgent owns the switch between them.
 *
 * IT WALKS A ROUTE OF ITS OWN, and that is the correction this struct was rewritten for. It
 * used to walk a prefix of the DEPARTURE route, which looks right at the stand - the
 * aeroplane reverses down its lead-in correctly - and is wrong the moment it reaches the
 * taxiway, because it then keeps reversing the way it meant to taxi and finishes past the
 * junction on the wrong side of its own turn. Reported from PIE with photographs, as "it
 * crabbed around using the wrong arm".
 *
 * PushbackPlanner::Plan builds the route instead: the stand, down the lead-in, and onto the
 * arm of the junction the departure does NOT take. Walking THAT forwards with the body
 * reversed is a real pushback - the aeroplane finishes on the taxiway beyond the turn, facing
 * the way it will taxi, and drives forward through the junction afterwards.
 *
 * THE HEADING LAW IS ONE LINE BECAUSE OF THAT. Facing is the line's tangent turned through
 * 180 degrees, the whole way. At the stand the lead-in runs away from the terminal, so
 * tangent + PI is exactly the parked heading; at the far end the route runs AWAY from where
 * the aeroplane is going, so tangent + PI is exactly the taxi-out heading. There is no swing
 * phase, no target heading and no corner to find - an earlier version needed all three only
 * because it was walking the wrong line, and each of them was a place to get it wrong.
 *
 * TRAVELLED IS THE STEERED AXLE'S distance along Plan.Polyline, as FRouteFollower's is. The
 * tug couples at the nose gear and its driver follows the painted line, so the nose gear
 * stays on the guideline throughout; what reverses is which end of the BODY leads it. See
 * FClaimPass::CentreOf, the one place that has to know.
 *
 * WORLD-FREE, like the three phases it sits beside: Airside.Model.PushbackRun drives the
 * whole of it with no actor, no world and no view.
 */
USTRUCT()
struct AIRSIDE_API FPushbackRun
{
	GENERATED_BODY()

	/**
	 * The push's own route - NOT the departure's. A COPY, as FRouteFollower::Plan is, so a
	 * graph rebuild has one object to re-resolve rather than two views of one.
	 */
	UPROPERTY() FRoutePlan Plan;

	/** The steered axle's distance along Plan.Polyline, uu. */
	UPROPERTY() double Travelled = 0.0;

	/** uu/s right now. Trapezoidal, and down to EXACTLY ZERO at the end - see Advance for why
	 *  a push may not hand over with speed on it. */
	UPROPERTY() double Speed = 0.0;

	/** Which way the BODY faces, radians: the line's tangent turned through 180 degrees. That
	 *  it differs from the direction of travel is the whole of this struct. */
	UPROPERTY() double Heading = 0.0;

	UPROPERTY() double PushSpeed = 0.0;
	UPROPERTY() double PushAccel = 0.0;

	/** A powerback cannot start without thrust - see FTrafficRules::PowerbackRPMFraction.
	 *  False for anything on a bar: the tug supplies the force, whatever the propeller does. */
	UPROPERTY() bool bNeedsThrust = false;

	/** Arms the manoeuvre on a route PushbackPlanner::Plan produced. False, and NOTHING is
	 *  touched, when that route is invalid or too short to have a direction at all. */
	bool Start(const FRoutePlan& InPlan, double InPushSpeed, double InPushAccel,
		bool bInNeedsThrust);

	/**
	 * One frame. FALSE MEANS THE PUSH IS OVER and the caller should hand over - the same
	 * contract FLandingRun::Advance uses, so FRoadAgent::Advance's arm for this phase reads
	 * exactly like the arm for an arrival.
	 *
	 * StopWithin is arbitration's one input, as it is for the follower. bHasThrust gates a
	 * powerback only; the agent answers it, because the RPM is the agent's and this struct
	 * holds no engine.
	 */
	bool Advance(double DeltaSeconds, double StopWithin, bool bHasThrust,
		FVector2D& OutPosition, double& OutHeading);

	/** True once the manoeuvre has run the length of its route. */
	bool HasArrived() const { return Travelled >= Plan.Length - UE_KINDA_SMALL_NUMBER; }
};
