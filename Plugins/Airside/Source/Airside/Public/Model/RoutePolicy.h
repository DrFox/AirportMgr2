#pragma once

#include "CoreMinimal.h"
#include "RoutePolicy.generated.h"

/**
 * What a route may do with edges that lie ALONG a runway (DerivedFrom a runway segment).
 * Crossing a runway at a junction is never affected: a crossing is a turn path and a node,
 * and turn paths carry no DerivedFrom. AN ENUM, NOT TWO BOOLS - "avoid all" and "avoid held"
 * can never both be wanted, and a pair of flags would have let a caller set both.
 *
 * MOVED HERE FROM RouteSearch.h, 2026-09-21. It is the vocabulary a policy is written in, and
 * FRoutePolicy below holds one by value - a USTRUCT member UHT cannot see through a forward
 * declaration. The dependency runs policy-then-search, which is the direction it wanted
 * anyway: the search consumes this vocabulary, nothing here needs the search.
 */
UENUM()
enum class ERunwayAvoidance : uint8
{
	/** Runway edges are ordinary line. A departure backtracking to a threshold needs this. */
	None,
	/**
	 * Skip a runway edge while the strip is IN USE: somebody else holds any segment of its
	 * chain, reserved or occupied, OR the querier itself is standing on it (its own claim
	 * is occupied). Set by a deadlock replan (2026-09-07): an agent turning round via a
	 * free runway end is what the player expects to see; one taxiing along a strip a
	 * landing has been cleared onto is the starvation the outright ban was written
	 * against. THE QUERIER'S OWN BODY COUNTS, and its own reservation does not: the head-on
	 * of 2026-09-06 was an arrival still standing on the strip with a departure REFUSED the
	 * bar for it - the waiter holds nothing, so the table shows the runway held by the
	 * querier alone, and a replan that kept it on the strip would be the very jam it was
	 * asked to leave (Traffic.HeadOnReplansRoundBarHolder measures exactly this). Needs
	 * FRouteQuery::Occupancy; with no table every runway is free. Properly a runway is
	 * used on a CLEARANCE, which is URunwaySequencer's (M3); until then the table is the
	 * truth about who is on the strip.
	 */
	Held,
	/**
	 * Skip every runway edge. An arrival's taxi-in and a departure's taxi to an
	 * intersection entry: neither may taxi along a strip whatever the table says.
	 */
	All,
};

/**
 * What a route is FOR. The single axis every routing policy is derived from.
 *
 * NOT WHO IS DRIVING, and the distinction is the whole reason this enum is allowed to exist
 * beside ETraversalClass, whose own comment forbids the neighbouring design: "what they do
 * is a service role... deliberately not this enum - otherwise this grows with every vehicle
 * type in the game and gets consulted by pathfinding for no reason". That warning is about
 * the ACTOR. This names the ERRAND, which is the one thing pathfinding genuinely must
 * consult, because it is the question the policy answers.
 *
 * THE TEST THAT KEEPS THEM APART: adding a vehicle type must not add an errand. A catering
 * truck and a fuel truck are both VehicleToJob.
 */
UENUM()
enum class ERouteErrand : uint8
{
	/**
	 * Never valid. RouteSearch::Find and FindToGoals refuse it and log at Error.
	 *
	 * ZERO, AND THE DEFAULT, deliberately: the bug this whole design exists to delete was
	 * four call sites that never mentioned runway avoidance and silently got the permissive
	 * answer. A default that cannot be searched with turns forgetting from a silent taxi
	 * down a runway into a line in the log at the first repro.
	 */
	Unset = 0,

	/** An arrival from a runway exit to a stand. Never touches a strip. */
	ArrivalTaxiIn,

	/** A departure from a stand to its runway entry. Never touches a strip. */
	DepartureToEntry,

	/**
	 * A departure backtracking up the strip to a threshold. THE ONE ERRAND THAT MUST USE A
	 * RUNWAY, and the reason the ban could never simply be made unconditional.
	 */
	DepartureBacktrack,

	/** Pushback's clearance route, off the stand and clear of the departure's own arm. */
	PushbackClear,

	/** The taxi from where a push ends to the runway entry. */
	PushbackTaxiOut,

