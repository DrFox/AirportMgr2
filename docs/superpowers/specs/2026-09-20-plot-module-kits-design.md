# Plot module kits: a depot whose capacity is proven, not priced

How a drawn plot decides what it can hold, how those modules become real meshes, and what
Blender ships to make that possible.

Builds on `2026-09-16-organic-module-placement-design.md`, which put the scatter solver in
place. That document's §6 (empty-slot markers removed, `RoomForMore` as a sampled number) is
overturned here, and §10 says why the objection it raised does not apply to what replaces it.

---

## 1. What this is for

`Solve/PlotYard.h` scatters a depot's modules and `Build/DepotKit.cpp` tells it how big each
one is. Both work. Neither has ever seen a mesh: `DepotKit.cpp`'s own comment says the
figures are "Grey-box figures chosen for LEGIBILITY... the real ones arrive with the meshes."

This is the document where they arrive. It covers three things that turn out to be one
thing:

- **Where a module's numbers live** once a mesh exists, so the footprint the solver uses and
  the mesh the player sees cannot disagree.
- **What a plot's capacity means** - the question that has to be settled before a ghosted
  preview can be honest about what the player is buying.
- **What Blender ships**, and the conventions that let building two cost less than building
  one.

`FuelDepot1/` is the first kit and the only one authored here. Everything stated as a
convention is stated so the goods depot, the terminal and the pushback depot inherit it
unchanged.

## 2. Decisions taken

- **Capacity is RESERVED at draw time, not PRICED.** The solver fills the plot once, and
  that solved layout is both the preview and the ceiling. Buying a module lights a slot that
  was already proven to exist. See §3 for the cost model this replaces and why it failed.
- **Slots are typed.** A shed stand is a shed stand. The player chooses what to buy and in
  what order, but cannot convert an unused shed slot into a tank slot. See §3.3.
- **Kits are hand-authored `UDataAsset`s, exactly as `UAircraftType` is.** No manifest
  emitted from Blender, no generated asset. Drift is caught by registry-walking tests, which
  is the precedent `AircraftLookTest` set and the reason it exists.
- **Blender bakes whole shed variants; it does not ship a wall kit.** One parametric script
  emits a 1-, 2- and 3-bay shed. The runtime assembles nothing. See §7.2.
- **The parts path is designed and not built.** `EKitAssembly` has both values from day one;
  `ResolveParts()` is a `checkf(false)` with a comment naming what it is for. A building that
  needs runtime assembly writes a resolver, not a seam.
- **The solvers stay dependency-free.** `PlotFit` and `PlotYard` keep `CoreMinimal.h` and
  nothing else, and still never see `EDepotModule`. Kits resolve presenter-side. This is not
  tidiness: it is what keeps the work in §5 testable with no world, no actor and no
  `NewObject`, and `Check-Architecture.ps1` enforces the direction.
- **The perimeter fence is not a kit.** It is plot infrastructure, derived from the outline,
  and Blender already shipped it. See §8.
- **Reservations are recomputed, not saved.** The layout is a pure function of the plot. See
  §3.4 for what that costs.

## 3. Capacity: reserved, not budgeted

### 3.1 The model that was rejected

The first model gave each plot a budget - a count of `PlotFit` bays, or square metres - and
each kit a cost. The player would spend the budget at upgrade time on whatever mix they
liked.

It was rejected for two reasons, in the user's words:

> *are we sure we could always resolve the costs to available space in the plot, there is a
> big difference between sheds at 32m2 and tanks at 25m2 but they cost the same... a player
> cannot just fill all space within the plot with kits, the layout resolver needs to give
> space etc.*

**A budget in any abstract unit asserts that a packing exists without proving one.** Moving
to square metres makes the price fairer - with the 1 m clearance skirt a shed really costs
~45 m2, a tank ~36, a pump ~12 - and changes nothing about the guarantee. The assertion
would be made against a solver that keeps a gate corridor clear, holds 1 m between modules,
jitters headings by up to 12 degrees and gives up after 64 tries, packing into a plot the
player drew freehand and which may be concave. The budget would be right nearly always and
occasionally wrong, and the occasional wrong is the expensive kind: the player spent money
and got a module that could not be placed.

**And it costs the preview, which already ships.** A plot's potential layout is shown while
the player drags the outline. A budget cannot preview, because the mix is unknown until
upgrade time. Trading a shipped feature for a trade-off the player has not asked for is the
wrong trade.

### 3.2 What replaces it

