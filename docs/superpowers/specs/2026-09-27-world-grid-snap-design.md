# World grid snap - design

2026-09-27. Branch `feature/world-grid-snap`.

## Why

Player drew a stand at the same depth as the previous one; the back edges did not line up.
Cause, from the code: a stand is placed relative to its taxiway segment, not the world.

- `PlotGesture::AnchorAt` quantises the anchor on a 5 m grid measured from the SEGMENT's A end
  (`PlotGesture.cpp`, "ANCHORED ON THE ROAD'S OWN BAY GRID"), then offsets it by that segment's
  kerb half-width. Two segments = two grid phases and two kerb lines.
- Depth is measured from the entrance line (`FStandPlotTool::Shape`), so equal depth means equal
  back-edge position only on the same segment, same width, same phase.
- `FStandPlotTool` has no `DescribeGuideAnchor`, so the guide chain never runs for it and nothing
  corrects the offset - although stand outline edges are already guide lines (Collinear x Stand,
  `SnapGuideChain.cpp` `GuideOutlineColumn`, added 2026-09-24).

## Outcome

1. A world-aligned grid (Off / 1 / 5 / 10 m) every guided builder snaps to, set from the bar's
   snap section.
2. A faint local grid overlay around the build point while the grid is on.
3. Two stands dragged to the same depth on parallel taxiways have bitwise-equal back edges,
   whether or not the grid is on (neighbour guide), and on any taxiway heading.

## Rulings (from the brainstorm)

- **Scope B:** world grid for all guided builders AND stands gain the guide chain for depth.
- **Precedence A:** road snap > guides > grid.
  - 0 guide winners: round X and Y to the grid.
  - 1 winner: slide along the guide line to its nearest grid-line crossing.
  - 2 winners: their crossing stands; the grid does not move it.
- **Control:** one bar button cycling Off -> 1 m -> 5 m -> 10 m; overlay radius scales with step
  (20 / 60 / 120 m).
- **Stands, grid on:** the world grid REPLACES the built-in plot quanta (per-segment 5 m anchor
  step, 5 m frontage step, 0.5 m depth step). Grid off: today's behaviour, unchanged.
- Grid is axis-aligned at the world origin. Guides are not replaced.

## Design

### 1. Core

- `FSnapGuideSettings` gains `GridStep` (enum `EGridStep { Off, OneMetre, FiveMetres,
  TenMetres }`; a phase is an enum, never bools). Stored on `ARoadNetworkActor::GuideSources`,
  so editor mode and PIE agree.
- Carried in `FBuildSessionTunables` and added to its frame-cache equality check - otherwise a
  step change keeps the cached frame's snap.
- New `Solve/GridSnap.h` (`CoreMinimal.h` only):
  - `double StepUu(EGridStep)` - 0 / 100 / 500 / 1000 uu. The only place the figures live.
  - `FVector2D Quantise(Point, Step)` - round both axes (0 winners).
  - `bool NearestCrossingAlong(Origin, Direction, Point, Step, FVector2D& Out)` - the point on the
    line Origin + t*Direction nearest to Point that lies on an X=k*Step or Y=k*Step line. Axis-
    parallel lines cross only the perpendicular family; returns false for a zero Step or
    direction and writes Out only on success.
  - `LinesInDisc(Centre, Radius, Step, TArray<FGridPiece>&)` - overlay pieces, one cell long,
    clipped to the disc, each tagged major (every 5th line from the origin) or minor.
  - Rounding is to NEAREST via `FMath::RoundToDouble`, never an int cast (truncates toward zero on
    negative coordinates).
- `FSnapGuideChain::Resolve` applies the grid LAST, after `SnapGuide::Arbitrate`, per rule A.
  Result lands in `FToolContext::Guide`; every guided tool reads it through `GuidedCursor()`.
- Road snap is untouched: resolved earlier in `FBuildSession::MakeContext`, and
  `RoadGuidedSnap` applies the guide only when the road snap is Free.
- `bSuspendGuides` (Alt hold) suspends the grid too - it is one "let me place freely" gesture.
- Rejected: grid lines as an `IGuideSource` (would compete in arbitration and hysteresis and
  take one of the two per-`EFit` winner slots from a real guide, contradicting rule A);
  per-tool rounding (how the per-segment phase happened).

### 2. Stands and plots

