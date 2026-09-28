# Taxiway Clearance Strip - Stages 1-2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every taxiway has a derived clearance strip; stands are drawn behind it, refused inside it, and closed to new arrivals when an existing one sits in it.

**Architecture:** The strip width is pure geometry in `Solve/IcaoCode` (letter from pavement width, strip from letter). A world-free `Model/TaxiwayStrip` query answers "which strips does this footprint intrude on, by how much". The stand tool anchors its entrance at the strip edge (so a committed outline is still the parking box alone and every `StandBox` reader is untouched); `WhyStandRefused` and `StandAdmission::Judge` each ask the query once.

**Tech Stack:** UE 5.8.2 C++, Airside plugin, UE automation tests (`Airside.*`).

**Spec:** `docs/superpowers/specs/2026-09-28-taxiway-clearance-strip-design.md` (stages 1 and 2). Stages 3-6 get their own plans.

## Deviations from the spec, found while planning (spec updated in Task 0)

1. **The taxiway's letter comes from its PAVEMENT WIDTH, not `URoadProfile::MaxWingspan`.** No such profile field exists; `FProfileGuideline::MaxWingspan` is 0 (unlimited) on every taxiway guideline. Rule: the LARGEST letter whose ICAO minimum taxiway width the pavement meets (Annex 14 figures are minimums - `build_road_profiles.py` rounds UP for that reason). Nearest-width, the runway rule, fails here: the game's 24 m standard taxiway is exactly between E's 23 and F's 25.
2. **The committed stand outline is the parking box alone.** The tool anchors the entrance at pavement edge + strip; the strip is the ground between. The spec's "the drag's first strip-width is the strip" is the same thing seen by the player; the data never contains the strip.
3. **Stage 1's debug overlay is dropped.** The stand tool's gap IS the visible strip in stage 2; a separate overlay is presentation nothing else needs yet (YAGNI). Drawing the strip itself is a live visual iteration after Task 6.

## Global Constraints

- `Solve/` includes `CoreMinimal.h` and `Solve/` only.
- `Model/` is world-free: tests build it with `NewObject<URoadNetwork>()`, no world.
- `UENUM` values are APPENDED, never inserted - renumbering repoints serialised values.
- Comments say WHY, and why the obvious alternative was rejected. A comment asserting a fact about other code carries `// ENFORCED BY: <test name>` within 3 lines (Check-Architecture rule 12).
- A doc comment touches its declaration.
- No new `DEFINE_LOG_CATEGORY_STATIC`; use `LogAirside` / existing categories.
- Build (worktree, editor may be open elsewhere):
  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
    -Project="C:\repos\airportmgr2-taxiway-clearance-strip\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
  ```
- Tests: `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\airportmgr2-taxiway-clearance-strip\AirportMgr.uproject" -Filter <prefix>`; read its `N test(s) run, N failed, N crashed` line, never the exit code. A NEW test .cpp needs two builds (the first says Succeeded without compiling it) - this plan adds tests to existing files where it can.
- Strip figures (uu), pinned by tests: B 900, C 1450, D 2450, E 2800, F 3450 for pavements 1200/1600/1800/2400/2600.

## Review Focus

1. **A stand at a junction** touches two taxiways' strips - the worst (deepest) intrusion is the one reported; Task 2 test "two taxiways, the deeper wins".
2. **A curved taxiway** - the strip follows the curve, not the chord; Task 2 test "bend".
3. **An aircraft already parked on a stand that becomes invalid** must finish its turnaround (user ruling). `GroundTrafficRebuild.cpp:936` re-chooses stands on rebuild; Task 5 Step 6 reads it and pins parked-agent behaviour.
4. **A stand drawn exactly at the strip edge** must NOT be refused by float noise; `ToleranceUu` = 1 uu, Task 2 test "flush at the edge".
5. **Service roads and runways** have no strip - a stand beside a runway is not this rule's business; Task 2 tests them.

---

### Task 0: Correct the spec

**Files:**
- Modify: `docs/superpowers/specs/2026-09-28-taxiway-clearance-strip-design.md`

- [ ] **Step 1:** In "The strip", replace "sized by the TAXIWAY's letter - the largest wingspan it admits (`URoadProfile::MaxWingspan`)" with "sized by the TAXIWAY's letter, read from its pavement width: the largest letter whose ICAO minimum taxiway width (A 7.5, B 10.5, C 15, D 18, E 23, F 25 m) the pavement meets. Not `MaxWingspan` - taxiway guidelines carry 0 (unlimited). Not nearest-width, the runway rule: 24 m sits exactly between E and F." In the formula, `MaxWingspan/2` becomes `MaxWingspanForLetter(letter)/2`.
- [ ] **Step 2:** In "Stands", after "The player still drags from the pavement edge", add: "Implemented as: the tool anchors the entrance at pavement edge + strip, so the committed outline is the parking box alone and no `StandBox` reader changes."
- [ ] **Step 3:** In "Stages", stage 1: replace "a debug overlay draws the strips. Visible: the player can see the zones on the current map." with "No overlay - stage 2's gap is the visible strip."
- [ ] **Step 4:** Commit: `git commit -am "docs(spec): taxiway letter from pavement width; outline stays the box"`

---

### Task 1: Taxiway letter and strip width in `IcaoCode`

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/IcaoCode.h` (declarations beside `MaxWingspanForWidth`, ~line 150)
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/IcaoCode.cpp` (`FRow` ~line 10, `Rows[]` ~line 111, functions after `MaxWingspanForWidth` ~line 317)
- Test: `Plugins/Airside/Source/AirsideTests/Private/IcaoCodeTest.cpp` (new `IMPLEMENT_SIMPLE_AUTOMATION_TEST` at file end, before `#endif`)

**Interfaces:**
- Produces:
  - `double IcaoCode::TaxiwayWidthForLetter(EIcaoCode Code)` - ICAO minimum, uu.
  - `EIcaoCode IcaoCode::TaxiwayLetterForWidth(double PavementWidthUu)`
  - `double IcaoCode::TaxiwayStripForWidth(double PavementWidthUu)` - strip each side, uu.

