# The Stand Layout Template — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the stand's derived service lane with an authored layout template — staging rank, a bay per service, one directed entry — whose legs are derived and proven drivable ONCE, for every vehicle, so that placing a stand computes no geometry at all.

**Architecture:** A human places poses in aircraft-relative coordinates on `UEntityDefinition`; `BuildCodeCStandFor` derives the connecting legs and a build-time test proves each one with the piece-A oracle and `FReverseRun::Start`. `FStandLayoutBuild` transforms the template into the guideline graph at placement — a transform preserves curvature, so a verified template cannot become undrivable. The entry is a one-way edge, which the search already honours.

**Tech Stack:** UE 5.8.2 C++, `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, `Tools/Run-AirsideTests.ps1`, `Tools/Python/build_stand_asset.py`.

**Spec:** `docs/superpowers/specs/2026-09-17-stand-layout-solver-design.md`

---

## Global Constraints

- **Engine:** UE 5.8.2 at `D:\Epic\UE_5.8`. Project at `C:\repos\AirportMgr2`, branch `feature/lane-entrances`.
- **The editor must be CLOSED to build.** Most tasks here add `UPROPERTY`s or `USTRUCT`s, so none is a Live Coding change.
- **A new test `.cpp` needs TWO builds.** The first reports `Result: Succeeded` without compiling it.
- **UHT cannot see a plain enum**, and a `UENUM`/`USTRUCT` must not sit between another type's doc comment and its declaration — UHT reports `Found 'UENUM' when expecting struct`. Put new reflected types ABOVE the doc comment of whatever follows them.
- **NEVER trust the runner's exit code.** Read `N test(s) run, N failed, N crashed`.
- **A `UE_LOG` Warning does NOT fail an automation test here.** `FAutomationTestBase::bElevateLogWarningsToErrors` is false (`Runtime/Core/Private/Misc/AutomationTest.cpp:181`) and nothing in `Config/` sets the key.
- **`FRoutePlan::IsValid()` is `Result == ERouteResult::Found`**, not "has a polyline". A fixture that fills only geometry is refused at the first guard of anything it is passed to.
- **`Solve/` may include only `CoreMinimal.h` and `Solve/`.** `Check-Architecture.ps1` enforces it and runs before the tests.
- **Comments explain WHY**, and record the measurement that settled a figure. Do not write a justification for anything you have not measured — four false-mechanism comments were caught on this branch already.
- **Tests restate arithmetic, never a judgement.** Ask `FSpeedProfile` whether a line is drivable; do not re-implement its rule. See `FSpeedProfile::WasTighterThanLock`.
- End commit messages with:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`

### Starting point

**366 tests run, 1 failed, 0 crashed.** The one failure is deliberate:
`Airside.Model.Traffic.TruckDrivesTheWholeRouteToTheHydrant`, red since piece A at
`sharpest 175 deg at 9489`. **It is this plan's acceptance criterion** and Task 7 is where it
goes green.

### Figures, all derived

| Quantity | Value | From |
|---|---|---|
| Forward limit, largest service vehicle | 699.4 uu | `L / sin(lock)`, `TightestFollowableRadius()` |
| Reverse limit, same vehicle | 494.5 uu | `L / tan(lock)`, `TightestReversibleRadius()` |
| Code C span band | 3600 uu | `IcaoCode` row C, already present |
| Code C wingtip clearance | 450 uu | NEW row; ICAO Code C |
| Code C stand width | 4500 uu | **derived**: 3600 + 2×450 |
| Code C stand depth | 5500 uu | NEW row; authored |
| Largest airframe admitted, Code C | B738 | tail −3430, nose +520, span 3580 |

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `Public/Solve/IcaoCode.h`, `Private/Solve/IcaoCode.cpp` | clearance and depth rows; width derived | 1 |
| `AirsideTests/Private/IcaoCodeTest.cpp` | the derivation, not its output | 1 |
| `Public/Entities/EntityDefinition.h` | `FServiceBay`, entry, staging, `RequiredExtent` | 2 |
| `Private/Entities/EntityDefinition.cpp` | `BuildCodeCStandFor` authors poses (2), derives legs (3) | 2, 3 |
| `AirsideTests/Private/StandLayoutTest.cpp` | **new** — poses, then every leg proven | 2, 3 |
| `Public/Build/StandLaneBuild.h` → `StandLayoutBuild.h` | lays the template; the lane's apparatus deleted | 4 |
| `Private/Build/StandLaneBuild.cpp` → `StandLayoutBuild.cpp` | same | 4 |
| `Private/Build/AnchorLink.cpp` | entry link validates instead of solving | 5 |
| `Public/Model/RoadEntity.h` | bay claim state on `FEntityInstance` | 6 |
| `Tools/Python/build_stand_asset.py` | reads back the template | 7 |

---

