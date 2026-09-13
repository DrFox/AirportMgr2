# Nose-Gear Steering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Aircraft steer like aircraft - the nose gear tracks the painted line, the mains cut inside the corner, and corner speed comes from lateral acceleration instead of a typed yaw rate.

**Architecture:** One follower, two steering laws selected by the airframe's own data (Strategy-by-parameters, not by subclass). A type carrying axle figures gets the kinematic bicycle model: the steered axle is constrained to the line, the heading error IS the steering angle, and yaw integrates at `v·sin(δ)/L`. A type without them keeps today's flat `MaxTurnRateDegPerSec` - a van pivots and genuinely is not a bicycle. Zero figures reproduces today's behaviour exactly, so types convert one at a time.

**Tech Stack:** UE 5.8 C++, `Airside` plugin (`Model/` plain UObjects and USTRUCTs, `Solve/` dependency-free geometry), `AirsideTests` automation tests, Python asset scripts under `Tools/Python`.

**Spec:** `docs/superpowers/specs/2026-09-13-nose-gear-steering-design.md`

## Global Constraints

- **The editor must be CLOSED for every build.** Live Coding holds the DLLs. You may `Get-Process -Name UnrealEditor | Stop-Process` when nothing is unsaved; never over unsaved work.
- **New `UPROPERTY`s need a full build**, not Live Coding. Tasks 1, 3, 4, 6 all add them - batch the rebuild at the end of each.
- **A NEW test .cpp needs TWO builds.** The first reports `Result: Succeeded` without compiling it, because the file list is gathered before the new file is seen. Task 2 and Task 5 create files; build twice there.
- Build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex`
- Tests: `./Tools/Run-AirsideTests.ps1` (add `-Filter Airside.Model` to narrow). **Never trust the exit code** - read the `N test(s) run, N failed, N crashed` line.
- `./Tools/Check-Architecture.ps1` runs first inside the test script and is the pre-commit lint.
- **Test names are `<Plugin>.<layer>.<Thing>` with DISTINCT leaf names.** UE's automation tree drops a bare-named parent once a dotted child exists, and only the run count catches it.
- **Comments explain WHY, and why the obvious alternative was rejected.** This codebase is dense with them deliberately; match it. Every `UE_LOG` survives a move.
- Angles: `Heading` is radians, yaw from +X. Distances are uu (1 uu = 1 cm). Speeds are uu/s.
- Commit messages end with:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01MHV2FfHcXTuYj1fYW49mRy`

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h` | `FGroundPerformance` gains the two steering figures; `FAirframe` gains the three geometry figures and `HasAxles()`; `FAgentMotion` gains `SteerAngleDegrees`. | 1, 6 |
| `Plugins/Airside/Source/Airside/Public/Entities/AircraftType.h` (`Airframe()`) | Copies the new figures out of the authored type. | 1 |
| `Plugins/Airside/Source/Airside/Public/Solve/RoadGeom.h` + `Private/Solve/RoadGeom.cpp` | `TrailPoint` - one rigid-offset-along-a-heading function, so a towed unit can call it again. | 2 |
| `Plugins/Airside/Source/Airside/Private/Model/RouteFollower.cpp` + its header | The two steering laws, the derived origin, the stop-point conversion, and the steer angle as state. | 3 |
| `Plugins/Airside/Source/Airside/Private/Model/SpeedProfile.cpp` + its header | Corner caps from lateral acceleration for axle types; `w*R` kept for pivot types. | 4 |
| `Plugins/Airside/Source/Airside/Private/Model/TrafficClaims.cpp` | Claim windows measured from the BODY CENTRE, which is no longer the origin. | 5 |
| `Plugins/Airside/Source/Airside/Private/Model/RoadAgent.cpp` (`DescribeMotion`) | Publishes the steer angle to the view. | 6 |
| `Plugins/Airside/Source/Airside/Public/Present/AirsideAgentAnim.h` + `.cpp` | `SteerAngleDegrees` for the AnimGraph. | 6 |
| `Plugins/Airside/Source/Airside/Private/Entities/AircraftType.cpp` | The Piper's measured axles; the A320 and 737 deliberately left on the pivot law. | 7 |
| `Tools/Python/build_plane2_type.py` | Authors plane2's axles, lock and lateral accel from the measured export. | 7 |
| `Plugins/Airside/Source/AirsideTests/Private/*` | Tests, per task. | all |

---

### Task 1: The figures, and the law selector

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h` (`FGroundPerformance` ~line 320-360, `FAirframe` ~line 505-515)
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/AircraftType.h` (`Airframe()`, ~line 137-156)
- Test: `Plugins/Airside/Source/AirsideTests/Private/AirframeAxlesTest.cpp` (new)

**Interfaces:**
- Consumes: nothing.
- Produces: `FAirframe::SteerAxleX`, `FAirframe::FixedAxleX`, `FAirframe::BodyCentreX`, `double FAirframe::Wheelbase() const`, `bool FAirframe::HasAxles() const`, `FGroundPerformance::MaxSteerDegrees`, `FGroundPerformance::MaxLateralAccelUu`. Every later task reads these.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/AirframeAxlesTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirframeAxlesTest,
	"Airside.Model.AirframeAxles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirframeAxlesTest::RunTest(const FString& Parameters)
{
	// 1. ZERO IS "NOT MEASURED", and it must mean the old law rather than a wheelbase of
	//    nothing - every vehicle in the game relies on that, so it is asserted first.
	const FAirframe Unmeasured;
	TestFalse(TEXT("an airframe nobody measured does not claim axles"), Unmeasured.HasAxles());
	TestEqual(TEXT("and its wheelbase is zero rather than a negative"),
		Unmeasured.Wheelbase(), 0.0);

	// 2. A conforming airframe: origin ON the steered axle, mains aft, so the wheelbase is
	//    the distance between them and is POSITIVE whichever way the figures are signed.
	FAirframe Conforming;
	Conforming.SteerAxleX = 0.0;
	Conforming.FixedAxleX = -454.3;
	TestTrue(TEXT("axle figures turn the geometric law on"), Conforming.HasAxles());
	TestEqual(TEXT("wheelbase is nose gear to main gear"), Conforming.Wheelbase(), 454.3, 0.01);

	// 3. The Piper's documented deviation - origin at the MAIN gear - measures the same
	//    wheelbase. If this ever fails, the two laws disagree about the same aeroplane.
	FAirframe Deviating;
	Deviating.SteerAxleX = 454.3;
	Deviating.FixedAxleX = 0.0;
	TestEqual(TEXT("a main-gear origin measures the same wheelbase"),
		Deviating.Wheelbase(), 454.3, 0.01);

	// 4. The figures survive the trip through an authored type, which is the only path the
	//    game uses - a field added to FAirframe and not copied in Airframe() is a figure
	//    that is authored and then silently dropped.
	UAircraftType* Type = NewObject<UAircraftType>();
	UAircraftType::BuildPiperMeridian(Type);
	Type->Footprint.NoseX = 385.1;
	Type->Footprint.TailX = -531.5;
	Type->SteerAxleX = 260.0;
	Type->FixedAxleX = 0.0;
	Type->Ground.MaxSteerDegrees = 55.0;
	Type->Ground.MaxLateralAccelUu = 200.0;

	const FAirframe Built = Type->Airframe();
	TestEqual(TEXT("SteerAxleX reaches the airframe"), Built.SteerAxleX, 260.0, 0.01);
	TestEqual(TEXT("FixedAxleX reaches the airframe"), Built.FixedAxleX, 0.0, 0.01);
	TestEqual(TEXT("the steering lock reaches the airframe"),
		Built.Ground.MaxSteerDegrees, 55.0, 0.01);
	TestEqual(TEXT("the lateral accel limit reaches the airframe"),
		Built.Ground.MaxLateralAccelUu, 200.0, 0.01);

	// BodyCentreX is DERIVED in Airframe() rather than authored, so it cannot disagree with
	// the footprint it comes from. Claims read it - see Task 5.
	TestEqual(TEXT("the body centre is derived from the footprint"),
		Built.BodyCentreX, (385.1 + -531.5) * 0.5, 0.01);

	return true;
}

#endif
```

- [ ] **Step 2: Build twice, then run the test to verify it fails**

A new test .cpp needs two builds; the first says `Result: Succeeded` without compiling it.

Run: the build line from Global Constraints, twice, then
`./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.AirframeAxles`
Expected: compile errors - `SteerAxleX` is not a member of `FAirframe`.

- [ ] **Step 3: Add the figures to `FGroundPerformance`**

In `RoadEntity.h`, after `MaxTurnRateDegPerSec` (~line 353):

```cpp
	/**
	 * The steering lock, degrees either side of straight ahead.
	 *
	 * Read ONLY by the rolling-steer law - see FAirframe::HasAxles. With a wheelbase it
	 * replaces MaxTurnRateDegPerSec outright rather than capping it: the airliner's
	 * hand-tuned 8 deg/s binds at every ordinary bend (a 30 m turn at 5 m/s needs 9.5), so
	 * keeping it as a ceiling would defeat the geometry it was standing in for.
	 *
	 * 60 degrees is tiller range. Rudder-pedal steering is nearer 10, which is a different
	 * manoeuvre and not what a taxi turn uses.
	 */
	UPROPERTY(EditAnywhere) double MaxSteerDegrees = 60.0;

	/**
	 * What a turn may pull sideways, uu/s^2. 147 is 0.15 g.
	 *
	 * THIS IS WHAT LIMITS CORNER SPEED once steering is geometric, and the reason is worth
	 * stating because it replaces a yaw-rate cap that looked like physics and was not.
	 * Required yaw is v/R and available yaw is v*sin(lock)/L - speed cancels, so whether a
	 * corner can be FOLLOWED is purely geometric (R >= L/sin(lock)) and says nothing about
	 * how fast. What actually stops an aircraft taking a 15 m bend at taxi speed is tyre
	 * side load and the cabin, which is this.
	 *
	 * Authored per type, not defaulted - a van corners far harder than a Twin Otter, and
	 * this is where that difference is DECIDED.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "1.0")) double MaxLateralAccelUu = 147.0;
```

`IsSet()` is NOT extended: a type with no steering figures is still a valid pivot-law airframe, and making these required would refuse every vehicle in the game.

- [ ] **Step 4: Add the geometry to `FAirframe`**

In `RoadEntity.h`, after `Wingspan` (~line 513):

```cpp
	/**
	 * The STEERED axle, uu along local +X. Nose gear on an aircraft, front axle on a vehicle.
	 *
	 * ZERO MEANS THE ORIGIN IS THAT AXLE, which is UAircraftType's documented local space:
	 * "origin at the NOSE GEAR, +X forward, +Y starboard". Non-zero is a declared deviation,
	 * and today exactly one type declares it - see BuildPiperMeridian, whose comment asked
	 * for this field by name: "that offset belongs here as a field and not as a constant at
	 * the call site".
	 *
	 * Named for the axle rather than the nose gear because vehicles use this struct too, and
	 * a truck has no nose gear.
	 */
	UPROPERTY(EditAnywhere) double SteerAxleX = 0.0;

	/** The FIXED axle, uu along local +X. Main gear, or a vehicle's rear axle. Negative on a
	 *  conforming airframe, since the mains sit aft of the nose gear. */
	UPROPERTY(EditAnywhere) double FixedAxleX = 0.0;

	/**
	 * The body's plan centre, uu along local +X, derived from the footprint in Airframe().
	 *
	 * NOT the origin, and that is the whole reason it exists: the traffic model's claim
	 * windows are measured from the agent's centre (see FClaimPass::FClaimWindow, "the
	 * agent's CENTRE, never its nose") and used Follower.Travelled for it. That was roughly
	 * true while origins sat mid-fuselage and became wrong by half a length the moment
	 * plane2 was re-exported about its nose gear.
	 */
	UPROPERTY(EditAnywhere) double BodyCentreX = 0.0;

	/**
	 * Nose gear to main gear, always positive, zero when unmeasured.
	 *
	 * THE LAW SELECTOR. An airframe with a wheelbase steers geometrically; one without keeps
	 * the flat MaxTurnRateDegPerSec. That is not a migration crutch - a van authored at 90
	 * deg/s would need 83 degrees of lock at its creep speed, so it is pivoting rather than
	 * steering, and a bicycle model would cripple every service vehicle in the game.
	 */
	double Wheelbase() const { return FMath::Abs(SteerAxleX - FixedAxleX); }

	bool HasAxles() const { return Wheelbase() > KINDA_SMALL_NUMBER; }
