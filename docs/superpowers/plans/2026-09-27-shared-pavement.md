# Shared Pavement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One pavement scale (`EPavement`) shared by runways, roads, taxiways and stands: stands admit by surface as well as size, every buildable prices and bills by its pavement through one factor, and Code A stands merge into B.

**Architecture:** `Model/Pavement.h` owns the scale, the comparison (`FPavementCheck`, a Value Object both admission judges hold) and the rate factor. `FBuildQuote` becomes a list of `FBuildLine`s; the factor is applied in `FBuildLine::Amount` only, so no buildable kind carries its own pricing rule. `StandAdmission` mirrors `RunwayAdmission` and replaces the span-only `IcaoCode::StandAdmits` at its callers.

**Tech Stack:** UE 5.8.2 C++, `Airside` + `AirportOps` plugins, UE automation tests, `Tools/Check-Architecture.ps1`.

**Spec:** `docs/superpowers/specs/2026-09-27-shared-pavement-design.md` - read it first; this plan argues from it.

## Global Constraints

- Worktree `C:\repos\airportmgr2-shared-pavement`, branch `feature/shared-pavement`. The editor may be open on `C:\repos\AirportMgr2`: every build passes `-NoHotReloadFromIDE`, every test run passes `-Project`.
- Build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-shared-pavement\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE`
- Test: `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\airportmgr2-shared-pavement\AirportMgr.uproject" -Filter <prefix>`. Read its `N test(s) run, N failed, N crashed` line; never the exit code.
- A NEW test .cpp needs two builds (the first says Succeeded without compiling it). Prefer adding cases to an existing test file; where a new file is named below, build twice and confirm the test appears in the run count.
- Factors: Grass 0.4, Tarmac 1.0, Concrete 1.4, Reinforced 1.8.
- `EPavement` order is `Grass, Tarmac, Concrete, Reinforced, Count` - strength by ordering. Every stored `EPavement` field defaults explicitly to `Tarmac`; `MinimumPavement` defaults to `Grass`.
- Refactor contract (CLAUDE.md): `UE_LOG(` count and comment-line count in touched files must not fall; every WHY comment travels with its code; every `UFUNCTION`/interface virtual stays reachable.
- Every commit builds. A commit that could not be built says "unbuilt" in its message.
- Commit messages: concise, end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- `// ENFORCED BY:` on any new comment claiming a fact about other code, naming the test or lint rule.

## Open Decisions

