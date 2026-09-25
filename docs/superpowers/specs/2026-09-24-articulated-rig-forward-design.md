# Articulated rig, step 1: import and forward driving on a test course

> Articulated fuel rig (`AirportMgr2Models/truckCab1`: cab `truckCab1` + semi-trailer
> `tankTrailer1`). Decisions taken with the user on 2026-09-23 (memory
> `articulated-vehicles-decisions`) and 2026-09-24 (this session).

## Where this sits

The whole job is four sub-projects, each with its own spec -> plan -> build:

1. **This one.** Import the rig and drive it forwards, with the trailer following, round a
   pre-laid road course that fails fast on road size.
2. Reverse **straight** into a bay (option B, ruled 2026-09-24): pull past, stop collinear
   with the bay, reverse straight in, drive out forwards. Curved trailer reversing is not
   wanted for now.
3. E/F stand bays sized for the rig. `FuelService` sends the rig to big stands and the
   bowser to small ones. Vehicle-to-service becomes many-to-many, with bays admitting
   vehicles by fit (`VehicleFit`).
4. The A/B tug with a 1000 l drawbar trailer, and small-stand templates, which make Codes A
   and B buildable.

## Already on main (not rebuilt here)

- `FVehicle` + `FTrailer` (#271). `UAirsideSettings::ResolveRigVehicle()` holds the rig's
  measured geometry: tractor wheelbase 370, kingpin 57.3 ahead of the drive axle, kingpin to
  tandem centre 1029.5, width 254 (uu). The lock is ASSUMED at 40 degrees.
- `VehicleSweep::Trace` (Solve/) steps cab and trailer along a path. `VehicleFit` gates route
  search on it (#276).
- `FRouteFollower` drives a rigid chassis. It has NO trailer.

## User rulings for this step

| Question | Ruling |
|---|---|
| Trailer length | **Keep the modelled 10.3 m** (airport rigs are long; they need not meet EU). Fail fast on road size; widen roads if the rig does not fit. |
| Where it is tested | **A new test level**, not the Model Yard, and no stands or depots. |
| How | **A pre-laid course that loops on its own.** Refused legs are logged and labelled. |

## 1. Model: trailer state and forward motion

- `FRoadAgent` carries a **hitch angle** (radians, trailer yaw relative to cab yaw). A rigid
  vehicle (`!HasTrailer()`) leaves it at 0 and never steps it.
  **REVISED 2026-09-25 (task 6, checked against the code):** this became a chain, not one
  angle - `FRoadAgent::TowAxles` (`TArray<FVector2D>`) holds each link's own axle position,
  road-plane XY, one per `FTowLink`, filled by `FollowAndTow` every tow sub-step and never
  re-derived from the route. A hitch angle for the actor to draw is recovered from
  consecutive axle positions where the presenter needs one; the agent's own stored state is
  positions, not an angle - see §4, which already documents the chain this replaced the
  single trailer with.
- **ONE STEPPER.** The trailer step inside `VehicleSweep::Trace` is extracted into one
  `Solve/` function. It takes the kingpin's motion over the step and returns the new hitch
  angle, with the kingpin offset EXACT, not placed on the drive axle. Trace and the agent
  both call it. If there were a second evaluator, the rig could drive a trailer path that the
  router judged differently: the guideline graph's sample-once rule, applied to the trailer.
- The rig **spawns straight** (hitch 0). The angle is carried between frames and never
  re-derived from the route.
- **Jackknife guard:** past a limit (say 90 degrees, stated at the site), the agent stops
  and logs `LogAirside` Warning. Driving forwards within the lock never reaches it, so here
  it is a bug detector. Step 2 needs it for real.

## 2. Content and presentation

- **Import:** `truckCab1.glb` and `tankTrailer1.glb` come in as two skeletal meshes, through
  the fleet pipeline. Materials are scraped from the glb onto `M_Fleet` instances, with the
  skeletal-usage flag on the master. Scale is fixed in Blender, never in import settings
  (memory: Interchange scales vertices twice, bones once). A new
  `Tools/Python/import_rig.py` does the first import AND re-imports in place, since
  `import_models.py` is first-import only.
- **Animation:** each mesh gets its own Animation Blueprint, built by script like
  `build_fueltruck_anim.py`. The cab gets steer on the front wheels and spin on all wheels;
  the trailer gets spin on its tandem wheels.
  **REVISED 2026-09-25 (task 6, checked against the code):** wheel radius for the spin is not
  a typed figure - `UAirsideAgentAnim` calls `WheelHubRadius(Mesh->GetRefSkeleton(),
  MainWheelRadius)`, measuring the hub off the vehicle's own imported skeleton (falling back
  to `MainWheelRadius` only when the bone is absent), the same "measured, not typed" rule
  the rest of the fleet's geometry follows.
  **REVISED 2026-09-25 (final-fix wave):** this is not new-vehicle-only - EVERY existing
  ground vehicle's wheel spin changed the moment `WheelHubRadius` shipped, because every one
  of them previously spun on `MainWheelRadius`'s class default (the Meridian aircraft's own
  figure, ~21 uu), and now spins on its own measured hub instead. The fuel truck (bowser) is
  the visible case: its hub sits well above 21 uu, so its wheels now turn about 2.5x SLOWER
  than they did before this branch - and that slower rate is the correct one, not a
  regression. `RigContentTest.cpp` pins the cab radius for `fueltruck1` and `truckCab1`
  against their measured hub Z so a future change to either mesh or to `WheelHubRadius` is
  caught here rather than only noticed by eye.
