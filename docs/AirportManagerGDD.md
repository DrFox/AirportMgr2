# Airport Manager — Game Design Document (v2)

**Revised 2026-09-05.** Rewritten from the ~2024 original after a review that found the
holes listed in `superpowers/specs/2026-09-05-game-systems-map-design.md` §1. That spec is
the systems-level companion to this document: this one says what the game is and how it
plays; the spec says what gets built and in what order.

> **Nothing here is law.** These decisions were the best call on what we knew when they
> were made. Development will teach us things design cannot, and some decisions here will
> turn out wrong. When that happens we discuss it, write down why, and change it. Drift is
> not allowed; reasoned change is expected.

---

## 1. Concept

**Working title:** Airport Manager
**Genre:** building and management sim
**Pitch:** Build, expand and run an airport from a grass strip with one stand to a large
international hub. Attract airlines by meeting the demands of their aircraft. Keep the
books positive while the airport outgrows every plan you made for it.

**Audience:** adults and young adults who play Sky Haven, Airport CEO, Transport Fever 2 and
Cities: Skylines. PC only, Steam, Windows 10+. Mouse and keyboard; no controller.

**What sets it apart:** free-form 3D layout. Roads, taxiways, runways and aprons are
spline-drawn at any angle with real junctions and curves, not cells on a grid. Grid snapping
is an optional toggle, never a constraint.

**Visual style:** realism. **Audio:** undecided.

---

## 2. Core loop

An aircraft arrives with demands. The airport meets them with buildings, vehicles and
infrastructure. The player is paid per demand met. The money buys more infrastructure and
research, which admits bigger aircraft with bigger demands.

```
demands → infrastructure + services → fees → research + building → bigger demands
```

The pressure that makes it a management game: every flight has a scheduled off-block time.
Miss it and there is a fine and a reputation hit with the airline. Reputation drives how
many offers arrive and how good they are. Let it fall far enough and an airline stops
coming.

There is no win screen. Growing from regional airfield to international airport is the
arc; milestones along it are soft, marked by the aircraft classes the airport can admit.

---

## 3. Demands

The central abstraction. Every aircraft type carries two lists.

**Infrastructure demands** are hard gates. If the airport does not meet every one, the
aircraft is never offered. Examples: runway length, runway surface, stand size class,
a Control building of a given tier, an instrument approach.

**Service demands** are what the aircraft wants once on the ground. Each is *required* or
*optional*. Missing a required service means no offer. Missing an optional one means the
aircraft still comes and that fee goes unpaid. Examples: refuelling, goods handling,
passenger handling, pushback.

A service demand is abstract ("needs fuel"). When the aircraft reaches its stand it becomes
one or more **jobs**, which are concrete ("deliver 500 litres to stand 5 before 14:20").

---

## 4. Time

A compressed 24-hour game day with a day/night cycle. Speeds: pause, ×1, ×2, ×4, ×8. Real
minutes per game day is a tunable. Everything in the simulation — turnarounds, contracts,
upkeep, research — runs on game time, never wall time.

---

## 5. The first fifteen minutes

The player gets a flat plot with one road leading off-map. Starting money buys a basic
kit: grass runway, taxiway, roads, one stand, a goods depot, a Control building.

1. Drag out a runway to the desired length.
2. Place a stand sized for a small aircraft.
3. Draw a taxiway from runway to stand.
4. Place a goods depot on a road and buy a truck.
5. Connect road from the off-map link to the depot, and from the depot to the stand.
6. Place the Control building on a road.

Offers now start arriving in the inbox: general aviation and charter flights in tiny
aircraft, because tiny aircraft are all the airport can admit. Each offer shows the
aircraft, its demands and the fee. Accept one and:

1. The aircraft lands and taxis to the stand.
2. The depot's truck drives to the stand, unloads cargo, returns, reloads.
3. The truck brings outbound cargo; the aircraft is loaded.
4. At its off-block time the aircraft taxis out and departs. Fees post to the ledger.

Add a stand and two aircraft can be handled at once. Research the fuel depot and longer
range flights start being offered. That is the whole game in miniature.

---

## 6. Airlines and offers

