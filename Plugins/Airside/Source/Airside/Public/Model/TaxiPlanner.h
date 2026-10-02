#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/TaxiReservations.h"

class URoadNetwork;
struct FAirframe;
struct FTrafficRules;

/**
 * How a taxi plan request was answered. Three outcomes, because each has a different fix: no
 * route at all is the LAYOUT's problem (RouteResult says which - connect, one-way, too wide), no
 * free window is TRAFFIC's (wait for a release and ask again, spec §2's event-driven retry).
 */
enum class ETaxiPlanResult : uint8
{
	Planned,

	/** RouteSearch's rules admit no route at all, whatever the table says. See FTaxiPlan::RouteResult. */
	NoRoute,

	/** A route exists, but no timing of it fits the table and ends where the aircraft can stay. */
	NoFreeWindow,
};

/** What to plan: one aircraft, from where it stands to where it will stay. */
struct FTaxiRequest
{
	FGuidelineNodeId Start;
	FGuidelineNodeId Goal;

	/**
	 * What the taxi is FOR - ArrivalTaxiIn or DepartureToEntry in production. It picks the routing
	 * policy (runway avoidance) through FRouteQuery::For exactly as a RouteSearch::Find would, and
	 * an Unset errand is refused by RouteSearch's own IsQueryAnswerable.
	 */
	ERouteErrand Errand = ERouteErrand::Unset;

	/** The aircraft's agent id: the windows it books, and the windows of its own the planner ignores. */
	int32 Holder = 0;

	/** The earliest it can leave Start, on the table's clock. It is at rest on Start until then. */
	double DepartAt = 0.0;
};

/** One planned edge: when the aircraft leaves its start node and reaches To. Legs[i] times Route.Steps[i]. */
struct FTaxiLeg
{
	FGuidelineEdgeId Edge;
	FGuidelineNodeId To;
	double Leave = 0.0;
	double Reach = 0.0;
};

/**
 * A planned wait, at rest on At, from arriving (From) to leaving (To). The stop and the restart
 * are inside it: From is when the aircraft would have passed At rolling.
 */
struct FTaxiHold
{
	FGuidelineNodeId At;
	double From = 0.0;
	double To = 0.0;
};

/**
 * How long one edge takes in each of the four ways it can be driven, from FSpeedProfile pieces
 * (FSpeedProfile::BuildPiece). Roll = at the piece's own speed limit, Rest = stopped.
 * bUsable false when the edge cannot be sampled or the chassis has no ground figures.
 */
struct FTaxiEdgeSeconds
{
	double RollRoll = 0.0;
	double RestRoll = 0.0;
	double RollRest = 0.0;
	double RestRest = 0.0;
	bool bUsable = false;
};

/** The answer to one FTaxiRequest. */
struct AIRSIDE_API FTaxiPlan
{
	ETaxiPlanResult Result = ETaxiPlanResult::NoRoute;

	/** On NoRoute, RouteSearch::Find's own answer for the same query (TooWide, Unreachable, ...). Found otherwise. */
	ERouteResult RouteResult = ERouteResult::NoStart;

	/** The route, welded by RouteSearch::PlanFromSteps - the same polyline a follower walks. */
	FRoutePlan Route;

	/** One per Route.Steps, same order. */
	TArray<FTaxiLeg> Legs;

	TArray<FTaxiHold> Holds;

	/** Every window the plan needs, margin included, held by the request's Holder - BookPasses-ready. */
	TArray<FTaxiPass> Passes;

	/** When the aircraft is at rest on Goal. */
	double Arrival = 0.0;

	bool IsPlanned() const { return Result == ETaxiPlanResult::Planned; }
};

/**
 * Space-time taxi planning: SIPP (safe-interval path planning, Phillips & Likhachev 2011) over
 * the guideline graph - the earliest arrival at Goal through FTaxiReservations' free intervals
 * (spec 2026-10-02 §1). Read-only on the table; booking the answer is the caller's.
 *
 * NO COPY OF ANY RULE IT DEPENDS ON:
 *  - which edges may be taken is RouteSearch's, through FRouteEdgeFilter (one-way, traffic
 *    class, wingspan, pavement, runway avoidance);
 *  - how long an edge takes is FSpeedProfile's, built per edge (BuildPiece) and timed by
 *    SecondsToDrive - the drivability authority, bends and stops included;
 *  - what a junction box is is the claim pass's, FTrafficRules::IsBox;
 *  - the route it returns is welded by RouteSearch::PlanFromSteps.
 * The search itself is the only new logic.
 *
 * WAITS ONLY WHERE THE AIRCRAFT CAN STAND WITHOUT BLOCKING A JUNCTION: the start, and CanHoldAt.
 * A plan never ends anywhere the aircraft cannot stay: the goal must be free for ever after it
 * arrives, which is what makes enforcing only ORDER deadlock-free (spec §2).
 *
 * ONE PLANNER PER BURST OF REQUESTS. Edge timings are memoised per edge handle for the planner's
 * lifetime, and a guideline edge reshaped in place keeps its handle - so a planner kept across a
 * graph rebuild would time the old curve. Construct one per clearance pass.
 */
class AIRSIDE_API FTaxiPlanner
{
public:
	FTaxiPlanner(const URoadNetwork& InNetwork, const FTaxiReservations& InTable, const FAirframe& InAirframe,
		const FTrafficRules& InRules);

	/** Earliest-arrival plan for Request against the table, or why not. */
	FTaxiPlan Plan(const FTaxiRequest& Request);

	/**
	 * How long Route takes on an empty airport by this planner's clock: from rest on its first
	 * node, rolling through every node between, to rest on its last. The figure an unobstructed
	 * plan's Arrival - DepartAt equals.
	 */
	double RouteSeconds(const FRoutePlan& Route);

	/** Edge's four timings, walked A to B, or B to A when bReversed. Memoised. */
	const FTaxiEdgeSeconds& SecondsFor(FGuidelineEdgeId Edge, bool bReversed);

	/**
	 * Whether an aircraft that reached At along Arrived may stop and wait there: Arrived is not a
	 * junction turn path and not a box (FTrafficRules::IsBox - its body fits on it, clear of the
	 * node behind), and At is not a road-taxiway crossing's conflict node. The start of a plan is
	 * always a hold and is not asked.
	 */
	static bool CanHoldAt(const URoadNetwork& Network, const FTrafficRules& Rules, FGuidelineEdgeId Arrived,
		FGuidelineNodeId At);

private:
	const URoadNetwork& Network;
	const FTaxiReservations& Table;
	const FAirframe& Airframe;
	const FTrafficRules& Rules;

	TMap<TPair<FGuidelineEdgeId, bool>, FTaxiEdgeSeconds> EdgeSeconds;
};
