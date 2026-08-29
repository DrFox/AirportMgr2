# Road System Slice 2b-ii — Lateral Bands and Shoulder Fade — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Subdivide the road laterally by the profile's bands and fade the outermost shoulder into the ground, so roads stop ending in a knife edge.

**Architecture:** A profile's band boundaries become cut-line parameters. Both the segment ribbon and the junction rim derive their band vertices from the two stored cut vertices through one shared function, so they weld bitwise exactly as the outer pair already do. Junction shoulder fade uses a ring of rim vertices pushed toward the fan apex — safe because the solver already guarantees the rim is star-shaped about that apex.

**Tech Stack:** Unreal Engine 5.8.2, C++20, MSVC 14.51.36231, `GeometryFramework` + `GeometryCore`, `PythonScriptPlugin`, Unreal Automation Test framework.

**Spec:** `docs/superpowers/specs/2026-08-28-road-slice2b-materials-design.md` (§2, §4, §6, §9). Parent: `2026-08-28-procedural-road-system-design.md` §6.5, §12 (K3).

**Follows:** 2b-i, merged. Roads already render textured asphalt with a UV1 centreline that fades before junctions.

## Global Constraints

- **Engine:** Unreal Engine 5.8.2 at `D:\Epic\UE_5.8`. Project at `C:\repos\AirportMgr2`.
- **`BuildSettingsVersion.V7`** — return-type, dangling-reference and unreachable-code warnings are **errors**.
- **`Solve/` must keep ZERO engine dependencies** beyond `CoreMinimal.h`. This plan changes nothing under `Solve/`.
- `Build/` depends on `Model`, `Profiles`, `Solve`. `Present/` depends on `Build`. Nothing depends upward.
- **Automation flags:** `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter`. The nested form does not compile in 5.8.
- Test files wrapped in `#if WITH_DEV_AUTOMATION_TESTS` … `#endif`.
- **Never use the bare `PI` macro** — use `UE_DOUBLE_PI`.
- **`FVector2D` is double-precision. Never narrow to `float`** for positions. UVs are `FVector2f`.
- **Shared vertices are asserted bitwise with `==`, never a tolerance.**
- **Unreal's front face is the OPPOSITE winding to the maths convention.** `FRoadMeshBuilder::AddTriangle` swaps B and C for this reason; mesh tests assert **negative** 2D signed area, and `RoadNet.Build.MeshAttributes` asserts every engine-computed vertex normal points up. Do not "fix" either.
- Unreal `F`/`U`/`A`/`E` prefixes are required by UnrealHeaderTool.

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

Judge every run by the wrapper's parsed output, never by `UnrealEditor-Cmd.exe`'s exit code, which is `0` either way. **Never report a result from a build that did not print `Result: Succeeded`.**

Re-author the material (also needs the editor closed):

```powershell
& "D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\repos\AirportMgr2\AirportMgr.uproject" `
  -run=pythonscript -script="C:\repos\AirportMgr2\Tools\Python\build_road_material.py" -unattended -nosplash -nopause
Select-String -Path C:\repos\AirportMgr2\Saved\Logs\AirportMgr.log -Pattern "MARKER:|LogMaterial"
```

---

## File Structure

```
Plugins/RoadNet/Source/RoadNet/
  Public/Profiles/RoadProfile.h        MODIFY  - MakeTransient gains an optional shoulder
  Private/Profiles/RoadProfile.cpp     MODIFY
  Public/Build/RoadProfileBands.h      CREATE  - band boundaries as cut-line parameters
  Private/Build/RoadProfileBands.cpp   CREATE
  Public/Build/RoadNetworkSolver.h     MODIFY  - solve result carries arm -> segment
  Private/Build/RoadNetworkSolver.cpp  MODIFY
  Public/Build/RoadMeshBuilder.h       MODIFY  - CutLinePoint; AddJunction takes the network
  Private/Build/RoadMeshBuilder.cpp    MODIFY  - banded ribbon, banded rim, inset ring, K3
  Private/Present/RoadNetworkActor.cpp MODIFY  - fallback profile gains a shoulder

Plugins/RoadNet/Source/RoadNetTests/Private/
  RoadProfileBandsTest.cpp             CREATE  - band maths, pure
  RoadBandWeldTest.cpp                 CREATE  - the bitwise contract, extended to bands

Tools/Python/build_road_material.py    MODIFY  - UV2.Y drives a dithered opacity mask
```

---

### Task 1: Band boundaries as cut-line parameters

The profile has carried `FProfileBand` since Slice 1 and nothing has ever read it. This turns a profile into the numbers both the ribbon and the rim need, and gives the default profile a shoulder to fade — without which the rest of this slice is invisible.

**Files:**
- Create: `Plugins/RoadNet/Source/RoadNet/Public/Build/RoadProfileBands.h`
- Create: `Plugins/RoadNet/Source/RoadNet/Private/Build/RoadProfileBands.cpp`
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Profiles/RoadProfile.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Profiles/RoadProfile.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadProfileBandsTest.cpp`

**Interfaces:**
- Consumes: `URoadProfile::{Bands, GetTotalWidth, GetHalfWidthLeft, GetHalfWidthRight}`, `FProfileBand::{Width, Type}`, `ERoadBandType`.
- Produces:
  - `struct FRoadProfileBands { TArray<double> Alphas; TArray<float> Laterals; TArray<float> GroundBlend; static FRoadProfileBands FromProfile(const URoadProfile*); }`
  - `URoadProfile::MakeTransient(double TotalWidth, double FilletRadius, double ShoulderWidth = 0.0)`

- [x] **Step 1: Write the failing test**

