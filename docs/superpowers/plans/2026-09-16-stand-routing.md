# The Stand Routes Like An Apron — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the stand's service ring — whose corners no 8.5 m dispenser can take — with a lane that comes inside the wingtip and runs along the row the equipment boxes are painted on, so four of five anchors sit ON a lane and nothing turns into one.

**Architecture:** One closed waypoint cycle per stand, computed by `BuildCodeCStand` at authoring time and placed into the guideline graph by `FStandLaneBuild` (today's `FServiceLoopBuild`, fed a different polyline). The corner-rounding, leg-clamping and quadratic-bend machinery already in that builder is kept verbatim; what changes is the polyline it rounds and the fact that some of its waypoints are anchors that already own a node. `FAnchorLink` stops discovering entrances and reads declared ones.

**Tech Stack:** UE 5.8.2 C++, `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, `Tools/Run-AirsideTests.ps1`, `Tools/Python/build_stand_asset.py`.

**Spec:** `docs/superpowers/specs/2026-09-16-stand-routing-design.md`

---

## Global Constraints

- **Engine:** UE 5.8.2 at `D:\Epic\UE_5.8`. Project at `C:\repos\AirportMgr2`, branch `feature/lane-entrances`.
- **The editor must be CLOSED to build.** Tasks 1–5 all add headers, `UPROPERTY`s or `USTRUCT`s, so none of them is a Live Coding change. Batch them: build once before Task 1's test run and once more at Task 6.
- **A new test `.cpp` needs TWO builds.** The first reports `Result: Succeeded` without compiling it, because UBT has not regenerated the module's source list. Build twice and confirm the test appears in the run count.
- **UHT cannot see a plain enum.** `EStandWaypointKind` must be `UENUM()` in a header that includes its own `.generated.h`, or the `USTRUCT` holding it will not compile. A forward declaration does not help.
- **Name every leaf test distinctly.** UE's automation tree drops a bare-named parent once a dotted child exists, and only the run count catches it. No new test name may be a prefix of another.
- **Never trust the runner's exit code.** Read `N test(s) run, N failed, N crashed` from `Tools/Run-AirsideTests.ps1`.
- **`Check-Architecture.ps1` runs first** and fails the run before the editor starts. Only `Model/` has a forbidden-include list (`Build/|Tool/|Present/|Entities/|Content/`); `Solve/` may include `CoreMinimal.h` and `Solve/` and nothing else; `Entities/` is unrestricted and may read `Content/`.
- **Comments explain WHY**, and especially why an obvious alternative was rejected. Record the measurement that settled a figure, not just the figure.
- **Tests restate arithmetic** rather than sharing a helper with production code — `FAirframe::TightestFollowableRadius`'s own comment gives the reason.
- Log categories: `LogAirside` in the plugin, `LogRoadBuild` in the game module.

### Baseline

354 tests run, **2 failed**, 0 crashed at `96424e6`. Both failures are this plan's business:
`Airside.Build.ServiceLaneEntersOnEverySideWithinReach` (Task 5 rewrites it) and
`Airside.Build.SpursLeaveTheLaneTangentially` (Task 4 replaces it).

### Figures this plan depends on

All derived from `UAirsideSettings::ResolveLargestServiceVehicle()`; none typed twice.

| Quantity | Value | From |
|---|---|---|
| `TightestFollowableRadius` | 699.4 uu | 494.538 / sin 45° |
| `CornerRunFor(R, θ)` | `R·cos(θ/2)/sin²(θ/2)` | inverse of `GuidelineGeom::TightestRadius` |
| Right-angle corner run | 989.1 uu | `CornerRunFor(R, 90°)` = 1.414 R |
| Hydrant dip half-extent | 1018.4 uu | flat bottom, legs 40.5°, `2·Run + 400/tan α` |
| `EquipmentFwd` derived X | −182 | −1200 + 1018.4 |
| `EquipmentAft` derived X | −2218 | −1200 − 1018.4 |
| Crossing diagonal | ≤ 73.55°, run 652.6 uu | 1807.2 uu of X beyond the outermost anchor |

### Corrections against the spec, ruled before execution

**1. A single CLOSED polyline, `TArray<FStandWaypoint> ServiceLane`**, not the spec's plural open
`ServiceLanes`. `FServiceLoopBuild::Build` already rounds an implicitly-closing polyline, clamps
corners that share a leg and lays quadratic bends; feeding it a longer closed polyline is a change
of input, not of algorithm. Decision 1 makes the network one cycle, so plural open lanes is
generality nothing uses.

**2. The four entries are the four corners of the two crossings**, not the spec's first-draft
port and starboard entries at y = ±2090. Those would have been STUBS hanging off the cycle, and
a stub is the dead end decision 1 forbids — the two corrections stand or fall together, which is
why they are ruled together. A stand sits in a row with neighbours abeam; the road runs fore or
aft. Spec amended to match (`5f9066b`+).

**3. `FuselageWidth` needs a consumer.** Spec test 4 — nothing crosses the fuselage *rectangle* —
had no task, which would have left Task 2 shipping a field nothing reads. It is folded into Task
4's placed-geometry test.

**4. Task 4 must delete `FAnchorLink`'s lane-link block, not just update it.** That block calls
`WalkRing` and `LaneTurnRadius` at `AnchorLink.cpp:418`, `:561` and `:570`, and Task 4 deletes
both. So Task 4 leaves stands **unlinked to roads**, and its suite run will show link tests
failing. That is a planned intermediate state, not a regression; Task 5 restores linking through
entries.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `Public/Solve/GuidelineGeom.h` / `Private/Solve/GuidelineGeom.cpp` | `CornerRunFor` — the corner cost, beside its inverse | 1 |
| `Public/Model/RoadEntity.h` | `FEntityFootprint::FuselageWidth` | 2 |
| `Public/Entities/EntityDefinition.h` | `EStandWaypointKind`, `FStandWaypoint`, `ServiceLane`; `ServiceLoop` and `ServiceLaneBounds()` deleted | 3 |
| `Private/Entities/EntityDefinition.cpp` | `BuildCodeCStand` derives the whole cycle | 3 |
| `Public/Build/ServiceLoopBuild.h` → `Public/Build/StandLaneBuild.h` | `FStandLaneBuild`; `WalkRing` and `LaneTurnRadius` deleted | 4 |
| `Private/Build/ServiceLoopBuild.cpp` → `Private/Build/StandLaneBuild.cpp` | lays the cycle; the whole spur block deleted | 4 |
| `Public/Model/RoadGuideline.h` | `ServiceLoopOwner` → `StandGeometryOwner`, `bServiceSpur` → `bStandApproach` | 4 |
| `Private/Build/AnchorLink.cpp` | entry links replace per-side discovery | 5 |
| `Tools/Python/build_stand_asset.py` | re-authors `DA_Stand_CodeC` | 6 |
| `AirsideTests/Private/CornerRunTest.cpp` | **new** — the corner cost round-trips | 1 |
| `AirsideTests/Private/StandLaneTest.cpp` | **new** — the cycle, its drivability, the derivation | 3, 4 |
| `AirsideTests/Private/ServiceLinkTest.cpp` | rewritten around entries | 5 |

---

## Task 1: The corner cost moves to `Solve/`, beside its inverse

`CornerRunFor` is a file-static in `ServiceLoopBuild.cpp`. Task 3 needs the same formula in
`Entities/` to derive where the boxes go. Two copies of it would drift, and the whole spec is
about a figure that drifted.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/GuidelineGeom.h` (declare beside `TightestRadius` at `:182`)
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/GuidelineGeom.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Build/ServiceLoopBuild.cpp` (delete the local copy; call the new one)
- Create: `Plugins/Airside/Source/AirsideTests/Private/CornerRunTest.cpp`

**Interfaces:**
- Produces: `double GuidelineGeom::CornerRunFor(double Radius, double Interior)` — `Interior` in radians, unsigned, as `FMath::Acos` of the dot of the two leg directions gives it. Returns `TNumericLimits<double>::Max()` for a hairpin.

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/CornerRunTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FCornerRunRoundTripsToItsRadiusTest,
    "Airside.Solve.CornerRunRoundTripsToItsRadius",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCornerRunRoundTripsToItsRadiusTest::RunTest(const FString& Parameters)
{
    // THE WHOLE POINT OF PUTTING IT HERE. CornerRunFor is the inverse of TightestRadius, and
    // nothing said so while they lived in different files - which is how the stand spec came
    // to cost a quadratic corner as if it were a circular fillet and lost 40% of the run it
    // needed at a right angle. The two are now measured against each other.
    const double Radius = 699.4;

    for (const double InteriorDegrees : { 30.0, 60.0, 90.0, 120.0, 137.25, 160.0 })
    {
        const double Interior = FMath::DegreesToRadians(InteriorDegrees);
        const double Run = GuidelineGeom::CornerRunFor(Radius, Interior);

        // The corner as the builder lays it: control ON the corner, ends Run back along each
        // leg. Put the corner at the origin with its legs symmetric about +X.
        const double Half = Interior * 0.5;
        const FVector2D Corner = FVector2D::ZeroVector;
        const FVector2D Back(FMath::Cos(Half), FMath::Sin(Half));
        const FVector2D Onward(FMath::Cos(Half), -FMath::Sin(Half));

        const double Delivered = GuidelineGeom::TightestRadius(
            Corner + Back * Run, Corner, Corner + Onward * Run);

        TestTrue(
            *FString::Printf(
                TEXT("a %.2f deg corner cut back %.1f uu delivers %.1f, wanted %.1f"),
                InteriorDegrees, Run, Delivered, Radius),
            FMath::IsNearlyEqual(Delivered, Radius, 0.5));
    }

    // AND THE FIGURE THE SPEC TURNS ON, pinned by name so a change to it is deliberate. A
    // right-angle corner costs 1.414 R, not R - the circular fillet's tangent length, which
    // is what the first draft used.
    TestTrue(
        TEXT("a right-angle corner costs sqrt(2) times its radius, not one times"),
        FMath::IsNearlyEqual(
            GuidelineGeom::CornerRunFor(Radius, UE_DOUBLE_HALF_PI), Radius * UE_DOUBLE_SQRT_2,
            0.5));

    // A hairpin has no cut that gives it any radius. The caller's clamp wants a number it can
    // scale, not an infinity that would scale both corners of a leg to nothing.
    TestTrue(
        TEXT("a hairpin reports the maximum rather than an infinity"),
        GuidelineGeom::CornerRunFor(Radius, 0.0) >= TNumericLimits<double>::Max() * 0.5);

    return true;
}

#endif
```

- [ ] **Step 2: Build twice, then run it and watch it fail for the right reason**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
```

Expected on the FIRST build: `Result: Succeeded` without compiling the new file — the two-builds
rule. On the SECOND: a compile error, `CornerRunFor` is not a member of `GuidelineGeom`. That is
the correct failure.

- [ ] **Step 3: Declare it in `Solve/GuidelineGeom.h`, immediately after `TightestRadius`**

```cpp
	/**
	 * How far back along each leg a corner must be cut so the quadratic laid across the cut
	 * delivers Radius. Interior is the unsigned angle between the two leg directions, radians.
	 *
	 * THE EXACT INVERSE OF TightestRadius, which is why it lives beside it. A corner cut back
	 * Run with its control ON the corner has delivered radius Run*sin^2(t/2)/cos(t/2); solve
	 * for Run and this is what falls out. Airside.Solve.CornerRunRoundTripsToItsRadius measures
	 * the two against each other rather than restating either.
	 *
	 * AT A RIGHT ANGLE THIS IS 1.414 R, NOT R. A circular fillet's tangent length at 90 degrees
	 * equals its radius, and the 2026-09-16 stand spec costed every corner that way and lost
	 * 40% of the run it needed - the same mistake 8be494c made one level up, in the same week,
	 * about the same kind of curve. It was a file-static in ServiceLoopBuild.cpp when that
	 * happened, where nothing outside the builder could find it.
	 *
	 * A HAIRPIN RETURNS THE MAXIMUM rather than an infinity: no cut gives it this radius, and a
	 * caller's proportional clamp asked for an infinity scales BOTH corners of a leg to nothing
	 * instead of cutting this one down to what its legs allow.
	 */
	AIRSIDE_API double CornerRunFor(double Radius, double Interior);
```

- [ ] **Step 4: Define it in `Private/Solve/GuidelineGeom.cpp`**

Body verbatim from `ServiceLoopBuild.cpp`'s file-static — it is already correct:

```cpp
double GuidelineGeom::CornerRunFor(double Radius, double Interior)
{
	const double Half = Interior * 0.5;
	const double Sin = FMath::Sin(Half);
	if (Sin * Sin < UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return TNumericLimits<double>::Max();
	}
	return Radius * FMath::Cos(Half) / (Sin * Sin);
}
```

- [ ] **Step 5: Delete the local copy and call the new one**

In `ServiceLoopBuild.cpp`, delete the anonymous-namespace `CornerRunFor` and change its one call
site (`:280`) to `GuidelineGeom::CornerRunFor(...)`. The file already includes
`Solve/GuidelineGeom.h`; confirm rather than assume.

- [ ] **Step 6: Build and run**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve
```

Expected: `Airside.Solve.CornerRunRoundTripsToItsRadius` passes. Confirm it appears in the run
count — a new test file that silently did not compile reports nothing rather than failing.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "refactor(solve): a corner's cut lives beside the radius it delivers"
```

---

## Task 2: The fuselage becomes a rectangle

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h` (`FEntityFootprint`, after `Wingspan`)
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/AircraftType.cpp` (`BuildA320`, beside the other footprint figures at `:15-20`)

**No test of its own.** This task ships a field and a default that nothing reads yet; its
assertion is `Airside.Entities.StandLaneCornersClearTheTruckLock` in Task 3, and the proof it is
inert here is that the suite does not move.

**Interfaces:**
- Produces: `FEntityFootprint::FuselageWidth` (double, uu). **Zero means the old zero-width centreline**, so every footprint authored before this field existed keeps its current meaning.

- [ ] **Step 1: Add the field**

In `FEntityFootprint`, after `Wingspan`:

```cpp
	/**
	 * Side to side, uu, so the fuselage is a BOX and not an axis. Zero keeps the old line.
	 *
	 * WHY IT DID NOT MATTER UNTIL NOW. Every route that had an opinion about an aeroplane ran
	 * OUTSIDE it - the service ring was outboard of the wingtips - so "does this line cross the
	 * centreline" was the whole of the question and a zero-width segment answered it. The stand
	 * lane now runs INSIDE the wingtip, alongside the fuselage, where a zero-width line permits
	 * a route straight down the aircraft's skin.
	 *
	 * WINGS AND TAILPLANE STAY PASSABLE, and that is unchanged rather than overlooked: driving
	 * under a wing is normal, and HydrantPit is under the starboard wing root because that is
	 * where a hydrant pit is. What is forbidden is passing THROUGH the aeroplane, and this only
	 * gives that rule a width.
	 *
	 * ZERO IS THE DEFAULT so a definition authored before this field keeps its old meaning
	 * rather than silently gaining a keep-out it was never laid out around.
	 */
	UPROPERTY(EditAnywhere) double FuselageWidth = 0.0;
```

- [ ] **Step 2: Author the A320's**

In `AircraftType.cpp`'s `BuildA320`, beside the other footprint figures:

```cpp
	// 3.95 m over the skin, the A320's actual fuselage diameter. NOT inflated here: the
	// clearance a vehicle keeps is the vehicle's business and is added where the route is
	// judged, so this stays a fact about the aeroplane.
	Type->Footprint.FuselageWidth = 395.0;
```

- [ ] **Step 3: Build, and confirm the suite is unmoved**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Run-AirsideTests.ps1
```

Expected: **354 run, 2 failed, 0 crashed** — the baseline, unchanged. Nothing reads the field yet.
A change here is a defect, not progress.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat(model): a fuselage has a width, because routes now pass beside it"
```

---

## Task 3: The stand declares a lane, and derives it

The large task. `ServiceLoop`'s four corners become a closed cycle of waypoints, and
`BuildCodeCStand` computes it from the aircraft, the anchors and the largest admitted vehicle.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h:96` (replace `ServiceLoop`), `:99-105` (delete `ServiceLaneBounds`)
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/EntityDefinition.cpp` (`BuildCodeCStand`'s loop block and the anchor positions)
- Create: `Plugins/Airside/Source/AirsideTests/Private/StandLaneTest.cpp`
- Modify: every caller of `ServiceLoop` / `ServiceLaneBounds()` — find them with
  `grep -rn "ServiceLoop\|ServiceLaneBounds" Plugins/ --include=*.cpp --include=*.h | grep -v Intermediate`
  before starting, and fix the list that grep returns, not this one.

**Interfaces:**
- Produces:
```cpp
UENUM()
enum class EStandWaypointKind : uint8 { Plain, Anchor, Entry };

USTRUCT()
struct AIRSIDE_API FStandWaypoint
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere) FVector2D Local = FVector2D::ZeroVector;
    UPROPERTY(EditAnywhere) FName AnchorId;                       // set iff Kind == Anchor
    UPROPERTY(EditAnywhere) EStandWaypointKind Kind = EStandWaypointKind::Plain;
};

