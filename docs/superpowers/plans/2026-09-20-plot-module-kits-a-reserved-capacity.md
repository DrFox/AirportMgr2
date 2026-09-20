# Plot Module Kits A — Reserved Capacity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A drawn plot reserves its whole layout once, and that solved layout is simultaneously the preview, the per-kit ceiling and where each module stands.

**Architecture:** `PlotYard` gains `Reserve`, which fills a plot by weighted round-robin over kit specs instead of laying out a caller-supplied list. Kit numbers move from a hardcoded switch into `UPlotModuleKit` data assets resolved presenter-side, leaving both solvers dependency-free. The presenter draws owned modules solid and reserved-but-unowned ones ghosted.

**Tech Stack:** Unreal Engine 5.8, C++20, Unreal automation tests (`IMPLEMENT_SIMPLE_AUTOMATION_TEST`), PowerShell test runner.

**Spec:** `docs/superpowers/specs/2026-09-20-plot-module-kits-design.md`

## Global Constraints

- **`Solve/` headers include `CoreMinimal.h` and nothing else.** No `Model/`, no `Entities/`, no `FTransform2D`. `Check-Architecture.ps1` enforces it and runs before the tests.
- **`Solve/` never sees `EDepotModule`.** It takes footprints and specs; mapping a module to its spec is the presenter's job.
- **`Model/` must not dereference `Entities/`.** `UPlotModuleKit` is resolved on the `Build/` or `Present/` side and handed down as plain data.
- **Registry-walking tests live in the game module, not `Airside`** — they touch `/Game` assets, and `Check-Architecture` enforces that direction.
- **Run tests with** `./Tools/Run-AirsideTests.ps1 -Filter <suite>`. It derives a real verdict from the log because Unreal's runner exits 0 either way. It resolves the project from its own location, so it tests this worktree.
- **Commit messages:** conventional-commit style with a scope, as the repo's log has it (`feat(airside):`, `fix(content):`, `docs:`). **No `Co-Authored-By` trailer.**
- **Comments carry the WHY.** This codebase's convention is that a non-obvious constant or decision states what it is for and what went wrong without it. Match it; a bare value with no reason will be sent back in review.

## Decision taken during planning, not in the spec

**Only the FIRST run of a back-fence kit stands against the back fence.** `PlotYard`'s back-fence placement walks a single ray from the gate (`BackFenceProbes` depths along `Inward`), so two back-fence stands would contend for one line of ground. With `RunCap = 3` and a generous `ReserveWeight`, a large plot reserves more than one shed run. The first takes the back fence; the rest are sampled like any other stand. Recorded in Task 4's code comment and worth confirming in PIE — a second shed run adrift in the yard may look wrong, in which case the fix is to cap shed runs at one, not to widen the ray.

---

### Task 1: `UPlotModuleKit`, and a resolver that falls back

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Entities/PlotModuleKit.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Content/AirsideContent.h` (add `DepotKits`, in the `Airside|Defaults` category beside `AgentMesh`)
- Modify: `Plugins/Airside/Source/Airside/Public/Build/DepotKit.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Build/DepotKit.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/DepotKitTest.cpp` (create)

**Interfaces:**
- Consumes: `PlotYard::FFootprint` (existing), `EDepotModule` (existing, `Model/RoadEntity.h`).
- Produces: `UPlotModuleKit`, `EKitAssembly`, and
  `PlotYard::FFootprint DepotFootprint(EDepotModule, const UAirsideContent*)` — the existing
  one-argument `DepotFootprint` stays, delegating with `nullptr`, so no caller breaks in this task.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/DepotKitTest.cpp`:

```cpp
#include "Build/DepotKit.h"
#include "Content/AirsideContent.h"
#include "CoreMinimal.h"
#include "Entities/PlotModuleKit.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A kit's figures win; no kit falls back to the grey-box table.
 *
 * THE FALLBACK IS THE POINT, not a convenience. Every other task in this plan lands and is
 * testable before a single mesh exists, and that is only true while a missing kit keeps
 * working. A resolver that returned a zero footprint would put every module on top of
 * every other one, which reads as a solver bug rather than as missing content.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDepotKitFallsBackTest,
	"Airside.Build.DepotKitFallsBackWhenUnauthored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDepotKitFallsBackTest::RunTest(const FString& Parameters)
{
	// No content at all: the grey-box figures that DepotKit.cpp has carried since the
	// yard solver was written.
	const PlotYard::FFootprint Bare = DepotFootprint(EDepotModule::Shed, nullptr);
	TestEqual(TEXT("unauthored shed keeps its grey-box length"), Bare.LengthUu, 800.0);
	TestEqual(TEXT("unauthored shed keeps its grey-box width"), Bare.WidthUu, 400.0);
	TestTrue(TEXT("unauthored shed still stands against the back fence"),
		Bare.bAgainstTheBackFence);

	// An authored kit overrides all three.
	UAirsideContent* Content = NewObject<UAirsideContent>();
	UPlotModuleKit* Kit = NewObject<UPlotModuleKit>();
	Kit->Footprint = FVector2D(1100.0, 500.0);
	Kit->bAgainstTheBackFence = false;
	Content->DepotKits.Add(EDepotModule::Shed, Kit);

	const PlotYard::FFootprint Authored = DepotFootprint(EDepotModule::Shed, Content);
	TestEqual(TEXT("an authored shed uses the kit's length"), Authored.LengthUu, 1100.0);
	TestEqual(TEXT("an authored shed uses the kit's width"), Authored.WidthUu, 500.0);
	TestFalse(TEXT("an authored shed uses the kit's back-fence flag"),
		Authored.bAgainstTheBackFence);

	// A kit for one module does not silently answer for another.
	const PlotYard::FFootprint Tank = DepotFootprint(EDepotModule::Tank, Content);
	TestEqual(TEXT("the tank still falls back"), Tank.LengthUu, 500.0);

	return true;
}

#endif
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build`
Expected: compile failure — `Entities/PlotModuleKit.h` does not exist, `UAirsideContent::DepotKits` does not exist, and `DepotFootprint` takes one argument.

- [ ] **Step 3: Create the kit asset**

Create `Plugins/Airside/Source/Airside/Public/Entities/PlotModuleKit.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "PlotModuleKit.generated.h"

class UStaticMesh;

UENUM()
enum class EKitAssembly : uint8
{
	/** Whole meshes, one per bay count. Blender bakes them. */
	Baked,
	/** Cap + Bay x N + Cap, assembled at runtime. DESIGNED, NOT BUILT - see ResolveParts. */
	Parts
};

/**
 * One kind of thing that stands in a plot: how big it is, how many of it a plot reserves,
 * and what it looks like.
 *
 * THE OTHER HALF OF A PLOT, and the sibling of UAircraftType. A plot says what ground it
 * has; a kit says what occupies a piece of it. Baking those figures into DepotKit.cpp's
 * switch was right while they were grey boxes and stops being right the moment a mesh
 * exists, because the switch and the mesh are then two statements of one dimension.
 *
 * HAND-AUTHORED, with provenance in the comment, exactly as UAircraftType is. A manifest
 * emitted by the Blender build script was considered and rejected: it is a mechanism this
 * project uses nowhere, and what it would buy is bought by the bounds test that walks the
 * registry - see the design doc section 9.1.
 */
UCLASS(BlueprintType)
class AIRSIDE_API UPlotModuleKit : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Kit") FText DisplayName;

	/**
	 * Plan extent of ONE module, uu. X is along the module's own +X, which faces AWAY from
	 * the road - see UEntityDefinition::BuildFuelDepot, whose comment records that this was
	 * once stated the wrong way round.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit") FVector2D Footprint = FVector2D::ZeroVector;

	/** Grey-box height, uu. Retired once BakedMeshes is set. */
	UPROPERTY(EditAnywhere, Category = "Kit") double HeightUu = 0.0;

	/**
	 * Stood against the plot's BACK boundary rather than sampled into the yard.
	 *
	 * POSITION ONLY - it says where the thing stands and nothing about which way it points,
	 * deliberately. PlotYard::FFootprint carries the same field with the same warning.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit") bool bAgainstTheBackFence = false;

	/**
	 * How often this kit comes up in the reservation round-robin.
	 *
	 * IT SETS A CEILING, NOT THE PLAYER'S STRATEGY. They buy in whatever order they like up
	 * to the ceiling, so a generous weight costs nothing and a mean one silently forbids a
	 * build the player wanted. See the design doc section 3.3.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit") int32 ReserveWeight = 1;

	/** Modules of this kit grouped into one run at one heading. 1 = never grouped. */
	UPROPERTY(EditAnywhere, Category = "Kit") int32 RunCap = 1;

	UPROPERTY(EditAnywhere, Category = "Kit") EKitAssembly Assembly = EKitAssembly::Baked;

	/** Baked only. Index = bay count - 1, so exactly RunCap entries and none null. */
	UPROPERTY(EditAnywhere, Category = "Kit")
	TArray<TSoftObjectPtr<UStaticMesh>> BakedMeshes;

	/** Parts only. UNUSED while EKitAssembly::Parts is unimplemented. */
	UPROPERTY(EditAnywhere, Category = "Kit|Parts") TSoftObjectPtr<UStaticMesh> PartCapMesh;
	UPROPERTY(EditAnywhere, Category = "Kit|Parts") TSoftObjectPtr<UStaticMesh> PartBayMesh;
	UPROPERTY(EditAnywhere, Category = "Kit|Parts") double PartPitchUu = 0.0;
};
```

