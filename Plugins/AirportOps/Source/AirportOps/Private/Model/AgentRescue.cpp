#include "Model/AgentRescue.h"

#include "AirportOpsLog.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"

#define LOCTEXT_NAMESPACE "AgentRescue"

namespace
{
	// THE STAND SEARCH'S RADIUS (StandSearchRadius) went to Airside with the search itself (#429): it is
	// UGroundTraffic::RescueToStand's figure now, beside the reason it is 50 m.

	FText PhaseRefusal(EAgentPhase Phase)
	{
		switch (Phase)
		{
		case EAgentPhase::Arriving:    return LOCTEXT("Arriving", "On the runway - the landing owns it");
		case EAgentPhase::Departing:   return LOCTEXT("Departing", "Taking off - the runway owns it");
		case EAgentPhase::Manoeuvring: return LOCTEXT("Manoeuvring", "Coming off its stand - wait for it to finish");
		case EAgentPhase::Reversing:   return LOCTEXT("Reversing", "Reversing - wait for it to finish");
		case EAgentPhase::Gone:        return LOCTEXT("Gone", "Already gone");
		default:                       return LOCTEXT("NotNow", "Not in a state to do that");
		}
	}
}

FUnstickVerdict UAgentRescue::CanUnstick(const UGroundTraffic& Traffic, int32 AgentId, EUnstickAction Action) const
{
	const FRoadAgent* Agent = Traffic.FindAgent(AgentId);
	if (Agent == nullptr)
	{
		return FUnstickVerdict::No(LOCTEXT("NoAgent", "Nothing selected"));
	}
	return Decide(*Agent, Action);
}

FUnstickVerdict UAgentRescue::Decide(const FRoadAgent& Agent, EUnstickAction Action) const
{
	const bool bVehicle = Agent.AsVehicle() != nullptr;
	switch (Action)
	{
	case EUnstickAction::Despawn:
		// ALWAYS: it is the hatch of last resort, and an aeroplane stuck on final approach is exactly
		// the case nothing else reaches (spec ruling, 2026-09-29).
		return FUnstickVerdict::Yes();

	case EUnstickAction::Replan:
		if (Agent.Phase == EAgentPhase::Taxiing || Agent.Phase == EAgentPhase::Stranded)
		{
			return FUnstickVerdict::Yes();
		}
		if (Agent.Phase == EAgentPhase::Parked)
		{
			return FUnstickVerdict::No(bVehicle ? LOCTEXT("ParkedVehicle", "Parked - it is not going anywhere to replan")
				: LOCTEXT("ParkedAircraft", "Parked - use Depart to send it"));
		}
		return FUnstickVerdict::No(PhaseRefusal(Agent.Phase));

	case EUnstickAction::SendHome:
		if (bVehicle)
		{
			if (JobBoard == nullptr || JobBoard->VehicleForAgent(Agent.Id) == nullptr)
			{
				return FUnstickVerdict::No(LOCTEXT("NoDepot", "Not a depot's vehicle - it has no home"));
			}
			// EVERY GROUND PHASE: DriveVehicleTo owns how each one turns for home (on the road, finishing
			// a reverse, parked, stranded).
			return FUnstickVerdict::Yes();
		}
		if (Agent.Phase == EAgentPhase::Stranded || (Agent.Phase == EAgentPhase::Parked && Agent.bAwaitingStand))
		{
			return FUnstickVerdict::Yes();
		}
		if (Agent.Phase == EAgentPhase::Taxiing)
		{
			// OUT OF SCOPE, said: re-seeking a stand for a MOVING aircraft needs an aircraft-safe
			// RerouteAgent (RedirectAgent restarts it at the plan's first point). Replan covers an
			// unreachable stand when any route exists at all.
			return FUnstickVerdict::No(LOCTEXT("MovingAircraft", "Moving - use Replan"));
		}
		if (Agent.Phase == EAgentPhase::Parked)
		{
			return FUnstickVerdict::No(LOCTEXT("OnStand", "Already parked"));
		}
		return FUnstickVerdict::No(PhaseRefusal(Agent.Phase));
	}
	return FUnstickVerdict::No(FText::GetEmpty());
}