`PlotFit` and `PlotYard` run **once, at draw time**, and fill the plot with as many modules
as actually fit. That layout is simultaneously:

- the **preview** while the outline is being dragged, drawn ghosted;
- the plot's **capacity**, per kit;
- and, once built, **where each module stands** - unchanged, because nothing is re-solved.

Buying a module does not run the solver. It lights a slot.

What this buys:

| Before | After |
|---|---|
| `RoomForMore` is a sampled estimate from a standard footprint | a count of unlit slots |
| `DroppedCount() == 0` is a hope | an invariant; a violation is a bug |
| clearance and gate corridor are a correction factor someone guesses at | paid for inside the solve |
| the player reads a number | the player sees the finished depot |

### 3.3 The mix, and what the player still chooses

The solve needs a rule for how many of each kit to reserve. It is a **weighted round-robin**:
each kit carries a `ReserveWeight`, the solve cycles by weight - `Shed Shed Shed Tank Tank
Pump |` repeating - placing stands until a whole cycle places nothing.

**Weights set ceilings; they do not set strategy.** This was the objection raised against
them, and it is worth recording because it looks right and is not:

> *A player may want to prioritize growing fuel storage over delivery and we limit that with
> all of our choices. Or are we? Just because the plot can hold 6 sheds and 2 fuel tanks
> doesnt mean they need to upgrade 6 sheds, they can still upgrade to a second fuel tank
> while they only have 1 shed*

Exactly so. The trade-off moves from *what the plot can hold* to *what you spend money on
first*, which is the one that was always interesting. A storage-first player buys their
second and third tank while still running one truck. A ceiling nobody reaches costs nothing,
so weights should be set generously.

**The one real limit: a kit's ceiling is fixed when the plot is drawn.** A plot that reserves
6 sheds and 4 tanks caps a storage-obsessed player at 4 tanks with five shed slots standing
empty. They cannot convert; they redraw the plot, or build a second depot.

Fungible pads - identical stands sized to the largest module, any kit in any pad - would
remove that limit. Rejected: roughly 25% more ground per module (a pump sitting in a
shed-sized pad), a preview that can only show generic pads instead of the real depot, and
shed runs would need a rule for claiming adjacent pads. The limit is cheaper than the cure,
and §11 says what to watch for.

### 3.4 Recomputed, not saved

The reservation is a pure function of `(outline, frontageA, frontageB, gate, kit specs,
seed)`. Every one of those is already stored on the plot, and `Seed` exists precisely to make
the solve repeatable - that was §5 of the organic-placement spec, called a requirement and
not a nicety.

So **nothing about the reservation is saved.** `FEntityInstance::Modules` stays exactly as it
is, holding what the player bought; the stands are re-derived. No save migration, and truck
count keeps deriving from the sheds in `Modules` as `FEntityPlacement`'s comment describes.

The cost, stated so it is not discovered in a patch: **retuning a kit's `ReserveWeight` or
footprint relayouts every existing airport.** Accepted. The escape, if it ever becomes
intolerable, is to persist the reservation and version it - which buys a migration problem
today for a risk that may never land.

## 4. `UPlotModuleKit`

A `UDataAsset` in `Plugins/Airside/Source/Airside/Public/Entities/`, sibling to
`UAircraftType`, authored by hand as `Content/Entities/DA_Kit_FuelShed`, `DA_Kit_FuelTank`,
`DA_Kit_FuelPump`.

**Hand-authored, with provenance in the header comment.** This is the airframe pattern and it
is deliberate. `UAircraftType` states that its figures come from published dimensions and
standard ramp practice; `MainWheelRadius` records that it was measured off
`piper_aligned.blend` and is "the same number arrived at twice". A JSON manifest emitted by
the Blender build script was considered and rejected: it is a mechanism this codebase does
not use anywhere, and what it would buy is bought more cheaply by the bounds test in §9.

