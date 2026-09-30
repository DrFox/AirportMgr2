#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "LandChoices.h"

#include "LandAircraftPanelWidget.generated.h"

class ARoadBuildController;
class UAircraftType;
class UButton;
class UUiButton;
class UUiButton;
class ULandAircraftPanelWidget;
class UTextBlock;
class UUIStyle;
class UOpsRuntime;
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
	UPROPERTY() TObjectPtr<UUiButton> Button;

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
 * GREYED ROWS ARE CLICKS THE GAME WOULD REFUSE: each row renders UOpsRuntime::QuoteLanding at the
 * view focus - the arrival plan and the airport's gate that the click's accept asks (#432) - never a
 * second opinion about admission. It judged the nearest runway itself until #432, stale since #412
 * made the planner land on whichever runway takes the arrival. While the airport is not open, every
 * row the plan would take is refused by the gate (whole-stack review M1).
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

	/** Top-left; closable (the close is the toggle, which the aircraft.land action runs - see BuildActions and UAirportMgrPanelWidget::Toggle). */
	virtual bool WantsWindow(FUiWindowSpec& Out) const override;
	/** JUDGED ON OPEN, not left to the next tick - a panel that appeared empty for a frame and then filled reads as a bug. */
	virtual void OnShownChanged(bool bShown) override;

	/**
	 * Re-quote every row if anything a quote reads has moved, and repaint if a verdict changed. What
	 * NativeTick calls while open, and what OnShownChanged calls on opening.
	 */
	void Refresh();

	/** Refresh against a given driver and ops runtime - Refresh passes Controller() and the ops subsystem's, both
	 *  null in a headless world (see ClickRowForTest), so a test hands its own in. No runtime, nothing lands: every row
	 *  is refused, since the land path is the flight board's (#431). */
	void RefreshFor(const ARoadBuildController* C, const UOpsRuntime* Runtime = nullptr);

	/** Row Index's button is enabled - it is a click the game would take. */
	bool IsRowEnabledForTest(int32 Index) const;

	/** What row Index last showed as its refusal, empty when admitted. */
	FString RowRefusalForTest(int32 Index) const { return PaintedRefusals.IsValidIndex(Index) ? PaintedRefusals[Index] : FString(); }

	/** How many times Refresh actually asked LandChoices::Build - FLandChoicesKey's gate's counter. */
	int32 BuildCountForTest() const { return BuildCalls; }

	/** Where the types come from, instead of LandChoices::EveryMeshedType - so a test can have none, then some. */
	void SetTypeSourceForTest(TFunction<TArray<UAircraftType*>()> Source) { TypeSource = MoveTemp(Source); }

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
	virtual void TickPanel(float DeltaTime) override;

private:
	/** The content's types, read once on first open - held here so the rows' raw pointers
	 *  (FLandChoice::Type) stay alive. */
	UPROPERTY() TArray<TObjectPtr<UAircraftType>> Types;

	UPROPERTY() TArray<TObjectPtr<ULandRowEntry>> Entries;

	/**
	 * What each row last SHOWED - its refusal, empty when admitted - in row order. The panel
	 * re-judged every tick while open, because the runway nearest the focus changes as the
	 * camera pans and a strip being drawn changes length, with no single event for either
	 * (since ops batch 3 PR E it re-judges when JudgedKey moves, which covers both).
	 * Rebuilding eighteen row widgets sixty times a second to discover nothing moved is the
	 * expensive kind of correct, so the widgets are rebuilt only when this differs - the
	 * ledger's revision gate, keyed on the output because there is no one input to key on.
	 */
	TArray<FString> PaintedRefusals;

	/**
	 * What the rows were last JUDGED from - one step before PaintedRefusals (ops batch 3 PR E). That gate saved the
	 * widgets; Build itself still ran every frame, a CheckArrival per type. Now Build runs when FLandChoicesKey moves:
	 * the focus reduced to the runway the planner asks first, the network's two revisions, the traffic's occupancy,
	 * the network object, the gate and the runtime. Each row is a whole arrival plan since #432, so this matters more.
	 * Valid once built; a close does not reset it, since nothing it keys on is the panel's own.
	 */
	FLandChoicesKey JudgedKey;
	bool bJudged = false;

	/**
	 * How many types the rows were judged over - the gate's other input (PR E review). Types is read on the first
	 * Refresh and read AGAIN every Refresh while it is empty (a registry not yet scanned), so it can grow from none to
	 * eighteen with nothing FLandChoicesKey reads moving; the rows must be built then.
	 * ENFORCED BY: AirportMgr.UI.LandPanelJudgesWhenTypesArrive
	 */
	int32 JudgedTypeCount = 0;

	/** See SetTypeSourceForTest. Unset: LandChoices::EveryMeshedType. */
	TFunction<TArray<UAircraftType*>()> TypeSource;

	/** See BuildCountForTest. */
	int32 BuildCalls = 0;

	void PaintRows(const TArray<FLandChoice>& Choices);

	/** Choose's body, against a given driver - see ClickRowForTest. */
	void ChooseFor(ARoadBuildController& C, UAircraftType* Type);
};
