#include "Model/ArrivalQueue.h"

#include "AirportOpsLog.h"
#include "Model/ArrivalSequencer.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayQuery.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Algo/StableSort.h"

// THE ARRIVALS' HALF OF UFlightBoard (#442 item 4) - see FArrivalQueue's class comment for the pattern and its deviations. Every body
// below MOVED here unchanged but for reaching the board through the Board it is handed (Board.TransitionTo, Board.LiveFlights(),
// Board.Allocator, ++Board.RevisionCount...), where it used to be the board's own `this`; TickQueue's body is Tick's.

void FArrivalQueue::Schedule(UFlightBoard& Board, UGroundTraffic& Traffic, USimClock& Clock, UFlight& Flight)
{
	// WEAK, not a captured reference. The callback outlives this call by design - minutes of
	// game time - and the traffic model belongs to an actor that a level change can take
	// away underneath it.
	TWeakObjectPtr<UGroundTraffic> WeakTraffic = &Traffic;
	const int32 Id = Flight.Id;
	// THE BOARD, CAPTURED AS IT CAPTURED ITSELF - its own `this` - before #442 item 4 moved this here: the callback outlives the
	// call, and the board outlives the clock's queue (RearmSchedules and every cancel disarm what this arms). Named, not an
	// init-capture, so Check-Architecture rule 96 reads what the callback reaches through it.
	UFlightBoard* const BoardPtr = &Board;

	const int32 Handle = Clock.At(Flight.ArrivesAt, [this, BoardPtr, WeakTraffic, Id]()
	{
		UFlight* Due = BoardPtr->FindById(Id);
		// ONLY AN ACCEPTED FLIGHT JOINS THE QUEUE (whole-stack review M3): FindById finds it in History too, and every
		// cancel disarming this callback first is a rule of the callers, not of this line. A flight that has become
		// anything else since it was armed is not due.
		// ENFORCED BY: AirportOps.Model.FlightBoard.ArrivalRequeuesOnlyAnAccepted
		if (WeakTraffic.Get() != nullptr && Due != nullptr && Due->GetPhase() == EFlightPhase::Accepted)
		{
			// INTO THE QUEUE, not straight onto the runway (spec 2026-09-28-arrival-queue): the
			// runway may be busy, and TickQueue is what decides when it is not. HoldingSince is
			// the ETA - the moment this callback exists to mark.
			Enqueue(*BoardPtr, *Due, Due->ArrivesAt);
		}
	});
	ArrivalHandles.Add(Flight.Id, Handle);
}

void FArrivalQueue::Enqueue(UFlightBoard& Board, UFlight& Flight, double Since)
{
	// THE INBOUND ROW OF TransitionTo: HoldingSince = Since, the arrival handle let go (it has fired, or a load's re-arm reset
	// the map), an aeroplane still flying the flight unhooked (a load's re-queue of a Landing or TaxiIn flight), the
	// revision, and FFlightInboundEvent. THE QUEUE PASS'S WAKE-UP (ops batch 3 §5): the queue is no longer ticked every frame, so a
	// flight joining it says so. Here, the one site every Inbound flight passes through - while the game runs, and since #426 a load's
	// re-queue too (DemoteRestoredMidFlight), which the load's MarkAllDirty used to be the only thing to wake.
	// ENFORCED BY: AirportOps.Model.FlightBoard.EnqueuePublishesInbound
	Board.TransitionTo(Flight, EFlightPhase::Inbound, FTransitionCause::Played(Since));
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) holding: #%d in queue"),
		Flight.Id, *Flight.Callsign, Queue(Board).Find(&Flight) + 1);
}

TArray<UFlight*> FArrivalQueue::Queue(const UFlightBoard& Board) const
{
	TArray<UFlight*> Out;
	for (const TObjectPtr<UFlight>& Each : Board.LiveFlights())
	{
		if (Each != nullptr && Each->GetPhase() == EFlightPhase::Inbound)
		{
			Out.Add(Each);
		}
	}
	// DERIVED, NOT STORED: the queue is the holding flights in the order they joined, ties by
	// id - one list (Flights), so a save needs nothing new and nothing can drift out of step.
	Algo::StableSortBy(Out, [](const UFlight* F) { return F->HoldingSince; });
	return Out;
}

