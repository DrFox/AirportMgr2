# Milestone 1: Foundation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the `AirportOps` plugin with a sim clock, an event bus, save/load, a definition catalog, and the Airside seam (agent phase events, redirect/retire primitives, capability query, time scale) so milestone 2 and 3 have something to build on.

**Architecture:** New runtime plugin `AirportOps` depending on `Airside`, never the reverse. `Model/` classes are plain `UObject`s built with `NewObject` and tested without a world. `Present/` holds the composition root `UOpsRuntime` (a UObject, so it is testable) and a thin `UGameInstanceSubsystem` forwarder that ticks it. Airside gains only what the seam needs: agent ids, two delegates, two agent primitives, a capability summary, and a time-scale knob on the actor.

**Tech Stack:** UE 5.8.2 C++, UnrealBuildTool, automation tests (`IMPLEMENT_SIMPLE_AUTOMATION_TEST`), `Tools/Run-AirsideTests.ps1`, `Tools/Check-Architecture.ps1`.

**Spec:** `docs/superpowers/specs/2026-09-05-game-systems-map-design.md` §2.0–§2.4, §5.4. Three amendments to that spec are made by Task 10 (see there).

## Global Constraints

- Dependency direction: `AirportMgr → AirportOps → Airside`. Airside never includes anything from AirportOps.
- `Model/` never includes `Present/` or `Tool/` (both plugins). `Solve/` includes only `CoreMinimal.h` and `Solve/`.
- One log category per name per module (unity build). AirportOps uses `LogAirportOps`, declared once in `Public/AirportOpsLog.h`.
- Every `UE_LOG` in Airside survives. Count before and after each Airside task; the count may rise, never fall.
- Every new model field is a `UPROPERTY()`. Fields that must not be saved are `UPROPERTY(Transient)`. Scheduler callbacks are plain C++ members and are deliberately not saved (Task 2 explains why in the header).
- Comments say WHY and name rejected alternatives, matching the existing codebase density.
- Tests assert behaviour with a named reason string.
- **The editor must be closed for every build in this plan** (new headers, new classes, new UPROPERTYs). Build line:
  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
  ```
- Test line: `./Tools/Run-AirsideTests.ps1` (default filter becomes `Airside+AirportOps` in Task 1). Read its `N test(s) run, N failed, N crashed` line; crashed must be 0. Narrow with `-Filter AirportOps.Model`.
- Commit messages: concise, no `Co-Authored-By` trailer (user rule).
- Branch: `feature/m1-foundation`.

---

## File map

**New plugin `Plugins/AirportOps/`**

| File | Responsibility |
|---|---|
| `AirportOps.uplugin` | Two modules: `AirportOps` (Runtime), `AirportOpsTests` (DeveloperTool) |
| `Source/AirportOps/AirportOps.Build.cs` | Deps: Core, CoreUObject, Engine, DeveloperSettings, Airside |
| `Source/AirportOps/Public/AirportOpsLog.h` / `Private/AirportOpsModule.cpp` | Module impl, the one `LogAirportOps` definition |
| `Source/AirportOps/Public/Model/SimClock.h` / `.cpp` | Game time, speed table, scheduler |
| `Source/AirportOps/Public/Model/OpsEvents.h` / `.cpp` | Multicast bus, one `Notify*` per event, each also logs |
| `Source/AirportOps/Public/Model/OpsSave.h` / `.cpp` | `FOpsSnapshot`, capture/restore, `UOpsSaveGame` slot wrapper |
| `Source/AirportOps/Public/Model/OpsDefinition.h` / `.cpp` | `UOpsDefinition` base, `UScenario` |
| `Source/AirportOps/Public/Model/OpsCatalog.h` / `.cpp` | Registry of loaded definitions, Asset Manager loader |
| `Source/AirportOps/Public/Content/AirportOpsSettings.h` / `.cpp` | `UDeveloperSettings` with `DefaultScenario` |
| `Source/AirportOps/Public/Present/OpsRuntime.h` / `.cpp` | Composition root: owns clock, events, catalog; attaches to `ARoadNetworkActor`; forwards Airside delegates; save/load |
| `Source/AirportOps/Public/Present/OpsRuntimeSubsystem.h` / `.cpp` | `UGameInstanceSubsystem` + `FTickableGameObject` that owns one `UOpsRuntime` and ticks it |
| `Source/AirportOpsTests/AirportOpsTests.Build.cs`, `Private/AirportOpsTestsModule.cpp` | Test module |
| `Source/AirportOpsTests/Private/*Test.cpp` | One file per system |

**Airside changes**

| File | Change |
|---|---|
| `Public/Present/AirsideTraffic.h` / `Private/Present/AirsideTraffic.cpp` | `FAgentSlot::Id`, `NextAgentId`, `OnAgentPhaseChanged`, `OnArrivalRefused`, `GetNewestAgentId`, `RedirectAgent`, `RetireAgent`, phase-change detection in `Advance` |
| `Public/Present/RoadNetworkActor.h` / `.cpp` | `GetTraffic()`, `SimTimeScale` + setter, `Tick` scales delta |
| `Public/Model/RoadEntity.h` | `FEntityInstance::DesignWingspan` |
| `Public/Model/RoadNetwork.h` / `Private/Model/RoadNetwork.cpp` | `PlaceEntity(..., double DesignWingspan = 0.0)` |
| `Private/Present/RoadEditFacadeSurfaces.cpp` | `PlaceStand` passes the design wingspan |
| `Public/Model/AirsideCapability.h` / `Private/Model/AirsideCapability.cpp` | New: `FAirsideCapability`, `AirsideCapability::Summarise` |
| `AirsideTests/Private/ArrivalDispatchTest.cpp` | Records phase events |
| `AirsideTests/Private/AgentEventsTest.cpp`, `AgentRedirectTest.cpp`, `AirsideCapabilityTest.cpp`, `SimTimeScaleTest.cpp` | New |

**Game module and tooling**

| File | Change |
|---|---|
| `Source/AirportMgr/AirportMgr.Build.cs` | Add `AirportOps` |
| `Source/AirportMgr/RoadBuildController.h` / `.cpp` | Keys: Comma slower, Period faster, P pause, F5 save, F9 load |
| `Source/AirportMgr/RoadBuildHUD.cpp` | One clock line under the tool name |
| `AirportMgr.uproject` | Enable `AirportOps` |
| `Config/DefaultGame.ini` | Asset Manager scan for `Scenario` |
| `Tools/Run-AirsideTests.ps1` | Default filter `Airside+AirportOps` |
| `Tools/Check-Architecture.ps1` | AirportOps layer rules, cross-plugin rule, log categories per module |

---

### Task 1: AirportOps plugin skeleton, guards, test runner

**Files:**
- Create: `Plugins/AirportOps/AirportOps.uplugin`
- Create: `Plugins/AirportOps/Source/AirportOps/AirportOps.Build.cs`
- Create: `Plugins/AirportOps/Source/AirportOps/Public/AirportOpsLog.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/AirportOpsModule.cpp`
- Create: `Plugins/AirportOps/Source/AirportOpsTests/AirportOpsTests.Build.cs`
- Create: `Plugins/AirportOps/Source/AirportOpsTests/Private/AirportOpsTestsModule.cpp`
- Create: `Plugins/AirportOps/Source/AirportOpsTests/Private/ModuleLoadsTest.cpp`
- Modify: `AirportMgr.uproject` (plugins array)
- Modify: `Source/AirportMgr/AirportMgr.Build.cs:12`
- Modify: `Tools/Run-AirsideTests.ps1:16`
- Modify: `Tools/Check-Architecture.ps1:40-41, 48-62, 77-88, 119`

**Interfaces:**
- Produces: module `AirportOps` with API macro `AIRPORTOPS_API`; log category `LogAirportOps`; test prefix `AirportOps.`.

- [ ] **Step 1: Create the branch**

```bash
git checkout main && git pull && git checkout -b feature/m1-foundation
```

- [ ] **Step 2: Write the plugin descriptor**

`Plugins/AirportOps/AirportOps.uplugin`:
```json
{
	"FileVersion": 3,
	"Version": 1,
	"VersionName": "0.1",
	"FriendlyName": "AirportOps",
	"Description": "Airport operations and economy: clock, flights, jobs, finance. Sits above Airside.",
	"Category": "AirportMgr",
	"CreatedBy": "DrFox",
	"CanContainContent": true,
	"IsBetaVersion": false,
	"Installed": false,
	"Modules": [
		{ "Name": "AirportOps", "Type": "Runtime", "LoadingPhase": "Default" },
		{ "Name": "AirportOpsTests", "Type": "DeveloperTool", "LoadingPhase": "Default" }
	],
	"Plugins": [
		{ "Name": "Airside", "Enabled": true }
	]
}
```

- [ ] **Step 3: Write the build rules**

`Plugins/AirportOps/Source/AirportOps/AirportOps.Build.cs`:
```csharp
using UnrealBuildTool;

public class AirportOps : ModuleRules
{
	public AirportOps(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Airside is a PUBLIC dependency: this module's headers name Airside types
		// (EAgentPhase, EArrivalRefusal, URoadNetwork) in their own signatures, so anything
		// that includes AirportOps must be able to see Airside too. The direction is one-way
		// by rule - Check-Architecture.ps1 fails the build if Airside ever includes us.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",  // UAirportOpsSettings
			"Airside"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
```

`Plugins/AirportOps/Source/AirportOpsTests/AirportOpsTests.Build.cs`:
```csharp
using UnrealBuildTool;

public class AirportOpsTests : ModuleRules
{
	public AirportOpsTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Airside",
			"AirportOps"
		});
	}
}
```

- [ ] **Step 4: Write the module and log category**

`Plugins/AirportOps/Source/AirportOps/Public/AirportOpsLog.h`:
```cpp
#pragma once

#include "CoreMinimal.h"

/**
 * The one log category for the AirportOps module. DECLARED here, DEFINED once in
 * AirportOpsModule.cpp. The module is a unity build, so a second DEFINE_LOG_CATEGORY_STATIC
 * of the same name in another .cpp compiles alone and collides when the files are stitched -
 * Airside learned this the hard way, and Check-Architecture.ps1 now fails it.
 */
AIRPORTOPS_API DECLARE_LOG_CATEGORY_EXTERN(LogAirportOps, Log, All);
```

`Plugins/AirportOps/Source/AirportOps/Private/AirportOpsModule.cpp`:
```cpp
#include "AirportOpsLog.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogAirportOps);

IMPLEMENT_MODULE(FDefaultModuleImpl, AirportOps)
```

`Plugins/AirportOps/Source/AirportOpsTests/Private/AirportOpsTestsModule.cpp`:
```cpp
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, AirportOpsTests)
```

- [ ] **Step 5: Write the smoke test**

`Plugins/AirportOps/Source/AirportOpsTests/Private/ModuleLoadsTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirportOpsModuleLoadsTest,
	"AirportOps.Module.Loads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirportOpsModuleLoadsTest::RunTest(const FString& Parameters)
{
	// The filter 'AirportOps' must match at least one test or Run-AirsideTests.ps1 reports
	// "no tests matched" and fails the run. This is that test, and it also proves the
	// plugin is enabled in the .uproject - a disabled plugin's modules never load.
	TestTrue(TEXT("AirportOps runtime module is loaded when its tests run"),
		FModuleManager::Get().IsModuleLoaded(TEXT("AirportOps")));
	return true;
}

#endif
```

- [ ] **Step 6: Enable the plugin and add the game dependency**

In `AirportMgr.uproject`, after the `Airside` entry:
```json
    {
      "Name": "AirportOps",
      "Enabled": true
    },
```

In `Source/AirportMgr/AirportMgr.Build.cs` replace the `PublicDependencyModuleNames` line with:
```csharp
		// Airside: the game module drives the road facade. AirportOps: the game module drives
		// the sim clock and save/load. Both dependencies run this way only - neither plugin
		// ever depends on the game, and AirportOps depends on Airside, never the reverse.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "Airside", "AirportOps" });
```

- [ ] **Step 7: Widen the test runner's default filter**

`Tools/Run-AirsideTests.ps1:16`: change `[string] $Filter  = 'Airside',` to
```powershell
    # Both plugins by default. '+' is the automation runner's own list separator
    # (AutomationCommandline.cpp splits RunTests arguments on it), so one cold editor
    # start covers both suites.
    [string] $Filter  = 'Airside+AirportOps',
```
Also update the `.SYNOPSIS`/`.EXAMPLE` block: add `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model`.

- [ ] **Step 8: Extend Check-Architecture.ps1**

Replace lines 40-41:
```powershell
$plugin  = Join-Path $Root 'Plugins\Airside\Source\Airside'
$ops     = Join-Path $Root 'Plugins\AirportOps\Source\AirportOps'
$modules = @($plugin, $ops)
$trees   = @((Join-Path $Root 'Plugins\Airside\Source'), (Join-Path $Root 'Plugins\AirportOps\Source'), (Join-Path $Root 'Source\AirportMgr'))
```

Replace the include-direction block (section 1, the `$forbidden` loop) so it runs per module:
```powershell
# --- 1. Include direction -----------------------------------------------------------------
# Layer -> regex of forbidden include prefixes, applied inside EACH module. Solve/ is
# handled separately as an allow-list.
$forbidden = @{
    'Model' = 'Build/|Tool/|Present/|Entities/'
    'Tool'  = 'Present/'
    'Build' = 'Present/|Tool/'
}
foreach ($module in $modules) {
    foreach ($layer in $forbidden.Keys) {
        foreach ($half in 'Public', 'Private') {
            $dir = Join-Path $module (Join-Path $half $layer)
            foreach ($file in Get-Sources $dir @('.h', '.cpp')) {
                $hits = Select-String -Path $file.FullName -Pattern ('#include\s+"(' + $forbidden[$layer] + ')')
                foreach ($h in $hits) {
                    $failures.Add("include-direction: $($file.FullName):$($h.LineNumber) $layer/ must not include $($h.Line.Trim())")
                }
            }
        }
    }
}

# --- 1b. Cross-plugin direction: Airside never includes AirportOps ------------------------
# AirportOps -> Airside is the only legal direction. A header from the ops plugin inside
# Airside would make movement depend on money, which is the boundary the plugin split exists
# to hold. Matched on the include path AND on the API macro, because a forward-declared
# AIRPORTOPS_API type is the same leak with no #include to catch.
foreach ($file in Get-Sources (Join-Path $Root 'Plugins\Airside\Source') @('.h', '.cpp')) {
    $hits = Select-String -Path $file.FullName -Pattern 'AirportOps|AIRPORTOPS_API'
    foreach ($h in $hits) {
        $failures.Add("cross-plugin: $($file.FullName):$($h.LineNumber) Airside must not reference AirportOps: $($h.Line.Trim())")
    }
}
```

In section 2 (log categories), change `foreach ($file in Get-Sources $plugin @('.cpp', '.h'))` to loop per module, resetting the table each time, and fix the message:
```powershell
foreach ($module in $modules) {
    $categories = @{}
    foreach ($file in Get-Sources $module @('.cpp', '.h')) {
        $hits = Select-String -Path $file.FullName -Pattern 'DEFINE_LOG_CATEGORY(_STATIC)?\(\s*(\w+)'
        foreach ($h in $hits) {
            $name = $h.Matches[0].Groups[2].Value
            if ($categories.ContainsKey($name)) {
                $failures.Add("log-category: $name defined in both $($categories[$name]) and $($file.FullName):$($h.LineNumber) - unity build collision; declare it once in the module's Public/*Log.h")
            }
            else {
                $categories[$name] = "$($file.FullName):$($h.LineNumber)"
            }
        }
    }
}
```

Section 4 (Piper fallback): the `-match '\\AirsideTests\\'` skip becomes `-match '\\(AirsideTests|AirportOpsTests)\\'`.

Line 119 PASS message: `'Check-Architecture: PASS (include direction, cross-plugin, log categories, doc comments, content default)'`.

- [ ] **Step 9: Run the architecture check**

```powershell
./Tools/Check-Architecture.ps1
```
Expected: `Check-Architecture: PASS (...)`.

- [ ] **Step 10: Build (editor closed)**

Run the build line from Global Constraints. Expected: `Result: Succeeded`. If it reports "Unable to build while Live Coding is active", the editor is open; ask the user to close it.

- [ ] **Step 11: Run the tests**

```powershell
./Tools/Run-AirsideTests.ps1 -Filter AirportOps
```
Expected: `1 test(s) run, 0 failed, 0 crashed.` and `AirportOps.Module.Loads` in the completed list. Then run the full default to confirm Airside still passes: `./Tools/Run-AirsideTests.ps1`, expected `0 failed, 0 crashed`.

- [ ] **Step 12: Commit**

```bash
git add Plugins/AirportOps AirportMgr.uproject Source/AirportMgr/AirportMgr.Build.cs Tools/
git commit -m "feat(ops): AirportOps plugin skeleton, cross-plugin guard, dual-suite test filter"
```

---

### Task 2: USimClock

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/SimClock.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/Model/SimClock.cpp`
- Create: `Plugins/AirportOps/Source/AirportOpsTests/Private/SimClockTest.cpp`

