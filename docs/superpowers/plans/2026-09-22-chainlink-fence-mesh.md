# Chainlink Fence Mesh Implementation Plan (chainlink fence, PR 2 of 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the grey-box fence panels round every drawn plot with the chainlink kit: 8-gon line posts, heavy corner and gate posts, and a masked chainlink fabric strip. The gate opening equals the yard solver's truck corridor.

**Architecture:** `Solve/FenceLayout` (CoreMinimal only) turns an outline and gate into posts and fabric spans, tested with no world. `UPlotPresenter` turns the layout into instances in two HISMs on `AAirsideBuildingsActor`, plus one `FRoadMeshBuffers` strip pushed through the existing `FDynamicMeshSink`. Content (posts, texture, material) is imported and authored by one headless script. It resolves through `UAirsideSettings::ResolveFenceKit()`, and null falls back to grey boxes.

**Tech Stack:** UE 5.8.2 C++, Airside plugin, UE automation tests, headless Python commandlet (Interchange FBX import, MaterialEditingLibrary).

**Spec:** `docs/superpowers/specs/2026-09-22-chainlink-fence-design.md` (PR 2 section). Asset contract: `C:\repos\AirportMgr2Models\accessories\chainlink\README.md`.

## Global Constraints

- The editor must be CLOSED to build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex`
- A new test .cpp needs TWO builds (the first says Succeeded without compiling it).
- Tests: `./Tools/Run-AirsideTests.ps1 -Filter <prefix>`. Read the `N test(s) run, N failed, N crashed` line; never the exit code. `Check-Architecture.ps1` runs first inside it.
- In AirsideTests, never name a local `TestWorld(` with arguments. The lint reads any `Test*(` as an assertion. The default-constructed `FAirsideTestWorld TestWorld;` is fine.
- The tests module is a UNITY build. Anonymous-namespace helpers need names unique across the module (prefix them `Fence…`).
- `Solve/` includes `CoreMinimal.h` and other `Solve/` headers only. No `Algo/`.
- Figures fixed by the asset contract (README): post height **245 uu**; fabric height **240 uu** (the texture's V range); texture tile **240 uu** of run; line post **Ø6 uu**, heavy post **Ø9 uu**; spacing **250 uu** nominal; fabric **3 uu** off the post centreline, on the outside face.
- Gate width = `PlotYard::GateCorridorUu` (620 uu), centred on the entity pose.
- Content folder: `/Game/Environment/Fence/`.
- Log category `LogAirside`. Commit messages are concise, with no Co-Authored-By trailer.
- Branch `feature/chainlink-fence-mesh` (created from main `98bdefc`).

## Decisions made while planning (spec to be amended in Task 6)

- **`FabricHeightUu` is not in `FenceLayout::FSpec`.** Solve is 2D; height is the presenter's. The layout reports `GateCentre`, not an edge index, because the solver may reverse the winding.
- **Fabric material fallback is the sink's own** (`UMaterial::GetDefaultMaterial(MD_Surface)`, the grid checker), not "the grey-box material". It's the same visible-but-wrong fallback the road surface uses, with no second path.
- **Content arrives by a new `build_fence_content.py`, not `import_models.py`.** `airside_import` is a glTF vehicle pipeline (wheel nodes, rigs, axle checks). These are two FBX static meshes plus textures.
- **Posts get no collision** (`NoCollision`), matching the modules. The FBX's `UCX_` hull is imported but unused; every pick in this game is maths against the road plane.
- **The fabric's vertical quads bypass `FRoadMeshBuffers::AppendTriangleUp`**, whose sliver guard measures XY area and would drop every vertical triangle. Indices are appended directly.
- **Clear opening is 611 uu**, not 620. Gate posts (Ø9) stand centred on the cut lines. The corridor is 620 wide and the truck ~250, so it clears either way.

## File Structure

| File | Responsibility |
|---|---|
| Create `Plugins/Airside/Source/Airside/Public/Solve/FenceLayout.h` / `Private/Solve/FenceLayout.cpp` | outline + gate → posts + spans |
| Create `Plugins/Airside/Source/AirsideTests/Private/FenceLayoutTest.cpp` | Solve tests |
| Create `Plugins/Airside/Source/Airside/Public/Content/FenceKit.h` | `FFenceKit` (resolved meshes + material) |
| Modify `Public/Content/AirsideContent.h`, `Public/Content/AirsideSettings.h`, `Private/Content/AirsideSettings.cpp` | three soft pointers + `ResolveFenceKit` |
| Modify `Public/Present/PlotPresenter.h` / `Private/Present/PlotPresenter.cpp` | fence from the layout |
| Modify `Public/Present/AirsideBuildingsActor.h` / `Private/Present/AirsideBuildingsActor.cpp` | three fence components |
| Create `Plugins/Airside/Source/AirsideTests/Private/PlotFenceTest.cpp` | composition tests |
| Modify `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp` | fence-panel assertions → fence counts |
| Create `Tools/Python/build_fence_content.py`; modify `Tools/Python/import_models.py` docstring | content |
| Modify `Plugins/Airside/Source/AirsideTests/Private/AirsideContentTest.cpp` | the authored kit resolves |

---

### Task 0: Baseline

- [ ] **Step 1:** `./Tools/Run-AirsideTests.ps1`. Record the run line in the scratchpad `baseline2.txt`. Expected `724 test(s) run, 0 failed, 0 crashed` (main after #263).
- [ ] **Step 2:** Record `UE_LOG(` and comment-line counts for `PlotPresenter.h/.cpp`, `AirsideBuildingsActor.h/.cpp`, `PlotPresenterTest.cpp`, `AirsideSettings.h/.cpp` and `AirsideContent.h`. Use the same `grep -c` loop as PR 1's Task 0.

---

### Task 1: `Solve/FenceLayout`, tests first

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Solve/FenceLayout.h`
- Create: `Plugins/Airside/Source/Airside/Private/Solve/FenceLayout.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/FenceLayoutTest.cpp`

**Interfaces:**
- Consumes: `RoadGeom::PolygonArea`, `RoadGeom::LineIntersect`, `RoadGeom::PointInPolygon`, `FRay2D` (`Solve/RoadGeom.h`).
- Produces:
  - `enum class FenceLayout::EPostKind : uint8 { Line, Corner, Gate }`
  - `struct FenceLayout::FSpec { double SpacingUu = 250.0; double FaceOffsetUu = 3.0; double TileUu = 240.0; double GateWidthUu = 0.0; }`
  - `struct FenceLayout::FPost { FVector2D Position; double YawRad; EPostKind Kind; }`
  - `struct FenceLayout::FSpan { FVector2D A, B; double U0, U1; }`
  - `struct FenceLayout::FLayout { TArray<FPost> Posts; TArray<FSpan> Spans; bool bHasGate; FVector2D GateCentre; int32 CountOf(EPostKind) const; }`
  - `FenceLayout::FLayout FenceLayout::Solve(TArrayView<const FVector2D> Outline, const FVector2D& Gate, const FSpec& Spec)`

- [ ] **Step 1: Write the header**

Create `FenceLayout.h`:

```cpp
#pragma once

#include "CoreMinimal.h"

/**
 * Where a plot's fence posts stand and where its fabric hangs.
 *
 * Dependency-free, like every Solve/ header: an outline and a gate in, posts and spans out,
 * testable with no world. The presenter turns this into instances and a mesh strip; nothing
 * here knows what a post looks like.
 *
 * WHY NOT A FIXED BAY LENGTH, which is what the grey-box fence did (floor(L / 250) panels and
 * the remainder dropped). A plot edge is whatever length the player dragged, so a fixed bay
 * either leaves a gap at the corner or overhangs it. Here every run between two FIXED posts
 * (corners, gate posts) is divided into round(L / Spacing) equal bays: the posts are rigid
 * and evenly spaced, and only the fabric's U stretches to take up the difference - see the
 * asset README's "Spacing and edge subdivision".
 */
namespace FenceLayout
{
	/** What a post is for, which decides its mesh. Corner and Gate both take the heavy post. */
	enum class EPostKind : uint8
	{
		Line,
		Corner,
		Gate,
	};

	/** The numbers the layout needs. Defaults are the asset contract's; see the README. */
	struct FSpec
	{
		/** Nominal post pitch, uu. Each run's actual pitch is its length / round(length / this). */
		double SpacingUu = 250.0;

		/**
		 * How far the fabric hangs OUTSIDE the post centreline, uu - on one face the way real
		 * fabric is tied on, not through the middle of the posts.
		 */
		double FaceOffsetUu = 3.0;

		/** Run per texture tile, uu. U is distance along the edge divided by this. */
		double TileUu = 240.0;

		/**
		 * The gate opening, post centre to post centre, uu. Zero means no gate. The caller passes
		 * PlotYard::GateCorridorUu so the gap and the lane the yard keeps clear are one number.
		 */
		double GateWidthUu = 0.0;
	};

	/** One post, standing on the outline. */
	struct FPost
	{
		FVector2D Position = FVector2D::ZeroVector;

		/** Heading in radians: along the edge for Line and Gate, the outward bisector for Corner. */
		double YawRad = 0.0;

		EPostKind Kind = EPostKind::Line;
	};

	/** One bay of fabric between two adjacent posts, already offset outward. */
	struct FSpan
	{
		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;

		/** Texture U at A and B: distance along the edge / TileUu, continuous along the edge. */
		double U0 = 0.0;
		double U1 = 0.0;
	};

	struct FLayout
	{
		TArray<FPost> Posts;
		TArray<FSpan> Spans;

		/**
		 * False when the gate's edge is too short to hold GateWidthUu plus a bay either side, or
		 * no gate was asked for. A fence with no gate is a depot no truck can leave, and it looks
		 * completely correct from every angle - so the presenter warns rather than draws quietly.
		 */
		bool bHasGate = false;

		/** Midpoint of the gate opening; the requested gate unless it slid clear of a corner. */
		FVector2D GateCentre = FVector2D::ZeroVector;

		/** Posts of one kind. */
		AIRSIDE_API int32 CountOf(EPostKind Kind) const;
	};

	/**
	 * The fence round Outline, gated on the edge nearest Gate.
	 *
	 * EITHER WINDING. The outline is read counter-clockwise internally (from its signed area),
	 * so a plot stored clockwise hangs its fabric outside too rather than inside. Consecutive
	 * vertices closer than 1 uu are merged. Fewer than three distinct vertices returns an empty
	 * layout.
	 */
	AIRSIDE_API FLayout Solve(TArrayView<const FVector2D> Outline, const FVector2D& Gate,
		const FSpec& Spec);
}
```

- [ ] **Step 2: Write the failing tests**

Create `FenceLayoutTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/FenceLayout.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** An axis-aligned rectangle, CCW, south edge (y = 0) first. */
	TArray<FVector2D> FenceRect(double Width, double Depth)
	{
		return { FVector2D(0.0, 0.0), FVector2D(Width, 0.0),
		         FVector2D(Width, Depth), FVector2D(0.0, Depth) };
	}

	/** The asset contract's spec with the depot gate. */
	FenceLayout::FSpec FenceGatedSpec()
	{
		FenceLayout::FSpec Spec;
		Spec.GateWidthUu = PlotYard::GateCorridorUu;
		return Spec;
	}

	/** Posts of one kind on the line y = 0 (the south edge), sorted by X. */
	TArray<double> FenceSouthXs(const FenceLayout::FLayout& Layout, FenceLayout::EPostKind Kind)
	{
		TArray<double> Xs;
		for (const FenceLayout::FPost& Post : Layout.Posts)
		{
			if (Post.Kind == Kind && FMath::IsNearlyZero(Post.Position.Y, 1e-9))
			{
				Xs.Add(Post.Position.X);
			}
		}
		Xs.Sort();
		return Xs;
	}
}

/**
 * A 10 m square: a heavy post on every corner, three line posts per edge, sixteen bays.
 *
 * 1000 / 250 = 4 bays exactly, so this is the case where nothing stretches - every count is
 * forced by the spacing alone, and a wrong one means the subdivision rule is wrong.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutSquareTest,
	"Airside.Solve.FenceLayout.Square",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutSquareTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = FenceRect(1000.0, 1000.0);
	const FenceLayout::FLayout Layout =
		FenceLayout::Solve(Outline, FVector2D(500.0, 0.0), FenceLayout::FSpec());

	TestEqual(TEXT("a heavy post on each of the four corners"),
		Layout.CountOf(FenceLayout::EPostKind::Corner), 4);
	TestEqual(TEXT("three line posts on each of four 10 m edges"),
		Layout.CountOf(FenceLayout::EPostKind::Line), 12);
	TestEqual(TEXT("no gate asked for, none made"), Layout.CountOf(FenceLayout::EPostKind::Gate), 0);
	TestFalse(TEXT("and it says so"), Layout.bHasGate);
	TestEqual(TEXT("four bays per edge"), Layout.Spans.Num(), 16);

	// ON THE VERTICES EXACTLY, not near them: a corner post a hair off the corner is a post the
	// fabric of the next edge does not reach.
	for (const FVector2D& Vertex : Outline)
	{
		bool bFound = false;
		for (const FenceLayout::FPost& Post : Layout.Posts)
		{
			bFound |= Post.Kind == FenceLayout::EPostKind::Corner && Post.Position == Vertex;
		}
		TestTrue(*FString::Printf(TEXT("a corner post stands on (%.0f, %.0f)"), Vertex.X, Vertex.Y), bFound);
	}

	// THE OUTWARD BISECTOR. At (0,0) the edges run in from the north and out to the east, so
	// outward is south-west: -135 degrees.
	for (const FenceLayout::FPost& Post : Layout.Posts)
	{
		if (Post.Kind == FenceLayout::EPostKind::Corner && Post.Position == FVector2D(0.0, 0.0))
		{
			TestEqual(TEXT("the corner post at the origin faces out along the bisector"),
				FMath::RadiansToDegrees(Post.YawRad), -135.0, 1e-9);
		}
	}

	const TArray<double> Line = FenceSouthXs(Layout, FenceLayout::EPostKind::Line);
	if (TestEqual(TEXT("three line posts on the south edge"), Line.Num(), 3))
	{
		TestEqual(TEXT("at 2.5 m"), Line[0], 250.0, 1e-9);
		TestEqual(TEXT("at 5 m"), Line[1], 500.0, 1e-9);
		TestEqual(TEXT("at 7.5 m"), Line[2], 750.0, 1e-9);
	}
	return true;
}

/**
 * An 11 m edge takes four bays of 2.75 m, not four of 2.5 m and a 1 m remainder.
 *
 * THE GREY BOX DROPPED THE REMAINDER, which read as a gap at every corner of every plot the
 * player did not happen to drag to a multiple of 2.5 m. The posts are rigid; only the
 * fabric's U takes up the 10%.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutStretchesEvenlyTest,
	"Airside.Solve.FenceLayout.StretchesEvenly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutStretchesEvenlyTest::RunTest(const FString& Parameters)
{
	const FenceLayout::FLayout Layout = FenceLayout::Solve(
		FenceRect(1100.0, 1000.0), FVector2D(550.0, 0.0), FenceLayout::FSpec());

	const TArray<double> Line = FenceSouthXs(Layout, FenceLayout::EPostKind::Line);
	if (!TestEqual(TEXT("round(11 / 2.5) = 4 bays, so three line posts"), Line.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("every 2.75 m"), Line[0], 275.0, 1e-9);
	TestEqual(TEXT("every 2.75 m"), Line[1], 550.0, 1e-9);
	TestEqual(TEXT("every 2.75 m"), Line[2], 825.0, 1e-9);

	// U IS DISTANCE ALONG THE EDGE, so a stretched bay stretches the texture rather than
	// restarting it: the south edge's last bay ends at 1100 / 240.
	double MaxU = 0.0;
	for (const FenceLayout::FSpan& Span : Layout.Spans)
	{
		if (Span.A.Y < 0.0 && Span.B.Y < 0.0)
		{
			MaxU = FMath::Max(MaxU, Span.U1);
		}
	}
	TestEqual(TEXT("U runs to the edge length over the tile"), MaxU, 1100.0 / 240.0, 1e-9);
	return true;
}

/**
 * The gate is the truck corridor's width, centred on the pose, with a heavy post each side.
 *
 * THE GAP AND THE LANE ARE ONE NUMBER. PlotYard keeps GateCorridorUu clear from the gate
 * inward; an opening of any other width is a fence that disagrees with the yard about where
 * the truck drives.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutGateIsTheCorridorTest,
	"Airside.Solve.FenceLayout.GateIsTheCorridor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutGateIsTheCorridorTest::RunTest(const FString& Parameters)
{
	const FenceLayout::FLayout Layout =
		FenceLayout::Solve(FenceRect(2000.0, 2400.0), FVector2D(1000.0, 0.0), FenceGatedSpec());

	if (!TestTrue(TEXT("a 20 m frontage holds a gate"), Layout.bHasGate)) { return false; }

	const TArray<double> Gate = FenceSouthXs(Layout, FenceLayout::EPostKind::Gate);
	if (!TestEqual(TEXT("a gate post each side of the opening"), Gate.Num(), 2)) { return false; }
	TestEqual(TEXT("the opening is exactly the truck corridor"),
		Gate[1] - Gate[0], PlotYard::GateCorridorUu, 1e-9);
	TestEqual(TEXT("centred on the gate"), (Gate[0] + Gate[1]) * 0.5, 1000.0, 1e-9);
	TestTrue(TEXT("and reported where it is"), Layout.GateCentre.Equals(FVector2D(1000.0, 0.0), 1e-9));

	// NO FABRIC ACROSS THE OPENING. The whole point of the gate; a span over it would be a
	// closed gate drawn as an open one.
	for (const FenceLayout::FSpan& Span : Layout.Spans)
	{
		const FVector2D Mid = (Span.A + Span.B) * 0.5;
		TestFalse(TEXT("no fabric hangs across the gate"),
			Mid.Y < 0.0 && Mid.X > Gate[0] && Mid.X < Gate[1]);
	}
	return true;
}

/**
 * A gate asked for near a corner slides along the edge until a full bay separates it from
 * the corner - it never swallows the corner post.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutGateSlidesClearOfCornerTest,
	"Airside.Solve.FenceLayout.GateSlidesClearOfCorner",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutGateSlidesClearOfCornerTest::RunTest(const FString& Parameters)
{
	const FenceLayout::FLayout Layout =
		FenceLayout::Solve(FenceRect(2000.0, 2400.0), FVector2D(100.0, 0.0), FenceGatedSpec());

	if (!TestTrue(TEXT("still gated"), Layout.bHasGate)) { return false; }
	const TArray<double> Gate = FenceSouthXs(Layout, FenceLayout::EPostKind::Gate);
	if (!TestEqual(TEXT("two gate posts"), Gate.Num(), 2)) { return false; }
	TestEqual(TEXT("slid to one spacing clear of the corner"), Gate[0], 250.0, 1e-9);
	TestEqual(TEXT("and still the corridor's width"), Gate[1] - Gate[0], PlotYard::GateCorridorUu, 1e-9);
	return true;
}

/**
 * An edge too short for the gate plus a bay either side gets no gate, and says so.
 *
 * 800 uu < 620 + 2 x 250. Squeezing the gate in anyway would stand a gate post on top of a
 * corner post; refusing is what lets the presenter warn instead.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutShortEdgeHasNoGateTest,
	"Airside.Solve.FenceLayout.ShortEdgeHasNoGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutShortEdgeHasNoGateTest::RunTest(const FString& Parameters)
{
	const FenceLayout::FLayout Layout =
		FenceLayout::Solve(FenceRect(800.0, 2400.0), FVector2D(400.0, 0.0), FenceGatedSpec());

	TestFalse(TEXT("no gate on an 8 m frontage"), Layout.bHasGate);
	TestEqual(TEXT("so no gate posts"), Layout.CountOf(FenceLayout::EPostKind::Gate), 0);
	TestTrue(TEXT("but the fence still stands"), Layout.Spans.Num() > 0);
	return true;
}

/**
 * Every bay of fabric hangs outside the posts, FaceOffsetUu off the line, and the fabric is
 * continuous round each corner.
 *
 * MEASURED, NOT NAMED: the offset is taken as the distance from each span's midpoint to the
 * outline, and continuity as bitwise equality of the two spans that meet at a corner - a
 * mitre computed twice with different arguments would leave a crack a hair wide.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutFabricHangsOutsideTest,
	"Airside.Solve.FenceLayout.FabricHangsOutside",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutFabricHangsOutsideTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = FenceRect(2000.0, 2400.0);
	const FenceLayout::FLayout Layout =
		FenceLayout::Solve(Outline, FVector2D(1000.0, 0.0), FenceGatedSpec());

	for (const FenceLayout::FSpan& Span : Layout.Spans)
	{
		const FVector2D Mid = (Span.A + Span.B) * 0.5;
		TestFalse(TEXT("the fabric is outside the plot"), RoadGeom::PointInPolygon(Outline, Mid));

		// Distance to the rectangle's nearest side.
		const double ToSide = FMath::Min(
			FMath::Min(FMath::Abs(Mid.X), FMath::Abs(Mid.X - 2000.0)),
			FMath::Min(FMath::Abs(Mid.Y), FMath::Abs(Mid.Y - 2400.0)));
		TestEqual(TEXT("hung 3 uu off the post line"), ToSide, 3.0, 1e-9);
	}

	// CONTINUOUS ROUND THE WHOLE RING, corners included: every bay ends exactly - bitwise -
	// where another begins, except the one bay that ends at the gate. Compared with == on
	// purpose; a tolerance here would pass a crack.
	int32 Continued = 0;
	for (const FenceLayout::FSpan& Span : Layout.Spans)
	{
		for (const FenceLayout::FSpan& Next : Layout.Spans)
		{
			if (Next.A == Span.B)
			{
				++Continued;
				break;
			}
		}
	}
	TestEqual(TEXT("the fabric is one unbroken ring but for the gate"),
		Continued, Layout.Spans.Num() - 1);
	return true;
}

/**
 * A plot stored clockwise fences exactly like the same plot stored counter-clockwise.
 *
 * The facade winds pads counter-clockwise today, but a solver correct only for the caller that
 * happens to get the winding right is how the pad once faced DOWN (RoadEditFacadeSurfaces.cpp).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFenceLayoutWindingDoesNotMatterTest,
	"Airside.Solve.FenceLayout.WindingDoesNotMatter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFenceLayoutWindingDoesNotMatterTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Ccw = FenceRect(2000.0, 2400.0);
	const TArray<FVector2D> Cw = { Ccw[0], Ccw[3], Ccw[2], Ccw[1] };

	const FenceLayout::FLayout A = FenceLayout::Solve(Ccw, FVector2D(1000.0, 0.0), FenceGatedSpec());
	const FenceLayout::FLayout B = FenceLayout::Solve(Cw, FVector2D(1000.0, 0.0), FenceGatedSpec());

	if (!TestEqual(TEXT("the same number of posts"), A.Posts.Num(), B.Posts.Num())) { return false; }
	TestEqual(TEXT("the same number of bays"), A.Spans.Num(), B.Spans.Num());

	// AS A SET: the two start from different vertices, so the ORDER legitimately differs.
	for (const FenceLayout::FPost& Post : A.Posts)
	{
		bool bFound = false;
		for (const FenceLayout::FPost& Other : B.Posts)
		{
			bFound |= Other.Kind == Post.Kind && Other.Position.Equals(Post.Position, 1e-9);
		}
		TestTrue(TEXT("every post stands in the same place"), bFound);
	}
	for (const FenceLayout::FSpan& Span : B.Spans)
	{
		TestFalse(TEXT("and the clockwise plot's fabric is still outside"),
			RoadGeom::PointInPolygon(Ccw, (Span.A + Span.B) * 0.5));
	}
	return true;
}

#endif
```

- [ ] **Step 3: Write a stub so it compiles, then build twice and watch it fail**

Create `FenceLayout.cpp` with only:

```cpp
#include "Solve/FenceLayout.h"

int32 FenceLayout::FLayout::CountOf(EPostKind Kind) const
{
	int32 Count = 0;
	for (const FPost& Post : Posts)
	{
		Count += Post.Kind == Kind ? 1 : 0;
	}
	return Count;
}

FenceLayout::FLayout FenceLayout::Solve(TArrayView<const FVector2D> Outline, const FVector2D& Gate,
	const FSpec& Spec)
{
	return FLayout();
}
```

Build twice. Run `./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve.FenceLayout`.
Expected: `7 test(s) run, 7 failed, 0 crashed`.

- [ ] **Step 4: Implement**

Replace `Solve` in `FenceLayout.cpp` (keep `CountOf`). Add `#include "Solve/RoadGeom.h"` after the first include:

```cpp
namespace
{
	/** Closer than this, two consecutive vertices are one repeated point rather than an edge. */
	constexpr double FenceDegenerateEdgeUu = 1.0;

	/**
	 * A mitre further than this many offsets from its corner is a spike, not a corner - a
	 * near-hairpin angle. The span ends at its own offset point instead; the crack that leaves
	 * is at a corner no plot the facade accepts can have.
	 */
	constexpr double FenceMitreLimit = 10.0;

	/** One outline edge, counter-clockwise. */
	struct FFenceEdge
	{
		FVector2D A = FVector2D::ZeroVector;
		FVector2D Dir = FVector2D(1.0, 0.0);

		/** Right of travel: OUTSIDE for a counter-clockwise ring. */
		FVector2D Out = FVector2D(0.0, -1.0);

		double Length = 0.0;
	};

	/** A stretch of one edge between two fixed posts. */
	struct FFenceRun
	{
		double From = 0.0;
		double To = 0.0;

		/** The run starts at the edge's first vertex, so its first bay mitres into the edge before. */
		bool bFromCorner = false;

		/** The run ends at the edge's last vertex, so its last bay mitres into the edge after. */
		bool bToCorner = false;
	};

	double FenceHeading(const FVector2D& V)
	{
		return FMath::Atan2(V.Y, V.X);
	}

	/**
	 * Where In's offset line meets Next's - the corner the fabric turns at.
	 *
	 * ONE FUNCTION, CALLED WITH THE SAME ARGUMENTS BY BOTH SIDES: the last bay of edge i and the
	 * first bay of edge i+1 each ask for FenceMitre(Edges[i], Edges[i+1]), so the two endpoints
	 * are the same bits and the fabric has no crack at the corner.
	 */
	FVector2D FenceMitre(const FFenceEdge& In, const FFenceEdge& Next, double Offset)
	{
		const FVector2D Fallback = Next.A + Next.Out * Offset;

		FRay2D RayIn;
		RayIn.Origin = In.A + In.Out * Offset;
		RayIn.Dir = In.Dir;
		FRay2D RayNext;
		RayNext.Origin = Fallback;
		RayNext.Dir = Next.Dir;

		// HONOURS THE RETURN: LineIntersect leaves Hit unwritten when the lines are parallel
		// (a collinear vertex), and the fallback IS the right answer there.
		FVector2D Hit = Fallback;
		if (!RoadGeom::LineIntersect(RayIn, RayNext, Hit)
			|| FVector2D::Distance(Hit, Next.A) > FenceMitreLimit * Offset)
		{
			return Fallback;
		}
		return Hit;
	}
}

FenceLayout::FLayout FenceLayout::Solve(TArrayView<const FVector2D> Outline, const FVector2D& Gate,
	const FSpec& Spec)
{
	FLayout Layout;
	if (Spec.SpacingUu <= 0.0 || Spec.TileUu <= 0.0)
	{
		return Layout;
	}

	// DISTINCT VERTICES, COUNTER-CLOCKWISE. Read off the signed area rather than assumed, so
	// "outside" is right-of-travel whichever way the plot was stored.
	TArray<FVector2D> Ring;
	for (const FVector2D& Point : Outline)
	{
		if (Ring.Num() == 0 || FVector2D::Distance(Ring.Last(), Point) >= FenceDegenerateEdgeUu)
		{
			Ring.Add(Point);
		}
	}
	if (Ring.Num() >= 2 && FVector2D::Distance(Ring.Last(), Ring[0]) < FenceDegenerateEdgeUu)
	{
		Ring.Pop();
	}
	if (Ring.Num() < 3)
	{
		return Layout;
	}
	if (RoadGeom::PolygonArea(Ring) < 0.0)
	{
		// Reversed by hand: Solve/ takes nothing beyond CoreMinimal, and Algo/Reverse is not in it.
		TArray<FVector2D> Reversed;
		Reversed.Reserve(Ring.Num());
		for (int32 Index = Ring.Num() - 1; Index >= 0; --Index)
		{
			Reversed.Add(Ring[Index]);
		}
		Ring = MoveTemp(Reversed);
	}

	const int32 N = Ring.Num();
	TArray<FFenceEdge> Edges;
	Edges.SetNum(N);
	for (int32 I = 0; I < N; ++I)
	{
		FFenceEdge& Edge = Edges[I];
		Edge.A = Ring[I];
		const FVector2D Along = Ring[(I + 1) % N] - Ring[I];
		Edge.Length = Along.Size();
		Edge.Dir = Along / Edge.Length;
		Edge.Out = FVector2D(Edge.Dir.Y, -Edge.Dir.X);
	}

	// THE GATE'S EDGE is the one nearest the requested gate; the gate's station is its
	// projection onto that edge.
	int32 GateEdge = INDEX_NONE;
	double GateAt = 0.0;
	if (Spec.GateWidthUu > 0.0)
	{
		double Best = TNumericLimits<double>::Max();
		for (int32 I = 0; I < N; ++I)
		{
			const double Along = FMath::Clamp(
				FVector2D::DotProduct(Gate - Edges[I].A, Edges[I].Dir), 0.0, Edges[I].Length);
			const double Distance = FVector2D::Distance(Edges[I].A + Edges[I].Dir * Along, Gate);
			if (Distance < Best)
			{
				Best = Distance;
				GateEdge = I;
				GateAt = Along;
			}
		}

		// A FULL BAY EITHER SIDE, or no gate at all - see bHasGate.
		const double Half = Spec.GateWidthUu * 0.5;
		const double Margin = Spec.SpacingUu + Half;
		if (Edges[GateEdge].Length >= 2.0 * Margin)
		{
			GateAt = FMath::Clamp(GateAt, Margin, Edges[GateEdge].Length - Margin);
			Layout.bHasGate = true;
			Layout.GateCentre = Edges[GateEdge].A + Edges[GateEdge].Dir * GateAt;
		}
		else
		{
			GateEdge = INDEX_NONE;
		}
	}

	for (int32 I = 0; I < N; ++I)
	{
		const FFenceEdge& Edge = Edges[I];
		const FFenceEdge& Before = Edges[(I + N - 1) % N];
		const FFenceEdge& After = Edges[(I + 1) % N];

		// THE CORNER POST at this edge's first vertex, facing out along the bisector. A hairpin
		// (outward normals cancelling) has no bisector; it faces along the edge instead.
		FPost Corner;
		Corner.Position = Edge.A;
		Corner.Kind = EPostKind::Corner;
		const FVector2D Bisector = Before.Out + Edge.Out;
		Corner.YawRad = Bisector.SizeSquared() > UE_DOUBLE_SMALL_NUMBER
			? FenceHeading(Bisector) : FenceHeading(Edge.Dir);
		Layout.Posts.Add(Corner);

		TArray<FFenceRun, TInlineAllocator<2>> Runs;
		if (I == GateEdge)
		{
			const double Half = Spec.GateWidthUu * 0.5;
			Runs.Add({ 0.0, GateAt - Half, true, false });
			Runs.Add({ GateAt + Half, Edge.Length, false, true });
			for (const double At : { GateAt - Half, GateAt + Half })
			{
				FPost GatePost;
				GatePost.Position = Edge.A + Edge.Dir * At;
				GatePost.YawRad = FenceHeading(Edge.Dir);
				GatePost.Kind = EPostKind::Gate;
				Layout.Posts.Add(GatePost);
			}
		}
		else
		{
			Runs.Add({ 0.0, Edge.Length, true, true });
		}

		for (const FFenceRun& Run : Runs)
		{
			const double RunLength = Run.To - Run.From;
			const int32 Bays = FMath::Max(1, FMath::RoundToInt(RunLength / Spec.SpacingUu));
			const double Pitch = RunLength / Bays;

			for (int32 Bay = 0; Bay < Bays; ++Bay)
			{
				const double From = Run.From + Pitch * Bay;
				// THE LAST BAY ENDS ON Run.To ITSELF, not on From + Pitch, so rounding in the
				// division cannot leave the final post a hair short of the fixed one.
				const double To = Bay == Bays - 1 ? Run.To : Run.From + Pitch * (Bay + 1);

				if (Bay > 0)
				{
					FPost Line;
					Line.Position = Edge.A + Edge.Dir * From;
					Line.YawRad = FenceHeading(Edge.Dir);
					Line.Kind = EPostKind::Line;
					Layout.Posts.Add(Line);
				}

				FSpan Span;
				Span.A = Bay == 0 && Run.bFromCorner
					? FenceMitre(Before, Edge, Spec.FaceOffsetUu)
					: Edge.A + Edge.Dir * From + Edge.Out * Spec.FaceOffsetUu;
				Span.B = Bay == Bays - 1 && Run.bToCorner
					? FenceMitre(Edge, After, Spec.FaceOffsetUu)
					: Edge.A + Edge.Dir * To + Edge.Out * Spec.FaceOffsetUu;
				Span.U0 = From / Spec.TileUu;
				Span.U1 = To / Spec.TileUu;
				Layout.Spans.Add(Span);
			}
		}
	}

	return Layout;
}
```

- [ ] **Step 5: Build and run**

Build (once; the test file is already known). Run `-Filter Airside.Solve.FenceLayout`.
Expected: `7 test(s) run, 0 failed, 0 crashed`.

If `FabricHangsOutside`'s mitre count fails, print both endpoints near (-3,-3) and check that `FenceMitre(Before, Edge)` for edge 0 and `FenceMitre(Edge, After)` for edge 3 receive the SAME pair (`Edges[3], Edges[0]`). They must, by the loop above. Do not loosen the bitwise check.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Solve/FenceLayout.h Plugins/Airside/Source/Airside/Private/Solve/FenceLayout.cpp Plugins/Airside/Source/AirsideTests/Private/FenceLayoutTest.cpp
git commit -m "feat(solve): FenceLayout - posts, gate and mitred fabric spans from an outline"
```

---

### Task 2: The fence kit in content (C++ half)

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Content/FenceKit.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Content/AirsideContent.h` (after `VehicleMesh`, line ~277)
- Modify: `Plugins/Airside/Source/Airside/Public/Content/AirsideSettings.h` (after `ResolveVehicleMesh`, line ~197)
- Modify: `Plugins/Airside/Source/Airside/Private/Content/AirsideSettings.cpp` (after `ResolveVehicleMesh`, line ~264)

**Interfaces:**
- Produces: `struct FFenceKit { UStaticMesh* LinePost; UStaticMesh* HeavyPost; UMaterialInterface* Fabric; }` and `static FFenceKit UAirsideSettings::ResolveFenceKit();`, with UPROPERTYs `FenceLinePost`, `FenceHeavyPost`, `FenceFabricMaterial` on `UAirsideContent` (Python: `fence_line_post`, `fence_heavy_post`, `fence_fabric_material`).

- [ ] **Step 1: `FenceKit.h`**

```cpp
#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UStaticMesh;

/**
 * What UAirsideSettings::ResolveFenceKit resolved: the two post meshes and the fabric material.
 *
 * EACH MAY BE NULL, and the presenter falls back per member - an engine cube scaled to the
 * post, or the sink's default surface material for the fabric. A test run with no content
 * set draws a grey-box fence rather than nothing.
 *
 * RAW POINTERS, NOT A USTRUCT: resolved and consumed within one UPlotPresenter::RebuildFrom
 * call, with no garbage collection between the two, and the soft pointers on UAirsideContent
 * are what actually hold the assets.
 */
struct FFenceKit
{
	UStaticMesh* LinePost = nullptr;
	UStaticMesh* HeavyPost = nullptr;
	UMaterialInterface* Fabric = nullptr;
};
```

- [ ] **Step 2: The three content properties**

In `AirsideContent.h`, directly after the `VehicleMesh` UPROPERTY:

```cpp

	/**
	 * The chainlink fence's Ø60 mm line post. Base-centred, 245 cm tall, 1 unit = 1 cm once
	 * imported - see AirportMgr2Models/accessories/chainlink/README.md. Null falls back to a
	 * scaled engine cube. Authored by Tools/Python/build_fence_content.py.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Fence")
	TSoftObjectPtr<UStaticMesh> FenceLinePost;

	/** The Ø90 mm corner and gate post - heavier instead of braced (README "Not here"). */
	UPROPERTY(EditAnywhere, Category = "Airside|Fence")
	TSoftObjectPtr<UStaticMesh> FenceHeavyPost;

	/**
	 * The fabric: Masked, two-sided, chainlink.png with its alpha-coverage mips. Null falls back
	 * to the sink's default surface material - visible and wrong, which is the failure to prefer.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Fence")
	TSoftObjectPtr<UMaterialInterface> FenceFabricMaterial;
```

- [ ] **Step 3: The resolver**

In `AirsideSettings.h`, add `#include "Content/FenceKit.h"` beside its other includes. After the `ResolveVehicleMesh` declaration add:

```cpp

	/** The chainlink fence's meshes and material - the content defaults, each null if unset. */
	static FFenceKit ResolveFenceKit();
```

In `AirsideSettings.cpp`, after `ResolveVehicleMesh`:

```cpp

FFenceKit UAirsideSettings::ResolveFenceKit()
{
	FFenceKit Kit;
	const UAirsideContent* Content = GetContent();
	if (Content != nullptr)
	{
		Kit.LinePost = Content->FenceLinePost.LoadSynchronous();
		Kit.HeavyPost = Content->FenceHeavyPost.LoadSynchronous();
		Kit.Fabric = Content->FenceFabricMaterial.LoadSynchronous();
	}
	return Kit;
}
```

- [ ] **Step 4: Build.** Expected `Result: Succeeded`. There's no test yet: with no assets authored, the only possible assertion is "all null", which proves nothing. Task 5 adds the test once the assets exist.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Content Plugins/Airside/Source/Airside/Private/Content/AirsideSettings.cpp
git commit -m "feat(content): fence kit slots and ResolveFenceKit"
```

---

### Task 3: The presenter draws the fence from the layout

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/AirsideBuildingsActor.h` / `Private/Present/AirsideBuildingsActor.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/PlotFenceTest.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp`

**Interfaces:**
- Consumes: `FenceLayout::Solve` (Task 1), `FFenceKit` + `UAirsideSettings::ResolveFenceKit()` (Task 2), `FDynamicMeshSink(UDynamicMeshComponent*, UMaterialInterface*, bool bUseConstantVertexColour, const URoadMaterialSet*, bool bQuiet)`, `FRoadMeshBuffers` (`Build/RoadMeshSink.h`).
- Produces:
  - `struct FFenceTargets { UHierarchicalInstancedStaticMeshComponent* Posts; UHierarchicalInstancedStaticMeshComponent* HeavyPosts; UDynamicMeshComponent* Fabric; }` in `PlotPresenter.h`.
  - `void UPlotPresenter::Initialise(UInstancedStaticMeshComponent* InBoxes, UInstancedStaticMeshComponent* InGhosts, const FFenceTargets& InFence = FFenceTargets());`
  - `void UPlotPresenter::RebuildFrom(const URoadNetwork&, TArrayView<const PlotYard::FKitSpec>, const FFenceKit& Kit = FFenceKit());`
  - `int32 GetFencePostCount() const`, `int32 GetFenceSpanCount() const`. `GetGateGapCount()` now counts plots with a gate.
  - `AAirsideBuildingsActor::GetFencePostsForTest()`, `GetFenceHeavyPostsForTest()`, `GetFenceFabricForTest()`.

- [ ] **Step 1: Write the failing composition tests**

Create `PlotFenceTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/FenceLayout.h"
#include "Solve/PlotYard.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** The 20 x 24 m depot, gate mid-frontage. */
	const TArray<FVector2D>& FenceTestOutline()
	{
		static const TArray<FVector2D> Outline = { FVector2D(0.0, 0.0), FVector2D(2000.0, 0.0),
			FVector2D(2000.0, 2400.0), FVector2D(0.0, 2400.0) };
		return Outline;
	}

	void PlaceFenceTestDepot(ARoadNetworkActor* Road)
	{
		UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(1000.0, 0.0);
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = FenceTestOutline();
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		Road->Network->PlaceEntity(Placement);
	}

	/** The layout the presenter is meant to have drawn - the same Solve, the same spec. */
	FenceLayout::FLayout FenceTestExpected()
	{
		FenceLayout::FSpec Spec;
		Spec.GateWidthUu = PlotYard::GateCorridorUu;
		return FenceLayout::Solve(FenceTestOutline(), FVector2D(1000.0, 0.0), Spec);
	}
}

/**
 * A placed depot's fence reaches the components: every post an instance of the right mesh,
 * every bay two triangles of fabric.
 *
 * COMPOSITION LEVEL, per CLAUDE.md: every Airside.Solve.FenceLayout test passes against a
 * presenter that never calls Solve, or calls it and draws into a component nobody renders.
 * This counts what the COMPONENTS hold, against the layout recomputed here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotFenceReachesTheComponentsTest,
	"Airside.Present.PlotFenceReachesTheComponents",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotFenceReachesTheComponentsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	if (!TestNotNull(TEXT("a buildings actor"), TestWorld.Buildings)) { return false; }
	ARoadNetworkActor* Road = TestWorld.Actor;
	Road->ClearNetwork();
	PlaceFenceTestDepot(Road);
	Road->RebuildMesh();

	const FenceLayout::FLayout Expected = FenceTestExpected();
	const int32 Heavy = Expected.CountOf(FenceLayout::EPostKind::Corner)
		+ Expected.CountOf(FenceLayout::EPostKind::Gate);

	TestEqual(TEXT("every line post is an instance of the line-post mesh"),
		TestWorld.Buildings->GetFencePostsForTest()->GetInstanceCount(),
		Expected.CountOf(FenceLayout::EPostKind::Line));
	TestEqual(TEXT("every corner and gate post is an instance of the heavy post"),
		TestWorld.Buildings->GetFenceHeavyPostsForTest()->GetInstanceCount(), Heavy);

	const UDynamicMeshComponent* Fabric = TestWorld.Buildings->GetFenceFabricForTest();
	TestEqual(TEXT("every bay is two triangles of fabric"),
		Fabric->GetDynamicMesh()->GetMeshRef().TriangleCount(), 2 * Expected.Spans.Num());

	TestEqual(TEXT("the presenter's count agrees"),
		TestWorld.Buildings->GetPlotPresenter()->GetFencePostCount(),
		Expected.CountOf(FenceLayout::EPostKind::Line) + Heavy);
	TestEqual(TEXT("and the depot has its gate"),
		TestWorld.Buildings->GetPlotPresenter()->GetGateGapCount(), 1);

	// IDEMPOTENT: RebuildMesh runs on every graph change, and an appending fence would double
	// on every road the player drew anywhere on the airport.
	Road->RebuildMesh();
	TestEqual(TEXT("a rebuild does not double the posts"),
		TestWorld.Buildings->GetFencePostsForTest()->GetInstanceCount(),
		Expected.CountOf(FenceLayout::EPostKind::Line));
	TestEqual(TEXT("nor the fabric"),
		Fabric->GetDynamicMesh()->GetMeshRef().TriangleCount(), 2 * Expected.Spans.Num());
	return true;
}

/**
 * The fabric faces OUT and is textured by distance along the edge.
 *
 * ENGINE-COMPUTED NORMALS, per memory: Unreal's winding is left-handed and a hand-derived
 * cross product has agreed with itself while disagreeing with the rasteriser before. The
 * material is two-sided so either winding draws; facing out is what makes the lighting right.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotFenceFabricFacesOutTest,
	"Airside.Present.PlotFenceFabricFacesOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotFenceFabricFacesOutTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	if (!TestNotNull(TEXT("a buildings actor"), TestWorld.Buildings)) { return false; }
	TestWorld.Actor->ClearNetwork();
	PlaceFenceTestDepot(TestWorld.Actor);
	TestWorld.Actor->RebuildMesh();

	const UE::Geometry::FDynamicMesh3& Mesh =
		TestWorld.Buildings->GetFenceFabricForTest()->GetDynamicMesh()->GetMeshRef();
	if (!TestTrue(TEXT("there is fabric"), Mesh.TriangleCount() > 0)) { return false; }

	int32 South = 0;
	double MaxZ = 0.0;
	for (const int32 Tri : Mesh.TriangleIndicesItr())
	{
		FVector3d A, B, C;
		Mesh.GetTriVertices(Tri, A, B, C);
		MaxZ = FMath::Max(MaxZ, FMath::Max(A.Z, FMath::Max(B.Z, C.Z)));
		const FVector3d Centroid = (A + B + C) / 3.0;
		if (Centroid.Y < 0.0)
		{
			++South;
			TestTrue(TEXT("the frontage fabric faces south, out of the plot"),
				Mesh.GetTriNormal(Tri).Y < -0.99);
		}
	}
	TestTrue(TEXT("the frontage has fabric"), South > 0);
	TestEqual(TEXT("fabric is 2.4 m tall - the texture's V range"), MaxZ, 240.0, 1e-6);

	// U BY DISTANCE: the longest run of U on the 20 m frontage ends at 2000 / 240.
	const UE::Geometry::FDynamicMeshUVOverlay* UV = Mesh.Attributes()->GetUVLayer(0);
	float MaxU = 0.0f;
	for (const int32 Element : UV->ElementIndicesItr())
	{
		MaxU = FMath::Max(MaxU, UV->GetElement(Element).X);
	}
	TestEqual(TEXT("U is distance along the edge over the 2.4 m tile"),
		static_cast<double>(MaxU), 2400.0 / 240.0, 1e-4);
	return true;
}

#endif
```

(MaxU is 10.0: the 24 m side is the longest edge, so the maximum U over the whole mesh is 2400/240.)

- [ ] **Step 2: Build twice and confirm it fails to compile**

Expected: errors naming `GetFencePostsForTest`, `GetFencePostCount`. That's the red state.

- [ ] **Step 3: Presenter header**

In `PlotPresenter.h`, add `#include "Content/FenceKit.h"`, and forward declarations `class UHierarchicalInstancedStaticMeshComponent;` and `class UDynamicMeshComponent;`. Above the `UCLASS()` add:

```cpp
/**
 * Where the fence is drawn. Bundled because it travels together - see CLAUDE.md "one struct
 * per thing" - and any member may be null, which draws that part of the fence nowhere.
 */
struct FFenceTargets
{
	UHierarchicalInstancedStaticMeshComponent* Posts = nullptr;
	UHierarchicalInstancedStaticMeshComponent* HeavyPosts = nullptr;
	UDynamicMeshComponent* Fabric = nullptr;
};
```

Change `Initialise` to:

```cpp
	void Initialise(UInstancedStaticMeshComponent* InBoxes,
		UInstancedStaticMeshComponent* InGhosts = nullptr,
		const FFenceTargets& InFence = FFenceTargets());
```

and add to its doc comment: `InFence names the fence's three components; see FFenceTargets.`

Change `RebuildFrom` to take `const FFenceKit& Kit = FFenceKit()` as its third parameter, and append to its comment: `Kit is the fence's meshes and material, resolved by the caller for the same one-resolver reason as the specs; a null member falls back to grey box.`

Replace the `GetGateGapCount` doc comment and body with:

```cpp
	/**
	 * For tests: how many plots got a gate. One per plot, or the census names the one that did
	 * not - see FenceLayout::FLayout::bHasGate.
	 *
	 * A fence with no gate is a depot no truck can leave, and it would look completely correct
	 * from every angle - which is why the gate is counted rather than eyeballed. It counted
	 * SKIPPED BAYS until 2026-09-22, when the gate stopped being "the bay nearest the pose" and
	 * became an opening of exactly PlotYard::GateCorridorUu.
	 */
	int32 GetGateGapCount() const { return Gates; }

	/** For tests: fence posts of every kind drawn in the last rebuild. */
	int32 GetFencePostCount() const { return FencePosts; }

	/** For tests: fabric bays drawn in the last rebuild. */
	int32 GetFenceSpanCount() const { return FenceSpans; }
```

In the private section, replace `int32 GateGaps = 0;` and its comment with:

```cpp
	/** Counted during the last RebuildFrom. See GetGateGapCount. */
	int32 Gates = 0;

	/** Counted during the last RebuildFrom. See GetFencePostCount. */
	int32 FencePosts = 0;

	/** Counted during the last RebuildFrom. See GetFenceSpanCount. */
	int32 FenceSpans = 0;

	/** See FFenceTargets. UPROPERTY for the reason Boxes is one. */
	UPROPERTY(Transient) TObjectPtr<UHierarchicalInstancedStaticMeshComponent> FencePostsInto;
	UPROPERTY(Transient) TObjectPtr<UHierarchicalInstancedStaticMeshComponent> FenceHeavyPostsInto;
	UPROPERTY(Transient) TObjectPtr<UDynamicMeshComponent> FenceFabricInto;
```

Update the `Placed` comment's first line to say `Every MODULE transform added during the last rebuild, in the order it was added - the fence is not in it since 2026-09-22, when it moved to its own components.` Keep the rest of that comment.

The class comment's "ONE COMPONENT FOR EVERY MODULE TYPE AND THE FENCE TOO" paragraph is now false for the fence. Change "AND THE FENCE TOO" to "- THE FENCE HAS ITS OWN, since 2026-09-22 (posts are two authored meshes and the fabric is a strip, not a box)". Keep the rest of the paragraph.

- [ ] **Step 4: Presenter .cpp**

Add includes: `"Build/RoadMeshSink.h"`, `"Components/DynamicMeshComponent.h"`, `"Components/HierarchicalInstancedStaticMeshComponent.h"`, `"Present/DynamicMeshSink.h"`, `"Solve/FenceLayout.h"`.

Replace the three fence constants (`FenceBayUu`, `FenceHeightUu`, `FenceThicknessUu` and their comment) with:

```cpp
	/**
	 * The chainlink kit's figures, uu - the asset README's, not chosen here. POST AND FABRIC
	 * HEIGHT ARE ONE DECISION WITH THE TEXTURE: its V range IS 240 uu of fabric, so changing the
	 * fabric height without regenerating chainlink.png makes the diamonds stop being square.
	 */
	constexpr double FencePostHeightUu = 245.0;
	constexpr double FenceFabricHeightUu = 240.0;
	constexpr double FenceLinePostDiameterUu = 6.0;
	constexpr double FenceHeavyPostDiameterUu = 9.0;

	/** The asset contract's layout, gated at the truck corridor's width. */
	FenceLayout::FSpec FenceSpec()
	{
		FenceLayout::FSpec Spec;
		Spec.GateWidthUu = PlotYard::GateCorridorUu;
		return Spec;
	}

	/**
	 * One post's instance transform.
	 *
	 * TWO ORIGINS, because the fallback and the asset disagree about where theirs is: the
	 * authored post is base-centred at 1:1, the engine cube is 100 uu and centred. A cube
	 * placed like a post sinks to its waist, and a post placed like a cube floats.
	 */
	FTransform FencePostAt(const FenceLayout::FPost& Post, const UStaticMesh* Authored)
	{
		const FRotator Rotation(0.0, FMath::RadiansToDegrees(Post.YawRad), 0.0);
		if (Authored != nullptr)
		{
			return FTransform(Rotation, FVector(Post.Position.X, Post.Position.Y, 0.0));
		}
		const double Diameter = Post.Kind == FenceLayout::EPostKind::Line
			? FenceLinePostDiameterUu : FenceHeavyPostDiameterUu;
		return FTransform(Rotation,
			FVector(Post.Position.X, Post.Position.Y, FencePostHeightUu * 0.5),
			FVector(Diameter / CubeUu, Diameter / CubeUu, FencePostHeightUu / CubeUu));
	}

	/**
	 * One bay of fabric: a vertical quad, four vertices of its own, two triangles.
	 *
	 * NOT SHARED WITH THE NEXT BAY. FDynamicMeshSink computes per-vertex normals, and a vertex
	 * shared at a corner would average two edges' normals and shade a smear down the post.
	 *
	 * NOT THROUGH AppendTriangleUp, whose sliver guard measures area in XY - which is zero for
	 * every vertical triangle, so it would drop the whole fence. Wound (A0, B1, B0), (A0, A1, B1)
	 * so the engine's left-handed normal points OUTWARD; Airside.Present.PlotFenceFabricFacesOut
	 * measures it.
	 */
	void AppendFenceBay(FRoadMeshBuffers& Buffers, const FenceLayout::FSpan& Span)
	{
		const int32 A0 = Buffers.Positions.Num();
		Buffers.Positions.Add(FVector3d(Span.A.X, Span.A.Y, 0.0));
		Buffers.Positions.Add(FVector3d(Span.B.X, Span.B.Y, 0.0));
		Buffers.Positions.Add(FVector3d(Span.B.X, Span.B.Y, FenceFabricHeightUu));
		Buffers.Positions.Add(FVector3d(Span.A.X, Span.A.Y, FenceFabricHeightUu));
		const int32 B0 = A0 + 1;
		const int32 B1 = A0 + 2;
		const int32 A1 = A0 + 3;

		// V 1 AT THE GROUND, 0 AT THE TOP - the image's own orientation. The texture does not
		// tile in V, so this is the one direction that is not arbitrary.
		const float U0 = static_cast<float>(Span.U0);
		const float U1 = static_cast<float>(Span.U1);
		Buffers.UV0.Append({ FVector2f(U0, 1.0f), FVector2f(U1, 1.0f),
		                     FVector2f(U1, 0.0f), FVector2f(U0, 0.0f) });

		// The sink reads three UV layers per vertex; the fabric's material samples only UV0.
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			Buffers.UV1.Add(FVector2f::ZeroVector);
			Buffers.UV2.Add(FVector2f::ZeroVector);
		}

		Buffers.Indices.Append({ A0, B1, B0, A0, A1, B1 });
		Buffers.MaterialIDs.Append({ 0, 0 });
	}
```

Update `Initialise`:

```cpp
void UPlotPresenter::Initialise(UInstancedStaticMeshComponent* InBoxes,
	UInstancedStaticMeshComponent* InGhosts, const FFenceTargets& InFence)
{
	Boxes = InBoxes;
	GhostBoxes = InGhosts;
	FencePostsInto = InFence.Posts;
	FenceHeavyPostsInto = InFence.HeavyPosts;
	FenceFabricInto = InFence.Fabric;
}
```

In `Clear()`, replace `GateGaps = 0;` with `Gates = 0; FencePosts = 0; FenceSpans = 0;`, and before the counters add:

```cpp
	if (FencePostsInto != nullptr)
	{
		FencePostsInto->ClearInstances();
	}
	if (FenceHeavyPostsInto != nullptr)
	{
		FenceHeavyPostsInto->ClearInstances();
	}
	if (FenceFabricInto != nullptr)
	{
		// THROUGH THE SINK, EMPTY, rather than resetting the mesh by hand: the sink is the one
		// place that knows how buffers become this component's mesh, empty ones included.
		FDynamicMeshSink(FenceFabricInto, nullptr, /*bInUseConstantVertexColour=*/false,
			nullptr, /*bInQuiet=*/true).Accept(FRoadMeshBuffers());
	}
```

Change `RebuildFrom`'s signature to add `const FFenceKit& Kit`. Directly after the `Clear();` call, add:

```cpp

	// THE AUTHORED POSTS, when there are any. Set per rebuild rather than once, because the
	// content set can change under an open editor; SetStaticMesh is a no-op when unchanged.
	if (FencePostsInto != nullptr && Kit.LinePost != nullptr)
	{
		FencePostsInto->SetStaticMesh(Kit.LinePost);
	}
	if (FenceHeavyPostsInto != nullptr && Kit.HeavyPost != nullptr)
	{
		FenceHeavyPostsInto->SetStaticMesh(Kit.HeavyPost);
	}
	FRoadMeshBuffers Fabric;
```

Replace the whole fence block (from `// --- The fence ---` to the end of its `for` over outline edges) with:

```cpp
		// --- The fence -----------------------------------------------------------------
		//
		// THE GATE IS A GAP OF EXACTLY THE TRUCK CORRIDOR, centred on the pose, so the hole in
		// the fence is where the truck actually leaves and as wide as the lane PlotYard keeps
		// clear behind it - one number, not two decisions that could disagree.
		const FenceLayout::FLayout Fence = FenceLayout::Solve(Entity.Outline, Entity.Position, FenceSpec());
		if (Fence.bHasGate)
		{
			++Gates;
		}
		else
		{
			// A DEPOT NO TRUCK CAN LEAVE, said out loud - see GetGateGapCount.
			UE_LOG(LogAirside, Warning,
				TEXT("Plots: the plot gated at (%.0f, %.0f) has no gate - its frontage is ")
				TEXT("shorter than a %.0f uu gate plus a bay either side"),
				Entity.Position.X, Entity.Position.Y, PlotYard::GateCorridorUu);
		}

		for (const FenceLayout::FPost& Post : Fence.Posts)
		{
			const bool bHeavy = Post.Kind != FenceLayout::EPostKind::Line;
			UHierarchicalInstancedStaticMeshComponent* Into =
				bHeavy ? FenceHeavyPostsInto.Get() : FencePostsInto.Get();
			if (Into == nullptr)
			{
				continue;
			}
			Into->AddInstance(FencePostAt(Post, bHeavy ? Kit.HeavyPost : Kit.LinePost),
				/*bWorldSpace=*/true);
			++FencePosts;
		}
		for (const FenceLayout::FSpan& Span : Fence.Spans)
		{
			AppendFenceBay(Fabric, Span);
			++FenceSpans;
		}
```

After the entity loop, before `RoomForMore = Ghosts;`, add:

```cpp
	// ONE STRIP FOR EVERY PLOT'S FABRIC, one draw call - the reason the fabric is a strip and
	// not a mesh per bay (asset README). Quiet: the sink's per-call DIAG lines describe the
	// road surface, and a fence line beside them would read as a second road.
	if (FenceFabricInto != nullptr)
	{
		FDynamicMeshSink(FenceFabricInto, Kit.Fabric, /*bInUseConstantVertexColour=*/false,
			nullptr, /*bInQuiet=*/true).Accept(Fabric);
	}
```

Update the modules-before-fence comment inside the loop (`// MODULES BEFORE THE FENCE, always: ...`) to:

```cpp
		// MODULES ONLY IN Placed since 2026-09-22 - the fence draws into its own components, so
		// the first ModuleBoxes instances of a plot are its modules with no ordering to keep.
```

Update the census log:

```cpp
		UE_LOG(LogAirside, Log,
			TEXT("Plots: %d plot(s), %d module bay(s) built, %d ghosted, %d dropped, "
				 "%d fence post(s), %d fabric bay(s), %d gate(s)"),
			Plots, ModuleBoxes, Ghosts, Dropped, FencePosts, FenceSpans, Gates);
```

Delete the now-unused `ModuleInstances` local, its `++ModuleInstances;` and its comment. Its only reader was the old fence tally.

- [ ] **Step 5: Buildings actor components**

In `AirsideBuildingsActor.h`, add forward declarations `class UHierarchicalInstancedStaticMeshComponent;` and `class UDynamicMeshComponent;`. After `GetPlotPresenter()` add:

```cpp

	/**
	 * For tests: the fence's components. Same ...ForTest precedent as
	 * UPlotPresenter::GetInstanceTransformForTest - widening them would open them to everything.
	 */
	UHierarchicalInstancedStaticMeshComponent* GetFencePostsForTest() const { return FencePosts; }
	UHierarchicalInstancedStaticMeshComponent* GetFenceHeavyPostsForTest() const { return FenceHeavyPosts; }
	UDynamicMeshComponent* GetFenceFabricForTest() const { return FenceFabric; }
```

After `ModuleGhosts`:

```cpp

	/**
	 * The chainlink line posts - HIERARCHICAL, unlike the module boxes, because a perimeter is
	 * hundreds of identical posts and the hierarchy is what culls the ones off screen.
	 */
	UPROPERTY() TObjectPtr<UHierarchicalInstancedStaticMeshComponent> FencePosts;

	/** Corner and gate posts - the heavier mesh. */
	UPROPERTY() TObjectPtr<UHierarchicalInstancedStaticMeshComponent> FenceHeavyPosts;

	/**
	 * Every plot's fabric, one strip. Distance-field lighting OFF: Lumen's distance fields
	 * ignore opacity masks, so a fence left on would be a solid wall in the field and drop a
	 * black box over the plot (asset README). A dynamic mesh builds no distance field today;
	 * the flag is set so a later switch to a static mesh cannot quietly bring the box back.
	 */
	UPROPERTY() TObjectPtr<UDynamicMeshComponent> FenceFabric;
```

In `AirsideBuildingsActor.cpp`, add includes `"Components/DynamicMeshComponent.h"`, `"Components/HierarchicalInstancedStaticMeshComponent.h"` and `"Content/AirsideSettings.h"`. In the constructor, before `Plots = CreateDefaultSubobject...`:

```cpp
	// THE FENCE. Posts start as the cube for the grey-box fallback; the presenter swaps in the
	// authored meshes each rebuild when the content set has them.
	FencePosts = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("FencePosts"));
	FencePosts->SetupAttachment(RootComponent);
	DressAsModuleBoxes(*FencePosts, CubeMesh);

	FenceHeavyPosts = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("FenceHeavyPosts"));
	FenceHeavyPosts->SetupAttachment(RootComponent);
	DressAsModuleBoxes(*FenceHeavyPosts, CubeMesh);

	// ABSOLUTE, like every surface ARoadNetworkActor draws: the strip is built in world
	// coordinates and must not be transformed a second time.
	FenceFabric = CreateDefaultSubobject<UDynamicMeshComponent>(TEXT("FenceFabric"));
	FenceFabric->SetupAttachment(RootComponent);
	FenceFabric->SetUsingAbsoluteLocation(true);
	FenceFabric->SetUsingAbsoluteRotation(true);
	FenceFabric->SetUsingAbsoluteScale(true);
	FenceFabric->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	FenceFabric->bAffectDistanceFieldLighting = false;
```

Change both `Plots->Initialise(ModuleBoxes, ModuleGhosts);` calls (constructor and `PostInitProperties`) to:

```cpp
	Plots->Initialise(ModuleBoxes, ModuleGhosts, FFenceTargets{ FencePosts, FenceHeavyPosts, FenceFabric });
```

In `PostInitProperties`, before that call, re-point the three by name:

```cpp
	FencePosts = Cast<UHierarchicalInstancedStaticMeshComponent>(GetDefaultSubobjectByName(TEXT("FencePosts")));
	FenceHeavyPosts = Cast<UHierarchicalInstancedStaticMeshComponent>(GetDefaultSubobjectByName(TEXT("FenceHeavyPosts")));
	FenceFabric = Cast<UDynamicMeshComponent>(GetDefaultSubobjectByName(TEXT("FenceFabric")));
```

`DressAsModuleBoxes` takes `UInstancedStaticMeshComponent&`, and HISM derives from ISM, so it accepts them. Rename it `DressAsCubes` in all four calls, with its comment's first line changed to "An instanced component of engine cubes, no collision." The name then stays true.

In `Rebuild`, change the last line to:

```cpp
	// THE FENCE'S CONTENT through UAirsideSettings' one resolver, like every content default.
	Plots->RebuildFrom(Network, Road->ResolveDepotKits(), UAirsideSettings::ResolveFenceKit());
```

- [ ] **Step 6: Move the old fence assertions in `PlotPresenterTest.cpp`**

`GetInstanceCount()` now counts modules only, and the 12×8 m `PlaceDepot` seats none (measured in PR 1). Change these tests:
- `DressesEachBay`: replace `const int32 One = ...GetInstanceCount();` and the `One > 3` assertion with:

```cpp
	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	const int32 One = Plots->GetFencePostCount();

	// A FENCE STANDS even round a plot too small to seat a module - the 12 x 8 m Tier 1 plot
	// seats none under the band layout. The fence is the claim here; modules are
	// PlotPresenterLaysOutTheDepot's.
	TestTrue(TEXT("a fence stands up"), One > 0);
```

  - The next two `GetInstanceCount()` assertions (`One * 2`) read `Plots->GetFencePostCount()`.
  - `GetGateGapCount() > 0` stays.
  - The first "an empty airport stands nothing up" assertion reads both `GetInstanceCount()` and `GetFencePostCount()`, each equal to 0. Split it into two `TestEqual`s.
- `LaysOutTheDepot`: replace the `TestTrue(TEXT("the fence went up around them"), Instances.Num() > Modules)` block with `TestTrue(TEXT("the fence went up around them"), Plots->GetFencePostCount() > 0)`, keeping the early return. Replace the "THE MODULES ARE THE FIRST INSTANCES a plot adds, before its fence..." comment with "EVERY INSTANCE IS A MODULE since the fence moved to its own components (2026-09-22), so the first depot's are simply the first Modules entries."
  - In the second-depot block, replace `Both.Num() > Instances.Num()` with `Both.Num() == Instances.Num() * 2` and reason text "the second depot stood its modules up too". `SecondStart = Instances.Num()` stays correct (it is now the first depot's module count).
  - Replace the "the ones added after the first plot's fence" comment with "the ones after the first depot's".
- `SurvivesDuplication`: replace `GetInstanceCount() > 3` with `GetFencePostCount() > 0` and keep the reason "the duplicate's own boxes stand up". The claim is re-pointing, and the fence components are re-pointed too.

- [ ] **Step 7: Build, run**

Build (the new test file needs a second build if Step 2's build did not get far enough to register it; build twice). Run `-Filter Airside.Present` and `-Filter Airside.Solve.FenceLayout`.
Expected: Present = the Task 0 subset baseline (102) + 2 passing, 0 failed; FenceLayout 7/0/0.

- [ ] **Step 8: Prove the composition test measures the wiring**

In the buildings actor constructor, temporarily pass `FFenceTargets{ nullptr, nullptr, nullptr }`. Rebuild and run `-Filter Airside.Present.PlotFence`.
Expected: `PlotFenceReachesTheComponents` FAILS on the post counts. Restore it, rebuild, rerun, and get 2 passing.

- [ ] **Step 9: Commit**

```bash
git add -A Plugins/Airside/Source
git commit -m "feat(present): chainlink fence from FenceLayout - post HISMs and one fabric strip"
```

---

### Task 4: Author the content headlessly

**Files:**
- Create: `Tools/Python/build_fence_content.py`
- Modify: `Tools/Python/import_models.py` (docstring lines 21-22)

- [ ] **Step 1: Write the script**

```python
"""Imports the chainlink fence kit and points DA_AirsideContent at it. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED. Every result line is prefixed MARKER: so it can be grepped out of
Saved/Logs/AirportMgr.log.

WHAT THE KIT IS - see AirportMgr2Models/accessories/chainlink/README.md, which is the asset
contract: two posts from Blender, one texture pair, and a material built here. The fabric
MESH is not content at all; UPlotPresenter generates it from each plot's outline.

NOT import_models.py. That table drives airside_import, a glTF vehicle pipeline - wheel
nodes, rigs, axle checks - and these are two static FBX meshes and two PNGs.

FIRST IMPORT ONLY for the meshes, like import_models.py: an existing asset is kept and
re-measured, never re-imported over (memory: a re-import strands a stray mesh). The
material and the texture settings are rebuilt every run; they are cheap and idempotent.

MEASURED AFTER IMPORT, not trusted: FBX unit handling differs between exporters, and a post
that arrives 2.45 uu tall is a correct-looking import of the wrong size. The height and
radius are checked against the README's figures and the run FAILS if they are off.
"""
import os

import unreal

MODELS = r"C:\repos\AirportMgr2Models\accessories"
FENCE_DIR = "/Game/Environment/Fence"
CONTENT = "/Game/DA_AirsideContent"

POSTS = [
    # asset name, fbx, expected radius uu, content property
    ("SM_Fence_Post", os.path.join(MODELS, "chainlink", "export", "SM_Fence_Post.fbx"), 3.0, "fence_line_post"),
    ("SM_Fence_CornerPost", os.path.join(MODELS, "chainlink", "export", "SM_Fence_CornerPost.fbx"), 4.5, "fence_heavy_post"),
]
POST_HEIGHT_UU = 245.0
TOLERANCE_UU = 0.5

TEX_ALBEDO = ("T_Chainlink", os.path.join(MODELS, "textures", "chainlink.png"))
TEX_NORMAL = ("T_Chainlink_N", os.path.join(MODELS, "textures", "chainlink_n.png"))
MAT_NAME = "M_ChainlinkFabric"

# README: "Opacity Mask Clip Value ~ 0.33" and "Alpha Coverage Thresholds ~ 0.33" - the one
# number, on both. Without the texture's, the mip chain averages the wire away and the fence
# dissolves at distance.
CLIP = 0.33


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def import_file(source, name):
    task = unreal.AssetImportTask()
    task.filename = source
    task.destination_path = FENCE_DIR
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    return unreal.EditorAssetLibrary.load_asset("%s/%s" % (FENCE_DIR, name))


def post(name, source, radius):
    path = "%s/%s" % (FENCE_DIR, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mesh = unreal.EditorAssetLibrary.load_asset(path)
        say("kept existing %s" % path)
    else:
        if not os.path.exists(source):
            fail("no export at %s - run build_posts.py in the models repo" % source)
            return None
        mesh = import_file(source, name)
        say("imported %s" % path)
    if not isinstance(mesh, unreal.StaticMesh):
        fail("%s is not a StaticMesh after import (got %r)" % (path, mesh))
        return None

    box = mesh.get_bounding_box()
    height = box.max.z - box.min.z
    half_x = (box.max.x - box.min.x) * 0.5
    say("%s bounds: height %.2f uu, radius %.2f uu, base z %.2f" % (name, height, half_x, box.min.z))
    ok = True
    if abs(height - POST_HEIGHT_UU) > TOLERANCE_UU:
        fail("%s is %.2f uu tall, expected %.1f - an FBX unit mismatch" % (name, height, POST_HEIGHT_UU))
        ok = False
    if abs(half_x - radius) > TOLERANCE_UU:
        fail("%s radius %.2f uu, expected %.1f" % (name, half_x, radius))
        ok = False
    if abs(box.min.z) > TOLERANCE_UU:
        fail("%s base at z %.2f, expected 0 - the presenter stands posts on their origin" % (name, box.min.z))
        ok = False
    return mesh if ok else None


def texture(name, source, normal):
    tex = import_file(source, name)
    if tex is None:
        fail("%s imported nothing" % name)
        return None
    if normal:
        tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
        tex.set_editor_property("srgb", False)
    else:
        # UE strips a bool's b prefix on the way to Python: bDoScaleMipsForAlphaCoverage.
        tex.set_editor_property("do_scale_mips_for_alpha_coverage", True)
        tex.set_editor_property("alpha_coverage_thresholds", unreal.Vector4(0.0, 0.0, 0.0, CLIP))
    path = "%s/%s" % (FENCE_DIR, name)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    # READ BACK after reload: a headless save that wrote nothing is a known failure here.
    back = unreal.EditorAssetLibrary.load_asset(path)
    if not normal:
        on = back.get_editor_property("do_scale_mips_for_alpha_coverage")
        thr = back.get_editor_property("alpha_coverage_thresholds")
        if not on or abs(thr.w - CLIP) > 1e-4:
            fail("%s alpha coverage did not stick: on=%s w=%.3f" % (name, on, thr.w))
            return None
        say("PASS %s alpha coverage on, threshold %.2f" % (name, thr.w))
    return back


def material(albedo, normal):
    lib = unreal.MaterialEditingLibrary
    path = "%s/%s" % (FENCE_DIR, MAT_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mat = unreal.EditorAssetLibrary.load_asset(path)
        lib.delete_all_material_expressions(mat)
    else:
        mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            MAT_NAME, FENCE_DIR, unreal.Material, unreal.MaterialFactoryNew())

    # MASKED, NOT TRANSLUCENT (README): it sorts correctly and costs a fraction as much.
    # TWO-SIDED: the player sees both faces of every bay.
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    mat.set_editor_property("two_sided", True)
    mat.set_editor_property("opacity_mask_clip_value", CLIP)

    base = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -500, 0)
    base.set_editor_property("texture", albedo)
    lib.connect_material_property(base, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    lib.connect_material_property(base, "A", unreal.MaterialProperty.MP_OPACITY_MASK)

    if normal is not None:
        nrm = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -500, 300)
        nrm.set_editor_property("texture", normal)
        nrm.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
        lib.connect_material_property(nrm, "RGB", unreal.MaterialProperty.MP_NORMAL)

    rough = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -300, 200)
    rough.set_editor_property("r", 0.45)
    lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    metal = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -300, 260)
    metal.set_editor_property("r", 1.0)
    lib.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)

    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    back = unreal.EditorAssetLibrary.load_asset(path)
    if back.get_editor_property("blend_mode") != unreal.BlendMode.BLEND_MASKED or not back.get_editor_property("two_sided"):
        fail("%s did not save masked and two-sided" % path)
        return None
    say("PASS %s masked, two-sided, clip %.2f" % (MAT_NAME, back.get_editor_property("opacity_mask_clip_value")))
    return back


def run():
    meshes = {}
    for name, source, radius, prop in POSTS:
        mesh = post(name, source, radius)
        if mesh is None:
            say("DONE")
            return
        meshes[prop] = mesh

    albedo = texture(TEX_ALBEDO[0], TEX_ALBEDO[1], normal=False)
    normal = texture(TEX_NORMAL[0], TEX_NORMAL[1], normal=True)
    if albedo is None:
        say("DONE")
        return
    mat = material(albedo, normal)
    if mat is None:
        say("DONE")
        return

    content = unreal.EditorAssetLibrary.load_asset(CONTENT)
    if content is None:
        fail("%s not found - content set not updated" % CONTENT)
        say("DONE")
        return
    for prop, mesh in meshes.items():
        content.set_editor_property(prop, mesh)
    content.set_editor_property("fence_fabric_material", mat)
    unreal.EditorAssetLibrary.save_asset(CONTENT, only_if_is_dirty=False)

    back = unreal.EditorAssetLibrary.load_asset(CONTENT)
    missing = [p for p in ("fence_line_post", "fence_heavy_post", "fence_fabric_material")
               if back.get_editor_property(p) is None]
    if missing:
        fail("DA_AirsideContent lost %s on save" % ", ".join(missing))
    else:
        say("PASS DA_AirsideContent names the fence kit")
        say("ALL VERIFIED")
    say("DONE")


run()
```

Before running, check that `unreal.StaticMesh.get_bounding_box` exists in 5.8. Grep the engine: `grep -rn "GetBoundingBox" D:/Epic/UE_5.8/Engine/Source/Runtime/Engine/Classes/Engine/StaticMesh.h`. If it isn't a UFUNCTION, use `mesh.get_bounds()` (BoxSphereBounds: `.box_extent`, `.origin`) and derive min/max from origin ± extent. Also confirm `delete_all_material_expressions` exists. The memory "Material graph authoring traps" says delete-all deletes half: if the rebuilt material shows duplicated nodes, delete the asset (checking on disk afterwards) and recreate it.

- [ ] **Step 2: Run it (editor closed)**

```
& "D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\repos\AirportMgr2\AirportMgr.uproject" `
  -run=pythonscript -script="C:\repos\AirportMgr2\Tools\Python\build_fence_content.py" -unattended -nosplash -nopause
```

Then `grep "MARKER:" Saved/Logs/AirportMgr.log`.
Expected: two bounds lines at ~245 uu height and radius 3.0 / 4.5, the alpha-coverage PASS line, the material PASS line, `PASS DA_AirsideContent names the fence kit`, and `ALL VERIFIED`. If a post measures 2.45 tall, the FBX came in at 1/100 scale. Report it and stop; don't patch the scale in the script without asking.

- [ ] **Step 3: Check what else the import created**

`ls Content/Environment/Fence/`. Interchange may have made a material for `chainlink_post`. Record its name. It should be a plain grey metal; if it's the grid checker, note it for the PIE check rather than authoring a replacement here.

- [ ] **Step 4: `import_models.py` docstring**

Replace the two lines starting `- accessories/chainlink. Two fence POSTS are exported and no panel...` with:

```
- accessories/chainlink. Imported by build_fence_content.py instead: two static FBX posts and a
  texture pair, none of which this glTF vehicle table's machinery applies to.
```

- [ ] **Step 5: Commit**

```bash
git add Tools/Python/build_fence_content.py Tools/Python/import_models.py Content/Environment/Fence Content/DA_AirsideContent.uasset
git commit -m "content: chainlink fence kit - posts, T_Chainlink with alpha-coverage mips, M_ChainlinkFabric"
```

---

### Task 5: The authored kit resolves

**Files:**
- Modify: `Plugins/Airside/Source/AirsideTests/Private/AirsideContentTest.cpp`

- [ ] **Step 1: Add the test** at the end of the file, before `#endif`:

```cpp
/**
 * The project's content set names a whole fence kit, and the fabric is masked and two-sided.
 *
 * AGAINST THE REAL DA_AirsideContent, not a NewObject: the failure this guards is the one a
 * build_*.py writes nothing and reports success, and only the saved asset can show it. A
 * translucent fabric would sort wrongly; a one-sided one vanishes from inside the plot.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirsideContentFenceKitResolvesTest,
	"Airside.Content.FenceKitResolves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirsideContentFenceKitResolvesTest::RunTest(const FString& Parameters)
{
	const FFenceKit Kit = UAirsideSettings::ResolveFenceKit();
	TestNotNull(TEXT("the line post is authored"), Kit.LinePost);
	TestNotNull(TEXT("the heavy post is authored"), Kit.HeavyPost);
	if (!TestNotNull(TEXT("the fabric material is authored"), Kit.Fabric)) { return false; }
	TestEqual(TEXT("the fabric is Masked, per the asset README"),
		Kit.Fabric->GetBlendMode(), EBlendMode::BLEND_Masked);
	TestTrue(TEXT("and two-sided"), Kit.Fabric->IsTwoSided());
	return true;
}
```

Add `#include "Content/FenceKit.h"` and `#include "Materials/MaterialInterface.h"` to the includes.

- [ ] **Step 2: Build, run** `-Filter Airside.Content`. Expected: all pass, including `FenceKitResolves`.

- [ ] **Step 3: Commit**

```bash
git add Plugins/Airside/Source/AirsideTests/Private/AirsideContentTest.cpp
git commit -m "test(content): the authored fence kit resolves, masked and two-sided"
```

---

### Task 6: Verify, measure, amend the spec, PR

- [ ] **Step 1:** `./Tools/Run-AirsideTests.ps1`. Expected: 0 failed, 0 crashed, total = Task 0 + 10 (7 FenceLayout, 2 PlotFence, 1 Content).
- [ ] **Step 2:** Re-run the Task 0 count loop and compute the `UE_LOG` delta: +1 in `PlotPresenter.cpp` for the no-gate Warning, none removed. The comment delta must not fall.
- [ ] **Step 3: PIE look (needs the user).** Ask them to open M_Starter, start PIE and place a fuel depot. Then run `python Tools/Mcp.py log LogAirside "Plots:"` and `python Tools/Mcp.py shot out.png editor`. Expected: `... N fence post(s), M fabric bay(s), 1 gate(s)`. The screenshot should show diamonds that look square, heavy posts on the corners and at the gate, fabric on the outside of the posts, no black box over the plot, and the fence still visible when zoomed out (alpha-coverage mips). Get close-up and far shots. Anything wrong here is a finding, not a pass.
- [ ] **Step 4: Amend the spec's PR 2 section**, in place: `FSpec` has no `FabricHeightUu`; the layout reports `GateCentre`, not `GateEdge`; content comes from `build_fence_content.py`; the fabric falls back to the sink's default material; the clear opening is 611 uu (post centres 620); posts have no collision. Commit `docs: fence spec follows the plan`.
- [ ] **Step 5:** Push and `gh pr create --base main --title "feat: chainlink fence round drawn plots (chainlink fence 2/2)"`. Use the repo template: build line, test line, `UE_LOG`/comment deltas, the Step 8 red check from Task 3, and the PIE evidence from Step 3. End the body with the Claude Code line.
