# Road System Slice 2b-i — UVs and Asphalt Material — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the road surface real asphalt and UV1-driven centreline markings, so the gallery stops rendering as a flat placeholder colour.

**Architecture:** `FRoadMeshBuffers` gains UV0, UV1 and vertex colours, written through `WeldVertex` on first append. UV0 is world-aligned XY, so it is a pure function of position and cannot disagree between a junction and a segment. UV1 carries `(lateral offset, distance along)`; segments own it at shared cut vertices, which is why segments are now added *before* junctions. `FDynamicMeshSink` pushes all of it into `FDynamicMesh3`'s attribute overlays. A Python commandlet imports the textures and authors the master material headlessly.

**Tech Stack:** Unreal Engine 5.8.2, C++20, MSVC 14.51.36231, `GeometryFramework` + `GeometryCore`, `PythonScriptPlugin` (enabled), Unreal Automation Test framework.

**Spec:** `docs/superpowers/specs/2026-08-28-road-slice2b-materials-design.md` (§3, §5, §7, §9). Parent: `2026-08-28-procedural-road-system-design.md` §6.3, §6.4.

**Scope note:** this is the first of two plans for Slice 2b. Lateral band subdivision, shoulder fade via the clamped inset ring, and K3's zero-area triangle drop are **2b-ii** and are deliberately absent here. `FProfileBand::Type` and `MaterialSlot` therefore stay unused until 2b-ii.

## Global Constraints

- **Engine:** Unreal Engine 5.8.2 at `D:\Epic\UE_5.8`. Project at `C:\repos\AirportMgr2`.
- **`BuildSettingsVersion.V7`** — return-type, dangling-reference and unreachable-code warnings are **errors**.
- **`Solve/` must keep ZERO engine dependencies** beyond `CoreMinimal.h`. This plan adds no includes there and changes nothing under `Solve/`.
- `Build/` depends on `Model`, `Profiles` and `Solve`. `Present/` depends on `Build`. Nothing depends upward.
- **Automation flags:** `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter`. The nested `EAutomationTestFlags::ApplicationContextMask` form does not compile in 5.8.
- Test files wrapped in `#if WITH_DEV_AUTOMATION_TESTS` … `#endif`.
- **Never use the bare `PI` macro** — deprecated float in 5.8. Use `UE_DOUBLE_PI`.
- **`FVector2D` is double-precision. Never narrow to `float`** for positions. Mesh positions are `FVector3d`. **UVs and vertex colours are float** (`FVector2f`, `FColor`) because that is what `FDynamicMesh3`'s overlays store.
- **Shared vertices are asserted bitwise with `==`, never a tolerance.** If an assertion on a shared vertex needs an epsilon, the contract has broken.
- Unreal prefixes `F`/`U`/`A`/`E` are required by UnrealHeaderTool.

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

Run a Python commandlet (also needs the editor closed):

```powershell
& "D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\repos\AirportMgr2\AirportMgr.uproject" `
  -run=pythonscript -script="C:\repos\AirportMgr2\Tools\Python\<name>.py" -unattended -nosplash -nopause
```

Python `print()` goes to the log, not stdout. Read results with:

```powershell
Select-String -Path C:\repos\AirportMgr2\Saved\Logs\AirportMgr.log -Pattern "MARKER:"
```

---

## File Structure

```
Plugins/RoadNet/Source/RoadNet/
  Public/Build/RoadMeshSink.h          MODIFY  - FRoadMeshBuffers gains UV0, UV1, Colors
  Public/Build/RoadMeshBuilder.h       MODIFY  - attributed WeldVertex, TexelsPerUnit
  Private/Build/RoadMeshBuilder.cpp    MODIFY  - write UV1 and colours; UV0 from position
  Public/Present/RoadNetworkActor.h    MODIFY  - SurfaceMaterial property
  Private/Present/RoadNetworkActor.cpp MODIFY  - sink pushes overlays; segments before junctions
  Private/Debug/RoadJunctionGallery.cpp MODIFY - segments before junctions

Plugins/RoadNet/Source/RoadNetTests/Private/
  RoadMeshAttributeTest.cpp            CREATE  - UV/colour generation and the ordering rule

Tools/Python/
  build_road_material.py               CREATE  - imports textures, authors M_RoadSurface
  probe_material_api.py                DELETE  - spike artifact, superseded
```

---

### Task 1: Buffers carry UV0, UV1 and vertex colours

The mesh has no attributes at all today, so there is nothing for a material to read. This task adds the channels and the rule that keeps them consistent at a shared vertex.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Build/RoadMeshSink.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Build/RoadMeshBuilder.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Build/RoadMeshBuilder.cpp`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Present/RoadNetworkActor.cpp`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Debug/RoadJunctionGallery.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadMeshAttributeTest.cpp`

**Interfaces:**
- Consumes: `FRoadSegment::{LeftCutA,RightCutA,LeftCutB,RightCutB}`, `URoadProfile::GetHalfWidthLeft/Right`, `URoadNetwork::GetOutgoingTangent`.
- Produces:
  - `FRoadMeshBuffers` gains `TArray<FVector2f> UV0; TArray<FVector2f> UV1; TArray<FColor> Colors;`
  - `FRoadMeshBuilder::FRoadMeshBuilder(double InZHeight, double InTexelsPerUnit = 512.0)`
  - private `int32 WeldVertex(const FVector2D& Point, const FVector2f& InUV1, const FColor& InColor)`

**The ordering rule this task establishes:** a cut vertex is one vertex holding one UV1. A segment measures `along` from end A, so its B-end cut vertices want `along = length`, while the junction sitting at B would want `along = 0`. Both cannot win. Segments are therefore added **before** junctions, and `WeldVertex` is first-writer-wins, so segments own the shared attributes.

