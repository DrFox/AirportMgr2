#pragma once

#include "CoreMinimal.h"
#include "Model/DeparturePlanner.h"
#include "Model/RoadHandles.h"
#include "Model/RouteSearch.h"
#include "UObject/Object.h"

class USimClock;
#include "FuelService.generated.h"

class UGroundTraffic;
class URoadNetwork;
enum class EAgentPhase : uint8;

/**
 * How far through being fuelled one parked aircraft is.
 *
 * AN ENUM, NEVER A SET OF BOOLS. "A truck is out" and "the dwell is running" can never both
 * be the current state, and the states are visited in one order, so the illegal combinations
 * stop being representable - the same rule EAgentPhase and ECrossingPhase follow.
 */
UENUM()
enum class EFuelDemandState : uint8
{
	/** Parked and asking. Every tick tries to find it a truck. */
	Needed,

	/** A truck is driving to the stand's hydrant. TruckId and Depot name which. */
	TruckEnRoute,

	/** The truck is at the hydrant, running down DwellEndsAt. */
	Fuelling,

	/**
	 * Fuelled. The truck is on its way home; the demand stays in this state so the
	 * aircraft's card can say so and so a second truck is never sent for it.
	 */
	Done,

	/**
	 * Nothing can serve it, and Why says what is missing.
	 *
	 * TERMINAL FOR THIS GRAPH: a re-offer needs the airport itself to have CHANGED, which is
	 * what URoadNetwork::GetGuidelineRevision reports. Without that test a demand refused
	 * once would be retried every tick for ever, logging as it went.
	 */
	Unserviceable
};

/**
 * WHY nothing can serve a demand - one cause, named for the thing the player would go and
 * fix.
 *
 * Not a bare "no route", which is the least useful thing a system can say to somebody
 * building an airport - the same argument ERouteResult's own header makes. "No fuel depot"
 * is a building to place and "depot not on a road" is a road to draw, and the two have
 * nothing in common but the symptom.
 */
UENUM()
enum class EFuelRefusal : uint8
{
	None,

	/** No live fuel depot anywhere on the airport. */
	NoDepot,

	/** Depots exist; not one of them has a road within its lead-in reach. */
	NoRoad,

	/** The stand's own fuel anchor joins nothing - no road within reach of the hydrant. */
	StandUnjoined,

	/** Everything is joined and the graph still does not connect the two. */
	NoRoute
};

/** One parked aircraft's fuel job. */
USTRUCT()
struct AIRPORTOPS_API FFuelDemand
{
	GENERATED_BODY()

	/** The parked agent. Ids start at 1, so 0 means "no aircraft". */
	UPROPERTY() int32 AircraftId = 0;

	/** Where it parked, so the hydrant can be found again on any later tick. */
	UPROPERTY() FEntityInstanceId Stand;

	UPROPERTY() EFuelDemandState State = EFuelDemandState::Needed;

	/** The truck out for it, or 0. Cleared the moment the truck is sent home - a truck on
	 *  its way back belongs to GoingHome, not to a demand. */
	UPROPERTY() int32 TruckId = 0;

	/** Whose truck, so the count is freed against the right depot. */
	UPROPERTY() FEntityInstanceId Depot;

	/** UGroundTraffic::GetSimSeconds at which the dwell ends. See UFuelService's header for
	 *  why that clock and not USimClock. */
	UPROPERTY() double DwellEndsAt = 0.0;

	UPROPERTY() EFuelRefusal Why = EFuelRefusal::None;

	/**
	 * USimClock::Now at which this aircraft is free to push back. Set when it parks.
	 *
	 * THE GAME CLOCK, not the DwellEndsAt one two fields up, and the pair is the clearest
	 * statement of why this class has to see both - see FAirframe::TurnaroundSeconds. A dwell
	 * is seconds and is watched; a turnaround is tens of minutes and is compressed.
	 *
	 * ALSO THE GRACE PERIOD when nothing can serve the aircraft. One deadline, not two: an
	 * aircraft whose fuel went Unserviceable waits exactly as long as one being fuelled, and
	 * then leaves without it.
	 */
	UPROPERTY() double TurnaroundEndsAt = 0.0;

