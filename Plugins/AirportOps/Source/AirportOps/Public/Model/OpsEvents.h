#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/OpsAlerts.h"
#include "Model/RoadAgent.h"
#include "Model/SimClock.h"
#include "UObject/Object.h"
#include "OpsEvents.generated.h"

/**
 * What a save or a load came to (#445 item 7) - a CASE, worded by the toast widget, not a sentence. UOpsRuntime used to
 * publish "Saved 'X'", "Save to 'X' failed" and "No save 'X'" as English lines on a catch-all event (FNotificationEvent),
 * so the toast could not tell a failed save from a good one and showed both as Info. An enum, not bOk: a load has two ends
 * of its own, and four cases are one field.
 */
UENUM(BlueprintType)
enum class EOpsSaveOutcome : uint8
{
	Saved,
	/** OpsSave::WriteSlot answered false - nothing reached disk. The player asked and it did not happen. */
	SaveFailed,
	Loaded,
	/** A load named a slot with no save in it. Asked before anything was torn down, so the airport is as it was. */
	NoSave
};

/**
 * What a purchase toast is about (#445 item 7): which of the fleet's or the depot's changes it reports. The sentence -
 * "Bought", "Sold", "Depot removed", "No room on its plot", with or without a figure - is the toast widget's.
 */
UENUM(BlueprintType)
enum class EOpsPurchaseKind : uint8
{
	VehicleBought,
	VehicleSold,
	/** A depot went and its vehicle with it; Amount is what the fleet's door credited for it, 0 for a starter vehicle (#443). */
	VehicleWithdrawn,
	ModuleBought,
	/** #266's repair: modules with no room left on the plot, removed and refunded - the game's doing, not the player's. */
	ModulesRefunded,
	/** A spot fuel order paid for (UOpsRuntime::OrderSpotFuel). Name is the litres; Amount the price, paid now. */
	FuelOrdered,
	/** A fuel contract signed. Name says the tier and term; Amount is ONE DAY's cost, charged at each day end - signing pays nothing. */
	FuelContractSigned,
	/** A fuel contract cancelled. Amount is the cancellation charge, 0 when the term had run out. */
	FuelContractCancelled
};

/**
 * One purchase, as the toast needs it: the facts, and THE NOUNS IN THEIR OWNERS' WORDS (#445 item 7). The vehicle's name is
 * FServiceFleet::NameOf, the module's is its offer's DisplayName (or PluralName when Count is not 1), the money is
 * UPricing::Format - each the one function that names that thing, called by UOpsRuntime, which holds the board, the offer
 * table and the pricing. The SENTENCE is the widget's. The widget resolving the nouns itself would reach three ops objects
 * from UI for words their owners already say - FBuildRefusedEvent's What and Price carry nouns for the same reason.
 */
USTRUCT(BlueprintType)
struct AIRPORTOPS_API FOpsPurchase
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Ops") EOpsPurchaseKind Kind = EOpsPurchaseKind::VehicleBought;
	/** What changed hands - see the struct. */
	UPROPERTY(BlueprintReadOnly, Category = "Ops") FText Name;
	/** How many: refunded modules come in a number, everything else is one. */
	UPROPERTY(BlueprintReadOnly, Category = "Ops") int32 Count = 1;
	/** The money that moved - a price, a resale, a credit, a refund - as posted; 0 when none did. */
	UPROPERTY(BlueprintReadOnly, Category = "Ops") double Amount = 0.0;
	/** Amount in UPricing::Format's words. */
	UPROPERTY(BlueprintReadOnly, Category = "Ops") FText Money;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOpsArrivalRefused, EArrivalRefusal, Why, const FString&, Sentence);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOpsSaveSlot, EOpsSaveOutcome, Outcome, const FString&, SlotName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsPurchased, const FOpsPurchase&, Purchase);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsAlertRaised, const FOpsAlert&, Alert);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsAlertCleared, const FOpsAlertKey&, Key);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsAlertChanged, const FOpsAlertKey&, Key);
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
 * EVERY DELEGATE HERE HAS A LISTENER, and a test says so by reflection (#445: OnAgentPhaseChanged and OnSpeedChanged had none
 * - nothing bound them outside a test, and the wiring test counted a log line as a consumer - so they were cut; the shape
 * of closed #169). The bus events with no face here are still consumed: by the boards, in the Sim tier.
 * ENFORCED BY: AirportMgr.UI.EveryOpsEventDelegateHasAListener (every delegate here), AirportOps.Present.Bus.EveryEventHasASubscriber (every event)
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
	UPROPERTY(BlueprintAssignable) FOpsArrivalRefused    OnArrivalRefused;

	/**
	 * A save or a load came to Outcome on Slot (#445 item 7). WAS OnNotification(FString), which carried the runtime's
	 * own sentence - so "Save to 'X' failed" reached the toast as a plain line and showed as Info. The case is typed now
	 * and the toast widget words it, at the severity the case calls for.
	 */
	UPROPERTY(BlueprintAssignable) FOpsSaveSlot          OnSaveSlot;

	/**
	 * A vehicle bought, sold or withdrawn with its depot; a depot module bought, or removed and refunded by #266's repair
	 * (#445 item 7). WAS OnNotification and OnWarning, each an FString the runtime worded - a DELEGATE OF ITS OWN for the
	 * warning only because the severity enum is the game module's (ENotificationSeverity), which AirportOps may not see.
	 * The kind carries that now: the widget decides that a refund is a Warning.
	 */
	UPROPERTY(BlueprintAssignable) FOpsPurchased         OnPurchase;

	/** A standing problem started / stopped (spec 2026-09-29-ops-alerts). The toast and the alert window. */
	UPROPERTY(BlueprintAssignable) FOpsAlertRaised       OnAlertRaised;
	UPROPERTY(BlueprintAssignable) FOpsAlertCleared      OnAlertCleared;
	/** A standing alert's words changed while it stayed true (#445): the window re-reads the model - it names only the key. */
	UPROPERTY(BlueprintAssignable) FOpsAlertChanged      OnAlertChanged;
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

	void NotifyArrivalRefused(EArrivalRefusal Why, const FString& Sentence);
	void NotifySaveSlot(EOpsSaveOutcome Outcome, const FString& Slot);
	void NotifyPurchase(const FOpsPurchase& Purchase);
};