- [ ] **Step 1: Write the failing test**

Create `Plugins/RoadNet/Source/RoadNetTests/Private/RoadMeshAttributeTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadMeshAttributeTest,
	"RoadNet.Build.MeshAttributes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadMeshAttributeTest::RunTest(const FString& Parameters)
{
	constexpr double TotalWidth = 800.0;
	constexpr double FilletRadius = 200.0;
	constexpr double TexelsPerUnit = 512.0;
	constexpr double ZHeight = 10.0;

	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(TotalWidth, FilletRadius);

	// A bend, so there is a real junction with a fan as well as two dead ends.
	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(12000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 12000.0));
	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	const FRoadSegmentId ToNorth = Net->AddStraightSegment(Centre, North, Profile);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	TestEqual(TEXT("every node solved"), Solved.FailedNodes, 0);

	// Segments FIRST, then junctions. The order is the contract: it decides who owns
	// UV1 at a shared cut vertex.
	FRoadMeshBuilder Builder(ZHeight, TexelsPerUnit);
	Builder.AddSegment(*Net, ToEast, 1);
	Builder.AddSegment(*Net, ToNorth, 1);
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		Builder.AddJunction(Pair.Value);
	}

	const FRoadMeshBuffers& Buffers = Builder.GetBuffers();

	// Every channel stays parallel to Positions, or the sink cannot index them.
	TestEqual(TEXT("UV0 is parallel to positions"), Buffers.UV0.Num(), Buffers.Positions.Num());
	TestEqual(TEXT("UV1 is parallel to positions"), Buffers.UV1.Num(), Buffers.Positions.Num());
	TestEqual(TEXT("colours are parallel to positions"), Buffers.Colors.Num(), Buffers.Positions.Num());

	// UV0 is a pure function of world position. This is what makes the asphalt continuous
	// across a junction boundary for free (design spec 6.3) - it cannot disagree between a
	// junction and a segment, because it never depends on which one wrote it.
	for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
	{
		const FVector3d& P = Buffers.Positions[Index];
		TestEqual(FString::Printf(TEXT("UV0.X derives from X at vertex %d"), Index),
			Buffers.UV0[Index].X, static_cast<float>(P.X / TexelsPerUnit));
		TestEqual(FString::Printf(TEXT("UV0.Y derives from Y at vertex %d"), Index),
			Buffers.UV0[Index].Y, static_cast<float>(P.Y / TexelsPerUnit));
	}

	// THE ORDERING RULE. The B end of ToEast is a dead end at (12000, 0). Its cut
	// vertices are shared with nothing, but its ribbon-end vertices carry along =
	// the trimmed length. If a junction had overwritten them with along = 0, this fails.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);
		if (!TestNotNull(TEXT("east segment resolves"), Seg))
		{
			return false;
		}

		const double Length = FVector2D::Distance(
			Net->GetNodes()[Centre.Index].Position, Net->GetNodes()[East.Index].Position);
		const double ExpectedAlong = Length - Seg->TrimB;

		int32 Found = INDEX_NONE;
		for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
		{
			if (Buffers.Positions[Index].X == Seg->LeftCutB.X &&
				Buffers.Positions[Index].Y == Seg->LeftCutB.Y)
			{
				Found = Index;
				break;
			}
		}

		if (TestTrue(TEXT("the B-end cut vertex is in the buffer"), Found != INDEX_NONE))
		{
			TestTrue(
				FString::Printf(TEXT("along at the B end is the trimmed length (got %f, want %f)"),
					Buffers.UV1[Found].Y, ExpectedAlong),
				FMath::IsNearlyEqual(static_cast<double>(Buffers.UV1[Found].Y), ExpectedAlong, 0.5));
		}
	}

	// Lateral is signed across the profile: left positive, right negative, and its
	// magnitude is the half-width. Checked on the A-end cut vertices, which the segment
	// owns outright.
	{
		const FRoadSegment* Seg = Net->GetSegment(ToEast);
		const float HalfLeft  = static_cast<float>(Profile->GetHalfWidthLeft());
		const float HalfRight = static_cast<float>(Profile->GetHalfWidthRight());

		for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
		{
			if (Buffers.Positions[Index].X == Seg->LeftCutA.X &&
				Buffers.Positions[Index].Y == Seg->LeftCutA.Y)
			{
				TestEqual(TEXT("left rail lateral is +HalfWidthLeft"), Buffers.UV1[Index].X, HalfLeft);
			}
			if (Buffers.Positions[Index].X == Seg->RightCutA.X &&
				Buffers.Positions[Index].Y == Seg->RightCutA.Y)
			{
				TestEqual(TEXT("right rail lateral is -HalfWidthRight"), Buffers.UV1[Index].X, -HalfRight);
			}
		}
	}

	// Junction blend: 0 wherever a segment wrote, 1 at a fan apex. The apex is the only
	// vertex that is not on any cut line or rail, so at least one vertex must carry 255.
	{
		bool bFoundApex = false;
		for (const FColor& Colour : Buffers.Colors)
		{
			if (Colour.G == 255)
			{
				bFoundApex = true;
				break;
			}
		}
		TestTrue(TEXT("a junction apex carries full junction blend"), bFoundApex);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Build — expect FAIL**

Run the build command. Expected: compile errors — `FRoadMeshBuffers` has no member `UV0`, and `FRoadMeshBuilder` has no two-argument constructor. That is the failure; it proves the test exercises something that does not exist.

- [ ] **Step 3: Add the channels to the buffers**

In `Public/Build/RoadMeshSink.h`, replace the `FRoadMeshBuffers` struct body with:

```cpp
struct FRoadMeshBuffers
{
	TArray<FVector3d> Positions;
	TArray<int32>     Indices;

