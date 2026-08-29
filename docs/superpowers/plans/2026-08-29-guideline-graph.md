# Guideline Graph Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the guideline graph — the network agents are routed along — as a second graph inside `URoadNetwork`, derived from surfaces but independently editable.

**Architecture:** Surfaces and guidelines are separate graphs. A profile declares which guidelines its cross-section produces; a segment generates one edge per declared guideline, and a junction generates one edge per ordered pair of arms (the turn paths). Guideline endpoints are shared **by handle**, not by coincident position, so this graph has no weld contract and its geometry may be recomputed freely.

**Tech Stack:** Unreal Engine 5.8.2, C++20, MSVC 14.51.36231, Unreal Automation Test framework.

**Spec:** `docs/superpowers/specs/2026-08-29-ground-movement-model-design.md` (§2, §3, §4.2, §5.1, §5.2, §5.4, §5.6, §8, §10). Parent: `2026-08-28-procedural-road-system-design.md` §4, §5.8, R3, R9.

**Scope:** This is **Plan A of two**. Aprons (`FApronSurface`), entities, anchors and the §6 marking-derivation test are Plan B and are deliberately absent here. Nothing in this plan depends on them.

## Global Constraints

- **Engine:** Unreal Engine 5.8.2 at `D:\Epic\UE_5.8`. Project at `C:\repos\AirportMgr2`.
- **`BuildSettingsVersion.V7`** — return-type, dangling-reference and unreachable-code warnings are **errors**.
- **`Solve/` must keep ZERO engine dependencies** beyond `CoreMinimal.h`. This plan changes nothing under `Solve/`.
- `Build/` depends on `Model`, `Profiles`, `Solve`. `Present/` depends on `Build`. Nothing depends upward.
- **Automation flags:** `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter`. The nested form does not compile in 5.8.
- Test files wrapped in `#if WITH_DEV_AUTOMATION_TESTS` … `#endif`.
- **`FVector2D` is double-precision. Never narrow to `float`** for positions.
- Unreal `F`/`U`/`A`/`E` prefixes are required by UnrealHeaderTool. Reflected types need `USTRUCT()`/`UENUM()` and `GENERATED_BODY()`.
- **Slot-map types must expose `int32 Generation` and `bool bAlive`**, or `RoadSlot::Add` will not compile against them.
- **This graph has NO bitwise weld contract.** Do not copy the surface model's `==`-on-position discipline into it; guideline endpoints are shared by handle. Recomputing a guideline's geometry is safe and expected.

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

Judge every run by the wrapper's parsed output. **Never report a result from a build that did not print `Result: Succeeded`** — with one standing exception: `Plugins/McpAutomationBridge` is git-ignored, unrelated, and currently fails to compile (a string literal over MSVC's 16380-char limit). It has a stale DLL so it does not block the test run. Confirm the RoadNet modules linked, then judge by the test wrapper.

The suite is at **16 tests** before this plan.

---

## File Structure

```
Plugins/RoadNet/Source/RoadNet/
  Public/Model/RoadHandles.h            MODIFY  - guideline node/edge handles
  Public/Model/RoadTraffic.h            CREATE  - traversal classes, mask, priority
  Private/Model/RoadTraffic.cpp         CREATE
  Public/Model/RoadGuideline.h          CREATE  - FGuidelineNode, FGuidelineEdge
  Public/Model/RoadNetwork.h            MODIFY  - guideline storage and accessors
  Private/Model/RoadNetwork.cpp         MODIFY
  Public/Profiles/RoadProfile.h         MODIFY  - FProfileGuideline replaces FProfileLane
  Private/Profiles/RoadProfile.cpp      MODIFY
  Public/Build/RoadGuidelineBuilder.h   CREATE  - derives guidelines from a solved network
  Private/Build/RoadGuidelineBuilder.cpp CREATE

Plugins/RoadNet/Source/RoadNetTests/Private/
  RoadTrafficTest.cpp                   CREATE  - mask and priority, pure
  RoadGuidelineGraphTest.cpp            CREATE  - storage, handles, incidence
  RoadGuidelineBuilderTest.cpp          CREATE  - derivation, sharing, idempotence
```

---

### Task 1: Traversal classes, traffic mask and priority

The vocabulary everything else is expressed in. Kept in `Model/` with no dependencies so both the graph and later simulation code can use it.

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadTraffic.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Model/RoadTraffic.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadTrafficTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `enum class ETraversalClass : uint8 { Aircraft, GroundVehicle, Pedestrian, Emergency }`
  - `struct FTrafficMask { uint8 Bits; bool Allows(ETraversalClass) const; void Add(ETraversalClass); static FTrafficMask All(); static FTrafficMask Only(ETraversalClass); }`
  - `enum class EGuidelineDir : uint8 { Bidirectional, AToB, BToA }`
  - `int32 TraversalPriority(ETraversalClass)`
  - `ETraversalClass ResolveRightOfWay(ETraversalClass, ETraversalClass)`

- [ ] **Step 1: Write the failing test**

Create `Plugins/RoadNet/Source/RoadNetTests/Private/RoadTrafficTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadTraffic.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadTrafficTest,
	"RoadNet.Model.Traffic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadTrafficTest::RunTest(const FString& Parameters)
{
	// A default mask admits nobody. The default must be the CLOSED case: a guideline
	// created without an explicit mask should be unusable rather than universally usable,
	// so that forgetting to set access is a visible dead end and not a silent free-for-all.
	{
		const FTrafficMask Empty;
		TestFalse(TEXT("default mask admits no aircraft"), Empty.Allows(ETraversalClass::Aircraft));
		TestFalse(TEXT("default mask admits no vehicles"), Empty.Allows(ETraversalClass::GroundVehicle));
		TestFalse(TEXT("default mask admits no pedestrians"), Empty.Allows(ETraversalClass::Pedestrian));
		TestFalse(TEXT("default mask admits no emergency"), Empty.Allows(ETraversalClass::Emergency));
	}

	// Adding one class admits exactly that class - masks are a set, not a threshold.
	{
		FTrafficMask ServiceRoad;
		ServiceRoad.Add(ETraversalClass::GroundVehicle);
		ServiceRoad.Add(ETraversalClass::Emergency);

		TestTrue(TEXT("service road admits vehicles"), ServiceRoad.Allows(ETraversalClass::GroundVehicle));
		TestTrue(TEXT("service road admits emergency"), ServiceRoad.Allows(ETraversalClass::Emergency));
		TestFalse(TEXT("service road excludes aircraft"), ServiceRoad.Allows(ETraversalClass::Aircraft));
		TestFalse(TEXT("service road excludes pedestrians"), ServiceRoad.Allows(ETraversalClass::Pedestrian));
	}

	{
		const FTrafficMask All = FTrafficMask::All();
		TestTrue(TEXT("All admits aircraft"), All.Allows(ETraversalClass::Aircraft));
		TestTrue(TEXT("All admits vehicles"), All.Allows(ETraversalClass::GroundVehicle));
		TestTrue(TEXT("All admits pedestrians"), All.Allows(ETraversalClass::Pedestrian));
		TestTrue(TEXT("All admits emergency"), All.Allows(ETraversalClass::Emergency));

		const FTrafficMask OnlyAir = FTrafficMask::Only(ETraversalClass::Aircraft);
		TestTrue(TEXT("Only admits its class"), OnlyAir.Allows(ETraversalClass::Aircraft));
		TestFalse(TEXT("Only excludes the rest"), OnlyAir.Allows(ETraversalClass::Pedestrian));
	}

	// Spec 5.4: Emergency > Aircraft > Pedestrian > GroundVehicle.
	{
		TestTrue(TEXT("emergency outranks aircraft"),
			TraversalPriority(ETraversalClass::Emergency) > TraversalPriority(ETraversalClass::Aircraft));
		TestTrue(TEXT("aircraft outrank pedestrians"),
			TraversalPriority(ETraversalClass::Aircraft) > TraversalPriority(ETraversalClass::Pedestrian));
		TestTrue(TEXT("pedestrians outrank ground vehicles"),
			TraversalPriority(ETraversalClass::Pedestrian) > TraversalPriority(ETraversalClass::GroundVehicle));
	}

	// The order must be TOTAL, and right-of-way antisymmetric. Asserted exhaustively over
	// every ordered pair rather than by spot-check: a partial order here would leave some
	// crossing in the game with no defined winner, and the failure would surface as two
	// agents deadlocked rather than as anything that looks like a rule bug.
	{
		const ETraversalClass Classes[] = {
			ETraversalClass::Aircraft, ETraversalClass::GroundVehicle,
			ETraversalClass::Pedestrian, ETraversalClass::Emergency };

		for (const ETraversalClass Left : Classes)
		{
			TestEqual(TEXT("a class ties with itself"), ResolveRightOfWay(Left, Left), Left);

			for (const ETraversalClass Right : Classes)
			{
				if (Left == Right)
				{
					continue;
				}

				TestNotEqual(TEXT("distinct classes never tie in priority"),
					TraversalPriority(Left), TraversalPriority(Right));

				TestEqual(TEXT("right of way is symmetric in its argument order"),
					ResolveRightOfWay(Left, Right), ResolveRightOfWay(Right, Left));
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Build — expect FAIL**

Expected: compile error, `Model/RoadTraffic.h` not found.

- [ ] **Step 3: Write the header**

Create `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadTraffic.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "RoadTraffic.generated.h"

