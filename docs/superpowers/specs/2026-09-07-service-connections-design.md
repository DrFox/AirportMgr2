# Service Connections and the Stand Service Loop — Design

**Status:** design, agreed in conversation 2026-09-07 after PR #67 (the fuel service slice)
was merged and PIE-verified. Sits between the fuel slice and M3.

**Parents:** `2026-09-07-fuel-service-slice-design.md` (the slice this fixes);
`2026-09-06-ground-traffic-design.md` (classes, claims, the resolver);
`2026-08-29-ground-movement-model-design.md` (the guideline graph).

## 0. Nothing here is law

As the systems map §0. Change it for a reason, after discussion, and record the reasoning
where the old decision was.

## 1. Why

The fuel slice works and was watched working. Getting it to work took twenty minutes of
fighting the ANCHOR GEOMETRY, and the reason is a rule that is right for aircraft and wrong
for everything else.

`FAnchorLink` casts ONE RAY per lead-in, along the anchor's own world heading, and joins the
first guideline it strikes within 200 m. For a stand's aircraft lead-in that is exactly
right: the ray IS the painted line, and the header records why "nearest guideline" was
rejected — nearest is regularly the taxiway on the far side of the terminal, and the link
would run through the building with nothing to report it.

For a SERVICE connection it is wrong, and the Code C stand makes it maximally wrong. Its five
service anchors nearly all cast the same way: `HydrantPit`, `EquipmentFwd` and
`EquipmentAft` at −90, `FixedGPU` at +90, `TugStand` at 180. So a road drawn the way a player
naturally draws one — ALONGSIDE a row of stands — is parallel to every anchor's ray on every
stand and serves none of them. Measured on M_Starter: with a road drawn along the row, `9 of
25 lead-in(s) joined`, and the only service anchor that joined at any stand was `TugStand`,
the one anchor that casts across the road instead of along it.

The player has no way to see any of this. An anchor's heading is authored data with no
representation on screen; the only symptom is a truck that never comes.

## 2. Decisions taken

- **Aircraft connections keep the ray. Service connections join by PROXIMITY, any
  direction.** The rule splits on the link's traversal class, which `FPendingLink` already
  carries. Rejected: nearest-for-everything, for the reason `FAnchorLink`'s header already
  gives — but note that rejection is about an AIRCRAFT lead-in, where the direction is the
  painted line. It never applied to a vehicle, which may genuinely arrive from any side.

- **A stand carries a SERVICE LOOP: a closed, invisible vehicle lane enclosing the aircraft
  and every anchor.** Anchors spur to it; roads join it. Rejected: joining each anchor
  directly to the road by proximity. That is simpler and it drives trucks THROUGH PARKED
  AIRCRAFT — the anchors sit around the aeroplane (`HydrantPit` under the starboard wing,
  `FixedGPU` off the port bow), so a straight spur from a road on one side to a box on the
  other crosses 37 m of fuselage. Nothing in the model would stop it: the guideline graph has
  never had an opinion about geometry crossing an aeroplane.

- **Vehicles stay on the graph. There is no free movement.** Rejected: letting a vehicle
  manoeuvre freely once inside the stand. Airside vehicles follow marked routes; and the
  arbiter's entire vocabulary is claims on graph resources, so free positioning has nothing
  to claim and would need a second movement mode. CLAUDE.md's second invariant exists to stop
  exactly that — a second evaluator lets an agent leave the line the player was shown.

- **The loop is INVISIBLE.** No marking builder, no material, no mesh. It exists only as
  guideline nodes and edges, and shows in the `G` overlay because everything in the graph
  does. It is a routing lane, not paint.

