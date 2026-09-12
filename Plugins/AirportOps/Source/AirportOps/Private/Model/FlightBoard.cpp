#include "Model/FlightBoard.h"

#include "AirportOpsLog.h"
#include "Model/Flight.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"

void UFlightBoard::AddOffer(UFlight* Offer)
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
	OnChanged.Broadcast();
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
	Schedule(Traffic, Clock, Flight);

	UE_LOG(LogAirportOps, Log, TEXT("Flight %d accepted: stand %d held, landing at %.0f"),
		Flight.Id, Flight.Stand.Index, Flight.ArrivesAt);
	OnChanged.Broadcast();
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
		OnChanged.Broadcast();
		return;
	}

	Flight.AgentId = Traffic.GetNewestAgentId();
	Flight.Phase = EFlightPhase::Landing;
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d dispatched as agent %d"), Flight.Id, Flight.AgentId);
	OnChanged.Broadcast();
}

void UFlightBoard::Decline(UFlight& Flight)
{
	if (Flight.Phase != EFlightPhase::Offered)
	{
		return;
	}
	Flight.Phase = EFlightPhase::Declined;
	UE_LOG(LogAirportOps, Log, TEXT("Flight %d declined"), Flight.Id);
	OnChanged.Broadcast();
}

EArrivalRefusal UFlightBoard::WhyNotAcceptable(const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const UFlight& Flight) const
{
	const FArrivalPlan Plan = ArrivalPlanner::Plan(Network, Flight.ApproachFocus, Flight.Airframe,
		&Traffic.GetOccupancy());
	return Plan.Why;
}

UFlight* UFlightBoard::MakeImmediateFlight(const FAirframe& Airframe, const FVector2D& Focus,
	FText Airline, double Now)
{
	UFlight* Flight = NewObject<UFlight>(this);
	Flight->Airframe = Airframe;
	Flight->AirlineName = Airline;
	Flight->TypeName = FText::FromName(Airframe.TypeCode);

	// NOW, not the generator's lead time: AcceptImmediate exists to put an aeroplane on the
	// field this second - see its own header.
	Flight->ArrivesAt = Now;
	Flight->ExpiresAt = Now;
	Flight->ApproachFocus = Focus;
	return Flight;
}

EArrivalRefusal UFlightBoard::AcceptImmediate(UGroundTraffic& Traffic, const URoadNetwork& Network,
	USimClock& Clock, const FAirframe& Airframe, const FVector2D& Focus, FText Airline)
{
	UFlight* Flight = MakeImmediateFlight(Airframe, Focus, Airline, Clock.Now());
	AddOffer(Flight);

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

void UFlightBoard::Tick(USimClock& Clock)
{
	const double Now = Clock.Now();
	bool bChanged = false;

	for (TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each != nullptr && Each->Phase == EFlightPhase::Offered && Now >= Each->ExpiresAt)
		{
			Each->Phase = EFlightPhase::Expired;
			UE_LOG(LogAirportOps, Log, TEXT("Offer %d lapsed unanswered"), Each->Id);
			bChanged = true;
		}
	}

	if (bChanged)
	{
		OnChanged.Broadcast();
	}
}

void UFlightBoard::OnAgentPhase(const UGroundTraffic& Traffic, const URoadNetwork& Network,
	int32 AgentId, EAgentPhase From, EAgentPhase To)
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
		Flight->AgentId = INDEX_NONE;
	}

	OnChanged.Broadcast();
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
	ArrivalHandles.Reset();

	for (const TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each == nullptr || Each->Phase != EFlightPhase::Accepted)
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

UFlight* UFlightBoard::FindByAgent(int32 AgentId)
{
	if (AgentId == INDEX_NONE)
	{
		return nullptr;
	}
	for (TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each != nullptr && Each->AgentId == AgentId)
		{
			return Each;
		}
	}
	return nullptr;
}

UFlight* UFlightBoard::FindById(int32 Id)
{
	for (TObjectPtr<UFlight>& Each : Flights)
	{
		if (Each != nullptr && Each->Id == Id)
		{
			return Each;
		}
	}
	return nullptr;
}
