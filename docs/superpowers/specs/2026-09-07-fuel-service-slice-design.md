# Fuel Service Slice — Design

**Status:** design, agreed in conversation 2026-09-07 after PRs #64–#66 (node reach,
reservation yield, free-runway-end replan). Sits between M2 and M3 in the build order.

**Parents:** `2026-09-05-game-systems-map-design.md` §3.5 (jobs), §3.6 (buildings, fleet),
§5.3 (build order); `2026-09-06-ground-traffic-design.md` (occupancy, classes, resolver);
`2026-09-07-stand-occupancy-design.md` (the stand's claim, the facts path).

## 0. Nothing here is law

As the systems map §0. Change it for a reason, after discussion, and record the reasoning
where the old decision was.

## 0.1 This is scaffolding

Agreed up front: this slice is built with what exists so the first service delivery can be
watched end to end before M3, and three of its pieces are placeholders that M3/M4 replace.
They are named here so nobody mistakes them for the design:

| Placeholder | Replaced by | Where |
|---|---|---|
| The depot is a `UEntityDefinition` with a truck count | `UBuildingInstance`: road anchor node, add-on modules, fleet, inventory | systems map §3.6, M4 |
| `UFuelService` picks the nearest depot and dwells on a timer | `UJobBoard`: demands from a flight, depot bids by ETA, N-trip jobs, stuck recovery | systems map §3.5, M3 |
| The truck's performance is an `FAirframe` with only its ground figures set | a vehicle-shaped performance bundle | M3, with the fleet |

The road profile, the crossing rules, the anchor joins, the vehicle agent and its view are
NOT scaffolding: M3's job board dispatches onto exactly these.

## 1. Why

The GDD's first fifteen minutes (§5) is: place a runway, a stand, a taxiway, a depot on a
road, accept an offer, watch the aircraft land, be serviced and leave. Everything before the
service exists and is PIE-verified. Nothing of the service does: no road a vehicle can drive
on, no building, no vehicle agent, and nothing that turns "this aircraft needs fuel" into a
journey. M3's offers and flight board would be building a demand pipeline into a void.

So: one service, fuel, demanded by every aircraft that parks, met by one truck from one
depot over service roads. When that is watched working, M3 puts a flight in front of it.

## 2. Decisions taken

- **Every parked aircraft demands fuel; nothing else triggers it.** No button, no offer.
  The player's only act is to have built a depot on a road that reaches the stand. M3
  replaces "every aircraft" with the flight's demand list; the service does not change.
- **Trucks drive on service roads only.** A road is its own profile with a GroundVehicle
  guideline; it may cross a taxiway at a junction and never shares taxiway or apron
  pavement. Rejected: a vehicle lane on the apron profile (no apron guidelines exist to
  hang it on), and a vehicle guideline beside every taxiway centreline (every taxiway
  becomes a mixed queue and the class priorities stop meaning anything).
- **The depot is an entity, not a building.** Same placement, anchor link, undo and save as
  a stand; §0.1 says what M4 does with it.
- **The service is AirportOps' business.** Demand, dwell and job state are game facts.
  Airside gains only nouns it lacks - a road profile, the depot definition, a vehicle
  performance default, a vehicle view - and keeps its rule of never knowing what a truck is
  for. Rejected: the traffic model spawning service vehicles itself.
- **Nearest depot by route length, one truck out per depot's count.** No bids, no ETA;
  M3's board is where bidding lives.
- **A placeholder truck**: a box of the vehicle footprint through a content-default mesh
  slot, so a real asset drops in without code.

## 3. Roads and crossings

- A second `URoadProfile` asset, `DA_ServiceRoad`, resolved through `UAirsideSettings`
  like the taxiway profile: a narrow lane band with kerb edges (see the edge-treatment
  note: kerbs for roads, run-offs for taxiways), and ONE guideline of class GroundVehicle,
  bidirectional, `MaxWingspan` 0 (unlimited), no exit length, not continuous through
  junctions.
- `FRoadDrawTool` takes its profile from its registry entry rather than assuming the
  taxiway; key 9 lays the road profile as "Road". One tool, two entries. Key 1 stays
  "Taxiway".
- Junctions between roads, and between a road and a taxiway, go through `FRoadNetworkSolver`
  and `FRoadGuidelineBuilder` unchanged. The builder already intersects the two arms'
  class masks for every turn path (`Turn.AllowedTraffic = From & To`), so at a crossing
  vehicles get road-to-road paths, aircraft get taxiway-to-taxiway paths, and there is no
  road-to-taxiway turn for anybody. That is the whole of the crossing rule and it needs no
  new code; a test pins that it holds.
- A road that ENDS against a taxiway's side makes a junction with no vehicle path through
  it. The builder's census line reports "N junction(s) with no through path for a class";
  the tool does not refuse the placement, because the player may be about to draw the far
  side.
- A road never carries a holding position, runway or intermediate: `EHoldingPositionKind`
  derivation reads the arm's guideline class and skips GroundVehicle arms. A road ending
  ON a runway is refused by the road tool, the one placement it refuses.
- Crossing a runway is a road-runway junction like any other taxiway crossing: turn paths
  and nodes, no `DerivedFrom`, and the crossing hold (spec 2026-09-06 §3.1 route four) holds
  the strip by the truck's body exactly as for an aircraft. Nothing here is special-cased.

## 4. The depot

- `UEntityDefinition` `DA_FuelDepot`, authored beside `DA_Stand_CodeC` by the same
  commandlet and made transient by `MakeFuelDepotTransient()` for tests. A footprint for
  the placement preview, and ONE anchor of role `EServiceRole::Fuel` whose cast direction
  points at the road it serves. `Trucks = 1` on the definition.
- `FAnchorLink::Build` joins the anchor to the nearest road guideline within the cast
  distance, creating the depot's pose node on the road, exactly as a stand's aircraft anchor
  joins a taxiway: the sweep, the split, the census. A vehicle anchor joins only a
  GroundVehicle guideline; the link already filters by the anchor's class mask, which is
  why stands' fuel anchors log as unjoined today - there is nothing of their class to reach.
- Placement is `FStandPlaceTool` parameterised by definition, under a new key, "Fuel depot",
  with the same undo history and save path. The Details of an entity instance already carry
  the definition, so save/load needs nothing.
- Inert cases say so: no road within reach -> the census warning at build and "Fuel depot:
  not on a road" in the inspector when the depot is selected (`InspectFacts::DescribeEntity`
  grows a depot case beside the stand one).

