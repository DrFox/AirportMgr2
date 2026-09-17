# Four-Point Plot Gesture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the three-click rectangle with a four-point quadrilateral that draws only
what the player has already pinned.

**Architecture:** `FPlotPlaceTool` stops carrying a width and a depth in bays and carries four
`FVector2D` corners with a count of how many are pinned. `BuildPreview` draws strictly by that
count: frontage at one, boundary at two, contents at three. Two new `EPreviewStyle` values
carry "pinned" and "provisional" so the plugin still names meaning and `ARoadBuildHUD` still
owns the look.

**Tech Stack:** UE 5.8 C++, `IToolPreviewSink`/`IToolReadoutSink`, `RoadGeom` for polygon
maths, `IMPLEMENT_SIMPLE_AUTOMATION_TEST`.

**Spec:** `docs/superpowers/specs/2026-09-16-four-point-plot-gesture-design.md`

## Global Constraints

- **The editor must be CLOSED to build.** `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat
  AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject"
  -WaitMutex -NoHotReloadFromIDE`. `-NoHotReloadFromIDE` is correct HERE because this is a
  worktree; never pass it on `C:\repos\AirportMgr2`.
- **A new test .cpp needs two builds.** The first reports `Result: Succeeded` without
  compiling it.
- **Never trust the runner's exit code.** Read `N test(s) run, N failed, N crashed` from
  `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject"`.
- **`Check-Architecture.ps1` is the pre-commit lint** and runs first inside the test script.
- **`Solve/` headers include `CoreMinimal.h` and nothing else.** `Tool/` may include `Build/`
  and `Solve/` but never `Present/`.
- **Frontage quantum: minimum 15 m (1500 uu), then 5 m (500 uu) steps.** Back corners are NOT
  quantised.
- **Comments explain WHY**, especially why an obvious alternative was rejected. Match the
  surrounding density.
- **Tests assert behaviour with a named reason.** Test names are distinct leaves under
  `Airside.Tool.` / `AirportMgr.HUD.`: UE's automation tree DROPS a bare-named test once a
  dotted child of that name exists.
- Do not add a `Co-Authored-By` trailer to commit messages.

---

