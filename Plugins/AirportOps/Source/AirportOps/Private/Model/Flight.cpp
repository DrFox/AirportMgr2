#include "Model/Flight.h"

#include "Model/RoadAgent.h"

EFlightPhase FlightPhaseFromAgent(EAgentPhase To, EFlightPhase Current)
{
	switch (To)
	{
	case EAgentPhase::Arriving:
		return EFlightPhase::Landing;

	case EAgentPhase::Taxiing:
		// Parked or later means this is the taxi OUT; anything earlier is the taxi in. The
		// comparison reads EFlightPhase's declaration order, which its own comment pins.
		return Current >= EFlightPhase::Turnaround ? EFlightPhase::TaxiOut : EFlightPhase::TaxiIn;

	case EAgentPhase::Parked:
		return EFlightPhase::Turnaround;

	case EAgentPhase::Manoeuvring:
		// WITHOUT THIS CASE the default below leaves the flight reading "Turnaround" while the
		// aeroplane is visibly moving off its stand - the board and the apron disagreeing, with
		// nothing to say which was right.
		return EFlightPhase::Manoeuvring;

	case EAgentPhase::Departing:
		return EFlightPhase::Departing;

	case EAgentPhase::Gone:
		return EFlightPhase::Departed;

	default:
		// Unchanged rather than a guess. A phase this does not know about must not move a
		// flight backwards through states the inbox is showing.
		return Current;
	}
}
