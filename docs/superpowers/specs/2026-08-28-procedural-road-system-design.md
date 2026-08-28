# Procedural Runtime Road System — Design

**Date:** 2026-08-28
**Project:** AirportMgr (Unreal Engine 5.8.2)
**Status:** Approved design, pending implementation plan
**Scope:** Slices 1–4 (foundations, solver, mesh, build tool, lanes). Slices 5–6 get their own specs.

---

## 1. Context

AirportMgr needs a Cities: Skylines-style road and taxiway system: the player draws
surfaces at runtime and they are procedurally generated, with correct junction geometry.

A previous Blueprint implementation reached a working state but produced poor visuals.
The reference screenshot (`samples/oldsystem.png`) shows the specific failures:

- **Mitred corners.** Where two segments met, the corner was a hard mitre. No fillet.
- **Seams and z-fighting.** Visible cracks between segment meshes and junction caps.
- **No junction at the runway.** The taxiway butted into the runway edge with no fillet.
- **Width discontinuity** across nodes.
- **No surface markings** on procedural geometry.
- **Knife edge against terrain**, and z-fighting on overlaid stand quads.

The static-mesh runway visible in the same screenshot is the quality bar.

### Root cause of the old failures

The junction cap and the segment meshes were generated **independently** and butted
together by position:

- Mitred corners, because the cap was built from the node's raw incoming directions
  rather than from an offset/fillet solve on the road *edges*.
- Cracks and z-fighting, because both meshes computed their shared boundary
  separately, in floating point, and got slightly different answers.

The fix is architectural: **one solver owns the node's boundary and hands the same
vertex values to both the cap and the trimmed segment ends.** Segments never compute
where they stop.

---

## 2. Requirements

Decided during design:

| # | Requirement | Decision |
|---|---|---|
| R1 | Surface types | Roads, taxiways/runways, and eventually aprons/stands. Aprons are a separate slice. |
| R2 | Terrain | Flat world, no terrain interaction. The solve is a plan-view 2D problem. |
| R3 | Navigation | Lane-level modelling from the start — aircraft taxiing needs per-lane turn paths through junctions. |
| R4 | Drawing interaction | CS-style: straight mode and curved mode (quadratic Bezier via a control point), with continuous chaining. |
| R5 | Scale | Airport-sized: a few hundred segments, all loaded, all visible. No streaming. Instant drag feedback. |
| R6 | Junction geometry | Analytic offset + tangent-arc fillet solve. |
| R7 | Ghost preview | CS-style translucent "blueprint" shader while dragging, with validity colouring. |
| R8 | Curve representation | Quadratic Bezier (one control point). S-curves are achieved by chaining segments, not within one. |
| R9 | Segment subdivision | A long drag auto-subdivides into multiple segments at a maximum length, giving finer granularity for AI pathing and per-segment logic. One drag remains one undo step (`FComposite`). |
| R10 | Runway crossings | A taxiway crossing a runway creates a **real junction node** with shared geometry, not an unconnected overlapping surface. Hold-short logic then has a topological place to live. |
| R11 | Fillet radius | Authored per profile as a plain number. An ICAO design-group table is a later refinement, not a Slice 1–4 concern. |

### Non-goals

- Terrain conformance, deformation, ramps, bridges, tunnels.
- Streaming or LODs.
- Traffic simulation itself (this system supplies the network; agents are separate).
- Aprons/stands and discrete decal markings (slices 5–6).
- Multiplayer or network replication.

---

## 3. Architecture

### 3.1 Module layout

`RoadNet` is a **plugin**, not a project module. A plugin is Unreal's real
package/library unit: its own content folder, a linker-enforced dependency boundary,
a separate test module that never ships, and portability to other projects.

```
Plugins/RoadNet/
  RoadNet.uplugin
  Source/RoadNet/          (Runtime)
    Model/     URoadNetwork, FRoadNode, FRoadSegment, FRoadNodeId
    Profiles/  URoadProfile (UDataAsset)
    Solve/     FJunctionSolver, FSegmentSolver, FCorner2D
    Build/     FRoadMeshBuilder, IRoadMeshSink
    Present/   ARoadNetworkActor, URoadRenderer
    Tools/     URoadToolSubsystem, IRoadToolState, IRoadCommand
  Source/RoadNetTests/     (Developer — excluded from shipping builds)
  Content/                 road materials, profile data assets
```

Top-level project folders outside Unreal's reserved set:

```
docs/    design notes and specs
Art/     .blend / .psd source files — never inside Content/
samples/ reference screenshots
```