An **airline** has a fleet of aircraft types, a route range, a service profile and a
reputation with the player. General aviation and charter are airlines with a fleet of one
type and no contract path.

Offers are generated over game time. For each airline the game filters its fleet against
what the airport can admit, weights by reputation and range, and posts offers. A more
capable airport gets a busier inbox. Reputation moves only on outcomes: on-time departures
up; late or unpaid optional services down; a diversion sharply down. Below a floor the
airline stops offering until a recovery period passes.

Once research unlocks it, an **airline contract** replaces the inbox for that airline: a
fixed number of flights per day, a fixed service set, fixed fees and a fine per late
departure. Contracted flights go into the **schedule grid**, a stand-by-time-slot view of
one game day. Ad-hoc offers keep arriving and use whatever stands the grid leaves free.

---

## 7. Flights on the ground

Every flight moves through one sequence: offered, accepted, inbound, landing, taxi-in,
turnaround, pushback, taxi-out, departing, departed. It can fail out as declined, diverted
or cancelled.

**Runway.** One occupant at a time. Landings need a clear runway and a usable exit that
reaches a stand; take-offs need a clear runway and no arrival within a separation window.
The Control building's tier sets how many inbound flights can hold. Holding is abstract:
a flight waits off-map and diverts if it waits too long.

**Taxiways and roads.** Agents reserve the ground ahead of them and stop short of anything
already reserved. Junctions and crossings admit one agent at a time; aircraft have priority
over vehicles. Hold-short lines protect runways: an aircraft stops at the line while the
runway is occupied. Vehicles route around congestion when they plan; aircraft follow the
cleared route and may replan only while stopped. Deadlocks resolve by the lowest-priority
waiter backing off; a layout that lets two aircraft face off on a single two-way taxiway
is a player mistake the game will eventually warn about.

**Stands.** Sized by ICAO wingspan class. An aircraft takes the smallest free stand that
fits it. Once scheduling is unlocked, contracted flights take the stand the grid assigns.

**Turnaround.** Service demands become jobs with ordering rules: unload before load,
pushback last, no fuelling during boarding. A job is offered to every depot that can do it;
the depot with the soonest free vehicle wins. A job bigger than one vehicle load is several
trips. Required jobs must finish before off-block time or the departure is late. Optional
jobs unfinished at off-block time are skipped and unpaid; the aircraft leaves on time.

---

## 8. Infrastructure

| Item | Purpose | Built by | Upgrades | Cost |
|---|---|---|---|---|
| Runway | Lets aircraft land and take off | Drag to length | Grass → asphalt → concrete; length | per metre |
| Taxiway | Links runway to stands | Drag a path, any angle | Grass → asphalt → concrete; width follows aircraft class | per metre |
| Road | Vehicle routes between buildings and stands | Drag a path | Surface | per metre |
| Apron | Paved area stands sit on | Draw a polygon | Surface | per m² |
| Stand | Where an aircraft parks and is serviced | Place a sized stand on an apron | Size class; surface | per stand |

The off-map road link is the airport's connection to the world. Inbound contract
deliveries physically drive in over it. A depot that no truck can reach from it gets no
deliveries, and the game says so.

---

## 9. Buildings

Buildings provide services and satisfy infrastructure demands. A building is **ploppable**:
a fixed footprint per tier, rotated into place, with a road anchor that must join the road
network or the building is inert. Capacity grows by attaching **add-on modules** (a fuel
tank, a vehicle bay, a warehouse wing) in the building's slots. Tiers change the building
outright; each tier is stronger than a fully extended building of the tier below and looks
the part, a portacabin becoming a brick block becoming a glass tower.

Polygon-drawn buildings that fill themselves with assets (the Manor Lords model, see
`images/gdd-manorlords-polygon-buildings.png`) are a later upgrade to the placement tool,
not a v1 feature. `images/gdd-skyhaven-ploppable-buildings.png` shows the v1 model.

There is no construction time. Placed is built.

