# Organic Module Placement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the bay grid that stamps modules in a row with a deterministic scatter, so a
depot's yard reads as built rather than placed by a solver.

**Architecture:** A new dependency-free `Solve/PlotYard` takes the plot outline, the frontage,
a gate and a list of FOOTPRINTS, and returns a STAND per footprint by rejection sampling
against a seeded `FRandomStream`. `UPlotPresenter` maps `EDepotModule` to footprints on the
other side of that seam, so `Solve/` never sees `Model/`. The seed is a hash of the entity's
position, because the presenter rebuilds every stand on every graph change.

**Tech Stack:** UE 5.8 C++, `FRandomStream` (Core), `RoadGeom` for polygon maths,
`IMPLEMENT_SIMPLE_AUTOMATION_TEST` for tests.

**Spec:** `docs/superpowers/specs/2026-09-16-organic-module-placement-design.md`

## Global Constraints

- **`Solve/` headers include `CoreMinimal.h` and nothing else.** `Solve/PlotYard.h` may not
  include `Model/`, `Entities/`, `Present/` or any engine type beyond CoreMinimal. This is
  what `Check-Architecture.ps1` lints and what lets the tests run with no world.
- **`Solve/` must not see `EDepotModule`.** It lives in `Model/RoadEntity.h`. The solver
  takes `PlotYard::FFootprint`; the mapping is `UPlotPresenter`'s.
- **The editor must be CLOSED to build.** `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat
  AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject"
  -WaitMutex -NoHotReloadFromIDE`. The `-NoHotReloadFromIDE` flag is correct HERE because
  this is a worktree; never pass it on `C:\repos\AirportMgr2`.
- **A new test .cpp needs two builds.** The first reports `Result: Succeeded` without
  compiling it.
- **Never trust the test runner's exit code.** Read the `N test(s) run, N failed, N crashed`
  line from `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject"`.
- **Comments explain WHY**, and especially why an obvious alternative was rejected. Match the
  surrounding density; do not strip it.
- **Tests assert behaviour with a named reason**, not bare values.
- **Test names are leaves under `Airside.Solve.` / `Airside.Present.`** and must be distinct:
  UE's automation tree DROPS a bare-named test once a dotted child of that name exists.
- Do not add a `Co-Authored-By` trailer to commit messages.

---

### Task 1: The footprint and stand types, and a solver that only places the shed

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Solve/PlotYard.h`
- Create: `Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/PlotYardTest.cpp`

**Interfaces:**
- Consumes: `RoadGeom::PerpCCW`, `RoadGeom::Bearing`, `RoadGeom::PointInPolygon`,
  `RoadGeom::Rotate` from `Solve/RoadGeom.h`; `PlotFit::CornerInsetUu` from `Solve/PlotFit.h`.
- Produces: `PlotYard::FFootprint`, `PlotYard::FStand`, `PlotYard::FYard`,
  `PlotYard::LayOut(...)`, `PlotYard::StandCorners(...)`.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/PlotYardTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/PlotYard.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** An axis-aligned rectangle, CCW, with its SOUTH edge (y = 0) as the frontage.
	 *  Same shape PlotFitTest uses, so the two files describe the same world. */
	TArray<FVector2D> YardRect(double Width, double Depth)
	{
		return { FVector2D(0.0, 0.0), FVector2D(Width, 0.0),
		         FVector2D(Width, Depth), FVector2D(0.0, Depth) };
	}

	/** A shed-sized footprint that fronts the gate. 8 m deep, 4 m wide. */
	PlotYard::FFootprint Shed()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 800.0;
		F.WidthUu = 400.0;
		F.bFrontsTheGate = true;
		return F;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardFrontsTheShedOnTheGateTest,
	"Airside.Solve.PlotYardFrontsTheShedOnTheGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardFrontsTheShedOnTheGateTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(2400.0, 1600.0);
	const FVector2D FrontageA(0.0, 0.0);
	const FVector2D FrontageB(2400.0, 0.0);
	const FVector2D Gate(1200.0, 0.0);

	const PlotYard::FFootprint Footprints[] = { Shed() };

	const PlotYard::FYard Yard = PlotYard::LayOut(
		Outline, FrontageA, FrontageB, Gate, Footprints, /*Seed=*/1234, Shed());

	if (!TestEqual(TEXT("one footprint in, one stand out"), Yard.Stands.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("the shed was placed"), Yard.Stands[0].bPlaced);

	// SQUARE TO THE FRONTAGE, not jittered. The truck drives out of the shed, so its
	// heading is functional - it is the one module that may not be turned for looks.
	// Interior is +Y here, so the inward bearing is +90 degrees.
	TestEqual(TEXT("the shed faces away from the road, square"),
		Yard.Stands[0].Heading, UE_DOUBLE_HALF_PI);

	// AND IT IS ON THE GATE. A shed behind the tank is a shed the truck cannot leave, and
	// it would look perfectly correct from every angle.
	TestTrue(TEXT("the shed sits at the gate, not deep in the yard"),
		FVector2D::Distance(Yard.Stands[0].Centre, Gate) < 800.0);

	return true;
}

#endif
```

- [ ] **Step 2: Build twice, run, verify it fails**

```bash
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
```

Expected: FAIL to compile — `Solve/PlotYard.h` does not exist. That is the red step; a
compile error naming the missing header is an acceptable failure here.

- [ ] **Step 3: Write the header**

Create `Plugins/Airside/Source/Airside/Public/Solve/PlotYard.h`:

```cpp
#pragma once

#include "CoreMinimal.h"

/**
 * Where each module STANDS in a drawn plot.
 *
 * Dependency-free, like every other Solve/ header: CoreMinimal.h and nothing else. In
 * particular it never sees EDepotModule, which lives in Model/RoadEntity.h - this layer
 * takes FOOTPRINTS and hands back STANDS, and mapping a module to its footprint is
 * UPlotPresenter's job on the other side of the seam. That is the same split FitBays makes
 * by taking an outline rather than an entity, and it is what keeps these tests free of a
 * world, an actor and NewObject.
 *
 * WHY NOT PlotFit. PlotFit answers "how many bays fit against this edge", which is a
 * question about the PLOT. This answers "where does each thing stand", which is a question
 * about its CONTENTS, and the two have different inputs and different failure modes. They
 * are separate files so that a change to one cannot quietly alter the other.
 */
namespace PlotYard
{
	/**
	 * How far apart two modules must stand, uu. 1 m.
	 *
	 * NOT a tolerance: modules that touch read as one building, which is precisely the
	 * "stamped" look this whole file exists to remove. Placement feel, judged in PIE - if it
	 * needs tuning it becomes a UAirsideSettings knob rather than being retyped here.
	 */
	inline constexpr double ClearanceUu = 100.0;

	/**
	 * The lane kept clear from the gate into the plot, uu. 6.2 m, the fuel truck's length.
	 *
	 * A DEPOT THE TRUCK CANNOT LEAVE LOOKS PERFECTLY CORRECT FROM EVERY ANGLE - the same
	 * failure the fence's gate gap exists to prevent, and the reason that gap is counted
	 * rather than eyeballed.
	 */
	inline constexpr double GateCorridorUu = 620.0;

	/**
	 * Candidate poses tried per module before it is dropped.
	 *
	 * BOUNDED AND SMALL. UPlotPresenter::RebuildFrom runs on every graph change, so an
	 * unbounded search would make laying a road stutter on an airport full of depots.
	 */
	inline constexpr int32 MaxTries = 24;

	/**
	 * How far a heading may wander off its quarter turn, radians. ~12 degrees.
	 *
	 * A QUARTER TURN PLUS A FEW DEGREES, never a uniform circle: uniform rotation reads as
	 * debris after an explosion, and this reads as something parked in a hurry, which is the
	 * feeling being bought.
	 */
	inline constexpr double HeadingJitterRadians = 0.21;

	struct FFootprint
	{
		/** Along the module's own +X, which faces away from the road. */
		double LengthUu = 0.0;
		double WidthUu = 0.0;

		/**
		 * Square to the frontage and nearest the gate, rather than sampled.
		 *
		 * The shed, because a truck drives out of it. Its heading is FUNCTIONAL and is the
		 * one thing here that may not be turned for looks.
		 */
		bool bFrontsTheGate = false;
	};

	struct FStand
	{
		FVector2D Centre = FVector2D::ZeroVector;
		/** Radians. +X faces away from the road, as UEntityDefinition::BuildFuelDepot states. */
		double Heading = 0.0;
		bool bPlaced = false;
	};

	struct FYard
	{
		/**
		 * One per footprint given, IN THE ORDER GIVEN; a dropped one has bPlaced false.
		 *
		 * NOT COMPACTED to the ones that fit. The caller knows which module it asked about
		 * only by index, and compacting would silently re-associate a pump's stand with a
		 * tank - a depot drawing the wrong box in the wrong place, with nothing to say so.
		 */
		TArray<FStand> Stands;

		/** How many more of the caller's sample footprint would still fit. */
		int32 RoomForMore = 0;

		/** Derived, never stored: a second count is a second thing to keep in agreement. */
		int32 DroppedCount() const
		{
			int32 Count = 0;
			for (const FStand& Stand : Stands)
			{
				if (!Stand.bPlaced) { ++Count; }
			}
			return Count;
		}
	};

	/**
	 * The four corners of a stand, in order, inset by PlotFit::CornerInsetUu.
	 *
	 * PUBLIC because the tests assert containment and non-overlap with it, and a test that
	 * computed corners its own way would be checking its own arithmetic rather than the
	 * solver's. One derivation, two consumers.
	 */
	AIRSIDE_API void StandCorners(const FStand& Stand, const FFootprint& Footprint,
		TArray<FVector2D>& OutCorners);

	/**
	 * Lay the footprints out in the plot.
	 *
	 * Gate is where the fence is left open - URoadEditFacade::PlaceEntityInPlot puts the
	 * entity's pose there, so it is the entity's Position. Seed makes the result repeatable;
	 * see the design doc section 5 for why that is a requirement and not a nicety.
	 */
	AIRSIDE_API FYard LayOut(TArrayView<const FVector2D> Outline,
		FVector2D FrontageA, FVector2D FrontageB, FVector2D Gate,
		TArrayView<const FFootprint> Footprints, int32 Seed,
		const FFootprint& RoomForFootprint);
}
```

- [ ] **Step 4: Write the minimal implementation — shed only**

Create `Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp`:

```cpp
#include "Solve/PlotYard.h"

#include "Solve/PlotFit.h"
#include "Solve/RoadGeom.h"

namespace
{
	/** The inward normal of the frontage: which way is INTO the plot. */
	FVector2D InwardOf(TArrayView<const FVector2D> Outline, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D Along = (B - A).GetSafeNormal();
		const FVector2D Left = RoadGeom::PerpCCW(Along);

		// Read off the polygon's winding rather than assumed counter-clockwise, exactly as
		// FitBays does: a plot stored the other way round would otherwise aim every module
		// out of the plot and across the road.
		return RoadGeom::PolygonArea(Outline) > 0.0 ? Left : -Left;
	}
}

void PlotYard::StandCorners(const FStand& Stand, const FFootprint& Footprint,
	TArray<FVector2D>& OutCorners)
{
	OutCorners.Reset();

	// INSET, and the inset is load-bearing rather than a fudge - see PlotFit::CornerInsetUu.
	// A stand flush against the outline puts its corner exactly ON the boundary, where a
	// containment test answers by floating-point coin flip and differently on another machine.
	const double HalfLength = FMath::Max(Footprint.LengthUu * 0.5 - PlotFit::CornerInsetUu, 0.0);
	const double HalfWidth = FMath::Max(Footprint.WidthUu * 0.5 - PlotFit::CornerInsetUu, 0.0);

	const FVector2D Forward(FMath::Cos(Stand.Heading), FMath::Sin(Stand.Heading));
	const FVector2D Side = RoadGeom::PerpCCW(Forward);

	OutCorners.Add(Stand.Centre + Forward * HalfLength + Side * HalfWidth);
	OutCorners.Add(Stand.Centre + Forward * HalfLength - Side * HalfWidth);
	OutCorners.Add(Stand.Centre - Forward * HalfLength - Side * HalfWidth);
	OutCorners.Add(Stand.Centre - Forward * HalfLength + Side * HalfWidth);
}

PlotYard::FYard PlotYard::LayOut(TArrayView<const FVector2D> Outline,
	FVector2D FrontageA, FVector2D FrontageB, FVector2D Gate,
	TArrayView<const FFootprint> Footprints, int32 Seed,
	const FFootprint& RoomForFootprint)
{
	FYard Yard;
	Yard.Stands.SetNum(Footprints.Num());

	if (Outline.Num() < 3)
	{
		return Yard;
	}

	const FVector2D Inward = InwardOf(Outline, FrontageA, FrontageB);
	const double InwardBearing = RoadGeom::Bearing(Inward);

	// The gate-fronting modules first: their pose is decided, not sampled, so they take
	// their ground before anything is allowed to sample into it.
	for (int32 Index = 0; Index < Footprints.Num(); ++Index)
	{
		if (!Footprints[Index].bFrontsTheGate)
		{
			continue;
		}

		FStand& Stand = Yard.Stands[Index];
		Stand.Heading = InwardBearing;
		Stand.Centre = Gate + Inward * (Footprints[Index].LengthUu * 0.5);
		Stand.bPlaced = true;
	}

	return Yard;
}
```

