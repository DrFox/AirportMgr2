# Per-Band Materials — Design

**Status:** approved 2026-09-02. Implements the slot mechanism that
`2026-08-28-road-slice2b-materials-design.md` §2 said lateral subdivision *enables* but
never designed.

## 1. What 2b left behind

`FProfileBand::MaterialSlot` is an `FName` that nothing reads. Verified 2026-09-02: it and
`FApronSurface::SurfaceMaterialSlot` are declared, set in tests, and consumed by no code in
`Build/` or `Present/`. Meanwhile `AddJunction` already carries the geometry this needs, and
says so at its site:

> The ring stays because it is exactly the boundary between a junction's outer shoulder band
> and its interior, which is what per-band materials need next.

So the work is a slot *id*, not new geometry.

## 2. Decisions

| Question | Decision |
|---|---|
| Where the name→material binding lives | **A `URoadMaterialSet` UDataAsset.** Flyweight, matching `URoadProfile`. Decisive over a `TMap` on the actor: the level is deliberately never saved, so an instance-edited table would be lost every session. An asset is on disk. |
| Slot container | **`TArray<FRoadMaterialSlot>`, never `TMap`.** The array index *is* the material id baked into the mesh and handed to `ConfigureMaterialSet`; it must be deterministic and stable across rebuilds. `TMap` iteration order is neither. |
| Id granularity | **Per triangle.** Not per vertex — see §4. |
| First slot set | **Asphalt, Concrete, Kerb.** Two map to materials that already exist; one new material, no new texture import. |
| Aprons | **Out of scope.** They render on their own component with a single `ApronMaterial`, so `SurfaceMaterialSlot` is dead for a different reason. Reviving it is a follow-up on this machinery, not a redesign. |
| Junction slots | **From the widest arm** — see §5. |

## 3. Data model

```cpp
USTRUCT() struct FRoadMaterialSlot
{
    UPROPERTY(EditAnywhere) FName Name;
    UPROPERTY(EditAnywhere) TObjectPtr<UMaterialInterface> Material;
};

UCLASS(BlueprintType) class URoadMaterialSet : public UDataAsset
{
    UPROPERTY(EditAnywhere) TArray<FRoadMaterialSlot> Slots;
    int32 IndexOf(FName Slot) const;   // INDEX_NONE when absent
};
```

`ARoadNetworkActor` gains `TObjectPtr<URoadMaterialSet> MaterialSet`. **Null is a supported
state**, not an error: it means one material and every id 0, which is exactly today's
behaviour. That is what keeps the gallery, the tests and every existing level working
unchanged.

## 4. The pipeline, and why the weld is untouched

Four hops:

| Where | Change |
|---|---|
| `FRoadProfileBands` | `+ TArray<int32> SlotIndices`, one per band (`Alphas.Num() - 1` entries). Resolved once per profile, beside the alphas — band identity already lives there. |
| `FRoadMeshBuffers` | `+ TArray<int32> MaterialIDs`, parallel to **triangles**. |
| `FRoadMeshBuilder::AddTriangle` | Gains an id parameter, **with no default argument**. |
| `FDynamicMeshSink::Accept` | `EnableMaterialID()`, set per triangle, `ConfigureMaterialSet`. |

`IRoadMeshSink` keeps its signature: `Accept` already takes the whole buffer struct, so a new
parallel array is not an interface change.

**No default argument on `AddTriangle`.** A default is precisely the mechanism by which a
band silently becomes slot 0, and every caller must be forced to state which surface it is
emitting. This is the same reasoning that put winding in that one function.

**Per-triangle ids leave the bitwise weld alone.** A vertex on a concrete/asphalt boundary is
shared by triangles of two different slots; because the id lives on the triangle, that vertex
stays one welded vertex and nothing is tempted to split it to carry material. The 2a contract
is not extended, weakened or re-tested here — but §7.5 *measures* that it did not move.

The two invariants are unrelated to this change, deliberately: material is a per-face
property and the weld is a per-vertex one.

## 5. Junctions

The rim→ring strip and the ring→apex fan are the two regions `AddJunction` already emits.