| Building | Provides | Stocks | Vehicles |
|---|---|---|---|
| Control | Inbound holding depth; higher tiers unlock instrument approaches | — | — |
| Goods depot | Cargo handling | throughput buffer (capacity cap) | goods trucks |
| Fuel depot | Refuelling | fuel, litres | fuel trucks |
| Terminal | Passenger handling; gate count and pax-per-hour throughput | — | buses, stairs |
| Pushback depot | Pushback | — | tugs |

Later candidates: mail warehouse, refrigerated goods, hazardous goods, high-value goods,
customs, de-icing, inspection.

**Who owns the cargo:** not the player. Goods belong to shippers and airlines; the airport
moves them and is paid a handling fee. There is no buying, selling, pricing or spoilage
stock. Fuel is the one thing the airport buys and resells.

**Passengers:** counts, not people, inside the terminal. Throughput and gate count set how
fast a flight boards. On the apron, buses, stairs and a short walking stream to the door
are visible, using the same movement system as every other vehicle. Nobody walks indoors.

**Staff:** none. Vehicles are the workers.

---

## 10. Vehicles

Owned by a building, parked at it, on the road only while on a job. Fleet size is an
add-on count. Movement is kinematic on a flat plane over the guideline graph, not physics.
Types: goods truck, fuel truck, pushback tug, passenger bus, stairs, service van, luggage
cart, mail truck. Delivery trucks that fulfil inbound contracts are not the player's; they
enter from the off-map link and leave again.

---

## 11. Finance

An append-only ledger; balance is the sum.

**Income:** landing fees, parking fees, per-service fees (goods, fuel, passengers,
pushback), contract payments, retail and car parking (later).
**Outgoings:** placement, building upkeep, vehicle maintenance, research, fuel purchase,
contract payments, fines, contract cancellation charges.

Fees per service are fixed until research unlocks editable fees. A negative balance locks
placement and research until it recovers; operations continue. Bankruptcy is undecided.

---

## 12. Contracts

**Inbound** contracts deliver a resource at an interval for a duration at a unit price.
Fuel is the first. A **top-up** is a one-shot delivery at a premium for when the player has
miscalculated. **Outbound** contracts are the airline contracts of §6. All contracts expire,
can be renegotiated at expiry, and can be cancelled for a charge. Negotiation is a price
multiplier from research and the other party's reputation; there is no minigame.

---

## 13. Research

A tree of nodes costing money and game time. Branches:

- **Ground services:** fuel depot, pushback, faster pumps, faster loading, vehicle speed.
- **Terminal:** passenger handling, international travel, baggage scanning, automated
  baggage.
- **Airfield:** asphalt, concrete, PAPI, ILS, windsock.
- **Operations:** Control tiers, radar, scheduling, negotiation.
- **Commercial:** editable fees, retail, car parking.

---

## 14. Aircraft

Aircraft are data assets, not prose. Each carries: ICAO wingspan code (A–F), required
runway length, grass capability, passenger and cargo capacity, fuel capacity, range,
performance figures for the movement model, and its two demand lists. Stand size and
taxiway width key off the wingspan code.

The display grouping the player sees:

| Group | ICAO | Examples |
|---|---|---|
| Tiny | A | Twin Otter, PC-12, Caravan, King Air, Dornier 228 |
| Small | B–C | Dash 8, ATR 42/72, Saab 340, E175 |
| Medium | C | A318–A321, 737-700/800/900 |
| Large | D–E | 777, A330, 787, A350 |
| Extra large | E–F | 747, A380, An-225 |

Figures come from a data pass against published sources when the assets are authored; the
original GDD's numbers had several errors and are not carried over.

---

## 15. Controls and camera

No avatar. A free camera with zoom, pan, tilt and orbit. Build tools are keyboard-selected
with mouse placement; every placement previews before it commits and every edit is
undoable.

---

## 16. Out of scope for v1

Polygon-fill buildings, go-around, wind and runway direction, weather, indoor passenger
simulation, bankruptcy, audio design, Steam features, controller support, landside beyond
the off-map road.

## 17. Open questions

- Real minutes per game day; whether schedule slots span midnight.
- Whether the airport's capability is shown as a number or only as an overlay.
- Research cost: money and time, or time only.
- Stand size: strict ICAO A–F or a coarser three-class abstraction over it.