/**
 * How a thing MOVES - not what it does.
 *
 * A refuel truck, a baggage cart and a catering van are all GroundVehicle: they obey
 * identical movement rules and differ only in job. What they do is a service role, on an
 * entity anchor, and it is deliberately not this enum - otherwise this grows with every
 * vehicle type in the game and gets consulted by pathfinding for no reason.
 */
UENUM()
enum class ETraversalClass : uint8
{
	Aircraft,
	GroundVehicle,
	Pedestrian,
	Emergency
};

/**
 * Which traversal classes a guideline admits.
 *
 * Defaults to EMPTY, admitting nobody. A guideline whose access was never set is then a
 * visible dead end rather than a silent free-for-all - the failure that is easy to see
 * beats the failure that routes a 747 down a footpath.
 */
USTRUCT()
struct ROADNET_API FTrafficMask
{
	GENERATED_BODY()

	UPROPERTY() uint8 Bits = 0;

	bool Allows(ETraversalClass Class) const
	{
		return (Bits & (1u << static_cast<uint8>(Class))) != 0;
	}

	void Add(ETraversalClass Class)
	{
		Bits |= static_cast<uint8>(1u << static_cast<uint8>(Class));
	}

	static FTrafficMask All()
	{
		FTrafficMask Mask;
		Mask.Add(ETraversalClass::Aircraft);
		Mask.Add(ETraversalClass::GroundVehicle);
		Mask.Add(ETraversalClass::Pedestrian);
		Mask.Add(ETraversalClass::Emergency);
		return Mask;
	}

	static FTrafficMask Only(ETraversalClass Class)
	{
		FTrafficMask Mask;
		Mask.Add(Class);
		return Mask;
	}
};

/** Which way a guideline may be traversed. */
UENUM()
enum class EGuidelineDir : uint8
{
	Bidirectional,
	AToB,
	BToA
};

/**
 * Right-of-way rank. Higher wins. Spec 5.4:
 *
 *     Emergency > Aircraft > Pedestrian > GroundVehicle
 *
 * On the class rather than on the edge or the crossing, so the common case needs no
 * authoring at all. Authoring priority per crossing would be laborious AND wrong the
 * moment a fire truck arrives - the fire truck's precedence is a fact about fire trucks,
 * not about any particular junction.
 *
 * Every value is distinct, which makes the order total: no crossing can be reached that
 * has no defined winner.
 */
ROADNET_API int32 TraversalPriority(ETraversalClass Class);

/** Which of two contending classes proceeds. Returns the class itself when they match. */
ROADNET_API ETraversalClass ResolveRightOfWay(ETraversalClass A, ETraversalClass B);
```

- [ ] **Step 4: Write the implementation**

Create `Plugins/RoadNet/Source/RoadNet/Private/Model/RoadTraffic.cpp`:

```cpp
#include "Model/RoadTraffic.h"

int32 TraversalPriority(ETraversalClass Class)
{
	switch (Class)
	{
	case ETraversalClass::Emergency:     return 3;
	case ETraversalClass::Aircraft:      return 2;
	case ETraversalClass::Pedestrian:    return 1;
	case ETraversalClass::GroundVehicle: return 0;
	}

	// Unreachable for any declared value. Returning the lowest rank rather than asserting
	// means a future class added without updating this yields to everything, which is the
	// safe direction to be wrong in.
	return 0;
}

ETraversalClass ResolveRightOfWay(ETraversalClass A, ETraversalClass B)
{
	return TraversalPriority(A) >= TraversalPriority(B) ? A : B;
}
```

Note the `switch` has no `default:` and every enumerator is covered, so it compiles clean
under V7's return-type-warnings-as-errors while still returning after the switch.

- [ ] **Step 5: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

Expected: 17 tests, 0 failed, including `RoadNet.Model.Traffic`.

- [ ] **Step 6: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): traversal classes, traffic mask and right of way"
```

---

### Task 2: Guideline handles and graph storage

