#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/UiWindowSpec.h"
#include "Widgets/Layout/Anchors.h"
#include "AirportMgrPanelWidget.generated.h"

class ARoadBuildController;
class UPanelWidget;
class UUIStyle;
class UUiWindowHost;
class UWidget;

/**
 * Shared base for the code-built HUD panels: the bar, the inspector, the offer inbox, the
 * toast stack.
 *
 * A TEMPLATE METHOD, not four copies of the same guard. Every one of the four used to repeat,
 * byte-for-byte: `Super::Initialize(); if (!bOk||bBuilt||RF_ClassDefaultObject||WidgetTree==
 * nullptr) return bOk; bBuilt=true; EnsureSlots();` (issue #90). Initialize() is sealed here
 * so that guard exists in exactly one place; a subclass builds its content in BuildOnce,
 * called exactly once, with the resolved style already in hand so it never has to ask whether
 * ResolveStyle() came back null (it never does - see UAirportMgrUISettings::ResolveStyle).
 *
 * Controller() and EnsureContentRoot() are the other two things duplicated across two or more of
 * the four: a click-driven panel asks the same question about who is playing, and a floating
 * panel builds the same root column before it ever reaches its own content.
 *
 * FLOATING PANELS ARE WINDOWS (UI library step 2): the inspector, inbox, ledger and Land panel
 * describe their window in WantsWindow and UUiWindowHost wraps them - the card, its placement
 * and its chrome are the window's, not the panel's. UBuildBarWidget (a full-width bar stretched
 * by OFFSETS) and UToastStackWidget (a bare VerticalBox, each toast its own card) are not windows;
 * both still sit on this base for Initialize/BuildOnce/Controller.
 */
UCLASS(Abstract)
class AIRPORTMGR_API UAirportMgrPanelWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/**
	 * Builds the panel right after the base class has bound the asset's slots.
	 *
	 * Initialize rather than NativeOnInitialized: UUserWidget::Initialize only calls the
	 * latter when a PLAYER CONTEXT is valid (or a Blueprint class opts in), so a panel created
	 * from a world with no player - which is what a headless test does - would never build
	 * and would silently pass through empty. Overriding the one call every creation path
	 * takes keeps the build unconditional. Moved here from each of the four subclasses, which
	 * carried this exact reasoning on an identical override (issue #90).
	 */
	virtual bool Initialize() override final;

	/**
	 * This panel's window, if it is one: fills Out and returns true. The default is "not a
	 * window" - the bar and the toast stack are fixed furniture, not something to drag.
	 */
	virtual bool WantsWindow(FUiWindowSpec& Out) const;

	/** Called once by UUiWindowHost::AddWindow; applies whatever SetShown already asked for. */
	void AttachToHost(UUiWindowHost& InHost, FName InId);

	/**
	 * One frame of this panel's own work. The HOST calls it for a hosted panel every frame, hidden
	 * or not - a collapsed window stops Slate ticking its contents, and the tick is what decides to
	 * show it again. NativeTick calls it only when there is no host.
	 * ENFORCED BY: AirportMgr.Panels.HostedPanelTicksOnce, AirportMgr.UI.WindowHost.TicksHiddenPanelsOnce.
	 */
	void RunPanelTick(float DeltaTime);

	/** The player pressed this panel's window's close. Default: nothing; a toggled panel un-toggles. */
	virtual void OnWindowClosedByPlayer();

	/** Whether the window shows: the host's answer when hosted, the last SetShown otherwise. */
	bool IsShown() const;

	/** Runs NativeTick with a throwaway geometry, so a headless test can drive a tick without a
	 *  viewport - the precedent of ARoadBuildController::PlayerTickForTest. Moved here from
	 *  UBuildBarWidget and UOfferInboxWidget, which each carried this exact one-liner. */
	void NativeTickForTest(float DeltaTime) { FGeometry G; NativeTick(G, DeltaTime); }

	/** How many times RunPanelTick has run - see HostedPanelTicksOnce. */
	int32 PanelTickCountForTest() const { return PanelTicks; }

protected:
	/** A subclass's per-frame work - what its NativeTick override used to do. See RunPanelTick. */
	virtual void TickPanel(float DeltaTime);

	/** Ask for this panel's window to show or hide. The host decides (a player's close sticks). */
	void SetShown(bool bShown);

	/** The text beside this panel's window title - survives a fold. Nothing when unhosted. */
	void SetWindowBadge(const FText& Badge);

	/** The host that owns this panel's window; null when unhosted (the bar, toasts, a bare test). */
	UUiWindowHost* GetWindowHost() const { return Host; }

	/** Clears a player's close, so the next SetShown(true) shows again - for a panel whose
	 *  content changed enough to deserve a second look (the inspector's new selection). */
	void ForgetPlayerClose();

	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

