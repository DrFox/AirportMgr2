#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
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

/**
 * Whether an offer can be accepted right now, and whether the airport can serve it.
 *
 * CACHED ON FOUR REVISIONS, TWO ANSWERS (issue #169, moved from UOfferViewModel 2026-09-28; split by #443). Why is a
 * full ArrivalPlanner::Plan - a route search over every stand, then every runway exit - so it
 * is recomputed only when something it depends on has moved: the board itself
 * (UFlightBoard::Revision - added/accepted/declined/expired), the guideline graph
 * (URoadNetwork::GetGuidelineRevision - an edit changed the taxiways), or occupancy
 * (UGroundTraffic::OccupancyRevision - a stand claimed or freed, a runway taken or cleared). bFuelServable is
 * CouldServe, which reads the airport's shape, the fleet's COMPOSITION and which of its vehicles are STRANDED (it is
 * handed the traffic model to ask, #443 - a stranded vehicle never counts) and nothing else: it is dated by the
 * guideline graph and by UJobBoard::GetFleetCompositionRevision (a vehicle added, withdrawn, seeded by the "FleetSeed"
 * pass, sold or a load replacing the fleet, and a vehicle's agent stranding or moving again), and by NEITHER the board
 * (the airframe is the flight's own) NOR occupancy (it judges no traffic's whereabouts) NOR any vehicle state but being
 * stranded. Before #443 the whole verdict hung on the counter that moves on every vehicle TRANSITION
 * (dispatched, arrived, serving, refilled - #428 made each one bump it), so a truck arriving or finishing a refill
 * re-planned every pending offer's full arrival plan, which reads no fleet. A few integer compares replace the
 * search on every frame where none of them moved.
 * AND THE NETWORK ITSELF, AND ITS EDIT REVISION (#471), the way FLandChoicesKey is keyed: since #431 the verdict's stand
 * is the one an accept HOLDS, so a stale yes is no longer a wrong row but a wrong hold. A drag moves the road (EditRevision)
 * a whole gesture before the guideline graph catches up at the drop, and a pre-drag yes served from the cache let the inbox
 * accept onto a graph the planner itself refuses mid-edit (GraphBeingEdited). A clear or a load is a NEW network counting
 * its revisions from zero, so equal numbers on another object are not the same graph - weak, so a recycled address is not
 * mistaken for the old network either.
 * ENFORCED BY: AirportOps.Model.FlightBoard.VehicleTransitionsDoNotReplanOffers,
 * AirportOps.Fuel.CouldServe.StrandingMovesTheCompositionAndTheVerdict (the stranded clause),
 * AirportOps.Model.FlightBoard.VerdictIsDatedByTheEdit, AirportOps.Model.FlightBoard.VerdictNamesTheNetwork
 */
struct FOfferVerdict
{
	EArrivalRefusal Why = EArrivalRefusal::None;
	/** The plan's own sentence for Why - ArrivalPlanner::DescribeRefusal(Plan), with its figures and its admission - or
	 *  empty when None. The row shows THIS, not the reason-only wording, which reads "not admitted to that runway" for
	 *  an arrivals-only field whose real reason is that nothing can take the departure (#456 review). */
	FString Sentence;
	/** The stand the plan taxis to when Why is None - what TryAccept holds (#431). Unset otherwise. */
	FEntityInstanceId Stand;

	/** Could a depot fuel this airframe on a stand it would take? True when nothing checks
	 *  (no fuel service wired, as in most board tests). A MISSING service does not block the
	 *  accept (spec ruling 5) - the row says so and C scores it. */
	bool bFuelServable = true;

	/** What Why was judged at. The guideline stamp and the network date bFuelServable too: an edit re-judges both. */
	uint32 BoardAt = 0;
	uint32 GuidelineAt = 0;
	uint32 OccupancyAt = 0;
	/** URoadNetwork::GetEditRevision when Why was judged - the plan's alone: CouldServe reads only the derived graph, which
	 *  a drag does not move until the drop's guideline revision, and its answer holds nothing. */
	uint32 EditAt = 0;
	/** The network both answers were judged on - compared, never dereferenced; weak, so a new network at a recycled address is not this one. */
	FWeakObjectPtr Network;
	/** UJobBoard::GetFleetCompositionRevision when bFuelServable was judged - a vehicle bought, sold, seeded (by the
	 *  "FleetSeed" pass) or withdrawn changes it (facility-upgrades spec), and so does a vehicle's agent stranding or moving
	 *  again (#443: a stranded vehicle does not count), but a vehicle changing any other STATE does not (#443: this was the
	 *  transition counter, which moved on every one and re-planned every offer's Why with it).
	 *  ENFORCED BY: AirportOps.Model.Fleet.OfferVerdictIsDatedByTheFleet,
	 *  AirportOps.Fuel.CouldServe.StrandingMovesTheCompositionAndTheVerdict */
	uint32 FleetAt = 0;
	bool bValid = false;
};