**Interfaces:**
- Produces: `USimClock` with `Now()`, `Day()`, `TimeOfDay()`, `GetSpeed()`, `SetSpeed(ESimSpeed)`, `Multiplier()`, `TimeScale()`, `Advance(double RealDeltaSeconds)`, `At(double, TFunction<void()>) -> int32`, `Every(double, TFunction<void()>) -> int32`, `Cancel(int32) -> bool`. `ESimSpeed { Paused, X1, X2, X4, X8 }`. `USimClock::SecondsPerDay` constant `86400.0`.

- [ ] **Step 1: Write the failing tests**

`Plugins/AirportOps/Source/AirportOpsTests/Private/SimClockTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimClockSpeedTest,
	"AirportOps.Model.SimClock.Speed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimClockSpeedTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	Clock->RealSecondsPerGameDay = 1200.0;  // 20 real minutes per day -> 72 game s per real s

	Clock->Advance(1.0);
	TestEqual(TEXT("x1 advances by the day compression alone"), Clock->Now(), 72.0, 1e-9);

	Clock->SetSpeed(ESimSpeed::X2);
	Clock->Advance(1.0);
	TestEqual(TEXT("x2 doubles the compressed rate"), Clock->Now(), 72.0 + 144.0, 1e-9);

	Clock->SetSpeed(ESimSpeed::Paused);
	Clock->Advance(10.0);
	TestEqual(TEXT("paused advances nothing however long the real step"), Clock->Now(), 216.0, 1e-9);

	TestEqual(TEXT("Multiplier is the speed table, not the compression"),
		USimClock::Multiplier(ESimSpeed::X8), 8.0, 1e-12);
	TestEqual(TEXT("paused multiplier is zero"), USimClock::Multiplier(ESimSpeed::Paused), 0.0, 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimClockDayTest,
	"AirportOps.Model.SimClock.Day",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimClockDayTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	Clock->RealSecondsPerGameDay = 1.0;  // one real second is one game day: makes the arithmetic readable

	Clock->Advance(1.5);
	TestEqual(TEXT("a day and a half is day 1"), Clock->Day(), 1);
	TestEqual(TEXT("time of day wraps to half a day"), Clock->TimeOfDay(), USimClock::SecondsPerDay * 0.5, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimClockSchedulerTest,
	"AirportOps.Model.SimClock.Scheduler",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimClockSchedulerTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	Clock->RealSecondsPerGameDay = USimClock::SecondsPerDay;  // 1 real s == 1 game s, so steps read plainly

	TArray<FString> Fired;
	Clock->At(5.0, [&Fired]() { Fired.Add(TEXT("at5")); });
	Clock->Every(2.0, [&Fired]() { Fired.Add(TEXT("every2")); });
	const int32 Cancelled = Clock->At(3.0, [&Fired]() { Fired.Add(TEXT("cancelled")); });
	TestTrue(TEXT("a pending entry can be cancelled"), Clock->Cancel(Cancelled));
	TestFalse(TEXT("cancelling twice reports nothing to cancel"), Clock->Cancel(Cancelled));

	Clock->Advance(1.0);
	TestEqual(TEXT("nothing is due at t=1"), Fired.Num(), 0);

	// ONE big step. The point of the scheduler is that a large step drains every due entry
	// in TIME order, not that it fires whatever happens to be due at the end - a x8 frame
	// crossing several deliveries must post them all, oldest first.
	Clock->Advance(5.0);  // now t=6: every2 at 2,4,6; at5 at 5
	const TArray<FString> Expected = { TEXT("every2"), TEXT("every2"), TEXT("at5"), TEXT("every2") };
	TestEqual(TEXT("due entries fire in time order across one large step"), Fired, Expected);

	// An entry scheduled in the past fires on the next Advance rather than being lost.
	Clock->At(1.0, [&Fired]() { Fired.Add(TEXT("late")); });
	Clock->Advance(0.0);
	TestEqual(TEXT("a past-due entry fires on the next advance"), Fired.Last(), FString(TEXT("late")));

	TestEqual(TEXT("Every rejects a non-positive interval"), Clock->Every(0.0, []() {}), INDEX_NONE);
	return true;
}

#endif
```

- [ ] **Step 2: Build to verify the tests fail to compile**

Run the build line. Expected: FAIL with `Model/SimClock.h: No such file or directory`.

- [ ] **Step 3: Write the header**

`Plugins/AirportOps/Source/AirportOps/Public/Model/SimClock.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SimClock.generated.h"

/**
 * The player's speed setting. AN ENUM, NOT A FLOAT: the game offers exactly these steps,
 * and a float would let two code paths disagree about what "faster" means. Paused lives
 * here rather than as a separate bool because "paused AND x4" is not a state the game has.
 */
UENUM(BlueprintType)
enum class ESimSpeed : uint8
{
	Paused,
	X1,
	X2,
	X4,
	X8
};

/**
 * The one authority on game time.
 *
 * Two independent scalings meet here and it matters which is which:
 *  - DAY COMPRESSION (RealSecondsPerGameDay): how many real seconds a 24h game day takes.
 *    This scales the CLOCK only - upkeep ticks, contract deliveries, off-block times.
 *  - SPEED (ESimSpeed): the player's x1..x8. This scales the clock AND the movement layer.
 *
 * Airside agents run on Multiplier() alone, never on TimeScale(): a truck that drove 72x
 * faster because the day is 20 real minutes long would be unwatchable. Turnaround
 * durations are therefore authored in game time knowing a truck covers "real" distance
 * while the clock runs compressed - the standard sim compromise, stated here so nobody
 * tries to fix it by scaling movement.
 *
 * World-free: built with NewObject and advanced by hand in tests. The subsystem that
 * ticks it (UOpsRuntimeSubsystem) is in Present/ and is a forwarder only.
 *
 * SCHEDULER ENTRIES ARE NOT SAVED, on purpose. They hold TFunctions, which cannot be
 * serialised, and every system that schedules something owns the knowledge of what to
 * schedule - so on load each system re-registers from its own saved state. Saving the
 * queue would duplicate that knowledge and desync the two on the first edit.
 */
UCLASS()
class AIRPORTOPS_API USimClock : public UObject
{
	GENERATED_BODY()

public:
	/** Game seconds in one game day. A definition, not a tunable. */
	static constexpr double SecondsPerDay = 86400.0;

	/**
	 * Real seconds one game day takes at x1. A tunable, set from the scenario by
	 * UOpsRuntime; the default here is only what a bare NewObject gets.
	 */
	UPROPERTY() double RealSecondsPerGameDay = 1200.0;

	double Now() const { return GameSeconds; }
	int32 Day() const;
	double TimeOfDay() const;

	ESimSpeed GetSpeed() const { return Speed; }
	void SetSpeed(ESimSpeed NewSpeed);

	/** The speed table: 0, 1, 2, 4, 8. What Airside movement is scaled by. */
	static double Multiplier(ESimSpeed Speed);

	/** Multiplier() * (SecondsPerDay / RealSecondsPerGameDay): game seconds per real second. */
	double TimeScale() const;

	/**
	 * Advances game time by RealDeltaSeconds * TimeScale(), then fires every scheduled entry
	 * whose due time has passed, oldest first. A repeating entry that falls due several
	 * times inside one step fires that many times.
	 */
	void Advance(double RealDeltaSeconds);

	/** Fire once at an absolute game time. Returns a handle for Cancel. A past time fires on the next Advance. */
	int32 At(double GameTime, TFunction<void()> Callback);

	/** Fire every Interval game seconds, first at Now()+Interval. INDEX_NONE if Interval <= 0. */
	int32 Every(double Interval, TFunction<void()> Callback);

	/** True if the handle named a pending entry. */
	bool Cancel(int32 Handle);

	int32 PendingCountForTest() const { return Entries.Num(); }

private:
	struct FEntry
	{
		int32 Handle = INDEX_NONE;
		double Due = 0.0;
		/** 0 for a one-shot. */
		double Interval = 0.0;
		TFunction<void()> Callback;
	};

	UPROPERTY() double GameSeconds = 0.0;
	UPROPERTY() ESimSpeed Speed = ESimSpeed::X1;

	// Plain members, not UPROPERTY: see the class comment on why the queue is not saved.
	TArray<FEntry> Entries;
	int32 NextHandle = 1;

	int32 Add(double Due, double Interval, TFunction<void()>&& Callback);
};
```

- [ ] **Step 4: Write the implementation**

`Plugins/AirportOps/Source/AirportOps/Private/Model/SimClock.cpp`:
```cpp
#include "Model/SimClock.h"
#include "AirportOpsLog.h"

int32 USimClock::Day() const
{
	return static_cast<int32>(FMath::FloorToDouble(GameSeconds / SecondsPerDay));
}

double USimClock::TimeOfDay() const
{
	return GameSeconds - Day() * SecondsPerDay;
}

void USimClock::SetSpeed(ESimSpeed NewSpeed)
{
	if (Speed == NewSpeed)
	{
		return;
	}
	Speed = NewSpeed;
	UE_LOG(LogAirportOps, Log, TEXT("Sim speed x%.0f"), Multiplier(Speed));
}

double USimClock::Multiplier(ESimSpeed InSpeed)
{
	switch (InSpeed)
	{
	case ESimSpeed::Paused: return 0.0;
	case ESimSpeed::X1:     return 1.0;
	case ESimSpeed::X2:     return 2.0;
	case ESimSpeed::X4:     return 4.0;
	case ESimSpeed::X8:     return 8.0;
	}
	return 1.0;
}

double USimClock::TimeScale() const
{
	// Guarded rather than asserted: a zero from a mis-authored scenario should give a
	// frozen clock and a log line, not a division by zero in Tick.
	if (RealSecondsPerGameDay <= 0.0)
	{
		return 0.0;
	}
	return Multiplier(Speed) * (SecondsPerDay / RealSecondsPerGameDay);
}

void USimClock::Advance(double RealDeltaSeconds)
{
	GameSeconds += RealDeltaSeconds * TimeScale();

	// Drain in due order, re-scanning after every callback: a callback may schedule
	// something that is ALREADY due (an At in the past), and it must fire in this same
	// drain rather than wait a frame. Bounded by the entries' own due times, since every
	// repeating entry advances past Now() on each fire.
	while (true)
	{
		int32 Earliest = INDEX_NONE;
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			if (Entries[Index].Due <= GameSeconds
				&& (Earliest == INDEX_NONE || Entries[Index].Due < Entries[Earliest].Due))
			{
				Earliest = Index;
			}
		}
		if (Earliest == INDEX_NONE)
		{
			break;
		}

		// Copy the callback out before firing: the callback may Cancel or add entries,
		// which reallocates the array under a reference.
		TFunction<void()> Fire = Entries[Earliest].Callback;
		if (Entries[Earliest].Interval > 0.0)
		{
			Entries[Earliest].Due += Entries[Earliest].Interval;
		}
		else
		{
			Entries.RemoveAt(Earliest);
		}
		Fire();
	}
}

int32 USimClock::Add(double Due, double Interval, TFunction<void()>&& Callback)
{
	FEntry Entry;
	Entry.Handle = NextHandle++;
	Entry.Due = Due;
	Entry.Interval = Interval;
	Entry.Callback = MoveTemp(Callback);
	Entries.Add(MoveTemp(Entry));
	return Entries.Last().Handle;
}

int32 USimClock::At(double GameTime, TFunction<void()> Callback)
{
	return Add(GameTime, 0.0, MoveTemp(Callback));
}

int32 USimClock::Every(double Interval, TFunction<void()> Callback)
{
	if (Interval <= 0.0)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("SimClock::Every refused: interval %.3f is not positive"), Interval);
		return INDEX_NONE;
	}
	return Add(GameSeconds + Interval, Interval, MoveTemp(Callback));
}

bool USimClock::Cancel(int32 Handle)
{
	const int32 Removed = Entries.RemoveAll([Handle](const FEntry& E) { return E.Handle == Handle; });
	return Removed > 0;
}
```

- [ ] **Step 5: Build and run**

Build line, then `./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.SimClock`. Expected: `3 test(s) run, 0 failed, 0 crashed.`

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps
git commit -m "feat(ops): USimClock - speed table, day compression, ordered scheduler"
```

---

### Task 3: UOpsEvents

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsEvents.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/Model/OpsEvents.cpp`
- Create: `Plugins/AirportOps/Source/AirportOpsTests/Private/OpsEventsTest.cpp`

**Interfaces:**
- Consumes: `ESimSpeed` (Task 2); `EAgentPhase` from `Model/RoadAgent.h`; `EArrivalRefusal` from `Model/ArrivalPlanner.h` (both Airside).
- Produces: `UOpsEvents` with dynamic multicast delegates `OnAgentPhaseChanged(int32, EAgentPhase, EAgentPhase)`, `OnArrivalRefused(EArrivalRefusal)`, `OnSpeedChanged(ESimSpeed)`, `OnNotification(const FString&)`, and `Notify*` publishers of the same names.

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/OpsEvents.h"

#if WITH_DEV_AUTOMATION_TESTS

/** A UObject listener, because dynamic delegates bind to UFUNCTIONs, not lambdas. */
UCLASS()
class UOpsEventsTestListener : public UObject
{
	GENERATED_BODY()
public:
	TArray<FString> Seen;

