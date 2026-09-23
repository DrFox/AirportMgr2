# Road lanes, drive side and width tiers - design

2026-09-23. Branch `feature/road-lanes-and-widths`.

## Goal

Service roads become two-way, two-lane roads whose vehicles keep to one side of the
carriageway (airport-wide Left/Right). The player picks from three road widths, and a
vehicle is routed only down roads it physically fits: body width in the lane, and swept
path through the corners. Routes, as drawn by the overlay and walked by the follower,
run down the correct lane.

Unblocks the articulated fuel rig (truckCab1 + tankTrailer1), whose import is parked on
this work (see memory `articulated-vehicles-decisions`).

## User rulings (2026-09-23)

| Question | Ruling |
|---|---|
| What "struggle" means | Both: too wide for the road AND can't make the turns |
| Drive-side scope | Airport-wide toggle, default Right |
| Width tiers | Three, all two-way: Narrow 2x3.0 m, Standard 2x3.5 m, Wide 2x4.5 m |
| No route fits the vehicle | Fail and tell the player. No fallback to a smaller vehicle |
| Dead ends | Derived U-turn guideline, no surface mesh bulb |

Decided in design, open to override: taxiways and runways unchanged (one bidirectional
centreline); stand/depot links join the correct-side lane; the existing single service
road profile maps to Standard.

## Rejected approaches

- **Centreline graph, follower offsets sideways at runtime.** A second evaluator of the
  path: breaks the guideline graph's sample-once invariant (CLAUDE.md), so the agent
  leaves the line the player was shown, and junction turns cut the wrong corner.
- **Each lane a separate model segment.** Doubles the edit model, undo, snapping and the
  junction solver for something the profile already expresses.

Chosen: lanes are declared by the profile and the side is applied when guidelines are
derived. `RoadProfile.h:59-62` already anticipates this ("a two-lane road declares two
with mirrored offsets and opposing directions"); route search already honours one-way
edges (`URoadNetwork::ForEachOutgoingGuideline`, `RoadNetwork.cpp:1072`).

## 1. Profiles and tiers

- `Tools/Python/build_road_profiles.py` authors three service road profiles:
  kerb | lane | lane | kerb, lane widths 300 / 350 / 450 uu, kerbs 60 uu unchanged.
  Each declares two `FProfileGuideline`s, `Class = GroundVehicle`, `Width` = lane width,
  `CentreOffset = +/- lane/2`, `Direction` AToB for the right-hand lane (negative
  offset) and BToA for the left-hand lane. That is the Right-drive layout; see 2.
- `UAirsideContent::ServiceRoadProfile` (`AirsideContent.h:182`) becomes
  `ServiceRoadProfiles` (ordered narrow to wide), with `GetServiceRoadProfileCount` /
  resolve-by-index beside the taxiway pair on `IRoadEditTarget` (`RoadEditTarget.h:208-222`).
  One resolver in `UAirsideSettings`, per the one-source-of-truth rule.
- `FRoadDrawTool::OnReselect` (`RoadDrawTool.cpp:400-408`) stops refusing ServiceRoad and
  cycles the tier; `ARoadNetworkActor::ResolveProfileFor` (`RoadNetworkActor.cpp:934`)
  honours `WidthIndex` for roads. The tier name shows where the taxiway code does.
- Each tier names a **design vehicle**; its fillet is solved so that vehicle's swept path
  fits (section 6). Narrow and Standard: the bowser (Standard's extra 0.5 m per lane
  is clearance, not a bigger vehicle - nothing between bowser and rig exists yet).
  Wide: the rig. Replaces
  `ResolveLargestServiceVehicle` as the fillet source for roads.
- Existing levels: today's road is a 600 uu lane + 60 uu kerbs (`build_road_profiles.py:52-53`),
  a 6.0 m carriageway - exactly Narrow (2 x 3.0 m), not Standard (7.0 m). The old profile
  asset is kept as the **Narrow** tier (same path, same total width), so placed roads
  re-derive as Narrow two-lane roads and nothing on the map changes width. The bowser
  (~2.5 m) stays admitted on Narrow (~0.25 m each side), so fuel service on existing
  maps keeps working. No player saves exist
  (memory `no-player-saves-yet`); note the break in the PR anyway.

## 2. Drive side

- `enum class EDriveSide : uint8 { Right, Left }` as a UPROPERTY on `URoadNetwork`,
  saved, default Right.
- Applied in `RoadGuidelineBuilder` when a segment's edges are derived: under Left,
  negate every road guideline's `CentreOffset` (Direction stays tied to A/B, so the
  lane that was right-of-travel is now left-of-travel). Taxiway/runway guidelines at
  offset 0 are unaffected by construction.
