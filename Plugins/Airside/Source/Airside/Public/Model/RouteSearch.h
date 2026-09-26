#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/RoutePolicy.h"
#include "Model/RoadTraffic.h"
#include "Model/SpeedProfile.h"
#include "Model/VehicleFit.h"
#include "RouteSearch.generated.h"

struct FVehicle;

class URoadNetwork;

/**
 * Why a route query answered the way it did.
 *
 * A bare "no route" is the least useful thing a pathfinder can say to someone building an
 * airport, because every cause has a different fix: connect the taxiway, reverse the
 * one-way, or bring a smaller aircraft. These are the distinctions the tool puts on screen.
 */
UENUM()
enum class ERouteResult : uint8
{
	Found,

	/** The start handle names no live node. */
	NoStart,

	/** The goal handle names no live node. */
	NoGoal,

	/** Start and goal are the same node. Not a failure of the graph, but not a route. */
	SameNode,

	/** The graph is connected wrongly, or not at all, for this traversal class. */
	Unreachable,

	/**
	 * Reachable, but only over guidelines this wingspan is too wide for.
	 *
	 * Distinguished from Unreachable by re-running the search unconstrained ONLY after a
	 * failure, so the common case pays nothing. Without it a Code F aircraft aimed at a
	 * Code C stand reports the same thing as a taxiway nobody ever joined up.
	 */
	TooWide,

	/**
	 * Reachable, but only over road this VEHICLE does not fit - a lane narrower than its body,
	 * or a corner its swept path or its lock cannot take (VehicleFit, spec 2026-09-23 §6).
	 * TooWide's twin, found the same way: an unconstrained retry after a failure. The plan's
	 * RejectedEdge names where.
	 *
	 * ALSO, SINCE 2026-09-25, a route whose every edge fits but whose WHOLE drive folds a tow
	 * (VehicleFit::JudgePlan), after RouteSearch::Find's bounded retries found no route that
	 * holds it. Not a new value: the fix is the same kind - a gentler road, or a smaller vehicle
	 * - and RejectedBy says which rule (TrailerFolds) and where.
	 */
	TooNarrow,
};

/** One edge of a found route, in traversal order. */
USTRUCT()
struct AIRSIDE_API FRouteStep
{
	GENERATED_BODY()

	UPROPERTY() FGuidelineEdgeId Edge;

	/** The node this step arrives at, so the route reads forwards without re-deriving it. */
	UPROPERTY() FGuidelineNodeId To;

	/** True when the edge is traversed B to A, which is what reverses its sampled points. */
	UPROPERTY() bool bReversed = false;

	/**
	 * True when this step's edge is a service bay's REVERSE leg - a span meant to be driven
	 * BACKWARDS. Copied off FGuidelineEdge::bReverseLeg by the search.
	 *
	 * NOT bReversed ABOVE, AND THE TWO ARE EASY TO CONFUSE. That one says the edge is walked B
	 * to A, which reverses its sampled POINTS and says nothing about the vehicle. This one says
	 * the vehicle travels the span facing the other way.
	 *
	 * COPIED RATHER THAN LOOKED UP, because FRoadAgent holds no URoadNetwork - it is world-free,
	 * like the five motion phases it switches between - so a plan is the only thing it can read
	 * this from.
	 */
	UPROPERTY() bool bReverseLeg = false;

	/**
	 * Cumulative route distance at which this step's edge ends, and the index of that point
	 * in FRoutePlan::Polyline. Filled by RunSearch from the SAME polyline it appends - never
	 * from the Bezier - so "which edge am I on at Travelled" is answered off the array the
	 * follower walks. A step map derived from the curve would disagree on every bend, and
	 * the arbiter would then stop an agent for a node it had already crossed.
	 */
	UPROPERTY() double EndDistance = 0.0;
	UPROPERTY() int32 EndVertex = 0;
};

/** One run of a route in one direction, for drawing (FRoutePlan::DescribeRuns). */
struct FRouteRun
{
	TArray<FVector2D> Points;
	bool bReverse = false;
};

