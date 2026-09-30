#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "Model/OpsAlerts.h"
#include "AlertsPanelWidget.generated.h"

class ARoadBuildController;
class UAlertsPanelWidget;
class UOpsEvents;
class UOpsRuntime;
class UUiButton;
class UUIStyle;
class UVerticalBox;

/** One row's "Go" and - for a flight that can never land - "Cancel flight": a UObject because UButton::OnClicked binds to a UFUNCTION
 *  (ULandRowEntry's shape); both buttons of a row share it, and so its key. */
UCLASS()
class UAlertRowEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() TWeakObjectPtr<UAlertsPanelWidget> Owner;
	/** BY KEY, not row index: a clear between a paint and a click shifts the indices (stage 2 review). */
	FOpsAlertKey Key;

	/** The row's Cancel flight button, kept so a test can click the real delegate and check it is bound (#442 review). Only a
	 *  FlightCannotLand row has one. */
	UPROPERTY() TObjectPtr<UUiButton> CancelButton;

	UFUNCTION() void HandleClick();
	/** The row's Cancel flight - UAlertsPanelWidget::OnCancelClicked, which asks the panel's own ops runtime (#442). */
	UFUNCTION() void HandleCancelClick();
};

/**
 * The standing alerts, as a window: one row per problem, each with "Go". Spec 2026-09-29-ops-alerts §3.
 *
 * THE LIST IS KEPT HERE, the one mirror of UOpsAlerts's set on the UI side - fed by UOpsEvents' raise,
 * clear and reset, never by asking the model each frame. The bar's Alerts button reads its count from
 * here (ARoadBuildController::AlertCount), so the badge and the window cannot disagree.
 *
 * TOP LEFT, not AboveBarLeft as the spec first said: that corner is the inspector's, and "Go" opens the
 * inspector - the two would sit on top of each other exactly when both are wanted.
 */
UCLASS()
class AIRPORTMGR_API UAlertsPanelWidget : public UAirportMgrPanelWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UVerticalBox> RowColumn;

	/** Mirror UOpsEvents' alerts. BuildOnce calls it with the runtime's; a test with its own. */
	void BindTo(UOpsEvents& Events);

	int32 AlertCount() const { return Alerts.Num(); }
	const TArray<FOpsAlert>& GetAlerts() const { return Alerts; }

	bool IsShowing() const { return bShowing; }
	void Toggle();

	/**
	 * Row Index's "Go": the camera to its subject and the subject selected (ARoadBuildController::
	 * SelectAndFocus). Overdrawn, which has no place, opens the ledger instead. False when there is no
	 * such row, or nowhere to go.
	 */
	bool Go(int32 Index, ARoadBuildController& Controller);

	/** Go by the alert's key - what a row's button calls. False when that alert has cleared since. */
	bool GoTo(const FOpsAlertKey& Key, ARoadBuildController& Controller);

	/**
	 * THE CANCEL BESIDE A FlightCannotLand ROW (#442): UOpsRuntime::CancelFlight for the row's flight - its stand released, the
	 * airline charged the closure's per-flight penalty (an open owner question: see UFlightBoard::CancelByPlayer). BY KEY, like GoTo:
	 * a clear between a paint and a click shifts the rows. False when that alert has cleared since, is not a FlightCannotLand (no
	 * other kind offers a cancel), or the runtime refuses.
	 *
	 * HUNG HERE, ON THE ALERT, and not on the aircraft card: the flight is holding off the map and has no aeroplane to select, so
	 * the card the Unstick lives on cannot open for it, and the arrivals panel's rows carry no actions at all.
	 * ENFORCED BY: AirportMgr.UI.Alerts.CancelFlightCancelsOnlyAFlightCannotLandRow
	 */
	bool CancelFlightOf(const FOpsAlertKey& Key, UOpsRuntime& Runtime);

	/**
	 * THE CLICK ITSELF (what a row's Cancel flight button calls): CancelFlightOf against this panel's own ops runtime - the world's
	 * resolver's, which a test overrides for a headless world - and LOGGED, not silent, when there is none: a button that does nothing
	 * with no trace is the shape this project keeps paying for.
	 */
	bool OnCancelClicked(const FOpsAlertKey& Key);

	/** Paint the rows now, instead of on the next tick while shown - for a test that clicks one. */
	void PaintRowsForTest() { PaintRows(); }

	/** Row Key's Cancel flight button is bound to its own entry's HandleCancelClick - the hop ClickCancelForTest skips when it
	 *  calls the method. Checked by name on the delegate, so an unbound button fails it. False for a row with no such button. */
	bool IsCancelBoundForTest(const FOpsAlertKey& Key) const;

	/** Click row Key's Cancel flight button - its real OnClicked, so the binding is part of what is measured. False when the row
	 *  has none. */
	bool ClickCancelForTest(const FOpsAlertKey& Key);

	virtual bool WantsWindow(FUiWindowSpec& Out) const override;
	virtual void OnWindowClosedByPlayer() override;

protected:
	virtual void BuildOnce(const UUIStyle& Style) override;
	virtual void TickPanel(float DeltaTime) override;

private:
	UFUNCTION() void OnAlertRaised(const FOpsAlert& Alert);
	UFUNCTION() void OnAlertCleared(const FOpsAlertKey& Key);
	UFUNCTION() void OnAlertsReset();

	UPROPERTY(Transient) TArray<FOpsAlert> Alerts;
	UPROPERTY(Transient) TArray<TObjectPtr<UAlertRowEntry>> Entries;
	/** The style's warning icon, resolved once in BuildOnce - the toast stack's reason (issue #186). */
	UPROPERTY(Transient) TObjectPtr<class UTexture2D> WarningIcon;
	bool bShowing = false;

	/** Set when the list changes; the rows are rebuilt on the next tick while shown - not every frame. */
	bool bRowsDirty = true;

	void PaintRows();
};
