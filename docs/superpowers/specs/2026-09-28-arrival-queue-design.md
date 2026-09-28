# Arrival queue: a busy runway queues, never refuses

2026-09-28. Status: design approved in conversation 2026-09-28. Chunk B's first slice, pulled
forward from the offers redesign (`2026-09-28-offers-and-demand-design.md`); stacked on #378.

## Problem

Measured from code on `feature/offers-and-demand` (a69e1bca):

1. **A busy runway greys out Accept.** The row's verdict is `ArrivalPlanner::Plan` with live
   occupancy, and Plan returns `RunwayOccupied` (step 1b, `ArrivalPlanner.cpp:307`) whenever
   an aircraft holds the strip - a landing rolling out, a departure lining up.
2. **A flight due while the runway is busy is lost.** `UFlightBoard::DispatchNow` releases the
   stand hold, the dispatcher refuses (the same Plan, same step), and the flight "keeps no
   stand" (`FlightBoard.cpp` DispatchNow) - Accepted forever, never retried, never landing.
3. **Nothing shows what is coming.** Accepted flights vanish from the UI until they appear on
   final.

## Decisions (user, 2026-09-28)

| # | Question | Ruling |
|---|---|---|
| 1 | Where a waiting aircraft is | **Off-map, abstract** (GDD section 7). Spawns on final when cleared, as today. Visible holding patterns are a later ATC payoff. |
| 2 | Can a waiting flight give up | **No diversion.** The stand is the gate: an offer is acceptable only when a suitable stand is free, and a queued flight keeps its hold, so every queued flight has somewhere to go. |
| 3 | How the queue is shown | **An ARRIVALS section under the offers**, same card: every accepted flight until airborne, landing order, status and contract time left. |
| - | Departure vs arrival priority | **First come**, through the existing runway claims. Sequencing is the ATC tower's. |
| - | Where the logic lives | **`UArrivalSequencer`** in AirportOps `Model/`, owning no list; the board stays the one door onto dispatch. |

## Section 1: Accept ignores a busy runway

`ArrivalPlanner::Plan` gains a parameter, `ERunwayBusy RunwayBusy = ERunwayBusy::Refuse`.
`ERunwayBusy::Queue` skips step 1b's occupancy check and carries on to the length, exit and
stand steps. An enum, not a bool (CLAUDE.md: a phase is an enum; a mode at a call site reads).

- `UFlightBoard::WhyNotAcceptable` passes `Queue`. So the row greys out on `NoFreeStand` (and the
  permanent refusals) and never on `RunwayOccupied` - "you can't accept this yet" means exactly
  "no stand for it".
- Mapping `RunwayOccupied` to acceptable after the fact was rejected: Plan returns at 1b BEFORE
  the stand steps, so a busy runway would hide a real `NoFreeStand`.
- The runway-occupied test itself moves into one function, `ArrivalPlanner::IsRunwayBusy(Network,
  Near, Occupancy)`, which step 1b calls and the sequencer calls. One rule.

## Section 2: The queue

**Phase.** `EFlightPhase::Inbound` ("between the accept and the ETA", entered by nothing
today) becomes **holding**: the ETA has passed, the flight is waiting for the runway, off-map.
New `UFlight::HoldingSince` (game time, saved) - for the row's "waited" and for C.

**At the ETA** (the board's `Schedule` callback, and `RearmSchedules` for an overdue flight on
load): the flight goes `Inbound`, `HoldingSince = Now`, and **keeps its stand hold**. It no
longer calls `DispatchNow` directly. Logged: `Flight %d (%s) holding: #%d in queue`.

**The queue is derived, not stored.** It is the board's `Inbound` flights ordered by
`HoldingSince`, ties by id - `UFlightBoard::Queue()`. No second list to save or keep in step
(CLAUDE.md: lists that must agree are one list).

**Clearing.** `UArrivalSequencer::Next(Queue, Network, Occupancy)` returns the FIRST flight in
queue order whose runway is not busy (`IsRunwayBusy` at the flight's own `ApproachFocus`), or
null. First-whose-runway-is-free rather than strictly the head, so a flight for a free second
runway is not stuck behind one waiting on a busy one. `UFlightBoard::TickQueue(Traffic, Network,
Clock)`, called every frame from `UOpsRuntime::Tick`, asks it once and clears at most ONE flight
per frame: `DispatchNow` as today (release the hold, dispatch, phase `Landing`). Logged:
`Flight %d (%s) cleared to land after %s holding`.

**A failed dispatch stays queued.** If the dispatcher refuses anyway (a graph mid-edit, a stand
race), the stand is re-held (`UStandAllocator::Reserve`) and the flight stays `Inbound` - the
fix for problem 2. If even the re-hold fails, it stays queued with no hold and says so in a
Warning; it is retried every frame, never dropped.

**Pause.** `TickQueue` does nothing while the clock is paused.

**The contract ticks throughout** without code: `AirborneBy` was fixed at the accept.

**Save.** Nothing new is saved beyond `HoldingSince`: phase and hold are already saved; the
queue order derives from them. `OnGraphRebuilt` re-applies holds for `Inbound` flights as well
as `Accepted` ones.

## Section 3: The ARRIVALS section

Under the offers in `OfferInboxWidget`'s card, a heading "ARRIVALS n" and one compact row per
flight from Accepted to Departing, in this order: holding (queue order, numbered), then
Accepted by ETA, then on the ground. No buttons.

- Line 1: `#1 CU 204  Saab 340B` and a status: `HOLDING`, `in 6 min`, `LANDING`, `TAXI IN`,
  `ON STAND`, `PUSHBACK`/`MANOEUVRING`, `TAXI OUT`, `DEPARTING`.
- Line 2: `waited 3 min - 52 min left` while holding, else `52 min left`; past `AirborneBy` it
  reads `12 min late` in Warning.
- A viewmodel (`UArrivalRowViewModel`, `UArrivalsViewModel`) beside the offer ones, fed from
  `UFlightBoard::Live()` and `Queue()`, rebuilt on board revision; text refreshed per frame.

## Testing

Model (world-free):

- Accept succeeds while the runway is held; the same field with its one stand taken refuses
  `NoFreeStand` while the runway is held (the stand check is not hidden).
- A flight due while the runway is held goes `Inbound`, keeps its hold, and lands once the
  runway clears (the regression for problem 2 - delete the queue and watch it drop the flight).
- Two queued flights clear in the order they joined; one per frame.
- A queued flight whose runway is free clears ahead of an earlier one whose runway is busy.
- A dispatcher that refuses leaves the flight `Inbound` with its stand held.
- Paused: nothing clears.
- Save/restore with a flight `Inbound`: it comes back queued, hold re-applied, and clears.
- Load with an Accepted flight past its ETA: it joins the queue, not a blind dispatch.
- `IsRunwayBusy` agrees with Plan's step 1b (the same function).

UI (composition):

- Row order: holding (numbered) before inbound before on-ground; status text per phase;
  "late" past `AirborneBy`.

## Out of scope

- Visible holding patterns, holding capacity, diversion, separation, departure priority - the
  ATC tower.
- Forecast stand availability at the ETA - no longer needed for arrival safety: the hold is taken
  at the accept.
