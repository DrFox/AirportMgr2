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
struct AIRSIDE_API FSpeedProfile
{
	GENERATED_BODY()

	/**
	 * Works out what Points permits for an aircraft with this performance.
	 *
	 * Clears first. Safe on a polyline too short to drive, which leaves it empty and makes
	 * LimitAt return the taxi speed - a follower with no plan is refused before it gets here.
	 *
	 * TAKES THE AIRFRAME, not just its ground performance (it was FGroundPerformance): the
	 * corner cap now depends on the WHEELBASE as well as on the figures, because whether a
	 * corner can be steered at all is geometric. Passing the bundle is this codebase's "one
	 * struct per thing" - the alternative was a second parameter that some caller would one
	 * day forget to keep in step.
	 */
	void Build(const TArray<FVector2D>& Points, const FAirframe& Airframe);

	/**
	 * The fastest the aircraft may be Distance along the route.
	 *
	 * Interpolated as sqrt(v^2 + 2 a s) rather than linearly, because that is the shape a
	 * braking curve has: a straight line between two vertex limits dips below the real curve
	 * in the middle and would ask for a deceleration the airframe has not got.
	 */
	double LimitAt(double Distance) const;

	bool IsEmpty() const { return Distances.Num() < 2; }

	/**
	 * The taxi cap Build was last given, straight off FGroundPerformance - see Fallback.
	 *
	 * PUBLIC FOR A REASON NARROWER THAN IT LOOKS: with FRouteFollower no longer keeping its
	 * own Ground copy (issue #83), this is the one follower-side record of which airframe's
	 * figures a taxi actually started on - UAirsideTraffic::LastAgentTaxiSpeedCapForTest
	 * reads it rather than the agent's own Airframe, which would be a tautology (the test
	 * already knows what it dispatched; the question is whether the handover used it).
	 */
	double GetFallback() const { return Fallback; }

	/**
	 * THE VERDICT THIS STRUCT ALREADY REACHES, kept instead of discarded.
	 *
	 * Build measures every span, decides whether the route asks for a radius the steering
	 * lock cannot hold, logs that, and then threw the answer away - it lived in three locals.
	 * So the one authority on "can this vehicle drive this line" could be READ only by a
	 * human looking at a log, and every test that wanted to know RE-IMPLEMENTED the rule on
	 * one edge at a time. Four attempts at the stand's routing passed a green suite and
	 * produced a truck that crabbed, because no test ever asked THIS function about a WHOLE
	 * ROUTE - and a route of individually-legal edges can still be illegal where two meet.
	 * Grep for "the same expression FSpeedProfile::Build uses, written out rather than
	 * shared": that comment appears at every site that should have called this instead.
	 *
	 * Restating arithmetic in a test is right, and FAirframe::TightestFollowableRadius argues
	 * for it. Restating a JUDGEMENT is not the same thing: the judgement is what the game
	 * acts on, and a copy of it can agree with itself while disagreeing with the original.
	 */
	bool WasTighterThanLock() const { return bTighterThanLock; }

	/** The tightest radius any span asked for, uu. Zero for a route with no corner in it. */
	double GetTightestRadius() const { return TightestRadiusUu; }

	/** How far along the route that span began, uu - what the warning prints as "at". */
	double GetTightestAt() const { return TightestRadiusAt; }

	/**
	 * THE SECOND RULE, and it is not the same question as the first.
	 *
	 * A span's radius asks "is this curve too tight to follow". A SHARP VERTEX asks "does the
	 * heading change INSTANTLY here" - a corner with no curve in it at all, which no vehicle
	 * takes at any speed, and which the radius rule cannot see because a zero-length turn has
	 * no Length to divide by.
	 *
	 * SPLIT OUT BECAUSE EXPOSING ONLY THE FIRST REPEATED THE ORIGINAL MISTAKE. The verdict was
	 * made readable on 2026-09-16 so tests could stop re-deriving it; the first test written
	 * against it passed while its route contained a 175 degree instantaneous reversal, because
	 * only the radius half had an accessor. Half an authority is still an authority nobody can
	 * fully ask.
	 */
	bool HasSharpVertex() const { return SharpVertexCount > 0; }

	/** How many, and the first one's turn in degrees and its distance along the route. */
	int32 GetSharpVertexCount() const { return SharpVertexCount; }
	double GetSharpestDegrees() const { return SharpestTurnDegrees; }
	double GetSharpestAt() const { return SharpestTurnAt; }

private:
	/** Cumulative distance to each vertex. Distances[0] is 0. */
	UPROPERTY() TArray<double> Distances;

	/**
	 * The cap AT each vertex, after the backward pass. Distances.Num() entries.
	 *
	 * The last is zero: an aircraft arriving at its destination stops there. That is also
	 * why FGroundPerformance::MinSteeringSpeed is not applied here - it bounds what a TURN may
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

	/** See WasTighterThanLock. Filled by Build; meaningless before it has run. */
	UPROPERTY() double TightestRadiusUu = 0.0;
	UPROPERTY() double TightestRadiusAt = 0.0;
	UPROPERTY() bool bTighterThanLock = false;

	/** See HasSharpVertex. Filled by Build; meaningless before it has run. */
	UPROPERTY() int32 SharpVertexCount = 0;
	UPROPERTY() double SharpestTurnDegrees = 0.0;
	UPROPERTY() double SharpestTurnAt = 0.0;
};
