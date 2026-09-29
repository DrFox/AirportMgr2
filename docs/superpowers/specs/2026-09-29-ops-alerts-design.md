# Ops alerts and money events - design

2026-09-29. Agreed in chat section by section. Base: main `76412bff` (ops event bus #403/#406/#407
merged). Source list: `docs/2026-09-29-ops-bus-event-candidates.md` - this spec is its batches 1
("player must act") and 4 ("money").

## 0. Why

The survey found problems the player must act on that are announced only by a log line (a stranded
flight, an unserviceable fuel job, a held stand deleted, an airline that cannot use the airport, an
all-aircraft deadlock), refusals that happen silently at commit (cannot afford; the key-7 land), and UI
that polls money state every frame. Toasts today hear only `Notification` and `ArrivalRefused`.

User rulings (2026-09-29):

- Standing problems reach the player as a **toast when they start AND a standing alert list** that
  stays until the condition clears. One-off facts stay toasts.
- The alert list lives behind a **badge on the bottom bar that opens a window**; an alert's "Go"
  moves the camera to its subject.
- Alerts are **derived from state** (approach C), not raised and cleared by paired events.
- Money: **Overdrawn is a standing alert**, plus a toast on the change; money events replace the UI's
  money polling. **No per-fee toasts.**

This brings back the `Alert` kind `NotificationCentre.h` records as cut in #105 for having "no
producer ... and no surface" - this spec supplies both.

Rejected, with reasons:

- **Paired onset/clear events per site (A)** - every way a condition can end needs its own publish;
  one missed path leaves an alert stuck for ever. This is the coverage trap the runway crossing set
  for stage 4 (`GroundTraffic.h:446-453`).
- **A generic `FAlertEvent` published by the boards (B)** - the same missed-clear risk, and the sim
  would author UI sentences.
- **Toasts only** - a condition the player missed would be gone from the screen while still true.
- **All ~50 build refusal sites** - they are explained before the click by the tool readout and the
  preview colour (`RoadDrawTool.cpp`, `RunwayTool.cpp`, `PlotPlaceTool.cpp`, `StandPlotTool.cpp` call
  the `Why*` evaluators). Only "cannot afford at commit" is silent.

## 1. The alert model - `UOpsAlerts`

`AirportOps/Model/OpsAlerts.h`, world-free, owned by `UOpsRuntime` (the `UAirlineRoster` shape).
Registered as the bus pass `"Alerts"`, after `"JobBoard"`.

```
enum class EAlertKind : uint8 { FlightStranded, VehicleStranded, JobUnserviceable, HeldStandLost,
                                AirlineCannotCome, Deadlock, Overdrawn };
struct FOpsAlertKey { EAlertKind Kind; int32 Id; FName Name; };  // one alert per condition per subject; Name for airlines
enum class EAlertFocusKind : uint8 { None, Agent, Entity, Point };
struct FAlertFocus { EAlertFocusKind Kind; int32 Id; FVector2D Point; };
struct FOpsAlert { FOpsAlertKey Key; FText Text; FAlertFocus Focus; double RaisedAt; };
```

| Kind | True while | Key id | Focus | Source (exists / new) |
|---|---|---|---|---|
| FlightStranded | a live flight's agent is `EAgentPhase::Stranded` | flight id | Agent | `UFlightBoard::Live()`, `UGroundTraffic::FindAgent` |
| VehicleStranded | a service vehicle's agent is Stranded | vehicle id | Agent | `UJobBoard::GetVehicles()` (+ `IsStranded` made public) |
| JobUnserviceable | a job is `Unserviceable`; text `UJobBoard::RefusalText(Why)` | job id | Entity (stand) | `UJobBoard::GetJobs()` |
| HeldStandLost | an Accepted/Inbound flight holds a stand whose entity is gone or has no pose node | flight id | Point (`ApproachFocus`) | predicate from `UStandAllocator::Reapply` |
| AirlineCannotCome | an airline's `FAirlineOfferState::bCouldCome` is false | Name = airline definition name (an index would change on re-attach) | None | NEW `UOfferGenerator::DescribeWhyNot(FName) const` - the sentence `OfferGenerator.cpp:167` logs |
| Deadlock | an all-aircraft wait cycle exists | lowest member agent id | Agent | NEW `UGroundTraffic::CurrentDeadlocks() const` (Airside) |
| Overdrawn | `ULedger::Balance() < 0` ("building locked") | 0 | None | `ULedger::Balance()` |