UPROPERTY(EditAnywhere) TArray<FStandWaypoint> ServiceLane;       // closed implicitly
```
- Consumes: `GuidelineGeom::CornerRunFor` (Task 1), `FEntityFootprint::FuselageWidth` (Task 2).

- [ ] **Step 1: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/StandLaneTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FStandLaneCarriesItsAnchorsTest,
    "Airside.Entities.StandLaneCarriesItsAnchors",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLaneCarriesItsAnchorsTest::RunTest(const FString& Parameters)
{
    // THE WHOLE REDESIGN IN ONE ASSERTION. The ring ran outboard of the wingtips and every
    // anchor hung off it on a spur, which needed a 90 degree turn in 990 uu of depth against
    // the 1399 a truck's steering lock demands. The lane now runs ALONG the row the boxes are
    // painted on, so the anchors are ON it and nothing turns into one.
    UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

    TSet<FName> OnLane;
    for (const FStandWaypoint& Point : Stand->ServiceLane)
    {
        if (Point.Kind == EStandWaypointKind::Anchor)
        {
            OnLane.Add(Point.AnchorId);
        }
    }

    for (const FEntityAnchor& Anchor : Stand->Anchors)
    {
        if (TraversalForRole(Anchor.Role) == ETraversalClass::Aircraft)
        {
            continue;
        }
        TestTrue(
            *FString::Printf(TEXT("anchor '%s' is ON the lane, not spurred off it"),
                *Anchor.Id.ToString()),
            OnLane.Contains(Anchor.Id));
    }

    // AND EVERY ANCHOR WAYPOINT SITS WHERE ITS ANCHOR DOES. A waypoint carrying an id but a
    // different position would put the truck beside the box rather than in it, and every
    // other assertion here would still pass.
    for (const FStandWaypoint& Point : Stand->ServiceLane)
    {
        if (Point.Kind != EStandWaypointKind::Anchor)
        {
            continue;
        }
        const FEntityAnchor* Declared = Stand->Anchors.FindByPredicate(
            [&Point](const FEntityAnchor& Candidate) { return Candidate.Id == Point.AnchorId; });
        if (!TestNotNull(
                *FString::Printf(TEXT("waypoint names a real anchor '%s'"),
                    *Point.AnchorId.ToString()),
                Declared))
        {
            continue;
        }
        TestTrue(
            *FString::Printf(TEXT("waypoint for '%s' sits on it"), *Point.AnchorId.ToString()),
            Point.Local.Equals(Declared->LocalPosition, 0.5));
    }

    // FOUR ENTRIES, one per side. The ring's header valued "entry from any side" and it is
    // worth keeping; a stand with one entrance is the cul-de-sac AnchorLink's per-side rule
    // exists to prevent.
    int32 Entries = 0;
    for (const FStandWaypoint& Point : Stand->ServiceLane)
    {
        Entries += Point.Kind == EStandWaypointKind::Entry ? 1 : 0;
    }
    TestEqual(TEXT("four entries, one per side"), Entries, 4);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FStandLaneCornersClearTheTruckLockTest,
    "Airside.Entities.StandLaneCornersClearTheTruckLock",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLaneCornersClearTheTruckLockTest::RunTest(const FString& Parameters)
{
    // MEASURED ON THE CURVE, NEVER ON THE FILLET THAT SHAPED IT. That distinction cost three
    // sessions in 8be494c one level up. Here it is asked of the DEFINITION's polyline: every
    // corner must have legs long enough for CornerRunFor at the radius the largest admitted
    // vehicle needs, with neither neighbour stealing the leg.
    UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
    const FAirframe Largest = UAirsideSettings::ResolveLargestServiceVehicle();
    const double Needed = Largest.TightestFollowableRadius();
    if (!TestTrue(TEXT("the largest service vehicle steers on measured axles"), Needed > 0.0))
    {
        return false;
    }

    const int32 Count = Stand->ServiceLane.Num();
    if (!TestTrue(TEXT("the stand has a lane with corners to check"), Count >= 4))
    {
        return false;
    }

    for (int32 At = 0; At < Count; ++At)
    {
        const FVector2D& Here = Stand->ServiceLane[At].Local;
        const FVector2D& Previous = Stand->ServiceLane[(At + Count - 1) % Count].Local;
        const FVector2D& Next = Stand->ServiceLane[(At + 1) % Count].Local;

        const FVector2D Back = (Previous - Here).GetSafeNormal();
        const FVector2D Onward = (Next - Here).GetSafeNormal();
        const double Interior = FMath::Acos(
            FMath::Clamp(FVector2D::DotProduct(Back, Onward), -1.0, 1.0));

        // A straight-through waypoint - an anchor sitting mid-run - has no corner to check.
        if (Interior > UE_DOUBLE_PI - 0.01)
        {
            continue;
        }

        const double Run = GuidelineGeom::CornerRunFor(Needed, Interior);
        const double Shortest = FMath::Min(
            FVector2D::Distance(Here, Previous), FVector2D::Distance(Here, Next));

        TestTrue(
            *FString::Printf(
                TEXT("corner %d (%.0f deg) needs %.0f uu of leg and the shorter leg is %.0f"),
                At, FMath::RadiansToDegrees(Interior), Run, Shortest),
            Shortest >= Run);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FStandBoxesMoveWithTheLargestVehicleTest,
    "Airside.Entities.StandBoxesMoveWithTheLargestVehicle",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandBoxesMoveWithTheLargestVehicleTest::RunTest(const FString& Parameters)
{
    // THE DERIVATION, NOT ITS OUTPUT. Asserting EquipmentFwd == -182 would pass just as well
    // against a hand-typed -182, which is the thing this change exists to stop. So: build the
    // stand around a LONGER vehicle and require the boxes to have moved outward.
    //
    // The pit does not move. A hydrant is plant dug into concrete under the wing root - it is
    // the fixed thing the paint is arranged around, not the other way about.
    UAircraftType* A320 = NewObject<UAircraftType>(GetTransientPackage());
    UAircraftType::BuildA320(A320);

    UEntityDefinition* Normal = NewObject<UEntityDefinition>(GetTransientPackage());
    UEntityDefinition::BuildCodeCStand(Normal, A320);

    FAirframe Longer = UAirsideSettings::ResolveLargestServiceVehicle();
    Longer.SteerAxleX *= 1.5;

    UEntityDefinition* Roomier = NewObject<UEntityDefinition>(GetTransientPackage());
    UEntityDefinition::BuildCodeCStandFor(Roomier, A320, Longer);

    auto XOf = [](const UEntityDefinition* Definition, const TCHAR* Id) -> double
    {
        const FEntityAnchor* Found = Definition->Anchors.FindByPredicate(
            [Id](const FEntityAnchor& Candidate) { return Candidate.Id == FName(Id); });
        return Found != nullptr ? Found->LocalPosition.X : TNumericLimits<double>::Lowest();
    };

    TestTrue(TEXT("a longer vehicle pushes EquipmentFwd forward"),
        XOf(Roomier, TEXT("EquipmentFwd")) > XOf(Normal, TEXT("EquipmentFwd")) + 1.0);
    TestTrue(TEXT("a longer vehicle pushes EquipmentAft aft"),
        XOf(Roomier, TEXT("EquipmentAft")) < XOf(Normal, TEXT("EquipmentAft")) - 1.0);
    TestTrue(TEXT("and the hydrant pit does not move, because concrete does not"),
        FMath::IsNearlyEqual(
            XOf(Roomier, TEXT("HydrantPit")), XOf(Normal, TEXT("HydrantPit")), 0.5));

    return true;
}

#endif
```

