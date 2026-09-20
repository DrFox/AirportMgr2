# Guide Grid Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the eight guide toggles into a relation axis and a reference axis, gate every candidate on both, so switching Runway off silences runway guidance in every relation.

**Architecture:** `SnapGuide::ESource` becomes two plain enums, `ERelation` (what a guide means) and `EReference` (what it is measured against). A `FCandidate` carries both. A declared grid, `IsLegalCell`, says which of the thirty pairs exist. `FSnapGuideSettings` holds one named bool per row and one per column, and a candidate is offered only when both are on. The runway partition follows, so the Runway column owns runway geometry with its own search policy.

**Tech Stack:** UE 5.8.2, C++. `Airside` plugin (`Solve/` is CoreMinimal-only; `Tool/` may see `URoadNetwork`), `AirportMgr` game module for the bar.

**Spec:** `docs/superpowers/specs/2026-09-20-guide-grid-design.md`

## Global Constraints

- **This plan is spec steps 1, 2, 3 and 7.** Widths on the anchor, the apron source, and the runway/apron tool anchors (spec §6, §8) are a second plan and are NOT in scope here. Do not start them.
- **The editor must be CLOSED for every build in this plan.** New UPROPERTYs (Task 3) and a new `EActionSection` value (Task 4) are outside Live Coding's reach. Build with:
  `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2_snapping\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE`
  `-NoHotReloadFromIDE` is correct HERE and only here: this is a worktree, and it writes only its own `Binaries/` and `Intermediate/`. Never pass it on `C:\repos\AirportMgr2`.
- **A new test `.cpp` needs TWO builds.** The first reports `Result: Succeeded` without compiling it, because UBT has not rescanned the module's source list. Task 1 and Task 6 each add a file — build twice before believing a green run.
- **Never trust the test runner's exit code.** Read the `N test(s) run, N failed, N crashed` line from `./Tools/Run-AirsideTests.ps1`. A crashing test used to report green.
- Run tests with `./Tools/Run-AirsideTests.ps1 -Filter <filter>` from inside the worktree. `-Project` defaults from the script's own location, so it does NOT need passing.
- `./Tools/Check-Architecture.ps1` runs first inside the test script and is the pre-commit lint: include direction, one log category per name, stacked doc comments.
- **Name leaf tests distinctly.** UE's automation tree drops a bare-named test once a dotted child exists; only the run count catches it.
- **`Solve/` headers may include `CoreMinimal.h` and nothing else.** `GuideArbiter.h` is a `Solve/` header.
- **Plain enums, not UENUMs**, for `ERelation` and `EReference`: UHT cannot see an enum without a `.generated.h`, and a `Solve/` header has none.
- **Refactor contract.** `UE_LOG` count and comment-line count in touched files must not fall. Count before and after.
- Do not add a `Co-Authored-By` trailer to commit messages.
- Rename the branch before the first commit: `git branch -m feature/snapping-docs feature/guide-grid`

---

### Task 1: The grid

Two enums and the declared list of legal pairs. Nothing consumes them yet, which is the point — this task lands the contract before anything depends on it.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h`
- Create: `Plugins/Airside/Source/Airside/Private/Solve/GuideGrid.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/GuideGridTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `SnapGuide::ERelation`, `SnapGuide::EReference`, `bool SnapGuide::IsLegalCell(ERelation, EReference)`.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/GuideGridTest.cpp`:

```cpp
#include "Misc/AutomationTest.h"
#include "Solve/GuideArbiter.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE GRID IS A LIST, AND THIS IS ITS FIRST CONSUMER. Sixteen of the thirty pairs are legal;
 * a hole is a statement, not an omission, so the count is asserted rather than the shape.
 * See the 2026-09-20 guide-grid design section 3 for each hole's reason.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideGridDeclaresSixteenCellsTest,
	"Airside.Solve.GuideGridDeclaresSixteenCells",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideGridDeclaresSixteenCellsTest::RunTest(const FString& Parameters)
{
	const int32 Relations = static_cast<int32>(SnapGuide::ERelation::MatchingGap) + 1;
	const int32 References = static_cast<int32>(SnapGuide::EReference::World) + 1;
	TestEqual(TEXT("five relations"), Relations, 5);
	TestEqual(TEXT("six references"), References, 6);

	int32 Legal = 0;
	for (int32 R = 0; R < Relations; ++R)
	{
		for (int32 F = 0; F < References; ++F)
		{
			if (SnapGuide::IsLegalCell(
				static_cast<SnapGuide::ERelation>(R), static_cast<SnapGuide::EReference>(F)))
			{
				++Legal;
			}
		}
	}
	TestEqual(TEXT("sixteen of the thirty pairs are legal"), Legal, 16);

	// THREE NAMED CELLS, not a re-listing of the table: a test that restated the whole grid
	// would be a second copy of it, and the two would drift. These three are the ones whose
	// reasoning the design argues hardest, so they are the ones worth pinning by name.
	TestTrue(TEXT("Extending is about the shape being drawn"),
		SnapGuide::IsLegalCell(SnapGuide::ERelation::Extending, SnapGuide::EReference::ThisGesture));
	TestFalse(TEXT("and Extending means nothing against a road"),
		SnapGuide::IsLegalCell(SnapGuide::ERelation::Extending, SnapGuide::EReference::Road));
	TestFalse(TEXT("a world axis has no position, so nothing can be in line with it"),
		SnapGuide::IsLegalCell(SnapGuide::ERelation::Collinear, SnapGuide::EReference::World));

	return true;
}

#endif
```

- [ ] **Step 2: Build twice, then run the test to verify it fails**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\airportmgr2_snapping\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
```

Expected: the FIRST build fails to compile `GuideGridTest.cpp` with `'ERelation': is not a member of 'SnapGuide'` — or reports `Succeeded` without having compiled the new file at all, which is the two-builds trap. Either way the enums do not exist yet, which is the failure this step wants.

- [ ] **Step 3: Add the enums to the header**

In `Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h`, replace the whole `enum class ESource` block (and its doc comment) with:

```cpp
	/**
	 * WHAT a guide means. Declaration order is the tiebreak WITHIN a fit kind - see Arbitrate.
	 *
	 * A PLAIN ENUM, not a UENUM, for the reason ESource carried before it: UHT cannot see an
	 * enum without a .generated.h, and a Solve/ header may not have one. FSnapGuideSettings
	 * wraps it in named bools rather than a reflected array for the same reason.
	 *
	 * ORDER IS MOST-SPECIFIC-FIRST: what you are extending is what you are thinking about;
	 * matching a neighbour's gap is the most incidental thing on the list.
	 */
	enum class ERelation : uint8
	{
		Extending,
		LevelWith,
		Parallel,
		Collinear,
		MatchingGap
	};

	/**
	 * WHAT a guide is measured against. Declaration order breaks ties within one relation.
	 *
	 * SPLIT OUT OF ESource ON 2026-09-20. ESource mixed these two axes: Extending, PointAlign,
	 * Collinear, Parallel and Offset named relationships, while Runway, World and Aligned named
	 * references - and Parallel and Collinear carried an unnamed, unswitchable reference, "a
	 * road". A player switching Runway off still saw "parallel to runway 18/36", because there
	 * was no axis for the toggle to act along. See the 2026-09-20 guide-grid design section 1.
	 *
	 * ThisGesture RANKS FIRST because the shape under the cursor is more specific than anything
	 * already on the field; World ranks last because it is what you fall back on.
	 */
	enum class EReference : uint8
	{
		ThisGesture,
		Road,
		Runway,
		Apron,
		Stand,
		World
	};

	/**
	 * Whether this pair of axes names a guide that exists. Design section 3's grid.
	 *
	 * THE ONE PLACE THE GRID IS WRITTEN DOWN. FSnapGuideSettings::IsEnabled consults it, the
	 * registry test walks it, and Airside.Tool.GuideGridHasNoCellOutsideTheList asserts no
	 * source can propose a pair it rejects. Sixteen of the thirty pairs are legal; the holes
	 * are reasoned about one by one in the design, not merely left out.
	 */
	AIRSIDE_API bool IsLegalCell(ERelation Relation, EReference Reference);
```

- [ ] **Step 4: Write the grid itself**

Create `Plugins/Airside/Source/Airside/Private/Solve/GuideGrid.cpp`:

```cpp
#include "Solve/GuideArbiter.h"

bool SnapGuide::IsLegalCell(ERelation Relation, EReference Reference)
{
	// SPELT OUT POSITIVELY, one row at a time, rather than as exclusions. "Every reference but
	// ThisGesture" would be shorter and would silently admit the NEXT reference anyone adds -
	// a new column has to be argued for cell by cell, which is the whole point of the grid.
	//
	// NO `default:`, matching FSnapGuideSettings::IsEnabled: a relation added without a row
	// here falls past the switch to `false`, so it proposes nothing rather than everything.
	switch (Relation)
	{
	case ERelation::Extending:
		// The edge this gesture is already growing. There is no other edge it could mean.
		return Reference == EReference::ThisGesture;

	case ERelation::LevelWith:
		// A point to be level with. A runway's alignable points are its thresholds, which are
		// ordinary nodes already served by Road; a world axis has no position at all.
		return Reference == EReference::ThisGesture
			|| Reference == EReference::Road
			|| Reference == EReference::Apron
			|| Reference == EReference::Stand;

	case ERelation::Parallel:
		// A direction. ThisGesture is absent because parallel-to-your-own-edge IS Extending,
		// and a second name for one behaviour is what this split exists to remove.
		return Reference == EReference::Road
			|| Reference == EReference::Runway
			|| Reference == EReference::Apron
			|| Reference == EReference::Stand
			|| Reference == EReference::World;

	case ERelation::Collinear:
		// A line to be ON, so the reference needs a position as well as a direction. That is
		// every column but World, which is a direction and nothing else.
		return Reference == EReference::ThisGesture
			|| Reference == EReference::Road
			|| Reference == EReference::Runway
			|| Reference == EReference::Apron
			|| Reference == EReference::Stand;

	case ERelation::MatchingGap:
		// Road alone. Its reference must be the same nearest road FParallelGuideSource picks,
		// or the two guides stop describing one road between them - see the design section 5.
		// Runway-to-taxiway separation is a real standard and a legitimate future cell, but it
		// needs its own search rather than a free ride on this one.
		return Reference == EReference::Road;
	}
	return false;
}
```

- [ ] **Step 5: Build and run the test to verify it passes**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\airportmgr2_snapping\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve.GuideGrid
```

Expected: `1 test(s) run, 0 failed, 0 crashed`. If it says `0 test(s) run`, the file was not compiled — build again (the two-builds trap) before doing anything else.

- [ ] **Step 6: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h `
        Plugins/Airside/Source/Airside/Private/Solve/GuideGrid.cpp `
        Plugins/Airside/Source/AirsideTests/Private/GuideGridTest.cpp
git commit -m "feat(airside): declare the guide grid as two axes and the legal pairs between them"
```

---

### Task 2: Candidates carry the pair

`FCandidate` stops carrying `ESource` and carries the two axes. Every source fills both. The arbiter ranks on the pair. **No behaviour change** — `IGuideSource::Kind()` still returns `ESource` and the settings still gate per source, so the existing suite is the proof.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/GuideArbiter.cpp:140-152`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp` (all eight sources)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/GuideArbiterTest.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/NetworkGuideSourceTest.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/OffsetGuideTest.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/SnapGuideChainTest.cpp`

**Interfaces:**
- Consumes: `SnapGuide::ERelation`, `SnapGuide::EReference` from Task 1.
- Produces: `FCandidate::Relation`, `FCandidate::Reference` (the `Source` field is gone). `IGuideSource::Kind()` is UNCHANGED and still returns `ESource` — Task 3 removes it.

- [ ] **Step 1: Write the failing test**

Append to `Plugins/Airside/Source/AirsideTests/Private/GuideGridTest.cpp`, before the `#endif`:

```cpp
/**
 * THE TIEBREAK IS A PAIR, RELATION FIRST. Two candidates equally close must not be separated
 * by the order the network happened to be walked in - that changes with an unrelated edit.
 * A stand's pose losing to the nearest road is the ONE ranking this split changes, and
 * FAlignedGuideSource's own header already argued for it: "the weakest of the four network
 * sources", which its old rank of third-of-eight contradicted.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideGridBreaksTiesByRelationThenReferenceTest,
	"Airside.Solve.GuideGridBreaksTiesByRelationThenReference",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideGridBreaksTiesByRelationThenReferenceTest::RunTest(const FString& Parameters)
{
	SnapGuide::FCandidate Road;
	Road.Direction = FVector2D(1.0, 0.0);
	Road.Through = FVector2D::ZeroVector;
	Road.Fit = SnapGuide::EFit::Angular;
	Road.Relation = SnapGuide::ERelation::Parallel;
	Road.Reference = SnapGuide::EReference::Road;

	// THE SAME DIRECTION, so the two are exactly tied on error and only the rank can separate
	// them. Listed AFTER the stand, so a first-wins bug shows up as the stand winning.
	SnapGuide::FCandidate Stand = Road;
	Stand.Reference = SnapGuide::EReference::Stand;

	const SnapGuide::FCandidate Candidates[] = { Stand, Road };
	const SnapGuide::FResult Result = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, FVector2D(1000.0, 0.0), SnapGuide::FResult());

	if (!TestTrue(TEXT("a guide holds"), Result.bActive)) { return false; }
	TestEqual(TEXT("the nearest road beats a stand's pose on a tie"),
		static_cast<int32>(Result.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Road));

	// AND RELATION OUTRANKS REFERENCE. Extending x ThisGesture is a worse reference rank than
	// Parallel x Road, and must still win - otherwise the pair is being compared the wrong way
	// round, which a reference-only test could not tell apart.
	SnapGuide::FCandidate Extending = Road;
	Extending.Relation = SnapGuide::ERelation::Extending;
	Extending.Reference = SnapGuide::EReference::ThisGesture;

	const SnapGuide::FCandidate Both[] = { Road, Extending };
	const SnapGuide::FResult Second = SnapGuide::Arbitrate(
		Both, FVector2D::ZeroVector, FVector2D(1000.0, 0.0), SnapGuide::FResult());

	if (!TestTrue(TEXT("a guide holds"), Second.bActive)) { return false; }
	TestEqual(TEXT("what you are extending beats the road you are beside"),
		static_cast<int32>(Second.Winners[0].Relation),
		static_cast<int32>(SnapGuide::ERelation::Extending));

	return true;
}
```

- [ ] **Step 2: Build and run to verify it fails**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve.GuideGrid
```

Expected: compile error — `FCandidate` has no member `Relation`.

- [ ] **Step 3: Replace the field on `FCandidate`**

In `GuideArbiter.h`, inside `struct FCandidate`, replace the line `ESource Source = ESource::World;` with:

```cpp
		/**
		 * WHAT this guide means, and WHAT it is measured against. Two fields since 2026-09-20;
		 * one `ESource` before, which is why a player could switch Runway off and still be told
		 * their taxiway was parallel to one - see EReference's own comment.
		 *
		 * THE DEFAULT IS THE WORLD GRID, exactly as `ESource::World` was: a candidate built
		 * without saying what it is should be the least specific thing on the list, never the
		 * most.
		 */
		ERelation Relation = ERelation::Parallel;
		EReference Reference = EReference::World;
```

- [ ] **Step 4: Rank on the pair in the arbiter**

In `Plugins/Airside/Source/Airside/Private/Solve/GuideArbiter.cpp`, add to the anonymous namespace at the top, after `ErrorDegrees`:

```cpp
	/**
	 * Whether A outranks B - relation first, then reference. Design section 4.
	 *
	 * LEXICOGRAPHIC, NOT A COMBINED INDEX. `Relation * 6 + Reference` would work today and
	 * would break silently the first time a sixth reference is added, which is exactly the
	 * class of bug the grid exists to make impossible.
	 */
	bool OutranksOnTie(const SnapGuide::FCandidate& A, const SnapGuide::FCandidate& B)
	{
		return A.Relation != B.Relation ? A.Relation < B.Relation : A.Reference < B.Reference;
	}
```

Then in `RunRace`, replace `&& Candidate.Source < Best->Source;` with:

```cpp
				&& OutranksOnTie(Candidate, *Best);
