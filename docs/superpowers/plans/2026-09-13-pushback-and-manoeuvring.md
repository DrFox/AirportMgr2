# Pushback and Manoeuvring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An aeroplane backs out of its stand and swings onto the taxiway before it taxis, instead of pirouetting on its nose wheel at the stop mark; and the airframe declares whether it can do that under its own power.

**Architecture:** A fourth phase struct, `FPushbackRun`, beside `FLandingRun` / `FRouteFollower` / `FTakeoffRun`, with `FRoadAgent::Advance` owning the switch — the State-pattern-over-an-enum that struct already documents. It walks a **prefix of the departure plan the planner already returns**, so there is one plan and one distance along it and the existing claim pass arbitrates a push with no new claim rule. `UGroundTraffic::DepartAgent` grants the whole push up front (pushback clearance) rather than letting arbitration stop it mid-manoeuvre, because a manoeuvring agent cannot replan.

**Tech Stack:** UE 5.8.2 C++. `Airside` plugin (`Model/`, `Build/`, `Entities/`, `Content/`), `AirportOps` plugin (`Model/`). Tests are `IMPLEMENT_SIMPLE_AUTOMATION_TEST` in `Plugins/Airside/Source/AirsideTests/Private/` and `Plugins/AirportOps/Source/AirportOpsTests/Private/`.

**Spec:** `docs/superpowers/specs/2026-09-13-pushback-and-manoeuvring-design.md`

## Global Constraints

- **Units are uu; 1 uu = 1 cm.** Headings are **radians** everywhere in `Model/` (`FRouteFollower::Heading`, `FEntityInstance::Heading`); only authored knobs and log lines use degrees.
- **`Model/` may not see `Entities/`.** `Check-Architecture.ps1` enforces it. Data crosses via `FAirframe`, filled by `UAircraftType::Airframe()`.
- **`Solve/` sees `CoreMinimal.h` only.** Nothing in this plan adds to `Solve/`.
- **One `DEFINE_LOG_CATEGORY_STATIC` per name per module** — unity build. Use the existing `LogAirsideTraffic` (Airside `Model/`), `LogAirside` (Airside elsewhere), `LogAirportOps` (AirportOps).
- **Every `FRoadAgent` field must be a `UPROPERTY`** — it lives inside `UPROPERTY TArray<FRoadAgent> UGroundTraffic::Agents`, which serializes only what UHT can see. No `TOptional`.
- **A phase is an enum, never a set of bools.**
- **Comments explain WHY, and especially why an obvious alternative was rejected.** Match the surrounding density; do not strip existing ones.
- **Automation test leaf names must be distinct** — a bare-named test disappears once a dotted child of the same name exists, and only the run count catches it.
- **A new test `.cpp` needs TWO builds**: the first reports `Result: Succeeded` without compiling it. Budget for that in Tasks 3, 6, 8, 9.
- **New `UPROPERTY`s, `UENUM`s and new `USTRUCT`s need a FULL build, not Live Coding.** Tasks 1–2 and 3 must be batched into one editor-closed build round. Function-body-only edits (Tasks 5, 6 partly) can go through `Tools/Mcp.py call LiveCodingToolset CompileLiveCoding`.
- **Build:** `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex` — the editor must be CLOSED.
- **Test:** `./Tools/Run-AirsideTests.ps1 -Filter <name>`. **Never trust the exit code**; read the `N test(s) run, N failed, N crashed` line.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `Airside/Public/Model/RoadEntity.h` | `EPushbackNeed`; `FAirframe::PushbackNeed` | 1 |
| `Airside/Public/Entities/AircraftType.h` | `UAircraftType::PushbackNeed`; carried by `Airframe()` | 1 |
| `Tools/Python/build_plane2_type.py`, `build_piper_type.py` | author the value on the two real types | 1 |
| `Airside/Public/Model/GroundTraffic.h` | `FTrafficRules` push figures + `PushSpeedFor` | 2 |
| `Airside/Private/Model/GroundTraffic.cpp` | `PushSpeedFor` body | 2 |
| **`Airside/Public/Model/PushbackRun.h`** (new) | `EPushPhase`, `FPushbackRun` — the whole manoeuvre, world-free | 3 |
| **`Airside/Private/Model/PushbackRun.cpp`** (new) | its `Start`, `Advance`, `PlanPushDistance` | 3 |
| `Airside/Public/Model/RoadAgent.h` | `EAgentPhase::Manoeuvring`, `EAgentEvent::PushedBack`, `FRoadAgent::Pushback`, three plan accessors | 4, 5 |
| `Airside/Private/Model/RoadAgent.cpp` | `StartPushback`, the `Advance` switch arm, the handover | 4 |
| `Airside/Private/Model/TrafficClaims.cpp` | `CentreOf` sign, `Run`'s phase branch, accessor reads | 5 |
| `Airside/Private/Model/GroundTrafficRebuild.cpp` | re-resolve a push's plan too | 5 |
| `Airside/Public/Model/DeparturePlanner.h` | `EDepartureRefusal::PushbackBlocked` | 6 |
| `Airside/Private/Model/GroundTraffic.cpp` | `DepartAgent`: straight-out test, clearance, enter the phase | 6 |
| `AirportOps/Public/Model/Flight.h` | `EFlightPhase::Manoeuvring` | 7 |
| `AirportOps/Private/Model/Flight.cpp` | `FlightPhaseFromAgent` case | 7 |
| `Airside/Public/Entities/EntityDefinition.h` | `bTaxiThrough` | 8 |
| `Airside/Private/Build/AnchorLink.cpp` | the second pose ray | 8 |
| `AirsideTests/Private/PushbackRunTest.cpp` (new) | the manoeuvre, world-free | 3 |
| `AirsideTests/Private/PushbackClaimTest.cpp` (new) | centre, claim branch, rebuild | 5 |
| `AirsideTests/Private/PushbackDepartTest.cpp` (new) | clearance, straight-out skip | 6 |
| `AirsideTests/Private/TaxiThroughStandTest.cpp` (new) | two lead-ins | 8 |
| `AirsideTests/Private/ClaimCentreTest.cpp` | extended for the reversed body | 5 |
| `AirsideTests/Private/AgentActorTest.cpp` | the composition-level pass-through | 9 |
| `AirportOpsTests/Private/FlightTest.cpp` | the new phase mapping | 7 |
| `docs/AirportManagerGDD.md` | §7 wording: *manoeuvring* the stage, *pushback* the service | 7 |

---

### Task 1: `EPushbackNeed` on the airframe

Data only. Nothing branches on it in this task — the spec is explicit that a value which changed behaviour before a tug existed would be a lie about who pushed.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h` (new enum above `FAirframe` at :570; new field beside `TurnaroundSeconds`)
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/AircraftType.h` (new `UPROPERTY` beside `TurnaroundSeconds` at :136; one line in `Airframe()` at :146-172)
- Modify: `Tools/Python/build_plane2_type.py`, `Tools/Python/build_piper_type.py`
- Test: `Plugins/Airside/Source/AirsideTests/Private/AuthoredPropertiesTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class EPushbackNeed : uint8 { SelfManoeuvre, HandTug, VehicleTug }`; `FAirframe::PushbackNeed` (default `VehicleTug`); `UAircraftType::PushbackNeed`.

- [ ] **Step 1: Add the enum and the airframe field**

In `RoadEntity.h`, immediately above `struct AIRSIDE_API FAirframe` (currently line 570):

```cpp
/**
 * How this aeroplane gets off its stand.
 *
 * A GAMEPLAY LEVER, not a detail. It is the one thing that makes a Pushback depot a
 * decision rather than a tax: an airport flying Twin Otters needs no tug at all, and the
 * first A320 offer is what forces the building.
 *
 * AUTHORED AND NOT DERIVED FROM THE CODE LETTER. "Code A and B reverse, C and above need a
 * tug" needs no field and is wrong for real types - a Dash 8 is Code C and reverses
 * perfectly well. It would also bury a gameplay lever in a letter that exists to dimension
 * pavement, which is the mistake FAirframe::TypeCode's own comment records.
 *
 * THREE VALUES AND NOT A BOOL even though slice 1 only ever asks "is it SelfManoeuvre". The
 * hand-tug tier is the Pushback depot's first upgrade rung - a cheap depot that can move a
 * Dash 8 but not an A320 - and widening a shipped bool is worse than carrying the value now.
 */
UENUM()
enum class EPushbackNeed : uint8
{
	/** Reverses under its own power. A Twin Otter beta-ranges out of a stand. */
	SelfManoeuvre,
	/** Light enough for a towbar on a manual tug. */
	HandTug,
	/** Needs a tug vehicle. */
	VehicleTug
};
```

In `FAirframe`, immediately after `TurnaroundSeconds`:

```cpp
	/**
	 * How this aeroplane leaves its stand - see EPushbackNeed.
	 *
	 * HERE WITH THE FIGURES, for the reason Wingspan, Requirements and Mesh are here: the
	 * thing that pushes an aeroplane holds an FAirframe and no UAircraftType, and Model/ may
	 * not see Entities/ at all.
	 *
	 * DEFAULTS TO VehicleTug, the CONSERVATIVE answer: an airframe assembled by hand (a test,
	 * the Piper fallback) must not silently claim it can reverse itself. Saying "needs a tug"
	 * of something that does not is a missing fee; saying "reverses itself" of an A320 is an
	 * airport that never needs the depot.
	 */
	UPROPERTY(EditAnywhere) EPushbackNeed PushbackNeed = EPushbackNeed::VehicleTug;
```

- [ ] **Step 2: Add the authored property and carry it**

In `AircraftType.h`, after `TurnaroundSeconds` (:136):

```cpp
	/** How this type gets off a stand - see EPushbackNeed. */
	UPROPERTY(EditAnywhere) EPushbackNeed PushbackNeed = EPushbackNeed::VehicleTug;
```

and in `Airframe()`, beside the existing `Out.TurnaroundSeconds = TurnaroundSeconds;`:

```cpp
		Out.PushbackNeed = PushbackNeed;
```

- [ ] **Step 3: Write the failing test**

Append to `AuthoredPropertiesTest.cpp`, inside its existing `RunTest` body (find where it already asserts on `UAircraftType` fields and add beside them; if it asserts per-type, follow that shape):

```cpp
	// THE CAPABILITY MUST SURVIVE THE FLATTENING. UAircraftType::Airframe() is the one
	// crossing from Entities/ into Model/, and a field added to the type but forgotten in
	// that function is a value the simulation never sees - exactly how the Piper's own
	// figures went stale at seven call sites.
	{
		UAircraftType* Type = NewObject<UAircraftType>();
		Type->PushbackNeed = EPushbackNeed::SelfManoeuvre;
		TestEqual(TEXT("Airframe() carries the pushback need"),
			Type->Airframe().PushbackNeed, EPushbackNeed::SelfManoeuvre);
	}

	// AND THE DEFAULT IS THE CONSERVATIVE ONE - a hand-built airframe must not claim it can
	// reverse itself.
	TestEqual(TEXT("a bare airframe needs a tug"),
		FAirframe().PushbackNeed, EPushbackNeed::VehicleTug);
```

- [ ] **Step 4: Build and run**

Close the editor. Run:
```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Run-AirsideTests.ps1 -Filter Airside.Content.AuthoredProperties
```
Expected: `1 test(s) run, 0 failed, 0 crashed`. Read that line; ignore the exit code.

- [ ] **Step 5: Author the two real types**

In `Tools/Python/build_plane2_type.py` (the Twin Otter) set the new property to `SelfManoeuvre`; in `build_piper_type.py` set it to `SelfManoeuvre` as well — a PA-46 is a single-engine light and is not pushed by anything. Follow whatever `set_editor_property` idiom those scripts already use for `turnaround_seconds`. **The editor must be closed** to run them (`Tools/Python/*.py` author headlessly).

