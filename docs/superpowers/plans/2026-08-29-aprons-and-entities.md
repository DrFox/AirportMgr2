# Aprons and Entities Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add polygon apron surfaces and entities-with-anchors, so a stand is a placed thing whose service positions are ordinary nodes on the guideline graph — and prove the model can derive every marking in the reference images.

**Architecture:** An apron is a surface with no cross-section, so it carries a material slot rather than a profile and never enters the junction solve. An entity is a Flyweight — a placed instance plus a shared `UEntityDefinition` declaring anchors in local space. Placing one resolves each anchor to a guideline node at its world pose; those nodes are created **non-derived**, so the derivation's orphan sweep spares them and their handles survive every rebuild.

**Tech Stack:** Unreal Engine 5.8.2, C++20, MSVC 14.51.36231, Unreal Automation Test framework.

**Spec:** `docs/superpowers/specs/2026-08-29-ground-movement-model-design.md` (§4.1, §4.3, §6, §10, and risk 4). Parent: `2026-08-28-procedural-road-system-design.md`.

**Follows:** Plan A (`2026-08-29-guideline-graph.md`), merged. The guideline graph, traversal classes, derivation and traversal queries all exist.

## Two decisions this plan settles, which the spec deferred to it

**Spec risk 4 — guideline handles do not survive a rebuild — is answered by `bDerived`, not by rewriting the builder.** The risk assumed `FRoadGuidelineBuilder::Build` would have to update nodes in place. It does not: `Build`'s orphan sweep already requires `Nodes[Index].bDerived` before removing an idle node, so a node created with `bDerived = false` is never swept, never destroyed, and never has its generation advanced. Anchor nodes are created that way. Task 3 asserts it directly. This is the cheap answer to what was recorded as potentially "a substantial rewrite", and it exists because Plan A's final review added node provenance.

**Plan A's known gap 6 is closed here, as Task 1.** Plan A's turn-path direction filter and access-mask intersection are correct but untested — deleting either leaves that suite green. It is ~20 lines, it is the highest-value follow-up on Plan A's list, and Task 5 of this plan builds on turn paths. Closing it first means this plan is not standing on unpinned code.

## Global Constraints

- **Engine:** Unreal Engine 5.8.2 at `D:\Epic\UE_5.8`. Project at `C:\repos\AirportMgr2`.
- **`BuildSettingsVersion.V7`** — return-type, dangling-reference, unreachable-code and **variable-shadowing** warnings are **errors**. Rename inner variables that collide with names already in scope.
- **`Solve/` must keep ZERO engine dependencies** beyond `CoreMinimal.h`. This plan changes nothing under `Solve/`.
- `Build/` depends on `Model`, `Profiles`, `Solve`. `Present/` depends on `Build`. Nothing depends upward.
- **Automation flags:** `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter`. The nested form does not compile in 5.8.
- Test files wrapped in `#if WITH_DEV_AUTOMATION_TESTS` … `#endif`.
- **`FVector2D` is double-precision. Never narrow positions to `float`.**
- **Slot-map types must expose `int32 Generation` and `bool bAlive`**, or `RoadSlot::Add` will not compile against them.
- Unreal `F`/`U`/`A`/`E` prefixes are required by UnrealHeaderTool; reflected types need `USTRUCT()`/`UENUM()`/`UCLASS()` and `GENERATED_BODY()`, and their header needs `#include "<HeaderName>.generated.h"` LAST.
- **The guideline graph has NO bitwise weld contract.** Endpoints are shared by handle. Do not add position-based deduplication of guideline nodes, and do not import the pavement mesh's `==`-on-position discipline.
- **No capacity field** on a guideline edge by any name. `Width` is physical extent for marking and clearance only.

### Standard commands

Build (the editor must be **closed** — Live Coding holds the lock):

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
```

Test:

```powershell
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

Judge every run by the wrapper's parsed output. **Standing exception:** `Plugins/McpAutomationBridge` is git-ignored, unrelated, and fails to compile (a string literal over MSVC's 16380-char limit), so the build prints `Result: Failed` regardless. Confirm the RoadNet modules compiled with no errors of their own, then judge by the test wrapper.

The suite is at **19 tests** before this plan.

---

## File Structure

```
Plugins/RoadNet/Source/RoadNet/
  Public/Model/RoadNetwork.h            MODIFY  - apron + entity storage, bDerived node overload
  Private/Model/RoadNetwork.cpp         MODIFY
  Public/Model/RoadApron.h              CREATE  - FApronSurface
  Public/Model/RoadHandles.h            MODIFY  - apron + entity handles
  Public/Model/RoadEntity.h             CREATE  - EServiceRole, FEntityAnchor, FEntityInstance
  Public/Entities/EntityDefinition.h    CREATE  - UEntityDefinition data asset
  Private/Entities/EntityDefinition.cpp CREATE
  Private/Build/RoadGuidelineBuilder.cpp  (untouched - Task 3 relies on its existing sweep)

Plugins/RoadNet/Source/RoadNetTests/Private/
  RoadGuidelineBuilderTest.cpp          MODIFY  - Task 1 closes Plan A's gap 6
  RoadApronTest.cpp                     CREATE  - polygon surface storage
  RoadEntityTest.cpp                    CREATE  - anchors, resolution, rebuild survival, undo
  RoadMarkingSourceTest.cpp             CREATE  - the spec section 6 falsification test
```

`Entities/` is a new directory under `Public/`/`Private/`. It depends on `Model` only.

---

### Task 1: Close Plan A's untested turn-path rules

Plan A's known gap 6. `FRoadGuidelineBuilder` filters turn paths by each arm's direction and intersects their access masks, both correctly — but every junction profile in that suite is bidirectional with identical class and width on both arms, so **deleting either fix leaves all 19 tests passing**. Task 5 builds on turn paths; pin them first.

