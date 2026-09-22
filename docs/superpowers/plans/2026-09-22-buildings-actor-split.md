# Buildings Actor Split Implementation Plan (chainlink fence, PR 1 of 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move plot presentation (`UPlotPresenter` + its two ISMs) off `ARoadNetworkActor` onto a new `AAirsideBuildingsActor`, with no visible behaviour change.

**Architecture:** `ARoadNetworkActor` broadcasts a native multicast `OnTopologyRebuilt(const URoadNetwork&)` where it used to call `Plots->RebuildFrom`. `AAirsideBuildingsActor` binds to it (resolved pointer, else the single road actor in the world), and rebuilds once on binding to catch up. The road actor stops knowing plots exist. The presenter stops reaching its outer for kits; the caller passes them.

**Tech Stack:** UE 5.8.2 C++, Airside plugin, UE automation tests, headless Python commandlet for the level edit.

**Spec:** `docs/superpowers/specs/2026-09-22-chainlink-fence-design.md`

## Global Constraints

- The editor must be CLOSED to build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex`. New UCLASS and UPROPERTYs, so Live Coding is not enough.
- Tests: `./Tools/Run-AirsideTests.ps1 -Filter <prefix>`. Read its `N test(s) run, N failed, N crashed` line; never the exit code.
- `./Tools/Check-Architecture.ps1` must pass (it runs first inside the test script).
- The refactor contract (CLAUDE.md): no `UE_LOG(` removed; comment-line count in touched files must not fall; WHY comments travel with their code; every new seam gets a composition-level test.
- A doc comment touches its declaration.
- Log category `LogAirside` (from `AirsideLog.h`), no new categories.
- Commit messages are concise, with no Co-Authored-By trailer (user rule).
- Branch: `feature/chainlink-fence` (already created, spec committed).

## Deviation from the spec, decided while planning

The spec said "the road actor's plot accessors stay as forwarders". **They don't.** `ARoadNetworkActor::GetPlotPresenter()` is plain C++ (not a UFUNCTION, not an interface virtual), so the refactor contract doesn't require it. A forwarder would need a road→buildings back-pointer, which undoes the split the user asked for. Its 21 callers are all tests, in `PlotPresenterTest.cpp` and `PlotPlaceToolTest.cpp`, and they move to `TestWorld.Buildings->GetPlotPresenter()`. Task 6 amends the spec to match.

**Where the actor gets created.** A level with no buildings actor draws no plots, so every place that creates a road actor also makes sure the buildings actor exists:

| Site | Change |
|---|---|
| `FAirsideTestWorld` (Public/Testing/AirsideTestWorld.h) | spawns `Buildings` after `Actor` when `bSpawnActor` |
| `URoadBuildEdMode::MakeReselectContext` (AirsideEditor/Private/RoadBuildEdMode.cpp:245) | `AAirsideBuildingsActor::FindOrCreate(World, Road)` |
| `URoadBuildEditorTool::ResolveTarget` (AirsideEditor/Private/RoadBuildEditorTool.cpp:268) | same |
| `ARoadBuildController::BeginPlay` (Source/AirportMgr/RoadBuildController.cpp:52) | same, after `Target` is found |
| `M_Starter.umap` | placed and saved by a headless script (Task 5) |

**The saved-component hazard.** `M_Starter.umap` contains a serialised `PlotBoxes` (measured: `grep -c -a PlotBoxes` = 1; `M_ModelYard` = 0). `AActor::ResetOwnedComponents` collects components by outer, so an orphaned ISM that loses its native property can still register and render stale boxes. Task 3 adds a named sweep that destroys it, and Task 5's resave drops it from disk.

## File Structure

| File | Responsibility |
|---|---|
| Create `Plugins/Airside/Source/Airside/Public/Present/AirsideBuildingsActor.h` | the actor: find/create, bind, own presenter + ISMs |
| Create `Plugins/Airside/Source/Airside/Private/Present/AirsideBuildingsActor.cpp` | its implementation |
| Modify `Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h` / `.cpp` | `RebuildFrom` takes the kits; `Actor()` removed |
| Modify `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h` / `.cpp` | plot members out, delegate in, legacy sweep |
| Modify `Plugins/Airside/Source/Airside/Public/Testing/AirsideTestWorld.h` | `Buildings` field |
| Create `Plugins/Airside/Source/AirsideTests/Private/BuildingsActorTest.cpp` | the seam tests |
| Modify `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp`, `PlotPlaceToolTest.cpp` | call sites |
| Modify `Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEdMode.cpp`, `RoadBuildEditorTool.cpp`, `Source/AirportMgr/RoadBuildController.cpp` | ensure-created |
| Create `Tools/Python/place_buildings_actor.py` | headless level edit |

A new test .cpp needs two builds (memory: the first says Succeeded without compiling it). The Task 4 steps build twice.

---

### Task 0: Baseline measurements

**Files:** none modified.

- [ ] **Step 1: Record the refactor-contract baselines**

```bash
cd /c/repos/AirportMgr2/Plugins/Airside/Source/Airside
for f in Public/Present/RoadNetworkActor.h Private/Present/RoadNetworkActor.cpp Public/Present/PlotPresenter.h Private/Present/PlotPresenter.cpp; do
  printf "%s logs=%s comments=%s\n" "$f" "$(grep -c 'UE_LOG(' $f)" "$(grep -cE '^\s*(//|/\*|\*)' $f)"
done | tee /c/Users/daren/AppData/Local/Temp/claude/C--repos-AirportMgr2/31fbe335-45a7-4423-a207-464bed3aec3c/scratchpad/baseline.txt
```

- [ ] **Step 2: Record the test baseline**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present` then `-Filter Airside.Tool.Plot`
Expected: record both `N test(s) run, N failed, N crashed` lines in the scratchpad `baseline.txt`. Any failure here predates the work and must be named in the PR.

---

### Task 1: `UPlotPresenter::RebuildFrom` takes its kits

The presenter reached its outer (`GetTypedOuter<ARoadNetworkActor>`) only for `ResolveDepotKits()`. Passing the kits in removes the presenter's only coupling to whichever actor owns it.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h:7,42-46,107-118`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp:10,108-117,143,165-174`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp:773`

**Interfaces:**
- Produces: `void UPlotPresenter::RebuildFrom(const URoadNetwork& Network, TArrayView<const PlotYard::FKitSpec> Specs);` and `void UPlotPresenter::Clear();`

- [ ] **Step 1: Change the header**

In `PlotPresenter.h`, add `#include "Solve/PlotYard.h"` after `#include "UObject/Object.h"`. Replace the `RebuildFrom` declaration and its comment with:

```cpp
	/**
	 * Clear and re-add an instance per module standing where the yard solver put it, plus a
	 * fence round every plot's outline.
	 *
	 * THE KITS ARE HANDED IN, not looked up. This used to reach its owning ARoadNetworkActor
	 * for ResolveDepotKits (issue #181), which tied the presenter to whichever actor happened
	 * to be its outer - and the buildings split moved that outer. The caller resolves through
	 * the ONE resolver, ARoadNetworkActor::ResolveDepotKits, so issue #181's second-resolution
	 * drift stays impossible: this class has no way to resolve them itself.
	 */
	void RebuildFrom(const URoadNetwork& Network, TArrayView<const PlotYard::FKitSpec> Specs);
```

Delete the `ARoadNetworkActor& Actor() const;` declaration and its whole doc comment (lines 107-118), and the `class ARoadNetworkActor;` forward declaration (line 7). The #181 reasoning in that comment now lives in the `RebuildFrom` comment above, so no WHY is lost.

- [ ] **Step 2: Change the .cpp**

In `PlotPresenter.cpp`, delete `#include "Present/RoadNetworkActor.h"` and the whole `UPlotPresenter::Actor()` definition (lines 108-117). Change the signature at line 143 to:

```cpp
void UPlotPresenter::RebuildFrom(const URoadNetwork& Network,
	TArrayView<const PlotYard::FKitSpec> Specs)
```

Replace lines 165-174 (the HOISTED/THROUGH THE ACTOR comment and `const TArray<PlotYard::FKitSpec> Specs = Actor().ResolveDepotKits();`) with this comment only; `Specs` is now the parameter:

```cpp
	// RESOLVED ONCE BY THE CALLER, not per plot: the specs are the same for every plot, and
	// resolving the same three kits once per depot would do the work once per building on
	// the airport. NULL CONTENT IS STILL A LEGAL ANSWER and the tests rely on it:
	// ARoadNetworkActor::ResolveDepotKits falls every kit back to the grey-box table.
```

`PlotLayoutFor(Layout)->Solve(PlotSite, Specs)` already takes `TArrayView<const PlotYard::FKitSpec>` (verified at `PlotLayoutStrategy.h:56-57`), so it needs no change.

Also add a `Clear()`, which the buildings actor needs for "bound to nothing". In the header, after `RebuildFrom`:

```cpp
	/**
	 * Empty every component and zero every count, as if rebuilt from an airport with no plots.
	 *
	 * FOR AN OWNER WITH NOTHING TO DRAW FROM - a buildings actor whose road network is gone.
	 * RebuildFrom starts with exactly this, so the two cannot disagree about what "empty" is.
	 */
	void Clear();
```

In the .cpp, move `RebuildFrom`'s opening block (from `Boxes->ClearInstances();` through `Ghosts = 0;`) into:

```cpp
void UPlotPresenter::Clear()
{
	if (Boxes != nullptr)
	{
		Boxes->ClearInstances();
	}
	if (GhostBoxes != nullptr)
	{
		GhostBoxes->ClearInstances();
	}
	Placed.Reset();
	GateGaps = 0;
	RoomForMore = 0;
	ModuleBoxes = 0;
	Dropped = 0;
	Ghosts = 0;
}
```

In `RebuildFrom`, after the `if (Boxes == nullptr) { return; }` guard, call `Clear();` in place of the moved lines. Keep the "CLEARED AND REBUILT WHOLE" comment above the call.

- [ ] **Step 3: Update the one caller**

`RoadNetworkActor.cpp:773`: `Plots->RebuildFrom(*Network);` becomes `Plots->RebuildFrom(*Network, ResolveDepotKits());`

- [ ] **Step 4: Build**

Run the Build.bat line from Global Constraints.
Expected: `Result: Succeeded`.

- [ ] **Step 5: Run the plot tests**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present.Plot`
Expected: the same run/failed/crashed line as the Task 0 baseline for this subset.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp
git commit -m "refactor(plots): RebuildFrom takes its kits; presenter no longer reaches its outer"
```

---

### Task 2: `AAirsideBuildingsActor`, with the seam tests first

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Present/AirsideBuildingsActor.h`
- Create: `Plugins/Airside/Source/Airside/Private/Present/AirsideBuildingsActor.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h` (delegate only)
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp` (broadcast only)
- Create: `Plugins/Airside/Source/AirsideTests/Private/BuildingsActorTest.cpp`

**Interfaces:**
- Consumes: `UPlotPresenter::RebuildFrom(const URoadNetwork&, TArrayView<const PlotYard::FKitSpec>)` (Task 1); `ARoadNetworkActor::ResolveDepotKits()`, `ResolveGhostMaterial()`, `Network`, `Find(const UWorld*)` (all existing and public).
- Produces:
  - `ARoadNetworkActor::FOnTopologyRebuilt` (`DECLARE_MULTICAST_DELEGATE_OneParam(FOnTopologyRebuilt, const URoadNetwork&)`) and the public member `FOnTopologyRebuilt OnTopologyRebuilt;`
  - `AAirsideBuildingsActor::Find(const UWorld*)`, `FindOrCreate(UWorld*, ARoadNetworkActor*)`, `BindTo(ARoadNetworkActor*)`, `GetRoadNetwork() const`, `GetPlotPresenter() const`, and the `UPROPERTY RoadNetwork`.

In this task the road actor still ALSO draws plots itself. Task 3 removes that. The two coexist for one commit so this task's tests can go red, then green, on the new seam alone.

- [ ] **Step 1: Write the failing tests**

Create `BuildingsActorTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A 12 m x 8 m three-module depot, gate at the frontage midpoint, placed through the model.
	 *
	 * NAMED FOR THIS FILE, not PlaceDepot: the tests module is a UNITY build, and
	 * PlotPresenterTest.cpp already has a PlaceDepot in its own anonymous namespace. Two of
	 * one name compile alone and collide together.
	 */
	void PlaceSeamTestDepot(ARoadNetworkActor* Road)
	{
		UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(600.0, 0.0);
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = { FVector2D(0.0, 0.0), FVector2D(1200.0, 0.0),
		                      FVector2D(1200.0, 800.0), FVector2D(0.0, 800.0) };
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		Road->Network->PlaceEntity(Placement);
	}
}

/**
 * The buildings actor draws a depot the road network holds, through the delegate alone.
 *
 * THE SEAM TEST, per CLAUDE.md's refactor contract: OnTopologyRebuilt replaced a direct
 * Plots->RebuildFrom call, and a delegate nobody bound passes every model test while the
 * airport's depots silently vanish. Spawned by hand rather than by FAirsideTestWorld so the
 * test owns the order - road first, depot placed, THEN the buildings actor - which is also
 * what proves the catch-up rebuild on binding.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingsActorDrawsThroughTheDelegateTest,
	"Airside.Present.BuildingsActorDrawsThroughTheDelegate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildingsActorDrawsThroughTheDelegateTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	ARoadNetworkActor* Road = TestWorld.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("a road network"), Road)) { return false; }
	Road->ClearNetwork();
	PlaceSeamTestDepot(Road);
	Road->RebuildMesh();

	AAirsideBuildingsActor* Buildings = TestWorld.World->SpawnActor<AAirsideBuildingsActor>();
	if (!TestNotNull(TEXT("a buildings actor"), Buildings)) { return false; }

	// FOUND WITHOUT BEING TOLD: the pointer was left empty, and there is one road network.
	TestEqual(TEXT("it bound to the only road network in the world"),
		Buildings->GetRoadNetwork(), Road);

	// CAUGHT UP ON BINDING. The depot was placed and rebuilt before this actor existed, so
	// the only way it can be standing is the rebuild BindTo runs itself.
	const UPlotPresenter* Plots = Buildings->GetPlotPresenter();
	if (!TestNotNull(TEXT("a plot presenter"), Plots)) { return false; }
	TestEqual(TEXT("the depot already standing is drawn on binding"), Plots->GetModuleCount(), 3);

	// AND FOLLOWS THE DELEGATE AFTERWARDS. Clearing the network and rebuilding must empty it;
	// an actor that drew once on binding and never listened again would keep the old depot.
	Road->ClearNetwork();
	Road->RebuildMesh();
	TestEqual(TEXT("a topology rebuild reaches it through the delegate"),
		Plots->GetModuleCount(), 0);

	return true;
}

/**
 * With no road network, the buildings actor draws nothing and does not crash.
 *
 * THE LEVEL THAT FORGOT ONE. A null dereference here would take the editor down on opening
 * any level that has a buildings actor and no road network; the Warning it logs instead is
 * what says why a depot is missing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingsActorAloneDrawsNothingTest,
	"Airside.Present.BuildingsActorAloneDrawsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildingsActorAloneDrawsNothingTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }

	AAirsideBuildingsActor* Buildings = TestWorld.World->SpawnActor<AAirsideBuildingsActor>();
	if (!TestNotNull(TEXT("a buildings actor"), Buildings)) { return false; }

	TestNull(TEXT("no road network to bind to"), Buildings->GetRoadNetwork());
	TestEqual(TEXT("and nothing drawn"), Buildings->GetPlotPresenter()->GetInstanceCount(), 0);
	return true;
}

/**
 * FindOrCreate returns the existing actor rather than stacking a second.
 *
 * TWO BUILDINGS ACTORS ON ONE NETWORK DRAW EVERY DEPOT TWICE, exactly on top of each other,
 * which looks correct from every angle and doubles the instance cost. Three drivers call
 * FindOrCreate (the editor mode, the editor tool, the PIE controller), so re-entry is the
 * normal case rather than the edge.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildingsActorFindOrCreateIsIdempotentTest,
	"Airside.Present.BuildingsActorFindOrCreateIsIdempotent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildingsActorFindOrCreateIsIdempotentTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Road = TestWorld.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("a road network"), Road)) { return false; }

	AAirsideBuildingsActor* First = AAirsideBuildingsActor::FindOrCreate(TestWorld.World, Road);
	AAirsideBuildingsActor* Second = AAirsideBuildingsActor::FindOrCreate(TestWorld.World, Road);
	if (!TestNotNull(TEXT("created"), First)) { return false; }
	TestEqual(TEXT("the second call finds the first"), Second, First);
	TestEqual(TEXT("bound to the road network it was given"), First->GetRoadNetwork(), Road);
	return true;
}

#endif
```

- [ ] **Step 2: Build to confirm it fails**

Run the Build.bat line.
Expected: FAIL, `AirsideBuildingsActor.h` not found. This is the red state. There's no header yet, so a test run isn't possible.

- [ ] **Step 3: Add the delegate to the road actor**

In `RoadNetworkActor.h`, in the first `public:` section, directly after `UPlotPresenter* GetPlotPresenter() const { return Plots; }` (line 258), add:

```cpp

	/**
	 * Fired after every TOPOLOGY rebuild, once the surface is built - where the plot boxes
	 * used to be drawn by a direct call on this actor.
	 *
	 * A DELEGATE, NOT A POINTER TO THE BUILDINGS ACTOR, so this actor does not know buildings
	 * exist: a depot's sheds are objects standing on the airport, not road network, and this
	 * class reached 2313 lines by owning everything that stood on it. Geometry (drag-frame)
	 * and Markings rebuilds do not fire it, for the reason RebuildMeshForChange gives - nothing
	 * a listener derives from has moved. Native, not dynamic: it carries a const reference,
	 * and nothing in Blueprint listens.
	 *
	 * ENFORCED BY: Airside.Present.BuildingsActorDrawsThroughTheDelegate.
	 */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnTopologyRebuilt, const URoadNetwork&);
	FOnTopologyRebuilt OnTopologyRebuilt;
```

In `RoadNetworkActor.cpp`, inside `RebuildMeshForChange`, directly after the closing brace of the `if (Plots != nullptr) { ... }` block (line 774) and before the Traffic block, add:

```cpp

	// THE BUILDINGS, through the delegate - see OnTopologyRebuilt's own comment. After the
	// surface, so a plot drawn this frame has its pad underneath it before its sheds go up;
	// before Traffic, matching where the direct call stood.
	OnTopologyRebuilt.Broadcast(*Network);
```

- [ ] **Step 4: Write the header**

Create `AirsideBuildingsActor.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AirsideBuildingsActor.generated.h"

class ARoadNetworkActor;
class UInstancedStaticMeshComponent;
class UPlotPresenter;
class URoadNetwork;

/**
 * Everything that stands ON the airport rather than being part of its surface: plotted
 * installations' modules today, their fences next.
 *
 * SPLIT OUT OF ARoadNetworkActor on 2026-09-22. That actor is the composition root for the
 * road network and had grown to own the depot presentation too, because every feature
 * entered through the one door. Only PRESENTATION moved: entities are still
 * URoadNetwork::Entities, read by 19 files and snapshotted by the one undo Memento, and
 * moving the model is a separate and much larger decision.
 *
 * ONE ACTOR FOR ALL BUILDINGS, not one per plot. A per-plot actor needs a spawn/destroy index
 * kept in step with the model through undo - the second index UPlotPresenter::RebuildFrom's
 * own comment refused, with tens of plots to rebuild. Revisit when per-building selection
 * needs a click target of its own.
 *
 * LISTENS, NEVER POLLS: ARoadNetworkActor::OnTopologyRebuilt is the only thing that makes it
 * draw, plus one catch-up rebuild on binding.
 */
UCLASS()
class AIRSIDE_API AAirsideBuildingsActor : public AActor
{
	GENERATED_BODY()

public:
	AAirsideBuildingsActor();

	/** The first buildings actor in a world, or nullptr. */
	static AAirsideBuildingsActor* Find(const UWorld* World);

	/**
	 * The world's buildings actor bound to Road, spawned if there is none.
	 *
	 * EVERY SITE THAT CREATES A ROAD NETWORK CALLS THIS BESIDE IT - the editor mode, the editor
	 * tool and the PIE controller - because a level with no buildings actor draws no depots,
	 * and nothing on screen would say why. Not transient, for ARoadNetworkActor::FindOrCreate's
	 * reason: this is the one saved with the level.
	 */
	static AAirsideBuildingsActor* FindOrCreate(UWorld* World, ARoadNetworkActor* Road);

	/**
	 * Listen to Road, dropping any previous binding, and rebuild once to catch up.
	 *
	 * THE CATCH-UP IS NOT OPTIONAL. Actors register in no promised order, so the road
	 * network's own first rebuild (its PostRegisterAllComponents) may already have fired
	 * before this bound - and without it a loaded level's depots stay invisible until the
	 * player next edits a road. Null unbinds and clears.
	 */
	void BindTo(ARoadNetworkActor* Road);

	/** The road network currently bound, or nullptr. */
	ARoadNetworkActor* GetRoadNetwork() const { return Bound.Get(); }

	/** The plot boxes - see UPlotPresenter. */
	UPlotPresenter* GetPlotPresenter() const { return Plots; }

	virtual void PostInitProperties() override;
	virtual void PostRegisterAllComponents() override;
	virtual void UnregisterAllComponents(bool bForReregister = false) override;

	/**
	 * The road network to draw for. EMPTY MEANS "THE ONE IN THE WORLD", which is every level
	 * this game has; set it only in a level that holds more than one.
	 *
	 * A SEARCH IS ACCEPTABLE HERE where ASunDriver refuses one, because this one cannot pick
	 * wrong silently: zero or several candidates is a Warning naming the count, and nothing is
	 * drawn - never a guess between two.
	 */
	UPROPERTY(EditInstanceOnly, Category = "Airside")
	TObjectPtr<ARoadNetworkActor> RoadNetwork;

private:
	/** OnTopologyRebuilt's handler. */
	void Rebuild(const URoadNetwork& Network);

	/** Remove the delegate binding, if any. Safe to call when unbound. */
	void Unbind();

	/** Same CreateDefaultSubobject and Transient reasoning as ARoadNetworkActor::Presenter. */
	UPROPERTY(Transient) TObjectPtr<UPlotPresenter> Plots;

	/**
	 * The one component every built module box is an instance in. A UPROPERTY and NOT
	 * Transient, unlike the presenter that fills it: it is a scene component this actor owns.
	 * Moved from ARoadNetworkActor::PlotBoxes.
	 */
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> ModuleBoxes;

	/**
	 * The same again for reserved bays nobody has bought, wearing the ghost material.
	 * A SECOND COMPONENT because an instance carries a transform and not a material, so a
	 * ghosted slot cannot differ from a built one inside ModuleBoxes. Moved from
	 * ARoadNetworkActor::PlotGhostBoxes.
	 */
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> ModuleGhosts;

	/**
	 * WEAK, because the road network can be destroyed first - a level unload tears actors
	 * down in no promised order - and a strong pointer here would be a dangling one then.
	 */
	TWeakObjectPtr<ARoadNetworkActor> Bound;

	/** The OnTopologyRebuilt binding on Bound; invalid when unbound. */
	FDelegateHandle BoundHandle;
};
```

- [ ] **Step 5: Write the implementation**

Create `AirsideBuildingsActor.cpp`:

```cpp
#include "Present/AirsideBuildingsActor.h"

#include "AirsideLog.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Content/AirsidePrimitives.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Model/RoadNetwork.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/**
	 * A module ISM: the engine cube, no collision.
	 *
	 * THE ENGINE'S OWN PRIMITIVE, not an authored asset, for the reason ARoadAgentActor's
	 * placeholder records at its own FObjectFinder - grey-box geometry that shows only until
	 * real meshes arrive has no business owning content of its own. No collision, matching
	 * every surface the road network draws: the world is flat and every pick is exact maths
	 * against the road plane, so a collider here would be something the build tools could
	 * trace against by accident.
	 */
	void DressAsModuleBoxes(UInstancedStaticMeshComponent& Component, UStaticMesh* Cube)
	{
		if (Cube != nullptr)
		{
			Component.SetStaticMesh(Cube);
		}
		Component.SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

AAirsideBuildingsActor::AAirsideBuildingsActor()
{
	// Nothing here ticks: every rebuild is driven by the road network's delegate.
	PrimaryActorTick.bCanEverTick = false;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(AirsidePrimitives::CubePath());
	UStaticMesh* CubeMesh = Cube.Succeeded() ? Cube.Object : nullptr;

	ModuleBoxes = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ModuleBoxes"));
	ModuleBoxes->SetupAttachment(RootComponent);
	DressAsModuleBoxes(*ModuleBoxes, CubeMesh);

	ModuleGhosts = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ModuleGhosts"));
	ModuleGhosts->SetupAttachment(RootComponent);
	DressAsModuleBoxes(*ModuleGhosts, CubeMesh);

	Plots = CreateDefaultSubobject<UPlotPresenter>(TEXT("Plots"));
	Plots->Initialise(ModuleBoxes, ModuleGhosts);
}

void AAirsideBuildingsActor::PostInitProperties()
{
	Super::PostInitProperties();

	// PIE DUPLICATES THE LEVEL, and a Transient non-instanced pointer comes back naming the
	// CDO's subobject rather than this actor's - the same repair ARoadNetworkActor makes, by
	// NAME so the constructor's Initialise call is kept rather than lost to a fresh object.
	// The components travel the same way and the presenter must be re-pointed AT THEM, or a
	// duplicate's boxes are added to a component no level renders.
	Plots = Cast<UPlotPresenter>(GetDefaultSubobjectByName(TEXT("Plots")));
	ModuleBoxes = Cast<UInstancedStaticMeshComponent>(GetDefaultSubobjectByName(TEXT("ModuleBoxes")));
	ModuleGhosts = Cast<UInstancedStaticMeshComponent>(GetDefaultSubobjectByName(TEXT("ModuleGhosts")));
	if (Plots != nullptr)
	{
		Plots->Initialise(ModuleBoxes, ModuleGhosts);
	}
}

void AAirsideBuildingsActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	// Templates excluded, for ARoadNetworkActor::PostRegisterAllComponents' reason: a class
	// default object has nothing to draw and no world to search.
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return;
	}

	ARoadNetworkActor* Road = RoadNetwork.Get();
	if (Road == nullptr)
	{
		int32 Found = 0;
		for (TActorIterator<ARoadNetworkActor> It(GetWorld()); It; ++It)
		{
			Road = *It;
			++Found;
		}
		if (Found != 1)
		{
			// NEVER A GUESS BETWEEN TWO - see RoadNetwork's own comment.
			UE_LOG(LogAirside, Warning,
				TEXT("Buildings: %d road network(s) in %s and none named - drawing nothing. ")
				TEXT("Set RoadNetwork on %s."),
				Found, *GetNameSafe(GetWorld()), *GetName());
			BindTo(nullptr);
			return;
		}
	}
	BindTo(Road);
}

void AAirsideBuildingsActor::UnregisterAllComponents(bool bForReregister)
{
	// A reregister (a property edit in the Details panel does one) keeps the binding: the
	// road network has not changed, and PostRegisterAllComponents rebinds anyway.
	if (!bForReregister)
	{
		Unbind();
	}
	Super::UnregisterAllComponents(bForReregister);
}

AAirsideBuildingsActor* AAirsideBuildingsActor::Find(const UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}
	for (TActorIterator<AAirsideBuildingsActor> It(const_cast<UWorld*>(World)); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

AAirsideBuildingsActor* AAirsideBuildingsActor::FindOrCreate(UWorld* World, ARoadNetworkActor* Road)
{
	if (AAirsideBuildingsActor* Existing = Find(World))
	{
		if (Existing->GetRoadNetwork() != Road)
		{
			Existing->RoadNetwork = Road;
			Existing->BindTo(Road);
		}
		return Existing;
	}
	if (World == nullptr)
	{
		return nullptr;
	}

	// DEFERRED, so RoadNetwork is set before PostRegisterAllComponents runs and the search
	// there is never consulted for an actor whose road network is already known.
	AAirsideBuildingsActor* Spawned = World->SpawnActorDeferred<AAirsideBuildingsActor>(
		AAirsideBuildingsActor::StaticClass(), FTransform::Identity);
	if (Spawned == nullptr)
	{
		return nullptr;
	}
	Spawned->RoadNetwork = Road;
	Spawned->FinishSpawning(FTransform::Identity);
	return Spawned;
}

void AAirsideBuildingsActor::BindTo(ARoadNetworkActor* Road)
{
	Unbind();

	if (Road == nullptr)
	{
		// Nothing to draw FOR, so nothing drawn - a depot left standing from a road network
		// that has gone would be a building the model no longer holds.
		if (Plots != nullptr)
		{
			Plots->Clear();
		}
		return;
	}

	Bound = Road;
	BoundHandle = Road->OnTopologyRebuilt.AddUObject(this, &AAirsideBuildingsActor::Rebuild);
	UE_LOG(LogAirside, Log, TEXT("Buildings: %s drawing plots for %s"),
		*GetName(), *Road->GetName());

	// THE CATCH-UP - see BindTo's own comment.
	if (Road->Network != nullptr)
	{
		Rebuild(*Road->Network);
	}
}

void AAirsideBuildingsActor::Unbind()
{
	if (ARoadNetworkActor* Road = Bound.Get())
	{
		Road->OnTopologyRebuilt.Remove(BoundHandle);
	}
	Bound.Reset();
	BoundHandle.Reset();
}

void AAirsideBuildingsActor::Rebuild(const URoadNetwork& Network)
{
	ARoadNetworkActor* Road = Bound.Get();
	if (Plots == nullptr || Road == nullptr)
	{
		return;
	}

	// THROUGH THE RESOLVER, never the raw property, and here rather than in the constructor:
	// a CDO cannot LoadSynchronous, and the material a level authored is only known once the
	// road network exists. Null leaves the cube's default, which reads as a built bay - wrong,
	// but visible, which is the failure mode to prefer. Moved from
	// ARoadNetworkActor::RebuildMeshForChange with the components it colours.
	if (ModuleGhosts != nullptr)
	{
		ModuleGhosts->SetMaterial(0, Road->ResolveGhostMaterial());
	}

	// THROUGH THE ROAD NETWORK'S ONE RESOLVER (issue #181) - see UPlotPresenter::RebuildFrom.
	Plots->RebuildFrom(Network, Road->ResolveDepotKits());
}
```

Already verified while planning: `ResolveDepotKits` (`RoadNetworkActor.h:394`) and `ResolveGhostMaterial` (line 1118) are both in `public:` sections.

- [ ] **Step 6: Build twice**

Run the Build.bat line twice (a new test .cpp needs two builds).
Expected: `Result: Succeeded` both times.

- [ ] **Step 7: Run the seam tests**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present.BuildingsActor`
Expected: `3 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 8: Prove the seam test measures the seam**

Comment out the `OnTopologyRebuilt.Broadcast(*Network);` line and rebuild (twice is not needed; this is an existing file). Run the same filter.
Expected: `BuildingsActorDrawsThroughTheDelegate` FAILS on "a topology rebuild reaches it through the delegate". Restore the line, rebuild, rerun, and get 3 passing. Memory: a green test may measure nothing.

- [ ] **Step 9: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Present/AirsideBuildingsActor.h Plugins/Airside/Source/Airside/Private/Present/AirsideBuildingsActor.cpp Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp Plugins/Airside/Source/AirsideTests/Private/BuildingsActorTest.cpp
git commit -m "feat(present): AAirsideBuildingsActor listens to OnTopologyRebuilt"
```

---

### Task 3: The road actor stops drawing plots

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h:26,36-41,257-258,1000-1012`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp:21,115-150,322-334,760-774`
- Modify: `Plugins/Airside/Source/Airside/Public/Testing/AirsideTestWorld.h`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp`, `PlotPlaceToolTest.cpp`

**Interfaces:**
- Consumes: `AAirsideBuildingsActor` (Task 2).
- Produces: `FAirsideTestWorld::Buildings` (`AAirsideBuildingsActor*`, null when `bSpawnActor=false`).

- [ ] **Step 1: The fixture spawns the buildings actor**

In `AirsideTestWorld.h`, add `#include "Present/AirsideBuildingsActor.h"` after the `RoadNetworkActor.h` include. After the `Actor` field add:

```cpp

	/**
	 * Null when constructed with bSpawnActor=false. Spawned AFTER Actor, so it finds and binds
	 * to it on registration - the real path, not a test-only BindTo. EVERY TEST THAT SPAWNS A
	 * ROAD NETWORK GETS ONE because plots stopped being the road network's to draw on
	 * 2026-09-22; a fixture without it would pass every plot test that forgot to ask.
	 */
	AAirsideBuildingsActor* Buildings = nullptr;
```

And in the constructor's `if (bSpawnActor)` block, after `Actor = ...`:

```cpp
			Buildings = World->SpawnActor<AAirsideBuildingsActor>();
```

- [ ] **Step 2: Move the test call sites**

In `PlotPresenterTest.cpp` and `PlotPlaceToolTest.cpp`, every `Actor->GetPlotPresenter()` becomes `TestWorld.Buildings->GetPlotPresenter()`. There are 18 in the first file and 2 in the second; `grep -c "GetPlotPresenter" <file>` before and after must be equal. Two cases don't fit that mechanical swap:
- `PlotInstances(const ARoadNetworkActor* Actor)` (PlotPresenterTest.cpp:78) becomes `PlotInstances(const AAirsideBuildingsActor* Buildings)` reading `Buildings->GetPlotPresenter()`, and its call sites pass `TestWorld.Buildings`. Add `#include "Present/AirsideBuildingsActor.h"`.
- `FPlotPresenterSurvivesDuplicationTest` (lines 302-342) duplicates the ROAD actor. Its claim now belongs to the buildings actor. Replace the body from `ARoadNetworkActor* Dup = ...` to the end with:

```cpp
	// THE CLAIM MOVED WITH THE PRESENTER on 2026-09-22: the buildings actor owns it now, so
	// that is the actor duplicated. DuplicateObject registers nothing, so the duplicate is
	// bound by hand - the thing under test is the re-pointing, not the binding.
	AAirsideBuildingsActor* Dup =
		DuplicateObject<AAirsideBuildingsActor>(TestWorld.Buildings, TestWorld.Buildings->GetOuter());
	if (!TestNotNull(TEXT("a duplicate"), Dup)) { return false; }

	if (!TestNotNull(TEXT("the duplicate has a plot presenter"), Dup->GetPlotPresenter()))
	{
		return false;
	}
	TestEqual(TEXT("and it is the duplicate's own, not the CDO's"),
		Dup->GetPlotPresenter()->GetOuter(), static_cast<UObject*>(Dup));

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	Actor->ClearNetwork();
	PlaceDepot(Actor, Depot, 0.0);
	Dup->BindTo(Actor);

	// The real claim: the duplicate's boxes land somewhere its own presenter can count,
	// which they cannot if it is still filling the CDO's component.
	TestTrue(TEXT("the duplicate's own boxes stand up"),
		Dup->GetPlotPresenter()->GetInstanceCount() > 3);

	return true;
}
```

Keep the test's existing header comment. It still describes the mechanism, which is unchanged.

- [ ] **Step 3: Remove the plot members from the road actor**

In `RoadNetworkActor.h`:
- delete `class UPlotPresenter;` (line 26);
- in the issue-#191 comment (lines 36-41), remove `UPlotPresenter` and `GetPlotPresenter` from what it lists, keeping the sentence about `UEntityDefinition`. Show the reworded lines in the diff;
- delete `GetPlotPresenter()` and its one-line comment (257-258);
- delete `Plots`, `PlotBoxes`, `PlotGhostBoxes` and their comments (1000-1012). Their WHY comments were copied onto the buildings actor's members in Task 2; check each sentence landed there before deleting.

In `RoadNetworkActor.cpp`:
- delete `#include "Present/PlotPresenter.h"` (line 21);
- delete the constructor block from `// The plot boxes: one instanced component...` through `Plots->Initialise(PlotBoxes, PlotGhostBoxes);` (115-150);
- delete the `Plots = ...` line and the `PlotBoxes`/`PlotGhostBoxes`/`Initialise` block in `PostInitProperties` (322-334);
- in `RebuildMeshForChange`, delete the `if (Plots != nullptr) { ... }` block (760-774) and its leading comment. The Broadcast added in Task 2 stays;
- the two comments at lines ~733 and ~752 say "Plots and Traffic are skipped". Change them to "The buildings (OnTopologyRebuilt) and Traffic are skipped" so they stay true.

- [ ] **Step 4: Sweep the saved `PlotBoxes` out of old levels**

In `ARoadNetworkActor::PostRegisterAllComponents`, inside the `if (!HasAnyFlags(...))` block and before `InitialisePresenterLayers();`, add:

```cpp
		// THE PLOT COMPONENTS THIS ACTOR NO LONGER OWNS, swept by name. Until 2026-09-22 they
		// were default subobjects here, and M_Starter.umap was saved with one. Deleting the
		// UPROPERTY does not delete the saved component: AActor::ResetOwnedComponents collects
		// components by OUTER, so the orphan still registers and renders whatever instances it
		// was saved with - stale grey boxes over a depot the buildings actor is also drawing.
		// Kept until every level has been resaved; place_buildings_actor.py resaves M_Starter.
		for (UInstancedStaticMeshComponent* Stale :
			TInlineComponentArray<UInstancedStaticMeshComponent*>(this))
		{
			const FName Name = Stale->GetFName();
			if (Name == TEXT("PlotBoxes") || Name == TEXT("PlotGhostBoxes"))
			{
				UE_LOG(LogAirside, Log, TEXT("Swept legacy plot component %s from %s"),
					*Name.ToString(), *GetName());
				Stale->DestroyComponent();
			}
		}
```

`Components/InstancedStaticMeshComponent.h` stays included in this .cpp; it's still needed here.

- [ ] **Step 5: Build**

Run the Build.bat line.
Expected: `Result: Succeeded`. A compile error naming `Plots`, `PlotBoxes` or `GetPlotPresenter` is a missed site: grep the whole `Plugins/` and `Source/` for it and fix it there.

- [ ] **Step 6: Run the suites**

Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Present`, then `-Filter Airside.Tool.Plot`
Expected: each run/failed/crashed line equals its Task 0 baseline plus the 3 new tests in `Airside.Present`.

- [ ] **Step 7: Commit**

```bash
git add -A Plugins/Airside/Source
git commit -m "refactor(present): road network actor stops drawing plots; sweep saved PlotBoxes"
```

---

### Task 4: Every driver ensures the buildings actor

**Files:**
- Modify: `Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEdMode.cpp:245`
- Modify: `Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEditorTool.cpp:268`
- Modify: `Source/AirportMgr/RoadBuildController.cpp:52-59`

**Interfaces:**
- Consumes: `AAirsideBuildingsActor::FindOrCreate(UWorld*, ARoadNetworkActor*)` (Task 2).

- [ ] **Step 1: The editor mode**

`RoadBuildEdMode.cpp`: add `#include "Present/AirsideBuildingsActor.h"`. Replace `Context.Target = ARoadNetworkActor::FindOrCreate(GetWorld());` with:

```cpp
	ARoadNetworkActor* Road = ARoadNetworkActor::FindOrCreate(GetWorld());
	// AND ITS BUILDINGS, created beside it: a level with a road network and no buildings
	// actor draws no depots and says nothing about why.
	AAirsideBuildingsActor::FindOrCreate(GetWorld(), Road);
	Context.Target = Road;
```

- [ ] **Step 2: The editor tool**

`RoadBuildEditorTool.cpp`: add the same include. Replace `return ARoadNetworkActor::FindOrCreate(World);` with:

```cpp
	ARoadNetworkActor* Road = ARoadNetworkActor::FindOrCreate(World);
	// See URoadBuildEdMode::MakeReselectContext: the buildings actor is created beside it.
	AAirsideBuildingsActor::FindOrCreate(World, Road);
	return Road;
```

- [ ] **Step 3: The PIE controller**

`RoadBuildController.cpp`: add `#include "Present/AirsideBuildingsActor.h"`. Directly after the `if (Target == nullptr) { ... return; }` block, add:

```cpp

	// THE BUILDINGS, found or spawned for this play session. M_Starter has one placed, so this
	// normally finds it; spawning covers a level that predates the split, where the depots
	// would otherwise be invisible in play and present in the editor.
	AAirsideBuildingsActor::FindOrCreate(GetWorld(), Target);
```

`Target` is `TObjectPtr<ARoadNetworkActor>` (`RoadBuildController.h:583`), so it passes directly.

- [ ] **Step 4: Build, run the editor-mode and controller tests**

Run the Build.bat line, then `./Tools/Run-AirsideTests.ps1 -Filter Airside.Editor` and `-Filter AirportMgr`.
Expected: `Result: Succeeded`, and each run/failed/crashed line shows 0 failed, 0 crashed. Record the counts for the PR.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/AirsideEditor Source/AirportMgr/RoadBuildController.cpp
git commit -m "feat(drivers): editor mode, editor tool and PIE controller ensure the buildings actor"
```

---

### Task 5: Place it in M_Starter and resave

**Files:**
- Create: `Tools/Python/place_buildings_actor.py`
- Modify (binary, by the script): `Content/Maps/M_Starter.umap`

- [ ] **Step 1: Write the script**

```python
"""Places AAirsideBuildingsActor in M_Starter, bound to its road network. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED. Every result line is prefixed MARKER: so it can be grepped out of
Saved/Logs/AirportMgr.log.

WHY IT IS PLACED AND NOT ONLY SPAWNED: the drivers FindOrCreate one, but outside the build
mode the editor viewport shows the level as saved - and a depot with no buildings actor is
invisible there. THE RESAVE IS THE SECOND POINT: M_Starter was saved with the road network's
old PlotBoxes component, which ARoadNetworkActor now sweeps on registration; saving after the
sweep drops it from disk.

Idempotent, like place_sun_driver.py: an existing buildings actor is replaced, never stacked.
"""
import unreal

LEVEL = "/Game/Maps/M_Starter"


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def run():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    levels.load_level(LEVEL)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    for old in [a for a in actors.get_all_level_actors() if isinstance(a, unreal.AirsideBuildingsActor)]:
        actors.destroy_actor(old)
        say("removed an existing AAirsideBuildingsActor")

    roads = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.RoadNetworkActor)]
    if len(roads) != 1:
        fail("expected exactly 1 RoadNetworkActor, found %d - will not guess" % len(roads))
        say("DONE")
        return

    buildings = actors.spawn_actor_from_class(unreal.AirsideBuildingsActor, unreal.Vector(0.0, 0.0, 0.0))
    buildings.set_actor_label("AirsideBuildings")
    buildings.set_editor_property("road_network", roads[0])
    levels.save_current_level()
    say("spawned AAirsideBuildingsActor bound to %s" % roads[0].get_actor_label())

    # Reload and read back: a level edit that reports success and writes nothing is the known
    # failure mode here (a locked .umap does exactly that).
    levels.load_level(LEVEL)
    found = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.AirsideBuildingsActor)]
    if len(found) != 1:
        fail("expected exactly 1 AAirsideBuildingsActor after reload, found %d" % len(found))
        say("DONE")
        return
    if found[0].get_editor_property("road_network") is None:
        fail("AAirsideBuildingsActor.RoadNetwork is empty after reload")
    else:
        say("PASS AAirsideBuildingsActor.RoadNetwork = %s" % found[0].get_editor_property("road_network").get_actor_label())
        say("ALL VERIFIED")
    say("DONE")


