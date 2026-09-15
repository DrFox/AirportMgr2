# Plot-built buildings, and the fuel depot as the probe

2026-09-15. Revisits `2026-09-05-game-systems-map-design.md` §1.3, which ruled polygon-fill
out as "a new solver plus a content pipeline". Half that price turns out to be paid.

Concept sheet: `C:\repos\AirportMgr2Models\FuelDepot1\Concept\concept.png` ("Tier 1 Fuel Depot").

## 1. Direction

**You draw your airport, you do not stamp it.**

Runways, taxiways, roads and aprons are already drawn. Buildings are the one thing that
would be stamped, and that is the inconsistency this closes. The pillar is not "we did
Manor Lords buildings" - that is a novelty claim, and it invites a comparison the game does
not need to win. It is that every other airport builder - Airport CEO, SimAirport, SkyHaven -
is grid-and-plop, and none of them can say the sentence above.

### 1.1 Why §1.3's price is stale

§1.3 priced a solver AND a content pipeline. The input half already exists and is tested:

| Piece | Where | State |
|---|---|---|
| Freeform closing polygon gesture | `FApronOutliningState` | Built. Refuses self-crossing (`WouldCross`), previews, undoes |
| Polygon as a model object | `FApronSurface::Outline` | Built. CCW, implicit close |
| Polygon to pavement mesh | `RoadMeshBuilder` apron path | Built, with material slots |
| Nearest-road query from a pose | `FAnchorLink` | Built |

What is genuinely new is four things: which edge is the frontage, fitting modules against
it, tiling a boundary, and the kit. **The depot's concrete pad is not among them** - it is
the polygon the player drew, through the tessellation aprons already use.

### 1.2 This is a probe, deliberately

Option A of three (probe / pillar-first / stamp-now). It answers one question - *does
drawing a plot feel like a plot, or like a chore?* - on one building, before the pillar is
committed. The kit is not a bet on the outcome: it is needed under all three options.

## 2. Decisions taken

Recorded here so the reasoning survives.

- **The plot is a cap, not a derivation.** Plot size sets a bay budget; the player chooses
  the mix. Pure derivation ("draw bigger, get more") is not a decision. The mix - storage
  against turnaround against trucks - is.
- **One gate, not one pose per shed.** `BuildFuelDepot` already ruled that *"two lead-ins
  from one small building into one road is a duplicate painted line"*. Multiple sheds each
  with a pose resurrects exactly that. The yard has one gate and the gate is the pose.
- **Tanks are visual and capped but mechanically inert.** No fuel inventory exists anywhere
  in the codebase - no capacity, no litres, nothing to run dry. Growing a consumable economy
  inside a feel probe is how a probe stops being one. Storage is recorded and unread.
- **The plot lives on the entity, not beside it as an apron.** Rejected: committing an apron
  AND an entity linked by id. It is truthful - a depot's pad genuinely is pavement - but it
  makes undo group two objects and obliges deleting one to delete the other. One
  installation is one memento and one delete.
- **One row of bays, depth 8 m.** Extra depth is yard, not a second row. Manor Lords'
  backyard, and it keeps the fit arithmetic rather than a packing solver.

## 3. The plot model

`FEntityInstance` grows an `Outline`: the drawn polygon in entity-local space, closed
implicitly and **not** repeating its first point - the same contract `ServiceLoop` states,
for the same reason (a repeated value is a value that must agree with another value in the
same array).

### 3.1 FootprintExtent changes meaning

Today it is the whole site - `(600, 400)` uu, 12 m x 8 m, commented *"a tank, a pump, and
room to turn a bowser round"*. Under plots it becomes **one bay**: `(200, 400)` uu, 4 m x
8 m. The site is whatever was drawn. This is a semantic change to a shipped field and is
called out because a saved level's number will not mean what it meant when written.

### 3.2 The 11 m / 12 m discrepancy resolves itself

The concept's plan view dimensions the site at 11.0 m x 8.0 m; the old `FootprintExtent`
said 12 m x 8 m. With a 4 m bay, **the old 12 m is exactly three bays**, and the Tier 1
depot as drawn is `1 shed + 1 tank + 1 pump`. The concept's 11 m was a drawing, not a
decision; the code's figure was right and now decomposes into something meaningful.

## 4. The fit

