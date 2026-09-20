# Plot layout strategies: a yard somebody could work in

How a plot decides where its modules stand, now that "wherever they fit" has been seen and
does not work.

Amends `2026-09-20-plot-module-kits-design.md` §3.2 and §5, and overturns part of
`2026-09-16-organic-module-placement-design.md` §2. Everything else in the kits design -
`UPlotModuleKit`, ghosted slots, the per-kit readout, the presenter split - stands unchanged.

---

## 1. What the probe found

PIE on 2026-09-20, a 45 m x ~35 m plot drawn by hand. (The screenshot is `samples/1.png` in
the author's checkout; `samples/` is gitignored, so the numbers are transcribed here rather
than cited.) The readout said:

```
Sheds  23
Tanks   9
Pumps  12
```

Forty-four buildings, jittered, wall to wall. Arithmetic on the drawn plot:

| | |
|---|---|
| 23 sheds x 32 m² | 736 m² |
| 9 tanks x 25 m² | 225 m² |
| 12 pumps x 6 m² | 72 m² |
| plot | ~1,575 m² |
| **coverage** | **65%** |

The user's words: *"It is not a practical fuel yard."*

**THIS WAS RAISED BEFORE IT WAS BUILT** and answered wrongly. During the kits design:

> *a player cannot just fill all space within the plot with kits, the layout resolver needs
> to give space etc.*

The answer given was that `ClearanceUu`, `GateCorridorUu` and the heading jitter meant
"you can't fill 100% of the plot" stopped being a correction factor anyone had to guess at.
A 1 m skirt and one truck-sized box at the gate is not giving a yard space. The concern was
correct and the reply was not.

Three faults, only one of which is a bug:

1. **`Reserve` fills until nothing more fits** - kits design §3.2, literally "fill the plot
   with as many modules as actually fit". Capacity became a packing result rather than a
   decision. This is the bug.
2. **Nothing guarantees circulation.** The only clear ground is the gate corridor, so at 65%
   coverage no truck reaches the middle of the yard.
3. **The counts are not gameplay.** Twelve pumps serving nine tanks is not a fuel depot at
   any tier. Scaling the concept sheet by FRONTAGE - 45 m against its 11 m - suggests four
   sheds, not twenty-three. Scaling by AREA suggests eighteen, which is the fault in one
   line: depth must not multiply capacity.

## 2. Decisions taken

- **Placement is a swappable STRATEGY, chosen per plot type.** Different buildings want
  different yards, and the first fuel-yard layout is explicitly scaffolding.
- **A strategy returns stands. Capacity is read off what it returned.** There is no global
  capacity rule and this document does not state one - see §3, which is the whole point.
- **The first fuel strategy is a band arrangement, and is labelled scaffolding in code.**
  Sheds across the back facing the gate, tanks down the left, pumps down the right. It
  wastes ground on purpose.
- **A kit states its APRON separately from its footprint.** The footprint is the object; the
  apron is the working room it needs. One rectangle is drawn, a larger one is reserved.
- **The scatter survives as a strategy.** It is built, it is tested, and it is the right
  answer for a yard that is meant to look unplanned. It stops being the only answer.

## 3. What this document deliberately does NOT decide

**There is no capacity model here.** An earlier draft of this section derived one from the
band layout - shed capacity is back-width over shed-width - and that was wrong for a reason
worth recording:

> *we should not be building capacity calculations of that as a rule, plus other strategies
> will be different*

Band-width arithmetic is an implementation detail of one placeholder. Promoting it to a rule
would bury a throwaway decision somewhere a later strategy has to dig it out of.
`FReservation::CeilingFor` already counts what is there and needs no change.

**Monotonicity is a per-strategy property, not a design guarantee.** The kits design §11
recorded that reservation is not monotonic in plot size; a sweep of 845 plot sizes then found
336 regressions, worst drop 11 bays, so §11's "it may never bite" is false for the scatter.
The band strategy happens to be monotonic. That is a fact about bands, not a promise of the
seam, and each strategy states its own behaviour.

**Circulation is not solved.** The band layout leaves the middle of the plot empty, which is
where a truck would drive, but nothing tests that a route exists. A strategy that guarantees
reachability is its own piece of work.

## 4. The seam

```cpp
/** Where a plot is and how big. Plain data, no UObject, no EDepotModule. */
struct FPlotSite
{
    TArrayView<const FVector2D> Outline;
    FVector2D FrontageA, FrontageB, Gate;
    int32 Seed = 0;
};

UCLASS(Abstract)
class AIRSIDE_API UPlotLayoutStrategy : public UObject
{
    GENERATED_BODY()
public:
    virtual PlotYard::FReservation Solve(
        const FPlotSite& Site, TArrayView<const PlotYard::FKitSpec> Kits) const
        PURE_VIRTUAL(UPlotLayoutStrategy::Solve, return PlotYard::FReservation(););
};
```

