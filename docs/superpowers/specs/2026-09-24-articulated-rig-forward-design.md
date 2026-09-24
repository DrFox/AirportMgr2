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
- **Content:** `UAirsideSettings` resolves the rig's meshes and ABPs in one function, beside
  `ResolveRigVehicle()`. No asset path appears at a second site.
- **Actor:** `ARoadAgentActor` gains an optional trailer skeletal-mesh component, created only
  when `HasTrailer()`. Each frame it is placed on the cab's kingpin world position at
  yaw = cab yaw + hitch angle. The actor READS the model's angle and never computes its own.
  Rigid vehicles are unchanged.

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
- **Driving:** the course names an ordered list of waypoint nodes. A small driver sends the
  rig from each waypoint to the next through the normal route search, with the rig's
  `FVehicle`, so `VehicleFit` gating applies. On arrival it takes the next leg; after the
  last it wraps to the first, forever.
- **Failing fast:** a leg with no route that fits is SKIPPED, not stopped on. It is logged
  once per loop, e.g. `LogRoadBuild: RigCourse: leg 7 (Standard, right 90) refused: TooNarrow
  at node 31, swept 7.6 m vs tarmac 6.0 m`, and a red label marks it on the road. Each loop
  ends with `RigCourse: loop N - D/T legs driven; refused: <list>`, which says which tier to
  widen.

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

## Tests

- **Stepper:** a straight keeps the hitch at 0; a steady circle converges to the analytic
  angle; the kingpin offset changes the answer (the offset is not dropped).
- **One stepper:** the agent driven along a 90 degree turn produces the same trailer axle
  samples as `VehicleSweep::Trace` on the same path (it extends `SweepAgreementTest`).
- **Actor:** a rig actor has a trailer component and a rigid one has none. After a tick, the
  trailer's world yaw == cab yaw + hitch, and its origin sits on the kingpin (57.3 uu ahead of
  the drive axle).
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