The graph itself, in `URoadNetwork` beside the surface graph, using the same slot-map machinery so it inherits generation-checked handles and will inherit the command/undo layer unchanged.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadHandles.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadGuideline.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadNetwork.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Model/RoadNetwork.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadGuidelineGraphTest.cpp`

**Interfaces:**
- Consumes: `FTrafficMask`, `ETraversalClass` (Task 1); `RoadSlot::{Add, Get, Remove, IsValid}`.
- Produces:
  - `FGuidelineNodeId`, `FGuidelineEdgeId` — handles, same shape as `FRoadNodeId`.
  - `FGuidelineNode { FVector2D Position; TArray<FGuidelineEdgeId> Incident; FRoadSegmentId HoldShortFor; TArray<ETraversalClass> PriorityOverride; int32 Generation; bool bAlive; }`
  - `FGuidelineEdge { FGuidelineNodeId A, B; FVector2D Control; FTrafficMask AllowedTraffic; EGuidelineDir Direction; double Width; double MaxWingspan; FRoadSegmentId DerivedFrom; bool bDerived; int32 Generation; bool bAlive; }`
  - `URoadNetwork::{AddGuidelineNode, AddGuidelineEdge, RemoveGuidelineNode, RemoveGuidelineEdge, GetGuidelineNode, GetGuidelineEdge, GetGuidelineEdgeMutable, GetGuidelineNodes, GetGuidelineEdges}`

- [ ] **Step 1: Write the failing test**

Create `Plugins/RoadNet/Source/RoadNetTests/Private/RoadGuidelineGraphTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadGuidelineGraphTest,
	"RoadNet.Model.GuidelineGraph",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadGuidelineGraphTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));

	TestTrue(TEXT("a new guideline node handle is set"), A.IsSet());
	TestNotNull(TEXT("a new guideline node resolves"), Net->GetGuidelineNode(A));

	FGuidelineEdge Edge;
	Edge.A = A;
	Edge.B = B;
	Edge.Control = FVector2D(500.0, 0.0);
	Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::Aircraft);
	Edge.Direction = EGuidelineDir::Bidirectional;
	Edge.Width = 0.0;

	const FGuidelineEdgeId Id = Net->AddGuidelineEdge(MoveTemp(Edge));
	TestTrue(TEXT("a new guideline edge handle is set"), Id.IsSet());

	// Incidence is maintained by the network, not by the caller. A caller-maintained
	// adjacency list is the classic way for a graph to go quietly inconsistent.
	{
		const FGuidelineNode* NodeA = Net->GetGuidelineNode(A);
		const FGuidelineNode* NodeB = Net->GetGuidelineNode(B);
		if (TestNotNull(TEXT("node A resolves"), NodeA) && TestNotNull(TEXT("node B resolves"), NodeB))
		{
			TestTrue(TEXT("the edge is incident to A"), NodeA->Incident.Contains(Id));
			TestTrue(TEXT("the edge is incident to B"), NodeB->Incident.Contains(Id));
		}
	}

	// Removing an edge must retract it from BOTH endpoints, or a later traversal walks a
	// dead handle.
	{
		TestTrue(TEXT("the edge removes"), Net->RemoveGuidelineEdge(Id));
		TestNull(TEXT("a removed edge no longer resolves"), Net->GetGuidelineEdge(Id));

		const FGuidelineNode* NodeA = Net->GetGuidelineNode(A);
		const FGuidelineNode* NodeB = Net->GetGuidelineNode(B);
		if (TestNotNull(TEXT("node A still resolves"), NodeA) && TestNotNull(TEXT("node B still resolves"), NodeB))
		{
			TestFalse(TEXT("A no longer lists the edge"), NodeA->Incident.Contains(Id));
			TestFalse(TEXT("B no longer lists the edge"), NodeB->Incident.Contains(Id));
		}
	}

	// Generation checking, which is the whole point of the handle. A recycled slot must
	// NOT resolve through the old handle - the failure it prevents is an edit silently
	// landing on whatever object took the slot over.
	{
		const FGuidelineNodeId Doomed = Net->AddGuidelineNode(FVector2D(50.0, 50.0));
		TestTrue(TEXT("the doomed node removes"), Net->RemoveGuidelineNode(Doomed));

		const FGuidelineNodeId Recycled = Net->AddGuidelineNode(FVector2D(60.0, 60.0));
		TestEqual(TEXT("the slot was reused"), Recycled.Index, Doomed.Index);
		TestNotEqual(TEXT("but the generation moved on"), Recycled.Generation, Doomed.Generation);
		TestNull(TEXT("the stale handle does not resolve"), Net->GetGuidelineNode(Doomed));
		TestNotNull(TEXT("the fresh handle does"), Net->GetGuidelineNode(Recycled));
	}

	// Removing a node takes its edges with it. Leaving them would strand edges pointing at
	// a dead node, which reads as a graph with a hole rather than as a removal.
	{
		const FGuidelineNodeId L = Net->AddGuidelineNode(FVector2D(0.0, 500.0));
		const FGuidelineNodeId R = Net->AddGuidelineNode(FVector2D(0.0, 900.0));

		FGuidelineEdge Span;
		Span.A = L;
		Span.B = R;
		const FGuidelineEdgeId SpanId = Net->AddGuidelineEdge(MoveTemp(Span));

		TestTrue(TEXT("the endpoint node removes"), Net->RemoveGuidelineNode(L));
		TestNull(TEXT("its edge went with it"), Net->GetGuidelineEdge(SpanId));

		const FGuidelineNode* Survivor = Net->GetGuidelineNode(R);
		if (TestNotNull(TEXT("the far node survives"), Survivor))
		{
			TestFalse(TEXT("and no longer lists the edge"), Survivor->Incident.Contains(SpanId));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Build — expect FAIL**

Expected: compile error, `Model/RoadGuideline.h` not found and `URoadNetwork` has no member `AddGuidelineNode`.

- [ ] **Step 3: Add the handles**

In `Public/Model/RoadHandles.h`, append before the final newline — the same shape as the
two existing handles, deliberately duplicated rather than templated, because
UnrealHeaderTool cannot reflect a template instantiation:

```cpp
/** Generation-checked handle to a guideline node in URoadNetwork. */
USTRUCT()
struct ROADNET_API FGuidelineNodeId
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() int32 Generation = 0;

	/** See FRoadNodeId::IsSet - this reports assignment, never liveness. */
	bool IsSet() const { return Index != INDEX_NONE; }

	bool operator==(const FGuidelineNodeId& Other) const
	{
		return Index == Other.Index && Generation == Other.Generation;
	}
	bool operator!=(const FGuidelineNodeId& Other) const { return !(*this == Other); }
};

FORCEINLINE uint32 GetTypeHash(const FGuidelineNodeId& Id)
{
	return HashCombine(::GetTypeHash(Id.Index), ::GetTypeHash(Id.Generation));
}

/** Generation-checked handle to a guideline edge in URoadNetwork. */
USTRUCT()
struct ROADNET_API FGuidelineEdgeId
{
	GENERATED_BODY()

	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() int32 Generation = 0;

	/** See FRoadNodeId::IsSet - this reports assignment, never liveness. */
	bool IsSet() const { return Index != INDEX_NONE; }

	bool operator==(const FGuidelineEdgeId& Other) const
	{
		return Index == Other.Index && Generation == Other.Generation;
	}
	bool operator!=(const FGuidelineEdgeId& Other) const { return !(*this == Other); }
};

FORCEINLINE uint32 GetTypeHash(const FGuidelineEdgeId& Id)
{
	return HashCombine(::GetTypeHash(Id.Index), ::GetTypeHash(Id.Generation));
}
```

- [ ] **Step 4: Write the guideline entities**

Create `Public/Model/RoadGuideline.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/RoadTraffic.h"
#include "RoadGuideline.generated.h"

/**
 * A point on the guideline graph where something happens.
 *
 * Nodes exist at junctions, crossings, hold-short positions and entity anchors - NOT at
 * a fixed interval. Spec 3: a node every N metres has nothing to say to anybody, and the
 * parent spec's R9 subdivision was justified by a pathing benefit that moved to this
 * graph when the two graphs were separated.
 */
USTRUCT()
struct ROADNET_API FGuidelineNode
{
	GENERATED_BODY()

	UPROPERTY() FVector2D Position = FVector2D::ZeroVector;

	/** Maintained by URoadNetwork. Never edit from outside it. */
	UPROPERTY() TArray<FGuidelineEdgeId> Incident;

	/** Set when this node requires clearance; names the surface it protects. Spec 5.5. */
	UPROPERTY() FRoadSegmentId HoldShortFor;

	/**
	 * Overrides the default class priority at this node. Empty - the overwhelmingly
	 * common case - means TraversalPriority applies. Spec 5.4.
	 */
	UPROPERTY() TArray<ETraversalClass> PriorityOverride;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};

/**
 * A line an agent is told to follow.
 *
 * NOTE, because the surface model next door does the opposite: this graph has NO bitwise
 * weld contract. Endpoints are shared by HANDLE, not by coincident position, so guideline
 * geometry may be recomputed freely and no seam can open. Do not import the surface
 * model's ==-on-position discipline here.
 */
USTRUCT()
struct ROADNET_API FGuidelineEdge
{
	GENERATED_BODY()

	UPROPERTY() FGuidelineNodeId A;
	UPROPERTY() FGuidelineNodeId B;

	/** Quadratic Bezier control point, as FRoadSegment. Equals the midpoint when straight. */
	UPROPERTY() FVector2D Control = FVector2D::ZeroVector;

	/** Who may use this. Defaults to nobody - see FTrafficMask. */
	UPROPERTY() FTrafficMask AllowedTraffic;

	UPROPERTY() EGuidelineDir Direction = EGuidelineDir::Bidirectional;

	/**
	 * Physical extent of the path in uu, driving marking geometry and clearance.
	 *
	 * NOT a capacity. Abreast concurrency is structural - two lanes are two guidelines -
	 * and flow-versus-single-file is a property of the traversal class, not of the edge.
	 * A four-metre service road could hold two vans abreast and never does. Spec 5.3.
	 */
	UPROPERTY() double Width = 0.0;

	/** 0 means unlimited. Spec 5.6. */
	UPROPERTY() double MaxWingspan = 0.0;

	/** The surface this was derived from; unset when hand-drawn. */
	UPROPERTY() FRoadSegmentId DerivedFrom;

	/**
	 * True while this edge is still owned by its surface and may be regenerated.
	 *
	 * Flips to false on first manual edit and never flips back on its own. Regeneration
	 * must then leave it alone, because regenerating it would silently discard the edit.
	 */
	UPROPERTY() bool bDerived = true;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};
```

- [ ] **Step 5: Add storage to the network**

In `Public/Model/RoadNetwork.h`, add to the public section after `GetOtherEnd`:

```cpp
	// --- Guideline graph -------------------------------------------------------------
	// A SECOND graph, deliberately in the same object. The build tool must make "draw a
	// taxiway" one atomic undo step spanning pavement and its derived guideline, and
	// Revert must restore handles identically including generation counters; splitting
	// the two graphs across two UObjects makes every composite command a two-phase commit
	// for no gain. Conceptual separation lives in the headers, not in the ownership.

	FGuidelineNodeId AddGuidelineNode(const FVector2D& Position);

	/** Removes the node AND every edge incident to it. */
	bool RemoveGuidelineNode(FGuidelineNodeId Node);

	/** A and B must both be live, or this returns an unset handle and adds nothing. */
	FGuidelineEdgeId AddGuidelineEdge(FGuidelineEdge&& Edge);
	bool RemoveGuidelineEdge(FGuidelineEdgeId Edge);

	const FGuidelineNode* GetGuidelineNode(FGuidelineNodeId Node) const;
	const FGuidelineEdge* GetGuidelineEdge(FGuidelineEdgeId Edge) const;
	FGuidelineEdge*       GetGuidelineEdgeMutable(FGuidelineEdgeId Edge);

	const TArray<FGuidelineNode>& GetGuidelineNodes() const { return GuidelineNodes; }
	const TArray<FGuidelineEdge>& GetGuidelineEdges() const { return GuidelineEdges; }
```

and to the private section:

```cpp
	UPROPERTY() TArray<FGuidelineNode> GuidelineNodes;
	UPROPERTY() TArray<int32>          GuidelineNodeFreeList;
	UPROPERTY() TArray<FGuidelineEdge> GuidelineEdges;
	UPROPERTY() TArray<int32>          GuidelineEdgeFreeList;
```

and add the include beside the existing ones:

```cpp
#include "Model/RoadGuideline.h"
```

- [ ] **Step 6: Implement the mutators**

In `Private/Model/RoadNetwork.cpp`, append:

```cpp
FGuidelineNodeId URoadNetwork::AddGuidelineNode(const FVector2D& Position)
{
	FGuidelineNode Node;
	Node.Position = Position;
	return RoadSlot::Add<FGuidelineNodeId>(GuidelineNodes, GuidelineNodeFreeList, MoveTemp(Node));
}

FGuidelineEdgeId URoadNetwork::AddGuidelineEdge(FGuidelineEdge&& Edge)
{
	// Both endpoints must be live BEFORE anything is added, or a rejected edge leaves a
	// half-linked graph behind.
	if (!RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, Edge.A) ||
		!RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, Edge.B))
	{
		return FGuidelineEdgeId();
	}

	const FGuidelineNodeId EndA = Edge.A;
	const FGuidelineNodeId EndB = Edge.B;

	const FGuidelineEdgeId Handle =
		RoadSlot::Add<FGuidelineEdgeId>(GuidelineEdges, GuidelineEdgeFreeList, MoveTemp(Edge));

	GuidelineNodes[EndA.Index].Incident.Add(Handle);
	if (EndB != EndA)
	{
		GuidelineNodes[EndB.Index].Incident.Add(Handle);
	}

	return Handle;
}

bool URoadNetwork::RemoveGuidelineEdge(FGuidelineEdgeId Edge)
{
	const FGuidelineEdge* Found =
		RoadSlot::Get<FGuidelineEdgeId>(GuidelineEdges, Edge);
	if (Found == nullptr)
	{
		return false;
	}

	// Retract from BOTH endpoints before freeing the slot - after Remove the payload is
	// still there but the generation has moved on, so read the endpoints now.
	const FGuidelineNodeId EndA = Found->A;
	const FGuidelineNodeId EndB = Found->B;

	if (RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, EndA))
	{
		GuidelineNodes[EndA.Index].Incident.Remove(Edge);
	}
	if (RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, EndB))
	{
		GuidelineNodes[EndB.Index].Incident.Remove(Edge);
	}

	return RoadSlot::Remove<FGuidelineEdgeId>(GuidelineEdges, GuidelineEdgeFreeList, Edge);
}

bool URoadNetwork::RemoveGuidelineNode(FGuidelineNodeId Node)
{
	if (!RoadSlot::IsValid<FGuidelineNodeId>(GuidelineNodes, Node))
	{
		return false;
	}

	// Copy the incidence list before removing anything: RemoveGuidelineEdge mutates it.
	const TArray<FGuidelineEdgeId> Doomed = GuidelineNodes[Node.Index].Incident;
	for (const FGuidelineEdgeId Edge : Doomed)
	{
		RemoveGuidelineEdge(Edge);
	}

	return RoadSlot::Remove<FGuidelineNodeId>(GuidelineNodes, GuidelineNodeFreeList, Node);
}

const FGuidelineNode* URoadNetwork::GetGuidelineNode(FGuidelineNodeId Node) const
{
	return RoadSlot::Get<FGuidelineNodeId>(GuidelineNodes, Node);
}

const FGuidelineEdge* URoadNetwork::GetGuidelineEdge(FGuidelineEdgeId Edge) const
{
	return RoadSlot::Get<FGuidelineEdgeId>(GuidelineEdges, Edge);
}

FGuidelineEdge* URoadNetwork::GetGuidelineEdgeMutable(FGuidelineEdgeId Edge)
{
	return RoadSlot::Get<FGuidelineEdgeId>(GuidelineEdges, Edge);
}
```

- [ ] **Step 7: Build and run — expect PASS**

Expected: 18 tests, 0 failed, including `RoadNet.Model.GuidelineGraph`.

- [ ] **Step 8: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): guideline graph storage on the network"
```

---

### Task 3: Profiles declare the guidelines they generate

`FProfileLane` is written by `MakeTransient` and read by **nothing** — verified 2026-08-29. Replacing it now costs one struct change and no migration; after the build tool starts authoring profile assets it costs a migration instead.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Profiles/RoadProfile.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Profiles/RoadProfile.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadProfileTest.cpp` (extend)

**Interfaces:**
- Consumes: `ETraversalClass`, `EGuidelineDir` (Task 1).
- Produces:
  - `struct FProfileGuideline { double CentreOffset; ETraversalClass Class; EGuidelineDir Direction; double Width; double MaxWingspan; }`
  - `URoadProfile::Guidelines` replaces `URoadProfile::Lanes`.

- [ ] **Step 1: Write the failing test**

Append inside `FRoadProfileTest::RunTest`, immediately before its final `return true;`:

```cpp
	// A taxiway declares ONE guideline, not two lanes. An aircraft occupies the full width
	// with its nose wheel on the line; there is no second lane to be in. Parent R3's
	// "per-lane turn paths" is corrected by spec 3 to guideline-level for exactly this.
	{
		URoadProfile* Taxiway = URoadProfile::MakeTransient(2300.0, 1500.0);
		TestEqual(TEXT("a taxiway declares one guideline"), Taxiway->Guidelines.Num(), 1);
		TestEqual(TEXT("centred on the profile"), Taxiway->Guidelines[0].CentreOffset, 0.0);
		TestEqual(TEXT("carrying aircraft"),
			Taxiway->Guidelines[0].Class, ETraversalClass::Aircraft);
		TestEqual(TEXT("in both directions"),
			Taxiway->Guidelines[0].Direction, EGuidelineDir::Bidirectional);
	}

	// A road is the case that recovers the original per-lane meaning: two guidelines,
	// mirrored offsets, opposing directions. Built by hand because MakeTransient only ever
	// produces the symmetric single-guideline profile.
	{
		URoadProfile* Road = NewObject<URoadProfile>(GetTransientPackage());

		FProfileBand Lane;
		Lane.Width = 350.0;
		Lane.Type = ERoadBandType::Lane;
		Road->Bands.Add(Lane);
		Road->Bands.Add(Lane);

		FProfileGuideline Left;
		Left.CentreOffset = 175.0;
		Left.Class = ETraversalClass::GroundVehicle;
		Left.Direction = EGuidelineDir::AToB;
		Left.Width = 350.0;

		FProfileGuideline Right = Left;
		Right.CentreOffset = -175.0;
		Right.Direction = EGuidelineDir::BToA;

		Road->Guidelines.Add(Left);
		Road->Guidelines.Add(Right);

		TestEqual(TEXT("a two-lane road declares two guidelines"), Road->Guidelines.Num(), 2);
		TestEqual(TEXT("mirrored about the centreline"),
			Road->Guidelines[0].CentreOffset, -Road->Guidelines[1].CentreOffset);
		TestNotEqual(TEXT("running opposite ways"),
			Road->Guidelines[0].Direction, Road->Guidelines[1].Direction);
	}
```

- [ ] **Step 2: Build — expect FAIL**

Expected: compile error, `URoadProfile` has no member `Guidelines` and `FProfileGuideline` is undeclared.

- [ ] **Step 3: Replace the lane struct**

In `Public/Profiles/RoadProfile.h`, add the include beside `CoreMinimal.h`. `Model/RoadTraffic.h`, not `Model/RoadGuideline.h` — a profile needs the traffic *vocabulary*, not the graph entities, and `RoadNode.h` forward-declares `URoadProfile` rather than including it, so this direction stays acyclic:

```cpp
#include "Model/RoadTraffic.h"
```

Replace `FProfileLane` entirely:

```cpp
/**
 * One guideline this cross-section generates.
 *
 * This replaces FProfileLane, which modelled a road lane and had no reader. A taxiway
 * declares exactly one of these; a two-lane road declares two with mirrored offsets and
 * opposing directions, which is the case that recovers what "lane" used to mean.
 */
USTRUCT()
struct ROADNET_API FProfileGuideline
{
	GENERATED_BODY()

	/** Lateral offset from the centreline in uu: positive left, negative right. */
	UPROPERTY(EditAnywhere) double CentreOffset = 0.0;

	UPROPERTY(EditAnywhere) ETraversalClass Class = ETraversalClass::Aircraft;
	UPROPERTY(EditAnywhere) EGuidelineDir Direction = EGuidelineDir::Bidirectional;

	/** Physical extent for marking and clearance. NOT a capacity - see FGuidelineEdge. */
	UPROPERTY(EditAnywhere) double Width = 0.0;

	/** 0 means unlimited. */
	UPROPERTY(EditAnywhere) double MaxWingspan = 0.0;
};
```

and replace the `Lanes` property on `URoadProfile`:

```cpp
	UPROPERTY(EditAnywhere) TArray<FProfileGuideline> Guidelines;
```

- [ ] **Step 4: Update MakeTransient**

In `Private/Profiles/RoadProfile.cpp`, replace the `FProfileLane DriveLane;` block — the
four lines from `FProfileLane DriveLane;` down to `Profile->Lanes.Add(DriveLane);` — with:

```cpp
	// One guideline, centred, bidirectional, carrying aircraft: a taxiway. Every existing
	// caller of MakeTransient is a taxiway or a stand-in for one, and the lane it used to
	// declare was read by nothing.
	FProfileGuideline Centre;
	Centre.CentreOffset = 0.0;
	Centre.Class = ETraversalClass::Aircraft;
	Centre.Direction = EGuidelineDir::Bidirectional;
	Centre.Width = 0.0;
	Profile->Guidelines.Add(Centre);
```

- [ ] **Step 5: Build and run — expect PASS**

Expected: 18 tests, 0 failed. If `RoadNet.Model.Profile` fails on a lane assertion, the
existing test still refers to `Lanes`; update that assertion to `Guidelines` rather than
reinstating the old field.

- [ ] **Step 6: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): profiles declare guidelines, absorbing FProfileLane"
```

---

### Task 4: Derive guideline edges from segments

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Build/RoadGuidelineBuilder.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Build/RoadGuidelineBuilder.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadGuidelineBuilderTest.cpp`