	/**
	 * World-aligned XY, divided by the texel scale. A pure function of Positions, which
	 * is exactly why design spec 6.3 chose it: asphalt is continuous across a
	 * segment/junction boundary by construction, because neither side can disagree
	 * about a value that depends only on where the vertex is.
	 */
	TArray<FVector2f> UV0;

	/** X = lateral offset across the profile in uu, Y = distance along the centreline in uu. */
	TArray<FVector2f> UV1;

	/** A = ground blend (unused until 2b-ii), G = junction blend, R and B reserved. */
	TArray<FColor> Colors;
};
```

Note the removal of `Reset()`: it was dead, and it is now actively unsafe, because resetting the buffers without the builder's weld map would desync five arrays instead of two.

- [ ] **Step 4: Make WeldVertex carry the attributes**

In `Public/Build/RoadMeshBuilder.h`, change the constructor and the private helper:

```cpp
	explicit FRoadMeshBuilder(double InZHeight, double InTexelsPerUnit = 512.0);
```

```cpp
private:
	/**
	 * Returns the index of Point, appending it only if this exact value is new.
	 *
	 * FIRST WRITER WINS. When the point is already present the incoming UV1 and colour
	 * are discarded, because a welded vertex can only hold one of each. That is why
	 * callers add segments BEFORE junctions: a segment measures `along` from its A end,
	 * so its B-end cut vertices carry along = length, while the junction at that node
	 * would write along = 0. Segments must therefore write first and own the shared
	 * attributes; junctions then supply values only for the vertices they alone
	 * introduce - arc samples and the fan apex.
	 */
	int32 WeldVertex(const FVector2D& Point, const FVector2f& InUV1, const FColor& InColor);

	void AddTriangle(int32 A, int32 B, int32 C);

	double ZHeight;
	double TexelsPerUnit;
	FRoadMeshBuffers Buffers;
	TMap<FVector2D, int32> WeldMap;
```

- [ ] **Step 5: Implement the attributed weld**

In `Private/Build/RoadMeshBuilder.cpp`, replace the constructor and `WeldVertex`:

```cpp
FRoadMeshBuilder::FRoadMeshBuilder(double InZHeight, double InTexelsPerUnit)
	: ZHeight(InZHeight)
	, TexelsPerUnit(InTexelsPerUnit > 0.0 ? InTexelsPerUnit : 1.0)
{
}

int32 FRoadMeshBuilder::WeldVertex(const FVector2D& Point, const FVector2f& InUV1, const FColor& InColor)
{
	// FVector2D::operator== compares X and Y by value, under which -0.0 == +0.0. But
	// GetTypeHash(const TVector2<T>&) is a CRC over the raw bytes, so -0.0 and +0.0 hash
	// to different buckets - the one case where this map's key equality and its hash
	// disagree. Left alone, Add(-0.0, ...) followed by Find(+0.0) misses, and the map
	// ends up holding two entries for what operator== calls one position: a duplicate
	// coincident vertex, exactly what welding on bits exists to make unrepresentable.
	// Normalising signed zero on the way in closes that gap without adding a tolerance -
	// a tolerance here would paper over a solver that had stopped sharing its vertices.
	const FVector2D Key(NormalizeSignedZero(Point.X), NormalizeSignedZero(Point.Y));

	if (const int32* Existing = WeldMap.Find(Key))
	{
		// First writer wins - see the header for why this is the contract and not a
		// convenience.
		return *Existing;
	}

	const int32 NewIndex = Buffers.Positions.Add(FVector3d(Key.X, Key.Y, ZHeight));

	// UV0 is derived here and nowhere else, so it is impossible for two callers to
	// supply different world-aligned UVs for the same position.
	Buffers.UV0.Add(FVector2f(
		static_cast<float>(Key.X / TexelsPerUnit),
		static_cast<float>(Key.Y / TexelsPerUnit)));
	Buffers.UV1.Add(InUV1);
	Buffers.Colors.Add(InColor);

	WeldMap.Add(Key, NewIndex);
	return NewIndex;
}
```

- [ ] **Step 6: Write UV1 and colours from the segment**

Still in `RoadMeshBuilder.cpp`, replace the body of `AddSegment` between the `Steps` line and the ribbon triangle loop. Add these helpers just below `NormalizeSignedZero` in the anonymous namespace:

```cpp
	/** Vertex colour for a vertex a segment owns: no junction blend, fully opaque. */
	FColor SegmentColour()
	{
		return FColor(0, 0, 0, 255);
	}

	/** Vertex colour for a junction's own vertices, blended by how far into the fan they are. */
	FColor JunctionColour(double Blend)
	{
		const uint8 G = static_cast<uint8>(FMath::Clamp(Blend, 0.0, 1.0) * 255.0 + 0.5);
		return FColor(0, G, 0, 255);
	}