- [ ] **Step 2: Build twice, run, and watch it fail for the right reason**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Entities.StandLane
```

Expected: compile errors — `FStandWaypoint` undefined, `ServiceLane` not a member,
`BuildCodeCStandFor` undefined. Correct: none of them exists yet.

- [ ] **Step 3: Declare the waypoint types in `EntityDefinition.h`**

Above `UEntityDefinition`, replacing `ServiceLoop`'s declaration at `:96` and deleting
`ServiceLaneBounds()` at `:99-105`:

```cpp
/** What a lane waypoint IS, so a builder never has to infer it from position. */
UENUM()
enum class EStandWaypointKind : uint8
{
	/** Shape only - a corner, or the flat a dip needs so the pit is driven through. */
	Plain,
	/** An anchor sits here. AnchorId names it, and the lane reuses that anchor's node. */
	Anchor,
	/** Where a road may join. The heading is the lane's own direction here. */
	Entry,
};

/**
 * One point of a stand's service lane, in the entity's own local space.
 *
 * NO HEADING FIELD, deliberately. An entry's heading is its lane's direction at that point and
 * an anchor's is already FEntityAnchor::LocalHeading; a copy here would be a value that must
 * agree with another value in the same asset, which is the drift FResolvedAnchor exists to
 * remove.
 */
