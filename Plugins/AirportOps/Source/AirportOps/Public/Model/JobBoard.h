#pragma once

#include "CoreMinimal.h"
#include "Model/OpsDefinition.h"
#include "Model/Airframe.h"
#include "Model/DeparturePlanner.h"
#include "Model/OpsSave.h"
#include "Model/RoadHandles.h"
#include "Model/RoutePlanCache.h"
#include "Model/RouteSearch.h"
#include "Model/ServiceBid.h"
#include "Model/ServiceJob.h"
#include "Model/ServiceRolePolicy.h"
#include "Model/ServiceVehicle.h"
#include "Model/Vehicle.h"
#include "Solve/IcaoCode.h"
#include "UObject/Object.h"

class USimClock;
#include "JobBoard.generated.h"

class UGroundTraffic;
class URoadNetwork;
class ULedger;
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
 * FUEL IS STILL THE ONLY ROLE BUILT. Everything here is keyed by EServiceRole and asks the role's
 * policy; the fuel-shaped remainder is the refusal texts, the pump check (the policy's
 * NeedsPumpAtHome) and the depot pose role it looks for.
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
	 * Jobs, turnarounds AND vehicles are cleared, not restored from a blob: all three name agents
	 * (AircraftId, a vehicle's AgentId), and UOpsRuntime::LoadFromSlot always clears every agent
	 * before calling OpsSave::Restore - so any id they held is stale the instant a load happens,
	 * whether or not this snapshot even has a Fuel blob. The vehicles come back from the placeholder
	 * fleet on the next tick (SyncFleet), Idle at home, which is exactly where a truck that was out
	 * at save time should reappear. Before the fix this descends from, a truck sent home, then a load,
	 * left its entry in GoingHome forever and its depot one truck short for the rest of the session.
	 */
	virtual void OnBeforeRestore() override;

	/**
	 * LOADING normalises every restored vehicle to Idle at home - see Vehicles. HERE, not in
	 * OnAfterRestore, because OpsSave::RestoreBlob restores one object without calling it, and a
	 * vehicle deserialized "on its way home" with no agent behind it would count as out for ever.
	 */
	virtual void Serialize(FArchive& Ar) override;

	/**
	 * Each fuel vehicle's tank and flow, by FVehicle::TypeCode - copied from UScenario::FuelVehicles
	 * at attach (spec 2026-09-28-fuel-litres). Empty in a bare NewObject; SpecFor then answers
	 * FallbackSpec.
	 */
	UPROPERTY() TMap<FName, FFuelVehicleSpec> VehicleSpecs;

	/** What a vehicle with no entry in VehicleSpecs carries: the trailer's figures. */
	UPROPERTY() FFuelVehicleSpec FallbackSpec = FFuelVehicleSpec(1000.0, 75.0);

	/** Litres per GAME minute per pump module a depot refills a returning vehicle at. Handed to the
	 *  fuel policy every time it is asked, so a test's or the scenario's value is always the one used. */
	UPROPERTY() double RefillLitresPerMinutePerPump = 500.0;

	/** VehicleSpecs[TypeCode], or FallbackSpec - with a Warning once per unknown code. */
	FFuelVehicleSpec SpecFor(FName TypeCode) const;

	/**
	 * The litres a parked aircraft asks for - the flight's own FuelLitres, drawn at its offer.
	 * UOpsRuntime::Attach points this at the flight board; unset, or for an agent no flight owns,
	 * DefaultLitres. 0 means it wants no fuel.
	 * ENFORCED BY: AirportOps.Fuel.RuntimeWiresLitresOwed
	 */
	TFunction<double(int32 AgentId, const FAirframe& Airframe)> LitresOwedFor;

	/** The one fallback load: 70% of the tank - the middle of the offer's 50-90% draw. */
	static double DefaultLitres(const FAirframe& Airframe) { return FMath::Max(Airframe.FuelCapacityLitres, 0.0) * 0.7; }

	/**
	 * True when Depot can fuel at all - it has a pump, or it predates modules entirely.
	 *
	 * A depot placed WITHOUT a plot has an empty module list and fuels as it always did. That is not
	 * a claim it has a pump; it is that the question does not apply, and a save written before plots
	 * existed must keep working.
	 */
	static bool HasWorkingPump(const FEntityInstance& Depot);

	/** Pump modules at Depot, or 1 for a plotless depot - see HasWorkingPump. The refill rate's multiplier. */
	static int32 PumpsAt(const FEntityInstance& Depot);

	/** How many ICAO letters the vehicle table holds, A-F. See VehiclesByLetter. */
	static constexpr int32 LetterCount = 6;

	/**
	 * Fill every letter's table entry from Resolve. UOpsRuntime::Attach fills it with
	 * UAirsideSettings::ResolveStandDesignVehicle, next to VehicleSpecs above; nothing on the dispatch
	 * path resolves content, and cannot: this is Model/, and reaching Content/ was the only
	 * Model->Content edge in either plugin (#104). A TFunctionRef rather than the resolve itself for
	 * the same reason, and so the world-free fixture fills the table through the one loop too.
	 *
	 * WHAT THE TABLE IS FOR NOW: the vehicle CATALOGUE (every distinct TypeCode in it is a kind of
	 * vehicle a depot can have - see FleetTypes and TypeFor) and the design-vehicle fallback for a
	 * stand whose definition carries none. It no longer decides which vehicle a stand is SENT: the
	 * bid does, among the vehicles the depots actually have.
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
	 * The kinds of vehicle the PLACEHOLDER FLEET gives a depot (spec §3.4): DefaultFleetTypes if set,
	 * else every distinct TypeCode in the letter table (today the utility tow and the bowser).
	 *
	 * A PLACEHOLDER BECAUSE THE PLAYER WILL BUY THE FLEET (user, 2026-09-28) and that is not in the
	 * game yet. A depot with Trucks = N gets N of each; purchase will add to the same list.
	 */
	TArray<FName> FleetTypes() const;

	/** Override for FleetTypes. Empty in production. A test that needs "the depot's only vehicle is
	 *  the bowser" says so here rather than contriving a table. */
	UPROPERTY() TArray<FName> DefaultFleetTypes;

	/** The resolved row for TypeCode: its chassis from the letter table, its figures from SpecFor. */
	FServiceVehicleType TypeFor(FName TypeCode) const;

	/**
	 * Could a depot fuel this airframe on some stand it would take - asked BEFORE the aircraft exists,
	 * for the offer row (spec 2026-09-28 section 3).
	 *
	 * THE SAME ELIGIBILITY the live bid applies, per admitting stand (StandAdmission::Judge), so the
	 * row's "fuel" chip and the truck that does or does not come cannot disagree. Busy vehicles count
	 * as servable: they bid with their queue. The cost is a route search per (depot, type, stand); the
	 * board calls it only when its verdict's revisions move - see FOfferVerdict.
	 */
	bool CouldServe(const URoadNetwork& Network, const FAirframe& Airframe) const;

	/**
	 * Every phase change in the traffic model - the events this class is driven by.
	 *
	 * An aircraft reaching Parked at a stand makes a turnaround and its jobs; a vehicle's agent
	 * reaching Parked is that vehicle ARRIVING, which moves the vehicle's own state; an aircraft
	 * LEAVING Parked drops its turnaround and jobs and moves any vehicle out for them on.
	 */
	void OnAgentPhase(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock,
		int32 AgentId, EAgentPhase From, EAgentPhase To);

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
	 * whose dispatch was refused, one parked at a stand with no decision (the final review #1
	 * backstop), a job still Open (a re-offered one is bid on the NEXT pass - see the body), or a due
	 * turnaround that could not leave. False means nothing here changes until an event or a deadline
	 * says so - which is what lets the ops bus run this as a pass rather than every frame (spec
	 * 2026-09-29-ops-event-bus section 2; UOpsRuntime::WireBus's "JobBoard" pass).
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
	 * The player's Unstick for a VEHICLE (spec 2026-09-29-unstick-agent): every job it holds - the
	 * one it is on and its whole queue - goes back to the board for another vehicle to win, then it
	 * heads home (GoToFacility) or, bRetire, is retired where it stands and is Idle at home at once.
	 * THE EXPLICIT FORM of what SyncFleet's "lost its agent" branch does a tick late - and that branch
	 * reopens only the current job, leaving the queue on a vehicle nobody is driving.
	 * False, nothing changed, when AgentId drives no vehicle of this board.
	 * ENFORCED BY: AirportOps.Model.AgentRescue.VehicleSendHome, .VehicleDespawn
	 */
	bool RecallVehicleOfAgent(int32 AgentId, bool bRetire, UGroundTraffic& Traffic, const URoadNetwork& Network,
		const USimClock& Clock);

	/**
	 * How far behind Depot is (user, 2026-09-28: "see how far behind your depot is in jobs"): every job
	 * on its vehicles - under way, being served, or queued - when the last of them is promised to
	 * finish, and how many of those promises land after their aircraft's turnaround ends, which is the
	 * backlog actually costing the airport. Read off each job's PromisedFinish, which the re-bid pass
	 * refreshes on every trigger (RebidQueued), so the card needs no bookkeeping of its own. Minutes are
	 * whole, so the inspector's text gate redraws at most once a game minute.
	 */
	FDepotBacklog DescribeDepot(FEntityInstanceId Depot, double Now) const;

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

	/** See FleetRevision. */
	uint32 GetFleetRevisionForTest() const { return FleetRevision; }

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

	/** The role's policy. Fuel is registered at construction; a test may register another. */
	const IServiceRolePolicy* PolicyFor(EServiceRole Role) const;
	void RegisterPolicyForTest(TSharedRef<IServiceRolePolicy> Policy);

private:
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
	 * first time gets Trucks x FleetTypes() vehicles, Idle at home and full; a vehicle whose depot is
	 * gone is withdrawn (its agent retired, its jobs back to the board); a vehicle whose agent vanished
	 * under it (retired by somebody else) is put back Idle at home, its job re-opened.
	 */
	void SyncFleet(UGroundTraffic& Traffic, const URoadNetwork& Network);

	/** Every Open job bid and assigned, or refused. See BidFor and Judge. */
	void AssignOpenJobs(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/**
	 * Decide the vehicle's next step now that it has none: its queue head direct, or its facility
	 * first (the policy's NextStep), or home when the queue is empty. The ONE place the lifecycle
	 * moves forward, so every path out of a step - serve done, refill done, recall, a job gone - reads
	 * the same rule the bid priced.
	 */
	void StartNext(FServiceVehicle& Vehicle, UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/** A vehicle's agent reached Parked. */
	void OnVehicleArrived(FServiceVehicle& Vehicle, const FRoadAgent& Agent, UGroundTraffic& Traffic,
		const URoadNetwork& Network, const USimClock& Clock);

	/** The aircraft left its stand: its turnaround and jobs go, and vehicles out for them move on. */
	void DropAircraft(int32 AircraftId, UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/** A trip's pumping is over: quantities move, the job is Done or re-opened with its remainder. */
	void FinishServe(FServiceVehicle& Vehicle, const USimClock& Clock);

	/** Head for the facility (home), or - if the vehicle cannot get there - be Idle at home. */
	void GoToFacility(FServiceVehicle& Vehicle, UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/** At the facility with no agent: time the refill, or finish it at once when there is none. */
	void BeginFacility(FServiceVehicle& Vehicle, const URoadNetwork& Network, const USimClock& Clock);

	/**
	 * Move the vehicle's agent to Goal - WAS UFuelService::SendTruckHome, which knew one goal only.
	 * Returns whether it is now on its way (or, for the last-leg case, will turn there on arrival).
	 * See the body for the three starts (no agent, on the road, parked) and why each is different.
	 */
	bool DriveVehicleTo(FServiceVehicle& Vehicle, FGuidelineNodeId Goal, bool bToFacility,
		UGroundTraffic& Traffic, const URoadNetwork& Network);

	/** The vehicle card's line - its kind, what it is doing, what it carries, what is queued. */
	FString DescribeVehicle(const FServiceVehicle& Vehicle) const;

	/** "to stand 3", "at depot 1" - the one phrase the vehicle card and the depot card share, so the
	 *  two cannot describe the same vehicle two ways. */
	FString VehicleDoing(const FServiceVehicle& Vehicle) const;

	/** A job the vehicle can no longer do goes back to the board, remainder and all. */
	void Reopen(FServiceJob& Job);

	/** Every job Vehicle holds - current and queued - Reopen'd; its CurrentJob and Queue emptied. The
	 *  count, for the caller's log line. */
	int32 ReleaseJobsOf(FServiceVehicle& Vehicle);


	/** What Judge learned about one job, for its refusal and its log line (#103: counted once). */
	struct FJudgement
	{
		int32 Depots = 0;
		int32 DepotsOnRoad = 0;
		bool bStandJoined = false;
		bool bAnyPumpless = false;
		bool bAnyTooLarge = false;
		bool bAnyTooNarrow = false;
		FGuidelineEdgeId NarrowAt;
		FName TooLargeType;
		FName DesignType;
		FGuidelineNodeId Hydrant;
	};

	/** One vehicle kind at one depot, a candidate for a job - a real vehicle, or (CouldServe) one the
	 *  placeholder fleet would give it. */
	struct FCandidate
	{
		FEntityInstanceId Depot;
		FName TypeCode;
		int32 VehicleId = 0;
	};

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
	 * committed is committed. Runs only when FleetRevision or the guideline revision moved since the
	 * last pass: nothing changed, nothing re-bid (#190's rule).
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

	/** Depots SyncFleet has already given their placeholder fleet, so a depot is seeded once and a
	 *  vehicle that is out does not get a twin at home. */
	UPROPERTY(Transient) TSet<FEntityInstanceId> SeededDepots;

	int32 NextJobId = 1;
	UPROPERTY() int32 NextVehicleId = 1;

	/** Unknown vehicle codes already warned about - see SpecFor. */
	mutable TSet<FName> WarnedSpecs;

	/** Vehicles whose refused dispatch has been warned about, until one of theirs works - see
	 *  DriveVehicleTo. */
	TSet<int32> DispatchRefusedWarned;

	/**
	 * Bumped whenever a vehicle's availability changes: a step ends, a vehicle is added or withdrawn.
	 * The fleet's half of "has anything changed that a re-bid could answer differently" (stage 2);
	 * the airport's half is URoadNetwork::GetGuidelineRevision.
	 *
	 * A SESSION CLOCK, NOT STATE, the same as URoadNetwork::GuidelineRevision: not a UPROPERTY, and it
	 * does not need to be one.
	 */
	uint32 FleetRevision = 0;

	/** See GetBidCallCountForTest. */
	mutable int32 BidCallCountForTest = 0;

	/** See StepCountForTest. A session counter, not saved. */
	int32 StepCount = 0;

	/** True while any of the turnaround's jobs is neither Done nor Unserviceable. One rule, read by
	 *  DepartTheReady and by Step's "is a departure waiting?" - so the two cannot disagree. */
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
