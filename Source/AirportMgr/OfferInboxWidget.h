#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "Blueprint/UserWidget.h"

#include "OfferInboxWidget.generated.h"

struct FAirlineOffers;
class UOfferGenerator;
class USimClock;
class ARoadNetworkActor;
class UButton;
class UUiButton;
enum class EUiButtonKind : uint8;
class UUiButton;
enum class EUiButtonKind : uint8;
class UListView;
class UProgressBar;
class USizeBox;
class UBorder;
class UHorizontalBox;
class UOfferInboxWidget;
class UOfferInboxViewModel;
class UOfferViewModel;
class UTextBlock;
class UUIStyle;
class UVerticalBox;
class UWidget;

/**
 * One row's two buttons, bound by index.
 *
 * A UObject per row for the same reason UBuildBarEntry is one: UButton::OnClicked is a
 * DYNAMIC delegate and binds only to a UFUNCTION on a UObject - a lambda cannot bind.
 */
UCLASS()
class UOfferRowEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 RowIndex = INDEX_NONE;
	UPROPERTY() TWeakObjectPtr<UOfferInboxWidget> Owner;

	/**
	 * The pieces of this row, HELD rather than found again by child index.
	 *
	 * The repaint used to reach back in with GetChildAt(0) for the label and GetChildAt(1)
	 * for the Accept button, so the layout and the repaint were two descriptions of the same
	 * tree that had to agree - insert one widget and the wrong thing gets the airline's name.
	 * Holding them makes the layout the only place that knows the shape.
	 */
	UPROPERTY() TObjectPtr<UTextBlock> AirlineText;
	UPROPERTY() TObjectPtr<UTextBlock> TypeText;
	UPROPERTY() TObjectPtr<UTextBlock> CountdownText;
	UPROPERTY() TObjectPtr<UProgressBar> CountdownBar;
	UPROPERTY() TObjectPtr<UTextBlock> ContractText;
	UPROPERTY() TObjectPtr<UTextBlock> FuelChip;
	UPROPERTY() TObjectPtr<UTextBlock> TugChip;
	UPROPERTY() TObjectPtr<UTextBlock> RefusalText;
	UPROPERTY() TObjectPtr<UUiButton> AcceptButton;

	UFUNCTION() void HandleAccept();
	UFUNCTION() void HandleDecline();
};

/**
 * The offer inbox: what the airlines are asking for, and the two answers.
 *
 * C++ BASE, BLUEPRINT OPTIONAL - the rule the build bar and the inspector already follow.
 * The code builds a plain panel when no asset supplies one, so a missing Blueprint degrades
 * the look rather than breaking the feature.
 *
 * TWO LIST PATHS, ONE VIEWMODEL. If a Widget Blueprint supplies a UListView, the rows are
 * its entry widgets, handed their UOfferViewModel through UMG's own IUserObjectListEntry -
 * not ModelViewViewModel (issue #191 dropped that dependency: no such Blueprint exists in
 * Content/UI, and the viewmodel is a plain UObject now, see OfferViewModels.h). Without a
 * UListView, the code builds a vertical box of rows itself. A UListView cannot be built
 * usefully in code here because its entry widget class is a Blueprint asset, and
 * virtualisation only earns its keep at hundreds of rows - the inbox has a handful.
 *
 * TO RESTYLE IN THE DESIGNER: make a Widget Blueprint with this class as parent, name the
 * widgets to match the BindWidgetOptional members below, and set the list's entry widget
 * class. A RENAMED VIEWMODEL FIELD NEEDS THE BLUEPRINT RECOMPILED AND RESAVED, or the old
 * getter calls run against the new class - the stale-Blueprint trap, in a new place.
 */