## Task 1: Stand sizes join the one table

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/IcaoCode.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/IcaoCode.cpp` (the `FRow` struct and `Rows[]`)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/IcaoCodeTest.cpp`

**Interfaces:**
- Produces: `double IcaoCode::StandWidthForLetter(const FString& Letter)` — derived, uu.
- Produces: `double IcaoCode::StandDepthForLetter(const FString& Letter)` — authored, uu.
- Produces: `FString IcaoCode::LetterForStandSize(double WidthUu, double DepthUu)` — the mirror of `LetterForWingspan`; the largest letter whose width AND depth both fit. Empty when none does.

- [ ] **Step 1: Write the failing test**

Append to `IcaoCodeTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FStandWidthIsDerivedFromClearanceTest,
    "Airside.Solve.StandWidthIsDerivedFromClearance",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandWidthIsDerivedFromClearanceTest::RunTest(const FString& Parameters)
{
    // THE DERIVATION, NOT ITS OUTPUT. Asserting 4500 for Code C would pass just as well against
    // a typed 4500, which is the thing this change exists to stop. A stand's width IS the span
    // band plus twice the wingtip clearance - every letter, to the centimetre - so that is what
    // is asserted, and the figures move together or the test fails.
    struct FCase { const TCHAR* Letter; double Span; double Clearance; };
    const FCase Cases[] = {
        { TEXT("A"), 1500.0, 300.0 },
        { TEXT("B"), 2400.0, 300.0 },
        { TEXT("C"), 3600.0, 450.0 },
        { TEXT("D"), 5200.0, 750.0 },
        { TEXT("E"), 6500.0, 750.0 },
        { TEXT("F"), 8000.0, 750.0 },
    };

    for (const FCase& Case : Cases)
    {
        TestEqual(
            *FString::Printf(TEXT("stand %s is its span band plus twice its clearance"), Case.Letter),
            IcaoCode::StandWidthForLetter(Case.Letter),
            Case.Span + 2.0 * Case.Clearance,
            0.5);
    }

    // AND THE MIRROR. A stand's SIZE decides which airframes may use it, which is the mechanic:
    // a player who drags a bigger stand gets bigger aircraft as a consequence.
    //
    // BOTH DIMENSIONS, NEVER ONE. Width alone would call a 67 x 30 m stand Code D, when nothing
    // bigger than a King Air fits in 30 m of depth. The letter is the largest whose width AND
    // depth both fit, and the shallow case below is the one that discriminates.
    TestEqual(TEXT("45 x 55 m is a Code C stand"),
        IcaoCode::LetterForStandSize(4500.0, 5500.0), FString(TEXT("C")));
    TestEqual(TEXT("60 x 55 m is still Code C - D needs 67 m of width"),
        IcaoCode::LetterForStandSize(6000.0, 5500.0), FString(TEXT("C")));
    TestEqual(TEXT("67 x 70 m is genuinely Code D"),
        IcaoCode::LetterForStandSize(6700.0, 7000.0), FString(TEXT("D")));
    TestEqual(TEXT("67 x 30 m is a Code B - D-wide but far too shallow"),
        IcaoCode::LetterForStandSize(6700.0, 3000.0), FString(TEXT("B")));
    TestEqual(TEXT("45 x 90 m is a Code C - deep, but the span binds"),
        IcaoCode::LetterForStandSize(4500.0, 9000.0), FString(TEXT("C")));

    // Below the smallest stand there is no letter to give, and saying "A" would admit a Cessna
    // to a space it does not fit. Empty means "no stand of any letter fits this", which is a
    // real answer: such a stand is refused at placement.
    TestTrue(TEXT("under the smallest stand, no letter"),
        IcaoCode::LetterForStandSize(2000.0, 2000.0).IsEmpty());

    // Depth is authored rather than derived, so it is asserted by value - with its provenance
    // in the table, which is where a reader checks it against a real aerodrome.
    TestEqual(TEXT("a Code C stand is 55 m deep"), IcaoCode::StandDepthForLetter(TEXT("C")), 5500.0, 0.5);

    return true;
}
```

- [ ] **Step 2: Build twice; the test must fail to compile**

Expected: `StandWidthForLetter` is not a member of `IcaoCode`. That is the right failure.

- [ ] **Step 3: Add the two columns and the three functions**

In `IcaoCode.cpp`, extend `FRow` and every row. **Keep the existing columns untouched.**