```

- [ ] **Step 5: Tag every source's candidates**

In `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp`, replace each `X.Source = SnapGuide::ESource::Y;` line with the pair below. Every other line in each function is unchanged.

| Existing line | Replace with |
|---|---|
| `Parallel.Source = SnapGuide::ESource::Extending;` (in `FExtendingGuideSource`) | `Parallel.Relation = SnapGuide::ERelation::Extending;`<br>`Parallel.Reference = SnapGuide::EReference::ThisGesture;` |
| `Candidate.Source = SnapGuide::ESource::World;` | `Candidate.Relation = SnapGuide::ERelation::Parallel;`<br>`Candidate.Reference = SnapGuide::EReference::World;` |
| `Level.Source = SnapGuide::ESource::PointAlign;` | `Level.Relation = SnapGuide::ERelation::LevelWith;`<br>`Level.Reference = SnapGuide::EReference::ThisGesture;` |
| `Along.Source = SnapGuide::ESource::Parallel;` | `Along.Relation = SnapGuide::ERelation::Parallel;`<br>`Along.Reference = ReferenceFor(Network, Nearest);` |
| `InLine.Source = SnapGuide::ESource::Collinear;` | `InLine.Relation = SnapGuide::ERelation::Collinear;`<br>`InLine.Reference = ReferenceFor(Network, Id);` |
| `Along.Source = SnapGuide::ESource::Runway;` (in `FRunwayGuideSource`) | `Along.Relation = SnapGuide::ERelation::Parallel;`<br>`Along.Reference = SnapGuide::EReference::Runway;` |
| `Along.Source = SnapGuide::ESource::Aligned;` | `Along.Relation = SnapGuide::ERelation::Parallel;`<br>`Along.Reference = SnapGuide::EReference::Stand;` |
| `Match.Source = SnapGuide::ESource::Offset;` | `Match.Relation = SnapGuide::ERelation::MatchingGap;`<br>`Match.Reference = ReferenceFor(Network, Reference);` |

Add `ReferenceFor` to the anonymous namespace at the top of the same file, after `ClosestOn`:

```cpp
	/**
	 * Which column a segment belongs to. A runway is not a type in the model - it is any
	 * segment whose profile is continuous through junctions - so the ONE test that decides it
	 * lives in URoadNetwork and is asked here rather than re-derived.
	 *
	 * TAGGING RATHER THAN SKIPPING, at this stage: the three segment sources still propose for
	 * runways exactly as they did before, so this task changes no behaviour. Task 3 gates on
	 * the tag and Task 5 partitions the geometry.
	 */
	SnapGuide::EReference ReferenceFor(const URoadNetwork& Network, FRoadSegmentId Segment)
	{
		return Network.IsRunwaySegment(Segment)
			? SnapGuide::EReference::Runway : SnapGuide::EReference::Road;
	}
```

Note `FOffsetGuideSource`'s local variable is already named `Reference` (an `FRoadSegmentId`), so `ReferenceFor(Network, Reference)` reads correctly there and does not collide with the new field name, which is qualified as `Match.Reference`.

- [ ] **Step 6: Update the four existing test files**

Every `Winners[0].Source` comparison against an `ESource` becomes a comparison against `.Relation` or `.Reference`. Apply this mapping wherever `SnapGuide::ESource::X` appears in a test:

| Was asserted | Assert instead |
|---|---|
| `ESource::Extending` | `.Relation` is `ERelation::Extending` |
| `ESource::PointAlign` | `.Relation` is `ERelation::LevelWith` |
| `ESource::Aligned` | `.Reference` is `EReference::Stand` |
| `ESource::Collinear` | `.Relation` is `ERelation::Collinear` |
| `ESource::Parallel` | `.Relation` is `ERelation::Parallel` **and** `.Reference` is `EReference::Road` |
| `ESource::Runway` | `.Reference` is `EReference::Runway` |
| `ESource::World` | `.Reference` is `EReference::World` |
| `ESource::Offset` | `.Relation` is `ERelation::MatchingGap` |

`Airside.Solve.GuideArbiterBreaksTiesBySource` must also be renamed to `Airside.Solve.GuideArbiterBreaksTiesByRank`, both its test name string and its `IMPLEMENT_SIMPLE_AUTOMATION_TEST` class name — the old name now describes a field that does not exist.

- [ ] **Step 7: Build and run the whole guide suite**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\airportmgr2_snapping\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
./Tools/Run-AirsideTests.ps1 -Filter Airside
```

Expected: every previously-passing test still passes, plus the two new grid tests. **This is the proof that Task 2 changed no behaviour** — if any pre-existing test fails, the mapping in Step 5 is wrong; fix the mapping, do not adjust the test.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "refactor(airside): a guide candidate carries its relation and its reference"
```

---

### Task 3: Gate on both axes

`FSnapGuideSettings` becomes five relation flags and five reference flags. `ESource` is deleted. This is the fix: the Runway column silences every relation.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideSettings.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideSettings.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h` (`Kind()` → `Relation()`)
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp` (`FSnapGuideChain::Resolve`)
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h` (delete `ESource`)
- Modify: `Source/AirportMgr/RoadBuildController.h:190-193`, `Source/AirportMgr/RoadBuildController.cpp:480-492`
- Modify: `Source/AirportMgr/BuildActions.cpp:157-201`
- Modify: `Source/AirportMgr/BuildActionsTest.cpp:164-209`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/GuideToggleTest.cpp`

**Interfaces:**
- Consumes: `IsLegalCell`, `FCandidate::Relation`/`Reference` from Tasks 1 and 2.
- Produces: `FSnapGuideSettings::IsEnabled(ERelation, EReference)`, `FSnapGuideSettings::ToggleRelation(ERelation)`, `FSnapGuideSettings::ToggleReference(EReference)`, `IGuideSource::Relation()`. On the controller: `ToggleGuideRelation(ERelation)`, `IsGuideRelationOn(ERelation) const`, `ToggleGuideReference(EReference)`, `IsGuideReferenceOn(EReference) const`.

- [ ] **Step 1: Share the guide test helpers**

`LayRunway`, `BareAnchor` and `ProposedBy` live in the anonymous namespace of
`NetworkGuideSourceTest.cpp`. The tests in this task and in Task 5 need them, and the test
module is a UNITY build: a second `LayRunway` in another file's anonymous namespace compiles
perfectly alone and collides the moment the two land in the same blob. That is the trap
`AirsideTestFixtures.h` exists to close, so move them there rather than copying them.

Cut all three out of `NetworkGuideSourceTest.cpp` and put them in
`Plugins/Airside/Source/AirsideTests/Private/AirsideTestFixtures.h` (declarations) and
`AirsideTestFixtures.cpp` (bodies), in a new namespace beside the existing `TestTool`:

```cpp
/** Guide-source builders shared by every guide test. Moved out of NetworkGuideSourceTest.cpp
 *  on 2026-09-20: three files need them, and the test module is a unity build. */
namespace TestGuide
{
	/** An anchor with no reference and no points, so ONLY the network sources answer. */
	FGuideAnchor BareAnchor(const FVector2D& Origin);

	/**
	 * A runway strip. NOT ConnectNodes: ERoadKind has only Taxiway and ServiceRoad, because a
	 * runway is not a road kind - it is a segment placed through PlaceRunway with a runway
	 * profile, which is what URoadNetwork::IsRunwaySegment then recognises.
	 *
	 * MinimumRunwayLength is dropped first: it defaults to 50000 uu and PlaceRunway refuses
	 * anything under it, so a test strip either lowers the bar or is half a kilometre long.
	 */
	bool LayRunway(ARoadNetworkActor* Actor, const FVector2D& From, const FVector2D& To);

	/** Every candidate ONE source proposes, with the rest of the chain kept out of it. */
	TArray<SnapGuide::FCandidate> ProposedBy(const IGuideSource& Source,
		const URoadNetwork& Network, const FGuideAnchor& Anchor);
}
```

Move the bodies VERBATIM, comments included - `LayRunway`'s "A NODE FIRST, PURELY TO BRING THE
NETWORK INTO BEING" paragraph records a crash and its cause, and the refactor contract says a
comment travels with its code. Then qualify the call sites left behind in
`NetworkGuideSourceTest.cpp` with `TestGuide::`.

- [ ] **Step 2: Build and run the existing guide tests, unchanged**

Build with the Global Constraints command, then:

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool
```

Expected: exactly the same tests run and pass as before the move. A move that changes the run
count has dropped a test to a name collision - find it before going on.

- [ ] **Step 3: Write the failing test**

Append to `Plugins/Airside/Source/AirsideTests/Private/GuideToggleTest.cpp`, before the final `#endif`:

```cpp
/**
 * THE REPORT, AS A TEST. 2026-09-20: "i have parallel on and runway off [and] the guide will
 * still show me that my taxiway is parallel to a runway". A column switched off must silence
 * every relation in it, not just the one relation that happened to be named after it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayColumnOffSilencesEveryRelationTest,
	"Airside.Tool.RunwayColumnOffSilencesEveryRelation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayColumnOffSilencesEveryRelationTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A RUNWAY AS THE ONLY THING ON THE FIELD, so anything the chain offers must reference it.
	//
	// NOT ConnectNodes: ERoadKind has only Taxiway and ServiceRoad, because a runway is not a
	// road kind - it is a segment laid through PlaceRunway with a profile that is continuous
	// through junctions, which is what IsRunwaySegment then recognises. TestGuide::LayRunway
	// also drops MinimumRunwayLength, which defaults to 50000 uu and refuses anything shorter.
	if (!TestTrue(TEXT("the runway is laid"),
		TestGuide::LayRunway(Actor, FVector2D(-20000.0, 0.0), FVector2D(20000.0, 0.0))))
	{
		// HONOURED, NOT ASSUMED. PlaceRunway can refuse, and every assertion below would then
		// be measuring an empty field while looking like a gating bug.
		return false;
	}
	if (!TestTrue(TEXT("and the network exists"), Actor->Network != nullptr)) { return false; }

	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(0.0, 3000.0);
	const FVector2D Cursor(4000.0, 3100.0);
	const FSnapGuideChain Chain;

	// EVERY RELATION ON, THE RUNWAY COLUMN OFF. Road stays on so the test cannot pass merely
	// by everything being switched off.
	FSnapGuideSettings Settings;
	Settings.bParallel = true;
	Settings.bCollinear = true;
	Settings.bMatchingGap = true;
	Settings.bRoad = true;
	Settings.bRunway = false;
	Settings.bWorld = false;

	const SnapGuide::FResult Off = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	for (const SnapGuide::FCandidate& Winner : Off.Winners)
	{
		TestNotEqual(TEXT("with Runway off, no relation may reference a runway"),
			static_cast<int32>(Winner.Reference),
			static_cast<int32>(SnapGuide::EReference::Runway));
	}

	// CONTROL LEG: the drag itself was fine. Without this, a Resolve that had simply broken
	// would pass the assertion above.
	Settings.bRunway = true;
	const SnapGuide::FResult On = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	if (!TestTrue(TEXT("with Runway on, the runway answers"), On.bActive)) { return false; }
	TestEqual(TEXT("and what it offers is the runway"),
		static_cast<int32>(On.Winners[0].Reference),
		static_cast<int32>(SnapGuide::EReference::Runway));

	return true;
}
```

- [ ] **Step 4: Build and run to verify it fails**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool.RunwayColumnOff
```

Expected: compile error — `FSnapGuideSettings` has no member `bRoad`.

- [ ] **Step 5: Rewrite the settings struct**

Replace the body of `FSnapGuideSettings` in `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideSettings.h` (keep the struct's existing doc comment, and extend it with the paragraph below):

```cpp
	// --- What a guide MEANS -----------------------------------------------------------

	/** The edge the gesture is already extending, and its perpendicular. */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bExtending = true;

	/** Lines through a point worth being level with. On with Extending: the same geometry. */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bLevelWith = true;

	/** A direction to point along, and its perpendicular. */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bParallel = true;

	/** The line an existing thing already lies on. */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bCollinear = false;

	/** The gap a neighbouring parallel road already keeps. */
	UPROPERTY(EditAnywhere, Category = "Align by")
	bool bMatchingGap = false;

	// --- What it is measured AGAINST --------------------------------------------------

	/**
	 * Taxiways and service roads.
	 *
	 * NEW ON 2026-09-20, and it is the column that had no switch: Parallel and Collinear each
	 * carried "a road" as an unnamed reference, which is half of why Runway could not mean what
	 * it was read to mean. ThisGesture is the only column with NO flag - see the design §7.
	 */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bRoad = true;

	/** Runways - any segment whose profile is continuous through junctions. */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bRunway = false;

	/** Apron edges and corners. */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bApron = false;

	/** A placed entity's pose - a stand, a depot. Was `bAligned`, which named a relation. */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bStand = false;

	/** 0/45/90/135 degrees. */
	UPROPERTY(EditAnywhere, Category = "Snap to")
	bool bWorld = true;

	/**
	 * Whether this CELL may propose: both axes on, and the pair legal.
	 *
	 * THE AND IS THE WHOLE FIX. A column switched off removes every row in it, which is what
	 * the 2026-09-20 report asked for. IsLegalCell is consulted as well as the two flags so a
	 * hole cannot be reached by switching both its axes on.
	 */
	bool IsEnabled(SnapGuide::ERelation Relation, SnapGuide::EReference Reference) const;

	/** Whether this relation may propose at all, ignoring references. The bar's ALIGN BY row. */
	bool IsRelationOn(SnapGuide::ERelation Relation) const;

	/** Whether this reference may be used at all. The bar's SNAP TO row. */
	bool IsReferenceOn(SnapGuide::EReference Reference) const;

	void ToggleRelation(SnapGuide::ERelation Relation);
	void ToggleReference(SnapGuide::EReference Reference);
```

Also update the struct's `NAMED BOOLS, NOT AN ARRAY INDEXED BY ESource` paragraph to say `ERelation`/`EReference` rather than `ESource` — the reasoning is unchanged, only the type names.

- [ ] **Step 6: Rewrite the settings implementation**

Replace the whole contents of `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideSettings.cpp` below the include with:

```cpp
bool FSnapGuideSettings::IsRelationOn(SnapGuide::ERelation Relation) const
{
	// NO `default:`, as before: a relation added without a case falls past the switch and is
	// OFF, which the registry test then says out loud rather than leaving a guide that quietly
	// never fires.
	switch (Relation)
	{
	case SnapGuide::ERelation::Extending:   return bExtending;
	case SnapGuide::ERelation::LevelWith:   return bLevelWith;
	case SnapGuide::ERelation::Parallel:    return bParallel;
	case SnapGuide::ERelation::Collinear:   return bCollinear;
	case SnapGuide::ERelation::MatchingGap: return bMatchingGap;
	}
	return false;
}

bool FSnapGuideSettings::IsReferenceOn(SnapGuide::EReference Reference) const
{
	switch (Reference)
	{
	// THE ONE COLUMN WITH NO FLAG. Extending is the only cell in its row, so a ThisGesture
	// button and an Extending button would switch off exactly the same behaviour - two
	// controls for one thing. The Alt hold covers "not for this drag". See design §7.
	case SnapGuide::EReference::ThisGesture: return true;
	case SnapGuide::EReference::Road:        return bRoad;
	case SnapGuide::EReference::Runway:      return bRunway;
	case SnapGuide::EReference::Apron:       return bApron;
	case SnapGuide::EReference::Stand:       return bStand;
	case SnapGuide::EReference::World:       return bWorld;
	}
	return false;
}

bool FSnapGuideSettings::IsEnabled(SnapGuide::ERelation Relation,
	SnapGuide::EReference Reference) const
{
	return SnapGuide::IsLegalCell(Relation, Reference)
		&& IsRelationOn(Relation) && IsReferenceOn(Reference);
}

void FSnapGuideSettings::ToggleRelation(SnapGuide::ERelation Relation)
{
	switch (Relation)
	{
	case SnapGuide::ERelation::Extending:   bExtending   = !bExtending;   return;
	case SnapGuide::ERelation::LevelWith:   bLevelWith   = !bLevelWith;   return;
	case SnapGuide::ERelation::Parallel:    bParallel    = !bParallel;    return;
	case SnapGuide::ERelation::Collinear:   bCollinear   = !bCollinear;   return;
	case SnapGuide::ERelation::MatchingGap: bMatchingGap = !bMatchingGap; return;
	}
}

