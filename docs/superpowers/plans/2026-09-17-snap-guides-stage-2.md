# Snap guides, stage 2: the network sources

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The four guide sources that must query the road network - Parallel, Collinear,
Runway and Aligned - so a corner can square itself to a taxiway, a runway or a placed stand,
not just to its own gesture.

**Architecture:** Four more `IGuideSource` implementations behind the interface stage 1 built.
Nothing about the arbiter, the context, the drawing or the plot tool changes: the chain gains
links, which is the whole reason it is a chain. Three propose `EFit::Angular` candidates
through the drag origin; Collinear proposes `EFit::Perpendicular`, because "in line with that
taxiway" is a statement about a line the cursor is ON, not a direction it set off in.

**Tech Stack:** UE 5.8.2 C++, Airside plugin, UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-17-snap-guides-design.md` - stage 2 of §8, i.e. §3's
rows Aligned, Collinear, Parallel and Runway.

**Stacked on:** branch `feature/snap-guides` (PR #146), which is NOT yet merged. This branch
is `feature/snap-guides-network` and its PR targets `feature/snap-guides`, not `main`.

## Global Constraints

- **Worktree.** Every command runs from `C:\repos\AirportMgr2_snap-guides-net`. Do not `cd` to
  `C:\repos\AirportMgr2` (unrelated uncommitted work) or to `C:\repos\AirportMgr2_snap-guides`
  (kept free for PR #146 review feedback).
- **Build.** A worktree build, so `-NoHotReloadFromIDE` is correct:

  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2_snap-guides-net\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
  ```

  Check the literal line `Result: Succeeded`. It exits 0 even when it fails - a build that
  collides with a running test editor reports `Result: Failed (OtherCompilationError)` and
  exit code 0, which happened twice in stage 1. Never read the exit code.
- **Tests.**

  ```
  ./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides-net\AirportMgr.uproject"
  ```

  Read the `N test(s) run, N failed, N crashed` line. **Baseline is 365** - record the actual
  number from a run before Task 1 and measure every later claim against it.
- **A new `.cpp` sometimes needs two builds** before it compiles; the first says
  `Result: Succeeded` without having compiled it. If a new test does not appear in the run
  count, build again before debugging.
- **`Solve/` includes `CoreMinimal.h` and `Solve/` only.** `Tool/` may include `Solve/` and
  `Model/`; `Tool/` may never include `Present/`. `Check-Architecture.ps1` enforces this and
  runs first inside the test script.
- **Never `git checkout --` a file whose task is not yet committed.** It reverts to HEAD, not
  to the last good state, and in stage 1 that silently discarded a whole task's work. To undo
  a deliberate sabotage, re-apply the edit by hand or commit the task first.
- **No string replacement without asserting it matched exactly once.**
- **Comments explain WHY**, especially why an obvious alternative was rejected.
- **Commits:** no `Co-Authored-By` trailer.

## What stage 1 already provides (do not rebuild it)

```cpp
// Solve/GuideArbiter.h
namespace SnapGuide
{
    enum class ESource : uint8 { Extending, PointAlign, Aligned, Collinear, Parallel, Runway, World, Offset };
    enum class EFit    : uint8 { Angular, Perpendicular };

    struct FCandidate
    {
        FVector2D Direction   = FVector2D(1.0, 0.0);  // unit; with Through, the LINE
        FVector2D Through     = FVector2D::ZeroVector; // the point the line passes through
        EFit      Fit         = EFit::Angular;
        double    Distance    = 0.0;                   // Offset only, stage 4
        FVector2D ReferenceAt = FVector2D::ZeroVector; // where the dashed line is drawn TO
        FString   Description;
        ESource   Source      = ESource::World;
    };

    struct FTuning
    {
        double ToleranceDegrees  = 7.0;
        double StickinessDegrees = 2.0;
        double ToleranceUu       = 300.0;
        double StickinessUu      = 100.0;
        double MaxPullUu         = 1000.0;
    };
}

// Tool/SnapGuideChain.h
struct FGuidePoint { FVector2D At; FString Name; };
struct FGuideAnchor
{
    FVector2D Origin, Reference, ReferenceAt;
    FString ReferenceName;
    TArray<FGuidePoint> AlignTo;
};
struct AIRSIDE_API IGuideSource
{
    virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
        TArray<SnapGuide::FCandidate>& Out) const = 0;
};
class AIRSIDE_API FSnapGuideChain
{
public:
    FSnapGuideChain();                               // installs the sources, in §3 order
    void AddSource(TUniquePtr<IGuideSource> Source);
    int32 NumSources() const;
    SnapGuide::FResult Resolve(const URoadNetwork&, const FGuideAnchor&, const FVector2D& Cursor,
        const SnapGuide::FResult& Previous, const SnapGuide::FTuning& = SnapGuide::FTuning()) const;
};
```

**At most one winner per fit kind**, so adding three angular sources does NOT put three more
lines on screen: they compete for the one angular slot, and the perpendicular slot is
contested by Collinear and stage 1's PointAlign. The screen still shows at most two guides.
That is the answer to §7's worry that "a player meeting seven live guides at once learns
nothing" - it was written before the one-per-kind rule existed.

## Model API these sources call (verified against the headers on this branch)

```cpp
const TArray<FRoadSegment>& URoadNetwork::GetSegments() const;   // walk by index
FRoadSegmentId              URoadNetwork::SegmentIdAt(int32) const;
const FRoadSegment*         URoadNetwork::GetSegment(FRoadSegmentId) const;
const FRoadNode*            URoadNetwork::GetNode(FRoadNodeId) const;
bool                        URoadNetwork::IsRunwaySegment(FRoadSegmentId) const;
const TArray<FEntityInstance>& URoadNetwork::GetEntities() const;

// FRoadSegment: bAlive, A, B (FRoadNodeId), Profile (URoadProfile*)
// FRoadNode:    Position (FVector2D)
// FEntityInstance: bAlive, Position (FVector2D), Heading (radians), Definition (UEntityDefinition*)

double    RoadGeom::ClosestPointOnSegment(const FVector2D& A, const FVector2D& B, const FVector2D& P); // returns T
FVector2D RoadGeom::PerpCCW(const FVector2D& V);
FString   RunwayDesignator::ToPairText(const FVector2D& Direction);   // "09/27", low end first
// A segment admits trucks when any Profile->Guidelines[i].Class == ETraversalClass::GroundVehicle
```