```cpp
		/** One row of ICAO Annex 14 Table 1-1: everything the letter sets, uu. */
		struct FRow
		{
			const TCHAR* Letter;
			double MaxWingspan;
			double RunwayWidth;
			double StandTurnRadius;

			/**
			 * Wingtip clearance on a stand, uu - the gap ICAO wants between a parked
			 * aeroplane's wingtip and anything beside it.
			 *
			 * THE STAND'S WIDTH IS NOT A COLUMN, because it is this plus the span band twice
			 * over and a stored width would be a third figure that has to agree with two
			 * others. See StandWidthForLetter, and this file's header for the three call
			 * sites that once typed the same table separately.
			 */
			double WingtipClearance;

			/**
			 * How deep a stand of this letter is, uu - nose to the back of its GSE road.
			 *
			 * AUTHORED, not derived, and it is the only figure here that is. Width follows
			 * from span and clearance; depth follows from aircraft LENGTH and the room an
			 * equipment area and a service road need, and no clean rule produces it. Standard
			 * aerodrome design values, as the header says of the rest - the first thing to
			 * check if a real layout looks wrong.
			 */
			double StandDepth;
		};

		static const FRow Rows[] = {
			{ TEXT("A"), 1500.0, 1800.0, 1500.0,  300.0,  2000.0 },
			{ TEXT("B"), 2400.0, 2300.0, 2000.0,  300.0,  3000.0 },
			{ TEXT("C"), 3600.0, 3000.0, 2500.0,  450.0,  5500.0 },
			{ TEXT("D"), 5200.0, 4500.0, 4000.0,  750.0,  7000.0 },
			{ TEXT("E"), 6500.0, 4500.0, 5000.0,  750.0,  9000.0 },
			{ TEXT("F"), 8000.0, 6000.0, 6000.0,  750.0, 10000.0 },
		};
```

Then the three functions, beside the existing ones. `LetterForStandSize` walks the rows and returns the LARGEST letter whose width and depth BOTH fit, or empty when none does. Match the letter case-insensitively as `RadiusForLetter` already does, and fall back to C for an unknown letter for the reason its comment gives.

- [ ] **Step 4: Run it**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Solve.StandWidthIsDerivedFromClearance
```

Expected: PASS. Confirm the test appears in the run count.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(solve): a stand's width is its span band plus twice its clearance"
```

---

## Task 2: The template's poses

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h` (replace `ServiceLane` and `FStandWaypoint`)
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/EntityDefinition.cpp` (`BuildCodeCStandFor`)
- Create: `Plugins/Airside/Source/AirsideTests/Private/StandLayoutTest.cpp`

**Interfaces:**
- Produces: `FServiceBay` with `AnchorId`, `Local`, `LocalHeading` and three `FRoutePlan` legs (filled in Task 3).
- Produces: on `UEntityDefinition`: `EntryLocal`, `EntryHeading`, `EntryLeg`, `StagingLocal`, `StagingHeading`, `StagingCapacity`, `ServiceBays`, `RequiredExtent`.
- Consumes: `IcaoCode::StandWidthForLetter` / `StandDepthForLetter` (Task 1).

- [ ] **Step 1: Write the failing test**

Create `StandLayoutTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Solve/IcaoCode.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FStandLayoutFitsItsLettersFloorTest,
    "Airside.Entities.StandLayoutFitsItsLettersFloor",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandLayoutFitsItsLettersFloorTest::RunTest(const FString& Parameters)
{
    // THE TEMPLATE IS BUILT FOR THE FLOOR OF ITS BAND. 45 m to just under 67 is all Code C, and
    // a template authored at a comfortable 55 would fail exactly where a player drew the
    // smallest stand the rules allow. So the assertion is against the letter's MINIMUM, which
    // is what IcaoCode reports.
    UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

    const double Width = IcaoCode::StandWidthForLetter(TEXT("C"));
    const double Depth = IcaoCode::StandDepthForLetter(TEXT("C"));

    AddInfo(FString::Printf(TEXT("Code C floor %.0f x %.0f uu; template needs %.0f x %.0f"),
        Width, Depth, Stand->RequiredExtent.X, Stand->RequiredExtent.Y));

    TestTrue(
        *FString::Printf(TEXT("the layout fits the width floor (%.0f needed, %.0f available)"),
            Stand->RequiredExtent.X, Width),
        Stand->RequiredExtent.X <= Width);
    TestTrue(
        *FString::Printf(TEXT("the layout fits the depth floor (%.0f needed, %.0f available)"),
            Stand->RequiredExtent.Y, Depth),
        Stand->RequiredExtent.Y <= Depth);

    // A BAY PER SERVICE THAT A VEHICLE DRIVES TO. An anchor a truck visits with no bay to park
    // in is the pile-up this design exists to prevent.
    for (const FEntityAnchor& Anchor : Stand->Anchors)
    {
        if (TraversalForRole(Anchor.Role) == ETraversalClass::Aircraft)
        {
            continue;
        }
        const FServiceBay* Bay = Stand->ServiceBays.FindByPredicate(
            [&Anchor](const FServiceBay& Candidate) { return Candidate.AnchorId == Anchor.Id; });
        TestNotNull(
            *FString::Printf(TEXT("anchor '%s' has a bay"), *Anchor.Id.ToString()), Bay);
    }

    // THE STAGING RANK HOLDS MORE THAN ONE. A single pose queues arriving trucks on the road
    // outside and blocks it, which is what "so the vehicles don't pile up" rules out.
    TestTrue(TEXT("staging is a rank, not a point"), Stand->StagingCapacity > 1);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FStandExtentClearsTheLargestAirframeAdmittedTest,
    "Airside.Entities.StandExtentClearsTheLargestAirframeAdmitted",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandExtentClearsTheLargestAirframeAdmittedTest::RunTest(const FString& Parameters)
{
    // A LIVE DEFECT THIS FIXES. The old geometry was sized from the A320's tail at -3250, but
    // DA_Aircraft_B738 is authored at -3430 and already parks on the same stand - 1.2 m of tail
    // clearance where 3 was intended. Sizing from the largest airframe the LETTER admits, never
    // from a named one, is the rule the taxiway widths and the service road fillet already
    // follow.
    UAircraftType* A320 = NewObject<UAircraftType>(GetTransientPackage());
    UAircraftType::BuildA320(A320);
    UAircraftType* B738 = NewObject<UAircraftType>(GetTransientPackage());
    UAircraftType::BuildB738(B738);

    TestTrue(TEXT("the 737-800 really is the longer of the two"),
        B738->Footprint.TailX < A320->Footprint.TailX);

    UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

    // Nothing in the layout may sit inside the largest admitted airframe's tail clearance.
    const double Aft = B738->Footprint.TailX;
    for (const FServiceBay& Bay : Stand->ServiceBays)
    {
        TestTrue(
            *FString::Printf(TEXT("bay '%s' at %.0f is clear of the longest tail at %.0f"),
                *Bay.AnchorId.ToString(), Bay.Local.X, Aft),
            Bay.Local.X > Aft || FMath::Abs(Bay.Local.Y) > B738->Footprint.Wingspan * 0.5);
    }

    return true;
}

#endif
```

