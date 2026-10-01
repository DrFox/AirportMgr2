#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "Blueprint/UserWidget.h"
#include "InspectorCards.h"
#include "Tool/Selection.h"
#include "UI/UiMenuButton.h"
#include "InspectorWidget.generated.h"

class ARoadBuildController;
class ARoadNetworkActor;
struct FBuildActionArg;
class UBuildBarWidget;
class UFlightBoard;
class UGroundTraffic;
class USimClock;
class UButton;
class UInspectorFacilityRows;
class UOpsRuntime;
class UPanelWidget;
class UUiButton;
class UTextBlock;
class UUIStyle;
struct FAgentFacts;

/**
 * The inspector: what the selected aircraft, runway, taxiway, stand or depot is doing, and the verbs for it.
 *
 * THE WIDGET PAINTS A VIEW (issue #441). What a selection SAYS is an IInspectorCard's (InspectorCards.h - one per card, each
 * with its own change key beside its own describe), what a depot's purchase rows show is UInspectorFacilityRows', and this class
 * is what is left: pick the card for the selection, ask it for its FInspectorCardView, paint the view - texts through the SetText
 * gate, the verbs the view names, the rows under it - and run the verbs' clicks. It used to be all of them in one 600-line Refresh,
 * with each card's outputs copied into members and restored by hand.
 *
 * The same recipe as UBuildBarWidget: a C++ base that builds a working panel with no asset,
 * and BindWidgetOptional slots a Widget Blueprint fills to restyle it. Set the Blueprint
 * as InspectorClass on the controller.
 *
 * POLLED each tick from InspectFacts, never subscribed: the bar's enabled states were
 * event-driven once and went stale, and a panel that shows speed needs every frame anyway.
 * WHAT IS ANNOUNCED IS THE SELECTION (HandleSelectionChanged, #446) - the one thing the owner knows: the panel paints whatever it is
 * handed each tick, and resets what a new card resets when the session says the selection changed, instead of diffing it with the last it saw.
 * It reads FAgentFacts / FStandFacts and never FRoadAgent, so M3's UFlight fills the same
 * struct and this file does not change.
 *
 * Its buttons run rows of BuildActions() by id, so the panel, the bar and the C key are one
 * list (spec §6.2).
 *
 * PanelTint/ButtonTint/DisabledTint/FontSize are GONE (issue #91) - EVERY COLOUR AND FONT
 * COMES FROM UUIStyle now, the rule UBuildBarWidget's own header already states. Only metrics
 * this panel alone needs (its width, how far it floats) stay as knobs.
 */
