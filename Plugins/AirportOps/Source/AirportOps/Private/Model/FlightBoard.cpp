#include "Model/FlightBoard.h"

#include "AirportOpsLog.h"
#include "Model/OpsEventBus.h"
#include "Model/AirsideCapability.h"
#include "Model/ArrivalSequencer.h"
#include "Model/Flight.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/Pricing.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Algo/StableSort.h"

namespace FlightBoardText
{
	/** "TaxiIn", not "EFlightPhase::TaxiIn": a log line a grep for "restored mid-TaxiIn" finds (review M9). */
	FString PhaseName(EFlightPhase Phase)
	{
		return StaticEnum<EFlightPhase>()->GetNameStringByValue(static_cast<int64>(Phase));
	}
}

namespace FlightBoardSave
{
	/**
	 * One list of flights BY VALUE (#425): a count, then each flight's own tagged properties - UObject::Serialize
	 * through the same proxy archive the board itself goes through, so a flight is saved by exactly the rule every
	 * blob is (its non-Transient UPROPERTYs), and ends with the tag terminator that lets the next one follow it.
	 * Loading makes each a NEW UFlight owned by Board. A NULL ENTRY IS NOT WRITTEN: it has no value to save, and every
	 * reader of these lists already skips one.
	 */
	void SerializeFlights(FArchive& Ar, UFlightBoard& Board, TArray<TObjectPtr<UFlight>>& List)
	{
		if (Ar.IsLoading())
		{
			int32 Count = 0;
			Ar << Count;
			// A NEGATIVE COUNT, OR ONE READ PAST THE END, IS A CORRUPT BLOB: the loop would fill flights with garbage.
			// No Reserve(Count) either, for the same reason - a corrupt count must not become a huge allocation.
			if (Ar.IsError() || Count < 0)
			{
				Ar.SetError();
				return;
			}
			for (int32 Index = 0; Index < Count && !Ar.IsError(); ++Index)
			{
				UFlight* Flight = NewObject<UFlight>(&Board);
				Flight->Serialize(Ar);
				// HALF-READ IS NOT RESTORED: the archive failed inside this flight, so its fields are part saved values,
				// part defaults - an offer with no id, or an accepted flight with no stand. Dropped, and so not counted in
				// the corrupt-blob Error, which reports only the flights read whole (#452 review).
				if (Ar.IsError())
				{
					Flight->MarkAsGarbage();
					break;
				}
				List.Add(Flight);
			}
			return;
		}
		TArray<UFlight*> Present;
		for (const TObjectPtr<UFlight>& Each : List)
		{
			if (Each != nullptr)
			{
				Present.Add(Each);
			}
		}
		int32 Count = Present.Num();
		Ar << Count;
		for (UFlight* Each : Present)
		{
			Each->Serialize(Ar);
		}
	}
}

void UFlightBoard::Serialize(FArchive& Ar)
{
	// THE BOARD'S OWN FIELDS FIRST, by the tagged pass (MaxDays, ApproachFocus, NextFlightId). Flights and History
	// are Transient, so it skips them - and so does an old blob's "Flights"/"History" tags on load: a Transient
	// property's saved tag is passed over, which is what makes a v5 blob load at all.
	Super::Serialize(Ar);

	// ONLY A SAVE OR A LOAD CARRIES THE FLIGHTS. A reference collector or a memory count calls Serialize too, and writes
	// nothing anyone reads back; the garbage collector does not come through here at all (it walks the Transient
	// arrays by reflection, which is why Transient costs the flights nothing in lifetime).
	// ENFORCED BY: AirportOps.Model.FlightSave.RestoresByValue ("a collection keeps the restored flights")
	if (!(Ar.IsSaving() || Ar.IsLoading()) || Ar.IsObjectReferenceCollector() || Ar.IsCountingMemory())
	{
		return;
	}

	if (!Ar.IsLoading())
	{
		FlightBoardSave::SerializeFlights(Ar, *this, Flights);
		FlightBoardSave::SerializeFlights(Ar, *this, History);
		return;
	}

	// RETIRED, NOT MERELY DROPPED: a viewmodel row, the inspector's cached lookup, a test's local - anything still
	// holding a flight this load replaces - must read null from here on, not the pre-load state, which is exactly the
	// wrong answer #425 was (a same-session load "restored" each flight as it was NOW). Garbage makes every weak
	// pointer to it null at once; the object itself lives until the next collection, so a raw pointer is not dangling.
	for (TArray<TObjectPtr<UFlight>>* List : { &Flights, &History })
	{
		for (const TObjectPtr<UFlight>& Each : *List)
		{
			if (Each != nullptr)
			{
				Each->MarkAsGarbage();
			}
		}
		List->Reset();
	}

	if (Ar.AtEnd())
	{
		// A BLOB FROM BEFORE v6 ends at the board's tags: its flights were saved as object paths (#425), which name
		// objects no later session has. Nothing to restore, and said once rather than read as an empty board.
		UE_LOG(LogAirportOps, Warning,
			TEXT("Restore: this Flights blob has no flights by value (a snapshot before v6 saved only their paths, #425) - none restored"));
	}
	else
	{
		FlightBoardSave::SerializeFlights(Ar, *this, Flights);
		FlightBoardSave::SerializeFlights(Ar, *this, History);
		if (Ar.IsError())
		{
			// SAID, NOT SWALLOWED: OpsSave::Restore reports success regardless, and a board short of flights is
			// otherwise indistinguishable from one that had fewer.
			UE_LOG(LogAirportOps, Error, TEXT("Restore: the Flights blob is corrupt - kept the %d flight(s) read whole before it failed"),
				Flights.Num() + History.Num());
		}
	}

	// THE INDICES NAMED THE REPLACED OBJECTS, and a restore is a change (UJobBoard::Serialize's idiom): every viewmodel
	// keyed on Revision re-reads, which is what re-points it at the restored flights.
	RebuildIndices();
	++RevisionCount;
	UE_LOG(LogAirportOps, Log, TEXT("Restore: %d flight(s) live, %d in history, by value"), Flights.Num(), History.Num());
}

bool UFlightBoard::DefaultApproachFocus(const URoadNetwork& Network, FVector2D& OutFocus)
{
	const FAirsideCapability Airport = AirsideCapability::Summarise(Network);
	const FRunwaySummary* Longest = nullptr;
	for (const FRunwaySummary& Runway : Airport.Runways)
	{
		if (Longest == nullptr || Runway.End.Length > Longest->End.Length)
		{
			Longest = &Runway;
		}
	}
	if (Longest == nullptr)
	{
		return false;
	}
	OutFocus = Longest->End.Threshold;
	return true;
}

