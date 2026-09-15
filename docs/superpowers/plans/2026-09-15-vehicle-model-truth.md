# The Vehicle Model Tells The Truth — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `MinTaxiSpeed`'s four distinct meanings four distinct homes, make the steering law explicit, resize the fuel dispenser to a real 8.5 m truck, and derive the service road's corner radius from the largest vehicle admitted instead of typing it in four places.

**Architecture:** Four separable changes to `Model/`, `Content/` and `Profiles/` in the Airside plugin. Nothing here touches the guideline graph, the traffic arbiter or any presenter. Every new test is world-free (`NewObject`, no `UWorld`) except the mesh-measurement one, which already exists.

**Tech Stack:** UE 5.8.2 C++, `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, `Tools/Run-AirsideTests.ps1`, `Tools/Python/build_road_profiles.py`.

**Spec:** `docs/superpowers/specs/2026-09-15-vehicle-model-truth-design.md`

---

## Global Constraints

- **Engine:** UE 5.8.2 at `D:\Epic\UE_5.8`. Project at `C:\repos\AirportMgr2`.
- **The editor must be CLOSED to build.** Live Coding holds the DLLs. Tasks 1, 2 and 6 change headers (new enum, new `UPROPERTY`, new method) and need a full `Build.bat`. Tasks 3, 4 and 5 are function bodies only and can go through Live Coding.
- **A new test `.cpp` needs TWO builds.** The first reports `Result: Succeeded` without compiling it, because UBT has not yet regenerated the module's source list. Always build twice when adding a test file, and confirm the test appears in the run count.
- **Name every leaf test distinctly.** UE's automation tree drops a bare-named parent once a dotted child exists, and only the run count catches it. No new test may be a prefix of another.
- **Never trust the runner's exit code.** Read `N test(s) run, N failed, N crashed` from `Tools/Run-AirsideTests.ps1`.
- **Comments explain WHY**, and especially why an obvious alternative was rejected. This codebase is dense with that deliberately — match it. Record the measurement that settled a figure, not just the figure.
- **Tests restate arithmetic rather than sharing a helper with production code.** `FAirframe::TightestFollowableRadius`'s own comment gives the reason: a helper both sides called could be wrong in one place and agree with itself. Keep that habit for every new test here.
- **`Check-Architecture.ps1` runs first** inside the test script and fails the run before the editor starts. `Model/` must not include `Build/|Tool/|Present/|Entities/|Content/`. `Profiles/` has no restriction and may include `Content/`.
- Log categories: `LogAirside` and `LogAirsideTraffic` in the plugin, `LogRoadBuild` in the game module.

### Figures this plan depends on (all derived, none typed twice)

| Quantity | Value | From |
|---|---|---|
| Mesh scale | ×1.371 | 8.5 m / 6.2 m |
| `SteerAxleX` after resize | 494.5 uu | 360.7 × 1.371, re-measured off bones |
| `MaxSteerDegrees` after | 45° | was 50°, which existed only to fit a 500 uu fillet |
| Forward radius `L / sin δ` | 699.3 uu | 494.5 / sin 45° |
| Reverse radius `L / tan δ` | 494.5 uu | part 2 uses this; recorded here so it is not re-derived |
| `JunctionScalingMargin` | 1.25 | the figure `ServiceRoadFilletTest` already justifies |
| Derived service fillet | 874 uu | 699.3 × 1.25 |
| `Van.MaxLateralAccelUu` | 294 | 0.3 g |

**Correction against the spec:** the spec says margin 1.5 → 1050 uu. That was my invention. `ServiceRoadFilletTest.cpp:70` already argues 1.25 and says why ("covers the scaling seen in play with room to spare, without demanding a motorway sweep on an apron road"). This plan uses **1.25 → 874 uu** rather than introducing a second margin. Amend the spec to match.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `Public/Model/RoadEntity.h` | `FGroundPerformance::MinSteeringSpeed`, `ESteerLaw`, `FAirframe::SteerLaw`, `IsSet()` | 1, 2 |
| `Private/Model/SpeedProfile.cpp` | span caps, vertex caps, the untakeable warning | 1, 2, 3, 4 |
| `Private/Model/RouteFollower.cpp` | law switch, `CrabLimit` floor | 1, 2, 3 |
| `Public/Model/RouteFollower.h` | `ProgressEpsilon` | 3 |
| `Private/Model/TakeoffRun.cpp`, `LandingRun.cpp` | rename only | 1 |
| `Private/Content/AirsideSettings.cpp` | `ResolveDefaultVehicle`, new `ResolveLargestServiceVehicle` | 5, 6 |
| `Public/Content/AirsideSettings.h` | declaration of the above | 6 |
| `Public/Profiles/RoadProfile.h` | `ResolvedFilletRadius()`, sentinel default | 6 |
| `Private/Profiles/RoadProfile.cpp` | the derivation, in one place | 6 |
| `Private/Build/RoadNetworkSolver.cpp:73` | consume `ResolvedFilletRadius()` | 6 |
| `Private/Debug/RoadJunctionGallery.cpp:187` | consume `ResolvedFilletRadius()` | 6 |
| `Tools/Python/build_road_profiles.py` | passes the sentinel, holds no radius | 6 |
| `AirsideTests/Private/SteerLawTest.cpp` | **new** — the enum and its validation | 2 |
| `AirsideTests/Private/SteeringFloorTest.cpp` | **new** — the four floors, separately | 1, 3, 4 |

---

## Task 1: `MinSteeringSpeed` — the rename, and the landmine

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h:371` (the field), `:426` (`IsSet`), `:355`, `:659` (comments)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/SpeedProfile.cpp:99,113,120,121,140,181,202`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RouteFollower.cpp:177,180`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/TakeoffRun.cpp:94,96,133`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/LandingRun.cpp:237`
- Modify: `Plugins/Airside/Source/Airside/Private/Content/AirsideSettings.cpp:102`
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/AircraftType.cpp:118,163,284`
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RouteFollower.h:71`, `Public/Model/SpeedProfile.h:82`, `Public/Model/TakeoffRun.h:40,81`
- Modify (tests, rename only): `LandingRunTest.cpp:200`, `TakeoffRunTest.cpp:70`, `TurnRateTest.cpp:319,320,333,440,457,538,586`, `NoseGearTracksTest.cpp:293,301`
- Create: `Plugins/Airside/Source/AirsideTests/Private/SteeringFloorTest.cpp`

