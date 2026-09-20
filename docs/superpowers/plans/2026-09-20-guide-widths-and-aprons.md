# Guide Widths and Aprons Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fill the Apron column, and make a positional guide against a boundary align edge-to-edge rather than centre-to-boundary.

**Architecture:** `FGuideAnchor` learns the drag's half-widths and what its moving point represents. A positional candidate whose reference is an extended BOUNDARY is displaced by that half-width, one candidate per side. `FApronGuideSource` fills the four Apron cells the grid declares. The runway and apron tools gain anchors, so the columns have gestures to serve.

**Tech Stack:** UE 5.8.2, C++. `Airside` plugin (`Solve/` is CoreMinimal-only; `Tool/` may see `URoadNetwork`).

**Spec:** `docs/superpowers/specs/2026-09-20-guide-grid-design.md`, sections 6 and 8.

## Global Constraints

- **The first plan (`2026-09-20-guide-grid.md`) is DONE and merged into this branch.** `ERelation` has six values, `EReference` six, and `SnapGuide::IsLegalCell` declares nineteen. Read `GuideArbiter.h` before starting; do not re-derive the grid from the spec, which the code has since overtaken.
- **The editor must be CLOSED for every build.** New UPROPERTYs and a changed virtual signature are outside Live Coding's reach. Build with:
  `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2_snapping\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE`
  `-NoHotReloadFromIDE` is correct only because this is a worktree. Never pass it on `C:\repos\AirportMgr2`.
- **A new test `.cpp` needs TWO builds.** The first reports `Result: Succeeded` without compiling it. Tasks 4 and 6 each add a file.
- **Never trust the runner's exit code.** Read the `N test(s) run, N failed, N crashed` line. A crash is reported there and nowhere else — the first plan hit exactly one.
- Run tests with `./Tools/Run-AirsideTests.ps1` from a **PowerShell** tool call, not Bash. `-Project` defaults from the script's own location.
- **Prove each new test can fail.** Break the rule it protects, watch that named assertion go red, restore by hand. In the first plan a new test passed with its rule deliberately broken, because a defensive filter in the chain was stripping the fault before the test could see it.
- `Solve/` headers include `CoreMinimal.h` and nothing else.
- Comment and `UE_LOG` counts in touched files must not fall.
- No `Co-Authored-By` trailer.

---

### Task 1: One width-to-profile resolution

`Kind + WidthIndex -> URoadProfile*` is written twice today and is about to be needed a third time. Collapse it first.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadEditTarget.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp:213-228`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacade.cpp:296-317`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/RoadNetworkActorTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `virtual URoadProfile* IRoadEditTarget::ResolveProfileFor(ERoadKind Kind, int32 WidthIndex) const = 0;`, implemented on `ARoadNetworkActor`.

- [ ] **Step 1: Write the failing test**

Add to `RoadNetworkActorTest.cpp`:

```cpp
/**
 * ONE RESOLUTION, ASKED BY EVERYONE. Kind plus WidthIndex names a cross-section, and before
 * 2026-09-20 that rule was written twice - URoadEditFacade::ConnectNodes and
 * ARoadNetworkActor::UpdateGhost, the second carrying a comment saying it must agree with the
 * first. A third copy was about to go into FRoadDrawTool::DescribeGuideAnchor for its
 * half-width. This pins the collapse: the ghost's profile IS what a click would lay.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProfileResolutionIsOneRuleTest,
	"Airside.Present.ProfileResolutionIsOneRule",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FProfileResolutionIsOneRuleTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	IRoadEditTarget* Target = Actor;

	// A SERVICE ROAD IGNORES THE INDEX OUTRIGHT - it has one authored cross-section, and an
	// index reaching it would lay a taxiway's width on a lane meant for vans.
	TestEqual(TEXT("a service road answers the same whatever index is passed"),
		Target->ResolveProfileFor(ERoadKind::ServiceRoad, 0),
		Target->ResolveProfileFor(ERoadKind::ServiceRoad, INDEX_NONE));

	// A TAXIWAY WITH NO INDEX FALLS BACK TO THE LEVEL'S OWN TUNING, which is the honest answer
	// where the service road has none.
	TestNotNull(TEXT("a taxiway always resolves something"),
		Target->ResolveProfileFor(ERoadKind::Taxiway, INDEX_NONE));

	// AND AN INDEX THE CONTENT SET CAN ANSWER GIVES THAT ONE, not the fallback. Skipped rather
	// than failed when the content set is empty: this is a rule about resolution, not about
	// what a particular project happens to ship.
	if (Target->GetTaxiwayProfileCount() > 0)
	{
		TestEqual(TEXT("an index resolves to that width"),
			Target->ResolveProfileFor(ERoadKind::Taxiway, 0),
			Target->ResolveTaxiwayProfile(0));
	}
	else
	{
		AddInfo(TEXT("No taxiway widths in the content set; index resolution not checked"));
	}

	return true;
}
```