- [ ] **Step 1: Write the failing test** (append to `IcaoCodeTest.cpp`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FIcaoCodeTaxiwayStripTest,
	"Airside.Solve.IcaoCode.TaxiwayStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FIcaoCodeTaxiwayStripTest::RunTest(const FString& Parameters)
{
	// THE GAME'S OWN FIVE WIDTHS (build_road_profiles.py TAXIWAY_WIDTHS, 2026-09-28) read as
	// their own letters. 2400 is the one that matters: nearest-width would call it a tie
	// between E (23 m) and F (25 m); "largest minimum met" says E, which is what it was
	// rounded up from.
	TestEqual(TEXT("12 m is B"), static_cast<int32>(IcaoCode::TaxiwayLetterForWidth(1200.0)), static_cast<int32>(EIcaoCode::B));
	TestEqual(TEXT("16 m is C"), static_cast<int32>(IcaoCode::TaxiwayLetterForWidth(1600.0)), static_cast<int32>(EIcaoCode::C));
	TestEqual(TEXT("18 m is D"), static_cast<int32>(IcaoCode::TaxiwayLetterForWidth(1800.0)), static_cast<int32>(EIcaoCode::D));
	TestEqual(TEXT("24 m is E, not F - it meets 23, not 25"), static_cast<int32>(IcaoCode::TaxiwayLetterForWidth(2400.0)), static_cast<int32>(EIcaoCode::E));
	TestEqual(TEXT("26 m is F"), static_cast<int32>(IcaoCode::TaxiwayLetterForWidth(2600.0)), static_cast<int32>(EIcaoCode::F));
	TestEqual(TEXT("the old 23 m standard is E"), static_cast<int32>(IcaoCode::TaxiwayLetterForWidth(2300.0)), static_cast<int32>(EIcaoCode::E));
	TestEqual(TEXT("under every minimum is still A - the least a taxiway can be"),
		IcaoCode::TaxiwayLetterForWidth(500.0), EIcaoCode::A);

	// THE STRIP: half the letter's widest wing, less half the pavement, plus the letter's
	// wingtip clearance. These five are the spec's table - if one moves, the table in the
	// spec and AIRCRAFT.md are wrong too.
	TestEqual(TEXT("B strip 9 m"), IcaoCode::TaxiwayStripForWidth(1200.0), 900.0, 0.5);
	TestEqual(TEXT("C strip 14.5 m"), IcaoCode::TaxiwayStripForWidth(1600.0), 1450.0, 0.5);
	TestEqual(TEXT("D strip 24.5 m"), IcaoCode::TaxiwayStripForWidth(1800.0), 2450.0, 0.5);
	TestEqual(TEXT("E strip 28 m"), IcaoCode::TaxiwayStripForWidth(2400.0), 2800.0, 0.5);
	TestEqual(TEXT("F strip 34.5 m"), IcaoCode::TaxiwayStripForWidth(2600.0), 3450.0, 0.5);

	// A PAVEMENT WIDER THAN ITS LETTER'S SPAN never yields a negative overhang: the strip
	// floors at the clearance, because nothing overhangs but the clearance is still owed.
	TestEqual(TEXT("absurdly wide A pavement: clearance only"),
		IcaoCode::TaxiwayStripForWidth(10000.0), IcaoCode::WingtipClearanceForLetter(EIcaoCode::F), 0.5);
	return true;
}
```

Note the last case: 10000 uu reads as F (meets 25 m), and F's half-span 4000 < 5000 half-width, so the overhang floors at 0 and the strip is F's clearance, 750.

- [ ] **Step 2: Build, run, verify it fails**

Build fails with "TaxiwayLetterForWidth is not a member of IcaoCode" - that is the failure.

- [ ] **Step 3: Add the column and functions**

In `FRow`, directly after `double RunwayWidth;` (designated initialisers must follow declaration order):

```cpp
			/**
			 * ICAO Annex 14's MINIMUM taxiway width for the letter, uu - 7.5/10.5/15/18/23/25 m.
			 * The ICAO figure, not the game's even-metre rounding (build_road_profiles.py
			 * TAXIWAY_WIDTHS rounds UP from these), for the reason RunwayWidth keeps ICAO's:
			 * the rounded widths are read back through TaxiwayLetterForWidth, and a minimum is
			 * what a wider pavement still meets.
			 */
			double TaxiwayWidth;
```

In each row, after `.RunwayWidth = ...,` insert `.TaxiwayWidth = ...,`: A `750.0`, B `1050.0`, C `1500.0`, D `1800.0`, E `2300.0`, F `2500.0`.

In `IcaoCode.h`, after `MaxWingspanForWidth`'s declaration:

```cpp
	/** ICAO Annex 14's minimum taxiway width for Code, uu. See FRow::TaxiwayWidth. */
	AIRSIDE_API double TaxiwayWidthForLetter(EIcaoCode Code);

	/**
	 * The letter a taxiway of this PAVEMENT width is built for: the largest whose ICAO minimum
	 * it meets, or A below every minimum.
	 *
	 * LARGEST MINIMUM MET, NOT NEAREST - MaxWingspanForWidth's runway rule. The game's standard
	 * 24 m taxiway is ICAO E's 23 m rounded up, and sits exactly between E and F, so nearest
	 * would decide it by a tie-break; a minimum is a floor, and 24 m meets E's and not F's.
	 * ENFORCED BY: Airside.Solve.IcaoCode.TaxiwayStrip
	 */
	AIRSIDE_API EIcaoCode TaxiwayLetterForWidth(double PavementWidthUu);

	/**
	 * The clearance strip each side of a taxiway of this pavement width, uu: how far past the
	 * pavement edge the widest wing its letter admits reaches, plus that letter's wingtip
	 * clearance. Nothing may stand in it (taxiway clearance strip spec, 2026-09-28).
	 *
	 * DERIVED, NEVER STORED, for StandWidthForLetter's reason: a stored strip would be a third
	 * figure obliged to agree with the pavement width and the span. Slightly tighter than
	 * ICAO's taxiway-to-object distances, which also allow for wandering off the centreline;
	 * accepted in the spec, and the clearance term is the knob if play says otherwise.
	 * ENFORCED BY: Airside.Solve.IcaoCode.TaxiwayStrip
	 */
	AIRSIDE_API double TaxiwayStripForWidth(double PavementWidthUu);
```

In `IcaoCode.cpp`, after `MaxWingspanForWidth`'s definition (use the file's existing `RowFor` and `Parse`):

```cpp
	double TaxiwayWidthForLetter(EIcaoCode Code)
	{
		return RowFor(Code).TaxiwayWidth;
	}

	EIcaoCode TaxiwayLetterForWidth(double PavementWidthUu)
	{
		// Half a uu of slack so a width typed as exactly a minimum, then carried through a
		// sum of band widths, still meets it.
		EIcaoCode Best = EIcaoCode::A;
		for (const FRow& Row : Rows)
		{
			if (Row.TaxiwayWidth <= PavementWidthUu + 0.5)
			{
				Best = Parse(Row.Letter).GetValue();
			}
		}
		return Best;
	}

	double TaxiwayStripForWidth(double PavementWidthUu)
	{
		const EIcaoCode Letter = TaxiwayLetterForWidth(PavementWidthUu);
		const double Overhang = FMath::Max(0.0, 0.5 * MaxWingspanForLetter(Letter) - 0.5 * PavementWidthUu);
		return Overhang + WingtipClearanceForLetter(Letter);
	}
```

(`Rows[]` is declared A to F in order - the loop relies on that; `EnvelopeFloorOrderedByLetter` already pins the table's order.)

- [ ] **Step 4: Build, run `-Filter Airside.Solve.IcaoCode`**, expect all pass including `TaxiwayStrip`.
- [ ] **Step 5: Commit** `feat(icao): taxiway letter from pavement width, derived clearance strip`

---

### Task 2: `Model/TaxiwayStrip` - the keep-out query

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Model/TaxiwayStrip.h`
- Create: `Plugins/Airside/Source/Airside/Private/Model/TaxiwayStrip.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PlotGesture.cpp:41-62` (`IsTaxiway` forwards)
- Test: `Plugins/Airside/Source/AirsideTests/Private/RoadNetworkTest.cpp` (append; an existing file, so one build compiles it)

**Interfaces:**
- Consumes: `IcaoCode::TaxiwayLetterForWidth`, `IcaoCode::TaxiwayStripForWidth` (Task 1).
- Produces:

```cpp
namespace TaxiwayStrip
{
	struct FIntrusion { FRoadSegmentId Taxiway; EIcaoCode Letter; double Required; double Depth; };
	inline constexpr double ToleranceUu = 1.0;
	AIRSIDE_API bool IsAircraftOnly(const URoadNetwork& Network, FRoadSegmentId Id);
	AIRSIDE_API bool HasStrip(const URoadNetwork& Network, FRoadSegmentId Id);
	AIRSIDE_API double StripWidthOf(const URoadNetwork& Network, FRoadSegmentId Id);
	AIRSIDE_API TOptional<FIntrusion> WorstIntrusion(const URoadNetwork& Network, TConstArrayView<FVector2D> Footprint);
}
```

- [ ] **Step 1: Write the failing test** (append to `RoadNetworkTest.cpp`; add `#include "Model/TaxiwayStrip.h"` at the top):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaxiwayStripQueryTest,
	"Airside.Model.TaxiwayStrip.Query",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiwayStripQueryTest::RunTest(const FString& Parameters)
{
	// A box 40 m wide whose near edge sits at NearY, running 40 m further from the road.
	auto BoxFrom = [](double NearY)
	{
		return TArray<FVector2D>{ { -2000.0, NearY }, { 2000.0, NearY }, { 2000.0, NearY + 4000.0 }, { -2000.0, NearY + 4000.0 } };
	};
	auto Lay = [](URoadNetwork* Net, URoadProfile* Profile, const FVector2D& Control)
	{
		const FRoadNodeId W = Net->AddNode(FVector2D(-10000.0, 0.0));
		const FRoadNodeId E = Net->AddNode(FVector2D(10000.0, 0.0));
		return Net->AddSegment(W, E, Control, Profile);
	};

	// STRAIGHT 24 m TAXIWAY: code E, pavement edge at 1200, strip edge at 1200 + 2800 = 4000.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadSegmentId Taxi = Lay(Net, URoadProfile::MakeTransient(2400.0, 1600.0), FVector2D::ZeroVector);
		TestEqual(TEXT("its strip is E's 28 m"), TaxiwayStrip::StripWidthOf(*Net, Taxi), 2800.0, 0.5);
		TestFalse(TEXT("flush at the strip edge: clear - float noise must not refuse it"),
			TaxiwayStrip::WorstIntrusion(*Net, BoxFrom(4000.0)).IsSet());
		const TOptional<TaxiwayStrip::FIntrusion> OneMetre = TaxiwayStrip::WorstIntrusion(*Net, BoxFrom(3900.0));
		if (TestTrue(TEXT("a metre inside the strip intrudes"), OneMetre.IsSet()))
		{
			TestEqual(TEXT("by a metre"), OneMetre->Depth, 100.0, 1.0);
			TestEqual(TEXT("naming the letter that set it"), static_cast<int32>(OneMetre->Letter), static_cast<int32>(EIcaoCode::E));
			TestEqual(TEXT("and what it needs"), OneMetre->Required, 2800.0, 0.5);
		}
		const TOptional<TaxiwayStrip::FIntrusion> Flush = TaxiwayStrip::WorstIntrusion(*Net, BoxFrom(1200.0));
		TestTrue(TEXT("flush to the pavement - today's stand - intrudes by the whole strip"),
			Flush.IsSet() && FMath::IsNearlyEqual(Flush->Depth, 2800.0, 1.0));
	}

	// A BEND: control (0, 10000) puts the curve's midpoint at y 5000. A chord test would see
	// the road at y 0 and pass a box at 8900; the curve's strip edge is 5000 + 1200 + 2800 = 9000.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		Lay(Net, URoadProfile::MakeTransient(2400.0, 1600.0), FVector2D(0.0, 10000.0));
		TestTrue(TEXT("the strip follows the bend, not the chord"),
			TaxiwayStrip::WorstIntrusion(*Net, BoxFrom(8900.0)).IsSet());
		TestFalse(TEXT("and clears past it"), TaxiwayStrip::WorstIntrusion(*Net, BoxFrom(9100.0)).IsSet());
	}

	// NO STRIP: a service road (trucks), and a runway (its own rules - out of scope).
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadSegmentId Road = Lay(Net, URoadProfile::MakeServiceRoadTransient(), FVector2D::ZeroVector);
		TestFalse(TEXT("a service road has no strip"), TaxiwayStrip::HasStrip(*Net, Road));
		TestFalse(TEXT("so nothing beside it intrudes"), TaxiwayStrip::WorstIntrusion(*Net, BoxFrom(400.0)).IsSet());

		URoadNetwork* RunwayNet = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* RunwayProfile = URoadProfile::MakeTransient(4600.0, 1600.0);
		RunwayProfile->bContinuousThroughJunctions = true;   // IsRunwaySegment's own rule
		const FRoadSegmentId Runway = Lay(RunwayNet, RunwayProfile, FVector2D::ZeroVector);
		TestFalse(TEXT("a runway has no taxiway strip"), TaxiwayStrip::HasStrip(*RunwayNet, Runway));
	}

	// TWO TAXIWAYS: a 12 m B along Y=0 and a 26 m F along X=6000. A box 1 m inside B's strip
	// and 10 m inside F's reports F - the deeper - so the readout names the one to fix first.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		Lay(Net, URoadProfile::MakeTransient(1200.0, 800.0), FVector2D::ZeroVector);
		const FRoadNodeId S = Net->AddNode(FVector2D(6000.0, -10000.0));
		const FRoadNodeId N = Net->AddNode(FVector2D(6000.0, 10000.0));
		const FRoadSegmentId Wide = Net->AddSegment(S, N, FVector2D(6000.0, 0.0), URoadProfile::MakeTransient(2600.0, 1733.0));
		// B strip edge at 600 + 900 = 1500; F strip edge at 6000 - 1300 - 3450 = 1250 in X.
		const TArray<FVector2D> Box{ { -2000.0, 1400.0 }, { 2250.0, 1400.0 }, { 2250.0, 5400.0 }, { -2000.0, 5400.0 } };
		const TOptional<TaxiwayStrip::FIntrusion> Worst = TaxiwayStrip::WorstIntrusion(*Net, Box);
		if (TestTrue(TEXT("the box intrudes"), Worst.IsSet()))
		{
			TestTrue(TEXT("the deeper intrusion wins"), Worst->Taxiway == Wide);
			TestEqual(TEXT("by 10 m"), Worst->Depth, 1000.0, 1.0);
		}
	}
	return true;
}
```

- [ ] **Step 2: Build; expect compile failure** ("Model/TaxiwayStrip.h: No such file").

- [ ] **Step 3: Write `TaxiwayStrip.h`**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Solve/IcaoCode.h"

class URoadNetwork;

/**
 * The clearance strip beside every taxiway, as a keep-out question: does this footprint stand
 * where a taxiing wing sweeps? (taxiway clearance strip spec, 2026-09-28)
 *
 * ONE QUERY FOR EVERYTHING PLACED - stands now, roads and building plots in stage 3 - rather
 * than a rule per object type, so a road with buildings on it needs no special case: each
 * footprint asks the same question. World-free, so it is tested with a bare URoadNetwork.
 */
namespace TaxiwayStrip
{
	/** One taxiway whose strip a footprint enters, and how far. */
	struct FIntrusion
	{
		FRoadSegmentId Taxiway;
		/** The taxiway's letter - what set the strip. */
		EIcaoCode Letter = EIcaoCode::A;
		/** The strip's width, uu - IcaoCode::TaxiwayStripForWidth of the pavement. */
		double Required = 0.0;
		/** How far inside the strip's outer edge the footprint reaches, uu. */
		double Depth = 0.0;
	};

	/**
	 * How far a footprint may reach past a strip edge before it counts, uu. One centimetre,
	 * the facade's OverlapToleranceUu, for the same reason: a stand the tool placed exactly on
	 * the edge must not be refused by the sampled curve's float noise.
	 */
	inline constexpr double ToleranceUu = 1.0;

	/**
	 * Does this segment carry aircraft and nothing else? THE taxiway-profile rule, moved here
	 * from PlotGesture::IsTaxiway (which now forwards) so Model/ can ask it - Model may not
	 * include Tool/. A runway passes this too; HasStrip is the one that excludes it.
	 */
	AIRSIDE_API bool IsAircraftOnly(const URoadNetwork& Network, FRoadSegmentId Id);

	/** A taxiway with a strip: aircraft only, and not a runway (runways have their own rules). */
	AIRSIDE_API bool HasStrip(const URoadNetwork& Network, FRoadSegmentId Id);

	/** The strip each side of this segment, uu; 0 for anything HasStrip refuses. */
	AIRSIDE_API double StripWidthOf(const URoadNetwork& Network, FRoadSegmentId Id);

	/**
	 * The deepest strip intrusion of a closed footprint polygon (any winding), or unset when it
	 * is clear of every strip by ToleranceUu. Pavement counts as strip: a footprint over the
	 * taxiway itself intrudes by the whole strip and more.
	 *
	 * DEEPEST, NOT ALL: every caller so far reports one reason, and the deepest is the one to
	 * fix first. A caller that needs the list is the day this grows one.
	 */
	AIRSIDE_API TOptional<FIntrusion> WorstIntrusion(const URoadNetwork& Network, TConstArrayView<FVector2D> Footprint);
}
```