- `URoadNetwork::SetDriveSide` re-derives all road guidelines, is undoable through the
  existing Memento, and logs `LogAirside: Drive side -> Left, <N> road lanes re-derived`.
- A toggle in the bottom bar (memory `ui-bottom-bar-cities-skylines-style`). Agents
  mid-route when it flips: their plans are invalidated and re-requested, same path as an
  edit that deletes an edge under them.

## 3. Junction turn paths

Current pairing is by guideline index over `min(From.Num, To.Num)`
(`RoadGuidelineBuilder.cpp:485-490`): wrong once offsets are relative to each segment's
A/B and arms meet at mixed ends, and it drops lanes when counts differ.

Replace with: for each ordered pair of distinct arms (From, To), connect every guideline
of From that may **arrive** at the node to every guideline of To that may **leave** it
(`bMayArrive` / `bMayLeave`, `:512-533`), subject to class match. With two-lane roads
that is exactly one turn per arm pair. Bidirectional guidelines (taxiways) both arrive
and leave, so taxiway behaviour is unchanged. A 1-lane arm meeting a 2-lane arm connects.
Still no U-turns at junctions (`From == To` skipped, `:455`).

Turn control point stays the junction node (`:545`) so samples stay on one quadratic.

## 4. Dead ends

A node with exactly one road arm gets a derived U-turn edge from that arm's arriving
lane end to its leaving lane end. Geometry: quadratic whose control point sits beyond the
node on the segment axis, giving a radius about half the lane spacing. No mesh bulb
(ruled): vehicles visibly leave the tarmac turning. The turn-radius check in section 6
means large vehicles cannot use it and report "dead end too tight". Edge flagged
`bDerived`, rebuilt with the segment.

## 5. Stand and depot links

`AnchorLinkFinder` (`AnchorLinkFinder.cpp:190-235`) links to the nearest allowed
guideline, which on a two-lane road is one lane - so traffic in the other lane could
never reach the anchor without a loop. Change: an anchor on a two-lane road gets one
one-way **inbound** link from each lane and one one-way **outbound** link into each lane,
as a driveway does (turning across oncoming traffic is allowed at an anchor; U-turning
in the carriageway is not). Route search picks among them by cost.

## 6. Vehicle size and gating

Model:

- `FVehicle` (`Vehicle.h:26`) gains `BodyWidth` and an optional `FTrailer`
  { `KingpinAheadOfRearAxle`, `KingpinToTrailerAxle`, `TrailerWidth` }.
  Rig figures from the glb: tractor wheelbase 3.70 m, fifth wheel 0.573 m ahead of the
  rear axle, kingpin 10.295 m ahead of the tandem centre, cab 2.94 m wide incl. mirrors,
  trailer 2.54 m (mesh-local accessor bounds, measured 2026-09-23).
- `Solve/`: `VehicleSweep::Envelope(FVehicle, Radius)` -> {outer, inner} offsets from the
  path, steady-state tractrix off-tracking. Steady state is conservative (a 90 deg turn
  never reaches it); stated at the site. Measured for the rig: swept width 7.6 m at
  R = 15 m, 6.2 m at R = 20 m.

Graph:

- Straight lane edges already carry `Width` (lane width).
- Turn and U-turn edges gain `MinRadius` and `ClearLeft` / `ClearRight`: distance from the
  sampled path to the junction polygon boundary on each side, measured on the **same
  sample array** the follower walks (sample-once), minimum over samples.

Search:

- `FRouteQuery` gains `const FVehicle* Vehicle` (null = unconstrained, as today for
  aircraft). `ExpandNode` (`RouteSearch.cpp:149-260`) rejects: a straight edge whose
  `Width < BodyWidth + margin`; a turn edge whose `MinRadius <`
  `Chassis.TightestFollowableRadius()` or whose envelope at `MinRadius` exceeds
  `ClearLeft` / `ClearRight`. One predicate, `VehicleFits(Edge, Vehicle)`, in `Model/`.