**Interfaces:**
- Produces: `FGroundPerformance::MinSteeringSpeed` (double, uu/s) — every later task reads this name.
- Produces: `FGroundPerformance::IsSet()` no longer requires a non-zero steering minimum.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/SteeringFloorTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSteeringFloorZeroStillTaxisTest,
    "Airside.Model.SteeringFloorZeroStillTaxis",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSteeringFloorZeroStillTaxisTest::RunTest(const FString& Parameters)
{
    // THE LANDMINE THIS WHOLE SPLIT WALKS PAST. FGroundPerformance::IsSet() used to require
    // MinTaxiSpeed > 0, back when that number meant "an aircraft cannot yaw without
    // rolling". A ground vehicle has no such limit - a truck really can stop mid-turn,
    // because nothing is redirecting thrust along a body axis - so the van is authored at
    // zero. Under the old clause that made the van answer IsSet() == false and FREEZE:
    // ArrivalPlanner and FRoadAgent both branch on exactly that call.
    //
    // Nothing else in the suite would have caught it. The van would simply never have moved,
    // which reads as a routing bug and would have been chased as one.
    FAirframe Van = UAirsideSettings::ResolveDefaultVehicle();
    Van.Ground.MinSteeringSpeed = 0.0;

    TestTrue(
        TEXT("a vehicle that can stop mid-turn still has usable ground performance"),
        Van.Ground.IsSet());

    return true;
}

#endif
```

- [ ] **Step 2: Run it and watch it fail for the right reason**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.SteeringFloorZeroStillTaxis
```

Expected on the FIRST build: a compile error, `MinSteeringSpeed` is not a member. That is the correct failure — the field does not exist yet. Remember the two-builds rule for a new test file.

- [ ] **Step 3: Rename the field and drop the clause**

In `RoadEntity.h`, rename the `UPROPERTY` at `:371` and rewrite its doc comment to state concept #1 only:

```cpp
	/**
	 * The slowest this type can be kept rolling WHILE STEERING - NOT zero for an aircraft,
	 * and ZERO for anything that can stop mid-turn.
	 *
	 * A wheeled aircraft cannot yaw without rolling: a prop or a fan produces thrust along
	 * the airframe, and a nosewheel steers the direction that thrust is taken in. It has no
	 * way to pivot on the spot. So a turn that cannot be made at speed is made at a crawl,
	 * which is what a pilot riding the brakes against idle thrust actually does.
	 *
	 * A GROUND VEHICLE SETS THIS TO ZERO and means it. A truck's wheels are driven and
	 * steered independently of any thrust line; it can stop with the wheel turned and pull
	 * away again. It was 50 uu/s until 2026-09-15 for a reason that had stopped being true -
	 * "a follower allowed to stop dead mid-turn would snap its heading round" - which is a
	 * PIVOT-law artefact. Under the rolling-steer law MaxStep is proportional to speed
	 * (RouteFollower.cpp), so at zero the heading cannot move at all, let alone snap.
	 *
	 * WAS CALLED MinTaxiSpeed, and was four things at once: this, a solver guard against the
	 * crab loop deadlocking, and the punishment for a corner tighter than the steering lock.
	 * The other two now live where they belong - FRouteFollower::ProgressEpsilon and a
	 * warning in FSpeedProfile respectively.
	 *
	 * Not a floor on speed in general: an aircraft parked at its destination is stopped.
	 * This bounds only what a TURN may slow it to.
	 */
	UPROPERTY(EditAnywhere) double MinSteeringSpeed = 50.0;
```

And at `:426`:

```cpp
	/** False when nothing was authored, so a caller can fall back rather than freeze an agent. */
	bool IsSet() const
	{
		// Landing is NOT required here. This answers "can this thing move about an airport",
		// which every agent needs; an arrival additionally checks Landing.IsSet() for itself,
		// so an airframe with no landing figures declines to land rather than failing to taxi.
		//
		// MinSteeringSpeed IS NOT CHECKED, since 2026-09-15. Zero is a legitimate authored
		// value - it is what every ground vehicle says - and requiring it non-zero froze the
		// van outright. See Airside.Model.SteeringFloorZeroStillTaxis.
		return Taxi.IsSet() && MaxTurnRateDegPerSec > 0.0;
	}
```

Then rename every reader listed under **Files**. This is mechanical; no behaviour changes in this task beyond `IsSet`.

- [ ] **Step 4: Run the new test and the whole suite**

```
./Tools/Run-AirsideTests.ps1
```