### 4.1 Frontage is not the solver's question

Finding the nearest road needs the network, so **the facade picks the frontage edge** - the
same query `FAnchorLink` already makes from a pose, asked of an edge - and passes it in.
That is what keeps `Solve/` free of engine types beyond `CoreMinimal.h`.

### 4.2 Solve/PlotFit.h

```
struct FPlotBay
{
    FVector2D Centre = FVector2D::ZeroVector;
    double    Heading = 0.0;   // radians, +X away from the road
};

struct FPlotFit
{
    TArray<FPlotBay> Bays;     // one per buildable bay, frontage-aligned
    bool bFits = false;
    EPlotRefusal Why = EPlotRefusal::None;
};

FPlotFit FitBays(TArrayView<const FVector2D> Outline,
                 FVector2D FrontageA, FVector2D FrontageB);
```

A bare centre-and-heading rather than `FTransform2D`: that type lives in
`Math/TransformCalculus2D.h`, which `CoreMinimal.h` does not pull in, and `Solve/` is
allowed nothing beyond it.

World-free, `NewObject`-less, testable with no world - the `Solve/` contract.

**Bays are 4.0 m wide x 8.0 m deep**, laid along the frontage edge in the strip behind it.
Bay count is the length of the largest frontage-aligned 8 m-deep rectangle inscribed against
that edge, divided by 4, floored. Arithmetic the player can see: a wider plot is visibly
more bays.

**Modules face +X away from the road.** `BuildFuelDepot` is explicit - *"+X FACES AWAY FROM
THE ROAD... the truck drives out behind it"* - and the comment records that this was once
written the wrong way round. The fit honours the corrected statement.

## 5. Modules and what they drive

| Module | Bays | Drives | Machinery today |
|---|---|---|---|
| Shed | 1 | `Trucks` | **Live.** `FuelService.cpp:160,165` gates dispatch on `Instance.Trucks` |
| Pump | 1 | Dwell | **Live-ish.** `DwellSeconds = 40.0`, set from `UScenario::FuelDwellSeconds` |
| Tank | 1 | Storage | **Inert.** Recorded, unread. See §2 |

`Trucks` stops being copied from the definition and starts being **derived from shed count** -
still snapshotted at placement, because `Model/` must not dereference `Entities/` and
`UFuelService` lives in another module's `Model/` besides. **`UFuelService` does not change
at all.** It still reads `Instance.Trucks`. A module system that lands without touching its
consumer is the sign the seam was already in the right place.

Dwell becomes `BaseDwell / PumpCount`, floored at a minimum so a pump farm cannot make
refuelling instant.

**A mix with no shed, or no pump, is allowed and warned** - not refused. The depot is inert
without either, and the anchor census already has the habit of saying so rather than
forbidding it. Zero pumps is therefore a reachable state and **must not reach the division
above**: no pump means no fuelling at all, checked before the dwell is computed, not papered
over with a `FMath::Max(1, ...)` that would silently give a pumpless depot a working dwell.

## 6. The seam

`IRoadEditTarget::PlaceEntity(FVector2D, double, EPlaceableEntity)` cannot express a plot:
a plot-placed entity's pose comes from the **fit**, not from the player's press-and-drag.

`RoadEntity.h:883` already wrote down what to do when this arrived:

> *If a FOURTH capture arrives, these become one struct passed by reference: three trailing
> defaulted parameters on `PlaceEntity` is the most a caller can still get right.*

The module list is that fourth fact. So `PlaceEntity` takes `FEntityPlacement`, carrying
`Where`, `Heading`, `Kind`, `Outline` and `Modules`. An empty `Outline` means an ordinary
plop, which is how `FStandPlaceTool` keeps working unchanged.

The plot tool reuses `IApronDrawState`'s outlining states; only the commit differs.

## 7. The kit

### 7.1 Per-module origins

An earlier draft of this design said one Blender scene, shared origin, separate exports.
**That was wrong and is corrected here**: modules repeat and move independently, so each
carries its own local origin - bay centre, on the ground - and tiling is `index * 4 m`.

The consequence is that **pipework cannot be baked across modules**. Adjacent modules mate
via stubs at the bay edge, at a fixed height and lateral offset recorded as a convention, so
a tank beside a pump connects because the grammar put them adjacent. A procedural pipe
between arbitrary modules was rejected as a second evaluator of a layout the bays already
determine.

