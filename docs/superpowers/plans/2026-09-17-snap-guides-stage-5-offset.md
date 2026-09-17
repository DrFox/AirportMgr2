# Snap guides, stage 5: Offset, and the family that turned out not to be needed

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A road drawn beside two evenly spaced taxiways offers to keep the same gap, so a
field laid out by hand comes out regular.

**Architecture:** `FOffsetGuideSource` proposes `EFit::Perpendicular` candidates - a line
parallel to the nearest road, at the gap a neighbouring parallel road already keeps. **No new
arbitration pass**, and that is the finding this stage turns on: §2 asked for a separate
distance family, and the `EFit` split introduced in stage 1 already provides exactly what it
asked for. The dead `FCandidate::Distance` field goes with it.

**Tech Stack:** UE 5.8.2 C++, UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-17-snap-guides-design.md` - §8 stage 5, plus
amendments to §2 and §4.

**Stacked on:** `feature/snap-guides-roads` (PR #149), itself on #148 → #147 → #146. None
merged. This branch is `feature/snap-guides-offset` and its PR targets
`feature/snap-guides-roads`.

## The finding this stage rests on

§2 says:

> DIRECTION guides constrain which way; DISTANCE guides constrain how far. [...] They are
> separate lists with separate arbitration, **because a rule that picked one winner across
> both would have "parallel to that taxiway" losing to "30 m from the last one"**.

That is the exact problem `EFit` solves, and has solved since stage 1's multi-alignment change:
**one winner per fit kind**, so an angular guide and a perpendicular guide both hold and never
compete for one slot. §2's requirement is met by machinery that arrived for a different reason.

And §4 defines Offset's quantity as:

> For Offset: how far along the **perpendicular**, uu.

A distance along the perpendicular from a reference road IS a line parallel to that road. That
is an `EFit::Perpendicular` candidate - `Through` a point at that offset, `Direction` the
road's own. Nothing else is required.

**So there is no distance family, no second arbitration pass, and no use for
`FCandidate::Distance`** - which has been declared and never read or written since stage 1
(verified by grep on this branch). Task 2 deletes it.

**What WOULD need the second family**, and is not Offset: a LENGTH guide - "make this segment
40 m, the same as the one before it". That constrains distance ALONG the drag direction, which
is a point on a ray and not a line. §3 lists no such source. If one is ever wanted, §2's
argument comes back into force and this plan's finding does not apply to it.

## Global Constraints

- **Worktree.** Every command runs from `C:\repos\AirportMgr2_snap-offset`. Do not `cd` to
  `C:\repos\AirportMgr2` nor to the four worktrees holding PRs #146-#149 open.
- **Build.**

  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2_snap-offset\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
  ```

  Check the literal line `Result: Succeeded`. **It exits 0 even when it fails** - that has
  happened five times across stages 1-4. Never read the exit code.
- **Tests.**

  ```
  ./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-offset\AirportMgr.uproject"
  ```

  Read `N test(s) run, N failed, N crashed`; **crashed must be 0**. **Baseline is 378**; record
  the real number before Task 1.
- **A new `.cpp` sometimes needs two builds.** **Unity build** - prefix file-local helpers.
- **`Solve/` includes `CoreMinimal.h` and `Solve/` only**; `Tool/` may include `Solve/` and
  `Model/`, never `Present/`.
- **Never `git checkout --` a file whose task is not yet committed.**
- **No string replacement without asserting it matched exactly once.**
- **Commits:** no `Co-Authored-By` trailer.

## What stages 1-4 provide

