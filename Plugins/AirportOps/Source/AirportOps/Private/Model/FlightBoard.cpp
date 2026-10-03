#include "Model/FlightBoard.h"

#include "AirportOpsLog.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/OpsEventBus.h"
#include "Model/AirsideCapability.h"
#include "Model/Flight.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Model/Turnarounds.h"
#include "Algo/StableSort.h"

// FlightBoardText::PhaseName WENT TO Flight.h AS FlightPhase::Name (#442 item 4): the arrival queue logs a phase too now, and one
// spelling beats a copy per file.

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

// ---- THE OFFERS: FORWARDERS to FOfferInbox (#442 item 4) - each body moved there whole, see OfferInbox.cpp. --------------------

void UFlightBoard::TickOffers(const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock, double RealDeltaSeconds)
{
	Inbox.TickOffers(*this, Traffic, Network, Clock, RealDeltaSeconds);
}

const FOfferVerdict& UFlightBoard::VerdictFor(const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const UFlight& Flight) const
{
	return Inbox.VerdictFor(*this, Traffic, Network, Flight);
}

FArrivalQuote UFlightBoard::QuoteFor(const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const UFlight& Flight) const
{
	return Inbox.QuoteFor(*this, Traffic, Network, Flight);
}

FArrivalQuote UFlightBoard::QuoteArrival(const UGroundTraffic& Traffic, const URoadNetwork& Network,
	const FAirframe& Airframe, const FVector2D& Focus) const
{
	return Inbox.QuoteArrival(*this, Traffic, Network, Airframe, Focus);
}

FArrivalQuote UFlightBoard::TryAccept(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock,
	UFlight& Flight)
{
	return Inbox.TryAccept(*this, Traffic, Network, Clock, Flight);
}

void UFlightBoard::Decline(USimClock& Clock, UFlight& Flight)
{
	Inbox.Decline(*this, Clock, Flight);
}

EArrivalRefusal UFlightBoard::WhyNotAcceptable(const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const UFlight& Flight) const
{
	return Inbox.WhyNotAcceptable(*this, Traffic, Network, Flight);
}

