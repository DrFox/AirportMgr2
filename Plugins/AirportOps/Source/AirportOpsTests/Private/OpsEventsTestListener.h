#pragma once

#include "CoreMinimal.h"
#include "Model/OpsEvents.h"
#include "Present/OpsRuntime.h"
#include "UObject/Object.h"
#include "OpsEventsTestListener.generated.h"

/**
 * A UObject listener for UOpsEvents, because dynamic delegates bind to UFUNCTIONs, not
 * lambdas. Records every event as a string so a test can assert on the exact sequence.
 * Shared by the Events test and the Runtime test.
 */
UCLASS()
class UOpsEventsTestListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<FString> Seen;

	UFUNCTION() void OnRefused(EArrivalRefusal Why) { Seen.Add(FString::Printf(TEXT("refused:%d"), static_cast<int32>(Why))); }
	/** "save:<outcome>:<slot>" - the case by its enumerator's name, so an assertion reads like the toast it stands for. */
	UFUNCTION() void OnSaveSlot(EOpsSaveOutcome Outcome, const FString& Slot)
	{
		Seen.Add(FString::Printf(TEXT("save:%s:%s"), *StaticEnum<EOpsSaveOutcome>()->GetNameStringByValue(static_cast<int64>(Outcome)), *Slot));
	}
	/** "buy:<kind>", and the whole purchase kept, so a test can assert the facts and the nouns the toast will word (#445). */
	UFUNCTION() void OnPurchase(const FOpsPurchase& Purchase)
	{
		Seen.Add(TEXT("buy:") + StaticEnum<EOpsPurchaseKind>()->GetNameStringByValue(static_cast<int64>(Purchase.Kind)));
		Purchases.Add(Purchase);
	}
	TArray<FOpsPurchase> Purchases;
	UFUNCTION() void OnAlertRaised(const FOpsAlert& Alert) { Seen.Add(TEXT("alert+:") + UEnum::GetValueAsString(Alert.Key.Kind)); }
	UFUNCTION() void OnAlertsReset() { Seen.Add(TEXT("reset")); }
	UFUNCTION() void OnAlertCleared(const FOpsAlertKey& Key) { Seen.Add(TEXT("alert-:") + UEnum::GetValueAsString(Key.Kind)); }
	UFUNCTION() void OnAlertChanged(const FOpsAlertKey& Key) { Seen.Add(TEXT("alert~:") + UEnum::GetValueAsString(Key.Kind)); }
	UFUNCTION() void OnBuildRefused(const FString& What, const FString& Price, const FString& Balance) { Seen.Add(TEXT("refused:") + What); }
	UFUNCTION() void OnLandRefused(EArrivalRefusal Why, const FString& Sentence)
	{
		Seen.Add(FString::Printf(TEXT("land:%d"), static_cast<int32>(Why)));
		LastLandSentence = Sentence;
	}

	/** The words the last land refusal carried - the toast shows these (#456 review). */
	FString LastLandSentence;

	int32 CountOf(const FString& Prefix) const
	{
		return Seen.FilterByPredicate([&Prefix](const FString& S) { return S.StartsWith(Prefix); }).Num();
	}

	/** A Blueprint-shaped autosave: a Presentation handler that saves when told to - for
	 *  AirportOps.Present.Bus.SaveFromAHandler, which needs a save made from INSIDE a drain. Cued by a save-slot event naming
	 *  the slot "autosave" (it was a notification reading "autosave" until #445 retired the catch-all); the save it makes
	 *  announces its own slot, which is not the cue, so it does not save again. */
	UPROPERTY() TObjectPtr<UOpsRuntime> SaveOnCue;
	FString SaveSlot;
	bool bSavedFromHandler = false;
	UFUNCTION() void OnSaveSlotSave(EOpsSaveOutcome Outcome, const FString& Slot)
	{
		if (SaveOnCue != nullptr && Slot == TEXT("autosave"))
		{
			bSavedFromHandler = SaveOnCue->SaveToSlot(SaveSlot);
		}
	}

	/** The load's twin (#445, AirportOps.Present.Bus.LoadFromAHandlerIsRefused): a Presentation handler that LOADS when told to. The
	 *  result is what LoadFromSlot answered from inside the drain - false is the guard. Cued by the slot "autoload". */
	UPROPERTY() TObjectPtr<UOpsRuntime> LoadOnCue;
	FString LoadSlot;
	bool bLoadAnswered = false;
	bool bLoadedFromHandler = false;
	UFUNCTION() void OnSaveSlotLoad(EOpsSaveOutcome Outcome, const FString& Slot)
	{
		if (LoadOnCue != nullptr && Slot == TEXT("autoload"))
		{
			bLoadAnswered = true;
			bLoadedFromHandler = LoadOnCue->LoadFromSlot(LoadSlot);
		}
	}
};
