# Taxiway Strip Stage 4 - Holds at the Strip Edge; Road Crossings Yield

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A ground vehicle crossing a taxiway stops at the strip edge and gives way to aircraft; a taxiway's intermediate holding position sits at the strip edge of the taxiway it joins; both are painted.

**Architecture:** The derived guideline graph gains, at every road-taxiway crossing, one SHARED conflict node where the road's through-path and the taxiway's through-path cross (today they share nothing, so nothing yields). Road arm ends at such a node are set back to the taxiway's strip edge and flagged with a new hold kind whose claim reserves the conflict node - the runway-bar pattern (reserve before you pass the bar) applied to a node. Intermediate holds move to the strip edge by splitting the arm, not by moving its end, so junction turns do not change.

**Tech Stack:** UE 5.8.2 C++, Airside plugin, UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-28-taxiway-clearance-strip-design.md` ("Holds sit at the strip edge", stage 4). Depends on stage 3 only for `TaxiwayStrip` accessors already merged; can land before or after it.

## Research this plan stands on (2026-09-29, against main 0b231a69)

- Runway holds: `ComputeExitSetBacks` (`Private/Build/RoadGuidelineBuilder.cpp:212`) acts at MIXED nodes (continuous + non-continuous arms); setback = `ExitGeometry::NodeExitLength` floored by `TaxiwayEndFloor` and the pavement cut; the END node is moved back along the straight tangent (`DeriveSegmentGuidelines :430-445`); flags applied in `ReapplyHoldingPositionMarks` (`:1039-1060`) via `Network.SetGuidelineNodeHoldingPosition(Node, EHoldingPositionKind::Runway, Seg)`.
- `EHoldingPositionKind { None, Runway, Intermediate }` (`Public/Model/RoadGuideline.h:52`) - a UENUM? check; append only.
- Claims: `TrafficClaims.cpp:720-748` - a step whose `To` node has `HoldingPositionFor` set wants a RESERVED (never occupied) `FTrafficResource::OfSurface(Segment)` claim on every segment of the runway chain, "APPLIES TO EVERY CLASS". Intermediate holds are INERT (`:337-340`).
- Rank: `TraversalPriority` (`RoadTraffic.h:102`) Emergency > Aircraft > Pedestrian > GroundVehicle; `FClaimPass::RankAt` (`TrafficClaims.cpp:1111`).
- **Crossings today do not yield.** At a road/taxiway node every segment end derives its own node (`AddGuidelineNode` never dedups, `:448`); through-turns of the road and of the taxiway cross geometrically but share no `FGuidelineNodeId`; occupancy is by resource identity only (`TrafficOccupancy.h:9-18`); nothing in Model/ intersects geometry. `Airside.Model.Traffic.TruckCrossesTaxiway` passes only because it hand-authors a shared Centre node.
- Splitting: `URoadNetwork::SplitGuidelineEdge(Edge, T, WeldTolerance, OutNode, OutHead, OutTail)` (`RoadNetwork.h:367`) copies fields, split node has no Origin; `SplitFromEnd(Network, EdgeId, bFromA, Length, OutRest)` (`RoadGuidelineBuilder.cpp:164`) - chord length, fails if `Chord <= Length`.
- Intermediate holds land on the segment-end node at the pavement cut line (`:1063-1086`, `CutLinePoint :418-421`); the tool picks the nearest aircraft node (`HoldingPointTool.cpp:18-31`).
- Paint: `FHoldingPositionMarkingBuilder::Build` (`Private/Build/HoldingPositionMarkingBuilder.cpp:41`), `MarkingAddBar(Out, Z, Node, Toward, Across, HalfWidth, Near, Far, bDashed)` (`:17`), frame `HoldingBarAt` (`Public/Model/HoldingBarFrame.h:52`). No stop-line or give-way marking exists; `FRoadLaneMarkingBuilder` paints road centre dashes only.
- Tests to lean on: `GroundTrafficTest.cpp` (`NodeYield`, `PriorityOverride`, `HoldingPosition`, `CrossingHoldsRunway`, `BarToBarCrossing`, fixture `FCrossingFixture` `AirsideTestFixtures.h:181`), `RoadCrossingTest.cpp` (`RoadCrossesTaxiway`, `RoadEndsAgainstTaxiway`, fixture `LayCrossing`), `TruckCrossingTest.cpp`, `HoldingPositionMarkingTest.cpp`, `HoldingPositionMarkTest.cpp` (`SurvivesRebuild`).

## Rulings made while planning (user asleep - see Unresolved questions)

1. **Conflict node, not geometry tests in traffic.** One shared node per crossing lets the existing node-claim rank rule decide who goes; a geometric overlap test inside traffic would be a second, parallel arbitration.
2. **Vehicle stop line = strip edge of the crossed taxiway**, measured along the road arm: `(TaxiwayHalfWidth + Strip) / sin(angle)` from the node, the runway's own `TaxiwayEndFloor` shape with the strip added.
3. **Aircraft do not stop at road crossings** - rank already gives them way; no aircraft-side hold is added.
4. **Intermediate holds move to the joined taxiway's strip edge** by SPLITTING the arm edge at that distance and flagging the split node; the arm's end node (where junction turns attach) does not move. Intermediate holds stay INERT in traffic (spec 2026-09-07) - this stage moves and paints them only.
5. **USER 2026-09-29: a SOLID WHITE STOP BAR** across the road at its hold node. White, because it is a road marking; the aircraft holds are yellow.

## Global Constraints

- UENUM values appended (`EHoldingPositionKind` gains `TaxiwayCrossing` at the end).
- The guideline graph samples ONCE (`GuidelineGeom::Sample`): the conflict node splits the two turn edges; the follower walks the split edges; the overlay draws them. No second evaluator.
- Look at the map, not just the suite (memory: a graph change that passed 348 tests put kilometre arcs on the apron) - Task 6 is a PIE look.
- Build/test commands as in the stage 3 plan; `Model/` world-free.

## Review Focus

1. **A road that ENDS on a taxiway** (no through-path, `RoadEndsAgainstTaxiway`) - no conflict node (nothing crosses); the road arm still gets a stop line if a turn onto the taxiway exists for Emergency only. Task 2 test.
2. **Two taxiways and a road at one node** - one conflict node per road-through x taxiway-through pair; the road hold reserves ALL of them. Task 2.
3. **Rebuild stability** - conflict nodes are derived, re-created each rebuild; a truck mid-crossing across a rebuild must re-resolve (GroundTrafficRebuild re-resolves by position). Task 3 test rebuilds mid-crossing.
4. **Emergency vehicles** - rank above aircraft; they cross without waiting. Task 3 asserts it.
5. **Arm too short for the setback** (road stub shorter than the strip) - clamp like `NodeExitLength`, log once, no negative lengths. Task 2.

---

### Task 1: The failing crossing, on a DERIVED graph

**Files:** Test `AirsideTests/Private/TruckCrossingTest.cpp` (append; existing file).

- [ ] **Step 1: Write the composition test** `Airside.Model.Traffic.TruckYieldsAtDerivedCrossing`:
  - Build with the facade-free fixture used by `RoadCrossingTest`'s `LayCrossing` (read it; reuse or copy into the shared fixtures header - never a second copy in two files) a taxiway W-E and a service road N-S through ONE shared road node, then `TestGraph::Derive`.
  - Dispatch an aircraft along the taxiway and a truck along the road, timed (read `TruckCrossesTaxiway` for how it times them) so both reach the crossing within the same second.
  - Assert, over `RunUntil`: the truck's nose is never within `TaxiwayHalfWidth + Strip` of the taxiway centreline while the aircraft's body is within its half-span of the crossing point; and the truck eventually crosses.
  - Control assertion first: the aircraft and truck paths share no guideline node today (walk both plans' step nodes) - documents why it fails.
- [ ] **Step 2:** Build, run `-Filter Airside.Model.Traffic.TruckYieldsAtDerivedCrossing`; expect FAIL (they overlap). Do not commit it red; it stays uncommitted until Task 3 turns it green and lands in Task 3's commit.

---

### Task 2: Derive the conflict node and the road stop line

**Files:** `Private/Build/RoadGuidelineBuilder.cpp` (a pass after turns are built: `DeriveCrossingConflicts`), `Public/Model/RoadGuideline.h` (append `TaxiwayCrossing` to `EHoldingPositionKind`; a node field `FGuidelineNodeId CrossingConflict` on `FGuidelineNode` is NOT enough for a hold protecting several conflicts - add `UPROPERTY() TArray<FGuidelineNodeId> ProtectsConflicts;` beside `HoldingPositionFor`, doc why), `Public/Model/RoadNetwork.h` (setter beside `SetGuidelineNodeHoldingPosition`), test `RoadCrossingTest.cpp`.

- [ ] **Step 1: Failing build tests** in `Airside.Build.RoadCrossesTaxiway.ConflictAndStopLine`:
  - after `Derive`, exactly one guideline node lies on both the road through-turn and the taxiway through-turn (edges incident to it: two of each class), within 1 uu of the geometric intersection;
  - each road arm's end node is `(HalfWidth + Strip)/sin(angle)` from the road node (1 uu), flagged `TaxiwayCrossing`, `ProtectsConflicts` = that conflict;
  - a road ENDING on the taxiway: no conflict node, arm still flagged (Review Focus 1) - decide from what turns exist; if only Emergency turns exist, flag anyway (the road meets a live taxiway);
  - road + two taxiways at one node: two conflicts, both in each road hold's list (Review Focus 2);
  - a 10 m road stub: setback clamped to the stub share, log `Crossing setback clamped` once (Review Focus 5).
- [ ] **Step 2:** Run, FAIL.
- [ ] **Step 3: Implement.**
  - Setback: extend `ComputeExitSetBacks`'s sibling - a new `ComputeCrossingSetBacks(Network, Solved, SetBack, CrossingHolds)` for nodes with >= 1 `TaxiwayStrip::HasStrip` arm and >= 1 ground-vehicle-only arm: each road arm's `SetBack` = max over taxiway arms of `(TaxiwayHalf + StripWidthOf)/max(sin(acute), sin 10 deg)`, clamped exactly as `NodeExitLength` clamps (reuse `ExitGeometry` helpers; name the reuse). The road END node moves (as runway exits do) - so road-to-road through turns re-attach there, which is right: the road stops back from the taxiway.
  - Conflict: after turns exist, for each pair (road through-turn edge, taxiway through-turn edge) at the same road node, intersect their SAMPLED polylines (`GuidelineGeom::Sample` - the one sampler); at the first intersection, split both edges with `SplitGuidelineEdge` at the matching T and WELD to one node (split the first, then split the second with `WeldTolerance` = 1 uu onto the first's node - read `SplitGuidelineEdge`'s weld semantics first; if it cannot weld to an existing node, split both and merge the second into the first via the network's node-merge used by guideline editing - grep `MergeGuidelineNodes`/`WeldGuidelineNodes`).
  - Flags: `SetGuidelineNodeHoldingPosition(RoadEnd, TaxiwayCrossing, TaxiwaySeg)` and the conflicts list, applied in the same place `ReapplyHoldingPositionMarks` applies Runway kinds so a rebuild re-applies them.
- [ ] **Step 4:** Tests pass; full suite (turn geometry at crossings moved - expect `RoadCrossingTest` assertions on end positions to need the new setback; update with comments naming the stop line). Commit `feat(crossing): conflict node and strip-edge stop line at road-taxiway crossings`.

---

### Task 3: The stop line reserves the conflict

**Files:** `Private/Model/TrafficClaims.cpp` (`:720-748` block), test from Task 1.

- [ ] **Step 1:** Task 1's test is the RED.
- [ ] **Step 2: Implement** beside the runway-bar claim: when `Step.To`'s node is `TaxiwayCrossing`, add a RESERVED `FTrafficResource::OfNode(Conflict)` wanted claim for each conflict, `Rank = RankAt(...)` - never occupied, for the runway bar's own reason (a queue at the line must not lock the conflict against the aircraft it protects). Aircraft need no change: their route passes THROUGH the conflict node, so their ordinary node claims contend with the truck's reservation and rank decides. Comment the claim with `ENFORCED BY: Airside.Model.Traffic.TruckYieldsAtDerivedCrossing`.
- [ ] **Step 3:** Extend Task 1's test: an emergency vehicle crosses without waiting (Review Focus 4); rebuild mid-crossing (`TestGraph::Rebuild` + `OnGraphRebuilt`) and assert the truck still completes and never overlaps (Review Focus 3).
- [ ] **Step 4:** `-Filter Airside.Model.Traffic` then full suite. Commit `feat(crossing): vehicles hold at the stop line until the conflict is clear`.

---

### Task 4: Intermediate holds at the joined taxiway's strip edge

**Files:** `Private/Build/RoadGuidelineBuilder.cpp` (`:1063-1086`), test `HoldingPositionMarkTest.cpp` (append).

- [ ] **Step 1: Failing test** `Airside.Build.IntermediateHoldAtStripEdge`: two E taxiways in a T; set an intermediate hold on the stem's end node; after `Rebuild`, the flagged node is `(HalfWidth + Strip)/sin(angle)` from the junction along the stem (1 uu); the stem's END node (turn attachment) is where it was before the hold was set (compare positions with and without the hold); survives a second rebuild.
- [ ] **Step 2:** Run, FAIL (hold is at the cut line).
- [ ] **Step 3:** In the intermediate re-apply, instead of flagging the end node: find the arm edge from that end, `SplitFromEnd(Network, Edge, bFromEnd, Distance, Rest)` where Distance = the setback above LESS the end node's own distance from the junction (it already sits at the cut line); flag the split node. If `SplitFromEnd` fails (arm shorter), flag the end node and log `Intermediate hold kept at the junction: arm too short`. The mark (`FHoldingPositionMark{ FGuidelineEndRef }`) is unchanged - it still names the END; only where it is realised moves, so saved marks keep meaning.
- [ ] **Step 4:** Tests + full suite (`HoldingPositionMarking*`, `MeshFreshnessTest HoldingPositionMeshFollowsToggle` may assert positions - update with reason). Commit `feat(holds): intermediate holds sit at the strip edge of the taxiway joined`.

---

### Task 5: Paint the stop line

**Files:** `Private/Build/HoldingPositionMarkingBuilder.cpp`, the paint-slot map (`RoadSurfacePresenter.cpp` - how lane markings get white; reuse that slot), test `HoldingPositionMarkingTest.cpp`.

- [ ] **Step 1: Failing test** `Airside.Build.HoldingPositionMarking.CrossingStopLine`: a derived crossing paints, per road arm, ONE SOLID bar (`MarkingAddBar(..., bDashed=false)`), 40 uu deep (the stand stop bar's `StopBarWidth` - share the constant, do not retype it), across the ROAD's full width at the hold node, in the white lane-marking slot (assert the meaning id / slot the way `PaintCarriesItsMeaningId` does for stands), none on the taxiway.
- [ ] **Step 2:** FAIL. **Step 3:** Add the `TaxiwayCrossing` case: one solid bar, `HalfWidth` = the ROAD profile's (HoldingBarAt already reads the node's own segment). **Step 4:** Pass; commit `feat(crossing): paint the stop bar`.

---

### Task 6: Look at it

- [ ] Worktree editor (port 8002), M_Test: lay a service road across a taxiway at a right angle; PIE; dispatch an arrival and a vehicle across it. Shot the stop bar (`Mcp.py shot x.png editor`) and read `LogAirsideTraffic` for the truck waiting. Ask the user whether the white stop bar reads right at zoom (visual iteration live, not SDD).

## Unresolved questions (for the user)

ANSWERED 2026-09-29: solid white stop bar (ruling 5).

2. Should aircraft ever wait for a vehicle already committed to the crossing (current rank rule: only if the vehicle holds the conflict first)? Rules as planned: yes, whoever reserves first; aircraft outrank when both ask at once.
3. Intermediate holds stay inert in traffic (spec 2026-09-07) - still right, or should a hold at the strip edge now actually stop aircraft when the joined taxiway is occupied?
