# Plot Layout Strategies Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Placement becomes a swappable strategy per plot type, and the fuel depot gets a band layout that leaves a yard somebody could work in.

**Architecture:** A strategy is a `UObject` in `Build/` that takes a plot site plus kit specs and returns stands; capacity is read off what it returned. `Solve/` keeps the geometry and stays UObject-free, so `PlotYard::Reserve` survives untouched as the scatter strategy's implementation. Kits gain an apron, stated separately from the footprint so the drawn box stays the object.

**Tech Stack:** Unreal Engine 5.8, C++20, Unreal automation tests (`IMPLEMENT_SIMPLE_AUTOMATION_TEST`), PowerShell test runner.

**Spec:** `docs/superpowers/specs/2026-09-20-plot-layout-strategies-design.md`

## Global Constraints

- **`Solve/` headers include `CoreMinimal.h` and nothing else**, and contain no `UObject`. `Check-Architecture.ps1` enforces both and runs before the tests. Strategies therefore live in `Build/`.
- **`Solve/` never sees `EDepotModule`.** It takes footprints and specs.
- **`Build/` may include `Entities/` and `Content/`; it may not include `Present/` or `Tool/`.**
- **No capacity rule is to be written anywhere.** Spec §3. Capacity is `FReservation::CeilingFor`, counting what a strategy returned. A reviewer should reject any task that introduces a formula for how many of something a plot holds.
- **`UFuelYardBandsStrategy` is scaffolding and its comment must say so** in those words. Spec §6.
- **Run tests with** `./Tools/Run-AirsideTests.ps1 -Filter <suite>`. A fresh worktree needs `Build.bat AirportMgrEditor Win64 Development -Project=<uproject>` first — an unbuilt worktree reports "no tests matched filter" rather than a failure.
- **`AirsideTests` is a unity build.** Two files' anonymous namespaces land in one translation unit, so a helper name already used in another test file is a redefinition. `ReserveRect`, `DepotSpecs` and `RunFootprint` are taken by `PlotReserveTest.cpp`.
- **Commit messages:** conventional-commit style with a scope. **No `Co-Authored-By` trailer.**
- **Comments carry the WHY.** A bare constant with no reason will be sent back in review.

## Geometry conventions, stated once

Every task below works in the plot's own frame. Getting these backwards is the single most
likely error, and `BuildFuelDepot`'s comment records that the project has already done it
once.

```
  Inward   = away from the road, into the plot. A stand's +X.
  Across   = RoadGeom::PerpCCW(Inward). For Inward (0,1) this is (-1,0),
             so Across points LEFT as you stand at the gate looking in.
  Gate     = the entity's pose, the midpoint of the frontage edge.
  "front"  = towards the gate = -Inward. A shed's door faces this way.
  "back"   = away from the gate = +Inward.
```

**Reserved ground versus drawn object**, with `Apron` in play:

```
  reserved length = Footprint.LengthUu + Apron.X        (apron extends towards the gate)
  reserved width  = Footprint.WidthUu * RunLength + Apron.Y * 2
  the object sits FLUSH TO THE BACK of the reserved rectangle:
      footprint centre = stand centre + Inward * (Apron.X * 0.5)
```

---

### Task 1: Kits carry an apron

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/PlotModuleKit.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/PlotYard.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Build/DepotKit.cpp`
- Modify: `Tools/Python/build_plot_kits.py`
- Test: `Plugins/Airside/Source/AirsideTests/Private/DepotKitTest.cpp`

**Interfaces:**
- Consumes: `UPlotModuleKit`, `PlotYard::FKitSpec`, `DepotKitSpecs(const UAirsideContent*)` — all existing.
- Produces: `UPlotModuleKit::ApronUu` (`FVector2D`), `PlotYard::FKitSpec::ApronUu` (`FVector2D`). Tasks 4 and 5 read both.

- [ ] **Step 1: Write the failing test**

Append to `DepotKitTest.cpp`, inside the existing `#if WITH_DEV_AUTOMATION_TESTS`:

```cpp
/**
 * A kit's apron reaches the solver, and is NOT folded into the footprint.
 *
 * TWO RECTANGLES, NOT ONE. The footprint is the object - what the mesh is and what the
 * presenter draws - and the apron is the working room in front of it. Inflating the shed to
 * 4 x 10 m to buy its apron would draw a ten-metre shed today and disagree with a six-metre
 * mesh tomorrow, which is exactly what the footprint-versus-bounds test exists to catch.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotKitCarriesItsApronTest,
	"Airside.Build.DepotKitCarriesItsApron",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotKitCarriesItsApronTest::RunTest(const FString& Parameters)
{
	UAirsideContent* Content = NewObject<UAirsideContent>();
	UPlotModuleKit* Kit = NewObject<UPlotModuleKit>();
	Kit->Footprint = FVector2D(600.0, 400.0);
	Kit->ApronUu = FVector2D(400.0, 0.0);
	Content->DepotKits.Add(EDepotModule::Shed, Kit);

	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(Content);
	if (!TestTrue(TEXT("a spec per module"), Specs.Num() > static_cast<int32>(EDepotModule::Shed)))
	{
		return false;
	}

	const PlotYard::FKitSpec& Shed = Specs[static_cast<int32>(EDepotModule::Shed)];

	// THE FOOTPRINT IS UNTOUCHED BY THE APRON. If these ever merge, the presenter draws the
	// apron as building.
	TestEqual(TEXT("the footprint is the object"), Shed.Footprint.LengthUu, 600.0);
	TestEqual(TEXT("and its width too"), Shed.Footprint.WidthUu, 400.0);

	TestEqual(TEXT("the apron reaches the spec"), Shed.ApronUu.X, 400.0);
	TestEqual(TEXT("and its lateral half"), Shed.ApronUu.Y, 0.0);

	// AN UNAUTHORED KIT HAS NO APRON rather than a default one: a module that needs clear
	// ground says so, and one that does not keeps the clearance every module already gets.
	const TArray<PlotYard::FKitSpec> Bare = DepotKitSpecs(nullptr);
	TestEqual(TEXT("an unauthored tank has no apron"),
		Bare[static_cast<int32>(EDepotModule::Tank)].ApronUu.X, 0.0);

	return true;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.DepotKitCarries`
Expected: compile failure — `UPlotModuleKit` has no `ApronUu`, `FKitSpec` has no `ApronUu`.

- [ ] **Step 3: Add the field to the kit**

In `PlotModuleKit.h`, directly below `Footprint`:

```cpp
	/**
	 * Clear ground this module needs BEYOND its own footprint, uu. X reaches towards the
	 * gate - the apron a truck stands on - and Y is kept clear to either side.
	 *
	 * SEPARATE FROM Footprint, WHICH STAYS THE OBJECT. Folding the apron in would make a
	 * shed 4 x 10 m: the grey box would draw ten metres deep today, and when the real
	 * six-metre mesh arrives the footprint-versus-bounds test would have to be told to
	 * ignore the difference - which is the test giving up on the thing it exists for.
	 *
	 * ZERO IS THE DEFAULT AND MEANS NONE. Every module already gets PlotYard::ClearanceUu
	 * between it and its neighbours; an apron is for a module that needs a vehicle to stand
	 * in front of it, and most do not.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit") FVector2D ApronUu = FVector2D::ZeroVector;
```

- [ ] **Step 4: Carry it on the spec**

In `PlotYard.h`, inside `FKitSpec`, below `Footprint`:

```cpp
		/**
		 * Clear ground beyond the footprint, uu. X towards the gate, Y to either side.
		 *
		 * STILL NO KIT IN Solve/. A spec is rectangles and integers; which asset they came
		 * from stays on the other side of the seam.
		 */
		FVector2D ApronUu = FVector2D::ZeroVector;
```

In `DepotKit.cpp`'s `DepotKitSpecs`, inside the `if (const UPlotModuleKit* Kit = *Found)`
block that already copies `ReserveWeight` and `RunCap`:

```cpp
					Spec.ApronUu = Kit->ApronUu;
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build`
Expected: PASS, including `DepotKitFallsBackWhenUnauthored`.

- [ ] **Step 6: Give the shed its apron**

In `Tools/Python/build_plot_kits.py`, extend the `KITS` table with an apron column and set it:

```python
# name, display, length uu, width uu, height uu, back fence, weight, run cap, apron x uu
#
# ONLY THE SHED HAS ONE. A truck stands in front of a shed and drives out of it, so the
# ground there is not somewhere another module may go. A tank is plumbed and a pump is
# walked up to; neither needs more than the clearance every module already gets.
KITS = [
    ("DA_Kit_FuelShed", "Vehicle shed", 800.0, 400.0, 400.0, True, 3, 3, 400.0),
    ("DA_Kit_FuelTank", "Fuel tank", 500.0, 500.0, 250.0, False, 2, 1, 0.0),
    ("DA_Kit_FuelPump", "Fuel pump", 300.0, 200.0, 150.0, False, 1, 1, 0.0),
]
```

and in `author_kit`, whose signature gains `apron_x`:

```python
    asset.set_editor_property("apron_uu", unreal.Vector2D(apron_x, 0.0))
```

Then run it:

```
& 'D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' <uproject> -run=pythonscript `
  -script=<repo>\Tools\Python\build_plot_kits.py -unattended -nosplash -nopause
```

- [ ] **Step 7: Run the whole suite**

Run: `./Tools/Run-AirsideTests.ps1`
Expected: PASS. The apron is carried but nobody reads it yet, so nothing moves on screen.

- [ ] **Step 8: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Entities/PlotModuleKit.h `
        Plugins/Airside/Source/Airside/Public/Solve/PlotYard.h `
        Plugins/Airside/Source/Airside/Private/Build/DepotKit.cpp `
        Plugins/Airside/Source/AirsideTests/Private/DepotKitTest.cpp `
        Tools/Python/build_plot_kits.py Content/Entities/DA_Kit_FuelShed.uasset
git commit -m "feat(airside): a kit states its apron apart from its footprint"
```

---

### Task 2: The strategy seam, and the scatter behind it

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Build/PlotLayoutStrategy.h`
- Create: `Plugins/Airside/Source/Airside/Private/Build/PlotLayoutStrategy.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotLayoutStrategyTest.cpp` (create)

**Interfaces:**
- Consumes: `PlotYard::FKitSpec`, `PlotYard::FReservation`, `PlotYard::Reserve` — all existing and unchanged.
- Produces: `FPlotSite`, `UPlotLayoutStrategy` (abstract, `Solve`), `UScatterLayoutStrategy`. Tasks 3, 4 and 5 all use these.

- [ ] **Step 1: Write the failing test**

Create `PlotLayoutStrategyTest.cpp`:

```cpp
#include "Build/PlotLayoutStrategy.h"
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/PlotYard.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** NAMED APART from PlotReserveTest's ReserveRect: AirsideTests is a unity build, so a
	 *  second definition in another file is a redefinition rather than a private copy. */
	TArray<FVector2D> StrategyRect(double Width, double Depth)
	{
		return { FVector2D(0.0, 0.0), FVector2D(Width, 0.0),
		         FVector2D(Width, Depth), FVector2D(0.0, Depth) };
	}

	TArray<PlotYard::FKitSpec> StrategySpecs()
	{
		PlotYard::FKitSpec Shed;
		Shed.Footprint.LengthUu = 800.0;
		Shed.Footprint.WidthUu = 400.0;
		Shed.Footprint.bAgainstTheBackFence = true;
		Shed.ApronUu = FVector2D(400.0, 0.0);
		Shed.ReserveWeight = 3;
		Shed.RunCap = 3;

		PlotYard::FKitSpec Tank;
		Tank.Footprint.LengthUu = 500.0;
		Tank.Footprint.WidthUu = 500.0;
		Tank.ReserveWeight = 2;

		PlotYard::FKitSpec Pump;
		Pump.Footprint.LengthUu = 300.0;
		Pump.Footprint.WidthUu = 200.0;
		Pump.ReserveWeight = 1;

		return { Shed, Tank, Pump };
	}

	FPlotSite StrategySite(const TArray<FVector2D>& Outline, double Width)
	{
		FPlotSite Site;
		Site.Outline = Outline;
		Site.FrontageA = FVector2D(0.0, 0.0);
		Site.FrontageB = FVector2D(Width, 0.0);
		Site.Gate = FVector2D(Width * 0.5, 0.0);
		Site.Seed = 1234;
		return Site;
	}
}

