# Ground cover: stylised grass near the camera

Date: 2026-10-01. Status: draft for review. Supersedes the "Slice C" line of
`2026-09-12-environment-art-direction-design.md` (sections 3, 10, 11.0) - see section 9 for why.

## 1. Goal

When the camera is down by the ground, the field should read as grass, not a flat green
plane, and the 10 cm lip where every built surface sits proud of the landscape
(`ARoadNetworkActor::SurfaceZ = 10.0`, `RoadNetworkActor.h:1446`) should be broken up by
blades rather than read as a plate laid on a table.

Success, judged by screenshot as in the spike (section 2):

- At 1-5 m the ground is visibly grass, densest nearest the camera.
- At a built edge, blades stand against the shoulder and hide the lip.
- At 50 m the grass cannot be told from bare landscape - no ring where it stops.
- Nothing grows on a built surface, ever, including one the player has just drawn.
- From the default zoom (80 m) upwards it costs nothing, because none of it exists.
- The frame cost at the lowest zoom is measured and inside the budget in section 7.

Out of scope: wind, grass shadows, painted grass on the landscape layers, grass in the editor
mode (`URoadBuildEdMode`), trimming grass around aircraft and vehicles. They only ever drive on
surfaces.

## 2. What the spike settled (2026-10-01, throwaway, `Content/Spike/Grass`)

The spike placed hand-made 4x4 m tiles of generated clumps beside the M_Test taxiway and judged them in
Simulate at 1, 5, 20 and 50 m. Measured facts this design rests on:

| Finding | Consequence here |
|---|---|
| Blades lit by their own normals render olive with near-black faces, and become a dirty patch at 50 m. Turning off shadows, distance-field AO and Lumen contribution changed nothing. | The material outputs a **world-space normal of (0,0,1)** (`bTangentSpaceNormal = false`). Blades then light exactly like the ground. |
| Custom split normals authored in Blender did not survive FBX import. | Normals are fixed in the material, never in the mesh. |
| A tint of `GRASS_MOWN` `#7D8E47` came out olive. The landscape does not render as that colour. | The blade tint is measured from a screenshot as a ratio against the rendered ground: linear (0.215, 0.38, 0.145). It is a palette row with its date, not a derived value. |
| With colour matched, a 20x40 m patch is near-invisible from 50 m. | A hard cut at about 35-45 m needs no fade and no ring treatment. |
| Import enabled **Nanite**. On this mesh, Nanite simplification deletes whole blades, even at 1 m. The asset thumbnail looked fine; only the level showed it. | Grass meshes are imported with `NaniteSettings.bEnabled = false`, and a verify step asserts it. |
| 0.2 m spacing reads well from 5 m. 0.12 m spacing reads lush at 1-5 m (user: "denser close to the camera"). | Three density layers (section 4.2). |
| The user preferred the **Nanite-thinned** 0.12 m tile ("somehow looks more natural"). Measured by side-by-side shots, that tile is the densest grass on screen at 1 m and thins to scattered single blades from about 2 m. A uniform 25% thinning looked busier at 2.5 m and barer at 1 m. | What was liked is a **steep distance falloff ending in sparse single blades**, not uniform density. The layers in 4.2 reproduce that deliberately; Nanite is not relied on to do it (section 7). |

## 3. Approach

**Chosen: our own camera-local scatterer, excluding built footprints taken from the model.**

Rejected:

- **`LandscapeGrassType`**, the engine's built-in landscape grass. It was the 2026-09-12 plan.
  It knows nothing about surfaces the player builds at runtime. Excluding them would need a
  render-target mask fed to `M_Ground`, plus forced regeneration of grass components on every
  edit. That is a runtime path with no world-free test, and it is exactly the "code right,
  runtime wrong" shape `CLAUDE.md` warns about.
- **Edge fringe only.** The user's ruling: grass at the edges with flat ground everywhere else
  looks wrong.
