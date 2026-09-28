# Grid follows the snap - design

2026-09-28. Branch `feature/grid-follows-snap`. Follows `2026-09-27-world-grid-snap-design.md`.

## Why

Reported 2026-09-28, verbatim:

> *my understanding was the grid orientation was going to change in the context of what was
> being build/selected but that doesnt seem to be what happens. The grid seems to always be
> oriented to the world*

Working as specified: the world-grid spec ruled "Grid is axis-aligned at the world origin", and
"local overlay" meant local in EXTENT (a disc round the cursor), not in orientation. The reading
the player had is the better design; this spec revises that ruling (design decisions are
revisable, with the reason recorded - this paragraph is the reason).

## Outcome

1. With **Follow** on, the grid turns to the thing being snapped to, and holds the last frame
   when nothing is.
2. With **World** on, today's behaviour, bitwise.
3. A bar button and the key H switch between them.

## Rulings (from the brainstorm)

- **Orientation source: the thing being snapped to** - not the selection, not the last segment
  drawn.
- **Toggle A:** `Follow` (held last frame as the fallback) / `World` (always axis-aligned). One
  toggle covers both "fall back to world" and "keep the last" and stops the grid flipping as the
  cursor passes near several things.
- **Phase 1:** one grid line runs along the thing's CENTRELINE; the lines across it are phased
  from the world origin. Rejected: the world grid rotated about the origin (the thing sits
  between lines - odd depths, the diagonal complaint rotated); origin at the thing's own point
  (a phase per segment - the bug the world grid fixed).
- **A key, deliberately.** H, an explicit exception to the registry's NO KEYS rule for snap
  toggles: the player asked for one, and this is a mode switched mid-gesture.

## Design

### 1. Frame

- `Solve/GridSnap.h` gains `FGridFrame { FVector2D Origin; FVector2D Axis /*unit*/; double StepUu; }`.
  - `FGridFrame::World(StepUu)` - origin 0, axis (1,0).
  - `FGridFrame::Along(Point, Direction, StepUu)`:
    - Axis = Direction folded into [0, 90) degrees. A square grid is the same after a quarter
      turn, so A->B and B->A, 30 and 120 degrees, give ONE grid.
    - Origin = the foot of the world origin on the line through Point. One line along the
      thing's centreline; cross lines through the foot, so collinear segments share them and
      parallel lines (whose feet lie on one perpendicular through the origin) share them too.
    - Zero Direction: returns `World(StepUu)`.
  - `bool IsOn() const` - StepUu > 0. Every step <= 0 stays "no grid", answered by each function.
- `Quantise`, `NearestCrossingAlong`, `NearestCrossingInRange`, `PiecesInDisc` take a
  `const FGridFrame&` instead of `double StepUu`. Each maps into the frame (u = (P-O).Axis,
  v = (P-O).Perp), does today's arithmetic, maps back.
- **World must be bitwise equal to today.** With O = 0 and Axis = (1,0) the dot products and the
  map back are exact in IEEE doubles; a test asserts it on the existing GridSnap inputs rather
  than trusting that argument. If it ever fails, World takes a fast path that skips the map.
- Major lines stay every fifth line FROM THE FRAME ORIGIN.

### 2. What sets the frame (Follow)

First match wins, resolved once per frame in `FBuildSession::MakeContext` after the guide chain:

1. **A guide winner** whose relation is `Parallel`, `Collinear` or `Offset` and whose reference is
   a road, runway or stand: `Along(Winner.Through, Winner.Direction)`. With two such winners, the
   first ranked. Excluded: `Angled`, `Extending`, `LevelWith`, and the World / ThisGesture
   references - following "45 degrees to the taxiway" would turn the grid 45 degrees off the
   thing meant. The plan verifies which fields name the relation and reference on
   `SnapGuide::FCandidate`.
2. **The tool's grid line** - `IBuildTool::DescribeGridLine`, added in implementation (plan,
   "Refinement"): the plot tools find their taxiway themselves, so neither a guide nor the road
   snap names it. The stand/depot tools return the centreline of the road `AnchorAt` would take
   (idle) or took (pinned).
3. **The tool's anchor reference** - `FGuideAnchor::Reference` through `Origin` (the stand's
   entrance edge; the segment a road extends).
4. **The road snap segment** under the cursor (from `FBuildSession`'s road snap, already resolved
   earlier in `MakeContext`).
5. **Nothing:** the held frame. The session's held frame starts as World.

- Held on `FBuildSession` beside `LastGuide`; survives tool switches within the session; NOT reset
  by Alt (Alt suspends the grid, it does not forget where it pointed).