	/**
	 * A deadlock or failed-edge replan for an agent already under way. May use a runway end
	 * that is FREE - an agent turning round via a free runway end is what the player expects
	 * to see, and the outright ban that preceded Held sent one round the whole taxiway loop
	 * past an end it could have used.
	 */
	Replan,

	/** A graph rebuild re-resolving a surviving plan onto new handles. */
	RebuildReResolve,

	/** A vehicle driving to or from a job. Re-chosen on dispatch, so it takes the table. */
	VehicleToJob,

	/**
	 * Comparing candidates - which depot, which stand. Shape only.
	 *
	 * A congestion-weighted comparison makes the WINNER flicker between ticks without ever
	 * changing what gets driven, which is what FuelService.cpp's own comment refused.
	 */
	CandidateComparison,

	/** A route the player or a tool asked for directly. */
	PlayerIssued,

	/**
	 * No policy: plain shortest path, runways free, no congestion. TESTS AND TOOLS ONLY.
	 *
	 * It exists so a graph-shape test ("is B reachable from A") need not pick a production
	 * errand whose policy it does not care about and would silently inherit. Without it,
	 * forty existing tests would each be asserting something about routing policy by
	 * accident.
	 *
	 * Check-Architecture.ps1 rule 11 fails the build if this name appears outside a test
	 * module or a Tool/ directory: the permissive default survives only where it is NAMED.
	 */
	GraphProbe,
};

/**
 * Whether an errand's cost reads the occupancy table.
 *
 * NOT "vehicles yes, aircraft no", which is what the ground-traffic spec's section 4 says and
 * has not been true since FuelService landed. The axis that actually survives all ten call
 * sites is whether THE ROUTE MAY STILL CHANGE: a route fixed when it is issued costs shape
 * only, a route being re-chosen for an agent already under way costs held length too.
 */
UENUM()
enum class EOccupancyUse : uint8
{
	/**
	 * Cost is shape only. A clearance, or a comparison that must not flicker.
	 *
	 * The search REFUSES a query that supplied a table anyway - see RouteSearch::Find. A
	 * caller that went to the trouble of passing occupancy believes it is being weighted by
	 * it, and silently ignoring the pointer leaves that caller reasoning about a cost term
	 * the search never applied.
	 */
	Never,

	/** Cost includes held length. The search REFUSES if no table was supplied. */
	Required,
};

/**
 * One errand's routing policy. THE one table - see FRoutePolicy::For.
 *
 * A NAMED PUBLIC TYPE rather than a constexpr array hidden in RouteSearch.cpp, so the table
 * can be swept by a test (Airside.Model.RoutePolicy.EveryErrandHasARow) and pinned against
 * what the call sites shipped (MatchesCallSitesAsShipped). A table nothing can enumerate is
 * a fourth outing for the bug in CLAUDE.md's "Check where a list is CONSUMED".
 */
USTRUCT()
struct AIRSIDE_API FRoutePolicy
{
	GENERATED_BODY()

	/** Which runway-derived edges the search may not use at all. */
	UPROPERTY() ERunwayAvoidance Avoidance = ERunwayAvoidance::All;

	/** Whether the cost reads the occupancy table, checked BOTH WAYS by the search. */
	UPROPERTY() EOccupancyUse Occupancy = EOccupancyUse::Never;

	/**
	 * Whether a runway edge this policy still ALLOWS costs a multiple of its length.
	 *
	 * A FLAG, NOT THE NUMBER. The magnitude is FTrafficRules::RunwayPenalty, copied onto
	 * FRouteQuery::RunwayPenalty, so it reaches the Details panel and can be tuned on a
	 * placed actor without a rebuild. The policy says WHETHER, the rules say HOW MUCH.
	 *
	 * Meaningless - and asserted false by the table test - when Avoidance is All: the edge
	 * is gone before anything asks its cost.
	 */
	UPROPERTY() bool bPenaliseRunways = false;

	/**
	 * The table. Every errand has exactly one row.
	 *
	 * DEFAULTS ARE THE RESTRICTIVE ONES on this struct, so an errand added without a row
	 * here gets All/Never rather than the permissive answer - and the table test catches it
	 * on the next run either way.
	 */
	static FRoutePolicy For(ERouteErrand Errand);
};