Expected: the new test passes. Every other test still passes — this task is a rename plus one relaxed precondition, and nothing yet authors a zero.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor(model): MinTaxiSpeed says which of its four jobs it is doing"
```

---

## Task 2: The steering law becomes an enum

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h:643-652` (`Wheelbase`, `HasAxles`), `:671-676` (`TightestFollowableRadius`), and a new enum above `FGroundPerformance`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RouteFollower.cpp:124,161`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/SpeedProfile.cpp:90`
- Modify: `Plugins/Airside/Source/Airside/Private/Content/AirsideSettings.cpp` (van declares `RollingSteer`)
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/AircraftType.cpp` (each type declares its law)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/AirframeAxlesTest.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/SteerLawTest.cpp`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `enum class ESteerLaw : uint8 { Pivot, RollingSteer }`; `FAirframe::SteerLaw` (UPROPERTY); `FAirframe::EffectiveSteerLaw() const` returning the law actually usable given the data.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/SteerLawTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSteerLawWithoutAxlesFallsBackTest,
    "Airside.Model.SteerLawWithoutAxlesFallsBack",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSteerLawWithoutAxlesFallsBackTest::RunTest(const FString& Parameters)
{
    // THE BUG THIS ENUM EXISTS TO MAKE IMPOSSIBLE. The law used to be INFERRED from whether
    // anyone had measured the axles - HasAxles() was the selector - so forgetting to measure
    // a vehicle silently swapped which physical law governed it. Reported from play
    // 2026-09-14: "the truck drives up to the stand, stops, swings 90 degrees on the spot and
    // drives off." That is the pivot law, running on something nobody meant to pivot.
    //
    // Declaring the law and measuring the axles are now two statements that must agree, and
    // the consumer checks rather than assumes - CLAUDE.md's "lists that must agree are ONE
    // list", where UE forces two.
    FAirframe Unmeasured;
    Unmeasured.SteerLaw = ESteerLaw::RollingSteer;
    Unmeasured.SteerAxleX = 0.0;
    Unmeasured.FixedAxleX = 0.0;

    TestEqual(
        TEXT("an airframe claiming to steer geometrically with no wheelbase falls back to pivot"),
        Unmeasured.EffectiveSteerLaw(), ESteerLaw::Pivot);

    FAirframe Measured;
    Measured.SteerLaw = ESteerLaw::RollingSteer;
    Measured.SteerAxleX = 360.0;
    Measured.FixedAxleX = 0.0;

    TestEqual(
        TEXT("and one with real axles keeps the law it declared"),
        Measured.EffectiveSteerLaw(), ESteerLaw::RollingSteer);

    // A PIVOT AIRFRAME IS NOT PROMOTED BY ACCIDENT. Measured axles on something declared
    // Pivot must stay Pivot - otherwise the enum is decoration and HasAxles() is still the
    // selector, which is the arrangement this replaces.
    FAirframe DeliberatePivot;
    DeliberatePivot.SteerLaw = ESteerLaw::Pivot;
    DeliberatePivot.SteerAxleX = 360.0;
    DeliberatePivot.FixedAxleX = 0.0;

    TestEqual(
        TEXT("measured axles do not promote an airframe that declared itself a pivot"),
        DeliberatePivot.EffectiveSteerLaw(), ESteerLaw::Pivot);

    return true;
}

#endif
```

- [ ] **Step 2: Run it and watch it fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.SteerLawWithoutAxlesFallsBack
```

Expected: compile error — `ESteerLaw` undeclared. Two builds for the new file.

- [ ] **Step 3: Add the enum and the accessor**

In `RoadEntity.h`, above `FGroundPerformance`. It MUST live in a header with a `.generated.h` — a plain enum is invisible to UHT and a forward declaration does not help:

```cpp
/**
 * Which law moves an airframe on the ground.
 *
 * AN ENUM AND NOT A DERIVED BOOL, since 2026-09-15. It used to be inferred from
 * FAirframe::HasAxles() - "has someone measured the wheelbase?" - which meant forgetting to
 * measure a vehicle silently changed its physics. That is not hypothetical: it is the
 * 2026-09-14 report of a fuel truck stopping at a stand and swinging ninety degrees on the
 * spot. An illegal state that cannot be represented cannot be shipped.
 */
UENUM()
enum class ESteerLaw : uint8
{
	/**
	 * A flat yaw rate, MaxTurnRateDegPerSec, independent of speed. What a tug, a belt loader
	 * or anything else that turns about itself actually does.
	 */
	Pivot,

	/**
	 * The kinematic bicycle model: yaw is v*sin(lock)/L, so it vanishes at a standstill and
	 * a corner tighter than L/sin(lock) cannot be followed at any speed.
	 */
	RollingSteer
};
```

On `FAirframe`, beside `SteerAxleX`:

```cpp
	/**
	 * Which law moves this airframe - DECLARED, not inferred from whether the axles happen
	 * to be filled in. See ESteerLaw.
	 *
	 * Pivot is the default because it is the one that needs no measurements: an airframe
	 * nobody has measured is exactly the one that must not claim to steer geometrically.
	 */
	UPROPERTY(EditAnywhere) ESteerLaw SteerLaw = ESteerLaw::Pivot;
```

Replace `HasAxles()`'s role. Keep the function — it is now a data check, not a selector:

```cpp
	/** True when the axle figures can actually support the rolling-steer law. */
	bool HasAxles() const { return Wheelbase() > KINDA_SMALL_NUMBER; }

	/**
	 * The law this airframe will actually be moved by, which is the declared one UNLESS the
	 * data cannot support it.
	 *
	 * TWO STATEMENTS THAT MUST AGREE, and UE gives us no way to make them one: the law is a
	 * UPROPERTY and the wheelbase is two more. So the consumer checks identity rather than
	 * trusting either - CLAUDE.md's rule where a second list is unavoidable. A RollingSteer
	 * airframe with no wheelbase would divide by zero in TightestFollowableRadius and yaw
	 * infinitely in FRouteFollower; falling back to Pivot is wrong but survivable, and the
	 * log is what gets it fixed.
	 */
	ESteerLaw EffectiveSteerLaw() const
	{
		if (SteerLaw == ESteerLaw::RollingSteer && !HasAxles())
		{
			UE_LOG(LogAirside, Error,
				TEXT("Airframe '%s' declares RollingSteer with no wheelbase (steer axle %.1f, "
				     "fixed axle %.1f). Falling back to the pivot law - it will turn about "
				     "itself rather than steer."),
				*Code.ToString(), SteerAxleX, FixedAxleX);
			return ESteerLaw::Pivot;
		}
		return SteerLaw;
	}
```

Then rewrite `TightestFollowableRadius` to gate on the law rather than on `HasAxles`:

```cpp
	double TightestFollowableRadius() const
	{
		const double Lock = FMath::Sin(FMath::DegreesToRadians(
			FMath::Clamp(Ground.MaxSteerDegrees, 0.0, 90.0)));
		return (EffectiveSteerLaw() == ESteerLaw::RollingSteer && Lock > KINDA_SMALL_NUMBER)
			? Wheelbase() / Lock : 0.0;
	}
```