```cpp
// Solve/GuideArbiter.h
namespace SnapGuide
{
    enum class ESource : uint8
    { Extending, PointAlign, Aligned, Collinear, Parallel, Runway, World, Offset };
    enum class EFit : uint8 { Angular, Perpendicular };
    struct FCandidate
    {
        FVector2D Direction;      // unit; with Through, the LINE
        FVector2D Through;        // the point the line passes through
        double    Distance = 0.0; // DEAD - deleted in task 2
        EFit      Fit;
        FVector2D ReferenceAt;    // where the dashed line is drawn TO
        FString   Description;
        ESource   Source;
    };
    struct FTuning { /* ... */ double ToleranceUu = 300.0; double SearchRadiusUu = 10000.0; };
}

// Tool/SnapGuideChain.h - seven installed sources, each answering Kind()
struct AIRSIDE_API IGuideSource
{
    virtual void Propose(const URoadNetwork&, const FGuideAnchor&,
        TArray<SnapGuide::FCandidate>&) const = 0;
    virtual SnapGuide::ESource Kind() const = 0;
};
class AIRSIDE_API FSnapGuideChain
{
    FSnapGuideChain();   // installs Extending, PointAlign, Aligned, Collinear, Parallel,
                         // Runway, World - in §3 order
    SnapGuide::FResult Resolve(const URoadNetwork&, const FGuideAnchor&, const FVector2D& Cursor,
        const SnapGuide::FResult& Previous, const FSnapGuideSettings& Enabled = {},
        const SnapGuide::FTuning& Tuning = {}) const;
};

// Tool/SnapGuideSettings.h - bOffset exists and defaults false.
// Source/AirportMgr/BuildActions.cpp:175 - the snap.offset button exists, with IsEnabled=Never
// (greyed), because nothing proposed the source. Task 1 un-greys it.

// Private/Tool/SnapGuideChain.cpp already has, in its anonymous namespace:
//   bool GuideSegmentEnds(const URoadNetwork&, FRoadSegmentId, FVector2D& OutA, FVector2D& OutB)
//   FVector2D ClosestOn(const FVector2D& A, const FVector2D& B, const FVector2D& P)
// and Tool/RoadNaming.h: FString RoadNaming::Describe(const URoadNetwork&, FRoadSegmentId)
```

## Decisions this plan takes

1. **Offset is a Perpendicular source**, per the finding above. No new family, no new pass.
2. **The reference is the NEAREST road to the drag**, the same road `FParallelGuideSource`
   picks, so "parallel to the taxiway" and "the same gap as its neighbour" describe the same
   reference and compose into one sensible answer.
3. **A neighbour is a road parallel to the reference**, within `SearchRadiusUu`, whose
   perpendicular gap from the reference is not near zero. Non-parallel roads have no single
   gap to copy.
4. **The proposed line is on the side of the reference AWAY FROM the neighbour**, so it can
   never land on the road that suggested the number. An earlier draft of this plan said "on the
   cursor's side", which is the same thing only while the cursor is on the far side from the
   neighbour: with roads at y=0 and y=3000 and a drag starting at y=1500, the reference is
   y=3000, the cursor's side is DOWN, and the offered line is y=0 - the neighbour itself. Away
   from the neighbour is decision 4's stated reason implemented directly, and it needs no
   anchor. When the drag is between the pair the line it proposes is simply far from the
   cursor, so nothing is offered, which is the honest answer: that gap is already taken.
5. **The label carries the number**: "40 m, matching the taxiway". A gap guide whose label
   said only "matching the taxiway" would leave the player unable to tell 40 m from 45 m,
   which is the one thing they are trying to control.

## File Structure

| File | Responsibility |
|---|---|
| `Public/Tool/SnapGuideChain.h` | `FOffsetGuideSource` declaration |
| `Private/Tool/SnapGuideChain.cpp` | its `Propose`, and installing it |
| `Source/AirportMgr/BuildActions.cpp` | the Offset button stops being greyed |
| `Public/Tool/SnapGuideSettings.h` | `bOffset`'s "until stage 4" comment (task 1) |
| `Public/Solve/GuideArbiter.h` | `FCandidate::Distance` deleted (task 2) |
| `AirsideTests/Private/OffsetGuideTest.cpp` (new) | the source, over a real network |
| `docs/superpowers/specs/2026-09-17-snap-guides-design.md` | §2, §4 and §8 amended (task 3) |

---