EArrivalRefusal FArrivalQueue::ClearanceFor(const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const UFlight& Flight)
{
	FClearance& Clearance = Clearances.FindOrAdd(Flight.Id);
	const uint32 GuidelineNow = Network.GetGuidelineRevision();
	const uint32 OccupancyNow = Traffic.OccupancyRevision();
	// AND THE STAND CHURN (#497 re-review): a body on or off a stand's pose moves no OccupancyRevision, and this plan reads exactly
	// that - see FClearance. FReholdMiss's third stamp, so the clearance and the re-hold date the plan alike.
	const uint32 StandChurnNow = Traffic.StandHoldChangeCount();
	if (Clearance.bValid && Clearance.GuidelineAt == GuidelineNow && Clearance.OccupancyAt == OccupancyNow
		&& Clearance.StandChurnAt == StandChurnNow)
	{
		return Clearance.Why;
	}
	// THE PLAN THE DISPATCH ITSELF WILL MAKE, MINUS THE RUNWAY STEP - Queue, not Refuse - and
	// without this flight's own stand hold counting against it: it keeps that hold precisely so
	// it has somewhere to go, and DispatchNow releases it the moment before dispatching.
	//
	// THE RUNWAY IS NOT CACHED HERE, it is asked live in TickQueue: OccupancyRevision does not
	// move for a taxiing aircraft's runway crossing (see its own comment), so a cached "busy"
	// could strand a quiet airport's queue until some unrelated claim happened to bump it.
	FArrivalPlan Planned = ArrivalPlanner::Plan(Network, Flight.RunwayPreference, Flight.Airframe,
		&Traffic.GetOccupancy(), ERunwayBusy::Queue, Flight.HolderId());
	const EArrivalRefusal Why = Planned.Why;
	// A DRAG IS NOT THE FLIGHT'S NEWS (#445): GraphBeingEdited is the planner's "not on a graph mid-edit", answered before anything
	// else is read, and it clears when the player lets go. It is returned - nothing is cleared to land on a stale graph - but NOT
	// KEPT: stored over a standing permanent verdict, it made UnlandableWhy read None for the length of the drag, the FlightCannotLand
	// alert clear, and the drop re-raise it with a fresh toast. The standing verdict stays dated by the graph revision it was made
	// at, which a drag does not move until its drop. Nothing is lost by not caching it: the planner answers it at its step 0, before any route
	// search (ArrivalPlanner.cpp, 2026-09-30), so asking again costs one revision compare.
	// ENFORCED BY: AirportOps.Present.Alerts.AlertSurvivesANodeDrag
	if (Why == EArrivalRefusal::GraphBeingEdited)
	{
		return Why;
	}
	if (Why != EArrivalRefusal::None && (!Clearance.bValid || Why != Clearance.Why))
	{
		// ONCE PER REASON, not per frame - the reason is the evidence, the repetition is noise.
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) holding: cannot land yet - %s"),
			Flight.Id, *Flight.Callsign, *ArrivalPlanner::DescribeRefusal(Why, Flight.Airframe.Wingspan));
	}
	Clearance.Why = Why;
	Clearance.GuidelineAt = GuidelineNow;
	Clearance.OccupancyAt = OccupancyNow;
	Clearance.StandChurnAt = StandChurnNow;
	Clearance.Usable = MoveTemp(Planned.UsableRunways);
	Clearance.bValid = true;
	return Why;
}

EArrivalRefusal FArrivalQueue::UnlandableWhy(const UFlight& Flight, const URoadNetwork& Network) const
{
	// HOLDING OR ACCEPTED: the clearance is computed for the queue (TickQueue), and a judgement of either is JudgeUnarrived's (#445) - also
	// the queue pass's, never this alert pass's: a plan run here would be a route search per flight per alert recompute.
	if (!Flight.IsUnarrived())
	{
		return EArrivalRefusal::None;
	}
	const FClearance* Clearance = Clearances.Find(Flight.Id);
	// A FLIGHT THE QUEUE HAS NOT JUDGED is None - not "refused" - so an alert is never raised on a guess. NOR ONE JUDGED AGAINST A
	// GRAPH THAT HAS MOVED since (#442 review): ClearanceFor is asked only after the runway is found free, so a flight waiting behind a
	// busy runway keeps its old "no exit" after the player fixes the airport, and the alert would outlive the fix until the runway
	// freed. An edit that could change the answer moves the guideline revision, so a judgement dated before it says nothing now. (The
	// occupancy stamp is not asked: the permanent refusals are about the graph, and the transient ones - a busy stand - are not here.)
	if (Clearance == nullptr || !Clearance->bValid || Clearance->GuidelineAt != Network.GetGuidelineRevision()
		|| !ArrivalPlanner::IsPermanentRefusal(Clearance->Why))
	{
		return EArrivalRefusal::None;
	}
	return Clearance->Why;
}