void UFlightBoard::AddOffer(USimClock& Clock, UFlight* Offer)
{
	if (Offer == nullptr)
	{
		return;
	}
	if (Offer->Id <= 0)
	{
		Offer->Id = TakeNextId();
	}
	else
	{
		// An offer that arrived with an id - from the generator, or from a save - must not
		// let the counter hand the same id out again: two flights sharing an id would share
		// a stand hold, and releasing one would free the other's stand.
		NextFlightId = FMath::Max(NextFlightId, Offer->Id + 1);
	}
	Flights.Add(Offer);
	ById.Add(Offer->Id, Offer);
	if (Offer->AgentId != INDEX_NONE)
	{
		// A TEST FIXTURE ROUTE, not a production one today (nothing hands AddOffer a flight
		// that already names an agent) - but FFlightBoardFollowsTheAgentTest sets AgentId
		// BEFORE AddOffer rather than through DispatchNow, and OnAgentPhase's FindByAgent has
		// to find it anyway. DispatchNow's own ByAgent.Add stays the production path.
		ByAgent.Add(Offer->AgentId, Offer);
	}
	if (Offer->Phase == EFlightPhase::Offered)
	{
		// GUARDED, NOT UNCONDITIONAL: AddOffer is also how FFlightBoardFollowsTheAgentTest-style
		// fixtures introduce a flight that is already Landing (an agent mid-flight before the
		// board existed, or a test skipping the accept/dispatch pipeline). Counting one of those
		// as pending would inflate PendingOfferCount() forever - nothing here ever leaves
		// Offered, so nothing would ever decrement it back out.
		++OfferedCount;
	}
	++RevisionCount;
}

void UFlightBoard::TickOffers(const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock, double RealDeltaSeconds)
{
	// PAUSE STOPS IT; SPEED DOES NOT. RealDeltaSeconds is the raw frame time - never scaled
	// by Multiplier() or TimeScale() - so x4 drains exactly as fast as x1.
	if (Clock.IsPaused())
	{
		return;
	}

	// NO OFFERS, NOTHING TO DRAIN (ops batch 3 PR E): the walk below copied Flights every frame to find none. The count is
	// the board's own, kept by AddOffer, every lapse and answer, and rebuilt from Flights with the indices on a load.
	// ENFORCED BY: AirportOps.Model.FlightBoard.EmptyBoardCopiesNothing (an offer added still drains and lapses)
	if (OfferedCount == 0)
	{
		return;
	}

	// SNAPSHOT: a lapse calls MoveToHistory, which removes from the array being walked.
	const TArray<TObjectPtr<UFlight>> Snapshot = Flights;
	++OfferSnapshots;   // See OfferSnapshotCountForTest.
	for (const TObjectPtr<UFlight>& Each : Snapshot)
	{
		if (Each == nullptr || Each->Phase != EFlightPhase::Offered)
		{
			continue;
		}
		// JUDGED BEFORE IT DRAINS, so an offer acceptable in its last frame counts as one the
		// player could have taken.
		if (VerdictFor(Traffic, Network, *Each).Why == EArrivalRefusal::None)
		{
			Each->bWasEverAcceptable = true;
		}
		Each->OfferSecondsLeft -= FMath::Max(RealDeltaSeconds, 0.0);
		if (Each->OfferSecondsLeft > 0.0)
		{
			continue;
		}
		Each->OfferSecondsLeft = 0.0;
		Each->Phase = EFlightPhase::Expired;
		Each->LapseReason = Each->bWasEverAcceptable ? ELapseReason::Ignored : ELapseReason::NeverAcceptable;
		--OfferedCount;
		UE_LOG(LogAirportOps, Log, TEXT("Offer %d (%s) lapsed unanswered (%s)"), Each->Id, *Each->Callsign,
			Each->LapseReason == ELapseReason::Ignored ? TEXT("ignored") : TEXT("never acceptable"));
		if (Bus != nullptr)
		{
			Bus->Publish(FOfferExpiredEvent{ Each->Id, Each->AirlineId, Each->LapseReason, Each->bFloorAirline });
		}
		MoveToHistory(*Each, Clock.Now());
		++RevisionCount;
	}
}

const FOfferVerdict& UFlightBoard::VerdictFor(const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const UFlight& Flight) const
{
	FOfferVerdict& Verdict = Verdicts.FindOrAdd(Flight.Id);
	const uint32 BoardNow = Revision();
	const uint32 GuidelineNow = Network.GetGuidelineRevision();
	const uint32 OccupancyNow = Traffic.OccupancyRevision();
	const uint32 FleetNow = Fuel != nullptr ? Fuel->GetFleetRevision() : 0;
	if (!Verdict.bValid || Verdict.BoardAt != BoardNow || Verdict.GuidelineAt != GuidelineNow
		|| Verdict.OccupancyAt != OccupancyNow || Verdict.FleetAt != FleetNow)
	{
		// THE REAL PLAN, with the live occupancy. The greyed-out reason is the sentence the
		// arrival itself would print, because it is the same refusal.
		Verdict.Why = WhyNotAcceptable(Traffic, Network, Flight);
		Verdict.bFuelServable = Fuel == nullptr || Fuel->CouldServe(Network, Flight.Airframe);
		Verdict.BoardAt = BoardNow;
		Verdict.GuidelineAt = GuidelineNow;
		Verdict.OccupancyAt = OccupancyNow;
		Verdict.FleetAt = FleetNow;
		Verdict.bValid = true;
	}
	return Verdict;
}

int32 UFlightBoard::TakeNextId()
{
	return NextFlightId++;
}

bool UFlightBoard::Accept(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock,
	UFlight& Flight)
{
	if (Flight.Phase != EFlightPhase::Offered || Allocator == nullptr)
	{
		return false;
	}
	// NOT OPEN, NOTHING ACCEPTED (ruling I1): before the stand is held, so a refusal holds nothing.
	if (AdmitsArrivals && !AdmitsArrivals())
	{
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d not accepted: the airport is not open"), Flight.Id);
		return false;
	}

	if (!Allocator->Reserve(Traffic, Network, Flight))
	{
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d not accepted: no stand admits it"), Flight.Id);
		return false;
	}

	Flight.Phase = EFlightPhase::Accepted;
	--OfferedCount;

	// THE LEAD TIME RUNS FROM THE ACCEPT, not from the offer: a player who took most of the
	// window to decide still gets the whole lead, and the contract is measured from here.
	Flight.AcceptedAt = Clock.Now();
	Flight.ArrivesAt = Flight.AcceptedAt + Flight.LeadTimeSeconds;
	Schedule(Traffic, Clock, Flight);

	UE_LOG(LogAirportOps, Log, TEXT("Flight %d accepted: stand %d held, landing at %.0f"),
		Flight.Id, Flight.Stand.Index, Flight.ArrivesAt);
	++RevisionCount;
	// PUBLISHED HERE, IN THE BOARD, not by the inbox that called it: Accept is a player command reached
	// straight from the game module, and the key-7 path (AcceptImmediate) comes through here too - one
	// publisher for every accept. After the hold, so a refusal above publishes nothing.
	// ENFORCED BY: AirportOps.Model.FlightBoard.AcceptPublishesOfferAccepted
	if (Bus != nullptr)
	{
		Bus->Publish(FOfferAcceptedEvent{ Flight.Id, Flight.AirlineId, Flight.Stand });
	}
	return true;
}

void UFlightBoard::Schedule(UGroundTraffic& Traffic, USimClock& Clock, UFlight& Flight)
{
	// WEAK, not a captured reference. The callback outlives this call by design - minutes of
	// game time - and the traffic model belongs to an actor that a level change can take
	// away underneath it.
	TWeakObjectPtr<UGroundTraffic> WeakTraffic = &Traffic;
	const int32 Id = Flight.Id;

	const int32 Handle = Clock.At(Flight.ArrivesAt, [this, WeakTraffic, Id]()
	{
		UFlight* Due = FindById(Id);
		// ONLY AN ACCEPTED FLIGHT JOINS THE QUEUE (whole-stack review M3): FindById finds it in History too, and every
		// cancel disarming this callback first is a rule of the callers, not of this line. A flight that has become
		// anything else since it was armed is not due.
		// ENFORCED BY: AirportOps.Model.FlightBoard.ArrivalRequeuesOnlyAnAccepted
		if (WeakTraffic.Get() != nullptr && Due != nullptr && Due->Phase == EFlightPhase::Accepted)
		{
			// INTO THE QUEUE, not straight onto the runway (spec 2026-09-28-arrival-queue): the
			// runway may be busy, and TickQueue is what decides when it is not. HoldingSince is
			// the ETA - the moment this callback exists to mark.
			Enqueue(*Due, Due->ArrivesAt);
		}
	});
	ArrivalHandles.Add(Flight.Id, Handle);
}

