# Runway Categories and Markings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A runway states its surface and approach class, an aircraft states what it needs, admission compares them, and the runway is painted with the ICAO marking set its width and class dictate.

**Architecture:** `FRunwayFacts` on the segment (Model/), `FRunwayRequirements` on the aircraft type and the airframe bundle, `RunwayAdmission::Check` (Model/) called first by both planners; `FRunwayMarkingBuilder` (Build/) beside the holding-position builder, drawn white through a dynamic instance of the road material on a second marking component; runway bands select a surface material slot resolved through the content set; the runway tool chooses surface and approach beside width.

**Tech Stack:** UE 5.8.2 C++, Airside plugin, `Run-AirsideTests.ps1`, `Tools/Python` material authoring (editor closed).

**Spec:** `docs/superpowers/specs/2026-09-07-runway-categories-and-markings-design.md`

## Global Constraints

- `Model/` never includes Build/Tool/Present/Entities; `Build/` may include Model/ and Solve/; `Tool/` never Present/. `Check-Architecture.ps1` enforces.
- The bitwise weld and the one-evaluator guideline rule are untouched: markings are a separate mesh, never welded to the road.
- Every content default resolves in exactly one `UAirsideSettings::Resolve*` / `ARoadNetworkActor::Resolve*` function.
- One struct per thing: requirements travel in `FAirframe` like the performance structs.
- Header changes need the editor closed; batch T1, T2 and T5's headers into one build where possible.
- Commits: concise, no Co-Authored-By trailer. Every `UE_LOG` survives; a new log line is a feature.

---

### Task 1: Vocabulary, facts on the segment, requirements on the aircraft

**Files:** create `Public/Model/RunwayFacts.h`; modify `Public/Model/RoadNode.h` (`FRoadSegment`), `Public/Model/RoadNetwork.h` / `Private/Model/RoadNetwork.cpp` (`RunwayFactsFor`, `SetRunwayFacts`), `Public/Model/RoadEntity.h` (`FRunwayRequirements`, `FAirframe::Requirements`), `Public/Entities/AircraftType.h` / `.cpp` (`Requirements`, Piper fallback), `Private/Content/AirsideSettings.cpp` (copy into the airframe), `Private/Present/RoadEditFacade.cpp` (`SplitSegmentIn` copies facts); test `AirsideTests/Private/RunwayFactsTest.cpp`.

