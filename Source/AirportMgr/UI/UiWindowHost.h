#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/UiWindowSpec.h"
#include "UiWindowHost.generated.h"

class UAirportMgrPanelWidget;
class UBuildBarWidget;
class UCanvasPanel;
class UCanvasPanelSlot;
class UUiWindow;
class UUIStyle;

/** One window the host owns. */
USTRUCT()
struct FUiWindowEntry
{
	GENERATED_BODY()

	UPROPERTY() TObjectPtr<UUiWindow> Window;
	UPROPERTY() TObjectPtr<UAirportMgrPanelWidget> Panel;
	UPROPERTY() TObjectPtr<UCanvasPanelSlot> Slot;
	UPROPERTY() FUiWindowSpec Spec;
	/** The panel wants it shown. */
	bool bWanted = false;
	/** The player closed it; cleared when the panel hides it itself or forgets the close. */
	bool bUserClosed = false;
	/** The player moved or resized it: its slot is top-left anchored and no longer docks. */
	bool bPlaced = false;
};

/**
 * Every window, on one full-screen canvas at Z 1 (UI library step 2). The ONE object that sees
 * more than one window, which is why placement, z-order, snapping, clamping and the inspector's
 * dock above the bar all live here and not in UUiWindow.
 *
 * IT TICKS EVERY HOSTED PANEL ITSELF, hidden or not: a collapsed window stops Slate ticking its
 * contents, and a panel's tick is what decides to show it again.
 * ENFORCED BY: AirportMgr.UI.WindowHost.TicksHiddenPanelsOnce.
 *
 * All positions are in this widget's local units - its canvas slots' own space.
 */
UCLASS()
class AIRPORTMGR_API UUiWindowHost : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual bool Initialize() override;

	/** Wraps Panel in a window if Panel->WantsWindow says so; null (and nothing added) otherwise,
	 *  or when another window already has that id. */
	UUiWindow* AddWindow(UAirportMgrPanelWidget& Panel);

	void SetShown(FName Id, bool bShown);
	bool IsShown(FName Id) const;
	/** Clears a player's close, so the panel's next SetShown(true) shows it. */
	void ForgetDismissal(FName Id);
	/** The window's close button. Hides it and tells the panel (a toggled panel un-toggles). */
	void CloseByPlayer(FName Id);
	void BringToFront(FName Id);

	/** The bar the AboveBarLeft windows ride, and whose live height is off-limits to all of them. */
	void DockAbove(const UBuildBarWidget* Bar);

	/** A drag or resize, in host-local units, clamped and snapped. Places the window first. */
	void MoveWindow(FName Id, FVector2D ProposedTopLeft);
	void ResizeWindow(FName Id, FVector2D ProposedSize);

	/** The window's rectangle now, host-local. Empty box for an unknown id. */
	FBox2D WindowRect(FName Id) const;
	FVector2D ToLocal(FVector2D ScreenPosition) const;

	void TickForTest(float DeltaTime) { TickWindows(DeltaTime); }
	void SetViewSizeForTest(FVector2D Size) { ViewSize = Size; }
	UUiWindow* WindowForTest(FName Id) const;
	int32 ZOrderForTest(FName Id) const;
	/** An AboveBarLeft window's distance above the screen's bottom, as its slot has it. */
	double WindowClearanceForTest(FName Id) const;
	const UBuildBarWidget* DockedBarForTest() const { return DockBar; }

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	FUiWindowEntry* Find(FName Id);
	const FUiWindowEntry* Find(FName Id) const;
	void Apply(FUiWindowEntry& E);
	void TickWindows(float DeltaTime);
	/** Folds a window's slot to top-left anchoring at its current rectangle. */
	void Place(FUiWindowEntry& E);
	FVector2D TopLeftOf(const FUiWindowEntry& E) const;
	FVector2D SizeOf(const FUiWindowEntry& E) const;
	/** The screen minus the bar: where a window may be. */
	FBox2D Bounds() const;
	/** Every OTHER shown window's rectangle - hidden ones are not snap targets. */
	TArray<FBox2D> OthersThan(FName Id) const;
	double BarHeight() const;

	UPROPERTY() TArray<FUiWindowEntry> Windows;
	UPROPERTY() TObjectPtr<UCanvasPanel> Canvas;
	UPROPERTY() TObjectPtr<const UBuildBarWidget> DockBar;
	UPROPERTY() TObjectPtr<const UUIStyle> Style;
	FVector2D ViewSize = FVector2D(1920.0, 1080.0);
	int32 TopZ = 0;
};