USTRUCT()
struct AIRSIDE_API FStandWaypoint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) FVector2D Local = FVector2D::ZeroVector;

	/** Set if and only if Kind is Anchor. */
	UPROPERTY(EditAnywhere) FName AnchorId;

	UPROPERTY(EditAnywhere) EStandWaypointKind Kind = EStandWaypointKind::Plain;
};
```

And on `UEntityDefinition`, in place of `ServiceLoop`:

```cpp
	/**
	 * A closed, INVISIBLE vehicle lane threading the anchors, in the entity's own local space.
	 * Empty means none.
	 *
	 * CLOSED IMPLICITLY: the last point joins the first, and the array does NOT repeat it.
	 *
	 * IT RUNS ALONG THE AEROPLANE, NOT AROUND IT, and that is the whole of the 2026-09-16
	 * redesign. The ring this replaces ran outboard of the wingtips and every anchor hung off
	 * it on a spur - which needs a 90 degree turn, and a 90 degree turn at the 699 uu a real
	 * 8.5 m dispenser's steering lock allows needs 989 uu of run on each arm against the 990 uu
	 * of depth between the ring and the box row. It did not fit, and widening the ring made it
	 * worse. So the lane comes inside the wingtip and runs along the row the boxes are already
	 * painted on: four of five anchors sit ON it and nothing turns into one.
	 *
	 * ONE CYCLE, because a dead end is a reverse and reverse is a later stage. The hydrant, 400
	 * uu inboard of the row, is a flat-bottomed DIP in the lane rather than a stub - a pure V
	 * there would be a corner no vehicle can take, and under the rolling-steer law an agent
	 * stopped at one cannot turn at all, so it would be stuck rather than slow.
	 *
	 * COMPUTED by the builder that lays the anchors, never authored beside them, and sized from
	 * the LARGEST SERVICE VEHICLE ADMITTED rather than the one driving - see BuildCodeCStandFor.
	 * Deriving it at rebuild time was rejected for the reason the ring's own header gave: that
	 * is a runtime algorithm's opinion with no override, and a second evaluator of the same
	 * geometry.
	 *
	 * INVISIBLE, and a decision rather than an omission: no marking builder, no material, no
	 * mesh. It is a routing lane, not paint.
	 */
	UPROPERTY(EditAnywhere) TArray<FStandWaypoint> ServiceLane;