- **Content:** `UAirsideSettings` resolves the rig's meshes and ABPs in one function, beside
  `ResolveRigVehicle()`. No asset path appears at a second site.
- **Actor:** `ARoadAgentActor` gains an optional trailer skeletal-mesh component, created only
  when `HasTrailer()`. Each frame it is placed on the cab's kingpin world position at
  yaw = cab yaw + hitch angle. The actor READS the model's angle and never computes its own.
  Rigid vehicles are unchanged.
  **REVISED 2026-09-25 (final-fix wave, checked against the code):** this is not what ships.
  `ARoadAgentActor::TickComponent` (`RoadAgentActor.cpp`) places each body-carrying tow link's
  mesh with `SetWorldLocationAndRotation(FVector(Pose.Axle.X, Pose.Axle.Y, ...),
  FRotator(0, Pose.Heading, 0))` - its OWN AXLE (the tandem centre, which is the mesh's own
  local origin - see `RigContentTest.cpp`) at its OWN heading, one call per link in the chain
  (`TowViews`), not the cab's kingpin position or `cab yaw + hitch`. The chain generalisation
  (spec §4) is what moved placement from "one trailer on the cab's coupling point" to "every
  link on its own link pose"; the code was updated, this paragraph was not.

## 3. Test course and loop

- **`ARigTestCourse`** lays its roads at `BeginPlay` through `IRoadEditTarget`
  (`PlaceNode` / `ConnectNodes`). The course is code, not saved level data, so the same
  course runs headless in a test, and none of the silent-success traps of headless level
  editing apply.
- **Layout:** three parallel lanes, one per width tier (Narrow, Standard, Wide). Each has a
  long straight, a left 90 and a right 90, a T junction taken both ways, and a dead end with
  its U-turn. The lanes are joined into one circuit, so one loop visits every feature.