### Task 1: Pinned and Provisional, and a dashed line to draw them with

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h` (the `EPreviewStyle` enum)
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PreviewPalette.cpp`
- Modify: `Source/AirportMgr/RoadBuildHUD.cpp` (the constructor's seeding list, and `Line`)
- Modify: `Source/AirportMgr/RoadBuildHUD.h`
- Modify: `Source/AirportMgr/RoadBuildHUDTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `EPreviewStyle::Pinned`, `EPreviewStyle::Provisional`. Any sink may now receive
  them; `ARoadBuildHUD` draws `Provisional` dashed.

**THREE LISTS MUST AGREE and only one of them fails loudly.** A style needs an entry in the
enum, a case in `PreviewPalette::DefaultLook`, and a slot in `ARoadBuildHUD`'s constructor
seeding loop. `AirportMgr.HUD.LooksCoverEveryStyle` walks the enum to `ServiceAnchor` and
checks the map, so it catches the third - **but only if its loop bound is updated too**, which
is the fourth list. Add the new members AFTER `ServiceAnchor` and change the test's bound to
the new last member in the same edit.

- [ ] **Step 1: Write the failing test**

In `Source/AirportMgr/RoadBuildHUDTest.cpp`, change the existing loop bound in
`FRoadBuildHUDLooksTest` from `EPreviewStyle::ServiceAnchor` to `EPreviewStyle::Provisional`,
and append this test:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPinnedAndProvisionalReadApartTest,
	"AirportMgr.HUD.PinnedAndProvisionalReadApart",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPinnedAndProvisionalReadApartTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadBuildHUD* Hud = World->SpawnActor<ARoadBuildHUD>();
	if (!TestNotNull(TEXT("the hud"), Hud)) { return false; }

	// THE TWO MUST BE TELLABLE APART AT A GLANCE, because that is their whole job: one edge
	// has stopped moving and the other has not. Identical looks would leave the player
	// reading a dashed boundary as decoration, which is exactly the verdict the marks this
	// replaces earned.
	const FPreviewLook& Pinned = Hud->LookForTest(EPreviewStyle::Pinned);
	const FPreviewLook& Provisional = Hud->LookForTest(EPreviewStyle::Provisional);

	TestTrue(TEXT("pinned has a positive thickness"), Pinned.ThicknessScale > 0.0f);
	TestTrue(TEXT("provisional has a positive thickness"), Provisional.ThicknessScale > 0.0f);

	// SAME WEIGHT, so the dash is what distinguishes them rather than a thickness the player
	// has to compare against a line elsewhere on screen.
	TestEqual(TEXT("both are drawn at the same weight"),
		Pinned.ThicknessScale, Provisional.ThicknessScale);

	TestTrue(TEXT("and the hud knows to dash one of them and not the other"),
		ARoadBuildHUD::IsDashed(EPreviewStyle::Provisional)
			&& !ARoadBuildHUD::IsDashed(EPreviewStyle::Pinned));

	return true;
}
```

- [ ] **Step 2: Build twice, run, verify it fails**

Expected: FAIL to compile — `EPreviewStyle::Pinned` does not exist. That is the red step.

- [ ] **Step 3: Add the two styles**

In `RoadBuildTool.h`, after `ServiceAnchor,` and inside the enum:

```cpp
	// --- The staged plot gesture's own two -----------------------------------------------
	//
	// PINNED AND PROVISIONAL SAY WHAT Pending CANNOT: whether the thing under them has
	// stopped moving. Pending means "this is what the click would do", which is true of an
	// edge being dragged AND of one already placed - and the player needs to tell those
	// apart to know how many corners are left.
	//
	// ADDED AT THE END, not beside Pending where they read better: this is a UENUM, and
	// renumbering it would silently repoint any value already serialised against it.

	/** An edge the player has already placed. It will not move again this gesture. */
	Pinned,

	/** An edge that follows the cursor. Drawn dashed - see ARoadBuildHUD::IsDashed. */
	Provisional,
```

In `PreviewPalette.cpp`'s colour switch, beside the existing cases:

```cpp
	// White, both of them, and deliberately not the palette's greens: these say "decided" and
	// "not yet", which is a statement about the GESTURE rather than about whether the thing
	// is good or bad. Manor Lords draws its plot boundary white for the same reason.
	case EPreviewStyle::Pinned:                      return FLinearColor(1.0f, 1.0f, 1.0f);
	case EPreviewStyle::Provisional:                 return FLinearColor(1.0f, 1.0f, 1.0f);
```

And in the same file's look-tuning switch, where `bDoubleRing` is set per style, give both a
heavier line so a plot boundary reads over grass:

```cpp
	case EPreviewStyle::Pinned:      Look.ThicknessScale = 2.0f; break;
	case EPreviewStyle::Provisional: Look.ThicknessScale = 2.0f; break;
```

**Read `PreviewPalette.cpp` before writing these** — it has two switches (colour, then look)
and the second one's exact shape is not quoted here. Add a case to whichever switches exist;
a style missing from either is what `LooksCoverEveryStyle` catches.

In `RoadBuildHUD.cpp`'s constructor seeding list, add both to the braced initialiser:

```cpp
		EPreviewStyle::ServiceAnchor, EPreviewStyle::Pinned, EPreviewStyle::Provisional })
```

- [ ] **Step 4: Dash the provisional line**

In `RoadBuildHUD.h`, public:

```cpp
	/**
	 * Whether this style is drawn as a dashed line.
	 *
	 * STATIC AND PUBLIC so the one rule is testable without a Canvas. The PLUGIN never names
	 * a dash length - it names a meaning, and this is where the meaning becomes a look, in
	 * the same class that turns a style into a colour.
	 */
	static bool IsDashed(EPreviewStyle Style) { return Style == EPreviewStyle::Provisional; }
```

In `RoadBuildHUD.cpp`, `Line` gains a dashed path. Replace its single `DrawLine` call with:

```cpp
	if (!IsDashed(Style))
	{
		DrawLine(
			static_cast<float>(ScreenA.X), static_cast<float>(ScreenA.Y),
			static_cast<float>(ScreenB.X), static_cast<float>(ScreenB.Y),
			Look.Colour, Weight);
		return;
	}

	// DASHED IN SCREEN SPACE, not world space. A world-space dash shortens with distance
	// until a far edge reads as a solid line, which is the one thing the dash exists to deny.
	// Same reason CrossMark takes its length from the overlay rather than from the tool.
	const FVector2D Span = ScreenB - ScreenA;
	const double Length = Span.Size();
	if (Length <= 0.0)
	{
		return;
	}

	const FVector2D Unit = Span / Length;
	for (double Along = 0.0; Along < Length; Along += DashPitch)
	{
		const FVector2D From = ScreenA + Unit * Along;
		const FVector2D To = ScreenA + Unit * FMath::Min(Along + DashPitch * 0.5, Length);
		DrawLine(static_cast<float>(From.X), static_cast<float>(From.Y),
			static_cast<float>(To.X), static_cast<float>(To.Y), Look.Colour, Weight);
	}
```

with, in `RoadBuildHUD.h` beside `PreviewThickness`:

```cpp
	/** Dash plus gap, in pixels. Half is drawn, half is skipped. */
	UPROPERTY(EditAnywhere, Category = "Airside|Preview", meta = (ClampMin = "4.0"))
	float DashPitch = 18.0f;
```

- [ ] **Step 5: Build twice, run, verify it passes**

```bash
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject" -Filter AirportMgr.HUD
```

Expected: `LooksCoverEveryStyle` and `PinnedAndProvisionalReadApart` both PASS. If
`LooksCoverEveryStyle` fails, a switch is missing a case — that is the test doing its job.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h `
        Plugins/Airside/Source/Airside/Private/Tool/PreviewPalette.cpp `
        Source/AirportMgr/RoadBuildHUD.h Source/AirportMgr/RoadBuildHUD.cpp `
        Source/AirportMgr/RoadBuildHUDTest.cpp
git commit -m "feat(present): an edge can say whether it has stopped moving"
```

---

### Task 2: The tool carries four corners

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/PlotPlaceTool.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotPlaceToolTest.cpp`

**Interfaces:**
- Consumes: Task 1's styles (used in Task 3, not here).
- Produces: `EPlotStage` becomes `{ Idle, Frontage, CornerA, CornerB, Confirm }`;
  `FPlotPlaceTool::PinnedCount() const` returning `int32` 0-4;
  `FPlotPlaceTool::Quad(const FToolContext&, TArray<FVector2D>& OutQuad) const` filling the
  four corners as they stand THIS frame, in CCW order with the frontage first.

- [ ] **Step 1: Write the failing tests**

Append to `PlotPlaceToolTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPinsOneCornerAtATimeTest,
	"Airside.Tool.PlotPinsOneCornerAtATime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPinsOneCornerAtATimeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	TestEqual(TEXT("a fresh tool has pinned nothing"), Tool.PinnedCount(), 0);

	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	TestEqual(TEXT("the first click pins the anchor"), Tool.PinnedCount(), 1);

	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 200.0)));
	TestEqual(TEXT("the second pins the frontage"), Tool.PinnedCount(), 2);

	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 1800.0)));
	TestEqual(TEXT("the third pins a back corner"), Tool.PinnedCount(), 3);

	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 1500.0)));
	TestEqual(TEXT("the fourth pins the last corner"), Tool.PinnedCount(), 4);
	TestEqual(TEXT("and the gesture is ready to commit"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::Confirm));

	// THE LAST CLICK LOCKS, IT DOES NOT BUILD. The review beat is the whole point; a click
	// that committed would delete it.
	TestEqual(TEXT("and nothing was built by pinning it"), LiveEntities(Actor), 0);

	// AND CANCEL WALKS BACK ONE AT A TIME, which is the answer a misclick deserves.
	Tool.OnCancel(PlotAt(Actor, FVector2D(0.0, 1500.0)));
	TestEqual(TEXT("cancel unpins the last corner"), Tool.PinnedCount(), 3);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotFrontageSnapsInFiveMetreStepsTest,
	"Airside.Tool.PlotFrontageSnapsInFiveMetreSteps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotFrontageSnapsInFiveMetreStepsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	auto FrontageFor = [&](double CursorX)
	{
		FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
		Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));

		TArray<FVector2D> Quad;
		Tool.Quad(PlotAt(Actor, FVector2D(CursorX, 200.0)), Quad);
		return Quad.Num() >= 2 ? FVector2D::Distance(Quad[0], Quad[1]) : 0.0;
	};

	// 17 m ROUNDS, it does not truncate and it does not stay at 17. Rounding is what makes a
	// grid feel magnetic rather than grudging - see WidthAt's own comment, which this keeps.
	TestEqual(TEXT("a 17 m drag locks at 15 m"), FrontageFor(1700.0), 1500.0);
	TestEqual(TEXT("an 18 m drag locks at 20 m"), FrontageFor(1800.0), 2000.0);
	TestEqual(TEXT("a 23 m drag locks at 25 m"), FrontageFor(2300.0), 2500.0);

	// THE FLOOR IS 15 m AND IT IS A FLOOR, not a step. A yard narrower than that is not a
	// yard, so a cursor 3 m along still asks for the smallest plot there is rather than one
	// the depot could not use.
	TestEqual(TEXT("a 3 m drag still asks for the 15 m minimum"), FrontageFor(300.0), 1500.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotBackCornersAreFreeTest,
	"Airside.Tool.PlotBackCornersAreFree",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotBackCornersAreFreeTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 200.0)));

	// AN ARBITRARY DEPTH, deliberately not a multiple of anything. The frontage is quantised
	// because it must tile with the plot next door; a back corner is shared with nothing, and
	// snapping it would refuse shapes the ground calls for.
	TArray<FVector2D> Quad;
	Tool.Quad(PlotAt(Actor, FVector2D(2000.0, 1737.0)), Quad);
	if (!TestEqual(TEXT("a quad has four corners"), Quad.Num(), 4)) { return false; }

	// Corner 2 is the one the cursor is placing - the back corner at the far end of the
	// frontage. Its depth is whatever was asked for.
	TestTrue(TEXT("the back corner keeps the depth the cursor asked for"),
		FMath::IsNearlyEqual(Quad[2].Y, 1737.0, 1.0));

	// AND THE TWO RULES ARE DEMONSTRABLY DIFFERENT, not accidentally the same: the frontage
	// in the same quad did snap.
	TestEqual(TEXT("while the frontage in the same quad snapped to 20 m"),
		FVector2D::Distance(Quad[0], Quad[1]), 2000.0);

	return true;
}
```

Delete `FPlotWidthRunsBothWaysTest` and `FPlotGhostAgreesWithTheBarTest` from this file: they
assert against `Width`/`Depth` in bays, which stop existing. **What they pinned is not lost** —
`PlotPinsOneCornerAtATime` covers the stage walk, and Task 3's
`Airside.Tool.PlotGhostAgreesWithTheReadout` rewrites the ghost-agrees claim against corners on
the SECOND gesture of a session, which is where the original caught a stale depth.

**Add the either-way assertion to `PlotFrontageSnapsInFiveMetreSteps`**, so the rule
`FPlotWidthRunsBothWaysTest` owned keeps a home rather than quietly lapsing:

```cpp
	// DRAGGED BACK PAST THE ANCHOR RUNS THE PLOT THE OTHER WAY rather than refusing or
	// collapsing to nothing. A player who anchors and then changes their mind about which way
	// to go should not have to cancel and start again.
	TestEqual(TEXT("dragging west of the anchor still gives a 20 m frontage"),
		FrontageFor(-1800.0), 2000.0);
