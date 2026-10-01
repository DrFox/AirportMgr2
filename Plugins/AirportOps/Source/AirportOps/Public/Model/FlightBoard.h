#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/ArrivalQueue.h"
#include "Model/OfferInbox.h"
#include "Model/OpsSave.h"
#include "Model/RoadEntity.h"
#include "UObject/Object.h"

#include "FlightBoard.generated.h"

class UArrivalSequencer;
class UFlight;
class UJobBoard;
class UPricing;
class UGroundTraffic;
class UOfferGenerator;
class URoadNetwork;
class UStandAllocator;
class ULedger;
class FOpsEventBus;
class USimClock;
enum class EAgentPhase : uint8;
struct FAgentTransition;
enum class ECancelReason : uint8;
enum class EFlightPhase : uint8;
struct FTransitionCause;

// FOfferVerdict and FArrivalQuote live in OfferInbox.h, FQueueTick in ArrivalQueue.h (#442 item 4) - each beside the owner that
// makes it. Included above, so every reader of this header still sees all three.

/**
 * Every live flight, and the ONLY thing that dispatches an arrival.
 *
 * ONE DOOR. The inbox and key 7 both come through here, because two doors onto arrival is
 * how this codebase has shipped three lists-that-must-agree bugs - see CLAUDE.md, "Check
 * where a list is CONSUMED". A second caller of DispatchArrival would put an aeroplane on
 * the field that no flight owns, and nothing would ever take its stand back.
 *
 * ARoadNetworkActor::DispatchArrival is reached through the Dispatcher seam rather than
 * called directly, so the schedule can be proved to fire without a UWorld. Everything in
 * this class is world-free by construction; if a change here needs a world, it belongs in
 * Present/.
 *
 * THE REGISTRY AND THE TRANSITION OWNER, AND THE DOOR ONTO TWO OWNERS (#442 item 4). It carried seven jobs - the inbox's countdown
 * and lapse, the verdict cache, accept/decline/cancel, the queue/clearance/dispatch, the agent-phase mapping, billing and the load -
 * in 1803 + 954 lines. What stays here is the flight registry (Flights, History, the indices, the ids, the save), the ONE writer of a
 * phase (TransitionTo) with every door that names a transition (the cancels, OnAgentPhase's mapping, the load's steps), and the quote
 * both owners ask (PlanQuote, Gated). FOfferInbox owns the offers - countdown, verdicts, lapse, accept, decline; FArrivalQueue owns
 * the arrivals - the clock's handles, the queue, the clearances, the dispatch and every re-hold; and BILLING is a reaction to the phase
 * on the ops bus (FlightBilling, FFlightPhaseChangedEvent) that this board no longer calls. Each owner's public names stay here as
 * forwarders, so no caller changed. See each owner's class comment for the pattern and its forced deviations.
 * ENFORCED BY: Check-Architecture rule 77 (FlightBoard.cpp's line budget), rule 96 (what the owners may reach of this board),
 * AirportOps.Present.FlightBoardOwnersAreWired, AirportOps.Present.Bus.BillingIsWired
 */
