# Snap guides, stage 1: the chain, the arbiter, and a square plot corner

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A plot's back corners snap square to their frontage, with a dashed violet line
drawn to the edge they are squared to and a label saying why.

**Architecture:** `Solve/GuideArbiter` is a pure function over a candidate list plus last
frame's winner - tolerance, source-order tiebreak, hysteresis - so every flicker case is a
world-free test. `Tool/SnapGuideChain` gathers candidates from `IGuideSource`s (stage 1:
Extending and World) and calls it. `FBuildSession::MakeContext` - the ONE context builder
both drivers already funnel through for `Snap` - owns the chain, holds the previous winner,
and puts the answer on `FToolContext::Guide`. The tool declares what it is dragging
(`IBuildTool::DescribeGuideAnchor`) and reads `Context.GuidedCursor()`; it resolves nothing
and remembers no frame.

**Tech Stack:** UE 5.8.2 C++, Airside plugin, UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-17-snap-guides-design.md` - stage 1 of §8 only.

## Global Constraints

- **Worktree.** Every command runs from `C:\repos\AirportMgr2_snap-guides`. Never `cd` to
  `C:\repos\AirportMgr2`, which holds unrelated uncommitted work on another branch.
- **Build.** This is a worktree, so `-NoHotReloadFromIDE` is correct and a build here is safe
  with the main checkout's editor open:

  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2_snap-guides\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
  ```

  Check the literal line `Result: Succeeded`. A failed link leaves stale DLLs, so a test run
  straight after a failed build reports a green that means nothing.
- **Tests.**

  ```
  ./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides\AirportMgr.uproject"
  ```

  Read the `N test(s) run, N failed, N crashed` line. Never trust the exit code.
  `-Filter Airside.Solve` narrows. `Check-Architecture.ps1` runs first inside it.
- **Record the suite's test count BEFORE starting Task 1.** Run the test line once and
  keep the `N test(s) run` number. Every "up by N" below is measured against it, and a test
  that silently failed to register is otherwise invisible - see the automation tree's habit
  of dropping a bare-named test once a dotted child exists.
- **A new `.cpp` sometimes needs two builds** before it compiles - the first reports
  `Result: Succeeded` without having compiled it. If a new test does not appear in the run
  count, build again before debugging it.
- **Every task here needs a FULL build.** A new `UENUM` value, new members on `FToolContext`
  and `FBuildSession`, a new virtual on `IBuildTool`: none is a function-body edit, so Live
  Coding cannot carry them.
- **`Solve/` includes `CoreMinimal.h` and `Solve/` only.** `Check-Architecture.ps1` rule 1
  fails the test run otherwise. `Tool/` may include `Solve/`; `Tool/` may never include
  `Present/`.
- **Comments explain WHY**, and especially why an obvious alternative was rejected. The
  comments in the code blocks below are part of the deliverable, not decoration.
- **Tests assert behaviour with a named reason.** Every assertion string says what is being
  protected, not what is being compared.
- **When a test passes, check it CAN fail.** Task 1 step 9 proves the hysteresis test by
  deleting the rule and watching that test go red. Where a step says "control leg", that
  assertion exists to keep the test from being vacuous - do not delete it as redundant.
- **No `sed` on Windows paths**, and no string replacement without asserting it matched
  exactly once.
- **Commits:** no `Co-Authored-By` trailer.

## File Structure

| File | Responsibility |
|---|---|
| `Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h` (new) | `SnapGuide::ESource`, `FCandidate`, `FTuning`, `FResult`, `Arbitrate`. CoreMinimal only. |
| `Plugins/Airside/Source/Airside/Private/Solve/GuideArbiter.cpp` (new) | The maths: tolerance, ranking, tiebreak, hysteresis, projection. |
| `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h` (new) | `FGuideAnchor`, `IGuideSource`, `FExtendingGuideSource`, `FWorldGuideSource`, `FSnapGuideChain`. |
| `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp` (new) | The two sources, and the chain's gather-then-arbitrate. |
| `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h` | `EPreviewStyle::Guide`; `FToolContext::Guide` and `GuidedCursor()`; `IBuildTool::DescribeGuideAnchor`. |
| `Plugins/Airside/Source/Airside/Private/Tool/PreviewPalette.cpp` | `Guide`'s colour and look. |
| `Source/AirportMgr/RoadBuildHUD.h` / `.cpp` | `Guide` seeded into `Looks`; `IsDashed` dashes it. |
| `Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h` + `Private/Tool/BuildSession.cpp` | Owns the chain and the previous winner; resolves the guide inside `MakeContext`. |
| `Plugins/Airside/Source/Airside/Public/Tool/PlotPlaceTool.h` + `Private/Tool/PlotPlaceTool.cpp` | Declares its anchor; reads `GuidedCursor()` for the back corners; draws the guide. |
| `Plugins/Airside/Source/AirsideTests/Private/GuideArbiterTest.cpp` (new) | The five world-free arbitration tests. |
| `Plugins/Airside/Source/AirsideTests/Private/SnapGuideChainTest.cpp` (new) | The chain and its two sources, over an empty network. |
| `Plugins/Airside/Source/AirsideTests/Private/PlotGuideTest.cpp` (new) | Composition: session -> context -> tool -> preview. |
| `Plugins/Airside/Source/AirsideTests/Private/ToolCursorTest.cpp` | Two legs for `GuidedCursor()`. |
| `Source/AirportMgr/RoadBuildHUDTest.cpp` | `Guide` reads apart from `Provisional`; the style loop's bound. |

## Decisions this plan takes that the spec left open

Each is written into the code as a comment where it binds. Listed here so a reviewer can
reject one without reading five files.

1. **A guide is a LINE, not a ray.** Angular error is the ACUTE angle between the cursor's
   direction and the candidate's, so a candidate and its opposite are one guide. That is why
   World proposes four directions and not eight, and why a back corner dragged to the far
   side of its origin still gets the square.
2. **Hysteresis needs no identity match.** The held winner is a whole `FCandidate`, so its
   error against this frame's cursor is computable from the stored value; the arbiter never
   has to find "the same candidate" in this frame's list. Stage 2 must revisit this - a
   network source's `ReferenceAt` can move under a held winner, where stage 1's cannot
   (Extending's reference is a pinned corner, World's is the origin).
3. **The tie is compared with an epsilon.** Two candidates whose directions came from
   different arithmetic can land a rounding apart, and a source-order tiebreak that only
   fired on bitwise equality would be decided by the last bit.
4. **The anchor is stateless and context-free.** `DescribeGuideAnchor` takes no
   `FToolContext`, because the driver calls it while building one. It reads only what the
   tool has already pinned.