Switch the three consumers: `RouteFollower.cpp:124` and `:161` and `SpeedProfile.cpp:90` all become
`Airframe.EffectiveSteerLaw() == ESteerLaw::RollingSteer`.

Author the law on every type that has axles today: the van in `AirsideSettings.cpp`, and each measured type in `AircraftType.cpp`. Anything left unmeasured keeps the `Pivot` default and now says so out loud.

- [ ] **Step 4: Run the suite**

```
./Tools/Run-AirsideTests.ps1
```

Expected: the new test passes. `AirframeAxlesTest.cpp` needs updating — it asserts `HasAxles()` as the law selector throughout, which is exactly what stopped being true. Rewrite its assertions in terms of `EffectiveSteerLaw()`, keeping each test's existing *reason* string.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "refactor(model): the steering law is declared, not inferred from a measurement"
```

---

## Task 3: The two floors that are solver guards, not physics

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RouteFollower.h` (new constant)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RouteFollower.cpp:180`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/SpeedProfile.cpp:140`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/SteeringFloorTest.cpp`

**Interfaces:**
- Consumes: `FGroundPerformance::MinSteeringSpeed` from Task 1.
- Produces: `FRouteFollower::ProgressEpsilon` (double, uu/s).

**Why this task exists, and what the spec missed.** The spec named `RouteFollower.cpp:180` as the only deadlock. There is a second, and it is worse: `SpeedProfile.cpp:140` sets a **sharp vertex**'s limit to the floor. With a van at zero that limit becomes `0`, the backward pass brakes the agent to a standstill at the vertex, `LimitAt` keeps returning zero, and the truck stops on the apron **permanently**. Both floors need the same treatment, so they are one task.

- [ ] **Step 1: Write the failing tests**

Append to `SteeringFloorTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSteeringFloorSharpVertexStillCreepsTest,
    "Airside.Model.SteeringFloorSharpVertexStillCreeps",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSteeringFloorSharpVertexStillCreepsTest::RunTest(const FString& Parameters)
{
    // A VEHICLE MUST NEVER BE PLANNED TO A DEAD STOP MID-ROUTE. A vertex whose heading
    // changes instantly cannot be taken at any speed, and the profile has always answered
    // that with "the slowest this thing can still steer at". For an aircraft that is a real
    // figure. For a van it is now ZERO - and zero here is not a crawl, it is a permanent
    // stop: the backward pass brakes the agent to rest at the vertex and LimitAt keeps
    // returning zero for ever after.
    //
    // So the floor at a sharp vertex is the GREATER of the physical minimum and the solver's
    // progress epsilon. The aircraft keeps its physics; the van creeps.
    FAirframe Van = UAirsideSettings::ResolveDefaultVehicle();
    Van.Ground.MinSteeringSpeed = 0.0;

    // A right-angle vertex at 1000 uu: due east, then due north. Nothing samples the corner,
    // so the heading changes instantly and FSpeedProfile calls it untakeable.
    const TArray<FVector2D> Corner = {
        FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0), FVector2D(1000.0, 1000.0) };

    FSpeedProfile Profile;
    Profile.Build(Corner, Van);

    TestTrue(
        TEXT("a van planned through a sharp vertex is still asked to move, not to stop dead"),
        Profile.LimitAt(1000.0) >= FRouteFollower::ProgressEpsilon - KINDA_SMALL_NUMBER);

    return true;
}
```

Check `FSpeedProfile::Build`'s actual signature before writing this — if it takes distances and headings rather than raw points, build them the way `NoseGearTracksTest.cpp` does and copy that setup rather than inventing one.

- [ ] **Step 2: Run and watch it fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.SteeringFloorSharpVertexStillCreeps
```

Expected: FAIL, reporting a limit of 0 at the vertex. That is the deadlock, reproduced.

- [ ] **Step 3: Add the epsilon and apply it at both floors**

In `RouteFollower.h`, beside `CrabAtMinSpeedDegrees`:

```cpp
	/**
	 * The slowest a PLAN may ask for, uu/s. 0.1 m/s - small enough never to be seen.
	 *
	 * A SOLVER GUARD, NOT A PERFORMANCE FIGURE, and that is why it is here and not on
	 * FGroundPerformance. Two loops in this model divide by their own progress: if v = 0 and
	 * the heading error exceeds the lock, MaxStep is zero, the heading never moves, the crab
	 * never shrinks, and the agent sits at zero for ever with an error it cannot resolve.
	 * The same applies to a sharp vertex, where the plan itself asks for a stop.
	 *
	 * It used to be FGroundPerformance::MinTaxiSpeed doing this by accident, at 0.5 m/s -
	 * five times larger than it needs to be, authored per vehicle as though it were physics,
	 * and indistinguishable in the logs from the three other rules reporting the same number.
	 */
	static constexpr double ProgressEpsilon = 10.0;
```

At `RouteFollower.cpp:180`, floor at the greater of the two — the aircraft keeps its physical
minimum, the van falls through to the epsilon:

```cpp
	const double CrabLimit = FMath::Max3(
		Ground.MinSteeringSpeed, ProgressEpsilon, Ground.Taxi.SpeedCap * Slowing);
```

At `SpeedProfile.cpp:140`, the same:

```cpp
			Limit = FMath::Max(Ground.MinSteeringSpeed, FRouteFollower::ProgressEpsilon);
```

Leave `SpeedProfile.cpp:121` (the span floor) alone apart from the rename. It is concept #1 and
it is correct: an aircraft genuinely cannot take a turn below its steering minimum. It simply
stops binding for ground vehicles once the van's figure is zero. Removing it would break the
aircraft to fix the truck.

Leave `VertexLimits[Count - 1] = 0.0` alone too — arriving and stopping is not a deadlock, and
its comment already says why it is exempt.

- [ ] **Step 4: Run the suite**

```
./Tools/Run-AirsideTests.ps1
```

Expected: both new tests pass, everything else unchanged. Nothing authors a zero yet, so no existing figure moves.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "fix(model): a plan never asks an agent to stop where it must keep steering"
```

---

## Task 4: An untakeable corner reports itself instead of crawling

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Model/SpeedProfile.cpp:95-101`, and the `UE_LOG` at `:196-203`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/NoseGearTracksTest.cpp:293,301`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/TurnRateTest.cpp:538`

