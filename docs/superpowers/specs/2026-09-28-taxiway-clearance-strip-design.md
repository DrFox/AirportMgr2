# Taxiway clearance strip - design

2026-09-28. Rulings from a design discussion the same day; each is marked (user) or
(proposed, accepted).

## Problem

A taxiing aircraft's wing overhangs the taxiway's pavement edge by half its span less half the
pavement width. Nothing in `Airside` reserves that ground. A stand's entrance edge sits ON the
taxiway edge and its parked tail only `WingtipClearance` inside it (`StandBox::EntranceSetback`
= `MaxTailAft + WingtipClearanceForLetter`), so at a stand of floor depth:

| Taxiing past, on its letter's taxiway | Wing past edge | Tail inside edge | Overlap |
|---|---|---|---|
| Saab 340 on B (12 m) | 4.7 m | 3 m | 1.7 m |
| 737-800 on C (16 m) | 9.9 m | 4.5 m | 5.4 m |
| 757-300 on D (18 m) | 10.0 m | 7.5 m | 2.5 m |
| 777-300ER on E (24 m) | 20.4 m | 7.5 m | 12.9 m |
| A380 on F (26 m) | 26.9 m | 7.5 m | 19.4 m |

Traced from `IcaoCode.cpp` and `build_road_profiles.py`, not measured in play.
`EntityDefinition.cpp` already concedes it: the strip between stand and taxiway is ground
"which nothing checked and a taxiing wing sweeps". Roads, and buildings on plots off roads, can
sit in the same sweep.

## Rejected: widen the pavement

Pavement of span + 2 x clearance is 45 m for C, 80 m for E, 95 m for F - wider than the game's
46/60 m runways. It reads as runway at zoom, fillets scale at 2/3 of width (E corners 16 -> 53 m),
and pavement implies aircraft may use it. Real aerodromes keep the pavement to the gear and
clear a STRIP beside it; so does this.

## The strip

**Every taxiway has a clearance strip each side of its pavement, sized by the TAXIWAY's
letter**, read from its pavement width: the largest letter whose ICAO minimum taxiway width
(A 7.5, B 10.5, C 15, D 18, E 23, F 25 m) the pavement meets - never the letter of whatever is
placed beside it. Not `MaxWingspan` - taxiway guidelines carry 0 (unlimited). Not nearest-width,
the runway rule: the game's 24 m sits exactly between E and F (per "taxi lines are painted infrastructure": size ground for the
largest aircraft admitted).

    Strip = MaxWingspanForLetter(letter)/2 - PavementWidth/2 + WingtipClearanceForLetter(letter)

| Taxiway | Pavement | Max span | Wing past edge | Clearance | Strip |
|---|---|---|---|---|---|
| B | 12 m | 24 m | 6 m | 3 m | 9 m |
| C | 16 m | 36 m | 10 m | 4.5 m | 14.5 m |
| D | 18 m | 52 m | 17 m | 7.5 m | 24.5 m |
| E | 24 m | 65 m | 20.5 m | 7.5 m | 28 m |
| F | 26 m | 80 m | 27 m | 7.5 m | 34.5 m |

DERIVED, not authored: a stored strip would be a third figure obliged to agree with the
profile's width and span (the same reason `IcaoCode` has no stand-width column). Slightly
tighter than ICAO's taxiway-to-object distances, which also allow for lateral deviation off
the centreline; accepted, and the formula's clearance term is the knob if play says otherwise.

A B stand on an F taxiway pays 34.5 m of strip (proposed, accepted). That is the cost of
parking a GA aircraft straight off an A380 taxiway, and it nudges the player to a code B spur -
which is what real aerodromes do - without the game forbidding anything.

Runways are out of scope: they have their own strip rules and holding positions already.

## One keep-out zone, one query (proposed, accepted)

The strip is a KEEP-OUT ZONE on the ground, not a rule per object type. **The rule applies to
everything placed (user)**: stands, roads, taxiways, aprons, building plots, and whatever comes
next.

- One world-free query in `Model/` - given a footprint (polygon) and the network, which
  taxiway strips does it intrude on, and by how much. `NewObject`-testable with no world.
- Every placement tool calls it, and so does the profile-change path. Lists that must agree
  are one list: a test enumerates the placement tools (`ToolRegistry()`) and asserts each one's
  preview refuses a footprint inside a strip, so a new tool that forgets the query goes red.
- Placement preview: the ghost goes red where it intrudes, and the readout says which taxiway
  and how far, e.g. "Inside taxiway T3's strip by 6 m (needs 34.5 m, code F)".

### What may pass through a strip

