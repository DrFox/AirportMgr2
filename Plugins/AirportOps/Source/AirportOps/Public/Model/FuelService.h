#pragma once

#include "CoreMinimal.h"
#include "Model/DeparturePlanner.h"
#include "Model/OpsSave.h"
#include "Model/RoadHandles.h"
#include "Model/RoutePlanCache.h"
#include "Model/RouteSearch.h"
#include "Model/Vehicle.h"
#include "Solve/IcaoCode.h"
#include "UObject/Object.h"

class USimClock;
#include "FuelService.generated.h"

class UGroundTraffic;
class URoadNetwork;
class ULedger;
class UPricing;
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
	 * Joined and connected, but only over road the truck does not fit - a lane narrower than
	 * its body or a corner its swept path cannot take (route search's TooNarrow, spec
	 * 2026-09-23 §6). ITS OWN REFUSAL AND NOT NoRoute, for NoPump's reason: "no road from
	 * depot" about a road that is there sends the player to fix the wrong thing.
	 */
	TooNarrow,

	/**
	 * A depot is on a road and has a truck, but no PUMP was built in its plot.
	 *
	 * ITS OWN REFUSAL AND NOT NoRoute, for the reason ChooseDepot's busy branch records at
	 * length: a pumpless depot falling through to the chain's default would report "no road
	 * from depot" and send the player to look at a road that is already there. What they
	 * actually need to do is build a pump in a bay.
	 */
	NoPump,

	/**
	 * A depot is on a road with a pump and a truck, but the vehicle it would send is LARGER than
	 * the stand was drawn for (VehicleFit::NoLargerThan against the letter's design vehicle,
	 * spec 2026-09-26 section 2). A stand's lane legs are only proven drivable by its own design
	 * vehicle and anything no larger, so sending a bigger one would strand it on a leg it
	 * cannot take. ITS OWN REFUSAL AND NOT TooNarrow: nothing about the ROAD is wrong, and a
	 * player widening one would fix nothing. UNREACHABLE TODAY, deliberately - fleets are
	 * untyped counts, so a depot sends the design vehicle itself - and here so typed fleets
	 * slot in behind a refusal that already exists.
	 */
	VehicleTooLarge
};

/**
 * One ICAO letter's two fuel vehicles: what its stand was DRAWN for, and what a depot SENDS.
 *
 * TWO FIELDS, ONE STRUCT ("one struct per thing"): they are always read together - the guard in
 * ChooseDepot compares them - and two parallel arrays would be two tables that could be filled
 * in different breaths. EQUAL TODAY: depot fleets are untyped counts, so the vehicle sent is the
 * most efficient that fits, which is the design vehicle itself (spec 2026-09-26 section 2). They
 * part company only when typed fleets arrive, and Sent is the field that will change.
 *
 * Each an FVehicle, not an FAirframe with its climb zeroed - the single TruckVehicle this table
 * replaced made that move on 2026-09-23 (see Model/Vehicle.h).
 */
USTRUCT()
struct AIRPORTOPS_API FLetterFuelVehicles
{
	GENERATED_BODY()

	/** What the letter's stand geometry is sized for - UAirsideSettings::ResolveStandDesignVehicle. */
	UPROPERTY() FVehicle Design;

	/** What a depot dispatches to a stand of this letter. Must be VehicleFit::NoLargerThan Design. */
	UPROPERTY() FVehicle Sent;
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

	/**
	 * The guideline revision THIS demand's refusal was decided against.
	 *
	 * PER DEMAND, NOT ON THE SERVICE (issue #193): a single UFuelService::LastRefusedRevision
	 * used to stand in for every demand's own fact, and two demands refused on different
	 * ticks share the one field. Demand A refusing later in the SAME Tick pass overwrites it
	 * with the newer revision before Demand B - already Unserviceable from an older one - is
	 * checked, so B reads "already seen this revision" against a value ITS OWN refusal never
	 * set, and stays stuck with the re-offer log line never firing. Moving the fact onto the
	 * demand it describes is what makes two demands unable to corrupt each other's history.
	 * Not saved, for the same reason UFuelService's copy never was: it dates a graph within
	 * one session, and Demands itself is Transient.
	 */
	UPROPERTY() uint32 RefusedAtRevision = 0;

	/**
	 * The two session clocks last seen when this demand found every depot simply BUSY - see
	 * UFuelService::Tick's Needed case and UFuelService::FleetRevision.
	 *
	 * NOT RefusedAtRevision above, and beside it rather than folded into it (issue #190,
	 * "one struct per thing"): busy is never Unserviceable - ChooseDepot's own "NOTHING IS
	 * WRONG - WAIT" comment - so it is a different fact with a different terminal condition,
	 * and a re-offer needs BOTH a fleet fact (a truck may have freed up) and a graph fact (a
	 * road, depot or pump may have been built), where Unserviceable only ever needed the one.
	 *
	 * MAX_uint32 so a demand that has never yet been found busy runs ChooseDepot once rather
	 * than matching a fresh session that happens to start both clocks at 0 - the same reason
	 * URoadNetwork::PoseNodeIndexRevision starts there instead of at 0.
	 */
	UPROPERTY() uint32 BusyAtGuidelineRevision = MAX_uint32;
	UPROPERTY() uint32 BusyAtFleetRevision = MAX_uint32;
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