- [ ] **Step 2: Build and run to verify it fails**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Present.ProfileResolutionIsOneRule
```

Expected: compile error — `IRoadEditTarget` has no member `ResolveProfileFor`.

- [ ] **Step 3: Declare it on the interface**

In `RoadEditTarget.h`, beside `ResolveTaxiwayProfile`:

```cpp
	/**
	 * The cross-section a click with this Kind and WidthIndex would actually lay.
	 *
	 * THE ONE PLACE THIS RULE LIVES, as of 2026-09-20. It was written twice - in
	 * URoadEditFacade::ConnectNodes and in ARoadNetworkActor::UpdateGhost, whose comment already
	 * said the two must agree - and a third copy was about to be added for the guide anchor's
	 * half-width. Both existing sites now forward here.
	 *
	 * NOT A REPLACEMENT for ResolveTaxiwayProfile and its siblings: those answer "what is width
	 * 2", which is a content question. This answers "what would this gesture lay", which folds
	 * in the service road's exemption and the taxiway's fallback.
	 */
	virtual URoadProfile* ResolveProfileFor(ERoadKind Kind, int32 WidthIndex) const = 0;
```

- [ ] **Step 4: Implement it once and forward both callers**

Move the body of `URoadEditFacade::ChooseProfile` (`RoadEditFacade.cpp:296-317`) onto `ARoadNetworkActor::ResolveProfileFor` **verbatim, comments included** — the "A service road ignores the index outright" paragraph and the fallback paragraph are the reasoning, and the refactor contract says a comment travels with its code.

Then:
- `URoadEditFacade::ChooseProfile` becomes `return Actor().ResolveProfileFor(Kind, WidthIndex);`
- `ARoadNetworkActor::UpdateGhost`'s five-line `if/else if` block (`RoadNetworkActor.cpp:218-227`) becomes `Settings.Profile = ResolveProfileFor(Kind, WidthIndex);`, keeping its "THE WIDTH THE CLICK WILL ACTUALLY LAY" comment and adding that the agreement is now structural rather than maintained by hand.

- [ ] **Step 5: Build and run the whole suite**

Expected: zero failed, zero crashed. `GhostPricesTheRoad` and `TaxiwayWidth` are the two most likely to notice a mistake here — read them before touching them.

- [ ] **Step 6: Prove the test can fail**

Temporarily make `ResolveProfileFor` ignore `Kind` and always take the taxiway path. `Airside.Present.ProfileResolutionIsOneRule` must go red on the service-road leg. Revert by hand.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "refactor(airside): one resolution from a road kind and width index to a profile"
```

---

### Task 2: The anchor carries the drag's width and what its point is

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h` (`FGuideAnchor`)
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h` (`DescribeGuideAnchor`)
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp:183`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadDrawTool.h`, `Private/Tool/RoadDrawTool.cpp:255`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/PlotPlaceTool.h`, `Private/Tool/PlotPlaceTool.cpp:217`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/RoadGuideTest.cpp`

