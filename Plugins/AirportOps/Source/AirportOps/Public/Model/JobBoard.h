#pragma once

#include "CoreMinimal.h"
#include "Model/OpsDefinition.h"
#include "Model/OpsDesignDefaults.h"
#include "Model/Airframe.h"
#include "Model/DeparturePlanner.h"
#include "Model/DepotCapability.h"
#include "Model/OpsSave.h"
#include "Model/RoadHandles.h"
#include "Model/RoutePlanCache.h"
#include "Model/RouteSearch.h"
#include "Model/ServiceBid.h"
#include "Model/ServiceFleet.h"
#include "Model/ServiceJob.h"
#include "Model/ServiceRolePolicy.h"
#include "Model/ServiceVehicle.h"
#include "Model/ServiceVehicleLifecycle.h"
#include "Model/Vehicle.h"
#include "Solve/IcaoCode.h"
#include "UObject/Object.h"

class USimClock;
#include "JobBoard.generated.h"

struct FAgentTransition;

class UGroundTraffic;
class URoadNetwork;
class ULedger;
class FOpsEventBus;
class UPricing;
struct FRoadAgent;
enum class EAgentPhase : uint8;

/**
 * How far behind one depot is: the depot card's two texts and the counts behind them. See
 * UJobBoard::DescribeDepot.
 */
struct FDepotBacklog
{
	/** "3 jobs · clears in 38 min · 1 late", or "No jobs". */
	FString Summary;

	/** One line per vehicle, then its jobs in the order it will do them. Empty with no vehicles. */
	FString Detail;

	int32 Jobs = 0;
	int32 LateJobs = 0;

	/** The latest promised finish among its jobs, USimClock game seconds; 0 with none. */
	double ClearsAt = 0.0;
};

/**
 * Every parked aircraft's service jobs, every service vehicle, and the bidding that puts one on the
 * other. Spec 2026-09-28-service-vehicle-lifecycle; the systems map's UJobBoard (§3.5), first cut.
 *
 * REPLACED UFuelService (2026-09-28), which called itself SCAFFOLDING: it found a demand "the
 * nearest depot with a truck free", kept the truck's progress on the demand, and let the truck exist
 * only while it was out. Three things were wrong with that shape, all named by the user: the vehicle
 * could not own its own state (it was spread over the demand, GoingHome, Refilling and EAgentPhase);
 * a busy bowser five minutes away lost a 4000 L job to an idle 1000 L tow; and a truck with fuel to
 * spare drove home between two stands. Here the VEHICLE owns its state, cargo and queue
 * (FServiceVehicle), the ROLE owns its rules (IServiceRolePolicy), and a job goes to whichever vehicle
 * would FINISH it first with the job appended to its queue (ServiceBid::Finish).
 *
 * IN AirportOps AND NOT IN Airside, deliberately. Airside knows how a thing MOVES and must never learn
 * what it is FOR - so the road, the depot's pose, the vehicle's performance and the truck's view all
 * live over there, and jobs, vehicles and their states live here. That boundary is what
 * Check-Architecture.ps1 enforces in one direction; this class is the other side of it.
 *
 * IN Model/ AND NOT Present/, which is what makes it testable with no world: it takes the
 * UGroundTraffic and the URoadNetwork PER CALL and holds neither, so it cannot outlive a graph and a
 * test drives it with NewObject fixtures. Dispatch goes through UGroundTraffic rather than through
 * ARoadNetworkActor for the same reason - and the truck's VIEW still appears, because
 * UAirsideTraffic spawns one off the model's own phase broadcast.
 *
 * IT MAY NOT SEE Entities/ EITHER - Check-Architecture forbids Model/ that include, in this module as
 * much as in Airside's. So everything it needs from a UEntityDefinition (the pose role, the truck
 * count) is read off FEntityInstance, where placement captured it.
 *
 * ONE CLOCK SINCE 2026-09-28 (spec fuel-litres): pumping, refills and the turnaround are all timed on
 * USimClock. A bid's DRIVES are movement time and are converted into it - see BidFor. The truck's
 * MOTION still runs on the speed multiplier alone, as everything's does.
 *
 * THIS BOARD IS FUEL'S UNTIL A SECOND ROLE IS SCHEDULED (#443, its option a - said, rather than half-generalised).
 * What is role-shaped and would carry a second role today: the vehicle lifecycle and the bid ask the ROLE'S policy
 * (PolicyFor: NextStep, TripQuantity, ServeSeconds, FacilitySeconds, DoneWithin), and a vehicle and a job each carry a
 * Role. What is FUEL, written as fuel, and would have to be routed through the catalogue row and the policy before
 * another role could join (#443's option b, NOT built: a second role to design against decides its shape):
 *  - the catalogue: FServiceFleet::ResolveCatalogue gives every row Role = Fuel and reads its figures from FFuelVehicleSpec;
 *  - the tank: FFuelRolePolicy::CapacityOf is called as a static (FServiceFleet::Add, the refill log, a job's TankLitres at
 *    the bid, the re-bid and the stand, and the shop's quote);
 *  - the jobs: OnAgentPhase creates a Fuel job for a parked aircraft and no other kind, the fee is Pricing->FuelFee
 *    (PostServiceFee), and the outcome enum (EFuelOutcome) and its rule (FuelOutcomeOf) are fuel's;
 *  - the depot: a fuel depot is an entity whose PoseRole is Fuel and whose pumps are its EDepotModule::Pump modules
 *    (the starter seeding, CouldServe, HasWorkingPump, PumpsAt); PolicyFor pushes RefillLitresPerMinutePerPump into the
 *    one FFuelRolePolicy the constructor registers;
 *  - the words: RefusalText, DescribeAgent's fuel line, the "Fuel:" log lines.
 * PolicyFor is where a second role's policy would be looked up, and the registry has the constructor's Fuel entry
 * alone: a test-only way to register another had no caller and was removed, because a seam nothing plugs into is a
 * claim the code does not keep.
 */
