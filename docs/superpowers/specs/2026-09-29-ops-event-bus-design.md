# Ops event bus - design

2026-09-29. Status: agreed in chat section by section; user then went AFK and asked for autonomous
execution. Base: origin/main c67699dc.

## 0. Why

The user asked whether things could subscribe to events, and whether the sim could come off Tick
("always described to me as bad practice"). Goal, all three weighted equally:

1. **Gameplay reactions** - airlines, flights and vehicles react to what happened.
2. **Cost** - the sim costs ~nothing when nothing happens.
3. **Extensibility** - new systems (audio, stats, UI, later BP/mods) hook in without editing the boards.

What "off Tick" means here (agreed): one manager ticking a simulation that really changes every frame
is right - motion, arbitration and a real-seconds countdown stay. What goes is **polling**: re-checking
every frame state that only changes on a discrete event, and **deadlines checked every frame** that
`USimClock::At` exists for.

Rejected, with reasons:

- **Synchronous delegates only (grow `UOpsEvents`).** Every publisher inherits the #193 re-entrancy
  hazard (a listener retires an agent mid-loop), and order is subscription order - the
  "service first, then the bus" rule in `UOpsRuntime::OnAgentPhase` has to be hand-kept at each site.
- **Tag-keyed router (Lyra `GameplayMessageSubsystem` style).** No defined order, tags are strings,
  subscribers cannot be grepped, and it is sample code not an engine plugin. Fails "check where a list
  is CONSUMED" by construction.
- **`FInstancedStruct` payloads.** An open set: "which events exist?" has no one answer.

## 1. The bus

`FOpsEventBus`, `AirportOps/Model/OpsEventBus.h`, plain C++, owned by `UOpsRuntime`. World-free.

**Events are one closed list** - `using FOpsEvent = TVariant<...>` over small structs (ids and figures,
no pointers to UObjects that may die). `Subscribe<T>` is compile-checked against the list. Adding an
event is one edit to the list plus its struct. "Lists that must agree are ONE list."

**Subscribe** - `Bus.Subscribe<T>(EOpsTier, FName Subscriber, TFunction<void(const T&)>)`. Legal only
inside `UOpsRuntime::WireBus` (`check` otherwise, and `check` if called during a drain). `WireBus` is
the single place the subscription map can be read. At wire time each subscription logs one line.

**Passes** - `Bus.RegisterPass(FName, TFunction<void()>)` and `Bus.MarkDirty(FName)`. A handler never
does bulk work; it marks a pass dirty, and each dirty pass runs once after the drain round. Five jobs
opening in one frame cost one bidding pass.

**Publish** - `Bus.Publish(FOpsEvent)` only enqueues. Safe from inside Airside's agent loop, a clock
callback, or a handler.

**Step** (in `UOpsRuntime::Tick`):

1. `Clock->Advance` - `At`/`Every` callbacks publish.
2. Drain round: take the whole queue; for each event in publish order run its Sim handlers, then
   Reaction, then Presentation. Events published meanwhile go to the next round.
3. Run every dirty pass once (registration order), clearing its flag first.
4. If the queue is non-empty or a pass was re-dirtied, repeat 2-3. **Cap: 8 rounds.** Hitting it logs
   an Error naming the event types still queued and the dirty passes, and they carry to next frame.
5. Continuous work (offer countdown in real seconds).

**Tiers**

| Tier | Who | Rule |
|---|---|---|
| Sim | JobBoard, FlightBoard | mutate simulation state |
| Reaction | UAirlineRoster | read Sim's settled result; own state only; may publish |
| Presentation | `UOpsEvents` (BP/UMG), toasts, audio | read-only; `ensure` + drop if it publishes |

**Airside bridge.** Airside must not learn ops exists. It keeps native delegates
(`OnAgentPhaseChanged`, `OnArrivalRefused`, new `OnRunwayFreed`), and `UOpsRuntime` publishes each onto
the bus - the only place the two meet. The motion ticker (`ARoadNetworkActor::Tick`) and the ops ticker
have no fixed order; an Airside event is handled at the next ops drain, at most one frame later. That
is deliberate: no ops handler runs inside Airside's loop. The tier order replaces
`OnAgentPhase`'s hand-written "SERVICE FIRST, THEN THE BUS".