- [ ] **Step 4: Add the map to `UAirsideContent`**

In `Plugins/Airside/Source/Airside/Public/Content/AirsideContent.h`, add `class UPlotModuleKit;`
to the forward declarations at the top, and this property in the `Airside|Defaults` section
beside `AgentMesh`:

```cpp
	/**
	 * What stands in a plot, by module.
	 *
	 * A MAP RATHER THAN FIELDS PER MODULE, because the consumer walks EDepotModule and a
	 * field per value could only be kept in step by someone remembering to add one - the
	 * failure AircraftLookTest exists for. An unmapped module falls back to DepotKit.cpp's
	 * grey-box table, which is what lets the solver work land before any mesh does.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TMap<EDepotModule, TObjectPtr<UPlotModuleKit>> DepotKits;
```

- [ ] **Step 5: Add the two-argument resolver**

In `Plugins/Airside/Source/Airside/Public/Build/DepotKit.h`, add below the existing declaration:

```cpp
class UAirsideContent;

/**
 * What a module occupies, preferring its authored kit and falling back to the grey-box table.
 *
 * CONTENT IS OPTIONAL AND THAT IS LOAD-BEARING. Every solver and presenter change in this
 * slice is testable with no content at all, so the two halves of the work - the runtime and
 * the meshes - proceed independently instead of blocking each other.
 */
AIRSIDE_API PlotYard::FFootprint DepotFootprint(EDepotModule Module,
	const UAirsideContent* Content);
```

In `Plugins/Airside/Source/Airside/Private/Build/DepotKit.cpp`, add the includes
`#include "Content/AirsideContent.h"` and `#include "Entities/PlotModuleKit.h"`, rename the
existing function body to the two-argument form, and keep the old signature delegating:

```cpp
PlotYard::FFootprint DepotFootprint(EDepotModule Module, const UAirsideContent* Content)
{
	if (Content != nullptr)
	{
		if (const TObjectPtr<UPlotModuleKit>* Found = Content->DepotKits.Find(Module))
		{
			if (const UPlotModuleKit* Kit = *Found)
			{
				PlotYard::FFootprint Out;
				Out.LengthUu = Kit->Footprint.X;
				Out.WidthUu = Kit->Footprint.Y;
				Out.bAgainstTheBackFence = Kit->bAgainstTheBackFence;
				return Out;
			}
		}
	}

	// ... the existing switch, unchanged, including its THEY DIFFER comment ...
}

PlotYard::FFootprint DepotFootprint(EDepotModule Module)
{
	// ONE ARGUMENT MEANS NO CONTENT, not "look it up from somewhere". A global lookup here
	// would make this function's answer depend on load order, and the two callers that still
	// use it are mid-migration rather than content-free forever.
	return DepotFootprint(Module, nullptr);
}
```

- [ ] **Step 6: Run test to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Build`
Expected: PASS, and `Check-Architecture.ps1` clean.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Entities/PlotModuleKit.h `
        Plugins/Airside/Source/Airside/Public/Content/AirsideContent.h `
        Plugins/Airside/Source/Airside/Public/Build/DepotKit.h `
        Plugins/Airside/Source/Airside/Private/Build/DepotKit.cpp `
        Plugins/Airside/Source/AirsideTests/Private/DepotKitTest.cpp
git commit -m "feat(airside): module footprints come from an authored kit, falling back to the grey box"
```

---

### Task 2: Extract PlotYard's placement machinery

A pure refactor. `Reserve` needs the sampler, the legality test and the taken-ground list that
`LayOut` currently keeps in lambdas inside one function. No behaviour changes, so the existing
tests are the test.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotYardTest.cpp` (unchanged — that is the point)

**Interfaces:**
- Consumes: everything already in `PlotYard.cpp`.
- Produces: a file-private `FYardSpace` used by Task 3. Nothing in the public header moves.

- [ ] **Step 1: Run the existing tests and record the baseline**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve`
Expected: PASS. Write down the test count — the same count must pass at Step 4.

- [ ] **Step 2: Introduce `FYardSpace`**

In `PlotYard.cpp`'s anonymous namespace, add a struct holding what both entry points need.
Move the bodies of `Padded` and `TryPlace` onto it verbatim; leave every comment attached to
the line it explains.

```cpp
	/**
	 * The ground a plot has, and what has been stood on it so far.
	 *
	 * EXTRACTED FROM LayOut RATHER THAN COPIED, because Reserve needs the identical sampler:
	 * two samplers would be two answers to "does this fit", and the preview would stop being
	 * the thing that gets built - which is the one property the whole reservation design
	 * rests on.
	 */
	struct FYardSpace
	{
		TArrayView<const FVector2D> Outline;
		FVector2D Gate = FVector2D::ZeroVector;
		FVector2D Inward = FVector2D::ZeroVector;
		FVector2D Across = FVector2D::ZeroVector;
		double InwardBearing = 0.0;
		double Deepest = 0.0;
		FVector2D Min = FVector2D::ZeroVector;
		FVector2D Max = FVector2D::ZeroVector;

		/** Corner rectangles of every stand already placed. Grows as we go. */
		TArray<TArray<FVector2D>> Taken;

		FRandomStream Stream;

		/** Stand it on the gate's ray, as deep as its whole footprint fits. */
		bool PlaceAgainstTheBackFence(const PlotYard::FFootprint& Footprint,
			PlotYard::FStand& OutStand);

		/** Sample up to MaxTries poses. False means dropped, which is a real answer. */
		bool TryPlace(const PlotYard::FFootprint& Footprint, PlotYard::FStand& OutStand);
	};

	/** Build the space from a plot. Returns false for a degenerate outline. */
	bool MakeYardSpace(TArrayView<const FVector2D> Outline, FVector2D FrontageA,
		FVector2D FrontageB, FVector2D Gate, int32 Seed, FYardSpace& Out);
```

`PlaceAgainstTheBackFence` takes the loop currently inlined in `LayOut`'s first pass,
including the 2026-09-17 comment about the shed standing in the gateway. Note that it must
also push the placed stand's corners onto `Taken`, which `LayOut` does in a separate loop
today — folding it in is the one behavioural detail to get right, and the existing tests
cover it.