void FArrivalQueue::JudgeUnarrived(const UFlightBoard& Board, const URoadNetwork& Network)
{
	const uint32 GuidelineNow = Network.GetGuidelineRevision();
	for (const TObjectPtr<UFlight>& Each : Board.LiveFlights())
	{
		// EVERY FLIGHT STILL TO ARRIVE - accepted or holding (#445 review). A holding flight used to be judged only by ClearanceFor, which TickQueue asks
		// only once the runway is found free and the clock is running: behind a busy runway, or while paused, a graph change left its verdict older than
		// the graph and the FlightCannotLand alert read None - cleared - until the queue happened to ask again (and with nothing on the ground, nothing
		// woke the alerts pass after that). Re-dated here on every graph change, before either exit.
		if (Each == nullptr || !Each->IsUnarrived())
		{
			continue;
		}
		FClearance& Clearance = Clearances.FindOrAdd(Each->Id);
		if (Clearance.bValid && Clearance.GuidelineAt == GuidelineNow)
		{
			continue;
		}
		// WHAT THE FIELD COULD EVER TAKE, not what is free this second: no occupancy, so a busy runway or a held stand is not an
		// answer here, and IsPermanentRefusal of what is left is UOfferGenerator::CouldEverAdmit's own test. A flight that was accepted
		// because its plan passed is refused now only by an edit since.
		const EArrivalRefusal Why = ArrivalPlanner::Plan(Network, Each->RunwayPreference, Each->Airframe).Why;
		if (Why == EArrivalRefusal::GraphBeingEdited)
		{
			// MID-DRAG: no judgement (ClearanceFor's reason), and the next network change is the drop's.
			continue;
		}
		if (Why != EArrivalRefusal::None && ArrivalPlanner::IsPermanentRefusal(Why) && (!Clearance.bValid || Why != Clearance.Why))
		{
			// SAID ONCE PER REASON, in the words each phase has always used: ClearanceFor's line for a holding flight (which it no longer
			// writes when this got there first), and the accepted flight's own.
			if (Each->GetPhase() == EFlightPhase::Accepted)
			{
				UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) accepted: it can no longer land - %s"),
					Each->Id, *Each->Callsign, *ArrivalPlanner::DescribeRefusal(Why, Each->Airframe.Wingspan));
			}
			else
			{
				UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) holding: cannot land yet - %s"),
					Each->Id, *Each->Callsign, *ArrivalPlanner::DescribeRefusal(Why, Each->Airframe.Wingspan));
			}
		}
		Clearance.Why = Why;
		Clearance.GuidelineAt = GuidelineNow;
		// NO OCCUPANCY WAS ASKED, so no occupancy revision dates it: ClearanceFor's own test then plans afresh with one when the flight
		// is next asked (the runway free, the clock running), and does not trust this verdict for the runway's busy-ness it never asked.
		Clearance.OccupancyAt = TNumericLimits<uint32>::Max();
		Clearance.bValid = true;
	}
}