- [ ] **Step 4: Write `TaxiwayStrip.cpp`**

```cpp
#include "Model/TaxiwayStrip.h"

#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

namespace TaxiwayStrip
{
	namespace
	{
		double PointToSegment(const FVector2D& P, const FVector2D& A, const FVector2D& B)
		{
			const double T = RoadGeom::ClosestPointOnSegment(A, B, P);
			return FVector2D::Distance(P, A + (B - A) * T);
		}

		/** Least distance between two segments: zero if they cross, else the least of the four
		 *  end-to-segment distances (exact for 2D segments that do not cross). */
		double SegmentToSegment(const FVector2D& A0, const FVector2D& A1, const FVector2D& B0, const FVector2D& B1)
		{
			const auto Side = [](const FVector2D& O, const FVector2D& D, const FVector2D& P)
			{
				return FVector2D::CrossProduct(D - O, P - O);
			};
			if (Side(A0, A1, B0) * Side(A0, A1, B1) < 0.0 && Side(B0, B1, A0) * Side(B0, B1, A1) < 0.0)
			{
				return 0.0;
			}
			return FMath::Min(FMath::Min(PointToSegment(A0, B0, B1), PointToSegment(A1, B0, B1)),
				FMath::Min(PointToSegment(B0, A0, A1), PointToSegment(B1, A0, A1)));
		}
	}

	bool IsAircraftOnly(const URoadNetwork& Network, FRoadSegmentId Id)
	{
		const FRoadSegment* Segment = Network.GetSegment(Id);
		if (Segment == nullptr || Segment->Profile == nullptr)
		{
			return false;
		}

		// BOTH HALVES, because either alone admits the wrong ground. An aircraft line alone
		// would admit a mixed cross-section a truck also drives (a stand opening onto a
		// service road is exactly what the stand tool refuses); no truck line alone would
		// admit a profile with no lines at all, which nothing can taxi on.
		bool bAircraft = false;
		for (const FProfileGuideline& Guideline : Segment->Profile->Guidelines)
		{
			if (Guideline.Class == ETraversalClass::GroundVehicle)
			{
				return false;
			}
			bAircraft |= Guideline.Class == ETraversalClass::Aircraft;
		}
		return bAircraft;
	}

	bool HasStrip(const URoadNetwork& Network, FRoadSegmentId Id)
	{
		return IsAircraftOnly(Network, Id) && !Network.IsRunwaySegment(Id);
	}

	double StripWidthOf(const URoadNetwork& Network, FRoadSegmentId Id)
	{
		if (!HasStrip(Network, Id))
		{
			return 0.0;
		}
		return IcaoCode::TaxiwayStripForWidth(Network.GetSegment(Id)->Profile->GetTotalWidth());
	}

	TOptional<FIntrusion> WorstIntrusion(const URoadNetwork& Network, TConstArrayView<FVector2D> Footprint)
	{
		TOptional<FIntrusion> Worst;
		if (Footprint.Num() < 3)
		{
			return Worst;
		}

		// EVERY LIVE SEGMENT, LINEARLY. Callers are a placement readout (once a frame) and stand
		// admission (once per candidate per plan); the implementer records M_Test's segment
		// count and the date here when Task 5 measures it.
		const TArray<FRoadSegment>& Segments = Network.GetSegments();
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FRoadSegmentId Id = Network.SegmentIdAt(Index);
			if (!Id.IsSet() || !HasStrip(Network, Id))
			{
				continue;
			}
			const FRoadSegment& Segment = Segments[Index];
			FVector2D A, B;
			if (!Network.SegmentEnds(Id, A, B))
			{
				continue;
			}

			// THE CURVE, SAMPLED - not the chord A-B, which on a bend sits a whole sagitta away
			// from the pavement the wing actually follows. GuidelineGeom::Eval is the one
			// quadratic evaluator; DefaultSamples is its own chord-error budget.
			TArray<FVector2D> Centre;
			for (int32 S = 0; S <= GuidelineGeom::DefaultSamples; ++S)
			{
				Centre.Add(GuidelineGeom::Eval(A, Segment.Control, B,
					static_cast<double>(S) / GuidelineGeom::DefaultSamples));
			}

			double Nearest = DBL_MAX;
			for (const FVector2D& P : Centre)
			{
				if (RoadGeom::PointInPolygon(Footprint, P)) { Nearest = 0.0; break; }
			}
			for (int32 C = 0; C + 1 < Centre.Num() && Nearest > 0.0; ++C)
			{
				for (int32 E = 0; E < Footprint.Num(); ++E)
				{
					Nearest = FMath::Min(Nearest, SegmentToSegment(Centre[C], Centre[C + 1],
						Footprint[E], Footprint[(E + 1) % Footprint.Num()]));
				}
			}

			// THE WIDER HALF, so an asymmetric profile is judged on its generous side rather
			// than leaving a sliver on the narrow one uncounted.
			const double Pavement = Segment.Profile->GetTotalWidth();
			const double Strip = IcaoCode::TaxiwayStripForWidth(Pavement);
			const double Depth = Segment.Profile->GetMaxHalfWidth() + Strip - Nearest;
			if (Depth > ToleranceUu && (!Worst.IsSet() || Depth > Worst->Depth))
			{
				Worst = FIntrusion{ Id, IcaoCode::TaxiwayLetterForWidth(Pavement), Strip, Depth };
			}
		}
		return Worst;
	}
}
```

