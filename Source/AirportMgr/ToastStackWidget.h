#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "Blueprint/UserWidget.h"
#include "Model/ArrivalPlanner.h"
#include "NotificationCentre.h"
#include "ToastStackWidget.generated.h"

class UBorder;
class UPanelWidget;
class UTexture2D;
class UUIStyle;

/**
 * A card and the entry it currently shows.
 *
 * PAIRED BY ID, not by array index. Notifications->Entries() is a queue - PostFeed appends at
 * the back, expiry and the MaxEntries cap only ever drop the FRONT - so Cards and Entries stay
 * index-aligned once trimmed, but the ID is what lets a tick tell "this card is still entry 7"
 * from "the list moved and this card is now showing a different entry", which a bare parallel
 * TArray<UBorder*> cannot say for itself. One struct, not two arrays that must be kept in step
 * by hand - see CLAUDE.md's "lists that must agree are one list".
 */
USTRUCT()
struct FToastCard
{
	GENERATED_BODY()

	UPROPERTY() int32 EntryId = INDEX_NONE;
	UPROPERTY() TObjectPtr<UBorder> Card;
};

/**
 * The feed surface: transient toasts, bottom right, above the bar.
 *
 * REPLACES A SINGLE UTextBlock ON THE BAR that every notification overwrote and nothing ever
 * cleared, so two events in the same second left only the second, permanently, and the first
 * was never seen at all. That the bar was also the notification surface is how it happened:
 * one widget driving the tools AND showing messages had no reason to keep more than one.
 *
 * The model is UNotificationCentre, which is world-free and carries every rule about what
 * stays and for how long. This widget is a FORWARDER - it subscribes, it ticks, and it draws
 * what the centre currently holds. No lifetime decision is taken here.
 */
UCLASS()
class AIRPORTMGR_API UToastStackWidget : public UAirportMgrPanelWidget
{
	GENERATED_BODY()

public:
	/** The column of toasts. Built in code when no asset supplies one. */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> ToastColumn;

	/**
	 * How far above the bottom edge the stack floats, uu.
	 *
	 * A FLOOR: the real offset clears the bar, whose height the bar itself derives from the
	 * style. Asking UBuildBarWidget rather than retyping a number is what stops the two
	 * drifting the first time ButtonSize changes and the toasts end up behind the bar.
	 */
	UPROPERTY(EditAnywhere, Category = "Toasts|Style") float BottomOffset = 16.0f;

	/** Card metrics. Wrap width is the one that stops a long refusal becoming a ribbon. */
	UPROPERTY(EditAnywhere, Category = "Toasts|Style") float ToastWrapWidth = 300.0f;
	UPROPERTY(EditAnywhere, Category = "Toasts|Style") float ToastIconSize = 22.0f;
	UPROPERTY(EditAnywhere, Category = "Toasts|Style") float ToastGap = 6.0f;
	UPROPERTY(EditAnywhere, Category = "Toasts|Style") float ToastOutlineWidth = 1.5f;

	/** The centre this draws. Public so a test can post to it without a world's event bus. */
	UNotificationCentre* Centre() const { return Notifications; }

	/**
	 * One frame of REAL seconds. Public because it is the seam NativeTick forwards to, and
	 * a seam that nothing tests is a seam that can be left unwired - see the refactor
	 * contract. AirportMgr.UI.ToastsSurviveAFastClock drives this directly.
	 */
	void TickFeed(float RealDeltaSeconds);

	int32 ToastCountForTest() const;

	/** The first card's brush, so a test can read the corner radius actually drawn. */
	bool FirstToastBrushForTest(struct FSlateBrush& OutBrush) const;

	/**
	 * How many cards have been CONSTRUCTED since the stack was built, as opposed to how many
	 * are currently showing. Issue #186: the whole tree used to be torn down and rebuilt every
	 * tick, so this is the counter that goes red if that regresses - N ticks of an unchanging
	 * feed must add nothing to it.
	 */
	int32 CardsConstructedForTest() const { return CardsConstructed; }

	/** The first card's own widget identity, so a test can tell "the same UBorder, unchanged"
	 *  from "a new UBorder that happens to look the same" - the brush alone cannot say that. */
	UBorder* FirstToastForTest() const;

protected:
	/** Builds the stack's chrome and subscribes to the ops runtime's events. See
	 *  UAirportMgrPanelWidget::Initialize for why this runs from Initialize. */
	virtual void BuildOnce(const UUIStyle& Style) override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UPROPERTY() TObjectPtr<UNotificationCentre> Notifications;

	/** Resolved once in BuildOnce, not re-resolved every tick - the pattern UInspectorWidget's
	 *  CachedStyle already uses. TObjectPtr, not a raw pointer, so the style asset stays a GC
	 *  root for as long as this widget is reachable. */
	UPROPERTY() TObjectPtr<const UUIStyle> CachedStyle;

	/** One icon per severity, resolved once in BuildOnce instead of a LoadSynchronous() every
	 *  tick per visible card - see IconFor. */
	UPROPERTY() TObjectPtr<UTexture2D> CachedIconInfo;
	UPROPERTY() TObjectPtr<UTexture2D> CachedIconSuccess;
	UPROPERTY() TObjectPtr<UTexture2D> CachedIconWarning;

	/**
	 * One card per live entry, index-aligned with Notifications->Entries() once SyncCards has
	 * trimmed the front. This is the whole fix for #186: a card is built once, on arrival, and
	 * every later tick only touches SetRenderOpacity on the ones that survive.
	 */
	UPROPERTY() TArray<FToastCard> Cards;

	int32 CardsConstructed = 0;

	void EnsureSlots(const UUIStyle* Style);

	/** Add and drop cards to match Notifications->Entries(), then set every survivor's
	 *  opacity for this frame. See the .cpp for why trimming from the front is exact rather
	 *  than a heuristic. */
	void SyncCards(const UUIStyle& Style);

	/** Builds the one card for a freshly-arrived entry. Everything about a toast except its
	 *  opacity is fixed at birth, so this is the only place that constructs Slate widgets. */
	UBorder* BuildCard(const UUIStyle& Style, const FNotificationEntry& Entry);

	/** The fade curve, read at construction and again every later tick. */
	float OpacityFor(const FNotificationEntry& Entry) const;

	/** Severity to palette slot. Static: it reads the style, not the widget. */
	static FLinearColor ColourFor(const UUIStyle& Style, ENotificationSeverity Severity);

	/** Severity to icon. Not static any more: the textures are resolved once per severity in
	 *  BuildOnce (see CachedIcon*) rather than LoadSynchronous()'d off the style every call. */
	UTexture2D* IconFor(ENotificationSeverity Severity) const;

	/** Both are FEED: they happened, they are worth knowing, and they need no decision. */
	UFUNCTION() void OnNotification(const FString& Text);
	UFUNCTION() void OnArrivalRefused(EArrivalRefusal Why);
};