Unreal's reserved root folders (`Source/`, `Content/`, `Config/`, `Plugins/`,
`Binaries/`, `Intermediate/`, `Saved/`) are located by hardcoded paths relative to
the `.uproject` and cannot be relocated into a `src/` directory.

### 3.2 Dependency direction

Strictly downward:

```
Tools   -> Model
Present -> Model + Solve + Build
Solve   -> core maths only        (no engine dependencies)
```

`Solve` having no engine dependencies is a hard constraint, not a preference. It is
what makes the junction solver unit-testable without spawning a World, and therefore
what makes the geometry debuggable.

### 3.3 Patterns

| Pattern | Applied to |
|---|---|
| Repository + entity handles | `URoadNetwork` and the graph |
| Flyweight | `URoadProfile` shared cross-sections |
| Strategy | corner treatment, `IRoadMeshSink` |
| Builder | `FRoadMeshBuilder` |
| Observer | model invalidation to renderer |
| State | tool modes |
| Command | graph mutation and undo/redo |
| Chain of Responsibility | snap resolution |
| Composite | placement validation rules |
| Service Locator (`UWorldSubsystem`) | tool ownership |

### 3.4 Deliberate departures from textbook OO

1. **Handles, not references.** `FRoadNodeId` is a typed `int32` plus generation
   counter, not `URoadNode*`. A raw `UObject*` not marked `UPROPERTY()` is invisible to
   Unreal's garbage collector and will be collected while still in use. Handles avoid
   GC entirely, serialise trivially, survive graph mutation without dangling, and keep
   node data contiguous.
2. **`Solve/` is plain structs and free functions, not `UObject`s.** Reflection buys
   nothing for pure maths and costs allocation, GC pressure, and the ability to run
   off the game thread.
3. **No virtual dispatch in per-vertex loops.** Polymorphism is resolved once per
   junction, at the profile and corner-treatment level; inner loops are flat data.
4. **Tool states and commands are `TUniquePtr` plain structs**, for the same reasons
   as (2).

---

## 4. Domain model

### 4.1 Handles — a slot map

```cpp
USTRUCT()
struct FRoadNodeId
{
    GENERATED_BODY()
    UPROPERTY() int32 Index      = INDEX_NONE;
    UPROPERTY() int32 Generation = 0;
    bool IsValid() const { return Index != INDEX_NONE; }
};
```

Deleting a node bumps its slot's generation, so a stale handle fails a cheap validity
check instead of silently reading a recycled node.

### 4.2 Entities

```cpp
USTRUCT()
struct FRoadNode
{
    UPROPERTY() FVector2D Position;
    UPROPERTY() TArray<FRoadSegmentId> Incident;   // maintained sorted by bearing
};

USTRUCT()
struct FRoadSegment
{
    UPROPERTY() FRoadNodeId A, B;
    UPROPERTY() FVector2D  Control;                // quadratic Bezier control point
    UPROPERTY() TObjectPtr<URoadProfile> Profile;
    UPROPERTY() float TrimA = 0.f, TrimB = 0.f;    // written ONLY by the junction solver
};
```

`FVector2D` because the world is flat (R2); this makes 3D bugs impossible to
introduce. Elevation, if ever needed, becomes a displacement pass over generated
vertices rather than a model change.

`TrimA`/`TrimB` live on the segment but are written **only** by the solver. This makes
the core invariant structural rather than conventional.

### 4.3 Profiles — the Flyweight

```cpp
UCLASS()
class URoadProfile : public UDataAsset
{
    UPROPERTY(EditAnywhere) TArray<FProfileBand> Bands;   // left to right: shoulder, lane, lane, shoulder
    UPROPERTY(EditAnywhere) TArray<FProfileLane> Lanes;   // offset, width, direction
    UPROPERTY(EditAnywhere) float PreferredFilletRadius;
    UPROPERTY(EditAnywhere) TObjectPtr<UMarkingSet> Markings;
};
```

A taxiway is one bidirectional lane, yellow centreline, ~15 m fillet. A runway is a
wide fixed profile with threshold markings and no fillets. A service road is two lanes
with curbs. **No new code per surface type** — this is why profiles are data rather
than a subclass hierarchy.

Where profiles differ across a node, the junction uses `min(radiusA, radiusB)`,
further clamped by what geometrically fits.

### 4.4 Mutation and invalidation

All graph edits go through `URoadNetwork` (Repository), which maintains the
sorted-by-bearing incident lists and raises change events (Observer).

> **Dirty rule:** moving or re-profiling a segment dirties **both its nodes**, and
> dirtying a node dirties **every segment incident to that node**, because the solve
> changes their trim points.