Run each script the way the file's own header documents, then confirm the value stuck by reading it back in the same script run — `unreal.log` the property after `save_asset`. Headless saves and deletes both report success while doing nothing, so a read-back is the only evidence.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h `
        Plugins/Airside/Source/Airside/Public/Entities/AircraftType.h `
        Plugins/Airside/Source/AirsideTests/Private/AuthoredPropertiesTest.cpp `
        Tools/Python/build_plane2_type.py Tools/Python/build_piper_type.py `
        Content/Entities
git commit -m "feat(model): an airframe declares how it gets off its stand"
```

---

### Task 2: The push figures on `FTrafficRules`

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/GroundTraffic.h:23-96` (`FTrafficRules`)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/GroundTraffic.cpp` (beside `FootprintFor`/`GapFor`)
- Test: `Plugins/Airside/Source/AirsideTests/Private/GroundTrafficTest.cpp`

**Interfaces:**
- Consumes: `EPushbackNeed` (Task 1).
- Produces: `FTrafficRules::PushSpeedFor(EPushbackNeed) const -> double`; fields `SelfManoeuvrePushSpeed`, `HandTugPushSpeed`, `VehicleTugPushSpeed`, `PushAccel`, `PushSwingLength`, `StraightOutDegrees`, `PowerbackRPMFraction`.

- [ ] **Step 1: Add the fields**

In `FTrafficRules`, after `VehicleGap` (:33):

```cpp
	/**
	 * How fast a push runs, uu/s. 1 uu is 1 cm - see UAircraftType::MainWheelRadius.
	 *
	 * ON THE RULES AND NOT THE AIRFRAME, because push speed is a property of what is doing
	 * the pushing: a hand tug is slower than a vehicle tug whatever it has on the bar. That
	 * is also why they are named for the tug. Slice 2 moves the two tug figures onto the
	 * depot's own types and leaves SelfManoeuvre here.
	 *
	 * FIGURES, NOT MEASUREMENTS. Nothing about a real tug is modelled yet; these exist so the
	 * manoeuvre reads at the right pace on screen.
	 */
	UPROPERTY(EditAnywhere) double SelfManoeuvrePushSpeed = 200.0;  // 2.0 m/s, on the engine
	UPROPERTY(EditAnywhere) double HandTugPushSpeed       = 80.0;   // 0.8 m/s, walking pace
	UPROPERTY(EditAnywhere) double VehicleTugPushSpeed    = 150.0;  // 1.5 m/s

	/** Into and out of the push, uu/s^2. Gentle: a towbar does not snatch. */
	UPROPERTY(EditAnywhere) double PushAccel              = 30.0;   // 0.3 m/s^2

	/**
	 * Room past the lead-in's corner the tug needs to straighten the aeroplane, uu.
	 *
	 * IT IS WHAT MAKES THE PUSH RUN PAST THE CORNER. Stopping at the end of the lead-in
	 * leaves the aeroplane facing straight out of its stand, 90 degrees off the taxiway on an
	 * ordinary perpendicular layout, and the follower then slews that 90 degrees away on the
	 * spot - the same pirouette this whole feature exists to remove, merely smaller.
	 *
	 * A LENGTH AND NOT A CONVERGENCE RULE ("swing until the heading error closes"): this one
	 * is known before the push starts, which is what lets DepartAgent reserve exactly the
	 * ground the push will use.
	 */
	UPROPERTY(EditAnywhere) double PushSwingLength        = 3000.0; // 30 m

	/**
	 * Within this of the parked heading, the way out is forward and no push is needed, deg.
	 *
	 * A MEASUREMENT OF THE GEOMETRY AHEAD, not a property of the stand: it answers a
	 * taxi-through stand, a player-drawn taxiway that happens to run past a stand, and a
	 * graph rebuilt since the aeroplane parked, all with the same question.
	 */
	UPROPERTY(EditAnywhere) double StraightOutDegrees     = 45.0;

	/**
	 * Fraction of MaxRPM a SelfManoeuvre airframe needs before it will move, 0..1.
	 *
	 * A POWERBACK IS THE ENGINE DOING THE WORK, so it cannot start until there is thrust.
	 * A tug-pushed aeroplane moves from the first frame whatever its propeller is doing.
	 *
	 * HERE AND NOT ON FEnginePerformance, which is per-type authored content: this is a rule
	 * about when a manoeuvre may begin, not a fact about any engine, and putting it on the
	 * type would mean re-authoring every aircraft asset to add a number none of them vary.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double PowerbackRPMFraction = 0.6;
```

and beside the existing accessor declarations at the bottom of the struct:

```cpp
	double PushSpeedFor(EPushbackNeed Need) const;
```

Add `#include "Model/RoadEntity.h"` to `GroundTraffic.h` only if it is not already reachable — check first; `FTrafficRules`'s own comment already references `FAirframe`, so it very likely is.

- [ ] **Step 2: Write the failing test**

Append to `GroundTrafficTest.cpp`'s `RunTest`:

```cpp
	// ONE CONSUMER THAT MUST AGREE WITH THE ENUM - the codebase's "lists that must agree are
	// ONE list". A value added to EPushbackNeed without a case here is a compiler warning,
	// not a silent zero, and this pins that the mapping is the one the comments claim.
	FTrafficRules PushRules;
	TestEqual(TEXT("a hand tug is the slowest"),
		PushRules.PushSpeedFor(EPushbackNeed::HandTug), 80.0, 0.001);
	TestEqual(TEXT("a vehicle tug is faster than a hand tug"),
		PushRules.PushSpeedFor(EPushbackNeed::VehicleTug), 150.0, 0.001);
	TestEqual(TEXT("a powerback is fastest of the three"),
		PushRules.PushSpeedFor(EPushbackNeed::SelfManoeuvre), 200.0, 0.001);
```

- [ ] **Step 3: Run it and watch it fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.GroundTraffic
```
Expected: a compile error, `PushSpeedFor` undeclared. That is the failure.

- [ ] **Step 4: Implement**

In `GroundTraffic.cpp`, beside `FootprintFor`/`GapFor`:

```cpp
double FTrafficRules::PushSpeedFor(EPushbackNeed Need) const
{
	// A SWITCH AND NOT A TERNARY CHAIN, deliberately: this is the one place that must agree
	// with EPushbackNeed, so a value added to that enum has to produce a warning here rather
	// than fall quietly into an else.
	switch (Need)
	{
	case EPushbackNeed::SelfManoeuvre: return SelfManoeuvrePushSpeed;
	case EPushbackNeed::HandTug:       return HandTugPushSpeed;
	case EPushbackNeed::VehicleTug:    return VehicleTugPushSpeed;
	}
	return VehicleTugPushSpeed;
}
```

- [ ] **Step 5: Build and run**

Full build (new `UPROPERTY`s — Live Coding will not do). Then:
```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.GroundTraffic
```
Expected: `0 failed, 0 crashed`.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/GroundTraffic.h `
        Plugins/Airside/Source/Airside/Private/Model/GroundTraffic.cpp `
        Plugins/Airside/Source/AirsideTests/Private/GroundTrafficTest.cpp
git commit -m "feat(model): the traffic rules carry what a push costs in time and room"
```

---

### Task 3: `FPushbackRun` — the manoeuvre itself

The core of the feature, and entirely world-free: `Start` and `Advance` take doubles and an `FRoutePlan` and touch no actor, no world, no view.

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Model/PushbackRun.h`
- Create: `Plugins/Airside/Source/Airside/Private/Model/PushbackRun.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PushbackRunTest.cpp` (create)

**Interfaces:**
- Consumes: `FRoutePlan` / `FRouteStep` (`Model/RouteSearch.h`); `GuidelineGeom::PointAtDistance(const TArray<FVector2D>&, double, FVector2D&, double&) -> bool` (`Solve/GuidelineGeom.h`); the Task 2 figures.
- Produces:
  - `enum class EPushPhase : uint8 { Back, Swing }`
  - `FPushbackRun::PlanPushDistance(const FRoutePlan&, double SwingLength, double& OutBack, double& OutPush, double& OutTargetHeading) -> bool` — static, so `DepartAgent` computes the same distances for clearance without re-deriving them
  - `FPushbackRun::Start(const FRoutePlan&, double ParkedHeading, double PushSpeed, double PushAccel, double SwingLength, bool bNeedsThrust) -> bool`
  - `FPushbackRun::Advance(double DeltaSeconds, double StopWithin, bool bHasThrust, FVector2D& OutPosition, double& OutHeading) -> bool` — **false means the push is over**, mirroring `FLandingRun::Advance`
  - fields `Phase`, `Plan`, `Travelled`, `Speed`, `Heading`, `BackDistance`, `PushDistance`, `TargetHeading`, `ParkedHeading`, `PushSpeed`, `PushAccel`, `bNeedsThrust`

- [ ] **Step 1: Write the header**

`Plugins/Airside/Source/Airside/Public/Model/PushbackRun.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RouteSearch.h"
#include "PushbackRun.generated.h"

/**
 * How far through the manoeuvre a push is.
 *
 * AN ENUM AND NOT A BOOL, and there really are two laws rather than one dressed up: Back
 * holds the heading and moves; Swing moves and turns. They can never both be current and
 * they are visited in one order, so the illegal state stops being representable - the same
 * rule EAgentPhase and ECrossingPhase follow.
 */
UENUM()
enum class EPushPhase : uint8
{
	/** Straight down the stand's lead-in. The body keeps the heading it parked at. */
	Back,

	/** Round the corner and onto the taxiway, the heading swinging to meet the line. */
	Swing
};

/**
 * One aeroplane being pushed or reversed off a stand: the fourth motion phase, beside
 * FLandingRun, FRouteFollower and FTakeoffRun.
 *
 * IT WALKS THE DEPARTURE PLAN AND DOES NOT INVENT GEOMETRY. DeparturePlanner::PlanAny already
 * returns a route whose Steps[0] IS the stand lead-in, with its EndDistance computed. Keeping
 * one plan and one distance along it is what lets FClaimPass arbitrate a push with the
 * machinery it already has: a run that computed its own back-out line would be a second
 * evaluator of where the agent is, which the guideline invariant forbids.
 *
 * TRAVELLED KEEPS FRouteFollower'S MEANING - the STEERED axle's distance along Plan.Polyline.
 * The tug couples at the nose gear and its driver follows the painted line, so the nose gear
 * stays constrained to the guideline through the whole manoeuvre. What reverses is which end
 * of the body is ahead of that axle, and that is a fact about the OFFSETS, not about the
 * line: a parked aeroplane's nose gear is at s = 0 and its tail at s > 0, because the lead-in
 * runs away from the terminal it faces. So as Travelled grows the MAINS LEAD and the nose
 * gear trails, which is what a towbar push is. See FClaimPass::CentreOf, which is the one
 * place that has to know.
 *
 * WORLD-FREE, like the three phases it sits beside: Airside.Model.PushbackRun drives the
 * whole of it with no actor, no world and no view.
 */
USTRUCT()
struct AIRSIDE_API FPushbackRun
{
	GENERATED_BODY()

	UPROPERTY() EPushPhase Phase = EPushPhase::Back;

	/** The departure route. A COPY, as FRouteFollower::Plan is, so the follower can be
	 *  started on the same plan at PushDistance when this hands over. */
	UPROPERTY() FRoutePlan Plan;

	/** The steered axle's distance along Plan.Polyline, uu. */
	UPROPERTY() double Travelled = 0.0;

	/** uu/s right now. Trapezoidal: up at PushAccel, down to EXACTLY ZERO at PushDistance. */
	UPROPERTY() double Speed = 0.0;

