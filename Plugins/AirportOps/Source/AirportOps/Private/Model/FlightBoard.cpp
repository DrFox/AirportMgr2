#include "Model/FlightBoard.h"

#include "AirportOpsLog.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/OpsEventBus.h"
#include "Model/AirsideCapability.h"
#include "Model/ArrivalSequencer.h"
#include "Model/Flight.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
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
	// THE BOARD'S OWN FIELDS FIRST, by the tagged pass (MaxDays, RunwayPreference, NextFlightId). Flights and History
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

	// RETIRED, NOT MERELY DROPPED - see RetireEveryFlight. OnBeforeRestore has already done it under OpsSave::Restore;
	// here as well because a board deserialised directly (OpsSave::DeserializeObject, as FlightSaveTest does) gets no
	// OnBeforeRestore, and a second retire of an empty board costs nothing.
	RetireEveryFlight();

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

bool UFlightBoard::DefaultRunwayPreference(const URoadNetwork& Network, FVector2D& OutFocus)
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
	if (Offer->GetPhase() == EFlightPhase::Offered)
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
		if (Each == nullptr || Each->GetPhase() != EFlightPhase::Offered)
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
		// THE REASON BEFORE THE TRANSITION: the Expired row publishes it (FOfferExpiredEvent carries LapseReason), and the
		// row owns the count, the publish, the move to History and the revision that used to follow here by hand.
		Each->LapseReason = Each->bWasEverAcceptable ? ELapseReason::Ignored : ELapseReason::NeverAcceptable;
		UE_LOG(LogAirportOps, Log, TEXT("Offer %d (%s) lapsed unanswered (%s)"), Each->Id, *Each->Callsign,
			Each->LapseReason == ELapseReason::Ignored ? TEXT("ignored") : TEXT("never acceptable"));
		TransitionTo(*Each, EFlightPhase::Expired, FTransitionCause::Played(Clock.Now()));
	}
}

const FOfferVerdict& UFlightBoard::VerdictFor(const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const UFlight& Flight) const
{
	FOfferVerdict& Verdict = Verdicts.FindOrAdd(Flight.Id);
	const uint32 BoardNow = Revision();
	const uint32 GuidelineNow = Network.GetGuidelineRevision();
	const uint32 OccupancyNow = Traffic.OccupancyRevision();
	// THE FLEET'S COMPOSITION, not its transitions (#443): CouldServe reads which vehicles exist, of what kind and where,
	// and never a vehicle's state, so a truck arriving or finishing a refill must not re-plan the offer.
	const uint32 FleetNow = Fuel != nullptr ? Fuel->GetFleetCompositionRevision() : 0;
	// BOTH DECIDED BEFORE EITHER IS REDONE: the guideline stamp is shared, and the first recompute would write it.
	const bool bPlanStale = !Verdict.bValid || Verdict.BoardAt != BoardNow || Verdict.GuidelineAt != GuidelineNow
		|| Verdict.OccupancyAt != OccupancyNow;
	// bFuelServable is CouldServe(Traffic, Network, Flight.Airframe): the airport's shape (the guideline graph - depots,
	// roads, stands, modules) and the fleet's composition, which includes which vehicles are stranded (#443: the traffic model
	// is asked who is, and the composition counter moves when one strands or moves again). Not the board (the airframe is the
	// flight's own, fixed) and not occupancy (it judges no traffic's whereabouts).
	// ENFORCED BY: AirportOps.Fuel.CouldServe.StrandingMovesTheCompositionAndTheVerdict
	const bool bFuelStale = !Verdict.bValid || Verdict.GuidelineAt != GuidelineNow || Verdict.FleetAt != FleetNow;
	if (bPlanStale)
	{
		// THE REAL PLAN, with the live occupancy. The greyed-out reason is the sentence the
		// arrival itself would print, because it is the same refusal - and since #431 the
		// stand the plan taxis to is kept with it, for TryAccept to hold.
		const FArrivalQuote Plan = PlanQuote(Traffic, Network, Flight.Airframe, Flight.RunwayPreference, 0);
		Verdict.Why = Plan.Why;
		Verdict.Sentence = Plan.Sentence;
		Verdict.Stand = Plan.Stand;
		Verdict.BoardAt = BoardNow;
		Verdict.OccupancyAt = OccupancyNow;
	}
	if (bFuelStale)
	{
		Verdict.bFuelServable = Fuel == nullptr || Fuel->CouldServe(Traffic, Network, Flight.Airframe);
		Verdict.FleetAt = FleetNow;
	}
	Verdict.GuidelineAt = GuidelineNow;
	Verdict.bValid = true;
	return Verdict;
}

int32 UFlightBoard::TakeNextId()
{
	return NextFlightId++;
}

FArrivalQuote UFlightBoard::PlanQuote(const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const FAirframe& Airframe, const FVector2D& Focus, int32 ExcludingHolder) const
{
	// #169: counted before the search runs, not after - GetWhyNotAcceptableCallsForTest exists
	// to measure exactly how often this expensive call is reached, whatever it returns.
	++WhyNotAcceptableCallsForTest;
	// QUEUE, not Refuse: a busy runway is something an accepted flight waits for (spec
	// 2026-09-28-arrival-queue section 1), so the row greys out only on what waiting cannot fix
	// - a stand above all - and never on RunwayOccupied.
	const FArrivalPlan Plan = ArrivalPlanner::Plan(Network, Focus, Airframe, &Traffic.GetOccupancy(),
		ERunwayBusy::Queue, ExcludingHolder);
	FArrivalQuote Quote;
	Quote.Why = Plan.Why;
	// THE PLAN'S OWN SENTENCE, figures and admission included (#456 review) - the reason-only wording cannot say that
	// an arrivals-only field refuses because nothing can take the departure.
	Quote.Sentence = Plan.IsValid() ? FString() : ArrivalPlanner::DescribeRefusal(Plan);
	if (Plan.IsValid())
	{
		// THE STAND THE PLAN TAXIS TO, as an entity - what an accept holds (#431). A plan whose route ends somewhere that
		// is not a stand's pose cannot be accepted onto one, and says so rather than holding something else.
		const int32 StandIndex = Network.FindEntityIndexByPoseNode(Plan.StandNode());
		Quote.Stand = StandIndex != INDEX_NONE ? Network.EntityIdAt(StandIndex) : FEntityInstanceId();
		if (!Quote.Stand.IsSet())
		{
			Quote.Why = EArrivalRefusal::NoRouteToStand;
			Quote.Sentence = ArrivalPlanner::DescribeRefusal(Quote.Why, Airframe.Wingspan);
		}
	}
	return Quote;
}