```

Then, inside `AddSegment`, replace the rail-building loop with one that computes `along` and `lateral`:

```cpp
	const URoadProfile* SegProfile = Segment->Profile;
	const float LateralLeft  = static_cast<float>(SegProfile ? FMath::Max(SegProfile->GetHalfWidthLeft(), 0.0) : 0.0);
	const float LateralRight = static_cast<float>(SegProfile ? FMath::Max(SegProfile->GetHalfWidthRight(), 0.0) : 0.0);

	// `along` runs from the A-end cut to the B-end cut, so the ribbon's own length rather
	// than the node-to-node distance. Markings therefore start at the cut line, which is
	// where the surface actually starts.
	const double RibbonLength = FVector2D::Distance(LeftStart, LeftEnd);

	TArray<int32> LeftRail;
	TArray<int32> RightRail;
	LeftRail.Reserve(Steps + 1);
	RightRail.Reserve(Steps + 1);

	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		const double Alpha = static_cast<double>(Step) / static_cast<double>(Steps);
		const float Along = static_cast<float>(Alpha * RibbonLength);

		if (Step == 0)
		{
			LeftRail.Add(WeldVertex(LeftStart, FVector2f(LateralLeft, Along), SegmentColour()));
			RightRail.Add(WeldVertex(RightStart, FVector2f(-LateralRight, Along), SegmentColour()));
		}
		else if (Step == Steps)
		{
			LeftRail.Add(WeldVertex(LeftEnd, FVector2f(LateralLeft, Along), SegmentColour()));
			RightRail.Add(WeldVertex(RightEnd, FVector2f(-LateralRight, Along), SegmentColour()));
		}
		else
		{
			// Interior samples are ours alone and may be interpolated freely; only the
			// ends are shared with a junction.
			LeftRail.Add(WeldVertex(FMath::Lerp(LeftStart, LeftEnd, Alpha),
				FVector2f(LateralLeft, Along), SegmentColour()));
			RightRail.Add(WeldVertex(FMath::Lerp(RightStart, RightEnd, Alpha),
				FVector2f(-LateralRight, Along), SegmentColour()));
		}
	}
```

In the two dead-end cap blocks, the four `WeldVertex(...)` calls gain attributes. The A-end cap sits at `along = 0` (before the ribbon starts) and the B-end cap at `along = RibbonLength`, because the cap is the surface between the node and the cut line:

```cpp
		const int32 R0 = WeldVertex(CapRight, FVector2f(-LateralRight, 0.0f), SegmentColour());
		const int32 R1 = RightRail[0];
		const int32 L1 = LeftRail[0];
		const int32 L0 = WeldVertex(CapLeft, FVector2f(LateralLeft, 0.0f), SegmentColour());
```

and for the B end:

```cpp
		const int32 R0 = RightRail[Steps];
		const int32 R1 = WeldVertex(CapRight, FVector2f(-LateralRight, static_cast<float>(RibbonLength)), SegmentColour());
		const int32 L1 = WeldVertex(CapLeft, FVector2f(LateralLeft, static_cast<float>(RibbonLength)), SegmentColour());
		const int32 L0 = LeftRail[Steps];
```

- [ ] **Step 7: Write UV1 and colours from the junction**

Replace `AddJunction`'s mapping loop. `Junction.Boundary` holds the rim followed by the fan apex, so the last entry is the apex and gets full junction blend; rim entries get none:

```cpp
void FRoadMeshBuilder::AddJunction(const FJunctionResult& Junction)
{
	if (!Junction.bValid || Junction.Triangles.Num() == 0)
	{
		return;
	}

	// Boundary holds the rim followed by the fan apex; Triangles indexes into it.
	// Map every boundary slot through the weld map once, then re-index.
	//
	// Almost every rim vertex is a cut vertex a segment has already welded, so these
	// attributes are discarded for those - see WeldVertex. They apply to the arc samples
	// and the apex, which no segment touches. The apex carries full junction blend so
	// markings taper out toward a junction's centre instead of stopping dead at the rim.
	const int32 ApexSlot = Junction.Boundary.Num() - 1;

	TArray<int32> Mapped;
	Mapped.Reserve(Junction.Boundary.Num());
	for (int32 Slot = 0; Slot < Junction.Boundary.Num(); ++Slot)
	{
		const bool bIsApex = (Slot == ApexSlot);
		const FVector2D& Point = Junction.Boundary[Slot];

		// An arc sample is an outer-edge point, so its lateral is the rim, and `along`
		// stays 0 because a junction has no centreline to measure along. Neither value
		// is read while junction blend fades the markings out.
		Mapped.Add(WeldVertex(
			Point,
			FVector2f(0.0f, 0.0f),
			bIsApex ? JunctionColour(1.0) : JunctionColour(0.0)));
	}

	for (int32 Slot = 0; Slot + 2 < Junction.Triangles.Num(); Slot += 3)
	{
		AddTriangle(
			Mapped[Junction.Triangles[Slot]],
			Mapped[Junction.Triangles[Slot + 1]],
			Mapped[Junction.Triangles[Slot + 2]]);
	}
}
```

- [ ] **Step 8: Reverse the rebuild order in both callers**

This is the load-bearing change. In `Private/Present/RoadNetworkActor.cpp`, inside `RebuildMesh`, move the segment loop **above** the junction loop and add the comment:

```cpp
	FRoadMeshBuilder Builder(SurfaceZ);

	// Segments first, junctions second, and the order is a contract rather than a style
	// choice. A cut vertex is one welded vertex holding one UV1, and WeldVertex is
	// first-writer-wins. A segment measures `along` from its A end, so its B-end cut
	// vertices carry along = length, while the junction standing at that node would
	// write along = 0. Segments must write first or every segment's markings jump at one
	// end, with nothing failing.
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

	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		Builder.AddJunction(Pair.Value);
	}
```

Make the identical change in `Private/Debug/RoadJunctionGallery.cpp`'s `RebuildGalleryMesh`, keeping its hardcoded `1` for ribbon segments and the same comment.

- [ ] **Step 9: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

Expected: 14 tests, 0 failed, including `RoadNet.Build.MeshAttributes`.

If "along at the B end is the trimmed length" fails with a value of 0, the ordering change in Step 8 did not take — check both callers, not just one.

- [ ] **Step 10: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): mesh buffers carry UV0, UV1 and vertex colours"
```

---

### Task 2: The sink pushes attributes into the dynamic mesh