	UFUNCTION() void OnPhase(int32 AgentId, EAgentPhase From, EAgentPhase To)
	{
		Seen.Add(FString::Printf(TEXT("phase:%d:%d->%d"), AgentId, static_cast<int32>(From), static_cast<int32>(To)));
	}
	UFUNCTION() void OnRefused(EArrivalRefusal Why) { Seen.Add(FString::Printf(TEXT("refused:%d"), static_cast<int32>(Why))); }
	UFUNCTION() void OnSpeed(ESimSpeed Speed) { Seen.Add(FString::Printf(TEXT("speed:%d"), static_cast<int32>(Speed))); }
	UFUNCTION() void OnNote(const FString& Text) { Seen.Add(TEXT("note:") + Text); }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsEventsTest,
	"AirportOps.Model.Events",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsEventsTest::RunTest(const FString& Parameters)
{
	UOpsEvents* Events = NewObject<UOpsEvents>();
	UOpsEventsTestListener* L = NewObject<UOpsEventsTestListener>();
	Events->OnAgentPhaseChanged.AddDynamic(L, &UOpsEventsTestListener::OnPhase);
	Events->OnArrivalRefused.AddDynamic(L, &UOpsEventsTestListener::OnRefused);
	Events->OnSpeedChanged.AddDynamic(L, &UOpsEventsTestListener::OnSpeed);
	Events->OnNotification.AddDynamic(L, &UOpsEventsTestListener::OnNote);

	Events->NotifyAgentPhaseChanged(7, EAgentPhase::Taxiing, EAgentPhase::Parked);
	Events->NotifyArrivalRefused(EArrivalRefusal::NoRunway);
	Events->NotifySpeedChanged(ESimSpeed::X4);
	Events->NotifyNotification(TEXT("hello"));

	const TArray<FString> Expected = {
		TEXT("phase:7:1->3"), TEXT("refused:1"), TEXT("speed:3"), TEXT("note:hello") };
	TestEqual(TEXT("each Notify reaches its bound listener with its arguments intact"), L->Seen, Expected);
	return true;
}

#endif
```
Note: the test file needs a `.generated.h` for its UCLASS. Name the file `OpsEventsTest.cpp` and put the listener class in `OpsEventsTestListener.h` beside it with `#include "OpsEventsTestListener.generated.h"`; UHT processes test module headers like any other. Move the UCLASS into that header and include it from the test.

- [ ] **Step 2: Build to verify failure**

Expected: FAIL, `Model/OpsEvents.h` not found.

- [ ] **Step 3: Write the header**

`Public/Model/OpsEvents.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadAgent.h"
#include "Model/SimClock.h"
#include "UObject/Object.h"
#include "OpsEvents.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOpsAgentPhaseChanged, int32, AgentId, EAgentPhase, From, EAgentPhase, To);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsArrivalRefused, EArrivalRefusal, Why);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsSpeedChanged, ESimSpeed, Speed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOpsNotification, const FString&, Text);

/**
 * The outcome bus. Pattern: Observer, via DYNAMIC multicast delegates so UMG and Blueprint
 * can bind without C++.
 *
 * Model code publishes through the Notify* functions and never knows who listens. This is
 * NOT the job pub/sub from the GDD - job assignment is request-response between the board
 * and depots (spec §3.5) and never goes through here. The bus announces what happened.
 *
 * Every Notify also writes a UE_LOG line: the log is this project's primary diagnostic
 * (CLAUDE.md "Diagnosing"), and an event nobody was bound to is otherwise invisible.
 *
 * Only events with a PUBLISHER in this milestone exist here. Flight, job, ledger and
 * contract events arrive with the systems that raise them; declaring them now would be a
 * list nothing consumes, which is the bug CLAUDE.md names three times.
 */
UCLASS(BlueprintType)
class AIRPORTOPS_API UOpsEvents : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable) FOpsAgentPhaseChanged OnAgentPhaseChanged;
	UPROPERTY(BlueprintAssignable) FOpsArrivalRefused    OnArrivalRefused;
	UPROPERTY(BlueprintAssignable) FOpsSpeedChanged      OnSpeedChanged;
	UPROPERTY(BlueprintAssignable) FOpsNotification      OnNotification;

	void NotifyAgentPhaseChanged(int32 AgentId, EAgentPhase From, EAgentPhase To);
	void NotifyArrivalRefused(EArrivalRefusal Why);
	void NotifySpeedChanged(ESimSpeed Speed);
	void NotifyNotification(const FString& Text);
};
```

- [ ] **Step 4: Write the implementation**

`Private/Model/OpsEvents.cpp`:
```cpp
#include "Model/OpsEvents.h"
#include "AirportOpsLog.h"

void UOpsEvents::NotifyAgentPhaseChanged(int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	UE_LOG(LogAirportOps, Verbose, TEXT("Agent %d: %s -> %s"), AgentId,
		*UEnum::GetValueAsString(From), *UEnum::GetValueAsString(To));
	OnAgentPhaseChanged.Broadcast(AgentId, From, To);
}

void UOpsEvents::NotifyArrivalRefused(EArrivalRefusal Why)
{
	UE_LOG(LogAirportOps, Log, TEXT("Arrival refused: %s"), *UEnum::GetValueAsString(Why));
	OnArrivalRefused.Broadcast(Why);
}

void UOpsEvents::NotifySpeedChanged(ESimSpeed Speed)
{
	OnSpeedChanged.Broadcast(Speed);
}

void UOpsEvents::NotifyNotification(const FString& Text)
{
	UE_LOG(LogAirportOps, Log, TEXT("Notification: %s"), *Text);
	OnNotification.Broadcast(Text);
}
```

- [ ] **Step 5: Build and run**

`./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.Events`. Expected: `1 test(s) run, 0 failed, 0 crashed.`

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps
git commit -m "feat(ops): UOpsEvents outcome bus with logging publishers"
```

---

### Task 4: Airside seam — agent ids and phase/refusal delegates

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/AirsideTraffic.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/AirsideTraffic.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/ArrivalDispatchTest.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/AgentEventsTest.cpp`

**Interfaces:**
- Produces on `UAirsideTraffic`: `DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentPhaseChanged, int32, EAgentPhase, EAgentPhase)` member `OnAgentPhaseChanged`; `DECLARE_MULTICAST_DELEGATE_OneParam(FOnArrivalRefused, EArrivalRefusal)` member `OnArrivalRefused`; `int32 GetNewestAgentId() const` (0 when none). On `ARoadNetworkActor`: `UAirsideTraffic* GetTraffic() const`.
- Convention: a spawn broadcasts `Gone -> Arriving` (or `Gone -> Taxiing`); a removal broadcasts `<last> -> Gone`. `Gone` doubles as "did not exist", the same reading `LastAgentPhaseForTest` already gives it.

- [ ] **Step 1: Count Airside log lines**

```bash
grep -c "UE_LOG(" Plugins/Airside/Source/Airside/Private/Present/AirsideTraffic.cpp
```
Record the number; it must not fall by the end of the task.

- [ ] **Step 2: Extend ArrivalDispatchTest to record phase events**

In `ArrivalDispatchTest.cpp`, immediately after the `Actor` null check (line ~44), add:
```cpp
	// PHASE EVENTS ARE THE SEAM AirportOps drives on (spec §2.0). Recorded here, in the test
	// that already ticks a real arrival end to end, because a delegate that fires in a unit
	// test of FRoadAgent proves nothing about whether UAirsideTraffic::Advance broadcasts it.
	TArray<TPair<EAgentPhase, EAgentPhase>> Transitions;
	Actor->GetTraffic()->OnAgentPhaseChanged.AddLambda(
		[&Transitions](int32, EAgentPhase From, EAgentPhase To) { Transitions.Emplace(From, To); });
```
At the end of the test, after the existing assertions that the agent reached `Parked`, add:
```cpp
	auto Has = [&Transitions](EAgentPhase From, EAgentPhase To)
	{
		return Transitions.ContainsByPredicate([From, To](const TPair<EAgentPhase, EAgentPhase>& T)
			{ return T.Key == From && T.Value == To; });
	};
	TestTrue(TEXT("spawn is announced as Gone -> Arriving"), Has(EAgentPhase::Gone, EAgentPhase::Arriving));
	TestTrue(TEXT("vacating the runway is announced as Arriving -> Taxiing"), Has(EAgentPhase::Arriving, EAgentPhase::Taxiing));
	TestTrue(TEXT("reaching the stand is announced as Taxiing -> Parked"), Has(EAgentPhase::Taxiing, EAgentPhase::Parked));
	TestTrue(TEXT("the agent id is assigned from 1"), Actor->GetTraffic()->GetNewestAgentId() >= 1);
```

- [ ] **Step 3: Write the refusal test**

`AgentEventsTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FArrivalRefusedEventTest,
	"Airside.Present.ArrivalRefusedEvent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FArrivalRefusedEventTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world to spawn into"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D::ZeroVector);  // forces the network into existence; no runway on it

	TArray<EArrivalRefusal> Refusals;
	Actor->GetTraffic()->OnArrivalRefused.AddLambda([&Refusals](EArrivalRefusal Why) { Refusals.Add(Why); });

	const bool bDispatched = Actor->DispatchArrival(FVector2D(1000.0, 0.0), UAirsideSettings::ResolveDefaultAirframe());
	TestFalse(TEXT("an airport with no runway refuses the arrival"), bDispatched);
	TestEqual(TEXT("the refusal is announced exactly once"), Refusals.Num(), 1);
	if (Refusals.Num() == 1)
	{
		TestEqual(TEXT("and names the reason the planner found"), Refusals[0], EArrivalRefusal::NoRunway);
	}
	TestEqual(TEXT("no agent exists after a refusal"), Actor->GetTraffic()->GetAgentCount(), 0);
	return true;
}

#endif
```
Check-Architecture's Piper rule: `ResolveDefaultAirframe` is the allowed path, so this compiles clean.

- [ ] **Step 4: Build to verify failure**

Expected: FAIL, `GetTraffic` and `OnAgentPhaseChanged` undeclared.

- [ ] **Step 5: Add ids and delegates to UAirsideTraffic**

In `AirsideTraffic.h`, inside `FAgentSlot` after `View`:
```cpp
	/**
	 * Stable identity for the lifetime of the agent. Slot indices shift on RemoveAt, and a
	 * pointer to the view dies with it, so a listener that wants to say "the one I
	 * dispatched" needs a number nothing else reuses. Assigned from UAirsideTraffic::
	 * NextAgentId; 0 means unassigned and is never handed out.
	 */
	UPROPERTY() int32 Id = 0;
```
Inside `UAirsideTraffic`, `public:`, before `DispatchArrival`:
```cpp
	/**
	 * Fired on every phase change, including spawn (From == Gone) and removal (To == Gone).
	 * Gone-as-"did not exist" is the reading LastAgentPhaseForTest already gives it, so one
	 * convention covers both ends of an agent's life without a separate spawned/removed pair.
	 * A plain (non-dynamic) delegate: the only subscriber is C++ in AirportOps, which relays
	 * onto its own Blueprint-facing bus. Making this one dynamic too would be two Blueprint
	 * surfaces for one fact.
	 */
	DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentPhaseChanged, int32 /*AgentId*/, EAgentPhase /*From*/, EAgentPhase /*To*/);
	FOnAgentPhaseChanged OnAgentPhaseChanged;

	/** Fired when DispatchArrival refuses, with the planner's reason. The log line stays too. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnArrivalRefused, EArrivalRefusal);
	FOnArrivalRefused OnArrivalRefused;

	/** Id of the most recently dispatched agent, or 0 when nothing is under way. */
	int32 GetNewestAgentId() const { return Agents.Num() > 0 ? Agents.Last().Id : 0; }
```
Add `#include "Model/ArrivalPlanner.h"` to the header's includes.

In the `private:` section after `Agents`:
```cpp
	/** Next FAgentSlot::Id to hand out. Transient: ids are per-session, agents never reach disk. */
	UPROPERTY(Transient) int32 NextAgentId = 1;

	/** Assigns the slot's id, stores it, and announces the spawn. The one place both happen. */
	void Admit(FAgentSlot&& Slot);
```

- [ ] **Step 6: Wire the broadcasts in AirsideTraffic.cpp**

In `DispatchArrival`, at the refusal branch (the line with `DescribeRefusal`), add after the `UE_LOG`:
```cpp
		OnArrivalRefused.Broadcast(Plan.Why);
```
Replace the two occurrences of
```cpp
	Slot.Agent = MoveTemp(Agent);
	Agents.Add(MoveTemp(Slot));
	return true;
```
(one in `DispatchArrival`, one in `DispatchAgent`) with
```cpp
	Slot.Agent = MoveTemp(Agent);
	Admit(MoveTemp(Slot));
	return true;
```
Add the helper:
```cpp
void UAirsideTraffic::Admit(FAgentSlot&& Slot)
{
	Slot.Id = NextAgentId++;
	const EAgentPhase Born = Slot.Agent.Phase;
	const int32 Id = Slot.Id;
	Agents.Add(MoveTemp(Slot));
	OnAgentPhaseChanged.Broadcast(Id, EAgentPhase::Gone, Born);
}
```
In `Advance`, change the loop body to detect transitions:
```cpp
	for (int32 Index = Agents.Num() - 1; Index >= 0; --Index)
	{
		FAgentSlot& Slot = Agents[Index];
		const EAgentPhase Before = Slot.Agent.Phase;
		const int32 Id = Slot.Id;

		FAgentMotion Motion;
		if (!Slot.Agent.Advance(DeltaSeconds, Motion))
		{
			// Cleared or otherwise finished - the aircraft has gone, so the actor goes with
			// it. An agent that stayed in the world would accumulate one per departure,
			// hanging above the airport for ever.
			if (Slot.View != nullptr)
			{
				Slot.View->Destroy();
			}
			Agents.RemoveAt(Index);
			// Broadcast AFTER the removal so a listener that asks GetAgentCount sees the
			// agent already gone, which is what "To == Gone" promises.
			OnAgentPhaseChanged.Broadcast(Id, Before, EAgentPhase::Gone);
			continue;
		}

		if (Slot.View != nullptr)
		{
			Slot.View->SetMotion(Motion, SurfaceZ);
		}

		if (Slot.Agent.Phase != Before)
		{
			OnAgentPhaseChanged.Broadcast(Id, Before, Slot.Agent.Phase);
		}
	}
```
Also in `ClearAgents`, before `Agents.Reset()`, broadcast each removal:
```cpp
	for (const FAgentSlot& Slot : Agents)
	{
		OnAgentPhaseChanged.Broadcast(Slot.Id, Slot.Agent.Phase, EAgentPhase::Gone);
	}
```
(place it after the existing view-destroy loop and before `Reset`).

- [ ] **Step 7: Expose Traffic on the actor**

In `RoadNetworkActor.h`, `public:`, near `DispatchArrival`:
```cpp
	/**
	 * The traffic mediator, for AirportOps to bind its delegates. READ ACCESS TO A SUBOBJECT,
	 * not a forwarder per delegate: the actor is a composition root that grows by forwarding
	 * (CLAUDE.md), and a forwarder per event would re-grow it one line per event for ever.
	 */
	UAirsideTraffic* GetTraffic() const { return Traffic; }
```

- [ ] **Step 8: Build, run, count**

Build. `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present`. Expected: `0 failed, 0 crashed` including `Airside.Present.ArrivalDispatch` and `Airside.Present.ArrivalRefusedEvent`. Re-run the grep from Step 1; the count must be equal or higher.

- [ ] **Step 9: Commit**

```bash
git add Plugins/Airside
git commit -m "feat(airside): agent ids and phase/refusal delegates on UAirsideTraffic"
```

---

### Task 5: Airside seam — RedirectAgent and RetireAgent

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/AirsideTraffic.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/AirsideTraffic.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/AgentRedirectTest.cpp`

**Interfaces:**
- Produces: `bool UAirsideTraffic::RedirectAgent(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Plan)`; `bool UAirsideTraffic::RetireAgent(int32 AgentId)`.
- Spec amendment (Task 10 records it): the seam offers PRIMITIVES. "Go to anchor, dwell, return" is composed by AirportOps from DispatchAgent → (Parked event) → wait dwell on the clock → RedirectAgent → (Parked event) → RetireAgent. Dwell is a fact about the job, so the sequencing belongs to the job board, not to movement.

- [ ] **Step 1: Write the failing test**

`AgentRedirectTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentRedirectTest,
	"Airside.Present.AgentRedirectRetire",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentRedirectTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world to spawn into"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }

	// A straight road A -> B. The derived guideline gives one node at each end.
	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(20000.0, 0.0));
	if (!TestTrue(TEXT("nodes connect into a segment"), Actor->ConnectNodes(A, B))) { return false; }
	URoadNetwork& Net = *Actor->Network;

	// Find the guideline nodes nearest each end. The derivation owns the ids, so look them up by position.
	auto NearestGuidelineNode = [&Net](const FVector2D& At)
	{
		FGuidelineNodeId Best;
		double BestDist = TNumericLimits<double>::Max();
		const TArray<FGuidelineNode>& Nodes = Net.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (!Nodes[Index].bAlive) { continue; }
			const double D = FVector2D::DistSquared(Nodes[Index].Position, At);
			if (D < BestDist) { BestDist = D; Best = Net.GuidelineNodeIdAt(Index); }
		}
		return Best;
	};
	const FGuidelineNodeId Start = NearestGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId End = NearestGuidelineNode(FVector2D(20000.0, 0.0));
	if (!TestTrue(TEXT("both guideline ends exist"), Start.IsSet() && End.IsSet())) { return false; }

	FRouteQuery Out; Out.Start = Start; Out.Goal = End; Out.Class = ETraversalClass::GroundVehicle;
	FRouteQuery Back; Back.Start = End; Back.Goal = Start; Back.Class = ETraversalClass::GroundVehicle;
	const FRoutePlan Outbound = RouteSearch::Find(Net, Out);
	const FRoutePlan Return = RouteSearch::Find(Net, Back);
	if (!TestTrue(TEXT("both legs route"), Outbound.IsValid() && Return.IsValid())) { return false; }

	UAirsideTraffic* Traffic = Actor->GetTraffic();
	TArray<TPair<EAgentPhase, EAgentPhase>> Transitions;
	Traffic->OnAgentPhaseChanged.AddLambda(
		[&Transitions](int32, EAgentPhase From, EAgentPhase To) { Transitions.Emplace(From, To); });

	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	if (!TestTrue(TEXT("outbound dispatch accepted"), Actor->DispatchAgent(Outbound, Airframe))) { return false; }
	const int32 Id = Traffic->GetNewestAgentId();

	auto TickUntil = [Actor](EAgentPhase Want, int32 MaxTicks)
	{
		for (int32 Tick = 0; Tick < MaxTicks; ++Tick)
		{
			Actor->Tick(1.0f / 30.0f);
			if (Actor->GetTraffic()->LastAgentPhaseForTest() == Want) { return true; }
		}
		return false;
	};
	if (!TestTrue(TEXT("the agent parks at the far end"), TickUntil(EAgentPhase::Parked, 20000))) { return false; }
	const FVector2D AtFarEnd = Traffic->LastAgentPositionForTest();
	TestTrue(TEXT("and it is near the far end"), FVector2D::Distance(AtFarEnd, FVector2D(20000.0, 0.0)) < 1500.0);

	TestFalse(TEXT("an unknown id is refused"), Traffic->RedirectAgent(Id + 999, &Net, Return));
	if (!TestTrue(TEXT("a parked agent accepts a new plan"), Traffic->RedirectAgent(Id, &Net, Return))) { return false; }
	TestEqual(TEXT("redirect keeps the SAME agent"), Traffic->GetAgentCount(), 1);
	TestEqual(TEXT("and the same id"), Traffic->GetNewestAgentId(), Id);
	if (!TestTrue(TEXT("it parks again at the start"), TickUntil(EAgentPhase::Parked, 20000))) { return false; }
	TestTrue(TEXT("back where it began"),
		FVector2D::Distance(Traffic->LastAgentPositionForTest(), FVector2D(0.0, 0.0)) < 1500.0);

	TestTrue(TEXT("retire removes it"), Traffic->RetireAgent(Id));
	TestEqual(TEXT("no agents remain"), Traffic->GetAgentCount(), 0);
	TestFalse(TEXT("retiring twice is refused"), Traffic->RetireAgent(Id));

	auto Count = [&Transitions](EAgentPhase From, EAgentPhase To)
	{
		int32 N = 0;
		for (const auto& T : Transitions) { if (T.Key == From && T.Value == To) { ++N; } }
		return N;
	};
	TestEqual(TEXT("two arrivals at a stop were announced"), Count(EAgentPhase::Taxiing, EAgentPhase::Parked), 2);
	TestEqual(TEXT("the redirect was announced as Parked -> Taxiing"), Count(EAgentPhase::Parked, EAgentPhase::Taxiing), 1);
	TestEqual(TEXT("the retirement was announced as Parked -> Gone"), Count(EAgentPhase::Parked, EAgentPhase::Gone), 1);
	return true;
}

#endif
```
Two helpers this test uses that may not exist yet: `URoadNetwork::GetGuidelineNodes()` and `GuidelineNodeIdAt(int32)`, and `UAirsideTraffic::LastAgentPositionForTest()`. Check with `grep -n "GetGuidelineNodes\|GuidelineNodeIdAt" Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h`. If `GetGuidelineNodes` exists but `GuidelineNodeIdAt` does not, add to `RoadNetwork.h` beside `GetGuidelineNode`:
```cpp
	/** The handle for a live slot index, for callers walking GetGuidelineNodes(). Unset if dead. */
	FGuidelineNodeId GuidelineNodeIdAt(int32 Index) const;
```
with the body building the id from `Index` and `GuidelineNodes[Index].Generation` the way the existing `Get*` accessors do (read `RoadSlotMap.h:25-31` for the exact construction). If `GetGuidelineNodes` is missing, add `const TArray<FGuidelineNode>& GetGuidelineNodes() const { return GuidelineNodes; }` beside `GetEntities()`. Add to `UAirsideTraffic` beside `LastAgentTaxiSpeedCapForTest`:
```cpp
	/** The newest agent's last shown position, road plane, for the redirect test. Zero when none. */
	FVector2D LastAgentPositionForTest() const
	{
		return Agents.Num() > 0 ? Agents.Last().Agent.LastMotion.Position : FVector2D::ZeroVector;
	}
```

- [ ] **Step 2: Build to verify failure**

Expected: FAIL, `RedirectAgent` undeclared.

- [ ] **Step 3: Declare the primitives**

In `AirsideTraffic.h`, `public:`, after `DispatchAgent`:
```cpp
	/**
	 * Sends an EXISTING agent along a new plan, keeping its id and its view.
	 *
	 * The seam AirportOps composes "go to the stand, dwell, return to the depot" from
	 * (spec §2.0, amended in §1.3 of the M1 plan): DispatchAgent, then on the Parked event
	 * wait the dwell on the sim clock, then this, then RetireAgent on the second Parked.
	 * Dwell lives with the job, not here, because how long a fuel truck stays is a fact about
	 * the fuel job, and movement should not have to be told about jobs.
	 *
	 * Accepted in Parked or Taxiing (a mid-route redirect is a replan). Refused in Arriving
	 * or Departing - an aircraft on the runway is not something a job can redirect - and for
	 * an unknown id. Arms a departure if the new route ends on a runway, exactly as
	 * DispatchAgent does, through the same helper.
	 */
	bool RedirectAgent(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Plan);

	/**
	 * Removes an agent and its view immediately, announcing <phase> -> Gone. For a service
	 * vehicle that has returned to its depot: it does not fly away, so nothing else would
	 * ever remove it. False for an unknown id.
	 */
	bool RetireAgent(int32 AgentId);
```
In `private:`:
```cpp
	int32 FindSlot(int32 AgentId) const;

	/**
	 * Arms a departure when Plan ends on a runway. Pulled out of DispatchAgent so
	 * RedirectAgent gets the identical rule; two copies would be the drift this codebase's
	 * "one struct per thing" rule exists to prevent.
	 */
	void ArmDepartureIfRunway(FRoadAgent& Agent, const URoadNetwork* Network, const FRoutePlan& Plan) const;
```

- [ ] **Step 4: Implement**

In `AirsideTraffic.cpp`, replace the "DOES THIS ROUTE END ON A RUNWAY?" block inside `DispatchAgent` (from the comment through the closing brace of the `if (Network != nullptr ...)`) with:
```cpp
	ArmDepartureIfRunway(Agent, Network, Plan);
```
Add:
```cpp
void UAirsideTraffic::ArmDepartureIfRunway(FRoadAgent& Agent, const URoadNetwork* Network, const FRoutePlan& Plan) const
{
	// DOES THIS ROUTE END ON A RUNWAY? Asked here rather than by the tool, because the answer
	// is a fact about the network and the last polyline point is the only thing that knows
	// where the route actually finished. A route that ends anywhere else simply taxis, which
	// is what every route did before departures existed.
	if (Network == nullptr || Plan.Polyline.Num() == 0 || !Agent.Airframe.Climb.IsSet())
	{
		return;
	}
	FVector2D Threshold;
	FVector2D Direction;
	double Length = 0.0;
	if (Network->RunwayExtentAt(Plan.Polyline.Last(), Threshold, Direction, Length))
	{
		Agent.ArmDeparture(Threshold, Direction, Length);

		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Route ends on runway %s: %.0f uu available, departure armed"),
			*RunwayDesignator::ToPairText(Direction), Length);
	}
}

int32 UAirsideTraffic::FindSlot(int32 AgentId) const
{
	return Agents.IndexOfByPredicate([AgentId](const FAgentSlot& S) { return S.Id == AgentId; });
}

bool UAirsideTraffic::RedirectAgent(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Plan)
{
	const int32 Index = FindSlot(AgentId);
	if (Index == INDEX_NONE || !Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		return false;
	}
	FAgentSlot& Slot = Agents[Index];
	const EAgentPhase Before = Slot.Agent.Phase;
	if (Before != EAgentPhase::Parked && Before != EAgentPhase::Taxiing)
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("RedirectAgent %d refused: agent is %s"),
			AgentId, *UEnum::GetValueAsString(Before));
		return false;
	}

	// The airframe is the agent's own - a redirect changes where it goes, not what it is.
	Slot.Agent.StartTaxi(Plan, Slot.Agent.Airframe);
	ArmDepartureIfRunway(Slot.Agent, Network, Plan);

	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d redirected: %.0f uu"), AgentId, Plan.Length);
	if (Slot.Agent.Phase != Before)
	{
		OnAgentPhaseChanged.Broadcast(AgentId, Before, Slot.Agent.Phase);
	}
	return true;
}

