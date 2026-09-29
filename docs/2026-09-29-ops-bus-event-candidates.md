# Ops bus: event candidates and per-frame work

Survey of main `76412bff` (2026-09-29), after the ops event bus landed (#403, #406, #407). Two questions:
which state changes happen that nothing announces (event candidates), and which per-frame work is
really waiting for an event. Key claims were checked against the code; line numbers are as of that
commit and will drift.

**Rule for adding an event:** only with a real consumer the same day. The bus's wiring test
(`AirportOps.Present.Bus.EveryEventHasASubscriber`) requires one, and the spec forbids the reverse
too ("an event with no publisher is the declared-never-consumed bug in reverse").

## 1. Event candidates, ranked

### Player must act, and is never told (consumer: toasts)

Toasts hear only `Notification` and `ArrivalRefused` today (`ToastStackWidget.cpp:44-45`).

| Event | Where | Seen today |
|---|---|---|
| `JobUnserviceable {aircraft, stand, why}` | `JobBoardBid.cpp:442` | Warning log only |
| `FlightStranded {flight, agent}` | `FlightBoard.cpp:591` | Warning ("retire it to free the flight") |
| `HeldStandLost {flight, stand}` | `StandAllocator.cpp:105` | Warning ("an aeroplane is coming for a stand nobody is holding") |
| `AirlineAvailability {airline, canCome, why}` | `OfferGenerator.cpp:181/194` (`bCouldCome` flip) | log on transition; an empty inbox once cost a PIE session to diagnose |
| `BuildRefused {what, why}` | 7 sites in `RoadEditFacade*.cpp` (Facade :700, :775; Surfaces :159, :279, :490, :832; Redo :88) | log only - the classic "does nothing" |
| `DeadlockDetected {members, allAircraft, resolved}` | `GroundTrafficDeadlock.cpp:344`, :454-471 | log; its comment calls an all-aircraft cycle "the input to the build-tool warning" |

Also: the key-7 land refusal shows no toast - `AcceptImmediate` never reaches `OnArrivalRefused`
(`OpsRuntime.cpp:754`).

### Gameplay

- **`TurnaroundEnded {aircraft, stand, outcome (fuelled/part/unfuelled), delivered, wanted, why}`** -
  `JobBoard.cpp:1134-1144`. The code says the shortfall "is C's to score" (:1131); the airline roster
  should score it.
- **`OfferAccepted {flight, airline, stand, arrivesAt}`** - `FlightBoard::Accept` :161. The one offer
  decision with no event (declined has one).
- **`RunwayFreed {runway}`, `StandFreed {pose node}`** - new Airside native delegates, bridged by
  `UOpsRuntime`. Runway has **two** release sites: take-off (`GroundTraffic.cpp:1426`) and a crossing
  clearing (`TrafficClaims.cpp:485`), which bumps no revision - the reason the arrival queue asks the
  runway live every frame (`GroundTraffic.h:446-453`). Stands: the `bStandsMayHaveFreed` sites
  (`GroundTraffic.cpp:460, 1077, 1309`, `GroundTrafficRebuild.cpp:463`). With both covered,
  `TickQueue` can become a pass - spec stage 4 done properly.

### Money

- **`MoneyPosted {category, amount, balance}`** - `ULedger::Post` (`Ledger.cpp:17`), the one funnel for
  every fee, charge, credit and reversal.
- **`BalanceSignChanged {overdrawn}`** - derived in `Post`; overdrawn locks placement
  (`Ledger.cpp:176`). UI polls it (`LedgerViewModels.cpp:61`).
- **`LandingFeeChanged {old, new}`** - `Pricing.cpp:37`; the bar polls the multiplier
  (`BuildBarWidget.cpp:548`).

### Lower value

`FlightPhaseChanged` (generalises `FlightAirborne`; audio, stats), `TurnaroundStarted`,
`JobFinished`, `VehicleStateChanged`, `FleetChanged`, `RunwayInUseChanged`
(`RoadEditFacade.cpp:845`), `GraphRebuilt {replanned, truncated, stranded}`
(`GroundTrafficRebuild.cpp:473`), `FlightCancelled`, `OfferMade`, typed `Saved`/`Loaded`,
`DaylightChanged` (`SimClock.cpp` band edges; sun, audio), `UnstickApplied`.

### Keep as they are

- **Revision counters read by viewmodels and caches** - `UFlightBoard::Revision`, `ULedger::Revision`,
  `UJobBoard::FleetRevision`, `OccupancyRevision`, `GetGuidelineRevision`, `ToolReadoutRevision`. A
  deliberate cheap gate; events complement them. `OccupancyRevision` cannot replace `RunwayFreed` (the
  crossing gap above).
- **Synchronous reads** - `FlightBoard->Fuel->CouldServe`, `JobBoard->LitresOwedFor`,
  `OfferGenerator->AirlineFactorOf`, `FlightBoard->Dispatcher`. Queries, not notifications ("a rate is a
  value asked for when it is needed").
- **Ordered commands** - `AgentRescue` calling the boards ("THE FLIGHT FIRST"). It may publish an event
  afterwards.

## 2. Per-frame work that should be event- or revision-driven

| # | Site | Waste today | Fix |
|---|---|---|---|
| 1 | `UGroundTraffic::ReplanHeldTaxiOuts` (`GroundTraffic.cpp:1169`) | `FindNearestNode` + `DeparturePlanner::PlanAny` **every substep, indefinitely**, per aircraft held with no route | Remember the guideline revision at the last failed attempt, as `UJobBoard`'s `RefusedAtRevision` does. Airside-local; the worst offender |
| 2 | `UOfferInboxWidget::TickPanel` (`OfferInboxWidget.cpp:169`) | Runs **while hidden**; `SampleDemand` = 24 `TotalRateAt` + allocation every frame | Skip when hidden; resample the strip on `DayEnded` / `AirlineSatisfaction` / fee change / hour |
| 3 | `UInspectorWidget` cards (`InspectorWidget.cpp:170`) | Stand: `DescribeStand` walks every frame. Depot: `DescribeDepot` strings for every vehicle and job every frame. Runway/taxiway formatted every frame. Aircraft: job and turnaround lookups ungated | Stand/runway/taxiway: key on edit + occupancy revision. Depot: key on a job board step counter + game minute |
| 4 | `ULandAircraftPanelWidget` (`:219`) | `LandChoices::Build` plans every type every frame while open | Key on guideline + occupancy revision + focus |
| 5 | `UFlightBoard::TickOffers` / `TickQueue` (`FlightBoard.cpp:76`, `:256`) | Copies `Flights` and builds and sorts `Queue()` every frame, even when empty | Early-out on `OfferedCount` / an inbound count now; queue becomes a pass with `RunwayFreed` |
| 6 | `UOpsRuntime::Tick` `SetSimTimeScale` (`OpsRuntime.cpp:556`) | Same double every frame; `ApplySpeed` already sets it | Set in `Attach` / `ApplySpeed` only |

### Local caching, not bus work

- `BuildBarWidget` `RefreshClock` (a Printf per frame) and `RefreshVariantsFor` (an FName signature
  per frame).
- Editor `RoadBuildEditorTool::DrawHUD` rebuilds the readout per frame with no key cache (the runtime
  has `FToolReadoutKey`).
- `RoadDrawTool::Tick` re-runs `RoadPlacement::Validate` with a still cursor.
- `UAirsideTraffic::Advance` calls `SetMotion` on parked agents.
- `ASunDriver` does `FindComponentByClass` every frame.
- `UToastStackWidget::TickFeed` could early-out when empty.
- `UUiWindowHost` ticks hidden panels.

### Correctly per-frame

Clock advance, agent motion, arbitration and claims, the deadlock stall scan, the offer countdown (real
seconds, "PAUSE STOPS IT; SPEED DOES NOT"), toast fades, camera, hover and cursor tools, animation.

### Already event-driven or gated

Surface, plot and buildings presenters (facade/topology delegates), `ReofferStands`
(`bStandsMayHaveFreed`), the job board pass, ledger rows, bar balance, `VerdictFor`,
`ClearanceFor`, tool readout, frame context, repeating clock timers.

## Suggested batches

1. Toast consumer + the six "player must act" events.
2. `RunwayFreed` / `StandFreed` from both release sites; arrival queue as a pass (stage 4).
3. `TurnaroundEnded` scored by airlines; `OfferAccepted`.
4. Money events; bar and ledger panel stop polling.
5. Per-frame fixes 1-4 (Airside 1, UI 2-4).