Buffers with UVs are useless until they reach `FDynamicMesh3`. Its overlays are per-triangle-corner, not per-vertex, so this is not a straight copy — and the mesh currently has no attribute set at all.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Present/RoadNetworkActor.cpp`
- Test: `Plugins/RoadNet/Source/RoadNetTests/Private/RoadMeshAttributeTest.cpp` (extend)

**Interfaces:**
- Consumes: `FRoadMeshBuffers::{Positions,Indices,UV0,UV1,Colors}`.
- Produces: no new public API. `FDynamicMeshSink::Accept` populates two UV overlays and the primary colour overlay.

- [ ] **Step 1: Write the failing test**

Append inside `FRoadMeshAttributeTest::RunTest`, immediately before its final `return true;`:

```cpp
	// The overlays the material actually samples. The buffers being right proves nothing
	// about what the component receives - that gap is exactly where slice 2a's invisible
	// surface hid, so it gets a test of its own.
	{
		UE::Geometry::FDynamicMesh3 Mesh;
		for (const FVector3d& Position : Buffers.Positions)
		{
			Mesh.AppendVertex(Position);
		}
		for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
		{
			Mesh.AppendTriangle(
				Buffers.Indices[Slot], Buffers.Indices[Slot + 1], Buffers.Indices[Slot + 2]);
		}

		FDynamicMeshSink::PopulateAttributes(Mesh, Buffers);

		if (!TestTrue(TEXT("attributes are enabled"), Mesh.HasAttributes()))
		{
			return false;
		}
		TestEqual(TEXT("two UV layers exist"), Mesh.Attributes()->NumUVLayers(), 2);
		TestTrue(TEXT("primary colours are enabled"), Mesh.Attributes()->HasPrimaryColors());

		const UE::Geometry::FDynamicMeshUVOverlay* UV0Layer = Mesh.Attributes()->GetUVLayer(0);
		const UE::Geometry::FDynamicMeshUVOverlay* UV1Layer = Mesh.Attributes()->GetUVLayer(1);

		TestEqual(TEXT("UV0 has one element per vertex"), UV0Layer->ElementCount(), Buffers.Positions.Num());
		TestEqual(TEXT("UV1 has one element per vertex"), UV1Layer->ElementCount(), Buffers.Positions.Num());

		// Every triangle must be set in both layers, or that triangle samples nothing.
		int32 UnsetUV0 = 0;
		int32 UnsetUV1 = 0;
		for (const int32 TriangleId : Mesh.TriangleIndicesItr())
		{
			if (!UV0Layer->IsSetTriangle(TriangleId)) { ++UnsetUV0; }
			if (!UV1Layer->IsSetTriangle(TriangleId)) { ++UnsetUV1; }
		}
		TestEqual(TEXT("every triangle has UV0"), UnsetUV0, 0);
		TestEqual(TEXT("every triangle has UV1"), UnsetUV1, 0);
	}
```

Add these includes at the top of the test file:

```cpp
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Present/RoadNetworkActor.h"
```

- [ ] **Step 2: Build — expect FAIL**

Expected: compile error — `FDynamicMeshSink` has no member `PopulateAttributes`.

- [ ] **Step 3: Declare the helper**

In `Public/Present/RoadNetworkActor.h`, add to `FDynamicMeshSink`'s public section:

```cpp
	/**
	 * Copy the buffers' UV and colour channels onto an already-populated mesh.
	 *
	 * Static and public so it can be tested without a component, a world or a renderer.
	 * The buffers being correct says nothing about what the component receives, and that
	 * gap is where slice 2a's invisible surface hid.
	 */
	static void PopulateAttributes(UE::Geometry::FDynamicMesh3& Mesh, const FRoadMeshBuffers& Buffers);
```

and forward-declare above the class:

```cpp
namespace UE::Geometry { class FDynamicMesh3; }
```

- [ ] **Step 4: Implement it**

In `Private/Present/RoadNetworkActor.cpp`, add above `FDynamicMeshSink::Accept`:

```cpp
void FDynamicMeshSink::PopulateAttributes(UE::Geometry::FDynamicMesh3& Mesh, const FRoadMeshBuffers& Buffers)
{
	using namespace UE::Geometry;

	Mesh.EnableAttributes();
	Mesh.Attributes()->SetNumUVLayers(2);
	Mesh.Attributes()->EnablePrimaryColors();

	FDynamicMeshUVOverlay* UV0Layer = Mesh.Attributes()->GetUVLayer(0);
	FDynamicMeshUVOverlay* UV1Layer = Mesh.Attributes()->GetUVLayer(1);
	FDynamicMeshColorOverlay* ColorLayer = Mesh.Attributes()->PrimaryColors();

	// The mesh is fully welded, so there is exactly one UV and one colour per vertex and
	// the overlay element ids can be kept identical to the vertex ids. That is only safe
	// because welding is on exact bits: a tolerance-welded mesh would need split elements
	// wherever two surfaces met at a seam.
	for (int32 Index = 0; Index < Buffers.Positions.Num(); ++Index)
	{
		UV0Layer->AppendElement(Buffers.UV0[Index]);
		UV1Layer->AppendElement(Buffers.UV1[Index]);
		ColorLayer->AppendElement(FVector4f(
			Buffers.Colors[Index].R / 255.0f,
			Buffers.Colors[Index].G / 255.0f,
			Buffers.Colors[Index].B / 255.0f,
			Buffers.Colors[Index].A / 255.0f));
	}

	for (const int32 TriangleId : Mesh.TriangleIndicesItr())
	{
		const FIndex3i Corners = Mesh.GetTriangle(TriangleId);
		UV0Layer->SetTriangle(TriangleId, Corners);
		UV1Layer->SetTriangle(TriangleId, Corners);
		ColorLayer->SetTriangle(TriangleId, Corners);
	}
}
```

- [ ] **Step 5: Call it from Accept**

In `FDynamicMeshSink::Accept`, replace the `QuickComputeVertexNormals` line with:

```cpp
	PopulateAttributes(Mesh, Buffers);

	// With an attribute set present the renderer reads the normal overlay, not the
	// per-vertex normals, so both are computed: the overlay for rendering and the
	// per-vertex normals because they cost nothing and keep the mesh self-describing.
	UE::Geometry::FMeshNormals::QuickComputeVertexNormals(Mesh);
	UE::Geometry::FMeshNormals::InitializeOverlayToPerVertexNormals(Mesh.Attributes()->PrimaryNormals(), false);
