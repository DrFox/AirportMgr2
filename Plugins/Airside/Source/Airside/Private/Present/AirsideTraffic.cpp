#include "Present/AirsideTraffic.h"

#include "AirsideLog.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadAgentActor.h"
#include "Solve/RunwayDesignator.h"

bool UAirsideTraffic::DispatchArrival(const URoadNetwork& Network, const FVector2D& Near,
	const FAirframe& Airframe, double SurfaceZ, double ShutdownPauseSeconds)
{
	// GetWorld() on a subobject walks Outer->GetWorld() by default (see UObject::GetWorld),
	// and this object's Outer is the actor that created it with CreateDefaultSubobject - so
	// this reaches the same world GetOuter()->GetWorld() would, without a second lookup.
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	// WHICH RUNWAY, WHICH EXIT, WHICH STAND - none of that needs a world, so issue #29 moved
	// it to Model/ArrivalPlanner. This is left with arming, spawning and logging the plan.
	const FArrivalPlan Plan = ArrivalPlanner::Plan(Network, Near, Airframe);

	// Reported whether or not this succeeds, because a refusal that does not say which of
	// these was the problem is a feature that "does nothing". Skipped only for NoRunway,
	// which found no runway at all - every other field here is meaningless until one is.
	if (Plan.Why != EArrivalRefusal::NoRunway)
	{
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Arrival: runway %s, %.0f uu long, %.0f needed to stop, %d usable exit(s), ")
			TEXT("%d stand(s) on the airport."),
			*RunwayDesignator::ToPairText(Plan.Direction), Plan.RunwayLength, Plan.Needed,
			Plan.ExitCount, Network.GetEntities().Num());
	}

	if (!Plan.IsValid())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("%s"), *ArrivalPlanner::DescribeRefusal(Plan));
		OnArrivalRefused.Broadcast(Plan.Why);
		return false;
	}

	FRoadAgent Agent;
	if (!Agent.StartArrival(Plan.Threshold, Plan.Direction, Plan.RunwayLength, Airframe, Plan.VacateAt, Plan.TaxiIn))
	{
		// FLandingRun has already logged why. Nothing spawns: an arrival that cannot be
		// flown must leave no aircraft in the world, rather than one frozen on final.
		return false;
	}

	// FRoadAgent is world-free and cannot read this actor's UPROPERTY, so the pause is copied in here.
	Agent.ShutdownPause = ShutdownPauseSeconds;
	FActorSpawnParameters Params;
	Params.Owner = GetTypedOuter<AActor>();
	Params.ObjectFlags |= RF_Transient;
	FAgentSlot Slot;
	Slot.View = World->SpawnActor<ARoadAgentActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (Slot.View == nullptr)
	{
		return false;
	}
	// The airframe MESH, not to be confused with the FAirframe performance struct above.
	if (const UAirsideContent* Content = UAirsideSettings::GetContent())
	{
		Slot.View->SetAirframe(Content->AgentMesh.LoadSynchronous(), Content->AgentAnimClass.LoadSynchronous());
	}

	// Posed before its first tick so it never appears at the origin - zero delta asks where it starts.
	FAgentMotion Motion;
	if (Agent.Advance(0.0, Motion))
	{
		Slot.View->SetMotion(Motion, SurfaceZ);
	}

	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Arrival on runway %s: %.0f uu available, %.0f needed, vacating at exit %d of %d, ")
		TEXT("taxiing %.0f uu to a stand."),
		*RunwayDesignator::ToPairText(Plan.Direction), Plan.RunwayLength, Plan.Needed,
		Plan.ExitOrdinal, Plan.ExitCount, Plan.TaxiIn.Length);

	Slot.Agent = MoveTemp(Agent);
	Admit(MoveTemp(Slot));
	return true;
}