**Logging** - `LogOpsBus`: one Verbose line per event dispatched, one Log line per subscription at
wire time, Error on the cap, Warning from the safety pass (§2).

## 2. What moves off Tick

**To passes/handlers**

| Today (per frame) | Becomes | Triggered by |
|---|---|---|
| `UJobBoard::SyncFleet` | pass | `FNetworkChanged`, `FAgentGone`; once at attach/load |
| `AssignOpenJobs` | pass | `FJobOpened`, `FVehicleFreed`, `FFleetChanged`, `FNetworkChanged` |
| `RebidQueued` (already revision-gated) | pass, same triggers | |
| unserviceable re-offer (per-job revision compare) | handler | `FNetworkChanged` |
| Idle-with-queue retry | handler | `FVehicleFreed` -> `StartNext` |
| "Serving && CurrentJob==0" backstop | handler on `FJobFinished` | **open**: its real cause is pinned while planning; if it cannot be, it stays a named pass with its reason written at the site |
| `UFlightBoard::TickQueue` (runway asked live) | pass | `FFlightInbound`, `FRunwayFreed`, `FStandFreed`, `FNetworkChanged` |
| `DepartTheReady` refusal retried per frame | pass | `FRunwayFreed`, `FNetworkChanged`, `FJobFinished`, `FTurnaroundDue` |
| `SetSimTimeScale` pushed per frame | handler | `FSpeedChanged` |
| Airside `ReplanHeldTaxiOuts` | Airside-local dirty flag | set on guideline rebuild (Airside has no bus) |
| Inspector `DescribeAgent`/`DescribeDepot` ungated | revision gate | same pattern as offer/ledger widgets |

**To `Clock.At`**

| Today | Becomes |
|---|---|
| per-vehicle `Now >= StepEndsAt` | `Clock.At(StepEndsAt)` -> `FVehicleStepDone`; handle on the vehicle (transient), cancelled on drop/reassign |
| `Now >= TurnaroundEndsAt` | `Clock.At` -> `FTurnaroundDue` |