FQueueTick FArrivalQueue::Tick(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
{
	FQueueTick Result;
	// ONE CLEARANCE A FRAME (#445): the rule is the queue's, and this is where it is kept - see BeginQueueFrame. Before anything is read
	// AND BEFORE THE COUNT: a deferred tick decides nothing, so it is no run - TickQueueCallsForTest counts what the runtime's
	// per-frame poll used to, and the deferral was never counted then either.
	if (bQueueFramed && bClearedThisQueueFrame)
	{
		Result.bDeferred = true;
		return Result;
	}
	++TickQueueCalls;
	// EVERY FLIGHT STILL TO ARRIVE, JUDGED AGAINST TODAY'S GRAPH (#445), before the closed and paused exits below: it reads no clock and no runway,
	// and the FlightCannotLand alert that follows this pass in the round reads what it leaves - so a graph change made while paused, or with the
	// runway busy, cannot leave a verdict older than the graph for the alert to read as "fixed". A closed airport cancels its accepted
	// flights by its own event, so a verdict made for one is not read for long.
	// ENFORCED BY: AirportOps.Present.Alerts.UnlandableAlertSurvivesAPausedEdit,
	// AirportOps.Model.Alerts.HoldingFlightAlertSurvivesAnUnrelatedEditBehindABusyRunway, AirportOps.Present.Airport.CloseCancelsThroughTheBus
	JudgeUnarrived(Board, Network);
	// COUNTED BEFORE THE PAUSE TEST: a paused queue is still a queue, and the pass arms its safety net from this.
	const TArray<UFlight*> Waiting = Queue(Board);
	Result.Waiting = Waiting.Num();
	// A CLOSED AIRPORT ADMITS NO ARRIVALS - HERE TOO (whole-stack review I1): Accept and Land were gated, and this, the
	// door onto the runway itself, was not; a flight left holding by a load or a closure's edge case landed anyway. The
	// same predicate as Accept, so the two doors cannot disagree about what "open" is. Before the stand re-reserve: a
	// closed airport takes no stand for a flight it will not land. AND BEFORE THE PAUSE TEST (re-review m6): bClosed is
	// what keeps the pass from arming a safety net, and a closed airport paused is still closed.
	// ENFORCED BY: AirportOps.Present.ArrivalQueue.ClosedAirportDispatchesNothing
	if (Board.AdmitsArrivals && !Board.AdmitsArrivals())
	{
		Result.bClosed = true;
		return Result;
	}

	// A HOLDING FLIGHT WITH NO STAND TAKES ONE BACK the moment one is free (review I1): the
	// stand is what makes an accept safe, and a holder without one could land on a stand the
	// next accept was promised. AND ANY UNARRIVED FLIGHT WHOSE HOLD WAS LOST (#442) gives it up and is re-held: one
	// routine, the load's too - UStandAllocator::Reconcile.
	//
	// A DEAD STAND COUNTS AS NONE (PR C's follow-up, done in PR D): Reapply keeps a deleted stand on the flight as
	// HeldStandLost's evidence, and a load that found no free stand leaves it there - so an Inbound flight naming a
	// gone stand must take one back here, the first time a stand frees, exactly as RestoreStandHolds' load-time pass
	// does. The re-hold overwrites the dead Stand, which clears the alert by the same test.
	// AFTER THE CLOSED EXIT, as it always was (a closed airport takes no stand for a flight it will not land), and NOW
	// BEFORE THE PAUSED ONE (#442): the player edits paused, and a hold that edit lost must not wait for the clock -
	// meanwhile the next accept could take the stand it is owed. The queue first, in its order (it lands sooner), then
	// the accepted flights - a re-hold is a plan per flight that HAS no hold, nothing for one that has.
	// ENFORCED BY: AirportOps.Model.ArrivalQueue.DeadStandReservesWhenOneFrees,
	// AirportOps.Present.RuntimeEdit.RefusedReholdAgreesWithTheTable (paused, and an Accepted flight)
	{
		TArray<UFlight*> InOrder = Waiting;
		for (const TObjectPtr<UFlight>& Each : Board.LiveFlights())
		{
			if (Each != nullptr && Each->GetPhase() == EFlightPhase::Accepted)
			{
				InOrder.Add(Each);
			}
		}
		ReconcileStandHolds(Board, Traffic, Network, InOrder);
	}

	if (Clock.IsPaused() || Waiting.Num() == 0)
	{
		return Result;
	}

	// LIVE RUNWAY, CACHED REST - see ClearanceFor. Together they are exactly the Refuse plan the
	// dispatch will make, so nothing is dispatched that it would refuse. THE RUNWAYS IT CAN USE, not
	// every arrival runway: a free strip this airframe cannot land on is no clearance, and gating on
	// "any runway free" dispatched into a RunwayOccupied refusal every frame (samples/refused.png).
	// ENFORCED BY: AirportOps.Model.ArrivalQueue.FreeRunwayItCannotUseIsNoClearance
	const auto CanClear = [this, &Traffic, &Network](const UFlight& F)
	{
		if (ClearanceFor(Traffic, Network, F) != EArrivalRefusal::None)
		{
			return false;
		}
		const FClearance* Cached = Clearances.Find(F.Id);
		return Cached != nullptr && !RunwayQuery::AreRunwaysHeld(Network, Cached->Usable, &Traffic.GetOccupancy());
	};
	// NULL SEQUENCER IS STRICT FIRST COME, for a test that does not wire one.
	UFlight* Next = Board.Sequencer != nullptr ? Board.Sequencer->Next(Waiting, CanClear)
		: (CanClear(*Waiting[0]) ? Waiting[0] : nullptr);
	if (Next == nullptr)
	{
		return Result;
	}
	// ONE CLEARANCE A FRAME: the aircraft just cleared claims the runway on its first tick, and
	// a second clearance this frame would be decided before that claim exists. The pass may run
	// twice in one drain, so the rule is kept across its rounds - bClearedThisQueueFrame, checked at the top.
	// ENFORCED BY: AirportOps.Present.ArrivalQueue.SecondRunwayNextFrame
	const double Held = Clock.Now() - Next->HoldingSince;
	if (DispatchNow(Board, Traffic, Network, *Next, Clock.Now()))
	{
		Clearances.Remove(Next->Id);
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) cleared to land after %.0f s holding"),
			Next->Id, *Next->Callsign, FMath::Max(Held, 0.0));
		Result.Cleared = Next;
		bClearedThisQueueFrame = true;
	}
	else
	{
		Result.bRetry = true;
	}
	return Result;
}

