#pragma once

#include "CoreMinimal.h"
#include "Model/RouteSearch.h"
#include "Model/SpeedProfile.h"
#include "ReverseRun.generated.h"

/**
 * One ground vehicle backing into a service bay: the fifth motion phase, beside FLandingRun,
 * FRouteFollower, FTakeoffRun and FPushbackRun. FRoadAgent owns the switch between them.
 *
 * IT PLAYS BACK A CURVE, IT DOES NOT TRACK ONE, and that distinction is the entire design.
 * Both ends of the manoeuvre are known before it begins - the vehicle is at its staging pose
 * and the bay pose is painted on the concrete - so the shape between them is solved ONCE, by
 * whatever laid the bay, and this walks it. There is no controller, no gains and no error
 * term.
 *
 * WHY NOT TRACK A LINE IN REVERSE. Reversing is unstable in the way forward driving is not: a
 * heading error GROWS rather than self-corrects, as anyone who has pushed a trolley backwards
 * knows. Tracking would need a tuned controller and would be the hardest thing in this
 * codebase to get right, in exchange for a generality nothing has asked for - every reverse
 * this game performs is between two poses somebody already chose.
 *
 * FPushbackRun IS NOT REUSABLE FOR THIS, though it looks close. It works precisely because a
 * tug couples at the NOSE GEAR, so the steered axle still leads and the ordinary forward
 * kinematics hold with the body turned round. A truck backing into a bay has no tug: it
 * pivots about its FIXED axle, which is why its limit is L/tan(lock) and not L/sin(lock).
 *
 * THE CURVE IS CHECKED WHEN THE MANOEUVRE IS ARMED, not assumed. Start refuses a curve this
 * airframe cannot hold going backwards and says so in the log. That is the whole of the
 * "no crabbing" guarantee: it is enforced where the manoeuvre begins rather than promised by
 * whatever drew the bay. See FSpeedProfile and EDriveDirection.
 *
 * WORLD-FREE, like the four phases it sits beside: Airside.Model.ReverseRun drives the whole
 * of it with no actor, no world and no view.
 */
USTRUCT()
struct AIRSIDE_API FReverseRun
{
	GENERATED_BODY()

	/**
	 * The solved manoeuvre - a COPY, as FRouteFollower::Plan is, so a graph rebuild has one
	 * object to re-resolve rather than two views of one.
	 */
	UPROPERTY() FRoutePlan Plan;

	/** The FIXED axle's distance along Plan.Polyline, which is the point the body pivots
	 *  about going backwards. FRouteFollower measures the steered axle; this does not. */
	UPROPERTY() double Travelled = 0.0;

	/** How fast a vehicle backs up, uu/s. A crawl by nature - nobody reverses at taxi speed. */
	UPROPERTY() double ReverseSpeed = 0.0;

	/**
	 * Arms the manoeuvre, and REFUSES it if this airframe cannot back along that curve.
	 *
	 * Returns false and touches nothing when the plan is invalid, too short to have a
	 * direction, or asks for an arc tighter than FAirframe::TightestReversibleRadius - the
	 * last of which is logged with the radius and the place, because a bay that cannot be
	 * entered is a defect in whatever laid it and the log is what finds it.
	 */
	bool Start(const FRoutePlan& InPlan, const FAirframe& Airframe, double InReverseSpeed);

	/**
	 * One frame. FALSE MEANS THE MANOEUVRE IS OVER and the caller should hand over - the same
	 * contract FLandingRun::Advance and FPushbackRun::Advance use, so FRoadAgent's arm for
	 * this phase reads like the arms either side of it.
	 *
	 * StopWithin is arbitration's one input, as it is for the follower: a vehicle asked to
	 * hold stops where it is and resumes when the distance opens again.
	 *
	 * OutHeading is the curve's tangent turned through 180 degrees, the whole way - the body
	 * faces AWAY from the direction of travel, which is what backing in means. No swing phase
	 * and no target heading, for the reason FPushbackRun's header gives about walking the
	 * right line in the first place.
	 */
	bool Advance(double DeltaSeconds, double StopWithin,
		FVector2D& OutPosition, double& OutHeading);

	/** True once the vehicle has backed the length of its manoeuvre. */
	bool HasArrived() const { return Travelled >= Plan.Length - UE_KINDA_SMALL_NUMBER; }
};