### Task 1: The Offset source, and its button lights up

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp`
- Modify: `Source/AirportMgr/BuildActions.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/SnapGuideChainTest.cpp` (source count)
- Create: `Plugins/Airside/Source/AirsideTests/Private/OffsetGuideTest.cpp`

**Interfaces:**
- Consumes: `IGuideSource`, `GuideSegmentEnds`, `ClosestOn`, `RoadNaming::Describe`,
  `SnapGuide::FTuning::SearchRadiusUu` (stages 1-4).
- Produces: `struct FOffsetGuideSource final : public IGuideSource`.

- [ ] **Step 1: Record the baseline**

Run the full test line. Expected `378 test(s) run, 0 failed, 0 crashed`. Write it down.

- [ ] **Step 2: Declare the source**

In `Public/Tool/SnapGuideChain.h`, after `FRunwayGuideSource`:

```cpp

/**
 * Source 8: the gap a neighbouring parallel road already keeps.
 *
 * PERPENDICULAR, NOT A NEW "DISTANCE FAMILY". Design §2 asked for direction and distance to be
 * separate lists with separate arbitration, because "a rule that picked one winner across both
 * would have 'parallel to that taxiway' losing to '30 m from the last one'". EFit already does
 * exactly that: one winner per kind, so an angular guide and this one both hold and never
 * compete. And §4's "how far along the perpendicular" IS a line parallel to the reference at
 * that offset - which is what a Perpendicular candidate already means.
 *
 * WHAT WOULD still need the second family, and is not this: a LENGTH guide - "make this segment
 * the same 40 m as the last one" - which constrains distance ALONG the drag and is a point on a
 * ray, not a line. §3 lists no such source.
 */
