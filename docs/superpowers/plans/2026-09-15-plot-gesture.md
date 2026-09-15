# Plot Gesture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A fuel depot is placed by snapping to a service road, dragging a width in whole
bays, dragging a depth in whole rows, and pressing Build — with a readout that says what you
are about to get before you buy it.

**Architecture:** A four-stage tool replaces the freeform polygon one. Stages are objects,
as the outline tool's already are. The tool emits display-ready FACTS through a second sink;
the controller collects them each frame and the bottom bar renders them. Committing is a
`FBuildAction` in the existing registry, so the button gets its enabling and its tests from
machinery that already exists.

**Tech Stack:** UE 5.8, C++. Airside plugin (`Solve/`, `Tool/`, `Present/`) and the
AirportMgr game module (UMG bottom bar).

**Spec:** `docs/superpowers/specs/2026-09-15-plot-gesture-design.md`

## Global Constraints

- **Units are uu, 100 uu = 1 m.** `PlotFit::BayWidthUu = 400.0`, `BayDepthUu = 800.0`.
- **Headings are radians.**
- **`Solve/` may include `CoreMinimal.h` and nothing else**, enforced by `Check-Architecture.ps1`.
- **`Model/` must not dereference `Entities/`.**
- **Tools describe intent in ROAD PLANE coordinates naming a MEANING, never a colour.** That
  is what keeps the plugin free of the game module.
- **Build:** `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE`
  — `-NoHotReloadFromIDE` is correct **because this is a worktree** and must never be used on
  the main checkout. **The editor must be closed**, or the link fails with
  `LNK1104: cannot open file '...UnrealEditor-Airside.dll'`.
- **Test:** `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject"`.
  Read the `N test(s) run, N failed, N crashed` line. **Never trust the exit code.**
- **A new test .cpp needs two builds.** The first reports `Result: Succeeded` without
  compiling it.
- **Test names must be distinct leaves.**

### Deviation from the spec, recorded

§5 says the readout sink is *"passed into `BuildPreview` beside `IToolPreviewSink`"*. This
plan adds a **separate defaulted virtual** `BuildReadout` instead. `BuildPreview` has eight
implementors and two test doubles, none of which has a readout, and changing its signature
would churn all ten to add a parameter they ignore. The spec's argument — that facts emitted
from a `const` per-frame call cannot drift from the geometry — survives, because both calls
are `const` and are made from the same place on the same frame, and Task 6's test pins it.

---

### Task 1: The slot grid