bool FArrivalQueue::DispatchNow(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight, double Now)
{
	ArrivalHandles.Remove(Flight.Id);

	if (!Board.Dispatcher)
	{
		UE_LOG(LogAirportOps, Warning,
			TEXT("Flight %d came due with no dispatcher: the board is not attached"), Flight.Id);
		return false;
	}

	// RELEASE BEFORE DISPATCH, and the order is the whole point. ArrivalPlanner asks IsHeld
	// excluding the AGENT, and this hold is under the flight's negative id, so a hold still
	// standing would make the planner refuse this arrival its OWN stand with NoFreeStand -
	// an aeroplane that never appears, for a reason that reads like a full airport.
	if (Board.Allocator != nullptr)
	{
		Board.Allocator->Release(Traffic, Flight);
	}

	if (!Board.Dispatcher(Flight.RunwayPreference, Flight.Airframe))
	{
		// STAYS IN THE QUEUE, STAND RE-HELD (spec 2026-09-28-arrival-queue): this used to leave
		// the flight Accepted with no stand for ever. RARE since the clearance gate: TickQueue
		// only dispatches what the same plan just said yes to, so this is a same-frame race,
		// not a standing condition - retried next frame, never dropped.
		// THROUGH Rehold (#471): the stand a plan taxis it to, not the smallest that fits by size.
		const bool bReheld = Rehold(Board, Traffic, Network, Flight);
		if (!bReheld)
		{
			// THE HOLD WAS GIVEN BACK ABOVE, so a copy still naming it would be a stand the table no longer holds for this
			// flight - and Reconcile's next pass would say "another holder has it" of a stand nobody took (#497 review).
			Flight.Stand = FEntityInstanceId();
		}
		UE_LOG(LogAirportOps, Warning,
			TEXT("Flight %d could not be cleared to land; still holding%s"), Flight.Id,
			bReheld ? TEXT(", stand re-held") : TEXT(" WITH NO STAND - no admitting stand was free to re-hold"));
		++Board.RevisionCount;
		return false;
	}

	// THE LANDING ROW OF TransitionTo hooks the aeroplane (AgentId and ByAgent) and bumps the revision. The stand's RELEASE
	// stays above, before the dispatch - it is not the row's, because the planner must not see the hold - so this row releases
	// nothing.
	Board.TransitionTo(Flight, EFlightPhase::Landing, FTransitionCause::Played(Now).WithAgent(Traffic.GetNewestAgentId()));
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d dispatched as agent %d"), Flight.Id, Flight.AgentId);
	return true;
}