**Interfaces:**
- Consumes: `MinSteeringSpeed` (Task 1), `EffectiveSteerLaw` (Task 2).
- Produces: no new symbols. Behaviour change only.

- [ ] **Step 1: Write the failing test**

Append to `SteeringFloorTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FSteeringFloorTightCornerUsesLateralAccelTest,
    "Airside.Model.SteeringFloorTightCornerUsesLateralAccel",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSteeringFloorTightCornerUsesLateralAccelTest::RunTest(const FString& Parameters)
{
    // WHY EVERYTHING CRAWLED. FSpeedProfile computes the correct lateral-accel speed for a
    // corner - sqrt(a * R), tyre side load, the figure that actually governs - and then threw
    // it away whenever the radius fell below the steering lock, substituting the floor. Three
    // rules reported the same number and the inspector could not tell them apart; that is
    // SpeedProfile.cpp's own comment, written before the number was split.
    //
    // A corner AT the lock's own limit is takeable. It must run at sqrt(a * R), not at a
    // creep - for the van that is the difference between 1.8 km/h and about 16.
    const FAirframe Van = UAirsideSettings::ResolveDefaultVehicle();

    const double R = Van.TightestFollowableRadius();
    if (!TestTrue(TEXT("the default vehicle steers geometrically, or there is no limit to test"),
            R > KINDA_SMALL_NUMBER))
    {
        return false;
    }

    // The arithmetic restated rather than shared - see the note in TightestFollowableRadius
    // about a helper that could be wrong in one place and agree with itself.
    const double Expected = FMath::Sqrt(Van.Ground.MaxLateralAccelUu * R);

    TestTrue(
        FString::Printf(
            TEXT("a corner at the tightest followable radius runs at the lateral-accel speed "
                 "%.0f uu/s, not the steering floor %.0f"),
            Expected, Van.Ground.MinSteeringSpeed),
        Expected > Van.Ground.MinSteeringSpeed * 2.0);

    // Build an arc of exactly that radius and confirm the profile agrees. Sample it finely
    // enough that no vertex reads as sharp - a coarse arc would be caught by the VERTEX rule
    // instead and would pass this test for the wrong reason.
    TArray<FVector2D> Arc;
    constexpr int32 Steps = 64;
    for (int32 At = 0; At <= Steps; ++At)
    {
        const double Theta = (PI * 0.5) * At / Steps;
        Arc.Add(FVector2D(R * FMath::Sin(Theta), R * (1.0 - FMath::Cos(Theta))));
    }

    FSpeedProfile Profile;
    Profile.Build(Arc, Van);

    const double Mid = Profile.LimitAt(R * PI * 0.25);
    TestTrue(
        FString::Printf(TEXT("and the built profile allows %.0f uu/s mid-arc, above the floor"), Mid),
        Mid > Van.Ground.MinSteeringSpeed * 2.0);

    return true;
}
```

- [ ] **Step 2: Run and watch it fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.SteeringFloorTightCornerUsesLateralAccel
```

Expected: FAIL at the mid-arc assertion, reporting the floor. The first assertion (pure arithmetic) should already pass — if it does not, `MaxLateralAccelUu` is wrong, not the profile.

- [ ] **Step 3: Stop substituting the floor; warn instead**

Replace `SpeedProfile.cpp:95-101`:

```cpp
			else if (Radius < TightestFollowable)
			{
				// THE LOCK CANNOT HOLD THIS LINE AT ANY SPEED, so no speed is the right
				// answer and slowing down does not make it one - the body will crab through
				// regardless. This used to substitute MinTaxiSpeed, which looked exactly like
				// an intended crawl and was diagnosed as one for three sessions.
				//
				// So: the lateral-accel speed for the radius actually asked for, which is
				// lower than the lock's own and honest about why, plus a warning naming the
				// figure that refused it. A corner this tight is a defect in whatever laid
				// the line, and the log is what finds it.
				Cap = FMath::Min(Cap, FMath::Sqrt(
					FMath::Max(0.0, Ground.MaxLateralAccelUu) * Radius));
				Rule = TEXT("TIGHTER THAN THE STEERING LOCK");

				UE_LOG(LogAirsideTraffic, Warning,
					TEXT("Route asks for R=%.0f uu at %.0f, but the steering lock allows only "
					     "R>=%.0f (wheelbase %.0f, lock %.0f deg). The body will crab through "
					     "it. Widen the corner that laid this line."),
					Radius, Distances[Span], TightestFollowable,
					Airframe.Wheelbase(), Ground.MaxSteerDegrees);
			}
```

In the summary `UE_LOG` at `:196`, rename the `Floor %.0f` argument to report `MinSteeringSpeed` under a name that says which floor it is — `steering floor %.0f` — so the line stops implying one number governs everything.

- [ ] **Step 4: Run the suite and fix the two tests that pinned the old rule**

```
./Tools/Run-AirsideTests.ps1
```

Expected failures, both deliberate:
- `NoseGearTracksTest.cpp:293` — `Limit <= MinTaxiSpeed + 1.0` asserted the old substitution. Re-express it as "below the lock, the cap is the lateral-accel speed for the radius asked for", keeping its existing reason string and adding why it changed.
- `TurnRateTest.cpp:538` — `Coarse.FinalSpeed <= Piper.MinTaxiSpeed`, same cause, same treatment.

Do NOT relax either into a weaker assertion. Each should still pin a specific number; the number is simply now a different one.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "fix(model): a corner too tight for the lock says so instead of crawling"
```

---

## Task 5: The dispenser becomes a truck

**Files:**
- Modify: `C:\repos\AirportMgr2Models\fueltruck1` export scale, then re-import `Content/Vehicles/FuelTruck1/SK_FuelTruck1`
- Modify: `Plugins/Airside/Source/Airside/Private/Content/AirsideSettings.cpp:113-155`
- Verify: `Airside.Content.VehicleFootprintMatchesTheMesh`