**Stays on Tick** - `Clock->Advance`; Airside motion, `Arbitrate`, deadlock resolver; offer countdown in
real seconds (#105's ruling); clock text.

**Pause falls out**: a frozen clock fires nothing and nothing publishes. Today `UJobBoard::Tick` runs
paused.

**Safety pass, which reports itself.** `TickQueue` asks the runway live because `OccupancyRevision`
does not move for a taxiing aircraft's runway crossing (FlightBoard.cpp comment). If any way a runway
frees fails to publish `FRunwayFreed`, the queue strands. So while the arrival queue or the
departure-ready set is non-empty, `Clock.Every(30 game s)` runs the same passes; if a safety run
dispatches anything, it logs
`Warning: LogOpsBus: safety pass dispatched <what> - no event covered it`, and a test fails on that
Warning. A missing event becomes a named defect, not a stuck airport. Remove the net once quiet.

## 3. Airline satisfaction - the proving consumer

`UAirline` (`AirportOps/Model/Airline.h`), one per `UAirlineDefinition` in the scenario, held by
`UAirlineRoster` on `UOpsRuntime`, keyed by definition name. First runtime airline state.
`UFlight` gains `AirlineId` (FName) beside display `AirlineName` - a display text is not a key.

State: `Satisfaction` 0..1, start 0.5; last 5 changes with cause (for UI).

Reaction-tier handlers:

| Event | Effect (defaults) |
|---|---|
| `FFlightAirborne{Flight, LateBySeconds}` with `LateBy = AirborneAt - AirborneBy()` | on time (<= 0): +0.03; late: -0.02 per 10 game-min, capped -0.10 |
| `FOfferExpired{Flight, ELapseReason}` | `Ignored` -0.02; `NeverAcceptable` (no stand free all window) -0.01 |
| `FOfferDeclined{Flight}` | 0 - declining is legitimate; event still published for stats/UI |
| `FDayEnded` (from the existing daily `Every`) | drift 20% of the way to 0.5 |

Each change publishes `FAirlineSatisfactionChanged{Airline, Old, New, Cause}`.

**Effect**: `UOfferGenerator::RateAt` gains a per-airline multiplier `lerp(0.5, 1.5, Satisfaction)`,
multiplied with `DemandFactor`. **Floor airlines never below 1.0x** - `bIsFloor` exists so the airport
is never empty. The generator READS the roster (a value, read on demand); it does not subscribe.

**Seen**: the offer inbox row gets one line, e.g. `Satisfaction 62% ▼ late departure (-0.04)`.

**Tuning**: every figure above is a `UPROPERTY` on `UScenario` beside `FuelVehicles`, commented as
unjudged (2026-09-29).

NOT `FFlightAbandoned`: `ELapseReason` covers offers only and no accepted contract lapses today -
an event with no publisher is the declared-never-consumed bug in reverse. It arrives with contracts.

Out of scope: cross-airline reputation, contracts/negotiation, personalities, lateness fines.

## 4. Save/load, failure, testing

**Save/load**

- The queue is never saved. `SaveToSlot` drains before snapshotting (Airside events can arrive
  between ops steps). `LoadFromSlot` discards the queue after `Restore` and logs how many were dropped
  - `ClearAgents` publishes Gone for agents that no longer exist.
- `Clock.At`s follow `USimClock`'s existing rule (SimClock.h: entries not saved; each system re-arms
  from its own state). Vehicles load Idle and turnarounds are not restored today, so JobBoard has
  nothing to re-arm; the rule gets a test anyway.
- After attach and after load, every pass is marked dirty once.
- Airlines: `IOpsPersistent` blob `"Airlines"` (satisfaction + history). Missing blob -> 0.5 each.
  No player saves exist (2026-09-23); the break is still noted in the PR.
- Found, NOT fixed here: flights restored in Landing..TaxiOut keep an `AgentId` whose agent was
  cleared. Filed as its own issue.

**Failure handling**

- Round cap: Error + carry over.
- Presentation publishes: `ensure`, drop.
- Subscribe outside `WireBus` or mid-drain: `check`.
- Stale ids: a handler that cannot find its flight/vehicle/airline logs Verbose and skips - an event
  is a fact about the past.

**Testing** (Model/, world-free unless marked)

- Bus: tier order; publish-in-handler lands next round; cap + Error; pass coalescing (5x
  `FJobOpened` -> one bidding pass via `BidCallCountForTest`); presentation publish dropped.
- Wiring (composition): after `WireBus` every type in `FOpsEvent` has >= 1 subscriber, by name.
- Per migrated poll: vehicle step ends exactly at `StepEndsAt`; paused -> nothing fires; freed runway
  dispatches the queued arrival in the same step; the safety-pass Warning fails a test (unbuffered
  log spy).
- Mutation check: delete a subscription, its test goes red.
- Airlines: late -> down, on time -> up; floor >= 1.0x; day drift; save round-trip; offer rate actually
  changes (composition).
- Refactor contract: `UE_LOG` count does not fall; once-per-reason refusal logs survive.

**Stages** (one PR each, stacked, visible first):

1. Bus + `WireBus` + bridge today's `OnAgentPhase`/`OnArrivalRefused`/speed. No behaviour change.
2. Airlines: runtime roster, satisfaction, rate multiplier, offer-row line.
3. JobBoard migration: passes, `Clock.At` deadlines, pause.
4. FlightBoard: `FRunwayFreed`, `TickQueue` pass, safety net.
5. Leftovers: Airside replan flag, inspector gating, `SetSimTimeScale`.

**PIE check (stage 2)**: force a late departure; expect a `LogOpsBus` line for
`FAirlineSatisfactionChanged` and the offer row showing `▼ late departure`.
