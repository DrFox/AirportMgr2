#include "Model/OpsEvents.h"
#include "AirportOpsLog.h"

void UOpsEvents::NotifyAgentPhaseChanged(int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	UE_LOG(LogAirportOps, Verbose, TEXT("Agent %d: %s -> %s"), AgentId,
		*UEnum::GetValueAsString(From), *UEnum::GetValueAsString(To));
	OnAgentPhaseChanged.Broadcast(AgentId, From, To);
}

void UOpsEvents::NotifyArrivalRefused(EArrivalRefusal Why)
{
	UE_LOG(LogAirportOps, Log, TEXT("Arrival refused: %s"), *UEnum::GetValueAsString(Why));
	OnArrivalRefused.Broadcast(Why);
}

void UOpsEvents::NotifySpeedChanged(ESimSpeed Speed)
{
	OnSpeedChanged.Broadcast(Speed);
}

void UOpsEvents::NotifyNotification(const FString& Text)
{
	UE_LOG(LogAirportOps, Log, TEXT("Notification: %s"), *Text);
	OnNotification.Broadcast(Text);
}
