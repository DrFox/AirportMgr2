# Ops event bus, stage 3 - the job board off polling

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Checkbox steps.

**Goal:** `UJobBoard` does no work on a frame where nothing happened: its per-frame `Tick` becomes one coalesced bus pass, dirtied by agent phase changes, network changes and one scheduled deadline.

**Architecture:** The body of `UJobBoard::Tick` is ORDER-DEPENDENT (due steps -> bids -> re-bids -> starts -> re-offers -> departures; each step's comment says why it is where it is). Re-plumbing each step onto its own event would re-derive that order across handlers and is where regressions would hide. So the sequence stays whole as `UJobBoard::Step`, which now also answers "is anything left unresolved?", and the ops runtime runs it as the `JobBoard` pass. Deviation from spec §2's per-step table, stated here and in the PR: the table's TRIGGERS are kept; its per-step split is not.

**Spec:** `docs/superpowers/specs/2026-09-29-ops-event-bus-design.md` §2. Base: `feature/airline-satisfaction`.

## Global Constraints

As the stages 1-2 plan. Branch `feature/jobboard-events` from `feature/airline-satisfaction`.

## Review Focus

1. A vehicle's serve/refill finishing exactly when due with no other event (deadline must fire). Test: Task 2.
2. A departure refused (runway busy) with nothing else happening - must retry every frame as today. Test: Task 2 (unresolved re-dirty).
3. A new depot placed on a quiet airport - fleet must seed. Test: existing FuelServiceWired + Task 2 idle test (depot placed after attach).
4. Load: pass must run once after load and re-arm the deadline. Covered by MarkAllDirty + existing save tests.
5. Paused: nothing due fires (clock frozen). Retry passes keep running while paused, as `Tick` did (not changed here).

### Task 1: bus - dirty for the NEXT drain

- `FOpsEventBus::MarkDirtyNextDrain(FName)`: sets a deferred flag; `Drain()` promotes deferred flags to dirty at its start. A pass that re-marks ITSELF with `MarkDirty` would run again next round and burn the cap; next-drain is the "try again next frame" a retry needs.
- Test `AirportOps.Model.Bus.NextDrainIsNextFrame`: a pass that calls `MarkDirtyNextDrain` on itself runs once per `Drain`, never twice in one, and no cap Error.

### Task 2: `UJobBoard::Step` + the `JobBoard` pass

- `bool UJobBoard::Step(Traffic, Network, Clock)` = today's `Tick` body; returns true when something must be looked at again next frame: a vehicle `Idle` with a queue after `StartNext` (a refused dispatch), a vehicle `Serving` with `CurrentJob == 0` (the review #1 backstop), an `Open` job left after bidding, or a due turnaround `DepartTheReady` could not send. `Tick` forwards to `Step` (world-free fixtures keep driving it). `++StepCount` for `StepCountForTest()`.
- `double UJobBoard::NextDeadline(double Now) const`: the earliest `StepEndsAt` of a Serving/AtFacility vehicle and `TurnaroundEndsAt` of a turnaround, strictly after Now; `TNumericLimits<double>::Max()` for none.
- New event `FNetworkChangedEvent { uint32 GuidelineRevision; }`. `UOpsRuntime::Tick` publishes it when the attached network OBJECT or its guideline revision differs from last step (one compare a frame - the same signal the board's own re-bid and re-offer gates read, so it cannot disagree with them).
- WireBus: `RegisterPass("JobBoard", ...)` - runs `Step` when a live model exists, then re-arms ONE `Clock.At(NextDeadline)` that marks the pass dirty (cancel the previous handle first), and `MarkDirtyNextDrain("JobBoard")` when `Step` returned true. Sim subscriptions that `MarkDirty("JobBoard")`: `FAgentPhaseEvent` (after the board's own handler), `FNetworkChangedEvent`. Attach gets its catch-up from the first-frame `FNetworkChangedEvent` (Detach resets `SeenNetwork`); Load `MarkAllDirty`s.
- `UOpsRuntime::Tick` no longer calls `JobBoard->Tick`.
- Tests (composition, `OpsRuntimeBusTest.cpp`):
  - `AirportOps.Present.Bus.QuietBoardDoesNoWork`: attached airport with a depot, no aircraft; after the first steps, 300 more `Tick`s leave `StepCountForTest` unchanged.
  - `AirportOps.Present.Bus.DeadlineWakesTheBoard`: a vehicle put `Serving` with `StepEndsAt = Now + 30` through the world-free seam (or the FuelServiceWired fixture run until Serving), then ticks with no events: it finishes on the first step at or after 30 s, not before.
  - Existing `AirportOps.Present.FuelServiceWired` (full serve round trip through the runtime) is the regression gate.
- Mutation: drop the deadline re-arm -> `DeadlineWakesTheBoard` red.