**Interfaces:**
- Consumes: `FRoadSolveResult` (`Build/RoadNetworkSolver.h`), `FRoadMeshBuilder::CutLinePoint`, `URoadProfile::Guidelines`, `URoadNetwork::{AddGuidelineNode, AddGuidelineEdge}`.
- Produces:
  - `struct FRoadGuidelineBuilder { static void Build(URoadNetwork& Network, const FRoadSolveResult& Solved); }`

**Why endpoints sit on the cut lines:** a segment's guideline must stop where its pavement stops, so the turn paths Task 5 adds can carry it across the junction. The endpoint is the point on the stored cut line at the guideline's lateral offset, reached through `FRoadMeshBuilder::CutLinePoint` so there is one lerp expression in the codebase. Reuse here is tidiness, not contract: this graph welds by handle, so a differing low bit would cost nothing.

- [ ] **Step 1: Write the failing test**

Create `Plugins/RoadNet/Source/RoadNetTests/Private/RoadGuidelineBuilderTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadGuidelineBuilderTest,
	"RoadNet.Build.GuidelineBuilder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadGuidelineBuilderTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(800.0, 200.0);

	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(12000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 12000.0));
	const FRoadSegmentId ToEast = Net->AddStraightSegment(Centre, East,  Profile);
	Net->AddStraightSegment(Centre, North, Profile);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node solved"), Solved.FailedNodes, 0);

	FRoadGuidelineBuilder::Build(*Net, Solved);

	// One edge per segment per declared guideline. The taxiway profile declares one, and
	// there are two segments, so exactly two edges carry a DerivedFrom naming a segment.
	{
		int32 SegmentEdges = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.DerivedFrom.IsSet())
			{
				++SegmentEdges;
			}
		}
		TestEqual(TEXT("one guideline edge per segment"), SegmentEdges, 2);
	}

	// The edge must inherit the profile's access and direction, or the guideline exists
	// and admits nobody - which looks like a pathfinding bug, not a derivation bug.
	{
		bool bFound = false;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (!Edge.bAlive || Edge.DerivedFrom != ToEast)
			{
				continue;
			}
			bFound = true;
			TestTrue(TEXT("the derived edge admits aircraft"),
				Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft));
			TestFalse(TEXT("and not pedestrians"),
				Edge.AllowedTraffic.Allows(ETraversalClass::Pedestrian));
			TestEqual(TEXT("bidirectional, as the profile declares"),
				Edge.Direction, EGuidelineDir::Bidirectional);
			TestTrue(TEXT("and is marked derived"), Edge.bDerived);
		}
		TestTrue(TEXT("the east segment produced an edge"), bFound);
	}

	// Endpoints sit ON the segment's stored cut lines, not at its node positions. A
	// guideline that ran node-to-node would overlap the junction it should hand off to.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);
		if (!TestNotNull(TEXT("east segment resolves"), Seg))
		{
			return false;
		}

		const FVector2D ExpectedA = FMath::Lerp(Seg->RightCutA, Seg->LeftCutA, 0.5);

		bool bOnCutLine = false;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (!Edge.bAlive || Edge.DerivedFrom != ToEast)
			{
				continue;
			}
			const FGuidelineNode* NodeA = Net->GetGuidelineNode(Edge.A);
			const FGuidelineNode* NodeB = Net->GetGuidelineNode(Edge.B);
			if (NodeA != nullptr && NodeB != nullptr)
			{
				bOnCutLine =
					NodeA->Position.Equals(ExpectedA, 0.01) ||
					NodeB->Position.Equals(ExpectedA, 0.01);
			}
		}
		TestTrue(TEXT("an endpoint sits on the A-end cut line"), bOnCutLine);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Build — expect FAIL**

Expected: compile error, `Build/RoadGuidelineBuilder.h` not found.

- [ ] **Step 3: Write the builder header**

Create `Public/Build/RoadGuidelineBuilder.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadHandles.h"