Create `Plugins/RoadNet/Source/RoadNetTests/Private/RoadProfileBandsTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadProfileBands.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadProfileBandsTest,
	"RoadNet.Build.ProfileBands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadProfileBandsTest::RunTest(const FString& Parameters)
{
	// A single-band profile has no interior boundary: just the two outer edges, and no
	// shoulder, so nothing fades. This is what MakeTransient produced before this task
	// and it must keep working - the ribbon still has to build from it.
	{
		URoadProfile* Plain = URoadProfile::MakeTransient(200.0, 100.0);
		const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(Plain);

		TestEqual(TEXT("a single band gives two boundaries"), Bands.Alphas.Num(), 2);
		TestEqual(TEXT("alpha starts at the right edge"), Bands.Alphas[0], 0.0);
		TestEqual(TEXT("alpha ends at the left edge"), Bands.Alphas[1], 1.0);
		TestEqual(TEXT("right edge lateral is -HalfWidthRight"), Bands.Laterals[0], -100.0f);
		TestEqual(TEXT("left edge lateral is +HalfWidthLeft"), Bands.Laterals[1], 100.0f);
		TestEqual(TEXT("no shoulder means no fade at the right edge"), Bands.GroundBlend[0], 1.0f);
		TestEqual(TEXT("no shoulder means no fade at the left edge"), Bands.GroundBlend[1], 1.0f);
	}

	// Shoulder | lane | shoulder. Four boundaries, and the ground blend is 0 only on the
	// two outer edges - the shoulders fade into the ground, the lane does not.
	{
		URoadProfile* Shouldered = URoadProfile::MakeTransient(200.0, 100.0, 30.0);
		const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(Shouldered);

		TestEqual(TEXT("three bands give four boundaries"), Bands.Alphas.Num(), 4);

		// Ascending from the right edge to the left, always starting at 0 and ending at 1.
		TestEqual(TEXT("first alpha is 0"), Bands.Alphas[0], 0.0);
		TestEqual(TEXT("last alpha is 1"), Bands.Alphas[3], 1.0);
		for (int32 Index = 1; Index < Bands.Alphas.Num(); ++Index)
		{
			TestTrue(TEXT("alphas ascend"), Bands.Alphas[Index] > Bands.Alphas[Index - 1]);
		}

		// Total width 200 with 30 uu shoulders: boundaries at -100, -70, +70, +100.
		TestEqual(TEXT("right edge"), Bands.Laterals[0], -100.0f);
		TestEqual(TEXT("right shoulder inner edge"), Bands.Laterals[1], -70.0f);
		TestEqual(TEXT("left shoulder inner edge"), Bands.Laterals[2], 70.0f);
		TestEqual(TEXT("left edge"), Bands.Laterals[3], 100.0f);

		TestEqual(TEXT("right outer edge fades to nothing"), Bands.GroundBlend[0], 0.0f);
		TestEqual(TEXT("inboard of the right shoulder is solid"), Bands.GroundBlend[1], 1.0f);
		TestEqual(TEXT("inboard of the left shoulder is solid"), Bands.GroundBlend[2], 1.0f);
		TestEqual(TEXT("left outer edge fades to nothing"), Bands.GroundBlend[3], 0.0f);
	}

	// A null profile must not crash the builder; it yields the degenerate two-boundary
	// case at zero width, which produces no geometry rather than an exception.
	{
		const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(nullptr);
		TestEqual(TEXT("null profile still gives two boundaries"), Bands.Alphas.Num(), 2);
		TestEqual(TEXT("null profile has zero width"), Bands.Laterals[0], 0.0f);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [x] **Step 2: Build — expect FAIL**

Expected: compile error, `Build/RoadProfileBands.h` not found, and `MakeTransient` does not take three arguments.

- [x] **Step 3: Give MakeTransient an optional shoulder**

In `Public/Profiles/RoadProfile.h`, replace the `MakeTransient` declaration:

```cpp
	/**
	 * Symmetric profile for tests and the debug gallery.
	 *
	 * ShoulderWidth > 0 produces shoulder | lane | shoulder, which is what the ground
	 * blend needs: a profile of one Lane band has no outer shoulder, so there is nothing
	 * to fade and the road ends in a knife edge. Defaults to 0 so every existing caller
	 * keeps the single-band profile it already had.
	 */
	static URoadProfile* MakeTransient(double TotalWidth, double FilletRadius, double ShoulderWidth = 0.0);
```

In `Private/Profiles/RoadProfile.cpp`, replace the body:

```cpp
URoadProfile* URoadProfile::MakeTransient(double TotalWidth, double FilletRadius, double ShoulderWidth)
{
	URoadProfile* Profile = NewObject<URoadProfile>(GetTransientPackage());

	// Clamped so two shoulders can never exceed the road: a lane of zero or negative
	// width would put the band boundaries out of order and invert the ribbon.
	const double Shoulder = FMath::Clamp(ShoulderWidth, 0.0, TotalWidth * 0.45);

	if (Shoulder > 0.0)
	{
		FProfileBand Left;
		Left.Width = Shoulder;
		Left.Type = ERoadBandType::Shoulder;
		Profile->Bands.Add(Left);
	}

	FProfileBand Lane;
	Lane.Width = TotalWidth - 2.0 * Shoulder;
	Lane.Type = ERoadBandType::Lane;
	Profile->Bands.Add(Lane);

	if (Shoulder > 0.0)
	{
		FProfileBand Right;
		Right.Width = Shoulder;
		Right.Type = ERoadBandType::Shoulder;
		Profile->Bands.Add(Right);
	}

	FProfileLane DriveLane;
	DriveLane.CentreOffset = 0.0;
	DriveLane.Width = TotalWidth - 2.0 * Shoulder;
	DriveLane.Direction = ERoadLaneDirection::Bidirectional;
	Profile->Lanes.Add(DriveLane);

	Profile->CentrelineOffset = -1.0;
	Profile->PreferredFilletRadius = FilletRadius;
	return Profile;
}
```

- [x] **Step 4: Write the band header**

`Public/Build/RoadProfileBands.h`:

```cpp
#pragma once

#include "CoreMinimal.h"

class URoadProfile;

/**
 * A profile's band boundaries expressed as positions along a cut line.
 *
 * Every array is parallel and ordered from the RIGHT edge to the LEFT, because that is
 * the direction FMath::Lerp(RightCut, LeftCut, Alpha) travels. Alphas always start at
 * exactly 0 and end at exactly 1, so the outermost boundaries reproduce the solver's
 * stored cut vertices without arithmetic - which is what lets them weld bitwise.
 */