	/** Which way the BODY faces, radians. Not the direction of travel - that is the whole
	 *  of this struct. */
	UPROPERTY() double Heading = 0.0;

	/** Where Back ends and Swing begins: the end of the straight lead-in. */
	UPROPERTY() double BackDistance = 0.0;

	/** Where the whole manoeuvre ends, and what DepartAgent reserved. */
	UPROPERTY() double PushDistance = 0.0;

	/** The plan's tangent at PushDistance. The Swing lerps the body to exactly this, so the
	 *  follower inherits ZERO heading error - which is the defect this feature removes. */
	UPROPERTY() double TargetHeading = 0.0;

	/** What it was facing on the stand. Back holds it; Swing lerps from it. */
	UPROPERTY() double ParkedHeading = 0.0;

	UPROPERTY() double PushSpeed = 0.0;
	UPROPERTY() double PushAccel = 0.0;

	/** A powerback cannot start without thrust - see FTrafficRules::PowerbackRPMFraction.
	 *  False for anything on a bar: the tug supplies the force. */
	UPROPERTY() bool bNeedsThrust = false;

	/**
	 * The three distances a push is defined by, WITHOUT starting one.
	 *
	 * STATIC AND SHARED WITH DepartAgent, which needs PushDistance before the phase begins so
	 * it can reserve exactly the ground the push will use. Re-deriving the arithmetic there
	 * would be two expressions that must agree and one day will not.
	 *
	 * False when the plan cannot be pushed along at all - no steps, or a polyline too short
	 * to have a direction. The outputs are then untouched: honour the return.
	 */
	static bool PlanPushDistance(const FRoutePlan& InPlan, double SwingLength,
		double& OutBackDistance, double& OutPushDistance, double& OutTargetHeading);

	/** Arms the manoeuvre. False, and nothing is touched, when PlanPushDistance declines. */
	bool Start(const FRoutePlan& InPlan, double InParkedHeading, double InPushSpeed,
		double InPushAccel, double InSwingLength, bool bInNeedsThrust);

	/**
	 * One frame. FALSE MEANS THE PUSH IS OVER and the caller should hand over - the same
	 * contract FLandingRun::Advance uses, so FRoadAgent::Advance's arm reads the same way.
	 *
	 * StopWithin is arbitration's one input, as it is for the follower; bHasThrust gates a
	 * powerback only. Outputs are untouched when it returns false.
	 */
	bool Advance(double DeltaSeconds, double StopWithin, bool bHasThrust,
		FVector2D& OutPosition, double& OutHeading);

	/** True once the manoeuvre has run its length. */
	bool HasArrived() const { return Travelled >= PushDistance - UE_KINDA_SMALL_NUMBER; }
};
```

- [ ] **Step 2: Write the failing test file**

`Plugins/Airside/Source/AirsideTests/Private/PushbackRunTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/PushbackRun.h"
#include "Model/RouteSearch.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	constexpr double PushbackFrame = 1.0 / 60.0;

	/** Shortest angle between two headings, degrees, unsigned. */
	double PushbackDeltaDegrees(double FromRadians, double ToRadians)
	{
		return FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(ToRadians - FromRadians)));
	}

	/**
	 * A stand square to its taxiway: 40 m of lead-in running +X out of the nose-stop, then a
	 * 90 degree corner onto a taxiway running +Y. The aeroplane parked facing -X, into the
	 * terminal, which is what makes the lead-in run away from it.
	 *
	 * This is the ORDINARY layout and the one the whole feature is sized for: the correct
	 * answer here is a 90 degree swing, not the 180 an earlier design would have produced.
	 */
	FRoutePlan PushbackPerpendicularPlan()
	{
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = { {0.0, 0.0}, {4000.0, 0.0}, {4000.0, 8000.0} };
		Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);

		// ONE STEP PER LEG, and Steps[0] is the straight lead-in - which is what
		// PlanPushDistance reads. EndDistance is route distance from the start, the same
		// meaning FRouteStep::EndDistance carries everywhere else.
		FRouteStep LeadIn;
		LeadIn.EndDistance = 4000.0;
		FRouteStep Taxiway;
		Taxiway.EndDistance = Plan.Length;
		Plan.Steps = { LeadIn, Taxiway };
		return Plan;
	}

	/** A dead-end apron: the taxi out runs back the way the lead-in points, so the tug has to
	 *  turn the aeroplane right round. The 180 degree case, which must still work. */
	FRoutePlan PushbackDeadEndPlan()
	{
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = { {0.0, 0.0}, {12000.0, 0.0} };
		Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);
		FRouteStep LeadIn;
		LeadIn.EndDistance = 4000.0;
		FRouteStep Onward;
		Onward.EndDistance = Plan.Length;
		Plan.Steps = { LeadIn, Onward };
		return Plan;
	}

	/** Parked facing the terminal: the lead-in leaves along +X, so the body faces -X. */
	constexpr double PushbackParkedHeading = UE_DOUBLE_PI;

	/** Drives a run to completion, or until Frames runs out. Returns frames used. */
	int32 PushbackRunToEnd(FPushbackRun& Run, int32 Frames, FVector2D& OutAt, double& OutHeading)
	{
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			if (!Run.Advance(PushbackFrame, TNumericLimits<double>::Max(), true, OutAt, OutHeading))
			{
				return Frame;
			}
		}
		return Frames;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackRunTest,
	"Airside.Model.PushbackRun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackRunTest::RunTest(const FString& Parameters)
{
	const double SwingLength = 3000.0;

	// THE PUSH RUNS PAST THE CORNER. An earlier design stopped at the end of the lead-in;
	// the plan's tangent there is still the LEAD-IN's, so the aeroplane would have been
	// handed over facing straight out of its stand and the follower would have slewed 90
	// degrees on the spot - the pirouette this whole feature exists to remove, merely
	// smaller. This assertion is that defect, measured.
	{
		double Back = 0.0;
		double Push = 0.0;
		double Target = 0.0;
		TestTrue(TEXT("a perpendicular stand can be pushed"),
			FPushbackRun::PlanPushDistance(PushbackPerpendicularPlan(), SwingLength,
				Back, Push, Target));
		TestEqual(TEXT("Back ends at the end of the lead-in"), Back, 4000.0, 0.01);
		TestEqual(TEXT("the push runs a swing length past it"), Push, 7000.0, 0.01);

		// +Y, the taxiway - NOT +X, the lead-in.
		TestEqual(TEXT("the target heading is the taxiway, not the lead-in"),
			PushbackDeltaDegrees(Target, UE_DOUBLE_HALF_PI), 0.0, 0.01);
	}

	// A PERPENDICULAR STAND SWINGS 90 DEGREES. Parked facing -X, ending facing +Y.
	{
		FPushbackRun Run;
		TestTrue(TEXT("the run starts"),
			Run.Start(PushbackPerpendicularPlan(), PushbackParkedHeading,
				150.0, 30.0, SwingLength, false));
		TestEqual(TEXT("it starts facing the way it parked"),
			PushbackDeltaDegrees(Run.Heading, PushbackParkedHeading), 0.0, 0.001);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		const int32 Frames = PushbackRunToEnd(Run, 20000, At, Heading);
		TestTrue(TEXT("the push ends"), Frames < 20000);

		TestEqual(TEXT("the swing is 90 degrees, not 180"),
			PushbackDeltaDegrees(PushbackParkedHeading, Run.Heading), 90.0, 0.5);

		// THE WHOLE POINT: the follower inherits no heading error at all.
		TestEqual(TEXT("it ends on the plan's tangent, so nothing is left to slew"),
			PushbackDeltaDegrees(Run.Heading, Run.TargetHeading), 0.0, 0.01);
		TestEqual(TEXT("and it ends where it was cleared to"),
			Run.Travelled, Run.PushDistance, 1.0);

		// IT STOPS DEAD before the handover. The aeroplane is about to reverse its direction
		// of travel; handing the follower a non-zero speed would have it taxi forwards at the
		// speed it was being pushed backwards at.
		TestEqual(TEXT("the push ends at rest"), Run.Speed, 0.0, 0.01);
	}

	// THE BODY IS REVERSED THROUGH THE STRAIGHT. Travelled grows along +X while the body
	// faces -X: the mains lead and the nose gear trails, which is what a towbar push is.
	{
		FPushbackRun Run;
		Run.Start(PushbackPerpendicularPlan(), PushbackParkedHeading,
			150.0, 30.0, SwingLength, false);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		for (int32 Frame = 0; Frame < 600 && Run.Travelled < 2000.0; ++Frame)
		{
			Run.Advance(PushbackFrame, TNumericLimits<double>::Max(), true, At, Heading);
		}
		TestTrue(TEXT("it has moved out along the lead-in"), Run.Travelled > 500.0);
		TestEqual(TEXT("the nose gear is on the line"), At.Y, 0.0, 0.01);
		TestEqual(TEXT("and the body still faces the terminal"),
			PushbackDeltaDegrees(Heading, PushbackParkedHeading), 0.0, 0.01);
		TestEqual(TEXT("Back is still the phase"), Run.Phase, EPushPhase::Back);
	}

	// A DEAD-END APRON TURNS IT RIGHT ROUND - 180 degrees, and that is correct, not a bug.
	{
		FPushbackRun Run;
		Run.Start(PushbackDeadEndPlan(), PushbackParkedHeading, 150.0, 30.0, SwingLength, false);
		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		PushbackRunToEnd(Run, 20000, At, Heading);
		TestEqual(TEXT("a dead end swings the full half turn"),
			PushbackDeltaDegrees(PushbackParkedHeading, Run.Heading), 180.0, 0.5);
	}

	// A POWERBACK WAITS FOR THRUST; A TUG DOES NOT. The engine is doing the work in one case
	// and not in the other, and this is the ONE place in slice 1 where the need changes
	// anything - justified because it is about the aeroplane's physics, not about a tug.
	{
		FPushbackRun Powerback;
		Powerback.Start(PushbackPerpendicularPlan(), PushbackParkedHeading,
			200.0, 30.0, SwingLength, /*bNeedsThrust*/ true);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		for (int32 Frame = 0; Frame < 120; ++Frame)
		{
			Powerback.Advance(PushbackFrame, TNumericLimits<double>::Max(),
				/*bHasThrust*/ false, At, Heading);
		}
		TestEqual(TEXT("a powerback without thrust has not moved"),
			Powerback.Travelled, 0.0, 0.0001);

		Powerback.Advance(PushbackFrame, TNumericLimits<double>::Max(), true, At, Heading);
		TestTrue(TEXT("and moves the frame it has thrust"), Powerback.Travelled > 0.0);

		FPushbackRun Towed;
		Towed.Start(PushbackPerpendicularPlan(), PushbackParkedHeading,
			150.0, 30.0, SwingLength, /*bNeedsThrust*/ false);
		Towed.Advance(PushbackFrame, TNumericLimits<double>::Max(), false, At, Heading);
		TestTrue(TEXT("a towed aeroplane moves on frame one whatever the propeller is doing"),
			Towed.Travelled > 0.0);
	}

	// ARBITRATION IS THE ONE INPUT, exactly as it is for the follower: a push held short
	// stops where it is told and does not creep.
	{
		FPushbackRun Run;
		Run.Start(PushbackPerpendicularPlan(), PushbackParkedHeading,
			150.0, 30.0, SwingLength, false);
		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		for (int32 Frame = 0; Frame < 2000; ++Frame)
		{
			Run.Advance(PushbackFrame, /*StopWithin*/ 1000.0, true, At, Heading);
		}
		TestEqual(TEXT("a push stops where arbitration says"), Run.Travelled, 1000.0, 1.0);
	}

	// AN UNPUSHABLE PLAN IS REFUSED AND LEAVES NOTHING HALF-ARMED - the rule
	// FRoadAgent::StartArrival already follows for a landing that cannot be flown.
	{
		FRoutePlan Empty;
		double Back = -1.0;
		double Push = -1.0;
		double Target = -1.0;
		TestFalse(TEXT("a plan with no steps cannot be pushed"),
			FPushbackRun::PlanPushDistance(Empty, SwingLength, Back, Push, Target));
		TestEqual(TEXT("and the outputs are untouched"), Back, -1.0, 0.0001);

		FPushbackRun Run;
		TestFalse(TEXT("Start declines it"),
			Run.Start(Empty, PushbackParkedHeading, 150.0, 30.0, SwingLength, false));
		TestEqual(TEXT("and arms nothing"), Run.PushDistance, 0.0, 0.0001);
	}

	return true;
}