bool UAirsideTraffic::RetireAgent(int32 AgentId)
{
	const int32 Index = FindSlot(AgentId);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	const EAgentPhase Before = Agents[Index].Agent.Phase;
	if (Agents[Index].View != nullptr)
	{
		Agents[Index].View->Destroy();
	}
	Agents.RemoveAt(Index);
	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d retired"), AgentId);
	OnAgentPhaseChanged.Broadcast(AgentId, Before, EAgentPhase::Gone);
	return true;
}
```
Check that `FRoadAgent::Airframe` is a public field (`grep -n "FAirframe Airframe" Plugins/Airside/Source/Airside/Public/Model/RoadAgent.h`). If it is private, add a public `const FAirframe& GetAirframe() const { return Airframe; }` and use it. `StartTaxi` resets `EngineRPM` to 0 ("from cold"); for a vehicle at a stand that is right.

- [ ] **Step 5: Build, run**

`./Tools/Run-AirsideTests.ps1 -Filter Airside.Present`. Expected: `0 failed, 0 crashed`, `Airside.Present.AgentRedirectRetire` listed. Grep `UE_LOG(` count in `AirsideTraffic.cpp`: must be at least the Task 4 count plus 3.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside
git commit -m "feat(airside): RedirectAgent/RetireAgent primitives; shared departure arming"
```

---

### Task 6: Airside capability summary

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h` (FEntityInstance)
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h:209-210`, `Private/Model/RoadNetwork.cpp` (PlaceEntity)
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacadeSurfaces.cpp:197`
- Create: `Plugins/Airside/Source/Airside/Public/Model/AirsideCapability.h`, `Private/Model/AirsideCapability.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/AirsideCapabilityTest.cpp`

**Interfaces:**
- Produces:
```cpp
USTRUCT() struct AIRSIDE_API FRunwaySummary { FVector2D Threshold; FVector2D Direction; double Length; TObjectPtr<const URoadProfile> Profile; };
USTRUCT() struct AIRSIDE_API FStandSummary  { FEntityInstanceId Entity; double DesignWingspan; TArray<EServiceRole> AnchorRoles; };
USTRUCT() struct AIRSIDE_API FAirsideCapability { TArray<FRunwaySummary> Runways; TArray<FStandSummary> Stands; double LongestRunway() const; };
namespace AirsideCapability { AIRSIDE_API FAirsideCapability Summarise(const URoadNetwork& Network); }
```
- `URoadNetwork::PlaceEntity(UEntityDefinition*, TConstArrayView<FEntityAnchor>, const FVector2D&, double Heading, double DesignWingspan = 0.0)`.

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/AirsideCapability.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirsideCapabilityTest,
	"Airside.Model.Capability",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirsideCapabilityTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>();
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);

	TestEqual(TEXT("an empty network has no runways"), AirsideCapability::Summarise(*Net).Runways.Num(), 0);

	// A 60000 uu runway SPLIT at 20000 by a taxiway junction: two collinear segments, one strip.
	const FRoadNodeId R0 = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId R1 = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadNodeId R2 = Net->AddNode(FVector2D(60000.0, 0.0));
	Net->AddStraightSegment(R0, R1, Runway);
	Net->AddStraightSegment(R1, R2, Runway);
	const FRoadNodeId T = Net->AddNode(FVector2D(20000.0, 15000.0));
	Net->AddStraightSegment(R1, T, Taxiway);

	FAirsideCapability Cap = AirsideCapability::Summarise(*Net);
	TestEqual(TEXT("two collinear runway segments are ONE runway"), Cap.Runways.Num(), 1);
	if (Cap.Runways.Num() == 1)
	{
		TestEqual(TEXT("whose length is the whole strip"), Cap.Runways[0].Length, 60000.0, 1.0);
		TestEqual(TEXT("and whose profile is the runway's"), Cap.Runways[0].Profile.Get(), static_cast<const URoadProfile*>(Runway));
	}
	TestEqual(TEXT("LongestRunway reads the same figure"), Cap.LongestRunway(), 60000.0, 1.0);

	// A second, separate runway.
	const FRoadNodeId S0 = Net->AddNode(FVector2D(0.0, 100000.0));
	const FRoadNodeId S1 = Net->AddNode(FVector2D(30000.0, 100000.0));
	Net->AddStraightSegment(S0, S1, Runway);
	Cap = AirsideCapability::Summarise(*Net);
	TestEqual(TEXT("a separate strip is a second runway"), Cap.Runways.Num(), 2);
	TestEqual(TEXT("longest is still the 60000 one"), Cap.LongestRunway(), 60000.0, 1.0);

	// Stands: wingspan captured at placement, roles read from resolved anchors.
	TArray<FEntityAnchor> Anchors;
	{ FEntityAnchor A; A.Id = TEXT("nose"); A.Role = EServiceRole::Aircraft; Anchors.Add(A); }
	{ FEntityAnchor A; A.Id = TEXT("fuel"); A.Role = EServiceRole::Fuel; A.LocalPosition = FVector2D(0.0, 800.0); Anchors.Add(A); }
	const FEntityInstanceId Small = Net->PlaceEntity(nullptr, Anchors, FVector2D(5000.0, 30000.0), 0.0, 1100.0);
	const FEntityInstanceId Big = Net->PlaceEntity(nullptr, Anchors, FVector2D(15000.0, 30000.0), 0.0, 3600.0);
	if (!TestTrue(TEXT("stands placed"), Small.IsSet() && Big.IsSet())) { return false; }

	Cap = AirsideCapability::Summarise(*Net);
	TestEqual(TEXT("two stands"), Cap.Stands.Num(), 2);
	const FStandSummary* BigSummary = Cap.Stands.FindByPredicate([Big](const FStandSummary& S) { return S.Entity == Big; });
	if (TestNotNull(TEXT("the big stand is summarised"), BigSummary))
	{
		TestEqual(TEXT("with the wingspan it was placed for"), BigSummary->DesignWingspan, 3600.0, 1e-9);
		TestTrue(TEXT("and the Fuel role its anchors carry"), BigSummary->AnchorRoles.Contains(EServiceRole::Fuel));
		TestTrue(TEXT("and the Aircraft role"), BigSummary->AnchorRoles.Contains(EServiceRole::Aircraft));
	}
	return true;
}

#endif
```
Note: `PlaceEntity` with a null definition returns unset per its comment ("Returns an unset handle for a null definition"). Check the body; if so, the test must pass a definition. Model tests elsewhere (`RoadEntityTest.cpp`) show how they construct one - copy that (likely `NewObject<UEntityDefinition>()` is NOT allowed from Model tests since it is Entities/; but tests are not bound by the include rule, only the plugin source is). Use `NewObject<UEntityDefinition>()` and include `Entities/EntityDefinition.h`.

- [ ] **Step 2: Build to verify failure**

Expected: FAIL, `Model/AirsideCapability.h` not found.

- [ ] **Step 3: Capture the design wingspan at placement**

In `RoadEntity.h`, inside `FEntityInstance` after `ResolvedAnchors`:
```cpp
	/**
	 * Wingspan of the aircraft this stand was designed for, uu, captured at placement.
	 * CAPTURED, not read from Definition->DesignAircraft, for the same reason FResolvedAnchor
	 * copies Role: Model/ must not dereference the Entities layer. 0 means unknown.
	 * The capability summary (Model/AirsideCapability.h) classes the stand from this.
	 */
	UPROPERTY() double DesignWingspan = 0.0;
```
In `RoadNetwork.h:209-210` change the declaration to:
```cpp
	FEntityInstanceId PlaceEntity(UEntityDefinition* Definition,
		TConstArrayView<FEntityAnchor> Anchors, const FVector2D& Position, double Heading,
		double DesignWingspan = 0.0);
```
and extend its doc comment with one line: `DesignWingspan is stored on the instance for the capability summary; the caller reads it from the definition for the same Model/-must-not-see-Entities/ reason as Anchors.` In `RoadNetwork.cpp`, add the parameter to the definition and set `Instance.DesignWingspan = DesignWingspan;` where the instance's `Position`/`Heading` are set.

In `RoadEditFacadeSurfaces.cpp:197`:
```cpp
	const double DesignWingspan = Stand->DesignAircraft != nullptr ? Stand->DesignAircraft->Footprint.Wingspan : 0.0;
	const FEntityInstanceId Placed = Net.PlaceEntity(Stand, Stand->Anchors, Where, Heading, DesignWingspan);
```
Check `UAircraftType::Footprint` is public (`AircraftType.h:39` shows `UPROPERTY(EditAnywhere) FEntityFootprint Footprint;`).

- [ ] **Step 4: Write the summary**

`Public/Model/AirsideCapability.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"
#include "AirsideCapability.generated.h"

class URoadNetwork;
class URoadProfile;

/** One runway strip: every collinear continuous-profile segment between two thresholds. */
USTRUCT()
struct AIRSIDE_API FRunwaySummary
{
	GENERATED_BODY()

	UPROPERTY() FVector2D Threshold = FVector2D::ZeroVector;
	UPROPERTY() FVector2D Direction = FVector2D(1.0, 0.0);
	UPROPERTY() double Length = 0.0;
	/** The pavement kind - the surface (grass/asphalt/concrete) is a fact about the profile. */
	UPROPERTY() TObjectPtr<const URoadProfile> Profile = nullptr;
};

/** One stand: what it was sized for and which service roles can reach it. */
USTRUCT()
struct AIRSIDE_API FStandSummary
{
	GENERATED_BODY()

	UPROPERTY() FEntityInstanceId Entity;
	UPROPERTY() double DesignWingspan = 0.0;
	UPROPERTY() TArray<EServiceRole> AnchorRoles;
};

/**
 * What the AIRFIELD can admit, as a pure function of the graph. The building half of the
 * airport's capability (which services are offered) lives in AirportOps and joins this.
 * Spec §3.1.
 */
USTRUCT()
struct AIRSIDE_API FAirsideCapability
{
	GENERATED_BODY()

	UPROPERTY() TArray<FRunwaySummary> Runways;
	UPROPERTY() TArray<FStandSummary> Stands;

	double LongestRunway() const;
};

namespace AirsideCapability
{
	/**
	 * Enumerates runways and stands. A runway is recognised by its profile
	 * (URoadProfile::bContinuousThroughJunctions), and a strip split by exits is ONE runway:
	 * each continuous segment is asked for its extent via URoadNetwork::RunwayExtentAt, and
	 * extents that share a threshold (either end) are the same strip. Deduplicated on the
	 * threshold pair rather than on segment adjacency so that the walk RunwayExtentAt
	 * already does is the only definition of "one runway" in the codebase.
	 */
	AIRSIDE_API FAirsideCapability Summarise(const URoadNetwork& Network);
}
```

`Private/Model/AirsideCapability.cpp`:
```cpp
#include "Model/AirsideCapability.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"

double FAirsideCapability::LongestRunway() const
{
	double Longest = 0.0;
	for (const FRunwaySummary& R : Runways)
	{
		Longest = FMath::Max(Longest, R.Length);
	}
	return Longest;
}

namespace
{
	bool SameStrip(const FRunwaySummary& A, const FVector2D& Threshold, const FVector2D& FarEnd)
	{
		// 1 uu: thresholds come from the same node positions, so anything looser would be
		// tolerating a disagreement that cannot happen.
		const double Tol = 1.0;
		const FVector2D AFar = A.Threshold + A.Direction * A.Length;
		return (FVector2D::Distance(A.Threshold, Threshold) < Tol && FVector2D::Distance(AFar, FarEnd) < Tol)
			|| (FVector2D::Distance(A.Threshold, FarEnd) < Tol && FVector2D::Distance(AFar, Threshold) < Tol);
	}
}

FAirsideCapability AirsideCapability::Summarise(const URoadNetwork& Network)
{
	FAirsideCapability Out;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (const FRoadSegment& Segment : Segments)
	{
		if (!Segment.bAlive) { continue; }
		const URoadProfile* Profile = Network.ProfileFor(Segment);
		if (Profile == nullptr || !Profile->bContinuousThroughJunctions) { continue; }

		const FRoadNode* A = Network.GetNode(Segment.A);
		const FRoadNode* B = Network.GetNode(Segment.B);
		if (A == nullptr || B == nullptr) { continue; }

		FVector2D Threshold, Direction;
		double Length = 0.0;
		if (!Network.RunwayExtentAt((A->Position + B->Position) * 0.5, Threshold, Direction, Length)) { continue; }
		const FVector2D FarEnd = Threshold + Direction * Length;

		const bool bKnown = Out.Runways.ContainsByPredicate(
			[&](const FRunwaySummary& R) { return SameStrip(R, Threshold, FarEnd); });
		if (bKnown) { continue; }

		FRunwaySummary R;
		R.Threshold = Threshold;
		R.Direction = Direction;
		R.Length = Length;
		R.Profile = Profile;
		Out.Runways.Add(R);
	}

	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& E = Entities[Index];
		if (!E.bAlive) { continue; }
		FStandSummary S;
		S.Entity = Network.EntityIdAt(Index);
		S.DesignWingspan = E.DesignWingspan;
		for (const FResolvedAnchor& Anchor : E.ResolvedAnchors)
		{
			S.AnchorRoles.AddUnique(Anchor.Role);
		}
		Out.Stands.Add(S);
	}
	return Out;
}
```
Two accessors may be missing on `URoadNetwork`: `GetSegments()` and `EntityIdAt(int32)`. Check with grep; add beside `GetEntities()` following the same pattern as `GuidelineNodeIdAt` in Task 5 (construct the handle from index + generation).

- [ ] **Step 5: Build, run**

`./Tools/Run-AirsideTests.ps1 -Filter Airside`. Expected: `0 failed, 0 crashed`, `Airside.Model.Capability` listed, all existing `PlaceEntity` callers (tests listed in the file map) still compile via the default parameter.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside
git commit -m "feat(airside): capability summary - runways by strip, stands by design wingspan"
```

---

### Task 7: Save/load model

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsSave.h`, `Private/Model/OpsSave.cpp`
- Create: `Plugins/AirportOps/Source/AirportOpsTests/Private/OpsSaveTest.cpp`

**Interfaces:**
- Consumes: `USimClock` (Task 2), `URoadNetwork`, `AirsideCapability::Summarise` (Task 6).
- Produces:
```cpp
USTRUCT() struct FOpsSnapshot { UPROPERTY() int32 Version = 1; UPROPERTY() TArray<uint8> Clock; UPROPERTY() TArray<uint8> Network; };
namespace OpsSave {
  AIRPORTOPS_API void SerializeObject(UObject& Object, TArray<uint8>& OutBytes);
  AIRPORTOPS_API void DeserializeObject(UObject& Object, const TArray<uint8>& Bytes);
  AIRPORTOPS_API void Capture(const USimClock& Clock, const URoadNetwork& Network, FOpsSnapshot& Out);
  AIRPORTOPS_API bool Restore(const FOpsSnapshot& In, USimClock& Clock, URoadNetwork& Network);
  AIRPORTOPS_API bool WriteSlot(const FString& SlotName, const FOpsSnapshot& Snapshot);
  AIRPORTOPS_API bool ReadSlot(const FString& SlotName, FOpsSnapshot& Out);
}
UCLASS() class UOpsSaveGame : public USaveGame { UPROPERTY() FOpsSnapshot Snapshot; };
```
- Spec amendment (Task 10 records it): the archive does NOT set `ArIsSaveGame`. `FProperty::ShouldSerializeValue` (`Property.cpp:1052`) skips every property lacking `CPF_SaveGame` when it is set, INCLUDING members of nested structs, so honouring §2.3 literally would mean tagging every field of every Airside struct. The rule becomes: **a model object's non-Transient UPROPERTYs are its saved state.** Anything that must not be saved is `Transient`.

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/AirsideCapability.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSaveRoundTripTest,
	"AirportOps.Model.Save.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSaveRoundTripTest::RunTest(const FString& Parameters)
{
	// A transient profile is NOT an asset, so its path cannot be restored by name. The
	// network's runway must therefore be recognised from a profile the loader can find, which
	// in the real game is a content asset. For the test: keep the SAME profile object alive
	// across capture and restore and assert the restored segments point back at it - which
	// is exactly what FObjectAndNameAsStringProxyArchive does for an object it can find by
	// path, and what a level-loaded asset gives in play.
	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;
	Runway->Rename(TEXT("RT_RunwayProfile"), GetTransientPackage());

	URoadNetwork* Source = NewObject<URoadNetwork>();
	const FRoadNodeId A = Source->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Source->AddNode(FVector2D(40000.0, 0.0));
	Source->AddStraightSegment(A, B, Runway);
	// Remove and re-add a node so the free list and a bumped generation are part of the state.
	const FRoadNodeId Spare = Source->AddNode(FVector2D(0.0, 5000.0));
	Source->RemoveNode(Spare);

	USimClock* Clock = NewObject<USimClock>();
	Clock->RealSecondsPerGameDay = 600.0;
	Clock->SetSpeed(ESimSpeed::X4);
	Clock->Advance(3.0);
	const double SavedNow = Clock->Now();

	FOpsSnapshot Snapshot;
	OpsSave::Capture(*Clock, *Source, Snapshot);
	TestTrue(TEXT("the snapshot holds bytes for both objects"), Snapshot.Clock.Num() > 0 && Snapshot.Network.Num() > 0);

	URoadNetwork* Restored = NewObject<URoadNetwork>();
	USimClock* RestoredClock = NewObject<USimClock>();
	if (!TestTrue(TEXT("restore succeeds"), OpsSave::Restore(Snapshot, *RestoredClock, *Restored))) { return false; }

	TestEqual(TEXT("game time survives"), RestoredClock->Now(), SavedNow, 1e-9);
	TestEqual(TEXT("speed survives"), RestoredClock->GetSpeed(), ESimSpeed::X4);
	TestEqual(TEXT("day length survives"), RestoredClock->RealSecondsPerGameDay, 600.0, 1e-9);

	const FAirsideCapability Cap = AirsideCapability::Summarise(*Restored);
	TestEqual(TEXT("the runway is still a runway after load - the profile reference resolved"), Cap.Runways.Num(), 1);
	TestEqual(TEXT("with its length"), Cap.LongestRunway(), 40000.0, 1.0);

	// Handles: the SAME id must still name the same node, generation included.
	const FRoadNode* NodeB = Restored->GetNode(B);
	if (TestNotNull(TEXT("a pre-save handle resolves on the restored network"), NodeB))
	{
		TestEqual(TEXT("to the node it named"), NodeB->Position, FVector2D(40000.0, 0.0));
	}
	TestNull(TEXT("a handle removed before the save stays dead after it"), Restored->GetNode(Spare));

	// The free list came too: the next AddNode reuses the freed slot with a higher generation.
	const FRoadNodeId Reused = Restored->AddNode(FVector2D(1.0, 1.0));
	TestEqual(TEXT("the freed slot is recycled"), Reused.Index, Spare.Index);
	TestTrue(TEXT("with a newer generation than the dead handle"), Reused.Generation > Spare.Generation);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsSaveSlotTest,
	"AirportOps.Model.Save.Slot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsSaveSlotTest::RunTest(const FString& Parameters)
{
	FOpsSnapshot Out;
	Out.Clock = { 1, 2, 3 };
	Out.Network = { 9, 8 };
	const FString Slot = TEXT("AirportOpsTest_Slot");
	if (!TestTrue(TEXT("a snapshot writes to a slot"), OpsSave::WriteSlot(Slot, Out))) { return false; }

	FOpsSnapshot In;
	if (!TestTrue(TEXT("and reads back"), OpsSave::ReadSlot(Slot, In))) { return false; }
	TestEqual(TEXT("byte-identical clock"), In.Clock, Out.Clock);
	TestEqual(TEXT("byte-identical network"), In.Network, Out.Network);
	TestEqual(TEXT("version tag carried"), In.Version, Out.Version);

	FOpsSnapshot Missing;
	TestFalse(TEXT("a slot that does not exist reads false, not garbage"), OpsSave::ReadSlot(TEXT("AirportOpsTest_NoSuchSlot"), Missing));
	return true;
}

#endif
```
Check `FRoadNodeId` exposes `Index` and `Generation` publicly (`RoadHandles.h:8-38`). If they are named differently, use the real names.

- [ ] **Step 2: Build to verify failure**

Expected: FAIL, `Model/OpsSave.h` not found.

- [ ] **Step 3: Write the header**

`Public/Model/OpsSave.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "OpsSave.generated.h"

class USimClock;
class URoadNetwork;

/**
 * Everything a save holds, as opaque byte blobs per model object.
 *
 * ONE BLOB PER OBJECT rather than one archive for all of them, so a save from before a
 * new system existed still loads: a missing blob means "that system starts fresh", and
 * a blob for an object the build no longer has is skipped. Version is for the day a blob's
 * own layout changes incompatibly, which tagged-property serialisation mostly absorbs.
 */
USTRUCT()
struct AIRPORTOPS_API FOpsSnapshot
{
	GENERATED_BODY()

	UPROPERTY() int32 Version = 1;
	UPROPERTY() TArray<uint8> Clock;
	UPROPERTY() TArray<uint8> Network;
};

/** The USaveGame wrapper UGameplayStatics needs for a slot on disk. Holds a snapshot and nothing else. */
UCLASS()
class AIRPORTOPS_API UOpsSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY() FOpsSnapshot Snapshot;
};

/**
 * Save and load of the model.
 *
 * THE RULE: a model object's non-Transient UPROPERTYs ARE its saved state. Spec §2.3 said
 * "tag every field SaveGame"; that was written before reading FProperty::ShouldSerializeValue
 * (Property.cpp:1052), which with ArIsSaveGame set skips EVERY untagged property including
 * the members of nested structs - so honouring it would mean tagging every field of every
 * Airside struct, and the first forgotten tag would silently drop a field from every save.
 * Not setting ArIsSaveGame and marking the exceptions Transient inverts the default to the
 * safe side: forgetting a Transient saves one field too many, which is visible; forgetting
 * a SaveGame loses one, which is not.
 *
 * Object references (profiles, definitions) go through FObjectAndNameAsStringProxyArchive
 * as path names and are re-found by path on load, which is what content assets support and
 * transient objects do not. Views are never saved; Present/ rebuilds from the model.
 */
namespace OpsSave
{
	AIRPORTOPS_API void SerializeObject(UObject& Object, TArray<uint8>& OutBytes);
	AIRPORTOPS_API void DeserializeObject(UObject& Object, const TArray<uint8>& Bytes);

	AIRPORTOPS_API void Capture(const USimClock& Clock, const URoadNetwork& Network, FOpsSnapshot& Out);

	/** False only when a blob is present and fails to deserialise. Missing blobs leave the target untouched. */
	AIRPORTOPS_API bool Restore(const FOpsSnapshot& In, USimClock& Clock, URoadNetwork& Network);

	AIRPORTOPS_API bool WriteSlot(const FString& SlotName, const FOpsSnapshot& Snapshot);
	AIRPORTOPS_API bool ReadSlot(const FString& SlotName, FOpsSnapshot& Out);
}
```

- [ ] **Step 4: Write the implementation**

`Private/Model/OpsSave.cpp`:
```cpp
#include "Model/OpsSave.h"
#include "AirportOpsLog.h"
#include "Kismet/GameplayStatics.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"

void OpsSave::SerializeObject(UObject& Object, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	FMemoryWriter Writer(OutBytes, /*bIsPersistent*/ true);
	FObjectAndNameAsStringProxyArchive Ar(Writer, /*bInLoadIfFindFails*/ false);
	// ArIsSaveGame deliberately left false - see the namespace comment in the header.
	Object.Serialize(Ar);
}

void OpsSave::DeserializeObject(UObject& Object, const TArray<uint8>& Bytes)
{
	FMemoryReader Reader(Bytes, /*bIsPersistent*/ true);
	// bLoadIfFindFails: an asset referenced by path that is not yet in memory gets loaded,
	// which is the case for a profile or a stand definition on a cold start.
	FObjectAndNameAsStringProxyArchive Ar(Reader, /*bInLoadIfFindFails*/ true);
	Object.Serialize(Ar);
}

void OpsSave::Capture(const USimClock& Clock, const URoadNetwork& Network, FOpsSnapshot& Out)
{
	// Serialize is non-const on UObject; the archive is saving, so nothing is written to them.
	SerializeObject(const_cast<USimClock&>(Clock), Out.Clock);
	SerializeObject(const_cast<URoadNetwork&>(Network), Out.Network);
	UE_LOG(LogAirportOps, Log, TEXT("Captured snapshot: clock %d bytes, network %d bytes"), Out.Clock.Num(), Out.Network.Num());
}

bool OpsSave::Restore(const FOpsSnapshot& In, USimClock& Clock, URoadNetwork& Network)
{
	if (In.Clock.Num() > 0)
	{
		DeserializeObject(Clock, In.Clock);
	}
	if (In.Network.Num() > 0)
	{
		DeserializeObject(Network, In.Network);
	}
	UE_LOG(LogAirportOps, Log, TEXT("Restored snapshot v%d: game time %.1f, %d nodes"),
		In.Version, Clock.Now(), Network.GetNodes().Num());
	return true;
}

bool OpsSave::WriteSlot(const FString& SlotName, const FOpsSnapshot& Snapshot)
{
	UOpsSaveGame* Save = Cast<UOpsSaveGame>(UGameplayStatics::CreateSaveGameObject(UOpsSaveGame::StaticClass()));
	if (Save == nullptr)
	{
		return false;
	}
	Save->Snapshot = Snapshot;
	const bool bOk = UGameplayStatics::SaveGameToSlot(Save, SlotName, 0);
	UE_LOG(LogAirportOps, Log, TEXT("Save to slot '%s': %s"), *SlotName, bOk ? TEXT("ok") : TEXT("FAILED"));
	return bOk;
}

bool OpsSave::ReadSlot(const FString& SlotName, FOpsSnapshot& Out)
{
	if (!UGameplayStatics::DoesSaveGameExist(SlotName, 0))
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Load from slot '%s': no such slot"), *SlotName);
		return false;
	}
	UOpsSaveGame* Save = Cast<UOpsSaveGame>(UGameplayStatics::LoadGameFromSlot(SlotName, 0));
	if (Save == nullptr)
	{
		UE_LOG(LogAirportOps, Error, TEXT("Load from slot '%s': not an AirportOps save"), *SlotName);
		return false;
	}
	Out = Save->Snapshot;
	return true;
}
```
`URoadNetwork::GetNodes()` exists (used by the HUD). If `Restore` finds the network has derived caches that are not UPROPERTY (check `RoadNetwork.h` for non-UPROPERTY members), call the network's own rebuild-derived function after deserialising; if none exists, nothing to do.

- [ ] **Step 5: Build, run**

`./Tools/Run-AirsideTests.ps1 -Filter AirportOps.Model.Save`. Expected: `2 test(s) run, 0 failed, 0 crashed.` If the round-trip test fails on "the runway is still a runway", the profile reference did not resolve: confirm the `Rename` into the transient package made it findable by path, or switch the test to load a real profile asset via `UAirsideSettings::GetContent()` and skip with a logged reason when no content is configured.

- [ ] **Step 6: Commit**

```bash
git add Plugins/AirportOps
git commit -m "feat(ops): snapshot save/load of clock and network via proxy archive; slot IO"
```

---

### Task 8: UOpsRuntime, subsystem, actor time scale, keys and HUD

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h`, `Private/Present/RoadNetworkActor.cpp:358-362`
- Create: `Plugins/Airside/Source/AirsideTests/Private/SimTimeScaleTest.cpp`
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Present/OpsRuntime.h`, `Private/Present/OpsRuntime.cpp`
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Present/OpsRuntimeSubsystem.h`, `Private/Present/OpsRuntimeSubsystem.cpp`
- Create: `Plugins/AirportOps/Source/AirportOpsTests/Private/OpsRuntimeTest.cpp`
- Modify: `Source/AirportMgr/RoadBuildController.h`, `.cpp:207-240`
- Modify: `Source/AirportMgr/RoadBuildHUD.cpp:62-70`

