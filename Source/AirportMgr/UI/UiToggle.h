#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UiToggle.generated.h"

class UBorder;
class UButton;
class UOverlaySlot;
class UUIStyle;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUiToggleChanged, bool, bOn);

/**
 * An on/off switch (UI library step 4a): a pill track, Accent when on and Rule when off, with a
 * Surface knob that sits at the end it is switched to. A UButton inside, so hover and press come
 * from the engine; the button draws nothing itself (the track IS the look).
 */
UCLASS()
class AIRPORTMGR_API UUiToggle : public UUserWidget
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style);

	/** From CODE: no event unless bBroadcast - see the step 4a plan's Review Focus 1. */
	void SetOn(bool bInOn, bool bBroadcast = false);
	bool IsOn() const { return bOn; }

	/** Raised when the PLAYER flips it (or code asks, with bBroadcast). */
	UPROPERTY(BlueprintAssignable) FUiToggleChanged OnToggled;

	/** The player's click. Public for the test that clicks it. */
	UFUNCTION() void HandleClicked();

	FLinearColor TrackColourForTest() const;
	int32 BroadcastCountForTest() const { return Broadcasts; }

private:
	void Paint();

	UPROPERTY() TObjectPtr<const UUIStyle> Style;
	UPROPERTY() TObjectPtr<UBorder> Track;
	UPROPERTY() TObjectPtr<UOverlaySlot> KnobSlot;
	bool bOn = false;
	int32 Broadcasts = 0;
};