void UFlightBoard::Enqueue(UFlight& Flight, double Since)
{
	ArrivalHandles.Remove(Flight.Id);
	Flight.Phase = EFlightPhase::Inbound;
	Flight.HoldingSince = Since;
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) holding: #%d in queue"),
		Flight.Id, *Flight.Callsign, Queue().Find(&Flight) + 1);
	++RevisionCount;
	// THE QUEUE PASS'S WAKE-UP (ops batch 3 §5): the queue is no longer ticked every frame, so a flight joining it
	// says so. Here, the one site every Inbound flight passes through while the game runs; a load's re-queue is
	// covered by the load's MarkAllDirty instead.
	// ENFORCED BY: AirportOps.Model.FlightBoard.EnqueuePublishesInbound
	if (Bus != nullptr)
	{
		Bus->Publish(FFlightInboundEvent{ Flight.Id, Flight.AirlineId });
	}
}

TArray<UFlight*> UFlightBoard::Queue() const
{
	TArray<UFlight*> Out;
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each != nullptr && Each->Phase == EFlightPhase::Inbound)
		{
			Out.Add(Each);
		}
	}
	// DERIVED, NOT STORED: the queue is the holding flights in the order they joined, ties by
	// id - one list (Flights), so a save needs nothing new and nothing can drift out of step.
	Algo::StableSortBy(Out, [](const UFlight* F) { return F->HoldingSince; });
	return Out;
}

EArrivalRefusal UFlightBoard::ClearanceFor(const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const UFlight& Flight)
{
	FClearance& Clearance = Clearances.FindOrAdd(Flight.Id);
	const uint32 GuidelineNow = Network.GetGuidelineRevision();
	const uint32 OccupancyNow = Traffic.OccupancyRevision();
	if (Clearance.bValid && Clearance.GuidelineAt == GuidelineNow && Clearance.OccupancyAt == OccupancyNow)
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
	const EArrivalRefusal Why = ArrivalPlanner::Plan(Network, Flight.ApproachFocus, Flight.Airframe,
		&Traffic.GetOccupancy(), ERunwayBusy::Queue, Flight.HolderId()).Why;
	if (Why != EArrivalRefusal::None && (!Clearance.bValid || Why != Clearance.Why))
	{
		// ONCE PER REASON, not per frame - the reason is the evidence, the repetition is noise.
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) holding: cannot land yet - %s"),
			Flight.Id, *Flight.Callsign, *ArrivalPlanner::DescribeRefusal(Why, Flight.Airframe.Wingspan));
	}
	Clearance.Why = Why;
	Clearance.GuidelineAt = GuidelineNow;
	Clearance.OccupancyAt = OccupancyNow;
	Clearance.bValid = true;
	return Why;
}

FQueueTick UFlightBoard::TickQueue(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
{
	++TickQueueCalls;
	FQueueTick Result;
	// COUNTED BEFORE THE PAUSE TEST: a paused queue is still a queue, and the pass arms its safety net from this.
	const TArray<UFlight*> Waiting = Queue();
	Result.Waiting = Waiting.Num();
	// A CLOSED AIRPORT ADMITS NO ARRIVALS - HERE TOO (whole-stack review I1): Accept and Land were gated, and this, the
	// door onto the runway itself, was not; a flight left holding by a load or a closure's edge case landed anyway. The
	// same predicate as Accept, so the two doors cannot disagree about what "open" is. Before the stand re-reserve: a
	// closed airport takes no stand for a flight it will not land. AND BEFORE THE PAUSE TEST (re-review m6): bClosed is
	// what keeps the pass from arming a safety net, and a closed airport paused is still closed.
	// ENFORCED BY: AirportOps.Present.ArrivalQueue.ClosedAirportDispatchesNothing
	if (AdmitsArrivals && !AdmitsArrivals())
	{
		Result.bClosed = true;
		return Result;
	}
	if (Clock.IsPaused() || Waiting.Num() == 0)
	{
		return Result;
	}

	// A HOLDING FLIGHT WITH NO STAND TAKES ONE BACK the moment one is free (review I1): the
	// stand is what makes an accept safe, and a holder without one could land on a stand the
	// next accept was promised. Reserve is a stand walk, no route search.
	//
	// A DEAD STAND COUNTS AS NONE (PR C's follow-up, done in PR D): Reapply keeps a deleted stand on the flight as
	// HeldStandLost's evidence, and a load that found no free stand leaves it there - so an Inbound flight naming a
	// gone stand must take one back here, the first time a stand frees, exactly as OnGraphRebuilt's load-time pass
	// does. Reserve overwrites the dead Stand, which clears the alert by the same test.
	// ENFORCED BY: AirportOps.Model.ArrivalQueue.DeadStandReservesWhenOneFrees
	if (Allocator != nullptr)
	{
		for (UFlight* Each : Waiting)
		{
			if ((!Each->Stand.IsSet() || UStandAllocator::HeldStandIsGone(*Each, Network))
				&& Allocator->Reserve(Traffic, Network, *Each))
			{
				UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) holding: stand %d held again"),
					Each->Id, *Each->Callsign, Each->Stand.Index);
			}
		}
	}

	// LIVE RUNWAY, CACHED REST - see ClearanceFor. Together they are exactly the Refuse plan the
	// dispatch will make, so nothing is dispatched that it would refuse.
	const auto CanClear = [this, &Traffic, &Network](const UFlight& F)
	{
		return !ArrivalPlanner::IsRunwayBusy(Network, F.ApproachFocus, &Traffic.GetOccupancy())
			&& ClearanceFor(Traffic, Network, F) == EArrivalRefusal::None;
	};
	// NULL SEQUENCER IS STRICT FIRST COME, for a test that does not wire one.
	UFlight* Next = Sequencer != nullptr ? Sequencer->Next(Waiting, CanClear)
		: (CanClear(*Waiting[0]) ? Waiting[0] : nullptr);
	if (Next == nullptr)
	{
		return Result;
	}
	// ONE CLEARANCE A FRAME: the aircraft just cleared claims the runway on its first tick, and
	// a second clearance this frame would be decided before that claim exists. The pass may run
	// twice in one drain, so UOpsRuntime::RunArrivalQueue keeps the rule across its rounds.
	// ENFORCED BY: AirportOps.Present.ArrivalQueue.SecondRunwayNextFrame
	const double Held = Clock.Now() - Next->HoldingSince;
	if (DispatchNow(Traffic, Network, *Next))
	{
		Clearances.Remove(Next->Id);
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) cleared to land after %.0f s holding"),
			Next->Id, *Next->Callsign, FMath::Max(Held, 0.0));
		Result.Cleared = Next;
	}
	else
	{
		Result.bRetry = true;
	}
	return Result;
}