Check before compiling: `RoadGeom::PointInPolygon` takes `TArrayView<const FVector2D>` - pass `Footprint` (a `TConstArrayView` converts). `FRoadSegmentId::IsSet()` - confirm in `Model/RoadHandles.h`; `SegmentIdAt` returns unset for dead slots.

- [ ] **Step 5: Make `PlotGesture::IsTaxiway` forward.** Replace its body in `PlotGesture.cpp` with `return TaxiwayStrip::IsAircraftOnly(Network, Id);`, add `#include "Model/TaxiwayStrip.h"`, and replace the "BOTH HALVES" comment there with: `// THE RULE LIVES IN Model/TaxiwayStrip (IsAircraftOnly) since 2026-09-28, so the strip query - which Model/ owns and cannot reach Tool/ for - asks the same question the stand tool anchors by.` The moved comment travels with the code (refactor contract).

- [ ] **Step 6: Build (header added - full build); run `-Filter Airside.Model.TaxiwayStrip` and `-Filter Airside.Tool.StandPlot`** (the forwarder must not change anchoring). Expect pass.
- [ ] **Step 7: `./Tools/Check-Architecture.ps1`** - expect its verdict line, no new failures.
- [ ] **Step 8: Commit** `feat(model): TaxiwayStrip keep-out query; IsTaxiway rule moves to Model`

