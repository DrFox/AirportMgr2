# Snap guides, stage 3: the toggles and the suspend

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The player decides which guide sources are live, from a new `SNAP` section on the
build bar, and can suspend all of them for one drag by holding Alt.

**Architecture:** Enabled state is a reflected `USTRUCT` of named bools on
`ARoadNetworkActor`, beside `Snap`, so PIE and the editor mode agree about what is switched
on. `FSnapGuideChain` skips a disabled source before it does any work, which needs
`IGuideSource` to say which `ESource` it speaks for. The suspend is a modifier on
`FToolContext`, filled by both drivers the same way `bRemoveModifier` already is - not a
registry action, because the registry binds presses and this is a hold.

**Tech Stack:** UE 5.8.2 C++, UMG, UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-17-snap-guides-design.md` - stage 3 of §8, i.e. §7
in full plus §5's rule 5 (suspension).

**Stacked on:** `feature/snap-guides-network` (PR #147), itself stacked on
`feature/snap-guides` (PR #146). Neither is merged. This branch is
`feature/snap-guides-toggles` and its PR targets `feature/snap-guides-network`.

## Global Constraints

- **Worktree.** Every command runs from `C:\repos\AirportMgr2_snap-toggles`. Do not `cd` to
  `C:\repos\AirportMgr2` (unrelated uncommitted work), nor to the two worktrees holding PRs
  #146 and #147 open for review.
- **Build.**

  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2_snap-toggles\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE
  ```

  Check the literal line `Result: Succeeded`. **It exits 0 even when it fails** - a build
  colliding with a running test editor reports `Result: Failed (OtherCompilationError)` and
  exit code 0. That happened three times across stages 1 and 2. Never read the exit code.
- **Tests.**

  ```
  ./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-toggles\AirportMgr.uproject"
  ```

  Read the `N test(s) run, N failed, N crashed` line; **crashed must be 0**. A crashing test
  is reported by the runner's started-vs-completed diff and by nothing else - stage 2 had one,
  and its exit code was 0 too. **Baseline is 371**; record the real number before Task 1.
- **A new `.cpp` sometimes needs two builds** before it compiles; the first says
  `Result: Succeeded` without having compiled it.
- **This module is a UNITY build.** Two `static`/anonymous-namespace helpers of one name in
  two `.cpp` files compile alone and collide once they land in the same blob - stage 2 hit
  this with `SegmentEnds`. Prefix a new file-local helper if its name is at all common.
- **`Solve/` includes `CoreMinimal.h` and `Solve/` only**; `Tool/` may include `Solve/` and
  `Model/` but never `Present/`. `Check-Architecture.ps1` enforces it and runs first.
- **Never `git checkout --` a file whose task is not yet committed** - it reverts to HEAD and
  in stage 1 that discarded a whole task's work.
- **No string replacement without asserting it matched exactly once.**
- **Commits:** no `Co-Authored-By` trailer.

## What stages 1 and 2 already provide

```cpp
// Solve/GuideArbiter.h  - a PLAIN enum: UHT cannot see it, and Solve/ has no .generated.h
namespace SnapGuide
{
    enum class ESource : uint8
    { Extending, PointAlign, Aligned, Collinear, Parallel, Runway, World, Offset };
    enum class EFit : uint8 { Angular, Perpendicular };
    struct FCandidate { FVector2D Direction, Through, ReferenceAt; EFit Fit; double Distance;
                        FString Description; ESource Source; };
    struct FTuning { double ToleranceDegrees, StickinessDegrees, ToleranceUu, StickinessUu,
                            MaxPullUu, SearchRadiusUu; };
    struct FResult { bool bActive; TArray<FCandidate, TInlineAllocator<2>> Winners;
                     FVector2D Point; const FCandidate* Of(EFit) const; };
    AIRSIDE_API FResult Arbitrate(TConstArrayView<FCandidate>, const FVector2D& Origin,
        const FVector2D& Cursor, const FResult& Previous, const FTuning& = FTuning());
}

// Tool/SnapGuideChain.h
struct AIRSIDE_API IGuideSource
{ virtual void Propose(const URoadNetwork&, const FGuideAnchor&,
      TArray<SnapGuide::FCandidate>&) const = 0; };
// seven concrete sources: FExtendingGuideSource, FPointAlignGuideSource, FAlignedGuideSource,
// FCollinearGuideSource, FParallelGuideSource, FRunwayGuideSource, FWorldGuideSource
class AIRSIDE_API FSnapGuideChain
{ public: FSnapGuideChain(); void AddSource(TUniquePtr<IGuideSource>); int32 NumSources() const;
    SnapGuide::FResult Resolve(const URoadNetwork&, const FGuideAnchor&, const FVector2D& Cursor,
        const SnapGuide::FResult& Previous, const SnapGuide::FTuning& = SnapGuide::FTuning()) const; };

// Tool/BuildSession.h
FToolContext FBuildSession::MakeContext(IRoadEditTarget* Target, const FVector2D& PlaneHit,
    const FBuildSessionTunables& Tunables, bool bRemoveModifier, bool bInsertModifier,
    int32 HoverAgent = 0) const;    // holds `mutable SnapGuide::FResult LastGuide`
```

## Model and UI API this stage calls (verified against the headers on this branch)

```cpp
// Source/AirportMgr/BuildActions.h
enum class EActionSection : uint8 { Time, Tools, Edit, Aircraft, Selection, Game, Count };
const TCHAR* ActionSectionName(EActionSection Section);     // indexes SectionNames[]
struct FBuildAction { FName Id; EActionSection Section; FText Label; FKey Key;
    bool bRequiresCtrl; TFunction<void(ARoadBuildController&)> Execute;
    TFunction<bool(const ARoadBuildController&)> IsActive;
    TFunction<bool(const ARoadBuildController&)> IsEnabled; };
const FBuildAction* FindAction(FName Id);

// Source/AirportMgr/BuildBarWidget.h - one UPanelWidget per section
UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> EditSection;   // and five more

// Present/RoadNetworkActor.h
UPROPERTY(EditAnywhere, Category = "Airside|Placement") FRoadSnapSettings Snap;
FBuildSessionTunables MakeTunables(double ViewWorldWidth);

// The editor mode registers modifiers by id and is told about them:
//   Plugins/Airside/Source/AirsideEditor/Public/RoadBuildEditorTool.h:109
static const int32 RemoveModifierId = 1;    // InsertModifierId = 2
virtual void OnUpdateModifierState(int ModifierID, bool bIsOn) override;
//   ...Private/RoadBuildEditorTool.cpp:183
Drag->Modifiers.RegisterModifier(RemoveModifierId, FInputDeviceState::IsCtrlKeyDown);
// FInputDeviceState::IsAltKeyDown exists: Engine/Source/Runtime/InteractiveToolsFramework/
//   Public/InputState.h:374 - VERIFIED, not assumed.

// ARoadBuildController reads held keys directly:
IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl)
```

