# The stand layout is a verified template, placed — piece C

> Piece C of `2026-09-16-stand-servicing-rethink-design.md`. A and B are built:
> the drivability oracle (`FSpeedProfile`'s verdict is readable) and the reverse-into-bay
> manoeuvre (`FReverseRun`, which refuses a curve it cannot hold).

## The property this whole design exists to get

**Drivability is a property of the TEMPLATE, verified once, for every vehicle — not a property
of every placement.**

Four attempts failed because each one derived geometry per stand and therefore had to re-prove
it drivable per stand. Here the inside of a stand is authored once, checked offline by the
oracle and by `FReverseRun::Start`, and **nothing recomputes it later**. Placement is a
transform and a yes/no. A transform preserves curvature, so it cannot make a verified layout
undrivable.

Everything below is in service of keeping that property true. Where a decision could have been
made either way, the tie-breaker was "does this reintroduce per-placement geometry".

## Stand sizes belong in `IcaoCode`

`IcaoCode` already holds the Annex 14 code letters as ONE table, and its header records that
three call sites once typed those figures separately and silently disagreed. Stand footprints
join it rather than starting a second table.

| Letter | Span band | Width | Depth | Typical |
|---|---|---|---|---|
| A | < 15 m | 21 m | 20 m | Cessna 172, PA-28 |
| B | < 24 m | 30 m | 30 m | King Air, small business jets |
| C | < 36 m | 45 m | 55 m | Q400, ATR 72, A320, B737 |
| D | < 52 m | 67 m | 70 m | B757, B767, A310 |
| E | < 65 m | 80 m | 90 m | B777, B787, A330/A350, B747-400 |
| F | < 80 m | 95 m | 100 m | A380, B747-8 |

**WIDTH IS DERIVED, NEVER TYPED.** Every figure above is exactly the letter's existing span band
plus twice its ICAO wingtip clearance — 3 m for A and B, 4.5 m for C, 7.5 m for D, E and F. All
six match to the centimetre, which is a check on the table rather than a coincidence. So the
new data is a **clearance row**, and width is computed from figures already present.

**DEPTH IS AUTHORED**, because it follows aircraft length and the room a GSE road and an
equipment area need, and no such clean rule produces it. Its provenance is stated at the table
the way `IcaoCode` already states its own: standard aerodrome design values, not figures lifted
from one Annex 14 edition, and the first thing to check if a real layout looks wrong.

### Admission runs backwards, and that is the game mechanic

A stand's SIZE decides which airframes may use it, not the reverse. `IcaoCode::LetterForWingspan`
already exists; this is its mirror. A player who drags a bigger stand gets bigger aircraft as a
consequence rather than as a setting.

### Width is a BAND, and the template is built for its floor

45 m to just under 67 m is all Code C. **The template is authored at 45 m** — the tightest legal
stand of its letter — for the same reason every other figure in this project is taken from the
worst case admitted. A template authored at a comfortable 55 m would fail precisely where a
player drew the smallest stand the rules allow.

**Extra width is absorbed as STRAIGHT, never as a reshaped curve.** Things near the aeroplane
cannot move — a hydrant pit sits under the refuel panel, a loader must reach the hold door — so
those bays are fixed relative to the aircraft. What moves outward is the staging rank and the
GSE road behind it, which makes the legs between them LONGER IN THE MIDDLE and leaves every arc
with its radius and both its tangents unchanged.

That is not a cosmetic choice. Curvature is what the oracle checks, and a straight insertion
preserves it, so a 60 m stand is covered by the same verification as the 45 m one. Widen by
reshaping arcs instead and every width becomes a fresh chance to be undrivable — the exact class
of bug this piece exists to remove. Depth is treated identically.

## The template

Authored poses, derived legs, both in aircraft-relative coordinates, on `UEntityDefinition`
where `ServiceLane` is now.

```cpp
USTRUCT() struct FServiceBay
{
    FName AnchorId;          // which service this bay serves
    FVector2D Local;         // where the vehicle ends up
    double LocalHeading;     // pointing which way, parked
    FRoutePlan ApproachLeg;  // staging -> past the bay, FORWARDS
    FRoutePlan ReverseLeg;   // the back-in, which FReverseRun plays
    FRoutePlan ExitLeg;      // bay -> onward, FORWARDS
};

UPROPERTY() FVector2D EntryLocal;        // must meet a road; see below
UPROPERTY() double EntryHeading = 0.0;   // DIRECTED
UPROPERTY() FRoutePlan EntryLeg;         // entry -> staging, forwards

UPROPERTY() FVector2D StagingLocal;
UPROPERTY() double StagingHeading = 0.0;
UPROPERTY() int32 StagingCapacity = 0;   // a rank, not a point

UPROPERTY() TArray<FServiceBay> ServiceBays;
UPROPERTY() FVector2D RequiredExtent;    // derived from the above, not typed
```

**A human places the poses; the machine derives the legs and proves them.** The poses are
decisions a person can look at and judge — is that where a hydrant goes, is that where a loader
waits. The legs are geometry, derived at template-build time and checked: forward legs against
the oracle, reverse legs through `FReverseRun::Start`, which already refuses what it cannot
hold. **A template whose legs do not check fails at build time, in a test.**

### Three legs per bay, not two

A bay is a dead end. The vehicle backs in, and then drives out FORWARDS — and if it retraced the
reverse leg, that curve would have to satisfy the forward limit of 699 uu, throwing away the
whole 30% advantage reversing buys. It does not retrace it: from the parked pose it leaves along
its own curve, checked at the forward limit.

So the reverse limit does the job it is good for — reaching a POSE that forward driving cannot
reach in the space available, which is the parallel-parking argument — rather than being spent
on a path that has to work both ways.

### Bays are airframe-independent

`BuildCodeCStand`'s own comment already settles this: a stand carries "plant dug into the
concrete and boxes painted on it", and where a service connects to the AIRCRAFT lives on
`UAircraftType` "because an A320 and a 737-800 both park here and their doors are metres apart".
Paint does not move when a different type parks.

What DOES depend on the airframe is the extent, and there is a live defect in it today: the
geometry is sized from the A320's tail at -3250, but `DA_Aircraft_B738` is authored at -3430 and
already parks on the same stand. The tail clearance is 1.2 m where it was meant to be 3.

**So the extent resolves the LARGEST airframe the letter admits**, never a named one — the rule
the taxiway widths and the service road fillet already follow. Admitting a bigger type moves the
extent instead of silently eating its clearance.

## One entry, fixed, and it must meet a road

The stand DECLARES where it may be entered. Placement VALIDATES that a road meets it; it does
not solve a curve to wherever a road happens to be.

**This removes the last per-placement geometry in the design.** With the entry floating, one
curve was still solved fresh on every placement, and that is one remaining chance to produce
something undrivable. With it fixed, placement computes no geometry at all.

It also gives the player a rule they already know from every city builder — a thing needs road
frontage — and a refusal they can act on: "this stand's entry must meet a road."

**ONE ENTRY SERVES BOTH WAYS.** The vehicle enters forwards and leaves forwards through the same
point, travelling opposite directions along the same curve. A radius is a radius, so a curve
drivable one way is drivable the other. A second entry is a small addition if a one-way GSE road
is ever wanted; it is not needed now.

**THE ENTRY IS DIRECTED.** It reaches the graph as an edge that cannot be traversed the wrong
way, so the search has no wrong choice to make. This is the fix for the defect the oracle found
on its first run: the route reversed 175 degrees on the spot at a stand entry because
`RouteSearch::EdgeCost` is length plus congestion with no heading term, so the NEARER entry won
regardless of which way its merge faced. Encoding it in the graph makes that unrepresentable
rather than merely detectable, and costs the search nothing.

## Placement, and refusing

A stand that exists is always serviceable. When the layout will not fit, **the placement is
refused and the tool says which constraint failed** — "needs 45 m of width; this apron gives 41"
or "the entry must meet a road". Naming the lever, not just the failure.

Nothing downstream ever has to handle a half-working stand: no flight board, fuel service or job
board needs a partially-serviceable case, and "why is nothing refuelling here" never becomes a
question the player has to go hunting for.

## Occupancy: why vehicles do not pile up

A bay is claimed before a vehicle leaves the staging rank. Occupied, and it waits AT staging
rather than setting off — which is why staging is a rank with an authored capacity and not a
point. A single pose would queue arriving trucks on the road outside, blocking it.

This reuses the existing claim machinery rather than inventing a second notion of "something is
in the way". If a bay turns out not to fit that machinery cleanly, that is a finding to bring
back, not to force.

## Tests

1. **Every leg of the template is drivable, for every ground vehicle** — forward legs against the
   oracle, reverse legs through `FReverseRun::Start`. Enumerated from the fleet resolvers, as
   piece B's test is, so a new vehicle widens it without anyone remembering.
2. **The template fits its letter's minimum.** Authored at 45 m; assert `RequiredExtent` is
   within it, so a layout that quietly grew is caught at build time.
3. **Extra width inserts straight only.** Build at 45 m and at 60 m; assert every arc's radius is
   unchanged and only straight length differs. This is the assertion that protects the
   verification property.
4. **Width is derived.** Change a clearance figure and assert the widths move; a test asserting
   45 would pass against a typed 45.
5. **A stand whose entry meets no road is refused**, with the reason naming the entry.
6. **The directed entry cannot be traversed the wrong way** — the search returns no route rather
   than a reversing one.
7. **The acceptance test goes green**: `Airside.Model.Traffic.TruckDrivesTheWholeRouteToTheHydrant`,
   red since piece A, is what says this worked.

## What this deletes

`ServiceLane`, `FStandWaypoint`, `EStandWaypointKind`, and `FStandLaneBuild`'s corner-rounding
and clamping — the lane's whole apparatus. `FStandLayoutBuild` replaces it and is smaller,
because it lays authored curves rather than solving them.

The 16 commits of the lane design on `feature/lane-entrances` are superseded here. Their
measurements remain true and are worth keeping: the quadratic corner cost, the 2R floor for a
180 degree turn, and the delivered-versus-requested distinction.

## Unresolved questions

1. **Does a Code C template serve a Q400 and an A320 equally?** Both are Code C, but a Q400 is
   28 m long against an A320's 37.6, and its doors are elsewhere. The bays are fixed paint, so
   either the stand is laid out for the largest the letter admits and a Q400 parks in a big
   stand, or a letter needs more than one template. Sized for the largest here; flagged because
   it may read oddly on screen.
2. **Does the staging rank's capacity scale with the letter**, or is it one figure? A Code F
   stand plausibly services more vehicles at once than a Code B.
3. **Does depth get the same band treatment as width** — minimum authored, extra absorbed as
   straight fore-aft? Written that way; confirm.