#endif
```

- [ ] **Step 3: Run it and watch it fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.PushbackRun
```
Expected: a compile error — `PushbackRun.cpp` does not exist, so `PlanPushDistance`, `Start` and `Advance` are unresolved externals. **Remember a new test `.cpp` needs two builds**; if the first build reports `Succeeded` and the filter matches `0 test(s) run`, build again before concluding anything.

- [ ] **Step 4: Implement**

`Plugins/Airside/Source/Airside/Private/Model/PushbackRun.cpp`:

```cpp
#include "Model/PushbackRun.h"

#include "Solve/GuidelineGeom.h"

bool FPushbackRun::PlanPushDistance(const FRoutePlan& InPlan, double SwingLength,
	double& OutBackDistance, double& OutPushDistance, double& OutTargetHeading)
{
	if (!InPlan.IsValid() || InPlan.Steps.Num() == 0)
	{
		return false;
	}

	// STEPS[0] IS THE STRAIGHT LEAD-IN. FAnchorLink::Gather casts it as a straight ray out of
	// the pose node and builds the corner's entry sweeps as steps of their own, so Back needs
	// no steering law at all - the body simply holds its heading.
	const double Back = InPlan.Steps[0].EndDistance;

	// PAST THE CORNER. See FTrafficRules::PushSwingLength for why stopping at Back is the
	// defect rather than the fix. Clamped to the plan: a route shorter than a swing length
	// is pushed as far as it goes, which is better than sampling off the end of it.
	const double Push = FMath::Min(Back + SwingLength, InPlan.Length);

	FVector2D At = FVector2D::ZeroVector;
	double Tangent = 0.0;
	if (!GuidelineGeom::PointAtDistance(InPlan.Polyline, Push, At, Tangent))
	{
		// A polyline too short to have a direction. HONOURED rather than ignored: the
		// outputs stay untouched, so a caller that took the heading anyway would be reading
		// its own uninitialised double.
		return false;
	}

	OutBackDistance = Back;
	OutPushDistance = Push;
	OutTargetHeading = Tangent;
	return true;
}

bool FPushbackRun::Start(const FRoutePlan& InPlan, double InParkedHeading, double InPushSpeed,
	double InPushAccel, double InSwingLength, bool bInNeedsThrust)
{
	double Back = 0.0;
	double Push = 0.0;
	double Target = 0.0;
	if (!PlanPushDistance(InPlan, InSwingLength, Back, Push, Target))
	{
		// NOTHING TOUCHED. A manoeuvre that cannot be flown must leave no trace of itself on
		// the struct rather than one half-armed - the rule FRoadAgent::StartArrival states.
		return false;
	}

	Plan = InPlan;
	Phase = EPushPhase::Back;
	Travelled = 0.0;
	Speed = 0.0;
	Heading = InParkedHeading;
	ParkedHeading = InParkedHeading;
	BackDistance = Back;
	PushDistance = Push;
	TargetHeading = Target;
	PushSpeed = FMath::Max(0.0, InPushSpeed);
	PushAccel = FMath::Max(UE_KINDA_SMALL_NUMBER, InPushAccel);
	bNeedsThrust = bInNeedsThrust;
	return true;
}

bool FPushbackRun::Advance(double DeltaSeconds, double StopWithin, bool bHasThrust,
	FVector2D& OutPosition, double& OutHeading)
{
	if (HasArrived())
	{
		return false;
	}

	// A POWERBACK IS THE ENGINE DOING THE WORK, so it holds at rest until there is thrust.
	// Returning true rather than false: the manoeuvre is still current, it is simply not
	// moving yet, and handing over here would put a stopped aeroplane on the taxi.
	if (bNeedsThrust && !bHasThrust)
	{
		Speed = 0.0;
		return GuidelineGeom::PointAtDistance(Plan.Polyline, Travelled, OutPosition, OutHeading)
			? (OutHeading = Heading, true)
			: true;
	}

	// WHERE IT MAY GET TO THIS FRAME: the end of the push, or wherever arbitration stopped
	// it, whichever is nearer. StopWithin is the ONE input into the motion, exactly as it is
	// for the follower - there is no second evaluator of where this agent may go.
	const double StopAt = FMath::Min(PushDistance, FMath::Max(0.0, StopWithin));

	// TRAPEZOIDAL, AND IT ENDS AT REST. The aeroplane is about to reverse its direction of
	// travel: handing the follower a non-zero speed would have it taxi forwards at the speed
	// it was just being pushed backwards at. Braking distance is the same v^2/2a the claim
	// window uses, so the two agree about what "far enough to stop" means.
	const double Remaining = FMath::Max(0.0, StopAt - Travelled);
	const double BrakingDistance = Speed * Speed / (2.0 * PushAccel);
	Speed = BrakingDistance >= Remaining
		? FMath::Max(0.0, Speed - PushAccel * DeltaSeconds)
		: FMath::Min(PushSpeed, Speed + PushAccel * DeltaSeconds);

	Travelled = FMath::Clamp(Travelled + Speed * DeltaSeconds, 0.0, StopAt);

	// THE STEERED AXLE IS ON THE LINE, and that never changes - see this struct's header.
	// What reverses is which end of the body is ahead of it, which is FClaimPass::CentreOf's
	// business and not this function's.
	double LineHeading = 0.0;
	if (!GuidelineGeom::PointAtDistance(Plan.Polyline, Travelled, OutPosition, LineHeading))
	{
		return true;
	}

	Phase = Travelled < BackDistance ? EPushPhase::Back : EPushPhase::Swing;

	if (Phase == EPushPhase::Back)
	{
		// The lead-in is straight, so there is nothing to steer for: the body holds the
		// heading it parked at while the tug pulls it out.
		Heading = ParkedHeading;
	}
	else
	{
		// LERPED LINEARLY IN TRAVELLED, not in time: at a constant push speed that is a
		// constant yaw rate, and when arbitration slows the push the turn slows with it -
		// which is what a tug does. Time-based would keep turning an aeroplane that had been
		// stopped, and it would arrive at TargetHeading before it arrived at PushDistance.
		//
		// UnwindRadians FIRST so the turn is taken the short way round rather than very
		// nearly all the way about, and so a 180 degree dead-end case is stable.
		const double Swing = FMath::Max(UE_KINDA_SMALL_NUMBER, PushDistance - BackDistance);
		const double Alpha = FMath::Clamp((Travelled - BackDistance) / Swing, 0.0, 1.0);
		Heading = ParkedHeading
			+ Alpha * FMath::UnwindRadians(TargetHeading - ParkedHeading);
	}

	OutHeading = Heading;

	// TRUE WHILE IT IS STILL MINE. HasArrived is re-asked at the TOP of the next frame rather
	// than answered here, so the last frame of the push still reports its own motion - a
	// frame with no motion at all is exactly the step the handover-continuity test catches.
	return true;
}
```

- [ ] **Step 5: Build twice, then run**

Close the editor. Build. Build again (the new `.cpp` files). Then:
```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.PushbackRun
```
Expected: `1 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/PushbackRun.h `
        Plugins/Airside/Source/Airside/Private/Model/PushbackRun.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PushbackRunTest.cpp
git commit -m "feat(model): a push backs an aeroplane out of its stand and swings it onto the line"
```

---

### Task 4: `EAgentPhase::Manoeuvring` and the handover

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadAgent.h` (`EAgentPhase` :26-43; `EAgentEvent` :64-79; a new `FPushbackRun Pushback` field; `StartPushback` declaration)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadAgent.cpp` (`DescribeMotion` :89-94; `StartTaxi` :164-183; `Advance`'s switch :208+)
- Test: `Plugins/Airside/Source/AirsideTests/Private/RoadAgentTest.cpp`, `AgentEventsTest.cpp`

**Interfaces:**
- Consumes: `FPushbackRun` (Task 3); `FTrafficRules::PowerbackRPMFraction` (Task 2).
- Produces: `EAgentPhase::Manoeuvring`; `EAgentEvent::PushedBack`; `FRoadAgent::Pushback`; `FRoadAgent::StartPushback(const FRoutePlan&, const FAirframe&, double ParkedHeading, double PushSpeed, double PushAccel, double SwingLength, double ThrustRPM) -> bool`.

- [ ] **Step 1: Add the phase and the event**

In `EAgentPhase`, **between `Parked` and `Gone`** — the enum's own comment does not pin the order and nothing compares its values (only `EFlightPhase`'s order is load-bearing), but keeping it in lifecycle order is what a reader expects:

```cpp
	/** At the stand, taxi over, running down the post-arrival shutdown pause. */
	Parked,

	/**
	 * Coming off the stand: backed down the lead-in and swung onto the taxiway. The push
	 * drives it - see FPushbackRun.
	 *
	 * NOT CALLED Pushback, and the distinction is the point of the feature: a Twin Otter
	 * reverses under its own power and is not being pushed by anything. The PHASE is the
	 * manoeuvre; the SERVICE - the depot, the tug, the fee - is pushback.
	 */
	Manoeuvring,

	/** The take-off has cleared. FRoadAgent::Advance returns false from here on. */
	Gone
```

In `EAgentEvent`, beside the other handovers:

```cpp
	/** Manoeuvring -> Taxiing: off the stand and aligned, the taxi out starts. */
	PushedBack,
```

- [ ] **Step 2: Add the field and the starter**

In `FRoadAgent`, beside `Departure`:

```cpp
	/**
	 * Drives Phase == Manoeuvring.
	 *
	 * A FOURTH MOTION PHASE, not a mode inside the follower - see FPushbackRun, and see this
	 * struct's own header for why four sibling structs beat four subclasses here.
	 */
	UPROPERTY() FPushbackRun Pushback;
```

and, beside `StartTaxi`:

```cpp
	/**
	 * Sends a parked aeroplane off its stand. False, and nothing is touched, when the plan
	 * cannot be pushed along.
	 *
	 * THE ENGINE IS STARTED HERE AND NOT IN StartTaxi. Real practice is "push and start": the
	 * crew spools up WHILE the tug pushes, and the spool routinely outlasts the manoeuvre.
	 * AdvanceEngine already runs first and unconditionally every frame whatever phase is
	 * driving, so moving the cold start to here is the whole of it - and the handover into
	 * the follower must then NOT go through StartTaxi, which writes EngineRPM = 0.0.
	 */
	bool StartPushback(const FRoutePlan& Plan, const FAirframe& InAirframe, double ParkedHeading,
		double PushSpeed, double PushAccel, double SwingLength, double ThrustRPM);
```

Add `#include "Model/PushbackRun.h"` to `RoadAgent.h`'s include block, in alphabetical order with the rest.

Also add a `ThrustRPM` member so `Advance` can ask the gate without a rules pointer:

```cpp
	/** RPM at or above which a powerback may begin, copied from the rules at StartPushback
	 *  because this struct is world-free and cannot read FTrafficRules for itself - the same
	 *  reason ShutdownPause is a copy. Zero for anything on a tug bar. */
	UPROPERTY() double PushbackThrustRPM = 0.0;
```