```

- [ ] **Step 5: Add the type's authored fields and copy them in `Airframe()`**

In `AircraftType.h`, beside `MainWheelRadius` (~line 72):

```cpp
	/** Steered axle in local X. Zero is the class convention - see FAirframe::SteerAxleX. */
	UPROPERTY(EditAnywhere) double SteerAxleX = 0.0;

	/** Fixed (main gear) axle in local X. Negative on a conforming airframe. */
	UPROPERTY(EditAnywhere) double FixedAxleX = 0.0;
```

In `Airframe()`, after `Out.Wingspan = Footprint.Wingspan;`:

```cpp
		Out.SteerAxleX = SteerAxleX;
		Out.FixedAxleX = FixedAxleX;

		// DERIVED, not authored: two numbers that must agree are one number. The footprint
		// already says where the nose and tail are, so the centre is arithmetic, and an
		// authored copy would drift the first time a mesh was re-exported.
		Out.BodyCentreX = (Footprint.NoseX + Footprint.TailX) * 0.5;
```

- [ ] **Step 6: Build and run the test to verify it passes**

Run: the build line, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.AirframeAxles`
Expected: `1 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h \
        Plugins/Airside/Source/Airside/Public/Entities/AircraftType.h \
        Plugins/Airside/Source/AirsideTests/Private/AirframeAxlesTest.cpp
git commit -m "feat(model): axle geometry and steering limits on FAirframe"
```

---

### Task 2: The trailing offset, as a free function

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/RoadGeom.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/RoadGeom.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/TrailPointTest.cpp` (new)

**Interfaces:**
- Consumes: nothing (`Solve/` is `CoreMinimal.h` only - no engine types beyond it).
- Produces: `FVector2D RoadGeom::TrailPoint(const FVector2D& At, double HeadingRadians, double OffsetAlongBody)`.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/TrailPointTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrailPointTest,
	"Airside.Solve.TrailPoint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrailPointTest::RunTest(const FString& Parameters)
{
	// Facing +X: a point 100 uu FORWARD along the body is +100 in X, and nothing in Y.
	const FVector2D Ahead = RoadGeom::TrailPoint(FVector2D(1000.0, 2000.0), 0.0, 100.0);
	TestEqual(TEXT("forward along +X moves X"), Ahead.X, 1100.0, 0.01);
	TestEqual(TEXT("and leaves Y alone"), Ahead.Y, 2000.0, 0.01);

	// Facing +Y (90 degrees): the same forward offset moves Y. This is the one that catches
	// a sin/cos swap, which looks correct at every multiple of 45 degrees.
	const FVector2D Turned = RoadGeom::TrailPoint(
		FVector2D(0.0, 0.0), FMath::DegreesToRadians(90.0), 100.0);
	TestEqual(TEXT("facing +Y, forward moves Y"), Turned.Y, 100.0, 0.01);
	TestEqual(TEXT("and not X"), Turned.X, 0.0, 0.01);

	// A NEGATIVE offset trails BEHIND, which is the direction every caller actually uses:
	// the origin is aft of the steered axle on a conforming airframe.
	const FVector2D Behind = RoadGeom::TrailPoint(
		FVector2D(0.0, 0.0), FMath::DegreesToRadians(90.0), -454.3);
	TestEqual(TEXT("a negative offset trails behind"), Behind.Y, -454.3, 0.01);

	// Zero offset is the identity - the conforming case, and it must be EXACT rather than
	// nearly so: every unmeasured airframe in the game goes through this path and must land
	// where it does today, bit for bit.
	const FVector2D Same = RoadGeom::TrailPoint(FVector2D(123.0, -456.0), 1.234, 0.0);
	TestTrue(TEXT("zero offset is exactly the identity"),
		Same.X == 123.0 && Same.Y == -456.0);

	return true;
}

#endif
```