```cpp
UENUM()
enum class EKitAssembly : uint8
{
    /** Whole meshes, one per bay count. Blender bakes them; see design §7.2. */
    Baked,
    /** Cap + Bay x N + Cap, assembled at runtime. DESIGNED, NOT BUILT - see §2. */
    Parts
};

UCLASS(BlueprintType)
class AIRSIDE_API UPlotModuleKit : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere) FText DisplayName;

    /** Plan extent of ONE module. Length is along its own +X, which faces away from
     *  the road - see UEntityDefinition::BuildFuelDepot. */
    UPROPERTY(EditAnywhere) FVector2D Footprint = FVector2D::ZeroVector;

    /** Grey-box height. Retired once BakedMeshes is set; see the fallback below. */
    UPROPERTY(EditAnywhere) double HeightUu = 0.0;

    /** POSITION ONLY, never heading - as PlotYard::FFootprint states. */
    UPROPERTY(EditAnywhere) bool bAgainstTheBackFence = false;

    /** Round-robin weight. Sets a CEILING, not the player's strategy - see design §3.3,
     *  which is why a generous number costs nothing. */
    UPROPERTY(EditAnywhere) int32 ReserveWeight = 1;

    /** Modules of this kit grouped into one run. 1 = never grouped. */
    UPROPERTY(EditAnywhere) int32 RunCap = 1;

    UPROPERTY(EditAnywhere) EKitAssembly Assembly = EKitAssembly::Baked;

    /** Baked only. Index = bay count - 1, so exactly RunCap entries, none null. */
    UPROPERTY(EditAnywhere) TArray<TSoftObjectPtr<UStaticMesh>> BakedMeshes;

    /** Parts only. UNUSED - Assembly::Parts is unimplemented, see ResolveParts. */
    UPROPERTY(EditAnywhere) TSoftObjectPtr<UStaticMesh> PartCapMesh;
    UPROPERTY(EditAnywhere) TSoftObjectPtr<UStaticMesh> PartBayMesh;
    UPROPERTY(EditAnywhere) double PartPitchUu = 0.0;
};
```

`Build/DepotKit.cpp`'s hardcoded switch becomes a lookup through a
`TMap<EDepotModule, TSoftObjectPtr<UPlotModuleKit>>` on `UAirsideContent`, which is already
the home for game-wide content defaults such as `AgentMesh`.

**Two fallbacks, both deliberate.** A kit with no meshes draws its grey box, as today. A
missing kit falls back to the current hardcoded figures. So every line of §5 and §6 lands
and is testable with no Blender work at all, and the two halves of this proceed
independently.

## 5. `PlotYard::Reserve`

New, beside the existing `LayOut`, in the same dependency-free header.

```cpp
struct FKitSpec
{
    FFootprint Footprint;    // ONE module's, not a run's
    int32 ReserveWeight = 1;
    int32 RunCap = 1;
};

struct FReservedStand : FStand
{
    int32 KitIndex = INDEX_NONE;  // into the Kits array given
    int32 RunLength = 1;          // modules this stand holds; 1 unless grouped
};

struct FReservation
{
    /** Every stand reserved, in placement order. */
    TArray<FReservedStand> Stands;

    /** Ceiling per kit, indexed as the Kits array was. DERIVED from Stands, never
     *  stored alongside it - a second count is a second thing to keep in agreement,
     *  which is the reason FYard::DroppedCount is derived too. */
    int32 CeilingFor(int32 KitIndex) const;
};

AIRSIDE_API FReservation Reserve(TArrayView<const FVector2D> Outline,
    FVector2D FrontageA, FVector2D FrontageB, FVector2D Gate,
    TArrayView<const FKitSpec> Kits, int32 Seed);
```

**It still never sees `EDepotModule`.** A spec is a footprint and two integers; mapping a kit
to its spec stays `UPlotPresenter`'s job on the other side of the seam. That is the same
split `FitBays` makes by taking an outline rather than an entity, and it is what keeps these
tests free of a world.

**The fill.** Weighted round-robin, placing each stand with the machinery already there -
`ClearanceUu`, `GateCorridorUu`, `MaxTries`, `HeadingJitterRadians`, `bAgainstTheBackFence`.
When a whole cycle places nothing, the plot is full.

**Runs.** A kit with `RunCap > 1` reserves **at full run width, up front**: three 8 x 4 m
sheds become one 8 x 12 m stand at a single heading, with no jitter inside the run. Owning
fewer than `RunCap` draws the shorter baked mesh anchored to one end, the remainder ghosted,
so a run visibly grows along its length.

Reserving at full width rather than growing is what keeps §3.2's guarantee: a run that grew
would need space it was never promised.

**`PlotFit` is unchanged.** It answers "how many bays of frontage does this plot have", which
is still the gate on whether a plot is legal at all (`EPlotRefusal::TooSmall`). `Reserve`
answers a different question and does not replace it.

## 6. What the presenter draws

**One ISM per distinct static mesh**, replacing the single `Boxes` component. `ModuleTransform`
and `ModuleBoxes` are how the existing tests reach in, so a module-index-to-transform
indirection sits in front of the components and those stay meaningful. Retrofitting that
later would mean rewriting the tests at the same time as the thing they guard.