**IN `Build/`, NOT `Solve/`.** `Solve/` takes `CoreMinimal.h` and nothing else and is free of
UObjects, which is what keeps its tests world-free; `Check-Architecture` enforces it. So the
strategies live in `Build/` - the layer that already serves both `Present/` and `Tool/` - and
`Solve/` keeps the geometry they call.

`UScatterLayoutStrategy` wraps today's `PlotYard::Reserve` unchanged, so nothing in `Solve/`
moves and the seven `PlotReserve` tests keep testing the thing they were written for.

**Chosen on `UEntityDefinition`**, resolved presenter-side and tool-side exactly as
`DepotKitSpecs` already is. `Build/` may include `Entities/`; the lint forbids it only
`Present/` and `Tool/`.

## 5. The apron

`UPlotModuleKit` gains one field:

```cpp
    /**
     * Clear ground the module needs BEYOND its own footprint, uu. X is in front - towards
     * the gate - and Y is to either side.
     *
     * SEPARATE FROM Footprint, which stays the OBJECT: what the mesh is and what gets drawn.
     * Inflating a shed to 4 x 10 m to buy its apron would draw a ten-metre shed today and
     * disagree with a six-metre mesh tomorrow, which is exactly what the footprint-versus-
     * bounds test exists to catch.
     */
    UPROPERTY(EditAnywhere, Category = "Kit") FVector2D ApronUu = FVector2D::ZeroVector;
```

Shed: 4 m of apron in front - a truck's length out of the door. Tank and pump: none beyond
the clearance every module already gets.

A strategy reserves footprint-plus-apron and the presenter draws the footprint, flush to the
back of the reserved ground. `PlotYard::FKitSpec` carries the apron so `Solve/` still never
sees a kit.

## 6. `UFuelYardBandsStrategy`

Scaffolding. The comment in the file says so, in those words.

```
        back fence
  [shed][shed][shed][shed]
  ....... aprons .........

  (tank)                    [pump]
  (tank)     yard           [pump]
  (tank)   (empty)

              gate
```

- **Sheds** fill the back edge, square to the frontage, opening towards the gate. Their
  aprons extend forward and nothing else may stand there.
- **Tanks** run down the left edge, **pumps** down the right, both clear of the shed aprons.
- **The middle is left empty.** Not as circulation - nothing checks that - but because it is
  what a real yard has and what the scatter never left.
- **No jitter.** Every fuel depot will look like every other fuel depot; see §7.

Capacity is however many of each fitted. No formula is stated, here or anywhere.

## 7. What this overturns

**§2 of `2026-09-16-organic-module-placement-design.md`:**

> *Deterministic rejection sampling, not a constructive grammar. A grammar that placed each
> module by rule (shed front, tank behind-left, pump beside it) with jitter was rejected:
> every depot would share the same bones and the jitter would read as wobble on a template.*

**That objection is still true, and is accepted rather than answered.** Every fuel depot
built by `UFuelYardBandsStrategy` will share its bones. What has changed is the evidence:
the sampler produced a yard nobody could work in, and a usable yard that repeats beats a
varied one that does not. The scatter is not deleted - it is one strategy among others, and
a plot type that wants an unplanned look can still have it.

**§3.2 and §5 of `2026-09-20-plot-module-kits-design.md`**, which said the solver fills the
plot with as many modules as fit. It no longer does; a strategy decides.

**Nothing here changes §3.3** (a kit's ceiling is fixed when the plot is drawn), **§3.4**
(reservations are recomputed, not saved), **§4** (`UPlotModuleKit`), or **§6** (what the
presenter draws).

## 8. Testing

**The seam:**
- Every `EPlaceableEntity` a plot can be drawn as resolves to a strategy - walked, not listed.
- `UScatterLayoutStrategy` returns exactly what `PlotYard::Reserve` returns for the same site.

**The band strategy:**
- No stand overlaps another, via `StandCorners`, as the scatter is already tested.
- No stand sits inside another's apron.
- Every shed touches the back edge and faces the gate; heading is exactly the inward bearing.
- Coverage on a 45 x 35 m plot is under 30%, against the 65% that prompted this document.
- Counts are sane at the concept sheet's own size: an 11 x 8 m plot holds about one of each.
- Monotonic: growing a plot never reduces any kit's ceiling. Stated for THIS strategy only.

**Unchanged and must stay green:** all seven `PlotReserve` tests, the presenter's ghost test,
and the tool's readout tests.

## 9. Out of scope

- Circulation and reachability. §3 says why.
- Any capacity rule. §3 says why.
- Strategies for buildings other than the fuel depot.
- Retiring the scatter. It stays.
- Meshes, the fence, dressing. Still Plans B and C.