UCLASS()
class AIRPORTMGR_API UInspectorWidget : public UAirportMgrPanelWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	/** "Deadlocked with G-HDVK - the layout needs another way round or out", in Style->Warning; collapsed
	 *  unless the aircraft is in a ring UGroundTraffic::CurrentDeadlocks reports. */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> DeadlockText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> FactsText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> StatusText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UUiButton> DepartButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UUiButton> FollowButton;
	/** The runway card's one verb, "Use 27" - selection.runway_in_use (2026-09-28). */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UUiButton> RunwayButton;
	/** The runway's mode - selection.runway_use, beside the flip. */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UUiButton> RunwayUseButton;
	/** An agent card's escape hatch - selection.unstick, a popup of UAgentRescue's three actions
	 *  (spec 2026-09-29-unstick-agent). */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UUiMenuButton> UnstickMenu;
	/**
	 * "Show G-HDVK" - selects the agent this one waits for (ShowWaitedFor). Collapsed when it waits
	 * for nobody. NOT A BuildActions ROW, unlike the verbs beside it: its subject and caption exist
	 * only on this card (no key could name "whoever this aircraft waits for" from the bar), the
	 * Unstick popup's lines' reason. It reuses the alert Go's selection path instead.
	 */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UUiButton> WaitingForButton;

	/**
	 * THE DEPOT CARD'S PURCHASE ROWS (facility-upgrades spec §4) - a sub-widget of their own since issue #441, filled from the
	 * depot card's quote. The C++ base builds it asset-free; a Blueprint places a UInspectorFacilityRows under this name to
	 * restyle it.
	 */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UInspectorFacilityRows> FacilityRows;

	/**
	 * How long an agent must have stood behind something before the Unstick button lights up, s.
	 * NOT the deadlock resolver's StallSeconds (3 s), which every queue at a hold bar passes; a
	 * stranded agent lights it at once. A knob, so it is judged in play rather than here.
	 */
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") double UnstickHighlightSeconds = 15.0;

	UPROPERTY(EditAnywhere, Category = "Inspector|Style") double PanelWidth = 300.0;

	/**
	 * Gap between the card's bottom and the bar's TOP EDGE, wherever that edge is this frame.
	 *
	 * WAS BottomOffset = 72, a distance above the SCREEN's bottom (until 2026-09-27) - and the
	 * code-only bar is 118 uu tall before it grows, so the card sat over the bar's left-hand
	 * sections and hid them. Measured from the bar instead (see UUiWindowHost::DockAbove), so the card rides
	 * up and down as the bar grows and shrinks and never covers it.
	 */
	UPROPERTY(EditAnywhere, Category = "Inspector|Style") double BarGap = 12.0;

	/** Bottom-left, riding the bar's top edge BarGap above it until the player moves it. */
	virtual bool WantsWindow(FUiWindowSpec& Out) const override;

	/**
	 * Captions the Follow button "Unfollow" while bFollowing, its action's own label otherwise
	 * (2026-09-27: the same word for both states left the player guessing which one pressing
	 * it would give). NativeTick calls it with the controller's IsWatchingAgent; public so a
	 * headless test, which has no camera to watch with, can drive it.
	 */
	void ShowFollowing(bool bFollowing);

	/** The Follow button's caption as it reads now. */
	FString FollowCaptionForTest() const;

	/**
	 * Opens the Unstick popup if this is an agent card that shows it (the menu is visible and the window shown); otherwise nothing, and the
	 * request is spent. What UBuildHudLayer::OpenUnstickMenu calls - the row's Execute reaches it directly (#446) instead of leaving a count
	 * for TickPanel to notice. Public for that caller; a headless test drives it the same way.
	 */
	void OpenUnstickMenu();

	/** How many times OpenUnstickMenu has actually asked the popup to open - a headless anchor cannot open, so this is what a test reads. */
	int32 UnstickOpenCountForTest() const { return UnstickOpens; }

	/**
	 * THE SELECTION CHANGED (FBuildSession::OnSelectionChanged, via ARoadBuildController::OnSelectionChanged): what a new card resets -
	 * a window the player closed reopens (the close meant "not this one"), a popup for the old agent closes, the purchase rows' armed sale
	 * goes - and, when nothing is selected now, a sale armed on the old card is disarmed. THE EVENT'S, not this panel's diff of the
	 * selection it was handed every tick against the last it saw (#446): TickPanel and Refresh only PAINT what the selection is.
	 * Public for the controller's forward and for a headless test, which has no session to fire it.
	 * ENFORCED BY: AirportMgr.Inspector.SelectionEventReachesTheInspector
	 */
	void HandleSelectionChanged(const FSelection& Old, const FSelection& New);

	/** Whether the Unstick button is lit (Selected) - the agent looks stuck. */
	bool IsUnstickHighlightedForTest() const { return bUnstickHighlighted; }

	/** The lines the Unstick popup would show now, as UUiMenuButton::Items would give them. */
	TArray<FUiMenuItem> UnstickItemsForTest() const { return UnstickItems(); }

	/**
	 * The WaitingFor button's action: select - and move the camera to - the agent the shown one waits
	 * for, through the alert Go's own ARoadBuildController::SelectAndFocus, so the card opens on it.
	 * False when the card shows no wait, or the blocker has gone. Public, taking the controller, for
	 * the reason UAlertsPanelWidget::Go does: a headless test has no owning player to find it by.
	 * ENFORCED BY: AirportMgr.Inspector.HoldAndDeadlockLines
	 */
	bool ShowWaitedFor(ARoadBuildController& InController);

	/**
	 * The flight board registrations are read from, in place of the ops runtime's - a headless world
	 * has no game instance and so no runtime of its own. Weak: the test owns the board.
	 */
	void UseFlightBoardForTest(const UFlightBoard* Board);
	/** The game clock the hold duration converts by, in place of the ops runtime's - UseFlightBoardForTest's reason. */
	void UseClockForTest(const USimClock* Clock);

	/** The deadlock line, or empty while it is collapsed. */
	FString DeadlockForTest() const;
	/** The WaitingFor button's caption, or empty while it is collapsed. */
	FString WaitingForCaptionForTest() const;


	/**
	 * Re-reads the facts for Selection over Target and repaints. What NativeTick calls with
	 * the controller's target and selection; public so a headless test can drive it with
	 * no controller.
	 *
	 * PrecomputedAgentFacts is issue #187: NativeTick already asked the controller for the
	 * selected aircraft's FAgentFacts to answer the bar's selection.depart row, and passes the
	 * SAME struct here so this does not call InspectFacts::DescribeAgent a second time for one
	 * frame's one selection. Null (the default, and always null from a headless test with no
	 * controller) falls back to asking DescribeAgent itself, unchanged from before.
	 */
	void Refresh(const ARoadNetworkActor* Target, const FSelection& Selection,
		const FAgentFacts* PrecomputedAgentFacts = nullptr);

	/**
	 * Refresh's body past the ops runtime lookup - Runtime is what Refresh finds through OpsRuntime() (the world's, by
	 * OpsRuntimeResolver), null for none. A headless test's world has no game instance to hold the runtime, so the depot card, the
	 * fuel line and the turnaround were out of its reach; UOfferInboxWidget::RefreshWith is the same seam.
	 */
	void RefreshWith(const UOpsRuntime* Runtime, const ARoadNetworkActor* Target, const FSelection& Selection,
		const FAgentFacts* PrecomputedAgentFacts = nullptr);

	/** The cards this panel asks - their counters and the table itself, for the tests that pin the seam. */
	const FInspectorCards& CardsForTest() const { return Cards; }

	/** How many times a network card's Describe (InspectFacts::DescribeRunway/Taxiway/Stand) actually ran - the
	 *  FInspectorCardKey gate's counter: a quiet frame adds nothing. */
	int32 CardDescribeCountForTest() const { return Cards.NetworkDescribeCount(); }

	/** How many times the depot card asked UJobBoard::DescribeDepot - at most once a game minute on a quiet board. */
	int32 DepotDescribeCountForTest() const { return Cards.Depot().BacklogCount(); }

	/** How many times the depot card asked UOpsRuntime::QuoteFacility - once per card key, not once per tick (#441). */
	int32 QuoteCountForTest() const { return Cards.Depot().QuoteCount(); }

	/** How many times the aircraft card looked its flight up (UFlightBoard::FlightForAgent), asked the job board
	 *  for its fuel line, and composed its turnaround sentence - the three lookups PR E keys. */
	int32 FlightLookupCountForTest() const { return Cards.Aircraft().FlightLookupCount(); }
	int32 FuelLookupCountForTest() const { return Cards.Aircraft().FuelLookupCount(); }
	int32 TurnaroundComposeCountForTest() const { return Cards.Aircraft().TurnaroundComposeCount(); }

	bool IsShownForTest() const;
	bool IsDepartEnabledForTest() const;
	FString TitleForTest() const;

	/** The composed facts text (heading, speed, ... and the Demands block). */
	FString FactsForTest() const;
	/** The status line as shown. */
	FString StatusForTest() const;
	/** Depart's CAPTION colour - the thing that must actually change with enabled state.
	 *  See PaintVerbs: the button's own background stays Style->Control always (UUiButton::LookFor). */
	FLinearColor DepartLabelColourForTest() const;
	/** How many times Refresh actually called SetText on one of its three fields, as opposed
	 *  to how many times it was asked - the seam issue #187's gate is measured through: an
	 *  idle tick (same selection, same facts) must add nothing to this. */
	int32 SetTextCallCountForTest() const { return SetTextCalls; }

	/** How many times the aircraft card actually RECOMPOSED its Title/Facts/Status sentences
	 *  (the Printf/FString::Format work FAircraftDisplay gates), as opposed to how many times it
	 *  was asked - issue #309, one level upstream of SetTextCallCountForTest: an idle tick on a
	 *  PARKED aircraft (same selection, same rounded facts) must add nothing to this, even
	 *  though the earlier SetText gate already read as flat by comparing the strings this
	 *  count now stops building in the first place. */
	int32 ComposeCountForTest() const { return Cards.Aircraft().ComposeCount(); }