class URoadNetwork;

/**
 * Derives the guideline graph from a solved surface network.
 *
 * A segment contributes one edge per guideline its profile declares. A junction
 * contributes one edge per ORDERED pair of distinct arms - the parent spec's 5.8 turn
 * paths - expressed as ordinary guideline edges so pathfinding never special-cases a
 * junction.
 *
 * Endpoints are shared by HANDLE. A segment edge and the turn paths that continue it
 * reference the same FGuidelineNodeId, which is what makes the graph connected; nothing
 * here depends on two positions being bitwise equal, unlike the surface mesh next door.
 */
struct ROADNET_API FRoadGuidelineBuilder
{
	/**
	 * Rebuilds every DERIVED guideline in Network from Solved.
	 *
	 * Edges with bDerived == false are left untouched, along with the nodes they need.
	 */
	static void Build(URoadNetwork& Network, const FRoadSolveResult& Solved);
};
```

- [ ] **Step 4: Write the segment derivation**

Create `Private/Build/RoadGuidelineBuilder.cpp`:

```cpp
#include "Build/RoadGuidelineBuilder.h"

#include "Build/RoadMeshBuilder.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

namespace
{
	/**
	 * Where a guideline crosses a cut line, as a lerp parameter from the right cut to the
	 * left. Mirrors FRoadProfileBands' convention so the two never disagree about which
	 * way "right to left" runs.
	 */
	double AlphaForOffset(const URoadProfile* Profile, double CentreOffset)
	{
		const double HalfLeft  = Profile ? FMath::Max(Profile->GetHalfWidthLeft(),  0.0) : 0.0;
		const double HalfRight = Profile ? FMath::Max(Profile->GetHalfWidthRight(), 0.0) : 0.0;
		const double Total = HalfLeft + HalfRight;
		if (Total <= 0.0)
		{
			return 0.5;
		}
		return FMath::Clamp((CentreOffset + HalfRight) / Total, 0.0, 1.0);
	}
}