Without crossings and junctions nothing could connect (user: "building an airport will
become impossible if we don't allow it").

- **Network that MEETS the taxiway may pass through its strip**: a road or taxiway segment
  whose end, or crossing node, lies on the taxiway, inside that strip. Its footprint within the
  strip is exempt.
- **It must meet within 30 degrees of square (user)**. A shallow diagonal technically
  "meets" the taxiway while running along its strip for 100 m; 30 degrees stops that.
- **A stand's lead-in counts as meeting** - it is exempt the same way.
- **Running alongside inside a strip is refused** - a segment that enters a strip and does not
  meet the taxiway within it.
- **Parallel taxiways**: one taxiway's PAVEMENT may not enter another's strip, which gives
  parallel taxiways span + clearance centreline spacing from the same query.

### Holds sit at the strip edge (proposed, accepted)

Anything waiting to cross or join a taxiway must wait clear of a passing wing - exactly the
strip edge.

- A road crossing a taxiway gets its vehicle stop line at the strip edge.
- A taxiway joining another gets its intermediate holding position at the strip edge - where
  ICAO places them, for the same reason. `SetIntermediateHoldingPosition` exists today and is
  player-placed; the distance from the junction is what this spec adds.

Whether ground vehicles yield to aircraft at a road-taxiway crossing today is UNVERIFIED -
`RoadGuidelineBuilder` restricts cross-class TURNS to Emergency, and a straight-through road
crossing appears to build, but the yield has not been read. Stage 4 checks it first.

## Stands

- **The stand box's entrance edge moves off the pavement edge to the strip edge.** The box
  itself (depth, width, template, stop mark relative to the box) is unchanged; the strip is
  separate ground in front of it.
- Assumptions that "entrance = taxiway edge" move with it: `StandBox::PoseFor`, the lead-in
  attachment in `AnchorLink`, `StandMarkingBuilder`.
- A stand touching two taxiways (at a junction) takes the larger strip (proposed).
- **The player still drags from the pavement edge.** The drag's first strip-width is the
  strip; the box is what lies beyond it. So the shortest legal drag is strip + the letter's
  floor depth, and the ghost shows both parts. A stand already drawn before stage 2 keeps its
  outline; if what remains beyond the strip is under floor depth it is invalid (same state as
  an upgrade leaves it in), and is redrawn. No player saves exist, so only test maps pay.
- Implemented as: the tool anchors the entrance at pavement edge + strip, so the committed
  outline is the parking box alone and no `StandBox` reader changes.

### Paint (user, from BHX)

- **White stand lines on the parking box only.** The strip carries no white paint.
- **Yellow lead-in**: painted on the taxiway pavement from the centreline, with an **arrow and
  the stand number** at the turn-off; a **gap across the strip**; yellow resumes inside the
  stand's white lines to the stop mark.
- The gap is PAINT ONLY. The guideline stays one continuous edge and the aircraft follows it;
  both painted pieces are cut from the one `GuidelineGeom::Sample`, so the guideline graph still
  samples once.
- The strip itself is drawn plain (grass or shoulder), so the player can see why the stand sits
  back.

### Stand numbers (user: "don't exist today but would be a great addition")

Stands have no number today. Each stand gets one on placement - painted at the turn-off arrow,
shown in the inspector. Numbering scheme is stage 5's first decision (sequential per airport is
the default; per-apron prefixes like "A12" are the obvious alternative).

## When a taxiway is upgraded

A profile change to a wider letter grows the strip and can leave things inside it. Validity is
therefore a RE-CHECK on every adjoining profile change, not a one-off at placement; a downgrade
restores for free.

- **Stands inside the new strip are invalidated** (proposed, accepted). No new aircraft is
  assigned; **an aircraft already parked finishes its turnaround, then leaves (user)**. Drawn
  flagged; the inspector says why ("strip 9 m, needs 34.5 m - taxiway T3 upgraded to F"). The
  player redraws further back or adds a lower-letter spur.
- **Roads and buildings inside the new strip RESTRICT the taxiway** (proposed, accepted): it
  operates at the largest letter whose strip is clear, and the inspector names the obstruction
  ("max span 65 m - restricted by building at ..."). Nothing the player built is destroyed;
  real taxiways carry published span limits for exactly this. Restriction feeds the same
  `MaxWingspan` admission the guideline graph already carries.

## Buildings

- **A building plot intruding on a strip is REFUSED, not clipped (user).**
- No building is placed against a taxiway today; plots hang off roads. The query sees the
  plot's own footprint, so a road outside the strip does not clear a plot that reaches into it.

## Stages

Each ships alone and leaves the game playable.

1. **The strip, measured.** `Model/` query + derived strip width per profile; world-free tests
   pin the table above. Nothing refuses yet. No overlay - stage 2's gap is the visible
   strip.
2. **Stands set back.** Entrance edge to strip edge; `PoseFor`, `AnchorLink`,
   `StandMarkingBuilder`; lead-in paint gap. Test: a stand off each letter of taxiway has its
   parked tail clear of the widest wing that taxiway admits - measured against the aircraft's
   swept envelope, not restated from the formula.
3. **Placement refuses.** Every tool calls the query; ghost red + readout; the 30-degree
   crossing rule; the per-tool wiring test.
4. **Holds and crossings.** Read the current road-taxiway crossing yield first. Stop line and
   intermediate holding position at the strip edge.
5. **Stand numbers.** Numbering, the turn-off arrow and number paint, inspector.
6. **Upgrades.** Re-check on profile change; stand invalidation (occupant finishes); taxiway
   restriction by roads/buildings; inspector messages.

## Saves

No player saves exist yet (2026-09-23), so no compatibility shim. Existing test maps
(`M_Test`, `M_Test_Small`) will have floor-depth stands go invalid at stage 2 and need
redrawing; roads inside strips show red from stage 3 and, from stage 6, restrict their taxiway
rather than fail to load. Stage 2 therefore also borrows stage 6's invalid-stand state (no
new assignments, occupant finishes) - it is the smallest thing that keeps the maps playable.

## Not decided here

- Numbering scheme (stage 5).
- How the strip is drawn - grass vs shoulder material - is a live visual iteration, not a spec
  figure.