FArrivalQuote UFlightBoard::Gated(FArrivalQuote Quote) const
{
	// AFTER THE PLAN: a runway-less field is the plan's NoRunway, worded as the plan words it. A plan's yes at an airport
	// the player has closed is the gate's - ruling I1, a closed airport admits no arrivals, the debug one included.
	// NotAdmitted BECAUSE EArrivalRefusal HAS NO CLOSURE VALUE (it is the planner's, in Airside, which knows no airport):
	// the nearest reason, with the real one in Sentence. A reader of Why alone must not word it - DescribeRefusal(
	// NotAdmitted) says "not admitted to that runway" - which is why every listener carries the Sentence.
	if (Quote.IsAccepted() && AdmitsArrivals && !AdmitsArrivals())
	{
		Quote.Why = EArrivalRefusal::NotAdmitted;
		Quote.Sentence = TEXT("Arrival refused: the airport is closed - open it to take arrivals.");
		Quote.Stand = FEntityInstanceId();
	}
	return Quote;
}

FArrivalQuote UFlightBoard::QuoteFor(const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const UFlight& Flight) const
{
	const FOfferVerdict& Verdict = VerdictFor(Traffic, Network, Flight);
	FArrivalQuote Quote;
	Quote.Why = Verdict.Why;
	Quote.Sentence = Verdict.Sentence;
	Quote.Stand = Verdict.Stand;
	return Gated(MoveTemp(Quote));
}

FArrivalQuote UFlightBoard::QuoteArrival(const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const FAirframe& Airframe, const FVector2D& Focus) const
{
	return Gated(PlanQuote(Traffic, Network, Airframe, Focus, 0));
}

bool UFlightBoard::Accept(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock,
	UFlight& Flight)
{
	return TryAccept(Traffic, Network, Clock, Flight).IsAccepted();
}

FArrivalQuote UFlightBoard::TryAccept(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock,
	UFlight& Flight)
{
	// REFUSED BEFORE THE PLAN, each worded and logged - a caller's bug or a fixture's, never a player's (see the header).
	// NotAdmitted for Gated's reason: EArrivalRefusal has no value for these, so the Sentence carries the real one.
	const auto Refuse = [&Flight](const TCHAR* Why)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d not accepted: %s"), Flight.Id, Why);
		FArrivalQuote Out;
		Out.Why = EArrivalRefusal::NotAdmitted;
		Out.Sentence = FString::Printf(TEXT("Arrival refused: %s."), Why);
		return Out;
	};
	if (Flight.GetPhase() != EFlightPhase::Offered)
	{
		return Refuse(TEXT("it is not an open offer"));
	}
	if (Allocator == nullptr)
	{
		return Refuse(TEXT("the board has no stand allocator to hold a stand with"));
	}
	if (Flight.Airframe.Wingspan <= 0.0)
	{
		return Refuse(TEXT("it has no airframe"));
	}

	// THE ONE EVALUATOR (#431): the cached plan verdict, then the airport's gate - the answer the inbox row shows and the
	// Land panel quotes. NOT OPEN, NOTHING ACCEPTED (ruling I1): the gate refuses before the stand is held.
	FArrivalQuote Quote = QuoteFor(Traffic, Network, Flight);
	if (!Quote.IsAccepted())
	{
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d not accepted: %s"), Flight.Id, *Quote.Sentence);
		return Quote;
	}

	// THE STAND THE PLAN TAXIS TO, and no other: the plan chose it from the stands it can REACH (ChooseStand), where
	// Reserve's smallest fit took any admitted stand, reachable or not, and could leave this flight waiting NoFreeStand
	// for the stand a later accept took. The verdict is dated by occupancy and by this board, so the stand it names is
	// free now; a refusal here is a same-call race, and says so.
	if (!Allocator->Hold(Traffic, Network, Flight, Quote.Stand))
	{
		Quote.Why = EArrivalRefusal::NoFreeStand;
		Quote.Sentence = ArrivalPlanner::DescribeRefusal(Quote.Why, Flight.Airframe.Wingspan);
		Quote.Stand = FEntityInstanceId();
		UE_LOG(LogAirportOps, Warning, TEXT("Flight %d not accepted: the stand its plan chose could not be held"), Flight.Id);
		return Quote;
	}

	// THE LEAD TIME RUNS FROM THE ACCEPT, not from the offer: a player who took most of the
	// window to decide still gets the whole lead, and the contract is measured from here. BEFORE the transition: the
	// Accepted row arms the arrival at ArrivesAt.
	Flight.AcceptedAt = Clock.Now();
	Flight.ArrivesAt = Flight.AcceptedAt + Flight.LeadTimeSeconds;

	// THE ACCEPTED ROW OF TransitionTo: the pending-offer count, the arrival on the clock (Schedule), the revision, and
	// FFlightAccepted's publish - PUBLISHED IN THE BOARD, not by the inbox that called it: Accept is a player command reached
	// straight from the game module, and the key-7 path (AcceptImmediate) comes through here too - one
	// publisher for every accept. After the hold, so a refusal above publishes nothing.
	// ENFORCED BY: AirportOps.Model.FlightBoard.AcceptPublishesOfferAccepted
	TransitionTo(Flight, EFlightPhase::Accepted, FTransitionCause::Played(Clock.Now()).WithWorld(&Clock, &Traffic));

	UE_LOG(LogAirportOps, Log, TEXT("Flight %d accepted: stand %d held, landing at %.0f"),
		Flight.Id, Flight.Stand.Index, Flight.ArrivesAt);
	return Quote;
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
		if (WeakTraffic.Get() != nullptr && Due != nullptr && Due->GetPhase() == EFlightPhase::Accepted)
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
	// THE INBOUND ROW OF TransitionTo: HoldingSince = Since, the arrival handle let go (it has fired, or a load's re-arm reset
	// the map), an aeroplane still flying the flight unhooked (a load's re-queue of a Landing or TaxiIn flight), the
	// revision, and FFlightInboundEvent. THE QUEUE PASS'S WAKE-UP (ops batch 3 §5): the queue is no longer ticked every frame, so a
	// flight joining it says so. Here, the one site every Inbound flight passes through - while the game runs, and since #426 a load's
	// re-queue too (DemoteRestoredMidFlight), which the load's MarkAllDirty used to be the only thing to wake.
	// ENFORCED BY: AirportOps.Model.FlightBoard.EnqueuePublishesInbound
	TransitionTo(Flight, EFlightPhase::Inbound, FTransitionCause::Played(Since));
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) holding: #%d in queue"),
		Flight.Id, *Flight.Callsign, Queue().Find(&Flight) + 1);
}

TArray<UFlight*> UFlightBoard::Queue() const
{
	TArray<UFlight*> Out;
	for (const TObjectPtr<UFlight>& Each : Flights)
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
	const EArrivalRefusal Why = ArrivalPlanner::Plan(Network, Flight.RunwayPreference, Flight.Airframe,
		&Traffic.GetOccupancy(), ERunwayBusy::Queue, Flight.HolderId()).Why;
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
	Clearance.bValid = true;
	return Why;
}

