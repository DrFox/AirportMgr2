# Handover: round the stand service loop's corners

Paste this into a fresh session. Everything below was established by measurement in the
session of 2026-09-14; nothing in it is a guess unless it says so.

---

## The job

**Every corner of every stand's service loop is a hard vertex, and a vehicle cannot take one
at any speed.** Round them as the loop is laid.

`UEntityDefinition::ServiceLoop` is an authored list of corner points. `FServiceLoopBuild`
turns each point into a guideline node and joins consecutive nodes with STRAIGHT edges:

```cpp
// Plugins/Airside/Source/Airside/Private/Build/ServiceLoopBuild.cpp  (~line 121)
TArray<FGuidelineNodeId> Corners;
for (const FVector2D& Point : Local)
    Corners.Add(Network.AddGuidelineNode(ToWorld(Point), /*bDerived=*/true));

for (int32 At = 0; At < Corners.Num(); ++At) {
    const FGuidelineNodeId A = Corners[At];
    const FGuidelineNodeId B = Corners[(At + 1) % Corners.Num()];
    Lane.Add(Network.AddGuidelineEdge(MakeServiceEdge(A, B, PositionA, PositionB, ...)));
}
```

No arcs, no trim, nothing that could smooth them. `FSpeedProfile::Build` treats a vertex
whose heading changes instantly as untakeable and drops the agent to `MinTaxiSpeed`
(50 uu/s = 0.5 m/s), so a fuel truck crawls round the stand.

### The evidence

Reported from play: the fuel truck crawls the last corner onto the stand. Its route log:

```
Speed profile IN: type=FUEL, 37 point(s), wheelbase 360.7 uu, lock 45.0 deg, Ground.IsSet=1
Speed profile: 7509 uu, 37 point(s). Tightest R=418 uu at 3254 -> 50 uu/s
  (TIGHTER THAN THE STEERING LOCK). 1 sharp vertex/vertices, first 90 deg at 6119.
  Floor 50, taxi cap 1000, lock allows R>=510 (wheelbase 361, lock 45 deg).
```

The **sharp vertex at 6119 uu** is route point `(16334,2488)`, where the polyline runs
2350 uu east then turns straight south. That is a service loop corner. The route is
`(13974,2488) (13984,2488) (16334,2488) (16334,1498) (16334,1098)`.

The *other* problem in that same log — `Tightest R=418` — was a different defect and is
**already fixed**; see "Already done" below. Do not re-chase it.

### Two wrong turns already taken, so they are not repeated

1. **"Guideline arcs are a runway-exit-only feature."** Wrong. `RoadGuidelineBuilder` lays a
   *turn path* between every pair of arms at a node, for non-continuous roads too. The
   runway-exit pass (gated on `bContinuousThroughJunctions`) is a separate, additional thing.
2. **"It is a road junction, so the fillet radius governs it."** Wrong. It is not a road at
   all. Redrawing the road does not touch it, and the fillet change did not fix it. Road
   corners get arcs because the arms are TRIMMED back by junction pavement and a quadratic
   spans the gap; a service loop has no pavement and no trim.

---

## What to build

Replace each corner with sampled arc points in that loop in `ServiceLoopBuild.cpp`.

Existing pieces to reuse rather than reinvent:

| what | where |
|---|---|
| `RoadGeom::SolveFillet(A, B, Radius)` | `Private/Solve/RoadGeom.cpp:140` — already clamps a radius that will not fit |
| `RoadGeom::SampleArc(Fillet, SegmentCount, OutPoints)` | `Private/Solve/RoadGeom.cpp:209` |
| `GuidelineGeom::VertexHeadings(Points, At, Arriving, Leaving)` | reports the SAME heading both sides of a sampled curve — that is what makes a corner stop being "sharp" |

### The hard constraint

A rigid vehicle cannot follow an arc tighter than `Wheelbase / sin(lock)` at **any** speed.
For `UAirsideSettings::ResolveDefaultVehicle()` that is now:

```
360.7 uu / sin(50 deg) = 471 uu  (4.71 m)
```

So the loop's corner radius must clear 471 uu, with margin if anything downstream can scale
it down. `FSpeedProfile::Build` computes the same figure as `TightestFollowable` — do not
duplicate the expression, but do assert against it.

### Two open design decisions — ASK, do not assume

