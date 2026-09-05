#include "Present/OpsRuntime.h"
#include "AirportOpsLog.h"
#include "Model/OpsEvents.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadEditHistory.h"

UOpsRuntime::UOpsRuntime()
{
	Clock = CreateDefaultSubobject<USimClock>(TEXT("Clock"));
	Events = CreateDefaultSubobject<UOpsEvents>(TEXT("Events"));
}

void UOpsRuntime::Attach(ARoadNetworkActor* Actor)
{
	Detach();
	Target = Actor;
	if (Target == nullptr || Target->GetTraffic() == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("OpsRuntime attached to nothing: no ARoadNetworkActor"));
		return;
	}
	UAirsideTraffic* Traffic = Target->GetTraffic();
	PhaseHandle = Traffic->OnAgentPhaseChanged.AddUObject(this, &UOpsRuntime::OnAgentPhase);
	RefusalHandle = Traffic->OnArrivalRefused.AddUObject(this, &UOpsRuntime::OnArrivalRefused);
	ApplySpeed(Clock->GetSpeed());
	UE_LOG(LogAirportOps, Log, TEXT("OpsRuntime attached to %s"), *Target->GetName());
}

void UOpsRuntime::Detach()
{
	if (Target != nullptr && Target->GetTraffic() != nullptr)
	{
		Target->GetTraffic()->OnAgentPhaseChanged.Remove(PhaseHandle);
		Target->GetTraffic()->OnArrivalRefused.Remove(RefusalHandle);
	}
	Target = nullptr;
}

void UOpsRuntime::Tick(double RealDeltaSeconds)
{
	Clock->Advance(RealDeltaSeconds);
	if (Target != nullptr)
	{
		// The MULTIPLIER, not TimeScale(): movement runs at the player's speed setting,
		// never at the day compression. See USimClock's class comment.
		Target->SetSimTimeScale(USimClock::Multiplier(Clock->GetSpeed()));
	}
}

void UOpsRuntime::ApplySpeed(ESimSpeed Speed)
{
	Clock->SetSpeed(Speed);
	if (Target != nullptr)
	{
		Target->SetSimTimeScale(USimClock::Multiplier(Speed));
	}
	Events->NotifySpeedChanged(Speed);
}

void UOpsRuntime::StepSpeed(int32 Delta)
{
	static const ESimSpeed Ladder[] = { ESimSpeed::X1, ESimSpeed::X2, ESimSpeed::X4, ESimSpeed::X8 };
	constexpr int32 Rungs = UE_ARRAY_COUNT(Ladder);

	// Stepping while paused steps from ResumeSpeed, which is what a player pressing
	// "faster" while paused means: resume, one notch up from where they were.
	const ESimSpeed From = Clock->GetSpeed() == ESimSpeed::Paused ? ResumeSpeed : Clock->GetSpeed();
	int32 Index = 0;
	for (int32 I = 0; I < Rungs; ++I)
	{
		if (Ladder[I] == From) { Index = I; }
	}
	Index = FMath::Clamp(Index + Delta, 0, Rungs - 1);
	ResumeSpeed = Ladder[Index];
	ApplySpeed(ResumeSpeed);
}

void UOpsRuntime::TogglePause()
{
	if (Clock->GetSpeed() == ESimSpeed::Paused)
	{
		ApplySpeed(ResumeSpeed);
	}
	else
	{
		ResumeSpeed = Clock->GetSpeed();
		ApplySpeed(ESimSpeed::Paused);
	}
}

void UOpsRuntime::OnAgentPhase(int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	Events->NotifyAgentPhaseChanged(AgentId, From, To);
}

void UOpsRuntime::OnArrivalRefused(EArrivalRefusal Why)
{
	Events->NotifyArrivalRefused(Why);
}

bool UOpsRuntime::SaveToSlot(const FString& SlotName)
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Save refused: no network attached"));
		return false;
	}
	FOpsSnapshot Snapshot;
	OpsSave::Capture(*Clock, *Target->Network, Snapshot);
	const bool bOk = OpsSave::WriteSlot(SlotName, Snapshot);
	Events->NotifyNotification(bOk ? FString::Printf(TEXT("Saved '%s'"), *SlotName)
	                               : FString::Printf(TEXT("Save to '%s' failed"), *SlotName));
	return bOk;
}

bool UOpsRuntime::LoadFromSlot(const FString& SlotName)
{
	// Target->Network is read HERE, not cached: URoadEditFacade::ClearNetwork replaces the
	// actor's network OBJECT rather than draining it, so a pointer held across a clear is
	// stale. The actor's field is the one authority on which network is current.
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Load refused: no network attached"));
		return false;
	}
	FOpsSnapshot Snapshot;
	if (!OpsSave::ReadSlot(SlotName, Snapshot))
	{
		Events->NotifyNotification(FString::Printf(TEXT("No save '%s'"), *SlotName));
		return false;
	}
	// Agents first: they were never saved, and one mid-taxi on a network about to be
	// replaced would be following a polyline through pavement that no longer exists.
	Target->GetTraffic()->ClearAgents();
	if (!OpsSave::Restore(Snapshot, *Clock, *Target->Network))
	{
		return false;
	}
	// The undo stack holds Mementos of the PRE-load network; an undo now would revert the
	// player to an airport they just replaced on purpose. A load is a new baseline.
	if (Target->History != nullptr)
	{
		Target->History->Clear();
	}
	// Present rebuilds from model: the mesh and the derived guideline graph are both
	// produced by the presenter's Rebuild, which is what RebuildMesh runs.
	Target->RebuildMesh();
	ApplySpeed(Clock->GetSpeed());
	Events->NotifyNotification(FString::Printf(TEXT("Loaded '%s'"), *SlotName));
	return true;
}