- [ ] **Step 2: Build twice, then run to verify it fails**

Run: the build line twice, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve.TrailPoint`
Expected: FAIL - `TrailPoint` is not a member of `RoadGeom`.

- [ ] **Step 3: Implement it**

In `RoadGeom.h`:

```cpp
	/**
	 * A point rigidly offset from At, along the body axis of something facing Heading.
	 *
	 * ONE FUNCTION RATHER THAN INLINE ARITHMETIC, because it is about to have a second
	 * caller and then a third. The follower derives an airframe's origin from its steered
	 * axle with it; a towed unit - a tug on a bar, a baggage train - is the same function
	 * applied again with the hitch as its lead point. Nothing in the game is articulated
	 * yet, and this is the shape that lets it be without a rewrite.
	 *
	 * Positive OffsetAlongBody is forward. Zero returns At unchanged and EXACTLY so: every
	 * airframe with no axle figures takes that path and must not move by a float epsilon.
	 */
	FVector2D TrailPoint(const FVector2D& At, double HeadingRadians, double OffsetAlongBody);
```

In `RoadGeom.cpp`:

```cpp
FVector2D RoadGeom::TrailPoint(const FVector2D& At, double HeadingRadians, double OffsetAlongBody)
{
	// Guarded rather than computed, so the conforming case is the identity and not a pair of
	// trig calls whose rounding could nudge an agent that has no offset at all.
	if (FMath::IsNearlyZero(OffsetAlongBody))
	{
		return At;
	}

	return At + FVector2D(FMath::Cos(HeadingRadians), FMath::Sin(HeadingRadians))
		* OffsetAlongBody;
}
```

- [ ] **Step 4: Build and run to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve.TrailPoint`
Expected: `1 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Solve/RoadGeom.h \
        Plugins/Airside/Source/Airside/Private/Solve/RoadGeom.cpp \
        Plugins/Airside/Source/AirsideTests/Private/TrailPointTest.cpp
git commit -m "feat(solve): TrailPoint, a rigid offset along a body axis"
```

---

### Task 3: The two steering laws in the follower

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RouteFollower.h` (state, ~line 84)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RouteFollower.cpp` (`Advance`, lines 55-135)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/TurnRateTest.cpp` (the corner-speed assertion at ~line 224)
- Test: `Plugins/Airside/Source/AirsideTests/Private/NoseGearTracksTest.cpp` (new)

**Interfaces:**
- Consumes: `FAirframe::HasAxles()`, `Wheelbase()`, `SteerAxleX`, `Ground.MaxSteerDegrees` (Task 1); `RoadGeom::TrailPoint` (Task 2).
- Produces: `FRouteFollower::SteerDegrees` (signed, degrees) and `FRouteFollower::YawRateDegPerSec`, both public state read by Task 6. `Advance`'s signature is UNCHANGED - `OutPosition` still reports the airframe ORIGIN, so no caller changes.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/NoseGearTracksTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RouteFollower.h"
#include "Model/RouteSearch.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"   // TrailPoint, to measure where the mains ended up

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.
	constexpr double NoseGearFrame = 1.0 / 60.0;

	FRoutePlan NoseGearPlan(const TArray<FVector2D>& Points)
	{
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = Points;
		Plan.Length = GuidelineGeom::PolylineLength(Points);
		return Plan;
	}

	/** A quarter circle of radius R, sampled finely enough not to be a corner. */
	TArray<FVector2D> NoseGearArc(double R)
	{
		TArray<FVector2D> Points;
		Points.Add(FVector2D(-2000.0, -R));
		for (int32 Step = 0; Step <= 24; ++Step)
		{
			const double Angle = -HALF_PI + (HALF_PI * Step) / 24.0;
			Points.Add(FVector2D(R * FMath::Cos(Angle), R + R * FMath::Sin(Angle)));
		}
		Points.Add(FVector2D(R + 2000.0, R));
		return Points;
	}

	/** A plane2-shaped airframe: origin on the nose gear, mains 4.54 m aft. */
	FAirframe NoseGearTwinOtter()
	{
		FAirframe Airframe;
		Airframe.Ground = TestAirframes::Piper().Ground;
		Airframe.Ground.MaxSteerDegrees = 60.0;
		Airframe.Ground.MaxLateralAccelUu = 147.0;
		Airframe.SteerAxleX = 0.0;
		Airframe.FixedAxleX = -454.3;
		return Airframe;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNoseGearTracksTest,
	"Airside.Model.NoseGearTracks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNoseGearTracksTest::RunTest(const FString& Parameters)
{
	// THE REPORTED DEFECT, PINNED: "it keeps the centre of its rear wheels on the route line
	// rather than the nose wheel". The steered axle must be ON the line and the mains INSIDE
	// it - a trailing wheel cuts the corner, which is the whole visible difference.
	const double R = 3000.0;
	const FAirframe Airframe = NoseGearTwinOtter();

	FRouteFollower Follower;
	Follower.Start(NoseGearPlan(NoseGearArc(R)), Airframe, Airframe.Ground.Taxi.SpeedCap);

	double WorstSteerOffLine = 0.0;
	double MainsInsideBy = 0.0;
	bool bMeasuredInTheTurn = false;

	for (int32 Frame = 0; Frame < 4000; ++Frame)
	{
		FVector2D Origin = FVector2D::ZeroVector;
		double Heading = 0.0;
		if (!Follower.Advance(NoseGearFrame, Airframe, Origin, Heading))
		{
			break;
		}

		// The steered axle is the origin for this airframe (SteerAxleX is zero), so the
		// reported position IS the point that must be on the line.
		FVector2D OnLine = FVector2D::ZeroVector;
		double LineHeading = 0.0;
		GuidelineGeom::PointAtDistance(Follower.Plan.Polyline, Follower.Travelled,
			OnLine, LineHeading);
		WorstSteerOffLine = FMath::Max(WorstSteerOffLine, FVector2D::Distance(Origin, OnLine));

		// Measured only in the arc proper, where "inside" has a meaning: the centre of the
		// circle is (0, R), so inside is nearer to it than the line is.
		const bool bInTheArc = Origin.X > 0.0 && Origin.Y < R;
		if (bInTheArc)
		{
			const FVector2D Mains = RoadGeom::TrailPoint(Origin, Heading, Airframe.FixedAxleX);
			const double MainsRadius = FVector2D::Distance(Mains, FVector2D(0.0, R));
			MainsInsideBy = FMath::Max(MainsInsideBy, R - MainsRadius);
			bMeasuredInTheTurn = true;
		}
	}

	TestTrue(TEXT("the arc was actually entered, so the measurements below mean something"),
		bMeasuredInTheTurn);

	// ON the line, not near it: the steered axle is CONSTRAINED to the polyline rather than
	// chasing it, so any real drift here is a derivation bug, not a tracking error.
	TestTrue(FString::Printf(
		TEXT("the steered axle stays on the line (worst %.2f uu)"), WorstSteerOffLine),
		WorstSteerOffLine < 1.0);

	// INSIDE BY THE GEOMETRY, not merely inside. A body of wheelbase L trailing round a
	// radius R sits at sqrt(R^2 - L^2) from the centre, so it cuts in by R - that, which is
	// 34 uu here. Asserting only "inside" would pass on a body that barely leaned.
	const double Expected = R - FMath::Sqrt(R * R - Airframe.Wheelbase() * Airframe.Wheelbase());
	TestTrue(FString::Printf(
		TEXT("the mains cut inside by about %.1f uu, measured %.1f"), Expected, MainsInsideBy),
		FMath::Abs(MainsInsideBy - Expected) < 5.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnmeasuredAirframeUnchangedTest,
	"Airside.Model.UnmeasuredAirframeUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUnmeasuredAirframeUnchangedTest::RunTest(const FString& Parameters)
{
	// EVERY VEHICLE IN THE GAME TAKES THIS PATH. A van is authored at 90 deg/s, which needs
	// 83 degrees of lock at its creep speed - it pivots, and the geometric law would cripple
	// it. So an airframe with no axles must behave EXACTLY as it does today.
	FAirframe Pivot;
	Pivot.Ground = TestAirframes::Piper().Ground;
	Pivot.Ground.MaxTurnRateDegPerSec = 90.0;

	FRouteFollower Follower;
	Follower.Start(NoseGearPlan({FVector2D(0.0, 0.0), FVector2D(4000.0, 0.0),
		FVector2D(4000.0, 4000.0)}), Pivot, Pivot.Ground.Taxi.SpeedCap);

	double Worst = 0.0;
	double Previous = Follower.Heading;
	for (int32 Frame = 0; Frame < 2000; ++Frame)
	{
		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		if (!Follower.Advance(NoseGearFrame, Pivot, At, Heading))
		{
			break;
		}
		Worst = FMath::Max(Worst, FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(Heading - Previous))) / NoseGearFrame);
		Previous = Heading;
	}

	// The flat rate still governs, and the SPEED does not enter it - which is exactly what
	// makes it a pivot law rather than a bicycle one.
	TestTrue(FString::Printf(TEXT("the flat 90 deg/s still governs (peak %.1f)"), Worst),
		Worst <= 90.0 + 1.0);
	TestTrue(TEXT("and it is actually used, not merely not exceeded"), Worst > 45.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSteerNeverExceedsLockTest,
	"Airside.Model.SteerNeverExceedsLock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSteerNeverExceedsLockTest::RunTest(const FString& Parameters)
{
	// A GENUINE 90 degree corner - past the ~20 degrees GuidelineGeom treats as a sampling
	// artefact, so the direction of travel really does change instantly and the lock is the
	// only thing standing between the model and an aeroplane that swaps ends.
	FAirframe Airframe = NoseGearTwinOtter();
	Airframe.Ground.MaxSteerDegrees = 45.0;

	FRouteFollower Follower;
	Follower.Start(NoseGearPlan({FVector2D(0.0, 0.0), FVector2D(4000.0, 0.0),
		FVector2D(4000.0, 4000.0)}), Airframe, Airframe.Ground.Taxi.SpeedCap);

	double WorstSteer = 0.0;
	for (int32 Frame = 0; Frame < 4000; ++Frame)
	{
		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		if (!Follower.Advance(NoseGearFrame, Airframe, At, Heading))
		{
			break;
		}
		WorstSteer = FMath::Max(WorstSteer, FMath::Abs(Follower.SteerDegrees));
	}

	TestTrue(FString::Printf(TEXT("steering never passes the lock (worst %.1f)"), WorstSteer),
		WorstSteer <= 45.0 + 0.01);
	TestTrue(TEXT("and the corner did demand full lock, so the clamp was exercised"),
		WorstSteer > 44.0);

	return true;
}

#endif
```