## Decisions this plan takes

1. **Named bools in a `USTRUCT`, not an array indexed by `ESource`.** `ESource` is a plain
   enum in a `Solve/` header with no `.generated.h`, so UHT cannot see it and a
   `TArray<bool>` keyed by it would serialise BY INDEX - reordering the enum would silently
   repoint every saved toggle. Named bools are reflected, show up in the details panel with
   their own tooltips, and survive a reorder. ONE switch maps `ESource` to its bool, and the
   registry test walks the enum against it so a source added without a toggle fails.
2. **Eight toggles, not §7's seven.** §7 was written before `PointAlign` existed. It defaults
   ON, with `Extending`, `Parallel` and `World`, because it is the gesture's own geometry -
   the same argument that puts `Extending` on.
3. **The chain skips a disabled source rather than filtering its candidates.** `Collinear`
   walks every segment in reach; doing that work and discarding it is waste, and filtering
   afterwards loses which source did it. `IGuideSource` gains `Kind()`.
4. **The suspend is a context modifier, not a registry action.** §7 says so, and the reason is
   mechanical: the registry binds PRESSES and this is a HOLD. It rides beside
   `bRemoveModifier` in `MakeContext`, which is the one place both drivers already agree.
5. **The editor mode must REGISTER an Alt modifier.** It does not read keys directly - it is
   told about modifier ids it registered. That is a cost §7 did not anticipate and it is why
   Task 2 touches four call sites in that file rather than one.

## File Structure

| File | Responsibility |
|---|---|
| `Public/Tool/SnapGuideSettings.h` (new) | `FSnapGuideSettings`: eight named bools, `IsEnabled(ESource)`, `Toggle(ESource)`. Has its own `.generated.h`. |
| `Private/Tool/SnapGuideSettings.cpp` (new) | The one `ESource` -> bool switch. |
| `Public/Tool/SnapGuideChain.h` | `IGuideSource::Kind()`; `Resolve` takes the settings. |
| `Private/Tool/SnapGuideChain.cpp` | Eight `Kind()` bodies; the skip. |
| `Public/Present/RoadNetworkActor.h` | `FSnapGuideSettings GuideSources` beside `Snap`. |
| `Public/Tool/BuildSession.h` + `.cpp` | `FBuildSessionTunables::GuideSources`; `MakeContext` gains `bSuspendGuides`. |
| `Public/Tool/RoadBuildTool.h` | `FToolContext::bSuspendGuides`. |
| `Source/AirportMgr/RoadBuildController.cpp` | Reads Alt. |
| `AirsideEditor/...RoadBuildEditorTool.h` + `.cpp` | `SuspendModifierId = 3`, registered twice, read once. |
| `Source/AirportMgr/BuildActions.h` + `.cpp` | `EActionSection::Snap`, its name, eight actions. |
| `Source/AirportMgr/BuildBarWidget.h` + `.cpp` | `SnapSection` panel. |
| `Source/AirportMgr/BuildActionsTest.cpp` | The enum-walked registry test. |
| `AirsideTests/Private/GuideToggleTest.cpp` (new) | Settings, the skip, and the suspend. |

---

### Task 1: The settings, and a chain that skips what is off

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideSettings.h`
- Create: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideSettings.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/GuideToggleTest.cpp`

**Interfaces:**
- Consumes: `SnapGuide::ESource`, `IGuideSource`, `FSnapGuideChain::Resolve` (stages 1-2).
- Produces: `USTRUCT FSnapGuideSettings` with eight `bool` UPROPERTYs
  (`bExtending`, `bPointAlign`, `bAligned`, `bCollinear`, `bParallel`, `bRunway`, `bWorld`,
  `bOffset`), `bool IsEnabled(SnapGuide::ESource) const`, `void Toggle(SnapGuide::ESource)`;
  `virtual SnapGuide::ESource IGuideSource::Kind() const = 0`;
  `FSnapGuideChain::Resolve(..., const FSnapGuideSettings& Sources, ...)`.

- [ ] **Step 1: Record the baseline**

Run the full test line. Expected `371 test(s) run, 0 failed, 0 crashed`. Write the number down.

- [ ] **Step 2: Write the settings header**

Create `Public/Tool/SnapGuideSettings.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Solve/GuideArbiter.h"
#include "SnapGuideSettings.generated.h"

/**
 * Which guide sources are live, per airport.
 *
 * BESIDE ARoadNetworkActor::Snap and for the same recorded reason as FRoadSnapSettings: the
 * editor mode and PIE must agree about what is switched on, and a per-driver copy is how the
 * two came to disagree about snap radii before issue #93 merged them.
 *
 * NAMED BOOLS, NOT AN ARRAY INDEXED BY ESource. SnapGuide::ESource is a plain enum in a
 * Solve/ header with no .generated.h - UHT cannot see it - so a TArray<bool> keyed by it
 * would serialise BY INDEX, and reordering the enum would silently repoint every toggle a
 * player had set. These are reflected, appear in the details panel with their own tooltips,
 * and survive a reorder. The cost is one switch in IsEnabled, which the registry test walks
 * the enum against.
 *
 * INDEPENDENT FLAGS, so bools are right here - CLAUDE.md's "a phase is an enum, never a set
 * of bools" is about states that cannot both be true, and any combination of these can.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FSnapGuideSettings
{
	GENERATED_BODY()

	/** The edge the gesture is already extending, and its perpendicular. */
	UPROPERTY(EditAnywhere) bool bExtending = true;

	/** Lines through the gesture's own pinned corners. On with Extending: same geometry. */
	UPROPERTY(EditAnywhere) bool bPointAlign = true;

	/** A placed entity's pose direction. */
	UPROPERTY(EditAnywhere) bool bAligned = false;

	/** The line an existing segment already lies on. */
	UPROPERTY(EditAnywhere) bool bCollinear = false;

	/** The nearest road's direction. */
	UPROPERTY(EditAnywhere) bool bParallel = true;

	/** Every runway's heading. */
	UPROPERTY(EditAnywhere) bool bRunway = false;

	/** 0/45/90/135 degrees. */
	UPROPERTY(EditAnywhere) bool bWorld = true;

	/** The gap a neighbouring parallel road keeps. Nothing proposes this until stage 4. */
	UPROPERTY(EditAnywhere) bool bOffset = false;

	/** Whether this source may propose at all. The ONE mapping from the enum to these flags. */
	bool IsEnabled(SnapGuide::ESource Source) const;

	/** Flips one. What the bar button does. */
	void Toggle(SnapGuide::ESource Source);
};
```

