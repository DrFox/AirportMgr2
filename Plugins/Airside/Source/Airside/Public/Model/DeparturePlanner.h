#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"
#include "Model/RouteSearch.h"
#include "Model/RunwayAdmission.h"
#include "DeparturePlanner.generated.h"

class URoadNetwork;

/** Why a departure could not be planned. None means it was. */
UENUM()
enum class EDepartureRefusal : uint8
{
	None,
	/** The point given is not on a runway. */
	NoRunway,
	/** The airframe has no take-off or climb performance to size the roll from. */
	NoPerformance,
	/** No taxi route reaches the runway at all, forward or backtracking. */
	NoRoute,
	/** The runway exists but this aircraft may not use it - FDeparturePlan::Admission says why. */
	NotAdmitted,
	/** DepartAgent only: the agent is not Parked, so there is nothing standing still to send. */
	NotParked,
	/**
	 * DepartAgent only: the ground the push off the stand needs is not free.
	 *
	 * UNLIKE EVERY OTHER REFUSAL HERE, this one is about OTHER TRAFFIC and clears itself - a
	 * caller that gives up on it gives up on an aeroplane that would have left a few seconds
	 * later. See UGroundTraffic::DepartAgent for why a push is granted whole rather than
	 * arbitrated as it goes.
	 */
	PushbackBlocked,
	/**
	 * The stand has no arm to reverse onto, so the aeroplane cannot be pushed off it at all.
	 *
	 * PERMANENT, unlike PushbackBlocked: no amount of waiting adds a second way out of a
	 * dead-end taxiway. Refusing is deliberate - see PushbackPlanner::Plan for why falling
	 * back on reversing down the departure's own arm was rejected.
	 */
	NoPushbackRoute,

	/**
	 * The guideline graph is behind the road it was derived from - a node is mid-drag, and a
	 * drag rebuilds geometry only (URoadSurfacePresenter, #165), deriving the graph on release.
	 * A route searched now would follow lines the player has already moved (2026-09-27: an
	 * SR22 joined the runway where its end used to be). TRANSIENT: it clears on release, and
	 * a retry then plans on the graph the player sees. Appended LAST so saved values keep
	 * their meaning. See URoadNetwork::AreGuidelinesBehindRoad.
	 */
	GraphBeingEdited,
	/** Runways exist, but every one is set to arrivals only (ERunwayUse). The fix is the runway
	 *  card's setting. Appended LAST, GraphBeingEdited's reason. */
	NoDepartureRunway,
};

/**
 * Everything a departure needs, decided before anything moves - the mirror of
 * FArrivalPlan. See DeparturePlanner::Plan.
 */
USTRUCT()
struct AIRSIDE_API FDeparturePlan
{
	GENERATED_BODY()

	UPROPERTY() EDepartureRefusal Why = EDepartureRefusal::NoRunway;

	/** The admission decision for this runway and airframe; Why == NotAdmitted when refused. */
	UPROPERTY() FRunwayAdmission Admission;

	/** The taxi out: from the start to the entry, ending ON the centreline. */
	UPROPERTY() FRoutePlan Route;

	/** The strip node the taxi ends on. */
	UPROPERTY() FGuidelineNodeId Entry;

	/** How far past the threshold the entry sits, uu. The roll starts here. */
	UPROPERTY() double EntryOffset = 0.0;

	/**
	 * True when no forward entry left enough runway and the taxi goes to the threshold
	 * instead, along the strip if it must, to turn round there. False for an intersection
	 * departure: the aircraft arrives aligned and rolls from where it joined.
	 */
	UPROPERTY() bool bBacktrack = false;

	/** The strip departed from, and the threshold the roll starts at - see #88. */
	UPROPERTY() FRunwayEnd End;
	/** Runway beyond the entry, uu: what the roll actually has. */
	UPROPERTY() double Available = 0.0;
	/** Roll to Vr this airframe needs, uu. */
	UPROPERTY() double Needed = 0.0;

	bool IsValid() const { return Why == EDepartureRefusal::None && Route.IsValid(); }
};

/**
 * WHERE A DEPARTURE JOINS THE RUNWAY, and how it gets there - the mirror of ArrivalPlanner.
 *
 * Before this existed a route "to the runway" went to whichever strip node the player
 * clicked, usually the threshold's own, and the search was free to use runway edges on the
 * way. Since the exit arcs that produced samples/runway1.png (2026-09-07): onto the runway
 * by the entry arc heading DOWN the runway, east to the split, hairpin, back west to the
 * threshold, and a 180 degree turn on the spot before rolling. Real operations call the
 * alternative an INTERSECTION DEPARTURE: join by the arc, already aligned, and roll from
 * there if the runway remaining is enough.
 *
 * The rule: the FIRST strip node from the threshold that leaves at least the roll needed,
 * that a taxi with the runway's own edges excluded can reach, and that it arrives at
 * heading down the runway. Only when no entry leaves enough runway is the threshold the
 * goal, with runway edges allowed - a genuine backtrack, where the turn at the end is the
 * right behaviour rather than an artefact.
 *
 * Model/: plain functions over URoadNetwork, no world, so it is testable with NewObject and
 * usable by both the Route tool now and M3's sequencer later.
 */
namespace DeparturePlanner
{
	/**
	 * Plan a departure from Start onto the runway under OnRunway (any point on its
	 * pavement). The END departed from is the runway IN USE (FRunwayFacts::InUse, through
	 * RunwayQuery::InUseRunwayAt), whichever end OnRunway is nearer - since 2026-09-28; it
	 * used to be the threshold nearest OnRunway.
	 */
	AIRSIDE_API FDeparturePlan Plan(const URoadNetwork& Network, FGuidelineNodeId Start,
		const FVector2D& OnRunway, const FAirframe& Airframe, ETraversalClass Class);

	/**
	 * Plan a departure from Start onto the best runway that takes departures (ERunwayUse): a
	 * FREE one before a held one (Occupancy, when given), one SET to departures before a mixed
	 * one, then the shortest admitted taxi (2026-09-29 - shortest alone sent every departure to
	 * the runway nearest the apron, samples/2runways.png). Each runway is tried once, from its
	 * end in use - never its other end, however much shorter that taxi would be
	 * (samples/deadlock.png). When none is valid the first refusal is returned, so the log can
	 * say "grass strip, needs tarmac" rather than "no runway". The inspector's Depart button;
	 * M3's sequencer replaces the choice, not the shape.
	 */
	AIRSIDE_API FDeparturePlan PlanAny(const URoadNetwork& Network, FGuidelineNodeId Start,
		const FAirframe& Airframe, ETraversalClass Class, const struct FTrafficOccupancy* Occupancy = nullptr);

	/** One line saying what Plan decided or refused, for a log. */
	AIRSIDE_API FString Describe(const FDeparturePlan& Plan);
}