**Unlit slots draw with `UAirsideContent::GhostMaterial`**, which already exists for the
build-mode preview.

**A part-built run** draws `BakedMeshes[owned - 1]` offset by `(runWidth - meshWidth) / 2`
along the run axis, anchoring it to one end. Each baked variant's origin is its own footprint
centre (§7.3), so this offset is the only place the arithmetic lives.

## 7. The Blender side

### 7.1 Layout

`FuelDepot1/` grows the house shape, as `accessories/chainlink/` and `fueltruck1/` have it:

```
FuelDepot1/
  Concept/concept.png
  fueldepot1.blend
  scripts/build_shed.py       -> export/SM_FuelShed_1Bay|2Bay|3Bay.fbx
          build_tank.py       -> export/SM_FuelTank.fbx
          build_pump.py       -> export/SM_FuelPump.fbx
          preview_depot.py    -> renders/depot_preview.png
  export/
  renders/
  README.md
```

One `.blend` holding all three modules and several exported meshes - the chainlink pattern,
one blend and two posts. Scripts run `blender -b -P <script>`.

### 7.2 The shed script emits all three variants in one run

Not a bay count on the command line. Argument passing through `blender -b -P` is awkward, and
emitting the whole set in one run means the three variants can never be built from different
source states.

Bay pitch is one constant, so `SM_FuelShed_2Bay` is **by construction** twice the bay width of
`_1Bay`. That is what makes the bounds test in §9 a real check rather than a circular one.

### 7.3 Origin, orientation, collision, materials

**Origin** is each variant's own footprint centre at Z = 0 - not the run's centre. The
presenter applies the anchoring offset (§6), so the meshes stay simple and the arithmetic
lives in one place.

**Orientation** follows the repo rule rather than inventing a building-specific one. Blender
-Y forward becomes UE +X via the `UE_YAW_DEG = 90.0` yaw, exported with `axis_forward` of
`-Z` and `axis_up` of `Y`. In engine +X faces **away** from the road - `BuildFuelDepot`'s
comment records that this was once written the wrong way round - so:

> **The shed's back wall faces Blender -Y. Its door faces +Y.**

That is the sentence in the README most likely to be got wrong later, and it goes in with the
reason attached.

**Collision** is `UCX_<mesh>_00` in the FBX, per the chainlink convention. One box per bay
rather than one per run, which keeps the three variants consistent with each other.

**Materials** follow the fleet contract - slot names are the Unreal contract,
`<asset>_<look>`, flat colour, master plus MI: `fueldepot_steel`, `fueldepot_roof`,
`fueldepot_concrete`, `fueldepot_tank`, `fueldepot_hazard`. The palette is lifted verbatim
from the existing GSE fleet, read out of the exported glbs rather than retyped.

No texture set. The concept sheet's decals (AVGAS 100LL, the hazard diamond) are deferred
rather than opening a texture pipeline for this pass - a named debt, not an oversight.

### 7.4 `KITS.md`

`AirportMgr2Models/KITS.md`, the building-side equivalent of the fleet material naming rule.
Three parts:

**Invariants**, obeyed by every plot building at every tier: roof pitch, door height,
cladding rib spacing, bollard diameter and spacing, pad edge treatment, painted-line width,
palette, material naming, origin rule, collision, tri budget. The concept sheet pins some
already - the shed is 3.5 m to the ridge, the site 11.0 x 8.0 m. The rest are fixed when the
shed is built. The doc is where they are **recorded**; the point is that building two writes
down nothing new.

**Tier bands**, sharing the invariants and differing only in material: T1 ribbed steel (the
concept sheet), T2 masonry, T3 glass. Only T1 is authored now. The GDD's progression is
"a portacabin becoming a brick block becoming a glass tower", and a band structure that
exists before anyone needs it costs a heading and a sentence.

**Plot dressing** - see §8.2.

## 8. The perimeter fence, and plot dressing

### 8.1 The fence is built and not connected

`accessories/chainlink/export/` holds `SM_Fence_Post.fbx` and `SM_Fence_CornerPost.fbx`.
Searching `Content` for any asset named Fence in this project returns nothing. Meanwhile
`PlotPresenter` draws the perimeter as grey boxes. The asset exists; the wire does not.

**It is not a kit.** Every plot has a fence regardless of what is built inside it; it derives
from the outline, is not reserved, not bought and not scattered. Its data home is
`UAirsideContent`, beside `SurfaceMaterial` and `GhostMaterial`.

