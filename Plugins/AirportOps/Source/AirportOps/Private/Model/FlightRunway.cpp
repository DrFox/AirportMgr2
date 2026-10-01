#include "Model/FlightRunway.h"

#include "Model/ExhaustiveSwitch.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RunwayQuery.h"

namespace FlightRunway
{
	// EVERY PHASE BY NAME, NO default - Check-Architecture rule 58, and a new phase is a build error here until it says
	// which runway it is on.
	AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	TArray<FRunwayEnd> For(const UFlightBoard& Board, const UGroundTraffic& Traffic, const UFlight& Flight)
	{
		const FRoadAgent* Agent = Traffic.FindAgent(Flight.AgentId);
		switch (Flight.GetPhase())
		{
		case EFlightPhase::Inbound:
			return Board.UsableRunwaysFor(Flight.Id);
		case EFlightPhase::Landing:
		case EFlightPhase::TaxiIn:
			return Agent != nullptr ? TArray<FRunwayEnd>{ Agent->Arrival.End } : TArray<FRunwayEnd>();
		case EFlightPhase::TaxiOut:
			// ONCE ARMED: a taxi out whose route has not reached a runway yet has none to name.
			return Agent != nullptr && Agent->bDepartureArmed ? TArray<FRunwayEnd>{ Agent->DepartureOrder.End } : TArray<FRunwayEnd>();
		case EFlightPhase::Departing:
			return Agent != nullptr ? TArray<FRunwayEnd>{ Agent->Departure.End } : TArray<FRunwayEnd>();
		case EFlightPhase::Offered:
		case EFlightPhase::Accepted:
		case EFlightPhase::Turnaround:
		case EFlightPhase::Manoeuvring:
		case EFlightPhase::Declined:
		case EFlightPhase::Expired:
		case EFlightPhase::Departed:
		case EFlightPhase::Cancelled:
		case EFlightPhase::Withdrawn:
			return TArray<FRunwayEnd>();
		}
		return TArray<FRunwayEnd>();
	}
	AIRSIDE_EXHAUSTIVE_SWITCH_END

	FString Names(const URoadNetwork& Network, TConstArrayView<FRunwayEnd> Ends)
	{
		TArray<FString> Out;
		for (const FRunwayEnd& End : Ends)
		{
			const FString Name = RunwayQuery::EndName(Network, End);
			if (!Name.IsEmpty())
			{
				Out.AddUnique(Name);
			}
		}
		return FString::Join(Out, TEXT(" or "));
	}
}
