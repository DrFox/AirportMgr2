# Taxiway Strip Stage 6 - Upgrading a Taxiway: Restriction and Closed Stands

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The player can widen (or narrow) an existing taxiway in place; things inside its grown strip do not vanish - roads and depots RESTRICT the taxiway to the largest letter whose strip is clear, stands inside it are flagged and closed to new arrivals - and the inspector says why.

**Architecture:** A new undoable network mutator `SetSegmentProfile` behind a facade seam, reached from the road tool's width row on an existing segment. A derived-but-saved per-segment `RestrictedLetter`, written only by a `TaxiwayRestriction::Apply` pass that runs in the Topology rebuild BEFORE the guideline builder, which then writes the first non-zero taxiway `MaxWingspan` (only where restricted). Stands need no new state - `StandAdmission::Judge` already re-checks the strip on every call; this stage makes it visible (HUD overlay, inspector) and gives taxiways an inspector card.

**Tech Stack:** UE 5.8.2 C++, Airside plugin, game module `AirportMgr` (inspector, HUD), UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-28-taxiway-clearance-strip-design.md` ("When a taxiway is upgraded", stage 6). Depends on stage 3 (the `JudgeSegment`/footprint machinery and `ProfileFor` fix); do stage 3 first.

## Research this plan stands on (2026-09-29, against main 0b231a69)

- **No in-place profile change exists.** `FRoadSegment::Profile` is written only in `URoadNetwork::AddSegment` (`RoadNetwork.cpp:63`) and copied by `SplitSegment`. The road tool's width cycle sets the NEXT segment's width only (`RoadDrawTool.cpp:596,610`; `ConnectNodes` -> `ResolveProfileFor(Kind, WidthIndex)`, `RoadEditFacade.cpp:598`). Pattern to copy: `URoadEditFacade::SetRunwayFacts` (`RoadEditFacade.cpp:741-771`) - guards before the snapshot, no-op returns true without an edit, `FRoadEditScope Edit(HistoryForEdit(), Network, TEXT(...)); ...; CommitAndNotify(Edit);`; priced edits `CommitPurchase(Edit, Quote)`, refused before the scope.
- **Wingspan admission:** `FGuidelineEdge::MaxWingspan` (0 = unlimited, `RoadGuideline.h:148`), set from `FProfileGuideline::MaxWingspan` for straight edges (`RoadGuidelineBuilder.cpp:473`) and the Min of both arms for turns (`:1353-1358`); refused only in route search `ExceedsWingspan` (`RouteSearch.cpp:167-170`, applied `:343`, `TooWide` result `:851-860`). **Every taxiway edge is 0 today** - no taxiway limits any aircraft.
- Rebuild order: `URoadSurfacePresenter::RebuildInternal` (`RoadSurfacePresenter.cpp:592`): DefaultProfile (`:633`) -> `SolveAll` (`:642`) -> Topology: `FRoadGuidelineBuilder::Build` (`:659`), `FAnchorLink::Build` (`:671`), `DepotKit::ReportIncomplete` (`:679`) -> mesh, aprons, markings (`:693-711`). Derived-but-saved precedent: `TrimA/B`, cut points on `FRoadSegment` (`RoadNode.h:72-93`), "Written ONLY by FRoadNetworkSolver". The undo snapshot duplicates the network, so a transient field would be dropped - derived fields are saved UPROPERTYs.
- Inspector: `ESelectionKind { None, Aircraft, Stand, Runway }` (`Public/Tool/Selection.h:11-25`); `FSelectTool::OnClick` picks aircraft, entity, runway (`SelectTool.cpp:105-141`); UI `Source/AirportMgr/InspectorWidget.cpp` (Runway `:330-354`, Stand `:355-414`), warnings are sentences inside Facts. `InspectFacts::DescribeStand`/`DescribeRunway` (`InspectFacts.h`).
- Flagging: red stand PAINT was removed on purpose (`RoadSurfacePresenter.cpp:447-448`); the HUD overlay `GraphOverlay::DescribeStands` (`GraphOverlay.cpp:29`, called `RoadBuildHUD.cpp:90` gated by `bDrawStands`, and the editor tool) can draw `Sink.Polygon(Outline, EPreviewStyle::Refused)`.
- Tests to extend: `RouteSearchTest.cpp` (`Find`, `FindToGoals`), `RoadGuidelineBuilderTest.cpp` (turn wingspan block `:315-366`), `InspectFactsTest.cpp`, `InspectorWidgetTest.cpp`, `StandAdmissionTest.cpp`, `Airside.Tool.TaxiwayWidth`, `Airside.Tool.Variants.RoadWidth`.

## Rulings made while planning (user asleep - see Unresolved questions)

1. **Gesture: the width row on an existing taxiway.** With the Taxiway tool active, clicking an existing taxiway segment while a different width is chosen RE-PROFILES it (priced: the new cost minus the old, refunds none). No modifier key (memory: destructive gestures need a deliberate mode) - the width row must be set away from the segment's current width first, and the ghost shows the grown strip and what it would restrict/close before the click. Undoable.
2. **Only restriction writes a taxiway wingspan limit.** An unrestricted taxiway keeps `MaxWingspan = 0` (today's behaviour: any aircraft on any taxiway). Making a taxiway's own letter a limit is a separate, bigger gameplay change - question 2.
3. **Restriction ignores stands.** Stands inside the strip close instead (spec ruling); a stand never lowers a taxiway's letter.
4. **Restriction is per SEGMENT**, the letter of the tightest obstruction along it; a turn takes the Min of its arms (existing turn rule).
5. **An aircraft already taxiing on a newly restricted segment finishes** its current route (routes are searched, not re-checked mid-follow); only new searches refuse. Consistent with the closed-stand occupant ruling.

## Global Constraints

- New state is saved UPROPERTYs written by ONE writer (the restriction pass), stated at the field.
- `ESelectionKind` appended (`Taxiway`), never inserted.
- Lists that must agree: selection kinds vs the inspector's branches vs `PositionOf` - the consumer checks names (test).
- Build/test commands as in the stage 3 plan. Look at the map (memory) - Task 7.

## Review Focus

1. **Undo of an upgrade** restores profile AND restriction AND the stand flags (the restriction is recomputed on rebuild; assert after undo). Task 2.
2. **Split of a restricted segment** - both halves keep the profile; restriction recomputed per half. Task 3.
3. **Obstruction removed** (delete the road) -> restriction lifts on the next rebuild. Task 3.
4. **Downgrade** narrows the strip: closed stands reopen, restriction lifts. Task 2/3.
5. **A route that only exists through a restricted taxiway** - arrival planning refuses with `TooWide` and a sentence naming the restriction, not "no route". Task 4.

---

### Task 1: Strip width for a letter below the pavement's

**Files:** `Public/Solve/IcaoCode.h`, `Private/Solve/IcaoCode.cpp`, test `IcaoCodeTest.cpp` (new leaf name, e.g. `Airside.Solve.IcaoCode.TaxiwayStripForLetter`).

**Produces:** `AIRSIDE_API double IcaoCode::TaxiwayStripFor(EIcaoCode Letter, double PavementWidthUu);` - `TaxiwayStripForWidth(W)` becomes `TaxiwayStripFor(TaxiwayLetterForWidth(W), W)`.

- [ ] Test: a 26 m pavement restricted to E -> `3250 - 1300 + 750 = 2700`; to C -> `max(0, 1800-1300) + 450 = 950`; `TaxiwayStripForWidth` unchanged for the five table widths. Implement, pass, commit `feat(icao): strip for a restricted letter`.

---

### Task 2: Change an existing segment's profile, undoably

**Files:** `Public/Model/RoadNetwork.h`/`.cpp` (`bool SetSegmentProfile(FRoadSegmentId, URoadProfile*)` - refuses dead segment, null profile, a runway<->taxiway kind change; bumps the edit revision so a Topology rebuild follows), `Public/Tool/RoadEditTarget.h` (`virtual bool SetSegmentWidth(int32 SegmentIndex, int32 WidthIndex) = 0;` + `virtual FString WhySetSegmentWidthRefused(int32 SegmentIndex, int32 WidthIndex) const = 0;`), `Private/Present/RoadEditFacade.cpp` (SetRunwayFacts pattern; priced by the difference), `ARoadNetworkActor` forwarder, test doubles, test `AirsideTests/Private/RoadEditFacadeTest.cpp` or the `TaxiwayWidth` test file (grep; append).

- [ ] **Step 1: Failing tests** `Airside.Present.SetSegmentWidth`: widen a taxiway C->F; the segment's profile is F's; one undo step restores C (Review Focus 1); same width = no edit opened (history length unchanged); a service road refuses a taxiway width index; unaffordable refuses before the scope; downgrade works (Review Focus 4).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. `WhySetSegmentWidthRefused` is the one evaluator (Task 5's ghost and the click both ask it): dead/kind/afford, and - reusing stage 3's `JudgeSegment` with the NEW shape, `bIsTaxiway`, ends = the segment's own nodes, `Ignore` = the segment itself - a new PAVEMENT intrusion into ANOTHER taxiway's strip is refused (widening into a neighbour is laying pavement); what the grown STRIP swallows is NOT refused (that is this stage's point: it restricts or closes). **Step 4:** Pass; full suite; commit `feat(taxiway): change an existing taxiway's width in place`.