UCLASS()
class AIRPORTOPS_API UFlightBoard : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	// --- IOpsPersistent ---------------------------------------------------------------
	/** "Flights": the name OpsSave.h's shim already expects for a pre-v4 save's bytes. */
	virtual FName SaveBlobName() const override { return TEXT("Flights"); }
	virtual UObject& AsPersistentObject() override { return *this; }

	/**
	 * Rebuild the lookups a restore does not carry. It migrated old snapshots too - the v2 focus copy and
	 * #188's sweep of terminal flights out of Flights - until the owner ruling of 2026-09-30: since #452 (v6) a pre-v6
	 * blob restores no flights, so neither could reach a real save.
	 *
	 * AN IOpsPersistent HOOK so that OpsSave::Restore stays a plain loop over every persistent object rather than
	 * naming this class as a parameter and calling it in one particular position.
	 */
	virtual void OnAfterRestore(int32 SnapshotVersion) override;

	/**
	 * RETIRES EVERY FLIGHT BEFORE ANY RESTORE, blob or none (#426 (b)). Serialize retires the flights a load replaces,
	 * but OpsSave only calls it when the snapshot HAS a "Flights" blob; one without (a v1 save, from before the board)
	 * restores no flights, and must not leave the replaced session's in the inbox. UJobBoard::OnBeforeRestore's reason.
	 * ENFORCED BY: AirportOps.Model.Save.NoFlightsBlobRetiresTheBoard
	 */
	virtual void OnBeforeRestore() override;

	/**
	 * THE FLIGHTS, BY VALUE (#425). Flights and History are Transient, so the tagged-property pass skips them, and this
	 * writes each flight's own tagged properties inline after the board's; a load re-creates every one as a NEW UFlight
	 * owned by this board. The "Flights" blob used to hold only their PATHS - OpsSave's proxy archive writes an object
	 * reference as its path - which in the same session re-found the LIVE flight (a load restored its state now, not at
	 * the save) and in the next session found nothing (every flight dropped, silently: every loop null-checks).
	 *
	 * A CUSTOM Serialize AND NOT A USTRUCT RECORD, the issue's other option: UFlight stays a UObject, so every reader of
	 * one - the viewmodels' weak pointers, the inspector's lookup, ById/ByAgent, the sequencer, the job board's litres
	 * lookup - is untouched, where a record would have changed every one of them for the save's sake. And each flight
	 * is still written by its OWN reflection, so a UPROPERTY added to UFlight is saved with no edit here - OpsSave's rule.
	 *
	 * A LOAD RETIRES THE FLIGHTS IT REPLACES (MarkAsGarbage), rebuilds the indices and moves Revision - HERE, not in
	 * OnAfterRestore, for UJobBoard::Serialize's reason: OpsSave::DeserializeObject restores a board without calling it.
	 * A reader still holding a pre-load flight then reads null rather than the pre-load state, and every viewmodel keyed
	 * on Revision re-reads the board.
	 * ENFORCED BY: AirportOps.Model.FlightSave.RestoresByValue, AirportOps.Model.FlightSave.RestoreRetiresReplacedFlights
	 */
	virtual void Serialize(FArchive& Ar) override;

	/**
	 * How many game days a terminal flight (Declined, Expired, Departed, Cancelled, Withdrawn) is kept in History
	 * before RollUp forgets it outright.
	 *
	 * DISCARDED, NOT FOLDED like ULedger::RollUp's BroughtForward entry: a flight has no
	 * summable amount to fold into a stand-in, and the money it earned already lives on
	 * permanently in ULedger's own rows (see PostLandingFee/PostParkingFee) - keeping a copy
	 * here would only be a second, decaying record of the same fact.
	 */
	UPROPERTY() int32 MaxDays = 30;

	/**
	 * What actually puts an aeroplane in the world. UOpsRuntime::Attach points this at
	 * ARoadNetworkActor::DispatchArrival; tests substitute a recorder.
	 *
	 * A TFunction and not an interface, because there is exactly one production
	 * implementation and it is a forwarder on an actor - an interface would be a class per
	 * call site, for one call site.
	 *
	 * Not a UPROPERTY and deliberately not saved: it is wiring, re-made on every Attach.
	 */
	TFunction<bool(const FVector2D& Near, const FAirframe& Airframe)> Dispatcher;

	/**
	 * Whether the airport admits arrivals now - asked by Accept, which every accept comes through (the inbox,
	 * and key 7 via AcceptImmediate) - AND BY TickQueue, the door onto the runway itself (whole-stack review I1: the
	 * entry doors alone let a flight a load or a plant left holding land at a closed airport). A CLOSED AIRPORT ADMITS
	 * NOTHING (ruling I1, 2026-09-30). A predicate, not a UAirport pointer, so the board still does not learn the
	 * airport (see CancelUnarrived). Set by UOpsRuntime's constructor; unset in a bare NewObject, which admits.
	 * ENFORCED BY: AirportOps.Present.Airport.AcceptRefusedWhileClosed, AirportOps.Present.ArrivalQueue.ClosedAirportDispatchesNothing
	 */
	TFunction<bool()> AdmitsArrivals;

	/**
	 * Bumped whenever anything a viewmodel displays has changed - an offer added, accepted,
	 * declined or expired, a phase change, a graph rebuild's re-apply.
	 *
	 * REPLACES A DELEGATE THIS CLASS USED TO CARRY (issue #169): OnChanged fired from nearly
	 * every method here - AddOffer, Accept, Decline, DispatchNow, OnAgentPhase, OnGraphRebuilt (RestoreStandHolds since #442)
	 * - documented as "a coarse go-re-read-everything signal a C++ viewmodel polls off of",
	 * but nothing ever bound to it: UOfferInboxViewModel::Refresh polled on TICK instead, and
	 * asked WhyNotAcceptable - a full ArrivalPlanner::Plan, a route search per stand per exit -
	 * for every row, every frame, whether or not the board had moved. A signal with zero
	 * subscribers cannot be told from a signal with none needed; a REVISION can, because the
	 * poller compares it to the number it last saw rather than trusting that something rang.
	 *
	 * SAME IDIOM AS ULedger::Revision AND URoadNetwork::GetGuidelineRevision, and for the same
	 * reason given there: the cheapest question a poller can ask is "has anything changed
	 * since the number I remember". A plain session counter, not a UPROPERTY - nothing saves a
	 * revision, and nothing should: a fresh load starts every cache one frame away from a
	 * correct recompute, which is what "invalid until proven otherwise" already means for a
	 * viewmodel with no cached answer yet.
	 */
	uint32 Revision() const { return RevisionCount; }

	/**
	 * THE WIRING, TRANSIENT - this and the five pointers below (#425). Each is one of UOpsRuntime's own subobjects, set
	 * once by its constructor, and none is state. Saved, each was a PATH to that subobject, which a later session's
	 * load resolved to null - silently, since null is a working state for every one of them: with no Allocator,
	 * Accept refuses every offer. A test's own board wires them by hand, and a load now leaves them as wired.
	 * ENFORCED BY: Tools/Check-Architecture.ps1 rule 37 (persistent-refs-transient), AirportOps.Model.FlightSave.RestoresByValue
	 */
	UPROPERTY(Transient) TObjectPtr<UStandAllocator> Allocator = nullptr;

	/** Who of the holding flights is cleared next. Null = strict first come. See UArrivalSequencer. Read by FArrivalQueue::Tick:
	 *  the board keeps the wiring, the queue the state the policy is over (#442 item 4). */
	UPROPERTY(Transient) TObjectPtr<UArrivalSequencer> Sequencer = nullptr;
	UPROPERTY(Transient) TObjectPtr<UOfferGenerator> Generator = nullptr;

	/**
	 * The money, or null in a test that does not care about it. Set by UOpsRuntime's constructor (#425; was Attach).
	 *
	 * NULL IS A WORKING STATE, not a bug to guard against at every call: dozens of existing
	 * board tests drive flights through their whole lifecycle and have no interest in fees, and
	 * making them all construct a ledger would be churn for nothing.
	 *
	 * WHAT THE BILLING REACTION POSTS TO (#442 item 4): FlightBilling::OnFlightPhaseChanged reads it from the board the runtime's
	 * "Billing" handler hands it - the board itself posts nothing.
	 */
	UPROPERTY(Transient) TObjectPtr<ULedger> Ledger = nullptr;

	/**
	 * What things cost, for the inbox row's fee text (OfferViewModels formats the flight's LandingFee through it). THE BOARD NO
	 * LONGER PRICES ANYTHING WITH IT since #442: PostParkingFee used to ask it for the rate at departure, and the rate is the
	 * flight's now (UFlight::ParkingRatePerHour, fixed at the offer).
	 */
	UPROPERTY(Transient) TObjectPtr<UPricing> Pricing = nullptr;

	/**
	 * Asked whether the airport could fuel an offer, for FOfferVerdict::bFuelServable. Null in
	 * a test that does not care - fuel then reads as servable. Set by UOpsRuntime's constructor (#425; was Attach).
	 */
	UPROPERTY(Transient) TObjectPtr<UJobBoard> Fuel = nullptr;

	/**
	 * Where this board publishes what happened to its flights - an offer lapsing or declined, a flight
	 * airborne. Set by UOpsRuntime::Attach. NULL IS A WORKING STATE, for Ledger's reason above: every
	 * publish checks. Raw: the runtime owns both this board and the bus.
	 */
	FOpsEventBus* Bus = nullptr;

	/**
	 * Bank the landing fee this flight was OFFERED at, into Ledger. FORWARDS to FlightBilling::PostLandingFee (#442 item 4), where
	 * the rule lives. Idempotent - a flight lands once.
	 *
	 * PUBLIC so a test can post a fee without driving a whole agent through its phases - and since #442 item 4 only a test
	 * does: play bills through the reaction (FlightBilling::OnFlightPhaseChanged), which posts through FlightBilling's own
	 * PostLandingFee, not this. Kept as the forwarder the refactor contract asks for, so no caller changed.
	 * ENFORCED BY: Check-Architecture rule 4 ('landing and parking fees posted outside FlightBilling')
	 */
	void PostLandingFee(double Now, UFlight& Flight);

	/** Bank the parking fee for the hours actually occupied, at the rate the flight was OFFERED at (UFlight::
	 *  ParkingRatePerHour), and record it on the flight. FORWARDS to FlightBilling::PostParkingFee (#442 item 4). */
	void PostParkingFee(double Now, UFlight& Flight);

	/**
	 * The DEFAULT runway preference for the next generated offer (was ApproachFocus, #442) - UOpsRuntime writes it before
	 * calling UOfferGenerator::TickMinute, which copies it onto every flight it builds.
	 *
	 * NOT consulted by Accept, WhyNotAcceptable or DispatchNow: those read UFlight::
	 * RunwayPreference, which is fixed on the flight at the offer and travels with it. This
	 * field used to be read there too, and whichever caller wrote it LAST decided every
	 * later offer's answer - see UFlight::RunwayPreference and issue #96.
	 */
	UPROPERTY() FVector2D RunwayPreference = FVector2D::ZeroVector;

	/**
	 * The longest runway's threshold, for the next generated offer to be ordered from. False, and
	 * OutFocus untouched, when the airport has no runway yet.
	 *
	 * MOVED OUT OF UOpsRuntime (issue #98): choosing a runway is a pure function of the
	 * graph, the same kind of decision ArrivalPlanner::Plan makes, and Present/ may hold no
	 * logic of its own - see OpsRuntime.h's own header. THE LONGEST RUNWAY, not wherever the
	 * land key last looked: the planner still ORDERS the runways by distance to this point (#412 made it plan
	 * them all, so it no longer picks one), and that order decides whose refusal is reported and which of two equal
	 * runways wins - an offer generated with a stale or default point would report a refusal for whichever strip happens to
	 * sit nearest it, and then be accepted against a different one. (Named DefaultApproachFocus until #442.)
	 *
	 * BOOL AND AN OUT-PARAMETER, not FVector2D::ZeroVector on "none": zero is a valid
	 * threshold, and a caller that cannot tell "no runway" from "a runway starting at the
	 * origin" would overwrite a perfectly good RunwayPreference with a false one the moment
	 * every runway was removed - see CLAUDE.md, "honour the return of anything that fills an
	 * out-parameter."
	 */
	static bool DefaultRunwayPreference(const URoadNetwork& Network, FVector2D& OutFocus);

	/**
	 * Takes ownership of an offer and gives it the next id if it has none. Its countdown is
	 * OfferSecondsLeft, drained by TickOffers - nothing goes on the clock.
	 */
	void AddOffer(USimClock& Clock, UFlight* Offer);

	// --- THE OFFERS: FORWARDERS to FOfferInbox (#442 item 4), where each rule and its reasons live ----------------------------

	/** Drain every offer's real-seconds countdown and lapse what reaches zero. FORWARDS to FOfferInbox::TickOffers. */
	void TickOffers(const UGroundTraffic& Traffic, const URoadNetwork& Network,
		const USimClock& Clock, double RealDeltaSeconds);

	/** Can this offer be accepted right now, and can the airport serve it - CACHED. FORWARDS to FOfferInbox::VerdictFor. */
	const FOfferVerdict& VerdictFor(const UGroundTraffic& Traffic, const URoadNetwork& Network,
		const UFlight& Flight) const;

	/** The id the next offer should carry. The board owns numbering; see UOfferGenerator. */
	int32 TakeNextId();

	/** WHERE EVERY ACCEPT IS DECIDED (#431): the quote, then the plan's stand held and the arrival armed. FORWARDS to
	 *  FOfferInbox::TryAccept. */
	FArrivalQuote TryAccept(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock, UFlight& Flight);

	/** TryAccept, answered yes or no - the tests' spelling. A FORWARDER, so nothing accepts a second way. */
	bool Accept(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock,
		UFlight& Flight);

	/** The cached plan verdict, then the airport's gate - what TryAccept asks. FORWARDS to FOfferInbox::QuoteFor. */
	FArrivalQuote QuoteFor(const UGroundTraffic& Traffic, const URoadNetwork& Network, const UFlight& Flight) const;

	/** The same quote for an arrival that is not a flight yet, UNCACHED - the Land panel's. FORWARDS to FOfferInbox::QuoteArrival. */
	FArrivalQuote QuoteArrival(const UGroundTraffic& Traffic, const URoadNetwork& Network, const FAirframe& Airframe,
		const FVector2D& Focus) const;

	/** Retires the offer at once; free. FORWARDS to FOfferInbox::Decline. */
	void Decline(USimClock& Clock, UFlight& Flight);

	/**
	 * The flight whose aeroplane the player is about to despawn ends CANCELLED, into history -
	 * spec 2026-09-29-unstick-agent. CALLED BEFORE UGroundTraffic::RetireAgent, never after: this
	 * unhooks the agent, so the Gone that follows finds no flight and cannot book it Departed
	 * (FlightPhaseFromTransition books Retired as a departure, which for a retired aeroplane is a lie).
	 * No money moves: fees already posted stay posted. False when the agent flies no flight.
	 * ENFORCED BY: AirportOps.Model.AgentRescue.AircraftDespawnCancelsFlight
	 */
	bool CancelByAgent(int32 AgentId, double Now);

	/**
	 * The airport stopped being open (spec 2026-09-29-ops-batch3 §3): everything not yet committed to the
	 * runway is called off. Accepted - its arrival disarmed on Clock, its stand hold released - and Inbound -
	 * out of the queue, stand released - become Cancelled, each publishing FFlightCancelledEvent with Reason.
	 * Offered becomes Withdrawn and publishes nothing (no OfferExpired, so no Ignored penalty). Landing and
	 * later are untouched: on the runway or on the ground, they finish. Returns how many were cancelled.
	 *
	 * TAKES THE REASON, NOT THE AIRPORT STATUS: the board does not learn the airport; UOpsRuntime maps one to
	 * the other. And the traffic and the clock, because a hold and an arrival are released through them -
	 * Accept's own signature.
	 * ENFORCED BY: AirportOps.Model.FlightBoard.CancelUnarrivedCancelsAndWithdraws
	 */
	int32 CancelUnarrived(UGroundTraffic& Traffic, USimClock& Clock, ECancelReason Reason);

	/** Accepted plus Inbound: what CancelUnarrived would cancel now - the close confirm's "N flights". */
	int32 UnarrivedCount() const;

	/** Landing through Departing: the aircraft a closed airport is still draining - the bar's readout. */
	int32 OnGroundCount() const;

	/**
	 * THE PLAYER CANCELS ONE FLIGHT THAT HAS NOT ARRIVED (#442): Accepted or Inbound, to Cancelled and History, its stand
	 * hold released and its arrival disarmed (the Cancelled row of TransitionTo), publishing FFlightCancelledEvent with
	 * ECancelReason::PlayerCancelled - which the airline roster charges the SAME per-flight ClosureCancelPenalty a closure
	 * does. A HOLDING FLIGHT THAT CAN NEVER LAND had no way out before but closing the airport, which cancels every flight and
	 * is penalised per flight - the very loss this takes on alone; the FlightCannotLand alert is where the player is offered it.
	 *
	 * OPEN OWNER QUESTION, the penalty: should cancelling an UNLANDABLE flight cost at all? The airline did not lose it to
	 * the player's choice so much as to the layout the player drew - but a free cancel is a way to shed any accepted flight at
	 * no cost, and the owner has not ruled. Charged for now, at the closure's rate, so the two exits cost alike.
	 *
	 * False, and nothing changed, for an unknown id or a flight that is not still to arrive (Landing and later is committed
	 * to the runway - a despawn is CancelByAgent's, through the aircraft card). Takes the traffic and the clock because a hold
	 * and an arrival are released through them - CancelUnarrived's signature.
	 * ENFORCED BY: AirportOps.Model.FlightBoard.CancelledRowIsOneRowForEveryDoor (stand released, reason published),
	 * AirportOps.Model.FlightBoard.PlayerCancelRefusesWhatHasNotToCancel, AirportOps.Model.Airlines.PlayerCancelCostsTheClosurePenalty
	 */
	bool CancelByPlayer(UGroundTraffic& Traffic, USimClock& Clock, int32 FlightId);

	/** WHY A HOLDING FLIGHT CAN NEVER LAND, or None - read off the queue's cached clearance, never planned here. FORWARDS to
	 *  FArrivalQueue::UnlandableWhy (#442 item 4), where the rule and its reasons live. */
	EArrivalRefusal UnlandableWhy(const UFlight& Flight, const URoadNetwork& Network) const;

	/** Make a flight from an airframe, aim it at Focus, and accept it on the spot - key 7. FORWARDS to
	 *  FOfferInbox::AcceptImmediate. */
	EArrivalRefusal AcceptImmediate(UGroundTraffic& Traffic, const URoadNetwork& Network,
		USimClock& Clock, const FAirframe& Airframe, const FVector2D& Focus, FText Airline, FString* OutSentence = nullptr);

	/** Why this offer could not be accepted this instant, or None - the real plan. FORWARDS to FOfferInbox::WhyNotAcceptable. */
	EArrivalRefusal WhyNotAcceptable(const UGroundTraffic& Traffic, const URoadNetwork& Network,
		const UFlight& Flight) const;

	/**
	 * How many times WhyNotAcceptable has actually run this session.
	 *
	 * WHAT ISSUE #169's TEST MEASURES: a route search per stand per exit is not free, and the
	 * whole point of gating UOfferViewModel's cache on Revision/GetGuidelineRevision/
	 * OccupancyRevision is that a quiet inbox calls this ZERO times a frame, not once per row.
	 * A test that only checked the CACHED ANSWER was still correct could not tell a cache from
	 * no cache at all - this counts the expensive call itself.
	 */
	int32 GetWhyNotAcceptableCallsForTest() const { return WhyNotAcceptableCallsForTest; }

	/** How many flights RollUp has ever forgotten outright. See RollUp and the test that
	 *  measures History staying bounded across many simulated days. */
	int32 GetHistoryCountForTest() const { return History.Num(); }

	/**
	 * THE MAINTAINED INDEX production reaches through the private FindByAgent/FindById -
	 * OnAgentPhase calls the first, the Schedule clock callback the second.
	 * Exposed only so a test can compare its answer against FindByAgentLinearForTest /
	 * FindByIdLinearForTest's O(n) scan - see issue #188.
	 */
	UFlight* FindByAgentForTest(int32 AgentId) const { return FindByAgent(AgentId); }

	/** The flight an agent flies, or null - what UOpsRuntime hands UJobBoard::LitresOwedFor. */
	const UFlight* FlightForAgent(int32 AgentId) const { return FindByAgent(AgentId); }
	UFlight* FindByIdForTest(int32 Id) const { return FindById(Id); }

	/**
	 * The flight with this id, live or in History until RollUp forgets it, or null - what the billing reaction resolves an
	 * FFlightPhaseChangedEvent's FlightId through (#442 item 4): an event names a flight by id, never by a UObject that may be
	 * gone by the time the queue drains. Mutable, because billing writes what the flight was paid (bLandingFeePaid, ParkedAt).
	 */
	UFlight* FlightById(int32 Id) const { return FindById(Id); }

	/**
	 * THE ORACLE those two are checked against: an O(n) scan of Flights (and, for an id,
	 * History too - see FindById's own comment on why an id keeps answering after the flight
	 * has gone terminal). Kept expressly so a maintained index that drifted from Flights/
	 * History fails a test rather than just costing more, the same reason ULedger keeps
	 * FoldBalanceForTest beside its own cache.
	 */
	UFlight* FindByAgentLinearForTest(int32 AgentId) const;
	UFlight* FindByIdLinearForTest(int32 Id) const;

	/**
	 * TAKES THE CLOCK because the phase changes made here are dated, and a change that could not say when it happened would
	 * leave the billing reaction nothing to price a fee by (FFlightPhaseChangedEvent::At) and the roll-up and the determinism
	 * test nothing to age. The sibling UJobBoard::OnAgentPhase already takes one, so this is the neighbouring shape rather
	 * than a second way of getting at the time.
	 *
	 * NO TRAFFIC MODEL since #436: the flight's phase follows the transition's Cause (FlightPhaseFromTransition) and
	 * the stand it parked on is its GoalAtEvent. It used to take the model to read the live agent - Phase, GoalNode,
	 * bDepartureArmed - a drain after the change, and an unused parameter is an invitation to read it again.
	 *
	 * NO MONEY since #442 item 4: the fees it posted after every event it heard are FlightBilling's reaction to the phase change
	 * this makes (FFlightPhaseChangedEvent), a round later in the same drain.
	 */
	void OnAgentPhase(const URoadNetwork& Network, const USimClock& Clock, const FAgentTransition& Transition);

	/** A LOAD'S STAND HOLDS, step 3 of RestoreAfterLoad. FORWARDS to FArrivalQueue::RestoreStandHolds (#442 item 4), where every
	 *  re-hold now lives. Public for the tests that pin a load's holds on their own. */
	void RestoreStandHolds(UGroundTraffic& Traffic, const URoadNetwork& Network, const TArray<UFlight*>& HoldLast = TArray<UFlight*>());

	/**
	 * THE FLIGHT HALF OF A LOAD, IN ITS ONE ORDER (issue #426). Four steps, each correct only in its position, which
	 * UOpsRuntime::LoadFromSlot used to call one by one and AirportOps.Model.FlightSave.MidFlightGoesRoundOrRetires
	 * re-typed by hand:
	 *
	 *   1. DemoteRestoredMidFlight - agents are never saved, so a flight saved landing goes round and one on the ground
	 *      retires. FIRST: every later step reads the phases it leaves.
	 *   2. CancelUnarrivedAtLoad, when !bAirportAdmits - BEFORE the holds and the re-arm, so no stand is held and no
	 *      arrival armed for a flight that can never land. It goes through the same Cancelled row a closure does (#442), which
	 *      releases and disarms when given the traffic and the clock - here there is nothing yet to release, but it no longer
	 *      depends on that. Unscored and unpublished (rulings I2/I1): see its own comment.
	 *   3. RestoreStandHolds(re-queued last) - the genuine holds first, then the re-queued flights' (review I1). AFTER the
	 *      network's rebuild, which took every claim with it - so this is called once the load's AdoptNetwork has run.
	 *   4. RearmSchedules - the clock's queue was never saved.
	 *   (0. Before all of them, every arrival the REPLACED session armed is cancelled on the clock and forgotten - the map
	 *   that holds them is not saved, and a restored flight can carry an id the old handle was booked under. So step 2 finds
	 *   nothing armed, literally, and so does a load with no traffic model, which never reaches step 4.)
	 *
	 * NOT THROUGH CancelUnarrived FOR STEP 2, though the issue asked for "the same transitions as live play": the live
	 * cancel publishes FFlightCancelledEvent, which the airline roster scores, and withdraws offers - both ruled against
	 * for a load (the closure's own cancellations were scored when it happened). It shares the Cancelled ROW, whose Load
	 * source is what publishes nothing. The re-queue of step 1 DOES go through Enqueue, the live door into the queue, so it
	 * announces itself as any arrival joining the queue does.
	 *
	 * bAirportAdmits is the airport's status after the load's silent re-derivation (UAirport::Reseat) - a bool, not
	 * the UAirport, so this board still does not learn the airport (see AdmitsArrivals).
	 *
	 * Traffic MAY BE NULL (an actor with no traffic model): steps 1 and 2 still run - they need no model, and before
	 * this function the load ran them regardless - and steps 3 and 4, which hold stands on it and dispatch to it, are
	 * skipped with a Warning.
	 * ENFORCED BY: AirportOps.Model.FlightSave.MidFlightGoesRoundOrRetires, AirportOps.Present.RuntimeLoad.MidFlightRequeuesOrRetires,
	 * AirportOps.Present.RuntimeLoad.MidFlightAtClosedAirport, Check-Architecture rule 4 (steps 1 and 2 have no caller outside this board)
	 */
	void RestoreAfterLoad(UGroundTraffic* Traffic, const URoadNetwork& Network, USimClock& Clock, bool bAirportAdmits);

	/**
	 * #404: a load's flights whose aeroplanes were not saved. Landing/TaxiIn go round again - Inbound, HoldingSince
	 * Now, at the back of the queue (through Enqueue); Turnaround..Departing retire as Departed, unscored. AgentId
	 * cleared either way. Returns the re-queued flights. Step 1 of RestoreAfterLoad - not from OnAfterRestore (review
	 * ruling M5), which is handed no clock and runs for every restore, agents cleared or not. Public for the tests that
	 * pin one step's rule on its own; production calls it through RestoreAfterLoad only.
	 * ENFORCED BY: Check-Architecture rule 4 (allowed callers)
	 */
	TArray<UFlight*> DemoteRestoredMidFlight(double Now);

	/**
	 * A load at an airport that is not open: every flight still to arrive - Accepted, or Inbound (the re-queued among
	 * them) - can never land, since a closed airport admits no arrivals. Cancelled, to History, UNSCORED (nothing
	 * published): whatever closed the airport happened before the save, and its own cancellations were scored then.
	 * Was CancelRequeued, the re-queued flights only (PR C review ruling I2); widened by the whole-stack review (I1),
	 * which found an Accepted flight saved at a closed airport still due to land after the load. Returns how many.
	 * Step 2 of RestoreAfterLoad; public for DemoteRestoredMidFlight's reason.
	 *
	 * GIVEN THE TRAFFIC AND THE CLOCK WHEN THE LOAD HAS THEM (#442), so it releases a stand and disarms an arrival through
	 * the same Cancelled row a closure's cancel does, rather than being correct only because of its position in the load:
	 * called where nothing is held or armed yet they find nothing to do, and called after the holds and the re-arm (a
	 * reordered load) they still leave none. Unscored and unpublished either way - that is the row's Load source.
	 * ENFORCED BY: Check-Architecture rule 4 (allowed callers), AirportOps.Model.FlightSave.LoadCancelDoesNotDependOnItsPosition
	 */
	int32 CancelUnarrivedAtLoad(double Now, UGroundTraffic* Traffic = nullptr, USimClock* Clock = nullptr);

	/** Re-arm the clock for every Accepted flight's arrival - CALLED AFTER A LOAD, and not optional. FORWARDS to
	 *  FArrivalQueue::RearmSchedules (#442 item 4). */
	void RearmSchedules(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock);

	/**
	 * Forgets any History entry older than MaxDays game-days.
	 *
	 * SAME DAILY BEAT AS ULedger::RollUp - see UOpsRuntime::PostDailyUpkeep, the one
	 * production caller. A separate schedule of its own would be a second timer to keep in
	 * step with the ledger's for no reason: both fold once a day and nothing here needs finer
	 * granularity than that.
	 */
	void RollUp(double Now);

	/**
	 * Offers still awaiting an answer, the least time left first (ties keep arrival order).
	 *
	 * SORTED, not arrival order, since windows became per airline (spec 2026-09-28): a
	 * 45-second offer that arrived after a 120-second one is the one the player must see first.
	 */
	TArray<UFlight*> Offers() const;

	/** The flights holding for the runway, in the order they joined - derived, never stored. FORWARDS to FArrivalQueue::Queue. */
	TArray<UFlight*> Queue() const;

	/** Clear at most one holding flight whose runway is free, and dispatch it - the "ArrivalQueue" pass's one call. FORWARDS to
	 *  FArrivalQueue::Tick (#442 item 4). Rule 33 reads THIS name: the pass alone calls it in production.
	 *  ENFORCED BY: AirportOps.Model.ArrivalQueue.DueWhileBusyWaitsThenLands, Check-Architecture rule 33 (queue-is-a-pass) */
	FQueueTick TickQueue(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/** A NEW FRAME FOR ONE CLEARANCE A FRAME (#445) - the queue's own rule. FORWARDS to FArrivalQueue::BeginFrame.
	 *  ENFORCED BY: AirportOps.Present.ArrivalQueue.SecondRunwayNextFrame, AirportOps.Model.FlightBoard.OneClearanceAFrameIsTheQueuesRule */
	void BeginQueueFrame() { Arrivals.BeginFrame(); }

	/** How many times TickQueue has run. For the runtime-wiring test. */
	int32 TickQueueCallsForTest() const { return Arrivals.TickCalls(); }

	/** Everything accepted and not yet departed. */
	TArray<UFlight*> Live() const;

	/**
	 * O(1): a maintained counter, not Offers().Num(). ISSUE #188 NAMED THIS CALL DIRECTLY - a
	 * count has no reason to cost an allocation and a full scan just to read its length, which
	 * is all Offers().Num() was ever doing here.
	 */
	int32 PendingOfferCount() const { return OfferedCount; }

	/** How many times TickOffers has copied Flights to walk them - its early-out's counter. */
	int32 OfferSnapshotCountForTest() const { return Inbox.OfferSnapshotCount(); }

private:
	/**
	 * THE TWO OWNERS (#442 item 4), held by value - see each one's class comment. FRIENDS, as FTurnarounds is UJobBoard's: an
	 * owner changes a phase only through TransitionTo, moves the one RevisionCount, and walks Flights; what else it may reach is
	 * held by Check-Architecture rule 96, not by access (a friend sees everything). Declared before the members they read, so a
	 * reader of this section meets the owners first.
	 */
	friend class FOfferInbox;
	friend class FArrivalQueue;
	FOfferInbox Inbox;
	FArrivalQueue Arrivals;

	/**
	 * Every flight not yet in a terminal phase: offered, accepted, or anywhere between landing
	 * and departing. TERMINAL flights (Declined, Expired, Departed, Cancelled, Withdrawn) are moved into History the
	 * moment they get there - see MoveToHistory - rather than staying here forever, which is
	 * what made FindByAgent, FindById, Offers(), Live() and every save cost O(every flight
	 * this session has ever seen) instead of O(what is actually happening) - issue #188.
	 *
	 * TRANSIENT BUT SAVED - by value, in Serialize (#425). The tagged pass would write only each flight's path.
	 */
	UPROPERTY(Transient) TArray<TObjectPtr<UFlight>> Flights;

	/**
	 * Terminal flights MoveToHistory has retired, in the order they arrived here.
	 *
	 * BOUNDED BY RollUp, which forgets anything older than MaxDays on the same daily beat as
	 * ULedger::RollUp. SAVED like Flights (see IOpsPersistent's class comment: a model
	 * object's non-Transient UPROPERTYs ARE its saved state) - now that it is bounded, keeping
	 * it in the save is cheap, and a player who just watched a flight leave should still find
	 * it if a "recent departures" view ever reads this. Transient since #425 for Flights' reason:
	 * Serialize writes it by value, which the tagged pass (a path per flight) never did.
	 */
	UPROPERTY(Transient) TArray<TObjectPtr<UFlight>> History;

	UPROPERTY() int32 NextFlightId = 1;

	/** See Revision. Not a UPROPERTY - a session counter, not state a save would ever need. */
	uint32 RevisionCount = 0;

	/** See GetWhyNotAcceptableCallsForTest. MUTABLE: WhyNotAcceptable is const, and counting a
	 *  call is bookkeeping about it, not a change to what the board holds. */
	mutable int32 WhyNotAcceptableCallsForTest = 0;

	/**
	 * FindByAgent's and FindById's O(1) answer (issue #188 item 2).
	 *
	 * ByAgent is maintained at the sites that set or clear UFlight::AgentId: DispatchNow adds,
	 * OnAgentPhase's Gone branch removes. ById is maintained at AddOffer (a flight gets its id
	 * once, there) and stays populated across the Flights -> History move in MoveToHistory - a
	 * clock callback keyed on an id must keep finding its flight for as long as the flight
	 * exists, which is exactly as long as RollUp has not yet forgotten it. RollUp is the one
	 * place an entry actually leaves this map.
	 *
	 * NOT UPROPERTYs: a restored flight's own Id/AgentId fields are the saved truth, and these
	 * are rebuilt from Flights and History in OnAfterRestore - the same split FArrivalQueue's
	 * ArrivalHandles already uses for the clock's handles. (And in Serialize's load, since #425: the flights they
	 * pointed at are replaced there, and a map left naming the pre-load objects would answer with them.)
	 */
	TMap<int32, TObjectPtr<UFlight>> ByAgent;
	TMap<int32, TObjectPtr<UFlight>> ById;

	/** PendingOfferCount's O(1) answer. Maintained at every entry into and exit from the
	 *  Offered phase; not a UPROPERTY, rebuilt in OnAfterRestore like the maps above. */
	int32 OfferedCount = 0;

	/**
	 * THE ONE WRITER OF UFlight::Phase, AND THE OWNER OF WHAT A PHASE CHANGE DOES (#442). Thirteen sites wrote the phase and
	 * each chose its own subset of {the pending-offer count, the clock handle, the stand hold, the agent hooks, a bus
	 * publish, the move to History, the revision}: the three writers that cancelled a flight did three different things
	 * (CancelUnarrived disarmed, released and published; CancelByAgent unhooked and published; the load's cancel did
	 * neither, and was right only because of where LoadFromSlot calls it). Now the (from, to) row says it once.
	 *
	 * THE ROWS, by the phase entered (see the switch for each's comment):
	 *   Accepted   - arms the arrival; publishes FOfferAcceptedEvent.
	 *   Inbound    - HoldingSince = Cause.At; publishes FFlightInboundEvent (the queue pass's wake-up).
	 *   Landing    - hooks the aeroplane (Cause.AgentId).
	 *   Departing  - stamps AirborneAt once; publishes FFlightAirborneEvent.
	 *   Declined / Expired - publish FOfferDeclinedEvent / FOfferExpiredEvent; into History.
	 *   Withdrawn  - into History, publishing nothing (no Ignored penalty: nobody let it lapse).
	 *   Cancelled  - from Accepted/Inbound releases the stand; publishes FFlightCancelledEvent(Cause.CancelReason) unless
	 *                the source is a Load; into History.
	 *   Departed   - into History.
	 * AND THE EFFECTS THAT DEPEND ON THE PHASE LEFT, not the one entered: leaving Offered drops the pending-offer count,
	 * leaving Accepted disarms the arrival (FArrivalQueue::Disarm), leaving the ground lets go of the aeroplane (unhooks ByAgent
	 * and AgentId). EVERY CHANGE bumps the revision once, AND PUBLISHES FFlightPhaseChangedEvent once (#442 item 4) - what the
	 * billing reaction hears. A change to the phase the flight is already in is nothing, and says nothing.
	 *
	 * WHAT IS NOT HERE, on purpose: the LOGS stay at each door (they carry the door's own context - the agent, the closure's
	 * reason), the money is FlightBilling's REACTION to the publish above (it was OnAgentPhase's, inline, until #442 item 4 -
	 * billing is its own job), and DispatchNow's stand release stays before its dispatch (the planner must not see the hold), so
	 * the Landing row does not release.
	 *
	 * THE SWITCH ON THE PHASE ENTERED IS EXHAUSTIVE (AIRSIDE_EXHAUSTIVE_SWITCH): a new phase is a build error here, at the
	 * one place that must say what entering it does.
	 * ENFORCED BY: C4062 as an error around the body, Check-Architecture rule 57 (nothing else writes the phase), and
	 * AirportOps.Model.FlightBoard.TransitionTable (one test per row)
	 */
	void TransitionTo(UFlight& Flight, EFlightPhase To, const FTransitionCause& Cause);

	/** The test door onto TransitionTo, for the one thing no production door can reach: a Play-source Cancelled with no reason
	 *  (every door names one). Defined in the test module - a friend by name, FRoadNetworkTestAccess's shape - so production has no
	 *  second way in: rule 57 reads FlightBoard.cpp's Phase writes, and this grants no write of the field. */
	friend struct FFlightBoardTestAccess;

	/** Cancel every arrival the clock holds for a flight and forget them, with the verdict and clearance caches keyed on the same
	 *  ids: RearmSchedules's first half, and a load's step 0. Both owners' share - FArrivalQueue::DisarmEvery, FOfferInbox::ForgetAll. */
	void DisarmEveryArrival(USimClock& Clock);

	/**
	 * Retires Flight out of the live list: stamps TerminatedAt, moves it Flights -> History,
	 * and drops its ByAgent entry if it still had one. THE ONE PLACE a flight leaves Flights,
	 * so every terminal transition goes through it rather than call sites each remembering their own
	 * piece of the move (issue #188) - and since #442 through TransitionTo's terminal rows, which call it.
	 * ENFORCED BY: Check-Architecture rule 57 (no call of MoveToHistory outside TransitionTo's body)
	 */
	void MoveToHistory(UFlight& Flight, double Now);

	/**
	 * Rebuilds ByAgent, ById and OfferedCount from scratch off Flights and History. Called
	 * from Serialize's load (#425), OnAfterRestore AND RearmSchedules - see RearmSchedules's own comment on why a
	 * board deserialised without going through OpsSave::Restore still needs this before its
	 * re-armed clock callbacks can find anything by id.
	 */
	void RebuildIndices();

	/**
	 * Every flight in Flights and History marked garbage and dropped - a load's replacement, see Serialize for why
	 * RETIRED and not merely forgotten. One body for its two callers, Serialize's load and OnBeforeRestore, so the no-
	 * blob case (#426 (b)) retires exactly what a blob's load does. Returns how many.
	 */
	int32 RetireEveryFlight();

	/**
	 * THE PLAN HALF OF A QUOTE - ArrivalPlanner::Plan for Airframe at Focus, a busy runway queued (an accepted flight
	 * waits for it, spec 2026-09-28-arrival-queue), ExcludingHolder's own stand hold not counting against it - with the
	 * plan's sentence and the stand it taxis to. Counted in WhyNotAcceptableCallsForTest: it IS the expensive call.
	 * VerdictFor caches it for a flight; QuoteArrival asks it uncached for an arrival that is not a flight yet. ON THE BOARD,
	 * NOT AN OWNER (#442 item 4): the inbox's accept and the queue's re-hold both ask it, so neither may own it.
	 */
	FArrivalQuote PlanQuote(const UGroundTraffic& Traffic, const URoadNetwork& Network, const FAirframe& Airframe,
		const FVector2D& Focus, int32 ExcludingHolder) const;

	/** THE GATE HALF: a plan's yes turned into NotAdmitted, worded, while the airport admits no arrivals (AdmitsArrivals).
	 *  After the plan, so a refusal the plan already names (no runway) is reported as the plan's. */
	FArrivalQuote Gated(FArrivalQuote Quote) const;

	/** O(1) via ByAgent/ById. CONST because a lookup does not change what the board holds -
	 *  which also lets FindByAgentForTest/FindByIdForTest above call them on a const board. */
	UFlight* FindByAgent(int32 AgentId) const;
	UFlight* FindById(int32 Id) const;
};
