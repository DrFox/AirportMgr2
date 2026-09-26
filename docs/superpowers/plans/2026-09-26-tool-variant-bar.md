# Tool Variant Bar Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A popout row under the build bar's tool row lists the active tool's variants (widths; runway surface/approach) and lets the player click one; key cycling drives the same list.

**Architecture:** Tools expose variants as data (`FToolVariantAxis`) through two new `IBuildTool` virtuals. `FBuildSession` and `ARoadBuildController` forward them. `UBuildBarWidget` gains a `VariantSection` it rebuilds on Id change and refreshes each tick.

**Tech Stack:** UE 5.8.2 C++, UMG built in code, automation tests (`Airside.*`, `AirportMgr.*`).

**Spec:** `docs/superpowers/specs/2026-09-26-tool-variant-bar-design.md`

## Global Constraints

- `Tool/` stays free of the game module and of `UAirsideContent` - widths come through `IRoadEditTarget` only.
- The level default (`WidthIndex == INDEX_NONE`) is never written by `GetVariantAxes`.
- Names come from their one source: `RunwaySurfaceName` / `RunwayApproachName` (RunwayFacts.h), widths from `URoadProfile::GetTotalWidth()`.
- Unity build: file-local helpers in anonymous namespaces carry a file-unique prefix.
- `Check-Architecture.ps1` passes; every existing `UE_LOG` survives (the two cycle lines move into `SelectVariant`).
- Build from the worktree with `-NoHotReloadFromIDE`; tests via `./Tools/Run-AirsideTests.ps1 -Project <worktree uproject>`.

## Decisions made while planning (deviations from spec, stated)

- **Width label is the width** ("15 m"). `URoadProfile` has no display name, and deriving "Code C" from the asset name would be a second source of truth. A `DisplayName` on the profile is a follow-up. `Detail` is left empty for widths.
- **Key cycling steps from the LIT option**, not from `WidthIndex`: when the level default lights preset k, the first press goes to k+1; when nothing is lit, to 0. The spec's own PIE check ("press 1, lit button advances") needs this. Two existing assertions ("the first press picks the narrowest") are rewritten to derive the expectation from the lit option.
- **The row sits under the tool row**, as the spec says; the bar is auto-sized so it grows upward.

## Review Focus

1. Content set with ONE width - row of one button, still shown (count > 0), cycling a no-op wrap. Pinned in Task 1.
2. Active tool changes while row is built (Taxiway -> Road): Ids differ, row must rebuild, not keep taxiway buttons. Pinned in Task 4.
3. Edit mode on: `GetActiveTool()` returns the edit tool, which has no axes; row hides. Pinned in Task 3.
4. Click on a disabled option does nothing (future unlocks). Pinned in Task 1 via a test-only tool? No - `bEnabled` is always true today; pinned by `SelectVariant` rejecting out-of-range only. Noted, not testable until a producer sets false.
5. Mid-chain width change: live state picks up the new width (existing `State->WidthIndex` copy). Pinned in Task 1.

---