- [ ] **Step 3: Rewrite `LayOut` to use it**

`LayOut` keeps its signature, its ordering (back-fence first, then largest-first sampling),
and its `RoomForMore` phantom loop. Its body becomes calls onto `FYardSpace`.

- [ ] **Step 4: Run the tests — same count, same result**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve`
Expected: PASS, with the same number of tests as Step 1. A changed count means something
moved that should not have.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp
git commit -m "refactor(airside): lift the yard sampler out of LayOut so a second caller can use it"
```

---

### Task 3: `PlotYard::Reserve` — weighted round-robin, no runs yet

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/PlotYard.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotReserveTest.cpp` (create)

**Interfaces:**
- Consumes: `FYardSpace` from Task 2.
- Produces: `PlotYard::FKitSpec`, `PlotYard::FReservedStand`, `PlotYard::FReservation` with
  `TArray<FReservedStand> Stands` and `int32 CeilingFor(int32 KitIndex) const`, and
  `PlotYard::Reserve(Outline, FrontageA, FrontageB, Gate, TArrayView<const FKitSpec>, Seed)`.
  Task 4 adds `RunCap` handling; Task 6 consumes all of it.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/PlotReserveTest.cpp`. Re-declare the
fixtures rather than sharing them with `PlotYardTest.cpp` — that file's `Shed()` names a
footprint, and this file needs specs.

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Same rectangle PlotYardTest and PlotFitTest use, so all three describe one world. */
	TArray<FVector2D> YardRect(double Width, double Depth)
	{
		return { FVector2D(0.0, 0.0), FVector2D(Width, 0.0),
		         FVector2D(Width, Depth), FVector2D(0.0, Depth) };
	}

	/** Shed, tank, pump, at the grey-box figures and the design doc's 3/2/1 weights. */
	TArray<PlotYard::FKitSpec> DepotSpecs()
	{
		PlotYard::FKitSpec Shed;
		Shed.Footprint.LengthUu = 800.0;
		Shed.Footprint.WidthUu = 400.0;
		Shed.Footprint.bAgainstTheBackFence = true;
		Shed.ReserveWeight = 3;

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
}

/**
 * A reservation contains only what it placed, and the ceilings are what it placed.
 *
 * "NEVER DROPS" IS NOT THE SAME CLAIM LayOut MAKES. LayOut is handed a list it must try to
 * honour and reports what it could not fit; Reserve decides the list itself, so a dropped
 * stand is not a refusal, it is a bug. Every ghosted slot the player is shown is a promise
 * that the module fits there, and this test is what makes the promise true.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveNeverDropsTest,
	"Airside.Solve.PlotReserveNeverDrops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveNeverDropsTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = DepotSpecs();

	const PlotYard::FReservation Reservation = PlotYard::Reserve(
		Outline, FVector2D(0.0, 0.0), FVector2D(3200.0, 0.0), FVector2D(1600.0, 0.0),
		Specs, /*Seed=*/1234);

	TestTrue(TEXT("a 32 x 24 m plot reserves something"), Reservation.Stands.Num() > 0);

	for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
	{
		TestTrue(TEXT("every reserved stand was placed"), Stand.bPlaced);
		TestTrue(TEXT("every reserved stand names a kit"),
			Specs.IsValidIndex(Stand.KitIndex));
	}

	// The ceilings are derived from the stands, never counted alongside them: a second count
	// is a second thing to keep in agreement, which is why FYard::DroppedCount is derived too.
	int32 Total = 0;
	for (int32 Kit = 0; Kit < Specs.Num(); ++Kit)
	{
		Total += Reservation.CeilingFor(Kit);
	}
	TestEqual(TEXT("the ceilings account for every stand"), Total, Reservation.Stands.Num());

	return true;
}

/**
 * No two reserved stands overlap.
 *
 * THE INVARIANT THE GHOST SELLS. A ghosted slot promises the module fits there, and two
 * promises over one piece of ground is a mesh through a mesh - the one failure no camera
 * angle hides. Checked with the solver's own StandCorners so the test is not checking its
 * own arithmetic, which is the same reason that function is public at all.
 *
 * SEVERAL SEEDS, because one seed proves one roll and the sampler is what is under test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveNeverOverlapsTest,
	"Airside.Solve.PlotReserveNeverOverlaps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveNeverOverlapsTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = DepotSpecs();

	for (int32 Seed = 0; Seed < 8; ++Seed)
	{
		const PlotYard::FReservation Reservation = PlotYard::Reserve(
			Outline, FVector2D(0.0, 0.0), FVector2D(3200.0, 0.0), FVector2D(1600.0, 0.0),
			Specs, Seed);

		// The run's footprint, not the module's - a three-bay shed occupies three bays of
		// ground, and testing the module's width would pass while two runs sat inside
		// each other.
		auto RunFootprint = [&Specs](const PlotYard::FReservedStand& Stand)
		{
			PlotYard::FFootprint Out = Specs[Stand.KitIndex].Footprint;
			Out.WidthUu *= Stand.RunLength;
			return Out;
		};

		for (int32 A = 0; A < Reservation.Stands.Num(); ++A)
		{
			for (int32 B = A + 1; B < Reservation.Stands.Num(); ++B)
			{
				TArray<FVector2D> CornersA;
				TArray<FVector2D> CornersB;
				PlotYard::StandCorners(Reservation.Stands[A],
					RunFootprint(Reservation.Stands[A]), CornersA);
				PlotYard::StandCorners(Reservation.Stands[B],
					RunFootprint(Reservation.Stands[B]), CornersB);

				bool bSeparated = false;
				const FVector2D Axes[] = {
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
						bSeparated = true;
						break;
					}
				}

				TestTrue(*FString::Printf(
					TEXT("seed %d: stands %d and %d do not overlap"), Seed, A, B),
					bSeparated);
			}
		}
	}

	return true;
}

/**
 * Weights set the ratio: more sheds than tanks, more tanks than pumps.
 *
 * NOT AN EXACT 3:2:1. The cycle is 3 sheds, 2 tanks, 1 pump, but a shed is 32 m2 against a
 * pump's 6, so the plot runs out of room for sheds long before it runs out for pumps and the
 * tail of the fill is small things. The ORDERING is what the weights buy and what is
 * asserted; an exact ratio would be asserting the plot's area, not the rule.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveHonoursWeightsTest,
	"Airside.Solve.PlotReserveHonoursWeights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveHonoursWeightsTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = DepotSpecs();

	const PlotYard::FReservation Reservation = PlotYard::Reserve(
		Outline, FVector2D(0.0, 0.0), FVector2D(3200.0, 0.0), FVector2D(1600.0, 0.0),
		Specs, /*Seed=*/1234);

	TestTrue(TEXT("at least one of every kit fits a 32 x 24 m plot"),
		Reservation.CeilingFor(0) > 0 && Reservation.CeilingFor(1) > 0
		&& Reservation.CeilingFor(2) > 0);

	// The first cycle is Shed Shed Shed Tank Tank Pump, so the first six stands - as many as
	// the plot took - must follow that order. This asserts the RULE rather than the outcome.
	const int32 Expected[] = { 0, 0, 0, 1, 1, 2 };
	for (int32 I = 0; I < Reservation.Stands.Num() && I < 6; ++I)
	{
		TestEqual(*FString::Printf(TEXT("stand %d belongs to the cycle's kit"), I),
			Reservation.Stands[I].KitIndex, Expected[I]);
	}

	return true;
}

