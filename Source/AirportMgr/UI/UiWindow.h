#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/UiWindowSpec.h"
#include "UiWindow.generated.h"

class UUiButton;
class UUiWindowHost;
class UUIStyle;

/** What a held left button is doing to a window. A phase, not two bools. */
UENUM()
enum class EUiWindowGesture : uint8
{
	None,
	Move,
	Resize,
};

/**
 * The chrome around one panel (UI library step 2): shadow, white card, title bar with an optional
 * close, hairline, a scroll body holding the panel, and a resize grip. It turns mouse input into
 * HOST calls in host-local units and decides nothing about geometry itself - clamping, snapping,
 * z-order and docking are UUiWindowHost's, because each of them needs every window at once.
 */
UCLASS()
class AIRPORTMGR_API UUiWindow : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Builds the chrome around Content. Once, from UUiWindowHost::AddWindow. */
	void Build(const UUIStyle& Style, const FUiWindowSpec& Spec, UWidget& Content, UUiWindowHost& InHost);

	FName GetId() const { return Id; }

	/**
	 * The tallest this window may lay itself out, uu; 0 or less lifts the cap. The host sets it
	 * every tick for an AUTO-SIZED window from the room below its top edge, so a window that
	 * grows with its content (Offers) scrolls at the screen's edge instead of running off it.
	 */
	void SetMaxHeight(double MaxHeight);

	/** Folds the window to its title bar, or unfolds it. Visual only - the HOST owns the state and
	 *  the slot's size (UUiWindowHost::SetCollapsed); this hides the body and swaps the button. */
	void ShowCollapsed(bool bCollapsed);

	/** A short text right of the title - still readable while the window is folded (the offer count). */
	void SetBadge(const FText& Badge);
	FString BadgeForTest() const;

	/** The fold button. Public for the test that presses it. */
	UFUNCTION() void HandleCollapse();
	double MaxHeightForTest() const;

	/** The close button. Public for the test that presses it. */
	UFUNCTION() void HandleClose();

	EUiWindowGesture GestureForTest() const { return Gesture; }
	void BeginGestureForTest(EUiWindowGesture Kind, FVector2D HostLocal) { BeginGesture(Kind, HostLocal); }
	void MoveGestureForTest(FVector2D HostLocal) { UpdateGesture(HostLocal); }

protected:
	/** Any press on the window raises it - in the PREVIEW pass, because a press on the panel's
	 *  content is handled by the panel before it could bubble up to here. */
	virtual FReply NativeOnPreviewMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;

private:
	void BeginGesture(EUiWindowGesture Kind, FVector2D HostLocal);
	void UpdateGesture(FVector2D HostLocal);
	void EndGesture(const TCHAR* Why);

	UPROPERTY() TObjectPtr<UUiWindowHost> Host;
	UPROPERTY() TObjectPtr<UWidget> TitleBar;
	UPROPERTY() TObjectPtr<UWidget> Grip;
	/** Between the shadow frame and the card; see SetMaxHeight. */
	UPROPERTY() TObjectPtr<class USizeBox> HeightCap;
	/** What folding hides: the hairline under the title, and the scroll body. */
	UPROPERTY() TObjectPtr<UWidget> Rule;
	UPROPERTY() TObjectPtr<UWidget> Body;
	UPROPERTY() TObjectPtr<UUiButton> CollapseButton;
	UPROPERTY() TObjectPtr<class UTextBlock> BadgeText;
	bool bCollapsible = false;
	UPROPERTY() TObjectPtr<UUiButton> CloseButton;
	FName Id;
	EUiWindowGesture Gesture = EUiWindowGesture::None;
	FVector2D GestureStartMouse = FVector2D::ZeroVector;
	FVector2D GestureStartTopLeft = FVector2D::ZeroVector;
	FVector2D GestureStartSize = FVector2D::ZeroVector;
};
