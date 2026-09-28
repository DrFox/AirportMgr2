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
class UGroundTraffic;
class UOfferGenerator;
class URoadNetwork;
class UStandAllocator;
class ULedger;
class UPricing;
class USimClock;
enum class EAgentPhase : uint8;

/**
 * Whether an offer can be accepted right now, and whether the airport can serve it.
 *
 * CACHED ON THREE REVISIONS (issue #169, moved from UOfferViewModel 2026-09-28): Why is a
 * full ArrivalPlanner::Plan - a route search over every stand, then every runway exit - so it
 * is recomputed only when something it depends on has moved: the board itself
 * (UFlightBoard::Revision - added/accepted/declined/expired), the guideline graph
 * (URoadNetwork::GetGuidelineRevision - an edit changed the taxiways), or occupancy
 * (UGroundTraffic::OccupancyRevision - a stand claimed or freed, a runway taken or cleared).
 * Three integer compares replace the search on every frame where none of them moved.
 */
struct FOfferVerdict
{
	EArrivalRefusal Why = EArrivalRefusal::None;

	/** Could a depot fuel this airframe on a stand it would take? True when nothing checks
	 *  (no fuel service wired, as in most board tests). A MISSING service does not block the
	 *  accept (spec ruling 5) - the row says so and C scores it. */
	bool bFuelServable = true;

	uint32 BoardAt = 0;
	uint32 GuidelineAt = 0;
	uint32 OccupancyAt = 0;
	bool bValid = false;
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
	 * Recreate every flight's own ApproachFocus from this board's one field, for a snapshot
	 * older than FOpsSnapshot::Version 3.
	 *
	 * MOVED HERE FROM OpsSave::Restore so that Restore could become a plain loop over every
	 * persistent object rather than naming this class as a parameter and calling it in one
	 * particular position - see IOpsPersistent::OnAfterRestore.
	 */
	virtual void OnAfterRestore(int32 SnapshotVersion) override;

	/**
	 * How many game days a terminal flight (Declined, Expired or Departed) is kept in History
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
	 * Bumped whenever anything a viewmodel displays has changed - an offer added, accepted,
	 * declined or expired, a phase change, a graph rebuild's re-apply.
	 *
	 * REPLACES A DELEGATE THIS CLASS USED TO CARRY (issue #169): OnChanged fired from nearly
	 * every method here - AddOffer, Accept, Decline, DispatchNow, OnAgentPhase, OnGraphRebuilt
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

	UPROPERTY() TObjectPtr<UStandAllocator> Allocator = nullptr;

	/** Who of the holding flights is cleared next. Null = strict first come. See UArrivalSequencer. */
	UPROPERTY() TObjectPtr<UArrivalSequencer> Sequencer = nullptr;
	UPROPERTY() TObjectPtr<UOfferGenerator> Generator = nullptr;

	/**
	 * The money, or null in a test that does not care about it. Set by UOpsRuntime::Attach.
	 *
	 * NULL IS A WORKING STATE, not a bug to guard against at every call: dozens of existing
	 * board tests drive flights through their whole lifecycle and have no interest in fees, and
	 * making them all construct a ledger would be churn for nothing.
	 */
	UPROPERTY() TObjectPtr<ULedger> Ledger = nullptr;
	UPROPERTY() TObjectPtr<UPricing> Pricing = nullptr;

	/**
	 * Asked whether the airport could fuel an offer, for FOfferVerdict::bFuelServable. Null in
	 * a test that does not care - fuel then reads as servable. Set by UOpsRuntime::Attach.
	 */
	UPROPERTY() TObjectPtr<UJobBoard> Fuel = nullptr;

	/**
	 * Bank the landing fee this flight was OFFERED at. Idempotent - a flight lands once.
	 *
	 * PUBLIC so a test can post a fee without driving a whole agent through its phases; the
	 * production caller is OnAgentPhase, and there is only the one.
	 */
	void PostLandingFee(double Now, UFlight& Flight);