**`FEntityInstance` carries NO player-facing name** - pose, `Definition`, anchors, `bAlive`,
and nothing else. The only place in the codebase that names an entity is a log line using
`Definition->GetName()` (the asset name). Task 4 addresses this and is the one task a reviewer
may want to reject on its own.

## Decisions this plan takes

1. **Three angular, one perpendicular.** Parallel, Runway and Aligned answer "which way from
   here" and are judged in degrees from the drag origin. Collinear answers "you are ON the
   line that taxiway already lies on" and is judged in uu from that line - measuring it as an
   angle from an origin that is nowhere on it would answer a different question, which is the
   same reasoning that gave PointAlign its fit kind.
2. **A search reach, except for runways.** `FTuning::SearchRadiusUu` (10000 uu, 100 m) bounds
   Parallel, Collinear and Aligned: without it every road on the field proposes and the
   nearest-wins race is decided by geometry the player cannot see. Runway is DELIBERATELY
   unbounded - §3 says "every runway's heading", because an airport squares to its runways
   from anywhere on it, and there are at most a handful.
3. **`ReferenceAt` is a point ON the thing**, so the dashed line lands on it: the closest point
   on that segment for Parallel, Collinear and Runway; the entity's own `Position` for Aligned.
4. **Roads are named by what they admit**, not by an asset name: a runway segment is "runway
   09/27", a segment with a GroundVehicle guideline is "the service road", anything else is
   "the taxiway". That classification already exists twice in the codebase (`IsRunwaySegment`,
   and `FPlotPlaceTool`'s `IsServiceRoad`); this plan gives it ONE home in `Tool/` and has the
   new sources call it.
5. **No toggles.** Stage 3 owns those. All four sources are live, which is exactly what makes
   this stage worth shipping on its own.

## File Structure

| File | Responsibility |
|---|---|
| `Public/Tool/SnapGuideChain.h` | +4 source declarations; `FTuning` gains nothing (see below) |
| `Public/Solve/GuideArbiter.h` | `FTuning::SearchRadiusUu` |
| `Private/Tool/SnapGuideChain.cpp` | the four `Propose` bodies and one shared road-naming helper |
| `Public/Tool/RoadNaming.h` (new) | `RoadNaming::Describe(Network, SegmentId)` - the one home for decision 4 |
| `Private/Tool/RoadNaming.cpp` (new) | its body |
| `Public/Entities/EntityDefinition.h` | Task 4 only: `DisplayName` |
| `Private/Tool/PlotPlaceTool.cpp` | Task 4 only, if the depot definition needs a name set |
| `Private/AirsideTests/NetworkGuideSourceTest.cpp` (new) | one test per source, over a real network |

---

### Task 1: Parallel, and the search reach

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h` (`FTuning`)
- Create: `Plugins/Airside/Source/Airside/Public/Tool/RoadNaming.h`
- Create: `Plugins/Airside/Source/Airside/Private/Tool/RoadNaming.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/NetworkGuideSourceTest.cpp` (new)

**Interfaces:**
- Consumes: `IGuideSource`, `SnapGuide::FCandidate`, `FGuideAnchor` (stage 1, above).
- Produces: `double SnapGuide::FTuning::SearchRadiusUu = 10000.0`;
  `FString RoadNaming::Describe(const URoadNetwork& Network, FRoadSegmentId Segment)`;
  `struct FParallelGuideSource final : public IGuideSource`.

- [ ] **Step 1: Record the baseline**

Run the full test line from Global Constraints. Write the `N test(s) run` number down; every
later step in this plan measures against it. Expected: `365 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 2: Add the search reach**

In `Public/Solve/GuideArbiter.h`, in `FTuning`, after `MaxPullUu`:

```cpp

	/**
	 * How far from the drag a network source will look for something to line up with. uu -
	 * 100 m.
	 *
	 * WITHOUT A REACH, every road on the field proposes and the nearest-wins race is decided
	 * by geometry the player cannot see - a taxiway half a kilometre away winning because it
	 * happened to be a degree closer. Runways are deliberately exempt (see FRunwayGuideSource):
	 * an airport squares to its runways from anywhere on it.
	 *
	 * NO PIE PASS YET.
	 */
	double SearchRadiusUu = 10000.0;
```

- [ ] **Step 3: Write the road namer**

Create `Public/Tool/RoadNaming.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

class URoadNetwork;

/**
 * What to CALL a road in something the player reads.
 *
 * ONE HOME for a classification the codebase had twice: URoadNetwork::IsRunwaySegment, and
 * FPlotPlaceTool's file-local IsServiceRoad, which asks the profile's guidelines whether
 * anything admits a ground vehicle. A guide label saying "parallel to the taxiway" over a
 * service road is the kind of wrong that survives review, because both readers assume the
 * other's definition.
 *
 * NAMED BY WHAT IT ADMITS, not by an asset name: a URoadProfile has no display name at all
 * (only a MaterialSlot FName), and the thing the player is lining up with is a taxiway or a
 * service road regardless of which cross-section asset drew it.
 */
namespace RoadNaming
{
	/**
	 * "runway 09/27", "the service road", "the taxiway". Empty for a segment that is not live.
	 *
	 * The runway form carries BOTH ends, low first, because that is how a runway is spoken of
	 * and because it stays stable when an edit reverses the segment's stored direction - see
	 * RunwayDesignator::ToPairText's own comment.
	 */
	AIRSIDE_API FString Describe(const URoadNetwork& Network, FRoadSegmentId Segment);
}
```

Create `Private/Tool/RoadNaming.cpp`:

```cpp
#include "Tool/RoadNaming.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/RoadTraffic.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RunwayDesignator.h"

FString RoadNaming::Describe(const URoadNetwork& Network, FRoadSegmentId Segment)
{
	const FRoadSegment* Road = Network.GetSegment(Segment);
	if (Road == nullptr || !Road->bAlive)
	{
		return FString();
	}

	// THE RUNWAY QUESTION FIRST, because a runway's cross-section may well admit a vehicle and
	// the service-road test below would then claim it.
	if (Network.IsRunwaySegment(Segment))
	{
		const FRoadNode* A = Network.GetNode(Road->A);
		const FRoadNode* B = Network.GetNode(Road->B);
		if (A != nullptr && B != nullptr)
		{
			const FString Pair = RunwayDesignator::ToPairText(B->Position - A->Position);
			if (!Pair.IsEmpty())
			{
				return FString::Printf(TEXT("runway %s"), *Pair);
			}
		}
		return TEXT("the runway");
	}

	// ASKED OF THE GUIDELINES THE PROFILE DECLARES, which is what FPlotPlaceTool's own
	// IsServiceRoad asks and for the reason its comment gives: the segment carries no
	// ERoadKind, only a profile, and "a truck may drive here" is exactly what a GroundVehicle
	// guideline means.
	if (Road->Profile != nullptr)
	{
		for (const FProfileGuideline& Guideline : Road->Profile->Guidelines)
		{
			if (Guideline.Class == ETraversalClass::GroundVehicle)
			{
				return TEXT("the service road");
			}
		}
	}

	return TEXT("the taxiway");
}
```

- [ ] **Step 4: Declare the source**

In `Public/Tool/SnapGuideChain.h`, after `FPointAlignGuideSource`:

```cpp

/**
 * Source 5: the nearest road's direction, and its perpendicular.
 *
 * THE NEAREST ONE ONLY. Every road proposing would put the whole field in the race, and the
 * winner would be decided by a road the player cannot see - see FTuning::SearchRadiusUu.
 *
 * Angular, through the drag's own origin: this answers "which way from here", the same
 * question Extending answers, and it loses to Extending on a tie because the edge you are
 * extending is what you are thinking about.
 */
struct AIRSIDE_API FParallelGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const override;
};
```

- [ ] **Step 5: Implement it**

In `Private/Tool/SnapGuideChain.cpp`, add the includes at the top (after the existing
`#include "Solve/RoadGeom.h"`):

```cpp
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Tool/RoadNaming.h"
```

Then, in the anonymous namespace at the top of the file (create one if the file has none,
directly below the includes):

```cpp
namespace
{
	/** A live segment's two ends. False when either node has gone. */
	bool SegmentEnds(const URoadNetwork& Network, FRoadSegmentId Id,
		FVector2D& OutA, FVector2D& OutB)
	{
		const FRoadSegment* Segment = Network.GetSegment(Id);
		if (Segment == nullptr || !Segment->bAlive)
		{
			return false;
		}
		const FRoadNode* A = Network.GetNode(Segment->A);
		const FRoadNode* B = Network.GetNode(Segment->B);
		if (A == nullptr || B == nullptr)
		{
			return false;
		}
		OutA = A->Position;
		OutB = B->Position;
		return true;
	}

	/** Where on a segment the given point falls, and how far off it is. */
	FVector2D ClosestOn(const FVector2D& A, const FVector2D& B, const FVector2D& P)
	{
		return FMath::Lerp(A, B, RoadGeom::ClosestPointOnSegment(A, B, P));
	}
}
```

And the body:

```cpp
void FParallelGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	TArray<SnapGuide::FCandidate>& Out) const
{
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;

	FRoadSegmentId Nearest;
	FVector2D NearestAt = FVector2D::ZeroVector;
	FVector2D NearestDir = FVector2D::ZeroVector;
	double BestSquared = Reach * Reach;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);
		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!SegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D On = ClosestOn(A, B, Anchor.Origin);
		const double Squared = FVector2D::DistSquared(On, Anchor.Origin);
		if (Squared > BestSquared)
		{
			continue;
		}

		const FVector2D Span = B - A;
		if (Span.IsNearlyZero())
		{
			continue;
		}

		BestSquared = Squared;
		Nearest = Id;
		NearestAt = On;
		NearestDir = Span.GetSafeNormal();
	}

	if (NearestDir.IsNearlyZero())
	{
		return;
	}

	// MEASURED FROM THE ORIGIN, not from the cursor: the road the gesture STARTED beside is
	// the one it is being drawn parallel to, and a search keyed to the cursor would hand the
	// guide to a different road halfway through the drag.
	SnapGuide::FCandidate Along;
	Along.Direction = NearestDir;
	Along.Through = Anchor.Origin;
	Along.Fit = SnapGuide::EFit::Angular;
	Along.ReferenceAt = NearestAt;
	Along.Source = SnapGuide::ESource::Parallel;
	Along.Description = FString::Printf(TEXT("parallel to %s"),
		*RoadNaming::Describe(Network, Nearest));
	Out.Add(Along);

	SnapGuide::FCandidate Square = Along;
	Square.Direction = RoadGeom::PerpCCW(NearestDir);
	Square.Description = FString::Printf(TEXT("square to %s"),
		*RoadNaming::Describe(Network, Nearest));
	Out.Add(Square);
}
```

And install it in the constructor, in §3's order (Extending, PointAlign, Parallel, World):

```cpp
FSnapGuideChain::FSnapGuideChain()
{
	AddSource(MakeUnique<FExtendingGuideSource>());
	AddSource(MakeUnique<FPointAlignGuideSource>());
	AddSource(MakeUnique<FParallelGuideSource>());
	AddSource(MakeUnique<FWorldGuideSource>());
}
```

- [ ] **Step 6: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/NetworkGuideSourceTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/RoadNaming.h"
#include "Tool/SnapGuideChain.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A road of the given kind between two points, through the facade so it gets a profile. */
	void Lay(ARoadNetworkActor* Actor, const FVector2D& From, const FVector2D& To, ERoadKind Kind)
	{
		IRoadEditTarget* Target = Actor;
		const int32 A = Target->PlaceNode(From);
		const int32 B = Target->PlaceNode(To);
		Target->ConnectNodes(A, B, Kind, INDEX_NONE);
	}

	/** An anchor with no reference and no points, so ONLY the network sources answer. */
	FGuideAnchor BareAnchor(const FVector2D& Origin)
	{
		FGuideAnchor Anchor;
		Anchor.Origin = Origin;
		return Anchor;
	}

	/** Every candidate one source proposes, with the rest of the chain kept out of it. */
	TArray<SnapGuide::FCandidate> ProposedBy(const IGuideSource& Source,
		const URoadNetwork& Network, const FGuideAnchor& Anchor)
	{
		TArray<SnapGuide::FCandidate> Out;
		Source.Propose(Network, Anchor, Out);
		return Out;
	}
}