FUnstickVerdict UAgentRescue::Unstick(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock,
	int32 AgentId, EUnstickAction Action)
{
	const FRoadAgent* Found = Traffic.FindAgent(AgentId);
	if (Found == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Unstick: agent %d %s -> refused: no such agent"),
			AgentId, *UEnum::GetValueAsString(Action));
		return FUnstickVerdict::No(LOCTEXT("NoAgent", "Nothing selected"));
	}
	// COPIED, NOT HELD: every action below can broadcast a phase change, and a listener can retire the
	// agent and shift the array this reference points into (see UGroundTraffic::ReleaseGoal's comment).
	const FRoadAgent Agent = *Found;
	const FString Body = Agent.AsVehicle() != nullptr ? TEXT("Vehicle") : TEXT("Aircraft");
	const FString Phase = UEnum::GetValueAsString(Agent.Phase);

	FUnstickVerdict Verdict = Decide(Agent, Action);
	if (Verdict.bAllowed)
	{
		switch (Action)
		{
		case EUnstickAction::Replan:
			Verdict = Replan(Traffic, Network, Agent);
			break;
		case EUnstickAction::SendHome:
			Verdict = Agent.AsVehicle() != nullptr ? SendVehicleHome(Traffic, Network, Clock, AgentId)
				: FindStand(Traffic, Network, Agent);
			break;
		case EUnstickAction::Despawn:
			Verdict = Despawn(Traffic, Network, Clock, Agent);
			break;
		}
	}

	if (Verdict.bAllowed)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Unstick: agent %d (%s, %s) %s -> done"),
			AgentId, *Body, *Phase, *UEnum::GetValueAsString(Action));
	}
	else
	{
		UE_LOG(LogAirportOps, Log, TEXT("Unstick: agent %d (%s, %s) %s -> refused: %s"),
			AgentId, *Body, *Phase, *UEnum::GetValueAsString(Action), *Verdict.Why.ToString());
	}
	return Verdict;
}