- [ ] **Step 3: Write the failing test**

Append to `RoadAgentTest.cpp`'s `RunTest`:

```cpp
	// PUSH AND START. The aeroplane's engine comes alive as the manoeuvre begins and the
	// propeller is STILL SPOOLING when the taxi takes over - which is what "the spool
	// outlasts the tug" means, and what handing over through StartTaxi would have destroyed
	// by writing EngineRPM back to zero.
	{
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = { {0.0, 0.0}, {4000.0, 0.0}, {4000.0, 8000.0} };
		Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);
		FRouteStep LeadIn;
		LeadIn.EndDistance = 4000.0;
		FRouteStep Taxiway;
		Taxiway.EndDistance = Plan.Length;
		Plan.Steps = { LeadIn, Taxiway };

		FAirframe Airframe = TestAirframes::Piper();
		Airframe.PushbackNeed = EPushbackNeed::VehicleTug;

		FRoadAgent Agent;
		Agent.Phase = EAgentPhase::Parked;
		TestTrue(TEXT("a parked aeroplane can be pushed"),
			Agent.StartPushback(Plan, Airframe, UE_DOUBLE_PI, 150.0, 30.0, 3000.0, 0.0));
		TestEqual(TEXT("it is manoeuvring"), Agent.Phase, EAgentPhase::Manoeuvring);
		TestTrue(TEXT("the engine is running"), Agent.bEngineRunning);
		TestEqual(TEXT("from cold"), Agent.EngineRPM, 0.0, 0.0001);

		FAgentMotion Motion;
		EAgentEvent Event = EAgentEvent::None;
		bool bPushedBackSeen = false;
		double RPMAtHandover = -1.0;
		for (int32 Frame = 0; Frame < 20000; ++Frame)
		{
			Agent.Advance(1.0 / 60.0, Motion, Event);
			if (Event == EAgentEvent::PushedBack)
			{
				bPushedBackSeen = true;
				RPMAtHandover = Agent.EngineRPM;
				break;
			}
		}
		TestTrue(TEXT("the push hands over"), bPushedBackSeen);
		TestEqual(TEXT("and it is taxiing after it"), Agent.Phase, EAgentPhase::Taxiing);
		TestTrue(TEXT("the propeller kept the RPM it had spooled to"), RPMAtHandover > 0.0);

		// THE HANDOVER IS CONTINUOUS - no frame with the aeroplane at the origin, and no
		// heading left for the follower to slew. The second half is the defect this whole
		// feature removes, asserted at the level of the agent rather than the run.
		TestTrue(TEXT("it is not at the world origin"), Motion.Position.SizeSquared() > 1.0);
		TestEqual(TEXT("the follower inherits no heading error"),
			FMath::Abs(FMath::RadiansToDegrees(
				FMath::UnwindRadians(Agent.Follower.Heading - Agent.Pushback.TargetHeading))),
			0.0, 0.01);
	}
```

Add `#include "Solve/GuidelineGeom.h"` to `RoadAgentTest.cpp` if it is not already there.

- [ ] **Step 4: Run it and watch it fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.RoadAgent
```
Expected: compile error, `StartPushback` undeclared.

- [ ] **Step 5: Implement the starter**

In `RoadAgent.cpp`, beside `StartTaxi`:

```cpp
bool FRoadAgent::StartPushback(const FRoutePlan& Plan, const FAirframe& InAirframe,
	double ParkedHeading, double PushSpeed, double PushAccel, double SwingLength, double ThrustRPM)
{
	// A POWERBACK IS THE ENGINE DOING THE WORK; anything on a bar is moved by the tug and its
	// propeller is incidental. This is the ONE place in slice 1 where the need changes what
	// happens, and it is justified because it is a fact about the AEROPLANE rather than about
	// a tug that does not exist yet.
	const bool bNeedsThrust = InAirframe.PushbackNeed == EPushbackNeed::SelfManoeuvre;

	if (!Pushback.Start(Plan, ParkedHeading, PushSpeed, PushAccel, SwingLength, bNeedsThrust))
	{
		// FPushbackRun has already declined. Nothing else is touched: a manoeuvre that cannot
		// be flown must leave no trace of itself on the agent rather than one half-armed.
		return false;
	}

	Phase = EAgentPhase::Manoeuvring;
	Airframe = InAirframe;
	PushbackThrustRPM = ThrustRPM;

	// PUSH AND START, and the cold start lives HERE rather than in StartTaxi - see this
	// function's declaration. AdvanceEngine spools it from this zero over SpoolUpSeconds,
	// whatever phase is driving, so the propeller is still coming up as the taxi takes over.
	bEngineRunning = true;
	EngineRPM = 0.0;

	// The fallback pose, for the same reason StartTaxi has one: a caller that reads LastMotion
	// before the first real Advance must see where the manoeuvre actually begins, never the
	// FVector2D default - the "world origin" bug this field exists to prevent.
	LastMotion = FAgentMotion();
	if (Plan.Polyline.Num() > 0)
	{
		LastMotion.Position = Plan.Polyline[0];
	}
	LastMotion.Heading = ParkedHeading;
	return true;
}
```

- [ ] **Step 6: Implement the switch arm and the handover**

In `FRoadAgent::Advance`, add a `case` **above** `case EAgentPhase::Taxiing:` so it can fall through into it the way `Arriving` does:

```cpp
	case EAgentPhase::Manoeuvring:
	{
		// THE THRUST GATE, asked here rather than inside FPushbackRun because the RPM is the
		// agent's, not the run's - the run is world-free and holds no engine.
		const bool bHasThrust = EngineRPM >= PushbackThrustRPM;

		FVector2D PushAt = At;
		double PushHeading = Heading;
		if (Pushback.Advance(DeltaSeconds, StopWithin, bHasThrust, PushAt, PushHeading))
		{
			LastMotion = DescribeMotion(PushAt, PushHeading);
			OutMotion = LastMotion;
			return true;
		}

		// OFF THE STAND: hand over to the taxi, on the SAME plan at the distance the push
		// reached. Heading carries across and is by construction the plan's tangent there, so
		// the follower starts with zero error - which is the whole of what this feature fixes.
		//
		// SPEED ZERO, deliberately, and unlike the Vacated handover above: the aeroplane has
		// just been moving BACKWARDS and is about to move forwards. Carrying the push's speed
		// across would have it pull away at the speed it was pushed at, in the other
		// direction, on the handover frame.
		//
		// Follower.Start AND NOT StartTaxi, which writes EngineRPM = 0.0 from cold and would
		// undo the spool the push has been running - see StartPushback.
		Phase = EAgentPhase::Taxiing;
		OutEvent = EAgentEvent::PushedBack;
		Follower.Start(Pushback.Plan, Airframe, 0.0, Pushback.Heading, Pushback.Travelled);
		UE_LOG(LogAirsideTraffic, Log, TEXT("Push complete; taxiing out."));

		// AND TAXI THIS SAME FRAME, for the reason the Vacated fallthrough gives: the push
		// declined this frame without moving, so the frame's dt is the follower's. A frame
		// with no motion at all is exactly the step the handover-continuity test catches.
		[[fallthrough]];
	}

	case EAgentPhase::Taxiing:
```

In `DescribeMotion`'s `switch (Phase)` for `GroundSpeed` (:89-94), add:

```cpp
	case EAgentPhase::Manoeuvring: Motion.GroundSpeed = Pushback.Speed; break;
```

**A phase missing from this line is a phase with stopped wheels** — that comment is already there and this is exactly the case it warns about. Leave `SteerAngleDegrees` reading `Follower.SteerDegrees`: a pushed aeroplane's nose gear is castoring on the towbar and the follower's zero is the right answer, the same reasoning the existing comment gives for a landing rollout.

- [ ] **Step 7: Build and run**

Full build (new `UENUM` value, new `USTRUCT` member). Then:
```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.RoadAgent
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.AgentEvents
```
Expected: `0 failed, 0 crashed` on both. `AgentEvents` may need a new case added for `PushedBack` if it enumerates the event list — check and extend it rather than letting it silently ignore the new value.

- [ ] **Step 8: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RoadAgent.h `
        Plugins/Airside/Source/Airside/Private/Model/RoadAgent.cpp `
        Plugins/Airside/Source/AirsideTests/Private/RoadAgentTest.cpp `
        Plugins/Airside/Source/AirsideTests/Private/AgentEventsTest.cpp
git commit -m "feat(model): an aeroplane manoeuvres off its stand before it taxis"
```

---

### Task 5: Claims, the reversed centre, and the rebuild

The task with the highest chance of a silent defect. Its first test is the one that fails on today's arithmetic.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadAgent.h` (three accessors)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/TrafficClaims.cpp` (`CentreOf` :169-176; `WindowFor` :178+; `Run` :919-931; the other reads of `Agent.Follower.*`)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/GroundTrafficRebuild.cpp` (:189 and the nine other reads)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/ClaimCentreTest.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PushbackClaimTest.cpp` (create)

**Interfaces:**
- Consumes: `EAgentPhase::Manoeuvring`, `FRoadAgent::Pushback` (Task 4).
- Produces: `FRoadAgent::PlanInProgress() const -> const FRoutePlan&`; `FRoadAgent::DistanceAlongPlan() const -> double`; `FRoadAgent::SpeedAlongPlan() const -> double`.

- [ ] **Step 1: Write the failing centre test**

Append to `ClaimCentreTest.cpp`'s `RunTest`, before `return true`:

```cpp
	// THE BODY REVERSES AND THE OFFSET MUST REVERSE WITH IT. BodyCentreX - SteerAxleX is
	// NEGATIVE on a conforming airframe - the plan centre sits aft of the nose gear - so the
	// taxi form puts the centre BEHIND the steered axle in plan distance. During a push the
	// mains lead and the body is AHEAD of it, so the same subtraction misplaces the claimed
	// body by twice the offset: 12.6 m on plane2, whose re-export about its nose gear is the
	// reason this function exists at all.
	//
	// What that would look like: an aeroplane still holding the stand it had left, and
	// pushing into ground it had not claimed. Neither is visible in a log.
	FRoadAgent Pushing;
	Pushing.Airframe.Ground = TestAirframes::Piper().Ground;
	Pushing.Airframe.SteerAxleX = 0.0;
	Pushing.Airframe.FixedAxleX = -454.3;
	Pushing.Airframe.BodyCentreX = -629.3;   // plane2 as measured
	Pushing.Phase = EAgentPhase::Manoeuvring;
	Pushing.Pushback.Travelled = 10000.0;

	TestEqual(TEXT("a pushed body's centre LEADS its nose gear"),
		FClaimPass::CentreOf(Pushing), 10000.0 + 629.3, 0.01);

	// AND THE TAXI ANSWER IS UNCHANGED - the assertions above this one already pin it, but
	// the same agent read in the other phase is what makes the pair a contrast rather than
	// two separate facts.
	Pushing.Phase = EAgentPhase::Taxiing;
	Pushing.Follower.Travelled = 10000.0;
	TestEqual(TEXT("a taxiing body's centre still trails it"),
		FClaimPass::CentreOf(Pushing), 10000.0 - 629.3, 0.01);
```

- [ ] **Step 2: Run it and watch it fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.ClaimCentre
```
Expected: FAIL — `a pushed body's centre LEADS its nose gear` reports `9370.7`, not `10629.3`. That number is the defect.

- [ ] **Step 3: Add the accessors**

In `FRoadAgent`, beside `GroundPosition()`:

```cpp
	/**
	 * The plan this agent is currently walking, and how far along it, and how fast.
	 *
	 * THREE ACCESSORS AND NOT THREE DIRECT READS OF Follower, because since the push there
	 * are TWO structs that can be driving an agent along a route, and the claim pass, the
	 * rebuild and the deadlock resolver all have to ask the question rather than assume the
	 * answer. Reading Agent.Follower.Travelled on a manoeuvring agent returns whatever the
	 * last taxi left there - a stale distance on a live plan, which claims ground the
	 * aeroplane is nowhere near.
	 *
	 * NOT A SWITCH ON EVERY PHASE: an arrival and a departure are not on a route at all, and
	 * their callers already branch away before they get here (FClaimPass::Run's first arm).
	 * The follower is the answer for everything else, which keeps a vehicle's path unchanged.
	 */
	const FRoutePlan& PlanInProgress() const
	{
		return Phase == EAgentPhase::Manoeuvring ? Pushback.Plan : Follower.Plan;
	}
	double DistanceAlongPlan() const
	{
		return Phase == EAgentPhase::Manoeuvring ? Pushback.Travelled : Follower.Travelled;
	}
	double SpeedAlongPlan() const
	{
		return Phase == EAgentPhase::Manoeuvring ? Pushback.Speed : Follower.Speed;
	}
```

- [ ] **Step 4: Fix `CentreOf` and route the claim pass through the accessors**

In `TrafficClaims.cpp`, replace `CentreOf`'s body, keeping its existing comment and adding to it:

```cpp
double FClaimPass::CentreOf(const FRoadAgent& Agent)
{
	// ALONG THE ROUTE rather than along the body axis, and the approximation is deliberate:
	// on a bend the two differ by well under a centimetre at these offsets, and the exact
	// form would need the heading here - turning a distance into a pose, and this function
	// into a second evaluator of where the agent is. See the guideline invariant.
	//
	// THE SIGN IS THE PHASE'S. Ahead is negative on a conforming airframe - the plan centre
	// sits aft of the nose gear - which puts the centre BEHIND the steered axle while the
	// nose gear leads. A push reverses which end leads, so it reverses this: the mains are
	// ahead of the nose gear in plan distance. Getting it wrong misplaces the claimed body by
	// twice the offset, 12.6 m on plane2, and shows as an aeroplane holding a stand it has
	// left. Airside.Model.ClaimCentre pins both directions.
	const double Ahead = Agent.Airframe.BodyCentreX - Agent.Airframe.SteerAxleX;
	const double Sign = Agent.Phase == EAgentPhase::Manoeuvring ? -1.0 : 1.0;
	return Agent.DistanceAlongPlan() + Sign * Ahead;
}
```

In `WindowFor`, replace `const FRoutePlan& Plan = Agent.Follower.Plan;` with `Agent.PlanInProgress()`, and `Agent.Follower.Speed` with `Agent.SpeedAlongPlan()`. Leave `Agent.Airframe.Ground.Taxi.Decel` alone — a push brakes on `FPushbackRun::PushAccel`, but the claim window is about the room the agent *reserves*, and reserving the taxi's longer braking distance during a 1.5 m/s push is conservative in the safe direction. Add a one-line comment saying so, so the next reader does not "fix" it.

In `Run`, change the first branch:

```cpp
	// NOT ON A ROUTE: hold the runway and nothing else. An arrival on the roll and a
	// departure lining up own the strip; whatever either held on the taxiway before the
	// handover is released here, which is what makes "Vacated releases the chain" fall out of
	// the tick rather than needing a call of its own.
	//
	// MANOEUVRING IS ON A ROUTE and must NOT take this arm: a pushing aeroplane is standing
	// on its stand's lead-in, and releasing every guideline claim would show that ground free
	// with an aeroplane on it - the same hole spec 3.4's crossing rule exists to close.
	if (Agent.Phase != EAgentPhase::Taxiing && Agent.Phase != EAgentPhase::Manoeuvring)
```

and replace the `const FRoutePlan& Plan = Agent.Follower.Plan;` below it with `Agent.PlanInProgress()`. Walk the remaining `Agent.Follower.` reads in this file and convert each one that is reached on a manoeuvring agent's path; leave any inside a `Phase == Taxiing` guard alone and say why in a comment if it is not obvious.

- [ ] **Step 5: Fix the rebuild path**

In `GroundTrafficRebuild.cpp:189`, the branch is currently:

```cpp
		if (Agent.Phase == EAgentPhase::Taxiing && Agent.Follower.Plan.Steps.Num() > 0)
```

A manoeuvring agent has a live plan that a graph rebuild can kill exactly as it kills a taxi's, so it must re-resolve too. Extend the branch and route its reads through the accessors. Where the rebuild *writes* back (it re-points steps and adjusts `Travelled`), it must write to whichever struct owns them — add a small local reference rather than duplicating the block:

```cpp
		// THE PUSH IS ON A ROUTE TOO. A player who redraws a taxiway while an aeroplane is
		// being pushed off its stand has the same stale plan a taxiing agent would, and a
		// push whose plan died under it would walk a polyline that no longer exists.
		const bool bOnRoute = Agent.Phase == EAgentPhase::Taxiing
			|| Agent.Phase == EAgentPhase::Manoeuvring;
		FRoutePlan& LivePlan = Agent.Phase == EAgentPhase::Manoeuvring
			? Agent.Pushback.Plan : Agent.Follower.Plan;
		double& LiveTravelled = Agent.Phase == EAgentPhase::Manoeuvring
			? Agent.Pushback.Travelled : Agent.Follower.Travelled;
```

Work through the file's ten reads one at a time; each is a small mechanical substitution, and **every `UE_LOG` in the file must survive** — count `UE_LOG(` before and after.

- [ ] **Step 6: Write the claim test**

`Plugins/Airside/Source/AirsideTests/Private/PushbackClaimTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/PushbackRun.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficClaims.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackHoldsLeadInTest,
	"Airside.Model.PushbackHoldsLeadIn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackHoldsLeadInTest::RunTest(const FString& Parameters)
{
	// A PUSHING AEROPLANE IS STANDING ON ITS LEAD-IN. FClaimPass::Run's first arm releases
	// every guideline claim for a phase that is not on a route, and a manoeuvring agent that
	// fell into it would show that ground free with an aeroplane on it - so something could
	// be cleared onto the stand's own line while an A320 was being pushed down it.
	//
	// ASSERTED AS A HOLD, not as a phase check: a test that merely named this contract would
	// pass on an implementation that took the wrong arm and claimed the ground some other
	// way, and this codebase's invariants say to measure rather than name.
	//
	// Build a two-agent network: one pushing off its stand, one asking for the lead-in.
	// AirsideTestFixtures provides the network builder; follow ServiceLinkTest.cpp's shape
	// for constructing a URoadNetwork with a guideline edge and two nodes.
	//
	// The assertions:
	//   1. after one Arbitrate, the occupancy table reports the lead-in edge held by the
	//      pushing agent;
	//   2. the second agent's StopWithin is finite - it was refused;
	//   3. the second agent's WaitingOn names the pushing agent by id.

	// NOTE TO THE IMPLEMENTER: build this against the live URoadNetwork API by copying the
	// fixture construction from GroundTrafficTest.cpp's Airside.Model.Traffic.BoxEntry
	// (:299-364), which already stands agents on one graph and asserts a refusal. Plant a
	// blocker with Traffic->OccupancyForTest().TryClaim(Claim, OutBlocker) and a phantom
	// AgentId, the way GroundTrafficTest.cpp:410-416 does; read it back with
	// Traffic->GetOccupancy().IsHeld(FTrafficResource::OfEdge(Id), 0). A real stand pose node
	// comes from FTestAirport::Build(Airframe, {.StandCount = 1}) then FTestAirport::Pose().
	// Do not invent a network shape; reuse those.
	return true;
}

#endif
```

**This is the one test whose body the plan does not write out in full**, because the network fixture must be copied from a live test rather than transcribed from memory — a plan that invents a `URoadNetwork` construction sequence is exactly how this project ships plans with real defects. **Corrected during execution: `JunctionClaimTest.cpp` is a road-geometry test with no `UGroundTraffic` and no agents at all.** The traffic/claims fixtures live in `GroundTrafficTest.cpp` (`Airside.Model.Traffic.BoxEntry`, :299-364) and `TrafficOccupancyTest.cpp`. Copy from there, and get a real stand pose node from `FTestAirport::Build(Airframe, {.StandCount = 1})` plus `FTestAirport::Pose(Stand)`. The three assertions above are the deliverable.

- [ ] **Step 7: Build twice, then run**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.ClaimCentre
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.PushbackHoldsLeadIn
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.Traffic
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.GraphRebuild
```
Expected: `0 failed, 0 crashed` on all four. The last two are the regression guard — Task 5 touched the files they cover.

- [ ] **Step 8: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RoadAgent.h `
        Plugins/Airside/Source/Airside/Private/Model/TrafficClaims.cpp `
        Plugins/Airside/Source/Airside/Private/Model/GroundTrafficRebuild.cpp `
        Plugins/Airside/Source/AirsideTests/Private/ClaimCentreTest.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PushbackClaimTest.cpp
git commit -m "fix(model): a reversed body claims the ground ahead of its nose gear, not behind it"
```

---

### Task 6: Pushback clearance and the straight-out skip

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/DeparturePlanner.h:14-27` (`EDepartureRefusal`)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/GroundTraffic.cpp:410-439` (`DepartAgent`)
- Test: `Plugins/Airside/Source/AirsideTests/Private/PushbackDepartTest.cpp` (create), `DepartAgentTest.cpp`

**Interfaces:**
- Consumes: `FPushbackRun::PlanPushDistance` (Task 3); `FRoadAgent::StartPushback` (Task 4); `FTrafficOccupancy::IsAnyHeld(TConstArrayView<FTrafficResource>, int32 Excluding, bool bCountOwnOccupied) const`.
- Produces: `EDepartureRefusal::PushbackBlocked`.

- [ ] **Step 1: Add the refusal**

In `DeparturePlanner.h`, after `NotParked`:

```cpp
	/** DepartAgent only: the ground the push needs is not free. Unlike every other refusal
	 *  here this one is about OTHER TRAFFIC and clears itself - see UGroundTraffic::DepartAgent
	 *  for why a push is granted whole rather than arbitrated as it goes. */
	PushbackBlocked,