## 5. The truck

- An Airside agent of class GroundVehicle, dispatched through
  `ARoadNetworkActor::DispatchAgent(Plan, Airframe, Class, ShutdownPause)`, which exists.
- Performance from `UAirsideSettings::ResolveDefaultVehicle()`: an `FAirframe` with
  `Ground` set (a van's figures: accel 100, decel 200, cap 1000 uu/s, nosewheel 90 deg/s),
  `TypeCode` "FUEL", climb/approach/engine left default and never read. Resolved in exactly
  one function, per the content rule. §0.1 names the struct as scaffolding.
- The view is `ARoadAgentActor` with the mesh from a new content slot `VehicleMesh`,
  defaulting to a box scaled to `FTrafficRules::VehicleFootprint` by half that width. The
  actor already sizes and poses from the agent; a mesh slot is the only addition.
- Two plans. OUT: `RouteSearch::Find` from the depot's pose node to the stand's Fuel anchor
  node, class GroundVehicle, wingspan 0, `AvoidRunways = All`; the truck parks at the anchor
  (`Parked`, holding the anchor node as any parked agent holds its node). HOME:
  `RedirectAgent` with a route from the anchor node back to the depot's pose node - the
  model accepts a redirect for a Parked agent - and on parking at home the service calls
  `RetireAgent`. The truck exists as an agent only between the two.
- On the road it is ordinary traffic: edge and node claims, the vehicle footprint and gap,
  aircraft over vehicle at every contested node, deadlock cycles like anything else. Stuck
  beyond the resolver's retry cadence is M3's stuck recovery, not this slice's.

## 6. The service — `UFuelService` (AirportOps)

A UObject owned by `UOpsRuntime`, world-free: it takes the `UGroundTraffic`, the
`URoadNetwork` and the `USimClock` it is given, so a test drives it with the traffic fixtures
and no actor.

**State.** `TArray<FFuelDemand>`:

```
struct FFuelDemand
{
    int32 AircraftId;            // the parked agent
    FEntityInstanceId Stand;     // where it parked
    EFuelDemandState State;      // Needed, TruckEnRoute, Fuelling, Done, Unserviceable
    int32 TruckId = 0;           // while a truck is out for it
    FEntityInstanceId Depot;     // whose truck
    double DwellEndsAt = 0.0;    // sim seconds, set on Fuelling
    EFuelRefusal Why;            // NoDepot, NoRoad, StandUnjoined, NoRoute - for Unserviceable
};
```

A phase is an enum, never a set of bools. `Unserviceable` is terminal for that demand;
a graph rebuild re-offers it (below).

**Inputs.** The runtime's existing `OnAgentPhase` relay and the sim clock's tick.

**Transitions.**

| Event | From | To | Action |
|---|---|---|---|
| aircraft `Taxiing -> Parked` at a stand's pose node | - | `Needed` | add the demand |
| tick, demand `Needed` | `Needed` | `TruckEnRoute` | nearest depot with a truck free and a route: dispatch OUT, record truck and depot |
| tick, demand `Needed`, nothing can serve it | `Needed` | `Unserviceable` | record `Why`; log once |
| truck `Taxiing -> Parked` at the stand's anchor node | `TruckEnRoute` | `Fuelling` | `DwellEndsAt = now + DwellSeconds` |
| tick, `now >= DwellEndsAt` | `Fuelling` | `Done` | redirect HOME |
| truck `Taxiing -> Parked` at the depot's pose node | `Done` | (demand stays `Done`) | retire the truck; the depot's count frees |
| aircraft leaves `Parked` (departs, retired, `Gone`) | any open | dropped | a truck out is redirected HOME as if `Done` |
| graph rebuilt | `Unserviceable` | `Needed` | the player may have drawn the road |

"At a stand's pose node" reads `URoadNetwork::FindEntityIndexByPoseNode` as the inspector
does; an aircraft parked on a taxiway junction (stand-death fallback) makes no demand.

**Choosing a depot.** Every live depot entity with a joined anchor and fewer trucks out than
its count; route length from its pose node to the stand's Fuel anchor node; the shortest
wins. No route from any: `NoRoute`. No depot at all: `NoDepot`. Depot(s) exist but none is
joined: `NoRoad`. The stand's Fuel anchor has no node: `StandUnjoined`. Checked in that
order so the reason names the thing nearest the player's hand.

**Figures.** `DwellSeconds = 40` sim seconds, on `UFuelService` as an EditAnywhere property
of the runtime's config, not a constant in code. `Trucks` on the depot definition.

**Outputs.** One log line per transition on `LogAirportOps`, naming aircraft, stand, depot
and truck ids. The inspector's aircraft card gains one line through `InspectFacts`:
`Fuel: needed | truck en route | fuelling | done | no fuel depot | depot not on a road |
stand not on a road | no road from depot`. `FAgentFacts` gains the field; the ops runtime
fills it, so the Airside facts layer stays ignorant of fuel.

**Not here.** Litres, fees, the ledger, a second service role, bids, more than one truck per
demand, a truck serving two demands in one trip.

## 7. Failure reporting

Every failure has a log line when it becomes known and a state the player can read:

| Situation | Log | Player sees |
|---|---|---|
| depot placed off any road | anchor census warning | "Fuel depot: not on a road" on the depot |
| stand's fuel anchor joins nothing | anchor census warning (exists today) | "Fuel: stand not on a road" on the aircraft |
| no depot | `UFuelService` once per demand | "Fuel: no fuel depot" |
| no route depot to stand | `UFuelService` once per demand | "Fuel: no road from depot" |
| road ends against a taxiway's side | builder census | nothing on the entity; the truck's `NoRoute` names it later |
| truck deadlocked | traffic resolver's own lines | truck stands still; "truck en route" persists |

## 8. Tests

World-free unless stated; every seam has one that fails if it is unwired.

- **Build.RoadProfileGuideline**: the road profile derives one GroundVehicle guideline, no
  holding position at a runway end.
- **Build.RoadCrossesTaxiway**: a road crossing a taxiway yields vehicle paths road to road,
  aircraft paths taxiway to taxiway, and no road-to-taxiway turn in either mask.
- **Build.RoadEndsAgainstTaxiway**: reported in the census; the aircraft path through the
  junction is unaffected.
- **Entities.DepotJoinsRoad**: the depot's Fuel anchor joins the nearest road; off-road
  stays unjoined and is counted.
- **Entities.StandFuelAnchorJoinsRoad**: the fixture that logs the stand's fuel anchor
  unjoined today joins it once a road is within reach.
- **Traffic.TruckCrossesTaxiway**: a truck routed depot to stand across a taxiway with an
  aircraft on it yields at the crossing; min separation measured; parks at the anchor.
- **Ops.FuelService**: one stand, one depot, one road: park an aircraft, tick, assert every
  transition of §6 in order, the dwell length, the retire at home, the count freed.
- **Ops.FuelServiceRefusals**: no depot, depot off-road, stand unjoined, no route - each
  `Unserviceable` with its `Why`; a rebuild that adds the road re-offers it.
- **Ops.FuelServiceAircraftLeaves**: the aircraft departs mid-service; the truck goes home;
  the demand is dropped.
- **Present.FuelServiceWired**: spawn the actor and runtime, park an aircraft, tick: a
  GroundVehicle agent appears, reaches the anchor, and goes - the composition-level proof the
  relay and the dispatch are bound.
- **Probe**: `StarterMapRoutes` reports how many stand fuel anchors join a road and whether
  each depot does, so the player's level can be read headlessly.

## 9. Out of scope, recorded

- A vehicle waiting for a stand's fuel anchor that another vehicle occupies: no queueing at
  anchors; the second truck's route ends on a held node and it waits, which the traffic
  model already does. Two trucks per stand is M3's board.
- Roads over aprons, road parking bays, depot geometry beyond a footprint.
- Vehicle-only priority overrides at crossings: the node `PriorityOverride` exists and the
  road tool does not author it yet.
- The truck's fuel quantity and the aircraft's tank: §3.6 inventory, M4.
