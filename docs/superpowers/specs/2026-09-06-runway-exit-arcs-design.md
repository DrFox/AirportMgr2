# Runway exit arcs — design

Date: 2026-09-06. Status: approved in conversation, binding for the plan.
Branch: `feature/runway-exit-arcs`. Follows M2 (PR #54).

## 1. The defect

Where a taxiway meets a runway the guideline builder emits a STRAIGHT turn path from the
taxiway's cut point to the road node, meeting the runway centreline at the taxiway's angle
with an instantaneous heading change. `samples/runwayexits.png` shows it: the taxiway-to-
taxiway turn at the same node is a proper arc, the two taxiway-to-runway turns are straight
lines into the node.

Cause, in one line: a continuous arm (`URoadProfile::bContinuousThroughJunctions`, the
runway) takes `CutDistance` 0 in the junction solver, so its derived guideline ENDS ON THE
NODE, and a quadratic whose control point (the node) coincides with an endpoint is a straight
line. The taxiway arm is cut back normally, which is why its own turns are arcs.

Consequences the player sees:

- The speed profile treats a zero-length heading change as uncrossable and caps it at creep.
- The Vacated handover (`FRoadAgent::Advance`) starts the follower AT the node with
  `Speed = 0` and a heading seeded from that straight stub, so the aircraft appears to stop
  dead, turn on the spot and pull away - "respawns facing the exit".
- A departure arrives at the node across the runway axis and lines up on the spot.

## 2. The rule

**A taxiway meeting a runway ends where its exit arc begins, and the arc joins the runway
centreline `ExitLength` before the node.**

- `URoadProfile::ExitLength` (uu, `EditAnywhere`, default `6000.0` = 60 m). Read from the
  CONTINUOUS arm's profile - the runway decides its exits, per profile, so `DA_Runway_18m`
  and a 45 m strip can differ. Meaningless on a profile that is not continuous.
- A length, not a radius, deliberately. Equal tangent lengths L either side of an angle θ
  make a near-circular arc of radius `L / tan(θ/2)`: at L = 60 m a 30° exit gets 224 m,
  45° gets 145 m, 90° gets 60 m. That is how real exits are graded (rapid exits shallow and
  wide, right-angle exits tight) from ONE number the player can read off the ground. A fixed
  radius would give a 90° exit a 150 m sweep that eats the taxiway.
- Per-aircraft figures are NOT consulted. Taxi lines are painted infrastructure sized for
  the largest aircraft admitted, never for the one taxiing (memory:
  airport-geometry-is-infrastructure-not-per-aircraft).

## 3. Geometry (guideline builder only)

At a solved node N with at least one continuous arm AND at least one non-continuous arm:

**3.1 Runway side.** For each continuous arm half at N, its derived centreline edge (which
ends on N) is split at distance `L_r` from N along the edge, producing a node `S` on the
centreline. The piece `S→N` stays, so the runway's through-route (the two halves' zero-length
turn at N) is unchanged. `S` becomes that arm's TURN ATTACHMENT at N: every turn path that
arrives from or leaves along that arm attaches at `S`, not at the node-end. Both halves get
an `S`: the upstream one is the exit, the downstream one the entry, and either serves a
backtrack in the other direction.

**3.2 Taxiway side.** For each non-continuous arm at N, the derived guideline's END at N is
placed `L_t` back from N along the arm's outgoing tangent
(`URoadNetwork::GetOutgoingTangent`) instead of at the cut-line point. The end node keeps its
`Origin` (segment, end, guideline index), so an `FHoldShortMark` keyed by `FGuidelineEndRef`
still resolves to it and the bar sits at the arc start. Nothing is added to the graph
between the arc start and the pavement edge.

**3.3 Turn paths.** Unchanged construction: quadratic from the From arm's attachment to the
To arm's attachment with `Control = N`. With both attachments set back the curve is tangent
to the runway at `S` and to the taxiway at its end. Taxiway↔taxiway turns at the same node
use the set-back taxiway ends too (a crossing taxiway's two arms are collinear, so their
turn is straight, as now).

**3.4 Clamps.** `L_t = max(taxiway CutDistance, min(ExitLength, 0.45 × taxiway segment
length))`; `L_r = min(ExitLength, 0.45 × runway-half segment length)`. *Amended 2026-09-07:*
the length is NODE TO NODE and the cut is a floor the clamp cannot undercut - measured between
cut points, a 55 m exit stub at an acute corner had no chord left and its end landed inside the
runway slab (`samples/holdlines.png`). The lower bound
on the taxiway keeps the end at or beyond the pavement cut it has today; the upper bounds
keep two exits on one short runway half, or a short stub taxiway, from crossing their own
far end. Each side clamps independently - the arc stays tangent at both ends whatever the
lengths.

