# Chainlink fence round drawn plots — design

2026-09-22. Replaces the grey-box fence panels in `UPlotPresenter` with the chainlink kit
in `AirportMgr2Models/accessories/chainlink` (README there is the asset contract). Two PRs:
the buildings-presentation split first, the fence on top of it.

## Why two PRs

`ARoadNetworkActor` owns the plot ISMs and `UPlotPresenter` although buildings are not
road network. The fence would add three more components there. The MODEL stays put -
entities are `URoadNetwork::Entities`, read by 19 files and covered by the one undo
Memento - so only presentation moves. Per-plot actors were rejected for now: they need a
spawn/destroy index kept in step with the model through undo, the second index
`UPlotPresenter::RebuildFrom`'s comment already refused. Revisit when per-building
selection needs a click target.

## PR 1 — `AAirsideBuildingsActor` (refactor, no behaviour change)

- New `Present/AirsideBuildingsActor`. Owns `UPlotPresenter` (`CreateDefaultSubobject`),
  `ModuleBoxes`, `ModuleGhosts` (moved from `PlotBoxes` / `PlotGhostBoxes`).
- Road actor: `EditInstanceOnly TObjectPtr<ARoadNetworkActor> Network`; empty falls back to
  the single one in the world; zero or several logs a Warning with the count and builds
  nothing. Bind in `PostRegisterAllComponents` (editor worlds and PIE both), unbind in
  `UnregisterAllComponents`.
- `ARoadNetworkActor` broadcasts `OnTopologyRebuilt(const URoadNetwork&)` exactly where it
  calls `Plots->RebuildFrom` today (`RoadNetworkActor.cpp:763`) - after the surface, before
  Traffic. Geometry and Markings rebuilds still do not broadcast.
- `UPlotPresenter::Actor()` retargets to the buildings actor. `ResolveDepotKits` and the
  ghost material are reached through its road-actor pointer (still one resolver each).
- `GetPlotPresenter` is REMOVED from the road actor, not forwarded: it is plain C++ with only
  test callers, and a forwarder needs a road-to-buildings pointer that undoes the split. Tests
  read `FAirsideTestWorld::Buildings`. Every road-network creation site (fixture, editor mode,
  editor tool, PIE controller) calls `AAirsideBuildingsActor::FindOrCreate` beside it.
- M_Starter was saved with the old `PlotBoxes`; the road actor sweeps it by name on
  registration, and the level is resaved.
- Place the actor in every `.umap` either driver uses (headless level edit; verify on disk,
  see memory on locked .umaps; fall back to a manual drag-in step).
- Refactor contract: `UE_LOG(` and comment-line counts before/after in the PR body.

Tests:
- `Airside.Present.BuildingsActorDrawsThroughTheDelegate` - road actor and depot first, then
  the buildings actor: it binds unasked, catches up, and empties on the next topology rebuild.
  Verified red with the Broadcast commented out.
- `Airside.Present.BuildingsActorAloneDrawsNothing` - road actor absent: no crash, nothing
  built, the Warning line emitted.
- `Airside.Present.BuildingsActorFindOrCreateIsIdempotent` - three drivers call it.
- Existing `PlotPresenterTest` cases move to the buildings actor and stay green unchanged.

## PR 2 — the fence

### Geometry: `Solve/FenceLayout` (CoreMinimal only)

```
FenceLayout::Solve(Outline, Gate, Spec) -> FenceLayout::FLayout
  FSpec   SpacingUu 250, TileUu 240, FaceOffsetUu 3, GateWidthUu   (height is the presenter's)
  FLayout Posts[] {Position, YawRad, Kind: Line | Corner | Gate}
               Spans[] {A, B, U0, U1}
               bHasGate, GateCentre   (no edge index: the solver may reverse the ring)
```

- Corner post at every outline vertex, yawed to the interior-angle bisector.
- Gate: the edge containing `Gate` is cut at `Gate ± GateWidthUu/2`; a Gate post at each
  cut. Caller passes `PlotYard::GateCorridorUu` (6.2 m) - the gap and the lane the yard
  solver keeps clear are one number. A cut within one spacing of a corner slides the gate
  along the edge to clear it. An edge too short for gate plus two spacings: no gate,
  `bHasGate = false`.