**`UGroundTraffic::CurrentDeadlocks()`** returns every all-aircraft wait cycle as member id arrays.
It rebuilds the resolver's functional wait-for graph from agent state (edge only when
`GetStalledSeconds() > Rules.StallSeconds && GetWaitingOn() != 0`, as `GroundTrafficDeadlock.cpp`
does) and applies the same all-aircraft test. It is shared with `FDeadlockResolver` as a static, so the
two cannot disagree about what a cycle is. Today cycles live only in locals and log lines.

**The pass**: build the full set of true conditions, diff it against the current set by key, publish
`FAlertRaisedEvent {Key, Text, Focus}` and `FAlertClearedEvent {Key}`, update the set.

- **Dirtied by:** `FAgentPhaseEvent`, `FNetworkChangedEvent`, the JobBoard pass having run, flight
  board changes (the offer events and `FFlightAirborne`), the offer minute (airline availability),
  `FMoneyPostedEvent`, and attach/load (`MarkAllDirty`).
- **Deadlock backstop:** stall time grows with no event, so while any agent is stalled a
  `Clock.Every(10 game s)` marks the pass dirty. Armed and cancelled by the pass itself.

**Not saved.** On load the set is emptied and the first pass re-raises everything true.

**One wording owner:** the text is built here, from the boards' existing sentences.

## 2. One-off events and money

| Event | Published at | Consumer |
|---|---|---|
| `FBuildRefusedEvent {What (FString), Why (EBuildRefusal), Price, Balance}` | the affordability sites in `RoadEditFacade*.cpp` (Facade :700, :775, drag commit :1424/1436; Surfaces :86, :159, :279, :490, :832). Airside, so via a NEW native delegate `URoadEditFacade::OnRefused`, bridged in `UOpsRuntime::Attach` | Warning toast "Can't afford <What> (<price>; balance <balance>)" |
| `FLandRefusedEvent {Why (EArrivalRefusal)}` | `UOpsRuntime::LandNear` when `AcceptImmediate` refuses (today logged only) | Warning toast with `ArrivalPlanner::DescribeRefusal` |
| `FMoneyPostedEvent {EntryId, Category, Amount, Balance}` | `ULedger::Post` - the one funnel (`ULedger` gains `FOpsEventBus* Bus`) | bar balance, ledger panel, alerts pass (Overdrawn) |
| `FBalanceSignChangedEvent {bOverdrawn}` | `ULedger::Post`, when the balance crosses 0 either way | toast "Overdrawn - building locked" / "Back in credit" |
| `FLandingFeeChangedEvent {Old, New}` | `UPricing::StepLandingFee` (`UPricing` gains `Bus`) | bar fee display; inbox demand strip resample |

`EBuildRefusal` (Airside, new): `CannotAfford` only, an enum so later refusals can join it.

**Polling removed:**

- Bar balance and fee (`BuildBarWidget.cpp:526-571`) refresh on `MoneyPosted` / `LandingFeeChanged`
  instead of comparing `Ledger->Revision()` and the multiplier every frame.
- Ledger panel rows keep their `Revision()` gate - the viewmodel pattern, complemented not replaced.
- Offer inbox `TickPanel` skips entirely when the panel is hidden; its demand strip resamples only on
  `LandingFeeChanged`, `AirlineSatisfaction`, `DayEnded` or the game hour changing (today 24
  `TotalRateAt` + an allocation every frame, even hidden).

**How the UI hears the bus:** `UOpsEvents` gains dynamic delegates `OnAlertRaised`, `OnAlertCleared`,
`OnBuildRefused`, `OnLandRefused`, `OnMoneyPosted`, `OnBalanceSignChanged`, `OnLandingFeeChanged`, each
fed by one Presentation-tier subscription in `WireBus` (rule 31). The payloads are USTRUCTs so the
dynamic delegates can carry them.

## 3. UI

**Toasts** (`UToastStackWidget`): `AlertRaised` -> Warning toast with the alert text. `AlertCleared` ->
an Info toast for `Overdrawn` only ("Back in credit"); other clears are silent. `BuildRefused`,
`LandRefused` -> Warning toasts.