run()
```

- [ ] **Step 2: Run it (editor closed)**

```
& "D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\repos\AirportMgr2\AirportMgr.uproject" `
  -run=pythonscript -script="C:\repos\AirportMgr2\Tools\Python\place_buildings_actor.py" -unattended -nosplash -nopause
```

Then: `grep "MARKER:" Saved/Logs/AirportMgr.log` and `grep "Swept legacy plot component" Saved/Logs/AirportMgr.log`
Expected: `MARKER: ALL VERIFIED` and one `Swept legacy plot component PlotBoxes` line.

- [ ] **Step 3: Verify on disk, with a control**

```bash
cd /c/repos/AirportMgr2/Content/Maps
echo "control (must be >0): $(grep -c -a RoadNetworkActor M_Starter.umap)"
echo "buildings (must be >0): $(grep -c -a AirsideBuildingsActor M_Starter.umap)"
echo "stale PlotBoxes (must be 0): $(grep -c -a PlotBoxes M_Starter.umap)"
```

Expected: the control is above 0, buildings is above 0, and PlotBoxes is 0. If PlotBoxes is still there, the sweep ran after the save or never ran. Stop and report; don't commit the map.

- [ ] **Step 4: Commit**

```bash
git add Tools/Python/place_buildings_actor.py Content/Maps/M_Starter.umap
git commit -m "content: M_Starter gets its buildings actor; resave drops the stale PlotBoxes"
```