5. **`MakeContext` runs about three times a frame** (the driver's `Tick`, its `BuildReadout`,
   the HUD's own call), each updating the stored winner - so "last frame's winner" is really
   "last call's". `Airside.Solve.GuideArbiterIsStableUnderRepetition` pins that repeating a
   resolution at one cursor cannot drift.
6. **The hard constraint keeps the last word.** The plot's `InFront` clamp runs AFTER the
   guide, so a guided corner dragged behind the frontage still slides onto it. The preview
   therefore draws its guide line from the corner the quad SHOWS, not from `Guide.Point`.
7. **The editor viewport draws `Guide` solid.** `FViewportPreviewSink::Line` has no dash and
   its `Label` deliberately draws nothing - the same limitation `Provisional` already lives
   with. Not fixed here.

## Not in this stage

Named so a reviewer does not report them as gaps: the five network sources (Parallel,
Collinear, Aligned, Runway, Offset), the per-source toggles and the `Snap` action section,
the Alt-held suspension, the distance family, and road drawing as a second consumer.
`ESource` lists all seven values now because the ORDER is the contract - adding Parallel in
stage 2 must not renumber what Runway means.

---

### Task 1: The arbiter, in `Solve/`

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h`
- Create: `Plugins/Airside/Source/Airside/Private/Solve/GuideArbiter.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/GuideArbiterTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `namespace SnapGuide` with `enum class ESource : uint8 { Extending, Aligned,
  Collinear, Parallel, Runway, World, Offset }`; `struct FCandidate { FVector2D Direction;
  double Distance; FVector2D ReferenceAt; FString Description; ESource Source; }`;
  `struct FTuning { double ToleranceDegrees = 7.0; double StickinessDegrees = 2.0; }`;
  `struct FResult { bool bActive; FCandidate Winner; FVector2D Point; }`; and
  `AIRSIDE_API FResult Arbitrate(TConstArrayView<FCandidate> Candidates, const FVector2D&
  Origin, const FVector2D& Cursor, const FResult& Previous, const FTuning& Tuning =
  FTuning())`.

- [ ] **Step 1: Write the header**

Create `Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h`:

```cpp
#pragma once

#include "CoreMinimal.h"

/**
 * What the cursor could be lining up with, and which of those wins.
 *
 * DEPENDENCY-FREE like every Solve/ header - CoreMinimal.h and nothing else. The chain that
 * FILLS these lives in Tool/SnapGuideChain.h, because four of the seven sources must query
 * URoadNetwork; the arbitration itself must not, which is what makes every flicker case in
 * the 2026-09-17 snap-guides design §5 a world-free test.
 */
namespace SnapGuide
{
	/**
	 * The sources, in priority order - most specific to what the player is doing, first.
	 *
	 * THE ORDER IS THE TIEBREAK AND NOTHING ELSE: a lower source still wins outright when it
	 * is the only one in tolerance. What you are extending is what you are thinking about;
	 * the world grid is what you fall back on when nothing else applies.
	 *
	 * ALL SEVEN ARE LISTED although stage 1 fills only Extending and World, because the
	 * ORDER is the contract - adding Parallel in stage 2 must not renumber what Runway means.
	 *
	 * A PLAIN ENUM, not a UENUM: UHT cannot see an enum without a .generated.h, and a Solve/
	 * header may not have one. Stage 3's toggles need reflection and will wrap it there.
	 */
	enum class ESource : uint8
	{
		Extending,
		Aligned,
		Collinear,
		Parallel,
		Runway,
		World,
		Offset
	};

	/** One thing the cursor could line up with. */
	struct FCandidate
	{
		/** Unit, and for a direction guide this is the whole answer. */
		FVector2D Direction = FVector2D(1.0, 0.0);

		/** For Offset: how far along the perpendicular, uu. Zero for direction guides, and
		 *  unread until stage 4 - carried now so the type does not change under stage 2. */
		double Distance = 0.0;

		/**
		 * The point the dashed line is drawn TO - the road it is parallel with, the edge it
		 * is squared to. NOT the guide's own geometry: the player needs to see WHICH thing
		 * they are lining up with, which is the whole of Cities Skylines' advantage here.
		 */
		FVector2D ReferenceAt = FVector2D::ZeroVector;

		/** "square to the frontage", "45 degrees". Shown beside the line. */
		FString Description;

		ESource Source = ESource::World;
	};

	/**
	 * Placement feel, judged in PIE - design §11.
	 *
	 * CONSTANTS FOR NOW, not UAirsideSettings knobs: these are numbers nobody has yet had a
	 * reason to move, and the route ClearanceUu took (constant first, property when tuning
	 * demanded it) is the one this follows. Passed as a struct rather than read from a
	 * global so a test can state the numbers it depends on instead of inheriting them.
	 */
	struct FTuning
	{
		/** How far off a candidate the cursor may be and still be offered it. Degrees. */
		double ToleranceDegrees = 7.0;

		/** How much better a challenger must be before it takes the guide off the incumbent. */
		double StickinessDegrees = 2.0;
	};

	struct FResult
	{
		bool bActive = false;

		FCandidate Winner;

		/** Where the constrained point ended up, which is what the tool uses. Left at zero
		 *  while bActive is false - a caller must branch on the flag, never read past it. */
		FVector2D Point = FVector2D::ZeroVector;
	};


	/**
	 * Which candidate the cursor is lined up with, and where that puts it.
	 *
	 * PURE, and that is the whole reason this lives apart from the chain: no network, no
	 * world, no frame. Previous is the last answer given and is what stops the guide
	 * flickering between two candidates a degree apart; pass a default-constructed FResult
	 * when there was none.
	 *
	 * A GUIDE IS A LINE, NOT A RAY. Error is the ACUTE angle between the cursor's direction
	 * and the candidate's, so a candidate and its opposite are ONE guide. That is why the
	 * World source proposes four directions rather than eight, and why a corner dragged to
	 * the far side of its origin still gets the square rather than losing the guide at the
	 * moment it crosses.
	 *
	 * HYSTERESIS IS MEASURED AGAINST THE PREVIOUS WINNER, not the previous cursor. A player
	 * dragging slowly past two near-equal candidates should feel one guide hold and then
	 * hand over; a cursor-delta rule produces the rapid alternation this exists to stop.
	 */
	AIRSIDE_API FResult Arbitrate(TConstArrayView<FCandidate> Candidates,
		const FVector2D& Origin, const FVector2D& Cursor,
		const FResult& Previous, const FTuning& Tuning = FTuning());
}
```

- [ ] **Step 2: Write the implementation**

Create `Plugins/Airside/Source/Airside/Private/Solve/GuideArbiter.cpp`:

```cpp
#include "Solve/GuideArbiter.h"

namespace
{
	/**
	 * Two candidates computed by different arithmetic paths - a frontage normalised from
	 * node positions and a world axis from a cosine - can land a rounding apart when they
	 * mean the same angle. A source-order tiebreak that only fired on bitwise equality would
	 * therefore be decided by the last bit of a double, which is not a rule anyone can read.
	 */
	constexpr double TieEpsilonDegrees = 1.0e-9;

	/** Acute angle between two unit directions, in degrees. A guide is a LINE: see Arbitrate. */
	double ErrorDegrees(const FVector2D& A, const FVector2D& B)
	{
		const double Aligned = FMath::Abs(FVector2D::DotProduct(A, B));
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Aligned, 0.0, 1.0)));
	}

	/** The cursor's perpendicular projection onto the line through Origin along Direction. */
	FVector2D Project(const FVector2D& Origin, const FVector2D& Direction, const FVector2D& Cursor)
	{
		const FVector2D Unit = Direction.GetSafeNormal();
		return Origin + Unit * FVector2D::DotProduct(Cursor - Origin, Unit);
	}
}

SnapGuide::FResult SnapGuide::Arbitrate(TConstArrayView<FCandidate> Candidates,
	const FVector2D& Origin, const FVector2D& Cursor,
	const FResult& Previous, const FTuning& Tuning)
{
	FResult Result;

	// A CURSOR ON TOP OF THE ORIGIN HAS NO DIRECTION, and this is not a degenerate case to
	// fudge: on the first frame of a drag the two ARE equal, and acos of a zero vector's dot
	// is 90 degrees against every candidate at once - seven guides tied, and whichever
	// sorted first would flash on before the player had moved the mouse.
	const FVector2D Reach = Cursor - Origin;
	const double Distance = Reach.Size();
	if (Distance <= 0.0)
	{
		return Result;
	}
	const FVector2D Heading = Reach / Distance;

	const FCandidate* Best = nullptr;
	double BestError = 0.0;
	for (const FCandidate& Candidate : Candidates)
	{
		// A SOURCE THAT HAD NOTHING TO SAY SHOULD HAVE SAID NOTHING, but a zero direction
		// would otherwise normalise to (1,0) and become a silent extra world axis.
		if (Candidate.Direction.IsNearlyZero())
		{
			continue;
		}

		const double Error = ErrorDegrees(Heading, Candidate.Direction.GetSafeNormal());
		if (Error > Tuning.ToleranceDegrees)
		{
			continue;
		}

		// SMALLEST ERROR FIRST, then SOURCE ORDER. The second half is what stops the answer
		// depending on the order candidates happened to be gathered in - which in stage 2 is
		// the network's iteration order, and therefore changes with an unrelated edit.
		const bool bClearlyBetter = Best == nullptr || Error < BestError - TieEpsilonDegrees;
		const bool bTiedAndHigherPriority = Best != nullptr
			&& Error <= BestError + TieEpsilonDegrees
			&& Candidate.Source < Best->Source;

		if (bClearlyBetter || bTiedAndHigherPriority)
		{
			Best = &Candidate;
			BestError = Error;
		}
	}

	// THE FLICKER RULE. The incumbent is re-measured against THIS cursor from the candidate
	// value itself, so the arbiter never has to recognise "the same candidate" in this
	// frame's list - an identity test that stage 2's network sources would make unreliable.
	// It still has to be IN TOLERANCE: an incumbent the cursor has walked away from is no
	// longer a guide, however sticky.
	if (Previous.bActive && !Previous.Winner.Direction.IsNearlyZero())
	{
		const double HeldError = ErrorDegrees(Heading, Previous.Winner.Direction.GetSafeNormal());
		const bool bStillEligible = HeldError <= Tuning.ToleranceDegrees;
		const bool bChallengerWins = Best != nullptr
			&& BestError < HeldError - Tuning.StickinessDegrees;

		if (bStillEligible && !bChallengerWins)
		{
			Result.bActive = true;
			Result.Winner = Previous.Winner;
			Result.Point = Project(Origin, Result.Winner.Direction, Cursor);
			return Result;
		}
	}

	if (Best == nullptr)
	{
		// NOTHING IN TOLERANCE MEANS NO GUIDE, not a nearest-anyway answer. A guide that is
		// always on is a constraint, and the player never asked for one.
		return Result;
	}

	Result.bActive = true;
	Result.Winner = *Best;
	Result.Point = Project(Origin, Best->Direction, Cursor);
	return Result;
}
```

- [ ] **Step 3: Write the failing tests**

Create `Plugins/Airside/Source/AirsideTests/Private/GuideArbiterTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/GuideArbiter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A candidate pointing Degrees off +X, named after its own angle so a failure says
	 *  which one won rather than which index did. */
	SnapGuide::FCandidate GuideAt(double Degrees, SnapGuide::ESource Source)
	{
		SnapGuide::FCandidate Candidate;
		const double Radians = FMath::DegreesToRadians(Degrees);
		Candidate.Direction = FVector2D(FMath::Cos(Radians), FMath::Sin(Radians));
		Candidate.Description = FString::Printf(TEXT("%.1f degrees"), Degrees);
		Candidate.Source = Source;
		return Candidate;
	}

	/** A cursor 1000 uu from the origin, Degrees off +X. */
	FVector2D CursorAt(double Degrees, double Reach = 1000.0)
	{
		const double Radians = FMath::DegreesToRadians(Degrees);
		return FVector2D(FMath::Cos(Radians), FMath::Sin(Radians)) * Reach;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideArbiterPicksTheNearestTest,
	"Airside.Solve.GuideArbiterPicksTheNearest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideArbiterPicksTheNearestTest::RunTest(const FString& Parameters)
{
	const TArray<SnapGuide::FCandidate> Candidates = {
		GuideAt(0.0, SnapGuide::ESource::World),
		GuideAt(45.0, SnapGuide::ESource::World) };

	const SnapGuide::FResult Result = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(4.0), SnapGuide::FResult());

	if (!TestTrue(TEXT("a cursor 4 degrees off an axis is offered a guide"), Result.bActive))
	{
		return false;
	}

	// BY DESCRIPTION, not by index: this asserts the WINNER travelled out whole, which is
	// what the overlay's label is drawn from.
	TestEqual(TEXT("the nearer candidate wins on smallest angular error"),
		Result.Winner.Description, FString(TEXT("0.0 degrees")));

	// THE POINT IS THE PERPENDICULAR PROJECTION onto the winner's line through the origin -
	// the tool uses this directly as its corner, so an answer that merely pointed the right
	// way would put the corner somewhere the player did not drag it.
	TestTrue(TEXT("the constrained point lands exactly on the winning line"),
		FMath::IsNearlyZero(Result.Point.Y, 1.0e-6));
	TestTrue(TEXT("and at the cursor's own reach along it"),
		FMath::IsNearlyEqual(Result.Point.X, 1000.0 * FMath::Cos(FMath::DegreesToRadians(4.0)), 1.0e-6));

	// A GUIDE IS A LINE, NOT A RAY. The same axis must still claim a cursor dragged 180
	// degrees the other way, or a corner would lose its square the instant it crossed the
	// origin - which is the case the World source proposing four directions depends on.
	const SnapGuide::FResult Behind = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(184.0), SnapGuide::FResult());
	TestTrue(TEXT("a candidate and its opposite are one guide"), Behind.bActive);
	TestTrue(TEXT("and the point projects onto the far side"), Behind.Point.X < 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideArbiterBreaksTiesBySourceTest,
	"Airside.Solve.GuideArbiterBreaksTiesBySource",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideArbiterBreaksTiesBySourceTest::RunTest(const FString& Parameters)
{
	// THE SAME ANGLE FROM TWO SOURCES: a frontage that happens to lie on a world axis. This
	// is not contrived - a plot drawn off an east-west service road produces it every time.
	const SnapGuide::FCandidate Extending = GuideAt(45.0, SnapGuide::ESource::Extending);
	const SnapGuide::FCandidate World = GuideAt(45.0, SnapGuide::ESource::World);

	const TArray<SnapGuide::FCandidate> ExtendingFirst = { Extending, World };
	const TArray<SnapGuide::FCandidate> WorldFirst = { World, Extending };

	const SnapGuide::FResult A = SnapGuide::Arbitrate(
		ExtendingFirst, FVector2D::ZeroVector, CursorAt(48.0), SnapGuide::FResult());
	const SnapGuide::FResult B = SnapGuide::Arbitrate(
		WorldFirst, FVector2D::ZeroVector, CursorAt(48.0), SnapGuide::FResult());

	// IN BOTH ORDERS. A tiebreak that depended on the order candidates happened to be
	// gathered in would be a guide that changed with the network's iteration order - i.e.
	// with an edit nobody connected to guides at all.
	TestEqual(TEXT("a tie goes to the higher-priority source"),
		static_cast<int32>(A.Winner.Source), static_cast<int32>(SnapGuide::ESource::Extending));
	TestEqual(TEXT("and does so whichever order the candidates arrived in"),
		static_cast<int32>(B.Winner.Source), static_cast<int32>(SnapGuide::ESource::Extending));

	// CONTROL LEG: source order is the TIEBREAK and nothing else. A World candidate that is
	// genuinely nearer must still win, or the test above would pass on an arbiter that
	// simply always preferred Extending.
	const TArray<SnapGuide::FCandidate> NearerWorld = {
		GuideAt(45.0, SnapGuide::ESource::Extending),
		GuideAt(47.0, SnapGuide::ESource::World) };
	const SnapGuide::FResult C = SnapGuide::Arbitrate(
		NearerWorld, FVector2D::ZeroVector, CursorAt(48.0), SnapGuide::FResult());
	TestEqual(TEXT("but a nearer low-priority source still wins outright"),
		static_cast<int32>(C.Winner.Source), static_cast<int32>(SnapGuide::ESource::World));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideArbiterHoldsItsWinnerTest,
	"Airside.Solve.GuideArbiterHoldsItsWinner",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideArbiterHoldsItsWinnerTest::RunTest(const FString& Parameters)
{
	// THE FLICKER RULE, and the reason the arbiter is a separate unit at all. Two candidates
	// four degrees apart: the incumbent on the axis, the challenger just off it.
	const TArray<SnapGuide::FCandidate> Candidates = {
		GuideAt(0.0, SnapGuide::ESource::Extending),
		GuideAt(4.0, SnapGuide::ESource::World) };

	SnapGuide::FResult Previous;
	Previous.bActive = true;
	Previous.Winner = GuideAt(0.0, SnapGuide::ESource::Extending);

	const SnapGuide::FTuning Tuning;   // 7 degrees tolerance, 2 degrees stickiness

	// Cursor at 3 degrees: the incumbent is 3 off, the challenger 1 off. Better by exactly 2,
	// which is NOT better by MORE than the stickiness - so the guide holds.
	const SnapGuide::FResult Held = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(3.0), Previous, Tuning);
	TestTrue(TEXT("a guide the cursor is still near stays active"), Held.bActive);
	TestEqual(TEXT("a challenger better by only the stickiness does not take the guide"),
		static_cast<int32>(Held.Winner.Source), static_cast<int32>(SnapGuide::ESource::Extending));

	// Cursor at 3.9 degrees: incumbent 3.9 off, challenger 0.1 off - better by 3.8, and the
	// handover happens. One guide holding then handing over is the feel being protected;
	// never handing over would be as bad as flickering.
	const SnapGuide::FResult Taken = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(3.9), Previous, Tuning);
	TestEqual(TEXT("a challenger better by more than the stickiness does take it"),
		static_cast<int32>(Taken.Winner.Source), static_cast<int32>(SnapGuide::ESource::World));

	// AND AN INCUMBENT OUT OF TOLERANCE IS DROPPED however sticky it is: at 8.5 degrees the
	// held axis is past the 7-degree tolerance, so stickiness must not resurrect it.
	const SnapGuide::FResult Dropped = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(8.5), Previous, Tuning);
	TestTrue(TEXT("an incumbent out of tolerance is still dropped"), Dropped.bActive);
	TestEqual(TEXT("and the eligible challenger takes over"),
		static_cast<int32>(Dropped.Winner.Source), static_cast<int32>(SnapGuide::ESource::World));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideArbiterRefusesOutsideToleranceTest,
	"Airside.Solve.GuideArbiterRefusesOutsideTolerance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideArbiterRefusesOutsideToleranceTest::RunTest(const FString& Parameters)
{
	const TArray<SnapGuide::FCandidate> Candidates = { GuideAt(0.0, SnapGuide::ESource::World) };

	const SnapGuide::FResult Far = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(20.0), SnapGuide::FResult());
	TestFalse(TEXT("nothing within tolerance means no guide, not a nearest-anyway answer"),
		Far.bActive);

	// CONTROL LEG: the same candidate and the same call DOES fire in tolerance, so the
	// assertion above is measuring the tolerance and not a broken call.
	const SnapGuide::FResult Near = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, CursorAt(5.0), SnapGuide::FResult());
	TestTrue(TEXT("and the same candidate five degrees off does fire"), Near.bActive);

	// A CURSOR ON THE ORIGIN HAS NO DIRECTION. The first frame of every drag is exactly this,
	// and an arbiter that answered here would flash a guide on before the player moved.
	const SnapGuide::FResult Degenerate = SnapGuide::Arbitrate(
		Candidates, FVector2D(500.0, 500.0), FVector2D(500.0, 500.0), SnapGuide::FResult());
	TestFalse(TEXT("a cursor on top of the origin is offered nothing"), Degenerate.bActive);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideArbiterIsStableUnderRepetitionTest,
	"Airside.Solve.GuideArbiterIsStableUnderRepetition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideArbiterIsStableUnderRepetitionTest::RunTest(const FString& Parameters)
{
	// FBuildSession::MakeContext runs about three times per frame - the driver's Tick, its
	// BuildReadout, and the HUD's own call - and each one feeds the last answer back in as
	// Previous. So "last frame's winner" is really "last call's", and a rule that drifted on
	// repetition would drift WITHIN one frame, which no amount of PIE would make legible.
	const TArray<SnapGuide::FCandidate> Candidates = {
		GuideAt(0.0, SnapGuide::ESource::Extending),
		GuideAt(4.0, SnapGuide::ESource::World) };

	const FVector2D Cursor = CursorAt(3.0);

	SnapGuide::FResult Carried = SnapGuide::Arbitrate(
		Candidates, FVector2D::ZeroVector, Cursor, SnapGuide::FResult());
	if (!TestTrue(TEXT("the first resolution answers"), Carried.bActive)) { return false; }

	const int32 FirstSource = static_cast<int32>(Carried.Winner.Source);
	const FVector2D FirstPoint = Carried.Point;

	for (int32 Repeat = 0; Repeat < 5; ++Repeat)
	{
		Carried = SnapGuide::Arbitrate(Candidates, FVector2D::ZeroVector, Cursor, Carried);
		TestEqual(FString::Printf(
			TEXT("resolving again at one cursor keeps the same winner (repeat %d)"), Repeat),
			static_cast<int32>(Carried.Winner.Source), FirstSource);
		TestTrue(FString::Printf(
			TEXT("and the same constrained point (repeat %d)"), Repeat),
			Carried.Point.Equals(FirstPoint, 1.0e-9));
	}

	return true;
}

#endif
```

- [ ] **Step 4: Build**

Run the build line from Global Constraints. Expect `Result: Succeeded`.

- [ ] **Step 5: Build again if the new tests do not appear**

Run the test line filtered to the new tests:

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides\AirportMgr.uproject" -Filter Airside.Solve.GuideArbiter
```

Expected: `5 test(s) run`. If it says 0, this is the known two-builds-for-a-new-.cpp case -
build again and rerun before debugging anything.

- [ ] **Step 6: Read the result line**

Expected: `5 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 7: Run the whole suite**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides\AirportMgr.uproject"
```

Expected: 0 failed, 0 crashed, and the total up by 5 on the baseline recorded before this
task. Put the before and after numbers in the commit message.

- [ ] **Step 8: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h Plugins/Airside/Source/Airside/Private/Solve/GuideArbiter.cpp Plugins/Airside/Source/AirsideTests/Private/GuideArbiterTest.cpp
git commit -m "feat(solve): an arbiter picks one alignment and holds it"
```

- [ ] **Step 9: Prove the hysteresis test can fail, then restore**

This is the step the spec asked for by name. A passing test that cannot fail is worse than
no test.

1. In `GuideArbiter.cpp`, comment out the whole `if (Previous.bActive && ...)` block.
2. Build.
3. Run `-Filter Airside.Solve.GuideArbiterHoldsItsWinner`.
4. Expected: `1 test(s) run, 1 failed` - failing on "a challenger better by only the
   stickiness does not take the guide". If it PASSES, the test is vacuous: fix the test, not
   the arbiter, and repeat from 1.
5. Restore the block (`git checkout -- Plugins/Airside/Source/Airside/Private/Solve/GuideArbiter.cpp`).
6. Build, rerun the filter, expect `1 test(s) run, 0 failed`.
7. Nothing to commit - the working tree is back where step 8 left it. Confirm with
   `git status --short`, which must be empty.

---

### Task 2: The chain and its two sources, in `Tool/`

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h`
- Create: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/SnapGuideChainTest.cpp`

**Interfaces:**
- Consumes: `SnapGuide::FCandidate`, `SnapGuide::FResult`, `SnapGuide::FTuning`,
  `SnapGuide::Arbitrate` from Task 1.
- Produces: `struct FGuideAnchor { FVector2D Origin; FVector2D Reference; FVector2D
  ReferenceAt; FString ReferenceName; }`; `struct IGuideSource` with `virtual void
  Propose(const URoadNetwork&, const FGuideAnchor&, TArray<SnapGuide::FCandidate>&) const`;
  `FExtendingGuideSource`; `FWorldGuideSource`; and `class FSnapGuideChain` with
  `FSnapGuideChain()`, `void AddSource(TUniquePtr<IGuideSource>)`, `int32 NumSources() const`
  and `SnapGuide::FResult Resolve(const URoadNetwork& Network, const FGuideAnchor& Anchor,
  const FVector2D& Cursor, const SnapGuide::FResult& Previous, const SnapGuide::FTuning&
  Tuning = SnapGuide::FTuning()) const`.

- [ ] **Step 1: Write the header**

Create `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Solve/GuideArbiter.h"

class URoadNetwork;

/**
 * What the tool is dragging, and what it is dragging it against.
 *
 * THE TOOL SUPPLIES IT, THE DRIVER RESOLVES FROM IT - see IBuildTool::DescribeGuideAnchor.
 * A source cannot ask "what is the player doing": only the tool knows which corner moves and
 * which edge it grew from, and a source that guessed would be a second opinion about the
 * gesture.
 */
struct FGuideAnchor
{
	/** The fixed point the moving point swings around. Every candidate is a line through it. */
	FVector2D Origin = FVector2D::ZeroVector;

	/**
	 * The direction the gesture is already extending - the frontage for a plot corner, the
	 * incoming segment for a road. ZERO when the tool has none, and the Extending source
	 * then proposes nothing rather than inventing an axis.
	 */
	FVector2D Reference = FVector2D::ZeroVector;

	/**
	 * The point Extending's dashed line is drawn TO: the far end of the reference edge, so
	 * the line SHOWS which edge is being squared to. Design §6 - a ray into the distance
	 * says "45 degrees", and a line to the thing says what you are lining up with, which is
	 * the whole of what the request asked for.
	 */
	FVector2D ReferenceAt = FVector2D::ZeroVector;

	/**
	 * "the frontage". Composed by the source into "square to the frontage", so the same
	 * source extending a road in stage 5 does not have to read as a plot. The tool names its
	 * own reference because the tool is the only thing that knows what it is.
	 */
	FString ReferenceName;
};

/**
 * One link of the guide chain - design §3.
 *
 * UNLIKE IRoadSnapRule, NOTHING CLAIMS. A snap rule answers "what did the cursor hit" and the
 * first hit ends the search; a guide source answers "what could this line up with", and the
 * whole point is that several answer at once and SnapGuide::Arbitrate chooses between them.
 * So this proposes into a shared array and never returns a verdict.
 */
struct AIRSIDE_API IGuideSource
{
	virtual ~IGuideSource() = default;

	/** Appends this source's candidates. NEVER clears Out - the chain owns that array. */
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const = 0;
};

/**
 * Source 1: what the tool is already extending, and its perpendicular.
 *
 * NEEDS NO NETWORK - the tool supplied the reference. It still takes one, like every source,
 * because the chain calls them through one interface and a signature that varied per source
 * would put the branch in the chain instead.
 */
struct AIRSIDE_API FExtendingGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const override;
};

/**
 * Source 6: 0, 45, 90 and 135 degrees.
 *
 * FOUR, NOT EIGHT. 180 degrees away is the same LINE and SnapGuide::Arbitrate measures the
 * acute angle, so eight would put two identical candidates into every tie the source-order
 * rule then has to break for no reason.
 */
struct AIRSIDE_API FWorldGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const override;
};

/**
 * Gathers every source's candidates and arbitrates between them - design §3 and §5.
 *
 * MODELLED ON FRoadSnapChain, deliberately, down to the move-only ownership: a source added
 * in stage 2 is a new link, not an edit to a widening conditional. It differs in the one way
 * that matters - see IGuideSource on why nothing claims.
 *
 * THE ORDER SOURCES ARE ADDED IN DOES NOT DECIDE TIES. SnapGuide::ESource does, inside the
 * arbiter. This chain's order is only the order they are asked, which is unobservable.
 */
class AIRSIDE_API FSnapGuideChain
{
public:
	/** Extending then World: stage 1's two, in design §3's order. */
	FSnapGuideChain();

	// Move-only for the same reason FRoadSnapChain is: the chain OWNS its sources through
	// TUniquePtr, so there is no copy to make, and saying so beats being told by the compiler.
	FSnapGuideChain(const FSnapGuideChain&) = delete;
	FSnapGuideChain& operator=(const FSnapGuideChain&) = delete;
	FSnapGuideChain(FSnapGuideChain&&) = default;
	FSnapGuideChain& operator=(FSnapGuideChain&&) = default;

	void AddSource(TUniquePtr<IGuideSource> Source);

	int32 NumSources() const { return Sources.Num(); }

	/**
	 * Every source's candidates, arbitrated, with Previous carrying the flicker rule.
	 *
	 * Previous is the caller's business to store: FBuildSession holds it, because
	 * IBuildTool::BuildPreview and BuildReadout are both const and neither could.
	 */
	SnapGuide::FResult Resolve(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, const SnapGuide::FResult& Previous,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const;

private:
	TArray<TUniquePtr<IGuideSource>> Sources;
};
```

- [ ] **Step 2: Write the implementation**

Create `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp`:

```cpp
#include "Tool/SnapGuideChain.h"

#include "Solve/RoadGeom.h"

void FExtendingGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	TArray<SnapGuide::FCandidate>& Out) const
{
	if (Anchor.Reference.IsNearlyZero())
	{
		return;
	}

	const FVector2D Along = Anchor.Reference.GetSafeNormal();

	SnapGuide::FCandidate Parallel;
	Parallel.Direction = Along;
	Parallel.ReferenceAt = Anchor.ReferenceAt;
	Parallel.Description = FString::Printf(TEXT("along %s"), *Anchor.ReferenceName);
	Parallel.Source = SnapGuide::ESource::Extending;
	Out.Add(Parallel);

	// THE PERPENDICULAR IS THE ONE THAT SQUARES A PLOT, and it is proposed from the same
	// reference rather than by a second source, because it is the same fact about the same
	// edge - see design §3's "the incoming segment's direction, and its perpendicular".
	// RoadGeom::PerpCCW rather than a hand-written (-y, x): the sign convention is stated
	// once in the codebase and this is not the place to restate it.
	SnapGuide::FCandidate Square = Parallel;
	Square.Direction = RoadGeom::PerpCCW(Along);
	Square.Description = FString::Printf(TEXT("square to %s"), *Anchor.ReferenceName);
	Out.Add(Square);
}

void FWorldGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	TArray<SnapGuide::FCandidate>& Out) const
{
	for (const int32 Degrees : { 0, 45, 90, 135 })
	{
		SnapGuide::FCandidate Candidate;
		const double Radians = FMath::DegreesToRadians(static_cast<double>(Degrees));
		Candidate.Direction = FVector2D(FMath::Cos(Radians), FMath::Sin(Radians));

		// THE LINE GOES BACK TO THE POINT IT SWINGS AROUND. The world grid is not a thing on
		// the map to point at, and a dashed line shot off to nowhere would say less than one
		// that says "this is the corner you are square from". The label carries the rest.
		Candidate.ReferenceAt = Anchor.Origin;
		Candidate.Description = FString::Printf(TEXT("%d degrees"), Degrees);
		Candidate.Source = SnapGuide::ESource::World;
		Out.Add(Candidate);
	}
}

FSnapGuideChain::FSnapGuideChain()
{
	AddSource(MakeUnique<FExtendingGuideSource>());
	AddSource(MakeUnique<FWorldGuideSource>());
}

void FSnapGuideChain::AddSource(TUniquePtr<IGuideSource> Source)
{
	if (Source.IsValid())
	{
		Sources.Add(MoveTemp(Source));
	}
}

SnapGuide::FResult FSnapGuideChain::Resolve(const URoadNetwork& Network,
	const FGuideAnchor& Anchor, const FVector2D& Cursor,
	const SnapGuide::FResult& Previous, const SnapGuide::FTuning& Tuning) const
{
	TArray<SnapGuide::FCandidate> Candidates;

	// Stage 1 gathers at most six. Reserved anyway because stage 2's Parallel and Collinear
	// propose per segment, and the array is rebuilt on every context - about three times a
	// frame, per FBuildSession::MakeContext.
	Candidates.Reserve(16);

	for (const TUniquePtr<IGuideSource>& Source : Sources)
	{
		Source->Propose(Network, Anchor, Candidates);
	}

	return SnapGuide::Arbitrate(Candidates, Anchor.Origin, Cursor, Previous, Tuning);
}
```

- [ ] **Step 3: Write the failing tests**

Create `Plugins/Airside/Source/AirsideTests/Private/SnapGuideChainTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Tool/SnapGuideChain.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * An EMPTY network, because stage 1's two sources read nothing from one - Extending is
	 * handed its reference by the tool and World is absolute. A network with roads in it
	 * would suggest these sources consult it, which is exactly what stage 2 changes.
	 */
	URoadNetwork* EmptyNetwork()
	{
		return NewObject<URoadNetwork>(GetTransientPackage());
	}

	/** A plot's back corner: swinging around the far end of an east-west frontage. */
	FGuideAnchor Frontage()
	{
		FGuideAnchor Anchor;
		Anchor.Origin = FVector2D(3000.0, 0.0);
		Anchor.Reference = FVector2D(1.0, 0.0);
		Anchor.ReferenceAt = FVector2D::ZeroVector;
		Anchor.ReferenceName = TEXT("the frontage");
		return Anchor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainProposesTheFrontageAndItsPerpendicularTest,
	"Airside.Tool.GuideChainProposesTheFrontageAndItsPerpendicular",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainProposesTheFrontageAndItsPerpendicularTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = EmptyNetwork();
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	const FSnapGuideChain Chain;
	TestEqual(TEXT("stage 1 installs Extending and World"), Chain.NumSources(), 2);

	const FGuideAnchor Anchor = Frontage();

	// A CORNER DRAGGED ALMOST SQUARE: 2000 uu out and 50 uu along, which is about 1.4
	// degrees off the perpendicular - inside the 7-degree tolerance.
	const FVector2D Cursor = Anchor.Origin + FVector2D(50.0, 2000.0);

	const SnapGuide::FResult Result = Chain.Resolve(
		*Network, Anchor, Cursor, SnapGuide::FResult());

	if (!TestTrue(TEXT("a corner dragged near square is offered a guide"), Result.bActive))
	{
		return false;
	}

	TestEqual(TEXT("the perpendicular of the tool's own reference is what wins"),
		static_cast<int32>(Result.Winner.Source), static_cast<int32>(SnapGuide::ESource::Extending));
	TestEqual(TEXT("and it is described by the name the TOOL gave its reference"),
		Result.Winner.Description, FString(TEXT("square to the frontage")));

	// THE DASHED LINE POINTS AT THE EDGE, not along the guide - design §6. This is the field
	// the overlay draws to, so an anchor whose ReferenceAt did not travel would draw a line
	// to the world origin and look like a bug in the gesture.
	TestTrue(TEXT("the reference point the tool supplied travels to the winner"),
		Result.Winner.ReferenceAt.Equals(Anchor.ReferenceAt, 1.0e-6));

	// EXACTLY SQUARE, not nearly: the constrained point is what the corner becomes.
	TestTrue(TEXT("the constrained point is exactly square to the frontage"),
		FMath::IsNearlyEqual(Result.Point.X, Anchor.Origin.X, 1.0e-6));

	// AND THE PARALLEL IS PROPOSED TOO. A corner dragged back along the frontage gets the
	// other of Extending's two candidates - without this leg the source could be proposing
	// one direction and the test above would not notice.
	const SnapGuide::FResult AlongIt = Chain.Resolve(
		*Network, Anchor, Anchor.Origin + FVector2D(2000.0, 30.0), SnapGuide::FResult());
	TestEqual(TEXT("dragging along the frontage gets the frontage's own direction"),
		AlongIt.Winner.Description, FString(TEXT("along the frontage")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainPrefersTheFrontageOverTheWorldGridTest,
	"Airside.Tool.GuideChainPrefersTheFrontageOverTheWorldGrid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainPrefersTheFrontageOverTheWorldGridTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = EmptyNetwork();
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	const FSnapGuideChain Chain;

	// A FRONTAGE ON A WORLD AXIS - every plot off an east-west service road. Extending
	// proposes 0 and 90; World proposes 0, 45, 90, 135. The 90s tie exactly.
	FGuideAnchor Anchor = Frontage();
	const FVector2D Cursor = Anchor.Origin + FVector2D(60.0, 2000.0);

	const SnapGuide::FResult Result = Chain.Resolve(
		*Network, Anchor, Cursor, SnapGuide::FResult());
	TestEqual(TEXT("a tie between the frontage and a world axis goes to the frontage"),
		static_cast<int32>(Result.Winner.Source), static_cast<int32>(SnapGuide::ESource::Extending));

	// CONTROL LEG: World was a live competitor, not an absent one. With no reference the
	// Extending source proposes nothing and the same cursor gets the world axis instead - so
	// the assertion above is measuring the tiebreak rather than an empty list.
	Anchor.Reference = FVector2D::ZeroVector;
	const SnapGuide::FResult WorldOnly = Chain.Resolve(
		*Network, Anchor, Cursor, SnapGuide::FResult());
	TestTrue(TEXT("with no reference the world grid still answers"), WorldOnly.bActive);
	TestEqual(TEXT("and it is the world axis that does"),
		static_cast<int32>(WorldOnly.Winner.Source), static_cast<int32>(SnapGuide::ESource::World));
	TestEqual(TEXT("named as an angle, since the grid has no thing to point at"),
		WorldOnly.Winner.Description, FString(TEXT("90 degrees")));
	TestTrue(TEXT("and its line points back at the corner it swings around"),
		WorldOnly.Winner.ReferenceAt.Equals(Anchor.Origin, 1.0e-6));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainOffersNothingBetweenCandidatesTest,
	"Airside.Tool.GuideChainOffersNothingBetweenCandidates",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainOffersNothingBetweenCandidatesTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Network = EmptyNetwork();
	if (!TestNotNull(TEXT("a network"), Network)) { return false; }

	const FSnapGuideChain Chain;

	FGuideAnchor Anchor = Frontage();
	Anchor.Reference = FVector2D::ZeroVector;   // world axes only, 45 degrees apart

	// 22 degrees off +X: 22 from one axis, 23 from the next, both past the 7-degree
	// tolerance. THE GUIDE MUST BE OFF MOST OF THE TIME, or it is a constraint the player
	// never asked for rather than an aid.
	const double Radians = FMath::DegreesToRadians(22.0);
	const FVector2D Cursor = Anchor.Origin
		+ FVector2D(FMath::Cos(Radians), FMath::Sin(Radians)) * 2000.0;

	const SnapGuide::FResult Result = Chain.Resolve(
		*Network, Anchor, Cursor, SnapGuide::FResult());
	TestFalse(TEXT("a cursor between two world axes is offered neither"), Result.bActive);

	return true;
}

#endif
```

- [ ] **Step 4: Build**

Expect `Result: Succeeded`. New `.cpp` files: if the tests do not appear in step 5, build a
second time.

- [ ] **Step 5: Run the new tests**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides\AirportMgr.uproject" -Filter Airside.Tool.GuideChain
```

Expected: `3 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 6: Run the whole suite**

Expected: 0 failed, 0 crashed. `Check-Architecture.ps1` passes - in particular Solve/ purity,
since `SnapGuideChain.cpp` includes `Solve/RoadGeom.h` from `Tool/`, which is the legal
direction.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp Plugins/Airside/Source/AirsideTests/Private/SnapGuideChainTest.cpp
git commit -m "feat(tool): a chain of sources proposes what the cursor could line up with"
```

---

### Task 3: `EPreviewStyle::Guide`, and the five lists that must agree

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h` (the enum, at its end)
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PreviewPalette.cpp` (both switches)
- Modify: `Source/AirportMgr/RoadBuildHUD.h` (`IsDashed`)
- Modify: `Source/AirportMgr/RoadBuildHUD.cpp` (the constructor's seeding list)
- Test: `Source/AirportMgr/RoadBuildHUDTest.cpp` (the loop's bound, plus a new test)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `EPreviewStyle::Guide`, drawn dashed and violet by `ARoadBuildHUD`.

**Why this is its own task:** a new `EPreviewStyle` value has FIVE consumers that must agree,
and only two of them fail loudly. The enum, `PreviewPalette::Default` and
`PreviewPalette::DefaultLook` are caught by `checkNoEntry()` at the first frame that draws
the style; `ARoadBuildHUD`'s constructor list and `FRoadBuildHUDLooksTest`'s loop BOUND are
caught by nothing at all. This is CLAUDE.md's "check where a list is CONSUMED, not where it
is declared", and it has shipped three times in this codebase.

- [ ] **Step 1: Add the enum value**

In `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h`, immediately after the
`Provisional,` entry and before the closing `};` of `EPreviewStyle`:

```cpp

	/**
	 * What the cursor is lined up with: the dashed line to the thing it is squared to.
	 *
	 * NOT Provisional, which already means "this edge is still moving" - and the two are
	 * drawn in the same frame, touching the same corner. Two meanings, two styles; whether
	 * the overlay dashes both is its business, not the plugin's (design §6).
	 *
	 * AT THE END, like Pinned and Provisional above and for the same reason: this is a UENUM
	 * and renumbering it repoints any value already serialised against it.
	 */
	Guide,