---

### Task 3: The stand tool anchors behind the strip

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/PlotGesture.h:125-134` (`AnchorAt`, `DescribeAnchors`)
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/PlotGesture.cpp:274` and `:343` (the two `KerbOffset` uses)
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/StagedPlotTool.h` (new virtual), `Private/Tool/StagedPlotTool.cpp:80,220,280` (pass it)
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/StandPlotTool.h`, `Private/Tool/StandPlotTool.cpp` (override)
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandPlotToolTest.cpp` (append)

**Interfaces:**
- Consumes: `TaxiwayStrip::StripWidthOf` (Task 2).
- Produces: `PlotGesture::FRoadSetback` = `TFunctionRef<double(const URoadNetwork&, FRoadSegmentId)>`; `AnchorAt(Network, Cursor, Accept, Setback, Out, Grid)`; `DescribeAnchors(Network, Cursor, Accept, Setback, Sink, Grid)`; `virtual double FStagedPlotTool::FrontSetback(const URoadNetwork&, FRoadSegmentId) const`.

- [ ] **Step 1: Write the failing test** (append to `StandPlotToolTest.cpp`; add `#include "Model/TaxiwayStrip.h"`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandPlotEntranceBehindStripTest,
	"Airside.Tool.StandPlot.EntranceBehindStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandPlotEntranceBehindStripTest::RunTest(const FString& Parameters)
{
	using namespace StandPlotToolFixture;

	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	TaxiwayWorld(Actor);

	const FRoadSegmentId Taxi = Actor->Network->SegmentIdAt(0);
	const FRoadSegment* Segment = Actor->Network->GetSegment(Taxi);
	if (!TestNotNull(TEXT("the taxiway"), Segment)) { return false; }
	const double Strip = TaxiwayStrip::StripWidthOf(*Actor->Network, Taxi);
	TestTrue(TEXT("the fixture's taxiway has a strip at all"), Strip > 0.0);

	// THE ENTRANCE SITS A STRIP BEYOND THE KERB, so the box the player draws - and the
	// outline committed - is the parking box alone, and a taxiing wing clears the tail.
	FStandPlotTool Tool;
	Tool.OnClick(At(Actor, AnchorCursor));
	const FVector2D Anchor = PinnedAnchor(Tool, Actor);
	TestEqual(TEXT("entrance at pavement edge + strip"),
		Anchor.Y, Segment->Profile->GetHalfWidthLeft() + Strip, 1.0);

	// AND THE COMMITTED STAND IS CLEAR OF IT - the query that will refuse (Task 4) agrees
	// with the tool that places, measured, not restated.
	Tool.OnClick(At(Actor, Anchor + FVector2D(ReachableWidthAtLeast(IcaoCode::StandWidthForLetter(EIcaoCode::C)), 0.0)));
	TArray<FVector2D> Shown;
	Tool.Rect(At(Actor, Anchor + FVector2D(0.0, IcaoCode::StandDepthForLetter(EIcaoCode::C))), Shown);
	TestFalse(TEXT("the drawn box intrudes on no strip"),
		TaxiwayStrip::WorstIntrusion(*Actor->Network, Shown).IsSet());
	return true;
}
```

The fixture's taxiway may be left-or-right of the segment direction; if `Anchor.Y` lands on the right half, swap to `GetHalfWidthRight()` - the West->East segment's left (PerpCCW of +X) is +Y, so Left is expected.

- [ ] **Step 2: Build, run `-Filter Airside.Tool.StandPlot.EntranceBehindStrip`**; expect FAIL "entrance at pavement edge + strip" (Anchor.Y is the half width only).

- [ ] **Step 3: Thread the setback through `PlotGesture`.** In `PlotGesture.h`, beside `FRoadFilter`:

```cpp
	/**
	 * How far beyond the kerb a gesture's frontage stands on this road, uu. The stand tool's
	 * is the taxiway's clearance strip (TaxiwayStrip::StripWidthOf); the depot's is 0 until
	 * stage 3 of the strip spec asks it. A FUNCTION OF THE ROAD, not a number, because the
	 * anchor search picks the road - the caller cannot know which before it is found.
	 */
	using FRoadSetback = TFunctionRef<double(const URoadNetwork&, FRoadSegmentId)>;