protected:
	/**
	 * Builds this panel's content. Called exactly once, right after WidgetTree is confirmed
	 * usable, with a style that is NEVER NULL - do here what each subclass's own EnsureSlots
	 * plus any one-time wiring (button bindings, initial visibility) used to do from
	 * Initialize() directly.
	 *
	 * A SUBCLASS WHOSE CONTENT CAN GO ENTIRELY EMPTY (the inspector with nothing selected, the
	 * offer inbox with no offers, the toast stack with nothing to show) must mark ITSELF
	 * `SetVisibility(ESlateVisibility::SelfHitTestInvisible)` here, never Collapsed: Slate only
	 * ticks a widget from its paint pass, so a Collapsed widget is never painted and never ticks
	 * - and the tick is the only thing that could later un-collapse it (PIE 2026-09-07: a panel
	 * built this way, never shown). A HOSTED panel is ticked by the host whatever its window's
	 * visibility (RunPanelTick) and hides through SetShown. Either way the root stays
	 * SelfHitTestInvisible, so an otherwise-empty panel does not sit over the world as an invisible
	 * pane that eats the player's clicks, nor eat clicks meant for its window's own chrome.
	 */
	virtual void BuildOnce(const UUIStyle& Style) PURE_VIRTUAL(UAirportMgrPanelWidget::BuildOnce, );

	/**
	 * The owning player when the controller created this widget; the first controller
	 * otherwise (tests create a panel straight from a world). Null is a supported state: a
	 * panel still builds, and whatever polls this simply has nothing to ask.
	 */
	ARoadBuildController* Controller() const;

	/**
	 * A root VerticalBox named ContentName, returned for the subclass to fill, when the asset gave
	 * no root; null when an asset supplied one - BindWidgetOptional has already filled every slot
	 * the asset supplies, and a code-built root would replace the designer's layout. Replaces
	 * EnsureCardRoot: the CARD - surface, padding, corners, placement - is the window's now
	 * (UUiWindow), not the panel's.
	 */
	UPanelWidget* EnsureContentRoot(FName ContentName);

	/**
	 * The style resolved ONCE, before BuildOnce runs - see Initialize(). NEVER NULL once set.
	 *
	 * issue #187: UInspectorWidget and ULedgerPanelWidget each carried their own private copy
	 * of exactly this ("resolving the style there would be a synchronous asset load per
	 * frame" - Refresh runs every tick), and UBuildBarWidget carried none, so it paid
	 * UAirportMgrUISettings::ResolveStyle() - a TSoftObjectPtr::LoadSynchronous - twice a tick
	 * from RefreshState and RefreshBalance. One field here is what the other two already knew
	 * to do, generalised so a fifth panel does not have to remember to invent it again.
	 *
	 * NAMED DIFFERENTLY FROM UInspectorWidget's OLD FIELD (CachedStyle) ON PURPOSE:
	 * UToastStackWidget (issue #186, PR #200, in flight alongside this one) independently
	 * added its OWN CachedStyle for the identical reason - proof the pattern generalises, but
	 * also a name UHT will not let a subclass shadow. THE FOLLOW-UP THIS COMMENT PROMISED
	 * LANDED with issue #309: Toast's CachedStyle is gone and TickFeed reads this field now.
	 */
	UPROPERTY() TObjectPtr<const UUIStyle> PanelStyle;


	/**
	 * A CLICK ON THE PANEL STOPS AT THE PANEL (2026-09-28): its card, a section's background, the
	 * gap between two buttons. Before this a press there started a road under the bar - a Border
	 * is hit-testable but handles no mouse event, so the press bubbled, unhandled, up to the
	 * game viewport and reached ARoadBuildController::OnPrimaryPressed as a click on the ground.
	 *
	 * REACHED ONLY OVER THE PANEL'S OWN PIXELS. Canvas panels, boxes and the user widget itself
	 * default to SelfHitTestInvisible (engine UMG constructors), so an event reaches this widget
	 * only through a Visible descendant under the pointer - never over the empty screen around
	 * it. A Blueprint that makes the ROOT Visible would make that screen the panel's too, which
	 * is why the root's own hit-testability is checked before eating anything.
	 *
	 * THE PRESS AND THE DOUBLE-CLICK STOP; THE RELEASE DOES NOT. The controller's click fires on
	 * RELEASE (ARoadBuildController::OnPrimaryReleased asks FBuildGesture what the press became),
	 * so a release whose press was eaten here finds no gesture and does nothing - while eating
	 * the release too would strand a drag that began on the ground and ended over the bar. A
	 * second click comes as a double-click, not a press, so that is stopped with it. The wheel
	 * is not touched - zoom over the bar still zooms.
	 * ENFORCED BY: AirportMgr.Panels.ChromeEatsClicks.
	 */
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

public:
	/**
	 * How many times Initialize has actually resolved PanelStyle and called BuildOnce - the
	 * bBuilt guard's own effect, made visible. Slate can call Initialize more than once on a
	 * widget (re-parenting a panel, say), and BuildOnce is a "called exactly once" contract:
	 * a second run would call WidgetTree->ConstructWidget again for content that already
	 * exists. Named for the count rather than a bool so a widget built twice by a future
	 * regression fails with "2", not merely "not 1" (issue #194: nothing measured this before).
	 */
	int32 BuildOnceCallCountForTest() const { return BuildOnceCalls; }

	/** See PanelStyle's own comment - never null once Initialize has run. */
	const UUIStyle* PanelStyleForTest() const { return PanelStyle; }

private:
	bool bBuilt = false;

	/** See BuildOnceCallCountForTest. */
	int32 BuildOnceCalls = 0;

	/** The host that owns this panel's window; null for the bar, toasts and a headless test. */
	UPROPERTY() TObjectPtr<UUiWindowHost> Host;
	FName WindowId;
	/** What the panel last asked for - applied to the host when it attaches, and what IsShown
	 *  answers when there is no host. */
	bool bShownRequested = false;
	/** See PanelTickCountForTest. */
	int32 PanelTicks = 0;
};