/**
 * The answer to one route query: what to draw, what to drive, and why not.
 *
 * Polyline is the point of this struct. The overlay draws it and the follower walks it -
 * the SAME array, never two evaluations of one curve - so a cube physically cannot leave
 * the line the player was shown. See GuidelineGeom.
 */
USTRUCT()
struct AIRSIDE_API FRoutePlan
{
	GENERATED_BODY()

	UPROPERTY() ERouteResult Result = ERouteResult::NoStart;

	UPROPERTY() FGuidelineNodeId Start;

	UPROPERTY() TArray<FRouteStep> Steps;

	/** Start to goal, welded across edges: consecutive edges contribute one shared point. */
	UPROPERTY() TArray<FVector2D> Polyline;

	UPROPERTY() double Length = 0.0;

	/**
	 * On TooNarrow: the first edge of the unconstrained route the vehicle does not fit, so a
	 * refusal can say WHERE ("the corner at ...") rather than only that. Unset otherwise.
	 */
	UPROPERTY() FGuidelineEdgeId RejectedEdge;

	/**
	 * On TooNarrow, WHY: the verdict that refused it, figures and all. Per edge (VehicleFit::
	 * Judge on RejectedEdge), or - for a tow - the whole route (VehicleFit::JudgePlan, bWholeRoute
	 * set): "trailer folds at guideline node 41 / (1200, -300), link 0, angle 91 deg". CARRIED
	 * rather than re-judged by the caller, because a whole-route fold is a fact about a plan the
	 * caller never sees - no edge on its own refuses it. Not a UPROPERTY: FFitVerdict is plain.
	 */
	FFitVerdict RejectedBy;

	bool IsValid() const { return Result == ERouteResult::Found; }

	/**
	 * Whether this plan is worth walking: found, with the two points a direction needs, and
	 * long enough to divide a speed into a time by.
	 *
	 * THE SAME THREE-PART CHECK WAS SPELLED OUT SEPARATELY at FPushbackRun::Start,
	 * FReverseRun::Start, FRouteFollower::HasArrived/Advance, VehicleFit::JudgePlan,
	 * UGroundTraffic (three call sites) and FDeparturePlanner (two) - issue #297, "is this
	 * plan drivable spelled ten ways". One of the ten had grown an extra `Length >
	 * UE_KINDA_SMALL_NUMBER` guard the other nine lacked (FReverseRun::Start), which is
	 * exactly how independently-copied checks drift: not wrong anywhere on its own, just
	 * not the same rule everywhere it is asked. Folded in here rather than dropped, since a
	 * zero-length "route" is not drivable at any of the ten sites.
	 */
	bool IsDrivable() const
	{
		return IsValid() && Polyline.Num() >= 2 && Length > UE_KINDA_SMALL_NUMBER;
	}

	/**
	 * Which way the vehicle travels along each SPAN of Polyline - one entry per span, so
	 * Polyline.Num() - 1 of them, Forward unless the step covering it is a bay's reverse leg.
	 *
	 * HERE BECAUSE THE PLAN IS WHAT KNOWS. FSpeedProfile takes the answer and has no idea what
	 * a route step is; FRouteFollower needs it and would otherwise have to walk the step map
	 * itself, which is a second reading of FRouteStep::EndVertex and the kind of duplicate
	 * this file's own comment on that field warns about.
	 *
	 * OFF EndVertex, NEVER RE-SAMPLED, for the same reason RouteSearch::Section is: that index
	 * points into the SAME polyline the follower walks, so the spans it names are the spans
	 * that exist rather than a second evaluation of the curve.
	 */
	void DescribeSpanDirections(TArray<EDriveDirection>& Out) const;

	/**
	 * The polyline cut into runs of one direction - forward, or backing along a reverse leg -
	 * each run's last point the next's first, for a view drawing each in its own style (spec
	 * 2026-09-26 §5). Off DescribeSpanDirections, so it names the spans the follower walks.
	 * ENFORCED BY: Airside.Tool.RouteSpansSplitAtReverse
	 */
	void DescribeRuns(TArray<FRouteRun>& Out) const;
};