EArrivalRefusal UFlightBoard::AcceptImmediate(UGroundTraffic& Traffic, const URoadNetwork& Network,
	USimClock& Clock, const FAirframe& Airframe, const FVector2D& Focus, FText Airline, FString* OutSentence)
{
	return Inbox.AcceptImmediate(*this, Traffic, Network, Clock, Airframe, Focus, MoveTemp(Airline), OutSentence);
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

bool UFlightBoard::Accept(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock,
	UFlight& Flight)
{
	return TryAccept(Traffic, Network, Clock, Flight).IsAccepted();
}

// ---- THE ARRIVALS: FORWARDERS to FArrivalQueue (#442 item 4) - each body moved there whole, see ArrivalQueue.cpp. --------------

FQueueTick UFlightBoard::TickQueue(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
{
	return Arrivals.Tick(*this, Traffic, Network, Clock);
}

TArray<UFlight*> UFlightBoard::Queue() const
{
	return Arrivals.Queue(*this);
}

EArrivalRefusal UFlightBoard::UnlandableWhy(const UFlight& Flight, const URoadNetwork& Network) const
{
	return Arrivals.UnlandableWhy(Flight, Network);
}

void UFlightBoard::RestoreStandHolds(UGroundTraffic& Traffic, const URoadNetwork& Network, const TArray<UFlight*>& HoldLast)
{
	Arrivals.RestoreStandHolds(*this, Traffic, Network, HoldLast);
}

void UFlightBoard::RearmSchedules(UGroundTraffic& Traffic, const URoadNetwork& Network, USimClock& Clock)
{
	Arrivals.RearmSchedules(*this, Traffic, Network, Clock);
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
	RestoreStandHolds(*Traffic, Network, Requeued);
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
			// RestoreStandHolds holds it again AFTER every genuine holder, and finds it another if that one was taken.
			// bLandingFeePaid IS LEFT AS SAVED rather than forced true: it is saved and set with the ledger post
			// (FlightBilling::PostLandingFee), so a flight that was charged stays charged and its second landing posts nothing - and
			// one saved before its landing was heard is charged once, when it lands.
			// ENFORCED BY: AirportOps.Model.FlightSave.UnchargedLandingIsChargedOnce
			UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) restored mid-%s: re-queued (its aircraft, agent %d, was not saved)"),
				Each->Id, *Each->Callsign, *FlightPhase::Name(Each->GetPhase()), Each->AgentId);
			// THROUGH Enqueue, THE LIVE DOOR INTO THE QUEUE (#426): Inbound, HoldingSince, the revision, and the
			// FlightInbound that wakes the arrival-queue pass - which this used to skip by setting the phase by hand,
			// leaving the load's MarkAllDirty as the only thing that dispatched it. THE INBOUND ROW ALSO LETS GO OF THE
			// AEROPLANE (ByAgent and AgentId): leaving the ground unhooks it, which this used to do by hand just above.
			Arrivals.Enqueue(*this, *Each, Now);
			Requeued.Add(Each);
		}
		else if (FlightPhase::HasReachedStand(Each->GetPhase()))
		{
			// DEPARTED, UNSCORED: the save system is not the player's fault, so nothing the airline roster scores is
			// published (no FlightOffBlocks, no TurnaroundEnded) and no parking fee is posted: the Departed row publishes
			// nothing of its own, and the FFlightPhaseChangedEvent every change publishes reaches a billing reaction
			// (FlightBilling::OnFlightPhaseChanged) that does not react to Departed - it bills Landing, Turnaround and TaxiOut.
			// ENFORCED BY: AirportOps.Model.FlightSave.MidFlightGoesRoundOrRetires ("the load posted no fee").
			// The test reads FlightPhase::HasReachedStand: Turnaround,
			// Manoeuvring, TaxiOut, Departing.
			UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s) restored mid-%s: retired as departed (its aircraft, agent %d, was not saved)"),
				Each->Id, *Each->Callsign, *FlightPhase::Name(Each->GetPhase()), Each->AgentId);
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
	// RestoreStandHolds, so no arrival is armed and no stand held for any of them - AND, SINCE #442, CORRECT WHEREVER IT IS
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
	// read through FTurnarounds::BeganAt - THE ONE DERIVATION of "a turnaround began" that the job board opens its
	// turnaround on (#427; review M8 made the two boards share StandAtNode, and #427 the whole question, where this asked
	// on To == Parked and the job board on the Parked cause). The fallback junction a stand-less
	// arrival waits on (UGroundTraffic::ReResolvePlan) is no stand's pose. This used to read the LIVE agent - Phase,
	// GoalNode, bDepartureArmed - because the bus delivers the event a step late and the (From, To) pair could not
	// say which taxi a Parked -> Taxiing was; its Cause says so now, and the two special cases that corrected the pair
	// (#405's fallback park, review M4's depart from it) are cases of FlightPhaseFromTransition, WHY comments and all.
	// ENFORCED BY: Check-Architecture rule 78 (turnaround-began-once), AirportOps.Model.Turnarounds.BothBoardsBeginAtTheOneStand
	const FEntityInstanceId ParkedStand = FTurnarounds::BeganAt(Network, Transition);

	const EFlightPhase Next = FlightPhaseFromTransition(Transition, WasPhase, ParkedStand.IsSet());
	if (Transition.Cause == EAgentEvent::Parked && !FlightPhase::HasReachedStand(WasPhase) && !ParkedStand.IsSet())
	{
		// SAID, because it is a Parked that moves the flight nowhere - see FlightPhaseFromTransition's Parked case.
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d (%s): its aircraft (agent %d) stopped short of a stand; still %s"),
			Flight->Id, *Flight->Callsign, AgentId, *FlightPhase::Name(WasPhase));
	}
	// THE PHASE CHANGE, THROUGH THE ONE WRITER (#442): whatever the new phase's row does - the agent unhooked when the flight
	// leaves the ground (Gone, and every other way off it), the OnBlocksAt stamp on entering Turnaround and the OffBlocksAt
	// stamp and FFlightOffBlocks publish on leaving the stand ("THE OTHER END OF THE TURNAROUND CONTRACT" - see
	// UFlight::OffBlocksAt; taken once, LATENESS AGAINST THE CONTRACT the row showed at the offer: OffBlocksBy is OnBlocksAt +
	// ContractSeconds, #398), History for Departed - happens
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
		// The node it parked on is the authority, read through the one derivation UJobBoard reads (BeganAt) - the EVENT's, not the
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

	// NO MONEY HERE since #442 item 4: the landing fee, the parking clock and the parking fee are FlightBilling's REACTION to the
	// FFlightPhaseChangedEvent the TransitionTo above published - a round later, in the same drain. The block that posted them after
	// every event this heard moved there whole, its reasons with it (FlightBilling::OnFlightPhaseChanged).
	// ENFORCED BY: AirportOps.Present.Bus.BillingIsWired, AirportOps.Model.FlightFees.BilledOnTheBusARoundLater (the change itself
	// posts nothing), Check-Architecture rule 4 ('FlightBilling reaction'; 'fee posters called outside FlightBilling' - billing
	// inline here again would be a PostLandingFee/PostParkingFee call)

	// A CHANGE OF PHASE BUMPED THE REVISION IN ITS ROW; one that changed nothing still did something a row would miss - the Stand
	// an agent parked on, a stranding the inbox shows - and OnAgentPhase has always bumped once for every event it heard.
	if (!bChanged)
	{
		++RevisionCount;
	}
}