- [ ] **Step 5: Build twice, run, verify it passes**

```bash
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject" -Filter Airside.Solve
```

Expected: `Airside.Solve.PlotYardFrontsTheShedOnTheGate` PASS. Read the
`N test(s) run, N failed, N crashed` line; the exit code does not catch a crash.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Solve/PlotYard.h `
        Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotYardTest.cpp
git commit -m "feat(solve): the shed stands on the gate, because a truck leaves it"
```

---

### Task 2: Sampling the rest, inside the plot and clear of each other

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotYardTest.cpp`

**Interfaces:**
- Consumes: Task 1's `FFootprint`, `FStand`, `FYard`, `LayOut`, `StandCorners`.
- Produces: no new public types. `LayOut` now places every footprint, not only the
  gate-fronting ones.

- [ ] **Step 1: Write the failing tests**

Append to `PlotYardTest.cpp`, and add these two helpers to its anonymous namespace first:

```cpp
	/** A tank-sized footprint: 5 m x 5 m, and it does not front the gate. */
	PlotYard::FFootprint Tank()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 500.0;
		F.WidthUu = 500.0;
		F.bFrontsTheGate = false;
		return F;
	}

	/** A pump: 3 m x 2 m, low and small. */
	PlotYard::FFootprint Pump()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 300.0;
		F.WidthUu = 200.0;
		F.bFrontsTheGate = false;
		return F;
	}

	/** True when two stands' corner rectangles intersect, by separating axis. */
	bool StandsOverlap(const PlotYard::FStand& A, const PlotYard::FFootprint& FA,
		const PlotYard::FStand& B, const PlotYard::FFootprint& FB)
	{
		TArray<FVector2D> CornersA;
		TArray<FVector2D> CornersB;
		PlotYard::StandCorners(A, FA, CornersA);
		PlotYard::StandCorners(B, FB, CornersB);

		// Four candidate axes - two per rectangle. Two convex shapes miss each other if and
		// only if some axis separates them, so finding one is proof of no overlap.
		const TArray<FVector2D> Axes = {
			(CornersA[1] - CornersA[0]).GetSafeNormal(),
			(CornersA[3] - CornersA[0]).GetSafeNormal(),
			(CornersB[1] - CornersB[0]).GetSafeNormal(),
			(CornersB[3] - CornersB[0]).GetSafeNormal() };

		for (const FVector2D& Axis : Axes)
		{
			double MinA = TNumericLimits<double>::Max();
			double MaxA = -TNumericLimits<double>::Max();
			double MinB = TNumericLimits<double>::Max();
			double MaxB = -TNumericLimits<double>::Max();
			for (const FVector2D& P : CornersA)
			{
				const double D = FVector2D::DotProduct(P, Axis);
				MinA = FMath::Min(MinA, D);
				MaxA = FMath::Max(MaxA, D);
			}
			for (const FVector2D& P : CornersB)
			{
				const double D = FVector2D::DotProduct(P, Axis);
				MinB = FMath::Min(MinB, D);
				MaxB = FMath::Max(MaxB, D);
			}
			if (MaxA < MinB || MaxB < MinA)
			{
				return false;
			}
		}
		return true;
	}
```

Then the tests:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardKeepsModulesInsideThePlotTest,
	"Airside.Solve.PlotYardKeepsModulesInsideThePlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardKeepsModulesInsideThePlotTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(2400.0, 1600.0);
	const PlotYard::FFootprint Footprints[] = { Shed(), Tank(), Pump() };

	// SEVERAL SEEDS, not one. A sampler that happens to keep everything inside on seed 1234
	// and hangs a tank over the fence on 1235 is exactly the bug this guards, and a
	// single-seed test would ship it.
	for (int32 Seed = 1; Seed <= 8; ++Seed)
	{
		const PlotYard::FYard Yard = PlotYard::LayOut(
			Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0), FVector2D(1200.0, 0.0),
			Footprints, Seed, Tank());

		for (int32 Index = 0; Index < Yard.Stands.Num(); ++Index)
		{
			if (!Yard.Stands[Index].bPlaced)
			{
				continue;
			}

			TArray<FVector2D> Corners;
			PlotYard::StandCorners(Yard.Stands[Index], Footprints[Index], Corners);
			for (const FVector2D& Corner : Corners)
			{
				// EVERY CORNER, not the centre. A centre-only test accepts a module hanging
				// out of the plot and the player watches a tank stand on the grass.
				TestTrue(*FString::Printf(
					TEXT("seed %d: module %d corner (%.0f, %.0f) is inside the plot"),
					Seed, Index, Corner.X, Corner.Y),
					RoadGeom::PointInPolygon(Outline, Corner));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardDoesNotOverlapModulesTest,
	"Airside.Solve.PlotYardDoesNotOverlapModules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardDoesNotOverlapModulesTest::RunTest(const FString& Parameters)
{
	// A TIGHT PLOT, deliberately: on a large one a naive sampler passes by luck. Two bays
	// wide and two deep has to hold a shed, a tank and a pump with little room to spare.
	const TArray<FVector2D> Outline = YardRect(800.0, 1600.0);
	const PlotYard::FFootprint Footprints[] = { Shed(), Tank(), Pump() };

	for (int32 Seed = 1; Seed <= 8; ++Seed)
	{
		const PlotYard::FYard Yard = PlotYard::LayOut(
			Outline, FVector2D(0.0, 0.0), FVector2D(800.0, 0.0), FVector2D(400.0, 0.0),
			Footprints, Seed, Tank());

		for (int32 A = 0; A < Yard.Stands.Num(); ++A)
		{
			for (int32 B = A + 1; B < Yard.Stands.Num(); ++B)
			{
				if (!Yard.Stands[A].bPlaced || !Yard.Stands[B].bPlaced)
				{
					continue;
				}
				// TWO MODULES IN ONE SPACE is the one failure that cannot be argued as
				// styling - it is a mesh through a mesh, and no camera angle hides it.
				TestFalse(*FString::Printf(TEXT("seed %d: module %d and %d do not intersect"),
					Seed, A, B),
					StandsOverlap(Yard.Stands[A], Footprints[A], Yard.Stands[B], Footprints[B]));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardLeavesTheGateClearTest,
	"Airside.Solve.PlotYardLeavesTheGateClear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardLeavesTheGateClearTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(2400.0, 1600.0);
	const FVector2D Gate(1200.0, 0.0);

	// NO SHED in this mix, so nothing is entitled to sit on the gate and every stand here
	// is one the sampler chose. With a shed present the shed IS on the gate by design, and
	// the test would be asserting the opposite of the rule it means to check.
	const PlotYard::FFootprint Footprints[] = { Tank(), Pump(), Pump() };

	for (int32 Seed = 1; Seed <= 8; ++Seed)
	{
		const PlotYard::FYard Yard = PlotYard::LayOut(
			Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0), Gate,
			Footprints, Seed, Tank());

		for (int32 Index = 0; Index < Yard.Stands.Num(); ++Index)
		{
			if (!Yard.Stands[Index].bPlaced)
			{
				continue;
			}
			TArray<FVector2D> Corners;
			PlotYard::StandCorners(Yard.Stands[Index], Footprints[Index], Corners);
			for (const FVector2D& Corner : Corners)
			{
				// The corridor runs from the gate straight into the plot, +Y here. A corner
				// inside that lane is a module the truck would drive through.
				const bool bInLane =
					FMath::Abs(Corner.X - Gate.X) < PlotYard::GateCorridorUu * 0.5
					&& Corner.Y >= 0.0;
				TestFalse(*FString::Printf(
					TEXT("seed %d: module %d keeps out of the truck's way"), Seed, Index),
					bInLane);
			}
		}
	}
	return true;
}
```