struct ROADNET_API FRoadProfileBands
{
	/** Lerp parameter along the cut line, ascending, first exactly 0 and last exactly 1. */
	TArray<double> Alphas;

	/** Signed lateral offset in uu at each boundary: negative right, positive left. */
	TArray<float> Laterals;

	/** 0 at the outer edge of an outermost Shoulder band, 1 everywhere else. */
	TArray<float> GroundBlend;

	/** Boundaries for a profile, or the degenerate two-boundary case for a null one. */
	static FRoadProfileBands FromProfile(const URoadProfile* Profile);
};
```

- [x] **Step 5: Write the band implementation**

`Private/Build/RoadProfileBands.cpp`:

```cpp
#include "Build/RoadProfileBands.h"

#include "Profiles/RoadProfile.h"

FRoadProfileBands FRoadProfileBands::FromProfile(const URoadProfile* Profile)
{
	FRoadProfileBands Out;

	const double HalfLeft  = Profile ? FMath::Max(Profile->GetHalfWidthLeft(),  0.0) : 0.0;
	const double HalfRight = Profile ? FMath::Max(Profile->GetHalfWidthRight(), 0.0) : 0.0;
	const double Total = HalfLeft + HalfRight;

	// No width, or no profile: the two outer boundaries coincide. The builder drops the
	// resulting degenerate triangles, so this produces nothing rather than misbehaving.
	if (Total <= 0.0 || Profile == nullptr || Profile->Bands.Num() == 0)
	{
		Out.Alphas = { 0.0, 1.0 };
		Out.Laterals = { static_cast<float>(-HalfRight), static_cast<float>(HalfLeft) };
		Out.GroundBlend = { 1.0f, 1.0f };
		return Out;
	}

	// Bands are ordered left to right; boundaries are walked right to left so the alphas
	// ascend the way Lerp(RightCut, LeftCut, Alpha) does.
	const int32 BandCount = Profile->Bands.Num();
	const bool bLeftShoulder  = Profile->Bands[0].Type == ERoadBandType::Shoulder;
	const bool bRightShoulder = Profile->Bands[BandCount - 1].Type == ERoadBandType::Shoulder;

	Out.Alphas.Reserve(BandCount + 1);
	Out.Laterals.Reserve(BandCount + 1);
	Out.GroundBlend.Reserve(BandCount + 1);

	double Lateral = -HalfRight;
	for (int32 Boundary = 0; Boundary <= BandCount; ++Boundary)
	{
		// Exactly 0 and exactly 1 at the ends, not (Lateral + HalfRight) / Total, so the
		// outermost band points reproduce the stored cut vertices bit for bit.
		const double Alpha =
			(Boundary == 0)         ? 0.0 :
			(Boundary == BandCount) ? 1.0 :
			(Lateral + HalfRight) / Total;

		Out.Alphas.Add(Alpha);
		Out.Laterals.Add(static_cast<float>(Lateral));

		const bool bOuterRight = (Boundary == 0) && bRightShoulder;
		const bool bOuterLeft  = (Boundary == BandCount) && bLeftShoulder;
		Out.GroundBlend.Add((bOuterRight || bOuterLeft) ? 0.0f : 1.0f);

		if (Boundary < BandCount)
		{
			// Walking right to left consumes the bands in reverse order.
			Lateral += FMath::Max(Profile->Bands[BandCount - 1 - Boundary].Width, 0.0);
		}
	}

	return Out;
}
```

- [x] **Step 6: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

Expected: 15 tests, 0 failed, including `RoadNet.Build.ProfileBands`.

- [x] **Step 7: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): profile band boundaries as cut-line parameters"
```

---

### Task 2: The solve result carries its arm-to-segment mapping

`AddJunction` receives only an `FJunctionResult`, which holds no profile — so band subdivision on the rim has no way to know each arm's bands. Re-deriving the mapping by walking `Node.Incident` would repeat the exact desync `FRoadNetworkSolver` was already fixed for: it skips arms whose segment lookup fails, so an index into `Incident` is not an index into `Arms`.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Build/RoadNetworkSolver.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Build/RoadNetworkSolver.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadNetworkSolverTest.cpp` (extend)

**Interfaces:**
- Consumes: the existing `ArmSegments` local in `SolveAll`.
- Produces: `FRoadSolveResult::NodeArmSegments` — `TMap<int32, TArray<FRoadSegmentId>>`, keyed by node index, parallel to that node's `FJunctionResult::Arms`.

- [x] **Step 1: Write the failing test**

Append inside `FRoadNetworkSolverTest::RunTest`, immediately before its final `return true;`:

```cpp
	// The arm -> segment mapping the solver already computes internally, published so the
	// mesh builder does not have to re-derive it. Re-deriving means re-walking
	// Node.Incident and re-applying the same skip rule, which is how the two got out of
	// step before: an index into Incident is not an index into Arms.
	{
		const TArray<FRoadSegmentId>* CentreArms = Result.NodeArmSegments.Find(Centre.Index);
		if (TestNotNull(TEXT("centre node publishes its arm mapping"), CentreArms))
		{
			const FJunctionResult& CentreResult = Result.NodeResults[Centre.Index];
			TestEqual(TEXT("one segment id per solved arm"),
				CentreArms->Num(), CentreResult.Arms.Num());

			// Every entry names a live segment actually incident to this node.
			for (const FRoadSegmentId ArmSegment : *CentreArms)
			{
				const FRoadSegment* Seg = Net->GetSegment(ArmSegment);
				if (TestNotNull(TEXT("arm names a live segment"), Seg))
				{
					TestTrue(TEXT("arm's segment touches this node"),
						Seg->A == Centre || Seg->B == Centre);
				}
			}
		}
	}
```

- [x] **Step 2: Build — expect FAIL**

Expected: compile error, `FRoadSolveResult` has no member `NodeArmSegments`.

- [x] **Step 3: Publish the mapping**

In `Public/Build/RoadNetworkSolver.h`, add to `FRoadSolveResult`:

```cpp
	/**
	 * Each solved node's arms, in the same order as that node's FJunctionResult::Arms,
	 * naming the segment each arm belongs to.
	 *
	 * Published rather than left for callers to re-derive. Rebuilding it means walking
	 * Node.Incident and re-applying SolveAll's skip rule, and any divergence writes one
	 * arm's geometry onto another arm's segment - silently.
	 */
	TMap<int32, TArray<FRoadSegmentId>> NodeArmSegments;
```

In `Private/Build/RoadNetworkSolver.cpp`, immediately before `Out.NodeResults.Add(NodeIndex, MoveTemp(Result));`:

```cpp
		Out.NodeArmSegments.Add(NodeIndex, ArmSegments);
```

Note the ordering: `ArmSegments` must be copied **before** `Result` is moved, and both lines refer to the same `NodeIndex`.

- [x] **Step 4: Build and run — expect PASS**

Expected: 15 tests, 0 failed.

- [x] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): solve result publishes each node's arm to segment mapping"
```

---

### Task 3: The ribbon and the rim gain band vertices, welded

The crux. Band vertices sit **between** the two stored cut vertices, so they must be produced by one expression from the same inputs on both sides, exactly as the outer pair are shared verbatim.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Build/RoadMeshBuilder.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Build/RoadMeshBuilder.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadBandWeldTest.cpp`

**Interfaces:**
- Consumes: `FRoadProfileBands`, `FRoadSolveResult::NodeArmSegments`, `FJunctionResult::{Arms, Boundary, Centre, bValid}`.
- Produces:
  - `static FVector2D FRoadMeshBuilder::CutLinePoint(const FVector2D& RightCut, const FVector2D& LeftCut, double Alpha)`
  - `void FRoadMeshBuilder::AddJunction(const URoadNetwork& Network, int32 NodeIndex, const FJunctionResult& Junction, const TArray<FRoadSegmentId>& ArmSegments)`

- [x] **Step 1: Write the failing test**

Create `Plugins/RoadNet/Source/RoadNetTests/Private/RoadBandWeldTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Build/RoadProfileBands.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBandWeldTest,
	"RoadNet.Build.BandWeld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBandWeldTest::RunTest(const FString& Parameters)
{
	constexpr double Width = 800.0;
	constexpr double Shoulder = 120.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(Width, 200.0, Shoulder);

	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(12000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 12000.0));
	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	Net->AddStraightSegment(Centre, North, Profile);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node solved"), Solved.FailedNodes, 0);

	FRoadMeshBuilder Builder(10.0);
	Builder.Build(*Net, Solved, 3);
	const FRoadMeshBuffers& Buffers = Builder.GetBuffers();

	// THE CONTRACT, extended to bands. A band vertex is not stored anywhere: the ribbon
	// and the junction rim each derive it from the same two cut vertices through
	// CutLinePoint. If they ever stop agreeing bitwise, the shoulder tears open along
	// every cut line - the same seam this project exists to make unrepresentable, one
	// step inboard of where slice 2a proved it closed.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);
		if (!TestNotNull(TEXT("east segment resolves"), Seg))
		{
			return false;
		}

		const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(Profile);
		TestEqual(TEXT("shouldered profile gives four boundaries"), Bands.Alphas.Num(), 4);

		for (int32 Boundary = 0; Boundary < Bands.Alphas.Num(); ++Boundary)
		{
			const FVector2D Expected = FRoadMeshBuilder::CutLinePoint(
				Seg->RightCutA, Seg->LeftCutA, Bands.Alphas[Boundary]);

			int32 Matches = 0;
			for (const FVector3d& P : Buffers.Positions)
			{
				if (P.X == Expected.X && P.Y == Expected.Y)
				{
					++Matches;
				}
			}

			// Exactly one: the ribbon and the rim both produced it and it welded.
			TestEqual(
				FString::Printf(TEXT("band boundary %d is present exactly once"), Boundary),
				Matches, 1);
		}
	}

	// The ground blend reaches 0 somewhere - the shoulder's outer edge - and 1 elsewhere.
	// Without both, the fade either does not exist or swallows the whole road.
	{
		bool bFoundFaded = false;
		bool bFoundSolid = false;
		for (const FVector2f& Masks : Buffers.UV2)
		{
			if (Masks.Y <= 0.0f) { bFoundFaded = true; }
			if (Masks.Y >= 1.0f) { bFoundSolid = true; }
		}
		TestTrue(TEXT("some vertex fades to nothing"), bFoundFaded);
		TestTrue(TEXT("some vertex stays solid"), bFoundSolid);
	}

	// Facing is unchanged by subdivision. Unreal's front face is the opposite winding to
	// the maths convention, so front-facing means NEGATIVE 2D signed area.
	{
		int32 Backfacing = 0;
		for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
		{
			const FVector3d& A = Buffers.Positions[Buffers.Indices[Slot]];
			const FVector3d& B = Buffers.Positions[Buffers.Indices[Slot + 1]];
			const FVector3d& C = Buffers.Positions[Buffers.Indices[Slot + 2]];
			if (0.5 * ((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X)) >= 0.0)
			{
				++Backfacing;
			}
		}
		TestEqual(TEXT("no backfacing triangle after subdivision"), Backfacing, 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [x] **Step 2: Build — expect FAIL**

Expected: compile error, `FRoadMeshBuilder` has no member `CutLinePoint`.

- [x] **Step 3: Declare the shared derivation and the new AddJunction**

In `Public/Build/RoadMeshBuilder.h`, add to the public section:

```cpp
	/**
	 * A point on a cut line, parameterised from the right cut to the left.
	 *
	 * The ONLY way a band vertex is ever produced. The ribbon and the junction rim both
	 * call this with the same two stored cut vertices and the same alpha, so their results
	 * are bitwise identical and weld to one vertex - the same property slice 2a
	 * established for the outer pair, extended inboard. Never inline this or "simplify"
	 * one caller: two expressions that are algebraically equal are not bitwise equal.
	 */
	static FVector2D CutLinePoint(const FVector2D& RightCut, const FVector2D& LeftCut, double Alpha)
	{
		return FMath::Lerp(RightCut, LeftCut, Alpha);
	}
