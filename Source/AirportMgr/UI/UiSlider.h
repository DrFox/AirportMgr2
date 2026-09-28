#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UiSlider.generated.h"

class USlider;
class UTextBlock;
class UUIStyle;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUiSliderChanged, float, Value);

/**
 * A value slider with its readout (UI library step 4a): a Rule-coloured bar, an Accent handle, and
 * the value to Decimals places with a suffix ("1.25x"). Values are CLAMPED to Min..Max and
 * QUANTISED to Step on every path in - a player's drag, a saved setting, code - so what the readout
 * shows is always a value the setting can actually hold.
 */
UCLASS()
class AIRPORTMGR_API UUiSlider : public UUserWidget
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style, float InMin, float InMax, float InStep, int32 InDecimals, const FText& InSuffix);

	/** From CODE: no event unless bBroadcast. */
	void SetValue(float V, bool bBroadcast = false);
	float GetValue() const { return Value; }

	UPROPERTY(BlueprintAssignable) FUiSliderChanged OnValueChanged;

	/** Raised when the player LETS GO, with the value let go on - for a setting too heavy to apply
	 *  on every step. OnValueChanged still fires during the drag. */
	UPROPERTY(BlueprintAssignable) FUiSliderChanged OnValueCommitted;

	/** USlider's own event - the player's drag, or the echo of our own SetValue (ignored). */
	UFUNCTION() void HandleSliderMoved(float Raw);

	/** USlider's mouse capture ending - the player let go. */
	UFUNCTION() void HandleReleased();

	FString ReadoutForTest() const;
	int32 BroadcastCountForTest() const { return Broadcasts; }
	int32 CommitCountForTest() const { return Commits; }

private:
	float Quantise(float V) const;

	UPROPERTY() TObjectPtr<USlider> Slider;
	UPROPERTY() TObjectPtr<UTextBlock> Readout;
	float Min = 0.0f;
	float Max = 1.0f;
	float Step = 0.0f;
	int32 Decimals = 2;
	FText Suffix;
	float Value = 0.0f;
	int32 Broadcasts = 0;
	int32 Commits = 0;
	/** True while SetValue pushes into USlider, whose own SetValue broadcasts - see SetValue. */
	bool bSettingFromCode = false;
};