EArrivalRefusal UFlightBoard::UnlandableWhy(const UFlight& Flight, const URoadNetwork& Network) const
{
	// HOLDING OR ACCEPTED: the clearance is computed for the queue (TickQueue), and an Accepted flight's judgement is JudgeAccepted's
	// (#445) - also the queue pass's, never this alert pass's: a plan run here would be a route search per flight per alert recompute.
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

void UFlightBoard::JudgeAccepted(const URoadNetwork& Network)
{
	const uint32 GuidelineNow = Network.GetGuidelineRevision();
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each == nullptr || Each->GetPhase() != EFlightPhase::Accepted)
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
			UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) accepted: it can no longer land - %s"),
				Each->Id, *Each->Callsign, *ArrivalPlanner::DescribeRefusal(Why, Each->Airframe.Wingspan));
		}
		Clearance.Why = Why;
		Clearance.GuidelineAt = GuidelineNow;
		// NO OCCUPANCY WAS ASKED, so no occupancy revision dates it: ClearanceFor's own test then plans afresh with one when the flight
		// joins the queue, and does not trust this verdict for the runway's busy-ness it never asked.
		Clearance.OccupancyAt = TNumericLimits<uint32>::Max();
		Clearance.bValid = true;
	}
}

FQueueTick UFlightBoard::TickQueue(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
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
	// THE ACCEPTED, JUDGED AGAINST TODAY'S GRAPH (#445), before the closed and paused exits below: it reads no clock and no runway, and
	// the FlightCannotLand alert that follows this pass in the round reads what it leaves. A closed airport cancels its accepted
	// flights by its own event, so a verdict made for one is not read for long.
	// ENFORCED BY: AirportOps.Present.Airport.CloseCancelsThroughTheBus
	JudgeAccepted(Network);
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
		return !ArrivalPlanner::IsRunwayBusy(Network, F.RunwayPreference, &Traffic.GetOccupancy())
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
	// twice in one drain, so the rule is kept across its rounds - bClearedThisQueueFrame, checked at the top.
	// ENFORCED BY: AirportOps.Present.ArrivalQueue.SecondRunwayNextFrame
	const double Held = Clock.Now() - Next->HoldingSince;
	if (DispatchNow(Traffic, Network, *Next, Clock.Now()))
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

bool UFlightBoard::DispatchNow(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight, double Now)
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

	if (!Dispatcher(Flight.RunwayPreference, Flight.Airframe))
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

	// THE LANDING ROW OF TransitionTo hooks the aeroplane (AgentId and ByAgent) and bumps the revision. The stand's RELEASE
	// stays above, before the dispatch - it is not the row's, because the planner must not see the hold - so this row releases
	// nothing.
	TransitionTo(Flight, EFlightPhase::Landing, FTransitionCause::Played(Now).WithAgent(Traffic.GetNewestAgentId()));
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d dispatched as agent %d"), Flight.Id, Flight.AgentId);
	return true;
}

void UFlightBoard::Decline(USimClock& Clock, UFlight& Flight)
{
	if (Flight.GetPhase() != EFlightPhase::Offered)
	{
		return;
	}
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d declined"), Flight.Id);
	// THE DECLINED ROW: the pending-offer count, FOfferDeclinedEvent, History, the revision.
	TransitionTo(Flight, EFlightPhase::Declined, FTransitionCause::Played(Clock.Now()));
}

bool UFlightBoard::CancelByAgent(int32 AgentId, double Now)
{
	UFlight* Flight = FindByAgent(AgentId);
	if (Flight == nullptr)
	{
		return false;
	}
	const EFlightPhase WasPhase = Flight->GetPhase();
	// THE CANCELLED ROW, from the ground: UNHOOKED BEFORE MoveToHistory, as OnAgentPhase's Gone branch does - see MoveToHistory's
	// DEFENSIVE comment for why a ByAgent entry must not outlive its flight - and THE SECOND PUBLISHER of FlightCancelled (spec
	// 2026-09-29-ops-batch3 §3): the roster hears it and charges nothing for Unstuck - heard, so the log and any later reaction
	// see every cancellation the same way.
	// ENFORCED BY: AirportOps.Model.FlightBoard.CancelByAgentPublishesUnstuck
	TransitionTo(*Flight, EFlightPhase::Cancelled, FTransitionCause::Played(Now).Cancelling(ECancelReason::Unstuck));
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s): %s -> Cancelled (agent %d despawned)"),
		Flight->Id, *Flight->Callsign, *UEnum::GetValueAsString(WasPhase), AgentId);
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
		if (Flight.GetPhase() == EFlightPhase::Offered)
		{
			// THE WITHDRAWN ROW: the pending-offer count and History, nothing published (no OfferExpired, so no Ignored penalty).
			TransitionTo(Flight, EFlightPhase::Withdrawn, FTransitionCause::Played(Clock.Now()));
			++Withdrawn;
			continue;
		}
		if (!Flight.IsUnarrived())
		{
			// LANDING AND LATER FINISH: committed to the runway, or on the ground being drained.
			continue;
		}
		// THE CANCELLED ROW, from Accepted or Inbound: THE ARRIVAL DISARMED, not merely forgotten - its Clock.At finds the flight
		// by id even in History (ById keeps it until RollUp) and would put a cancelled flight back in the queue (Inbound has none
		// left) - the stand hold released, FFlightCancelledEvent published with Reason, into History. The row does all four;
		// this door used to, and the other two cancelling doors did different subsets.
		// ENFORCED BY: AirportOps.Model.FlightBoard.CancelUnarrivedCancelsAndWithdraws ("still Cancelled after its ETA")
		const EFlightPhase WasPhase = Flight.GetPhase();
		TransitionTo(Flight, EFlightPhase::Cancelled, FTransitionCause::Played(Clock.Now()).WithWorld(&Clock, &Traffic).Cancelling(Reason));
		++Cancelled;
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s): %s -> Cancelled (%s)"), Flight.Id, *Flight.Callsign,
			*UEnum::GetValueAsString(WasPhase), *UEnum::GetValueAsString(Reason));
	}
	if (Cancelled + Withdrawn > 0)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Airport not open (%s): %d flight(s) cancelled, %d offer(s) withdrawn"),
			*UEnum::GetValueAsString(Reason), Cancelled, Withdrawn);
	}
	return Cancelled;
}

bool UFlightBoard::CancelByPlayer(UGroundTraffic& Traffic, USimClock& Clock, int32 FlightId)
{
	// AN UNKNOWN ID, OR ONE ALREADY IN HISTORY, is nothing to cancel - FindById answers for History too, so the phase is what
	// says. Landing and later is committed to the runway: a despawn of its aeroplane is CancelByAgent's, through the card.
	UFlight* Flight = FindById(FlightId);
	if (Flight == nullptr || !Flight->IsUnarrived())
	{
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d not cancelled by the player: %s"), FlightId,
			Flight == nullptr ? TEXT("no such flight") : TEXT("it is not still to arrive"));
		return false;
	}
	const EFlightPhase WasPhase = Flight->GetPhase();
	// THE CANCELLED ROW, from Accepted or Inbound - the closure's own - with the reason the roster charges ClosureCancelPenalty for.
	TransitionTo(*Flight, EFlightPhase::Cancelled,
		FTransitionCause::Played(Clock.Now()).WithWorld(&Clock, &Traffic).Cancelling(ECancelReason::PlayerCancelled));
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s): %s -> Cancelled (by the player)"),
		Flight->Id, *Flight->Callsign, *UEnum::GetValueAsString(WasPhase));
	return true;
}