namespace RouteSearch
{
	/**
	 * The portion of Plan covering steps [First, Last] inclusive, as a plan of its own.
	 *
	 * FOR HANDING A SPAN TO A DIFFERENT MOTION PHASE. FReverseRun plays back a whole FRoutePlan
	 * and knows nothing of routes, so the reverse leg buried in the middle of a taxi has to be
	 * cut out before it can be armed. Distances and vertex indices are rebased, so the section
	 * reads as though it had been searched for on its own.
	 *
	 * OFF THE STEP MAP, NEVER RE-SAMPLED. FRouteStep::EndVertex indexes the SAME Polyline the
	 * follower walks - see the comment there - so a section built from it shares those exact
	 * points rather than a second evaluation of the curve that would differ on every bend.
	 *
	 * An empty plan back when the range is not a range, which is what a caller that found no
	 * span should already have checked.
	 */
	AIRSIDE_API FRoutePlan Section(const FRoutePlan& Plan, int32 First, int32 Last);
}

struct FTrafficOccupancy;

/** What is being routed, and what it is allowed to use. */
USTRUCT()
struct AIRSIDE_API FRouteQuery
{
	GENERATED_BODY()

	UPROPERTY() FGuidelineNodeId Start;

	UPROPERTY() FGuidelineNodeId Goal;

	/**
	 * What this route is FOR. Everything policy-shaped below is derived from it.
	 *
	 * UNSET IS REFUSED by Find and FindToGoals - see IsQueryAnswerable. The four call sites
	 * that used to say nothing about runway avoidance, and silently got the permissive
	 * answer, are the reason the default cannot be a usable one.
	 */
	UPROPERTY() ERouteErrand Errand = ERouteErrand::Unset;

	/**
	 * The resolved row for Errand, filled by For(). Carried on the query rather than
	 * re-resolved inside the search so that ONE lookup answers the avoidance test, the cost
	 * term and the occupancy check - three readers, one answer, the same rule
	 * FRouteStep::EndVertex follows about the polyline.
	 */
	UPROPERTY() FRoutePolicy Policy;

	/**
	 * Multiplier on a runway edge's length, applied only when Policy.bPenaliseRunways.
	 *
	 * MIRRORS FTrafficRules::RunwayPenalty exactly as CongestionWeight below mirrors
	 * FTrafficRules::CongestionWeight: a query built with no rules to hand must still be
	 * costed the way one built with them is. Airside.Model.RoutePolicy.QueryResolvesTheTable
	 * asserts the two defaults agree, because two constants in two files is how the Piper's
	 * figures ended up different at seven sites.
	 *
	 * NEVER BELOW 1.0. A multiplier under one would make an edge cheaper than its own chord
	 * and break the straight-line heuristic's admissibility silently - the first pop would
	 * stop being optimal and nothing would say so.
	 */
	UPROPERTY() double RunwayPenalty = 10.0;

	UPROPERTY() ETraversalClass Class = ETraversalClass::GroundVehicle;

	/**
	 * Wingspan in uu, or 0 for unconstrained.
	 *
	 * Compared against FGuidelineEdge::MaxWingspan, where 0 means UNLIMITED - so the test
	 * is not a plain >, and getting it backwards routes a widebody down a link that cannot
	 * take it.
	 */
	UPROPERTY() double Wingspan = 0.0;

	/**
	 * An edge the search may not use. Set by a deadlock replan to forbid the edge the agent
	 * was refused. One edge, not a set: the resolver bans exactly the thing it is stuck on
	 * and lets the occupancy cost steer round the rest.
	 */
	UPROPERTY() FGuidelineEdgeId BannedEdge;

	/**
	 * A node the search may not pass through. Set by a deadlock replan when what refused
	 * the agent was a NODE somebody is standing on: banning only the edge it was about to
	 * take lets the search walk round the block and re-enter the same node from its other
	 * arm - measured on 2026-09-06 as an aircraft looping a runway's end taxiway and coming
	 * back to the very bar-holder it was refused by. The node is the wall; the edge is not.
	 */
	UPROPERTY() FGuidelineNodeId BannedNode;