void FRoadGuidelineBuilder::Build(URoadNetwork& Network, const FRoadSolveResult& Solved)
{
	const TArray<FRoadSegment>& Segments = Network.GetSegments();

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegment& Segment = Segments[Index];
		if (!Segment.bAlive || !Segment.bSolvedA || !Segment.bSolvedB)
		{
			continue;
		}

		FRoadSegmentId SegmentId;
		SegmentId.Index = Index;
		SegmentId.Generation = Segment.Generation;

		const URoadProfile* Profile = Segment.Profile.Get();
		if (Profile == nullptr)
		{
			continue;
		}

		for (const FProfileGuideline& Declared : Profile->Guidelines)
		{
			const double Alpha = AlphaForOffset(Profile, Declared.CentreOffset);

			// End B's cut line is authored from B's point of view, so its left is this
			// segment's right walking A to B - swapped here exactly as AddSegment swaps it.
			const FVector2D AtA =
				FRoadMeshBuilder::CutLinePoint(Segment.RightCutA, Segment.LeftCutA, Alpha);
			const FVector2D AtB =
				FRoadMeshBuilder::CutLinePoint(Segment.LeftCutB, Segment.RightCutB, Alpha);

			FGuidelineEdge Edge;
			Edge.A = Network.AddGuidelineNode(AtA);
			Edge.B = Network.AddGuidelineNode(AtB);
			Edge.Control = (AtA + AtB) * 0.5;   // straight until curves are derived
			Edge.AllowedTraffic = FTrafficMask::Only(Declared.Class);
			Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
			Edge.Direction = Declared.Direction;
			Edge.Width = Declared.Width;
			Edge.MaxWingspan = Declared.MaxWingspan;
			Edge.DerivedFrom = SegmentId;
			Edge.bDerived = true;

			Network.AddGuidelineEdge(MoveTemp(Edge));
		}
	}
}
```

`Emergency` is added to every derived edge because spec §5.1 puts it on nearly everything;
a surface an emergency vehicle may not reach is the exception and must be authored.

- [ ] **Step 5: Build and run — expect PASS**

Expected: 19 tests, 0 failed, including `RoadNet.Build.GuidelineBuilder`.

- [ ] **Step 6: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): derive guideline edges from segments"
```

---

### Task 5: Derive turn paths at junctions

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Build/RoadGuidelineBuilder.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadGuidelineBuilderTest.cpp` (extend)

**Interfaces:**
- Consumes: `FRoadSolveResult::{NodeResults, NodeArmSegments}`, `FJunctionResult::Arms`.
- Produces: no new public API. `Build` additionally emits turn-path edges.

**Two decisions, and why:**

*Quadratic, not the parent spec's cubic.* §5.8 says cubic with control points along the two tangents. Both arms' tangent lines meet at the node, so the control point they define is a single point — which is exactly the quadratic case. A cubic here would carry two control points describing one, and `FGuidelineEdge` matches `FRoadSegment` in holding one.

*Ordered pairs, `AToB`, no U-turns.* One edge per ordered pair of **distinct** arms, each `AToB`. A bidirectional pair therefore yields two edges rather than one bidirectional edge. That is uniform, needs no special case when the arms are directional, and lets a one-way road produce only the legal direction. A U-turn at a junction is not a taxi movement and is not emitted.

- [ ] **Step 1: Write the failing test**

Append inside `FRoadGuidelineBuilderTest::RunTest`, before its final `return true;`:

```cpp
	// Turn paths. The centre node has two arms, so two ordered pairs, so two turn edges.
	// A turn edge is recognised by carrying no DerivedFrom - it belongs to a junction
	// rather than to any one segment.
	{
		int32 TurnEdges = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && !Edge.DerivedFrom.IsSet())
			{
				++TurnEdges;
				TestEqual(TEXT("a turn path runs one way"), Edge.Direction, EGuidelineDir::AToB);
			}
		}
		TestEqual(TEXT("two arms give two ordered turn paths"), TurnEdges, 2);
	}

	// THE CONNECTIVITY PROPERTY. A turn path must reuse the SAME node handles its segments
	// end on, or the graph is a heap of disconnected sticks that each look fine on their
	// own and that nothing can route across. Handles, not positions - this graph has no
	// weld contract, so two coincident-but-distinct nodes would be invisible to any
	// position check while being fatal to pathfinding.
	{
		TSet<FGuidelineNodeId> SegmentEnds;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.DerivedFrom.IsSet())
			{
				SegmentEnds.Add(Edge.A);
				SegmentEnds.Add(Edge.B);
			}
		}

		int32 Connected = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && !Edge.DerivedFrom.IsSet() &&
				SegmentEnds.Contains(Edge.A) && SegmentEnds.Contains(Edge.B))
			{
				++Connected;
			}
		}
		TestEqual(TEXT("every turn path joins two segment endpoints"), Connected, 2);
	}

	// A three-arm node gives six ordered pairs. Asserted separately because two arms
	// cannot distinguish "ordered pairs" from "pairs" - both give two.
	{
		URoadNetwork* Tee = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* TeeProfile = URoadProfile::MakeTransient(800.0, 200.0);

		const FRoadNodeId Hub = Tee->AddNode(FVector2D(0.0, 0.0));
		Tee->AddStraightSegment(Hub, Tee->AddNode(FVector2D( 12000.0,     0.0)), TeeProfile);
		Tee->AddStraightSegment(Hub, Tee->AddNode(FVector2D(-12000.0,     0.0)), TeeProfile);
		Tee->AddStraightSegment(Hub, Tee->AddNode(FVector2D(     0.0, 12000.0)), TeeProfile);

		const FRoadSolveResult TeeSolved = FRoadNetworkSolver::SolveAll(*Tee);
		TestEqual(TEXT("the tee solved"), TeeSolved.FailedNodes, 0);

		FRoadGuidelineBuilder::Build(*Tee, TeeSolved);

		int32 TeeTurns = 0;
		for (const FGuidelineEdge& Edge : Tee->GetGuidelineEdges())
		{
			if (Edge.bAlive && !Edge.DerivedFrom.IsSet())
			{
				++TeeTurns;
			}
		}
		TestEqual(TEXT("three arms give six ordered turn paths"), TeeTurns, 6);
	}