bool UFlightBoard::DispatchNow(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight)
{
	ArrivalHandles.Remove(Flight.Id);

	if (!Dispatcher)
	{
		UE_LOG(LogAirportOps, Warning,
			TEXT("Flight %d came due with no dispatcher: the board is not attached"), Flight.Id);
		return false;
	}

	// RELEASE BEFORE DISPATCH, and the order is the whole point. ArrivalPlanner asks IsHeld
	// excluding the AGENT, and this hold is under the flight's negative id, so a hold still
	// standing would make the planner refuse this arrival its OWN stand with NoFreeStand -
	// an aeroplane that never appears, for a reason that reads like a full airport.
	if (Allocator != nullptr)
	{
		Allocator->Release(Traffic, Flight);
	}

	if (!Dispatcher(Flight.ApproachFocus, Flight.Airframe))
	{
		// STAYS IN THE QUEUE, STAND RE-HELD (spec 2026-09-28-arrival-queue): this used to leave
		// the flight Accepted with no stand for ever. RARE since the clearance gate: TickQueue
		// only dispatches what the same plan just said yes to, so this is a same-frame race,
		// not a standing condition - retried next frame, never dropped.
		const bool bReheld = Allocator != nullptr && Allocator->Reserve(Traffic, Network, Flight);
		UE_LOG(LogAirportOps, Warning,
			TEXT("Flight %d could not be cleared to land; still holding%s"), Flight.Id,
			bReheld ? TEXT(", stand re-held") : TEXT(" WITH NO STAND - no admitting stand was free to re-hold"));
		++RevisionCount;
		return false;
	}

	Flight.AgentId = Traffic.GetNewestAgentId();
	ByAgent.Add(Flight.AgentId, &Flight);
	Flight.Phase = EFlightPhase::Landing;
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d dispatched as agent %d"), Flight.Id, Flight.AgentId);
	++RevisionCount;
	return true;
}

void UFlightBoard::Decline(USimClock& Clock, UFlight& Flight)
{
	if (Flight.Phase != EFlightPhase::Offered)
	{
		return;
	}
	Flight.Phase = EFlightPhase::Declined;
	--OfferedCount;
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d declined"), Flight.Id);
	if (Bus != nullptr)
	{
		Bus->Publish(FOfferDeclinedEvent{ Flight.Id, Flight.AirlineId });
	}
	MoveToHistory(Flight, Clock.Now());
	++RevisionCount;
}

bool UFlightBoard::CancelByAgent(int32 AgentId, double Now)
{
	UFlight* Flight = FindByAgent(AgentId);
	if (Flight == nullptr)
	{
		return false;
	}
	const EFlightPhase WasPhase = Flight->Phase;
	Flight->Phase = EFlightPhase::Cancelled;
	// UNHOOKED BEFORE MoveToHistory, as OnAgentPhase's Gone branch does - see MoveToHistory's
	// DEFENSIVE comment for why a ByAgent entry must not outlive its flight.
	ByAgent.Remove(AgentId);
	Flight->AgentId = INDEX_NONE;
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s): %s -> Cancelled (agent %d despawned)"),
		Flight->Id, *Flight->Callsign, *UEnum::GetValueAsString(WasPhase), AgentId);
	// THE SECOND PUBLISHER of FlightCancelled (spec 2026-09-29-ops-batch3 §3): the roster hears it and charges
	// nothing for Unstuck - heard, so the log and any later reaction see every cancellation the same way.
	// ENFORCED BY: AirportOps.Model.FlightBoard.CancelByAgentPublishesUnstuck
	if (Bus != nullptr)
	{
		Bus->Publish(FFlightCancelledEvent{ Flight->Id, Flight->AirlineId, ECancelReason::Unstuck });
	}
	MoveToHistory(*Flight, Now);
	++RevisionCount;
	return true;
}

int32 UFlightBoard::CancelUnarrived(UGroundTraffic& Traffic, USimClock& Clock, ECancelReason Reason)
{
	int32 Cancelled = 0;
	int32 Withdrawn = 0;
	// SNAPSHOT: MoveToHistory removes from the array being walked.
	const TArray<TObjectPtr<UFlight>> Snapshot = Flights;
	for (const TObjectPtr<UFlight>& Each : Snapshot)
	{
		if (Each == nullptr)
		{
			continue;
		}
		UFlight& Flight = *Each;
		if (Flight.Phase == EFlightPhase::Offered)
		{
			Flight.Phase = EFlightPhase::Withdrawn;
			--OfferedCount;
			++Withdrawn;
			MoveToHistory(Flight, Clock.Now());
			continue;
		}
		if (Flight.Phase != EFlightPhase::Accepted && Flight.Phase != EFlightPhase::Inbound)
		{
			// LANDING AND LATER FINISH: committed to the runway, or on the ground being drained.
			continue;
		}
		// THE ARRIVAL DISARMED, not merely forgotten: its Clock.At finds the flight by id even in History (ById
		// keeps it until RollUp) and would put a cancelled flight back in the queue. Inbound has none left.
		// ENFORCED BY: AirportOps.Model.FlightBoard.CancelUnarrivedCancelsAndWithdraws ("still Cancelled after its ETA")
		if (const int32* Handle = ArrivalHandles.Find(Flight.Id))
		{
			Clock.Cancel(*Handle);
			ArrivalHandles.Remove(Flight.Id);
		}
		if (Allocator != nullptr)
		{
			Allocator->Release(Traffic, Flight);
		}
		const EFlightPhase WasPhase = Flight.Phase;
		Flight.Phase = EFlightPhase::Cancelled;
		++Cancelled;
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s): %s -> Cancelled (%s)"), Flight.Id, *Flight.Callsign,
			*UEnum::GetValueAsString(WasPhase), *UEnum::GetValueAsString(Reason));
		if (Bus != nullptr)
		{
			Bus->Publish(FFlightCancelledEvent{ Flight.Id, Flight.AirlineId, Reason });
		}
		MoveToHistory(Flight, Clock.Now());
	}
	if (Cancelled + Withdrawn > 0)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Airport not open (%s): %d flight(s) cancelled, %d offer(s) withdrawn"),
			*UEnum::GetValueAsString(Reason), Cancelled, Withdrawn);
		++RevisionCount;
	}
	return Cancelled;
}

int32 UFlightBoard::UnarrivedCount() const
{
	// A SCAN OF Flights, not a maintained counter: asked by the bar's confirm when the popup opens, and Flights
	// holds only the live flights (~10 on 2026-09-29, terminal ones move to History at once).
	int32 Count = 0;
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		Count += Each != nullptr && (Each->Phase == EFlightPhase::Accepted || Each->Phase == EFlightPhase::Inbound);
	}
	return Count;
}

int32 UFlightBoard::OnGroundCount() const
{
	// LANDING THROUGH DEPARTING - what a closure leaves to finish (CancelUnarrived's "untouched"). The same range
	// Live() reads, minus the two a closure cancels. A scan, for UnarrivedCount's reason.
	int32 Count = 0;
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		Count += Each != nullptr && Each->Phase >= EFlightPhase::Landing && Each->Phase <= EFlightPhase::Departing;
	}
	return Count;
}

