#include "Model/OpsEvents.h"
#include "AirportOpsLog.h"

void UOpsEvents::NotifyArrivalRefused(EArrivalRefusal Why)
{
	UE_LOG(LogAirportOps, Log, TEXT("Arrival refused: %s"), *UEnum::GetValueAsString(Why));
	OnArrivalRefused.Broadcast(Why);
}

void UOpsEvents::NotifySaveSlot(EOpsSaveOutcome Outcome, const FString& Slot)
{
	// THE CASE AND THE SLOT, where "Notification: <sentence>" used to be (#445 item 7): the sentence is the toast's now, and
	// the log keeps what the runtime decided. A failure is logged as Log, not Warning - OpsSave::WriteSlot and ReadSlot log
	// their own failures at Warning or above (WriteSlot's two since #499's review); this records what the player was told.
	// "Toast: " FIRST, so these lines grep apart from the model's own (a depot's "Purchase: ...", OpsSave's "Save to slot").
	UE_LOG(LogAirportOps, Log, TEXT("Toast: save slot '%s' %s"), *Slot, *UEnum::GetValueAsString(Outcome));
	OnSaveSlot.Broadcast(Outcome, Slot);
}

void UOpsEvents::NotifyPurchase(const FOpsPurchase& Purchase)
{
	// LOG, NOT WARNING, for a refund too: whoever published it has already written its own Warning with the ids in it (the
	// repair's RemoveUnseated); this one records only what the player was told, as "Notification (warning): ..." did.
	UE_LOG(LogAirportOps, Log, TEXT("Toast: purchase %s, %d x %s, %s"), *UEnum::GetValueAsString(Purchase.Kind), Purchase.Count,
		*Purchase.Name.ToString(), *Purchase.Money.ToString());
	OnPurchase.Broadcast(Purchase);
}
