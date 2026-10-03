# Taxiway Naming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every taxiway gets a stored, stable, renameable ICAO-style name (A, B, A1...), kept a single chain after every edit, drawn as labels in both drivers, used by the deadlock alert and the aircraft card, and renameable from the inspector.

**Architecture:** Names are `URoadNetwork` state (`TArray<FTaxiway> Taxiways`, `FRoadSegment::TaxiwayId`), so the undo Memento (`DuplicateObject` + `CopyFrom`) and the save carry them. ONE pass, `URoadNetwork::NormaliseTaxiways`, names every unnamed taxiway chain (`AssignTaxiway`) and then enforces the single-chain invariant; it runs from ONE door, `URoadEditFacade::NotifyChanged` on a Topology change, and on load from `ARoadNetworkActor::RepairLoadedNetwork` (`EnsureTaxiwayNames`). Labels are MEANINGS (`Model/TaxiwayLabels` -> `Tool/TaxiwayNameOverlay` -> `EPreviewStyle::TaxiwayName`); each driver decides the look. All naming code lives in a new `Private/Model/RoadNetworkTaxiways.cpp`, the only file allowed to write `TaxiwayId` (Check-Architecture rule 103).

**Tech Stack:** UE 5.8.2 C++, `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, PowerShell tooling (`Tools/Run-AirsideTests.ps1`, `Tools/Check-Architecture.ps1`).

**Spec:** `docs/superpowers/specs/2026-10-02-taxiway-naming-design.md`

## Global Constraints

- Worktree `C:\repos\airportmgr2-slot3` only. The main checkout `C:\repos\AirportMgr2` is off-limits.
- Build (PowerShell, from the worktree; `-NoHotReloadFromIDE` ONLY because an editor may be open on ANOTHER checkout. First check with `Get-CimInstance Win32_Process | ? Name -like 'UnrealEditor*' | select ProcessId,CommandLine` that no editor has `airportmgr2-slot3` open):
  ```powershell
  & "D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat" AirportMgrEditor Win64 Development `
    -Project="C:\repos\airportmgr2-slot3\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
  ```