EArrivalRefusal UFlightBoard::WhyNotAcceptable(const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const UFlight& Flight) const
{
	// #169: counted before the search runs, not after - GetWhyNotAcceptableCallsForTest exists
	// to measure exactly how often this expensive call is reached, whatever it returns.
	++WhyNotAcceptableCallsForTest;
	// QUEUE, not Refuse: a busy runway is something an accepted flight waits for (spec
	// 2026-09-28-arrival-queue section 1), so the row greys out only on what waiting cannot fix
	// - a stand above all - and never on RunwayOccupied.
	const FArrivalPlan Plan = ArrivalPlanner::Plan(Network, Flight.ApproachFocus, Flight.Airframe,
		&Traffic.GetOccupancy(), ERunwayBusy::Queue);
	return Plan.Why;
}

EArrivalRefusal UFlightBoard::AcceptImmediate(UGroundTraffic& Traffic, const URoadNetwork& Network,
	USimClock& Clock, const FAirframe& Airframe, const FVector2D& Focus, FText Airline)
{
	UFlight* Flight = NewObject<UFlight>(this);
	Flight->Airframe = Airframe;
	Flight->AirlineName = Airline;
	Flight->TypeName = FText::FromName(Airframe.TypeCode);

	// NO LEAD TIME: AcceptImmediate exists to put an aeroplane on the field this second - see
	// its own header. The one-second window is never drained: the accept below is this call.
	Flight->LeadTimeSeconds = 0.0;
	// THE FUEL SERVICE'S OWN FALLBACK, not a second figure: key 7 was never offered.
	Flight->FuelLitres = UJobBoard::DefaultLitres(Airframe);
	Flight->OfferWindowSeconds = 1.0;
	Flight->OfferSecondsLeft = 1.0;
	Flight->ApproachFocus = Focus;

	AddOffer(Clock, Flight);

	if (Accept(Traffic, Network, Clock, *Flight))
	{
		return EArrivalRefusal::None;
	}

	// Says WHICH refusal, the same sentence the inbox would show for it - see
	// ArrivalPlanner::DescribeRefusal. The flight is left in the inbox rather than removed:
	// an offer nobody could accept yet is exactly what the board already does for one the
	// generator makes, and a player watching the inbox sees the same row either way.
	return WhyNotAcceptable(Traffic, Network, *Flight);
}

void UFlightBoard::OnAfterRestore(int32 SnapshotVersion)
{
	// Flights.Num() replaces the bHadFlights flag OpsSave::Restore used to keep around this
	// call: a board with no flights has nothing to migrate either way, so asking its own state
	// answers the same question without Restore having to remember which blobs it saw - which
	// is what let Restore become a plain loop over every persistent object.
	if (SnapshotVersion < 3 && Flights.Num() > 0)
	{
		AimUnaimedFlightsAtBoardFocus();
	}

	// SWEEP UNCONDITIONALLY, NOT GATED ON SnapshotVersion < 4 (issue #188): every save before
	// History existed put every flight ever created in Flights, terminal or not, because there
	// was nowhere else for one to go - and an unconditional sweep is self-healing if a future
	// change ever finds a second way a terminal flight ends up back in Flights, rather than
	// growing a second version check to remember here.
	//
	// SNAPSHOT FIRST: MoveToHistory mutates Flights, and this loop is walking it.
	const TArray<TObjectPtr<UFlight>> Loaded = Flights;
	for (const TObjectPtr<UFlight>& Each : Loaded)
	{
		if (Each == nullptr)
		{
			continue;
		}
		if (Each->Phase == EFlightPhase::Declined || Each->Phase == EFlightPhase::Expired
			|| Each->Phase == EFlightPhase::Departed || Each->Phase == EFlightPhase::Cancelled
			|| Each->Phase == EFlightPhase::Withdrawn)
		{
			// TerminatedAt DID NOT EXIST before this change, so a flight loaded from a save
			// that predates it has none - ArrivesAt is the closest recorded moment to when it
			// actually finished, and is only ever a fallback for THIS one-time migration: a
			// flight retired after this change always carries the real TerminatedAt its own
			// call site set - see Decline, TickOffers, and OnAgentPhase below. (ExpiresAt, the
			// closer figure for a lapsed offer, went with snapshot v5.)
			MoveToHistory(*Each, Each->ArrivesAt);
		}
	}

	RebuildIndices();
}

TArray<UFlight*> UFlightBoard::DemoteRestoredMidFlight(double Now)
{
	// #404: AGENTS ARE NOT SAVED - UOpsRuntime::LoadFromSlot clears every one before OpsSave::Restore - so a flight
	// saved past Inbound comes back naming an aeroplane that does not exist, and nothing would ever move it again.
	// Ruled (spec 2026-09-29-ops-batch3 §0, §4): the arrivals side GOES ROUND AGAIN, the ground side RETIRES AS
	// DEPARTED. CALLED BY LoadFromSlot, beside the ClearAgents that causes it (review ruling M5) - not from
	// OnAfterRestore, which is handed no clock and runs for every restore, agents cleared or not.
	// ENFORCED BY: AirportOps.Model.FlightSave.MidFlightGoesRoundOrRetires, AirportOps.Present.RuntimeLoad.MidFlightRequeuesOrRetires
	TArray<UFlight*> Requeued;
	int32 Retired = 0;
	// SNAPSHOT: MoveToHistory mutates Flights, and this loop is walking it.
	const TArray<TObjectPtr<UFlight>> Loaded = Flights;
	for (const TObjectPtr<UFlight>& Each : Loaded)
	{
		if (Each == nullptr)
		{
			continue;
		}
		if (Each->Phase == EFlightPhase::Landing || Each->Phase == EFlightPhase::TaxiIn)
		{
			// INBOUND, at the BACK of the queue (HoldingSince = now, not its old ETA: the flights that were already
			// holding when the game was saved were waiting first). Its Stand is still the one it was accepted onto;
			// OnGraphRebuilt holds it again AFTER every genuine holder, and finds it another if that one was taken.
			// bLandingFeePaid IS LEFT AS SAVED rather than forced true: it is saved and set with the ledger post
			// (PostLandingFee), so a flight that was charged stays charged and its second landing posts nothing - and
			// one saved before its landing was heard is charged once, when it lands.
			// ENFORCED BY: AirportOps.Model.FlightSave.UnchargedLandingIsChargedOnce
			UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) restored mid-%s: re-queued (its aircraft, agent %d, was not saved)"),
				Each->Id, *Each->Callsign, *FlightBoardText::PhaseName(Each->Phase), Each->AgentId);
			ByAgent.Remove(Each->AgentId);
			Each->AgentId = INDEX_NONE;
			Each->Phase = EFlightPhase::Inbound;
			Each->HoldingSince = Now;
			Requeued.Add(Each);
		}
		else if (Each->Phase >= EFlightPhase::Turnaround && Each->Phase <= EFlightPhase::Departing)
		{
			// DEPARTED, UNSCORED: the save system is not the player's fault, so nothing the airline roster scores is
			// published (no FlightAirborne, no TurnaroundEnded) and no parking fee is posted. The range reads
			// EFlightPhase's load-bearing declaration order: Turnaround, Manoeuvring, TaxiOut, Departing.
			UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) restored mid-%s: retired as departed (its aircraft, agent %d, was not saved)"),
				Each->Id, *Each->Callsign, *FlightBoardText::PhaseName(Each->Phase), Each->AgentId);
			ByAgent.Remove(Each->AgentId);
			Each->AgentId = INDEX_NONE;
			Each->Phase = EFlightPhase::Departed;
			MoveToHistory(*Each, Now);
			++Retired;
		}
	}
	if (Requeued.Num() + Retired > 0)
	{
		++RevisionCount;
	}
	return Requeued;
}