struct AIRSIDE_API FOffsetGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const override;

	virtual SnapGuide::ESource Kind() const override { return SnapGuide::ESource::Offset; }
};
```

- [ ] **Step 3: Implement it**

In `Private/Tool/SnapGuideChain.cpp`, before `FSnapGuideChain::FSnapGuideChain()`:

```cpp
void FOffsetGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	TArray<SnapGuide::FCandidate>& Out) const
{
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;

	// THE SAME REFERENCE FParallelGuideSource PICKS - the nearest road to the drag - so
	// "parallel to the taxiway" and "the same gap as its neighbour" describe one road between
	// them, and the two guides compose into an answer rather than two unrelated ones.
	FRoadSegmentId Reference;
	FVector2D ReferenceAt = FVector2D::ZeroVector;
	FVector2D ReferenceDir = FVector2D::ZeroVector;
	double BestSquared = Reach * Reach;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);
		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!GuideSegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		const FVector2D On = ClosestOn(A, B, Anchor.Origin);
		const double Squared = FVector2D::DistSquared(On, Anchor.Origin);
		if (Span.IsNearlyZero() || Squared > BestSquared)
		{
			continue;
		}

		BestSquared = Squared;
		Reference = Id;
		ReferenceAt = On;
		ReferenceDir = Span.GetSafeNormal();
	}

	if (ReferenceDir.IsNearlyZero())
	{
		return;
	}

	const FVector2D Across = RoadGeom::PerpCCW(ReferenceDir);
	const FString ReferenceName = RoadNaming::Describe(Network, Reference);

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);
		if (Id == Reference)
		{
			continue;
		}

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!GuideSegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		if (Span.IsNearlyZero()
			|| FVector2D::DistSquared(ClosestOn(A, B, Anchor.Origin), Anchor.Origin) > Reach * Reach)
		{
			continue;
		}

		// A NEIGHBOUR IS A ROAD PARALLEL TO THE REFERENCE. One that crosses it has no single
		// gap to copy - the distance between them depends where you measure, so there is no
		// number to offer.
		const FVector2D Dir = Span.GetSafeNormal();
		if (!FMath::IsNearlyZero(Dir.X * ReferenceDir.Y - Dir.Y * ReferenceDir.X, 1.0e-3))
		{
			continue;
		}

		// THE GAP, measured perpendicular from the reference's line to the neighbour's near
		// end. Near-zero means the two are the same road drawn twice, or a continuation of it:
		// offering a zero gap would propose drawing on top of the reference.
		const FVector2D ToNeighbour = ClosestOn(A, B, ReferenceAt) - ReferenceAt;
		const double Signed = FVector2D::DotProduct(ToNeighbour, Across);
		const double Gap = FMath::Abs(Signed);
		if (Gap < 1.0)
		{
			continue;
		}

		SnapGuide::FCandidate Match;
		Match.Direction = ReferenceDir;

		// AWAY FROM THE NEIGHBOUR, never toward it: the whole offer is "another road, one gap
		// further on", and a line laid on the neighbour's own side would propose drawing on top
		// of the very road that suggested the number. Keyed to the neighbour rather than to the
		// cursor because the two agree everywhere the guide can actually be seen - a drag
		// BETWEEN the pair is nearer the reference than the neighbour, so the cursor's side is
		// the neighbour's side, and "the cursor's side" would hand back the neighbour's line.
		Match.Through = ReferenceAt - Across * FMath::Sign(Signed) * Gap;
		Match.Fit = SnapGuide::EFit::Perpendicular;
		Match.ReferenceAt = ReferenceAt;
		Match.Source = SnapGuide::ESource::Offset;

		// THE NUMBER IS IN THE LABEL. "matching the taxiway" alone would leave the player
		// unable to tell 40 m from 45 m, which is the one thing they are trying to control.
		Match.Description = FString::Printf(TEXT("%.0f m, matching %s"),
			Gap / 100.0, *ReferenceName);
		Out.Add(Match);
	}
}
```

Install it last, completing §3's order:

```cpp
FSnapGuideChain::FSnapGuideChain()
{
	AddSource(MakeUnique<FExtendingGuideSource>());
	AddSource(MakeUnique<FPointAlignGuideSource>());
	AddSource(MakeUnique<FAlignedGuideSource>());
	AddSource(MakeUnique<FCollinearGuideSource>());
	AddSource(MakeUnique<FParallelGuideSource>());
	AddSource(MakeUnique<FRunwayGuideSource>());
	AddSource(MakeUnique<FWorldGuideSource>());
	AddSource(MakeUnique<FOffsetGuideSource>());
}
```

Then bump the chain's source count in
`Plugins/Airside/Source/AirsideTests/Private/SnapGuideChainTest.cpp` from 7 to 8 - grep
`Chain.NumSources()` and assert the literal you replace appears exactly once.

- [ ] **Step 4: Let the button work**

In `Source/AirportMgr/BuildActions.cpp`, the `snap.offset` row ends with `Never` as its
`IsEnabled`, with a comment saying nothing proposes the source until stage 4 (now 5). Replace
that argument with `Always` and the comment with:

```cpp
		// LIVE SINCE STAGE 5: FOffsetGuideSource proposes for it, so the button is no longer a
		// promise. It was greyed rather than absent precisely so this change is one word.