- [ ] **Step 2: Build twice; watch it fail**

Expected: `FServiceBay` undefined, `RequiredExtent` not a member, `BuildB738` may not exist under that name — check `AircraftType.h` and use whatever the second airframe's builder is actually called.

- [ ] **Step 3: Declare the template types**

In `EntityDefinition.h`, ABOVE the doc comment of whatever follows (UHT rejects a `USTRUCT` between a comment and its declaration), replacing `FStandWaypoint` and `EStandWaypointKind`:

```cpp
/**
 * One service vehicle's parking bay, and the three legs that serve it.
 *
 * THREE LEGS, NOT TWO, and the third is not an oversight. A bay is a dead end: the vehicle
 * backs in and then drives out FORWARDS. If it retraced the reverse leg, that curve would have
 * to satisfy the FORWARD limit of 699 uu and the whole 30% that reversing buys would be spent
 * on a path that has to work both ways. It does not retrace it - from the parked pose it leaves
 * along its own curve.
 *
 * AIRFRAME-INDEPENDENT. A bay is paint on concrete, and paint does not move when a different
 * type parks; where a service connects to the AIRCRAFT lives on UAircraftType, because an A320
 * and a 737-800 park here with their doors metres apart.
 */
USTRUCT()
struct AIRSIDE_API FServiceBay
{
	GENERATED_BODY()

	/** Which service this bay serves. Matches an FEntityAnchor::Id on the same definition. */
	UPROPERTY(EditAnywhere) FName AnchorId;

	/** Where the vehicle ends up, and pointing which way, in the entity's local space. */
	UPROPERTY(EditAnywhere) FVector2D Local = FVector2D::ZeroVector;
	UPROPERTY(EditAnywhere) double LocalHeading = 0.0;

	/** Staging past the bay, forwards. Checked against the FORWARD limit. */
	UPROPERTY() FRoutePlan ApproachLeg;

	/** The back-in, which FReverseRun plays. Checked against the REVERSE limit. */
	UPROPERTY() FRoutePlan ReverseLeg;

	/** Bay to onward, forwards. Checked against the FORWARD limit. */
	UPROPERTY() FRoutePlan ExitLeg;
};
```

And on `UEntityDefinition`, in place of `ServiceLane`, the entry, staging and extent fields with the doc comments the spec's "One entry" and "Width is a BAND" sections argue for. State at `EntryLocal` that it MUST meet a road and that placement validates rather than solves, and at `RequiredExtent` that it is derived from the poses and never typed.

- [ ] **Step 4: Author the poses in `BuildCodeCStandFor`**

Replace the `ServiceLane` block. Place, in aircraft-relative coordinates:

- the entry, on the aft edge of the stand, facing into it — aft because a stand sits in a row and the GSE road runs along the back of the row;
- the staging rank, inboard of the entry, with a capacity of at least 2;
- a bay per non-aircraft anchor, beside its anchor and clear of the largest admitted airframe.