- Every run between fixed posts (corner-corner, corner-gate) spaced independently:
  `n = max(1, round(L / SpacingUu))`, line posts every `L / n`.
- Spans: one per adjacent post pair, offset `FaceOffsetUu` OUTWARD. Outward comes from the
  signed area (as `PlotYard::InwardOf`), never the stored winding. At a corner the two
  edges' offset lines are mitred to their intersection.
- U = distance along the edge / `TileUu`, continuous along an edge, reset per edge.
  V = 0..1 over `FabricHeightUu`.

Tests `Airside.Solve.FenceLayout.*`: post counts per kind on a 10 x 10 m square; a Corner
post on every vertex at the bisector; gate width exactly `GateWidthUu` and centred on
`Gate`; gate near a corner slides and clears it; short gate edge gives no gate; every span
endpoint 3 uu outside the outline; CW and CCW input give the same layout; every run's
stretch within ±12% of `SpacingUu`.

### Presentation (on `AAirsideBuildingsActor`)

- `FencePosts` HISM (`SM_Fence_Post`), `FenceHeavyPosts` HISM (`SM_Fence_CornerPost`,
  corner and gate posts - one mesh, the README's "heavier instead of braced" post),
  `FenceFabric` `UDynamicMeshComponent` with `bAffectDistanceFieldLighting = false`, no
  Nanite, shadows on.
- `UPlotPresenter::Initialise` takes one `FFenceTargets {Posts, HeavyPosts, Fabric}`.
- The fence loop (`PlotPresenter.cpp:319-359`) becomes `FenceLayout::Solve` plus instances
  plus one `FRoadMeshBuffers` strip through `FDynamicMeshSink`. Spans share NO vertices
  across posts: the sink computes per-vertex normals and a shared corner vertex would
  average two edges' normals.
- `Placed` holds modules only; the "MODULES BEFORE THE FENCE" ordering comment is replaced
  by one line saying why it no longer applies.
- Census line keeps its one `UE_LOG`, reworded to posts, fabric bays and gates; a plot with
  no gate gets its own Warning. `GetGateGapCount` now counts plots with a gate.
- Null content (the tests' case): posts are the engine cube scaled to Ø6 / Ø9 x 245 uu;
  the fabric takes the sink's own default surface material (the grid checker) - the same
  visible-but-wrong fallback the road uses, no second path.
- Fabric quads bypass `FRoadMeshBuffers::AppendTriangleUp`: its sliver guard measures XY
  area, zero for every vertical triangle. Wound so the engine normal faces out (measured).
- Posts have no collision, like the modules. Clear gate opening is 611 uu (post centres 620).

Tests: `Airside.Present.PlotFence` - spawn both actors, place a plot, assert HISM instance
counts equal the layout's per-kind post counts and fabric triangles equal `2 x Spans`.
`PlotPresenterTest`'s fence-panel assertions are rewritten against the layout counts.

### Content (editor closed)

- One new `Tools/Python/build_fence_content.py`, NOT `import_models.py` (that table drives a
  glTF vehicle pipeline). Imports both post FBXs into `/Game/Environment/Fence/` and
  MEASURES them (245 uu tall, radius 3 / 4.5, base at 0 - all matched first run); imports
  `chainlink.png` / `chainlink_n.png`,
  sets `bDoScaleMipsForAlphaCoverage = true`, `AlphaCoverageThresholds = (0,0,0,0.33)`
  (`Texture.h:1371-1375`), reads both back; authors `M_ChainlinkFabric` - Masked, Two
  Sided, clip 0.33, BaseColor + OpacityMask from the albedo, Normal from `_n`.
- `UAirsideContent` gains `FenceLinePost`, `FenceHeavyPost`, `FenceFabricMaterial`;
  `UAirsideSettings::ResolveFenceKit()` is the one resolver, returning a struct.

## Out of scope

Top rail, per-edge collision wall, gate leaves, barbed arm, bracing, sloped ground (all per
the README), and moving entities out of `URoadNetwork`.

## Verification

Each PR: full `Build.bat` line, `Run-AirsideTests.ps1` run/failed/crashed line,
`Check-Architecture.ps1` clean. PR 2 also: PIE `shot x editor` of a placed depot showing
the gate aligned with the truck corridor, heavy posts on the vertices, fabric outside the
posts, no black distance-field box over the plot.