	/**
	 * What to do with edges along a runway - see ERunwayAvoidance. The outright ban (All)
	 * was written for the deadlock replan of 2026-09-06: an agent that had left the runway
	 * routed back ALONG it to get round a queue, and a taxi route on the strip re-reserves
	 * the surface (spec §3.1's first route) and starves the departure waiting at the bar
	 * for exactly that surface. The replan now asks for Held instead, which keeps that case
	 * banned and opens a free runway end as the turnaround the player can see it is.
	 */
	UPROPERTY() ERunwayAvoidance AvoidRunways = ERunwayAvoidance::None;

	/**
	 * Who holds what, for the congestion cost term - or null for a plain shortest route,
	 * which is bitwise the search this class ran before occupancy existed. A raw pointer
	 * rather than a UPROPERTY: a query lives on the stack for one call and the table it
	 * reads outlives it.
	 */
	const FTrafficOccupancy* Occupancy = nullptr;

	/** The agent asking, so its own claims do not cost it. 0 when nobody is. */
	UPROPERTY() int32 QueryingAgent = 0;

	/** Weight on held length. Ignored when Occupancy is null. */
	UPROPERTY() double CongestionWeight = 2.0;

	/**
	 * Start/Goal/Class/Wingspan AND the whole routing policy, in one expression, rather than
	 * default-constructing and setting each by hand - five call sites did (#103). The bans
	 * stay per-caller: a deadlock resolver's banned edge is per-incident, not per-errand, and
	 * a table row for it would be a row of one.
	 *
	 * THE ONLY FACTORY. An errand-less overload existed until 2026-09-21 and is deliberately
	 * gone: it was the shape that let five call sites build a query without ever saying what
	 * it was for, and inherit the permissive policy by omission. Avoidance is no longer in
	 * the "set it after" list for the same reason.
	 */
	static FRouteQuery For(ERouteErrand Errand, FGuidelineNodeId Start, FGuidelineNodeId Goal,
		double Wingspan, ETraversalClass Class);

	/**
	 * The vehicle to gate on: route only where it fits (VehicleFit). Null, the default, gates
	 * nothing - every query made before 2026-09-23, and every aircraft's, is unchanged. A
	 * POINTER, not a copy and not a UPROPERTY: a query lives for one search, and the caller's
	 * vehicle outlives it.
	 */
	const FVehicle* Vehicle = nullptr;

	/**
	 * The tow's LIVE chain, when the vehicle searching is already on the road with its trailer
	 * angled: RouteSearch::Find's whole-route check starts from it instead of a straight lay
	 * (VehicleFit::JudgePlan). Travelled is along the plan this search returns - a rejoin that
	 * starts part-way along its first step sets it. Set by FPlanReResolver::QueryFor; unset,
	 * the default, for a fresh dispatch. A splice's TAIL is not where the vehicle is, so
	 * FPlanReResolver::SpliceReplan clears it for the search and judges the whole splice with it.
	 */
	TOptional<FTowSeed> TowSeed;

	/**
	 * A CALLER-OWNED memo of VehicleFit::Fits per edge, carried across Finds (review of
	 * aa90eec2): a trace round every curve the search relaxes is most of what a vehicle's Find
	 * costs - measured 2026-09-25, ~20 ms a Find for the rig on its course, 92 Finds on one tick.
	 * The CALLER guarantees what the memo is keyed on and cannot see: the same Vehicle and the
	 * same graph (URoadNetwork::GetGuidelineRevision). Null, the default: Find memoises within
	 * itself only. Edge handles carry a generation, so a re-derived edge is a new key even so.
	 * ENFORCED BY: AirportMgr.RigCourse.FitCacheDropsOnRebuild (the one owner, the rig course)
	 */
	TMap<FGuidelineEdgeId, bool>* FitCache = nullptr;

	FRouteQuery& WithVehicle(const FVehicle& InVehicle)
	{
		Vehicle = &InVehicle;
		return *this;
	}

	/** Chainable: the congestion cost term, set together because CongestionWeight is
	 *  meaningless without Occupancy and QueryingAgent is meaningless without both. */
	FRouteQuery& WithCongestion(const FTrafficOccupancy& InOccupancy, int32 InQueryingAgent, double InCongestionWeight)
	{
		Occupancy = &InOccupancy;
		QueryingAgent = InQueryingAgent;
		CongestionWeight = InCongestionWeight;
		return *this;
	}
};

