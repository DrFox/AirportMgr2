#pragma once

#include "CoreMinimal.h"
#include "AirportMgrPanelWidget.h"
#include "Blueprint/UserWidget.h"
#include "Tool/Selection.h"
#include "Model/FacilityPurchases.h"
#include "UI/UiMenuButton.h"
#include "InspectorWidget.generated.h"

class ARoadBuildController;
class ARoadNetworkActor;
class UBuildBarWidget;
class UFlightBoard;
class UGroundTraffic;
class USimClock;
class UButton;
class UInspectorWidget;
class UOpsRuntime;
class UPanelWidget;
class UUiButton;
class UTextBlock;
class UUIStyle;
class UFlight;
class UJobBoard;
struct FAgentFacts;

/**
 * What Refresh needs to know has NOT changed to skip RECOMPOSING the aircraft Title/Facts/
 * Status sentences - issue #309. Refresh used to run every Printf/FString::Format in the
 * aircraft branch (four Printfs plus two NSLOCTEXT lookups) EVERY TICK regardless of whether
 * the facts had moved, and only gated the resulting SetText call (see LastTitle/LastFacts/
 * LastStatus) - so a parked aircraft awaiting dispatch rebuilt the same three sentences a tick
 * for a SetText that then did nothing.
 *
 * ROUNDED, not raw: HeadingRounded/SpeedTenthsRounded/AltitudeRounded each round to the FINEST
 * precision the composed sentence shows for that quantity (see Refresh's own Printf specifiers),
 * so two facts that would compose to the IDENTICAL string never miss this cheaper equality check
 * first - the same reasoning LastTitle/LastFacts/LastStatus's own comment gives for comparing
 * composed text over a bare (id, phase) key, one step earlier.
 *
 * SpeedTenthsRounded is TENTHS OF m/s, not whole knots, even though the sentence prints BOTH
 * from the same Shown value: m/s prints to one decimal place (%.1f) and knots to zero (%.0f),
 * and 1 kt is 0.514 m/s - finer than a whole knot - so keying on the coarser knots figure alone
 * left a real change (3.4 -> 3.5 m/s, same 7 kt) uncomposed and the m/s line stale on screen.
 * PR #329 review caught this: whichever of a quantity's several displayed roundings is FINEST is
 * the one the key must use, since it changes at least as often as every coarser sibling derived
 * from the same raw value.
 *
 * Phase HOLDS F.Status (the exact string the Status line prints), not F.Phase (the enum): the
 * whole point named in that same comment is that phase ALONE (Taxiing, Rolling) sits still for
 * many seconds while heading/speed/altitude keep moving, so gating on the enum here would
 * freeze this struct's equality while the sentence it stands for kept changing underneath it.
 *
 * AIRCRAFT ONLY, not the stand/depot branch: issue #309's evidence cites this file's aircraft
 * composition (InspectorWidget.cpp:163-190, :198) and nothing about the stand branch, which
 * composes far less per call (one Printf, no NSLOCTEXT sentence) and is not what was reported -
 * gating it too would be scope the issue did not ask for.
 */
struct FInspectorKey
{
	int32 Id = INDEX_NONE;
	FString Phase;
	int32 HeadingRounded = 0;
	/** Tenths of m/s, not whole knots - see the class comment on why the finer of the two
	 *  displayed roundings is the one that must gate composition. */
	int32 SpeedTenthsRounded = 0;
	int32 AltitudeRounded = 0;
	FString Destination;
	bool bEngineRunning = false;
	FString Fuel;
	FString Pushback;
	FString Turnaround;
	/**
	 * The NAMES the title, hold and deadlock lines print (2026-09-30) - the registration and airline
	 * off the flight board, the blocker's and ring partners' names. Keyed RAW, like Fuel: looked up
	 * before the gate, composed behind it. The stall clock is Waited, below.
	 */
	FString Registration;
	FString Airline;
	FString Blocker;
	FString Partners;
	/** The hold's game-time duration as printed ("12 min") - it moves while Phase, since the hold
	 *  line left Airside without a figure, does not. */
	FString Waited;