int32 UFlightBoard::UnarrivedCount() const
{
	// A SCAN OF Flights, not a maintained counter: asked by the bar's confirm when the popup opens, and Flights
	// holds only the live flights (~10 on 2026-09-29, terminal ones move to History at once).
	int32 Count = 0;
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		Count += Each != nullptr && Each->IsUnarrived();
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
		Count += Each != nullptr && Each->IsOnGround();
	}
	return Count;
}

EArrivalRefusal UFlightBoard::WhyNotAcceptable(const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const UFlight& Flight) const
{
	// THE PLAN HALF OF THE ONE QUOTE - PlanQuote, where the count and the Queue rule now live, so this and the cached
	// verdict cannot plan two different ways.
	return PlanQuote(Traffic, Network, Flight.Airframe, Flight.RunwayPreference, 0).Why;
}

EArrivalRefusal UFlightBoard::AcceptImmediate(UGroundTraffic& Traffic, const URoadNetwork& Network,
	USimClock& Clock, const FAirframe& Airframe, const FVector2D& Focus, FText Airline, FString* OutSentence)
{
	// A FIELD THAT TAKES NO ARRIVALS HOLDS NO OFFERS (ruling I1, 2026-09-30): the closure withdrew them and the generator
	// makes none, so a refused debug flight left in the inbox would be the only offer on a closed field. Not made at all,
	// then - and the refusal is still THE ONE QUOTE (QuoteArrival: the plan, then the gate), asked before the flight
	// exists rather than re-derived after, so a runway-less field says NoRunway and a closed one says it is closed.
	// ENFORCED BY: AirportOps.Present.Airport.LandRefusedWhileClosed
	if (AdmitsArrivals && !AdmitsArrivals())
	{
		const FArrivalQuote Refused = QuoteArrival(Traffic, Network, Airframe, Focus);
		UE_LOG(LogAirportOps, Log, TEXT("AcceptImmediate: no flight made - %s"), *Refused.Sentence);
		if (OutSentence != nullptr)
		{
			*OutSentence = Refused.Sentence;
		}
		return Refused.Why;
	}

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
	Flight->RunwayPreference = Focus;

	AddOffer(Clock, Flight);

	// THE REFUSAL TryAccept HIT, returned by the gate that refused - not re-derived by asking the plan again, which is
	// how a closure, a null allocator or a span-0 airframe used to come back as None, "success" (#431). Says WHICH
	// refusal, the same sentence the inbox would show for it. The flight is left in the inbox rather than removed:
	// an offer nobody could accept yet is exactly what the board already does for one the
	// generator makes, and a player watching the inbox sees the same row either way.
	const FArrivalQuote Quote = TryAccept(Traffic, Network, Clock, *Flight);
	if (OutSentence != nullptr)
	{
		*OutSentence = Quote.Sentence;
	}
	return Quote.Why;
}

int32 UFlightBoard::RetireEveryFlight()
{
	// RETIRED, NOT MERELY DROPPED: a viewmodel row, the inspector's cached lookup, a test's local - anything still
	// holding a flight this load replaces - must read null from here on, not the pre-load state, which is exactly the
	// wrong answer #425 was (a same-session load "restored" each flight as it was NOW). Garbage makes every weak
	// pointer to it null at once; the object itself lives until the next collection, so a raw pointer is not dangling.
	int32 Retired = 0;
	for (TArray<TObjectPtr<UFlight>>* List : { &Flights, &History })
	{
		for (const TObjectPtr<UFlight>& Each : *List)
		{
			if (Each != nullptr)
			{
				Each->MarkAsGarbage();
				++Retired;
			}
		}
		List->Reset();
	}
	return Retired;
}

void UFlightBoard::OnBeforeRestore()
{
	// WHATEVER THE SNAPSHOT HOLDS (#426 (b)) - see the header. The indices named the retired flights, and a restore
	// is a change: every viewmodel keyed on Revision re-reads, which is what empties an inbox the snapshot has no
	// flights for. Serialize repeats all three when there IS a blob, on the flights that blob brings.
	const int32 Retired = RetireEveryFlight();
	RebuildIndices();
	++RevisionCount;
	if (Retired > 0)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Restore: %d flight(s) of the replaced session retired"), Retired);
	}
}

void UFlightBoard::RestoreAfterLoad(UGroundTraffic* Traffic, const URoadNetwork& Network, USimClock& Clock,
	bool bAirportAdmits)
{
	// THE ORDER IS THE HEADER'S, step for step - see RestoreAfterLoad there for why each is where it is.
	// 0. THE REPLACED SESSION'S ARRIVALS, CANCELLED ON THE CLOCK AND FORGOTTEN (#442 review): ArrivalHandles is not saved, so it still
	// holds the handles the session being replaced armed, keyed by ids a restored flight may carry. Before step 1, so step 2's cancel
	// finds nothing armed - and a load with no traffic model, which returns before step 4's re-arm, no longer leaves them on the clock.
	DisarmEveryArrival(Clock);

	// 1. BESIDE ITS CAUSE (review ruling M5): the load's ClearAgents threw away every aeroplane, so a flight saved
	// landing or taxiing in goes round again and one on the ground retires as departed - dated by the LOADED clock.
	const TArray<UFlight*> Requeued = DemoteRestoredMidFlight(Clock.Now());

	// 2. THE ONE THING A LOAD DOES HAVE TO CANCEL (review ruling I2): the flights step 1 put back in the queue were on
	// the ground when the closure happened, so the closure left them - and at an airport that is not open they can
	// never land again. AND EVERY OTHER FLIGHT STILL TO ARRIVE (whole-stack review I1): an Accepted flight saved at the
	// closed airport, which the closure's own cancel never met. Before step 4, so none is armed.
	int32 Cancelled = 0;
	if (!bAirportAdmits)
	{
		// WITH THE TRAFFIC AND THE CLOCK THE LOAD HAS (#442), so the Cancelled row releases and disarms as it does in play: in this
		// position nothing is held or armed yet (step 0 cancelled the old session's arrivals, nothing re-arms until step 4) and
		// they find nothing to do, but the cancel no longer depends on it.
		Cancelled = CancelUnarrivedAtLoad(Clock.Now(), Traffic, &Clock);
	}

	// 3 AND 4, IN THIS ORDER, and both are needed. The network's rebuild regenerated the guideline graph, which takes
	// every node claim with it (FTrafficOccupancy::ReleaseGuidelineClaims), so the restored flights' stand holds have
	// to be re-made against the new nodes BEFORE anything can allocate - the genuine holds first, the re-queued last.
	// Then the arrivals go back on the clock, whose queue was never saved. BOTH NEED THE TRAFFIC MODEL - the holds are
	// claims on it and the arrivals dispatch into it - so with none they are skipped, and said so.
	if (Traffic == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Load: no traffic model - %d flight(s) demoted or cancelled, but no stand re-held and no arrival re-armed"),
			Requeued.Num() + Cancelled);
		return;
	}
	OnGraphRebuilt(*Traffic, Network, Requeued);
	RearmSchedules(*Traffic, Network, Clock);

	UE_LOG(LogAirportOps, Log, TEXT("Load: flights restored - %d re-queued, %d cancelled at a closed airport, %d live"),
		Requeued.Num(), Cancelled, Flights.Num());
}