/**
 * A LAID TAXIWAY PRODUCES A PARALLEL CANDIDATE with its direction and a ReferenceAt ON it -
 * spec §9's Airside.Tool.GuideChainProposesFromTheNetwork, which stage 1 could not write
 * because no source read the network.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FParallelGuideFollowsTheNearestRoadTest,
	"Airside.Tool.ParallelGuideFollowsTheNearestRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FParallelGuideFollowsTheNearestRoadTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// An east-west taxiway through the origin, and a north-south one far to the east.
	Lay(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0), ERoadKind::Taxiway);
	Lay(Actor, FVector2D(50000.0, -10000.0), FVector2D(50000.0, 10000.0), ERoadKind::Taxiway);

	const FParallelGuideSource Source;
	const FGuideAnchor Anchor = BareAnchor(FVector2D(0.0, 2000.0));
	const TArray<SnapGuide::FCandidate> Candidates =
		ProposedBy(Source, *Actor->Network, Anchor);

	if (!TestEqual(TEXT("the nearest road proposes its direction and its perpendicular"),
		Candidates.Num(), 2))
	{
		return false;
	}

	// THE NEAR ROAD, NOT THE FAR ONE. Both are taxiways; only the reach tells them apart, and
	// without it the far one would be in the race on equal terms.
	TestTrue(TEXT("the candidate runs along the near road"),
		FMath::IsNearlyZero(Candidates[0].Direction.Y, 1.0e-6));
	TestTrue(TEXT("and the dashed line points at a spot ON that road"),
		FMath::IsNearlyZero(Candidates[0].ReferenceAt.Y, 1.0e-6));
	TestTrue(TEXT("beneath the drag, not at the road's far end"),
		FMath::IsNearlyZero(Candidates[0].ReferenceAt.X, 1.0e-6));

	TestEqual(TEXT("angular, because it answers which way from here"),
		static_cast<int32>(Candidates[0].Fit), static_cast<int32>(SnapGuide::EFit::Angular));
	TestTrue(TEXT("through the drag's own origin"),
		Candidates[0].Through.Equals(Anchor.Origin, 1.0e-6));
	TestEqual(TEXT("and named by what the road admits"),
		Candidates[0].Description, FString(TEXT("parallel to the taxiway")));
	TestEqual(TEXT("with the perpendicular named too"),
		Candidates[1].Description, FString(TEXT("square to the taxiway")));

	// CONTROL LEG: the reach is real. Drag beyond it and the near road stops answering, so
	// the assertions above are measuring the search and not merely the first segment laid.
	const TArray<SnapGuide::FCandidate> FarAway =
		ProposedBy(Source, *Actor->Network, BareAnchor(FVector2D(0.0, 30000.0)));
	TestEqual(TEXT("a drag beyond the search reach gets nothing from this source"),
		FarAway.Num(), 0);

	return true;
}

/**
 * A SERVICE ROAD IS NOT A TAXIWAY, and a label that called it one would be the kind of wrong
 * that survives review because both readers assume the other's definition.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNamingSaysWhatARoadAdmitsTest,
	"Airside.Tool.RoadNamingSaysWhatARoadAdmits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNamingSaysWhatARoadAdmitsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	Lay(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0), ERoadKind::Taxiway);
	Lay(Actor, FVector2D(-10000.0, 5000.0), FVector2D(10000.0, 5000.0), ERoadKind::ServiceRoad);

	TestEqual(TEXT("a taxiway is called one"),
		RoadNaming::Describe(*Actor->Network, Actor->Network->SegmentIdAt(0)),
		FString(TEXT("the taxiway")));
	TestEqual(TEXT("and a service road is not called a taxiway"),
		RoadNaming::Describe(*Actor->Network, Actor->Network->SegmentIdAt(1)),
		FString(TEXT("the service road")));

	return true;
}

#endif
```

- [ ] **Step 7: Build**

Run the build line. Expect `Result: Succeeded`. New `.cpp` files - if the tests do not appear
in step 8, build a second time.

- [ ] **Step 8: Run the new tests**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides-net\AirportMgr.uproject" -Filter Airside.Tool.ParallelGuide
```

Then the naming one:

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides-net\AirportMgr.uproject" -Filter Airside.Tool.RoadNaming
```

Expected: `1 test(s) run, 0 failed, 0 crashed` each.

- [ ] **Step 9: Run the whole suite**

Expected: baseline + 2, 0 failed, 0 crashed. **Watch `Airside.Tool.GuideChain*` and
`Airside.Tool.Plot*` in particular**: those run over networks that now have roads in them, and
a Parallel candidate they did not expect could take the angular slot from Extending. If one
fails, read it before changing it - the fix may be the test's geometry, or it may be that
Parallel is out-competing Extending somewhere it should not.

- [ ] **Step 10: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Solve/GuideArbiter.h Plugins/Airside/Source/Airside/Public/Tool/RoadNaming.h Plugins/Airside/Source/Airside/Private/Tool/RoadNaming.cpp Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp Plugins/Airside/Source/AirsideTests/Private/NetworkGuideSourceTest.cpp
git commit -m "feat(tool): a corner can square itself to the nearest road"
```

---

### Task 2: Collinear

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/NetworkGuideSourceTest.cpp`

**Interfaces:**
- Consumes: `RoadNaming::Describe`, `SegmentEnds`, `ClosestOn` (Task 1).
- Produces: `struct FCollinearGuideSource final : public IGuideSource`.

- [ ] **Step 1: Declare it**

In `Public/Tool/SnapGuideChain.h`, after `FParallelGuideSource`:

```cpp

/**
 * Source 4: the line an existing segment already lies on.
 *
 * PERPENDICULAR, not angular, and that is the whole difference from Parallel above. Parallel
 * says "point the same way as that taxiway"; this says "you are ON the line that taxiway lies
 * along", which is a statement about where the cursor ENDED UP. Measuring it as an angle from
 * an origin that is nowhere on the line would answer a different question - the same
 * reasoning that gave PointAlign its fit kind.
 *
 * ONE CANDIDATE PER SEGMENT IN REACH, not just the nearest: a cursor can be on the extension
 * of a distant segment while standing beside a nearer one, and that is exactly the case worth
 * telling the player about.
 */