int32 UFlightBoard::CancelUnarrivedAtLoad(double Now)
{
	// REVIEW RULING I2 (PR C), WIDENED (whole-stack review I1): a flight a load leaves still to arrive, at an airport
	// that is not open, can never land - a closed airport admits no arrivals (PR B ruling I1). The re-queued (Inbound)
	// were the first case found; an Accepted flight saved at the closed airport is the other. CANCELLED, UNSCORED:
	// NOTHING IS PUBLISHED, so the roster charges neither ClosureCancelPenalty nor anything else; what closed the
	// airport happened before the save, and its own cancellations were scored then. Called before RearmSchedules and
	// OnGraphRebuilt, so no arrival is armed and no stand held for any of them.
	// ENFORCED BY: AirportOps.Present.RuntimeLoad.MidFlightAtClosedAirport, AirportOps.Present.Airport.ClosedLoadCancelsTheUnarrived
	int32 Count = 0;
	// SNAPSHOT: MoveToHistory removes from the array being walked.
	const TArray<TObjectPtr<UFlight>> Snapshot = Flights;
	for (const TObjectPtr<UFlight>& Each : Snapshot)
	{
		if (Each == nullptr || (Each->Phase != EFlightPhase::Inbound && Each->Phase != EFlightPhase::Accepted))
		{
			continue;
		}
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) %s at an airport that is not open, after a load: cancelled, unscored"),
			Each->Id, *Each->Callsign, Each->Phase == EFlightPhase::Inbound ? TEXT("holding") : TEXT("accepted"));
		Each->Phase = EFlightPhase::Cancelled;
		MoveToHistory(*Each, Now);
		++Count;
	}
	if (Count > 0)
	{
		++RevisionCount;
	}
	return Count;
}

void UFlightBoard::RebuildIndices()
{
	// THE MAPS AND THE COUNTER (see their own comments) ARE NOT UPROPERTYs and so never
	// survive a load - rebuilt here from Flights/History, which ARE the saved truth, rather
	// than at every FindByAgent/FindById/PendingOfferCount call, which is the whole point of
	// maintaining them at all.
	//
	// CALLED FROM Serialize's LOAD TOO since #425 (the flights these maps named were replaced there).
	// CALLED FROM BOTH OnAfterRestore AND RearmSchedules, not just the first: OpsSave::Restore
	// calls OnAfterRestore, but FFlightSurvivesASaveTest - and anything else that goes through
	// OpsSave::SerializeObject/DeserializeObject directly rather than the full Restore - never
	// does. RearmSchedules is already documented as "call this after a load, unconditionally",
	// so it is the one place that can be relied on to leave these maps correct regardless of
	// which deserialising path got the flights here. Idempotent either way: this only ever
	// reads Flights/History and rebuilds from scratch.
	ByAgent.Reset();
	ById.Reset();
	OfferedCount = 0;
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each == nullptr)
		{
			continue;
		}
		ById.Add(Each->Id, Each);
		if (Each->AgentId != INDEX_NONE)
		{
			ByAgent.Add(Each->AgentId, Each);
		}
		if (Each->Phase == EFlightPhase::Offered)
		{
			++OfferedCount;
		}
	}
	for (const TObjectPtr<UFlight>& Each : History)
	{
		if (Each != nullptr)
		{
			ById.Add(Each->Id, Each);
		}
	}
}

void UFlightBoard::AimUnaimedFlightsAtBoardFocus()
{
	// A LOAD-ONLY MIGRATION for a snapshot older than FOpsSnapshot::Version 3 - see
	// OnAfterRestore, the one caller. Before UFlight::ApproachFocus existed (issue #96) every flight
	// shared this one board-wide field, so a v1/v2 blob's flights have no per-flight focus
	// at all; tagged-property load leaves the new field at FVector2D::ZeroVector, which
	// would aim every restored flight at the world origin rather than wherever it was
	// actually saved aimed.
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each != nullptr)
		{
			Each->ApproachFocus = ApproachFocus;
		}
	}
}

void UFlightBoard::PostLandingFee(double Now, UFlight& Flight)
{
	// bLandingFeePaid AND NOT "is the phase Landing": OnAgentPhase fires for every agent phase
	// change, and more than one of them can map to EFlightPhase::Landing. A flag on the flight
	// is the only thing that survives a reload as well - see UFlight::bLandingFeePaid.
	if (Ledger == nullptr || Flight.LandingFee <= 0.0 || Flight.bLandingFeePaid)
	{
		return;
	}

	Flight.bLandingFeePaid = true;
	Ledger->Post(Now, ELedgerCategory::LandingFee, Flight.LandingFee,
		FText::Format(NSLOCTEXT("Ledger", "LandingBy", "Landing: {0}"), Flight.AirlineName));
}

void UFlightBoard::PostParkingFee(double Now, UFlight& Flight)
{
	// ParkedAt of zero means it never parked - see UFlight::ParkedAt for the two ways a flight
	// reaches TaxiOut without having done so, and for what billing from the epoch would cost.
	if (Ledger == nullptr || Pricing == nullptr || Flight.ParkedAt <= 0.0)
	{
		return;
	}

	const double Hours = FMath::Max(0.0, (Now - Flight.ParkedAt) / 3600.0);
	const double Fee = Pricing->ParkingFeePerHour(Flight.Airframe) * Hours;
	if (Fee <= 0.0)
	{
		return;
	}

	Flight.ParkingFee = Fee;
	Ledger->Post(Now, ELedgerCategory::ParkingFee, Fee,
		FText::Format(NSLOCTEXT("Ledger", "ParkingBy", "Parking: {0}"), Flight.AirlineName));
}

