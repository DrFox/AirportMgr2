# Plot-Built Fuel Depot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The player draws a polygon plot against a service road; the game fits 4 m bays
along the road edge, the player picks a mix of shed / tank / pump modules into them, and the
depot's truck count follows the shed count.

**Architecture:** A dependency-free bay solver in `Solve/`; the drawn outline and the chosen
modules stored on `FEntityInstance`; a new `FPlotDrawTool` replacing `FStandPlaceTool` behind
the existing fuel-depot key; grey-box presentation as scaled engine cubes in one instanced
static mesh component. `UFuelService` is not touched - it keeps reading `Instance.Trucks`.

**Tech Stack:** UE 5.8, C++. Airside plugin (`Model/`, `Solve/`, `Tool/`, `Present/`).
Automation tests via `IMPLEMENT_SIMPLE_AUTOMATION_TEST`.

**Spec:** `docs/superpowers/specs/2026-09-15-plot-built-buildings-design.md`

## Global Constraints

- **Units are uu, and 100 uu = 1 m.** A 4 m bay is `400.0`; an 8 m bay depth is `800.0`.
  Confirmed against `FootprintExtent = FVector2D(600.0, 400.0)` documented as 12 m x 8 m.
- **Headings are radians**, everywhere `PlaceEntity` is involved.
- **`Solve/` may include `CoreMinimal.h` and nothing else.** No engine types beyond it. This
  is enforced by `Tools/Check-Architecture.ps1`.
- **`Model/` must not dereference the `Entities/` layer.** Anything `UEntityDefinition` knows
  that the model needs is *captured at placement* and passed in.
- **A UENUM must live in a UHT-parsed header** (one with a `.generated.h`). A plain enum in
  `Tool/RoadEditTarget.h` is invisible to UHT and a forward declaration does not help.
- **Build:** `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE`
  The `-NoHotReloadFromIDE` is correct **because this is a worktree** and must never be used
  on the main checkout.
- **Test:** `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject"`
  Read its `N test(s) run, N failed, N crashed` line. **Never trust the exit code** - a
  crashing test used to report green.
- **A new test .cpp needs two builds.** The first reports `Result: Succeeded` without having
  compiled it. Build twice before believing a new test file's absence of failures.
- **Test names must be distinct leaves.** A bare-named test vanishes from the automation tree
  once a dotted child exists, and only the run count catches it.

---

### Task 1: The bay solver

