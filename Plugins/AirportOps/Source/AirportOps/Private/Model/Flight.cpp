#include "Model/Flight.h"

#include "Model/ExhaustiveSwitch.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"

FEntityInstanceId StandAtNode(const URoadNetwork& Network, FGuidelineNodeId Node)
{
	const int32 Index = Network.FindEntityIndexByPoseNode(Node);
	return Index != INDEX_NONE && Network.GetEntities()[Index].IsStand() ? Network.EntityIdAt(Index) : FEntityInstanceId();
}

// A MISSING CASE BELOW IS A BUILD ERROR - see ExhaustiveSwitch.h for why it would not be otherwise.
// ENFORCED BY: C4062 as an error, AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (checked 2026-09-30 by a stray enumerator: the build failed here)
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
EFlightPhase FlightPhaseFromTransition(const FAgentTransition& Transition, EFlightPhase Current, bool bParkedAtStand)
{
	// A TAXI THAT GOES ON goes the way it was going: Turnaround or later means the taxi OUT, anything earlier the taxi
	// in. EAgentPhase::Taxiing happens twice and the agent cannot tell the two apart - only the flight can - which is
	// why this takes Current at all. The comparison reads EFlightPhase's declaration order, which its own comment pins.
	const EFlightPhase TaxiGoesOn = Current >= EFlightPhase::Turnaround ? EFlightPhase::TaxiOut : EFlightPhase::TaxiIn;

	// EVERY CAUSE BY NAME, NO default (#436): the default of FlightPhaseFromAgent's switch on To is where a new pair
	// hid - a cause added to EAgentEvent lands here as a BUILD ERROR on a missing case (C4062, raised above), not as
	// a silent "moves nothing".
	switch (Transition.Cause)
	{
	case EAgentEvent::Dispatched:
		// BORN: an arrival is born Arriving - the landing; an aircraft dispatched straight onto a taxi (DispatchAgent)
		// is born Taxiing, on the taxi it is on.
		return Transition.To == EAgentPhase::Arriving ? EFlightPhase::Landing
			: Transition.To == EAgentPhase::Taxiing ? TaxiGoesOn : Current;

	case EAgentEvent::Vacated:
		return EFlightPhase::TaxiIn;

	case EAgentEvent::Parked:
		// #405: A TURNAROUND IS TIME ON A STAND. Parked on the fallback junction - or a Parked the agent has already
		// left (ReofferStands redirected it in the same frame) - the flight is still taxiing in, so the re-offer that
		// follows reads its taxi in rather than out. No parking clock, no turnaround: UJobBoard::OnAgentPhase opens none
		// there either, by the same StandAtNode. bParkedAtStand is the EVENT's goal - the node it parked on - since
		// #436; it used to be the live agent's, asked a drain late, and was right only because the agent was asked
		// whether it was STILL parked first. (Moved here from UFlightBoard::OnAgentPhase, which special-cased it.)
		// ENFORCED BY: AirportOps.Model.Bus.FallbackParkStaysTaxiIn, AirportOps.Model.Bus.SameFrameRedirectStaysTaxiIn
		return Current < EFlightPhase::Turnaround && !bParkedAtStand ? Current : EFlightPhase::Turnaround;

	case EAgentEvent::PushedBack:
		return EFlightPhase::TaxiOut;

	case EAgentEvent::DepartOrdered:
		// THE TAXI OUT, WHEREVER IT LEFT FROM: a push is Manoeuvring; driving straight out - from a stand, or from the
		// fallback junction that never saw a turnaround (UGroundTraffic::DepartAgent's RedirectAgent branch) - is the
		// taxi out. That second case used to need the live agent's bDepartureArmed (review M4): "never reached
		// Turnaround" otherwise read as the taxi in. THE POSITIVE FACT, not "its goal is no stand" (review M1): a
		// redirect whose new stand is deleted before the event is heard has no stand goal either, and is still taxiing
		// in - that one is ReOffered, below. (Moved here from UFlightBoard::OnAgentPhase.)
		// ENFORCED BY: AirportOps.Model.Bus.DepartFromFallbackReadsTaxiOut, AirportOps.Model.Bus.RedirectStaysTaxiInWhenItsStandGoes
		//
		// AND THE PUSH IS ITS OWN PHASE: without the Manoeuvring answer the flight would read "Turnaround" while the
		// aeroplane is visibly moving off its stand - the board and the apron disagreeing, with nothing to say which
		// was right. (Carried from FlightPhaseFromAgent's Manoeuvring case.)
		return Transition.To == EAgentPhase::Manoeuvring ? EFlightPhase::Manoeuvring
			: Transition.To == EAgentPhase::Taxiing ? EFlightPhase::TaxiOut : Current;

	case EAgentEvent::Redirected:
	case EAgentEvent::ReOffered:
	case EAgentEvent::Rescued:
	case EAgentEvent::BackedOut:
		// SENT ON, OR BACK ON ITS WAY: the taxi it was in, in the direction it was going. A ReOffered aeroplane is a
		// waiter for a stand, so that is its taxi in; a rescued one resumes the taxi it was stranded in.
		return Transition.To == EAgentPhase::Taxiing ? TaxiGoesOn : Current;

	case EAgentEvent::LinedUp:
		return EFlightPhase::Departing;

	case EAgentEvent::Gone:
		return EFlightPhase::Departed;

	case EAgentEvent::Retired:
	case EAgentEvent::Cleared:
		// NOT A DEPARTURE, and still booked as one, as the pair Gone always was: UFlightBoard::CancelByAgent is called
		// BEFORE a despawn's RetireAgent and unhooks the flight, so this never finds one; a load discards the queue
		// that holds a ClearAgents' events (UOpsRuntime::LoadFromSlot). A flight reaching here has lost its aeroplane
		// through a door that skipped the cancel - Departed at least takes it off the live list.
		// ENFORCED BY: AirportOps.Model.AgentRescue.AircraftDespawnCancelsFlight, AirportOps.Present.Bus.LoadDiscardsQueue
		return EFlightPhase::Departed;

	case EAgentEvent::Stranded:
		// NOWHERE, by name (issue #396): the aeroplane stopped short of its stand, so no turnaround has started, and
		// the flight reads the taxi it was in until the player retires the aeroplane. Turnaround here was the
		// stranded-at-the-stand bug.
		return Current;

	case EAgentEvent::BackingIn:
		// A SERVICE VEHICLE'S MANOEUVRE - no aeroplane reverses into anything; a flight that somehow heard it keeps its
		// phase rather than guess one.
		return Current;

	case EAgentEvent::Airborne:
	case EAgentEvent::TouchedDown:
		// MOMENTS, never announced as a phase change (UGroundTraffic::GetMomentsThisAdvance) - here only so the switch
		// names every cause.
		return Current;

	case EAgentEvent::None:
		// NOTHING NAMED IT - UGroundTraffic::Announce has already logged that as an Error. Unchanged rather than a
		// guess: a phase this cannot place must not move a flight backwards through states the inbox is showing.
		return Current;
	}
	return Current;
}
AIRSIDE_EXHAUSTIVE_SWITCH_END