- **PCG.** Its runtime generation would work, but it is deliberately kept out of the editor's
  load (`AllToolsets` note in `CLAUDE.md`), and it would put the exclusion rule in a graph
  rather than in tested code.

## 4. Design

### 4.1 Units

| Unit | Where | Does | Depends on |
|---|---|---|---|
| `GroundCover::ScatterCell` | `Solve/GroundCover.h` | Deterministic jittered tuft points for one cell and one density layer, from a hash of (cell x, cell y, layer). Same input, same grass. Returning to a spot shows the same tufts. | `CoreMinimal.h` |
| `FGroundCoverMask` | `Build/GroundCoverMask.h` | "Is this 2D point on a built surface?" Built from world-space triangles plus outline polygons, bucketed into a uniform 2D grid of triangle and polygon indices. The test is exact point-in-triangle; there is no rasterised bitmap, so grass meets an edge exactly. | `Solve/` (`RoadGeom::PointInPolygon`) |
| `UGroundCoverPresenter` | `Present/` | Owns the live cells and their ISM components. Each tick it adds and removes cells around the camera, and rebuilds the mask when the network changes. | the mask, `ScatterCell`, resolved content |
| `AAirsideGroundCoverActor` | `Present/` | A composition root, modelled on `AAirsideBuildingsActor`: `FindOrCreate`, binds through `URoadNetworkRegistry::OnAirportChanged`, and forwards to the presenter. | registry, presenter |
| `UAirsideSettings::ResolveGroundCover()` | `Content/` | The one place the tuft meshes, the material, the layer spacings and cull distances, and the cell size are read. | `UAirsideContent` |

`ARoadNetworkActor` does not grow. The grass gets its own actor, just as buildings did.

### 4.2 Density layers

The instance unit is a **tuft of 1-3 blades**, not the spike's 5-8 blade clump. Scattered
single blades are what made the liked version look natural.