Compute `RequiredExtent` from the extremes of everything placed, plus the largest admitted airframe's footprint. **Log the derived positions and the extent** with `UE_LOG(LogAirside, Log, ...)` on the first build and read them before trusting this listing — the poses here are written from the geometry, not run, and that is where this plan is most likely to be wrong.

- [ ] **Step 5: Run, and read the extent**

```
./Tools/Run-AirsideTests.ps1 -Filter Airside.Entities.StandLayout
./Tools/Run-AirsideTests.ps1 -Filter Airside.Entities.StandExtent
```

Both pass. **Report the derived extent in the commit message** so it can be sanity-checked against a real Code C stand: something near 45 × 55 m is right; 60 m means the layout is wrong, not the figure.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(entities): a stand declares a staging rank, a bay per service and one entry"
```

---

## Task 3: The legs, derived and proven

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/EntityDefinition.cpp` (`BuildCodeCStandFor`)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/StandLayoutTest.cpp`

**Interfaces:**
- Consumes: `FSpeedProfile::Build(Points, Airframe, EDriveDirection)`, `WasTighterThanLock()`, `HasSharpVertex()` (piece A); `FReverseRun::Start(Plan, Airframe, Speed)` (piece B).
- Produces: every `FServiceBay`'s three legs and `UEntityDefinition::EntryLeg`, filled and proven.

- [ ] **Step 1: Write the failing test**

Append to `StandLayoutTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FEveryTemplateLegIsDrivableByEveryVehicleTest,
    "Airside.Entities.EveryTemplateLegIsDrivableByEveryVehicle",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FEveryTemplateLegIsDrivableByEveryVehicleTest::RunTest(const FString& Parameters)
{
    // THE PROPERTY THE WHOLE PIECE EXISTS TO GET. Drivability is a property of the TEMPLATE,
    // verified once for every vehicle - not of every placement. Four attempts failed because
    // each derived geometry per stand and so had to re-prove it per stand; placement is now a
    // transform, and a transform preserves curvature.
    //
    // ASKED OF THE AUTHORITIES, never re-derived: forward legs of FSpeedProfile, reverse legs of
    // FReverseRun::Start, which already refuses what it cannot hold.
    UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

    TArray<TPair<FString, FAirframe>> Fleet;
    Fleet.Emplace(TEXT("default vehicle"), UAirsideSettings::ResolveDefaultVehicle());
    const FAirframe Largest = UAirsideSettings::ResolveLargestServiceVehicle();
    if (!FMath::IsNearlyEqual(Largest.Wheelbase(), Fleet[0].Value.Wheelbase(), 0.01))
    {
        Fleet.Emplace(TEXT("largest service vehicle"), Largest);
    }

    auto CheckForward = [this](const FString& Who, const TCHAR* What, const FRoutePlan& Leg,
        const FAirframe& Airframe)
    {
        if (!TestTrue(*FString::Printf(TEXT("%s: %s exists"), *Who, What), Leg.IsValid()))
        {
            return;
        }
        FSpeedProfile Profile;
        Profile.Build(Leg.Polyline, Airframe, EDriveDirection::Forward);
        TestFalse(
            *FString::Printf(TEXT("%s: %s holds the forward limit (tightest %.0f at %.0f)"),
                *Who, What, Profile.GetTightestRadius(), Profile.GetTightestAt()),
            Profile.WasTighterThanLock());
        TestFalse(
            *FString::Printf(TEXT("%s: %s has no instant turn (sharpest %.0f deg)"),
                *Who, What, Profile.GetSharpestDegrees()),
            Profile.HasSharpVertex());
    };

    for (const TPair<FString, FAirframe>& Vehicle : Fleet)
    {
        CheckForward(Vehicle.Key, TEXT("the entry leg"), Stand->EntryLeg, Vehicle.Value);

        for (const FServiceBay& Bay : Stand->ServiceBays)
        {
            const FString Who = FString::Printf(TEXT("%s at '%s'"),
                *Vehicle.Key, *Bay.AnchorId.ToString());

            CheckForward(Who, TEXT("the approach leg"), Bay.ApproachLeg, Vehicle.Value);
            CheckForward(Who, TEXT("the exit leg"), Bay.ExitLeg, Vehicle.Value);

            // THE REVERSE LEG IS ASKED OF THE MANOEUVRE ITSELF, not of a profile built here -
            // FReverseRun::Start is what will actually arm it in play, and it refuses a curve it
            // cannot hold AND a sharp vertex, separately. Anything else is a second evaluator.
            FReverseRun Run;
            TestTrue(
                *FString::Printf(TEXT("%s: the reverse leg arms"), *Who),
                Run.Start(Bay.ReverseLeg, Vehicle.Value, /*InReverseSpeed=*/100.0));
        }
    }

    return true;
}
```

Add `#include "Model/ReverseRun.h"` and `#include "Model/SpeedProfile.h"` to the file.

- [ ] **Step 2: Run it and watch it fail**