Pure geometry, no world, no engine types. Built first because everything else consumes it.

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Solve/PlotFit.h`
- Create: `Plugins/Airside/Source/Airside/Private/Solve/PlotFit.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/PlotFitTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `PlotFit::FPlotBay { FVector2D Centre; double Heading; }`,
  `PlotFit::EPlotRefusal { None, TooSmall, NoFrontage }`,
  `PlotFit::FPlotFit { TArray<FPlotBay> Bays; bool bFits; EPlotRefusal Why; }`,
  `PlotFit::FitBays(TArrayView<const FVector2D> Outline, FVector2D FrontageA, FVector2D FrontageB)`,
  and the constants `PlotFit::BayWidthUu = 400.0`, `PlotFit::BayDepthUu = 800.0`.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/PlotFitTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/PlotFit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** An axis-aligned rectangle, CCW, with its SOUTH edge (y = 0) as the frontage. */
	TArray<FVector2D> Rect(double Width, double Depth)
	{
		return { FVector2D(0.0, 0.0), FVector2D(Width, 0.0),
		         FVector2D(Width, Depth), FVector2D(0.0, Depth) };
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotFitBaysTest,
	"Airside.Solve.PlotFitBays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotFitBaysTest::RunTest(const FString& Parameters)
{
	// 12 m of frontage is three 4 m bays exactly - the Tier 1 depot as drawn.
	{
		const TArray<FVector2D> Outline = Rect(1200.0, 800.0);
		const PlotFit::FPlotFit Fit = PlotFit::FitBays(
			Outline, FVector2D(0.0, 0.0), FVector2D(1200.0, 0.0));
		TestTrue(TEXT("12 m of frontage fits"), Fit.bFits);
		TestEqual(TEXT("and gives exactly three bays"), Fit.Bays.Num(), 3);
	}

	// 11 m floors to two. The concept sheet's 11 m was a drawing, not a decision - a
	// solver that rounded up would silently give the player a bay they did not draw.
	{
		const TArray<FVector2D> Outline = Rect(1100.0, 800.0);
		const PlotFit::FPlotFit Fit = PlotFit::FitBays(
			Outline, FVector2D(0.0, 0.0), FVector2D(1100.0, 0.0));
		TestEqual(TEXT("11 m floors to two bays, never rounds up"), Fit.Bays.Num(), 2);
	}

	// Under one bay is a refusal WITH A REASON, because the tool has to say which.
	{
		const TArray<FVector2D> Outline = Rect(300.0, 800.0);
		const PlotFit::FPlotFit Fit = PlotFit::FitBays(
			Outline, FVector2D(0.0, 0.0), FVector2D(300.0, 0.0));
		TestFalse(TEXT("3 m of frontage does not fit"), Fit.bFits);
		TestEqual(TEXT("and says why, so the tool can tell the player"),
			static_cast<int32>(Fit.Why), static_cast<int32>(PlotFit::EPlotRefusal::TooSmall));
	}

	// Too SHALLOW is also TooSmall: a 12 m x 2 m strip has frontage but no room for a bay.
	{
		const TArray<FVector2D> Outline = Rect(1200.0, 200.0);
		const PlotFit::FPlotFit Fit = PlotFit::FitBays(
			Outline, FVector2D(0.0, 0.0), FVector2D(1200.0, 0.0));
		TestFalse(TEXT("a plot shallower than one bay does not fit"), Fit.bFits);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotFitFacesAwayFromRoadTest,
	"Airside.Solve.PlotFitFacesAwayFromRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotFitFacesAwayFromRoadTest::RunTest(const FString& Parameters)
{
	// BuildFuelDepot: "+X FACES AWAY FROM THE ROAD... the truck drives out behind it",
	// and its comment records that this was once written the wrong way round. Pin the
	// corrected statement: with the frontage on y = 0 and the plot to the north, every
	// bay's +X must point north, away from the road.
	const TArray<FVector2D> Outline = Rect(1200.0, 800.0);
	const PlotFit::FPlotFit Fit = PlotFit::FitBays(
		Outline, FVector2D(0.0, 0.0), FVector2D(1200.0, 0.0));

	if (!TestTrue(TEXT("the plot fits"), Fit.bFits)) { return false; }

	for (const PlotFit::FPlotBay& Bay : Fit.Bays)
	{
		const FVector2D Forward(FMath::Cos(Bay.Heading), FMath::Sin(Bay.Heading));
		TestTrue(TEXT("the bay's +X points away from the frontage, not at it"),
			Forward.Y > 0.9);
		TestTrue(TEXT("and the bay sits inside the plot, not on its boundary"),
			Bay.Centre.Y > 0.0 && Bay.Centre.Y < 800.0);
	}
	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Run the build command from Global Constraints.
Expected: FAIL - `Cannot open include file: 'Solve/PlotFit.h'`.

- [ ] **Step 3: Write the header**

Create `Plugins/Airside/Source/Airside/Public/Solve/PlotFit.h`:

```cpp
#pragma once

#include "CoreMinimal.h"

/**
 * Fitting module bays into a drawn plot, against the edge that faces the road.
 *
 * DEPENDENCY-FREE, like every other Solve/ header: CoreMinimal.h and nothing else. In
 * particular NOT FTransform2D, which lives in Math/TransformCalculus2D.h and is not pulled
 * in by CoreMinimal.h - a bay is a centre and a heading, which is all a placement needs.
 *
 * WHICH EDGE IS THE FRONTAGE IS NOT ASKED HERE. Finding the nearest road needs the network,
 * which this layer may not see, so the caller picks the edge and hands it in. See
 * URoadEditFacade::FindFrontageEdge.
 */
namespace PlotFit
{
	/** 4 m. The Tier 1 depot as drawn is three of these, which is where the number came
	 *  from - see the design doc §3.2. Everything else keys off it. */
	inline constexpr double BayWidthUu = 400.0;

	/** 8 m, the concept sheet's site depth. One row only: a plot drawn deeper is yard. */
	inline constexpr double BayDepthUu = 800.0;

	enum class EPlotRefusal : uint8
	{
		None,
		/** Less than one bay of frontage, or shallower than one bay. */
		TooSmall,
		/** The caller found no road-facing edge to fit against. */
		NoFrontage
	};

	struct FPlotBay
	{
		FVector2D Centre = FVector2D::ZeroVector;

		/** Radians. +X points AWAY from the frontage, because the truck drives out of the
		 *  back of the installation - see UEntityDefinition::BuildFuelDepot. */
		double Heading = 0.0;
	};

	struct FPlotFit
	{
		TArray<FPlotBay> Bays;
		bool bFits = false;
		EPlotRefusal Why = EPlotRefusal::None;
	};

	/**
	 * Lay bays along the frontage edge, inside Outline.
	 *
	 * Outline is a simple polygon, closed implicitly - the last point joins the first and
	 * the array does NOT repeat it.
	 */
	AIRSIDE_API FPlotFit FitBays(TArrayView<const FVector2D> Outline,
		FVector2D FrontageA, FVector2D FrontageB);
}
```

- [ ] **Step 4: Write the implementation**

Create `Plugins/Airside/Source/Airside/Private/Solve/PlotFit.cpp`:

```cpp
#include "Solve/PlotFit.h"

namespace
{
	/** Winding-number point-in-polygon. Robust for the non-convex plots a freeform
	 *  gesture produces, which a convexity assumption would silently mis-answer. */
	bool Contains(TArrayView<const FVector2D> Outline, const FVector2D& P)
	{
		int32 Winding = 0;
		const int32 Count = Outline.Num();
		for (int32 I = 0; I < Count; ++I)
		{
			const FVector2D& A = Outline[I];
			const FVector2D& B = Outline[(I + 1) % Count];
			const double Side = (B.X - A.X) * (P.Y - A.Y) - (P.X - A.X) * (B.Y - A.Y);
			if (A.Y <= P.Y)
			{
				if (B.Y > P.Y && Side > 0.0) { ++Winding; }
			}
			else if (B.Y <= P.Y && Side < 0.0)
			{
				--Winding;
			}
		}
		return Winding != 0;
	}
}

PlotFit::FPlotFit PlotFit::FitBays(TArrayView<const FVector2D> Outline,
	FVector2D FrontageA, FVector2D FrontageB)
{
	FPlotFit Result;

	if (Outline.Num() < 3)
	{
		Result.Why = EPlotRefusal::TooSmall;
		return Result;
	}

	const FVector2D Along = FrontageB - FrontageA;
	const double Length = Along.Size();
	if (Length < BayWidthUu)
	{
		Result.Why = EPlotRefusal::TooSmall;
		return Result;
	}

	const FVector2D Unit = Along / Length;

	// Inward is the frontage's left normal. The outline is CCW (FApronSurface's contract),
	// so walking A->B along a boundary edge keeps the interior on the left - which is what
	// makes this a derivation rather than a guess that would flip on a mirrored plot.
	const FVector2D Inward(-Unit.Y, Unit.X);

	// HEADING IS INWARD, not along the frontage: +X faces away from the road.
	const double Heading = FMath::Atan2(Inward.Y, Inward.X);

	const int32 BayCount = FMath::FloorToInt(Length / BayWidthUu);

	for (int32 I = 0; I < BayCount; ++I)
	{
		const double AlongOffset = (static_cast<double>(I) + 0.5) * BayWidthUu;
		const FVector2D Centre = FrontageA + Unit * AlongOffset + Inward * (BayDepthUu * 0.5);

		// Every corner inside, not merely the centre: a centre-only test accepts a bay
		// hanging out of a notch in a non-convex plot, and the player would see a shed
		// standing on the grass.
		const FVector2D HalfAlong = Unit * (BayWidthUu * 0.5);
		const FVector2D HalfDeep = Inward * (BayDepthUu * 0.5);
		const bool bInside =
			Contains(Outline, Centre + HalfAlong + HalfDeep) &&
			Contains(Outline, Centre + HalfAlong - HalfDeep) &&
			Contains(Outline, Centre - HalfAlong + HalfDeep) &&
			Contains(Outline, Centre - HalfAlong - HalfDeep);

		if (!bInside)
		{
			continue;
		}

		FPlotBay Bay;
		Bay.Centre = Centre;
		Bay.Heading = Heading;
		Result.Bays.Add(Bay);
	}

	Result.bFits = Result.Bays.Num() > 0;
	if (!Result.bFits)
	{
		Result.Why = EPlotRefusal::TooSmall;
	}
	return Result;
}
```

- [ ] **Step 5: Build TWICE, then run the tests**

A new test .cpp needs two builds - the first says `Result: Succeeded` without compiling it.

Run the build command twice, then:
`./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject" -Filter Airside.Solve`

Expected: the run line names both `Airside.Solve.PlotFitBays` and
`Airside.Solve.PlotFitFacesAwayFromRoad`, 0 failed, 0 crashed. **If the run count did not go
up by two, the tests did not register** - check the names are distinct leaves.

- [ ] **Step 6: Run the architecture lint**

`./Tools/Check-Architecture.ps1`

Expected: pass. It enforces that `Solve/` includes nothing beyond `CoreMinimal.h`; if
`PlotFit.h` picked up another include it fails here rather than in review.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Solve/PlotFit.h Plugins/Airside/Source/Airside/Private/Solve/PlotFit.cpp Plugins/Airside/Source/AirsideTests/Private/PlotFitTest.cpp
git commit -m "feat(solve): bays fitted to the edge of a plot that faces the road"
```

---

### Task 2: The placement struct, without breaking thirty callers

`URoadNetwork::PlaceEntity` already carries three captured facts and its own comment says a
fourth becomes a struct. The outline and the module list are the fourth and fifth. But there
are roughly thirty existing call sites, almost all tests, and churning them is not this
slice's work - so the struct arrives as an **overload**, and the old signature stays as a
forwarder. That is the refactor contract's own rule: every interface stays reachable at its
old name, as a forwarder if the logic moved.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h` (add `FDepotModule`, `EDepotModule`)
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h:383` (add the struct + overload)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp:860`
- Create: `Plugins/Airside/Source/AirsideTests/Private/PlotPlacementTest.cpp`

**Interfaces:**
- Consumes: Task 1's `PlotFit::FPlotBay`.
- Produces: `EDepotModule { Shed, Tank, Pump }`;
  `FEntityPlacement { UEntityDefinition* Definition; TConstArrayView<FEntityAnchor> Anchors; FVector2D Position; double Heading; double DesignWingspan; EServiceRole PoseRole; TArray<FVector2D> Outline; TArray<EDepotModule> Modules; }`;
  `URoadNetwork::PlaceEntity(const FEntityPlacement&)` returning `FEntityInstanceId`.
  Note there is **no `Trucks` field** - see Task 3.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/PlotPlacementTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPlacementCarriesOutlineTest,
	"Airside.Entities.PlotPlacementCarriesOutline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPlacementCarriesOutlineTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>();
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	FEntityPlacement Placement;
	Placement.Definition = Depot;
	Placement.Anchors = Depot->Anchors;
	Placement.Position = FVector2D::ZeroVector;
	Placement.Heading = 0.0;
	Placement.PoseRole = EServiceRole::Fuel;
	Placement.Outline = { FVector2D(0.0, 0.0), FVector2D(1200.0, 0.0),
	                      FVector2D(1200.0, 800.0), FVector2D(0.0, 800.0) };
	Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };

	const FEntityInstanceId Placed = Net->PlaceEntity(Placement);
	if (!TestTrue(TEXT("the depot is placed"), Placed.IsSet())) { return false; }

	const FEntityInstance* Instance = Net->GetEntity(Placed);
	if (!TestNotNull(TEXT("and readable back"), Instance)) { return false; }

	TestEqual(TEXT("the drawn outline survives placement"), Instance->Outline.Num(), 4);
	TestEqual(TEXT("and so does the chosen mix"), Instance->Modules.Num(), 3);

	// The whole point of the overload: the THREE-ARGUMENT form still compiles and still
	// means what it meant. A forwarder nobody exercises is a forwarder that rots.
	const FEntityInstanceId Old = Net->PlaceEntity(
		Depot, Depot->Anchors, FVector2D(5000.0, 0.0), 0.0);
	TestTrue(TEXT("the pre-plot signature still places"), Old.IsSet());

	const FEntityInstance* Legacy = Net->GetEntity(Old);
	if (!TestNotNull(TEXT("and is readable"), Legacy)) { return false; }
	TestEqual(TEXT("with no outline, which is what a plain plop means"),
		Legacy->Outline.Num(), 0);

	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Build. Expected: FAIL - `'FEntityPlacement': undeclared identifier`.

- [ ] **Step 3: Add the module enum and the instance fields**

In `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h`, above `FEntityInstance`:

```cpp
/**
 * One module in a depot's plot: what occupies a bay.
 *
 * A UENUM in this UHT-parsed header rather than a plain enum beside the tool, for the reason
 * EPlaceableEntity records at its own declaration: UHT cannot resolve a type declared in a
 * header with no .generated.h, and a forward declaration does not satisfy it either.
 *
 * WHAT EACH ONE DRIVES, and how real it is today:
 *   Shed - the truck count. LIVE: UFuelService gates dispatch on FEntityInstance::Trucks.
 *   Pump - the dwell. LIVE-ISH: UFuelService::DwellSeconds exists and is divided by these.
 *   Tank - storage. INERT: no fuel inventory exists anywhere yet. Counted, never read.
 */
UENUM()
enum class EDepotModule : uint8
{
	Shed,
	Tank,
	Pump
};
```

Then add to `FEntityInstance`, after `Trucks`:

```cpp
	/**
	 * The plot the player drew, in the entity's own local space. Empty means an ordinary
	 * plop with no plot - which is every stand, and every depot placed before this existed.
	 *
	 * CLOSED IMPLICITLY: the last point joins the first and the array does NOT repeat it,
	 * the same contract UEntityDefinition::ServiceLoop states and for the same reason - a
	 * repeated point is a value that must agree with another value in the same array.
	 */
	UPROPERTY() TArray<FVector2D> Outline;

	/**
	 * What the player put in the bays, in bay order. Empty for a plotless entity.
	 *
	 * THE FOURTH CAPTURED FACT - see Trucks above, whose comment called for exactly this:
	 * the three trailing defaulted parameters became FEntityPlacement rather than a fourth.
	 */
	UPROPERTY() TArray<EDepotModule> Modules;
```

- [ ] **Step 3b: Retire FootprintExtent's old meaning**

In `Plugins/Airside/Source/Airside/Private/Entities/EntityDefinition.cpp`, in `BuildFuelDepot`,
replace the footprint line and its comment:

```cpp
	// HALF-extents of ONE BAY: 4 m by 8 m. It used to be the whole site - 12 m by 8 m,
	// "a tank, a pump, and room to turn a bowser round" - but the site is now whatever
	// the player drew, and the only fixed extent left is the module that fills a bay.
	//
	// The old 12 m was not wrong, and it is where the 4 m bay came from: it is exactly
	// three of them, which is the Tier 1 depot as drawn on the concept sheet.
	Definition->FootprintExtent = FVector2D(200.0, 400.0);
```

Then delete the `Trucks = 1` line beneath it and say why:

```cpp
	// NO TRUCK COUNT. A depot's trucks are its sheds, counted at placement - see
	// URoadNetwork::PlaceEntity(const FEntityPlacement&). A number here could only ever
	// disagree with the sheds the player actually built.
```

**`FuelDepotAnchorTest.cpp:73` asserts `Depot->Trucks == 1` and will now fail.** That is the
test telling the truth: change it to assert the definition no longer carries a count, and
move the truck-count claim to Task 3's test, which places a depot and counts its sheds.

- [ ] **Step 4: Add the struct and the overload**

In `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h`, above the existing
`PlaceEntity` declaration at line 383:

```cpp
/**
 * Everything one placement needs, replacing the three trailing defaulted parameters that
 * FEntityInstance::Trucks' comment warned would not survive a fourth.
 *
 * NO Trucks FIELD, deliberately: it is now DERIVED from the shed count in Modules rather
 * than captured separately, so a caller cannot state a truck count that disagrees with the
 * sheds the player actually built. The old three-argument overload still takes one, because
 * a plotless caller has no modules to derive it from.
 */
USTRUCT()
struct AIRSIDE_API FEntityPlacement
{
	GENERATED_BODY()

	UEntityDefinition* Definition = nullptr;
	TConstArrayView<FEntityAnchor> Anchors;
	FVector2D Position = FVector2D::ZeroVector;

	/** Radians, as everywhere else PlaceEntity is involved. */
	double Heading = 0.0;

	double DesignWingspan = 0.0;
	EServiceRole PoseRole = EServiceRole::Aircraft;

	/** The drawn plot. Empty for an ordinary plop. */
	TArray<FVector2D> Outline;

	/** What fills the bays. Empty for an ordinary plop. */
	TArray<EDepotModule> Modules;
};
```

And beside the existing declaration:

```cpp
	/**
	 * Place from a full description, including a drawn plot and its modules.
	 *
	 * THE OVERLOAD IS THE NEW HOME OF THE LOGIC and the old signature forwards to it, rather
	 * than the other way round. The refactor contract's rule: every interface stays reachable
	 * at its old name, as a forwarder if the logic moved. Roughly thirty call sites - almost
	 * all tests - use the old form, and churning them is not this slice's work.
	 */
	FEntityInstanceId PlaceEntity(const FEntityPlacement& Placement);
```

- [ ] **Step 5: Move the body and forward the old signature**

In `Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp`, rename the existing
definition at line 860 to take `const FEntityPlacement&`, replacing each parameter reference
with `Placement.<Field>`. Then add, beside it, the forwarder:

```cpp
FEntityInstanceId URoadNetwork::PlaceEntity(UEntityDefinition* Definition,
	TConstArrayView<FEntityAnchor> Anchors, const FVector2D& Position, double Heading,
	double DesignWingspan, EServiceRole PoseRole, int32 Trucks)
{
	FEntityPlacement Placement;
	Placement.Definition = Definition;
	Placement.Anchors = Anchors;
	Placement.Position = Position;
	Placement.Heading = Heading;
	Placement.DesignWingspan = DesignWingspan;
	Placement.PoseRole = PoseRole;

	const FEntityInstanceId Placed = PlaceEntity(Placement);

	// A PLOTLESS caller states its truck count outright, because it has no modules to
	// derive one from. Written after the fact rather than passed through the struct, so
	// the struct has exactly one way to mean a truck count - the sheds.
	if (Placed.IsSet())
	{
		if (FEntityInstance* Instance = FindEntityMutable(Placed))
		{
			Instance->Trucks = Trucks;
		}
	}
	return Placed;
}
```

**Before writing this, confirm the mutable accessor's real name.** `GetEntity` returns
`const FEntityInstance*`. Search `RoadNetwork.h` for the non-const path the file already uses
internally and use that name; do not invent `FindEntityMutable` if the codebase calls it
something else.

- [ ] **Step 6: Build twice and run the tests**

`./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject"`

Expected: `Airside.Entities.PlotPlacementCarriesOutline` passes, and **the whole suite still
passes** - the forwarder's entire purpose is that the thirty existing callers are unchanged.
Read the `N test(s) run, N failed, N crashed` line; a drop in N means tests stopped
registering, not that they passed.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp Plugins/Airside/Source/AirsideTests/Private/PlotPlacementTest.cpp
git commit -m "feat(model): a placement struct carrying the drawn plot and its modules"
```

---

### Task 3: Trucks follow the sheds

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp` (the struct overload)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotPlacementTest.cpp` (add a test)

**Interfaces:**
- Consumes: Task 2's `FEntityPlacement`, `EDepotModule`.
- Produces: the invariant `Instance.Trucks == count of EDepotModule::Shed in Modules`, for
  any placement with a non-empty `Modules`.

- [ ] **Step 1: Write the failing test**

Append to `PlotPlacementTest.cpp`, before the `#endif`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrucksDerivedFromShedsTest,
	"Airside.Entities.TrucksDerivedFromSheds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrucksDerivedFromShedsTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>();
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	auto PlaceWith = [&](const TArray<EDepotModule>& Modules, double X)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(X, 0.0);
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = { FVector2D(0.0, 0.0), FVector2D(1200.0, 0.0),
		                      FVector2D(1200.0, 800.0), FVector2D(0.0, 800.0) };
		Placement.Modules = Modules;
		return Net->PlaceEntity(Placement);
	};

	// Two sheds is two trucks. The number UFuelService counts dispatches against, so this
	// is the one module whose effect is real today.
	{
		const FEntityInstance* Two = Net->GetEntity(
			PlaceWith({ EDepotModule::Shed, EDepotModule::Shed, EDepotModule::Tank }, 0.0));
		if (!TestNotNull(TEXT("two sheds placed"), Two)) { return false; }
		TestEqual(TEXT("two sheds is two trucks"), Two->Trucks, 2);
	}

	// No shed is no trucks - allowed, because a depot mid-build is a legitimate state and
	// the census warns rather than forbidding. It must not silently become one.
	{
		const FEntityInstance* None = Net->GetEntity(
			PlaceWith({ EDepotModule::Tank, EDepotModule::Pump }, 5000.0));
		if (!TestNotNull(TEXT("a shedless depot still places"), None)) { return false; }
		TestEqual(TEXT("no shed is no trucks, not a default of one"), None->Trucks, 0);
	}

	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

Build twice, run the suite.
Expected: FAIL - `two sheds is two trucks` reports 0, because nothing derives it yet.

- [ ] **Step 3: Derive it in the struct overload**

In the `FEntityPlacement` overload of `URoadNetwork::PlaceEntity`, where the instance is
filled in, replace any direct truck assignment with:

```cpp
	// DERIVED, not captured. A shed is a truck; the player's mix IS the fleet size, so a
	// separately-stated count could only ever disagree with the sheds they built.
	// UFuelService is untouched by this: it still reads Instance.Trucks and always will.
	int32 Sheds = 0;
	for (const EDepotModule Module : Placement.Modules)
	{
		if (Module == EDepotModule::Shed)
		{
			++Sheds;
		}
	}
	Instance.Trucks = Sheds;
```

- [ ] **Step 4: Run to verify it passes**

Build, run the suite.
Expected: `Airside.Entities.TrucksDerivedFromSheds` passes, **and every AirportOps fuel test
still passes** - that is the claim that `UFuelService` needed no change.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp Plugins/Airside/Source/AirsideTests/Private/PlotPlacementTest.cpp
git commit -m "feat(model): a depot's trucks are its sheds, counted"
```

---

### Task 4: Which edge faces the road

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacadeSurfaces.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/FrontageEdgeTest.cpp`

**Interfaces:**
- Consumes: Task 1's `PlotFit`.
- Produces: `URoadEditFacade::FindFrontageEdge(const TArray<FVector2D>& Outline, FVector2D& OutA, FVector2D& OutB) const` returning `bool`.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/FrontageEdgeTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadEditFacade.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFrontageEdgeFacesTheRoadTest,
	"Airside.Entities.FrontageEdgeFacesTheRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFrontageEdgeFacesTheRoadTest::RunTest(const FString& Parameters)
{
	// A service road along y = -400, and a square plot with its south edge nearest it.
	// The south edge must win - not merely "an edge was returned", which a broken
	// implementation returning Outline[0]..Outline[1] would also satisfy by luck. So the
	// plot is drawn with its FIRST edge on the north, where the road is not.
	URoadNetwork* Net = NewObject<URoadNetwork>();

	const FGuidelineNodeId West = Net->AddGuidelineNode(FVector2D(-5000.0, -400.0));
	const FGuidelineNodeId East = Net->AddGuidelineNode(FVector2D(5000.0, -400.0));
	FGuidelineEdge Road;
	Road.A = West;
	Road.B = East;
	Road.Control = FVector2D(0.0, -400.0);
	Road.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
	Road.Direction = EGuidelineDir::Bidirectional;
	Road.Width = 600.0;
	Road.bDerived = true;
	Net->AddGuidelineEdge(MoveTemp(Road));

	// North edge first, then west, then south, then east.
	const TArray<FVector2D> Outline = {
		FVector2D(1200.0, 800.0), FVector2D(0.0, 800.0),
		FVector2D(0.0, 0.0), FVector2D(1200.0, 0.0) };

	URoadEditFacade* Facade = NewObject<URoadEditFacade>();
	Facade->AttachNetworkForTest(Net);

	FVector2D A = FVector2D::ZeroVector;
	FVector2D B = FVector2D::ZeroVector;
	const bool bFound = Facade->FindFrontageEdge(Outline, A, B);

	if (!TestTrue(TEXT("a frontage edge is found"), bFound)) { return false; }
	TestEqual(TEXT("the SOUTH edge wins, being nearest the road"), A.Y, 0.0);
	TestEqual(TEXT("and it is the south edge's other end too"), B.Y, 0.0);

	// No road at all is a refusal, not edge zero. A plot floating in a field must say so.
	URoadNetwork* Empty = NewObject<URoadNetwork>();
	URoadEditFacade* Bare = NewObject<URoadEditFacade>();
	Bare->AttachNetworkForTest(Empty);
	TestFalse(TEXT("no road means no frontage, not a lucky first edge"),
		Bare->FindFrontageEdge(Outline, A, B));

	return true;
}

#endif
```

**Before writing this test, confirm how a facade is given a network in tests.**
`AttachNetworkForTest` is a guess at the name. Read
`Plugins/Airside/Source/AirsideTests/Private/SelectToolTest.cpp` and
`AirsideTestFixtures.h`, which already construct a facade over a network, and use whatever
they use. If they build the facade through `ARoadNetworkActor`, do that instead and drop the
bare `NewObject` pair.

- [ ] **Step 2: Run to verify it fails**

Build twice. Expected: FAIL - no member `FindFrontageEdge`.

- [ ] **Step 3: Implement it**

In `RoadEditFacade.h`, public:

```cpp
	/**
	 * Which edge of Outline faces a ground-vehicle guideline - the plot's road frontage.
	 *
	 * HERE AND NOT IN Solve/PlotFit, because finding the nearest road needs the network and
	 * Solve/ may not see it. The split is what keeps the bay solver dependency-free and
	 * world-free testable; PlotFit is handed the answer.
	 *
	 * False when no guideline admitting GroundVehicle is within reach of any edge, which is
	 * a plot drawn in a field. The caller refuses and says so - it must NOT fall back to
	 * edge zero, which would aim the depot at whatever the player happened to click first.
	 */
	bool FindFrontageEdge(const TArray<FVector2D>& Outline,
		FVector2D& OutA, FVector2D& OutB) const;
```

In `RoadEditFacadeSurfaces.cpp`:

```cpp
bool URoadEditFacade::FindFrontageEdge(const TArray<FVector2D>& Outline,
	FVector2D& OutA, FVector2D& OutB) const
{
	if (Outline.Num() < 3 || Network == nullptr)
	{
		return false;
	}

	// The reach a lead-in is cast over. Shared with FAnchorLink rather than retyped:
	// a plot the tool accepts but the anchor link then cannot join is the worst outcome
	// here, and two separately-typed distances is exactly how that happens.
	const double ReachUu = FAnchorLink::LeadInReachUu;

	double BestDistance = TNumericLimits<double>::Max();
	int32 BestEdge = INDEX_NONE;

	for (int32 I = 0; I < Outline.Num(); ++I)
	{
		const FVector2D& A = Outline[I];
		const FVector2D& B = Outline[(I + 1) % Outline.Num()];
		const FVector2D Midpoint = (A + B) * 0.5;

		double Distance = 0.0;
		if (!Network->FindNearestGuideline(Midpoint, ETraversalClass::GroundVehicle,
			ReachUu, Distance))
		{
			continue;
		}

		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			BestEdge = I;
		}
	}

	if (BestEdge == INDEX_NONE)
	{
		return false;
	}

	OutA = Outline[BestEdge];
	OutB = Outline[(BestEdge + 1) % Outline.Num()];
	return true;
}
```

**`FAnchorLink::LeadInReachUu` and `URoadNetwork::FindNearestGuideline` are both names this
task must confirm before writing.** Open `Plugins/Airside/Source/Airside/Public/Build/AnchorLink.h`
and `Model/RoadNetwork.h` and use the real constant and the real nearest-guideline query. If
no such query exists, add one to `URoadNetwork` in this task with its own test - do not
reimplement a nearest search inside the facade, which would be a second evaluator of a
question `FAnchorLink` already answers.

- [ ] **Step 4: Run to verify it passes**

Build, run `-Filter Airside.Entities`. Expected: `FrontageEdgeFacesTheRoad` passes.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h Plugins/Airside/Source/Airside/Private/Present/RoadEditFacadeSurfaces.cpp Plugins/Airside/Source/AirsideTests/Private/FrontageEdgeTest.cpp
git commit -m "feat(present): the plot edge nearest a service road is its frontage"
```

---

### Task 5: The plot tool, on the key the depot already has

Key `Zero` currently makes `FStandPlaceTool(EPlaceableEntity::FuelDepot)` - press to
position, drag to aim, release. It becomes `FPlotDrawTool`. **This is a swap, not an
addition:** no new key, and `RoadBuildEdModeCommands`' parallel list keeps the name
`FuelDepot`, so the two lists still agree.

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/PlotDrawTool.h`
- Create: `Plugins/Airside/Source/Airside/Private/Tool/PlotDrawTool.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp:67-69` (the `EKeys::Zero` entry)
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadEditTarget.h:178`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h:117`, `RoadNetworkActor.h:418` and their .cpps
- Modify: `Plugins/Airside/Source/AirsideTests/Private/RunwayToolTest.cpp:82`, `TaxiwayWidthTest.cpp:59` (the two test doubles)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/FuelDepotPlaceToolTest.cpp`

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces: `IRoadEditTarget::PlaceEntityInPlot(const TArray<FVector2D>& Outline, const TArray<EDepotModule>& Modules, EPlaceableEntity Kind)` returning `int32`; `FPlotDrawTool`.

- [ ] **Step 1: Find every implementor before changing the interface**

Run:

```bash
grep -rn "PlaceEntity" --include=*.h --include=*.cpp Plugins | grep -v Intermediate
```

Expected: four implementors of the `IRoadEditTarget` virtual - `URoadEditFacade`,
`ARoadNetworkActor`, and the two test doubles in `RunwayToolTest.cpp` and
`TaxiwayWidthTest.cpp`. **All four must gain the new virtual or the build breaks.** This is
the "check where a list is CONSUMED" rule: the interface is one list and its implementors are
another, and they must agree.

- [ ] **Step 2: Write the failing test**

Rewrite `FuelDepotPlaceToolTest.cpp`'s depot case to drive the new gesture. Read the file
first - it currently exercises press/drag/release and those assertions are being replaced,
not deleted wholesale. Add:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/PlotDrawTool.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Free-snap, as ApronDrawToolTest does. */
	FToolContext PlotAt(ARoadNetworkActor* Actor, const FVector2D& Where)
	{
		return TestTool::ContextAt(*Actor, Where);
	}

	int32 LiveEntities(const ARoadNetworkActor* Actor)
	{
		int32 Alive = 0;
		for (const FEntityInstance& Entity : Actor->Network->GetEntities())
		{
			if (Entity.bAlive) { ++Alive; }
		}
		return Alive;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotDrawToolPlacesADepotTest,
	"Airside.Tool.PlotDrawToolPlacesADepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotDrawToolPlacesADepotTest::RunTest(const FString& Parameters)
{
	// A REAL WORLD, for the reason ApronDrawToolTest records at its own top: this drives an
	// actor whose components must be registered, and a bare NewObject is a half-built actor
	// masquerading as a working one.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();

	// A service road along y = -400, so the plot's south edge has a frontage to find.
	// Lay it with the road tool's own model calls, as AnchorLinkTest does - a plot with
	// no road is Task 7's refusal case, not this one.
	const FGuidelineNodeId West = Actor->Network->AddGuidelineNode(FVector2D(-5000.0, -400.0));
	const FGuidelineNodeId East = Actor->Network->AddGuidelineNode(FVector2D(5000.0, -400.0));
	FGuidelineEdge Road;
	Road.A = West;
	Road.B = East;
	Road.Control = FVector2D(0.0, -400.0);
	Road.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
	Road.Direction = EGuidelineDir::Bidirectional;
	Road.Width = 600.0;
	Road.bDerived = true;
	Actor->Network->AddGuidelineEdge(MoveTemp(Road));

	FPlotDrawTool Tool(EPlaceableEntity::FuelDepot);
	Tool.SetModules({ EDepotModule::Shed, EDepotModule::Shed, EDepotModule::Tank });

	TestTrue(TEXT("a fresh tool has nothing part-drawn"), Tool.IsIdle());

	// 12 m x 8 m, south edge on the road side.
	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 0.0)));
	TestFalse(TEXT("the first click starts an outline"), Tool.IsIdle());
	Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 0.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 800.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 800.0)));
	TestEqual(TEXT("four corners placed"), Tool.GetCorners().Num(), 4);
	TestEqual(TEXT("and nothing committed yet"), LiveEntities(Actor), 0);

	// Near the first corner, not on it - the cursor never lands on a stored coordinate.
	Tool.OnClick(PlotAt(Actor, FVector2D(60.0, 40.0)));
	TestTrue(TEXT("closing on the first corner ends the outline"), Tool.IsIdle());
	TestEqual(TEXT("and commits exactly one depot"), LiveEntities(Actor), 1);

	const TArray<FEntityInstance>& Entities = Actor->Network->GetEntities();
	if (!TestTrue(TEXT("an entity to read"), Entities.Num() > 0)) { return false; }
	const FEntityInstance& Depot = Entities[0];

	TestEqual(TEXT("the four corners drawn, not the closing click"), Depot.Outline.Num(), 4);
	TestEqual(TEXT("two sheds is two trucks, through the whole gesture"), Depot.Trucks, 2);
	TestEqual(TEXT("and the mix survives the commit"), Depot.Modules.Num(), 3);

	return true;
}

