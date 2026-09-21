#pragma once

#include "CoreMinimal.h"
#include "Model/Airframe.h"
#include "Model/RouteSearch.h"
#include "Model/SpeedProfile.h"
#include "RouteFollower.generated.h"

/**
 * Walks a route plan, as fast as the airframe can take the corners.
 *
 * Deliberately knows nothing about actors, ticks or worlds: it is a distance and a
 * polyline, so the whole of "does the cube go the right way" is testable by calling
 * Advance in a loop with no world at all. The actor that carries one is a view.
 *
 * It walks Plan.Polyline - the very array the overlay draws - so the agent cannot drift
 * off the line the player was shown. See GuidelineGeom.
 *
 * IT NOW HAS A TURN RATE, and that is a boundary this comment used to draw being crossed
 * on purpose, so it is worth saying exactly how far it moved.
 *
 * Heading no longer snaps to the direction of travel. It is state, it slews toward the line
 * at the airframe's rate, and when it cannot keep up the aircraft SLOWS DOWN rather than
 * leaving the line - see Advance. What that buys is the only reason to do it: an aeroplane
 * that swings its nose at five thousand degrees a second does not look like an aeroplane,
 * and no amount of traffic modelling would have fixed that.
 *
 * IT ALSO HAS ACCELERATION, which arrived second and forced something bigger than itself.
 * Braking at a real rate means a corner cannot be discovered by reaching it - two metres a
 * second squared puts the aircraft twenty-five metres past the turn - so the follower now
 * PLANS: FSpeedProfile works out what the whole route permits before the first frame, and
 * Advance follows it. That is the first predictive thing in this class.
 *
 * WHAT IS STILL NOT HERE: any awareness of WHO is ahead. StopWithin (see Advance) is the one
 * number the traffic model is allowed to hand in - a distance, not a reason - so this class
 * still does not know whether it is a holding-position node, another aircraft's tail, or a
 * deadlock resolver that put the cap there. Two aircraft told nothing would still pass
 * straight through each other, and the right-of-way rules the graph carries are still
 * somebody else's job. That somebody is UGroundTraffic: it watches the other agents and the
 * graph's own rules and turns what it sees into the one number this class understands.
 *
 * It is still world-free: FGroundPerformance is a handful of doubles, so the whole of "does
 * it round a corner like an aeroplane" is testable by calling Advance in a loop with no world
 * at all. See Airside.Model.TurnRate.
 */
USTRUCT()
struct AIRSIDE_API FRouteFollower
{
	GENERATED_BODY()

	UPROPERTY() FRoutePlan Plan;

	/** How far along Plan.Polyline, in uu. */
	UPROPERTY() double Travelled = 0.0;

	/**
	 * uu per second, RIGHT NOW - not the speed it was dispatched at.
	 *
	 * Advance rewrites this every frame, within the airframe's acceleration and braking, as
	 * the aircraft winds up, slows for turns it cannot take at pace, and stops at the end.
	 * What it was ASKED for is Ground.Taxi.SpeedCap, which does not change.
	 *
	 * Starts at zero: an aeroplane leaves a stand from rest.
	 */
	UPROPERTY() double Speed = 0.0;

	// GROUND IS NOT STORED HERE ANY MORE (issue #83) - see FLandingRun's own note. Start,
	// Advance and Replace take the airframe by reference from FRoadAgent::Airframe instead.

	/**
	 * The crab angle at which speed has fallen all the way to FGroundPerformance::MinSteeringSpeed.
	 *
	 * The CONTROLLER's tolerance for "still tracking the line", not a fact about any
	 * airframe - which is why it is here and not on the type. Ten degrees is deliberately
	 * well inside the twenty that GuidelineGeom treats as a real corner rather than a
	 * sampling artefact: the follower must never be the reason an agent looks like it is
	 * cornering when it is not.
	 *
	 * Public because a test that copied this number could pass while disagreeing with it.
	 */
	static constexpr double CrabAtMinSpeedDegrees = 10.0;

