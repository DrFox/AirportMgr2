# Road System Slice 1 — Foundations and Junction Solver — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the `RoadNet` plugin's data model and analytic junction solver, proven by automation tests and a debug-drawn junction gallery showing clean fillets and exactly-shared vertices.

**Architecture:** A slot-map graph of nodes and segments addressed by generation-checked handles, with cross-sections supplied by `URoadProfile` data assets. A dependency-free `Solve/` layer computes, per node, the fillet arcs between adjacent road edges, the perpendicular cut distance for each incident segment, and a single closed boundary polygon assembled from those exact cut vertices. Slice 1 renders only via `DrawDebugLine` — no mesh generation, no materials, no build tool.

**Tech Stack:** Unreal Engine 5.8.2, C++20, MSVC 14.51.36231, Unreal Automation Test framework.

**Spec:** `docs/superpowers/specs/2026-08-28-procedural-road-system-design.md`

## Global Constraints

- **Engine:** Unreal Engine 5.8.2 at `D:\Epic\UE_5.8`. Project at `C:\repos\AirportMgr2`.
- **Build settings:** `BuildSettingsVersion.V7`, `EngineIncludeOrderVersion.Unreal5_8`. V7 promotes return-type, dangling-reference and unreachable-code warnings to **errors**.
- **`Solve/` has no engine dependencies** beyond `CoreMinimal.h` / `FVector2D` / `FMath`. No `UObject`, no `AActor`, no `UWorld`, no logging macros that pull in Engine. This is what keeps its tests World-free. Enforced by review, not by tooling.
- **Automation test flags:** UE 5.8 uses `enum class EAutomationTestFlags` with masks as **free constants**. Always write `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter`. The older `EAutomationTestFlags::ApplicationContextMask` form does not compile.
- **Plugin module type for tests** is `"DeveloperTool"`. `"Developer"` has been deprecated since 4.24 and emits a build warning.
- **Units are Unreal centimetres.** A taxiway is 2300 uu wide (23 m), default fillet radius 1500 uu (15 m).
- **`FVector2D` is double-precision** in UE5 (`TVector2<double>`). Do all solver maths in `double`; never narrow to `float`.
- **Naming:** `F` prefix for plain structs, `U` for `UObject` classes, `A` for actors, `E` for enums. Required by UnrealHeaderTool, not stylistic.
- **Every task ends with a commit.** Do not batch commits across tasks.

### Standard commands

Build:

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
```

Regenerate project files (only after adding/removing modules or plugins):

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" -projectfiles `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -game -rocket -progress
```

Run the RoadNet tests headless:

```powershell
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1" -Filter RoadNet.Solve   # a subset
```

The script prints one `PASS`/`FAIL` line per test and **exits non-zero if any test failed or if the filter matched no tests at all**.

**Do not judge a test run by the engine's process exit code.** With `-testexit`, `UnrealEditor-Cmd.exe` exits `0` whether tests pass or fail, so the raw command cannot distinguish them; the script parses `Test Completed. Result={...}` out of the run's log instead. Note the engine writes `Result={Success}`, not `Result={Success}`.

---

## File Structure

```
Plugins/RoadNet/
  RoadNet.uplugin
  Source/RoadNet/
    RoadNet.Build.cs
    Public/
      RoadNetModule.h
      Model/RoadHandles.h          FRoadNodeId, FRoadSegmentId
      Model/RoadSlotMap.h          template algorithms over reflected TArrays
      Model/RoadNode.h             FRoadNode, FRoadSegment
      Model/RoadNetwork.h          URoadNetwork  (Repository)
      Profiles/RoadProfile.h       URoadProfile, FProfileBand, FProfileLane
      Solve/RoadGeom.h             2D primitives + SolveFillet
      Solve/JunctionSolver.h       FJunctionInput, FJunctionResult, FJunctionSolver
      Debug/RoadDebugDraw.h        road.DebugDraw cvar + draw helpers
      Debug/RoadJunctionGallery.h  ARoadJunctionGallery
    Private/
      RoadNetModule.cpp
      Model/RoadNetwork.cpp
      Profiles/RoadProfile.cpp
      Solve/RoadGeom.cpp
      Solve/JunctionSolver.cpp
      Debug/RoadDebugDraw.cpp
      Debug/RoadJunctionGallery.cpp
  Source/RoadNetTests/
    RoadNetTests.Build.cs
    Private/
      RoadNetTestsModule.cpp
      ScaffoldTest.cpp
      SlotMapTest.cpp
      RoadNetworkTest.cpp
      RoadGeomTest.cpp
      FilletTest.cpp
      JunctionCutTest.cpp
      JunctionPolygonTest.cpp
```

`Debug/` is an addition to the spec's §3.1 folder list, holding Slice-1-only visualisation that Slice 2 will not need to modify.

---

### Task 1: Plugin scaffold and green test harness

**Files:**
- Create: `Plugins/RoadNet/RoadNet.uplugin`
- Create: `Plugins/RoadNet/Source/RoadNet/RoadNet.Build.cs`
- Create: `Plugins/RoadNet/Source/RoadNet/Public/RoadNetModule.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/RoadNetModule.cpp`
- Create: `Plugins/RoadNet/Source/RoadNetTests/RoadNetTests.Build.cs`
- Create: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadNetTestsModule.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/ScaffoldTest.cpp`
- Modify: `AirportMgr.uproject`

**Interfaces:**
- Consumes: nothing.
- Produces: modules `RoadNet` (Runtime) and `RoadNetTests` (DeveloperTool). Export macro `ROADNET_API`. Test name prefix `RoadNet.`.

- [ ] **Step 1: Write the plugin descriptor**

`Plugins/RoadNet/RoadNet.uplugin`:

```json
{
	"FileVersion": 3,
	"Version": 1,
	"VersionName": "0.1",
	"FriendlyName": "RoadNet",
	"Description": "Procedural runtime road, taxiway and runway network.",
	"Category": "AirportMgr",
	"CreatedBy": "DrFox",
	"CanContainContent": true,
	"IsBetaVersion": false,
	"Installed": false,
	"Modules": [
		{
			"Name": "RoadNet",
			"Type": "Runtime",
			"LoadingPhase": "Default"
		},
		{
			"Name": "RoadNetTests",
			"Type": "DeveloperTool",
			"LoadingPhase": "Default"
		}
	]
}
```

- [ ] **Step 2: Write the runtime module build rules**

`Plugins/RoadNet/Source/RoadNet/RoadNet.Build.cs`:

```csharp
using UnrealBuildTool;

public class RoadNet : ModuleRules
{
	public RoadNet(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
```

`Engine` is required for `UDataAsset`, `AActor` and `DrawDebugHelpers`. The `Solve/` files must not include anything from it.

- [ ] **Step 3: Write the runtime module implementation**

`Plugins/RoadNet/Source/RoadNet/Public/RoadNetModule.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FRoadNetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
```

`Plugins/RoadNet/Source/RoadNet/Private/RoadNetModule.cpp`:

```cpp
#include "RoadNetModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoadNet, Log, All);

void FRoadNetModule::StartupModule()
{
	UE_LOG(LogRoadNet, Log, TEXT("RoadNet module started."));
}

void FRoadNetModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FRoadNetModule, RoadNet)
```

- [ ] **Step 4: Write the test module build rules and implementation**

`Plugins/RoadNet/Source/RoadNetTests/RoadNetTests.Build.cs`:

```csharp
using UnrealBuildTool;

public class RoadNetTests : ModuleRules
{
	public RoadNetTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RoadNet"
		});
	}
}
```

`Plugins/RoadNet/Source/RoadNetTests/Private/RoadNetTestsModule.cpp`:

```cpp
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, RoadNetTests)
```

- [ ] **Step 5: Write the failing scaffold test**

`Plugins/RoadNet/Source/RoadNetTests/Private/ScaffoldTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNetScaffoldTest,
	"RoadNet.Scaffold.HarnessRuns",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNetScaffoldTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("harness arithmetic"), 2 + 2, 5);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

The deliberately wrong assertion proves the harness actually reports failures. A test harness that has never failed is not known to work.

- [ ] **Step 6: Enable the plugin in the project**

In `AirportMgr.uproject`, replace the `"Plugins"` array with:

```json
	"Plugins": [
		{
			"Name": "ModelingToolsEditorMode",
			"Enabled": true,
			"TargetAllowList": [
				"Editor"
			]
		},
		{
			"Name": "RoadNet",
			"Enabled": true
		}
	]
```

- [ ] **Step 7: Regenerate project files and build**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" -projectfiles `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -game -rocket -progress
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
```

Expected: `Result: Succeeded`, and the log lists `Compile [x64] RoadNetModule.cpp` and `Compile [x64] ScaffoldTest.cpp`.

- [ ] **Step 8: Run the test and verify it FAILS**

Run: `& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"`
Expected: `RoadNet.Scaffold.HarnessRuns` reports `Result={Failed}` with `Expected 4 but it was 5` (or the reverse wording). Non-zero exit code.

- [ ] **Step 9: Correct the assertion**

In `ScaffoldTest.cpp` change the assertion to:

```cpp
	TestEqual(TEXT("harness arithmetic"), 2 + 2, 4);
```

- [ ] **Step 10: Run the test and verify it PASSES**

Expected: `RoadNet.Scaffold.HarnessRuns` reports `Result={Success}`, zero failures, zero exit code.

- [ ] **Step 11: Commit**

```bash
git add Plugins/RoadNet AirportMgr.uproject
git commit -m "feat(roadnet): plugin scaffold with runtime and test modules"
```

---

### Task 2: Handles and slot-map algorithms

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadHandles.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadSlotMap.h`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/SlotMapTest.cpp`

**Interfaces:**
- Consumes: `ROADNET_API` from Task 1.
- Produces:
  - `FRoadNodeId { int32 Index; int32 Generation; bool IsValid() const; }`
  - `FRoadSegmentId` with the same shape.
  - `template<typename TItem, typename THandle> THandle RoadSlot::Add(TArray<TItem>&, TArray<int32>& FreeList, TItem&&)`
  - `template<typename TItem, typename THandle> bool RoadSlot::Remove(TArray<TItem>&, TArray<int32>& FreeList, THandle)`
  - `template<typename TItem, typename THandle> TItem* RoadSlot::Get(TArray<TItem>&, THandle)`
  - `template<typename TItem, typename THandle> const TItem* RoadSlot::Get(const TArray<TItem>&, THandle)`
  - Items must expose `int32 Generation` and `bool bAlive`.

**Why templates for the algorithms but plain `TArray` for the storage:** UnrealHeaderTool cannot reflect templates, so a `TSlotMap<T>` member could never be a `UPROPERTY` and would not serialise or be seen by the garbage collector. Reflecting the *data* as plain `TArray`s while sharing the *algorithms* as free function templates keeps both properties. This is a UE constraint, not a design preference.

- [ ] **Step 1: Write the handles header**

`Public/Model/RoadHandles.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "RoadHandles.generated.h"

/** Generation-checked handle to a node in URoadNetwork. */
USTRUCT()
struct ROADNET_API FRoadNodeId
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() int32 Generation = 0;

	bool IsValid() const { return Index != INDEX_NONE; }

	bool operator==(const FRoadNodeId& Other) const
	{
		return Index == Other.Index && Generation == Other.Generation;
	}
	bool operator!=(const FRoadNodeId& Other) const { return !(*this == Other); }
};

FORCEINLINE uint32 GetTypeHash(const FRoadNodeId& Id)
{
	return HashCombine(::GetTypeHash(Id.Index), ::GetTypeHash(Id.Generation));
}

/** Generation-checked handle to a segment in URoadNetwork. */
USTRUCT()
struct ROADNET_API FRoadSegmentId
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() int32 Generation = 0;

	bool IsValid() const { return Index != INDEX_NONE; }

	bool operator==(const FRoadSegmentId& Other) const
	{
		return Index == Other.Index && Generation == Other.Generation;
	}
	bool operator!=(const FRoadSegmentId& Other) const { return !(*this == Other); }
};

FORCEINLINE uint32 GetTypeHash(const FRoadSegmentId& Id)
{
	return HashCombine(::GetTypeHash(Id.Index), ::GetTypeHash(Id.Generation));
}
```

- [ ] **Step 2: Write the slot-map algorithms header**

`Public/Model/RoadSlotMap.h`:

```cpp
#pragma once

#include "CoreMinimal.h"

/**
 * Slot-map algorithms over plain reflected TArrays.
 * TItem must expose: int32 Generation; bool bAlive;
 */
namespace RoadSlot
{
	template<typename THandle, typename TItem>
	THandle Add(TArray<TItem>& Items, TArray<int32>& FreeList, TItem&& NewItem)
	{
		int32 Index;
		if (FreeList.Num() > 0)
		{
			Index = FreeList.Pop();
			const int32 NextGeneration = Items[Index].Generation;
			Items[Index] = MoveTemp(NewItem);
			Items[Index].Generation = NextGeneration;
		}
		else
		{
			Index = Items.Add(MoveTemp(NewItem));
			Items[Index].Generation = 1;
		}
		Items[Index].bAlive = true;

		THandle Handle;
		Handle.Index = Index;
		Handle.Generation = Items[Index].Generation;
		return Handle;
	}

	template<typename THandle, typename TItem>
	bool IsValid(const TArray<TItem>& Items, THandle Handle)
	{
		return Handle.Index != INDEX_NONE
			&& Items.IsValidIndex(Handle.Index)
			&& Items[Handle.Index].bAlive
			&& Items[Handle.Index].Generation == Handle.Generation;
	}

	template<typename THandle, typename TItem>
	TItem* Get(TArray<TItem>& Items, THandle Handle)
	{
		return IsValid<THandle, TItem>(Items, Handle) ? &Items[Handle.Index] : nullptr;
	}

	template<typename THandle, typename TItem>
	const TItem* Get(const TArray<TItem>& Items, THandle Handle)
	{
		return IsValid<THandle, TItem>(Items, Handle) ? &Items[Handle.Index] : nullptr;
	}

	template<typename THandle, typename TItem>
	bool Remove(TArray<TItem>& Items, TArray<int32>& FreeList, THandle Handle)
	{
		if (!IsValid<THandle, TItem>(Items, Handle))
		{
			return false;
		}
		Items[Handle.Index].bAlive = false;
		++Items[Handle.Index].Generation;
		FreeList.Push(Handle.Index);
		return true;
	}
}
```

- [ ] **Step 3: Write the failing tests**

`Plugins/RoadNet/Source/RoadNetTests/Private/SlotMapTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadHandles.h"
#include "Model/RoadSlotMap.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FTestItem
	{
		int32 Generation = 0;
		bool  bAlive = false;
		int32 Payload = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadSlotMapTest,
	"RoadNet.Model.SlotMap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadSlotMapTest::RunTest(const FString& Parameters)
{
	TArray<FTestItem> Items;
	TArray<int32> FreeList;

	FTestItem A; A.Payload = 10;
	FTestItem B; B.Payload = 20;

	const FRoadNodeId HandleA = RoadSlot::Add<FRoadNodeId>(Items, FreeList, MoveTemp(A));
	const FRoadNodeId HandleB = RoadSlot::Add<FRoadNodeId>(Items, FreeList, MoveTemp(B));

	TestTrue(TEXT("A valid after add"), RoadSlot::IsValid<FRoadNodeId>(Items, HandleA));
	TestEqual(TEXT("A payload"), RoadSlot::Get<FRoadNodeId>(Items, HandleA)->Payload, 10);
	TestEqual(TEXT("B payload"), RoadSlot::Get<FRoadNodeId>(Items, HandleB)->Payload, 20);

	TestTrue(TEXT("remove A"), RoadSlot::Remove<FRoadNodeId>(Items, FreeList, HandleA));
	TestFalse(TEXT("A invalid after remove"), RoadSlot::IsValid<FRoadNodeId>(Items, HandleA));
	TestNull(TEXT("A get returns null"), RoadSlot::Get<FRoadNodeId>(Items, HandleA));
	TestTrue(TEXT("B still valid"), RoadSlot::IsValid<FRoadNodeId>(Items, HandleB));

	TestFalse(TEXT("double remove fails"), RoadSlot::Remove<FRoadNodeId>(Items, FreeList, HandleA));

	// Recycling the slot must NOT resurrect the stale handle.
	FTestItem C; C.Payload = 30;
	const FRoadNodeId HandleC = RoadSlot::Add<FRoadNodeId>(Items, FreeList, MoveTemp(C));
	TestEqual(TEXT("C reuses A's slot"), HandleC.Index, HandleA.Index);
	TestNotEqual(TEXT("C has a new generation"), HandleC.Generation, HandleA.Generation);
	TestFalse(TEXT("stale A still invalid"), RoadSlot::IsValid<FRoadNodeId>(Items, HandleA));
	TestEqual(TEXT("C payload"), RoadSlot::Get<FRoadNodeId>(Items, HandleC)->Payload, 30);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

The stale-handle-after-recycle assertions are the entire reason the generation counter exists; without them the slot map is just an array with holes.

- [ ] **Step 4: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" -projectfiles `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -game -rocket -progress
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
```

Then `& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"`. Expected: `RoadNet.Model.SlotMap` `Result={Success}`.

If `RoadSlot::Add` fails to deduce `TItem`, call it as `RoadSlot::Add<FRoadNodeId, FTestItem>(...)`.

- [ ] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): generation-checked handles and slot-map algorithms"
```

---

### Task 3: URoadProfile

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Profiles/RoadProfile.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Profiles/RoadProfile.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadProfileTest.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `URoadProfile` with `GetTotalWidth()`, `GetHalfWidthLeft()`, `GetHalfWidthRight()`, all returning `double`, and `PreferredFilletRadius` (`double`).
  - `URoadProfile::MakeTransient(double TotalWidth, double FilletRadius)` — a static factory used by tests and the gallery so neither needs content assets.

- [ ] **Step 1: Write the header**

`Public/Profiles/RoadProfile.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RoadProfile.generated.h"

UENUM()
enum class ERoadBandType : uint8
{
	Shoulder,
	Lane,
	Curb
};

UENUM()
enum class ERoadLaneDirection : uint8
{
	Forward,
	Backward,
	Bidirectional
};

/** One lateral band of the cross-section, ordered left to right. */
USTRUCT()
struct ROADNET_API FProfileBand
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) double Width = 0.0;
	UPROPERTY(EditAnywhere) ERoadBandType Type = ERoadBandType::Lane;
	UPROPERTY(EditAnywhere) FName MaterialSlot;
};

/** A navigable lane. Populated in Slice 1, consumed in Slice 4. */
USTRUCT()
struct ROADNET_API FProfileLane
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) double CentreOffset = 0.0;
	UPROPERTY(EditAnywhere) double Width = 0.0;
	UPROPERTY(EditAnywhere) ERoadLaneDirection Direction = ERoadLaneDirection::Bidirectional;
};

/**
 * Shared, immutable cross-section description (Flyweight).
 * A taxiway, runway and service road differ only by their profile asset.
 */
UCLASS()
class ROADNET_API URoadProfile : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere) TArray<FProfileBand> Bands;
	UPROPERTY(EditAnywhere) TArray<FProfileLane> Lanes;

	/** Distance from the leftmost band edge to the centreline. Defaults to half the total width. */
	UPROPERTY(EditAnywhere) double CentrelineOffset = -1.0;

	/** Preferred corner radius in uu. Clamped by geometry at solve time. */
	UPROPERTY(EditAnywhere) double PreferredFilletRadius = 1500.0;

	double GetTotalWidth() const;
	double GetHalfWidthLeft() const;
	double GetHalfWidthRight() const;

	/** Symmetric single-band profile for tests and the debug gallery. */
	static URoadProfile* MakeTransient(double TotalWidth, double FilletRadius);
};
```

- [ ] **Step 2: Write the implementation**

`Private/Profiles/RoadProfile.cpp`:

```cpp
#include "Profiles/RoadProfile.h"

double URoadProfile::GetTotalWidth() const
{
	double Total = 0.0;
	for (const FProfileBand& Band : Bands)
	{
		Total += Band.Width;
	}
	return Total;
}

double URoadProfile::GetHalfWidthLeft() const
{
	const double Total = GetTotalWidth();
	return (CentrelineOffset < 0.0) ? Total * 0.5 : CentrelineOffset;
}

double URoadProfile::GetHalfWidthRight() const
{
	return GetTotalWidth() - GetHalfWidthLeft();
}

URoadProfile* URoadProfile::MakeTransient(double TotalWidth, double FilletRadius)
{
	URoadProfile* Profile = NewObject<URoadProfile>(GetTransientPackage());

	FProfileBand Band;
	Band.Width = TotalWidth;
	Band.Type = ERoadBandType::Lane;
	Profile->Bands.Add(Band);

	FProfileLane Lane;
	Lane.CentreOffset = 0.0;
	Lane.Width = TotalWidth;
	Lane.Direction = ERoadLaneDirection::Bidirectional;
	Profile->Lanes.Add(Lane);

	Profile->CentrelineOffset = -1.0;
	Profile->PreferredFilletRadius = FilletRadius;
	return Profile;
}
```

- [ ] **Step 3: Write the failing tests**

`Plugins/RoadNet/Source/RoadNetTests/Private/RoadProfileTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadProfileTest,
	"RoadNet.Model.Profile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadProfileTest::RunTest(const FString& Parameters)
{
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0);

	TestEqual(TEXT("total width"), Taxiway->GetTotalWidth(), 2300.0);
	TestEqual(TEXT("left half"), Taxiway->GetHalfWidthLeft(), 1150.0);
	TestEqual(TEXT("right half"), Taxiway->GetHalfWidthRight(), 1150.0);
	TestEqual(TEXT("fillet radius"), Taxiway->PreferredFilletRadius, 1500.0);

	// Asymmetric: centreline pushed toward the left edge.
	URoadProfile* Offset = URoadProfile::MakeTransient(2000.0, 1000.0);
	Offset->CentrelineOffset = 500.0;
	TestEqual(TEXT("asymmetric left"), Offset->GetHalfWidthLeft(), 500.0);
	TestEqual(TEXT("asymmetric right"), Offset->GetHalfWidthRight(), 1500.0);

	// Multi-band widths sum.
	URoadProfile* Road = NewObject<URoadProfile>(GetTransientPackage());
	FProfileBand Shoulder; Shoulder.Width = 300.0; Shoulder.Type = ERoadBandType::Shoulder;
	FProfileBand LaneBand; LaneBand.Width = 700.0; LaneBand.Type = ERoadBandType::Lane;
	Road->Bands.Add(Shoulder);
	Road->Bands.Add(LaneBand);
	Road->Bands.Add(LaneBand);
	Road->Bands.Add(Shoulder);
	TestEqual(TEXT("summed width"), Road->GetTotalWidth(), 2000.0);
	TestEqual(TEXT("summed half"), Road->GetHalfWidthLeft(), 1000.0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 4: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

The script exits non-zero if any test failed or if no test matched. Do not judge the
run by the engine's own exit code — it is 0 either way.

Expected: `RoadNet.Model.Profile` `Result={Success}`.

- [ ] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): URoadProfile cross-section flyweight"
```

---

### Task 4: URoadNetwork repository with bearing-sorted incidence

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadNode.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadNetwork.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Model/RoadNetwork.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadNetworkTest.cpp`

**Interfaces:**
- Consumes: `FRoadNodeId`, `FRoadSegmentId`, `RoadSlot::*` (Task 2); `URoadProfile` (Task 3).
- Produces:
  - `FRoadNode { FVector2D Position; TArray<FRoadSegmentId> Incident; int32 Generation; bool bAlive; }`
  - `FRoadSegment { FRoadNodeId A, B; FVector2D Control; TObjectPtr<URoadProfile> Profile; double TrimA, TrimB; int32 Generation; bool bAlive; }`
  - `URoadNetwork::AddNode(const FVector2D&) -> FRoadNodeId`
  - `URoadNetwork::AddSegment(FRoadNodeId, FRoadNodeId, const FVector2D& Control, URoadProfile*) -> FRoadSegmentId`
  - `URoadNetwork::AddStraightSegment(FRoadNodeId, FRoadNodeId, URoadProfile*) -> FRoadSegmentId`
  - `URoadNetwork::RemoveSegment(FRoadSegmentId) -> bool`
  - `URoadNetwork::GetNode/GetSegment` const accessors returning pointers
  - `URoadNetwork::GetOutgoingTangent(FRoadSegmentId, FRoadNodeId) const -> FVector2D` (normalised, points away from the node)
  - `URoadNetwork::GetOtherEnd(FRoadSegmentId, FRoadNodeId) const -> FRoadNodeId`

**Quadratic Bezier tangents.** For `B(t) = (1-t)^2*A + 2(1-t)t*C + t^2*B`, the derivative at `t=0` is `2(C-A)` and at `t=1` is `2(B-C)`. The tangent *pointing away from* a node is therefore `normalize(C - A)` at end A and `normalize(C - B)` at end B — both point toward the control point. A straight segment is simply `C = (A+B)/2`, which makes the tangent `normalize(B - A)` at A with no special case in the code.