/**
 * Same plot and seed, same stands - to the bit.
 *
 * LOAD-BEARING, not a nicety. Nothing about the reservation is saved; it is re-derived every
 * time the presenter rebuilds. A solve that wandered would move a player's built depot when
 * they laid a road somewhere else on the airport.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveIsDeterministicTest,
	"Airside.Solve.PlotReserveIsDeterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveIsDeterministicTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = DepotSpecs();

	const PlotYard::FReservation A = PlotYard::Reserve(Outline, FVector2D(0.0, 0.0),
		FVector2D(3200.0, 0.0), FVector2D(1600.0, 0.0), Specs, /*Seed=*/77);
	const PlotYard::FReservation B = PlotYard::Reserve(Outline, FVector2D(0.0, 0.0),
		FVector2D(3200.0, 0.0), FVector2D(1600.0, 0.0), Specs, /*Seed=*/77);

	if (!TestEqual(TEXT("the same plot reserves the same number of stands"),
		A.Stands.Num(), B.Stands.Num()))
	{
		return false;
	}

	for (int32 I = 0; I < A.Stands.Num(); ++I)
	{
		TestEqual(TEXT("same kit"), A.Stands[I].KitIndex, B.Stands[I].KitIndex);
		TestTrue(TEXT("same centre"),
			A.Stands[I].Centre.Equals(B.Stands[I].Centre, 0.0));
		TestEqual(TEXT("same heading"), A.Stands[I].Heading, B.Stands[I].Heading);
	}

	return true;
}

/**
 * No reserved stand stands in the gateway.
 *
 * A DEPOT THE TRUCK CANNOT LEAVE LOOKS PERFECTLY CORRECT FROM EVERY ANGLE - the reason
 * GateCorridorUu is a counted rule rather than something eyeballed in PIE.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveLeavesTheGateClearTest,
	"Airside.Solve.PlotReserveLeavesTheGateClear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveLeavesTheGateClearTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(3200.0, 2400.0);
	const TArray<PlotYard::FKitSpec> Specs = DepotSpecs();
	const FVector2D Gate(1600.0, 0.0);
	const FVector2D Inward(0.0, 1.0);
	const FVector2D Across(1.0, 0.0);

	// Several seeds: one seed proves one roll, and the sampler is the thing under test.
	for (int32 Seed = 0; Seed < 8; ++Seed)
	{
		const PlotYard::FReservation Reservation = PlotYard::Reserve(
			Outline, FVector2D(0.0, 0.0), FVector2D(3200.0, 0.0), Gate, Specs, Seed);

		TArray<FVector2D> Corners;
		for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
		{
			PlotYard::FFootprint Run = Specs[Stand.KitIndex].Footprint;
			Run.WidthUu *= Stand.RunLength;
			PlotYard::StandCorners(Stand, Run, Corners);

			for (const FVector2D& Corner : Corners)
			{
				const FVector2D FromGate = Corner - Gate;
				const double Into = FVector2D::DotProduct(FromGate, Inward);
				const bool bInCorridor = Into >= 0.0 && Into <= PlotYard::GateCorridorUu
					&& FMath::Abs(FVector2D::DotProduct(FromGate, Across))
						< PlotYard::GateCorridorUu * 0.5;
				TestFalse(*FString::Printf(TEXT("seed %d keeps the gate clear"), Seed),
					bInCorridor);
			}
		}
	}

	return true;
}

#endif
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve.PlotReserve`
Expected: compile failure — `PlotYard::FKitSpec`, `FReservation` and `Reserve` do not exist.

- [ ] **Step 3: Declare the types**

In `Plugins/Airside/Source/Airside/Public/Solve/PlotYard.h`, below `FYard`:

```cpp
	/**
	 * One kind of thing a plot can reserve room for.
	 *
	 * STILL NOT EDepotModule. A spec is a footprint and two integers, and mapping a kit to
	 * one stays UPlotPresenter's job on the other side of the seam - the same split FitBays
	 * makes by taking an outline rather than an entity, and what keeps these tests free of a
	 * world, an actor and NewObject.
	 */
	struct FKitSpec
	{
		/** ONE module's, never a run's. Reserve multiplies it by RunLength itself. */
		FFootprint Footprint;

		/** How often this kit comes up in the fill cycle. Sets a ceiling, not a strategy. */
		int32 ReserveWeight = 1;

		/** Modules of this kit grouped into one stand at one heading. 1 = never grouped. */
		int32 RunCap = 1;
	};

	struct FReservedStand : FStand
	{
		/** Into the Kits array Reserve was given. */
		int32 KitIndex = INDEX_NONE;

		/** Modules this stand holds. 1 unless the kit groups into runs. */
		int32 RunLength = 1;
	};

	/**
	 * Everything a plot has room for, decided once.
	 *
	 * ONLY WHAT IT PLACED. Unlike FYard, which reports a module it could not fit, a
	 * reservation has nothing to refuse - it chose the list. A stand here is a promise that
	 * the module fits, which is what lets the player be shown ghosted slots and charged for
	 * them.
	 */
	struct FReservation
	{
		/** In placement order, which is the fill cycle's order. */
		TArray<FReservedStand> Stands;

		/**
		 * How many modules of one kit this plot can hold.
		 *
		 * DERIVED, never stored beside Stands: a second count is a second thing to keep in
		 * agreement, for the same reason FYard::DroppedCount is derived.
		 */
		int32 CeilingFor(int32 KitIndex) const
		{
			int32 Count = 0;
			for (const FReservedStand& Stand : Stands)
			{
				if (Stand.KitIndex == KitIndex) { Count += Stand.RunLength; }
			}
			return Count;
		}
	};

	/**
	 * Fill the plot, and report what fits.
	 *
	 * THE PLOT'S CAPACITY IS WHAT THIS PLACED, not a number derived beside it. A budget in
	 * bays or square metres would assert that a packing exists without proving one, against
	 * a sampler that keeps a gate corridor clear, holds ClearanceUu between modules and
	 * jitters headings - and it would be right nearly always. See the design doc section 3.1.
	 */
	AIRSIDE_API FReservation Reserve(TArrayView<const FVector2D> Outline,
		FVector2D FrontageA, FVector2D FrontageB, FVector2D Gate,
		TArrayView<const FKitSpec> Kits, int32 Seed);
```

- [ ] **Step 4: Implement the fill**

In `PlotYard.cpp`, using `FYardSpace` from Task 2. Runs are Task 4 — every stand here has
`RunLength = 1` and `RunCap` is ignored.

```cpp
PlotYard::FReservation PlotYard::Reserve(TArrayView<const FVector2D> Outline,
	FVector2D FrontageA, FVector2D FrontageB, FVector2D Gate,
	TArrayView<const FKitSpec> Kits, int32 Seed)
{
	FReservation Reservation;

	FYardSpace Space;
	if (!MakeYardSpace(Outline, FrontageA, FrontageB, Gate, Seed, Space))
	{
		return Reservation;
	}

	// THE CYCLE, EXPANDED ONCE. Weight 3 means three attempts per turn round the kits, and
	// building the order up front keeps the loop below a plain walk rather than three nested
	// counters that have to agree.
	//
	// LARGEST FIRST WITHIN A CYCLE, which is LayOut's lesson kept rather than re-learnt: a
	// tank offered the yard after four pumps have taken the middle has nowhere left to go,
	// and the player loses the big object rather than the small one.
	TArray<int32> Cycle;
	for (int32 Kit = 0; Kit < Kits.Num(); ++Kit)
	{
		for (int32 N = 0; N < FMath::Max(Kits[Kit].ReserveWeight, 0); ++N)
		{
			Cycle.Add(Kit);
		}
	}
	Cycle.Sort([&Kits](int32 A, int32 B)
	{
		if (A == B) { return false; }
		return Kits[A].Footprint.LengthUu * Kits[A].Footprint.WidthUu
			> Kits[B].Footprint.LengthUu * Kits[B].Footprint.WidthUu;
	});

	if (Cycle.Num() == 0)
	{
		return Reservation;
	}

	// A WHOLE CYCLE THAT PLACES NOTHING MEANS FULL, rather than one failure meaning full: a
	// plot with no room left for a shed may still take three pumps, and stopping at the shed
	// would waste the corner the player paid for.
	//
	// The cap is a backstop against a zero-area footprint looping forever, not an expected
	// limit - the same role the cap plays in LayOut's RoomForMore loop.
	bool bPlacedAny = true;
	while (bPlacedAny && Reservation.Stands.Num() < 256)
	{
		bPlacedAny = false;
		for (const int32 Kit : Cycle)
		{
			FReservedStand Stand;
			Stand.KitIndex = Kit;
			Stand.RunLength = 1;

			const bool bPlaced = Kits[Kit].Footprint.bAgainstTheBackFence
					&& Reservation.CeilingFor(Kit) == 0
				? Space.PlaceAgainstTheBackFence(Kits[Kit].Footprint, Stand)
				: Space.TryPlace(Kits[Kit].Footprint, Stand);

			if (bPlaced)
			{
				Reservation.Stands.Add(Stand);
				bPlacedAny = true;
			}
		}
	}

	return Reservation;
}
```

Note the back-fence condition: **only the first stand of a back-fence kit takes the ray**,
because the ray is one line of ground from the gate and a second stand on it would contend
with the first. See the planning decision at the top of this document.

- [ ] **Step 5: Run test to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve`
Expected: PASS — the five new tests plus every existing `PlotYard` test.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Solve/PlotYard.h `
        Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotReserveTest.cpp
git commit -m "feat(airside): a plot reserves its whole layout at draw time"
```

