#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"
#include "Model/RouteSearch.h"
#include "Model/RunwayAdmission.h"
#include "ArrivalPlanner.generated.h"

class URoadNetwork;
struct FTrafficOccupancy;

/**
 * Why ArrivalPlanner::Plan could not produce a plan.
 *
 * One enum rather than a bare false, for the reason FRouteQuery's ERouteResult exists: a
 * refusal that does not say which of "no runway", "too short" or "no exit" applied is a
 * feature that "does nothing" - see CLAUDE.md and the log line "pressing 7 does nothing"
 * this project has already shipped once.
 */
UENUM()
enum class EArrivalRefusal : uint8
{
	None,

	/** Near is not on, or near enough to be answered by, any runway. */
	NoRunway,

	/** The runway exists but is shorter than this airframe needs to stop. */
	RunwayTooShort,

	/** The runway can be stopped on, but nothing joins it far enough down to be usable. */
	NoExit,

	/** At least one usable exit, but no route from any of them reaches a stand. */
	NoRouteToStand,

	/**
	 * The runway exists and would do, but someone holds it - a landing rolling out, a
	 * departure lining up, or an aircraft crossing at a holding-position. The one refusal that
	 * clears on its own; M3's sequencer queues on it.
	 */
	RunwayOccupied,

	/**
	 * The runway exists but this aircraft may not use it: surface, approach, published
	 * field length or width - FArrivalPlan::Admission says which. Asked BEFORE occupancy,
	 * because a refusal that never clears must not be reported as one that will.
	 */
	NotAdmitted,

	/** Stands are reachable, but every one of them is held by another aircraft. Distinct from
	 *  NoRouteToStand because the player's fix differs: wait (or build a stand), not a taxiway. */
	NoFreeStand,
};

/**
 * The answer to one arrival query: which runway, which exit, which stand, and why not.
 *
 * Pulled out of ARoadNetworkActor::DispatchArrival (issue #29) because choosing all three
 * is a pure function of the graph and the airframe - nothing in it needs a world - and it
 * was the reason Airside.Present.ArrivalDispatch had to UWorld::CreateWorld just to reach
 * three decisions with no view in them at all.
 */
USTRUCT()
struct AIRSIDE_API FArrivalPlan
{
	GENERATED_BODY()

	/** The runway threshold nearest the query point. */
	UPROPERTY() FVector2D Threshold = FVector2D::ZeroVector;

	/** Unit vector from Threshold toward the far end - the landing heading. */
	UPROPERTY() FVector2D Direction = FVector2D(1.0, 0.0);

	/** Runway available beyond Threshold, uu. */
	UPROPERTY() double RunwayLength = 0.0;

	/** The runway segment nearest the query, and every segment continuous with it. What the
	 *  landing holds in the occupancy table from StartArrival until Vacated. */
	UPROPERTY() FRoadSegmentId RunwaySegment;
	UPROPERTY() TArray<FRoadSegmentId> RunwayChain;

	/** The admission decision for this runway and airframe; Why == NotAdmitted when refused. */
	UPROPERTY() FRunwayAdmission Admission;

	/** Runway needed past Threshold to stop, uu - see FLandingRun::RequiredLandingDistance. */
	UPROPERTY() double Needed = 0.0;

	/** The chosen exit: the earliest one that reaches any stand at all. */
	UPROPERTY() FGuidelineNodeId Exit;

	/** Exit's own position in the ordered exit list, 1-based, for the "vacating at exit N of M" log. */
	UPROPERTY() int32 ExitOrdinal = 0;

	/** How many usable exits the runway offered. */
	UPROPERTY() int32 ExitCount = 0;

	/** Distance along the runway centreline from Threshold at which the aircraft leaves it, uu. */
	UPROPERTY() double VacateAt = 0.0;

	/** Shortest taxi from Exit to a stand. */
	UPROPERTY() FRoutePlan TaxiIn;

	/**
	 * None means every step above succeeded and every other field is meaningful.
	 *
	 * DEFAULTS TO NoRunway, not None - fail closed. A default-constructed plan (one nobody
	 * has run Plan() over yet) must read as refused, never as an arrival some caller could
	 * mistake for valid and act on.
	 */
	UPROPERTY() EArrivalRefusal Why = EArrivalRefusal::NoRunway;

	bool IsValid() const { return Why == EArrivalRefusal::None; }
};

/**
 * Chooses a runway, an exit and a stand for an arrival - the model half of a landing.
 *
 * Free functions over a const URoadNetwork&, matching RouteSearch's shape rather than a
 * class: there is no state to own between calls, only a graph to read and an airframe to
 * read it against.
 */
namespace ArrivalPlanner
{
	/**
	 * The best FREE stand reachable from From: shortest taxi with runway edges excluded, skipping
	 * any stand whose pose node Occupancy says is held by an agent other than ExcludingAgent.
	 * Unset when none. OutRoute receives the winning route; bOutSawHeld reports that at least one
	 * reachable stand was skipped for being held, which is how Plan tells NoFreeStand from
	 * NoRouteToStand. Factored out of Plan so the rebuild can ask it from a node that is not a
	 * runway exit (UGroundTraffic::ReResolvePlan) and the re-offer from wherever a waiter stopped.
	 */
	AIRSIDE_API FGuidelineNodeId ChooseStand(const URoadNetwork& Network, FGuidelineNodeId From,
		const FAirframe& Airframe, const FTrafficOccupancy* Occupancy, int32 ExcludingAgent,
		FRoutePlan* OutRoute = nullptr, bool* bOutSawHeld = nullptr);

	/**
	 * Plans an arrival at the runway nearest Near, for an airframe with Airframe's
	 * performance and wingspan.
	 *
	 * NEAREST RUNWAY AND SHORTEST TAXI FROM THE EARLIEST USABLE EXIT are the user's own
	 * rules, carried over unchanged from DispatchArrival: there is no wind model to choose
	 * a runway by, and an aircraft takes the earliest turn-off it can rather than rolling to
	 * the end in search of a marginally shorter taxi.
	 *
	 * Occupancy, when given, refuses RunwayOccupied while any segment of the chain is held.
	 * Null is the pre-traffic answer, which is what a tool that only asks "could this land
	 * here" still wants.
	 */
	AIRSIDE_API FArrivalPlan Plan(const URoadNetwork& Network, const FVector2D& Near,
		const FAirframe& Airframe, const FTrafficOccupancy* Occupancy = nullptr);

	/**
	 * The user-facing sentence for a refused plan - the same wording DispatchArrival used to
	 * log inline, now read off the plan instead of re-derived from it, so the actor logs
	 * from the SAME decision it acted on rather than a second opinion about why.
	 */
	AIRSIDE_API FString DescribeRefusal(const FArrivalPlan& Plan);
}