**Low density is the look** (user, 2026-10-01: they prefer "the nanite very low density amount
more than higher density"). Layer 0 carries it and matches the left half of
`sparseCmp3m.jpg`: scattered single blades with ground showing between them.

The nearer layers are small top-ups so the ground right under the camera is not bare. They are
not a lush carpet. Each layer is a separate set of points per cell, so a nearer layer only ever
ADDS tufts; nothing pops out as the camera approaches.

| Layer | Tufts/m² | Shown within | Reads as |
|---|---|---|---|
| 0 | ~8 | 40 m | scattered singles: the look |
| 1 | ~6 | 4 m | a little fuller at the feet |
| 2 | 0 by default | 2 m | spare; kept so a top-up exists if the foreground reads bare |

All densities and distances are starting guesses, tuned live against the Nanite reference
shot (section 8), erring sparse. Each lives in `ResolveGroundCover()` only.

Each layer of each cell is one `UInstancedStaticMeshComponent`, with that layer's cull distance
set as the component's end cull distance. The GPU culls per instance, so the camera moving
within a cell costs nothing on the CPU.

### 4.3 Cells

- **Cell size:** a 16 m world-aligned grid.
- **Live set:** cells whose nearest point is within the layer-0 distance (40 m) of the camera
  location in 3D. A camera 40 m or more above the ground therefore has **no live cells**. The
  "only when low" behaviour falls out of the geometry; there is no separate switch to keep in
  agreement with the camera rig. For scale: the rig's default `StartDistance` is 80 m, so the
  default view has no grass.
- **Pool:** cells leaving the live set have their ISMs cleared and returned to a pool, so there is
  no component churn. Per tick, the work is bounded by a cap on cells built per frame (4), so a
  fast pan streams in over a few frames rather than hitching.
- **Camera source:** the local player's `PlayerCameraManager` location. The plugin cannot see
  the game module's camera rig, and lint rules 54/55 close the controller's surface. No camera,
  no cells.

### 4.4 Exclusion

The mask is built from:

1. **The surface triangle soup the presenter already builds**: roads, taxiways, runways,
   junctions, aprons and stand pads. These are `FRoadMeshBuffers` world-space positions and
   indices from `FRoadMeshBuilder` (`RoadMeshBuilder.h:87`, `:167`, `:188`).
   `URoadSurfacePresenter` exposes its last committed buffers read-only; it does not run the
   derivation a second time. One builder, one footprint, no second evaluator to drift.
2. **Plotted entity outlines**: plots, depots and drawn stands
   (`FEntityInstance::Outline` with `IsPlotted()`, `RoadEntity.h:324-335`).

**Tuft centres are tested, not blade tips.** A tuft on the boundary leans up to about 13 cm
over the shoulder. That overhang is what hides the lip, and it is wanted.

**Grass runways** (`EPavement::Grass`): excluded, like every other surface. Ruled by the user
2026-10-01.

**Rebuild trigger.** `ARoadNetworkActor::OnNetworkChanged`, bound the same way as
`AAirsideBuildingsActor` (`AirsideBuildingsActor.cpp:255-311`), with an exhaustive switch over
`EChangeKind`:

- **Topology / Facts:** rebuild the mask, then re-filter every live cell.
- **Geometry**, which fires every drag frame: mark the mask dirty, and rebuild once no Geometry
  change has arrived for 0.25 s. Grass under a node being dragged lags a quarter-second; it
  never pokes through a surface after the drag ends.
- **Markings:** ignore. They do not change footprints.

Lint rule 51 holds: the change is announced, never polled.

### 4.5 Content

- **Tufts:** 6 variants of 1-3 pyramid blades each (3 triangles per blade, opaque), 14-24 cm tall.
  Vertex colour is greyscale from root 0.55 to tip 1.0, with per-tuft jitter. Generated by a
  checked-in Blender script in the models repo, the same convention as the existing
  `build_*.py` exports. There is no LOD: at 3-9 triangles a tuft has nothing to simplify.
- **Material `M_Grass`:** `VertexColor × Tint × (1 ± 0.08 × PerInstanceRandom)` into base colour,
  plus world-space normal (0,0,1), roughness 0.9 and specular 0.2, matching the landscape after
  #486. Opaque and one-sided. No world position offset (no wind).
- **ISM component flags:** `CastShadow = false`, `bAffectDistanceFieldLighting = false`,
  `bAffectDynamicIndirectLighting = false`, no collision. The spike showed shadows add nothing
  the normal fix does not already give. With virtual shadow maps on, shadowed grass is the
  expensive case to avoid.
- **Authored by `Tools/Python/build_grass_content.py`**, headless:
  - It imports the tufts with Nanite **off**, builds `M_Grass`, and fills `UAirsideContent`.
  - Its `verify()` names any tuft with Nanite on, or a material with
    `bTangentSpaceNormal` true.
  - Each of those failed silently in the spike, which is why the verify step exists.

### 4.6 Quality

Driven by the existing Graphics dropdown (`FPlayerSettings::GraphicsQuality`,
`SettingsPanelWidget.cpp:61-89`) through the engine's `sg.FoliageQuality` scalability group, so
there is no new setting:

| Quality | Layers |
|---|---|
| Low | none (no actor work at all) |
| Medium | 0 |
| High | 0, 1 |
| Epic | 0, 1, 2 |

### 4.7 Logging (`LogAirside`)

- `GroundCover: mask rebuilt - N triangles, M outlines, K buckets in X ms (kind=Topology)`
- `GroundCover: live cells N, instances L0/L1/L2 = a/b/c` - logged on change, not per tick.
- `GroundCover: no camera - idle` - logged once, when the presenter finds no camera to stream from.

## 5. Determinism and the weld contract

The grass reads the surface triangle soup and never writes to it. The bitwise weld contract and the guideline graph are
untouched. The scatter is a pure function of cell and layer, so a test can assert exact point
sets.

## 6. Tests

World-free, in `Solve/` and `Build/`:

- `ScatterCell` is deterministic: same cell and layer give identical points; different layers
  give disjoint points.
- `ScatterCell` density lands within ±10% of the nominal tufts/m² over 100 cells.
- `FGroundCoverMask`:
  - excludes points inside a triangle and keeps points outside it;
  - gives the same answer for a point on a bucket boundary from both buckets;
  - treats an outline polygon the same as triangles covering the same area.

Composition, at actor level. Spawn the actor in a test world with a network holding one runway,
and drive the camera through the presenter's camera input:

- Instances exist beside the runway, and **none has a centre inside the runway footprint**.
  This is measured by testing every instance against the runway's own triangles, not against
  the mask (the a-green-test-may-measure-nothing lesson).
- Adding a taxiway through the edit facade removes the instances under it after the change.
  If the `OnNetworkChanged` binding is not wired, this goes red.
- A camera 50 m up yields zero live cells.
- Quality Low yields zero components.

## 7. Performance budget and measurement

- **Estimate at the lowest zoom (the rig's 6 m `MinDistance`):**
  - Layer 0: a 40 m radius is about 5,000 m², at ~8 tufts/m² about 40k instances.
  - Layer 1 adds about 300; layer 2 adds nothing by default.
  - Frustum culling leaves roughly a third, at ~6 triangles per tuft. That is about 80k
    triangles: opaque, unshadowed, with a trivial material.
  - The low density the user chose is about 10x cheaper than the uniform layers it replaced.
- **Budget:** grass adds at most **1.5 ms GPU** at 1080p Epic on the dev machine, at the lowest
  zoom over the M_Test field.
- **Measured, not asserted:** a temporary `airside.GroundCover 0|1` console variable, with the
  game module logging the averaged `RHIGetGPUFrameCycles` over 2 s on each toggle. The user
  toggles it in PIE, and the log line is the evidence.
- **If over budget, in order:**
  1. Shorten the layer 0 distance: 40 m to 30 m.
  2. Lower layer 0's density.
  3. Cut layer 1's distance.
- **Nanite stays off**, even though it produced the look the user liked. Its thinning is driven by
  screen-space error, so it changes with resolution, field of view and scalability, and nothing
  in the code controls it. The layers make the same falloff a number we own and can test.

## 8. Visual acceptance

Iterated live, per the visual-changes rule, not through the implementation plan:

- Shots at 1, 5, 20 and 50 m beside a runway edge, plus edge-on at 60 cm, with and without grass.
- **Reference:** the spike's Nanite-thinned tile, `sparseCmp3m.jpg` and `sparseCmpClose.jpg`
  (left half). They are in `docs/images/2026-10-01-grass/`, together with the edge and 50 m shots.
- Tint, spacings and distances are tuned on the running editor, then baked into
  `ResolveGroundCover()` defaults and the build script.

## 9. Why this replaces the 2026-09-12 Slice C line

That spec named `LandscapeGrassType` before the game let the player build surfaces at runtime
over grass. It also asked (in 11.0) for a technique-research step before Slice C, and flagged
two unverified risks: clump normals, and the colour ring at the cull distance. The spike was
that step. It confirmed both risks, fixed both, and found a third (Nanite). The old spec gets
a one-line pointer to this one; its other slices are unchanged.

## 10. Rulings and follow-ups

Ruled by the user on 2026-10-01:

- **Grass runways:** no tufts.
- **Quality:** the existing Graphics dropdown, as in 4.6. There is no separate setting.
- **Density:** very low, as in the Nanite reference, rather than the denser versions.

Follow-up, not in this spec:

- **Rough grass on the `GrassRough` landscape layer.** Wanted, but only if it costs nothing
  measurable, and there is no fence border to put it past yet.
- **The likely shape:** taller, sparser tufts that REPLACE layer 0 where the rough layer is
  painted, so the instance count does not rise.
- **The open technical question:** the scatterer must read the paint-layer weight at runtime
  without a GPU readback. Landscape weightmaps are GPU textures in a cooked build. Settle that
  first, with a measurement, before the slice is planned.