void FSnapGuideSettings::ToggleReference(SnapGuide::EReference Reference)
{
	switch (Reference)
	{
	// ThisGesture has no flag and so cannot be toggled - see IsReferenceOn.
	case SnapGuide::EReference::ThisGesture: return;
	case SnapGuide::EReference::Road:        bRoad   = !bRoad;   return;
	case SnapGuide::EReference::Runway:      bRunway = !bRunway; return;
	case SnapGuide::EReference::Apron:       bApron  = !bApron;  return;
	case SnapGuide::EReference::Stand:       bStand  = !bStand;  return;
	case SnapGuide::EReference::World:       bWorld  = !bWorld;  return;
	}
}
```

- [ ] **Step 7: Gate the chain on the cell**

In `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h`, rename `virtual SnapGuide::ESource Kind() const = 0;` to `virtual SnapGuide::ERelation Relation() const = 0;` and update all eight overrides:

| Source | New override body |
|---|---|
| `FExtendingGuideSource` | `return SnapGuide::ERelation::Extending;` |
| `FPointAlignGuideSource` | `return SnapGuide::ERelation::LevelWith;` |
| `FAlignedGuideSource` | `return SnapGuide::ERelation::Parallel;` |
| `FCollinearGuideSource` | `return SnapGuide::ERelation::Collinear;` |
| `FParallelGuideSource` | `return SnapGuide::ERelation::Parallel;` |
| `FRunwayGuideSource` | `return SnapGuide::ERelation::Parallel;` |
| `FWorldGuideSource` | `return SnapGuide::ERelation::Parallel;` |
| `FOffsetGuideSource` | `return SnapGuide::ERelation::MatchingGap;` |

Update the `PURE VIRTUAL rather than a field` comment: its closing sentence — "Every source proposes candidates of exactly ONE ESource today" — is now false in the other direction and should read:

```cpp
	/**
	 * Which ERelation this link proposes. The toggle asks, and the chain skips it when off.
	 *
	 * PURE VIRTUAL rather than a field, so a source cannot be written without answering it.
	 *
	 * A RELATION, NOT A CELL. Several sources share one relation - Parallel is proposed by the
	 * road, runway, stand and world sources alike - and one source may span several REFERENCES,
	 * which is why the reference is tagged per candidate rather than declared here. The chain
	 * skips a source whose relation is off; a candidate whose reference is off is dropped as it
	 * is gathered.
	 */
	virtual SnapGuide::ERelation Relation() const = 0;
```

Then in `FSnapGuideChain::Resolve` in `SnapGuideChain.cpp`, replace the gathering loop with:

```cpp
	for (const TUniquePtr<IGuideSource>& Source : Sources)
	{
		// THE RELATION IS SKIPPED BEFORE IT WORKS, as before: Collinear walks every segment in
		// reach, and doing that to discard the result is waste.
		if (!Enabled.IsRelationOn(Source->Relation()))
		{
			continue;
		}

		const int32 Before = Candidates.Num();
		Source->Propose(Network, Anchor, Candidates);

		// THE REFERENCE IS FILTERED AFTER, and only for what this source just added. A source
		// may span columns - the segment walkers tag Road or Runway per segment - so there is
		// no single column to skip up front. Only the newly-added range is examined, so this
		// stays linear however many sources have already answered.
		for (int32 Index = Candidates.Num() - 1; Index >= Before; --Index)
		{
			if (!Enabled.IsEnabled(Candidates[Index].Relation, Candidates[Index].Reference))
			{
				Candidates.RemoveAtSwap(Index, 1, EAllowShrinking::No);
			}
		}
	}
```

- [ ] **Step 8: Delete `ESource` and rewire its callers**

Delete the `enum class ESource` block from `GuideArbiter.h` — Task 1 already replaced it, so confirm no declaration remains.

In `Source/AirportMgr/RoadBuildController.h`, replace the two guide members with four:

```cpp
	/** Flips one row of the guide grid. What an ALIGN BY button does. */
	void ToggleGuideRelation(SnapGuide::ERelation Relation);

	/** Whether that row is lit. */
	bool IsGuideRelationOn(SnapGuide::ERelation Relation) const;

	/** Flips one column. What a SNAP TO button does. */
	void ToggleGuideReference(SnapGuide::EReference Reference);

	/** Whether that column is lit. */
	bool IsGuideReferenceOn(SnapGuide::EReference Reference) const;
```

In `RoadBuildController.cpp`, replace `ToggleGuideSource`/`IsGuideSourceOn` with the four, each forwarding to `Actor->GuideSources` exactly as the two did — `ToggleRelation`/`IsRelationOn`/`ToggleReference`/`IsReferenceOn`. Keep the existing null-Actor guard (`return Actor != nullptr && ...`).

In `Source/AirportMgr/BuildActions.cpp`, replace the eight `snap.*` registrations with ten. All ten stay in `EActionSection::Snap` for now — Task 4 splits the sections. Ids and labels:

| Id | Label | Execute | IsActive |
|---|---|---|---|
| `snap.extending` | "Extending" | `C.ToggleGuideRelation(SnapGuide::ERelation::Extending)` | `C.IsGuideRelationOn(SnapGuide::ERelation::Extending)` |
| `snap.levelwith` | "Level with" | `...ERelation::LevelWith` | `...ERelation::LevelWith` |
| `snap.parallel` | "Parallel" | `...ERelation::Parallel` | `...ERelation::Parallel` |
| `snap.collinear` | "Collinear" | `...ERelation::Collinear` | `...ERelation::Collinear` |
| `snap.matchinggap` | "Matching gap" | `...ERelation::MatchingGap` | `...ERelation::MatchingGap` |
| `snapto.road` | "Road" | `C.ToggleGuideReference(SnapGuide::EReference::Road)` | `C.IsGuideReferenceOn(SnapGuide::EReference::Road)` |
| `snapto.runway` | "Runway" | `...EReference::Runway` | `...EReference::Runway` |
| `snapto.apron` | "Apron" | `...EReference::Apron` | `...EReference::Apron` |
| `snapto.stand` | "Stand" | `...EReference::Stand` | `...EReference::Stand` |
| `snapto.world` | "World" | `...EReference::World` | `...EReference::World` |

Each keeps `EKeys::Invalid, false, ... , Always` exactly as the eight did, and the `NO KEYS` comment above them stays.

- [ ] **Step 9: Update the two existing toggle tests**

In `GuideToggleTest.cpp`, `Airside.Tool.GuideSettingsGiveEverySourceItsOwnFlag` asserts the defaults. Rename it to `Airside.Tool.GuideSettingsGiveEveryAxisItsOwnFlag` and assert the new defaults: `bExtending`, `bLevelWith`, `bParallel`, `bRoad` and `bWorld` true; `bCollinear`, `bMatchingGap`, `bRunway`, `bApron`, `bStand` false.

`Airside.Tool.GuideChainSkipsADisabledSource` needs one edit: it sets `Settings.bParallel = true` with everything else off, and must now also set `Settings.bRoad = true`, or the road it expects to answer is gated off by its column and the test fails for the right reason in the wrong place.

In `BuildActionsTest.cpp`, `SnapTogglesAreInTheRegistry` will not compile. Leave it broken here ONLY if Task 4 follows immediately; otherwise update its table to the ten ids above. Prefer updating it now — a plan that leaves the suite red between tasks cannot prove the next task's green.

- [ ] **Step 10: Build and run the whole suite**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\airportmgr2_snapping\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
./Tools/Run-AirsideTests.ps1
```

Expected: `Airside.Tool.RunwayColumnOffSilencesEveryRelation` passes, and the `N test(s) run, N failed, N crashed` line reports zero failed and zero crashed.

- [ ] **Step 11: Commit**

```bash
git add -A
git commit -m "fix(airside): a guide is offered only when its relation and its reference are both on

Switching Runway off left Parallel, Collinear and Offset free to pick a runway as
the nearest road and label it 'parallel to runway 18/36', because ESource mixed
relationships with references and there was no axis for the toggle to act along."
```

---

### Task 4: The bar's two lists

Ten buttons in two sections, so the grid's two axes are visible as two axes.

**Files:**
- Modify: `Source/AirportMgr/BuildActions.h:12-25` (`EActionSection`)
- Modify: `Source/AirportMgr/BuildActions.cpp:12-18` (`SectionNames`), and the ten registrations
- Modify: `Source/AirportMgr/BuildBarWidget.h` (a `SnapToSection` UPROPERTY)
- Modify: `Source/AirportMgr/BuildBarWidget.cpp:45-56` (`SectionSpecs`)
- Modify: `Source/AirportMgr/BuildActionsTest.cpp`
- Modify: the `UUIStyle` asset's `IconsByActionId` map (editor work, see Step 5)

