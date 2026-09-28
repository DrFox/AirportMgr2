# Service vehicle lifecycle, role policies and the job board

2026-09-28. Ruled in chat with the user the same evening; they then asked for all stages to
be built unattended. Supersedes the "SCAFFOLDING" `UFuelService` (its own header, §0.1 of
`2026-09-07-fuel-service-slice-design.md`) and is the first cut of the systems map's
`UJobBoard` (`2026-09-05-game-systems-map-design.md` §3.5).

## 0. Why

A fuel truck's state lived in four places: `FFuelDemand` (TruckEnRoute / Fuelling / Done),
`UFuelService::GoingHome`, `UFuelService::Refilling`, and Airside's `EAgentPhase`. A depot
was an `int32 Trucks`; a vehicle existed only while it was an agent on the road, so nothing
could own its state. The vehicle a stand received was chosen by the stand's letter
(`VehiclesByLetter`), not by what the depot had. None of that survives a second vehicle type
competing for the same job, nor a truck going stand to stand without returning.

## 1. Rulings (user, 2026-09-28)

1. **The vehicle owns its state**: lifecycle, cargo, and a queue of jobs. It exists while idle.
2. **One generic lifecycle for every service role.** What varies per role is behind a
   Strategy, `IServiceRolePolicy`. Cargo is one of three kinds: none (stairs, GPU, tug), a
   depleting stock (fuel, catering), or a transfer (baggage). All three are "carries something,
   and sometimes needs a facility for it".
3. **Vehicle types are data rows** under a role. The utility tow, the bowser and the
   articulated tanker share the fuel policy and differ in figures.
4. **A fuel vehicle with enough left goes straight to its next job.** Only when it cannot
   cover the next job does it go via a facility (the depot) first. Baggage would always go via
   the hall; stairs never need a facility.
5. **A job tracks the quantity owed.** One trip is one commitment; when a trip ends with some
   still owed, the REMAINDER goes back to the board and is re-bid. That is the whole of the
   split mechanism. No simultaneous service: a stand has one fuel point.
6. **The bid is a finish time**: when would this job be finished if appended to this vehicle's
   queue - time to clear the queue (drives, serves, facility visits, with cargo tracked through
   it), plus the drive to the job from where the queue leaves the vehicle, plus serving the whole
   remaining quantity alone, including its own facility visits. Lowest wins. Append only - an
   insertion would move finish times already promised.
7. **Commitment is final once the vehicle sets off toward the job.** A queued job not yet set
   off toward is re-bid when things change, and moves only if the new bid beats its promised
   finish by a margin (`RebidMarginSeconds`, 120 game s).
8. **A flight owns its requirements; one board owns the jobs**, keyed by flight id. "This
   flight's jobs" is an index inside the board, not a second board.
9. **The stand's design vehicle is a bid eligibility filter** (today's `VehicleTooLarge`).
10. **The fleet will be bought by the player in play.** Not in the game yet, so the fleet is a
    runtime list that purchase will add to; a depot's placement-time `Trucks` seeds a
    placeholder default (§3.4).

## 2. Units

All in AirportOps `Model/` (world-free, `NewObject`-testable, may not include `Entities/` or
`Content/`). Airside keeps knowing only how things MOVE.

| Unit | Owns | Does not own |
|---|---|---|
| `FServiceVehicle` | id, `TypeCode`, role, home depot, `EServiceVehicleState`, cargo, queue of job ids, the Airside agent id while on the road, the step's timing | motion (`EAgentPhase`) |
| `FServiceJob` | id, flight id, aircraft agent id, role, stand, quantity owed/delivered, trips, `EServiceJobState`, refusal, deadline, promised finish, prerequisites | which vehicle - that is a queue entry on the vehicle, plus `AssignedVehicle` as a back-reference |
| `IServiceRolePolicy` | role rules: eligibility, `NextStep`, serve duration and effect, facility visit duration and effect, the bid's cargo arithmetic | any state |
| `FServiceVehicleType` | the data row: `FVehicle` chassis, role, capacity, rate | behaviour |
| `UJobBoard` | vehicles, jobs, the per-flight index, bidding, re-bidding, the tick, the phase-event entry point, departures | role rules |