- [ ] **Step 2: Build twice, then run to verify it fails**

Run: the build line twice, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.NoseGear`
Expected: compile errors - `SteerDegrees` is not a member of `FRouteFollower`.

- [ ] **Step 3: Add the follower's new state**

In `RouteFollower.h`, after `Heading` (~line 84):

```cpp
	/**
	 * The steering angle this frame, degrees, signed the way Heading turns.
	 *
	 * STATE RATHER THAN AN OUT-PARAMETER, because Advance's signature is the one FAgentMotion
	 * exists to stop growing: "SetPose used to take a position, a heading, a surface height,
	 * an altitude and a pitch... Eight arguments in a row is a signature nobody can call
	 * correctly."
	 *
	 * It is the INPUT to the motion, not a description of it - the view animates the nose
	 * gear from this exact number, so what the player sees the wheel doing is what is
	 * turning the aeroplane. Zero on a pivot-law airframe, which has no steered wheel.
	 */
	UPROPERTY() double SteerDegrees = 0.0;

	/** The yaw actually applied this frame, degrees per second. Kept beside SteerDegrees so
	 *  a reader can see the two agree; nothing outside the view reads it. */
	UPROPERTY() double YawRateDegPerSec = 0.0;
```

- [ ] **Step 4: Replace the slew block in `Advance`**

In `RouteFollower.cpp`, replace lines 88-98 (from `const double Error` through the `Crab` line) with:

```cpp
	const double Error = FMath::UnwindRadians(LineHeading - Heading);

	// HOW FAR THE NOSE MAY COME ROUND THIS FRAME, and there are two laws because there are
	// two kinds of vehicle - see FAirframe::HasAxles.
	//
	// ROLLING-STEER is the kinematic bicycle model, and the diff is smaller than it sounds
	// because the steering angle was already being computed here: Error IS the angle between
	// the body axis and the direction the steered axle is being asked to travel. The only
	// change is which limit clamps it and what that yields.
	//
	//   d    = clamp(Error, +/- lock)
	//   Step = v * sin(d) / L * dt
	//
	// sin rather than tan because Speed is the STEERED axle's speed along the line (that is
	// what Travelled measures), not the rear axle's. Using the rear-axle form here would
	// overstate the yaw by 1/cos(d) - invisible at small angles, double at full lock.
	//
	// No lookahead and no gain to tune: the steered axle is CONSTRAINED to the line rather
	// than chasing it, so there is no lateral error to feed back and nothing to oscillate.
	//
	// PIVOT keeps the flat rate, and permanently. A van is authored at 90 deg/s with the
	// note "a van CAN pivot"; at its 0.5 m/s creep that needs 83 degrees of lock, so it is
	// not steering at all and a geometric law would cripple it.
	double MaxStep = 0.0;
	if (InAirframe.HasAxles())
	{
		const double Lock = FMath::DegreesToRadians(FMath::Max(0.0, Ground.MaxSteerDegrees));
		const double Steer = FMath::Clamp(Error, -Lock, Lock);
		SteerDegrees = FMath::RadiansToDegrees(Steer);
		MaxStep = FMath::Abs(Speed * FMath::Sin(Steer) / InAirframe.Wheelbase()) * DeltaSeconds;
	}
	else
	{
		// No steered wheel to show, so the view must not draw one turning.
		SteerDegrees = 0.0;
		MaxStep = FMath::DegreesToRadians(Ground.MaxTurnRateDegPerSec) * DeltaSeconds;
	}

	// Unwound first, so a turn across the +/-PI seam is taken the short way round rather
	// than very nearly all the way about. Step kept apart from the slew itself (RoadGeom::
	// SlewAngle) because Crab below needs the exact clamped step, not just the new heading.
	const double Step = FMath::Clamp(Error, -MaxStep, MaxStep);
	Heading = RoadGeom::SlewAngle(Heading, LineHeading, MaxStep);
	YawRateDegPerSec = DeltaSeconds > 0.0
		? FMath::RadiansToDegrees(Step) / DeltaSeconds
		: 0.0;

	// WHAT COULD NOT BE TAKEN OUT THIS FRAME, which is the crab the player is now looking
	// at. Measured after the slew rather than before it: an airframe that CAN make the turn
	// has no reason to slow for it, and taking the pre-slew error instead would have shaved
	// a few percent off the speed of every agent on every gentle bend for nothing.
	const double Crab = FMath::RadiansToDegrees(FMath::Abs(Error - Step));
```

- [ ] **Step 5: Derive the origin and convert the stop point**

Still in `Advance`: the stop point is fixed before the move, so convert it there. Replace the `StopAt` line (~line 69) with:

```cpp
	// The stop point in route distance, fixed BEFORE the move: StopWithin was measured from
	// where the agent was when the arbiter looked, and re-measuring it after moving would
	// let the agent creep past it one frame at a time.
	//
	// CONVERTED TO THE STEERED AXLE'S FRAME, because Travelled measures that axle and every
	// authored stop - a holding position, a stand's park point - was measured against the
	// ORIGIN. A no-op for the conforming airframes (SteerAxleX zero) and the reason the
	// Piper, whose origin is its main-gear axle, still parks where it always did.
	const double StopAt = FMath::Min(Plan.Length,
		Travelled + FMath::Max(0.0, StopWithin) + InAirframe.SteerAxleX);
```

And at the end, replace `OutHeading = Heading;` with:

```cpp
	// WHAT THE CALLER GETS IS THE ORIGIN, unchanged in meaning from before this law existed:
	// ARoadAgentActor::SetPose puts it on the ground and stands compose against it. The
	// STEERED AXLE is what rides the line, and for every conforming airframe those are the
	// same point, so TrailPoint returns OutPosition untouched.
	OutPosition = RoadGeom::TrailPoint(OutPosition, Heading, -InAirframe.SteerAxleX);
	OutHeading = Heading;