- **Anchor (click 1), grid on:** `PlotGesture::AnchorAt` slides the anchor along the kerb line to
  its nearest grid crossing (`NearestCrossingAlong`). Shared with the fuel depot - one rule, every
  plot tool, as `AnchorAt` already insists. The per-segment rule's rationale (flush on a segment,
  never straddle a junction) is kept for grid off; the design doc it cites records the new case.
- **Frontage (click 2), grid on:** far end slides to the nearest crossing along the kerb line.
  Grid off: `QuantisedFrontage` as now.
- **Depth (click 3):**
  - `FStandPlotTool` overrides `DescribeGuideAnchor` while `PinnedCount() == 2`: anchor at the far
    entrance corner, reference direction = entrance edge.
  - The existing Collinear x Stand outline source then proposes neighbouring stands' edges.
    Candidates restricted to lines parallel to the entrance edge (within the chain's angle
    tolerance) if `FGuideAnchor` can express it; plan verifies against the header, else the tool
    filters.
  - Depth = projection of `GuidedCursor()` onto `Inward`, as now. Order: neighbour guide >
    grid crossing along Inward > 0.5 m step (grid off).
  - A pinned back corner stays `Corners[2]` as stored (the existing ulp comment).
- Known trade-off: on a diagonal taxiway with the grid on, depths are odd lengths (e.g. 7.07 m)
  because they land on world crossings. The neighbour guide covers aligning stands there.
- `StandPlotRules::DepthStepUu` floor test still holds for grid off; grid-on depths are not
  multiples of it, so any test asserting that must scope itself to grid off.
- Out of scope: side-edge guides across a taxiway.

### 3. Bar button and overlay

- `snap.grid` action, `EActionSection::Snap`, no key (the registry's NO KEYS rule for snap
  toggles). Click calls new `ARoadBuildController::CycleGridStep`; caption "Grid: off" /
  "Grid: 1 m" / ...; shown on while step != Off. Logs `LogRoadBuild: Grid step -> <n> m`.
- Overlay drawn by `FBuildSession` into `IToolPreviewSink` BEFORE the tool's preview, so
  `ARoadBuildHUD` and `FViewportPreviewSink` both get it with no per-driver code.
  - Centre: `GuidedCursor()`, so it shows the lines the point is landing on.
  - Radius per step: 20 / 60 / 120 m (lives beside `StepUu`).
  - Pieces one cell long: the HUD culls a line if either end projects > 64 px off-screen.
  - New `EPreviewStyle::GridMinor` / `GridMajor`, appended at the END of the UENUM; faint colours
    in `PreviewPalette` and the HUD's style map (every list that must agree - check consumers).
  - Only while a build tool other than Select is active and step != Off.
- No edge fade this round: the sink speaks meanings, not alpha; fade would change its API. Hard
  circle of faint lines; fade is a follow-up if it looks harsh.
- Piece count logged once per step change (`LogAirside: Grid overlay <n> pieces at <step>`) -
  ~1600 at 1 m / 20 m was estimated, not measured.

## Tests

- `Airside.Solve.GridSnap`: Quantise at 1/5/10 m; negative coordinates round to nearest; Off
  returns input bitwise-equal; NearestCrossingAlong on axis-parallel and diagonal lines, zero
  direction refused; LinesInDisc count, clipping, major every 5th.
- `Airside.Tool.SnapGuideChain` (or existing chain suite): 0/1/2 winners x grid on, per rule A;
  grid off leaves Resolve bitwise-unchanged.
- `Airside.Tool.StandPlot`:
  - THE REPRO: two stands on two parallel taxiway segments with different A-end phases; second
    dragged near the first's depth -> back edges bitwise-equal (grid off and on).
  - Grid on, no neighbour: back edge on a grid line.
  - Grid off: every existing stand and plot test unchanged.
- `Airside.Tool.PlotGesture`: grid on, two segments with different A-end phases -> anchors on one
  world grid line.
- Session seam: spy sink sees GridMinor pieces with grid on, none with it off (fails if the
  session never emits).
- Registry: `snap.grid` present in the Snap section (bar identity check covers the rest).
- Tunables: changing `GridStep` invalidates the frame cache.

## Verification in PIE

Grid 5 m, stand tool: overlay ring visible around cursor; `Grid step -> 5 m` in
`Saved/Logs/AirportMgr.log`; two stands on different parallel segments, second dragged to the
first's depth, back edges visually flush (`python Tools/Mcp.py shot out.png editor`).

## Open

- Whether `FGuideAnchor` can restrict candidates to one direction (plan checks header).
- `UPROPERTY` on the settings struct means a full build (editor closed or worktree build).