	/** How many ICAO letters the vehicle table holds, A-F. See VehiclesByLetter. */
	static constexpr int32 LetterCount = 6;

	/**
	 * Fill every letter's table entry from Resolve - Design and Sent both, since fleets are
	 * untyped (FLetterFuelVehicles). Called ONCE, at attach, by UOpsRuntime with
	 * UAirsideSettings::ResolveStandDesignVehicle, next to DwellSeconds above - not resolved
	 * here at dispatch time. This is Model/, and reaching Content/ was the only Model->Content
	 * edge in either plugin (#104): Present/ (UOpsRuntime) is where every other content default
	 * gets resolved once and handed down. A TFunctionRef rather than the resolve itself for the
	 * same reason, and so the world-free fixture fills the table through the one loop too.
	 */
	void ResolveVehicles(TFunctionRef<FVehicle(EIcaoCode)> Resolve);

	/** One letter's entry. Mutable so a test can make the sent vehicle differ from the design. */
	FLetterFuelVehicles& VehiclesFor(EIcaoCode Letter) { return VehiclesByLetter[static_cast<uint8>(Letter)]; }
	const FLetterFuelVehicles& VehiclesFor(EIcaoCode Letter) const { return VehiclesByLetter[static_cast<uint8>(Letter)]; }

	/**
	 * The vehicle a depot sends to Stand: its outline's letter's Sent entry.
	 *
	 * THE OUTLINE'S LETTER (StandBox::LetterOf), not DesignWingspan's: the outline is what the
	 * player drew and what the stand's definition was chosen from, the same reading
	 * ARoadNetworkActor::RebindStandDefinitions makes. A stand whose outline reads as no letter
	 * gets Code C's - the letter URoadNetwork::PlaceEntity gives every stand placed with no plot.
	 * ENFORCED BY: Airside.Model.StandOutline.PointPlacedStandGetsOutline
	 */
	FVehicle VehicleFor(const FEntityInstance& Stand) const;

	/** The letter VehicleFor reads Stand as - LetterOf(Outline), else C. See VehicleFor. */
	static EIcaoCode LetterOfStand(const FEntityInstance& Stand);

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
	 * The money, or null in a test that does not care. Set by UOpsRuntime::Attach, in the same
	 * breath as the board's and the generator's, so none of them is the one left unconnected.
	 */
	UPROPERTY() TObjectPtr<ULedger> Ledger = nullptr;
	UPROPERTY() TObjectPtr<UPricing> Pricing = nullptr;

	/**
	 * Bank the fee for one completed fuelling.
	 *
	 * THERE IS NO MATCHING PENALTY METHOD, and that is the design rather than an omission (spec
	 * 2026-09-13 D7): an aircraft that times out Unserviceable never reaches here, so the
	 * forfeit is an entry that does not happen. A negative entry would be a FINE, which is a
	 * different thing needing a promised time to be late against - and nothing in this build
	 * has one, because FFuelDemand::TurnaroundEndsAt is computed from the actual park time.
	 */
	void PostServiceFee(double Now, const FAirframe& Airframe);

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

	/** TrucksOutFor, for a test to assert a depot got its truck back - not merely that the
	 *  truck stopped being on its way (TrucksGoingHomeForTest). */
	int32 TrucksOutForTest(FEntityInstanceId Depot) const { return TrucksOutFor(Depot); }

	/**
	 * Whether a route home found UNGATED (SendTruckHome's too-narrow fallback) may be driven:
	 * yes, unless the truck tows something the route folds (VehicleFit::JudgePlan,
	 * EFitRefusal::TrailerFolds) - a scuffed kerb is accepted, a jack-knife is not (review of
	 * 9441ccf1). OutWhy, when given and the answer is no, names the fold. Static and public so
	 * the rule is testable on a road that folds; the fuel fixture has none.
	 */
	static bool MayDriveUngated(const FRoutePlan& Plan, const FVehicle& Vehicle, const URoadNetwork& Network,
		FString* OutWhy = nullptr);

	/** Puts a truck in GoingHome without running the traffic model - so OpsSave's tests can
	 *  reach the leak OnBeforeRestore fixes without a full arrival-to-turnaround fixture,
	 *  which FuelServiceTest.cpp already builds for the behavioural side of this class. */
	void SetGoingHomeForTest(int32 TruckId, FEntityInstanceId Depot) { GoingHome.Add(TruckId, Depot); }

	/**
	 * How many times ChooseDepot actually ran this session - the route search, the
	 * IsServiceNodeConnected BFS, the whole depot walk. For a test to prove the Needed case's
	 * busy-wait skip (issue #190) really skips it, rather than merely naming the contract.
	 *
	 * MUTABLE: ChooseDepot is const and this counts real work it did, not a decision - the
	 * same reason URoadNetwork::SampleGuidelineCalls is mutable.
	 */
	int32 GetChooseDepotCallCountForTest() const { return ChooseDepotCallCountForTest; }
	void ResetChooseDepotCallCountForTest() { ChooseDepotCallCountForTest = 0; }