**Interfaces:**
- Consumes: `ResolveProfileFor` from Task 1.
- Produces: `FGuideAnchor::HalfWidthLeft`, `::HalfWidthRight`, `::Point` (an `EDragPoint`). `EDragPoint { Centreline, Boundary }` in `Tool/SnapGuideChain.h`. New signature: `virtual bool DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target, FGuideAnchor& Out) const`.

- [ ] **Step 1: Write the failing test**

Add to `RoadGuideTest.cpp`:

```cpp
/**
 * THE ANCHOR KNOWS HOW WIDE THE DRAG IS, and that its point is a CENTRELINE.
 *
 * A road's centreline lined up with an apron's EDGE is not what anybody means - you want the
 * road's edge flush with the apron's, which needs the half-width at the point the guide is
 * proposed. See the 2026-09-20 design section 6.
 *
 * THE TWO HALF-WIDTHS ARE ASKED SEPARATELY because URoadProfile's are separate: a cross-section
 * may be off-centre, and a mirrored pair would be wrong on every such road.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadAnchorCarriesItsHalfWidthTest,
	"Airside.Tool.RoadAnchorCarriesItsHalfWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadAnchorCarriesItsHalfWidthTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(-10000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(0.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);

	FRoadDrawTool Tool(ERoadKind::Taxiway);
	Tool.OnClick(TestTool::ContextAt(*Target, FVector2D(0.0, 0.0), ERoadSnapKind::Node));

	FGuideAnchor Anchor;
	if (!TestTrue(TEXT("the tool describes an anchor"),
		Tool.DescribeGuideAnchor(Actor->Network, Target, Anchor)))
	{
		return false;
	}

	// A ROAD'S MOVING POINT IS ITS CENTRELINE. The plot and apron tools drag a boundary corner
	// instead, and the displacement rule turns on exactly that difference.
	TestEqual(TEXT("a road drags a centreline"),
		static_cast<int32>(Anchor.Point), static_cast<int32>(EDragPoint::Centreline));

	// HONOURED, NOT ASSUMED: the profile has to resolve for the widths to mean anything, and a
	// content set with no taxiway would leave them legitimately zero.
	const URoadProfile* Profile = Target->ResolveProfileFor(ERoadKind::Taxiway, INDEX_NONE);
	if (Profile == nullptr)
	{
		AddInfo(TEXT("No taxiway profile resolves; half-widths not checked"));
		return true;
	}
	TestEqual(TEXT("the anchor carries the profile's own left half-width"),
		Anchor.HalfWidthLeft, Profile->GetHalfWidthLeft());
	TestEqual(TEXT("and its right, separately"),
		Anchor.HalfWidthRight, Profile->GetHalfWidthRight());

	return true;
}
```

- [ ] **Step 2: Build and run to verify it fails**

Expected: compile error — `FGuideAnchor` has no member `Point`, and `DescribeGuideAnchor` takes two arguments.

- [ ] **Step 3: Add the fields**

In `SnapGuideChain.h`, above `FGuideAnchor`:

```cpp
/**
 * What the point the player is moving REPRESENTS.
 *
 * A positional guide aligns like with like: centreline to centreline, boundary to boundary, and
 * a centreline against a boundary is displaced by the drag's half-width. A road's cursor is its
 * CENTRELINE; a plot's or an apron's is a corner of the shape itself, which is a BOUNDARY. See
 * the 2026-09-20 design section 6.
 *
 * AN ENUM, NOT A BOOL, per CLAUDE.md: the two cannot both be true, so the illegal state should
 * not be representable - and a third kind is easy to imagine (a kerb line, a painted edge).
 */
enum class EDragPoint : uint8
{
	Centreline,
	Boundary
};
```

And inside `FGuideAnchor`:

```cpp
	/** See EDragPoint. Centreline unless the tool says otherwise, because a road is the common case. */
	EDragPoint Point = EDragPoint::Centreline;

	/**
	 * How far the drag's pavement reaches either side of the point, uu. Zero when the gesture
	 * has no width - a plot corner, a guideline.
	 *
	 * TWO FIELDS, NOT ONE: URoadProfile::GetHalfWidthLeft and GetHalfWidthRight are separate
	 * and a cross-section may be off-centre, so a flush-left candidate and a flush-right one are
	 * not a mirrored pair and must not be computed as one.
	 */
	double HalfWidthLeft = 0.0;
	double HalfWidthRight = 0.0;
```