#endif
```

**Confirm `FEntityInstance::bAlive` and `ARoadNetworkActor::ClearNetwork` before writing
`LiveEntities`.** `FApronSurface` has `bAlive`; check `FEntityInstance` in `Model/RoadEntity.h`
uses the same name for liveness rather than a generation handle, and if it does not, count
through `GetEntities()` the way another entity test already does.

`FPlotDrawTool::SetModules` is this task's own invention - the mix has to reach the tool
somehow, and a UI for choosing it is not in this slice. A setter the test drives and the HUD
will later call is the smallest thing that works; default it to
`{ Shed, Tank, Pump }` so pressing `0` and drawing gives the Tier 1 depot as drawn.

- [ ] **Step 3: Add the interface method to all four implementors**

In `RoadEditTarget.h`, beside `PlaceEntity`:

```cpp
	/**
	 * Drop an installation into a drawn plot. The pose comes from the FIT, not from the
	 * player's drag - which is why this cannot be an overload of PlaceEntity above.
	 *
	 * Returns INDEX_NONE when the plot has no road frontage or is smaller than one bay.
	 * The tool says which; see URoadEditFacade::PlaceEntityInPlot.
	 */
	virtual int32 PlaceEntityInPlot(const TArray<FVector2D>& Outline,
		const TArray<EDepotModule>& Modules, EPlaceableEntity Kind) = 0;