Add `#include "Solve/RoadGeom.h"` to the test's includes — `PointInPolygon` comes from there.

- [ ] **Step 2: Build twice, run, verify they fail**

Expected: `PlotYardLeavesTheGateClear` FAILS. Its mix has no shed, so Task 1's `LayOut`
places nothing at all, and the test's loop over placed stands is empty - it fails on the
`Stands.Num()` check rather than on a corner. That is the hole this task fills.

**`PlotYardKeepsModulesInsideThePlot` and `PlotYardDoesNotOverlapModules` PASS VACUOUSLY**
here: every non-shed stand has `bPlaced` false, and both loops `continue` past those. That is
expected and proves nothing. They become real assertions at Step 4, once modules are actually
being placed - judge them THERE, not here.

- [ ] **Step 3: Implement the sampling pass**

Replace the body of `LayOut` in `PlotYard.cpp` after the gate-fronting loop. Add these to the
file's anonymous namespace first:

```cpp
	/** The outline's axis-aligned bounds, which is where candidate points are drawn from. */
	void BoundsOf(TArrayView<const FVector2D> Outline, FVector2D& OutMin, FVector2D& OutMax)
	{
		OutMin = FVector2D(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
		OutMax = FVector2D(-TNumericLimits<double>::Max(), -TNumericLimits<double>::Max());
		for (const FVector2D& P : Outline)
		{
			OutMin.X = FMath::Min(OutMin.X, P.X);
			OutMin.Y = FMath::Min(OutMin.Y, P.Y);
			OutMax.X = FMath::Max(OutMax.X, P.X);
			OutMax.Y = FMath::Max(OutMax.Y, P.Y);
		}
	}

	/** Do these two convex quads intersect? Separating axis; four axes for two rectangles. */
	bool QuadsIntersect(TArrayView<const FVector2D> A, TArrayView<const FVector2D> B)
	{
		const FVector2D Axes[] = {
			(A[1] - A[0]).GetSafeNormal(), (A[3] - A[0]).GetSafeNormal(),
			(B[1] - B[0]).GetSafeNormal(), (B[3] - B[0]).GetSafeNormal() };

		for (const FVector2D& Axis : Axes)
		{
			double MinA = TNumericLimits<double>::Max();
			double MaxA = -TNumericLimits<double>::Max();
			double MinB = TNumericLimits<double>::Max();
			double MaxB = -TNumericLimits<double>::Max();
			for (const FVector2D& P : A)
			{
				const double D = FVector2D::DotProduct(P, Axis);
				MinA = FMath::Min(MinA, D);
				MaxA = FMath::Max(MaxA, D);
			}
			for (const FVector2D& P : B)
			{
				const double D = FVector2D::DotProduct(P, Axis);
				MinB = FMath::Min(MinB, D);
				MaxB = FMath::Max(MaxB, D);
			}
			if (MaxA < MinB || MaxB < MinA)
			{
				// AN AXIS SEPARATES THEM, which is proof rather than evidence: two convex
				// shapes miss each other if and only if one exists. A bounding-box test
				// would refuse legal poses and accept illegal ones by turns, now that these
				// rectangles are not axis-aligned.
				return false;
			}
		}
		return true;
	}
```

Then, inside `LayOut`, after the gate-fronting loop and before `return Yard;`:

```cpp
	// PLACED FOOTPRINTS GROW AS WE GO, and a candidate is tested against every one already
	// standing - including the shed, which took its ground first for exactly this reason.
	TArray<TArray<FVector2D>> Taken;
	TArray<FVector2D> Corners;
	for (int32 Index = 0; Index < Footprints.Num(); ++Index)
	{
		if (Yard.Stands[Index].bPlaced)
		{
			StandCorners(Yard.Stands[Index], Footprints[Index], Corners);
			Taken.Add(Corners);
		}
	}

	FVector2D Min = FVector2D::ZeroVector;
	FVector2D Max = FVector2D::ZeroVector;
	BoundsOf(Outline, Min, Max);

	FRandomStream Stream(Seed);

	// LARGEST FIRST, by index order into a sorted list rather than by sorting Yard.Stands,
	// whose order is the caller's contract. A tank placed after four pumps have taken the
	// middle has nowhere left to go, and the player loses the biggest object rather than
	// the smallest.
	TArray<int32> Order;
	for (int32 Index = 0; Index < Footprints.Num(); ++Index)
	{
		if (!Footprints[Index].bFrontsTheGate)
		{
			Order.Add(Index);
		}
	}
	Order.Sort([&Footprints](int32 A, int32 B)
	{
		return Footprints[A].LengthUu * Footprints[A].WidthUu
			> Footprints[B].LengthUu * Footprints[B].WidthUu;
	});

	// Grown by ClearanceUu on every side, so the gap between two modules is enforced by the
	// same test that stops them intersecting. Two modules that merely touch read as one
	// building, which is the stamped look this file exists to remove.
	auto Padded = [](const FFootprint& Footprint)
	{
		FFootprint Out = Footprint;
		Out.LengthUu += ClearanceUu;
		Out.WidthUu += ClearanceUu;
		return Out;
	};

	auto TryPlace = [&](const FFootprint& Footprint, FStand& OutStand) -> bool
	{
		for (int32 Try = 0; Try < MaxTries; ++Try)
		{
			FStand Candidate;
			Candidate.Centre = FVector2D(
				Stream.FRandRange(Min.X, Max.X), Stream.FRandRange(Min.Y, Max.Y));

			// A QUARTER TURN PLUS A FEW DEGREES. Uniform over a circle reads as debris
			// after an explosion; this reads as something parked in a hurry.
			Candidate.Heading = InwardBearing
				+ Stream.RandRange(0, 3) * UE_DOUBLE_HALF_PI
				+ Stream.FRandRange(-HeadingJitterRadians, HeadingJitterRadians);

			const FFootprint Grown = Padded(Footprint);
			StandCorners(Candidate, Grown, Corners);

			bool bLegal = true;
			for (const FVector2D& Corner : Corners)
			{
				if (!RoadGeom::PointInPolygon(Outline, Corner))
				{
					bLegal = false;
					break;
				}

				// The corridor from the gate into the plot. Measured ACROSS the frontage
				// and only on the inward side, so a module beside the gate but outside the
				// lane is legal - the truck turns once it is clear of the fence.
				const FVector2D FromGate = Corner - Gate;
				const double Across = FVector2D::DotProduct(FromGate, RoadGeom::PerpCCW(Inward));
				const double Into = FVector2D::DotProduct(FromGate, Inward);
				if (Into >= 0.0 && FMath::Abs(Across) < GateCorridorUu * 0.5)
				{
					bLegal = false;
					break;
				}
			}
			if (!bLegal)
			{
				continue;
			}

			for (const TArray<FVector2D>& Other : Taken)
			{
				if (QuadsIntersect(Corners, Other))
				{
					bLegal = false;
					break;
				}
			}
			if (!bLegal)
			{
				continue;
			}

			Candidate.bPlaced = true;
			OutStand = Candidate;
			Taken.Add(Corners);
			return true;
		}

		// DROPPED, not forced. A module shoved in anyway would intersect something, and a
		// mesh through a mesh is the one failure no camera angle hides. The caller reports
		// it - "Modules 2 of 3" is already the readout's habit.
		return false;
	};

	for (const int32 Index : Order)
	{
		TryPlace(Footprints[Index], Yard.Stands[Index]);
	}

	// HOW MANY MORE WOULD FIT, from the same pass that places things - so the number the
	// player reads is produced by the code that would actually put the thing down. A
	// separate free-area calculation would be a second opinion about one question.
	FStand Phantom;
	while (Yard.RoomForMore < 64 && TryPlace(RoomForFootprint, Phantom))
	{
		++Yard.RoomForMore;
	}
```

**`TryPlace` appends to `Taken` on success**, which is what makes the `RoomForMore` loop
terminate: each phantom occupies ground the next one cannot use. The `< 64` guard is a
backstop against a zero-area footprint looping forever, not an expected limit.

- [ ] **Step 4: Run to verify they pass**

```bash
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject" -Filter Airside.Solve
```

Expected: all four `Airside.Solve.PlotYard*` tests PASS.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotYardTest.cpp
git commit -m "feat(solve): modules are sampled into the yard, inside it and clear of each other"
```

---

### Task 3: The same seed gives the same yard

**Files:**
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotYardTest.cpp`

**Interfaces:**
- Consumes: Task 2's `LayOut`.
- Produces: nothing new. This task adds only the tests that PIN determinism.

No implementation step: `FRandomStream(Seed)` already makes `LayOut` deterministic. This
task exists because determinism is a REQUIREMENT with no other test covering it — a later
refactor reaching for `FMath::FRand` would pass every test in Task 2 and make every depot on
the airport twitch each time the player laid a road.

- [ ] **Step 1: Write the tests**

Append to `PlotYardTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardIsDeterministicTest,
	"Airside.Solve.PlotYardIsDeterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardIsDeterministicTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(2400.0, 1600.0);
	const PlotYard::FFootprint Footprints[] = { Shed(), Tank(), Pump() };

	auto Lay = [&]()
	{
		return PlotYard::LayOut(Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0),
			FVector2D(1200.0, 0.0), Footprints, /*Seed=*/4242, Tank());
	};

	const PlotYard::FYard First = Lay();
	const PlotYard::FYard Second = Lay();

	if (!TestEqual(TEXT("both lay out the same number of stands"),
		First.Stands.Num(), Second.Stands.Num()))
	{
		return false;
	}

	for (int32 Index = 0; Index < First.Stands.Num(); ++Index)
	{
		// BITWISE, not nearly. UPlotPresenter::RebuildFrom clears and rebuilds on every
		// graph change, so "close enough" is a yard that shivers every time the player lays
		// a road somewhere else on the airport - and nothing on screen would explain why.
		TestTrue(*FString::Printf(TEXT("stand %d lands on exactly the same spot"), Index),
			First.Stands[Index].Centre == Second.Stands[Index].Centre);
		TestTrue(*FString::Printf(TEXT("stand %d takes exactly the same heading"), Index),
			First.Stands[Index].Heading == Second.Stands[Index].Heading);
		TestEqual(*FString::Printf(TEXT("stand %d agrees about being placed"), Index),
			First.Stands[Index].bPlaced, Second.Stands[Index].bPlaced);
	}
	TestEqual(TEXT("and both agree how much room is left"),
		First.RoomForMore, Second.RoomForMore);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardVariesWithSeedTest,
	"Airside.Solve.PlotYardVariesWithSeed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardVariesWithSeedTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(2400.0, 1600.0);
	const PlotYard::FFootprint Footprints[] = { Shed(), Tank(), Pump() };

	auto LaySeeded = [&](int32 Seed)
	{
		return PlotYard::LayOut(Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0),
			FVector2D(1200.0, 0.0), Footprints, Seed, Tank());
	};

	const PlotYard::FYard A = LaySeeded(1);
	const PlotYard::FYard B = LaySeeded(2);

	// WITHOUT THIS, a solver that ignored the seed entirely - or one that quietly placed
	// everything on a grid again - would pass every other test in this file. Two depots
	// looking identical IS the complaint this whole feature answers.
	bool bAnyDifference = false;
	for (int32 Index = 0; Index < A.Stands.Num() && Index < B.Stands.Num(); ++Index)
	{
		if (A.Stands[Index].Centre != B.Stands[Index].Centre
			|| A.Stands[Index].Heading != B.Stands[Index].Heading)
		{
			bAnyDifference = true;
			break;
		}
	}
	TestTrue(TEXT("two seeds lay out two different yards"), bAnyDifference);

	// THE SHED IS THE EXCEPTION and must NOT vary: its pose is functional, not decorative.
	TestTrue(TEXT("but the shed still faces the gate in both"),
		A.Stands[0].Heading == B.Stands[0].Heading);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardDropsWhatWillNotFitTest,
	"Airside.Solve.PlotYardDropsWhatWillNotFit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardDropsWhatWillNotFitTest::RunTest(const FString& Parameters)
{
	// One bay wide and one row deep: the shed alone fills it.
	const TArray<FVector2D> Outline = YardRect(400.0, 800.0);
	const PlotYard::FFootprint Footprints[] = { Shed(), Tank(), Pump() };

	const PlotYard::FYard Yard = PlotYard::LayOut(
		Outline, FVector2D(0.0, 0.0), FVector2D(400.0, 0.0), FVector2D(200.0, 0.0),
		Footprints, /*Seed=*/7, Tank());

	// ONE ENTRY PER FOOTPRINT, IN ORDER, even for the ones that did not fit. A compacted
	// array would re-associate a pump's stand with a tank, and the depot would draw the
	// wrong box in the wrong place with nothing to say so.
	if (!TestEqual(TEXT("three footprints in, three stands out"), Yard.Stands.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("something had to be dropped from a one-bay plot"), Yard.DroppedCount() > 0);
	TestEqual(TEXT("and there is no room for more"), Yard.RoomForMore, 0);

	return true;
}
```

