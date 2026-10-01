#include "Model/OpsEvents.h"
#include "AirportOpsLog.h"

void UOpsEvents::NotifyArrivalRefused(EArrivalRefusal Why)
{
	UE_LOG(LogAirportOps, Log, TEXT("Arrival refused: %s"), *UEnum::GetValueAsString(Why));
	OnArrivalRefused.Broadcast(Why);
}

void UOpsEvents::NotifyNotification(const FString& Text)
{
	UE_LOG(LogAirportOps, Log, TEXT("Notification: %s"), *Text);
	OnNotification.Broadcast(Text);
}

void UOpsEvents::NotifyWarning(const FString& Text)
{
	// LOG, NOT WARNING, for the line itself: whoever published this has already written its own Warning with the ids in
	// it; this one records only what the player was told.
	UE_LOG(LogAirportOps, Log, TEXT("Notification (warning): %s"), *Text);
	OnWarning.Broadcast(Text);
}
