#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

struct FRoadAgent;
struct FRoutePlan;
struct FRouteQuery;
struct FTrafficContext;

// THE REJOIN SEARCH AND ITS REACHES (#502): RejoinNearby puts an agent whose line went from under it onto the live
// pavement nearest it, running its way, with a route on from there; each radius is one caller's reach. Moved whole out
// of GroundTrafficRebuild.cpp's anonymous namespace (it was 1690 lines, rule 77), with UGroundTraffic::RescueStranded,
// its third caller, into GroundTrafficRejoin.cpp. A PRIVATE header: the search writes nothing to the agent and leaves the
// route change's aftermath to its caller, which only the traffic model's own files know how to spell.
namespace GroundTrafficRejoin
{
	/**
	 * How far a driving vehicle may be moved sideways onto a lane running its way after a
	 * drive-side flip, uu: two Wide lanes (2 x 450), rounded up. The flip moves every lane by
	 * the lane spacing; anything further is a different road.
	 */
	constexpr double FlipRejoinRadius = 1000.0;

	/**
	 * How far a driving agent whose own step no longer re-resolves may be moved onto a live edge
	 * running its way, uu: 3 m. Issue #396: linking a new exit to a taxiway SPLITS the edge under
	 * a taxiing aeroplane - same ground, two edges and a junction node where there was one edge -
	 * and re-resolve, which matches a step by its two end nodes, found no edge between them and
	 * stranded it as if the pavement had gone. A split leaves the line where it was (0 uu on
	 * Airside.Model.Traffic.RebuildSplitUnderTheAgent); 3 m is the drive-side flip's own accepted
	 * jump, for a re-fitted curve. Deleted pavement has nothing this close running the same way -
	 * GraphRebuild's case 5 bypass is 3123 uu off - so it still strands.
	 */
	constexpr double SplitRejoinRadius = 300.0;

	/**
	 * How far from a pushing aeroplane a live line may run for its push to rejoin it, uu: 10 m (#498 review). Wider than
	 * SplitRejoinRadius because a push does not hop - FPushbackRun::Rejoin drives a join leg - and because the edits that
	 * leave a push off its line are a junction DRAGGED behind it, a few metres (5 m on both pins). Deleted ground still has
	 * nothing this close running the push's way, and that push stops and holds.
	 * ENFORCED BY: Airside.Model.PushbackJunctionMovedBehindItCompletes (5 m, rejoined),
	 * Airside.Model.PushbackOnDeletedGroundStops (deleted, holds)
	 */
	constexpr double PushRejoinRadius = 1000.0;

	// THE RESCUE'S HOP (RescueRejoinRadius) is UGroundTraffic's own public constant since #429's review, with its reason
	// on it: a test measures a rescue against it, and a figure retyped in a test is a second one that drifts.

	/**
	 * The nearest point, within Radius of the agent, on an edge running the way it is facing with
	 * a route to its goal from there; that route and how far along its first step the point is.
	 * The agent hops onto it. Two callers: a drive-side flip (FlipRejoinRadius - a visible 3-4 m
	 * jump, once, on a deliberate airport-wide setting change; the alternative was stranding every
	 * truck on the road), and a step that no longer re-resolves under a driving agent
	 * (SplitRejoinRadius - see there). A THIRD, the player's rescue of a stranded agent
	 * (UGroundTraffic::RescueStranded, RescueRejoinRadius), passes WantedGoal: Send home and Find
	 * stand rescue toward a goal that is not the agent's own.
	 *
	 * FINDS, AND WRITES NOTHING TO THE AGENT (issue #429). It used to set the goal it found on the
	 * agent before returning, which was right for the rebuild (same goal, new handle) and wrong for
	 * the rescue, which had to put the old goal back so ReleaseGoal could free it and then let
	 * TakeGoal write the new one a third time. The found route ENDS at that goal, so each caller's
	 * route change says how the goal follows it: Repoint for the rebuild, Move for the rescue.
	 * ENFORCED BY: C++ const (Agent is const here)
	 *
	 * A FOURTH, bPushed: a push whose step under it does not re-resolve (the rebuild's RejoinPush, PushRejoinRadius, #498
	 * review). A push TRAVELS TAIL FIRST, so "running the way it is facing" is the way its body faces turned about - the
	 * line's own direction, which FPushbackRun's heading law turned about.
	 */
	bool RejoinNearby(const FRoadAgent& Agent, const FRoutePlan& Plan, const FTrafficContext& Context,
		double Radius, FRoutePlan& OutPlan, double& OutTravelled, FVector2D& OutAt,
		FGuidelineNodeId WantedGoal = FGuidelineNodeId(), const FRouteQuery* QueryTemplate = nullptr, bool bPushed = false);
}
