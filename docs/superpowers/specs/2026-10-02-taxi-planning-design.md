# Space-time taxi planning - design

2026-10-02. Approved in conversation section by section (owner); owner asked for implementation
to continue with batched test runs. Directional route locks follow as their own spec and become
this design's fallback.

## Why

On `M_ScaleGatwick` (40 stands, 2 runways, a stand taxilane parallel to the main taxiways) an
arrival and a departure met head-on on the stand taxilane and jammed ("All-aircraft Deadlock among
agents [1, 5]: no member can turn", log 2026-10-02 10:32). Root causes, from the code:

- An aircraft claims only its braking distance ahead (`FClaimPass`, TrafficClaims.cpp ~400-425),
  so two aircraft can enter one two-way stretch from opposite ends.
- No reversing on taxiways; the resolver can only turn a member stopped AT a node
  (DeadlockResolver.h ~195-205). A mid-edge head-on is permanent.
- Taxi-in and departure routes ignore traffic (RoutePolicy.cpp ~47-54).

The spike (branch `feature/taxi-deadlock-spike` ac4562b4, `Airside.Spike.TaxiDeadlock.*`) measured
it headless on the Gatwick layout, 2 h game time:

| | Baseline | Directional locks | Crude space-time (no order enforcement) |
|---|---|---|---|
| 40 mov/h permanent jams | 1 (27 stuck) | 0 | 0 |
| 80 mov/h permanent jams | 1 (53 stuck) | 0 | 1 (25 stuck) |
| 80 mov/h departure wait mean / p95 | - | 793 s / 1920 s | 419 s / 1534 s |

Locks are deadlock-free but throttle throughput; space-time keeps throughput but needs ORDER
ENFORCEMENT (Hönig et al. 2019, action dependency graph) to stay deadlock-free under delay.
Rejected by the owner: "reserve to the next junction" (deadlocks AT junctions where aircraft
hold) and whole-route locks alone (runway exit, throughput).

## Rulings (owner, 2026-10-02)

1. Scope: AIRCRAFT ONLY. Vehicles keep today's claims; road-taxiway crossing nodes are planned
   resources so a vehicle yields when an aircraft is due.
2. Priority: ARRIVALS FIRST. An arrival may revoke the plan of a departure that has not started
   pushback; a moving aircraft's plan is never revoked.
3. Clearances read in taxiway names (taxiway-naming spec, PR #524).
4. Map visualisation for the SELECTED aircraft only (zoomed-out readability ruling).

## Section 1 - Components (Model/, world-free)

- **`FTaxiReservations`** - per RESOURCE an ordered list of time windows (holder, from, to). A
  resource is what claims already use: a guideline edge (a two-way edge is ONE resource, both
  directions) and a node. Plain data: insert, remove, free intervals of resource R.
- **`FTaxiPlanner`** - SIPP (safe-interval path planning) over the guideline graph: earliest
  arrival through the table's free intervals. Waiting is allowed only at the start (stand /
  holding queue) and at plain nodes that are not junction boxes. Edge traversal times come from
  `FSpeedProfile` (the drivability authority - bends and stops included), plus a safety margin on
  every window. Edge admissibility (one-way `EGuidelineDir`, aircraft size, runway rules) is
  `RouteSearch`'s, reused - never a second copy.
- **`FPassingOrder`** - from the table, per resource the sequence of aircraft due through it. At
  run time an aircraft may enter a resource only when everyone ahead of it there has LEFT.
  Plugged into `FClaimPass` as one more refusal ("not my turn: waiting for X"); braking-distance
  spacing is unchanged.
- **`UTaxiPlanning`** - the owner, a `UGroundTraffic` subobject:
  - arrival clearance: the arrival queue's `ClearanceFor` requires a taxi-in plan (runway exit to
    stand); none -> the flight keeps holding (holding in the air, not a go-around);
  - departure clearance: pushback is gated on a plan (stand to runway); pushback ground is the
    plan's first window;
  - arrival priority (ruling 2);
  - release: a resource frees when the aircraft's tail clears it.
- Runway crossings are resources in the plan; the existing runway bar chain stays the authority
  for the runway itself, and the plan's order only adds to it.

## Section 2 - Delays, edits, failures

- **Delay**: only ORDER is enforced, never times, so lateness propagates as waiting, never as
  deadlock. Conditions this rests on: every plan ends where the aircraft can stay (stand, or the
  runway for take-off); all orders come from ONE timeline (acyclic); an aircraft waits only where
  its plan says. When an aircraft lags its plan by more than ~15 s (knob), its remaining windows
  are RE-TIMED - same order, later times - so new plans are built around reality. Re-timing
  never changes order, so it is always safe.
