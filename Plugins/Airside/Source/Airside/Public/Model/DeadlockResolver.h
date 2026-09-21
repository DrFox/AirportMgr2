#pragma once

#include "CoreMinimal.h"
#include "Model/PlanReResolver.h"
#include "Model/RoadAgent.h"
#include "Model/TrafficContext.h"

class URoadNetwork;

/**
 * The wait-for graph, its cycles, and one replan per cycle per retry window - spec §5.
 * Issue #84 pulled this off UGroundTraffic, whose nine ForTest doors existed because these
 * six fields - CyclesSeen through DeadlockLogLines, all TEST-FACING bookkeeping, see their
 * own comments - lived as private members of the god class. They are public fields of a
 * plain struct now: still forwarded from UGroundTraffic under their old ForTest names (so
 * nothing that already reads them has to change), but also reachable directly by a test
 * that constructs an FDeadlockResolver of its own and drives it with no UGroundTraffic at all.
 *
 * MOVED OFF Model/GroundTraffic.h (issue #175), with FTrafficRules and FPlanReResolver - see
 * that header's own comment for the file map. GroundTraffic.h still includes this header, so
 * every existing includer of it keeps compiling unchanged. The body stays in
 * GroundTrafficDeadlock.cpp.
 */
struct AIRSIDE_API FDeadlockResolver
{
	/**
	 * The wait-for graph, its cycles, and one replan per cycle per retry window. Spec §5.
	 *
	 * AT THE END OF THE TICK, after every agent has claimed and moved, so the WaitingOn edges
	 * it reads are this frame's and not a mixture of two. Every agent stalled longer than
	 * Rules.StallSeconds contributes ONE edge - id -> WaitingOn - which makes the graph a
	 * functional one (out-degree at most 1), and the walk from any member therefore either
	 * runs out of stalled waiters or closes into exactly one cycle. That is what bounds it:
	 * each step adds an agent not already on the path, and there are finitely many agents.
	 *
	 * A CYCLE IS KEYED BY ITS LOWEST MEMBER ID, so it is handled once however many members
	 * would have found it - that key, and nothing else, is why one jam is one entry in
	 * CyclesSeen. WHAT THE RETRY STAMP DOES IS SEPARATE: re-detecting a cycle inside
	 * Rules.RetrySeconds does nothing at all, so an unresolvable jam is REPORTED on a cadence
	 * rather than on every tick, and the player's later fix (a new edge out of it) is picked
	 * up on the next window.
	 *
	 * NO REVERSING - spec §1. The one move available is a member turning at the node it is
	 * stopped at, so a cycle whose members are all mid-edge is logged and left, which is what
	 * Airside.Model.Traffic.HeadOnStops measures.
	 *
	 * CONTEXT IN PLACE OF Network, Rules, Occupancy AND Reach (issue #175) - four of what
	 * used to be seven positional parameters, all of them UGroundTraffic's own members handed
	 * down in a fixed order a caller had to remember rather than a bundle the type system
	 * names. PlanReResolver stays a separate parameter: it is a MECHANISM this calls into, not
	 * data about the graph, and Model/TrafficContext.h's own comment says why a data bundle
	 * and a behaviour are kept apart rather than merged into one "everything" struct.
	 */
	void Resolve(TArray<FRoadAgent>& Agents, const FTrafficContext& Context, FPlanReResolver& PlanReResolver);

	/**
	 * Every cycle key (the lowest member id) this session has LOGGED. Its only reader is
	 * UGroundTraffic::GetCyclesDetectedForTest.
	 *
	 * NOT reflected: it is test-facing bookkeeping about log lines, not state the simulation
	 * reads - nothing in the tick branches on it, and an agent list that never reaches disk
	 * cannot leave a meaningful key behind for a later session. Reflecting it would say it
	 * mattered to the model, which it does not.
	 */
	TSet<int32> CyclesSeen;

	/** Last agent whose deadlock replan succeeded; 0 until one does. Test-facing, as above. */
	int32 LastResolvedAgent = 0;

	/**
	 * When each cycle key last settled by a YIELD (SimSeconds). Read by Resolve, unlike the
	 * sets above: a cycle that re-forms within Rules.RetrySeconds of yielding is one a yield
	 * did not fix - the other member wanted something a third party holds - and goes to the
	 * replan path instead of yielding for ever. Test-facing for the same reason as CyclesSeen:
	 * agents never reach disk, so a key could not mean anything to a later session.
	 */
	TMap<int32, double> YieldedAt;

	/** Reservation cycles settled by a yield; and who yielded last. Test-facing. */
	int32 Yields = 0;
	int32 LastYieldedAgent = 0;

	/** Deadlock lines emitted, resolved and unresolvable alike. Test-facing, as above. */
	int32 DeadlockLogLines = 0;

private:
	/**
	 * Resolve's own scratch, PROMOTED FROM LOCALS (issue #190): Waiting and Visited used to
	 * be declared fresh at the top of every Resolve() call, and Path/Position fresh at the
	 * top of every not-yet-visited waiter's walk within it - four heap allocations (Waiting,
	 * Visited always; Path/Position once per waiter) every substep ANY agent is stalled,
	 * which for a busy junction with a jam in it is every substep for as long as the jam
	 * lasts. Reset at the top of the scope that used to declare them; kept as members purely
	 * so their capacity survives from one Resolve() call - or one waiter's walk - to the next.
	 */
	TMap<int32, int32> Waiting;

	/** See Waiting above. Reset once per Resolve() call, at the same point Waiting is filled. */
	TSet<int32> Visited;

	/** See Waiting above. Reset once per WAITER walked, not once per Resolve() call - a
	 *  cycle's path is only meaningful within the one walk that built it. */
	TArray<int32> Path;

	/** See Path above - the same walk's membership test and the cycle's start index in one,
	 *  reset alongside it. */
	TMap<int32, int32> Position;

	/**
	 * Can this member of a cycle turn where it stands? Spec §5's refined resolver rule.
	 *
	 * True only for a Taxiing agent that is STOPPED, was refused something (BlockedStep), and
	 * is AT the node that step leaves from - within Gap + Footprint/2 short of it and not
	 * past it. The alternative to a banned edge is another edge OUT of that node, so an agent
	 * that has already entered the edge cannot take it without reversing, and one still a
	 * whole edge short of the node would be replanned from a node it is nowhere near.
	 */
	bool CanReplanAtBlockedStep(const FRoadAgent& Agent, const URoadNetwork* Network,
		const FTrafficRules& Rules, FNodeReachCache& Reach) const;
};