UCLASS()
class AIRPORTMGR_API UOfferInboxWidget : public UAirportMgrPanelWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UListView> OfferList;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UVerticalBox> OfferColumn;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> BadgeText;

	// ARRIVALS moved to their own window, UArrivalsPanelWidget (2026-09-29) - see its comment.

	/**
	 * Distance from the TOP of the screen for the code-built card.
	 *
	 * TOP RIGHT, moved from bottom right (spec section 6.2). Two things forced it: the feed
	 * now owns the bottom-right corner, and the two-row bar is 138 uu tall against the 56 it
	 * was, so the old 120 uu bottom offset put the card behind it. An offer must never be
	 * the thing that scrolls away or hides - missing one costs money.
	 */
	UPROPERTY(EditAnywhere, Category = "Inbox|Style") float TopOffset = 12.0f;

	UOfferInboxViewModel* GetInbox() const { return Inbox; }

	/** Top-right; NOT closable - an offer must never be hidden. Resizable and foldable since
	 *  2026-09-29 (the player asked): the count rides in the title badge, so a fold hides the
	 *  cards but never the fact that offers are waiting. */
	virtual bool WantsWindow(FUiWindowSpec& Out) const override;

	/**
	 * Re-read the board and repaint. What NativeTick calls, and what a headless test calls
	 * directly - the tick-to-Refresh seam is one line, and a test has no viewport to paint in.
	 */
	void Refresh(ARoadNetworkActor* Target);

	/** Called by a row's entry object. Public because UOfferRowEntry is a separate UObject. */
	void AcceptRow(int32 RowIndex);
	void DeclineRow(int32 RowIndex);

	/** How many offer CARDS are built, as opposed to how many the viewmodel holds. */
	int32 RowWidgetCountForTest() const;

	/** Paint from the viewmodel without a tick. A headless test never paints, so NativeTick
	 *  never runs - the same seam UInspectorWidget's test uses. */
	void PaintRowsForTest() { PaintRows(); }
	/** The count beside the heading, as it reads now. */
	FString BadgeForTest() const;

	/**
	 * Resample the demand strip IF ITS INPUTS MOVED - the fee's demand factor, each airline's own factor
	 * (satisfaction), the airline count - and set the "now" slot. True when it resampled. The strip used to
	 * resample 24 hours every frame (ops bus survey 2026-09-29); these inputs change a few times a game day.
	 * A KEY, NOT EVENTS ALONE: a load changes them without announcing it, and the key costs a few compares.
	 * ENFORCED BY: AirportMgr.UI.OfferInbox.DemandStripResamplesOnlyOnChange
	 */
	bool RefreshDemand(TArrayView<const FAirlineOffers> Airlines, const USimClock& Clock, const UOfferGenerator* Generator);

	int32 DemandSampleCountForTest() const { return DemandSampleCount; }
	/** Row N's Accept button, or null - see AirportMgr.UI.OfferInbox's Primary-kind assertion. */
	const UUiButton* AcceptButtonForTest(int32 Row) const;

	// NativeTickForTest is the panel base's now: it lets a headless test prove an idle tick
	// resolves no style (issue #309, closes the #260 item). It needs Controller() to reach Refresh
	// at all; PaintRowsForTest above skips straight to PaintRows for a test with no controller.

	/** Where the refusal sentence wraps, uu. The card is sized from this: its floor is this plus
	 *  the card's padding. 420, not 260: two lines per card merged into their neighbours
	 *  (BuildRow) and need the width (2026-09-29). */
	UPROPERTY(EditAnywhere, Category = "Inbox|Style") float RowWrapWidth = 420.0f;

	/** Gap between offer cards, so two offers do not read as one. */
	UPROPERTY(EditAnywhere, Category = "Inbox|Style") float RowGap = 6.0f;

	/** The demand strip's tallest bar, uu. */
	UPROPERTY(EditAnywhere, Category = "Inbox|Style") float DemandStripHeight = 22.0f;

	/** Seconds left at which the countdown turns amber, and at which it turns red and pulses. */
	UPROPERTY(EditAnywhere, Category = "Inbox|Style") float CountdownAmberSeconds = 30.0f;
	UPROPERTY(EditAnywhere, Category = "Inbox|Style") float CountdownUrgentSeconds = 10.0f;

protected:
	/** Builds the inbox's chrome. See UAirportMgrPanelWidget::Initialize for why this runs
	 *  from Initialize rather than NativeOnInitialized. */
	virtual void BuildOnce(const UUIStyle& Style) override;
	virtual void TickPanel(float DeltaTime) override;

private:
	UPROPERTY() TObjectPtr<UOfferInboxViewModel> Inbox;
	/** One offer card: airline and countdown, airframe, refusal, then the two answers. */
	UWidget* BuildRow(const class UUIStyle& Style, UOfferRowEntry& Entry, int32 Index);

	/** A rounded Accept or Decline. See its body for why the ROUNDING goes on the style. */
	UUiButton* MakeAnswerButton(const class UUIStyle& Style, const TCHAR* Name, const FText& Label,
		EUiButtonKind Kind, int32 Index);
	UPROPERTY() TArray<TObjectPtr<UOfferRowEntry>> Entries;

	/**
	 * The demand strip: one bar per hour, heights from UOfferInboxViewModel::SampleDemand.
	 *
	 * BARS BUILT FROM PLAIN WIDGETS, not a NativePaint class of its own: 24 size boxes are the
	 * whole of it, and a painter would be a second widget class to style and test for the same
	 * picture. Code-built path only, like the rows.
	 */
	UPROPERTY() TArray<TObjectPtr<USizeBox>> DemandBars;
	UPROPERTY() TArray<TObjectPtr<UBorder>> DemandFills;

	/** Samples for the strip, set by Refresh from the runtime's airlines. Empty = no strip. */
	TArray<double> DemandSamples;
	/** Which strip slot "now" falls in, or INDEX_NONE. */
	int32 DemandNowSlot = INDEX_NONE;
	/** Which slots are night, for their colour. */
	TArray<bool> DemandNight;

	/** RefreshDemand's key - see there. */
	double DemandKeyFee = -1.0;
	TArray<double> DemandKeyFactors;
	int32 DemandSampleCount = 0;

	void PaintDemand(const UUIStyle& Style);

	void EnsureSlots(const UUIStyle* Style);
	void PaintRows();
};