```

Add `FRoadSetback Setback` after `FRoadFilter Accept` in BOTH `AnchorAt` and `DescribeAnchors` (declaration and definition). In `PlotGesture.cpp`:
- line ~274: `Anchor.Corner += Anchor.Inward * (KerbOffset(Network, Road, Side >= 0.0) + Setback(Network, Road));`
- line ~343: `... * (KerbOffset(Network, Anchor.Road, bLeft) + Setback(Network, Anchor.Road));`
Extend the "OFF THE CARRIAGEWAY" comment: `// AND PAST THE CLEARANCE STRIP where the caller has one (Setback) - a stand's entrance sits where a taxiing wing no longer reaches (strip spec 2026-09-28).`

- [ ] **Step 4: `FStagedPlotTool` asks the tool.** In `StagedPlotTool.h`, after `Filter`:

```cpp
	/**
	 * How far past the kerb this tool's frontage stands on Id, uu - PlotGesture::FRoadSetback.
	 * ZERO BY DEFAULT because the depot tool's plot is not yet held to the strip (stage 3 of
	 * the strip spec); the stand tool overrides it. Not pure: a pure virtual would force the
	 * depot to state a rule that spec has not written yet.
	 */
	virtual double FrontSetback(const URoadNetwork& Network, FRoadSegmentId Id) const { return 0.0; }
```

At each of the three `AnchorAt`/`DescribeAnchors` calls in `StagedPlotTool.cpp`, pass after `Accept`:
`[this](const URoadNetwork& N, FRoadSegmentId Id) { return FrontSetback(N, Id); }`

- [ ] **Step 5: Stand override.** `StandPlotTool.h`: `virtual double FrontSetback(const URoadNetwork& Network, FRoadSegmentId Id) const override;` `StandPlotTool.cpp` (+ `#include "Model/TaxiwayStrip.h"`):

```cpp
double FStandPlotTool::FrontSetback(const URoadNetwork& Network, FRoadSegmentId Id) const
{
	// THE TAXIWAY'S STRIP, BY ITS OWN LETTER - not the stand's. The wing that overhangs the
	// stand edge belongs to what taxis past, so a Code B stand off a Code F taxiway pays F's
	// 34.5 m (strip spec 2026-09-28; the cost nudges the player to a B spur, as real aprons do).
	// ENFORCED BY: Airside.Tool.StandPlot.EntranceBehindStrip
	return TaxiwayStrip::StripWidthOf(Network, Id);
}
```

- [ ] **Step 6: Fix every other caller the compiler names** (tests calling `PlotGesture::AnchorAt`/`DescribeAnchors` directly): pass `[](const URoadNetwork&, FRoadSegmentId) { return 0.0; }` - they test the kerb anchor, and saying 0 there states it.
- [ ] **Step 7: Build; run `-Filter Airside.Tool.StandPlot` and `-Filter Airside.Tool.PlotPlace`.** Expect the new test to pass. Existing StandPlot tests that assert positions relative to the kerb may now fail - for each, read the assertion: if it pins "entrance at kerb", update it to kerb + strip with a comment naming the spec; if it pins a letter/size from the drag, it should still pass (the drag is measured from the new anchor). List every changed assertion in the commit message.
- [ ] **Step 8: Commit** `feat(stand-tool): entrance anchors past the taxiway's clearance strip`

---

### Task 4: `WhyStandRefused` refuses a stand inside a strip

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacadeSurfaces.cpp` (`WhyStandRefused`, after the "a taxiway crosses the stand" loop, ~line 740)
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandPlotPlacementTest.cpp` (append)

**Interfaces:**
- Consumes: `TaxiwayStrip::WorstIntrusion` (Task 2).
- Produces: refusal text containing `"clearance strip"` - read by the stand tool's readout and by `PlaceStandInPlot`, which already logs `PlaceStandInPlot refused: %s`.