1. **What radius?** It must clear 471 uu with margin, but the loop hugs the stand and a big
   radius eats clearance from the aircraft. Options: a fixed authored figure; derived from
   the loop's own offset from the stand box; or a field on `UEntityDefinition`. There may be
   a natural bound from the loop geometry — check before inventing a constant.
2. **Short legs and tight corners.** A radius that does not fit between two corners must be
   clamped. `SolveFillet` already does this; confirm its behaviour is what is wanted here
   rather than adding a second clamp.

---

## How to work in this repo

From `C:\repos\AirportMgr2\CLAUDE.md`, and all of it bit during this session:

- **Test first.** "Write the failing automation test that reproduces the report first, then
  fix until it passes." Watch it fail for the RIGHT REASON before fixing.
- **Build:**
  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
    -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
  ```
- **Test:** `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build` (full run is ~348 tests).
- **The editor must be CLOSED to build.** Live Coding holds the DLLs.
- **Never trust the runner's exit code.** Read its `N test(s) run, N failed, N crashed` line.
- `Check-Architecture.ps1` runs first inside the test script and fails the run before the
  editor starts.
- Comments in this codebase explain WHY and record the measurement that settled it. Match
  that; it is the house style and it is load-bearing.

### Suggested first test

Build a stand, lay its service loop, walk the resulting polyline through
`GuidelineGeom::VertexHeadings`, and assert:

1. **no vertex reports a heading change** (every corner is now a sampled curve), and
2. the **minimum radius clears** `ResolveDefaultVehicle()`'s `Wheelbase / sin(lock)`.

`Airside.Build.ServiceLoopEnclosesTheStand`, `ServiceLoopClearsTheAircraft` and
`DepotJoinsRoad` already build layouts close to this — start from one of them.
`Airside.Model.ServiceRoadFilletClearsTheTruckLock` (added this session) is a worked example
of pinning a geometry figure against the vehicle's steering.

---

## Already done this session — do not redo

**The 418 uu corner (a separate defect, now fixed).** The service road's junction fillet was
authored at 500 uu while the truck needed 510 uu — ten centimetres short, and 418 once
`RoadNetworkSolver` scaled it to fit the junction. Fixed on both sides because they are one
decision:

| | was | now |
|---|---|---|
| `Van.Ground.MaxSteerDegrees` (`AirsideSettings.cpp`) | 45° | **50°** |
| `MakeServiceRoadTransient` fillet default (`RoadProfile.h`) | 500 uu | **750 uu** |
| `build_road_profiles.py FILLET_RADIUS` | 500.0 | **750.0** |
| `DA_RoadProfile_ServiceRoad` asset | 500.0 | **750.0** (regenerated and read back) |

Pinned by **`Airside.Model.ServiceRoadFilletClearsTheTruckLock`** (new file
`Plugins/Airside/Source/AirsideTests/Private/ServiceRoadFilletTest.cpp`), which fails if the
two figures ever drift apart again. Full suite after: **348 run, 0 failed, 0 crashed.**

To regenerate the profile assets (editor CLOSED; `create_asset` refuses to replace a loaded
asset, so delete the `.uasset` first — they are tracked in git):

```
UnrealEditor-Cmd.exe C:\repos\AirportMgr2\AirportMgr.uproject -run=pythonscript `
  -script=C:\repos\AirportMgr2\Tools\Python\build_road_profiles.py -unattended -nosplash -nopause
```
Python `unreal.log()` goes to the log file only — use `unreal.log_error()` to see output on
stdout.

**The truck asset itself is finished** and is not in scope here: `SK_FuelTruck1` imported to
`Content/Vehicles/FuelTruck1/`, 8 bones, 14 flat materials, wheels verified same-handed.
Source and exporter live in `C:\repos\AirportMgr2Models\fueltruck1`.

---

## Loose ends to tidy

- **A temporary log is still in the tree.** `SpeedProfile.cpp`, just before the early return
  in `Build`, marked `TEMPORARY, 2026-09-14`. It prints `Speed profile IN: ...` and exists
  to tell "Build was never reached" apart from "Build took the early return". Useful while
  this corner is being chased — **remove it when done.**
- **Uncommitted work** on branch `feature/ledger-and-fees`: `DA_RoadProfile_ServiceRoad.uasset`,
  `ABP_Plane2.uasset`, `DA_AirsideContent.uasset`, `M_Starter.umap`, and untracked
  `Content/Vehicles/`.
- `samples/vehicleCorners.png` and `samples/routeOntoStand.png` show the layout and the
  desired sweep (red line).