```

- [ ] **Step 4: Split the builder so the vehicle is an argument**

In `EntityDefinition.h`, beside `BuildCodeCStand`:

```cpp
	/**
	 * BuildCodeCStand, with the vehicle the ground is sized for made explicit.
	 *
	 * EXISTS FOR THE TEST that proves the derivation is a derivation. Given a longer vehicle
	 * the equipment boxes must move outward; asked of the shipping vehicle alone, that
	 * assertion would pass just as well against a hand-typed figure, which is exactly what
	 * this change removes. BuildCodeCStand forwards with ResolveLargestServiceVehicle().
	 */
	static void BuildCodeCStandFor(
		UEntityDefinition* Definition, UAircraftType* Aircraft, const FAirframe& Largest);
```

- [ ] **Step 5: Derive the lane in `BuildCodeCStandFor`**

Replace the `THE SERVICE LOOP` block in `EntityDefinition.cpp`. The anchors that do not move are
authored as today; `EquipmentFwd` and `EquipmentAft` are placed from the dip:

```cpp
	// WHAT THE GROUND HAS TO GIVE A DRIVER, resolved once and spent everywhere below. Sized for
	// the largest vehicle ADMITTED and never for the one driving now - the same rule the
	// taxiway geometry follows, and the reason a bigger dispenser needs no edit here.
	const double Radius = Largest.TightestFollowableRadius();

	// THE HYDRANT DIP. The pit sits 400 uu inboard of the box row, on a FLAT whose half-length
	// is one corner's run, with a leg rising to the row at DipLegAngle and a corner at each
	// end. Half-extent is 2*Run + Depth/tan(angle) - measured 1018.4 uu at the shipping truck.
	//
	// 40.5 DEGREES IS THE MINIMUM OF THAT EXPRESSION, not a taste: steeper legs shorten the
	// diagonal but lengthen the corner runs, shallower ones the reverse. Anything from about
	// 30 to 50 costs within 5% of it; the figure is stated so the arithmetic below has one
	// input rather than a search.
	constexpr double DipDepth = 400.0;
	constexpr double DipLegAngle = 40.5;
	const double DipLeg = FMath::DegreesToRadians(DipLegAngle);
	const double DipRun = GuidelineGeom::CornerRunFor(Radius, UE_DOUBLE_PI - DipLeg);
	const double DipHalfExtent = 2.0 * DipRun + DipDepth / FMath::Tan(DipLeg);

	AddFixture(TEXT("HydrantPit"), -1200.0, 700.0, 180.0, EServiceRole::Fuel);

	// THE BOXES SIT WHERE THE DIP LETS THEM, not where they were typed. They were at -300 and
	// -2100, which is 900 uu from the pit against the 1018 the dip needs - short by 118, and
	// short by a different amount for any other vehicle. So the figure is derived.
	//
	// HEADING 180, re-authored 2026-09-16. They faced -90 because the old spur arrived square
	// on from outboard; a truck driving up the row finishes pointing along the aircraft, so
	// that is how the box is painted. Slightly wrong for a belt loader, which really does
	// square up to a hold door - accepted until the reverse leg exists to do it properly.
	const double BoxY = 1100.0;
	AddFixture(TEXT("EquipmentFwd"), -1200.0 + DipHalfExtent, BoxY, 180.0, EServiceRole::Baggage);
	AddFixture(TEXT("EquipmentAft"), -1200.0 - DipHalfExtent, BoxY, 180.0, EServiceRole::Baggage);

	AddFixture(TEXT("FixedGPU"), 300.0, -600.0, 90.0, EServiceRole::GPU);
	AddFixture(TEXT("TugStand"), 1400.0, -600.0, 180.0, EServiceRole::Tug);
```

Then build the cycle. Order matters — it is a closed polyline and must not self-intersect:

```cpp
	// THE CYCLE, laid out starboard-forward, across the nose, port-aft, across the tail.
	//
	// THE CROSSINGS ARE DIAGONAL, and that is forced rather than chosen. Two square corners
	// between the two lanes need 2 * CornerRunFor(R, 90deg) = 1978 uu of lateral run and there
	// is 1700 between y=1100 and y=-600. Short by 278. A diagonal at CrossAngle spreads the
	// same turn over two shallower corners that do fit.
	constexpr double CrossAngleDegrees = 73.55;
	const double CrossAngle = FMath::DegreesToRadians(CrossAngleDegrees);
	const double CrossRun = GuidelineGeom::CornerRunFor(Radius, UE_DOUBLE_PI - CrossAngle);
	const double LaneGap = BoxY - (-600.0);
	const double CrossX = LaneGap / FMath::Tan(CrossAngle) + 2.0 * CrossRun;

	Definition->ServiceLane.Reset();
	auto Add = [Definition](double X, double Y, EStandWaypointKind Kind, const TCHAR* Id = nullptr)
	{
		FStandWaypoint Point;
		Point.Local = FVector2D(X, Y);
		Point.Kind = Kind;
		Point.AnchorId = Id != nullptr ? FName(Id) : NAME_None;
		Definition->ServiceLane.Add(Point);
	};
```

The waypoint order, with the entries placed on the runs where a road can reach them:

```cpp
	const double Fwd = -1200.0 + DipHalfExtent;     // EquipmentFwd's X
	const double Aft = -1200.0 - DipHalfExtent;     // EquipmentAft's X
	const double NoseX = FMath::Max(1400.0, Fwd) + CrossX;   // clear of TugStand and the nose
	const double TailX = Aft - CrossX;

	// Starboard, running forward: the dip's aft flat, the pit, the dip's forward flat, the
	// boxes, and the entry between the forward box and the nose crossing.
	Add(Aft,                      BoxY,    EStandWaypointKind::Anchor, TEXT("EquipmentAft"));
	Add(-1200.0 - DipRun,         700.0,   EStandWaypointKind::Plain);
	Add(-1200.0,                  700.0,   EStandWaypointKind::Anchor, TEXT("HydrantPit"));
	Add(-1200.0 + DipRun,         700.0,   EStandWaypointKind::Plain);
	Add(Fwd,                      BoxY,    EStandWaypointKind::Anchor, TEXT("EquipmentFwd"));
	Add(NoseX,                    BoxY,    EStandWaypointKind::Entry);

	// Across the nose, port-aft through the tug stand and the GPU.
	Add(NoseX,                    -600.0,  EStandWaypointKind::Entry);
	Add(1400.0,                   -600.0,  EStandWaypointKind::Anchor, TEXT("TugStand"));
	Add(300.0,                    -600.0,  EStandWaypointKind::Anchor, TEXT("FixedGPU"));
	Add(TailX,                    -600.0,  EStandWaypointKind::Entry);

	// Across the tail, and back onto the starboard run - the close is implicit.
	Add(TailX,                    BoxY,    EStandWaypointKind::Entry);