```

- [ ] **Step 2: Add both palette cases**

In `Plugins/Airside/Source/Airside/Private/Tool/PreviewPalette.cpp`, in `Default`, after the
`case EPreviewStyle::Selected:` line:

```cpp

	// VIOLET, and deliberately nothing else in this table. A guide line is drawn touching a
	// Provisional edge every frame it exists, so white was out; Snap's amber and Route's cyan
	// both already mean something a gesture would DO, and this means something it is measured
	// against.
	case EPreviewStyle::Guide:                       return FLinearColor(0.75f, 0.5f, 1.0f);
```

And in `DefaultLook`, after the `case EPreviewStyle::Provisional:` block:

```cpp

	// THINNER THAN THE BOUNDARY IT HELPS DRAW. Pinned and Provisional are 2.0 because a plot
	// edge must read over grass; the guide is an aid to that edge, and drawn at the same
	// weight it competes with the shape the player is actually making.
	case EPreviewStyle::Guide:
		Look.ThicknessScale = 1.0f;
		break;
```

- [ ] **Step 3: Dash it, and seed it**

In `Source/AirportMgr/RoadBuildHUD.h`, replace the single line

```cpp
	static bool IsDashed(EPreviewStyle Style) { return Style == EPreviewStyle::Provisional; }
```

with

```cpp
	// TWO STYLES DASH, for two different reasons: Provisional because the edge has not
	// stopped moving, Guide because it is not an edge at all. They are told apart by colour
	// - see PreviewPalette::Default - not by the dash they share.
	static bool IsDashed(EPreviewStyle Style)
	{
		return Style == EPreviewStyle::Provisional || Style == EPreviewStyle::Guide;
	}
