#pragma once

#include "CoreMinimal.h"
#include "Model/DeparturePlanner.h"
#include "Model/OpsSave.h"
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
	NoRoute,

	/**
	 * A depot is on a road and has a truck, but no PUMP was built in its plot.
	 *
	 * ITS OWN REFUSAL AND NOT NoRoute, for the reason ChooseDepot's busy branch records at
	 * length: a pumpless depot falling through to the chain's default would report "no road
	 * from depot" and send the player to look at a road that is already there. What they
	 * actually need to do is build a pump in a bay.
	 */
	NoPump
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
	 *
	 * THE SINGLE OFF-BLOCK TRUTH. UFlight used to carry its own OffBlockAt, computed from the
	 * ETA at offer time; this is computed from the actual park time and the two disagreed the
	 * moment an arrival was late. Departure is decided here and nowhere else - see issue #97.
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
class AIRPORTOPS_API UFuelService : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	// --- IOpsPersistent ---------------------------------------------------------------
	/** "Fuel": a NEW blob - no save before this issue ever captured this class at all, which
	 *  is the other half of the bug OnBeforeRestore fixes (Demands/GoingHome were never even
	 *  reset, let alone saved). */
	virtual FName SaveBlobName() const override { return TEXT("Fuel"); }
	virtual UObject& AsPersistentObject() override { return *this; }

	/**
	 * Demands AND GoingHome are cleared, not restored from a blob: both name agents
	 * (TruckId/AircraftId), and UOpsRuntime::LoadFromSlot always clears every agent before
	 * calling OpsSave::Restore - so any id either held is stale the instant a load happens,
	 * whether or not this snapshot even has a Fuel blob. Before this fix GoingHome in
	 * particular was never touched at all: a truck sent home, then a load, left its entry in
	 * GoingHome forever (nothing removes an entry except the truck arriving, which it now
	 * never will, being gone) - TrucksOutFor(Depot) counted it as out for the rest of the
	 * session, one truck short, for every depot a truck happened to be homeward bound from
	 * at save time. The demands agents rebuild the moment OnAgentPhase sees them again.
	 */
	virtual void OnBeforeRestore() override { Demands.Reset(); GoingHome.Reset(); }

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
	 * The floor DwellSecondsFor cannot go below, however many pumps a depot has.
	 *
	 * A pump farm must not make refuelling instant: the dwell is the only pressure the fuel
	 * loop applies, and a depot that discharged a demand the frame it arrived would delete
	 * the reason to build a second one.
	 */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0"))
	double MinDwellSeconds = 5.0;

	/**
	 * True when Depot can fuel at all - it has a pump, or it predates modules entirely.
	 *
	 * A depot placed WITHOUT a plot has an empty module list and fuels as it always did.
	 * That is not a claim it has a pump; it is that the question does not apply, and a save
	 * written before plots existed must keep working.
	 */
	static bool HasWorkingPump(const FEntityInstance& Depot);

	/**
	 * How long a truck from Depot dwells at the hydrant - the base divided by its pumps.
	 *
	 * READS EDepotModule DIRECTLY, which this layer may do because that enum lives in
	 * Airside's Model/ rather than its Entities/. Had it been an Entities/ type the count
	 * would have needed a captured field on FEntityInstance to reach here at all, exactly as
	 * Trucks did - that it does not is the test that the enum sits in the right layer.
	 *
	 * A depot with no modules gets the base dwell: see HasWorkingPump. A depot with modules
	 * and no pump never reaches here, because ChooseDepot will not dispatch to one.
	 */
	double DwellSecondsFor(const FEntityInstance& Depot) const;

	/**
	 * The truck's performance figures, dispatched with every fuel demand.
	 *
	 * Set from UAirsideSettings::ResolveDefaultVehicle() at attach, next to DwellSeconds
	 * above - not resolved here at dispatch time. This is Model/, and reaching Content/ was
	 * the only Model->Content edge in either plugin (#104): Present/ (UOpsRuntime) is where
	 * every other content default gets resolved once and handed down.
	 */
	UPROPERTY() FAirframe TruckAirframe;

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

	/** Puts a truck in GoingHome without running the traffic model - so OpsSave's tests can
	 *  reach the leak OnBeforeRestore fixes without a full arrival-to-turnaround fixture,
	 *  which FuelServiceTest.cpp already builds for the behavioural side of this class. */
	void SetGoingHomeForTest(int32 TruckId, FEntityInstanceId Depot) { GoingHome.Add(TruckId, Depot); }

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

	/**
	 * TRANSIENT (PR #137 review, issue #105 item 8): both this and GoingHome below NAME
	 * AGENTS (TruckId, AircraftId), and agents are never saved - see OpsRuntimeTest's own
	 * "agents do not survive a load" assertion. Non-Transient, these were serialized straight
	 * into the new Fuel blob, and RestoreBlob's OnBeforeRestore() (which clears both) ran
	 * BEFORE the deserialize that then overwrote them right back from the blob - the leak
	 * this issue traces would have come back through any v4 save with a truck homeward-bound,
	 * exactly the bug OnBeforeRestore exists to fix.
	 */
	UPROPERTY(Transient) TArray<FFuelDemand> Demands;

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
	 * has actually arrived. TRANSIENT for the same reason as Demands above.
	 */
	UPROPERTY(Transient) TMap<int32, FEntityInstanceId> GoingHome;

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
	 * ChooseDepot's whole answer, so a caller that refuses does not have to re-walk
	 * Network.GetEntities() a second time just to LOG the counts the search already saw
	 * (#103) - Tick's Unserviceable branch used to do exactly that, plus a second
	 * FuelAnchorOf(Demand.Stand) call for the same node ChooseDepot was already given.
	 */
	struct FDepotChoice
	{
		FEntityInstanceId Depot;
		FRoutePlan Plan;
		EFuelRefusal Why = EFuelRefusal::NoDepot;

		/** How many entities are fuel depots at all, and how many of those are joined to a
		 *  road - the two counts the refusal log names so the player knows which end of the
		 *  airport to look at. */
		int32 Depots = 0;
		int32 DepotsOnRoad = 0;
	};

	/**
	 * Nearest depot with a truck free and a route to StandFuel, BY ROUTE LENGTH.
	 *
	 * Not by straight-line distance: a depot 200 m away across a runway is further than one
	 * 400 m away along the road, and the truck drives the road. The result's Plan is the
	 * winning route so the caller dispatches the very route the choice was made on.
	 *
	 * Reports Why in the order the spec fixes - NoDepot, NoRoad, StandUnjoined, NoRoute - so
	 * the reason names the thing nearest the player's hand.
	 */
	FDepotChoice ChooseDepot(const URoadNetwork& Network, FGuidelineNodeId StandFuel) const;

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