bool UAirsideTraffic::DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan,
	const FAirframe& Airframe, double SurfaceZ, double ShutdownPauseSeconds)
{
	if (!Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	// Editor worlds included, deliberately. This first refused outside a game world on the
	// grounds that an editor world would SAVE the cubes - but they are spawned RF_Transient
	// below, so they were never going to be saved and the guard was protecting against
	// nothing. What it DID do was make the tool look broken in the one place the build
	// tools are actually used: the ed mode, where pressing 4 and clicking twice drew a
	// route and produced no cube, with nothing on screen to say why.
	//
	// See ARoadNetworkActor::ShouldTickIfViewportsOnly: an actor does not tick in an editor
	// world unless it asks to, so allowing the spawn without that would have left a cube
	// frozen at its start - which is a worse lie than no cube at all.

	FRoadAgent Agent;
	Agent.StartTaxi(Plan, Airframe);

	// FRoadAgent is world-free and cannot read this actor's UPROPERTY for itself, so the
	// pause is copied in at dispatch - the only time the two ever need to meet.
	Agent.ShutdownPause = ShutdownPauseSeconds;

	ArmDepartureIfRunway(Agent, Network, Plan);

	FActorSpawnParameters Params;
	Params.Owner = GetTypedOuter<AActor>();
	Params.ObjectFlags |= RF_Transient;

	FAgentSlot Slot;
	Slot.View = World->SpawnActor<ARoadAgentActor>(
		FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (Slot.View == nullptr)
	{
		return false;
	}

	// The airframe is pushed in, like the pose. A view that fetched its own mesh by path was
	// how a content move turned every aircraft into a cube - see ARoadAgentActor::SetAirframe.
	if (const UAirsideContent* Content = UAirsideSettings::GetContent())
	{
		Slot.View->SetAirframe(Content->AgentMesh.LoadSynchronous(),
			Content->AgentAnimClass.LoadSynchronous());
	}

	// Posed before the first tick, so it appears at its start rather than at the origin for
	// one frame. Zero delta asks Advance for where the taxi starts without moving it; the
	// fallback FRoadAgent::StartTaxi left in LastMotion covers a plan too short to advance.
	{
		FAgentMotion Motion;
		if (Agent.Advance(0.0, Motion))
		{
			Slot.View->SetMotion(Motion, SurfaceZ);
		}
	}

	Slot.Agent = MoveTemp(Agent);
	Admit(MoveTemp(Slot));
	return true;
}

void UAirsideTraffic::ArmDepartureIfRunway(FRoadAgent& Agent, const URoadNetwork* Network, const FRoutePlan& Plan) const
{
	// DOES THIS ROUTE END ON A RUNWAY? Asked here rather than by the tool, because the answer
	// is a fact about the network and the last polyline point is the only thing that knows
	// where the route actually finished. A route that ends anywhere else simply taxis, which
	// is what every route did before departures existed.
	if (Network == nullptr || Plan.Polyline.Num() == 0 || !Agent.Airframe.Climb.IsSet())
	{
		return;
	}
	FVector2D Threshold;
	FVector2D Direction;
	double Length = 0.0;
	if (Network->RunwayExtentAt(Plan.Polyline.Last(), Threshold, Direction, Length))
	{
		Agent.ArmDeparture(Threshold, Direction, Length);

		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Route ends on runway %s: %.0f uu available, departure armed"),
			*RunwayDesignator::ToPairText(Direction), Length);
	}
}

void UAirsideTraffic::Admit(FAgentSlot&& Slot)
{
	Slot.Id = NextAgentId++;
	const EAgentPhase Born = Slot.Agent.Phase;
	const int32 Id = Slot.Id;
	Agents.Add(MoveTemp(Slot));
	OnAgentPhaseChanged.Broadcast(Id, EAgentPhase::Gone, Born);
}

int32 UAirsideTraffic::FindSlot(int32 AgentId) const
{
	return Agents.IndexOfByPredicate([AgentId](const FAgentSlot& S) { return S.Id == AgentId; });
}

bool UAirsideTraffic::RedirectAgent(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Plan)
{
	const int32 Index = FindSlot(AgentId);
	if (Index == INDEX_NONE || !Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		return false;
	}
	FAgentSlot& Slot = Agents[Index];
	const EAgentPhase Before = Slot.Agent.Phase;
	if (Before != EAgentPhase::Parked && Before != EAgentPhase::Taxiing)
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("RedirectAgent %d refused: agent is %s"),
			AgentId, *UEnum::GetValueAsString(Before));
		return false;
	}

	// The airframe is the agent's own - a redirect changes where it goes, not what it is.
	// Copied out first: StartTaxi assigns Airframe from its argument, and handing it a
	// reference to the very field it overwrites is a self-assignment nobody should rely on.
	const FAirframe Own = Slot.Agent.Airframe;
	Slot.Agent.StartTaxi(Plan, Own);
	ArmDepartureIfRunway(Slot.Agent, Network, Plan);

	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d redirected: %.0f uu"), AgentId, Plan.Length);
	if (Slot.Agent.Phase != Before)
	{
		OnAgentPhaseChanged.Broadcast(AgentId, Before, Slot.Agent.Phase);
	}
	return true;
}

