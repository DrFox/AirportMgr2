# Road System Slice 2a — Mesh Geometry — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the junction solver's output into real triangle meshes, rendered as solid surfaces, with the shared-vertex contract carried all the way into the mesh so a crack cannot be represented.

**Architecture:** A network→solver adapter walks `URoadNetwork`, solves every node, and writes each segment's trim distances **and its four cut vertices** back into the model. `FRoadMeshBuilder` accumulates vertices through a weld map keyed on the exact `FVector2D` bits, so a junction corner and a segment end holding the same value resolve to the same vertex index. `IRoadMeshSink` receives the finished buffers; `FDynamicMeshSink` pushes them into a batched `UDynamicMeshComponent`.

**Tech Stack:** Unreal Engine 5.8.2, C++20, MSVC 14.51.36231, `GeometryFramework` + `GeometryCore` runtime modules, Unreal Automation Test framework.

**Spec:** `docs/superpowers/specs/2026-08-28-procedural-road-system-design.md` (§4, §5, §6, §9, §12)

## Global Constraints

- **Engine:** Unreal Engine 5.8.2 at `D:\Epic\UE_5.8`. Project at `C:\repos\AirportMgr2`.
- **`BuildSettingsVersion.V7`** — return-type, dangling-reference and unreachable-code warnings are **errors**.
- **`Solve/` must keep ZERO engine dependencies** beyond `CoreMinimal.h`. The adapter touches `URoadNetwork`, so it lives in `Build/`, never in `Solve/`.
- **Automation flags:** `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter`. The nested `EAutomationTestFlags::ApplicationContextMask` form does not compile in 5.8.
- Test files wrapped in `#if WITH_DEV_AUTOMATION_TESTS` … `#endif`.
- **Never use the bare `PI` macro** — deprecated float in 5.8. Use `UE_DOUBLE_PI`.
- **`FVector2D` is double-precision.** Never narrow to `float`. Mesh positions are `FVector3d`.
- Handle liveness is `RoadSlot::IsValid(Items, Handle)`. `FRoadNodeId::IsSet()` reports only that a handle was assigned — it is **not** a liveness check.
- Unreal prefixes `F`/`U`/`A`/`E` are required by UnrealHeaderTool.
- **NO materials, UVs, markings or shoulder fade.** Those are Slice 2b. Surfaces render with the default material.

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

The script exits non-zero if any test failed **or if the filter matched no tests**. Do not judge a run by `UnrealEditor-Cmd.exe`'s own exit code — it is `0` either way.

---

## File Structure

```
Plugins/RoadNet/Source/RoadNet/
  Public/Model/RoadNode.h              MODIFY  - FRoadSegment gains four cut vertices
  Public/Build/RoadNetworkSolver.h     CREATE  - walks URoadNetwork, solves nodes, writes back
  Private/Build/RoadNetworkSolver.cpp  CREATE
  Public/Build/RoadMeshSink.h          CREATE  - IRoadMeshSink + FRoadMeshBuffers
  Public/Build/RoadMeshBuilder.h       CREATE  - weld map, junction fans, segment ribbons
  Private/Build/RoadMeshBuilder.cpp    CREATE
  Public/Present/RoadNetworkActor.h    CREATE  - ARoadNetworkActor + FDynamicMeshSink
  Private/Present/RoadNetworkActor.cpp CREATE
  Public/Debug/RoadJunctionGallery.h   MODIFY  - build real networks, render meshes
  Private/Debug/RoadJunctionGallery.cpp MODIFY
  RoadNet.Build.cs                     MODIFY  - add GeometryFramework, GeometryCore

Plugins/RoadNet/Source/RoadNetTests/Private/
  RoadNetworkSolverTest.cpp            CREATE
  RoadMeshBuilderTest.cpp              CREATE
```

`Build/` depends on `Model`, `Profiles` and `Solve`. `Present/` depends on `Build`. Nothing depends upward.

---

### Task 1: Persist the cut vertices in the model

Closes **K2**, the load-bearing issue carried out of Slice 1. The model currently stores only the scalar trim distance, so a mesh builder reconstructing a segment end as `Position + Tangent*Trim ± PerpCCW(Tangent)*HalfWidth` is bitwise-identical to the solver's cut vertex **only if its `Tangent` is the same `double` the solver received**. One differing low bit reintroduces exactly the cracks this whole design exists to prevent. Storing the vertices makes the contract structural instead of conventional — and a convention is precisely what failed in the original Blueprint system.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Model/RoadNode.h`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadNetworkTest.cpp` (extend)

**Interfaces:**
- Consumes: `FRoadNodeId`, `FRoadSegmentId` from Slice 1.
- Produces: on `FRoadSegment` — `FVector2D LeftCutA, RightCutA, LeftCutB, RightCutB` and `bool bSolved`.

- [ ] **Step 1: Write the failing test**

Append inside `FRoadNetworkTest::RunTest`, immediately before its final `return true;`:

```cpp
	// --- Cut vertices are part of the model, not something callers recompute (K2) ---
	{
		URoadNetwork* CutNet = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* CutProfile = URoadProfile::MakeTransient(2300.0, 1500.0);

		const FRoadNodeId P = CutNet->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Q = CutNet->AddNode(FVector2D(10000.0, 0.0));
		const FRoadSegmentId Seg = CutNet->AddStraightSegment(P, Q, CutProfile);

		const FRoadSegment* Fresh = CutNet->GetSegment(Seg);
		TestFalse(TEXT("a new segment is not yet solved"), Fresh->bSolved);
		TestTrue(TEXT("cut vertices start at zero"),
			Fresh->LeftCutA.IsZero() && Fresh->RightCutA.IsZero() &&
			Fresh->LeftCutB.IsZero() && Fresh->RightCutB.IsZero());

		// Only the solver writes these; the test stands in for it here.
		FRoadSegment* Mutable = CutNet->GetSegmentMutable(Seg);
		Mutable->LeftCutA  = FVector2D(1150.0, 1150.0);
		Mutable->RightCutA = FVector2D(1150.0, -1150.0);
		Mutable->LeftCutB  = FVector2D(8850.0, -1150.0);
		Mutable->RightCutB = FVector2D(8850.0, 1150.0);
		Mutable->bSolved = true;

		const FRoadSegment* Solved = CutNet->GetSegment(Seg);
		TestTrue(TEXT("solved flag survives"), Solved->bSolved);
		// Bitwise, not Equals(). These values are the shared truth.
		TestTrue(TEXT("left cut A stored exactly"),
			Solved->LeftCutA.X == 1150.0 && Solved->LeftCutA.Y == 1150.0);
		TestTrue(TEXT("right cut B stored exactly"),
			Solved->RightCutB.X == 8850.0 && Solved->RightCutB.Y == 1150.0);
	}
```

- [ ] **Step 2: Build and run — expect FAIL**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
```

Expected: a **compile error** — `FRoadSegment` has no member `bSolved`. That is the failure; it proves the test is exercising something that does not yet exist.

- [ ] **Step 3: Add the fields**

In `Public/Model/RoadNode.h`, inside `struct FRoadSegment`, replace the `TrimA`/`TrimB` block with:

```cpp
	/** Written ONLY by FRoadNetworkSolver. Distance from each end at which the segment is cut. */
	UPROPERTY() double TrimA = 0.0;
	UPROPERTY() double TrimB = 0.0;

	/**
	 * The segment's four end vertices, written ONLY by FRoadNetworkSolver.
	 *
	 * These are the SAME values the junction boundary polygon contains, stored rather
	 * than recomputed. A mesh builder that rebuilt them from Position + Tangent*Trim
	 * would be bitwise-identical only if its Tangent were the same double the solver
	 * received; one differing low bit reopens the seam. Never recompute these.
	 */
	UPROPERTY() FVector2D LeftCutA  = FVector2D::ZeroVector;
	UPROPERTY() FVector2D RightCutA = FVector2D::ZeroVector;
	UPROPERTY() FVector2D LeftCutB  = FVector2D::ZeroVector;
	UPROPERTY() FVector2D RightCutB = FVector2D::ZeroVector;

	/** True once a solve has written the trims and cut vertices above. */
	UPROPERTY() bool bSolved = false;
```

- [ ] **Step 4: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

Expected: 8 tests, 0 failed. `RoadNet.Model.Network` passes with the new assertions.

- [ ] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): persist segment cut vertices in the model (closes K2)"
```

---

### Task 2: Network to solver adapter

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Build/RoadNetworkSolver.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Build/RoadNetworkSolver.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadNetworkSolverTest.cpp`

**Interfaces:**
- Consumes: `URoadNetwork` (`GetNodes`, `GetSegment`, `GetSegmentMutable`, `GetOutgoingTangent`, `GetOtherEnd`), `URoadProfile::GetHalfWidthLeft/Right/PreferredFilletRadius`, `FJunctionInput`, `FJunctionArm`, `FJunctionResult`, `FJunctionSolver::SolveCuts/SolveBoundary`.
- Produces:
  - `struct FRoadSolveResult { TMap<int32, FJunctionResult> NodeResults; int32 SolvedNodes = 0; int32 FailedNodes = 0; }` — keyed by `FRoadNodeId::Index`.
  - `FRoadNetworkSolver::SolveAll(URoadNetwork& Network, int32 ArcSegments = 12) -> FRoadSolveResult`

**Why the map is keyed on `int32` and not `FRoadNodeId`:** the mesh builder looks results up while walking segments, and a segment stores node handles whose generation matches the live node, so the index alone is unambiguous within one solve. Keying on the handle would work too but adds nothing.

- [ ] **Step 1: Write the failing test**

`Plugins/RoadNet/Source/RoadNetTests/Private/RoadNetworkSolverTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/JunctionSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNetworkSolverTest,
	"RoadNet.Build.NetworkSolver",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNetworkSolverTest::RunTest(const FString& Parameters)
{
	constexpr double W = 1150.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(W * 2.0, 1500.0);

	// A 90-degree bend: centre node with an east arm and a north arm.
	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(40000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 40000.0));

	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	const FRoadSegmentId ToNorth = Net->AddStraightSegment(Centre, North, Profile);

	const FRoadSolveResult Result = FRoadNetworkSolver::SolveAll(*Net);

	TestTrue(TEXT("every node solved"), Result.FailedNodes == 0);
	TestEqual(TEXT("three nodes solved"), Result.SolvedNodes, 3);
	TestTrue(TEXT("centre node has a result"), Result.NodeResults.Contains(Centre.Index));

	// Both segments are marked solved and carry non-zero trims at the bend end.
	const FRoadSegment* SegEast = Net->GetSegment(ToEast);
	TestTrue(TEXT("east segment solved"), SegEast->bSolved);
	TestTrue(TEXT("east segment trimmed at the bend"), SegEast->TrimA > 0.0);

	// THE CONTRACT, carried into the model: the segment's stored cut vertices are
	// bitwise identical to the ones the junction result holds for that arm.
	const FJunctionResult& CentreResult = Result.NodeResults[Centre.Index];
	bool bFoundLeft = false;
	bool bFoundRight = false;
	for (const FJunctionArmResult& Arm : CentreResult.Arms)
	{
		if (Arm.LeftCut.X == SegEast->LeftCutA.X && Arm.LeftCut.Y == SegEast->LeftCutA.Y)
		{
			bFoundLeft = true;
		}
		if (Arm.RightCut.X == SegEast->RightCutA.X && Arm.RightCut.Y == SegEast->RightCutA.Y)
		{
			bFoundRight = true;
		}
	}
	TestTrue(TEXT("stored left cut matches the junction result exactly"), bFoundLeft);
	TestTrue(TEXT("stored right cut matches the junction result exactly"), bFoundRight);

	// A dead-end node still solves and still writes its end's cut vertices.
	const FRoadSegment* SegNorth = Net->GetSegment(ToNorth);
	TestTrue(TEXT("north segment solved"), SegNorth->bSolved);
	TestFalse(TEXT("dead end wrote a real cut line"),
		SegNorth->LeftCutB.Equals(SegNorth->RightCutB, 1.0));

	// Re-solving is idempotent: same inputs, same bits.
	const FVector2D BeforeLeft = SegEast->LeftCutA;
	FRoadNetworkSolver::SolveAll(*Net);
	const FRoadSegment* Again = Net->GetSegment(ToEast);
	TestTrue(TEXT("re-solve is bitwise idempotent"),
		Again->LeftCutA.X == BeforeLeft.X && Again->LeftCutA.Y == BeforeLeft.Y);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Build — expect FAIL**