Missing this transitive step produces cracks that appear only sometimes, only near
edits. It belongs in the model, not the renderer.

---

## 5. Junction solver

Pure functions over plain structs. No engine types beyond `FVector2D`.

**Input per node:** position `P`, and for each incident segment: outgoing tangent `T`
at the node, left/right half-widths, preferred fillet radius.

### Step 1 — Bearings

Sort incident segments by `atan2(T.Y, T.X)`. Adjacent pairs in this cyclic order
define the corners. `n` segments produce `n` corners.

### Step 2 — Facing edge rays

For the corner between segment *i* and segment *i+1*: take the left edge of *i* and
the right edge of *i+1*, each offset from its centreline by its half-width.

For curved segments this uses the tangent at the node — exact enough while the trim
distance stays well below the curvature radius, with iterative refinement available
if it ever isn't.

### Step 3 — The tangent arc

Two rays diverging from their intersection `X`, with interior angle `theta`:

```
                  / ray u2
                 /
           T2 o
            / /
     C o   /            C = arc centre
        \ /             R = fillet radius
  ------o------o------- ray u1
        X      T1

  d = |X->T1| = |X->T2| = R / tan(theta/2)
  |X->C|                = R / sin(theta/2)
```

### Step 4 — Clamping

| Case | Symptom | Handling |
|---|---|---|
| theta approaches pi (nearly straight through) | `d` approaches 0 | no fillet, straight join |
| theta approaches 0 (acute, near-parallel) | `d` diverges | clamp `d` to a fraction of segment length |
| short segment between two junctions | both ends' fillets overlap | reduce `R` until `d` fits, solving back for `R` |

### Step 5 — The segment/junction contract

> **Every segment ends in a straight cut perpendicular to its centreline.** The solver
> computes that cut distance — `max` over the segment's two adjacent corners — and the
> two vertices at its ends. The junction polygon is assembled **from those exact same
> vertex values**. Neither side recomputes the other's geometry.

Because the cut is at `max(d)` of the two adjacent corners, both tangent points lie at
or beyond the cut, so each corner's boundary is `straight -> arc -> straight` with
non-negative lengths throughout. No special cases, no epsilon tolerance, and the
shared vertices are bitwise identical rather than merely close.

This is the single most important decision in the design. It makes the seams from the
old system structurally impossible rather than tuned away.

### Step 6 — Polygon assembly

Walk the segments in bearing order, emitting each cut's two vertices with each
corner's arc sampled between them. Result: one closed CCW polygon.

### Step 7 — Triangulation

The polygon is star-shaped about `P` in all realistic configurations, so a triangle
fan from `P` is correct, cheap, and yields clean topology plus a natural centre vertex
for UV work. Star-shapedness is validated; a pathological node falls back to
ear-clipping.

### Step 8 — Turn paths

The solve already knows every lane's entry and exit point and tangent at the cuts. For
each ordered in-lane to out-lane pair, emit a cubic Bezier with control points along
those tangents. This is the aircraft taxi routing (R3), produced by the geometry
solver rather than bolted on afterwards.

### Degenerate node counts

All handled by the same code path: **1 segment** gives a dead-end cap; **2 segments**
is the corner case (the mid-image failure in the reference screenshot), getting a real
fillet outside and a tight one inside; **3 or more** is the normal case; **differing
widths** falls out naturally, since each segment contributes its own half-widths.

---

## 6. Mesh generation and materials

### 6.1 Two sinks, one pipeline

```
FRoadMeshBuilder --emits--> IRoadMeshSink
                              |-- FCommittedSink  batched UDynamicMeshComponents, rebuilt on commit
                              |-- FPreviewSink    single component + ghost MID, rebuilt per mouse-move
```

Both sinks are fed by the same builder from the same solver output, which gives a
correctness property: **the ghost is WYSIWYG by construction.** It cannot disagree
with the committed result because it is not a separate approximation.

Dragging rebuilds only the preview sink; committing rebuilds the affected batch.
Hundreds of segments of flat 2D geometry is a few tens of thousands of vertices —
sub-millisecond, so no incremental-update machinery is warranted.

### 6.2 Backend

`UDynamicMeshComponent`, batched by material rather than one component per segment
(hundreds of components would mean hundreds of draw calls). `IRoadMeshSink` keeps this
swappable.

**No collision meshes.** Cursor position comes from an exact ray/plane intersection
(the world is flat), and snapping queries a spatial hash over the graph. Generating
collision purely to support mouse picking would be waste.