void UFlightBoard::OnAfterRestore(int32 /*SnapshotVersion*/)
{
	// NO MIGRATION LEFT (owner ruling 2026-09-30): since #452 (snapshot v6) a pre-v6 blob restores no flights - the
	// board's Serialize warns "no flights by value" - so the v2 focus migration and #188's sweep of terminal
	// flights out of Flights could reach nothing from any real save, and went. A v6 save's Flights holds live flights
	// only: every terminal transition goes through TransitionTo, whose terminal rows call MoveToHistory - eight doors
	// onto it on 2026-09-30 (TickOffers' lapse, Decline, CancelByAgent, CancelUnarrived's withdrawal and cancel,
	// DemoteRestoredMidFlight, CancelUnarrivedAtLoad, OnAgentPhase's Departed) and one more since (#442: CancelByPlayer),
	// which is now ONE place, not nine, Decline's pinned, the rest by the table test.
	// ENFORCED BY: AirportOps.Model.FlightSave.PreV6BlobRestoresNoFlights (the load half), AirportOps.Model.FlightBoard.HistoryStaysBoundedAcrossManySimulatedDays (Decline), AirportOps.Model.FlightBoard.TransitionTable (every terminal row reaches History)
	RebuildIndices();
}

TArray<UFlight*> UFlightBoard::DemoteRestoredMidFlight(double Now)
{
	// #404: AGENTS ARE NOT SAVED - UOpsRuntime::LoadFromSlot clears every one before OpsSave::Restore - so a flight
	// saved past Inbound comes back naming an aeroplane that does not exist, and nothing would ever move it again.
	// Ruled (spec 2026-09-29-ops-batch3 §0, §4): the arrivals side GOES ROUND AGAIN, the ground side RETIRES AS
	// DEPARTED. STEP 1 OF RestoreAfterLoad, which LoadFromSlot calls after the ClearAgents that causes it (review ruling
	// M5) - not from OnAfterRestore, which is handed no clock and runs for every restore, agents cleared or not.
	// ENFORCED BY: AirportOps.Model.FlightSave.MidFlightGoesRoundOrRetires, AirportOps.Present.RuntimeLoad.MidFlightRequeuesOrRetires
	TArray<UFlight*> Requeued;
	// SNAPSHOT: MoveToHistory mutates Flights, and this loop is walking it.
	const TArray<TObjectPtr<UFlight>> Loaded = Flights;
	for (const TObjectPtr<UFlight>& Each : Loaded)
	{
		if (Each == nullptr)
		{
			continue;
		}
		if (FlightPhase::IsArriving(Each->GetPhase()))
		{
			// INBOUND, at the BACK of the queue (HoldingSince = now, not its old ETA: the flights that were already
			// holding when the game was saved were waiting first). Its Stand is still the one it was accepted onto;
			// OnGraphRebuilt holds it again AFTER every genuine holder, and finds it another if that one was taken.
			// bLandingFeePaid IS LEFT AS SAVED rather than forced true: it is saved and set with the ledger post
			// (PostLandingFee), so a flight that was charged stays charged and its second landing posts nothing - and
			// one saved before its landing was heard is charged once, when it lands.
			// ENFORCED BY: AirportOps.Model.FlightSave.UnchargedLandingIsChargedOnce
			UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) restored mid-%s: re-queued (its aircraft, agent %d, was not saved)"),
				Each->Id, *Each->Callsign, *FlightBoardText::PhaseName(Each->GetPhase()), Each->AgentId);
			// THROUGH Enqueue, THE LIVE DOOR INTO THE QUEUE (#426): Inbound, HoldingSince, the revision, and the
			// FlightInbound that wakes the arrival-queue pass - which this used to skip by setting the phase by hand,
			// leaving the load's MarkAllDirty as the only thing that dispatched it. THE INBOUND ROW ALSO LETS GO OF THE
			// AEROPLANE (ByAgent and AgentId): leaving the ground unhooks it, which this used to do by hand just above.
			Enqueue(*Each, Now);
			Requeued.Add(Each);
		}
		else if (FlightPhase::HasReachedStand(Each->GetPhase()))
		{
			// DEPARTED, UNSCORED: the save system is not the player's fault, so nothing the airline roster scores is
			// published (no FlightAirborne, no TurnaroundEnded) and no parking fee is posted - the Departed row publishes
			// nothing and the fees are OnAgentPhase's. The test reads FlightPhase::HasReachedStand: Turnaround,
			// Manoeuvring, TaxiOut, Departing.
			UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) restored mid-%s: retired as departed (its aircraft, agent %d, was not saved)"),
				Each->Id, *Each->Callsign, *FlightBoardText::PhaseName(Each->GetPhase()), Each->AgentId);
			TransitionTo(*Each, EFlightPhase::Departed, FTransitionCause::Played(Now));
		}
	}
	return Requeued;
}