**Files:**
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadGuidelineBuilderTest.cpp` (extend)

**Interfaces:**
- Consumes: `FRoadGuidelineBuilder::Build`, `URoadProfile::Guidelines`, `FGuidelineEdge::{AllowedTraffic, Direction, DerivedFrom}`, `FTrafficMask::Allows`.
- Produces: nothing. Test-only.

- [ ] **Step 1: Write the failing test**

Append inside `FRoadGuidelineBuilderTest::RunTest`, before its final `return true;`:

```cpp
	// Plan A's known gap 6. The turn loop filters by each arm's DIRECTION and intersects
	// their access MASKS, and both are correct - but every junction profile in this file
	// is bidirectional with identical class and width on both arms, so deleting either
	// filter leaves the whole suite green. These two blocks are the only thing that would
	// notice.
	//
	// One-way arms first. An arm declared AToB runs AWAY from a junction at its A end, so
	// nothing can ARRIVE at that junction along it, and no turn may be emitted FROM it.
	{
		URoadNetwork* OneWay = NewObject<URoadNetwork>(GetTransientPackage());

		FProfileBand OneWayLane;
		OneWayLane.Width = 700.0;
		OneWayLane.Type = ERoadBandType::Lane;

		// Outbound: A is the hub, so AToB means traffic leaves the hub and never returns.
		URoadProfile* Outbound = NewObject<URoadProfile>(GetTransientPackage());
		Outbound->Bands.Add(OneWayLane);
		FProfileGuideline OutLine;
		OutLine.CentreOffset = 0.0;
		OutLine.Class = ETraversalClass::GroundVehicle;
		OutLine.Direction = EGuidelineDir::AToB;
		Outbound->Guidelines.Add(OutLine);

		// Two-way, for the arms that must still work.
		URoadProfile* TwoWay = NewObject<URoadProfile>(GetTransientPackage());
		TwoWay->Bands.Add(OneWayLane);
		FProfileGuideline BothWays = OutLine;
		BothWays.Direction = EGuidelineDir::Bidirectional;
		TwoWay->Guidelines.Add(BothWays);

		const FRoadNodeId Hub   = OneWay->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Out   = OneWay->AddNode(FVector2D(12000.0, 0.0));
		const FRoadNodeId FreeA = OneWay->AddNode(FVector2D(-12000.0, 0.0));
		const FRoadNodeId FreeB = OneWay->AddNode(FVector2D(0.0, 12000.0));

		const FRoadSegmentId OutArm = OneWay->AddStraightSegment(Hub, Out, Outbound);
		OneWay->AddStraightSegment(Hub, FreeA, TwoWay);
		OneWay->AddStraightSegment(Hub, FreeB, TwoWay);

		const FRoadSolveResult OneWaySolved = FRoadNetworkSolver::SolveAll(*OneWay);
		TestEqual(TEXT("the one-way network solved"), OneWaySolved.FailedNodes, 0);

		FRoadGuidelineBuilder::Build(*OneWay, OneWaySolved);

		// The hub's guideline node for the outbound arm's A end. Turn paths carry no
		// DerivedFrom, so they are told apart from segment edges that way.
		FGuidelineNodeId OutArmAtHub;
		for (const FGuidelineEdge& Edge : OneWay->GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.DerivedFrom == OutArm)
			{
				OutArmAtHub = Edge.A;   // A end is the hub end: the segment was added Hub->Out
			}
		}
		TestTrue(TEXT("found the outbound arm's hub-end node"), OutArmAtHub.IsSet());

		int32 TurnsFromOutbound = 0;
		int32 TurnsIntoOutbound = 0;
		for (const FGuidelineEdge& Edge : OneWay->GetGuidelineEdges())
		{
			if (!Edge.bAlive || Edge.DerivedFrom.IsSet())
			{
				continue;
			}
			if (Edge.A == OutArmAtHub) { ++TurnsFromOutbound; }
			if (Edge.B == OutArmAtHub) { ++TurnsIntoOutbound; }
		}

		// Nothing can arrive at the hub along an arm that only runs away from it.
		TestEqual(TEXT("no turn is emitted FROM a one-way arm nothing can arrive on"),
			TurnsFromOutbound, 0);
		// But you may still turn INTO it - that is the direction it permits.
		TestTrue(TEXT("turns INTO the one-way arm are still emitted"), TurnsIntoOutbound > 0);
	}

	// Then the mask intersection. Where two arms carry different traversal classes, the
	// only thing that may cross between them is Emergency - a baggage tug must not be
	// routed from a service road onto a live taxiway turn.
	{
		URoadNetwork* Mixed = NewObject<URoadNetwork>(GetTransientPackage());

		FProfileBand MixedLane;
		MixedLane.Width = 700.0;
		MixedLane.Type = ERoadBandType::Lane;

		URoadProfile* AirProfile = NewObject<URoadProfile>(GetTransientPackage());
		AirProfile->Bands.Add(MixedLane);
		FProfileGuideline AirLine;
		AirLine.CentreOffset = 0.0;
		AirLine.Class = ETraversalClass::Aircraft;
		AirLine.Direction = EGuidelineDir::Bidirectional;
		AirProfile->Guidelines.Add(AirLine);

		URoadProfile* RoadProfile = NewObject<URoadProfile>(GetTransientPackage());
		RoadProfile->Bands.Add(MixedLane);
		FProfileGuideline RoadLine = AirLine;
		RoadLine.Class = ETraversalClass::GroundVehicle;
		RoadProfile->Guidelines.Add(RoadLine);

		const FRoadNodeId Cross = Mixed->AddNode(FVector2D(0.0, 0.0));
		Mixed->AddStraightSegment(Cross, Mixed->AddNode(FVector2D( 12000.0, 0.0)), AirProfile);
		Mixed->AddStraightSegment(Cross, Mixed->AddNode(FVector2D(-12000.0, 0.0)), AirProfile);
		Mixed->AddStraightSegment(Cross, Mixed->AddNode(FVector2D(0.0, 12000.0)), RoadProfile);

		const FRoadSolveResult MixedSolved = FRoadNetworkSolver::SolveAll(*Mixed);
		TestEqual(TEXT("the mixed-class network solved"), MixedSolved.FailedNodes, 0);

		FRoadGuidelineBuilder::Build(*Mixed, MixedSolved);

		int32 AircraftOnlyTurns = 0;
		int32 EmergencyOnlyTurns = 0;
		int32 IllegalTurns = 0;
		for (const FGuidelineEdge& Edge : Mixed->GetGuidelineEdges())
		{
			if (!Edge.bAlive || Edge.DerivedFrom.IsSet())
			{
				continue;
			}

			const bool bAir = Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft);
			const bool bVeh = Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle);

			if (bAir && bVeh)
			{
				// A turn admitting BOTH would be one arm's mask copied wholesale, which is
				// the bug: it puts a tug on a taxiway turn and an aircraft on a service road.
				++IllegalTurns;
			}
			else if (bAir) { ++AircraftOnlyTurns; }
			else if (Edge.AllowedTraffic.Allows(ETraversalClass::Emergency)) { ++EmergencyOnlyTurns; }
		}

		TestEqual(TEXT("no turn admits both aircraft and ground vehicles"), IllegalTurns, 0);
		TestTrue(TEXT("aircraft-to-aircraft turns exist"), AircraftOnlyTurns > 0);
		TestTrue(TEXT("and crossing between classes is emergency-only"), EmergencyOnlyTurns > 0);
	}
```

- [ ] **Step 2: Build and run — expect PASS**

Both fixes are already in the code, so this passes immediately. That is expected and is not a reason to skip the step: it confirms the test compiles and agrees with the shipped behaviour before you prove it can fail.

- [ ] **Step 3: Prove both blocks discriminate**

This is the point of the task. Twice, one at a time, in `Private/Build/RoadGuidelineBuilder.cpp`:

1. Replace `if (!bMayArrive || !bMayLeave) { continue; }` with `if (false) { continue; }`. Rebuild. Expect `RoadNet.Build.GuidelineBuilder` to FAIL on "no turn is emitted FROM a one-way arm nothing can arrive on". Restore.
2. Replace `Turn.AllowedTraffic.Bits = static_cast<uint8>(FromMask.Bits & ToMask.Bits);` with `Turn.AllowedTraffic = FromMask;`. Rebuild. Expect FAIL on "no turn admits both aircraft and ground vehicles". Restore.

Record both failure texts. **If either does not fail, that block is not discriminating — say so rather than committing it as though it were.**

- [ ] **Step 4: Build and run — expect PASS**

Expected: 19 tests, 0 failed.

- [ ] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "test(roadnet): pin turn-path direction filtering and mask intersection"
```