```

- [ ] **Step 6: Update the corner assertion in `TurnRateTest.cpp`**

That test's comment at ~line 224 says *"a turn is v/R, so the fastest this radius can be taken at is MaxTurnRate times R. At 20 deg/s and 25 m that is 8.7 m/s"*. Its fixture has no axle figures, so it still exercises the pivot law and still passes - but the comment now describes only half the world. Extend it:

```cpp
	// THE CAP IS THE GEOMETRY, and it is arithmetic rather than a tuned number: a turn is
	// v/R, so the fastest this radius can be taken at is MaxTurnRate times R. At 20 deg/s
	// and 25 m that is 8.7 m/s, against a 10 m/s taxi speed.
	//
	// THIS FIXTURE HAS NO AXLE FIGURES, deliberately: it is the PIVOT law's test and must
	// keep asserting the flat rate. What an airframe with a wheelbase does instead is
	// Airside.Model.CornerSpeedIsLateralAccel, where the cap is sqrt(a*R) and this yaw-rate
	// cap does not apply at all.
```

- [ ] **Step 7: Write the stop-point test**

The conversion in Step 5 protects every authored holding position and park point. Untested, it
is a silent 2.6 m shift on the one type that deviates. Append to `NoseGearTracksTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAuthoredStopPointsDoNotMoveTest,
	"Airside.Model.AuthoredStopPointsDoNotMove",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAuthoredStopPointsDoNotMoveTest::RunTest(const FString& Parameters)
{
	// Travelled measures the STEERED AXLE, and every authored stop - a holding position, a
	// stand's park point - was measured against the ORIGIN. On a type whose origin is not
	// its steered axle, forgetting to convert parks the aeroplane a wheelbase short, which
	// looks like a level-authoring mistake and is not.
	const TArray<FVector2D> Straight{FVector2D(0.0, 0.0), FVector2D(20000.0, 0.0)};
	const double StopWithin = 5000.0;

	auto ParkedOriginX = [&](double SteerAxleX)
	{
		FAirframe Airframe = NoseGearTwinOtter();
		Airframe.SteerAxleX = SteerAxleX;
		Airframe.FixedAxleX = SteerAxleX - 454.3;

		FRouteFollower Follower;
		Follower.Start(NoseGearPlan(Straight), Airframe, 0.0);

		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		// Long enough to accelerate, run and brake to a stand-still at the cap.
		for (int32 Frame = 0; Frame < 6000; ++Frame)
		{
			Follower.Advance(NoseGearFrame, Airframe, StopWithin, At, Heading);
		}
		return At.X;
	};

	const double Conforming = ParkedOriginX(0.0);
	const double Deviating = ParkedOriginX(260.0);   // the Piper's declared offset

	TestTrue(FString::Printf(TEXT("a conforming airframe stops at the authored point (%.1f)"),
		Conforming), FMath::Abs(Conforming - StopWithin) < 5.0);

	// THE POINT OF THE CONVERSION: the two park their ORIGINS in the same place, despite
	// measuring Travelled from different points on the aeroplane.
	TestTrue(FString::Printf(
		TEXT("a declared deviation parks its origin in the same place (%.1f vs %.1f)"),
		Deviating, Conforming), FMath::Abs(Deviating - Conforming) < 5.0);

	return true;
}
```

- [ ] **Step 8: Build and run the model tests**

Run: the build line, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model`
Expected: `0 failed, 0 crashed`, and `TurnRate`, `FollowerHeading`, `StopWithin`, `LongFrameNeverPassesTheStop` all still pass - they use axle-free fixtures, so the pivot law carries them unchanged.

- [ ] **Step 9: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RouteFollower.h \
        Plugins/Airside/Source/Airside/Private/Model/RouteFollower.cpp \
        Plugins/Airside/Source/AirsideTests/Private/NoseGearTracksTest.cpp \
        Plugins/Airside/Source/AirsideTests/Private/TurnRateTest.cpp
git commit -m "feat(model): the steered axle tracks the line and the body trails it"
```

---

### Task 4: Corner speed from lateral acceleration

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/SpeedProfile.h` (`Build`, line 44)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/SpeedProfile.cpp` (lines 25-100)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RouteFollower.cpp` (lines 21 and 143, the two `Profile.Build` calls)
- Test: `Plugins/Airside/Source/AirsideTests/Private/NoseGearTracksTest.cpp` (add to the file from Task 3)

**Interfaces:**
- Consumes: `FAirframe::HasAxles()`, `Wheelbase()`, `Ground.MaxLateralAccelUu`, `Ground.MaxSteerDegrees`.
- Produces: `FSpeedProfile::Build(const TArray<FVector2D>& Points, const FAirframe& Airframe)` - signature CHANGED from taking `FGroundPerformance`.

- [ ] **Step 1: Write the failing tests**