/**
 * A uniform grid over a network's LIVE guideline nodes, built once and queried many times
 * by FindNearestNode's indexed overload (#172).
 *
 * WHY A GRID AND NOT A KD-TREE OR A SORTED-X ARRAY: every caller that builds one
 * (UGroundTraffic::OnGraphRebuilt) already knows the one radius every query will use -
 * Rules.ResolveRadius, the same figure for the from-node, every remaining step, and the
 * goal - so a grid cell sized to that radius turns "nearest within R" into "look at my own
 * cell and its ring of eight neighbours", no tree balance or rebuild-on-insert to reason
 * about. A sorted-X array with a binary-searched window would need a second pass to bound
 * Y within the window; the grid bounds both axes by construction.
 *
 * BUILT FRESH EVERY REBUILD, deliberately never kept across one: FRoadGuidelineBuilder
 * frees and re-adds every derived node on each rebuild, so an index kept from the last one
 * would be answering with dead slots by the time anything queried it. One O(N) build here
 * replaces up to A*S+A separate O(N) scans in the rebuild that follows - see OnGraphRebuilt.
 */
struct AIRSIDE_API FGuidelineNodeIndex
{
	/** Indexes every live node in Network into cells of CellSizeIn on a side. */
	FGuidelineNodeIndex(const URoadNetwork& Network, double CellSizeIn);

	/**
	 * The grid cell containing Position, keyed the same way cells are stored below.
	 * FLOOR, not truncation - FMath::FloorToInt32 rounds -0.5 to -1 rather than to 0, which
	 * truncation would, and a node just west of the origin belongs in cell -1, not cell 0.
	 */
	FIntPoint CellOf(const FVector2D& Position) const
	{
		return FIntPoint(FMath::FloorToInt32(Position.X / CellSize), FMath::FloorToInt32(Position.Y / CellSize));
	}

	/** Never 0 - see the constructor's guard - so CellOf always has something to divide by. */
	double CellSize = 1.0;

	/** Live node indices (into Network.GetGuidelineNodes()), bucketed by CellOf. */
	TMap<FIntPoint, TArray<int32>> Cells;
};

/**
 * One goal's answer from RouteSearch::FindToGoals: reachable, and if so, at what cost.
 *
 * ONE STRUCT, not two parallel TArrays a caller could index out of step with each other or
 * with the Goals array they were computed from - see CLAUDE.md's "one struct per thing".
 */
struct AIRSIDE_API FGoalReach
{
	bool bReachable = false;
	double Length = 0.0;
};

/**
 * The state one RouteSearch::FindToGoals call leaves behind, so a caller that has already
 * picked its winner out of the FGoalReach array can get that ONE goal's full FRoutePlan
 * without a second Dijkstra - see FindToGoals' own comment for why this exists (#190,
 * deferred from #171/#201).
 *
 * A PLAIN STRUCT, not a class hiding these behind an interface, matching FGuidelineNodeIndex
 * above: Arrived and Best ARE the state, and a caller that reads them directly is not
 * depending on anything that can drift out from under it.
 */
struct AIRSIDE_API FMultiGoalSearch
{
	/** Every node the search settled and the step that reached it - bitwise the same map
	 *  RunSearch's own Arrived is, built by the same expansion. */
	TMap<FGuidelineNodeId, FRouteStep> Arrived;

	/** Shortest cost to every settled node, keyed the same way. */
	TMap<FGuidelineNodeId, double> Best;

	FGuidelineNodeId Start;

	/**
	 * Backtraces Arrived from Goal to Start and welds the polyline exactly as RunSearch's own
	 * tail does - see BuildPlanFromArrival, which both now share. An Unreachable plan (never
	 * Found) if Goal was never settled by the search this came from.
	 */
	FRoutePlan BuildPlan(const URoadNetwork& Network, FGuidelineNodeId Goal) const;
};