- **The loop is COMPUTED by the stand's own builder, and stored on the definition.**
  `BuildCodeCStand` fills it from the design aircraft's footprint and the anchors. Rejected:
  four hand-typed corners (a third authored thing that must agree with the aircraft and the
  anchors, and would drift from both — the class of bug `FResolvedAnchor` and
  `RefreshResolvedAnchors` already exist to paper over). Rejected: deriving it at rebuild
  time (a runtime algorithm's opinion with no override, and a second evaluator).

- **Four-sided, closed.** Rejected: an open U round the nose. A closed loop gives entry from
  any side, including from behind the tail, and costs nothing because the lane is invisible.

## 3. The connection rule

In `FAnchorLink::Build`, the search splits on `FPendingLink::Class`:

| Class | Rule | Reach |
|---|---|---|
| `Aircraft` | Ray along the link's heading, first hit wins. UNCHANGED. | `DefaultMaxLeadIn`, 20000 uu |
| anything else | Nearest point on the nearest guideline of that class, any direction. | `ServiceLinkRadius`, 5000 uu |

**The radius is short, and that is what keeps the rejected case rejected.** A Code C stand is
about 40 m deep and the gap from stand to service road is typically 10–30 m, so 50 m reaches
the road the player meant and cannot reach the far side of a terminal. It is a level-authored
`UPROPERTY` on `ARoadNetworkActor` beside `TrafficRules`, not a constant: it is per-airport
gameplay tuning, not a content default (the same distinction `TrafficRules` records).

A road within reach of two stands joins BOTH, each getting its own connection. That is
correct and is the normal case — one service road serving a row of stands.

**The fuel depot needs no loop.** It has one pose and no anchors, and its pose is a service
connection, so it joins by proximity under this same rule. It stops depending on the player
aiming it, which is the other half of what cost time in the fuel slice.

## 4. The service loop

`UEntityDefinition` gains one field, in the entity's own local space, filled by the same
builder that lays the anchors:

```
UPROPERTY(EditAnywhere) TArray<FVector2D> ServiceLoop;   // implicitly closed; empty means none
```

CLOSED IMPLICITLY: the last point joins the first, and the array does NOT repeat it. Storing
the repeat would be a value that must agree with another value in the same array, which is
the drift this design is otherwise careful about.

Empty is a supported state and means "no service loop" — a definition with no service
anchors needs none, and the fuel depot has none.

**How the builder computes it.** The bounding box of the design aircraft's footprint UNION
every anchor, expanded by a clearance of 300 uu — a constant in the builder, NOT a runtime
property: it is a fact about how this stand type is laid out, decided at authoring time
beside the anchors, and a level-authored version of it would be a knob that silently
reshapes stands already placed. The union matters: a
footprint-only box would leave `TugStand` at +1400 outside, because the tug waits nine metres
ahead of the nose.

For the A320 the Code C stand is sized for — footprint X ∈ [−3250, +507], wingtips ±1790,
anchors reaching to X = +1400 and Y = +1100 — that gives:

```
X ∈ [-3550, +1700]      Y ∈ [-2090, +2090]
```

3 m outboard of the wingtips, 3 m behind the tail, 3 m ahead of the tug's box.

This needs `BuildCodeCStand` to KNOW the design aircraft, which it currently does not —
`MakeStandTransient` and `build_stand_asset.py` both set `DesignAircraft` after calling it.
Passing it in is how it should always have been: a stand's geometry is laid out around the
aircraft it is sized for.

**Spurs.** Each service anchor's node gets a derived edge to its nearest point on the loop.
On the Code C stand every one is short, and none crosses the aeroplane:

| anchor | local | spur | 
|---|---|---|
| `HydrantPit` | (−1200, +700) | 13.9 m, under the starboard wing |
| `EquipmentFwd` | (−300, +1100) | 9.9 m |
| `EquipmentAft` | (−2100, +1100) | 9.9 m |
| `FixedGPU` | (+300, −600) | 14.0 m, forward along the port side |
| `TugStand` | (+1400, −600) | 3.0 m |

## 5. The invariant

> No loop segment and no anchor spur crosses the design aircraft's centreline between
> `TailX` and `NoseX`.

That is the fuselage as a line, computable from `FEntityFootprint` as it stands with no new
field. Crossing it means driving through the aeroplane lengthwise, which must never happen.

**Deliberately NOT "does not intersect the footprint".** That would forbid passing under a
wing, which is normal and which the hydrant requires — `HydrantPit` is under the starboard
wing root because that is where a hydrant pit is.

Measured, not asserted: the test computes the crossings. An assertion that merely named the
contract would pass on a stand whose lane ran straight through the fuselage.

## 6. What reaches the graph

All of it derived, and rebuilt every pass like every other derived edge, so it follows the
stand when the stand moves and is swept when the stand goes:

1. The loop transforms to world by the instance's pose and becomes GroundVehicle guideline
   nodes and edges.
2. Each service anchor's node gets its spur to the nearest point on the loop.
3. The loop joins the nearest vehicle guideline within `ServiceLinkRadius`, any direction,
   splitting both and linking — the same split-and-link `FAnchorLink` already does.

   **The search must exclude the stand's OWN loop and spurs**, and every other entity's,
   or a loop will join itself and a stand will read as connected while reaching no road.
   `FAnchorLink` already has this problem and its answer: it excludes any edge touching an
   anchor node. Loop and spur edges need the same treatment, which is why they are recorded
   as belonging to the entity that generated them rather than being anonymous derived edges.
4. The stand's own POSE lead-in is untouched: still a ray, still 200 m, still the aircraft's
   painted line.

Anchor nodes and the pose node stay non-derived, as today, so their handles survive a
rebuild.

## 7. Migration

None, and that is a property of the design rather than luck. The loop lives on the
DEFINITION and is derived into the graph at build time; no instance stores one. So:

- Re-running `Tools/Python/build_stand_asset.py` gives `DA_Stand_CodeC` its loop.
- Every stand already placed in `M_Starter` picks it up on the next rebuild.
- Nothing in the level needs patching, and no saved instance changes shape.

## 8. Tests

World-free unless stated; every seam has one that fails if it is unwired.

- **Entities.ServiceLoopEnclosesTheStand**: the computed loop contains the design aircraft's
  footprint and every anchor, with the clearance.
- **Entities.ServiceLoopClearsTheAircraft**: the invariant of §5, measured — no loop segment
  and no spur crosses the centreline between `TailX` and `NoseX`. Includes a hand-built
  definition with an anchor on the far side, which must fail the crossing test, so the test
  can tell a passing case from a vacuous one.
- **Build.ServiceLinkJoinsFromAnyDirection**: a road placed on each of the four sides in turn
  joins the loop, at 50 m; one at 200 m does not.
- **Build.AircraftLeadInStillCastsARay**: a stand whose pose faces away from a taxiway 10 m
  behind it still does NOT join it — the aircraft rule is unchanged, and this is the test
  that fails if the proximity rule leaks into it.
- **Build.RoadAlongsideARowOfStands**: the case that failed in PIE on 2026-09-07 — a single
  road parallel to four stands connects every stand's `HydrantPit`. Pins the whole point.
- **Traffic.TruckReachesHydrantWithoutCrossingTheAircraft**: a truck routed depot to hydrant;
  its plan's polyline is measured against the parked aircraft's centreline.
- **Build.ServiceLoopDoesNotJoinItself**: a stand with a loop and NO road within reach stays
  unconnected and is counted as unjoined. The loop is itself a vehicle guideline, so a search
  that failed to exclude it would report every stand connected and no truck would ever route.
- **Probe**: `StarterMapRoutes` reports, per stand, whether its loop is connected to a road.

## 9. Out of scope, recorded

- **Manoeuvring inside the stand.** Vehicles follow the loop and the spurs. Two vehicles
  wanting the same stretch queue on it, exactly as two aircraft queue on a taxiway. Genuine
  local avoidance arrives when M3's job board puts several vehicles on one stand and there is
  something real to avoid.
- **A player drawing a road across a stand.** Nothing prevents it, the same way nothing
  prevents a taxiway through where a terminal would go. Buildable area is its own question.
- **A loop for the fuel depot.** It has one pose and no anchors; §3 covers it.
- **Anything visible.** No paint, no mesh, no marking builder. If a stand's service road
  should be visible later, it is a marking builder over the same polyline and changes nothing
  here.
- **Per-anchor exclusivity for several vehicles at one stand.** M3.