```

- [ ] **Step 2: Build twice, run, verify they fail**

Expected: FAIL to compile — `PinnedCount` and `Quad` do not exist, and `EPlotStage::Frontage`
does not either.

- [ ] **Step 3: Replace the stage enum and the state**

In `PlotPlaceTool.h`, the enum becomes:

```cpp
UENUM()
enum class EPlotStage : uint8
{
	/** Nothing pinned. The cursor hunts for a service road. */
	Idle,

	/** The anchor is pinned. The cursor runs along the road setting the frontage. */
	Frontage,

	/** The frontage is pinned. The cursor places the back corner at its far end. */
	CornerA,

	/** Three corners pinned. The cursor places the last one. */
	CornerB,

	/** Four corners. Nothing moves until Build, or until Cancel steps back. */
	Confirm
};
```

and the state becomes:

```cpp
	/**
	 * The corners, in the order they are pinned: anchor, far frontage end, far back, near
	 * back. Entries past PinnedCount() are stale and must not be read - Quad() rebuilds the
	 * moving one from the cursor every frame rather than leaving it to be trusted.
	 */
	FVector2D Corners[4] = { FVector2D::ZeroVector, FVector2D::ZeroVector,
		FVector2D::ZeroVector, FVector2D::ZeroVector };
```

Delete `int32 Width` and `int32 Depth`, and delete `WidthAt`, `DepthAt`, `ShownSize`,
`ShownPlot` and `Frontage` — `Quad` replaces all five.

Add to the public section:

```cpp
	/** How many corners the player has placed, 0 to 4. What the readout reports as "N/4". */
	int32 PinnedCount() const;

	/**
	 * The plot as it stands THIS frame: pinned corners as placed, the moving one from the
	 * cursor, in the outline's own winding with the frontage as edge 0->1.
	 *
	 * ONE DERIVATION, EVERY CALLER. The preview draws it, the readout measures it and
	 * OnCommit builds from it, so the ghost, the facts and the built thing cannot describe
	 * three different shapes. Fewer than two pinned gives fewer than four corners out.
	 */
	void Quad(const FToolContext& Context, TArray<FVector2D>& OutQuad) const;