**Interfaces:**
- Consumes: `USimClock`, `UOpsEvents`, `OpsSave`, `UAirsideTraffic` delegates (Tasks 2-7).
- Produces on `ARoadNetworkActor`: `void SetSimTimeScale(double)`, `double GetSimTimeScale() const`. `UOpsRuntime` (UObject): `USimClock* GetClock()`, `UOpsEvents* GetEvents()`, `void Attach(ARoadNetworkActor*)`, `void Tick(double RealDeltaSeconds)`, `bool SaveToSlot(const FString&)`, `bool LoadFromSlot(const FString&)`, `void StepSpeed(int32 Delta)`, `void TogglePause()`. `UOpsRuntimeSubsystem::Get(UWorld*) -> UOpsRuntime*` static convenience.

- [ ] **Step 1: Write the Airside time-scale test**

`SimTimeScaleTest.cpp`: same world/actor/road fixture as `AgentRedirectTest` Steps (A→B road, route Start→End, `DispatchAgent`). Then:
```cpp
	// Two identical agents, one ticked at scale 1 for N frames and read; then the actor set
	// to scale 2 and the second agent ticked N frames. Distance covered must be ~2x. Measured
	// on position, not on a flag: a flag that said "scaled" while Advance ignored it would
	// pass and the clock would run at x8 with trucks at x1.
	Actor->SetSimTimeScale(1.0);
	Actor->DispatchAgent(Outbound, Airframe);
	for (int32 I = 0; I < 30; ++I) { Actor->Tick(1.0f / 30.0f); }
	const double D1 = FVector2D::Distance(Traffic->LastAgentPositionForTest(), FVector2D(0.0, 0.0));
	Traffic->ClearAgents();

	Actor->SetSimTimeScale(2.0);
	Actor->DispatchAgent(Outbound, Airframe);
	for (int32 I = 0; I < 30; ++I) { Actor->Tick(1.0f / 30.0f); }
	const double D2 = FVector2D::Distance(Traffic->LastAgentPositionForTest(), FVector2D(0.0, 0.0));

	TestTrue(TEXT("the agent moved at all at x1"), D1 > 100.0);
	TestEqual(TEXT("x2 covers twice the ground in the same real frames"), D2 / D1, 2.0, 0.05);
```
Name: `"Airside.Present.SimTimeScale"`. Use a 200000 uu road so neither run reaches the end (Piper taxi cap 1000 uu/s × 1 s × 2 = 2000 uu, so even 20000 is fine; keep 20000).

- [ ] **Step 2: Add the knob to the actor**

`RoadNetworkActor.h`, `public:`:
```cpp
	/**
	 * Multiplier applied to every Tick's DeltaSeconds before it reaches Traffic. Set each
	 * frame by AirportOps from the sim clock's SPEED (x0..x8), never from its day
	 * compression - see USimClock's class comment for why the two are different numbers.
	 * Transient and runtime-only: it is a fact about the current session's speed setting,
	 * not about the level, so it must not be saved into the map or a game save.
	 */
	void SetSimTimeScale(double Scale) { SimTimeScale = FMath::Max(0.0, Scale); }
	double GetSimTimeScale() const { return SimTimeScale; }
```
`private:` (near `Traffic`): `UPROPERTY(Transient) double SimTimeScale = 1.0;`
`RoadNetworkActor.cpp` Tick:
```cpp
void ARoadNetworkActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Traffic->Advance(static_cast<float>(DeltaSeconds * SimTimeScale), SurfaceZ);
}
```

- [ ] **Step 3: Write the runtime test**

`OpsRuntimeTest.cpp` — same fixture (world, actor, road, plans). Then:
```cpp
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);

	UOpsEventsTestListener* L = NewObject<UOpsEventsTestListener>();  // from Task 3's header
	Runtime->GetEvents()->OnAgentPhaseChanged.AddDynamic(L, &UOpsEventsTestListener::OnPhase);
	Runtime->GetEvents()->OnSpeedChanged.AddDynamic(L, &UOpsEventsTestListener::OnSpeed);

	Actor->DispatchAgent(Outbound, Airframe);
	TestTrue(TEXT("a spawn on the Airside traffic reaches the ops bus"),
		L->Seen.ContainsByPredicate([](const FString& S) { return S.StartsWith(TEXT("phase:")) && S.EndsWith(TEXT("->1")); }));

	Runtime->StepSpeed(+1);
	TestEqual(TEXT("stepping speed announces the new speed"), L->Seen.Last(), FString(TEXT("speed:2")));
	TestEqual(TEXT("and pushes the multiplier into the actor"), Actor->GetSimTimeScale(), 2.0, 1e-12);

	Runtime->TogglePause();
	TestEqual(TEXT("pause zeroes the actor's scale"), Actor->GetSimTimeScale(), 0.0, 1e-12);
	Runtime->TogglePause();
	TestEqual(TEXT("unpause restores the previous speed"), Actor->GetSimTimeScale(), 2.0, 1e-12);

	Runtime->Tick(1.0);
	TestTrue(TEXT("ticking the runtime advances the clock"), Runtime->GetClock()->Now() > 0.0);

	// Save, mutate, load: the network comes back and the mesh is rebuilt (measured by
	// triangle count, the same probe MeshFreshnessTest uses).
	const int32 TrisBefore = Actor->SurfaceTriangleCountForTest();
	if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(TEXT("AirportOpsTest_Runtime")))) { return false; }
	Actor->ClearNetwork();
	TestEqual(TEXT("cleared network has no nodes"), Actor->Network->GetNodes().Num(), 0);
	if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(TEXT("AirportOpsTest_Runtime")))) { return false; }
	TestTrue(TEXT("the nodes are back"), Actor->Network->GetNodes().Num() >= 2);
	TestEqual(TEXT("and the surface mesh was rebuilt from them"), Actor->SurfaceTriangleCountForTest(), TrisBefore);
	TestEqual(TEXT("agents do not survive a load - they were never saved"), Actor->GetTraffic()->GetAgentCount(), 0);
```
Name: `"AirportOps.Present.Runtime"`. `ClearNetwork` — check the actor's facade for the clear-all name (`grep -n "Clear" Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h`); the controller's `OnClearNetwork` calls it. Note the AirportOpsTests module must add `"AirportOps"` test headers path: since `OpsEventsTestListener.h` lives in AirportOpsTests/Private, it is already visible.

- [ ] **Step 4: Build to verify failure**

Expected: FAIL, `Present/OpsRuntime.h` not found, `SetSimTimeScale` undeclared.

- [ ] **Step 5: Write UOpsRuntime**

`Public/Present/OpsRuntime.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/SimClock.h"
#include "UObject/Object.h"
#include "OpsRuntime.generated.h"

class ARoadNetworkActor;
class UOpsEvents;
enum class EAgentPhase : uint8;
enum class EArrivalRefusal : uint8;

/**
 * The AirportOps composition root. Owns the clock and the event bus, attaches to the one
 * ARoadNetworkActor, relays Airside delegates onto the bus, pushes the speed multiplier
 * down into the actor each tick, and performs save/load end to end.
 *
 * A UObject rather than the subsystem itself so a test can NewObject one, Attach a spawned
 * actor and Tick it by hand - a UGameInstanceSubsystem needs a UGameInstance, which a
 * CreateWorld test does not have. UOpsRuntimeSubsystem is the forwarder that gives this
 * a lifetime in play. Same split as ARoadNetworkActor (composition root) over
 * UAirsideTraffic (testable subobject), for the same reason.
 *
 * It GROWS BY FORWARDING. Flight board, job board, ledger arrive as further owned
 * subobjects in later milestones; logic lands in them, not here.
 */
UCLASS()
class AIRPORTOPS_API UOpsRuntime : public UObject
{
	GENERATED_BODY()

public:
	UOpsRuntime();

	USimClock* GetClock() const { return Clock; }
	UOpsEvents* GetEvents() const { return Events; }
	ARoadNetworkActor* GetTarget() const { return Target; }

	/** Binds to the actor's traffic delegates. Safe to call again with a new actor (unbinds the old). */
	void Attach(ARoadNetworkActor* Actor);

	/** Advances the clock and pushes the speed multiplier into the actor. Real seconds in. */
	void Tick(double RealDeltaSeconds);

	/** Speed control. StepSpeed(+1) goes X1->X2->X4->X8 and stops; -1 the other way down to X1. */
	void StepSpeed(int32 Delta);
	/** Paused <-> the speed that was set before pausing. */
	void TogglePause();

	bool SaveToSlot(const FString& SlotName);
	/** Restores clock and network, clears agents, rebuilds the actor's mesh. */
	bool LoadFromSlot(const FString& SlotName);

private:
	UPROPERTY() TObjectPtr<USimClock> Clock;
	UPROPERTY() TObjectPtr<UOpsEvents> Events;
	UPROPERTY(Transient) TObjectPtr<ARoadNetworkActor> Target;

	/** What TogglePause returns to. X1 if nothing was ever set. */
	UPROPERTY() ESimSpeed ResumeSpeed = ESimSpeed::X1;

	FDelegateHandle PhaseHandle;
	FDelegateHandle RefusalHandle;

	void Detach();
	void ApplySpeed(ESimSpeed Speed);
	void OnAgentPhase(int32 AgentId, EAgentPhase From, EAgentPhase To);
	void OnArrivalRefused(EArrivalRefusal Why);
};
```

