#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "PlayerSettings.h"
#include "SettingsPanelWidget.generated.h"

class UUiDropdown;
class UUiRadioGroup;
class UUiSlider;
class UUiToggle;
class UVerticalBox;

/**
 * The player's Settings (UI library step 4b, spec section 3): a MODAL window, opened by the
 * game.settings action (the gear on the bar, and Escape).
 *
 * EVERY CONTROL APPLIES LIVE, so the player sees what a value does before keeping it: each change
 * writes one field of Current and hands the whole value to the sink. Opening SNAPSHOTS the values;
 * Cancel, Escape and the window's close put the snapshot back; Save keeps what is in force. UI
 * scale alone applies on release (OnValueCommitted), not per step - re-scaling the screen under a
 * captured mouse re-lays the very slider being dragged (step 4a final review, Important 2).
 *
 * The panel knows only IPlayerSettingsSink and the step 4a controls - what each value DOES is the
 * sink's (FGamePlayerSettingsSink), so every behaviour here is testable over a memory sink.
 */
UCLASS()
class AIRPORTMGR_API USettingsPanelWidget : public UAirportMgrPanelWidget
{
	GENERATED_BODY()

public:
	/** Where the values come from and go to. The game hands FGamePlayerSettingsSink; a test a memory one. */
	void SetSink(TSharedPtr<IPlayerSettingsSink> InSink) { Sink = MoveTemp(InSink); }

	/** Snapshot the values, load every control from them (raising nothing), show. */
	void Open();
	/** Put the snapshot back in force, save it, close - Cancel, Escape and the window's close. */
	void Cancel();
	/** Persist what is in force, close. */
	void SaveAndClose();

	/** Open if closed, cancel if open - what the game.settings action does. The base Toggle would only flip the window; opening
	 *  begins an edit session and closing reverts it. */
	virtual void Toggle() override;

	/** Centred, modal, closable (the close is Cancel), not resizable - a form, not a list. */
	virtual bool WantsWindow(FUiWindowSpec& Out) const override;
	virtual void OnWindowClosedByPlayer() override;

	// The controls' events. UFUNCTIONs because the 4a controls' events are dynamic delegates.
	UFUNCTION() void HandleUiScale(float Value);
	UFUNCTION() void HandlePanSpeed(float Value);
	UFUNCTION() void HandleZoomSpeed(float Value);
	UFUNCTION() void HandleGraphics(int32 Index);
	UFUNCTION() void HandleDriveSide(int32 Index);
	UFUNCTION() void HandleGridSnap(bool bOn);
	UFUNCTION() void HandleResetLayout();
	UFUNCTION() void HandleCancel();
	UFUNCTION() void HandleSave();

protected:
	virtual void BuildOnce(const UUIStyle& Style) override;

private:
	void Close();
	/** Every control shows Current, raising nothing (the 4a controls' Set*, bBroadcast false). */
	void LoadControls();
	/** Current changed by the player: into force through the sink. */
	void ApplyCurrent(const TCHAR* Field, const FString& Value);

	/** A Heading-role caption above a group. */
	void AddCaption(const UUIStyle& Style, UVerticalBox& Column, const FText& Text);
	/** A Well row: the label on the left, the control on the right. */
	void AddRow(const UUIStyle& Style, UVerticalBox& Column, const FText& Label, UWidget& Control);

	UPROPERTY() TObjectPtr<UUiSlider> UiScale;
	UPROPERTY() TObjectPtr<UUiSlider> PanSpeed;
	UPROPERTY() TObjectPtr<UUiSlider> ZoomSpeed;
	UPROPERTY() TObjectPtr<UUiDropdown> Graphics;
	UPROPERTY() TObjectPtr<UUiRadioGroup> DriveSide;
	UPROPERTY() TObjectPtr<UUiToggle> GridSnap;

	TSharedPtr<IPlayerSettingsSink> Sink;
	/** What Open found - what Cancel puts back. */
	FPlayerSettings Snapshot;
	/** What is in force while the dialog is open. */
	FPlayerSettings Current;
};