struct AIRSIDE_API FCollinearGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const override;
};
```

- [ ] **Step 2: Implement it**

In `Private/Tool/SnapGuideChain.cpp`:

```cpp
void FCollinearGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	TArray<SnapGuide::FCandidate>& Out) const
{
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);
		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!SegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		if (Span.IsNearlyZero()
			|| FVector2D::DistSquared(ClosestOn(A, B, Anchor.Origin), Anchor.Origin) > Reach * Reach)
		{
			continue;
		}

		// THROUGH THE SEGMENT'S OWN END, which is what makes this the line the road LIES ON
		// rather than one through the drag. The arbiter measures the cursor's distance from
		// that line, so the candidate is eligible exactly when the cursor is on the road's
		// extension - however far along it that happens to be.
		SnapGuide::FCandidate InLine;
		InLine.Direction = Span.GetSafeNormal();
		InLine.Through = A;
		InLine.Fit = SnapGuide::EFit::Perpendicular;
		InLine.Source = SnapGuide::ESource::Collinear;
		InLine.Description = FString::Printf(TEXT("in line with %s"),
			*RoadNaming::Describe(Network, Id));

		// THE DASHED LINE GOES TO THE ROAD ITSELF, not to the point on its extension where
		// the cursor is: the player needs to see WHICH road they are in line with, and the
		// near end of it is the part they can recognise.
		InLine.ReferenceAt = ClosestOn(A, B, Anchor.Origin);
		Out.Add(InLine);
	}
}
```

Install it in the constructor, in §3's order (Extending, PointAlign, Collinear, Parallel,
World):

```cpp
FSnapGuideChain::FSnapGuideChain()
{
	AddSource(MakeUnique<FExtendingGuideSource>());
	AddSource(MakeUnique<FPointAlignGuideSource>());
	AddSource(MakeUnique<FCollinearGuideSource>());
	AddSource(MakeUnique<FParallelGuideSource>());
	AddSource(MakeUnique<FWorldGuideSource>());
}
```

- [ ] **Step 3: Write the failing test**

Append to `NetworkGuideSourceTest.cpp`, before the final `#endif`:

```cpp
/**
 * IN LINE WITH A ROAD IS NOT THE SAME AS PARALLEL TO IT. A cursor 3000 uu past the end of a
 * taxiway, dead on its centreline, is collinear with it and parallel to it; a cursor 3000 uu
 * to the SIDE is parallel and not collinear. The two sources must disagree there, or one of
 * them is redundant.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCollinearGuideIsNotParallelTest,
	"Airside.Tool.CollinearGuideIsNotParallel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCollinearGuideIsNotParallelTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// One east-west taxiway, from the origin eastwards.
	Lay(Actor, FVector2D(0.0, 0.0), FVector2D(6000.0, 0.0), ERoadKind::Taxiway);

	const FCollinearGuideSource Source;
	const TArray<SnapGuide::FCandidate> Candidates =
		ProposedBy(Source, *Actor->Network, BareAnchor(FVector2D(7000.0, 0.0)));

	if (!TestEqual(TEXT("the one road in reach proposes its own line"), Candidates.Num(), 1))
	{
		return false;
	}

	TestEqual(TEXT("perpendicular, because it is about where the cursor ended up"),
		static_cast<int32>(Candidates[0].Fit),
		static_cast<int32>(SnapGuide::EFit::Perpendicular));
	TestTrue(TEXT("the line passes through the road, not through the drag"),
		FMath::IsNearlyZero(Candidates[0].Through.Y, 1.0e-6));
	TestEqual(TEXT("named as being in line with it"),
		Candidates[0].Description, FString(TEXT("in line with the taxiway")));

	// THE LINE IS THE ROAD'S, so the arbiter finds the cursor ON it however far past the end
	// the drag has gone - which is the case Parallel cannot express.
	SnapGuide::FResult Result = SnapGuide::Arbitrate(
		Candidates, FVector2D(7000.0, 0.0), FVector2D(9000.0, 40.0), SnapGuide::FResult());
	TestTrue(TEXT("a cursor on the road's extension is offered the guide"), Result.bActive);
	TestTrue(TEXT("and is pulled exactly onto the centreline"),
		FMath::IsNearlyZero(Result.Point.Y, 1.0e-6));

	// CONTROL LEG: a cursor well to the SIDE of the road is not in line with it, however
	// parallel it may be. Without this the test would pass on a source that proposed a line
	// through the drag instead of through the road.
	SnapGuide::FResult Beside = SnapGuide::Arbitrate(
		Candidates, FVector2D(7000.0, 0.0), FVector2D(9000.0, 3000.0), SnapGuide::FResult());
	TestFalse(TEXT("a cursor 30 m to the side is not in line with anything"), Beside.bActive);

	return true;
}
```

Add `#include "Solve/GuideArbiter.h"` to the file's include block if it is not already there.

- [ ] **Step 4: Build**

Expect `Result: Succeeded`.

- [ ] **Step 5: Run the new test**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides-net\AirportMgr.uproject" -Filter Airside.Tool.CollinearGuide
```

Expected: `1 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 6: Run the whole suite**

Expected: baseline + 3, 0 failed, 0 crashed. Collinear contests the PERPENDICULAR slot with
stage 1's PointAlign, so watch `Airside.Tool.PlotLastCornerTakesTwoGuides`: if a road happens
to lie where that test drags, Collinear could take the slot from the corner alignment. Read
the failure before changing anything.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp Plugins/Airside/Source/AirsideTests/Private/NetworkGuideSourceTest.cpp
git commit -m "feat(tool): a corner can sit in line with a road, not just parallel to it"
```

---

### Task 3: Runway

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/NetworkGuideSourceTest.cpp`

**Interfaces:**
- Consumes: `RoadNaming::Describe`, `SegmentEnds`, `ClosestOn` (Task 1).
- Produces: `struct FRunwayGuideSource final : public IGuideSource`.

- [ ] **Step 1: Declare it**

In `Public/Tool/SnapGuideChain.h`, after `FParallelGuideSource`:

```cpp

/**
 * Source 6: every runway's heading, and its perpendicular.
 *
 * DELIBERATELY UNBOUNDED by SearchRadiusUu, unlike every other network source. An airport
 * squares to its runways from anywhere on it - that is what makes a field read as one place
 * rather than as a pile of unrelated pavement - and there are at most a handful of runways to
 * walk. §3 says "every runway's heading" and means it.
 *
 * Below the local sources and above the world axes, because an airport squares to its
 * runways but not in preference to the taxiway the player is actually working on.
 */
struct AIRSIDE_API FRunwayGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const override;
};
```

- [ ] **Step 2: Implement it**