bool UAirsideTraffic::RetireAgent(int32 AgentId)
{
	const int32 Index = FindSlot(AgentId);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	const EAgentPhase Before = Agents[Index].Agent.Phase;
	if (Agents[Index].View != nullptr)
	{
		Agents[Index].View->Destroy();
	}
	Agents.RemoveAt(Index);
	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d retired"), AgentId);
	OnAgentPhaseChanged.Broadcast(AgentId, Before, EAgentPhase::Gone);
	return true;
}

void UAirsideTraffic::ClearAgents()
{
	for (FAgentSlot& Slot : Agents)
	{
		if (Slot.View != nullptr)
		{
			Slot.View->Destroy();
		}
	}

	// Announced before Reset so a listener asking about the id still gets To == Gone for
	// every agent it was tracking, the same promise Advance's removal path makes.
	for (const FAgentSlot& Slot : Agents)
	{
		OnAgentPhaseChanged.Broadcast(Slot.Id, Slot.Agent.Phase, EAgentPhase::Gone);
	}

	Agents.Reset();
}

void UAirsideTraffic::Advance(float DeltaSeconds, double SurfaceZ)
{
	// Every handover (arrive -> taxi -> depart -> gone, or arrive -> taxi -> park) is owned
	// by FRoadAgent::Advance now - see its own comment. This loop is left with exactly one
	// job: hand the model's answer to the view, and drop an agent once it says Gone.
	for (int32 Index = Agents.Num() - 1; Index >= 0; --Index)
	{
		FAgentSlot& Slot = Agents[Index];
		const EAgentPhase Before = Slot.Agent.Phase;
		const int32 Id = Slot.Id;

		FAgentMotion Motion;
		if (!Slot.Agent.Advance(DeltaSeconds, Motion))
		{
			// Cleared or otherwise finished - the aircraft has gone, so the actor goes with
			// it. An agent that stayed in the world would accumulate one per departure,
			// hanging above the airport for ever.
			if (Slot.View != nullptr)
			{
				Slot.View->Destroy();
			}
			Agents.RemoveAt(Index);
			// Broadcast AFTER the removal so a listener that asks GetAgentCount sees the
			// agent already gone, which is what "To == Gone" promises.
			OnAgentPhaseChanged.Broadcast(Id, Before, EAgentPhase::Gone);
			continue;
		}

		if (Slot.View != nullptr)
		{
			Slot.View->SetMotion(Motion, SurfaceZ);
		}

		if (Slot.Agent.Phase != Before)
		{
			OnAgentPhaseChanged.Broadcast(Id, Before, Slot.Agent.Phase);
		}
	}
}
