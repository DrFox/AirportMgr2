# Vehicle size gating (PR 3 of 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A vehicle is routed only where it fits - body width in the lane, steering lock and swept path through every corner - and a fuel job no road can serve says so.

**Architecture:** Measured body dimensions on `FVehicle` (+ optional `FTrailer`); a dependency-free steady-state envelope in `Solve/VehicleSweep`; the guideline builder MEASURES each turn's radius and its clearance to the pavement on the sample array the follower walks; route search filters on `VehicleFits(Edge, Vehicle)` beside its wingspan filter and answers `TooNarrow` from the same unconstrained retry that answers `TooWide`; `UFuelService` passes its truck and reports `EFuelRefusal::TooNarrow`.

**Spec:** `docs/superpowers/specs/2026-09-23-road-lanes-and-widths-design.md` §6 (tests 6-7). Stacked on #274, with #275 (bowser 6.2 m) cherry-picked.

## Global Constraints

- The STEERED axle rides the line (RouteFollower.cpp:237), so every radius here is the steered axle's; the fixed axle runs at sqrt(R^2 - L^2).
- Measured figures (glb, 2026-09-24): bowser wheelbase 360.7, body +481.8 / -138.2 from the fixed axle, width 237.4 (mirrors excluded). Rig tractor wheelbase 370, body +516 / -78, width 254, fifth wheel +57.3; trailer kingpin 1029.5 ahead of its tandem, body +166 ahead of the kingpin / -121.5 behind the tandem, width 254. Rig lock ASSUMED 40 deg (not in the model).
- Mirrors are excluded from widths: they overhang kerbs.
- Envelope is steady-state (conservative: a 90 deg turn never reaches it); said at the site.
- An unmeasured clearance (-1) or width (0) constrains nothing, so hand-made and legacy edges keep routing.
- Build/test as PR 1-2; commits carry no Co-Authored-By.

## Review Focus

1. Legacy/hand-authored edges with no measurements must not start refusing vehicles - Task 3 default test.
2. Aircraft queries (no vehicle) unchanged - wingspan-only path untouched; existing suite.
3. A route that is too narrow only at a stand lead-in (not a road) - lead-ins carry Width only; gating only their width (Task 4 test).
4. `TooNarrow` must name an edge so the log can point at it - Task 4 asserts `RejectedEdge` is set and on the unconstrained route.
5. The bowser must still reach every stand on existing maps - full suite + AirportOps fuel tests.

---

### Task 1: Vehicle body and trailer
Files: `Public/Model/Vehicle.h`, `Private/Content/AirsideSettings.cpp` (+ `ResolveRigVehicle` decl in `Public/Content/AirsideSettings.h`), test `VehicleBodyTest.cpp`.
- `FVehicle`: `BodyWidth`, `BodyFrontX`, `BodyRearX` (uu from the fixed axle; rear negative), `FTrailer Trailer` {`KingpinX` (on the tractor, from its fixed axle), `KingpinToAxle`, `FrontAheadOfKingpin`, `RearBehindAxle`, `Width`}; `HasTrailer()` = `KingpinToAxle > 0`.
- `ResolveDefaultVehicle` fills the bowser's; `ResolveRigVehicle()` the rig's (TypeCode "RIG").
- Test `Airside.Model.VehicleBody`: bowser body length (front - rear) equals `FTrafficRules::VehicleFootprint` (620) within 5 uu - the footprint the mesh test already pins, so the body is tied to the mesh through it; rig has a trailer, bowser does not.

