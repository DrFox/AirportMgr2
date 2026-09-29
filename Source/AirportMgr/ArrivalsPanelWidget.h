#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"

#include "ArrivalsPanelWidget.generated.h"

class UArrivalsViewModel;
class UTextBlock;
class UUIStyle;
class UVerticalBox;

/**
 * The flights the player ACCEPTED, inbound and on the ground (spec 2026-09-28-arrival-queue
 * section 3).
 *
 * ITS OWN WINDOW, split out of the offer inbox 2026-09-29: the two lists answer different
 * questions - "what might I take" wants an answer and a countdown, "what did I take" is status -
 * and sharing one card made the inbox the tallest thing on screen. Beside Offers by default, and
 * foldable like it, so the player chooses how much of the right edge each gets.
 *
 * C++ base, Blueprint optional - the rule the other panels follow; BindWidgetOptional below.
 */
UCLASS()
class AIRPORTMGR_API UArrivalsPanelWidget : public UAirportMgrPanelWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UVerticalBox> ArrivalColumn;

	/** From the screen's top-right corner: LEFT of the Offers window, whose cards are
	 *  UOfferInboxWidget::RowWrapWidth + 20 wide plus the window's padding - about 470 on
	 *  2026-09-29. A default only; the player's own placement is remembered. */
	UPROPERTY(EditAnywhere, Category = "Arrivals|Style") FVector2D Offset = FVector2D(500.0, 12.0);

	/** Width floor, so the window does not jog as flight names change length. */
	UPROPERTY(EditAnywhere, Category = "Arrivals|Style") float MinWidth = 260.0f;

	UArrivalsViewModel* GetArrivals() const { return Arrivals; }

	/** How many rows are built, as opposed to how many the viewmodel holds. */
	int32 RowCountForTest() const { return Titles.Num(); }
	/** Paint from the viewmodel without a runtime - UOfferInboxWidget::PaintRowsForTest's seam. */
	void PaintRowsForTest() { if (PanelStyle != nullptr) { PaintRows(*PanelStyle); } }

	/** Top-right beside Offers; foldable and resizable, not closable - there is no bar button to
	 *  bring it back, and a flight the player took must stay findable. */
	virtual bool WantsWindow(FUiWindowSpec& Out) const override;

	/** Re-read the board and repaint - NativeTick's work, callable by a headless test. */
	void Refresh();

protected:
	virtual void BuildOnce(const UUIStyle& Style) override;
	virtual void TickPanel(float DeltaTime) override;

private:
	UPROPERTY() TObjectPtr<UArrivalsViewModel> Arrivals;

	/** Each row's three texts, HELD rather than found by child index - the rule UOfferRowEntry
	 *  states for the offer cards. Rebuilt when the row count changes. */
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> Titles;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> Statuses;
	UPROPERTY() TArray<TObjectPtr<UTextBlock>> Details;

	void PaintRows(const UUIStyle& Style);
};