	/**
	 * The last reason a departure was refused, so the retry does not log every tick.
	 *
	 * A departure can be refused for as long as the player leaves a runway occupied, and this
	 * runs every tick - see the busy-depot branch in Tick for the same problem solved the same
	 * way. None means nothing has been refused yet.
	 */
	UPROPERTY() EDepartureRefusal LastDepartureRefusal = EDepartureRefusal::None;
};

/**
 * Every aircraft that parks demands fuel; this finds it a truck, waits out the dwell, and
 * sends the truck home. Spec 2026-09-07-fuel-service-slice §6.
 *
 * IN AirportOps AND NOT IN Airside, deliberately. Airside knows how a thing MOVES and must
 * never learn what it is FOR - so the road, the depot's pose, the vehicle's performance and
 * the truck's view all live over there, and demand, dwell and job state live here. That
 * boundary is what Check-Architecture.ps1 enforces in one direction; this class is the other
 * side of it.
 *
 * IN Model/ AND NOT Present/, which is what makes it testable with no world: it takes the
 * UGroundTraffic and the URoadNetwork PER CALL and holds neither, so it cannot outlive a
 * graph and a test drives it with NewObject fixtures. Dispatch goes through UGroundTraffic
 * rather than through ARoadNetworkActor for the same reason - and the truck's VIEW still
 * appears, because UAirsideTraffic spawns one off the model's own phase broadcast.
 *
 * IT MAY NOT SEE Entities/ EITHER - Check-Architecture forbids Model/ that include, in this
 * module as much as in Airside's. So everything it needs from a UEntityDefinition (the pose
 * role, the truck count) is read off FEntityInstance, where placement captured it.
 *
 * IT SEES BOTH CLOCKS, and which one answers which question is the whole of the paragraph
 * below. A DWELL is timed on UGroundTraffic::GetSimSeconds; a TURNAROUND is timed on
 * USimClock - see FFuelDemand::TurnaroundEndsAt and FAirframe::TurnaroundSeconds. They are
 * opposite cases of the same compression and neither reading generalises to the other.
 *
 * THE DWELL COMES FROM UGroundTraffic::GetSimSeconds, NOT FROM USimClock. The clock is
 * day-compressed - at the default 1200 real seconds per game day a 40 s dwell would be 0.55
 * real seconds - while the truck's MOTION runs on the speed multiplier alone. A dwell timed
 * on the clock would be over before the truck had finished rolling to a stop. USimClock's own
 * header says turnaround durations are authored in game time; this is the one place that
 * reading does not survive contact with a dwell the player watches, and the reason is
 * recorded here rather than argued again later.
 *
 * SCAFFOLDING, and named as such by the spec (§0.1): M3's UJobBoard replaces "nearest depot
 * with a truck free" with demands from a flight, depots bidding by ETA, multi-trip jobs and
 * stuck recovery. What survives is everything BELOW it - the road, the anchor joins, the
 * vehicle agent and its view - which is why none of that is in this class.
 */