	bool operator==(const FInspectorKey& Other) const
	{
		return Id == Other.Id && Phase == Other.Phase && HeadingRounded == Other.HeadingRounded
			&& SpeedTenthsRounded == Other.SpeedTenthsRounded && AltitudeRounded == Other.AltitudeRounded
			&& Destination == Other.Destination && bEngineRunning == Other.bEngineRunning
			&& Fuel == Other.Fuel && Pushback == Other.Pushback && Turnaround == Other.Turnaround
			&& Registration == Other.Registration && Airline == Other.Airline && Blocker == Other.Blocker
			&& Partners == Other.Partners && Waited == Other.Waited;
	}
	bool operator!=(const FInspectorKey& Other) const { return !(*this == Other); }
};

/**
 * One fleet row's Sell click - UBuildBarEntry's reason: a dynamic delegate binds only to a UFUNCTION on a
 * UObject. The first click ARMS (the caption asks again), the second sells: selling cannot be undone
 * (memory: destructive gestures need a deliberate mode).
 */
UCLASS()
class AIRPORTMGR_API UInspectorFleetRow : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 VehicleId = 0;
	UPROPERTY() TObjectPtr<UTextBlock> Line;
	UPROPERTY() TObjectPtr<UUiButton> SellButton;
	UPROPERTY() TWeakObjectPtr<UInspectorWidget> Owner;
	bool bArmed = false;

	UFUNCTION() void HandleSell();
};

/**
 * What a NETWORK card - runway, taxiway, stand, depot - was last described from (ops batch 3 PR E). Its Describe
 * walked the network every tick; now it runs when one of these moves, and the card is repainted from what it said.
 *
 * EVERY INPUT, BY CARD (checked against InspectFacts.cpp and UJobBoard::DescribeDepot, 2026-09-30):
 *  - all three read segments, nodes and profiles (EditRevision - every node, segment and profile mutator bumps it,
 *    a drag included) and runway facts, entities, the stored restriction letters and the guideline graph. In play
 *    those four change only through the facade, whose Topology notify rebuilds the guideline graph - every derived
 *    edge removed and re-made - so GuidelineRevision moves for each. The network OBJECT too: a clear or a load is a
 *    new one, counting from its own zero.
 *  - a STAND also reads who holds its pose node and whether that agent is parked: OccupancyRevision (goal claims,
 *    holds, every phase change) and UGroundTraffic::StandHoldChangeCount (a body on or off the pose, which moves no
 *    revision). Runway and taxiway cards read no occupancy, so they leave these zero and a moving airport does not
 *    recompose them.
 *  - a DEPOT reads its vehicles and their jobs (UJobBoard::Revision) and the clock, which the card reads AT THE
 *    MINUTE (Now floored to 60 s) so its text is a function of Minute exactly: DescribeDepot rounds its minutes
 *    from Now, and a key on the minute alone would have held a figure that moved mid-minute. Its PURCHASE ROWS
 *    and "No vehicles" (UOpsRuntime::QuoteFacility - the balance, the fleet, the sheds) are NOT keyed: they are
 *    asked every tick and laid over the kept card, since the balance moves with no revision this key holds.
 * ENFORCED BY: AirportMgr.Inspector.Cache.* - one test per input, each red when its field is left out of ==.
 */
struct FInspectorCardKey
{
	ESelectionKind Kind = ESelectionKind::None;
	int32 Id = INDEX_NONE;
	const void* Network = nullptr;
	uint32 EditRevision = 0;
	uint32 GuidelineRevision = 0;
	/** A stand's only - see the class comment. */
	const void* Traffic = nullptr;
	uint32 OccupancyRevision = 0;
	uint32 StandHolds = 0;
	/** A depot's only - see the class comment. */
	const void* JobBoard = nullptr;
	uint32 JobRevision = 0;
	int64 Minute = 0;

	bool operator==(const FInspectorCardKey& Other) const
	{
		return Kind == Other.Kind && Id == Other.Id && Network == Other.Network && EditRevision == Other.EditRevision
			&& GuidelineRevision == Other.GuidelineRevision && Traffic == Other.Traffic
			&& OccupancyRevision == Other.OccupancyRevision && StandHolds == Other.StandHolds
			&& JobBoard == Other.JobBoard && JobRevision == Other.JobRevision && Minute == Other.Minute;
	}
	bool operator!=(const FInspectorCardKey& Other) const { return !(*this == Other); }
};