Pure geometry. Built first because everything else consumes it.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/PlotFit.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/PlotFit.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotFitTest.cpp`

**Interfaces:**
- Consumes: existing `PlotFit::FPlotBay`, `BayWidthUu`, `BayDepthUu`.
- Produces: `PlotFit::FPlotGrid { TArray<FPlotBay> Slots; int32 Width; int32 Depth; }`,
  `PlotFit::BuildGrid(FVector2D FrontageA, FVector2D FrontageB, int32 Width, int32 Depth)`,
  and `PlotFit::GridOutline(FVector2D FrontageA, FVector2D FrontageB, int32 Width, int32 Depth)`
  returning `TArray<FVector2D>` — the plot rectangle, counter-clockwise, implicitly closed.

- [ ] **Step 1: Write the failing test**

Append to `PlotFitTest.cpp` before `#endif`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotGridSlotsTest,
	"Airside.Solve.PlotGridSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotGridSlotsTest::RunTest(const FString& Parameters)
{
	// Frontage on y = 0 running east, so the interior is to the north.
	const FVector2D A(0.0, 0.0);
	const FVector2D B(1200.0, 0.0);

	// THREE WIDE, TWO DEEP is six slots. The first THREE are row 1 - the row that fronts
	// the road and gets the modules - and the order matters, because the presenter fills
	// slots in order and a row-major mistake would put a shed in the back yard.
	{
		const PlotFit::FPlotGrid Grid = PlotFit::BuildGrid(A, B, 3, 2);
		TestEqual(TEXT("three by two is six slots"), Grid.Slots.Num(), 6);
		TestEqual(TEXT("and remembers its width"), Grid.Width, 3);
		TestEqual(TEXT("and its depth"), Grid.Depth, 2);

		// Row 1 sits half a bay depth in; row 2 a full bay behind it.
		for (int32 I = 0; I < 3; ++I)
		{
			TestTrue(TEXT("row 1 fronts the road"),
				FMath::IsNearlyEqual(Grid.Slots[I].Centre.Y, PlotFit::BayDepthUu * 0.5, 1.0));
		}
		for (int32 I = 3; I < 6; ++I)
		{
			TestTrue(TEXT("row 2 sits a full bay behind row 1"),
				FMath::IsNearlyEqual(Grid.Slots[I].Centre.Y, PlotFit::BayDepthUu * 1.5, 1.0));
		}
	}

	// EVERY SLOT FACES AWAY FROM THE ROAD, back rows included - a module in row 2 is still
	// a shed whose doors face the yard, not one turned round because it is not on the road.
	{
		const PlotFit::FPlotGrid Grid = PlotFit::BuildGrid(A, B, 2, 2);
		for (const PlotFit::FPlotBay& Slot : Grid.Slots)
		{
			const FVector2D Forward(FMath::Cos(Slot.Heading), FMath::Sin(Slot.Heading));
			TestTrue(TEXT("+X points away from the frontage"), Forward.Y > 0.9);
		}
	}

	// THE OUTLINE IS COUNTER-CLOCKWISE, because the pad goes through the same triangulator
	// an apron does and a clockwise one faces DOWN - which shipped once and rendered as no
	// concrete at all. Shoelace sign, positive for CCW.
	{
		const TArray<FVector2D> Outline = PlotFit::GridOutline(A, B, 3, 2);
		TestEqual(TEXT("a rectangle has four corners, not five"), Outline.Num(), 4);

		double Twice = 0.0;
		for (int32 I = 0; I < Outline.Num(); ++I)
		{
			const FVector2D& P = Outline[I];
			const FVector2D& Q = Outline[(I + 1) % Outline.Num()];
			Twice += P.X * Q.Y - Q.X * P.Y;
		}
		TestTrue(TEXT("and is wound counter-clockwise"), Twice > 0.0);
	}

	// A degenerate size yields nothing rather than a zero-area rectangle the player could
	// still press Build on.
	{
		TestEqual(TEXT("zero width is no slots"), PlotFit::BuildGrid(A, B, 0, 2).Slots.Num(), 0);
		TestEqual(TEXT("zero depth is no slots"), PlotFit::BuildGrid(A, B, 3, 0).Slots.Num(), 0);
	}

	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

Run the build command. Expected: FAIL — `'BuildGrid': is not a member of 'PlotFit'`.

- [ ] **Step 3: Declare the grid**

In `PlotFit.h`, after `FPlotFit`:

```cpp
	/**
	 * A plot's slots: Width bays across the frontage, Depth rows back from it.
	 *
	 * ROW-MAJOR AND FRONT ROW FIRST. Slots[0 .. Width-1] is the row that fronts the road and
	 * takes the modules; everything after it is expansion space. The presenter fills in this
	 * order, so the order is part of the contract rather than an accident of the loop.
	 */
	struct FPlotGrid
	{
		TArray<FPlotBay> Slots;
		int32 Width = 0;
		int32 Depth = 0;
	};

	/**
	 * Lay Width x Depth slots against the frontage edge A->B, interior on its left.
	 *
	 * NO POLYGON AND NO CONTAINMENT TEST, unlike FitBays: a rectangle built from the edge
	 * cannot fall outside itself. This is the whole reason the staged gesture is cheaper than
	 * the freeform one - the shape is known before the geometry is asked about.
	 */
	AIRSIDE_API FPlotGrid BuildGrid(FVector2D FrontageA, FVector2D FrontageB,
		int32 Width, int32 Depth);

	/**
	 * The plot's own boundary, counter-clockwise and implicitly closed.
	 *
	 * COUNTER-CLOCKWISE IS NOT COSMETIC. The pad is triangulated by the same ear-clipper an
	 * apron uses, which orients its triangles from the winding, and the surface is not
	 * two-sided - a clockwise outline renders as no concrete at all, which shipped on
	 * 2026-09-15 and took a screenshot to find.
	 */
	AIRSIDE_API TArray<FVector2D> GridOutline(FVector2D FrontageA, FVector2D FrontageB,
		int32 Width, int32 Depth);
```

- [ ] **Step 4: Implement both**

In `PlotFit.cpp`:

```cpp
PlotFit::FPlotGrid PlotFit::BuildGrid(FVector2D FrontageA, FVector2D FrontageB,
	int32 Width, int32 Depth)
{
	FPlotGrid Grid;
	if (Width <= 0 || Depth <= 0)
	{
		return Grid;
	}

	const FVector2D Along = FrontageB - FrontageA;
	const double Length = Along.Size();
	if (Length <= 0.0)
	{
		return Grid;
	}

	const FVector2D Unit = Along / Length;

	// Interior on the LEFT of A->B. The caller owns that convention: the gesture builds the
	// frontage from the road and the side the cursor was on, so there is no winding to
	// discover here the way FitBays has to.
	const FVector2D Inward = RoadGeom::PerpCCW(Unit);
	const double Heading = RoadGeom::Bearing(Inward);

	Grid.Width = Width;
	Grid.Depth = Depth;
	Grid.Slots.Reserve(Width * Depth);

	// ROW-MAJOR, FRONT ROW FIRST - see FPlotGrid. Row is the outer loop.
	for (int32 Row = 0; Row < Depth; ++Row)
	{
		for (int32 Bay = 0; Bay < Width; ++Bay)
		{
			FPlotBay Slot;
			Slot.Centre = FrontageA
				+ Unit * ((static_cast<double>(Bay) + 0.5) * BayWidthUu)
				+ Inward * ((static_cast<double>(Row) + 0.5) * BayDepthUu);
			Slot.Heading = Heading;
			Grid.Slots.Add(Slot);
		}
	}
	return Grid;
}

TArray<FVector2D> PlotFit::GridOutline(FVector2D FrontageA, FVector2D FrontageB,
	int32 Width, int32 Depth)
{
	TArray<FVector2D> Outline;
	if (Width <= 0 || Depth <= 0)
	{
		return Outline;
	}

	const FVector2D Along = FrontageB - FrontageA;
	const double Length = Along.Size();
	if (Length <= 0.0)
	{
		return Outline;
	}

	const FVector2D Unit = Along / Length;
	const FVector2D Inward = RoadGeom::PerpCCW(Unit);

	const FVector2D Front = Unit * (static_cast<double>(Width) * BayWidthUu);
	const FVector2D Back = Inward * (static_cast<double>(Depth) * BayDepthUu);

	// A -> A+Front -> A+Front+Back -> A+Back. Walking the frontage first and then turning
	// INWARD is what makes this counter-clockwise, given Inward is the left normal.
	Outline.Add(FrontageA);
	Outline.Add(FrontageA + Front);
	Outline.Add(FrontageA + Front + Back);
	Outline.Add(FrontageA + Back);
	return Outline;
}
```

- [ ] **Step 5: Build twice, run, lint, commit**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject" -Filter Airside.Solve
./Tools/Check-Architecture.ps1
```

Expected: `PlotGridSlots` passes and the existing `PlotFitBays` / `PlotFitFacesAwayFromRoad`
still do. The lint proves `PlotFit.h` gained no include beyond `CoreMinimal.h`.

```bash
git add Plugins/Airside/Source/Airside/Public/Solve/PlotFit.h Plugins/Airside/Source/Airside/Private/Solve/PlotFit.cpp Plugins/Airside/Source/AirsideTests/Private/PlotFitTest.cpp
git commit -m "feat(solve): a plot is a grid of slots, front row first"
```

---

### Task 2: The readout seam

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/ToolReadout.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h` (two new defaulted virtuals)
- Create: `Plugins/Airside/Source/AirsideTests/Private/ToolReadoutTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `IToolReadoutSink` with `Fact(const FString&, const FString&)`,
  `Warning(const FString&)`, `Committable(bool)`; `FToolReadout { TArray<TPair<FString,FString>> Facts; TArray<FString> Warnings; bool bCommittable; }`
  and `FToolReadoutCollector : IToolReadoutSink` holding one; plus
  `IBuildTool::BuildReadout(const FToolContext&, IToolReadoutSink&) const {}` and
  `IBuildTool::OnCommit(const FToolContext&) {}`.

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tool/ToolReadout.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FToolReadoutCollectorTest,
	"Airside.Tool.ToolReadoutCollector",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FToolReadoutCollectorTest::RunTest(const FString& Parameters)
{
	FToolReadoutCollector Collector;

	// COMMITTABLE DEFAULTS TO FALSE. A tool that emits nothing must not light the Build
	// button - the default has to be the safe answer, because every tool but one will
	// never call Committable at all.
	TestFalse(TEXT("a fresh readout is not committable"), Collector.Readout.bCommittable);

	Collector.Fact(TEXT("Bays"), TEXT("3"));
	Collector.Fact(TEXT("Rows"), TEXT("2"));
	Collector.Warning(TEXT("No room to grow"));
	Collector.Committable(true);

	TestEqual(TEXT("two facts, in the order emitted"), Collector.Readout.Facts.Num(), 2);
	TestEqual(TEXT("the first is the one emitted first"),
		Collector.Readout.Facts[0].Key, FString(TEXT("Bays")));
	TestEqual(TEXT("one warning"), Collector.Readout.Warnings.Num(), 1);
	TestTrue(TEXT("and it is committable now"), Collector.Readout.bCommittable);

	// RESET IS TOTAL, because the collector is refilled every frame and a fact left over
	// from last frame is a readout describing a gesture the player has already changed.
	Collector.Reset();
	TestEqual(TEXT("reset clears the facts"), Collector.Readout.Facts.Num(), 0);
	TestEqual(TEXT("and the warnings"), Collector.Readout.Warnings.Num(), 0);
	TestFalse(TEXT("and takes committable back to false"), Collector.Readout.bCommittable);

	return true;
}

#endif
```

- [ ] **Step 2: Run to verify it fails**

Expected: FAIL — `Cannot open include file: 'Tool/ToolReadout.h'`.

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include "CoreMinimal.h"

/**
 * What a tool tells the player about the gesture in progress - bays, cost, what is wrong.
 *
 * A SECOND SINK, NOT AN EXTENSION OF IToolPreviewSink. That interface's whole contract is to
 * describe intent in ROAD PLANE coordinates naming a MEANING; a bay count is not road-plane
 * geometry, and putting it there would blur the one boundary keeping Tool/ free of
 * presentation.
 *
 * AND NOT STATE THE HUD POLLS. This is filled from a const per-frame call beside the one
 * that draws the preview, so the number the player reads cannot describe a different gesture
 * from the one they are looking at. A GetReadout() the bar pulled would be a second thing
 * that must agree with the preview.
 *
 * STRINGS, NOT NUMBERS. Display-ready values, because the alternative is an enum of fact
 * KINDS the bar switches on - which puts the plugin back in the business of knowing what the
 * bar can render. The plugin still names no widget.
 */
struct AIRSIDE_API IToolReadoutSink
{
	virtual ~IToolReadoutSink() = default;

	/** A named value the player is deciding on: "Bays", "3". */
	virtual void Fact(const FString& Label, const FString& Value) = 0;

	/** Something wrong with the gesture that does not stop it. */
	virtual void Warning(const FString& Text) = 0;

	/**
	 * Whether committing now would succeed - the Build button's enabled state.
	 *
	 * Travels with the facts rather than being asked for separately, because a button lit
	 * while committing would fail is the same drift this sink exists to make impossible.
	 */
	virtual void Committable(bool bCan) = 0;
};

/** One frame's worth, as collected. */
struct AIRSIDE_API FToolReadout
{
	TArray<TPair<FString, FString>> Facts;
	TArray<FString> Warnings;

	/** FALSE BY DEFAULT: every tool but one never calls Committable, and none of them
	 *  should light the Build button by saying nothing. */
	bool bCommittable = false;
};

/** The sink the driver hands to the active tool each frame. */
struct AIRSIDE_API FToolReadoutCollector final : public IToolReadoutSink
{
	FToolReadout Readout;

	/** Refilled every frame - a fact left from last frame describes a gesture the player
	 *  has already changed. */
	void Reset() { Readout = FToolReadout(); }

	virtual void Fact(const FString& Label, const FString& Value) override
	{
		Readout.Facts.Emplace(Label, Value);
	}
	virtual void Warning(const FString& Text) override { Readout.Warnings.Add(Text); }
	virtual void Committable(bool bCan) override { Readout.bCommittable = bCan; }
};
```

- [ ] **Step 4: Add the two defaulted virtuals**

In `RoadBuildTool.h`, in `IBuildTool` beside `BuildPreview`:

```cpp
	/**
	 * What to tell the player about this gesture. Defaulted to silence.
	 *
	 * A SEPARATE VIRTUAL and not a second parameter on BuildPreview, which has eight
	 * implementors and two test doubles - none of which has a readout. Changing that
	 * signature would churn ten classes to add a parameter they ignore. Both calls are const
	 * and are made from the same place on the same frame, so the drift the readout sink
	 * exists to prevent is still prevented; Airside.Tool.PlotReadoutMatchesPreview pins it.
	 */
	virtual void BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const {}

	/**
	 * The player pressed Build. Defaulted to nothing, so no existing tool changes.
	 *
	 * SEPARATE FROM OnClick because the gesture's last click LOCKS rather than commits - the
	 * review beat between the two is where cost and warnings get read.
	 */
	virtual void OnCommit(const FToolContext& Context) {}
```

Add `#include "Tool/ToolReadout.h"` to `RoadBuildTool.h`.

- [ ] **Step 5: Build twice, run, commit**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject" -Filter Airside.Tool
```

Expected: `ToolReadoutCollector` passes; every existing `Airside.Tool.*` still passes,
which is the claim that defaulting both virtuals changed no tool.

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/ToolReadout.h Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h Plugins/Airside/Source/AirsideTests/Private/ToolReadoutTest.cpp
git commit -m "feat(tool): a sink for what a gesture tells the player"
```

---

### Task 3: The frontage becomes an argument

Done before the tool, so the tool has somewhere to commit to.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadEditTarget.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h`, `RoadNetworkActor.h` and their `.cpp`s
- Modify: `Plugins/Airside/Source/AirsideTests/Private/RunwayToolTest.cpp`, `TaxiwayWidthTest.cpp`
- Delete: `FAnchorLink::FindFrontageEdge` from `Build/AnchorLink.h` and `.cpp`
- Delete: `Plugins/Airside/Source/AirsideTests/Private/FrontageEdgeTest.cpp`

**Interfaces:**
- Consumes: Task 1's `PlotFit::GridOutline`.
- Produces: `IRoadEditTarget::PlaceEntityInPlot(const TArray<FVector2D>& Outline, FVector2D FrontageA, FVector2D FrontageB, const TArray<EDepotModule>& Modules, EPlaceableEntity Kind)`.

- [ ] **Step 1: Find every implementor before changing the interface**

```bash
grep -rn "PlaceEntityInPlot" --include=*.h --include=*.cpp Plugins | grep -v Intermediate
```

Expected: four — `URoadEditFacade`, `ARoadNetworkActor`, and the two test doubles. All four
must change or the build breaks.

- [ ] **Step 2: Change the signature and the facade body**

In `RoadEditTarget.h`, replace the declaration:

```cpp
	/**
	 * Drop an installation into a drawn plot whose FRONTAGE THE CALLER ALREADY KNOWS.
	 *
	 * The edge is passed rather than searched for, because a road-snapped rectangle knows
	 * which of its edges is on the road by construction. Searching would be a second opinion
	 * about a fact the gesture already established - and it is why
	 * FAnchorLink::FindFrontageEdge was deleted rather than left looking authoritative.
	 *
	 * FrontageA -> FrontageB must run in Outline's winding order; PlotFit reads the interior
	 * side from that direction.
	 */
	virtual int32 PlaceEntityInPlot(const TArray<FVector2D>& Outline,
		FVector2D FrontageA, FVector2D FrontageB,
		const TArray<EDepotModule>& Modules, EPlaceableEntity Kind) = 0;
```

In `RoadEditFacadeSurfaces.cpp`, delete the `FindFrontageEdge` call and its refusal block,
keeping everything from the winding correction onward. The winding correction STAYS: it is
cheap, and it makes the facade correct for any caller rather than only for a caller that
happens to hand it a counter-clockwise rectangle.

Both test doubles gain:

```cpp
		virtual int32 PlaceEntityInPlot(const TArray<FVector2D>&, FVector2D, FVector2D,
			const TArray<EDepotModule>&, EPlaceableEntity) override { return INDEX_NONE; }
```

- [ ] **Step 3: Delete the search and its test**

```bash
git rm Plugins/Airside/Source/AirsideTests/Private/FrontageEdgeTest.cpp
```

Remove `FindFrontageEdge` from `AnchorLink.h` and `AnchorLink.cpp`. **Leave the census
warning loop at the foot of `Build` alone** — it is a different feature that happens to live
in the same file.

- [ ] **Step 4: Build, run the full suite, commit**

Expected: the suite passes with **two fewer tests** than before (`FrontageEdgeFacesTheRoad`
is gone). Confirm the count moved by exactly that, since a silently vanished test looks
identical to a deleted one.

```bash
git add -A Plugins
git commit -m "refactor(tool): the plot's frontage is given, not searched for"
```

---

### Task 4: The staged tool

The large one.

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/PlotPlaceTool.h`
- Create: `Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp` (the `EKeys::Zero` entry)
- Delete: `Tool/PlotDrawTool.h`, `Private/Tool/PlotDrawTool.cpp`, `AirsideTests/Private/PlotDrawToolTest.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/PlotPlaceToolTest.cpp`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces: `FPlotPlaceTool`, with `EPlotStage { Idle, Width, Depth, Confirm }` and
  `EPlotStage GetStage() const` for tests.

- [ ] **Step 1: Write the failing test**

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/PlotPlaceTool.h"
#include "Tool/ToolReadout.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FToolContext PlotAt(ARoadNetworkActor* Actor, const FVector2D& Where)
	{
		return TestTool::ContextAt(*Actor, Where);
	}

	int32 LiveEntities(const ARoadNetworkActor* Actor)
	{
		int32 Alive = 0;
		for (const FEntityInstance& Entity : Actor->Network->GetEntities())
		{
			if (Entity.bAlive) { ++Alive; }
		}
		return Alive;
	}

	/** A service ROAD as a real segment, so FRoadSnap has something to snap to. Guideline
	 *  edges alone are not enough - the snap works on the road graph, not the guidelines. */
	void LayServiceRoad(ARoadNetworkActor* Actor, double Y)
	{
		IRoadEditTarget* Target = Actor;
		const int32 West = Target->AddNode(FVector2D(-10000.0, Y));
		const int32 East = Target->AddNode(FVector2D(10000.0, Y));
		Target->ConnectNodes(West, East, ERoadKind::ServiceRoad, INDEX_NONE);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotStagesAdvanceTest,
	"Airside.Tool.PlotStagesAdvance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotStagesAdvanceTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	TestEqual(TEXT("a fresh tool is idle"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Idle));

	// Anchor on the road, north side.
	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 200.0)));
	TestEqual(TEXT("the first click anchors and asks for width"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Width));

	// Three bays east.
	Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 200.0)));
	TestEqual(TEXT("the second click locks width and asks for depth"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Depth));

	// Two rows north.
	Tool.OnClick(PlotAt(Actor, FVector2D(600.0, 1600.0)));
	TestEqual(TEXT("the third click locks depth and asks for confirmation"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Confirm));

	// THE FOURTH CLICK BUILDS NOTHING. The review beat is the whole point of the Build
	// button; a click that committed would delete it.
	TestEqual(TEXT("and nothing is built yet"), LiveEntities(Actor), 0);

	Tool.OnCommit(PlotAt(Actor, FVector2D(600.0, 1600.0)));
	TestEqual(TEXT("Build commits exactly one depot"), LiveEntities(Actor), 1);
	TestEqual(TEXT("and the tool returns to idle for the next one"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Idle));

	const TArray<FEntityInstance>& Entities = Actor->Network->GetEntities();
	if (!TestTrue(TEXT("an entity to read"), Entities.Num() > 0)) { return false; }
	TestEqual(TEXT("the plot is a rectangle"), Entities[0].Outline.Num(), 4);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotCancelStepsBackOneStageTest,
	"Airside.Tool.PlotCancelStepsBackOneStage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotCancelStepsBackOneStageTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(600.0, 1600.0)));

	// ONE STAGE AT A TIME, the same answer the outline tool gives to a misclick: binning
	// the whole gesture is harsher than the mistake deserves.
	Tool.OnCancel(PlotAt(Actor, FVector2D(600.0, 1600.0)));
	TestEqual(TEXT("cancel from Confirm goes back to Depth"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Depth));

	Tool.OnCancel(PlotAt(Actor, FVector2D(600.0, 1600.0)));
	TestEqual(TEXT("and again to Width"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Width));

	Tool.OnCancel(PlotAt(Actor, FVector2D(600.0, 1600.0)));
	TestEqual(TEXT("and again to Idle"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Idle));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotAnchorsSnapToBaysTest,
	"Airside.Tool.PlotAnchorsSnapToBays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotAnchorsSnapToBaysTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);

	// A cursor BETWEEN two bay multiples anchors on one of them, not where it was clicked.
	// Without this a plot starts at an arbitrary offset and two depots on the same road can
	// never sit flush - which is what the snap dots are promising the player.
	Tool.OnClick(PlotAt(Actor, FVector2D(517.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(1717.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(1100.0, 1600.0)));
	Tool.OnCommit(PlotAt(Actor, FVector2D(1100.0, 1600.0)));

	if (!TestEqual(TEXT("a depot is built"), LiveEntities(Actor), 1)) { return false; }

	double MinX = TNumericLimits<double>::Max();
	for (const FVector2D& Corner : Actor->Network->GetEntities()[0].Outline)
	{
		MinX = FMath::Min(MinX, Corner.X);
	}
	const double Remainder = FMath::Fmod(FMath::Abs(MinX), PlotFit::BayWidthUu);
	TestTrue(TEXT("the plot starts on a bay multiple, not where the cursor was"),
		Remainder < 1.0 || Remainder > PlotFit::BayWidthUu - 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotCommitsOnlyFromConfirmTest,
	"Airside.Tool.PlotCommitsOnlyFromConfirm",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotCommitsOnlyFromConfirmTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	LayServiceRoad(Actor, 0.0);

	// OnCommit IS REACHABLE FROM THE BAR AT ANY MOMENT - the button is a widget, not a
	// stage of the gesture - so every stage but the last must ignore it. A half-drawn plot
	// committed from the Width stage would have no depth at all.
	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);

	Tool.OnCommit(PlotAt(Actor, FVector2D(0.0, 200.0)));
	TestEqual(TEXT("Build in Idle builds nothing"), LiveEntities(Actor), 0);

	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 200.0)));
	Tool.OnCommit(PlotAt(Actor, FVector2D(0.0, 200.0)));
	TestEqual(TEXT("Build in Width builds nothing"), LiveEntities(Actor), 0);

	Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 200.0)));
	Tool.OnCommit(PlotAt(Actor, FVector2D(1200.0, 200.0)));
	TestEqual(TEXT("Build in Depth builds nothing"), LiveEntities(Actor), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotWidthRunsBothWaysTest,
	"Airside.Tool.PlotWidthRunsBothWays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotWidthRunsBothWaysTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	LayServiceRoad(Actor, 0.0);

	// WEST of the anchor. Dragging back past it must run the plot the other way rather than
	// refusing or collapsing to zero - a player who anchors and then changes their mind
	// about which way to go should not have to cancel.
	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(-1200.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(-600.0, 1600.0)));
	Tool.OnCommit(PlotAt(Actor, FVector2D(-600.0, 1600.0)));

	if (!TestEqual(TEXT("a westward plot is built"), LiveEntities(Actor), 1)) { return false; }

	const FEntityInstance& Depot = Actor->Network->GetEntities()[0];
	double MinX = TNumericLimits<double>::Max();
	for (const FVector2D& Corner : Depot.Outline)
	{
		MinX = FMath::Min(MinX, Corner.X);
	}
	TestTrue(TEXT("and it lies west of the anchor"), MinX < -100.0);

	return true;
}