- [ ] **Step 1: Write the entity header**

`Public/Model/RoadNode.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "RoadNode.generated.h"

class URoadProfile;

USTRUCT()
struct ROADNET_API FRoadNode
{
	GENERATED_BODY()

	UPROPERTY() FVector2D Position = FVector2D::ZeroVector;

	/** Incident segments, maintained sorted by outgoing bearing, ascending in (-UE_DOUBLE_PI, UE_DOUBLE_PI]. */
	UPROPERTY() TArray<FRoadSegmentId> Incident;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};

USTRUCT()
struct ROADNET_API FRoadSegment
{
	GENERATED_BODY()

	UPROPERTY() FRoadNodeId A;
	UPROPERTY() FRoadNodeId B;

	/** Quadratic Bezier control point. Equals (A+B)/2 for a straight segment. */
	UPROPERTY() FVector2D Control = FVector2D::ZeroVector;

	UPROPERTY() TObjectPtr<URoadProfile> Profile = nullptr;

	/** Written ONLY by FJunctionSolver. Distance from each end at which the segment is cut. */
	UPROPERTY() double TrimA = 0.0;
	UPROPERTY() double TrimB = 0.0;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};
```

- [ ] **Step 2: Write the network header**

`Public/Model/RoadNetwork.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Model/RoadHandles.h"
#include "Model/RoadNode.h"
#include "RoadNetwork.generated.h"

class URoadProfile;

/**
 * Repository owning the road graph. All mutation goes through this type;
 * from Slice 3 onward only IRoadCommand implementations may call the mutators.
 */
UCLASS()
class ROADNET_API URoadNetwork : public UObject
{
	GENERATED_BODY()

public:
	FRoadNodeId AddNode(const FVector2D& Position);
	bool RemoveNode(FRoadNodeId Node);

	FRoadSegmentId AddSegment(FRoadNodeId A, FRoadNodeId B, const FVector2D& Control, URoadProfile* Profile);
	FRoadSegmentId AddStraightSegment(FRoadNodeId A, FRoadNodeId B, URoadProfile* Profile);
	bool RemoveSegment(FRoadSegmentId Segment);

	const FRoadNode*    GetNode(FRoadNodeId Node) const;
	const FRoadSegment* GetSegment(FRoadSegmentId Segment) const;
	FRoadSegment*       GetSegmentMutable(FRoadSegmentId Segment);

	/** Normalised tangent at AtNode, pointing away from that node along the segment. */
	FVector2D GetOutgoingTangent(FRoadSegmentId Segment, FRoadNodeId AtNode) const;

	FRoadNodeId GetOtherEnd(FRoadSegmentId Segment, FRoadNodeId AtNode) const;

	const TArray<FRoadNode>&    GetNodes()    const { return Nodes; }
	const TArray<FRoadSegment>& GetSegments() const { return Segments; }

private:
	void SortIncident(FRoadNodeId Node);

	UPROPERTY() TArray<FRoadNode>    Nodes;
	UPROPERTY() TArray<int32>        NodeFreeList;
	UPROPERTY() TArray<FRoadSegment> Segments;
	UPROPERTY() TArray<int32>        SegmentFreeList;
};
```

- [ ] **Step 3: Write the implementation**

`Private/Model/RoadNetwork.cpp`:

```cpp
#include "Model/RoadNetwork.h"
#include "Model/RoadSlotMap.h"
#include "Profiles/RoadProfile.h"

FRoadNodeId URoadNetwork::AddNode(const FVector2D& Position)
{
	FRoadNode Node;
	Node.Position = Position;
	return RoadSlot::Add<FRoadNodeId>(Nodes, NodeFreeList, MoveTemp(Node));
}

bool URoadNetwork::RemoveNode(FRoadNodeId Node)
{
	const FRoadNode* Existing = RoadSlot::Get<FRoadNodeId>(Nodes, Node);
	if (Existing == nullptr)
	{
		return false;
	}

	// Copy: removing segments mutates the incident array we would otherwise iterate.
	TArray<FRoadSegmentId> ToRemove = Existing->Incident;
	for (const FRoadSegmentId Segment : ToRemove)
	{
		RemoveSegment(Segment);
	}
	return RoadSlot::Remove<FRoadNodeId>(Nodes, NodeFreeList, Node);
}

FRoadSegmentId URoadNetwork::AddSegment(FRoadNodeId A, FRoadNodeId B, const FVector2D& Control, URoadProfile* Profile)
{
	if (!RoadSlot::IsValid<FRoadNodeId>(Nodes, A) || !RoadSlot::IsValid<FRoadNodeId>(Nodes, B) || A == B)
	{
		return FRoadSegmentId();
	}

	FRoadSegment Segment;
	Segment.A = A;
	Segment.B = B;
	Segment.Control = Control;
	Segment.Profile = Profile;

	const FRoadSegmentId Handle = RoadSlot::Add<FRoadSegmentId>(Segments, SegmentFreeList, MoveTemp(Segment));

	RoadSlot::Get<FRoadNodeId>(Nodes, A)->Incident.Add(Handle);
	RoadSlot::Get<FRoadNodeId>(Nodes, B)->Incident.Add(Handle);
	SortIncident(A);
	SortIncident(B);

	return Handle;
}

FRoadSegmentId URoadNetwork::AddStraightSegment(FRoadNodeId A, FRoadNodeId B, URoadProfile* Profile)
{
	const FRoadNode* NodeA = RoadSlot::Get<FRoadNodeId>(Nodes, A);
	const FRoadNode* NodeB = RoadSlot::Get<FRoadNodeId>(Nodes, B);
	if (NodeA == nullptr || NodeB == nullptr)
	{
		return FRoadSegmentId();
	}
	return AddSegment(A, B, (NodeA->Position + NodeB->Position) * 0.5, Profile);
}

bool URoadNetwork::RemoveSegment(FRoadSegmentId Segment)
{
	const FRoadSegment* Existing = RoadSlot::Get<FRoadSegmentId>(Segments, Segment);
	if (Existing == nullptr)
	{
		return false;
	}

	const FRoadNodeId EndA = Existing->A;
	const FRoadNodeId EndB = Existing->B;

	if (FRoadNode* NodeA = RoadSlot::Get<FRoadNodeId>(Nodes, EndA))
	{
		NodeA->Incident.Remove(Segment);
	}
	if (FRoadNode* NodeB = RoadSlot::Get<FRoadNodeId>(Nodes, EndB))
	{
		NodeB->Incident.Remove(Segment);
	}

	return RoadSlot::Remove<FRoadSegmentId>(Segments, SegmentFreeList, Segment);
}

const FRoadNode* URoadNetwork::GetNode(FRoadNodeId Node) const
{
	return RoadSlot::Get<FRoadNodeId>(Nodes, Node);
}

const FRoadSegment* URoadNetwork::GetSegment(FRoadSegmentId Segment) const
{
	return RoadSlot::Get<FRoadSegmentId>(Segments, Segment);
}

FRoadSegment* URoadNetwork::GetSegmentMutable(FRoadSegmentId Segment)
{
	return RoadSlot::Get<FRoadSegmentId>(Segments, Segment);
}

FRoadNodeId URoadNetwork::GetOtherEnd(FRoadSegmentId Segment, FRoadNodeId AtNode) const
{
	const FRoadSegment* Seg = GetSegment(Segment);
	if (Seg == nullptr)
	{
		return FRoadNodeId();
	}
	return (Seg->A == AtNode) ? Seg->B : Seg->A;
}

FVector2D URoadNetwork::GetOutgoingTangent(FRoadSegmentId Segment, FRoadNodeId AtNode) const
{
	const FRoadSegment* Seg = GetSegment(Segment);
	const FRoadNode* Node = GetNode(AtNode);
	if (Seg == nullptr || Node == nullptr)
	{
		return FVector2D(1.0, 0.0);
	}

	// Both ends: the outgoing tangent points toward the control point.
	FVector2D Dir = Seg->Control - Node->Position;

	if (Dir.IsNearlyZero())
	{
		// Degenerate control point; fall back to the straight chord.
		const FRoadNode* Other = GetNode(GetOtherEnd(Segment, AtNode));
		Dir = (Other != nullptr) ? (Other->Position - Node->Position) : FVector2D(1.0, 0.0);
	}

	return Dir.GetSafeNormal();
}

void URoadNetwork::SortIncident(FRoadNodeId NodeId)
{
	FRoadNode* Node = RoadSlot::Get<FRoadNodeId>(Nodes, NodeId);
	if (Node == nullptr)
	{
		return;
	}

	Node->Incident.Sort([this, NodeId](const FRoadSegmentId& L, const FRoadSegmentId& R)
	{
		const FVector2D DirL = GetOutgoingTangent(L, NodeId);
		const FVector2D DirR = GetOutgoingTangent(R, NodeId);
		return FMath::Atan2(DirL.Y, DirL.X) < FMath::Atan2(DirR.Y, DirR.X);
	});
}
```

- [ ] **Step 4: Write the failing tests**

`Plugins/RoadNet/Source/RoadNetTests/Private/RoadNetworkTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNetworkTest,
	"RoadNet.Model.Network",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNetworkTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(2300.0, 1500.0);

	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(10000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 10000.0));
	const FRoadNodeId West   = Net->AddNode(FVector2D(-10000.0, 0.0));

	const FRoadSegmentId ToNorth = Net->AddStraightSegment(Centre, North, Profile);
	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	const FRoadSegmentId ToWest  = Net->AddStraightSegment(Centre, West,  Profile);

	TestTrue(TEXT("segments created"), ToNorth.IsValid() && ToEast.IsValid() && ToWest.IsValid());

	// Outgoing tangents at the centre node.
	const FVector2D TanEast = Net->GetOutgoingTangent(ToEast, Centre);
	TestTrue(TEXT("east tangent"), TanEast.Equals(FVector2D(1.0, 0.0), 1e-6));

	const FVector2D TanNorth = Net->GetOutgoingTangent(ToNorth, Centre);
	TestTrue(TEXT("north tangent"), TanNorth.Equals(FVector2D(0.0, 1.0), 1e-6));

	// Tangent at the far end points back toward the centre.
	const FVector2D TanBack = Net->GetOutgoingTangent(ToEast, East);
	TestTrue(TEXT("reverse tangent"), TanBack.Equals(FVector2D(-1.0, 0.0), 1e-6));

	// Incident list sorted by bearing ascending: east(0), north(UE_DOUBLE_PI/2), west(UE_DOUBLE_PI).
	const FRoadNode* CentreNode = Net->GetNode(Centre);
	TestEqual(TEXT("incident count"), CentreNode->Incident.Num(), 3);
	TestTrue(TEXT("order[0] east"),  CentreNode->Incident[0] == ToEast);
	TestTrue(TEXT("order[1] north"), CentreNode->Incident[1] == ToNorth);
	TestTrue(TEXT("order[2] west"),  CentreNode->Incident[2] == ToWest);

	// Removing a segment updates both endpoints' incident lists.
	TestTrue(TEXT("remove"), Net->RemoveSegment(ToNorth));
	TestEqual(TEXT("incident after remove"), Net->GetNode(Centre)->Incident.Num(), 2);
	TestEqual(TEXT("far node emptied"), Net->GetNode(North)->Incident.Num(), 0);
	TestNull(TEXT("segment gone"), Net->GetSegment(ToNorth));

	// Removing a node cascades to its segments.
	TestTrue(TEXT("remove centre"), Net->RemoveNode(Centre));
	TestNull(TEXT("centre gone"), Net->GetNode(Centre));
	TestNull(TEXT("east segment cascaded"), Net->GetSegment(ToEast));
	TestEqual(TEXT("east node emptied"), Net->GetNode(East)->Incident.Num(), 0);

	// Self-loops and invalid handles are rejected.
	TestFalse(TEXT("self loop rejected"), Net->AddStraightSegment(East, East, Profile).IsValid());
	TestFalse(TEXT("stale handle rejected"), Net->AddStraightSegment(Centre, East, Profile).IsValid());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 5: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

The script exits non-zero if any test failed or if no test matched. Do not judge the
run by the engine's own exit code — it is 0 either way.

Expected: `RoadNet.Model.Network` `Result={Success}`.

- [ ] **Step 6: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): URoadNetwork repository with bearing-sorted incidence"
```

---