```

In `Source/AirportMgr/RoadBuildHUD.cpp`, in the constructor's brace-initialiser list, change
the last entry `EPreviewStyle::Pinned, EPreviewStyle::Provisional })` to
`EPreviewStyle::Pinned, EPreviewStyle::Provisional, EPreviewStyle::Guide })`.

Before editing, confirm the text `EPreviewStyle::Pinned, EPreviewStyle::Provisional })`
appears EXACTLY ONCE in that file (`grep -c`). If it appears zero or twice, stop and read the
file - a replacement that silently hit the wrong site is this project's recorded way of
losing an afternoon.

- [ ] **Step 4: Widen the test's loop, and say why it had a bound**

In `Source/AirportMgr/RoadBuildHUDTest.cpp`, in `FRoadBuildHUDLooksTest`, replace

```cpp
	// EPreviewStyle is a plain 0-based enum ending at Provisional - iterated the same way
	// FBuildActionsRegistryTest walks EActionSection, rather than by reflection.
	for (uint8 S = 0; S <= static_cast<uint8>(EPreviewStyle::Provisional); ++S)
```

with

```cpp
	// EPreviewStyle is a plain 0-based enum ending at Guide - iterated the same way
	// FBuildActionsRegistryTest walks EActionSection, rather than by reflection.
	//
	// THIS BOUND IS THE FIFTH LIST a new style has to appear in, and the only one nothing
	// would have caught: a value added after the old bound was simply not tested, and the
	// constructor's seeding list could have missed it in silence. Adding a style means
	// moving this line.
	for (uint8 S = 0; S <= static_cast<uint8>(EPreviewStyle::Guide); ++S)