```

- [ ] **Step 2: Write the failing test**

`Plugins/Airside/Source/AirsideTests/Private/PushbackDepartTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackClearanceTest,
	"Airside.Model.PushbackClearance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackClearanceTest::RunTest(const FString& Parameters)
{
	// A PUSH IS GRANTED WHOLE OR NOT AT ALL. A manoeuvring agent cannot replan - there is no
	// alternative back-out line - so if the deadlock resolver ever picked one it would have
	// no move to make. Granting the whole push up front makes it atomic and removes it as a
	// deadlock source, and it is what ground control actually does: pushback clearance is
	// granted or withheld, never half-granted.
	//
	// Build the departure fixture DepartAgentTest.cpp already uses, then:
	//   1. with the lead-in and the swing clear, DepartAgent returns None and the agent's
	//      phase is Manoeuvring, NOT Taxiing;
	//   2. with a second agent standing on the swing's ground, DepartAgent returns
	//      PushbackBlocked and the aeroplane is STILL Parked - nothing half-started;
	//   3. retire the blocker, call DepartAgent again, and it is granted - the refusal
	//      clears itself, unlike NoRoute.
	//
	// NOTE TO THE IMPLEMENTER: copy the network and agent construction from DepartAgentTest.cpp
	// rather than inventing one.
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPushbackStraightOutTest,
	"Airside.Model.PushbackStraightOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPushbackStraightOutTest::RunTest(const FString& Parameters)
{
	// A STAND WITH A WAY OUT FORWARD NEEDS NO PUSH. Measured off the plan rather than flagged
	// on the stand, so it answers a taxi-through stand, a player-drawn taxiway that happens to
	// run past a stand, and a graph rebuilt since the aeroplane parked, all at once.
	//
	// Build a departure plan whose first tangent is within StraightOutDegrees of the parked
	// heading, and assert DepartAgent takes the agent straight to Taxiing - it never enters
	// Manoeuvring at all.
	return true;
}

#endif
```

Both bodies are stubs for the same reason Task 5's was: the network fixture is copied from `DepartAgentTest.cpp`, not transcribed. The comments are the specification; the assertions listed are the deliverable.

- [ ] **Step 3: Run and watch them fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.Pushback
```
Expected: two tests run, both trivially passing as stubs. **That is not the failure state** — fill the bodies in from the fixtures first, run again, and confirm they fail against today's `DepartAgent` before writing Step 4.

- [ ] **Step 4: Implement `DepartAgent`**

Replace the body of `UGroundTraffic::DepartAgent` (`GroundTraffic.cpp:410`), keeping every existing `UE_LOG`:

```cpp
EDepartureRefusal UGroundTraffic::DepartAgent(int32 AgentId, const URoadNetwork& Network)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE || Agents[Index].Phase != EAgentPhase::Parked)
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("DepartAgent %d refused: %s"), AgentId,
			Index == INDEX_NONE ? TEXT("no such agent") : *UEnum::GetValueAsString(Agents[Index].Phase));
		return EDepartureRefusal::NotParked;
	}
	FRoadAgent& Agent = Agents[Index];
	if (!Agent.GoalNode.IsSet())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("DepartAgent %d refused: parked at no node."), AgentId);
		return EDepartureRefusal::NoRoute;
	}

	// From where it PARKED - its goal node - not from its polyline position: the search is
	// over the graph and the pose node is the graph's name for this stand.
	const FDeparturePlan Plan = DeparturePlanner::PlanAny(Network, Agent.GoalNode, Agent.Airframe, Agent.Class);
	UE_LOG(LogAirsideTraffic, Log, TEXT("DepartAgent %d: %s"), AgentId, *DeparturePlanner::Describe(Plan));
	if (!Plan.IsValid())
	{
		return Plan.Why;
	}

	// CAN IT SIMPLY DRIVE OUT? A MEASUREMENT OF THE GROUND AHEAD, not a flag on the stand -
	// which is why it answers a taxi-through stand, a taxiway a player drew past a stand, and
	// a graph rebuilt since this aeroplane parked, all with one question.
	FVector2D StartAt = FVector2D::ZeroVector;
	double OutTangent = 0.0;
	const bool bHaveTangent =
		GuidelineGeom::PointAtDistance(Plan.Route.Polyline, 0.0, StartAt, OutTangent);
	const double OffDegrees = bHaveTangent
		? FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(OutTangent - Agent.LastMotion.Heading)))
		: 180.0;

	if (bHaveTangent && OffDegrees <= Rules.StraightOutDegrees)
	{
		// THE MEASURED ANGLE IS IN THE LINE, because when a player asks why an aeroplane did
		// not push back, the angle that was measured IS the answer - and guessing it back out
		// of the geometry costs a PIE session.
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Agent %d departs straight out (%.0f deg off the parked heading)"),
			AgentId, OffDegrees);
		return RedirectAgent(AgentId, &Network, Plan.Route)
			? EDepartureRefusal::None : EDepartureRefusal::NoRoute;
	}

	// HOW MUCH GROUND THE PUSH WILL USE, from the SAME arithmetic the push itself will run -
	// see FPushbackRun::PlanPushDistance for why that is static and shared rather than
	// re-derived here.
	double BackDistance = 0.0;
	double PushDistance = 0.0;
	double TargetHeading = 0.0;
	if (!FPushbackRun::PlanPushDistance(Plan.Route, Rules.PushSwingLength,
		BackDistance, PushDistance, TargetHeading))
	{
		UE_LOG(LogAirsideTraffic, Warning,
			TEXT("DepartAgent %d refused: the route cannot be pushed along."), AgentId);
		return EDepartureRefusal::NoRoute;
	}

	// PUSHBACK CLEARANCE: granted whole, or withheld. See EDepartureRefusal::PushbackBlocked.
	if (!IsPushGroundFree(Agent, AgentId, Plan.Route, PushDistance, Network))
	{
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Agent %d cannot push back yet: %.0f uu of ground is not free."),
			AgentId, PushDistance);
		return EDepartureRefusal::PushbackBlocked;
	}

	const EAgentPhase Before = Agent.Phase;
	if (!Agent.StartPushback(Plan.Route, Agent.Airframe, Agent.LastMotion.Heading,
		Rules.PushSpeedFor(Agent.Airframe.PushbackNeed), Rules.PushAccel,
		Rules.PushSwingLength,
		Agent.Airframe.Engine.MaxRPM * Rules.PowerbackRPMFraction))
	{
		return EDepartureRefusal::NoRoute;
	}

	Agent.SetGoalFrom(Plan.Route);
	ArmDepartureIfRunway(Agent, Network, Plan.Route);
	ClaimGoalNodeAtDispatch(Agent, AgentId, Network);

	// THE NEED IS NAMED even though nothing branches on it yet: slice 1 pushes all three the
	// same way and nobody is doing the pushing, so the log line is the only place the gap is
	// visible at all.
	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Agent %d pushing back: %.0f uu, %s"), AgentId, PushDistance,
		*UEnum::GetValueAsString(Agent.Airframe.PushbackNeed));

	OnAgentPhaseChanged.Broadcast(AgentId, Before, Agent.Phase);
	return EDepartureRefusal::None;
}
```

Add the private helper beside it:

```cpp
bool UGroundTraffic::IsPushGroundFree(const FRoadAgent& Agent, int32 AgentId,
	const FRoutePlan& Plan, double PushDistance, const URoadNetwork& Network) const
{
	// Every edge and end node the push will touch, from the start to PushDistance. Built in
	// route order like the claim pass's own wanted list, though nothing here needs the order:
	// clearance is all-or-nothing, so the FIRST refusal and the LAST are the same answer.
	TArray<FTrafficResource> Wanted;
	for (const FRouteStep& Step : Plan.Steps)
	{
		Wanted.Add(FTrafficResource::ForEdge(Step.Edge));
		Wanted.Add(FTrafficResource::ForNode(Step.To));
		if (Step.EndDistance >= PushDistance)
		{
			break;
		}
	}

	// bCountOwnOccupied FALSE: the aeroplane is standing on its own stand node and the head of
	// its own lead-in, and refusing a push because the aeroplane is where it is would refuse
	// every push there has ever been.
	return !Occupancy.IsAnyHeld(Wanted, AgentId, /*bCountOwnOccupied*/ false);
}
```

**Check `FTrafficResource`'s real constructors before writing this** — `ForEdge`/`ForNode` are the names this plan assumes; if the live struct spells them differently, use the live spelling and say so in the commit.

Add `#include "Model/PushbackRun.h"` and `#include "Solve/GuidelineGeom.h"` to `GroundTraffic.cpp` if absent, and declare `IsPushGroundFree` in `GroundTraffic.h`'s private section.

- [ ] **Step 5: Build and run**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.Pushback
./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.DepartAgent
```
Expected: `0 failed, 0 crashed`. `DepartAgent` is the regression guard: its existing cases must still pass, and any that assert `Parked -> Taxiing` directly now need updating to expect `Manoeuvring` — **update the expectation, do not weaken the assertion.**

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/DeparturePlanner.h `
        Plugins/Airside/Source/Airside/Public/Model/GroundTraffic.h `
        Plugins/Airside/Source/Airside/Private/Model/GroundTraffic.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PushbackDepartTest.cpp `
        Plugins/Airside/Source/AirsideTests/Private/DepartAgentTest.cpp
git commit -m "feat(model): a push is cleared whole, and a stand with a way out forward needs none"
```

---

### Task 7: The flight board and the GDD

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/Flight.h:26-44`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/Flight.cpp:5-30`
- Modify: `Plugins/AirportOps/Source/AirportOpsTests/Private/FlightTest.cpp`
- Modify: `docs/AirportManagerGDD.md:133`

**Interfaces:**
- Consumes: `EAgentPhase::Manoeuvring` (Task 4).
- Produces: `EFlightPhase::Manoeuvring`, declared **between `Turnaround` and `TaxiOut`**.

- [ ] **Step 1: Write the failing test**

Append to `FlightTest.cpp`'s `RunTest`:

```cpp
	// THE DECLARATION ORDER IS LOAD-BEARING and this is what pins it. FlightPhaseFromAgent
	// decides taxi-in from taxi-out by asking whether the flight has reached Turnaround, so
	// Manoeuvring must sit AFTER Turnaround - inserting it before would read a taxi out as a
	// taxi in, with no compiler complaint at all.
	TestTrue(TEXT("Turnaround comes before Manoeuvring"),
		EFlightPhase::Turnaround < EFlightPhase::Manoeuvring);
	TestTrue(TEXT("and Manoeuvring before TaxiOut"),
		EFlightPhase::Manoeuvring < EFlightPhase::TaxiOut);

	TestEqual(TEXT("a manoeuvring agent is a manoeuvring flight"),
		FlightPhaseFromAgent(EAgentPhase::Manoeuvring, EFlightPhase::Turnaround),
		EFlightPhase::Manoeuvring);

	// AND THE TAXI AFTER IT IS STILL A TAXI OUT - the ordering test above says this must
	// hold, and this says it does. Without the Manoeuvring case, the switch's default would
	// leave the flight at Turnaround and the board would read "Turnaround" while the
	// aeroplane was visibly moving.
	TestEqual(TEXT("taxiing after a push is the taxi out"),
		FlightPhaseFromAgent(EAgentPhase::Taxiing, EFlightPhase::Manoeuvring),
		EFlightPhase::TaxiOut);
```

- [ ] **Step 2: Run and watch it fail**

```
./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.Flight -Project "C:\repos\AirportMgr2\AirportMgr.uproject"
```
Expected: compile error, `EFlightPhase::Manoeuvring` undeclared.

- [ ] **Step 3: Implement**

In `Flight.h`, insert between `Turnaround` and `TaxiOut`:

```cpp
	Turnaround,
	/** Coming off the stand - see EAgentPhase::Manoeuvring. Its position in this list is
	 *  load-bearing; see the enum's own comment. */
	Manoeuvring,
	TaxiOut,
```

and rewrite the enum's closing paragraph, which currently reads "Pushback, Diverted and Cancelled are deliberately ABSENT". It becomes:

```cpp
 * Manoeuvring is the aeroplane coming off the stand - the PHASE. The SERVICE that does it
 * for an aeroplane that cannot manage alone is pushback, and that is the job board's, which
 * does not exist yet. Diverted and Cancelled are still deliberately ABSENT: the sequencer
 * owns them and it does not exist either. A phase nothing can enter is a lie.
```

In `Flight.cpp`'s `FlightPhaseFromAgent`, add above the `Parked` case:

```cpp
	case EAgentPhase::Manoeuvring:
		return EFlightPhase::Manoeuvring;
```

- [ ] **Step 4: Run**

```
./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model -Project "C:\repos\AirportMgr2\AirportMgr.uproject"
```
Expected: `0 failed, 0 crashed`. `FlightBoardTest.cpp:144-157` walks a whole lifecycle through `OnAgentPhase`; it will need `Parked -> Manoeuvring -> Taxiing` inserted. Update it to match the real sequence.

- [ ] **Step 5: Amend the GDD**

`docs/AirportManagerGDD.md:133` currently reads:

> Every flight moves through one sequence: offered, accepted, inbound, landing, taxi-in, turnaround, pushback, taxi-out, departing, departed.

Change `pushback` to `manoeuvring`, and add a sentence to §7's Turnaround paragraph:

> **Manoeuvring** is the stage; **pushback** is the service that performs it. Small types reverse off a stand under their own power and need no tug at all; larger ones need a hand tug or a tug vehicle, and that is what makes a Pushback depot a choice rather than a tax.

Leave §9's "Pushback depot" and §11's pushback fee exactly as they are — both name the service, which is the right word for both.

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps/Public/Model/Flight.h `
        Plugins/AirportOps/Source/AirportOps/Private/Model/Flight.cpp `
        Plugins/AirportOps/Source/AirportOpsTests/Private/FlightTest.cpp `
        Plugins/AirportOps/Source/AirportOpsTests/Private/FlightBoardTest.cpp `
        docs/AirportManagerGDD.md
git commit -m "feat(ops): the board shows an aeroplane manoeuvring off its stand"
```

---

### Task 8: Taxi-through stands

Without this, Task 6's straight-out rule is correct but dormant: a stand pose node cannot have a second lead-in today.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h` (beside `PoseRole` :122)
- Modify: `Plugins/Airside/Source/Airside/Private/Build/AnchorLink.cpp:213-252` (the pose branch)
- Test: `Plugins/Airside/Source/AirsideTests/Private/TaxiThroughStandTest.cpp` (create)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `UEntityDefinition::bTaxiThrough`.

- [ ] **Step 1: Add the property**

In `UEntityDefinition`, after `PoseRole`:

```cpp
	/**
	 * This stand has pavement on BOTH sides: an aeroplane enters nose-first and leaves
	 * nose-first, so it needs no push at all.
	 *
	 * IT CHANGES THE GRAPH, NOT THE TRAFFIC MODEL. All it does is make FAnchorLink::Gather
	 * cast a SECOND lead-in ray, forward along +Heading, so the pose node has a way out as
	 * well as a way in. Whether a given aeroplane then needs a push is measured off the route
	 * it is actually given - see UGroundTraffic::DepartAgent - which is why nothing in Model/
	 * reads this flag.
	 *
	 * ONE LEAD-IN IS STILL THE RULE for every stand that does not set this. A second line
	 * into one nose-stop is normally a defect, and the anchor loop below refuses it by name.
	 */
	UPROPERTY(EditAnywhere) bool bTaxiThrough = false;
```

- [ ] **Step 2: Write the failing test**

`Plugins/Airside/Source/AirsideTests/Private/TaxiThroughStandTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Build/AnchorLink.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaxiThroughStandTest,
	"Airside.Build.TaxiThroughStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiThroughStandTest::RunTest(const FString& Parameters)
{
	// A TAXI-THROUGH STAND HAS A WAY OUT AS WELL AS A WAY IN. Without it the straight-out
	// rule in DepartAgent is correct but can never fire: FAnchorLink::Gather adds the pose
	// lead-in only when the node has NO incident edge, so a stand has exactly one line and
	// every departure has to reverse down it.
	//
	// Copy the network and entity construction from AnchorLinkTest.cpp, which already places
	// a stand between two guidelines and counts the lead-ins that joined. Then:
	//   1. a plain stand's pose node has ONE incident edge - today's behaviour, unchanged;
	//   2. the same stand with bTaxiThrough has TWO;
	//   3. the second one leaves along +Heading, not Heading + PI - assert the direction, not
	//      just the count, or a bug that cast the same ray twice would pass.
	return true;
}

#endif
```

Fill the body from `AnchorLinkTest.cpp`'s fixture. The three assertions are the deliverable; assertion 3 is the one that catches the likely bug.

- [ ] **Step 3: Run and watch it fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.TaxiThroughStand
```
After filling the body: expected FAIL on assertion 2 — one incident edge where two were wanted.

- [ ] **Step 4: Implement the second ray**

In `AnchorLink.cpp`'s pose branch (`:226` onward), the existing code builds one `FPendingLink` from `Out = Instance.Heading + UE_DOUBLE_PI`. Factor the body into a small lambda taking a direction, call it for `Heading + PI` as now, and call it a second time for `Heading` when `Instance.Definition->bTaxiThrough`:

```cpp
			// A SECOND RAY, FORWARD, for a stand with pavement on both sides. The first one
			// runs back out of the stand to the movement area, because +X faces the terminal
			// and casting the other way would have every stand trying to join a guideline
			// inside it. A taxi-through stand is the declared exception: there is no terminal
			// that side, so +X finds pavement too, and the aeroplane that parks here can drive
			// straight out instead of being pushed.
			//
			// THE FLAG IS ON THE DEFINITION AND NOT MEASURED HERE. Casting speculatively and
			// keeping whatever hit would join a stand to a taxiway on the far side of a
			// terminal building, which the graph has no way to know is not pavement.
			if (Instance.Definition->bTaxiThrough)
			{
				AddPoseLink(Instance.Heading);
			}
```

The existing comment block about `Heading + PI` must survive the refactor — move it with the code, do not re-derive it.

- [ ] **Step 5: Build twice, then run**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.TaxiThroughStand
./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.AnchorLink
./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.ServiceLink
```
Expected: `0 failed, 0 crashed` on all three. The last two are the regression guard — a plain stand and a depot pose must be untouched.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h `
        Plugins/Airside/Source/Airside/Private/Build/AnchorLink.cpp `
        Plugins/Airside/Source/AirsideTests/Private/TaxiThroughStandTest.cpp
git commit -m "feat(build): a taxi-through stand has a way out as well as a way in"
```

---

### Task 9: The composition test and the full run

The refactor contract's "every seam you introduce gets a test that fails if it is unwired", at the level of the composition rather than the model struct.

**Files:**
- Modify: `Plugins/Airside/Source/AirsideTests/Private/AgentActorTest.cpp`
- Test: the whole suite.

**Interfaces:**
- Consumes: everything above.
- Produces: nothing new.

- [ ] **Step 1: Write the failing composition test**

Append a new test to `AgentActorTest.cpp` (a distinct leaf name — `Airside.Present.AgentPushback`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentPushbackTest,
	"Airside.Present.AgentPushback",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentPushbackTest::RunTest(const FString& Parameters)
{
	// AT THE LEVEL OF THE COMPOSITION, not the model struct. Every assertion above this one
	// is on FPushbackRun or FRoadAgent with no world at all; this one spawns the actor, ticks
	// it, and watches an aeroplane actually leave a stand - which is the only thing that
	// fails if the phase is wired into the model but never reached from the driver, or if the
	// view is not posed during it.
	//
	// Copy the world, actor and dispatch construction from ArrivalDispatchTest.cpp:167, which
	// already ticks an arrival all the way to Parked. From there:
	//   1. call DepartAgent and tick;
	//   2. the agent passes through EAgentPhase::Manoeuvring - record every phase seen, and
	//      assert Manoeuvring appears BETWEEN Parked and Taxiing;
	//   3. the view is posed on every frame of it - no frame where the actor's transform is
	//      the world origin, which is this project's recurring failure and is what
	//      LastMotion's whole comment block exists to prevent;
	//   4. the aeroplane's position at the handover is further from the stand than where it
	//      parked - it actually moved, rather than merely changing phase.
	return true;
}
```

- [ ] **Step 2: Fill it in, run it, and confirm it fails before Task 4's code is present**

If Tasks 1–8 are already committed it will pass immediately; that is expected. To confirm it is not vacuous, comment out the `case EAgentPhase::Manoeuvring:` arm added in Task 4 Step 6, run, watch assertion 2 fail, then restore it.

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Present.AgentPushback
```

- [ ] **Step 3: Run the whole suite**

```
./Tools/Check-Architecture.ps1
./Tools/Run-AirsideTests.ps1
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2\AirportMgr.uproject" -Filter AirportOps
```
Read the `N test(s) run, N failed, N crashed` line on each. **A crashed test used to vanish and report green** — the count is the evidence, not the exit code.

- [ ] **Step 4: Count the logs and the comments**

The refactor contract applies to Task 5, which moved reads around in two files:

```bash
git diff origin/main --stat
grep -c "UE_LOG(" Plugins/Airside/Source/Airside/Private/Model/GroundTrafficRebuild.cpp
grep -c "UE_LOG(" Plugins/Airside/Source/Airside/Private/Model/TrafficClaims.cpp
```

Compare against `origin/main`'s counts for the same files. A drop means a log line was lost and has to be named and justified, or restored.

- [ ] **Step 5: PIE verification**

This is the step that turns "builds" into "works". Start the editor, open `M_Starter`, enter PIE, and dispatch an arrival to a stand. After the turnaround:

- **What to watch:** the aeroplane backs away from the stop mark in a straight line, then swings onto the taxiway and pulls forward. The nose wheel should NOT spin on the spot at any point.
- **What to grep** in `Saved/Logs/AirportMgr.log`:
  - `Agent N pushing back: <distance> uu, EPushbackNeed::...` — the phase was entered, and which need
  - `Push complete; taxiing out.` — the handover fired
  - **absence** of `Agent N departs straight out` for a normal nose-in stand
- **A screenshot is admissible evidence**: `python Tools/Mcp.py shot push.png editor` (`shot` alone grabs the level viewport, not PIE).

- [ ] **Step 6: Commit and open the PR**

```bash
git add -A
git commit -m "test(present): an aeroplane leaving a stand passes through the manoeuvre"
git push -u origin feature/pushback-and-manoeuvring
gh pr create --base main --title "Pushback and manoeuvring out of the stand" --body "..."
```

Fill the PR template: the build line, the test line (`N test(s) run, N failed, N crashed`), and — because Task 5 moved code — the log-line and comment-line deltas from Step 4.

---

## Self-Review

**Spec coverage.** §3 `EPushbackNeed` → Task 1. §4 `FPushbackRun`, `EPushPhase`, the two sub-phases, `PushDistance` past the corner, the handover → Tasks 3 and 4. §5 the straight-out measurement → Task 6; its content half → Task 8. §6 push and start, and the powerback thrust gate → Tasks 2 (the fraction), 3 (`bNeedsThrust`) and 4 (`StartPushback`, the gate). §7 the figures → Task 2. §8 the claim window, the `CentreOf` sign, the `Run` branch, the accessors, the rebuild, and clearance → Tasks 5 and 6. §9 the flight board → Task 7. §10 the four log lines → Tasks 4 (`Push complete`) and 6 (the other three; the fuel-service line already exists and needs no change). §11 every named test has a task. §12 the slice-2 seam is documentation, not code.

**Two spec tests have no separate task and are folded in deliberately:** `Pushback.WalksTheLeadIn` and `Pushback.BodyIsReversed` are both assertions inside `Airside.Model.PushbackRun` (Task 3 Step 2), because they share its fixture and a reviewer could not reject one without the other.

**Known soft spots, stated rather than hidden.** Four test bodies are specified as comments plus an assertion list rather than written out: Task 5 Step 6, Task 6 Step 2 (two tests), Task 8 Step 2, Task 9 Step 1. Each needs a `URoadNetwork` or a world fixture, and this project has shipped plans whose invented fixture code did not compile against the live headers. Each names the existing test file to copy the fixture from. That is a deliberate trade: a named source beats a plausible invention.

**Execution note, 2026-09-13.** The first named source was wrong: this plan originally sent Task 5 to `JunctionClaimTest.cpp`, which turns out to contain no `UGroundTraffic`, no agents and no claims - it is a `FRoadNetworkSolver` geometry test. Corrected to `GroundTrafficTest.cpp`. The lesson is the one the plan already states, applied to itself: a file named from memory is a plausible invention too.

**One name to check against the live header before writing it:** `FTrafficResource::ForEdge` / `ForNode` in Task 6 Step 4. The struct is in `Model/TrafficOccupancy.h`; use whatever it actually spells.