**The existing numbers are wrong, and the fence asset is the authority.** `PlotPresenter` has
`FenceBayUu = 250.0` and `FenceHeightUu = 200.0` - 2.5 m bays, 2 m tall. The chainlink README
is explicit: post 2.450 m, fabric 2.400 m, the texture's U tiling every 2.400 m of run and V
spanning 0..1 over 2.400 m of height without tiling. Its warning is exact:

> *`FABRIC_TOP` and `POST_H` in `build_posts.py` are one decision, not two. The texture's V
> range IS 2.400 m - change the fabric height without regenerating the texture and the
> diamond pattern stops being square.*

So **the presenter moves to 2.400 / 2.450; the texture does not move to 2.5.** Those two
constants are deleted and replaced by figures read from the fence's definition, so they
cannot drift apart again.

**The build split is already decided and this honours it.** Blender ships two posts and a
texture; UE builds the fabric. Per the README's table: one HISM of posts, one generated
fabric quad strip, the top rail, and a per-edge collision wall - two draw calls for a whole
perimeter, against one box per bay today. Its reasoning applies verbatim: a plot edge is
whatever length the player drew, so a fixed-length fabric mesh either stretches the diamonds
or leaves a gap at one end.

**Two things the current loop does not do**, both in scope here:

- **Corner posts.** `SM_Fence_CornerPost` is 90 mm diameter against the line post's 60 mm.
  The present loop walks each edge independently and has no notion of a corner, so corners
  get a doubled line post or none. Corner detection comes off the outline.
- **The gate needs end posts.** `GateGaps` currently skips the bay nearest the entity pose. A
  real gap is fabric omitted between two posts that are still standing, and the gap is
  measured against `GateCorridorUu` rather than falling out of bay arithmetic. The README's
  framing is the one to keep: *"a depot the truck cannot leave looks perfectly correct from
  every angle"*, which is why that gap is counted rather than eyeballed.

**Import**: both FBXs into `Content/Environment/`, with `chainlink.png` / `chainlink_n.png`
and a `chainlink_post` material. The README warns that name arrives in UE as-is and should
not be renamed casually.

### 8.2 Plot dressing

The fence is the first of a third category the design was otherwise missing. **Kits are
bought; dressing follows from geometry.** `accessories/` already holds `bollard.blend`,
`barrier.blend` and `cone.blend`, and the concept sheet rings the pump and the shed door with
bollards.

Dressing is placed by rule against the solved layout - bollards along the pad edge and beside
a pump stand, cones near the gate - and never occupies a reserved slot. `KITS.md` carries the
rules; no dressing beyond the fence is authored in this pass.

## 9. Testing

### 9.1 Registry-walking, per the airframe precedent

`AircraftLookTest` earned this the hard way and its comment is the rule: **check where a list
is CONSUMED, not where it is declared.** Its first version compared a hand-written pair and
passed while the A320 and the 737 both still wore the default mesh. So these walk the
`EDepotModule` enum and the asset registry, never a list written in the test:

- Every `EDepotModule` value resolves to a kit through `UAirsideContent`.
- No two kits share a mesh - the exact bug class `AircraftLookTest` exists for.
- Every `Baked` kit has exactly `RunCap` entries in `BakedMeshes`, none null.
- **Each kit's stated `Footprint` matches its 1-bay mesh's bounds, and the N-bay mesh's width
  is N times the 1-bay width**, within tolerance. This is most of what a JSON manifest would
  have bought, for a few lines.
- Every kit mesh's material slots match `<asset>_<look>`.

In the game module, not `Airside` - these are `/Game` assets and `Check-Architecture`
enforces that direction, as `AircraftLookTest`'s own comment notes.

### 9.2 Solver, world-free in `AirsideTests`

- `Reserve` never drops: every `FReservedStand` it returns has `bPlaced` true. Unlike
  `LayOut`, which is handed a list it must try to honour and reports what it could not fit,
  `Reserve` only ever returns what it placed - so a dropped stand is not a refusal, it is a
  bug. Under the budget model this was a hope; here it is an invariant.
- No two stands overlap, checked with the public `StandCorners` so test and solver share one
  derivation and the test is not checking its own arithmetic.
- No stand intrudes on the gate corridor.
- **Determinism**: same inputs and seed produce identical stands. Load-bearing - it is what
  permits §3.4's recompute-instead-of-save.
- A run's stand is exactly `RunCap` times the kit's own `Footprint` width, at a single
  heading, with no internal jitter.