Expected: compile error, `Build/RoadNetworkSolver.h` not found.

- [ ] **Step 3: Write the header**

`Public/Build/RoadNetworkSolver.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Solve/JunctionSolver.h"

class URoadNetwork;

/** Every node's solved boundary, keyed by FRoadNodeId::Index. */
struct FRoadSolveResult
{
	TMap<int32, FJunctionResult> NodeResults;
	int32 SolvedNodes = 0;
	int32 FailedNodes = 0;
};

/**
 * Walks a URoadNetwork, solves every live node, and writes each segment's trim
 * distances AND its four cut vertices back into the model.
 *
 * This is the only writer of FRoadSegment::TrimA/TrimB and the cut vertices. It lives
 * in Build/ rather than Solve/ because it touches UObjects, and Solve/ must stay free
 * of engine dependencies so its tests can run without a World.
 */
class ROADNET_API FRoadNetworkSolver
{
public:
	static FRoadSolveResult SolveAll(URoadNetwork& Network, int32 ArcSegments = 12);
};
```

- [ ] **Step 4: Write the implementation**

`Private/Build/RoadNetworkSolver.cpp`:

```cpp
#include "Build/RoadNetworkSolver.h"

#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

FRoadSolveResult FRoadNetworkSolver::SolveAll(URoadNetwork& Network, int32 ArcSegments)
{
	FRoadSolveResult Out;

	const TArray<FRoadNode>& Nodes = Network.GetNodes();
	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		const FRoadNode& Node = Nodes[NodeIndex];
		if (!Node.bAlive || Node.Incident.Num() == 0)
		{
			continue;
		}

		FRoadNodeId NodeId;
		NodeId.Index = NodeIndex;
		NodeId.Generation = Node.Generation;

		// Incident is maintained sorted by CCW bearing, which is exactly what
		// FJunctionSolver requires. Do not re-sort here.
		FJunctionInput Input;
		Input.Position = Node.Position;
		Input.ArcSegments = ArcSegments;

		for (const FRoadSegmentId SegmentId : Node.Incident)
		{
			const FRoadSegment* Segment = Network.GetSegment(SegmentId);
			if (Segment == nullptr)
			{
				continue;
			}

			const URoadProfile* Profile = Segment->Profile;

			FJunctionArm Arm;
			Arm.Tangent = Network.GetOutgoingTangent(SegmentId, NodeId);
			Arm.HalfWidthLeft  = Profile ? Profile->GetHalfWidthLeft()  : 0.0;
			Arm.HalfWidthRight = Profile ? Profile->GetHalfWidthRight() : 0.0;
			Arm.FilletRadius   = Profile ? Profile->PreferredFilletRadius : 0.0;
			Arm.UserData = SegmentId.Index;
			Input.Arms.Add(Arm);
		}

		if (Input.Arms.Num() == 0)
		{
			continue;
		}

		FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		FJunctionSolver::SolveBoundary(Input, Result);

		if (!Result.bValid)
		{
			++Out.FailedNodes;
			continue;
		}

		// Write the solve back into the model. Arm order matches Input.Arms, which
		// matches Node.Incident, so UserData tells us which segment each arm is.
		for (int32 ArmIndex = 0; ArmIndex < Result.Arms.Num(); ++ArmIndex)
		{
			const FRoadSegmentId SegmentId = Node.Incident[ArmIndex];
			FRoadSegment* Segment = Network.GetSegmentMutable(SegmentId);
			if (Segment == nullptr)
			{
				continue;
			}

			const FJunctionArmResult& ArmResult = Result.Arms[ArmIndex];
			const bool bIsEndA = (Segment->A == NodeId);

			if (bIsEndA)
			{
				Segment->TrimA = ArmResult.CutDistance;
				Segment->LeftCutA = ArmResult.LeftCut;
				Segment->RightCutA = ArmResult.RightCut;
			}
			else
			{
				Segment->TrimB = ArmResult.CutDistance;
				Segment->LeftCutB = ArmResult.LeftCut;
				Segment->RightCutB = ArmResult.RightCut;
			}
			Segment->bSolved = true;
		}

		Out.NodeResults.Add(NodeIndex, MoveTemp(Result));
		++Out.SolvedNodes;
	}

	return Out;
}
```

