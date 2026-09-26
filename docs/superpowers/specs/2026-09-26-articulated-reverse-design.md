# Articulated reversing (step 2) - design

2026-09-26. Follows `2026-09-24-articulated-rig-forward-design.md` (step 1, merged #285).

## Goal

Articulated vehicles - the rig (truckCab1 + tankTrailer1) and the utility tow (utility1 +
towbar + fuelTrailer1) - can reverse, straight AND on a curve, and a reversing yard on
M_RigTest demonstrates it: a straight bay, a 90 degree bay and a hammerhead turn.

## Rulings this records (user, 2026-09-26)

- **Curved trailer reversing is wanted.** Reverses the 2026-09-24 "curved trailer reversing
  not wanted for now". Option B (pull past, reverse straight in) is still how stands will do it;
  it is now one case of the general reverse.
- **The utility tow reverses with its turntable LOCKED.** Forward, both joints swing (utility
  hitch, towbar_yaw). Reversing, only the first joint does: towbar and body are one rigid link.
  A real turntable dolly has such a lock. One joint, not the unbackable two.
- **Dead-end turning in the yard is a HAMMERHEAD**, not a three-point turn in the balloon.
  Amends the 2026-09-25 ruling "the rig U-turns at road ends with a three-point turn" for
  service dead ends that carry a hammerhead stub. Balloons are unchanged and still refuse the
  rig (`Airside.Build.DesignVehicle.WideDeadEndRefusesRigUntilReversing` stays green - its
  name is now wrong and gets renamed to `WideDeadEndRefusesRig`).
- **The reverse is SOLVED ONCE AND PLAYED BACK** (approach A). Rejected: a hybrid-A* search
  planner (a subsystem nothing needs yet) and live tracking at runtime (FReverseRun's header
  already rejects it: reversing is unstable, and gains tuned live are the hardest thing to get
  right).
- **Handover starts from the MEASURED hitch angle**, not a guaranteed straight run-in (the
  question left open on 2026-09-23).
- **Reverse spans draw in their own colour** in the selected-agent route view and the guideline
  overlay.

## 1. `FTowReverseRun` - Model/, world-free

Beside `FReverseRun`, which stays untouched for rigid vehicles.

- **The line's meaning.** A `bReverseLeg` polyline is the path of the axle that LEADS while
  reversing: the fixed axle of a rigid vehicle (as today), the rearmost trailer axle of a chain.
- **The reverse body.** `ReverseBody(const FBody&)`: link 0 keeps its joint; every later link is
  merged rigidly into its puller (utility: HitchX -90.7, Length 114 + 221 = 335, body reaches
  from the body link). Rig: unchanged, one link. Arming refuses if a locked joint is more than
  `TurntableLockDegrees = 3` off straight; the lock snaps it to zero.
- **Solve** (`Start`), from the agent's measured cab pose and `TowAxles`:
  - Step `VehicleSweep::TraceStep` (10 uu) of the tractor's fixed axle, backwards.
  - Controller, cascaded: pure pursuit of the trailer axle onto the line (look-ahead a
    multiple of the trailer length) gives a desired trailer curvature; its steady-state hitch
    angle is the reference; tractor steer = steady-state steer for that reference + gain x
    hitch-angle error, clamped to the chassis lock.
  - Chain geometry per step: the same hitch/axle relations as `StepChain`, applied with the
    tractor moving backwards (the trailer axle moves along its own heading; the hitch is where
    the tractor puts it).
- **Refusals**, each logged with the figure and the place (`LogAirside`, like FReverseRun's
  "Reverse manoeuvre refused: ..."):
  - hitch angle past the CRITICAL angle - derived: the steady-state hitch angle at full lock for
    this body, beyond which no steering recovers it. Not typed.
  - trailer axle more than `MaxLineError = 30` uu off the line;
  - final heading error above 3 degrees, or final position error above 20 uu;
  - the line invalid or too short.
- **Samples** stored: cab fixed axle, cab heading, chain axles (full chain, towbar included, so
  the view poses the real links), steer degrees, leading-axle distance.
- **`Advance`** interpolates samples by leading-axle distance at `ServiceReverseSpeed`, honours
  `StopWithin`, and keeps the #297/#330 contract: false means the manoeuvre is over and nothing
  moved this call.
- `FRoadAgent` chooses the run by `Vehicle.HasTrailer()` - a fact about the vehicle, not a
  state flag. One `EAgentPhase::Reversing` for both, so no consumer of that phase changes.

Tests `Airside.Model.TowReverse.*`: StraightIsExact, ArcTrackedWithinTolerance (both chains),
TooTightIsRefused (past critical angle), BentTurntableIsRefused, NeverJackknives (worst hitch
angle below critical over the playback), StopWithinHolds.

## 2. `FRoadAgent` and `VehicleFit`

- **Arm** (`TryArmReverseLeg`), trailer vehicles: `FTowReverseRun::Start(Span, Vehicle, cab
  pose, TowAxles, ReverseSpeed)`. The start offset is the projection of the MEASURED trailer axle
  onto the line (the rigid code's `Travelled = Wheelbase` is the same idea for a rigid body).
  Layout contract, as for rigid bays today: a reverse leg's first stretch retraces the approach.
  Refusal keeps today's behaviour: warning, stall, retry.
- **During**: `TowAxles` written from the playback every frame (ends the frozen chain).
  `DescribeMotion` then poses the trailer unchanged in code.
- **Handover**: the forward remainder starts with its steered axle at the playback's last
  pose, measured along the exit line; the chain is kept, so `StepChain`'s fold guard sees no
  jump. The lock releases. The solver checks at ARM time that the final steered axle lies on the
  exit line within 20 uu, so a badly laid exit is a refusal, never a snap.
- **`VehicleFit::JudgePlan`** stops cutting at the first reverse leg: it runs the same solver
  from the chain state the forward judge reached, traces the samples' `BodyCorners` against
  tarmac, then judges the forward remainder. New `EFitRefusal::ReverseUnsolvable` carries the
  solver's reason. "The router's verdict is the agent's" stays true across reverses.

Tests: `Airside.Model.RoadAgent.TowReverseMovesTheChain`, `TowReverseHandoverHasNoJump`;
`Airside.Model.Tow.WholeRouteJudgesTheReverse` (refuses unsolvable, admits solvable, agrees
with the agent).

## 3. `FReverseTurn` - the builder derives reverse legs

- Persistent record on `URoadNetwork`: `FReverseTurn { Node, FromArm, IntoArm }` - "a vehicle
  may back from arm A through junction N into arm B". Added by
  `IRoadEditTarget::AddReverseTurn`, like `AddApron`.
- The guideline builder, in the turn-path pass, derives:
  - one `bReverseLeg` edge: starts on arm A's outbound lane a PULL-PAST distance beyond N
    (the tier design vehicle's straight chain length - steered axle to rearmost axle - plus a
    margin; derived, not typed), retraces that lane to N, then curves (or runs straight, for an
    opposite pair) onto arm B's centreline to arm B's far end;
  - one forward exit edge from there back along arm B, onto its outbound lane by the existing
    `GuidelineGeom::LaneChange` S-taper.
- Stubs are ordinary service-road segments: normal mesh and lanes, no new paving. Their
  bowser-sized balloon is left as it is.
- One record covers the straight bay (opposite arms), the 90 degree bay and the hammerhead.
  Seed for hammerheads at real service dead ends - noted, not built.
- A record whose node or arms die is dropped on rebuild, logged.
- Rejected: the course re-lays hand-drawn edges after each rebuild - a fixture hack that
  re-invites step 1's rebuild-order bugs and that nothing else could use.

Tests `Airside.Build.ReverseTurn.*`: EdgeIsFlaggedAndStartsAtTheStop, PullPastHoldsTheChain,
SurvivesRebuild, DeadArmDropsTheRecord.

## 4. Reversing yard on M_RigTest

- South of the return road, Y -6000 to -14000; the level's floor already reaches -17000, so no
  content change. Laid by `ARigTestCourse::Lay`, Wide tier (rig is its design vehicle).
- A feeder south off the return road to a T on an east-west yard road:
  - **straight bay** - a stub continuing the yard road west past the T. Turn east out of the
    feeder, pull past, reverse straight west through the T into the stub;
  - **90 degree bay** - a stub off the yard road's south side, midway;
  - **hammerhead** - the yard road's east end is a dead end with a short stub off its north
    side: pull past, reverse into the stub, drive out west.
  Exact figures go in the plan, drawn to scale.
- Two NEW yard runners (rig, utility) loop feeder -> straight bay -> 90 degree bay ->
  hammerhead -> feeder, staggered. The loop runners and their tests are untouched; the "course
  has no reverse legs" warning stays scoped to loop runners, and on a yard leg a missing reverse
  is the failure.
- Log (`LogRoadBuild`, `RigYard:`) per reverse: vehicle, feature, armed/refused + reason, worst
  hitch angle, final pose error. Refusals marked with the existing `DrawRefusals`.

Tests: `AirportMgr.RigCourse.YardHeadless` (one yard loop per vehicle at a fixed step: every
reverse armed, no jackknife, final bay pose within 20 uu and 3 degrees, trailer inside tarmac
clearance at every sample, chain continuous across every handover), `YardSurvivesRebuild`.

## 5. Reverse spans in their own colour

- New `EPreviewStyle::ReverseRoute` (selected agent's route) and `EPreviewStyle::ReverseGuideline`
  (the overlay's `bReverseLeg` edges). Meanings in `Tool/`; colours only in `PreviewPalette.cpp`:
  amber-orange at Route's weight, a dimmer amber at Guideline's. First guesses, the user's to
  judge.
- `UAirsideTraffic` gains `RemainingRouteSpans(Id)`: the remaining route cut into spans tagged
  forward/reverse from the follower's reverse-leg runs. During a reverse, the reverse span is
  the solved trailer-axle path. `SelectTool` emits each span in its style; `GuidelineOverlay`
  styles `bReverseLeg` edges.
- `RoadBuildHUD.cpp`'s style list gains both; a test checks by name that every `EPreviewStyle`
  has a palette colour and a HUD entry.

Tests: `Airside.Tool.RouteSpansSplitAtReverse` (three spans, forward/reverse/forward, meeting
end to end), `Airside.Present.EveryPreviewStyleIsDrawn`.

## Out of scope

Hammerheads at real dead ends; stands and `FuelService` adopting tow reverse (step 3);
three-point turns in balloons; an automatic pull-forward to straighten a bent turntable; the
tug + 1000 l trailer (step 4).

## Verification

Headless suite line (`N test(s) run, 0 failed, 0 crashed`) and, in PIE on M_RigTest: both
vehicles back into all three features without the trailer freezing or snapping; reverse spans
show amber on a selected vehicle; `RigYard:` lines in `Saved/Logs/AirportMgr.log` give each
reverse's error figures.

## Unresolved

- Controller gains and look-ahead are found in the ArcTracked test, then fixed with the date.
- Whether the utility's unmeasured 45 degree lock makes its 90 degree bay tighter than it
  really is (step 1 already flags the lock as unmeasured).

## Implementation notes (2026-09-26, found while building)

- **Mid-route reverse legs never armed.** `NextReverseLegRun` arms only while the steered axle is
  AT the leg's start, and a follower crawling through the cusp stepped past it. Every reverse in
  play before began its own route at 0. `FRoadAgent::FollowAndTow` now caps its allowance at the
  next reverse leg's start (same input as arbitration).
- **Start line error.** A tow that pulled past a corner stops with its trailer still settling (the
  rig 30 uu off after a 50 m pull-past). `TowReverse` accepts up to `MaxStartLineError` (100 uu) at
  the start and then `MaxLineError` (30) beyond where it started - reversing is non-minimum-phase.
- **End pose: cab straight too.** The hitch reference eases to zero over the last look-ahead, so the
  tow comes to rest in line (the rig held 2 deg of hitch to take out 3 uu otherwise).
- **Bays must hold the whole vehicle.** The builder refuses a bay whose run from its junction lane end
  to its end is under the design vehicle's chain + 200 uu (Wide corners take ~19 m of each arm); a
  25 m bay left the rig's cab on the junction's turn path.
- **Pull-past failure keeps the record** (not laid, logged) - an edit may lengthen the arm. Only a
  dead node/arm drops it.
- **`RedirectAgent` seats a tow's cab from its SHOWN pose** (`LastMotion`), projecting its steered axle
  onto the new plan when it is not at the start - after a reverse the follower is stale.
- **No tarmac check on the reverse itself:** no reverse edge measures per-sample clearances.
- **Colours:** `FRoutePlan::DescribeRuns` / `UGroundTraffic::RemainingRouteRuns` (named runs, not
  spans). The HUD looks test now counts `EPreviewStyle` by reflection, replacing the planned
  `EveryPreviewStyleIsDrawn`.
- **Yard layout** changed from section 4's outline: straight-bay stub 45 m, 40 m bays, spur 50 m below
  NE (at 20 m the junctions squeezed the turns to 5.1 m, under the rig's 5.8 m lock), hammer stub
  north. Legs to the straight bay route via the ring's start (the four dead-end balloons otherwise
  exhaust the tow retries). Drawing: `docs/superpowers/plans/2026-09-26-rig-yard.png`.
- **Open:** reverse edges carry no route-cost penalty (the yard is an island, so nothing detours
  through a bay yet). The utility's lock is still unmeasured (45 deg).