### 9.3 Presenter and fence

The census line counts boxes today - `"%d fence panel(s), %d gate gap(s)"`, derived from
`Boxes->GetInstanceCount() - ModuleBoxes`. Once the fence is a strip plus an HISM that
arithmetic stops meaning anything, so it moves to counting posts and spans directly.

**This seam is known-fragile**: `PlotPresenter.cpp` already carries a comment that modules
must be added before the fence or `PlotPresenterScattersModules` silently measures fence
panels instead. This change walks straight through it, so the ordering comment and the test
are revisited together rather than one at a time.

New: fabric run length is an exact multiple of the 2.400 m tile within tolerance, and every
outline corner gets exactly one corner post.

## 10. What this overturns

**§6 of `2026-09-16-organic-module-placement-design.md`**, which reads:

> *The empty-slot markers are removed. They were added the same morning (Task 7) and were
> honest then: a marker was a BAY, and a bay was a thing a module would stand in. Once
> modules do not stand in bays, a grid of slot markers is a claim about the plot that is no
> longer true.*

**That objection does not apply to what replaces them, and the distinction is the whole
design.** The markers it removed were a GRID - uniform bays, drawn beside scattered modules,
claiming a structure the yard did not have. A ghosted slot here is **a solved stand**: its
own footprint, its own sampled heading, produced by the identical code path that will place
the module when it is bought. It does not claim the yard has a structure. It shows the yard.

That spec's ONE EVALUATOR principle survives intact and is in fact strengthened. It required
that "the number the player reads is produced by the code that would actually place the
thing". Here the player does not read a number at all - they see the placement itself, and
`RoomForMore` stops being a sampling estimate and becomes a count of unlit slots.

**§4 of `2026-09-15-plot-built-buildings-design.md`** continues to stand as the PLOT's
contract - frontage, gate, pad - exactly as the organic spec left it.

**Nothing in this document reinstates pipework.** That remains a named fidelity debt of the
organic spec, and free rotation still forbids the stub convention that would have bought it.

## 11. Risks and named debts

**Reservation is not monotonic in plot size.** `Reserve` samples candidate poses from the
region a module can occupy, and that region changes shape as the outline changes. Enlarging a
plot slightly can resample differently and reserve FEWER of some kit - drag the outline out
by half a metre and watch the tank ceiling drop from 3 to 2. It is rare, it is legal, and it
will read to a player as a bug. §3.4's recompute-don't-save makes it worse, because there is
no previous solve to hold the line against.

Three responses, in order of what to spend:

1. **Watch for it in PIE.** The preview updates live while dragging, so a dip is visible
   during development. It may never bite at real plot sizes. Do this first and measure.
2. **Replace random sampling with a deterministic scan** - a space-filling order over the
   plot instead of `MaxTries` random draws. Near-monotonic by construction, and it removes
   `Seed`'s reason to exist. Costs a rewrite of the placement loop and some of the "parked in
   a hurry" character the jitter buys, which was an explicit goal of the organic spec.
3. **Persist the reservation**, so an existing plot's ceilings can never drop. Solves it
   completely; buys the save migration §3.4 avoided.

**A kit's ceiling is fixed at draw time** (§3.3). Watch for players who want a mix the
weights do not reach. The cure is fungible pads and it is expensive.

**Kit weights and footprints are layout-affecting** (§3.4). Changing one in a patch
relayouts existing airports.

**Decals are deferred** (§7.3). The concept sheet's AVGAS lettering and hazard diamonds are
not authored in this pass.

**Trucks still pop at the gate**, and depot module sizes are therefore set dressing rather
than derived from vehicle dimensions. Recorded here because it is load-bearing for §7: the
shed does not have to fit `fueltruck1`, whose `LENGTH_TARGET` is now 8.50 m against an 8 m
shed. If an articulated fuel truck is ever added, this is the assumption to revisit.

`PlotYard::GateCorridorUu` still carries a comment calling 6.2 m "the fuel truck's length",
which is stale - the corridor is about keeping the gateway clear, and the figure should be
restated on its own terms whenever that file is next touched.

## 12. Out of scope

- Every plot building except the fuel depot. The conventions are written generally; only
  `FuelDepot1` is authored.
- Tiers. T2 and T3 are a band structure in `KITS.md` and nothing more.
- `EKitAssembly::Parts`. Designed, not built.
- LODs. Not needed at these tri counts; revisit when a plot holds many depots.
- Dressing beyond the fence.
