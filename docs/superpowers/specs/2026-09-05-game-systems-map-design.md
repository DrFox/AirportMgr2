# Game Systems Map — Design

**Status:** design. Names every system the game needs beyond the Airside movement layer,
where each lives, what it depends on, and the order to build them. Defines no code.

**Parents:** `2026-08-28-procedural-road-system-design.md` (surfaces),
`2026-08-29-ground-movement-model-design.md` (guidelines, agents). Both stand. This document
sits above them and adds one system to Airside (§3.8); everything else is new.

**Source:** `docs/AirportManagerGDD.md` (v2, rewritten alongside this spec). The original
~2024 GDD was never committed; §1 records what it got right and wrong so the review outlives
the document.

---

## 0. Nothing here is law

Every decision below was the best call at the time of writing, made on what we knew then.
Building the systems will teach us things the design could not, and some of those lessons
will mean a decision here was wrong. When that happens the decision changes.

The rule is not "the spec is fixed"; the rule is "changes are discussed and made for the
right reason". A change is legitimate when it comes from new knowledge — a mechanism that
does not work as assumed, a cost that turned out higher, a player-facing result that is
worse than expected — and when the reasoning is written down where the old decision was.
A change is not legitimate when it is drift: quietly narrowing a system because it was
hard, or widening it because it was fun. Say what changed, why, and what it replaces. Then
change it. Nothing in this document is immune to that.

