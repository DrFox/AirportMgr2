# The stand routes like an apron, not like a ring

> Part 2 of the work begun in `2026-09-15-vehicle-model-truth-design.md`. Part 1 made the
> vehicle model honest; this makes the ground under it drivable.

## The problem, in one line

A real 8.5 m fuel dispenser needs a 699 uu turn radius, and **no corner of a Code C stand's
service ring is takeable at any speed** — so the anchors it exists to reach are unreachable.

## The measurement that settles it

Not a judgement about elegance. The ring fails arithmetically and cannot be rescued by any
value of any parameter.

A rigid vehicle cannot follow an arc tighter than `Wheelbase / sin(lock)` at ANY speed. For
`ResolveLargestServiceVehicle()` after part 1 that is `494.538 / sin 45°` = **699.4 uu**.

On a Code C stand the ring runs outboard of the wingtips at y = ±2090. The three starboard
anchors sit at y = 700 (`HydrantPit`, under the wing root) and y = 1100 (`EquipmentFwd`,
`EquipmentAft`). That leaves **990 uu of depth** between the ring and the anchor row.

A 90° turn at R = 699.4 needs a tangent run of R on each arm — 1399 uu, against 990 of
depth. **Short by 409.**

It did not fit at the old 471 uu lock either, and the shortfall there was measured along a
different axis: `ServiceLinkTest.cpp:701` records that three spurs sharing the north side
needed 5328 uu of spacing between them and had 5250 — short by 78. Two independent ways for
the same shape to be too small, which is why neither was fixable by moving one number.

**Widening the ring makes it worse**, which is the tell. Push it outboard and the depth the
turn must happen in grows, but so does the distance, and the anchors do not move. There is no
radius, no spacing and no lane width that solves it. The shape is wrong.

## The shape that is right

**The lane comes inside the wingtip and runs along the row the boxes are painted on.** Then
nothing turns into an anchor at all — a truck drives up the row and stops.

```
              tail crossing                          nose crossing
                   |                                       |
   y=+1100  -------+---- EquipmentAft --.      .-- EquipmentFwd --------+-------  starboard
                   |                     `- HydrantPit -'               |         (under the wing)
   y=  -600  ------+---------------- FixedGPU -------- TugStand --------+-------  port
                x≈-3550                                             x≈+2100
