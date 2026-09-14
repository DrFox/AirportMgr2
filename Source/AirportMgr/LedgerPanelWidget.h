#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"

#include "LedgerPanelWidget.generated.h"

class ULedgerPanelViewModel;
class ULedgerRowViewModel;
class UTextBlock;
class UUIStyle;
class UVerticalBox;

/**
 * The ledger: where the money went, newest first.
 *
 * WHAT IT IS FOR, TODAY. Slice C made money move in half a dozen places and the bar shows
 * only the total, so "the balance dropped and I do not know why" had no answer but the log.
 * This is that answer, and it is deliberately plain: every movement, in order, with no
 * filtering, grouping or totals. Those are the M4 finance screen, which this GROWS INTO
 * rather than being replaced by.
 *
 * HIDDEN BY DEFAULT, toggled from the bar. Unlike the offer inbox - which must never be the
 * thing that scrolls away, because missing an offer costs money - a ledger is something the
 * player goes and looks at. It is also the first real DATA TABLE in this project, and
 * therefore the measurement that should decide whether a later screen wants an HTML UI stack
 * rather than UMG: if this fights, that is evidence.
 *
 * C++ BASE, BLUEPRINT OPTIONAL, the rule every panel here follows. The code builds a plain
 * card when no asset supplies one. A Widget Blueprint may supply RowColumn, TitleText and
 * BalanceText by name; a UListView is deliberately NOT built in code - see
 * UOfferInboxWidget's header for why - which is what caps the code path at MaxRows.
 */
UCLASS()
class AIRPORTMGR_API ULedgerPanelWidget : public UAirportMgrPanelWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UVerticalBox> RowColumn;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> BalanceText;

	/** Distance from the top of the screen for the code-built card. Below the inbox, which
	 *  owns the top-right corner and must not be covered by something optional. */
	UPROPERTY(EditAnywhere, Category = "Ledger|Style") float TopOffset = 12.0f;

	/** Column widths, uu, so the four fields line up down the panel rather than ragging. */
	UPROPERTY(EditAnywhere, Category = "Ledger|Style") float WhenWidth = 110.0f;
	UPROPERTY(EditAnywhere, Category = "Ledger|Style") float CategoryWidth = 90.0f;
	UPROPERTY(EditAnywhere, Category = "Ledger|Style") float AmountWidth = 90.0f;

	ULedgerPanelViewModel* GetPanel() const { return Panel; }

	/** Whether the card is showing. The bar's button reads this to light itself. */
	bool IsShowing() const { return bShowing; }

	/** Open or close it. Called by the game.ledger action - see BuildActions. */
	void Toggle();

	/**
	 * Re-read the ledger and repaint. What NativeTick calls, and what a headless test calls
	 * directly - a test has no viewport to paint in, the same seam the inbox uses.
	 */
	void Refresh();

	/** How many row widgets are built, as opposed to how many the viewmodel holds. */
	int32 RowWidgetCountForTest() const;

	/** Paint from the viewmodel without a tick, for a headless test. */
	void PaintRowsForTest() { PaintRows(); }

protected:
	virtual void BuildOnce(const UUIStyle& Style) override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UPROPERTY() TObjectPtr<ULedgerPanelViewModel> Panel;

	/** The card itself, so Toggle can hide it without hiding the widget's root - see
	 *  UAirportMgrPanelWidget::BuildOnce on why the root stays SelfHitTestInvisible. */
	UPROPERTY() TObjectPtr<UWidget> Card;

	/** Cached for Refresh, which runs every tick: resolving the style there would be a
	 *  synchronous asset load per frame. The same reason UInspectorWidget caches it. */
	const UUIStyle* CachedStyle = nullptr;

	bool bShowing = false;

	/** One row: when, category, what, amount. */
	UWidget* BuildRow(const UUIStyle& Style, const ULedgerRowViewModel& Row);

	void EnsureSlots(const UUIStyle* Style);
	void PaintRows();
};