**Interfaces:**
- Consumes: the ten action ids from Task 3.
- Produces: `EActionSection::SnapTo`, named `"Snap to"`.

- [ ] **Step 1: Write the failing test**

Replace `FSnapTogglesAreInTheRegistryTest` in `Source/AirportMgr/BuildActionsTest.cpp` with:

```cpp
/**
 * AirportMgr.Actions.GuideGridIsInTheRegistry: a row or column with no button is a guide the
 * player cannot switch, and nothing else would say so. Replaces SnapTogglesAreInTheRegistry,
 * which walked ESource - an enum that no longer exists, and whose single axis is the defect
 * the grid was built to remove.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideGridIsInTheRegistryTest,
	"AirportMgr.Actions.GuideGridIsInTheRegistry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideGridIsInTheRegistryTest::RunTest(const FString& Parameters)
{
	const TArray<TPair<SnapGuide::ERelation, const TCHAR*>> Relations = {
		{ SnapGuide::ERelation::Extending,   TEXT("snap.extending")   },
		{ SnapGuide::ERelation::LevelWith,   TEXT("snap.levelwith")   },
		{ SnapGuide::ERelation::Parallel,    TEXT("snap.parallel")    },
		{ SnapGuide::ERelation::Collinear,   TEXT("snap.collinear")   },
		{ SnapGuide::ERelation::MatchingGap, TEXT("snap.matchinggap") } };

	// THISGESTURE IS ABSENT ON PURPOSE and the count below is what keeps that deliberate: it
	// is checked against the enum MINUS ONE, so a sixth reference added without a button still
	// fails here. See the 2026-09-20 design section 7 for why that one column has no switch.
	const TArray<TPair<SnapGuide::EReference, const TCHAR*>> References = {
		{ SnapGuide::EReference::Road,   TEXT("snapto.road")   },
		{ SnapGuide::EReference::Runway, TEXT("snapto.runway") },
		{ SnapGuide::EReference::Apron,  TEXT("snapto.apron")  },
		{ SnapGuide::EReference::Stand,  TEXT("snapto.stand")  },
		{ SnapGuide::EReference::World,  TEXT("snapto.world")  } };

	TestEqual(TEXT("every ERelation is covered by this test's own table"),
		Relations.Num(), static_cast<int32>(SnapGuide::ERelation::MatchingGap) + 1);
	TestEqual(TEXT("every EReference but ThisGesture is covered"),
		References.Num(), static_cast<int32>(SnapGuide::EReference::World));

	for (const TPair<SnapGuide::ERelation, const TCHAR*>& Pair : Relations)
	{
		const FBuildAction* Action = FindAction(FName(Pair.Value));
		if (!TestNotNull(*FString::Printf(TEXT("%s is registered"), Pair.Value), Action))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s sits in the Snap section"), Pair.Value),
			Action->Section, EActionSection::Snap);
		TestTrue(*FString::Printf(TEXT("%s can be executed"), Pair.Value),
			static_cast<bool>(Action->Execute));
		TestTrue(*FString::Printf(TEXT("%s reports whether it is lit"), Pair.Value),
			static_cast<bool>(Action->IsActive));
	}

	for (const TPair<SnapGuide::EReference, const TCHAR*>& Pair : References)
	{
		const FBuildAction* Action = FindAction(FName(Pair.Value));
		if (!TestNotNull(*FString::Printf(TEXT("%s is registered"), Pair.Value), Action))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s sits in the Snap to section"), Pair.Value),
			Action->Section, EActionSection::SnapTo);
		TestTrue(*FString::Printf(TEXT("%s can be executed"), Pair.Value),
			static_cast<bool>(Action->Execute));
	}

	// AND BOTH SECTIONS HAVE A NAME. ActionSectionName indexes SectionNames by the enum, so a
	// row added in the wrong slot renames two sections at once and the static_assert cannot see
	// it. SENTENCE CASE, like every other row.
	TestEqual(TEXT("the Snap section is named"),
		FString(ActionSectionName(EActionSection::Snap)), FString(TEXT("Snap")));
	TestEqual(TEXT("the Snap to section is named"),
		FString(ActionSectionName(EActionSection::SnapTo)), FString(TEXT("Snap to")));

	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

```
./Tools/Run-AirsideTests.ps1 -Filter AirportMgr.Actions
```

Expected: compile error — `EActionSection` has no member `SnapTo`.

- [ ] **Step 3: Add the section**

In `Source/AirportMgr/BuildActions.h`, add after `Snap`:

```cpp
	/** The guide REFERENCES: what a guide is measured against. See the 2026-09-20 design §7. */
	SnapTo,
```

In `Source/AirportMgr/BuildActions.cpp`, add `TEXT("Snap to"),` to `SectionNames` after `TEXT("Snap")`. The existing `static_assert` on its length catches a mismatch at compile time.

Move the five `snapto.*` registrations from `EActionSection::Snap` to `EActionSection::SnapTo`, and update the `Snap` section's own comment to say it now holds the relations only.

- [ ] **Step 4: Give the bar a slot for it**

In `Source/AirportMgr/BuildBarWidget.h`, add beside the existing `SnapSection` UPROPERTY:

```cpp
	UPROPERTY(Transient, meta = (BindWidgetOptional))
	TObjectPtr<UPanelWidget> SnapToSection;
```

Match the exact specifiers on `SnapSection` in that file rather than copying the line above blindly — if they differ, the existing one is right and this is wrong.

In `Source/AirportMgr/BuildBarWidget.cpp`, add to `SectionSpecs`:

```cpp
		{ EActionSection::SnapTo,    &UBuildBarWidget::SnapToSection },
```

The `static_assert` on line 55 binds this list to `EActionSection::Count`; if it fires, the slot above is missing.

- [ ] **Step 5: Map the icons**

`AirportMgr.UI.EveryActionResolvesAnIcon` exempts only the Time section, so all ten ids need an entry in the `UUIStyle` asset's `IconsByActionId` map. Eight glyphs already exist under the old ids and are re-keyed:

| Old id | New id |
|---|---|
| `snap.extending` | unchanged |
| `snap.pointalign` | `snap.levelwith` |
| `snap.parallel` | unchanged |
| `snap.collinear` | unchanged |
| `snap.offset` | `snap.matchinggap` |
| `snap.aligned` | `snapto.stand` |
| `snap.runway` | `snapto.runway` |
| `snap.world` | `snapto.world` |

Two are genuinely new and need artwork: `snapto.road` and `snapto.apron`.

This is an asset edit, not code. Open the `UUIStyle` asset in the editor, re-key the eight rows and add the two. If the two glyphs do not exist yet, point them at the `snap.parallel` texture as a placeholder so the suite is green, and raise the artwork separately — a red suite blocks every later task, and a duplicated glyph is visibly wrong rather than silently missing.

- [ ] **Step 6: Build and run the whole suite**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\airportmgr2_snapping\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
./Tools/Run-AirsideTests.ps1
```

Expected: zero failed, zero crashed, including `AirportMgr.UI.EveryActionResolvesAnIcon`.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(airportmgr): the bar shows the guide grid's two axes as two lists"
```

---

### Task 5: Partition the runway geometry

The Runway column owns runway segments, with its own search policy. The other three segment sources skip them, and the extended centreline moves to where it belongs.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp` (`FParallelGuideSource`, `FCollinearGuideSource`, `FOffsetGuideSource`, `FRunwayGuideSource`)
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h` (`FRunwayGuideSource`'s doc comment)
- Create: `Plugins/Airside/Source/AirsideTests/Private/RunwayColumnTest.cpp`

**Interfaces:**
- Consumes: `ReferenceFor` from Task 2, the gating from Task 3.
- Produces: nothing new. `FRunwayGuideSource` now proposes three candidates per runway rather than two.

- [ ] **Step 1: Write the failing tests**

Create `Plugins/Airside/Source/AirsideTests/Private/RunwayColumnTest.cpp`:

```cpp
#include "Misc/AutomationTest.h"