### 2.1 Vehicle state

```
enum class EServiceVehicleState { Idle, ToJob, Serving, ToFacility, AtFacility, ToHome }
```

One enum, not bools (Conventions: "a phase is an enum"). `Idle` has no agent: a vehicle at
home is not on the road (today's retire-at-depot behaviour kept). Every other state has one.

```
Idle --queue non-empty--> NextStep(head)
   Direct      -> ToJob       --arrive--> Serving    --done--> (after-serve)
   ViaFacility -> ToFacility  --arrive--> AtFacility --done--> NextStep(head)
   Refuse      -> head job back to board; NextStep(new head) / home
(after-serve): remainder? -> back to board.  Queue non-empty? -> NextStep(head).
               Empty -> policy.OnQueueEmpty: ToHome (fuel: arriving home runs AtFacility refill)
ToHome --arrive--> AtFacility (if policy refills at home) --> Idle
```

Arrival is Airside's `Parked` phase event, routed by agent id. `Serving`/`AtFacility` are
timed on `USimClock` (game time), `StepStartedAt`/`StepEndsAt` on the vehicle.

### 2.2 Job state

```
enum class EServiceJobState { Open, Queued, Underway, Serving, Done, Unserviceable }
```

`Queued`: on a vehicle's queue, not set off toward (re-biddable). `Underway`: the vehicle is
driving to it (committed). `Serving`: at the stand, pumping. A trip that leaves a remainder
returns the job to `Open`. A job whose aircraft leaves is removed (with its fee paid for what
was delivered, as `PartFuelledPaysForWhatItGot` requires). Refusal reasons generalise
`EFuelRefusal` into `EServiceRefusal` with the same members and order.

### 2.3 Role policy

```
class IServiceRolePolicy
{
  EServiceRole Role() const;
  bool CanServe(const FServiceVehicleType&, const FServiceJob&) const;        // role match etc.
  EServiceStep NextStep(const FServiceVehicle&, const FServiceJob&) const;    // Direct/ViaFacility
  double ServeSeconds(const FServiceVehicle&, const FServiceJob&) const;      // this trip
  double TripQuantity(const FServiceVehicle&, const FServiceJob&) const;      // this trip
  void   ApplyServe(FServiceVehicle&, double Quantity) const;                 // cargo effect
  double FacilitySeconds(const FServiceVehicle&, int32 Pumps) const;          // refill/unload
  void   ApplyFacility(FServiceVehicle&) const;
  bool   VisitsFacilityAtHome() const;
};
```

`FFuelRolePolicy`: cargo is litres in the tank. `NextStep` is Direct when the tank holds the
job's remaining quantity or the tank is full (a full tank cannot do better by refilling first),
else ViaFacility. Serve pumps `min(tank, owed)` at the type's flow. Facility refills to capacity
at `RefillLitresPerMinutePerPump x pumps`. A test-only `FTransferRolePolicy` (always via facility,
fixed serve) proves the interface is not fuel-shaped.

### 2.4 The bid

`UJobBoard::Bid(Vehicle, Job)` simulates the vehicle's queue in order from its present state,
carrying cargo and position forward: for each queued job, the policy's `NextStep` decides
whether a facility visit comes first; drives are `route length / vehicle cruise speed` (the
cached route plan, `FRoutePlanCache`); serves and facility visits are the policy's durations.
Then the new job is appended and served to completion ALONE (its own facility visits between
trips, drives there and back). The bid is that finish time. Eligibility, before any of it:
role matches, vehicle `VehicleFit::NoLargerThan` the stand's design vehicle, home depot joined
and pumped, a route exists. The ineligibility reasons feed the job's refusal in today's order
(NoDepot, NoRoad, StandUnjoined, NoPump, VehicleTooLarge, TooNarrow, NoRoute).

A drive estimate uses the chassis' cruise speed only; acceleration and corners make the real
drive longer, equally for every bidder, so it ranks correctly without being a promise to the
second.

**One clock in the bid: game seconds.** Serves and facility visits are already game time; a
drive is movement time, which runs at the speed multiplier while game time runs at the multiplier
times the day compression. So a drive's `Length / Taxi.SpeedCap` movement seconds is converted by
`USimClock::GameSecondsPerRealSecond(TimeOfDay())`, the ratio of the two at the multiplier's
common factor. The conversion is taken once per bid (the hour a bid spans barely moves it).

### 2.5 Re-bidding (stage 2)

Triggers: a vehicle finishes a step or goes idle, a vehicle is added or withdrawn
(`FleetRevision`), the graph changes (`GetGuidelineRevision`). On a trigger every `Queued` job
is re-bid against every eligible vehicle (its current queue without it); it moves only if the
best bid beats its promised finish by `RebidMarginSeconds`. `Underway`/`Serving` never move.
With no trigger nothing is re-bid (issue #190's busy-wait rule, kept). Fleets were under ten
vehicles and jobs under ten on 2026-09-28, so a full re-bid per trigger is a few hundred cached
route lookups.

## 3. Behaviour kept

Every behaviour the 31 fuel tests pin survives, most at their old names under the new class:
the tow's reverse off a stand judged from the live chain; the recall that turns on the road;
home even when too narrow, never into a fold; refusal texts; part-fuelled pays; paused pump
does not finish; the route-plan cache; the per-job refusal revision; the busy-wait skip.

### 3.1 Driving

`SendTruckHome` generalises to `DriveVehicleTo(Vehicle, Goal)`, used for every leg: depot to
job, job to job, job to depot, recall. A vehicle with no agent (Idle) is dispatched from its
depot's pose; one parked is redirected (the reverse seeded from its chain); one on the road
turns where it is. A leg that cannot be driven: to a job, the job goes back to the board; home,
the agent is retired where it stands and the vehicle becomes Idle at home (logged Warning), so
a depot never loses a vehicle.

### 3.2 Departure

`DepartTheReady` asks the board's per-flight index: every job of that aircraft Done or
Unserviceable, and the turnaround over. An aircraft that wants no fuel has no job and leaves at
its turnaround (a record of the turnaround deadline is kept per parked aircraft, not faked as a
Done job).

### 3.3 Offers

`CouldServe` asks the same eligibility the bid does, per admitting stand; busy vehicles count as
servable, as today.

### 3.4 Placeholder fleet

Until purchase exists: a live fuel depot with `Trucks = N` gets N vehicles of EACH distinct type
in the per-letter table (today: N utility tows and N bowsers), created the first tick the board
sees the depot, withdrawn when it goes. This keeps A/B stands (tow-only) and C-F stands served,
and is the behaviour change to note: a one-truck depot now has one of each, so an A and a C
aircraft are served at once, and a C stand may be sent the tow when the bowser is busy and the
tow would finish first.

## 4. Logging (`LogAirportOps`)

Every transition keeps or gains a line. New: `Bid: job %d (%s %.0f) -> vehicle %d %s finish
+%.1f game min (next: vehicle %d %s +%.1f)`; `Rebid: job %d vehicle %d -> %d, +%.1f -> +%.1f`;
`Vehicle %d %s: %s -> %s`; `Fleet: depot %d gains vehicle %d %s`.

## 5. Out of scope

Stuck recovery (no movement for a threshold); depot fuel stock running out; player purchase of
vehicles; roles other than fuel in production; simultaneous service of one stand.

## 6. Stages

1. Vehicles, jobs, board, fuel policy, queued bids; `UFuelService` becomes `UJobBoard`.
2. Re-bidding of queued jobs and trip remainders, with the margin.
3. Save/load of jobs and vehicles (jobs keyed by flight id; on load vehicles are Idle at home
   and unfinished jobs re-open with their remaining quantity); the inspector shows a vehicle's
   state and queue.

## 7. Tests

The user's worked examples are the tests: an idle tow loses a 4000 L job to a bowser busy for
five minutes; on a 600 L job a tow with one 500 L job queued wins and with two loses BECAUSE
its tank runs out (the test asserts the facility visit was priced in). Chaining without the
depot; via the depot when short; remainder re-bid to a bowser freed after a tow's trip; margin
(1 min better does not move, 3 min does); committed never moves; the transfer policy through the
same board; a composition test (runtime ticks a real actor, a truck view appears); all 31 fuel
tests ported. `ENFORCED BY:` names cited in comments keep resolving.