/**
 * Shortest route over the guideline graph, by A*.
 *
 * NOT in Solve/ and not behind a graph adapter, unlike the junction solver. That solver is
 * pure because its maths stands alone and is reusable; this is a query over URoadNetwork's
 * own handles, incidence and traffic masks, with no second consumer in sight. An adapter
 * interface here would exist to satisfy a rule rather than a need, and the model is
 * already testable without a world - every model test simply NewObjects a network.
 * A NAMED deviation, on those grounds.
 *
 * The heuristic is straight-line distance to the goal, which is admissible because an
 * edge's cost is its sampled polyline length and a polyline is never shorter than the
 * chord between its ends. So the first time the goal is popped, it is optimal.
 */
namespace RouteSearch
{
	AIRSIDE_API FRoutePlan Find(const URoadNetwork& Network, const FRouteQuery& Query);

	/**
	 * ONE Dijkstra out of Query.Start that settles the shortest cost to EVERY node in Goals,
	 * in place of Query.Goal - the primitive ArrivalPlanner::ChooseStand needs (#190, deferred
	 * from #171/#201): its old per-stand loop ran one Find() per candidate, O(exits x stands)
	 * searches per dispatch, per re-offer, per plan re-resolve, even once #201 made each one
	 * cheap. This turns that count into O(exits).
	 *
	 * ZERO HEURISTIC, NOT Find()'s single-goal one: min(straight-line distance to any goal)
	 * would still be admissible for every goal at once (each edge already costs at least its
	 * own chord, the reason Find()'s heuristic is admissible at all), but the traversal-order
	 * saving it buys is moot here - the caller wants EVERY goal's true cost, including held
	 * ones a plain "first goal popped" search would never visit (see ChooseStand's own
	 * comment on bSawHeld). A zero heuristic is exactly that full settle with one fewer
	 * moving part, and correctness - not traversal order - is what a refactor with no
	 * behaviour change is measured on.
	 *
	 * Stops once every live, distinct goal (Start itself and unset/dead handles excluded, the
	 * same SameNode/NoGoal exclusion Find() applies per-goal before it ever searches) has been
	 * settled, or the open list empties - whichever comes first - so an unreachable goal costs
	 * exactly the graph it actually touches, not a search to exhaustion.
	 *
	 * OutReach is written for every entry of Goals, order preserved, so a caller doing its own
	 * tie-break (ChooseStand's "first minimum in enumeration order") reads OutReach[Index]
	 * against Goals[Index] rather than re-deriving which is which.
	 *
	 * The wingspan retry Find() pays for on failure (ERouteResult::TooWide) is deliberately
	 * NOT reproduced: ChooseStand never reads Result, only IsValid()/Polyline/Steps, so that
	 * second, unconstrained search would buy it nothing - see ArrivalPlanner.cpp's own comment
	 * at the call site.
	 */
	AIRSIDE_API FMultiGoalSearch FindToGoals(const URoadNetwork& Network, const FRouteQuery& Query,
		const TArray<FGuidelineNodeId>& Goals, TArray<FGoalReach>& OutReach);

	/**
	 * Head's first KeepSteps steps followed by all of Tail, welded at the node Tail starts
	 * from. Precondition: Tail.Start is the node Head's KeepSteps-th step arrives at (or
	 * Head.Start when KeepSteps is 0). EndDistance and EndVertex are re-based so the
	 * follower and the arbiter keep reading one set of numbers. Returns an invalid plan
	 * when the precondition fails, because splicing two lines that do not meet would put a
	 * jump in the polyline the agent would then drive across.
	 */
	AIRSIDE_API FRoutePlan Splice(const FRoutePlan& Head, int32 KeepSteps, const FRoutePlan& Tail);

	/**
	 * How many full graph searches - RunSearch (Find's constrained pass or its unconstrained
	 * retry) or FindToGoals - have run since the last reset. ArrivalPlanner::ChooseStand's own
	 * measurement (#190, deferred from #171/#201): its old per-stand loop ran one of these per
	 * CANDIDATE STAND; the multi-goal search runs one per EXIT regardless of stand count. See
	 * Airside.Model.ArrivalPlanner.ChooseStandSearchCount. A free variable behind namespace
	 * functions, not a member, for the same reason NodeVisitCountForTest just below is: there
	 * is no instance to count on.
	 */
	AIRSIDE_API int32 SearchCallCountForTest();

