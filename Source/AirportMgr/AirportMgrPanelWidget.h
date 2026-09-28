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
 * Controller() and EnsureCardRoot() are the other two things duplicated across two or more of
 * the four: a click-driven panel asks the same question about who is playing, and a floating
 * corner card builds the same canvas-root-plus-bordered-card skeleton before it ever reaches
 * its own content.
 *
 * NOT ALL FOUR USE EnsureCardRoot. UBuildBarWidget's chrome is a full-width bar stretched by
 * OFFSETS across two differently-coloured rows, and UToastStackWidget's root holds a bare
 * VerticalBox with no card at all (each toast is its own rounded card) - neither shape is the
 * single anchored-and-auto-sized card EnsureCardRoot builds. Both still sit on this base for
 * Initialize/BuildOnce/Controller; only UInspectorWidget and UOfferInboxWidget call
 * EnsureCardRoot.
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
	 * ticks a widget from its paint pass, so a Collapsed widget is never arranged, is never
	 * painted, and so never ticks - and the tick is the only thing that could later un-collapse
	 * it (PIE 2026-09-07: a panel built this way, never shown). SelfHitTestInvisible keeps the
	 * panel laid out and running while staying click-transparent, so an otherwise-empty panel
	 * does not sit over the world as an invisible pane that eats the player's clicks. Individual
	 * pieces of content (the inspector's card, an offer row) still hide themselves normally;
	 * this is about the PANEL's own root, once, not about them.
	 */
	virtual void BuildOnce(const UUIStyle& Style) PURE_VIRTUAL(UAirportMgrPanelWidget::BuildOnce, );

	/**
	 * The owning player when the controller created this widget; the first controller
	 * otherwise (tests create a panel straight from a world). Null is a supported state: a
	 * panel still builds, and whatever polls this simply has nothing to ask.
	 */
	ARoadBuildController* Controller() const;

	/**
	 * A canvas root (if the asset gave none) plus ONE bordered card, anchored, aligned and
	 * positioned as given, auto-sized to its content. Returns the panel INSIDE the card that
	 * a subclass adds its own rows to, or nullptr when an asset already supplied a root - in
	 * that case BindWidgetOptional has already filled every slot the asset supplies, and a
	 * code-built card would replace the designer's layout.
	 *
	 * Surface, ALWAYS: the card is the outer surface everything a subclass draws sits ON,
	 * and Panel is left free for whatever goes inside it (a button, an offer row) - the same
	 * split UBuildBarWidget's two rows and UOfferInboxWidget's offer cards already draw.
	 */
	UPanelWidget* EnsureCardRoot(FName CardName, const FAnchors& Anchors, FVector2D Alignment,
		FVector2D Position, bool bRounded);

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
	 * The card EnsureCardRoot built (or found by name on an asset-supplied root), so a
	 * subclass that shows/hides it need not re-walk the widget tree to ask again.
	 *
	 * issue #187: UInspectorWidget's ShowInspectorCard called WidgetTree->FindWidget (a
	 * recursive walk) once or twice EVERY tick just to toggle a Collapsed/Visible flag;
	 * ULedgerPanelWidget already found it once in EnsureSlots and held it for exactly this
	 * reason - this is that field, promoted so EnsureCardRoot itself can fill it for every
	 * caller. Null for a panel that never calls EnsureCardRoot (UBuildBarWidget,
	 * UToastStackWidget - see EnsureCardRoot's own comment).
	 */
	UPROPERTY() TObjectPtr<UWidget> CardWidget;

	/**
	 * Shows or hides CardWidget, doing nothing if it is already in the requested state.
	 *
	 * THE GATE ITSELF: SetVisibility has no early-out of its own (the same reason SetText is
	 * this issue's other half), so a panel polled every tick used to re-invalidate Slate's
	 * layout for a visibility that had not changed since the last frame. A no-op when CardWidget is
	 * null (EnsureCardRoot not called, or an asset root with no widget of that name), so a
	 * caller need not guard the call itself.
	 */
	void SetCardShown(bool bShown);

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
