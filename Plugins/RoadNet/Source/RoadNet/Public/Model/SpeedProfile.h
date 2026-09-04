#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "SpeedProfile.generated.h"

/**
 * How fast an aircraft MAY be at each point of a route. Built once, then read.
 *
 * Separate from FRouteFollower because it answers a different question. The follower knows
 * where the aircraft is; this knows what the route permits, which depends on nothing that
 * changes while driving it. Keeping them apart is what makes "did it plan the corner
 * correctly" a question that can be asked without simulating anything.
 *
 * WHY IT HAS TO EXIST AT ALL. Once braking is limited, reacting is too late: discovering a
 * corner by arriving at it and then braking at 2 m/s2 puts an aircraft twenty-five metres
 * past the turn. So the aircraft must know the corner is coming, and the cheapest honest way
 * to know is to have worked the whole route out in advance - the route does not change.
 *
 * THE BACK-PASS IS THE WHOLE TRICK. Walking the caps backwards and raising each one to
 * sqrt(next^2 + 2 a s) turns "here is where it must be slow" into "here is where it must
 * START slowing", and after it every cap in the array is reachable by braking at a rate the
 * airframe actually has. Standard velocity-profile planning - the same pass a CNC feed
 * planner or a racing line makes - and named here because it looks like an optimisation and
 * is in fact the entire point.
 *
 * IT SAMPLES THE SAME HEADING FUNCTION THE FOLLOWER WILL BE GIVEN, through
 * GuidelineGeom::VertexHeadings, and that is deliberate rather than incidental. A profile
 * derived from its own idea of the curve would let the planner brake for a corner in a place
 * the driver does not agree is a corner - which is the second-evaluator failure this graph
 * has already been designed once to avoid. See GuidelineGeom.
 */
USTRUCT()
struct ROADNET_API FSpeedProfile
{
	GENERATED_BODY()

	/**
	 * Works out what Points permits for an aircraft with this performance.
	 *
	 * Clears first. Safe on a polyline too short to drive, which leaves it empty and makes
	 * LimitAt return the taxi speed - a follower with no plan is refused before it gets here.
	 */
	void Build(const TArray<FVector2D>& Points, const FGroundPerformance& Ground);

	/**
	 * The fastest the aircraft may be Distance along the route.
	 *
	 * Interpolated as sqrt(v^2 + 2 a s) rather than linearly, because that is the shape a
	 * braking curve has: a straight line between two vertex limits dips below the real curve
	 * in the middle and would ask for a deceleration the airframe has not got.
	 */
	double LimitAt(double Distance) const;

	bool IsEmpty() const { return Distances.Num() < 2; }

private:
	/** Cumulative distance to each vertex. Distances[0] is 0. */
	UPROPERTY() TArray<double> Distances;

	/**
	 * The cap AT each vertex, after the backward pass. Distances.Num() entries.
	 *
	 * The last is zero: an aircraft arriving at its destination stops there. That is also
	 * why FGroundPerformance::MinTaxiSpeed is not applied here - it bounds what a TURN may
	 * slow the aircraft to, and an aeroplane parked on a stand is not turning.
	 */
	UPROPERTY() TArray<double> VertexLimits;

	/**
	 * The cap ALONG each span, from its curvature. Distances.Num() - 1 entries.
	 *
	 * Held separately from the vertex limits because it does not brake-propagate: it is a
	 * standing restriction for the length of the span, the way a speed limit on a bend is,
	 * whereas a vertex limit is a point the aircraft has to be slow BY.
	 */
	UPROPERTY() TArray<double> SpanCaps;

	/** Kept so LimitAt can shape the braking curve between vertices. */
	UPROPERTY() double Decel = 200.0;

	/** What LimitAt reports when nothing was built. */
	UPROPERTY() double Fallback = 1000.0;
};
