# The vehicle model tells the truth

**Date:** 2026-09-15
**Status:** design agreed, not implemented
**Sub-project 1 of 2.** Part 2 ("stands are served from player roads") has its own spec and
depends on this one landing first.

---

## The problem, in one line

Four rules report `MinTaxiSpeed`, the steering law is chosen by a derived bool, and the
vehicle named `FuelTruck` has a Ford Transit's turning circle.

Everything below was read out of the tree on 2026-09-15, not recalled. Line numbers are
against `04f1af9`.

---

## 1. `MinTaxiSpeed` is four concepts wearing one name

`SpeedProfile.cpp:181` already says so: *"THREE RULES ALL REPORT MinTaxiSpeed and the
inspector panel cannot tell them apart."* There are in fact four.

| # | The concept | Where it is read | Is it real? |
|---|---|---|---|
| 1 | A wheeled aircraft cannot yaw without rolling | `SpeedProfile.cpp:121`, `TakeoffRun.cpp:94-96,133`, `LandingRun.cpp:237` | **Yes** — physics, per type |
| 2 | A follower must not stop dead mid-turn and snap | `AirsideSettings.cpp:102` (the van's 50) | **No longer** — see below |
| 3 | The crab loop must not deadlock at zero | `RouteFollower.cpp:180` | **Yes** — but it is a solver guard, not a data sheet figure |
| 4 | The penalty for a corner tighter than the lock | `SpeedProfile.cpp:99` | **No** — it is a symptom report |

### Why #2 is dead

The van's floor is justified as *"a follower allowed to stop dead mid-turn would snap its
heading round."* Under the rolling-steer law it cannot:

```
RouteFollower.cpp:128   MaxStep = |Speed * sin(Steer) / Wheelbase| * dt
RoadGeom.cpp:41-45      SlewAngle(Current, Target, 0) == Current
```

`MaxStep` is proportional to speed, so at `v = 0` the heading is clamped to no change at
all. Snapping is a **pivot-law** artefact — there `MaxStep = MaxTurnRateDegPerSec * dt`,
independent of speed. The moment `fueltruck1` got measured axles, the stated reason for its
own floor stopped applying to it.

### Why #3 is real, and different

If `v = 0` and `|Error| > Lock`, then `MaxStep = 0`, the heading never moves, `Crab` stays
positive (`RouteFollower.cpp:161`), `CrabLimit` collapses to the floor, and with a floor of
zero the agent sits at zero forever with an error it can never resolve. That is a control-loop
deadlock in the follower. It belongs to the follower, one value for every agent, never
authored per vehicle.

### Decisions

- **Rename `FGroundPerformance::MinTaxiSpeed` to `MinSteeringSpeed`** and keep it as concept
  #1 only. It stays authored per aircraft type. `TakeoffRun.h:40` already argues that the
  runway line-up uses the same rule, and that stays true under the new name.
- **`Van.Ground.MinSteeringSpeed = 0`.** A truck genuinely can stop mid-turn. It has no
  thrust along a body axis that must be redirected.
- **New `FRouteFollower` constant, `ProgressEpsilon = 10.0` uu/s** (0.1 m/s), added *beside*
  the physical floor rather than replacing it:
  `CrabLimit = max(MinSteeringSpeed, ProgressEpsilon, SpeedCap * Slowing)`.
  An aircraft keeps its physical minimum; a van with `MinSteeringSpeed = 0` falls through to
  the epsilon and cannot deadlock. Solver-owned, never authored, small enough never to be
  seen.
- **`SpeedProfile.cpp:99` stops substituting the floor.** A corner tighter than the lock gets
  the lateral-accel speed for its *actual* radius, plus a `UE_LOG(LogAirside, Warning)`
  naming the radius, the position along the route, and the lock that refused it. The vehicle
  cannot follow that arc at any speed, so the speed choice barely matters — what matters is
  that it stops looking like an intended crawl.
- **`SpeedProfile.cpp:121` keeps its floor, renamed.** It is concept #1 and it is correct: an
  aircraft genuinely cannot take a turn below its steering minimum. It simply stops *binding*
  for ground vehicles, because the van's figure becomes 0. Removing the line instead would
  have broken the aircraft to fix the truck — the floor was never the problem, the shared
  number was.

### The landmine

`FGroundPerformance::IsSet()` (`RoadEntity.h:426`) currently reads:

```cpp
return Taxi.IsSet() && MinTaxiSpeed > 0.0 && MaxTurnRateDegPerSec > 0.0;
```

With `Van.MinSteeringSpeed = 0` the van answers `IsSet() == false` and **freezes** — both
`ArrivalPlanner` and `FRoadAgent` branch on exactly that call. The `> 0.0` clause must go.
This is the single most likely way to ship this change broken, so it gets its own test.

---

## 2. The steering law becomes an enum

`FAirframe::HasAxles()` (`RoadEntity.h:652`) is a derived bool standing in for a *kind*:

```cpp
bool HasAxles() const { return Wheelbase() > KINDA_SMALL_NUMBER; }
```

Forgetting to measure a vehicle's axles silently swaps which physical law governs it. That is
not hypothetical — it is precisely the 2026-09-14 report, *"the truck drives up to the stand,
stops, swings 90 degrees on the spot and drives off."* CLAUDE.md: *a phase is an enum, never
a set of bools.*

### Decisions

- **`enum class ESteerLaw : uint8 { Pivot, RollingSteer }`**, declared in `RoadEntity.h`
  beside `FGroundPerformance`. It must live in a header with a `.generated.h` — a plain enum
  is invisible to UHT, and a forward declaration does not help.
- **`FAirframe::SteerLaw`**, authored, replacing inference.
- **`HasAxles()` survives as a validation predicate, not a selector**: "does the data support
  the law this airframe declares?" A `RollingSteer` airframe with no wheelbase logs an error
  and falls back to `Pivot` rather than dividing by zero. Two things that must agree, with the
  consumer checking identity — CLAUDE.md's rule, and the reason the pair is worth keeping.

### Consumers that must all agree

`RouteFollower.cpp:124`, `RouteFollower.cpp:161`, `SpeedProfile.cpp:90`,
`FAirframe::TightestFollowableRadius` (`RoadEntity.h:675`). Every one switches on the enum.

---

## 3. The truck becomes a truck

`SK_FuelTruck1` is 6.2 m with a 3.607 m wheelbase — a van. Resize to 8.5 m, the smallest fuel
dispenser the game will carry. Larger classes arrive later; the *rule* below is what makes
that cheap.

| | now | after | why |
|---|---|---|---|
| Length | 6.2 m | 8.5 m | smallest real hydrant dispenser |
| Uniform scale | — | ×1.371 | 8.5 / 6.2 |
| `SteerAxleX` (wheelbase) | 360.7 uu | **494.5 uu** | 360.7 × 1.371; measured off the re-imported bones, not typed |
| `MaxSteerDegrees` | 50° | **45°** | 50 existed only to make a 500 uu fillet fit — see `AirsideSettings.cpp:144` |
| Forward radius `L/sin δ` | 471 uu | **699 uu** | what the guideline must clear |
| Reverse radius `L/tan δ` | 303 uu | **494 uu** | part 2's dock leg; tighter than forward, which is why drivers reverse into tight spots |
| Kerb-to-kerb circle | ~11.2 m | **~16.2 m** | correct for a rigid 8.5 m truck |
| `MaxLateralAccelUu` | 147 (inherited) | **294** (0.3 g) | see below |

### `MaxLateralAccelUu`

`Van` never authors it (`grep` finds it only in `AircraftType.cpp:311` and the struct default),
so it silently inherits 147 uu/s² — 0.15 g, an **aircraft cabin comfort** figure. A van on dry
concrete does 0.3 g without drama. At 0.3 g and R = 699 uu that is 453 uu/s ≈ **16 km/h** at
the tightest corner the truck can make, against 1.8 km/h today. That is the professional
driver.

`AirsideSettings.cpp` already argues every figure should be written out rather than inherited
("this is where a truck's performance is DECIDED"). This one was missed.

### The content step

Re-import at ×1.371 from `C:\repos\AirportMgr2Models\fueltruck1`, re-measure the bones, update
`SteerAxleX`. `Airside.Content.VehicleFootprintMatchesTheMesh` already asserts the figures
against the mesh's own bones, so drift between the two cannot survive a test run.

---

## 4. The fillet is derived, not typed

The service road's corner radius is hand-typed in four places today: `RoadProfile.h:241`,
`Tools/Python/build_road_profiles.py`, `DA_RoadProfile_ServiceRoad.uasset`, and
`ServiceRoadFilletTest.cpp` which pins two of them together. That pairing is why 2026-09-14
fixed a too-tight corner by **shrinking the truck's steering** — the inverse of the rule
already on record for aircraft geometry: *size infrastructure for the largest thing admitted,
never for the one using it now.*

With a 699 uu truck, today's 750 uu fillet clears by 1.07×, against the 1.6× it happens to
have now. `RoadNetworkSolver` scales a preferred radius **down** to fit a junction's arms —
that is what turned 500 into 418 and started this whole thread — so 1.07× will fail as soon as
two junctions sit close together.

### Decisions

- **`UAirsideSettings::ResolveLargestServiceVehicle()`** — one function, following the
  existing `Resolve*` pattern. Today it returns the resized dispenser; when a larger class
  arrives it returns that, and every service road corner widens on the next rebuild.
- **`URoadProfile::FilletRadius == 0` means "derive"**, resolved in exactly one place at build
  time from `ResolveLargestServiceVehicle().TightestFollowableRadius() * JunctionScalingMargin`.
  The `.uasset` then carries 0 and there is no number left to drift.
  *Rejected alternative:* keep the number in the asset and add a test asserting it equals the
  derivation. That keeps four sites and makes the test the only thing holding them together —
  which is the arrangement this change exists to remove. (Fall back to it only if a zero
  sentinel turns out to break the profile editor UI.)
- **`JunctionScalingMargin = 1.5`**, stated as headroom for `RoadNetworkSolver`'s down-scaling
  rather than as taste. 699 × 1.5 = **1050 uu**.
- **`Airside.Model.ServiceRoadFilletClearsTheTruckLock` becomes an assertion on the
  derivation** — that a laid junction's *solved* radius still clears the largest vehicle —
  rather than a check that two hand-typed constants match.

---

## Test fallout

Found by grep, not by running. Every one pins behaviour this change deliberately alters.

| File | Lines | Why it moves |
|---|---|---|
| `TurnRateTest.cpp` | 319-320, 333, 440, 457, 538, 586 | pins Piper speeds against `MinTaxiSpeed` |
| `NoseGearTracksTest.cpp` | 293, 301 | pins the untakeable-corner branch |
| `AirframeAxlesTest.cpp` | throughout | rewritten around `ESteerLaw` |
| `ServiceLinkTest.cpp` | 541, 702, 802, 1048 | van radii change |
| `ServiceRoadFilletTest.cpp` | 24, 42 | becomes the derivation assertion |
| `LandingRunTest.cpp` | 200 | rename only |
| `TakeoffRunTest.cpp` | 70 | rename only |

## New tests

Each is world-free in `Model/` or `Solve/` unless noted.

1. A corner at exactly `TightestFollowableRadius` runs at `sqrt(a·R)`, **not** the floor.
2. A corner tighter than the lock logs a warning and does **not** report the floor.
3. A van with `MinSteeringSpeed = 0` answers `IsSet() == true` and taxis. *(The landmine.)*
4. A follower stopped with `|Error| > Lock` still makes progress — the deadlock guard.
5. An airframe declaring `RollingSteer` with no wheelbase logs an error and falls back.
6. A solved service-road junction's radius clears `ResolveLargestServiceVehicle()` **after**
   `RoadNetworkSolver` has scaled it.
7. The re-imported mesh's bones match `SteerAxleX` *(existing, re-pinned)*.

## Build and verification

One round. The enum, the rename and the new UPROPERTY all need a full rebuild with the editor
closed, and the content step (mesh re-import, `build_road_profiles.py`) needs it closed too —
so they batch rather than costing two closes.

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Run-AirsideTests.ps1
```

Read the `N test(s) run, N failed, N crashed` line, never the exit code.

**Then look at the map.** This changes how *aircraft* move, not just trucks — the
lateral-accel branch applies to everything with a wheelbase. A green suite is not evidence
here; a Piper taxi and a fuel truck run in PIE are, with the `Speed profile:` log line quoted.

Remove the `TEMPORARY, 2026-09-14` log in `SpeedProfile.cpp` as part of this work — the
question it was added to answer is settled by the new warning.

---

## Out of scope — part 2

Deleting `UEntityDefinition::ServiceLoop` and `FServiceLoopBuild`; the road-to-stand-boundary
handover; the in-stand visibility graph; the dock-start pose and the reverse leg; reverse
kinematics in `FRouteFollower` (fixed axle on the line, steering inverted — `FPushbackRun` is
not reusable, it works precisely because the tug couples at the nose gear so the *steered*
axle still leads); UI feedback for a stand no road reaches.

---

## Unresolved questions

1. **`Van.MaxLateralAccelUu = 294` (0.3 g)** — agreed, or keep it more conservative?
2. **The zero sentinel for `FilletRadius`** — agreed as the way to collapse four sites to one,
   or would you rather keep a number in the asset?
3. **Do any aircraft `MinSteeringSpeed` *values* change**, or is it a rename only? The Piper
   and the airliner are at 50 and 80 today and nothing here argues they are wrong.
4. **The uncommitted service-loop corner work on `feature/lane-entrances`** — `AnchorLink.cpp`,
   `ServiceLoopBuild.cpp/.h`, `GuidelineGeom.cpp/.h`, `ServiceLinkTest.cpp`, plus the handover
   doc. Part 2 deletes most of what it touches. Commit it for the record, or drop it?
5. **Branch.** New `feature/vehicle-model-truth` off `main`, or continue on the current branch?