	/** Zeroes the counter above - see ResetNodeVisitCountForTest's own reason for existing. */
	AIRSIDE_API void ResetSearchCallCountForTest();

	/**
	 * How many whole-route tow checks (VehicleFit::JudgePlan) Find has run since the last reset:
	 * the measurement that a rigid vehicle or an aircraft is never judged this way (its routing
	 * bit-identical to before the check existed), and that a tow's retries are bounded.
	 */
	AIRSIDE_API int32 TowCheckCountForTest();

	/** Zeroes the counter above, and the seconds below. */
	AIRSIDE_API void ResetTowCheckCountForTest();

	/** Wall-clock seconds those checks took since the reset: the check's cost, per course loop. */
	AIRSIDE_API double TowCheckSecondsForTest();

	/**
	 * The nearest guideline node to a world position that this class could actually use.
	 *
	 * Nodes with no edge admitting the class are skipped rather than returned and refused
	 * later: an aircraft-only node is not a sensible start for a van, and offering it as a
	 * snap only to fail the search afterwards reads as a broken pathfinder rather than as
	 * a node the van was never entitled to.
	 *
	 * Returns an unset handle when nothing qualifies within MaxDistance.
	 *
	 * Index IS OPTIONAL AND DEFAULTS TO NONE (#172): a caller with no FGuidelineNodeIndex
	 * to hand - HoldingPointTool's single click, or a test that asks once - gets exactly the
	 * O(N) scan this function has always run, and pays for a grid it would use once. A
	 * caller that will ask many times in one rebuild (UGroundTraffic::OnGraphRebuilt, via
	 * FPlanReResolver::ReResolvePlan) builds one FGuidelineNodeIndex and passes it every
	 * time, turning every one of those calls into a look at a handful of nearby cells
	 * instead of the whole graph. THE ANSWER IS IDENTICAL EITHER WAY, tie for tie - both
	 * paths run the same per-node distance-then-usability test in ascending node-index
	 * order, so a query that hands in an index gets the same node an unindexed scan would
	 * have, never an approximation. See Airside.Model.RouteSearch.IndexMatchesLinear.
	 */
	AIRSIDE_API FGuidelineNodeId FindNearestNode(
		const URoadNetwork& Network, const FVector2D& Position,
		ETraversalClass Class, double MaxDistance, const FGuidelineNodeIndex* Index = nullptr);

	/**
	 * How many guideline nodes FindNearestNode has actually examined - alive or not, every
	 * one it looked at rather than only the ones it kept - across every call since the last
	 * reset. The #172 measurement that an indexed rebuild visits a small fraction of what a
	 * linear one would, not merely that it returns the same answer. A free-standing counter, not a
	 * member: FindNearestNode is a namespace function with no instance to count on, the
	 * same reason FRoadNetworkSolver::NodeClaimsCallCountForTest is static there too.
	 */
	AIRSIDE_API int32 NodeVisitCountForTest();

	/** Zeroes the counter above, so an earlier test's or an earlier rebuild's visits are
	 *  never mistaken for the ones a test is about to measure. */
	AIRSIDE_API void ResetNodeVisitCountForTest();

	/**
	 * How many times a runway SEED has been resolved through URoadNetwork::IsRunwaySegment
	 * inside a search, since the last reset - the measurement that ExpandNode's per-search
	 * memo is actually consulted, rather than that it merely exists.
	 *
	 * A SEED, NOT AN EDGE: a long strip carries many guideline edges and one segment, and the
	 * whole point of the memo is that the second edge along it costs nothing. A count that
	 * rose with edges would be green on a memo that had been deleted. See
	 * Airside.Model.RouteSearch.RunwaySeedMemo.
	 */
	AIRSIDE_API int32 RunwaySeedResolveCountForTest();

	/** Zeroes the counter above, so an earlier search's resolves are never mistaken for the
	 *  ones a test is about to measure. */
	AIRSIDE_API void ResetRunwaySeedResolveCountForTest();
}