```

- [ ] **Step 5: Write the failing test**

Append to `Source/AirportMgr/RoadBuildHUDTest.cpp`, immediately before the final `#endif`:

```cpp
/**
 * A GUIDE MUST NOT READ AS A PLOT EDGE. It is drawn from the very corner a Provisional edge
 * ends at, in the same frame, so if the two shared a look the player would see a five-sided
 * plot rather than a four-sided one with an aid attached.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideReadsApartFromProvisionalTest,
	"AirportMgr.HUD.GuideReadsApartFromProvisional",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideReadsApartFromProvisionalTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadBuildHUD* Hud = World->SpawnActor<ARoadBuildHUD>();
	if (!TestNotNull(TEXT("the hud"), Hud)) { return false; }

	const FPreviewLook& Guide = Hud->LookForTest(EPreviewStyle::Guide);
	const FPreviewLook& Provisional = Hud->LookForTest(EPreviewStyle::Provisional);

	// SEEDED AT ALL. LookFor falls back rather than crashing, so a style left out of the
	// constructor's list would otherwise pass every assertion below by accident.
	TestTrue(TEXT("the guide style is seeded with a positive thickness"),
		Guide.ThicknessScale > 0.0f);

	TestFalse(TEXT("a guide is not drawn in the plot boundary's colour"),
		Guide.Colour.Equals(Provisional.Colour));
	TestTrue(TEXT("and it is lighter than the edge it helps draw"),
		Guide.ThicknessScale < Provisional.ThicknessScale);

	// DASHED, BOTH - and that is deliberate: the dash says "not settled", which is true of
	// both. The colour is what separates them.
	TestTrue(TEXT("a guide line is dashed"), ARoadBuildHUD::IsDashed(EPreviewStyle::Guide));
	TestTrue(TEXT("as is a provisional edge"),
		ARoadBuildHUD::IsDashed(EPreviewStyle::Provisional));
	TestFalse(TEXT("while a pinned edge is solid"),
		ARoadBuildHUD::IsDashed(EPreviewStyle::Pinned));

	return true;
}

```