protected:
	/** Builds the panel's chrome and binds its two verbs. See
	 *  UAirportMgrPanelWidget::Initialize for why this runs from Initialize. */
	virtual void BuildOnce(const UUIStyle& Style) override;
	virtual void TickPanel(float DeltaTime) override;

private:
	bool bDepartEnabled = false;

	/**
	 * INDICES INTO BuildActions(), never ids to look up - the same reason UBuildBarEntry
	 * holds one. Found once, in EnsureSlots, by walking the Selection section positionally:
	 * BuildActions.cpp adds selection.depart then selection.follow so the panel, the bar and
	 * the C key stay one list (its own comment, spec §6.2) - the pair this panel needs is
	 * exactly those two rows, in that order.
	 */
	int32 DepartActionIndex = INDEX_NONE;
	int32 FollowActionIndex = INDEX_NONE;
	/** Found BY ID, not third-in-line: the pair above predates the rule that lists which must
	 *  agree check names, not positions (CLAUDE.md); a third positional row would have made a
	 *  reorder of the Selection section silently wire Depart's slot to the runway flip. */
	int32 RunwayActionIndex = INDEX_NONE;
	/** By id, RunwayActionIndex's rule. */
	int32 RunwayUseActionIndex = INDEX_NONE;
	/** By id, RunwayActionIndex's rule. Its row only opens the popup - see UBuildHudLayer::OpenUnstickMenu. */
	int32 UnstickActionIndex = INDEX_NONE;

	bool bUnstickHighlighted = false;

	/** See UnstickOpenCountForTest. */
	int32 UnstickOpens = 0;

	/** The popup's lines for the selected agent, one per EUnstickAction in enum order - HandleUnstickChosen
	 *  reads the line's index AS the action, so the two cannot disagree about which line is which. */
	TArray<FUiMenuItem> UnstickItems() const;

	/** THE CARDS, one of each kind - see InspectorCards.h. Per panel: each keeps what it last described from. */
	FInspectorCards Cards;

	/**
	 * What Title/Facts/Status last actually SET, so a repeat with nothing changed - the
	 * common case, since heading/speed/altitude only move while an agent is actually taxiing
	 * or flying and most ticks are "still parked" or "nothing selected" - calls SetText zero
	 * times (issue #187: SetText has no early-out of its own, same reason UBuildBarWidget
	 * gates its clock and balance). Compared against the COMPOSED string rather than a
	 * (selection id, phase) key: phase alone stays "Taxiing" or "Rolling / Climbing" for many
	 * seconds while heading, speed and altitude keep changing every one of them, and gating on
	 * phase would freeze those numbers mid-motion - a real behaviour change, not a saving.
	 */
	FString LastTitle, LastFacts, LastStatus;

	/** See SetTextCallCountForTest. */
	int32 SetTextCalls = 0;

	/** The deadlock line - SetText-gated like its siblings above. */
	FString LastDeadlock;

	/** The agent the shown card waits for, 0 for none - what ShowWaitedFor selects. */
	int32 WaitedForId = 0;

	/** See UseFlightBoardForTest. */
	TWeakObjectPtr<const UFlightBoard> FlightBoardForTest;

	/** UseFlightBoardForTest's board, else Runtime's (the one RefreshWith was handed); null when neither exists. */
	const UFlightBoard* Flights(const UOpsRuntime* Runtime) const;

	/** See UseClockForTest. */
	TWeakObjectPtr<const USimClock> ClockForTest;

	/** UseClockForTest's clock, else Runtime's (the one RefreshWith was handed); null when neither exists. */
	const USimClock* GameClock(const UOpsRuntime* Runtime) const;

	/** Whether the "no card for this kind" warning has been said for the selection now shown: once per selection, not per tick. Reset by HandleSelectionChanged. */
	bool bWarnedNoCard = false;

	void EnsureSlots(const UUIStyle* Style);
	void RunAction(int32 ActionIndex);
	/** RunAction with an argument - a parameterised verb's (FBuildAction::TryRunWith). The purchase rows run through it. */
	void RunActionWith(int32 ActionIndex, const FBuildActionArg& Arg);

	/** A selection that is not the last one: the window opens again, and everything armed for the old card goes. */
	void OnNewSelection();
	/** Nothing to show: hides the window and unlights Depart. */
	void HideCard();
	/** Paints View: the texts, the verbs it names, the purchase rows under it. */
	void PaintView(const FInspectorCardView& View, const FSelection& Selection, const ARoadNetworkActor& Target);
	void PaintTexts(const FInspectorCardView& View);
	void PaintVerbs(const FInspectorCardView& View, const FSelection& Selection, const ARoadNetworkActor& Target);

	UFUNCTION() void HandleDepart();
	UFUNCTION() void HandleFollow();
	UFUNCTION() void HandleRunway();
	UFUNCTION() void HandleRunwayUse();
	UFUNCTION() void HandleUnstickChosen(int32 Index);
	UFUNCTION() void HandleWaitingFor();
};
