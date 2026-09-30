#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/UiButton.h"
#include "UI/UiRadioGroup.h"
#include "UiMenuButton.generated.h"

class UMenuAnchor;
class UUiButton;
class UUiMenuButton;
class UUIStyle;

/** One line of a UUiMenuButton's popup, as it should read at the moment the popup opens. */
struct FUiMenuItem
{
	FText Label;
	/** Greyed and unclickable when false; Why is its tooltip, so the player learns what would enable it. */
	bool bEnabled = true;
	FText Why;
	/**
	 * DESTRUCTIVE: the first click only arms it - the line re-reads ConfirmLabel - and the second
	 * chooses it. Closing the popup disarms it. Misclick safety outranks one fewer click on anything
	 * that cannot be undone (memory: destructive gestures need a deliberate mode).
	 */
	bool bConfirm = false;
	FText ConfirmLabel;
};

/** One line's click - the UUiDropdownEntry reason: a dynamic delegate needs a UObject target. */
UCLASS()
class AIRPORTMGR_API UUiMenuEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() TWeakObjectPtr<UUiMenuButton> Owner;
	UFUNCTION() void HandleClicked();
};

/** The popup: UUiDropdownList's Surface card of Ghost lines, each enabled or greyed with a reason. */
UCLASS()
class AIRPORTMGR_API UUiMenuList : public UUserWidget
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style, const TArray<FUiMenuItem>& Items, UUiMenuButton& Owner);
	/** Re-captions line Index in place - the armed confirm - without rebuilding the popup. */
	void SetLineLabel(int32 Index, const FText& Label);

private:
	UPROPERTY() TArray<TObjectPtr<UUiMenuEntry>> Entries;
	UPROPERTY() TArray<TObjectPtr<UUiButton>> Lines;
};

/**
 * A VERB WITH SUB-CHOICES (UI library, 2026-09-29, for the inspector's Unstick): a Secondary
 * UUiButton inside a UMenuAnchor whose popup lists what it can do. NOT UUiDropdown, which is a
 * CHOICE - it shows the current option and remembers it; this remembers nothing and shows its own
 * name, and its lines can be greyed with a reason or need a confirming second click.
 *
 * The lines are ASKED FOR ON EVERY OPEN (Items), never cached: what an agent can do changes every
 * frame it moves, and a popup built at BuildOnce would offer yesterday's answers.
 */
UCLASS()
class AIRPORTMGR_API UUiMenuButton : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Layout, padding and icon are the button's own (UUiButton::Build, SetIcon) - defaulted to the inspector's inline
	 *  verb; the bar passes its stacked, icon-topped look so a menu verb there looks like every other button. */
	void Build(const UUIStyle& Style, const FText& Label, EUiButtonLayout Layout = EUiButtonLayout::Inline,
		bool bStylePadding = true, UTexture2D* Icon = nullptr, float IconSize = 0.0f);

	/** The lines, asked for as the popup opens. */
	TFunction<TArray<FUiMenuItem>()> Items;

	/** A line was chosen - after its confirm, for a bConfirm line. The popup is closed first. */
	UPROPERTY(BlueprintAssignable) FUiChoiceChanged OnChosen;

	void Open();
	void Close();
	bool IsOpen() const;

	/** From a line's click: arms a bConfirm line, else closes and raises OnChosen. */
	void Choose(int32 Index);

	UUiButton* GetButton() const { return Button; }

	/** The anchor's content, built on each open. Public for the test that builds it headless. */
	UFUNCTION() UUserWidget* BuildMenu();
	UFUNCTION() void HandleOpenClicked();
	UFUNCTION() void HandleOpenChanged(bool bIsOpen);

	int32 ArmedForTest() const { return Armed; }
	int32 ChosenCountForTest() const { return Chosen; }
	int32 LastChosenForTest() const { return LastChosen; }
	const TArray<FUiMenuItem>& ShownItemsForTest() const { return Shown; }

private:
	UPROPERTY() TObjectPtr<const UUIStyle> Style;
	UPROPERTY() TObjectPtr<UMenuAnchor> Anchor;
	UPROPERTY() TObjectPtr<UUiButton> Button;
	UPROPERTY() TObjectPtr<UUiMenuList> OpenList;

	/** What the open popup shows - the answer Items gave as it opened. */
	TArray<FUiMenuItem> Shown;

	/** The bConfirm line clicked once, or INDEX_NONE. Reset whenever the popup closes. */
	int32 Armed = INDEX_NONE;

	/** How many times OnChosen was raised, and with what - UUiDropdown's Broadcasts, for a headless test
	 *  that has no UObject to bind a dynamic delegate to. */
	int32 Chosen = 0;
	int32 LastChosen = INDEX_NONE;
};