/**
 * The scatter strategy is the scatter, exactly.
 *
 * A WRAPPER AND NOTHING MORE. PlotYard::Reserve has seven tests written against it and they
 * keep testing the thing they were written for only while this adds no behaviour of its own.
 * If these two ever disagree, the seven tests are describing code nobody runs.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScatterStrategyIsTheScatterTest,
	"Airside.Build.ScatterStrategyIsTheScatter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FScatterStrategyIsTheScatterTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 3200.0);

	const UScatterLayoutStrategy* Strategy = NewObject<UScatterLayoutStrategy>();
	const PlotYard::FReservation Mine = Strategy->Solve(Site, Specs);
	const PlotYard::FReservation Theirs = PlotYard::Reserve(
		Site.Outline, Site.FrontageA, Site.FrontageB, Site.Gate, Specs, Site.Seed);

	if (!TestEqual(TEXT("the same number of stands"),
		Mine.Stands.Num(), Theirs.Stands.Num()))
	{
		return false;
	}
	TestTrue(TEXT("and a plot this size reserves something"), Mine.Stands.Num() > 0);

	for (int32 I = 0; I < Mine.Stands.Num(); ++I)
	{
		TestEqual(TEXT("same kit"), Mine.Stands[I].KitIndex, Theirs.Stands[I].KitIndex);
		TestEqual(TEXT("same run length"),
			Mine.Stands[I].RunLength, Theirs.Stands[I].RunLength);
		TestTrue(TEXT("same centre, exactly"),
			Mine.Stands[I].Centre.Equals(Theirs.Stands[I].Centre, 0.0));
		TestEqual(TEXT("same heading"), Mine.Stands[I].Heading, Theirs.Stands[I].Heading);
	}

	return true;
}

#endif
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.ScatterStrategy`
Expected: compile failure — `Build/PlotLayoutStrategy.h` does not exist.

- [ ] **Step 3: Write the header**

Create `Plugins/Airside/Source/Airside/Public/Build/PlotLayoutStrategy.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Solve/PlotYard.h"
#include "UObject/Object.h"
#include "PlotLayoutStrategy.generated.h"

/**
 * Where a plot is and how big. Plain data: no UObject, no EDepotModule, no entity.
 *
 * A STRUCT RATHER THAN FIVE PARAMETERS, because PlotYard::Reserve already takes five and
 * FEntityPlacement's own comment warned that its three trailing defaults would not survive a
 * fourth. Every strategy needs the same five and none of them needs a sixth yet.
 */
struct FPlotSite
{
	TArrayView<const FVector2D> Outline;
	FVector2D FrontageA = FVector2D::ZeroVector;
	FVector2D FrontageB = FVector2D::ZeroVector;

	/** Where the fence is left open, which is the entity's own pose. */
	FVector2D Gate = FVector2D::ZeroVector;

	/** Makes a sampling strategy repeatable. A prescriptive one may ignore it. */
	int32 Seed = 0;
};

/**
 * How a plot decides where its modules stand.
 *
 * IN Build/ RATHER THAN Solve/, and not by preference: Solve/ takes CoreMinimal.h and
 * nothing else and holds no UObject, which is what lets its tests run with no world, no
 * actor and no NewObject. Check-Architecture enforces both. Build/ is the layer that already
 * serves Present/ and Tool/ alike - the reason DepotFootprint lives there - so it is where a
 * UObject strategy belongs.
 *
 * A STRATEGY RETURNS STANDS. IT DOES NOT RETURN A CAPACITY. How many of something a plot
 * holds is read off what came back, by FReservation::CeilingFor, and no strategy states a
 * rule for it. See the design doc section 3: deriving a capacity model from the first
 * layout's arithmetic would bury a throwaway decision where a later strategy has to dig it
 * out again.
 */
UCLASS(Abstract)
class AIRSIDE_API UPlotLayoutStrategy : public UObject
{
	GENERATED_BODY()

public:
	virtual PlotYard::FReservation Solve(
		const FPlotSite& Site, TArrayView<const PlotYard::FKitSpec> Kits) const
		PURE_VIRTUAL(UPlotLayoutStrategy::Solve, return PlotYard::FReservation(););
};

/**
 * The sampled yard: modules scattered at quarter turns with a bounded jitter.
 *
 * KEPT, NOT RETIRED. It produced a fuel depot nobody could work in - 65% coverage, 44
 * buildings wall to wall - and that is a statement about a FUEL DEPOT rather than about
 * scattering. A plot type that is meant to look unplanned still wants exactly this.
 *
 * A WRAPPER OVER PlotYard::Reserve AND NOTHING MORE. Seven tests are written against that
 * function; giving this behaviour of its own would leave them describing code nobody runs.
 */
UCLASS()
class AIRSIDE_API UScatterLayoutStrategy : public UPlotLayoutStrategy
{
	GENERATED_BODY()

public:
	virtual PlotYard::FReservation Solve(
		const FPlotSite& Site, TArrayView<const PlotYard::FKitSpec> Kits) const override;
};
```

- [ ] **Step 4: Write the implementation**

Create `Plugins/Airside/Source/Airside/Private/Build/PlotLayoutStrategy.cpp`:

```cpp
#include "Build/PlotLayoutStrategy.h"

