#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadAgent.h"
#include "Model/SimClock.h"
#include "UObject/Object.h"
#include "OpsEvents.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOpsAgentPhaseChanged, int32, AgentId, EAgentPhase, From, EAgentPhase, To);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsArrivalRefused, EArrivalRefusal, Why);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsSpeedChanged, ESimSpeed, Speed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsNotification, const FString&, Text);

/**
 * The outcome bus. Pattern: Observer, via DYNAMIC multicast delegates so UMG and Blueprint
 * can bind without C++.
 *
 * Model code publishes through the Notify* functions and never knows who listens. This is
 * NOT the job pub/sub from the GDD - job assignment is request-response between the board
 * and depots (spec §3.5) and never goes through here. The bus announces what happened.
 *
 * Every Notify also writes a UE_LOG line: the log is this project's primary diagnostic
 * (CLAUDE.md "Diagnosing"), and an event nobody was bound to is otherwise invisible.
 *
 * Only events with a PUBLISHER in this milestone exist here. Flight, job, ledger and
 * contract events arrive with the systems that raise them; declaring them now would be a
 * list nothing consumes, which is the bug CLAUDE.md names three times.
 */
UCLASS(BlueprintType)
class AIRPORTOPS_API UOpsEvents : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable) FOpsAgentPhaseChanged OnAgentPhaseChanged;
	UPROPERTY(BlueprintAssignable) FOpsArrivalRefused    OnArrivalRefused;
	UPROPERTY(BlueprintAssignable) FOpsSpeedChanged      OnSpeedChanged;
	UPROPERTY(BlueprintAssignable) FOpsNotification      OnNotification;

	void NotifyAgentPhaseChanged(int32 AgentId, EAgentPhase From, EAgentPhase To);
	void NotifyArrivalRefused(EArrivalRefusal Why);
	void NotifySpeedChanged(ESimSpeed Speed);
	void NotifyNotification(const FString& Text);
};