```

Add the include:

```cpp
#include "DynamicMesh/DynamicMeshAttributeSet.h"
```

- [ ] **Step 6: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

Expected: 14 tests, 0 failed.

- [ ] **Step 7: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): push UV and colour overlays into the dynamic mesh"
```

---

### Task 3: Import the asphalt textures and author the material

No GUI editor work. `PythonScriptPlugin` is already enabled in the `.uproject`, and `MaterialEditingLibrary`, `MaterialFactoryNew`, `AssetImportTask` and asset tools were verified reachable through `-run=pythonscript`.

**Files:**
- Create: `Tools/Python/build_road_material.py`
- Delete: `Tools/Python/probe_material_api.py`

**Interfaces:**
- Produces: `/Game/RoadNet/Materials/M_RoadSurface` with scalar parameters `TexelsPerUnit`, `CentrelineWidth`, `EdgeLineInset`, `EdgeLineWidth`, `DashLength`, `DashGap` and vector parameter `MarkingColor`; textures under `/Game/RoadNet/Textures/`.

- [ ] **Step 1: Write the script**

Create `Tools/Python/build_road_material.py`:

```python
"""Imports the asphalt PBR set and authors M_RoadSurface. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because
print() goes to the log rather than stdout under the commandlet.
"""
import os
import unreal

SOURCE = r"C:\repos\models\materials\concrete-bl\pebbled-asphalt1-bl"
TEX_DIR = "/Game/RoadNet/Textures"
MAT_DIR = "/Game/RoadNet/Materials"

# name -> (file, is_srgb, compression)
MAPS = {
    "T_Asphalt_Albedo":    ("pebbled_asphalt_albedo.png",    True,  unreal.TextureCompressionSettings.TC_DEFAULT),
    "T_Asphalt_Normal":    ("pebbled_asphalt_Normal-ogl.png", False, unreal.TextureCompressionSettings.TC_NORMALMAP),
    "T_Asphalt_Roughness": ("pebbled_asphalt_Roughness.png", False, unreal.TextureCompressionSettings.TC_MASKS),
    "T_Asphalt_AO":        ("pebbled_asphalt_ao.png",        False, unreal.TextureCompressionSettings.TC_MASKS),
}


def import_textures():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    imported = {}
    for name, (filename, srgb, compression) in MAPS.items():
        path = os.path.join(SOURCE, filename)
        if not os.path.isfile(path):
            unreal.log_error("MARKER: missing source texture %s" % path)
            continue

        task = unreal.AssetImportTask()
        task.filename = path
        task.destination_path = TEX_DIR
        task.destination_name = name
        task.automated = True
        task.replace_existing = True
        task.save = True
        tools.import_asset_tasks([task])

        asset = unreal.load_asset("%s/%s" % (TEX_DIR, name))
        if asset is None:
            unreal.log_error("MARKER: import failed for %s" % name)
            continue

        asset.set_editor_property("srgb", srgb)
        asset.set_editor_property("compression_settings", compression)
        if name == "T_Asphalt_Normal":
            # The vendor ships an OpenGL-convention normal map. UE expects DirectX, so
            # the green channel has to be inverted or every lit surface reads as though
            # it is lit from the opposite side - wrong in a way that looks plausible.
            asset.set_editor_property("flip_green_channel", True)
        unreal.EditorAssetLibrary.save_asset("%s/%s" % (TEX_DIR, name))
        imported[name] = asset
        unreal.log("MARKER: imported %s srgb=%s" % (name, srgb))
    return imported


def build_material(textures):
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset("M_RoadSurface", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    lib = unreal.MaterialEditingLibrary

    # --- UV0: world-aligned asphalt -------------------------------------------------
    tex_coord0 = lib.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -1200, 0)
    tex_coord0.set_editor_property("coordinate_index", 0)

    albedo = lib.create_material_expression(material, unreal.MaterialExpressionTextureSample, -900, -200)
    albedo.texture = textures["T_Asphalt_Albedo"]
    lib.connect_material_expressions(tex_coord0, "", albedo, "UVs")

    normal = lib.create_material_expression(material, unreal.MaterialExpressionTextureSample, -900, 100)
    normal.texture = textures["T_Asphalt_Normal"]
    normal.sampler_type = unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL
    lib.connect_material_expressions(tex_coord0, "", normal, "UVs")

    rough = lib.create_material_expression(material, unreal.MaterialExpressionTextureSample, -900, 400)
    rough.texture = textures["T_Asphalt_Roughness"]
    rough.sampler_type = unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GREYSCALE
    lib.connect_material_expressions(tex_coord0, "", rough, "UVs")

    # --- UV1: markings ---------------------------------------------------------------
    # UV1.X is lateral offset in uu, UV1.Y is distance along the centreline in uu.
    tex_coord1 = lib.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -1200, 700)
    tex_coord1.set_editor_property("coordinate_index", 1)

    lateral = lib.create_material_expression(material, unreal.MaterialExpressionComponentMask, -1000, 700)
    lateral.set_editor_property("r", True)
    lateral.set_editor_property("g", False)
    lib.connect_material_expressions(tex_coord1, "", lateral, "Input")

    centre_width = lib.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -1000, 850)
    centre_width.parameter_name = "CentrelineWidth"
    centre_width.default_value = 15.0

    # |lateral| < CentrelineWidth  ->  on the centreline
    abs_lateral = lib.create_material_expression(material, unreal.MaterialExpressionAbs, -820, 700)
    lib.connect_material_expressions(lateral, "", abs_lateral, "Input")

    centre_mask = lib.create_material_expression(material, unreal.MaterialExpressionIf, -640, 700)
    lib.connect_material_expressions(abs_lateral, "", centre_mask, "A")
    lib.connect_material_expressions(centre_width, "", centre_mask, "B")
    centre_mask.set_editor_property("const_a_less_than_b", 1.0)
    centre_mask.set_editor_property("const_a_equals_b", 0.0)
    centre_mask.set_editor_property("const_a_greater_than_b", 0.0)

    # --- junction blend fades the markings out --------------------------------------
    vertex_colour = lib.create_material_expression(material, unreal.MaterialExpressionVertexColor, -640, 1000)

    fade = lib.create_material_expression(material, unreal.MaterialExpressionOneMinus, -460, 1000)
    lib.connect_material_expressions(vertex_colour, "G", fade, "Input")

    marking_amount = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -300, 850)
    lib.connect_material_expressions(centre_mask, "", marking_amount, "A")
    lib.connect_material_expressions(fade, "", marking_amount, "B")

    marking_colour = lib.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -300, 1150)
    marking_colour.parameter_name = "MarkingColor"
    marking_colour.default_value = unreal.LinearColor(0.85, 0.72, 0.05, 1.0)

    base_colour = lib.create_material_expression(material, unreal.MaterialExpressionLinearInterpolate, -100, 0)
    lib.connect_material_expressions(albedo, "RGB", base_colour, "A")
    lib.connect_material_expressions(marking_colour, "", base_colour, "B")
    lib.connect_material_expressions(marking_amount, "", base_colour, "Alpha")

    # --- parameters the mesh scale depends on ---------------------------------------
    # Declared so C++ can drive them even though the world-aligned UVs already carry the
    # scale; a material instance is how slice 2b-ii and later tune this without a rebuild.
    texels = lib.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -1200, 1300)
    texels.parameter_name = "TexelsPerUnit"
    texels.default_value = 512.0

    for name, default in (
        ("EdgeLineInset", 40.0),
        ("EdgeLineWidth", 10.0),
        ("DashLength", 300.0),
        ("DashGap", 300.0),
    ):
        node = lib.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -1200, 1400)
        node.parameter_name = name
        node.default_value = default

    lib.connect_material_property(base_colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    lib.connect_material_property(normal, "RGB", unreal.MaterialProperty.MP_NORMAL)
    lib.connect_material_property(rough, "R", unreal.MaterialProperty.MP_ROUGHNESS)

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset("%s/M_RoadSurface" % MAT_DIR)

    names = [p.parameter_info.name for p in lib.get_scalar_parameter_names(material)] \
        if hasattr(lib, "get_scalar_parameter_names") else []
    unreal.log("MARKER: material saved at %s/M_RoadSurface" % MAT_DIR)
    unreal.log("MARKER: scalar parameters = %s" % names)
    return material


textures = import_textures()
if len(textures) == len(MAPS):
    build_material(textures)
    unreal.log("MARKER: done")
else:
    unreal.log_error("MARKER: aborted, %d of %d textures imported" % (len(textures), len(MAPS)))
```

