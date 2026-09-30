# Ops batch 3 - design

2026-09-29. Agreed in chat section by section. Base: main `c48fd130` (bus #403/#406/#407 and alerts
#411/#413/#414 merged). Extends `2026-09-29-ops-event-bus-design.md` (this is its stages 4-5, plus
more) and `2026-09-29-ops-alerts-design.md`; the survey is `docs/2026-09-29-ops-bus-event-candidates.md`.
Everything those specs rule (tiers, passes, rule 31, derived alerts, load paths) holds here unless said.

## 0. Why, and the rulings

The survey's batches 2, 3 and 5 plus two open issues: the arrival queue still asks the runway live every
frame; turnaround shortfalls are logged but not scored; `OfferAccepted` is the one offer decision with no
event; several sites do per-frame work that waits on a discrete change; #404 and #405.

User rulings (2026-09-29):

- **RunwayFreed is DERIVED FROM STATE in Airside**, not published at each release site. The runway has
  three release paths in `GroundTraffic.cpp`/`TrafficClaims.cpp` (take-off, landing-roll handover to a
  crossing, crossing clear) plus every way an agent ends (Gone, Unstick, `ClearAgents`, a rebuild).
  Per-site publishing is the paired-event trap the alerts spec rejected: one missed path strands the queue.
- **Turnaround shortfall**: proportional, one knob `ShortfallPenalty = 0.06`; a fuelled turnaround
  scores 0 (the on-time bonus already rewards it).
- **Tuning moves into a real asset**: `DA_Scenario_Default`. Today `Content/Ops` does not exist, so
  `ResolveDefaultScenario` returns the CDO and every figure is a constructor default. A primary data asset
  is Unreal's equivalent of a config file; a DataTable or raw JSON was rejected for one struct (tables
  and mods are what those are for).
- **`OfferAccepted` has no toast**: the flight moving into the accepted list is the feedback.
- **An airport without a runway is not an airport**: it gets no offers. Generalised by the user into an
  **airport status**: open, closed by the player, or no runway. Closed stops all offers; accepted flights
  are cancelled; aircraft on the ground are serviced and depart until the airport is empty.
- **A player closure costs** `ClosureCancelPenalty = 0.05` per cancelled flight; `NoRunway` costs nothing
  (user accepted the loophole of deleting the runway instead).
- **#404**: arrivals-side restored flights go round again; ground-side ones retire as departed.

## 1. Stages (one PR each, stacked, visible first)

| PR | Content | Seen |
|---|---|---|
| A | scenario asset; `TurnaroundEnded` scored; `OfferAccepted` | inbox row `▼ left unfuelled (-0.06)`; tuning in the Details panel |
| B | airport status: open / closed / no runway | bar status; no offers on a runway-less new game |
| C | #405, #404 | bug fixes |
| D | derived `RunwayFreed` / `StandsFreed`; arrival queue as a pass; safety net | none - same behaviour |
| E | per-frame fixes; two missing UI tests | none - cost |

## 2. PR A - scenario asset, TurnaroundEnded, OfferAccepted

**Scenario asset.** `Tools/Python/build_scenario.py` (headless, editor closed) creates
`/Game/Ops/DA_Scenario_Default` from `UScenario`'s defaults and sets `UAirportOpsSettings::DefaultScenario`
in `Config/DefaultGame.ini` (the `PrimaryAssetTypesToScan` line for `/Game/Ops` already exists). Verified by
the asset on disk and by the log line changing from `using UScenario's built-in defaults` to the asset's
name. Constructor defaults stay: they are what the asset is created from and what a test's `NewObject` gets.

**`FTurnaroundEndedEvent { AircraftAgentId, Stand, EFuelOutcome Outcome, Delivered, Wanted }`**,
`EFuelOutcome { Fuelled, PartFuelled, Unfuelled }`.

- Published in `UJobBoard::DepartTheReady` only AFTER `DepartAgent` succeeds - a refusal retries, and
  publishing before it would score one turnaround many times.
- Reaction handler on `UAirlineRoster`: resolves the airline through `FlightBoard->FindByAgent`
  (JobBoard must not learn flights); a flight not found logs Verbose and skips (an event is a fact
  about the past).
- Score: `-ShortfallPenalty * (1 - Delivered / Wanted)`; `Wanted == 0` is Fuelled. Causes read
  `left part-fuelled` / `left unfuelled`. `ShortfallPenalty` joins `FAirlineSatisfactionTuning`,
  commented unjudged (2026-09-29).

**`FOfferAcceptedEvent { Flight, AirlineId, Stand }`**, published from `UFlightBoard::Accept`.

- Found: `Accept` is called on the board directly from the game module (`OfferViewModels.cpp:212`),
  so an accept dirties no pass today; `HeldStandLost` is re-derived only when something unrelated happens.
