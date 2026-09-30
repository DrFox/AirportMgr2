#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/UiLayoutStore.h"
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
	/** Folded to its title bar (FUiWindowSpec::bCollapsible). */
	bool bCollapsed = false;
	/** Whether it had a player-set size when it folded, and its size then - a folded window
	 *  auto-sizes to its title bar and holds this WIDTH; unfolding puts a player's size back. */
	bool bSizedWhenExpanded = false;
	FVector2D ExpandedSize = FVector2D::ZeroVector;
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
	/**
	 * Shows Id if hidden, hides it if shown, and returns whether it shows now - the ONE toggle a key, a bar button and a panel's own Toggle()
	 * all run (#447), for a window whose spec is bToggled. The state it flips is the host's (bWanted, bUserClosed), read back by IsShown:
	 * there is no second copy on the panel to keep in step. False for an unknown id.
	 */
	bool Toggle(FName Id);
	/** Clears a player's close, so the panel's next SetShown(true) shows it. */
	void ForgetDismissal(FName Id);
	/**
	 * The window's close button. Tells the panel FIRST, while it still shows (Settings' Cancel asks IsShown to know it has a session to revert),
	 * then hides it - plainly for a bToggled window, whose close is its toggle, and STICKING for any other (see FUiWindowSpec::bToggled).
	 */
	void CloseByPlayer(FName Id);
	void BringToFront(FName Id);

	/**
	 * Folds Id to its title bar, or unfolds it - for a window whose spec says bCollapsible, and
	 * nothing otherwise. Remembered with the layout (FUiWindowPlacement::bCollapsed).
	 */
	void SetCollapsed(FName Id, bool bCollapsed);
	bool IsCollapsed(FName Id) const;
	/** The text beside Id's title - what a panel shows that must survive a fold (UUiWindow::SetBadge). */
	void SetBadge(FName Id, const FText& Badge);

	/** The bar the AboveBarLeft windows ride, and whose live height is off-limits to all of them. */
	void DockAbove(const UBuildBarWidget* Bar);

	/** A drag or resize, in host-local units, clamped and snapped. Places the window first. */
	void MoveWindow(FName Id, FVector2D ProposedTopLeft);
	void ResizeWindow(FName Id, FVector2D ProposedSize);

	/** Where the layout is remembered; null (the default, and every test's) remembers nothing. */
	void SetLayoutStore(TSharedPtr<IUiLayoutStore> InStore);
	/** The player finished moving or resizing Id: remember where it is. Once per gesture. */
	void CommitPlacement(FName Id);
	/** "Reset window layout": forget everything and put every window back where it starts. */
	void ResetLayout();

	/** A modal window is showing - its scrim is up and the game's keys wait (the controller asks). */
	bool IsModalOpen() const;

	/** The window's rectangle now, host-local. Empty box for an unknown id. */
	FBox2D WindowRect(FName Id) const;
	FVector2D ToLocal(FVector2D ScreenPosition) const;

	void TickForTest(float DeltaTime) { TickWindows(DeltaTime); }
	void SetViewSizeForTest(FVector2D Size) { ViewSize = Size; }
	UUiWindow* WindowForTest(FName Id) const;
	UUserWidget* ScrimForTest() const { return Scrim; }
	int32 ZOrderForTest(FName Id) const;
	/** An AboveBarLeft window's distance above the screen's bottom, as its slot has it. */
	double WindowClearanceForTest(FName Id) const;
	const UBuildBarWidget* DockedBarForTest() const { return DockBar; }

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	/** The spec's anchor, offset and auto-size - where a window starts, and where a reset puts it. */
	void ApplyDefaultPlacement(FUiWindowEntry& E);
	/** Applies each saved placement that still fits this view; the rest keep their default. */
	void RestoreSavedLayout();
	FUiWindowEntry* Find(FName Id);
	const FUiWindowEntry* Find(FName Id) const;
	void Apply(FUiWindowEntry& E);
	/** Shows the scrim under a modal that just opened (and raises both), hides it when none is. */
	void UpdateScrim(const FUiWindowEntry& Changed);
	void TickWindows(float DeltaTime);
	/** Folds a window's slot to top-left anchoring at its current rectangle. */
	void Place(FUiWindowEntry& E);
	/** SetCollapsed's work without the write - for a restore, which must not write back what it read.
	 *  WidthHint: the width to hold folded when the window cannot measure it now (a restore before
	 *  first paint); 0 measures it. */
	void FoldWithoutCommit(FUiWindowEntry& E, bool bCollapsed, double WidthHint = 0.0);
	FVector2D TopLeftOf(const FUiWindowEntry& E) const;
	FVector2D SizeOf(const FUiWindowEntry& E) const;
	/** The tallest an auto-sized window may grow where it stands; see its body. */
	double MaxAutoHeight(const FUiWindowEntry& E) const;
	/** The screen minus the bar: where a window may be. */
	FBox2D Bounds() const;
	/** Every OTHER shown window's rectangle - hidden ones are not snap targets. */
	TArray<FBox2D> OthersThan(FName Id) const;
	double BarHeight() const;

	UPROPERTY() TArray<FUiWindowEntry> Windows;
	UPROPERTY() TObjectPtr<UCanvasPanel> Canvas;
	/** Under the modal window, over everything else; Collapsed while no modal shows. */
	UPROPERTY() TObjectPtr<UUserWidget> Scrim;
	UPROPERTY() TObjectPtr<const UBuildBarWidget> DockBar;
	UPROPERTY() TObjectPtr<const UUIStyle> Style;
	TSharedPtr<IUiLayoutStore> LayoutStore;
	/** The saved layout is judged once: when the view's real size is known AND the bar's height has
	 *  settled - see TickWindows. All windows are added before the first tick (UBuildHudLayer::
	 *  CreateAll, 2026-09-28), so judging once covers every window. */
	bool bLayoutRestored = false;
	/** The bar height last tick, and how many ticks the judgement has waited for it to settle. */
	double LastBarHeightSeen = -1.0;
	int32 RestoreWaitTicks = 0;
	FVector2D ViewSize = FVector2D(1920.0, 1080.0);
	int32 TopZ = 0;
};
