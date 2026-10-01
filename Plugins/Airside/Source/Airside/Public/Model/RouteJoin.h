#pragma once

#include "CoreMinimal.h"
#include "Model/RouteSearch.h"

/**
 * A ROUTE ENTERED FROM A POINT OFF IT BY ONE STRAIGHT JOIN LEG (#502) - the shape two callers built by hand: the held taxi
 * out's leg from where a push ended to the first node of its new route (UGroundTraffic::ReplanHeldTaxiOut), and a push's leg
 * onto the live line a rebuild moved under it (FPushbackRun::Rejoin). A LEG, NOT A HOP: the follower and a push both walk
 * Plan.Polyline by Travelled, so a route that begins anywhere but where the agent stands is a jump on its first frame.
 */
namespace RouteJoin
{
	/**
	 * Route entered from From: a straight leg to the point JoinAlong along Route, then Route on from there. THE LEG IS INSIDE
	 * the first step reaching past JoinAlong, so every step still ends at a vertex of the line it names: a step ending at or
	 * before JoinAlong ends AT the join (vertex 1, the leg's length), and every later one keeps its own end vertex and length,
	 * re-based by the leg. JoinAlong 0 prepends the leg to the whole route. Length is the leg plus what was left of Route, so
	 * the last step's EndDistance is still the route's length. Where the leg meets the line and how steeply is the caller's to
	 * judge - FPushbackRun::Rejoin refuses a steep one, the held taxi out takes any within its 30 m.
	 * False, and OutJoined untouched, for a JoinAlong off Route or a route too short to have a point there.
	 * ENFORCED BY: Airside.Model.RouteJoinPrepend (the arithmetic, both callers' shapes)
	 */
	AIRSIDE_API bool Prepend(const FRoutePlan& Route, const FVector2D& From, double JoinAlong, FRoutePlan& OutJoined);
}