### Task 2: Steady-state envelope
Files: `Public/Solve/VehicleSweep.h`, `Private/Solve/VehicleSweep.cpp`, test `VehicleSweepTest.cpp`.
- `FSweepBody` (plain doubles mirroring Task 1), `FEnvelope { double Inner, Outer; bool bHolds; }`, `Envelope(const FSweepBody&, double SteerRadius)`. Inner/Outer are offsets from the path (steered axle) toward/away from the turn centre. `bHolds` false when R < wheelbase or the trailer cannot hold the turn (kingpin radius <= KingpinToAxle).
- Test `Airside.Solve.VehicleSweep`: rig at R=1500 -> Inner ~599, Outer ~163 (hand figures in the plan's notes, within 5); bowser at R=1000 hand figures; R < wheelbase -> !bHolds; a larger R gives a smaller Inner (monotone).

### Task 3: Measured turn clearance on the graph
Files: `Public/Model/RoadGuideline.h` (FGuidelineEdge `MinRadius = 0`, `ClearInner = -1`, `ClearOuter = -1`), `Private/Build/RoadGuidelineBuilder.cpp`, test in `TwoWayLaneTest.cpp` or new `TurnClearanceTest.cpp`.
- Turn paths: `MinRadius` = `GuidelineGeom::TightestRadius`; clearances measured by marching 10 uu steps along each sample's inner and outer normals until the point leaves the pavement (junction `Boundary` without its appended centre, or any arm's ribbon quad), cap 3000; min over samples. Samples from `GuidelineGeom::Sample(A, Control, B)` - the same call SampleGuideline makes.
- Balloons: `MinRadius` measured, clearances left -1 (over grass by ruling - only the lock is checked).
- Tests `Airside.Build.TurnClearance`: a Narrow T-junction's turns carry MinRadius > 0 and both clearances > lane half-width; a straight lane edge keeps -1/-1/0; widening the fillet raises the inner clearance of the near-side turn.

### Task 4: Search gating and TooNarrow
Files: `Public/Model/RouteSearch.h` (`FRouteQuery::Vehicle` + `WithVehicle`, `ERouteResult::TooNarrow`, `FRoutePlan::RejectedEdge`), `Private/Model/RouteSearch.cpp`, `Public/Model/VehicleFit.h` + `.cpp` (`bool VehicleFits(const FGuidelineEdge&, const FVehicle&)`), test `VehicleGatingTest.cpp`.
- Fits: width - `max(BodyWidth, Trailer.Width) + 2 * 15 <= Edge.Width` when both > 0; curve (`MinRadius > 0`) - lock `TightestFollowableRadius() <= MinRadius`, envelope holds, `Inner <= ClearInner` and `Outer <= ClearOuter` where measured.
- Search: the wingspan filter's flag becomes `bIgnoreSize`; after a failure with a wingspan OR a vehicle, the unconstrained retry answers `TooWide` (aircraft) or `TooNarrow` (vehicle), and `RejectedEdge` = first step of the unconstrained plan the vehicle does not fit.
- Tests (spec 6): rig on a straight Narrow and Standard road -> TooNarrow with RejectedEdge set; rig on a straight Wide road -> found; bowser on Narrow T-junction turn -> found and `FSpeedProfile::Build` over its polyline with the bowser chassis reports no `bTighterThanLock`; a hand-made edge with no measurements admits the rig.

### Task 5: Wide corners sized for the rig
Files: `Tools/Python/build_road_profiles.py` (Wide tier `PreferredFilletRadius`), test `VehicleGatingTest.cpp` (`RigTurnsOnWide`).
- Test first: a Wide T-junction built from `MakeServiceRoadTransient(450, 60, Fillet)`, rig routes left AND right (both directions). Find the smallest fillet (whole metres) that passes; author it on the Wide asset with a comment carrying the number and date. Narrow/Standard keep the derived (bowser) fillet.

### Task 6: Fuel service
Files: `Plugins/AirportOps/.../FuelService.h/.cpp`, test in the AirportOps fuel tests.
- `ChooseDepot` / `SendTruckHome` queries `.WithVehicle(TruckVehicle)`; a depot route answering TooNarrow is remembered; if no depot was chosen and at least one was TooNarrow -> `EFuelRefusal::TooNarrow`, text "no road wide enough for FUEL", log `LogAirside`-family Warning naming the rejecting edge.
- Test: a depot joined to a stand only through a road hand-narrowed below the bowser -> `Why == TooNarrow`.