One Blender scene still, for proportion: an 8 m shed cannot be judged without the 6.2 m
truck beside it.

### 7.2 Parts

| | Parts | Placed by |
|---|---|---|
| Modules | `depot_shed`, `depot_tank`, `depot_pump` | the fit, one per bay |
| Boundary | `fence_panel`, `fence_gate` | tiled round the outline, gate on the frontage |
| Extensions | `windsock_mast`, existing `bollard`, `cone` | scattered in leftover yard |

**The truck is not in the kit.** The concept shows one parked in the shed; it is an
`FRoadAgent` that drives out, and baking it in gives two trucks the moment one is dispatched.

### 7.3 Conventions

From `fueltruck1/README.md`, unchanged: 1 unit = 1 m, **-Y forward in Blender** (becomes +X
in Unreal on FBX export), +Z up, resting on Z=0. Module forward is the direction facing away
from the road, so it agrees with §4.2.

`fence_panel` is the exception to bay origins: it tiles along the outline, so its origin is
at a post and its bay length is recorded separately.

### 7.4 The painted lines are not modelled

The concept's white and yellow lines on the concrete mark the truck's path off the pad. That
path is the pose's lead-in, which the guideline graph already computes and draws. Painting
them into the pad mesh makes a second evaluator of where the truck drives - they would agree
at one rotation and visibly disagree at every other, which is the failure the
"guideline graph samples ONCE" invariant exists to prevent. Pad geometry is a mesh; pad
markings derive from the lead-in.

## 8. Refusals

Legible refusal matters more than clever fitting. Each says which, in the inspector and on
`LogAirside`:

| Case | Said |
|---|---|
| Plot smaller than one bay | "Plot too small: needs 4 m of frontage" |
| No road within reach of any edge | "Fuel depot: not on a road" (the existing wording) |
| Outline self-crossing | Refused at draw time by `WouldCross`, as today |
| Mix with no shed | Warned, not refused: "Fuel depot: no shed, no trucks" |

## 9. Tests

Per the refactor contract, every seam introduced gets a test that fails if it is unwired.

- **Solve.PlotFitBays** - a 12 m frontage yields 3 bays; 11 m yields 2; 3 m yields none with
  `TooSmall`. World-free.
- **Solve.PlotFitFacesAwayFromRoad** - bay transforms put +X away from the frontage edge.
  Pins the direction the code comment records as once having been stated backwards.
- **Entities.TrucksDerivedFromSheds** - two sheds gives `Instance.Trucks == 2`; zero sheds
  gives 0 and the warning.
- **Entities.DepotHasOneGate** - N sheds still yields exactly one `PoseNode`. Pins §2's
  ruling against a second lead-in.
- **Ops.FuelServiceUnchanged** - the existing fuel tests pass untouched, which is the claim
  that `UFuelService` did not need to change.
- Composition-level, per CLAUDE.md: spawn the actor, draw a plot, assert the pad mesh and
  the fence tiles exist - not just that the model struct is right.

Test names are leaves under distinct parents: a bare-named test vanishes from the automation
tree once a dotted child exists, and only the run count catches it.

## 10. Out of scope

- Fuel inventory, resupply, running dry (§2).
- A second row of bays; plots deeper than 8 m are yard.
- Trucks rolling out of their own shed - they pop at the gate. A named fidelity debt.
- The terminal. Its shape is not free: piers and gates must align with stands already
  placed, and it may end a hybrid - drawn footprint, solver-placed gates. Deferred
  knowingly, not overlooked.
- Tier 2/3 depots. The kit is shaped for them; nothing here builds them.

## 11. Open questions

1. Bay depth is fixed at 8 m to match the concept's site depth. Should a plot drawn deeper
   than 8 m push the module row back off the frontage, or keep it at the road and yard the
   rest? This spec assumes the latter.
2. ~~Mix at placement, or buy modules into an empty plot afterwards?~~ **Resolved: at
   placement, for the probe.** One gesture, one commit, one memento, and the probe's question
   is about drawing the plot rather than about shopping in it. Buying into a standing plot is
   the more Manor Lords answer and gives upkeep something to tick against - it is the
   expected follow-up, not this slice.