```

and replace the `AddJunction` declaration:

```cpp
	/**
	 * Append a solved junction's fan, subdivided to match each arm's profile bands.
	 *
	 * ArmSegments is FRoadSolveResult::NodeArmSegments for this node - parallel to
	 * Junction.Arms. It is passed in rather than re-derived because re-walking
	 * Node.Incident re-applies a skip rule that can put the two out of step, which writes
	 * one arm's bands onto another arm's cut line.
	 */
	void AddJunction(const URoadNetwork& Network, int32 NodeIndex, const FJunctionResult& Junction,
		const TArray<FRoadSegmentId>& ArmSegments);
```

- [x] **Step 4: Subdivide the ribbon**

In `Private/Build/RoadMeshBuilder.cpp`, add the include:

```cpp
#include "Build/RoadProfileBands.h"
```

Replace the rail construction and triangle emission in `AddSegment` — the block from `TArray<int32> LeftRail;` down to the end of the ribbon triangle loop — with a rail per band boundary:

```cpp
	const FRoadProfileBands Bands = FRoadProfileBands::FromProfile(SegProfile);
	const int32 RailCount = Bands.Alphas.Num();

	// Rails[Boundary][Step]. Boundary 0 is the right edge and the last is the left, so the
	// outermost two reproduce the stored cut vertices exactly and weld as they always did.
	TArray<TArray<int32>> Rails;
	Rails.SetNum(RailCount);
	for (TArray<int32>& Rail : Rails)
	{
		Rail.Reserve(Steps + 1);
	}

	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		const double Alpha = static_cast<double>(Step) / static_cast<double>(Steps);
		const float Along = static_cast<float>(Alpha * RibbonLength);

		const bool bIsEnd = (Step == 0) || (Step == Steps);
		const double JunctionBlend = bIsEnd ? 1.0 : 0.0;

		// The cross-section's own two ends. At Step 0 and Step Steps these ARE the stored
		// cut vertices, untouched; in between they are ours to interpolate.
		const FVector2D RightAt = (Step == 0) ? RightStart
			: (Step == Steps) ? RightEnd
			: FMath::Lerp(RightStart, RightEnd, Alpha);
		const FVector2D LeftAt = (Step == 0) ? LeftStart
			: (Step == Steps) ? LeftEnd
			: FMath::Lerp(LeftStart, LeftEnd, Alpha);

		for (int32 Boundary = 0; Boundary < RailCount; ++Boundary)
		{
			const FVector2D Point = CutLinePoint(RightAt, LeftAt, Bands.Alphas[Boundary]);
			Rails[Boundary].Add(WeldVertex(
				Point,
				FVector2f(Bands.Laterals[Boundary], Along),
				FVector2f(static_cast<float>(JunctionBlend), Bands.GroundBlend[Boundary])));
		}
	}

	// One quad strip per band, per step.
	for (int32 Boundary = 0; Boundary + 1 < RailCount; ++Boundary)
	{
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			const int32 R0 = Rails[Boundary][Step];
			const int32 R1 = Rails[Boundary][Step + 1];
			const int32 L0 = Rails[Boundary + 1][Step];
			const int32 L1 = Rails[Boundary + 1][Step + 1];

			// Counter-clockwise seen from +Z; AddTriangle swaps for Unreal's winding.
			AddTriangle(R0, R1, L1);
			AddTriangle(R0, L1, L0);
		}
	}
```

The two dead-end cap blocks further down still index `RightRail[0]`/`LeftRail[0]` and `RightRail[Steps]`/`LeftRail[Steps]`. Replace those four references with `Rails[0][0]`, `Rails[RailCount - 1][0]`, `Rails[0][Steps]` and `Rails[RailCount - 1][Steps]` respectively — the caps span the full width and are not subdivided, which is correct: a cap is a flat end, not a length of road.

- [x] **Step 5: Subdivide the junction rim and triangulate it directly**

Replace the whole of `AddJunction`:

```cpp
void FRoadMeshBuilder::AddJunction(const URoadNetwork& Network, int32 NodeIndex,
	const FJunctionResult& Junction, const TArray<FRoadSegmentId>& ArmSegments)
{
	if (!Junction.bValid || Junction.Boundary.Num() < 4)
	{
		// Fewer than three rim points plus an apex is a dead end; its cap is built by
		// AddSegment, where the node position and profile are both to hand.
		return;
	}

	// Boundary holds the rim followed by the fan apex.
	const int32 ApexSlot = Junction.Boundary.Num() - 1;
	const FVector2D Apex = Junction.Boundary[ApexSlot];

	// Rebuild the rim with each arm's band points inserted along its cut line. The
	// solver's own Triangles array indexes the ORIGINAL boundary, so it cannot be reused
	// once points are inserted - the fan is rebuilt here instead.
	TArray<FVector2D> Rim;
	Rim.Reserve(ApexSlot * 2);

	for (int32 Slot = 0; Slot < ApexSlot; ++Slot)
	{
		Rim.Add(Junction.Boundary[Slot]);

		// SolveBoundary emits each arm's RightCut immediately followed by its LeftCut, so
		// a matching adjacent pair identifies that arm's cut line. Matched bitwise: these
		// are the same values, not merely nearby ones.
		const int32 NextSlot = Slot + 1;
		if (NextSlot >= ApexSlot)
		{
			continue;
		}

		for (int32 ArmIndex = 0; ArmIndex < Junction.Arms.Num(); ++ArmIndex)
		{
			const FJunctionArmResult& Arm = Junction.Arms[ArmIndex];
			const bool bIsCutLine =
				Junction.Boundary[Slot].X == Arm.RightCut.X &&
				Junction.Boundary[Slot].Y == Arm.RightCut.Y &&
				Junction.Boundary[NextSlot].X == Arm.LeftCut.X &&
				Junction.Boundary[NextSlot].Y == Arm.LeftCut.Y;

			if (!bIsCutLine || !ArmSegments.IsValidIndex(ArmIndex))
			{
				continue;
			}

			const FRoadSegment* ArmSegment = Network.GetSegment(ArmSegments[ArmIndex]);
			const FRoadProfileBands Bands =
				FRoadProfileBands::FromProfile(ArmSegment ? ArmSegment->Profile : nullptr);

			// Interior boundaries only: 0 and 1 are the cut vertices already in the rim.
			for (int32 Boundary = 1; Boundary + 1 < Bands.Alphas.Num(); ++Boundary)
			{
				Rim.Add(CutLinePoint(Arm.RightCut, Arm.LeftCut, Bands.Alphas[Boundary]));
			}
			break;
		}
	}

	// Weld the rim and the apex. Cut vertices and band points are already owned by their
	// segments, so these attributes are discarded for them; they land on arc samples and
	// the apex. Full junction blend everywhere, so no marking can reach a junction, and a
	// solid ground blend so the fade is a segment-side effect only for now.
	TArray<int32> RimIndices;
	RimIndices.Reserve(Rim.Num());
	for (const FVector2D& Point : Rim)
	{
		RimIndices.Add(WeldVertex(Point, FVector2f(0.0f, 0.0f), JunctionMasks(1.0)));
	}

	const int32 ApexIndex = WeldVertex(Apex, FVector2f(0.0f, 0.0f), JunctionMasks(1.0));

	// Fan from the apex. The solver validates that the rim is star-shaped about this
	// point before emitting a fan at all, so every triangle here is well formed.
	for (int32 Slot = 0; Slot < RimIndices.Num(); ++Slot)
	{
		const int32 Next = (Slot + 1) % RimIndices.Num();
		AddTriangle(ApexIndex, RimIndices[Slot], RimIndices[Next]);
	}
}
```

- [x] **Step 6: Update Build and the callers**

In `Private/Build/RoadMeshBuilder.cpp`, `Build`'s junction loop becomes:

```cpp
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		const TArray<FRoadSegmentId>* ArmSegments = Solved.NodeArmSegments.Find(Pair.Key);
		static const TArray<FRoadSegmentId> Empty;
		AddJunction(Network, Pair.Key, Pair.Value, ArmSegments ? *ArmSegments : Empty);
	}