```

- [ ] **Step 4: Implement the quantum and the quad**

In `PlotPlaceTool.cpp`'s anonymous namespace:

```cpp
	/** 15 m. A yard narrower than this is not a yard - see the design doc section 2. */
	constexpr double MinFrontageUu = 1500.0;

	/** 5 m. The step above the minimum, because the frontage must tile with its neighbour. */
	constexpr double FrontageStepUu = 500.0;

	/**
	 * A frontage length quantised to the plot's own steps.
	 *
	 * ROUNDED, NOT FLOORED, which is the difference between a grid that feels magnetic and
	 * one that feels grudging: floored, the cursor must travel a whole further step before
	 * the plot grows, so it always lags behind the hand.
	 *
	 * NOT PlotFit::BayWidthUu. That is 4 m because a SHED is 4 m, and it used to mean the
	 * plot's step as well - one number doing two jobs, which is why a plot could be drawn
	 * narrower than anything could stand in.
	 */
	double QuantisedFrontage(double Raw)
	{
		if (Raw <= MinFrontageUu)
		{
			return MinFrontageUu;
		}
		const double Steps = FMath::RoundToDouble((Raw - MinFrontageUu) / FrontageStepUu);
		return MinFrontageUu + Steps * FrontageStepUu;
	}
```

`PinnedCount` reads straight off the stage, so the two cannot disagree:

```cpp
int32 FPlotPlaceTool::PinnedCount() const
{
	switch (Stage)
	{
	case EPlotStage::Idle:     return 0;
	case EPlotStage::Frontage: return 1;
	case EPlotStage::CornerA:  return 2;
	case EPlotStage::CornerB:  return 3;
	case EPlotStage::Confirm:  return 4;
	}
	return 0;
}
```

`Quad` fills the corners as they stand, taking the moving one from the cursor:

```cpp
void FPlotPlaceTool::Quad(const FToolContext& Context, TArray<FVector2D>& OutQuad) const
{
	OutQuad.Reset();

	const int32 Pinned = PinnedCount();
	if (Pinned < 1)
	{
		return;
	}

	// Corner 0 is always the anchor, pinned at the first click and never moving after.
	OutQuad.Add(Corners[0]);

	// Corner 1: the far end of the frontage. Along the road, quantised, and it may run
	// EITHER WAY - a player who anchors and then changes their mind about direction should
	// not have to cancel and start again.
	FVector2D Far = Corners[1];
	if (Pinned == 1)
	{
		const double Reach = FVector2D::DotProduct(Context.Cursor - Corners[0], Along);
		const double Signed = Reach < 0.0 ? -1.0 : 1.0;
		Far = Corners[0] + Along * (Signed * QuantisedFrontage(FMath::Abs(Reach)));
	}
	OutQuad.Add(Far);

	if (Pinned < 2)
	{
		return;
	}

	// Corners 2 and 3: the back pair, free of any quantum. Depth is measured along the inward
	// normal FROM THE ANCHOR, so dragging depth does not slide a corner sideways along road.
	auto DepthAtCursor = [&]()
	{
		return FMath::Max(FVector2D::DotProduct(Context.Cursor - Corners[0], Inward), 0.0);
	};
	auto DepthOf = [&](const FVector2D& Corner)
	{
		return FVector2D::DotProduct(Corner - Corners[0], Inward);
	};

	const double DepthFar = Pinned == 2 ? DepthAtCursor() : DepthOf(Corners[2]);
	OutQuad.Add(Far + Inward * DepthFar);

	// UNTIL IT IS REACHED, THE NEAR CORNER MIRRORS THE FAR ONE, so two pinned corners read as
	// a rectangle the player then adjusts rather than as an open shape trailing off.
	//
	// IT MUST NOT READ Corners[3] HERE: that entry is stale until the fourth click writes it,
	// and drawing a stale corner is how a ghost shows the PREVIOUS gesture's geometry - the
	// exact bug the old Depth member caused, which took a deliberate re-break to prove.
	double DepthNear = DepthFar;
	if (Pinned == 3)
	{
		DepthNear = DepthAtCursor();
	}
	else if (Pinned >= 4)
	{
		DepthNear = DepthOf(Corners[3]);
	}
	OutQuad.Add(Corners[0] + Inward * DepthNear);
}
```

**`Along` and `Inward` keep their current meaning and their comments** — set at the anchor
click from the road's direction and the side the cursor was on. Do not change them.

**`AnchorIndexAt` must change its step from `PlotFit::BayWidthUu` to `FrontageStepUu`.** It
quantises where along the road the anchor lands, and leaving it on the 4 m shed width would let
a plot start on a 4 m grid while its frontage grows in 5 m steps — so two plots drawn side by
side would never sit flush, which is the entire reason the frontage has a quantum at all. This
is the other half of "4 m stops being the plot's unit". The idle preview's dot spacing follows
automatically, because the preview and `OnClick` already read the same function.

`OnClick` advances a stage and records the corner `Quad` just produced, so a click always pins
exactly what was on screen:

```cpp
	case EPlotStage::Frontage:
	{
		TArray<FVector2D> Shown;
		Quad(Context, Shown);
		if (Shown.Num() < 2)
		{
			return;
		}
		Corners[1] = Shown[1];
		Stage = EPlotStage::CornerA;
		return;
	}

	case EPlotStage::CornerA:
	{
		TArray<FVector2D> Shown;
		Quad(Context, Shown);
		if (Shown.Num() < 3)
		{
			return;
		}
		Corners[2] = Shown[2];
		Stage = EPlotStage::CornerB;
		return;
	}

	case EPlotStage::CornerB:
	{
		TArray<FVector2D> Shown;
		Quad(Context, Shown);
		if (Shown.Num() < 4)
		{
			return;
		}

		// REFUSED AT THE CLICK THAT WOULD MAKE IT, not at commit - the player is never left
		// holding a shape that cannot be built. PlaceEntityInPlot asks the same question and
		// keeps asking it; this is earlier, not instead.
		if (!RoadGeom::IsSimplePolygon(Shown))
		{
			return;
		}
		Corners[3] = Shown[3];
		Stage = EPlotStage::Confirm;
		return;
	}