#endif
```

**`IRoadEditTarget::AddNode` and `ConnectNodes` are used above — confirm their exact
signatures in `Tool/RoadEditTarget.h` before writing this**, and follow whatever
`RoadDrawToolTest.cpp` already does to lay a road in a test rather than inventing a second
way.

- [ ] **Step 2: Run to verify it fails**

Build twice. Expected: FAIL — `Cannot open include file: 'Tool/PlotPlaceTool.h'`.

- [ ] **Step 3: Write the stages**

`PlotPlaceTool.h` declares `EPlotStage`, an `IPlotStage` interface with
`OnClick`/`OnCancel`/`BuildPreview`/`BuildReadout`/`GetStage`, and four classes implementing
it, plus `FPlotPlaceTool : IBuildTool` holding `TUniquePtr<IPlotStage>` and the anchor,
width and depth decided so far.

Follow `OutlineDrawTool.h`'s shape exactly: state objects returning a new state or `nullptr`,
the tool swapping on non-null. Do NOT reuse `FOutlineDrawTool` — the gesture is different and
inheriting it to override three of five methods would be worse than a sibling.

The snap, in the Idle stage:

```cpp
	// THE SHARED SNAP, not a second one. FRoadSnapSettings is the single per-airport tuning
	// struct both drivers build from through MakeTunables, and a second snapper would make
	// the same click behave differently in PIE and in the editor mode - which is exactly the
	// bug that struct's own comment records being fixed.
	const FRoadSnapResult Snap = RoadSnap::Nearest(*Context.Network(), Context.Cursor,
		Context.SnapSettings);
