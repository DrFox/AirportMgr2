# Organic module placement: a yard that was built, not stamped

Supersedes the module-placement half of
`2026-09-15-plot-built-buildings-design.md` (§4, §7.1) and the empty-slot drawing added by
`2026-09-15-plot-gesture.md` Task 7. The plot GESTURE is untouched by this document.

## 1. What the probe found

PIE on 2026-09-16 built a depot correctly and it still looked wrong. The user's words:

> *in Manor Lords the place that the building is built has a bit of randomness to it, the
> rotation may change, the location within the plot. It's not just a fixed grid. Tanks should
> not just be next to the shed and next to the pump.*

The bay grid was doing its job exactly as designed - `PlotFit::BuildGrid` hands back slots
and row 1 is filled left to right - and that IS the complaint. Three identical rectangles in
a row, all facing one way, reads as a placeholder for a building rather than as a yard.

This matters more than it sounds. "Buildings are drawn, not stamped" is the project's USP
pillar. A plot the player draws freely, filled by a grid that ignores what they drew, keeps
the stamp and merely hides it one level down.

## 2. Decisions taken

- **The shed fronts the gate; everything behind it is free.** The shed is functional - a
  truck drives out of it - so it stays square to the frontage and nearest the gate. Tanks,
  pumps and clutter behind it take free positions and headings. This is the Manor Lords
  reading: the house fronts the road, the backyard is a jumble.
- **Deterministic rejection sampling, not a constructive grammar.** A grammar that placed
  each module by rule (shed front, tank behind-left, pump beside it) with jitter was
  rejected: every depot would share the same bones and the jitter would read as wobble on a
  template. Sampling-and-testing is genuinely varied, its legality tests are ones this
  codebase already owns, and "how many more would fit" falls out of it for free (§6).
- **Headings are sampled in the plot's own frame, not uniformly over a circle.** A quarter
  turn plus a bounded jitter. Uniform rotation reads as debris after an explosion; a
  quarter turn plus a few degrees reads as something parked in a hurry, which is the
  feeling being bought.
- **Each module gets its own footprint.** They are all one bay today (§5 of the previous
  spec) and drawn at bay size. Scattering three identically-sized rectangles would produce
  a jumble of identical boxes - varied placement makes differing footprints legible where a
  grid hid them, so the two changes belong in one slice.
- **Per-depot determinism is a requirement, not a nicety.** See §5.
- **The empty-slot markers go.** See §6.

## 3. Where it lives

`Solve/PlotYard.h`, new, `CoreMinimal.h` and nothing else - the dependency rule every
`Solve/` header keeps, and what lets this be tested with no world, no actor and no
`NewObject`.

**It never sees `EDepotModule`.** That enum lives in `Model/RoadEntity.h`, and `Solve/` may
not reach into `Model/`. So the solver takes FOOTPRINTS and gives back STANDS; mapping a
module to its footprint is `UPlotPresenter`'s job, on the other side of the seam. This is
the same split `FitBays` already makes by taking an outline rather than an entity, and it is
what keeps the solver's tests free of the engine.

```cpp
namespace PlotYard
{
    struct FFootprint
    {
        double LengthUu = 0.0;   // along the module's own +X, which faces away from the road
        double WidthUu  = 0.0;
        /** Square to the frontage, nearest the gate. The shed, because a truck leaves it. */
        bool   bFrontsTheGate = false;
    };

    struct FStand
    {
        FVector2D Centre  = FVector2D::ZeroVector;
        double    Heading = 0.0;   // radians
        bool      bPlaced = false;
    };

    struct FYard
    {
        /** One per footprint given, IN THE ORDER GIVEN. A dropped one has bPlaced false. */
        TArray<FStand> Stands;
        /** How many more RoomForFootprint would still fit. See §6. */
        int32 RoomForMore = 0;

        /** Stands with bPlaced false. Derived, not a second count to keep in agreement. */
        int32 DroppedCount() const;
    };

    AIRSIDE_API FYard LayOut(TArrayView<const FVector2D> Outline,
        FVector2D FrontageA, FVector2D FrontageB, FVector2D Gate,
        TArrayView<const FFootprint> Footprints, int32 Seed,
        const FFootprint& RoomForFootprint);
}
```

**Stands come back in the order the footprints went in**, with a `bPlaced` flag, rather than
as a compacted list of the ones that fit. The caller knows which module it asked about only
by index, and a compacted array would silently re-associate a pump's stand with a tank.

## 4. The pass

1. **Sheds first**, square to the frontage, nearest the gate, walking outward along the
   frontage from the gate as more are added.
2. **The rest largest-footprint-first.** A packing heuristic and the reason is ordinary: a
   tank placed after four pumps have taken the middle of the plot has nowhere left to go,
   and the player loses the biggest object rather than the smallest.
3. **Each gets up to `MaxTries` candidate poses** from an `FRandomStream`: a point inside
   the outline's bounding box, and a heading of `frontage bearing + N * 90 degrees + jitter`,
   where `N` is drawn uniformly from 0-3 and the jitter from `+/- HeadingJitterRadians`.
4. **A candidate is rejected if any of:**
   - a corner falls outside the outline. Every corner, probed from `CornerInsetUu` inside -
     the test `PlotFit::FitBays` already makes, for the reason its comment gives: a
     centre-only test accepts a module hanging out of a notch and the player watches a tank
     stand on the grass.
   - it overlaps an already-placed stand, plus `ClearanceUu`. Separating-axis test on the
     two oriented rectangles; they are not axis-aligned any more, so a bounding-box test
     would refuse legal poses and accept illegal ones by turns.
   - it intrudes on the gate corridor: a lane `GateCorridorUu` wide running from the gate
     into the plot. **A depot the truck cannot leave looks perfectly correct from every
     angle** - the same failure the fence's gate gap exists to prevent, and the reason that
     gap is counted rather than eyeballed.