**3.5 Not touched.** The junction solver, the mesh builder, `bContinuous` and the bitwise
weld are untouched: this is the guideline graph, which shares by handle. Runway↔runway nodes
(no taxiway) and taxiway↔taxiway nodes are unchanged.

**3.6 Identity.** Both split pieces of a runway half carry the same `DerivedFrom` /
`DerivedGuidelineIndex` (the precedent is `FAnchorLink::Build`, which splits a taxiway for a
stand lead-in). `FindSparedEdge` already tolerates that. `S` is a derived node with no
`Origin`.

## 4. What falls out without code

- `ArrivalPlanner::Plan` lists strip nodes sorted from the threshold and takes the first
  that reaches a stand. The upstream `S` is now that node, `VacateAt` becomes the arc start,
  and `TaxiIn` begins with the arc.
- `RunwayExitNodes` / `IsGuidelineNodeOnRunway` key on position within the strip's half
  width, so `S` counts as on the runway for occupancy, crossings and the deadlock resolver.
- Departures route to the downstream `S` and arrive on the centreline aligned;
  `FTakeoffRun`'s line-up slew clamps to its remaining error, which is now ~0.

## 5. Handover

- `FRouteFollower::Start(const FRoutePlan&, const FGroundPerformance&, double InitialSpeed
  = 0.0)`. Default keeps every dispatch caller and test at rest. `Speed` is clamped to the
  profile's limit at the first vertex so a handover can never begin above what the route
  permits.