- [ ] **Step 4: Widen the signature**

`IBuildTool::DescribeGuideAnchor` gains an `IRoadEditTarget* Target` parameter, second. Update its doc comment to say why the target and not the `FToolContext`:

```cpp
	 * TAKES THE TARGET, NOT THE CONTEXT. The call sits INSIDE FBuildSession::MakeContext while
	 * that context is being built, so a context passed here would be half-filled - its own Guide
	 * field is the thing being computed. The target is what resolves a width index to a profile
	 * (IRoadEditTarget::ResolveProfileFor) and is fully formed by then.
```

Update the two existing overrides and `BuildSession.cpp:183` to `Tool->DescribeGuideAnchor(Network, Target, Anchor)`.

- [ ] **Step 5: Fill them in each tool**

`FRoadDrawTool::DescribeGuideAnchor`, after setting `Out.Origin`:

```cpp
	// THE WIDTH THIS GESTURE WOULD LAY - the same question the ghost asks, through the same one
	// function, so a guide cannot disagree with the pavement it is guiding.
	Out.Point = EDragPoint::Centreline;
	if (Target != nullptr)
	{
		if (const URoadProfile* Profile = Target->ResolveProfileFor(Kind, WidthIndex))
		{
			Out.HalfWidthLeft = Profile->GetHalfWidthLeft();
			Out.HalfWidthRight = Profile->GetHalfWidthRight();
		}
	}
```

`FPlotPlaceTool::DescribeGuideAnchor` sets `Out.Point = EDragPoint::Boundary;` and leaves both half-widths at zero — a plot corner is a corner of the shape, with no pavement either side of it. Say that in a comment; the zero is meaningful, not an omission.

- [ ] **Step 6: Build, run the whole suite, prove the test can fail**

Break it by making the plot tool say `Centreline`. `RoadAnchorCarriesItsHalfWidth` should stay green (it tests the road) and a plot test should not notice either — **which means the plot's half of this needs its own assertion.** Add one to the plot's existing anchor test rather than leaving `EDragPoint::Boundary` unmeasured.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(airside): a guide anchor carries the drag's half-widths and what its point is"
```

---

### Task 3: A guide point carries its own reference

`Anchor.AlignTo` is a flat array; the plot tool fills it with its own corners and the road tool with network nodes, and `FPointAlignGuideSource` cannot tell them apart. `LevelWith` cannot be gated by column until it can.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h` (`FGuidePoint`)
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp` (`FPointAlignGuideSource`)
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/RoadDrawTool.cpp`, `Private/Tool/PlotPlaceTool.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/GuideToggleTest.cpp`

**Interfaces:**
- Consumes: Task 2's signature.
- Produces: `FGuidePoint::Reference`, a `SnapGuide::EReference`, defaulting to `ThisGesture`.

- [ ] **Step 1: Write the failing test**

Add to `GuideToggleTest.cpp` a test `Airside.Tool.LevelWithIsGatedPerPoint`: build a network with one live node away from the drag, drive `FRoadDrawTool`'s anchor, resolve with `bLevelWith` on and `bRoad` **off**, and assert no winner carries `EReference::Road`. Control leg: switch `bRoad` on and the node answers. Today every point is `ThisGesture`, so the Road column cannot gate it and the first assertion fails.

- [ ] **Step 2: Build and run to verify it fails**

Expected: `1 test(s) run, 1 failed`, on "with the Road column off, a network node offers nothing".

- [ ] **Step 3: Add the field**

```cpp
	/**
	 * Which column this point belongs to - ThisGesture for the gesture's own corners, Road for
	 * a live network node, Apron for an apron's corner.
	 *
	 * THE TOOL TAGS IT, for the same reason the tool supplies the point at all: only the tool
	 * knows where its own points came from, and a source that guessed would be a second opinion
	 * about the gesture. Without it LevelWith cannot be gated by column - one flat array served
	 * both the plot's corners and the road's nodes, and the two are different columns.
	 */
	SnapGuide::EReference Reference = SnapGuide::EReference::ThisGesture;
```