- Subscribers: the Alerts pass (Reaction, `MarkDirty`) now; the arrival-queue pass in PR D.
- No roster score: accepting is not something the airline experiences.

**Tests**: part-fuelled scores in proportion; unfuelled scores the full penalty; fuelled scores 0; a
refused departure publishes nothing and the later success publishes once; accept dirties Alerts
(mutation: drop the subscription, the HeldStandLost test goes red); the scenario resolves to the asset
(composition).

## 3. PR B - airport status

**`UAirport`** (`AirportOps/Model/Airport.h`), world-free, owned by `UOpsRuntime` beside the roster.

- Stores only player intent, `bClosedByPlayer`; `IOpsPersistent` blob `"Airport"`, missing = open.
- `EAirportStatus Status()` is DERIVED: `ClosedByPlayer` if closed by the player, else `NoRunway` if the
  network has no runway, else `Open`. Player intent wins - only the player reopens.
- Re-derived on `FNetworkChangedEvent` and on the command (a command marks its own change - it does not
  wait for an event). On a change: `FAirportStatusChangedEvent { Old, New }`.
- Draining is not a state: a readout, "aircraft on the ground" while not Open.

**Entering a closed status** - Sim handler on `AirportStatusChanged` calls
`UFlightBoard::CancelUnarrived(Now, EAirportStatus Reason)`:

- Accepted: disarm the arrival `Clock.At`, release the stand hold -> `Cancelled`.
- Inbound: out of the queue, stand released -> `Cancelled`.
- Offered: -> `Withdrawn`, a NEW `EFlightPhase` appended last (the enum's order is load-bearing), entered
  only here. No `OfferExpired`, so no Ignored penalty.
- Landing and after: untouched - committed to the runway or on the ground.
- Each cancellation publishes `FFlightCancelledEvent { Flight, AirlineId, ECancelReason }`,
  `ECancelReason { AirportClosed, NoRunway, Unstuck }`. Roster: `ClosureCancelPenalty` (0.05) for
  `AirportClosed`, 0 otherwise. `CancelByAgent` (Unstick) publishes it too, reason `Unstuck` - the
  event then has two publishers and a subscriber.

**Offers**: `UOfferGenerator::TickMinute` returns nothing unless `Open`, and skips the per-airline
`bCouldCome` evaluation - so a runway-less field raises no `AirlineCannotCome` alert and no toast per
airline. The generator READS the status (a value asked for when needed), it does not subscribe. The inbox
demand strip reads "Closed".

**Alert**: new `EAlertKind::NoRunway`, derived like the rest, "No runway - build one to receive offers",
no focus. `ClosedByPlayer` raises none: it is deliberate. Dirtied by `AirportStatusChanged`.

**UI**: bar action "Close airport" / "Open airport" (`BuildActions.cpp`, the Ledger action's shape) with a
status label: `Closed (draining: 3)`, `Closed`, `No runway`. Closing confirms AT THE CURSOR ("Close? 4
flights will be cancelled") - a destructive gesture; opening does not.

**Load**: status is re-derived after load; a load never re-runs the cancellation (nothing to cancel that
was not cancelled when saved).

**Tests**: close cancels Accepted and Inbound, keeps Landing, withdraws offers; penalty only for
`AirportClosed`; no offers while closed or with no runway; delete the last runway -> `NoRunway` and the
alert; build one -> `Open` and the alert clears; save round-trip keeps `ClosedByPlayer`; composition: a
closed airport drains to no aircraft; the confirm cancels nothing when dismissed.

## 4. PR C - #405 and #404

**#405**: a flight enters `Turnaround` only when its agent parks AT A STAND. Parked on the fallback
junction it stays `TaxiIn`, so the later redirect (Parked -> Taxiing) still reads `TaxiIn`. Test: park on
the fallback, redirect, assert `TaxiIn` throughout and no turnaround opened at the junction.

**#404**: agents are not saved (`LoadFromSlot` clears them), so every restored flight past Inbound points
at nothing. In `UFlightBoard::OnAfterRestore`:

| Saved phase | After load |
|---|---|
| Landing, TaxiIn | `Inbound`: stand re-held, rejoins the queue at `HoldingSince = now`; landing fee marked paid, not charged twice |
| Turnaround, Manoeuvring, TaxiOut, Departing | `Departed`, to history, unscored - the save system is not the player's fault |

`AgentId` cleared, no `ByAgent` entry; one log line per flight (`restored mid-<phase>: re-queued` /
`retired as departed`). Test: save with flights in TaxiIn and Turnaround, load, assert Inbound with stand
held and Departed. No player saves exist (2026-09-23); the break is noted in the PR.

## 5. PR D - derived runway and stand freedom; the queue as a pass

**Airside, one predicate.** At the end of `UGroundTraffic::Advance`, for each runway seed,
`IsChainHeld(Network, Seed, &Occupancy)` - EXACTLY what `ArrivalPlanner::IsRunwayBusy` asks, so "freed"
means "what the queue asks just turned false". The per-tick claim pass puts crossings into occupancy, so
the crossing clear needs no site of its own. Held -> free fires native `OnRunwayFreed(Seed)` and bumps a
session counter `RunwayFreedCount()`. Cost: one check per runway per Advance (2 runways, 2026-09-29).

**Stands, same shape**: diff the set of held stand pose nodes (holds plus parked agents); freed ones fire
`OnStandsFreed(TArray<FGuidelineNodeId>)`. The four `bStandsMayHaveFreed` sites stay as they are - they
gate `ReofferStands`, inside Airside; the bus signal does not depend on them.

A rebuild resets both baselines (seeds change) and reports as freed whatever vanished - a deleted runway
is no longer busy.

**Bridge**: `UOpsRuntime::Attach` binds both, publishing `FRunwayFreedEvent` / `FStandsFreedEvent`.

**Pass `"ArrivalQueue"`** replaces the per-frame `TickQueue` call. Dirtied by `RunwayFreed`,
`StandsFreed`, `OfferAccepted`, a flight going Inbound, `NetworkChanged`, `AirportStatusChanged`, and
`MarkAllDirty` on attach/load. ONE CLEARANCE A FRAME is kept: after a dispatch the pass does not re-dirty
itself; the new agent's Landing phase event, next frame, does - so the second runway gets its flight one
frame later and the claim exists first. The JobBoard pass is also dirtied by `RunwayFreed`, replacing
`DepartTheReady`'s per-frame refusal retry.

**Safety net**: while the queue or the departure-ready set is non-empty, `Clock.Every(30 game s)` runs
both in safety mode; a safety run that dispatches anything logs
`Warning: LogOpsBus: safety pass dispatched <what> - no event covered it`. Armed and cancelled by the
passes themselves. Removed once quiet in play.

**Tests**: Airside - take-off frees (event fires); a taxiing crossing clearing frees (the no-revision
case); a despawn on the runway frees; a deleted runway frees; no event while still held. Composition - a
queued arrival dispatches on the frame after a crossing clears with NO safety Warning (unbuffered log spy
fails the test). Mutation - remove the diff broadcast: the safety Warning fires and that test is red.
Quiet - the queue pass runs 0 times over N frames with an empty queue.

## 6. PR E - per-frame fixes

The survey's inbox-while-hidden item shipped in #414.

| Site | Fix | Test (a call counter, red first) |
|---|---|---|
| `UGroundTraffic::ReplanHeldTaxiOuts` | per agent, remember `(GuidelineRevision, RunwayFreedCount)` at its last failed attempt; skip until either moves. Occupancy matters: `PlanAny` is given it, so a guideline-only gate would strand a hold behind a busy runway | `PlanAny` 1 call over 100 substeps; move a line -> retried once, joins |
| Inspector stand / runway / taxiway cards | key on edit revision + occupancy revision + the freed counters | `DescribeStand` 1 call over N quiet frames |
| Inspector depot card | key on a JobBoard step counter + game minute | `DescribeDepot` |
| Aircraft card lookups | key on flight board revision + agent phase | lookup count |
| Land panel `LandChoices::Build` | key on guideline revision + `RunwayFreedCount` + focus | 1 build over N frames; a new runway rebuilds |
| `UFlightBoard::TickOffers` | early-out on `OfferedCount == 0` | no copy on an empty board |
| `UOpsRuntime::Tick` `SetSimTimeScale` | set in `Attach`, `ApplySpeed`, after `LoadFromSlot`, and anywhere else found to reset the actor's scale (checked first) | speed change, load, network clear: agents move at the multiplier each time |

Also: **Go on an agent alert** sets camera focus and selection; **`FocusOn` leaves watch mode**.

## 7. Process (rules proved by the previous batches)

- Subscribe only in `UOpsRuntime::WireBus` (rule 31). Every event an `AIRPORTOPS_API` struct with
  `EventName()` and `Describe()` and a real subscriber the same PR; no `UOpsEvents` delegate nobody binds.
- Standing conditions derived and re-queried when dirtied; commands mark their pass dirty; a load needs
  its path (`Reset` / `MarkAllDirty` / `AlertsReset`).
- TDD; watch each test fail; mutation-check every seam, restoring with cp + touch.
- A fresh opus reviewer per PR; Critical/Important fixed with a test that failed first; rulings ledgered.
- PRs fill the template and say "unverified in PIE" unless verified.

## 8. PIE checks

- A: force an unfuelled departure (no depot road): inbox row `▼ left unfuelled (-0.06)`.
- B: new game with no runway - no offers, one `No runway` alert; build one, offers start. Close with a
  flight accepted: it is cancelled, ground aircraft leave, bar reads `Closed`.
- D: a taxiway crossing a runway with an arrival queued: the arrival lands after the crossing clears, and
  `grep "safety pass" AirportMgr.log` is empty.