- **Refused clearance**: arrival keeps holding, departure stays on stand. Retry is EVENT-DRIVEN
  (a release, revoke or re-time changes the table), not polling. The inspector says why.
- **Layout edit while traffic moves**: planning freezes; the existing re-resolve/truncate/strand
  pass runs; moving aircraft re-plan from where they are in their current order with nobody new
  admitted; then parked/holding aircraft. A moving aircraft that cannot get a plan drops to
  UNPLANNED (today's claims + resolver), flagged in the inspector; directional locks later
  become this fallback.
- **Cost**: planning runs on clearance requests, re-times and edits only - never per frame.
  Measured on `M_ScaleGatwick` at 80 mov/h against the spike's figures.

## Section 3 - What the player sees

- **Aircraft card**: "Clearance: Taxi to stand 14 via A, A3, B" (consecutive segments of one
  taxiway collapse; runway crossing "cross 09L at A2"). "Now:" one of taxiing on time / "Holding
  at A/B for G-ABCD, ~40 s" (that aircraft's planned clear time) / "Re-timed +25 s" /
  "Unplanned (layout edit)" in amber. The existing Locate jumps to the aircraft it waits for.
- **Map, selected aircraft only**: remaining planned route along the guidelines, a marker with
  countdown at each planned hold, a thin link to the aircraft it waits for. Through
  `IToolPreviewSink` as MEANINGS (PlannedRoute, PlannedHold, WaitingOn), coloured by both drivers.
- **Arrivals panel**: "Holding: awaiting taxi-in route" / "Holding: runway busy"; a departure on
  stand "Awaiting pushback route".
- **Alert**: an aircraft dropping to unplanned raises one alert ("G-ABCD lost its plan after a
  layout edit - taxiing unplanned"), cleared on a new plan or arrival. The deadlock alert stays
  and should become rare - if it fires, it is a planner bug.
- **Log** (category for planning; once per event, never per frame):
  `TaxiPlan: G-ABCD planned stand 14 via A,A3,B (eta 182 s)`, `... refused - <reason>`,
  `... re-timed +25 s`, `TaxiPlan: G-WXYZ revoked by arrival G-ABCD`,
  `... unplanned - <why>`.
- Not included: an all-plans overlay.

## Section 4 - Testing and delivery

World-free `Airside.Model.TaxiPlan.*`:

- reservations: insert / remove / free intervals, touching and overlapping windows;
- planner: empty-network earliest arrival equals the shortest-route time; waits at a plain node,
  never inside a junction; takes the other parallel rather than waiting when that is earlier;
  refuses when nothing is free; respects one-way and size via RouteSearch's rules;
- order: an aircraft is refused entry until everyone ahead has cleared; the claim pass reports
  "not my turn: waiting for X";
- priority: an arrival revokes an unstarted departure, never a moving one;
- delay: lag one aircraft 60 s - order holds, others wait, no deadlock, re-time fires;
- edit: remove a segment mid-traffic - moving aircraft re-plan or fall to unplanned; alert
  raised and cleared.

**Headline test - no permanent deadlock**: the spike harness made permanent - the
`M_ScaleGatwick` layout, 2 h game time at 40 and 80 mov/h plus seeded random delays; asserts
ZERO permanent deadlocks and every admitted aircraft parks or departs; logs the spike's table
(waits, taxi times, ms per tick). Mutation proof: with order enforcement disabled it must go red.

Seam tests (actor level): spawn and tick the actor - inspector facts carry the clearance line;
the overlay receives PlannedRoute when an aircraft is selected.

Lint: only `UTaxiPlanning` writes `FTaxiReservations`.

Delivery, four stacked PRs:

1. Reservations + planner (model only, nothing wired).
2. Order enforcement in the claim pass; arrival and departure clearance through the arrival
   queue and pushback. THE behaviour change; carries the no-deadlock test.
3. Re-time, revoke, layout edit, unplanned fallback + alert.
4. UI: inspector lines, map route and holds, arrivals reasons.

**PIE check**: `M_ScaleGatwick` at peak for game-time hours at max speed - no deadlock alert;
select an aircraft and see its route, holds and what it waits for.

## Out of scope

Vehicles in the plan, towed aircraft, an all-plans overlay, score-based (non-priority)
re-optimisation, directional locks (next spec).