---

### Task 6: Full verification, contract measurements, spec amendment, PR

- [ ] **Step 1: Authoritative test run**

Run: `./Tools/Run-AirsideTests.ps1`
Expected: the run/failed/crashed line shows 0 failed, 0 crashed, and the total is the Task 0 total + 3. Quote the line in the PR.

- [ ] **Step 2: Refactor-contract deltas**

Re-run the Task 0 Step 1 loop, adding the two new files. Compute the differences against `baseline.txt`:
- `UE_LOG(`: the road actor and presenter files lose none. The road actor gains the sweep line, and the buildings actor adds 2 (Warning, bind). Any fall is a dropped log line and must be found.
- Comment lines: summed over all touched files including the new ones, the count must not fall.

- [ ] **Step 3: Runtime look**

Open the editor on M_Starter, enter PIE, and place a fuel depot. Then run `python Tools/Mcp.py shot out.png editor` and `python Tools/Mcp.py log LogAirside "Buildings:|Plots:"`.
Expected: `Buildings: AirsideBuildings drawing plots for RoadNetwork`, and a `Plots: 1 plot(s), ...` line after the placement. The screenshot shows the depot's boxes and fence panels, the same as on `main`.

- [ ] **Step 4: Amend the spec**

In `docs/superpowers/specs/2026-09-22-chainlink-fence-design.md`, PR 1 section, replace the bullet beginning "The road actor's plot accessors stay as forwarders" with:

```
- `GetPlotPresenter` is REMOVED from the road actor, not forwarded: it is plain C++ with only
  test callers, and a forwarder needs a road-to-buildings pointer that undoes the split. Tests
  read `FAirsideTestWorld::Buildings`. Every road-network creation site (fixture, editor mode,
  editor tool, PIE controller) calls `AAirsideBuildingsActor::FindOrCreate` beside it.
- M_Starter was saved with the old `PlotBoxes`; the road actor sweeps it by name on
  registration, and the level is resaved.
```

- [ ] **Step 5: Commit, push, PR**

```bash
git add docs/superpowers/specs/2026-09-22-chainlink-fence-design.md
git commit -m "docs: spec follows the planned split - no GetPlotPresenter forwarder"
git push -u origin feature/chainlink-fence
gh pr create --base main --title "refactor: plots move to AAirsideBuildingsActor (chainlink fence 1/2)" --body-file <scratchpad pr-body.md>
```

The PR body follows the repo template: the build line, the test line, the `UE_LOG` and comment-line deltas from Step 2, the runtime evidence from Step 3, and the deviation paragraph from this plan. End it with:

```
🤖 Generated with [Claude Code](https://claude.com/claude-code)
```