---

### Task 4: Runs

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotReserveTest.cpp`

**Interfaces:**
- Consumes: Task 3's `FKitSpec::RunCap` and `FReservedStand::RunLength`, unused until now.
- Produces: nothing new. `CeilingFor` already sums `RunLength`, so it needs no change.

- [ ] **Step 1: Write the failing test**

Append to `PlotReserveTest.cpp`:

```cpp
/**
 * Three sheds become one stand three bays wide, at one heading.
 *
 * RESERVED AT FULL WIDTH UP FRONT, never grown. A run that grew as the player bought bays
 * would need ground it was never promised, and the promise is the whole design: every
 * ghosted slot is a claim that the module fits there.
 *
 * NO JITTER INSIDE A RUN. The bays share walls, so a heading that wandered between them
 * would open a wedge of daylight down the middle of one building.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveGroupsShedsIntoRunsTest,
	"Airside.Solve.PlotReserveGroupsShedsIntoRuns",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveGroupsShedsIntoRunsTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(3200.0, 2400.0);

	TArray<PlotYard::FKitSpec> Specs = DepotSpecs();
	Specs[0].RunCap = 3;

	const PlotYard::FReservation Reservation = PlotYard::Reserve(
		Outline, FVector2D(0.0, 0.0), FVector2D(3200.0, 0.0), FVector2D(1600.0, 0.0),
		Specs, /*Seed=*/1234);

	int32 ShedStands = 0;
	for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
	{
		if (Stand.KitIndex != 0)
		{
			continue;
		}
		++ShedStands;
		TestTrue(TEXT("a shed stand holds between one and RunCap bays"),
			Stand.RunLength >= 1 && Stand.RunLength <= 3);
	}

	TestTrue(TEXT("a 32 x 24 m plot reserves at least one shed run"), ShedStands > 0);

	// Grouping is not cosmetic: three sheds in a 12 m run occupy less frontage than three
	// scattered 4 m sheds each carrying a 1 m clearance skirt, so the ceiling must not fall.
	TestTrue(TEXT("grouping does not cost the plot sheds"),
		Reservation.CeilingFor(0) >= 3);

	return true;
}

/**
 * Only one shed run stands against the back fence.
 *
 * THE RAY IS ONE LINE OF GROUND. PlaceAgainstTheBackFence walks depths along the gate's
 * inward ray, so a second stand offered the same ray either lands on the first or is
 * refused. The rest of the runs are sampled like anything else. Worth watching in PIE - if a
 * second run adrift in the yard reads wrong, cap shed runs at one rather than widening the
 * ray.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReserveBacksOneRunOnlyTest,
	"Airside.Solve.PlotReserveBacksOneRunOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReserveBacksOneRunOnlyTest::RunTest(const FString& Parameters)
{
	// Deep and wide enough that more than one shed run fits.
	const TArray<FVector2D> Outline = YardRect(6000.0, 3000.0);

	TArray<PlotYard::FKitSpec> Specs = DepotSpecs();
	Specs[0].RunCap = 3;

	const PlotYard::FReservation Reservation = PlotYard::Reserve(
		Outline, FVector2D(0.0, 0.0), FVector2D(6000.0, 0.0), FVector2D(3000.0, 0.0),
		Specs, /*Seed=*/1234);

	// The back-fence heading is the inward bearing exactly - no jitter - so counting stands
	// at exactly that heading counts the ones that took the ray.
	int32 Squared = 0;
	for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
	{
		if (Stand.KitIndex == 0 && Stand.Heading == UE_DOUBLE_HALF_PI)
		{
			++Squared;
		}
	}

	TestEqual(TEXT("exactly one shed run is square against the back fence"), Squared, 1);
	return true;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve.PlotReserveGroups`
Expected: FAIL — every shed stand has `RunLength` 1, so `CeilingFor(0) >= 3` fails unless
three separate sheds happened to fit, and the run width is never three bays.

- [ ] **Step 3: Reserve runs at full width**

In `Reserve`'s fill loop, replace the single-module placement with a run attempt that shrinks.

```cpp
			// AS LONG AS IT CAN, THEN SHORTER. A plot with room for two bays should get a
			// two-bay run rather than nothing: refusing the whole run because the third bay
			// does not fit would leave ground empty that the player drew and paid for.
			const int32 Cap = FMath::Clamp(Kits[Kit].RunCap, 1, 64);
			bool bPlaced = false;
			for (int32 Length = Cap; Length >= 1 && !bPlaced; --Length)
			{
				// THE RUN'S FOOTPRINT, not the module's. Bays share walls, so a run is N
				// times as wide and exactly as deep - no clearance between bays, because
				// they are one building. FKitSpec::Footprint stays one module's so that this
				// is the only place the multiplication happens.
				FFootprint Run = Kits[Kit].Footprint;
				Run.WidthUu *= Length;

				Stand.RunLength = Length;
				bPlaced = Run.bAgainstTheBackFence && Reservation.CeilingFor(Kit) == 0
					? Space.PlaceAgainstTheBackFence(Run, Stand)
					: Space.TryPlace(Run, Stand);
			}
```

`Space.PlaceAgainstTheBackFence` and `Space.TryPlace` push the run's own corners onto
`Taken`, so the width multiplication must happen before the call, not after.

- [ ] **Step 4: Run test to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve`
Expected: PASS, all seven `PlotReserve` tests plus the existing `PlotYard` suite.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Private/Solve/PlotYard.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotReserveTest.cpp
git commit -m "feat(airside): sheds reserve as runs of up to three bays"
```

---

### Task 5: Give the presenter a stable instance index

A pure refactor, ahead of the behaviour change in Task 6. Today `GetInstanceTransformForTest`
indexes straight into one `UInstancedStaticMeshComponent`; once there is a component per mesh
that index means nothing. Doing it now keeps Task 6's diff about reservation.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp` (unchanged)

**Interfaces:**
- Produces: a private `TArray<FTransform> Placed` on `UPlotPresenter`, appended in the same
  order instances are added. `GetInstanceTransformForTest` reads it rather than the component.

- [ ] **Step 1: Run the existing presenter tests and record the baseline**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present`
Expected: PASS. Record the count.

- [ ] **Step 2: Record every transform as it is added**

In `PlotPresenter.h`, beside the existing counters:

```cpp
	/**
	 * Every transform added during the last rebuild, in the order it was added.
	 *
	 * BECAUSE THE INSTANCE INDEX IS ABOUT TO STOP MEANING ANYTHING. One component held every
	 * box, so "instance 3" was a fact about the plot; a component per authored mesh makes it
	 * a fact about which mesh happened to be used. The tests ask about the PLOT, so the
	 * order they rely on lives here rather than in a component they do not own.
	 */
	TArray<FTransform> Placed;