/**
 * THE ANSWER TO "MAY THIS ARRIVAL BE ACCEPTED NOW" (#431, #432): the arrival plan (ArrivalPlanner::Plan, a busy runway
 * queued), then the airport's gate (UAirport::AdmitsArrivals) - one evaluator for the inbox, key 7 and the Land panel.
 * Why, the sentence the refusal is worded with (the plan's own, or the gate's), and the stand an accept would hold.
 * Plain C++: an answer, not state.
 */
struct FArrivalQuote
{
	EArrivalRefusal Why = EArrivalRefusal::NoRunway;
	FString Sentence;
	FEntityInstanceId Stand;
	bool IsAccepted() const { return Why == EArrivalRefusal::None; }
};

/**
 * What one UFlightBoard::TickQueue did - read by UOpsRuntime's "ArrivalQueue" pass, which decides from it whether to
 * look again (bRetry), whether the safety net stays armed (Waiting) and whether a safety run found work nobody's event
 * covered (Cleared). Plain C++: a pass result, not state.
 */
struct FQueueTick
{
	/** Flights holding when the tick began, cleared one included. Counted paused too: holding is not paused. */
	int32 Waiting = 0;
	/** The flight cleared to land this tick, or null - at most one (ONE CLEARANCE A FRAME, see TickQueue). */
	UFlight* Cleared = nullptr;
	/** The sequencer chose a flight and the dispatch refused it - a same-frame race, retried next frame. */
	bool bRetry = false;
	/** The airport admits no arrivals (AdmitsArrivals): nothing was cleared, and nothing can be until it opens -
	 *  so the pass keeps no safety net ticking for the flights still Waiting (whole-stack review I1). */
	bool bClosed = false;
	/**
	 * A flight was already cleared in this queue frame (BeginQueueFrame), so nothing was decided: the pass asks again next frame
	 * (#445 - the one-clearance-a-frame rule is the queue's own, and this is how it says "not yet" without the runtime counting
	 * frames). Waiting and the rest of this result are NOT filled - a deferred tick looked at nothing - so the pass leaves its
	 * safety-net wish as it was.
	 */
	bool bDeferred = false;
};

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

	/** Who of the holding flights is cleared next. Null = strict first come. See UArrivalSequencer. */
	UPROPERTY(Transient) TObjectPtr<UArrivalSequencer> Sequencer = nullptr;
	UPROPERTY(Transient) TObjectPtr<UOfferGenerator> Generator = nullptr;

	/**
	 * The money, or null in a test that does not care about it. Set by UOpsRuntime's constructor (#425; was Attach).
	 *
	 * NULL IS A WORKING STATE, not a bug to guard against at every call: dozens of existing
	 * board tests drive flights through their whole lifecycle and have no interest in fees, and
	 * making them all construct a ledger would be churn for nothing.
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
	 * Bank the landing fee this flight was OFFERED at. Idempotent - a flight lands once.
	 *
	 * PUBLIC so a test can post a fee without driving a whole agent through its phases; the
	 * production caller is OnAgentPhase, and there is only the one.
	 */
	void PostLandingFee(double Now, UFlight& Flight);

	/** Bank the parking fee for the hours actually occupied, at the rate the flight was OFFERED at (UFlight::
	 *  ParkingRatePerHour), and record it on the flight. */
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

	/**
	 * Drain every offer's REAL-seconds countdown and lapse the ones that reach zero.
	 *
	 * REAL SECONDS, AND ONLY WHILE UNPAUSED (spec 2026-09-28 ruling 3). It replaced a
	 * Clock.At callback at a GAME-time ExpiresAt, which made the window shrink with the speed
	 * setting: 600 game seconds was eight real seconds at x1. A plain seconds-left field also
	 * saves as itself, so there is no load-time re-arm to get wrong.
	 *
	 * EACH OFFER'S VERDICT IS REFRESHED HERE TOO (see VerdictFor), which is what lets a lapse
	 * say whether the player could ever have taken it - UFlight::LapseReason - with no inbox
	 * open. Called from UOpsRuntime::Tick; tests call it directly.
	 */
	void TickOffers(const UGroundTraffic& Traffic, const URoadNetwork& Network,
		const USimClock& Clock, double RealDeltaSeconds);

	/**
	 * Can this offer be accepted right now, and can the airport serve it - CACHED.
	 *
	 * MOVED HERE FROM UOfferViewModel (issue #169's cache): the board is what classifies a
	 * lapse, so it has to know the answer whether or not a row is on screen, and one cache
	 * both the board and the row read is one evaluator rather than two. Recomputed only when
	 * the board, the guideline graph or the occupancy has moved since - see FOfferVerdict.
	 */
	const FOfferVerdict& VerdictFor(const UGroundTraffic& Traffic, const URoadNetwork& Network,
		const UFlight& Flight) const;

	/** The id the next offer should carry. The board owns numbering; see UOfferGenerator. */
	int32 TakeNextId();

	/**
	 * WHERE EVERY ACCEPT IS DECIDED (#431). QuoteFor - the cached plan verdict, then the airport's gate - and on a yes,
	 * the stand THAT PLAN taxis to is held (UStandAllocator::Hold) and the arrival goes on the clock. Returns the
	 * quote: None and the held stand, or the refusal it actually hit, worded.
	 *
	 * BEFORE THIS, Accept was the airport's status plus UStandAllocator::Reserve - the smallest fitting stand, reachable
	 * or not - while the inbox, the lapse classifier and the queue asked ArrivalPlanner::Plan. Two evaluators: key 7
	 * accepted flights the planner refused (no exit, no route) and they held for ever, AcceptImmediate reported a refusal
	 * by asking the OTHER evaluator, and a hold could sit on a stand nothing could taxi to while the flight waited for
	 * the one it could.
	 *
	 * A refusal before the plan - not an offer, no stand allocator, no airframe - is NotAdmitted with a sentence saying
	 * which, and logged: a caller's bug or a fixture's, never a player's. So is the gate's closure. NotAdmitted because
	 * EArrivalRefusal (the planner's enum, in Airside) has no value for either: it is the nearest reason, and the quote's
	 * Sentence carries the real one - a reader of Why alone must not word it.
	 * ENFORCED BY: AirportOps.Model.FlightBoard.AcceptHoldsTheReachableStand, AirportOps.Present.LandWithNoRouteHoldsNothing
	 */
	FArrivalQuote TryAccept(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock, UFlight& Flight);

	/** TryAccept, answered yes or no - the tests' spelling. A FORWARDER, so nothing accepts a second way. */
	bool Accept(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock,
		UFlight& Flight);

	/**
	 * VerdictFor's plan answer, then the airport's gate - what TryAccept asks, and the inbox row shows. The gate is NOT in
	 * the cached verdict: the lapse classifier reads the verdict alone, and a flight that lapses while the player has
	 * closed the airport was ignored, not unacceptable.
	 * ENFORCED BY: AirportOps.Model.Offers.Countdown.LapseReadsThePlanNotTheGate
	 */
	FArrivalQuote QuoteFor(const UGroundTraffic& Traffic, const URoadNetwork& Network, const UFlight& Flight) const;

	/**
	 * The same quote for an arrival that is not a flight yet - Airframe, aimed at Focus - UNCACHED. What the Land panel
	 * asks per type (UOpsRuntime::QuoteLanding, #432): the SAME plan and the SAME gate AcceptImmediate's accept will
	 * ask, so a row the panel lights is a click the game takes.
	 */
	FArrivalQuote QuoteArrival(const UGroundTraffic& Traffic, const URoadNetwork& Network, const FAirframe& Airframe,
		const FVector2D& Focus) const;

	/** Retires the offer at once; free (spec ruling 8) - only a lapse will cost anything, in C. */
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

	/**
	 * WHY A HOLDING FLIGHT CAN NEVER LAND, or None (#442): the refusal its cached clearance (ClearanceFor - the plan minus
	 * the runway, which TickQueue asks) holds, when that refusal is one the player must build or change something to clear
	 * (ArrivalPlanner::IsPermanentRefusal). What the FlightCannotLand alert is derived from. An ACCEPTED flight is judged too
	 * (#445; JudgeUnarrived, the queue pass's own - the plan with no occupancy, the question the offer generator asks), so the alert
	 * does not wait for its ETA. None for a flight that is neither Accepted nor Inbound, that the queue has not judged yet, whose refusal will
	 * clear on its own, or whose cached judgement is OLDER THAN THE GUIDELINE GRAPH.
	 *
	 * READS THE CACHE AND NEVER FILLS IT: an alert pass that planned would be a route search per holding flight per
	 * recompute. And it trusts the cache only while it is dated by Network's guideline revision: a judgement older than the graph says nothing
	 * - not a guess either way - so after the player fixes the airport its old "no exit" cannot keep the alert up. The queue pass keeps the cache
	 * CURRENT (JudgeUnarrived re-dates every unarrived flight on a graph change, runway busy or clock paused or not), so "older than the graph"
	 * is the window between an edit and the pass that runs for it - inside one drain, the pass running before the alerts'.
	 * ENFORCED BY: AirportOps.Model.Alerts.UnlandableHoldingFlightRaisesAnAlert,
	 * AirportOps.Model.Alerts.FixedAirportClearsTheAlertBehindABusyRunway,
	 * AirportOps.Model.Alerts.HoldingFlightAlertSurvivesAnUnrelatedEditBehindABusyRunway
	 */
	EArrivalRefusal UnlandableWhy(const UFlight& Flight, const URoadNetwork& Network) const;

	/**
	 * Make a flight from an airframe, aim it at Focus, and accept it on the spot - the debug
	 * land key's whole job, and previously done by hand at the call site (issue #96).
	 *
	 * Its lead time is zero, so ArrivesAt is Clock.Now(): this exists to put an aeroplane on the field
	 * THIS SECOND, not to queue a normal offer. Focus travels onto the flight itself - see
	 * UFlight::RunwayPreference - so it never has to touch the board's own field, which the
	 * generator also writes and would otherwise fight over.
	 *
	 * Returns EArrivalRefusal::None on success, or the refusal TryAccept HIT - returned by the gate that refused, not
	 * re-derived by asking the plan again (#431: a closure, a null allocator or a span-0 airframe used to come back as
	 * None, "success"). OutSentence, when given, receives its words. The flight is left in the inbox on refusal, exactly
	 * as a generated offer nobody could accept yet is.
	 */
	EArrivalRefusal AcceptImmediate(UGroundTraffic& Traffic, const URoadNetwork& Network,
		USimClock& Clock, const FAirframe& Airframe, const FVector2D& Focus, FText Airline, FString* OutSentence = nullptr);

	/**
	 * Why this offer could not be accepted this instant, or EArrivalRefusal::None.
	 *
	 * The REAL ArrivalPlanner::Plan against the live occupancy, so the inbox's greyed-out
	 * reason is the same sentence the arrival itself would print - ArrivalPlanner::
	 * DescribeRefusal renders it. A second, cheaper guess here would be a second source of
	 * truth about whether an aeroplane can land.
	 */
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
	 * THE ORACLE those two are checked against: an O(n) scan of Flights (and, for an id,
	 * History too - see FindById's own comment on why an id keeps answering after the flight
	 * has gone terminal). Kept expressly so a maintained index that drifted from Flights/
	 * History fails a test rather than just costing more, the same reason ULedger keeps
	 * FoldBalanceForTest beside its own cache.
	 */
	UFlight* FindByAgentLinearForTest(int32 AgentId) const;
	UFlight* FindByIdLinearForTest(int32 Id) const;

	/**
	 * TAKES THE CLOCK because the fees posted here are dated, and a ledger entry that could not
	 * say when it happened would break the roll-up and the determinism test both. The sibling
	 * UJobBoard::OnAgentPhase already takes one, so this is the neighbouring shape rather
	 * than a second way of getting at the time.
	 *
	 * NO TRAFFIC MODEL since #436: the flight's phase follows the transition's Cause (FlightPhaseFromTransition) and
	 * the stand it parked on is its GoalAtEvent. It used to take the model to read the live agent - Phase, GoalNode,
	 * bDepartureArmed - a drain after the change, and an unused parameter is an invitation to read it again.
	 */
	void OnAgentPhase(const URoadNetwork& Network, const USimClock& Clock, const FAgentTransition& Transition);

	/**
	 * A LOAD'S STAND HOLDS, step 3 of RestoreAfterLoad: re-make every Accepted and Inbound flight's hold from its saved
	 * UFlight::Stand (UStandAllocator::Reapply), HoldLast's after all the others, then ReconcileStandHolds over them in
	 * that order - a refused one given up and re-held, an Inbound flight with no stand (or a gone one) re-held. HoldLast
	 * is the load's re-queued flights (DemoteRestoredMidFlight).
	 *
	 * IT WAS OnGraphRebuilt, named for a rebuild it no longer serves: since PR D's review (I1) an edit's rebuild keeps
	 * the holds inside Airside, and the load is this one's only caller (#442). NOT AN EDIT'S REACTION: an edit's refusal
	 * reaches ReconcileStandHolds through the queue pass the edit's FNetworkChangedEvent runs.
	 */
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

	/**
	 * Re-arm the clock for every Accepted flight's arrival.
	 *
	 * CALLED AFTER A LOAD, and it is not optional: USimClock deliberately does not save its
	 * callback queue, so a restored flight has an ETA and nothing armed. Without this an
	 * accepted flight never arrives - and nothing anywhere says so. Offers need nothing: their
	 * countdown is a saved seconds-left figure TickOffers resumes (snapshot v5).
	 */
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

	/**
	 * The flights holding for the runway (phase Inbound), in the order they joined -
	 * HoldingSince, ties by id. DERIVED from Flights every call, never stored: see Enqueue.
	 */
	TArray<UFlight*> Queue() const;

	/**
	 * Clear at most one holding flight whose runway is free - UArrivalSequencer picks which -
	 * and dispatch it. Nothing while paused. Run by UOpsRuntime's "ArrivalQueue" bus pass when an
	 * event dirties it (ops batch 3 §5) - no longer every frame from UOpsRuntime::Tick.
	 *
	 * A REFUSED DISPATCH STAYS QUEUED with its stand re-held (see DispatchNow): the flight that
	 * used to be lost at a busy ETA is now always landed eventually - FQueueTick::bRetry asks the
	 * pass to look again next frame.
	 * ENFORCED BY: AirportOps.Model.ArrivalQueue.DueWhileBusyWaitsThenLands
	 */
	FQueueTick TickQueue(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/**
	 * A NEW FRAME FOR ONE CLEARANCE A FRAME (#445): the aircraft just cleared claims the runway on its first motion tick, so a
	 * second clearance in the frame of the first would be decided before that claim exists - on two runways, onto a strip the first
	 * is about to hold. The rule is the QUEUE'S, so its state is here: TickQueue clears at most one flight between two calls of this,
	 * and says bDeferred when asked for another. UOpsRuntime::Tick calls it once per frame before the drain - the drain runs the
	 * arrival pass more than once when a clearance's own Arriving event wakes it again, which is exactly the case - and it counted
	 * the frames itself (DrainFrame, QueueClearedFrame) until this moved. THE RULE APPLIES ONLY ONCE A FRAME HAS BEGUN: a caller
	 * that never calls this (a world-free test driving TickQueue by hand) sets its own pace and is not limited - a board that cleared one
	 * flight and then refused for ever would be a trap for whoever called it without knowing.
	 * ENFORCED BY: AirportOps.Present.ArrivalQueue.SecondRunwayNextFrame, AirportOps.Model.FlightBoard.OneClearanceAFrameIsTheQueuesRule
	 */
	void BeginQueueFrame()
	{
		bQueueFramed = true;
		bClearedThisQueueFrame = false;
	}

	/** How many times TickQueue has run. For the runtime-wiring test. */
	int32 TickQueueCallsForTest() const { return TickQueueCalls; }

	/** Everything accepted and not yet departed. */
	TArray<UFlight*> Live() const;

	/**
	 * O(1): a maintained counter, not Offers().Num(). ISSUE #188 NAMED THIS CALL DIRECTLY - a
	 * count has no reason to cost an allocation and a full scan just to read its length, which
	 * is all Offers().Num() was ever doing here.
	 */
	int32 PendingOfferCount() const { return OfferedCount; }

	/** How many times TickOffers has copied Flights to walk them - its early-out's counter. */
	int32 OfferSnapshotCountForTest() const { return OfferSnapshots; }

private:
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
	 * Clock handles by flight id, so an accepted flight can be un-scheduled.
	 *
	 * NOT a UPROPERTY and NOT saved, on purpose: USimClock does not save its queue either,
	 * and a handle restored against a queue that no longer holds it would cancel somebody
	 * else's callback. UFlight::ArrivesAt is the saved truth; RearmSchedules rebuilds this.
	 */
	TMap<int32, int32> ArrivalHandles;

	/** See VerdictFor. By flight id; MUTABLE because VerdictFor is const and caching an answer
	 *  is bookkeeping about the board, not a change to what it holds. Not saved: a load starts
	 *  every verdict invalid, one recompute away from correct. */
	mutable TMap<int32, FOfferVerdict> Verdicts;

	/**
	 * Could a holding flight land, runway aside - the rest of the plan (exit, route, stand),
	 * excluding its own stand hold - by flight id, cached on the guideline and occupancy
	 * revisions like FOfferVerdict. The runway itself is asked live in TickQueue.
	 *
	 * THE CLEARANCE GATE (review C1/C2, 2026-09-28): the queue used to ask only "is the runway
	 * busy" and hand everything else to the dispatcher, which refused - with a toast, four log
	 * lines, a stand release and a board revision - every frame, for as long as the reason
	 * lasted, and one stuck flight at the head blocked the rest. Now nothing is dispatched that
	 * the plan would refuse, and a refusal is logged once, when its reason changes.
	 */
	struct FClearance
	{
		EArrivalRefusal Why = EArrivalRefusal::None;
		uint32 GuidelineAt = 0;
		uint32 OccupancyAt = 0;
		bool bValid = false;
	};
	TMap<int32, FClearance> Clearances;
	int32 TickQueueCalls = 0;

	/** Set by a clearance, cleared by BeginQueueFrame - see there. bQueueFramed: a frame has ever begun. Session state, never saved. */
	bool bClearedThisQueueFrame = false;
	bool bQueueFramed = false;

	/**
	 * Judges every flight still to arrive - accepted or holding - against the current guideline graph (#445), once per graph revision: the plan
	 * with no occupancy (what the field could EVER take, as UOfferGenerator::CouldEverAdmit asks) into Clearances, so UnlandableWhy has an answer
	 * for a flight that has not joined the queue AND one for a holding flight whatever the runway or the clock is doing - ClearanceFor is asked only
	 * with the runway free and the clock running, which left a verdict older than the graph after any edit made while paused or behind a busy
	 * runway (the FlightCannotLand alert cleared, and was raised again with a fresh toast). Run by TickQueue, first, which the pass runs when the
	 * network changes. A TRANSIENT verdict (GraphBeingEdited) is not kept: see ClearanceFor.
	 */
	void JudgeUnarrived(const URoadNetwork& Network);

	/** See Clearances. Logs on a change of reason; never bumps the revision. */
	EArrivalRefusal ClearanceFor(const UGroundTraffic& Traffic, const URoadNetwork& Network, const UFlight& Flight);

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
	 * are rebuilt from Flights and History in OnAfterRestore - the same split ArrivalHandles
	 * above already uses for the clock's handles. (And in Serialize's load, since #425: the flights they
	 * pointed at are replaced there, and a map left naming the pre-load objects would answer with them.)
	 */
	TMap<int32, TObjectPtr<UFlight>> ByAgent;
	TMap<int32, TObjectPtr<UFlight>> ById;

	/** PendingOfferCount's O(1) answer. Maintained at every entry into and exit from the
	 *  Offered phase; not a UPROPERTY, rebuilt in OnAfterRestore like the maps above. */
	int32 OfferedCount = 0;

	/** See OfferSnapshotCountForTest. A session counter, not saved. */
	int32 OfferSnapshots = 0;

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
	 * leaving Accepted disarms the arrival, leaving the ground lets go of the aeroplane (unhooks ByAgent and AgentId).
	 * EVERY CHANGE bumps the revision once. A change to the phase the flight is already in is nothing, and says nothing.
	 *
	 * WHAT IS NOT HERE, on purpose: the LOGS stay at each door (they carry the door's own context - the agent, the closure's
	 * reason), the money stays in OnAgentPhase (PostLandingFee/PostParkingFee - billing is its own job, the issue's
	 * Part 2), and DispatchNow's stand release stays before its dispatch (the planner must not see the hold), so the Landing
	 * row does not release.
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
	 *  ids: RearmSchedules's first half, and a load's step 0. */
	void DisarmEveryArrival(USimClock& Clock);

	/** Release the hold and put it on final, Now being the game time the change is dated. False, flight still Inbound and
	 *  stand re-held (Rehold), if refused. */
	bool DispatchNow(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight, double Now);

	/**
	 * THE ONE RE-HOLD (#471): hold Flight the stand a fresh plan taxis it to - PlanQuote, a busy runway queued, its own
	 * hold not counting - through UStandAllocator::Hold, exactly as TryAccept holds its plan's stand. False, Flight
	 * unchanged, when the plan names no stand (none free and reachable, or the plan refuses outright: a hold on a stand
	 * the flight cannot reach guarantees nothing).
	 *
	 * EVERY RE-HOLD IS THIS - the queue's (ReconcileStandHolds, from TickQueue), a load's (the same, from
	 * RestoreStandHolds), a failed dispatch's (DispatchNow). Each called UStandAllocator::Reserve, the smallest admitted
	 * unheld stand with no reach check: an unconnected small stand beat a connected bigger one, and the queue's "every
	 * queued flight has somewhere to go" held by size only. A full plan per re-hold is the cost - paid only by a flight
	 * that has no hold, never by one that has.
	 * ENFORCED BY: AirportOps.Model.FlightBoard.Rehold.LoadTakesTheReachableStand, .QueueTakesTheReachableStand,
	 * .FailedDispatchTakesTheReachableStand (each red with the smallest-fit choice back)
	 */
	bool Rehold(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight);

	/**
	 * THE ONE CONFLICT RULE FOR A FLIGHT'S STAND (#442): the occupancy table is the record of what a flight holds, and
	 * UFlight::Stand is the board's saved copy of it; this brings the copy back to the record, over InOrder in that order.
	 *  - A flight whose copy names a live stand the table does not hold for it (UStandAllocator::HoldIsLost - its hold was
	 *    refused when re-made, by Airside's rebuild on an edit or by Reapply on a load) GIVES IT UP and is re-held at
	 *    once, whatever its phase: the accept promised it a stand, and the next accept must not take the last one first.
	 *  - An Inbound flight with no stand, or a gone one, is re-held (the queue's rule since review I1): it is next to land.
	 * A gone stand on an Accepted flight is LEFT - the HeldStandLost alert's evidence (see UStandAllocator::Reapply) -
	 * until its ETA puts it in the queue.
	 *
	 * PATTERN: RECONCILIATION AGAINST A SYSTEM OF RECORD - the board observes the table, rather than Airside announcing
	 * which hold failed (the issue's second option, a delegate per refusal). The refusal is one of several ways the copy
	 * and the record part - a rebuild that drops a hold whose stand's pose moved, Reapply's refusal, whatever comes next -
	 * and asking the table catches each by the same test, with no new delegate across the plugin line. The edit IS still
	 * announced, once: FNetworkChangedEvent dirties the queue pass, which runs this after its closed exit and before its
	 * paused one, so a paused edit is reconciled too. NOT the issue's first option (Airside restores holds from a list the
	 * board hands it, UFlight::Stand a read of the table): UFlight::Stand has a second job - once parked it names the
	 * stand the aeroplane is on, which no hold records - and the dead-stand evidence the HeldStandLost alert reads is a
	 * stand the table can no longer hold at all.
	 * ENFORCED BY: AirportOps.Present.RuntimeEdit.RefusedReholdAgreesWithTheTable, AirportOps.Model.FlightSave.RequeueDoesNotTakeAnAcceptedStand
	 */
	void ReconcileStandHolds(UGroundTraffic& Traffic, const URoadNetwork& Network, TConstArrayView<UFlight*> InOrder);

	/** Into the queue: phase Inbound, HoldingSince = Since, stand kept. From the ETA callback and a load. */
	void Enqueue(UFlight& Flight, double Since);
	void Schedule(UGroundTraffic& Traffic, USimClock& Clock, UFlight& Flight);

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
	 * VerdictFor caches it for a flight; QuoteArrival asks it uncached for an arrival that is not a flight yet.
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