**Badge:** a new `FBuildAction "Alerts"` in `BuildActions.cpp`, the Ledger action's shape.
`DynamicLabel` "Alerts (3)" / "Alerts"; `IsActive` lit while any alert exists; `IsEnabled` needs a
runtime; `Execute` toggles the window. It reads a mirror of the alert set on the controller, kept by
`OnAlertRaised` / `OnAlertCleared` - never a per-frame query.

**Window:** `UAlertsPanelWidget : UAirportMgrPanelWidget`. AboveBarLeft, closable, not modal. Rows as
`ULedgerPanelWidget` builds them (`BuildRow`, rows reused): warning icon, text, a "Go" button. It
repaints on raise/clear only. Added to `UBuildHudLayer::CreateAll` and `WireWindows` - both, a list
that must agree.

**Go:**

- NEW `UBuildCameraComponent::FocusOn(FVector2D)`: sets `TargetView.Focus`, leaves watch mode; the
  existing ease glides there.
- NEW `FBuildSession::Select(ESelectionKind, int32)` plus a controller forwarder; the inspector opens by
  itself, because it shows whatever is selected.
- Agent -> focus on `GroundPosition()` and select it. Entity -> focus on its position and select it.
  Point -> focus only.
- No focus: `AirlineCannotCome` opens the offer inbox; `Overdrawn` opens the ledger.

Out of scope: sound, sorting or filtering alerts, per-kind icons, dismissing alerts.

## 4. Failure, testing, stages

**Save/load:** nothing new is saved. The UI mirror is cleared on load and on attach, and rebuilds from
the re-raised events.

**Failure handling:**

- "Go" on a subject that has gone: no camera move, Verbose log; the row stays until the next pass
  clears it.
- A condition that flips (a job re-offered on a network change, then unserviceable again) raises and
  clears each time. Accepted: it really recurred.
- Deadlock query cost: stalled agents only, and only when dirty or on the backstop while something is
  stalled.

**Testing** (world-free unless marked):

- `UOpsAlerts`, per kind: raised while true, cleared when not, no duplicate on re-detection, the same
  set rebuilt after a load.
- `CurrentDeadlocks`: an all-aircraft cycle found; a mixed cycle ignored; below the stall threshold
  ignored; it agrees with the resolver on the same state.
- `DescribeWhyNot`: returns the sentence the log line prints.
- Money: `Post` publishes `MoneyPosted`; crossing 0 each way publishes `BalanceSignChanged` exactly
  once; `StepLandingFee` publishes `LandingFeeChanged`.
- Composition (runtime attached): an unaffordable facade commit -> Warning toast; key 7 on a
  runway-less field -> toast; a job made unserviceable (depot deleted) raises the alert, restoring it
  clears it.
- UI: badge label counts; window rows follow raise and clear; "Go" on an agent alert sets camera
  focus and selection; the hidden inbox runs no refresh (counted); the demand strip does not resample
  on a quiet frame.
- Mutation checks: drop the `MoneyPosted` subscription -> the bar test is red; drop the alerts pass's
  dirtying on phase changes -> the stranded alert test is red.
- `AirportOps.Present.Bus.EveryEventHasASubscriber` covers every new event.

**Stages** (one PR each, stacked, visible first):

1. Alerts and toasts - `UOpsAlerts` with all seven kinds, `CurrentDeadlocks`, `DescribeWhyNot`,
   `BuildRefused` / `LandRefused`, the toast wiring.
2. Badge, window and "Go" - `FocusOn`, `Select`, `UAlertsPanelWidget`, the bar action.
3. Money - the three events, the bar / ledger / inbox polling removal, the inbox skipping while hidden.

(Overdrawn needs `MoneyPosted` to dirty the pass. Stage 1 dirties it on the existing events and the
offer minute; stage 3 adds the exact trigger.)

**PIE check (stage 1):** place a stand with no road to the fuel depot and accept a fuelled flight.
Expect a Warning toast "... no road ..." and `LogOpsBus: Bus: + AlertRaised {JobUnserviceable, ...}`.
Join the road: expect `Bus: + AlertCleared {JobUnserviceable, ...}`.