PlotYard::FReservation UScatterLayoutStrategy::Solve(
	const FPlotSite& Site, TArrayView<const PlotYard::FKitSpec> Kits) const
{
	return PlotYard::Reserve(
		Site.Outline, Site.FrontageA, Site.FrontageB, Site.Gate, Kits, Site.Seed);
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build`
Expected: PASS, and `Check-Architecture` clean — the lint would object if this header had
landed in `Solve/`.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Build/PlotLayoutStrategy.h `
        Plugins/Airside/Source/Airside/Private/Build/PlotLayoutStrategy.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotLayoutStrategyTest.cpp
git commit -m "feat(airside): placement is a strategy, and the scatter is the first one"
```

---

### Task 3: A plot type names its strategy

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/EntityDefinition.cpp` (`BuildFuelDepot`)
- Modify: `Plugins/Airside/Source/Airside/Public/Build/PlotLayoutStrategy.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Build/PlotLayoutStrategy.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotLayoutStrategyTest.cpp`

**Interfaces:**
- Consumes: `UPlotLayoutStrategy`, `UScatterLayoutStrategy` from Task 2.
- Produces: `EPlotLayout { Scatter, FuelYardBands }`, `UEntityDefinition::Layout`, and
  `const UPlotLayoutStrategy* PlotLayoutFor(EPlotLayout)`. Task 5 calls the resolver.

- [ ] **Step 1: Write the failing test**

Append to `PlotLayoutStrategyTest.cpp`:

```cpp
/**
 * Every layout resolves to a strategy.
 *
 * WALKED, NOT LISTED, which is the lesson AircraftLookTest paid for: a test that names its
 * subjects catches only the subjects somebody remembered. A layout added to the enum with no
 * strategy behind it fails here rather than drawing an empty depot in a shipped build.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEveryPlotLayoutResolvesTest,
	"Airside.Build.EveryPlotLayoutResolves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryPlotLayoutResolvesTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 3200.0);

	for (int32 Raw = 0; Raw <= static_cast<int32>(EPlotLayout::FuelYardBands); ++Raw)
	{
		const EPlotLayout Layout = static_cast<EPlotLayout>(Raw);
		const UPlotLayoutStrategy* Strategy = PlotLayoutFor(Layout);

		if (!TestNotNull(*FString::Printf(TEXT("layout %d has a strategy"), Raw), Strategy))
		{
			continue;
		}

		// AND IT ANSWERS. A strategy that resolved but returned nothing would pass a null
		// check and draw an empty plot, which is the failure this walk is for.
		const PlotYard::FReservation R = Strategy->Solve(Site, Specs);
		TestTrue(*FString::Printf(TEXT("layout %d reserves something on a 32 x 24 m plot"),
			Raw), R.Stands.Num() > 0);
	}

	// AND THE FUEL DEPOT ASKS FOR THE BAND LAYOUT rather than defaulting into it - the
	// default is the scatter, so a definition that never stated a layout keeps the old
	// behaviour instead of silently changing shape.
	const UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (TestNotNull(TEXT("a depot definition"), Depot))
	{
		TestEqual(TEXT("a fuel depot lays out in bands"),
			static_cast<int32>(Depot->Layout),
			static_cast<int32>(EPlotLayout::FuelYardBands));
	}

	return true;
}
```

Add `#include "Entities/EntityDefinition.h"` to this file's includes.

- [ ] **Step 2: Run test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.EveryPlotLayout`
Expected: compile failure — `EPlotLayout`, `PlotLayoutFor` and `UEntityDefinition::Layout`
do not exist.

- [ ] **Step 3: Declare the enum and the resolver**

`EPlotLayout` goes in **`Entities/EntityDefinition.h`**, beside `EPlaceableEntity` — a
definition owns it, and putting it in `Build/` would make `Entities/` depend on `Build/` to
declare its own field. `PlotLayoutStrategy.h` includes `Entities/EntityDefinition.h`;
`Build/` may include `Entities/`, and the lint forbids it only `Present/` and `Tool/`.

```cpp
UENUM()
enum class EPlotLayout : uint8
{
	/** Sampled, jittered, unplanned. The default - see PlotLayoutFor. */
	Scatter,
	/** Sheds across the back, tanks left, pumps right. Scaffolding; see the class. */
	FuelYardBands
};
```

and at the bottom of `PlotLayoutStrategy.h`:

```cpp
/**
 * The strategy for a layout. Never null for a declared value.
 *
 * ONE INSTANCE PER LAYOUT, held for the life of the process. A strategy is a pure function
 * wearing a UObject - it holds no state between calls and takes its whole world as
 * arguments - so allocating one per plot per rebuild would be churn for nothing.
 */
AIRSIDE_API const UPlotLayoutStrategy* PlotLayoutFor(EPlotLayout Layout);
```

- [ ] **Step 4: Implement the resolver**

In `PlotLayoutStrategy.cpp`:

```cpp
#include "UObject/Package.h"

const UPlotLayoutStrategy* PlotLayoutFor(EPlotLayout Layout)
{
	// ROOTED ON FIRST USE, so the GC cannot take a strategy the presenter is about to call.
	// Static locals rather than a registry: the set is closed at compile time, and a
	// registry would be a second list to keep in step with the enum.
	static UScatterLayoutStrategy* Scatter = []
	{
		UScatterLayoutStrategy* Made = NewObject<UScatterLayoutStrategy>(
			GetTransientPackage(), TEXT("ScatterLayout"));
		Made->AddToRoot();
		return Made;
	}();

	static UFuelYardBandsStrategy* Bands = []
	{
		UFuelYardBandsStrategy* Made = NewObject<UFuelYardBandsStrategy>(
			GetTransientPackage(), TEXT("FuelYardBandsLayout"));
		Made->AddToRoot();
		return Made;
	}();

	switch (Layout)
	{
	case EPlotLayout::Scatter: return Scatter;
	case EPlotLayout::FuelYardBands: return Bands;
	}

	// A LAYOUT ADDED TO THE ENUM WITH NO CASE gets the scatter rather than a null: an
	// unplanned yard is wrong, and an empty plot is wrong AND looks like a broken presenter.
	// Airside.Build.EveryPlotLayoutResolves fails either way.
	return Scatter;
}
```

`UFuelYardBandsStrategy` is Task 4. Until it exists, declare it in the header as a subclass
whose `Solve` returns `UScatterLayoutStrategy`'s result, and replace that body in Task 4 —
this task's test only asserts that every layout resolves and answers.

- [ ] **Step 5: Add the field to the definition**

In `EntityDefinition.h`, beside `PoseRole`:

```cpp
	/**
	 * How this plot's modules are arranged. See Build/PlotLayoutStrategy.h.
	 *
	 * SCATTER IS THE DEFAULT so an un-migrated definition keeps the behaviour it had. A
	 * default of FuelYardBands would silently re-shape every plot ever saved.
	 */
	UPROPERTY(EditAnywhere) EPlotLayout Layout = EPlotLayout::Scatter;
```

In `EntityDefinition.cpp`'s `BuildFuelDepot`, beside the `PoseRole` assignment:

```cpp
	// BANDS, NOT SCATTER. The sampled yard put 23 sheds, 9 tanks and 12 pumps wall to wall
	// across a 45 m plot at 65% coverage - see the 2026-09-20 layout-strategies design.
	Definition->Layout = EPlotLayout::FuelYardBands;
```

- [ ] **Step 6: Run test to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside`
Expected: PASS. Nothing reads `Layout` yet, so the depot is unchanged on screen.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h `
        Plugins/Airside/Source/Airside/Private/Entities/EntityDefinition.cpp `
        Plugins/Airside/Source/Airside/Public/Build/PlotLayoutStrategy.h `
        Plugins/Airside/Source/Airside/Private/Build/PlotLayoutStrategy.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotLayoutStrategyTest.cpp
git commit -m "feat(airside): a plot type names the layout it wants"
```

---

### Task 4: `UFuelYardBandsStrategy`

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/PlotYard.h` (expose two helpers)
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Build/PlotLayoutStrategy.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Build/PlotLayoutStrategy.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotLayoutStrategyTest.cpp`

**Interfaces:**
- Consumes: `FPlotSite`, `UPlotLayoutStrategy`, `FKitSpec::ApronUu`.
- Produces: `PlotYard::InwardOf(Outline, A, B)` and
  `PlotYard::StandsOverlap(A, FA, B, FB)`, both `AIRSIDE_API`; and the strategy's real
  `Solve`. Task 5 reads the stands it returns.

- [ ] **Step 1: Write the failing test**

Append to `PlotLayoutStrategyTest.cpp`:

```cpp
/**
 * The band yard leaves room to work in.
 *
 * THE NUMBER THAT PROMPTED THIS: a 45 x 35 m plot reserved 23 sheds, 9 tanks and 12 pumps -
 * 1,033 m2 of building on 1,575 m2 of ground, 65% coverage, wall to wall. "It is not a
 * practical fuel yard."
 *
 * 30% IS A CEILING, NOT A TARGET, and it is asserted rather than tuned to: a band layout
 * that crept back above it has stopped leaving a yard, whatever else it does.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardLeavesRoomTest,
	"Airside.Build.FuelYardLeavesRoom",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardLeavesRoomTest::RunTest(const FString& Parameters)
{
	// The plot from the screenshot: 45 m of frontage, 35 m deep.
	const TArray<FVector2D> Outline = StrategyRect(4500.0, 3500.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 4500.0);

	const UPlotLayoutStrategy* Strategy = PlotLayoutFor(EPlotLayout::FuelYardBands);
	if (!TestNotNull(TEXT("a band strategy"), Strategy)) { return false; }

	const PlotYard::FReservation R = Strategy->Solve(Site, Specs);
	if (!TestTrue(TEXT("a 45 x 35 m plot holds something"), R.Stands.Num() > 0))
	{
		return false;
	}

	double Covered = 0.0;
	for (const PlotYard::FReservedStand& Stand : R.Stands)
	{
		const PlotYard::FKitSpec& Kit = Specs[Stand.KitIndex];
		Covered += Kit.Footprint.LengthUu * Kit.Footprint.WidthUu * Stand.RunLength;
	}
	const double PlotArea = 4500.0 * 3500.0;
	const double Coverage = Covered / PlotArea;

	TestTrue(*FString::Printf(TEXT("coverage is under 30%%, got %.0f%%"), Coverage * 100.0),
		Coverage < 0.30);

	// AND THE COUNTS ARE A DEPOT. Twelve pumps serving nine tanks is not one at any tier.
	TestTrue(*FString::Printf(TEXT("a sane shed count, got %d"), R.CeilingFor(0)),
		R.CeilingFor(0) >= 2 && R.CeilingFor(0) <= 12);
	TestTrue(*FString::Printf(TEXT("a sane pump count, got %d"), R.CeilingFor(2)),
		R.CeilingFor(2) >= 1 && R.CeilingFor(2) <= 6);

	return true;
}

/**
 * The concept sheet's own plot still holds the concept sheet's own depot.
 *
 * 11 x 8 m, ONE OF EACH - "Essential fuel infrastructure for general aviation airfields.
 * Compact, reliable, easy to maintain." A layout that needs a big plot before it produces
 * anything has moved the Tier 1 depot out of reach of the tier it is for, which is a subtler
 * failure than 65% coverage and a harder one to see.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardFitsTheConceptSheetTest,
	"Airside.Build.FuelYardFitsTheConceptSheet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardFitsTheConceptSheetTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(1100.0, 800.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 1100.0);

	const PlotYard::FReservation R =
		PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Site, Specs);

	// AT LEAST ONE OF EACH, and not many more: this is the smallest depot that works, so a
	// layout that fits five sheds here has packed the plot rather than laid it out.
	for (int32 Kit = 0; Kit < Specs.Num(); ++Kit)
	{
		const int32 Held = R.CeilingFor(Kit);
		TestTrue(*FString::Printf(TEXT("kit %d gets at least one, got %d"), Kit, Held),
			Held >= 1);
		TestTrue(*FString::Printf(TEXT("and no more than three, got %d"), Kit, Held),
			Held <= 3);
	}

	return true;
}

/**
 * Nothing overlaps, and nothing stands on a shed's apron.
 *
 * THE APRON IS THE POINT OF THE FIELD. A tank parked in front of a shed door is a depot whose
 * truck cannot get out, and it looks perfectly correct from every angle - the same failure
 * the gate corridor exists to prevent, one step further in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardKeepsApronsClearTest,
	"Airside.Build.FuelYardKeepsApronsClear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardKeepsApronsClearTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(4500.0, 3500.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 4500.0);

	const PlotYard::FReservation R =
		PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Site, Specs);

	// The ground each stand actually claims: footprint plus apron, run-width included.
	auto Claimed = [&Specs](const PlotYard::FReservedStand& Stand)
	{
		const PlotYard::FKitSpec& Kit = Specs[Stand.KitIndex];
		PlotYard::FFootprint Out;
		Out.LengthUu = Kit.Footprint.LengthUu + Kit.ApronUu.X;
		Out.WidthUu = Kit.Footprint.WidthUu * Stand.RunLength + Kit.ApronUu.Y * 2.0;
		return Out;
	};

	for (int32 A = 0; A < R.Stands.Num(); ++A)
	{
		for (int32 B = A + 1; B < R.Stands.Num(); ++B)
		{
			TestFalse(*FString::Printf(TEXT("stands %d and %d claim separate ground"), A, B),
				PlotYard::StandsOverlap(R.Stands[A], Claimed(R.Stands[A]),
					R.Stands[B], Claimed(R.Stands[B])));
		}
	}

	return true;
}

/**
 * Sheds stand at the back, square, facing the gate.
 *
 * FUNCTIONAL, NOT DECORATIVE: a truck drives out of a shed, so its heading is the one this
 * layout may not turn for looks. BuildFuelDepot's comment records that +X faces AWAY from the
 * road, and that this was once written the wrong way round.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardStandsShedsAtTheBackTest,
	"Airside.Build.FuelYardStandsShedsAtTheBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardStandsShedsAtTheBackTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = StrategyRect(4500.0, 3500.0);
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const FPlotSite Site = StrategySite(Outline, 4500.0);

	const PlotYard::FReservation R =
		PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Site, Specs);

	int32 Sheds = 0;
	for (const PlotYard::FReservedStand& Stand : R.Stands)
	{
		if (Stand.KitIndex != 0) { continue; }
		++Sheds;

		// Interior is +Y here, so the inward bearing is +90 degrees, exactly - no jitter.
		TestEqual(TEXT("a shed is square to the frontage"),
			Stand.Heading, UE_DOUBLE_HALF_PI);

		// AT THE BACK: the 8 m shed plus its 4 m apron is 12 m of claimed depth, so a stand
		// hard against a 35 m back fence has its centre at 35 - 6 = 29 m.
		TestTrue(*FString::Printf(TEXT("a shed is at the back, got y %.0f"), Stand.Centre.Y),
			FMath::IsNearlyEqual(Stand.Centre.Y, 2900.0, 1.0));
	}
	TestTrue(TEXT("sheds were placed"), Sheds > 0);

	return true;
}

/**
 * Growing a plot never costs it capacity.
 *
 * ASSERTED FOR THIS STRATEGY ONLY, and that limit is the point. The scatter is not monotonic
 * - a sweep of 845 plot sizes found 336 regressions, worst drop 11 bays - and the seam
 * promises nothing either way. A band layout that lost this would be a bug in the bands.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelYardIsMonotonicTest,
	"Airside.Build.FuelYardIsMonotonic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelYardIsMonotonicTest::RunTest(const FString& Parameters)
{
	const TArray<PlotYard::FKitSpec> Specs = StrategySpecs();
	const UPlotLayoutStrategy* Strategy = PlotLayoutFor(EPlotLayout::FuelYardBands);

	for (double WidthUu = 1200.0; WidthUu <= 5000.0; WidthUu += 400.0)
	{
		TArray<int32> Previous;
		Previous.SetNumZeroed(Specs.Num());

		for (double DepthUu = 800.0; DepthUu <= 4000.0; DepthUu += 50.0)
		{
			const TArray<FVector2D> Outline = StrategyRect(WidthUu, DepthUu);
			const PlotYard::FReservation R =
				Strategy->Solve(StrategySite(Outline, WidthUu), Specs);

			for (int32 Kit = 0; Kit < Specs.Num(); ++Kit)
			{
				const int32 Now = R.CeilingFor(Kit);
				TestTrue(*FString::Printf(
					TEXT("%.0f x %.0f kit %d did not lose capacity: %d -> %d"),
					WidthUu, DepthUu, Kit, Previous[Kit], Now), Now >= Previous[Kit]);
				Previous[Kit] = Now;
			}
		}
	}

	return true;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.FuelYard`
Expected: compile failure on `PlotYard::StandsOverlap`, then assertion failures once it
compiles — the placeholder `Solve` from Task 3 returns the scatter, which is 65% coverage
and not monotonic.

- [ ] **Step 3: Expose the two geometry helpers**

In `Solve/PlotYard.h`, beside `StandCorners`:

```cpp
	/**
	 * The inward normal of the frontage: which way is INTO the plot.
	 *
	 * PUBLIC because a prescriptive layout needs the plot's own frame before it can place
	 * anything, and deriving it a second time in Build/ would be a second opinion about
	 * which way a depot faces - the failure BuildFuelDepot's comment records.
	 *
	 * Read off the polygon's winding rather than assumed counter-clockwise: a plot stored
	 * the other way round would otherwise aim every module across the road.
	 */
	AIRSIDE_API FVector2D InwardOf(TArrayView<const FVector2D> Outline,
		FVector2D FrontageA, FVector2D FrontageB);

	/**
	 * Do these two stands' rectangles intersect? Separating axis.
	 *
	 * PUBLIC for the same reason StandCorners is: a caller that computed overlap its own way
	 * would be checking its own arithmetic rather than the solver's. One derivation, now
	 * three consumers - the sampler, the band layout and the tests.
	 */
	AIRSIDE_API bool StandsOverlap(const FStand& A, const FFootprint& FootprintA,
		const FStand& B, const FFootprint& FootprintB);
```

In `PlotYard.cpp`, rename the file-private `InwardOf` to call through to the public one, and
implement `StandsOverlap` over the existing `QuadsIntersect`:

```cpp
FVector2D PlotYard::InwardOf(TArrayView<const FVector2D> Outline,
	FVector2D FrontageA, FVector2D FrontageB)
{
	const FVector2D Along = (FrontageB - FrontageA).GetSafeNormal();
	const FVector2D Left = RoadGeom::PerpCCW(Along);
	return RoadGeom::PolygonArea(Outline) > 0.0 ? Left : -Left;
}

bool PlotYard::StandsOverlap(const FStand& A, const FFootprint& FootprintA,
	const FStand& B, const FFootprint& FootprintB)
{
	TArray<FVector2D> CornersA;
	TArray<FVector2D> CornersB;
	StandCorners(A, FootprintA, CornersA);
	StandCorners(B, FootprintB, CornersB);
	return QuadsIntersect(CornersA, CornersB);
}
```

Delete the anonymous-namespace `InwardOf` and point `MakeYardSpace` at `PlotYard::InwardOf`.

- [ ] **Step 4: Implement the band layout**

In `PlotLayoutStrategy.h`, replace the Task 3 placeholder declaration:

```cpp
/**
 * Sheds across the back, tanks down the left, pumps down the right.
 *
 * THIS IS SCAFFOLDING. It exists to make a fuel depot usable now, not to be the fuel depot's
 * final arrangement, and it wastes ground on purpose - the middle of the plot is left empty
 * because that is what a real yard has and what the sampler never left. Nothing here is a
 * capacity rule: how many fit is whatever fitted.
 *
 * IT REPLACES A SAMPLED YARD THAT WAS REJECTED ON 2026-09-16 in favour of sampling, and that
 * rejection was right about this: every fuel depot built this way will share its bones. The
 * evidence changed - the sampled yard reached 65% coverage and 44 buildings wall to wall on a
 * 45 m plot - and a usable yard that repeats beats a varied one that does not.
 */
UCLASS()
class AIRSIDE_API UFuelYardBandsStrategy : public UPlotLayoutStrategy
{
	GENERATED_BODY()

public:
	virtual PlotYard::FReservation Solve(
		const FPlotSite& Site, TArrayView<const PlotYard::FKitSpec> Kits) const override;
};
```

In `PlotLayoutStrategy.cpp`, above the resolver:

```cpp
namespace
{
	/** The ground a stand claims: its object plus its apron, run width included. */
	PlotYard::FFootprint ClaimedBy(const PlotYard::FKitSpec& Kit, int32 RunLength)
	{
		PlotYard::FFootprint Out;
		Out.LengthUu = Kit.Footprint.LengthUu + Kit.ApronUu.X;
		Out.WidthUu = Kit.Footprint.WidthUu * RunLength + Kit.ApronUu.Y * 2.0;
		Out.bAgainstTheBackFence = Kit.Footprint.bAgainstTheBackFence;
		return Out;
	}

	/** Is this stand wholly inside the outline, and clear of everything already placed? */
	bool IsLegal(const PlotYard::FStand& Stand, const PlotYard::FFootprint& Claimed,
		TArrayView<const FVector2D> Outline,
		const TArray<PlotYard::FReservedStand>& Placed,
		TArrayView<const PlotYard::FKitSpec> Kits)
	{
		TArray<FVector2D> Corners;
		PlotYard::StandCorners(Stand, Claimed, Corners);
		for (const FVector2D& Corner : Corners)
		{
			if (!RoadGeom::PointInPolygon(Outline, Corner)) { return false; }
		}
		for (const PlotYard::FReservedStand& Other : Placed)
		{
			if (PlotYard::StandsOverlap(Stand, Claimed, Other,
				ClaimedBy(Kits[Other.KitIndex], Other.RunLength)))
			{
				return false;
			}
		}
		return true;
	}
}

PlotYard::FReservation UFuelYardBandsStrategy::Solve(
	const FPlotSite& Site, TArrayView<const PlotYard::FKitSpec> Kits) const
{
	PlotYard::FReservation Reservation;
	if (Site.Outline.Num() < 3 || Kits.Num() == 0)
	{
		return Reservation;
	}

	const FVector2D Inward = PlotYard::InwardOf(Site.Outline, Site.FrontageA, Site.FrontageB);
	const double InwardBearing = RoadGeom::Bearing(Inward);

	// ACROSS POINTS LEFT as you stand at the gate looking into the plot: PerpCCW of an inward
	// (0,1) is (-1,0). Tanks take +Across, pumps -Across, and the test pins which is which so
	// a sign flip here cannot pass unnoticed.
	const FVector2D Across = RoadGeom::PerpCCW(Inward);

	double Deepest = 0.0;
	double HalfSpan = 0.0;
	for (const FVector2D& Point : Site.Outline)
	{
		Deepest = FMath::Max(Deepest, FVector2D::DotProduct(Point - Site.Gate, Inward));
		HalfSpan = FMath::Max(HalfSpan,
			FMath::Abs(FVector2D::DotProduct(Point - Site.Gate, Across)));
	}

	// A band walks OUTWARD FROM THE CENTRELINE in both directions, so a wider plot only ever
	// adds stands to the ends of a band and never re-places the ones already there. That is
	// what makes this layout monotonic where the sampler is not.
	// PITCH IS PASSED, NOT DERIVED. A back band steps ACROSS the plot and its pitch is the
	// claimed WIDTH; a side band steps INTO the plot and its pitch is the claimed LENGTH.
	// Deriving it from the footprint here would space the tanks by their width and overlap
	// them - which Airside.Build.FuelYardKeepsApronsClear catches, but only after the fact.
	auto FillBand = [&](int32 Kit, const FVector2D& Origin, const FVector2D& Step,
		int32 RunLength, double Pitch)
	{
		const PlotYard::FFootprint Claimed = ClaimedBy(Kits[Kit], RunLength);

		for (int32 Index = 0; Index < 64; ++Index)
		{
			PlotYard::FReservedStand Stand;
			Stand.KitIndex = Kit;
			Stand.RunLength = RunLength;
			Stand.Heading = InwardBearing;
			Stand.Centre = Origin + Step * (Pitch * Index);
			Stand.bPlaced = true;

			if (!IsLegal(Stand, Claimed, Site.Outline, Reservation.Stands, Kits))
			{
				// THE BAND ENDS AT ITS FIRST REFUSAL rather than skipping past it. A gap
				// jumped over would put a stand beyond the plot's taper, and the band would
				// read as scattered - which is the thing this layout exists to stop.
				return;
			}
			Reservation.Stands.Add(Stand);
		}
	};

	// --- Sheds, across the back ------------------------------------------------------
	//
	// THE BACK BAND FIRST, and it takes its ground before anything else is offered any: the
	// truck drives out of a shed, so a shed's position is functional where a tank's is not.
	{
		const int32 Kit = 0;
		const int32 RunLength = FMath::Clamp(Kits[Kit].RunCap, 1, 64);
		const PlotYard::FFootprint Claimed = ClaimedBy(Kits[Kit], RunLength);
		const FVector2D Centre =
			Site.Gate + Inward * (Deepest - Claimed.LengthUu * 0.5);

		const double Pitch = Claimed.WidthUu + PlotYard::ClearanceUu;
		FillBand(Kit, Centre, Across, RunLength, Pitch);
		FillBand(Kit, Centre - Across * Pitch, -Across, RunLength, Pitch);
	}

	// --- Tanks down the left, pumps down the right -----------------------------------
	//
	// STARTED ONE TRUCK-CORRIDOR IN FROM THE GATE so a side band never grows across the way
	// out, and stepping BACKWARDS into the plot so the yard fills from the fence forwards.
	for (int32 Kit = 1; Kit < Kits.Num(); ++Kit)
	{
		const PlotYard::FFootprint Claimed = ClaimedBy(Kits[Kit], 1);
		const double Side = HalfSpan - Claimed.WidthUu * 0.5;
		const FVector2D Edge = (Kit == 1) ? Across : -Across;

		const FVector2D Centre = Site.Gate + Edge * Side
			+ Inward * (PlotYard::GateCorridorUu + Claimed.LengthUu * 0.5);

		FillBand(Kit, Centre, Inward, 1, Claimed.LengthUu + PlotYard::ClearanceUu);
	}

	return Reservation;
}
```

Add `#include "Solve/RoadGeom.h"` to `PlotLayoutStrategy.cpp`.

- [ ] **Step 5: Run the tests**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build`
Expected: PASS — all four `FuelYard` tests plus Tasks 1–3's.

- [ ] **Step 6: Run the whole suite**

Run: `./Tools/Run-AirsideTests.ps1`
Expected: PASS. Nothing reads the strategy yet, so the depot on screen is still the scatter.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Solve/PlotYard.h `
        Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp `
        Plugins/Airside/Source/Airside/Public/Build/PlotLayoutStrategy.h `
        Plugins/Airside/Source/Airside/Private/Build/PlotLayoutStrategy.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotLayoutStrategyTest.cpp
git commit -m "feat(airside): the fuel yard lays out in bands, and leaves room to work in"
```

---

### Task 5: The presenter and the tool use the strategy

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp`

**Interfaces:**
- Consumes: `PlotLayoutFor`, `FPlotSite`, `UEntityDefinition::Layout`, `FKitSpec::ApronUu`.
- Produces: nothing new. This is the last task.

- [ ] **Step 1: Write the failing test**

Append to `PlotPresenterTest.cpp`:

```cpp
/**
 * A built depot draws its object, not its apron.
 *
 * THE APRON IS RESERVED GROUND, NOT BUILDING. A shed drawn at footprint-plus-apron is a
 * twelve-metre shed, and the mesh that arrives later is eight - the disagreement the apron
 * was made a separate field to prevent.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterDrawsTheObjectNotTheApronTest,
	"Airside.Present.PlotPresenterDrawsTheObjectNotTheApron",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterDrawsTheObjectNotTheApronTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Actor->ClearNetwork();
	PlaceDeepDepot(Actor, Depot, /*X=*/0.0);
	Actor->RebuildMesh();

	const UPlotPresenter* Plots = Actor->GetPlotPresenter();

	// THE DEPOT IS THE BAND STRATEGY'S DEPOT, measured against the strategy directly. This is
	// what makes the test red before the wiring: the presenter still calls the scatter, and
	// the two disagree about how many bays a plot holds. Asserting only "no box is 12 m long"
	// would pass against the scatter too, which knows nothing of aprons.
	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(UAirsideSettings::GetContent());
	FPlotSite Expected;
	const TArray<FVector2D> Outline = DeepPlotAt(0.0);
	Expected.Outline = Outline;
	Expected.FrontageA = FVector2D(0.0, 0.0);
	Expected.FrontageB = FVector2D(2000.0, 0.0);
	Expected.Gate = FVector2D(1000.0, 0.0);
	Expected.Seed = DepotYardSeed(Expected.Gate);

	const PlotYard::FReservation Bands =
		PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Expected, Specs);

	int32 Bays = 0;
	for (const PlotYard::FReservedStand& Stand : Bands.Stands)
	{
		Bays += Stand.RunLength;
	}
	if (!TestTrue(TEXT("the band layout holds something here"), Bays > 0)) { return false; }

	TestEqual(TEXT("the depot drawn is the band layout's depot"),
		Plots->GetModuleCount() + Plots->GetGhostCount(), Bays);

	// AND ITS OBJECTS ARE DRAWN, NOT ITS APRONS. The shed is 8 m long and its apron 4 m, so a
	// box 12 m long is the apron drawn as building. Scale is length / 100 - the engine cube
	// is 100 uu on a side.
	for (int32 I = 0; I < Plots->GetInstanceCount(); ++I)
	{
		FTransform At;
		if (!Plots->GetInstanceTransformForTest(I, At)) { continue; }
		TestFalse(TEXT("no box is drawn at footprint plus apron"),
			FMath::IsNearlyEqual(At.GetScale3D().X * 100.0, 1200.0, 1.0));
	}

	// AND NOTHING DROPPED. A reservation returns only what it placed, whatever strategy made
	// it, so this stays an invariant across the seam.
	TestEqual(TEXT("nothing was dropped"), Plots->GetDroppedCount(), 0);

	return true;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present.PlotPresenterDrawsTheObject`
Expected: FAIL on *"the depot drawn is the band layout's depot"* — the presenter still calls
the scatter, which reserves a different number of bays from the band layout on the same plot.

Add `#include "Build/PlotLayoutStrategy.h"` and `#include "Content/AirsideSettings.h"` to
this test file.

- [ ] **Step 3: Route the presenter through the strategy**

In `PlotPresenter.cpp`'s `RebuildFrom`, replace the direct `PlotYard::Reserve` call:

```cpp
		// THE PLOT TYPE DECIDES ITS OWN ARRANGEMENT. A fuel depot bands; something meant to
		// look unplanned still scatters. Null definition keeps the scatter, which is what an
		// un-migrated save has.
		const EPlotLayout Layout = Entity.Definition != nullptr
			? Entity.Definition->Layout : EPlotLayout::Scatter;

		FPlotSite PlotSite;
		PlotSite.Outline = Entity.Outline;
		PlotSite.FrontageA = FrontageA;
		PlotSite.FrontageB = FrontageB;
		PlotSite.Gate = Entity.Position;
		PlotSite.Seed = DepotYardSeed(Entity.Position);

		const PlotYard::FReservation Reservation =
			PlotLayoutFor(Layout)->Solve(PlotSite, Specs);
```

Then, where the solid and ghost boxes are built, offset the drawn footprint to the back of
the claimed ground:

```cpp
			// FLUSH TO THE BACK OF WHAT IT CLAIMED. The apron reaches towards the gate, so
			// the object sits at the far end of the reserved rectangle and the clear ground
			// is in front of its door where a truck can use it.
			const FVector2D Forward(FMath::Cos(Stand.Heading), FMath::Sin(Stand.Heading));
			const FVector2D ToBack = Forward * (Specs[Stand.KitIndex].ApronUu.X * 0.5);
```

and add `ToBack` to both the lit and ghosted centres already computed there.

Add `#include "Build/PlotLayoutStrategy.h"` and `#include "Entities/EntityDefinition.h"`.

- [ ] **Step 4: Route the tool the same way**

In `PlotPlaceTool.cpp`'s `ReservationFor`, replace the `PlotYard::Reserve` call with the same
`FPlotSite` plus `PlotLayoutFor(...)->Solve(...)`. The tool knows its `Kind`
(`EPlaceableEntity`), not a definition, so map it:

```cpp
	// THE TOOL KNOWS WHAT IT IS PLACING, not which asset will be placed, so it maps its own
	// kind. One line, and the alternative - reaching into the facade for the definition
	// mid-drag - would make the preview depend on state the player has not committed to.
	const EPlotLayout Layout = Kind == EPlaceableEntity::FuelDepot
		? EPlotLayout::FuelYardBands : EPlotLayout::Scatter;
```

The ghost loop must outline the CLAIMED ground, not the footprint — the player is being shown
what the plot commits, and a ghost that outlined only the buildings would look like there is
room between them that there is not.

- [ ] **Step 5: Run the whole suite**

Two tests in `PlotPlaceToolTest.cpp` compute their own expectation by calling
`PlotYard::Reserve` directly, and will compare a band yard against a scattered one until they
go through the same seam the tool now does. In `PlotGhostDrawsTheModules`, replace:

```cpp
	const PlotYard::FReservation Reservation = PlotYard::Reserve(
		Shown, Shown[0], Shown[1], Pose, Specs, DepotYardSeed(Pose));
```

with:

```cpp
	// THROUGH THE SEAM, because the tool goes through it. Calling Reserve here would measure
	// the ghost of a band yard against the stand count of a scattered one.
	FPlotSite Site;
	Site.Outline = Shown;
	Site.FrontageA = Shown[0];
	Site.FrontageB = Shown[1];
	Site.Gate = Pose;
	Site.Seed = DepotYardSeed(Pose);
	const PlotYard::FReservation Reservation =
		PlotLayoutFor(EPlotLayout::FuelYardBands)->Solve(Site, Specs);
```

`PlotReadoutCountsRoomNotSlots` and `PlotReadoutMatchesPreview` read the readout's own facts
rather than calling the solver, so they need no change — but both assert counts that a band
yard will answer differently. Re-run and update the expected numbers to whatever the bands
give, and say in the commit message what they moved from and to.

Run: `./Tools/Run-AirsideTests.ps1`
Expected: PASS across all three suites.

- [ ] **Step 6: Look at it in PIE**

Draw the 45 x 35 m plot from `samples/1.png` again. Expect sheds in a row across the back with
clear ground in front of them, tanks down one side, pumps down the other, and an empty middle.
Record the readout's three numbers in the commit message — they are the before-and-after
against 23/9/12.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp `
        Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotPlaceToolTest.cpp
git commit -m "feat(airside): a fuel depot is laid out in bands, not scattered"
```

---

## What this plan does not do

- **Circulation.** The middle is empty because bands leave it empty, not because anything
  tested that a truck can cross it. Spec §3.
- **Any capacity rule.** Spec §3, and a reviewer should reject one.
- **Strategies for other buildings.** The seam takes them; none is written.
- **Retiring the scatter.** It stays, wrapped and tested.
- **The apron's shape.** One `FVector2D` — front and sides. A module wanting clear ground
  behind, or on one side only, needs a richer field and nothing here has asked for one.
- **Meshes, the fence, dressing.** Still Plans B and C of the kits work.