```

In both test doubles (`RunwayToolTest.cpp:82`, `TaxiwayWidthTest.cpp:59`), beside their
existing stub:

```cpp
		virtual int32 PlaceEntityInPlot(const TArray<FVector2D>&,
			const TArray<EDepotModule>&, EPlaceableEntity) override { return INDEX_NONE; }
```

- [ ] **Step 4: Implement it on the facade**

In `RoadEditFacadeSurfaces.cpp`:

```cpp
int32 URoadEditFacade::PlaceEntityInPlot(const TArray<FVector2D>& Outline,
	const TArray<EDepotModule>& Modules, EPlaceableEntity Kind)
{
	FVector2D FrontageA = FVector2D::ZeroVector;
	FVector2D FrontageB = FVector2D::ZeroVector;
	if (!FindFrontageEdge(Outline, FrontageA, FrontageB))
	{
		UE_LOG(LogAirside, Warning,
			TEXT("PlaceEntityInPlot refused: the plot has no edge near a service road."));
		return INDEX_NONE;
	}

	const PlotFit::FPlotFit Fit = PlotFit::FitBays(Outline, FrontageA, FrontageB);
	if (!Fit.bFits)
	{
		UE_LOG(LogAirside, Warning,
			TEXT("PlaceEntityInPlot refused: the plot is smaller than one 4 m bay."));
		return INDEX_NONE;
	}

	const UEntityDefinition* Definition = GetEntityDefinition(Kind);
	if (Definition == nullptr)
	{
		UE_LOG(LogAirside, Warning,
			TEXT("PlaceEntityInPlot refused: no definition for the kind."));
		return INDEX_NONE;
	}

	// THE GATE IS THE POSE, and there is exactly one however many sheds were built:
	// BuildFuelDepot already ruled that two lead-ins from one small building into one road
	// is a duplicate painted line. The gate sits at the middle of the frontage edge.
	const FVector2D Gate = (FrontageA + FrontageB) * 0.5;

	FEntityPlacement Placement;
	Placement.Definition = const_cast<UEntityDefinition*>(Definition);
	Placement.Anchors = Definition->Anchors;
	Placement.Position = Gate;
	Placement.Heading = Fit.Bays[0].Heading;
	Placement.PoseRole = Definition->PoseRole;
	Placement.Outline = Outline;
	Placement.Modules = Modules;

	// Trailing modules the plot cannot hold are DROPPED, not squeezed in. A player who
	// picked four modules for a three-bay plot gets three and is told, which is honest;
	// scaling the bays to fit would silently change the size they drew.
	if (Placement.Modules.Num() > Fit.Bays.Num())
	{
		UE_LOG(LogAirside, Warning,
			TEXT("PlaceEntityInPlot: %d modules chosen but only %d bays fit; dropped %d."),
			Placement.Modules.Num(), Fit.Bays.Num(),
			Placement.Modules.Num() - Fit.Bays.Num());
		Placement.Modules.SetNum(Fit.Bays.Num());
	}

	const FEntityInstanceId Placed = Network->PlaceEntity(Placement);
	return Placed.IsSet() ? static_cast<int32>(Placed.Index) : INDEX_NONE;
}
```

**Confirm `FEntityInstanceId`'s accessor before writing `Placed.Index`.** Read its
declaration in `Model/RoadEntity.h` and use whatever it really exposes; the existing
`URoadEditFacade::PlaceEntity` at `RoadEditFacadeSurfaces.cpp:213` already converts one to an
`int32` - copy that line's conversion exactly rather than inventing a field.

Forward from `ARoadNetworkActor::PlaceEntityInPlot` to the facade, exactly as
`RoadNetworkActor.cpp:798` forwards `PlaceEntity`.

- [ ] **Step 5: Write the tool**

`Plugins/Airside/Source/Airside/Public/Tool/PlotDrawTool.h` mirrors `ApronDrawTool.h`'s
state objects. Read `ApronDrawTool.h` and `ApronDrawTool.cpp` and follow their shape: an
idle state, an outlining state holding the corners, `WouldClose`, `WouldCross`. The only
difference is the commit - `PlaceEntityInPlot` instead of `AddApron`.

Preview: describe the bays to `IToolPreviewSink` **naming a MEANING, not a colour** - that is
what keeps the plugin free of the game module.

- [ ] **Step 6: Swap the registry entry**

In `BuildSession.cpp`, replace the `EKeys::Zero` entry's factory:

```cpp
		// ZERO, after nine: it is the next key along a keyboard's top row, and every other
		// number is spoken for. No longer FStandPlaceTool - a depot is DRAWN now, not
		// stamped, so the gesture is the apron's closing polygon rather than press-drag-
		// release. The key, the name and the editor-mode command are unchanged, so the two
		// lists that must agree still do.
		{ EKeys::Zero,  TEXT("FuelDepot"), LOCTEXT("FuelDepot", "Fuel depot"),
			LOCTEXT("FuelDepotTooltip", "Draw a fuel depot plot: click each corner, click the first again to close. One edge must run along a service road."),
			[] { return MakeUnique<FPlotDrawTool>(EPlaceableEntity::FuelDepot); } },