### Task 1: Variant types, IBuildTool virtuals, FRoadDrawTool

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadBuildTool.h` (types above `IBuildTool`, virtuals after `OnReselect`)
- Modify: `Plugins/Airside/Source/Airside/Public/Tool/RoadDrawTool.h`, `Private/Tool/RoadDrawTool.cpp`
- Create: `Plugins/Airside/Source/AirsideTests/Private/ToolVariantTest.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/TaxiwayWidthTest.cpp:109-124`, `ServiceRoadWidthTest.cpp:62-84`

**Interfaces produced:**
```cpp
struct FToolVariant { FName Id; FText Label; FText Detail; bool bEnabled = true; };
struct FToolVariantAxis { FName Id; FText Label; TArray<FToolVariant> Options; int32 Current = INDEX_NONE; };
// IBuildTool
virtual void GetVariantAxes(const FToolContext& Context, TArray<FToolVariantAxis>& Out) const {}
virtual bool SelectVariant(const FToolContext& Context, int32 Axis, int32 Option) { return false; }
```

- [ ] **Step 1: failing test** `ToolVariantTest.cpp`, test `Airside.Tool.Variants.RoadWidth`, with a `FTvFakeTarget : FNullEditTarget` holding three transient taxiway profiles (1050, 1500, 2300) and a settable `DefaultProfile` returned from `ResolveProfileFor(Kind, INDEX_NONE)`:
  - axes: one axis `Width`, 3 options, Ids distinct, labels "10.5 m"/"15 m"/"23 m" (`FText::AsNumber` on metres, max 1 fractional digit).
  - default = profile[1] -> `Current == 1`, `GetWidthIndex() == INDEX_NONE` (not written).
  - default off-list (transient 1800) -> `Current == INDEX_NONE`.
  - `SelectVariant(0, 2)` true, `GetWidthIndex()==2`, axes `Current==2`; `SelectVariant(0, 3)`, `(1, 0)`, `(0, -1)` false and unchanged.
  - `OnReselect` from default lit at 1 -> 2 -> wraps 0; from nothing lit -> 0.
  - one-profile target: one axis of one option; `OnReselect` leaves index 0.
  - empty target: no axes (`Out.Num()==0`).
  - A `FSelectTool` (or any registry tool without variants, e.g. `ToolRegistry()[0].Make()`) reports no axes.
- [ ] **Step 2:** build fails (types missing) - that is the red.
- [ ] **Step 3: implement.** Types + virtuals in RoadBuildTool.h with WHY comments (one list, two gestures). In `FRoadDrawTool`:
  - `GetVariantAxes`: bail on null target / count 0. Build options from `ResolveWidthProfile(Kind, i)`; Id = `FName(*FString::Printf(TEXT("W%d"), FMath::RoundToInt(Width)))` (uu, stable per width). `Current = WidthIndex` if set, else the index whose profile `== ResolveProfileFor(Kind, INDEX_NONE)` or whose `GetTotalWidth()` equals it within 0.5 uu, else `INDEX_NONE`.
  - `SelectVariant(Axis, Option)`: Axis 0 only, `0 <= Option < Count`; sets `WidthIndex`, copies into `State`, logs the moved line `"%s width -> %d of %d, %.1f m"`. Returns true.
  - `OnReselect`: keep both refusal logs; then `Current` from `GetVariantAxes`; `Next = Current == INDEX_NONE ? 0 : (Current + 1) % Count`; `SelectVariant(Context, 0, Next)`.
- [ ] **Step 4:** rewrite the two "first press picks the narrowest" blocks: compute `Lit` from `GetVariantAxes` before the first press and assert `(Lit == INDEX_NONE ? 0 : (Lit + 1) % Count)`; keep walk/wrap/laid-width assertions, relative to that start. TaxiwayWidthTest block 3 (`// index 0`) sets width with `SelectVariant(Context, 0, 0)` instead of one reselect so it still lays the narrowest.
- [ ] **Step 5:** build + `-Filter Airside.Tool` green. Commit `feat(tool): variant axes on IBuildTool; road/taxiway width axis`.

### Task 2: FRunwayTool axes

**Files:** `Public/Tool/RunwayTool.h`, `Private/Tool/RunwayTool.cpp`, `AirsideTests/Private/ToolVariantTest.cpp`

- [ ] **Step 1: failing test** `Airside.Tool.Variants.Runway` with the runway fake (3 profiles 2300/3000/4500):
  - three axes, Ids `Width`,`Surface`,`Approach`; option counts 3, `ERunwaySurface::Count`, `ERunwayApproach::Count`; surface labels equal `RunwaySurfaceName(e)`.
  - Current = 0, `Tarmac` index, `Visual` index at construction.
  - `SelectVariant(1, Concrete)` sets `Surface`, leaves width; `(2, Precision)` sets approach; `(3, 0)` false.
  - `OnReselect` plain/Shift(insert)/Ctrl(remove) moves axis 0/1/2 only (existing `Airside.Tool.Runway` also pins this through the session).
- [ ] **Step 2:** red.
- [ ] **Step 3: implement.** `GetVariantAxes`: Width axis only when count > 0 (Current = clamped `WidthIndex`); Surface/Approach always. `SelectVariant` sets the field; width logs the existing `"Runway width -> ..."` line. `NextWidth/NextSurface/NextApproach` become `SelectVariant(Axis, (Current + 1) % N)` so their tests keep passing; the empty-target and null-target refusals stay in `NextWidth`.
- [ ] **Step 4:** build + `-Filter Airside.Tool` green. Commit `feat(tool): runway width/surface/approach axes`.

### Task 3: Session + controller forwarding

**Files:** `Public/Tool/BuildSession.h`, `Private/Tool/BuildSession.cpp`, `Source/AirportMgr/RoadBuildController.h/.cpp`, test in `ToolVariantTest.cpp`

**Interfaces produced:**
```cpp
// FBuildSession
void GetActiveVariantAxes(const FToolContext& Context, TArray<FToolVariantAxis>& Out) const;
bool SelectActiveVariant(const FToolContext& Context, int32 Axis, int32 Option);
// ARoadBuildController
void GetActiveVariantAxes(TArray<FToolVariantAxis>& Out) const;   // context = { Target }
bool SelectActiveVariant(int32 Axis, int32 Option);               // logs LogRoadBuild, invalidates readout cache
```