- [ ] **Step 6: Build**

Expect `Result: Succeeded`. A `UENUM` gained a value, so Live Coding cannot carry this.

- [ ] **Step 7: Run the HUD tests**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides\AirportMgr.uproject" -Filter AirportMgr.HUD
```

Expected: every `AirportMgr.HUD.*` test passes, including
`AirportMgr.HUD.LooksCoverEveryStyle` now covering one more style, and the new
`AirportMgr.HUD.GuideReadsApartFromProvisional`.

- [ ] **Step 8: Run the whole suite**

Expected: 0 failed, 0 crashed.

- [ ] **Step 9: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h Plugins/Airside/Source/Airside/Private/Tool/PreviewPalette.cpp Source/AirportMgr/RoadBuildHUD.h Source/AirportMgr/RoadBuildHUD.cpp Source/AirportMgr/RoadBuildHUDTest.cpp
git commit -m "feat(present): a guide line is dashed, violet, and not a plot edge"
```

---

### Task 4: `FToolContext::Guide`, and the driver that resolves it

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h` (`FToolContext`, `IBuildTool`)
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp` (`MakeContext`)
- Test: `Plugins/Airside/Source/AirsideTests/Private/ToolCursorTest.cpp` (two legs)
- Test: `Plugins/Airside/Source/AirsideTests/Private/BuildSessionTest.cpp` (one leg)

**Interfaces:**
- Consumes: `FGuideAnchor`, `FSnapGuideChain` (Task 2); `SnapGuide::FResult` (Task 1).
- Produces: `FToolContext::Guide` (a `SnapGuide::FResult`); `FVector2D
  FToolContext::GuidedCursor() const`; `virtual bool IBuildTool::DescribeGuideAnchor(
  FGuideAnchor& Out) const` defaulting to `return false;`; and `FBuildSession::MakeContext`
  filling `Guide`.

- [ ] **Step 1: Add the context member and its accessor**

In `RoadBuildTool.h`, add `#include "Tool/SnapGuideChain.h"` to the include block (after
`#include "Tool/RoadSnap.h"`), and add to `FToolContext`, immediately after the `FRoadSnapResult Snap;`
member and its comment:

```cpp

	/**
	 * What the cursor is lined up with - resolved by the DRIVER, exactly like Snap above and
	 * for the same recorded reason.
	 *
	 * Both drivers build their context through FBuildSession::MakeContext, so one gesture
	 * cannot guide differently in PIE and in the editor mode - which is the bug FRoadSnapSettings
	 * records having shipped before the two were merged.
	 *
	 * IT IS ALSO THE ONLY PLACE HYSTERESIS CAN LIVE. BuildPreview and BuildReadout are both
	 * const and neither may remember last frame's winner; the session holds it instead (see
	 * FBuildSession::LastGuide) and the flicker rule in design §5 depends on that.
	 */
	SnapGuide::FResult Guide;

	/**
	 * Where a MOVING point should go: the guide's answer when there is one, the raw cursor
	 * when there is not.
	 *
	 * The ternary written once rather than at every consumer - the same argument
	 * Network() makes for its own null check. A tool that read Guide.Point unconditionally
	 * would park its geometry at the origin on every frame no guide was active.
	 */
	FVector2D GuidedCursor() const { return Guide.bActive ? Guide.Point : Cursor; }
```

- [ ] **Step 2: Add the tool hook**

In `RoadBuildTool.h`, add to `IBuildTool`, immediately before `virtual void BuildPreview(...)`:

```cpp
	/**
	 * What this tool is dragging, and against what, for the guide chain. False means "no
	 * gesture is in progress", and the driver then resolves no guide at all.
	 *
	 * CONST AND CONTEXT-FREE, read off what the tool has already pinned. It cannot take an
	 * FToolContext because the driver calls it WHILE BUILDING ONE - and it should not want
	 * to: where the cursor is now is the chain's input, not the anchor's.
	 *
	 * RETURNS BOOL rather than setting a flag inside FGuideAnchor, so a caller branches on
	 * the return - CLAUDE.md's rule about honouring anything that fills an out-parameter.
	 *
	 * Silent by default, like BuildReadout above: eight tools implement this interface and
	 * stage 1 gives an anchor to exactly one of them.
	 */
	virtual bool DescribeGuideAnchor(FGuideAnchor& Out) const { return false; }

```

- [ ] **Step 3: Give the session the chain and the memory**

In `Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h`, add
`#include "Tool/SnapGuideChain.h"` to the include block, and add to the private section,
after the `FRoadSnapChain SnapChain;` member:

```cpp

	/**
	 * Extending then World. Not a UPROPERTY, like SnapChain above: it owns its sources
	 * through TUniquePtr and holds no state worth saving, only the ordering.
	 */
	FSnapGuideChain GuideChain;

	/**
	 * THE PREVIOUS WINNER - the one piece of state the flicker rule needs, and the reason
	 * the driver resolves the guide rather than the tool. IBuildTool::BuildPreview and
	 * BuildReadout are both const and could not hold it; a member on the tool would also put
	 * it on the wrong side of the two-driver split, where PIE and the editor mode each kept
	 * their own and drifted.
	 *
	 * mutable for the same reason Selection and LastPlaneHitValue above are: MakeContext is
	 * const, deliberately (see FBuildSessionTunables), and this is a reported answer rather
	 * than a decision.
	 *
	 * CLEARED WHENEVER NO TOOL OFFERS AN ANCHOR, so a winner cannot outlive the gesture that
	 * earned it and reappear on the next one - which is exactly the class of bug
	 * FPlotPlaceTool's own "stale corner" comment records.
	 */
	mutable SnapGuide::FResult LastGuide;
```

- [ ] **Step 4: Resolve it in `MakeContext`**

In `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp`, in `MakeContext`, replace

```cpp
	Context.SetCursor(PlaneHit, Snapped);
	return Context;
```

with

```cpp
	Context.SetCursor(PlaneHit, Snapped);

	// THE GUIDE, RESOLVED HERE AND NOWHERE ELSE - beside Snap, from the same place, for the
	// same recorded reason (design §2). The active tool says what it is dragging; this asks
	// the chain what that lines up with and hands the tool the answer, so a tool can neither
	// resolve a guide of its own nor remember one between frames.
	//
	// THE NETWORK GUARD IS NOT COSMETIC: every source takes a URoadNetwork& because stage 2's
	// four of them must query it. With no target there is nothing to guide against and
	// nothing on screen to guide, so the default inactive FResult is the right answer.
	FGuideAnchor Anchor;
	SnapGuide::FResult Guide;
	const URoadNetwork* Network = Target != nullptr ? Target->GetNetwork() : nullptr;
	const IBuildTool* Tool = GetActiveTool();
	if (Tool != nullptr && Network != nullptr && Tool->DescribeGuideAnchor(Anchor))
	{
		Guide = GuideChain.Resolve(*Network, Anchor, PlaneHit, LastGuide);
	}

	// ASSIGNED EVEN WHEN NOTHING RESOLVED, which is the clearing half: a gesture that ends
	// must not leave its winner behind for the next one to inherit and hold on to through
	// the hysteresis rule.
	LastGuide = Guide;
	Context.Guide = Guide;
	return Context;
```

`MakeContext` already has `Target` and `PlaneHit` in scope; `GetActiveTool()` is a const
member of this class. No new includes are needed beyond step 3's.

- [ ] **Step 5: Write the failing tests**

In `Plugins/Airside/Source/AirsideTests/Private/ToolCursorTest.cpp`, inside
`FToolCursorTest::RunTest`, immediately after the closing brace of section 1 (the block that
asserts `SetCursor` keeps the two answers apart), insert:

```cpp

	// 1b. GuidedCursor is the THIRD answer and must not fold into either of the other two.
	//     A tool reading Guide.Point unconditionally parks its geometry at the origin on
	//     every frame no guide is active, which looks exactly like a broken gesture.
	{
		FToolContext Context;
		Context.SetCursor(FVector2D(700.0, 300.0), FRoadSnapResult());

		TestTrue(TEXT("with no guide, the guided cursor IS the cursor"),
			Context.GuidedCursor().Equals(FVector2D(700.0, 300.0), 1e-6));

		Context.Guide.bActive = true;
		Context.Guide.Point = FVector2D(700.0, 0.0);
		TestTrue(TEXT("with a guide, it is the guide's constrained point"),
			Context.GuidedCursor().Equals(FVector2D(700.0, 0.0), 1e-6));
		TestTrue(TEXT("and the raw cursor is still there beside it, unchanged"),
			Context.Cursor.Equals(FVector2D(700.0, 300.0), 1e-6));
	}
```

In `Plugins/Airside/Source/AirsideTests/Private/BuildSessionTest.cpp`, add
`#include "Tool/SnapGuideChain.h"` to the include block (`FGuideAnchor` reaches it through
`Tool/BuildSession.h` either way, but include what you use), then immediately before the
closing `return true;` of `FBuildSessionTest::RunTest`, insert:

```cpp

	// 5. EVERY TOOL DECLINES AN ANCHOR WHEN IDLE. The registry walk, not a list of tools
	// written here - the same check items 2 and 3 above make, applied to the guide hook.
	// A tool that offered an anchor from its idle state would guide a gesture that has not
	// started, and the player would see a dashed line hanging off nothing.
	{
		FBuildSession IdleSession;
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			IdleSession.SelectTool(Index);
			const IBuildTool* Active = IdleSession.GetActiveTool();
			if (Active == nullptr) { continue; }

			FGuideAnchor Anchor;
			TestFalse(
				FString::Printf(TEXT("tool %d offers no guide anchor while it is idle"), Index),
				Active->DescribeGuideAnchor(Anchor));
		}
	}
```

- [ ] **Step 6: Build**

Expect `Result: Succeeded`.