```

- [ ] **Step 7: Build twice, run the whole suite, check the editor mode still agrees**

`./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject"`

Expected: the new tool test passes; `Airside.Tool.*` and `BuildSessionTest` still pass.
`BuildSessionTest` is the consumer that checks the two command lists agree **by name** - if
it fails, the editor mode's `UI_COMMAND` list and the registry have diverged.

- [ ] **Step 8: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/PlotDrawTool.h Plugins/Airside/Source/Airside/Private/Tool/PlotDrawTool.cpp Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp Plugins/Airside/Source/Airside/Public/Tool/RoadEditTarget.h Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h Plugins/Airside/Source/Airside/Private/Present/RoadEditFacadeSurfaces.cpp Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp Plugins/Airside/Source/AirsideTests/Private
git commit -m "feat(tool): a fuel depot is drawn as a plot, not stamped as a box"
```

---

### Task 6: Grey-box the modules

One instanced static mesh component holding scaled engine cubes. **One ISM, not one per
module type:** instance transforms carry per-instance scale, so a single cube mesh dresses
sheds, tanks, pumps and fence panels alike. When real meshes arrive it splits into one ISM
per mesh, and that split is the whole of the art swap.

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h`
- Create: `Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h` (own the subobject)
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp` (create and forward)
- Create: `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp`

**Interfaces:**
- Consumes: Tasks 1-5.
- Produces: `UPlotPresenter::RebuildFrom(const URoadNetwork& Network)`, and
  `UPlotPresenter::GetInstanceCount() const` returning `int32` for the test.

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterDressesEachBayTest,
	"Airside.Present.PlotPresenterDressesEachBay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterDressesEachBayTest::RunTest(const FString& Parameters)
{
	// COMPOSITION LEVEL, per CLAUDE.md: spawn the actor and drive it, rather than poking
	// the model struct. A presenter that is never wired to the actor passes every model
	// test and draws nothing, which is exactly the failure this test exists to catch.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	auto PlaceThreeBayDepot = [&](double X)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(X, 0.0);
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = { FVector2D(X, 0.0), FVector2D(X + 1200.0, 0.0),
		                      FVector2D(X + 1200.0, 800.0), FVector2D(X, 800.0) };
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		return Actor->Network->PlaceEntity(Placement);
	};

	PlaceThreeBayDepot(0.0);
	Actor->RebuildMesh();
	TestEqual(TEXT("three modules stand as three boxes"),
		Actor->GetPlotPresenter()->GetInstanceCount(), 3);

	// A SECOND depot must ADD, not replace. A presenter that rebuilt from only the last
	// entity passes every single-depot assertion and loses every depot but one on screen.
	PlaceThreeBayDepot(4000.0);
	Actor->RebuildMesh();
	TestEqual(TEXT("a second depot adds its own three, it does not replace the first"),
		Actor->GetPlotPresenter()->GetInstanceCount(), 6);

	return true;
}

#endif
```