### 6.3 UV channels

- **UV0 — world-aligned XY.** The asphalt texture is projected from world space, so it
  is automatically continuous across the segment/junction boundary. No stretching at
  fillets, no seam, nothing to reconcile. Junction polygons and segment quads texture
  identically without either knowing about the other.
- **UV1 — profile space** (lateral offset across the profile, distance along the
  centreline). Drives markings, curb wear, and edge fade.

### 6.4 Markings without z-fighting

- **Continuous markings** (centreline, edge lines, lane dividers) are drawn *in the
  road material* from UV1 via a marking mask sampled against lateral offset. No extra
  geometry, so z-fighting is impossible by construction.
- **Discrete symbols** (hold-short bars, stand numbers, runway designators) are
  `UDecalComponent`s — slice 6.

Junction markings then become a data question, not a geometry one: the profile's
marking set states whether centrelines continue through, stop short, or follow the
turn paths the solver already produced.

### 6.5 Ground blend

The profile's outermost band is a shoulder whose vertex alpha falls to zero at its
outer edge, so the surface fades into the ground rather than ending in a knife edge.
(Runtime Virtual Texturing would be the answer if a real landscape is ever added.)

### 6.6 Ghost material (R7)

One translucent unlit material, applied as a `UMaterialInstanceDynamic`:

| Parameter | Purpose |
|---|---|
| `BaseColor` | CS-style cyan for valid placement |
| `ValidityBlend` | lerps to red where placement is illegal |
| `ScanSpeed` | animated stripe along UV1's length axis; follows the road's curve rather than sliding across it |
| `EdgeGlow` | Fresnel rim, readable against both grass and asphalt |

Because validity is a material parameter rather than a mesh variant, one junction can
glow red while the rest of the drag stays cyan, with no regeneration.

---

## 7. Build tool

### 7.1 Ownership

`URoadToolSubsystem : UWorldSubsystem` owns the active tool and the command stack.
Subsystems are Unreal's sanctioned service locator — engine-managed lifetime tied to
the World, no singleton boilerplate. Unreal has no DI container; this is the idiomatic
substitute.

### 7.2 State pattern

```cpp
struct IRoadToolState
{
    virtual ~IRoadToolState() = default;
    virtual TUniquePtr<IRoadToolState> OnClick (const FToolContext&) = 0;
    virtual TUniquePtr<IRoadToolState> OnCancel(const FToolContext&) = 0;
    virtual void BuildPreview(const FToolContext&, FPreviewSink&) const = 0;
};
```

States: `FIdle` to `FStartPlaced` to (curved mode only) `FControlPlaced` to commit,
then back to `FStartPlaced` with the new end node as the new start (CS continuous
chaining). Escape or right-click unwinds one state.

`BuildPreview` being `const` means a state physically cannot mutate the network —
preview and mutation are separated by the type system rather than by discipline.

### 7.3 Command pattern

```cpp
struct IRoadCommand
{
    virtual void Apply (URoadNetwork&) = 0;
    virtual void Revert(URoadNetwork&) = 0;
};
```

`FCreateSegment`, `FDeleteSegment`, `FSplitSegment`, `FMoveNode`, and `FComposite` so
a drag creating four segments is one undo step. The mutating API of `URoadNetwork` is
reachable only from commands.

> **`Revert` must restore handles identically, generation counter included.** A delete
> command stores the full node/segment payload and re-inserts into the *same slot*.
> Otherwise undo silently invalidates every outstanding handle.

### 7.4 Snapping — Chain of Responsibility

First hit wins, in priority order:

| Priority | Rule | Result |
|---|---|---|
| 1 | Existing node within radius | connect to it |
| 2 | Existing segment within radius | split it, create a node |
| 3 | Collinear extension of an incident segment | continue straight |
| 4 | Angle snap (15/5 degree increments off existing segments) | tidy geometry |
| 5 | Grid snap | aligned layout |
| 6 | Free | raw cursor position |

Backed by a uniform grid spatial hash. Rule 4 is most of why CS layouts read as
deliberate rather than hand-wobbled. Future rules ("snap to runway centreline",
"snap parallel at taxiway separation minima") are new links, not modifications.

### 7.5 Validation

`FPlacementValidator` is a Composite of small rules: minimum segment length, minimum
turn angle, fillet radius fits, no overlap with an existing surface, no unauthorised
runway crossing.

Ordering, every mouse move:

```
snap -> solve -> validate -> build preview
```

Validation runs after the solve because it needs to know whether a fillet had to
clamp. Its `FValidationResult` sets `ValidityBlend` and gates commit.

