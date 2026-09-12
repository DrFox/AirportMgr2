#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "Blueprint/UserWidget.h"
#include "Model/ArrivalPlanner.h"
#include "NotificationCentre.h"
#include "ToastStackWidget.generated.h"

class UPanelWidget;
class UUIStyle;

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

protected:
	/** Builds the stack's chrome and subscribes to the ops runtime's events. See
	 *  UAirportMgrPanelWidget::Initialize for why this runs from Initialize. */
	virtual void BuildOnce(const UUIStyle& Style) override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UPROPERTY() TObjectPtr<UNotificationCentre> Notifications;

	void EnsureSlots();
	void Rebuild(const UUIStyle& Style);

	/** Severity to palette slot, and to icon. Static: they read the style, not the widget. */
	static FLinearColor ColourFor(const UUIStyle& Style, ENotificationSeverity Severity);
	static UTexture2D* IconFor(const UUIStyle& Style, ENotificationSeverity Severity);

	/** Both are FEED: they happened, they are worth knowing, and they need no decision. */
	UFUNCTION() void OnNotification(const FString& Text);
	UFUNCTION() void OnArrivalRefused(EArrivalRefusal Why);
};