```

Two comments elsewhere say the same thing and are now wrong. Both are one line, and a stale
"nothing proposes this" is exactly the kind of claim "a log line is not evidence the thing it
describes exists" warns about - a reader believes it:

- `Public/Tool/SnapGuideSettings.h`, on `bOffset`: "Nothing proposes this until stage 4."
  becomes "The gap a neighbouring parallel road keeps. Off by default: judged in PIE first."
- `AirsideTests/Private/SnapGuideChainTest.cpp`, above the `NumSources` assertion: "Stage 2
  takes this from 3 to 7, one at a time." gains ", and stage 5 to 8."

- [ ] **Step 5: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/OffsetGuideTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/GuideArbiter.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	void LayTaxiway(ARoadNetworkActor* Actor, const FVector2D& From, const FVector2D& To)
	{
		IRoadEditTarget* Target = Actor;
		const int32 A = Target->PlaceNode(From);
		const int32 B = Target->PlaceNode(To);
		Target->ConnectNodes(A, B, ERoadKind::Taxiway, INDEX_NONE);
	}

	FGuideAnchor AnchorAt(const FVector2D& Origin)
	{
		FGuideAnchor Anchor;
		Anchor.Origin = Origin;
		return Anchor;
	}
}

/**
 * TWO ROADS 40 m APART OFFER A THIRD AT 40 m. That is the whole of Offset: a field laid out by
 * hand comes out regular because the spacing already there is what is proposed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOffsetGuideMatchesTheExistingGapTest,
	"Airside.Tool.OffsetGuideMatchesTheExistingGap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOffsetGuideMatchesTheExistingGapTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// Two east-west taxiways, 4000 uu (40 m) apart.
	LayTaxiway(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0));
	LayTaxiway(Actor, FVector2D(-10000.0, 4000.0), FVector2D(10000.0, 4000.0));
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	// A drag just north of the SECOND road: its nearest road is the one at y = 4000, and the
	// neighbour 4000 below it is the gap to copy.
	const FOffsetGuideSource Source;
	TArray<SnapGuide::FCandidate> Candidates;
	Source.Propose(*Actor->Network, AnchorAt(FVector2D(0.0, 5000.0)), Candidates);

	if (!TestEqual(TEXT("the one neighbouring road offers its gap"), Candidates.Num(), 1))
	{
		return false;
	}

	TestEqual(TEXT("perpendicular, because it is a line the new road must sit on"),
		static_cast<int32>(Candidates[0].Fit),
		static_cast<int32>(SnapGuide::EFit::Perpendicular));
	TestTrue(TEXT("running parallel to the road it is measured from"),
		FMath::IsNearlyZero(Candidates[0].Direction.Y, 1.0e-6));

	// AWAY FROM THE NEIGHBOUR: 4000 above the reference at y = 4000, not below it where the
	// neighbour already is.
	TestTrue(TEXT("the line sits one gap beyond the reference, away from the neighbour"),
		FMath::IsNearlyEqual(Candidates[0].Through.Y, 8000.0, 1.0));

	// THE NUMBER IS IN THE LABEL, or the player cannot tell 40 m from 45 m.
	TestTrue(*FString::Printf(TEXT("the label carries the gap, got '%s'"),
		*Candidates[0].Description),
		Candidates[0].Description.Contains(TEXT("40 m")));

	// AND IT PULLS A NEAR CURSOR EXACTLY ONTO THE SPACING.
	const SnapGuide::FResult Result = SnapGuide::Arbitrate(
		Candidates, FVector2D(0.0, 5000.0), FVector2D(3000.0, 7900.0), SnapGuide::FResult());
	TestTrue(TEXT("a drag within tolerance of the matching gap is offered it"), Result.bActive);
	TestTrue(TEXT("and lands exactly on it"),
		FMath::IsNearlyEqual(Result.Point.Y, 8000.0, 1.0e-6));

	return true;
}

/**
 * A ROAD THAT CROSSES THE REFERENCE HAS NO GAP TO COPY. The distance between two crossing roads
 * depends where you measure, so there is no number to offer - and offering one anyway would be
 * a figure the player could not account for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOffsetGuideIgnoresACrossingRoadTest,
	"Airside.Tool.OffsetGuideIgnoresACrossingRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOffsetGuideIgnoresACrossingRoadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// One east-west reference, and one north-south road crossing it.
	LayTaxiway(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0));
	LayTaxiway(Actor, FVector2D(2000.0, -8000.0), FVector2D(2000.0, 8000.0));
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	const FOffsetGuideSource Source;
	TArray<SnapGuide::FCandidate> Crossing;
	Source.Propose(*Actor->Network, AnchorAt(FVector2D(0.0, 1500.0)), Crossing);
	TestEqual(TEXT("a crossing road offers no gap"), Crossing.Num(), 0);

	// CONTROL LEG: add a PARALLEL neighbour and a drag beside the pair does get an offer - so
	// the assertion above is measuring the parallel test, not an empty search.
	//
	// THE DRAG SITS OUTSIDE THE PAIR, at y = 3500, rather than between them at y = 1500. Not
	// fussiness: at 1500 the two roads are EQUIDISTANT, so which one becomes the reference is
	// decided by the network's iteration order, and the leg would be asserting against an
	// accident. At 3500 the reference is the road at 3000 and there is one answer.
	LayTaxiway(Actor, FVector2D(-10000.0, 3000.0), FVector2D(10000.0, 3000.0));
	TArray<SnapGuide::FCandidate> WithNeighbour;
	Source.Propose(*Actor->Network, AnchorAt(FVector2D(0.0, 3500.0)), WithNeighbour);
	if (!TestEqual(TEXT("but a parallel one does"), WithNeighbour.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("one 3000 gap beyond the reference, away from the neighbour"),
		FMath::IsNearlyEqual(WithNeighbour[0].Through.Y, 6000.0, 1.0));

	return true;
}

/**
 * A DRAG BETWEEN THE PAIR IS NEVER OFFERED THE NEIGHBOUR'S OWN LINE.
 *
 * The first draft of this source put the line on the CURSOR's side of the reference, which is
 * the same side as the NEIGHBOUR whenever the drag starts between the two - so the gap it
 * offered to keep was the one already occupied, and the guide proposed drawing a road on top of
 * an existing one. Offsetting away from the neighbour instead is what this pins.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOffsetGuideNeverProposesTheNeighboursLineTest,
	"Airside.Tool.OffsetGuideNeverProposesTheNeighboursLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOffsetGuideNeverProposesTheNeighboursLineTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	LayTaxiway(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0));
	LayTaxiway(Actor, FVector2D(-10000.0, 3000.0), FVector2D(10000.0, 3000.0));
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	// BETWEEN THEM, and nearer the northern one, so the reference is unambiguous and the
	// neighbour lies on the same side as the drag.
	const FOffsetGuideSource Source;
	TArray<SnapGuide::FCandidate> Candidates;
	Source.Propose(*Actor->Network, AnchorAt(FVector2D(0.0, 2000.0)), Candidates);

	for (const SnapGuide::FCandidate& Candidate : Candidates)
	{
		TestTrue(*FString::Printf(
			TEXT("no guide is offered along an existing road, got y = %.1f"),
			Candidate.Through.Y),
			!FMath::IsNearlyEqual(Candidate.Through.Y, 0.0, 1.0)
				&& !FMath::IsNearlyEqual(Candidate.Through.Y, 3000.0, 1.0));
	}

	// AND THE CURSOR IS OFFERED NOTHING while it is in there: the only line on the drag's own
	// side is the neighbour's, and that gap is taken.
	const SnapGuide::FResult Result = SnapGuide::Arbitrate(
		Candidates, FVector2D(0.0, 2000.0), FVector2D(2000.0, 2050.0), SnapGuide::FResult());
	TestFalse(TEXT("a drag between two roads is offered no gap to match"), Result.bActive);

	return true;
}

#endif
```