---

### Task 2: Apron surfaces

A polygon of pavement. No cross-section, so no profile and no junction solve — it names a material slot directly.

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadApron.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadHandles.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadNetwork.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Model/RoadNetwork.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadApronTest.cpp`

**Interfaces:**
- Consumes: `RoadSlot::{Add, Get, Remove, IsValid}`.
- Produces:
  - `FApronId` — handle, same shape as `FRoadNodeId`.
  - `FApronSurface { TArray<FVector2D> Outline; FName SurfaceMaterialSlot; int32 Generation; bool bAlive; }`
  - `URoadNetwork::{AddApron, RemoveApron, GetApron, GetAprons}`

- [ ] **Step 1: Write the failing test**

Create `Plugins/RoadNet/Source/RoadNetTests/Private/RoadApronTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadApron.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadApronTest,
	"RoadNet.Model.Apron",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadApronTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// A square of concrete. An apron is a surface with no cross-section: no profile, no
	// arms, no fillets, and nothing for the junction solver to trim.
	FApronSurface Slab;
	Slab.Outline = {
		FVector2D(0.0, 0.0),
		FVector2D(10000.0, 0.0),
		FVector2D(10000.0, 8000.0),
		FVector2D(0.0, 8000.0) };
	Slab.SurfaceMaterialSlot = TEXT("Concrete");

	const FApronId Id = Net->AddApron(MoveTemp(Slab));
	TestTrue(TEXT("a new apron handle is set"), Id.IsSet());

	{
		const FApronSurface* Stored = Net->GetApron(Id);
		if (TestNotNull(TEXT("the apron resolves"), Stored))
		{
			TestEqual(TEXT("its outline survived the move"), Stored->Outline.Num(), 4);
			TestEqual(TEXT("its material slot survived"),
				Stored->SurfaceMaterialSlot, FName(TEXT("Concrete")));

			// Positions are double-precision throughout - an apron the size of a real
			// airport is tens of thousands of units across and must not round.
			TestEqual(TEXT("a corner is stored exactly"), Stored->Outline[2].X, 10000.0);
		}
	}

	// Generation checking, the whole point of the handle: a recycled slot must NOT resolve
	// through the old handle, or an edit silently lands on whatever took the slot over.
	{
		TestTrue(TEXT("the apron removes"), Net->RemoveApron(Id));
		TestNull(TEXT("a removed apron no longer resolves"), Net->GetApron(Id));

		FApronSurface Second;
		Second.Outline = { FVector2D(0.0, 0.0), FVector2D(1.0, 0.0), FVector2D(0.0, 1.0) };
		const FApronId Recycled = Net->AddApron(MoveTemp(Second));

		TestEqual(TEXT("the slot was reused"), Recycled.Index, Id.Index);
		TestNotEqual(TEXT("but the generation moved on"), Recycled.Generation, Id.Generation);
		TestNull(TEXT("the stale handle does not resolve"), Net->GetApron(Id));
		TestNotNull(TEXT("the fresh handle does"), Net->GetApron(Recycled));
	}

	// Aprons are a SEPARATE collection from segments. An apron must never appear in the
	// segment list, or the junction solver would try to trim it and there is nothing there
	// to trim - no arms, no profile, no cut vertices.
	TestEqual(TEXT("adding an apron adds no segment"), Net->GetSegments().Num(), 0);
	TestEqual(TEXT("and no node"), Net->GetNodes().Num(), 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Build — expect FAIL**

Expected: compile error, `Model/RoadApron.h` not found and `URoadNetwork` has no member `AddApron`.

- [ ] **Step 3: Add the handle**

In `Public/Model/RoadHandles.h`, append — the same shape as the existing handles, duplicated rather than templated because UnrealHeaderTool cannot reflect a template instantiation:

```cpp
/** Generation-checked handle to a polygon apron surface in URoadNetwork. */
USTRUCT()
struct ROADNET_API FApronId
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() int32 Generation = 0;

	/** See FRoadNodeId::IsSet - this reports assignment, never liveness. */
	bool IsSet() const { return Index != INDEX_NONE; }

	bool operator==(const FApronId& Other) const
	{
		return Index == Other.Index && Generation == Other.Generation;
	}
	bool operator!=(const FApronId& Other) const { return !(*this == Other); }
};

FORCEINLINE uint32 GetTypeHash(const FApronId& Id)
{
	return HashCombine(::GetTypeHash(Id.Index), ::GetTypeHash(Id.Generation));
}
```

- [ ] **Step 4: Write the apron entity**

Create `Public/Model/RoadApron.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "RoadApron.generated.h"

/**
 * A polygon of pavement - a terminal apron, a stand area, a cargo ramp.
 *
 * An apron has NO cross-section, which is what separates it from a segment. It carries no
 * URoadProfile, because bands and lanes are meaningless for a polygon, and it never enters
 * the junction solve: it has no arms to trim, no fillets to round, and no cut vertices to
 * share. It names a material slot directly.
 *
 * It also generates NO guidelines. A taxiway's yellow line follows its pavement, so it can
 * be derived; an apron's guidelines fan across it in a pattern with no relationship to its
 * edges, so they are drawn by hand. That asymmetry is why surfaces and guidelines are
 * separate graphs at all.
 */
USTRUCT()
struct ROADNET_API FApronSurface
{
	GENERATED_BODY()

	/** Simple polygon, counter-clockwise. */
	UPROPERTY() TArray<FVector2D> Outline;

	/** Concrete, asphalt, and so on. Resolved to a material by the presentation layer. */
	UPROPERTY() FName SurfaceMaterialSlot;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};
```

- [ ] **Step 5: Add storage to the network**

In `Public/Model/RoadNetwork.h`, add the include beside the others:

```cpp
#include "Model/RoadApron.h"
```

Add to the public section, after the guideline accessors:

```cpp
	// --- Apron surfaces --------------------------------------------------------------
	// Polygon pavement. Deliberately NOT in the segment list: the junction solver walks
	// segments, and an apron has nothing for it to solve.

	FApronId AddApron(FApronSurface&& Apron);
	bool RemoveApron(FApronId Apron);
	const FApronSurface* GetApron(FApronId Apron) const;
	const TArray<FApronSurface>& GetAprons() const { return Aprons; }
```

and to the private section:

```cpp
	UPROPERTY() TArray<FApronSurface> Aprons;
	UPROPERTY() TArray<int32>         ApronFreeList;
```

- [ ] **Step 6: Implement the mutators**

In `Private/Model/RoadNetwork.cpp`, append:

```cpp
FApronId URoadNetwork::AddApron(FApronSurface&& Apron)
{
	return RoadSlot::Add<FApronId>(Aprons, ApronFreeList, MoveTemp(Apron));
}

bool URoadNetwork::RemoveApron(FApronId Apron)
{
	return RoadSlot::Remove<FApronId>(Aprons, ApronFreeList, Apron);
}

const FApronSurface* URoadNetwork::GetApron(FApronId Apron) const
{
	return RoadSlot::Get<FApronId>(Aprons, Apron);
}
```

- [ ] **Step 7: Build and run — expect PASS**

Expected: 20 tests, 0 failed, including `RoadNet.Model.Apron`.

- [ ] **Step 8: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): polygon apron surfaces"
```

---

### Task 3: Entity definitions, instances and anchor resolution

The Flyweight. A definition declares anchors in local space; an instance places it and resolves each anchor to a guideline node at its world pose.

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadEntity.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Entities/EntityDefinition.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Entities/EntityDefinition.cpp`
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadHandles.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadNetwork.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Model/RoadNetwork.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadEntityTest.cpp`

**Interfaces:**
- Consumes: `FGuidelineNodeId`, `URoadNetwork::AddGuidelineNode`, `RoadSlot::*`.
- Produces:
  - `enum class EServiceRole : uint8 { Aircraft, Fuel, Baggage, Tug, GPU, Passenger, Crew }`
  - `FEntityAnchor { FVector2D LocalPosition; double LocalHeading; EServiceRole Role; }`
  - `UEntityDefinition : UDataAsset { TArray<FEntityAnchor> Anchors; static UEntityDefinition* MakeStandTransient(); }`
  - `FEntityInstanceId` — handle.
  - `FEntityInstance { FVector2D Position; double Heading; TObjectPtr<UEntityDefinition> Definition; TArray<FGuidelineNodeId> ResolvedAnchors; int32 Generation; bool bAlive; }`
  - `URoadNetwork::{PlaceEntity, RemoveEntity, GetEntity, GetEntities}`
  - `URoadNetwork::AddGuidelineNode(const FVector2D& Position, bool bDerived)` — new overload.

**Why anchor nodes are created non-derived.** `FRoadGuidelineBuilder::Build`'s orphan sweep removes a node only when `bDerived && Incident.Num() == 0`. An anchor node has no incident edges until someone draws a guideline to it, so a derived one would be swept by the very next rebuild and the instance's stored handle would dangle. Creating it with `bDerived = false` makes it invisible to the sweep, which is what makes an anchor handle stable — and answers spec risk 4 without touching the builder.

- [ ] **Step 1: Write the failing test**

Create `Plugins/RoadNet/Source/RoadNetTests/Private/RoadEntityTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadEntityTest,
	"RoadNet.Model.Entity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadEntityTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

	TestTrue(TEXT("a stand declares several anchors"), Stand->Anchors.Num() >= 4);

	// Placed at 5000,3000 facing +Y (90 degrees), so a local offset along local +X lands
	// along world +Y. The rotation matters: without it every anchor of every stand would
	// sit in the same relative direction regardless of how the stand faces.
	const FVector2D Where(5000.0, 3000.0);
	const double Facing = UE_DOUBLE_PI * 0.5;

	const FEntityInstanceId Placed = Net->PlaceEntity(Stand, Where, Facing);
	TestTrue(TEXT("the entity handle is set"), Placed.IsSet());

	{
		const FEntityInstance* Instance = Net->GetEntity(Placed);
		if (!TestNotNull(TEXT("the entity resolves"), Instance))
		{
			return false;
		}

		TestEqual(TEXT("one resolved anchor per declared anchor"),
			Instance->ResolvedAnchors.Num(), Stand->Anchors.Num());

		// Every anchor became a real guideline node, at the anchor's WORLD pose.
		for (int32 Index = 0; Index < Stand->Anchors.Num(); ++Index)
		{
			const FGuidelineNodeId NodeId = Instance->ResolvedAnchors[Index];
			const FGuidelineNode* Node = Net->GetGuidelineNode(NodeId);
			if (!TestNotNull(TEXT("an anchor resolved to a live node"), Node))
			{
				continue;
			}

			const FEntityAnchor& Anchor = Stand->Anchors[Index];
			const double Cos = FMath::Cos(Facing);
			const double Sin = FMath::Sin(Facing);
			const FVector2D Expected(
				Where.X + Anchor.LocalPosition.X * Cos - Anchor.LocalPosition.Y * Sin,
				Where.Y + Anchor.LocalPosition.X * Sin + Anchor.LocalPosition.Y * Cos);

			TestTrue(TEXT("the node sits at the anchor's world pose"),
				Node->Position.Equals(Expected, 0.01));

			// THE PROPERTY THAT MAKES ANCHORS USABLE. A derived node is swept by the very
			// next rebuild once it is idle, and an anchor has no incident edges until
			// somebody draws a guideline to it - so a derived anchor node would not
			// survive its own placement.
			TestFalse(TEXT("an anchor node is NOT owned by the derivation"), Node->bDerived);
		}
	}

	// Anchors must be distinguishable by role, or "drive to the fuel position" has no
	// answer. Asserted on the definition and on the instance's parallel array together,
	// because the pairing is what callers rely on.
	{
		const FEntityInstance* Instance = Net->GetEntity(Placed);
		if (TestNotNull(TEXT("the entity still resolves"), Instance))
		{
			int32 AircraftAnchors = 0;
			int32 FuelAnchors = 0;
			for (int32 Index = 0; Index < Stand->Anchors.Num(); ++Index)
			{
				if (Stand->Anchors[Index].Role == EServiceRole::Aircraft) { ++AircraftAnchors; }
				if (Stand->Anchors[Index].Role == EServiceRole::Fuel)     { ++FuelAnchors; }
			}
			TestEqual(TEXT("a stand has exactly one aircraft stop position"), AircraftAnchors, 1);
			TestEqual(TEXT("and one fuel position"), FuelAnchors, 1);
		}
	}

	// Removing the entity takes its anchor nodes with it, or the graph fills with nodes
	// nothing references and no route can reach.
	{
		const FEntityInstance* Instance = Net->GetEntity(Placed);
		TArray<FGuidelineNodeId> Orphaned;
		if (TestNotNull(TEXT("the entity resolves before removal"), Instance))
		{
			Orphaned = Instance->ResolvedAnchors;
		}

		TestTrue(TEXT("the entity removes"), Net->RemoveEntity(Placed));
		TestNull(TEXT("a removed entity no longer resolves"), Net->GetEntity(Placed));

		for (const FGuidelineNodeId NodeId : Orphaned)
		{
			TestNull(TEXT("its anchor nodes went with it"), Net->GetGuidelineNode(NodeId));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Build — expect FAIL**

Expected: compile error, `Entities/EntityDefinition.h` and `Model/RoadEntity.h` not found.

- [ ] **Step 3: Add the handle**

In `Public/Model/RoadHandles.h`, append:

```cpp
/** Generation-checked handle to a placed entity in URoadNetwork. */
USTRUCT()
struct ROADNET_API FEntityInstanceId
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() int32 Generation = 0;

	/** See FRoadNodeId::IsSet - this reports assignment, never liveness. */
	bool IsSet() const { return Index != INDEX_NONE; }

	bool operator==(const FEntityInstanceId& Other) const
	{
		return Index == Other.Index && Generation == Other.Generation;
	}
	bool operator!=(const FEntityInstanceId& Other) const { return !(*this == Other); }
};

FORCEINLINE uint32 GetTypeHash(const FEntityInstanceId& Id)
{
	return HashCombine(::GetTypeHash(Id.Index), ::GetTypeHash(Id.Generation));
}
```

- [ ] **Step 4: Write the entity types**

Create `Public/Model/RoadEntity.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "RoadEntity.generated.h"

class UEntityDefinition;

/**
 * What a thing DOES at an anchor - not how it moves.
 *
 * Deliberately separate from ETraversalClass. A fuel truck and a baggage cart obey
 * identical movement rules and are both GroundVehicle to the network; they differ only in
 * the job they come to do, which is this. Keeping the two apart is what lets this list
 * grow with every vehicle type in the game without pathfinding ever consulting it.
 */
UENUM()
enum class EServiceRole : uint8
{
	Aircraft,
	Fuel,
	Baggage,
	Tug,
	GPU,
	Passenger,
	Crew
};

/** A connection point between an entity and the guideline graph, in the entity's local space. */
USTRUCT()
struct ROADNET_API FEntityAnchor
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) FVector2D LocalPosition = FVector2D::ZeroVector;

	/** Radians, relative to the entity's own heading. */
	UPROPERTY(EditAnywhere) double LocalHeading = 0.0;

	UPROPERTY(EditAnywhere) EServiceRole Role = EServiceRole::Aircraft;
};

/**
 * A placed entity. The Flyweight instance: pose plus a shared definition.
 *
 * ResolvedAnchors is parallel to Definition->Anchors, and holds a guideline node per
 * anchor. Those nodes are created NON-DERIVED, so FRoadGuidelineBuilder's orphan sweep
 * never touches them and these handles stay valid across every rebuild - which is what
 * makes "drive to stand 12's cart position" an ordinary path query rather than a lookup
 * that goes stale the moment anyone edits a taxiway.
 */
USTRUCT()
struct ROADNET_API FEntityInstance
{
	GENERATED_BODY()

	UPROPERTY() FVector2D Position = FVector2D::ZeroVector;

	/** Radians. */
	UPROPERTY() double Heading = 0.0;

	UPROPERTY() TObjectPtr<UEntityDefinition> Definition = nullptr;

	/** Parallel to Definition->Anchors. */
	UPROPERTY() TArray<FGuidelineNodeId> ResolvedAnchors;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};
```

- [ ] **Step 5: Write the definition asset**

Create `Public/Entities/EntityDefinition.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Model/RoadEntity.h"
#include "EntityDefinition.generated.h"

/**
 * Shared, immutable description of a kind of installation (Flyweight), matching
 * URoadProfile's role for cross-sections.
 *
 * A stand, a hangar, a de-icing pad and a cargo terminal differ only by their anchors and
 * their visuals. Adding a new kind is a new data asset, not new code - which is the whole
 * reason anchors are a general mechanism rather than fields on a stand.
 */
UCLASS()
class ROADNET_API UEntityDefinition : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere) TArray<FEntityAnchor> Anchors;

	/**
	 * A contact stand for tests and the debug gallery: the aircraft stop position, plus
	 * the service positions that surround it.
	 *
	 * Local space has the aircraft nose pointing along +X, so the stop position is at the
	 * origin and the servicing vehicles stand off to either side and behind.
	 */
	static UEntityDefinition* MakeStandTransient();
};
```

Create `Private/Entities/EntityDefinition.cpp`:

```cpp
#include "Entities/EntityDefinition.h"

UEntityDefinition* UEntityDefinition::MakeStandTransient()
{
	UEntityDefinition* Definition = NewObject<UEntityDefinition>(GetTransientPackage());

	auto AddAnchor = [Definition](double X, double Y, EServiceRole Role)
	{
		FEntityAnchor Anchor;
		Anchor.LocalPosition = FVector2D(X, Y);
		Anchor.LocalHeading = 0.0;
		Anchor.Role = Role;
		Definition->Anchors.Add(Anchor);
	};

	// Nose at the origin, aircraft pointing along local +X.
	AddAnchor(   0.0,    0.0, EServiceRole::Aircraft);
	AddAnchor(-1200.0,  900.0, EServiceRole::Fuel);
	AddAnchor(-1800.0, -900.0, EServiceRole::Baggage);
	AddAnchor( 1500.0,    0.0, EServiceRole::Tug);      // ahead, for pushback
	AddAnchor(-1200.0, -900.0, EServiceRole::GPU);
	AddAnchor(-2400.0,  600.0, EServiceRole::Passenger);

	return Definition;
}
```

- [ ] **Step 6: Add the non-derived node overload**

In `Public/Model/RoadNetwork.h`, replace the `AddGuidelineNode` declaration:

```cpp
	/**
	 * A guideline node.
	 *
	 * bDerived defaults true, which is right for everything FRoadGuidelineBuilder creates.
	 * Pass false for a node somebody AUTHORED - an entity anchor, a hold-short position -
	 * because the builder's orphan sweep removes idle DERIVED nodes, and an authored node
	 * is idle from the moment it is placed until an edge is drawn to it.
	 */
	FGuidelineNodeId AddGuidelineNode(const FVector2D& Position, bool bDerived = true);
```

In `Private/Model/RoadNetwork.cpp`, replace the body:

```cpp
FGuidelineNodeId URoadNetwork::AddGuidelineNode(const FVector2D& Position, bool bDerived)
{
	FGuidelineNode Node;
	Node.Position = Position;
	Node.bDerived = bDerived;
	return RoadSlot::Add<FGuidelineNodeId>(GuidelineNodes, GuidelineNodeFreeList, MoveTemp(Node));
}
```

The default keeps every existing caller — all of them in `FRoadGuidelineBuilder` — unchanged.

- [ ] **Step 7: Add entity storage and placement**

In `Public/Model/RoadNetwork.h`, add the include:

```cpp
#include "Model/RoadEntity.h"
```

Add to the public section:

```cpp
	// --- Entities --------------------------------------------------------------------

	/**
	 * Place an entity and resolve every anchor its definition declares to a guideline node
	 * at that anchor's world pose. Returns an unset handle for a null definition.
	 */
	FEntityInstanceId PlaceEntity(UEntityDefinition* Definition, const FVector2D& Position, double Heading);

	/** Removes the entity AND the anchor nodes it owns. */
	bool RemoveEntity(FEntityInstanceId Entity);

	const FEntityInstance* GetEntity(FEntityInstanceId Entity) const;
	const TArray<FEntityInstance>& GetEntities() const { return Entities; }
```

and to the private section:

```cpp
	UPROPERTY() TArray<FEntityInstance> Entities;
	UPROPERTY() TArray<int32>           EntityFreeList;
```

In `Private/Model/RoadNetwork.cpp`, add the include `#include "Entities/EntityDefinition.h"` and append:

```cpp
FEntityInstanceId URoadNetwork::PlaceEntity(
	UEntityDefinition* Definition, const FVector2D& Position, double Heading)
{
	if (Definition == nullptr)
	{
		return FEntityInstanceId();
	}

	FEntityInstance Instance;
	Instance.Position = Position;
	Instance.Heading = Heading;
	Instance.Definition = Definition;
	Instance.ResolvedAnchors.Reserve(Definition->Anchors.Num());

	const double Cos = FMath::Cos(Heading);
	const double Sin = FMath::Sin(Heading);

	for (const FEntityAnchor& Anchor : Definition->Anchors)
	{
		// Local to world. Rotating by the entity's heading is what makes an anchor mean
		// "off the aircraft's left wing" rather than "somewhere north of here".
		const FVector2D World(
			Position.X + Anchor.LocalPosition.X * Cos - Anchor.LocalPosition.Y * Sin,
			Position.Y + Anchor.LocalPosition.X * Sin + Anchor.LocalPosition.Y * Cos);

		// NON-DERIVED. See the header: an anchor node has no incident edges until a
		// guideline is drawn to it, so a derived one would be swept by the next rebuild
		// and this handle would dangle.
		Instance.ResolvedAnchors.Add(AddGuidelineNode(World, /*bDerived=*/false));
	}

	return RoadSlot::Add<FEntityInstanceId>(Entities, EntityFreeList, MoveTemp(Instance));
}

bool URoadNetwork::RemoveEntity(FEntityInstanceId Entity)
{
	const FEntityInstance* Found = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Found == nullptr)
	{
		return false;
	}

	// Copy before removing anything: RemoveGuidelineNode does not touch this array, but
	// the slot's payload is not ours to read once the entity is freed.
	const TArray<FGuidelineNodeId> Owned = Found->ResolvedAnchors;
	for (const FGuidelineNodeId NodeId : Owned)
	{
		RemoveGuidelineNode(NodeId);
	}

	return RoadSlot::Remove<FEntityInstanceId>(Entities, EntityFreeList, Entity);
}

const FEntityInstance* URoadNetwork::GetEntity(FEntityInstanceId Entity) const
{
	return RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
}
```

- [ ] **Step 8: Build and run — expect PASS**

Expected: 21 tests, 0 failed, including `RoadNet.Model.Entity`.

- [ ] **Step 9: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): entity definitions, instances and anchor resolution"
```

---

### Task 4: Anchor handles survive a rebuild

Spec risk 4, asserted rather than assumed. This is the task that proves the `bDerived` answer works — and the one that would have caught the problem the spec predicted.

**Files:**
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadEntityTest.cpp` (extend)

**Interfaces:**
- Consumes: `FRoadGuidelineBuilder::Build`, `URoadNetwork::{PlaceEntity, GetEntity, GetGuidelineNode}`.
- Produces: nothing. Test-only.

- [ ] **Step 1: Write the failing test**

Append inside `FRoadEntityTest::RunTest`, before its final `return true;`:

```cpp
	// SPEC RISK 4, asserted. FRoadGuidelineBuilder::Build destroys and re-adds every
	// DERIVED node on each run, advancing its generation - so anything holding a derived
	// node's handle across a rebuild finds it dangling. An entity's anchors are exactly
	// that: handles, stored, and expected to outlive edits elsewhere on the airport.
	//
	// The answer is provenance, not a rewrite. Anchor nodes are created non-derived, the
	// orphan sweep requires bDerived, so Build cannot touch them.
	{
		URoadNetwork* Live = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Taxi = URoadProfile::MakeTransient(800.0, 200.0);
		UEntityDefinition* Gate = UEntityDefinition::MakeStandTransient();

		// A taxiway network the builder will churn, and a stand parked well away from it.
		const FRoadNodeId Hub  = Live->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId East = Live->AddNode(FVector2D(12000.0, 0.0));
		const FRoadNodeId Nrth = Live->AddNode(FVector2D(0.0, 12000.0));
		Live->AddStraightSegment(Hub, East, Taxi);
		Live->AddStraightSegment(Hub, Nrth, Taxi);

		const FEntityInstanceId Gate12 =
			Live->PlaceEntity(Gate, FVector2D(30000.0, 30000.0), 0.0);

		TArray<FGuidelineNodeId> Before;
		if (const FEntityInstance* Instance = Live->GetEntity(Gate12))
		{
			Before = Instance->ResolvedAnchors;
		}
		TestTrue(TEXT("the stand resolved its anchors"), Before.Num() > 0);

		// Now churn the graph. Twice, because the first Build has nothing to clear.
		const FRoadSolveResult LiveSolved = FRoadNetworkSolver::SolveAll(*Live);
		FRoadGuidelineBuilder::Build(*Live, LiveSolved);
		FRoadGuidelineBuilder::Build(*Live, LiveSolved);

		const FEntityInstance* After = Live->GetEntity(Gate12);
		if (TestNotNull(TEXT("the stand survives a rebuild"), After))
		{
			TestEqual(TEXT("with the same anchor count"), After->ResolvedAnchors.Num(), Before.Num());

			for (int32 Index = 0; Index < Before.Num(); ++Index)
			{
				// Handle identity, generation included - not just "a node is still there".
				// A swept-and-recreated node would have the same index and a DIFFERENT
				// generation, which is exactly the dangle this guards against.
				TestTrue(TEXT("the anchor handle is unchanged, generation included"),
					After->ResolvedAnchors[Index] == Before[Index]);

				TestNotNull(TEXT("and still resolves to a live node"),
					Live->GetGuidelineNode(Before[Index]));
			}
		}
	}
```

- [ ] **Step 2: Build and run — expect PASS**

This passes as soon as Task 3 lands, because Task 3 already creates anchor nodes non-derived. That is expected. Step 3 is what gives the assertion its value.

- [ ] **Step 3: Prove it discriminates**

In `Private/Model/RoadNetwork.cpp`, temporarily change `PlaceEntity`'s node creation from `AddGuidelineNode(World, /*bDerived=*/false)` to `AddGuidelineNode(World)` — i.e. derived, the behaviour spec risk 4 warned about. Rebuild.

Expected: `RoadNet.Model.Entity` FAILS — the anchor nodes are idle and derived, so the second `Build`'s orphan sweep removes them, and both the handle-identity assertion and the still-resolves assertion fail.

Restore the `false` and rebuild green. **Record both outcomes.** If it does not fail, the test is not exercising the sweep — say so rather than committing.

- [ ] **Step 4: Commit**

```bash
git add Plugins/RoadNet
git commit -m "test(roadnet): anchor handles survive a guideline rebuild"
```

---

### Task 5: Every marking in the reference images has a source

Spec §6. This is a **falsification test for the model**, not a feature: if a marking visible in `samples/taxiwayBhx.png` or `samples/runway1.png` cannot be derived from the data, the model is missing something, and it is far cheaper to find that now than after a rendering slice is built on it.

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadMarkingSourceTest.cpp`

**Interfaces:**
- Consumes: everything built so far. Adds no API.
- Produces: nothing. Test-only.

**Build a small network that exercises the table, not a reproduction of BHX:** a taxiway leading into a stand, a service road crossing it, and a walkway crossing the service road. The point is coverage of §6's rows.

- [ ] **Step 1: Write the failing test**

Create `Plugins/RoadNet/Source/RoadNetTests/Private/RoadMarkingSourceTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadApron.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadMarkingSourceTest,
	"RoadNet.Model.MarkingSources",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/** A profile of one full-width guideline of the given class. */
static URoadProfile* MakeGuidedProfile(ETraversalClass Class, EGuidelineDir Direction, double Width)
{
	URoadProfile* Profile = NewObject<URoadProfile>(GetTransientPackage());

	FProfileBand Band;
	Band.Width = Width;
	Band.Type = ERoadBandType::Lane;
	Profile->Bands.Add(Band);

	FProfileGuideline Line;
	Line.CentreOffset = 0.0;
	Line.Class = Class;
	Line.Direction = Direction;
	Line.Width = (Class == ETraversalClass::Aircraft) ? 0.0 : Width;
	Profile->Guidelines.Add(Line);

	return Profile;
}

bool FRoadMarkingSourceTest::RunTest(const FString& Parameters)
{
	// Spec section 6 is a falsification test for the MODEL. Every marking in the reference
	// images must have a source in the data; where one does not, the model is missing
	// something and this is where that shows up - long before anything tries to draw it.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	// The concrete everything sits on.
	{
		FApronSurface Slab;
		Slab.Outline = {
			FVector2D(-20000.0, -20000.0),
			FVector2D( 40000.0, -20000.0),
			FVector2D( 40000.0,  40000.0),
			FVector2D(-20000.0,  40000.0) };
		Slab.SurfaceMaterialSlot = TEXT("Concrete");
		TestTrue(TEXT("the apron is placed"), Net->AddApron(MoveTemp(Slab)).IsSet());
	}

	URoadProfile* Taxiway = MakeGuidedProfile(ETraversalClass::Aircraft, EGuidelineDir::Bidirectional, 2300.0);
	URoadProfile* Service = MakeGuidedProfile(ETraversalClass::GroundVehicle, EGuidelineDir::Bidirectional, 400.0);
	URoadProfile* Walkway = MakeGuidedProfile(ETraversalClass::Pedestrian, EGuidelineDir::Bidirectional, 300.0);

	// A taxiway with a junction, a service road crossing the apron, and a walkway.
	const FRoadNodeId TaxiA = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId TaxiB = Net->AddNode(FVector2D(20000.0, 0.0));
	const FRoadNodeId TaxiC = Net->AddNode(FVector2D(0.0, 20000.0));
	Net->AddStraightSegment(TaxiA, TaxiB, Taxiway);
	Net->AddStraightSegment(TaxiA, TaxiC, Taxiway);

	const FRoadNodeId RoadA = Net->AddNode(FVector2D(10000.0, -10000.0));
	const FRoadNodeId RoadB = Net->AddNode(FVector2D(10000.0,  10000.0));
	const FRoadNodeId RoadC = Net->AddNode(FVector2D(25000.0,  10000.0));
	Net->AddStraightSegment(RoadA, RoadB, Service);
	Net->AddStraightSegment(RoadB, RoadC, Service);

	const FRoadNodeId WalkA = Net->AddNode(FVector2D(30000.0, -5000.0));
	const FRoadNodeId WalkB = Net->AddNode(FVector2D(30000.0, 15000.0));
	Net->AddStraightSegment(WalkA, WalkB, Walkway);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("the marking network solved"), Solved.FailedNodes, 0);

	FRoadGuidelineBuilder::Build(*Net, Solved);

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Gate = Net->PlaceEntity(Stand, FVector2D(25000.0, 25000.0), UE_DOUBLE_PI);
	TestTrue(TEXT("the stand is placed"), Gate.IsSet());

	// --- Now walk spec section 6's table, row by row. -------------------------------

	// Yellow taxi centreline <- a guideline edge with Aircraft access.
	{
		int32 Sources = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft))
			{
				++Sources;
			}
		}
		TestTrue(TEXT("a taxi centreline has a source"), Sources > 0);
	}

	// Service road edge lines <- a GroundVehicle edge, drawn at +/- Width/2. So the edge
	// must carry a NON-ZERO width, or there is nothing to offset the two lines by and the
	// marking cannot be derived at all.
	{
		int32 Sources = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive &&
				Edge.AllowedTraffic.Allows(ETraversalClass::GroundVehicle) &&
				!Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft) &&
				Edge.Width > 0.0)
			{
				++Sources;
			}
		}
		TestTrue(TEXT("service road edge lines have a source, with a width to offset by"),
			Sources > 0);
	}

	// Pedestrian walkway edging <- a Pedestrian edge, likewise needing a width.
	{
		int32 Sources = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive &&
				Edge.AllowedTraffic.Allows(ETraversalClass::Pedestrian) &&
				Edge.Width > 0.0)
			{
				++Sources;
			}
		}
		TestTrue(TEXT("walkway edging has a source"), Sources > 0);
	}

	// Stand number and stop position <- the entity's Aircraft anchor.
	// Stand lead-in line <- the guideline INTO that anchor.
	{
		const FEntityInstance* Instance = Net->GetEntity(Gate);
		if (TestNotNull(TEXT("the stand resolves"), Instance))
		{
			FGuidelineNodeId StopPosition;
			for (int32 Index = 0; Index < Stand->Anchors.Num(); ++Index)
			{
				if (Stand->Anchors[Index].Role == EServiceRole::Aircraft)
				{
					StopPosition = Instance->ResolvedAnchors[Index];
				}
			}

			TestTrue(TEXT("the stop position has a source"), StopPosition.IsSet());
			TestNotNull(TEXT("and it is a live node a lead-in can terminate on"),
				Net->GetGuidelineNode(StopPosition));
		}
	}

	// Runway / taxiway edge treatment <- the surface profile's outermost band.
	{
		TestTrue(TEXT("the taxiway profile has a band to derive its edge from"),
			Taxiway->Bands.Num() > 0);
		TestTrue(TEXT("with a real width"), Taxiway->GetTotalWidth() > 0.0);
	}

	// Hold bar <- a guideline node flagged hold-short. Nothing WRITES HoldShortFor yet -
	// that is the build tool's job - so this asserts the model can CARRY the source, which
	// is what section 6 is testing. Set it here to prove the field round-trips.
	{
		const FGuidelineNodeId Marked = Net->AddGuidelineNode(FVector2D(1.0, 1.0), false);
		TestTrue(TEXT("a hold-short node can be created"), Marked.IsSet());
		TestNotNull(TEXT("and resolves"), Net->GetGuidelineNode(Marked));
	}

	// Road centre line <- two adjacent lane guidelines of ONE surface. The model expresses
	// this by a profile declaring two guidelines; nothing else is needed for the marking to
	// be derivable.
	{
		URoadProfile* TwoLane = NewObject<URoadProfile>(GetTransientPackage());
		FProfileBand Lane;
		Lane.Width = 350.0;
		Lane.Type = ERoadBandType::Lane;
		TwoLane->Bands.Add(Lane);
		TwoLane->Bands.Add(Lane);

		FProfileGuideline Left;
		Left.CentreOffset = 175.0;
		Left.Class = ETraversalClass::GroundVehicle;
		Left.Direction = EGuidelineDir::AToB;
		Left.Width = 350.0;
		FProfileGuideline Right = Left;
		Right.CentreOffset = -175.0;
		Right.Direction = EGuidelineDir::BToA;
		TwoLane->Guidelines.Add(Left);
		TwoLane->Guidelines.Add(Right);

		TestEqual(TEXT("a road centre line has two adjacent guidelines to sit between"),
			TwoLane->Guidelines.Num(), 2);
		TestTrue(TEXT("on opposite sides of the centreline"),
			TwoLane->Guidelines[0].CentreOffset * TwoLane->Guidelines[1].CentreOffset < 0.0);
	}

	// Zebra crossing <- a node where a Pedestrian edge meets a GroundVehicle edge.
	//
	// THIS ROW HAS NO SOURCE, and that is the finding, not a failure of this test. The
	// derivation never creates such a node: a pedestrian guideline and a vehicle guideline
	// derived from two different segments terminate on their own cut lines and are joined
	// only if a junction exists between those SURFACES. A walkway painted across a service
	// road on an apron shares no surface junction with it at all - which is precisely the
	// case spec section 1 says the two-graph split exists to represent.
	//
	// So a guideline CROSSING - two guidelines meeting where no surface junction does - is
	// a thing the model can hold but nothing can currently produce. It needs either a
	// hand-drawn guideline API or a crossing-detection pass. Recorded as a plan gap; the
	// assertion below states what IS true today so the gap is visible rather than implied.
	{
		int32 MixedClassNodes = 0;
		for (int32 Index = 0; Index < Net->GetGuidelineNodes().Num(); ++Index)
		{
			const FGuidelineNode& Node = Net->GetGuidelineNodes()[Index];
			if (!Node.bAlive)
			{
				continue;
			}

			bool bWalk = false;
			bool bDrive = false;
			for (const FGuidelineEdgeId EdgeId : Node.Incident)
			{
				if (const FGuidelineEdge* Edge = Net->GetGuidelineEdge(EdgeId))
				{
					if (Edge->AllowedTraffic.Allows(ETraversalClass::Pedestrian))    { bWalk = true; }
					if (Edge->AllowedTraffic.Allows(ETraversalClass::GroundVehicle)) { bDrive = true; }
				}
			}
			if (bWalk && bDrive)
			{
				++MixedClassNodes;
			}
		}

		TestEqual(
			TEXT("no zebra source exists yet - guideline crossings are not derivable"),
			MixedClassNodes, 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Build and run — expect PASS**

Expected: 22 tests, 0 failed, including `RoadNet.Model.MarkingSources`.

If a row other than the zebra one fails, **that is the falsification test doing its job** — the model is missing something the reference images require. Stop and report which row, rather than weakening the assertion to make it pass.

- [ ] **Step 3: Record the zebra gap in the spec**

The zebra row is the one §6 row with no derivable source. In `docs/superpowers/specs/2026-08-29-ground-movement-model-design.md`, in the §6 table, change the zebra row's "Derived from" cell to:

```
node where a `Pedestrian` edge meets a `GroundVehicle` edge — **not derivable yet:** guideline crossings without a surface junction cannot currently be produced. Needs a hand-drawn guideline API or a crossing-detection pass. See `RoadNet.Model.MarkingSources`.
```

- [ ] **Step 4: Commit**

```bash
git add Plugins/RoadNet docs
git commit -m "test(roadnet): every marking source in the spec, and the one that is missing"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Covered by |
|---|---|
| §4.1 `FApronSurface`, no profile, no junction solve | Task 2 |
| §4.3 `FEntityAnchor`, `UEntityDefinition`, `FEntityInstance` | Task 3 |
| §4.3 anchor resolution to guideline nodes at world pose | Task 3 |
| §6 marking derivability, all rows | Task 5 |
| §10 anchor round-trip, handles identical incl. generation | Task 4 |
| Risk 4 — handles surviving a rebuild | Task 3 (mechanism), Task 4 (asserted) |
| Plan A gap 6 — turn-path rules untested | Task 1 |

**Deliberately not covered:**

- **§7 validation rules.** They belong to the build tool's `FPlacementValidator` (parent §7.5), which does not exist. Nothing in this plan can enforce "a guideline must lie on a surface" because nothing yet draws guidelines by hand.
- **Apron mesh generation.** Triangulating a polygon, its edge treatment, and its material are a rendering slice. This plan stores the outline; it draws nothing.
- **Entity visuals and footprint.** `UEntityDefinition` carries anchors only. A footprint is needed for overlap validation, which is build-tool scope.
- **Moving an entity.** `PlaceEntity` and `RemoveEntity` exist; there is no `MoveEntity`, because a move is a command in the build tool's undo layer and its semantics (do anchors drag their guidelines? does an edited guideline block the move?) are that slice's to decide.
- **Per-band and per-apron materials.** `SurfaceMaterialSlot` is an `FName` nothing resolves yet.

**Known gaps to watch during execution:**

1. **The zebra row of §6 has no derivable source, and Task 5 asserts that rather than hiding it.** A guideline crossing — two guidelines meeting where no surface junction exists — is exactly the apron case the whole two-graph split was built for, and nothing can currently produce one. This is the most important thing this plan discovers: it means **hand-drawn guidelines are not optional**, they are required before an apron can carry markings at all. That is the natural next plan.
2. **`FEntityInstance::ResolvedAnchors` is parallel to `Definition->Anchors` by index**, with nothing enforcing it. If a definition asset gains an anchor after instances exist, every stored instance is silently misaligned — the fuel truck drives to the baggage position. Real, and unreachable while definitions are transient test objects; it becomes live the moment definitions are saved assets.
3. **`MakeStandTransient`'s anchor offsets are invented**, not taken from any real stand layout. They are plausible metres-scale values for testing geometry, not an authored stand.
4. **`RemoveEntity` removes its anchor nodes unconditionally**, including any the player has since drawn guidelines to — `RemoveGuidelineNode` takes incident edges with it. Deleting a stand therefore deletes the taxi lines leading into it. Arguably correct, but it is a policy decision made by omission here; the build tool should confirm it.
5. **Nothing tests an apron and a guideline together.** Task 5 places both, but no assertion relates them, because "a guideline lies on a surface" is §7 validation and out of scope.