5. **Exhausting `MaxTries` drops that module**, with `bPlaced` false. Reported, never
   hidden: "Modules 2 of 3" is already the readout's habit and this feeds the same line.

`MaxTries` is bounded and small (24 to start). `RebuildFrom` runs on every graph change, so
an unbounded search would make laying a road stutter on an airport full of depots.

## 5. Determinism

**Seed is a hash of `FEntityInstance::Position`, quantised to whole uu.**

`UPlotPresenter::RebuildFrom` clears and rebuilds every stand on every graph change. A
`FMath::Rand` here would reroll every depot on the airport each time the player laid a road
anywhere on it - the yard would twitch, and nothing on screen would explain why.

Position rather than an entity id: `FEntityInstance` carries no id field, entity handles are
slot indices that reuse, and the position is set once at placement and never changes. Two
depots cannot share one. Quantised because a float that survives a save/load round trip one
bit different would re-roll that depot and only that depot, which is the kind of bug that
takes a day.

## 6. What it draws, and what the readout says

**The empty-slot markers are removed.** They were added the same morning (Task 7) and were
honest then: a marker was a BAY, and a bay was a thing a module would stand in. Once modules
do not stand in bays, a grid of slot markers is a claim about the plot that is no longer
true, and drawing it beside scattered modules would say the yard has a structure it does not
have.

**What replaces them is a number from the same pass.** `RoomForMore` continues the sampling
loop with a standard footprint until it fails, and the readout says "Room for 4 more". ONE
EVALUATOR: the number the player reads is produced by the code that would actually place the
thing, so it cannot drift from what they get. A separate "free area" calculation would be a
second opinion about the same question, which is the failure this codebase names most often.

The census line follows: `%d plot(s), %d module(s) standing, %d dropped, room for %d more`.

## 7. What this overturns

**§7.1 of `2026-09-15-plot-built-buildings-design.md`**, which reads:

> *Adjacent modules mate via stubs at the bay edge, at a fixed height and lateral offset
> recorded as a convention, so a tank beside a pump connects because the grammar put them
> adjacent. A procedural pipe between arbitrary modules was rejected as a second evaluator of
> a layout the bays already determine.*

That convention bought pipework for free and it was correct while the bays were real. It
cannot survive free rotation: two modules at different headings have stubs that visibly do
not meet, and the procedural pipe that would join them was rejected on the same page.

**So pipework between modules is not modelled.** A named fidelity debt, in the same class as
"trucks pop at the gate" - written down here rather than discovered later by someone
wondering where the pipes went. If it is wanted back, the honest route is a mated PAIR
placed as one rigid object, not a pipe solver.

**§4 of that spec** (bays fitted to the road-facing edge) stands as the PLOT's contract -
frontage, gate, pad - but no longer decides where modules stand.

## 8. Refusals and warnings

Nothing new refuses. A plot too small to hold the mix drops modules and says so, which is
what it already did when there were fewer bays than modules. The existing "no shed" and
"no pump" warnings are unaffected: they count modules on the instance, not stands on the
ground.

## 9. Tests

All in `AirsideTests`, all world-free - `Solve/` needs no actor, which is the point of the
`FFootprint`/`FStand` seam.

- **`Airside.Solve.PlotYardIsDeterministic`** - the same outline, footprints and seed produce
  bitwise-identical centres and headings. Bitwise, because this is what stops the yard
  twitching on rebuild, and "close enough" would not.
- **`Airside.Solve.PlotYardVariesWithSeed`** - two seeds give different layouts. Without it a
  solver that ignored the seed entirely would pass every other test here.
- **`Airside.Solve.PlotYardKeepsModulesInsideThePlot`** - every placed stand's four corners are
  inside the outline, tested on a plot with a notch so a centre-only implementation fails.
- **`Airside.Solve.PlotYardDoesNotOverlapModules`** - no two placed stands intersect, on a plot
  tight enough that a naive sampler would collide.
- **`Airside.Solve.PlotYardFrontsTheShedOnTheGate`** - the shed's heading is square to the frontage
  and it is the stand nearest the gate.
- **`Airside.Solve.PlotYardLeavesTheGateClear`** - no stand intrudes on the gate corridor.
- **`Airside.Solve.PlotYardDropsWhatWillNotFit`** - a plot too small reports `bPlaced` false rather
  than stacking modules, and `Stands` still has one entry per footprint, in order.
- **`Airside.Present.PlotPresenterScattersModules`** - composition level, per CLAUDE.md: spawn the
  actor, place two depots with identical mixes at different positions, and assert their
  stand headings are not all equal. A presenter that called the solver and then drew on a
  grid anyway would pass every `Solve/` test above.

## 10. Out of scope

- The four-click plot outline. Chosen deliberately: how modules fill a plot is independent
  of the plot's shape, this is the bigger visible change, and seeing an organic yard may
  well change how much a rectangular boundary matters. It is the expected follow-up.
- Buying modules into a standing plot. Still the next slice, unchanged.
- Authored meshes. Everything here places grey boxes; the split into one component per
  authored mesh IS the art swap and nothing in this document moves when it happens.
- Pipework, per §7.

## 11. Open questions

1. `ClearanceUu` and `GateCorridorUu` are placement feel, and feel is judged in PIE. Both
   start as named constants in `PlotYard.h` with the truck's 6.2 m as the corridor's floor;
   if they need tuning after a look, they become `UAirsideSettings` knobs rather than being
   retyped.
2. Should a dropped module refund, or should the gesture refuse a plot that cannot hold the
   mix? Today it drops and warns. Buying is not built, so there is nothing to refund and the
   question is genuinely open until it is.
