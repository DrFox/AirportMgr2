#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"

#include "AirlinesPanelWidget.generated.h"

class UAirlineDetailViewModel;
class UAirlineListViewModel;
class UAirlinesPanelWidget;
class UOpsRuntime;
class UScrollBox;
class UTextBlock;
class UUiButton;
class UUiSparkline;
class UUIStyle;
class UVerticalBox;
struct FAirlineDetail;

/**
 * One airline row's button, bound to its airline.
 *
 * A UObject per row for the reason ULandRowEntry and UAlertRowEntry are one: UButton::OnClicked is a DYNAMIC delegate and binds only
 * to a UFUNCTION on a UObject - a lambda cannot bind.
 */
UCLASS()
class UAirlineRowEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() FName AirlineId;
	UPROPERTY() TWeakObjectPtr<UAirlinesPanelWidget> Owner;
	UPROPERTY() TObjectPtr<UUiButton> Button;

	UFUNCTION() void HandleClick();
};

/**
 * The Airlines window (spec 2026-10-02-airlines-panel section 2): who flies here, how they feel about it, and why.
 *
 * LIST LEFT, DETAIL RIGHT - the layout the owner chose. Left, one row per airline ("Flying Club  71% ▲"), the selected one lit; right,
 * the selected airline's header, its week as a sparkline, what moved it (tally rows), which of its fleet can come, its offers and its
 * flights. Every figure and sentence is a view model's (UAirlineListViewModel, UAirlineDetailViewModel) - this file only lays them out
 * and colours them by meaning from UUIStyle.
 *
 * READ-ONLY. Offers are listed, never answered here: the inbox is the one place that acts on an offer, so the two windows cannot
 * disagree about what Accept did.
 *
 * REBUILT WHEN WHAT IT PRINTS CHANGES, NOT PER FRAME. Neither the roster nor the generator keeps a revision, so each half keys on a
 * hash of what it draws from. The LIST's key is every standing's printed percentage and arrow; it is rebuilt only when that moves,
 * because its rows are buttons: rebuilding them under the cursor between a press and its release would eat the click. Selecting
 * relights the existing buttons and repaints the detail, never the list.
 *
 * The DETAIL mixes two clocks, and keys on neither directly. An offer's countdown drains in REAL seconds, while the game clock runs 20
 * to 2400 game seconds per real one (x1 daylight to x32 night), so a key on game time rebuilt every frame at speed. Two checks instead:
 *  - every frame, a cheap key of what moves the detail at once - the selection, the board's revision, the selected standing's value,
 *    the history's day, and each of the airline's offers' countdown in the whole seconds it prints (DetailKeyOf);
 *  - once a REAL second (TextCheckSeconds), whatever the game clock has moved - the rate, the fleet verdicts, a flight's contract.
 * Either one builds the view model, and the widgets are rebuilt only when its printed text differs from what is on screen (TextKeyOf).
 * So a countdown ticks on the frame its second changes, a game-time sentence shows at most a real second late, and a frame that
 * changes nothing printed rebuilds nothing at any speed.
 *
 * C++ BASE, BLUEPRINT OPTIONAL in principle (UAirportMgrPanelWidget's rule) but CODE-ONLY in practice, like Alerts and Arrivals: no
 * *Class hook on UBuildHudLayer, and no BindWidgetOptional slots - a window born after windows existed (see SettingsPanel's comment).
 */
UCLASS()
class AIRPORTMGR_API UAirlinesPanelWidget : public UAirportMgrPanelWidget
{
	GENERATED_BODY()

public:
	/** The list column's width, uu - wide enough for "Flying Club  71% ▲" without wrapping. */
	UPROPERTY(EditAnywhere, Category = "Airlines|Style") float ListWidth = 210.0f;
	/** The detail pane's width, uu; the fleet reasons wrap inside it. */
	UPROPERTY(EditAnywhere, Category = "Airlines|Style") float DetailWidth = 320.0f;
	/** The detail pane's tallest, uu, before it scrolls on its own - so a long fleet list does not push the window off screen. */
	UPROPERTY(EditAnywhere, Category = "Airlines|Style") float DetailMaxHeight = 460.0f;

	/** Centred on screen (the corners are taken - see the definition); toggled from the bar's Airlines button (game.airlines). */
	virtual bool WantsWindow(FUiWindowSpec& Out) const override;
	/** REPAINTED ON OPEN, the ledger's reason: a window that appears empty for a frame reads as a bug, not as latency. */
	virtual void OnShownChanged(bool bShown) override;