void FArrivalQueue::RestoreStandHolds(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, const TArray<UFlight*>& HoldLast)
{
	if (Board.Allocator == nullptr)
	{
		return;
	}

	TArray<UFlight*> Holding;
	TArray<UFlight*> Last;
	for (const TObjectPtr<UFlight>& Each : Board.LiveFlights())
	{
		// INBOUND TOO: a holding flight keeps its stand, and a graph rebuild takes every claim.
		if (Each != nullptr && Each->IsUnarrived())
		{
			// THE GENUINE HOLDS FIRST (review I1): a flight a load re-queued (#404) names the stand it was ACCEPTED
			// onto, which it gave up at its dispatch - and which another flight may have been accepted onto since.
			// That flight's promise stands; the re-queued one is re-held after it, and refused if it was taken.
			(HoldLast.Contains(Each.Get()) ? Last : Holding).Add(Each);
		}
	}
	Holding.Append(Last);
	Board.Allocator->Reapply(Traffic, Network, Holding);

	// A HOLDING FLIGHT WITHOUT A STAND TAKES ONE NOW (review I1) - one Reapply just refused, or one whose stand is
	// gone (Reapply keeps a dead Stand for the HeldStandLost alert; a re-hold overwrites it) - rather than on the
	// first frame's TickQueue, whose same pass this is: a load must not hand the next
	// accept a stand the queue was owed. After every Reapply, so no genuine hold is beaten to its own stand - and in
	// Holding's order, the genuine holds before the re-queued. THE SAME ROUTINE AN EDIT'S REFUSAL MEETS (#442): Reapply
	// no longer settles a refusal itself, so one conflict has one outcome whichever door it came through.
	// ENFORCED BY: AirportOps.Model.FlightSave.RequeueDoesNotTakeAnAcceptedStand, AirportOps.Model.FlightSave.RequeueOffADeadStandReserves
	ReconcileStandHolds(Board, Traffic, Network, Holding);
}

bool FArrivalQueue::Rehold(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight)
{
	if (Board.Allocator == nullptr)
	{
		return false;
	}
	// THE PLAN'S STAND, as TryAccept holds it (#471) - asked fresh, not read from VerdictFor's cache: that verdict is an
	// offer's, kept while the flight is one, and this flight's offer is long answered. Its own hold does not count against
	// it (ExcludingHolder), which matters to nothing here - a flight re-held has none - but is the clearance gate's rule
	// for the same plan, so the two cannot disagree about which stand this flight could reach.
	// NOT AGAIN UNTIL SOMETHING MOVED - see FReholdMiss: a stand-less flight is offered this every queue pass.
	FReholdMiss Now;
	Now.Network = &Network;
	Now.GuidelineAt = Network.GetGuidelineRevision();
	Now.OccupancyAt = Traffic.OccupancyRevision();
	Now.StandChurnAt = Traffic.StandHoldChangeCount();
	if (const FReholdMiss* Missed = ReholdMisses.Find(Flight.Id); Missed != nullptr && *Missed == Now)
	{
		return false;
	}
	const FArrivalQuote Plan = Board.PlanQuote(Traffic, Network, Flight.Airframe, Flight.RunwayPreference, Flight.HolderId());
	if (Plan.Stand.IsSet() && Board.Allocator->Hold(Traffic, Network, Flight, Plan.Stand))
	{
		ReholdMisses.Remove(Flight.Id);
		return true;
	}
	ReholdMisses.Add(Flight.Id, Now);
	return false;
}

void FArrivalQueue::ReconcileStandHolds(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, TConstArrayView<UFlight*> InOrder)
{
	// THE RULE IS THE ALLOCATOR'S (UStandAllocator::Reconcile), THE RE-HOLD THIS BOARD'S - a plan's stand. A stand given up
	// changes what a row shows, so it moves the board's revision, as any phase change does.
	const auto Reheld = [this, &Board, &Traffic, &Network](UFlight& Flight) { return Rehold(Board, Traffic, Network, Flight); };
	if (Board.Allocator != nullptr && Board.Allocator->Reconcile(Traffic, Network, InOrder, Reheld) > 0)
	{
		++Board.RevisionCount;
	}
}