**Interfaces:**
- Consumes: `ESteerLaw` (Task 2).
- Produces: a `ResolveDefaultVehicle()` whose `TightestFollowableRadius()` is ~699 uu. Task 6 reads it.

**This task has a manual step.** The mesh re-export happens in the modelling repo and the import happens in the editor. The figures below are *predictions* until the bones are re-measured — take the measured values, not these, if they differ.

- [ ] **Step 1: Re-export and re-import at 8.5 m**

Scale `fueltruck1` by ×1.371 (8.5 / 6.2) about its existing origin — which its README fixes at "rear axle centre, projected to the ground, because that is what a front-steered truck pivots about". Re-import over `Content/Vehicles/FuelTruck1/SK_FuelTruck1`.

- [ ] **Step 2: Read the new bone positions and watch the existing test fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Content.VehicleFootprintMatchesTheMesh
```

Expected: FAIL, reporting the new `steer_FL/FR` X against the authored 360.7. **Record the reported number** — that is `SteerAxleX`, and it is measured rather than the 494.5 predicted above.

- [ ] **Step 3: Author the new figures**

In `AirsideSettings.cpp`, replacing the axle and lock blocks. Keep every existing WHY paragraph and add the new decision beneath it — a refactor that drops a justification is how a rule nobody knows about gets reintroduced:

```cpp
	// RESIZED 2026-09-15 to 8.5 m, the smallest real hydrant dispenser. It was 6.2 m with a
	// 3.607 m wheelbase, which is a Ford Transit: an 11.2 m kerb-to-kerb circle, parked
	// beside a 737. Larger classes follow; this is the SMALLEST the fleet will carry, which
	// is why the road it turns on is sized from ResolveLargestServiceVehicle and not from
	// this.
	Van.SteerAxleX = 494.5;   // measured off the re-imported bones - see Step 2
	Van.FixedAxleX = 0.0;
	Van.SteerLaw = ESteerLaw::RollingSteer;

	// BACK TO 45 FROM 50. The 50 was not a measurement - it was raised on 2026-09-14 to make
	// a 500 uu authored fillet clear the truck's lock, which is the tail wagging the dog:
	// shrinking a vehicle's turning circle to fit a road, where the rule everywhere else on
	// this airport is to size ground geometry for the largest thing admitted. The fillet is
	// now derived from the vehicle instead (URoadProfile::ResolvedFilletRadius), so the lock
	// can go back to what a rigid truck actually has.
	//
	// 45 degrees on a 4.945 m wheelbase is a 6.99 m front-axle radius, so a kerb-to-kerb
	// circle near 16.2 m - correct for a rigid 8.5 m truck, where the old figures gave 11.2.
	Van.Ground.MaxSteerDegrees = 45.0;

	// 0.3 g. AUTHORED RATHER THAN INHERITED, which it was until 2026-09-15: the struct
	// default is 147 uu/s^2, an AIRCRAFT CABIN comfort figure, and nothing here ever chose
	// it. A van on dry concrete does 0.3 g without drama, and this is what decides corner
	// speed once steering is geometric - at the 699 uu the lock allows it is the difference
	// between 11.5 km/h and 16.
	//
	// The comment above about writing every figure out rather than inheriting it applies to
	// this one too; it was simply missed.
	Van.Ground.MaxLateralAccelUu = 294.0;

	// A TRUCK CAN STOP MID-TURN, so zero, and the reason is in
	// FGroundPerformance::MinSteeringSpeed: nothing here is redirecting thrust along a body
	// axis. It was 50 for a rolling-steer artefact that never existed.
	Van.Ground.MinSteeringSpeed = 0.0;
```

- [ ] **Step 4: Run the suite**

```
./Tools/Run-AirsideTests.ps1
```

Expected: `VehicleFootprintMatchesTheMesh` passes again. `ServiceLinkTest.cpp:541,702,802,1048` and `ServiceRoadFilletTest.cpp` will fail — the van's radii have moved and the 750 uu fillet no longer clears them with margin. **That is the correct failure and Task 6 fixes it.** Do not patch the numbers here.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(content): the fuel dispenser is a truck, not a Transit"
```

---

## Task 6: The fillet is derived from the largest vehicle admitted

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Content/AirsideSettings.h` (declare `ResolveLargestServiceVehicle`)
- Modify: `Plugins/Airside/Source/Airside/Private/Content/AirsideSettings.cpp` (define it)
- Modify: `Plugins/Airside/Source/Airside/Public/Profiles/RoadProfile.h:124,241` (sentinel default, `ResolvedFilletRadius`)
- Modify: `Plugins/Airside/Source/Airside/Private/Profiles/RoadProfile.cpp:92,157`
- Modify: `Plugins/Airside/Source/Airside/Private/Build/RoadNetworkSolver.cpp:73`
- Modify: `Plugins/Airside/Source/Airside/Private/Debug/RoadJunctionGallery.cpp:187`
- Modify: `Tools/Python/build_road_profiles.py:36-49,105`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/ServiceRoadFilletTest.cpp`
- Regenerate: `Content/Profiles/DA_RoadProfile_ServiceRoad.uasset`

**Interfaces:**
- Consumes: `ResolveDefaultVehicle()` (Task 5), `TightestFollowableRadius()` (Task 2).
- Produces: `UAirsideSettings::ResolveLargestServiceVehicle()` → `FAirframe`; `URoadProfile::ResolvedFilletRadius() const` → `double`; `URoadProfile::JunctionScalingMargin` = 1.25.

- [ ] **Step 1: Rewrite the existing test to assert the derivation**

Replace the body of `ServiceRoadFilletTest.cpp` below its existing history comment — **keep that comment**, it is the record of the 500-vs-510 bug and the reason this test exists:

```cpp
	// WHAT CHANGED 2026-09-15. The two numbers above no longer exist as two numbers. The
	// service road's fillet is DERIVED from the largest vehicle admitted, the way a painted
	// taxi line is swept for the largest aircraft admitted (IcaoCode::RadiusForLetter) - so
	// there is nothing left to drift, and this test asserts the derivation holds rather than
	// that two transcriptions still match.
	//
	// It still measures rather than trusting: ResolvedFilletRadius could be wired to the
	// wrong resolver, or the margin dropped, and the arithmetic below would catch both.
	const FAirframe Largest = UAirsideSettings::ResolveLargestServiceVehicle();

	if (!TestEqual(TEXT("the largest service vehicle steers geometrically"),
			Largest.EffectiveSteerLaw(), ESteerLaw::RollingSteer))
	{
		return false;
	}
	const double Lock = FMath::Sin(FMath::DegreesToRadians(
		FMath::Clamp(Largest.Ground.MaxSteerDegrees, 0.0, 90.0)));
	if (!TestTrue(TEXT("and has a steering lock above zero"), Lock > KINDA_SMALL_NUMBER))
	{
		return false;
	}

	// The same expression FSpeedProfile::Build uses. Written out rather than shared because a
	// helper both sides called could be wrong in one place and agree with itself.
	const double TightestFollowable = Largest.Wheelbase() / Lock;

	const URoadProfile* Service = URoadProfile::MakeServiceRoadTransient();
	if (!TestNotNull(TEXT("the service road profile is buildable"), Service))
	{
		return false;
	}

	// HEADROOM, NOT MERE SUFFICIENCY - unchanged in reasoning from the original test. An
	// exact match would pass while sitting on the cliff edge, and RoadNetworkSolver scales
	// the preferred radius DOWN when a junction's arms cannot fit it, which is how 500 became
	// 418 on the reported route. 1.25 covers the scaling seen in play with room to spare,
	// without demanding a motorway sweep on an apron road.
	//
	// RESTATED rather than read from URoadProfile::JunctionScalingMargin, for the same reason
	// the radius is restated: a test that imported the constant would pass if someone set it
	// to 1.0.
	const double Required = TightestFollowable * 1.25;

	TestTrue(
		FString::Printf(
			TEXT("the service road's derived %.0f uu fillet clears the %.0f uu the largest "
			     "service vehicle's steering needs, with margin for the junction solver "
			     "scaling it down (wheelbase %.1f, lock %.1f deg, needs >= %.0f)"),
			Service->ResolvedFilletRadius(), TightestFollowable,
			Largest.Wheelbase(), Largest.Ground.MaxSteerDegrees, Required),
		Service->ResolvedFilletRadius() >= Required);

	// AND THE ASSET CARRIES NO NUMBER TO DRIFT. A stored radius would be stale the moment a
	// larger vehicle joined the fleet, which is precisely the failure this change removes.
	TestEqual(
		TEXT("the service road profile stores the derive sentinel rather than a radius"),
		Service->PreferredFilletRadius, 0.0);
```

- [ ] **Step 2: Run and watch it fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.ServiceRoadFilletClearsTheTruckLock
```

Expected: compile error — `ResolveLargestServiceVehicle` and `ResolvedFilletRadius` do not exist.

- [ ] **Step 3: Add the resolver**

In `AirsideSettings.h`, beside `ResolveDefaultVehicle`:

```cpp
	/**
	 * The biggest thing that may drive on a service road, which is what the road's corners
	 * are sized for.
	 *
	 * A SEPARATE FUNCTION FROM ResolveDefaultVehicle even though it returns the same airframe
	 * today, because the two answer different questions and will diverge the moment a second
	 * vehicle class exists: "what does a truck without a type look like" against "what must
	 * every service road be able to turn". Merging them would mean a new, larger dispenser
	 * silently widened nothing, or a small van silently narrowed every junction.
	 *
	 * THE RULE IS THE ONE AIRCRAFT GEOMETRY ALREADY USES. IcaoCode::RadiusForLetter sweeps a
	 * painted taxi line for the largest aircraft a stand admits, never for the one taxiing
	 * now. This is that, for vehicles. It was inverted on 2026-09-14 - a truck's steering
	 * lock was widened from 45 to 50 degrees so it would fit an authored 500 uu corner - and
	 * this function is what makes the inversion impossible to repeat.
	 */
	static FAirframe ResolveLargestServiceVehicle();
```

In `AirsideSettings.cpp`:

```cpp
FAirframe UAirsideSettings::ResolveLargestServiceVehicle()
{
	// ONE CLASS TODAY, and deliberately no taxonomy yet: EVehicleClass with a single member
	// would be a list nothing chooses from. When the second dispenser arrives this function
	// gains the comparison and every service road corner widens on the next rebuild, with no
	// other site to find.
	return ResolveDefaultVehicle();
}
```

- [ ] **Step 4: Add the sentinel and the one derivation site**

In `RoadProfile.h`, change the default and document the sentinel:

```cpp
	/**
	 * The corner radius a junction on this profile prefers, uu. ZERO MEANS DERIVE - see
	 * ResolvedFilletRadius, and never read this field directly.
	 *
	 * 1500 is a taxiway's, authored, because a taxiway's corner is swept for the largest
	 * AIRCRAFT admitted and that figure comes from IcaoCode, not from a vehicle.
	 */
	UPROPERTY(EditAnywhere) double PreferredFilletRadius = 1500.0;

	/** Headroom over the bare steering limit, because RoadNetworkSolver scales a preferred
	 *  radius DOWN to fit a junction's arms - which is how a 500 uu fillet became 418 on the
	 *  route that reported this in 2026-09-14. 1.25 covers the scaling seen in play without
	 *  demanding a motorway sweep on an apron road. */
	static constexpr double JunctionScalingMargin = 1.25;

	/**
	 * The radius a junction on this profile actually turns on: the authored one, or - when
	 * that is zero - one derived from the largest vehicle admitted.
	 *
	 * THE ONLY LEGAL READER OF PreferredFilletRadius. Read the field directly and a service
	 * road turns on nothing at all.
	 *
	 * The sentinel exists so the ASSET carries no number. A stored radius is stale the moment
	 * a larger vehicle joins the fleet, and the four places this figure used to be typed -
	 * this header, build_road_profiles.py, DA_RoadProfile_ServiceRoad, and the test holding
	 * two of them together - are exactly the arrangement that shipped a ten-centimetre
	 * shortfall nobody could see.
	 */
	double ResolvedFilletRadius() const;
