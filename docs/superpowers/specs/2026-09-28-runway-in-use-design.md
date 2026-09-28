# Runway in use - design

2026-09-28. Branch `feature/runway-in-use`. Trigger: `samples/deadlock.png` - arrivals
vacating the east end of a strip while a departure taxied out through the same connector to
take off the other way. Nose to nose on a one-lane connector.

## Cause (traced, not measured)

- `ArrivalPlanner::Plan` lands at the threshold NEAREST the flight's approach focus.
- `DeparturePlanner::PlanAny` tries BOTH thresholds of every runway and keeps the shortest taxi.

Neither knows about the other, so one strip runs opposite-direction traffic.

## Rulings (user, 2026-09-28)

1. **One direction in use per runway, chosen by the player.** No wind model. Landings and
   take-offs both use it: land over the in-use threshold, take off from it, same heading. The
   taxi flow round the field follows from that; no one-way taxiways.
2. **A flip affects new plans only.** A landing rolling or a departure already planned finishes
   as planned (real runway-change practice).
3. **No exit ahead in the direction in use = `NoExit`**, refused with a sentence naming the
   fix. No arrival backtrack.
4. **Flipped from the runway's inspector card**, a run-time traffic control, not the build tool.
5. **Default = the direction drawn**: take-off from the first click toward the last.
6. **No ground indicator in this branch** - card text only; follow-up issue for a marking.

## Design

### Data - `FRunwayFacts::InUse`

`int32 InUse = 0` beside Surface and Approach: the DESIGNATOR of the end in use (27, not a
vector, not a chain-end flag). A designator survives splits, heals and drags that reorder a
chain's ends; a rotated runway resolves to whichever end is nearer the stored heading. 0 means
unset (runways placed before this field) and resolves to the LOWER designator - deterministic,
never map-order. In `operator==`. `URoadNetwork::SetRunwayFacts` already writes the whole chain
and a split copies the facts, so no new propagation code.

### One resolver - `RunwayQuery`

- `FRunwayEnd InUseEnd(const URoadNetwork&, const FRunwayEnd& Either)` - Either, or
  `Either.Reversed()`, whichever's `Designate(Direction)` is circularly nearer
  `RunwayFactsFor(Seed).InUse` (lower designator when 0; tie keeps the lower too).
- `bool InUseRunwayAt(Network, Near, Out)` = `RunwayExtentAt` then `InUseEnd`.
- `bool InUseRunwayNearest(Network, Near, Out)` = `NearestRunwayThreshold` then `InUseEnd`.
- URoadNetwork forwarders at the same names, as for every other RunwayQuery function.

### Consumers

| Site | Change |
|---|---|
| `ArrivalPlanner::Plan` step 1 and `IsRunwayBusy` | `InUseRunwayNearest`. Runway choice unchanged (nearest to focus); END is the one in use. |
| `DeparturePlanner::Plan` | `InUseRunwayAt`. OnRunway now picks the runway, not the end. |
| `DeparturePlanner::PlanAny` | one probe per runway instead of `Ends[2]`; shortest taxi still chooses BETWEEN runways. |
| `URoadEditFacade::PlaceRunway` | `Facts.InUse == 0` -> `Designate(To - From)`. One site covers the tool and every test caller. |
| `ArrivalPlanner::DescribeRefusal(NoExit)` | "no exit ahead landing 27: add one, or change the runway in use" (plan overload; figure-free overload says the same without the number). |

**Not changed, and why:**

- `LandChoices::Build` reads only `End.Seed` for `CheckArrival`, which is per chain. Either
  end gives the same answer.
- `UGroundTraffic::ArmDepartureIfRunway` derives the roll from the route's end (mid-strip:
  the way it arrived; at an end: away from it). A planner route now arrives aligned with, or
  backtracks to, the in-use threshold, so the derivation gives the in-use direction. And on a
  rebuild re-arm AFTER a flip it keeps the ORIGINAL direction, which is ruling 2. Forcing
  `InUseEnd` there would break ruling 2. It gains a Warning when the armed direction
  disagrees with the runway in use, so a future planner that stops honouring it is loud.
- Marking, capability, admission and inspector-destination `RunwayExtentAt` calls describe
  the strip, not traffic on it.

### Flip - runway selection, card, action

There is no runway card today: `ESelectionKind` is None/Aircraft/Stand. Added:

- `ESelectionKind::Runway`, Id = a runway segment INDEX (as Stand's is an entity index);
  `FSelectTool` selects it when the click is on a runway strip and on no aircraft or stand.
- `InspectFacts::DescribeRunway` -> `FRunwayCardFacts`: pair name "09/27", in use "27",
  surface, approach, length.
- Inspector card: title "Runway 09/27", facts "In use: 27 ...", and a button.
- Action `selection.runway_in_use` (Selection section, no key), label "Use 09" (the OTHER
  end), enabled only with a runway selected. Found by ID in the inspector, not position.
- `ARoadBuildController::FlipSelectedRunway` -> reads facts, sets `InUse` to the reciprocal,
  `ARoadNetworkActor::SetRunwayFacts` (existing forwarder -> facade; undoable for free).
- Log: `LogRoadMesh: Runway 09/27 in use: 27 (was 09)` from the facade when InUse changed.

Flipping can change which offers the field can accept (offers plan with no occupancy, and
`NoExit` now depends on direction). Intended.

## Tests

Model (no world):

- Depart honours in-use even when the other end's taxi is shorter; flip -> the other end.
- Arrival lands at the in-use threshold whatever side the focus is on; flip -> reverses.
- In-use with no exit ahead -> `NoExit`, sentence names the direction and the fix.
- `InUse` survives a split (exit added) and resolves after the runway is dragged 30 degrees.
- `InUse == 0` resolves to the lower designator.
- Composition: actor `PlaceRunway(From, To)` stores `Designate(To - From)`; flip through
  `SetRunwayFacts` forwarder, the next `DepartAgent` rolls the new way.
- **Deadlock regression**: `UGroundTraffic` ticked with arrivals and a departure on the
  one-connector layout; no departure is ever armed against the direction in use.
- `ArmDepartureIfRunway` after a flip + rebuild keeps the original direction (ruling 2).
- Select tool: click on strip selects Runway; aircraft/stand still win over it.

Lint - Check-Architecture rule: `ArrivalPlanner.cpp` and `DeparturePlanner.cpp` may not call
`RunwayExtentAt(` or `NearestRunwayThreshold(`; they choose an end only through the in-use
resolvers. Deleting the in-use call in either file must turn the rule red.

## Out of scope

Ground indicator (follow-up issue). Arrival backtrack. One-way taxiways. Parallel-runway
L/R. Wind.