```

`OnCancel` walks back one stage, as it already does — update its switch for the new names.

`OnCommit` builds from `Quad` rather than `GridOutline`:

```cpp
	TArray<FVector2D> Outline;
	Quad(Context, Outline);
	if (Outline.Num() < 4)
	{
		return;
	}
	Context.Target->PlaceEntityInPlot(Outline, Outline[0], Outline[1], Modules, Kind);
```

`YardFor` takes the outline instead of a width and depth:

```cpp
PlotYard::FYard FPlotPlaceTool::YardFor(TArrayView<const FVector2D> Outline) const
```

with its body unchanged except that it uses `Outline` directly rather than calling
`PlotFit::GridOutline`, and takes `FrontA`/`FrontB` as `Outline[0]`/`Outline[1]`.

- [ ] **Step 5: Run to verify they pass**

```bash
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject" -Filter Airside.Tool
```

Expected: the three new tests PASS. `PlotStagesAdvance`, `PlotCancelStepsBackOneStage`,
`PlotCommitsOnlyFromConfirm`, `PlotAnchorsSnapToBays`, `PlotIgnoresATaxiway` and
`PlotReadoutMatchesPreview` all reference the old stages or bay widths — update them to the
new stage names and the 15 m/5 m quantum as part of this step. **Do not delete any of them**:
each pins a rule that survives the shape change.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/PlotPlaceTool.h `
        Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotPlaceToolTest.cpp
git commit -m "feat(tool): a plot is four corners, pinned one at a time"
```

---

### Task 3: Draw only what is pinned

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp` (`BuildPreview`)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotPlaceToolTest.cpp`

**Interfaces:**
- Consumes: Task 1's `EPreviewStyle::Pinned` / `Provisional`, Task 2's `Quad` and
  `PinnedCount`.
- Produces: no new types.

- [ ] **Step 1: Write the failing test**

Append to `PlotPlaceToolTest.cpp`. The recording sink `FPlotGhostSink` already exists in this
file and already counts `Lines`, `CrossMarks` and `MarkerStyles`; add a style tally for lines:

```cpp
		TArray<EPreviewStyle> LineStyles;
```

set in its `Line` override with `LineStyles.Add(Style)` (the parameter is currently unnamed —
name it), and a helper beside `MarkersOf`:

```cpp
		int32 LinesOf(EPreviewStyle Style) const
		{
			int32 Count = 0;
			for (const EPreviewStyle S : LineStyles)
			{
				if (S == Style) { ++Count; }
			}
			return Count;
		}
```

Then:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotDrawsOnlyWhatIsPinnedTest,
	"Airside.Tool.PlotDrawsOnlyWhatIsPinned",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotDrawsOnlyWhatIsPinnedTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);

	// ONE POINT: the frontage, solid, and NOTHING ELSE. At one corner the shape is not
	// decided, so a boundary drawn here is a promise the next click breaks - which is the
	// whole reason this document exists.
	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	{
		FPlotGhostSink Sink;
		Tool.BuildPreview(PlotAt(Actor, FVector2D(2000.0, 200.0)), Sink);

		TestEqual(TEXT("one pinned corner draws one solid edge"),
			Sink.LinesOf(EPreviewStyle::Pinned), 1);
		TestEqual(TEXT("and no provisional boundary at all"),
			Sink.LinesOf(EPreviewStyle::Provisional), 0);
		TestEqual(TEXT("and no module footprints"),
			Sink.LinesOf(EPreviewStyle::Pending), 0);
	}

	// TWO POINTS: a boundary appears, dashed, because the corner making it is still moving.
	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 200.0)));
	{
		FPlotGhostSink Sink;
		Tool.BuildPreview(PlotAt(Actor, FVector2D(2000.0, 1800.0)), Sink);

		TestEqual(TEXT("the pinned frontage is still solid"),
			Sink.LinesOf(EPreviewStyle::Pinned), 1);
		TestTrue(TEXT("and the rest of the boundary is provisional"),
			Sink.LinesOf(EPreviewStyle::Provisional) > 0);

		// STILL NO CONTENTS. Both back corners are unknown, so the plot has no settled depth
		// anywhere and anything drawn inside it would move on the next two clicks.
		TestEqual(TEXT("two corners is too early to promise what fits"),
			Sink.LinesOf(EPreviewStyle::Pending), 0);
	}

	// THREE POINTS: only the last corner moves, so what is drawn inside is a promise the
	// gesture can keep.
	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 1800.0)));
	{
		FPlotGhostSink Sink;
		Tool.BuildPreview(PlotAt(Actor, FVector2D(0.0, 1500.0)), Sink);

		TestTrue(TEXT("three corners is when the contents appear"),
			Sink.LinesOf(EPreviewStyle::Pending) > 0);

		// AND THEY ARE THE MODULES, four lines each - the same footprints the readout counts.
		TestEqual(TEXT("the contents are whole footprints, four lines apiece"),
			Sink.LinesOf(EPreviewStyle::Pending) % 4, 0);
	}

	return true;
}
```