- [ ] **Step 3: Write the one switch**

Create `Private/Tool/SnapGuideSettings.cpp`:

```cpp
#include "Tool/SnapGuideSettings.h"

bool FSnapGuideSettings::IsEnabled(SnapGuide::ESource Source) const
{
	// NO `default:`. This project does not build switches as exhaustive-or-error, so a source
	// added to ESource without a case here would silently fall past the switch - which is why
	// the return below is `false` and AirportMgr.Actions.SnapTogglesAreInTheRegistry walks the
	// enum: a source with no toggle is OFF and has no button, and the test says so out loud.
	switch (Source)
	{
	case SnapGuide::ESource::Extending:  return bExtending;
	case SnapGuide::ESource::PointAlign: return bPointAlign;
	case SnapGuide::ESource::Aligned:    return bAligned;
	case SnapGuide::ESource::Collinear:  return bCollinear;
	case SnapGuide::ESource::Parallel:   return bParallel;
	case SnapGuide::ESource::Runway:     return bRunway;
	case SnapGuide::ESource::World:      return bWorld;
	case SnapGuide::ESource::Offset:     return bOffset;
	}
	return false;
}

void FSnapGuideSettings::Toggle(SnapGuide::ESource Source)
{
	switch (Source)
	{
	case SnapGuide::ESource::Extending:  bExtending  = !bExtending;  return;
	case SnapGuide::ESource::PointAlign: bPointAlign = !bPointAlign; return;
	case SnapGuide::ESource::Aligned:    bAligned    = !bAligned;    return;
	case SnapGuide::ESource::Collinear:  bCollinear  = !bCollinear;  return;
	case SnapGuide::ESource::Parallel:   bParallel   = !bParallel;   return;
	case SnapGuide::ESource::Runway:     bRunway     = !bRunway;     return;
	case SnapGuide::ESource::World:      bWorld      = !bWorld;      return;
	case SnapGuide::ESource::Offset:     bOffset     = !bOffset;     return;
	}
}
```

- [ ] **Step 4: Make every source say what it speaks for**

In `Public/Tool/SnapGuideChain.h`, add `#include "Tool/SnapGuideSettings.h"` to the includes,
and to `IGuideSource`:

```cpp
	/**
	 * Which ESource this link proposes. The toggle asks, and the chain skips it when off.
	 *
	 * PURE VIRTUAL rather than a field, so a source cannot be written without answering. Every
	 * source proposes candidates of exactly ONE ESource today; if one ever proposes two, this
	 * is the assumption to revisit rather than quietly widen.
	 */
	virtual SnapGuide::ESource Kind() const = 0;
```

Then add one line to each of the seven concrete sources' declarations, inside the struct body:

```cpp
	virtual SnapGuide::ESource Kind() const override { return SnapGuide::ESource::Extending; }
```

with `Extending`, `PointAlign`, `Aligned`, `Collinear`, `Parallel`, `Runway` and `World`
respectively - matching the `Source` each one already stamps on its candidates. Getting one
wrong makes a toggle control the wrong guide, which is what Task 1's test checks.

And change `Resolve`'s declaration to take the settings:

```cpp
	/**
	 * Every ENABLED source's candidates, arbitrated, with Previous carrying the flicker rule.
	 *
	 * Sources is taken by value-reference rather than stored: the chain is shared and the
	 * settings live on the airport, so a chain holding its own copy would be a second place
	 * for them to drift - the exact failure FRoadSnapSettings records.
	 */
	SnapGuide::FResult Resolve(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, const SnapGuide::FResult& Previous,
		const FSnapGuideSettings& Sources = FSnapGuideSettings(),
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const;
```

- [ ] **Step 5: Skip what is off**

In `Private/Tool/SnapGuideChain.cpp`, replace the body of `FSnapGuideChain::Resolve`.

**The parameter is called `Enabled`, not `Sources`:** the chain's own member holding the links
is already called `Sources`, and a parameter of that name would shadow it - the loop would then
iterate the settings struct, which does not compile, or worse would compile against something
that did.

```cpp
SnapGuide::FResult FSnapGuideChain::Resolve(const URoadNetwork& Network,
	const FGuideAnchor& Anchor, const FVector2D& Cursor,
	const SnapGuide::FResult& Previous, const FSnapGuideSettings& Enabled,
	const SnapGuide::FTuning& Tuning) const
{
	TArray<SnapGuide::FCandidate> Candidates;
	Candidates.Reserve(16);

	for (const TUniquePtr<IGuideSource>& Source : Sources)
	{
		// SKIPPED BEFORE IT WORKS, not filtered after. Collinear walks every segment in reach;
		// doing that and discarding the result is waste, and filtering candidates afterwards
		// would lose which source had done the work.
		if (Enabled.IsEnabled(Source->Kind()))
		{
			Source->Propose(Network, Anchor, Candidates);
		}
	}

	return SnapGuide::Arbitrate(Candidates, Anchor.Origin, Cursor, Previous, Tuning);
}
```

Change the declaration in the header to name the parameter `Enabled` too.

- [ ] **Step 6: Write the failing test**