**Confirm `ARoadNetworkActor::RebuildMesh` is the real rebuild entry point** before writing
this - `StandPlaceTool.cpp:95` mentions it by name but also says it was removed from that
call site, so read `RoadNetworkActor.h` for the method the actor actually exposes now.
`GetPlotPresenter()` is this task's own addition; add it as a plain accessor beside the
subobject.

- [ ] **Step 2: Run to verify it fails**

Build twice. Expected: FAIL - no `Present/PlotPresenter.h`.

- [ ] **Step 3: Write the presenter**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "UObject/Object.h"
#include "PlotPresenter.generated.h"

class URoadNetwork;

/**
 * The boxes standing in a depot's bays.
 *
 * ITS OWN SUBOBJECT rather than another method on ARoadNetworkActor, which grew to 2313
 * lines because every feature entered through the one door. The actor is a composition root
 * and forwards; this owns the geometry.
 *
 * ONE ISM FOR EVERY MODULE TYPE. An instance carries its own scale, so one cube dresses a
 * shed, a tank and a pump by scaling differently - and the split into one component per
 * mesh IS the art swap, when there are meshes to swap in.
 */
UCLASS()
class AIRSIDE_API UPlotPresenter : public UObject
{
	GENERATED_BODY()

public:
	/** Create the component and attach it under Owner's root. */
	void Initialise(AActor* Owner);