```

**Before implementing, sanity-check the shape numerically** — the dip's flats and the crossing's
diagonal must not overlap an anchor's straight run. Write the positions out with a
`UE_LOG(LogAirside, Log, ...)` on first build and read them, rather than trusting this listing.

- [ ] **Step 6: Run the three new tests**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Entities.StandLane
./Tools/Run-AirsideTests.ps1 -Filter Airside.Entities.StandBoxes
```

Expected: all three pass. If `StandLaneCornersClearTheTruckLock` fails, it is naming a corner
whose legs are too short — read the corner index and angle from the message and fix the LAYOUT,
never the tolerance. That assertion is the point of the task.

- [ ] **Step 7: Full suite, expecting breakage**

```
./Tools/Run-AirsideTests.ps1
```

Expected: several failures in `ServiceLinkTest` and anything else reading `ServiceLoop` — Tasks 4
and 5 fix them. Record the exact list in the commit message so the next task knows its target.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "feat(entities): a stand's lane runs along the aeroplane, not around it"
```

---

## Task 4: `FStandLaneBuild` lays the cycle

**Files:**
- Rename: `Public/Build/ServiceLoopBuild.h` → `Public/Build/StandLaneBuild.h`, `Private/Build/ServiceLoopBuild.cpp` → `Private/Build/StandLaneBuild.cpp` (use `git mv`, so the diff reads as a move)
- Modify: `Public/Model/RoadGuideline.h:185` and `:201` (the two marks)
- Modify: `Private/Build/AnchorLink.cpp:210` (the call), `:418-420`, `:561`, `:570`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/StandLaneTest.cpp` (add the placed-geometry test)

**Interfaces:**
- Produces: `FStandLaneBuild::FResult { TSet<FGuidelineNodeId> Nodes; TMap<FEntityInstanceId, TArray<FGuidelineEdgeId>> Lanes; TMap<FEntityInstanceId, TArray<FGuidelineNodeId>> Entries; int32 LanesBuilt; }`
- Produces: `FGuidelineEdge::StandGeometryOwner`, `FGuidelineEdge::bStandApproach`.
- Consumes: `UEntityDefinition::ServiceLane` (Task 3).

- [ ] **Step 1: Write the failing test**

Append to `StandLaneTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FPlacedStandLaneIsOneDrivableCycleTest,
    "Airside.Build.PlacedStandLaneIsOneDrivableCycle",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlacedStandLaneIsOneDrivableCycleTest::RunTest(const FString& Parameters)
{
    using namespace ServiceLinkFixture;

    URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
    UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
    const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

    const FStandLaneBuild::FResult Built = FStandLaneBuild::Build(*Net);
    const TArray<FGuidelineEdgeId>* Lane = Built.Lanes.Find(Placed);
    if (!TestNotNull(TEXT("the stand got a lane"), Lane))
    {
        return false;
    }

    // NO DEAD END. Forward-only rests entirely on this: a stub is a reverse, and reverse does
    // not exist yet. Every lane node carries two lane edges - the anchors included, which is
    // what "the lane runs THROUGH the box" means and what the ring's spur pairs never had.
    TMap<FGuidelineNodeId, int32> Degree;
    for (const FGuidelineEdgeId& Id : *Lane)
    {
        const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
        if (Edge == nullptr || !Edge->bAlive)
        {
            continue;
        }
        ++Degree.FindOrAdd(Edge->A);
        ++Degree.FindOrAdd(Edge->B);
    }
    for (const TPair<FGuidelineNodeId, int32>& Node : Degree)
    {
        TestEqual(TEXT("every lane node is driven through, never into"), Node.Value, 2);
    }

    // AND EVERY CURVE IS DRIVABLE, measured on the SAMPLED geometry rather than on the
    // definition's corners - the 8be494c lesson. A definition whose legs are long enough can
    // still deliver a tight curve if the builder's clamp shrank a corner to fit its leg.
    const double Needed =
        UAirsideSettings::ResolveLargestServiceVehicle().TightestFollowableRadius();
    for (const FGuidelineEdgeId& Id : *Lane)
    {
        const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
        if (Edge == nullptr || !Edge->bAlive)
        {
            continue;
        }
        const FGuidelineNode* A = Net->GetGuidelineNode(Edge->A);
        const FGuidelineNode* B = Net->GetGuidelineNode(Edge->B);
        if (A == nullptr || B == nullptr)
        {
            continue;
        }
        const double Delivered =
            GuidelineGeom::TightestRadius(A->Position, Edge->Control, B->Position);
        TestTrue(
            *FString::Printf(TEXT("a lane curve delivers %.0f uu against the %.0f needed"),
                Delivered, Needed),
            Delivered >= Needed - 0.5);
    }

    // AND NOTHING RUNS THROUGH THE AEROPLANE. The rule is unchanged - under a wing is normal,
    // through the fuselage is not - but the fuselage is a RECTANGLE now, not an axis, because
    // this lane runs alongside it where a zero-width centreline would permit a route down the
    // skin. Measured on the sampled curve, not on the definition's corners.
    {
        const FEntityFootprint& Footprint = Stand->DesignAircraft->Footprint;
        const double HalfWidth = Footprint.FuselageWidth * 0.5;
        const FBox2D Fuselage(
            FVector2D(Footprint.TailX, -HalfWidth), FVector2D(Footprint.NoseX, HalfWidth));
        TestTrue(TEXT("the design aircraft has a fuselage width to test against"),
            Footprint.FuselageWidth > 0.0);

        for (const FGuidelineEdgeId& Id : *Lane)
        {
            TArray<FVector2D> Points;
            if (!Net->SampleGuideline(Id, Points))
            {
                continue;
            }
            for (const FVector2D& Point : Points)
            {
                TestFalse(
                    *FString::Printf(TEXT("a lane point at (%.0f,%.0f) is inside the fuselage"),
                        Point.X, Point.Y),
                    Fuselage.IsInside(Point));
            }
        }
    }

    // THE ANCHORS KEPT THEIR OWN NODES. A lane that made fresh nodes at the anchor positions
    // would look identical here and route nothing: FuelService asks for the ANCHOR's node.
    const FEntityInstance* Entity = Net->GetEntity(Placed);
    if (TestNotNull(TEXT("the stand is placed"), Entity))
    {
        for (const FResolvedAnchor& Anchor : Entity->ResolvedAnchors)
        {
            if (TraversalForRole(Anchor.Role) == ETraversalClass::Aircraft)
            {
                continue;
            }
            const FGuidelineNode* Node = Net->GetGuidelineNode(Anchor.Node);
            if (TestNotNull(
                    *FString::Printf(TEXT("anchor '%s' has a node"), *Anchor.Id.ToString()),
                    Node))
            {
                TestEqual(
                    *FString::Printf(TEXT("and the lane runs through '%s'"),
                        *Anchor.Id.ToString()),
                    Node->Incident.Num(), 2);
            }
        }
    }

    return true;
}
```