- [ ] **Step 2: Run it**

```powershell
& "D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\repos\AirportMgr2\AirportMgr.uproject" `
  -run=pythonscript -script="C:\repos\AirportMgr2\Tools\Python\build_road_material.py" `
  -unattended -nosplash -nopause
Select-String -Path C:\repos\AirportMgr2\Saved\Logs\AirportMgr.log -Pattern "MARKER:"
```

Expected markers: four `imported` lines, `material saved`, and `done`.

**This is the step most likely to need iteration.** The Python material API is unforgiving about expression input names and enum spellings, and an error here is a Python traceback in the log rather than a compile failure. If a `connect_material_expressions` call fails, read the traceback for the offending input name and check it against the node's actual inputs — do not guess a different name and re-run blind.

- [ ] **Step 3: Verify the assets exist**

```powershell
Get-ChildItem -Recurse C:\repos\AirportMgr2\Content\RoadNet | Select-Object -ExpandProperty Name
```

Expected: `T_Asphalt_Albedo.uasset`, `T_Asphalt_Normal.uasset`, `T_Asphalt_Roughness.uasset`, `T_Asphalt_AO.uasset`, `M_RoadSurface.uasset`.

- [ ] **Step 4: Remove the spike artifact and commit**

```bash
rm Tools/Python/probe_material_api.py
git add Tools/Python Content/RoadNet
git commit -m "feat(roadnet): import asphalt textures and author M_RoadSurface"
```

---

### Task 4: Put the material on the surface

The component currently gets `GetDefaultMaterial(MD_Surface)` plus a constant colour override — a placeholder that exists only because that default is `WorldGridMaterial`, the same checker as the template floor.

**Files:**
- Modify: `Plugins/RoadNet/Source/RoadNet/Public/Present/RoadNetworkActor.h`
- Modify: `Plugins/RoadNet/Source/RoadNet/Private/Present/RoadNetworkActor.cpp`

**Interfaces:**
- Consumes: `/Game/RoadNet/Materials/M_RoadSurface` from Task 3.
- Produces: `ARoadNetworkActor::SurfaceMaterial`, an `EditAnywhere` `TObjectPtr<UMaterialInterface>`.

- [ ] **Step 1: Add the property**