```cpp
void FRunwayGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	TArray<SnapGuide::FCandidate>& Out) const
{
	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = Network.SegmentIdAt(Index);
		if (!Network.IsRunwaySegment(Id))
		{
			continue;
		}

		FVector2D A = FVector2D::ZeroVector;
		FVector2D B = FVector2D::ZeroVector;
		if (!SegmentEnds(Network, Id, A, B))
		{
			continue;
		}

		const FVector2D Span = B - A;
		if (Span.IsNearlyZero())
		{
			continue;
		}

		// NO REACH TEST, and that is the one line where this source differs from Parallel -
		// see the declaration for why.
		SnapGuide::FCandidate Along;
		Along.Direction = Span.GetSafeNormal();
		Along.Through = Anchor.Origin;
		Along.Fit = SnapGuide::EFit::Angular;
		Along.ReferenceAt = ClosestOn(A, B, Anchor.Origin);
		Along.Source = SnapGuide::ESource::Runway;
		Along.Description = FString::Printf(TEXT("parallel to %s"),
			*RoadNaming::Describe(Network, Id));
		Out.Add(Along);

		SnapGuide::FCandidate Square = Along;
		Square.Direction = RoadGeom::PerpCCW(Along.Direction);
		Square.Description = FString::Printf(TEXT("square to %s"),
			*RoadNaming::Describe(Network, Id));
		Out.Add(Square);
	}
}
```

Install it in the constructor, in §3's order (Extending, PointAlign, Collinear, Parallel,
Runway, World):

```cpp
FSnapGuideChain::FSnapGuideChain()
{
	AddSource(MakeUnique<FExtendingGuideSource>());
	AddSource(MakeUnique<FPointAlignGuideSource>());
	AddSource(MakeUnique<FCollinearGuideSource>());
	AddSource(MakeUnique<FParallelGuideSource>());
	AddSource(MakeUnique<FRunwayGuideSource>());
	AddSource(MakeUnique<FWorldGuideSource>());
}
```

- [ ] **Step 3: Write the failing test**

**`ERoadKind` has only `Taxiway` and `ServiceRoad` - there is no runway kind.** A runway is
placed through its own facade call with its own profile. Add this helper to
`NetworkGuideSourceTest.cpp`'s anonymous namespace first:

```cpp
	/**
	 * A runway strip. NOT ConnectNodes: ERoadKind has no runway value, because a runway is not
	 * a road kind - it is a segment placed through PlaceRunway with a runway profile and
	 * facts, which is what URoadNetwork::IsRunwaySegment then recognises.
	 *
	 * MinimumRunwayLength is dropped first: it defaults to 50000 uu and PlaceRunway refuses
	 * anything under it, so a test strip has to lower the bar or place a realistic 500 m one.
	 * MeshFreshnessTest does exactly this and for the same reason.
	 */
	void LayRunway(ARoadNetworkActor* Actor, const FVector2D& From, const FVector2D& To)
	{
		URoadProfile* Profile = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Profile->bContinuousThroughJunctions = true;
		Actor->MinimumRunwayLength = 100.0;
		Actor->PlaceRunway(From, To, Profile);
	}
```

Add `#include "Profiles/RoadProfile.h"` to the file's includes for `URoadProfile`.

Then append the test itself, before the final `#endif`:

```cpp
/**
 * A RUNWAY IS OFFERED FROM ANYWHERE ON THE FIELD, which is the one way this source differs
 * from Parallel - and the difference is the point of it: an airport squares to its runways.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayGuideReachesTheWholeFieldTest,
	"Airside.Tool.RunwayGuideReachesTheWholeField",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayGuideReachesTheWholeFieldTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A runway along +X through the origin. NORTH IS +X in this project (see
	// RunwayDesignator's own header comment), so this strip is 18/36 and NOT 09/27 - the
	// first draft of this test asserted 09/27 and would have failed on a correct source.
	LayRunway(Actor, FVector2D(-40000.0, 0.0), FVector2D(40000.0, 0.0));

	const FRunwayGuideSource Source;

	// FAR BEYOND SearchRadiusUu - 500 m out, where Parallel would have given up.
	const TArray<SnapGuide::FCandidate> Candidates =
		ProposedBy(Source, *Actor->Network, BareAnchor(FVector2D(0.0, 50000.0)));

	if (!TestEqual(TEXT("the runway proposes its heading and its perpendicular"),
		Candidates.Num(), 2))
	{
		return false;
	}

	TestTrue(TEXT("offered from right across the field, unlike every other network source"),
		FMath::IsNearlyZero(Candidates[0].Direction.Y, 1.0e-6));
	TestEqual(TEXT("angular, through the drag's own origin"),
		static_cast<int32>(Candidates[0].Fit), static_cast<int32>(SnapGuide::EFit::Angular));

	// NAMED AS A RUNWAY IS SPOKEN OF, both ends, low first - a label saying "the taxiway" over
	// a runway would be the classification quietly disagreeing with itself.
	TestTrue(TEXT("named by its designators, low end first"),
		Candidates[0].Description.Contains(TEXT("runway 18/36")));

	// CONTROL LEG: the source is selective. A taxiway laid beside it must NOT be offered here,
	// or this test would pass on a source that proposed every segment.
	Lay(Actor, FVector2D(-10000.0, 20000.0), FVector2D(10000.0, 20000.0), ERoadKind::Taxiway);
	const TArray<SnapGuide::FCandidate> Again =
		ProposedBy(Source, *Actor->Network, BareAnchor(FVector2D(0.0, 50000.0)));
	TestEqual(TEXT("and a taxiway is not mistaken for a runway"), Again.Num(), 2);

	return true;
}
```

Before writing it, add whatever `LayRunway` helper the existing runway tests use, into this
file's anonymous namespace, copied from `RunwayToolTest.cpp` rather than re-derived.

- [ ] **Step 4: Build**

Expect `Result: Succeeded`.

- [ ] **Step 5: Run the new test**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides-net\AirportMgr.uproject" -Filter Airside.Tool.RunwayGuide
```

Expected: `1 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 6: Run the whole suite**

Expected: baseline + 4, 0 failed, 0 crashed.

- [ ] **Step 7: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp Plugins/Airside/Source/AirsideTests/Private/NetworkGuideSourceTest.cpp
git commit -m "feat(tool): the field squares to its runways, from anywhere on it"
```

---

### Task 4: Aligned, and a name for a placed entity

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/NetworkGuideSourceTest.cpp`