`ServiceLinkFixture::PlaceStand` lives in `ServiceLinkTest.cpp`; move it into a shared header
`AirsideTests/Private/StandFixture.h` as part of this task rather than copying it, and include
that from both files.

- [ ] **Step 2: Build twice, run, watch it fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.PlacedStandLane
```

Expected: `FStandLaneBuild` undefined.

- [ ] **Step 3: Rename the two edge marks**

In `RoadGuideline.h`, rename `ServiceLoopOwner` → `StandGeometryOwner` and `bServiceSpur` →
`bStandApproach`, keeping both doc comments and updating the words "loop"/"ring"/"spur" in them.
**Keep the marks.** `URoadNetwork::IsServiceNodeConnected` walks `StandGeometryOwner` to answer
"does this hydrant reach a road", and `FuelService.cpp:131` and `:472` are its callers; without
it every stand in an empty field reads as connected. Add that sentence to the comment.

Then fix every reader — `grep -rn "ServiceLoopOwner\|bServiceSpur" Plugins/ --include=*.cpp --include=*.h | grep -v Intermediate`.

- [ ] **Step 4: Move the builder and gut the spur block**

```bash
git mv Plugins/Airside/Source/Airside/Public/Build/ServiceLoopBuild.h \
       Plugins/Airside/Source/Airside/Public/Build/StandLaneBuild.h
git mv Plugins/Airside/Source/Airside/Private/Build/ServiceLoopBuild.cpp \
       Plugins/Airside/Source/Airside/Private/Build/StandLaneBuild.cpp
```

Then, inside:

- `FServiceLoopBuild` → `FStandLaneBuild`; `LoopsBuilt` → `LanesBuilt`; drop `SpursBuilt`.
- **Delete `WalkRing` and `LaneTurnRadius`.** `WalkRing` walks a ring and steps over spurs;
  neither exists. `LaneTurnRadius` was a typed 750 and is now
  `UAirsideSettings::ResolveLargestServiceVehicle().TightestFollowableRadius()`, read once at the
  top of `Build`.
- **Delete the entire spur block** (everything from `// SPURS. TWO to each service anchor` to the
  end of the per-entity loop). Anchors are on the lane.
- **Keep** the corner measurement, the proportional clamp, and the enter/exit node laying
  verbatim. They are the algorithm and they are right.
- `Instance.Definition->ServiceLoop` becomes `Instance.Definition->ServiceLane`, and the guard
  `ServiceLoop.Num() < 3` becomes `ServiceLane.Num() < 3`.
- `World[]` is filled from `Waypoint.Local` through the same `ToWorld`.

The one genuinely new behaviour — an anchor waypoint reuses its anchor's node instead of making
one:

```cpp
		// AN ANCHOR WAYPOINT DOES NOT GET A NEW NODE. The anchor already owns one, made at
		// placement, and FuelService routes to THAT handle - a lane that laid a fresh node at
		// the same position would look right in the overlay and route nothing.
		//
		// An anchor on the lane is mid-run by construction, so it is the no-bend case: one
		// node, used as both enter and exit. A definition that put an anchor ON a corner would
		// be asking for the bend to start inside the painted box, and is refused here rather
		// than laid crooked.
		auto NodeAt = [&Network, &Instance](
			const FStandWaypoint& Waypoint, const FVector2D& At) -> FGuidelineNodeId
		{
			if (Waypoint.Kind == EStandWaypointKind::Anchor)
			{
				for (const FResolvedAnchor& Resolved : Instance.ResolvedAnchors)
				{
					if (Resolved.Id == Waypoint.AnchorId)
					{
						return Resolved.Node;
					}
				}
				UE_LOG(LogAirside, Warning,
					TEXT("Stand lane names anchor '%s', which the instance does not have. "
					     "Laying a plain node; nothing will route to that service."),
					*Waypoint.AnchorId.ToString());
			}
			return Network.AddGuidelineNode(At, /*bDerived=*/true);
		};
```

Use it in the `Run[At] < LaneWeldTolerance` branch. In the bend branch, log a warning and treat
the waypoint as plain if it is an anchor — an anchor on a corner is an authoring error.

Record every `Entry` waypoint's node in `Result.Entries`, keyed by entity. Task 5 reads it.

- [ ] **Step 5: Run the new test and the suite**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.PlacedStandLane
./Tools/Run-AirsideTests.ps1
```

Expected: the new test passes. `SpursLeaveTheLaneTangentially` should now be **deleted** — it
asserts a spur pair that no longer exists, and `PlacedStandLaneIsOneDrivableCycle` is its
replacement. Say so in the commit; a deleted test is a claim that needs an argument.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(build): the stand's lane is laid through its anchors, not past them"
```

---

## Task 5: A road joins a declared entry

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Build/AnchorLink.cpp` — the lane-link block at `:352-600`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/ServiceLinkTest.cpp` — rewrite `ServiceLaneEntersOnEverySideWithinReach`

**Interfaces:**
- Consumes: `FStandLaneBuild::FResult::Entries` (Task 4).

- [ ] **Step 1: Rewrite the failing test**

Replace `FServiceLaneEntersOnEverySideWithinReachTest`'s body in `ServiceLinkTest.cpp`. It has
been red since `563fa44`; rewriting it around entries is what makes it pass.

```cpp
    // A ROAD BESIDE EACH SIDE IN TURN joins THAT side's entry and no other. The ring had to
    // DISCOVER which sides a road was beside - IsLaneBend, WholeSide, and three thresholds
    // tuned against one another at AnchorLink.cpp:418 - because it declared no entrances. A
    // stand now declares four, so this measures a declaration rather than a heuristic.
    for (const FVector2D& RoadAt : { FVector2D(0.0, 4000.0), FVector2D(0.0, -4000.0),
                                     FVector2D(6000.0, 0.0), FVector2D(-8000.0, 0.0) })
    {
        URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
        UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
        const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
        LayServiceRoadThrough(*Net, RoadAt);

        FAnchorLink::Build(*Net);

        int32 Linked = 0;
        for (const FGuidelineNodeId& Entry : EntriesOf(*Net, Placed))
        {
            const FGuidelineNode* Node = Net->GetGuidelineNode(Entry);
            Linked += (Node != nullptr && Node->Incident.Num() > 2) ? 1 : 0;
        }
        TestEqual(
            *FString::Printf(TEXT("a road at (%.0f,%.0f) joins exactly one entry"),
                RoadAt.X, RoadAt.Y),
            Linked, 1);
    }
```

`LayServiceRoadThrough` and `EntriesOf` are fixture helpers; put them in `StandFixture.h` beside
`PlaceStand`.

