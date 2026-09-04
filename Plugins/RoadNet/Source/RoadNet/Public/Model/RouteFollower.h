#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "Model/RouteSearch.h"
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
 * WHAT IS STILL NOT HERE, unchanged: no acceleration, and no awareness of any other agent.
 * Two agents pass straight through each other, hold-short nodes are not consulted, and the
 * right-of-way rules the graph already carries are ignored. Those are a traffic model. This
 * is still the thing that has to work before a traffic model means anything - it has simply
 * stopped being wrong about one aircraft on an empty airport.
 *
 * It is still world-free: FTaxiPerformance is three doubles, so the whole of "does it round
 * a corner like an aeroplane" is testable by calling Advance in a loop. See RoadNet.Model.TurnRate.
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
	 * Advance rewrites this every frame as the aircraft slows for turns it cannot make at
	 * pace. What it was asked for is Taxi.TaxiSpeed, which does not change.
	 */
	UPROPERTY() double Speed = 1000.0;

	/** What the airframe can do. See FTaxiPerformance. */
	UPROPERTY() FTaxiPerformance Taxi;

	/**
	 * The crab angle at which speed has fallen all the way to FTaxiPerformance::MinTaxiSpeed.
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

	void Start(const FRoutePlan& InPlan, const FTaxiPerformance& InTaxi);

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