- [ ] **Step 5: Add `Build/` to the module and build**

No `Build.cs` change is needed — `Public/` and `Private/` subfolders are found automatically. Run:

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

Expected: 9 tests, 0 failed, including `RoadNet.Build.NetworkSolver`.

- [ ] **Step 6: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): network to solver adapter writing trims and cut vertices"
```

---

### Task 3: Mesh builder with exact vertex welding

The heart of the slice. **The weld map is keyed on the exact `FVector2D` value**, so a junction boundary vertex and a segment end vertex holding the same bits resolve to the same vertex index. At that point a crack is not merely absent — it is unrepresentable, because there is only one vertex.

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Build/RoadMeshSink.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Build/RoadMeshBuilder.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Build/RoadMeshBuilder.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadMeshBuilderTest.cpp`

**Interfaces:**
- Consumes: `FRoadSolveResult`, `FJunctionResult`, `URoadNetwork`, `FRoadSegment`.
- Produces:
  - `struct FRoadMeshBuffers { TArray<FVector3d> Positions; TArray<int32> Indices; }`
  - `struct IRoadMeshSink { virtual void Accept(const FRoadMeshBuffers&) = 0; }`
  - `FRoadMeshBuilder::FRoadMeshBuilder(double InZHeight)`
  - `FRoadMeshBuilder::AddJunction(const FJunctionResult&)`
  - `FRoadMeshBuilder::AddSegment(const URoadNetwork&, FRoadSegmentId, int32 RibbonSegments)`
  - `FRoadMeshBuilder::Emit(IRoadMeshSink&) const`
  - `FRoadMeshBuilder::GetBuffers() const -> const FRoadMeshBuffers&`
  - `FRoadMeshBuilder::VertexCount() const -> int32`

- [ ] **Step 1: Write the failing test**

`Plugins/RoadNet/Source/RoadNetTests/Private/RoadMeshBuilderTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Signed area of a triangle projected on XY. Positive means CCW seen from +Z. */
	double TriangleArea2D(const FVector3d& A, const FVector3d& B, const FVector3d& C)
	{
		return 0.5 * ((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadMeshBuilderTest,
	"RoadNet.Build.MeshBuilder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadMeshBuilderTest::RunTest(const FString& Parameters)
{
	constexpr double W = 1150.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(W * 2.0, 1500.0);

	// A 90-degree bend, the case that failed in the original Blueprint system.
	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(40000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 40000.0));
	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	const FRoadSegmentId ToNorth = Net->AddStraightSegment(Centre, North, Profile);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestTrue(TEXT("network solved"), Solved.FailedNodes == 0);

	FRoadMeshBuilder Builder(10.0);
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		Builder.AddJunction(Pair.Value);
	}
	Builder.AddSegment(*Net, ToEast, 1);
	Builder.AddSegment(*Net, ToNorth, 1);

	const FRoadMeshBuffers& Buffers = Builder.GetBuffers();

	TestTrue(TEXT("mesh has vertices"), Buffers.Positions.Num() > 0);
	TestTrue(TEXT("mesh has triangles"), Buffers.Indices.Num() > 0);
	TestEqual(TEXT("indices come in threes"), Buffers.Indices.Num() % 3, 0);

	for (const int32 Index : Buffers.Indices)
	{
		TestTrue(TEXT("index in range"), Index >= 0 && Index < Buffers.Positions.Num());
	}

	// Flat world: every vertex sits on the same plane.
	for (const FVector3d& P : Buffers.Positions)
	{
		TestTrue(TEXT("vertex is finite"),
			FMath::IsFinite(P.X) && FMath::IsFinite(P.Y) && FMath::IsFinite(P.Z));
		TestTrue(TEXT("vertex is on the road plane"), FMath::IsNearlyEqual(P.Z, 10.0, 1e-9));
	}

	// Every triangle faces up. A wound-backwards triangle renders black or invisible.
	for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
	{
		const double Area = TriangleArea2D(
			Buffers.Positions[Buffers.Indices[Slot]],
			Buffers.Positions[Buffers.Indices[Slot + 1]],
			Buffers.Positions[Buffers.Indices[Slot + 2]]);
		TestTrue(TEXT("triangle winds counter-clockwise"), Area > 0.0);
	}

	// THE POINT OF THE SLICE: the segment's end vertices and the junction's boundary
	// vertices are not merely coincident, they are the SAME vertex. Welding happens on
	// exact bits, so a crack cannot be represented in this buffer at all.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);

		int32 LeftMatches = 0;
		int32 RightMatches = 0;
		for (const FVector3d& P : Buffers.Positions)
		{
			if (P.X == Seg->LeftCutA.X && P.Y == Seg->LeftCutA.Y)  { ++LeftMatches; }
			if (P.X == Seg->RightCutA.X && P.Y == Seg->RightCutA.Y) { ++RightMatches; }
		}
		TestEqual(TEXT("left cut appears exactly once - welded, not duplicated"), LeftMatches, 1);
		TestEqual(TEXT("right cut appears exactly once - welded, not duplicated"), RightMatches, 1);
	}

	// No duplicate positions anywhere: welding is global, not per-primitive.
	{
		TSet<FVector2D> Seen;
		int32 Duplicates = 0;
		for (const FVector3d& P : Buffers.Positions)
		{
			const FVector2D Key(P.X, P.Y);
			if (Seen.Contains(Key)) { ++Duplicates; }
			Seen.Add(Key);
		}
		TestEqual(TEXT("no duplicated vertex positions"), Duplicates, 0);
	}

	// A sink receives what the builder holds.
	{
		struct FCountingSink : public IRoadMeshSink
		{
			int32 Vertices = 0;
			int32 Tris = 0;
			virtual void Accept(const FRoadMeshBuffers& In) override
			{
				Vertices = In.Positions.Num();
				Tris = In.Indices.Num() / 3;
			}
		};
		FCountingSink Sink;
		Builder.Emit(Sink);
		TestEqual(TEXT("sink got every vertex"), Sink.Vertices, Buffers.Positions.Num());
		TestEqual(TEXT("sink got every triangle"), Sink.Tris, Buffers.Indices.Num() / 3);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Build — expect FAIL**

Expected: compile error, `Build/RoadMeshBuilder.h` not found.

- [ ] **Step 3: Write the sink header**

`Public/Build/RoadMeshSink.h`:

```cpp
#pragma once