```

In `PlotPresenter.cpp`, `Placed.Reset()` beside the other counters in `RebuildFrom`, and
`Placed.Add(...)` at each of the two `AddInstance` call sites. Rewrite the accessor:

```cpp
bool UPlotPresenter::GetInstanceTransformForTest(int32 Index, FTransform& OutTransform) const
{
	if (!Placed.IsValidIndex(Index))
	{
		return false;
	}
	OutTransform = Placed[Index];
	return true;
}
```

Change `GetInstanceCount` to `return Placed.Num();` so the two agree by construction, and
change the census line's fence count from `Boxes->GetInstanceCount() - ModuleBoxes` to
`Placed.Num() - ModuleBoxes`.

- [ ] **Step 3: Run the tests — same count, same result**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present`
Expected: PASS, same count as Step 1.

- [ ] **Step 4: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h `
        Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp
git commit -m "refactor(airside): the presenter owns its instance order, not the component"
```

---

### Task 6: The presenter draws the reservation

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Build/DepotKit.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Build/DepotKit.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp`

**Interfaces:**
- Consumes: `PlotYard::Reserve`, `DepotFootprint(Module, Content)`.
- Produces: `TArray<PlotYard::FKitSpec> DepotKitSpecs(const UAirsideContent*)` — one spec per
  `EDepotModule` value, in enum order, so a kit index IS an `EDepotModule`. Task 7 uses it.
  `UPlotPresenter::GetGhostCount()`, and `Initialise` gains a second component.

**Two things this task settles that the spec left open:**

*Where the presenter gets its content.* It calls `UAirsideSettings::GetContent()` inside
`RebuildFrom`, the same accessor `ARoadNetworkActor` uses at eleven call sites. In tests that
returns null and every kit falls back to the grey-box table — which is exactly why Task 1
built the fallback, and why these tests need no content fixture.

*Ghosts need their own component.* An instance carries a transform, not a material, so a
ghosted box cannot differ from a solid one inside one `UInstancedStaticMeshComponent`.
`Initialise` takes a second component for ghosts, wearing `UAirsideContent::GhostMaterial`,
and `ARoadNetworkActor::InitialisePresenterLayers` creates it beside `PlotBoxes`.

Note what this task does **not** do: the spec's §6 "one ISM per distinct static mesh" and the
run-anchoring mesh choice both need meshes to exist, so they belong to Plan B. Plan A is two
components — solid and ghost — and the split by mesh happens when there is a mesh to split by.

- [ ] **Step 1: Write the failing test**

Append to `PlotPresenterTest.cpp`. Follow the fixture already in that file for building a
`URoadNetwork` with a plotted depot.

```cpp
/**
 * A plot draws every reserved stand: the bought ones solid, the rest ghosted.
 *
 * THE GHOST IS NOT A MARKER. The 2026-09-16 spec removed a GRID of slot markers because a
 * uniform grid claimed a structure the scattered yard did not have. A ghost here is a solved
 * stand - its own footprint, its own sampled heading, from the code path that will place the
 * module when it is bought. It does not claim the yard has a structure; it shows the yard.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterGhostsUnboughtSlotsTest,
	"Airside.Present.PlotPresenterGhostsUnboughtSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterGhostsUnboughtSlotsTest::RunTest(const FString& Parameters)
{
	// COMPOSITION LEVEL, per CLAUDE.md and the two tests above it: every Airside.Solve
	// test would still pass if the presenter called Reserve and then drew the owned
	// modules anyway, which is the "declared but never consumed" shape this project has
	// shipped three times.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	if (!TestNotNull(TEXT("a plot presenter"), Actor->GetPlotPresenter())) { return false; }

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Actor->ClearNetwork();

	// THE DEEP PLOT, not ThreeBayPlotAt: 20 x 24 m has room to reserve more than the
	// starter one-of-each, and a plot that reserved exactly three would pass this test
	// while proving nothing about ghosts.
	PlaceDeepDepot(Actor, Depot, /*X=*/0.0);
	Actor->RebuildMesh();

	const UPlotPresenter* Plots = Actor->GetPlotPresenter();

	// WHAT WAS BOUGHT IS DRAWN SOLID. PlaceDeepDepot owns one of each, so three bays are
	// lit however the reservation grouped them.
	TestEqual(TEXT("the three owned modules are drawn solid"), Plots->GetModuleCount(), 3);

	// AND THE SPARE ROOM IS DRAWN. This is the claim the whole design rests on: the player
	// can see what the plot would hold before spending anything on it.
	TestTrue(TEXT("a 20 x 24 m plot has spare capacity to ghost"),
		Plots->GetGhostCount() > 0);

	// A RESERVATION CANNOT DROP. Unlike LayOut, Reserve chose the list, so a drop is a bug
	// rather than a refusal - and this counter staying is how that stays visible.
	TestEqual(TEXT("nothing was dropped"), Plots->GetDroppedCount(), 0);

	// SOLID PLUS GHOSTED IS THE WHOLE RESERVATION, so no bay is drawn twice and none is
	// silently skipped. Recomputed here rather than remembered: the presenter derives it
	// the same way, and a second stored copy is the drift this project names most often.
	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(nullptr);
	const PlotYard::FReservation Reservation = PlotYard::Reserve(
		DeepPlotAt(0.0), FVector2D(0.0, 0.0), FVector2D(2000.0, 0.0),
		FVector2D(1000.0, 0.0), Specs, DepotYardSeed(FVector2D(1000.0, 0.0)));

	int32 Bays = 0;
	for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
	{
		Bays += Stand.RunLength;
	}
	TestEqual(TEXT("every reserved bay is drawn, once"),
		Plots->GetModuleCount() + Plots->GetGhostCount(), Bays);

	return true;
}
```

Add `#include "Build/DepotKit.h"` and `#include "Solve/PlotYard.h"` to this file's includes.

- [ ] **Step 2: Run test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present.PlotPresenterGhosts`
Expected: compile failure — `GetGhostCount` does not exist.

- [ ] **Step 3: Add `DepotKitSpecs`**

In `DepotKit.h`:

```cpp
/**
 * Every module kind a depot can hold, as specs the yard solver understands.
 *
 * IN ENUM ORDER, so a spec's index IS its EDepotModule and the presenter needs no second
 * map to get back. Built by walking the enum rather than from a list written here: a list
 * would answer only for the modules somebody remembered to add, which is the failure
 * AircraftLookTest exists for.
 */