- [ ] **Step 2: Build twice, run, verify it fails**

Expected: FAIL. `BuildPreview` currently draws the whole outline in `Pending` at every stage,
so `LinesOf(Pinned)` is 0 and the one-point case fails first.

- [ ] **Step 3: Draw by pinned count**

Replace `BuildPreview`'s body after the Idle branch with:

```cpp
	TArray<FVector2D> Shown;
	Quad(Context, Shown);
	if (Shown.Num() < 2)
	{
		return;
	}

	// EVERY CORNER ALREADY PLACED gets a dot, so "Plot Points: 2/4" has something to count
	// against on screen rather than being a number the player has to trust.
	const int32 Pinned = PinnedCount();
	for (int32 I = 0; I < Pinned && I < Shown.Num(); ++I)
	{
		Sink.Marker(Shown[I], EPreviewStyle::Pinned);
	}

	// THE FRONTAGE IS PINNED FROM THE SECOND CLICK ON. At one corner it is still following
	// the cursor, so it is drawn solid only once it has stopped moving.
	Sink.Line(Shown[0], Shown[1],
		Pinned >= 2 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional);

	if (Shown.Num() < 4)
	{
		return;
	}

	// The rest of the boundary. An edge is PINNED when both its ends are, and provisional
	// the moment either is still under the cursor.
	Sink.Line(Shown[1], Shown[2], Pinned >= 3 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional);
	Sink.Line(Shown[2], Shown[3], Pinned >= 4 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional);
	Sink.Line(Shown[3], Shown[0], Pinned >= 4 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional);

	// CONTENTS AT THREE CORNERS, not two. With two pinned both back corners are unknown and
	// the plot has no settled depth anywhere; with three, only one corner moves, so these
	// footprints are what the player gets unless they move that one. See the design doc's
	// "Contents appear at three points, not two".
	if (Pinned < 3)
	{
		return;
	}

	const PlotYard::FYard Yard = YardFor(Shown);

	TArray<FVector2D> Corners;
	for (int32 I = 0; I < Yard.Stands.Num() && I < Modules.Num(); ++I)
	{
		if (!Yard.Stands[I].bPlaced)
		{
			continue;
		}
		PlotYard::StandCorners(Yard.Stands[I], DepotFootprint(Modules[I]), Corners);
		Sink.Polygon(Corners, EPreviewStyle::Pending);
	}
```

- [ ] **Step 4: Write the refusal and the agreement tests**

Both are listed in the spec's §9 and neither is covered by the steps above. Append to
`PlotPlaceToolTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotRefusesACrossedQuadTest,
	"Airside.Tool.PlotRefusesACrossedQuad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotRefusesACrossedQuadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);
	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(2000.0, 1800.0)));
	if (!TestEqual(TEXT("three corners are down"), Tool.PinnedCount(), 3)) { return false; }

	// A LAST CORNER PAST THE FAR END folds the quad: corner 3 lands beyond corner 2 along the
	// road, so edge 2->3 doubles back across edge 3->0.
	Tool.OnClick(PlotAt(Actor, FVector2D(4000.0, 1800.0)));

	// REFUSED AT THE CLICK, not at commit - the player is never left holding a shape that
	// cannot be built and can only escape by cancelling.
	TestEqual(TEXT("a crossed quad does not pin"), Tool.PinnedCount(), 3);
	TestEqual(TEXT("and the gesture stays on the last corner"),
		static_cast<int32>(Tool.GetStage()), static_cast<int32>(EPlotStage::CornerB));

	// AND A LEGAL LAST CORNER STILL PINS, or this test would pass on a tool that refused
	// everything - which is the shape a refusal test fails in.
	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 1500.0)));
	TestEqual(TEXT("but a legal one is accepted"), Tool.PinnedCount(), 4);

	return true;
}
```

**The crossing position above is a guess.** If it pins anyway, `PinnedCount()` comes back 4
and the test fails loudly — then find a position that genuinely crosses by printing the quad
from `Quad()` and running `RoadGeom::IsSimplePolygon` over it. Do NOT weaken the assertion to
match whatever the tool happens to do.

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotGhostAgreesWithTheReadoutTest,
	"Airside.Tool.PlotGhostAgreesWithTheReadout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotGhostAgreesWithTheReadoutTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

	Actor->ClearNetwork();
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	LayServiceRoad(Actor, 0.0);

	FPlotPlaceTool Tool(EPlaceableEntity::FuelDepot);

	// A FIRST GESTURE, BACKED OUT OF. This test's predecessor caught a ghost drawing the
	// PREVIOUS gesture's depth, and could only catch it on the SECOND gesture of a session:
	// the first leaves every member at its initial value, where stale and correct agree.
	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(3000.0, 200.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(3000.0, 2600.0)));
	Tool.OnClick(PlotAt(Actor, FVector2D(0.0, 2600.0)));
	for (int32 I = 0; I < 4; ++I)
	{
		Tool.OnCancel(PlotAt(Actor, FVector2D(0.0, 2600.0)));
	}
	if (!TestEqual(TEXT("backed all the way out"), Tool.PinnedCount(), 0)) { return false; }

	// The second gesture, mid-frontage: the stage where a stale corner would show.
	Tool.OnClick(OnRoad(Actor, FVector2D(0.0, 200.0)));
	const FToolContext Dragging = PlotAt(Actor, FVector2D(1600.0, 200.0));

	FPlotGhostSink Sink;
	Tool.BuildPreview(Dragging, Sink);

	FToolReadoutCollector Collector;
	Tool.BuildReadout(Dragging, Collector);

	const TPair<FString, FString>* Frontage = Collector.Readout.Facts.FindByPredicate(
		[](const TPair<FString, FString>& F) { return F.Key == TEXT("Frontage"); });
	if (!TestNotNull(TEXT("a Frontage fact"), Frontage)) { return false; }

	// 16 m ASKED FOR ROUNDS TO 15 m, and the bar must say what the line on the ground says.
	TestEqual(TEXT("the readout reports the frontage that actually snapped"),
		Frontage->Value, FString(TEXT("15 m")));

	// AND NOTHING BEHIND IT. One corner pinned means no boundary and no contents, so a ghost
	// still holding the first gesture's 26 m depth would draw lines these two count.
	TestEqual(TEXT("no provisional boundary at one pinned corner"),
		Sink.LinesOf(EPreviewStyle::Provisional), 0);
	TestEqual(TEXT("and no contents"), Sink.LinesOf(EPreviewStyle::Pending), 0);

	return true;
}
```

- [ ] **Step 5: Run to verify everything passes**

Expected: PASS, and `PlotGhostDrawsTheModules` still passes — it counts `Pending` lines as
`4 × modules` and no longer counts the outline, because the outline is `Pinned`/`Provisional`
now. **Update its arithmetic from `4 + 4 * Standing` to `4 * Standing` in this step**; the
claim is unchanged, the outline simply stopped being the same style.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotPlaceToolTest.cpp
git commit -m "feat(tool): the ghost promises only what the player has pinned"
```