```

**Confirm `RoadSnap`'s entry point and how `FToolContext` carries the settings** by reading
`Tool/RoadSnap.h` and one existing caller — `FRoadDrawTool` snaps on every click and is the
reference. Filter to `ERoadKind::ServiceRoad` segments only.

Anchor candidates are `SegmentT` quantised to `BayWidthUu` along the segment. The side is the
sign of the cursor's offset from the segment tangent at the anchor.

- [ ] **Step 3b: Draw the preview, including which way the sheds face**

Each stage's `BuildPreview`, in existing vocabulary only:

```cpp
	// Anchor candidates, Idle only. Snap style, because they are what the gesture would
	// attach to - the meaning the style names, not a colour.
	for (const FVector2D& Candidate : Candidates) { Sink.Marker(Candidate, EPreviewStyle::Snap); }

	// The plot, and the lines that divide it into slots.
	Sink.Polygon(PlotFit::GridOutline(A, B, Width, Depth), EPreviewStyle::Pending);
	for (each bay and row division) { Sink.Line(From, To, EPreviewStyle::Pending); }

	// WHICH WAY EACH FILLED BAY FACES. Not decoration: +X faces AWAY from the road and the
	// truck leaves out of the back, so a depot built facing the wrong way looks perfectly
	// correct until something drives - the most expensive class of mistake this project has.
	for (int32 I = 0; I < Placed; ++I)
	{
		const PlotFit::FPlotBay& Slot = Grid.Slots[I];
		Sink.CrossMark(Slot.Centre, FVector2D(FMath::Cos(Slot.Heading), FMath::Sin(Slot.Heading)),
			EPreviewStyle::Pending);
	}
