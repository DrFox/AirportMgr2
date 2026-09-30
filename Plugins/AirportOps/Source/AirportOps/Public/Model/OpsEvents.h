#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/OpsAlerts.h"
#include "Model/RoadAgent.h"
#include "Model/SimClock.h"
#include "UObject/Object.h"
#include "OpsEvents.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOpsAgentPhaseChanged, int32, AgentId, EAgentPhase, From, EAgentPhase, To);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsArrivalRefused, EArrivalRefusal, Why);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsSpeedChanged, ESimSpeed, Speed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsNotification, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsAlertRaised, const FOpsAlert&, Alert);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsAlertCleared, const FOpsAlertKey&, Key);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOpsAlertsReset);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOpsBuildRefused, const FString&, What, const FString&, Price, const FString&, Balance);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOpsLandRefused, EArrivalRefusal, Why, const FString&, Sentence);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsBalanceSignChanged, bool, bOverdrawn);

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
 *
 * NOT EVERY CHANGE BELONGS HERE, though - see UFlightBoard::Revision's own comment for the
 * one deliberate exception: a coarse "something changed, re-read me" counter a C++ viewmodel
 * polls off of is not an outcome, and putting it here would be a discrete-events bus growing
 * a member that carries no information about what happened.
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

	/**
	 * A line for the toast stack that the player may want to act on - OnNotification's words at the Warning severity
	 * (#266: unplaced modules removed and refunded). A DELEGATE OF ITS OWN rather than a severity on OnNotification: the
	 * severity enum is the game module's (ENotificationSeverity), which AirportOps may not see.
	 */
	UPROPERTY(BlueprintAssignable) FOpsNotification      OnWarning;

	/** A standing problem started / stopped (spec 2026-09-29-ops-alerts). The toast and the alert window. */
	UPROPERTY(BlueprintAssignable) FOpsAlertRaised       OnAlertRaised;
	UPROPERTY(BlueprintAssignable) FOpsAlertCleared      OnAlertCleared;
	/** Every alert forgotten (a load, an attach) - a UI list empties; re-raises follow. */
	UPROPERTY(BlueprintAssignable) FOpsAlertsReset       OnAlertsReset;
	/** A build refused at commit, and key 7 refused - silent before this (spec §2). */
	UPROPERTY(BlueprintAssignable) FOpsBuildRefused      OnBuildRefused;
	UPROPERTY(BlueprintAssignable) FOpsLandRefused       OnLandRefused;

	/**
	 * The balance crossed zero - the back-in-credit toast. The ONE money delegate: the bar keeps its own
	 * Ledger->Revision gate, because a load restores the balance without a post and an event-only bar would
	 * be stale after every load (stage 3 review). OnMoneyPosted/OnLandingFeeChanged were cut for having no
	 * listener - declared, never consumed. THE GATE SEES A LOAD only since #426 - ULedger::Serialize bumps the
	 * revision; before it, a load moved the balance and not the revision, and the bar was stale all the same.
	 * ENFORCED BY: AirportMgr.UI.LedgerPanelGate (the Serialize bump alone - it deserialises directly, no OnBeforeRestore)
	 */
	UPROPERTY(BlueprintAssignable) FOpsBalanceSignChanged OnBalanceSignChanged;

	void NotifyAgentPhaseChanged(int32 AgentId, EAgentPhase From, EAgentPhase To);
	void NotifyArrivalRefused(EArrivalRefusal Why);
	void NotifySpeedChanged(ESimSpeed Speed);
	void NotifyNotification(const FString& Text);
	void NotifyWarning(const FString& Text);
};
