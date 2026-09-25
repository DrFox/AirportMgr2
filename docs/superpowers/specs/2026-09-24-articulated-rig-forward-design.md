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
- **The rig's trailer has a known, bounded near-side overrun.** `RigTestCourseTest.cpp`
  measured 2026-09-25: on every near-side (right) turn the trailer cuts 2.0-3.4 m past the
  inner pavement edge while its swept WIDTH still fits - `VehicleFit` judges the swept
  envelope against the tarmac as a whole, not which side of the lane centreline the cut
  lands on. The test pins the gap at the measured 3.4 m worst case rather than treating it as
  unbounded. **Open question, not decided here:** should `VehicleFit` start judging
  clearance per side while the agent holds its lane, or should the agent swing wide on a
  near-side turn to keep the cut inside its own lane? Either changes gating or steering
  behaviour outside this step's scope.
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
- **The course:** the rig bypasses all three dead-end stems (look-ahead), their U-turns refused
  on the lock-radius reason; its corners on Wide are laid for it.

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

- **The width-step builder fix.** Blend the lane offset over a transition length in the
  guideline builder so a Narrow -> Wide straight-through node stops producing a `MinRadius`-0
  jog. Today it is pinned as a deliberate course feature (§3) rather than routed round.
- **The rig U-turns at road ends once reversing exists (step 2):** a three-point turn, not a
  bigger balloon (user ruling 2026-09-25). Until then every dead end refuses it on its lock.
- **One evaluator for a whole route's tow (a real gap, found 2026-09-25).** Two halves:
  (a) balloon edges carry no clearance data, so `VehicleFit::Judge` returns "fits" before it
  reaches `VehicleSweep::Trace` - the jack-knife check never runs on a balloon; (b) `Trace` lays
  the train straight at the start of EVERY edge, forgetting the hitch angle carried across
  edges - S-bends, the width-step jog, and lead-ins all start the trailer straighter than it is.
  Measured consequence: a rig-lock balloon was admitted and the agent folded on it. Proposed
  fix: trace the WHOLE found plan once, with the chain carried across edges, and reject the plan
  if it folds. Not implemented.
- **The near-side overrun.** The rig's trailer cuts 2.0-3.4 m past the inner pavement edge on
  near-side turns while its swept width still fits (§3, measured and bounded, not fixed).
  Needs a decision: judge `VehicleFit` per side, or have the agent swing wide.
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