- [ ] **Step 6: Build**

Expect `Result: Succeeded`. A new `.cpp`: if the tests do not appear in step 7, build again.

- [ ] **Step 7: Run the new tests**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-offset\AirportMgr.uproject" -Filter Airside.Tool.OffsetGuide
```

Expected: `3 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 8: Run the whole suite**

Expected: baseline + 3, 0 failed, 0 crashed. **Offset defaults OFF** (`FSnapGuideSettings::
bOffset`), so no existing chain test can change behaviour - if one does, the source is being
proposed when it should be skipped, which means `Kind()` is wrong.

- [ ] **Step 9: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp Source/AirportMgr/BuildActions.cpp Plugins/Airside/Source/AirsideTests/Private/SnapGuideChainTest.cpp Plugins/Airside/Source/AirsideTests/Private/OffsetGuideTest.cpp
git commit -m "feat(tool): a road can keep the gap its neighbour already keeps"
```

---

### Task 2: Delete the field that was never used

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h`

**Interfaces:**
- Consumes: nothing.
- Produces: `SnapGuide::FCandidate` without `Distance`.

- [ ] **Step 1: Prove it is dead before deleting it**

```
grep -rn "\.Distance\b" Plugins/Airside/Source Source/AirportMgr
```

Expected: **no hit that refers to `FCandidate::Distance`.** Hits on `FVector2D::Distance`,
`DistSquared` or `SearchRadiusUu` are different things and do not count. If anything does read
it, stop - the field is live and this task is wrong.