- [ ] **Step 2: Build twice, run**

Expected: all three PASS immediately — `FRandomStream` is already deterministic. If
`PlotYardVariesWithSeed` fails, the sampler is not consuming the stream and Task 2 is wrong.

- [ ] **Step 3: Commit**

```bash
git add Plugins/Airside/Source/AirsideTests/Private/PlotYardTest.cpp
git commit -m "test(solve): the same seed gives the same yard, different seeds do not"
```

---

### Task 4: The presenter scatters instead of gridding

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Build/DepotKit.h`
- Create: `Plugins/Airside/Source/Airside/Private/Build/DepotKit.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp`

**Interfaces:**
- Consumes: Task 2's `PlotYard::LayOut`, `PlotYard::FFootprint`, `PlotYard::FStand`.
- Produces: `AIRSIDE_API PlotYard::FFootprint DepotFootprint(EDepotModule Module)` in
  `Build/DepotKit.h`, which Task 5 also calls; `UPlotPresenter::GetRoomForMore() const`
  returning `int32`. `UPlotPresenter::GetEmptySlotCount()` is REMOVED.

- [ ] **Step 1: Write the failing test**

In `PlotPresenterTest.cpp`, DELETE `FPlotShowsRoomToGrowTest` entirely — it asserts
`GetEmptySlotCount()`, which this task removes, and a test kept alive by renaming the thing
it measures is a test that has stopped meaning anything. Replace it with:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterScattersModulesTest,
	"Airside.Present.PlotPresenterScattersModules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterScattersModulesTest::RunTest(const FString& Parameters)
{
	// COMPOSITION LEVEL, per CLAUDE.md. Every Airside.Solve.PlotYard test would still pass
	// if the presenter called the solver and then drew on a grid anyway, or never called it
	// at all - which is the "declared but never consumed" shape this project has shipped
	// three times.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	if (!TestNotNull(TEXT("a plot presenter"), Actor->GetPlotPresenter())) { return false; }

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Actor->ClearNetwork();
	PlaceDepot(Actor, Depot, 0.0);
	Actor->RebuildMesh();

	const int32 One = Actor->GetPlotPresenter()->GetInstanceCount();
	TestTrue(TEXT("a depot still stands its modules and a fence up"), One > 3);

	// A REBUILD IS IDEMPOTENT, and here that is the whole determinism requirement seen from
	// the outside: RebuildMesh runs on every graph change, and a yard reseeded each time
	// would shift while the player laid a road on the far side of the airport.
	const TArray<FTransform> Before = InstanceTransforms(Actor);
	Actor->RebuildMesh();
	const TArray<FTransform> After = InstanceTransforms(Actor);

	if (!TestEqual(TEXT("a rebuild puts the same number of things up"),
		Before.Num(), After.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < Before.Num(); ++Index)
	{
		TestTrue(*FString::Printf(TEXT("instance %d did not move on rebuild"), Index),
			Before[Index].GetLocation().Equals(After[Index].GetLocation(), 0.0f));
	}

	// AND TWO DEPOTS DO NOT LOOK ALIKE. Seeded off position, so the second depot on the
	// airport must lay out differently from the first - the complaint that started this.
	PlaceDepot(Actor, Depot, 4000.0);
	Actor->RebuildMesh();

	const TArray<FTransform> Both = InstanceTransforms(Actor);
	bool bAnyRotationDiffers = false;
	for (int32 Index = 1; Index < Both.Num(); ++Index)
	{
		if (!Both[Index].GetRotation().Equals(Both[0].GetRotation(), 0.001f))
		{
			bAnyRotationDiffers = true;
			break;
		}
	}
	TestTrue(TEXT("the yard is not one heading repeated"), bAnyRotationDiffers);

	return true;
}
```

Add this helper to the file's anonymous namespace:

```cpp
	/** Every instance transform on the plot component, in order. */
	TArray<FTransform> InstanceTransforms(const ARoadNetworkActor* Actor)
	{
		TArray<FTransform> Out;
		const UInstancedStaticMeshComponent* Boxes = Actor->PlotBoxesComponent.Get();
		if (Boxes == nullptr)
		{
			return Out;
		}
		for (int32 Index = 0; Index < Boxes->GetInstanceCount(); ++Index)
		{
			FTransform T;
			Boxes->GetInstanceTransform(Index, T, /*bWorldSpace=*/true);
			Out.Add(T);
		}
		return Out;
	}
```

**`PlotBoxesComponent` is a guess at the component's name on `ARoadNetworkActor`.** Read
`Present/RoadNetworkActor.h` and use whatever `UPlotPresenter::Initialise` is actually passed
from the actor. Do not invent a name; if the actor exposes no accessor, add one following
`GetPlotPresenter()`'s precedent rather than making the member public.

Add `#include "Components/InstancedStaticMeshComponent.h"` to the test's includes.

- [ ] **Step 2: Build twice, run, verify it fails**

Expected: FAIL to compile — `GetEmptySlotCount` is still referenced nowhere now, but
`InstanceTransforms` needs the component accessor. Fix the accessor, then expect
`PlotPresenterScattersModules` to FAIL on "the yard is not one heading repeated", because
`RebuildFrom` still lays a grid where every heading is identical.

