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
