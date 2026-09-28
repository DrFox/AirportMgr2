#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/UiRadioGroup.h"
#include "UiDropdown.generated.h"

class UMenuAnchor;
class UUiButton;
class UUiDropdown;
class UUIStyle;

/** One option's click in the popup - the UUiRadioEntry reason. */
UCLASS()
class AIRPORTMGR_API UUiDropdownEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() TWeakObjectPtr<UUiDropdown> Owner;
	UFUNCTION() void HandleClicked();
};

/** The popup: a Surface card of Ghost options, one per line. */
UCLASS()
class AIRPORTMGR_API UUiDropdownList : public UUserWidget
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style, const TArray<FText>& Options, UUiDropdown& Owner);
	int32 OptionCountForTest() const { return Entries.Num(); }

private:
	UPROPERTY() TArray<TObjectPtr<UUiDropdownEntry>> Entries;
};

/**
 * A choice from a list that opens on click (UI library step 4a): a Secondary UUiButton reading the
 * current option and a down-arrow, inside a UMenuAnchor whose popup Slate places in its own layer -
 * above every window, and never clipped by a window's ClipToBounds (the spec's known risk).
 */
UCLASS()
class AIRPORTMGR_API UUiDropdown : public UUserWidget
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style, const TArray<FText>& InOptions);

	/** The "this opens a list" arrow after the label. A character Inter HAS - see UUIStyle::CanDraw.
	 *  ENFORCED BY: AirportMgr.UI.Controls.DropdownArrowIsInInter. */
	static constexpr UTF32CHAR ArrowCodepoint = 0x25BC;   // U+25BC; U+25BE (the first choice) is not in Inter

	/** From CODE: clamped into range; no event unless bBroadcast. */
	void SetSelected(int32 Index, bool bBroadcast = false);
	int32 GetSelected() const { return Selected; }

	/**
	 * Shows Label for a value that is none of the options (a custom graphics mix) - selection
	 * INDEX_NONE, so EVERY option is a change and choosing any raises the event. A stand-in option
	 * instead would swallow the player's choice of that one option.
	 * ENFORCED BY: AirportMgr.Settings.Panel.CustomGraphicsStaysCustom.
	 */
	void ShowUnlisted(const FText& Label);

	/** From the popup: selects, raises the event if it changed, and closes the popup. */
	void Choose(int32 Index);

	UPROPERTY(BlueprintAssignable) FUiChoiceChanged OnSelectionChanged;

	/** The anchor's content, built on each open. Public for the test that builds it headless. */
	UFUNCTION() UUserWidget* BuildMenu();

	UFUNCTION() void HandleOpenClicked();

	FString LabelForTest() const;
	int32 BroadcastCountForTest() const { return Broadcasts; }

private:
	UPROPERTY() TObjectPtr<const UUIStyle> Style;
	UPROPERTY() TObjectPtr<UMenuAnchor> Anchor;
	UPROPERTY() TObjectPtr<UUiButton> Button;
	TArray<FText> Options;
	int32 Selected = 0;
	int32 Broadcasts = 0;
};
