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

| Quantity | Value | From |
|---|---|---|
| `TightestFollowableRadius` | 699.4 uu | 494.538 / sin 45° |
| Hydrant S-curve: sweep, run | 44.44°, **979.3 uu** | 400 uu offset over two arcs at R |
| Right-angle corner tangent run | 699.4 uu | equals R at 90° |
| Cross-lane needs / has | 1399 / 1700 uu | two corners; y = 1100 to −600 |
| Side entry S-curve: sweep, run | 73.0°, **1337.7 uu** | 990 uu offset, y = 2090 to 1100 |
| Lane length available for it | 5650 uu | x = −3550 to +2100 |

Two arcs of radius R each sweeping θ give lateral `2R(1 − cos θ)` over longitudinal
`2R sin θ`. That is the whole of the S-curve arithmetic, and it is restated in the tests
rather than shared with production code, for the reason `FAirframe::TightestFollowableRadius`
gives at its own copy.

### The boxes are 79 uu too close, and stop being typed

`EquipmentFwd` is authored at x = −300 and must be at x ≥ −221. `EquipmentAft` is at −2100 and
must be at x ≤ −2179. One metre each. Today's spacing was fine for a ring approached square-on
and is not fine for a lane that has to S past the pit.

So `BuildCodeCStand` **derives** those two X values from the S-curve run the largest admitted
vehicle needs, and types neither. This is the move the service-road fillet made six commits
ago, for the same reason, and it follows the rule the apron geometry already lives by: size
ground geometry for the largest vehicle ADMITTED, never for the one driving now. Admit a
bigger dispenser and the boxes move; nobody has to notice.

`HydrantPit` stays where it is. A hydrant pit is plant dug into concrete under the wing root —
it is the fixed thing the paint is arranged around, not the other way about.

### The crossings clear the aircraft

Nose crossing at x ≈ +2100: forward of the nose at +507, and ≥ 699 uu ahead of `TugStand` at
1400 so the corner has its run. Tail crossing at x ≈ −3550: aft of the tail at −3250. Neither
enters the fuselage rectangle, so the existing "no route crosses the fuselage lengthwise"
assertions hold unchanged.

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

**Four entries**, because the ring's header valued something worth keeping — "a closed loop
gives entry from any side":

- **fore** and **aft**, at the nose and tail crossings. Open ground clear of the aircraft, and
  where an apron service road actually runs.
- **port** and **starboard**, out at y = ±2090 — clear of the wingtip at 1790, which is where
  the ring itself ran. Each S-curves into its long lane over 1337.7 uu, against 5650 uu of
  lane to spend it on.

A side entry starts outboard of the wingtip because a road joining UNDER a wing would be
wrong; the S is what brings it in. An entry no road reaches is an unvisited node — nothing
routes into it, because nothing is there to reach, so it is not a dead end in the sense
decision 1 cares about.

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