Create `Plugins/Airside/Source/AirsideTests/Private/GuideToggleTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** Every value of ESource, so a test can walk the enum rather than list it. */
	const TArray<SnapGuide::ESource>& EverySource()
	{
		static const TArray<SnapGuide::ESource> All = {
			SnapGuide::ESource::Extending, SnapGuide::ESource::PointAlign,
			SnapGuide::ESource::Aligned,   SnapGuide::ESource::Collinear,
			SnapGuide::ESource::Parallel,  SnapGuide::ESource::Runway,
			SnapGuide::ESource::World,     SnapGuide::ESource::Offset };
		return All;
	}
}

/**
 * EVERY SOURCE HAS A FLAG, AND NO TWO SHARE ONE. A switch with a case missing returns false
 * for that source, which reads on screen as a guide that simply never fires - and a case
 * copied from its neighbour makes one button control two guides. Both are silent.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideSettingsGiveEverySourceItsOwnFlagTest,
	"Airside.Tool.GuideSettingsGiveEverySourceItsOwnFlag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideSettingsGiveEverySourceItsOwnFlagTest::RunTest(const FString& Parameters)
{
	for (const SnapGuide::ESource Source : EverySource())
	{
		// TOGGLING ONE MUST MOVE THAT ONE AND NOTHING ELSE. Starting from a fresh struct each
		// time so the defaults, whatever they are, cannot mask a shared flag.
		FSnapGuideSettings Settings;
		const bool Before = Settings.IsEnabled(Source);

		Settings.Toggle(Source);
		TestNotEqual(
			*FString::Printf(TEXT("source %d has a flag Toggle actually moves"),
				static_cast<int32>(Source)),
			Settings.IsEnabled(Source), Before);

		for (const SnapGuide::ESource Other : EverySource())
		{
			if (Other == Source) { continue; }
			TestEqual(
				*FString::Printf(TEXT("and toggling %d leaves %d alone"),
					static_cast<int32>(Source), static_cast<int32>(Other)),
				Settings.IsEnabled(Other), FSnapGuideSettings().IsEnabled(Other));
		}
	}

	// THE DEFAULTS THE DESIGN ASKED FOR (§7, amended for PointAlign in stage 3): the three that
	// fire most often are on, and a player meeting every guide at once learns nothing.
	const FSnapGuideSettings Defaults;
	TestTrue(TEXT("Extending is on by default"),
		Defaults.IsEnabled(SnapGuide::ESource::Extending));
	TestTrue(TEXT("PointAlign is on, being the gesture's own geometry"),
		Defaults.IsEnabled(SnapGuide::ESource::PointAlign));
	TestTrue(TEXT("Parallel is on"), Defaults.IsEnabled(SnapGuide::ESource::Parallel));
	TestTrue(TEXT("World is on"), Defaults.IsEnabled(SnapGuide::ESource::World));
	TestFalse(TEXT("Aligned is off"), Defaults.IsEnabled(SnapGuide::ESource::Aligned));
	TestFalse(TEXT("Collinear is off"), Defaults.IsEnabled(SnapGuide::ESource::Collinear));
	TestFalse(TEXT("Runway is off"), Defaults.IsEnabled(SnapGuide::ESource::Runway));

	return true;
}

/**
 * A SOURCE THAT IS OFF PROPOSES NOTHING, and the guide the player was getting from it stops.
 * Driven through the chain rather than the settings, because the settings agreeing with
 * themselves proves nothing about whether anything reads them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideChainSkipsADisabledSourceTest,
	"Airside.Tool.GuideChainSkipsADisabledSource",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideChainSkipsADisabledSourceTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	// An east-west taxiway, and a drag beside it running almost along it.
	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(-10000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(10000.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::Taxiway, INDEX_NONE);
	if (!TestTrue(TEXT("the network exists"), Actor->Network != nullptr)) { return false; }

	FGuideAnchor Anchor;
	Anchor.Origin = FVector2D(0.0, 2000.0);
	const FVector2D Cursor(3000.0, 2100.0);

	const FSnapGuideChain Chain;

	// PARALLEL ON: the road answers.
	FSnapGuideSettings Settings;
	Settings.bParallel = true;
	Settings.bWorld = false;
	Settings.bExtending = false;
	Settings.bPointAlign = false;

	const SnapGuide::FResult On = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	if (!TestTrue(TEXT("with Parallel on, the nearest road offers a guide"), On.bActive))
	{
		return false;
	}
	TestEqual(TEXT("and it is Parallel that offered it"),
		static_cast<int32>(On.Winners[0].Source),
		static_cast<int32>(SnapGuide::ESource::Parallel));

	// PARALLEL OFF: nothing else is on, so nothing answers at all. That is the assertion the
	// whole toggle exists for.
	Settings.bParallel = false;
	const SnapGuide::FResult Off = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	TestFalse(TEXT("with Parallel off, the same drag is offered nothing"), Off.bActive);

	// CONTROL LEG: the drag itself was fine - turn World back on and a guide returns. Without
	// this, a Resolve that had simply broken would pass the assertion above.
	Settings.bWorld = true;
	const SnapGuide::FResult World = Chain.Resolve(
		*Actor->Network, Anchor, Cursor, SnapGuide::FResult(), Settings);
	TestTrue(TEXT("and another source switched on still answers"), World.bActive);
	TestEqual(TEXT("from the world grid this time"),
		static_cast<int32>(World.Winners[0].Source),
		static_cast<int32>(SnapGuide::ESource::World));

	return true;
}

#endif
```

- [ ] **Step 7: Build**

Expect `Result: Succeeded`. A new `USTRUCT` means UHT runs. New `.cpp` files: if the tests do
not appear in step 8, build again.

- [ ] **Step 8: Run the new tests**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-toggles\AirportMgr.uproject" -Filter Airside.Tool.Guide
```

Expected: every `Airside.Tool.Guide*` test passes, including the two new ones.

- [ ] **Step 9: Run the whole suite**

Expected: baseline + 2, 0 failed, 0 crashed. **Watch every existing guide test**: `Resolve`
gained a parameter with a default, so callers still compile, and the default
`FSnapGuideSettings()` has `Aligned`, `Collinear` and `Runway` OFF. Stage 2's tests that call
those sources DIRECTLY are unaffected, but any that go through the chain and expect a
Collinear or Runway winner will now fail - and the right fix is to pass a settings struct with
that source on, not to change the defaults.

- [ ] **Step 10: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/SnapGuideSettings.h Plugins/Airside/Source/Airside/Private/Tool/SnapGuideSettings.cpp Plugins/Airside/Source/Airside/Public/Tool/SnapGuideChain.h Plugins/Airside/Source/Airside/Private/Tool/SnapGuideChain.cpp Plugins/Airside/Source/AirsideTests/Private/GuideToggleTest.cpp
git commit -m "feat(tool): a guide source can be switched off, and the chain skips it"
```

- [ ] **Step 11: Prove the skip is real**

