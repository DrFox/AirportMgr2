#include "Model/AgentRescue.h"

#include "AirportOpsLog.h"
#include "Model/ArrivalPlanner.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "Model/TrafficOccupancy.h"

#define LOCTEXT_NAMESPACE "AgentRescue"

namespace
{
	/**
	 * How far from a stranded aircraft the node its stand search starts from may be, uu. The search
	 * only CHOOSES the stand; the route driven is RescueStranded's, from the pavement it hops onto -
	 * so this is generous (50 m) rather than tight: too tight and a stranded aircraft finds no stand
	 * at all, too loose and the choice favours a stand near some other node. Neither moves anything.
	 */
	constexpr double StandSearchRadius = 5000.0;

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

bool UAgentRescue::LooksStuck(const FRoadAgent& Agent, double Seconds)
{
	return Agent.Phase == EAgentPhase::Stranded || Agent.GetStalledSeconds() >= Seconds;
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

FUnstickVerdict UAgentRescue::Replan(UGroundTraffic& Traffic, const URoadNetwork& Network, const FRoadAgent& Agent)
{
	if (Agent.Phase == EAgentPhase::Stranded)
	{
		return Traffic.RescueStranded(Agent.Id, Network, FGuidelineNodeId()) ? FUnstickVerdict::Yes()
			: FUnstickVerdict::No(LOCTEXT("NoPavement", "No pavement close enough to rejoin - despawn it"));
	}

	const FRoutePlan& Plan = Agent.Follower.Plan;
	const int32 OnStep = UGroundTraffic::CurrentStep(Plan, Agent.Follower.Travelled);
	const int32 Blocked = Agent.GetBlockedStep();

	// HELD AT A STEP IT HAS NOT ENTERED: splice there and ban what refused it - the deadlock resolver's
	// own ban (FDeadlockResolver::Resolve), a whole node when a node refused it, else the step's edge. Not
	// when it is already INSIDE the refused step: ReplanAt keeps Travelled, and another edge out of a node
	// behind it would re-map its distance onto other geometry - the teleport ReplanAt's precondition names.
	if (Blocked >= 0 && Blocked < Plan.Steps.Num()
		&& UGroundTraffic::StepStart(Plan, Blocked) - Agent.Follower.Travelled >= -KINDA_SMALL_NUMBER)
	{
		const FGuidelineEdgeId BannedEdge = Plan.Steps[Blocked].Edge;
		const FGuidelineNodeId BannedNode = Agent.GetBlockedResource().Kind == ETrafficResourceKind::Node
			? Agent.GetBlockedResource().Node : FGuidelineNodeId();
		if (Traffic.ReplanAt(Agent.Id, Network, Blocked, BannedEdge, BannedNode))
		{
			return FUnstickVerdict::Yes();
		}
		return FUnstickVerdict::No(LOCTEXT("NoWayRound", "No other way round what is holding it"));
	}

	// NOT HELD, OR HELD INSIDE ITS STEP: the next node ahead, no ban - a fresh search under today's
	// congestion. ReplanAt refuses a "replan" that finds the route it already has, which is the answer.
	const int32 Splice = OnStep + 1;
	if (Splice >= Plan.Steps.Num())
	{
		return FUnstickVerdict::No(LOCTEXT("LastStep", "On its last stretch - nothing left to replan"));
	}
	if (Traffic.ReplanAt(Agent.Id, Network, Splice, FGuidelineEdgeId()))
	{
		return FUnstickVerdict::Yes();
	}
	return FUnstickVerdict::No(LOCTEXT("BestRoute", "Already on its best route"));
}

FUnstickVerdict UAgentRescue::SendVehicleHome(UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock, int32 AgentId)
{
	// ALWAYS HOME, one way or the other: GoToFacility drives it there or, with no way, retires it where it
	// stands and puts it Idle at its depot. Either is what the player asked for.
	return JobBoard->RecallVehicleOfAgent(AgentId, /*bRetire=*/false, Traffic, Network, Clock)
		? FUnstickVerdict::Yes() : FUnstickVerdict::No(LOCTEXT("NoDepot", "Not a depot's vehicle - it has no home"));
}

FUnstickVerdict UAgentRescue::FindStand(UGroundTraffic& Traffic, const URoadNetwork& Network, const FRoadAgent& Agent)
{
	const FAirframe* Airframe = Agent.AsAircraft();
	if (Airframe == nullptr)
	{
		return FUnstickVerdict::No(LOCTEXT("NoAirframe", "Not an aircraft"));
	}

	// PARKED WAITING FOR A STAND: exactly UGroundTraffic::ReofferStands' move, asked now instead of when
	// a stand next frees - from the node it waits on, and a redirect from rest where it stands.
	if (Agent.Phase == EAgentPhase::Parked)
	{
		FRoutePlan Route;
		const FGuidelineNodeId Stand = ArrivalPlanner::ChooseStand(Network, Agent.GoalNode, *Airframe,
			&Traffic.GetOccupancy(), Agent.Id, &Route);
		if (!Stand.IsSet() || !Route.IsValid())
		{
			return FUnstickVerdict::No(LOCTEXT("NoFreeStand", "No free stand fits it"));
		}
		// RESCUED, the player's own reason (#436): the flight board keeps the taxi in it was in.
		return Traffic.RedirectAgent(Agent.Id, &Network, Route, EAgentEvent::Rescued) ? FUnstickVerdict::Yes()
			: FUnstickVerdict::No(LOCTEXT("RedirectRefused", "Could not send it to the stand"));
	}

	// STRANDED: the stand is CHOSEN from a node near it, and DRIVEN to by the rescue from wherever it
	// hops onto. THE NODE AHEAD ON ITS OWN ROUTE when that still exists - the way it was facing, off any
	// runway it was leaving - and only else the nearest node: measured 2026-09-29, an aeroplane stranded
	// just off a runway exit found the runway's own node nearest (198 uu), and a taxi-in search avoids
	// runway edges, so no stand was reachable from there at all. See StandSearchRadius.
	const FRoutePlan& Plan = Agent.Follower.Plan;
	const int32 OnStep = UGroundTraffic::CurrentStep(Plan, Agent.Follower.Travelled);
	FGuidelineNodeId Near = Plan.Steps.IsValidIndex(OnStep) && Network.GetGuidelineNode(Plan.Steps[OnStep].To) != nullptr
		? Plan.Steps[OnStep].To : FGuidelineNodeId();
	if (!Near.IsSet())
	{
		Near = RouteSearch::FindNearestNode(Network, Agent.LastMotion.Position, Agent.Class, StandSearchRadius);
	}
	if (!Near.IsSet())
	{
		return FUnstickVerdict::No(LOCTEXT("NoPavement", "No pavement close enough to rejoin - despawn it"));
	}
	bool bSawHeld = false;
	const FGuidelineNodeId Stand = ArrivalPlanner::ChooseStand(Network, Near, *Airframe,
		&Traffic.GetOccupancy(), Agent.Id, nullptr, &bSawHeld);
	if (!Stand.IsSet())
	{
		// WHICH NODE IT SEARCHED FROM, said: "no stand" from a stranded aeroplane is either the airport
		// (every stand held or too small) or the search origin, and only the log can tell them apart.
		const FGuidelineNode* From = Network.GetGuidelineNode(Near);
		UE_LOG(LogAirportOps, Log, TEXT("Unstick: agent %d found no stand searching from node %d, %.0f uu away (a held stand %s)"),
			Agent.Id, Near.Index, From != nullptr ? FVector2D::Distance(From->Position, Agent.LastMotion.Position) : -1.0,
			bSawHeld ? TEXT("was seen") : TEXT("was not seen"));
		return FUnstickVerdict::No(LOCTEXT("NoFreeStand", "No free stand fits it"));
	}
	return Traffic.RescueStranded(Agent.Id, Network, Stand) ? FUnstickVerdict::Yes()
		: FUnstickVerdict::No(LOCTEXT("NoPavement", "No pavement close enough to rejoin - despawn it"));
}

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