- [ ] **Step 3: Map modules to footprints and call the solver**

Create `Plugins/Airside/Source/Airside/Public/Build/DepotKit.h`. **In `Build/`, not in
`PlotPresenter.cpp`'s anonymous namespace**: Task 5 needs this same table for the tool's
readout, and two copies of it would be two things to keep in agreement - the bug CLAUDE.md
names most often. `Build/` is the right layer because `Tool/` may include it and `Present/`
may too, while `Solve/` may not see `EDepotModule` at all.

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "Solve/PlotYard.h"

/**
 * Each module's own footprint, uu.
	 *
	 * THEY DIFFER, and that is the point: all three were one bay and drawn at bay size, so
	 * scattering them would give a jumble of IDENTICAL boxes. Varied placement is what makes
	 * differing footprints legible, where the grid hid them. Grey-box figures chosen for
	 * legibility, like the heights above - the real ones arrive with the meshes.
	 */
AIRSIDE_API PlotYard::FFootprint DepotFootprint(EDepotModule Module);
```

with the body in `Private/Build/DepotKit.cpp`:

```cpp
#include "Build/DepotKit.h"

PlotYard::FFootprint DepotFootprint(EDepotModule Module)
{
	PlotYard::FFootprint Out;
	switch (Module)
	{
	case EDepotModule::Shed:
		Out.LengthUu = 800.0;
		Out.WidthUu = 400.0;
		// THE ONLY ONE THAT FRONTS THE GATE. A truck drives out of it, so its heading is
		// functional and may not be turned for looks.
		Out.bFrontsTheGate = true;
		return Out;
	case EDepotModule::Tank:
		Out.LengthUu = 500.0;
		Out.WidthUu = 500.0;
		return Out;
	case EDepotModule::Pump:
		Out.LengthUu = 300.0;
		Out.WidthUu = 200.0;
		return Out;
	}
	Out.LengthUu = 400.0;
	Out.WidthUu = 400.0;
	return Out;
}
```

Then in `PlotPresenter.cpp`, add to the anonymous namespace:

```cpp
	/**
	 * A stable seed for one depot.
	 *
	 * POSITION, NOT AN ENTITY HANDLE: handles are slot indices that reuse, and a position is
	 * set once at placement and never changes. Quantised to whole uu because a float that
	 * came back from a save one bit different would re-roll that depot and only that depot.
	 */
	int32 SeedFor(const FEntityInstance& Entity)
	{
		const int32 X = FMath::RoundToInt(Entity.Position.X);
		const int32 Y = FMath::RoundToInt(Entity.Position.Y);
		return HashCombine(GetTypeHash(X), GetTypeHash(Y));
	}
```

Add `#include "Build/DepotKit.h"` and `#include "Solve/PlotYard.h"` to `PlotPresenter.cpp`.

Then in `RebuildFrom`, replace the whole block from `// THE GRID, NOT FitBays.` down to the
end of the empty-slot loop (everything that computes `Width`, `Depth`, `Grid`, `Placed`, the
module loop and the marker loop) with:

```cpp
		// THE YARD, NOT A GRID. PlotFit::BuildGrid still answers what the PLOT is - the
		// gesture's preview uses it - but where each module stands is a different question
		// with different inputs, and a row of identical boxes all facing one way is what
		// made a built depot read as a placeholder. See the 2026-09-16 design doc.
		TArray<PlotYard::FFootprint> Footprints;
		Footprints.Reserve(Entity.Modules.Num());
		for (const EDepotModule Module : Entity.Modules)
		{
			Footprints.Add(DepotFootprint(Module));
		}

		// The gate is where the fence is left open, which is the entity's own pose - see
		// the fence loop below, which skips the bay nearest exactly this point.
		const PlotYard::FYard Yard = PlotYard::LayOut(Entity.Outline, FrontageA, FrontageB,
			Entity.Position, Footprints, SeedFor(Entity), DepotFootprint(EDepotModule::Tank));

		RoomForMore += Yard.RoomForMore;

		for (int32 I = 0; I < Yard.Stands.Num() && I < Entity.Modules.Num(); ++I)
		{
			const PlotYard::FStand& Stand = Yard.Stands[I];
			if (!Stand.bPlaced)
			{
				++Dropped;
				continue;
			}
			Boxes->AddInstance(BoxAt(Stand.Centre, Stand.Heading,
				Footprints[I].LengthUu, Footprints[I].WidthUu, HeightFor(Entity.Modules[I])),
				/*bWorldSpace=*/true);
			++ModuleBoxes;
		}
```

Delete `RecoverGridSize` and its call — nothing reads the grid size any more. Delete the
`Solve/PlotFit.h` include only if nothing else in the file still uses it; `FrontageA`/
`FrontageB` come from `RecoverFrontage`, which stays.

In `PlotPresenter.h`, replace `GetEmptySlotCount()` and the `EmptySlots` member with:

```cpp
	/** How many more modules would still fit across every plot. For tests and the census. */
	int32 GetRoomForMore() const { return RoomForMore; }
```

```cpp
	/** Counted during the last RebuildFrom. See GetRoomForMore. */
	int32 RoomForMore = 0;

	/** Modules that had nowhere to stand in the last RebuildFrom. */
	int32 Dropped = 0;
```

Reset both beside `GateGaps = 0;` at the top of `RebuildFrom`, and declare
`int32 Dropped = 0;` is a MEMBER, not a local — the census reads it after the loop.

Update the census line:

```cpp
		UE_LOG(LogAirside, Log,
			TEXT("Plots: %d plot(s), %d module box(es), %d dropped, room for %d more, "
				 "%d fence panel(s), %d gate gap(s)"),
			Plots, ModuleBoxes, Dropped, RoomForMore,
			Boxes->GetInstanceCount() - ModuleBoxes, GateGaps);
```

- [ ] **Step 4: Run to verify it passes**

```bash
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject"
```

Expected: the whole suite green. `Airside.Present.PlotPresenterDressesEachBay` and
`PlotPresenterSurvivesDuplication` both assert `GetInstanceCount() > 3`, which still holds —
if either fails, the solver is dropping modules on a plot that should hold them.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Build/DepotKit.h `
        Plugins/Airside/Source/Airside/Private/Build/DepotKit.cpp `
        Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp `
        Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h `
        Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp
git commit -m "feat(present): a depot's yard is laid out, not gridded"
```

---

### Task 5: The readout says how much room is left

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotPlaceToolTest.cpp`

