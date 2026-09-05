#pragma once

#include "CoreMinimal.h"
#include "Model/OpsEvents.h"
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
};