- [ ] **Step 1: failing test** `Airside.Tool.Variants.Session` (real `FAirsideTestWorld`): select the Taxiway registry index; `GetActiveVariantAxes` has one Width axis with `GetWidthCount(Taxiway)` options; `SelectActiveVariant(0, 1)` -> the taxiway tool's `GetWidthIndex()==1`; toggle `EGestureMode::Edit` -> no axes (edit tool has none).
- [ ] **Step 2:** red. **Step 3:** implement (Out.Reset() first; null active tool -> empty / false). Controller builds `FToolContext Ctx; Ctx.Target = Target;` - no snap pipeline, it is polled per tick; `SelectActiveVariant` logs `Variant: %s axis %d -> %d (%s)` and calls `InvalidateToolReadoutCache()`.
- [ ] **Step 4:** green. Commit `feat(session): forward variant axes to the active tool`.

### Task 4: VariantSection on the bar

**Files:** `Source/AirportMgr/BuildBarWidget.h`, create `Source/AirportMgr/BuildBarVariants.cpp` (the variant half of `UBuildBarWidget`, keeping BuildBarWidget.cpp from growing), `BuildBarWidget.cpp` (EnsureSlots row + tick call), `BuildBarWidgetTest.cpp`

**Interfaces:**
```cpp
UCLASS() class UBuildBarVariantEntry : public UObject {
  UPROPERTY() int32 Axis; UPROPERTY() int32 Option;
  UPROPERTY() TObjectPtr<UButton> Button; UPROPERTY() TObjectPtr<UTextBlock> Label;
  UPROPERTY() TWeakObjectPtr<UBuildBarWidget> Owner;
  UFUNCTION() void HandleClicked();   // Owner->RunVariant(Axis, Option)
};
// UBuildBarWidget
UPROPERTY(meta=(BindWidgetOptional)) TObjectPtr<UPanelWidget> VariantSection;
void RunVariant(int32 Axis, int32 Option);
void RefreshVariantsForTest(ARoadBuildController& C) { RefreshVariantsFor(C); }
int32 VariantButtonCountForTest() const;
bool IsVariantSectionVisibleForTest() const;
private: void RefreshVariantsFor(ARoadBuildController& C);
         void RebuildVariants(const TArray<FToolVariantAxis>& Axes);
         UPROPERTY() TArray<TObjectPtr<UBuildBarVariantEntry>> VariantEntries;
         TArray<FName> VariantSignature;   // tool index + axis/option Ids
```

- [ ] **Step 1: failing test** `AirportMgr.Actions.VariantRowFollowsTool` (pattern of `BarTickBuildsOneActionContext`): Select tool -> `RefreshVariantsForTest` -> section hidden, 0 buttons. Taxiway -> visible, buttons == width count. Road -> buttons == road count and rebuilt (count differs, or compare Ids). Runway -> width + surface + approach counts. `RunVariant(0, 1)` on taxiway -> tool `GetWidthIndex()==1`.
- [ ] **Step 2:** red.
- [ ] **Step 3: implement.** EnsureSlots (code-built root): after `ToolsBorder`, a `UBorder` (`Style->Panel`) holding a `UVerticalBox` `VariantSection`, collapsed. Asset root without it: append to root with a `LogBuildBar` warning (same as `Ensure`). `RefreshVariantsFor`: get axes; signature = `[FName(tool index), axis Id, option Ids...]`; differs -> `RebuildVariants` (clear children and entries; per axis a `UHorizontalBox` with a muted `Heading` text of the axis label then one button per option, label text via `ApplyText(Label)`), log `LogBuildBar` `Variant bar rebuilt: %s` listing axis[ids]. Every call: visibility `Collapsed` if no axes else `SelfHitTestInvisible`; per entry `SetIsEnabled(bEnabled)`, accent/button colours exactly as `RefreshStateFor` does for lit. `RefreshStateFor` calls `RefreshVariantsFor(C)` at its end so `NativeTick` covers it.
- [ ] **Step 4:** build + `-Filter AirportMgr.Actions` green (includes the four existing bar tests). Commit `feat(ui): variant popout row on the build bar`.

### Task 5: Full verification

- [ ] `./Tools/Check-Architecture.ps1` clean (no new rule-12 claims without `// ENFORCED BY:`).
- [ ] `./Tools/Run-AirsideTests.ps1 -Project <worktree>` - quote the `N test(s) run, 0 failed, 0 crashed` line.
- [ ] `UE_LOG(` count before/after on touched files: must not fall.
- [ ] PIE check for the user (the editor must be on the worktree build, so after merge or by opening the worktree .uproject): select Taxiway -> row shows widths, default lit; click 23 m, draw -> ghost + segment 23 m; press 1 -> lit advances. Log: `Variant bar rebuilt` and `Taxiway width -> ...`.
