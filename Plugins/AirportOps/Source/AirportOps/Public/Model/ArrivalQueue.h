#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "UObject/WeakObjectPtr.h"

class UFlight;
class UFlightBoard;
class UGroundTraffic;
class URoadNetwork;
class USimClock;
enum class EFlightPhase : uint8;

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
 * THE ARRIVALS' HALF OF THE FLIGHT BOARD (#442 item 4): an accepted flight's arrival ARMED on the clock (Schedule) and the queue
 * it joins when it comes due (Enqueue, Queue); the CLEARANCE cache that says whether a holding flight could land, runway aside
 * (ClearanceFor, JudgeUnarrived, UnlandableWhy); the pass that clears one and DISPATCHES it (Tick, DispatchNow, one clearance a
 * frame); and every stand RE-HOLD (Rehold, ReconcileStandHolds, a load's RestoreStandHolds) with the load's re-arm (RearmSchedules).
 * The natural state holder UArrivalSequencer never was: the sequencer stays POLICY ONLY (who is next), and the state it is a policy
 * over - the arrival handles, the clearances, the dated re-hold misses, the frame's one clearance - lives here.
 *
 * PATTERN: Extract Class, into a value-type COMPONENT that UFlightBoard holds - FOfferInbox's shape and FTurnarounds' before it
 * (#427, #494). The board stays the flight registry and the one transition owner; every public name it had stays on it as a
 * forwarder (TickQueue, BeginQueueFrame, Queue, UnlandableWhy, RestoreStandHolds, RearmSchedules, TickQueueCallsForTest), so no
 * caller changed. Tick, not TickQueue: Check-Architecture rule 33 holds TickQueue( to the "ArrivalQueue" pass, and the board's
 * forwarder is the name that rule reads.
 * ENFORCED BY: the build (every caller compiles unchanged), AirportOps.Present.FlightBoardOwnersAreWired, rule 33
 * NAMED DEVIATIONS, each forced (FOfferInbox's, for the same reasons):
 *  - A FRIEND OF UFlightBoard: Enqueue and a dispatch are phase changes (TransitionTo), a re-hold or a refused dispatch moves the
 *    board's one revision (RevisionCount - a second counter would split what Revision() promises its pollers), and a load's re-arm
 *    rebuilds the board's indices first. What it may reach is held by lint. ENFORCED BY: Check-Architecture rule 96
 *  - THE BOARD IS PASSED PER CALL, NEVER HELD - except by the clock callback Schedule arms, which captured the board before this
 *    moved (UFlightBoard's own `this`) and still does: the callback outlives the call by design, and the board outlives the clock's
 *    queue (RearmSchedules cancels every handle it armed).
 *  - NOT A UObject AND NOT A USTRUCT: nothing here is saved (the handles are re-made by RearmSchedules, the caches start a load
 *    empty) and nothing holds an object, so the board's UPROPERTY(Transient) wiring - the Sequencer, the Allocator, the Dispatcher -
 *    stays the board's, read through it.
 */
class AIRPORTOPS_API FArrivalQueue
{
public:
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
	FQueueTick Tick(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/**
	 * A NEW FRAME FOR ONE CLEARANCE A FRAME (#445): the aircraft just cleared claims the runway on its first motion tick, so a
	 * second clearance in the frame of the first would be decided before that claim exists - on two runways, onto a strip the first
	 * is about to hold. The rule is the QUEUE'S, so its state is here: Tick clears at most one flight between two calls of this,
	 * and says bDeferred when asked for another. UOpsRuntime::Tick calls it once per frame before the drain - the drain runs the
	 * arrival pass more than once when a clearance's own Arriving event wakes it again, which is exactly the case - and it counted
	 * the frames itself (DrainFrame, QueueClearedFrame) until this moved. THE RULE APPLIES ONLY ONCE A FRAME HAS BEGUN: a caller
	 * that never calls this (a world-free test driving TickQueue by hand) sets its own pace and is not limited - a board that cleared one
	 * flight and then refused for ever would be a trap for whoever called it without knowing.
	 * ENFORCED BY: AirportOps.Present.ArrivalQueue.SecondRunwayNextFrame, AirportOps.Model.FlightBoard.OneClearanceAFrameIsTheQueuesRule
	 */
	void BeginFrame()
	{
		bQueueFramed = true;
		bClearedThisQueueFrame = false;
	}

	/**
	 * The flights holding for the runway (phase Inbound), in the order they joined -
	 * HoldingSince, ties by id. DERIVED from Flights every call, never stored: see Enqueue.
	 */
	TArray<UFlight*> Queue(const UFlightBoard& Board) const;

	/**
	 * WHY A HOLDING FLIGHT CAN NEVER LAND, or None (#442): the refusal its cached clearance (ClearanceFor - the plan minus
	 * the runway, which Tick asks) holds, when that refusal is one the player must build or change something to clear
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

	/** Arm Flight's arrival on the clock at its ArrivesAt: the Accepted row of UFlightBoard::TransitionTo, and a load's re-arm. */
	void Schedule(UFlightBoard& Board, UGroundTraffic& Traffic, USimClock& Clock, UFlight& Flight);

	/** Into the queue: phase Inbound, HoldingSince = Since, stand kept. From the ETA callback and a load. */
	void Enqueue(UFlightBoard& Board, UFlight& Flight, double Since);

	/**
	 * THE ARRIVAL LET GO OF, whichever way Flight leaves Accepted (for To): its clock handle cancelled on Clock and forgotten.
	 * Called by UFlightBoard::TransitionTo for the phase LEFT - the row owns WHEN, this owns the handle.
	 */
	void Disarm(const UFlight& Flight, EFlightPhase To, USimClock* Clock);

	/**
	 * A LOAD'S STAND HOLDS, step 3 of RestoreAfterLoad: re-make every Accepted and Inbound flight's hold from its saved
	 * UFlight::Stand (UStandAllocator::Reapply), HoldLast's after all the others, then ReconcileStandHolds over them in
	 * that order - a refused one given up and re-held, an Inbound flight with no stand (or a gone one) re-held. HoldLast
	 * is the load's re-queued flights (DemoteRestoredMidFlight).
	 *
	 * IT WAS OnGraphRebuilt, named for a rebuild it stopped serving when PR D's review (I1) kept an edit's holds inside
	 * Airside; it is the load's step now (#442). NOT AN EDIT'S REACTION: an edit's refusal
	 * reaches ReconcileStandHolds through the queue pass the edit's FNetworkChangedEvent runs.
	 */
	void RestoreStandHolds(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, const TArray<UFlight*>& HoldLast);

	/**
	 * Re-arm the clock for every Accepted flight's arrival.
	 *
	 * CALLED AFTER A LOAD, and it is not optional: USimClock deliberately does not save its
	 * callback queue, so a restored flight has an ETA and nothing armed. Without this an
	 * accepted flight never arrives - and nothing anywhere says so. Offers need nothing: their
	 * countdown is a saved seconds-left figure TickOffers resumes (snapshot v5).
	 */
	void RearmSchedules(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock);

	/** Every armed arrival cancelled on Clock and forgotten, with the clearances and re-hold misses keyed on the same ids -
	 *  the queue's share of UFlightBoard::DisarmEveryArrival. */
	void DisarmEvery(USimClock& Clock);

	/** A flight left the board's live list (UFlightBoard::MoveToHistory): its clearance and re-hold miss are nobody's now. */
	void Forget(int32 FlightId)
	{
		Clearances.Remove(FlightId);
		ReholdMisses.Remove(FlightId);
	}

	/** How many times Tick has run. For the runtime-wiring test. */
	int32 TickCalls() const { return TickQueueCalls; }

private:
	/**
	 * Judges every flight still to arrive - accepted or holding - against the current guideline graph (#445), once per graph revision: the plan
	 * with no occupancy (what the field could EVER take, as UOfferGenerator::CouldEverAdmit asks) into Clearances, so UnlandableWhy has an answer
	 * for a flight that has not joined the queue AND one for a holding flight whatever the runway or the clock is doing - ClearanceFor is asked only
	 * with the runway free and the clock running, which left a verdict older than the graph after any edit made while paused or behind a busy
	 * runway (the FlightCannotLand alert cleared, and was raised again with a fresh toast). Run by Tick, first, which the pass runs when the
	 * network changes. A TRANSIENT verdict (GraphBeingEdited) is not kept: see ClearanceFor.
	 */
	void JudgeUnarrived(const UFlightBoard& Board, const URoadNetwork& Network);

	/** See Clearances. Logs on a change of reason; never bumps the revision. */
	EArrivalRefusal ClearanceFor(const UGroundTraffic& Traffic, const URoadNetwork& Network, const UFlight& Flight);

	/** Release the hold and put it on final, Now being the game time the change is dated. False, flight still Inbound and
	 *  stand re-held (Rehold), if refused. */
	bool DispatchNow(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight, double Now);

	/**
	 * THE ONE RE-HOLD (#471): hold Flight the stand a fresh plan taxis it to - PlanQuote, a busy runway queued, its own
	 * hold not counting - through UStandAllocator::Hold, exactly as TryAccept holds its plan's stand. False, Flight
	 * unchanged, when the plan names no stand (none free and reachable, or the plan refuses outright: a hold on a stand
	 * the flight cannot reach guarantees nothing).
	 *
	 * EVERY RE-HOLD IS THIS - the queue's (ReconcileStandHolds, from Tick), a load's (the same, from
	 * RestoreStandHolds), a failed dispatch's (DispatchNow).
	 * ENFORCED BY: Check-Architecture rule 84 (stand-hold-is-a-plans-stand), AirportOps.Model.FlightBoard.Rehold.
	 * LoadTakesTheReachableStand, .QueueTakesTheReachableStand, .FailedDispatchTakesTheReachableStand (each red with the
	 * smallest-fit choice back). Each used to call UStandAllocator::Reserve, the smallest admitted unheld stand with no
	 * reach check: an unconnected small stand beat a connected bigger one, and the queue's "every queued flight has
	 * somewhere to go" held by size only. A full plan per re-hold is the cost - paid by a flight that has no hold, and not
	 * again after a miss until something the plan reads has moved (FReholdMiss).
	 */
	bool Rehold(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight);

	/**
	 * THE BOARD'S DOOR ONTO THE ONE CONFLICT RULE (#442) - UStandAllocator::Reconcile, which states the rule and the pattern
	 * (the occupancy table is the record of a flight's hold, UFlight::Stand the saved copy brought back to it) - with
	 * Rehold as its re-hold and a revision bump when a copy changed. From Tick (after its closed exit, before its
	 * paused one: a paused edit is reconciled too) and RestoreStandHolds; InOrder is who wins a contested stand.
	 * ENFORCED BY: AirportOps.Present.RuntimeEdit.RefusedReholdAgreesWithTheTable, AirportOps.Model.FlightSave.RequeueDoesNotTakeAnAcceptedStand
	 */
	void ReconcileStandHolds(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, TConstArrayView<UFlight*> InOrder);

	/**
	 * Clock handles by flight id, so an accepted flight can be un-scheduled.
	 *
	 * NOT a UPROPERTY and NOT saved, on purpose: USimClock does not save its queue either,
	 * and a handle restored against a queue that no longer holds it would cancel somebody
	 * else's callback. UFlight::ArrivesAt is the saved truth; RearmSchedules rebuilds this.
	 */
	TMap<int32, int32> ArrivalHandles;

	/**
	 * Could a holding flight land, runway aside - the rest of the plan (exit, route, stand),
	 * excluding its own stand hold - by flight id, cached on the guideline and occupancy
	 * revisions like FOfferVerdict. The runway itself is asked live in Tick.
	 *
	 * THE CLEARANCE GATE (review C1/C2, 2026-09-28): the queue used to ask only "is the runway
	 * busy" and hand everything else to the dispatcher, which refused - with a toast, four log
	 * lines, a stand release and a board revision - every frame, for as long as the reason
	 * lasted, and one stuck flight at the head blocked the rest. Now nothing is dispatched that
	 * the plan would refuse, and a refusal is logged once, when its reason changes.
	 *
	 * AND ON UGroundTraffic::StandHoldChangeCount (#497 re-review), FReholdMiss's third stamp: a body the per-tick claim pass rolls
	 * onto a stand's pose, or off it, moves no OccupancyRevision, and the plan this caches reads exactly that (IsHeld on a pose).
	 * Dated by occupancy alone, a NoFreeStand outlived the body that caused it until some unrelated claim moved the revision - the
	 * re-hold's HoldStand bump was what happened to rescue AirportOps.Model.ArrivalQueue.StandFreedByChurnIsNotStale.
	 * ENFORCED BY: AirportOps.Model.ArrivalQueue.ClearanceIsDatedByStandChurn
	 */
	struct FClearance
	{
		EArrivalRefusal Why = EArrivalRefusal::None;
		uint32 GuidelineAt = 0;
		uint32 OccupancyAt = 0;
		uint32 StandChurnAt = 0;
		bool bValid = false;
	};
	TMap<int32, FClearance> Clearances;

	/**
	 * A RE-HOLD THAT FOUND NO STAND, DATED (#497 review), by flight id - so the queue pass does not plan again, every pass, for
	 * a flight that has none until something a plan reads has moved: ClearanceFor's two stamps; UGroundTraffic::
	 * StandHoldChangeCount, because a body rolling off a stand's pose moves no OccupancyRevision (the blind spot
	 * AirportOps.Model.ArrivalQueue.StandFreedByChurnIsNotStale pins); and the network itself, VerdictFor's reason. Session
	 * state, never saved: a load starts it empty, one plan away from right.
	 * ENFORCED BY: AirportOps.Model.FlightBoard.Rehold.FailedReholdIsDated
	 */
	struct FReholdMiss
	{
		FWeakObjectPtr Network;
		uint32 GuidelineAt = 0;
		uint32 OccupancyAt = 0;
		uint32 StandChurnAt = 0;
		bool operator==(const FReholdMiss& Other) const
		{
			return Network == Other.Network && GuidelineAt == Other.GuidelineAt && OccupancyAt == Other.OccupancyAt
				&& StandChurnAt == Other.StandChurnAt;
		}
	};
	TMap<int32, FReholdMiss> ReholdMisses;
	int32 TickQueueCalls = 0;

	/** Set by a clearance, cleared by BeginFrame - see there. bQueueFramed: a frame has ever begun. Session state, never saved. */
	bool bClearedThisQueueFrame = false;
	bool bQueueFramed = false;
};