AIRSIDE_API TArray<PlotYard::FKitSpec> DepotKitSpecs(const UAirsideContent* Content);
```

In `DepotKit.cpp`, walk `EDepotModule` from `Shed` to `Pump`, taking `Footprint` from
`DepotFootprint(Module, Content)` and `ReserveWeight` / `RunCap` from the kit when there is
one — falling back to weight 3 / cap 3 for `Shed`, 2 / 1 for `Tank`, 1 / 1 for `Pump`, which
are the design doc's figures and keep the grey-box path behaving sensibly.

- [ ] **Step 4: Rewrite `RebuildFrom`'s module pass**

Replace the `Footprints` / `LayOut` block with `Reserve`, and draw both kinds of stand.
Keep the fence loop below it untouched — that is Plan C.

Hoist the specs **above** the entity loop. They are the same for every plot, and building
them per entity would resolve the same three kits once per depot on the airport:

```cpp
	// NULL IS A LEGAL ANSWER, and the tests rely on it: with no content every kit falls back
	// to the grey-box table, which is what lets this whole slice be tested without authoring
	// a single asset. Same accessor ARoadNetworkActor uses at every other content call site.
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	const TArray<PlotYard::FKitSpec> Specs = DepotKitSpecs(Content);
```

Then, inside the loop:

```cpp
		const PlotYard::FReservation Reservation = PlotYard::Reserve(
			Entity.Outline, FrontageA, FrontageB, Entity.Position,
			Specs, DepotYardSeed(Entity.Position));

		// HOW MANY OF EACH THE PLAYER HAS BOUGHT. Entity.Modules is still the owned list and
		// still the save's only record of this depot - the reservation is re-derived, so
		// nothing here is stored.
		TArray<int32> Owned;
		Owned.SetNumZeroed(Specs.Num());
		for (const EDepotModule Module : Entity.Modules)
		{
			Owned[static_cast<int32>(Module)] += 1;
		}

		// MODULES BEFORE THE FENCE, always: Airside.Present.PlotPresenterScattersModules
		// names the first ModuleBoxes instances of a plot as its modules, and reordering
		// these loops would silently make it measure fence panels instead. The ghosts go
		// between them, and GetGhostCount is how the test tells the two apart.
		for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
		{
			const PlotYard::FKitSpec& Spec = Specs[Stand.KitIndex];

			// A RUN FILLS FROM ONE END. Bays the player owns are drawn solid at that end and
			// the rest ghosted, so a run visibly GROWS along its length rather than
			// appearing whole. While these are boxes it is two boxes; with meshes it becomes
			// BakedMeshes[owned - 1] plus a ghost for the remainder.
			const int32 Lit = FMath::Clamp(Owned[Stand.KitIndex], 0, Stand.RunLength);
			Owned[Stand.KitIndex] -= Lit;

			// ... add a solid box Lit bays wide, then a ghosted box (RunLength - Lit) wide,
			// each anchored to its own end of the run's width. ModuleBoxes counts the solid
			// ones and Ghosts the rest.
		}
```

Add `GetGhostCount()` and a private `Ghosts` counter to `PlotPresenter.h`, reset beside the
others. Extend the census line to name the ghosts: a depot drawn entirely in ghosts is a
depot nobody bought anything for, and that should be visible in the log rather than inferred.

`Dropped` stays and stays zero — `Reserve` returns only what it placed. Leave the counter and
its accessor so the invariant is visible rather than assumed.

- [ ] **Step 5: Run test to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present`
Expected: PASS, including the existing `PlotPresenterScattersModules`.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Build/DepotKit.h `
        Plugins/Airside/Source/Airside/Private/Build/DepotKit.cpp `
        Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h `
        Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp
git commit -m "feat(airside): a depot draws its reserved slots, ghosting the ones nobody bought"
```

---