	/** Bank the parking fee for the hours actually occupied, and record it on the flight. */
	void PostParkingFee(double Now, UFlight& Flight);

	/**
	 * The DEFAULT focus for the next generated offer - UOpsRuntime writes it before calling
	 * UOfferGenerator::TickMinute, which copies it onto every flight it builds.
	 *
	 * NOT consulted by Accept, WhyNotAcceptable or DispatchNow: those read UFlight::
	 * ApproachFocus, which is fixed on the flight at the offer and travels with it. This
	 * field used to be read there too, and whichever caller wrote it LAST decided every
	 * later offer's answer - see UFlight::ApproachFocus and issue #96.
	 */
	UPROPERTY() FVector2D ApproachFocus = FVector2D::ZeroVector;

	/**
	 * The longest runway's threshold, for the next generated offer to aim at. False, and
	 * OutFocus untouched, when the airport has no runway yet.
	 *
	 * MOVED OUT OF UOpsRuntime (issue #98): choosing a runway is a pure function of the
	 * graph, the same kind of decision ArrivalPlanner::Plan makes, and Present/ may hold no
	 * logic of its own - see OpsRuntime.h's own header. AIMED AT THE LONGEST RUNWAY, not
	 * wherever the land key last looked: ArrivalPlanner chooses by nearest threshold to the
	 * focus, so an offer generated with a stale or default focus would be planned against
	 * whichever strip happens to sit nearest it and then accepted against a different one.
	 *
	 * BOOL AND AN OUT-PARAMETER, not FVector2D::ZeroVector on "none": zero is a valid
	 * threshold, and a caller that cannot tell "no runway" from "a runway starting at the
	 * origin" would overwrite a perfectly good ApproachFocus with a false one the moment
	 * every runway was removed - see CLAUDE.md, "honour the return of anything that fills an
	 * out-parameter."
	 */
	static bool DefaultApproachFocus(const URoadNetwork& Network, FVector2D& OutFocus);

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
	 * Hold a stand and put the arrival on the clock.
	 *
	 * False when no stand admits it, which is the refusal the inbox shows - and the whole of
	 * "the player cannot over-commit": an accepted flight always has somewhere to go.
	 */
	bool Accept(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock,
		UFlight& Flight);

	/** Retires the offer at once; free (spec ruling 8) - only a lapse will cost anything, in C. */
	void Decline(USimClock& Clock, UFlight& Flight);

	/**
	 * Make a flight from an airframe, aim it at Focus, and accept it on the spot - the debug
	 * land key's whole job, and previously done by hand at the call site (issue #96).
	 *
	 * Its lead time is zero, so ArrivesAt is Clock.Now(): this exists to put an aeroplane on the field
	 * THIS SECOND, not to queue a normal offer. Focus travels onto the flight itself - see
	 * UFlight::ApproachFocus - so it never has to touch the board's own field, which the
	 * generator also writes and would otherwise fight over.
	 *
	 * Returns EArrivalRefusal::None on success, or the reason Accept refused it - the same
	 * sentence ArrivalPlanner::DescribeRefusal would print for it. The flight is left in the
	 * inbox on refusal, exactly as a generated offer nobody could accept yet is.
	 */
	EArrivalRefusal AcceptImmediate(UGroundTraffic& Traffic, const URoadNetwork& Network,
		USimClock& Clock, const FAirframe& Airframe, const FVector2D& Focus, FText Airline);

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
	 */
	void OnAgentPhase(const UGroundTraffic& Traffic, const URoadNetwork& Network,
		const USimClock& Clock, int32 AgentId, EAgentPhase From, EAgentPhase To);