- The step always comes from `GridStep`; only origin and axis are held.

### 3. Consumers

- `FToolContext::GridFrame` replaces `GridStepUu`. Every reader moves to it:
  `FSnapGuideChain::ApplyGrid`, `PlotGesture::AnchorAt` / `FrontageEnd` / the depth step,
  `PlotPlaceTool`, `StagedPlotTool`, `StandPlotTool`, `GridOverlay`. Grep `GridStepUu` after -
  zero readers left (CLAUDE.md: check where a value is CONSUMED).
- **Order problem the plan must settle:** `ApplyGrid` runs INSIDE `FSnapGuideChain::Resolve`, but
  the frame depends on Resolve's winners. Resolution: `Resolve` stops applying the grid;
  `MakeContext` calls Resolve, derives the frame from its winners, then `ApplyGrid(Result, Cursor,
  Frame)`. The no-network branch already calls `ApplyGrid` itself and keeps doing so.
- Plots, with Follow on and the anchor on a segment: the kerb line is parallel to the frame axis,
  so crossings along it are the cross family only - world-phased, shared by collinear segments.

### 4. Setting, toggle, key

- `FSnapGuideSettings` gains `UPROPERTY EGridOrientation GridOrientation = Follow`
  (`enum class EGridOrientation : uint8 { Follow, World }`, UENUM - a phase is an enum). Stored on
  `ARoadNetworkActor::GuideSources`. Added to `FBuildSessionTunables`' frame-cache equality check.
  **UPROPERTY: full build.**
- Behaviour change for existing levels: default Follow means a saved level with the grid on now
  turns. No player saves (2026-09-23 ruling); note it in the PR.
- `snap.gridorient` action, `EActionSection::Snap`, beside `snap.grid`; caption
  "Grid: follow" / "Grid: world". Key H - plan checks H is unbound in BOTH drivers
  (`ARoadBuildController`, `URoadBuildEdMode`) and in the registry.
- `ARoadBuildController::ToggleGridOrientation`; editor mode gets the same through the registry.
  Logs `LogRoadBuild: Grid orientation -> follow|world`.
- The registry's NO KEYS comment gains the exception, dated, with this spec's name.

### 5. Overlay and logs

- `GridOverlay` draws `PiecesInDisc(Context.GridFrame, ...)`: the disc turns with the grid, which
  is how the player sees what it followed.
- `LogAirside: Grid frame -> <deg> deg through (<x>, <y>) from <winner|anchor|roadsnap|held|world>`
  once per CHANGE of frame, not per tick.

## Tests

- `Airside.Solve.GridSnap`:
  - World frame bitwise equal to the pre-change functions on the existing inputs.
  - `Along`: a line and its reverse give an identical frame; 30 and 120 degrees identical; zero
    direction gives World.
  - Rotated `Quantise` result lies on a frame line (u, v integral multiples of step, within the
    rotation's ulp - this is a rotated frame, not the bitwise surface weld).
  - Two collinear segments' frames share cross lines; two parallel lines one step apart share both
    families.
  - `PiecesInDisc` in a rotated frame: every piece parallel or perpendicular to Axis; major every
    fifth from the frame origin.
- Frame resolver (chain suite or new `Airside.Tool.GridFrame`), one per source:
  - Parallel winner turns the grid; Angled winner does not.
  - Anchor reference applies with no winner.
  - Road snap applies last.
  - Nothing snapped holds the previous frame.
  - World ignores all of the above.
- `Airside.Tool.StandPlot`, 30-degree taxiway, Follow on:
  - back edge parallel to the taxiway, a whole number of steps from its centreline;
  - two stands on two collinear segments: frontage ends on shared cross lines.
  - World: every existing stand/plot grid test unchanged.
- Session seam: spy sink sees overlay pieces rotated with Follow, axis-aligned with World - fails if
  the session never passes the frame.
- Registry: `snap.gridorient` present in Snap; H bound in both drivers, checked by name.
- Tunables: changing `GridOrientation` invalidates the frame cache.

## Verification in PIE

Grid 5 m, Follow, stand tool on a diagonal taxiway: overlay disc turns to the taxiway;
`Grid frame -> <deg> deg ... from anchor` in `Saved/Logs/AirportMgr.log`; press H, disc goes
axis-aligned and `Grid orientation -> world` logs. `python Tools/Mcp.py shot out.png editor`.

## Out of scope

- Edge fade on the overlay disc.
- Orienting to a selected object when no build tool is active.
