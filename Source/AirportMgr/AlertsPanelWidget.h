#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "Model/OpsAlerts.h"
#include "AlertsPanelWidget.generated.h"

class ARoadBuildController;
class UAlertsPanelWidget;
class UOpsEvents;
class UUiButton;
class UUIStyle;
class UVerticalBox;

/** One row's "Go": a UObject because UButton::OnClicked binds to a UFUNCTION (ULandRowEntry's shape). */
UCLASS()
class UAlertRowEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() TWeakObjectPtr<UAlertsPanelWidget> Owner;
	int32 Index = INDEX_NONE;

	UFUNCTION() void HandleClick();
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
	bool bShowing = false;

	/** Set when the list changes; the rows are rebuilt on the next tick while shown - not every frame. */
	bool bRowsDirty = true;

	void PaintRows();
};