- **`M_RigTest`** holds a floor, the network actor, the course actor and the road-build game
  mode (so the camera and tools work). It is made once by `Tools/Python/build_rig_test_level.py`.
  **REVISED 2026-09-25 (task 6):** built. The floor is centred and sized off
  `RigTestCourse.cpp`'s own layout constants (250 x 340 m course, 500 x 600 m floor,
  centred on the course's own bounding box, not the world origin - the course lays its nodes
  in absolute world XY, so the floor follows it rather than the other way round), plus a sun,
  sky light and fog matching `M_ModelYard`'s. `ARoadNetworkActor` and `ARigTestCourse` are
  spawned with every property at its content-resolved default (`ResolveSurfaceMaterial` and
  siblings fall back to `UAirsideSettings::GetContent()` when the actor's own field is null),
  and the world settings' game mode override is `BP_RoadBuildGameMode`.
- **Driving:** the course names an ordered list of waypoint nodes. A small driver sends the
  rig from each waypoint to the next through the normal route search, with the rig's
  `FVehicle`, so `VehicleFit` gating applies. On arrival it takes the next leg; after the
  last it wraps to the first, forever.
- **Failing fast:** a leg with no route that fits is SKIPPED, not stopped on. It is logged
  once per loop, e.g. `LogRoadBuild: RigCourse: leg 7 (Standard, right 90) refused: TooNarrow
  at node 31, swept 7.6 m vs tarmac 6.0 m`, and a red label marks it on the road. Each loop
  ends with `RigCourse: loop N - D/T legs driven; refused: <list>`, which says which tier to
  widen.

**REVISED 2026-09-25 (task 6, checked against the code) - four course facts pinned by the
built level and its tests:**

- **A width-step feature is laid on purpose, to pin a builder defect.** The course's return
  road changes Narrow -> Wide mid-straight, at a degree-2 node (`RigTestCourse.cpp`'s
  `RigCourse::WidthStepX`), rather than routing round it. The derived turn there is a
  lane-offset jog with `MinRadius` 0 (unmeasured, so route search does not gate it), and
  `FSpeedProfile` reports it as a sharp vertex and crawls: 55 s over a 69 m straight that
  takes 14 s without the step (measured, `RigTestCourse.cpp`'s own comment). The fix belongs
  in the guideline builder - blending the lane offset over a transition length - not in the
  course; kept as a feature (controller ruling 5, 2026-09-25) rather than laid round, so the
  defect stays pinned until that fix lands. Open follow-up below.
  **REVISED 2026-09-25 ("width taper", user ruling: inset nodes and a curve between the two
  widths) - fixed in the builder.** At a straight-through node of two arms whose lanes sit at
  different offsets, `FRoadNetworkSolver` insets both cuts by half a taper length
  (`FJunctionArm::MinCutDistance`, capped by the arm's slack allowance so a short segment gets a
  shorter taper, never a failed node); the polygon between the two cut lines IS the taper, paved
  from the same cut vertices as the ribbons (bitwise weld, measured by
  `Airside.Build.WidthTaper.SurfaceWelds`: every cut-line edge shared by exactly two triangles).
  Each lane crosses on an S of two quadratics meeting mid-way - `GuidelineGeom`'s own lane change
  (`ShiftDeflectionFor`): tangent s deflecting by b, L = 2 s (1 + cos b) along, 2 s sin b across -
  sized for the WIDER tier's design vehicle. Narrow -> Wide: lane shift 75 uu, rig lock 576 uu,
  taper 412 uu (206 each side), measured MinRadius 576.2 uu per piece; the circular reverse
  curve's sqrt(4Rd - d^2) would say 409. The course keeps the feature; its expected result flipped
  to no sharp vertex, and neither vehicle crawls there. Consequence to know: a Narrow -> Standard
  taper is sized for the bowser, so the rig is now REFUSED on its lock there, where the unmeasured
  jog used to wave it through to crawl.
  **Review round:** the trigger is a LANE OFFSET, not a width difference - intended: a same-width
  profile change whose lanes move, and a one-lane bidirectional road meeting a two-lane one, taper
  too (`WidthTaper.SameWidthOffsetLanes`, `.OneLaneMeetsTwo`: insets 174 and 272 uu, bowser-sized).
  The S is ONE construction in `GuidelineGeom` (`LaneChangeLength` sizes, `LaneChange` lays;
  `Airside.Solve.LaneChangeRoundTrips`). A taper capped by a short segment warns against the
  vehicle that sized it, with the length it needs (`WidthTaper.CappedTaperWarns`: 3 m Wide stub,
  S 397 uu vs the rig's 576, "at least 4.6 m").
- **The rig's trailer has a known, bounded near-side overrun.** `RigTestCourseTest.cpp`
  measured 2026-09-25: on every near-side (right) turn the trailer cuts 2.0-3.4 m past the
  inner pavement edge while its swept WIDTH still fits - `VehicleFit` judges the swept
  envelope against the tarmac as a whole, not which side of the lane centreline the cut
  lands on. The test pins the gap at the measured 3.4 m worst case rather than treating it as
  unbounded. **Open question, not decided here:** should `VehicleFit` start judging
  clearance per side while the agent holds its lane, or should the agent swing wide on a
  near-side turn to keep the cut inside its own lane? Either changes gating or steering
  behaviour outside this step's scope.
  **REVISED 2026-09-25 ("bend lanes follow the pavement concentrically; inside widened only by
  the measured remainder", user-approved) - step 1 of 2, the lanes.** At a two-arm bend of one
  width, each service-road lane's turn is now an ARC about the inner fillet's centre, at the
  lane's own distance from the inner edge, laid as quadratics of at most 22.5 degrees each
  (`GuidelineGeom::Arc`) and sampled once like any edge. The lane ends were already that circle's
  tangent points (the cut is the inner fillet's tangent), so nothing moves at the lane ends.
  Measured on a plain right angle per tier (`Airside.Build.BendLanes.*`), before -> after:
  inner lane 726 -> 1006, 743 -> 1031, 853 -> 1183 uu; outer lane 938 -> 1301, 991 -> 1374,
  1171 -> 1624 (Narrow, Standard, Wide). **Not concentric with the OUTER edge, and why:** both
  fillets are the tier's one radius (816 / 816 / 921), so their centres sit a road width apart on
  each axis and no circle is concentric with both. About the outer centre the lanes would use the
  outside of the bend - and turn at 306 / 606 (Narrow), 231 / 581 (Standard), 186 / 636 (Wide),
  below both design vehicles' locks; the ruling forbids a tighter turn, so that reading STOPPED on
  measurement. The unused band outside a bend is the outer fillet being the same radius as the
  inner (R, not R + width), not the lanes. **What the arc did NOT do:** the rig's trailer still
  leaves the inner edge - 340 -> 358 (Narrow), 309 -> 328 (Standard), 225 -> 247 (Wide) uu,
  traced (`BendProbe`, `VehicleSweep::Trace`) - a little worse, because the one quadratic bulged
  62 uu off the inner edge at its apex and the arc keeps the straights' own distance throughout.
  The bowser and the utility stay on the tarmac on every tier, before and after. Mixed-width bends
  keep the quadratic (the lanes sit 210 and 285 uu off their inner edges, so no one circle fits);
  taxiway bends are untouched (authored for aircraft, `PreferredFilletRadius`); T and X junctions
  are untouched in this step.
  **REVISED 2026-09-25 (same ruling) - step 2 of 2, the inside widened only by the measured
  remainder.** The solver (`FRoadNetworkSolver::SolveNodeCuts`, `BendWidening`) drives the bend's
  design vehicle round every concentric lane turn (`VehicleSweep::Drive`, the pursuit `Trace`
  runs, split out so the widening and the router share it), puts every body point against the
  inner edge as one length along it (arm, fillet, arm), and pushes that edge into the grass by the
  deepest reach plus 25 uu (`VehicleFit::WidthMargin` 15 + the clearance march's 10), leaning back
  1:4 either side. The arms are cut back past the fillet to hold it (a floor under the cut, capped
  by the allowance like a taper's inset) and the rim replaces the fillet's arc in the junction
  polygon (`FJunctionArm::RimToNext`), so the ribbon welds to the same shared cut vertices, bitwise
  (`BendLanes.WeldExact`), and the fan paves it with the same bands. The lanes keep their arc and
  reach their moved lane ends by straight leads (`GuidelineGeom::BendLane`, one construction for
  the builder and the solver). The corner's design vehicle is the less demanding arm's, as its
  fillet is: the rig on Wide, the bowser on Narrow and Standard - which leaves those tarmacs on no
  bend, so they do not move. `FRoadDesignVehicles` carries whole vehicles now (`VehicleFor`), since
  a chassis has no trailer to trace. Measured on the plain right angle: Wide cuts 1431 -> 1797 on
  the arriving arm and 2418 on the leaving one (the trailer converges on its line for ~8 m after
  the turn), the rig 247 -> 0 uu off the tarmac and passing within the margin of the new edge
  (`BendLanes.WideBendCarriesTheRig`, `.WideningOnlyWhereNeeded`). **Where a short arm caps it the
  rig still cuts in:** on the rig course the Wide lane's corners have a 30 m arm already cut to its
  allowance, so the widening is capped there (traced 231 -> 253 -> 188 uu); the connector corners
  with long arms go 225 -> 247 -> 0-13. Logged Verbose per solve (`LogRoadSolve`, "capped by its
  arms' length"), since the snap solves nodes on every cursor move.
- **The rig is refused at every dead-end U-turn, and this is a sizing decision, not a bug.**
  **(Ruled 2026-09-25, see the per-tier design vehicle note below: balloons stay bowser-sized;
  the rig turns at road ends once reversing exists.)**
  `UTurnGeom::Balloon`'s dead-end balloons are sized off
  `UAirsideSettings::ResolveLargestServiceVehicle().TightestFollowableRadius()` - the bowser,
  510 uu (5.1 m) forwards (`Chassis.h:406`'s own comment: "For the 6.2 m bowser
  that is 361 uu against 510"). The rig's own figure is `Wheelbase()/sin(lock)` = 370 uu /
  sin(40 deg) = 576 uu (5.8 m), so every dead end on this course refuses it by design - the
  balloon was never built to admit it. **Open, for the user:** widen every dead-end balloon
  to the rig's 5.8 m (which oversizes it for every other vehicle that uses one today), add a
  second, rig-sized balloon class, or accept that the rig never reverses out of a dead end on
  this course.

**REVISED 2026-09-25 ("continuous", user-approved design, checked against the code) - both
tows drive the course at once, continuously:**

- **One persistent agent per vehicle.** At each waypoint the arrived agent is REDIRECTED
  (`UAirsideTraffic::RedirectAgent`) onto its next leg, not retired and respawned, so the tow
  chain (`FRoadAgent::TowAxles`) carries across the waypoint instead of being re-laid straight.
  Finding: a redirect already kept the chain (`RestartTaxi` never touches `TowAxles`; only
  `StartDrive` lays it straight) and does NOT clear `JackknifedLink` - but it re-seeded the
  cab's HEADING from the new line. At the width step's jog that swung the cab's fixed axle
  sideways in one frame and folded both tows on the first frame of the next leg; every other
  handover moved the rig's trailer axle up to 350 uu. `UGroundTraffic::RedirectAgent` now keeps
  the heading of anything that has a chain (`RestartTaxi`'s new `InitialHeading`); rigid
  vehicles and aircraft still face their new line at once. Pinned by
  `Airside.Model.Tow.RedirectKeepsChainAndHeading` and by `OneLoopHeadless`'s handover check
  (axles move <= `VehicleSweep::TraceStep` across every handover; a re-lay would have moved
  the rig's by up to 660 uu).
- **Opposite directions.** The rig runs the waypoints forwards; the utility + trailer runs
  them in reverse, arriving at each waypoint along `(Node, Next)` - `FRigCourseWaypoint::Next`
  is the first node of the road the forward leg leaves by - after `UtilityStartDelay` (20 s,
  so the rig is clear of the shared start junction). Results are keyed by FORWARD leg index
  (the same node pair) in both directions. A leg turns at the node it LEAVES, so a reversed
  leg turns at its other end: the utility's dead-end U-turn falls in "T junction into the
  stem, reversed", and its width-step jog in "return, to the width step, reversed".
- **Refused legs.** A refused leg is logged once per loop and keeps its red label; the vehicle
  routes on to the next waypoint it can reach from where it stands. None is reachable from a
  dead end the rig cannot U-turn in (§3's sizing decision), so there it is STRANDED: logged,
  retired, and dispatched fresh at the next waypoint - the one place the rig's chain is still
  re-laid (3 times a loop). Jack-knifed and stuck vehicles take the same exit.
- **Per-vehicle loops:** `RigCourse: rig loop N - D/T legs driven; refused: ...` and
  `RigCourse: utility loop N - ...`.
- **Known gap, measured not fixed:** claims do not reserve the oncoming lane, and the two
  vehicles' bodies do overlap - see `OneLoopHeadless`'s overlap lines for the worst figure and
  where. No claim logic was added.

**REVISED 2026-09-25 ("per-tier design vehicle", two user rulings the same day) - corner
fillets are sized per width tier; dead-end balloons are NOT:**

- **Each service-road width tier names the design vehicle of its CORNERS:** Wide = the
  articulated rig, Narrow and Standard = the bowser (unchanged). Decided in ONE place,
  `UAirsideSettings::ResolveTierDesignVehicles` (Wide is `ServiceRoadProfiles[WideServiceTier]`),
  cached there (re-resolved only when the content set or its Wide asset changes, because the
  snap and ghost paths ask per arm per cursor move), and carried per rebuild as
  `FRoadDesignVehicles` (`Profiles/RoadDesignVehicles.h`) in place of the one `FChassis` the
  solver and builder took. Per tier: junction fillets (per arm in `FRoadNetworkSolver`) and the
  builder's corner-shortfall warning. **Stayed the bowser:** stand and depot links
  (`FAnchorLink`, `PoseSetbackFor`) and stand layout - rigid trucks use them; the rig has none
  yet (step 3). Taxiways and runways are unaffected.
- **Dead-end balloons stay bowser-sized on EVERY tier, by ruling ("smaller, reverse later").**
  Sizing the Wide balloon for the rig was tried and measured: at the rig's 576 uu lock the router
  admitted it and the agent folded the trailer (the lock is the cab's limit, not the trailer's);
  a balloon the rig can be driven round without folding reaches ~24 m past the road end. The
  user chose the smaller road: the rig is refused at every dead end on its lock, and will turn
  at a road end with a three-point turn once reversing exists (step 2) - not with a bigger
  balloon. Pinned by `Airside.Build.DesignVehicle.WideDeadEndRefusesRigUntilReversing`.
  **REVISED 2026-09-25 ("same footprint, gentler curves", approved, then STOPPED on measurement,
  not landed):** the balloon was reshaped inside today's bowser footprint and measured. The box
  (old construction, bowser lock 510.1 uu): 1818 uu past the lane ends, 727 uu either side, on
  every tier; its tightest piece 514 uu (a quadratic across a 90 degree arc gives only 0.707 R).
  Reshaped - half circle in four 45 degree quadratics, `GuidelineGeom`'s lane change out to it,
  circle radius chosen where the two deliver the same radius - the tightest piece rose to
  642 / 650 / 666 uu (Narrow / Standard / Wide) at reach 1818 and half-width 695 / 704 / 721,
  i.e. no larger on either axis; free S tangent lengths gained nothing, 8 circle pieces ~27 uu more.
  All three clear the rig's 575.6 uu lock, so the router ADMITTED the rig at every dead end - and
  the rig JACK-KNIFED (90 deg at link 0) in all three balloons on `OneLoopHeadless`, the utility
  and bowser driving them cleanly. The lock is the cab's limit, not the trailer's, and
  `VehicleFit` does not trace a balloon's tow (then Open: "One evaluator for a whole route's tow", both
  halves). So inside this footprint a gentler balloon only moves the rig's refusal from the lock
  to a fold; it was not committed. To land it, the whole-route tow trace must land first (the rig
  then refused on Jackknife, the bowser and utility on gentler curves); the patch is kept with the
  session's report.
  **REVISED 2026-09-25 ("whole-route tow check" landed, then the reshape - LANDED):** with the
  router following the trailer over the whole route (see the note below), the reshape went in as
  measured - same footprint (1818 x 727 uu box, `UTurnGeom::FootprintFor`), 4 + `CirclePieces`
  quadratics, the swing out laid by `GuidelineGeom::LaneChange` (the taper's S, one construction),
  tightest 642 / 650 / 666 uu (Narrow / Standard / Wide), `Airside.Solve.UTurnBalloonFootprint`.
  Every piece now clears the rig's 575.6 lock, and the whole-route check refuses the rig at all
  three on its TRAILER: "trailer folds at guideline node N / (x, y), link 0, angle 90 deg" - the
  rig does not turn in any balloon inside this footprint; it turns where its trailer holds, and
  it holds in none. The bowser and the utility's drawbar train (judged whole-route too) turn at
  every dead end. `Airside.Build.DesignVehicle.WideDeadEndRefusesRigUntilReversing` pins it: each
  edge fits the rig alone, the whole route refuses it (`EFitRefusal::TrailerFolds`), and the agent
  driven round the unjudged plan anyway jack-knifes - the refusal is the agent's.
- **The course:** the rig bypasses all three dead-end stems (look-ahead), their U-turns refused
  on the TRAILER-FOLD reason since 2026-09-25 (was the lock radius; `OneLoopHeadless` asserts the
  reason); its corners on Wide are laid for it.

**REVISED 2026-09-25 ("whole-route tow check", user-approved) - the router follows the trailer
over the whole planned route, so it cannot send a tow where its trailer folds:**

- **The gap it closes** (was Open, "One evaluator for a whole route's tow"): `VehicleFit::Judge`
  lays the train straight at the start of every edge, and returns "fits" before tracing an edge
  with no clearance data (balloons), so the router admitted the rig into reshaped balloons whose
  every piece cleared its lock, and the agent jack-knifed in all three.
- **The check:** `VehicleFit::JudgePlan` drives a found plan exactly as `FRoadAgent` will - a
  `FRouteFollower` from rest, the chain laid straight ONCE at the plan's first point
  (`VehicleFit::LayTow`, which `StartDrive` now calls) and stepped every
  `VehicleFit::TowSubStepSeconds` with `VehicleFit::StepTow`, the one step `FollowAndTow` now
  takes too. It refuses on a fold (`EFitRefusal::TrailerFolds`), and on swept body over tarmac
  at any sample of an edge that HAS per-sample clearances (the per-edge rule, on the whole
  route's carried chain). A lint row holds `VehicleSweep::StepChain` to its two sanctioned callers.
- **Where it hooks:** `RouteSearch::Find`, after a found plan, only when the query's vehicle has a
  tow - a decorator on the search, not a term in it (a trailer's angle depends on the path into a
  node, which A* cannot cost). Rigid vehicles and aircraft never reach it: their routing is
  bit-identical (`Airside.Model.Tow.WholeRouteSkipsRigidAndAircraft` counts zero checks).
- **Retry:** on a fold the edge the cab is driving at the first failing sample is excluded and
  the search re-run, up to 4 times; then `TooNarrow` with `FRoutePlan::RejectedBy` carrying the
  first fold: "trailer folds at guideline node N / (x, y), link L, angle A deg". Excluding where
  the angle began to build was traced and rejected: that is usually a junction's entry curve the
  way round shares.
- **One evaluator, pinned:** `Airside.Model.Tow.WholeRouteVerdictIsTheAgents` - over ten shapes
  the router's verdict equals whether the dispatched agent jack-knifes, at the check's own step
  (fold at the same route distance, bitwise) and at the game's 1/30 s frames.
- **An S relieves, it does not fold** (the brief proposed an S as the failing case): after a left
  curve the trailer lags right, and a right curve swings the cab back toward it. What folds is
  same-hand turning: three 90 degree quarters that each clear the lock (worst 90 deg) where two
  hold (75 deg). `Airside.Model.Tow.WholeRouteFoldRefused`.
- **Known approximation:** the chain is laid straight at the PLAN's start. A redirect or a rebuild
  re-resolve plans from a vehicle whose trailer is already angled; the check starts it straight.

**REVISED 2026-09-25 ("one route per loop") - the course drives one route per loop, like the
game; legs are progress markers:**

- **Why:** the user saw the rig brake to a halt at every waypoint. Each leg was its own route,
  and `FSpeedProfile` brakes to zero at a route's end (leg 0, 68 m, took 14.3 s - exactly
  rest to rest). In the game a job dispatches ONE plan to a real destination and intermediate
  road nodes stop nothing.
- **How:** at each loop start every leg is still planned for its verdict (look-ahead, bypass,
  refusal - unchanged); the admissible legs are joined into ONE `FRoutePlan` with the router's
  own `RouteSearch::Splice`, and dispatched once. A leg is ARRIVED when the agent's distance
  along that route passes the leg's end (`FRigLegMarker`); the arrival log, the per-leg timeout
  (from the previous marker) and the per-leg sharp-vertex report keep their shape.
- **Loop to loop:** within twice its stopping distance of the route's end, the next loop is
  planned and spliced onto the LIVE route with the new `UGroundTraffic::ExtendRoute`
  (`Splice` + `FRouteFollower::Replace`: Travelled, Speed, Heading and the chain carry on).
  `RedirectAgent` was rejected for this: it restarts the follower from rest, the very stop being
  removed. It survives only as the fallback when a route runs out unjoined.
- **Review of ed81410c:** `ExtendRoute` moves the goal through the same `ReleaseGoal`/`TakeGoal`
  as `RedirectAgent` (stand wait cleared, departure re-armed or disarmed for the new end -
  `ArmDepartureIfRunway` now disarms, which a redirect off a runway also lacked - the occupancy
  revision bumped), warns when a tail does not continue the route, and trims the driven history
  (`KeepBehind`, Travelled rebased) so an endless course's route stays bounded. The course
  reports a sharp vertex AT a join; the only ones were the width step's jog, at its node's lane
  end - none since the width taper (§3).
- **A GAME FIX, NOT A COURSE FIX (2026-09-25, 0277a642 + review round):** the utility skipping
  every dead end in reverse was `FPlanReResolver::ReResolvePlan` - which runs for EVERY agent
  class on EVERY graph rebuild, on main too. It found each step's end by position; at a
  straight-through node the two arms' lane ends are coincident twins joined by a zero-length
  turn path, `FindNearestNode`'s slot-order tie-break named the wrong one, the step "failed", and
  the replan to the route's end dropped every via point. Fixed: a position resolves to the node
  and its zero-length twins, the step takes the one its edge reaches (the first step's start is
  re-pointed when its twin is the one), and an intact plan's goal is its last step's end.
  Pinned by `Airside.Model.Traffic.RebuildCoincidentTwins`, `.RebuildTwinAtCurrentStep`,
  `.RebuildGoalIsATwin` and `AirportMgr.RigCourse.RebuildKeepsTheCourse`.
- **Measured:** the rig's leg 0 now takes 12.1 s against a derived no-stop 11.8 s (it slows for
  the corner at its end); every waypoint is passed moving, at the speed profile's own limit
  where the profile slows it. One dispatch per vehicle, one extension per loop boundary.

## 4. REVISED 2026-09-24: the tow is a CHAIN, and the utility + fuel trailer drives too

User ruling (the same day, before Task 2): `AirportMgr2Models/utility1` now has `fuelTrailer1`,
a **full drawbar trailer**. A towbar pivots on a steered front axle (`towbar_yaw`,
`steer_FL/FR`), and the body has a fixed rear axle. It couples its `tow_eye` onto utility1's
`hitch` socket, which is about 0.91 m BEHIND utility1's rear axle at z 0.308. The trailer
wheelbase is 2.21 m (from the model scripts; MEASURE from the glb, do not retype).

- **The tow is a chain of links, not one semi-trailer.** Each link is (hitch offset along the
  previous body from its fixed axle, where negative is behind; length from the hitch to this
  link's axle; body extents and width). The rig is ONE link (hitch +57.3, length 1029.5). The
  drawbar trailer is TWO: the towbar (hitch -~91, length = eye to front axle, no body), then
  the body (hitch 0 at the front axle, length = the trailer wheelbase, body extents). A
  baggage train later is N links. `FTrailer` becomes this chain on `FVehicle`, and the rig's
  figures move into it unchanged.
- **The same stepper runs per link**, pulled by the previous link's hitch point: the Task 1
  `StepTrailer`. `VehicleSweep::Trace` and `VehicleFit` walk the chain, so gating covers the
  drawbar trailer too. It stays ONE evaluator.
- The agent stores one axle position per link. Motion carries a pose per link. The actor
  places one mesh per BODY-carrying link, and the towbar is animated on the trailer's own
  `towbar_yaw` bone from the towbar link's angle.
- **The test course loops both vehicles**, rig and utility + trailer, one at a time and
  alternating, and reports refusals per vehicle.
  **REVISED 2026-09-25:** no longer one at a time - both at once, in opposite directions,
  each with one persistent agent; see §3's "continuous" note.

## Tests

- **Stepper:** a straight keeps the hitch at 0; a steady circle converges to the analytic
  angle; the kingpin offset changes the answer (the offset is not dropped).
- **One stepper:** the agent driven along a 90 degree turn produces the same trailer axle
  samples as `VehicleSweep::Trace` on the same path (it extends `SweepAgreementTest`).
- **Actor:** a rig actor has a trailer component and a rigid one has none. After a tick, the
  trailer's world yaw == cab yaw + hitch, and its origin sits on the kingpin (57.3 uu ahead of
  the drive axle).
  **REVISED 2026-09-25 (final-fix wave, checked against the code):** same correction as §2's
  Actor bullet - the shipped placement is per-link, each mesh at its own `Pose.Axle` and
  `Pose.Heading`, not at the cab's kingpin and `cab yaw + hitch`. See
  `RoadAgentActor.cpp`'s `TickComponent`.
- **Content:** both meshes and both ABPs resolve.
- **Course (headless composition):** the course builds its feature count. One full loop at a
  fixed time step drives every leg `VehicleFit` admits and reports exactly the refused ones,
  no jackknife guard fires, and the trailer stays inside the tarmac clearance at every sample.

## Out of this step

Reversing (step 2). Stands, depots and `FuelService` (step 3). The tug and trailer (step 4).
The rig's own performance figures (it inherits the bowser's, as `ResolveRigVehicle` says).
Measuring the real lock (40 degrees stays assumed). Oncoming-lane claims for a rig that swings
across both lanes (a known gap from the road-lanes spec).

## Open

- If the loop reports refusals on Standard or Wide, the roads-spec response is to widen the
  tier or its derived corner. That is a follow-up decision, not part of this step.

**ADDED 2026-09-25 (task 6), follow-ups found by the full build and not fixed here:**

- **A zero-length twin STEP re-resolved in reverse** (review item 5, rare): where the plan's own
  step is the zero-length B1->B2 path and the lookup names the twins the other way round, the
  twin match can pick the wrong one of the pair; the step then fails and replans as before the fix.
- **`TwinsOf` searches one hop** (review item 6): only nodes a single zero-length edge joins count
  as twins. Three or more coincident nodes chained through each other are not gathered.
- **A taper capped to nothing falls back to the chord jog** (review item 9): `GuidelineGeom::
  LaneChange` lays nothing when the lane ends are within 1 uu along, so a segment too short to
  inset at all still gets the old straight diagonal. The capped-taper warning names the length.
- **The rig U-turns at road ends once reversing exists (step 2):** a three-point turn, not a
  bigger balloon (user ruling 2026-09-25). Until then every dead end refuses it - on its trailer
  folding since the reshape and the whole-route check (2026-09-25), on its lock before.
- **The near-side overrun, narrowed (2026-09-25, bend lanes).** Fixed where the rig is the design
  vehicle and the arms hold it: a Wide two-arm bend. Still open (a) on Narrow and Standard bends
  and at every T/X junction, where the rig is not the design vehicle - the course's worst, 355 uu,
  is a Narrow T out of the stem; (b) on a Wide bend whose arm is too short to hold the widening
  (the course's Wide lane corners, 188 uu). Needs a decision for (a): judge `VehicleFit` per side,
  have the agent swing wide, or widen T/X corners the same way; (b) is the segment length.
- **`AAnimYard` draws no trailer.** The bench adopts one `ASkeletalMeshActor` per placed
  model and calls `SetVehicleAirframe`/`SetAirframe` with a single mesh; nothing in
  `AnimYard.cpp` wires a second, tow-carried mesh for a body-carrying link, so a rig or
  drawbar trailer placed in `M_ModelYard` would animate its cab only.
- **utility1's steering lock is unmeasured.** `ResolveUtilityTowVehicle` leaves it at
  `ResolveDefaultVehicle`'s 45 degrees; utility1/SPEC.md's own "turning radius 115 in
  (2.921 m)" datasheet figure would resolve to about 27 degrees on the measured 1.493 m
  wheelbase if measured properly against the model, per that function's own comment.
- **The models moved (`MOVED.md`).** `AirportMgr2Models/fueltruck1/export/` now holds only
  `MOVED.md`: since 2026-09-24 fueltruck1 is built inside `rigidCab1/rigidCab1.blend` and
  exported to `rigidCab1/export/fueltruck1.glb`. This branch's own copies of
  `import_fueltruck.py`, `reimport_fueltruck1.py` and `build_fueltruck_anim.py` (checked in
  this worktree, `C:\repos\airportmgr-rig`) still point at the old, now-empty path.
- **The reverse chain (step 2).** Reversing does not step the chain: `EAgentPhase::Reversing`
  moves the fixed axle through `Reverse.Advance` alone, and nothing updates `TowAxles` while it
  does, so a trailer's axle positions stay frozen for the whole reverse. The post-reverse
  drive-on then calls `FollowAndTow` from those frozen axles against the cab's new (reversed)
  position in one step - a jump `StepTrailer`'s `Dot < 0` fold guard could misread as a
  jack-knife that never happened. Out of this step already (see "Out of this step"), restated
  here because it is the next of the four sub-projects "Where this sits" lists.