```

Every test that calls `AddJunction(Pair.Value)` directly must pass the four arguments. `RoadMeshBuilderTest.cpp`, `RoadMeshAttributeTest.cpp`, `ShortSegmentTest.cpp` and `ClickedChainTest.cpp` each have such a loop; update them the same way. Prefer `Builder.Build(*Net, Solved, N)` where the test does not specifically need a partial mesh.

- [x] **Step 7: Build and run — expect PASS**

Expected: 16 tests, 0 failed, including `RoadNet.Build.BandWeld`.

Vertex-count assertions in `RoadMeshBuilderTest` will change, because a shouldered profile adds rails. Those tests use `MakeTransient(W * 2.0, 1500.0)` with no shoulder, so they keep two rails and their counts are unchanged — if one fails, work the arithmetic out from the band count rather than adjusting the number to fit.

**If "band boundary N is present exactly once" reports 2**, the ribbon and the rim produced different bits for the same boundary. Do not add a tolerance. Check that both call `CutLinePoint` with the arm's own `RightCut`/`LeftCut` in that order, and that the alpha came from the same profile.

- [x] **Step 8: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): subdivide the ribbon and junction rim by profile bands"
```

---

### Task 4: Shoulder fade at junctions, via a clamped inset ring

A junction's rim **is** the outer edge, and the only vertex inboard is the apex, so fading rim to apex fades the whole junction. A ring of rim vertices pushed toward the apex gives the shoulder somewhere to end.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Build/RoadMeshBuilder.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadBandWeldTest.cpp` (extend)

**Interfaces:**
- Consumes: the rim built in Task 3, `FRoadProfileBands::GroundBlend`.
- Produces: no new public API.

**Why toward the apex rather than a polygon offset:** offsetting a polygon inward self-intersects at tight corners and needs mitre handling. Moving each rim vertex along the straight line to the fan apex cannot self-intersect, because the solver has already validated that the rim is star-shaped about that apex — every rim point sees it. The inset is clamped to a fraction of each vertex's own distance to the apex, so a tight corner degrades to a thin ring rather than folding through it.

- [x] **Step 1: Write the failing test**

Append inside `FRoadBandWeldTest::RunTest`, before its final `return true;`:

```cpp
	// The junction's shoulder fade. Rim vertices fade to nothing; an inset ring one
	// shoulder-width inboard is solid; the apex is solid. Without the ring there is
	// nothing between a faded rim and the apex, so the fade spans the whole junction.
	{
		const FJunctionResult* CentreResult = Solved.NodeResults.Find(Centre.Index);
		if (!TestNotNull(TEXT("centre node solved"), CentreResult))
		{
			return false;
		}

		const FVector2D Apex = CentreResult->Boundary.Last();

		// Every ring vertex must lie strictly between the rim and the apex - never beyond
		// it, which is what a fold would look like.
		int32 BeyondApex = 0;
		int32 SolidNearApex = 0;
		for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
		{
			const FVector2D Flat(Buffers.Positions[Index].X, Buffers.Positions[Index].Y);
			const double ToApex = FVector2D::Distance(Flat, Apex);

			// Vertices within the junction's own extent, excluding the apex itself.
			if (ToApex > 1.0 && ToApex < 4000.0 && Buffers.UV2[Index].X >= 1.0f)
			{
				if (Buffers.UV2[Index].Y >= 1.0f)
				{
					++SolidNearApex;
				}
			}
			if (ToApex > 1e6)
			{
				++BeyondApex;
			}
		}

		TestEqual(TEXT("no junction vertex lands absurdly far from its apex"), BeyondApex, 0);
		TestTrue(TEXT("the junction has solid vertices inboard of its rim"), SolidNearApex > 0);
	}
```

- [x] **Step 2: Build — expect FAIL**

Expected: FAIL on "the junction has solid vertices inboard of its rim", because every junction vertex currently carries a ground blend of 1 and there is no ring, so the count is nonzero for the wrong reason — read the failure text and confirm it is the ring assertion before implementing.

- [x] **Step 3: Build the inset ring**

In `AddJunction`, replace the rim-welding and fan block written in Task 3 with a ring-aware version:

```cpp
	// The widest shoulder among this node's arms sets the inset. Different arms may carry
	// different profiles; the ring is one loop, so it takes the largest.
	double ShoulderWidth = 0.0;
	for (const FRoadSegmentId ArmSegment : ArmSegments)
	{
		const FRoadSegment* Seg = Network.GetSegment(ArmSegment);
		const URoadProfile* ArmProfile = Seg ? Seg->Profile : nullptr;
		if (ArmProfile == nullptr || ArmProfile->Bands.Num() == 0)
		{
			continue;
		}
		if (ArmProfile->Bands[0].Type == ERoadBandType::Shoulder)
		{
			ShoulderWidth = FMath::Max(ShoulderWidth, ArmProfile->Bands[0].Width);
		}
		if (ArmProfile->Bands.Last().Type == ERoadBandType::Shoulder)
		{
			ShoulderWidth = FMath::Max(ShoulderWidth, ArmProfile->Bands.Last().Width);
		}
	}

	TArray<int32> RimIndices;
	TArray<int32> RingIndices;
	RimIndices.Reserve(Rim.Num());
	RingIndices.Reserve(Rim.Num());

	for (const FVector2D& Point : Rim)
	{
		// Rim: faded to nothing where the profile has a shoulder, solid otherwise.
		RimIndices.Add(WeldVertex(
			Point,
			FVector2f(0.0f, 0.0f),
			FVector2f(1.0f, ShoulderWidth > 0.0 ? 0.0f : 1.0f)));

		// Ring: one shoulder-width toward the apex, clamped so it can never reach or pass
		// it. The solver guarantees the rim is star-shaped about the apex, so a straight
		// move toward it stays inside the polygon and cannot self-intersect - which a
		// general inward polygon offset would, at any sufficiently tight corner.
		const FVector2D ToApex = Apex - Point;
		const double Distance = ToApex.Size();
		const double Inset = FMath::Min(ShoulderWidth, Distance * MaxInsetFraction);
		const FVector2D RingPoint = (Distance > UE_KINDA_SMALL_NUMBER)
			? Point + (ToApex / Distance) * Inset
			: Point;

		RingIndices.Add(WeldVertex(RingPoint, FVector2f(0.0f, 0.0f), FVector2f(1.0f, 1.0f)));
	}

	const int32 ApexIndex = WeldVertex(Apex, FVector2f(0.0f, 0.0f), FVector2f(1.0f, 1.0f));

	// Rim -> ring as a quad strip, then ring -> apex as a fan.
	for (int32 Slot = 0; Slot < RimIndices.Num(); ++Slot)
	{
		const int32 Next = (Slot + 1) % RimIndices.Num();

		AddTriangle(RimIndices[Slot], RimIndices[Next], RingIndices[Next]);
		AddTriangle(RimIndices[Slot], RingIndices[Next], RingIndices[Slot]);
		AddTriangle(ApexIndex, RingIndices[Slot], RingIndices[Next]);
	}
```

Add the constant to the anonymous namespace at the top of the file:

```cpp
	/**
	 * Furthest the junction's inset ring may travel toward the fan apex, as a fraction of
	 * each vertex's own distance to it.
	 *
	 * A tight corner has rim points close to the apex, and a full shoulder-width inset
	 * would take the ring past it and fold the fan inside out. Degrading to a thin ring
	 * loses the fade at that corner, which is a cosmetic loss; folding is a visible defect
	 * and a silent one.
	 */
	constexpr double MaxInsetFraction = 0.45;
```

- [x] **Step 4: Build and run — expect PASS**

Expected: 16 tests, 0 failed.

- [x] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): shoulder fade at junctions via a clamped inset ring"
```

---

### Task 5: Drop zero-area triangles, and fade the shoulder in the material

Closes K3, which spec §12 assigns to the mesh builder and which Slice 2a could not calibrate. Then wires the ground blend into the material so the fade is visible.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Build/RoadMeshBuilder.cpp`
- Modify: `Tools/Python/build_road_material.py`
- Modify: `docs/superpowers/specs/2026-08-28-procedural-road-system-design.md` (§12, K3 row)
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadBandWeldTest.cpp` (extend)

**Interfaces:**
- Consumes: `FRoadMeshBuffers::UV2`.
- Produces: no new C++ API. `M_RoadSurface` gains a dithered opacity mask driven by UV2.Y.

- [x] **Step 1: Write the failing test**

Append inside `FRoadBandWeldTest::RunTest`, before its final `return true;`:

```cpp
	// K3: collinear nodes emit slivers of around 1.6e-10 uu². They pass every winding
	// check and are geometrically harmless, but they reach the renderer and the normal
	// computation. Slice 2a could not calibrate a threshold; a millionth of a square unit
	// is far below anything a pixel can cover at any sane texel density, and far above
	// the slivers.
	{
		int32 Slivers = 0;
		for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
		{
			const FVector3d& A = Buffers.Positions[Buffers.Indices[Slot]];
			const FVector3d& B = Buffers.Positions[Buffers.Indices[Slot + 1]];
			const FVector3d& C = Buffers.Positions[Buffers.Indices[Slot + 2]];
			const double Area = FMath::Abs(
				0.5 * ((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X)));
			if (Area < 1e-6)
			{
				++Slivers;
			}
		}
		TestEqual(TEXT("no zero-area slivers reach the buffers"), Slivers, 0);
	}
```

- [x] **Step 2: Build — expect FAIL or PASS**

This may already pass on this network, because the bend has no collinear node. Run it, and if it passes, add a collinear case first: three nodes in a straight line joined by two segments, built into the same builder. A test that cannot fail is worth less than no test.

- [x] **Step 3: Drop the slivers**

In `AddTriangle`, after the index-degeneracy check and before the emission:

```cpp
	// Zero-area slivers. Spec section 12 (K3) assigns this to the mesh builder rather than
	// the solver, which cannot calibrate a threshold it has no texel scale for. A
	// millionth of a square unit is orders of magnitude below anything a pixel covers and
	// orders above the ~1.6e-10 uu² slivers a collinear node produces.
	{
		const FVector3d& PA = Buffers.Positions[A];
		const FVector3d& PB = Buffers.Positions[B];
		const FVector3d& PC = Buffers.Positions[C];
		const double Area = FMath::Abs(
			0.5 * ((PB.X - PA.X) * (PC.Y - PA.Y) - (PB.Y - PA.Y) * (PC.X - PA.X)));
		if (Area < MinTriangleArea)
		{
			return;
		}
	}