**Interfaces:**
- Consumes: `SegmentEnds` (Task 1, unused here), `FGuideAnchor`.
- Produces: `FText UEntityDefinition::DisplayName`; `FString EntityNaming::Describe(const
  FEntityInstance&)`; `struct FAlignedGuideSource final : public IGuideSource`.

**THIS IS THE TASK TO REJECT ON ITS OWN if the `DisplayName` addition is unwanted.** A placed
entity carries no player-facing name anywhere in the codebase - `FEntityInstance` is pose,
`Definition`, anchors and `bAlive`, and the only naming that exists is a log line using the
asset name. A guide label is the first thing that needs one. The alternative is to drop the
Aligned source from this stage; it is the least valuable of the four, because a stand's pose
direction is usually the taxiway's direction anyway and Parallel already offers that.

- [ ] **Step 1: Give a definition a name**

In `Public/Entities/EntityDefinition.h`, inside `UEntityDefinition`'s public section:

```cpp
	/**
	 * What to call one of these where the player reads it - "the stand", "the fuel depot".
	 *
	 * ADDED FOR THE GUIDE LABELS, which are the first player-facing text that has to name a
	 * placed entity: FEntityInstance carries a pose, a definition and anchors, and the only
	 * naming anywhere in the codebase before this was a log line printing the asset's name.
	 *
	 * FALLS BACK TO THE ASSET NAME when unset (see EntityNaming::Describe), so every existing
	 * .uasset keeps working without being re-authored - an empty FText here is a content task,
	 * not a bug.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Presentation")
	FText DisplayName;
```

- [ ] **Step 2: Write the namer and the source's declaration**

In `Public/Tool/SnapGuideChain.h`, after `FRunwayGuideSource`:

```cpp

/** What to call a placed entity in something the player reads. Falls back to the asset name. */
namespace EntityNaming
{
	AIRSIDE_API FString Describe(const struct FEntityInstance& Entity);
}

/**
 * Source 3: a placed entity's pose direction, and its perpendicular.
 *
 * Bounded by SearchRadiusUu like the other local sources. Angular, through the drag's origin:
 * "point the way that stand points" is a direction, not a line the cursor is on.
 *
 * THE WEAKEST OF THE FOUR, and worth knowing why it is still here: a stand's pose is usually
 * square to the taxiway it serves, so Parallel already offers the same direction most of the
 * time. It earns its place on the apron, where a row of stands sets the local grain and the
 * nearest road is a long way off.
 */
struct AIRSIDE_API FAlignedGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const override;
};
```

- [ ] **Step 3: Implement both**

In `Private/Tool/SnapGuideChain.cpp`, add `#include "Entities/EntityDefinition.h"` and
`#include "Model/RoadEntity.h"` to the includes, then:

```cpp
FString EntityNaming::Describe(const FEntityInstance& Entity)
{
	if (Entity.Definition == nullptr)
	{
		return TEXT("the installation");
	}

	// THE AUTHORED NAME WHEN THERE IS ONE, the asset's own when there is not. An unset
	// DisplayName is a content task rather than a bug, so this must not read as one on screen.
	const FString Authored = Entity.Definition->DisplayName.ToString();
	return Authored.IsEmpty() ? Entity.Definition->GetName() : Authored;
}

void FAlignedGuideSource::Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
	TArray<SnapGuide::FCandidate>& Out) const
{
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;

	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		if (!Entity.bAlive
			|| FVector2D::DistSquared(Entity.Position, Anchor.Origin) > Reach * Reach)
		{
			continue;
		}

		// HEADING IS RADIANS - see FEntityInstance::Heading. A degrees/radians slip here would
		// point the guide somewhere plausible and wrong, which is the worst kind.
		const FVector2D Facing(FMath::Cos(Entity.Heading), FMath::Sin(Entity.Heading));

		SnapGuide::FCandidate Along;
		Along.Direction = Facing;
		Along.Through = Anchor.Origin;
		Along.Fit = SnapGuide::EFit::Angular;

		// THE DASHED LINE GOES TO THE THING ITSELF, which for an entity is simply its pose.
		Along.ReferenceAt = Entity.Position;
		Along.Source = SnapGuide::ESource::Aligned;
		Along.Description = FString::Printf(TEXT("aligned with %s"),
			*EntityNaming::Describe(Entity));
		Out.Add(Along);

		SnapGuide::FCandidate Square = Along;
		Square.Direction = RoadGeom::PerpCCW(Facing);
		Square.Description = FString::Printf(TEXT("square to %s"),
			*EntityNaming::Describe(Entity));
		Out.Add(Square);
	}
}
```

Install it in the constructor, completing §3's order (Extending, PointAlign, Aligned,
Collinear, Parallel, Runway, World):

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
}
```

- [ ] **Step 4: Write the failing test**

Append to `NetworkGuideSourceTest.cpp`, before the final `#endif`. A stand is placed through
`IRoadEditTarget::PlaceStand(FVector2D Where, double Heading)` - a non-virtual helper over
`PlaceEntity(Where, Heading, EPlaceableEntity::Stand)`, with the heading in RADIANS.

```cpp
/**
 * A STAND'S POSE SETS A DIRECTION, and the guide must take it from the pose rather than from
 * anything the stand happens to sit beside.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAlignedGuideTakesThePoseDirectionTest,
	"Airside.Tool.AlignedGuideTakesThePoseDirection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAlignedGuideTakesThePoseDirectionTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A stand at the origin facing 45 degrees - deliberately not a world axis, so a source
	// that quietly proposed an axis instead of the pose would be caught.
	Actor->PlaceStand(FVector2D(0.0, 0.0), FMath::DegreesToRadians(45.0));

	const FAlignedGuideSource Source;
	const TArray<SnapGuide::FCandidate> Candidates =
		ProposedBy(Source, *Actor->Network, BareAnchor(FVector2D(2000.0, 2000.0)));

	if (!TestEqual(TEXT("the stand proposes its facing and its perpendicular"),
		Candidates.Num(), 2))
	{
		return false;
	}

	const double Sin45 = FMath::Sin(FMath::DegreesToRadians(45.0));
	TestTrue(TEXT("the candidate points the way the stand faces, in radians not degrees"),
		FMath::IsNearlyEqual(Candidates[0].Direction.X, Sin45, 1.0e-6)
			&& FMath::IsNearlyEqual(Candidates[0].Direction.Y, Sin45, 1.0e-6));
	TestTrue(TEXT("and the dashed line points at the stand itself"),
		Candidates[0].ReferenceAt.Equals(FVector2D::ZeroVector, 1.0e-6));

	// CONTROL LEG: the reach applies here too, so this source cannot quietly become global.
	const TArray<SnapGuide::FCandidate> FarAway =
		ProposedBy(Source, *Actor->Network, BareAnchor(FVector2D(0.0, 40000.0)));
	TestEqual(TEXT("a stand beyond the search reach proposes nothing"), FarAway.Num(), 0);

	return true;
}
```