	/** Re-read the runtime and repaint whichever half's memo key moved. What the tick calls while open, and a test directly. */
	void Refresh();

	/** Show AirlineId in the detail pane and light its row. An id with no standing still shows - its percentage reads "—". */
	void Select(FName AirlineId);
	FName GetSelected() const { return Selected; }

	// --- test seams -------------------------------------------------------------------------------------------------------------
	int32 ListRowCountForTest() const { return Entries.Num(); }
	/** Click list row Index through its button's real OnClicked, so the binding is part of what is measured. False: no such row. */
	bool ClickListRowForTest(int32 Index);
	/** Whether list row Index is drawn selected. */
	bool IsRowSelectedForTest(int32 Index) const;
	FText HeaderNameForTest() const;
	FText HeaderPercentForTest() const;
	/** Whether any VISIBLE text block in the window reads exactly Words. */
	bool ShowsTextForTest(const FString& Words) const;
	UUiSparkline* TrendForTest() const { return Trend; }
	int32 ListRebuildsForTest() const { return ListRebuilds; }
	int32 DetailRebuildsForTest() const { return DetailRebuilds; }

protected:
	virtual void BuildOnce(const UUIStyle& Style) override;
	virtual void TickPanel(float DeltaTime) override;

private:
	UPROPERTY() TObjectPtr<UAirlineListViewModel> ListModel;
	UPROPERTY() TObjectPtr<UAirlineDetailViewModel> DetailModel;

	UPROPERTY() TObjectPtr<UVerticalBox> ListColumn;
	UPROPERTY() TObjectPtr<UTextBlock> NoAirlinesText;
	UPROPERTY() TObjectPtr<UVerticalBox> DetailColumn;
	UPROPERTY() TObjectPtr<UTextBlock> HeaderName;
	UPROPERTY() TObjectPtr<UTextBlock> HeaderPercent;
	UPROPERTY() TObjectPtr<UTextBlock> RateText;
	UPROPERTY() TObjectPtr<UTextBlock> FactorText;
	UPROPERTY() TObjectPtr<UUiSparkline> Trend;
	UPROPERTY() TObjectPtr<UTextBlock> NoHistoryText;
	UPROPERTY() TObjectPtr<UVerticalBox> TallyColumn;
	UPROPERTY() TObjectPtr<UVerticalBox> FleetColumn;
	UPROPERTY() TObjectPtr<UVerticalBox> OfferColumn;
	UPROPERTY() TObjectPtr<UVerticalBox> FlightColumn;

	UPROPERTY() TArray<TObjectPtr<UAirlineRowEntry>> Entries;

	FName Selected;
	/** The keys the two halves were last painted at - see the class comment. bHas* false: never painted. */
	uint32 PaintedListKey = 0;
	uint32 PaintedDetailKey = 0;
	uint32 PaintedTextKey = 0;
	bool bHasPaintedList = false;
	bool bHasPaintedDetail = false;
	bool bHasPaintedText = false;
	/** Real seconds since the detail's game-time text was last checked - accumulated from the tick's real DeltaTime. */
	float RealSinceTextCheck = 0.0f;
	/** How stale a game-time sentence (rate, verdicts, a flight's contract) may get: one REAL second, the finest any of them prints at
	 *  x1 ("12 min" moves once per ~3 real s in daylight) and the countdown's own grain - see the class comment. */
	static constexpr float TextCheckSeconds = 1.0f;
	/** See ListRebuildsForTest / DetailRebuildsForTest: how often each half was actually rebuilt - the memo key's effect, made visible. */
	int32 ListRebuilds = 0;
	int32 DetailRebuilds = 0;

	void BuildLayout(const UUIStyle& Style);
	void PaintList(const UOpsRuntime& Runtime);
	/** Rebuild the detail's widgets from D; null: no airline selected (an empty roster) and the pane collapses. */
	void PaintDetail(const FAirlineDetail* D);
	void LightSelectedRow();

	/** A heading in the detail pane ("Last 7 days"), muted, with a section's gap above it. */
	UTextBlock* AddHeading(UVerticalBox& Column, const FText& Text);
	/** A muted line - an empty state ("No offers") or a fleet reason. */
	UTextBlock* AddLine(UVerticalBox& Column, const FText& Text, const FLinearColor& Colour, bool bWrap = false);

	static uint32 ListKeyOf(const UOpsRuntime& Runtime);
	uint32 DetailKeyOf(const UOpsRuntime& Runtime) const;
	/** Every string, flag and figure PaintDetail prints from D, hashed - what decides whether a built detail is a rebuild. */
	static uint32 TextKeyOf(const FAirlineDetail& D);
};