	/** Clear and re-add an instance per module of every plotted entity. */
	void RebuildFrom(const URoadNetwork& Network);

	/** For tests: how many boxes are standing. */
	int32 GetInstanceCount() const;

private:
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> Boxes;
};
```

The .cpp resolves the cube the way `RoadAgentActor.cpp:38` already does - **the engine's own
primitive, not an authored asset**, which is the same reasoning that file records:

```cpp
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
```

`ConstructorHelpers` works only in a constructor, so if `Initialise` is called at runtime use
`LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"))` instead. Set
`SetCollisionEnabled(ECollisionEnabled::NoCollision)` and `SetCastShadow(false)`, as
`RoadAgentActor` does and for the reason it gives: every pick is exact maths against the road
plane, and a collider here is something the build tools would trace against by accident.

Per-instance transform, for module index `I` of an entity at `Position`/`Heading`:

```cpp
	// The engine cube is 100 uu with a CENTRED pivot, so a box resting on the apron needs
	// half its height in Z - without it every module sinks to its waist.
	static constexpr double CubeUu = 100.0;
	const FVector Scale(PlotFit::BayWidthUu / CubeUu, PlotFit::BayDepthUu / CubeUu,
		HeightUu / CubeUu);
	const FVector Location(BayCentreWorld.X, BayCentreWorld.Y, HeightUu * 0.5);
```

Heights, so the three read apart at a glance: shed `400.0`, tank `250.0`, pump `150.0`.

- [ ] **Step 4: Wire it to the actor**

`ARoadNetworkActor` creates the subobject and forwards a rebuild to it wherever it already
rebuilds surfaces. **Find that call site rather than adding a new one** - the mesh-freshness
contract is tested by `MeshFreshnessTest.cpp`, which lists the mutators that must trigger a
rebuild, and `PlaceEntity` is named there.

- [ ] **Step 5: Run to verify it passes**

Build, run `-Filter Airside.Present`. Expected: `PlotPresenterDressesEachBay` passes.

- [ ] **Step 6: Tile the fence round the outline**

Same component, more instances: a thin scaled cube per fence bay, walked round every edge of
`Outline`, with the bay nearest the frontage midpoint **left out** - that gap is the gate.

Append to the test:

```cpp
	// The fence is instances too, so the count jumps. What matters is not the exact
	// number - that moves with the panel length - but that there is a GAP: a fence with
	// no gate is a depot no truck can leave, and it would look completely correct.
	const int32 WithFence = Actor->GetPlotPresenter()->GetInstanceCount();
	TestTrue(TEXT("the fence adds instances round the outline"), WithFence > 6);
	TestEqual(TEXT("and leaves exactly one bay out, which is the gate"),
		Actor->GetPlotPresenter()->GetGateGapCount(), 2);
```

Two, because there are two depots. `GetGateGapCount()` is a test-only accessor counting the
skipped bays; add it beside `GetInstanceCount()`.

Panel length `250.0` (2.5 m), height `200.0` (2 m), thickness `10.0`. A bay that would
overhang the end of an edge is dropped rather than shortened - a stretched last panel is a
second opinion about where the boundary is.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp
git commit -m "feat(present): grey-box boxes standing in a depot's bays, fenced"
```

---

### Task 6b: The pad is the polygon the player drew

The spec's cheapest claim and the one most easily forgotten: a depot's concrete pad is not
new geometry, it is `Outline` through the tessellation aprons already use. If this task is
skipped the modules stand on grass.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Build/RoadMeshBuilder.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadSurfacePresenter.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp`

**Interfaces:**
- Consumes: Task 2's `FEntityInstance::Outline`.
- Produces: no new types - plotted entities contribute to the existing apron surface build.

- [ ] **Step 1: Write the failing test**

Append to `PlotPresenterTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPadIsPavementTest,
	"Airside.Present.PlotPadIsPavement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPadIsPavementTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->RebuildMesh();

	// Read the apron component's triangle count BEFORE, so the assertion is about what
	// the plot ADDED rather than about an absolute number that moves with every other
	// surface in the level.
	const int32 Before = TestTool::ApronTriangleCount(*Actor);

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	FEntityPlacement Placement;
	Placement.Definition = Depot;
	Placement.Anchors = Depot->Anchors;
	Placement.Position = FVector2D::ZeroVector;
	Placement.PoseRole = EServiceRole::Fuel;
	Placement.Outline = { FVector2D(0.0, 0.0), FVector2D(1200.0, 0.0),
	                      FVector2D(1200.0, 800.0), FVector2D(0.0, 800.0) };
	Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
	Actor->Network->PlaceEntity(Placement);
	Actor->RebuildMesh();

	TestTrue(TEXT("the drawn plot became pavement, not bare grass"),
		TestTool::ApronTriangleCount(*Actor) > Before);
	return true;
}
```

**`TestTool::ApronTriangleCount` does not exist yet.** `ApronDrawToolTest.cpp` already reads
the apron component's mesh back - it includes `DynamicMesh/DynamicMesh3.h` and
`Components/DynamicMeshComponent.h` for exactly that. Read how it counts, and either reuse
its expression inline or add this helper to `AirsideTestFixtures.h` in this task.

- [ ] **Step 2: Run to verify it fails**

Build twice. Expected: FAIL - the triangle count is unchanged, because nothing feeds
`Outline` to the surface build.

- [ ] **Step 3: Feed plotted outlines into the apron build**

Wherever `RoadMeshBuilder` walks `Network.GetAprons()`, walk the live entities too and
tessellate any non-empty `Outline` through the **same** function. Do not add a second
tessellator:

```cpp
	// A PLOTTED ENTITY'S OUTLINE IS PAVEMENT, through the same tessellation an apron uses.
	// Not a copy of it: a second triangulator is a second opinion about the same polygon,
	// and the two would drift on exactly the concave plots a freeform gesture produces.
	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		if (!Entity.bAlive || Entity.Outline.Num() < 3)
		{
			continue;
		}
		AppendApronPolygon(Mesh, Entity.Outline, ConcreteSlot);
	}
```

