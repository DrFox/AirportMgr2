#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UiRadioGroup.generated.h"

class UUiButton;
class UUiRadioGroup;
class UUIStyle;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUiChoiceChanged, int32, Index);

/** One segment's click, as an object: UButton::OnClicked binds only to a UFUNCTION on a UObject
 *  (the UBuildBarEntry reason). Holds the INDEX, never a label to look up. */
UCLASS()
class AIRPORTMGR_API UUiRadioEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() TWeakObjectPtr<UUiRadioGroup> Owner;
	UFUNCTION() void HandleClicked();
};

/**
 * One of N as a segmented row of UUiButtons (UI library step 4a) - flatter than radio circles, and
 * the lit segment is the bar's own "selected" look (UUiButton::LookFor: Accent), so a choice reads
 * the same here as an armed tool does on the bar.
 */
UCLASS()
class AIRPORTMGR_API UUiRadioGroup : public UUserWidget
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style, const TArray<FText>& Options);

	/** From CODE: clamped into range; no event unless bBroadcast. */
	void SetSelected(int32 Index, bool bBroadcast = false);
	int32 GetSelected() const { return Selected; }

	/** The player's click: selects and raises the event - unless it is already the one lit. */
	void Choose(int32 Index);

	UPROPERTY(BlueprintAssignable) FUiChoiceChanged OnSelectionChanged;

	int32 SelectedButtonCountForTest() const;
	int32 BroadcastCountForTest() const { return Broadcasts; }

private:
	void Paint();

	UPROPERTY() TObjectPtr<const UUIStyle> Style;
	UPROPERTY() TArray<TObjectPtr<UUiButton>> Buttons;
	UPROPERTY() TArray<TObjectPtr<UUiRadioEntry>> Entries;
	int32 Selected = 0;
	int32 Broadcasts = 0;
};