**Interfaces:**
- Produces: `enum class ERunwaySurface : uint8 { Grass, Tarmac, Concrete, Reinforced }`, `enum class ERunwayApproach : uint8 { Visual, NonPrecision, Precision }`, `USTRUCT FRunwayFacts { ERunwaySurface Surface = Tarmac; ERunwayApproach Approach = Visual; }`, `UPROPERTY() FRunwayFacts FRoadSegment::Runway;`, `FRunwayFacts URoadNetwork::RunwayFactsFor(FRoadSegmentId Seed) const;` (the seed's own; every chain member carries the same), `bool URoadNetwork::SetRunwayFacts(FRoadSegmentId Seed, const FRunwayFacts&)` (writes the whole `RunwayChain(Seed)`; false when Seed is not a runway), `USTRUCT FRunwayRequirements { ERunwaySurface MinimumSurface = Grass; ERunwayApproach ApproachNeeded = Visual; double TakeoffFieldLength = 0.0; double LandingFieldLength = 0.0; }`, `UPROPERTY(EditAnywhere) FRunwayRequirements FAirframe::Requirements;`, `UPROPERTY(EditAnywhere) FRunwayRequirements UAircraftType::Requirements;`, `static FRunwayRequirements UAircraftType::PiperMeridianRequirements();` (Grass, Visual, 80000, 80000).

- [ ] **Red test** `Airside.Model.RunwayFacts`: a runway split by a taxiway (two segments) plus a taxiway; `SetRunwayFacts(RW1, {Concrete, Precision})` writes both halves; `RunwayFactsFor(RW2)` reads it back; `SetRunwayFacts` on the taxiway returns false and changes nothing; a segment added by `SplitSegmentIn` (via the facade in a world-free test: call `URoadEditFacade::SplitSegmentIn` is private - instead split through `ARoadNetworkActor::SplitSegment` in the tool-test world pattern, or expose `URoadNetwork::SplitSegmentForTest`; choose the actor pattern used by `HoldingPointToolTest`) carries the facts on both halves; `DuplicateObject` of the network keeps them (the PIE path).
- [ ] Run: `./Tools/Run-AirsideTests.ps1 -Filter Airside.Model.RunwayFacts` → FAIL (no such members).
- [ ] Implement. `RoadEntity.h` doc on `FRunwayRequirements`: field lengths are PUBLISHED figures for admission, the physics keeps its rolls for motion (spec §3.2). `AirsideSettings.cpp` `ResolveDefaultAirframe`: `Piper.Requirements = UAircraftType::PiperMeridianRequirements();` and the asset path copies `Type->Requirements`.
- [ ] Run → PASS. Full suite: same count + 1, 0 failed.
- [ ] Commit: `feat(airside): runway facts on the segment, runway requirements on the aircraft`.

### Task 2: Admission

**Files:** create `Public/Model/RunwayAdmission.h`, `Private/Model/RunwayAdmission.cpp`; modify `Public/Model/ArrivalPlanner.h` / `.cpp`, `Public/Model/DeparturePlanner.h` / `.cpp`; tests `AirsideTests/Private/RunwayAdmissionTest.cpp`, additions to `ArrivalPlannerTest.cpp` and `DeparturePlannerTest.cpp`.

**Interfaces:**
- Produces: `enum class ERunwayRefusal : uint8 { None, Surface, Approach, TooShort, TooNarrow }`; `ERunwayRefusal RunwayAdmission::Check(const URoadNetwork&, FRoadSegmentId Seed, const FAirframe&, bool bLanding);` `FString RunwayAdmission::Describe(ERunwayRefusal, const FRunwayFacts&, const FAirframe&)`; `EArrivalRefusal::NotAdmitted` + `UPROPERTY() ERunwayRefusal FArrivalPlan::Admission`; `EDepartureRefusal::NotAdmitted` + `FDeparturePlan::Admission`.
- Consumes: T1. Length: `RunwayExtentAt` (chain length). Width: `Profile->Guidelines[0].MaxWingspan` if > 0 else the ICAO code table `{1800: 1500 (A, 15 m), 2300: 2400 (B), 3000: 3600 (C), 4500: 6500 (E), 6000: 8000 (F)}` in uu of wingspan, keyed by total width, nearest.

- [ ] **Red tests.** `Airside.Model.RunwayAdmission`: Piper (Grass/Visual/80000/80000, wingspan 1311) against a 100000 tarmac visual runway → None; the same runway with facts Grass → None; an airframe needing Tarmac against Grass → Surface; needing Precision against Visual → Approach; a 90000 field length against 80000 of runway → TooShort (landing and take-off separately: bLanding picks the figure); wingspan 3600 against the 2300 width → TooNarrow. `ArrivalPlanner.NotAdmitted`: the two-exit airport with facts Grass and an airframe needing Tarmac → `Why == NotAdmitted`, `Admission == Surface`, `DescribeRefusal` contains "grass". `DeparturePlanner.NotAdmitted` likewise on the departure fixture.
- [ ] Implement. In both planners the admission check runs right after the runway is found and before exits/entries are listed; `RunwayTooShort` stays as the physics backstop after it.
- [ ] Run → PASS. Suite green. Commit: `feat(airside): runway admission - surface, approach, field length, width - refused by name`.

### Task 3: Field lengths cover the roll

**Files:** `AirsideTests/Private/FieldLengthTest.cpp`.

- [ ] `Airside.Model.FieldLengthsCoverTheRoll`: for `UAirsideSettings::ResolveDefaultAirframe()` (and every `UAircraftType` the content set names, when one exists): `FTakeoffRun::RequiredRoll(Ground, Climb) <= Requirements.TakeoffFieldLength` and `FLandingRun::RequiredLandingDistance(...) * FLandingRun::LandingMargin <= Requirements.LandingFieldLength`, each with the figures in the message. Commit with T2 or alone: `test(airside): a published field length is never shorter than the roll`.

### Task 4: The runway marking builder

**Files:** create `Public/Build/RunwayMarkingBuilder.h`, `Private/Build/RunwayMarkingBuilder.cpp`, `Private/Build/MarkingGlyphs.h` (stroke font, private to Build/); test `AirsideTests/Private/RunwayMarkingTest.cpp`.

**Interfaces:**
- Produces: `struct FRunwayMarkingBuilder { static int32 Build(const URoadNetwork&, double Z, FRoadMeshBuffers& Out); }` returning runways painted. Constants (uu) per spec §4.1: `StripeLength = 3000`, `StripeWidth = 180`, `StripeStart = 600`, `DashOn = 3000`, `DashOff = 2000`, `CentrelineWidth = 45 (90 at width >= 4500)`, `AimingPointAt = 40000 (30000 below 120000 of runway)`, `AimingPointLength = 4500 (3000 below width 3000)`, `AimingPointWidth = 600`, `AimingPointGap = 1800`, `TouchdownPairsAt = {15000, 30000, 45000}`, `TouchdownStripe = 2250 x 180`, `SideStripeWidth = 90`, `DigitHeight = 900`, `GrassMarker = 60 every 6000, corners 300`. Threshold stripe count by total width: `{1800: 4, 2300: 6, 3000: 8, 4500: 12, 6000: 16}`.
- Glyphs: `MarkingGlyphs::Strokes(TCHAR)` returning a `TArray<FVector2D>` polyline set on a 0..1 x 0..1.6 cell for `0-9`, `L`, `C`, `R`; each stroke a quad of width 0.15 cells. Designators are `RunwayDesignator::Designate(Direction)` at the near end and `Reciprocal` at the far end, two digits each, `ToText` for the string.
- Per chain: walk `RunwayChain` from any runway segment not yet visited; `RunwayExtentAt` on its first node's position gives threshold, direction, length; facts from `RunwayFactsFor`; width from `ProfileFor`. Each marking is quads via the same `MarkingAddQuad` helper as the holding positions (move it to a shared `Private/Build/MarkingQuads.h` with the signed-area check; both builders include it).

- [ ] **Red test** `Airside.Build.RunwayMarkings.Precision45`: a 45 m (4500) Precision tarmac runway 200000 long W→E. Count and measure: 12 threshold stripes at each end, each 3000 x 180, symmetric about y = 0, outermost 300 inside the edge; designation glyphs: two digits at each end whose bounding box is 900 tall and sits 600 past the stripes; centreline dashes: `floor((length - 2 * (600 + 3000 + 600 + 900 + 600)) / 5000)` quads of 3000 x 90; aiming point: two bars 4500 x 600, 1800 apart, starting 40000 from each threshold; touchdown zone: pairs at 15000/30000/45000 from each end (one, two, three stripes a side); side stripes: two quads 200000 long x 90 at ±(2250 - 45). Every vertex at Z, UV1 zero, engine-computed normals up (copy the block from `HoldingPositionMarkingTest`). `Airside.Build.RunwayMarkings.ByWidth`: for each of 1800/2300/3000/6000 the stripe count and the centreline width. `Airside.Build.RunwayMarkings.ByApproach`: Visual has no aiming point and no TDZ; NonPrecision has the aiming point, no TDZ, no side stripes; Precision has all. `Airside.Build.RunwayMarkings.Grass`: only markers, `count == 2 * (length / 6000 + 1) + 4`. `Airside.Build.RunwayMarkings.ShortPrecision`: a 60000 runway paints only the TDZ pairs that do not reach the far end's set (paint what fits, spec §8).
- [ ] Run → FAIL (no builder). Implement. Run → PASS.
- [ ] Commit: `feat(airside): runway markings - designation, threshold, centreline, aiming point, touchdown zone, side stripes, grass markers`.

### Task 5: Drawing it - the white component and the surface materials

**Files:** modify `Public/Present/RoadSurfacePresenter.h` / `.cpp` (second marking component, `RebuildRunwayMarkings`, a `UMaterialInstanceDynamic` of the surface material with `MarkingColor` white), `Public/Present/RoadNetworkActor.h` / `.cpp` (`RunwayMarkingComponent`, `Initialize` gains it), `Public/Content/AirsideContent.h` (`RunwayGrassMaterial`, `RunwayTarmacMaterial`, `RunwayConcreteMaterial`), `Public/Present/RoadNetworkActor.h` (`ResolveRunwayMaterial(ERunwaySurface)`), `Public/Profiles/RoadMaterialSet.h` (`static const FName RunwaySlotName(ERunwaySurface)`), `Public/Build/RoadProfileBands.h` / `.cpp` (`FromProfile(..., FName SlotOverride = NAME_None)`), `Private/Build/RoadMeshBuilder.cpp` (a runway segment's bands take `RunwaySlotName(Facts.Surface)`), `Tools/Python/build_runway_materials.py`; test `AirsideTests/Private/RunwaySurfaceTest.cpp`.

- [ ] Materials: `build_runway_materials.py` makes `M_RunwayGrass`, `M_RunwayTarmac`, `M_RunwayConcrete` as instances of `M_RoadSurface` with `CentrelineWidth = 0` (no yellow line; spec §4.2) and a base tint per surface (grass green-brown, tarmac as the road, concrete pale grey); reinforced reuses concrete (spec §8). Run with the editor closed; commit the `.uasset`s.
- [ ] Red test `Airside.Build.RunwaySurfaceSlots`: a network with a tarmac runway and a taxiway, built with a `URoadMaterialSet::MakeTransient({"Lane", "Shoulder", "RunwayTarmac", ...})`: every triangle of the runway segments carries the `RunwayTarmac` slot id, the taxiway's carry their profile's. Change the facts to Grass, rebuild: `RunwayGrass`.
- [ ] Presenter: `RebuildRunwayMarkings` after `RebuildMarkings`, Z = `SurfaceZ + 0.5`, the white MID cached like `GhostMID`; census `UE_LOG(LogRoadMesh, "Runway markings: %d runway(s), %d triangle(s)")`. The material set the presenter hands the mesh builder gains the three runway slots resolved from the content set (fallbacks: the surface material).
- [ ] Build (headers), suite green. Commit: `feat(airside): runways painted white, surfaced by their facts, the yellow line gone`.

### Task 6: The tool

**Files:** `Public/Tool/RoadBuildTool.h` (`virtual void OnReselect(const FToolContext&) {}` on `IBuildTool`), `Private/Tool/BuildSession.cpp` (pressing the active tool's key calls `OnReselect`), `Public/Tool/RunwayTool.h` / `.cpp` (`Surface`, `Approach`; `OnReselect`: plain cycles width, `bInsertModifier` cycles surface, `bRemoveModifier` cycles approach; the preview label reads `"45 m, concrete, precision"`), `Public/Tool/RoadEditTarget.h` (`PlaceRunway(From, To, Profile, const FRunwayFacts&)`, `SetRunwayFacts(int32 SegmentIndex, const FRunwayFacts&)`), facade and actor overrides (undoable edits), `AirsideEditor` command tooltip; test `AirsideTests/Private/RunwayToolTest.cpp` (extend the existing one).

Note: `FRunwayTool::NextWidth` exists and has NO caller today - the width cycle was never wired. `OnReselect` is the wiring; say so in its comment.

- [ ] Red test: reselect cycles width; with the insert modifier cycles surface; placement writes the facts onto every segment of the strip; `SetRunwayFacts` through the actor is undoable and changes the chain.
- [ ] Implement. Suite green. Commit: `feat(airside): the runway tool chooses surface and approach; reselect cycles width`.

### Task 7: Probe, docs, PR

- [ ] `StarterMapProbeTest`: per runway chain log facts, width, length, admission of the default airframe, marking triangle count.
- [ ] Spec outcome section; memory; PR body with build line, test line, `UE_LOG` delta, the PIE recipe (place a 45 m precision concrete runway: piano keys, numbers, dashed white centreline, aiming bars, TDZ pairs, side stripes; a grass strip: markers only; Land with the Piper on grass: accepted; set the aircraft's minimum surface to Tarmac in the asset: refused with "grass").
- [ ] `gh pr create` to main.