#include "CoreMinimal.h"

/** Flat triangle soup in world space. Slice 2b adds UV channels here. */
struct FRoadMeshBuffers
{
	TArray<FVector3d> Positions;
	TArray<int32>     Indices;

	void Reset()
	{
		Positions.Reset();
		Indices.Reset();
	}
};

/**
 * Where finished geometry goes. Strategy: the builder does not know whether its output
 * becomes a UDynamicMeshComponent, a preview ghost, or a test counter.
 */
struct IRoadMeshSink
{
	virtual ~IRoadMeshSink() = default;
	virtual void Accept(const FRoadMeshBuffers& Buffers) = 0;
};
```

- [ ] **Step 4: Write the builder header**

`Public/Build/RoadMeshBuilder.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"
#include "Model/RoadHandles.h"
#include "Solve/JunctionSolver.h"

class URoadNetwork;

/**
 * Accumulates junction fans and segment ribbons into one welded triangle soup.
 *
 * Vertices are welded through a map keyed on the EXACT FVector2D value. A junction
 * boundary vertex and a segment end vertex that hold the same bits therefore resolve
 * to the same vertex index, which is what makes a seam unrepresentable rather than
 * merely small. The solver guarantees those bits match by storing the cut vertices
 * rather than letting anyone recompute them.
 */
class ROADNET_API FRoadMeshBuilder
{
public:
	explicit FRoadMeshBuilder(double InZHeight);

	/** Append a solved junction's triangle fan. */
	void AddJunction(const FJunctionResult& Junction);

	/**
	 * Append a segment's ribbon between its two stored cut lines.
	 * RibbonSegments is the number of quads along the segment; 1 is correct for a
	 * straight segment, more for a curve.
	 */
	void AddSegment(const URoadNetwork& Network, FRoadSegmentId SegmentId, int32 RibbonSegments = 8);

	void Emit(IRoadMeshSink& Sink) const;

	const FRoadMeshBuffers& GetBuffers() const { return Buffers; }
	int32 VertexCount() const { return Buffers.Positions.Num(); }

private:
	/** Returns the index of Point, appending it only if this exact value is new. */
	int32 WeldVertex(const FVector2D& Point);

	void AddTriangle(int32 A, int32 B, int32 C);

	double ZHeight;
	FRoadMeshBuffers Buffers;
	TMap<FVector2D, int32> WeldMap;
};
```

- [ ] **Step 5: Write the builder implementation**

`Private/Build/RoadMeshBuilder.cpp`:

```cpp
#include "Build/RoadMeshBuilder.h"

#include "Model/RoadNetwork.h"
#include "Solve/RoadGeom.h"

FRoadMeshBuilder::FRoadMeshBuilder(double InZHeight)
	: ZHeight(InZHeight)
{
}

int32 FRoadMeshBuilder::WeldVertex(const FVector2D& Point)
{
	// FVector2D::operator== is an exact comparison and its GetTypeHash covers both
	// components, so this map welds on bits, not on proximity. That is deliberate:
	// a tolerance here would silently paper over a solver that had stopped sharing
	// its vertices, which is the exact failure this design exists to prevent.
	if (const int32* Existing = WeldMap.Find(Point))
	{
		return *Existing;
	}

	const int32 NewIndex = Buffers.Positions.Add(FVector3d(Point.X, Point.Y, ZHeight));
	WeldMap.Add(Point, NewIndex);
	return NewIndex;
}

void FRoadMeshBuilder::AddTriangle(int32 A, int32 B, int32 C)
{
	// A degenerate triangle contributes nothing and upsets downstream normal
	// computation, so drop it rather than emit it.
	if (A == B || B == C || A == C)
	{
		return;
	}
	Buffers.Indices.Add(A);
	Buffers.Indices.Add(B);
	Buffers.Indices.Add(C);
}