void FArrivalQueue::RearmSchedules(UFlightBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network,
	USimClock& Clock)
{
	// CANCELLED, not just forgotten (PR #137 review): a handle left in Clock's own queue
	// after this map drops it is an orphaned entry that outlives every reference to it here -
	// harmless against a genuinely fresh post-load Clock (its queue starts empty), but this
	// function has no way to know that is the only time it is ever called, and an entry that
	// fires later still runs its captured lambda against whatever Id it named.
	Board.DisarmEveryArrival(Clock);

	// REBUILT HERE TOO, NOT ONLY IN OnAfterRestore: this is the one call every load path is
	// documented to make (see this function's own header), while OnAfterRestore only runs
	// under OpsSave::Restore. A board deserialised directly - OpsSave::SerializeObject/
	// DeserializeObject, which FFlightSurvivesASaveTest uses on purpose to isolate the clock's
	// own re-arm from the rest of Restore - would otherwise re-arm a Schedule
	// callback whose eventual FindById(Id) found nothing, because ById was still empty.
	// (Serialize's load rebuilds them first since #425; this stays the one rebuild every load path reaches.)
	Board.RebuildIndices();

	// SNAPSHOT, NOT A LIVE ITERATION: DispatchNow below mutates the board, and a range-based
	// for over Flights must not see that happen under it.
	const TArray<TObjectPtr<UFlight>> Loaded = Board.LiveFlights();
	for (const TObjectPtr<UFlight>& Each : Loaded)
	{
		if (Each == nullptr)
		{
			continue;
		}

		// OFFERS NEED NOTHING HERE since snapshot v5: their countdown is OfferSecondsLeft,
		// saved as itself and resumed by TickOffers. The branch that lapsed an offer whose
		// GAME-time window had passed "while the game was shut" went with it - a countdown in
		// real seconds cannot run while the game is not.
		if (Each->GetPhase() != EFlightPhase::Accepted)
		{
			continue;
		}

		if (Each->ArrivesAt <= Clock.Now())
		{
			// Its slot passed while the game was shut. It is still owed an arrival, so it joins
			// the queue at once rather than being dropped - and it says so (Enqueue logs),
			// because a flight that silently never came is the exact bug this function exists
			// to stop. The QUEUE, not a blind dispatch: TickQueue decides whether the runway
			// the loaded game has is free.
			UE_LOG(LogAirportOps, Log,
				TEXT("Flight %d was due at %.0f, before this load at %.0f: holding now"),
				Each->Id, Each->ArrivesAt, Clock.Now());
			Enqueue(Board, *Each, Each->ArrivesAt);
			continue;
		}

		Schedule(Board, Traffic, Clock, *Each);
	}
}

void FArrivalQueue::Disarm(const UFlight& Flight, EFlightPhase To, USimClock* Clock)
{
	// MOVED FROM UFlightBoard::TransitionTo's leaving-Accepted block (#442 item 4), which calls this - the row decides WHEN, the
	// handle is the queue's. Cause.Clock is Clock here.
	// THE ARRIVAL LET GO OF, whichever way the flight leaves Accepted. Cancelling it on the clock is what a cancel needs, or
	// its Clock.At finds the flight by id even in History (ById keeps it until RollUp) and would put a cancelled flight
	// back in the queue. The ETA callback's own entry to Inbound has FIRED already and has no clock to cancel on, which
	// is why a missing Cause.Clock is only a fault when the flight is not joining the queue.
	// ENFORCED BY: AirportOps.Model.FlightBoard.CancelUnarrivedCancelsAndWithdraws ("still Cancelled after its ETA")
	if (const int32* Handle = ArrivalHandles.Find(Flight.Id))
	{
		if (Clock != nullptr)
		{
			Clock->Cancel(*Handle);
		}
		else if (To != EFlightPhase::Inbound)
		{
			UE_LOG(LogAirportOps, Error,
				TEXT("Flight %d left Accepted for %s with no clock to cancel its arrival on - it is still armed"),
				Flight.Id, *FlightPhase::Name(To));
		}
		ArrivalHandles.Remove(Flight.Id);
	}
}

void FArrivalQueue::DisarmEvery(USimClock& Clock)
{
	// THE QUEUE'S SHARE OF UFlightBoard::DisarmEveryArrival (#442 item 4), in its order: the handles cancelled and forgotten, then the
	// two caches keyed on the same ids (the board drops the inbox's verdicts beside this).
	for (const TPair<int32, int32>& Handle : ArrivalHandles) { Clock.Cancel(Handle.Value); }
	ArrivalHandles.Reset();
	Clearances.Reset();
	ReholdMisses.Reset();
}
