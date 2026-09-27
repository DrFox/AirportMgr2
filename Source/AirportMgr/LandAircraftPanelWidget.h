#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "LandChoices.h"

#include "LandAircraftPanelWidget.generated.h"

class ARoadBuildController;
class UAircraftType;
class UButton;
class ULandAircraftPanelWidget;
class UTextBlock;
class UUIStyle;
class UVerticalBox;

/**
 * One row's button, bound to its type.
 *
 * A UObject per row for the same reason UOfferRowEntry is one: UButton::OnClicked is a
 * DYNAMIC delegate and binds only to a UFUNCTION on a UObject - a lambda cannot bind.
 */
UCLASS()
class ULandRowEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() TObjectPtr<UAircraftType> Type;
	UPROPERTY() TWeakObjectPtr<ULandAircraftPanelWidget> Owner;
	UPROPERTY() TObjectPtr<UButton> Button;

	UFUNCTION() void HandleClick();
};

/**
 * The Land panel: every aircraft type with a model, and a click lands it at the view focus.
 *
 * WHAT IT IS FOR. Before it, key 7 landed one type - whatever DefaultGame.ini's
 * LandAircraftType named - so seeing any other aeroplane land meant an ini edit and a PIE
 * restart, or waiting for the board to offer one. The flare fix of 2026-09-27 was found by
 * reading numbers because nothing could put an A380 on the runway to be watched.
 *
 * GREYED ROWS ARE CLICKS THE GAME WOULD REFUSE, judged by LandChoices::Build against the
 * runway a landing from the view focus would use - never a second opinion about admission.
 *
 * C++ BASE, BLUEPRINT OPTIONAL, the rule every panel here follows: the code builds a plain
 * card when no asset supplies one. A Widget Blueprint may supply RowColumn and TitleText.
 */
UCLASS()
class AIRPORTMGR_API ULandAircraftPanelWidget : public UAirportMgrPanelWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UVerticalBox> RowColumn;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;

	/** Distance from the top of the screen for the code-built card - top LEFT, because the
	 *  offer inbox and the ledger own the top right. */
	UPROPERTY(EditAnywhere, Category = "Land|Style") float TopOffset = 12.0f;

	/** Width of the aeroplane's name, so the refusals line up down the panel. */
	UPROPERTY(EditAnywhere, Category = "Land|Style") float NameWidth = 230.0f;

	bool IsShowing() const { return bShowing; }

	/** Open or close it. Called by the aircraft.land action - see BuildActions. */
	void Toggle();

	/**
	 * Re-judge every row against the current runway and repaint if anything changed. What
	 * NativeTick calls while open, and what Toggle calls on opening.
	 */
	void Refresh();

	/** A row was clicked: land its type. ULandRowEntry's only way back in. */
	void Choose(UAircraftType* Type);

	int32 RowWidgetCountForTest() const;

	/** The row index for an aircraft type asset by name, or INDEX_NONE. */
	int32 RowIndexOfForTest(const TCHAR* AssetName) const;

	/** Row Index's button is bound to its own entry's HandleClick - the hop ClickRowForTest
	 *  skips. Checked by name on the delegate, so an unbound button fails it. */
	bool IsRowBoundForTest(int32 Index) const;

	/**
	 * Click row Index on behalf of C. C is passed rather than found because Controller()'s
	 * GetFirstPlayerController() fallback is null in FAirsideTestWorld (no
	 * InitializeActorsForPlay) - UBuildBarWidget::RefreshStateForTest's seam, same reason.
	 */
	void ClickRowForTest(int32 Index, ARoadBuildController& C);

protected:
	virtual void BuildOnce(const UUIStyle& Style) override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	/** The content's types, read once on first open - held here so the rows' raw pointers
	 *  (FLandChoice::Type) stay alive. */
	UPROPERTY() TArray<TObjectPtr<UAircraftType>> Types;

	UPROPERTY() TArray<TObjectPtr<ULandRowEntry>> Entries;

	bool bShowing = false;

	/**
	 * What each row last SHOWED - its refusal, empty when admitted - in row order. The panel
	 * re-judges every tick while open, because the runway nearest the focus changes as the
	 * camera pans and a strip being drawn changes length, with no single event for either.
	 * Rebuilding eighteen row widgets sixty times a second to discover nothing moved is the
	 * expensive kind of correct, so the widgets are rebuilt only when this differs - the
	 * ledger's revision gate, keyed on the output because there is no one input to key on.
	 */
	TArray<FString> PaintedRefusals;

	void PaintRows(const TArray<FLandChoice>& Choices);

	/** Choose's body, against a given driver - see ClickRowForTest. */
	void ChooseFor(ARoadBuildController& C, UAircraftType* Type);
};
