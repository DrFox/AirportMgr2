# Ops batch 3 - PR B plan: airport status (open / closed / no runway)

Spec: `docs/superpowers/specs/2026-09-29-ops-batch3-design.md` §3, §7. Branch `feature/ops-airport-status`, stacked
on `feature/ops-batch3` (PR A, #415, tip `6aa4fc00`), worktree `C:\repos\airportmgr2-ops-batch3`. Baseline full
suite on this tree: `1482 test(s) run, 0 failed, 0 crashed` (2026-09-29).

Order: model first (Airport, board, roster, generator, alerts - world-free, one build), then the runtime wiring
(composition tests), then the game module (bar action, confirm, inbox strip), then the icon asset.

## Files

| File | Change |
|---|---|
| `AirportOps/Public/Model/Airport.h` + `Private/Model/Airport.cpp` | NEW. `EAirportStatus`, `UAirport` (IOpsPersistent blob `"Airport"`) |
| `AirportOps/Public/Model/Flight.h` | `EFlightPhase::Withdrawn` appended LAST; `UENUM() ECancelReason { AirportClosed, NoRunway, Unstuck }` |
| `AirportOps/Public/Model/OpsEventBus.h` / `.cpp` | `FAirportStatusChangedEvent`, `FFlightCancelledEvent`, appended to `FOpsEvent`; `Describe()` |
| `AirportOps/Public/Model/FlightBoard.h` / `.cpp` | `CancelUnarrived`, `UnarrivedCount`, `OnGroundCount`; `CancelByAgent` publishes `Unstuck`; Withdrawn is terminal in `OnAfterRestore` |
| `AirportOps/Public/Model/OpsDefinition.h` | `FAirlineSatisfactionTuning::ClosureCancelPenalty = 0.05`, unjudged (2026-09-29) |
| `AirportOps/Public/Model/AirlineRoster.h` / `.cpp` | `OnFlightCancelled` |
| `AirportOps/Public/Model/OfferGenerator.h` / `.cpp` | `UPROPERTY(Transient) TObjectPtr<const UAirport> Airport`; `TickMinute` returns nothing unless Open, before any per-airline work |
| `AirportOps/Public/Model/OpsAlerts.h` / `.cpp` | `EAlertKind::NoRunway` appended; `FOpsAlertSources::Airport`; AirlineCannotCome only while Open |
| `AirportOps/Public/Present/OpsRuntime.h` / `.cpp` | owns `UAirport`; `SetAirportClosed`; WireBus subscriptions; Attach/Detach/Load/Persistents lines |
| `Source/AirportMgr/BuildActions.h` / `.cpp` | `FBuildAction::MenuItems` / `Choose` / `TryChoose`; action `game.airport` |
| `Source/AirportMgr/UI/UiMenuButton.h` / `.cpp` | `Build` takes the bar button's layout and icon (defaults unchanged) |
| `Source/AirportMgr/BuildBarWidget.h` / `.cpp` | an action with `MenuItems` is built as a `UUiMenuButton` |
| `Source/AirportMgr/OfferInboxWidget.h` / `.cpp` | the demand strip reads "Closed" while not Open |
| `Tools/Python/fetch_ui_icons.py`, `Content/UI/...` | icon for `game.airport` (AirportMgr.UI.EveryActionResolvesAnIcon) |
| tests | NEW `AirportTest.cpp` (model + composition); existing `FlightBoardEventsTest.cpp`, `AirlineRosterTest.cpp`, `OfferGeneratorTest.cpp`, `OpsAlertsTest.cpp`, `OpsRuntimeBusTest.cpp`, `BuildActionsTest.cpp`, `OfferInboxWidgetTest.cpp` |

`AirportTest.cpp` is a NEW test file: two builds, and the count must rise.

## Interfaces (checked against the live headers, 2026-09-29)

```cpp
// Airport.h
UENUM(BlueprintType)
enum class EAirportStatus : uint8 { Open, ClosedByPlayer, NoRunway };

UCLASS()
class AIRPORTOPS_API UAirport : public UObject, public IOpsPersistent
{
	virtual FName SaveBlobName() const override { return TEXT("Airport"); }
	virtual UObject& AsPersistentObject() override { return *this; }
	virtual void OnBeforeRestore() override { bClosedByPlayer = false; }   // missing blob = open
	FOpsEventBus* Bus = nullptr;
	static EAirportStatus Derive(bool bClosedByPlayer, bool bHasRunway);    // intent wins
	EAirportStatus Status() const;
	bool IsClosedByPlayer() const;
	bool Refresh(const URoadNetwork& Network);                  // re-derive; publish on a change
	void Reseat(const URoadNetwork& Network);                   // re-derive, NO event (attach, load)
	bool SetClosedByPlayer(bool bClosed, const URoadNetwork& Network);  // the command: sets, Refresh
	void ResetForNewGame();
private:
	UPROPERTY() bool bClosedByPlayer = false;
	UPROPERTY(Transient) EAirportStatus Current = EAirportStatus::Open;
};

// OpsEventBus.h
struct AIRPORTOPS_API FAirportStatusChangedEvent { EAirportStatus Old, New; EventName "AirportStatusChanged"; Describe(); };
struct AIRPORTOPS_API FFlightCancelledEvent { int32 FlightId; FName AirlineId; ECancelReason Reason; EventName "FlightCancelled"; Describe(); };

// FlightBoard.h
int32 CancelUnarrived(UGroundTraffic& Traffic, USimClock& Clock, ECancelReason Reason);  // returns flights cancelled
int32 UnarrivedCount() const;   // Accepted + Inbound: what a close would cancel (the confirm's N)
int32 OnGroundCount() const;    // Landing..Departing: the draining readout

// AirlineRoster.h
void OnFlightCancelled(const FFlightCancelledEvent& Event);

// OpsRuntime.h
UAirport* GetAirport() const;
bool SetAirportClosed(bool bClosed);   // false, logged, when unattached

// BuildActions.h
TFunction<TArray<FUiMenuItem>(const FBuildActionContext&)> MenuItems;   // set: the bar builds a UUiMenuButton
TFunction<void(FBuildActionContext&, int32)> Choose;
bool TryChoose(ARoadBuildController& C, int32 Line, const TCHAR* Via) const;
```

Rulings taken here (reported as deviations):

- `CancelUnarrived` takes `(Traffic, Clock, ECancelReason)`, not the spec's `(Now, EAirportStatus)`: releasing a hold
  needs the traffic model and disarming the arrival needs the clock (Accept's own signature); the board does not
  learn the airport, the runtime maps `ClosedByPlayer -> AirportClosed`, `NoRunway -> NoRunway`.
- ATTACH AND LOAD RE-DERIVE WITHOUT AN EVENT (`Reseat`): only a change seen while attached is a change. A load
  never re-runs the cancellation (spec), and an attach to a runway-less new game has nothing to cancel.
- Withdrawals publish no `FFlightCancelledEvent` (an offer was never the airline's flight at this airport) and no
  `OfferExpired` (spec): no score.
- AirlineCannotCome is raised only while Open: the generator skips the per-airline evaluation while closed, so a
  state left from before the close would otherwise stand as a stale alert.
- THE CONFIRM is the inspector Unstick's: `UUiMenuButton` + `FUiMenuItem::bConfirm` / `ConfirmLabel`, popped at
  the button the player just clicked. Line: `Close airport` -> armed caption `Close? N flights will be cancelled`;
  a close disarms (`HandleOpenChanged`). Reopening is the popup's plain line `Open airport` - no confirm.
- The bar caption: `Close airport` while Open, else the status (`Closed (draining: N)`, `Closed`, `No runway`).

## Task 1 - model (world-free)

1. Declarations with empty bodies (Airport returns Open, CancelUnarrived returns 0, roster handler empty, generator
   gate absent, alert kind unraised); events + Describe + variant entries. Build. RED:
   - `AirportOps.Model.Airport.StatusIsDerived` - no runway NoRunway; runway Open; closed ClosedByPlayer with or
     without a runway; reopened back to derived.
   - `AirportOps.Model.Airport.ChangePublishesOnce` - a local bus: one event {Open, NoRunway} on the change, none
     on an unchanged Refresh, none from Reseat.
   - `AirportOps.Model.Airport.SaveKeepsClosedByPlayer` - CaptureBlob/RestoreBlob keeps it; a snapshot with no
     blob restores open.
   - `AirportOps.Model.FlightBoard.CancelUnarrivedCancelsAndWithdraws` - Offered Withdrawn, Accepted and Inbound
     Cancelled (stand released; the Accepted one's arrival disarmed - the clock past its ETA enqueues nothing;
     the Inbound one out of Queue()), Landing untouched; PendingOfferCount 0; one FlightCancelled each with the
     reason; no OfferExpired.
   - `AirportOps.Model.FlightBoard.CancelByAgentPublishesUnstuck`.
   - `AirportOps.Model.Airlines.ClosureCancelScoresOnlyAirportClosed` - AirportClosed -0.05 ("cancelled: airport
     closed"); NoRunway, Unstuck: nothing.
   - `AirportOps.Model.Offers.Generate.NothingUnlessOpen` - closed / no-runway: no offers, zero admission checks,
     no bCouldCome written; Open control generates.
   - `AirportOps.Model.Alerts.NoRunwayRaised` - NoRunway: one alert, no focus; ClosedByPlayer: none; not Open:
     no AirlineCannotCome.
2. GREEN: bodies.
3. Mutations: generator gate (NothingUnlessOpen red), alert branch (NoRunwayRaised red), CancelByAgent publish.

## Task 2 - runtime

1. RED (composition, `AirportTest.cpp`):
   - `AirportOps.Present.Airport.CloseCancelsThroughTheBus` - runway field, one accepted flight of a seeded
     airline; `SetAirportClosed(true)`; Tick: flight Cancelled, airline down by the tuning's ClosureCancelPenalty
     (`> 0` asserted first - the scenario asset carries it). The status-changed Sim seam and the roster seam.
   - `AirportOps.Present.Airport.StatusChangeDirtiesAlerts` - quiet, empty board; close; Tick: alerts pass ran
     once more.
   - `AirportOps.Present.Airport.RunwayComesAndGoes` - runway-less attach: Tick: NoRunway + its alert, zero
     generator admission checks over a game minute; build a runway: Open, alert cleared; delete it: NoRunway,
     alert back.
   - `AirportOps.Present.Airport.LoadRederivesWithoutCancelling` - closed + saved with an Accepted flight planted;
     load: status ClosedByPlayer before any Tick; Ticks: the flight still Accepted.
   - `AirportOps.Present.Airport.ClosedAirportDrains` - the fuel fixture's field: an aircraft on the ground, close,
     tick: it departs, OnGroundCount 0, still ClosedByPlayer.
   - `AirportOps.Present.Bus.DetachUnhooksEveryPublisher` gains the airport's bus.
2. GREEN: runtime wiring.
3. Mutations: (a) drop the AirportStatusChanged Sim subscription -> CloseCancelsThroughTheBus red; (b) drop the
   FlightCancelled roster subscription -> its penalty assertion red; (c) drop the AirportStatusChanged Alerts
   subscription -> StatusChangeDirtiesAlerts red; (d) load publishes (Refresh for Reseat) ->
   LoadRederivesWithoutCancelling red; (e) drop `OfferGenerator->Airport` wiring -> RunwayComesAndGoes red.

## Task 3 - game module

1. RED:
   - `AirportMgr.Actions.AirportCloseConfirms` - a hand-made context over a runtime: Open -> one line, bConfirm,
     armed caption `Close? 1 flights will be cancelled`; through a UUiMenuButton: arm then close -> nothing
     chosen, a Tick later the flight is still Accepted and the airport Open; arm, choose, Choose -> ClosedByPlayer,
     cancelled; closed -> one plain line `Open airport`.
   - `AirportMgr.Actions.AirportStatusCaption` - `Close airport` / `Closed` / `Closed (draining: 1)` / `No runway`.
   - `AirportMgr.Actions.BarBuildsMenuActionsAsMenus` - the bar's entry for `game.airport` is a UUiMenuButton.
   - `AirportMgr.UI.OfferInbox.DemandStripReadsClosed`.
2. GREEN. Icon: `game.airport` in fetch_ui_icons.py, headless fetch + build_ui_style.py, revert collateral.

## Scenario asset

`DA_Scenario_Default.uasset` is 1193 bytes and holds no property names (`grep -a` for ShortfallPenalty,
AirlineSatisfaction, StartingBalance finds none): every value equals the CDO, so tagged serialisation wrote
none, and a new field loads its constructor default. No re-run of build_scenario.py; CloseCancelsThroughTheBus
asserts the resolved tuning's penalty is non-zero.

## Finish

Full suite; UE_LOG / comment-line counts of touched files against `6aa4fc00`; Check-Architecture verdict and
rule-12 warning count. Unverified in PIE: the bar popup and caption, the inbox strip, the NoRunway toast.

## Execution notes (2026-09-30)

- Full suite: `1499 test(s) run, 0 failed, 0 crashed` (baseline 1482; +17). Check-Architecture PASS, rule-12 warnings 111 (unchanged).
- UE_LOG in touched production files 64 -> 74; comment lines 2443 -> 2650; no file fell.
- The FlightBoard and Airport model tests live in the new `AirportTest.cpp` beside the composition tests, not in
  `FlightBoardEventsTest.cpp` as planned: one fixture (a two-stand field, a bus with recorders) serves both.
- Existing test changed by the spec: `AirportOps.Present.Alerts.PassRaisesThroughTheRuntime` runs on a runway-less
  field, which now raises NoRunway (and no AirlineCannotCome); its first assertion counts that alert instead of zero.
- Mutations (each red, restored with cp + touch, rebuilt green): generator gate -> NothingUnlessOpen; NoRunway alert
  branch -> NoRunwayRaised; CancelByAgent publish -> CancelByAgentPublishesUnstuck; arrival disarm ->
  CancelUnarrivedCancelsAndWithdraws; status Sim subscription -> CloseCancelsThroughTheBus; roster handler body ->
  CloseCancelsThroughTheBus (penalty); status Alerts subscription -> StatusChangeDirtiesAlerts; load Refresh for Reseat
  -> LoadRederivesWithoutCancelling; `OfferGenerator->Airport` wiring -> RunwayComesAndGoes; NetworkChanged Airport
  subscription -> RunwayComesAndGoes; Detach's `Airport->Bus = nullptr` -> DetachUnhooksEveryPublisher; bar menu
  branch -> BarBuildsMenuActionsAsMenus; `bConfirm` -> AirportCloseConfirms.
- Scenario asset: not re-run (see "Scenario asset"); CloseCancelsThroughTheBus's `ClosureCancelPenalty > 0` passes
  against the resolved DA_Scenario_Default.
- Icon: `game.airport` -> delapouite/control-tower via fetch_ui_icons.py + build_ui_style.py headless; 34 re-saved
  T_Icon_* reverted; DA_UIStyle holds `game.airport` (grep -a, 0 before).

## Review ledger (fresh review of PR B, 2026-09-30: 0 Critical, 3 Important)

Rulings are the orchestrator's. Each fix got a test; a pin test on behaviour that already held went red under its
mutation instead.

| # | Finding | Ruling / fix | Test (red line) |
|---|---|---|---|
| I1 | A closed airport still took arrivals: debug Land, and an inbox accept | RULING: a closed airport admits NO arrivals. `UOpsRuntime::LandNear` gates before AcceptImmediate (which leaves a refused flight as an offer): NoRunway -> `FLandRefusedEvent{NoRunway}`, returns NoRunway; ClosedByPlayer -> `FNotificationEvent "Airport closed"`, returns NotAdmitted (Airside has no word for a closure). `UFlightBoard::Accept` asks `AdmitsArrivals` (a predicate set by the runtime, so the board still does not learn the airport) - the one door the inbox and key 7 share. `aircraft.land` IsEnabled = HasRunway && Open | `AirportOps.Present.Airport.LandRefusedWhileClosed` ("no flight is on its way" expected 0, got 1); `AirportOps.Present.Airport.AcceptRefusedWhileClosed` ("closed: an accept is refused" expected false); `AirportMgr.Actions.LandGreyedWhileClosed` ("closed: Land is greyed" expected false) |
| I2 | NoRunway -> ECancelReason::NoRunway mapping unpinned | Pinned | `AirportOps.Present.Airport.RunwayLossCancelsFree` (green; mutant mapping to AirportClosed: "airline minds not at all" 0.50 expected, 0.45) |
| I3 | The bar's production door (OnChosen -> TryChoose) untested | `UBuildBarWidget::UseForTest` / `MenuForTest`; the bar resolves its context once (`WithContext`); `TryChoose(FBuildActionContext&)` overload | `AirportMgr.Actions.BarMenuVerbReachesTheRuntime` ("game.airport is a menu on the bar" expected not null); `AirportMgr.Actions.TryChooseGatesOnEnabled` (green; mutant without the gate: "refuses a disabled action" expected false) |
| M1 | Stale AirlineCannotCome flashed after a reopen | A Sim "Offers" subscription: entering Open calls `UOfferGenerator::ForgetAirlineVerdicts` | `AirportOps.Present.Airport.ReopenForgetsAirlineVerdicts` ("reopened: no airline alert" expected 0, got 2) |
| M2 | ResetForNewGame kept Current | Resets it | `AirportOps.Model.Airport.NewGameForgetsTheStatus` ("and the status with it" not equal) |
| M3 | Attach's silence not measured | `UAirport::ChangeCountForTest`; RunwayComesAndGoes asserts status and zero changes straight after Attach | green; mutant Attach Refresh: "publishes no status change" expected 0, got 1 |
| M4 | Silent skip with no traffic model | Warning | `AirportOps.Present.Airport.ClosureWithNoTrafficWarns` (expected Warning found 0 times) |
| M5 | A save in the frame of an edit snapshot a stale status | `Airport->Refresh` before the save's drain | `AirportOps.Present.Airport.SaveRefreshesTheStatus` ("the save itself saw the runway go" not equal) |
| M6 | Inbox Refresh -> ShowAirportStatus unpinned | `UOfferInboxWidget::RefreshWith(Runtime, Target)` - Refresh's body past the subsystem lookup | `AirportMgr.UI.OfferInbox.RefreshReadsTheStatus` ("Closed" expected, got "") |
| M7 | Unmarked claims | ENFORCED BY on Flight.h Withdrawn and FFlightCancelledEvent (reworded: no "only"/"one subscriber"); Execute's claim | `AirportMgr.Actions.AirportExecuteNeverCloses` (green; mutant toggle: "open stays open" not equal) |
| M8 | Flight.h class comment | Cancelled's two paths and Withdrawn | - |
| M9 | Choose toggled | Two fixed lines (0 Close, 1 Open), each enabled only when it applies; Choose acts on the line | `AirportMgr.Actions.AirportChooseActsOnTheLine` ("Close on a closed airport leaves it closed" not equal); AirportCloseConfirms updated to two lines |
| M10 | Other menu-verb shape unnamed | Comment on `FBuildAction::MenuItems` naming selection.unstick's request shape | - |

Existing tests changed by ruling I1: `AirportOps.Present.LandNear` (its runway-less vantage point is now refused before
AcceptImmediate; it lays a 60 m runway, asserts Open, and watches the planner's refusal instead) and
`AirportOps.Present.Alerts.AcceptDirtiesAlerts` (lays a runway so the accept is admitted).