Change `Enabled.IsEnabled(Source->Kind())` to `true` in `Resolve`. Build. Run
`-Filter Airside.Tool.GuideChainSkipsADisabledSource`. Expected: `1 test(s) run, 1 failed` on
"with Parallel off, the same drag is offered nothing". Restore the line by hand - **not with
`git checkout --`**, which would take the whole file back to HEAD. Rebuild and rerun; expected
`0 failed`.

---

### Task 2: Alt suspends every guide for one drag

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h`
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h`
- Modify: `Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp`
- Modify: `Source/AirportMgr/RoadBuildController.cpp`
- Modify: `Plugins/Airside/Source/AirsideEditor/Public/RoadBuildEditorTool.h`
- Modify: `Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEditorTool.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/GuideToggleTest.cpp`

**Interfaces:**
- Consumes: `FSnapGuideSettings` (Task 1); `FBuildSession::MakeContext`.
- Produces: `bool FToolContext::bSuspendGuides`; `MakeContext(..., bool bRemoveModifier,
  bool bInsertModifier, bool bSuspendGuides, int32 HoverAgent = 0)`;
  `URoadBuildEditorTool::SuspendModifierId = 3`.

- [ ] **Step 1: Add the context flag**

In `Public/Tool/RoadBuildTool.h`, after `bInsertModifier`:

```cpp

	/**
	 * Alt: the player wants NO guide for this drag.
	 *
	 * A HELD MODIFIER, not a registry action, and the distinction is mechanical rather than
	 * stylistic: the action registry binds PRESSES, and this has to be true only while the key
	 * is down. Design §7.
	 *
	 * Toggles are for "I never want this"; this is for "not for this one drag", and without it
	 * the player fights the guide for a position it will not give them.
	 */
	bool bSuspendGuides = false;
```

- [ ] **Step 2: Carry it through the session**

In `Public/Tool/BuildSession.h`, change `MakeContext`'s declaration to:

```cpp
	FToolContext MakeContext(IRoadEditTarget* Target, const FVector2D& PlaneHit,
		const FBuildSessionTunables& Tunables, bool bRemoveModifier, bool bInsertModifier,
		bool bSuspendGuides = false, int32 HoverAgent = 0) const;
```

**The default matters:** `HoverAgent` already had one, and inserting a parameter before it
would silently repoint every two-argument call. Defaulting `bSuspendGuides` to false keeps
existing callers compiling AND behaving as they did - and the tests in Task 1 pass nothing,
so they keep testing the unsuspended path.

In `Private/Tool/BuildSession.cpp`, in `MakeContext`, add the parameter to the signature and
set it beside the other modifiers:

```cpp
	Context.bSuspendGuides = bSuspendGuides;
```

and gate the guide resolution on it:

```cpp
	FGuideAnchor Anchor;
	SnapGuide::FResult Guide;
	const IBuildTool* Tool = GetActiveTool();
	if (!bSuspendGuides && Tool != nullptr && Network != nullptr
		&& Tool->DescribeGuideAnchor(Anchor))
	{
		Guide = GuideChain.Resolve(*Network, Anchor, PlaneHit, LastGuide, Tunables.GuideSources);
	}
```

**Suspending must also CLEAR the remembered winner**, which the existing `LastGuide = Guide;`
below already does - releasing Alt therefore starts the guide afresh rather than resuming a
held winner from before the suspend. Leave that line where it is and add nothing.

- [ ] **Step 3: Give the tunables the settings**

In `Public/Tool/BuildSession.h`, in `FBuildSessionTunables`, after `Snap`:

```cpp
	/** Which guide sources are live. From ARoadNetworkActor, like Snap above. */
	FSnapGuideSettings GuideSources;
```

and add `#include "Tool/SnapGuideSettings.h"` to that header's includes.

In `Public/Present/RoadNetworkActor.h`, after the `FRoadSnapSettings Snap;` UPROPERTY:

```cpp
	/**
	 * Which guide sources the player has switched on. PER AIRPORT, beside Snap and for the
	 * same reason it is: the editor mode and PIE must agree about what is live.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Placement")
	FSnapGuideSettings GuideSources;
```

and in `ARoadNetworkActor::MakeTunables`, beside where it fills `Tunables.Snap`:

```cpp
	Tunables.GuideSources = GuideSources;
```

- [ ] **Step 4: Read Alt in the runtime driver**

In `Source/AirportMgr/RoadBuildController.cpp`, in `MakeToolContext`, find the
`Session.MakeContext(...)` call and add the Alt read as the new argument, before `HoverAgent`:

```cpp
		IsInputKeyDown(EKeys::LeftAlt) || IsInputKeyDown(EKeys::RightAlt),
```

matching how Ctrl is read two lines above it. Confirm the call site by reading it first - the
argument order is Target, PlaneHit, Tunables, bRemove, bInsert, bSuspendGuides, HoverAgent,
and putting the bool in the wrong slot compiles fine and suspends on Shift.

- [ ] **Step 5: Register Alt in the editor driver**

The editor mode does NOT read keys - it registers modifier ids and is told when they change.
In `Public/RoadBuildEditorTool.h`, beside `RemoveModifierId`:

```cpp
	/** Alt: suspend every guide for this drag. Third because Remove is 1 and Insert is 2. */
	static const int32 SuspendModifierId = 3;
```

and beside `bRemoveHeld`:

```cpp
	bool bSuspendHeld = false;
```

In `Private/RoadBuildEditorTool.cpp`, register it on BOTH behaviours, beside the existing two
pairs at roughly lines 183 and 191:

```cpp
	Drag->Modifiers.RegisterModifier(SuspendModifierId, FInputDeviceState::IsAltKeyDown);
```

```cpp
	Hover->Modifiers.RegisterModifier(SuspendModifierId, FInputDeviceState::IsAltKeyDown);
```

**BOTH, and that is the point of naming the line numbers:** registering only on Drag would
suspend while dragging and not while hovering, so the ghost would show a guide the click then
ignored. In `OnUpdateModifierState`:

```cpp
	if (ModifierID == SuspendModifierId) { bSuspendHeld = bIsOn; }
```

and in `MakeContextAt`, pass it:

```cpp
	return Sess().MakeContext(Target, Plane, Tunables, bRemoveHeld, bInsertHeld, bSuspendHeld);
```

- [ ] **Step 6: Write the failing test**

Append to `GuideToggleTest.cpp`, before the final `#endif`. It needs
`#include "Tool/BuildSession.h"` and `#include "Tool/PlotPlaceTool.h"` added to the file's
includes.