### Task 7: The readout shows ceilings

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/PlotPlaceTool.h:104` (`SetModules`), `:128` (`YardFor`)
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp:353-364` (`YardFor`), `:676-685` (the ghost outlines), `:687-735` (`BuildReadout`)
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotPlaceToolTest.cpp`

**Interfaces:**
- Consumes: `DepotKitSpecs`, `PlotYard::Reserve`, `FReservation::CeilingFor`.
- Produces: `FPlotPlaceTool::ReservationFor(TArrayView<const FVector2D>) const`, replacing
  `YardFor`.

- [ ] **Step 1: Write the failing test**

Append to `PlotPlaceToolTest.cpp`, following its existing fixture for driving the tool
through anchor and drag:

```cpp
/**
 * Dragging a bigger plot raises what it will hold.
 *
 * THE READOUT IS THE GHOST'S OWN NUMBERS. YardFor was shared by the ghost and the readout so
 * the boxes on screen and the figures beside them were one computation; ReservationFor keeps
 * that property. A second evaluator for the numbers would be a preview quietly describing a
 * different depot.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPlaceToolReadsOutCeilingsTest,
	"Airside.Tool.PlotPlaceToolReadsOutCeilings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPlaceToolReadsOutCeilingsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	// How many sheds a plot of this depth says it will hold, read off the readout the way
	// the player reads it - not off Reserve directly. A test that called the solver would
	// pass while the readout still said "Room for N", which is the line being replaced.
	auto ShedsFor = [Actor](double DepthUu) -> int32
	{
		FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
		DrawPlot(Tool, Actor, FVector2D(0.0, 0.0), FVector2D(2000.0, 0.0),
			FVector2D(2000.0, DepthUu));

		FToolReadoutCollector Collector;
		Tool.BuildReadout(PlotAt(Actor, FVector2D(1000.0, DepthUu)), Collector);

		for (const FToolFact& Fact : Collector.Readout.Facts)
		{
			if (Fact.Label == TEXT("Sheds"))
			{
				return FCString::Atoi(*Fact.Value);
			}
		}
		return INDEX_NONE;
	};

	const int32 Small = ShedsFor(1200.0);
	const int32 Large = ShedsFor(2400.0);

	// THE READOUT NAMES THE KIT. "Room for 4" could only ever mean "4 of the sample
	// footprint", which is a number about a phantom tank rather than about anything the
	// player can buy.
	TestTrue(TEXT("the readout names sheds"), Small != INDEX_NONE && Large != INDEX_NONE);
	TestTrue(TEXT("a plot that can hold a depot holds at least one shed"), Small >= 1);

	// GREATER OR EQUAL, NOT GREATER. Sampling is not monotonic in plot size - see the design
	// doc section 11 - so a strict increase is not a property this solver has. If even this
	// flakes, that is the risk landing, and the answer is section 11's option 2 or 3 rather
	// than a looser assertion here.
	TestTrue(*FString::Printf(TEXT("a deeper plot holds no fewer sheds: %d then %d"),
		Small, Large), Large >= Small);

	// AND PlotFit STILL GUARDS THE FLOOR. Reserve answers "what fits"; whether the plot is
	// legal at all is a different question with a different failure mode, and it is still
	// PlotFit's. A plot under one bay never reaches the readout.
	FPlotPlaceTool Tiny(EPlaceableEntity::FuelDepot);
	DrawPlot(Tiny, Actor, FVector2D(0.0, 0.0), FVector2D(200.0, 0.0),
		FVector2D(200.0, 200.0));
	FToolReadoutCollector TinyCollector;
	Tiny.BuildReadout(PlotAt(Actor, FVector2D(100.0, 200.0)), TinyCollector);
	TestFalse(TEXT("a plot under one bay is not committable"),
		TinyCollector.Readout.bCommittable);

	return true;
}
```

Check `FToolFact`'s real field names against `IToolReadoutSink` before writing this — the
existing tests read `Collector.Readout.Warnings` and `.bCommittable`, and the facts array is
what `Sink.Fact(...)` feeds. If the label and value fields are named differently, follow the
header, not this plan.

**On monotonicity:** `>=` and not `>`, deliberately. Sampling is not monotonic in plot size
(design doc §11) and asserting a strict increase would make this test flake. If it flakes on
`>=` too, that is the risk landing and the answer is §11's option 2 or 3 — not a looser
assertion.

- [ ] **Step 2: Run test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool.PlotPlaceToolReadsOut`
Expected: compile failure — `ReservationFor` does not exist.

- [ ] **Step 3: Replace `YardFor` with `ReservationFor`**

Same shape as `YardFor`: build specs with `DepotKitSpecs`, call `Reserve` with
`DepotYardSeed(Pose)` so the preview and the built depot share a seed. Keep the doc comment
about the ghost and readout being one computation — it is still true and still the reason.

- [ ] **Step 4: Update the ghost and the readout**

The ghost loop draws `StandCorners` per reserved stand with the run's width
(`Spec.Footprint.WidthUu * Stand.RunLength`), not per owned module.

`BuildReadout` replaces `Modules` / `Room for` with a line per kit — "Sheds 3", "Tanks 2",
"Pumps 1" — and the `Stage == EPlotStage::Confirm` warning fires when the reservation is
empty rather than when `RoomForMore` is zero: a plot that holds nothing is the refusal worth
warning about, and a full one is now the normal end state.

- [ ] **Step 5: Delete `SetModules`**

It has no callers, and under reservation the plot's capacity comes from its shape rather than
from a list handed in. `Modules` keeps its `{Shed, Tank, Pump}` default as the starter build —
the concept sheet's Tier 1 depot, and the smallest one that works.

- [ ] **Step 6: Run the whole suite**

Run: `./Tools/Run-AirsideTests.ps1`
Expected: PASS across `Airside`, `AirportOps` and `AirportMgr`.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/PlotPlaceTool.h `
        Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotPlaceToolTest.cpp
git commit -m "feat(airside): the plot readout names what the plot will hold, per kit"
```

---

### Task 8: Author the kit assets, and guard them

**Files:**
- Create: `Content/Entities/DA_Kit_FuelShed.uasset`, `DA_Kit_FuelTank.uasset`, `DA_Kit_FuelPump.uasset`
- Modify: `Content/DA_AirsideContent.uasset` (fill `DepotKits`)
- Test: `Source/AirportMgr/PlotKitContentTest.cpp` (create)

**Interfaces:**
- Consumes: everything above.
- Produces: the content the whole slice has been falling back from.

- [ ] **Step 1: Write the failing test**

Create `Source/AirportMgr/PlotKitContentTest.cpp`, modelled on `AircraftLookTest.cpp`:

```cpp
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Content/AirsideContent.h"
#include "CoreMinimal.h"
#include "Entities/PlotModuleKit.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Every module has a kit, no two kits share a mesh, and a baked kit's variants are dense.
 *
 * IT WALKS THE ENUM AND THE REGISTRY, not a list written here - the lesson AircraftLookTest
 * paid for. Its first version compared a hand-written PAIR and passed while the A320 and the
 * 737 both still wore the default mesh, because a test that names its subjects can only
 * catch the subjects somebody remembered.
 *
 * In the game module because these are /Game assets: Airside may not reach them, and
 * Check-Architecture enforces that direction.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotKitContentTest,
	"AirportMgr.Content.EveryDepotModuleHasItsOwnKit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotKitContentTest::RunTest(const FString& Parameters)
{
	FAssetRegistryModule& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	Registry.Get().SearchAllAssets(true);

	FARFilter Filter;
	Filter.ClassPaths.Add(UAirsideContent::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Found;
	Registry.Get().GetAssets(Filter, Found);

	for (const FAssetData& Data : Found)
	{
		const UAirsideContent* Content = Cast<UAirsideContent>(Data.GetAsset());
		if (Content == nullptr) { continue; }

		TSet<FString> MeshPaths;

		// EVERY VALUE OF THE ENUM, so adding a module without a kit fails here rather than
		// silently falling back to a grey box in a shipped build.
		for (int32 Raw = 0; Raw <= static_cast<int32>(EDepotModule::Pump); ++Raw)
		{
			const EDepotModule Module = static_cast<EDepotModule>(Raw);
			const TObjectPtr<UPlotModuleKit>* Found2 = Content->DepotKits.Find(Module);

			if (!TestTrue(*FString::Printf(TEXT("%s maps module %d"), *Data.AssetName.ToString(), Raw),
				Found2 != nullptr && *Found2 != nullptr))
			{
				continue;
			}

			const UPlotModuleKit* Kit = *Found2;
			TestTrue(TEXT("a kit has a footprint"),
				Kit->Footprint.X > 0.0 && Kit->Footprint.Y > 0.0);
			TestTrue(TEXT("a kit's run cap is at least one"), Kit->RunCap >= 1);
			TestTrue(TEXT("a kit's weight is at least one"), Kit->ReserveWeight >= 1);

			if (Kit->Assembly != EKitAssembly::Baked) { continue; }

			// DENSE UP TO RunCap: the presenter indexes BakedMeshes[owned - 1], so a hole
			// is a run the player can buy and cannot see. Empty is legal and means grey box.
			if (Kit->BakedMeshes.Num() == 0) { continue; }

			TestEqual(TEXT("a baked kit has one mesh per bay count"),
				Kit->BakedMeshes.Num(), Kit->RunCap);

			for (const TSoftObjectPtr<UStaticMesh>& Mesh : Kit->BakedMeshes)
			{
				const FString Path = Mesh.ToString();
				TestFalse(TEXT("a baked variant is not null"), Path.IsEmpty());
				TestFalse(*FString::Printf(TEXT("no two kits share mesh %s"), *Path),
					MeshPaths.Contains(Path));
				MeshPaths.Add(Path);
			}
		}
	}

	return true;
}

#endif
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./Tools/Run-AirsideTests.ps1 -Filter AirportMgr.Content`
Expected: FAIL — `DA_AirsideContent`'s `DepotKits` map is empty.

- [ ] **Step 3: Author the three kits**

Through the editor, or scripted via `Tools/Mcp.py` and the `McpAutomationBridge` plugin — the
route `ABP_Plane1`'s AnimGraph was wired by. Figures are the grey-box table and the design
doc's weights, and they carry their provenance:

| Asset | Footprint (uu) | Height | Back fence | Weight | RunCap |
|---|---|---|---|---|---|
| `DA_Kit_FuelShed` | 800 x 400 | 400 | true | 3 | 3 |
| `DA_Kit_FuelTank` | 500 x 500 | 250 | false | 2 | 1 |
| `DA_Kit_FuelPump` | 300 x 200 | 150 | false | 1 | 1 |

`BakedMeshes` stays **empty** on all three — the meshes are Plan B. An empty array means grey
box, which the test permits and the presenter already handles.

- [ ] **Step 4: Fill `DepotKits` on `DA_AirsideContent`**

Map `Shed`, `Tank` and `Pump` to the three assets.

- [ ] **Step 5: Run test to verify it passes**

Run: `./Tools/Run-AirsideTests.ps1`
Expected: PASS across all three suites. The grey boxes should be unchanged on screen — the
kits restate the figures the fallback was already using, which is the point: content landing
changes nothing until the numbers change.

- [ ] **Step 6: Open PIE and check the risk**

Draw several plots, growing one slowly. Watch the readout's per-kit ceilings for a **drop as
the plot grows** — the non-monotonicity named in design doc §11. Record what you see in the
commit message whether or not it appears; "watched for it and did not see it at plot sizes
from 12 x 8 m to 60 x 30 m" is the measurement §11 asks for.

- [ ] **Step 7: Commit**

```bash
git add Content/Entities/DA_Kit_FuelShed.uasset `
        Content/Entities/DA_Kit_FuelTank.uasset `
        Content/Entities/DA_Kit_FuelPump.uasset `
        Content/DA_AirsideContent.uasset `
        Source/AirportMgr/PlotKitContentTest.cpp
git commit -m "feat(content): the three fuel depot kits, and a test that walks the enum"
```

---

## What this plan does not do

- **Meshes.** Every kit's `BakedMeshes` is empty; the depot is still grey boxes. Plan B.
- **One ISM per mesh, and the run-anchoring mesh choice** (spec §6). Both need a mesh to
  exist. Plan A draws two components — solid and ghost — and splits by mesh in Plan B.
- **The material-slot naming test** (spec §9.1, `<asset>_<look>`). It walks meshes, and there
  are none. Plan B, in the same file as the bounds test.
- **The footprint-versus-mesh-bounds test** (spec §9.1). Same reason. Plan B.
- **The fence.** `RebuildFrom`'s fence loop and its 2.5 m constants are untouched. Plan C.
- **A buy-an-upgrade UI.** `Entity.Modules` keeps its `{Shed, Tank, Pump}` starter, and the
  ghosted slots show capacity nobody can yet spend money on. That UI is a later plan and is
  what makes this one worth having.