```

- [ ] **Step 3c: Refuse an overlapping plot**

Newly reachable and newly checkable: a rectangle can be dragged over a neighbour, and unlike
a freeform outline it can be tested BEFORE the gesture ends.

In the Depth and Confirm stages, test the plot rectangle against every live entity's
`Outline` with `RoadGeom::PolygonsOverlap` — **confirm that helper exists in
`Solve/RoadGeom.h` before using it; if it does not, add it there with its own world-free
test rather than writing an overlap test inside the tool.** On overlap:

```cpp
	Sink.Polygon(Rect, EPreviewStyle::Refused);
	Sink.Label(Context.Cursor, TEXT("overlaps an installation"), EPreviewStyle::Refused);
```

and `Committable(false)` from `BuildReadout`, so the Build button cannot be pressed on it.

- [ ] **Step 4: Swap the registry entry and delete the old tool**

In `BuildSession.cpp`, change the `EKeys::Zero` factory to
`MakeUnique<FPlotPlaceTool>(EPlaceableEntity::FuelDepot)` and update the tooltip to describe
the staged gesture. Update the comment beneath it, which currently describes the freeform
gesture — **that comment is load-bearing and a stale one has already cost this branch a
commit to fix.**

```bash
git rm Plugins/Airside/Source/Airside/Public/Tool/PlotDrawTool.h Plugins/Airside/Source/Airside/Private/Tool/PlotDrawTool.cpp Plugins/Airside/Source/AirsideTests/Private/PlotDrawToolTest.cpp
```

- [ ] **Step 5: Build twice, run the full suite, commit**

Expected: the three new tests pass; `ToolCommandsMatchRegistry` and `BuildSession` still
pass, which is the claim that the two lists that must agree still do.

```bash
git add -A Plugins
git commit -m "feat(tool): a depot plot is snapped to a road and sized in bays"
```

---

### Task 5: The readout the tool emits

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotPlaceToolTest.cpp`

