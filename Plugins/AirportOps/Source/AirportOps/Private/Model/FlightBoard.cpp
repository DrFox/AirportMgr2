#include "Model/FlightBoard.h"

#include "AirportOpsLog.h"
#include "Model/AirsideCapability.h"
#include "Model/Flight.h"
#include "Model/Ledger.h"
#include "Model/Pricing.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"

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
	ScheduleExpiry(Clock, *Offer);
	++RevisionCount;
}

void UFlightBoard::ScheduleExpiry(USimClock& Clock, UFlight& Offer)
{
	// WEAK BY ID, exactly as Schedule (arrivals) does - the callback outlives this call by
	// design, and the flight it names may be gone (declined, accepted then departed, or the
	// board itself torn down) long before its ExpiresAt comes due.
	const int32 Id = Offer.Id;
	const int32 Handle = Clock.At(Offer.ExpiresAt, [this, Id]()
	{
		UFlight* Due = FindById(Id);
		ExpiryHandles.Remove(Id);
		// STILL Offered, not just found: Accept/Decline cancel this handle, but a callback
		// already popped off the clock's queue this same Advance() cannot be un-fired -
		// the phase check is the second guard for that ordering, not a substitute for
		// CancelExpiry.
		if (Due != nullptr && Due->Phase == EFlightPhase::Offered)
		{
			Due->Phase = EFlightPhase::Expired;
			--OfferedCount;
			UE_LOG(LogAirportOps, Log, TEXT("Offer %d lapsed unanswered"), Due->Id);
			// USES ExpiresAt, NOT Clock.Now(): this lambda only captured Id, and ExpiresAt IS
			// the moment Advance decided to fire this callback in the first place.
			MoveToHistory(*Due, Due->ExpiresAt);
			++RevisionCount;
		}
	});
	ExpiryHandles.Add(Offer.Id, Handle);
}

void UFlightBoard::CancelExpiry(USimClock& Clock, UFlight& Offer)
{
	int32 Handle = INDEX_NONE;
	if (ExpiryHandles.RemoveAndCopyValue(Offer.Id, Handle))
	{
		Clock.Cancel(Handle);
	}
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

	if (!Allocator->Reserve(Traffic, Network, Flight))
	{
		UE_LOG(LogAirportOps, Log, TEXT("Flight %d not accepted: no stand admits it"), Flight.Id);
		return false;
	}

	Flight.Phase = EFlightPhase::Accepted;
	--OfferedCount;
	CancelExpiry(Clock, Flight);
	Schedule(Traffic, Clock, Flight);

	UE_LOG(LogAirportOps, Log, TEXT("Flight %d accepted: stand %d held, landing at %.0f"),
		Flight.Id, Flight.Stand.Index, Flight.ArrivesAt);
	++RevisionCount;
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
		UGroundTraffic* Model = WeakTraffic.Get();
		UFlight* Due = FindById(Id);
		if (Model != nullptr && Due != nullptr)
		{
			DispatchNow(*Model, *Due);
		}
	});
	ArrivalHandles.Add(Flight.Id, Handle);
}

void UFlightBoard::DispatchNow(UGroundTraffic& Traffic, UFlight& Flight)
{
	ArrivalHandles.Remove(Flight.Id);

	if (!Dispatcher)
	{
		UE_LOG(LogAirportOps, Warning,
			TEXT("Flight %d came due with no dispatcher: the board is not attached"), Flight.Id);
		return;
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
		// Stays Accepted, with no hold. Queueing this properly is the sequencer's job and it
		// does not exist yet; saying so is what stops it being a silent disappearance.
		UE_LOG(LogAirportOps, Warning,
			TEXT("Flight %d could not be dispatched at its ETA; it keeps no stand"), Flight.Id);
		++RevisionCount;
		return;
	}

	Flight.AgentId = Traffic.GetNewestAgentId();
	ByAgent.Add(Flight.AgentId, &Flight);
	Flight.Phase = EFlightPhase::Landing;
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d dispatched as agent %d"), Flight.Id, Flight.AgentId);
	++RevisionCount;
}

void UFlightBoard::Decline(USimClock& Clock, UFlight& Flight)
{
	if (Flight.Phase != EFlightPhase::Offered)
	{
		return;
	}
	CancelExpiry(Clock, Flight);
	Flight.Phase = EFlightPhase::Declined;
	--OfferedCount;
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d declined"), Flight.Id);
	MoveToHistory(Flight, Clock.Now());
	++RevisionCount;
}

EArrivalRefusal UFlightBoard::WhyNotAcceptable(const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const UFlight& Flight) const
{
	// #169: counted before the search runs, not after - GetWhyNotAcceptableCallsForTest exists
	// to measure exactly how often this expensive call is reached, whatever it returns.
	++WhyNotAcceptableCallsForTest;
	const FArrivalPlan Plan = ArrivalPlanner::Plan(Network, Flight.ApproachFocus, Flight.Airframe,
		&Traffic.GetOccupancy());
	return Plan.Why;
}