---

### Task 4: The panel at the plot, and the bar's readout retired

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp` (`BuildReadout`)
- Modify: `Source/AirportMgr/RoadBuildHUD.cpp` and `.h`
- Modify: `Source/AirportMgr/BuildBarWidget.cpp` and `.h`
- Modify: `Source/AirportMgr/RoadBuildHUDTest.cpp`
- Modify: `Source/AirportMgr/PlotReadoutBarTest.cpp`

**Interfaces:**
- Consumes: Task 2's `PinnedCount`; the existing `FToolReadout` and
  `ARoadBuildHUD::CommitPromptText`.
- Produces: `ARoadBuildHUD::PanelLines(const FToolReadout&)` returning `TArray<FString>`.

- [ ] **Step 1: Write the failing test**

In `RoadBuildHUDTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPanelShowsProgressAndBuildTest,
	"AirportMgr.HUD.PlotPanelShowsProgressAndBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPanelShowsProgressAndBuildTest::RunTest(const FString& Parameters)
{
	// NO CANVAS AND NO PIE. The drawing cannot be tested headlessly; what CAN go wrong
	// silently is the content - a panel that never mentions Build, or one that offers it
	// before the shape is finished.
	FToolReadout Mid;
	Mid.Facts.Emplace(TEXT("Plot Points"), TEXT("2/4"));
	Mid.Facts.Emplace(TEXT("Frontage"), TEXT("20 m"));
	Mid.bCommittable = false;

	const TArray<FString> MidLines = ARoadBuildHUD::PanelLines(Mid);

	TestTrue(TEXT("the panel reports progress through the gesture"),
		MidLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("2/4")); }));
	TestFalse(TEXT("and does not offer Build before the shape is finished"),
		MidLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("Build")); }));

	FToolReadout Ready = Mid;
	Ready.bCommittable = true;
	const TArray<FString> ReadyLines = ARoadBuildHUD::PanelLines(Ready);

	// THE KEY COMES FROM THE REGISTRY, through CommitPromptText - so a rebound Build cannot
	// leave the panel advertising a key that does nothing.
	TestTrue(TEXT("a committable gesture is offered Build, by name"),
		ReadyLines.ContainsByPredicate([](const FString& L) { return L.Contains(TEXT("Build")); }));

	const FBuildAction* Build = FindAction(FName(TEXT("edit.build")));
	if (!TestNotNull(TEXT("a Build action"), Build)) { return false; }
	TestTrue(TEXT("and the key it names is the one the registry bound"),
		ReadyLines.ContainsByPredicate([Build](const FString& L)
		{
			return L.Contains(Build->Key.GetDisplayName().ToString());
		}));

	// WARNINGS SURVIVE. "No room to grow" is the one fact that changes a decision, and a
	// panel that dropped it would be a readout that only ever says good news.
	FToolReadout Warned = Ready;
	Warned.Warnings.Add(TEXT("No room to grow"));
	TestTrue(TEXT("a warning reaches the panel"),
		ARoadBuildHUD::PanelLines(Warned).ContainsByPredicate(
			[](const FString& L) { return L.Contains(TEXT("No room to grow")); }));

	return true;
}
```

- [ ] **Step 2: Build twice, run, verify it fails**

Expected: FAIL to compile — `PanelLines` does not exist.

- [ ] **Step 3: Emit the progress fact, and build the panel**

In `FPlotPlaceTool::BuildReadout`, emit the point count FIRST, before the other facts, so the
panel's top line is the progress:

```cpp
	// FIRST, because it is the line that says where the player is in the gesture and every
	// other fact is about a shape that is not finished yet.
	Sink.Fact(TEXT("Plot Points"), FString::Printf(TEXT("%d/4"), PinnedCount()));
```

Replace the `Bays`/`Rows` facts with the one the quad can honestly state:

```cpp
	Sink.Fact(TEXT("Frontage"), FString::Printf(TEXT("%.0f m"),
		FVector2D::Distance(Shown[0], Shown[1]) / 100.0));
```

**`Bays` and `Rows` go.** A quadrilateral has neither, and a fact whose name survived its
meaning is worse than one that was removed — the same reasoning that retired
`Expansion slots`. `Airside.Tool.PlotReadoutMatchesPreview` asserts the `Bays` fact; update it
to assert `Frontage` in this step.

In `RoadBuildHUD.h`, public:

```cpp
	/**
	 * The plot panel's text, one line per entry, or empty when there is nothing to say.
	 *
	 * STATIC AND TAKING THE READOUT for the same reason CommitPromptText is: the drawing
	 * needs a Canvas and the CONTENT does not, so the part that can silently go wrong stays
	 * testable with no world and no PIE.
	 */
	static TArray<FString> PanelLines(const FToolReadout& Readout);
