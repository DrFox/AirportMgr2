#pragma once

#include "CoreMinimal.h"
#include "Model/RoadAgent.h"
#include "Model/TrafficContext.h"

class URoadNetwork;

/**
 * The replan MECHANISM shared by the deadlock resolver and a graph rebuild - spec §4 and
 * §6. Issue #84 pulled it off UGroundTraffic, which used to own ReplanAt, ReResolvePlan and
 * SpliceReplan directly: "HOW, NEVER WHEN" was already the split UGroundTraffic::ReplanAt's
 * own header drew between the resolver deciding an agent is stuck and this deciding what to
 * do about it, so making it a struct of its own says the same thing in the type system.
 *
 * NO PERSISTENT STATE. Every method takes what it needs (Rules for the congestion weight
 * and resolve radius, Occupancy for the table) and leaves nothing behind between calls, so
 * UGroundTraffic's own instance and one a test constructs fresh behave identically.
 *
 * MOVED OFF Model/GroundTraffic.h (issue #175), with FTrafficRules and FDeadlockResolver -
 * see that header's own comment for the file map. GroundTraffic.h still includes this header,
 * so every existing includer of it keeps compiling unchanged. The bodies stay in
 * GroundTrafficRebuild.cpp, beside UGroundTraffic::OnGraphRebuilt, which is tick-order
 * orchestration around this struct rather than mechanism of its own - see that file's header.
 */
struct AIRSIDE_API FPlanReResolver
{
	/**
	 * Re-routes Agent from SpliceStep onward, forbidding BannedEdge/BannedNode. Implements
	 * the full contract UGroundTraffic::ReplanAt documents - guards, PRECONDITION, the two
	 * rejected alternatives - which stays there as the PUBLIC description callers read;
	 * that function now forwards to this one.
	 *
	 * CONTEXT, NOT Rules AND Occupancy SEPARATELY (issue #175): every caller already holds
	 * both as one bundle - UGroundTraffic's own members, or a test's bare pair - and this
	 * was two of FDeadlockResolver::Resolve's seven positional parameters. See
	 * Model/TrafficContext.h.
	 */
	bool ReplanAt(FRoadAgent& Agent, int32 SpliceStep, FGuidelineEdgeId BannedEdge,
		FGuidelineNodeId BannedNode, const FTrafficContext& Context);

	/** What one re-resolution did. Counted by UGroundTraffic::OnGraphRebuilt for its one
	 *  log line. */
	enum class EReResolve : uint8
	{
		/** Every remaining step found its node and its edge again; only handles changed. */
		Intact,
		/** A step's edge was gone; a fresh route from there to the goal was spliced on. */
		Replanned,
		/** Gone with nothing to replace it: the route now ends at the last live node. */
		Truncated,
		/** Not even the ground under the agent resolved, or truncation left no route at all. */
		Stranded,
	};

	/**
	 * Re-points Plan's steps from FromStep onward at the rebuilt graph. See
	 * UGroundTraffic::OnGraphRebuilt for the outer contract (three outcomes, the fourth
	 * that is not a degree of them, why guideline claims go and surface claims stay).
	 *
	 * FromStep IS THE FIRST STEP THE AGENT HAS NOT FINISHED, and the steps behind it keep
	 * their dead handles - with ONE exception that is not optional. The node the current step
	 * LEAVES FROM is Steps[FromStep-1].To (or Plan.Start at step 0), it is what StepFromNode
	 * answers, and FOUR live readers ask for it every tick:
	 *
	 *   - FClaimPass::Run's crossing arm, which asks whether that node carries a
	 *     HoldingPositionFor bar. A dead handle reads as no bar, so no crossing would ever
	 *     arm again after a rebuild;
	 *   - FClaimPass::Run's tail-node claim, OfNode(From), held while the body is still
	 *     within Footprint/2 of it. On a dead handle that claim protects nothing and the
	 *     junction BEHIND the agent is open for somebody to drive into;
	 *   - FClaimPass::RankAt, which falls back to the class order when the node cannot be
	 *     found - so a node's PriorityOverride would silently stop applying;
	 *   - ReplanAt's Query.Start when the failed step IS the current one, where a dead handle
	 *     gives ERouteResult::NoStart - so that replan could never succeed, and the truncation
	 *     that followed would write the same dead handle into GoalNode.
	 *
	 * So this function re-points that one node as soon as it has resolved it. Steps further
	 * back are genuinely never read again - the follower walks the polyline - and are left
	 * alone, because re-resolving them could only change numbers the agent has already passed.
	 *
	 * Applied to Follower.Plan from CurrentStep for a Taxiing agent, and to TaxiInPlan from
	 * 0 for an Arriving one - the route it will fly when it vacates, which no follower is on
	 * yet and which is just as dead after a rebuild as one being driven.
	 *
	 * WHICH OF THE TWO DECIDES WHAT A FAILURE AT FromStep MEANS. Under a moving follower it
	 * is the ground the agent is on, so it strands in place (spec §6.2, and see the branch
	 * itself for why a replan there teleports). A taxi-in plan is a route nobody has entered
	 * - the aircraft is on the runway - so its first step failing is an ordinary replan, and
	 * only a route that cannot be rebuilt at all strands it.
	 *
	 * MUTATES Agent.GoalNode when the goal position still resolves, because every replan
	 * from here on searches to it: a goal handle left naming a freed slot would fail every
	 * subsequent deadlock replan for the rest of the session, silently.
	 *
	 * NodeIndex IS THE #172 FIX: this calls RouteSearch::FindNearestNode for the from-node,
	 * once per remaining step, and for the goal - A x S searches of a graph that could hold
	 * thousands of nodes, on every committed edit, before this. A reference and not a
	 * pointer: OnGraphRebuilt builds exactly one FGuidelineNodeIndex before its loop starts
	 * and every call this rebuild makes shares it, so there is no caller here that could
	 * have none to pass.
	 *
	 * CONTEXT IN PLACE OF Network, Rules AND Occupancy (issue #175) - the same bundle
	 * ReplanAt above takes, for the same reason. NodeIndex stays a separate parameter: it is
	 * built once per REBUILD, not once per UGroundTraffic, so it does not belong on a struct
	 * that also describes a bare test's hand-built pair.
	 */
	EReResolve ReResolvePlan(FRoadAgent& Agent, FRoutePlan& Plan, int32 FromStep,
		const FTrafficContext& Context, const FGuidelineNodeIndex& NodeIndex);

	/**
	 * Runs Query and splices its answer onto Plan's first KeepSteps steps, IN PLACE.
	 *
	 * True and Plan is the new journey; FALSE AND PLAN IS UNTOUCHED - the search or the
	 * splice failed, and a caller that ignored the return would otherwise be driving
	 * something half-replanned. That all-or-nothing shape is the whole reason this is one
	 * function and not two calls at each site.
	 *
	 * NO AGENT AND NO FOLLOWER. ReplanAt needs the follower handled (Travelled survives, the
	 * reservations drop, the stall clock resets); a TaxiInPlan has no follower on it at all
	 * and must not touch one. What the two share is exactly this - search, splice, keep or
	 * discard - so this is where it lives, and each caller adds its own aftermath.
	 */
	static bool SpliceReplan(const URoadNetwork& Network, const FRouteQuery& Query, int32 KeepSteps,
		FRoutePlan& Plan);
};