	/**
	 * The slowest a PLAN may ask for, uu/s. 0.1 m/s - small enough never to be seen.
	 *
	 * A SOLVER GUARD, NOT A PERFORMANCE FIGURE, and that is why it lives here rather than on
	 * FGroundPerformance. Two loops in this model divide by their own progress, and both
	 * deadlock at zero:
	 *
	 *   the crab loop - if v = 0 and the heading error exceeds the lock, MaxStep is zero, the
	 *   heading never moves, the crab never shrinks, and the agent sits at zero for ever with
	 *   an error it cannot resolve;
	 *
	 *   a sharp vertex - FSpeedProfile plans a stop there, the backward pass brakes to rest,
	 *   and LimitAt returns zero from then on.
	 *
	 * Neither is physics. FGroundPerformance::MinSteeringSpeed was doing this by accident, at
	 * 0.5 m/s - five times larger than it needs to be, authored per vehicle as though it were
	 * a fact about the machine, and indistinguishable in the logs from the other rules
	 * reporting the same number.
	 *
	 * APPLIED BESIDE THE PHYSICAL FLOOR, never instead of it: an aircraft keeps its own
	 * steering minimum, which is far above this, and only something authored at zero ever
	 * sees this figure at all.
	 */
	static constexpr double ProgressEpsilon = 10.0;

	/** Which way the agent is FACING, radians. Its own state now, not a function of where it is. */
	UPROPERTY() double Heading = 0.0;

	/**
	 * The steering angle this frame, degrees, signed the way Heading turns.
	 *
	 * STATE RATHER THAN AN OUT-PARAMETER, because Advance's signature is the one FAgentMotion
	 * exists to keep from growing again: "SetPose used to take a position, a heading, a
	 * surface height, an altitude and a pitch... Eight arguments in a row is a signature
	 * nobody can call correctly."
	 *
	 * It is the INPUT to the motion rather than a description of it - the view animates the
	 * nose gear from this exact number, so what the player sees the wheel doing is what
	 * turned the aeroplane. Zero on a pivot-law airframe, which has no steered wheel to draw.
	 */
	UPROPERTY() double SteerDegrees = 0.0;

	/**
	 * The yaw actually applied this frame, degrees per second.
	 *
	 * Kept beside SteerDegrees so a reader can check the two agree - they are the same fact
	 * stated as an angle and as a rate. Nothing outside a test reads it.
	 */
	UPROPERTY() double YawRateDegPerSec = 0.0;

	/**
	 * What this route permits, worked out once in Start. See FSpeedProfile.
	 *
	 * Held by value rather than rebuilt per frame because the route does not change: an
	 * agent that re-planned every tick would be spending the whole airport's frame budget
	 * arriving at the same answer.
	 */
	UPROPERTY() FSpeedProfile Profile;

	void Start(const FRoutePlan& InPlan, const FAirframe& InAirframe, double InitialSpeed = 0.0,
		TOptional<double> InitialHeading = TOptional<double>(), double InitialTravelled = 0.0);

	/**
	 * Moves forward by DeltaSeconds and reports where that leaves the agent.
	 *
	 * StopWithin is the ONE input the traffic model adds (spec 3.8): the distance, from
	 * where the agent is at the start of this tick, beyond which it may not go. It becomes
	 * a third cap on the target speed - sqrt(2 a s), the same shape the profile's own
	 * braking curve has - and a clamp on Travelled, so a long frame cannot carry the agent
	 * through a node it was told to hold at. Unbounded means today's behaviour exactly.
	 *
	 * False when there is no valid route to walk, leaving the outputs untouched - so a
	 * caller that ignores the return value leaves its agent where it was rather than
	 * teleporting it to the origin, which is this project's most-repeated bug.
	 *
	 * InAirframe MUST be the same one Start (or the last Replace) was given - see
	 * FLandingRun::Advance's note; this is the same contract by the same construction.
	 */
	bool Advance(double DeltaSeconds, const FAirframe& InAirframe, double StopWithin,
		FVector2D& OutPosition, double& OutHeading);

	/** Advance with nothing ahead. Kept so every caller and test from before the traffic
	 *  model reads exactly as it did. */
	bool Advance(double DeltaSeconds, const FAirframe& InAirframe, FVector2D& OutPosition, double& OutHeading)
	{
		return Advance(DeltaSeconds, InAirframe, TNumericLimits<double>::Max(), OutPosition, OutHeading);
	}

	/**
	 * Swaps the plan under a MOVING agent, keeping Travelled, Speed and Heading.
	 *
	 * Start() is for a dispatch: it resets to rest at the polyline's first point. A replan
	 * spliced at a node the agent has not reached yet must not do that - the agent is part
	 * way along a line that is unchanged up to the splice, so only the profile is rebuilt.
	 *
	 * TAKES THE AIRFRAME TOO, now that Ground is not stored here (issue #83): the profile
	 * rebuild below needs it exactly as Start's did.
	 */
	void Replace(const FRoutePlan& NewPlan, const FAirframe& InAirframe);

	/** True once the whole polyline has been walked. Always true for an invalid plan. */
	bool HasArrived() const;
};