UCLASS()
class AIRPORTOPS_API UFuelService : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * How long a truck stays at the hydrant, in the sim seconds a truck MOVES in.
	 *
	 * A PROPERTY AND NOT A CONSTANT, so it is a figure a designer changes rather than a
	 * recompile. Set from UScenario::FuelDwellSeconds at attach, exactly as USimClock's
	 * RealSecondsPerGameDay is; the default here is only what a bare NewObject gets.
	 */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0"))
	double DwellSeconds = 40.0;

	/**
	 * Every phase change in the traffic model - the events this class is driven by.
	 *
	 * An aircraft reaching Parked at a stand makes a demand; a truck reaching Parked at the
	 * hydrant starts the dwell; a truck reaching Parked at its depot is retired; an aircraft
	 * LEAVING Parked drops its demand and sends any truck home.
	 */
	void OnAgentPhase(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock,
		int32 AgentId,
		EAgentPhase From, EAgentPhase To);

	/**
	 * One pass: offer every Needed demand a truck, run the dwells down, and re-offer the
	 * unserviceable ones when the graph has changed under them.
	 */
	void Tick(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/**
	 * The one line the inspector's aircraft card shows for this agent, or empty when it has
	 * no demand.
	 *
	 * A STRING BUILT HERE rather than an enum the panel switches on: it is presentation of
	 * two orthogonal model facts (the state, and for Unserviceable the reason), and nothing
	 * branches on it - the same argument InspectFacts::StatusOf makes for its own line.
	 */
	FString DescribeAgent(int32 AgentId) const;

	const TArray<FFuelDemand>& GetDemands() const { return Demands; }

	/** How many trucks are driving back to a depot right now. For the wiring test, which has
	 *  to tell "retired at home" from "never dispatched". */
	int32 TrucksGoingHomeForTest() const { return GoingHome.Num(); }

private:
	/**
	 * Send every aircraft whose turnaround has run out and whose services are finished.
	 *
	 * Its own function and not a fifth case in Tick's switch, because it is a pass OVER the
	 * demands rather than a transition of one: departing an aircraft removes its demand, so
	 * this cannot run inside the loop that walks them. See its body.
	 *
	 * THE SEAM M3 TAKES OVER. "All services done" is one demand today because fuel is the
	 * only service; UJobBoard replaces this with a flight's whole set, and this function is
	 * where that question is asked. See this class's header on being scaffolding.
	 */
	void DepartTheReady(UGroundTraffic& Traffic, const URoadNetwork& Network,
		const USimClock& Clock);

	UPROPERTY() TArray<FFuelDemand> Demands;

	/**
	 * Trucks on their way back, and the depot each is going to.
	 *
	 * SEPARATE FROM THE DEMAND, because a truck outlives one. The demand is DROPPED when its
	 * aircraft leaves mid-service (spec §6), and the truck is still out on the road - so a
	 * home-bound truck tracked only on the demand would never be retired, and its depot would
	 * lose a truck for the rest of the session. Both paths home therefore go through here,
	 * and the demand's TruckId is cleared at the same moment.
	 *
	 * A truck in here still COUNTS as out (see TrucksOutFor): it is not available until it
	 * has actually arrived.
	 */
	UPROPERTY() TMap<int32, FEntityInstanceId> GoingHome;

	/**
	 * The guideline revision the last refusal was decided against.
	 *
	 * WHAT MAKES Unserviceable RE-OFFERABLE WITHOUT BEING RETRIED EVERY TICK. The revision is
	 * bumped by every guideline mutation (URoadNetwork::GetGuidelineRevision), so a player
	 * drawing the missing road changes it and nothing else does. Not saved: it dates a graph
	 * within one session, and a loaded graph starts at zero with every agent gone anyway.
	 */
	UPROPERTY(Transient) uint32 LastRefusedRevision = 0;

	FFuelDemand* FindByAircraft(int32 AircraftId);
	const FFuelDemand* FindByAircraft(int32 AircraftId) const;
	FFuelDemand* FindByTruck(int32 TruckId);

	/** The stand's Fuel anchor node, or unset. By ROLE and then by ID, never by index - see
	 *  URoadNetwork::GetAnchorIdsForRole. */
	static FGuidelineNodeId FuelAnchorOf(const URoadNetwork& Network, FEntityInstanceId Stand);

	/**
	 * Nearest depot with a truck free and a route to StandFuel, BY ROUTE LENGTH.
	 *
	 * Not by straight-line distance: a depot 200 m away across a runway is further than one
	 * 400 m away along the road, and the truck drives the road. Fills OutPlan with the
	 * winning route so the caller dispatches the very route the choice was made on.
	 *
	 * Reports Why in the order the spec fixes - NoDepot, NoRoad, StandUnjoined, NoRoute - so
	 * the reason names the thing nearest the player's hand.
	 */
	FEntityInstanceId ChooseDepot(const URoadNetwork& Network, FGuidelineNodeId StandFuel,
		FRoutePlan& OutPlan, EFuelRefusal& OutWhy) const;

	/**
	 * How many trucks this depot has out right now, counted off Demands and GoingHome.
	 *
	 * COUNTED, NEVER STORED on the entity: a field there would be a second source of truth
	 * about the same fact, and the two would drift the first time a truck was retired by any
	 * path this class did not write.
	 */
	int32 TrucksOutFor(FEntityInstanceId Depot) const;

	/**
	 * Redirect a truck to its depot's pose node, or retire it where it stands if it cannot
	 * get home. Used by Done and by an aircraft leaving mid-service.
	 */
	void SendTruckHome(UGroundTraffic& Traffic, const URoadNetwork& Network, int32 TruckId,
		FEntityInstanceId Depot);
};