`Private/Present/OpsRuntime.cpp`:
```cpp
#include "Present/OpsRuntime.h"
#include "AirportOpsLog.h"
#include "Model/OpsEvents.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"

UOpsRuntime::UOpsRuntime()
{
	Clock = CreateDefaultSubobject<USimClock>(TEXT("Clock"));
	Events = CreateDefaultSubobject<UOpsEvents>(TEXT("Events"));
}

void UOpsRuntime::Attach(ARoadNetworkActor* Actor)
{
	Detach();
	Target = Actor;
	if (Target == nullptr || Target->GetTraffic() == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("OpsRuntime attached to nothing: no ARoadNetworkActor"));
		return;
	}
	UAirsideTraffic* Traffic = Target->GetTraffic();
	PhaseHandle = Traffic->OnAgentPhaseChanged.AddUObject(this, &UOpsRuntime::OnAgentPhase);
	RefusalHandle = Traffic->OnArrivalRefused.AddUObject(this, &UOpsRuntime::OnArrivalRefused);
	ApplySpeed(Clock->GetSpeed());
	UE_LOG(LogAirportOps, Log, TEXT("OpsRuntime attached to %s"), *Target->GetName());
}

void UOpsRuntime::Detach()
{
	if (Target != nullptr && Target->GetTraffic() != nullptr)
	{
		Target->GetTraffic()->OnAgentPhaseChanged.Remove(PhaseHandle);
		Target->GetTraffic()->OnArrivalRefused.Remove(RefusalHandle);
	}
	Target = nullptr;
}

void UOpsRuntime::Tick(double RealDeltaSeconds)
{
	Clock->Advance(RealDeltaSeconds);
	if (Target != nullptr)
	{
		// The MULTIPLIER, not TimeScale(): movement runs at the player's speed setting,
		// never at the day compression. See USimClock's class comment.
		Target->SetSimTimeScale(USimClock::Multiplier(Clock->GetSpeed()));
	}
}

void UOpsRuntime::ApplySpeed(ESimSpeed Speed)
{
	Clock->SetSpeed(Speed);
	if (Target != nullptr)
	{
		Target->SetSimTimeScale(USimClock::Multiplier(Speed));
	}
	Events->NotifySpeedChanged(Speed);
}

void UOpsRuntime::StepSpeed(int32 Delta)
{
	static const ESimSpeed Ladder[] = { ESimSpeed::X1, ESimSpeed::X2, ESimSpeed::X4, ESimSpeed::X8 };
	int32 Index = 0;
	for (int32 I = 0; I < UE_ARRAY_COUNT(Ladder); ++I)
	{
		if (Ladder[I] == Clock->GetSpeed()) { Index = I; }
	}
	// Stepping while paused resumes at the neighbouring speed of ResumeSpeed, which is
	// what a player pressing "faster" while paused means.
	if (Clock->GetSpeed() == ESimSpeed::Paused)
	{
		for (int32 I = 0; I < UE_ARRAY_COUNT(Ladder); ++I) { if (Ladder[I] == ResumeSpeed) { Index = I; } }
	}
	Index = FMath::Clamp(Index + Delta, 0, static_cast<int32>(UE_ARRAY_COUNT(Ladder)) - 1);
	ResumeSpeed = Ladder[Index];
	ApplySpeed(ResumeSpeed);
}

void UOpsRuntime::TogglePause()
{
	if (Clock->GetSpeed() == ESimSpeed::Paused)
	{
		ApplySpeed(ResumeSpeed);
	}
	else
	{
		ResumeSpeed = Clock->GetSpeed();
		ApplySpeed(ESimSpeed::Paused);
	}
}

void UOpsRuntime::OnAgentPhase(int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	Events->NotifyAgentPhaseChanged(AgentId, From, To);
}

void UOpsRuntime::OnArrivalRefused(EArrivalRefusal Why)
{
	Events->NotifyArrivalRefused(Why);
}

bool UOpsRuntime::SaveToSlot(const FString& SlotName)
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Save refused: no network attached"));
		return false;
	}
	FOpsSnapshot Snapshot;
	OpsSave::Capture(*Clock, *Target->Network, Snapshot);
	const bool bOk = OpsSave::WriteSlot(SlotName, Snapshot);
	Events->NotifyNotification(bOk ? FString::Printf(TEXT("Saved '%s'"), *SlotName)
	                               : FString::Printf(TEXT("Save to '%s' failed"), *SlotName));
	return bOk;
}

bool UOpsRuntime::LoadFromSlot(const FString& SlotName)
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Load refused: no network attached"));
		return false;
	}
	FOpsSnapshot Snapshot;
	if (!OpsSave::ReadSlot(SlotName, Snapshot))
	{
		Events->NotifyNotification(FString::Printf(TEXT("No save '%s'"), *SlotName));
		return false;
	}
	// Agents first: they were never saved, and one mid-taxi on a network about to be
	// replaced would be following a polyline through pavement that no longer exists.
	Target->GetTraffic()->ClearAgents();
	if (!OpsSave::Restore(Snapshot, *Clock, *Target->Network))
	{
		return false;
	}
	// Present rebuilds from model: the mesh and the guideline overlay are derived, and the
	// facade's OnChanged is what a mutator would have fired. RebuildMesh is the actor's
	// public door for exactly this.
	Target->RebuildMesh();
	ApplySpeed(Clock->GetSpeed());
	Events->NotifyNotification(FString::Printf(TEXT("Loaded '%s'"), *SlotName));
	return true;
}
```
Check whether `RebuildMesh` also re-derives guidelines or whether a separate call is needed (`grep -n "RebuildGuidelines\|DeriveGuidelines" Plugins/Airside/Source/Airside/Public/Present/*.h`); if so, call it too. Also check the undo history: after a load, the actor's `URoadEditHistory` stack refers to pre-load snapshots; call its clear function if one exists (`grep -n "Clear\|Reset" Plugins/Airside/Source/Airside/Public/Tool/RoadEditHistory.h`) and note it in the log.

- [ ] **Step 6: Write the subsystem**

`Public/Present/OpsRuntimeSubsystem.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "OpsRuntimeSubsystem.generated.h"

class UOpsRuntime;

/**
 * Gives UOpsRuntime a lifetime in play. A GAME INSTANCE subsystem because it must outlive a
 * level load (save/load, later a main menu); a world subsystem would die with the map.
 * FTickableGameObject because game-instance subsystems do not tick on their own, and the
 * alternative - ticking from the player controller - would tie the sim clock to a pawn.
 *
 * A forwarder only. Nothing here has logic worth a test; the test drives UOpsRuntime.
 * Attaches lazily to the first ARoadNetworkActor it finds in the game instance's world,
 * because the actor is level-resident and does not exist when the subsystem initialises.
 */
UCLASS()
class AIRPORTOPS_API UOpsRuntimeSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return Runtime != nullptr; }
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UOpsRuntimeSubsystem, STATGROUP_Tickables); }
	virtual bool IsTickableInEditor() const override { return false; }

	UOpsRuntime* GetRuntime() const { return Runtime; }

	/** The runtime for a world's game instance, or null (editor worlds have no game instance). */
	static UOpsRuntime* Get(const UWorld* World);

private:
	UPROPERTY() TObjectPtr<UOpsRuntime> Runtime;

	void EnsureAttached();
};
```

`Private/Present/OpsRuntimeSubsystem.cpp`:
```cpp
#include "Present/OpsRuntimeSubsystem.h"
#include "AirportOpsLog.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"

void UOpsRuntimeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Runtime = NewObject<UOpsRuntime>(this, TEXT("OpsRuntime"));
	UE_LOG(LogAirportOps, Log, TEXT("OpsRuntimeSubsystem initialised"));
}

void UOpsRuntimeSubsystem::Deinitialize()
{
	Runtime = nullptr;
	Super::Deinitialize();
}

void UOpsRuntimeSubsystem::EnsureAttached()
{
	if (Runtime->GetTarget() != nullptr)
	{
		return;
	}
	UWorld* World = GetGameInstance() != nullptr ? GetGameInstance()->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return;
	}
	for (TActorIterator<ARoadNetworkActor> It(World); It; ++It)
	{
		Runtime->Attach(*It);
		break;
	}
}

void UOpsRuntimeSubsystem::Tick(float DeltaTime)
{
	EnsureAttached();
	Runtime->Tick(DeltaTime);
}

UOpsRuntime* UOpsRuntimeSubsystem::Get(const UWorld* World)
{
	if (World == nullptr || World->GetGameInstance() == nullptr)
	{
		return nullptr;
	}
	UOpsRuntimeSubsystem* Sub = World->GetGameInstance()->GetSubsystem<UOpsRuntimeSubsystem>();
	return Sub != nullptr ? Sub->GetRuntime() : nullptr;
}
```
Note: `Tick(float DeltaTime)` receives real seconds already scaled by the engine's global time dilation; the project does not use dilation, so it is real time.

- [ ] **Step 7: Keys and HUD in the game module**

`RoadBuildController.h` declarations (beside `OnUndo`):
```cpp
	/** Sim clock. Comma slower, Period faster, P pause; F5 save, F9 load. Only in play: the ed mode has no game instance. */
	void OnSpeedDown();
	void OnSpeedUp();
	void OnTogglePause();
	void OnQuickSave();
	void OnQuickLoad();
```
`RoadBuildController.cpp` in `SetupInputComponent` after the `OnRedo` bind:
```cpp
	InputComponent->BindKey(EKeys::Comma, IE_Pressed, this, &ARoadBuildController::OnSpeedDown);
	InputComponent->BindKey(EKeys::Period, IE_Pressed, this, &ARoadBuildController::OnSpeedUp);
	InputComponent->BindKey(EKeys::P, IE_Pressed, this, &ARoadBuildController::OnTogglePause);
	InputComponent->BindKey(EKeys::F5, IE_Pressed, this, &ARoadBuildController::OnQuickSave);
	InputComponent->BindKey(EKeys::F9, IE_Pressed, this, &ARoadBuildController::OnQuickLoad);
```
Handlers:
```cpp
namespace
{
	UOpsRuntime* RuntimeFor(const APlayerController& PC)
	{
		UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(PC.GetWorld());
		if (Runtime == nullptr)
		{
			UE_LOG(LogRoadBuild, Warning, TEXT("No OpsRuntime: clock keys need a game instance (PIE), not the editor mode"));
		}
		return Runtime;
	}
}

void ARoadBuildController::OnSpeedDown()   { if (UOpsRuntime* R = RuntimeFor(*this)) { R->StepSpeed(-1); } }
void ARoadBuildController::OnSpeedUp()     { if (UOpsRuntime* R = RuntimeFor(*this)) { R->StepSpeed(+1); } }
void ARoadBuildController::OnTogglePause() { if (UOpsRuntime* R = RuntimeFor(*this)) { R->TogglePause(); } }
void ARoadBuildController::OnQuickSave()   { if (UOpsRuntime* R = RuntimeFor(*this)) { R->SaveToSlot(TEXT("QuickSave")); } }
void ARoadBuildController::OnQuickLoad()   { if (UOpsRuntime* R = RuntimeFor(*this)) { R->LoadFromSlot(TEXT("QuickSave")); } }
```
Includes: `#include "Present/OpsRuntime.h"`, `#include "Present/OpsRuntimeSubsystem.h"`, `#include "Model/SimClock.h"`. Extend the "Road building ready" banner `UE_LOG` text with `, comma/period speed, P pause, F5/F9 save/load`.

Check the digit keys the tool registry binds do not include Comma/Period/P/F5/F9: `grep -rn "EKeys::" Plugins/Airside/Source/Airside/Private/Tool/*.cpp Source/AirportMgr/*.cpp | grep -v "LeftMouse\|RightMouse\|W)\|A)\|S)\|D)\|Q)\|E)"`.

`RoadBuildHUD.cpp`, inside the `bDrawToolName` block after the tool-name `DrawText`, add:
```cpp
			if (UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld()))
			{
				const USimClock* Clock = Runtime->GetClock();
				const int32 Hour = static_cast<int32>(Clock->TimeOfDay() / 3600.0);
				const int32 Minute = static_cast<int32>(FMath::Fmod(Clock->TimeOfDay(), 3600.0) / 60.0);
				const FString Line = FString::Printf(TEXT("Day %d  %02d:%02d  x%.0f%s"),
					Clock->Day() + 1, Hour, Minute, USimClock::Multiplier(Clock->GetSpeed()),
					Clock->GetSpeed() == ESimSpeed::Paused ? TEXT(" PAUSED") : TEXT(""));
				DrawText(Line, PendingColour, 24.0f, 48.0f, GEngine->GetSmallFont(), 1.1f);
			}
```
The clock line is inside the tool-name block deliberately: same toggle, same corner.

- [ ] **Step 8: Build, run all**

Build. `./Tools/Run-AirsideTests.ps1`. Expected: `0 failed, 0 crashed`, with `Airside.Present.SimTimeScale` and `AirportOps.Present.Runtime` listed.

- [ ] **Step 9: Commit**

```bash
git add Plugins Source
git commit -m "feat(ops): UOpsRuntime composition root, subsystem tick, speed into Airside, keys and HUD clock"
```

---

### Task 9: Definitions, catalog, settings

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsDefinition.h`, `Private/Model/OpsDefinition.cpp`
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsCatalog.h`, `Private/Model/OpsCatalog.cpp`
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Content/AirportOpsSettings.h`, `Private/Content/AirportOpsSettings.cpp`
- Create: `Plugins/AirportOps/Source/AirportOpsTests/Private/OpsCatalogTest.cpp`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Present/OpsRuntime.h/.cpp` (owns catalog; applies scenario)
- Modify: `Config/DefaultGame.ini`