- Test (from `C:\repos\airportmgr2-slot3`): `./Tools/Run-AirsideTests.ps1 -Project C:\repos\airportmgr2-slot3\AirportMgr.uproject -Filter <filter>`. Read its `N test(s) run, N failed, N crashed` line, never the exit code.
- **A new .cpp needs two builds** (memory `unreal-adding-a-test-file-needs-two-builds`): after creating a file, build twice and check the second run prints `Compile [x64] <File>.cpp`; if not, touch an existing .cpp in the same module and build again. "no tests matched" for a new test means a stale build first, a typo second.
- Test names: `Airside.Model.TaxiwayNames.<Leaf>` for world-free model tests, `Airside.Present.TaxiwayNames.<Leaf>` for actor-level seams. NEVER register the bare `Airside.Model.TaxiwayNames` (rule 42: a bare parent beside dotted children silently never runs).
- Log category: `LogAirside` (Airside model), `LogRoadMesh` (facade/actor), `LogInspector` (game inspector). Every naming line starts `TaxiwayNames:`. No new `DEFINE_LOG_CATEGORY_STATIC`.
- Spec values, verbatim: letters `A..Z skipping I, O, X; then AA, AB, ...`; in-line window `ExitGeometry::InLineEndDegrees, 10 deg`; `ConnectorMaxLength (~300 m, a UPROPERTY knob)`; labels `repeated every ~500 m (knob)`; rename `1-3 characters, letters/digits, unique, upper-cased`; refusal words `"B is taken"`, `"I, O and X are avoided: they read as 1, 0 and closed"`; toast `"C split off from A"`; backfill log `TaxiwayNames: backfilled N taxiway(s), M connector(s)`.
- Units are uu (cm): 300 m = `30000.0`, 500 m = `50000.0`.
- `Private/Model/RoadNetwork.cpp` is held to 2287 lines by rule 77 and is 2254 on 2026-10-02: this plan adds at most 12 lines there. Every naming body goes in `RoadNetworkTaxiways.cpp` (held to rule 77's 800-line default).
- Comments explain WHY; a comment that states a fact about other code carries `ENFORCED BY:` naming the test or rule (rule 12). Dated numbers, never "the graph is small".
- Commits: concise, imperative, body ends with `Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp`, NO `Co-Authored-By` trailer. A commit has been built (CLAUDE.md); if a full build was impossible the message says "unbuilt".
- Stacked PRs: PR 1 `feature/taxiway-names` -> `main`; PR 2 `feature/taxiway-labels` -> `feature/taxiway-names`; PR 3 `feature/taxiway-rename` -> `feature/taxiway-labels`. Run the full suite once per branch tip (after the last rebase), then `gh pr create`. PR body: the build line, the test line, the UE_LOG and comment-line deltas of touched files.

## Review Focus

1. **Click-by-click drawing.** `FRoadChainingState::OnClick` (RoadDrawTool.cpp:146-210) commits ONE segment per click, so every bend is a node and every click a separate normalise. A five-click bent taxiway must stay one letter, and a 90 degree corner must start a new one. Test added to Task 3 (`ClickByClickDrawing`).
2. **A name decided "as drawn so far".** A connector drawn in two clicks (stub, then onto the runway) must end a connector; a stub extended past `ConnectorMaxLength` must become a letter; a letter drawn from open ground that lands on A must become A's connector. Test added to Task 4 (`DrawnSoFarIsRejudged`).
3. **Undo and redo of a split.** Undo must bring the old names back exactly and must not toast again; redo must split again. Test added to Task 7 (`SplitIsAnnouncedAndUndone`).
4. **A save game written before names, with roads that lost their own profile.** OpsSave's load is `Serialize` alone (no `PostLoad`), and a road saved with the transient fallback profile loads with `Profile == nullptr`, readable as a taxiway only once `DefaultProfile` is re-resolved. Both load paths must name it. Test added to Task 6 (`LoadBackfillsBothPaths`).
5. **Rename input the player actually types.** Lower case, surrounding spaces, a name that collides only through a DERIVED connector ("K" while a "K2" override exists), I/O/X, four characters. Test added to Task 13 (`RenameRefusals`).

## Deviations from spec

- **D1 - Bends.** Spec: in line within 10 deg everywhere. Plan: at a node carrying exactly two roads (a bend) a chain carries on within `FTaxiwayNamingRules::BendDegrees` (45, a knob); at a junction (3+ roads) within `RoadGeom::InLineDegrees` (10). Reason: the draw tool commits one segment per click (RoadDrawTool.cpp:193), so every bend of a curved taxiway is a node; at 10 deg each click past a gentle bend would mint a letter. 45 keeps a 90 deg corner a name boundary.
- **D2 - A dead end anchors a connector.** Spec: a connector has BOTH ends on a named taxiway or a runway. Plan: a dead end (a node no other road touches) also counts, provided one end is on a named taxiway. Reason: the spec's own PIE check expects "the stand links as connectors", and a stand link ends at a stand, not on a taxiway.
- **D3 - Re-judged while being drawn.** Spec: names are assigned at draw time and never re-derived. Plan: when a new piece is inherited into an AUTO-named taxiway, that taxiway is judged again by the same table on its whole chain - a letter with no connectors becomes a connector if it now qualifies; a connector longer than `ConnectorMaxLength` becomes a letter. Logged (`TaxiwayNames: B is now A1 (as drawn so far)`), not toasted. Adds `FTaxiway::bPlayerNamed`; a player-renamed taxiway is never re-judged. Reason: one gesture is many commits (D1's reason), and without this a two-click connector reads as a letter forever.
- **D4 - AssignTaxiway names a chain, from one pass.** Spec: "called for every new taxiway segment". Plan: `NormaliseTaxiways` first groups every unnamed taxiway segment into chains (the backfill's grouping) and calls `AssignTaxiway` per chain, longest first; the load backfill (`EnsureTaxiwayNames`) is that same pass. Reason: one rule for draw and load, and a rebuild batch (`FRoadRebuildBatch`) commits several segments under one notify.
- **D5 - Ids are append-only.** Spec: "stable handle (slot list, like entities)". Plan: `Taxiways` is append-only, `Id == index`, dead entries stay `bAlive = false` and are never reused - so a bare `int32` IS a stable handle without a generation (and rule 5's hand-built-handle ban never arises). Cost: one dead entry per taxiway ever minted (under 200 on a 40-stand airport, 2026-10-02 estimate).
- **D6 - Backfill in RepairLoadedNetwork, not PostLoad.** Spec: "PostLoad groups unnamed taxiway segments...". Plan: `EnsureTaxiwayNames` runs in `ARoadNetworkActor::RepairLoadedNetwork` (both load paths) right after `DefaultProfile` is re-resolved (RoadNetworkActor.cpp:589-594). Reason: at a level's `PostLoad` `DefaultProfile` is still null, so a segment saved without its own profile cannot be told from a service road (`TaxiwayStrip::IsAircraftOnly` reads `ProfileFor`); naming then would name half the map and name the rest later in a different order.
- **D7 - Split copies through the one writer; the lint is file-granular.** Spec: `TaxiwayId` written only inside AssignTaxiway / NormaliseTaxiways / backfill. Plan: `URoadNetwork::SplitSegment` (which already copies `Runway`/`Surface` to both halves) also calls `WriteTaxiwayId` on both halves; rule 103 holds every `.TaxiwayId =` / `->TaxiwayId =` to `RoadNetworkTaxiways.cpp`, where `WriteTaxiwayId` is the one writer. Rule 4 rows are scoped by file, not function.
- **D8 - The in-line constant moves to Solve/.** Model may not include Build/ (rule 1), so `RoadGeom::InLineDegrees` + `RoadGeom::IsInLine` become THE definition and `ExitGeometry::InLineEndDegrees` aliases it.
- **D9 - The one place is the facade's notify.** `NormaliseTaxiways` runs from `URoadEditFacade::NotifyChanged` for `EChangeKind::Topology` (after a batch folds), not inside `URoadNetwork` mutators. Undo/redo pass through it too (a no-op on a normalised snapshot). Model tests call `NormaliseTaxiways` themselves after each raw mutator.
- **D10 - The toast's route.** Airside never names AirportOps (rule 1b): facade native delegate `OnTaxiwaySplit(SplitOff, From)` -> `UOpsRuntime` bridge `TaxiwaySplit` -> bus `FTaxiwaySplitEvent` -> `UOpsEvents::OnTaxiwaySplit` (two nouns, not a sentence face - rule 4) -> `UToastStackWidget` words it. The editor mode has no ops runtime: a log line only.
- **D11 - A rename moves GuidelineRevision.** Via `NoteFactChanged`, the clock the inspector key (`InspectorNetworkKey`) and every planner cache already pair with (RoadNetwork.h:580-585, "WHY THIS CLOCK AND NOT A THIRD").
- **D12 - Editor labels need a hover.** The editor collects labels only in `DescribeFrame` under `bHoverValid` (RoadBuildEditorTool.cpp:824-836) and has no G toggle (its guidelines are always on, RoadBuildEditorTool.cpp:594-600), so it passes `true` for the toggle and names show while the mouse is over the viewport.
- **D13 - Junction and "On:" words.** A junction is named by the TAXIWAYS meeting there, sorted, `/`-joined ("A/B"); a runway/taxiway junction reads as the taxiway alone. `InspectFacts::WhereIs`: a lane derived from a runway -> the runway pair ("09/27"), from a taxiway -> its name, a turn path -> its junction's name, anything else (a stand lead-in, a hand-laid line) -> empty, and the card hides the line.
- **D14 - Unnamed card keeps the old title.** `FTaxiwayCard` titles "Taxiway A" when the segment is named and falls back to "Taxiway <segment index>" when not (a raw-built fixture), so `AirportMgr.Inspector.Card.Taxiway` stays true.
- **D15 - Rename refuses empty, and frees the old letter.** An empty request is refused (1-3 characters); renaming A to K frees A (nothing displays it any more), per the spec's own pool rule.

---

# PR 1 - Model (branch `feature/taxiway-names`)

### Task 1: One in-line definition in Solve/

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Solve/RoadGeom.h:43` (insert after `AngleBetween`'s declaration)
- Modify: `Plugins/Airside/Source/Airside/Private/Solve/RoadGeom.cpp` (append)
- Modify: `Plugins/Airside/Source/Airside/Public/Build/ExitGeometry.h:81-87`
- Modify: `Plugins/Airside/Source/Airside/Private/Build/ExitGeometry.cpp:78-82`
- Test: `Plugins/Airside/Source/AirsideTests/Private/StraightThroughCornerTest.cpp` (append before its `#endif`)

**Interfaces:**
- Consumes: `RoadGeom::AngleBetween(const FVector2D&, const FVector2D&)` (RoadGeom.h:43).
- Produces: `inline constexpr double RoadGeom::InLineDegrees = 10.0;`, `AIRSIDE_API bool RoadGeom::IsInLine(const FVector2D& OutA, const FVector2D& OutB);` (two OUTGOING tangents at one node).

- [ ] **Step 1: Write the failing test** - append to StraightThroughCornerTest.cpp, before `#endif`:

```cpp
/**
 * TAXIWAY NAMES (spec 2026-10-02) READ THE RUNWAY END'S WINDOW: "continue in line (within ExitGeometry::InLineEndDegrees,
 * 10 deg)". Model/ may not include Build/ (Check-Architecture rule 1), so the window moved to RoadGeom and the runway end
 * forwards to it - one definition, measured here on both sides of the edge and through both callers.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInLineIsOneDefinitionTest,
	"Airside.Solve.InLineIsOneDefinition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInLineIsOneDefinitionTest::RunTest(const FString& Parameters)
{
	static_assert(ExitGeometry::InLineEndDegrees == RoadGeom::InLineDegrees, "the runway end's window IS the naming window");
	const FVector2D Back(-1.0, 0.0);
	for (double Off = 0.0; Off <= 20.0; Off += 0.5)
	{
		const double Radians = FMath::DegreesToRadians(Off);
		const FVector2D On(FMath::Cos(Radians), FMath::Sin(Radians));
		const bool bExpected = Off < RoadGeom::InLineDegrees - 0.01;
		if (Off > RoadGeom::InLineDegrees - 0.01 && Off < RoadGeom::InLineDegrees + 0.01) { continue; }   // the edge itself
		TestEqual(FString::Printf(TEXT("%.1f deg off straight: in line is %d"), Off, bExpected ? 1 : 0),
			RoadGeom::IsInLine(Back, On), bExpected);
		TestEqual(FString::Printf(TEXT("%.1f deg: the runway end agrees"), Off),
			ExitGeometry::IsInLineAtRunwayEnd(Back, On), RoadGeom::IsInLine(Back, On));
	}
	return true;
}
```
Add includes at the top of the file if absent: `#include "Build/ExitGeometry.h"`, `#include "Solve/RoadGeom.h"`.

- [ ] **Step 2: Build - expect a compile failure**

Run the Global Constraints build. Expected: `error C2039: 'IsInLine': is not a member of 'RoadGeom'` (and `InLineDegrees`).

- [ ] **Step 3: Implement**

RoadGeom.h, after the `AngleBetween` declaration (line 43):
```cpp
	/**
	 * How far off straight two arms' outgoing tangents may be for a road to CARRY ON through their node - 10 degrees,
	 * set 2026-10-01 for a taxiway leaving a runway's end (ExitGeometry::InLineEndDegrees, which now aliases this) and
	 * read by the taxiway names (spec 2026-10-02: "continue in line ... 10 deg"). HERE, in Solve/, because Model/ may not
	 * include Build/ (Check-Architecture rule 1) and two copies of "10" is the drift CLAUDE.md's one-list rule names.
	 * ENFORCED BY: Airside.Solve.InLineIsOneDefinition (static_assert and a sweep through both callers)
	 */
	inline constexpr double InLineDegrees = 10.0;

	/** Whether OutA and OutB - two OUTGOING tangents at one node - are within InLineDegrees of straight through. */
	AIRSIDE_API bool IsInLine(const FVector2D& OutA, const FVector2D& OutB);
```
RoadGeom.cpp, append:
```cpp
bool RoadGeom::IsInLine(const FVector2D& OutA, const FVector2D& OutB)
{
	return AngleBetween(OutA, OutB) >= UE_DOUBLE_PI - FMath::DegreesToRadians(InLineDegrees);
}
```
ExitGeometry.h: replace `constexpr double InLineEndDegrees = 10.0;` (line 87) with `constexpr double InLineEndDegrees = RoadGeom::InLineDegrees;`, keep its comment and append to it: ` THE VALUE IS RoadGeom::InLineDegrees since 2026-10-02 (taxiway names read it from Model/). ENFORCED BY: Airside.Solve.InLineIsOneDefinition`. Add `#include "Solve/RoadGeom.h"` after `#include "Model/RoadHandles.h"`.
ExitGeometry.cpp:78-82 body becomes:
```cpp
	bool IsInLineAtRunwayEnd(const FVector2D& RunwayTangent, const FVector2D& TaxiwayTangent)
	{
		// RoadGeom's one definition - see InLineEndDegrees.
		return RoadGeom::IsInLine(RunwayTangent, TaxiwayTangent);
	}
```

- [ ] **Step 4: Build, then run**

`./Tools/Run-AirsideTests.ps1 -Project C:\repos\airportmgr2-slot3\AirportMgr.uproject -Filter "Airside.Solve+Airside.Build.RunwayEnd"`
Expected: `0 failed, 0 crashed`, `Airside.Solve.InLineIsOneDefinition` among those run.

- [ ] **Step 5: Commit**
```powershell
git add Plugins/Airside/Source/Airside/Public/Solve/RoadGeom.h Plugins/Airside/Source/Airside/Private/Solve/RoadGeom.cpp `
  Plugins/Airside/Source/Airside/Public/Build/ExitGeometry.h Plugins/Airside/Source/Airside/Private/Build/ExitGeometry.cpp `
  Plugins/Airside/Source/AirsideTests/Private/StraightThroughCornerTest.cpp
git commit -m "solve: one in-line window in RoadGeom, runway end aliases it" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

---

### Task 2: Taxiway data, letters, and the Memento carrying them

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Solve/TaxiwayLetters.h`, `Plugins/Airside/Source/Airside/Private/Solve/TaxiwayLetters.cpp`
- Create: `Plugins/Airside/Source/Airside/Public/Model/Taxiway.h`
- Create: `Plugins/Airside/Source/Airside/Private/Model/RoadNetworkTaxiways.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNode.h:79-80` (new field after `RestrictedLetter`)
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h:13` (include), `:902` (public block), `:1225` (private helpers), `:1307` (UPROPERTY), `:1391` (test access)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp:467` (CopyFrom)
- Create test: `Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp`

**Interfaces:**
- Consumes: `RoadSlot`-free; `URoadNetwork::GetSegment`, `SegmentIdAt`, `GetSegmentMutable` (private, RoadNetwork.h:1206), `NoteFactChanged` (private, :1249).
- Produces (all later tasks use these names):
  - `TaxiwayLetters::LetterAt(int32 Index) -> FString`, `TaxiwayLetters::IsAvoided(TCHAR) -> bool`
  - `USTRUCT FTaxiway { int32 Id; FString Name; int32 ParentId; int32 ConnectorNumber; int32 NextConnectorNumber; bool bPlayerNamed; bool bAlive; bool IsConnector() const; }`
  - `USTRUCT FTaxiwayNamingRules { double ConnectorMaxLength = 30000.0; double BendDegrees = 45.0; double LabelRepeatDistance = 50000.0; }`
  - `struct FTaxiwayChain { TArray<FRoadSegmentId> Segments; FRoadNodeId First; FRoadNodeId Last; double Length; int32 LowestIndex; }`
  - `struct FTaxiwayRename { FString SplitOff; FString From; }`
  - `FRoadSegment::TaxiwayId` (int32, INDEX_NONE)
  - `URoadNetwork`: `const FTaxiway* GetTaxiway(int32) const`, `const TArray<FTaxiway>& GetTaxiways() const`, `int32 TaxiwayOf(FRoadSegmentId) const`, `FString TaxiwayDisplayName(int32) const`, `bool IsTaxiwayNameTaken(const FString&, int32 Except) const`, `bool HasTaxiwayConnectors(int32) const`, `int32 TaxiwayConnectorCount(int32) const`; private `void WriteTaxiwayId(FRoadSegmentId, int32)`, `FTaxiway* FindTaxiwayMutable(int32)`; `FRoadNetworkTestAccess::ClearTaxiwayNamesForTest()`.

- [ ] **Step 1: Add the UPROPERTY first and watch the existing completeness test go red**

RoadNetwork.h line 13, after `#include "Solve/LetterEnvelope.h"`: `#include "Model/Taxiway.h"`.

Create `Public/Model/Taxiway.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Taxiway.generated.h"

/**
 * One taxiway's NAME (spec docs/superpowers/specs/2026-10-02-taxiway-naming-design.md). Stored and stable - never
 * re-derived from geometry, for the reason stand numbers are not: re-deriving reshuffles letters on every edit. Owned by
 * URoadNetwork::Taxiways; a segment names it by FRoadSegment::TaxiwayId.
 *
 * APPEND-ONLY, Id == index, never reused (a deviation from the spec's slot list - see the plan's D5): a dead entry stays
 * bAlive == false, so an int32 is a stable handle with no generation, and a card holding one cannot come to name another taxiway.
 */
USTRUCT()
struct AIRSIDE_API FTaxiway
{
	GENERATED_BODY()

	UPROPERTY() int32 Id = INDEX_NONE;

	/**
	 * "A", "AB", or the player's override ("K7"). EMPTY for a connector that has not been renamed: its display name is
	 * derived, Parent's + ConnectorNumber, so renaming A to K makes A1..A4 read K1..K4 (URoadNetwork::TaxiwayDisplayName).
	 */
	UPROPERTY() FString Name;

	/** INDEX_NONE for a lettered taxiway; the parent (always a LETTERED one) for a connector. */
	UPROPERTY() int32 ParentId = INDEX_NONE;

	/** 1.. for a connector, 0 otherwise. */
	UPROPERTY() int32 ConnectorNumber = 0;

	/** The next number this taxiway gives a connector. Only advances: a deleted A1 is never reissued while A lives. */
	UPROPERTY() int32 NextConnectorNumber = 1;

	/**
	 * The player renamed it (PR 3's card). A named-by-the-player taxiway is never re-judged while it is being drawn
	 * (URoadNetwork::RejudgeTaxiway, the plan's D3): the player's word outranks "as drawn so far".
	 */
	UPROPERTY() bool bPlayerNamed = false;

	UPROPERTY() bool bAlive = false;

	bool IsConnector() const { return ParentId != INDEX_NONE; }
};

/** The naming knobs - an actor UPROPERTY (ARoadNetworkActor::TaxiwayNaming), defaulted for a model test. */
USTRUCT()
struct AIRSIDE_API FTaxiwayNamingRules
{
	GENERATED_BODY()

	/** A new chain shorter than this, anchored at both ends, is a CONNECTOR (A1) rather than a letter. 300 m (spec). */
	UPROPERTY(EditAnywhere, Category = "Airside|Taxiway names", meta = (ClampMin = "0.0"))
	double ConnectorMaxLength = 30000.0;

	/**
	 * At a node carrying exactly two roads - a BEND - how far off straight the road may turn and still be the same
	 * taxiway. NOT RoadGeom::InLineDegrees (10), which is the window at a JUNCTION: the draw tool commits one segment
	 * per click, so every bend of a curved taxiway is a node, and 10 would mint a letter per click (plan D1). 45 keeps
	 * a 90 degree corner a name boundary.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Taxiway names", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	double BendDegrees = 45.0;

	/** A long taxiway's name is repeated this far apart along it. 500 m (spec). */
	UPROPERTY(EditAnywhere, Category = "Airside|Taxiway names", meta = (ClampMin = "1000.0"))
	double LabelRepeatDistance = 50000.0;
};

/** A run of road segments in chain order and the two nodes it ends at (First == Last for a loop). Plain. */
struct FTaxiwayChain
{
	TArray<FRoadSegmentId> Segments;
	FRoadNodeId First;
	FRoadNodeId Last;
	double Length = 0.0;
	int32 LowestIndex = MAX_int32;
};

/** One "C split off from A" - URoadNetwork::NormaliseTaxiways' report, one per split. Plain. */
struct FTaxiwayRename
{
	FString SplitOff;
	FString From;
};
```

RoadNode.h, after line 79 (`UPROPERTY() uint8 RestrictedLetter = 0xFF;`) and a blank line:
```cpp
	/**
	 * The taxiway this segment belongs to - an Id into URoadNetwork::GetTaxiways() - or INDEX_NONE: a service road, a
	 * runway, or a taxiway segment the next URoadNetwork::NormaliseTaxiways has not named yet. SAVED, so names ride the
	 * level, the save game and the undo Memento; a level saved before 2026-10-02 loads INDEX_NONE and is named by
	 * URoadNetwork::EnsureTaxiwayNames. Written ONLY through URoadNetwork::WriteTaxiwayId, in RoadNetworkTaxiways.cpp.
	 * ENFORCED BY: Check-Architecture rule 103
	 */
	UPROPERTY() int32 TaxiwayId = INDEX_NONE;
```

RoadNetwork.h, after line 1307 (`UPROPERTY() int32 NextDepotNumber = 1;`):
```cpp

	/**
	 * Every taxiway ever named - see FTaxiway (append-only). SAVED and copied by CopyFrom, so an undo restores names and
	 * ids exactly. ENFORCED BY: Airside.Model.CopyFromCoversEveryProperty, Airside.Present.TaxiwayNames.SplitIsAnnouncedAndUndone
	 */
	UPROPERTY() TArray<FTaxiway> Taxiways;
```

Build. Run `-Filter Airside.Model.CopyFromCoversEveryProperty`. Expected: `1 failed`, message `CopyFrom carries every UPROPERTY across (missed: Taxiways)`.

- [ ] **Step 2: Write the failing letters/accessor test** - create `TaxiwayNamesTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/TaxiwayStrip.h"
#include "Profiles/RoadProfile.h"
#include "Solve/TaxiwayLetters.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * TAXIWAY NAMES, world-free (spec docs/superpowers/specs/2026-10-02-taxiway-naming-design.md): one test per rule of the
 * assignment table and the single-chain invariant. Every mutator here is URoadNetwork's raw one, so each test calls
 * NormaliseTaxiways itself where the facade's NotifyChanged would (plan D9).
 */
namespace TaxiwayNamesTest
{
	/** Prefixed against the UNITY build. A transient network and the three kinds of road. */
	struct FTaxiwayNamesNet
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Taxi = TestProfiles::Taxiway();
		URoadProfile* Runway = TestProfiles::Runway();
		URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
		FTaxiwayNamingRules Rules;

		FRoadNodeId Node(double X, double Y) { return Net->AddNode(FVector2D(X, Y)); }
		FRoadSegmentId Lay(FRoadNodeId A, FRoadNodeId B, URoadProfile* Profile = nullptr)
		{
			return Net->AddStraightSegment(A, B, Profile != nullptr ? Profile : Taxi);
		}
		/** Lay, then normalise - one click of the road tool, which commits one segment and notifies once. */
		FRoadSegmentId Click(FRoadNodeId A, FRoadNodeId B) { const FRoadSegmentId S = Lay(A, B); Normalise(); return S; }
		TArray<FTaxiwayRename> Normalise() { return Net->NormaliseTaxiways(Rules); }
		FString NameOf(FRoadSegmentId S) const { return Net->TaxiwayDisplayName(Net->TaxiwayOf(S)); }
		int32 Alive() const
		{
			int32 Count = 0;
			for (const FTaxiway& T : Net->GetTaxiways()) { Count += T.bAlive ? 1 : 0; }
			return Count;
		}
	};
}

/** I, O and X read as 1, 0 and a closed runway, so they are never issued; after Z come two letters (spec). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesLetterSequenceTest, "Airside.Model.TaxiwayNames.LetterSequence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesLetterSequenceTest::RunTest(const FString&)
{
	TestEqual(TEXT("the first letter is A"), TaxiwayLetters::LetterAt(0), FString(TEXT("A")));
	TestEqual(TEXT("H is followed by J - I reads as 1"), TaxiwayLetters::LetterAt(8), FString(TEXT("J")));
	TestEqual(TEXT("N is followed by P - O reads as 0"), TaxiwayLetters::LetterAt(13), FString(TEXT("P")));
	TestEqual(TEXT("W is followed by Y - X is a closed runway"), TaxiwayLetters::LetterAt(21), FString(TEXT("Y")));
	TestEqual(TEXT("Z is the last single letter"), TaxiwayLetters::LetterAt(22), FString(TEXT("Z")));
	TestEqual(TEXT("then AA"), TaxiwayLetters::LetterAt(23), FString(TEXT("AA")));
	TestEqual(TEXT("then AB"), TaxiwayLetters::LetterAt(24), FString(TEXT("AB")));
	TestEqual(TEXT("AH is followed by AJ"), TaxiwayLetters::LetterAt(31), FString(TEXT("AJ")));
	TestEqual(TEXT("AZ is followed by BA"), TaxiwayLetters::LetterAt(46), FString(TEXT("BA")));
	TSet<FString> Seen;
	for (int32 Index = 0; Index < 600; ++Index)
	{
		const FString Letter = TaxiwayLetters::LetterAt(Index);
		if (!TestFalse(FString::Printf(TEXT("%s avoids I, O and X"), *Letter),
			Letter.Contains(TEXT("I")) || Letter.Contains(TEXT("O")) || Letter.Contains(TEXT("X")))) { return false; }
		bool bAlready = false;
		Seen.Add(Letter, &bAlready);
		if (!TestFalse(FString::Printf(TEXT("%s is issued once"), *Letter), bAlready)) { return false; }
	}
	TestTrue(TEXT("I, O and X are the avoided ones"), TaxiwayLetters::IsAvoided(TEXT('I')) && TaxiwayLetters::IsAvoided(TEXT('O'))
		&& TaxiwayLetters::IsAvoided(TEXT('X')) && !TaxiwayLetters::IsAvoided(TEXT('A')) && !TaxiwayLetters::IsAvoided(TEXT('1')));
	return true;
}

/** A network with no names reads as one: nothing names a segment, and an unknown id has no name. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesUnnamedTest, "Airside.Model.TaxiwayNames.UnnamedByDefault",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesUnnamedTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadSegmentId S = N.Lay(N.Node(0.0, 0.0), N.Node(50000.0, 0.0));
	TestTrue(TEXT("control: the test profile is a taxiway"), TaxiwayStrip::HasStrip(*N.Net, S));
	TestEqual(TEXT("no taxiway yet"), N.Net->GetTaxiways().Num(), 0);
	TestEqual(TEXT("the segment names none"), N.Net->TaxiwayOf(S), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("an unknown id has no display name"), N.Net->TaxiwayDisplayName(INDEX_NONE), FString());
	TestEqual(TEXT("nor does an id past the end"), N.Net->TaxiwayDisplayName(7), FString());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 3: Build - expect a compile failure**

Expected: `'TaxiwayLetters': is not a class or namespace name` and `'TaxiwayOf': is not a member of 'URoadNetwork'`.

- [ ] **Step 4: Implement**

`Public/Solve/TaxiwayLetters.h`:
```cpp
#pragma once

#include "CoreMinimal.h"

/**
 * The letters a taxiway may be issued (ICAO Annex 14; spec 2026-10-02): A..Z without I, O and X - they read as 1, 0 and
 * a closed runway - then AA, AB, ... (bijective base 23). Solve/: no engine type beyond CoreMinimal.
 */
namespace TaxiwayLetters
{
	inline constexpr int32 Count = 23;

	/** The Index'th letter: 0 -> "A", 22 -> "Z", 23 -> "AA", 46 -> "BA". */
	AIRSIDE_API FString LetterAt(int32 Index);

	/** I, O or X, either case - never issued, and refused in a rename (URoadNetwork::WhyTaxiwayNameRefused). */
	AIRSIDE_API bool IsAvoided(TCHAR Character);
}
```
`Private/Solve/TaxiwayLetters.cpp`:
```cpp
#include "Solve/TaxiwayLetters.h"

namespace
{
	/** The 23, in order. A literal rather than A..Z filtered at run time: the order IS the issue order. */
	const TCHAR* const TaxiwayLettersAlphabet = TEXT("ABCDEFGHJKLMNPQRSTUVWYZ");
}

FString TaxiwayLetters::LetterAt(int32 Index)
{
	// BIJECTIVE BASE 23 (no zero digit), so Z is followed by AA, not BA - the spreadsheet-column rule the spec's
	// "then AA, AB" names.
	FString Out;
	for (int64 N = static_cast<int64>(FMath::Max(Index, 0)) + 1; N > 0; N = (N - 1) / Count)
	{
		Out.InsertAt(0, TaxiwayLettersAlphabet[(N - 1) % Count]);
	}
	return Out;
}

bool TaxiwayLetters::IsAvoided(TCHAR Character)
{
	const TCHAR Upper = FChar::ToUpper(Character);
	return Upper == TEXT('I') || Upper == TEXT('O') || Upper == TEXT('X');
}
```
RoadNetwork.h public block - insert after line 902 (`int32 GetNextDepotNumber() const { return NextDepotNumber; }`):
```cpp

	// --- Taxiway names (spec 2026-10-02-taxiway-naming) - bodies in RoadNetworkTaxiways.cpp, which rule 77's budget
	// on this file's .cpp (33 lines to spare on 2026-10-02) and rule 103's single writer both send there. -------------

	/** The live taxiway with this Id, or null - dead, never minted, or INDEX_NONE. */
	const FTaxiway* GetTaxiway(int32 TaxiwayId) const;
	const TArray<FTaxiway>& GetTaxiways() const { return Taxiways; }

	/** The live taxiway Segment belongs to, or INDEX_NONE (a road, a runway, an unnamed or dead segment). */
	int32 TaxiwayOf(FRoadSegmentId Segment) const;

	/**
	 * THE ONE FUNCTION that turns a taxiway id into text (spec): its Name, or - for a connector with none - its parent's
	 * Name plus its ConnectorNumber ("A3"). Empty for a dead or unknown id.
	 * ENFORCED BY: Airside.Model.TaxiwayNames.Connector, Airside.Model.TaxiwayNames.RenamePropagatesToConnectors
	 */
	FString TaxiwayDisplayName(int32 TaxiwayId) const;

	/** Some live taxiway other than Except displays Name. Display names are unique (the invariant). */
	bool IsTaxiwayNameTaken(const FString& Name, int32 Except) const;

	/** A live connector names this taxiway its parent - which keeps an empty parent alive, its letter reserved. */
	bool HasTaxiwayConnectors(int32 TaxiwayId) const;
	int32 TaxiwayConnectorCount(int32 TaxiwayId) const;
```
RoadNetwork.h private - insert after line 1225 (`FEntityInstance* GetEntityMutable(FEntityInstanceId Entity);`):
```cpp

	/** THE ONE WRITER of FRoadSegment::TaxiwayId (RoadNetworkTaxiways.cpp). ENFORCED BY: Check-Architecture rule 103 */
	void WriteTaxiwayId(FRoadSegmentId Segment, int32 TaxiwayId);
	FTaxiway* FindTaxiwayMutable(int32 TaxiwayId);
```
FRoadNetworkTestAccess - insert after line 1391 (`bool SetEntityFrontageForTest(...)`):
```cpp

	/** Every segment unnamed and every taxiway forgotten - a level as saved before 2026-10-02 loads
	 *  (Airside.Model.TaxiwayNames.BackfillGatwickShape). */
	void ClearTaxiwayNamesForTest();
```
RoadNetwork.cpp CopyFrom - after line 467 (`NextDepotNumber = Source.NextDepotNumber;`):
```cpp
	// THE NAMES (2026-10-02): an undo that restored the roads but not their names would re-letter the airport.
	Taxiways = Source.Taxiways;
```
Create `Private/Model/RoadNetworkTaxiways.cpp`:
```cpp
#include "Model/RoadNetwork.h"

#include "Algo/Reverse.h"
#include "AirsideLog.h"
#include "Model/TaxiwayStrip.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"
#include "Solve/TaxiwayLetters.h"

// TAXIWAY NAMES (spec docs/superpowers/specs/2026-10-02-taxiway-naming-design.md): URoadNetwork's members for them, in a
// file of their own. RoadNetwork.cpp is held to its line figure by Check-Architecture rule 77, and rule 103 holds every
// write of FRoadSegment::TaxiwayId to THIS file - which a shared file could not be scoped to.

const FTaxiway* URoadNetwork::GetTaxiway(int32 TaxiwayId) const
{
	return Taxiways.IsValidIndex(TaxiwayId) && Taxiways[TaxiwayId].bAlive ? &Taxiways[TaxiwayId] : nullptr;
}

FTaxiway* URoadNetwork::FindTaxiwayMutable(int32 TaxiwayId)
{
	return Taxiways.IsValidIndex(TaxiwayId) && Taxiways[TaxiwayId].bAlive ? &Taxiways[TaxiwayId] : nullptr;
}

int32 URoadNetwork::TaxiwayOf(FRoadSegmentId Segment) const
{
	const FRoadSegment* Found = GetSegment(Segment);
	return Found != nullptr && GetTaxiway(Found->TaxiwayId) != nullptr ? Found->TaxiwayId : INDEX_NONE;
}

FString URoadNetwork::TaxiwayDisplayName(int32 TaxiwayId) const
{
	const FTaxiway* Taxiway = GetTaxiway(TaxiwayId);
	if (Taxiway == nullptr)
	{
		return FString();
	}
	if (!Taxiway->Name.IsEmpty())
	{
		return Taxiway->Name;
	}
	// ONE LEVEL: a connector's parent is always lettered - AssignTaxiway parents a connector on a ROOT, and only a
	// taxiway with no connectors may become one (RejudgeTaxiway) - so this never recurses.
	const FTaxiway* Parent = Taxiway->IsConnector() ? GetTaxiway(Taxiway->ParentId) : nullptr;
	return Parent != nullptr && !Parent->Name.IsEmpty() ? Parent->Name + FString::FromInt(Taxiway->ConnectorNumber) : FString();
}

bool URoadNetwork::IsTaxiwayNameTaken(const FString& Name, int32 Except) const
{
	// A LINEAR SCAN per question: under 200 taxiways ever minted on a 40-stand airport (2026-10-02 estimate), and asked
	// only while naming or renaming - a click, never a frame.
	for (const FTaxiway& Each : Taxiways)
	{
		if (Each.bAlive && Each.Id != Except && TaxiwayDisplayName(Each.Id).Equals(Name, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

bool URoadNetwork::HasTaxiwayConnectors(int32 TaxiwayId) const
{
	return TaxiwayConnectorCount(TaxiwayId) > 0;
}

int32 URoadNetwork::TaxiwayConnectorCount(int32 TaxiwayId) const
{
	int32 Count = 0;
	for (const FTaxiway& Each : Taxiways)
	{
		Count += Each.bAlive && Each.ParentId == TaxiwayId && TaxiwayId != INDEX_NONE ? 1 : 0;
	}
	return Count;
}

void URoadNetwork::WriteTaxiwayId(FRoadSegmentId Segment, int32 TaxiwayId)
{
	if (FRoadSegment* Found = GetSegmentMutable(Segment))
	{
		Found->TaxiwayId = TaxiwayId;
	}
}

void FRoadNetworkTestAccess::ClearTaxiwayNamesForTest()
{
	for (FRoadSegment& Segment : Network.Segments)
	{
		Segment.TaxiwayId = INDEX_NONE;
	}
	Network.Taxiways.Reset();
}
```

- [ ] **Step 5: Build twice, run**

Build twice (new .cpp files; the second must print `Compile [x64] RoadNetworkTaxiways.cpp` and `TaxiwayNamesTest.cpp`). Run `-Filter "Airside.Model.TaxiwayNames+Airside.Model.CopyFromCoversEveryProperty"`. Expected: `3 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 6: Commit**
```powershell
git add Plugins/Airside/Source/Airside/Public/Solve/TaxiwayLetters.h Plugins/Airside/Source/Airside/Private/Solve/TaxiwayLetters.cpp `
  Plugins/Airside/Source/Airside/Public/Model/Taxiway.h Plugins/Airside/Source/Airside/Public/Model/RoadNode.h `
  Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp `
  Plugins/Airside/Source/Airside/Private/Model/RoadNetworkTaxiways.cpp Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp
git commit -m "model: taxiway name data, letter sequence, carried by CopyFrom" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

---

### Task 3: Naming new chains - assign, inherit, connector, split

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h` (public block from Task 2; private helpers from Task 2)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetworkTaxiways.cpp`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp:161` and `:200-207` (SplitSegment)
- Test: `Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp`

**Interfaces:**
- Consumes: Task 2's API; `TaxiwayStrip::HasStrip(const URoadNetwork&, FRoadSegmentId)` (TaxiwayStrip.h:81 - "aircraft only, and not a runway": THE taxiway test); `URoadNetwork::IsRunwaySegment`, `GetOutgoingTangent`, `GetOtherEnd`, `SegmentEnds`, `NodeIdAt`; `GuidelineGeom::Length(A, Control, B)` (GuidelineGeom.h:77); `RoadGeom::AngleBetween`, `RoadGeom::IsInLine` (Task 1).
- Produces: `TArray<FTaxiwayRename> URoadNetwork::NormaliseTaxiways(const FTaxiwayNamingRules& Rules)` (this task: steps 0-1; Task 4 adds 2-4), `FTaxiwayChain URoadNetwork::TaxiwayChainOf(int32) const`, `FString URoadNetwork::JunctionName(FRoadNodeId) const`; private `int32 AssignTaxiway(const FTaxiwayChain&, const FTaxiwayNamingRules&)`, `int32 AssignUnnamedTaxiways(const FTaxiwayNamingRules&)`, `int32 MintTaxiway(int32 ParentId)`, `int32 IssueConnectorNumber(int32 ParentId)`, `FString NextFreeTaxiwayLetter(int32 Except) const`, `void RejudgeTaxiway(int32, const FTaxiwayNamingRules&)`.

- [ ] **Step 1: Write the failing tests** - append inside TaxiwayNamesTest.cpp (before `#endif`):

```cpp
/** Assign: a taxiway is named the next free letter; roads and runways are not named. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesAssignTest, "Airside.Model.TaxiwayNames.Assign",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesAssignTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadSegmentId A = N.Lay(N.Node(0.0, 0.0), N.Node(50000.0, 0.0));
	TestEqual(TEXT("unnamed until normalised"), N.NameOf(A), FString());
	TestEqual(TEXT("a first taxiway splits nothing"), N.Normalise().Num(), 0);
	TestEqual(TEXT("the first taxiway is A"), N.NameOf(A), FString(TEXT("A")));
	const FRoadSegmentId B = N.Click(N.Node(0.0, 100000.0), N.Node(50000.0, 100000.0));
	TestEqual(TEXT("an unconnected 500 m taxiway is the next letter"), N.NameOf(B), FString(TEXT("B")));
	const FRoadSegmentId Road = N.Lay(N.Node(0.0, 200000.0), N.Node(50000.0, 200000.0), N.Road);
	const FRoadSegmentId Strip = N.Lay(N.Node(0.0, 300000.0), N.Node(50000.0, 300000.0), N.Runway);
	N.Normalise();
	TestEqual(TEXT("a service road is unnamed"), N.Net->TaxiwayOf(Road), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("a runway keeps its designator, not a letter"), N.Net->TaxiwayOf(Strip), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("two taxiways and nothing else"), N.Alive(), 2);
	TestEqual(TEXT("a second normalise of the same network changes nothing"), N.Normalise().Num(), 0);
	TestEqual(TEXT("and mints nothing"), N.Net->GetTaxiways().Num(), 2);
	return true;
}

/**
 * REVIEW FOCUS 1 - CLICK BY CLICK (RoadDrawTool commits one segment per click): carrying on from the END of a taxiway
 * inherits it, through a bend within FTaxiwayNamingRules::BendDegrees (plan D1); a 90 degree corner starts a new name;
 * at a JUNCTION only RoadGeom::InLineDegrees carries on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesClickByClickTest, "Airside.Model.TaxiwayNames.ClickByClickDrawing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesClickByClickTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadNodeId P0 = N.Node(0.0, 0.0);
	const FRoadNodeId P1 = N.Node(40000.0, 0.0);
	const FRoadNodeId P2 = N.Node(80000.0, 0.0);
	const FVector2D Bend(FMath::Cos(FMath::DegreesToRadians(30.0)), FMath::Sin(FMath::DegreesToRadians(30.0)));
	const FVector2D At3 = FVector2D(80000.0, 0.0) + Bend * 40000.0;
	const FRoadNodeId P3 = N.Node(At3.X, At3.Y);
	const FVector2D Corner(-Bend.Y, Bend.X);   // 90 degrees from the bend's direction
	const FVector2D At4 = At3 + Corner * 40000.0;
	const FRoadNodeId P4 = N.Node(At4.X, At4.Y);

	const FRoadSegmentId S1 = N.Click(P0, P1);
	const FRoadSegmentId S2 = N.Click(P1, P2);
	TestEqual(TEXT("in line from A's end: A"), N.NameOf(S2), FString(TEXT("A")));
	const FRoadSegmentId S3 = N.Click(P2, P3);
	TestEqual(TEXT("through a 30 degree bend: still A"), N.NameOf(S3), FString(TEXT("A")));
	const FRoadSegmentId S4 = N.Click(P3, P4);
	TestEqual(TEXT("round a 90 degree corner: a new letter"), N.NameOf(S4), FString(TEXT("B")));
	TestEqual(TEXT("the first click's segment is still A"), N.NameOf(S1), FString(TEXT("A")));
	TestEqual(TEXT("four clicks, two taxiways"), N.Alive(), 2);

	// AT A JUNCTION: the first taxiway runs north-south through Q; D ends at Q from the west. F leaves Q 30 degrees off
	// D's line - within BendDegrees but NOT within the junction's 10 - so it is new; E then carries straight on from D.
	FTaxiwayNamesNet J;
	const FRoadNodeId Q = J.Node(200000.0, 0.0);
	J.Click(J.Node(200000.0, -50000.0), Q);
	const FRoadSegmentId C = J.Click(Q, J.Node(200000.0, 50000.0));
	const FRoadSegmentId D = J.Click(J.Node(150000.0, 0.0), Q);
	TestEqual(TEXT("the north-south one is A"), J.NameOf(C), FString(TEXT("A")));
	TestEqual(TEXT("D meets A's middle: a new letter"), J.NameOf(D), FString(TEXT("B")));
	const FVector2D Off = FVector2D(200000.0, 0.0) + FVector2D(FMath::Cos(FMath::DegreesToRadians(30.0)),
		FMath::Sin(FMath::DegreesToRadians(30.0))) * 50000.0;
	const FRoadSegmentId F = J.Click(Q, J.Node(Off.X, Off.Y));
	TestEqual(TEXT("30 degrees off B's line at a junction is NOT in line (10 deg there): a new letter"), J.NameOf(F), FString(TEXT("C")));
	const FRoadSegmentId E = J.Click(Q, J.Node(250000.0, 0.0));
	TestEqual(TEXT("straight on through the junction from B's end: B"), J.NameOf(E), FString(TEXT("B")));
	return true;
}

/** Connector: a short chain anchored at both ends is its parent's next number; leaving a runway, the taxiway is the parent. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesConnectorTest, "Airside.Model.TaxiwayNames.Connector",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesConnectorTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadNodeId N0 = N.Node(0.0, 0.0);
	const FRoadNodeId N1 = N.Node(50000.0, 0.0);
	const FRoadNodeId N2 = N.Node(100000.0, 0.0);
	const FRoadSegmentId A = N.Click(N0, N1);
	N.Click(N1, N2);
	const FRoadNodeId R0 = N.Node(0.0, 25000.0);
	const FRoadNodeId R1 = N.Node(50000.0, 25000.0);
	const FRoadNodeId R2 = N.Node(100000.0, 25000.0);
	N.Lay(R0, R1, N.Runway);
	N.Lay(R1, R2, N.Runway);
	N.Normalise();

	const FRoadSegmentId Stub = N.Click(N1, N.Node(50000.0, -20000.0));
	TestEqual(TEXT("a 200 m stub off A to a dead end is A's first connector (plan D2)"), N.NameOf(Stub), FString(TEXT("A1")));
	const FRoadSegmentId Link = N.Click(R2, N2);
	TestEqual(TEXT("a 250 m link FROM the runway TO A is A's: the taxiway is the parent"), N.NameOf(Link), FString(TEXT("A2")));
	const FRoadSegmentId Long = N.Click(R1, N.Node(50000.0, -60000.0));
	TestEqual(TEXT("an 850 m link is no connector: a letter"), N.NameOf(Long), FString(TEXT("B")));
	TestEqual(TEXT("A lists its two connectors"), N.Net->TaxiwayConnectorCount(N.Net->TaxiwayOf(A)), 2);
	const FTaxiway* Connector = N.Net->GetTaxiway(N.Net->TaxiwayOf(Stub));
	if (TestNotNull(TEXT("the stub's taxiway"), Connector))
	{
		TestTrue(TEXT("derived, not stored: its own Name is empty"), Connector->Name.IsEmpty());
		TestEqual(TEXT("parented on A"), Connector->ParentId, N.Net->TaxiwayOf(A));
	}
	return true;
}

/** Insert a node / split a segment: both halves keep the taxiway, before any normalise. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesSplitTest, "Airside.Model.TaxiwayNames.SplitKeepsTheTaxiway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesSplitTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadSegmentId S = N.Click(N.Node(0.0, 0.0), N.Node(100000.0, 0.0));
	const int32 Was = N.Net->TaxiwayOf(S);
	const FRoadNodeId Mid = N.Net->SplitSegment(S, FVector2D(50000.0, 0.0));
	const FRoadNode* MidNode = N.Net->GetNode(Mid);
	if (!TestNotNull(TEXT("the split made a node"), MidNode)) { return false; }
	const TArray<FRoadSegmentId> Halves = MidNode->Incident;
	TestEqual(TEXT("two halves"), Halves.Num(), 2);
	for (const FRoadSegmentId& Half : Halves)
	{
		TestEqual(TEXT("the split itself carries the taxiway to each half"), N.Net->TaxiwayOf(Half), Was);
	}
	TestEqual(TEXT("so normalising renames nothing"), N.Normalise().Num(), 0);
	TestEqual(TEXT("and it is still one taxiway"), N.Alive(), 1);
	return true;
}

/** Two taxiways meeting in line each keep their own; the junction reads "A/B". */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesInLineMeetTest, "Airside.Model.TaxiwayNames.InLineMeetingKeepsBoth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesInLineMeetTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadNodeId A1 = N.Node(50000.0, 0.0);
	const FRoadSegmentId A = N.Click(N.Node(0.0, 0.0), A1);
	const FRoadNodeId B0 = N.Node(60000.0, 0.0);
	const FRoadSegmentId B = N.Click(B0, N.Node(110000.0, 0.0));
	TestTrue(TEXT("merged end to end"), N.Net->MergeNodes(A1, B0));
	TestEqual(TEXT("meeting in line renames nothing"), N.Normalise().Num(), 0);
	TestEqual(TEXT("A is A"), N.NameOf(A), FString(TEXT("A")));
	TestEqual(TEXT("B is B"), N.NameOf(B), FString(TEXT("B")));
	TestEqual(TEXT("the junction is named by both"), N.Net->JunctionName(A1), FString(TEXT("A/B")));
	return true;
}
```

- [ ] **Step 2: Build - expect a compile failure**

Expected: `'NormaliseTaxiways': is not a member of 'URoadNetwork'`, `'JunctionName': is not a member`.

- [ ] **Step 3: Implement**

RoadNetwork.h public block (after `TaxiwayConnectorCount`):
```cpp

	/**
	 * THE ONE PASS that keeps names true (spec "The single-chain invariant"), run after EVERY topology change from ONE
	 * place - URoadEditFacade::NotifyChanged for EChangeKind::Topology (plan D9) - and on load (EnsureTaxiwayNames).
	 * Mutators never patch names themselves (issue #255: fixes at sites regress; fixes at shapes hold). In order:
	 *   0. a name only on a live taxiway segment (a road or a runway carries none);
	 *   1. every UNNAMED taxiway chain named by AssignTaxiway, longest first (plan D4);
	 *   2-4. the invariant: a branch split off, a disconnected piece split off, an empty taxiway retired.
	 * Deterministic: same network in, same names out. Returns one FTaxiwayRename per split ("C split off from A"); logs
	 * each as "TaxiwayNames: C split off from A". Moves GuidelineRevision when it changed anything (NoteFactChanged):
	 * the inspector card and the labels key on it.
	 * ENFORCED BY: Airside.Model.TaxiwayNames.InvariantUnderRandomEdits, Airside.Present.TaxiwayNames.SplitIsAnnouncedAndUndone
	 */
	TArray<FTaxiwayRename> NormaliseTaxiways(const FTaxiwayNamingRules& Rules);

	/** The taxiway's segments in chain order, from an end (a node holding one of them); a closed loop starts at its lowest. */
	FTaxiwayChain TaxiwayChainOf(int32 TaxiwayId) const;

	/** A junction named by the taxiways meeting there, sorted, "/"-joined ("A/B"); "A" for one; empty for none (plan D13). */
	FString JunctionName(FRoadNodeId Node) const;
```
RoadNetwork.h private (after `FindTaxiwayMutable`):
```cpp
	/**
	 * Name ONE unnamed chain (spec "Assignment rules"), in order:
	 *   inherit - it carries on (RoadGeom::IsInLine at a junction, FTaxiwayNamingRules::BendDegrees at a bend, plan D1)
	 *             from a node where a named taxiway ENDS -> that taxiway, then RejudgeTaxiway (plan D3);
	 *   connector - shorter than ConnectorMaxLength, both ends anchored (a named taxiway, a runway, or a dead end - plan
	 *             D2) and a named taxiway at one end -> a connector of the lettered taxiway at its FIRST end, else its other;
	 *   letter - anything else -> the next free letter.
	 * ENFORCED BY: Airside.Model.TaxiwayNames.Assign / .ClickByClickDrawing / .Connector
	 */
	int32 AssignTaxiway(const FTaxiwayChain& Chain, const FTaxiwayNamingRules& Rules);
	int32 AssignUnnamedTaxiways(const FTaxiwayNamingRules& Rules);
	void RejudgeTaxiway(int32 TaxiwayId, const FTaxiwayNamingRules& Rules);
	int32 MintTaxiway(int32 ParentId);
	int32 IssueConnectorNumber(int32 ParentId);
	FString NextFreeTaxiwayLetter(int32 Except) const;
```
RoadNetworkTaxiways.cpp - add after the includes, before `GetTaxiway`:
```cpp
namespace
{
	// Prefixed against the UNITY build, like every file-local helper in this module.

	double TaxiwayNamesLength(const URoadNetwork& Net, FRoadSegmentId Id)
	{
		// THE ONE LENGTH a road has here: GuidelineGeom's sampled length of its own Bezier, the figure the route search
		// already costs - not the chord, which would rank a bent taxiway shorter than it is drawn.
		FVector2D A;
		FVector2D B;
		const FRoadSegment* Segment = Net.GetSegment(Id);
		return Segment != nullptr && Net.SegmentEnds(Id, A, B) ? GuidelineGeom::Length(A, Segment->Control, B) : 0.0;
	}

	/** Does the road carry on from From to Other through Node? A bend (two roads at the node) within BendDegrees, a
	 *  junction within RoadGeom::InLineDegrees (plan D1). OutAngle is the straightness, PI being straight. */
	bool TaxiwayNamesCarriesOn(const URoadNetwork& Net, FRoadNodeId Node, FRoadSegmentId From, FRoadSegmentId Other,
		const FTaxiwayNamingRules& Rules, double& OutAngle)
	{
		const FRoadNode* At = Net.GetNode(Node);
		const FVector2D OutFrom = Net.GetOutgoingTangent(From, Node);
		const FVector2D OutOther = Net.GetOutgoingTangent(Other, Node);
		OutAngle = RoadGeom::AngleBetween(OutFrom, OutOther);
		if (At == nullptr)
		{
			return false;
		}
		return At->Incident.Num() == 2
			? OutAngle >= UE_DOUBLE_PI - FMath::DegreesToRadians(Rules.BendDegrees)
			: RoadGeom::IsInLine(OutFrom, OutOther);
	}

	TArray<FRoadSegmentId> TaxiwayNamesOwnAt(const URoadNetwork& Net, FRoadNodeId Node, int32 TaxiwayId)
	{
		TArray<FRoadSegmentId> Own;
		if (const FRoadNode* At = Net.GetNode(Node))
		{
			for (const FRoadSegmentId& Each : At->Incident)
			{
				if (Net.TaxiwayOf(Each) == TaxiwayId)
				{
					Own.Add(Each);
				}
			}
		}
		return Own;
	}

	/** Walk both ways from Seed, NextAt choosing each step; Taken stops a loop and keeps chains disjoint. */
	FTaxiwayChain TaxiwayNamesWalk(const URoadNetwork& Net, FRoadSegmentId Seed,
		TFunctionRef<FRoadSegmentId(FRoadNodeId, FRoadSegmentId)> NextAt, TSet<FRoadSegmentId>& Taken)
	{
		FTaxiwayChain Chain;
		const FRoadSegment* Start = Net.GetSegment(Seed);
		if (Start == nullptr)
		{
			return Chain;
		}
		Taken.Add(Seed);
		TArray<FRoadSegmentId> Back;
		TArray<FRoadSegmentId> Ahead;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			FRoadNodeId Node = Side == 0 ? Start->A : Start->B;
			FRoadSegmentId Previous = Seed;
			TArray<FRoadSegmentId>& Into = Side == 0 ? Back : Ahead;
			for (;;)
			{
				const FRoadSegmentId Next = NextAt(Node, Previous);
				if (!Next.IsSet() || Taken.Contains(Next))
				{
					break;
				}
				Taken.Add(Next);
				Into.Add(Next);
				Node = Net.GetOtherEnd(Next, Node);
				Previous = Next;
			}
			(Side == 0 ? Chain.First : Chain.Last) = Node;
		}
		Algo::Reverse(Back);
		Chain.Segments = MoveTemp(Back);
		Chain.Segments.Add(Seed);
		Chain.Segments.Append(Ahead);
		for (const FRoadSegmentId& Id : Chain.Segments)
		{
			Chain.Length += TaxiwayNamesLength(Net, Id);
			Chain.LowestIndex = FMath::Min(Chain.LowestIndex, Id.Index);
		}
		return Chain;
	}

	/** The LETTERED taxiways at Node outside Chain - a connector there counts as its parent. Sorted, so lowest id first. */
	TArray<int32> TaxiwayNamesRootsAt(const URoadNetwork& Net, FRoadNodeId Node, const FTaxiwayChain& Chain)
	{
		TArray<int32> Roots;
		if (const FRoadNode* At = Net.GetNode(Node))
		{
			for (const FRoadSegmentId& Other : At->Incident)
			{
				const FTaxiway* Taxiway = Chain.Segments.Contains(Other) ? nullptr : Net.GetTaxiway(Net.TaxiwayOf(Other));
				if (Taxiway != nullptr)
				{
					Roots.AddUnique(Taxiway->IsConnector() ? Taxiway->ParentId : Taxiway->Id);
				}
			}
		}
		Roots.Sort();
		return Roots;
	}

	/** An end a connector may stop at: a named taxiway, a runway, or a dead end - nothing but Chain there (plan D2). */
	bool TaxiwayNamesAnchored(const URoadNetwork& Net, FRoadNodeId Node, const FTaxiwayChain& Chain)
	{
		const FRoadNode* At = Net.GetNode(Node);
		if (At == nullptr)
		{
			return false;
		}
		bool bDeadEnd = true;
		for (const FRoadSegmentId& Other : At->Incident)
		{
			if (Chain.Segments.Contains(Other))
			{
				continue;
			}
			bDeadEnd = false;
			if (Net.IsRunwaySegment(Other) || Net.TaxiwayOf(Other) != INDEX_NONE)
			{
				return true;
			}
		}
		return bDeadEnd;
	}

	/** The parent Chain would be a connector of, or INDEX_NONE (a letter). Except: a taxiway that may not parent it (itself). */
	int32 TaxiwayNamesConnectorParent(const URoadNetwork& Net, const FTaxiwayChain& Chain, const FTaxiwayNamingRules& Rules,
		int32 Except)
	{
		if (Chain.Segments.Num() == 0 || Chain.Length >= Rules.ConnectorMaxLength
			|| !TaxiwayNamesAnchored(Net, Chain.First, Chain) || !TaxiwayNamesAnchored(Net, Chain.Last, Chain))
		{
			return INDEX_NONE;
		}
		// FIRST END FIRST (spec: "connector of the taxiway it leaves (first end)"); a first end on a runway has no root,
		// so the taxiway at the other end parents it - "if it leaves a runway and lands on a taxiway, the taxiway is the parent".
		for (const FRoadNodeId& End : { Chain.First, Chain.Last })
		{
			for (const int32 Root : TaxiwayNamesRootsAt(Net, End, Chain))
			{
				if (Root != Except)
				{
					return Root;
				}
			}
		}
		return INDEX_NONE;
	}

	/** The taxiway Chain carries on from: one that ENDS at an end of Chain (one of its segments there) and lines up
	 *  with Chain's terminal segment. The straightest wins, then the lowest id; the first end before the last. */
	int32 TaxiwayNamesInheritable(const URoadNetwork& Net, const FTaxiwayChain& Chain, const FTaxiwayNamingRules& Rules)
	{
		for (int32 EndIndex = 0; EndIndex < 2; ++EndIndex)
		{
			const FRoadNodeId End = EndIndex == 0 ? Chain.First : Chain.Last;
			const FRoadSegmentId Terminal = EndIndex == 0 ? Chain.Segments[0] : Chain.Segments.Last();
			const FRoadNode* At = Net.GetNode(End);
			if (At == nullptr)
			{
				continue;
			}
			int32 Best = INDEX_NONE;
			double BestAngle = -1.0;
			for (const FRoadSegmentId& Other : At->Incident)
			{
				const int32 Owner = Net.TaxiwayOf(Other);
				double Angle = 0.0;
				if (Owner == INDEX_NONE || Chain.Segments.Contains(Other) || TaxiwayNamesOwnAt(Net, End, Owner).Num() != 1
					|| !TaxiwayNamesCarriesOn(Net, End, Terminal, Other, Rules, Angle))
				{
					continue;
				}
				if (Angle > BestAngle || (Angle == BestAngle && Owner < Best))
				{
					Best = Owner;
					BestAngle = Angle;
				}
			}
			if (Best != INDEX_NONE)
			{
				return Best;
			}
		}
		return INDEX_NONE;
	}
}
```
Then the members (append to the file):
```cpp
FString URoadNetwork::NextFreeTaxiwayLetter(int32 Except) const
{
	// A LETTER IS FREE ONLY WHEN NOTHING DISPLAYS IT (spec): an empty parent whose connectors survive is still alive, so
	// its letter stays taken and a reused "A" can never mint a second "A1".
	for (int32 Index = 0;; ++Index)
	{
		const FString Letter = TaxiwayLetters::LetterAt(Index);
		if (!IsTaxiwayNameTaken(Letter, Except))
		{
			return Letter;
		}
	}
}

int32 URoadNetwork::IssueConnectorNumber(int32 ParentId)
{
	FTaxiway* Parent = FindTaxiwayMutable(ParentId);
	if (Parent == nullptr)
	{
		return 0;
	}
	// NEVER REUSED while the parent lives (NextConnectorNumber only advances), and never a number a player's override
	// already shows ("K7" renamed onto a connector of B would collide with K's seventh).
	const FString Root = TaxiwayDisplayName(ParentId);
	int32 Number = FMath::Max(Parent->NextConnectorNumber, 1);
	while (IsTaxiwayNameTaken(Root + FString::FromInt(Number), INDEX_NONE))
	{
		++Number;
	}
	Parent->NextConnectorNumber = Number + 1;
	return Number;
}

int32 URoadNetwork::MintTaxiway(int32 ParentId)
{
	FTaxiway Fresh;
	Fresh.Id = Taxiways.Num();
	Fresh.bAlive = true;
	if (GetTaxiway(ParentId) != nullptr)
	{
		Fresh.ParentId = ParentId;
		Fresh.ConnectorNumber = IssueConnectorNumber(ParentId);
	}
	else
	{
		Fresh.Name = NextFreeTaxiwayLetter(INDEX_NONE);
	}
	Taxiways.Add(MoveTemp(Fresh));
	return Taxiways.Last().Id;
}

int32 URoadNetwork::AssignTaxiway(const FTaxiwayChain& Chain, const FTaxiwayNamingRules& Rules)
{
	if (Chain.Segments.Num() == 0)
	{
		return INDEX_NONE;
	}
	int32 Named = TaxiwayNamesInheritable(*this, Chain, Rules);
	const bool bInherited = Named != INDEX_NONE;
	if (!bInherited)
	{
		Named = MintTaxiway(TaxiwayNamesConnectorParent(*this, Chain, Rules, INDEX_NONE));
	}
	for (const FRoadSegmentId& Segment : Chain.Segments)
	{
		WriteTaxiwayId(Segment, Named);
	}
	if (bInherited)
	{
		RejudgeTaxiway(Named, Rules);
	}
	return Named;
}

void URoadNetwork::RejudgeTaxiway(int32 TaxiwayId, const FTaxiwayNamingRules& Rules)
{
	// "AS DRAWN SO FAR" (plan D3): a gesture is many commits, one per click, so a taxiway that just grew is judged again
	// by the same table, on its whole chain - but only an AUTO-named one, and only across the letter/connector line.
	const FTaxiway* Judged = GetTaxiway(TaxiwayId);
	if (Judged == nullptr || Judged->bPlayerNamed)
	{
		return;
	}
	const FTaxiwayChain Whole = TaxiwayChainOf(TaxiwayId);
	const FString Was = TaxiwayDisplayName(TaxiwayId);
	if (!Judged->IsConnector())
	{
		// A LETTER WITH CONNECTORS STAYS ONE: its connectors' names derive from it.
		const int32 Parent = HasTaxiwayConnectors(TaxiwayId) ? INDEX_NONE
			: TaxiwayNamesConnectorParent(*this, Whole, Rules, TaxiwayId);
		if (Parent == INDEX_NONE)
		{
			return;
		}
		const int32 Number = IssueConnectorNumber(Parent);
		FTaxiway& Becomes = Taxiways[TaxiwayId];
		Becomes.Name.Reset();
		Becomes.ParentId = Parent;
		Becomes.ConnectorNumber = Number;
	}
	else if (Whole.Length >= Rules.ConnectorMaxLength)
	{
		const FString Letter = NextFreeTaxiwayLetter(TaxiwayId);
		FTaxiway& Becomes = Taxiways[TaxiwayId];
		Becomes.ParentId = INDEX_NONE;
		Becomes.ConnectorNumber = 0;
		Becomes.Name = Letter;
	}
	else
	{
		return;
	}
	UE_LOG(LogAirside, Log, TEXT("TaxiwayNames: %s is now %s (as drawn so far)"), *Was, *TaxiwayDisplayName(TaxiwayId));
}

int32 URoadNetwork::AssignUnnamedTaxiways(const FTaxiwayNamingRules& Rules)
{
	const auto Unnamed = [this](FRoadSegmentId Id) { return TaxiwayStrip::HasStrip(*this, Id) && TaxiwayOf(Id) == INDEX_NONE; };
	TSet<FRoadSegmentId> Taken;
	TArray<FTaxiwayChain> Chains;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = SegmentIdAt(Index);
		if (!Id.IsSet() || Taken.Contains(Id) || !Unnamed(Id))
		{
			continue;
		}
		Chains.Add(TaxiwayNamesWalk(*this, Id, [this, &Rules, &Unnamed](FRoadNodeId Node, FRoadSegmentId From)
		{
			// THE STRAIGHTEST unnamed continuation; Incident's own order (bearing-sorted) settles an exact tie.
			FRoadSegmentId Best;
			double BestAngle = -1.0;
			if (const FRoadNode* At = GetNode(Node))
			{
				for (const FRoadSegmentId& Other : At->Incident)
				{
					double Angle = 0.0;
					if (Other != From && Unnamed(Other) && TaxiwayNamesCarriesOn(*this, Node, From, Other, Rules, Angle)
						&& Angle > BestAngle)
					{
						Best = Other;
						BestAngle = Angle;
					}
				}
			}
			return Best;
		}, Taken));
	}
	// LONGEST FIRST (spec, backfill): the long parallels take the early letters, and a connector is judged after the
	// taxiways it joins are named. Ties by lowest segment index, so the order never depends on a hash.
	Chains.Sort([](const FTaxiwayChain& L, const FTaxiwayChain& R)
	{
		return L.Length != R.Length ? L.Length > R.Length : L.LowestIndex < R.LowestIndex;
	});
	for (const FTaxiwayChain& Chain : Chains)
	{
		AssignTaxiway(Chain, Rules);
	}
	return Chains.Num();
}

TArray<FTaxiwayRename> URoadNetwork::NormaliseTaxiways(const FTaxiwayNamingRules& Rules)
{
	TArray<FTaxiwayRename> Renames;
	int32 Changes = 0;

	// 0. A NAME ONLY ON A LIVE TAXIWAY SEGMENT. A merge can keep the WIDER arm (MergeNodes' CASE 2) and an upgrade can
	// never change kind (SetSegmentProfile), but a load or a hand edit can still hand a road a stale id.
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = SegmentIdAt(Index);
		if (Id.IsSet() && Segments[Index].TaxiwayId != INDEX_NONE
			&& (!TaxiwayStrip::HasStrip(*this, Id) || GetTaxiway(Segments[Index].TaxiwayId) == nullptr))
		{
			WriteTaxiwayId(Id, INDEX_NONE);
			++Changes;
		}
	}

	// 1. EVERY NEW CHAIN NAMED - first, so a heal that rejoins two pieces of A inherits A before step 3 would split them.
	Changes += AssignUnnamedTaxiways(Rules);

	Changes += Renames.Num();
	for (const FTaxiwayRename& Rename : Renames)
	{
		UE_LOG(LogAirside, Log, TEXT("TaxiwayNames: %s split off from %s"), *Rename.SplitOff, *Rename.From);
	}
	if (Changes > 0)
	{
		NoteFactChanged();
	}
	return Renames;
}

FTaxiwayChain URoadNetwork::TaxiwayChainOf(int32 TaxiwayId) const
{
	FRoadSegmentId Seed;
	FRoadSegmentId Lowest;
	for (int32 Index = 0; Index < Segments.Num() && !Seed.IsSet(); ++Index)
	{
		const FRoadSegmentId Id = SegmentIdAt(Index);
		if (!Id.IsSet() || TaxiwayOf(Id) != TaxiwayId)
		{
			continue;
		}
		if (!Lowest.IsSet())
		{
			Lowest = Id;
		}
		if (TaxiwayNamesOwnAt(*this, Segments[Index].A, TaxiwayId).Num() == 1
			|| TaxiwayNamesOwnAt(*this, Segments[Index].B, TaxiwayId).Num() == 1)
		{
			Seed = Id;
		}
	}
	if (!Seed.IsSet())
	{
		Seed = Lowest;
	}
	if (!Seed.IsSet())
	{
		return FTaxiwayChain();
	}
	TSet<FRoadSegmentId> Taken;
	return TaxiwayNamesWalk(*this, Seed, [this, TaxiwayId](FRoadNodeId Node, FRoadSegmentId From)
	{
		const TArray<FRoadSegmentId> Own = TaxiwayNamesOwnAt(*this, Node, TaxiwayId);
		return Own.Num() == 2 ? (Own[0] == From ? Own[1] : Own[0]) : FRoadSegmentId();
	}, Taken);
}

FString URoadNetwork::JunctionName(FRoadNodeId Node) const
{
	TArray<FString> Names;
	if (const FRoadNode* At = GetNode(Node))
	{
		for (const FRoadSegmentId& Each : At->Incident)
		{
			const FString Name = TaxiwayDisplayName(TaxiwayOf(Each));
			if (!Name.IsEmpty())
			{
				Names.AddUnique(Name);
			}
		}
	}
	Names.Sort();
	return FString::Join(Names, TEXT("/"));
}
```
RoadNetwork.cpp SplitSegment: after line 161 (`const EPavement KeepSurface = Segment->Surface;`) add `const int32 KeepTaxiway = Segment->TaxiwayId;`. In the `for (const FRoadSegmentId& Half : { First, Second })` loop (line 200), after the `if (FRoadSegment* Fresh ...) { ... }` block, add:
```cpp
		// AND THE TAXIWAY (spec: "insert a node / split a segment: both halves keep the taxiway") - through the one
		// writer. ENFORCED BY: Airside.Model.TaxiwayNames.SplitKeepsTheTaxiway, Check-Architecture rule 103
		WriteTaxiwayId(Half, KeepTaxiway);
```
Then extend the comment above the loop by one line: `// THE TAXIWAY NAME LIKEWISE (2026-10-02), below.`

- [ ] **Step 4: Build, run**

Run `-Filter Airside.Model.TaxiwayNames`. Expected: `7 test(s) run, 0 failed, 0 crashed`. Check `wc -l Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp` is at most 2287.

- [ ] **Step 5: Commit**
```powershell
git add Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp `
  Plugins/Airside/Source/Airside/Private/Model/RoadNetworkTaxiways.cpp Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp
git commit -m "model: name new taxiway chains - letter, inherit, connector; split keeps the name" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

---

### Task 4: The single-chain invariant - branch, pieces, empty, numbers, re-judge

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h` (private helpers)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetworkTaxiways.cpp` (`NormaliseTaxiways` steps 2-4)
- Test: `Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp`

**Interfaces:**
- Consumes: Task 3's `NormaliseTaxiways`, `MintTaxiway`, file-locals `TaxiwayNamesOwnAt`, `TaxiwayNamesLength`.
- Produces: private `void SplitTaxiwayBranches(int32, TArray<FTaxiwayRename>&)`, `void SplitTaxiwayPieces(int32, TArray<FTaxiwayRename>&)`, `int32 RetireEmptyTaxiways()`.

- [ ] **Step 1: Write the failing tests** - append:

```cpp
/** Disconnected: the longer piece keeps the taxiway, the other gets the next free letter, and says so once. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesDisconnectTest, "Airside.Model.TaxiwayNames.DisconnectedPieceSplitsOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesDisconnectTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadNodeId N0 = N.Node(0.0, 0.0);
	const FRoadNodeId N1 = N.Node(40000.0, 0.0);
	const FRoadNodeId N2 = N.Node(50000.0, 0.0);
	const FRoadNodeId N3 = N.Node(70000.0, 0.0);
	const FRoadSegmentId Long = N.Click(N0, N1);
	const FRoadSegmentId Middle = N.Click(N1, N2);
	const FRoadSegmentId Short = N.Click(N2, N3);
	TestEqual(TEXT("setup: one taxiway A"), N.NameOf(Short), FString(TEXT("A")));
	TestTrue(TEXT("the middle goes"), N.Net->RemoveSegment(Middle));
	const TArray<FTaxiwayRename> Renames = N.Normalise();
	if (!TestEqual(TEXT("one split, one report"), Renames.Num(), 1)) { return false; }
	TestEqual(TEXT("B split off"), Renames[0].SplitOff, FString(TEXT("B")));
	TestEqual(TEXT("from A"), Renames[0].From, FString(TEXT("A")));
	TestEqual(TEXT("the 400 m piece keeps A"), N.NameOf(Long), FString(TEXT("A")));
	TestEqual(TEXT("the 200 m piece is B"), N.NameOf(Short), FString(TEXT("B")));
	return true;
}

/** Branch: A's end merged onto A's middle - the shortest branch at the node splits off as the next letter. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesBranchTest, "Airside.Model.TaxiwayNames.BranchSplitsOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesBranchTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	TArray<FRoadNodeId> P;
	for (int32 Index = 0; Index <= 4; ++Index) { P.Add(N.Node(30000.0 * Index, 0.0)); }
	const FRoadSegmentId Tail = N.Click(P[0], P[1]);
	const FRoadSegmentId Loop = N.Click(P[1], P[2]);
	N.Click(P[2], P[3]);
	N.Click(P[3], P[4]);
	TestEqual(TEXT("setup: one taxiway"), N.Alive(), 1);
	TestTrue(TEXT("A's far end merged onto its second node"), N.Net->MergeNodes(P[1], P[4]));
	const TArray<FTaxiwayRename> Renames = N.Normalise();
	if (!TestEqual(TEXT("one branch split off"), Renames.Num(), 1)) { return false; }
	TestEqual(TEXT("the 300 m tail is the shortest branch: B"), N.NameOf(Tail), FString(TEXT("B")));
	TestEqual(TEXT("the loop keeps A"), N.NameOf(Loop), FString(TEXT("A")));
	return true;
}

/** Empty: a parent with surviving connectors keeps its letter reserved; with none it is retired and A returns to the pool. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesReservationTest, "Airside.Model.TaxiwayNames.CollapseAndLetterReservation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesReservationTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadNodeId A0 = N.Node(0.0, 0.0);
	const FRoadNodeId A1 = N.Node(50000.0, 0.0);
	const FRoadNodeId A2 = N.Node(100000.0, 0.0);
	const FRoadSegmentId First = N.Click(A0, A1);
	const FRoadSegmentId Second = N.Click(A1, A2);
	const FRoadSegmentId Stub = N.Click(A1, N.Node(50000.0, -20000.0));
	const int32 AId = N.Net->TaxiwayOf(First);
	TestEqual(TEXT("setup: A1"), N.NameOf(Stub), FString(TEXT("A1")));
	N.Net->RemoveSegment(First);
	N.Net->RemoveSegment(Second);
	N.Normalise();
	TestNotNull(TEXT("A is kept with no segment: A1 still reads from it"), N.Net->GetTaxiway(AId));
	TestEqual(TEXT("A1 still reads A1"), N.NameOf(Stub), FString(TEXT("A1")));
	const FRoadSegmentId Fresh = N.Click(N.Node(0.0, 200000.0), N.Node(60000.0, 200000.0));
	TestEqual(TEXT("a new taxiway is B - A is reserved, or a second A1 could be minted"), N.NameOf(Fresh), FString(TEXT("B")));
	N.Net->RemoveSegment(Stub);
	N.Normalise();
	TestNull(TEXT("A goes with its last connector"), N.Net->GetTaxiway(AId));
	const FRoadSegmentId Again = N.Click(N.Node(0.0, 300000.0), N.Node(60000.0, 300000.0));
	TestEqual(TEXT("and A is back in the pool"), N.NameOf(Again), FString(TEXT("A")));
	return true;
}

/** A connector number is never reissued while its parent lives. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesNumbersTest, "Airside.Model.TaxiwayNames.ConnectorNumbersNeverReused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesNumbersTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	TArray<FRoadNodeId> A;
	for (int32 Index = 0; Index <= 3; ++Index) { A.Add(N.Node(50000.0 * Index, 0.0)); }
	for (int32 Index = 0; Index < 3; ++Index) { N.Click(A[Index], A[Index + 1]); }
	const FRoadSegmentId One = N.Click(A[1], N.Node(50000.0, -20000.0));
	const FRoadSegmentId Two = N.Click(A[2], N.Node(100000.0, -20000.0));
	TestEqual(TEXT("A1"), N.NameOf(One), FString(TEXT("A1")));
	TestEqual(TEXT("A2"), N.NameOf(Two), FString(TEXT("A2")));
	N.Net->RemoveSegment(One);
	N.Normalise();
	const FRoadSegmentId Three = N.Click(A[1], N.Node(50000.0, 20000.0));
	TestEqual(TEXT("the next is A3, never the retired A1"), N.NameOf(Three), FString(TEXT("A3")));
	return true;
}

/** REVIEW FOCUS 2 - a gesture is many commits: names are judged "as drawn so far" (plan D3). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesRejudgeTest, "Airside.Model.TaxiwayNames.DrawnSoFarIsRejudged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesRejudgeTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	{
		// A TWO-CLICK CONNECTOR: a stub off A's middle, then on to the runway 250 m away, in line.
		FTaxiwayNamesNet N;
		const FRoadNodeId Mid = N.Node(50000.0, 0.0);
		N.Click(N.Node(0.0, 0.0), Mid);
		N.Click(Mid, N.Node(100000.0, 0.0));
		const FRoadNodeId R = N.Node(50000.0, 25000.0);
		N.Lay(N.Node(0.0, 25000.0), R, N.Runway);
		N.Lay(R, N.Node(100000.0, 25000.0), N.Runway);
		N.Normalise();
		const FRoadNodeId Free = N.Node(50000.0, 15000.0);
		const FRoadSegmentId Click1 = N.Click(Mid, Free);
		const FRoadSegmentId Click2 = N.Click(Free, R);
		TestEqual(TEXT("click 1 is A1"), N.NameOf(Click1), FString(TEXT("A1")));
		TestEqual(TEXT("click 2 carries A1 on to the runway"), N.NameOf(Click2), FString(TEXT("A1")));
	}
	{
		// A STUB GROWN LONG: 200 m off A (A1), then straight on to 600 m - no connector any more.
		FTaxiwayNamesNet N;
		const FRoadNodeId Mid = N.Node(50000.0, 0.0);
		N.Click(N.Node(0.0, 0.0), Mid);
		N.Click(Mid, N.Node(100000.0, 0.0));
		const FRoadNodeId Free = N.Node(50000.0, -20000.0);
		const FRoadSegmentId Stub = N.Click(Mid, Free);
		TestEqual(TEXT("setup: A1"), N.NameOf(Stub), FString(TEXT("A1")));
		const FRoadSegmentId On = N.Lay(Free, N.Node(50000.0, -60000.0));
		const TArray<FTaxiwayRename> Renames = N.Normalise();
		TestEqual(TEXT("600 m is a letter now: B"), N.NameOf(On), FString(TEXT("B")));
		TestEqual(TEXT("the stub went with it"), N.NameOf(Stub), FString(TEXT("B")));
		TestEqual(TEXT("re-judging is not a split: no toast"), Renames.Num(), 0);
	}
	{
		// A LETTER FROM OPEN GROUND THAT LANDS ON A: 100 m alone is B (no taxiway at either end), and once it reaches
		// A's end, 250 m in all, it is A's connector - and B goes back in the pool.
		FTaxiwayNamesNet N;
		const FRoadNodeId End = N.Node(200000.0, 0.0);
		N.Click(N.Node(100000.0, 0.0), End);
		const FRoadNodeId Free = N.Node(200000.0, -15000.0);
		const FRoadSegmentId Click1 = N.Click(N.Node(200000.0, -25000.0), Free);
		TestEqual(TEXT("click 1 has no taxiway at either end: B"), N.NameOf(Click1), FString(TEXT("B")));
		N.Click(Free, End);
		TestEqual(TEXT("it reached A: A1"), N.NameOf(Click1), FString(TEXT("A1")));
		const FRoadSegmentId Next = N.Click(N.Node(0.0, 100000.0), N.Node(60000.0, 100000.0));
		TestEqual(TEXT("B is free again"), N.NameOf(Next), FString(TEXT("B")));
	}
	return true;
}
```
Note on the third case: `Click(Free, End)` lays Free->End; at `End` A has one segment and the corner is 90 degrees (no inherit from A); at `Free` the chain carries straight on from B's end (inherit B), then `RejudgeTaxiway(B)` finds a 250 m chain anchored at a dead end and A -> connector of A.

- [ ] **Step 2: Build, run - expect failures**

`-Filter Airside.Model.TaxiwayNames`. Expected FAIL: `DisconnectedPieceSplitsOff` ("one split, one report" 0 != 1), `BranchSplitsOff`, `CollapseAndLetterReservation` ("A goes with its last connector"). `ConnectorNumbersNeverReused` and `DrawnSoFarIsRejudged` may already pass (Task 3's issue and re-judge code) - they pin behaviour that must survive this task.

- [ ] **Step 3: Implement**

RoadNetwork.h private (after `NextFreeTaxiwayLetter`):
```cpp
	/** Invariant step 2 (spec "Branch"): while a node holds 3+ of the taxiway's segments, the shortest branch from it
	 *  (by length, ties by lowest first-segment index) becomes the next free letter. */
	void SplitTaxiwayBranches(int32 TaxiwayId, TArray<FTaxiwayRename>& OutRenames);
	/** Invariant step 3 (spec "Disconnected"): the longest piece keeps it (ties by lowest index); each other piece is a new letter. */
	void SplitTaxiwayPieces(int32 TaxiwayId, TArray<FTaxiwayRename>& OutRenames);
	/** Invariant step 4 (spec "Empty"): a taxiway with no segment and no live connector is retired; one with a
	 *  connector stays, its letter reserved. Returns how many it retired. */
	int32 RetireEmptyTaxiways();
```
RoadNetworkTaxiways.cpp - in `NormaliseTaxiways`, between step 1 and `Changes += Renames.Num();` insert:
```cpp
	// 2-3. ONE CHAIN PER TAXIWAY. Indices, not a range-for: a split mints into Taxiways while this walks it, and a
	// split-off is a simple path that the same two steps then leave alone.
	for (int32 TaxiwayId = 0; TaxiwayId < Taxiways.Num(); ++TaxiwayId)
	{
		if (Taxiways[TaxiwayId].bAlive)
		{
			SplitTaxiwayBranches(TaxiwayId, Renames);
			SplitTaxiwayPieces(TaxiwayId, Renames);
		}
	}

	// 4. EMPTY - after the splits, so a taxiway emptied by step 1's inheritance is seen too.
	Changes += RetireEmptyTaxiways();
```
Append the three members:
```cpp
void URoadNetwork::SplitTaxiwayBranches(int32 TaxiwayId, TArray<FTaxiwayRename>& OutRenames)
{
	// EACH ROUND takes one arm off one node, so it ends within the taxiway's segment count; the guard only says so.
	for (int32 Guard = 0; Guard <= Segments.Num(); ++Guard)
	{
		FRoadNodeId At;
		TArray<FRoadSegmentId> Arms;
		for (int32 Index = 0; Index < Nodes.Num() && !At.IsSet(); ++Index)
		{
			const FRoadNodeId Node = NodeIdAt(Index);
			TArray<FRoadSegmentId> Own = Node.IsSet() ? TaxiwayNamesOwnAt(*this, Node, TaxiwayId) : TArray<FRoadSegmentId>();
			if (Own.Num() >= 3)
			{
				At = Node;
				Arms = MoveTemp(Own);
			}
		}
		if (!At.IsSet())
		{
			return;
		}
		TArray<FRoadSegmentId> Shortest;
		double ShortestLength = TNumericLimits<double>::Max();
		int32 ShortestFirst = MAX_int32;
		for (const FRoadSegmentId& Arm : Arms)
		{
			// THE BRANCH: from At along Arm, through every node that is a plain pass-through of this taxiway, stopping at
			// a fork, an end, or back at At (a loop's two arms walk the same loop).
			TArray<FRoadSegmentId> Branch;
			double Length = 0.0;
			FRoadSegmentId Through = Arm;
			FRoadNodeId Node = At;
			while (Through.IsSet() && !Branch.Contains(Through))
			{
				Branch.Add(Through);
				Length += TaxiwayNamesLength(*this, Through);
				Node = GetOtherEnd(Through, Node);
				if (Node == At)
				{
					break;
				}
				const TArray<FRoadSegmentId> Own = TaxiwayNamesOwnAt(*this, Node, TaxiwayId);
				Through = Own.Num() == 2 ? (Own[0] == Through ? Own[1] : Own[0]) : FRoadSegmentId();
			}
			if (Length < ShortestLength || (Length == ShortestLength && Arm.Index < ShortestFirst))
			{
				Shortest = MoveTemp(Branch);
				ShortestLength = Length;
				ShortestFirst = Arm.Index;
			}
		}
		const int32 Fresh = MintTaxiway(INDEX_NONE);
		for (const FRoadSegmentId& Segment : Shortest)
		{
			WriteTaxiwayId(Segment, Fresh);
		}
		OutRenames.Add({ TaxiwayDisplayName(Fresh), TaxiwayDisplayName(TaxiwayId) });
	}
}

void URoadNetwork::SplitTaxiwayPieces(int32 TaxiwayId, TArray<FTaxiwayRename>& OutRenames)
{
	TArray<FTaxiwayChain> Pieces;
	TSet<FRoadSegmentId> Seen;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Seed = SegmentIdAt(Index);
		if (!Seed.IsSet() || Seen.Contains(Seed) || TaxiwayOf(Seed) != TaxiwayId)
		{
			continue;
		}
		FTaxiwayChain& Piece = Pieces.AddDefaulted_GetRef();
		TArray<FRoadSegmentId> Frontier = { Seed };
		Seen.Add(Seed);
		while (Frontier.Num() > 0)
		{
			const FRoadSegmentId Each = Frontier.Pop();
			Piece.Segments.Add(Each);
			Piece.Length += TaxiwayNamesLength(*this, Each);
			Piece.LowestIndex = FMath::Min(Piece.LowestIndex, Each.Index);
			const FRoadSegment* Segment = GetSegment(Each);
			for (const FRoadNodeId& End : { Segment->A, Segment->B })
			{
				for (const FRoadSegmentId& Next : TaxiwayNamesOwnAt(*this, End, TaxiwayId))
				{
					if (!Seen.Contains(Next))
					{
						Seen.Add(Next);
						Frontier.Add(Next);
					}
				}
			}
		}
	}
	if (Pieces.Num() <= 1)
	{
		return;
	}
	Pieces.Sort([](const FTaxiwayChain& L, const FTaxiwayChain& R)
	{
		return L.Length != R.Length ? L.Length > R.Length : L.LowestIndex < R.LowestIndex;
	});
	for (int32 Piece = 1; Piece < Pieces.Num(); ++Piece)
	{
		const int32 Fresh = MintTaxiway(INDEX_NONE);
		for (const FRoadSegmentId& Segment : Pieces[Piece].Segments)
		{
			WriteTaxiwayId(Segment, Fresh);
		}
		OutRenames.Add({ TaxiwayDisplayName(Fresh), TaxiwayDisplayName(TaxiwayId) });
	}
}

int32 URoadNetwork::RetireEmptyTaxiways()
{
	TArray<int32> Held;
	Held.Init(0, Taxiways.Num());
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const int32 Owner = TaxiwayOf(SegmentIdAt(Index));
		if (Owner != INDEX_NONE)
		{
			++Held[Owner];
		}
	}
	// UNTIL NOTHING MOVES: retiring the last connector of an empty parent frees the parent in the next round.
	int32 Retired = 0;
	for (bool bChanged = true; bChanged;)
	{
		bChanged = false;
		for (FTaxiway& Each : Taxiways)
		{
			if (!Each.bAlive || Held[Each.Id] > 0 || HasTaxiwayConnectors(Each.Id))
			{
				continue;
			}
			UE_LOG(LogAirside, Log, TEXT("TaxiwayNames: %s retired - no segment and no connector left"),
				*TaxiwayDisplayName(Each.Id));
			Each.bAlive = false;
			++Retired;
			bChanged = true;
		}
	}
	return Retired;
}
```

- [ ] **Step 4: Build, run**

`-Filter Airside.Model.TaxiwayNames`. Expected: `12 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 5: Commit**
```powershell
git add Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h Plugins/Airside/Source/Airside/Private/Model/RoadNetworkTaxiways.cpp `
  Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp
git commit -m "model: taxiway single-chain invariant - branch, pieces, empty, reservation" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

---

### Task 5: The invariant under random edits

**Files:**
- Test: `Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp`
- Modify (only if the test finds a defect): `Plugins/Airside/Source/Airside/Private/Model/RoadNetworkTaxiways.cpp`

**Interfaces:**
- Consumes: `URoadNetwork::AddStraightSegment`, `SplitSegment`, `RemoveSegment`, `MergeNodes`, `RemoveNode`, `SegmentEnds`, `NodeIdAt`, `SegmentIdAt`, `NormaliseTaxiways`, `GetTaxiways`, `TaxiwayOf`, `TaxiwayDisplayName`, `HasTaxiwayConnectors`; `TaxiwayStrip::HasStrip`.
- Produces: `TaxiwayNamesTest::InvariantViolation(const URoadNetwork&) -> FString` (empty when the contract holds).

- [ ] **Step 1: Write the test** (append; add `#include "Math/RandomStream.h"` at the top)

```cpp
namespace TaxiwayNamesTest
{
	/** THE CONTRACT, measured (spec "Invariant test"): every live taxiway is ONE connected chain with at most 2 of its
	 *  segments at any node, display names are unique and non-empty, an empty one is kept only for its connectors, every
	 *  taxiway segment is named and nothing else is. Empty string when it holds; the first breach otherwise. */
	FString InvariantViolation(const URoadNetwork& Net)
	{
		TMap<FString, int32> Shown;
		for (const FTaxiway& T : Net.GetTaxiways())
		{
			if (!T.bAlive) { continue; }
			const FString Name = Net.TaxiwayDisplayName(T.Id);
			if (Name.IsEmpty()) { return FString::Printf(TEXT("taxiway %d has no display name"), T.Id); }
			if (const int32* Other = Shown.Find(Name)) { return FString::Printf(TEXT("%s is shown by %d and %d"), *Name, *Other, T.Id); }
			Shown.Add(Name, T.Id);
			TArray<FRoadSegmentId> Own;
			for (int32 Index = 0; Index < Net.GetSegments().Num(); ++Index)
			{
				const FRoadSegmentId Id = Net.SegmentIdAt(Index);
				if (Id.IsSet() && Net.TaxiwayOf(Id) == T.Id) { Own.Add(Id); }
			}
			if (Own.Num() == 0)
			{
				if (!Net.HasTaxiwayConnectors(T.Id)) { return FString::Printf(TEXT("%s is empty with no connector, still alive"), *Name); }
				continue;
			}
			TMap<int32, int32> PerNode;
			for (const FRoadSegmentId& Id : Own)
			{
				const FRoadSegment* S = Net.GetSegment(Id);
				for (const int32 Node : { S->A.Index, S->B.Index })
				{
					if (++PerNode.FindOrAdd(Node) > 2) { return FString::Printf(TEXT("%s has 3+ segments at node %d"), *Name, Node); }
				}
			}
			TSet<FRoadSegmentId> Reached = { Own[0] };
			for (bool bGrew = true; bGrew;)
			{
				bGrew = false;
				for (const FRoadSegmentId& Id : Own)
				{
					if (Reached.Contains(Id)) { continue; }
					const FRoadSegment* S = Net.GetSegment(Id);
					for (const FRoadSegmentId& R : Reached)
					{
						const FRoadSegment* Q = Net.GetSegment(R);
						if (S->A == Q->A || S->A == Q->B || S->B == Q->A || S->B == Q->B) { Reached.Add(Id); bGrew = true; break; }
					}
				}
			}
			if (Reached.Num() != Own.Num()) { return FString::Printf(TEXT("%s is in pieces (%d of %d reached)"), *Name, Reached.Num(), Own.Num()); }
		}
		for (int32 Index = 0; Index < Net.GetSegments().Num(); ++Index)
		{
			const FRoadSegmentId Id = Net.SegmentIdAt(Index);
			if (!Id.IsSet()) { continue; }
			const bool bTaxiway = TaxiwayStrip::HasStrip(Net, Id);
			if (bTaxiway && Net.TaxiwayOf(Id) == INDEX_NONE) { return FString::Printf(TEXT("taxiway segment %d is unnamed"), Index); }
			if (!bTaxiway && Net.GetSegment(Id)->TaxiwayId != INDEX_NONE) { return FString::Printf(TEXT("segment %d is no taxiway but is named"), Index); }
		}
		return FString();
	}

	/** One seeded run: 300 random raw edits, normalised after each. Fills the per-slot names for the determinism check. */
	FString RandomEditRun(int32 Seed, TArray<FString>& OutNames)
	{
		FTaxiwayNamesNet N;
		FRandomStream Stream(Seed);
		for (int32 X = 0; X < 6; ++X)
		{
			for (int32 Y = 0; Y < 6; ++Y) { N.Node(X * 20000.0, Y * 20000.0); }
		}
		const auto AnyNode = [&N, &Stream]() { return N.Net->NodeIdAt(Stream.RandRange(0, N.Net->GetNodes().Num() - 1)); };
		const auto AnySegment = [&N, &Stream]()
		{
			const int32 Count = N.Net->GetSegments().Num();
			return Count == 0 ? FRoadSegmentId() : N.Net->SegmentIdAt(Stream.RandRange(0, Count - 1));
		};
		for (int32 Step = 0; Step < 300; ++Step)
		{
			const int32 Op = Stream.RandRange(0, 9);
			if (Op <= 4)
			{
				URoadProfile* Profile = Op == 4 ? N.Road : (Op == 3 && Stream.FRand() < 0.3f ? N.Runway : N.Taxi);
				const FRoadNodeId A = AnyNode();
				const FRoadNodeId B = AnyNode();
				if (A.IsSet() && B.IsSet() && A != B) { N.Lay(A, B, Profile); }
			}
			else if (Op == 5)
			{
				FVector2D A, B;
				const FRoadSegmentId S = AnySegment();
				if (S.IsSet() && N.Net->SegmentEnds(S, A, B)) { N.Net->SplitSegment(S, (A + B) * 0.5); }
			}
			else if (Op <= 7)
			{
				const FRoadSegmentId S = AnySegment();
				if (S.IsSet()) { N.Net->RemoveSegment(S); }
			}
			else if (Op == 8)
			{
				const FRoadNodeId A = AnyNode();
				const FRoadNodeId B = AnyNode();
				if (A.IsSet() && B.IsSet()) { N.Net->MergeNodes(A, B); }
			}
			else
			{
				const FRoadNodeId A = AnyNode();
				if (A.IsSet()) { N.Net->RemoveNode(A); }
			}
			N.Normalise();
			const FString Violation = InvariantViolation(*N.Net);
			if (!Violation.IsEmpty()) { return FString::Printf(TEXT("seed %d step %d op %d: %s"), Seed, Step, Op, *Violation); }
		}
		for (int32 Index = 0; Index < N.Net->GetSegments().Num(); ++Index)
		{
			const FRoadSegmentId Id = N.Net->SegmentIdAt(Index);
			OutNames.Add(Id.IsSet() ? N.NameOf(Id) : FString(TEXT("-")));
		}
		return FString();
	}
}

/** THE INVARIANT TEST (spec): a seeded random edit sequence, the contract measured after every mutator - and the same
 *  seed twice gives the same names (spec: "same network in, same names out"). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesInvariantTest, "Airside.Model.TaxiwayNames.InvariantUnderRandomEdits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesInvariantTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	{
		// CONTROL: the checker sees a breach - a taxiway left unnamed.
		FTaxiwayNamesNet N;
		N.Lay(N.Node(0.0, 0.0), N.Node(50000.0, 0.0));
		TestFalse(TEXT("control: an unnormalised taxiway is a breach the checker reports"), InvariantViolation(*N.Net).IsEmpty());
	}
	for (int32 Seed = 1; Seed <= 6; ++Seed)
	{
		TArray<FString> First;
		TArray<FString> Second;
		const FString Violation = RandomEditRun(Seed, First);
		if (!TestTrue(FString::Printf(TEXT("the contract holds after every edit (%s)"), *Violation), Violation.IsEmpty())) { return false; }
		RandomEditRun(Seed, Second);
		TestEqual(FString::Printf(TEXT("seed %d: same edits, same names"), Seed), First, Second);
	}
	return true;
}
```

- [ ] **Step 2: Build, run**

`-Filter Airside.Model.TaxiwayNames.InvariantUnderRandomEdits`. Expected: PASS. If it FAILS, the message names seed, step, op and the breach: write that sequence as its own deterministic test in this file first (rule: a test that pins the bug), then fix `RoadNetworkTaxiways.cpp` until both pass. Do not loosen `InvariantViolation`.

- [ ] **Step 3: Mutation check (a green test may measure nothing)**

Temporarily comment out the `SplitTaxiwayPieces(TaxiwayId, Renames);` line in `NormaliseTaxiways`, build, run the filter: expected FAIL with "is in pieces". Restore the line, build, run: PASS.

- [ ] **Step 4: Commit**
```powershell
git add Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp Plugins/Airside/Source/Airside/Private/Model/RoadNetworkTaxiways.cpp
git commit -m "test: taxiway name invariant under seeded random edits" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

---

### Task 6: Backfill on load - both load paths

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h` (public block)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetworkTaxiways.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h:611` (new UPROPERTY after `GuideSources`)
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp:620-652` (RepairLoadedNetwork)
- Test: `Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp` (model) and create `Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesPresentTest.cpp` (actor)

**Interfaces:**
- Consumes: `NormaliseTaxiways`; `ARoadNetworkActor::RepairLoadedNetwork(ELoadedFrom)` (public, RoadNetworkActor.h:1340), `ELoadedFrom::Level/SaveGame`, `ARoadNetworkActor::ResolveProfile()`; `FAirsideTestWorld` (Testing/AirsideTestWorld.h:56); `FLogLineSpy` (AirsideTestWorld.h:142).
- Produces: `int32 URoadNetwork::EnsureTaxiwayNames(const FTaxiwayNamingRules& Rules)`; `UPROPERTY(EditAnywhere) FTaxiwayNamingRules ARoadNetworkActor::TaxiwayNaming`.

- [ ] **Step 1: Write the failing tests**

TaxiwayNamesTest.cpp (append):
```cpp
namespace TaxiwayNamesTest
{
	/** A GATWICK-SHAPED airport (M_ScaleGatwick's pattern, 2026-10-02): a runway, two 3 km parallels A (y -200 m) and B
	 *  (y -400 m), three 200 m exits runway->A, two 200 m end links A->B round 90 degree corners, two 150 m stand stubs
	 *  off B ending at stands. Laid in that order, so segment indices are fixed. */
	struct FGatwickShape
	{
		FTaxiwayNamesNet N;
		TArray<FRoadSegmentId> ASegs, BSegs, Exits, EndLinks, Stubs;

		FGatwickShape()
		{
			TArray<FRoadNodeId> R, A, B;
			for (const double X : { 0.0, 60000.0, 150000.0, 240000.0, 300000.0 }) { R.Add(N.Node(X, 0.0)); }
			for (int32 I = 0; I + 1 < R.Num(); ++I) { N.Lay(R[I], R[I + 1], N.Runway); }
			for (const double X : { 0.0, 60000.0, 100000.0, 150000.0, 200000.0, 240000.0, 300000.0 }) { A.Add(N.Node(X, -20000.0)); }
			for (int32 I = 0; I + 1 < A.Num(); ++I) { ASegs.Add(N.Lay(A[I], A[I + 1])); }
			for (const double X : { 0.0, 50000.0, 100000.0, 150000.0, 200000.0, 250000.0, 300000.0 }) { B.Add(N.Node(X, -40000.0)); }
			for (int32 I = 0; I + 1 < B.Num(); ++I) { BSegs.Add(N.Lay(B[I], B[I + 1])); }
			Exits = { N.Lay(R[1], A[1]), N.Lay(R[2], A[3]), N.Lay(R[3], A[5]) };
			EndLinks = { N.Lay(A[0], B[0]), N.Lay(A[6], B[6]) };
			Stubs = { N.Lay(B[1], N.Node(50000.0, -55000.0)), N.Lay(B[2], N.Node(100000.0, -55000.0)) };
		}
	};
}

/** Backfill (spec): unnamed chains, longest first - the parallels take A and B, the links and stubs are connectors;
 *  logged once; a second load names nothing; and it is deterministic. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesBackfillTest, "Airside.Model.TaxiwayNames.BackfillGatwickShape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesBackfillTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FGatwickShape G;
	FLogLineSpy Spy(TEXT("LogAirside"));
	GLog->AddOutputDevice(&Spy);
	const int32 Named = G.N.Net->EnsureTaxiwayNames(G.N.Rules);
	GLog->RemoveOutputDevice(&Spy);
	TestEqual(TEXT("two letters and seven connectors"), Named, 9);
	TestTrue(TEXT("logged as the spec words it"), Spy.CapturedLines.ContainsByPredicate([](const FString& L)
		{ return L.Contains(TEXT("TaxiwayNames: backfilled 2 taxiway(s), 7 connector(s)")); }));
	for (const FRoadSegmentId& S : G.ASegs) { TestEqual(TEXT("the first parallel is A"), G.N.NameOf(S), FString(TEXT("A"))); }
	for (const FRoadSegmentId& S : G.BSegs) { TestEqual(TEXT("the second parallel is B"), G.N.NameOf(S), FString(TEXT("B"))); }
	const TArray<FString> Expected = { TEXT("A1"), TEXT("A2"), TEXT("A3"), TEXT("A4"), TEXT("A5"), TEXT("B1"), TEXT("B2") };
	TArray<FString> Got;
	for (const FRoadSegmentId& S : G.Exits) { Got.Add(G.N.NameOf(S)); }
	for (const FRoadSegmentId& S : G.EndLinks) { Got.Add(G.N.NameOf(S)); }
	for (const FRoadSegmentId& S : G.Stubs) { Got.Add(G.N.NameOf(S)); }
	TestEqual(TEXT("exits A1-A3, end links A4-A5, stand stubs B1-B2"), Got, Expected);
	TestEqual(TEXT("a second load names nothing"), G.N.Net->EnsureTaxiwayNames(G.N.Rules), 0);

	FGatwickShape Again;
	Again.N.Net->EnsureTaxiwayNames(Again.N.Rules);
	FRoadNetworkTestAccess(*G.N.Net).ClearTaxiwayNamesForTest();
	G.N.Net->EnsureTaxiwayNames(G.N.Rules);
	for (int32 Index = 0; Index < G.N.Net->GetSegments().Num(); ++Index)
	{
		const FRoadSegmentId Id = G.N.Net->SegmentIdAt(Index);
		TestEqual(FString::Printf(TEXT("segment %d: same network, same name"), Index), G.N.NameOf(Id), Again.N.NameOf(Again.N.Net->SegmentIdAt(Index)));
	}
	return true;
}
```
Add `#include "Testing/AirsideTestWorld.h"` to the file's includes (for `FLogLineSpy`).

Create `TaxiwayNamesPresentTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Model/TaxiwayStrip.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * TAXIWAY NAMES AT THE COMPOSITION (spec "Seam tests"): through ARoadNetworkActor and its facade, so each test goes red
 * if its seam - the load repair, the facade's notify - stops calling the model.
 */

/**
 * REVIEW FOCUS 4: a network saved before names, with roads that lost their own profile to a save (Profile null -
 * URoadNetwork::DefaultProfile's own comment), is named on BOTH load paths: a level's and a save game's (Serialize alone).
 * Plan D6: only after RepairLoadedNetwork re-resolves the default can those roads be read as taxiways at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesLoadTest, "Airside.Present.TaxiwayNames.LoadBackfillsBothPaths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesLoadTest::RunTest(const FString&)
{
	for (const ELoadedFrom From : { ELoadedFrom::Level, ELoadedFrom::SaveGame })
	{
		FAirsideTestWorld TestWorld;
		ARoadNetworkActor* Actor = TestWorld.Actor;
		if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
		URoadNetwork* Net = Actor->Network;
		const FRoadSegmentId Lost = Net->AddStraightSegment(Net->AddNode({ 0.0, 0.0 }), Net->AddNode({ 50000.0, 0.0 }), nullptr);
		Net->DefaultProfile = nullptr;
		TestFalse(TEXT("control: with no default it reads as no taxiway, so naming before the repair would skip it"),
			TaxiwayStrip::HasStrip(*Net, Lost));
		Actor->RepairLoadedNetwork(From);
		TestEqual(FString::Printf(TEXT("load path %d: the profile-less road is taxiway A"), static_cast<int32>(From)),
			Net->TaxiwayDisplayName(Net->TaxiwayOf(Lost)), FString(TEXT("A")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Build - expect a compile failure**

Expected: `'EnsureTaxiwayNames': is not a member of 'URoadNetwork'`.

- [ ] **Step 3: Implement**

RoadNetwork.h public block (after `JunctionName`):
```cpp

	/**
	 * Name every unnamed taxiway - a level or save written before 2026-10-02 (every map then, M_ScaleGatwick included).
	 * NormaliseTaxiways itself, whose step 1 IS the backfill (longest chain first, plan D4), plus the spec's one line:
	 * "TaxiwayNames: backfilled N taxiway(s), M connector(s)". Returns N + M; 0, silent, when nothing was unnamed, so a
	 * second load is a no-op. Called from ARoadNetworkActor::RepairLoadedNetwork on BOTH load paths, after the default
	 * profile is re-resolved - NOT from PostLoad, where a road saved without its own profile cannot yet be told from a
	 * service road (plan D6).
	 * ENFORCED BY: Airside.Model.TaxiwayNames.BackfillGatwickShape, Airside.Present.TaxiwayNames.LoadBackfillsBothPaths
	 */
	int32 EnsureTaxiwayNames(const FTaxiwayNamingRules& Rules);
```
RoadNetworkTaxiways.cpp (append):
```cpp
int32 URoadNetwork::EnsureTaxiwayNames(const FTaxiwayNamingRules& Rules)
{
	int32 Unnamed = 0;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegmentId Id = SegmentIdAt(Index);
		Unnamed += Id.IsSet() && TaxiwayStrip::HasStrip(*this, Id) && TaxiwayOf(Id) == INDEX_NONE ? 1 : 0;
	}
	if (Unnamed == 0)
	{
		return 0;
	}
	const int32 Before = Taxiways.Num();
	NormaliseTaxiways(Rules);
	int32 Lettered = 0;
	int32 Connectors = 0;
	for (int32 Index = Before; Index < Taxiways.Num(); ++Index)
	{
		if (Taxiways[Index].bAlive)
		{
			(Taxiways[Index].IsConnector() ? Connectors : Lettered) += 1;
		}
	}
	UE_LOG(LogAirside, Log, TEXT("TaxiwayNames: backfilled %d taxiway(s), %d connector(s)"), Lettered, Connectors);
	return Lettered + Connectors;
}
```
RoadNetworkActor.h, after line 611 (`FSnapGuideSettings GuideSources;`):
```cpp

	/**
	 * The taxiway-naming knobs (spec 2026-10-02): how short a connector is, how far a bend may turn, how far apart a
	 * long taxiway's name repeats. Read by URoadEditFacade::NotifyChanged's normalise, RepairLoadedNetwork's backfill and
	 * both drivers' labels - one struct, so the three cannot disagree about the same number.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Taxiway names")
	FTaxiwayNamingRules TaxiwayNaming;
```
Add `#include "Model/Taxiway.h"` after `#include "Model/TrafficRules.h"` (line 5).

RoadNetworkActor.cpp RepairLoadedNetwork, after `const int32 Entranced = Network->EnsureStandFrontages();` (line 620):
```cpp
	// AND THE TAXIWAY NAMES (2026-10-02), AFTER THE DEFAULT ABOVE: a road saved without a profile of its own reads as a
	// taxiway only through DefaultProfile (ProfileFor), so naming before it is re-resolved would leave those roads
	// unnamed and name them later in a different order. Both paths, like the migrations above.
	// ENFORCED BY: Airside.Present.TaxiwayNames.LoadBackfillsBothPaths
	const int32 Named = Network->EnsureTaxiwayNames(TaxiwayNaming);
```
Change the total and its log line (lines 646-651) to:
```cpp
	const int32 Total = DefaultsResolved + Outlined + Numbered + Fronted + Entranced + Named + Rebound + RefreshedAnchors + Forgotten;
	if (Total > 0)
	{
		UE_LOG(LogRoadMesh, Log,
			TEXT("Load repairs on %s: %d default re-resolved, %d outline(s), %d number(s), %d depot frontage(s), %d stand entrance(s), %d taxiway name(s), %d definition(s), %d anchor(s), %d transient profile ref(s)"),
			*GetName(), DefaultsResolved, Outlined, Numbered, Fronted, Entranced, Named, Rebound, RefreshedAnchors, Forgotten);
	}
```

- [ ] **Step 4: Build twice (new test .cpp), run**

`-Filter "Airside.Model.TaxiwayNames+Airside.Present.TaxiwayNames"`. Expected: `14 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 5: Commit**
```powershell
git add Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h Plugins/Airside/Source/Airside/Private/Model/RoadNetworkTaxiways.cpp `
  Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp `
  Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesPresentTest.cpp
git commit -m "load: backfill taxiway names on both load paths, longest chain first" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

---

### Task 7: One door - the facade's notify normalises, announces, and undo carries names

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h:234` (delegate after `OnRefused`), `:612` (private helper beside `NotifyChanged`)
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacade.cpp:196-211` (NotifyChanged)
- Test: `Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesPresentTest.cpp`

**Interfaces:**
- Consumes: `URoadEditFacade::NotifyChanged(EChangeKind)` (the single `OnChanged.Broadcast` site, RoadEditFacade.cpp:210), `Actor()` (RoadEditFacade.h:979), `ARoadNetworkActor::TaxiwayNaming` (Task 6), `URoadEditFacade::Undo()/Redo()` (:550-551), `ARoadNetworkActor::PlaceNode/ConnectNodes/DeleteSegment` (RoadNetworkActor.h:401-538), `GetEditFacade()` (:338).
- Produces: `DECLARE_MULTICAST_DELEGATE_TwoParams(FOnTaxiwaySplit, const FString& /*SplitOff*/, const FString& /*From*/); FOnTaxiwaySplit URoadEditFacade::OnTaxiwaySplit;` private `void URoadEditFacade::NormaliseTaxiwayNames();`

- [ ] **Step 1: Write the failing test** (append to TaxiwayNamesPresentTest.cpp, before `#endif`)

```cpp
/**
 * THE ONE DOOR (plan D9) AND REVIEW FOCUS 3: a taxiway drawn click by click through the actor is ONE name because the
 * facade normalises on every Topology notify; deleting its middle announces "B split off from A" ONCE; undo brings A
 * back whole and says nothing; redo splits it again.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesFacadeTest, "Airside.Present.TaxiwayNames.SplitIsAnnouncedAndUndone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesFacadeTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("its facade"), Facade)) { return false; }
	TArray<FString> Heard;
	const FDelegateHandle Handle = Facade->OnTaxiwaySplit.AddLambda([&Heard](const FString& SplitOff, const FString& From)
		{ Heard.Add(SplitOff + TEXT("<") + From); });

	const int32 N0 = Actor->PlaceNode({ 0.0, 0.0 });
	const int32 N1 = Actor->PlaceNode({ 40000.0, 0.0 });
	const int32 N2 = Actor->PlaceNode({ 50000.0, 0.0 });
	const int32 N3 = Actor->PlaceNode({ 70000.0, 0.0 });
	TestTrue(TEXT("click 1"), Actor->ConnectNodes(N0, N1, ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	const int32 Long = Actor->Network->GetSegments().Num() - 1;
	TestTrue(TEXT("click 2"), Actor->ConnectNodes(N1, N2, ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	const int32 Middle = Actor->Network->GetSegments().Num() - 1;
	TestTrue(TEXT("click 3"), Actor->ConnectNodes(N2, N3, ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	const int32 Short = Actor->Network->GetSegments().Num() - 1;
	const auto NameAt = [Actor](int32 Index) { return Actor->Network->TaxiwayDisplayName(Actor->Network->TaxiwayOf(Actor->Network->SegmentIdAt(Index))); };
	TestEqual(TEXT("three clicks through the actor are one taxiway, named with no explicit call"), NameAt(Short), FString(TEXT("A")));

	TestTrue(TEXT("the middle is deleted"), Actor->DeleteSegment(Middle));
	TestEqual(TEXT("the split is announced once"), Heard, TArray<FString>{ TEXT("B<A") });
	TestEqual(TEXT("the long piece keeps A"), NameAt(Long), FString(TEXT("A")));
	TestEqual(TEXT("the short piece is B"), NameAt(Short), FString(TEXT("B")));

	TestTrue(TEXT("undo"), Facade->Undo());
	TestEqual(TEXT("undo brings A back whole"), NameAt(Short), FString(TEXT("A")));
	TestEqual(TEXT("and announces nothing - the Memento carried the names"), Heard.Num(), 1);
	int32 Alive = 0;
	for (const FTaxiway& T : Actor->Network->GetTaxiways()) { Alive += T.bAlive ? 1 : 0; }
	TestEqual(TEXT("one live taxiway after the undo"), Alive, 1);

	TestTrue(TEXT("redo"), Facade->Redo());
	TestEqual(TEXT("redo splits again"), NameAt(Short), FString(TEXT("B")));
	Facade->OnTaxiwaySplit.Remove(Handle);
	return true;
}
```

- [ ] **Step 2: Build - expect a compile failure**

Expected: `'OnTaxiwaySplit': is not a member of 'URoadEditFacade'`.

- [ ] **Step 3: Implement**

RoadEditFacade.h, after line 235 (`FOnBuildRefused OnRefused;`):
```cpp

	/**
	 * "C split off from A" - one broadcast per split NotifyChanged's normalise made (URoadNetwork::NormaliseTaxiways,
	 * spec "Each rename emits ONE event the game turns into a toast"). NATIVE, OnRefused's reason; the layer with a UI
	 * bridges it (UOpsRuntime's "TaxiwaySplit" bridge) - Airside never names that layer (rule 1b). The editor mode has no
	 * such layer: the model's own log line is the record there.
	 * ENFORCED BY: Airside.Present.TaxiwayNames.SplitIsAnnouncedAndUndone, AirportOps.Present.Bus.ReattachDoesNotDouble
	 */
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnTaxiwaySplit, const FString& /*SplitOff*/, const FString& /*From*/);
	FOnTaxiwaySplit OnTaxiwaySplit;
```
RoadEditFacade.h, after `void NotifyChanged(EChangeKind Kind = EChangeKind::Topology);` (line 612) - the private section that holds it:
```cpp

	/** NotifyChanged's naming half - see the call there. */
	void NormaliseTaxiwayNames();
```
RoadEditFacade.cpp NotifyChanged - replace the final `OnChanged.Broadcast(Kind);` (line 210) with:
```cpp
	// THE NAMES BEFORE THE BROADCAST (spec: normalise "after EVERY topology change, in one place"; plan D9): this is the
	// one door every mutator's notify passes - draw, delete, merge, insert, a heal, undo and redo - and a listener that
	// rebuilds labels or a card must read names that are already true. Topology only: a drag frame (Geometry), a facts
	// edit or a marking changes no chain. After the batch fold above, so a bulk edit normalises once.
	// ENFORCED BY: Airside.Present.TaxiwayNames.SplitIsAnnouncedAndUndone
	if (Kind == EChangeKind::Topology)
	{
		NormaliseTaxiwayNames();
	}
	OnChanged.Broadcast(Kind);
```
Append to RoadEditFacade.cpp:
```cpp
void URoadEditFacade::NormaliseTaxiwayNames()
{
	URoadNetwork* Network = Actor().Network;
	if (Network == nullptr)
	{
		return;
	}
	// OUTSIDE ANY EDIT SCOPE, and that is right: the scope that made this change has already committed its BEFORE
	// snapshot, so the names written here are part of the state the next edit snapshots, and an undo restores the
	// previous state's names whole (URoadNetwork::CopyFrom carries Taxiways).
	for (const FTaxiwayRename& Rename : Network->NormaliseTaxiways(Actor().TaxiwayNaming))
	{
		OnTaxiwaySplit.Broadcast(Rename.SplitOff, Rename.From);
	}
}
```

- [ ] **Step 4: Build, run**

`-Filter Airside.Present.TaxiwayNames`. Expected: `2 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 5: Run the identity tests the new notify touches**

`-Filter "Airside.Present+Airside.Model.RoadEditHistory+Airside.Tool.RoadDrawTool+Airside.Model.HoldingPositionMark+Airside.Build.MeshFreshness+Airside.Tool.TaxiwayWidth"`. These compare whole networks through `NetworkIdentity::DifferingProperties` (NetworkIdentity.h). Expected: 0 failed. IF one fails naming `Segments` or `Taxiways`: its fixture built a network with raw mutators (unnamed) and then edited through the facade (normalised) - a state no player reaches. Fix the FIXTURE: call `Network->NormaliseTaxiways(Actor->TaxiwayNaming)` right after its raw build, with a one-line comment saying why. Never add `Taxiways` to `NetworkIdentity::RederivedOnNotify`: names are state, not derived.

- [ ] **Step 6: Commit**
```powershell
git add Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h Plugins/Airside/Source/Airside/Private/Present/RoadEditFacade.cpp `
  Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesPresentTest.cpp
git commit -m "facade: normalise taxiway names on every topology notify; announce splits" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

---

### Task 8: Lint - TaxiwayId has one writer (rule 103)

**Files:**
- Modify: `Tools/Check-Architecture.ps1:1323` (new row at the end of `$AllowedCallers`, after rule 83's row, before the closing `)`)

**Interfaces:**
- Consumes: rule 4's row format (`Name`, `Pattern`, `ProdAllowed`, `TestExempt`, `ProdReason`; matched against comment-stripped code, path-suffix allow-list). 102 is the highest rule number on 2026-10-02, so this is 103.
- Produces: Check-Architecture rule 103.

- [ ] **Step 1: Write the rule**

Insert after the `'runway point spelled by hand (rule 83)'` row's closing `}` (add a comma after that `}`):
```powershell
    @{
        # RULE 103 (taxiway naming, spec 2026-10-02): A SEGMENT'S TAXIWAY IS WRITTEN IN ONE FILE. FRoadSegment::TaxiwayId is
        # named by URoadNetwork::NormaliseTaxiways / EnsureTaxiwayNames (and copied to both halves by SplitSegment) through
        # their one writer, WriteTaxiwayId, in RoadNetworkTaxiways.cpp. A write anywhere else is a second naming the
        # single-chain invariant never sees - the "fix at a site" #255 says regresses. FILE-GRANULAR (rule 4 scopes by
        # file), which is why the writers live in a file of their own. A member write only (`.TaxiwayId =`, `->TaxiwayId =`):
        # a local of that name is not a segment's field. DOES NOT SEE a write through a memcpy or a reflection walk.
        Name        = 'FRoadSegment::TaxiwayId write (rule 103)'
        Pattern     = '(?:\.|->)\s*TaxiwayId\s*=(?!=)'
        ProdAllowed = @('Private\Model\RoadNetworkTaxiways.cpp')
        TestExempt  = $true
        ProdReason  = 'name a segment through URoadNetwork::NormaliseTaxiways / EnsureTaxiwayNames (their one writer, WriteTaxiwayId, is in RoadNetworkTaxiways.cpp) - a second writer is a second naming the invariant never sees (spec 2026-10-02-taxiway-naming)'
    }
```

- [ ] **Step 2: Mutation check - the rule must fail on the shape it bans**

Temporarily add `Segments[0].TaxiwayId = 0;` as the first line of `URoadNetwork::CopyFrom`'s body in RoadNetwork.cpp. Run `./Tools/Check-Architecture.ps1`. Expected: a failure line `allowed-callers: ...RoadNetwork.cpp:<n> FRoadSegment::TaxiwayId write (rule 103) - name a segment through ...`. Remove the line.

- [ ] **Step 3: Run clean**

`./Tools/Check-Architecture.ps1`. Expected: the PASS banner (no failures). Rule 12 may WARN on new claims without markers: every claim this PR added carries `ENFORCED BY:`; fix any it names.

- [ ] **Step 4: Commit**
```powershell
git add Tools/Check-Architecture.ps1
git commit -m "lint: rule 103 - FRoadSegment::TaxiwayId written only in RoadNetworkTaxiways.cpp" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

---

### Task 9: First consumers - "On: A3" and "deadlocked on A3"

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/InspectFacts.h:182` (FAgentFacts field, after `DeadlockedWith`), `:239` (WhereIs declaration after `StatusOf`)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/InspectFacts.cpp:184-230` (DescribeAgent), append WhereIs
- Modify: `Plugins/Airside/Source/Airside/Private/Model/GroundTrafficDeadlock.cpp:630-635` (the "no member can turn" Warning)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/OpsAlerts.cpp:236-258` (deadlock text)
- Modify: `Source/AirportMgr/InspectorCards.h:360` (FAircraftDisplay field), `Source/AirportMgr/InspectorAircraftCard.cpp:110` (DisplayOf), `:176` (Compose)
- Test: `Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp`; `Plugins/AirportOps/Source/AirportOpsTests/Private/OpsAlertsTest.cpp`; `Source/AirportMgr/InspectorCardsTest.cpp`

**Interfaces:**
- Consumes: `FRoadAgent::PlanInProgress()`, `FRoadAgent::DistanceAlongPlan()` (RoadAgent.h:695-716), `UGroundTraffic::CurrentStep(const FRoutePlan&, double)` (public static, GroundTraffic.h:1351), `FRouteStep::Edge`, `FGuidelineEdge::DerivedFrom` / `AtJunction` (RoadGuideline.h:230, :262), `InspectFacts::DescribeRunway`, `URoadNetwork::TaxiwayOf/TaxiwayDisplayName/JunctionName`.
- Produces: `AIRSIDE_API FString InspectFacts::WhereIs(const FRoadAgent& Agent, const URoadNetwork& Network);`, `FString FAgentFacts::On;`, `FString FAircraftDisplay::On;`.

- [ ] **Step 1: Write the failing tests**

TaxiwayNamesTest.cpp - add includes `#include "Content/AirsideSettings.h"`, `#include "Model/GroundTraffic.h"`, `#include "Model/InspectFacts.h"`, `#include "Model/RouteSearch.h"`; append:
```cpp
/** Where an agent is, in names (spec "First consumers"): a taxiing aircraft on FTestAirport's one taxiway is on A - and
 *  before the names exist it is nowhere, which is what proves the answer is read from them (plan D13). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesWhereIsTest, "Airside.Model.TaxiwayNames.WhereIsAnAgent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesWhereIsTest::RunTest(const FString&)
{
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	FTestAirport Airport = FTestAirport::Build(Airframe);
	const FGuidelineNodeId Exit = RouteSearch::FindNearestNode(*Airport.Net, Airport.ExitAt, ETraversalClass::Aircraft, 200.0);
	const FRoutePlan Plan = TestGraph::Probe(*Airport.Net, Exit, Airport.Pose(Airport.Stands[0]), ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("setup: a taxi route"), Plan.IsValid())) { return false; }
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = Traffic->DispatchAgent(Airport.Net, Plan, Airframe, ETraversalClass::Aircraft, 0.0);
	if (!TestNotNull(TEXT("setup: dispatched"), Traffic->FindAgent(Plane))) { return false; }
	// OFF THE RUNWAY FIRST: the route starts on the runway's centreline node (FTestAirport), where the answer is the pair.
	const auto OnRunway = [&]()
	{
		const FRoadAgent* Moving = Traffic->FindAgent(Plane);
		const FRoutePlan& Route = Moving->PlanInProgress();
		const FGuidelineEdge* Edge = Airport.Net->GetGuidelineEdge(
			Route.Steps[UGroundTraffic::CurrentStep(Route, Moving->DistanceAlongPlan())].Edge);
		return Edge != nullptr && Airport.Net->IsRunwaySegment(Edge->DerivedFrom);
	};
	for (int32 Tick = 0; Tick < 4000 && OnRunway(); ++Tick) { Traffic->Advance(0.05, Airport.Net); }
	const FRoadAgent* Agent = Traffic->FindAgent(Plane);
	if (!TestTrue(TEXT("setup: off the runway"), Agent != nullptr && !OnRunway())) { return false; }
	TestEqual(TEXT("control: before any name exists it is nowhere"), InspectFacts::WhereIs(*Agent, *Airport.Net), FString());
	Airport.Net->NormaliseTaxiways(FTaxiwayNamingRules());
	const FString Where = InspectFacts::WhereIs(*Agent, *Airport.Net);
	TestTrue(FString::Printf(TEXT("on the fixture's one taxiway, A, or its junction ('%s')"), *Where), Where == TEXT("A"));
	FAgentFacts Facts;
	if (TestTrue(TEXT("described"), InspectFacts::DescribeAgent(*Traffic, Airport.Net, Plane, Facts)))
	{
		TestEqual(TEXT("the card's On is the same answer"), Facts.On, Where);
	}
	return true;
}
```
OpsAlertsTest.cpp - append before its `#endif`:
```cpp
// THE DEADLOCK SAYS WHERE (taxiway naming spec 2026-10-02): on a 40-stand airport "2 aircraft deadlocked" cannot be found.
// Two aircraft on the airport's taxiway wait on each other; once the network is named, the alert names the taxiway the
// lowest member is on - and before, it says what it always said.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsDeadlockWhereTest, "AirportOps.Model.Alerts.DeadlockSaysWhere",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsDeadlockWhereTest::RunTest(const FString&)
{
	FAlertsField F;
	if (!TestTrue(TEXT("an aircraft taxiing in"), F.Build())) { return false; }
	// THE LOWEST MEMBER (the alert's key and focus) is F.Plane, taxiing in from the exit; it is moved off the runway's
	// centreline node first, where WhereIs would answer the runway's pair instead of the taxiway.
	const auto OnRunway = [&F]()
	{
		const FRoadAgent* Moving = F.Traffic->FindAgent(F.Plane);
		const FRoutePlan& Route = Moving->PlanInProgress();
		const FGuidelineEdge* Edge = F.Airport.Net->GetGuidelineEdge(
			Route.Steps[UGroundTraffic::CurrentStep(Route, Moving->DistanceAlongPlan())].Edge);
		return Edge != nullptr && F.Airport.Net->IsRunwaySegment(Edge->DerivedFrom);
	};
	for (int32 Tick = 0; Tick < 4000 && OnRunway(); ++Tick) { F.Traffic->Advance(0.05, F.Airport.Net); }
	if (!TestFalse(TEXT("setup: the first aircraft is off the runway"), OnRunway())) { return false; }
	// A SECOND AIRCRAFT, from the other stand out to the exit - a higher id, so F.Plane stays the lowest member.
	const FGuidelineNodeId Exit = RouteSearch::FindNearestNode(*F.Airport.Net, F.Airport.ExitAt, ETraversalClass::Aircraft, 200.0);
	const FRoutePlan Out = TestGraph::Probe(*F.Airport.Net, F.Airport.Pose(F.Airport.Stands[1]), Exit, ETraversalClass::Aircraft);
	const int32 Second = Out.IsValid() ? F.Traffic->DispatchAgent(F.Airport.Net, Out, UAirsideSettings::ResolveDefaultAirframe(),
		ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("a second aircraft"), Second > F.Plane)) { return false; }
	FGroundTrafficTestAccess Access(*F.Traffic);
	const FGuidelineNodeId Any = F.Airport.Pose(F.Airport.Stands[0]);
	Access.ScriptWait(F.Plane, FTrafficResource::OfNode(Any), Second, F.Traffic->Rules.StallSeconds * 2.0);
	Access.ScriptWait(Second, FTrafficResource::OfNode(Any), F.Plane, F.Traffic->Rules.StallSeconds * 2.0);
	F.Airport.Net->NormaliseTaxiways(FTaxiwayNamingRules());
	F.Recompute();
	FString Text;
	for (const FOpsAlert& Alert : F.Raised)
	{
		if (Alert.Key.Kind == EAlertKind::Deadlock) { Text = Alert.Text.ToString(); }
	}
	TestTrue(FString::Printf(TEXT("the deadlock says where ('%s')"), *Text), Text.Contains(TEXT("2 aircraft deadlocked on A - ")));
	TestTrue(TEXT("and still gives the remedy"), Text.Contains(UOpsAlerts::DeadlockRemedy().ToString()));
	return true;
}
```
OpsAlertsTest.cpp already includes `Model/RoadNetwork.h` (which brings `FTaxiwayNamingRules` since Task 2), `Model/RouteSearch.h` and `Testing/AirsideTestGraph.h`; add `#include "Content/AirsideSettings.h"` only if absent (it is present, line 2).

InspectorCardsTest.cpp - append before `#endif`:
```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorAircraftOnTest, "AirportMgr.Inspector.Card.AircraftSaysWhereItIs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorAircraftOnTest::RunTest(const FString&)
{
	// "On: A3" (taxiway naming spec, "First consumers"): the line appears when the facts know where, and not otherwise.
	FAgentFacts Facts;
	Facts.On = TEXT("A3");
	FInspectorCardView View;
	FAircraftCard::Compose(FAircraftCard::DisplayOf(Facts, FAircraftNames()), View);
	TestTrue(FString::Printf(TEXT("the card says where ('%s')"), *View.Facts), View.Facts.Contains(TEXT("\nOn: A3")));
	Facts.On.Reset();
	FAircraftCard::Compose(FAircraftCard::DisplayOf(Facts, FAircraftNames()), View);
	TestFalse(TEXT("and says nothing when it does not know"), View.Facts.Contains(TEXT("On:")));
	return true;
}
```

- [ ] **Step 2: Build - expect a compile failure**

Expected: `'WhereIs': is not a member of 'InspectFacts'`, `'On': is not a member of 'FAgentFacts'`.

- [ ] **Step 3: Implement**

InspectFacts.h, FAgentFacts after `TArray<int32> DeadlockedWith;`:
```cpp

	/** Where it is, in names - "A3", "A/B", "09/27" - or empty (InspectFacts::WhereIs). Taxiway naming spec 2026-10-02. */
	FString On;
```
InspectFacts.h, after `StatusOf`'s declaration:
```cpp

	/**
	 * Where Agent is, in the words a controller uses (taxiway naming spec 2026-10-02, plan D13): the edge it is on now
	 * (UGroundTraffic::CurrentStep at DistanceAlongPlan - the map the claim pass uses, so it cannot disagree with where
	 * the follower is) - a lane derived from a runway -> its pair ("09/27", DescribeRunway's); from a taxiway -> its name
	 * (URoadNetwork::TaxiwayDisplayName); a turn path -> its junction's name ("A/B"); anything else - a stand lead-in, a
	 * hand-laid line, an unnamed network - empty. ONE answer for the aircraft card's "On:" and the Deadlock alert.
	 * ENFORCED BY: Airside.Model.TaxiwayNames.WhereIsAnAgent, AirportOps.Model.Alerts.DeadlockSaysWhere
	 */
	AIRSIDE_API FString WhereIs(const FRoadAgent& Agent, const URoadNetwork& Network);
```
InspectFacts.cpp, in DescribeAgent after `Out.Destination = DestinationOf(*Agent, Network);`:
```cpp
		Out.On = Network != nullptr ? WhereIs(*Agent, *Network) : FString();
```
InspectFacts.cpp, inside `namespace InspectFacts` (after DescribeAgent):
```cpp
	FString WhereIs(const FRoadAgent& Agent, const URoadNetwork& Network)
	{
		const FRoutePlan& Plan = Agent.PlanInProgress();
		if (Plan.Steps.Num() == 0)
		{
			return FString();
		}
		const int32 Step = UGroundTraffic::CurrentStep(Plan, Agent.DistanceAlongPlan());
		const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Plan.Steps[Step].Edge);
		if (Edge == nullptr)
		{
			return FString();   // a plan against an edge a rebuild has since replaced
		}
		if (Network.GetSegment(Edge->DerivedFrom) != nullptr)
		{
			if (Network.IsRunwaySegment(Edge->DerivedFrom))
			{
				FRunwayCardFacts Card;
				return DescribeRunway(Network, Edge->DerivedFrom.Index, Card) ? Card.Pair : FString();
			}
			return Network.TaxiwayDisplayName(Network.TaxiwayOf(Edge->DerivedFrom));
		}
		return Edge->AtJunction.IsSet() ? Network.JunctionName(Edge->AtJunction) : FString();
	}
```
GroundTrafficDeadlock.cpp - add `#include "Model/InspectFacts.h"`; replace the "no member can turn" block (lines 631-635):
```cpp
			if (!LogAirsideTraffic.IsSuppressed(ELogVerbosity::Warning))
			{
				// WHERE, by its lowest member (the alert's key), in names (taxiway naming spec: "the deadlock alert/log
				// says where") - empty on an unnamed network, which reads as it always did.
				const FRoadAgent* Lowest = FindAgentIn(Agents, AgentIndex, Key);
				const FString Where = Lowest != nullptr ? InspectFacts::WhereIs(*Lowest, Network) : FString();
				UE_LOG(LogAirsideTraffic, Warning, TEXT("%sDeadlock among agents [%s]%s: no member can turn; retrying in %.0f s"),
					bAllAircraft ? TEXT("All-aircraft ") : TEXT(""), *JoinCycleMembers(Cycle, Agents, AgentIndex),
					Where.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" on %s"), *Where), Rules.RetrySeconds);
			}
```
(`Network` is Resolve's local at line 402 and `Key` the lowest member at line 437 - verify both are in scope at this line before building; if `Agents` is a const view here, `FindAgentIn`'s const overload at line 29 is the one taken.)

OpsAlerts.cpp - add `#include "Model/InspectFacts.h"`; in the deadlock block replace from `FText Text;` (line 243) through the closing brace of its if/else chain (line 258) with:
```cpp
			// WHERE, by the lowest member - the agent the alert focuses (taxiway naming spec 2026-10-02): " on A3", or
			// nothing on an unnamed network, so the sentence reads as before there.
			const FRoadAgent* Focus = Sources.Traffic->FindAgent(Lowest);
			const FString Where = Focus != nullptr && Sources.Network != nullptr ? InspectFacts::WhereIs(*Focus, *Sources.Network) : FString();
			const FText On = Where.IsEmpty() ? FText::GetEmpty()
				: FText::Format(NSLOCTEXT("OpsAlerts", "DeadlockOn", " on {0}"), FText::FromString(Where));
			FText Text;
			if (Vehicles == 0)
			{
				Text = FText::Format(NSLOCTEXT("OpsAlerts", "Deadlock", "{0} aircraft deadlocked{2} - {1}"),
					FText::AsNumber(Aircraft), DeadlockRemedy(), On);
			}
			else if (Aircraft == 0)
			{
				Text = FText::Format(NSLOCTEXT("OpsAlerts", "DeadlockVehicles", "{0} {0}|plural(one=vehicle,other=vehicles) deadlocked{2} - {1}"),
					Vehicles, DeadlockRemedy(), On);
			}
			else
			{
				Text = FText::Format(NSLOCTEXT("OpsAlerts", "DeadlockMixed", "{0} aircraft and {1} {1}|plural(one=vehicle,other=vehicles) deadlocked{3} - {2}"),
					Aircraft, Vehicles, DeadlockRemedy(), On);
			}
```
(Keep the `const int32 Vehicles = Cycle.Num() - Aircraft;` line above it.)

InspectorCards.h, FAircraftDisplay after `FString Destination;`: `FString On;`.
InspectorAircraftCard.cpp DisplayOf, after `D.Destination = F.Destination;`: `D.On = F.On;`. Compose, immediately after the `Out.Facts = FString::Format(...)` statement:
```cpp
	// WHERE IT IS, in names (taxiway naming spec 2026-10-02) - only when the facts know (InspectFacts::WhereIs), so an
	// unnamed airport's card reads as before.
	if (!D.On.IsEmpty())
	{
		Out.Facts += FString::Format(*NSLOCTEXT("AirportMgr", "InspectorOn", "\nOn: {0}").ToString(), { D.On });
	}
```

- [ ] **Step 4: Build, run**

`-Filter "Airside.Model.TaxiwayNames+AirportOps.Model.Alerts+AirportMgr.Inspector"`. Expected: 0 failed, 0 crashed; `DeadlockWithAReversingTruckDoesNotCallItAnAircraft` still passes (its network is never named).

- [ ] **Step 5: Commit**
```powershell
git add Plugins/Airside/Source/Airside/Public/Model/InspectFacts.h Plugins/Airside/Source/Airside/Private/Model/InspectFacts.cpp `
  Plugins/Airside/Source/Airside/Private/Model/GroundTrafficDeadlock.cpp Plugins/AirportOps/Source/AirportOps/Private/Model/OpsAlerts.cpp `
  Source/AirportMgr/InspectorCards.h Source/AirportMgr/InspectorAircraftCard.cpp `
  Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp Plugins/AirportOps/Source/AirportOpsTests/Private/OpsAlertsTest.cpp `
  Source/AirportMgr/InspectorCardsTest.cpp
git commit -m "names: aircraft card says On: A3, deadlock alert and log say where" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

---

### Task 10: The split toast - facade -> bus -> toast

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsEventBus.h:453` (event struct after `FPushGroundFreedEvent`), `:524` (variant)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/OpsEventBus.cpp:183` (Describe after `FPushGroundFreedEvent::Describe`)
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsEvents.h:76` (delegate type), `:145` (UPROPERTY)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp:671` (Presentation subscriber), `:942` (bridge after "BuildRefused")
- Modify: `Source/AirportMgr/ToastStackWidget.h:195`, `Source/AirportMgr/ToastStackWidget.cpp:94` and a new handler
- Modify: `Plugins/AirportOps/Source/AirportOpsTests/Private/OpsEventsTestListener.h:46`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/OpsRuntimeBusTest.cpp:168` (probe) and a new test; `Source/AirportMgr/ToastStackWidgetTest.cpp`

**Interfaces:**
- Consumes: `URoadEditFacade::OnTaxiwaySplit` (Task 7); `UOpsRuntime::AirsideBridges()` table (OpsRuntime.h:427-442); `FOpsEventBus::Publish`, `Subscribe<T>(EOpsTier::Presentation, ...)`; `UNotificationCentre::PostFeed`.
- Produces: `struct AIRPORTOPS_API FTaxiwaySplitEvent { FString SplitOff; FString From; static const TCHAR* EventName() { return TEXT("TaxiwaySplit"); } FString Describe() const; }`; `DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOpsTaxiwaySplit, const FString&, SplitOff, const FString&, From)`; `UOpsEvents::OnTaxiwaySplit`; `UToastStackWidget::OnTaxiwaySplit(const FString&, const FString&)`.

- [ ] **Step 1: Write the failing tests**

ToastStackWidgetTest.cpp - append before `#endif`:
```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FToastsTaxiwaySplitTest, "AirportMgr.UI.ToastsSayTaxiwaySplits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FToastsTaxiwaySplitTest::RunTest(const FString& Parameters)
{
	// "C split off from A" (taxiway naming spec): the player's own edit renamed part of a taxiway - Info, not a Warning,
	// since nothing needs fixing. Bound through BindTo, the seam the runtime binding uses.
	FAirsideTestWorld TestWorld;
	UToastStackWidget* Stack = MakeStack(TestWorld.World);
	if (!TestNotNull(TEXT("a toast stack"), Stack)) { return false; }
	UOpsEvents* Events = NewObject<UOpsEvents>();
	Stack->BindTo(*Events);
	Events->OnTaxiwaySplit.Broadcast(TEXT("C"), TEXT("A"));
	if (!TestEqual(TEXT("one toast"), Stack->Centre()->Entries().Num(), 1)) { return false; }
	TestEqual(TEXT("in the spec's words"), Stack->Centre()->Entries()[0].Text.ToString(), FString(TEXT("C split off from A")));
	TestEqual(TEXT("Info"), Stack->Centre()->Entries()[0].Severity, ENotificationSeverity::Info);
	return true;
}
```
OpsEventsTestListener.h - after the `OnLandRefused` UFUNCTION (line 46 block):
```cpp
	UFUNCTION() void OnTaxiwaySplit(const FString& SplitOff, const FString& From) { Seen.Add(TEXT("split:") + SplitOff + TEXT("<") + From); }
```
OpsRuntimeBusTest.cpp - in the `Probes` list (after the `BuildRefused` probe, line 169):
```cpp
		{ TEXT("TaxiwaySplit"), [&]() { Facade->OnTaxiwaySplit.Broadcast(TEXT("C"), TEXT("A")); },
			[&]() { return Bus.DispatchedCountOfForTest<FTaxiwaySplitEvent>(); } },
```
and append a test after `AirportOps.Present.Alerts.BuildRefusalReachesUi`:
```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsRuntimeTaxiwaySplitTest, "AirportOps.Present.TaxiwaySplitReachesUi",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsRuntimeTaxiwaySplitTest::RunTest(const FString&)
{
	// THE WHOLE ROUTE (plan D10): a real delete through the actor, the facade's normalise, the bridge, the bus, the face.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor to attach to"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = RuntimeBusTestAttach(TestWorld);
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Runtime->GetEvents()->OnTaxiwaySplit.AddDynamic(Listener, &UOpsEventsTestListener::OnTaxiwaySplit);
	ARoadNetworkActor* Actor = TestWorld.Actor;
	const int32 N0 = Actor->PlaceNode(FVector2D(0.0, 60000.0));
	const int32 N1 = Actor->PlaceNode(FVector2D(40000.0, 60000.0));
	const int32 N2 = Actor->PlaceNode(FVector2D(50000.0, 60000.0));
	const int32 N3 = Actor->PlaceNode(FVector2D(70000.0, 60000.0));
	TestTrue(TEXT("click 1"), Actor->ConnectNodes(N0, N1, ERoadKind::Taxiway, INDEX_NONE));
	TestTrue(TEXT("click 2"), Actor->ConnectNodes(N1, N2, ERoadKind::Taxiway, INDEX_NONE));
	const int32 Middle = Actor->Network->GetSegments().Num() - 1;
	TestTrue(TEXT("click 3"), Actor->ConnectNodes(N2, N3, ERoadKind::Taxiway, INDEX_NONE));
	TestTrue(TEXT("the middle is deleted"), Actor->DeleteSegment(Middle));
	Runtime->Tick(0.0);
	TestEqual(TEXT("the split reaches the UI's face of the bus, once"), Listener->CountOf(TEXT("split:")), 1);
	TestEqual(TEXT("as B from A"), Listener->CountOf(TEXT("split:B<A")), 1);
	return true;
}
```

- [ ] **Step 2: Build - expect a compile failure**

Expected: `'OnTaxiwaySplit': is not a member of 'UOpsEvents'`, `'FTaxiwaySplitEvent': undeclared identifier`.

- [ ] **Step 3: Implement**

OpsEventBus.h, after `FPushGroundFreedEvent` (line 453):
```cpp

/** A taxiway edit split a piece off a taxiway (URoadEditFacade::OnTaxiwaySplit, bridged) - "C split off from A". */
struct AIRPORTOPS_API FTaxiwaySplitEvent
{
	FString SplitOff;
	FString From;
	static const TCHAR* EventName() { return TEXT("TaxiwaySplit"); }
	FString Describe() const;
};
```
Variant (line 524): `FFlightPhaseChangedEvent>;` becomes `FFlightPhaseChangedEvent, FTaxiwaySplitEvent>;` (APPENDED: an index never moves).
OpsEventBus.cpp, after `FPushGroundFreedEvent::Describe`'s body:
```cpp
FString FTaxiwaySplitEvent::Describe() const
{
	return FString::Printf(TEXT("%s split off from %s"), *SplitOff, *From);
}
```
OpsEvents.h, after line 76: 
```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOpsTaxiwaySplit, const FString&, SplitOff, const FString&, From);
```
After line 145 (`FOpsBalanceSignChanged OnBalanceSignChanged;`):
```cpp

	/** "C split off from A" (taxiway naming spec 2026-10-02): two NOUNS, the toast words them - no sentence face (rule 4). */
	UPROPERTY(BlueprintAssignable) FOpsTaxiwaySplit OnTaxiwaySplit;
```
OpsRuntime.cpp WireBus, after the `FBuildRefusedEvent` Presentation subscriber (line 671):
```cpp
	Bus.Subscribe<FTaxiwaySplitEvent>(EOpsTier::Presentation, TEXT("OpsEvents"),
		[this](const FTaxiwaySplitEvent& E) { Events->OnTaxiwaySplit.Broadcast(E.SplitOff, E.From); });
```
OpsRuntime.cpp AirsideBridges, after the "BuildRefused" entry (before `return Out;`):
```cpp
	// A TAXIWAY SPLIT, bridged (taxiway naming spec 2026-10-02): the facade's normalise reports it, the toast says it.
	Out.Add({ TEXT("TaxiwaySplit"),
		[](UOpsRuntime& Runtime, ARoadNetworkActor& Actor)
		{
			URoadEditFacade* Facade = Actor.GetEditFacade();
			return Facade == nullptr ? FDelegateHandle() : Facade->OnTaxiwaySplit.AddWeakLambda(&Runtime,
				[&Runtime](const FString& SplitOff, const FString& From) { Runtime.Bus.Publish(FTaxiwaySplitEvent{ SplitOff, From }); });
		},
		[](ARoadNetworkActor& Actor, FDelegateHandle Handle) { if (URoadEditFacade* Facade = Actor.GetEditFacade()) { Facade->OnTaxiwaySplit.Remove(Handle); } } });
```
ToastStackWidget.h, after `OnBalanceSignChanged`'s UFUNCTION (line 195):
```cpp
	/** "C split off from A" - Info: the player's own edit renamed part of a taxiway, and nothing needs fixing. */
	UFUNCTION() void OnTaxiwaySplit(const FString& SplitOff, const FString& From);
```
ToastStackWidget.cpp BindTo, after line 94: `Events.OnTaxiwaySplit.AddUniqueDynamic(this, &UToastStackWidget::OnTaxiwaySplit);` and the handler (after `OnBuildRefused`):
```cpp
void UToastStackWidget::OnTaxiwaySplit(const FString& SplitOff, const FString& From)
{
	if (Notifications != nullptr)
	{
		Notifications->PostFeed(FText::Format(NSLOCTEXT("AirportMgr", "TaxiwaySplit", "{0} split off from {1}"),
			FText::FromString(SplitOff), FText::FromString(From)), ENotificationSeverity::Info);
	}
}
```

- [ ] **Step 4: Build, run**

`-Filter "AirportOps.Present.Bus+AirportOps.Model.Bus+AirportOps.Present.TaxiwaySplitReachesUi+AirportMgr.UI.Toasts"`. Expected: 0 failed; `ReattachDoesNotDouble` names TaxiwaySplit among its probes; `EveryEventHasASubscriber` and `Bus.DescribeEveryEvent`-style tests cover the new event.

- [ ] **Step 5: Commit**
```powershell
git add Plugins/AirportOps/Source/AirportOps/Public/Model/OpsEventBus.h Plugins/AirportOps/Source/AirportOps/Private/Model/OpsEventBus.cpp `
  Plugins/AirportOps/Source/AirportOps/Public/Model/OpsEvents.h Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp `
  Source/AirportMgr/ToastStackWidget.h Source/AirportMgr/ToastStackWidget.cpp Source/AirportMgr/ToastStackWidgetTest.cpp `
  Plugins/AirportOps/Source/AirportOpsTests/Private/OpsEventsTestListener.h Plugins/AirportOps/Source/AirportOpsTests/Private/OpsRuntimeBusTest.cpp
git commit -m "toast: C split off from A - facade split bridged to the toast stack" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

### PR 1 boundary

- [ ] Full suite once at the tip: `./Tools/Run-AirsideTests.ps1 -Project C:\repos\airportmgr2-slot3\AirportMgr.uproject` -> quote the `N test(s) run, 0 failed, 0 crashed` line.
- [ ] `git fetch; git merge-base --is-ancestor origin/main HEAD` true, else rebase onto origin/main, rebuild, re-run.
- [ ] Count deltas for the PR body: `UE_LOG(` and comment lines in every touched file, before (`git show origin/main:<file>`) and after.
- [ ] `git push -u origin feature/taxiway-names`; `gh pr create --base main --title "Taxiway names: model, backfill, deadlock + inspector text, split toast"` with the template filled (build line, test line, deltas, Deviations D1-D11 copied in, "no save-compat shim: old saves backfill").

---

# PR 2 - Labels (branch `feature/taxiway-labels`, from `feature/taxiway-names`)

`git switch -c feature/taxiway-labels` at PR 1's tip.

### Task 11: Label anchors as meanings - Model/TaxiwayLabels

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Model/TaxiwayLabels.h`, `Plugins/Airside/Source/Airside/Private/Model/TaxiwayLabels.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp`

**Interfaces:**
- Consumes: `URoadNetwork::GetTaxiways`, `TaxiwayChainOf`, `TaxiwayDisplayName`, `GetSegment`, `GetNode`, `GetOtherEnd`; `GuidelineGeom::Eval(A, Control, B, T)`, `GuidelineGeom::Length`.
- Produces: `struct FTaxiwayLabel { int32 Taxiway; FString Name; FVector2D At; }`; `AIRSIDE_API TArray<FTaxiwayLabel> TaxiwayLabels::Anchors(const URoadNetwork& Network, double RepeatEvery);`

- [ ] **Step 1: Write the failing test** (append; add `#include "Model/TaxiwayLabels.h"`)

```cpp
/** Labels (spec "UI"): one per taxiway at the middle of its longest segment, repeated every RepeatEvery along it;
 *  none for an empty parent kept only for its connectors. Meanings - a name and a road-plane point - never a colour. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesLabelAnchorsTest, "Airside.Model.TaxiwayNames.LabelAnchors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesLabelAnchorsTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	TArray<FRoadNodeId> A;
	for (int32 Index = 0; Index <= 4; ++Index) { A.Add(N.Node(30000.0 * Index, 0.0)); }
	for (int32 Index = 0; Index < 4; ++Index) { N.Click(A[Index], A[Index + 1]); }
	const FRoadSegmentId Stub = N.Click(A[2], N.Node(60000.0, -20000.0));
	const TArray<FTaxiwayLabel> Labels = TaxiwayLabels::Anchors(*N.Net, 50000.0);
	TArray<double> AxAt;
	int32 Connectors = 0;
	for (const FTaxiwayLabel& Label : Labels)
	{
		if (Label.Name == TEXT("A")) { AxAt.Add(Label.At.X); }
		if (Label.Name == TEXT("A1")) { ++Connectors; TestEqual(TEXT("A1 at its middle"), Label.At, FVector2D(60000.0, -10000.0), 1.0); }
	}
	AxAt.Sort();
	// 1200 m in four equal 300 m segments: the tie goes to the first in chain order, middle at 150 m; then every 500 m.
	TestEqual(TEXT("A repeated every 500 m from the middle of its longest segment"), AxAt, TArray<double>{ 15000.0, 65000.0, 115000.0 });
	TestEqual(TEXT("a short connector once"), Connectors, 1);
	N.Net->RemoveSegment(N.Net->SegmentIdAt(0));
	for (int32 Index = 1; Index < 4; ++Index) { N.Net->RemoveSegment(N.Net->SegmentIdAt(Index)); }
	N.Normalise();
	bool bParentLabel = false;
	for (const FTaxiwayLabel& Label : TaxiwayLabels::Anchors(*N.Net, 50000.0)) { bParentLabel |= Label.Name == TEXT("A"); }
	TestFalse(TEXT("an empty parent kept for A1 draws no label"), bParentLabel);
	TestEqual(TEXT("A1 is still drawn"), N.NameOf(Stub), FString(TEXT("A1")));
	return true;
}
```
(Anchors at 15000 + k * 50000 inside [0, 120000]: 15000, 65000, 115000.)

- [ ] **Step 2: Build - expect a compile failure** (`Model/TaxiwayLabels.h: No such file`).

- [ ] **Step 3: Implement**

`Public/Model/TaxiwayLabels.h`:
```cpp
#pragma once

#include "CoreMinimal.h"

class URoadNetwork;

/** One taxiway name to show and the road-plane point it belongs at - a MEANING (Tool/'s rule): the driver picks the look. */
struct FTaxiwayLabel
{
	int32 Taxiway = INDEX_NONE;
	FString Name;
	FVector2D At = FVector2D::ZeroVector;
};

/**
 * Where taxiway names go (spec "UI"): one per live taxiway WITH segments, at the middle of its longest segment (ties to
 * the first in chain order), repeated every RepeatEvery uu along the chain both ways while on it. Model/, world-free,
 * so the two drivers draw one answer (Tool/TaxiwayNameOverlay hands it to their sinks).
 * ENFORCED BY: Airside.Model.TaxiwayNames.LabelAnchors, Airside.Present.TaxiwayNames.LabelsMatchNames
 */
namespace TaxiwayLabels
{
	AIRSIDE_API TArray<FTaxiwayLabel> Anchors(const URoadNetwork& Network, double RepeatEvery);
}
```
`Private/Model/TaxiwayLabels.cpp`:
```cpp
#include "Model/TaxiwayLabels.h"

#include "Model/RoadNetwork.h"
#include "Solve/GuidelineGeom.h"

TArray<FTaxiwayLabel> TaxiwayLabels::Anchors(const URoadNetwork& Network, double RepeatEvery)
{
	TArray<FTaxiwayLabel> Out;
	const double Step = FMath::Max(RepeatEvery, 1000.0);
	for (const FTaxiway& Taxiway : Network.GetTaxiways())
	{
		if (!Taxiway.bAlive)
		{
			continue;
		}
		const FTaxiwayChain Chain = Network.TaxiwayChainOf(Taxiway.Id);
		if (Chain.Segments.Num() == 0)
		{
			continue;   // an empty parent kept for its connectors: nothing to stand on
		}
		// EACH SEGMENT IN CHAIN DIRECTION: its start node, its length, where it starts along the chain.
		struct FPiece { FVector2D From; FVector2D Control; FVector2D To; double Start = 0.0; double Length = 0.0; };
		TArray<FPiece> Pieces;
		FRoadNodeId Node = Chain.First;
		double Along = 0.0;
		int32 Longest = 0;
		for (const FRoadSegmentId& Id : Chain.Segments)
		{
			const FRoadSegment* Segment = Network.GetSegment(Id);
			const FRoadNodeId Next = Network.GetOtherEnd(Id, Node);
			const FRoadNode* A = Network.GetNode(Node);
			const FRoadNode* B = Network.GetNode(Next);
			if (Segment == nullptr || A == nullptr || B == nullptr)
			{
				break;
			}
			FPiece& Piece = Pieces.AddDefaulted_GetRef();
			Piece.From = A->Position;
			Piece.Control = Segment->Control;
			Piece.To = B->Position;
			Piece.Start = Along;
			Piece.Length = GuidelineGeom::Length(Piece.From, Piece.Control, Piece.To);
			Along += Piece.Length;
			Longest = Piece.Length > Pieces[Longest].Length ? Pieces.Num() - 1 : Longest;
			Node = Next;
		}
		if (Pieces.Num() == 0)
		{
			continue;
		}
		const FString Name = Network.TaxiwayDisplayName(Taxiway.Id);
		const double Centre = Pieces[Longest].Start + Pieces[Longest].Length * 0.5;
		const int32 Before = FMath::FloorToInt(Centre / Step);
		const int32 After = FMath::FloorToInt((Along - Centre) / Step);
		for (int32 K = -Before; K <= After; ++K)
		{
			const double S = Centre + K * Step;
			for (const FPiece& Piece : Pieces)
			{
				if (S <= Piece.Start + Piece.Length || &Piece == &Pieces.Last())
				{
					// A CURVE PARAMETER, not an arc length - close enough for a tag, and GuidelineGeom's one evaluator.
					const double T = Piece.Length > 0.0 ? FMath::Clamp((S - Piece.Start) / Piece.Length, 0.0, 1.0) : 0.5;
					Out.Add({ Taxiway.Id, Name, GuidelineGeom::Eval(Piece.From, Piece.Control, Piece.To, T) });
					break;
				}
			}
		}
	}
	return Out;
}
```

- [ ] **Step 4: Build twice (new .cpp), run** `-Filter Airside.Model.TaxiwayNames.LabelAnchors` -> PASS.

- [ ] **Step 5: Commit**
```powershell
git add Plugins/Airside/Source/Airside/Public/Model/TaxiwayLabels.h Plugins/Airside/Source/Airside/Private/Model/TaxiwayLabels.cpp `
  Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp
git commit -m "model: taxiway label anchors - longest segment's middle, repeated every 500 m" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

---

### Task 12: Both drivers draw the names - one style, one predicate

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h:421` (append `TaxiwayName` after `GridMajor`)
- Modify: `Plugins/Airside/Source/Airside/Public/Present/PreviewPalette.h` (FPreviewLook `bTag`), `Plugins/Airside/Source/Airside/Private/Present/PreviewPalette.cpp:83` (Default case), `:204` (DefaultLook case)
- Create: `Plugins/Airside/Source/Airside/Public/Tool/TaxiwayNameOverlay.h`, `Plugins/Airside/Source/Airside/Private/Tool/TaxiwayNameOverlay.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h:425` (predicate after `WantsRoadNodesDrawn`)
- Modify: `Source/AirportMgr/RoadBuildController.h:354`, `Source/AirportMgr/RoadBuildController.cpp:896`
- Modify: `Source/AirportMgr/RoadBuildHUD.cpp:25-35` (Looks list), `:113-116` (DrawHUD), `:412-424` (Label)
- Modify: `Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEditorTool.cpp:600` (DrawPersistentState), `:965-974` (DrawHUD label loop)
- Test: `Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesPresentTest.cpp`; `Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEditorToolPreviewLabelTest.cpp`

**Interfaces:**
- Consumes: `TaxiwayLabels::Anchors` (Task 11); `IToolPreviewSink::Label(const FVector2D&, const FString&, EPreviewStyle)` (RoadBuildTool.h:445); `ARoadNetworkActor::TaxiwayNaming.LabelRepeatDistance`; `FBuildSession::WantsRoadNodesDrawn()`; `ARoadBuildController::IsGuidelineOverlayOn()` (the G toggle, RoadBuildController.h:354); `AHUD::GetTextSize`, `AHUD::DrawRect` (HUD.h:221, :257); `UFont::GetStringSize/GetStringHeightSize` (Font.h:269, :278).
- Produces: `EPreviewStyle::TaxiwayName`; `FPreviewLook::bTag`; `AIRSIDE_API void TaxiwayNameOverlay::Describe(const URoadNetwork&, double RepeatEvery, IToolPreviewSink&)`; `bool FBuildSession::WantsTaxiwayNamesDrawn(bool bOverlayToggle) const`; `bool ARoadBuildController::WantsTaxiwayNamesDrawn() const`.

- [ ] **Step 1: Write the failing tests**

TaxiwayNamesPresentTest.cpp - add includes `#include "Present/PreviewPalette.h"`, `#include "Tool/BuildSession.h"`, `#include "Tool/RoadBuildTool.h"`, `#include "Tool/TaxiwayNameOverlay.h"`; append:
```cpp
namespace TaxiwayNamesPresentTest
{
	/** Records Label calls only. Prefixed against the UNITY build. */
	struct FTaxiwayNamesLabelSink : IToolPreviewSink
	{
		TArray<FString> Names;
		virtual void Marker(const FVector2D&, EPreviewStyle) override {}
		virtual void Line(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void CrossMark(const FVector2D&, const FVector2D&, EPreviewStyle) override {}
		virtual void Label(const FVector2D&, const FString& Text, EPreviewStyle Style) override
		{
			if (Style == EPreviewStyle::TaxiwayName) { Names.AddUnique(Text); }
		}
	};
}

/** Spawn the actor, draw, and the labels the drivers are handed are exactly the names (spec "Seam tests"). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesLabelsTest, "Airside.Present.TaxiwayNames.LabelsMatchNames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesLabelsTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesPresentTest;
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	TestTrue(TEXT("a taxiway"), Actor->ConnectNodes(Actor->PlaceNode({ 0.0, 0.0 }), Actor->PlaceNode({ 60000.0, 0.0 }),
		ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	TestTrue(TEXT("another"), Actor->ConnectNodes(Actor->PlaceNode({ 0.0, 100000.0 }), Actor->PlaceNode({ 60000.0, 100000.0 }),
		ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	FTaxiwayNamesLabelSink Sink;
	TaxiwayNameOverlay::Describe(*Actor->Network, Actor->TaxiwayNaming.LabelRepeatDistance, Sink);
	Sink.Names.Sort();
	TestEqual(TEXT("the labels are the names"), Sink.Names, TArray<FString>{ TEXT("A"), TEXT("B") });
	TestTrue(TEXT("a name is drawn as a tag"), PreviewPalette::DefaultLook(EPreviewStyle::TaxiwayName).bTag);
	TestFalse(TEXT("and nothing else is"), PreviewPalette::DefaultLook(EPreviewStyle::Pending).bTag);

	// WHEN (spec): shown while a build tool is lit; while watching, the G toggle decides. The session answers for both drivers.
	FBuildSession Session;
	TestTrue(TEXT("watching, G on: names shown"), Session.WantsTaxiwayNamesDrawn(true));
	TestEqual(TEXT("G off: names exactly while a build tool is lit (the rings' answer)"),
		Session.WantsTaxiwayNamesDrawn(false), Session.WantsRoadNodesDrawn());
	return true;
}
```
RoadBuildEditorToolPreviewLabelTest.cpp - append before `#endif`, reusing that file's harness shape:
```cpp
/** The editor driver hands the names to DrawHUD too (spec: "Both drivers draw them"; plan D12: while hovering). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEditorToolTaxiwayNamesTest,
	"Airside.Editor.TaxiwayNamesReachTheEditor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEditorToolTaxiwayNamesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a target actor"), TestWorld.Actor)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	TestTrue(TEXT("a taxiway"), Actor->ConnectNodes(Actor->PlaceNode({ 0.0, 0.0 }), Actor->PlaceNode({ 60000.0, 0.0 }),
		ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));

	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(Mode);
	Builder->ToolIndex = 0;
	FToolBuilderState State;
	State.ToolManager = NewObject<UInteractiveToolManager>(Mode);
	URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	if (!TestNotNull(TEXT("the builder made a tool"), Tool)) { return false; }
	Tool->Setup();
	Tool->SetTargetForTest(Actor);
	const double SurfaceZ = Actor->SurfaceZ;
	Tool->OnUpdateHover(FInputDeviceRay(FRay(FVector(100.0, 100.0, SurfaceZ + 1000.0), FVector(0.0, 0.0, -1.0)), FVector2D(100.0, 100.0)));
	if (!TestNotNull(TEXT("a tool is active"), Tool->SessionForTest()->GetActiveTool())) { return false; }
	Tool->CachePreviewLabelsForTest();
	TestTrue(TEXT("A reaches the editor's labels"), Tool->CollectPreviewLabelTextForTest().Contains(TEXT("A")));
	Tool->Shutdown(EToolShutdownType::Cancel);
	return true;
}
```

- [ ] **Step 2: Build - expect a compile failure** (`'TaxiwayName': is not a member of 'EPreviewStyle'`, `TaxiwayNameOverlay.h` missing).

- [ ] **Step 3: Implement**

RoadBuildTool.h, after `GridMajor,` (line 421):
```cpp

	/**
	 * A taxiway's name (spec 2026-10-02 "Labels") - context, like Guideline: true whatever the gesture. Drawn as a
	 * fixed-pixel tag (FPreviewLook::bTag), never sized against the road. AT THE END, for ServiceEdge's reason: this is a
	 * UENUM and renumbering it repoints any value already serialised against it.
	 */
	TaxiwayName,
```
PreviewPalette.h, FPreviewLook after `bTranslucentLine`:
```cpp

	/** Text on a filled black tile, centred on its point, at a fixed pixel size - a NAME to read at any zoom
	 *  (EPreviewStyle::TaxiwayName). Both drivers honour it: ARoadBuildHUD::Label, URoadBuildEditorTool::DrawHUD. */
	UPROPERTY(EditAnywhere)
	bool bTag = false;
```
PreviewPalette.cpp Default, after `GridMajor`'s case:
```cpp
	// AIRFIELD SIGN YELLOW on the tag's black (spec: "yellow-on-black tags") - the colour a taxiway location sign is
	// painted, so the name reads as a sign, and a hue no gesture style here uses at this saturation.
	case EPreviewStyle::TaxiwayName:                 return FLinearColor(1.0f, 0.85f, 0.0f);
```
PreviewPalette.cpp DefaultLook, after `GridMajor`'s case block:
```cpp
	case EPreviewStyle::TaxiwayName:
		Look.bTag = true;
		break;
```
`Public/Tool/TaxiwayNameOverlay.h`:
```cpp
#pragma once

#include "CoreMinimal.h"

class URoadNetwork;
struct IToolPreviewSink;

/**
 * The taxiway names, described to a sink as EPreviewStyle::TaxiwayName labels at TaxiwayLabels::Anchors - ONE call
 * both drivers make (ARoadBuildHUD::DrawHUD, URoadBuildEditorTool::DrawPersistentState), GuidelineOverlay's shape, so
 * they cannot place names differently. WHETHER to call it is FBuildSession::WantsTaxiwayNamesDrawn's.
 * ENFORCED BY: Airside.Present.TaxiwayNames.LabelsMatchNames, Airside.Editor.TaxiwayNamesReachTheEditor
 */
namespace TaxiwayNameOverlay
{
	AIRSIDE_API void Describe(const URoadNetwork& Network, double RepeatEvery, IToolPreviewSink& Sink);
}
```
`Private/Tool/TaxiwayNameOverlay.cpp`:
```cpp
#include "Tool/TaxiwayNameOverlay.h"

#include "Model/TaxiwayLabels.h"
#include "Tool/RoadBuildTool.h"

void TaxiwayNameOverlay::Describe(const URoadNetwork& Network, double RepeatEvery, IToolPreviewSink& Sink)
{
	for (const FTaxiwayLabel& Label : TaxiwayLabels::Anchors(Network, RepeatEvery))
	{
		Sink.Label(Label.At, Label.Name, EPreviewStyle::TaxiwayName);
	}
}
```
BuildSession.h after `WantsRoadNodesDrawn()` (line 425):
```cpp

	/**
	 * Whether taxiway names belong on screen (spec "Labels"): while a build tool is lit (the rings' own answer), and
	 * otherwise - watching - when the driver's Guidelines toggle is on (G in PIE; the editor, which has no toggle, passes
	 * true - plan D12). No new key. ON THE SESSION, WantsRoadNodesDrawn's reason: both drivers ask here.
	 * ENFORCED BY: Airside.Present.TaxiwayNames.LabelsMatchNames, Check-Architecture rule 47 (both drivers call it)
	 */
	bool WantsTaxiwayNamesDrawn(bool bOverlayToggle) const
	{
		return WantsRoadNodesDrawn() || bOverlayToggle;
	}
```
RoadBuildController.h, after `bool IsGuidelineOverlayOn() const { return bShowGuidelines; }` (line 354):
```cpp
	/** FBuildSession::WantsTaxiwayNamesDrawn with the G toggle - read by ARoadBuildHUD every frame. */
	bool WantsTaxiwayNamesDrawn() const;
```
RoadBuildController.cpp, after `ARoadBuildController::WantsRoadNodesDrawn`'s body (line 896):
```cpp

bool ARoadBuildController::WantsTaxiwayNamesDrawn() const
{
	return Session.WantsTaxiwayNamesDrawn(bShowGuidelines);
}
```
RoadBuildHUD.cpp constructor list (line 34): `EPreviewStyle::GridMinor, EPreviewStyle::GridMajor })` becomes `EPreviewStyle::GridMinor, EPreviewStyle::GridMajor, EPreviewStyle::TaxiwayName })`. Add `#include "Tool/TaxiwayNameOverlay.h"`. In DrawHUD after the GuidelineOverlay block (line 116):
```cpp

	// TAXIWAY NAMES (spec 2026-10-02), after the routing graph and before the tool's intent: context, read at any zoom.
	if (Target->Network != nullptr && Controller->WantsTaxiwayNamesDrawn())
	{
		TaxiwayNameOverlay::Describe(*Target->Network, Target->TaxiwayNaming.LabelRepeatDistance, *this);
	}
```
RoadBuildHUD.cpp Label - replace the body (lines 414-423) with:
```cpp
	FVector2D Screen;
	if (!ProjectPlanePoint(At, PlaneZ, Screen) || GEngine == nullptr)
	{
		return;
	}
	const FPreviewLook& Look = LookFor(Style);
	if (Look.bTag)
	{
		// A TAG: centred on its point on a black tile, in pixels - a name must read the same at every zoom.
		constexpr float Pad = 4.0f;
		UFont* Font = GEngine->GetMediumFont();
		float Width = 0.0f;
		float Height = 0.0f;
		GetTextSize(Text, Width, Height, Font);
		const float X = static_cast<float>(Screen.X) - Width * 0.5f;
		const float Y = static_cast<float>(Screen.Y) - Height * 0.5f;
		DrawRect(FLinearColor::Black, X - Pad, Y - Pad * 0.5f, Width + Pad * 2.0f, Height + Pad);
		DrawText(Text, Look.Colour, X, Y, Font);
		return;
	}
	DrawText(Text, Look.Colour,
		static_cast<float>(Screen.X) + NodeRingRadius * 1.8f,
		static_cast<float>(Screen.Y) + NodeRingRadius,
		GEngine->GetSmallFont());
```
RoadBuildEditorTool.cpp - add `#include "Tool/TaxiwayNameOverlay.h"` and `#include "CanvasItem.h"`. In DrawPersistentState after `GuidelineOverlay::Draw(*Target->Network, Sink);` (line 600):
```cpp

	// TAXIWAY NAMES - the session's answer, with the editor's guidelines-always-on standing in for PIE's G (plan D12).
	if (Sess().WantsTaxiwayNamesDrawn(true))
	{
		TaxiwayNameOverlay::Describe(*Target->Network, Target->TaxiwayNaming.LabelRepeatDistance, Sink);
	}
```
DrawHUD label loop (lines 965-974) becomes:
```cpp
	for (const FEditorPreviewLabel& Label : PendingLabels)
	{
		FVector2D LabelPixelPos;
		if (!SceneView->WorldToPixel(FVector(Label.At, Target->SurfaceZ), LabelPixelPos))
		{
			continue;
		}
		float X = static_cast<float>(LabelPixelPos.X) / DPIScale;
		float Y = static_cast<float>(LabelPixelPos.Y) / DPIScale;
		if (PreviewPalette::DefaultLook(Label.Style).bTag)
		{
			// THE SAME TAG ARoadBuildHUD::Label draws: centred, on a black tile.
			const float Width = static_cast<float>(Font->GetStringSize(*Label.Text));
			const float Height = static_cast<float>(Font->GetStringHeightSize(*Label.Text));
			X -= Width * 0.5f;
			Y -= Height * 0.5f;
			FCanvasTileItem Tile(FVector2D(X - 4.0f, Y - 2.0f), FVector2D(Width + 8.0f, Height + 4.0f), FLinearColor::Black);
			Canvas->DrawItem(Tile);
		}
		Canvas->DrawShadowedString(X, Y, *Label.Text, Font, PreviewPalette::Default(Label.Style));
	}
```

- [ ] **Step 4: Build twice (new .cpp), run**

`-Filter "Airside.Present.TaxiwayNames+Airside.Editor+AirportMgr.HUD+Airside.Tool"`, then `./Tools/Check-Architecture.ps1` (rule 47: the controller's `Session.WantsTaxiwayNamesDrawn(` must be matched by the editor's `Sess().WantsTaxiwayNamesDrawn(` - it is). Expected: 0 failed; PASS banner. `FRoadBuildHUDLooksTest` passes only with `TaxiwayName` in the constructor list - check it ran.

- [ ] **Step 5: Commit**
```powershell
git add Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h Plugins/Airside/Source/Airside/Public/Present/PreviewPalette.h `
  Plugins/Airside/Source/Airside/Private/Present/PreviewPalette.cpp Plugins/Airside/Source/Airside/Public/Tool/TaxiwayNameOverlay.h `
  Plugins/Airside/Source/Airside/Private/Tool/TaxiwayNameOverlay.cpp Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h `
  Source/AirportMgr/RoadBuildController.h Source/AirportMgr/RoadBuildController.cpp Source/AirportMgr/RoadBuildHUD.cpp `
  Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEditorTool.cpp Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEditorToolPreviewLabelTest.cpp `
  Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesPresentTest.cpp
git commit -m "labels: taxiway names as yellow-on-black tags in PIE HUD and editor, G toggles while watching" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

### PR 2 boundary

- [ ] Visual check (memory `visual-changes-iterate-live-not-sdd`): editor up on THIS worktree with its own MCP port if another editor holds 8000 (`-ModelContextProtocolPort=8002`, `AIRSIDE_MCP_PORT=8002`); open `M_ScaleGatwick`, PIE, `python Tools/Mcp.py shot out.png editor`; look: tags readable, not over each other on the parallels. Tune `LabelRepeatDistance` on the instance, not the constructor.
- [ ] Full suite at the tip; push; `gh pr create --base feature/taxiway-names --title "Taxiway names: labels in both drivers"`.

---

# PR 3 - Rename (branch `feature/taxiway-rename`, from `feature/taxiway-labels`)

### Task 13: Rename in the model and through the facade

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h` (public taxiway block)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetworkTaxiways.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h:270` (public, after `SetDriveSide`), `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacade.cpp` (append)
- Test: `TaxiwayNamesTest.cpp`, `TaxiwayNamesPresentTest.cpp`

**Interfaces:**
- Consumes: `IsTaxiwayNameTaken`, `TaxiwayDisplayName`, `FindTaxiwayMutable`, `NoteFactChanged`; `TaxiwayLetters::IsAvoided`; facade `FRoadEditScope(HistoryForEdit(), Network, Label)`, `CommitAndNotify(Edit, EChangeKind::Facts)`, `Edit.Rollback()`.
- Produces: `FString URoadNetwork::WhyTaxiwayNameRefused(int32 TaxiwayId, const FString& Requested) const`; `bool URoadNetwork::RenameTaxiway(int32 TaxiwayId, const FString& Requested)`; `bool URoadEditFacade::RenameTaxiway(int32 TaxiwayId, const FString& Requested, FString& OutWhy)`.

- [ ] **Step 1: Write the failing tests**

TaxiwayNamesTest.cpp (append):
```cpp
/** Rename propagation (spec): A -> K makes its connectors K1..; a connector's own override survives its parent's rename;
 *  a renamed taxiway is never re-judged (plan D3); the old letter is free again (plan D15). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesRenameTest, "Airside.Model.TaxiwayNames.RenamePropagatesToConnectors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesRenameTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	TArray<FRoadNodeId> A;
	for (int32 Index = 0; Index <= 3; ++Index) { A.Add(N.Node(50000.0 * Index, 0.0)); }
	FRoadSegmentId Main;
	for (int32 Index = 0; Index < 3; ++Index) { Main = N.Click(A[Index], A[Index + 1]); }
	const FRoadSegmentId One = N.Click(A[1], N.Node(50000.0, -20000.0));
	const FRoadSegmentId Two = N.Click(A[2], N.Node(100000.0, -20000.0));
	const int32 AId = N.Net->TaxiwayOf(Main);
	const uint32 Revision = N.Net->GetGuidelineRevision();
	TestTrue(TEXT("A renamed k"), N.Net->RenameTaxiway(AId, TEXT(" k ")));
	TestTrue(TEXT("a rename moves the revision the card and caches key on (plan D11)"), N.Net->GetGuidelineRevision() != Revision);
	TestEqual(TEXT("trimmed and upper-cased"), N.NameOf(Main), FString(TEXT("K")));
	TestEqual(TEXT("A1 reads K1"), N.NameOf(One), FString(TEXT("K1")));
	TestEqual(TEXT("A2 reads K2"), N.NameOf(Two), FString(TEXT("K2")));
	TestTrue(TEXT("K2 overridden Q7"), N.Net->RenameTaxiway(N.Net->TaxiwayOf(Two), TEXT("Q7")));
	TestTrue(TEXT("K renamed M"), N.Net->RenameTaxiway(AId, TEXT("M")));
	TestEqual(TEXT("the derived one follows"), N.NameOf(One), FString(TEXT("M1")));
	TestEqual(TEXT("the override stays"), N.NameOf(Two), FString(TEXT("Q7")));
	const FRoadSegmentId Fresh = N.Click(N.Node(0.0, 200000.0), N.Node(60000.0, 200000.0));
	TestEqual(TEXT("A is free again: nothing displays it"), N.NameOf(Fresh), FString(TEXT("A")));
	// A PLAYER'S NAME IS NEVER RE-JUDGED: rename a stub, then grow it past 300 m.
	const FRoadNodeId Free = N.Node(150000.0, -20000.0);
	const FRoadSegmentId Stub = N.Click(A[3], Free);
	TestTrue(TEXT("the stub renamed Z9"), N.Net->RenameTaxiway(N.Net->TaxiwayOf(Stub), TEXT("Z9")));
	N.Click(Free, N.Node(150000.0, -60000.0));
	TestEqual(TEXT("grown past 300 m it keeps the player's name"), N.NameOf(Stub), FString(TEXT("Z9")));
	return true;
}

/** REVIEW FOCUS 5: what a player types. Each refusal says why, in the spec's words; a refused rename changes nothing. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesRenameRefusalsTest, "Airside.Model.TaxiwayNames.RenameRefusals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesRenameRefusalsTest::RunTest(const FString&)
{
	using namespace TaxiwayNamesTest;
	FTaxiwayNamesNet N;
	const FRoadNodeId A1 = N.Node(50000.0, 0.0);
	const FRoadSegmentId A = N.Click(N.Node(0.0, 0.0), A1);
	N.Click(A1, N.Node(100000.0, 0.0));
	const FRoadSegmentId B = N.Click(N.Node(0.0, 100000.0), N.Node(60000.0, 100000.0));
	const FRoadSegmentId Stub = N.Click(A1, N.Node(50000.0, -20000.0));
	const int32 AId = N.Net->TaxiwayOf(A);
	TestTrue(TEXT("setup: the stub overridden K2"), N.Net->RenameTaxiway(N.Net->TaxiwayOf(Stub), TEXT("K2")));
	TestEqual(TEXT("taken"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("b")), FString(TEXT("B is taken")));
	TestEqual(TEXT("I"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("I")), FString(TEXT("I, O and X are avoided: they read as 1, 0 and closed")));
	TestEqual(TEXT("O inside"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("GO")), FString(TEXT("I, O and X are avoided: they read as 1, 0 and closed")));
	TestEqual(TEXT("four characters"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("ABCD")), FString(TEXT("A taxiway name is 1 to 3 letters or digits")));
	TestEqual(TEXT("empty"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("  ")), FString(TEXT("A taxiway name is 1 to 3 letters or digits")));
	TestEqual(TEXT("punctuation"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("A-1")), FString(TEXT("A taxiway name is 1 to 3 letters or digits")));
	// A COLLISION ONLY THROUGH A DERIVED NAME: the stub took A1 and now shows K2, so A's next connector is A2 - and
	// renaming A to K would make it read K2 as well.
	const FRoadSegmentId Second = N.Click(N.Net->GetSegment(A)->A, N.Node(0.0, -20000.0));
	TestEqual(TEXT("setup: A's next connector is A2"), N.NameOf(Second), FString(TEXT("A2")));
	TestEqual(TEXT("K would make A2 read K2, which the stub shows"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("K")), FString(TEXT("K2 is taken")));
	TestFalse(TEXT("a refused rename changes nothing"), N.Net->RenameTaxiway(AId, TEXT("K")));
	TestEqual(TEXT("A is still A"), N.NameOf(A), FString(TEXT("A")));
	TestEqual(TEXT("renaming to its own name is no refusal"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("a")), FString());
	TestEqual(TEXT("digits are fine"), N.Net->WhyTaxiwayNameRefused(AId, TEXT("10")), FString());
	TestEqual(TEXT("a dead taxiway"), N.Net->WhyTaxiwayNameRefused(999, TEXT("Q")), FString(TEXT("That taxiway is gone")));
	TestEqual(TEXT("B untouched"), N.NameOf(B), FString(TEXT("B")));
	return true;
}
```
TaxiwayNamesPresentTest.cpp (append):
```cpp
/** A rename through the facade is ONE undo step (spec: "a test undoes a rename"). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayNamesRenameUndoTest, "Airside.Present.TaxiwayNames.RenameIsOneUndoStep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayNamesRenameUndoTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	TestTrue(TEXT("a taxiway"), Actor->ConnectNodes(Actor->PlaceNode({ 0.0, 0.0 }), Actor->PlaceNode({ 60000.0, 0.0 }),
		ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	const FRoadSegmentId Seg = Actor->Network->SegmentIdAt(Actor->Network->GetSegments().Num() - 1);
	const int32 Id = Actor->Network->TaxiwayOf(Seg);
	URoadEditFacade* Facade = Actor->GetEditFacade();
	FString Why;
	TestFalse(TEXT("a refusal is said"), Facade->RenameTaxiway(Id, TEXT("I"), Why));
	TestEqual(TEXT("in the spec's words"), Why, FString(TEXT("I, O and X are avoided: they read as 1, 0 and closed")));
	TestTrue(TEXT("renamed"), Facade->RenameTaxiway(Id, TEXT("k"), Why));
	TestEqual(TEXT("K"), Actor->Network->TaxiwayDisplayName(Id), FString(TEXT("K")));
	TestTrue(TEXT("undo"), Facade->Undo());
	TestEqual(TEXT("one undo puts A back"), Actor->Network->TaxiwayDisplayName(Id), FString(TEXT("A")));
	return true;
}
```

- [ ] **Step 2: Build - expect a compile failure** (`'RenameTaxiway': is not a member`).

- [ ] **Step 3: Implement**

RoadNetwork.h public taxiway block (append):
```cpp

	/**
	 * Why Requested cannot name TaxiwayId, or empty when it can (spec "Rename"): 1-3 letters or digits after trimming,
	 * none of I/O/X, and unique - counting every connector name the rename would DERIVE ("K" refused while "K2" shows).
	 * Renaming to its own display name is no refusal. Case-insensitive; RenameTaxiway stores it upper-cased.
	 * ENFORCED BY: Airside.Model.TaxiwayNames.RenameRefusals
	 */
	FString WhyTaxiwayNameRefused(int32 TaxiwayId, const FString& Requested) const;

	/**
	 * Store Requested (trimmed, upper-cased) as TaxiwayId's Name and mark it the player's (never re-judged). False,
	 * nothing written, when WhyTaxiwayNameRefused objects. Moves GuidelineRevision (plan D11). URoadEditFacade::
	 * RenameTaxiway is the edit (undo step); this is the write.
	 * ENFORCED BY: Airside.Model.TaxiwayNames.RenamePropagatesToConnectors
	 */
	bool RenameTaxiway(int32 TaxiwayId, const FString& Requested);
```
RoadNetworkTaxiways.cpp (append):
```cpp
FString URoadNetwork::WhyTaxiwayNameRefused(int32 TaxiwayId, const FString& Requested) const
{
	const FTaxiway* Taxiway = GetTaxiway(TaxiwayId);
	if (Taxiway == nullptr)
	{
		return TEXT("That taxiway is gone");
	}
	const FString Name = Requested.TrimStartAndEnd().ToUpper();
	if (Name.Len() < 1 || Name.Len() > 3)
	{
		return TEXT("A taxiway name is 1 to 3 letters or digits");
	}
	for (const TCHAR Character : Name)
	{
		if (!FChar::IsAlnum(Character))
		{
			return TEXT("A taxiway name is 1 to 3 letters or digits");
		}
		if (TaxiwayLetters::IsAvoided(Character))
		{
			return TEXT("I, O and X are avoided: they read as 1, 0 and closed");
		}
	}
	if (Name.Equals(TaxiwayDisplayName(TaxiwayId), ESearchCase::IgnoreCase))
	{
		return FString();
	}
	if (IsTaxiwayNameTaken(Name, TaxiwayId))
	{
		return FString::Printf(TEXT("%s is taken"), *Name);
	}
	// A LETTERED TAXIWAY'S RENAME RENAMES EVERY CONNECTOR THAT DERIVES FROM IT (spec: "renaming A to K makes A1..A4 read
	// K1..K4") - so each derived name must be free too, or the rename would show one name twice.
	if (!Taxiway->IsConnector())
	{
		for (const FTaxiway& Each : Taxiways)
		{
			if (Each.bAlive && Each.ParentId == TaxiwayId && Each.Name.IsEmpty())
			{
				const FString Derived = Name + FString::FromInt(Each.ConnectorNumber);
				if (IsTaxiwayNameTaken(Derived, Each.Id))
				{
					return FString::Printf(TEXT("%s is taken"), *Derived);
				}
			}
		}
	}
	return FString();
}

bool URoadNetwork::RenameTaxiway(int32 TaxiwayId, const FString& Requested)
{
	if (!WhyTaxiwayNameRefused(TaxiwayId, Requested).IsEmpty())
	{
		return false;
	}
	FTaxiway* Taxiway = FindTaxiwayMutable(TaxiwayId);
	Taxiway->Name = Requested.TrimStartAndEnd().ToUpper();
	Taxiway->bPlayerNamed = true;
	NoteFactChanged();
	return true;
}
```
RoadEditFacade.h public (after `bool SetDriveSide(EDriveSide Side);`, line 270):
```cpp

	/**
	 * Rename a taxiway (spec "Rename", PIE's inspector card in v1) - ONE undo step, "rename taxiway". False with OutWhy
	 * the player's sentence (URoadNetwork::WhyTaxiwayNameRefused) when refused, before any snapshot; true and no step
	 * when it already has that name. EChangeKind::Facts: a name re-derives nothing; the card and labels read the
	 * GuidelineRevision the model moved.
	 * ENFORCED BY: Airside.Present.TaxiwayNames.RenameIsOneUndoStep
	 */
	bool RenameTaxiway(int32 TaxiwayId, const FString& Requested, FString& OutWhy);
```
RoadEditFacade.cpp (append):
```cpp
bool URoadEditFacade::RenameTaxiway(int32 TaxiwayId, const FString& Requested, FString& OutWhy)
{
	URoadNetwork* Network = Actor().Network;
	if (Network == nullptr)
	{
		OutWhy = TEXT("No airport");
		return false;
	}
	// REFUSED BEFORE THE SNAPSHOT, every other guard on this seam's reason (SetRunwayFacts).
	OutWhy = Network->WhyTaxiwayNameRefused(TaxiwayId, Requested);
	if (!OutWhy.IsEmpty())
	{
		UE_LOG(LogRoadMesh, Log, TEXT("Rename taxiway %d to '%s' refused: %s"), TaxiwayId, *Requested, *OutWhy);
		return false;
	}
	const FString Was = Network->TaxiwayDisplayName(TaxiwayId);
	if (Was.Equals(Requested.TrimStartAndEnd(), ESearchCase::IgnoreCase))
	{
		return true;   // already so: no undo step that changes nothing
	}
	FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("rename taxiway"));
	if (!Network->RenameTaxiway(TaxiwayId, Requested))
	{
		Edit.Rollback();   // rule 40
		OutWhy = TEXT("Refused");
		return false;
	}
	CommitAndNotify(Edit, EChangeKind::Facts);
	UE_LOG(LogRoadMesh, Log, TEXT("Taxiway %s renamed %s"), *Was, *Network->TaxiwayDisplayName(TaxiwayId));
	return true;
}
```

- [ ] **Step 4: Build, run** `-Filter "Airside.Model.TaxiwayNames+Airside.Present.TaxiwayNames"` -> 0 failed. Then `./Tools/Check-Architecture.ps1` -> PASS (rules 40 and 4's scope rows).

- [ ] **Step 5: Commit**
```powershell
git add Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h Plugins/Airside/Source/Airside/Private/Model/RoadNetworkTaxiways.cpp `
  Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h Plugins/Airside/Source/Airside/Private/Present/RoadEditFacade.cpp `
  Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesTest.cpp Plugins/Airside/Source/AirsideTests/Private/TaxiwayNamesPresentTest.cpp
git commit -m "rename: taxiway rename with refusals, connectors follow, one undo step" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

---

### Task 14: The "Taxiway A" card with a Rename field

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/InspectFacts.h:181-196` (FTaxiwayCardFacts), `Plugins/Airside/Source/Airside/Private/Model/InspectFacts.cpp:358-387` (DescribeTaxiway)
- Modify: `Source/AirportMgr/InspectorCards.h:107` (FInspectorCardView field after `Locate`), `Source/AirportMgr/InspectorNetworkCards.cpp:152-182` (FTaxiwayCard::Compose)
- Modify: `Source/AirportMgr/InspectorWidget.h` (two BindWidgetOptional slots, handler, test seam, two members), `Source/AirportMgr/InspectorWidget.cpp` (BuildOnce bind, EnsureSlots, PaintView, OnNewSelection, handler)
- Test: `Source/AirportMgr/InspectorCardsTest.cpp`

**Interfaces:**
- Consumes: Task 13's `URoadEditFacade::RenameTaxiway`; `URoadNetwork::TaxiwayOf/TaxiwayDisplayName/TaxiwayChainOf/TaxiwayConnectorCount`; `ARoadNetworkActor::GetEditFacade() const` (RoadNetworkActor.h:338); `FSelectTool` + `FToolContext::SetCursor/BindSelection` (as SelectToolTest.cpp:80-90); `UEditableTextBox` (UMG, already a private dependency of AirportMgr.Build.cs:43).
- Produces: `FTaxiwayCardFacts::{ int32 Taxiway; FString Name; double Length; int32 Connectors; }`; `FInspectorCardView::RenameTaxiwayId`; `UInspectorWidget::RenameBox`, `RenameRefusalText`, `FString UInspectorWidget::SubmitRename(const FString&)`.

- [ ] **Step 1: Write the failing tests** (InspectorCardsTest.cpp; add includes `#include "Tool/SelectTool.h"`, `#include "Tool/RoadBuildTool.h"`, `#include "Present/RoadEditFacade.h"`)

```cpp
/** Spec seam test: Select-pick a taxiway and the card's title is its name; the card says its length and connectors and
 *  offers Rename. A raw-built (unnamed) network keeps the old title (plan D14) - AirportMgr.Inspector.Card.Taxiway. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorTaxiwayNamedTest, "AirportMgr.Inspector.Card.TaxiwayNamed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorTaxiwayNamedTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	const int32 Mid = Actor->PlaceNode({ 30000.0, 0.0 });
	TestTrue(TEXT("a taxiway"), Actor->ConnectNodes(Actor->PlaceNode({ -20000.0, 0.0 }), Mid, ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	TestTrue(TEXT("a stub"), Actor->ConnectNodes(Mid, Actor->PlaceNode({ 30000.0, -20000.0 }), ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));

	FSelectTool Tool;
	FSelection Selection;
	FToolContext Context;
	Context.Target = Actor;
	Context.SnapRadius = 400.0;
	FRoadSnapResult NoSnap;
	NoSnap.Position = FVector2D(5000.0, 500.0);
	Context.SetCursor(NoSnap.Position, NoSnap);
	Context.BindSelection(Selection);
	Tool.OnClick(Context);
	if (!TestTrue(TEXT("the click picked the taxiway"), Selection.Kind == ESelectionKind::Taxiway)) { return false; }

	FTaxiwayCard Card;
	FInspectorCardInput In;
	In.Target = Actor;
	In.Selection = Selection;
	const FInspectorCardView* View = Card.Describe(In);
	if (!TestNotNull(TEXT("a view"), View)) { return false; }
	TestEqual(TEXT("titled by its name"), View->Title, FString(TEXT("Taxiway A")));
	TestTrue(FString::Printf(TEXT("its length and connectors ('%s')"), *View->Facts),
		View->Facts.StartsWith(TEXT("Length 500 m, 1 connector")));
	TestEqual(TEXT("and it can be renamed"), View->RenameTaxiwayId, Actor->Network->TaxiwayOf(Actor->Network->SegmentIdAt(Selection.Id)));
	return true;
}

/** The card's Rename field, through the widget: a refusal is shown under it, a rename reaches the title, Undo puts it back. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorRenameTest, "AirportMgr.Inspector.RenameFromTheCard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorRenameTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	TestTrue(TEXT("a taxiway"), Actor->ConnectNodes(Actor->PlaceNode({ 0.0, 0.0 }), Actor->PlaceNode({ 60000.0, 0.0 }),
		ERoadKind::Taxiway, INDEX_NONE, EPavement::Tarmac));
	FSelection Selection;
	Selection.Kind = ESelectionKind::Taxiway;
	Selection.Id = Actor->Network->GetSegments().Num() - 1;
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel"), Panel)) { return false; }
	Panel->Refresh(Actor, Selection, nullptr);
	TestEqual(TEXT("titled A"), Panel->TitleForTest(), FString(TEXT("Taxiway A")));
	TestEqual(TEXT("a refusal is said"), Panel->SubmitRename(TEXT("O")), FString(TEXT("I, O and X are avoided: they read as 1, 0 and closed")));
	TestEqual(TEXT("a rename is not refused"), Panel->SubmitRename(TEXT("k")), FString());
	Panel->Refresh(Actor, Selection, nullptr);
	TestEqual(TEXT("the title follows"), Panel->TitleForTest(), FString(TEXT("Taxiway K")));
	TestTrue(TEXT("undo"), Actor->GetEditFacade()->Undo());
	Panel->Refresh(Actor, Selection, nullptr);
	TestEqual(TEXT("and undo puts A back"), Panel->TitleForTest(), FString(TEXT("Taxiway A")));
	return true;
}
```

- [ ] **Step 2: Build - expect a compile failure** (`'RenameTaxiwayId': is not a member of 'FInspectorCardView'`, `'SubmitRename'`).

- [ ] **Step 3: Implement**

InspectFacts.h FTaxiwayCardFacts, after `int32 Index = INDEX_NONE;`:
```cpp
	/** Its taxiway (URoadNetwork::TaxiwayOf) and name, "A3" - empty on an unnamed network (the card keeps its old title). */
	int32 Taxiway = INDEX_NONE;
	FString Name;
	/** The whole chain's length, uu, and how many connectors name it their parent. */
	double Length = 0.0;
	int32 Connectors = 0;
```
InspectFacts.cpp DescribeTaxiway, after `Out.Surface = Segment.Surface;`:
```cpp
		// THE NAME AND THE CHAIN (taxiway naming spec 2026-10-02): the card is the taxiway's, not just this segment's.
		Out.Taxiway = Network.TaxiwayOf(Id);
		Out.Name = Network.TaxiwayDisplayName(Out.Taxiway);
		Out.Length = Out.Taxiway != INDEX_NONE ? Network.TaxiwayChainOf(Out.Taxiway).Length : 0.0;
		Out.Connectors = Network.TaxiwayConnectorCount(Out.Taxiway);
```
InspectorCards.h FInspectorCardView, after `FAlertFocus Locate;`:
```cpp

	/**
	 * THE TAXIWAY CARD'S RENAME (taxiway naming spec, PIE only in v1): the taxiway the Rename field renames, INDEX_NONE
	 * on every other card, which collapses the field. ENFORCED BY: AirportMgr.Inspector.RenameFromTheCard
	 */
	int32 RenameTaxiwayId = INDEX_NONE;
```
InspectorNetworkCards.cpp FTaxiwayCard::Compose - replace the `Out.Title = ...` line and prepend the chain line to Facts:
```cpp
	// "TAXIWAY A" (spec): the name when there is one; an unnamed network (a raw-built fixture) keeps the segment index (plan D14).
	Out.Title = !T.Name.IsEmpty()
		? FString::Format(*NSLOCTEXT("AirportMgr", "InspectorTaxiwayNamed", "Taxiway {0}").ToString(), { T.Name })
		: FString::Format(*NSLOCTEXT("AirportMgr", "InspectorTaxiwayTitle", "Taxiway {0}").ToString(), { T.Index });
	const FString Chain = T.Name.IsEmpty() ? FString() : FString::Format(
		*NSLOCTEXT("AirportMgr", "InspectorTaxiwayChain", "Length {0} m, {1} connector(s)\n").ToString(),
		{ FString::Printf(TEXT("%.0f"), T.Length / 100.0), FString::FromInt(T.Connectors) });
```
and change the `Out.Facts = FString::Format(...)` assignment to `Out.Facts = Chain + FString::Format(...)` (same arguments). After `Out.Locate = ...;` add `Out.RenameTaxiwayId = T.Taxiway;`.
(The test expects `Length 500 m, 1 connector` as a prefix - "connector(s)" satisfies `StartsWith(TEXT("Length 500 m, 1 connector"))`.)

InspectorWidget.h - forward declare `class UEditableTextBox;`; after `LocateButton`'s UPROPERTY:
```cpp
	/** The taxiway card's Rename field (taxiway naming spec, PIE only in v1) - Enter submits. Collapsed on every other card. */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UEditableTextBox> RenameBox;
	/** Why the last rename was refused ("B is taken"), in Style->Warning; collapsed when it was not. */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> RenameRefusalText;

	/** Rename the shown taxiway to Requested through the facade (one undo step); returns the refusal, empty on success.
	 *  The Enter handler's body - public so a test drives it without Slate (memory: synthetic input never reaches Slate). */
	FString SubmitRename(const FString& Requested);
```
in the private section:
```cpp
	UFUNCTION() void HandleRenameCommitted(const FText& Text, ETextCommit::Type How);
	/** The taxiway and actor the painted card renames - set by PaintView. Weak: a level change can take the actor. */
	int32 RenameTaxiwayId = INDEX_NONE;
	TWeakObjectPtr<const ARoadNetworkActor> PaintedTarget;
```
InspectorWidget.cpp - `#include "Components/EditableTextBox.h"`, `#include "Present/RoadEditFacade.h"`. BuildOnce, beside the RunwayButton bind (line 42): `if (RenameBox != nullptr) { RenameBox->OnTextCommitted.AddDynamic(this, &UInspectorWidget::HandleRenameCommitted); }` - and because EnsureSlots builds the box, confirm BuildOnce's binds run AFTER EnsureSlots (they do for RunwayButton; keep the new line beside it). EnsureSlots, after `Text(StatusText, ...)`:
```cpp
	Text(RenameRefusalText, TEXT("RenameRefusalText"), EUITextRole::Body, Style->Warning);
	if (RenameBox == nullptr && Column != nullptr)
	{
		RenameBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("RenameBox"));
		RenameBox->SetHintText(NSLOCTEXT("AirportMgr", "InspectorRenameHint", "Rename (Enter)"));
		Column->AddChildToVerticalBox(RenameBox)->SetPadding(FMargin(0.0f, 2.0f));
	}
```
PaintView, after `LocateFocus = View.Locate;`:
```cpp
	RenameTaxiwayId = View.RenameTaxiwayId;
	PaintedTarget = &Target;
	const ESlateVisibility RenameShown = RenameTaxiwayId != INDEX_NONE ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
	if (RenameBox != nullptr && RenameBox->GetVisibility() != RenameShown) { RenameBox->SetVisibility(RenameShown); }
```
OnNewSelection, at its end:
```cpp
	// A REFUSAL BELONGS TO THE TAXIWAY IT WAS ABOUT.
	if (RenameRefusalText != nullptr) { RenameRefusalText->SetVisibility(ESlateVisibility::Collapsed); }
```
Append:
```cpp
void UInspectorWidget::HandleRenameCommitted(const FText& Text, ETextCommit::Type How)
{
	if (How == ETextCommit::OnEnter)
	{
		SubmitRename(Text.ToString());
	}
}

FString UInspectorWidget::SubmitRename(const FString& Requested)
{
	const ARoadNetworkActor* Target = PaintedTarget.Get();
	URoadEditFacade* Facade = Target != nullptr ? Target->GetEditFacade() : nullptr;
	FString Why;
	if (Facade == nullptr || RenameTaxiwayId == INDEX_NONE)
	{
		Why = TEXT("Nothing to rename");
	}
	else if (Facade->RenameTaxiway(RenameTaxiwayId, Requested, Why))
	{
		Why.Reset();
		if (RenameBox != nullptr) { RenameBox->SetText(FText::GetEmpty()); }
	}
	if (RenameRefusalText != nullptr)
	{
		RenameRefusalText->SetText(FText::FromString(Why));
		RenameRefusalText->SetVisibility(Why.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	}
	UE_LOG(LogInspector, Log, TEXT("Rename taxiway %d to '%s': %s"), RenameTaxiwayId, *Requested, Why.IsEmpty() ? TEXT("done") : *Why);
	return Why;
}
```

- [ ] **Step 4: Build, run**

`-Filter "AirportMgr.Inspector+Airside.Model.InspectFacts"`. Expected: 0 failed - `AirportMgr.Inspector.Card.Taxiway` still titles "Taxiway <index>" on its raw-built rig; `Check-Architecture.ps1` rule 49 (RefreshWith length) untouched - PASS.

- [ ] **Step 5: Commit**
```powershell
git add Plugins/Airside/Source/Airside/Public/Model/InspectFacts.h Plugins/Airside/Source/Airside/Private/Model/InspectFacts.cpp `
  Source/AirportMgr/InspectorCards.h Source/AirportMgr/InspectorNetworkCards.cpp Source/AirportMgr/InspectorWidget.h `
  Source/AirportMgr/InspectorWidget.cpp Source/AirportMgr/InspectorCardsTest.cpp
git commit -m "inspector: Taxiway A card - length, connectors, Rename field" -m "Claude-Session: https://claude.ai/code/session_01SEWjLjMNao3tq4Cmfb3tBp"
```

### PR 3 boundary and the spec's PIE check

- [ ] Full suite at the tip; push; `gh pr create --base feature/taxiway-labels --title "Taxiway names: rename from the card"`.
- [ ] PIE check (spec): open `M_ScaleGatwick`; read `Saved/Logs/AirportMgr.log` for `TaxiwayNames: backfilled N taxiway(s), M connector(s)` and `Load repairs on ...: ... taxiway name(s)`; labels appear (`Mcp.py shot out.png editor` in PIE); the parallels read as letters and the stand links as connectors; select a taxiway, rename it (typing letters into the field must NOT fire build hotkeys - if it does, that is a focus defect to fix before merge); delete a middle segment and see the toast and `TaxiwayNames: X split off from Y` in the log; press G while watching and the names hide.
- [ ] Merge order (memory: stacked squash merges land in the base branch): merge PR 1 to main, retarget PR 2 to main, rebase, re-test, merge; same for PR 3.