```

- [ ] **Step 2: Build — expect FAIL**

Expected: FAIL on "two arms give two ordered turn paths" — the count is 0, because no
junction edges are emitted yet.

- [ ] **Step 3: Emit turn paths**

In `Private/Build/RoadGuidelineBuilder.cpp`, the segment loop must record the endpoint it
created for each (segment, end, guideline) so the junction loop can reuse the handle.
Replace the whole of `FRoadGuidelineBuilder::Build` with:

```cpp
void FRoadGuidelineBuilder::Build(URoadNetwork& Network, const FRoadSolveResult& Solved)
{
	// (SegmentIndex, which end, GuidelineIndex) -> the node that segment end terminates on.
	//
	// The junction loop reuses these handles rather than adding coincident nodes of its
	// own. THAT is what connects the graph: this graph shares endpoints by handle, so two
	// coincident-but-distinct nodes would satisfy every position check while leaving the
	// turn paths as separate sticks nothing can route across.
	//
	// Packed into one integer rather than given a key struct, because a local struct needs
	// a GetTypeHash that ADL can find, and hoisting one to file scope for a lookup this
	// small is not worth it.
	auto EndKey = [](int32 SegmentIndex, bool bEndA, int32 GuidelineIndex) -> uint64
	{
		return (static_cast<uint64>(SegmentIndex) << 32)
			 | (static_cast<uint64>(GuidelineIndex) << 1)
			 | (bEndA ? 1ull : 0ull);
	};

	TMap<uint64, FGuidelineNodeId> Ends;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegment& Segment = Segments[Index];
		if (!Segment.bAlive || !Segment.bSolvedA || !Segment.bSolvedB)
		{
			continue;
		}

		FRoadSegmentId SegmentId;
		SegmentId.Index = Index;
		SegmentId.Generation = Segment.Generation;

		const URoadProfile* Profile = Segment.Profile.Get();
		if (Profile == nullptr)
		{
			continue;
		}

		for (int32 Which = 0; Which < Profile->Guidelines.Num(); ++Which)
		{
			const FProfileGuideline& Declared = Profile->Guidelines[Which];
			const double Alpha = AlphaForOffset(Profile, Declared.CentreOffset);

			// End B's cut line is authored from B's point of view, so its left is this
			// segment's right walking A to B - swapped exactly as AddSegment swaps it.
			const FVector2D AtA =
				FRoadMeshBuilder::CutLinePoint(Segment.RightCutA, Segment.LeftCutA, Alpha);
			const FVector2D AtB =
				FRoadMeshBuilder::CutLinePoint(Segment.LeftCutB, Segment.RightCutB, Alpha);

			FGuidelineEdge Edge;
			Edge.A = Network.AddGuidelineNode(AtA);
			Edge.B = Network.AddGuidelineNode(AtB);
			Edge.Control = (AtA + AtB) * 0.5;
			Edge.AllowedTraffic = FTrafficMask::Only(Declared.Class);
			Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
			Edge.Direction = Declared.Direction;
			Edge.Width = Declared.Width;
			Edge.MaxWingspan = Declared.MaxWingspan;
			Edge.DerivedFrom = SegmentId;
			Edge.bDerived = true;

			Ends.Add(EndKey(Index, true,  Which), Edge.A);
			Ends.Add(EndKey(Index, false, Which), Edge.B);

			Network.AddGuidelineEdge(MoveTemp(Edge));
		}
	}

	// Turn paths: one edge per ordered pair of DISTINCT arms at each solved node.
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		const TArray<FRoadSegmentId>* ArmSegments = Solved.NodeArmSegments.Find(Pair.Key);
		if (ArmSegments == nullptr || !Pair.Value.bValid)
		{
			continue;
		}

		FRoadNodeId NodeId;
		NodeId.Index = Pair.Key;
		const FRoadNode* Node = Network.GetNodes().IsValidIndex(Pair.Key)
			? &Network.GetNodes()[Pair.Key] : nullptr;
		if (Node == nullptr || !Node->bAlive)
		{
			continue;
		}
		NodeId.Generation = Node->Generation;

		for (int32 From = 0; From < ArmSegments->Num(); ++From)
		{
			for (int32 To = 0; To < ArmSegments->Num(); ++To)
			{
				// No U-turns: a junction does not connect an arm to itself.
				if (From == To)
				{
					continue;
				}

				const FRoadSegmentId FromSeg = (*ArmSegments)[From];
				const FRoadSegmentId ToSeg   = (*ArmSegments)[To];
				const FRoadSegment* FromSegment = Network.GetSegment(FromSeg);
				const FRoadSegment* ToSegment   = Network.GetSegment(ToSeg);
				if (FromSegment == nullptr || ToSegment == nullptr)
				{
					continue;
				}

				const URoadProfile* FromProfile = FromSegment->Profile.Get();
				const URoadProfile* ToProfile   = ToSegment->Profile.Get();
				if (FromProfile == nullptr || ToProfile == nullptr)
				{
					continue;
				}

				const int32 Count = FMath::Min(
					FromProfile->Guidelines.Num(), ToProfile->Guidelines.Num());

				for (int32 Which = 0; Which < Count; ++Which)
				{
					const FGuidelineNodeId* FromEnd =
						Ends.Find(EndKey(FromSeg.Index, FromSegment->A == NodeId, Which));
					const FGuidelineNodeId* ToEnd =
						Ends.Find(EndKey(ToSeg.Index, ToSegment->A == NodeId, Which));
					if (FromEnd == nullptr || ToEnd == nullptr)
					{
						continue;
					}

					const FProfileGuideline& Declared = FromProfile->Guidelines[Which];

					FGuidelineEdge Turn;
					Turn.A = *FromEnd;
					Turn.B = *ToEnd;

					// Both arms' tangent lines meet AT the node, so the single control
					// point they define is the node itself - which is precisely the
					// quadratic case, and why this is not the parent spec's cubic.
					Turn.Control = Node->Position;

					Turn.AllowedTraffic = FTrafficMask::Only(Declared.Class);
					Turn.AllowedTraffic.Add(ETraversalClass::Emergency);
					Turn.Direction = EGuidelineDir::AToB;
					Turn.Width = Declared.Width;
					Turn.MaxWingspan = FMath::Min(
						Declared.MaxWingspan, ToProfile->Guidelines[Which].MaxWingspan);
					Turn.bDerived = true;
					// DerivedFrom stays unset: a turn path belongs to the junction, not to
					// either segment, and that is how the two are told apart.

					Network.AddGuidelineEdge(MoveTemp(Turn));
				}
			}
		}
	}
}
```

`MaxWingspan` takes the **minimum** of the two arms: a turn is only usable by an aircraft
both arms admit. Note `0` means unlimited, so a `Min` against `0` would wrongly clamp to
unlimited — if either value can be `0`, treat `0` as `TNumericLimits<double>::Max()` for
the comparison and convert back. Implement that if the test for mixed profiles is added;
with a single uniform profile both values are `0` and the naive `Min` is correct.

- [ ] **Step 4: Build and run — expect PASS**

Expected: 19 tests, 0 failed.

**If "every turn path joins two segment endpoints" reports 0**, the `Ends` lookup missed:
check that `FromSegment->A == NodeId` correctly identifies which end of that segment
touches this node, and that `NodeId.Generation` was filled from the live node.

- [ ] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): derive junction turn paths as guideline edges"
```

---

### Task 6: Regeneration is idempotent and spares edited guidelines

The property that makes "derived by default, independent in the data" (spec §4.2) real rather than aspirational.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Build/RoadGuidelineBuilder.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadGuidelineBuilderTest.cpp` (extend)

**Interfaces:**
- Consumes: `URoadNetwork::{GetGuidelineEdges, RemoveGuidelineEdge, RemoveGuidelineNode}`.
- Produces: no new public API. `Build` becomes idempotent.

- [ ] **Step 1: Write the failing test**

Append inside `FRoadGuidelineBuilderTest::RunTest`, before its final `return true;`:

```cpp
	// Idempotence. Build runs on every edit in the build tool, so a Build that accumulates
	// is a leak that grows with every mouse move - and one that looks like nothing at all
	// until the graph is large.
	{
		int32 Before = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive) { ++Before; }
		}

		FRoadGuidelineBuilder::Build(*Net, Solved);

		int32 After = 0;
		for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
		{
			if (Edge.bAlive) { ++After; }
		}

		TestEqual(TEXT("rebuilding does not accumulate edges"), After, Before);
	}

	// An edited guideline survives regeneration. Without this, a player who redraws a
	// taxi line loses it the next time anything touches the pavement - silently, because
	// the replacement looks exactly like a correct derived guideline.
	{
		FGuidelineEdgeId Edited;
		for (int32 Index = 0; Index < Net->GetGuidelineEdges().Num(); ++Index)
		{
			const FGuidelineEdge& Edge = Net->GetGuidelineEdges()[Index];
			if (Edge.bAlive && Edge.DerivedFrom.IsSet())
			{
				Edited.Index = Index;
				Edited.Generation = Edge.Generation;
				break;
			}
		}

		if (TestTrue(TEXT("found a derived edge to edit"), Edited.IsSet()))
		{
			FGuidelineEdge* Mutable = Net->GetGuidelineEdgeMutable(Edited);
			if (TestNotNull(TEXT("the edge is mutable"), Mutable))
			{
				Mutable->bDerived = false;
				Mutable->MaxWingspan = 6543.0;   // a value derivation would never produce
			}

			FRoadGuidelineBuilder::Build(*Net, Solved);

			const FGuidelineEdge* Survivor = Net->GetGuidelineEdge(Edited);
			if (TestNotNull(TEXT("the edited edge survives regeneration"), Survivor))
			{
				TestFalse(TEXT("still marked as edited"), Survivor->bDerived);
				TestEqual(TEXT("and keeps its edited value"), Survivor->MaxWingspan, 6543.0);
			}
		}
	}