Append to `NoseGearTracksTest.cpp` (before the final `#endif`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerSpeedIsLateralAccelTest,
	"Airside.Model.CornerSpeedIsLateralAccel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCornerSpeedIsLateralAccelTest::RunTest(const FString& Parameters)
{
	// WHY THE TURN IS SLOW is the question this answers. It used to be a flat 10 deg/s,
	// which is not a fact about any aeroplane; it is now sqrt(a*R), which is tyre side load
	// and the cabin - the thing that actually stops a taxiing aircraft cornering faster.
	const double R = 3000.0;
	FAirframe Airframe = NoseGearTwinOtter();
	Airframe.Ground.MaxLateralAccelUu = 147.0;   // 0.15 g

	FRouteFollower Follower;
	Follower.Start(NoseGearPlan(NoseGearArc(R)), Airframe, 0.0);

	double FastestOnTheStraight = 0.0;
	double FastestInTheArc = 0.0;
	for (int32 Frame = 0; Frame < 6000; ++Frame)
	{
		FVector2D At = FVector2D::ZeroVector;
		double Heading = 0.0;
		if (!Follower.Advance(NoseGearFrame, Airframe, At, Heading))
		{
			break;
		}
		if (At.X < -500.0)
		{
			FastestOnTheStraight = FMath::Max(FastestOnTheStraight, Follower.Speed);
		}
		else if (At.X > 500.0 && At.Y < R - 500.0)
		{
			FastestInTheArc = FMath::Max(FastestInTheArc, Follower.Speed);
		}
	}

	// It got going first, or "slower in the turn" would be true of an aeroplane that never
	// moved.
	TestTrue(FString::Printf(TEXT("it reached taxi speed on the straight (%.0f uu/s)"),
		FastestOnTheStraight), FastestOnTheStraight > Airframe.Ground.Taxi.SpeedCap * 0.9);

	// sqrt(147 * 3000) = 664 uu/s. The old law gave 10 deg/s * 3000 = 524, so this is the
	// turn getting FASTER for a stated reason rather than a raised number.
	const double Expected = FMath::Sqrt(Airframe.Ground.MaxLateralAccelUu * R);
	TestTrue(FString::Printf(TEXT("the arc is taken at about sqrt(a*R) = %.0f, measured %.0f"),
		Expected, FastestInTheArc), FMath::Abs(FastestInTheArc - Expected) < 60.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerTighterThanLockCrawlsTest,
	"Airside.Model.CornerTighterThanLockCrawls",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCornerTighterThanLockCrawlsTest::RunTest(const FString& Parameters)
{
	// A corner tighter than R = L/sin(lock) cannot be followed AT ANY SPEED - the geometry,
	// not the pace, is what refuses it. The honest answer there is the crawl the model
	// already specifies: "a turn that cannot be made at speed is made at a crawl, which is
	// what a pilot riding the brakes against idle thrust actually does."
	FAirframe Airframe = NoseGearTwinOtter();
	Airframe.Ground.MaxSteerDegrees = 10.0;   // rudder-pedal range: L/sin(10) = 26 m

	FSpeedProfile Profile;
	Profile.Build(NoseGearArc(1000.0), Airframe);   // a 10 m radius, far inside that

	// Sampled inside the arc rather than at its ends, where the smoothed vertex headings
	// average with the straights and legitimately allow more speed.
	const double Limit = Profile.LimitAt(GuidelineGeom::PolylineLength(NoseGearArc(1000.0)) * 0.5);
	TestTrue(FString::Printf(TEXT("an impossible corner crawls (%.0f uu/s)"), Limit),
		Limit <= Airframe.Ground.MinTaxiSpeed + 1.0);

	return true;
}
```

- [ ] **Step 2: Build and run to verify they fail**

Run: the build line, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.Corner`
Expected: compile error - `Build` takes an `FGroundPerformance`, not an `FAirframe`.

- [ ] **Step 3: Change `Build`'s signature**

In `SpeedProfile.h` line 44:

```cpp
	/**
	 * ...(keep the existing comment)...
	 *
	 * TAKES THE AIRFRAME, not just its ground performance (was FGroundPerformance): the
	 * corner cap now depends on the WHEELBASE as well as the figures, because whether a
	 * corner can be steered at all is geometric. Passing the bundle is this codebase's "one
	 * struct per thing" - the alternative was a second parameter that some caller would one
	 * day forget to keep in step.
	 */
	void Build(const TArray<FVector2D>& Points, const FAirframe& Airframe);
```

In `RouteFollower.cpp`, both call sites (lines 21 and 143) become:

```cpp
	Profile.Build(Plan.Polyline, InAirframe);
```

- [ ] **Step 4: Replace the span cap**

In `SpeedProfile.cpp`, `Build` opens with `const FGroundPerformance& Ground = ...`; add that line at the top (`const FGroundPerformance& Ground = Airframe.Ground;`) so the rest of the function is untouched, then replace the span-cap loop (lines 56-73):

```cpp
	// SPAN CAPS, from curvature. Heading changes by so many radians over so many uu, so the
	// radius is Length/Turn - v = wR written without ever naming a radius, which is what
	// lets it work on a polyline that is not an arc.
	//
	// WHAT LIMITS THE SPEED THERE depends on which law steers this airframe.
	//
	// PIVOT: the yaw rate is all there is, so the cap is MaxTurnRate * R, exactly as before.
	//
	// ROLLING-STEER: the yaw rate does NOT limit it. Required yaw is v/R and available yaw
	// is v*sin(lock)/L, so speed cancels and a corner is either followable at every speed or
	// at none: R >= L/sin(lock). For plane2 that threshold is 5.2 m, tighter than any
	// taxiway bend. What is left is the physical limit a yaw-rate cap was always standing in
	// for - lateral acceleration, sqrt(a*R), tyre side load and the cabin.
	const double MaxTurnRate = FMath::DegreesToRadians(Ground.MaxTurnRateDegPerSec);
	const double Lock = FMath::Sin(FMath::DegreesToRadians(
		FMath::Clamp(Ground.MaxSteerDegrees, 0.0, 90.0)));
	const double TightestFollowable = (Airframe.HasAxles() && Lock > KINDA_SMALL_NUMBER)
		? Airframe.Wheelbase() / Lock
		: 0.0;

	SpanCaps.SetNumUninitialized(Count - 1);
	for (int32 Span = 0; Span + 1 < Count; ++Span)
	{
		const double Length = Distances[Span + 1] - Distances[Span];
		const double Turn = FMath::Abs(FMath::UnwindRadians(Arriving[Span + 1] - Leaving[Span]));

		double Cap = Ground.Taxi.SpeedCap;
		if (Length > 0.0 && Turn > CornerEpsilon)
		{
			const double Radius = Length / Turn;
			if (!Airframe.HasAxles())
			{
				Cap = FMath::Min(Cap, MaxTurnRate * Radius);
			}
			else if (Radius < TightestFollowable)
			{
				// The lock cannot hold this line at any speed. Crawl, wear the crab, and let
				// the follower's own crab term do the rest - the same answer the vertex caps
				// give an instant corner.
				Cap = Ground.MinTaxiSpeed;
			}
			else
			{
				Cap = FMath::Min(Cap, FMath::Sqrt(
					FMath::Max(0.0, Ground.MaxLateralAccelUu) * Radius));
			}
		}

		// Never below the creep speed. A span this tight is one the aircraft has to crab
		// through, and crawling is the slowest an aeroplane may do that at - see
		// FGroundPerformance::MinTaxiSpeed.
		SpanCaps[Span] = FMath::Max(Cap, Ground.MinTaxiSpeed);
	}
```

The vertex caps below are unchanged: an instant corner has no radius, is followable at no speed under either law, and already falls to `MinTaxiSpeed`.

- [ ] **Step 5: Build and run**

Run: the build line, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model`
Expected: `0 failed, 0 crashed`. `Airside.Model.Profile` and `Airside.Model.TurnRate` still pass on axle-free fixtures.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/SpeedProfile.h \
        Plugins/Airside/Source/Airside/Private/Model/SpeedProfile.cpp \
        Plugins/Airside/Source/Airside/Private/Model/RouteFollower.cpp \
        Plugins/Airside/Source/AirsideTests/Private/NoseGearTracksTest.cpp
git commit -m "feat(model): corner speed is lateral acceleration, not a typed yaw rate"
```

---

### Task 5: Claims measure from the body centre

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Model/TrafficClaims.cpp` (`WindowFor`, ~line 169-180)
- Modify: `Plugins/Airside/Source/Airside/Public/Model/TrafficClaims.h` (the `T` comment, line 149)
- Test: `Plugins/Airside/Source/AirsideTests/Private/ClaimCentreTest.cpp` (new)

**Interfaces:**
- Consumes: `FAirframe::BodyCentreX`, `FAirframe::SteerAxleX` (Task 1).
- Produces: nothing new; `FClaimWindow::T` keeps its name and regains its documented meaning.

**Why this task exists:** `TrafficClaims.h:149` declares `T` to be *"Follower.Travelled: the agent's CENTRE, never its nose"*, and the whole window is `T ± F/2`. That was approximately true while mesh origins sat mid-fuselage. Plane2 is now exported about its nose gear, so its claim window already runs half a length too far forward and leaves its own tail unclaimed. This is a live defect introduced by the content change, not a consequence of Tasks 3-4.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/ClaimCentreTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadAgent.h"
#include "Model/TrafficClaims.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaimCentreTest,
	"Airside.Model.ClaimCentre",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FClaimCentreTest::RunTest(const FString& Parameters)
{
	// The claim window is T +/- F/2 and T is documented as the agent's CENTRE. With the
	// origin on the nose gear, Travelled is the NOSE's distance, so taking it for the centre
	// puts the whole window half a length too far forward - the agent reserves line it is
	// not on and leaves its own tail unclaimed, which is a collision nobody would explain.
	FRoadAgent Agent;
	Agent.Airframe.Ground = TestAirframes::Piper().Ground;
	Agent.Airframe.SteerAxleX = 0.0;
	Agent.Airframe.FixedAxleX = -454.3;
	Agent.Airframe.BodyCentreX = -629.3;   // (167.5 + -1426.1) / 2, plane2 measured
	Agent.Follower.Travelled = 10000.0;

	const double Centre = FClaimPass::CentreOf(Agent);
	TestEqual(TEXT("the centre is the body centre, not the steered axle"),
		Centre, 10000.0 - 629.3, 0.01);

	// An agent nobody measured keeps today's answer EXACTLY - every vehicle takes this path.
	FRoadAgent Unmeasured;
	Unmeasured.Airframe.Ground = TestAirframes::Piper().Ground;
	Unmeasured.Follower.Travelled = 10000.0;
	TestEqual(TEXT("an unmeasured agent's centre is still Travelled"),
		FClaimPass::CentreOf(Unmeasured), 10000.0, 0.0001);

	return true;
}

#endif
```

- [ ] **Step 2: Build twice, then run to verify it fails**

Run: the build line twice, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.ClaimCentre`
Expected: FAIL - `CentreOf` is not a member of `FClaimPass`.

- [ ] **Step 3: Implement it**

In `TrafficClaims.h`, beside the other statics, declare:

```cpp
	/**
	 * Where this agent's BODY CENTRE is along its route, uu.
	 *
	 * Travelled measures the STEERED AXLE (see FRouteFollower), and the origin it is derived
	 * from is the nose gear on a conforming airframe - neither of which is the centre the
	 * claim window is written in terms of. Both offsets are per-type data, so an airframe
	 * with none of them measured returns Travelled and behaves exactly as it did.
	 *
	 * Static and taking the agent so a test can ask it one question without a claim pass,
	 * a graph or a world.
	 */
	static double CentreOf(const FRoadAgent& Agent);
```

In `TrafficClaims.cpp`:

```cpp
double FClaimPass::CentreOf(const FRoadAgent& Agent)
{
	// Along the ROUTE rather than along the body axis, which is an approximation and a
	// deliberate one: on a bend the two differ by less than a centimetre at these offsets,
	// and the exact form would need the heading here, turning a distance into a pose and
	// this function into a second evaluator of where the agent is.
	return Agent.Follower.Travelled - Agent.Airframe.SteerAxleX + Agent.Airframe.BodyCentreX;
}
```

And in `WindowFor`, replace `const double T = Agent.Follower.Travelled;` with:

```cpp
	// THE CENTRE, which is no longer Travelled - see CentreOf. It was, back when every mesh
	// origin sat mid-fuselage; plane2's re-export about its nose gear (2026-09-13) made the
	// two differ by 6.3 m and this window silently move with it.
	const double T = CentreOf(Agent);
```

Update the header comment at line 149:

```cpp
		/** The agent's CENTRE, never its nose - see FClaimPass::CentreOf, which derives it
		 *  from Follower.Travelled and the airframe's own offsets. */
		double T = 0.0;
```

- [ ] **Step 4: Build and run the traffic tests**

Run: the build line, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model`
Expected: `0 failed, 0 crashed`. `ClaimGeometry`, `ClaimPassStandalone`, `CarFollowing`, `HeadOnStops`, `CrossingHoldsRunway` all use axle-free fixtures and are unchanged by this.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/TrafficClaims.h \
        Plugins/Airside/Source/Airside/Private/Model/TrafficClaims.cpp \
        Plugins/Airside/Source/AirsideTests/Private/ClaimCentreTest.cpp
git commit -m "fix(traffic): claim windows measure from the body centre, not the nose gear"
```

---

### Task 6: The steer angle reaches the AnimGraph

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h` (`FAgentMotion`, ~line 158)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadAgent.cpp` (`DescribeMotion`, lines 71-105)
- Modify: `Plugins/Airside/Source/Airside/Public/Present/AirsideAgentAnim.h` (beside `WheelAngleDegrees`)
- Modify: `Plugins/Airside/Source/Airside/Private/Present/AirsideAgentAnim.cpp` (`NativeUpdateAnimation`)
- Test: `Plugins/Airside/Source/AirsideTests/Private/AgentMotionTest.cpp` (extend the existing `Airside.Present.AgentMotion`)

**Interfaces:**
- Consumes: `FRouteFollower::SteerDegrees` (Task 3).
- Produces: `FAgentMotion::SteerAngleDegrees`, `UAirsideAgentAnim::SteerAngleDegrees`.

- [ ] **Step 1: Write the failing test**

Add to `AgentMotionTest.cpp`, inside the existing `Airside.Present.AgentMotion` test body:

```cpp
	// THE FORWARDER SEAM. The steer angle crosses three objects to reach a bone - follower,
	// motion, anim instance - and a seam with no test is how Tick, view spawn and view
	// destroy ended up with zero coverage. Asserted at the composition, not the struct.
	{
		FRoadAgent Steering;
		Steering.Airframe.Ground = TestAirframes::Piper().Ground;
		Steering.Follower.SteerDegrees = -17.5;

		const FAgentMotion Motion = Steering.DescribeMotion(FVector2D::ZeroVector, 0.0);
		TestEqual(TEXT("the motion carries the follower's steer angle, sign and all"),
			Motion.SteerAngleDegrees, -17.5, 0.01);
	}
```

- [ ] **Step 2: Build and run to verify it fails**

Run: the build line, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present.AgentMotion`
Expected: FAIL - `SteerAngleDegrees` is not a member of `FAgentMotion`.

- [ ] **Step 3: Add the field and publish it**

In `RoadEntity.h`, in `FAgentMotion` after `bAirborne`:

```cpp
	/**
	 * The steered wheel's deflection, degrees, signed the way Heading turns.
	 *
	 * THE CONTROL INPUT, not a description of the motion: this is the angle the follower
	 * steered with, and the yaw the aircraft actually took came out of it. An earlier design
	 * had the view derive this from the yaw rate for animation, which would have been a
	 * second model of the same thing and would have drifted from the first.
	 *
	 * Zero on a pivot-steered vehicle, which has no steered wheel to draw.
	 */
	UPROPERTY() double SteerAngleDegrees = 0.0;
```

In `DescribeMotion`, beside the `GroundSpeed` switch:

```cpp
	// FROM THE FOLLOWER WHATEVER THE PHASE, unlike GroundSpeed above: a landing rollout and
	// a take-off roll steer with the rudder and the nosewheel trails straight, so the
	// follower's zero is the right answer there rather than a missing one.
	Motion.SteerAngleDegrees = Follower.SteerDegrees;
```

In `AirsideAgentAnim.h`, after `WheelAngleDegrees`:

```cpp
	/**
	 * Nose-gear deflection, degrees. Apply to the STEER bone, not the rolling one.
	 *
	 * A separate bone because one bone cannot do both: the roll axis has to turn with the
	 * steering, and a single Transform (Modify) Bone applies its rotations in a fixed order,
	 * so a shared bone wobbles instead of steering. plane2's rig is nosewheel_steer (yaw
	 * about the strut) with nosewheel (roll) as its child.
	 *
	 * NOT integrated, unlike the prop and the wheels: this is an absolute deflection the
	 * model already decided, not a rate to accumulate.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Airside")
	float SteerAngleDegrees = 0.0f;
```

In `NativeUpdateAnimation`, beside `bAirborne = Motion.bAirborne;`:

```cpp
	SteerAngleDegrees = static_cast<float>(Motion.SteerAngleDegrees);
```

- [ ] **Step 4: Build and run**

Run: the build line, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present`
Expected: `0 failed, 0 crashed`.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h \
        Plugins/Airside/Source/Airside/Private/Model/RoadAgent.cpp \
        Plugins/Airside/Source/Airside/Public/Present/AirsideAgentAnim.h \
        Plugins/Airside/Source/Airside/Private/Present/AirsideAgentAnim.cpp \
        Plugins/Airside/Source/AirsideTests/Private/AgentMotionTest.cpp
git commit -m "feat(present): the steer angle reaches the AnimGraph"
```

---

### Task 7: Measure the two aircraft that have meshes

**Files:**
- Modify: `Tools/Python/build_plane2_type.py` (`measure()` and the apply site)
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/AircraftType.cpp` (`BuildPiperMeridian`, ~line 167-200)
- Test: `Plugins/Airside/Source/AirsideTests/Private/AirframeAxlesTest.cpp` (extend)

**Interfaces:**
- Consumes: everything above.
- Produces: authored content. No new C++ interfaces.

- [ ] **Step 1: Write the failing test**

Append inside `FAirframeAxlesTest::RunTest`, before `return true;`:

```cpp
	// THE PIPER IS MEASURED, and keeps its main-gear origin: SteerAxleX is its wheelbase,
	// because BuildPiperMeridian's own comment said this day would come - "that offset
	// belongs here as a field and not as a constant at the call site".
	UAircraftType* Meridian = NewObject<UAircraftType>();
	UAircraftType::BuildPiperMeridian(Meridian);
	const FAirframe Piper = Meridian->Airframe();
	TestTrue(TEXT("the Piper steers geometrically"), Piper.HasAxles());
	TestTrue(FString::Printf(TEXT("its wheelbase is about 2.6 m (%.1f uu)"), Piper.Wheelbase()),
		Piper.Wheelbase() > 200.0 && Piper.Wheelbase() < 320.0);
	TestEqual(TEXT("and its steered axle is ahead of its origin, which is the main gear"),
		Piper.FixedAxleX, 0.0, 0.01);

	// THE AIRLINERS STAY ON THE PIVOT LAW, deliberately: no mesh to measure against, and
	// their published figures would be a second opinion nobody could check on screen. They
	// are kept because EntityDefinition builds the A320 in production and the stand
	// geometry tests size against both - they are the only large footprints in the project.
	UAircraftType* Airbus = NewObject<UAircraftType>();
	UAircraftType::BuildA320(Airbus);
	TestFalse(TEXT("the A320 is not measured, so it keeps the flat rate"),
		Airbus->Airframe().HasAxles());
```

- [ ] **Step 2: Build and run to verify it fails**

Run: the build line, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.AirframeAxles`
Expected: FAIL - "the Piper steers geometrically" is false.

- [ ] **Step 3: Author the Piper's figures**

In `AircraftType.cpp`, in `BuildPiperMeridian` after the footprint block:

```cpp
	// THE AXLES, and the origin deviation this type has always declared in prose now has a
	// field. Measured off piper_aligned.blend the same way MainWheelRadius was: the mains
	// are AT the origin (that is what "origin is the main-gear axle" means), and the nose
	// gear sits 2.6 m ahead of them - the figure this file's own comment named when it said
	// "composing the two needs the wheelbase".
	Type->SteerAxleX = 260.0;
	Type->FixedAxleX = 0.0;

	// A light single on a quiet apron corners harder than an airliner, and 0.25 g is what a
	// Meridian's tyres and its pilot will take without anything sliding or spilling.
	Type->Ground.MaxSteerDegrees = 50.0;
	Type->Ground.MaxLateralAccelUu = 245.0;
```

Then update the class comment at `AircraftType.h:192` so the prose and the field agree:

```cpp
	 * Its LOCAL ORIGIN IS THE MAIN-GEAR AXLE, not the nose gear this class otherwise
	 * specifies, and that is a deviation with a reason rather than an oversight - see the
	 * comment at the footprint. It is DECLARED, not merely described: SteerAxleX carries the
	 * wheelbase so the follower and the stands can both compose against it.
```

- [ ] **Step 4: Author plane2's figures in the Python script**

In `Tools/Python/build_plane2_type.py`, in `measure()`, after `prop_diameter`:

```python
    # THE AXLES, off the wheels themselves. The origin is the nose gear (import_plane2.py
    # asserts it), so the steered axle is zero and the mains are the measurement.
    nose_lo, nose_hi = find("nosewheel")
    axles = {
        "steer_axle_x": (nose_lo[0] + nose_hi[0]) * 0.5,
        "fixed_axle_x": (wheel_lo[0] + wheel_hi[0]) * 0.5,
    }
    return footprint, wheel_radius, prop_diameter, axles
```

Update `measured()`'s docstring to say four values, and at the apply site:

```python
    footprint_figures, wheel_radius, prop_diameter, axles = measured()
    ...
    asset.set_editor_property("steer_axle_x", axles["steer_axle_x"])
    asset.set_editor_property("fixed_axle_x", axles["fixed_axle_x"])

    # PUBLISHED, not measured - nothing in the mesh knows how hard the type may corner. A
    # Twin Otter on a quiet apron takes more than an airliner: 0.2 g.
    ground.set_editor_property("max_steer_degrees", 60.0)
    ground.set_editor_property("max_lateral_accel_uu", 196.0)
```

- [ ] **Step 5: Write the pipeline test**

A half-run content pipeline - new mesh, old DA - gives an aeroplane whose envelope is 6.4 m
from its body, and nothing catches it today. Append to `AirframeAxlesTest.cpp` as its own
test, since it loads content and the others do not:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFootprintMatchesTheMeshTest,
	"Airside.Content.FootprintMatchesTheMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFootprintMatchesTheMeshTest::RunTest(const FString& Parameters)
{
	// THE HALF-RUN PIPELINE, pinned. build_plane2_type.py measures the export and writes
	// the DA; re-import the mesh without re-running it and the figures describe an aeroplane
	// that no longer exists. That is invisible on screen - the model looks right and its
	// clearance envelope is somewhere else - so it has to be a test.
	UAircraftType* Type = Cast<UAircraftType>(StaticLoadObject(
		UAircraftType::StaticClass(), nullptr, TEXT("/Game/Entities/DA_Aircraft_Plane2")));
	if (!TestNotNull(TEXT("DA_Aircraft_Plane2 loads"), Type))
	{
		return false;
	}

	USkeletalMesh* Mesh = Type->Mesh.LoadSynchronous();
	if (!TestNotNull(TEXT("and the mesh it names loads"), Mesh))
	{
		return false;
	}

	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	const double MeshNoseX = Bounds.Origin.X + Bounds.BoxExtent.X;
	const double MeshTailX = Bounds.Origin.X - Bounds.BoxExtent.X;

	// A centimetre either way: these come from the same measurement, so anything larger is
	// a pipeline that was not re-run rather than a rounding difference.
	TestEqual(TEXT("the authored nose matches the mesh"),
		Type->Footprint.NoseX, MeshNoseX, 1.0);
	TestEqual(TEXT("the authored tail matches the mesh"),
		Type->Footprint.TailX, MeshTailX, 1.0);

	// AND THE ORIGIN IS THE NOSE GEAR, which is what makes the steered axle zero. A mesh
	// re-exported about some other point would pass the two checks above and steer wrong.
	TestEqual(TEXT("plane2's steered axle is its origin, per the class convention"),
		Type->SteerAxleX, 0.0, 1.0);

	return true;
}
```

`AirframeAxlesTest.cpp` needs `#include "Engine/SkeletalMesh.h"` and `#include "UObject/UObjectGlobals.h"` for this.

- [ ] **Step 6: Run the content script and verify what landed**

The editor must be closed (kill it if nothing is unsaved).

Run:
```
& "D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\repos\AirportMgr2\AirportMgr.uproject" `
  -run=pythonscript -script="C:\repos\AirportMgr2\Tools\Python\build_plane2_type.py" -unattended -nosplash -nopause
```
Then grep `MARKER` out of `Saved/Logs/AirportMgr.log`.
Expected: the measured line reports `steer 0.0, fixed -454.3`, and every `PASS` as before.

- [ ] **Step 7: Run the full suite**

Run: `./Tools/Run-AirsideTests.ps1`
Expected: `0 failed, 0 crashed`, and the count is the previous total plus the seven tests this plan adds.

- [ ] **Step 8: Commit**

```bash
git add Plugins/Airside/Source/Airside/Private/Entities/AircraftType.cpp \
        Plugins/Airside/Source/Airside/Public/Entities/AircraftType.h \
        Tools/Python/build_plane2_type.py \
        Plugins/Airside/Source/AirsideTests/Private/AirframeAxlesTest.cpp \
        Content/Entities/DA_Aircraft_Plane2.uasset
git commit -m "feat(content): plane2 and the Piper steer on measured geometry"
```

---

## Verification, in PIE

The plan's tests prove the arithmetic. They cannot prove the aeroplane looks right, so finish with a repro and say exactly what to look for:

1. Open the editor, PIE, key 7 to land a plane2, and watch it taxi a bend.
2. **The nose wheel is on the painted line and the mains are inside it.** That is the reported defect, fixed.
3. **The turn is faster than it was** - roughly 13 kn where a 30 m bend used to give 10.
4. `Saved/Logs/AirportMgr.log` after the run: no `LogAirsideTraffic` warnings about crabs or replans on an ordinary bend.

The nose wheel will not YET turn: the AnimGraph node for `nosewheel_steer` is hand-authored work that UE exposes no Python for. After Task 6 lands, `build_plane2_anim.py` prints the row for it, and the node is Rotation = **ADD TO EXISTING**, Bone Space, Translation and Scale on **IGNORE**.

## What this plan does NOT do

- **Swept-path claims.** The mains cutting inside a corner is now visible and nothing reserves pavement for it. Task 5 restores the claim window's documented meaning; it does not make the window follow the curve.
- **Articulation.** `RoadGeom::TrailPoint` is the shape a towed unit would reuse. Nothing tows anything yet.
- **The A320 and 737.** Kept, unmeasured, on the pivot law.
- **`TakeoffRun.cpp:128`**, which slews at `MaxTurnRateDegPerSec` for the runway lineup. Left alone deliberately: the roll is straight, and the figure keeps that one job.