#include "AirsideTestFixtures.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE EXTENDED CENTRELINE SURVIVES THE PARTITION.
 *
 * FCollinearGuideSource used to offer "in line with runway 18/36" by accident - it walked every
 * segment, runways included - so the line existed but answered to the Collinear toggle rather
 * than the Runway one. Skipping runways there without moving it here would have DELETED it,
 * which is the loss this test exists to make loud.
 *
 * UNBOUNDED, unlike Collinear's own: a runway's extended centreline is the approach path.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayGuideOffersItsOwnLineTest,
	"Airside.Tool.RunwayGuideOffersItsOwnLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayGuideOffersItsOwnLineTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A runway along +X through Y=0. NORTH IS +X in this project, so this strip is 18/36.
	if (!TestTrue(TEXT("the runway is laid"),
		TestGuide::LayRunway(Actor, FVector2D(-40000.0, 0.0), FVector2D(40000.0, 0.0))))
	{
		return false;
	}
	if (!TestTrue(TEXT("and the network exists"), Actor->Network != nullptr)) { return false; }

	// THE ORIGIN IS 500 m OUT, far beyond SearchRadiusUu, where every reach-limited source has
	// given up. The CURSOR is on the runway's own line - which is what a perpendicular fit
	// measures, however far along it the drag has gone.
	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(60000.0, 50000.0);
	const FVector2D Cursor(60000.0, 0.0);

	FSnapGuideSettings Settings;
	Settings.bExtending = false;
	Settings.bLevelWith = false;
	Settings.bParallel = false;
	Settings.bCollinear = true;
	Settings.bMatchingGap = false;
	Settings.bRoad = false;
	Settings.bRunway = true;
	Settings.bApron = false;
	Settings.bStand = false;
	Settings.bWorld = false;

	const FSnapGuideChain Chain;
	const SnapGuide::FResult Result = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);

	if (!TestTrue(TEXT("the runway's own line reaches the whole field"), Result.bActive))
	{
		return false;
	}
	const SnapGuide::FCandidate* Winner = Result.Of(SnapGuide::EFit::Perpendicular);
	if (!TestNotNull(TEXT("and it is a perpendicular fit, not an angular one"), Winner))
	{
		return false;
	}
	TestEqual(TEXT("it is a Collinear guide"),
		static_cast<int32>(Winner->Relation),
		static_cast<int32>(SnapGuide::ERelation::Collinear));
	TestEqual(TEXT("against the Runway column, not the Road one"),
		static_cast<int32>(Winner->Reference),
		static_cast<int32>(SnapGuide::EReference::Runway));

	// CONTROL LEG: switch the Runway column off and the same drag is offered nothing. Without
	// this, a Resolve answering from some other source would pass the assertions above.
	Settings.bRunway = false;
	const SnapGuide::FResult Off = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	TestFalse(TEXT("with the Runway column off, nothing answers"), Off.bActive);

	return true;
}