void UFlightBoard::OnAgentPhase(const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock, int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	UFlight* Flight = FindByAgent(AgentId);
	if (Flight == nullptr)
	{
		// A fuel truck, or an aeroplane from before this board existed. Not an error: the
		// board hears every agent's phase and owns only some of them.
		return;
	}

	// SAID, every change (issue #396): a panel that seemed to desync after a stranding could be
	// neither confirmed nor ruled out from the log, because nothing here logged a phase. Changes
	// only - OnAgentPhase hears every agent phase, and most move a flight nowhere.
	const EFlightPhase WasPhase = Flight->Phase;

	// WHERE THE AEROPLANE IS NOW, read live off the agent - the event is a fact about the past (the bus delivers
	// it a step late), exactly as UJobBoard::OnAgentPhase reads it, and through the same StandAtGoal (review M8).
	// The fallback junction a stand-less arrival waits on (UGroundTraffic::ReResolvePlan) is no stand's pose.
	const FRoadAgent* Agent = Traffic.FindAgent(AgentId);
	const bool bStillParked = Agent != nullptr && Agent->Phase == EAgentPhase::Parked;
	const FEntityInstanceId GoalStand = Agent != nullptr ? StandAtGoal(Network, *Agent) : FEntityInstanceId();

	EFlightPhase Next = FlightPhaseFromAgent(To, WasPhase);
	if (WasPhase < EFlightPhase::Turnaround)
	{
		if (To == EAgentPhase::Parked && !(bStillParked && GoalStand.IsSet()))
		{
			// #405: A TURNAROUND IS TIME ON A STAND. Parked on the fallback junction - or a Parked the agent has
			// already left (ReofferStands redirected it in the same frame) - the flight is still taxiing in, so the
			// re-offer's Parked -> Taxiing reads TaxiIn below rather than TaxiOut. No parking clock, no turnaround:
			// UJobBoard::OnAgentPhase opens none there either, by the same StandAtGoal.
			// ENFORCED BY: AirportOps.Model.Bus.FallbackParkStaysTaxiIn, AirportOps.Model.Bus.SameFrameRedirectStaysTaxiIn
			Next = WasPhase;
			UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s): its aircraft (agent %d) stopped short of a stand; still %s"),
				Flight->Id, *Flight->Callsign, AgentId, *FlightBoardText::PhaseName(WasPhase));
		}
		else if (To == EAgentPhase::Taxiing && From == EAgentPhase::Parked && Agent != nullptr && Agent->bDepartureArmed)
		{
			// THE MIRROR: leaving the junction ARMED FOR A DEPARTURE is the taxi out - the inspector's Depart driving
			// straight out (UGroundTraffic::DepartAgent's RedirectAgent branch, whose TakeGoal arms the route that ends
			// on a runway). FlightPhaseFromAgent reads "never reached Turnaround" as the taxi in; without this, that
			// departure would read TaxiIn. THE POSITIVE FACT, not "its goal is no stand" (review M1): a redirect whose
			// new stand is deleted before the event is heard has no stand goal either, and is still taxiing in. A
			// pushback enters Manoeuvring, which maps absolutely.
			// ENFORCED BY: AirportOps.Model.Bus.DepartFromFallbackReadsTaxiOut, AirportOps.Model.Bus.RedirectStaysTaxiInWhenItsStandGoes
			Next = EFlightPhase::TaxiOut;
		}
	}
	Flight->Phase = Next;
	if (Flight->Phase != WasPhase)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s): %s -> %s (agent %d %s -> %s)"),
			Flight->Id, *Flight->Callsign, *UEnum::GetValueAsString(WasPhase), *UEnum::GetValueAsString(Flight->Phase),
			AgentId, *UEnum::GetValueAsString(From), *UEnum::GetValueAsString(To));
	}
	else if (To == EAgentPhase::Stranded)
	{
		// A STRANDED AEROPLANE MOVES ITS FLIGHT NOWHERE - FlightPhaseFromAgent's default. It is not
		// at its stand, so no turnaround starts; the flight stays in its taxi until the player
		// retires the aeroplane. Said, because it is the one agent phase change here with no
		// flight phase change to show for it.
		UE_LOG(LogAirportOps, Warning, TEXT("Flight %d (%s): its aircraft (agent %d) is stranded in %s; retire it to free the flight"),
			Flight->Id, *Flight->Callsign, AgentId, *UEnum::GetValueAsString(Flight->Phase));
	}

	if (To == EAgentPhase::Parked)
	{
		// WHICH stand it actually got, which need not be the one held - see UFlight::Stand.
		// The agent's own GoalNode is the authority, exactly as UJobBoard reads it - and, as there,
		// only while the agent is STILL parked: the bus delivers this a step late, and an aircraft
		// redirected since has a GoalNode that is its next stand, not this one (UJobBoard::OnAgentPhase).
		if (bStillParked && GoalStand.IsSet())
		{
			Flight->Stand = GoalStand;
		}
	}

	if (To == EAgentPhase::Gone)
	{
		ByAgent.Remove(Flight->AgentId);
		Flight->AgentId = INDEX_NONE;
	}

	if (Flight->Phase == EFlightPhase::Departing && Flight->AirborneAt <= 0.0)
	{
		// THE OTHER END OF THE TURNAROUND CONTRACT - see UFlight::AirborneAt. Taken once.
		Flight->AirborneAt = Clock.Now();
		if (Bus != nullptr)
		{
			// LATENESS AGAINST THE CONTRACT the row showed at the offer: AirborneBy is AcceptedAt +
			// ContractSeconds. Published once, because AirborneAt is taken once.
			Bus->Publish(FFlightAirborneEvent{ Flight->Id, Flight->AirlineId, Flight->AirborneAt - Flight->AirborneBy() });
		}
	}

	if (Flight->Phase == EFlightPhase::Departed)
	{
		// TERMINAL: retired out of the live list on the same phase change that made it so,
		// rather than waiting for RollUp's daily beat - see MoveToHistory and issue #188.
		MoveToHistory(*Flight, Clock.Now());
	}

	// THE MONEY, at two phases and those two specifically.
	//
	// Landing is where an aeroplane becomes the airport's business. TaxiOut is the one phase
	// EVERY departure reaches - Manoeuvring is NOT, because an aeroplane parked within
	// StraightOutDegrees of its exit heading simply drives out and never enters it (commit
	// 021cc2e). Charging parking at a phase some flights never enter would be a fee that went
	// silently uncollected on exactly the layouts the player built best.
	const double Now = Clock.Now();
	if (Flight->Phase == EFlightPhase::Landing)
	{
		PostLandingFee(Now, *Flight);
	}
	else if (Flight->Phase == EFlightPhase::Turnaround && Flight->ParkedAt <= 0.0)
	{
		// The start of the parking clock, taken once - Turnaround is reached again by anything
		// that re-enters it, and the second visit must not restart the meter in the player's
		// favour.
		Flight->ParkedAt = Now;
	}
	else if (Flight->Phase == EFlightPhase::TaxiOut)
	{
		PostParkingFee(Now, *Flight);
	}

	++RevisionCount;
}

void UFlightBoard::OnGraphRebuilt(UGroundTraffic& Traffic, const URoadNetwork& Network, const TArray<UFlight*>& HoldLast)
{
	if (Allocator == nullptr)
	{
		return;
	}

	TArray<UFlight*> Holding;
	TArray<UFlight*> Last;
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		// INBOUND TOO: a holding flight keeps its stand, and a graph rebuild takes every claim.
		if (Each != nullptr && (Each->Phase == EFlightPhase::Accepted || Each->Phase == EFlightPhase::Inbound))
		{
			// THE GENUINE HOLDS FIRST (review I1): a flight a load re-queued (#404) names the stand it was ACCEPTED
			// onto, which it gave up at its dispatch - and which another flight may have been accepted onto since.
			// That flight's promise stands; the re-queued one is re-held after it, and refused if it was taken.
			(HoldLast.Contains(Each.Get()) ? Last : Holding).Add(Each);
		}
	}
	Holding.Append(Last);
	Allocator->Reapply(Traffic, Network, Holding);

	// A HOLDING FLIGHT WITHOUT A STAND TAKES ONE NOW (review I1) - one Reapply just refused, or one whose stand is
	// gone (Reapply keeps a dead Stand for the HeldStandLost alert; a Reserve overwrites it) - rather than on the
	// first frame's TickQueue, whose same pass this is: a load must not hand the next
	// accept a stand the queue was owed. After every Reapply, so no genuine hold is beaten to its own stand.
	// ENFORCED BY: AirportOps.Model.FlightSave.RequeueDoesNotTakeAnAcceptedStand, AirportOps.Model.FlightSave.RequeueOffADeadStandReserves
	for (UFlight* Each : Holding)
	{
		if (Each->Phase == EFlightPhase::Inbound && (!Each->Stand.IsSet() || UStandAllocator::HeldStandIsGone(*Each, Network))
			&& Allocator->Reserve(Traffic, Network, *Each))
		{
			UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) holding: stand %d held again"),
				Each->Id, *Each->Callsign, Each->Stand.Index);
		}
	}
}