UCLASS()
class AIRPORTOPS_API UJobBoard : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	UJobBoard();

	// --- IOpsPersistent ---------------------------------------------------------------
	/** "Fuel", KEPT from UFuelService so a snapshot written before the rename still finds its blob
	 *  (there are no player saves yet - memory note 2026-09-23 - but a test snapshot is one). */
	virtual FName SaveBlobName() const override { return TEXT("Fuel"); }
	virtual UObject& AsPersistentObject() override { return *this; }

	/**
	 * Jobs, turnarounds AND vehicles are CLEARED here: all three name agents (AircraftId, a
	 * vehicle's AgentId), and UOpsRuntime::LoadFromSlot always clears every agent before calling
	 * OpsSave::Restore - so any id they held is stale the instant a load happens, whether or not
	 * this snapshot even has a Fuel blob. Jobs and turnarounds (Transient) stay cleared. Vehicles
	 * and SeededDepots are saved, so a snapshot WITH a Fuel blob then restores them, and Serialize
	 * normalises each vehicle Idle at home with its agent forgotten. Before the fix this descends
	 * from, a truck sent home, then a load, left its entry in GoingHome forever and its depot one
	 * truck short for the rest of the session.
	 */
	virtual void OnBeforeRestore() override;

	/**
	 * LOADING normalises every restored vehicle to Idle at home - see Vehicles. HERE, not in
	 * OnAfterRestore, because OpsSave::RestoreBlob restores one object without calling it, and a
	 * vehicle deserialized "on its way home" with no agent behind it would count as out for ever.
	 */
	virtual void Serialize(FArchive& Ar) override;

	/**
	 * THE VEHICLE CATALOGUE (#430): one resolved row per KIND - its chassis from Content, its tank, flow, name and money
	 * from the scenario - joined at attach and after every load, by UOpsRuntime::ApplyScenarioFigures through
	 * FServiceFleet::ResolveCatalogue (#449), and read through TypeFor.
	 *
	 * REPLACED VehicleSpecs (each fuel vehicle's tank and flow by TypeCode, copied from UScenario::FuelVehicles at attach,
	 * spec 2026-09-28-fuel-litres) and FallbackSpec (the trailer's figures, which a code with no entry used to get). TypeFor
	 * re-assembled a row on every call from VehicleSpecs and a scan of the stand-letter table, so a kind no letter was
	 * designed for came back with a zero chassis and passed every fit gate. EMPTY in a bare NewObject: a fixture resolves
	 * it the way Attach does (UOpsRuntime::ResolveVehicleCatalogue).
	 *
	 * TRANSIENT, as VehiclesByLetter is: a content default resolved every attach and every load. VehicleSpecs was saved, so a load put the
	 * figures the save was written under back over the ones the attach had just resolved.
	 */
	const TMap<FName, FServiceVehicleType>& GetCatalogue() const { return Catalogue; }

	/** A catalogue row a test re-shapes (widens its body, empties its tank) - the row must exist. */
	FServiceVehicleType& CatalogueRowForTest(FName TypeCode) { return Catalogue.FindChecked(TypeCode); }

	/**
	 * The kinds of vehicle a STARTER fleet gives a depot (spec §3.4): UScenario::StarterFleet, filtered to the catalogue
	 * by FServiceFleet::ResolveCatalogue. A depot with Trucks = N gets N of each, once; the player's depot has Trucks 0
	 * and buys its fleet instead (UFacilityPurchases, facility-upgrades spec). A test that needs "the depot's only
	 * vehicle is the bowser" says so here rather than contriving a table.
	 *
	 * REPLACED FleetTypes() and its override DefaultFleetTypes (#430): with no override it was every distinct TypeCode
	 * in the stand-letter table, so changing the vehicle a stand LETTER was designed for silently changed which kinds a
	 * starter depot was seeded with. Transient for the catalogue's reason.
	 */
	UPROPERTY(Transient) TArray<FName> StarterFleet;

	/**
	 * Litres per GAME minute per pump module a depot refills a returning vehicle at. Handed to the
	 * fuel policy every time it is asked, so a test's or the scenario's value is always the one used.
	 *
	 * TRANSIENT (#449), with the catalogue above: the scenario's, never the save's, written by
	 * UOpsRuntime::ApplyScenarioFigures at attach and after every load. Saved, a retune reached a new game and
	 * never a loaded one.
	 * ENFORCED BY: AirportOps.Model.Save.DesignFiguresAreNotSaved (not saved), AirportOps.Present.RuntimeLoad.DesignFiguresAreTheScenarios (re-applied)
	 */
	UPROPERTY(Transient) double RefillLitresPerMinutePerPump = OpsDesignDefaults::RefillLitresPerMinutePerPump;

	/**
	 * The litres a parked aircraft asks for - the flight's own FuelLitres, drawn at its offer.
	 * UOpsRuntime::Attach points this at the flight board; unset, or for an agent no flight owns,
	 * DefaultLitres. 0 means it wants no fuel.
	 * ENFORCED BY: AirportOps.Fuel.RuntimeWiresLitresOwed
	 */
	TFunction<double(int32 AgentId, const FAirframe& Airframe)> LitresOwedFor;

	/** The one fallback load: the middle of the offer's draw (OpsDesignDefaults::FuelLoadDefault) - derived from the
	 *  draw's own bounds since #449, where it was a 0.7 typed beside them. */
	static double DefaultLitres(const FAirframe& Airframe) { return FMath::Max(Airframe.FuelCapacityLitres, 0.0) * OpsDesignDefaults::FuelLoadDefault; }

	/**
	 * How many modules of a kind a depot's plot can hold - the ceiling FDepotCapability seats the owned modules against.
	 * UOpsRuntime::Attach sets it to the same answer UFacilityPurchases::ReservedSlotsOf gives (DepotKit::ReservationOf,
	 * the presenter's own solve, memoised per depot), so the board and the shop read ONE plot. UNSET in a bare NewObject
	 * board, which has no plot solve: every owned module then counts as seated (see FDepotCapability::Of).
	 * ENFORCED BY: AirportOps.Present.Facility.CapabilityReadsTheRuntimesPlotSolve (a runtime whose board is left unwired
	 * counts the owned pumps, not the seated ones)
	 */
	FModuleCeilingFn ModuleCeilingOf;

	/**
	 * What Depot's SEATED modules give it (#443, ruled 2026-09-30): its pumps and whether it can fuel at all. THE MODULES
	 * THAT COUNT ARE THE PLACED ONES - the presenter could not seat an owned module beyond its plot's ceiling, and that
	 * module grants nothing. A depot placed WITHOUT a plot has an empty module list and fuels as it always did: the named
	 * legacy exemption of FDepotCapability, not a claim it has a pump.
	 */
	FDepotCapability CapabilityOf(FEntityInstanceId Id, const FEntityInstance& Depot) const
	{
		return FDepotCapability::Of(Id, Depot, ModuleCeilingOf);
	}

	/** True when Depot can fuel at all - a seated pump, or the legacy plotless exemption. See CapabilityOf. */
	bool HasWorkingPump(FEntityInstanceId Id, const FEntityInstance& Depot) const { return CapabilityOf(Id, Depot).HasWorkingPump(); }

	/** Seated pumps at Depot, or 1 for a plotless depot - see CapabilityOf. The refill rate's multiplier. */
	int32 PumpsAt(FEntityInstanceId Id, const FEntityInstance& Depot) const { return CapabilityOf(Id, Depot).Pumps(); }

	/** How many ICAO letters the vehicle table holds, A-F. See VehiclesByLetter. */
	static constexpr int32 LetterCount = 6;

	/**
	 * Fill every letter's table entry from Resolve. UOpsRuntime::Attach fills it with
	 * UAirsideSettings::ResolveStandDesignVehicle, beside the catalogue above; nothing on the dispatch
	 * path resolves content, and cannot: this is Model/, and reaching Content/ was the only
	 * Model->Content edge in either plugin (#104). A TFunctionRef rather than the resolve itself for
	 * the same reason, and so the world-free fixture fills the table through the one loop too.
	 *
	 * WHAT THE TABLE IS FOR NOW: the design-vehicle fallback for a stand whose definition carries
	 * none - the per-letter DESIGN vehicle, and nothing else (#430). It WAS also the vehicle catalogue
	 * (every distinct TypeCode in it was a kind a depot could have, and TypeFor took a kind's chassis
	 * from the first letter that sent it), so a kind no letter was designed for had no chassis at all,
	 * and changing a letter's design vehicle changed the starter fleet. The catalogue is its own map
	 * now (GetCatalogue). It no longer decides which vehicle a stand is SENT either: the bid does,
	 * among the vehicles the depots actually have.
	 * ENFORCED BY: Check-Architecture's include-direction rule (Model/ may not include Content/),
	 * and AirportOps.Fuel.RuntimeResolvesPerStand (red if Attach leaves one vehicle for all).
	 */
	void ResolveVehicles(TFunctionRef<FVehicle(EIcaoCode)> Resolve);

	/** One letter's entry. Mutable so a test can make a letter's vehicle something else. */
	FVehicle& VehiclesFor(EIcaoCode Letter) { return VehiclesByLetter[static_cast<uint8>(Letter)]; }
	const FVehicle& VehiclesFor(EIcaoCode Letter) const { return VehiclesByLetter[static_cast<uint8>(Letter)]; }

	/**
	 * What Stand's lanes were proven drivable by - UEntityDefinition::DesignVehicle, read by whoever
	 * set it (UOpsRuntime::Attach, through UAirsideSettings::ResolveStandDesignVehicleOf): this layer
	 * may not dereference a UEntityDefinition, so the read is handed in, the same way ResolveVehicles'
	 * resolve is. UNSET in a bare NewObject, and then DesignVehicleFor answers the letter's table entry.
	 * ENFORCED BY: AirportOps.Fuel.RuntimeResolvesPerStand (A's table entry made the truck, the runtime still reads A's stand as tow-built)
	 */
	TFunction<FVehicle(const FEntityInstance&)> DesignVehicleOf;

	/** DesignVehicleOf(Stand), or VehicleFor(Stand) with nothing set. The bid's eligibility ceiling. */
	FVehicle DesignVehicleFor(const FEntityInstance& Stand) const;

	/**
	 * The letter table's entry for Stand's outline letter.
	 *
	 * THE OUTLINE'S LETTER (StandBox::LetterOf), not DesignWingspan's: the outline is what the player
	 * drew and what the stand's definition was chosen from, the same reading
	 * ARoadNetworkActor::RebindStandDefinitions makes. A stand whose outline reads as no letter gets
	 * Code C's - the letter URoadNetwork::PlaceEntity gives every stand placed with no plot.
	 * ENFORCED BY: Airside.Model.StandOutline.PointPlacedStandGetsOutline
	 */
	FVehicle VehicleFor(const FEntityInstance& Stand) const;

	/** The letter VehicleFor reads Stand as - LetterOf(Outline), else C. See VehicleFor. */
	static EIcaoCode LetterOfStand(const FEntityInstance& Stand);

	/** What the player is told for Why - on the aircraft card and in the log. The wording IS the
	 *  contract: tests assert it, because it is what sends the player to the right thing to fix. */
	static const TCHAR* RefusalText(EServiceRefusal Why);

	/**
	 * The catalogue's row for TypeCode (#430): its chassis from Content, its figures from the scenario, joined once by
	 * FServiceFleet::ResolveCatalogue - no longer re-assembled per call from the letter table and the scenario map.
	 *
	 * A CODE WITH NO ROW - a vehicle restored under a scenario that has since dropped its kind, or a test's hand-made
	 * one; FServiceFleet::Add refuses to make such a vehicle - answers a row whose TypeCode is None and whose chassis is
	 * empty, with a Warning once per code. The bid's candidate filter skips it (Judge), so it serves nothing rather than
	 * fitting a zero-size vehicle through every gate, which is what the old per-call join did.
	 * ENFORCED BY: AirportOps.Fleet.CatalogueDropsARowWithNoChassis, AirportOps.Fleet.UnknownKindServesNothing
	 */
	FServiceVehicleType TypeFor(FName TypeCode) const;

	/**
	 * Could a depot fuel this airframe on some stand it would take - asked BEFORE the aircraft exists,
	 * for the offer row (spec 2026-09-28 section 3).
	 *
	 * THE SAME ELIGIBILITY the live bid applies, per admitting stand (StandAdmission::Judge), so the
	 * row's "fuel" chip and the truck that does or does not come cannot disagree. Busy vehicles count
	 * as servable: they bid with their queue. The cost is a route search per (depot, type, stand); the
	 * board calls it only when its verdict's revisions move - see FOfferVerdict.
	 *
	 * REAL VEHICLES ONLY (#443): it used to add the starter fleet a not-yet-seeded depot WOULD get, a prediction
	 * written beside the seeding it had to mirror and already fixed once for drifting from it ("seeded is not
	 * not-yet-seeded"). The seeding is FServiceFleet::SeedStarterFleets, run by every Step (SyncFleet) through the
	 * fleet's door, so a starter depot has its vehicles before any bid can use them and the offer's answer and the
	 * bid are read off the SAME vehicles. THE PRICE: asked before a board's first Step, a starter depot says "no
	 * fuel". A Step is the first thing UOpsRuntime::Attach schedules (Bus.MarkAllDirty), so no offer is read in that
	 * window in play; a test that asks earlier steps the board first.
	 * ENFORCED BY: AirportOps.Fuel.CouldServe.StarterDepotVerdictAgreesWithItsFirstBid,
	 * AirportOps.Fuel.CouldServe.SoldOutStarterDepotCannot, AirportOps.Present.Fleet.AttachSeedsTheStarterFleetOnTheFirstDrain
	 */
	bool CouldServe(const URoadNetwork& Network, const FAirframe& Airframe) const;

	/**
	 * Every phase change in the traffic model - the events this class is driven by.
	 *
	 * An aircraft reaching Parked at a stand makes a turnaround and its jobs; a vehicle's agent
	 * reaching Parked is that vehicle ARRIVING, which moves the vehicle's own state; an aircraft
	 * LEAVING Parked drops its turnaround and jobs and moves any vehicle out for them on; a vehicle's
	 * agent going Gone is that vehicle LOSING its agent (LoseAgent), and a Stranded one releases its jobs.
	 *
	 * MAPS THE TRANSITION'S CAUSE, not (From, To) plus the live agent (#436): it is heard a drain late, so what
	 * decides is what was true when the change was made - DepartOrdered, not bDepartureArmed read now; GoalAtEvent,
	 * not the GoalNode the agent may have been redirected to since.
	 */
	void OnAgentPhase(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock,
		const FAgentTransition& Transition);

	/**
	 * The money, or null in a test that does not care. Set by UOpsRuntime's constructor, in the same
	 * breath as the board's and the generator's, so none of them is the one left unconnected.
	 * TRANSIENT (#425): wiring, not state - saved, each was a path a later session resolved to null.
	 */
	UPROPERTY(Transient) TObjectPtr<ULedger> Ledger = nullptr;
	UPROPERTY(Transient) TObjectPtr<UPricing> Pricing = nullptr;

	/**
	 * Where a turnaround's end is announced (FTurnaroundEndedEvent). Set by UOpsRuntime::Attach beside the
	 * ledger; null in a bare NewObject, and the publish checks - Ledger's reason. Raw: the runtime owns
	 * both this board and the bus.
	 */
	FOpsEventBus* Bus = nullptr;

	/**
	 * Bank the fee for one completed fuelling.
	 *
	 * THERE IS NO MATCHING PENALTY METHOD, and that is the design rather than an omission (spec
	 * 2026-09-13 D7): an aircraft that times out Unserviceable never reaches here, so the forfeit is
	 * an entry that does not happen. A negative entry would be a FINE, which is a different thing
	 * needing a promised time to be late against - and nothing in this build has one, because
	 * FTurnaround::TurnaroundEndsAt is computed from the actual park time.
	 */
	void PostServiceFee(double Now, double Litres);

	/**
	 * One pass: the fleet brought in line with the depots, due serves and refills finished, open jobs
	 * bid, idle vehicles started on their queues, refused jobs re-offered when the graph changed, and
	 * the ready aircraft sent.
	 *
	 * RETURNS TRUE WHEN SOMETHING IS LEFT UNRESOLVED and must be looked at again next frame: a vehicle
	 * whose dispatch was refused (Idle with a queue), or a job still Open (a re-offered one is bid on the
	 * NEXT pass - see the body). NOT A VEHICLE PARKED AT A STAND WITH NO DECISION, which the final review
	 * #1 backstop used to keep re-running the pass for: that shape is no longer representable (Deciding
	 * never survives a Step - StartNext always lands the decision, and the Step ensures it - see
	 * EServiceVehicleState). False means nothing here changes until an event or a deadline says so - which
	 * is what lets the ops bus run this as a pass rather than every frame (spec 2026-09-29-ops-event-bus
	 * section 2; UOpsRuntime::WireBus's "JobBoard" pass).
	 * ENFORCED BY: AirportOps.Fuel.Lifecycle.BlockedHeadJobNeverLeavesItServingWithNoJob
	 *
	 * A DUE TURNAROUND THAT COULD NOT LEAVE IS NOT UNRESOLVED (ops push-ground-freed, 2026-09-30): it used
	 * to re-run this whole Step every frame for as long as it was refused. Its retries now come from what
	 * can change the answer - PushGroundFreed for PushbackBlocked (Airside's push watch), NetworkChanged
	 * for every other refusal (a runway, an arm or a route the player draws) - and the ops safety net
	 * while HasRefusedDeparture says one is waiting.
	 * ENFORCED BY: AirportOps.Present.PushGroundFreed.NoPushbackRouteIsQuiet
	 *
	 * ONE SEQUENCE, KEPT WHOLE, on purpose: each step's place in it is argued in its own comment, and
	 * splitting it across event handlers would re-derive that order in several places.
	 */
	bool Step(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/** Step, ignoring its answer - the world-free fixtures' per-frame driver, as it always was. */
	void Tick(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock) { Step(Traffic, Network, Clock); }

	/**
	 * The earliest game time after Now at which a Step would find something due: a vehicle's serve or
	 * refill ending (StepEndsAt), a turnaround's deadline. Max double for none. What the ops runtime
	 * schedules ONE Clock.At for, so a due step is not found by looking every frame.
	 */
	double NextDeadline(double Now) const;

	/** How many Steps have run - for the test that a quiet airport runs none. */
	int32 StepCountForTest() const { return StepCount; }

	/**
	 * The aircraft the last Step's DepartTheReady got away, cleared as each Step begins. The ops runtime reads it
	 * after a run the safety net alone asked for, to name the departure no event covered.
	 */
	const TArray<int32>& DepartedLastStep() const { return LastStepDeparted; }

	/**
	 * Is any turnaround due, not being served, and refused its departure by the last try? What keeps the ops
	 * safety net armed for departures: such an aircraft is retried only by an event now (see Step), and the net
	 * is for the one whose event never came. DERIVED from the board, asked after every Step.
	 */
	bool HasRefusedDeparture(double Now) const;

	/**
	 * Moves with every CHANGE to Jobs, Vehicles and Turnarounds, and not with a call that changed nothing (#443: it was
	 * bumped on entry to every OnAgentPhase for every agent in the airport and to every Step, so the inspector's depot
	 * card and fuel line, which key on it (ops batch 3 PR E), were rebuilt for changes that were not there). It is TWO
	 * COUNTERS SUMMED, so a monotonic sum moves when either does:
	 *  - FleetRevision: every vehicle transition (FServiceVehicleLifecycle bumps it) and every membership change
	 *    (FServiceFleet does) - which is most of what a Step or a phase event can do;
	 *  - RevisionCount: what moves WITHOUT a vehicle transition - a job made, queued, refused, re-opened or dropped, a
	 *    queue trimmed, a turnaround opened or closed, a re-bid that refreshed its promises, a load, and the ForTest adders.
	 * So a Step with nothing to do, an OnAgentPhase for an agent that is none of the board's business and a recall of
	 * a vehicle the board does not have leave it where it was. NOT StepCount, which the spec named: OnAgentPhase and
	 * the recall change the board outside Step, and a card keyed on steps would show them a pass late or never.
	 * A session counter, not saved - the same idiom as UFlightBoard::Revision.
	 * WHAT THIS DOES NOT CLAIM: that every bump is a visible change (a re-bid that finds nothing better still refreshes
	 * the promises it runs over, and says so by moving it), only that a change with no bump is a defect.
	 * ENFORCED BY: AirportOps.Fuel.RevisionMovesOnEveryChange (each public door, and the queue trim, the refused-job
	 * re-open and the released job, one line each), AirportOps.Fuel.RevisionMovesAtEachPointOfChange (the turnaround
	 * open, the drop, an assignment and a rebid that ran), AirportOps.Fuel.RevisionHoldsStillWhenNothingChanged (a Step
	 * with nothing to do, a foreign agent's phase, a recall of nobody)
	 */
	uint32 Revision() const { return RevisionCount + FleetRevision; }

	/**
	 * RevisionCount alone: the changes that move no vehicle (see Revision). FOR THE TESTS that pin each point-of-change bump
	 * on its own - the vehicle transitions that usually ride beside a change move FleetRevision, so Revision() cannot tell
	 * whether the leaf's own bump is still there, and a deleted one would go unseen.
	 */
	uint32 RevisionCountForTest() const { return RevisionCount; }

	/**
	 * The one line the inspector's card shows for this agent, or empty when the board has nothing to
	 * say about it: an aircraft's fuel line, or - for a service vehicle's agent - the vehicle's own
	 * line (DescribeVehicle). ONE SEAM FOR BOTH, because the inspector already asks it for whatever
	 * agent is selected.
	 *
	 * A STRING BUILT HERE rather than an enum the panel switches on: it is presentation of two
	 * orthogonal model facts (the state, and for Unserviceable the reason), and nothing branches on
	 * it - the same argument InspectFacts::StatusOf makes for its own line.
	 */
	FString DescribeAgent(int32 AgentId, double Now) const;

	/**
	 * DescribeAgent, and whether its answer MOVES WITH THE CLOCK: true only while the fuel line counts litres down
	 * through a trip being pumped (its "LIVE WHILE PUMPING" rule). Anything else it says changes only with the
	 * board, so a caller may keep it until Revision moves - the inspector does (ops batch 3 PR E).
	 * ENFORCED BY: AirportOps.Fuel.LineSaysWhenItMovesWithTheClock (live only while serving);
	 * AirportMgr.Inspector.Cache.FuelLineLiveWhilePumping (the card follows it while live)
	 */
	FString DescribeAgent(int32 AgentId, double Now, bool& bOutMovesWithClock) const;

	/**
	 * The player's Unstick for a VEHICLE (spec 2026-09-29-unstick-agent): every job it holds - the
	 * one it is on and its whole queue - goes back to the board for another vehicle to win, then it
	 * heads home (GoToFacility) or, bRetire, is retired where it stands and is Idle at home at once.
	 * THE EXPLICIT FORM of what an agent that is lost to the board does (LoseAgent): the Gone phase event
	 * for a vehicle's agent - the player's despawn through another door, a cleared traffic model - takes the
	 * same body, so every job it held, current AND queued, goes back to the board however the agent went. It
	 * used to be a poll in SyncFleet that re-opened only the current job and left the queue on a vehicle nobody
	 * was driving; the poll stays, as the net under the event.
	 * False, nothing changed, when AgentId drives no vehicle of this board.
	 * ENFORCED BY: AirportOps.Model.AgentRescue.VehicleSendHome, .VehicleDespawn,
	 * AirportOps.Fuel.Lifecycle.AgentRetiredElsewhereRebidsWholeQueue, .LostAgentNetRecallsTheSameWay
	 */
	bool RecallVehicleOfAgent(int32 AgentId, bool bRetire, UGroundTraffic& Traffic, const URoadNetwork& Network,
		const USimClock& Clock);

	/**
	 * How far behind Depot is (user, 2026-09-28: "see how far behind your depot is in jobs"): every job
	 * on its vehicles - under way, being served, or queued - when the last of them is promised to
	 * finish, and how many of those promises land after their aircraft's turnaround ends, which is the
	 * backlog actually costing the airport. Read off each job's PromisedFinish, which the re-bid pass
	 * refreshes on every trigger (RebidQueued), so the card needs no bookkeeping of its own. Minutes are
	 * whole - but rounded from Now, so they move at half-minute offsets of it, not at the game minute. The
	 * inspector therefore passes the START of the game minute as Now (ops batch 3 PR E), which makes its
	 * card a function of the minute it keys on, and redraws it at most once a game minute.
	 */
	FDepotBacklog DescribeDepot(FEntityInstanceId Depot, double Now) const;

	/**
	 * THE FLEET'S MEMBERSHIP DOOR (#443): every vehicle that joins or leaves goes through FServiceFleet::Add and
	 * Withdraw - the player's purchase and sale (UFacilityPurchases), the starter seeding and a removed depot's
	 * withdrawal (SyncFleet) - so each owes the same three things whichever asked: its Fleet ledger line, its
	 * FFleetChangedEvent, and (on an add) the re-opening of the jobs a missing vehicle refused. It was four doors with
	 * four sets of side effects, and the job board posted fleet money itself. A handle constructed per use, the way
	 * Lifecycle is, because the vehicles, the jobs and both revision counters are this board's to hold.
	 * ENFORCED BY: Check-Architecture rule 43 (fleet-one-door), AirportOps.Model.Fleet.*
	 */
	FServiceFleet Fleet() { return FServiceFleet(*this); }

	/** True when VehicleId is Idle, has no agent, no current job and an empty queue - the only vehicle
	 *  that may leave by a SALE (R5). FServiceFleet::Withdraw(Sold) asks exactly this. */
	bool CanRemoveVehicle(int32 VehicleId) const;

	/** Vehicles whose Home is Depot - counted off the vehicles, never stored on the depot. */
	int32 VehiclesAt(FEntityInstanceId Depot) const;

	/** "Bowser #7 · at depot 1 · 10,000 L" - the one line the depot card's backlog and its fleet rows share. The kind's
	 *  NAME, through FServiceFleet::NameOf (#430): the card listed "FUEL #7" beside the "Bowser" it sold. */
	FString VehicleLine(const FServiceVehicle& Vehicle) const;

	/** A turnaround with a deadline and one job, bypassing OnAgentPhase - for the backlog's lateness. */
	void AddTurnaroundForTest(int32 AircraftId, double TurnaroundEndsAt, int32 JobId);

	const TArray<FServiceJob>& GetJobs() const { return Jobs; }

	/**
	 * Its agent is Stranded: it bids for nothing until the player unsticks it (AssignOpenJobs, RebidQueued).
	 * PUBLIC since the ops alerts (spec 2026-09-29): UOpsAlerts reports a stranded vehicle by this same rule,
	 * so the board and the alert cannot disagree about what "stranded" is.
	 */
	static bool IsStranded(const FServiceVehicle& Vehicle, const UGroundTraffic& Traffic);
	const TArray<FServiceVehicle>& GetVehicles() const { return Vehicles; }
	const TArray<FTurnaround>& GetTurnarounds() const { return Turnarounds; }

	/** The aircraft's job of Role, or null. */
	const FServiceJob* JobForAircraft(int32 AircraftId, EServiceRole Role = EServiceRole::Fuel) const;

	/**
	 * How a departing aircraft left, from the FIGURES, not the job's state (batch 3 review I1): a job still
	 * being served when the player pressed Depart is part-fuelled, though it never went Unserviceable.
	 * Wanted <= 0 or delivered within FFuelRolePolicy::FuelledWithinLitres of it: Fuelled; nothing delivered:
	 * Unfuelled; otherwise PartFuelled. THE POLICY'S FIGURE, the one FinishServe calls a job Done by (DoneWithin), so
	 * the outcome and the fee use the same number - or a Done job 0.4 L short would read part-fuelled and be paid for
	 * twice (#443: it was a constant here beside two literals in the policy and two in the bid).
	 */
	static EFuelOutcome FuelOutcomeOf(double Delivered, double Wanted);

	/** The aircraft's turnaround, or null. */
	const FTurnaround* TurnaroundFor(int32 AircraftId) const;

	const FServiceVehicle* FindVehicle(int32 VehicleId) const;
	const FServiceVehicle* VehicleForAgent(int32 AgentId) const;

	/** The agent of the vehicle driving to or serving Job right now, or 0. NOT a vehicle that merely
	 *  has it queued: that vehicle may be on the road for somebody else. */
	int32 AgentForJob(const FServiceJob& Job) const;

	/** How many vehicles are driving to their facility right now. For the wiring test, which has to
	 *  tell "going home" from "never dispatched". */
	int32 TrucksGoingHomeForTest() const;

	/** How many of Depot's vehicles are not Idle - out, or refilling. Counted off the vehicles, never
	 *  stored on the depot: a stored count would be a second source of truth about the same fact. */
	int32 TrucksOutForTest(FEntityInstanceId Depot) const;

	/** How many vehicles are at their facility being refilled right now. */
	int32 RefillingForTest() const;

	/**
	 * Whether a route home found UNGATED (DriveVehicleTo's too-narrow fallback) may be driven: yes,
	 * unless the vehicle tows something the route folds (VehicleFit::JudgePlan,
	 * EFitRefusal::TrailerFolds) - a scuffed kerb is accepted, a jack-knife is not (review of
	 * 9441ccf1). OutWhy, when given and the answer is no, names the fold. Static and public so the
	 * rule is testable on a road that folds; the fuel fixture has none. Seed is the live chain and cab,
	 * so a route home opening with a reverse is solved from where the tow is; a reverse it cannot back
	 * is refused like a fold.
	 */
	static bool MayDriveUngated(const FRoutePlan& Plan, const FVehicle& Vehicle, const URoadNetwork& Network,
		FString* OutWhy = nullptr, const FTowSeed* Seed = nullptr);

	/** Puts a vehicle on the board without the placeholder fleet or the traffic model - for OpsSave's
	 *  and the re-bid tests. ITS HOME COUNTS AS SEEDED: a test that places a depot's vehicles by hand
	 *  means those to be the fleet, and SyncFleet must not add the placeholder's beside them. */
	FServiceVehicle& AddVehicleForTest(FName TypeCode, FEntityInstanceId Home, EServiceVehicleState State, double Cargo);

	/**
	 * How many JOB bids ran this session - one per job per bidding pass, whatever the fleet. For a test
	 * to prove an idle tick bids nothing (issue #190's rule, which the busy-wait skip used to keep).
	 * MUTABLE: counted from const paths, like URoadNetwork::SampleGuidelineCalls.
	 */
	int32 GetBidCallCountForTest() const { return BidCallCountForTest; }
	void ResetBidCallCountForTest() { BidCallCountForTest = 0; }

	/** Vehicle's bid for Job, exactly as AssignOpenJobs would compute it. For the route-cache test. */
	ServiceBid::FResult BidForTest(const UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock,
		int32 VehicleId, int32 JobId) const;

	/** See FleetRevision: every vehicle transition and every membership change. What the re-bid keys on. */
	uint32 GetFleetRevision() const { return FleetRevision; }
	uint32 GetFleetRevisionForTest() const { return GetFleetRevision(); }

	/**
	 * See FleetCompositionRevision: only who is in the fleet. What the offer verdict's bFuelServable is dated by
	 * (UFlightBoard::VerdictFor), because CouldServe reads which vehicles exist and where they live and nothing of
	 * their state - so a truck arriving, serving or refilling does not re-plan every pending offer.
	 */
	uint32 GetFleetCompositionRevision() const { return FleetCompositionRevision; }

	/**
	 * How much better, in GAME seconds, a re-bid must be before a queued job moves to another
	 * vehicle (user's ruling 7, spec §2.5). Two minutes: less and two near-equal vehicles swap a job
	 * back and forth on every trigger, which on screen is a truck changing its mind for nothing.
	 */
	static constexpr double RebidMarginSeconds = 120.0;

	/**
	 * A test's drive times, GAME seconds from node to node for a vehicle kind - replacing the road
	 * length over cruise speed. Empty in production. The re-bid's margin is a comparison of two
	 * finish times, and a test that has to derive both from road geometry cannot pin which side of
	 * two minutes they fall.
	 */
	TFunction<double(FGuidelineNodeId From, FGuidelineNodeId To, FName TypeCode)> DriveSecondsOverride;

	/**
	 * Appends a job with the caller's own starting state, Stand left unset.
	 *
	 * BYPASSES OnAgentPhase, so a test can put two jobs in a CHOSEN ARRAY ORDER with a chosen
	 * refusal-revision HISTORY - exactly the two facts FServiceJob::RefusedAtRevision's bug (issue
	 * #193) turns on - without contriving two stands that fail for independent, timing-sensitive
	 * reasons real geometry cannot pin to a single tick. Safe only where eligibility never reads Stand,
	 * i.e. with no depot on the fixture's airport at all: NoDepot is decided from Network.GetEntities()
	 * alone.
	 */
	FServiceJob& AddJobForTest(int32 AircraftId, EServiceJobState State, EServiceRefusal Why, uint32 RefusedAtRevision);

	/** The role's policy - Fuel's, registered at construction; null for a role with none. See the class comment for
	 *  what else a second role would have to reach. */
	const IServiceRolePolicy* PolicyFor(EServiceRole Role) const;

private:
	/** FServiceFleet writes the fleet's containers and both membership counters: it is this board's membership door,
	 *  and the vehicles, jobs and counters it moves are private here on purpose (Check-Architecture rule 43). */
	friend class FServiceFleet;

	/**
	 * Send every aircraft whose turnaround has run out and whose jobs are finished.
	 *
	 * Its own function and not a step in the job loop, because it is a pass OVER the turnarounds
	 * rather than a transition of one: departing an aircraft removes its turnaround and jobs, so this
	 * cannot run inside a loop that walks them. See its body.
	 */
	void DepartTheReady(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/**
	 * The placeholder fleet brought in line with the depots (spec §3.4): a live depot seen for the
	 * first time gets Trucks x StarterFleet vehicles, Idle at home and full (FServiceFleet::SeedStarterFleets); a
	 * vehicle whose depot is gone is withdrawn (its agent retired, its jobs back to the board, then
	 * FServiceFleet::Withdraw(DepotRemoved) credits its resale value and announces it); a vehicle whose agent vanished
	 * under it (retired by somebody else) is put back Idle at home by LoseAgent, every job it held re-opened - THE
	 * NET under OnAgentPhase's Gone branch, which is how the board normally hears of it.
	 */
	void SyncFleet(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/** Every Open job bid and assigned, or refused. See BidFor and Judge. */
	void AssignOpenJobs(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/**
	 * Decide the vehicle's next step now that it has none: its queue head direct, or its facility
	 * first (the policy's NextStep), or home when the queue is empty. The ONE place the lifecycle
	 * moves forward, so every path out of a step - serve done, refill done, recall, a job gone - reads
	 * the same rule the bid priced.
	 *
	 * ALWAYS LANDS: it never returns with the vehicle Deciding. A vehicle at home stays Idle (a job held by a
	 * prerequisite waits there, retried each Step); one on the road is set off, sent to its facility or - with
	 * nothing it can do - taken off the road, and a job it cannot start waits at home rather than where it stands.
	 * ENFORCED BY: AirportOps.Fuel.Lifecycle.BlockedHeadJobNeverLeavesItServingWithNoJob (the fixture's per-Step
	 * check), AirportOps.Fuel.UnreachableQueueSendsItHome
	 */
	void StartNext(FServiceVehicle& Vehicle, UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/** A vehicle's agent reached Parked, on ParkedOn - the transition's GoalAtEvent, not the live GoalNode (#436). */
	void OnVehicleArrived(FServiceVehicle& Vehicle, FGuidelineNodeId ParkedOn, UGroundTraffic& Traffic,
		const URoadNetwork& Network, const USimClock& Clock);

	/**
	 * The aircraft left its stand: its turnaround and jobs go, and vehicles out for them move on.
	 *
	 * bDeparted - it left for a departing phase (pushed back, taxied), not Gone or Stranded - makes this
	 * where a TURNED-AROUND aircraft's turnaround ends (batch 3 review I1): it calls EndTurnaround, the one
	 * publisher of FTurnaroundEndedEvent and poster of the part-fuelled fee, whoever sent it - DepartTheReady,
	 * or the inspector's Depart calling UGroundTraffic::DepartAgent directly, which never passes through
	 * DepartTheReady. EndTurnaround's other caller is OnAgentPhase, for a departure never turned around. A retire
	 * (Unstick's despawn) is not a departure: PR B scores it as a cancelled flight.
	 * ENFORCED BY: AirportOps.Fuel.ManualDepartEndsTurnaroundOnce, AirportOps.Fuel.RetiredAircraftEndsNoTurnaround
	 */
	void DropAircraft(int32 AircraftId, bool bDeparted, UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/**
	 * A turnaround's end, announced - THE ONE PUBLISHER of FTurnaroundEndedEvent (and poster of the part-fuelled fee).
	 * Two callers, one per way an aircraft can leave: DropAircraft for one that had a turnaround, and OnAgentPhase for
	 * one that departed WITHOUT ever being turned around (whole-stack review M4) - which can never both be true of one
	 * departure, since OnAgentPhase takes the second branch only when TurnaroundFor finds nothing.
	 * ENFORCED BY: AirportOps.Model.Bus.DepartFromFallbackReadsTaxiOut ("one TurnaroundEnded"), AirportOps.Fuel.ManualDepartEndsTurnaroundOnce
	 */
	void EndTurnaround(int32 AircraftId, FEntityInstanceId Stand, double Delivered, double Wanted, const USimClock& Clock);

	/**
	 * The litres AgentId's flight is owed: LitresOwedFor (the offer's FuelLitres), else DefaultLitres. ONE READ for both
	 * sites that ask - a turnaround's fuel job and a departure never turned around (whole-stack re-review m1) - so the
	 * two cannot be owed different amounts.
	 * ENFORCED BY: AirportOps.Model.Bus.DepartFromFallbackReadsTaxiOut ("owed what the flight was owed")
	 */
	double LitresWanted(int32 AgentId, const FAirframe& Airframe) const;

	/**
	 * A trip's pumping is over: quantities move, the job is Done or re-opened with its remainder. The vehicle is
	 * left Deciding (EndServe), parked at the stand with its agent, for the re-bid and then StartNext.
	 */
	void FinishServe(FServiceVehicle& Vehicle, const USimClock& Clock);

	/** Head for the facility (home), or - if the vehicle cannot get there - be Idle at home. */
	void GoToFacility(FServiceVehicle& Vehicle, UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/** At the facility with no agent: time the refill, or finish it at once when there is none. */
	void BeginFacility(FServiceVehicle& Vehicle, const URoadNetwork& Network, const USimClock& Clock);

	/**
	 * THE HANDLE for Vehicle's transitions, bound to this board's FleetRevision: every write of a vehicle's State,
	 * AgentId and CurrentJob goes through it (FServiceVehicleLifecycle), and it bumps FleetRevision itself.
	 * ENFORCED BY: Check-Architecture rule 38 (vehicle-lifecycle-one-writer)
	 */
	FServiceVehicleLifecycle Lifecycle(FServiceVehicle& Vehicle) { return FServiceVehicleLifecycle(Vehicle, FleetRevision); }

	/**
	 * Take Vehicle's agent off the road: the vehicle unhooks from it FIRST (LeaveRoad, Idle at home), then the
	 * agent is retired. THAT ORDER IS THE POINT: RetireAgent broadcasts Gone, the fixtures deliver it inside the
	 * call, and OnAgentPhase treats a Gone for a vehicle's agent as the agent being lost from under it - which
	 * would recall a vehicle this call is already moving. Unhooked first, the broadcast finds no vehicle. Production
	 * delivers it a drain later (#436), where either order works; the one that works in both is this one.
	 * The one spelling of "retire where it stands" (there were six, with six different tails).
	 * ENFORCED BY: AirportOps.Fuel.Lifecycle.OwnRetirementsAreNotLosses (arrival home, player despawn, depot removed:
	 * a log spy that sees Warnings, with a control), AirportOps.Fuel.DepotGoneBeforeRecallLeavesNoAgent,
	 * AirportOps.Model.AgentRescue.VehicleDespawn
	 */
	void RetireAgentOf(FServiceVehicle& Vehicle, UGroundTraffic& Traffic);

	/**
	 * The vehicle's agent is already gone from the traffic model: every job it holds goes back to the board and
	 * it is Idle at home with no agent. The body of both ways the board learns of it - OnAgentPhase's Gone branch
	 * and SyncFleet's net - so they cannot leave different vehicles behind. Returns the jobs released. Retires
	 * nothing: it is called from inside the broadcast of the agent's removal, where retiring again would remove
	 * an agent from a list a clear is walking.
	 * ENFORCED BY: AirportOps.Fuel.Lifecycle.AgentRetiredElsewhereRebidsWholeQueue, .LostAgentNetRecallsTheSameWay
	 */
	int32 LoseAgent(FServiceVehicle& Vehicle);

	/**
	 * Move the vehicle's agent to Goal - WAS UFuelService::SendTruckHome, which knew one goal only.
	 * Returns whether it is now on its way (or, for the last-leg case, will turn there on arrival).
	 * See the body for the three starts (no agent, on the road, parked) and why each is different.
	 */
	bool DriveVehicleTo(FServiceVehicle& Vehicle, FGuidelineNodeId Goal, bool bToFacility,
		UGroundTraffic& Traffic, const URoadNetwork& Network);

	/** The vehicle card's line - its kind's NAME (FServiceFleet::NameOf, #430), what it is doing, what it carries, what
	 *  is queued. */
	FString DescribeVehicle(const FServiceVehicle& Vehicle) const;

	/** "to stand 3", "at depot 1" - the one phrase the vehicle card and the depot card share, so the
	 *  two cannot describe the same vehicle two ways. */
	FString VehicleDoing(const FServiceVehicle& Vehicle) const;

	/** A job the vehicle can no longer do goes back to the board, remainder and all. */
	void Reopen(FServiceJob& Job);

	/** Every job Vehicle holds - current and queued - Reopen'd; its CurrentJob released (ToJob or Serving becomes
	 *  Deciding, which the caller must then settle) and Queue emptied. The count, for the caller's log line. */
	int32 ReleaseJobsOf(FServiceVehicle& Vehicle);

	/** What Judge learned about one job, for its refusal and its log line (#103: counted once). */
	struct FJudgement
	{
		int32 Depots = 0;
		int32 DepotsOnRoad = 0;
		/** Real vehicles whose home depot is alive and on a road - NoVehicles when zero. */
		int32 FleetOnRoad = 0;
		bool bStandJoined = false;
		bool bAnyPumpless = false;
		bool bAnyTooLarge = false;
		bool bAnyTooNarrow = false;
		FGuidelineEdgeId NarrowAt;
		FName TooLargeType;
		FName DesignType;
		FGuidelineNodeId Hydrant;
	};

	/** One real vehicle, a candidate for a job: its depot, its kind and its id. */
	struct FCandidate
	{
		FEntityInstanceId Depot;
		FName TypeCode;
		int32 VehicleId = 0;
	};

	/**
	 * THE CANDIDATE FILTER, written once (#443: it was three copies in the two bid passes and CouldServe): the vehicles
	 * of Role that may bid for a job. Every one, less
	 *  - a STRANDED vehicle, when Traffic says who is: it prices itself as "home soon" (ToFacility with no plan left, so no
	 *    drive remaining), wins, and holds the job for a trip it will never make - the wedge OnAgentPhase's Stranded branch
	 *    releases jobs from. CouldServe passes null Traffic: it is asked before the aircraft exists, with no agent to be
	 *    stranded, and answers for the fleet as it stands;
	 *  - Except (a vehicle id, 0 for none): the re-bid asks for the ALTERNATIVES to the vehicle that holds the job.
	 * READS REAL VEHICLES ONLY: the starter fleet is seeded through the fleet's door before any of this runs.
	 * ENFORCED BY: AirportOps.Fuel.CouldServe.StarterDepotVerdictAgreesWithItsFirstBid
	 */
	TArray<FCandidate> CandidatesFor(EServiceRole Role, const UGroundTraffic* Traffic, int32 Except = 0) const;

	/**
	 * WAS ChooseDepot's classification, per candidate instead of per depot, and without "busy": a busy
	 * vehicle bids. Returns the candidates that may bid (their depot joined and pumped, the vehicle no
	 * larger than the stand was built for, a route it fits from its depot to the stand), filling Out
	 * with what the refusal chain needs when none may.
	 */
	TArray<FCandidate> Judge(const URoadNetwork& Network, EServiceRole Role, FEntityInstanceId Stand,
		const TArray<FCandidate>& Candidates, FJudgement& Out) const;

	/** Out's refusal, in the spec's order - the order of the player's hand. */
	static EServiceRefusal RefusalOf(const FJudgement& Out);

	/** The route a vehicle drives from its depot's pose to Goal - the SAME cached plan the eligibility
	 *  judged and the dispatch drives (#301's cache). Invalid when there is none it fits. */
	FRoutePlan DepotRoute(const URoadNetwork& Network, FGuidelineNodeId DepotPose, FGuidelineNodeId Goal,
		const FVehicle& Vehicle, bool* bOutTooNarrow = nullptr, FGuidelineEdgeId* OutNarrowAt = nullptr) const;

	/**
	 * ServiceBid's input for Vehicle taking Job, built from its live state. See the body. QueueAhead
	 * is how many of its queued jobs come first: all of them (INDEX_NONE) for a new bid, the ones
	 * before Job for the finish Job is already promised on this vehicle (the re-bid's "current").
	 */
	ServiceBid::FResult BidFor(const FServiceVehicle& Vehicle, const FServiceJob& Job, const UGroundTraffic& Traffic,
		const URoadNetwork& Network, const USimClock& Clock, int32 QueueAhead = INDEX_NONE) const;

	/**
	 * Every Queued job re-bid against every other vehicle that may take it, and moved when one now
	 * beats its current finish by RebidMarginSeconds (spec §2.5). Underway and Serving never move -
	 * committed is committed. Runs only when FleetRevision (the TRANSITION counter: a re-bid wants every change of a
	 * vehicle's state, not only of the fleet's composition) or the guideline revision moved since the last pass:
	 * nothing changed, nothing re-bid (#190's rule).
	 */
	void RebidQueued(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/** The two clocks RebidQueued last ran against - see RebidQueued. */
	uint32 LastRebidFleetRevision = MAX_uint32;
	uint32 LastRebidGuidelineRevision = MAX_uint32;

	/** Movement seconds from one node to another for Type, over the road length, cached per graph
	 *  revision; negative when there is no road. */
	double DriveSeconds(const URoadNetwork& Network, FGuidelineNodeId From, FGuidelineNodeId To,
		const FServiceVehicleType& Type) const;

	/** Put Job on Vehicle's queue (append only - user's ruling 6), with the promise it was won on. */
	void Assign(FServiceVehicle& Vehicle, FServiceJob& Job, double PromisedFinish);

	FServiceJob* FindJob(int32 JobId);
	const FServiceJob* FindJob(int32 JobId) const;
	FServiceVehicle* FindVehicleMutable(int32 VehicleId);
	FTurnaround* FindTurnaround(int32 AircraftId);

	/** The stand's anchor node for Role, or unset. By ROLE and then by ID, never by index - see
	 *  URoadNetwork::GetAnchorIdsForRole. */
	static FGuidelineNodeId ServiceAnchorOf(const URoadNetwork& Network, FEntityInstanceId Stand, EServiceRole Role);

	/** Home's pose node, or unset if the depot is gone. */
	static FGuidelineNodeId HomePose(const URoadNetwork& Network, const FServiceVehicle& Vehicle);

	/**
	 * TRANSIENT (PR #137 review, issue #105 item 8): jobs and turnarounds NAME AGENTS, and agents are
	 * never saved - see OpsRuntimeTest's own "agents do not survive a load" assertion. Nor do the
	 * aircraft they are for come back: a load re-arms Accepted and Inbound flights only, so a job
	 * restored from a save would be a job for an aircraft that is not there. Non-Transient, they would
	 * be serialized straight into the Fuel blob and restored right over what OnBeforeRestore cleared -
	 * the leak it exists to fix.
	 */
	UPROPERTY(Transient) TArray<FServiceJob> Jobs;
	UPROPERTY(Transient) TArray<FTurnaround> Turnarounds;

	/**
	 * SAVED, unlike the two above (stage 3): the fleet is what the player will BUY, and a load that
	 * lost it would lose property. What a vehicle held that names the session - its agent, its job,
	 * its queue, its timed step - is dropped on the way in (Serialize), so it comes back Idle at home
	 * with what it was carrying. NextVehicleId travels with it, so a vehicle added after a load never
	 * reuses a restored one's id.
	 * ENFORCED BY: AirportOps.Model.Save.FleetSurvivesALoad
	 */
	UPROPERTY() TArray<FServiceVehicle> Vehicles;

	/**
	 * Depots whose STARTER fleet has been seeded, so a depot is seeded once and a vehicle that is out
	 * does not get a twin at home. SAVED since 2026-09-29 (facility-upgrades spec): a starter fleet the
	 * player sold must stay sold across a load, and a transient set came back empty and re-seeded it.
	 * Written only by FServiceFleet (seeding, a test's staging, a load) - rule 43.
	 * ENFORCED BY: AirportOps.Model.Fleet.SoldStarterFleetIsNotReseededAfterLoad
	 */
	UPROPERTY() TSet<FEntityInstanceId> SeededDepots;

	/** Terminal -> Open for one refused job. The ONE body of "ask again", shared by the guideline-revision
	 *  pass in Step and FServiceFleet::Add (ruling C2: two hand copies drift); each caller logs its own reason. A
	 *  change to the jobs with no vehicle transition behind it, so it moves RevisionCount itself. */
	void ReopenRefusedJob(FServiceJob& Job);

	int32 NextJobId = 1;
	UPROPERTY() int32 NextVehicleId = 1;

	/** Unknown vehicle codes already warned about - see TypeFor. */
	mutable TSet<FName> WarnedTypes;

	/** See GetCatalogue. Written by FServiceFleet::ResolveCatalogue alone (Check-Architecture rule 43). */
	UPROPERTY(Transient) TMap<FName, FServiceVehicleType> Catalogue;

	/** Vehicles whose refused dispatch has been warned about, until one of theirs works - see
	 *  DriveVehicleTo. */
	TSet<int32> DispatchRefusedWarned;

	/**
	 * Bumped whenever a vehicle is added or withdrawn (FServiceFleet) AND on every state change of a vehicle - each
	 * FServiceVehicleLifecycle transition moves it (issue #428): dispatched, arrived, serving, deciding, heading
	 * home, off the road, refilling, reset by a load. Before #428 about half of those sites did.
	 * The fleet's half of "has anything changed that a re-bid could answer differently" (stage 2);
	 * the airport's half is URoadNetwork::GetGuidelineRevision. THE TRANSITION COUNTER: what RebidQueued keys on, and
	 * what Revision sums. Its other reader wanted less of it - UFlightBoard::VerdictFor needs only a change of
	 * COMPOSITION, which is FleetCompositionRevision (#443 split them); keying an offer's whole verdict on this one
	 * re-planned every pending offer's arrival whenever a truck arrived or finished a refill.
	 *
	 * A SESSION CLOCK, NOT STATE, the same as URoadNetwork::GuidelineRevision: not a UPROPERTY, and it
	 * does not need to be one.
	 */
	uint32 FleetRevision = 0;

	/**
	 * Bumped only when WHO IS IN THE FLEET changes: a vehicle added or withdrawn (FServiceFleet, whatever the origin),
	 * the fleet cleared or replaced by a load. A vehicle's state, agent, job and cargo are not composition, and
	 * CouldServe reads none of them - only which vehicles exist, of what kind, at which depot (verified in #454's review).
	 * The offer verdict's bFuelServable is dated by it; every composition change also moves FleetRevision, so the re-bid
	 * still hears of it. A session clock, like the one above.
	 * ENFORCED BY: AirportOps.Model.FlightBoard.VehicleTransitionsDoNotReplanOffers, AirportOps.Model.Fleet.OfferVerdictIsDatedByTheFleet
	 */
	uint32 FleetCompositionRevision = 0;

	/** See GetBidCallCountForTest. */
	mutable int32 BidCallCountForTest = 0;

	/** See StepCountForTest. A session counter, not saved. */
	int32 StepCount = 0;

	/** See DepartedLastStep. Transient: one Step's answer. */
	TArray<int32> LastStepDeparted;

	/** See Revision: the changes that move no vehicle. A session counter, not saved. */
	uint32 RevisionCount = 0;

	/** True while any of the turnaround's jobs is neither Done nor Unserviceable. One rule, read by
	 *  DepartTheReady and by HasRefusedDeparture - so the two cannot disagree. */
	bool IsBeingServed(const FTurnaround& Turnaround) const;

	/**
	 * The depot-to-stand route cache, dated by the graph's guideline revision (#301: the shape
	 * ARigTestCourse's identical cache carried alone - see Airside/Model/RoutePlanCache.h for the one
	 * owner both now share). Every bid asks every eligible vehicle's depot-to-stand route, and the same
	 * pair is asked again at every job the stand ever has. MUTABLE: the bid is const.
	 */
	mutable FRoutePlanCache RouteCache;

	/**
	 * Every OTHER leg a bid prices (stand to stand, stand to depot), by length, dated the same way.
	 * NOT RouteCache: a bid only needs the length, and a leg that starts at a service point opens
	 * with a reverse the unseeded search may refuse - the length of the ungated route is a fair
	 * estimate where the plan itself would not be drivable as is.
	 */
	struct FLegKey
	{
		FGuidelineNodeId From;
		FGuidelineNodeId To;
		FName TypeCode;
		bool operator==(const FLegKey& Other) const { return From == Other.From && To == Other.To && TypeCode == Other.TypeCode; }
		friend uint32 GetTypeHash(const FLegKey& Key)
		{
			return HashCombine(HashCombine(GetTypeHash(Key.From), GetTypeHash(Key.To)), GetTypeHash(Key.TypeCode));
		}
	};
	mutable TMap<FLegKey, double> LegLengths;
	mutable uint32 LegLengthsRevision = MAX_uint32;
	mutable const URoadNetwork* LegLengthsNetwork = nullptr;

	/** Role -> policy. Fuel's is FuelPolicy, kept typed so RefillLitresPerMinutePerPump reaches it. */
	TMap<EServiceRole, TSharedRef<IServiceRolePolicy>> Policies;
	TSharedRef<FFuelRolePolicy> FuelPolicy;

	/**
	 * The per-letter vehicle table, indexed by EIcaoCode - see ResolveVehicles for where it is filled
	 * and what it is for now. An FVehicle, not an FAirframe with its climb zeroed - the single
	 * TruckVehicle this replaced made that move on 2026-09-23 (see Model/Vehicle.h).
	 *
	 * A TABLE RESOLVED ONCE, NOT A RESOLVE PER DISPATCH (#104's rule): one resolve per letter at
	 * attach, then a lookup. TRANSIENT: it is a content default resolved every attach, and a saved
	 * copy would pin the figures of whatever content the save was written under.
	 *
	 * A LITERAL 6, NOT LetterCount: UHT parses the dimension and a class constant is not something it
	 * is promised to resolve. JobBoard.cpp static_asserts the two agree.
	 */
	UPROPERTY(Transient) FVehicle VehiclesByLetter[6];
};
