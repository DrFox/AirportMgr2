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

	/** How fast a vehicle backs up, uu/s. A crawl by nature - nobody reverses at taxi speed.
	 *  THE ASK, not the state: Start writes it once and nothing else touches it. Speed below
	 *  is what the vehicle actually managed. */
	UPROPERTY() double ReverseSpeed = 0.0;

	/**
	 * uu/s right now, as a MAGNITUDE - what the last Advance actually covered, over its own
	 * delta. Zero while arbitration holds the vehicle, and less than ReverseSpeed on the frame
	 * that arrives.
	 *
	 * SEPARATE FROM ReverseSpeed rather than replacing it, and the distinction is the one
	 * FRouteFollower::Speed draws in its own header - "what it was ASKED for is
	 * Ground.Taxi.SpeedCap, which does not change". FRoadAgent::DescribeMotion used to report
	 * the ask, so a truck held at a standstill told the view it was doing a full metre a
	 * second and its wheels spun on the spot. A held vehicle must still remember what it
	 * intends to back up at once it is released, which is why the cap survives.
	 *
	 * UNSIGNED, like FPushbackRun::Speed. Which way a phase points is DescribeMotion's to say,
	 * once, rather than a fact repeated in every run struct for the phases that go backwards.
	 */
	UPROPERTY() double Speed = 0.0;

	/**
	 * Where the steered wheels are pointing, degrees. Zero on a pivot-law airframe, which has
	 * no steered wheel to draw - the rule FRouteFollower states at its own SteerDegrees.
	 *
	 * IT IS GEOMETRY, NOT AN INPUT. Playback has no error term to steer on, so this is derived
	 * from the curve the fixed axle is on: backing along an arc, a rigid vehicle pivots about
	 * that axle, so tan(steer) = Wheelbase / Radius - the exact inverse of
	 * FAirframe::TightestReversibleRadius. At the vehicle's own limit it comes out as its own
	 * lock, which is what Airside.Model.ReverseSteersRatherThanSliding checks it against.
	 *
	 * OPPOSITE THE YAW, and that is not a sign slip. Reversing counter-steers: the front wheels
	 * go right to swing the back of the vehicle left. FRouteFollower::SteerDegrees is
	 * documented "signed the way Heading turns" because going forwards the two coincide; going
	 * backwards they cannot.
	 *
	 * WHY IT EXISTS AT ALL: FRoadAgent::DescribeMotion used to read the FOLLOWER's steering in
	 * every phase, and the follower does not run during a reverse - so the wheels held the
	 * angle they had when the manoeuvre armed. Reported from play on 2026-09-20 as a truck
	 * that "straightened its wheels while still turning and then slid around".
	 */
	UPROPERTY() double SteerDegrees = 0.0;

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
	 *
	 * THE AIRFRAME IS TAKEN PER FRAME, as FLandingRun::Advance and FTakeoffRun::Advance take
	 * it, rather than having Start copy a wheelbase and a steering lock in. Two struct fields
	 * that must agree with an FAirframe somewhere else are two chances to disagree with it;
	 * see CLAUDE.md, "one struct per thing". SteerDegrees is what needs it.
	 */
	bool Advance(double DeltaSeconds, const FAirframe& Airframe, double StopWithin,
		FVector2D& OutPosition, double& OutHeading);

	/** True once the vehicle has backed the length of its manoeuvre. */
	bool HasArrived() const { return Travelled >= Plan.Length - UE_KINDA_SMALL_NUMBER; }
};