**Interfaces:**
- Consumes: Tasks 2 and 4.
- Produces: no new types — `FPlotPlaceTool::BuildReadout` fills the sink.

- [ ] **Step 1: Write the failing test**

Append to `PlotPlaceToolTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReadoutMatchesPreviewTest,
	"Airside.Tool.PlotReadoutMatchesPreview",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReadoutMatchesPreviewTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);

	// IDLE IS NOT COMMITTABLE. Every stage but the last must say so, or the Build button
	// lights before there is anything to build.
	{
		FToolReadoutCollector Collector;
		Tool.BuildReadout(PlotAt(Actor, FVector2D(0.0, 200.0)), Collector);
		TestFalse(TEXT("idle is not committable"), Collector.Readout.bCommittable);
	}

	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(1200.0, 200.0)));

	// ONE ROW DEEP WARNS, and does not refuse - a one-row depot works perfectly well and
	// the player may want exactly that.
	{
		FToolReadoutCollector Collector;
		Tool.OnClick(PlotAt(Actor, FVector2D(600.0, 800.0)));
		Tool.BuildReadout(PlotAt(Actor, FVector2D(600.0, 800.0)), Collector);

		TestTrue(TEXT("confirm is committable"), Collector.Readout.bCommittable);
		TestEqual(TEXT("and it warns there is no room to grow"),
			Collector.Readout.Warnings.Num(), 1);

		const TPair<FString, FString>* Bays = Collector.Readout.Facts.FindByPredicate(
			[](const TPair<FString, FString>& F) { return F.Key == TEXT("Bays"); });
		if (!TestNotNull(TEXT("a Bays fact"), Bays)) { return false; }
		TestEqual(TEXT("three bays, the width that was dragged"),
			Bays->Value, FString(TEXT("3")));
	}

	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

Expected: FAIL — `bCommittable` is false at Confirm, because `BuildReadout` is still the
base class's empty default.

- [ ] **Step 3: Implement BuildReadout per stage**

Idle emits nothing but `Committable(false)`, plus a `Warning` when the cursor is near no
service road. Width and Depth emit `Bays` and `Rows` so far with `Committable(false)`.
Confirm emits:

```cpp
	Sink.Fact(TEXT("Bays"), FString::FromInt(Width));
	Sink.Fact(TEXT("Rows"), FString::FromInt(Depth));

	// WHAT YOU GET NOW against what you asked for, because a width of two silently dropping
	// the pump is the kind of thing a player discovers after paying for it.
	const int32 Placed = FMath::Min(Width, Modules.Num());
	Sink.Fact(TEXT("Modules"),
		FString::Printf(TEXT("%d of %d"), Placed, Modules.Num()));

	Sink.Fact(TEXT("Expansion slots"), FString::FromInt(Width * Depth - Placed));

	if (Depth <= 1)
	{
		// The direct analogue of Manor Lords' "Plots without Extension Space". A warning and
		// never a refusal.
		Sink.Warning(TEXT("No room to grow"));
	}

	Sink.Committable(true);