### Task 5: 2D geometry primitives

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Solve/RoadGeom.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Solve/RoadGeom.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadGeomTest.cpp`

**Interfaces:**
- Consumes: nothing. **This file must include only `CoreMinimal.h`.**
- Produces:
  - `struct FRay2D { FVector2D Origin; FVector2D Dir; }` — `Dir` always normalised.
  - `RoadGeom::PerpCCW(const FVector2D&) -> FVector2D`
  - `RoadGeom::Bearing(const FVector2D&) -> double`
  - `RoadGeom::CcwAngleBetween(const FVector2D& From, const FVector2D& To) -> double` in `[0, 2*UE_DOUBLE_PI)`
  - `RoadGeom::Rotate(const FVector2D&, double Radians) -> FVector2D`
  - `RoadGeom::LineIntersect(const FRay2D&, const FRay2D&, FVector2D& Out) -> bool`
  - `RoadGeom::PolygonArea(TArrayView<const FVector2D>) -> double` (signed; positive means CCW)
  - `RoadGeom::IsSimplePolygon(TArrayView<const FVector2D>) -> bool`

- [ ] **Step 1: Write the header**

`Public/Solve/RoadGeom.h`:

```cpp
#pragma once

#include "CoreMinimal.h"

/** A 2D ray. Dir is always normalised. */
struct FRay2D
{
	FVector2D Origin = FVector2D::ZeroVector;
	FVector2D Dir    = FVector2D(1.0, 0.0);
};

/**
 * Dependency-free 2D geometry used by the junction solver.
 * Must not gain any dependency beyond CoreMinimal.h.
 */
namespace RoadGeom
{
	/** Counter-clockwise perpendicular: (x,y) -> (-y,x). */
	ROADNET_API FVector2D PerpCCW(const FVector2D& V);

	/** Rotate by Radians counter-clockwise. */
	ROADNET_API FVector2D Rotate(const FVector2D& V, double Radians);

	/** atan2 bearing in (-UE_DOUBLE_PI, UE_DOUBLE_PI]. */
	ROADNET_API double Bearing(const FVector2D& Dir);

	/** CCW angle from From to To, in [0, 2*UE_DOUBLE_PI). */
	ROADNET_API double CcwAngleBetween(const FVector2D& From, const FVector2D& To);

	/** Intersection of the two infinite lines. False if near-parallel. */
	ROADNET_API bool LineIntersect(const FRay2D& A, const FRay2D& B, FVector2D& OutPoint);

	/** Signed area via the shoelace formula. Positive means CCW winding. */
	ROADNET_API double PolygonArea(TArrayView<const FVector2D> Points);

	/** True if no pair of non-adjacent edges intersects. O(n^2); n is tiny here. */
	ROADNET_API bool IsSimplePolygon(TArrayView<const FVector2D> Points);
}
```

- [ ] **Step 2: Write the implementation**

`Private/Solve/RoadGeom.cpp`:

```cpp
#include "Solve/RoadGeom.h"

namespace
{
	constexpr double ParallelEpsilon = 1e-9;

	bool SegmentsIntersect(const FVector2D& P1, const FVector2D& P2,
	                       const FVector2D& Q1, const FVector2D& Q2)
	{
		auto Cross = [](const FVector2D& A, const FVector2D& B)
		{
			return A.X * B.Y - A.Y * B.X;
		};

		const FVector2D R = P2 - P1;
		const FVector2D S = Q2 - Q1;
		const double Denominator = Cross(R, S);
		if (FMath::Abs(Denominator) < ParallelEpsilon)
		{
			return false; // parallel or collinear; treated as non-crossing
		}

		const double T = Cross(Q1 - P1, S) / Denominator;
		const double U = Cross(Q1 - P1, R) / Denominator;
		return T > 0.0 && T < 1.0 && U > 0.0 && U < 1.0;
	}
}

FVector2D RoadGeom::PerpCCW(const FVector2D& V)
{
	return FVector2D(-V.Y, V.X);
}

FVector2D RoadGeom::Rotate(const FVector2D& V, double Radians)
{
	const double C = FMath::Cos(Radians);
	const double S = FMath::Sin(Radians);
	return FVector2D(V.X * C - V.Y * S, V.X * S + V.Y * C);
}

double RoadGeom::Bearing(const FVector2D& Dir)
{
	return FMath::Atan2(Dir.Y, Dir.X);
}

double RoadGeom::CcwAngleBetween(const FVector2D& From, const FVector2D& To)
{
	double Angle = Bearing(To) - Bearing(From);
	while (Angle < 0.0)
	{
		Angle += 2.0 * UE_DOUBLE_PI;
	}
	while (Angle >= 2.0 * UE_DOUBLE_PI)
	{
		Angle -= 2.0 * UE_DOUBLE_PI;
	}
	return Angle;
}

bool RoadGeom::LineIntersect(const FRay2D& A, const FRay2D& B, FVector2D& OutPoint)
{
	const double Denominator = A.Dir.X * B.Dir.Y - A.Dir.Y * B.Dir.X;
	if (FMath::Abs(Denominator) < ParallelEpsilon)
	{
		return false;
	}

	const FVector2D Delta = B.Origin - A.Origin;
	const double T = (Delta.X * B.Dir.Y - Delta.Y * B.Dir.X) / Denominator;
	OutPoint = A.Origin + A.Dir * T;
	return true;
}

double RoadGeom::PolygonArea(TArrayView<const FVector2D> Points)
{
	const int32 Count = Points.Num();
	if (Count < 3)
	{
		return 0.0;
	}

	double Sum = 0.0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector2D& Current = Points[Index];
		const FVector2D& Next = Points[(Index + 1) % Count];
		Sum += Current.X * Next.Y - Next.X * Current.Y;
	}
	return Sum * 0.5;
}

bool RoadGeom::IsSimplePolygon(TArrayView<const FVector2D> Points)
{
	const int32 Count = Points.Num();
	if (Count < 3)
	{
		return false;
	}

	for (int32 I = 0; I < Count; ++I)
	{
		for (int32 J = I + 1; J < Count; ++J)
		{
			const bool bAdjacent = (J == I + 1) || (I == 0 && J == Count - 1);
			if (bAdjacent)
			{
				continue;
			}
			if (SegmentsIntersect(Points[I], Points[(I + 1) % Count],
			                      Points[J], Points[(J + 1) % Count]))
			{
				return false;
			}
		}
	}
	return true;
}
```

- [ ] **Step 3: Write the failing tests**

`Plugins/RoadNet/Source/RoadNetTests/Private/RoadGeomTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadGeomTest,
	"RoadNet.Solve.Geom",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadGeomTest::RunTest(const FString& Parameters)
{
	// PerpCCW
	TestTrue(TEXT("perp of +X is +Y"),
		RoadGeom::PerpCCW(FVector2D(1.0, 0.0)).Equals(FVector2D(0.0, 1.0), 1e-9));

	// Rotate
	TestTrue(TEXT("rotate +X by 90deg"),
		RoadGeom::Rotate(FVector2D(1.0, 0.0), UE_DOUBLE_PI * 0.5).Equals(FVector2D(0.0, 1.0), 1e-9));

	// CcwAngleBetween is always in [0, 2PI)
	TestTrue(TEXT("east to north is 90deg"),
		FMath::IsNearlyEqual(RoadGeom::CcwAngleBetween(FVector2D(1.0, 0.0), FVector2D(0.0, 1.0)), UE_DOUBLE_PI * 0.5, 1e-9));
	TestTrue(TEXT("north to east is 270deg"),
		FMath::IsNearlyEqual(RoadGeom::CcwAngleBetween(FVector2D(0.0, 1.0), FVector2D(1.0, 0.0)), UE_DOUBLE_PI * 1.5, 1e-9));

	// LineIntersect
	FRay2D Horizontal; Horizontal.Origin = FVector2D(0.0, 5.0); Horizontal.Dir = FVector2D(1.0, 0.0);
	FRay2D Vertical;   Vertical.Origin   = FVector2D(7.0, 0.0); Vertical.Dir   = FVector2D(0.0, 1.0);
	FVector2D Hit;
	TestTrue(TEXT("lines cross"), RoadGeom::LineIntersect(Horizontal, Vertical, Hit));
	TestTrue(TEXT("crossing point"), Hit.Equals(FVector2D(7.0, 5.0), 1e-9));

	FRay2D Parallel; Parallel.Origin = FVector2D(0.0, 9.0); Parallel.Dir = FVector2D(1.0, 0.0);
	TestFalse(TEXT("parallel lines do not cross"), RoadGeom::LineIntersect(Horizontal, Parallel, Hit));

	// PolygonArea: CCW unit square has area +1
	const TArray<FVector2D> Square = {
		FVector2D(0.0, 0.0), FVector2D(1.0, 0.0), FVector2D(1.0, 1.0), FVector2D(0.0, 1.0)
	};
	TestTrue(TEXT("ccw square area"), FMath::IsNearlyEqual(RoadGeom::PolygonArea(Square), 1.0, 1e-9));

	TArray<FVector2D> Reversed = Square;
	Algo::Reverse(Reversed);
	TestTrue(TEXT("cw square area negative"), RoadGeom::PolygonArea(Reversed) < 0.0);

	// IsSimplePolygon
	TestTrue(TEXT("square is simple"), RoadGeom::IsSimplePolygon(Square));

	const TArray<FVector2D> Bowtie = {
		FVector2D(0.0, 0.0), FVector2D(1.0, 1.0), FVector2D(1.0, 0.0), FVector2D(0.0, 1.0)
	};
	TestFalse(TEXT("bowtie is not simple"), RoadGeom::IsSimplePolygon(Bowtie));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

Add `#include "Algo/Reverse.h"` to the test file if `Algo::Reverse` does not resolve.

- [ ] **Step 4: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

The script exits non-zero if any test failed or if no test matched. Do not judge the
run by the engine's own exit code — it is 0 either way.

Expected: `RoadNet.Solve.Geom` `Result={Success}`.

- [ ] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): dependency-free 2D geometry primitives"
```

---

### Task 6: SolveFillet — the tangent-arc maths

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Solve/RoadGeom.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Solve/RoadGeom.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/FilletTest.cpp`

**Interfaces:**
- Consumes: `FRay2D`, `RoadGeom::LineIntersect`, `RoadGeom::CcwAngleBetween`, `RoadGeom::Rotate` (Task 5).
- Produces:
  - `struct FFillet { bool bValid; bool bStraightThrough; FVector2D Corner, Centre, TangentA, TangentB; double Distance, Radius, Theta, ParamA, ParamB; }`
  - `RoadGeom::SolveFillet(const FRay2D& A, const FRay2D& B, double Radius) -> FFillet`

**The maths, stated exactly.** `A` is the left edge of the earlier segment in CCW bearing order; `B` is the right edge of the next. Both `Dir` vectors point **away from the node**. Let `X` be the intersection of their infinite lines and `Theta = CcwAngleBetween(A.Dir, B.Dir)`.

```
d      = R / tan(Theta / 2)
T_A    = X - d * A.Dir
T_B    = X - d * B.Dir
C      = X - (R / sin(Theta / 2)) * Rotate(A.Dir, Theta / 2)
ParamA = dot(T_A - A.Origin, A.Dir)
ParamB = dot(T_B - B.Origin, B.Dir)
```

Both tangent points use the **same** `X - d * Dir` form. The formula is uniform across convex and reflex corners because `d` changes sign: at `Theta < UE_DOUBLE_PI` (convex) `d > 0`; at `Theta > UE_DOUBLE_PI` (reflex) `tan(Theta/2) < 0` so `d < 0` and the tangent points move outward from `X` instead of inward. At `Theta == UE_DOUBLE_PI` the segments are collinear, `tan` diverges, `d -> 0`, and there is no corner to round — return `bStraightThrough = true`. **Because R9 auto-subdivides long drags, collinear nodes are the common case, not an edge case.** A solver that rounds them produces visible faceting down every straight run.