```

Four of the five anchors lie ON a lane. `EquipmentFwd` and `EquipmentAft` — authored at
(−300, 1100) and (−2100, 1100), and moved one metre each by the derivation below — are already
a row; `FixedGPU` (300, −600) and `TugStand` (1400, −600) are
another. Only `HydrantPit` is off-lane, by 400 uu, and it is reached by a **bulge in the lane,
not a stub**: the lane S-curves inboard to the pit and back out.

### One cycle, and why that is not negotiable

Stage 2 is forward-only — reverse kinematics is stage 3. **A dead end is a reverse.** So the
lanes form a single cycle: starboard lane, nose crossing, port lane, tail crossing. Nothing is
a stub, including the hydrant.

This is a ring again in the trivial topological sense, and that is not what changed. What
changed is that the long sides pass THROUGH the anchors instead of outboard of everything.

## The arithmetic, all derived

**A CORNER HERE IS A QUADRATIC, NOT A CIRCULAR FILLET**, and the first draft of this spec
costed every corner as if it were an arc. It is the same mistake `8be494c` made one level up,
in the same week, about the same kind of curve.

`FServiceLoopBuild` lays a quadratic whose control is the corner and whose ends are `Run` back
along each leg. Its delivered radius is `Run · sin²(θ/2) / cos(θ/2)` for an interior angle θ,
which inverts to exactly the `CornerRunFor` already in the file:

```
CornerRunFor(R, θ) = R · cos(θ/2) / sin²(θ/2)
```

At a right angle that is **1.414 R**, not R. Every figure below is computed from it.

| Quantity | Value | From |
|---|---|---|
| `TightestFollowableRadius` | 699.4 uu | 494.538 / sin 45° |
| Right-angle corner run | **989.1 uu** | `CornerRunFor(R, 90°)` = 1.414 R |
| Hydrant dip, half-extent | **1018.4 uu** | flat-bottomed, legs at 40.5°, `2·Run + 400/tan α` |
| Crossing as a square corner | 1978.2 needed / 1700 available | **does not fit** — see below |
| Crossing corner, 4 × 45° | 313.6 uu run each | a U-turn is 180°; two corners would be 90° each |
| Floor for ANY 180° turn | 2R = 1398.8 uu lateral | the semicircle; 1700 available |

The corner arithmetic is restated in the tests rather than shared with production code, for the
reason `FAirframe::TightestFollowableRadius` gives at its own copy.

### The boxes are 118 uu too close, and stop being typed

The hydrant dip is a flat-bottomed trapezoid, not a pure two-arc S: the pit sits on a flat
whose half-length is one corner's run, with a leg rising at 40.5° to the box row and a corner
at each end. Half-extent `2·Run + 400/tan α` = **1018.4 uu**.

`EquipmentFwd` is authored at x = −300 and must be at x ≥ −182. `EquipmentAft` is at −2100 and
must be at x ≤ −2218. About 1.2 m each. Today's spacing was fine for a ring approached
square-on and is not fine for a lane that has to dip past the pit.

The flat bottom is not a detail. A pure V at the pit would be a corner no vehicle can take,
and under the rolling-steer law from part 1 — where `MaxStep` is proportional to speed — an
agent that stopped there could not turn at all, so it would not crawl through, it would be
stuck. The flat is what makes the pit a place a truck drives THROUGH.

So `BuildCodeCStand` **derives** those two X values from the S-curve run the largest admitted
vehicle needs, and types neither. This is the move the service-road fillet made six commits
ago, for the same reason, and it follows the rule the apron geometry already lives by: size
ground geometry for the largest vehicle ADMITTED, never for the one driving now. Admit a
bigger dispenser and the boxes move; nobody has to notice.

`HydrantPit` stays where it is. A hydrant pit is plant dug into concrete under the wing root —
it is the fixed thing the paint is arranged around, not the other way about.

### A crossing is a U-TURN, and needs four corners, not two

The two runs are **anti-parallel** — starboard goes forward, port goes aft — so a crossing
between them is a **180° reversal**, however it is cut up. That single observation governs
everything here, and two drafts of this spec missed it:

- Two corners joining them are therefore **90° each**, because 2 × 90 = 180. There is no
  diagonal that makes them shallower. The second draft's "legs no steeper than 73.55°" solved
  a lateral-OFFSET problem — the hydrant dip's shape — and silently applied the answer to a
  reversal, which is a different problem with a different constraint.
- Two 90° corners cost 2 × 989.1 = 1978.2 uu of lateral run against the 1700 between
  y = 1100 and y = −600. **Short by 278**, exactly as the second draft said; its proposed fix
  simply did not fix it.

**Four corners of 45° each** do fit: `CornerRunFor(R, 135°)` = 313.6 uu, so the worst shared
straight carries 627.1 uu. An octagon's corner, twice.

The floor underneath all of it is the semicircle: **any** 180° turn needs at least 2R =
1398.8 uu of lateral room, and the crossing is possible at all only because 1700 > 1398.8.
That figure, not a leg angle, is what a second stand type has to clear.

The builder derives the crossing from the clearance it must keep and the run its corners need;
the stand's fore-and-aft extent falls out of it rather than being typed. At the A320 the TAIL
clamp binds — x = −3550 against the −2618 the anchors alone would allow — because a crossing
level with the 12 m tailplane would have run under it.

Both crossings stay clear of the fuselage rectangle — forward of the nose at +507, aft of the
tail at −3250 — so the existing "no route crosses the fuselage lengthwise" assertions hold
unchanged.

## Six decisions, taken 2026-09-16

1. **Forward-only first.** Reverse kinematics and the dock-start pose are stage 3.
2. **The anchor's heading is binding**, and approaches run along the aircraft.
3. **The fuselage becomes a rectangle.** `FEntityFootprint` gains `FuselageWidth`; wings and
   tailplane stay passable. Routing moved INSIDE the stand, where a zero-width centreline
   permits a route down the aircraft's skin. Under a wing is normal; through the aeroplane is
   not — the rule is unchanged, only its width.
4. **Computed at authoring, per definition**, as `ServiceLoop` was, and for the reason
   `ServiceLoop`'s own header gives: deriving at rebuild time is a runtime algorithm's opinion
   with no override, and a second evaluator of the same geometry. Sized from `DesignAircraft`,
   so a smaller aircraft on the stand gets more clearance than it needs and never less.
5. **Entries are authored poses.** A stand declares its entrances instead of the link pass
   discovering them.
6. **The starboard anchor headings are re-authored along the aircraft.** They faced −90°
   because the old spur arrived square-on from outboard. A truck driving up the row finishes
   pointing along X, so the boxes are painted along X.

Decision 6 is slightly wrong for baggage specifically — a belt loader really does square up to
a hold door — and is accepted for stage 2 with the reason recorded at the anchor. Stage 3's
reverse leg is what makes it right.

## What changes

### Deleted

- `UEntityDefinition::ServiceLoop` and `ServiceLaneBounds()`, and the rectangle's derivation
  in `BuildCodeCStand`.
- `FServiceLoopBuild::WalkRing` — a ring-only walk, meaningless once lanes are open.
- The per-side discovery machinery in `FAnchorLink`: `IsLaneBend`, `WholeSide`, the swept
  `NearestRoadApproach`, and the `Qualifies` heuristic at `AnchorLink.cpp:418` —
  `max(NearestSide + CornerReach, NearestSide * 2, NearestSide + LaneWidth)`, three thresholds
  tuned against one another, whose own comment records that the corner-reach term had to be
  added after the ring's corners were rounded or the cul-de-sac came straight back. All of it
  existed only because the ring had no declared entrances.

### Kept, renamed

- `FGuidelineEdge::ServiceLoopOwner` → `StandGeometryOwner`. **Load-bearing, not cosmetic.**
  `URoadNetwork::IsServiceNodeConnected` walks exactly this mark to answer "does this hydrant
  reach a road", and `FuelService.cpp:131` and `:472` are its callers. Delete the mark and
  every stand in an empty field reads as connected.
- `FGuidelineEdge::bServiceSpur` → `bStandApproach`. Same job: telling a lane from an approach
  so the link search never joins one to the other.
- `FServiceLoopBuild` → `FStandLaneBuild`, keeping `CornerRunFor` and `TangentRunFor`. Those
  two were right. The rectangle they were applied to was wrong.

### New, on `UEntityDefinition`

```cpp
enum class EStandWaypointKind : uint8 { Plain, Anchor, Entry };

