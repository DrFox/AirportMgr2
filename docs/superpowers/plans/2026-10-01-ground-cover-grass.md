# Ground cover grass - implementation plan

> Executed natively in the authoring session (the user asked to "go through implementation"). Steps use `- [ ]`.

**Goal:** sparse stylised grass tufts near the camera, never on a built surface, cheap and invisible from the default zoom.

**Architecture:**
- **Solve/** places tufts per (cell, layer) deterministically.
- **Build/** answers "is this point on a built surface?" from triangles and outlines.
- **Present/** streams cells of instanced static meshes around whichever view rendered the last frame.
  - A world subsystem spawns one transient actor per game or PIE world.
  - That actor binds `OnNetworkChanged` and rebuilds the mask.
- Content is generated: Blender tufts, a Python-built material and import, and data-asset fields.

**Spec:** `docs/superpowers/specs/2026-10-01-ground-cover-grass-design.md`

## Global constraints

- Solve/ includes only `CoreMinimal.h` and Solve/. Build/ must not include Present/, Tool/ or `Content/AirsideSettings`.
- Log through `LogAirside`. No new log category.
- The network is found only through `URoadNetworkRegistry`, and changes are heard only through `OnNetworkChanged`. No polling.
- Tuft meshes are imported with Nanite **off**. The material uses a world-space normal of (0,0,1), `bTangentSpaceNormal = false`.
- Tuft components: no shadow, no distance-field lighting, no dynamic indirect lighting, no collision.
- Every density, distance and cell size lives in `UAirsideContent`, read only through `UAirsideSettings::ResolveGroundCover()`.
- No tufts on any built surface, grass runways included.

## Review focus

1. **A road drawn over live grass.** The tufts under it must go once the change lands; a Geometry drag clears within 0.25 s of the last frame. Test: `Airside.Present.GroundCover.NewTaxiwayClearsItsTufts`.
2. **The camera high above the field.** There must be zero cells and zero work. Test: `Airside.Solve.GroundCover.NoCellsFromHigh`.
3. **Quality Low** (`sg.FoliageQuality 0`). There must be zero instances. Test: `Airside.Present.GroundCover.MaxLayersZeroDrawsNothing`.
4. **A point on a bucket or triangle edge.** It must count as covered, so no tuft centre sits on a slab edge. Test: `Airside.Build.GroundCoverMask.EdgeIsCovered`.
5. **No tuft meshes in content.** The actor must stay idle, not crash or spam. Test: `Airside.Present.GroundCover.EmptyKitIsIdle`.

---

### Task 1: Solve/GroundCover - deterministic scatter and cell maths

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Solve/GroundCover.h`
- Create: `Plugins/Airside/Source/Airside/Private/Solve/GroundCover.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/GroundCoverTest.cpp`

**Produces:**
- `GroundCover::FTuft{Position, YawDegrees, Scale, Variant}` and `GroundCover::FLayerSpec{TuftsPerSquareMetre, ShowWithinUu}`.
- `CellOf(FVector2D, double)`, `DistanceToCell(FVector, FIntPoint, double)` and `CellsWithin(FVector, double R, double S, TArray<FIntPoint>&)`.
- `ScatterCell(FIntPoint, int32 Layer, double S, double Density, int32 NumVariants, TArray<FTuft>&)`.
- The random number generator is a SplitMix64 seeded from (cell, layer). It cannot use `FRandomStream`, which is outside the Solve include rule.

- [ ] Tests:
  - **SameCellSameTufts:** identical output twice.
  - **LayersDiffer:** layer 0 differs from layer 1.
  - **DensityHolds:** over 100 cells the count is within ±1% of density × area (the count is rounded per cell).
  - **TuftsStayInCell:** every position lies inside the cell square.
  - **NoCellsFromHigh:** a viewer 50 m up with a 40 m radius gives no cells.
  - **CellsWithinRadius:** each returned cell is within R, and the viewer's own cell is included.
- [ ] Build, run `-Filter Airside.Solve.GroundCover`: red then green.

### Task 2: Build/GroundCoverMask - "is this point on a built surface?"

**Files:**
- Create: `Public/Build/GroundCoverMask.h` and `Private/Build/GroundCoverMask.cpp`
- Test: append to `GroundCoverTest.cpp`

**Produces:** `FGroundCoverMask(double BucketSizeUu = 1600)` with `AddTriangle`, `AddPolygon`, `IsCovered`, `NumTriangles`, `NumPolygons` and `NumBuckets`.
- The point-in-triangle test is inclusive, by edge-function signs.
- Degenerate triangles are skipped.
- Polygons go through `RoadGeom::PointInPolygon`.

- [ ] Tests:
  - **TriangleCovers:** inside, outside, and a vertex.
  - **EdgeIsCovered:** the midpoint of an edge counts as covered.
  - **BucketBoundary:** a triangle spanning 4 buckets covers a point on the shared corner.
  - **PolygonMatchesTriangles:** a square outline and two triangles agree on a 50×50 sample grid.
  - **EmptyCoversNothing.**

### Task 3: Content - ResolveGroundCover

**Files:**
- Modify: `Public/Content/AirsideContent.h`. Add the `FGroundCoverLayerSetting` USTRUCT and the `GroundCoverTufts`, `GroundCoverLayers` and `GroundCoverCellMetres` fields. Layer defaults are {8/m², 40 m}, {6/m², 4 m} and {0/m², 2 m}; the cell is 32 m.
- Create: `Public/Content/GroundCoverKit.h`, holding `FGroundCoverKit{Tufts, Layers (uu), CellSizeUu, IsUsable()}`.
- Modify: `AirsideSettings.h/.cpp` to add `static FGroundCoverKit ResolveGroundCover()`.

- [ ] Test **ResolveGroundCoverConvertsMetres:** the kit's layers are in uu and their count matches the content's.

### Task 4: Surface triangles out of URoadSurfacePresenter

**Files:** `Public/Present/RoadSurfacePresenter.h/.cpp`. Add `ForEachSurfaceTriangle(TFunctionRef<void(const FVector2D&, const FVector2D&, const FVector2D&)>) const` over the Road and Apron layers' `ProcessMesh`.

- [ ] Test **SurfaceTrianglesCoverTheRunway**, using `FAirsideTestWorld`:
  - Place a runway with `PlaceNode` and `ConnectNodes`.
  - Its centreline midpoint is covered by some visited triangle.
  - A point 100 m off it is not.

### Task 5: Presenter, actor and subsystem

**Files (Present/):**
- `GroundCoverPresenter.h/.cpp`, a UObject:
  - The cell → component map and the component pool.
  - `SetKit`, `SetMask`, `SetMaxLayers`, `StreamAround`, `Clear`, the counters, and `ForEachInstanceLocation`.
  - A budget of 4 cell builds per `StreamAround`.
- `AirsideGroundCoverActor.h/.cpp`, Transient and NotPlaceable:
  - `BindTo`, `SetKit`, `StreamAround`, `Tick`.
  - `OnNetworkChanged` uses an exhaustive switch: Topology and Facts rebuild now, Geometry is debounced 0.25 s, Markings are ignored.
  - The viewer is `World->ViewLocationsRenderedLastFrame[0]`. The world resets it at the end of its tick and the renderer refills it (`LevelTick.cpp:2039`, `UnrealClient.cpp:1780`), so PIE, Simulate and the game all work.
  - Console variable `airside.GroundCover` (0 hides everything).
  - Quality comes from `sg.FoliageQuality`, as a layer count of `min(Quality, 3)`.
- `GroundCoverSubsystem.h/.cpp`, a UWorldSubsystem for Game and PIE worlds:
  - `OnWorldBeginPlay` spawns and binds the actor to `URoadNetworkRegistry::Find`.
  - `OnAirportChanged` rebinds after begin play.
  - `FindActor(World)`.

- [ ] Tests, using `FAirsideTestWorld` plus a spawned actor, a cube kit and `StreamAround`:
  - **TuftsBesideNotOnRunway:** some instances exist, and none sits on a runway triangle. The check is against the surface triangles directly, not the mask.
  - **NewTaxiwayClearsItsTufts:** add a taxiway with `ConnectNodes`. Instances under it go, and this needs the delegate binding.
  - **MaxLayersZeroDrawsNothing.**
  - **EmptyKitIsIdle.**
  - **SubsystemSpawnsOnBeginPlay:** call `OnWorldBeginPlay` and check `FindActor` returns an actor bound to the fixture's network.

### Task 6: Content pipeline

**Files:**
- Create: `C:\repos\AirportMgr2Models\accessories\grass\scripts\build_tufts.py`, a Blender script. It writes three tufts (1, 2 and 3 blades) as `export/SM_GrassTuft{1,2,3}.fbx`, with the greyscale vertex gradient.
- Create: `Tools/Python/build_grass_content.py`, a commandlet script. It:
  - builds `/Game/Environment/Grass/M_Grass`, with the measured tint (0.215, 0.38, 0.145) linear;
  - imports the tufts with Nanite off and the material assigned;
  - fills `UAirsideContent.GroundCoverTufts`;
  - has a `verify()` that fails on Nanite on, the material's `bTangentSpaceNormal` true, or fewer than 3 tufts.

- [ ] Run headless, read the `MARKER:` lines, and check `git status Content/`.

### Task 7: Look and cost (live)

- [ ] Launch the editor on the slot (port 8002). Simulate M_Test.
- [ ] Take shots at 1, 5, 20 and 50 m and edge-on at 60 cm, compared against `docs/images/2026-10-01-grass/sparseCmp3m.jpg`.
- [ ] Tune `GroundCoverLayers` on the content asset live, then bake the values into the C++ defaults.
- [ ] Cost: `GroundCover: frame` log lines over 2 s, with `airside.GroundCover` at 1 and at 0.

### Task 8: Ship

- [ ] Run `Run-AirsideTests.ps1` in full and `Check-Architecture.ps1`, then open a PR with the build and test lines and the screenshots.
- [ ] Delete `Content/Spike/` and the spike actors in the main checkout's M_Test (unsaved).