**Interfaces:**
- Produces: `UOpsDefinition : UPrimaryDataAsset` (primary asset type = class name); `UScenario : UOpsDefinition { double StartingBalance; double RealSecondsPerGameDay; }`; `UOpsCatalog` with `void Add(UOpsDefinition*)`, `template<class T> TArray<T*> All() const`, `template<class T> T* Find(FName) const`, `int32 LoadFromAssetManager()`; `UAirportOpsSettings : UDeveloperSettings { TSoftObjectPtr<UScenario> DefaultScenario; static const UScenario* ResolveDefaultScenario(); }`. `UOpsRuntime::GetCatalog()`.

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/OpsCatalog.h"
#include "Model/OpsDefinition.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsCatalogTest,
	"AirportOps.Model.Catalog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsCatalogTest::RunTest(const FString& Parameters)
{
	UOpsCatalog* Catalog = NewObject<UOpsCatalog>();
	UScenario* Easy = NewObject<UScenario>(GetTransientPackage(), TEXT("Easy"));
	Easy->StartingBalance = 900000.0;
	UScenario* Hard = NewObject<UScenario>(GetTransientPackage(), TEXT("Hard"));
	Hard->StartingBalance = 100000.0;

	Catalog->Add(Easy);
	Catalog->Add(Hard);
	Catalog->Add(Hard);  // duplicates are ignored, not doubled
	Catalog->Add(nullptr);

	TestEqual(TEXT("All<UScenario> lists each definition once"), Catalog->All<UScenario>().Num(), 2);
	UScenario* Found = Catalog->Find<UScenario>(TEXT("Hard"));
	if (TestNotNull(TEXT("Find by asset name"), Found))
	{
		TestEqual(TEXT("returns the right one"), Found->StartingBalance, 100000.0, 1e-9);
	}
	TestNull(TEXT("an unknown name is null, not a crash"), Catalog->Find<UScenario>(TEXT("Nope")));

	TestEqual(TEXT("the primary asset type is the class name, so the Asset Manager can scan per type"),
		Easy->GetPrimaryAssetId().PrimaryAssetType.GetName(), FName(TEXT("Scenario")));
	TestEqual(TEXT("and the asset name is the id"), Easy->GetPrimaryAssetId().PrimaryAssetName, FName(TEXT("Easy")));
	return true;
}

#endif
```

- [ ] **Step 2: Build to verify failure**

Expected: FAIL, `Model/OpsCatalog.h` not found.

- [ ] **Step 3: Write the definitions**

`Public/Model/OpsDefinition.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "OpsDefinition.generated.h"

/**
 * Base of every authored definition in AirportOps: scenarios, and in later milestones
 * vehicles, buildings, airlines, cargo classes, research nodes, contract templates.
 *
 * A PRIMARY data asset so the Asset Manager can enumerate them by type without loading
 * the world, which is how UOpsCatalog fills itself on a cold start. The primary asset
 * TYPE is the class name minus its prefix ("Scenario" for UScenario), so DefaultGame.ini
 * needs one PrimaryAssetTypesToScan entry per subclass. Only the subclasses that exist
 * are declared: spec §2.2 lists more, and they arrive with the milestone that reads them.
 */
UCLASS(Abstract, BlueprintType)
class AIRPORTOPS_API UOpsDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;
};

/** A new-game setup. Difficulty is these numbers and nothing else (spec §5.2). */
UCLASS(BlueprintType)
class AIRPORTOPS_API UScenario : public UOpsDefinition
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "0.0"))
	double StartingBalance = 500000.0;

	/** Real seconds one game day takes at x1. Copied into USimClock by UOpsRuntime at attach. */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "1.0"))
	double RealSecondsPerGameDay = 1200.0;
};
```
`Private/Model/OpsDefinition.cpp`:
```cpp
#include "Model/OpsDefinition.h"

FPrimaryAssetId UOpsDefinition::GetPrimaryAssetId() const
{
	// GetClass()->GetFName() would be "Scenario" already: UHT strips the U prefix from the
	// reflected class name. Stated because it is easy to expect "UScenario" here.
	return FPrimaryAssetId(GetClass()->GetFName(), GetFName());
}
```

- [ ] **Step 4: Write the catalog**

`Public/Model/OpsCatalog.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/OpsDefinition.h"
#include "UObject/Object.h"
#include "OpsCatalog.generated.h"

/**
 * Every loaded definition, by class and by name. The one place content defaults are
 * resolved from (CLAUDE.md "Content/": a literal asset path at a second site is a second
 * source of truth).
 *
 * World-free: tests Add() NewObject'd definitions. In play, UOpsRuntime calls
 * LoadFromAssetManager once, which needs DefaultGame.ini's PrimaryAssetTypesToScan.
 */
UCLASS()
class AIRPORTOPS_API UOpsCatalog : public UObject
{
	GENERATED_BODY()

public:
	/** Ignores null and duplicates. */
	void Add(UOpsDefinition* Definition);

	template<class T>
	TArray<T*> All() const
	{
		TArray<T*> Out;
		for (const TObjectPtr<UOpsDefinition>& D : Definitions)
		{
			if (T* Typed = Cast<T>(D.Get())) { Out.Add(Typed); }
		}
		return Out;
	}

	template<class T>
	T* Find(FName AssetName) const
	{
		for (const TObjectPtr<UOpsDefinition>& D : Definitions)
		{
			if (D != nullptr && D->GetFName() == AssetName)
			{
				if (T* Typed = Cast<T>(D.Get())) { return Typed; }
			}
		}
		return nullptr;
	}

	int32 Num() const { return Definitions.Num(); }

	/**
	 * Loads every primary asset of every registered subclass type synchronously and Adds it.
	 * Returns how many were added. Logs the per-type count: "0 Scenario(s)" in the log is the
	 * answer to "why does the game use the fallback numbers".
	 */
	int32 LoadFromAssetManager();

private:
	UPROPERTY(Transient) TArray<TObjectPtr<UOpsDefinition>> Definitions;
};
```
`Private/Model/OpsCatalog.cpp`:
```cpp
#include "Model/OpsCatalog.h"
#include "AirportOpsLog.h"
#include "Engine/AssetManager.h"

void UOpsCatalog::Add(UOpsDefinition* Definition)
{
	if (Definition == nullptr || Definitions.Contains(Definition))
	{
		return;
	}
	Definitions.Add(Definition);
}

int32 UOpsCatalog::LoadFromAssetManager()
{
	if (!UAssetManager::IsInitialized())
	{
		UE_LOG(LogAirportOps, Warning, TEXT("OpsCatalog: Asset Manager not initialised; nothing loaded"));
		return 0;
	}
	UAssetManager& Manager = UAssetManager::Get();
	int32 Added = 0;

	// Every concrete subclass of UOpsDefinition is a type to scan. Derived from the class
	// hierarchy rather than a hand-kept list so a new definition class cannot be forgotten
	// here - though it CAN still be forgotten in DefaultGame.ini, which the per-type log
	// line below is there to show.
	TArray<UClass*> Types;
	GetDerivedClasses(UOpsDefinition::StaticClass(), Types, /*bRecursive*/ true);
	for (UClass* Type : Types)
	{
		if (Type->HasAnyClassFlags(CLASS_Abstract)) { continue; }
		const FPrimaryAssetType AssetType(Type->GetFName());
		TArray<FPrimaryAssetId> Ids;
		Manager.GetPrimaryAssetIdList(AssetType, Ids);
		int32 ForType = 0;
		for (const FPrimaryAssetId& Id : Ids)
		{
			const FSoftObjectPath Path = Manager.GetPrimaryAssetPath(Id);
			if (UOpsDefinition* Loaded = Cast<UOpsDefinition>(Path.TryLoad()))
			{
				Add(Loaded);
				++ForType;
				++Added;
			}
		}
		UE_LOG(LogAirportOps, Log, TEXT("OpsCatalog: %d %s(s)"), ForType, *Type->GetName());
	}
	return Added;
}
```

- [ ] **Step 5: Settings and config**

`Public/Content/AirportOpsSettings.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AirportOpsSettings.generated.h"

class UScenario;

/**
 * Project Settings > Game > AirportOps. The one place the default scenario is named; every
 * caller goes through ResolveDefaultScenario. Mirrors UAirsideSettings for the same reason
 * it exists: a path typed at a second call site is a second source of truth.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "AirportOps"))
class AIRPORTOPS_API UAirportOpsSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(config, EditAnywhere, Category = "Content")
	TSoftObjectPtr<UScenario> DefaultScenario;

	/**
	 * The configured scenario, loaded on first use, or null when none is set. Null is a
	 * SUPPORTED state - the clock and balance fall back to their constructor defaults and
	 * the log says so once. A configured scenario that fails to load is an error and is
	 * logged as one.
	 */
	static const UScenario* ResolveDefaultScenario();
};
```
`Private/Content/AirportOpsSettings.cpp`:
```cpp
#include "Content/AirportOpsSettings.h"
#include "AirportOpsLog.h"
#include "Model/OpsDefinition.h"

const UScenario* UAirportOpsSettings::ResolveDefaultScenario()
{
	const UAirportOpsSettings* Settings = GetDefault<UAirportOpsSettings>();
	if (Settings->DefaultScenario.IsNull())
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogAirportOps, Log, TEXT("No DefaultScenario configured; clock and balance use built-in defaults"));
		}
		return nullptr;
	}
	const UScenario* Scenario = Settings->DefaultScenario.LoadSynchronous();
	if (Scenario == nullptr)
	{
		UE_LOG(LogAirportOps, Error, TEXT("DefaultScenario '%s' is configured but failed to load"),
			*Settings->DefaultScenario.ToString());
	}
	return Scenario;
}
```
`Config/DefaultGame.ini`, append:
```ini
[/Script/Engine.AssetManagerSettings]
+PrimaryAssetTypesToScan=(PrimaryAssetType="Scenario",AssetBaseClass="/Script/AirportOps.Scenario",bHasBlueprintClasses=False,bIsEditorOnly=False,Directories=((Path="/Game/Ops")),SpecificAssets=,Rules=(Priority=-1,ChunkId=-1,bApplyRecursively=True,CookRule=AlwaysCook))
```

- [ ] **Step 6: Give UOpsRuntime the catalog and apply the scenario**

`OpsRuntime.h`: add `class UOpsCatalog;`, `UOpsCatalog* GetCatalog() const { return Catalog; }`, private `UPROPERTY() TObjectPtr<UOpsCatalog> Catalog;`. Constructor: `Catalog = CreateDefaultSubobject<UOpsCatalog>(TEXT("Catalog"));`. In `Attach`, after the target is set and before `ApplySpeed`:
```cpp
	// Content is resolved ONCE, here, and applied to the clock. Balance goes to the ledger
	// when it exists (M3); until then the scenario's day length is the only field consumed.
	if (Catalog->Num() == 0)
	{
		Catalog->LoadFromAssetManager();
	}
	if (const UScenario* Scenario = UAirportOpsSettings::ResolveDefaultScenario())
	{
		Clock->RealSecondsPerGameDay = Scenario->RealSecondsPerGameDay;
		UE_LOG(LogAirportOps, Log, TEXT("Scenario '%s': %.0f real s per game day"),
			*Scenario->GetName(), Scenario->RealSecondsPerGameDay);
	}
```
Includes: `Model/OpsCatalog.h`, `Model/OpsDefinition.h`, `Content/AirportOpsSettings.h`.

- [ ] **Step 7: Build, run all**

`./Tools/Run-AirsideTests.ps1`. Expected: `0 failed, 0 crashed`, `AirportOps.Model.Catalog` listed. In the log, `OpsCatalog: 0 Scenario(s)` is expected until an asset is authored.

- [ ] **Step 8: Commit**

```bash
git add Plugins/AirportOps Config/DefaultGame.ini
git commit -m "feat(ops): definitions base, UScenario, catalog via Asset Manager, settings"
```

---

### Task 10: Spec amendments, full verification, PR

**Files:**
- Modify: `docs/superpowers/specs/2026-09-05-game-systems-map-design.md` §2.0, §2.1, §2.3

- [ ] **Step 1: Amend the spec (reasoned changes, per §0)**

§2.0 bullet "A ground-vehicle dispatch shaped as ...": replace with
```
- Agent primitives: `RedirectAgent(id, plan)` and `RetireAgent(id)` beside the existing
  `DispatchAgent`, plus a stable per-agent id. *Amended 2026-09-05 (M1):* the first draft
  asked Airside for "go to anchor, dwell, return" as one call. Dwell is a fact about the
  job, so that shape belongs in the job board; Airside offers the three primitives and the
  phase events, and AirportOps composes them.
```
§2.1: after "Airside's Advance stays wall-scaled; the game module multiplies by speed" add
```
  *Clarified (M1):* "speed" is the ×0/1/2/4/8 multiplier only. Day compression
  (real seconds per game day) scales the clock, never movement; a truck driving 72× faster
  because the day is twenty real minutes would be unwatchable.
```
§2.3: replace the "Rule from commit one: every new model field is UPROPERTY(SaveGame)" bullet with
```
- *Amended 2026-09-05 (M1):* the archive does not set `ArIsSaveGame`. `FProperty::
  ShouldSerializeValue` (Property.cpp:1052) would then skip every untagged property,
  nested struct members included, so honouring "tag every field" meant tagging all of
  Airside. Rule instead: **a model object's non-Transient UPROPERTYs are its saved
  state**; mark what must not be saved `Transient`. Forgetting a Transient saves one
  field too many (visible); forgetting a SaveGame would lose one (silent).
```

- [ ] **Step 2: Full build and full test run**

Build line; then `./Tools/Run-AirsideTests.ps1`. Record both lines verbatim. `UE_LOG(` count across `Plugins/Airside/Source/Airside`: compare `git diff main --stat` files; run `git grep -c "UE_LOG(" main -- Plugins/Airside/Source/Airside | awk -F: '{s+=$2} END {print s}'` vs the same on HEAD. HEAD must be ≥ main.

- [ ] **Step 3: Runtime check in PIE (user, or MCP shot)**

Ask the user to open the editor, PIE, press Period twice and P once, then F5. The log should show, in order: `OpsRuntimeSubsystem initialised`, `OpsRuntime attached to RoadNetworkActor...`, `Sim speed x2`, `Sim speed x4`, `Sim speed x0`, `Captured snapshot: ...`, `Save to slot 'QuickSave': ok`. The HUD's second line reads `Day 1  HH:MM  x4 PAUSED`. If the `unreal` MCP is up, `python Tools/Mcp.py log LogAirportOps` and `python Tools/Mcp.py shot out.png` gather the same evidence.

- [ ] **Step 4: Commit and PR**

```bash
git add docs
git commit -m "docs: M1 spec amendments - agent primitives, speed vs compression, save rule"
git push -u origin feature/m1-foundation
gh pr create --title "feat: M1 foundation - AirportOps plugin, clock, events, save/load, Airside seam" --body-file <(...)  # fill the PR template: build line, test line, UE_LOG count before/after, seams and their tests, runtime evidence or "builds, unverified at runtime"
```

---

## Self-review

**Spec coverage.** §2.0 module layout: Task 1 (plugin, guard), Tasks 4-6 (seam: events, primitives, capability). §2.1 clock: Task 2, wiring into Airside Task 8. §2.2 definitions and catalog: Task 9 (base + UScenario only; other subclasses deferred to their milestones, stated in the header). §2.3 save/load: Task 7 model, Task 8 end to end, amendment Task 10. §2.4 events: Task 3, relayed Task 8. §5.4 testing: every Model class has a NewObject test; seams have composition tests (ArrivalDispatch records phases, AgentRedirectRetire, Runtime end to end, SimTimeScale); determinism test ("same seed, same inputs, same ledger") has no ledger yet and is deferred to M3 with the ledger. Check-Architecture: Task 1.

**Placeholders.** None: every step has code. Three points tell the executor to `grep` for an accessor and add it if missing, with the exact signature to add.

**Type consistency.** `ESimSpeed` (Task 2) used in Tasks 3, 8, 9. `FOnAgentPhaseChanged` params `(int32, EAgentPhase, EAgentPhase)` in Tasks 4, 5, 8. `FOpsSnapshot` fields `Version/Clock/Network` in Tasks 7, 8. `GetTraffic()` introduced Task 4, used 5, 8. `LastAgentPositionForTest` introduced Task 5, used 8. `DesignWingspan` param Task 6 only. `UOpsEventsTestListener` header from Task 3 reused Task 8.