**`AppendApronPolygon` and `ConcreteSlot` are placeholders for the real names.** Read
`RoadMeshBuilder.cpp`'s existing apron loop and call what it calls, with the material slot
it uses for concrete.

- [ ] **Step 4: Run to verify it passes**

Build, run `-Filter Airside.Present`. Expected: `PlotPadIsPavement` passes, and
`Airside.Tool.ApronDraw` **still** passes - the shared tessellation must not have changed
what an ordinary apron produces.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Private/Build/RoadMeshBuilder.cpp Plugins/Airside/Source/Airside/Private/Present/RoadSurfacePresenter.cpp Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp
git commit -m "feat(build): a depot's pad is the polygon the player drew"
```

---

### Task 6c: Pumps shorten the dwell

The second of the three modules with real machinery behind it. In `AirportOps`, not Airside.

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/FuelService.cpp`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/FuelService.h:207`
- Create: `Plugins/AirportOps/Source/AirportOpsTests/Private/PumpDwellTest.cpp`

**Interfaces:**
- Consumes: Task 2's `FEntityInstance::Modules`, `EDepotModule`.
- Produces: `UFuelService::DwellSecondsFor(const FEntityInstance& Depot) const` returning `double`.

`EDepotModule` lives in `Model/RoadEntity.h`, which is Airside's `Model/` layer - so
`UFuelService` may read it directly. **No new captured fact is needed**, which is the test
that this design put the enum in the right layer.

- [ ] **Step 1: Write the failing test**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPumpDwellTest,
	"AirportOps.Ops.PumpDwell",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPumpDwellTest::RunTest(const FString& Parameters)
{
	UFuelService* Service = NewObject<UFuelService>();

	FEntityInstance Depot;
	Depot.Modules = { EDepotModule::Shed, EDepotModule::Pump };
	const double One = Service->DwellSecondsFor(Depot);
	TestTrue(TEXT("one pump gives the base dwell"), FMath::IsNearlyEqual(One, 40.0));

	Depot.Modules = { EDepotModule::Shed, EDepotModule::Pump, EDepotModule::Pump };
	TestTrue(TEXT("two pumps halve it"),
		FMath::IsNearlyEqual(Service->DwellSecondsFor(Depot), 20.0));

	// THE FLOOR: a pump farm must not make refuelling instant, which would delete the
	// only pressure the fuel loop has.
	Depot.Modules.Init(EDepotModule::Pump, 100);
	TestTrue(TEXT("a pump farm is floored, not instant"),
		Service->DwellSecondsFor(Depot) >= 5.0);

	// ZERO PUMPS IS REACHABLE - the census warns rather than forbidding it - and must NOT
	// reach the division. A FMath::Max(1, PumpCount) here would silently hand a pumpless
	// depot a working dwell, which is worse than the divide it was papering over.
	Depot.Modules = { EDepotModule::Shed };
	TestTrue(TEXT("no pump means no fuelling, not a default dwell"),
		Service->DwellSecondsFor(Depot) < 0.0);

	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

Build twice, run `-Filter AirportOps`. Expected: FAIL - no `DwellSecondsFor`.

- [ ] **Step 3: Implement it**

```cpp
double UFuelService::DwellSecondsFor(const FEntityInstance& Depot) const
{
	int32 Pumps = 0;
	for (const EDepotModule Module : Depot.Modules)
	{
		if (Module == EDepotModule::Pump) { ++Pumps; }
	}

	// A depot built before modules existed has an empty list and one notional pump: it
	// fuelled before and must keep fuelling, or loading an old save breaks the fuel loop.
	if (Depot.Modules.Num() == 0)
	{
		return DwellSeconds;
	}

	// NO PUMP, NO FUELLING. Returned as a negative rather than clamped to one pump, so the
	// caller must branch on it - see the test. Silently giving a pumpless depot a working
	// dwell is the bug this shape exists to make impossible.
	if (Pumps == 0)
	{
		return -1.0;
	}

	return FMath::Max(DwellSeconds / static_cast<double>(Pumps), MinDwellSeconds);
}
```

Add `MinDwellSeconds = 5.0` beside `DwellSeconds` at `FuelService.h:207`, set from the
scenario the same way, with a comment saying it is the floor a pump farm cannot cross.

Then find the dispatch path that sets `DwellEndsAt` and route it through `DwellSecondsFor`,
refusing the dispatch when it comes back negative.

- [ ] **Step 4: Run to verify it passes**

Build, run the full suite. Expected: `AirportOps.Ops.PumpDwell` passes and every existing
fuel test still passes - old depots have no modules and keep the base dwell.

- [ ] **Step 5: Commit**

```bash
git add Plugins/AirportOps/Source/AirportOps/Private/Model/FuelService.cpp Plugins/AirportOps/Source/AirportOps/Public/Model/FuelService.h Plugins/AirportOps/Source/AirportOpsTests/Private/PumpDwellTest.cpp
git commit -m "feat(ops): a depot's pumps divide its dwell, and none refuses it"
```

---

### Task 7: Say why, when it will not go

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Model/InspectFacts.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Debug/RoadRebuildCensus.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/PlotRefusalTest.cpp`

**Interfaces:**
- Consumes: Tasks 1-6.
- Produces: no new types; the inspector strings from the spec's §8 table.

- [ ] **Step 1: Write the failing test**

Assert `InspectFacts::DescribeEntity` on a depot with no shed contains
`"no shed, no trucks"`, and that a depot whose plot found no road still says
`"Fuel depot: not on a road"` - **the existing wording**, reused rather than reworded, so a
player who learned it once does not meet two phrasings for one condition. Read
`InspectFactsTest.cpp` for the shape of a `DescribeEntity` assertion.

- [ ] **Step 2: Run to verify it fails**

Build twice. Expected: FAIL on the missing shed string.

- [ ] **Step 3: Add the cases**

Add a shedless-depot case to `DescribeEntity` beside the existing depot case, and a census
warning beside the existing anchor one.

- [ ] **Step 4: Run to verify it passes**

Build, run the full suite. Expected: 0 failed, 0 crashed.

- [ ] **Step 5: Run the architecture lint and commit**

```bash
./Tools/Check-Architecture.ps1
git add Plugins/Airside/Source/Airside/Private/Model/InspectFacts.cpp Plugins/Airside/Source/Airside/Private/Debug/RoadRebuildCensus.cpp Plugins/Airside/Source/AirsideTests/Private/PlotRefusalTest.cpp
git commit -m "feat(model): a depot that cannot work says which part is missing"
```

---

## What this plan deliberately does not build

**The Blender kit.** Spec §7.2 names `depot_shed`, `depot_tank`, `depot_pump`,
`fence_panel`, `fence_gate` and `windsock_mast`, and none of them is a task here. That is the
decision taken when this was scoped: the probe asks whether drawing a plot feels right, and
the bay size is the number every module would be modelled against. Modelling six meshes to a
4 m bay that the probe might move is the wasted work this ordering exists to avoid.

Task 6's one instanced component is shaped so the swap is a data change: per-instance
transforms already carry the scale, so real meshes arrive as one component per mesh and
nothing else moves.

## Verifying the probe, not just the build

The probe's question is *does drawing a plot feel like a plot, or like a chore?* - which no
test answers. After Task 6, in PIE:

1. Draw a service road.
2. Press `0`, click four corners of a plot with one edge along the road, click the first
   corner again to close.
3. Look for `LogRoadBuild: Road building ready on ...` to confirm which driver is live, and
   for the `PlaceEntityInPlot` lines on `LogAirside` if it refuses.
4. `python Tools/Mcp.py shot out.png` for the viewport, or `shot out.png editor` for the
   whole window.

What would make the answer "chore": needing more than four clicks for a rectangle, the
frontage picking an edge the player did not think of as the front, or bays that read as
arbitrary rather than as capacity. Each of those is a design answer, not a bug - write it
down rather than fixing it in flight.