	/**
	 * ChooseDepot, public for the route-plan-cache test (#301): calling it directly lets a test
	 * repeat the SAME ask any number of times without contriving that many distinct
	 * FleetRevision bumps to get past the busy-wait skip Tick's own Needed case applies -
	 * FFuelBusyWaitSkipsChooseDepotTest already covers that skip on its own. The return type is
	 * FDepotChoice, private below; auto lets a test hold one without naming it.
	 */
	auto ChooseDepotForTest(const URoadNetwork& Network, FGuidelineNodeId StandFuel,
		const FLetterFuelVehicles& Vehicles) const { return ChooseDepot(Network, StandFuel, Vehicles); }

	/** See FleetRevision. For a test to assert a truck retiring/recalling actually moved it. */
	uint32 GetFleetRevisionForTest() const { return FleetRevision; }

	/**
	 * Appends a demand with the caller's own starting state, Stand left unset.
	 *
	 * BYPASSES OnAgentPhase, so a test can put two demands in a CHOSEN ARRAY ORDER with a
	 * chosen refusal-revision HISTORY - exactly the two facts FFuelDemand::RefusedAtRevision's
	 * bug (issue #193) turns on - without contriving two stands that fail for independent,
	 * timing-sensitive reasons real geometry cannot pin to a single tick. Safe only where
	 * ChooseDepot never reads Stand, i.e. with no depot on the fixture's airport at all: the
	 * NoDepot branch is decided from Network.GetEntities() alone.
	 */
	void AddDemandForTest(int32 AircraftId, EFuelDemandState State, EFuelRefusal Why,
		uint32 RefusedAtRevision)
	{
		FFuelDemand Demand;
		Demand.AircraftId = AircraftId;
		Demand.State = State;
		Demand.Why = Why;
		Demand.RefusedAtRevision = RefusedAtRevision;
		Demands.Add(Demand);
	}

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
	 *
	 * Vehicles is the STAND's letter's entry: the route is searched for Vehicles.Sent, and a
	 * depot whose Sent is larger than Vehicles.Design is refused VehicleTooLarge.
	 */
	FDepotChoice ChooseDepot(const URoadNetwork& Network, FGuidelineNodeId StandFuel,
		const FLetterFuelVehicles& Vehicles) const;

	/** VehiclesFor(LetterOfStand) for a stand by id; Code C's for a stand no longer there. */
	const FLetterFuelVehicles& VehiclesForStand(const URoadNetwork& Network, FEntityInstanceId Stand) const;

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

	/**
	 * Bumped at the three places TrucksOutFor's total for a depot can go DOWN: a truck
	 * arriving home (OnAgentPhase's own retire), and SendTruckHome's two "this truck no
	 * longer counts as out anywhere" branches (no route home; the agent was already gone).
	 * A depot's truck COUNT growing is not tracked here - Instance.Trucks is set only at
	 * placement (URoadNetwork::PlaceEntity), which already bumps GetGuidelineRevision, and
	 * Tick's Needed case checks both clocks - see FFuelDemand::BusyAtFleetRevision.
	 *
	 * A SESSION CLOCK, NOT STATE, the same as URoadNetwork::GuidelineRevision: not a
	 * UPROPERTY, and it does not need to be one, because Demands (Transient anyway) is the
	 * only place a comparison against it has to survive from one Tick to the next.
	 */
	uint32 FleetRevision = 0;

	/** See GetChooseDepotCallCountForTest. */
	mutable int32 ChooseDepotCallCountForTest = 0;

	/**
	 * ChooseDepot's per-(depot, stand, truck) route cache, dated by the graph's guideline
	 * revision (#301: the shape ARigTestCourse's identical cache carried alone - see
	 * Airside/Model/RoutePlanCache.h for the one owner both now share). ChooseDepot walks
	 * every depot on every call the busy-wait above does not skip, and the same pair is asked
	 * again the moment a DIFFERENT depot frees up (FleetRevision) with the graph unchanged.
	 * MUTABLE: ChooseDepot is const, same reason as ChooseDepotCallCountForTest above.
	 */
	mutable FRoutePlanCache RouteCache;

	/**
	 * The per-letter fuel vehicles, indexed by EIcaoCode - see ResolveVehicles for where they
	 * are filled, and FLetterFuelVehicles for why each entry is two vehicles.
	 *
	 * A TABLE RESOLVED ONCE, NOT A RESOLVE PER DISPATCH (#104's rule, which the single
	 * TruckVehicle this replaced also kept): one resolve per letter at attach, then a lookup.
	 * TRANSIENT, where TruckVehicle was saved: it is a content default resolved every attach,
	 * and a saved copy would pin the figures of whatever content the save was written under.
	 *
	 * A LITERAL 6, NOT LetterCount: UHT parses the dimension and a class constant is not
	 * something it is promised to resolve. FuelService.cpp static_asserts the two agree.
	 */
	UPROPERTY(Transient) FLetterFuelVehicles VehiclesByLetter[6];
};