EArrivalRefusal UFlightBoard::AcceptImmediate(UGroundTraffic& Traffic, const URoadNetwork& Network,
	USimClock& Clock, const FAirframe& Airframe, const FVector2D& Focus, FText Airline)
{
	UFlight* Flight = NewObject<UFlight>(this);
	Flight->Airframe = Airframe;
	Flight->AirlineName = Airline;
	Flight->TypeName = FText::FromName(Airframe.TypeCode);

	// NOW, not the generator's lead time: AcceptImmediate exists to put an aeroplane on the
	// field this second - see its own header.
	Flight->ArrivesAt = Clock.Now();
	Flight->ExpiresAt = Clock.Now();
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
			|| Each->Phase == EFlightPhase::Departed)
		{
			// TerminatedAt DID NOT EXIST before this change, so a flight loaded from a save
			// that predates it has none - ExpiresAt (for a lapsed offer) or ArrivesAt (for
			// anything else terminal) is the closest recorded moment to when it actually
			// finished, and is only ever a fallback for THIS one-time migration: a flight
			// retired after this change always carries the real TerminatedAt its own call
			// site set - see Decline, the ScheduleExpiry callback, and OnAgentPhase below.
			const double Approx =
				Each->Phase == EFlightPhase::Expired ? Each->ExpiresAt : Each->ArrivesAt;
			MoveToHistory(*Each, Approx);
		}
	}

	RebuildIndices();
}

void UFlightBoard::RebuildIndices()
{
	// THE MAPS AND THE COUNTER (see their own comments) ARE NOT UPROPERTYs and so never
	// survive a load - rebuilt here from Flights/History, which ARE the saved truth, rather
	// than at every FindByAgent/FindById/PendingOfferCount call, which is the whole point of
	// maintaining them at all.
	//
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

	Flight->Phase = FlightPhaseFromAgent(To, Flight->Phase);

	if (To == EAgentPhase::Parked)
	{
		// WHICH stand it actually got, which need not be the one held - see UFlight::Stand.
		// The agent's own GoalNode is the authority, exactly as UFuelService reads it.
		if (const FRoadAgent* Agent = Traffic.FindAgent(AgentId))
		{
			const int32 Index = Network.FindEntityIndexByPoseNode(Agent->GoalNode);
			if (Index != INDEX_NONE)
			{
				Flight->Stand = Network.EntityIdAt(Index);
			}
		}
	}

	if (To == EAgentPhase::Gone)
	{
		ByAgent.Remove(Flight->AgentId);
		Flight->AgentId = INDEX_NONE;
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

void UFlightBoard::OnGraphRebuilt(UGroundTraffic& Traffic, const URoadNetwork& Network)
{
	if (Allocator == nullptr)
	{
		return;
	}

	TArray<UFlight*> Holding;
	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each != nullptr && Each->Phase == EFlightPhase::Accepted)
		{
			Holding.Add(Each);
		}
	}
	Allocator->Reapply(Traffic, Network, Holding);
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
	for (const TPair<int32, int32>& Handle : ExpiryHandles) { Clock.Cancel(Handle.Value); }
	ArrivalHandles.Reset();
	ExpiryHandles.Reset();

	// REBUILT HERE TOO, NOT ONLY IN OnAfterRestore: this is the one call every load path is
	// documented to make (see this function's own header), while OnAfterRestore only runs
	// under OpsSave::Restore. A board deserialised directly - OpsSave::SerializeObject/
	// DeserializeObject, which FFlightSurvivesASaveTest uses on purpose to isolate the clock's
	// own re-arm from the rest of Restore - would otherwise re-arm a Schedule/ScheduleExpiry
	// callback whose eventual FindById(Id) found nothing, because ById was still empty.
	RebuildIndices();

	// SNAPSHOT, NOT A LIVE ITERATION: the Offered branch below can call MoveToHistory, which
	// removes its argument from Flights - mutating the array a range-based for is walking.
	const TArray<TObjectPtr<UFlight>> Loaded = Flights;
	for (const TObjectPtr<UFlight>& Each : Loaded)
	{
		if (Each == nullptr)
		{
			continue;
		}

		if (Each->Phase == EFlightPhase::Offered)
		{
			if (Each->ExpiresAt <= Clock.Now())
			{
				// Its offer window passed while the game was shut - the same "act at once,
				// and say so" rule the arrival branch below follows, rather than leaving a
				// stale "still offered" row nothing will ever expire.
				Each->Phase = EFlightPhase::Expired;
				--OfferedCount;
				UE_LOG(LogAirportOps, Log,
					TEXT("Offer %d expired at %.0f, before this load at %.0f: lapsed on load"),
					Each->Id, Each->ExpiresAt, Clock.Now());
				MoveToHistory(*Each, Each->ExpiresAt);
				continue;
			}
			ScheduleExpiry(Clock, *Each);
			continue;
		}

		if (Each->Phase != EFlightPhase::Accepted)
		{
			continue;
		}

		if (Each->ArrivesAt <= Clock.Now())
		{
			// Its slot passed while the game was shut. It is still owed an arrival, so it
			// gets one at once rather than being dropped - and it says so, because a flight
			// that silently never came is the exact bug this whole function exists to stop.
			UE_LOG(LogAirportOps, Log,
				TEXT("Flight %d was due at %.0f, before this load at %.0f: dispatching now"),
				Each->Id, Each->ArrivesAt, Clock.Now());
			DispatchNow(Traffic, *Each);
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
