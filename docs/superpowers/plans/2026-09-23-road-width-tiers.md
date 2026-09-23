# Road width tiers (PR 2 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** The road tool cycles Narrow / Standard / Wide two-lane service roads the way the taxiway tool cycles ICAO widths.

**Architecture:** The tool seam's taxiway-only width pair becomes one pair keyed by `ERoadKind` (`GetWidthCount(Kind)`, `ResolveWidthProfile(Kind, Index)`), so there is one width list per kind and one resolver, not two parallel pairs. `UAirsideContent::ServiceRoadProfile` becomes `ServiceRoadProfiles` (narrow to wide). `ResolveProfileFor` honours `WidthIndex` for roads. Three assets authored by `build_road_profiles.py`.

**Tech Stack:** UE 5.8 C++, automation tests, headless Python.

**Spec:** `docs/superpowers/specs/2026-09-23-road-lanes-and-widths-design.md` §1. Stacked on PR 1 (`feature/road-lanes-and-widths`).

## Global Constraints

- Tiers: Narrow 300 / Standard 350 / Wide 450 uu per lane, kerb 60 uu; Narrow keeps the asset path `/Game/DA_RoadProfile_ServiceRoad`.
- Fillets stay derived from the largest service vehicle for every tier in THIS PR. Per-tier design vehicles (the rig for Wide) arrive with vehicle sizes in PR 3 - ruled 2026-09-23: the rig is not a vehicle in the model yet, so a Wide fillet sized for it now would be a typed number PR 3 deletes.
- A service road with `WidthIndex == INDEX_NONE` lays the actor override if set, else `ServiceRoadProfiles[0]`. Never a transient fallback (a saved road would reload as a taxiway).
- Build/test commands, commit rules: as PR 1's plan.

## Review Focus

1. A content set with no service road profiles: the road tool refuses and says so; cycling says so - `ServiceRoadWidth` test covers the count-zero branch.
2. A level whose actor has a per-instance `ServiceRoadProfile` override: INDEX_NONE still lays the override, a cycled index lays the content tier - `ProfileResolutionIsOneRule` extension.
3. The ghost and the click resolve the same profile for a cycled road - both go through `ResolveProfileFor`; asserted in `ServiceRoadWidth`.
4. Switching tool kind (road -> taxiway) with a remembered index must not lay a taxiway at a road index - each tool instance owns its own index (existing per-tool state); asserted.
5. An old DA_AirsideContent with the removed `ServiceRoadProfile` field loads without crashing and the script rewires the list - Task 3 step checks the MARKER line.

---

### Task 1: One width pair keyed by kind

**Files:** `Public/Tool/RoadEditTarget.h`, `Public/Testing/AirsideTestWorld.h` (FNullEditTarget), `Public/Present/RoadEditFacade.h/.cpp`, `Public/Present/RoadNetworkActor.h/.cpp`, `Private/Tool/RoadDrawTool.cpp`, `AirsideTests/Private/TaxiwayWidthTest.cpp`, `RoadNetworkActorTest.cpp`; new test `ServiceRoadWidthTest.cpp`.

**Interfaces:** Produces `virtual int32 GetWidthCount(ERoadKind Kind) const = 0;` and `virtual URoadProfile* ResolveWidthProfile(ERoadKind Kind, int32 Index) const = 0;` replacing `GetTaxiwayProfileCount()` / `ResolveTaxiwayProfile(int32)` (all callers migrated; the old names deleted so nothing keeps the second list alive).

- [ ] Step 1: failing test `Airside.Tool.ServiceRoadWidth` - a fake target (FTaxiwayWidthTest's `FFakeWidthTarget` shape) holding three road profiles; a road-kind `FRoadDrawTool`: OnReselect x4 -> connects carry indices 0,1,2,0; count zero -> index stays INDEX_NONE and a Warning is logged (spy with `FLogLineSpy`). A taxiway-kind tool on the same target still cycles taxiway profiles only.
- [ ] Step 2: build twice, run, expect compile failure (no `GetWidthCount`).
- [ ] Step 3: implement; `OnReselect` drops the ServiceRoad refusal and reads `GetWidthCount(Kind)` / `ResolveWidthProfile(Kind, ...)`; log `"%s width -> %d of %d, %.1f m"` with "Road"/"Taxiway". Keep the WHY comments that still hold; replace the "A SERVICE ROAD HAS NOTHING TO CYCLE" paragraph with the dated reason it changed.
- [ ] Step 4: run `-Filter Airside.Tool+Airside.Present`; PASS. Commit `feat(tool): road width cycles like taxiway width`.

### Task 2: Content list and resolution

**Files:** `Public/Content/AirsideContent.h`, `Private/Present/RoadNetworkActor.cpp` (`ResolveServiceRoadProfile`, `ResolveProfileFor`, width pair), `Private/Present/RoadEditFacade.cpp:364-380` message, `AirsideContentTest.cpp` if it names the field.

- [ ] Step 1: failing test extending `Airside.Present.ProfileResolutionIsOneRule` (or a new `Airside.Present.RoadWidthResolution` if that test is taxiway-shaped): with a content set holding three road tiers, `ResolveProfileFor(ServiceRoad, 2)` is tier 2; `INDEX_NONE` is tier 0; with an actor override set, `INDEX_NONE` is the override and `1` is tier 1.
- [ ] Step 2: RED. Step 3: `TArray<TSoftObjectPtr<URoadProfile>> ServiceRoadProfiles` replaces the single field (doc comment carries the old one's authored-asset reasoning). Step 4: GREEN. Commit `feat(content): service road width tiers`.

### Task 3: Assets

- [ ] `build_road_profiles.py`: `ROAD_TIERS = [("", 300.0), ("_Standard", 350.0), ("_Wide", 450.0)]`, filled in place, wired to `service_road_profiles`. Run headless; MARKER lines show three profiles, 4 bands and 2 guidelines each, and the content list of 3. Confirm `.uasset` mtimes.
- [ ] Full suite; commit `content: Standard and Wide road tiers`.