void UFlightBoard::DisarmEveryArrival(USimClock& Clock)
{
	// THE QUEUE'S HANDLES AND CACHES, THEN THE INBOX'S VERDICTS (#442 item 4): the same four resets as before the split, the
	// verdicts last rather than second - independent maps, so the order among them is no behaviour.
	Arrivals.DisarmEvery(Clock);
	Inbox.ForgetAll();
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
		// not sent here at all) must not re-run the row: a second Manoeuvring would be a second off-blocks, a second
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
		// THE ARRIVAL LET GO OF, whichever way the flight leaves Accepted - the handle is the queue's since #442 item 4, and so is
		// the block that cancels it (its reasons went with it); this row still says WHEN.
		Arrivals.Disarm(Flight, To, Cause.Clock);
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
			Flight.Id, *FlightPhase::Name(From));
		break;

	case EFlightPhase::Accepted:
		// ARMS THE ARRIVAL, and says so: the player's yes. The hold was made by TryAccept before this (a refusal there never
		// gets here), and AcceptedAt and ArrivesAt with it.
		if (Cause.Traffic != nullptr && Cause.Clock != nullptr)
		{
			Arrivals.Schedule(*this, *Cause.Traffic, *Cause.Clock, Flight);
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
	case EFlightPhase::Departing:
		// THE AGENT DRIVES THESE: nothing to arm, hook or publish. The ground phases' money (parking clock, parking fee) is billing's
		// REACTION to FFlightPhaseChangedEvent below. DEPARTING SCORES NOTHING since #398: the contract ended at off-blocks.
		break;

	case EFlightPhase::Turnaround:
		// ON BLOCKS: the turnaround contract starts, once (#398) - see UFlight::MarkOnBlocks.
		Flight.MarkOnBlocks(Cause.At);
		break;

	case EFlightPhase::Manoeuvring:
	case EFlightPhase::TaxiOut:
		// OFF BLOCKS, the contract's other end, scored ONCE against the contract the row showed (#398) - see UFlight::MarkOffBlocks.
		if (const TOptional<double> LateBy = Flight.MarkOffBlocks(Cause.At); LateBy.IsSet() && Bus != nullptr)
		{
			Bus->Publish(FFlightOffBlocksEvent{ Flight.Id, Flight.AirlineId, LateBy.GetValue() });
		}
		break;

	case EFlightPhase::Departed:
		// TERMINAL, and nothing of its own published: the airline scored the turnaround at off-blocks (FFlightOffBlocks). Filed
		// below; the FFlightPhaseChangedEvent every change publishes bills nothing for it.
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
					Flight.Id, *FlightPhase::Name(From));
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
					Flight.Id, *FlightPhase::Name(From));
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

	// ---- EVERY CHANGE, ANNOUNCED ONCE (#442 item 4): what the billing reaction hears - the money OnAgentPhase posted inline is
	// FlightBilling's answer to THIS, a round later in the same drain. LAST, after the row's own publish and the move to History, so a
	// listener that resolves the flight finds it where the change left it; dated by Cause.At, so a fee priced a round later is priced
	// as of the change. Published for a load's changes too: none enters a phase billing reacts to (see FFlightPhaseChangedEvent).
	// ENFORCED BY: AirportOps.Model.FlightBoard.EveryChangeIsPublishedOnce, AirportOps.Model.FlightSave.MidFlightGoesRoundOrRetires
	// (a load's Inbound, Departed and Cancelled post no fee)
	if (Bus != nullptr)
	{
		Bus->Publish(FFlightPhaseChangedEvent{ Flight.Id, From, To, Cause.At });
	}
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
	// EACH OWNER'S CACHE FOR IT, forgotten - the verdict the inbox's, the clearance and re-hold miss the queue's (#442 item 4).
	Inbox.Forget(Flight.Id);
	Arrivals.Forget(Flight.Id);

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