/**
 * The inspector: what the selected aircraft or stand is doing, and the verbs for it.
 *
 * The same recipe as UBuildBarWidget: a C++ base that builds a working panel with no asset,
 * and BindWidgetOptional slots a Widget Blueprint fills to restyle it. Set the Blueprint
 * as InspectorClass on the controller.
 *
 * POLLED each tick from InspectFacts, never subscribed: the bar's enabled states were
 * event-driven once and went stale, and a panel that shows speed needs every frame anyway.
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
	/** "Deadlocked with G-HDVK - the layout needs another way round", in Style->Warning; collapsed
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
	 * THE DEPOT CARD'S PURCHASE ROWS (facility-upgrades spec §4), all filled from ShowFacilityQuote. The C++
	 * base builds them asset-free; a Blueprint restyles through these names.
	 */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> ShedsRow;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ShedsText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UUiButton> BuyModuleButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> VehiclesRow;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> VehiclesText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UUiMenuButton> BuyVehicleMenu;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> FleetList;

	/**
	 * Render the purchase rows from Quote and NOTHING ELSE - the spec's rule that the card cannot disagree
	 * with the rules. A quote that is no facility collapses them. Public so a headless test (no runtime,
	 * no controller) can drive it; Refresh calls it every tick with the selected depot's quote.
	 * ENFORCED BY: AirportMgr.Inspector.FacilityCardRendersTheQuote
	 */
	void ShowFacilityQuote(const FFacilityQuote& Quote);

	/** The depot status line: "No vehicles — buy one" for an empty depot on a road, else Current. */
	static FString DepotStatus(const FFacilityQuote& Quote, bool bReachable, const FString& Current);

	/** A fleet row's click - see UInspectorFleetRow. */
	void OnFleetSell(UInspectorFleetRow& Row);

	bool AreFacilityRowsShownForTest() const;
	FString ShedsTextForTest() const;
	FString VehiclesTextForTest() const;
	bool IsBuyModuleEnabledForTest() const;
	FString BuyModuleCaptionForTest() const;
	TArray<FUiMenuItem> BuyVehicleItemsForTest() const { return BuyVehicleItems(); }
	int32 FleetRowCountForTest() const { return FleetRows.Num(); }
	bool IsSellEnabledForTest(int32 Row) const;
	/** The card's clicks, raised through each widget's OWN delegate so an unbound button fails the test. */
	void ClickSellForTest(int32 Row);
	FString SellCaptionForTest(int32 Row) const;
	void ClickBuyModuleForTest();
	void ChooseBuyVehicleForTest(int32 Line);

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
	 * has no game instance and so no UOpsRuntimeSubsystem. Weak: the test owns the board.
	 */
	void UseFlightBoardForTest(const UFlightBoard* Board);
	/** The game clock the hold duration converts by, in place of the ops runtime's - UseFlightBoardForTest's reason. */
	void UseClockForTest(const USimClock* Clock);

	/**
	 * A stall (FAgentHold::StalledSeconds - movement seconds, real x speed) as GAME seconds, the
	 * unit the turnaround line beside it counts in: x the day's compression at Clock's time of day.
	 * ENFORCED BY: AirportMgr.Inspector.HoldAndDeadlockLines (a known stall at 72x reads 1 h 36 min)
	 */
	static double GameSecondsOfStall(double StalledSeconds, const USimClock& Clock);

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
	 * Refresh's body past the ops runtime lookup - Runtime is what Refresh finds through OpsRuntime() (the
	 * controller's, else UOpsRuntimeSubsystem's), null for none. A headless test's world has no game instance to hold that subsystem, so the depot card, the
	 * fuel line and the turnaround were out of its reach; UOfferInboxWidget::RefreshWith is the same seam.
	 */
	void RefreshWith(const UOpsRuntime* Runtime, const ARoadNetworkActor* Target, const FSelection& Selection,
		const FAgentFacts* PrecomputedAgentFacts = nullptr);

	/** How many times a network card's Describe (InspectFacts::DescribeRunway/Taxiway/Stand) actually ran - the
	 *  FInspectorCardKey gate's counter: a quiet frame adds nothing. */
	int32 CardDescribeCountForTest() const { return CardDescribeCalls; }

	/** How many times the depot card asked UJobBoard::DescribeDepot - at most once a game minute on a quiet board. */
	int32 DepotDescribeCountForTest() const { return DepotDescribeCalls; }

	/** How many times the aircraft card looked its flight up (UFlightBoard::FlightForAgent), asked the job board
	 *  for its fuel line, and composed its turnaround sentence - the three lookups PR E keys. */
	int32 FlightLookupCountForTest() const { return FlightLookups; }
	int32 FuelLookupCountForTest() const { return FuelLookups; }
	int32 TurnaroundComposeCountForTest() const { return TurnaroundComposes; }

	bool IsShownForTest() const;
	bool IsDepartEnabledForTest() const;
	FString TitleForTest() const;

	/** The composed facts text (heading, speed, ... and the Demands block). */
	FString FactsForTest() const;
	/** The status line as shown. */
	FString StatusForTest() const;
	/** Depart's CAPTION colour - the thing that must actually change with enabled state.
	 *  See Refresh: the button's own background stays Style->Control always (UUiButton::LookFor). */
	FLinearColor DepartLabelColourForTest() const;
	/** How many times Refresh actually called SetText on one of its three fields, as opposed
	 *  to how many times it was asked - the seam issue #187's gate is measured through: an
	 *  idle tick (same selection, same facts) must add nothing to this. */
	int32 SetTextCallCountForTest() const { return SetTextCalls; }

	/** How many times Refresh actually RECOMPOSED the aircraft Title/Facts/Status sentences
	 *  (the Printf/FString::Format work FInspectorKey gates), as opposed to how many times it
	 *  was asked - issue #309, one level upstream of SetTextCallCountForTest: an idle tick on a
	 *  PARKED aircraft (same selection, same rounded facts) must add nothing to this, even
	 *  though the earlier SetText gate already read as flat by comparing the strings this
	 *  count now stops building in the first place. */
	int32 ComposeCountForTest() const { return ComposeCalls; }

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
	/** By id, RunwayActionIndex's rule. Its row only opens the popup - see ARoadBuildController::RequestUnstickMenu. */
	int32 UnstickActionIndex = INDEX_NONE;
	/** By id, RunwayActionIndex's rule - and caught BEFORE the positional Depart/Follow pair, or a new
	 *  Selection row would shift it. */
	int32 BuyModuleActionIndex = INDEX_NONE;
	int32 BuyVehicleActionIndex = INDEX_NONE;
	int32 SellVehicleActionIndex = INDEX_NONE;

	/** The quote the rows were last drawn from - the buy menu's lines are asked of it as it opens. */
	FFacilityQuote LastQuote;
	/** The fleet's vehicle ids last drawn; the rows are rebuilt only when this changes. */
	FString LastFleetKey;
	/** The type codes the open buy menu lists, by line - HandleBuyVehicleChosen reads the line's code. */
	mutable TArray<FName> ShownVehicleCodes;
	UPROPERTY() TArray<TObjectPtr<UInspectorFleetRow>> FleetRows;

	TArray<FUiMenuItem> BuyVehicleItems() const;
	void RebuildFleetRows();
	/** Unarm every fleet row and the controller's armed sale - a new card, a deselect or a rebuild. */
	void DisarmSale();
	/** The runtime the card quotes: the controller's (ARoadBuildController::GetOpsRuntime), else the
	 *  game instance's. */
	const UOpsRuntime* OpsRuntime() const;
	UFUNCTION() void HandleBuyModule();
	UFUNCTION() void HandleBuyVehicleChosen(int32 Index);

	/** The controller's request count last acted on - see ARoadBuildController::RequestUnstickMenu. */
	int32 SeenUnstickRequests = 0;
	bool bUnstickHighlighted = false;

	/** The popup's lines for the selected agent, one per EUnstickAction in enum order - HandleUnstickChosen
	 *  reads the line's index AS the action, so the two cannot disagree about which line is which. */
	TArray<FUiMenuItem> UnstickItems() const;



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

	/** The aircraft facts the composed Title/Facts/Status were last built from - see
	 *  FInspectorKey's own comment. Compared before Refresh does any Printf/FString::Format
	 *  work, not after: this is the gate ONE STEP EARLIER than LastTitle/LastFacts/LastStatus. */
	FInspectorKey LastComposedKey;

	/** What Refresh composed last time FInspectorKey changed - reused verbatim on an unchanged
	 *  tick rather than recomposed, which is the entire saving ComposeCountForTest measures. */
	FString LastComposedTitle, LastComposedFacts, LastComposedStatus;

	/** See ComposeCountForTest. */
	int32 ComposeCalls = 0;

	/** The deadlock line - composed and SetText-gated like its siblings above. */
	FString LastComposedDeadlock, LastDeadlock;

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

	/**
	 * How the card names another agent: its flight's registration, else "<type> #<id>" (the title's
	 * own fallback - a service vehicle reads "FUEL #7"), else "aircraft <id>" for one already gone.
	 */
	FString NameOfAgent(const UOpsRuntime* Runtime, const UGroundTraffic* Traffic, int32 AgentId) const;

	/** The network card as last described, and what it was described from - see FInspectorCardKey. Valid once a
	 *  Describe has succeeded; a failed one hides the card and leaves the last good key, which cannot match the
	 *  key that failed (it differed, or it would not have been asked). */
	FInspectorCardKey LastCardKey;
	bool bCardValid = false;
	FString CardTitle, CardFacts, CardStatus;
	bool bCardRunway = false;
	FText CardRunwayCaption, CardRunwayUseCaption;
	/** Whether the kept card is a depot's, and on a road - what the per-tick purchase quote is laid on by. */
	bool bCardDepot = false;
	bool bCardDepotReachable = false;

	/** See CardDescribeCountForTest / DepotDescribeCountForTest. */
	int32 CardDescribeCalls = 0;
	int32 DepotDescribeCalls = 0;

	/**
	 * THE AIRCRAFT CARD'S THREE LOOKUPS, each kept until what it reads moves (ops batch 3 PR E; they ran every tick,
	 * before FInspectorKey could be compared, because their answers are fields of it):
	 *  - the flight: UFlightBoard::FlightForAgent reads the board's agent index, which moves with Revision.
	 *  - the fuel line: UJobBoard::DescribeAgent reads the job board (Revision) and, while pumping only, the clock -
	 *    which it says (bFuelLineLive), and then it is asked every tick, as before.
	 *  - the turnaround: DescribeTurnaround reads the flight's contract and Now, which it shows only as whole minutes
	 *    left or late - so it is keyed on exactly those (TurnaroundKey), computed from the flight each tick for two
	 *    subtractions instead of an FText::Format.
	 * The spec keyed them on "flight board revision + agent phase"; the phase is no input of any of the three (a phase
	 * change reaches the board as a Revision), while the job board and the clock are.
	 * WEAK, all three pointers: a load or a level change can take either board or the flight away between ticks.
	 */
	TWeakObjectPtr<const UFlightBoard> FlightLookupBoard;
	uint32 FlightLookupRevision = 0;
	int32 FlightLookupAgent = INDEX_NONE;
	TWeakObjectPtr<const UFlight> FlightLookup;

	TWeakObjectPtr<const UJobBoard> FuelLineBoard;
	uint32 FuelLineRevision = 0;
	int32 FuelLineAgent = INDEX_NONE;
	bool bFuelLineLive = false;
	FString FuelLine;

	/** Flight, contract, late?, whole minutes shown - DescribeTurnaround's every input at the resolution it prints. */
	TWeakObjectPtr<const UFlight> TurnaroundFlight;
	double TurnaroundContract = -1.0;
	bool bTurnaroundLate = false;
	int32 TurnaroundMinutes = -1;
	FString TurnaroundLine;

	/** See FlightLookupCountForTest and its two siblings. */
	int32 FlightLookups = 0;
	int32 FuelLookups = 0;
	int32 TurnaroundComposes = 0;

	/** The key for Selection's network card now - see FInspectorCardKey for what each kind reads. */
	FInspectorCardKey CardKeyFor(const UOpsRuntime* Runtime, const ARoadNetworkActor& Target, const FSelection& Selection) const;

	/** What Refresh last showed, so a NEW selection can reopen a window the player closed. */
	FSelection LastSelection;

	void EnsureSlots(const UUIStyle* Style);
	void RunAction(int32 ActionIndex);

	UFUNCTION() void HandleDepart();
	UFUNCTION() void HandleFollow();
	UFUNCTION() void HandleRunway();
	UFUNCTION() void HandleRunwayUse();
	UFUNCTION() void HandleUnstickChosen(int32 Index);
	UFUNCTION() void HandleWaitingFor();
};