Expected: the legs are empty, so `Leg.IsValid()` is false and every assertion reports it. That is the right failure — the poses exist, the geometry does not.

- [ ] **Step 3: Derive the legs**

In `BuildCodeCStandFor`, after the poses, build each leg as an `FRoutePlan` — polyline, `Length`, and **`Result = ERouteResult::Found`**, without which every consumer refuses it at its first guard.

Shape each leg from the radii the vehicles need, using `GuidelineGeom::CornerRunFor` for a corner and `GuidelineGeom::ShiftDeflectionFor` for a lateral shift; both are in `Solve/GuidelineGeom.h` and each is the exact inverse of `TightestRadius`. Size forward legs at `ResolveLargestServiceVehicle().TightestFollowableRadius()` and the reverse leg at `TightestReversibleRadius()`.

**Derive, do not type.** A leg whose radius is a literal will pass this task's test and fail the moment a bigger vehicle is admitted.

- [ ] **Step 4: Run the whole suite**

```
./Tools/Run-AirsideTests.ps1
```

The new test passes. Others may fail where they read the deleted `ServiceLane`; record the exact list in the commit message — Task 4 fixes them.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(entities): the template's legs are derived, and proven for every vehicle"
```

---

## Task 4: Lay the template into the graph

**Files:**
- Rename: `Public/Build/StandLaneBuild.h` → `StandLayoutBuild.h`, `Private/Build/StandLaneBuild.cpp` → `StandLayoutBuild.cpp` (`git mv`)
- Modify: `Private/Build/AnchorLink.cpp` (the call site)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/ServiceLinkTest.cpp`

**Interfaces:**
- Produces: `FStandLayoutBuild::FResult { TSet<FGuidelineNodeId> Nodes; TMap<FEntityInstanceId, TArray<FGuidelineEdgeId>> Layouts; TMap<FEntityInstanceId, FGuidelineNodeId> Entries; int32 LayoutsBuilt; }` — one entry node per stand now, not an array.

- [ ] **Step 1: Write the failing test**

In `ServiceLinkTest.cpp`, replacing `PlacedStandLaneIsOneDrivableCycle`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FPlacedStandLayoutIsWhatTheTemplateSaidTest,
    "Airside.Build.PlacedStandLayoutIsWhatTheTemplateSaid",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlacedStandLayoutIsWhatTheTemplateSaidTest::RunTest(const FString& Parameters)
{
    using namespace ServiceLinkFixture;

    // PLACEMENT IS A TRANSFORM, AND A TRANSFORM PRESERVES CURVATURE. That is the whole claim,
    // so it is measured rather than asserted: place the same stand at two poses and require
    // every laid edge to deliver the same tightest radius as the template's own leg.
    URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
    UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();

    const FEntityInstanceId Origin = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
    const FEntityInstanceId Turned =
        PlaceStand(*Net, *Stand, FVector2D(20000.0, 7000.0), FMath::DegreesToRadians(37.0));

    const FStandLayoutBuild::FResult Built = FStandLayoutBuild::Build(*Net);

    const FAirframe Truck = UAirsideSettings::ResolveLargestServiceVehicle();
    for (const FEntityInstanceId& Placed : { Origin, Turned })
    {
        const TArray<FGuidelineEdgeId>* Laid = Built.Layouts.Find(Placed);
        if (!TestNotNull(TEXT("the stand got a layout"), Laid))
        {
            continue;
        }

        FSpeedProfile Profile;
        for (const FGuidelineEdgeId& Id : *Laid)
        {
            TArray<FVector2D> Points;
            if (!Net->SampleGuideline(Id, Points))
            {
                continue;
            }
            Profile.Build(Points, Truck, EDriveDirection::Forward);

            // A reverse leg is legitimately tighter than the forward limit, so only edges the
            // vehicle drives FORWARDS are judged by it. The reverse legs were proven by
            // FReverseRun::Start at template-build time and are not re-judged here by the
            // wrong rule.
            const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
            if (Edge != nullptr && Edge->bReverseLeg)
            {
                continue;
            }
            TestFalse(
                *FString::Printf(TEXT("a laid forward edge holds the limit (tightest %.0f)"),
                    Profile.GetTightestRadius()),
                Profile.WasTighterThanLock());
        }

        // THE ENTRY IS ONE-WAY. EGuidelineDir already exists and URoadNetwork honours it when
        // it walks outgoing edges, so the search cannot choose to arrive the wrong way round -
        // which is the 175 degree on-the-spot reversal the oracle found, made unrepresentable.
        const FGuidelineNodeId* Entry = Built.Entries.Find(Placed);
        if (TestNotNull(TEXT("the stand declared an entry"), Entry))
        {
            const FGuidelineNode* Node = Net->GetGuidelineNode(*Entry);
            bool bFoundDirected = false;
            if (Node != nullptr)
            {
                for (const FGuidelineEdgeId& Id : Node->Incident)
                {
                    const FGuidelineEdge* Edge = Net->GetGuidelineEdge(Id);
                    bFoundDirected |= Edge != nullptr
                        && Edge->Direction != EGuidelineDir::Bidirectional;
                }
            }
            TestTrue(TEXT("the entry is a one-way edge"), bFoundDirected);
        }
    }

    return true;
}
```

- [ ] **Step 2: Run it; watch it fail**

Expected: `FStandLayoutBuild` undefined, and `bReverseLeg` is not a member of `FGuidelineEdge`.

- [ ] **Step 3: Move the builder and gut the lane**

```bash
git mv Plugins/Airside/Source/Airside/Public/Build/StandLaneBuild.h \
       Plugins/Airside/Source/Airside/Public/Build/StandLayoutBuild.h
