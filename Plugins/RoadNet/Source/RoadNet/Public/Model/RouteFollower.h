#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
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
 * WHAT IS STILL NOT HERE: any awareness of another agent. Two aircraft pass straight through
 * each other, hold-short nodes are not consulted, and the right-of-way rules the graph
 * already carries are ignored. That is the traffic model, and it is still the thing this has
 * to work before - it has simply stopped being wrong about one aircraft on an empty airport.
 *
 * It is still world-free: FGroundPerformance is a handful of doubles, so the whole of "does
 * it round a corner like an aeroplane" is testable by calling Advance in a loop with no world
 * at all. See RoadNet.Model.TurnRate.
 */
USTRUCT()
struct ROADNET_API FRouteFollower
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

	/** What the airframe can do. See FGroundPerformance. */
	UPROPERTY() FGroundPerformance Ground;

	/**
	 * The crab angle at which speed has fallen all the way to FGroundPerformance::MinTaxiSpeed.
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

	/** Which way the agent is FACING, radians. Its own state now, not a function of where it is. */
	UPROPERTY() double Heading = 0.0;

	/**
	 * What this route permits, worked out once in Start. See FSpeedProfile.
	 *
	 * Held by value rather than rebuilt per frame because the route does not change: an
	 * agent that re-planned every tick would be spending the whole airport's frame budget
	 * arriving at the same answer.
	 */
	UPROPERTY() FSpeedProfile Profile;

	void Start(const FRoutePlan& InPlan, const FGroundPerformance& InGround);

	/**
	 * Moves forward by DeltaSeconds and reports where that leaves the agent.
	 *
	 * False when there is no valid route to walk, leaving the outputs untouched - so a
	 * caller that ignores the return value leaves its agent where it was rather than
	 * teleporting it to the origin, which is this project's most-repeated bug.
	 */
	bool Advance(double DeltaSeconds, FVector2D& OutPosition, double& OutHeading);

	/** True once the whole polyline has been walked. Always true for an invalid plan. */
	bool HasArrived() const;
};