---

### Task 3: The restriction pass

**Files:** Create `Public/Model/TaxiwayRestriction.h`, `Private/Model/TaxiwayRestriction.cpp`; `Public/Model/RoadNode.h` (`FRoadSegment`: `UPROPERTY() uint8 RestrictedLetter = 0xFF;` - 0xFF = unrestricted; doc: written ONLY by `TaxiwayRestriction::Apply`), `Public/Model/RoadNetwork.h` (a single writer `WriteSegmentRestriction`, the `WriteSegmentEndSolve` precedent), `Private/Present/RoadSurfacePresenter.cpp` (call `Apply` after `SolveAll`, before `FRoadGuidelineBuilder::Build`, Topology rebuilds only), test `RoadNetworkTest.cpp` (`Airside.Model.TaxiwayRestriction`).

**Produces:**
```cpp
namespace TaxiwayRestriction
{
	struct FObstruction { enum class EKind : uint8 { Road, Depot, Taxiway } Kind; int32 Index; double Depth; };
	/** The largest letter <= the pavement's whose strip is clear of roads, depots and other
	 *  taxiways' pavement (stands excluded - they close instead); unset = unrestricted. */
	AIRSIDE_API TOptional<EIcaoCode> RestrictionOf(const URoadNetwork& Network, FRoadSegmentId Taxiway, FObstruction* OutWorst = nullptr);
	/** Writes RestrictedLetter on every taxiway segment. Returns how many are restricted. */
	AIRSIDE_API int32 Apply(URoadNetwork& Network);
}
```