```cpp
/**
 * ALT MEANS NOT THIS TIME. Toggles are for "I never want this"; without a hold the player
 * fights the guide for a position it will not give them.
 *
 * Driven through FBuildSession::MakeContext, because that is where both drivers meet and the
 * only place the flag can be proved to reach the resolution.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGuideSuspendsOnHoldTest,
	"Airside.Tool.GuideSuspendsOnHold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGuideSuspendsOnHoldTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("a network actor"), Actor)) { return false; }

	IRoadEditTarget* Target = Actor;
	const int32 West = Target->PlaceNode(FVector2D(-20000.0, 0.0));
	const int32 East = Target->PlaceNode(FVector2D(20000.0, 0.0));
	Target->ConnectNodes(West, East, ERoadKind::ServiceRoad, INDEX_NONE);

	// The depot tool, found by id - never by a literal index, which the next tool added moves.
	int32 Depot = INDEX_NONE;
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
	for (int32 Index = 0; Index < Registry.Num(); ++Index)
	{
		if (Registry[Index].Id == FName(TEXT("FuelDepot"))) { Depot = Index; }
	}
	if (!TestTrue(TEXT("the registry lists a fuel-depot tool"), Depot != INDEX_NONE))
	{
		return false;
	}

	FBuildSession Session;
	const FBuildSessionTunables Tunables = Actor->MakeTunables(10000.0);
	Session.SelectTool(Depot);
	IBuildTool* Tool = Session.GetActiveTool();
	if (!TestNotNull(TEXT("the depot tool is active"), Tool)) { return false; }

	// Anchor beside the road and run the frontage east, so a back corner has a guide to get.
	Tool->OnClick(Session.MakeContext(Actor, FVector2D(0.0, 1000.0), Tunables, false, false));
	Tool->OnClick(Session.MakeContext(Actor, FVector2D(6000.0, 1000.0), Tunables, false, false));

	TArray<FVector2D> Frontage;
	static_cast<FPlotPlaceTool*>(Tool)->Quad(
		Session.MakeContext(Actor, FVector2D(6000.0, 3000.0), Tunables, false, false), Frontage);
	if (!TestTrue(TEXT("two corners are pinned"), Frontage.Num() >= 2)) { return false; }

	// A corner dragged near square: with Alt up this is exactly the case stage 1 guides.
	const FVector2D NearSquare = Frontage[1] + FVector2D(60.0, 2000.0);

	const FToolContext Free = Session.MakeContext(
		Actor, NearSquare, Tunables, false, false, false);
	if (!TestTrue(TEXT("with Alt up, the corner is guided"), Free.Guide.bActive))
	{
		return false;
	}

	const FToolContext Held = Session.MakeContext(
		Actor, NearSquare, Tunables, false, false, true);
	TestFalse(TEXT("with Alt held, the same drag is offered nothing"), Held.Guide.bActive);
	TestTrue(TEXT("and the raw cursor is what the tool would use"),
		Held.GuidedCursor().Equals(NearSquare, 1.0e-6));

	// RELEASING ALT STARTS AFRESH rather than resuming the winner it was holding - the
	// suspended frame cleared it, so this is the hysteresis rule being fed an empty previous.
	const FToolContext Released = Session.MakeContext(
		Actor, NearSquare, Tunables, false, false, false);
	TestTrue(TEXT("and releasing Alt gives the guide back"), Released.Guide.bActive);

	return true;
}
```

- [ ] **Step 7: Build**

Expect `Result: Succeeded`. `FToolContext` gained a member and `MakeContext` a parameter, so
this is a full build, not Live Coding.

- [ ] **Step 8: Run the new test**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-toggles\AirportMgr.uproject" -Filter Airside.Tool.GuideSuspendsOnHold
```

Expected: `1 test(s) run, 0 failed, 0 crashed`.

- [ ] **Step 9: Run the whole suite**

Expected: baseline + 3, 0 failed, 0 crashed.

- [ ] **Step 10: Commit**

```bash
git add Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h Plugins/Airside/Source/Airside/Private/Tool/BuildSession.cpp Plugins/Airside/Source/Airside/Public/Present/RoadNetworkActor.h Plugins/Airside/Source/Airside/Private/Present/RoadNetworkActor.cpp Source/AirportMgr/RoadBuildController.cpp Plugins/Airside/Source/AirsideEditor/Public/RoadBuildEditorTool.h Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEditorTool.cpp Plugins/Airside/Source/AirsideTests/Private/GuideToggleTest.cpp
git commit -m "feat(tool): Alt suspends every guide for one drag"
```

---

### Task 3: The SNAP section and its eight buttons

**Files:**
- Modify: `Source/AirportMgr/BuildActions.h`
- Modify: `Source/AirportMgr/BuildActions.cpp`
- Modify: `Source/AirportMgr/BuildBarWidget.h`
- Modify: `Source/AirportMgr/BuildBarWidget.cpp`
- Test: `Source/AirportMgr/BuildActionsTest.cpp`

**Interfaces:**
- Consumes: `FSnapGuideSettings::Toggle/IsEnabled` (Task 1).
- Produces: `EActionSection::Snap`; eight actions with ids `snap.extending`,
  `snap.pointalign`, `snap.aligned`, `snap.collinear`, `snap.parallel`, `snap.runway`,
  `snap.world`, `snap.offset`; `UBuildBarWidget::SnapSection`.

**FOUR LISTS MUST AGREE, AND THREE OF THEM NOW FAIL THE BUILD.** The spec's §7 says only two
do; that is out of date - `SectionSpecs` gained a `static_assert` of its own
(`BuildBarWidget.cpp:49`) alongside `SectionNames`' (`BuildActions.cpp:16`), both tied to
`EActionSection::Count`. So the enum, `SectionNames` and `SectionSpecs` all fail at compile
time. The FOURTH - `UBuildBarWidget`'s `BindWidgetOptional` UPROPERTY - is the silent one: with
no member for a section, the Blueprint has nothing to bind and the buttons simply never draw.

- [ ] **Step 1: Add the section to all four lists**

In `Source/AirportMgr/BuildActions.h`, add `Snap` to `EActionSection` **before `Count`**:

```cpp
	Game,

	/** The guide sources: eight toggles, lit when live. See the snap-guides design §7. */
	Snap,

	Count
