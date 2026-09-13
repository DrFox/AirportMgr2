#pragma once

#include "CoreMinimal.h"
#include "Model/RouteSearch.h"
#include "PushbackRun.generated.h"

/**
 * How far through the manoeuvre a push is.
 *
 * AN ENUM AND NOT A BOOL, and there really are two laws rather than one dressed up: Back
 * holds the heading and moves; Swing moves AND turns. They can never both be current and
 * they are visited in one order, so the illegal state stops being representable - the same
 * rule EAgentPhase and ECrossingPhase follow.
 */
UENUM()
enum class EPushPhase : uint8
{
	/** Straight down the stand's lead-in. The body keeps the heading it parked at. */
	Back,

	/** Round the corner and onto the taxiway, the heading swinging to meet the line. */
	Swing
};

/**
 * One aeroplane being pushed or reversed off a stand: the fourth motion phase, beside
 * FLandingRun, FRouteFollower and FTakeoffRun. FRoadAgent owns the switch between them.
 *
 * IT WALKS THE DEPARTURE PLAN AND DOES NOT INVENT GEOMETRY. DeparturePlanner::PlanAny
 * already returns a route whose Steps[0] IS the stand lead-in, with its EndDistance computed
 * off the same polyline the follower walks. Keeping ONE plan and ONE distance along it is
 * what lets FClaimPass arbitrate a push with the machinery it already has: a run that
 * computed its own back-out line would be a second evaluator of where the agent is, which
 * the guideline invariant forbids in as many words.
 *
 * TRAVELLED KEEPS FRouteFollower'S MEANING - the STEERED axle's distance along
 * Plan.Polyline. The tug couples at the nose gear and its driver follows the painted line,
 * so the nose gear stays constrained to the guideline through the whole manoeuvre. What
 * reverses is which end of the BODY is ahead of that axle, and that is a fact about the
 * OFFSETS rather than about the line: a parked aeroplane's nose gear is at s = 0 and its
 * tail at s > 0, because the lead-in runs away from the terminal it faces. So as Travelled
 * grows the MAINS LEAD and the nose gear trails, which is what a towbar push is. See
 * FClaimPass::CentreOf, which is the one place that has to know.
 *
 * WORLD-FREE, like the three phases it sits beside: Airside.Model.PushbackRun drives the
 * whole of it with no actor, no world and no view.
 */
USTRUCT()
struct AIRSIDE_API FPushbackRun
{
	GENERATED_BODY()

	UPROPERTY() EPushPhase Phase = EPushPhase::Back;

	/**
	 * The departure route. A COPY, as FRouteFollower::Plan is, so the follower can be started
	 * on the same plan at PushDistance when this hands over - and so a graph rebuild has one
	 * object to re-resolve rather than two views of one.
	 */
	UPROPERTY() FRoutePlan Plan;

	/** The steered axle's distance along Plan.Polyline, uu. */
	UPROPERTY() double Travelled = 0.0;

	/** uu/s right now. Trapezoidal: up at PushAccel, and down to EXACTLY ZERO at PushDistance
	 *  - see Advance for why a push may not hand over with speed on it. */
	UPROPERTY() double Speed = 0.0;

	/** Which way the BODY faces, radians. Not the direction of travel - that difference is
	 *  the whole of this struct. */
	UPROPERTY() double Heading = 0.0;

	/** Where Back ends and Swing begins: the end of the straight lead-in. */
	UPROPERTY() double BackDistance = 0.0;

	/** Where the whole manoeuvre ends, and exactly what DepartAgent reserved for it. */
	UPROPERTY() double PushDistance = 0.0;

	/**
	 * The plan's tangent at PushDistance. The Swing lerps the body to exactly this, so the
	 * follower inherits ZERO heading error - which is the defect this whole feature removes.
	 */
	UPROPERTY() double TargetHeading = 0.0;

	/** What it was facing on the stand. Back holds it; Swing lerps from it. */
	UPROPERTY() double ParkedHeading = 0.0;

	UPROPERTY() double PushSpeed = 0.0;
	UPROPERTY() double PushAccel = 0.0;

	/** A powerback cannot start without thrust - see FTrafficRules::PowerbackRPMFraction.
	 *  False for anything on a bar: the tug supplies the force, whatever the propeller does. */
	UPROPERTY() bool bNeedsThrust = false;

	/**
	 * The three numbers a push is defined by, WITHOUT starting one.
	 *
	 * STATIC AND SHARED WITH UGroundTraffic::DepartAgent, which needs PushDistance before the
	 * phase begins so it can reserve exactly the ground the push will use. Re-deriving that
	 * arithmetic there would be two expressions that must agree, and one day would not.
	 *
	 * False when the plan cannot be pushed along at all - no steps, or a polyline too short
	 * to have a direction. The outputs are then UNTOUCHED, so a caller that used them anyway
	 * would be reading its own uninitialised doubles: honour the return.
	 */
	static bool PlanPushDistance(const FRoutePlan& InPlan, double SwingLength, double MaxBack,
		double& OutBackDistance, double& OutPushDistance, double& OutTargetHeading);

	/** Arms the manoeuvre. False, and NOTHING is touched, when PlanPushDistance declines. */
	bool Start(const FRoutePlan& InPlan, double InParkedHeading, double InPushSpeed,
		double InPushAccel, double InSwingLength, double InMaxBack, bool bInNeedsThrust);

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

	/**
	 * True once the manoeuvre has run its length - OR once its plan can no longer carry it
	 * that far.
	 *
	 * CLAMPED TO Plan.Length, and that second half is not belt-and-braces. A graph rebuild can
	 * TRUNCATE a plan under an agent (see FPlanReResolver: the route keeps the longest prefix
	 * that is still pavement), and a plan truncated shorter than PushDistance would leave
	 * Travelled unable to reach it - a push that never ends, on an aeroplane wedged half off
	 * its stand, with nothing in the model able to notice. Ending at the new end instead hands
	 * over to the follower with a small heading error, which the follower is perfectly able to
	 * slew out; a truncation under a pushing aeroplane is a degraded case either way, and this
	 * is the recoverable one.
	 */
	bool HasArrived() const
	{
		return Travelled >= FMath::Min(PushDistance, Plan.Length) - UE_KINDA_SMALL_NUMBER;
	}
};