- [ ] **Step 2: Run it and watch it fail**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Build.ServiceLaneEnters
```

Expected: fails — the per-side search still runs and links whatever it finds.

- [ ] **Step 3: Replace discovery with declaration**

Delete `IsLaneBend`, `WholeSide`, the `Sides`/`Approach`/`Seen` sweep and the `Qualifies`
expression. In their place, for each entity in `Built.Entries`:

```cpp
		// ONE PENDING LINK PER DECLARED ENTRY. A stand says where it may be entered; this only
		// asks whether a road is within reach of each. The per-side search this replaces
		// existed because a ring declared nothing, and it carried three thresholds tuned
		// against one another - see the 2026-09-16 spec for what each was compensating for.
		//
		// RAY, NOT PROXIMITY, and along the entry's OWN heading - as the aircraft pose link
		// already is. That is what makes a truck turn off the road ALONG the stand instead of
		// across it, which is the property the ring was buying by sliding a join along itself.
		for (const FGuidelineNodeId& EntryNode : Entries)
		{
			const FGuidelineNode* Node = Network.GetGuidelineNode(EntryNode);
			if (Node == nullptr || Node->Incident.Num() > 2)
			{
				// Two lane edges is an unjoined entry; more means a road is already on it.
				continue;
			}

			FPendingLink Link;
			Link.Node = EntryNode;
			Link.At = Node->Position;
			Link.Dir = EntryHeading(Network, EntryNode);
			Link.Class = ETraversalClass::GroundVehicle;
			Link.Kind = ELinkKind::Ray;
			Link.MaxWingspan = 0.0;
			Link.Radius = StandRadius;
			Link.Reach = ServiceLinkRadius;
			OutPending.Add(Link);
		}
```

`EntryHeading` reads the entry's own lane edge and returns the outward direction — the tangent at
that end, negated, since the lane runs inward from it. Use `GuidelineGeom::Tangent`, never a
difference of samples: a quadratic's first sampled chord is a degree or two off its true tangent.

- [ ] **Step 4: Run the test and the suite**

```
./Tools/Run-AirsideTests.ps1
```

Expected: `ServiceLaneEntersOnEverySideWithinReach` passes — the first time since `563fa44`.
Read the `N test(s) run, N failed, N crashed` line; the target is **0 failed**.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(build): a stand declares where it may be entered"
```

---

## Task 6: Content, the whole suite, and the map

**Files:**
- Modify: `Tools/Python/build_stand_asset.py`
- Modify: `Content/Entities/DA_Stand_CodeC.uasset` (re-authored by the script)

- [ ] **Step 1: Teach the script the new field**

The script does not write the lane — `build_code_c_stand` does, and `replace_asset` recreates the
asset — so the only change is the READBACK at `build_stand_asset.py:142-146`, which is the one
place a lane with no mesh and no material can be read at all:

```python
    # THE LANE, logged as its own fact. It is invisible in the editor - no mesh, no material,
    # no marking builder - so this line is the only place its waypoints can be read back. The
    # KIND is logged beside the position because a waypoint that lost its anchor id still has a
    # position, and would read as correct here.
    lane = stand.get_editor_property("service_lane")
    unreal.log("MARKER: DA_Stand_CodeC service lane, %d waypoint(s)" % len(lane))
    for point in lane:
        local = point.get_editor_property("local")
        unreal.log("MARKER:   (%.0f, %.0f) %s %s" % (
            local.x, local.y,
            point.get_editor_property("kind"),
            point.get_editor_property("anchor_id")))
```

**`BuildCodeCStand` must keep its `UFUNCTION` and its name.** The script calls
`unreal.EntityDefinition.build_code_c_stand(stand, design_aircraft)` at `:125`, and Task 3 splits
the body into `BuildCodeCStandFor`. The old name stays as the forwarder — a `UFUNCTION` that
moves is a Python script and a Blueprint that stop compiling.

- [ ] **Step 2: Full build with the editor closed, then re-author**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
```

Then run `build_stand_asset.py` headlessly and **check the .uasset's mtime on disk afterwards** —
a headless save reports success without writing.

- [ ] **Step 3: The authoritative suite run**

```
./Tools/Run-AirsideTests.ps1
```

Read `N test(s) run, N failed, N crashed`. Target: **0 failed, 0 crashed**, and a run count of
354 minus the deleted `SpursLeaveTheLaneTangentially` plus the five added here.

- [ ] **Step 4: Look at the map — this is not optional**

A green suite is not evidence for a geometry change: a graph change that passed 348 tests once
put kilometre-wide arcs across the apron. In PIE, with `Saved/Logs/AirportMgr.log` open:

1. Send a fuel truck from the depot to a stand. Quote its `Speed profile:` line. The
   tightest-radius rule must read `lateral accel`, **not** `TIGHTER THAN THE STEERING LOCK`.
2. Confirm no `Route asks for R=...` warnings for that route.
3. Confirm no `Junction at (...)` warnings from `RoadGuidelineBuilder` on the road feeding it.
4. `python Tools/Mcp.py shot out.png` with the truck parked. It must sit in its box **along** the
   aircraft, and the lane must not pass through the fuselage.

- [ ] **Step 5: Commit, push, and open the PR**

```bash
git add -A
git commit -m "feat(content): the Code C stand ships its lane"
git push -u origin HEAD
gh pr create --fill
```

Fill the PR template's build line and test line. This is partly a refactor, so give the log-line
and comment-line deltas too: `UE_LOG(` count should be **up** by two (the missing-anchor warning
and the anchor-on-a-corner warning) and down by none.

---

## Self-review notes

**Spec coverage.** Spec decisions 1–6 map to tasks: 1 → Task 3 (the cycle, no stubs); 2 → Task 3
(headings re-authored); 3 → Task 2; 4 → Task 3 (`BuildCodeCStandFor`); 5 → Tasks 3 and 5; 6 →
Task 3. The spec's six tests map to Tasks 1, 3, 4 and 5. The unreachable-stand UI needs no task,
which the spec argues: `IsServiceNodeConnected` is unchanged by construction, and Task 4 adds the
sentence to `StandGeometryOwner`'s comment that says why.

**One deviation, argued above:** a single closed `ServiceLane` in place of the spec's plural open
`ServiceLanes`. Amend the spec.

**Three figures are stated, not derived, and each says so at its site:** `DipDepth` 400 (the
pit's real offset under the wing root), `DipLegAngle` 40.5° and `CrossAngleDegrees` 73.55°. The
two angles are the minima of expressions whose optimum is flat — anything from about 30° to 50°
costs within 5% for the dip — so they are inputs rather than searches. Each is measured against
the delivered radius by `StandLaneCornersClearTheTruckLock`, so a wrong one fails loudly.

**Not verified, and the first thing to check in Task 3:** the waypoint ORDER in Step 5 is written
from the geometry, not run. The dip's flats, the boxes and the crossings must not overlap, and
`NoseX` uses `FMath::Max(1400, Fwd)` on the assumption that `TugStand` is the forward-most anchor.
Step 5 says to log the derived positions and read them rather than trusting the listing. That is
where this plan is most likely to be wrong.
