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
		// AT A STAND - which this function cannot see. UFlightBoard::OnAgentPhase asks the agent and keeps a
		// flight parked on the fallback junction in its taxi (#405); this is the answer once it has.
		// ENFORCED BY: AirportOps.Model.Bus.FallbackParkStaysTaxiIn
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

	case EAgentPhase::Stranded:
		// NOWHERE, by name rather than by the default (issue #396): the aeroplane stopped short of
		// its stand, so no turnaround has started, and the flight reads the taxi it was in until the
		// player retires the aeroplane. Turnaround here was the stranded-at-the-stand bug.
		return Current;

	default:
		// Unchanged rather than a guess. A phase this does not know about must not move a
		// flight backwards through states the inbox is showing.
		return Current;
	}
}