- [ ] **Step 7: Run both tests**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides\AirportMgr.uproject" -Filter Airside.Tool
```

Expected: `Airside.Tool.CursorContract` and `Airside.Tool.BuildSession` both pass, 0 failed,
0 crashed. The test COUNT does not change - both are new legs on existing tests.

- [ ] **Step 8: Run the whole suite**

Expected: 0 failed, 0 crashed. Nothing on screen has changed yet: no tool reads `Guide`, and
`DescribeGuideAnchor` defaults to false, so every existing gesture behaves exactly as before.
That is what makes this task committable on its own.

- [ ] **Step 9: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp Plugins/Airside/Source/AirsideTests/Private/ToolCursorTest.cpp Plugins/Airside/Source/AirsideTests/Private/BuildSessionTest.cpp
git commit -m "feat(tool): the driver resolves the guide, beside the snap"
```

---

### Task 5: The plot's back corners follow it

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/PlotPlaceTool.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotGuideTest.cpp` (new)

**Interfaces:**
- Consumes: `FGuideAnchor` and `IBuildTool::DescribeGuideAnchor` (Task 4);
  `FToolContext::GuidedCursor()` and `FToolContext::Guide` (Task 4); `EPreviewStyle::Guide`
  (Task 3).
- Produces: nothing new. This is the stage's only consumer.

- [ ] **Step 1: Declare the override**

In `Plugins/Airside/Source/Airside/Public/Tool/PlotPlaceTool.h`, in the public section of
`FPlotPlaceTool`, immediately after `virtual void BuildReadout(...) const override;`:

```cpp

	/**
	 * THE BACK CORNERS ONLY - see the implementation for why the anchor and the frontage are
	 * left alone. This is what tells the driver's guide chain which point is moving and which
	 * edge it grew from; the tool resolves nothing itself.
	 */
	virtual bool DescribeGuideAnchor(FGuideAnchor& Out) const override;
```

- [ ] **Step 2: Implement it**

In `Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp`, immediately after
`FPlotPlaceTool::PinnedCount()`:

```cpp
bool FPlotPlaceTool::DescribeGuideAnchor(FGuideAnchor& Out) const
{
	// ONLY THE TWO BACK CORNERS. The anchor click is a search for a service road and the
	// frontage runs ALONG one in quantised 5 m steps - both are already constrained, and an
	// angular guide over them would be a second opinion about where they may go, which is how
	// two rules about one number come to disagree (see QuantisedFrontage's own comment).
	if (Stage != EPlotStage::CornerA && Stage != EPlotStage::CornerB)
	{
		return false;
	}

	const FVector2D Frontage = Corners[1] - Corners[0];
	if (Frontage.IsNearlyZero())
	{
		return false;
	}

	// THE CORNER THE MOVING EDGE GROWS FROM: the far end of the frontage while corner 2 is
	// being placed, the anchor while corner 3 is. Both measured against the SAME frontage
	// direction, which is what makes the pair of clicks a rectangle rather than two
	// unrelated right angles.
	const bool bFarEnd = Stage == EPlotStage::CornerA;
	Out.Origin      = bFarEnd ? Corners[1] : Corners[0];
	Out.ReferenceAt = bFarEnd ? Corners[0] : Corners[1];
	Out.Reference   = Frontage.GetSafeNormal();

	// THE TOOL NAMES ITS OWN REFERENCE, so the source can say "square to the frontage"
	// without knowing what a frontage is - the same split that keeps EPreviewStyle a meaning
	// rather than a colour.
	Out.ReferenceName = TEXT("the frontage");
	return true;
}
```

- [ ] **Step 3: Read the guided cursor for the back corners**

In `FPlotPlaceTool::Quad`, the back-corner section currently reads:

```cpp
	const FVector2D Back = Pinned == 2 ? InFront(Context.Cursor) : Corners[2];
```

Replace it with:

```cpp
	// THE GUIDED CURSOR, not the raw one. The driver resolved it (FToolContext::Guide) and
	// InFront still has the last word: a corner guided square to the frontage but dragged
	// behind it slides back onto the frontage line, because concrete on the carriageway is a
	// harder rule than an alignment aid. That is also why BuildPreview draws its guide line
	// from the corner SHOWN here rather than from Guide.Point.
	const FVector2D Back = Pinned == 2 ? InFront(Context.GuidedCursor()) : Corners[2];
```

And in the same function, the near-corner branch currently reads:

```cpp
	if (Pinned == 3)
	{
		Near = InFront(Context.Cursor);
	}
```

Replace `Context.Cursor` there with `Context.GuidedCursor()`. Leave the `Pinned == 1`
frontage branch reading `Context.Cursor` - it is quantised along the road and has no guide.

Before each edit, confirm the text being replaced appears EXACTLY ONCE in the file.

- [ ] **Step 4: Draw it**

In `FPlotPlaceTool::BuildPreview`, immediately after the four `Sink.Line(...)` boundary calls
and BEFORE the `if (Pinned < 3) { return; }` guard:

```cpp

	// THE DASHED LINE TO WHAT IT IS LINED UP WITH, and the label saying which - design §6,
	// and the whole of what the request asked for: a ray along the guide direction would say
	// "you are at 90 degrees", and this says WHICH edge you are square to.
	//
	// DRAWN FROM THE CORNER THE QUAD SHOWS, not from Guide.Point, because InFront above may
	// have moved it - a guide line that did not touch the shape would be pointing at nothing.
	//
	// GATED ON THE MOVING CORNER, because Guide is only ever active while one of the two back
	// corners is under the cursor (see DescribeGuideAnchor), and Shown[Pinned] IS that corner.
	if (Context.Guide.bActive && (Pinned == 2 || Pinned == 3) && Shown.IsValidIndex(Pinned))
	{
		const FVector2D Moving = Shown[Pinned];
		Sink.Line(Moving, Context.Guide.Winner.ReferenceAt, EPreviewStyle::Guide);
		Sink.Label(Moving, Context.Guide.Winner.Description, EPreviewStyle::Guide);
	}
```

- [ ] **Step 5: Write the failing tests**

Create `Plugins/Airside/Source/AirsideTests/Private/PlotGuideTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/BuildSession.h"
#include "Tool/PlotPlaceTool.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadEditTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Records the guide's own geometry, which is what this file is about.
	 *
	 * The plot's existing ghost sink (PlotPlaceToolTest) counts styles and measures depth;
	 * here the ENDPOINTS matter, because "a dashed line to the thing" is a claim about where
	 * the line goes and a style count cannot tell a line to the frontage from one to the
	 * world origin.
	 */
	struct FGuideSink : IToolPreviewSink
	{
		struct FRecordedLine
		{
			FVector2D From = FVector2D::ZeroVector;
			FVector2D To = FVector2D::ZeroVector;
			EPreviewStyle Style = EPreviewStyle::Pending;
		};

		TArray<FRecordedLine> Lines;
		TArray<FString> GuideLabels;

		virtual void Marker(const FVector2D& At, EPreviewStyle Style) override {}
		virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) override
		{
			Lines.Add({ From, To, Style });
		}
		virtual void CrossMark(const FVector2D& At, const FVector2D& Along, EPreviewStyle Style) override {}
		virtual void Label(const FVector2D& At, const FString& Text, EPreviewStyle Style) override
		{
			if (Style == EPreviewStyle::Guide) { GuideLabels.Add(Text); }
		}

		TArray<FRecordedLine> Of(EPreviewStyle Style) const
		{
			return Lines.FilterByPredicate(
				[Style](const FRecordedLine& L) { return L.Style == Style; });
		}
	};

	/** An east-west service road through the origin, long enough to anchor anywhere on. */
	void LayServiceRoad(ARoadNetworkActor* Actor)
	{
		IRoadEditTarget* Target = Actor;
		const int32 West = Target->PlaceNode(FVector2D(-20000.0, 0.0));
		const int32 East = Target->PlaceNode(FVector2D(20000.0, 0.0));
		Target->ConnectNodes(West, East, ERoadKind::ServiceRoad, INDEX_NONE);
	}

	/** The registry index of the fuel-depot tool, BY ID - never a literal. The registry is
	 *  the one list, and a test that hard-coded 8 would break on the next tool added. */
	int32 DepotToolIndex()
	{
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			if (Registry[Index].Id == FName(TEXT("FuelDepot"))) { return Index; }
		}
		return INDEX_NONE;
	}
}

