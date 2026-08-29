# Road System Slice 2b — Materials and Markings — Design

**Status:** approved in design, not yet planned.

**Parent spec:** `2026-08-28-procedural-road-system-design.md` §6.3–6.6 states what 2b delivers. This document records the decisions that spec leaves open, and it is authoritative where the two differ on detail.

**Slice contents** (parent §9): dual UV channels, asphalt material, shoulder fade, UV1 centreline markings. Exit criterion: *the gallery matches the reference runway's quality.*

---

## 1. What 2a left behind

Slice 2a renders solid, seamless surfaces with no material at all. It leaves four things for this slice:

- A placeholder `SetConstantOverrideColor` on the mesh component, and `GetDefaultMaterial(MD_Surface)` as the material — which is `WorldGridMaterial`, the same checker as the default floor. Both go.
- `FProfileBand` carries `Type` (Shoulder/Lane/Curb) and `MaterialSlot` and has been unused since Slice 1. This slice is the first consumer.
- **K3** (parent §12): `AddTriangle` drops only *index*-degenerate triangles, so near-collinear slivers of ~1.6e-10 uu² survive. 2a could not calibrate an area threshold; with a texel scale, 2b can.
- Curved segments render as a chord, because `AddSegment` lerps interior samples in a straight line. Out of scope here — see §8.

## 2. Decisions

| Question | Decision |
|---|---|
| Ribbon geometry | **Subdivide laterally per profile band.** Enables per-band material slots. (Originally also justified by a vertex-alpha shoulder fade; see §6.) |
| Material authoring | **Authored by commandlet.** `PythonScriptPlugin` is enabled in the `.uproject`; `MaterialEditingLibrary`, `MaterialFactoryNew`, `AssetImportTask` and asset tools were all verified available headlessly via `UnrealEditor-Cmd -run=pythonscript`. No GUI editor work required. |
| Textures | **`concrete-bl/pebbled-asphalt1-bl`** from `c:\repos\models\materials` — *dark charcoal asphalt with pale pebble aggregate, roadways and yard surfacing*. Imported into `Content/RoadNet/Textures` and committed; the repo is private and the user has confirmed this is acceptable. |
| Junction markings | **Fade out across the junction**, via a vertex-colour channel. Parent §6.4's "stop short" and "follow the turn paths" remain later data choices; turn paths are Slice 4. |
| Shoulder fade at junctions | ~~**Inset ring, clamped to a fraction of the junction's inradius.**~~ **SUPERSEDED — see §6.** The ring survives as band-boundary geometry; the fade does not. |

## 3. Buffers

`FRoadMeshBuffers` gains three arrays, each parallel to `Positions`:

```cpp
TArray<FVector2f> UV0;     // world-aligned XY, divided by TexelsPerUnit
TArray<FVector2f> UV1;     // X = lateral offset in uu, Y = distance along the centreline in uu
TArray<FColor>    Colors;  // A = ground blend, G = junction blend, R/B reserved
```

**UV0 cannot break the weld.** It is a pure function of world position, so a junction rim vertex and a segment end vertex holding the same position necessarily hold the same UV0. This is exactly why parent §6.3 specifies world-aligned asphalt: continuity across the boundary is structural, not tuned.

## 4. Lateral subdivision, and why it does not endanger the contract

Slice 2a's property is that a junction rim vertex and a segment end vertex holding the same bits resolve to one vertex index. Subdividing the ribbon adds vertices *between* the two stored cut vertices, and those must weld too.

They are never stored and never recomputed independently. Both paths call one function:

```cpp
FVector2D CutLinePoint(const FVector2D& RightCut, const FVector2D& LeftCut, double Alpha);
// returns FMath::Lerp(RightCut, LeftCut, Alpha)
```

Same expression, same two stored inputs, same `Alpha` from the same profile → bitwise-identical output on both sides. The shared truth is still the solver's two cut vertices; everything between them is derived. **No change to `Solve/`, and no new class of bitwise risk.**

`Alpha` per band boundary comes from the profile: cumulative band width divided by total width, computed once per profile.

## 5. UV1, vertex colour, and the ordering that makes them agree

A cut vertex is *one* vertex, so it carries *one* UV1 and *one* colour. Both the segment and the junction touching it want to write those. They must not disagree.

- **Lateral (UV1.X)** — they already agree. The segment's outer rails are at `+HalfWidthLeft` and `−HalfWidthRight`; the junction's `LeftCut`/`RightCut` are the same two edges. Band boundaries agree because both derive them from the same `Alpha`.
- **Along (UV1.Y)** — they do **not** agree, and this is the trap. The segment measures from end A, so its B-end cut vertices carry `along = length`. The junction at B would naturally write `along = 0`. One vertex cannot hold both.

**Resolution: segments are added before junctions, and `WeldVertex` is first-writer-wins.** Segments therefore own UV1 and colour at every shared cut vertex; junctions supply attributes only for the vertices they alone introduce — arc samples, the inset ring, and the fan apex. Rim vertices from different arms then carry different `along` values, which is harmless because markings are faded out across the junction anyway.

This reverses `RebuildMesh`'s current order (junctions, then segments). The order is now load-bearing and must be commented as such.