- [ ] **Step 4: Tag at both fill sites and read it in the source**

`FPlotPlaceTool` leaves the default. `FRoadDrawTool`'s node loop sets `SnapGuide::EReference::Road` — its `Out.AlignTo.Add({ Node.Position, TEXT("that node") })` becomes an explicit `FGuidePoint` with all three fields, since a braced init would now silently take the wrong member.

In `FPointAlignGuideSource::Propose`, both candidates per point take `Level.Reference = Point.Reference;` rather than the hard-coded `ThisGesture`.

- [ ] **Step 5: Build, run the whole suite, prove the test can fail**

Break it by hard-coding `ThisGesture` back in the source. The new test must go red. Revert by hand.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(airside): a guide point says which column it came from"
```

---

### Task 4: The apron source

Fills the four Apron cells the grid already declares, aligning centre-to-centre throughout. The flush rule is Task 5.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/ApronGuideTest.cpp`

**Interfaces:**
- Consumes: `AddSpokes` (already in the chain's anonymous namespace), `FGuidePoint::Reference` from Task 3.
- Produces: `FApronGuideSource`, declaring `ERelation::Parallel`; `FApronLineGuideSource` (`Collinear`); `FApronAngledGuideSource` (`AngledFrom`); `FApronCornerGuideSource` (`LevelWith`).

**Four sources, one per relation**, for the reason the first plan established the hard way: `FSnapGuideChain::Resolve` skips a source by its declared `Relation()` before it walks anything, so one source proposing four relations would have all four silenced by whichever it declared. Share the walk through one file-local helper that yields each live apron's edges as `(A, B, Along, Name)`.

- [ ] **Step 1: Write the failing tests**

Create `ApronGuideTest.cpp` with:

`Airside.Tool.ApronEdgeOffersItsDirection` — an apron square from (0,0) to (8000,8000); a drag beside it; `bParallel` + `bApron` on, everything else off; assert a winner with `Reference == EReference::Apron` whose direction matches an edge.

`Airside.Tool.ApronCornerIsSomethingToBeLevelWith` — `bLevelWith` + `bApron`; cursor level with a corner; assert `Relation == LevelWith`, `Reference == Apron`.

`Airside.Tool.ApronColumnOffSilencesEveryRelation` — the 2026-09-20 shape, applied to the new column: all four relations on, `bApron` off, assert nothing references an apron; control leg switches it on.

Add an apron in the fixture with `Actor->Network->AddApron(...)` via the facade's placement path — check `ApronDrawToolTest` for the call the rest of the suite uses rather than inventing one.

- [ ] **Step 2: Build twice, run, verify all three fail**

- [ ] **Step 3: Implement the four sources**

Each walks `Network.GetAprons()`, skips `!bAlive`, and for each edge of `Outline` (wrapping the last back to the first) applies the reach test from the drag's origin. Naming: `"the apron edge"` — `FApronSurface` has no display name, and a node has no name either, which is why `FRoadDrawTool` labels its points `"that node"` and relies on the drawn line to say which. Say that in the source's header.

- [ ] **Step 4: Build, run the whole suite**

`Airside.Tool.GuideGridHasNoCellOutsideTheList` becomes materially stronger here — its fixture comment says the Apron column is unexercised, so **update that comment and add an apron to its fixture** in this task.

- [ ] **Step 5: Prove each test can fail, then commit**

```bash
git add -A
git commit -m "feat(airside): an apron's edges and corners are something to line up with"
```

---

### Task 5: Flush by half-width against a boundary

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/ApronGuideTest.cpp`

**Interfaces:**
- Consumes: `FGuideAnchor::HalfWidthLeft`/`Right`/`Point` from Task 2, `FApronLineGuideSource` from Task 4.
- Produces: nothing new.

**Where it applies, and where it does not** — decide once, here, and write it down:

| Cell | Displaced? |
|---|---|
| `Collinear x Apron` | **yes** — the reference is an extended boundary, and the road's edge is what you want flush with it |
| `LevelWith x Apron` | no — a corner is a point, not an extended edge; there is nothing to be flush WITH |
| `AngledFrom x Apron` | no — same, it radiates from a corner |
| `Parallel x Apron` | no — angular, through the origin: it constrains direction, never position |
| anything x Road/Runway/Stand | no — those references are centrelines, and centre-to-centre is what a junction solve wants |

- [ ] **Step 1: Write the failing test**

Add `Airside.Tool.CollinearAgainstAnApronIsFlushByHalfWidth` to `ApronGuideTest.cpp`. Lay a road whose profile has a **known, asymmetric** pair of half-widths (build one with `URoadProfile::MakeTransient` as `TestGuide::LayRunway` does, and set the bands so left and right differ). Drive an anchor carrying those. Assert:

- **two** candidates for that apron edge, not one
- their `Through` points are displaced from the edge by `HalfWidthLeft` and `HalfWidthRight` respectively — **two different values**, which is the assertion that catches a mirrored pair
- with `Anchor.Point == EDragPoint::Boundary` and both half-widths zero, the two collapse onto the edge itself

- [ ] **Step 2: Build and run to verify it fails**

- [ ] **Step 3: Implement**

In `FApronLineGuideSource::Propose`, where the edge's line is built:

```cpp
		// FLUSH, NOT CENTRED. An apron edge is a BOUNDARY and a road's cursor is its CENTRELINE,
		// so lining the two up directly would put the road's middle on the apron's edge - half
		// the pavement over it. Displacing by the half-width puts the road's EDGE there, which
		// is what "in line with the apron" means to a player. See the 2026-09-20 design §6.
		//
		// BOTH SIDES, and not a mirrored pair: GetHalfWidthLeft and GetHalfWidthRight are
		// separate on URoadProfile because a cross-section may be off-centre. Flush-inside and
		// flush-outside are both real intents - a taxiway running along the apron, or one
		// abutting it - so neither may be chosen for the player.
		//
		// A BOUNDARY DRAG IS NOT DISPLACED. An apron corner against another apron's edge is
		// boundary against boundary, and the two already mean the same thing.
```

The displacement is along the edge's perpendicular. When `Anchor.Point != EDragPoint::Centreline`, or both half-widths are zero, emit the single undisplaced line rather than two coincident ones — two identical candidates would tie and make the source-order rule arbitrate a choice that does not exist.

- [ ] **Step 4: Build, run the whole suite, prove the test can fail**

Break it by using `HalfWidthLeft` for both sides. The asymmetry assertion must go red; if it stays green the fixture's profile is symmetric and the test is measuring nothing.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "fix(airside): a road lines up EDGE to edge with an apron, not centre to edge"
```

---

### Task 6: Anchors for the runway and apron tools

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RunwayTool.h`, `Private/Tool/RunwayTool.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/ApronDrawTool.h`, `Private/Tool/ApronDrawTool.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/ToolAnchorTest.cpp`

**Interfaces:**
- Consumes: Task 2's signature, Task 3's tagged points.
- Produces: `DescribeGuideAnchor` overrides on both tools.

- [ ] **Step 1: Write the failing tests**

`Airside.Tool.RunwayAnchorIsItsFirstThreshold` — click one threshold, assert the anchor returns true with `Origin` at that click, `Reference` **zero** (a runway has no incoming edge, so Extending correctly proposes nothing), and `Point == EDragPoint::Centreline`.

`Airside.Tool.ApronAnchorExtendsItsLastEdge` — click three corners, assert `Origin` is the last, `Reference` is the previous edge's direction, `AlignTo` holds the corners placed so far tagged `ThisGesture`, and `Point == EDragPoint::Boundary`.

Both are composition-level: spawn through `FAirsideTestWorld`, drive the tool with `TestTool::ContextAt`.

- [ ] **Step 2: Build twice, run, verify both fail**

Expected: `2 test(s) run, 2 failed` — the base `DescribeGuideAnchor` returns false.

- [ ] **Step 3: Implement the runway anchor**

```cpp
bool FRunwayTool::DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
	FGuideAnchor& Out) const
{
	// NOTHING PENDING MEANS NOTHING TO GUIDE, exactly as FRoadDrawTool has it: before the first
	// threshold there is no point for a line to swing around.
	if (!bHasThreshold)
	{
		return false;
	}

	Out.Origin = Threshold;
	Out.Point = EDragPoint::Centreline;

	// NO REFERENCE, DELIBERATELY. A runway is two clicks and no chaining - there is no incoming
	// edge to extend, so FExtendingGuideSource and FPointAlignGuideSource both correctly propose
	// nothing and every network column answers instead. Leaving Reference zero is how a tool says
	// that; inventing an axis here would square the strip to something arbitrary.
	return true;
}
```

Fill the half-widths from `Target->ResolveRunwayProfile(WidthIndex)`, clamped against
`GetRunwayProfileCount()`. **Not** `FRunwayTool::ProfileForWidth` — that takes an `FToolContext`,
which is precisely what `DescribeGuideAnchor` cannot be handed (see Task 2, Step 4). If the clamp
is about to be written twice, change `ProfileForWidth` to take the target instead of the context
and have both call it — the same collapse Task 1 makes, one class down.

- [ ] **Step 4: Implement the apron anchor**

`FOutlineDrawTool::GetCorners()` holds everything. Origin is the last corner; `Reference` is `Last - Previous` normalised, with `ReferenceAt` the previous corner; `ReferenceName` is `TEXT("this edge")`; `AlignTo` is every corner before the last, tagged `ThisGesture` and named `"corner N"` as the player counts them. Return false with fewer than two corners — one corner has no edge to extend.

`Out.Point = EDragPoint::Boundary;` and both half-widths stay zero.

The apron tool is a `FOutlineDrawTool`, which also backs `FPlotDrawTool`; put the anchor on the **base** if both want it, and say in the comment which. Check whether `FPlotDrawTool` has an anchor of its own already before choosing — `FPlotPlaceTool` does, and it is a different class.

- [ ] **Step 5: Build, run the whole suite, prove both tests can fail**

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(airside): the runway and apron tools describe a guide anchor"
```