```

- [ ] **Step 4: Run to verify it passes, then commit**

```bash
git add -A Plugins
git commit -m "feat(tool): the plot gesture says what it will build before it builds"
```

---

### Task 6: The bar renders it

**Files:**
- Modify: `Source/AirportMgr/RoadBuildController.h` and `.cpp`
- Modify: `Source/AirportMgr/BuildBarWidget.h` and `.cpp`
- Modify: `Source/AirportMgr/BuildActions.h` / its registry
- Modify: `Source/AirportMgr/BuildBarWidgetTest.cpp`

**Interfaces:**
- Consumes: Tasks 2, 4, 5.
- Produces: `ARoadBuildController::GetToolReadout() const` returning `const FToolReadout&`;
  a `Build` entry in `BuildActions()`.

- [ ] **Step 1: Write the failing test**

Create `Source/AirportMgr/PlotReadoutBarTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "BuildActions.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildController.h"
#include "Tool/ToolReadout.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotReadoutReachesTheBarTest,
	"AirportMgr.Actions.PlotReadoutReachesTheBar",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotReadoutReachesTheBarTest::RunTest(const FString& Parameters)
{
	// World-and-controller setup exactly as AirportMgr.Actions.ControllerOwnsCameraAndHud
	// does it - the same degraded path, with no asset and no level.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadBuildController* C = World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	// THE BUILD ACTION IS IN THE REGISTRY, not a bespoke button. The bar builds itself from
	// this list and RegistryIsComplete already guards it, so an action added anywhere else
	// would render nowhere and be tested by nothing.
	const FBuildAction* Build = BuildActions().FindByPredicate(
		[](const FBuildAction& A) { return A.Id == TEXT("Build"); });
	if (!TestNotNull(TEXT("a Build action in the registry"), Build)) { return false; }

	// DISABLED WHEN NOTHING IS COMMITTABLE. A fresh controller has no gesture in progress,
	// so the readout's bCommittable default of false must reach the button - a Build button
	// lit with nothing to build is the failure the sink's own comment names.
	TestFalse(TEXT("nothing to build, so Build is disabled"), Build->IsEnabled(*C));
	TestFalse(TEXT("and the readout says so"), C->GetToolReadout().bCommittable);

	// AND THE COLLECTOR IS REFILLED, not appended to. Two ticks with no gesture must leave
	// the readout empty rather than growing it - a fact from last frame describes a gesture
	// the player has already changed.
	C->CollectToolReadoutForTest();
	C->CollectToolReadoutForTest();
	TestEqual(TEXT("two idle frames leave no facts behind"),
		C->GetToolReadout().Facts.Num(), 0);

	return true;
}