- **rim→ring strip** → the outer (shoulder) band slot.
- **ring→apex fan** → the interior (centreline) band slot.

Arms may carry different profiles, and the strip is one continuous annulus, so a junction
takes both slots from a single arm: the **widest** — greatest `GetTotalWidth()`, ties broken
by lowest segment id so the result is deterministic. The dominant road paves the junction. A
junction paved unlike every road entering it is the wrong answer, and per-arm strips would
need the ring subdivided per arm, which the inset does not do.

## 6. Failure handling

`FDynamicMeshSceneProxy` punishes both mistakes here **silently**, which sets the tone for
this section:

- It splits by material only when `HasAttributes() && HasMaterialID() && NumMaterials > 1`.
  Miss one and it falls back to a single buffer on material 0, with no error.
- Inside the split path, `if (MatIdx >= 0 && MatIdx < NumMaterials)` counts an out-of-range
  triangle into *no* buffer. The triangle disappears. Nothing is logged.

So an out-of-range id is never allowed to exist:

- An unresolved slot name resolves to 0 **and is reported** — a warning naming the profile
  and band index, and a count the tests can assert on without parsing a log.
- The sink clamps any id outside `[0, N)` to 0 and warns. Belt and braces, because the
  punishment is invisible.
- `ConfigureMaterialSet` always receives exactly `Slots.Num()` entries; a null `Material`
  entry is filled with the engine default, so no slot is empty and `NumMaterials > 1` holds.

## 7. Testing

Data-level, no world, except where noted.

1. Band slots resolve in band order through the set.
2. An unresolved name yields 0 and is reported — asserted on the returned count, not a log.
3. Every emitted id lies in `[0, Slots.Num())`.
4. `MaterialIDs.Num() * 3 == Indices.Num()`.
5. **The weld measurement.** A profile whose bands carry *different* slots produces an
   identical `VertexCount()` to the same profile with one slot. This measures that
   per-triangle ids split no vertex. An assertion that merely named the contract would pass
   on a mesh that had split every band boundary — the failure mode 2a's plan already shipped
   twice.
6. A junction between two differently-wide profiles takes the widest arm's slots.

**The real gate is the visual check.** 2a's whole test suite passed while the surface was
invisible, because no test crossed into the component. A material set that resolves perfectly
and renders as one flat colour looks exactly like one that works, to every test above.

## 8. Materials

| Slot | Material | Source |
|---|---|---|
| `Asphalt` | `M_RoadSurface` | exists |
| `Concrete` | `M_ApronConcrete` | exists |
| `Kerb` | `M_RoadKerb` | new, `Tools/Python/build_kerb_material.py` |

`M_RoadKerb` reuses the imported concrete textures, darker and rougher, with no markings. No
new texture import. `DA_RoadMaterials` is authored by script rather than by hand, because
slot order is load-bearing and a hand-edited asset records no reason for its ordering.

`URoadProfile::MakeTransient` gains slot names — shoulder `Asphalt`, lane `Concrete` — so the
gallery and PIE show the effect the moment the mechanism works, rather than needing an asset
authored by hand first.

## 9. Out of scope

- **Aprons**, per §2.
- **Marking geometry.** `M_RoadSurface` measures its edge lines off UV1.X across the *full*
  profile width, so with the lane on its own slot an edge line may fall under the shoulder
  material. That is a parameter to tune at the visual check, not a reason to change UV1.
- **Curved-segment sampling**, still carried from 2b §8.

## 10. Risks

- **Slot order is a silent contract.** Reordering `DA_RoadMaterials` re-skins every existing
  mesh with no error. Authoring it by script is the mitigation; a test that pins the expected
  order would only pin the test's own copy.
- **`ConfigureMaterialSet` has no slot names.** The engine matches purely by index, so the
  mesh's ids and the material array must be built from the same `Slots` array in the same
  pass. They are, in `FDynamicMeshSink`.
- **The one-material fallback looks like success.** Both proxy hazards in §6 render a
  complete, plausible road. Only the visual check distinguishes them.