**Clamping.** The polygon stays sane only if both tangent points sit at a non-negative parameter along their edges, so that the later cut (`max` over a segment's two corners) lies at or beyond them. Enforce `ParamA >= 0` and `ParamB >= 0` by adjusting `R`:

- Convex (`Theta < UE_DOUBLE_PI`, `d > 0`): `d` must not exceed `min(a_A, a_B)` where `a = dot(X - Origin, Dir)`. Reduce `R` to `min(a_A, a_B) * tan(Theta/2)` when it does.
- Reflex (`Theta > UE_DOUBLE_PI`, `d < 0`): `d` must still satisfy `d <= min(a_A, a_B)`, and since both sides are negative this **raises** the required `R`. Increase `R` to `min(a_A, a_B) * tan(Theta/2)` when needed.

In both cases the corrected value is the same expression, `R = min(a_A, a_B) * tan(Theta/2)`; only the direction of the adjustment differs. Worked check on a 90° two-way bend of half-width `w`: the convex corner has `a_A = a_B = w`, so `R <= w`; the reflex corner has `a_A = a_B = -w` and `tan(135°) = -1`, so `R >= w`. Inner radius capped at the half-width, outer radius floored at it — which is what a road corner should do.

- [ ] **Step 1: Add the declarations to the header**

Append inside `namespace RoadGeom` in `Public/Solve/RoadGeom.h`:

```cpp
	/** Result of rounding one corner between two adjacent road edges. */
	struct FFillet
	{
		/** False when the corner could not be solved at all (parallel, non-intersecting edges). */
		bool bValid = false;

		/** True when the edges are collinear: no arc, join the cuts with a straight line. */
		bool bStraightThrough = false;

		FVector2D Corner   = FVector2D::ZeroVector;  // X
		FVector2D Centre   = FVector2D::ZeroVector;  // C
		FVector2D TangentA = FVector2D::ZeroVector;  // T_A
		FVector2D TangentB = FVector2D::ZeroVector;  // T_B

		double Distance = 0.0;  // d, signed
		double Radius   = 0.0;  // actual radius after clamping
		double Theta    = 0.0;  // CCW angle from A.Dir to B.Dir, [0, 2*UE_DOUBLE_PI)
		double ParamA   = 0.0;  // distance of T_A along A from A.Origin
		double ParamB   = 0.0;  // distance of T_B along B from B.Origin
	};

	/**
	 * Round the corner between edge A (left edge of the earlier segment) and edge B
	 * (right edge of the next segment in CCW bearing order). Both Dir point away
	 * from the node. Radius is clamped so both tangent parameters are non-negative.
	 */
	ROADNET_API FFillet SolveFillet(const FRay2D& A, const FRay2D& B, double Radius);

	/** Sample an arc from TangentA to TangentB about Centre, inclusive of both ends. */
	ROADNET_API void SampleArc(const FFillet& Fillet, int32 SegmentCount, TArray<FVector2D>& OutPoints);
```

- [ ] **Step 2: Add the implementation**

Append to `Private/Solve/RoadGeom.cpp`:

```cpp
RoadGeom::FFillet RoadGeom::SolveFillet(const FRay2D& A, const FRay2D& B, double Radius)
{
	FFillet Result;
	Result.Theta = CcwAngleBetween(A.Dir, B.Dir);

	constexpr double CollinearEpsilon = 1e-6;
	if (FMath::Abs(Result.Theta - UE_DOUBLE_PI) < CollinearEpsilon)
	{
		Result.bValid = true;
		Result.bStraightThrough = true;
		return Result;
	}

	FVector2D Corner;
	if (!LineIntersect(A, B, Corner))
	{
		return Result; // bValid stays false
	}
	Result.Corner = Corner;

	const double HalfTheta = Result.Theta * 0.5;
	const double TanHalf = FMath::Tan(HalfTheta);
	const double SinHalf = FMath::Sin(HalfTheta);

	if (FMath::Abs(TanHalf) < CollinearEpsilon || FMath::Abs(SinHalf) < CollinearEpsilon)
	{
		Result.bValid = true;
		Result.bStraightThrough = true;
		return Result;
	}

	// Clamp the radius so both tangent parameters land at or beyond the edge origins.
	const double ReachA = FVector2D::DotProduct(Corner - A.Origin, A.Dir);
	const double ReachB = FVector2D::DotProduct(Corner - B.Origin, B.Dir);
	const double MaxDistance = FMath::Min(ReachA, ReachB);

	double EffectiveRadius = Radius;
	double Distance = EffectiveRadius / TanHalf;
	if (Distance > MaxDistance)
	{
		EffectiveRadius = MaxDistance * TanHalf;
		Distance = MaxDistance;
	}

	Result.bValid = true;
	Result.Radius = EffectiveRadius;
	Result.Distance = Distance;
	Result.TangentA = Corner - A.Dir * Distance;
	Result.TangentB = Corner - B.Dir * Distance;
	Result.Centre = Corner - Rotate(A.Dir, HalfTheta) * (EffectiveRadius / SinHalf);
	Result.ParamA = FVector2D::DotProduct(Result.TangentA - A.Origin, A.Dir);
	Result.ParamB = FVector2D::DotProduct(Result.TangentB - B.Origin, B.Dir);

	return Result;
}

void RoadGeom::SampleArc(const FFillet& Fillet, int32 SegmentCount, TArray<FVector2D>& OutPoints)
{
	if (!Fillet.bValid || Fillet.bStraightThrough || SegmentCount < 1)
	{
		return;
	}

	const FVector2D FromCentreA = Fillet.TangentA - Fillet.Centre;
	const FVector2D FromCentreB = Fillet.TangentB - Fillet.Centre;

	const double StartAngle = Bearing(FromCentreA);
	double Sweep = Bearing(FromCentreB) - StartAngle;
	while (Sweep > UE_DOUBLE_PI)  { Sweep -= 2.0 * UE_DOUBLE_PI; }
	while (Sweep < -UE_DOUBLE_PI) { Sweep += 2.0 * UE_DOUBLE_PI; }

	const double ArcRadius = FromCentreA.Length();
	for (int32 Step = 0; Step <= SegmentCount; ++Step)
	{
		const double Alpha = static_cast<double>(Step) / static_cast<double>(SegmentCount);
		const double Angle = StartAngle + Sweep * Alpha;
		OutPoints.Add(Fillet.Centre + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * ArcRadius);
	}
}
```

- [ ] **Step 3: Write the failing tests**

`Plugins/RoadNet/Source/RoadNetTests/Private/FilletTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadFilletTest,
	"RoadNet.Solve.Fillet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadFilletTest::RunTest(const FString& Parameters)
{
	constexpr double W = 1150.0;  // taxiway half-width

	// --- Convex corner: east segment's left edge vs north segment's right edge ---
	// East heads +X, its left edge is the line y = +W.
	// North heads +Y, its right edge is the line x = +W.
	FRay2D EastLeft;   EastLeft.Origin   = FVector2D(0.0, W); EastLeft.Dir   = FVector2D(1.0, 0.0);
	FRay2D NorthRight; NorthRight.Origin = FVector2D(W, 0.0); NorthRight.Dir = FVector2D(0.0, 1.0);

	{
		const RoadGeom::FFillet Inner = RoadGeom::SolveFillet(EastLeft, NorthRight, 500.0);
		TestTrue(TEXT("inner valid"), Inner.bValid);
		TestFalse(TEXT("inner not straight"), Inner.bStraightThrough);
		TestTrue(TEXT("inner theta 90deg"), FMath::IsNearlyEqual(Inner.Theta, UE_DOUBLE_PI * 0.5, 1e-9));
		TestTrue(TEXT("inner corner at (W,W)"), Inner.Corner.Equals(FVector2D(W, W), 1e-6));
		TestTrue(TEXT("inner d equals R"), FMath::IsNearlyEqual(Inner.Distance, 500.0, 1e-6));
		TestTrue(TEXT("inner tangent A"), Inner.TangentA.Equals(FVector2D(W - 500.0, W), 1e-6));
		TestTrue(TEXT("inner tangent B"), Inner.TangentB.Equals(FVector2D(W, W - 500.0), 1e-6));
		TestTrue(TEXT("inner centre"), Inner.Centre.Equals(FVector2D(W - 500.0, W - 500.0), 1e-6));
		TestTrue(TEXT("inner paramA non-negative"), Inner.ParamA >= -1e-9);
		TestTrue(TEXT("inner paramB non-negative"), Inner.ParamB >= -1e-9);

		// Centre must be exactly R from each tangent point.
		TestTrue(TEXT("inner radius to A"),
			FMath::IsNearlyEqual((Inner.TangentA - Inner.Centre).Length(), Inner.Radius, 1e-6));
		TestTrue(TEXT("inner radius to B"),
			FMath::IsNearlyEqual((Inner.TangentB - Inner.Centre).Length(), Inner.Radius, 1e-6));
	}

	// Convex clamping: asking for more than the half-width must reduce the radius to W.
	{
		const RoadGeom::FFillet Clamped = RoadGeom::SolveFillet(EastLeft, NorthRight, 5000.0);
		TestTrue(TEXT("convex radius clamped down"), FMath::IsNearlyEqual(Clamped.Radius, W, 1e-6));
		TestTrue(TEXT("clamped paramA is zero"), FMath::IsNearlyEqual(Clamped.ParamA, 0.0, 1e-6));
	}

	// --- Reflex corner: north segment's left edge vs east segment's right edge ---
	// North heads +Y, its left edge is the line x = -W.
	// East heads +X, its right edge is the line y = -W.
	FRay2D NorthLeft; NorthLeft.Origin = FVector2D(-W, 0.0); NorthLeft.Dir = FVector2D(0.0, 1.0);
	FRay2D EastRight; EastRight.Origin = FVector2D(0.0, -W); EastRight.Dir = FVector2D(1.0, 0.0);

	{
		const RoadGeom::FFillet Outer = RoadGeom::SolveFillet(NorthLeft, EastRight, 3000.0);
		TestTrue(TEXT("outer valid"), Outer.bValid);
		TestTrue(TEXT("outer theta 270deg"), FMath::IsNearlyEqual(Outer.Theta, UE_DOUBLE_PI * 1.5, 1e-9));
		TestTrue(TEXT("outer corner at (-W,-W)"), Outer.Corner.Equals(FVector2D(-W, -W), 1e-6));
		TestTrue(TEXT("outer d is negative"), Outer.Distance < 0.0);
		TestTrue(TEXT("outer tangent A"), Outer.TangentA.Equals(FVector2D(-W, -W + 3000.0), 1e-6));
		TestTrue(TEXT("outer tangent B"), Outer.TangentB.Equals(FVector2D(-W + 3000.0, -W), 1e-6));
		TestTrue(TEXT("outer centre"), Outer.Centre.Equals(FVector2D(-W + 3000.0, -W + 3000.0), 1e-6));
		TestTrue(TEXT("outer paramA non-negative"), Outer.ParamA >= -1e-9);
		TestTrue(TEXT("outer paramB non-negative"), Outer.ParamB >= -1e-9);
	}

	// Reflex clamping: asking for less than the half-width must raise the radius to W.
	{
		const RoadGeom::FFillet Raised = RoadGeom::SolveFillet(NorthLeft, EastRight, 100.0);
		TestTrue(TEXT("reflex radius raised up"), FMath::IsNearlyEqual(Raised.Radius, W, 1e-6));
		TestTrue(TEXT("raised paramA is zero"), FMath::IsNearlyEqual(Raised.ParamA, 0.0, 1e-6));
	}

	// --- Collinear: the common case once long drags auto-subdivide ---
	{
		FRay2D WestLeft;  WestLeft.Origin  = FVector2D(0.0, -W); WestLeft.Dir  = FVector2D(-1.0, 0.0);
		const RoadGeom::FFillet Straight = RoadGeom::SolveFillet(EastLeft, WestLeft, 1500.0);
		TestTrue(TEXT("collinear valid"), Straight.bValid);
		TestTrue(TEXT("collinear is straight-through"), Straight.bStraightThrough);
	}

	// --- Arc sampling starts and ends exactly on the tangent points ---
	{
		const RoadGeom::FFillet Inner = RoadGeom::SolveFillet(EastLeft, NorthRight, 500.0);
		TArray<FVector2D> Arc;
		RoadGeom::SampleArc(Inner, 8, Arc);
		TestEqual(TEXT("arc point count"), Arc.Num(), 9);
		TestTrue(TEXT("arc starts at tangent A"), Arc[0].Equals(Inner.TangentA, 1e-6));
		TestTrue(TEXT("arc ends at tangent B"), Arc.Last().Equals(Inner.TangentB, 1e-6));
		for (const FVector2D& Point : Arc)
		{
			TestTrue(TEXT("arc point is on the circle"),
				FMath::IsNearlyEqual((Point - Inner.Centre).Length(), Inner.Radius, 1e-6));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 4: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

The script exits non-zero if any test failed or if no test matched. Do not judge the
run by the engine's own exit code — it is 0 either way.

Expected: `RoadNet.Solve.Fillet` `Result={Success}`.

If the reflex-corner assertions fail on the sign of `Distance` or the position of `Centre`, the likely cause is `CcwAngleBetween` returning the clockwise angle: verify with the `RoadNet.Solve.Geom` case asserting north-to-east is 270°, and check that the arguments are passed as (earlier segment's LEFT edge, next segment's RIGHT edge) in that order.

- [ ] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): tangent-arc fillet solve with convex and reflex clamping"
```

---

### Task 7: Junction solver — cut distances and cut vertices

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Solve/JunctionSolver.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Solve/JunctionSolver.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/JunctionCutTest.cpp`

**Interfaces:**
- Consumes: `FRay2D`, `RoadGeom::SolveFillet`, `RoadGeom::PerpCCW` (Tasks 5–6).
- Produces:
  - `struct FJunctionArm { FVector2D Tangent; double HalfWidthLeft, HalfWidthRight, FilletRadius; int32 UserData; }`
  - `struct FJunctionInput { FVector2D Position; TArray<FJunctionArm> Arms; }` — **arms must already be sorted by CCW bearing**
  - `struct FJunctionArmResult { double CutDistance; FVector2D LeftCut, RightCut; }`
  - `struct FJunctionResult { bool bValid; TArray<FJunctionArmResult> Arms; TArray<RoadGeom::FFillet> Corners; TArray<FVector2D> Boundary; TArray<int32> Triangles; }`
  - `FJunctionSolver::SolveCuts(const FJunctionInput&) -> FJunctionResult` (fills `Arms` and `Corners` only; `Boundary`/`Triangles` come in Task 8)

**Cut rule.** Corner `i` lies between arm `i`'s **left** edge and arm `i+1`'s **right** edge. Arm `i`'s cut distance is therefore `max(Corner[i].ParamA, Corner[i-1].ParamB, 0)` — the larger of the two tangent parameters its two adjacent corners produced. Given Task 6 guarantees both parameters are non-negative, the cut always lies at or beyond every tangent point on that arm, which is what makes Task 8's boundary walk have non-negative-length straight runs.

Cut vertices, with `N = PerpCCW(Tangent)`:

```
Centre    = Position + Tangent * CutDistance
LeftCut   = Centre + N * HalfWidthLeft
RightCut  = Centre - N * HalfWidthRight
```

- [ ] **Step 1: Write the header**

`Public/Solve/JunctionSolver.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Solve/RoadGeom.h"

/** One segment arriving at a junction. */
struct FJunctionArm
{
	/** Normalised, pointing away from the node. */
	FVector2D Tangent = FVector2D(1.0, 0.0);

	double HalfWidthLeft  = 0.0;
	double HalfWidthRight = 0.0;
	double FilletRadius   = 0.0;

	/** Opaque caller tag, e.g. a packed FRoadSegmentId index. Never read by the solver. */
	int32 UserData = INDEX_NONE;
};

struct FJunctionInput
{
	FVector2D Position = FVector2D::ZeroVector;

	/** MUST be sorted ascending by CCW bearing of Tangent. */
	TArray<FJunctionArm> Arms;

	/** Points per fillet arc. */
	int32 ArcSegments = 8;
};

struct FJunctionArmResult
{
	double    CutDistance = 0.0;
	FVector2D LeftCut  = FVector2D::ZeroVector;
	FVector2D RightCut = FVector2D::ZeroVector;
};

struct FJunctionResult
{
	bool bValid = false;

	/** Parallel to FJunctionInput::Arms. */
	TArray<FJunctionArmResult> Arms;

	/** Corner i lies between arm i's left edge and arm (i+1)'s right edge. */
	TArray<RoadGeom::FFillet> Corners;

	/** Closed CCW boundary polygon. Filled by SolveBoundary. */
	TArray<FVector2D> Boundary;

	/** Triangle fan indices into Boundary, with the centre appended as the last vertex. */
	TArray<int32> Triangles;

	/** Fan centre, appended to Boundary by SolveBoundary. */
	FVector2D Centre = FVector2D::ZeroVector;
};

/**
 * Owns the boundary of a node. Segments never compute where they stop:
 * SolveCuts writes the cut distance and both cut vertices, and SolveBoundary
 * assembles the junction polygon from those exact same values.
 */
class ROADNET_API FJunctionSolver
{
public:
	static FJunctionResult SolveCuts(const FJunctionInput& Input);
	static void SolveBoundary(const FJunctionInput& Input, FJunctionResult& InOutResult);

	/** Left edge of an arm, as a ray originating at the node. */
	static FRay2D MakeLeftEdge(const FJunctionInput& Input, int32 ArmIndex);

	/** Right edge of an arm, as a ray originating at the node. */
	static FRay2D MakeRightEdge(const FJunctionInput& Input, int32 ArmIndex);
};
```

- [ ] **Step 2: Write the implementation**

`Private/Solve/JunctionSolver.cpp`:

```cpp
#include "Solve/JunctionSolver.h"

FRay2D FJunctionSolver::MakeLeftEdge(const FJunctionInput& Input, int32 ArmIndex)
{
	const FJunctionArm& Arm = Input.Arms[ArmIndex];
	FRay2D Ray;
	Ray.Dir = Arm.Tangent;
	Ray.Origin = Input.Position + RoadGeom::PerpCCW(Arm.Tangent) * Arm.HalfWidthLeft;
	return Ray;
}

FRay2D FJunctionSolver::MakeRightEdge(const FJunctionInput& Input, int32 ArmIndex)
{
	const FJunctionArm& Arm = Input.Arms[ArmIndex];
	FRay2D Ray;
	Ray.Dir = Arm.Tangent;
	Ray.Origin = Input.Position - RoadGeom::PerpCCW(Arm.Tangent) * Arm.HalfWidthRight;
	return Ray;
}

FJunctionResult FJunctionSolver::SolveCuts(const FJunctionInput& Input)
{
	FJunctionResult Result;

	const int32 ArmCount = Input.Arms.Num();
	if (ArmCount == 0)
	{
		return Result;
	}

	Result.Arms.SetNum(ArmCount);

	if (ArmCount == 1)
	{
		// Dead end: cut back by the arm's own half-width so the cap has room.
		const FJunctionArm& Arm = Input.Arms[0];
		Result.Arms[0].CutDistance = FMath::Max(Arm.HalfWidthLeft, Arm.HalfWidthRight);
		Result.bValid = true;
	}
	else
	{
		Result.Corners.SetNum(ArmCount);

		for (int32 Index = 0; Index < ArmCount; ++Index)
		{
			const int32 NextIndex = (Index + 1) % ArmCount;
			const FRay2D LeftEdge  = MakeLeftEdge(Input, Index);
			const FRay2D RightEdge = MakeRightEdge(Input, NextIndex);

			const double Radius = FMath::Min(Input.Arms[Index].FilletRadius,
			                                 Input.Arms[NextIndex].FilletRadius);

			Result.Corners[Index] = RoadGeom::SolveFillet(LeftEdge, RightEdge, Radius);
			if (!Result.Corners[Index].bValid)
			{
				return Result; // bValid stays false
			}
		}

		// Arm i's cut is the larger of the two tangent parameters on its own edges:
		// corner i supplied ParamA on arm i's LEFT edge,
		// corner i-1 supplied ParamB on arm i's RIGHT edge.
		for (int32 Index = 0; Index < ArmCount; ++Index)
		{
			const int32 PrevIndex = (Index + ArmCount - 1) % ArmCount;

			const double FromLeft  = Result.Corners[Index].bStraightThrough
				? 0.0 : Result.Corners[Index].ParamA;
			const double FromRight = Result.Corners[PrevIndex].bStraightThrough
				? 0.0 : Result.Corners[PrevIndex].ParamB;

			Result.Arms[Index].CutDistance = FMath::Max3(FromLeft, FromRight, 0.0);
		}

		Result.bValid = true;
	}

	// Cut vertices, computed once and shared with the boundary walk.
	for (int32 Index = 0; Index < ArmCount; ++Index)
	{
		const FJunctionArm& Arm = Input.Arms[Index];
		const FVector2D Normal = RoadGeom::PerpCCW(Arm.Tangent);
		const FVector2D CutCentre = Input.Position + Arm.Tangent * Result.Arms[Index].CutDistance;

		Result.Arms[Index].LeftCut  = CutCentre + Normal * Arm.HalfWidthLeft;
		Result.Arms[Index].RightCut = CutCentre - Normal * Arm.HalfWidthRight;
	}

	return Result;
}
```

`SolveBoundary` is added in Task 8; declare it now and leave it unimplemented only if the linker permits. To keep this task independently buildable, add a temporary empty definition:

```cpp
void FJunctionSolver::SolveBoundary(const FJunctionInput& Input, FJunctionResult& InOutResult)
{
	// Implemented in Task 8.
}
```

- [ ] **Step 3: Write the failing tests**

`Plugins/RoadNet/Source/RoadNetTests/Private/JunctionCutTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/JunctionSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FJunctionArm MakeArm(const FVector2D& Tangent, double HalfWidth, double Radius)
	{
		FJunctionArm Arm;
		Arm.Tangent = Tangent.GetSafeNormal();
		Arm.HalfWidthLeft = HalfWidth;
		Arm.HalfWidthRight = HalfWidth;
		Arm.FilletRadius = Radius;
		return Arm;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadJunctionCutTest,
	"RoadNet.Solve.JunctionCuts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadJunctionCutTest::RunTest(const FString& Parameters)
{
	constexpr double W = 1150.0;

	// --- Four-way, equal widths, arms sorted CCW from east ---
	{
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.Arms.Add(MakeArm(FVector2D( 1.0,  0.0), W, 1500.0));  // east   0
		Input.Arms.Add(MakeArm(FVector2D( 0.0,  1.0), W, 1500.0));  // north  90
		Input.Arms.Add(MakeArm(FVector2D(-1.0,  0.0), W, 1500.0));  // west   180
		Input.Arms.Add(MakeArm(FVector2D( 0.0, -1.0), W, 1500.0));  // south  270

		const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		TestTrue(TEXT("4-way solves"), Result.bValid);
		TestEqual(TEXT("4-way arm results"), Result.Arms.Num(), 4);
		TestEqual(TEXT("4-way corners"), Result.Corners.Num(), 4);

		// All four corners are convex 90deg, radius clamped to W, so every cut is W.
		for (int32 Index = 0; Index < 4; ++Index)
		{
			TestTrue(TEXT("4-way cut equals half-width"),
				FMath::IsNearlyEqual(Result.Arms[Index].CutDistance, W, 1e-6));
			TestTrue(TEXT("4-way cut is non-negative"), Result.Arms[Index].CutDistance >= 0.0);
		}

		// East arm cut vertices: cut centre (W,0), left (W,+W), right (W,-W).
		TestTrue(TEXT("east left cut"),  Result.Arms[0].LeftCut.Equals(FVector2D(W,  W), 1e-6));
		TestTrue(TEXT("east right cut"), Result.Arms[0].RightCut.Equals(FVector2D(W, -W), 1e-6));
	}

	// --- Straight-through node: the common case after R9 auto-subdivision ---
	{
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.Arms.Add(MakeArm(FVector2D( 1.0, 0.0), W, 1500.0));  // east
		Input.Arms.Add(MakeArm(FVector2D(-1.0, 0.0), W, 1500.0));  // west

		const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		TestTrue(TEXT("collinear solves"), Result.bValid);
		TestTrue(TEXT("corner 0 straight"), Result.Corners[0].bStraightThrough);
		TestTrue(TEXT("corner 1 straight"), Result.Corners[1].bStraightThrough);

		// No corner to round, so nothing is trimmed: a straight run must not facet.
		TestTrue(TEXT("east cut is zero"), FMath::IsNearlyEqual(Result.Arms[0].CutDistance, 0.0, 1e-6));
		TestTrue(TEXT("west cut is zero"), FMath::IsNearlyEqual(Result.Arms[1].CutDistance, 0.0, 1e-6));
	}

	// --- Dead end ---
	{
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.Arms.Add(MakeArm(FVector2D(1.0, 0.0), W, 1500.0));

		const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		TestTrue(TEXT("dead end solves"), Result.bValid);
		TestEqual(TEXT("dead end arms"), Result.Arms.Num(), 1);
		TestTrue(TEXT("dead end cut"), FMath::IsNearlyEqual(Result.Arms[0].CutDistance, W, 1e-6));
	}

	// --- Mixed widths: a narrow taxiway meeting a wide runway (R10) ---
	{
		constexpr double RunwayHalf = 2250.0;   // 45 m runway
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.Arms.Add(MakeArm(FVector2D( 1.0,  0.0), RunwayHalf, 3000.0));  // runway east
		Input.Arms.Add(MakeArm(FVector2D( 0.0,  1.0), W,          1500.0));  // taxiway north
		Input.Arms.Add(MakeArm(FVector2D(-1.0,  0.0), RunwayHalf, 3000.0));  // runway west

		const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		TestTrue(TEXT("mixed width solves"), Result.bValid);
		for (const FJunctionArmResult& Arm : Result.Arms)
		{
			TestTrue(TEXT("mixed cut non-negative"), Arm.CutDistance >= -1e-9);
			TestTrue(TEXT("mixed cut finite"), FMath::IsFinite(Arm.CutDistance));
		}
	}

	// --- Acute 15deg fork, 3-way and 5-way stay finite and non-negative ---
	{
		const TArray<TArray<double>> BearingSets = {
			{ 0.0, UE_DOUBLE_PI * (15.0 / 180.0), UE_DOUBLE_PI },                                 // acute fork
			{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI * 1.25 },                                     // 3-way Y
			{ 0.0, UE_DOUBLE_PI * 0.4, UE_DOUBLE_PI * 0.8, UE_DOUBLE_PI * 1.2, UE_DOUBLE_PI * 1.6 }                   // 5-way
		};

		for (const TArray<double>& Bearings : BearingSets)
		{
			FJunctionInput Input;
			Input.Position = FVector2D::ZeroVector;
			for (const double Bearing : Bearings)
			{
				Input.Arms.Add(MakeArm(FVector2D(FMath::Cos(Bearing), FMath::Sin(Bearing)), W, 1500.0));
			}

			const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
			TestTrue(TEXT("configuration solves"), Result.bValid);
			for (const FJunctionArmResult& Arm : Result.Arms)
			{
				TestTrue(TEXT("cut non-negative"), Arm.CutDistance >= -1e-9);
				TestTrue(TEXT("cut finite"), FMath::IsFinite(Arm.CutDistance));
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 4: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

The script exits non-zero if any test failed or if no test matched. Do not judge the
run by the engine's own exit code — it is 0 either way.

Expected: `RoadNet.Solve.JunctionCuts` `Result={Success}`.

- [ ] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): junction cut distances and shared cut vertices"
```

---

### Task 8: Junction boundary polygon and fan triangulation

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Solve/JunctionSolver.cpp` (replace the stub `SolveBoundary`)
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/JunctionPolygonTest.cpp`

**Interfaces:**
- Consumes: `FJunctionResult` from Task 7, `RoadGeom::SampleArc` from Task 6.
- Produces: `FJunctionResult::Boundary`, `::Triangles`, `::Centre` populated.

**Boundary walk, CCW.** For each arm `i` in order: emit `RightCut[i]`, then `LeftCut[i]` (crossing the segment's cut line), then corner `i`'s connection to arm `i+1`. The connection is:

- If `Corners[i].bStraightThrough`: nothing — `LeftCut[i]` joins directly to `RightCut[i+1]`.
- Otherwise: the straight run from `LeftCut[i]` back to `TangentA`, the sampled arc from `TangentA` to `TangentB`, then the straight run from `TangentB` forward to `RightCut[i+1]`. Emitting the arc samples alone suffices, since the straight runs are implied by the polyline connecting `LeftCut[i]` to the first arc point and the last arc point to `RightCut[i+1]`.

Both straight runs have non-negative length precisely because Task 7 set each cut to the `max` of its two adjacent tangent parameters.

- [ ] **Step 1: Replace the stub implementation**

In `Private/Solve/JunctionSolver.cpp`, replace the temporary `SolveBoundary` with:

```cpp
void FJunctionSolver::SolveBoundary(const FJunctionInput& Input, FJunctionResult& InOutResult)
{
	InOutResult.Boundary.Reset();
	InOutResult.Triangles.Reset();

	if (!InOutResult.bValid)
	{
		return;
	}

	const int32 ArmCount = Input.Arms.Num();

	for (int32 Index = 0; Index < ArmCount; ++Index)
	{
		// The segment's cut line, traversed right-to-left so the interior stays on the left.
		InOutResult.Boundary.Add(InOutResult.Arms[Index].RightCut);
		InOutResult.Boundary.Add(InOutResult.Arms[Index].LeftCut);

		if (ArmCount == 1)
		{
			continue; // dead end: the cap closes through the fan centre
		}

		const RoadGeom::FFillet& Corner = InOutResult.Corners[Index];
		if (!Corner.bStraightThrough)
		{
			RoadGeom::SampleArc(Corner, Input.ArcSegments, InOutResult.Boundary);
		}
	}

	// Fan centre is appended last so Boundary indices stay stable for callers.
	InOutResult.Centre = Input.Position;
	const int32 CentreIndex = InOutResult.Boundary.Add(Input.Position);

	const int32 RimCount = CentreIndex; // every vertex before the centre
	if (RimCount < 3)
	{
		// A dead end contributes only two cut vertices: there is no fan to build.
		// Proper end-cap geometry is Slice 2's mesh-builder concern; the boundary
		// is still populated so debug draw can show the cut.
		return;
	}

	InOutResult.Triangles.Reserve(RimCount * 3);
	for (int32 Index = 0; Index < RimCount; ++Index)
	{
		InOutResult.Triangles.Add(CentreIndex);
		InOutResult.Triangles.Add(Index);
		InOutResult.Triangles.Add((Index + 1) % RimCount);
	}
}
```

- [ ] **Step 2: Write the failing tests**

`Plugins/RoadNet/Source/RoadNetTests/Private/JunctionPolygonTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/JunctionSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FJunctionArm MakePolyArm(double Bearing, double HalfWidth, double Radius)
	{
		FJunctionArm Arm;
		Arm.Tangent = FVector2D(FMath::Cos(Bearing), FMath::Sin(Bearing));
		Arm.HalfWidthLeft = HalfWidth;
		Arm.HalfWidthRight = HalfWidth;
		Arm.FilletRadius = Radius;
		return Arm;
	}

	FJunctionResult SolveFull(const FJunctionInput& Input)
	{
		FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		FJunctionSolver::SolveBoundary(Input, Result);
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadJunctionPolygonTest,
	"RoadNet.Solve.JunctionPolygon",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadJunctionPolygonTest::RunTest(const FString& Parameters)
{
	constexpr double W = 1150.0;

	// The gallery: every configuration the solver must survive.
	const TArray<TArray<double>> Gallery = {
		{ 0.0, UE_DOUBLE_PI * (15.0 / 180.0) },          // 2-way, 15 deg
		{ 0.0, UE_DOUBLE_PI * 0.25 },                    // 2-way, 45 deg
		{ 0.0, UE_DOUBLE_PI * 0.5 },                     // 2-way, 90 deg
		{ 0.0, UE_DOUBLE_PI * (170.0 / 180.0) },         // 2-way, 170 deg
		{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI },                 // 3-way T
		{ 0.0, UE_DOUBLE_PI * 0.6667, UE_DOUBLE_PI * 1.3333 },     // 3-way Y
		{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI, UE_DOUBLE_PI * 1.5 },       // 4-way
		{ 0.0, UE_DOUBLE_PI * 0.4, UE_DOUBLE_PI * 0.8, UE_DOUBLE_PI * 1.2, UE_DOUBLE_PI * 1.6 }  // 5-way
	};

	for (int32 CaseIndex = 0; CaseIndex < Gallery.Num(); ++CaseIndex)
	{
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.ArcSegments = 8;
		for (const double Bearing : Gallery[CaseIndex])
		{
			Input.Arms.Add(MakePolyArm(Bearing, W, 1500.0));
		}

		const FJunctionResult Result = SolveFull(Input);
		const FString Label = FString::Printf(TEXT("case %d"), CaseIndex);

		TestTrue(*(Label + TEXT(" solves")), Result.bValid);
		TestTrue(*(Label + TEXT(" has boundary")), Result.Boundary.Num() >= 4);

		// The rim excludes the appended fan centre.
		TArray<FVector2D> Rim = Result.Boundary;
		Rim.Pop();

		TestTrue(*(Label + TEXT(" winding is CCW")), RoadGeom::PolygonArea(Rim) > 0.0);
		TestTrue(*(Label + TEXT(" polygon is simple")), RoadGeom::IsSimplePolygon(Rim));

		// Triangle indices are all in range and reference the centre.
		TestEqual(*(Label + TEXT(" triangle index count")), Result.Triangles.Num() % 3, 0);
		for (const int32 VertexIndex : Result.Triangles)
		{
			TestTrue(*(Label + TEXT(" index in range")),
				VertexIndex >= 0 && VertexIndex < Result.Boundary.Num());
		}

		// THE CONTRACT: every arm's cut vertices appear in the boundary bit-for-bit.
		for (int32 ArmIndex = 0; ArmIndex < Result.Arms.Num(); ++ArmIndex)
		{
			bool bFoundLeft = false;
			bool bFoundRight = false;
			for (const FVector2D& Point : Result.Boundary)
			{
				// Exact equality, not Equals(). Shared vertices must be identical values.
				if (Point.X == Result.Arms[ArmIndex].LeftCut.X && Point.Y == Result.Arms[ArmIndex].LeftCut.Y)
				{
					bFoundLeft = true;
				}
				if (Point.X == Result.Arms[ArmIndex].RightCut.X && Point.Y == Result.Arms[ArmIndex].RightCut.Y)
				{
					bFoundRight = true;
				}
			}
			TestTrue(*(Label + TEXT(" left cut shared exactly")), bFoundLeft);
			TestTrue(*(Label + TEXT(" right cut shared exactly")), bFoundRight);
		}
	}

	// Continuity: sweeping one arm's bearing must not make the boundary jump.
	{
		double PreviousArea = -1.0;
		for (int32 Step = 1; Step < 360; ++Step)
		{
			const double Bearing = UE_DOUBLE_PI * 2.0 * static_cast<double>(Step) / 360.0;
			if (FMath::Abs(Bearing - UE_DOUBLE_PI) < 0.02)
			{
				continue; // the collinear case is a legitimate discontinuity in topology
			}

			FJunctionInput Input;
			Input.Position = FVector2D::ZeroVector;
			Input.Arms.Add(MakePolyArm(0.0, W, 1500.0));
			Input.Arms.Add(MakePolyArm(Bearing, W, 1500.0));

			const FJunctionResult Result = SolveFull(Input);
			TestTrue(TEXT("sweep solves"), Result.bValid);

			TArray<FVector2D> Rim = Result.Boundary;
			Rim.Pop();
			const double Area = RoadGeom::PolygonArea(Rim);
			TestTrue(TEXT("sweep winding CCW"), Area > 0.0);

			if (PreviousArea > 0.0)
			{
				const double Change = FMath::Abs(Area - PreviousArea) / PreviousArea;
				TestTrue(TEXT("sweep area changes continuously"), Change < 0.25);
			}
			PreviousArea = Area;
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

The exact-equality check on cut vertices is the whole point of Slice 1. If it ever needs a tolerance, the shared-truth contract has been broken somewhere and the seams will come back.

- [ ] **Step 3: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

The script exits non-zero if any test failed or if no test matched. Do not judge the
run by the engine's own exit code — it is 0 either way.

Expected: `RoadNet.Solve.JunctionPolygon` `Result={Success}`.

If the continuity sweep fails near a particular bearing, log the offending bearing and inspect that single case with `road.DebugDraw` after Task 9 rather than loosening the threshold.

- [ ] **Step 4: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): junction boundary polygon and fan triangulation"
```

---

### Task 9: Debug draw and the junction gallery

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Debug/RoadDebugDraw.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Debug/RoadDebugDraw.cpp`
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Debug/RoadJunctionGallery.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Debug/RoadJunctionGallery.cpp`

**Interfaces:**
- Consumes: `URoadNetwork`, `URoadProfile`, `FJunctionSolver`, `FJunctionResult`.
- Produces:
  - cvar `road.DebugDraw` (`int32`, default `1`): `0` off, `1` boundary + cuts, `2` adds solver internals (edge rays, corner points, arc centres).
  - `ARoadJunctionGallery` — a placeable actor that builds the gallery network and draws it every tick.

- [ ] **Step 1: Write the debug draw header**

`Public/Debug/RoadDebugDraw.h`:

```cpp
#pragma once

#include "CoreMinimal.h"

struct FJunctionInput;
struct FJunctionResult;
class UWorld;

namespace RoadDebug
{
	/** 0 = off, 1 = boundary and cuts, 2 = adds solver internals. */
	ROADNET_API int32 GetDebugDrawLevel();

	ROADNET_API void DrawJunction(UWorld* World, const FJunctionInput& Input, const FJunctionResult& Result, double ZHeight);
}
```

- [ ] **Step 2: Write the debug draw implementation**

`Private/Debug/RoadDebugDraw.cpp`:

```cpp
#include "Debug/RoadDebugDraw.h"

#include "DrawDebugHelpers.h"
#include "Solve/JunctionSolver.h"

namespace
{
	TAutoConsoleVariable<int32> CVarRoadDebugDraw(
		TEXT("road.DebugDraw"),
		1,
		TEXT("RoadNet debug drawing. 0 = off, 1 = boundary and cuts, 2 = adds solver internals."),
		ECVF_Default);

	FVector To3D(const FVector2D& Point, double Z)
	{
		return FVector(Point.X, Point.Y, Z);
	}
}

int32 RoadDebug::GetDebugDrawLevel()
{
	return CVarRoadDebugDraw.GetValueOnGameThread();
}

void RoadDebug::DrawJunction(UWorld* World, const FJunctionInput& Input, const FJunctionResult& Result, double ZHeight)
{
	const int32 Level = GetDebugDrawLevel();
	if (World == nullptr || Level <= 0 || !Result.bValid || Result.Boundary.Num() < 3)
	{
		return;
	}

	// Rim excludes the appended fan centre.
	const int32 RimCount = Result.Boundary.Num() - 1;

	// Boundary polygon in green.
	for (int32 Index = 0; Index < RimCount; ++Index)
	{
		const FVector Start = To3D(Result.Boundary[Index], ZHeight);
		const FVector End   = To3D(Result.Boundary[(Index + 1) % RimCount], ZHeight);
		DrawDebugLine(World, Start, End, FColor::Green, false, -1.0f, 0, 6.0f);
	}

	// Cut lines in cyan, cut vertices as spheres.
	for (const FJunctionArmResult& Arm : Result.Arms)
	{
		DrawDebugLine(World, To3D(Arm.RightCut, ZHeight), To3D(Arm.LeftCut, ZHeight),
			FColor::Cyan, false, -1.0f, 0, 10.0f);
		DrawDebugSphere(World, To3D(Arm.LeftCut,  ZHeight), 40.0f, 8, FColor::Cyan, false, -1.0f, 0, 2.0f);
		DrawDebugSphere(World, To3D(Arm.RightCut, ZHeight), 40.0f, 8, FColor::Cyan, false, -1.0f, 0, 2.0f);
	}

	if (Level < 2)
	{
		return;
	}

	// Solver internals.
	for (int32 Index = 0; Index < Input.Arms.Num(); ++Index)
	{
		const FRay2D LeftEdge  = FJunctionSolver::MakeLeftEdge(Input, Index);
		const FRay2D RightEdge = FJunctionSolver::MakeRightEdge(Input, Index);
		const double Extent = 6000.0;

		DrawDebugLine(World,
			To3D(LeftEdge.Origin, ZHeight),
			To3D(LeftEdge.Origin + LeftEdge.Dir * Extent, ZHeight),
			FColor::Yellow, false, -1.0f, 0, 2.0f);
		DrawDebugLine(World,
			To3D(RightEdge.Origin, ZHeight),
			To3D(RightEdge.Origin + RightEdge.Dir * Extent, ZHeight),
			FColor::Orange, false, -1.0f, 0, 2.0f);
	}

	for (const RoadGeom::FFillet& Corner : Result.Corners)
	{
		if (!Corner.bValid || Corner.bStraightThrough)
		{
			continue;
		}
		DrawDebugSphere(World, To3D(Corner.Corner,   ZHeight), 60.0f, 8, FColor::Red,     false, -1.0f, 0, 2.0f);
		DrawDebugSphere(World, To3D(Corner.Centre,   ZHeight), 50.0f, 8, FColor::Magenta, false, -1.0f, 0, 2.0f);
		DrawDebugSphere(World, To3D(Corner.TangentA, ZHeight), 35.0f, 8, FColor::White,   false, -1.0f, 0, 2.0f);
		DrawDebugSphere(World, To3D(Corner.TangentB, ZHeight), 35.0f, 8, FColor::White,   false, -1.0f, 0, 2.0f);
	}
}
```

- [ ] **Step 3: Write the gallery actor header**

`Public/Debug/RoadJunctionGallery.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RoadJunctionGallery.generated.h"

class URoadNetwork;
class URoadProfile;

/**
 * Slice 1 visual regression harness: builds every junction configuration the
 * solver must survive, side by side, and debug-draws the solved boundaries.
 */
UCLASS()
class ROADNET_API ARoadJunctionGallery : public AActor
{
	GENERATED_BODY()

public:
	ARoadJunctionGallery();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Spacing between gallery cells, in uu. */
	UPROPERTY(EditAnywhere, Category = "RoadNet") double CellSpacing = 16000.0;

	/** Arm length within each cell, in uu. */
	UPROPERTY(EditAnywhere, Category = "RoadNet") double ArmLength = 6000.0;

	UPROPERTY(EditAnywhere, Category = "RoadNet") double TaxiwayWidth = 2300.0;
	UPROPERTY(EditAnywhere, Category = "RoadNet") double FilletRadius = 1500.0;

private:
	void BuildGallery();

	UPROPERTY() TObjectPtr<URoadNetwork> Network;
	UPROPERTY() TObjectPtr<URoadProfile> Profile;

	/** Centre node of each gallery cell. */
	TArray<FVector2D> CellCentres;
	TArray<TArray<double>> CellBearings;
};
```

- [ ] **Step 4: Write the gallery actor implementation**

`Private/Debug/RoadJunctionGallery.cpp`:

```cpp
#include "Debug/RoadJunctionGallery.h"

#include "Debug/RoadDebugDraw.h"
#include "DrawDebugHelpers.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/JunctionSolver.h"

ARoadJunctionGallery::ARoadJunctionGallery()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ARoadJunctionGallery::BeginPlay()
{
	Super::BeginPlay();
	BuildGallery();
}

void ARoadJunctionGallery::BuildGallery()
{
	Network = NewObject<URoadNetwork>(this);
	Profile = URoadProfile::MakeTransient(TaxiwayWidth, FilletRadius);

	CellBearings = {
		{ 0.0, UE_DOUBLE_PI * (15.0 / 180.0) },
		{ 0.0, UE_DOUBLE_PI * 0.25 },
		{ 0.0, UE_DOUBLE_PI * 0.5 },
		{ 0.0, UE_DOUBLE_PI * (170.0 / 180.0) },
		{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI },
		{ 0.0, UE_DOUBLE_PI * 0.6667, UE_DOUBLE_PI * 1.3333 },
		{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI, UE_DOUBLE_PI * 1.5 },
		{ 0.0, UE_DOUBLE_PI * 0.4, UE_DOUBLE_PI * 0.8, UE_DOUBLE_PI * 1.2, UE_DOUBLE_PI * 1.6 }
	};

	CellCentres.Reset();
	constexpr int32 Columns = 4;
	for (int32 Index = 0; Index < CellBearings.Num(); ++Index)
	{
		const int32 Column = Index % Columns;
		const int32 Row = Index / Columns;
		CellCentres.Add(FVector2D(Column * CellSpacing, Row * CellSpacing));
	}
}

void ARoadJunctionGallery::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (Network == nullptr || RoadDebug::GetDebugDrawLevel() <= 0)
	{
		return;
	}

	const double ZHeight = GetActorLocation().Z + 10.0;

	for (int32 CellIndex = 0; CellIndex < CellCentres.Num(); ++CellIndex)
	{
		FJunctionInput Input;
		Input.Position = CellCentres[CellIndex];
		Input.ArcSegments = 12;

		for (const double Bearing : CellBearings[CellIndex])
		{
			FJunctionArm Arm;
			Arm.Tangent = FVector2D(FMath::Cos(Bearing), FMath::Sin(Bearing));
			Arm.HalfWidthLeft = Profile->GetHalfWidthLeft();
			Arm.HalfWidthRight = Profile->GetHalfWidthRight();
			Arm.FilletRadius = Profile->PreferredFilletRadius;
			Input.Arms.Add(Arm);
		}

		FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		FJunctionSolver::SolveBoundary(Input, Result);

		RoadDebug::DrawJunction(GetWorld(), Input, Result, ZHeight);

		// Draw each arm's ribbon edges from its cut out to ArmLength, in blue.
		for (int32 ArmIndex = 0; ArmIndex < Input.Arms.Num(); ++ArmIndex)
		{
			const FJunctionArm& Arm = Input.Arms[ArmIndex];
			const FVector2D Normal = FVector2D(-Arm.Tangent.Y, Arm.Tangent.X);
			const FVector2D FarCentre = Input.Position + Arm.Tangent * ArmLength;

			const FVector2D FarLeft  = FarCentre + Normal * Arm.HalfWidthLeft;
			const FVector2D FarRight = FarCentre - Normal * Arm.HalfWidthRight;

			auto ToWorld = [ZHeight](const FVector2D& P) { return FVector(P.X, P.Y, ZHeight); };

			DrawDebugLine(GetWorld(), ToWorld(Result.Arms[ArmIndex].LeftCut),  ToWorld(FarLeft),
				FColor::Blue, false, -1.0f, 0, 5.0f);
			DrawDebugLine(GetWorld(), ToWorld(Result.Arms[ArmIndex].RightCut), ToWorld(FarRight),
				FColor::Blue, false, -1.0f, 0, 5.0f);
			DrawDebugLine(GetWorld(), ToWorld(FarLeft), ToWorld(FarRight),
				FColor::Blue, false, -1.0f, 0, 3.0f);
		}
	}
}
```

`DrawDebugHelpers.h` is included directly because `RoadDebugDraw.h` only forward-declares; it does not pull the drawing API through.

- [ ] **Step 5: Build**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
```

Expected: `Result: Succeeded`.

- [ ] **Step 6: Verify the gallery visually — the Slice 1 exit criterion**

1. Open the project in the editor.
2. Create or open a level with a flat floor and place one `ARoadJunctionGallery` actor at the origin.
3. Press Play. Look straight down from a high camera.
4. Confirm, for all eight cells:
   - Green boundary polygons show **rounded corners**, not mitres.
   - Every blue arm edge meets a cyan cut vertex with **no gap and no overlap**.
   - The 15° cell does not self-intersect.
   - The 5-way cell is closed and convex-ish, with no inverted lobes.
5. In the console run `road.DebugDraw 2` and confirm the yellow/orange edge rays, red corner points, magenta arc centres and white tangent points sit where the maths says they should.
6. Run `road.DebugDraw 0` and confirm all drawing stops.

- [ ] **Step 7: Run the full test suite once more**

```powershell
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

Expected: all of `RoadNet.Scaffold.*`, `RoadNet.Model.*`, `RoadNet.Solve.*` report `Result={Success}`, zero failures.

- [ ] **Step 8: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): road.DebugDraw cvar and junction gallery harness"
```

---

## Self-Review

**Spec coverage (Slice 1 rows only):**

| Spec section | Covered by |
|---|---|
| §3.1 module layout (plugin, runtime + test modules) | Task 1 |
| §3.4 handles not references | Task 2 |
| §4.1 slot map with generation counters | Task 2 |
| §4.2 `FRoadNode`, `FRoadSegment`, `TrimA`/`TrimB` written only by the solver | Task 4 |
| §4.3 `URoadProfile` flyweight, `min(radiusA, radiusB)` | Tasks 3, 7 |
| §4.4 bearing-sorted incidence | Task 4 |
| §5.1 bearings | Tasks 4, 7 |
| §5.2 facing edge rays | Task 7 (`MakeLeftEdge` / `MakeRightEdge`) |
| §5.3 tangent arc | Task 6 |
| §5.4 clamping | Task 6 |
| §5.5 segment/junction contract | Tasks 7, 8 (exact-equality test) |
| §5.6 polygon assembly | Task 8 |
| §5.7 fan triangulation | Task 8 |
| §5.8 turn paths | **Slice 4 — deliberately out of scope** |
| §8 tiers 1–2 (property and golden tests) | Tasks 5–8 |
| §8 tier 5 visual regression, junction gallery | Task 9 |
| §8 `road.DebugDraw` cvar | Task 9 |

Not covered, by design: §4.4 Observer change events (no consumer until Slice 2), §6 all mesh generation, §7 the build tool, §8 tiers 3–4 (command and snap tests — those types do not exist until Slice 3).

**Known gaps to watch during execution:**

1. `FRoadSegment::TrimA`/`TrimB` are declared in Task 4 but nothing writes them until Slice 2 wires `URoadNetwork` to `FJunctionSolver`. Slice 1 deliberately keeps the solver free of the graph so it stays engine-independent; the adapter that walks the network, builds `FJunctionInput` per node and writes the trims back is Slice 2's first task.
2. Star-shapedness is asserted indirectly (simple polygon plus CCW winding plus in-range fan indices) rather than directly. If a real configuration ever produces a non-star-shaped boundary, the fan will visibly fold and the ear-clipping fallback named in §5.7 becomes necessary.
3. The continuity sweep in Task 8 uses a 25 % area-change threshold, chosen to catch topology flips rather than to police smoothness. Tighten it only with a specific failure in hand.