In `Public/Present/RoadNetworkActor.h`, after `Profile`:

```cpp
	/**
	 * Material for the road surface. Defaults to M_RoadSurface, which reads UV0 for
	 * asphalt and UV1 for markings. Left null, the surface falls back to the engine
	 * default - which is WorldGridMaterial, the same world-aligned checker the template
	 * floor uses, so the road becomes very hard to distinguish from the ground.
	 */
	UPROPERTY(EditAnywhere, Category = "RoadNet")
	TObjectPtr<UMaterialInterface> SurfaceMaterial;
```

and forward-declare `class UMaterialInterface;` at the top.

- [ ] **Step 2: Default it in the constructor**

In `ARoadNetworkActor::ARoadNetworkActor()`, after the component setup:

```cpp
	// Resolved by path rather than assigned in a Blueprint so a freshly placed actor
	// renders as asphalt with no setup at all.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> RoadMaterial(
		TEXT("/Game/RoadNet/Materials/M_RoadSurface"));
	if (RoadMaterial.Succeeded())
	{
		SurfaceMaterial = RoadMaterial.Object;
	}
```

with `#include "UObject/ConstructorHelpers.h"`.

- [ ] **Step 3: Use it in the sink**

`FDynamicMeshSink` does not know the actor, so pass the material in. Change its constructor and field:

```cpp
	explicit FDynamicMeshSink(UDynamicMeshComponent* InComponent, UMaterialInterface* InMaterial = nullptr)
		: Component(InComponent), Material(InMaterial) {}
```

```cpp
	UMaterialInterface* Material = nullptr;
```

and in `Accept`, replace the whole `GetNumMaterials() == 0` block with:

```cpp
	// A UDynamicMeshComponent has NO surface-material fallback: GetNumMaterials() is just
	// BaseMaterials.Num(), so a component nobody called SetMaterial on reports zero
	// material slots and the renderer has no section to draw - present, bounded,
	// registered, visible, and invisible.
	if (Material != nullptr)
	{
		Component->SetMaterial(0, Material);
		Component->SetColorOverrideMode(EDynamicMeshComponentColorOverrideMode::None);
	}
	else if (Component->GetNumMaterials() == 0)
	{
		Component->SetMaterial(0, UMaterial::GetDefaultMaterial(MD_Surface));
		Component->SetColorOverrideMode(EDynamicMeshComponentColorOverrideMode::Constant);
		Component->SetConstantOverrideColor(FColor(40, 40, 45));
	}
```

Update both construction sites to pass the material: `FDynamicMeshSink Sink(MeshComponent, SurfaceMaterial);` in `RebuildMesh`, and in the gallery `FDynamicMeshSink Sink(MeshComponent, nullptr);` — the gallery keeps the placeholder until its own material is chosen.

- [ ] **Step 4: Build and run — expect PASS**

```powershell
& "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
  -project="C:\repos\AirportMgr2\AirportMgr.uproject" -waitmutex
& "C:\repos\AirportMgr2\Tools\Run-RoadNetTests.ps1"
```

Expected: 14 tests, 0 failed. No new tests here — this is asset wiring whose behaviour is verified visually in Step 6.

- [ ] **Step 5: Commit**

```bash
git add Plugins/RoadNet
git commit -m "feat(roadnet): render the surface with M_RoadSurface"
```

- [ ] **Step 6: Visual verification — the 2b-i exit criterion**

This step is for a human. Do not attempt to drive the editor.

1. Open the project and a level containing an `ARoadNetworkActor`.
2. Play, and draw a road with two or three clicks.
3. Confirm:
   - the surface is **asphalt**, not a flat colour and not the floor's checker;
   - a **yellow centreline** runs down the middle of each segment;
   - the centreline **fades out** approaching a junction rather than stopping abruptly;
   - the asphalt texture is **continuous** across every segment/junction boundary, with no visible change of scale or offset at the seam — this is UV0's world alignment working, and it is the one thing 2b-i exists to demonstrate.
4. Check the Output Log for `LogRoadMesh: Warning` — rejected triangles mean holes.

---

## Self-Review

**Spec coverage (2b-i rows only):**

| Design spec section | Covered by |
|---|---|
| §3 buffers gain UV0/UV1/Colors | Task 1 |
| §5 ordering rule, segments own shared UV1 | Task 1 Steps 6–8, tested Step 1 |
| §5 junction blend in vertex colour G | Task 1 Step 7 |
| §7 material contract, texture import settings | Task 3 |
| §9 UV0 purity, ordering, overlay population | Tasks 1–2 tests |

Deliberately **not** covered, deferred to 2b-ii: §2 lateral band subdivision, §4 `CutLinePoint`, §6 shoulder fade and the inset ring, §1's K3 zero-area drop. Ghost material, turn-path markings and curved-segment sampling remain later slices per §8.

**Known gaps to watch during execution:**

1. `FProfileBand::Type` and `MaterialSlot` stay unused. 2b-i uses only the profile's half-widths.
2. Vertex colour `A` is written as 255 everywhere. It becomes the ground blend in 2b-ii; until then the material must not sample it as opacity or the whole road disappears.
3. The junction's UV1 is `(0, 0)` for arc samples and the apex. Nothing reads it while junction blend fades markings out, but a future material that samples UV1 inside a junction will find it meaningless.
4. Task 3 is the riskiest step in the plan. It is the only one whose failure mode is a Python traceback rather than a compile error, and the material API's input names are easy to get wrong.
5. `ConstructorHelpers::FObjectFinder` runs at CDO construction. If Task 3's asset is missing, `SurfaceMaterial` is null and the placeholder path takes over — which is the intended degradation, but it means a missing asset looks like the old behaviour rather than an error.