- **Task 8 Step 7 - grass stand pads need a multi-material apron layer** (found 2026-09-27 auditing #356): pads draw on the one-material apron layer, grass roads by material slot. Recommended: the apron layer resolves slots through the road layer's existing `EffectiveMaterialSet`; tarmac pads and bare aprons unchanged. Alternative: pads move onto the road layer (loses `ApronZOffset`, so a pad over a road z-fights). Needs the user's yes before Task 8 Step 7 runs; Tasks 0-7 do not depend on it.

## Review Focus

1. **A demolish refund after the line change.** `Credit` must scrap per line at today's price; a stand's pad line (null Source) must still refund. Test in Task 5.
2. **Upkeep on a grass runway and a grass road.** A runway's pavement is `FRunwayFacts::Surface`, a road's is the segment's; reading the wrong one bills a grass runway as tarmac. Test in Task 6.
3. **An arrival whose only stand is big enough but grass.** The refusal must tell the player to pave it, not to draw a bigger stand or build a taxiway. Test in Task 9.
4. **A drag between A's old floor and B's floor** (e.g. 50 x 36 m). It was a Code A stand; now it is refused as too small, and the refusal must name the depth. Test in Task 7.
5. **The afford gate while hovering a grass stand.** `WhyStandRefused` must price with the tool's current pavement, or the ghost says "cannot afford" for a grass stand the click would have paid for. Test in Task 8.

---

### Task 0: Rebase onto the landed grass branch and record baselines

**Files:** none changed; notes go in the PR body draft `C:\Users\daren\AppData\Local\Temp\claude\C--repos-AirportMgr2\ed7bce6f-53ee-4b2b-99f5-23014a349e5e\scratchpad\pavement-baseline.md`.

- [x] **Step 1: Confirm the grass branch is on main.** DONE 2026-09-27: `f20add83` (#356).
  Run: `git -C C:\repos\airportmgr2-shared-pavement fetch origin; git -C C:\repos\airportmgr2-shared-pavement log origin/main --oneline -20`
  Expected: a commit merging `feature/road-grass-surface`. If absent, STOP - this plan does not start before it lands (user ruling 2026-09-27).

- [x] **Step 2: Rebase.** DONE 2026-09-27, clean.
  Run: `git -C C:\repos\airportmgr2-shared-pavement rebase origin/main`
  Expected: clean (this branch holds only docs).

- [x] **Step 3: Record what the grass branch actually shipped.** DONE 2026-09-27 - findings are Task 4's table; grass is drawn by MATERIAL SLOT (`SurfaceSlotFor` -> the grass runway slot), which decides Task 8 Step 7. The plan below was written against its UNCOMMITTED 2026-09-27 state. Grep and write into the baseline file:
  ```
  grep -rn "enum class ERoadSurface" -A8 Plugins/Airside/Source/Airside/Public
  grep -rn "ERoadSurface Surface\|\bSurface\b.*ERoadSurface" Plugins/Airside/Source/Airside/Public/Model/RoadSegment*.h Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h
  grep -rn "GrassRateFactor\|SurfaceRateFactor\|RoadSurfacePavement\|RoadSurfaceName" Plugins Source --include=*.h --include=*.cpp
  grep -rn "ERoadSurface" Plugins Source --include=*.h --include=*.cpp -l
  grep -rn "Grass" Plugins/Airside/Source/Airside/Private/Build/RoadMeshBuilder.cpp
  ```
  The last grep answers HOW a grass road is drawn (UV1 channel, vertex colour, or material section - see memory `road-appearance-comes-from-uv1-not-materials`). Task 8 Step 7 reuses exactly that mechanism for a grass stand pad. If any name in Tasks 4 and 6 differs from what shipped, use the shipped name and note the difference.

- [ ] **Step 4: Baseline build and test.**
  Run the Build command (Global Constraints), then the full test run (no `-Filter`).
  Expected: `Result: Succeeded`; record the `N test(s) run, N failed, N crashed` line in the baseline file. Pre-existing failures are recorded, not fixed.

- [ ] **Step 5: Baseline counts for the refactor contract.**
  ```
  grep -rc "UE_LOG(" Plugins/Airside/Source/Airside Plugins/AirportOps/Source/AirportOps --include=*.cpp | awk -F: '{s+=$2} END {print s}'
  ```
  Record the number. Also capture every aircraft's current runway need (Task 3 asserts against it) - add temporarily to `Plugins/Airside/Source/AirsideTests/Private/AirsideContentTest.cpp`, run it, record the log lines, then `git checkout` the file:
  ```cpp
  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBaselineMinimumSurfaceTest, "Airside.Content.BaselineMinimumSurface",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FBaselineMinimumSurfaceTest::RunTest(const FString&)
  {
      // NO RESOLVER LISTS THE FLEET (AirsideSettings.h:316 - there is no AircraftTypes array),
      // so load every DA_Aircraft_* by path: A320, B738, Plane1-16 on 2026-09-27
      // (`ls Content/Entities | grep DA_Aircraft` - re-list at execution time).
      for (const TCHAR* Name : { TEXT("DA_Aircraft_A320"), TEXT("DA_Aircraft_B738"), TEXT("DA_Aircraft_Plane1") /* , ...Plane2-16 */ })
      {
          const UAircraftType* Type = LoadObject<UAircraftType>(nullptr,
              *FString::Printf(TEXT("/Game/Entities/%s.%s"), Name, Name));
          UE_LOG(LogTemp, Display, TEXT("BASELINE %s %s"), Name,
              Type != nullptr ? RunwaySurfaceName(Type->Requirements.MinimumSurface) : TEXT("<missing>"));
      }
      return true;
  }
  ```
  Write out all eighteen names; `/* , ...Plane2-16 */` is shorthand in this plan only.
  Also capture the tarmac prices Task 5 pins, in the same temporary test:
  ```cpp
  const URoadProfile* Taxi = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
  UE_LOG(LogTemp, Display, TEXT("BASELINE segment %.6f"), BuildCost::ForSegment(*Taxi, 50000.0).BaseAmount);
  const TArray<FVector2D> Pad = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
  UE_LOG(LogTemp, Display, TEXT("BASELINE apron %.6f"), BuildCost::ForApron(Pad, 10.0).BaseAmount);
  UE_LOG(LogTemp, Display, TEXT("BASELINE entity %.6f"), BuildCost::ForEntity(*UEntityDefinition::MakeStandTransient(EIcaoCode::B)).BaseAmount);
  ```

---

### Task 1: Rename `ERunwaySurface` to `EPavement` (pure rename)

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Model/Pavement.h`, `Plugins/Airside/Source/Airside/Private/Model/Pavement.cpp`
- Modify: `Public/Model/RunwayFacts.h`, `Private/Model/RunwayFacts.cpp`, and every file from `grep -rln "ERunwaySurface\|RunwaySurfaceName\|RunwayMaterialSlot" Plugins Source` (25 on 2026-09-27, including tests and `Source/AirportMgr/BuildBarWidgetTest.cpp`)
- Modify: `Config/DefaultEngine.ini` (`[CoreRedirects]`)

**Interfaces:**
- Produces: `enum class EPavement : uint8 { Grass, Tarmac, Concrete, Reinforced, Count }`; `inline constexpr int32 PavementMaterialSlotCount = 3;`; `namespace Pavement { const TCHAR* Name(EPavement); int32 MaterialSlot(EPavement); }`

- [ ] **Step 1: Create `Model/Pavement.h`** by MOVING the enum and its two helpers out of `RunwayFacts.h` - every doc comment moves with its declaration, reworded only where it says "runway" of the whole scale:
  ```cpp
  #pragma once

  #include "CoreMinimal.h"
  #include "Pavement.generated.h"

  /**
   * What ground is paved with - a runway, a road or taxiway, a stand's pad. ORDERED: strength
   * as well as look, so an aircraft names the weakest pavement it may use and admission
   * compares with >=.
   *
   * Pavement strength is the scale itself rather than a separate PCN figure (spec 2026-09-07
   * §1): four steps is what a player can read off the ground, and a classification number
   * would be a second axis nothing in the game varies independently.
   *
   * ONE SCALE FOR EVERY BUILDABLE since 2026-09-27 (was ERunwaySurface; spec
   * 2026-09-27-shared-pavement). A second enum per kind is how roads came to have a surface
   * scale runway admission could not read.
   * ENFORCED BY: Check-Architecture rule 23 (one pavement enum)
   */
  UENUM(BlueprintType)
  enum class EPavement : uint8
  {
      Grass,
      Tarmac,
      Concrete,
      Reinforced,
      /** Sentinel, never a real pavement - sizes tables and % cycling instead of retyping 4. */
      Count UMETA(Hidden),
  };

  /** <moved verbatim from RunwayFacts.h: RunwayMaterialSlotCount's comment> */
  inline constexpr int32 PavementMaterialSlotCount = 3;

  namespace Pavement
  {
      /** Lower case, for a refusal sentence or a tool label. One spelling each. */
      AIRSIDE_API const TCHAR* Name(EPavement P);

      /** <moved verbatim from RunwayFacts.h: RunwayMaterialSlot's comment> */
      AIRSIDE_API int32 MaterialSlot(EPavement P);
  }
  ```
- [ ] **Step 2: Move the bodies** of `RunwaySurfaceName` and `RunwayMaterialSlot` from `RunwayFacts.cpp` into `Pavement.cpp` as `Pavement::Name` / `Pavement::MaterialSlot`, switch cases renamed `EPavement::`, comments kept. `RunwayFacts.h` includes `Model/Pavement.h`; `FRunwayFacts::Surface` and `FRunwayRequirements::MinimumSurface` change type to `EPavement` (field names unchanged in this task).
- [ ] **Step 3: Rename every use.** `ERunwaySurface::` -> `EPavement::`, `RunwaySurfaceName(` -> `Pavement::Name(`, `RunwayMaterialSlot(` -> `Pavement::MaterialSlot(`, `RunwayMaterialSlotCount` -> `PavementMaterialSlotCount`. Use the Edit tool or a Python script per file - never sed (Windows paths; memory `avoid-sed-for-windows-paths`). Comments that NAME the old symbol are updated to the new one.
- [ ] **Step 4: Add the redirect** to `Config/DefaultEngine.ini`:
  ```ini
  [CoreRedirects]
  +EnumRedirects=(OldName="/Script/Airside.ERunwaySurface",NewName="/Script/Airside.EPavement")
  ```
  (If a `[CoreRedirects]` section exists, append the line there.)
- [ ] **Step 5: Build.** Expected: `Result: Succeeded`, and `grep -rn "ERunwaySurface\|RunwaySurfaceName\|RunwayMaterialSlot" Plugins Source --include=*.h --include=*.cpp` returns only comments that explain the rename.
- [ ] **Step 6: Run the runway tests.** `-Filter Airside.Model.Runway` then `-Filter Airside.Tool.Runway` then `-Filter AirportMgr`. Expected: same pass counts as the Task 0 baseline for those prefixes.
- [ ] **Step 7: Prove the level still loads its runway surfaces.** Run `-Filter Airside.Content` and `-Filter StarterMap` (StarterMapProbeTest loads M_Starter). Expected: pass. A failure here means the redirect is wrong.
- [ ] **Step 8: Commit.** `refactor(pavement): ERunwaySurface becomes EPavement, one scale for every buildable`

---

### Task 2: `FPavementCheck` and `Pavement::RateFactor`; runway admission uses the check

**Files:**
- Modify: `Public/Model/Pavement.h`, `Private/Model/Pavement.cpp`
- Modify: `Public/Model/RunwayAdmission.h:33-54`, `Private/Model/RunwayAdmission.cpp:23-58,97-106`
- Test: `Plugins/Airside/Source/AirsideTests/Private/RunwayFactsTest.cpp` (add cases; no new file)

**Interfaces:**
- Consumes: `EPavement`, `Pavement::Name` (Task 1)
- Produces:
  ```cpp
  USTRUCT() struct AIRSIDE_API FPavementCheck { EPavement Have = EPavement::Tarmac; EPavement Need = EPavement::Grass; bool Passes() const; };
  namespace Pavement {
      FPavementCheck Judge(EPavement Have, EPavement Need);
      FString Describe(const FPavementCheck& Check);   // empty when it passes
      double RateFactor(EPavement P);
  }
  // FRunwayAdmission gains: UPROPERTY() FPavementCheck Pavement;
  ```

- [ ] **Step 1: Capture the runway refusal sentence from the CURRENT code** (before touching it) - it is the byte-for-byte target:
  `the surface is grass; this aircraft needs tarmac`
  (from `RunwayAdmission.cpp:103`: `"the surface is %s; this aircraft needs %s"`).
- [ ] **Step 2: Write the failing tests** in `RunwayFactsTest.cpp`:
  ```cpp
  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPavementOrderIsStrengthTest, "Airside.Model.Pavement.OrderIsStrength",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FPavementOrderIsStrengthTest::RunTest(const FString&)
  {
      // ALL SIXTEEN PAIRS, because the rule is the ordering itself: a hand-picked pair or two
      // would pass against a Judge that special-cased grass.
      for (uint8 Have = 0; Have < static_cast<uint8>(EPavement::Count); ++Have)
      {
          for (uint8 Need = 0; Need < static_cast<uint8>(EPavement::Count); ++Need)
          {
              const FPavementCheck Check = Pavement::Judge(static_cast<EPavement>(Have), static_cast<EPavement>(Need));
              TestEqual(FString::Printf(TEXT("%s ground, %s needed: passes iff strong enough"),
                  Pavement::Name(static_cast<EPavement>(Have)), Pavement::Name(static_cast<EPavement>(Need))),
                  Check.Passes(), Have >= Need);
              TestEqual(TEXT("and a passing check has no sentence"), Check.Passes(), Pavement::Describe(Check).IsEmpty());
          }
      }
      return true;
  }

  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPavementDescribeMatchesRunwaySentenceTest, "Airside.Model.Pavement.DescribeMatchesRunwaySentence",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FPavementDescribeMatchesRunwaySentenceTest::RunTest(const FString&)
  {
      // BYTE-IDENTICAL to the sentence RunwayAdmission::Describe printed before the check moved
      // here (captured from main 2026-09-27) - the move must not change what the player reads.
      TestEqual(TEXT("the runway's refusal sentence survives the move"),
          Pavement::Describe(Pavement::Judge(EPavement::Grass, EPavement::Tarmac)),
          FString(TEXT("the surface is grass; this aircraft needs tarmac")));
      return true;
  }

  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPavementRateFactorTest, "Airside.Model.Pavement.RateFactor",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FPavementRateFactorTest::RunTest(const FString&)
  {
      // TARMAC IS 1 so every price authored before pavement mattered is unchanged; the others
      // are the spec's first guesses and ordered like the scale - stronger costs more.
      TestEqual(TEXT("grass"), Pavement::RateFactor(EPavement::Grass), 0.4);
      TestEqual(TEXT("tarmac is the authored rate itself"), Pavement::RateFactor(EPavement::Tarmac), 1.0);
      TestEqual(TEXT("concrete"), Pavement::RateFactor(EPavement::Concrete), 1.4);
      TestEqual(TEXT("reinforced"), Pavement::RateFactor(EPavement::Reinforced), 1.8);
      return true;
  }
  ```
- [ ] **Step 3: Build; run `-Filter Airside.Model.Pavement`.** Expected: compile FAILURE naming `FPavementCheck` / `Pavement::Judge` (the tests are the failing state).
- [ ] **Step 4: Implement** in `Pavement.h` (after the namespace's existing two functions, and the USTRUCT above the namespace):
  ```cpp
  /**
   * One pavement comparison with the two figures it was made from, so the sentence can be
   * written from the decision - FRunwayAdmission's rule for Describe.
   *
   * A VALUE OBJECT, the one piece runway and stand admission share (spec 2026-09-27 §1). A
   * Specification chain of rule objects was rejected: each refusal carries different figures
   * for its sentence, USTRUCT plans cannot hold polymorphic rules, and two facilities with six
   * rules did not pay for it. Revisit at a third facility.
   */
  USTRUCT()
  struct AIRSIDE_API FPavementCheck
  {
      GENERATED_BODY()

      /** The ground's. */
      UPROPERTY() EPavement Have = EPavement::Tarmac;

      /** The weakest the aircraft may use. */
      UPROPERTY() EPavement Need = EPavement::Grass;

      bool Passes() const { return Have >= Need; }
  };
  ```
  and in the namespace:
  ```cpp
  AIRSIDE_API FPavementCheck Judge(EPavement Have, EPavement Need);

  /** "the surface is grass; this aircraft needs tarmac". Empty when the check passes. */
  AIRSIDE_API FString Describe(const FPavementCheck& Check);

  /**
   * What building or owning a thing on P costs, as a multiple of its authored rate - build AND
   * upkeep, for every buildable. THE ONE TABLE; FBuildLine::Amount and BuildCost's upkeep are
   * its only readers. First guesses 2026-09-27: grass is levelled ground and seed, no base
   * course; concrete and reinforced carry heavier slabs.
   * ENFORCED BY: Check-Architecture rule 4 row 'Pavement::RateFactor'
   */
  AIRSIDE_API double RateFactor(EPavement P);
  ```
  `Pavement.cpp`:
  ```cpp
  FPavementCheck Pavement::Judge(EPavement Have, EPavement Need)
  {
      FPavementCheck Out;
      Out.Have = Have;
      Out.Need = Need;
      return Out;
  }

  FString Pavement::Describe(const FPavementCheck& Check)
  {
      return Check.Passes() ? FString()
          : FString::Printf(TEXT("the surface is %s; this aircraft needs %s"), Name(Check.Have), Name(Check.Need));
  }

  double Pavement::RateFactor(EPavement P)
  {
      switch (P)
      {
      case EPavement::Grass:      return 0.4;
      case EPavement::Tarmac:     return 1.0;
      case EPavement::Concrete:   return 1.4;
      case EPavement::Reinforced: return 1.8;
      default:                    break;
      }
      // An out-of-range byte from a bad save: bill at the authored rate rather than at zero,
      // because a zero factor would build anything for free.
      return 1.0;
  }
  ```
- [ ] **Step 5: Route runway admission through it.** `FRunwayAdmission` gains, beside `Facts`:
  ```cpp
  /** The surface comparison - see FPavementCheck. Written by Judge whatever the verdict. */
  UPROPERTY() FPavementCheck Pavement;
  ```
  In `RunwayAdmission::Judge`, before the `if` chain: `Out.Pavement = Pavement::Judge(Facts.Surface, Airframe.Requirements.MinimumSurface);` and the first branch becomes `if (!Out.Pavement.Passes())`. Keep the "Both scales are ORDERED" comment, reworded to name `FPavementCheck::Passes`. In `Describe`, the `Surface` case becomes `return Pavement::Describe(Admission.Pavement);`.
- [ ] **Step 6: Build; run `-Filter Airside.Model.Pavement` and `-Filter Airside.Model.RunwayAdmission`.** Expected: all pass (RunwayAdmission's existing `Contains("grass")`/`Contains("tarmac")` assertions still hold).
- [ ] **Step 7: Commit.** `feat(pavement): FPavementCheck value object and rate factor; runway admission shares it`

---

### Task 3: `MinimumPavement` moves to the aircraft type

**Files:**
- Modify: `Public/Model/RunwayFacts.h:88-108` (`FRunwayRequirements`), `Public/Model/Airframe.h:504-505`, `Public/Entities/AircraftType.h:171-177,201-225`, `Private/Entities/AircraftType.cpp:481-502`, `Private/Model/RunwayAdmission.cpp`, `Config/DefaultEngine.ini`
- Modify: every `Requirements.MinimumSurface` user from `grep -rn "MinimumSurface" Plugins Source` - since #356 that includes the five `NeedsSurface(...Requirements.MinimumSurface)` sites (ArrivalPlanner, DeparturePlanner x2, PushbackPlanner, GroundTrafficRebuild), which Task 4 then retypes
- Modify (assets): the nine `Content/Entities/DA_Aircraft_*.uasset` that store it
- Test: `Plugins/Airside/Source/AirsideTests/Private/AirsideContentTest.cpp`

**Interfaces:**
- Produces: `UAircraftType::MinimumPavement` (`UPROPERTY(EditAnywhere) EPavement MinimumPavement = EPavement::Grass;`), `FAirframe::MinimumPavement` (same type/default), copied in `UAircraftType::Airframe()`.

- [ ] **Step 1: Write the failing test** in `AirsideContentTest.cpp`, with the Task 0 Step 5 baseline table typed in:
  ```cpp
  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAircraftMinimumPavementMigratedTest, "Airside.Content.AircraftMinimumPavementMigrated",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FAircraftMinimumPavementMigratedTest::RunTest(const FString&)
  {
      // THE VALUES EACH ASSET HELD BEFORE THE FIELD MOVED, captured from main 2026-09-27
      // (Task 0). A migration that silently reset every type to grass would admit an airliner
      // to a grass strip and pass every test that only checks the Piper.
      const TMap<FString, EPavement> Before = {
          // one line per BASELINE log line, e.g. { TEXT("DA_Aircraft_Plane3"), EPavement::Tarmac },
      };
      int32 Seen = 0;
      for (const TPair<FString, EPavement>& Row : Before)
      {
          const UAircraftType* Type = LoadObject<UAircraftType>(nullptr,
              *FString::Printf(TEXT("/Game/Entities/%s.%s"), *Row.Key, *Row.Key));
          if (const EPavement* Expected = Type != nullptr ? &Row.Value : nullptr)
          {
              ++Seen;
              TestEqual(FString::Printf(TEXT("%s keeps the pavement it needed"), *Type->GetName()),
                  Type->MinimumPavement, *Expected);
              TestEqual(TEXT("and carries it into the airframe admission reads"), Type->Airframe().MinimumPavement, *Expected);
          }
      }
      TestEqual(TEXT("every baseline type loaded - a moved asset would otherwise skip silently"), Seen, Before.Num());
      return true;
  }
  ```
  Fill `Before` from the baseline log - every line, no omissions.
- [ ] **Step 2: Build; run it.** Expected: compile FAILURE (`MinimumPavement` is not a member).
- [ ] **Step 3: Move the field.**
  - `FRunwayRequirements`: `MinimumSurface` becomes
    ```cpp
    /** MOVED to UAircraftType::MinimumPavement 2026-09-27 - read only by UAircraftType::PostLoad
     *  to migrate assets saved before. Deleted once every DA_Aircraft_* is resaved (this task). */
    UPROPERTY() EPavement MinimumSurface_DEPRECATED = EPavement::Grass;
    ```
    and `Config/DefaultEngine.ini` `[CoreRedirects]` gains
    `+PropertyRedirects=(OldName="/Script/Airside.RunwayRequirements.MinimumSurface",NewName="/Script/Airside.RunwayRequirements.MinimumSurface_DEPRECATED")`
  - `UAircraftType` (beside `Requirements`, with a comment saying it is not a runway-only need - a stand reads it too):
    ```cpp
    /**
     * The weakest pavement this type may use - runway AND stand (spec 2026-09-27). Left
     * FRunwayRequirements because a stand check reading a field named for runways is the
     * drift this moved to stop.
     */
    UPROPERTY(EditAnywhere) EPavement MinimumPavement = EPavement::Grass;

    virtual void PostLoad() override;
    ```
  - `FAirframe`: `UPROPERTY(EditAnywhere) EPavement MinimumPavement = EPavement::Grass;` beside `Requirements`, and `Airframe()` sets `Out.MinimumPavement = MinimumPavement;`.
  - `AircraftType.cpp`:
    ```cpp
    void UAircraftType::PostLoad()
    {
        Super::PostLoad();
        // ONE-TIME MIGRATION (2026-09-27): an asset saved before MinimumPavement existed holds
        // its need in the deprecated runway field. Grass is both fields' default, so a
        // non-grass deprecated value is the only case that carries information.
        if (Requirements.MinimumSurface_DEPRECATED != EPavement::Grass && MinimumPavement == EPavement::Grass)
        {
            MinimumPavement = Requirements.MinimumSurface_DEPRECATED;
            UE_LOG(LogAirside, Log, TEXT("AircraftType %s: MinimumPavement migrated to %s"),
                *GetName(), Pavement::Name(MinimumPavement));
        }
    }
    ```
    (`LogAirside` - check the include the file already uses for its logging; `grep -n "UE_LOG" Private/Entities/AircraftType.cpp`.)
  - `PiperMeridianRequirements()` no longer sets the surface; the Piper factory sets `Type->MinimumPavement = EPavement::Grass;` with the existing "turboprop single operates off grass" comment moved beside it.
  - `RunwayAdmission::Judge`: `Pavement::Judge(Facts.Surface, Airframe.MinimumPavement)`.
  - Every other `Requirements.MinimumSurface` reader/writer (tests included) becomes `MinimumPavement` on the airframe or type.
- [ ] **Step 4: Build; run `-Filter Airside.Content.AircraftMinimumPavementMigrated` and `-Filter Airside.Model.RunwayAdmission`.** Expected: pass - PostLoad migrates in memory.
- [ ] **Step 5: Resave the nine assets** headlessly (editor closed on THIS worktree's content; memory `unreal-authoring-uassets-headlessly`: save_asset writes nothing unless forced). Load each `DA_Aircraft_*`, mark dirty, save with force. Then verify on disk, with a control:
  ```
  grep -c "MinimumPavement" Content/Entities/DA_Aircraft_Plane3.uasset     # expect >= 1
  grep -c "MinimumPavement" Content/Entities/DA_Aircraft_Plane2.uasset     # control: an asset not in the nine; expect whatever it held before
  ```
  (If `grep` on binary returns nothing for a resaved asset, the probe is at fault first - memory `verify-a-probe-before-trusting-its-silence`; use `python -c "print(open(p,'rb').read().count(b'MinimumPavement'))"`.)
- [ ] **Step 6: Delete the migration.** Remove `MinimumSurface_DEPRECATED`, its PropertyRedirect, and `PostLoad` (and its declaration). Build; rerun Step 4's filters. Expected: pass - the values now come from the resaved assets.
- [ ] **Step 7: Commit** code and the nine assets together. `refactor(pavement): MinimumPavement moves from the runway requirements to the aircraft type`

---

### Task 4: Fold #356's `ERoadSurface` into `EPavement`, and give its three pavement rules the shared concepts

#356 (`f20add83`, merged 2026-09-27) shipped grass roads and taxiways. Its audit against this plan's concepts (recorded 2026-09-27, Task 0 Step 3) found five places that must change, and they are this task. The first is the second enum. The other four are each a pavement rule written on the side of that enum, which the fold would otherwise leave as a hand-mapped special case:

| #356 shipped | Concept it breaks | Becomes |
|---|---|---|
| `ERoadSurface { Tarmac, Grass }` + `RoadSurfacePavement` + `RoadSurfaceName` (`RunwayFacts.h/.cpp`) | one scale | `EPavement` on `FRoadSegment::Surface`; the mapping and the name function are deleted |
| **Route gate** (`RouteSearch.cpp`): `Query.MinimumSurface > RoadSurfacePavement(Grass) && IsGrassRoad(...)` | one comparison (`FPavementCheck`) | `!Pavement::Judge(Network.PavementOf(Edge->DerivedFrom), Query.MinimumPavement).Passes()` - a third comparison site that only knew grass would silently pass a jet down a future weak surface |
| `FRouteQuery::MinimumSurface`/`NeedsSurface(ERunwaySurface)`, fed `Airframe.Requirements.MinimumSurface` at 5 sites (Arrival, Departure x2, Pushback, GroundTrafficRebuild) | the need lives on the aircraft (Task 3) | `FRouteQuery::MinimumPavement`/`NeedsPavement(EPavement)`, fed `Airframe.MinimumPavement` |
| **Mesh slot** (`RoadMeshBuilder.cpp`, `SurfaceSlotFor`): runway -> `RunwaySlotName(facts)`; grass road -> `RunwaySlotName(RoadSurfacePavement(Grass))`; else `NAME_None` | one pavement answer (`URoadNetwork::PavementOf`, added here rather than Task 6) | runway or non-tarmac road -> `URoadMaterialSet::RunwaySlotName(Network.PavementOf(Segment))`; tarmac road -> `NAME_None` (profile's own bands, unchanged) |
| **Junction "paved wins"** (`RoadMeshBuilder.cpp`): `bPaved = !IsGrassRoad(Arm)` | the scale is ordered | unchanged in behaviour. The rule is "grass loses", not "strongest wins" (a runway always counts as paved whatever it is surfaced with, #356's ruling), so it stays a grass test and says why |
| `GrassRateFactor`/`SurfaceRateFactor(ERoadSurface)` (`BuildCost.h/.cpp`) | one factor site | `Pavement::RateFactor` inline until Task 5's lines take it (marked; see Step 5) |
| `ConnectNodes`/`QuoteForConnect(..., ERoadSurface)` + tarmac overload (`RoadEditTarget.h`), `FRoadDeletionPlan::HealSurface`, road tool's `Surface` state (`RoadDrawTool.*`) | one scale | same shapes, typed `EPavement`; the non-virtual tarmac overload stays (#356's choice, and a non-virtual overload is not the default-argument-on-a-virtual trap Task 8 avoids) |

**Files:**
- Modify: `Public/Model/RunwayFacts.h`, `Private/Model/RunwayFacts.cpp`, `Public/Model/RoadNode.h` (`FRoadSegment::Surface`), `Public/Model/RoadNetwork.h`, `Private/Model/RoadNetwork.cpp`, `Public/Model/RouteSearch.h`, `Private/Model/RouteSearch.cpp`, `Private/Model/ArrivalPlanner.cpp`, `Private/Model/DeparturePlanner.cpp`, `Private/Model/PushbackPlanner.cpp`, `Private/Model/GroundTrafficRebuild.cpp`, `Public/Build/RoadMeshBuilder.h`, `Private/Build/RoadMeshBuilder.cpp`, `Private/Build/RoadLaneMarkingBuilder.cpp`, `Public/Build/BuildCost.h`, `Private/Build/BuildCost.cpp`, `Public/Tool/RoadEditTarget.h`, `Public/Tool/RoadHeal.h`, `Private/Tool/RoadHeal.cpp`, `Public/Tool/RoadDrawTool.h`, `Private/Tool/RoadDrawTool.cpp`, `Public/Present/RoadEditFacade.h`, `Private/Present/RoadEditFacade.cpp`, `Public/Present/RoadNetworkActor.h`, `Private/Present/RoadNetworkActor.cpp`, `Public/Testing/AirsideTestWorld.h`, `Profiles/RoadProfile.h`, `Config/DefaultEngine.ini`
- Create: `Public/Tool/PavementAxis.h`, `Private/Tool/PavementAxis.cpp`
- Test: `AirsideTests/Private/RoadSurfaceTest.cpp` (#356's six tests: `Airside.Tool.Variants.RoadSurface`, `Airside.Present.GrassRoadLaid`, `Airside.Build.GrassRoadCost`, `Airside.Build.GrassRoadSlots`, `Airside.Build.GrassRoadUnpainted`, `Airside.Model.RouteGrassGate`), `ToolVariantTest.cpp`, `TaxiwayWidthTest.cpp`, `Source/AirportMgr/BuildBarWidgetTest.cpp`

**Interfaces:**
- Consumes: `EPavement`, `Pavement::Name`, `FPavementCheck`, `Pavement::Judge` (Tasks 1-2); `FAirframe::MinimumPavement` (Task 3)
- Produces:
  - `FRoadSegment::Surface : EPavement = EPavement::Tarmac`
  - `EPavement URoadNetwork::PavementOf(FRoadSegmentId Segment) const` - runway: `RunwayFactsFor(Segment).Surface`; live road: `Segment.Surface`; dead/unknown: `Tarmac`. THE one answer to "what is this segment paved with"; `IsGrassRoad` becomes `!IsRunwaySegment(S) && PavementOf(S) == EPavement::Grass`
  - `FRouteQuery::MinimumPavement : EPavement = EPavement::Grass`, `FRouteQuery& NeedsPavement(EPavement)`
  - `UPROPERTY(EditAnywhere) TArray<EPavement> AllowedPavements;` on `URoadProfile` (empty = all four)
  - `namespace Pavement { TArray<EPavement> Offered(TConstArrayView<EPavement> Allowed); void AppendAxis(TArray<FToolVariantAxis>& Out, EPavement Current, TConstArrayView<EPavement> Allowed); }` in `Tool/PavementAxis.h` (Tool/ may see Model/; Model/ must not see `FToolVariantAxis`)
  - `bool URoadNetwork::SetSegmentSurface(FRoadSegmentId, EPavement)` - refuses (false, logs `SetSegmentSurface refused: %s is not offered by profile %s`) a pavement not in `Pavement::Offered(Profile->AllowedPavements)`, as well as the runway/dead refusals #356 already has

- [ ] **Step 1: Write the failing tests.** In `ToolVariantTest.cpp`:
  ```cpp
  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRoadSurfaceRowOffersTheProfileListTest, "Airside.Tool.Variant.RoadSurfaceRowOffersTheProfileList",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FRoadSurfaceRowOffersTheProfileListTest::RunTest(const FString&)
  {
      // THE PROFILE'S LIST, NOT A SECOND ENUM: the reason ERoadSurface existed - a road tool
      // must not offer a concrete service road - is now data on the profile.
      TArray<FToolVariantAxis> Axes;
      const EPavement RoadList[] = { EPavement::Tarmac, EPavement::Grass };
      Pavement::AppendAxis(Axes, EPavement::Tarmac, RoadList);
      TestEqual(TEXT("one row"), Axes.Num(), 1);
      TestEqual(TEXT("with exactly the profile's two options"), Axes[0].Options.Num(), 2);
      TestEqual(TEXT("in the profile's order - tarmac first, as #356's row had it"), Axes[0].Options[1].Id, FName(TEXT("grass")));

      TArray<FToolVariantAxis> All;
      Pavement::AppendAxis(All, EPavement::Tarmac, {});
      TestEqual(TEXT("an empty list offers all four - runways and stands"), All[0].Options.Num(), 4);
      TestEqual(TEXT("and lights the current one by its place in the OFFERED list"), All[0].Current, 1);
      return true;
  }
  ```
  In `RoadSurfaceTest.cpp`, beside `Airside.Model.RouteGrassGate` (read its fixture; reuse it):
  ```cpp
  // THE GATE IS THE SHARED COMPARISON, not a grass special case: a taxiway whose pavement is
  // below the aircraft's need is refused whatever that pavement is. #356's gate tested
  // "IsGrassRoad" and would have passed a concrete-needing jet down a tarmac taxiway.
  // <RouteGrassGate's fixture, taxiway left TARMAC, airframe MinimumPavement = Concrete>
  TestFalse(TEXT("a concrete-needing jet is refused a tarmac taxiway"), /* the fixture's route result */ .IsValid());
  // <same fixture, MinimumPavement = Tarmac>
  TestTrue(TEXT("and a tarmac-needing one is not"), /* same */ .IsValid());
  ```
  Replace the `<...>` / `/* */` with `RouteGrassGate`'s own setup and query, copied - only the pavement values differ.
  AND the second assertion is a behaviour change #356's gate did not have: before, only grass gated. It is right (a jet needing concrete cannot taxi on tarmac any more than it can land on it), but it is new, and every existing fleet type's `MinimumPavement` must be checked against the taxiways the starter map lays (Step 6).
- [ ] **Step 2: Build; run `-Filter Airside.Tool.Variant` and `-Filter Airside.Model.RouteGrassGate`.** Expected: compile FAILURE (`Pavement::AppendAxis`, `MinimumPavement` on `FRouteQuery`).
- [ ] **Step 3: `Tool/PavementAxis.h/.cpp`.**
  ```cpp
  namespace Pavement
  {
      /** Allowed as given, or all four when empty - the one reading of "empty means all". */
      AIRSIDE_API TArray<EPavement> Offered(TConstArrayView<EPavement> Allowed);

      /**
       * The "Surface" variant row, for every tool that lays ground - runway, road, stand. ONE
       * BUILDER so the three rows cannot name or order the scale differently. Current is lit
       * by its index in Offered(Allowed); a Current the list does not offer lights nothing
       * (INDEX_NONE) rather than a neighbour.
       */
      AIRSIDE_API void AppendAxis(TArray<FToolVariantAxis>& Out, EPavement Current, TConstArrayView<EPavement> Allowed);
  }
  ```
  `Offered` returns `Allowed` or `{Grass, Tarmac, Concrete, Reinforced}`. `AppendAxis` adds one axis, `Id = "Surface"` (the Id both #353's popout and #356's Shift+key look up), `Label = LOCTEXT("VariantAxisSurface", "Surface")`, one option per offered pavement (`Id = Pavement::Name(P)`, `Label = FText::FromString(Pavement::Name(P))`), `Current = Offered.IndexOfByKey(Current)`. Move BOTH existing row builders onto it, each keeping its "THE ENUM'S OWN NAMES" comment on the helper once: `FRunwayTool::GetVariantAxes` (RunwayTool.cpp:103-114) -> `Pavement::AppendAxis(Out, Surface, {})`; `FRoadDrawTool::GetVariantAxes` (RoadDrawTool.cpp:478-490) -> `Pavement::AppendAxis(Out, Surface, Profile->AllowedPavements)`. `FRoadDrawTool::SelectVariant` (RoadDrawTool.cpp:550-558) maps `Option` through `Pavement::Offered(Profile->AllowedPavements)[Option]` instead of `static_cast<ERoadSurface>(Option)` - an index into the offered list, never into the enum.
- [ ] **Step 4: The fold.** Delete `ERoadSurface`, `RoadSurfacePavement`, `RoadSurfaceName` from `RunwayFacts.h/.cpp`; every caller (#356's list above) takes `EPavement` / `Pavement::Name`. Add `URoadNetwork::PavementOf` and rewrite `IsGrassRoad` on it (Interfaces). `SurfaceSlotFor` becomes:
  ```cpp
  // A FACT ON THE SEGMENT decides its whole width's slot unless it is a tarmac road, whose
  // bands name their own - the path every taxiway and road has always taken. ONE PAVEMENT
  // ANSWER (PavementOf) for runway and road alike: #356 asked the runway's facts and the
  // road's surface separately, and mapped grass across by hand. Grass keeps the grass
  // runway's slot, so a grass taxiway meeting a grass strip is one field (#356's ruling).
  const EPavement P = Network.PavementOf(Segment);
  return Network.IsRunwaySegment(Segment) || P != EPavement::Tarmac
      ? URoadMaterialSet::RunwaySlotName(P)
      : NAME_None;
  ```
  Keep #356's "EVERY BAND takes it, kerbs and run-offs included" and "M_RunwayGrass already paints no centreline" comments. `RoadLaneMarkingBuilder.cpp`'s no-paint-on-grass test stays `IsGrassRoad`. The junction's `bPaved = !IsGrassRoad(ArmSegment)` stays, with one added line on its comment: `Not "strongest pavement wins" - a runway is paved whatever its surface (above), so the rule is "grass loses".` The route gate:
  ```cpp
  // GROUND TOO WEAK for the traveller, by the SAME comparison runway and stand admission use
  // (FPavementCheck) - not a grass test: #356 gated grass only, which would have passed a jet
  // needing concrete down a tarmac taxiway. Asked only when the query needs more than grass,
  // so a vehicle's or a grass-capable aircraft's search never pays the lookup. <keep #356's
  // turn-path and Find-retry sentences>
  if (Query.MinimumPavement > EPavement::Grass && Edge->DerivedFrom.IsSet()
      && !Network.IsRunwaySegment(Edge->DerivedFrom)
      && !Pavement::Judge(Network.PavementOf(Edge->DerivedFrom), Query.MinimumPavement).Passes())
  {
      return;
  }
  ```
  (`!IsRunwaySegment`: #356's "RUNWAYS ARE NOT JUDGED HERE - a strip's surface is RunwayAdmission's" stays true.) `FRouteQuery::MinimumSurface`/`NeedsSurface` become `MinimumPavement`/`NeedsPavement(EPavement)`, their doc comment kept and its "compared on the runway scale through RoadSurfacePavement" sentence replaced by "compared with FPavementCheck, the rule RunwayAdmission and StandAdmission share". The five callers pass `Airframe.MinimumPavement` (GroundTrafficRebuild: `Airframe->MinimumPavement`).
  - `URoadProfile` gains `AllowedPavements` (doc comment: the reason #356 made a two-step enum, now data; empty = all four, which is what runway profiles leave). Author `{Tarmac, Grass}` on every road and taxiway profile asset (`grep -rl "RoadProfile" Content --include=*.uasset` for the list; set through the same headless authoring path the profiles were made with, force-save, verify each with a byte count of `AllowedPavements`, and a runway profile as the control that must NOT gain it). `SetSegmentSurface` gains the offered-list refusal.
  - `GrassRateFactor` and `SurfaceRateFactor` are deleted; `ForSegment`'s pavement parameter becomes `EPavement` and multiplies by `Pavement::RateFactor` inline, marked `// Task 5 of the shared-pavement plan moves this into FBuildLine::Amount`. #356's "A FACTOR ON THE PROFILE, not a second set of rates" paragraph moves onto `Pavement::RateFactor`'s header - it is the reason the table is a factor.
  - SAVED DATA: `ERoadSurface` was `Tarmac=0, Grass=1`; `EPavement` is `Grass=0, Tarmac=1`. UENUM properties serialise by NAME, and the names match, so the redirect is enough: `+EnumRedirects=(OldName="/Script/Airside.ERoadSurface",NewName="/Script/Airside.EPavement")`. #356 shipped 2026-09-27, so a level holding a saved grass road may exist; Step 6 proves the redirect on one.
- [ ] **Step 5: Build; run `-Filter Airside.Tool`, `-Filter Airside.Build.GrassRoad`, `-Filter Airside.Present.GrassRoadLaid`, `-Filter Airside.Model.RouteGrassGate`, `-Filter Airside.Model`, and `-Filter AirportMgr` (BuildBarWidgetTest).** Expected: all pass, #356's six included, and the two new tests.
- [ ] **Step 6: Prove saved grass survives, and the gate's widening strands nothing.**
  - `grep -l "ERoadSurface" Content -r --include=*.umap`. For each hit (if none, lay one: open the worktree editor, draw a grass taxiway on M_ModelYard, save, close), load it after the build and log each segment's `PavementOf` - add the line to `Airside.Present.GrassRoadLaid`'s level variant if it has one, else a one-off `StarterMapProbe`-style assertion. Expected: the grass road reads `grass`.
  - The widened gate: list every fleet type's `MinimumPavement` (Task 0 baseline) above `Tarmac`. For each, confirm M_Starter's taxiways are at least that strong, or record in the PR that such a type can no longer taxi there and why that is correct. No such type is expected on 2026-09-27 (the fleet's needs were grass or tarmac); the check is that the baseline says so.
- [ ] **Step 7: Commit.** `refactor(pavement): #356's road surfaces join the one scale - PavementOf, shared gate, one surface row`

---

### Task 5: `FBuildQuote` becomes priced lines; the ledger prices per line

**Files:**
- Modify: `Public/Model/BuildPurse.h:5-43`, create `Private/Model/BuildPurse.cpp` if it does not exist (`FBuildLine::Amount`, `FBuildQuote::BaseAmount`)
- Modify: `Public/Build/BuildCost.h`, `Private/Build/BuildCost.cpp:40-98`
- Modify: every `BaseAmount`/`Quote.Source` site (13 files on 2026-09-27: `RoadEditFacade.cpp`(12), `BuildCostTest.cpp`, `BuildPurseTest.cpp`, `BuildCost.cpp`, `Pricing.cpp`, `RunwayToolTest.cpp`, `RoadEditFacadeSurfaces.cpp`, `Ledger.cpp`, `Pricing.h`, `OpsRuntimeTest.cpp`, `LedgerPurseTest.cpp`, `LedgerDeterminismTest.cpp`)
- Test: `AirsideTests/Private/BuildCostTest.cpp`, `AirportOpsTests/Private/LedgerPurseTest.cpp`

**Interfaces:**
- Consumes: `EPavement`, `Pavement::RateFactor` (Task 2)
- Produces:
  ```cpp
  enum class EBuildUnit : uint8 { Metre, SquareMetre, Each };
  struct FBuildLine { TWeakObjectPtr<const UObject> Source; EBuildUnit Unit = EBuildUnit::Each; double Quantity = 0.0;
                      double RatePerUnit = 0.0; TOptional<EPavement> Pavement; AIRSIDE_API double Amount() const; };
  struct FBuildQuote { TArray<FBuildLine> Lines; FText What; AIRSIDE_API double BaseAmount() const; bool IsFree() const; };
  namespace BuildCost {
      FBuildQuote ForSegment(const URoadProfile& Profile, double LengthUu, EPavement Pavement);
      FBuildQuote ForApron(TConstArrayView<FVector2D> Outline, double RatePerSquareMetre, TOptional<EPavement> Pavement);
      FBuildQuote ForEntity(const UEntityDefinition& Definition);
      FBuildQuote Combine(FBuildQuote A, const FBuildQuote& B);   // lines appended, What "{A} + {B}"
  }
  ```

- [ ] **Step 1: Write the failing tests.** In `BuildCostTest.cpp`:
  ```cpp
  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTarmacPricesUnchangedTest, "Airside.Build.BuildCost.TarmacPricesUnchanged",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FTarmacPricesUnchangedTest::RunTest(const FString&)
  {
      // FIGURES CAPTURED FROM MAIN before quotes became lines (Task 0). Tarmac is factor 1, so
      // every price a player could see before this change must be the same after it.
      const URoadProfile* Taxi = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
      const TArray<FVector2D> Pad = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
      // Replace each 0.0 with the Task 0 BASELINE figure, typed exactly.
      TestEqual(TEXT("a 500 m taxiway"), BuildCost::ForSegment(*Taxi, 50000.0, EPavement::Tarmac).BaseAmount(), 0.0 /*BASELINE segment*/, 1e-6);
      TestEqual(TEXT("a B-sized pad"), BuildCost::ForApron(Pad, 10.0, EPavement::Tarmac).BaseAmount(), 0.0 /*BASELINE apron*/, 1e-6);
      TestEqual(TEXT("a B stand's equipment"), BuildCost::ForEntity(*UEntityDefinition::MakeStandTransient(EIcaoCode::B)).BaseAmount(), 0.0 /*BASELINE entity*/, 1e-6);
      return true;
  }

  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQuoteIsItsLinesTest, "Airside.Build.BuildCost.QuoteIsItsLines",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FQuoteIsItsLinesTest::RunTest(const FString&)
  {
      // A STAND IS TWO LINES, so pricing can key a discount on the definition without also
      // discounting the ground under it - the reason the pad's line keeps its own null source.
      const TArray<FVector2D> Pad = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
      const FBuildQuote Entity = BuildCost::ForEntity(*UEntityDefinition::MakeStandTransient(EIcaoCode::B));
      const FBuildQuote Ground = BuildCost::ForApron(Pad, 10.0, EPavement::Tarmac);
      const FBuildQuote Stand = BuildCost::Combine(Entity, Ground);
      TestEqual(TEXT("two lines"), Stand.Lines.Num(), 2);
      TestEqual(TEXT("summing to both parts"), Stand.BaseAmount(), Entity.BaseAmount() + Ground.BaseAmount());
      TestTrue(TEXT("the pad line has no source asset"), Stand.Lines[1].Source.Get() == nullptr);
      TestFalse(TEXT("and a building line has no pavement"), Stand.Lines[0].Pavement.IsSet());
      return true;
  }
  ```
  In `LedgerPurseTest.cpp`:
  ```cpp
  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLedgerPricesPerLineTest, "AirportOps.Ledger.PricesPerLine",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FLedgerPricesPerLineTest::RunTest(const FString&)
  {
      // PER LINE, BOTH WAYS: a charge is the sum of each line's price, and a demolish is the sum
      // of each line's scrap - the pad line (null source) included, or a demolished stand would
      // refund only its equipment. (Review Focus 1.)
      UPricing* Pricing = nullptr;
      ULedger* Ledger = PurseWith(100000.0, Pricing);   // LedgerPurseTest.cpp's own helper
      FBuildQuote Quote;
      Quote.Lines.Add({ nullptr, EBuildUnit::Each, 1.0, 1000.0, {} });
      Quote.Lines.Add({ nullptr, EBuildUnit::SquareMetre, 100.0, 10.0, EPavement::Grass });
      const double Before = Ledger->Balance();
      Ledger->Charge(Quote);
      TestEqual(TEXT("charged 1000 + 100 x 10 x 0.4"), Before - Ledger->Balance(), 1400.0);
      const double AfterCharge = Ledger->Balance();
      Ledger->Credit(Quote);
      TestEqual(TEXT("scrap is the refund fraction of the same sum"), Ledger->Balance() - AfterCharge,
          1400.0 * Pricing->RefundFraction);
      return true;
  }
  ```
- [ ] **Step 2: Build; run.** Expected: compile FAILURE (`BaseAmount()` is not callable, `Lines` missing).
- [ ] **Step 3: Implement the types** in `BuildPurse.h`. `FBuildQuote`'s existing doc comments on `BaseAmount`, `Source`, `What` and `IsFree` move onto the matching members of `FBuildLine`/`FBuildQuote` - `Source`'s "NOT a parallel enum of build kinds" paragraph goes onto `FBuildLine::Source` verbatim. New comment on `FBuildLine`:
  ```cpp
  /**
   * One priced quantity: so many metres of a profile, square metres of ground, or one placed
   * thing, at its authored rate, on its pavement.
   *
   * WHY LINES (spec 2026-09-27 §4): a pavement factor applied per buildable kind is a factor
   * that the next kind forgets. Every buildable is lines; Amount is the ONE place the factor
   * meets a rate, so a grass road, runway and stand pad are cheaper with no kind-specific code.
   * ENFORCED BY: Check-Architecture rule 4 row 'Pavement::RateFactor'
   */
  ```
  `Amount()`: `Quantity * RatePerUnit * (Pavement.IsSet() ? Pavement::RateFactor(*Pavement) : 1.0)`, clamped at 0 (keep `ForApron`'s "A NEGATIVE CHARGE PAYS THE PLAYER TO BUILD" reasoning as a comment here - the clamp now covers every kind). `BaseAmount()` sums `Amount()`.
- [ ] **Step 4: Rebuild the constructors** in `BuildCost.cpp`. Each builds one line: `ForSegment` -> `{ &Profile, Metre, MetresFromUu(LengthUu), Profile.CostPerMetre, Pavement }` (remove Task 4's inline factor and its marker); `ForApron` -> `{ nullptr, SquareMetre, PolygonAreaSquareMetres(Outline), RatePerSquareMetre, Pavement }`; `ForEntity` -> `{ &Definition, Each, 1.0, Definition.PlacementCost, {} }`. `What` texts unchanged. Add `Combine`, and rewrite `URoadEditFacade::QuoteStand` (RoadEditFacadeSurfaces.cpp:629-644) and `PlaceEntityInPlot`'s two-quote sum (`:513-530`) onto it - both hand-summed `BaseAmount` today; their "THE ONE PLACE THIS QUOTE IS BUILT" comments stay.
- [ ] **Step 5: The ledger.** `ULedger::PriceOf`:
  ```cpp
  double ULedger::PriceOf(const FBuildQuote& Quote) const
  {
      // PER LINE, so a discount keyed on one line's source never discounts the ground beside
      // it - see FBuildLine. UPricing is unchanged: it still prices an amount for a source.
      double Price = 0.0;
      for (const FBuildLine& Line : Quote.Lines)
      {
          Price += Pricing != nullptr ? Pricing->PriceOfBuild(Line.Amount(), Line.Source.Get()) : Line.Amount();
      }
      return Price;
  }
  ```
  `Credit`: the same loop over `Pricing->ScrapValue(Line.Amount(), Line.Source.Get())` (0 with no pricing, as today). Every other `.BaseAmount` read becomes `.BaseAmount()`; every write builds a line instead.
- [ ] **Step 6: Build; run `-Filter Airside.Build.BuildCost`, `-Filter AirportOps`, `-Filter Airside.Present`, `-Filter Airside.Tool`.** Expected: pass, and `TarmacPricesUnchanged` with the baseline figures.
- [ ] **Step 7: Commit.** `refactor(cost): quotes are priced lines; the ledger prices per line`

---

### Task 6: Pavement reaches every segment's price and upkeep

**Files:**
- Modify: `Private/Build/BuildCost.cpp:100-138` (`DailyUpkeep`), `Private/Present/RoadEditFacade.cpp:199-215,484-505,550,580-620`, `Public/Tool/RoadEditTarget.h:119-126`, `Public/Present/RoadEditFacade.h:319`, `Public/Present/RoadNetworkActor.h:352`, `Private/Present/RoadNetworkActor.cpp:931-934`, `Private/Tool/RunwayTool.cpp:336`
- Test: `AirsideTests/Private/BuildCostTest.cpp`

**Interfaces:**
- Consumes: `FBuildLine`, `BuildCost::ForSegment(Profile, Length, EPavement)` (Task 5); `FRoadSegment::Surface : EPavement` (Task 4)
- Produces: `IRoadEditTarget::QuoteForRunway(FVector2D From, FVector2D To, const URoadProfile* Profile, EPavement Pavement) const`; `URoadNetwork::PavementOf(const FRoadSegment&) const -> EPavement` - the ONE answer to "what is this segment paved with" (runway: `RunwayFactsFor(Id).Surface`; else `Segment.Surface`).

- [ ] **Step 1: Write the failing tests** in `BuildCostTest.cpp`:
  ```cpp
  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFactorOnEveryKindTest, "Airside.Build.BuildCost.FactorOnEveryKind",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FFactorOnEveryKindTest::RunTest(const FString&)
  {
      // EVERY KIND THAT LIES ON GROUND takes the factor, and a building does not - the point of
      // applying it in FBuildLine::Amount rather than per kind.
      const URoadProfile& Taxi = *URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
      const TArray<FVector2D> Pad = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
      TestEqual(TEXT("grass taxiway is 0.4 of tarmac"),
          BuildCost::ForSegment(Taxi, 50000.0, EPavement::Grass).BaseAmount(),
          0.4 * BuildCost::ForSegment(Taxi, 50000.0, EPavement::Tarmac).BaseAmount(), 1e-6);
      TestEqual(TEXT("a grass stand pad is 0.4 of a tarmac one"),
          BuildCost::ForApron(Pad, 10.0, EPavement::Grass).BaseAmount(),
          0.4 * BuildCost::ForApron(Pad, 10.0, EPavement::Tarmac).BaseAmount(), 1e-6);
      TestEqual(TEXT("a bare apron has no pavement and bills at the rate itself"),
          BuildCost::ForApron(Pad, 10.0, {}).BaseAmount(), BuildCost::ForApron(Pad, 10.0, EPavement::Tarmac).BaseAmount(), 1e-6);
      return true;
  }

  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUpkeepUsesBuildFactorTest, "Airside.Build.BuildCost.UpkeepUsesBuildFactor",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FUpkeepUsesBuildFactorTest::RunTest(const FString&)
  {
      // A RUNWAY'S PAVEMENT IS ITS FACTS', a road's is its segment's (Review Focus 2). Two
      // networks, identical but for one fact each: upkeep must move by exactly the factor.
      auto UpkeepOf = [](bool bRunway, EPavement P)
      {
          URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
          URoadProfile* Profile = URoadProfile::MakeTransient(2300.0, 1500.0, 230.0);
          Profile->bContinuousThroughJunctions = bRunway;   // RunwayAdmissionTest's MakeRunway shape
          Profile->UpkeepPerMetrePerDay = 1.0;              // non-zero, or the ratio proves nothing
          const FRoadSegmentId S = Net->AddStraightSegment(Net->AddNode({0,0}), Net->AddNode({100000,0}), Profile);
          if (bRunway) { FRunwayFacts F; F.Surface = P; Net->SetRunwayFacts(S, F); }
          else         { Net->SetSegmentSurface(S, P); }
          return BuildCost::DailyUpkeep(*Net, 0.0);
      };
      TestEqual(TEXT("grass runway upkeep is 0.4 of tarmac"), UpkeepOf(true, EPavement::Grass), 0.4 * UpkeepOf(true, EPavement::Tarmac), 1e-6);
      TestEqual(TEXT("grass taxiway upkeep is 0.4 of tarmac"), UpkeepOf(false, EPavement::Grass), 0.4 * UpkeepOf(false, EPavement::Tarmac), 1e-6);
      TestTrue(TEXT("and upkeep is not zero, or the ratio proves nothing"), UpkeepOf(true, EPavement::Tarmac) > 0.0);
      return true;
  }
  ```
  (`SetSegmentSurface` refuses a pavement the profile does not offer; `MakeTransient`'s `AllowedPavements` is empty = all four, so grass is accepted. If `IsRunwaySegment` needs more than the flag, copy RunwayAdmissionTest's `MakeRunway` exactly.)
- [ ] **Step 2: Build; run.** Expected: `UpkeepUsesBuildFactor` FAILS on the runway line (upkeep ignores runway facts today).
- [ ] **Step 3: Implement.** Add `URoadNetwork::PavementOf` (declaration in `RoadNetwork.h` beside `IsGrassRoad`, with a comment saying it is the one answer, and `IsGrassRoad` rewritten to `PavementOf(...) == EPavement::Grass && !IsRunwaySegment(...)` if its semantics allow - otherwise leave it and note why). `DailyUpkeep`'s segment term multiplies by `Pavement::RateFactor(Network.PavementOf(Segment))` - keep the grass branch's "THE SAME FACTOR THE BUILD QUOTE PAID" comment and drop its "a runway's Surface is never written" sentence, which stops being true. Every `ForSegment` call site passes `Network.PavementOf(Segment)` (existing segment) or the tool's chosen pavement (a new one). `QuoteForRunway` gains `EPavement Pavement` through interface, facade, actor and `RunwayTool.cpp:336` (which passes its `Surface`); `PlaceRunway` prices with `Facts.Surface`.
- [ ] **Step 4: Build; run `-Filter Airside.Build.BuildCost`, `-Filter Airside.Tool.Runway`, `-Filter Airside.Present`.** Expected: pass.
- [ ] **Step 5: Commit.** `feat(cost): runway and road price and upkeep follow their pavement`

---

### Task 7: Code A stands merge into B

**Files:**
- Modify: `Public/Solve/IcaoCode.h:160-240`, `Private/Solve/IcaoCode.cpp:110-127,200-230,341-362,445-460`, `Private/Present/RoadEditFacadeSurfaces.cpp:655-672`
- Test: `AirsideTests/Private/IcaoCodeTest.cpp:91-200` and the A-stand cases in `StandBoxTest.cpp`, `StandDesignVehicleTest.cpp`, `StandPlotPlacementTest.cpp`, `StandPlotToolTest.cpp`, `ServiceLinkTest.cpp`, `AirsideContentTest.cpp`, `StarterMapProbeTest.cpp`

**Interfaces:**
- Produces: `IcaoCode::StandLetterFor(EIcaoCode Aircraft) -> EIcaoCode` (A -> B, identity otherwise); `IcaoCode::SmallestStandLetter() -> EIcaoCode` (== `StandLetterFor(EIcaoCode::A)`).

- [ ] **Step 1: Grep before deleting.** For each of `StandDepth`, `TowLaneWidth`, `AftEdgeAllowance`: `grep -n "Row.StandDepth\|\.StandDepth\b" Private/Solve/IcaoCode.cpp` (and the others). Expected: read only by `WidthOf`, `StandDepthForLetter`, `LetterForStandSize`. If any other reader appears, keep that column for A and say why in the task's commit.
- [ ] **Step 2: Rewrite the A-band assertions** in `FStandWidthIsDerivedFromClearanceTest` (IcaoCodeTest.cpp:140-160) - delete the `MaxStandWidthForLetter(A) == StandWidthForLetter(A)` and `LetterForStandSize(...A...) == "A"` lines and add:
  ```cpp
  // A MERGED INTO B (user, 2026-09-27): the difference between them was 5.5 m of depth, which
  // no player chose on purpose; the choice at the small end is now the SURFACE. No rectangle
  // reads as A, at any width - A stays an AIRCRAFT letter only.
  TestEqual(TEXT("an A aircraft parks on a B stand"), IcaoCode::StandLetterFor(EIcaoCode::A), EIcaoCode::B);
  TestEqual(TEXT("every other letter parks on its own"), IcaoCode::StandLetterFor(EIcaoCode::D), EIcaoCode::D);
  TestEqual(TEXT("the smallest stand is B's"), IcaoCode::SmallestStandLetter(), EIcaoCode::B);
  const double BW = IcaoCode::StandWidthForLetter(EIcaoCode::B);
  const double BD = IcaoCode::StandDepthForLetter(EIcaoCode::B);
  for (const double Width : { BW, BW + 500.0, IcaoCode::StandWidthForLetter(EIcaoCode::C) - 1.0 })
  {
      TestEqual(FString::Printf(TEXT("%.0f wide at B's depth is B"), Width), IcaoCode::LetterForStandSize(Width, BD), FString(TEXT("B")));
      TestEqual(FString::Printf(TEXT("%.0f wide one uu under B's depth is no stand - once A's"), Width),
          IcaoCode::LetterForStandSize(Width, BD - 1.0), FString());
  }
  TestEqual(TEXT("A's stand width IS B's, through the alias"), IcaoCode::StandWidthForLetter(EIcaoCode::A), BW);
  ```
  Update the `ENFORCED BY` line in `IcaoCode.h` if the assertions' location moves.
- [ ] **Step 3: Write the Review Focus 4 test** in `StandPlotPlacementTest.cpp` (read its fixture first; use its facade/actor setup):
  ```cpp
  // A DRAG THAT WAS A CODE A STAND (50 x 36 m) is now refused, and the refusal names DEPTH -
  // the dimension that is short - so the player is not left guessing (Review Focus 4).
  const TArray<FVector2D> OldA = { {0,0}, {5000,0}, {5000,3600}, {0,3600} };
  const FString Why = Facade->WhyStandRefused(OldA, EPavement::Tarmac);   // signature from Task 8; until then (Outline)
  TestTrue(TEXT("refused"), !Why.IsEmpty());
  TestTrue(TEXT("for depth"), Why.Contains(TEXT("deep")) || Why.Contains(TEXT("depth")));
  TestFalse(TEXT("and not for width, which is enough"), Why.Contains(TEXT("wide")) || Why.Contains(TEXT("width")));
  ```
  (Match the words to the facade's actual deficit strings - `RoadEditFacadeSurfaces.cpp:668-690`.)
- [ ] **Step 4: Build; run `-Filter Airside.Solve.StandWidth` and the new case.** Expected: FAIL (`StandLetterFor` missing; the 36 m drag reads as A).
- [ ] **Step 5: Implement.** In `IcaoCode.cpp`: omit `StandDepth`, `TowLaneWidth`, `AftEdgeAllowance` from row A (they become 0), with a comment on the row: `// NO STANDS OF ITS OWN since 2026-09-27 - an A aircraft parks on B's (StandLetterFor). Its size columns are omitted, not zeroed by hand, so HasStands reads the table rather than a flag.` Add
  ```cpp
  /** A row sizes stands only if it has a stand depth - see row A. */
  static bool HasStands(const FRow& Row) { return Row.StandDepth > 0.0; }
  ```
  `StandLetterFor(Code)`: `return Code == EIcaoCode::A ? EIcaoCode::B : Code;` - with a header comment stating it is the ONE alias, `// ENFORCED BY: Airside.Solve.StandWidthIsDerivedFromClearance`. `StandWidthForLetter`, `MaxStandWidthForLetter`, `StandDepthForLetter` read `RowFor(StandLetterFor(Code))`. `LetterForStandSize`'s loop adds `if (!HasStands(Row)) { continue; }` as its first line, and its trailing comment reads "Smaller than the smallest stand letter". `SmallestStandLetter()` returns `StandLetterFor(EIcaoCode::A)`. Replace the header's "CODE A'S BAND IS EMPTY since 2026-09-26" paragraph with the merge's reason and date. In `RoadEditFacadeSurfaces.cpp:665-666`, `EIcaoCode::A` -> `IcaoCode::SmallestStandLetter()`, and the comment above it ("MEASURED AGAINST CODE A'S OWN FLOOR") is rewritten to "the smallest stand letter's floor".
- [ ] **Step 6: Move or delete the other A-stand tests.** For each file in the Files list: every stand built at `EIcaoCode::A` moves to `B`; a case that then duplicates an existing B case is deleted. List each moved/deleted test in the commit message body.
- [ ] **Step 7: Build; run `-Filter Airside.Solve`, `-Filter Airside.Present`, `-Filter Airside.Tool.Stand`, `-Filter Airside.Content`.** Expected: pass; the run count falls by exactly the number of deleted duplicates listed in Step 6.
- [ ] **Step 8: Commit.** `feat(stands): Code A stands merge into B; the small-stand choice is the surface`

---

### Task 8: A stand has a pavement - placement, price, upkeep, tool row, look

**Files:**
- Modify: `Public/Model/RoadEntity.h:240-250` (`FEntityInstance`), `Public/Model/AirsideCapability.h:25-33` (`FStandSummary`), the capability builder that fills `FStandSummary` (`grep -rn "FStandSummary" Plugins/Airside/Source/Airside/Private`)
- Modify: `Public/Tool/RoadEditTarget.h:342-369`, `Public/Present/RoadEditFacade.h:196-198,557-565`, `Public/Present/RoadNetworkActor.h:580-585`, `Private/Present/RoadNetworkActor.cpp` (forwarders), `Private/Present/RoadEditFacadeSurfaces.cpp:629-850`, every test fake implementing `IRoadEditTarget` (`grep -rln "public IRoadEditTarget" Plugins`)
- Modify: `Public/Tool/StandPlotTool.h`, `Private/Tool/StandPlotTool.cpp:95-115`
- Modify: `Private/Build/BuildCost.cpp` (`DailyUpkeep`)
- Modify: `Private/Present/RoadSurfacePresenter.cpp:353-358`, `Public/Build/RoadMeshBuilder.h:114`, `Private/Build/RoadMeshBuilder.cpp`
- Test: `AirsideTests/Private/StandPlotPlacementTest.cpp`, `BuildCostTest.cpp`, `StandPlotToolTest.cpp`

**Interfaces:**
- Consumes: `Pavement::AppendAxis` (Task 4), `BuildCost::ForApron(..., TOptional<EPavement>)`, `BuildCost::Combine` (Task 5)
- Produces: `FEntityInstance::Pavement` (`UPROPERTY() EPavement Pavement = EPavement::Tarmac;`); `FStandSummary::Pavement`; `IRoadEditTarget::PlaceStandInPlot(const TArray<FVector2D>& Outline, FVector2D EntranceA, FVector2D EntranceB, EPavement Pavement)`; `IRoadEditTarget::WhyStandRefused(TArrayView<const FVector2D> Outline, EPavement Pavement) const`; `URoadEditFacade::QuoteStand(const UEntityDefinition&, TArrayView<const FVector2D>, EPavement) const`.

- [ ] **Step 1: Write the failing tests.** `StandPlotPlacementTest.cpp` (reuse its fixture):
  ```cpp
  // THE PAVEMENT THE TOOL CHOSE is the pavement placed, priced and afforded (Review Focus 5):
  // with exactly enough money for a grass stand, the afford gate must pass for grass and
  // refuse tarmac, or the ghost would say "cannot afford" for a click that would have paid.
  const TArray<FVector2D> B = { {0,0}, {5000,0}, {5000,3950}, {0,3950} };
  const double GrassPrice = Facade->QuoteStand(*Actor->ResolveStandDefinitionFor(EIcaoCode::B), B, EPavement::Grass).BaseAmount();
  // Open the fixture's purse at exactly GrassPrice, through the same ULedger::Open call
  // LedgerPurseTest's PurseWith uses, on the ledger StandPlotPlacementTest's fixture wires in.
  TestTrue(TEXT("grass is affordable"), Facade->WhyStandRefused(B, EPavement::Grass).IsEmpty());
  TestTrue(TEXT("tarmac is not"), Facade->WhyStandRefused(B, EPavement::Tarmac).Contains(TEXT("afford")));
  const int32 Placed = Facade->PlaceStandInPlot(B, B[0], B[1], EPavement::Grass);
  TestEqual(TEXT("and the placed stand is grass"), Actor->Network->GetEntities()[Placed].Pavement, EPavement::Grass);
  ```
  `BuildCostTest.cpp`:
  ```cpp
  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandPadUpkeepByAreaTest, "Airside.Build.BuildCost.StandPadUpkeepByArea",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FStandPadUpkeepByAreaTest::RunTest(const FString&)
  {
      // A STAND'S GROUND IS BILLED BY AREA AND PAVEMENT (user, 2026-09-27) - before, only its
      // definition's flat UpkeepPerDay, so a grass stand was cheaper to build and the same to own.
      auto PadUpkeep = [](double Depth, EPavement P)
      {
          URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
          UEntityDefinition* Def = UEntityDefinition::MakeStandTransient(EIcaoCode::B);
          Def->UpkeepPerDay = 0.0;   // isolate the pad's term
          const FEntityInstanceId Id = ServiceLinkFixture::PlaceStand(*Net, *Def, FVector2D::ZeroVector, 0.0);
          FRoadNetworkTestAccess::SetEntityOutlineForTest(*Net, Id, { {0,0}, {5000,0}, {5000,Depth}, {0,Depth} });
          FRoadNetworkTestAccess::SetEntityPavementForTest(*Net, Id, P);   // added in this step
          return BuildCost::DailyUpkeep(*Net, 1.0);
      };
      TestEqual(TEXT("grass pad upkeep is 0.4 of tarmac"), PadUpkeep(3950.0, EPavement::Grass), 0.4 * PadUpkeep(3950.0, EPavement::Tarmac), 1e-6);
      TestEqual(TEXT("double the area, double the upkeep"), PadUpkeep(7900.0, EPavement::Tarmac), 2.0 * PadUpkeep(3950.0, EPavement::Tarmac), 1e-6);
      TestEqual(TEXT("1975 m2 at rate 1 and factor 1"), PadUpkeep(3950.0, EPavement::Tarmac), 50.0 * 39.5, 1e-6);
      return true;
  }
  ```
  Add `SetEntityPavementForTest(URoadNetwork&, FEntityInstanceId, EPavement)` to `FRoadNetworkTestAccess` beside `SetEntityOutlineForTest`, in the same shape (`grep -rn "SetEntityOutlineForTest" Plugins/Airside/Source` finds its definition - match its real parameter list). `ServiceLinkFixture` is `StandFixture.h`.
  `StandPlotToolTest.cpp`: the stand tool offers one "Surface" row of four, lit on tarmac, and `SelectVariant` on grass makes the next `PlaceStandInPlot` receive `EPavement::Grass` (use the test's existing recording fake target and extend its recorded arguments).
- [ ] **Step 2: Build; run.** Expected: compile FAILURE on the new signatures.
- [ ] **Step 3: Model.** `FEntityInstance` gains, after `DesignWingspan`:
  ```cpp
  /**
   * What this stand's pad is paved with - admission (StandAdmission), price and upkeep read
   * it. Captured at placement from the stand tool's surface row. Tarmac for every non-stand
   * entity and for anything saved before 2026-09-27, which is the pavement they were drawn
   * with.
   */
  UPROPERTY() EPavement Pavement = EPavement::Tarmac;
  ```
  `RoadEntity.h` includes `Model/Pavement.h` (rule 19 bans only Airframe.h / AgentMotion.h). `FStandSummary` gains `UPROPERTY() EPavement Pavement = EPavement::Tarmac;`, filled where `DesignWingspan` is.
- [ ] **Step 4: Signatures.** Add `EPavement Pavement` as the last parameter of `PlaceStandInPlot` and `WhyStandRefused` on `IRoadEditTarget`, `URoadEditFacade`, `ARoadNetworkActor` and every test fake - no default argument (a defaulted parameter on a virtual binds by static type and would let a forgotten caller place tarmac silently). `QuoteStand` gains `EPavement` and passes it to `ForApron`. `PlaceStandInPlot` writes `Instance.Pavement = Pavement;` where it writes `DesignWingspan`, and logs it in its existing placement line (append `pavement %s` with `Pavement::Name`).
- [ ] **Step 5: Upkeep.** In `DailyUpkeep`'s entity loop, after the definition's `UpkeepPerDay`:
  ```cpp
  // A STAND'S GROUND, by area and pavement (user, 2026-09-27), beside its definition's flat
  // figure - that one is the equipment, this is the pad. PolygonAreaSquareMetres, the same
  // measure the pad's build line used, so the two cannot measure one pad differently.
  if (Entity.IsStand() && Entity.Outline.Num() >= 3)
  {
      Total += PolygonAreaSquareMetres(Entity.Outline) * ApronRatePerSquareMetrePerDay * Pavement::RateFactor(Entity.Pavement);
  }
  ```
  (`IsStand()` - check the exact predicate name in `RoadEntity.h`; a depot's plot is NOT billed here, it has its own kit upkeep.)
- [ ] **Step 6: Tool.** `FStandPlotTool` gains `EPavement Pavement = EPavement::Tarmac;`, `GetVariantAxes` -> `Pavement::AppendAxis(Out, Pavement, {})`, `SelectVariant(Axis 0, Option)` sets `Pavement = Pavement::Offered({})[Option]` (refuse out-of-range), and its `WhyStandRefused` / `PlaceStandInPlot` calls pass `Pavement`. The tool's per-outline refusal memo (`StandPlotTool.h:160-170`) must key on the pavement too, or a row change leaves a stale "cannot afford" - add it to the memo key and say so in the memo's comment.
- [ ] **Step 7: Look.** DECIDED BY #356'S MECHANISM (2026-09-27 audit): a grass road is drawn by MATERIAL SLOT - `FRoadMeshBuilder::SurfaceSlotFor` hands the whole width the grass runway's slot, resolved through the road layer's `URoadMaterialSet` (`URoadSurfacePresenter::EffectiveMaterialSet`, which already carries the four `RunwaySlotName` slots). The apron layer, where pads are drawn (`RoadSurfacePresenter.cpp:343-372`), is ONE material through `RebuildLayer`. So a grass pad needs the apron layer to take slots: `FRoadMeshBuilder::AddApron(const TArray<FVector2D>& Outline, FName Slot)` tags the polygon's triangles with `Slot` the way the road builder tags bands, and the apron layer's build resolves slots through the same `EffectiveMaterialSet` the road layer uses - `NAME_None` (tarmac pads, bare aprons) keeping today's `Settings.ApronMaterial`. The pad passes `Entity.Pavement == EPavement::Tarmac ? NAME_None : URoadMaterialSet::RunwaySlotName(Entity.Pavement)` - the same rule Task 4 gave `SurfaceSlotFor`, so a grass stand beside a grass taxiway is one field. APPROVAL NEEDED before this step (asked 2026-09-27; see the plan's Open Decisions). Test: `Airside.Build.StandPadSlots` in `RoadSurfaceTest.cpp`, modelled on `Airside.Build.GrassRoadSlots` - a grass pad's triangles carry the grass slot, a tarmac pad's carry none.
- [ ] **Step 8: Build; run `-Filter Airside.Present`, `-Filter Airside.Tool.Stand`, `-Filter Airside.Build`.** Expected: pass.
- [ ] **Step 9: Look at it.** In an editor on this worktree (memory `worktree-editor-mcp-port`: `-ini ServerPortNumber=8001` + `AIRSIDE_MCP_PORT`), draw a grass and a tarmac B stand side by side; `python Tools/Mcp.py shot pads.png`. Expected: the grass pad reads as grass. Attach the shot to the PR.
- [ ] **Step 10: Commit.** `feat(stands): a stand has a pavement - placed, priced, billed by area, drawn`

---

### Task 9: `StandAdmission` replaces span-only stand admission

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Model/StandAdmission.h`, `Private/Model/StandAdmission.cpp`
- Modify: `Private/Model/ArrivalPlanner.cpp:15-35,50-60,95-140,320-335,370-440`, `Public/Model/ArrivalPlanner.h:52-60`, `Plugins/AirportOps/Source/AirportOps/Private/Model/StandAllocator.cpp:10-60`
- Test: `AirsideTests/Private/ArrivalPlannerTest.cpp` (add cases), `AirsideTests/Private/StandAdmissionTest.cpp` (NEW - two builds), `AirportOpsTests/Private/StandAllocatorTest.cpp`

**Interfaces:**
- Consumes: `FPavementCheck`, `Pavement::Judge/Describe` (Task 2); `FEntityInstance::Pavement` (Task 8); `FAirframe::MinimumPavement` (Task 3)
- Produces:
  ```cpp
  UENUM() enum class EStandRefusal : uint8 { None, Surface, TooSmall, Service };
  USTRUCT() struct AIRSIDE_API FStandAdmission { EStandRefusal Why; FPavementCheck Pavement; double StandDesignSpan; double Wingspan;
                                                 EServiceRole RefusedRole = EServiceRole::Aircraft; bool IsAdmitted() const; };
  namespace StandAdmission {
      FStandAdmission Judge(const FEntityInstance& Stand, const FAirframe& Airframe);
      FString Describe(const FStandAdmission& Admission);
      bool PavementAdmitsRole(EPavement P, EServiceRole Role);
  }
  // EArrivalRefusal gains: NoStandPavedEnough, NoStandServiceable
  ```

- [ ] **Step 1: Write the failing tests** in the new `StandAdmissionTest.cpp`:
  ```cpp
  #include "CoreMinimal.h"
  #include "AirsideTestFixtures.h"
  #include "Content/AirsideSettings.h"
  #include "Misc/AutomationTest.h"
  #include "Model/RoadEntity.h"
  #include "Model/StandAdmission.h"
  #include "Solve/IcaoCode.h"

  #if WITH_DEV_AUTOMATION_TESTS

  namespace
  {
      FEntityInstance StandOf(EIcaoCode Letter, EPavement P)
      {
          FEntityInstance Stand;
          Stand.bAlive = true;
          Stand.DesignWingspan = IcaoCode::DesignSpanForLetter(Letter);
          Stand.Pavement = P;
          return Stand;
      }
  }

  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandAdmissionGrassRefusesTarmacAircraftTest,
      "Airside.Model.StandAdmission.GrassStandRefusesTarmacAircraft",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FStandAdmissionGrassRefusesTarmacAircraftTest::RunTest(const FString&)
  {
      // THE USER'S CASE: an F stand an A380 fits on, laid on grass, is not usable by it. Size
      // alone - the rule this replaces - admitted it.
      FAirframe Heavy = UAirsideSettings::ResolveDefaultAirframe();
      Heavy.Wingspan = 7980.0;
      Heavy.MinimumPavement = EPavement::Concrete;
      const FStandAdmission A = StandAdmission::Judge(StandOf(EIcaoCode::F, EPavement::Grass), Heavy);
      TestEqual(TEXT("refused"), A.Why, EStandRefusal::Surface);
      TestEqual(TEXT("with the runway's own sentence"), StandAdmission::Describe(A),
          FString(TEXT("the surface is grass; this aircraft needs concrete")));
      TestTrue(TEXT("and admitted once paved"), StandAdmission::Judge(StandOf(EIcaoCode::F, EPavement::Concrete), Heavy).IsAdmitted());
      return true;
  }

  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandAdmissionSurfaceBeatsSizeTest, "Airside.Model.StandAdmission.SurfaceBeatsSize",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FStandAdmissionSurfaceBeatsSizeTest::RunTest(const FString&)
  {
      // FIRST REFUSAL WINS, surface first - runway admission's order and reason: drawing the
      // stand bigger does not fix its pavement.
      FAirframe Heavy = UAirsideSettings::ResolveDefaultAirframe();
      Heavy.Wingspan = 7980.0;
      Heavy.MinimumPavement = EPavement::Concrete;
      TestEqual(TEXT("too small AND too soft reports surface"),
          StandAdmission::Judge(StandOf(EIcaoCode::B, EPavement::Grass), Heavy).Why, EStandRefusal::Surface);
      TestEqual(TEXT("too small on the right pavement reports size"),
          StandAdmission::Judge(StandOf(EIcaoCode::B, EPavement::Concrete), Heavy).Why, EStandRefusal::TooSmall);
      return true;
  }

  IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandAdmissionEveryRoleTest, "Airside.Model.StandAdmission.EveryRoleWorksOnEveryPavement",
      EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
  bool FStandAdmissionEveryRoleTest::RunTest(const FString&)
  {
      // PINS TODAY'S RULING (user, 2026-09-27): every service works on every pavement. This goes
      // red BY DESIGN the day grass restricts a role - update it with that ruling, do not delete it.
      for (uint8 P = 0; P < static_cast<uint8>(EPavement::Count); ++P)
      {
          for (const EServiceRole Role : TEnumRange<EServiceRole>())
          {
              TestTrue(FString::Printf(TEXT("%s works on %s"), *UEnum::GetValueAsString(Role), Pavement::Name(static_cast<EPavement>(P))),
                  StandAdmission::PavementAdmitsRole(static_cast<EPavement>(P), Role));
          }
      }
      return true;
  }

  #endif
  ```
  (`TEnumRange<EServiceRole>` needs `ENUM_RANGE_BY_COUNT`/`ENUM_RANGE_BY_FIRST_AND_LAST` on the enum - check `grep -n "ENUM_RANGE" Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h`; if absent, iterate `StaticEnum<EServiceRole>()->NumEnums() - 1`. `IcaoCode::DesignSpanForLetter` exists at IcaoCode.cpp:290.)
  In `ArrivalPlannerTest.cpp` (Review Focus 3 and the composition-level seam test):
  ```cpp
  // THROUGH THE PLANNER, not the judge: an arrival whose only stand is big enough but grass is
  // told to PAVE it - not to draw a bigger stand, and not to build a taxiway that already reaches it.
  // <build the network ArrivalPlannerTest's existing "NoStandBigEnough" case builds, with its one stand
  //  sized for the aircraft and Pavement = Grass, and the airframe's MinimumPavement = Tarmac>
  const FArrivalPlan Plan = ArrivalPlanner::Plan(*Net, Near, Airframe, nullptr);
  TestEqual(TEXT("refused for pavement"), Plan.Why, EArrivalRefusal::NoStandPavedEnough);
  ```
  Copy the existing NoStandBigEnough test's setup verbatim (grep `NoStandBigEnough` in `ArrivalPlannerTest.cpp`) and change only the stand's pavement and the airframe's need.
  In `StandAllocatorTest.cpp`: a flight needing tarmac is not reserved a grass stand of its size, and IS reserved the tarmac one beside it.
- [ ] **Step 2: Build TWICE (new test file); run `-Filter Airside.Model.StandAdmission`.** Expected: compile FAILURE on `StandAdmission.h`.
- [ ] **Step 3: Implement `StandAdmission.h/.cpp`.** Header comment: mirrors RunwayAdmission's shape; shares only `FPavementCheck` (Task 2's Value Object comment explains why not a Specification). `Judge`:
  ```cpp
  FStandAdmission StandAdmission::Judge(const FEntityInstance& Stand, const FAirframe& Airframe)
  {
      FStandAdmission Out;
      Out.Pavement = Pavement::Judge(Stand.Pavement, Airframe.MinimumPavement);
      Out.StandDesignSpan = Stand.DesignWingspan;
      Out.Wingspan = Airframe.Wingspan;

      // SURFACE, SIZE, SERVICE - first wins. Surface first for RunwayAdmission's reason: it is
      // the fact a player cannot fix by drawing the stand bigger.
      if (!Out.Pavement.Passes())
      {
          Out.Why = EStandRefusal::Surface;
      }
      // IcaoCode::StandAdmits CALLED, not re-implemented - its "unknown admits anything" and
      // "wider than F is never admitted" live there. ENFORCED BY: Check-Architecture rule 4 row
      // 'IcaoCode::StandAdmits'
      else if (!IcaoCode::StandAdmits(Stand.DesignWingspan, Airframe.Wingspan))
      {
          Out.Why = EStandRefusal::TooSmall;
      }
      else
      {
          // THE SERVICE HOOK (user, 2026-09-27): consumed here so restricting a role on grass
          // later is one function body, not a new call site. Never refuses today.
          for (const FResolvedAnchor& Anchor : Stand.ResolvedAnchors)
          {
              if (!PavementAdmitsRole(Stand.Pavement, Anchor.Role))
              {
                  Out.Why = EStandRefusal::Service;
                  Out.RefusedRole = Anchor.Role;
                  break;
              }
          }
      }
      return Out;
  }
  ```
  `Describe`: Surface -> `Pavement::Describe(Out.Pavement)`; TooSmall -> `FString::Printf(TEXT("the stand is Code %s; this aircraft needs Code %s"), *IcaoCode::LetterForWingspan(StandDesignSpan), *IcaoCode::LetterForWingspan(Wingspan))`; Service -> `FString::Printf(TEXT("%s cannot work on %s"), *UEnum::GetDisplayValueAsText(RefusedRole).ToString(), Pavement::Name(Pavement.Have))`; None -> empty. `PavementAdmitsRole` returns `true` with a comment naming the ruling and date.
- [ ] **Step 4: The planner.** `EArrivalRefusal` gains, after `NoStandBigEnough`, with doc comments in its style:
  ```cpp
  /** Stands exist and at least one is big enough, but every big-enough one is paved too
   *  weakly for this aircraft. The player's fix: pave a stand, not draw a bigger one. */
  NoStandPavedEnough,

  /** A stand is big enough and paved enough, but a service this aircraft needs cannot work on
   *  its pavement. Unreachable until StandAdmission::PavementAdmitsRole restricts a role. */
  NoStandServiceable,
  ```
  `ChooseStand` builds a parallel `TArray<const FEntityInstance*> CandidateStand` beside `CandidateSpan` (same filter, same push - keep the "kept parallel by construction" comment and extend it), and its admission test becomes `const FStandAdmission Admission = StandAdmission::Judge(*CandidateStand[Index], Airframe); if (!Admission.IsAdmitted()) { ++RefusedCount[Admission.Why]; continue; }` - the `ChooseStand:` log line keeps its format and appends `, %d unpaved` (Surface count). `EveryStandTooSmall` becomes `EStandRefusal WhyEveryStandRefused(Network, Airframe)` returning `None` if any stand admits, else: `Service` if any stand was refused only for service, else `Surface` if any stand passes size (so paving fixes it), else `TooSmall`. The caller at `:328` maps `TooSmall -> NoStandBigEnough`, `Surface -> NoStandPavedEnough`, `Service -> NoStandServiceable`; the "Three refusals for three fixes" comment grows to name the fourth. `DescribeRefusal` (`:370-440`) gains the two cases, in the existing wording style ("no stand is paved for this aircraft - pave one", "no stand can be serviced on its pavement").
- [ ] **Step 5: The allocator.** `StandAllocator.cpp:38`: `!IcaoCode::StandAdmits(Stand.DesignWingspan, Wingspan)` -> `!StandAdmission::Judge(Stand, Flight.Airframe).IsAdmitted()`; the "RANK, NOT THE RAW SPAN" comment is updated to say admission is `StandAdmission::Judge`, the one rule ChooseStand also calls, and ranking stays `IcaoCode::StandRank`.
- [ ] **Step 6: Build; run `-Filter Airside.Model.StandAdmission`, `-Filter Airside.Model.ArrivalPlanner`, `-Filter AirportOps.Model.StandAllocator`.** Expected: pass; confirm the three new StandAdmission tests appear in the run count.
- [ ] **Step 7: Commit.** `feat(stands): stand admission judges surface, size and service; arrivals say which`

---

### Task 10: Delete the M_Starter stand

**Files:** `Content/Maps/M_Starter.umap`; `AirsideTests/Private/StarterMapProbeTest.cpp` if it asserts the stand.

- [ ] **Step 1: See what asserts it.** `grep -n -i "stand" Plugins/Airside/Source/AirsideTests/Private/StarterMapProbeTest.cpp`. Any assertion that the map HAS a stand is changed to assert it has none (reason: "deleted 2026-09-27 - drawn fresh on the pavement scale").
- [ ] **Step 2: Delete the stand headlessly** (editor closed on this worktree; memory `unreal-editing-levels-headlessly`: a locked .umap reports success and writes nothing). Remove the one stand `FEntityInstance` from the level's `ARoadNetworkActor` network through the facade's delete-entity path, save the level.
- [ ] **Step 3: Verify on disk with a control.** `python -c "print(open(r'Content/Maps/M_Starter.umap','rb').read().count(b'DA_Stand_CodeC'))"` before and after (expect the count to fall to 0 if the stand referenced it; if it was 0 before, pick the name table entry the probe test reads instead). Run `-Filter StarterMap`. Expected: pass.
- [ ] **Step 4: Commit.** `chore(map): delete M_Starter's stand - stands are drawn on the pavement scale now`

---

### Task 11: Check-Architecture rules for the three shapes removed

**Files:** `Tools/Check-Architecture.ps1` (rule 4's `$AllowedCallers` table, and a new rule 23 before `# --- Verdict`)

- [ ] **Step 1: Add two rows to `$AllowedCallers`** (rule 4), after the last existing row:
  ```powershell
  @{
      # ONE FACTOR SITE (2026-09-27, shared pavement): the pavement factor meets a rate in
      # FBuildLine::Amount and in BuildCost's upkeep, nowhere else - a factor per buildable
      # kind is how roads came to be cheaper on grass while runways were not.
      Name        = 'Pavement::RateFactor'
      Pattern     = '\bRateFactor\s*\('
      ProdAllowed = @('Public\Model\Pavement.h', 'Private\Model\Pavement.cpp', 'Private\Model\BuildPurse.cpp', 'Private\Build\BuildCost.cpp')
      TestExempt  = $true
      ProdReason  = 'price through a FBuildLine (BuildCost::For*), which applies the factor once'
  },
  @{
      # ONE STAND ADMISSION (2026-09-27): size alone admitted an A380 to a grass F stand.
      Name        = 'IcaoCode::StandAdmits'
      Pattern     = '\bStandAdmits\s*\('
      ProdAllowed = @('Public\Solve\IcaoCode.h', 'Private\Solve\IcaoCode.cpp', 'Private\Model\StandAdmission.cpp')
      TestExempt  = $true
      ProdReason  = 'admit a stand through StandAdmission::Judge, which also checks its pavement and services'
  }
  ```
- [ ] **Step 2: Add rule 23** (renumber if a rule 23 exists by then):
  ```powershell
  # --- 23. One pavement scale: no second surface enum ------------------------------------------
  # 2026-09-27, shared pavement: ERoadSurface was a second surface enum beside ERunwaySurface,
  # mapped onto it by hand, so a road's grass and a runway's grass were two values that happened
  # to agree. EPavement in Model/Pavement.h is the one scale; a buildable that offers fewer steps
  # says so with a list (URoadProfile::AllowedPavements), not a new enum.
  foreach ($module in $modules) {
      foreach ($file in Get-Sources $module @('.h')) {
          foreach ($h in (Select-String -Path $file.FullName -Pattern '\benum\s+class\s+E\w*(Surface|Pavement)\b')) {
              if ($file.FullName -like '*\Public\Model\Pavement.h') { continue }
              $failures.Add("one-pavement-scale: $($file.FullName):$($h.LineNumber) declares a second surface scale; use EPavement and an allowed list: $($h.Line.Trim())")
          }
      }
  }
  $ranRules.Add('one-pavement-scale')
  ```
  Before adding it, run `grep -rn "enum class E\w*Surface\b" Plugins --include=*.h`: `FWantedClaim::ESurface` (TrafficClaims) is a nested enum named `ESurface` - the pattern `E\w*(Surface|Pavement)\b` matches `ESurface`. Exclude it explicitly by adding `if ($h.Line -match '\bESurface\b') { continue }` with a comment saying it is a traffic-claim kind (runway edge / holding position), not ground.
- [ ] **Step 3: Prove each rule can fail** (memory `a-green-test-may-measure-nothing`): temporarily add `enum class ERoadSurface : uint8 { A };` to any Airside header and a `Pavement::RateFactor(EPavement::Grass);` call and a `IcaoCode::StandAdmits(1.0, 1.0);` call to `ArrivalPlanner.cpp`; run `./Tools/Check-Architecture.ps1`. Expected: three failures naming the three rules. Revert; run again. Expected: the verdict line prints and passes (memory `check-architecture-can-stop-parsing-silently`: no verdict line = it did not run).
- [ ] **Step 4: Commit.** `build(lint): one pavement scale, one factor site, one stand admission`

---

### Task 12: Whole-branch verification and PR

- [ ] **Step 1: Full build and full test run** (no filter). Expected: `Result: Succeeded`; `N test(s) run, 0 failed, 0 crashed` against Task 0's baseline (run count = baseline + new tests - deleted duplicates from Task 7 Step 6).
- [ ] **Step 2: Refactor-contract counts.** Rerun Task 0 Step 5's `UE_LOG(` count; it must be >= baseline (Task 3 adds and then removes one; Task 8 extends an existing line). Comment lines in touched files: `git diff origin/main --stat` plus `git diff origin/main -U0 | grep -c "^-\s*//\|^-\s*\*"` vs `grep -c "^+\s*//\|^+\s*\*"` - added >= removed.
- [ ] **Step 3: Grep the added comments for contracts** (memory `review-own-comments-for-contracts`): every new "the only", "never", "nothing else" claim carries `// ENFORCED BY:` naming a test or rule that exists.
- [ ] **Step 4: Blueprints still compile** (memory `unreal-compile-all-blueprints-headless`) - the enum rename touches BP-visible types. Run the CompileAllBlueprints commandlet on this worktree; expected: no errors naming `EPavement`/`ERunwaySurface`.
- [ ] **Step 5: Push and open the PR** to `main`, filling the template: build line, test line, `UE_LOG`/comment deltas, the list of moved/deleted A-stand tests, the stand pad screenshot from Task 8 Step 9, and the verification step for the user: "PIE on M_Starter: draw a grass B stand and a tarmac B stand; dispatch an arrival needing tarmac (the default fleet's `MinimumPavement`, per `Airside.Content.AircraftMinimumPavementMigrated`); expected `ChooseStand: ... -> node <tarmac stand's>` in `LogAirside`, and the grass stand's build cost in the ledger log at 0.4 of the pad's tarmac price."