- [ ] **Step 1: Failing tests:** a 26 m (F) taxiway with a 6 m service road whose near edge is 42 m off the taxiway centreline - inside F's reach (`1300 + 3450 = 4750`), outside E's at that pavement (`1300 + TaxiwayStripFor(E, 2600) = 1300 + 2700 = 4000`) -> restricted to E, `OutWorst` names the road. Compute every reach in the test from `TaxiwayStripFor`, never a typed figure; no obstruction -> unset; road deleted -> unset after `Apply` (Review Focus 3); split the restricted segment -> both halves evaluated separately (Review Focus 2); a stand in the strip -> NOT a restriction (ruling 3); roads/taxiways the segment MEETS (shares a node with, at an allowed angle) are exempt (stage 3's exempt rule - reuse, do not re-derive).
- [ ] **Step 2:** FAIL. **Step 3:** Implement with stage 3's reverse-query machinery (`JudgeSegment`'s strip-swallow step, factored so it can be asked "at letter L"): try letters from the pavement's down to A; first clear wins. Log `Restriction: taxiway %d -> Code %s (was %s), by %s %d` once per change, not per rebuild (compare with the stored value). **Step 4:** Pass; commit `feat(taxiway): a taxiway is restricted to the largest letter its strip is clear for`.

---

### Task 4: Routing honours the restriction

**Files:** `Private/Build/RoadGuidelineBuilder.cpp:473` (straight edges) and turns `:1353-1358` (already Min), `Private/Model/ArrivalPlanner.cpp` (refusal wording for `TooWide` caused by a restriction), tests `RoadGuidelineBuilderTest.cpp`, `RouteSearchTest.cpp`, `ArrivalPlannerTest.cpp`.