This is the same habit the codebase already keeps at the code level ("name the pattern,
then justify the deviation"). It applies to the design too.

---

## 1. Review of the original GDD

### 1.1 What holds up

- **Infrastructure demand vs service demand.** Hard gate vs paid-if-fulfilled. This is the
  literal data model of a flight (§3.2), not a metaphor.
- **Demand → Job → vehicle.** The GDD's job manager is the right shape. Assignment is
  changed from broadcast to request-response (§3.5) but the decomposition stands.
- **Kinematic vehicles over a junction graph, no navmesh, no Chaos.** Built, tested, in
  `main`. The GDD called it two years before it existed.
- **Contracts as a planning tool.** Certainty about future inflow and outflow is the
  management game; keep it.
- **The first-15-minutes walkthrough.** It is the vertical slice (§5.3, M3) almost verbatim.

### 1.2 Holes, and what this spec does about each

| # | Hole | Resolution |
|---|---|---|
| 1 | No time model | §2.1 sim clock, first thing built |
| 2 | Passengers undesigned | Hybrid: abstract in terminal, visible agents on apron (§3.7) |
| 3 | Cargo ownership open | Airport is a handler, never a trader (§3.6) |
| 4 | Airlines are empty headers | Roster + reputation + offer generator (§4.1) |
| 5 | No rule behind "an aircraft wishes to land" | Capability → eligible pool → weighted rate (§4.1) |
| 6 | Two contradictory turnaround rules | Scheduled off-block time is the target; late = fine (§3.2) |
| 7 | No failure pressure | Late departures, diversions, reputation floor, unreachable depots |
| 8 | Job system underspecified | Priority, bidding, N-trip jobs, ordering, stuck recovery (§3.5) |
| 9 | Management Building vs ATC Tower overlap | One `Control` building type with tiers (§3.3) |
| 10 | Runway, taxiway and road occupancy silent | §3.3 sequencing policy, §3.8 occupancy mechanism |
| 11 | Aircraft classed by feel | ICAO wingspan code A–F + runway length on the data asset (§2.2) |
| 12 | Three overlapping upgrade mechanisms | Ploppable + modular add-ons; polygon-fill deferred (§3.6) |
| 13 | Stands: GDD polygon vs code sized-stand | Code wins; GDD v2 follows it |
| 14 | UI, save/load, new game, landside, weather, milestones, staff all absent | §5.1, §2.3, §5.2; weather out of v1; no staff |
| 15 | Stale roadmap; 1.4 MB of inline images | GDD v2 drops the roadmap; the two reference screenshots live in `docs/images/` |

### 1.3 Decisions taken during review

Recorded here so the reasoning survives.

- **Passengers: hybrid.** A people-sim (Airport CEO) is a whole second game. Abstract counts
  alone (Transport Fever) leave the apron dead. Buses, stairs and a short pedestrian stream
  on the apron guideline give visible life using agents the game already has.
- **Cargo: handler.** Goods belong to shippers and airlines. The airport moves them and is
  paid a fee. No prices, no market, no spoilage stock. Fuel is the one consumable the
  airport buys and resells, because refuelling is a service the airport physically provides.
- **Buildings: ploppable with add-on modules.** Polygon-fill (Manor Lords) is a new solver
  plus a content pipeline; it is the most expensive line in the GDD and gates nothing else.
  Add-ons give the "grows with the airport" feel at a fraction of the cost. Polygon-fill
  stays on the list as a later upgrade to the placement tool, not a prerequisite.
- **Turnaround: target off-block time.** "Waits the time it indicated" and "leaves when
  jobs complete" cannot both be true. A scheduled time the jobs must beat is the only one
  of the two that produces pressure.
- **Offers early, schedules late.** Ad-hoc offers are the tutorial; a slot-by-stand grid is
  the late game. GA keeps trickling into the inbox throughout so the grid never fully
  replaces the inbox.
- **Clock: compressed 24-hour day, speed ×0/1/2/4/8.** A day cycle gives schedules,
  upkeep ticks and contract deliveries a natural period and the world a sky.
- **Save/load in the first milestone.** Not because saving is urgent but because the
  saved-state rule (§2.3: non-Transient UPROPERTYs are saved) must hold from every model
  class's first commit. Retrofitting is how saves break.
- **Staff: none.** Vehicles are the workers. A staff system doubles the agent count for
  no new decision the player makes.

---

## 2. Foundation

All in `AirportOps/Model/` unless stated. World-free, `NewObject`-testable.

### 2.0 Module layout

New plugin **`AirportOps`** beside `Airside`. Dependency chain:

```
AirportMgr (game module)  →  AirportOps  →  Airside
```

Airside never includes AirportOps. AirportOps mirrors Airside's internal layering:
`Model/` (world-free UObjects), `Present/` (views, UMG bindings), `Tool/` (placement).
`Tools/Check-Architecture.ps1` gains the cross-plugin rule.

Why a plugin and not directories inside Airside: movement is a finished, tested subsystem
with a bitwise welding contract and a sample-once guideline contract. Money, jobs and a
clock touching it would blur what its tests defend. The seam already exists
(`UAirsideTraffic::DispatchArrival`, `EAgentPhase`, `EArrivalRefusal`); AirportOps drives
that seam and subscribes to it. Why not the game module: no headless test module of its
own, and the composition-root-swallows-everything problem returns one level up.

**Airside additions the seam needs** (the only changes to Airside outside §3.8):

- Phase-transition events: agent entered `Parked`, agent `Gone`, arrival refused with its
  `EArrivalRefusal`. Today these are log lines only.
- Agent primitives: `RedirectAgent(id, plan)` and `RetireAgent(id)` beside the existing
  `DispatchAgent`, plus a stable per-agent id. *Amended 2026-09-05 (M1):* the first draft
  asked Airside for "go to anchor, dwell, return" as one call. Dwell is a fact about the
  job, so that shape belongs in the job board; Airside offers the three primitives and the
  phase events, and AirportOps composes them.
- A read-only capability query over the network (§3.1): longest runway and its surface,
  stands with size class and anchors. Pure function of the graph; lives in Airside `Model/`.

### 2.1 Sim clock — `USimClock`

- Game seconds as `double`; `SpeedIndex` into the fixed table `{0, 1, 2, 4, 8}`. Real
  minutes per game day is a tunable on a settings object, never a constructor literal.
- API: `Now()`, `TimeOfDay()`, `Day()`, and a scheduler — `At(GameTime, Callback)`,
  `Every(Interval, Callback)`. One `Advance(DeltaSeconds)` drains every due callback in
  time order; a large step drains several.
- Nothing sim-side reads wall time or raw `DeltaSeconds`. Airside's
  `UAirsideTraffic::Advance(float, double)` stays wall-scaled; the game module multiplies by
  speed before calling it. Airside never learns the clock exists.
  *Clarified 2026-09-05 (M1):* "speed" is the ×0/1/2/4/8 multiplier only. Day compression
  (real seconds per game day) scales the clock, never movement; a truck driving 72× faster
  because the day is twenty real minutes would be unwatchable. The multiplier reaches
  Airside through `ARoadNetworkActor::SetSimTimeScale`, applied once in its `Tick`.
- UE: a `UGameInstanceSubsystem`. Subsystems are engine-managed singletons scoped to a
  lifetime (engine / game instance / world / local player). Game-instance scope survives
  level loads, which the save/load flow needs. Tests build one with `NewObject` and never
  touch the subsystem collection.

### 2.2 Definitions — data assets and `UOpsCatalog`

- Everything the GDD lists as prose becomes a `UPrimaryDataAsset` subclass. `UAircraftType`
  exists in Airside and stays there. New: `UVehicleType`, `UBuildingType` (footprint, road
  anchor, capacity, add-on slots, service roles), `UAirlineDefinition`, `UCargoClass`,
  `UResearchNode`, `UContractTemplate`, `UScenario`.
- `UOpsCatalog` resolves them through the Asset Manager by primary asset type. Same rule as
  `UAirsideSettings::Resolve*`: one function per default, no literal asset paths elsewhere.
- `UAircraftType` gains ICAO wingspan code (A–F) and required runway length. Stand size
  class and taxiway width key off the code. The GDD's Tiny/Small/Medium/Large/XL becomes a
  display grouping, not a rule.

### 2.3 Save/load

- Model objects are UObjects with `UPROPERTY(SaveGame)` fields;
  `FObjectAndNameAsStringProxyArchive` with `ArIsSaveGame` serialises them without
  per-class code. `URoadNetwork`'s slot maps save with their generation counters, so
  handles stay valid across a load.
- Snapshot contents: clock, network, buildings, inventory, fleet, flights, ledger,
  contracts, research state. Agents mid-taxi save as flight state plus guideline position
  and are re-dispatched on load. Views are never saved; `Present/` rebuilds from model,
  as the road mesh already does.
- *Amended 2026-09-05 (M1):* the archive does not set `ArIsSaveGame`.
  `FProperty::ShouldSerializeValue` (Property.cpp:1052) would then skip every untagged
  property, nested struct members included, so honouring "tag every field" meant tagging
  all of Airside. Rule instead: **a model object's non-Transient UPROPERTYs are its saved
  state**; mark what must not be saved `Transient`. Forgetting a Transient saves one field
  too many (visible); forgetting a SaveGame would lose one (silent).

### 2.4 Events — `UOpsEvents`

- One bus, typed per event: `OnFlightPhaseChanged`, `OnJobStateChanged`, `OnLedgerEntry`,
  `OnContractEvent`, `OnResearchUnlocked`, `OnNotification`. UI, audio and the feed
  subscribe; model publishes and never knows who listens.
- Pattern: Observer via dynamic multicast delegates so Blueprint UI can bind.
- Deviation, named: the GDD's job pub/sub is **not** this bus. Job assignment is
  request-response (§3.5). The bus announces outcomes only.

---

## 3. Operations

### 3.1 Airport capability — `FAirportCapability`

Pure function of network + placed buildings: longest usable runway and surface; stands
with ICAO class and their service anchors; service roles offered by buildings with a road
route to at least one stand; landing-queue depth from the Control building. Recomputed on
`URoadEditFacade` mutation events and building placement; cached; read by offer generation,
admission and the "what can I handle" overlay. The graph half is an Airside `Model/` query;
AirportOps joins in the building half.

### 3.2 Flight lifecycle — `UFlight`, `UFlightBoard`

- `EFlightPhase`: `Offered, Accepted, Inbound, Landing, TaxiIn, Turnaround, Pushback,
  TaxiOut, Departing, Departed` plus terminal failures `Declined, Diverted, Cancelled`.
  Airside `EAgentPhase` maps: `Arriving→Landing`, `Taxiing→TaxiIn/TaxiOut`,
  `Parked→Turnaround`, `Departing→Departing`, `Gone→Departed`. The flight owns the agent
  handle while one exists.
- `UFlight`: aircraft type, airline, arrival time, scheduled off-block time, demands
  (infrastructure and service, each required or optional), stand, jobs, fee schedule.
- `UFlightBoard`: owns live flights, advances phases on clock and Airside events; the only
  caller of `DispatchArrival` and the departure taxi.
- Pressure: off-block time passes with **required** jobs incomplete → late departure, fine,
  reputation hit. With **optional** jobs incomplete → those fees unpaid, leaves on time.
  Inbound patience: hold beyond a threshold with no stand or runway → `Diverted`.

### 3.3 Runway and airspace sequencing — `URunwaySequencer`

- Arrival and departure queues per runway. Landing clearance needs the runway free in
  §3.8's occupancy table and an `ArrivalPlanner` plan; take-off clearance needs the runway
  free and no arrival inside a separation window.
- Queue depth is the Control building's figure. GDD's Management Building and ATC Tower
  collapse into one `Control` building type; tiers raise depth and later unlock instrument
  approaches as infrastructure demands.
- Holding is abstract (off-map timer, no orbiting agents). Go-around: out of v1.
  Wind and runway direction: out of v1; a research hook.

### 3.4 Stand allocation — `UStandAllocator`

First-fit by ICAO class, smallest stand that fits, nearest exit as tiebreak. Reserve at
`Accepted`, release at `TaxiOut`. The schedule grid (§4.5) supersedes it for contracted
flights.

### 3.5 Turnaround jobs — `UJobBoard`

- On `Turnaround`, each service demand expands to `FJob`s: role, stand anchor, quantity,
  duration model, prerequisites, deadline (= off-block time). Ordering is data on the
  demand template: unload before load, pushback last, no fuelling during boarding.
- **Assignment is request-response.** The board asks each depot offering the role for a
  bid (free vehicle, ETA via `RouteSearch`); lowest ETA wins, ties by stock. A job larger
  than one vehicle load is N trips on one job — the fuel-truck-capacity case is a loop,
  not a special case.
- Vehicle sequence: depot → anchor → dwell → depot → unload/reload; driven by the Airside
  anchor dispatch (§2.0), advanced by Airside's arrival event.
- Stuck recovery: no movement for a threshold → reassign and log. The log line is the
  feature.

### 3.6 Buildings, fleet, inventory

- `UBuildingInstance`: type, transform, road anchor node, add-on modules, owned vehicles,
  inventory if the type stocks anything. Placement reuses apron/stand tool machinery and the
  same undo history. No road anchor on the guideline graph → inert, and it says so in the
  log and the UI.
- Vehicles belong to a building, park there, and exist as Airside agents only while on a
  job. Fleet size is an add-on count.
- Inventory is per building per resource. Fuel litres now; de-icing fluid later. Goods are
  a throughput buffer with a cap (handler decision), never a stock.

### 3.7 Passengers — hybrid

Terminal is a building with pax-per-hour throughput and a gate count. A pax demand is one
job pair (disembark, board), each duration = pax / throughput share. Visible agents: a bus
or stairs vehicle to the anchor and a short pedestrian stream on the apron guideline.
Nothing walks indoors.

### 3.8 Ground traffic — occupancy, arbitration, congestion-aware routing (**Airside** `Model/`)

The one new system inside Airside. `FRouteFollower`'s own header names it as the next
thing owed: "two aircraft pass straight through each other, hold-short nodes are not
consulted, and the right-of-way rules the graph already carries are ignored."

- **Mechanism vs policy.** Airside owns `FTrafficOccupancy`: who holds which guideline
  edge, node and runway surface, plus reservations a short way ahead. `URunwaySequencer`
  (§3.3) is policy — it decides who is next and asks occupancy for the grant. Runway
  occupancy is one more surface in the same table, so a taxiway crossing a runway and a
  landing share one rule.
- **Reservation window.** Each agent reserves the edges and nodes inside its braking
  distance plus a class separation gap. `FSpeedProfile` already plans speed for the route;
  occupancy adds one input per tick — stop-within distance to the first unreserved
  resource. Aircraft on a taxiway get car-following separation for free: the agent ahead's
  reservation is the stop point.
- **Nodes are the conflict points.** Junctions and crossings grant one agent at a time.
  Order: `PriorityOverride`, then class priority (aircraft over vehicles — already on the
  graph), then first-to-reserve. Hold-short nodes reserve the surface named by
  `HoldShortFor`; if held, the agent stops at the line. That is the whole hold-short rule.
- **Roads.** Lanes are separate guidelines, so opposing flows never conflict. Road–taxiway
  crossings are nodes under the node rule; vehicles yield to aircraft by class priority.
- **Routing.** `RouteSearch` gains an occupancy cost term, so vehicles route around jams at
  plan time. Aircraft routes are fixed at clearance (as a real taxi clearance is) and may
  replan only while stopped at a node — an aircraft rerouting mid-edge leaves the line the
  player was shown, which is the invariant the guideline graph exists to protect.
- **Deadlock.** Reservations form a wait-for graph; a cycle is a deadlock. The
  lowest-priority waiter releases and replans. Aircraft never reverse except pushback, so an
  all-aircraft cycle (two-way single taxiway, two aircraft) is a layout the build tool
  should warn about. v1 logs it and diverts the later arrival; the tool warning is a later
  slice.
- **Tests, world-free.** Two agents converge on a node; one yields. An aircraft holds
  short while the runway is occupied and proceeds on release. Three vehicles in a cycle
  resolve without teleporting. Measured, not narrated.

---

## 4. Economy and progression

### 4.1 Demand generation — `UOfferGenerator`, `UAirlineRoster`

- `UAirlineDefinition`: fleet (aircraft types), route range, service profile, reputation.
  The generator runs on the clock scheduler: per airline, filter fleet against
  `FAirportCapability`, weight by reputation and range, emit `Offered` flights. Rate scales
  with capability score — growing the airport is what busies the inbox.
- Reputation moves on outcomes only: on-time up; late or unpaid optional down; divert
  sharply down. Below a floor the airline stops offering until a recovery timer expires.
- GA and charter are airlines with a one-type fleet and no contract path.

### 4.2 Finance — `ULedger`

Append-only entries (game time, category from the GDD's lists, amount, source id); balance
is a fold. Upkeep, maintenance and contract payments are clock callbacks that post entries.
Fees per service live on the demand template; editable after the fee research node.
Negative balance locks placement and research until positive; operations continue.
Bankruptcy: a later decision.

### 4.3 Contracts — `UContractBook`

- **Inbound:** resource, quantity, interval, duration, unit price. Each delivery is a
  scheduled truck from the external connection through the road network to the depot — the
  landside road matters. Top-up is a one-shot at a premium. Unreachable depot → missed
  delivery, and the notification says why.
- **Outbound:** an airline contract fixes flights per day, service set, fees, fine per late
  departure. Its flights bypass the inbox and land in the schedule grid (§4.5).
- Expiry, renegotiation, cancellation: three transitions on one `EContractState`.
  Negotiation is a price multiplier from a research node and reputation. No minigame.

### 4.4 Research — `UResearchTree`

Nodes are data assets: prerequisites, currency cost, game-time cost, unlocks (building
types, add-ons, editable fees, scheduling, Control tiers, surfaces). Branches: Ground
Services, Terminal, Airfield, Operations, Commercial. Research completes via the clock
scheduler like everything else.

### 4.5 Flight scheduling — `USchedule`

Research-unlocked. A stand × time-slot grid for one game day. Contracted flights are placed
by the player or auto-placed first-fit; the board reads the grid for `Accepted` flights
instead of the allocator. Ad-hoc offers keep using the allocator on stands the grid leaves
free.

---

## 5. Presentation, meta, build order, testing

### 5.1 Presentation (`AirportOps/Present/`, game module)

- UMG bound to `UOpsEvents` and read-only model views. No widget mutates the model; every
  action goes through a facade, as `URoadEditFacade` already enforces. One door for undo,
  save and tests.
- Screens in build order: HUD (clock, speed, balance, feed); build palette (tool registry +
  buildings); offer inbox; flight and stand panels; finance; contracts; research; schedule
  grid. Overlays: capability heatmap; occupancy debug draw extending `RoadDebugDraw`.
- `ARoadAgentActor` stays the one agent view; mesh per definition asset. Buildings: one
  actor per instance, add-ons as child components.
- Audio: out of scope. Hook is the event bus.

### 5.2 Meta

New game from a `UScenario` asset: flat plot, one pre-placed external road node, starting
balance, starting unlocks. Difficulty is that asset's numbers. Settings, Steam, controller:
out of v1.

### 5.3 Build order

Each milestone is one spec + one plan with a playable result.

| M | Deliverable | Systems |
|---|---|---|
| 1 | Clock, catalog, save/load, events, Airside seam (phase events, anchor dispatch, capability query) | §2.0–2.4 |
| 2 | Ground traffic: occupancy, arbitration, hold-short, congestion routing | §3.8 |
| 3 | First 15 minutes: offers, flight board, sequencer, allocator, goods depot + fleet, jobs, ledger, HUD, inbox | §3.1–3.6 subset, §4.1 subset, §4.2 |
| 4 | Fuel + inventory, inbound contracts, research, Control tiers, remaining buildings, finance + contract screens | §3.6 rest, §4.3, §4.4 |
| 5 | Passengers hybrid, airline contracts, schedule grid, reputation loop closed | §3.7, §4.1 rest, §4.5 |

M1 first because everything keys off the clock and `SaveGame` tagging starts at commit one.
M2 before M3 because the first time two aircraft share the field they pass through each
other.

### 5.4 Testing

- Every `Model/` system: world-free tests with `NewObject`, filters `Airside.*` /
  `AirportOps.*`, run by `Run-AirsideTests.ps1`.
- Every seam: a composition test that fails if unwired. Flight board receives `Parked` from
  a real `UAirsideTraffic` tick; a job completes on the vehicle's Airside arrival event; a
  saved-and-reloaded network yields the same arrival plan.
- Determinism: same seed, same inputs → same ledger. This is the test that keeps wall time
  out of the sim.
- `Check-Architecture.ps1`: AirportOps includes Airside, never the reverse; Airside
  `Model/` still includes nothing above it.

---

## 6. Out of v1, recorded so they are not forgotten

Polygon-fill buildings; go-around; wind and runway direction; weather; indoor passenger
simulation; bankruptcy; build-tool warning for all-aircraft deadlock layouts; audio; Steam;
controller support; landside beyond the external road (car parks as a revenue line).

## 7. Open questions

- Day length in real minutes, and whether flights span midnight in the schedule grid.
- Whether the capability score is exposed to the player as a number or only as the
  heatmap.
- Whether research cost is currency + time or time only (currency then goes to buildings).
- Stand size classes: strictly ICAO A–F, or a coarser three-class game abstraction over it.