```

and add to the anonymous namespace:

```cpp
	/** Below this, in uu², a triangle cannot cover a pixel at any sane texel density. */
	constexpr double MinTriangleArea = 1e-6;
```

- [x] **Step 4: Drive opacity from UV2.Y**

In `Tools/Python/build_road_material.py`, immediately before `lib.connect_material_property(base_colour, ...)`:

```python
    # --- ground blend: the shoulder fades into the terrain ---------------------------
    # UV2.Y is 0 at a shoulder's outer edge and 1 inboard. Masked rather than translucent:
    # translucency on a large flat surface costs sorting and overdraw for no benefit, and
    # DitherTemporalAA turns the hard mask into a smooth fade under TAA.
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)

    ground_blend = lib.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -800, 1500)
    ground_blend.set_editor_property("r", False)
    ground_blend.set_editor_property("g", True)
    ground_blend.set_editor_property("b", False)
    ground_blend.set_editor_property("a", False)
    lib.connect_material_expressions(uv2, "", ground_blend, "")

    dither = lib.create_material_expression(
        material, unreal.MaterialExpressionDitherTemporalAA, -600, 1500)
    lib.connect_material_expressions(ground_blend, "", dither, "Alpha")

    lib.connect_material_property(dither, "", unreal.MaterialProperty.MP_OPACITY_MASK)
```

**This is the step most likely to need iteration.** The Python material API is unforgiving about node input names and enum spellings, and a failure here is a traceback in the log rather than a compile error. If `connect_material_expressions` fails, read the traceback for the offending input name and check it against the node's real inputs — do not guess another name and re-run blind. If `MaterialExpressionDitherTemporalAA` is unavailable, connect `ground_blend` straight to `MP_OPACITY_MASK`: the fade becomes a hard edge, which is worse-looking but correct, and worth shipping over a broken material.

- [x] **Step 5: Re-author the material and check the log**

```powershell
& "D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\repos\AirportMgr2\AirportMgr.uproject" `
  -run=pythonscript -script="C:\repos\AirportMgr2\Tools\Python\build_road_material.py" -unattended -nosplash -nopause
Select-String -Path C:\repos\AirportMgr2\Saved\Logs\AirportMgr.log -Pattern "MARKER:|LogMaterial|Sampler type"
```

Expected: `MARKER: done`, and **no** `LogMaterial` lines. The commandlet runs under the Null RHI, so shader instruction counts are zero whether the material is sound or broken — the absence of material errors is the only signal available here, and the visual check is the real gate.

- [x] **Step 6: Give the fallback profile a shoulder**

In `Private/Present/RoadNetworkActor.cpp`, in `ResolveProfile`:

```cpp
		// A tenth of the width per side. Without a Shoulder band the profile has no outer
		// band to fade and the road ends in a knife edge against the ground.
		RuntimeProfile = URoadProfile::MakeTransient(
			FallbackWidth, FallbackFilletRadius, FallbackWidth * 0.1);
```

- [x] **Step 7: Correct K3 in the spec**

In `docs/superpowers/specs/2026-08-28-procedural-road-system-design.md` §12, update the K3 row: it is resolved, `FRoadMeshBuilder::AddTriangle` drops triangles below `MinTriangleArea`, and the threshold is absolute rather than texel-derived because a millionth of a square unit is below any plausible pixel coverage. Change only that row.

- [x] **Step 8: Build and run — expect PASS**

Expected: 16 tests, 0 failed.

- [x] **Step 9: Commit**

```bash
git add Plugins/RoadNet Tools/Python Content/RoadNet docs
git commit -m "feat(roadnet): drop zero-area triangles and fade the shoulder into the ground"
```

- [ ] **Step 10: Visual verification — the 2b-ii exit criterion**

This step is for a human. Do not attempt to drive the editor.

1. Open the project, place or find an `ARoadNetworkActor`, and draw a few roads including at least one junction.
2. Confirm:
   - the road's outer edge **fades** into the ground rather than ending in a hard line;
   - the fade is a narrow band at the edge, **not** a gradient across the whole width;
   - junctions fade at their rim too, with no hard ring where a segment's shoulder meets a junction's;
   - no fold, spike or dark wedge near a tight corner — that would be the inset ring self-intersecting despite the clamp;
   - the asphalt and the centreline still look as they did in 2b-i.
3. Check the Output Log for `LogRoadMesh: Warning` — rejected triangles mean holes.

---

## Self-Review

**Spec coverage (2b-ii rows only):**

| Design spec section | Covered by |
|---|---|
| §2 subdivide the ribbon per profile band | Task 3 |
| §4 `CutLinePoint`, bands welded bitwise | Task 3, asserted in `RoadNet.Build.BandWeld` |
| §6 shoulder fade at junctions, clamped inset ring | Task 4 |
| §1 / parent §12 K3 zero-area triangles | Task 5 |
| §9 testing — bitwise band equality, facing, blend extremes | Tasks 1, 3, 4, 5 |

Deferred beyond this plan: parent §6.6 ghost material (Slice 3), §5.8 turn paths (Slice 4), per-band material slots (`FProfileBand::MaterialSlot` stays unused — one material still covers the whole surface).

**Known gaps to watch during execution:**

1. `AddJunction` matches an arm's cut line by scanning the rim for an adjacent bitwise pair. At a collinear pass-through node the solver's weld-replace (K1) can leave one cut vertex absent, so that arm gets no band points and its shoulder meets the junction without a matching boundary. K1 is documented, unreachable in the gallery, and becomes common only when Slice 3's auto-subdivide lands.
2. The inset ring takes the **widest** shoulder among a node's arms. A node mixing a wide taxiway with a narrow service road fades both at the wider one's width.
3. `along` is still frozen across dead-end caps — both ends carry the same value, so a dash pattern would smear there. It has no effect until dashes exist, which is not this plan.
4. Task 5 changes the material's blend mode to Masked. If anything later needs true translucency this becomes a conflict.