- **Junction blend (Colors.G)** — 0 at the rim, 1 at the apex. Shared cut vertices are written by the segment as 0, which is what the junction wants there too, so the two agree by construction. Markings taper into the junction rather than stopping dead.
- **Ground blend (Colors.A)** — 0 at the outermost band's outer edge, 1 at its inner boundary and everywhere inboard. The segment writes 0 at its outer rails; the junction wants 0 at the rim. They agree.

## 6. Shoulder fade at junctions — SUPERSEDED

> **Retired 2026-08-29, after playing it.** There is no fade. An airport's surfaces meet at hard material lines — concrete slab, asphalt run-off, grass — and a road's edge is a kerbstone. Edge treatment is a **per-profile, per-band material choice** on a flat ribbon: a `Curb` band for roads, a `Shoulder` band for taxiways and runways. `ERoadBandType` has carried `{ Shoulder, Lane, Curb }` since Slice 1 and was already shaped for this.
>
> This section describes a feature that was built, shipped in 2b-ii, and removed. It is kept because **the inset ring it specifies survives** — no longer as an alpha ramp, but as exactly the boundary between a junction's outer band and its interior, which per-band materials need. The clamp below is still load-bearing for that.
>
> The fade also concealed a real defect for a day: because the dead-end cap was built from the two outer rails alone, every one of its corners sat on an outer band and the whole cap was masked away, so roads visibly stopped short of where they were drawn. Per-band materials would have failed the same way. The cap is now subdivided band for band.

A junction's rim *is* the outer edge, and the only vertex inboard is the fan apex, so fading rim→apex would fade the whole junction.

The fan gains an **inset ring**: each rim vertex offset toward the apex by the outermost band's width, alpha 1, with the rim at alpha 0 and the apex at 1. Triangulation becomes rim → ring → apex instead of rim → apex.

Offsetting a polygon inward can self-intersect at tight corners. The inset distance is therefore clamped to a fraction of the junction's inradius (distance from apex to the nearest rim edge); where the clamp binds, the ring collapses toward the rim and the junction degrades to little or no fade rather than folding. **Degrading is required behaviour, not a fallback** — a folded ring is a visible defect and a silent one.

## 7. Material contract

`M_RoadSurface`, authored by commandlet:

| Input | Source | Drives |
|---|---|---|
| UV0 | world XY / `TexelsPerUnit` | albedo, normal, roughness, metallic, AO |
| UV1.X | lateral offset, uu | marking mask — centreline near 0, edge lines inset from the rim |
| UV1.Y | distance along, uu | dash pattern |
| ~~Vertex A~~ | ~~ground blend~~ | ~~opacity mask at the shoulder's outer edge~~ — **removed with §6.** The material is Opaque; `UV2.Y` is reserved and always 1. |
| Vertex G | junction blend | fades markings toward a junction's centre |

Scalar/vector parameters: `TexelsPerUnit`, `MarkingColor`, `CentrelineWidth`, `EdgeLineInset`, `EdgeLineWidth`, `DashLength`, `DashGap`.

**Texture import settings matter and fail silently if wrong:** albedo is sRGB; normal, roughness, metallic, AO and height are **not**; the normal map is `pebbled_asphalt_Normal-ogl.png`, an OpenGL-convention map whose **green channel must be flipped** for UE.

## 8. Out of scope

- **Ghost material** (parent §6.6). Listed under §6 but not in §9's 2b contents; it belongs with the build tool in Slice 3.
- **Turn-path markings** — Slice 4, since turn paths do not exist yet.
- **Discrete symbols** (hold-shorts, stand numbers) — decals, Slice 6.
- **Curved-segment sampling.** `AddSegment` still lerps interior samples straight, so a segment with a non-midpoint `Control` renders as a chord. It is a mesh-geometry defect rather than a materials one, and lateral subdivision does not make it worse. Recorded here so it is not lost.

## 9. Testing

UV and colour generation is pure data and fully unit-testable without a World:

- UV0 is a pure function of position — identical positions give identical UV0.
- A band boundary vertex computed from the ribbon and from the junction rim is **bitwise** equal, asserted with `==`, never a tolerance. This is Slice 2a's contract extended and it is tested the same way.
- Adding a segment to a builder that already holds its junction raises `VertexCount()` by exactly the segment's unshared vertices, with the band count accounted for.
- `along` at a B-end cut vertex equals the segment's length, proving the ordering rule holds and the junction did not overwrite it.
- Ground blend is exactly 0 at an outer rail and exactly 1 at the shoulder's inner boundary.
- The inset ring never crosses the rim: every ring vertex lies inside the rim polygon, on a gallery cell with the tightest corner available.
- Every triangle still winds counter-clockwise, and every vertex still lands on one plane.

The material itself is verified by the commandlet reporting the created asset's parameter names, and finally by the gallery visual check.

## 10. Risks

- **Inset-ring self-intersection** is the one genuinely hard piece of geometry. The clamp bounds it, but the clamp fraction needs a value chosen against the gallery's tightest cell, not guessed.
- **Ordering is now load-bearing.** Segments must precede junctions in every rebuild path — `ARoadNetworkActor::RebuildMesh` and `ARoadJunctionGallery::RebuildGalleryMesh` both. A future third caller that gets it wrong produces markings that jump at one end of every segment, with nothing failing.
- **Slice 2a's lesson applies directly:** every 2a test passed while the surface was invisible, because no test crossed into the component. A material that loads but renders wrong will look exactly like one that works, in a suite that is entirely data-level. The commandlet must assert on the created asset, and the gallery check remains the real gate.