```

- [ ] **Step 2: Build — expect FAIL**

Expected: FAIL on "rebuilding does not accumulate edges" — the second `Build` doubles the
count, because nothing clears the previous derivation.

- [ ] **Step 3: Clear derived output before rebuilding**

In `Private/Build/RoadGuidelineBuilder.cpp`, insert at the very top of `Build`, before the
`Ends` map is declared:

```cpp
	// Clear the previous derivation before regenerating, or Build accumulates.
	//
	// bDerived == false edges are the player's, not ours: regenerating one would discard a
	// deliberate edit and replace it with something indistinguishable from correct. They
	// are left in place, and so are the nodes they still reference - RemoveGuidelineNode
	// takes incident edges with it, so a node is only safe to drop once nothing kept
	// points at it.
	{
		TArray<FGuidelineEdgeId> Doomed;
		const TArray<FGuidelineEdge>& Existing = Network.GetGuidelineEdges();
		for (int32 Index = 0; Index < Existing.Num(); ++Index)
		{
			if (Existing[Index].bAlive && Existing[Index].bDerived)
			{
				FGuidelineEdgeId Id;
				Id.Index = Index;
				Id.Generation = Existing[Index].Generation;
				Doomed.Add(Id);
			}
		}
		for (const FGuidelineEdgeId Id : Doomed)
		{
			Network.RemoveGuidelineEdge(Id);
		}

		TArray<FGuidelineNodeId> Orphans;
		const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].Incident.Num() == 0)
			{
				FGuidelineNodeId Id;
				Id.Index = Index;
				Id.Generation = Nodes[Index].Generation;
				Orphans.Add(Id);
			}
		}
		for (const FGuidelineNodeId Id : Orphans)
		{
			Network.RemoveGuidelineNode(Id);
		}
	}
```

- [ ] **Step 4: Build and run — expect PASS**

Expected: 19 tests, 0 failed.

**If "the edited edge survives regeneration" fails**, the clear pass is dropping edges by
index without re-reading generation after earlier removals — collect all the handles
first, as above, then remove.

- [ ] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): guideline derivation is idempotent and spares edits"
```

---

### Task 7: Traversal queries

The read API pathfinding will use. Small, because everything it needs is already on the edge.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadNetwork.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Model/RoadNetwork.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadGuidelineGraphTest.cpp` (extend)

**Interfaces:**
- Consumes: `FGuidelineNode::Incident`, `FGuidelineEdge::{AllowedTraffic, Direction, A, B}`.
- Produces: `TArray<FGuidelineEdgeId> URoadNetwork::GetOutgoingGuidelines(FGuidelineNodeId, ETraversalClass) const`

- [ ] **Step 1: Write the failing test**

Append inside `FRoadGuidelineGraphTest::RunTest`, before its final `return true;`:

```cpp
	// Traversal respects BOTH access and direction. Either one ignored routes an agent
	// somewhere it may not go, and the two failures look identical from the outside.
	{
		const FGuidelineNodeId P = Net->AddGuidelineNode(FVector2D(0.0, 2000.0));
		const FGuidelineNodeId Q = Net->AddGuidelineNode(FVector2D(1000.0, 2000.0));

		FGuidelineEdge OneWay;
		OneWay.A = P;
		OneWay.B = Q;
		OneWay.Direction = EGuidelineDir::AToB;
		OneWay.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		const FGuidelineEdgeId OneWayId = Net->AddGuidelineEdge(MoveTemp(OneWay));

		const TArray<FGuidelineEdgeId> FromP =
			Net->GetOutgoingGuidelines(P, ETraversalClass::GroundVehicle);
		TestTrue(TEXT("a vehicle may leave P along the one-way"), FromP.Contains(OneWayId));

		const TArray<FGuidelineEdgeId> FromQ =
			Net->GetOutgoingGuidelines(Q, ETraversalClass::GroundVehicle);
		TestFalse(TEXT("but may not leave Q against it"), FromQ.Contains(OneWayId));

		const TArray<FGuidelineEdgeId> Walking =
			Net->GetOutgoingGuidelines(P, ETraversalClass::Pedestrian);
		TestFalse(TEXT("and a pedestrian may not use it at all"), Walking.Contains(OneWayId));
	}
```

- [ ] **Step 2: Build — expect FAIL**

Expected: compile error, `URoadNetwork` has no member `GetOutgoingGuidelines`.

- [ ] **Step 3: Declare and implement**

In `Public/Model/RoadNetwork.h`, add beside the other guideline accessors:

```cpp
	/**
	 * Edges an agent of this class may leave Node along, honouring access AND direction.
	 *
	 * Returns edges, not neighbours, because a caller needs the edge's own width, wingspan
	 * limit and geometry to decide whether to take it.
	 */
	TArray<FGuidelineEdgeId> GetOutgoingGuidelines(FGuidelineNodeId Node, ETraversalClass Class) const;
```

In `Private/Model/RoadNetwork.cpp`, append:

```cpp
TArray<FGuidelineEdgeId> URoadNetwork::GetOutgoingGuidelines(
	FGuidelineNodeId Node, ETraversalClass Class) const
{
	TArray<FGuidelineEdgeId> Out;

	const FGuidelineNode* Found = RoadSlot::Get<FGuidelineNodeId>(GuidelineNodes, Node);
	if (Found == nullptr)
	{
		return Out;
	}

	for (const FGuidelineEdgeId Id : Found->Incident)
	{
		const FGuidelineEdge* Edge = RoadSlot::Get<FGuidelineEdgeId>(GuidelineEdges, Id);
		if (Edge == nullptr || !Edge->AllowedTraffic.Allows(Class))
		{
			continue;
		}

		const bool bLeavingA = (Edge->A == Node);
		const bool bPermitted =
			Edge->Direction == EGuidelineDir::Bidirectional ||
			(bLeavingA  && Edge->Direction == EGuidelineDir::AToB) ||
			(!bLeavingA && Edge->Direction == EGuidelineDir::BToA);

		if (bPermitted)
		{
			Out.Add(Id);
		}
	}

	return Out;
}
```

- [ ] **Step 4: Build and run — expect PASS**

Expected: 19 tests, 0 failed.

- [ ] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): guideline traversal queries honouring access and direction"
```

---

## Self-Review

**Spec coverage (Plan A rows only):**

| Spec section | Covered by |
|---|---|
| §2 traversal class vs service role | Task 1 (traversal only; service roles are Plan B) |
| §3 R3 amendment — guideline-level, not lane-level | Task 3 |
| §3 guideline nodes at meaning, not interval | Task 5 (nodes only at cut lines and junctions) |
| §4.2 guideline node and edge | Task 2 |
| §4.2 generation from segments | Task 4 |
| §4.2 generation from junctions (turn paths) | Task 5 |
| §4.2 derived by default, independent in data | Task 6 |
| §4.2 no weld contract, shared by handle | Task 5, asserted in `RoadNet.Build.GuidelineBuilder` |
| §5.1 access mask | Tasks 1, 4, 7 |
| §5.2 direction | Tasks 3, 7 |
| §5.3 width, and no capacity field | Tasks 2, 3 |
| §5.4 priority on the class | Task 1 |
| §5.5 hold-short node flag | Task 2 (field only; nothing sets it until the build tool) |
| §5.6 wingspan limits | Tasks 3, 5 |
| §8 `FProfileLane` absorbed | Task 3 |
| §10 testing invariants | Tasks 1–7 |

**Deferred to Plan B:** `FApronSurface`, `UEntityDefinition`, `FEntityInstance`, anchors, service roles, and §6's marking-derivation falsification test — which needs entities before it can assert the stand rows.

**Not covered by either plan, and deliberately:** §7's validation rules belong to the build tool's `FPlacementValidator` (parent §7.5) and have no home until that exists. §5.7 occupancy is a simulation concern the model only has to not preclude.

**Known gaps to watch during execution:**

1. **`FGuidelineEdge::Control` is a straight midpoint for segment edges.** Curved segments have a Bezier `Control` of their own that the guideline ignores, so a guideline down a curved taxiway will cut the corner. Correct for the straight segments every current test uses, and wrong the moment curves are drawn — Task 4's `Control` should be derived from the segment's own control point when that matters.
2. **Turn paths pair guidelines by index.** Arm A's guideline 0 connects to arm B's guideline 0. That is right for uniform profiles and arbitrary where a two-guideline road meets a one-guideline taxiway; `FMath::Min` stops it crashing but does not make it meaningful. Real lane-to-lane pairing needs the offsets compared, not the indices.
3. **`MaxWingspan` uses a naive `Min`** where `0` means unlimited, so a limited arm meeting an unlimited one yields `0` — unlimited — which is the unsafe direction. Harmless while every profile is `0`; fix before mixed profiles exist.
4. **Nothing sets `HoldShortFor` or `PriorityOverride`.** Both are fields with no writer until the build tool can author them. They are in Task 2 so the graph does not need reshaping later, not because anything uses them yet.
5. **`Build` is O(segments x guidelines) plus O(arms²) per node** and rebuilds everything. At parent R5's few-hundred-segment scale that is fine; it is not an incremental rebuild and would need to become one before it runs on every mouse move.