```

In `RoadBuildHUD.cpp`:

```cpp
TArray<FString> ARoadBuildHUD::PanelLines(const FToolReadout& Readout)
{
	TArray<FString> Lines;
	for (const TPair<FString, FString>& Fact : Readout.Facts)
	{
		Lines.Add(FString::Printf(TEXT("%s: %s"), *Fact.Key, *Fact.Value));
	}
	for (const FString& Warning : Readout.Warnings)
	{
		Lines.Add(Warning);
	}

	// BUILD LAST, under the facts it is a decision about - and only when the gesture can
	// actually take it. CommitPromptText already answers both, and reads the key off the
	// registry, so this does not get to hold a second opinion about either.
	const FString Prompt = CommitPromptText(Readout);
	if (!Prompt.IsEmpty())
	{
		Lines.Add(Prompt);
	}
	return Lines;
}
```

`DrawCommitPrompt` becomes `DrawPlotPanel(const FVector2D& PlanePoint, const TArray<FString>&)`:
same projection, same dark ground, one `DrawText` per line stacked downward, sized to the
widest line. Call it from `DrawHUD` with the plot's centroid rather than the cursor — the
centroid of `Context.Cursor` and the readout is not available there, so pass
`Controller->MakeToolContext().Cursor` as now and leave the centroid as a follow-up; **say so
in a comment rather than pretending the panel is plot-anchored when it is cursor-anchored.**

- [ ] **Step 4: Retire the bar's readout section**

In `BuildBarWidget.h` delete the `ReadoutSection` UPROPERTY and the `ApplyReadout` and
`ReadoutLineCountForTest` declarations; in `.cpp` delete their bodies, the block in
`EnsureSlots` that builds the section, and the `ApplyReadout` call at the end of
`RefreshState`. In `PlotReadoutBarTest.cpp` delete the four assertions that call
`ApplyReadout` / `ReadoutLineCountForTest`.

**The rest of `PlotReadoutBarTest` stays.** Its claims about the registry's `edit.build`
action and the controller's per-frame collection are untouched by where the facts render, and
deleting the file would lose them.

- [ ] **Step 5: Run the full suite**

```bash
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_ManorSpike\AirportMgr.uproject"
```

Expected: green. `AirportMgr.Actions.BarBuildsFromRegistry` counts buttons per section and is
unaffected — the readout was never a button.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp `
        Source/AirportMgr/RoadBuildHUD.h Source/AirportMgr/RoadBuildHUD.cpp `
        Source/AirportMgr/BuildBarWidget.h Source/AirportMgr/BuildBarWidget.cpp `
        Source/AirportMgr/RoadBuildHUDTest.cpp Source/AirportMgr/PlotReadoutBarTest.cpp
git commit -m "feat(ui): the gesture reads itself out where the player is looking"
```

---

### Task 5: Retire the grid solver nothing calls

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/PlotFit.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/PlotFit.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotFitTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `PlotFit::BuildGrid`, `PlotFit::GridOutline` and `PlotFit::FPlotGrid` no longer
  exist. `PlotFit::FitBays`, `FPlotBay`, `FPlotFit`, `BayWidthUu`, `BayDepthUu` and
  `CornerInsetUu` all REMAIN.

- [ ] **Step 1: Prove they have no callers**

```bash
grep -rn "BuildGrid\|GridOutline\|FPlotGrid" --include=*.cpp --include=*.h Plugins Source
```

Expected: hits only in `PlotFit.h`, `PlotFit.cpp`, `PlotFitTest.cpp`, and prose comments.
**If any other file appears, stop** — a caller means this task is wrong and the plan needs
revisiting, not that the caller should be deleted to suit it.

- [ ] **Step 2: Delete them**

Remove `FPlotGrid`, `BuildGrid` and `GridOutline` from the header and the .cpp, and the
`Airside.Solve.PlotGridSlots` test plus the `GridOutline` assertions inside
`Airside.Solve.PlotFitBays` from `PlotFitTest.cpp`.

Leave a note where `FPlotGrid` was:

```cpp
	// A grid of Width x Depth slots lived here until 2026-09-16. It served the rectangle
	// gesture, and the four-point gesture that replaced it has no rows and no bays - see
	// docs/superpowers/specs/2026-09-16-four-point-plot-gesture-design.md. Deleted rather
	// than left, because a solver nothing calls is a thing the next reader has to disprove
	// the importance of. FitBays below is NOT dead: PlaceEntityInPlot still calls it.
```

- [ ] **Step 3: Run the full suite**

Expected: green, with the test count DOWN by however many assertions went. Read the
`N test(s) run` line and confirm the drop is the tests you deleted and nothing else.

- [ ] **Step 4: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Solve/PlotFit.h `
        Plugins/Airside/Source/Airside/Private/Solve/PlotFit.cpp `
        Plugins/Airside/Source/AirsideTests/Private/PlotFitTest.cpp
git commit -m "refactor(solve): the bay grid had no callers left"
```

---

## Verifying the iteration, not just the build

After Task 5, in PIE:

1. Press `0`. Hover a service road — anchor dots, with the one under the cursor highlighted,
   and you should be able to hover anywhere on the tarmac rather than the centreline.
2. Click. **Only a solid white line along the road**, growing in 5 m steps from 15 m.
3. Click. A dashed boundary follows the cursor. **No modules yet.**
4. Click. Modules appear and settle as the last corner moves.
5. Click. The panel offers Build; press Enter.
6. `python Tools/Mcp.py log LogAirside` for the census.

What would say it is still wrong: a dash too fine to read at zoom (`DashPitch`), a 15 m
minimum that feels enormous next to a 4 m shed, or contents at three corners that still jump
noticeably when the fourth lands — which would mean the yard is more sensitive to the last
corner than the "promise the gesture can keep" argument assumes.