int32 UFlightBoard::CancelUnarrivedAtLoad(double Now, UGroundTraffic* Traffic, USimClock* Clock)
{
	// REVIEW RULING I2 (PR C), WIDENED (whole-stack review I1): a flight a load leaves still to arrive, at an airport
	// that is not open, can never land - a closed airport admits no arrivals (PR B ruling I1). The re-queued (Inbound)
	// were the first case found; an Accepted flight saved at the closed airport is the other. CANCELLED, UNSCORED:
	// NOTHING IS PUBLISHED, so the roster charges neither ClosureCancelPenalty nor anything else; what closed the
	// airport happened before the save, and its own cancellations were scored then. Called before RearmSchedules and
	// OnGraphRebuilt, so no arrival is armed and no stand held for any of them - AND, SINCE #442, CORRECT WHEREVER IT IS
	// CALLED: it goes through the same Cancelled row a closure does, which releases the stand and disarms the arrival
	// when given the means (Traffic, Clock) and publishes nothing for a Load source.
	// ENFORCED BY: AirportOps.Present.RuntimeLoad.MidFlightAtClosedAirport, AirportOps.Present.Airport.ClosedLoadCancelsTheUnarrived,
	// AirportOps.Model.FlightSave.LoadCancelDoesNotDependOnItsPosition
	int32 Count = 0;
	// SNAPSHOT: MoveToHistory removes from the array being walked.
	const TArray<TObjectPtr<UFlight>> Snapshot = Flights;
	for (const TObjectPtr<UFlight>& Each : Snapshot)
	{
		if (Each == nullptr || !Each->IsUnarrived())
		{
			continue;
		}
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) %s at an airport that is not open, after a load: cancelled, unscored"),
			Each->Id, *Each->Callsign, Each->GetPhase() == EFlightPhase::Inbound ? TEXT("holding") : TEXT("accepted"));
		// THE REASON IS WHAT IT WOULD HAVE BEEN - never published for a Load source, so never scored.
		TransitionTo(*Each, EFlightPhase::Cancelled,
			FTransitionCause::Loaded(Now).WithWorld(Clock, Traffic).Cancelling(ECancelReason::AirportClosed));
		++Count;
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
		if (Each->GetPhase() == EFlightPhase::Offered)
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
	// ONCE PER FLIGHT, like the landing fee: an aeroplane that parks again after its taxi out began enters TaxiOut a second time, and
	// would be billed the overlapping hours again from the original ParkedAt (#442 review).
	// ENFORCED BY: AirportOps.Model.FlightFees.ParkingIsBilledOncePerFlight
	if (Ledger == nullptr || Flight.ParkedAt <= 0.0 || Flight.bParkingFeePaid)
	{
		return;
	}

	// THE RATE THE FLIGHT WAS OFFERED AT (#442), not the lever as it stands at departure: UPricing::ParkingFeePerHour applies the
	// CURRENT landing-fee multiplier, so asking it here let a player accept cheaply and put the price up before the aeroplane
	// left - the trade UOfferGenerator::MakeOffer rules out for the landing fee. A flight never offered (the debug land key's)
	// carries no rate and so pays no parking, as it already paid no landing fee.
	// ENFORCED BY: AirportOps.Model.FlightFees.ParkingIsBilledAtTheOffersRate
	const double Hours = FMath::Max(0.0, (Now - Flight.ParkedAt) / 3600.0);
	const double Fee = Flight.ParkingRatePerHour * Hours;
	if (Fee <= 0.0)
	{
		return;
	}

	Flight.bParkingFeePaid = true;
	Flight.ParkingFee = Fee;
	Ledger->Post(Now, ELedgerCategory::ParkingFee, Fee,
		FText::Format(NSLOCTEXT("Ledger", "ParkingBy", "Parking: {0}"), Flight.AirlineName));
}

void UFlightBoard::OnAgentPhase(const URoadNetwork& Network, const USimClock& Clock, const FAgentTransition& Transition)
{
	const int32 AgentId = Transition.AgentId;
	const EAgentPhase From = Transition.From;
	const EAgentPhase To = Transition.To;
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
	const EFlightPhase WasPhase = Flight->GetPhase();

	// WHERE IT PARKED, AS THE EVENT SAYS (#436): the transition's GoalAtEvent is the node it parked on when it parked,
	// through the same StandAtNode UJobBoard::OnAgentPhase asks (review M8). The fallback junction a stand-less
	// arrival waits on (UGroundTraffic::ReResolvePlan) is no stand's pose. This used to read the LIVE agent - Phase,
	// GoalNode, bDepartureArmed - because the bus delivers the event a step late and the (From, To) pair could not
	// say which taxi a Parked -> Taxiing was; its Cause says so now, and the two special cases that corrected the pair
	// (#405's fallback park, review M4's depart from it) are cases of FlightPhaseFromTransition, WHY comments and all.
	const FEntityInstanceId ParkedStand = To == EAgentPhase::Parked ? StandAtNode(Network, Transition.GoalAtEvent) : FEntityInstanceId();

	const EFlightPhase Next = FlightPhaseFromTransition(Transition, WasPhase, ParkedStand.IsSet());
	if (Transition.Cause == EAgentEvent::Parked && !FlightPhase::HasReachedStand(WasPhase) && !ParkedStand.IsSet())
	{
		// SAID, because it is a Parked that moves the flight nowhere - see FlightPhaseFromTransition's Parked case.
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s): its aircraft (agent %d) stopped short of a stand; still %s"),
			Flight->Id, *Flight->Callsign, AgentId, *FlightBoardText::PhaseName(WasPhase));
	}
	// THE PHASE CHANGE, THROUGH THE ONE WRITER (#442): whatever the new phase's row does - the agent unhooked when the flight
	// leaves the ground (Gone, and every other way off it), the AirborneAt stamp and FFlightAirborne publish on entering
	// Departing ("THE OTHER END OF THE TURNAROUND CONTRACT" - see UFlight::AirborneAt; taken once, LATENESS AGAINST THE
	// CONTRACT the row showed at the offer: AirborneBy is AcceptedAt + ContractSeconds), History for Departed - happens
	// there. THIS FUNCTION IS THE ONE DECISION POINT FOR AGENT-DRIVEN PHASES: FlightPhaseFromTransition says WHICH phase,
	// TransitionTo applies it.
	TransitionTo(*Flight, Next, FTransitionCause::Played(Clock.Now()));
	const bool bChanged = Flight->GetPhase() != WasPhase;
	if (bChanged)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s): %s -> %s (agent %d %s -> %s, %s)"),
			Flight->Id, *Flight->Callsign, *UEnum::GetValueAsString(WasPhase), *UEnum::GetValueAsString(Flight->GetPhase()),
			AgentId, *UEnum::GetValueAsString(From), *UEnum::GetValueAsString(To), *UEnum::GetValueAsString(Transition.Cause));
	}
	else if (To == EAgentPhase::Stranded)
	{
		// A STRANDED AEROPLANE MOVES ITS FLIGHT NOWHERE - FlightPhaseFromTransition's Stranded case. It is not
		// at its stand, so no turnaround starts; the flight stays in its taxi until the player
		// retires the aeroplane. Said, because it is the one agent phase change here with no
		// flight phase change to show for it.
		UE_LOG(LogAirportOps, Warning, TEXT("Flight %d (%s): its aircraft (agent %d) is stranded in %s; retire it to free the flight"),
			Flight->Id, *Flight->Callsign, AgentId, *UEnum::GetValueAsString(Flight->GetPhase()));
	}

	if (To == EAgentPhase::Parked)
	{
		// WHICH stand it actually got, which need not be the one held - see UFlight::Stand.
		// The node it parked on is the authority, exactly as UJobBoard reads it - the EVENT's, not the
		// live agent's: the bus delivers this a step late, and an aircraft redirected since has a GoalNode
		// that is its next stand, not this one. It used to be asked whether it was STILL parked to rule
		// that out; GoalAtEvent cannot be anything but the node it parked on (#436).
		if (ParkedStand.IsSet())
		{
			Flight->Stand = ParkedStand;
		}
	}

	if (To == EAgentPhase::Gone)
	{
		// THE AGENT IS GONE WHATEVER THE FLIGHT'S PHASE SAYS: FlightPhaseFromTransition books every Gone as Departed, whose row
		// unhooks it already; this is the guard for a Gone that somehow left the flight on the ground, where a hook to an
		// aeroplane that no longer exists would be found by some later agent's event.
		ByAgent.Remove(Flight->AgentId);
		Flight->AgentId = INDEX_NONE;
	}

	// THE MONEY, at two phases and those two specifically.
	//
	// Landing is where an aeroplane becomes the airport's business. TaxiOut is the one phase
	// EVERY departure reaches - Manoeuvring is NOT, because an aeroplane parked within
	// StraightOutDegrees of its exit heading simply drives out and never enters it (commit
	// 021cc2e). Charging parking at a phase some flights never enter would be a fee that went
	// silently uncollected on exactly the layouts the player built best.
	const double Now = Clock.Now();
	if (Flight->GetPhase() == EFlightPhase::Landing)
	{
		PostLandingFee(Now, *Flight);
	}
	else if (Flight->GetPhase() == EFlightPhase::Turnaround && Flight->ParkedAt <= 0.0)
	{
		// The start of the parking clock, taken once - Turnaround is reached again by anything
		// that re-enters it, and the second visit must not restart the meter in the player's
		// favour.
		Flight->ParkedAt = Now;
	}
	else if (Flight->GetPhase() == EFlightPhase::TaxiOut && bChanged)
	{
		// ON ENTERING TAXIOUT, NOT ON EVERY EVENT WHILE IN IT (#442): this block ran after every agent event, so a redirect of an
		// aeroplane already taxiing out (an edit re-routed it, a stranding was rescued) found the flight still TaxiOut and posted
		// the parking fee AGAIN, for a longer stay, as a second ledger row. The landing fee is guarded by its own flag and
		// the turnaround stamp by ParkedAt; this one had no guard.
		// ENFORCED BY: AirportOps.Model.FlightFees.ParkingIsBilledOnceAcrossARedirect
		PostParkingFee(Now, *Flight);
	}

	// A CHANGE OF PHASE BUMPED THE REVISION IN ITS ROW; one that changed nothing still did something a row would miss - the Stand
	// an agent parked on, a stranding the inbox shows - and OnAgentPhase has always bumped once for every event it heard.
	if (!bChanged)
	{
		++RevisionCount;
	}
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
		if (Each != nullptr && Each->IsUnarrived())
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
		if (Each->GetPhase() == EFlightPhase::Inbound && (!Each->Stand.IsSet() || UStandAllocator::HeldStandIsGone(*Each, Network))
			&& Allocator->Reserve(Traffic, Network, *Each))
		{
			UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) holding: stand %d held again"),
				Each->Id, *Each->Callsign, Each->Stand.Index);
		}
	}
}

