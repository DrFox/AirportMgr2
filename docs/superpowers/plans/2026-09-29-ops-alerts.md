# Ops alerts and money events - implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Checkbox steps.

**Goal:** Standing problems become alerts (toast on raise, a bar badge and window, "Go" to the subject); silent refusals become toasts; money state is pushed, not polled.

**Architecture:** `UOpsAlerts` (AirportOps Model/) recomputes seven conditions from board and traffic state when the `"Alerts"` pass is dirtied, diffs by key, and publishes `FAlertRaisedEvent` / `FAlertClearedEvent`. UI hears the bus only through `UOpsEvents` dynamic delegates (rule 31). Airside facts reach the bus through native delegates bridged in `UOpsRuntime`.

**Tech stack:** UE 5.8 C++, UMG (code-built panels), UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-29-ops-alerts-design.md`.

**Execution note:** executed inline by the planning session (native), with a fresh reviewer per stage. So tasks name exact files, signatures and tests, and write code in the session against the headers as they are, rather than transcribing it here - memory `roadnet-plan-defects-found-in-execution`: plan code that is not checked against live headers ships defects.

## Global constraints

- Worktree `C:\repos\airportmgr2-ops-alerts`. Branches: `feature/ops-alerts` (stage 1) -> `feature/ops-alerts-ui` (stage 2) -> `feature/ops-money-events` (stage 3), stacked, one PR each.
- Build `Build.bat AirportMgrEditor Win64 Development -Project=<worktree>\AirportMgr.uproject -WaitMutex -NoHotReloadFromIDE`; a new .cpp needs a second build. Tests `Tools/Run-AirsideTests.ps1` from the worktree; judge the `run / failed / crashed` line.
- Airside never includes AirportOps. Model/ is world-free. Subscribe only in `UOpsRuntime::WireBus` (rule 31).
- New bus events: `AIRPORTOPS_API` structs, `EventName()`, `Describe()`, a subscriber the same day.
- Commits: no `Co-Authored-By`; end with the Claude-Session line. A mutation check for every new seam test.
- Refactor contract for the deadlock extraction: no `UE_LOG` lost, WHY comments travel, `Resolve` behaviour unchanged (existing deadlock tests are the gate).

## Review focus

1. An alert that never clears: a condition fixed by a path that dirties nothing. Every dirtier is listed in Task 4; a composition test fixes-and-clears for the job kind.
2. Alerts duplicated after a load or a re-attach: the set is emptied and the UI mirror cleared on both. Tested.
3. Deadlock false positives: a reservation cycle that a yield settles is still a cycle; alerts show only ALL-AIRCRAFT cycles (the class that "is a DESIGN problem the player must be told about").
4. A facade refusal at design time (editor mode, no purse): `CanAfford` answers true with no purse, so nothing fires. Tested at the facade.
5. "Go" on a gone subject: no crash, no move.

---

## Stage 1 - alerts and toasts (`feature/ops-alerts`)

### Task 1: one definition of a wait cycle (Airside)

- `FDeadlockResolver`: extract the walk in `Resolve` (`GroundTrafficDeadlock.cpp`, from "ONE EDGE PER STALLED WAITER" through the cycle slice) into
  `static void FindCycles(TConstArrayView<FRoadAgent> Agents, const FTrafficRules& Rules, FCycleScratch& Scratch, TArray<TArray<int32>>& OutCycles);`
  `FCycleScratch` holds the four members `Waiting`, `Visited`, `Path`, `Position` (#190's reason for keeping them - their capacity - still holds). `Resolve` calls it with its own scratch, then loops the cycles with its existing body. Comments move with the code.
- `UGroundTraffic::CurrentDeadlocks(TArray<TArray<int32>>& Out) const` - `FindCycles` with a local scratch, keeping only cycles whose members are all found and all `ETraversalClass::Aircraft` (the same test as `bAllAircraft` in `Resolve`).
- Tests (`Plugins/Airside/Source/AirsideTests/Private/`, extend the existing deadlock test file): an all-aircraft two-cycle is reported; a mixed cycle is not; below `Rules.StallSeconds` nothing; a queue (waiting on a mover) nothing. Existing `Airside.*Deadlock*` tests stay green (the refactor gate).

### Task 2: the facade says when it cannot afford (Airside)

- `Public/Present/RoadEditFacade.h`: `enum class EBuildRefusal : uint8 { CannotAfford };`, `DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnBuildRefused, const FText& /*What*/, EBuildRefusal, double /*Price*/)`, member `FOnBuildRefused OnRefused;`, and private `bool AffordOrRefuse(const FBuildQuote& Quote, const TCHAR* Who);` - `CanAfford`, and when false logs the existing line and broadcasts `OnRefused`.
- Replace all ten `if (!CanAfford(...))` COMMIT sites (RoadEditFacade.cpp :698, :773, :918, :1424; RoadEditFacadeSurfaces.cpp :86, :157, :277, :488, :749, :830) with `AffordOrRefuse`; each keeps its own log text by passing `Who`. `CanAfford` itself stays for previews (const, silent).
- Check-Architecture rule 32 `refusal-is-announced`: in `RoadEditFacade*.cpp`, `!CanAfford(` appears only inside `AffordOrRefuse`. Mutation: add a bare one -> fails.
- Test (Airside, facade with a stub `IBuildPurse` that refuses): an unaffordable `ConnectNodes` broadcasts `OnRefused` once with the quote's `What`; with no purse, nothing.

### Task 3: `UOpsAlerts` (AirportOps Model)

- `UOfferGenerator::DescribeWhyNot(FName AirlineId) const` -> the `AdmissionCache` first refusal sentence, falling back to `ArrivalPlanner::DescribeRefusal` (same as the "cannot use this airport" log line). `UJobBoard::IsStranded` moves to public.
- `Public/Model/OpsAlerts.h`: `UENUM EAlertKind`, `UENUM EAlertFocusKind`, `USTRUCT FOpsAlertKey {Kind, Id, Name}` with `==` and `GetTypeHash`, `USTRUCT FAlertFocus`, `USTRUCT FOpsAlert {Key, Text (FText), Focus, RaisedAt}`.
  `UCLASS UOpsAlerts : UObject` - `FOpsEventBus* Bus`; `void Recompute(const FOpsAlertSources&, double Now)`; `const TArray<FOpsAlert>& GetAlerts() const`; `void Reset()` (empties, publishes nothing - load/attach).
  `FOpsAlertSources` = const pointers to `UFlightBoard`, `UJobBoard`, `UGroundTraffic`, `URoadNetwork`, `UOfferGenerator`, `ULedger`, plus `TArrayView<const FAirlineOffers>`; any null skips its kinds.
- Events in `OpsEventBus.h`: `FAlertRaisedEvent {FOpsAlert Alert}`, `FAlertClearedEvent {FOpsAlertKey Key}`, `FBuildRefusedEvent {FString What; EBuildRefusal Why; double Price; double Balance}`, `FLandRefusedEvent {EArrivalRefusal Why}`.
- Wording: FlightStranded "Flight <callsign> is stranded - retire or unstick it"; VehicleStranded "<type> <id> is stranded - unstick it"; JobUnserviceable "No fuel for stand <n>: <RefusalText>"; HeldStandLost "Flight <callsign>'s stand was removed - it has nowhere to park"; AirlineCannotCome "<airline> cannot use this airport: <sentence>"; Deadlock "Aircraft deadlocked (<n>) - the layout needs another way round"; Overdrawn "Overdrawn - building is locked".
- Tests (`AirportOpsTests/Private/OpsAlertsTest.cpp`), world-free fixtures reused from FlightBoardTest / FuelServiceTest / AirlineRosterTest: per kind raised while true, cleared when not, no second raise on an unchanged recompute; `Reset` then recompute re-raises. JobUnserviceable text contains `RefusalText`.

### Task 4: wiring (Present/)

- `UOpsRuntime`: owns `UOpsAlerts* Alerts`; `Alerts->Bus = &Bus`; pass `"Alerts"` registered after `"JobBoard"`, running `Recompute` with live sources; `Reset` in Attach and LoadFromSlot (with `MarkAllDirty` already there).
- Dirtied by: `FAgentPhaseEvent`, `FNetworkChangedEvent`, the JobBoard pass (it calls `MarkDirty("Alerts")` after `Step`), `FOfferExpiredEvent`, `FOfferDeclinedEvent`, `FFlightAirborneEvent`, `FAirlineSatisfactionEvent`, the offer minute (`OfferTick` marks it), and a `Clock.Every(10 game s)` armed while any agent `GetStalledSeconds() > 0` and cancelled when none (the pass arms/cancels it).
- Bridge `Target->GetEditFacade()->OnRefused` -> `FBuildRefusedEvent` with `Ledger->Balance()`; unbound in Detach.
- `LandNear`: when `AcceptImmediate` refuses, `Bus.Publish(FLandRefusedEvent{Why})`.
- `UOpsEvents`: `DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsAlertRaised, const FOpsAlert&, Alert)`, `FOpsAlertCleared(const FOpsAlertKey&)`, `FOpsBuildRefused(const FString& What, double Price, double Balance)`, `FOpsLandRefused(EArrivalRefusal)`; Presentation subscriptions in WireBus.
- Composition tests (`OpsRuntimeBusTest.cpp`): a job made unserviceable raises `JobUnserviceable` through the runtime and clears after the road is joined (reuse FuelServiceWired geometry, delete the spur); an unaffordable placement via the facade reaches `UOpsEvents::OnBuildRefused`; `LandNear` on a runway-less field reaches `OnLandRefused`; load leaves no duplicate alerts. Mutation: drop the phase dirtier -> a stranded-flight composition test red.

### Task 5: toasts

- `UToastStackWidget`: bind `OnAlertRaised` (Warning, alert text), `OnAlertCleared` (Info "Back in credit" for Overdrawn only - the widget keeps no alert list, so the cleared key's Kind decides), `OnBuildRefused` ("Can't afford <What> (<price>; balance <balance>)", Warning, money via `UPricing::Format` from the runtime), `OnLandRefused` (Warning, `ArrivalPlanner::DescribeRefusal`).
- Tests (`ToastStackWidgetTest.cpp`): each delegate posts one entry of the right severity.

### Task 6: stage 1 PR

Full build + suite + Check-Architecture; fresh reviewer; fix; PR to main. PIE check per spec §4.

## Stage 2 - badge, window, Go (`feature/ops-alerts-ui`)

### Task 7: camera and selection seams

- `UBuildCameraComponent::FocusOn(const FVector2D& At)`: leave watch mode, set the target focus; the ease does the rest.
- `FBuildSession::Select(ESelectionKind Kind, int32 Id)`; `ARoadBuildController::SelectAndFocus(const FAlertFocus&)` - Agent/Entity: position via `FSelectTool::PositionOf`, `FocusOn`, `Select`; Point: `FocusOn`; None: nothing. Returns false when the subject is gone (Verbose log).
- Tests: `FocusOn` moves the target focus; `Select` then `GetSelection()` round-trips; a gone agent returns false and moves nothing.

### Task 8: the alerts window and the bar badge

- Controller mirror: `TArray<FOpsAlert> AlertMirror`, kept by `OnAlertRaised`/`OnAlertCleared`, cleared on attach/load (the runtime's `Reset` is followed by re-raises).
- `UAlertsPanelWidget : UAirportMgrPanelWidget` - `WantsWindow` (Id "Alerts", AboveBarLeft, closable), rows as `ULedgerPanelWidget` (icon, text, "Go"), repaint on a mirror revision. No-focus kinds: AirlineCannotCome opens the offer inbox, Overdrawn the ledger.
- `UBuildHudLayer`: field, `CreateAll`, `WireWindows`. `BuildActions.cpp`: action "Alerts" (DynamicLabel with count, IsActive when any, IsEnabled with runtime, Execute toggles).
- Tests: badge label counts; rows follow raise/clear; Go calls `SelectAndFocus` with the row's focus.

### Task 9: stage 2 PR (as Task 6, base `feature/ops-alerts`).

## Stage 3 - money events (`feature/ops-money-events`)

### Task 10: ledger and pricing publish

- `ULedger::Bus`, `UPricing::Bus` (set in Attach). `Post` publishes `FMoneyPostedEvent {EntryId, Category, Amount, Balance}` and, when the sign crosses, `FBalanceSignChangedEvent {bOverdrawn}`. `StepLandingFee` publishes `FLandingFeeChangedEvent {Old, New}`.
- Alerts pass dirtied by `MoneyPosted`. Toast: `BalanceSignChanged` -> "Overdrawn - building locked" (Warning) / "Back in credit" (Info) - and Task 5's Overdrawn-cleared toast is removed in favour of this one (one source).
- `UOpsEvents` delegates for the three; tests: Post publishes, sign crossing each way exactly once, fee step publishes.

### Task 11: UI stops polling money; inbox skips when hidden

- Bar balance/fee refresh on `OnMoneyPosted` / `OnLandingFeeChanged` (a dirty flag the tick reads) instead of revision + multiplier compare.
- `UOfferInboxWidget::TickPanel` returns early when not shown; the demand strip resamples on `OnLandingFeeChanged`, `OnAirlineSatisfaction`-equivalent (via `OnMoneyPosted`? no - add `UOpsEvents::OnDayEnded` and `OnAirlineSatisfaction` Presentation delegates) or the game hour changing.
- Tests: hidden inbox runs no refresh (counter); strip resamples on fee change and not on a quiet frame; bar balance updates after a Post with no revision compare (mutation: drop the subscription -> red).

### Task 12: stage 3 PR (as Task 6, base `feature/ops-alerts-ui`).