- [ ] **Step 5: Build**

Expect `Result: Succeeded`. A new `UPROPERTY` means UHT runs - this is not a Live Coding
change, and the editor must be closed for the checkout it has open (a worktree build with
`-NoHotReloadFromIDE` is still fine alongside another checkout's editor).

- [ ] **Step 6: Run the new test**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides-net\AirportMgr.uproject" -Filter Airside.Tool.AlignedGuide
```

Expected: `1 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 7: Run the whole suite**

Expected: baseline + 5, 0 failed, 0 crashed. Watch `Airside.Content.*` and
`Airside.Present.AuthoredPropertiesUntouched`: a new `UPROPERTY` on a `UDataAsset` can trip
tests that compare authored defaults.

- [ ] **Step 8: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp Plugins/Airside/Source/AirsideTests/Private/NetworkGuideSourceTest.cpp
git commit -m "feat(tool): a corner can align with a placed stand"
```

---

### Task 5: Prove the chain still picks sensibly, and update the spec

**Files:**
- Test: `Plugins/Airside/Source/AirsideTests/Private/NetworkGuideSourceTest.cpp`
- Modify: `docs/superpowers/specs/2026-09-17-snap-guides-design.md`

**Interfaces:**
- Consumes: all four sources.
- Produces: nothing new.

- [ ] **Step 1: Write the priority test**

Seven sources now feed one angular slot. The source order is the tiebreak and nothing else,
and nothing yet proves that end to end over a real network. Append to
`NetworkGuideSourceTest.cpp`:

```cpp
/**
 * SEVEN SOURCES, ONE ANGULAR SLOT. With a taxiway, a runway and the world grid all offering
 * the same direction, the tiebreak must go to the most specific - and "most specific" is the
 * §3 order, not the order the chain happens to ask in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainPrefersTheLocalOverTheGlobalTest,
	"Airside.Tool.GuideChainPrefersTheLocalOverTheGlobal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainPrefersTheLocalOverTheGlobalTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// A taxiway and a runway both running along +X, which is also a world axis: three sources
	// offering one direction, every one of them in tolerance at once.
	Lay(Actor, FVector2D(-10000.0, 0.0), FVector2D(10000.0, 0.0), ERoadKind::Taxiway);
	LayRunway(Actor, FVector2D(-40000.0, 8000.0), FVector2D(40000.0, 8000.0));

	const FSnapGuideChain Chain;
	const FGuideAnchor Anchor = BareAnchor(FVector2D(0.0, 2000.0));

	const SnapGuide::FResult Result = Chain.Resolve(
		*Actor->Network, Anchor, FVector2D(3000.0, 2100.0), SnapGuide::FResult());

	if (!TestTrue(TEXT("something answers"), Result.bActive)) { return false; }

	// PARALLEL BEATS RUNWAY BEATS WORLD. An airport squares to its runways, but not in
	// preference to the taxiway the player is actually working beside.
	TestEqual(TEXT("the nearest road wins over the runway and the world grid"),
		static_cast<int32>(Result.Winners[0].Source),
		static_cast<int32>(SnapGuide::ESource::Parallel));

	return true;
}
```

- [ ] **Step 2: Build and run it**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-guides-net\AirportMgr.uproject" -Filter Airside.Tool.GuideChainPrefersTheLocal
```

Expected: `1 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 3: Record the stage in the spec**

In `docs/superpowers/specs/2026-09-17-snap-guides-design.md`, in §8, mark stage 2 done and
note what it taught, in the same voice as the stage 1 amendment:

```markdown
2. ~~The network sources: Parallel, Collinear, Aligned, Runway.~~ **Done 2026-09-17.**
   Collinear is the only one of the four that is `EFit::Perpendicular` - "in line with that
   taxiway" is about where the cursor ended up, not which way it set off. Runway is
   deliberately exempt from `SearchRadiusUu`; the other three are bounded by it, because
   without a reach the nearest-wins race is decided by geometry off screen.
```

- [ ] **Step 4: Run the whole suite**

Expected: baseline + 6, 0 failed, 0 crashed.

- [ ] **Step 5: Commit**

```bash
git add Plugins/Airside/Source/AirsideTests/Private/NetworkGuideSourceTest.cpp docs/superpowers/specs/2026-09-17-snap-guides-design.md
git commit -m "test(tool): the local guide beats the global one, and the spec says stage 2 landed"
```

- [ ] **Step 6: Judge it in PIE**

Nothing above measures feel, and this stage adds six candidates to a race that had three.

1. Open the project, press `0` (Fuel depot), and draw a plot near a taxiway that is NOT
   parallel to the service road it fronts.
2. Drag a back corner slowly through the taxiway's direction.

What confirms it: the label changes from "square to the frontage" to "parallel to the taxiway"
as the nearer alignment takes over, and it does so ONCE rather than flickering. What to report
back: whether `SearchRadiusUu` at 100 m grabs roads the player is not thinking about, and
whether seven sources feeding one slot makes the guide feel indecisive - the answer to that
decides whether stage 3's toggles need to come next or can wait.

---

## Unresolved questions

1. **`UEntityDefinition::DisplayName` (Task 4)** is a content-facing addition made for a guide
   label. Reject Task 4 if that trade is wrong; the Aligned source is the least valuable of
   the four and the stage stands without it.
2. **`SearchRadiusUu` is one number for three sources** with different needs - Aligned wants
   the apron's grain (small), Collinear wants a road's extension (potentially long). Split it
   only if PIE says so; one knob is easier to tune than three until it is not.
3. **Stage 3's toggles may need to jump the queue.** §7 says defaults on are Extending,
   Parallel and World - which is exactly the set this stage makes competitive. If PIE says
   seven live sources is indecisive, do stage 3 before stage 4.