void UFlightBoard::DisarmEveryArrival(USimClock& Clock)
{
	for (const TPair<int32, int32>& Handle : ArrivalHandles) { Clock.Cancel(Handle.Value); }
	ArrivalHandles.Reset();
	Verdicts.Reset();
	Clearances.Reset();
}

void UFlightBoard::RearmSchedules(UGroundTraffic& Traffic, const URoadNetwork& Network,
	USimClock& Clock)
{
	// CANCELLED, not just forgotten (PR #137 review): a handle left in Clock's own queue
	// after this map drops it is an orphaned entry that outlives every reference to it here -
	// harmless against a genuinely fresh post-load Clock (its queue starts empty), but this
	// function has no way to know that is the only time it is ever called, and an entry that
	// fires later still runs its captured lambda against whatever Id it named.
	DisarmEveryArrival(Clock);

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
		if (Each != nullptr && Each->GetPhase() == EFlightPhase::Offered)
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
		if (Each->IsLive())
		{
			Out.Add(Each);
		}
	}
	return Out;
}

// EVERY PHASE BY NAME BELOW, NO default: a missing case is a BUILD ERROR - see ExhaustiveSwitch.h for why it would not be
// otherwise. Diverted, when the sequencer makes it, lands here first.
// ENFORCED BY: C4062 as an error, AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (checked 2026-09-30 by a stray enumerator: the build failed here)
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
void UFlightBoard::TransitionTo(UFlight& Flight, EFlightPhase To, const FTransitionCause& Cause)
{
	const EFlightPhase From = Flight.Phase;
	if (From == To)
	{
		// NOT A TRANSITION. A caller that asks for the phase the flight is already in (an agent event that moves it nowhere is
		// not sent here at all) must not re-run the row: a second Departing would publish FFlightAirborne again, a second
		// terminal would file the flight in History twice.
		return;
	}
	Flight.Phase = To;

	// ---- WHAT LEAVING A PHASE DOES: the effects keyed on the phase LEFT ------------------------------------------------
	if (From == EFlightPhase::Offered)
	{
		// COUNTED AT AddOffer, UNCOUNTED HERE by every way out of Offered - accept, decline, lapse, withdrawal - which each
		// did by hand before.
		--OfferedCount;
	}
	if (From == EFlightPhase::Accepted)
	{
		// THE ARRIVAL LET GO OF, whichever way the flight leaves Accepted. Cancelling it on the clock is what a cancel needs, or
		// its Clock.At finds the flight by id even in History (ById keeps it until RollUp) and would put a cancelled flight
		// back in the queue. The ETA callback's own entry to Inbound has FIRED already and has no clock to cancel on, which
		// is why a missing Cause.Clock is only a fault when the flight is not joining the queue.
		// ENFORCED BY: AirportOps.Model.FlightBoard.CancelUnarrivedCancelsAndWithdraws ("still Cancelled after its ETA")
		if (const int32* Handle = ArrivalHandles.Find(Flight.Id))
		{
			if (Cause.Clock != nullptr)
			{
				Cause.Clock->Cancel(*Handle);
			}
			else if (To != EFlightPhase::Inbound)
			{
				UE_LOG(LogAirportOps, Error,
					TEXT("Flight %d left Accepted for %s with no clock to cancel its arrival on - it is still armed"),
					Flight.Id, *FlightBoardText::PhaseName(To));
			}
			ArrivalHandles.Remove(Flight.Id);
		}
	}
	if (FlightPhase::IsOnGround(From) && !FlightPhase::IsOnGround(To))
	{
		// THE AEROPLANE LEAVES THE FLIGHT whichever way off the ground it goes: departed (Gone), cancelled (the player's
		// despawn), or sent round again (a load's re-queue). A ByAgent entry that outlived its flight would be found by some LATER
		// agent's phase change and move a flight that has already finished - see MoveToHistory's DEFENSIVE comment.
		ByAgent.Remove(Flight.AgentId);
		Flight.AgentId = INDEX_NONE;
	}

	// ---- WHAT ENTERING A PHASE DOES: the row ------------------------------------------------------------------------
	switch (To)
	{
	case EFlightPhase::Offered:
		// NOTHING ENTERS AN OFFER: a flight is BORN Offered (UFlight::Phase's default), and every transition leaves it. Said,
		// because a caller that got here has a lifecycle bug and the row it would have run does not exist.
		UE_LOG(LogAirportOps, Error, TEXT("Flight %d moved back to Offered from %s - nothing enters an offer; its phase is now wrong"),
			Flight.Id, *FlightBoardText::PhaseName(From));
		break;

	case EFlightPhase::Accepted:
		// ARMS THE ARRIVAL, and says so: the player's yes. The hold was made by TryAccept before this (a refusal there never
		// gets here), and AcceptedAt and ArrivesAt with it.
		if (Cause.Traffic != nullptr && Cause.Clock != nullptr)
		{
			Schedule(*Cause.Traffic, *Cause.Clock, Flight);
		}
		else
		{
			UE_LOG(LogAirportOps, Error, TEXT("Flight %d accepted with no traffic model or clock to arm its arrival - it will never come"), Flight.Id);
		}
		if (Bus != nullptr)
		{
			Bus->Publish(FOfferAcceptedEvent{ Flight.Id, Flight.AirlineId, Flight.Stand });
		}
		break;

	case EFlightPhase::Inbound:
		// INTO THE QUEUE, stand kept: HoldingSince is the moment the caller dates it at - the ETA for the clock's callback,
		// now for a load's re-queue. The pass that clears flights wakes on the publish (ops batch 3 §5), for a flight that
		// joined by any door.
		Flight.HoldingSince = Cause.At;
		if (Bus != nullptr)
		{
			Bus->Publish(FFlightInboundEvent{ Flight.Id, Flight.AirlineId });
		}
		break;

	case EFlightPhase::Landing:
		// THE AEROPLANE HOOKED under the flight, when the caller has one (DispatchNow's). Not released here: DispatchNow frees
		// the stand hold BEFORE it dispatches, because the planner must not see it.
		if (Cause.AgentId != INDEX_NONE)
		{
			Flight.AgentId = Cause.AgentId;
			ByAgent.Add(Flight.AgentId, &Flight);
		}
		break;

	case EFlightPhase::TaxiIn:
	case EFlightPhase::Turnaround:
	case EFlightPhase::Manoeuvring:
	case EFlightPhase::TaxiOut:
		// THE AGENT DRIVES THESE and the flight follows: nothing to arm, hook or publish. The money they trigger (the
		// parking clock on Turnaround, the parking fee on TaxiOut) is OnAgentPhase's billing, not a phase effect.
		break;

	case EFlightPhase::Departing:
		// THE OTHER END OF THE TURNAROUND CONTRACT - see UFlight::AirborneAt. Taken once.
		if (Flight.AirborneAt <= 0.0)
		{
			Flight.AirborneAt = Cause.At;
			if (Bus != nullptr)
			{
				// LATENESS AGAINST THE CONTRACT the row showed at the offer: AirborneBy is AcceptedAt +
				// ContractSeconds. Published once, because AirborneAt is taken once.
				Bus->Publish(FFlightAirborneEvent{ Flight.Id, Flight.AirlineId, Flight.AirborneAt - Flight.AirborneBy() });
			}
		}
		break;

	case EFlightPhase::Departed:
		// TERMINAL, and nothing published: the airline scored the departure at Departing (FFlightAirborne). Filed below.
		break;

	case EFlightPhase::Declined:
		if (Bus != nullptr)
		{
			Bus->Publish(FOfferDeclinedEvent{ Flight.Id, Flight.AirlineId });
		}
		break;

	case EFlightPhase::Expired:
		// THE REASON IT LAPSED was set by the caller before this (TickOffers), and is what the roster scores by.
		if (Bus != nullptr)
		{
			Bus->Publish(FOfferExpiredEvent{ Flight.Id, Flight.AirlineId, Flight.LapseReason, Flight.bFloorAirline });
		}
		break;

	case EFlightPhase::Withdrawn:
		// AN OFFER THE AIRPORT TOOK BACK, publishing nothing: no OfferExpired, so no Ignored penalty - nobody let it lapse.
		break;

	case EFlightPhase::Cancelled:
		// FROM A FLIGHT STILL TO ARRIVE, ITS STAND HOLD IS RELEASED. A flight cancelled on the ground (CancelByAgent) holds none
		// - DispatchNow released it - and the Accepted-row disarm above already ran. The release needs the traffic model: a
		// caller that cannot give one (a load with no model) has no claims to release, and a PLAY caller that did not is a bug.
		if (FlightPhase::IsUnarrived(From) && Allocator != nullptr)
		{
			if (Cause.Traffic != nullptr)
			{
				Allocator->Release(*Cause.Traffic, Flight);
			}
			else if (Cause.Source == EFlightChangeSource::Play)
			{
				UE_LOG(LogAirportOps, Error, TEXT("Flight %d cancelled from %s with no traffic model to release its stand hold on"),
					Flight.Id, *FlightBoardText::PhaseName(From));
			}
		}
		// A LOAD'S CANCEL IS NOT NEWS: what closed the airport happened before the save, and its own cancellations were scored
		// then - publishing here would charge ClosureCancelPenalty again (PR C ruling I2). The one deliberate difference between
		// two doors onto this row.
		// ENFORCED BY: AirportOps.Present.RuntimeLoad.MidFlightAtClosedAirport, AirportOps.Model.FlightBoard.CancelUnarrivedCancelsAndWithdraws
		// A PLAY CANCEL MUST SAY WHY: the reason is what the roster scores by, and Unstuck - the free one - was the silent default
		// of a door that forgot it (#442 review). Loud and unpublished instead: an event with an invented reason is a lie.
		// ENFORCED BY: AirportOps.Model.FlightBoard.CancelWithNoReasonIsLoudNotFree
		if (Cause.Source == EFlightChangeSource::Play)
		{
			if (!Cause.CancelReason.IsSet())
			{
				UE_LOG(LogAirportOps, Error, TEXT("Flight %d cancelled from %s with no reason given - nothing published, so the airline is not charged and nobody is told"),
					Flight.Id, *FlightBoardText::PhaseName(From));
			}
			else if (Bus != nullptr)
			{
				Bus->Publish(FFlightCancelledEvent{ Flight.Id, Flight.AirlineId, Cause.CancelReason.GetValue() });
			}
		}
		break;
	}

	// ---- EVERY TERMINAL PHASE IS FILED IN HISTORY, at the moment the caller dates it: the one place a flight leaves Flights.
	if (FlightPhase::IsTerminal(To))
	{
		MoveToHistory(Flight, Cause.At);
	}
	++RevisionCount;
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

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