- `FRoadAgent::Advance`, Vacated branch: `Follower.Start(TaxiInPlan, Airframe.Ground,
  Arrival.Speed)`. The rollout still brakes to the taxi cap before `VacateAt` ("roll out to
  taxi speed, then turn" - the player's words); holding the arc's permitted speed through
  the rollout is a follow-up, not this work.
- The one-frame `LastMotion` hand-back on the handover frame stays; with a continuous
  heading and speed it is now merely conservative.

## 6. Tests (all on the real solver + builder; measured, not named)

1. **`Airside.Build.RunwayExitArc`** — runway with one 45° taxiway. Asserts: the runway half
   is split at `L` (a strip node exactly `ExitLength` before the road node); the taxiway's
   end is `L` back from the node; the exit turn's sampled polyline has no vertex heading
   change above the turn a 10°/s aircraft can make at taxi speed over the sample spacing
   (measured on `GuidelineGeom::Sample`, the ONE evaluator); its first tangent equals the
   runway direction and its last the taxiway direction within 1°; an entry arc exists on
   the downstream side; the runway's through-route still exists; the 90° case is tangent
   too; a taxiway shorter than `2 × ExitLength` clamps to 45% and stays tangent; a
   runway↔runway node (no taxiway) is unchanged.
2. **`Airside.Build.RunwayExitArcHoldShort`** — a hold-short mark set on the taxiway's
   runway end survives the rebuild and lands on the set-back node.
3. **`Airside.Model.ArrivalExitAtArcStart`** — `Plan` picks the upstream `S`; `VacateAt`
   equals its distance from the threshold; `TaxiIn.Polyline[0]` is on the centreline.
4. **`Airside.Model.Traffic.VacatedHandoverIsContinuous`** — dispatch an arrival on the
   fixture, tick to Parked, record speed and heading every tick; assert the largest
   speed step ≤ `max(Landing.Decel, Taxi.Accel) × dt + ε` and the largest heading step ≤
   `MaxTurnRateDegPerSec × dt + ε` across the WHOLE run, including the handover frame.
   Red on main: the handover frame steps speed by the taxi floor and heading by 45°.
5. Existing runway tests (`RunwayExtent`, `ArrivalPlanner`, `HeadOnReplansRoundBarHolder`,
   `DepartureReleasesWhenAirborne`, hold-short tests) are expected to move their numbers,
   not their meaning; each change is justified in the PR.

## 7. Out of scope

Spiral (clothoid) exits; a rollout that leaves the runway above taxi speed; any mesh or
solver change; exit markings on the surface; a stand tool warning for unjoined stands (a
separate follow-up already filed in the M2 handover).

## 8. Risks

- **Runway halves shorter than `2 × ExitLength`** (many close exits): the 45% clamp keeps
  the arcs but shortens them; a very short half gets a tighter arc. Accepted.
- **A taxiway crossing the runway** gets four arcs (two per side). Its straight-through turn
  is between two set-back ends and stays straight. Accepted; the box-junction rule keys on
  runway-derived edges by segment, and the arcs are turn paths (no `DerivedFrom`), exactly
  as today's stubs.
- **Bar-to-bar arming window** (M2 follow-up): the arc is longer than the stub it replaces,
  so an aircraft on the arc is committed to the runway for longer before its nose is on the
  asphalt. The crossing rule is body-keyed; unchanged here, noted for that follow-up.

## 9. Outcome (2026-09-06, same day)

Implemented on `feature/runway-exit-arcs` in one commit after the spec and plan. Measured:

- 45 degree exit: leaves the runway 0.32 degrees off tangent, meets the taxiway 0.32 off
  (64-sample chords), worst chord turn 3.2 degrees at 16 samples. 90 degree exit at the
  runway end: 0.46 / 0.46, worst 7.6. The entry in the roll direction from a 45 degree exit
  is a 135 degree hairpin (worst chord 17.9) - tangent both ends, crawled by the profile.
- The taxiway's end sits at `max(ExitLength, CutDistance)`: at an acute 45 degree corner the
  cut is 6044 uu, so the lower bound in §3.4 binds there.
- Handover on the real fixture: worst speed step 5 uu/s per tick (the rollout's braking),
  worst heading step 0.33 degrees (the follower's slew), parked in 82 s. The first cut
  showed an 888 uu/s step: the rollout stops the tick AFTER crossing `VacateAt`, and the
  follower discarded that overshoot. `Start` now takes `InitialTravelled` as well.
- Movers: `EarliestExitWins` (exit is the arc start, on the centreline, within
  `ExitLength` of the junction); `HeadOnReplansRoundBarHolder` keeps `ExitLength = 0` - with
  arcs a bar 60 m down the taxiway leaves the tail clear of the strip and the deadlock
  never forms, so the replay pins the resolver on the geometry it was recorded on.
- 126 tests, 0 failed, 0 crashed; `UE_LOG` 98 -> 98. Built in a detached worktree because
  the editor was open; unverified in PIE.

## 10. PIE round 1 (2026-09-06): "it rolled straight past the exit"

Log: `vacating at exit 1 of 8` at 41402 - the junction node itself - then `taxiing 30275`
where it had been 20658. The exit's arc start (35400) lay between the raw slowed-by distance
(29632) and the margined `Needed` (37039), so `RunwayExitNodes` ruled it unusable; the
junction's node-end qualified, and its only route off was along the strip to the downstream
split and back through the hairpin. Two planner rules, both in `ArrivalPlanner::Plan`:

- Exit usability is judged at `Needed / LandingMargin`. The margin is a refusal margin on the
  strip, not a statement about which turn-off is takeable.
- The taxi-in is searched with `bAvoidRunways` (the replan's rule): a node-end then has no
  route at all, and the earliest arc wins on its taxiways rather than losing to a later exit
  whose shortest route ran down the strip. A forward turn-off beats a backtrack at any
  distance.

§4's "falls out without code" was wrong by exactly these two rules. Test:
`Airside.Model.ArrivalTakesTheArcNotTheJunction`. 127 tests, 0 failed, 0 crashed, built on
the main checkout.

Verified in PIE by the player after this round, on the M_Starter level: "that works" (2026-09-06).

## 11. PIE round 2 (2026-09-07): symmetric arcs and intersection departures

`samples/runway1.png` showed two things. An exit arc with 60 m of tangent on the runway side
and ~30 m on a stub taxiway swung wide of the fillet; and a departure routed to the threshold
node drove onto the runway by the entry arc, east to the split, hairpinned and came back to
spin 180 degrees at the threshold.

- **§3.4 amended again: ONE length per node, symmetric.** `L = min(ExitLength, 0.45 × every
  arm's node-to-node length)`, the same on the runway side and the taxiway side; a taxiway's
  pavement cut remains a floor on its own side. `Airside.Build.RunwayExitArcOnPavement`
  samples every arc at 45/60/90 degree stubs against the triangles the surface builder paves:
  160 of 160 points on the pavement.
- **§4 corrected: departures did NOT fall out.** `DeparturePlanner::Plan` (Model/) mirrors the
  arrival planner: the first strip node from the threshold that leaves at least the roll
  needed, reachable with runway edges excluded, arrived at heading down the runway - an
  intersection departure; else the threshold with runway edges allowed - a backtrack, where
  the 180 at the end is right. `FDepartureOrder::EntryOffset`; `FTakeoffRun::Start` takes the
  entry offset and the arrival speed, judges the roll on the runway REMAINING and starts
  `Travelled` at the entry so nothing jumps. `ArmDepartureIfRunway` measures the offset from
  the route's end, admits any point ON the strip (`RunwayExtentAt` was "within a width of a
  runway node", which a split node 60 m out never is), and rolls the way the taxi arrived for
  a mid-strip entry. The Route tool sends a runway goal through the planner.
- Tests: `DeparturePlanner.Intersection`, `DeparturePlanner.Backtrack`,
  `Traffic.DepartureFromIntersectionIsContinuous` (handover frame step 0.6 uu, airborne).
  132 tests, 0 failed. `UE_LOG` 99 -> 100. Unverified in PIE at the time of writing.