- [ ] **Step 1: Write the failing test.** Before writing, read the top of `StandPlotPlacementTest.cpp` for its fixture (world, `TaxiwayWorld`-like setup, how it reaches `WhyStandRefused` - `IRoadEditTarget` exposes it per `RoadEditTarget.h:424`). Append, using that file's own fixture names:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandRefusedInsideStripTest,
	"Airside.Tool.StandPlotPlacement.RefusedInsideStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandRefusedInsideStripTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->ClearNetwork();
	Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
	IRoadEditTarget* Target = Actor;
	const int32 W = Target->PlaceNode(FVector2D(-10000.0, 0.0));
	const int32 E = Target->PlaceNode(FVector2D(10000.0, 0.0));
	Target->ConnectNodes(W, E, ERoadKind::Taxiway, INDEX_NONE);

	const FRoadSegmentId Taxi = Actor->Network->SegmentIdAt(0);
	const double Kerb = Actor->Network->GetSegment(Taxi)->Profile->GetHalfWidthLeft();
	const double Strip = TaxiwayStrip::StripWidthOf(*Actor->Network, Taxi);
	const double Wd = IcaoCode::StandWidthForLetter(EIcaoCode::C);
	const double Dp = IcaoCode::StandDepthForLetter(EIcaoCode::C);
	auto Box = [&](double NearY)
	{
		return TArray<FVector2D>{ { 0.0, NearY }, { Wd, NearY }, { Wd, NearY + Dp }, { 0.0, NearY + Dp } };
	};

	// TODAY'S STAND - flush to the kerb - is what a taxiing wing sweeps.
	const FString Flush = Target->WhyStandRefused(Box(Kerb), EPavement::Tarmac);
	TestTrue(TEXT("a stand flush to the kerb is refused for the strip"), Flush.Contains(TEXT("clearance strip")));

	// THE SAME STAND A STRIP BACK is the tool's own placement, and passes every gate.
	TestEqual(TEXT("a stand at the strip edge is not refused"),
		Target->WhyStandRefused(Box(Kerb + Strip), EPavement::Tarmac), FString());
	return true;
}
```

If the second assertion fails for a reason other than the strip (e.g. "cannot afford"), set up funds the way this file's other placement tests do - the assertion is about the strip, not money.

- [ ] **Step 2: Build, run `-Filter Airside.Tool.StandPlotPlacement.RefusedInsideStrip`;** expect FAIL on the first assertion.

- [ ] **Step 3: Add the gate** after the taxiway-crosses loop (+ `#include "Model/TaxiwayStrip.h"`):

```cpp
	// INSIDE A TAXIWAY'S CLEARANCE STRIP (strip spec 2026-09-28): a taxiing wing overhangs the
	// pavement edge, and a parked tail within its reach is struck. AFTER the crossing check,
	// which names the more specific fault when the taxiway runs through the box itself. THE
	// STRIP IS THE TAXIWAY'S LETTER'S, so the message names the letter, not the stand's.
	// ENFORCED BY: Airside.Tool.StandPlotPlacement.RefusedInsideStrip
	if (Network != nullptr)
	{
		if (const TOptional<TaxiwayStrip::FIntrusion> In = TaxiwayStrip::WorstIntrusion(*Network, Outline))
		{
			return FString::Printf(TEXT("inside a taxiway's clearance strip by %.1f m (a Code %s taxiway needs %.1f m clear)"),
				In->Depth / 100.0, IcaoCode::ToLetter(In->Letter), In->Required / 100.0);
		}
	}
```

- [ ] **Step 4: Build; run `-Filter Airside.Tool.StandPlot`** (all stand tool + placement tests). New test passes. Placement tests that built stands flush to the kerb now refuse - Task 6 handles fixtures; here fix only tests in these two files: move the stand back by `TaxiwayStrip::StripWidthOf`, comment `// behind the clearance strip (spec 2026-09-28)`.
- [ ] **Step 5: Commit** `feat(stands): refuse a stand inside a taxiway's clearance strip`

---

### Task 5: An existing stand inside a strip takes no new arrivals

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/StandAdmission.h` (`EStandRefusal`, `Judge`)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/StandAdmission.cpp` (`Judge`, `Describe`)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/ArrivalPlanner.cpp:53` and `:199` (pass `Network`)
- Modify: every test the compiler names (`StandAdmissionTest.cpp` x5, `ArrivalPlannerTest.cpp` x1)
- Test: `Plugins/Airside/Source/AirsideTests/Private/StandAdmissionTest.cpp` (append)

**Interfaces:**
- Consumes: `TaxiwayStrip::WorstIntrusion`.
- Produces: `StandAdmission::Judge(const URoadNetwork& Network, const FEntityInstance& Stand, const FAirframe& Airframe)`; `EStandRefusal::InsideStrip`.

- [ ] **Step 1: Write the failing test.** Read `StandAdmissionTest.cpp`'s existing helper for building an `FEntityInstance` and an `FAirframe`; append:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandAdmissionInsideStripTest,
	"Airside.Model.StandAdmission.InsideStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandAdmissionInsideStripTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId W = Net->AddNode(FVector2D(-10000.0, 0.0));
	const FRoadNodeId E = Net->AddNode(FVector2D(10000.0, 0.0));
	Net->AddSegment(W, E, FVector2D::ZeroVector, URoadProfile::MakeTransient(2400.0, 1600.0));

	// <build Stand and Airframe with this file's own helpers: a Code C tarmac stand admitting
	//  a 737-sized airframe, so every other gate passes>
	FEntityInstance Stand = /* file helper */;
	const FAirframe Airframe = /* file helper */;
	const double Wd = IcaoCode::StandWidthForLetter(EIcaoCode::C);
	const double Dp = IcaoCode::StandDepthForLetter(EIcaoCode::C);
	auto BoxFrom = [&](double NearY)
	{
		return TArray<FVector2D>{ { 0.0, NearY }, { Wd, NearY }, { Wd, NearY + Dp }, { 0.0, NearY + Dp } };
	};

	// A STAND DRAWN BEFORE THE STRIP EXISTED - flush to the kerb - is closed to new arrivals.
	// Placement already refuses these; this is the one the player built last week.
	Stand.Outline = BoxFrom(1200.0);
	const FStandAdmission Closed = StandAdmission::Judge(*Net, Stand, Airframe);
	TestEqual(TEXT("flush to the kerb: refused for the strip"), static_cast<int32>(Closed.Why), static_cast<int32>(EStandRefusal::InsideStrip));
	TestFalse(TEXT("and not admitted"), Closed.IsAdmitted());
	TestTrue(TEXT("the sentence names the strip"), StandAdmission::Describe(Closed).Contains(TEXT("strip")));

	Stand.Outline = BoxFrom(1200.0 + 2800.0);
	TestTrue(TEXT("behind the strip: admitted"), StandAdmission::Judge(*Net, Stand, Airframe).IsAdmitted());
	return true;
}
```