```

In `RoadProfile.cpp`:

```cpp
double URoadProfile::ResolvedFilletRadius() const
{
	if (PreferredFilletRadius > 0.0)
	{
		return PreferredFilletRadius;
	}

	// ONE PLACE, which is the whole point - Content/ resolves every content default in
	// exactly one function, and this is that function for a vehicle-sized corner.
	const FAirframe Largest = UAirsideSettings::ResolveLargestServiceVehicle();
	return Largest.TightestFollowableRadius() * JunctionScalingMargin;
}
```

Add `#include "Content/AirsideSettings.h"` to `RoadProfile.cpp`. `Profiles/` carries no include restriction in `Check-Architecture.ps1` — only `Model/`, `Tool/` and `Build/` do — so this direction is legal. Confirm by running the script.

Change `MakeServiceRoadTransient`'s default parameter from `750.0` to `0.0` and rewrite the doc comment above it to describe the derivation rather than the 7.5 m figure. Switch `RoadNetworkSolver.cpp:73` and `RoadJunctionGallery.cpp:187` to call `ResolvedFilletRadius()`.

- [ ] **Step 5: Retire the Python constant**

In `build_road_profiles.py`, delete `FILLET_RADIUS` and its 13-line comment, and pass the sentinel at `:105`:

```python
# NO FILLET RADIUS HERE ANY MORE. It is derived in C++ from the largest service vehicle
# admitted - URoadProfile::ResolvedFilletRadius - so this script has nothing to state and
# nothing to drift from. It was 500 until 2026-09-14 and 750 after, while the figure it had
# to agree with lived in a different language in a different repository directory.
unreal.RoadProfile.fill_service_road(profile, LANE_WIDTH, KERB_WIDTH, 0.0)
```

Regenerate the asset with the editor CLOSED. `create_asset` refuses to replace a loaded asset, so delete the `.uasset` first — it is tracked in git:

```
UnrealEditor-Cmd.exe C:\repos\AirportMgr2\AirportMgr.uproject -run=pythonscript `
  -script=C:\repos\AirportMgr2\Tools\Python\build_road_profiles.py -unattended -nosplash -nopause
```

Python `unreal.log()` goes to the log file only — use `unreal.log_error()` to see output on stdout.

- [ ] **Step 6: Run the whole suite**

```
./Tools/Run-AirsideTests.ps1
```

Expected: `ServiceRoadFilletClearsTheTruckLock` passes against a derived ~874 uu. `ServiceLinkTest.cpp:541,702,802,1048` — which pinned distances against the old van radii and the old 750 uu corner — need their figures re-derived. Each has a comment explaining the number it asserts; update the number **and** the comment, and keep the reason.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(build): a service road's corner is sized for the largest vehicle admitted"
```

---

## Task 7: Remove the temporary log, and look at the map

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Model/SpeedProfile.cpp` (the `TEMPORARY, 2026-09-14` block)

- [ ] **Step 1: Remove the temporary instrumentation**

Delete the `Speed profile IN: ...` log marked `TEMPORARY, 2026-09-14`, just before the early return in `Build`. It existed to tell "Build was never reached" apart from "Build took the early return"; the Task 4 warning now answers the question it was standing in for.

- [ ] **Step 2: Full build and full suite**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Run-AirsideTests.ps1
```

Read the `N test(s) run, N failed, N crashed` line. The baseline before this work was **348 run, 0 failed, 0 crashed**; expect 348 plus the four tests added here.

- [ ] **Step 3: Look at the map — this is not optional**

A green suite is **not** evidence for this change. It alters how *aircraft* move, not just trucks: the lateral-accel branch governs everything declaring `RollingSteer`, and a graph change that passed 348 tests has previously still put kilometre-wide arcs across the apron.

In PIE, with `Saved/Logs/AirportMgr.log` open:

1. Taxi an aircraft from a stand to the runway. Quote its `Speed profile:` line. Confirm the tightest-radius rule reads `lateral accel`, not `TIGHTER THAN THE STEERING LOCK`.
2. Send a fuel truck from the depot to a stand. Quote its `Speed profile:` line and confirm the same.
3. Confirm **no** `Route asks for R=...` warnings appear for either. If one does, the line that produced it is the defect — record it for part 2 rather than widening anything to silence it.
4. `python Tools/Mcp.py shot out.png` after the truck reaches the stand, and check the service road corners still read as a road rather than a motorway sweep at 874 uu.

- [ ] **Step 4: Commit and open the PR**

```bash
git add -A
git commit -m "chore(model): retire the temporary speed-profile probe"
git push -u origin HEAD
gh pr create --fill
```

Fill the PR template's build line, test line, and — since this is partly a refactor — the log-line and comment-line deltas. `UE_LOG` count should be **up** by one (Task 4's warning) and down by one (Task 7's probe): net zero, and say so.

---

## Self-review notes

**Spec coverage.** All four spec sections map to tasks 1–6; the verification section maps to task 7. Two gaps found while planning and fixed here:

1. **A fifth `MinTaxiSpeed` consumer.** `SpeedProfile.cpp:140` sets a sharp vertex's limit to the floor. With the van at zero that plans a **permanent stop**, not a crawl — worse than the deadlock the spec did identify. Task 3 covers both.
2. **The margin.** The spec invented 1.5 → 1050 uu. `ServiceRoadFilletTest.cpp:70` already justifies 1.25. This plan uses 1.25 → 874 uu rather than adding a second margin with no argument behind it.

**Signature confirmed.** `SpeedProfile.h:50` — `void Build(const TArray<FVector2D>& Points, const FAirframe& Airframe)`. The test code in Tasks 3 and 4 matches it as written. `SteeringFloorTest.cpp` needs `#include "Model/SpeedProfile.h"` and `#include "Model/RouteFollower.h"` alongside the includes listed in Task 1.