	/** Re-make every accepted flight's stand hold. See UStandAllocator::Reapply. */
	void OnGraphRebuilt(UGroundTraffic& Traffic, const URoadNetwork& Network);

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
	 * and dispatch it. Nothing while paused. Called every frame from UOpsRuntime::Tick.
	 *
	 * A REFUSED DISPATCH STAYS QUEUED with its stand re-held (see DispatchNow): the flight that
	 * used to be lost at a busy ETA is now always landed eventually.
	 * ENFORCED BY: AirportOps.Model.ArrivalQueue.DueWhileBusyWaitsThenLands
	 */
	void TickQueue(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

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

	/**
	 * Copies this board's own ApproachFocus onto every flight it holds.
	 *
	 * A LOAD-ONLY MIGRATION for a snapshot older than FOpsSnapshot::Version 3 - see
	 * OpsSave::Restore, which is the one caller. Before UFlight::ApproachFocus existed
	 * (issue #96) every flight shared this one board-wide field, so recreating it per-flight
	 * is the only way an old load lands where it was actually aimed rather than the origin.
	 */
	void AimUnaimedFlightsAtBoardFocus();

private:
	/**
	 * Every flight not yet in a terminal phase: offered, accepted, or anywhere between landing
	 * and departing. TERMINAL flights (Declined, Expired, Departed) are moved into History the
	 * moment they get there - see MoveToHistory - rather than staying here forever, which is
	 * what made FindByAgent, FindById, Offers(), Live() and every save cost O(every flight
	 * this session has ever seen) instead of O(what is actually happening) - issue #188.
	 */
	UPROPERTY() TArray<TObjectPtr<UFlight>> Flights;

	/**
	 * Terminal flights MoveToHistory has retired, in the order they arrived here.
	 *
	 * BOUNDED BY RollUp, which forgets anything older than MaxDays on the same daily beat as
	 * ULedger::RollUp. SAVED like Flights (see IOpsPersistent's class comment: a model
	 * object's non-Transient UPROPERTYs ARE its saved state) - now that it is bounded, keeping
	 * it in the save is cheap, and a player who just watched a flight leave should still find
	 * it if a "recent departures" view ever reads this.
	 */
	UPROPERTY() TArray<TObjectPtr<UFlight>> History;

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
	 * above already uses for the clock's handles.
	 */
	TMap<int32, TObjectPtr<UFlight>> ByAgent;
	TMap<int32, TObjectPtr<UFlight>> ById;

	/** PendingOfferCount's O(1) answer. Maintained at every entry into and exit from the
	 *  Offered phase; not a UPROPERTY, rebuilt in OnAfterRestore like the maps above. */
	int32 OfferedCount = 0;

	/** Release the hold and put it on final. False, flight still Inbound and stand re-held, if refused. */
	bool DispatchNow(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight);

	/** Into the queue: phase Inbound, HoldingSince = Since, stand kept. From the ETA callback and a load. */
	void Enqueue(UFlight& Flight, double Since);
	void Schedule(UGroundTraffic& Traffic, USimClock& Clock, UFlight& Flight);

	/**
	 * Retires Flight out of the live list: stamps TerminatedAt, moves it Flights -> History,
	 * and drops its ByAgent entry if it still had one. THE ONE PLACE a flight leaves Flights,
	 * so every terminal transition - Decline, TickOffers' lapse, OnAgentPhase's Departed
	 * branch, and the migration sweep in
	 * OnAfterRestore - goes through it rather than five call sites each remembering their own
	 * piece of the move (issue #188).
	 */
	void MoveToHistory(UFlight& Flight, double Now);

	/**
	 * Rebuilds ByAgent, ById and OfferedCount from scratch off Flights and History. Called
	 * from OnAfterRestore AND from RearmSchedules - see RearmSchedules's own comment on why a
	 * board deserialised without going through OpsSave::Restore still needs this before its
	 * re-armed clock callbacks can find anything by id.
	 */
	void RebuildIndices();

	/** O(1) via ByAgent/ById. CONST because a lookup does not change what the board holds -
	 *  which also lets FindByAgentForTest/FindByIdForTest above call them on a const board. */
	UFlight* FindByAgent(int32 AgentId) const;
	UFlight* FindById(int32 Id) const;
};