void FRoadMeshBuilder::AddJunction(const FJunctionResult& Junction)
{
	if (!Junction.bValid || Junction.Triangles.Num() == 0)
	{
		return;
	}

	// Boundary holds the rim followed by the fan apex; Triangles indexes into it.
	// Map every boundary slot through the weld map once, then re-index.
	TArray<int32> Mapped;
	Mapped.Reserve(Junction.Boundary.Num());
	for (const FVector2D& Point : Junction.Boundary)
	{
		Mapped.Add(WeldVertex(Point));
	}

	for (int32 Slot = 0; Slot + 2 < Junction.Triangles.Num(); Slot += 3)
	{
		AddTriangle(
			Mapped[Junction.Triangles[Slot]],
			Mapped[Junction.Triangles[Slot + 1]],
			Mapped[Junction.Triangles[Slot + 2]]);
	}
}

void FRoadMeshBuilder::AddSegment(const URoadNetwork& Network, FRoadSegmentId SegmentId, int32 RibbonSegments)
{
	const FRoadSegment* Segment = Network.GetSegment(SegmentId);
	if (Segment == nullptr || !Segment->bSolved)
	{
		return;
	}

	const int32 Steps = FMath::Max(RibbonSegments, 1);

	// The two ends come from the model verbatim. Never recompute them: these are the
	// same values the junction boundary holds, and only bitwise equality welds.
	const FVector2D LeftStart  = Segment->LeftCutA;
	const FVector2D RightStart = Segment->RightCutA;

	// End B's cut line is authored from B's point of view, so its left is this
	// segment's right when walking A to B. Swap so the ribbon does not cross itself.
	const FVector2D LeftEnd  = Segment->RightCutB;
	const FVector2D RightEnd = Segment->LeftCutB;

	TArray<int32> LeftRail;
	TArray<int32> RightRail;
	LeftRail.Reserve(Steps + 1);
	RightRail.Reserve(Steps + 1);

	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		if (Step == 0)
		{
			LeftRail.Add(WeldVertex(LeftStart));
			RightRail.Add(WeldVertex(RightStart));
		}
		else if (Step == Steps)
		{
			LeftRail.Add(WeldVertex(LeftEnd));
			RightRail.Add(WeldVertex(RightEnd));
		}
		else
		{
			// Interior samples are ours alone and may be interpolated freely; only the
			// ends are shared with a junction.
			const double Alpha = static_cast<double>(Step) / static_cast<double>(Steps);
			LeftRail.Add(WeldVertex(FMath::Lerp(LeftStart, LeftEnd, Alpha)));
			RightRail.Add(WeldVertex(FMath::Lerp(RightStart, RightEnd, Alpha)));
		}
	}

	for (int32 Step = 0; Step < Steps; ++Step)
	{
		const int32 R0 = RightRail[Step];
		const int32 R1 = RightRail[Step + 1];
		const int32 L0 = LeftRail[Step];
		const int32 L1 = LeftRail[Step + 1];

		// Wound counter-clockwise seen from +Z so the surface faces up.
		AddTriangle(R0, R1, L1);
		AddTriangle(R0, L1, L0);
	}
}

void FRoadMeshBuilder::Emit(IRoadMeshSink& Sink) const
{
	Sink.Accept(Buffers);
}
```

- [ ] **Step 6: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

Expected: 10 tests, 0 failed, including `RoadNet.Build.MeshBuilder`.

**If "triangle winds counter-clockwise" fails on the ribbon**, the left/right swap at end B is the suspect — report the failing triangle's three positions rather than flipping the winding blindly, because the same swap decides whether the ribbon is twisted.

- [ ] **Step 7: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): mesh builder welding junction and segment vertices on exact bits"
```

---

### Task 4: Dynamic mesh renderer

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/RoadNet.Build.cs`
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Present/RoadNetworkActor.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Present/RoadNetworkActor.cpp`

**Interfaces:**
- Consumes: `FRoadMeshBuffers`, `IRoadMeshSink`, `FRoadMeshBuilder`.
- Produces:
  - `class FDynamicMeshSink : public IRoadMeshSink` — constructed with `UDynamicMeshComponent*`
  - `ARoadNetworkActor` with `URoadNetwork* Network`, `void RebuildMesh()`, `UDynamicMeshComponent* MeshComponent`

`GeometryFramework` is a **Runtime module**, not a plugin, so this is a `Build.cs` dependency with nothing to enable in the `.uplugin`.

- [ ] **Step 1: Add the module dependencies**

In `RoadNet.Build.cs`, replace the `PublicDependencyModuleNames` block with:

```csharp
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GeometryCore",       // FDynamicMesh3
			"GeometryFramework"   // UDynamicMeshComponent
		});
```

- [ ] **Step 2: Write the actor header**

`Public/Present/RoadNetworkActor.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Build/RoadMeshSink.h"
#include "RoadNetworkActor.generated.h"

class URoadNetwork;
class UDynamicMeshComponent;

/** Pushes finished buffers into a UDynamicMeshComponent. */
class ROADNET_API FDynamicMeshSink : public IRoadMeshSink
{
public:
	explicit FDynamicMeshSink(UDynamicMeshComponent* InComponent) : Component(InComponent) {}
	virtual void Accept(const FRoadMeshBuffers& Buffers) override;

private:
	UDynamicMeshComponent* Component = nullptr;
};

/** Owns a road network and renders it as one batched dynamic mesh. */
UCLASS()
class ROADNET_API ARoadNetworkActor : public AActor
{
	GENERATED_BODY()

public:
	ARoadNetworkActor();

	/** Solve every node, build the mesh, and push it to the component. */
	UFUNCTION(CallInEditor, Category = "RoadNet")
	void RebuildMesh();

	UPROPERTY(VisibleAnywhere, Category = "RoadNet")
	TObjectPtr<UDynamicMeshComponent> MeshComponent;

	UPROPERTY() TObjectPtr<URoadNetwork> Network;

	/** Height of the road surface above the actor, in uu. */
	UPROPERTY(EditAnywhere, Category = "RoadNet") double SurfaceZ = 10.0;

	/** Quads along each segment. 1 is right for straight segments. */
	UPROPERTY(EditAnywhere, Category = "RoadNet") int32 RibbonSegments = 1;
};
```