```

In `Source/AirportMgr/BuildActions.cpp`, `SectionNames` is SENTENCE CASE, not upper - the
existing rows are `TEXT("Time")`, `TEXT("Tools")` and so on. Append in the SAME position the
enum uses; the array is indexed by the enum, so an entry in the wrong slot renames two sections
at once and the `static_assert` cannot see that:

```cpp
	constexpr const TCHAR* SectionNames[] =
	{
		TEXT("Time"), TEXT("Tools"), TEXT("Edit"), TEXT("Aircraft"), TEXT("Selection"), TEXT("Game"),
		TEXT("Snap"),
	};
```

In `Source/AirportMgr/BuildBarWidget.h`, beside the other section panels:

```cpp
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> SnapSection;
```

and in `BuildBarWidget.cpp`, add the row to `SectionSpecs` - a table of
`{ EActionSection, TObjectPtr<UPanelWidget> UBuildBarWidget::* }`:

```cpp
		{ EActionSection::Game,      &UBuildBarWidget::GameSection },
		{ EActionSection::Snap,      &UBuildBarWidget::SnapSection },
```

- [ ] **Step 2: Add the eight actions**

In `BuildActions.cpp`, in the table that `Make(...)` fills, after the Game rows:

```cpp
	// ONE PER ESource, and the test walks the enum against this list rather than counting it -
	// a source added without a toggle is a guide the player cannot switch off.
	//
	// NO KEYS. Eight more bindings would crowd a keyboard that already spends 0-9 on tools,
	// and a toggle is a thing you set once rather than reach for mid-drag. The Alt hold is
	// what mid-drag needs, and it is not a registry action - see FToolContext::bSuspendGuides.
	Make(TEXT("snap.extending"), EActionSection::Snap, LOCTEXT("SnapExtending", "Extending"),
		EKeys::Invalid, false,
		[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::Extending); },
		[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::Extending); },
		Always),
```

Then the other seven, written out rather than left as "the same again" - each differs in three
tokens and a copied row with one token unchanged is a button that toggles its neighbour:

```cpp
	Make(TEXT("snap.pointalign"), EActionSection::Snap, LOCTEXT("SnapPointAlign", "Point"),
		EKeys::Invalid, false,
		[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::PointAlign); },
		[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::PointAlign); },
		Always),
	Make(TEXT("snap.aligned"), EActionSection::Snap, LOCTEXT("SnapAligned", "Aligned"),
		EKeys::Invalid, false,
		[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::Aligned); },
		[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::Aligned); },
		Always),
	Make(TEXT("snap.collinear"), EActionSection::Snap, LOCTEXT("SnapCollinear", "Collinear"),
		EKeys::Invalid, false,
		[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::Collinear); },
		[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::Collinear); },
		Always),
	Make(TEXT("snap.parallel"), EActionSection::Snap, LOCTEXT("SnapParallel", "Parallel"),
		EKeys::Invalid, false,
		[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::Parallel); },
		[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::Parallel); },
		Always),
	Make(TEXT("snap.runway"), EActionSection::Snap, LOCTEXT("SnapRunway", "Runway"),
		EKeys::Invalid, false,
		[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::Runway); },
		[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::Runway); },
		Always),
	Make(TEXT("snap.world"), EActionSection::Snap, LOCTEXT("SnapWorld", "World"),
		EKeys::Invalid, false,
		[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::World); },
		[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::World); },
		Always),

	// GREYED, NOT ABSENT: nothing proposes Offset until stage 4, and a lit button that did
	// nothing would be a worse lie than a greyed one that explains itself. Its row is still
	// here because that is what makes the list walkable from the enum.
	Make(TEXT("snap.offset"), EActionSection::Snap, LOCTEXT("SnapOffset", "Offset"),
		EKeys::Invalid, false,
		[](ARoadBuildController& C) { C.ToggleGuideSource(SnapGuide::ESource::Offset); },
		[](const ARoadBuildController& C) { return C.IsGuideSourceOn(SnapGuide::ESource::Offset); },
		Never),
```

`Always` and `Never` are the existing file-local helpers in `BuildActions.cpp`'s anonymous
namespace (`bool Always(const ARoadBuildController&)`); do not re-declare them.

`SnapGuide::ESource` reaches this file through `#include "Solve/GuideArbiter.h"`, which
`BuildActions.cpp` must gain.

- [ ] **Step 3: Give the controller the two accessors**

In `Source/AirportMgr/RoadBuildController.h`, in the public section:

```cpp
	/** Flip one guide source on the airport the controller is driving. Bar buttons call this. */
	void ToggleGuideSource(SnapGuide::ESource Source);

	/** Whether that source is live. What lights the button. */
	bool IsGuideSourceOn(SnapGuide::ESource Source) const;
```

with `#include "Solve/GuideArbiter.h"` added to that header, and in the `.cpp`:

```cpp
void ARoadBuildController::ToggleGuideSource(SnapGuide::ESource Source)
{
	// THROUGH THE TARGET, because the settings live on the airport and not on the driver -
	// see FSnapGuideSettings. A copy here would be a second place for them to drift.
	if (ARoadNetworkActor* Actor = GetTarget())
	{
		Actor->GuideSources.Toggle(Source);
	}
}

bool ARoadBuildController::IsGuideSourceOn(SnapGuide::ESource Source) const
{
	const ARoadNetworkActor* Actor = GetTarget();
	return Actor != nullptr && Actor->GuideSources.IsEnabled(Source);
}
```

`GetTarget()` is real and returns `ARoadNetworkActor*`
(`RoadBuildController.h:180`); the member behind it is `TObjectPtr<ARoadNetworkActor> Target`
at line 397. Verified, not assumed.

- [ ] **Step 4: Write the failing test**

In `Source/AirportMgr/BuildActionsTest.cpp`, append before the final `#endif`:

```cpp
/**
 * ONE TOGGLE PER SOURCE, WALKED FROM THE ENUM. Spec §9's
 * AirportMgr.Actions.SnapTogglesAreInTheRegistry: a source added in a later stage without a
 * button is a guide the player cannot switch off, and nothing else would say so.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapTogglesAreInTheRegistryTest,
	"AirportMgr.Actions.SnapTogglesAreInTheRegistry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSnapTogglesAreInTheRegistryTest::RunTest(const FString& Parameters)
{
	const TArray<TPair<SnapGuide::ESource, const TCHAR*>> Expected = {
		{ SnapGuide::ESource::Extending,  TEXT("snap.extending")  },
		{ SnapGuide::ESource::PointAlign, TEXT("snap.pointalign") },
		{ SnapGuide::ESource::Aligned,    TEXT("snap.aligned")    },
		{ SnapGuide::ESource::Collinear,  TEXT("snap.collinear")  },
		{ SnapGuide::ESource::Parallel,   TEXT("snap.parallel")   },
		{ SnapGuide::ESource::Runway,     TEXT("snap.runway")     },
		{ SnapGuide::ESource::World,      TEXT("snap.world")      },
		{ SnapGuide::ESource::Offset,     TEXT("snap.offset")     } };

	// THE LIST ABOVE IS ITSELF A SECOND LIST, so it is checked against the enum's own size
	// first - otherwise a source added to ESource could be missed by this test as easily as by
	// the registry, which is the failure the test exists to prevent.
	TestEqual(TEXT("every ESource value is covered by this test's own table"),
		Expected.Num(), static_cast<int32>(SnapGuide::ESource::Offset) + 1);

	for (const TPair<SnapGuide::ESource, const TCHAR*>& Pair : Expected)
	{
		const FBuildAction* Action = FindAction(FName(Pair.Value));
		if (!TestNotNull(*FString::Printf(TEXT("%s is registered"), Pair.Value), Action))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("%s sits in the SNAP section"), Pair.Value),
			Action->Section, EActionSection::Snap);
		TestTrue(*FString::Printf(TEXT("%s can be executed"), Pair.Value),
			static_cast<bool>(Action->Execute));
		TestTrue(*FString::Printf(TEXT("%s reports whether it is lit"), Pair.Value),
			static_cast<bool>(Action->IsActive));
	}

	// AND THE SECTION HAS A NAME. ActionSectionName indexes SectionNames by the enum, so a row
	// added in the wrong slot renames two sections at once and the static_assert cannot see it.
	// SENTENCE CASE, like every other row in SectionNames - "Time", "Tools", "Game". An
	// upper-case literal here would have failed against correct code.
	TestEqual(TEXT("the Snap section is named"),
		FString(ActionSectionName(EActionSection::Snap)), FString(TEXT("Snap")));

	return true;
}
```

Add `#include "Solve/GuideArbiter.h"` to that test file.

- [ ] **Step 5: Build**

Expect `Result: Succeeded`.

- [ ] **Step 6: Run the new test**

```
./Tools/Run-AirsideTests.ps1 -Project "C:\repos\AirportMgr2_snap-toggles\AirportMgr.uproject" -Filter AirportMgr.Actions
```

Expected: all pass, including the new one.

- [ ] **Step 7: Run the whole suite**

Expected: baseline + 4, 0 failed, 0 crashed. **Watch `AirportMgr.HUD.VerbsComeFromBuildActions`
and `AirportMgr.Bar.BarBuildsFromRegistry`** - both walk the registry and a new section with
eight rows is exactly what they measure.

- [ ] **Step 8: Commit**

```bash
git add Source/AirportMgr/BuildActions.h Source/AirportMgr/BuildActions.cpp Source/AirportMgr/BuildBarWidget.h Source/AirportMgr/BuildBarWidget.cpp Source/AirportMgr/RoadBuildController.h Source/AirportMgr/RoadBuildController.cpp Source/AirportMgr/BuildActionsTest.cpp
git commit -m "feat(ui): a SNAP section, one lit button per guide source"
```

---

### Task 4: Record the stage, and judge it in PIE

**Files:**
- Modify: `docs/superpowers/specs/2026-09-17-snap-guides-design.md`

- [ ] **Step 1: Amend §7 for the eighth toggle**

§7 lists seven sources and predates `PointAlign`. Replace its bar sketch and defaults line:

```markdown
```
SNAP
[Extending] [Point] [Aligned] [Collinear] [Parallel] [Runway] [World] [Offset]
```

Defaults on: Extending, PointAlign, Parallel, World. Off: Aligned, Collinear, Runway, Offset.
**A player meeting every guide at once learns nothing**; the ones that fire most often teach
the mechanism, and the rest are found when wanted. PointAlign joins the on set because it is
the gesture's own geometry, the same argument that puts Extending there - it was added after
§7 was first written.
```

- [ ] **Step 2: Mark the stage done in §8**

```markdown
3. ~~The toggles, the `Snap` section, and the Alt suspend.~~ **Done 2026-09-17.** Enabled
   state is a USTRUCT of NAMED BOOLS rather than an array indexed by `ESource`: that enum is
   plain (Solve/ has no `.generated.h`), so an index-keyed array would serialise by position
   and a reorder would silently repoint every saved toggle. The chain skips a disabled source
   before it works rather than filtering its candidates afterwards, which needed
   `IGuideSource::Kind()`.
```

- [ ] **Step 3: Run the whole suite one last time**

Expected: baseline + 4, 0 failed, 0 crashed.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-09-17-snap-guides-design.md
git commit -m "docs(spec): stage 3 landed, and there are eight toggles not seven"
```

- [ ] **Step 5: Judge it in PIE**

Nothing above measures feel, and this is the stage that decides whether the guides are
pleasant or fussy.

1. Open the project and look at the build bar: a **SNAP** section with eight buttons, four of
   them lit (Extending, Point, Parallel, World) and Offset greyed.
2. Press `0`, draw a plot, and drag a back corner - confirm a guide appears.
3. Click **Parallel** off and drag the same corner. The taxiway guide should stop; the
   frontage one should remain.
4. Hold **Alt** mid-drag. Every guide should vanish and the corner should follow the raw
   cursor. Release, and it should come back.

What to report back: whether the defaults feel right, whether Alt is comfortable to hold while
dragging (it fights window-drag on some setups), and whether eight buttons crowd the bar.

---

## Unresolved questions

1. **Alt may be taken.** On some Windows setups Alt-drag is a window gesture, and in the
   editor viewport Alt is orbit. If it fights, the fallback is a different hold - but that is
   a PIE finding, not something to pre-empt.
2. **Toggle state is per-airport and saved with the level.** That is what §7 asked for, but it
   means a player's preference travels with the map rather than with them. If that reads
   wrong, the state belongs in a user setting instead, and `FSnapGuideSettings` moves.
3. **`Offset`'s greyed button** is a promise of stage 4. If stage 4 slips, consider hiding the
   button rather than greying it.