#endif
```

**`FBuildAction::Id` is a guess at the field that names an action** — read `BuildActions.h`
and use whatever it really keys on (it may be a `FName`, a label, or an enum). Do not invent
a field.

`CollectToolReadoutForTest()` is a thin public wrapper around the Tick body from Step 3,
added so this test does not have to tick a whole controller — the same `...ForTest` idiom
`GetHudForTest` and `RebuildCountForTest` already use.

- [ ] **Step 2: Run to verify it fails**

Build twice. Expected: FAIL — no `Build` action in the registry, and no `GetToolReadout`.

- [ ] **Step 3: Collect the readout on the controller**

In `ARoadBuildController::Tick`, after the existing per-frame work:

```cpp
	// COLLECTED HERE AND NOT IN THE HUD, though the HUD is where BuildPreview is called
	// from: a readout is not a drawing concern, and the bar must be able to read it whether
	// or not the world preview is switched on (see bDrawBuildPreview).
	ToolReadoutCollector.Reset();
	if (const IBuildTool* Tool = GetActiveTool())
	{
		Tool->BuildReadout(MakeToolContext(), ToolReadoutCollector);
	}
```

with `FToolReadoutCollector ToolReadoutCollector;` as a member and
`const FToolReadout& GetToolReadout() const { return ToolReadoutCollector.Readout; }`.

**`GetActiveTool()` returns a non-const `IBuildTool*`** — `BuildReadout` is `const`, so this
compiles; do not add a const overload for it.

- [ ] **Step 4: Add the Build action and the readout section**

A `FBuildAction` whose `IsEnabled` returns `Controller.GetToolReadout().bCommittable` and
whose run calls `OnCommit` on the active tool. **In the registry, not as a bespoke button** —
the bar builds itself from `BuildActions()` and `RegistryIsComplete` already guards that list.

`UBuildBarWidget` gains `UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> ReadoutSection;`
filled in `RefreshState` from `GetToolReadout()`. **`BindWidgetOptional`, like every other
section** — a Blueprint that has not been re-saved must degrade to drawing nothing rather
than failing to construct.

- [ ] **Step 5: Build twice, run the full suite, commit**

```bash
git add -A Source Plugins
git commit -m "feat(ui): the bar reads the gesture out, and Build commits it"
```

---

### Task 7: Empty slots are drawn

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp`

**Interfaces:**
- Consumes: Task 1's `PlotFit::BuildGrid`.
- Produces: no new types; `UPlotPresenter::GetEmptySlotCount() const`.

- [ ] **Step 1: Write the failing test**

Append to `PlotPresenterTest.cpp`, reusing its existing `PlaceDepot` helper but with the
outline and module list varied:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotShowsRoomToGrowTest,
	"Airside.Present.PlotShowsRoomToGrow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotShowsRoomToGrowTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	auto PlaceSized = [&](int32 Width, int32 Depth)
	{
		Actor->ClearNetwork();

		const FVector2D A(0.0, 0.0);
		const FVector2D B(Width * PlotFit::BayWidthUu, 0.0);

		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = (A + B) * 0.5;
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = PlotFit::GridOutline(A, B, Width, Depth);
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		Actor->Network->PlaceEntity(Placement);
		Actor->RebuildMesh();
	};

	// THREE WIDE, TWO DEEP holds six slots and three modules, so three stand empty. This
	// is the whole payoff of the depth step before buying exists: a plot that visibly says
	// "three more fit here" rather than three sheds dumped in a corner.
	PlaceSized(3, 2);
	TestEqual(TEXT("a 3x2 plot with three modules has three slots to grow into"),
		Actor->GetPlotPresenter()->GetEmptySlotCount(), 3);

	// ONE ROW DEEP has nowhere to grow, which is what the gesture warned about.
	PlaceSized(3, 1);
	TestEqual(TEXT("a one-row plot has no room to grow"),
		Actor->GetPlotPresenter()->GetEmptySlotCount(), 0);

	// AND A NARROW PLOT DROPS MODULES RATHER THAN OVERFLOWING INTO ROW 2. Two bays across
	// take two modules; the third is not quietly moved to the back where no truck reaches.
	PlaceSized(2, 2);
	TestEqual(TEXT("two bays take two modules, not three"),
		Actor->GetPlotPresenter()->GetEmptySlotCount(), 2);

	return true;
}
```

Add `#include "Solve/PlotFit.h"` to the test's includes.

- [ ] **Step 2: Run to verify it fails**

Expected: FAIL — no `GetEmptySlotCount`.

- [ ] **Step 3: Draw them**

`RebuildFrom` switches from `FitBays` to `BuildGrid`, using the width and depth implied by
the outline, and adds a **flat marker** instance per unfilled slot — a box scaled to bay size
with a height of `10.0` uu, so it reads as a painted outline on the pad rather than a
structure.

```cpp
	// AN EMPTY SLOT IS DRAWN, not left as bare concrete. It is the whole payoff of the depth
	// step before buying exists: a plot that visibly says "three more fit here" is the
	// difference between a yard with capacity and a yard with three sheds dumped in a corner,
	// which is what PIE showed on 2026-09-15.
```

Extend the census line with the empty-slot count.

- [ ] **Step 4: Run to verify it passes, then commit**

```bash
git add -A Plugins
git commit -m "feat(present): a plot shows the slots it has room to grow into"
```

---

## Verifying the iteration, not just the build

After Task 7, in PIE:

1. Press `0`. Move near a service road — anchor dots should appear along it at 4 m intervals.
2. Click to anchor, drag along the road, click to lock the width.
3. Drag away from the road, click to lock the depth.
4. Read the bar: bays, rows, modules, expansion slots, and "No room to grow" at one row.
5. Press **Build**.

`python Tools/Mcp.py log LogAirside` will show the plot census; `shot out.png` captures the
viewport.

What would say the gesture is still wrong: anchors that are hard to hit, a width that fights
the cursor when dragged back past the anchor, or a readout nobody looks at because the
in-world preview already said it. The third is the most likely and the most worth knowing —
it would mean the bar section should be dropped and the facts drawn at the cursor instead.