- [ ] **Step 2: Delete it**

In `Public/Solve/GuideArbiter.h`, remove:

```cpp
		/** For Offset: how far along the perpendicular, uu. Zero for direction guides, and
		 *  unread until stage 4 - carried now so the type does not change under stage 2. */
		double Distance = 0.0;
```

- [ ] **Step 3: Build**

Expect `Result: Succeeded`. **A compile error here is the good outcome to fear**: it would
mean something read the field and step 1's grep missed it. Fix by restoring the field, not by
patching the caller.

- [ ] **Step 4: Run the whole suite**

Expected: baseline + 3 (unchanged from task 1), 0 failed, 0 crashed.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h
git commit -m "refactor(solve): drop the distance field no guide ever used"
```

---

### Task 3: Amend the spec, and judge it in PIE

**Files:**
- Modify: `docs/superpowers/specs/2026-09-17-snap-guides-design.md`

- [ ] **Step 1: Correct §2's two-family claim**

Append to that bullet:

```markdown
  **AMENDED 2026-09-17, after stage 5.** The separate arbitration this asked for was never
  built, because `EFit` - added in stage 1 for simultaneous alignments - already provides
  exactly what the reasoning above demands: one winner per fit kind, so "parallel to that
  taxiway" and "the same gap as its neighbour" both hold and never compete for one slot. And a
  distance along the perpendicular IS a line, which is what a Perpendicular candidate already
  means. A LENGTH guide - "the same 40 m as the last segment" - would still need the second
  family, because a distance along the DRAG is a point on a ray rather than a line; no such
  source is listed in §3.
```

- [ ] **Step 2: Correct §4's Distance field**

Remove the `Distance` line from §4's `FCandidate` listing and add beneath the struct:

```markdown
**`Distance` was removed in stage 5.** It was declared for Offset and never read: an offset is
a LINE parallel to its reference, so the line's own `Through` point carries it.
```

- [ ] **Step 3: Mark stage 5 done in §8**

```markdown
5. ~~**Offset**, the distance family, which needs its own arbitration pass.~~ **Done
   2026-09-17 - and it needed no such pass.** See §2's amendment. Offset proposes a
   Perpendicular line parallel to the nearest road, at the gap a neighbouring parallel road
   already keeps, on the side of the reference AWAY from that neighbour - so it can never
   propose the line of a road that is already there. Its label carries the number,
   because "matching the taxiway" alone cannot distinguish 40 m from 45 m.

**All five stages are done.** What §10 still lists as out of scope remains so: curved roads,
terrain contours, and numeric entry.
```

- [ ] **Step 4: Run the whole suite**

Expected: baseline + 3, 0 failed, 0 crashed.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/specs/2026-09-17-snap-guides-design.md
git commit -m "docs(spec): Offset needed no distance family, and stage 5 landed"
```

- [ ] **Step 6: Judge it in PIE**

1. Lay two parallel taxiways roughly 40 m apart.
2. Turn **Offset** on in the Snap section - it is off by default and now clickable.
3. Start a third taxiway beside them and drag it out at about the same spacing.

What confirms it: a dashed line appears at the matching gap, labelled with the metres, and the
road lands exactly on that spacing.

What to report: whether the number in the label reads as useful or as clutter, whether the
guide competes badly with `Parallel` (they should BOTH show - one angular, one perpendicular),
and whether Offset is worth defaulting on.

---

## Unresolved questions

1. **Offset proposes one candidate per neighbour.** On an apron with five parallel taxiways
   that is four candidates, all perpendicular, competing for one slot. The nearest wins, which
   is probably right, but PIE on a busy field is what would show it.
2. **The gap is measured at the reference's closest point** to the drag. For two roads that are
   parallel but offset along their length, the gap is the same anywhere, so this is safe - but
   it would not be for near-parallel roads, which the 1e-3 cross-product test excludes.
3. **Offset defaults off.** With every stage done, the defaults (Extending, PointAlign,
   Parallel, World) deserve one deliberate PIE pass as a set rather than one source at a time.