void UFlightBoard::RearmSchedules(UGroundTraffic& Traffic, const URoadNetwork& Network,
	USimClock& Clock)
{
	// CANCELLED, not just forgotten (PR #137 review): a handle left in Clock's own queue
	// after this map drops it is an orphaned entry that outlives every reference to it here -
	// harmless against a genuinely fresh post-load Clock (its queue starts empty), but this
	// function has no way to know that is the only time it is ever called, and an entry that
	// fires later still runs its captured lambda against whatever Id it named.
	for (const TPair<int32, int32>& Handle : ArrivalHandles) { Clock.Cancel(Handle.Value); }
	ArrivalHandles.Reset();
	Verdicts.Reset();
	Clearances.Reset();

	// REBUILT HERE TOO, NOT ONLY IN OnAfterRestore: this is the one call every load path is
	// documented to make (see this function's own header), while OnAfterRestore only runs
	// under OpsSave::Restore. A board deserialised directly - OpsSave::SerializeObject/
	// DeserializeObject, which FFlightSurvivesASaveTest uses on purpose to isolate the clock's
	// own re-arm from the rest of Restore - would otherwise re-arm a Schedule
	// callback whose eventual FindById(Id) found nothing, because ById was still empty.
	// (Serialize's load rebuilds them first since #425; this stays the one rebuild every load path reaches.)
	RebuildIndices();

	// SNAPSHOT, NOT A LIVE ITERATION: DispatchNow below mutates the board, and a range-based
	// for over Flights must not see that happen under it.
	const TArray<TObjectPtr<UFlight>> Loaded = Flights;
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
		if (Each->Phase != EFlightPhase::Accepted)
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
			Enqueue(*Each, Each->ArrivesAt);
			continue;
		}

		Schedule(Traffic, Clock, *Each);
	}
}

TArray<UFlight*> UFlightBoard::Offers() const
{
	TArray<UFlight*> Out;
	// RESERVED, NOT LEFT TO GROW BY DOUBLING: Flights is small now that terminal flights are
	// moved out at once (see MoveToHistory), but a poller calling this every frame still pays
	// for however many reallocations an unreserved TArray::Add needs to reach it - issue #188
	// item 3. A full maintained Offered-only list was rejected: see PendingOfferCount's own
	// comment for the counter that replaced Offers().Num(), and CLAUDE.md's "lists that must
	// agree are one list" for why a second array mirroring Flights was not the answer here too.
	Out.Reserve(Flights.Num());
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each != nullptr && Each->Phase == EFlightPhase::Offered)
		{
			Out.Add(Each);
		}
	}
	// STABLE, so two offers with the same time left keep the order they arrived in.
	Algo::StableSortBy(Out, [](const UFlight* F) { return F->OfferSecondsLeft; });
	return Out;
}

TArray<UFlight*> UFlightBoard::Live() const
{
	TArray<UFlight*> Out;
	Out.Reserve(Flights.Num());   // See Offers()'s own comment.
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each == nullptr)
		{
			continue;
		}
		const bool bLive = Each->Phase >= EFlightPhase::Accepted
			&& Each->Phase <= EFlightPhase::Departing;
		if (bLive)
		{
			Out.Add(Each);
		}
	}
	return Out;
}

void UFlightBoard::MoveToHistory(UFlight& Flight, double Now)
{
	Flight.TerminatedAt = Now;

	// FOUND BY RAW-POINTER IDENTITY, hand-rolled rather than IndexOfByKey(&Flight): the array
	// holds TObjectPtr<UFlight>, and comparing that to a bare UFlight* here rather than relying
	// on TObjectPtr's implicit pointer conversion keeps this unambiguous at every UE_5.8 point
	// release regardless of what else that conversion operator might overload against.
	int32 Index = INDEX_NONE;
	for (int32 I = 0; I < Flights.Num(); ++I)
	{
		if (Flights[I].Get() == &Flight)
		{
			Index = I;
			break;
		}
	}
	if (Index != INDEX_NONE)
	{
		// STABLE REMOVAL, NOT A SWAP: Offers() promises "still awaiting an answer, in the
		// order they arrived" for whichever offers remain, and RemoveAtSwap would reorder the
		// last element into the hole this leaves - a Declined offer moving out would silently
		// reshuffle every OTHER offer's position in the inbox.
		Flights.RemoveAt(Index);
	}
	History.Add(&Flight);
	Verdicts.Remove(Flight.Id);
	Clearances.Remove(Flight.Id);

	if (Flight.AgentId != INDEX_NONE)
	{
		// DEFENSIVE: every call site that can reach here already clears AgentId first
		// (OnAgentPhase's Gone branch) or never set it at all (Decline, the expiry callback,
		// RearmSchedules' load-time lapse never dispatch an offer) - but a ByAgent entry that
		// outlived the flight it named would be found by some LATER agent's phase change and
		// move a flight that has already departed.
		ByAgent.Remove(Flight.AgentId);
	}
}

void UFlightBoard::RollUp(double Now)
{
	const double Cutoff = Now - (MaxDays * USimClock::SecondsPerDay);

	int32 Count = 0;
	History.RemoveAll([this, Cutoff, &Count](const TObjectPtr<UFlight>& Each)
	{
		if (Each == nullptr || Each->TerminatedAt >= Cutoff)
		{
			return false;
		}
		// THE ONE PLACE ById LOSES AN ENTRY - see its own comment. Once this runs, nothing
		// (a clock callback, a test, a future "recent departures" view) can find this flight
		// again by any means, which is the point: it is forgotten, not archived.
		ById.Remove(Each->Id);
		++Count;
		return true;
	});

	if (Count == 0)
	{
		return;
	}

	++RevisionCount;
	UE_LOG(LogAirportOps, Log,
		TEXT("Flight history rolled up: %d flight(s) older than %d day(s) forgotten"),
		Count, MaxDays);
}

UFlight* UFlightBoard::FindByAgent(int32 AgentId) const
{
	if (AgentId == INDEX_NONE)
	{
		return nullptr;
	}
	return ByAgent.FindRef(AgentId);
}

UFlight* UFlightBoard::FindById(int32 Id) const
{
	return ById.FindRef(Id);
}

UFlight* UFlightBoard::FindByAgentLinearForTest(int32 AgentId) const
{
	// THE PRE-#188 IMPLEMENTATION, verbatim in shape: an id-bearing flight is always in
	// Flights (see MoveToHistory, which is only ever reached after AgentId is cleared), so
	// scanning Flights alone already agrees with ByAgent for every case that can occur.
	if (AgentId == INDEX_NONE)
	{
		return nullptr;
	}
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each != nullptr && Each->AgentId == AgentId)
		{
			return Each;
		}
	}
	return nullptr;
}

UFlight* UFlightBoard::FindByIdLinearForTest(int32 Id) const
{
	// BOTH ARRAYS: unlike AgentId, an Id is never cleared, and ById answers for a flight in
	// EITHER array for as long as RollUp has not forgotten it - see ById's own comment.
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each != nullptr && Each->Id == Id)
		{
			return Each;
		}
	}
	for (const TObjectPtr<UFlight>& Each : History)
	{
		if (Each != nullptr && Each->Id == Id)
		{
			return Each;
		}
	}
	return nullptr;
}
