# Runway in use - implementation plan

> Executed natively in-session (user: "continue through implementation"). Steps are checkboxes.

**Goal:** each runway has one player-chosen direction in use; both planners use only that end.
**Architecture:** `FRunwayFacts::InUse` (designator) + `RunwayQuery::InUseEnd` resolver; planners
choose ends only through it; runway selection + inspector button flips it.
**Spec:** `docs/superpowers/specs/2026-09-28-runway-in-use-design.md`

## Global constraints

- UE 5.8, worktree build with `-NoHotReloadFromIDE` if the editor is open on the main checkout.
- New UPROPERTY -> full build, not Live Coding.
- Every refactored `UE_LOG` survives; WHY comments travel.
- `Run-AirsideTests.ps1 -Project <worktree uproject>` is the authoritative run; read its
  `N run, N failed, N crashed` line.

## Review focus

1. A runway EXTENDED by drawing onto its end must keep the chain's direction, not reset to the
   new piece's draw direction - test in Task 1.
2. Re-classifying surface/approach (tool `Facts()` has `InUse == 0`) must not wipe the
   direction - test in Task 3.
3. A departure armed before a flip, re-armed by a rebuild after it, keeps its original roll
   (ruling 2) - test in Task 3.
4. Old levels load `InUse == 0` -> lower designator, deterministic - test in Task 1.
5. Clicking an aircraft or stand ON/near a runway still selects the aircraft/stand - Task 4.

---

### Task 1: data + resolver + draw-direction default

Files: `RunwayFacts.h`, `RunwayQuery.h/.cpp`, `RoadNetwork.h/.cpp` (forwarders, AddSegment),
test in `RunwayQueryTest.cpp`.

- [ ] `FRunwayFacts`: `UPROPERTY(EditAnywhere) int32 InUse = 0;` + `operator==`.
- [ ] `RunwayQuery::InUseEnd(Network, Either)`, `InUseRunwayAt`, `InUseRunwayNearest`; network forwarders.
- [ ] `URoadNetwork::AddSegment`: a new RUNWAY segment adopts a non-zero InUse from any other
      member of its chain, else `Designate(B - A)`. Split already overwrites with KeepFacts.
- [ ] Tests: drawn direction stored; extension adopts chain's; split keeps; rotated 30 deg
      still resolves; InUse 0 -> lower designator; InUseEnd flips with facts.

### Task 2: planners honour it

Files: `ArrivalPlanner.cpp`, `DeparturePlanner.cpp`, `Tools/Check-Architecture.ps1`, tests in
`DeparturePlannerTest.cpp`, `ArrivalPlannerTest.cpp`; existing tests that encode the old
nearest-end rule get an explicit InUse.

- [ ] Arrival step 1 + IsRunwayBusy -> `InUseRunwayNearest`. Departure Plan -> `InUseRunwayAt`.
      PlanAny: one probe per runway.
- [ ] NoExit sentence: plan overload names the designator + fix; figure-free one says the fix.
- [ ] Lint rule: those two files may not call `RunwayExtentAt(` / `NearestRunwayThreshold(`.
      Prove it red by reverting one call locally.
- [ ] Tests: depart from in-use end although other end's taxi shorter; flip reverses; arrival
      at in-use threshold with focus on the far side; NoExit wording.

### Task 3: facade + traffic

Files: `RoadEditFacade.cpp`, `GroundTraffic.cpp`, tests in `GroundTrafficTest.cpp` /
`RoadNetworkActorTest.cpp`.

- [ ] Facade `PlaceRunway`/`SetRunwayFacts`: `Facts.InUse == 0` means keep the strip's.
      SetRunwayFacts logs `Runway 09/27 in use: 27 (was 09)` when it changed.
- [ ] `ArmDepartureIfRunway`: Warning when armed direction != in use.
- [ ] Tests: actor PlaceRunway stores drawn direction; surface reclassify keeps InUse; flip via
      actor forwarder -> next DepartAgent armed the new way; armed-then-flip-then-rebuild keeps
      the old roll; deadlock regression (arrivals + departure ticked, never armed against in-use).

### Task 4: runway selection + facts

Files: `Tool/Selection.h`, `SelectTool.cpp`, `InspectFacts.h/.cpp`, `SelectToolTest.cpp`,
`InspectFactsTest.cpp`.

- [ ] `ESelectionKind::Runway` (Id = segment index); SelectTool falls through aircraft -> stand
      -> runway strip. `InspectFacts::DescribeRunway(Network, SegmentIndex, FRunwayCardFacts&)`.
- [ ] Tests: click on strip selects runway; stand wins; facts pair/in-use text.

### Task 5: game module - action, controller, card

Files: `BuildActions.cpp`, `RoadBuildController.h/.cpp`, `InspectorWidget.h/.cpp`,
`BuildActionsTest.cpp`.

- [ ] Action `selection.runway_in_use`, label "Use NN" (dynamic), enabled with runway selected.
- [ ] `ARoadBuildController::CanFlipSelectedRunway` / `FlipSelectedRunway`.
- [ ] Inspector runway card + button found by id.
- [ ] Test: action registered, flips through controller.

### Task 6: build, suite, PR

- [ ] Full build, `Run-AirsideTests.ps1`, Check-Architecture; PR with build/test lines; follow-up
      issue for the ground indicator.