git mv Plugins/Airside/Source/Airside/Private/Build/StandLaneBuild.cpp \
       Plugins/Airside/Source/Airside/Private/Build/StandLayoutBuild.cpp
```

Then: `FStandLaneBuild` → `FStandLayoutBuild`. **Delete `MeasureLane`, `RecoverEntries`, `NodeNear`, the corner measurement, the proportional clamp and the bend laying** — the whole apparatus for solving a lane's shape. The template arrives solved; this transforms and lays it.

Add `FGuidelineEdge::bReverseLeg`, so a consumer can tell which edges are judged by which limit, with a comment saying that a reverse leg is legitimately tighter than the forward limit and that judging it by the wrong rule is a false refusal.

Set `Edge.Direction` from the entry's heading — `AToB` or `BToA` according to which way the edge was laid — and keep `StandGeometryOwner` on every laid edge, because `URoadNetwork::IsServiceNodeConnected` walks it and `FuelService.cpp:131` and `:472` are its callers.

**Verify before relying on it:** `RoadNetwork.cpp:776-778` honours `Direction` when walking outgoing edges. Confirm that function is what `RouteSearch` uses; if the search reaches edges another way, say so rather than assuming the one-way holds.

- [ ] **Step 4: Run the suite; record what is still red**

Link tests will fail until Task 5. Record the list.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(build): a stand's layout is transformed into the graph, not solved there"
```

---

## Task 5: The entry meets a road, or the stand is refused

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Build/AnchorLink.cpp` (the entry link block)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/ServiceLinkTest.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FStandEntryMustMeetARoadTest,
    "Airside.Build.StandEntryMustMeetARoad",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandEntryMustMeetARoadTest::RunTest(const FString& Parameters)
{
    using namespace ServiceLinkFixture;

    // PLACEMENT VALIDATES; IT DOES NOT SOLVE. With the entry floating, one curve was still
    // solved fresh on every placement - the last chance in the design to produce something
    // undrivable. The stand declares where it may be entered and a road either meets it or the
    // stand is refused, with a reason a player can act on.
    {
        URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
        UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
        const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);
        FAnchorLink::Build(*Net);

        TestFalse(TEXT("a stand whose entry meets no road is not connected"),
            Net->IsServiceNodeConnected(AnchorNode(*Net, Placed, TEXT("HydrantPit"))));
    }

    {
        URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
        UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
        const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

        // A road across the stand's entry. Its position comes from the template rather than
        // being typed, so moving the entry moves this fixture with it.
        const FVector2D Entry = Stand->EntryLocal;
        FGuidelineNodeId East;
        Lay(*Net, FVector2D(Entry.X - 6000.0, Entry.Y), FVector2D(Entry.X + 6000.0, Entry.Y),
            ETraversalClass::GroundVehicle, East);

        FAnchorLink::Build(*Net);

        TestTrue(TEXT("a stand whose entry meets a road is connected"),
            Net->IsServiceNodeConnected(AnchorNode(*Net, Placed, TEXT("HydrantPit"))));
    }

    return true;
}
```

- [ ] **Step 2: Run it; watch the second case fail**

- [ ] **Step 3: Replace the entry link**

Delete whatever remains of the per-side and ray-based entry search. In its place: for each stand's declared entry node, find a road within `ServiceLinkRadius` **at the entry's own position**, and join them with a short straight. Refuse — and `UE_LOG(LogAirside, Warning, ...)` naming the stand and the distance — when none is within reach.

Because the entry is fixed and the join is straight, there is no curve to check here. If the implementation finds itself building an arc, stop: that is per-placement geometry coming back, and it is the thing this task exists to remove.