- [ ] **Step 1: Failing tests:** a segment with `RestrictedLetter = C` derives edges with `MaxWingspan = IcaoCode::MaxWingspanForLetter(C)`; an unrestricted one keeps 0 (ruling 2); a junction turn from restricted to unrestricted takes C's; `RouteSearch` with a Code D wingspan (`IcaoCode::DesignSpanForLetter(D)`) on a route through the C-restricted segment returns `TooWide`; `ArrivalPlanner::DescribeRefusal` says `"... taxiway restricted to Code C by a service road - move it clear of the strip"` (Review Focus 5).
- [ ] **Step 2:** FAIL. **Step 3:** In the builder, `Edge.MaxWingspan = Declared.MaxWingspan` becomes: the declared value, then if `Segment.RestrictedLetter != 0xFF`, `Min` with the letter's max span (treating 0 as unlimited). Comment the ONE writer and ruling 2. Arrival wording: when the route result is `TooWide` and any edge on the size-ignored route derives from a restricted segment, name it (read `ArrivalPlanner`'s TooWide branch first). **Step 4:** Pass; full suite (no existing taxiway is restricted in fixtures unless a road sits in a strip - stage 3's fixture moves mostly cleared those; any that still restrict are real findings: report, then move the fixture road with a reason). Commit `feat(taxiway): routing refuses aircraft too wide for a restricted taxiway`.

---

### Task 5: The ghost shows what an upgrade would do; stands flagged

**Files:** `Private/Tool/RoadDrawTool.cpp` (hover over an existing taxiway with a different width chosen: preview the new strip outline in `EPreviewStyle::Guide`, label `"-> Code F: restricts to E (service road), closes stand 4"`, click calls `SetSegmentWidth`), `Private/Tool/GraphOverlay.cpp` (`DescribeStands`: a stand for which `TaxiwayStrip::WorstIntrusion` is set draws its outline `EPreviewStyle::Refused`), tests in the road tool test file and `GraphOverlay` test (grep `DescribeStands` tests).

- [ ] **Step 1: Failing tests:** the hover label lists the restriction and the closed stand by NUMBER (stage 5) or by index if stage 5 has not landed - check which is on main when implementing; the click re-profiles; hovering with the SAME width draws nothing new; `DescribeStands` emits a Refused polygon for a stand in a strip and the normal style otherwise.
- [ ] **Step 2:** FAIL. **Step 3:** Implement; the preview asks `WhySetSegmentWidthRefused` (one evaluator) and `TaxiwayRestriction::RestrictionOf` on a ghost copy of the network with the new profile (the ghost network precedent `BuildGhostBuffers`) - never a second rule. **Step 4:** Pass; commit `feat(taxiway): the width row upgrades an existing taxiway, showing what it restricts and closes`.

---

### Task 6: Inspector - taxiway card and closed stands

**Files:** `Public/Tool/Selection.h` (append `Taxiway`), `Private/Tool/SelectTool.cpp` (pick order: aircraft, entity, runway, then taxiway segment; `PositionOf`; preview), `Public/Model/InspectFacts.h/.cpp` (`struct FTaxiwayCardFacts { FString Letter; double Width; double Strip; TOptional<FString> RestrictedTo; FString RestrictedBy; };` `DescribeTaxiway(Network, SegmentIndex, Out)`; `FStandFacts` gains `FString ClosedBecause` from `StandAdmission::Describe` of an InsideStrip verdict, detailed like `WhyStandRefused`'s), `Source/AirportMgr/InspectorWidget.cpp` (Taxiway branch; stand branch shows `ClosedBecause`), tests `InspectFactsTest.cpp`, `InspectorWidgetTest.cpp`, and a consumer test that every `ESelectionKind` value has an inspector branch (iterate the UENUM; names, not counts).
- [ ] TDD as above: facts first (Model), then widget. Commit `feat(inspector): taxiway card; stands say why they are closed`.

---

### Task 7: Look at it

- [ ] Worktree editor (port 8002), M_Test: widen a taxiway next to a road and a stand; check the ghost label, the click, the restricted card, the flagged stand, and an arrival of a too-wide type refused with the restriction named in the log (`LogAirside`), plus `Restriction:` log lines. Screenshot; iterate the look live with the user.

## Unresolved questions (for the user)

1. Upgrade gesture: click an existing taxiway with a different width chosen on the Taxiway tool (ruling 1) - or a separate "Widen" variant/mode on the row?
2. Should an UNRESTRICTED taxiway limit aircraft to its own letter (an A380 refused on a 24 m E taxiway)? Realistic, but it changes every existing map's routing - planned as NO (ruling 2).
3. Pricing an upgrade: the new width's cost minus the old, no refund on a downgrade - OK?
4. Existing maps: after this lands, M_Test's roads that sit inside strips (left alone by stage 3) will restrict their taxiways - acceptable, or should the first load after this ship warn once per restricted taxiway?