/**
 * THE COMPOSITION TEST, and the one that fails if the tool never calls the chain.
 *
 * Every Solve/ and chain test in this stage passes on a tool that ignores FToolContext::Guide
 * entirely - which is why the contexts here come from FBuildSession::MakeContext, the seam
 * both drivers go through, rather than from TestTool::ContextAt. What is being pinned is the
 * whole path: tool declares an anchor, session resolves a guide, tool reads it back.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotCornerFollowsTheGuideTest,
	"Airside.Tool.PlotCornerFollowsTheGuide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotCornerFollowsTheGuideTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }
	LayServiceRoad(Actor);

	const int32 Depot = DepotToolIndex();
	if (!TestTrue(TEXT("the registry lists a fuel-depot tool"), Depot != INDEX_NONE))
	{
		return false;
	}

	FBuildSession Session;
	const FBuildSessionTunables Tunables = Actor->MakeTunables(10000.0);
	Session.SelectTool(Depot);

	IBuildTool* Tool = Session.GetActiveTool();
	if (!TestNotNull(TEXT("the depot tool is active"), Tool)) { return false; }

	auto ContextAt = [&](const FVector2D& Where)
	{
		return Session.MakeContext(Actor, Where, Tunables, false, false);
	};

	// Anchor from OFF the road, on its north side, then run the frontage east.
	Tool->OnClick(ContextAt(FVector2D(0.0, 1000.0)));
	Tool->OnClick(ContextAt(FVector2D(6000.0, 1000.0)));

	TArray<FVector2D> Frontage;
	{
		const FToolContext Probe = ContextAt(FVector2D(6000.0, 3000.0));
		static_cast<FPlotPlaceTool*>(Tool)->Quad(Probe, Frontage);
	}
	if (!TestTrue(TEXT("two corners are pinned before the back corner is dragged"),
		Frontage.Num() >= 2))
	{
		return false;
	}

	// A CORNER DRAGGED ALMOST SQUARE: out along +Y from the frontage's far end, and 60 uu
	// east of it - about 1.7 degrees off the perpendicular, inside the 7-degree tolerance.
	const FVector2D FarEnd = Frontage[1];
	const FToolContext Guided = ContextAt(FarEnd + FVector2D(60.0, 2000.0));

	if (!TestTrue(TEXT("the driver resolved a guide for a corner dragged near square"),
		Guided.Guide.bActive))
	{
		return false;
	}

	TArray<FVector2D> Shown;
	static_cast<FPlotPlaceTool*>(Tool)->Quad(Guided, Shown);
	if (!TestTrue(TEXT("the quad shows four corners"), Shown.Num() == 4)) { return false; }

	// EXACTLY SQUARE, not merely nearer. The corner the player gets is the guide's point, so
	// an assertion with a loose tolerance would pass on a tool that ignored the guide and
	// simply took a cursor that was already close.
	TestTrue(TEXT("the guided corner lands exactly square to the frontage"),
		FMath::IsNearlyEqual(Shown[2].X, FarEnd.X, 1.0e-6));
	TestFalse(TEXT("and therefore not where the raw cursor was"),
		FMath::IsNearlyEqual(Guided.Cursor.X, FarEnd.X, 1.0e-6));

	// THE DASHED LINE, AND WHERE IT GOES. To the frontage's other end - the edge being
	// squared to - and from the corner the quad actually shows.
	FGuideSink Sink;
	Tool->BuildPreview(Guided, Sink);

	const TArray<FGuideSink::FRecordedLine> GuideLines = Sink.Of(EPreviewStyle::Guide);
	if (!TestEqual(TEXT("exactly one guide line is drawn"), GuideLines.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("it starts at the corner the quad shows"),
		GuideLines[0].From.Equals(Shown[2], 1.0e-6));
	TestTrue(TEXT("and ends at the far end of the edge it is squared to"),
		GuideLines[0].To.Equals(Shown[0], 1.0e-6));

	if (!TestEqual(TEXT("and one label says which"), Sink.GuideLabels.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("naming the reference the tool supplied"),
		Sink.GuideLabels[0], FString(TEXT("square to the frontage")));

	// THE PLOT EDGES ARE STILL DRAWN, in their own styles. A guide that had replaced the
	// boundary rather than joined it would pass every assertion above.
	TestTrue(TEXT("the boundary is still drawn beside the guide"),
		Sink.Of(EPreviewStyle::Pinned).Num() + Sink.Of(EPreviewStyle::Provisional).Num() >= 4);

	return true;
}

/**
 * THE CONTROL: the guide must be OFF most of the time. A corner dragged nowhere near an
 * alignment keeps the cursor it was given, and nothing is drawn - otherwise the feature is a
 * constraint the player never asked for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotCornerIgnoresADistantGuideTest,
	"Airside.Tool.PlotCornerIgnoresADistantGuide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotCornerIgnoresADistantGuideTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }
	LayServiceRoad(Actor);

	const int32 Depot = DepotToolIndex();
	if (!TestTrue(TEXT("the registry lists a fuel-depot tool"), Depot != INDEX_NONE))
	{
		return false;
	}

	FBuildSession Session;
	const FBuildSessionTunables Tunables = Actor->MakeTunables(10000.0);
	Session.SelectTool(Depot);
	IBuildTool* Tool = Session.GetActiveTool();
	if (!TestNotNull(TEXT("the depot tool is active"), Tool)) { return false; }

	auto ContextAt = [&](const FVector2D& Where)
	{
		return Session.MakeContext(Actor, Where, Tunables, false, false);
	};

	Tool->OnClick(ContextAt(FVector2D(0.0, 1000.0)));
	Tool->OnClick(ContextAt(FVector2D(6000.0, 1000.0)));

	TArray<FVector2D> Frontage;
	{
		const FToolContext Probe = ContextAt(FVector2D(6000.0, 3000.0));
		static_cast<FPlotPlaceTool*>(Tool)->Quad(Probe, Frontage);
	}
	if (!TestTrue(TEXT("two corners are pinned"), Frontage.Num() >= 2)) { return false; }

	// 30 degrees off the perpendicular and 15 off the nearest world axis: nothing is within
	// the 7-degree tolerance.
	const FVector2D FarEnd = Frontage[1];
	const FToolContext Free = ContextAt(FarEnd + FVector2D(1150.0, 2000.0));

	TestFalse(TEXT("a corner dragged between alignments is offered no guide"),
		Free.Guide.bActive);

	TArray<FVector2D> Shown;
	static_cast<FPlotPlaceTool*>(Tool)->Quad(Free, Shown);
	if (!TestTrue(TEXT("the quad still shows four corners"), Shown.Num() == 4)) { return false; }
	TestTrue(TEXT("and the corner is exactly where the cursor is"),
		Shown[2].Equals(Free.Cursor, 1.0e-6));

	FGuideSink Sink;
	Tool->BuildPreview(Free, Sink);
	TestEqual(TEXT("with no guide line drawn"), Sink.Of(EPreviewStyle::Guide).Num(), 0);
	TestEqual(TEXT("and no guide label"), Sink.GuideLabels.Num(), 0);

	return true;
}

/**
 * THE WINNER DIES WITH THE GESTURE. The session holds it so hysteresis can work; a winner
 * that outlived the gesture would be held into the NEXT one by that same rule, and the
 * player would get a guide off an edge that no longer exists.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotGuideEndsWithTheGestureTest,
	"Airside.Tool.PlotGuideEndsWithTheGesture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotGuideEndsWithTheGestureTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }
	LayServiceRoad(Actor);

	const int32 Depot = DepotToolIndex();
	if (!TestTrue(TEXT("the registry lists a fuel-depot tool"), Depot != INDEX_NONE))
	{
		return false;
	}

	FBuildSession Session;
	const FBuildSessionTunables Tunables = Actor->MakeTunables(10000.0);
	Session.SelectTool(Depot);
	IBuildTool* Tool = Session.GetActiveTool();
	if (!TestNotNull(TEXT("the depot tool is active"), Tool)) { return false; }

	auto ContextAt = [&](const FVector2D& Where)
	{
		return Session.MakeContext(Actor, Where, Tunables, false, false);
	};

	Tool->OnClick(ContextAt(FVector2D(0.0, 1000.0)));
	Tool->OnClick(ContextAt(FVector2D(6000.0, 1000.0)));

	TArray<FVector2D> Frontage;
	{
		const FToolContext Probe = ContextAt(FVector2D(6000.0, 3000.0));
		static_cast<FPlotPlaceTool*>(Tool)->Quad(Probe, Frontage);
	}
	if (!TestTrue(TEXT("two corners are pinned"), Frontage.Num() >= 2)) { return false; }

	const FVector2D Square = Frontage[1] + FVector2D(60.0, 2000.0);
	if (!TestTrue(TEXT("a guide is live mid-gesture"), ContextAt(Square).Guide.bActive))
	{
		return false;
	}

	// PUT THE TOOL DOWN. SelectTool deactivates the depot tool, which returns it to Idle, so
	// it offers no anchor and the session must forget the winner it was holding.
	Session.SelectTool(0);
	TestFalse(TEXT("with the tool put down, the guide is gone"),
		ContextAt(Square).Guide.bActive);

	// AND IT DOES NOT COME BACK on re-selecting the tool at the same cursor: the gesture
	// starts from Idle, which has no anchor at all.
	Session.SelectTool(Depot);
	TestFalse(TEXT("and a fresh gesture does not inherit it"),
		ContextAt(Square).Guide.bActive);

	return true;
}

#endif
```

- [ ] **Step 6: Build**

Expect `Result: Succeeded`. New `.cpp`: if the tests do not appear in step 8, build again.

- [ ] **Step 7: Run the new tests**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides\AirportMgr.uproject" -Filter Airside.Tool.Plot
```

Expected: every existing `Airside.Tool.Plot*` test still passes, plus the three new ones -
`PlotCornerFollowsTheGuide`, `PlotCornerIgnoresADistantGuide`, `PlotGuideEndsWithTheGesture`.
0 failed, 0 crashed.

If `Airside.Tool.PlotBackCornersAreFree` fails, read it before touching it: it asserts the
corners move freely in the plane, which the guide narrows near an alignment. If its cursor
happens to sit within 7 degrees of square, the test is now measuring the guide and its
positions need moving off an alignment - with a comment saying why. Do NOT weaken its
assertion.

- [ ] **Step 8: Prove the composition test can fail**

The spec's warning in full: every `Solve/` test in this stage passes if the tool never calls
the chain.

1. In `PlotPlaceTool.cpp`, change `InFront(Context.GuidedCursor())` back to
   `InFront(Context.Cursor)` in the `Pinned == 2` branch only.
2. Build, run `-Filter Airside.Tool.PlotCornerFollowsTheGuide`.
3. Expected: `1 test(s) run, 1 failed` - on "the guided corner lands exactly square to the
   frontage".
4. Restore it, build, rerun: `1 test(s) run, 0 failed`.
5. `git status --short` must show only the files this task means to change.

- [ ] **Step 9: Run the whole suite**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides\AirportMgr.uproject"
```

Expected: 0 failed, 0 crashed, total up by 12 across the stage (5 + 3 + 1 + 3; Task 4 added
legs, not tests).

- [ ] **Step 10: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/PlotPlaceTool.h Plugins/Airside/Source/Airside/Private/Tool/PlotPlaceTool.cpp Plugins/Airside/Source/AirsideTests/Private/PlotGuideTest.cpp
git commit -m "feat(tool): a plot's back corners square themselves to the frontage"
```

- [ ] **Step 11: Judge it in PIE, which is the only thing that can**

§11 leaves `ToleranceDegrees` (7) and `StickinessDegrees` (2) as placement feel. Nothing
above measures feel. Ask the user to:

1. Open the project, press `0` (Fuel depot), click beside a service road, drag the frontage,
   click.
2. Drag the third corner slowly through square.

What confirms the stage: a violet dashed line appears back along the frontage as the corner
comes within a few degrees of square, with "square to the frontage" beside it; the corner
sits exactly square while it is shown; and the line does NOT flicker on and off as the
cursor creeps past.

What to report back: whether 7 degrees grabs too early or too late, and whether the guide
ever visibly alternates. Those two answers are what decides whether the numbers move - and
whether they become `UAirsideSettings` knobs, which is a stage-2 edit either way.

---

## Unresolved questions

1. **`ReferenceName` is unlocalised** (`TEXT("the frontage")`), so the label reads English in
   every culture. Every other player-facing string in the tools is `LOCTEXT`. Left as
   `FString` because `FCandidate::Description` is an `FString` by the spec's own §4
   reasoning - but that reasoning was about the plugin not switching on description KINDS,
   not about localisation. Worth settling before stage 2 multiplies it by five sources.
2. **`Airside.Tool.PlotBackCornersAreFree` may now sit on an alignment.** Step 8 says what to
   do if it fails, but if it passes, its cursors are off-alignment by luck rather than by
   intent. Should it get an explicit comment saying it deliberately drags nowhere near
   square?
3. **The editor mode draws `Guide` solid and shows no label** (`FViewportPreviewSink`). The
   same limitation `Provisional` already has, so it is consistent - but the editor mode is
   where levels get authored, and a solid violet line touching a solid white one reads worse
   there than the dashed pair does in PIE. Fix in this stage, or leave for the stage that
   gives the viewport sink a dash?