Replace the two `/* file helper */` with the file's actual helpers when writing it - this is the one place the plan cannot name them; read the file first.

- [ ] **Step 2: Build; expect compile failure** (`Judge` takes 2 arguments; `InsideStrip` undeclared).

- [ ] **Step 3: Extend the enum and `Judge`.** In `EStandRefusal`, APPEND after `Service,`:

```cpp
	/** The stand sits inside a taxiway's clearance strip - drawn before the strip existed, or
	 *  its taxiway was upgraded since. Closed to NEW arrivals only; one already parked
	 *  finishes its turnaround (user, 2026-09-28). Appended, never inserted: a UENUM. */
	InsideStrip,
```

Change `Judge`'s declaration to `AIRSIDE_API FStandAdmission Judge(const URoadNetwork& Network, const FEntityInstance& Stand, const FAirframe& Airframe);` (forward-declare `class URoadNetwork;`), and update its doc comment: `NETWORK because a stand's validity now depends on the taxiway beside it (the strip), which the stand alone cannot know.` In `Judge`'s body, first thing after filling `Out`'s figures:

```cpp
		// FIRST: a stand in a strip refuses every aircraft, so no size or surface reason
		// should speak over it. Only a plotted stand has ground to judge.
		if (Stand.IsPlotted() && TaxiwayStrip::WorstIntrusion(Network, Stand.Outline).IsSet())
		{
			Out.Why = EStandRefusal::InsideStrip;
			return Out;
		}
```

In `Describe`, add the case: `"the stand is inside a taxiway's clearance strip - redraw it further back"`. If `IsAdmitted()` is written as `Why == None && ...`, confirm it returns false for `InsideStrip`.

- [ ] **Step 4: Pass `Network` at both `ArrivalPlanner.cpp` sites** (`Judge(Network, Stand, Airframe)`, `Judge(Network, *CandidateStand[Index], Airframe)`) and in every test the compiler names (tests with no network: `*NewObject<URoadNetwork>(GetTransientPackage())` - an empty network has no strips, which states that those tests are not about strips).
- [ ] **Step 5: Record the scan's figure.** Run the editor on `M_Test` (Task 7's launch), grep the log for the segment count (add a temporary `UE_LOG(LogAirside, Log, TEXT("TaxiwayStrip: %d segments"), Segments.Num());` if none exists, then remove it), and write into `WorstIntrusion`'s "EVERY LIVE SEGMENT" comment: `N was <count> on M_Test, 2026-09-28.`
- [ ] **Step 6: Occupant finishes (Review Focus 3).** Read `GroundTrafficRebuild.cpp` around line 936. If `ChooseStand` there runs for an agent already PARKED on its stand, a rebuild would evict it - violating the ruling. In that case, skip re-choosing for a parked agent (its current stand stays its goal) and add to `StandAdmissionTest.cpp` or the rebuild's own test file a composition test: park an aircraft, move the stand's outline into the strip, rebuild, assert the agent is still parked and its phase unchanged. If it runs only for agents still inbound, write that finding in a comment at line 936 with `// ENFORCED BY:` naming the existing test that pins inbound-only, or add one.
- [ ] **Step 7: Build; run `-Filter Airside.Model.StandAdmission` and `-Filter Airside.Model.ArrivalPlanner`.** Expect pass.
- [ ] **Step 8: Commit** `feat(admission): a stand inside a taxiway strip takes no new arrivals`

---

### Task 6: Fixtures move behind the strip; full suite green

**Files:**
- Modify: `Plugins/Airside/Source/AirsideTests/Private/StandFixture.h:46` and `AirsideTestFixtures.cpp` (stand placement helpers), plus any test the suite names.

- [ ] **Step 1: Run the full suite** (`Run-AirsideTests.ps1 -Project ...`, no filter). Record the `N run, N failed, N crashed` line.
- [ ] **Step 2: For each failure, classify it** - (a) a fixture places a stand flush to a taxiway, now refused or closed: fix the FIXTURE, once, by adding `TaxiwayStrip::StripWidthOf` of the adjoining taxiway to its setback (`StandFixture.h:46` computes `-StandBox::EntranceSetback(...)` - the stand moves strip-width further from the road); (b) anything else: stop and report it - it is a behaviour change this plan did not predict.
- [ ] **Step 3: Rerun until** `0 failed, 0 crashed`, and the run count equals Step 1's plus the tests this plan added (5).
- [ ] **Step 4: Check-Architecture, then commit** `test: stand fixtures sit behind the taxiway clearance strip`, body listing each fixture changed.

---

### Task 7: Look at it

Per "visual changes iterate live": the suite cannot see paint.

- [ ] **Step 1:** Launch the worktree editor on its own MCP port (check first for other editors: `Get-CimInstance Win32_Process | ? Name -like 'UnrealEditor*' | select ProcessId,CommandLine`):
  `& "D:\Epic\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" "C:\repos\airportmgr2-taxiway-clearance-strip\AirportMgr.uproject" -ModelContextProtocolPort=8002`
  then `$env:AIRSIDE_MCP_PORT=8002; python Tools/Mcp.py log LogRoadMesh` to confirm it is this build.
- [ ] **Step 2:** Ask the user to open `M_Test`, draw one stand off a 24 m taxiway, and one off a narrower one. Evidence: `PlaceStandInPlot: Code X stand ...` in the log, and `python Tools/Mcp.py shot strip.png editor`.
- [ ] **Step 3:** Look at the shot for: the gap between pavement and white box (should be the strip width); whether the yellow lead-in is painted across the gap (spec: it must not be). If it is, find which builder paints the derived lead-in between taxiway and stand and clip it to paved ground - a live iteration with the user, not a plan step.
- [ ] **Step 4:** Existing M_Test stands are now closed to arrivals (Task 5). Tell the user: they must be redrawn; the log will show arrivals refused with "clearance strip".

---

## Not in this plan (stages 3-6)

Refusing roads, taxiways and plots in strips + the 30-degree crossing exemption (3); holds at the strip edge (4); stand numbers + turn-off arrow (5); upgrade re-check, stand invalidation flag/inspector, taxiway restriction (6). Each gets its own plan once this lands.