---

### Task 7: Say what is now true

**Files:**
- Modify: `docs/superpowers/specs/2026-09-20-guide-grid-design.md`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideSettings.h`

- [ ] **Step 1: Update the spec**

Section 8 lists the tools with no guides. Three registrations answered when it was written; five do now. Update the list, and move Stand placement from "out of scope, and named so the absence is deliberate" to whatever is true after this plan — it is still unguided, and still has the strongest case of the four left.

Section 6's table is now implemented; mark which cells are displaced, matching Task 5's table exactly rather than restating it loosely.

- [ ] **Step 2: Drop the stale note on `bApron`**

`FSnapGuideSettings::bApron`'s comment, and the first plan's closing section, both say the Apron column is a switch with nothing behind it. It has four sources now.

- [ ] **Step 3: Full suite, then commit**

```bash
git add -A
git commit -m "docs(airside): the guide grid design matches what is built"
```

---

## What this plan does not do

- **Stand placement has no guide anchor.** Its drag IS a heading, so it has the strongest case of the four tools left, but nothing here needs it.
- **`MatchingGap x Runway`** — runway-to-taxiway separation is a real ICAO standard and a legitimate cell, but it needs its own search rather than a free ride on the road one. Still a declared hole.
- **No PIE pass on `SearchRadiusUu`, `MaxPullUu`, or the candidate count.** `AngledFrom` already put three spokes on every end of every segment in reach, and the Apron column multiplies by every edge of every apron. `FSnapGuideChain::ProposeAll` still reserves 16. This is the first thing to measure once the whole grid is switched on in PIE, and the first place a tuning change will be wanted.