// ENFORCED BY: AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (C4062 as an error over this function: a replan outcome added in Airside
// is a build error here until the player has a sentence for it)
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
FUnstickVerdict UAgentRescue::Replan(UGroundTraffic& Traffic, const URoadNetwork& Network, const FRoadAgent& Agent)
{
	if (Agent.Phase == EAgentPhase::Stranded)
	{
		return Traffic.RescueStranded(Agent.Id, Network, FGuidelineNodeId()) ? FUnstickVerdict::Yes()
			: FUnstickVerdict::No(LOCTEXT("NoPavement", "No pavement close enough to rejoin - despawn it"));
	}

	// HELD WHERE THE DEADLOCK RESOLVER WOULD TURN IT: the resolver's own step - its bound and its ban, a whole node when
	// a node refused it, else the step's edge (UGroundTraffic::ReplanAroundBlocker, #429). This used to be a copy of
	// that ban with no upper bound, and turned an agent a whole edge short of its block round it from a node it was
	// nowhere near.
	// ENFORCED BY: AirportOps.Model.AgentRescue.ReplanHonoursTheResolverBound
	switch (Traffic.ReplanAroundBlocker(Agent.Id, Network))
	{
	case EBlockerReplan::Turned:
		return FUnstickVerdict::Yes();
	case EBlockerReplan::NoWayRound:
		return FUnstickVerdict::No(LOCTEXT("NoWayRound", "No other way round what is holding it"));
	case EBlockerReplan::NotAtItsBlock:
		break;
	}

	// NOT HELD, OR NOT WHERE THE RESOLVER WOULD TURN IT: the next node ahead, no ban - a fresh search under today's
	// congestion (UGroundTraffic::ReplanFromNextNode). A "replan" that finds the route it already has is refused,
	// which is the answer.
	switch (Traffic.ReplanFromNextNode(Agent.Id, Network))
	{
	case ENextNodeReplan::Replanned:
		return FUnstickVerdict::Yes();
	case ENextNodeReplan::LastStep:
		return FUnstickVerdict::No(LOCTEXT("LastStep", "On its last stretch - nothing left to replan"));
	case ENextNodeReplan::NoBetterRoute:
		return FUnstickVerdict::No(LOCTEXT("BestRoute", "Already on its best route"));
	}
	return FUnstickVerdict::No(LOCTEXT("BestRoute", "Already on its best route"));
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

FUnstickVerdict UAgentRescue::SendVehicleHome(UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock, int32 AgentId)
{
	// ALWAYS HOME, one way or the other: GoToFacility drives it there or, with no way, retires it where it
	// stands and puts it Idle at its depot. Either is what the player asked for.
	return JobBoard->RecallVehicleOfAgent(AgentId, /*bRetire=*/false, Traffic, Network, Clock)
		? FUnstickVerdict::Yes() : FUnstickVerdict::No(LOCTEXT("NoDepot", "Not a depot's vehicle - it has no home"));
}

// ENFORCED BY: AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (C4062 as an error over this function: a stand-offer outcome added in
// Airside is a build error here until the player has a sentence for it)
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
FUnstickVerdict UAgentRescue::FindStand(UGroundTraffic& Traffic, const URoadNetwork& Network, const FRoadAgent& Agent)
{
	if (Agent.AsAircraft() == nullptr)
	{
		return FUnstickVerdict::No(LOCTEXT("NoAirframe", "Not an aircraft"));
	}

	// PARKED WAITING FOR A STAND: exactly UGroundTraffic::ReofferStands' move, asked now instead of when a stand next
	// frees - ReofferStand, the one waiter's move (#429): from the node it waits on, and a redirect from rest where it
	// stands. RESCUED, the player's own reason (#436): the flight board keeps the taxi in it was in.
	//
	// STRANDED: UGroundTraffic::RescueToStand - the stand CHOSEN from a node near it (the node ahead on its dead route,
	// else the nearest) and DRIVEN to by the rescue from wherever it hops onto. Not ReofferStand's restart from its
	// goal node, which for a stranded aeroplane may be the stand that went.
	const FStandOffer Offer = Agent.Phase == EAgentPhase::Parked
		? Traffic.ReofferStand(Agent.Id, Network, EAgentEvent::Rescued)
		: Traffic.RescueToStand(Agent.Id, Network);
	switch (Offer.Outcome)
	{
	case EStandOffer::Sent:
		return FUnstickVerdict::Yes();
	case EStandOffer::NoFreeStand:
		return FUnstickVerdict::No(LOCTEXT("NoFreeStand", "No free stand fits it"));
	case EStandOffer::NoPavement:
		return FUnstickVerdict::No(LOCTEXT("NoPavement", "No pavement close enough to rejoin - despawn it"));
	case EStandOffer::NotSent:
		return FUnstickVerdict::No(LOCTEXT("RedirectRefused", "Could not send it to the stand"));
	}
	return FUnstickVerdict::No(LOCTEXT("RedirectRefused", "Could not send it to the stand"));
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

FUnstickVerdict UAgentRescue::Despawn(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock,
	const FRoadAgent& Agent)
{
	if (Agent.AsVehicle() != nullptr && JobBoard != nullptr
		&& JobBoard->RecallVehicleOfAgent(Agent.Id, /*bRetire=*/true, Traffic, Network, Clock))
	{
		return FUnstickVerdict::Yes();
	}
	// THE FLIGHT FIRST - see UFlightBoard::CancelByAgent: after the retire, Gone would book it Departed.
	if (Agent.AsAircraft() != nullptr && FlightBoard != nullptr)
	{
		FlightBoard->CancelByAgent(Agent.Id, Clock.Now());
	}
	return Traffic.RetireAgent(Agent.Id) ? FUnstickVerdict::Yes()
		: FUnstickVerdict::No(LOCTEXT("NoAgent", "Nothing selected"));
}

#undef LOCTEXT_NAMESPACE