- [ ] **Step 4: Run the suite**

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(build): a stand's entry must meet a road, and says so when it does not"
```

---

## Task 6: A bay is claimed, and the rank holds the queue

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h` (bay claim state)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/FuelService.cpp` (claim before departing staging)
- Modify: `Plugins/Airside/Source/AirsideTests/Private/StandLayoutTest.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FASecondTruckWaitsAtStagingTest,
    "Airside.Model.ASecondTruckWaitsAtStaging",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FASecondTruckWaitsAtStagingTest::RunTest(const FString& Parameters)
{
    // "SO THE VEHICLES DON'T PILE UP ON EACH OTHER". A bay is claimed before a vehicle leaves
    // the rank; an occupied bay means it waits AT staging rather than setting off and arriving
    // to find the space taken. Waiting on the road outside instead would block the road, which
    // is why staging is a rank with a capacity and not a point.
    URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
    UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
    const FEntityInstanceId Placed = PlaceStand(*Net, *Stand, FVector2D::ZeroVector, 0.0);

    const FName Hydrant(TEXT("HydrantPit"));

    TestTrue(TEXT("the first truck claims the fuel bay"),
        Net->ClaimServiceBay(Placed, Hydrant, /*AgentId=*/1));
    TestFalse(TEXT("a second truck cannot claim the same bay"),
        Net->ClaimServiceBay(Placed, Hydrant, /*AgentId=*/2));

    Net->ReleaseServiceBay(Placed, Hydrant, /*AgentId=*/1);

    TestTrue(TEXT("and can once the first has left"),
        Net->ClaimServiceBay(Placed, Hydrant, /*AgentId=*/2));

    // A claim is per BAY, not per stand - two different services are worked at once, which is
    // the entire reason each has its own bay.
    TestTrue(TEXT("a different service's bay is unaffected"),
        Net->ClaimServiceBay(Placed, FName(TEXT("EquipmentFwd")), /*AgentId=*/3));

    return true;
}
```

- [ ] **Step 2: Run it; watch it fail**

- [ ] **Step 3: Implement the claim**

Add the claim state to `FEntityInstance` and the two methods to `URoadNetwork`. **Look first at the existing claim machinery** — `TrafficClaims.h`, and how a stand itself is reserved — and extend that rather than inventing a second notion of "something is in the way". If a bay genuinely does not fit that machinery, say so in the commit message rather than forcing it.

Then make the fuel service claim its bay before leaving the rank, and release it on departure.

- [ ] **Step 4: Run the suite**

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(model): a service bay is claimed, so a second vehicle waits at the rank"
```

---

## Task 7: Content, and the acceptance test

**Files:**
- Modify: `Tools/Python/build_stand_asset.py`
- Modify: `Content/Entities/DA_Stand_CodeC.uasset` (re-authored by the script)

- [ ] **Step 1: Teach the script the new readback**

`build_code_c_stand` does the work; the script's job is the readback, which is the only place an invisible layout can be inspected. Replace the `service_lane` loop with one that logs the entry, the staging rank and each bay with its anchor id — and the `MARKER:` prefix every line in that file uses.

- [ ] **Step 2: Build with the editor closed, then re-author**

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
```

Then run the script headlessly. **Check the .uasset's sha1 or mtime on disk afterwards** — a headless save in this project has reported success while writing nothing.

- [ ] **Step 3: The authoritative suite run**

```
./Tools/Run-AirsideTests.ps1
```

**`Airside.Model.Traffic.TruckDrivesTheWholeRouteToTheHydrant` must now PASS.** Red since piece A at `sharpest 175 deg at 9489`, it is this plan's acceptance criterion and the only test that says the redesign worked. If it is still red, that is the finding — report it with the figures; do not weaken it.

- [ ] **Step 4: Look at the map**

A green suite is not evidence for a geometry change. In PIE, with `Saved/Logs/AirportMgr.log` open:

1. Send a fuel truck from the depot to a stand. Its `Speed profile:` line must read `lateral accel`, not `TIGHTER THAN THE STEERING LOCK`.
2. No `Route asks for R=...` warnings on that route.
3. `python Tools/Mcp.py shot out.png` with the truck backed into its bay — it should sit **in** the bay, square, working end toward the aircraft.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(content): the Code C stand ships its layout template"
```

---

## Self-review notes

**Spec coverage.** `IcaoCode` sizes → Task 1. Template poses → Task 2. Legs derived and proven → Task 3. Placement as transform, directed entry → Task 4. Entry meets a road, refusal → Task 5. Bay claims and the rank → Task 6. Content and verification → Task 7. The airframe-independence ruling is asserted in Task 2's second test; the extra-width-inserts-straight rule has **no task**, because nothing varies width until piece D — noted here so its absence is deliberate rather than missed.

**Where this plan is most likely to be wrong:** Task 2's pose positions are written from the geometry and have not been run. Step 4 says to log the derived positions and read them. Expect to move them; that is the task working, not failing.

**A second place to watch:** Task 4 asserts the search honours `EGuidelineDir`. `RoadNetwork.cpp:776-778` does, but whether `RouteSearch` reaches edges only through that function is unverified. If it does not, the one-way entry is not enforced and Task 4's last assertion passes while the property does not hold — check the consumer before trusting it.