**Interfaces:**
- Consumes: Task 2's `PlotYard::LayOut`; Task 4's `DepotFootprint` from `Build/DepotKit.h`.
- Produces: no new types. The `Expansion slots` fact becomes `Room for`.

- [ ] **Step 1: Write the failing test**

Append to `PlotPlaceToolTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReadoutCountsRoomNotSlotsTest,
	"Airside.Tool.PlotReadoutCountsRoomNotSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReadoutCountsRoomNotSlotsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	DrawPlot(Tool, Actor, FVector2D(0.0, 200.0), FVector2D(2400.0, 200.0),
		FVector2D(1200.0, 2400.0));

	FToolReadoutCollector Collector;
	Tool.BuildReadout(PlotAt(Actor, FVector2D(1200.0, 2400.0)), Collector);

	// "EXPANSION SLOTS" WAS A CLAIM ABOUT BAYS, and modules no longer stand in bays. A fact
	// whose name survived its meaning is worse than one that was removed: the player reads
	// a number that describes a structure the plot does not have.
	const TPair<FString, FString>* Slots = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Expansion slots"); });
	TestNull(TEXT("the bay-slot fact is gone"), Slots);

	const TPair<FString, FString>* Room = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Room for"); });
	if (!TestNotNull(TEXT("a Room for fact"), Room)) { return false; }

	// A SIX BY THREE PLOT HAS ROOM TO SPARE for three small modules, so the number is not
	// zero. The exact figure is the sampler's to decide and asserting it would pin an
	// implementation detail; that it is positive is the claim.
	TestTrue(TEXT("a large plot reports room to grow"), FCString::Atoi(**Room) > 0);

	return true;
}
```

Note: `**Room` dereferences the `FString` value to a `TCHAR*` — if that reads awkwardly in
context, use `FCString::Atoi(*Room->Value)`, which is the same thing said plainly.

- [ ] **Step 2: Build twice, run, verify it fails**

Expected: FAIL — `Expansion slots` is still emitted and `Room for` does not exist.

- [ ] **Step 3: Replace the fact**

In `FPlotPlaceTool::BuildReadout`, replace:

```cpp
	Sink.Fact(TEXT("Expansion slots"),
		FString::FromInt(ShownWidth * ShownDepth - Placed));
```

with a call through the same solver the presenter uses, so the number previewed is the number
built:

```cpp
	// THE SAME SOLVER THE PRESENTER RUNS, on the rectangle being dragged. A slot count
	// computed as width * depth - placed would be a second opinion about how much fits, and
	// it would disagree with the yard the player gets the moment a module does not fit.
	const TArray<FVector2D> Outline = PlotFit::GridOutline(FrontA, FrontB, ShownWidth, ShownDepth);
	TArray<PlotYard::FFootprint> Footprints;
	for (const EDepotModule Module : Modules)
	{
		Footprints.Add(DepotFootprint(Module));
	}
	const PlotYard::FYard Yard = PlotYard::LayOut(Outline, FrontA, FrontB,
		(FrontA + FrontB) * 0.5, Footprints, PreviewSeed, DepotFootprint(EDepotModule::Tank));

	Sink.Fact(TEXT("Room for"), FString::FromInt(Yard.RoomForMore));
```

`Modules` is `FPlotPlaceTool`'s own member and `FrontA`/`FrontB` are already computed in that
function for the other facts — reuse them rather than recomputing.

`DepotFootprint` comes from `Build/DepotKit.h`, created in Task 4 - add
`#include "Build/DepotKit.h"` and `#include "Solve/PlotYard.h"` here. `Tool/` including
`Build/` is allowed by the include-direction lint; `Tool/` including `Present/` is not, which
is why the table lives where it does.

**`PreviewSeed` is a named constant in this file**, not a hash of a position: the plot is not
placed yet and has no position to seed from. It must be FIXED rather than drawn afresh per
frame, or the ghost's modules would shimmer while the player drags the plot - the same
twitching the presenter's seed exists to prevent, one stage earlier.

**The yard previewed here will not be the yard that gets built**, because the committed one is
seeded off where the plot landed. Only the COUNT is being previewed, and the count is all the
fact claims. Do not be tempted to draw these stands in the ghost: that WOULD be a preview
disagreeing with the result, which is the one thing this codebase will not have.

- [ ] **Step 4: Run to verify it passes, and check the fact order still reads**

```bash
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject"
```

Expected: the whole suite green, including `Airside.Tool.PlotReadoutMatchesPreview`, which
asserts the `Bays` fact and the "No room to grow" warning — neither changes here.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotPlaceToolTest.cpp
git commit -m "feat(tool): the readout counts room to grow, from the solver that lays it out"
```

---

### Task 6: Record what this overturned

**Files:**
- Modify: `docs/superpowers/specs/2026-09-15-plot-built-buildings-design.md`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing. This task exists because a rule nobody knows existed is the failure the
  refactor contract names, and §7.1 of that spec is now false.

- [ ] **Step 1: Mark §7.1 superseded**

In `2026-09-15-plot-built-buildings-design.md` §7.1, leave the existing paragraph in place and
append:

```markdown
> **Superseded 2026-09-16** by `2026-09-16-organic-module-placement-design.md`. Modules take
> free positions and headings, so they are no longer adjacent and their stubs no longer meet.
> **Pipework between modules is not modelled** - a named fidelity debt, in the same class as
> "trucks pop at the gate". The honest route to getting it back is a mated PAIR placed as one
> rigid object, not the procedural pipe solver this section rejected.
```

Leave §4 alone: it is still the PLOT's contract — frontage, gate, pad — and only module
placement moved.

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-09-15-plot-built-buildings-design.md
git commit -m "docs(spec): §7.1's stub mating did not survive free rotation"
```

---

## Verifying the iteration, not just the build

After Task 6, in PIE:

1. Press `0`, anchor on a service road, drag width, drag depth, press Build.
2. Build a SECOND depot elsewhere on the same road with the same mix.
3. The two yards should not look alike: different headings, different positions.
4. Lay a road somewhere else entirely. **Neither yard should move.** That is the determinism
   requirement seen with your own eyes, and it is the one thing no test here can show you.
5. `python Tools/Mcp.py log LogAirside` for the census:
   `Plots: 2 plot(s), 6 module box(es), 0 dropped, room for N more, ...`

What would say the placement is still wrong: modules that read as scattered DEBRIS rather
than as a working yard (the heading jitter is too wide, or the quarter turns should be
dropped for the shed-like modules), or a yard so sparse it looks abandoned (`ClearanceUu` is
too large). Both are one constant, named in `PlotYard.h`, and §11 of the design doc says they
become settings rather than being retyped.