/**
 * OFFSET AND PARALLEL MUST NAME ONE ROAD BETWEEN THEM.
 *
 * FOffsetGuideSource's header records that it deliberately picks the same nearest road
 * FParallelGuideSource does, so "parallel to the taxiway" and "the same gap as its neighbour"
 * compose into one answer rather than two unrelated ones. Once Parallel excludes runways,
 * Offset must too - otherwise the two disagree about which road they are talking about, and
 * nothing else in the suite would say so.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOffsetAndParallelNameOneRoadTest,
	"Airside.Tool.OffsetAndParallelNameOneRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOffsetAndParallelNameOneRoadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A RUNWAY AND TWO TAXIWAYS, ALL PARALLEL. The runway is laid first so it holds the lowest
	// segment index - a source that picked by index rather than by distance would choose it,
	// and this test would catch that too.
	if (!TestTrue(TEXT("the runway is laid"),
		TestGuide::LayRunway(Actor, FVector2D(-40000.0, 12000.0), FVector2D(40000.0, 12000.0))))
	{
		return false;
	}

	IRoadEditTarget* Target = Actor;
	const int32 NearWest = Target->PlaceNode(FVector2D(-20000.0, 0.0));
	const int32 NearEast = Target->PlaceNode(FVector2D(20000.0, 0.0));
	Target->ConnectNodes(NearWest, NearEast, ERoadKind::Taxiway, INDEX_NONE);

	// The neighbour, 4000 uu south of the near taxiway - the gap Offset has to copy.
	const int32 FarWest = Target->PlaceNode(FVector2D(-20000.0, -4000.0));
	const int32 FarEast = Target->PlaceNode(FVector2D(20000.0, -4000.0));
	Target->ConnectNodes(FarWest, FarEast, ERoadKind::Taxiway, INDEX_NONE);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	// THE ORIGIN IS NEAREST THE FIRST TAXIWAY, and the cursor sits on the line one gap north
	// of it - away from the neighbour, which is the only side Offset ever proposes.
	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(0.0, 600.0);
	const FVector2D Cursor(8000.0, 4000.0);

	FSnapGuideSettings Settings;
	Settings.bExtending = false;
	Settings.bLevelWith = false;
	Settings.bParallel = true;
	Settings.bCollinear = false;
	Settings.bMatchingGap = true;
	Settings.bRoad = true;
	Settings.bRunway = true;
	Settings.bApron = false;
	Settings.bStand = false;
	Settings.bWorld = false;

	const FSnapGuideChain Chain;
	const SnapGuide::FResult Result = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);

	if (!TestTrue(TEXT("both guides hold"), Result.bActive)) { return false; }
	if (!TestEqual(TEXT("one winner per fit kind"), Result.Winners.Num(), 2)) { return false; }

	// THE RUNWAY COLUMN IS ON, so a source that had not been partitioned would be free to name
	// it - the runway is a segment like any other until IsRunwaySegment is asked.
	for (const SnapGuide::FCandidate& Winner : Result.Winners)
	{
		TestEqual(*FString::Printf(TEXT("%s names a road, not a runway"), *Winner.Description),
			static_cast<int32>(Winner.Reference),
			static_cast<int32>(SnapGuide::EReference::Road));
	}

	return true;
}

#endif
```

- [ ] **Step 2: Build twice, then run to verify both fail**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\airportmgr2_snapping\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool.RunwayGuide+Airside.Tool.OffsetAndParallel
```

Expected: `2 test(s) run, 2 failed`. If it reports `0 test(s) run`, build again — the new file was not compiled.

- [ ] **Step 3: Skip runways in the three road sources**

In `FParallelGuideSource::Propose`, inside the segment loop immediately after `const FRoadSegmentId Id = Network.SegmentIdAt(Index);`, add:

```cpp
		// THE RUNWAY COLUMN OWNS RUNWAYS, and owns them with a different search: every runway
		// proposes, from anywhere on the field, where this source takes the nearest one within
		// reach. Walking them here too would put two near-identical candidates into the same
		// race and would let a runway answer while the Runway column was switched off - which
		// is the 2026-09-20 report. See FRunwayGuideSource.
		if (Network.IsRunwaySegment(Id))
		{
			continue;
		}
```

Add the same guard to `FCollinearGuideSource::Propose` and to BOTH loops in `FOffsetGuideSource::Propose` (the reference search and the neighbour search). In `FOffsetGuideSource` the comment differs and should say why:

```cpp
		// ITS REFERENCE MUST BE THE ONE FParallelGuideSource PICKS, which now excludes runways.
		// A reference the two sources disagree about breaks the composition this source's own
		// header promises: "parallel to the taxiway" and "the same gap as its neighbour"
		// describing ONE road between them.
```

With every source skipping runways, `ReferenceFor` is now only ever called on non-runway segments from those three. Leave it in place and leave its call sites alone: it stays correct, and Task 5 of the SECOND plan (the apron source) has a use for it. Do NOT replace its calls with a literal `EReference::Road` — that would be a second statement of the same classification, which is what `RoadNaming` exists to prevent.

- [ ] **Step 4: Give the runway source its own line**

In `FRunwayGuideSource::Propose`, after the existing `Square` candidate is added, add:

```cpp
		// THE LINE THE RUNWAY LIES ON, which FCollinearGuideSource used to offer by accident -
		// it walked every segment, runways included, so "in line with runway 09/27" existed but
		// answered to the Collinear toggle rather than the Runway one. Partitioning runways out
		// of that source would have deleted it, so it moves here.
		//
		// UNBOUNDED, like this source's other two and unlike Collinear's: a runway's extended
		// centreline is the approach path, and it is meaningful from anywhere on the field.
		SnapGuide::FCandidate InLine;
		InLine.Direction = Along.Direction;
		InLine.Through = A;
		InLine.Fit = SnapGuide::EFit::Perpendicular;
		InLine.ReferenceAt = Along.ReferenceAt;
		InLine.Relation = SnapGuide::ERelation::Collinear;
		InLine.Reference = SnapGuide::EReference::Runway;
		InLine.Description = FString::Printf(TEXT("in line with %s"), *Name);
		Out.Add(InLine);
```

Update `FRunwayGuideSource`'s doc comment in `SnapGuideChain.h`: it says "every runway's heading, and its perpendicular", which is now two of three. Say what the third is and why it is here rather than in `FCollinearGuideSource`.

- [ ] **Step 5: Build and run the whole suite**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\airportmgr2_snapping\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
./Tools/Run-AirsideTests.ps1
```

Expected: zero failed, zero crashed. `Airside.Tool.RunwayGuideReachesTheWholeField` and `Airside.Tool.CollinearGuideIsNotParallel` are the two most likely to break — read them before changing them. If `CollinearGuideIsNotParallel` was relying on a runway, it was relying on the bug, and its fixture should become a taxiway.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "fix(airside): the Runway column owns runway geometry, extended centreline included"
```

---

### Task 6: The grid has no cell outside the list

The test that would have caught the original report. It asserts the thing no existing test could: that nothing proposes a pair the grid does not declare.

**Files:**
- Modify: `Plugins/Airside/Source/AirsideTests/Private/GuideGridTest.cpp`

**Interfaces:**
- Consumes: everything from Tasks 1 to 5.
- Produces: nothing.

- [ ] **Step 1: Write the test**

Append to `GuideGridTest.cpp`, before the `#endif`. It needs a world, so add `#include "AirsideTestFixtures.h"`, `#include "Present/RoadNetworkActor.h"`, `#include "Tool/SnapGuideChain.h"` and `#include "Tool/SnapGuideSettings.h"` to the file's includes.

```cpp
/**
 * NOTHING MAY PROPOSE A PAIR THE GRID DOES NOT DECLARE.
 *
 * THE TEST THE 2026-09-20 REPORT NEEDED. AirportMgr.Actions.SnapTogglesAreInTheRegistry walked
 * the enum against the button list and could never have caught it: Collinear x Runway was
 * firing while nothing declared that cell existed, because there was no notion of a cell. This
 * walks the other way - from what the chain actually produces, back to the list.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideGridHasNoCellOutsideTheListTest,
	"Airside.Tool.GuideGridHasNoCellOutsideTheList",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideGridHasNoCellOutsideTheListTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// ONE OF EACH THING A SOURCE CAN LOOK AT, all within reach of one origin, so every source
	// has something to answer with. A field holding only a taxiway would let a source proposing
	// into a hole pass simply for having nothing to propose about.
	//
	// NO APRON AND NO STAND YET. FApronGuideSource is the second plan, and placing an entity
	// needs a UEntityDefinition this fixture has no business authoring - so the Apron and Stand
	// columns are unexercised here, and this test gets stronger when that plan lands. Said out
	// loud because a test whose coverage is narrower than its name is how a green run comes to
	// mean nothing.
	if (!TestTrue(TEXT("the runway is laid"),
		TestGuide::LayRunway(Actor, FVector2D(-30000.0, 9000.0), FVector2D(30000.0, 9000.0))))
	{
		return false;
	}
	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(-10000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(10000.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);
	const int32 SouthWest = Target->PlaceNode(FVector2D(-10000.0, -6000.0));
	const int32 SouthEast = Target->PlaceNode(FVector2D(10000.0, -6000.0));
	Target->ConnectNodes(SouthWest, SouthEast, ERoadKind::Taxiway, INDEX_NONE);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(0.0, 3000.0);
	Anchor.Reference = FVector2D(1.0, 0.0);
	Anchor.ReferenceAt = FVector2D(-4000.0, 3000.0);
	Anchor.ReferenceName = TEXT("this road");
	Anchor.AlignTo.Add({ FVector2D(6000.0, 3000.0), TEXT("that node") });

	// EVERY ROW AND EVERY COLUMN ON, which is the only setting under which a source proposing
	// into a hole is visible at all: with anything switched off, the gate would remove the
	// illegal candidate for the wrong reason and the test would pass while the bug stood.
	FSnapGuideSettings Settings;
	Settings.bExtending = true;
	Settings.bLevelWith = true;
	Settings.bParallel = true;
	Settings.bCollinear = true;
	Settings.bMatchingGap = true;
	Settings.bRoad = true;
	Settings.bRunway = true;
	Settings.bApron = true;
	Settings.bStand = true;
	Settings.bWorld = true;

	// THE CHAIN'S OWN SOURCES, ASKED DIRECTLY. Resolve returns only the winners, and a winner
	// is at most two candidates - an illegal cell that lost its race would never be seen. So
	// each source is asked to propose into one array and every candidate in it is checked.
	const FSnapGuideChain Chain;
	TArray<SnapGuide::FCandidate> Everything;
	Chain.ProposeAll(*Actor->Network, Anchor, Settings, Everything);

	if (!TestTrue(TEXT("the sources proposed something to check"), Everything.Num() > 0))
	{
		return false;
	}

	for (const SnapGuide::FCandidate& Candidate : Everything)
	{
		TestTrue(*FString::Printf(TEXT("relation %d against reference %d is a declared cell (%s)"),
				static_cast<int32>(Candidate.Relation),
				static_cast<int32>(Candidate.Reference),
				*Candidate.Description),
			SnapGuide::IsLegalCell(Candidate.Relation, Candidate.Reference));
	}

	return true;
}
```

- [ ] **Step 2: Run to verify it fails**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Tool.GuideGridHasNoCell
```

Expected: compile error — `FSnapGuideChain` has no member `ProposeAll`.

- [ ] **Step 3: Expose the gathering half of `Resolve`**

`FSnapGuideChain::Resolve` gathers and then arbitrates. Split the gathering out so a test can see what arbitration discards. In `SnapGuideChain.h`, add to the public section above `Resolve`:

```cpp
	/**
	 * Every enabled source's candidates, gathered and gated but NOT arbitrated.
	 *
	 * FOR THE GRID TEST, and said plainly rather than hidden behind a friend declaration:
	 * Resolve returns at most two winners, so a source proposing into a hole would be
	 * invisible the moment it lost its race. Production callers want Resolve.
	 */
	void ProposeAll(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FSnapGuideSettings& Enabled, TArray<SnapGuide::FCandidate>& Out) const;
```

In `SnapGuideChain.cpp`, move the whole gathering loop out of `Resolve` into `ProposeAll`, and make `Resolve` call it:

```cpp
SnapGuide::FResult FSnapGuideChain::Resolve(const URoadNetwork& Network,
	const FGuideAnchor& Anchor, const FVector2D& Cursor,
	const SnapGuide::FResult& Previous, const FSnapGuideSettings& Enabled,
	const SnapGuide::FTuning& Tuning) const
{
	TArray<SnapGuide::FCandidate> Candidates;
	ProposeAll(Network, Anchor, Enabled, Candidates);
	return SnapGuide::Arbitrate(Candidates, Anchor.Origin, Cursor, Previous, Tuning);
}
```

Keep the `Candidates.Reserve(16)` call and its comment with the loop, in `ProposeAll`.

- [ ] **Step 4: Build and run to verify it passes**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\airportmgr2_snapping\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
./Tools/Run-AirsideTests.ps1
```

Expected: zero failed, zero crashed.

- [ ] **Step 5: Prove the test can fail**

A green test may measure nothing. Temporarily change `FWorldGuideSource::Propose` to set `Candidate.Relation = SnapGuide::ERelation::Collinear;` — Collinear x World is a declared hole, so the test must go RED. Run it, see it fail, then revert the change and see it pass again. Do not commit the temporary edit.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "test(airside): no source may propose a cell the grid does not declare"
```

---

## What this plan does not do

Named so the absence is deliberate, not an oversight. These are the second plan, against spec §6 and §8:

- Half-widths on `FGuideAnchor`, `DescribeGuideAnchor` taking an `IRoadEditTarget*`, and the flush-by-half-width rule for a centreline against a boundary.
- `FApronGuideSource` and the Apron column's three cells. Until it exists, `bApron` is a switch with nothing behind it — the same state `bOffset` was deliberately left in during the first snap-guides stage, and for the same reason: the button is registered so the wiring is proved, and the source arrives behind it.
- `FGuidePoint` carrying its own reference, needed before LevelWith can span columns.
- Guide anchors for `FRunwayTool` and `FApronDrawTool`.
