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

	UFUNCTION() void OnPhase(int32 AgentId, EAgentPhase From, EAgentPhase To)
	{
		Seen.Add(FString::Printf(TEXT("phase:%d:%d->%d"), AgentId, static_cast<int32>(From), static_cast<int32>(To)));
	}
	UFUNCTION() void OnRefused(EArrivalRefusal Why) { Seen.Add(FString::Printf(TEXT("refused:%d"), static_cast<int32>(Why))); }
	UFUNCTION() void OnSpeed(ESimSpeed Speed) { Seen.Add(FString::Printf(TEXT("speed:%d"), static_cast<int32>(Speed))); }
	UFUNCTION() void OnNote(const FString& Text) { Seen.Add(TEXT("note:") + Text); }
	UFUNCTION() void OnAlertRaised(const FOpsAlert& Alert) { Seen.Add(TEXT("alert+:") + UEnum::GetValueAsString(Alert.Key.Kind)); }
	UFUNCTION() void OnAlertsReset() { Seen.Add(TEXT("reset")); }
	UFUNCTION() void OnAlertCleared(const FOpsAlertKey& Key) { Seen.Add(TEXT("alert-:") + UEnum::GetValueAsString(Key.Kind)); }
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
	 *  AirportOps.Present.Bus.SaveFromAHandler, which needs a save made from INSIDE a drain. */
	UPROPERTY() TObjectPtr<UOpsRuntime> SaveOnNote;
	FString SaveSlot;
	bool bSavedFromHandler = false;
	UFUNCTION() void OnNoteSave(const FString& Text)
	{
		if (SaveOnNote != nullptr && Text == TEXT("autosave"))
		{
			bSavedFromHandler = SaveOnNote->SaveToSlot(SaveSlot);
		}
	}
};