- [ ] **Step 3: Write the actor implementation**

`Private/Present/RoadNetworkActor.cpp`:

```cpp
#include "Present/RoadNetworkActor.h"

#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/MeshNormals.h"
#include "Model/RoadNetwork.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoadMesh, Log, All);

void FDynamicMeshSink::Accept(const FRoadMeshBuffers& Buffers)
{
	if (Component == nullptr)
	{
		return;
	}

	UE::Geometry::FDynamicMesh3 Mesh;
	Mesh.Clear();

	for (const FVector3d& Position : Buffers.Positions)
	{
		Mesh.AppendVertex(Position);
	}

	int32 Rejected = 0;
	for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
	{
		const int32 Result = Mesh.AppendTriangle(
			Buffers.Indices[Slot], Buffers.Indices[Slot + 1], Buffers.Indices[Slot + 2]);

		// AppendTriangle REFUSES rather than throws: negative results mean the triangle
		// was non-manifold or a duplicate. Silently ignoring that would leave holes in
		// the surface that look exactly like the cracks this system exists to remove.
		if (Result < 0)
		{
			++Rejected;
		}
	}

	if (Rejected > 0)
	{
		UE_LOG(LogRoadMesh, Warning,
			TEXT("%d triangle(s) rejected as non-manifold or duplicate - the surface will have holes"),
			Rejected);
	}

	UE::Geometry::FMeshNormals::QuickComputeVertexNormals(Mesh);

	Component->SetMesh(MoveTemp(Mesh));
	Component->NotifyMeshUpdated();
}

ARoadNetworkActor::ARoadNetworkActor()
{
	PrimaryActorTick.bCanEverTick = false;

	MeshComponent = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("RoadMesh"));
	RootComponent = MeshComponent;
}

void ARoadNetworkActor::RebuildMesh()
{
	if (Network == nullptr || MeshComponent == nullptr)
	{
		return;
	}

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Network);

	FRoadMeshBuilder Builder(SurfaceZ);
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		Builder.AddJunction(Pair.Value);
	}

	const TArray<FRoadSegment>& Segments = Network->GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		if (!Segments[Index].bAlive)
		{
			continue;
		}
		FRoadSegmentId SegmentId;
		SegmentId.Index = Index;
		SegmentId.Generation = Segments[Index].Generation;
		Builder.AddSegment(*Network, SegmentId, RibbonSegments);
	}

	FDynamicMeshSink Sink(MeshComponent);
	Builder.Emit(Sink);

	UE_LOG(LogRoadMesh, Log, TEXT("Rebuilt: %d nodes (%d failed), %d vertices, %d triangles"),
		Solved.SolvedNodes, Solved.FailedNodes,
		Builder.GetBuffers().Positions.Num(), Builder.GetBuffers().Indices.Num() / 3);
}
```

- [ ] **Step 4: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

Expected: 10 tests, 0 failed. No new tests here — this task is engine plumbing whose behaviour is verified visually in Task 5. If the build fails on `UDynamicMeshComponent` not found, confirm `GeometryFramework` reached `PublicDependencyModuleNames`.

- [ ] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): batched dynamic mesh renderer for the road network"
```

---

### Task 5: Gallery renders solid surfaces

The gallery currently builds `FJunctionInput` directly, bypassing `URoadNetwork` entirely — so it exercises the solver but not the model or the mesh path. Rewiring it through real networks makes it an end-to-end check of model → solver → mesh, which is what the exit criterion actually needs.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Debug/RoadJunctionGallery.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Debug/RoadJunctionGallery.cpp`

**Interfaces:**
- Consumes: `URoadNetwork`, `FRoadNetworkSolver::SolveAll`, `FRoadMeshBuilder`, `FDynamicMeshSink`, `UDynamicMeshComponent`.
- Produces: no new public API. `ARoadJunctionGallery` gains `UDynamicMeshComponent* MeshComponent`, `bool bDrawDebugLines`, and `void RebuildGalleryMesh()`.

- [ ] **Step 1: Add the mesh component and controls to the header**

In `Public/Debug/RoadJunctionGallery.h`, add these includes at the top after the existing ones:

```cpp
class UDynamicMeshComponent;
```

and add these members to the `public:` section, after `DebugLineThickness`:

```cpp
	/** Draw the solver's debug lines on top of the solid surface. */
	UPROPERTY(EditAnywhere, Category = "RoadNet") bool bDrawDebugLines = true;

	/** Solve every cell, build one batched mesh, and push it to the component. */
	UFUNCTION(CallInEditor, Category = "RoadNet")
	void RebuildGalleryMesh();

	UPROPERTY(VisibleAnywhere, Category = "RoadNet")
	TObjectPtr<UDynamicMeshComponent> MeshComponent;
```

- [ ] **Step 2: Create the component and build real networks**

In `Private/Debug/RoadJunctionGallery.cpp`, add these includes after the existing ones:

```cpp
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Components/DynamicMeshComponent.h"
#include "Present/RoadNetworkActor.h"
```

Replace the constructor body with:

```cpp
ARoadJunctionGallery::ARoadJunctionGallery()
{
	PrimaryActorTick.bCanEverTick = true;

	MeshComponent = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("GalleryMesh"));
	RootComponent = MeshComponent;
}
```

Then replace the whole of `BuildGallery()` with a version that builds real graph nodes and segments per cell:

```cpp
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

	// Each cell is a real sub-network: a centre node plus one outer node per arm.
	// The arm must reach past its own cut or the ribbon inverts, so ArmLength is a
	// floor, not the actual length.
	for (int32 CellIndex = 0; CellIndex < CellCentres.Num(); ++CellIndex)
	{
		const FVector2D Centre = CellCentres[CellIndex];
		const FRoadNodeId CentreNode = Network->AddNode(Centre);

		for (const double Bearing : CellBearings[CellIndex])
		{
			const FVector2D Dir(FMath::Cos(Bearing), FMath::Sin(Bearing));
			const FRoadNodeId Outer = Network->AddNode(Centre + Dir * ArmLength);
			Network->AddStraightSegment(CentreNode, Outer, Profile);
		}
	}

	UE_LOG(LogRoadGallery, Log, TEXT("Gallery built: %d cells, %d nodes, %d segments"),
		CellBearings.Num(), Network->GetNodes().Num(), Network->GetSegments().Num());

	RebuildGalleryMesh();
}
```

- [ ] **Step 3: Implement the mesh rebuild**

Add this after `BuildGallery()`:

```cpp
void ARoadJunctionGallery::RebuildGalleryMesh()
{
	if (Network == nullptr || MeshComponent == nullptr)
	{
		return;
	}

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Network);

	FRoadMeshBuilder Builder(10.0);
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		Builder.AddJunction(Pair.Value);
	}

	const TArray<FRoadSegment>& Segments = Network->GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		if (!Segments[Index].bAlive)
		{
			continue;
		}
		FRoadSegmentId SegmentId;
		SegmentId.Index = Index;
		SegmentId.Generation = Segments[Index].Generation;
		Builder.AddSegment(*Network, SegmentId, 1);
	}

	FDynamicMeshSink Sink(MeshComponent);
	Builder.Emit(Sink);

	UE_LOG(LogRoadGallery, Log, TEXT("Gallery mesh: %d nodes (%d failed), %d vertices, %d triangles"),
		Solved.SolvedNodes, Solved.FailedNodes,
		Builder.GetBuffers().Positions.Num(), Builder.GetBuffers().Indices.Num() / 3);
}
```

- [ ] **Step 4: Gate the debug lines behind the new flag**

In `Tick`, change the early-out so the debug overlay can be switched off independently of the mesh:

```cpp
	if (!bDrawDebugLines || RoadDebug::GetDebugDrawLevel() <= 0)
	{
		return;
	}
```

- [ ] **Step 5: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

Expected: 10 tests, 0 failed.

- [ ] **Step 6: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): gallery builds real networks and renders solid meshes"
```

- [ ] **Step 7: Visual verification — the Slice 2a exit criterion**

This step is for a human. Do not attempt to drive the editor.

1. Open the project and load `Content/Maps/RoadGallery`.
2. Select the gallery actor. In Details, set **Draw Debug Lines** to `false` so only the solid surface shows.
3. Press **Alt+S** (Simulate), not Play — Simulate ticks the world while leaving the editor camera under your control.
4. Look down at each of the eight cells and confirm:
   - each junction is a **continuous solid surface**, with no gap, notch or sliver where a segment ribbon meets a junction;
   - corners are **rounded**, not mitred;
   - no triangle is missing or inside-out (a backwards triangle reads as a black or invisible patch).
5. Re-enable **Draw Debug Lines** and confirm the green boundary sits exactly on the edge of the solid surface.
6. Check the Output Log for `LogRoadMesh: Warning` — any rejected triangles mean holes.

---

## Self-Review

**Spec coverage (Slice 2a rows only):**

| Spec section | Covered by |
|---|---|
| §4.2 `FRoadSegment` trims written only by the solver | Task 1, Task 2 |
| §6.1 `FRoadMeshBuilder` → `IRoadMeshSink` strategy | Task 3 |
| §6.2 `UDynamicMeshComponent`, batched, no collision | Task 4 |
| §9 slice 2a exit criterion | Task 5 Step 7 |
| §12 K2 — persist cut vertices | Task 1, enforced by Task 3's weld test |

Deliberately **not** covered, deferred to Slice 2b: §6.3 dual UV channels, §6.4 markings, §6.5 shoulder fade, §6.6 ghost material. §5.8 turn paths remain Slice 4.

**Known gaps to watch during execution:**

1. `AddSegment` lerps its interior samples in a straight line, so a curved segment renders as a chord rather than following its Bezier. Correct for Slice 2a, where every segment is straight and `RibbonSegments` is 1. Slice 2b must sample the curve properly when `Control` is not the midpoint.
2. The end-B left/right swap in `AddSegment` is asserted only indirectly, through the CCW-winding check. A twisted ribbon that happened to keep positive area would slip through; the visual check is the real gate.
3. `FDynamicMeshSink` logs rejected triangles but does not fail. That is intentional — a partial surface plus a warning is more diagnosable than an empty one — but it means a green test run does not by itself prove a hole-free mesh. Step 7 point 6 exists for that.
4. Welding is global across the whole network, so two unrelated roads that happen to share an exact vertex position would be joined. At airport scale with double precision this cannot occur by accident, and it is the same property that makes the junction weld work.