- On failure, re-run unconstrained exactly as `TooWide` does (`RouteSearch.cpp:624-636`)
  and return `ERouteResult::TooNarrow` plus the first rejecting edge.
- `FSpeedProfile` stays the drivability authority (memory
  `airside-speedprofile-is-the-drivability-authority`): tests run it over whole admitted
  routes and assert no `bTighterThanLock`, rather than re-implementing the rule per edge.

Reporting:

- `UFuelService` (`FuelService.cpp:112-290`, `ChooseDepot`) treats TooNarrow as no
  candidate; if every depot is TooNarrow the job stays unserved and the stand carries a
  reason, "No road wide enough for <TypeCode>", shown in the entity inspector and logged
  `LogAirside: TooNarrow <TypeCode> at edge <id> (<tier>)`. No fallback vehicle (ruled).
- Callers pass the vehicle: `FuelService` currently queries with `Wingspan = 0`
  (`:229-232`) and no vehicle.

## 7. Lane markings

Requested 2026-09-23. `FRoadLaneMarkingBuilder` in `Build/`, beside
`FRunwayMarkingBuilder` and `FHoldingPositionMarkingBuilder` and for the same reason
(paint landing on a road vertex must not perturb the bitwise-welded surface): quads for a
marking component of its own, UV1 = 0 painting solid `MarkingColor`, white instance.

- A dashed centre line between the two lanes of every two-lane road segment: 100 mm
  wide, 3 m on / 6 m off (tunable constants, unjudged until seen), phase starting at each
  cut line so dashes never enter a junction polygon.
- Drawn on the segment's surface centreline (offset 0), which the drive side does not
  move, so flipping side repaints nothing.
- Nothing stored: derived per build, like runway paint. A census count
  (`LogAirside: Lane markings: <N> dashes on <M> segments`) for the log and tests.
- Out of scope: edge lines, direction arrows, give-way lines at junctions.

## 8. Testing

World-free `Model/` / `Solve/` tests written first, each red before its code:

1. Two-lane profile derives two one-way edges with offsets of opposite sign.
2. Right drive: every sample of a routed polyline along a straight road lies right of the
   direction of travel; flip to Left and it lies left. (Measures the lane, not a flag.)
3. 1-lane arm meets 2-lane arm: route exists through the junction both ways.
4. T-junction, Right drive: a left turn and a right turn each start on the right lane and
   end on the right lane of the new road.
5. Dead-end stub: out-and-back route exists for the bowser, via the U-turn edge; rig
   reports TooNarrow there.
6. Rig: TooNarrow on Narrow and Standard, admitted on Wide straight and through a Wide
   junction; `FSpeedProfile` over the admitted route has no `bTighterThanLock`.
7. `VehicleSweep::Envelope` against hand figures (7.6 m at 15 m).
8. Anchor on a two-lane road gets an inbound and an outbound link on each lane; a
   route arriving in either direction reaches it without a detour.
9. Composition: spawn `ARoadNetworkActor`, toggle drive side, tick; agent re-plans and
   ends on the new side (seam test for `SetDriveSide`).
10. Lane markings: every dash lies within the segment between its cut lines, centred on
    offset 0; none inside a junction polygon; count matches length / pitch.

Each rule also gets the "delete it and watch the test go red" check (memory
`a-green-test-may-measure-nothing`).

After the suite: a map screenshot with the lanes overlay on (memory
`graph-changes-need-a-look-at-the-map`) - Right, then Left.

Lint: a Check-Architecture rule that `RoadGuidelineBuilder` never pairs guidelines by
index across arms (the shape section 3 removes).

## Delivery

Three PRs, in order, each built and tested:

1. Two-way lanes, drive side, junction pairing, dead-end U-turn, anchor links, lane
   markings (sections 2-5, 7, tests 1-5, 8-10). The existing road becomes the Narrow
   two-lane profile.
2. Width tiers and tool cycling (section 1).
3. Vehicle size, sweep, gating, TooNarrow reporting (section 6, tests 6-7).

The rig import (articulated step 2) follows PR 3.

## Open

- Per-edge search cost of `VehicleFits` is a few comparisons; no concern at ~hundreds of
  edges (2026-09-23).
- Dash length/gap and line width are first guesses; judge on screen after PR 1.
