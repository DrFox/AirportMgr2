# Taxiway Strip Stage 5 - Stand Numbers and the Turn-Off Paint

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every stand has a stable number, shown in the inspector and painted on the taxiway beside a yellow lead-in that leaves the centreline, gaps across the clearance strip, and resumes inside the stand box (BHX, user 2026-09-28).

**Architecture:** A saved `StandNumber` on `FEntityInstance`, issued by `URoadNetwork` from a saved counter and never reused; backfilled on load. A shared marking-text renderer is extracted from the runway builder (its digit font already exists). A new `FStandTurnOffMarkingBuilder` reads the derived lead-in (pose -> lead end -> sweeps) and paints the lead-in only where it lies on taxiway pavement, plus an arrow and the number.

**Tech Stack:** UE 5.8.2 C++, Airside plugin, game module `AirportMgr` (inspector), UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-28-taxiway-clearance-strip-design.md` ("Paint (user, from BHX)", "Stand numbers", stage 5). Independent of stages 3, 4 and 6.

## Research this plan stands on (2026-09-29, against main 0b231a69)

- `FEntityInstance` (`Public/Model/RoadEntity.h:228-373`) - all bare `UPROPERTY()`; placement data travels in `FEntityPlacement` (`:390-429`, "new captured facts go on this struct"). Stand creation: `URoadEditFacade::PlaceEntity` (`RoadEditFacadeSurfaces.cpp:233`, legacy) and `PlaceStandInPlot` (`:794`); both end in `URoadNetwork::PlaceEntity(const FEntityPlacement&, const FLetterEnvelope&)` (`RoadNetwork.cpp:1500`), which `RoadSlot::Add`s into `Entities` REUSING freed slots - an index is not a stable number.
- Undo is a Memento (`DuplicateObject` of the network, `RoadEditHistory.cpp:17`): new UPROPERTYs on `FEntityInstance` and `URoadNetwork` are captured automatically.
- Save: tagged serialisation (`OpsSave.cpp:20-36`); new fields load at default. `URoadNetwork::PostLoad` calls `EnsureStandOutlines` (`RoadNetwork.cpp:1407`). M_Test, M_Test_Small, M_Test_Large contain saved stands -> backfill needed.
- Names today: raw 0-based entity index, shared with depots, reused: inspector `"Stand {0}"` with `S.Index` (`Source/AirportMgr/InspectorWidget.cpp:371`), `InspectFacts::DestinationOf` `"Stand %d"` (`InspectFacts.cpp:23`), `WhyStandRefused` `"overlaps stand %d"` (`RoadEditFacadeSurfaces.cpp:727`, and `:455`). Tests asserting them: `InspectFactsTest.cpp:95`, `InspectorWidgetTest.cpp:313`.
- Lead-in: `FAnchorLink::Join` (`Private/Build/AnchorLink.cpp:615`) - `Corner` on the taxiway centreline (`:732`); taxiway edge split at +/-Offset into `BackNode`/`FwdNode` (`:833-852`); `LeadEnd = AddGuidelineNode(Corner - Link.Dir*Offset, true)` (`:866`); lead edge `PoseNode -> LeadEnd` (Aircraft+Emergency, derived, straight) (`:880-897`); two sweeps `LeadEnd -> BackNode/FwdNode` with `Control = Corner` (`:932-941`); no-room branch: single split, no sweeps. Nothing records the turn-off; re-derived every Topology rebuild. Taxi-through stands have TWO turn-offs (forward ray).
- Paint: no builder paints the lead-in outside the stand box (`RoadMeshBuilder.cpp:817-821` deliberately does not); the taxiway yellow centreline is the MATERIAL (`bMaterialCentreline`). `FStandMarkingBuilder` paints inside the box only (lead-in clipped to `Frame.Inner`, `StandMarkingBuilder.cpp:256-286`), stand letter via 7-segment `GlyphSegments` (A-F only).
- Digits exist: `MarkingGlyphs::Strokes(TCHAR)` (`Private/Build/MarkingGlyphs.h`, 0-9 L C R, 1x1.6 cell, stroke 0.15). The string renderer `Designation(FRoadMeshBuffers&, double Z, const FRunwayFrame&, const FString&)` is in an anonymous namespace in `RunwayMarkingBuilder.cpp:65`. No TextRender anywhere (the stand header rejects it).
- Marking order: `RebuildMarkings` (`RoadSurfacePresenter.cpp:489`, builders `:548-549`) runs after `FAnchorLink::Build` in a Topology rebuild - a new builder there sees the lead-in edges.
- Inspector: `InspectFacts::DescribeStand(Traffic, Network, EntityIndex, FStandFacts&)` (`InspectFacts.h:135`); `FStandFacts{Index, SizeClass, DesignWingspan, OccupantAgent, bOccupantParked, AnchorCount, bReachable, PoseRole, bServiceable}`; widget `UInspectorWidget::Refresh` builds one `FString::Format` (`InspectorWidget.cpp:366-397`).
- Tests: `StandMarkingTest.cpp` (`Airside.Build.StandMarking.*`, `Airside.Present.StandMarking.*`), `RunwayMarkingTest.cpp` (`Airside.Build.RunwayMarkings.*`), `LeadInSweepTest.cpp`, `AnchorLinkTest.cpp`, `InspectFactsTest.cpp`, `InspectorWidgetTest.cpp`.

## Rulings made while planning (user asleep - see Unresolved questions)

1. **Numbering: sequential per airport from 1, never reused** (USER 2026-09-29: "1..N for now"). A deleted stand's number is retired (real aprons do not renumber); gaps are fine. Depots are not numbered.
2. **Backfill on load** for stands saved without a number: in `Entities` order, starting past the saved counter.
3. **One number painted per turn-off**, on the taxiway pavement beside the centreline on the approach side of the turn, reading for a pilot approaching - two for a taxi-through stand (both turn-offs). Arrow: a chevron pointing along the sweep into the stand.
4. **The lead-in paint follows the derived edges, clipped to taxiway pavement** - so the strip gap falls out of "paint only where paved" rather than a second rule, and the samples are the edges' own (the graph samples once).
5. **Every "stand N" string becomes the number**, including log lines that name stands to the player (refusal text); pure debug logs keep indices.

## Global Constraints

- New UPROPERTYs only (undo and save capture them); no custom serialisation.
- The guideline graph samples ONCE: paint the lead-in from `GuidelineGeom::Sample` of the actual edges.
- Refactor contract for the `Designation` extraction: runway paint byte-identical (RunwayMarkings tests + a triangle-count check), UE_LOG and comment counts do not fall.
- Visual changes iterate live (memory): Task 6 is a look with the user, not more SDD.
- Build/test commands as in the stage 3 plan.

## Review Focus

1. **Undo after delete** - delete stand 3, undo: it comes back as 3, and the counter does not advance twice. Task 1.
2. **Load of M_Test** - saved stands backfilled once; a second load does not renumber. Task 1.
3. **No-room lead-in** (single split, no sweeps) - still one number, placed at the lead end. Task 4.
4. **Paint never lands inside the strip** - every lead-in paint triangle is on taxiway pavement or inside the stand box. Task 4.
5. **A stand whose lead-in found no taxiway** - no turn-off paint, no crash, number still shown. Task 4.

---

### Task 1: The number - issued, stable, backfilled

**Files:** `Public/Model/RoadEntity.h` (`UPROPERTY() int32 StandNumber = 0;` doc: 0 = unnumbered/not a stand), `Public/Model/RoadNetwork.h` (`UPROPERTY() int32 NextStandNumber = 1;` beside `Entities`, doc the never-reuse rule), `Private/Model/RoadNetwork.cpp` (`PlaceEntity` issues for `IsStand()`; `PostLoad` backfills), test `AirsideTests/Private/RoadNetworkTest.cpp` or the entity test file (grep `PlaceEntity(` tests; append).

- [ ] **Step 1: Failing test** `Airside.Model.StandNumbers`: place stands A, B, C -> 1, 2, 3; depot -> 0; remove B, place D -> 4 (not 2); undo-equivalent: `DuplicateObject` the network before removing C, restore, C is 3 and `NextStandNumber` is what it was (Review Focus 1); a network with three stands at 0 and counter 1 -> after `PostLoad`-path backfill (call the function PostLoad calls, e.g. `EnsureStandNumbers()`) they are 1, 2, 3 in entity order and a second call changes nothing (Review Focus 2).
- [ ] **Step 2:** FAIL (compile).
- [ ] **Step 3:** Implement: in `PlaceEntity(const FEntityPlacement&, ...)` after the instance is built, `if (Instance.IsStand()) { Instance.StandNumber = NextStandNumber++; }`. `EnsureStandNumbers()` (private, beside `EnsureStandOutlines`, called from `PostLoad`): for live stands with 0 in index order, assign from `max(NextStandNumber, 1 + max existing)`. Comments: never reused, and why (a stand's number is painted on the ground; renumbering would repaint the airport).
- [ ] **Step 4:** Pass; full build (UPROPERTY - header rebuild ~4 min). Commit `feat(stands): stable stand numbers`.

---

### Task 2: Show the number wherever a stand is named to the player

**Files:** `Public/Model/InspectFacts.h` (`FStandFacts` gains `int32 Number`), `Private/Model/InspectFacts.cpp` (`DescribeStand`, `DestinationOf`), `Source/AirportMgr/InspectorWidget.cpp:371` (title from Number), `Private/Present/RoadEditFacadeSurfaces.cpp:455,727` (refusals), tests `InspectFactsTest.cpp:95`, `InspectorWidgetTest.cpp:313`.

- [ ] **Step 1:** Change the two tests to expect the NUMBER, built so number != index (place and delete a stand first): `"Stand 2"` where the index is 1 or 3. They fail.
- [ ] **Step 2:** Implement the four sites. `DestinationOf` falls back to the index only for a stand with 0 (should not happen after Task 1; log once if it does).
- [ ] **Step 3:** Pass; `-Filter Airside.Model.InspectFacts`, `-Filter AirportMgr.Inspector`, full suite. Commit `feat(stands): the inspector and refusals name stands by number`.

---

### Task 3: Extract the marking-text renderer

**Files:** Create `Private/Build/MarkingText.h/.cpp` (namespace `MarkingText`), modify `Private/Build/RunwayMarkingBuilder.cpp:65` (call it), test `RunwayMarkingTest.cpp`.

**Produces:** `int32 MarkingText::AddString(FRoadMeshBuffers& Out, double Z, const FVector2D& Origin, const FVector2D& Up, const FVector2D& Right, double CellHeight, const FString& Text);` - the body of `Designation`, framed by an origin and two axes instead of `FRunwayFrame` (the runway builder builds them from its frame). Returns triangles added.

- [ ] **Step 1: Pin first:** add to `RunwayMarkingTest` an assertion of the exact triangle count and bounding box of the designation paint on a fixed runway (record today's values by running once). This is the refactor's safety net.
- [ ] **Step 2:** Move the code; `Designation` becomes a three-line caller. Every WHY comment moves with it (count comment lines before/after).
- [ ] **Step 3:** `-Filter Airside.Build.RunwayMarkings` green with identical counts. Commit `refactor(markings): marking text renderer shared, runway paint unchanged`.

---

### Task 4: Paint the turn-off - lead-in on the taxiway, arrow, number

**Files:** Create `Public/Build/StandTurnOffMarkingBuilder.h`, `Private/Build/StandTurnOffMarkingBuilder.cpp`; modify `Private/Present/RoadSurfacePresenter.cpp` (`RebuildMarkings`, call it beside `FStandMarkingBuilder::Build`, stand Guidance paint slot); test `StandMarkingTest.cpp` (append - existing file).

**Produces:** `static int32 FStandTurnOffMarkingBuilder::Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out, FStandTurnOffCensus* Census = nullptr);` with `struct FStandTurnOffCensus { int32 TurnOffs = 0; int32 Numbers = 0; TArray<int32> NumbersPainted; };`

- [ ] **Step 1: Failing tests** (`Airside.Build.StandTurnOff.*`), on a stand placed through the facade behind the strip beside a straight E taxiway, after a Topology rebuild:
  - `PaintsOneNumberPerTurnOff` - census: 1 turn-off, `NumbersPainted == { StandNumber }`; a taxi-through stand: 2.
  - `LeadInOnlyOnPavement` - every lead-in triangle's centroid is within the taxiway pavement (distance to the taxiway centreline <= half-width) or inside the stand outline; none within the strip band (Review Focus 4).
  - `FollowsTheDerivedEdges` - lead-in paint centre points lie within 1 uu of `GuidelineGeom::Sample` of the sweep and lead edges (the graph samples once).
  - `NoRoomBranch` and `NoTaxiway` (Review Focus 3, 5).
- [ ] **Step 2:** FAIL. **Step 3: Implement:**
  - For each live stand: from `PoseNode`, the incident Aircraft edge that is not stand-internal (`StandGeometryOwner` unset) -> its far node is `LeadEnd`; the edges from `LeadEnd` whose `Control` differs from their chord midpoint are the sweeps (or, no-room, `LeadEnd` IS the taxiway node).
  - Paint: sample each (lead, sweeps) with `GuidelineGeom::Sample`; emit quads `LeadInWidth` wide (use `StandMarkingBuilder`'s constant - move it to a shared header if private) only for sample runs whose points lie on taxiway pavement: test with `TaxiwayStrip::ShapeOf`/footprint of the taxiway segment the sweep derives from (`Edge.DerivedFrom` or the split segment id) - a point-in-footprint check.
  - Arrow: a chevron at the sweep's taxiway end, pointing along the sweep tangent. Number: `MarkingText::AddString` at the approach side, `Up` = taxiway direction toward the turn, `CellHeight` 2 m (a first figure - tune live).
- [ ] **Step 4:** Pass; full suite; commit `feat(stands): paint the turn-off - lead-in on the taxiway, arrow, stand number`.

---

### Task 5: Look at it

- [ ] Worktree editor (port 8002), M_Test (stands backfilled): shot the apron from the stand tool's default camera; check with the user - number size, placement side, arrow shape, lead-in width against the material centreline. Iterate live (Live Coding for body changes), not through new plan tasks.

## Unresolved questions (for the user)

ANSWERED 2026-09-29: 1..N for now (ruling 1).

2. Where the number sits: on the taxiway at the turn-off (ruling 3, as BHX) - also on the stand itself (at the stop bar), or only there?
3. Number height 2 m to start - fine to tune live?
4. Legacy indices in debug logs stay as indices (ruling 5) - OK?