struct FStandWaypoint
{
    FVector2D Local;          // entity-local, as ServiceLoop's points were
    FName AnchorId;           // set iff Kind == Anchor
    EStandWaypointKind Kind;
};

struct FStandLane { TArray<FStandWaypoint> Waypoints; };

/** Open polylines. Where two share a position they join; no lane repeats a point. */
UPROPERTY(EditAnywhere) TArray<FStandLane> ServiceLanes;
```

And one field on `FEntityFootprint`, per decision 3:

```cpp
/** Side to side, so the fuselage is a box and not an axis. Zero keeps the old line. */
UPROPERTY(EditAnywhere) double FuselageWidth = 0.0;
```

**No heading on a waypoint.** An entry's heading is its lane's own direction at that end; an
anchor's is already `FEntityAnchor::LocalHeading`. A second copy is a value that must agree
with another value in the same asset, which is the drift `FResolvedAnchor` exists to remove.

## The road handover

**Four entries, and they are the four corners of the two crossings** — nose-starboard,
nose-port, tail-starboard, tail-port. All four sit ON the cycle, so the lane stays one closed
polyline and every lane node is still driven through.

The first draft put two of them out at y = ±2090, abeam the aircraft, inheriting "a closed loop
gives entry from any side" from the ring. That was the ring's property and not a requirement.
**A stand sits in a row, with neighbouring stands abeam it**; the service road runs along the
back of the row, which is aft of each stand, or in front of it. Nobody runs a road between two
parked aeroplanes' wingtips. Abeam entries would also have had to S-curve 990 uu inward under
the wing, as stubs off the cycle — which reintroduces the dead ends decision 1 exists to
forbid.

Two corners per crossing rather than one, because a road may approach either lane's end: the
search takes whichever entry is nearer what it is going to.

An entry no road reaches is an ordinary lane corner — it is on the cycle, so there is nothing
to strand.

The link becomes `ELinkKind::Ray` along the entry's own heading, as the aircraft pose link
already is, rather than `ELinkKind::Proximity`. A truck then turns off the road ALONG the
stand — the same property the tangential join was buying by sliding along a ring, and
`TangentRunFor` still does that work at the road end.

### Unreachable-stand UI

Falls out with no new mechanism. `StandGeometryOwner` is on every lane and approach edge, the
entry-to-road link still deliberately carries none, so `IsServiceNodeConnected` answers exactly
as it does today and `FuelService.cpp:131` needs no change. What is new is that the answer is
reportable per ENTRY, so the message becomes "no road within reach of any of this stand's four
entrances" rather than a bare false.

## Tests

World-free in `Solve/` or `Build/` unless noted. Each leaf named distinctly — UE's automation
tree drops a bare-named parent once a dotted child exists, and only the run count catches it.

1. **Drivability, measured.** Every corner and every S in the PLACED, SAMPLED lane clears
   `ResolveLargestServiceVehicle().TightestFollowableRadius()`, via
   `GuidelineGeom::TightestRadius`. On the curve laid, never the fillet that shaped it. That
   distinction cost three sessions in `8be494c` and becomes an assertion here.
2. **No dead end.** Every lane node except an entry carries at least two live lane edges.
   Forward-only rests entirely on this, so it is pinned rather than reasoned about.
3. **The spacing is derived.** Give the builder a longer-wheelbase vehicle and assert the two
   boxes MOVED. A test asserting −221 would pin the arithmetic's output and would pass against
   a frozen constant.
4. **Nothing crosses the fuselage**, upgraded from the centreline segment to the inflated
   rectangle — and still permitting a route under a wing, which `HydrantPit` requires.
5. **Every entry links.** A road beside each of the four in turn joins that entry and only
   that entry; a stand alone in a field answers `IsServiceNodeConnected` false. This is
   `ServiceLaneEntersOnEverySideWithinReach` rewritten around entries, and rewriting it is what
   turns the pre-existing red test from `563fa44` green.
6. **End to end**, replacing `SpursLeaveTheLaneTangentially`: a route from the depot to
   `HydrantPit` has no vertex tighter than the lock and produces no `Route asks for R=`
   warning.

## Build and verification

New `USTRUCT`s and a new `UPROPERTY` on `UEntityDefinition` need a full `Build.bat` with the
editor closed, and `DA_Stand_CodeC` must be re-authored by `Tools/Python/build_stand_asset.py`,
which needs it closed too. One close, batched.

The `ServiceLoopOwner` rename needs no migration: those edges are derived and swept on every
rebuild, which is the property `FServiceLoopBuild`'s own header claims for them.

```
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Run-AirsideTests.ps1
```

Read the `N test(s) run, N failed, N crashed` line, never the exit code.

**Then look at the map, and this is not optional.** A green suite is not evidence for a
geometry change: a graph change that passed 348 tests once put kilometre-wide arcs across the
apron. In PIE, with `Saved/Logs/AirportMgr.log` open:

1. Send a fuel truck from the depot to a stand. Quote its `Speed profile:` line and confirm
   the tightest-radius rule reads `lateral accel`, not `TIGHTER THAN THE STEERING LOCK`.
2. Confirm no `Route asks for R=...` warnings for that route.
3. `python Tools/Mcp.py shot out.png` with the truck parked, and confirm it sits in its box
   along the aircraft rather than across it.

## Out of scope — stage 3

Reverse kinematics in `FRouteFollower` (fixed axle on the line, steering inverted —
`FPushbackRun` is NOT reusable, it works precisely because the tug couples at the nose gear so
the STEERED axle still leads); the dock-start pose; squaring a belt loader up to a hold door;
a second stand type; and what a truck does once parked.

## Unresolved questions

1. `FuselageWidth` for the A320 — 400 uu (the 3.95 m fuselage) plus what clearance?
2. Do the port anchors keep their headings? `FixedGPU` at 90° and `TugStand` at 180° are
   already roughly along the aircraft, unlike the starboard three.
3. Should an entry with no road in reach be drawn in the G overlay, or is the
   `IsServiceNodeConnected` message enough?