---

## 8. Testing

A Developer-type module `RoadNetTests` using Unreal's Automation framework. Because
`Solve/` has no engine dependencies, its tests need no World, no PIE, no editor.

```
UnrealEditor-Cmd.exe AirportMgr.uproject -ExecCmds="Automation RunTests RoadNet" `
  -unattended -nopause -nosplash -testexit="Automation Test Queue Empty" -log
```

| Tier | Coverage |
|---|---|
| 1. Solver property tests | polygon is simple; winding CCW; **junction vertices bitwise-equal to segment cut vertices**; output continuous as bearing sweeps 0 to 2pi; clamping never yields negative-length cuts; node counts 1/2/3/4/6 all valid |
| 2. Golden tests | known configurations pinned to expected vertex positions within tolerance |
| 3. Command round-trips | `Apply` then `Revert` restores state exactly including generations; composite undo atomic |
| 4. Snap resolver | pure, table-driven, exhaustive |
| 5. Visual regression | **manual by design** — a junction gallery map |

**Junction gallery:** 2-way at 15, 45, 90 and 170 degrees; 3-way T and Y; 4-way;
5-way; and mixed-width nodes, all on screen at once.

**`road.DebugDraw` cvar** renders solver internals — bearings, edge rays, the
intersection point `X`, tangent points, arc centres, cut lines. Geometry code is
miserable to debug without it and tractable with it.

---

## 9. Slice plan

| Slice | Contents | Exit criterion |
|---|---|---|
| **1. Foundations + solver** | plugin scaffold, model/handles, profiles, `FJunctionSolver`, tests, debug draw, code-built test network | junction gallery renders with clean fillets and **zero seams** |
| **2. Mesh + materials** | mesh builder, sinks, batched components, dual UVs, asphalt, shoulder fade, UV1 markings | the gallery matches the reference runway's quality |
| **3. Build tool** | subsystem, state machine, commands + undo, snap chain, validation, ghost material | the reference screenshot's layout is buildable interactively, and looks better |
| **4. Lanes + turn paths** | lane data, turn path generation, debug viz | taxi routes visualised through every junction |
| *5. Aprons and stands* | polygon surfaces | *separate spec* |
| *6. Decals, hold-shorts, signage* | discrete markings | *separate spec* |

Slice 1's exit criterion is deliberately the failure mode that beat the previous
attempt: seams and corners are proven before any tool UX exists.

---

## 10. Environment

The project was upgraded from UE 5.4 to **5.8.2** during design (commit `2f89b91`):
`EngineAssociation` 5.4 to 5.8, `BuildSettingsVersion` V5 to V7,
`EngineIncludeOrderVersion` Unreal5_4 to Unreal5_8 (the 5.4 include order is
unsupported in 5.8). `BuildSettingsVersion.V7` promotes return-type,
dangling-reference and unreachable-code warnings to errors.

**Blocking prerequisite for Slice 1:** no installed MSVC toolchain is currently
accepted by UE 5.8. Per `Engine/Config/Windows/Windows_SDK.json`:

- `14.50.35717` (VS 2026 Professional 18.1) — **banned**, internal compiler errors;
  resolved in `14.50.35723`.
- `14.39.33519` — **banned**, AVX codegen crash.
- `14.36.32532` — below the `14.38.33130` minimum.

Resolution: update Visual Studio 2026 to a build shipping MSVC `14.50.35723` or newer.

---

## 11. Resolved decisions

All four questions raised during design review are settled; see R8–R11 in §2.

| Question | Decision | Consequence |
|---|---|---|
| Fillet radius defaults | Per-profile plain number (R11) | ICAO design-group tables deferred; `PreferredFilletRadius` stays a single float on `URoadProfile` |
| Curve representation | Quadratic Bezier (R8) | One `Control` point per segment, as modelled in §4.2. S-curves come from chaining |
| Runway crossings | Real junction node (R10) | The graph must tolerate very large width disparities at a node, and the fillet clamp of §5.4 must handle a narrow taxiway meeting a wide runway. Affects Slice 4, not Slice 1 |
| Segment length limits | Auto-subdivide at a maximum length (R9) | The build tool emits *n* segments per drag wrapped in one `FComposite` command; `URoadNetwork` must handle collinear interior nodes efficiently, and §5's theta-approaches-pi case (no fillet, straight join) becomes the common path rather than an edge case |

### Deferred to later slices

- ICAO design-group fillet tables.
- Hold-short markings and logic at runway crossings (slice 6).
- Aprons and stands as polygon surfaces (slice 5).
