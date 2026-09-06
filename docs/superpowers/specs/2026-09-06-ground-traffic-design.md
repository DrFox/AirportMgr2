# Ground Traffic — Design

**Status:** design. Milestone 2 of `2026-09-05-game-systems-map-design.md` §3.8 and §5.3.
The one new system inside Airside; everything lands in `Plugins/Airside`, `Model/` first.

**Goal:** two aircraft on the same field no longer pass through each other. An aircraft
holds at a player-placed hold-short bar while another uses the runway. Vehicles queue
behind each other, yield to aircraft at crossings, route round jams, and a gridlock of
vehicles clears itself or clears when the player builds a way out.

**Nothing here is law** — §0 of the systems map applies. Where this document differs from
§3.8 it says so and records why, and §3.8 is amended in place to point here.

---

## 1. Decisions

| Question | Decision | Why |
|---|---|---|
| Shape | One reservation table (`FTrafficOccupancy`), a priority-ordered tick over all agents. | One table answers junctions, hold-short, car-following, head-on, the runway grant M3 needs, and the routing cost. Local sensing (each agent scanning for others) answers car-following only and cannot detect a deadlock, because nobody records who waits on whom. |
| Edge granularity | Interval: an agent holds a distance range on each edge it touches. | Spec 5.7 deferred interval-vs-block to this slice. Block occupancy makes a 1 km taxiway hold one aircraft and needs interior nodes the graph deliberately lacks. Interval gives car-following and head-on detection for free. |
| Where the tick lives | New `Model/` class `UGroundTraffic` owns agents, ids, the table and the tick. `UAirsideTraffic` (`Present/`) shrinks to a view registry and forwarders. | The three milestone tests must run with `NewObject` and no world. Today every multi-agent test calls `UWorld::CreateWorld` because the agent list is in `Present/`. Free functions over a hand-built agent array were considered and rejected: the ordering that makes arbitration work would then be untested without a world. |
| Hold-short authoring | Player-placed with a build-bar tool, never derived by the builder. | User decision. A hold bar is a piece of airport design, not a consequence of geometry. Nothing writes `HoldShortFor` today; spec 5.5's "the topology already exists" is true of the node, not the flag. |
| Hold-short tool scope | In M2. | Without it the runway rule exists only in tests. |
| Reservation window | Braking distance from current speed plus a per-class gap. | A fixed lookahead under-reserves a fast aircraft and over-reserves a stopped one. |
| Deadlock resolution | Lowest-ranked waiter replans at a node with the blocked edge banned. No reversing. | Every case reversing fixes is also fixed by the player adding an exit, which §5 picks up. Reversing needs the follower to walk backwards with a heading rule; deferred as a v2 option, not a v1 debt. |
| Unresolvable waiters | Stay waiters. Retry on a sim-time cadence and on every graph rebuild. | The player's fix must be picked up. "Logged once and left" was the first wording and was wrong. |
| Graph rebuild | Re-resolve every agent's steps by position; replan what fails; clear the table. | A road edit regenerates the guideline graph with new handles. Today agents survive only because the follower never reads a handle. A handle-keyed table would go stale on every edit. |
| Aircraft routes | Fixed at clearance against congestion. Replanned only while stopped at a node: deadlock, or a rebuild that broke the route. | Amendment to §3.8, which said replan "only while stopped at a node" without naming the rebuild case. Freezing an aircraft whose taxiway was deleted leaves it on air. |

Rejected outright: time-windowed reservations (resource × time interval, CBS-style). Correct
and heavy; the speed profile would have to be honoured in time as well as distance. Named so
it is not rediscovered.

---

## 2. Components and ownership

```
ARoadNetworkActor
  └─ UAirsideTraffic        Present/  view registry TMap<AgentId, ARoadAgentActor*>; forwards
       └─ UGroundTraffic    Model/    agents, ids, occupancy, the tick, the delegates
            ├─ TArray<FRoadAgent>      Id moves onto FRoadAgent; FAgentSlot goes
            ├─ FTrafficOccupancy       the table
            └─ FTrafficRules           gap and footprint per class, cadences
```

Pattern: Mediator, moved down one layer. The view registry is a plain map, not a second
mediator. Named deviation: `FAgentSlot` dies because a `Model/` class cannot hold a view
pointer, and the agent list must be `Model/` for the tests to be world-free.

Both `UAirsideTraffic` and `UGroundTraffic` are `Transient` subobjects created in the
constructor and re-pointed in `PostInitProperties` — the PIE-duplication rule M1 learned.

### 2.1 `FTrafficOccupancy` — `Model/TrafficOccupancy.h`

```cpp
enum class ETrafficResourceKind : uint8 { Edge, Node, Surface };

struct FTrafficResource
{
    ETrafficResourceKind Kind;
    FGuidelineEdgeId Edge;     // Kind == Edge
    FGuidelineNodeId Node;     // Kind == Node
    FRoadSegmentId   Surface;  // Kind == Surface: one segment of a runway chain
};

struct FTrafficClaim
{
    int32 AgentId;
    FTrafficResource Resource;
    double From, To;           // edge distance, Edge kind only; 0..0 otherwise
    bool bOccupied;            // contains the agent's own position; never preemptable
};

enum class EClaimResult : uint8 { Granted, Held };

struct FTrafficOccupancy
{
    EClaimResult TryClaim(const FTrafficClaim& Claim, int32 Rank, int32& OutHolder);
    void Release(int32 AgentId, const FTrafficResource& Resource);
    void ReleaseBehind(int32 AgentId, const FGuidelineEdgeId& Edge, double Before);
    void ReleaseAll(int32 AgentId);
    double HeldLengthOn(FGuidelineEdgeId Edge, int32 ExcludingAgent) const;
    bool IsHeld(const FTrafficResource& Resource, int32 ExcludingAgent) const;
    void Clear();
};
```

Rank is decided by the caller (§3.3); the table only compares. A claim on a resource whose
existing claim is `bOccupied` is `Held` regardless of rank. Otherwise a higher rank preempts:
the old claim is removed and its owner refuses on its own next claim pass. Two edge intervals
conflict when they overlap; two node or surface claims always conflict.

A runway surface is one `FTrafficResource` per segment of its chain. A landing or take-off
claims every segment; a hold-short node claims the one it names. `IsHeld` on any segment of
the chain is the grant `URunwaySequencer` (M3) will ask for.

### 2.2 `UGroundTraffic` — `Model/GroundTraffic.h`

Takes over from `UAirsideTraffic`: `DispatchArrival`, `DispatchAgent`, `RedirectAgent`,
`RetireAgent`, `ClearAgents`, `Advance`, `OnAgentPhaseChanged`, `OnArrivalRefused`,
`GetNewestAgentId`, `GetAgentCount`, the `*ForTest` accessors. Gains:

```cpp
void Advance(double DeltaSeconds, const URoadNetwork& Network);
void OnGraphRebuilt(const URoadNetwork& Network);
const FTrafficOccupancy& GetOccupancy() const;
const FRoadAgent* FindAgent(int32 AgentId) const;
UPROPERTY(EditAnywhere) FTrafficRules Rules;
```

The network is passed per call, never held: the class stays world-free and cannot outlive
a graph. `DispatchAgent` gains an `ETraversalClass` parameter; the arrival path sets
`Aircraft`.

### 2.3 `FTrafficRules`

```cpp
struct FTrafficRules
{
    double FootprintUU[4];     // per ETraversalClass: aircraft ~ one fuselage, vehicle ~ 5 m
    double GapUU[4];           // separation kept ahead, per class
    double CongestionWeight = 2.0;
    double StallSeconds = 3.0;     // stopped-and-waiting before deadlock detection looks
    double RetrySeconds = 5.0;     // unresolvable waiter re-plans this often
    double ResolveRadiusUU = 25.0; // rebuild: how near a live node must be to a step end
};
```

Footprint lives here and not on `FAirframe` because the airframe has no length figure and a
second copy is the drift rule. Per-type lengths are a later refinement with one owner.

### 2.4 `FRoadAgent` additions

| Field | Written by | Read by |
|---|---|---|
| `int32 Id` | `UGroundTraffic::Admit` | everyone; replaces `FAgentSlot::Id` |
| `ETraversalClass Class` | dispatch | rank, footprint, gap, routing |
| `FGuidelineNodeId GoalNode` | dispatch, redirect | replan |
| `double StopWithin` | arbitration, every tick | `FRouteFollower::Advance` |
| `int32 WaitingOn` | arbitration | deadlock detection; 0 = none |
| `double StalledSeconds` | `Advance` | deadlock detection |
| `double LastResolveAttempt` | resolver | retry cadence |
| `int32 BlockedStep` | arbitration | the step whose resource refused; −1 when none. Names the node a replan starts from |
| `TArray<FRoadSegmentId> RunwayHeld` | dispatch, departure handover | the chain an Arriving or Departing agent occupies each tick |

Arbitration writes, motion reads. There is no second evaluator of where the agent may go.

### 2.5 `FRouteFollower::Advance(DeltaSeconds, StopWithin, OutPosition, OutHeading)`

One more cap on the target speed: `sqrt(2 · Taxi.Decel · StopWithin)`, and `Travelled` is
clamped so it never passes `Travelled + StopWithin` as measured at the start of the tick.
`FSpeedProfile` is untouched. `StopWithin` unbounded (`TNumericLimits<double>::Max()`) is
exactly today's behaviour, which is what every existing follower test passes.

### 2.6 `FRouteStep::EndDistance`

Cumulative route distance at which the step's edge ends, filled by `RouteSearch::RunSearch`
from the same polyline it appends. This is the map from `Travelled` to "which edge am I on,
which node is next". Filled from the polyline the follower walks, so it cannot disagree with
it — the sample-once rule.

### 2.7 `URoadNetwork` additions

- `TArray<FRoadSegmentId> RunwayChain(FRoadSegmentId Seed) const` — every segment continuous
  with `Seed` through junctions under a runway profile.
- `bool IsRunwaySegment(FRoadSegmentId) const` — the profile rule
  (`bContinuousThroughJunctions`) in one place.
- `RunwayExtentAt` / `NearestRunwayThreshold` gain a trailing `FRoadSegmentId* OutSegment =
  nullptr`, carried on `FArrivalPlan::RunwaySegment` and `RunwayChain`.
- `FRoadSegmentId RunwayNearGuidelineNode(FGuidelineNodeId) const` — the runway an edge
  incident to this node, or to a neighbour, derives from; unset when none. What the
  hold-short tool asks.
- `HoldShortMarks` and `bool SetHoldShort(FGuidelineNodeId, FRoadSegmentId Protects)` — §6.
- `EArrivalRefusal::RunwayOccupied`.

---

## 3. Resources, the window, the tick

### 3.1 What an agent holds

- **Edge interval** `[Travelled − Footprint/2, min(Travelled + Window, edge end)]` in route
  distance, mapped onto the edge through `EndDistance`. Continues onto following edges while
  the window has distance left. Half the footprint behind, because `Travelled` is the
  agent's CENTRE: a vehicle 300 uu past a node with a 500 uu footprint has cleared it.
- **Node**: every `Steps[i].To` whose `EndDistance` lies inside the window, and the node the
  current step LEFT while `Travelled` is still within `Footprint/2` of it. Exclusive. A node
  is *occupied* (never preemptable) while the agent's centre is within `Footprint/2` of it.
- **Box-junction entry rule.** When the window reaches the START of a step shorter than
  `Footprint + Gap` — an edge the agent cannot stand on without still blocking the node
  behind it, which is what every junction turn path is — the step's END node must be
  granted as well, and a refusal stops the agent `Gap` short of the step's start rather than
  inside it. Only at entry: once inside, a blocked end node stops the agent short of that
  node like any other. The alternative, extending the requirement through every consecutive
  short step, was traced by hand on the three-vehicle triangle and deadlocks HARDER — an
  agent then refuses to move until a node two junctions ahead is free, and the agent holding
  that node is waiting on it. Traced, not measured; the triangle test measures it.
- **Surface**: a runway chain, by three routes to one rule:
  - an edge whose `DerivedFrom` is a runway segment implies the chain;
  - a hold-short node claims the chain its `HoldShortFor` names, so the stop is at the bar,
    not at the runway edge;
  - `StartArrival` claims the chain and `Vacated` releases it; a departure holds it from
    entering the runway edge until `Gone`.

`DispatchArrival` refuses with `RunwayOccupied` while the chain is held.

### 3.2 Window

`Window = Speed² / (2 · Taxi.Decel) + Gap[Class]`. A stopped agent still holds
`Footprint + Gap`, so a parked queue keeps its spacing.

### 3.3 Occupied versus reserved, and rank

A claim containing the agent's own position is **occupied** and can never be taken from it.
Everything ahead is **reserved** and a higher-ranked claimant preempts it.

Rank at a resource: if the node carries `PriorityOverride`, the class's index in that list;
else `TraversalPriority(Class)`. Ties keep the existing holder; among fresh claimants the
lower id wins. That is §3.8's "override, then class, then first-to-reserve", with the
guarantee that nobody is evicted from a node they are standing in.

### 3.4 The tick — `UGroundTraffic::Advance`

1. Order agents by rank (class, then id). Only `Taxiing` agents claim; other phases hold
   their surface and nothing else.
2. Per agent in that order: `ReleaseBehind(Travelled − Footprint)`, then claim forward along
   the steps. First refusal: `StopWithin` = distance to that resource's start,
   `WaitingOn` = holder. All granted: `StopWithin` unbounded, `WaitingOn` = 0.
3. Per agent: `FRoadAgent::Advance(Delta)` as today, the follower capped by `StopWithin`.
   `StalledSeconds` accrues while `Speed == 0 && WaitingOn != 0`, else resets.
4. Deadlock pass (§5).
5. Phase-change broadcasts, as today, plus view spawn/destroy in `UAirsideTraffic` on
   those events instead of inline.

Two passes so a claim by one agent is visible to every later agent in the same frame.
Sorting by rank rather than list order is what makes aircraft-over-vehicle true when both
reach a node in one frame.

---

## 4. Routing

`FRouteQuery` gains `const FTrafficOccupancy* Occupancy = nullptr` and
`int32 QueryingAgent = 0`. Edge cost becomes
`Length + Rules.CongestionWeight · HeldLengthOn(edge, QueryingAgent)`. Nodes are not costed:
a held node is a moment, a held edge is a queue. The heuristic stays straight-line and stays
admissible, because the added term is non-negative. Null occupancy is the old search
bitwise; no existing route test moves.

**Who routes with it.** Vehicles always. Aircraft never at dispatch (fixed at clearance);
aircraft replan only via `ReplanFrom` below, in the deadlock and rebuild cases.
`RedirectAgent` is AirportOps's decision and is unchanged.

**`UGroundTraffic::ReplanFrom(agent, node, bannedEdge)`.** Precondition: the agent is stopped
with `node` the next step's `To`. Search `node → GoalNode` with the ban and the occupancy
cost; splice the polyline from `Travelled` to `node` onto the new plan's polyline; restart
the follower on the spliced plan keeping `Speed` and `Heading`, so no jump. Releases the
agent's reservations ahead. Logged with old and new remaining lengths.

---

## 5. Deadlock

**Detection.** Each tick, for every agent with `StalledSeconds > Rules.StallSeconds`: follow
`WaitingOn`. Reaching an agent already on the path is a cycle. A cycle is identified by its
lowest member id, so it is reported and resolved once per tick, not once per member.

**Resolver.** *Refined 2026-09-06 while planning.* A member can replan only if it is stopped
AT the node where the edge it was refused begins — within `Gap + Footprint/2` short of that
node and not past it — because the alternative is another edge OUT of that node, and a
waiter that has already entered the edge cannot take it without reversing. Every member
records `BlockedStep`, the index of the step whose resource refused it, so the node is
`FromNode(BlockedStep)` and the ban is `Steps[BlockedStep].Edge`. Among the members that
qualify, the candidate is the lowest-ranked: lowest class priority, ties to the highest id
(the later arrival).

- **Its replan finds a route:** `ReplanFrom` with the ban and the occupancy cost. Spliced
  at that node, so it drives the remaining `Gap` on the old line and turns onto the new.
- **No member qualifies, or the candidate's replan fails:** the cycle stays. Each member's
  `LastResolveAttempt` is stamped; retried every `Rules.RetrySeconds` and on every graph
  rebuild. Logged once with the members.
- **All aircraft:** the same rule; the highest id is the candidate. Logged at Warning as
  an all-aircraft cycle: the input to the future build-tool warning (systems map §6).

Mid-edge waiters need no case of their own: §3.1's box-entry rule is what makes a gridlock
form with its members AT nodes, where they can turn, rather than inside the junction, where
nobody can.

---

## 6. Graph rebuild

**Signal.** `ARoadNetworkActor::RebuildMesh` calls `Traffic->OnGraphRebuilt(*Network)` after
the guideline builder has run. Nothing in `Model/` subscribes to anything; the call comes
down the same forwarder chain as the tick.

**Re-resolve.** For each agent with a plan:

1. For each remaining step, the live node nearest the step's end position within
   `ResolveRadiusUU`, class-filtered (`RouteSearch::FindNearestNode`), and the live edge
   between consecutive resolved nodes admitting the class. Polyline and `EndDistance` are
   untouched; the follower never notices.
2. First step that fails: replan from the last resolved node to `GoalNode` and splice
   (§4). A goal that no longer resolves parks the agent where it stands, logged.
3. `Occupancy.Clear()`; every agent re-claims on its next tick.

Aircraft are replanned here too — the §3.8 amendment above.

**Hold-short survives — by identity, not by node.** *Corrected 2026-09-06 while planning:*
the first draft made the flagged node non-derived so the sweep would keep it. Reading
`FRoadGuidelineBuilder::Build` shows that is not enough: the builder allocates FRESH nodes
for every derived edge end (`AddGuidelineNode` never deduplicates) and only hand-drawn
EDGES are re-pointed at them through `Ends`. A kept node would survive with no incident
edge — flagged, and routing nothing. So the flag is stored the way a hand-drawn edge stores
its ends: `URoadNetwork::HoldShortMarks`, an array of `{FGuidelineEndRef At;
FRoadSegmentId Protects;}`, saved with the level and snapshotted by undo like every other
network field. `FGuidelineNode::HoldShortFor` stays the thing everything READS (spec 5.5);
it is the derived cache, and the builder re-applies every mark to whichever node now holds
that identity as its last pass before the sweep. One source, one cache, rebuilt together.
`Protects` names a segment, and segment ids are the surface model, not regenerated, so the
mark itself needs no re-resolution.

---

## 7. Hold-short tool

`Tool/HoldShortTool.h`, an `IBuildTool`. One entry in `BuildActions()` — "Hold short",
key 8 — which drives the key, the banner and the bar button.

- Click within reach of a guideline node that has an incident edge derived from a runway
  chain, or adjacent to one: `URoadEditFacade::SetHoldShort(node, chain.First())`.
  Undoable by the existing Memento.
- Click a node already flagged: `SetHoldShort(node, unset)`.
- Any other click: refused, logged with the reason, previewed as an invalid intent.
- The overlay draws a bar across the taxiway at the node with the meaning `HoldShort`. The
  tool never writes the node; the facade does, like every mutator.

---

## 8. `UAirsideTraffic` after the split

Keeps every public name as a forwarder to `UGroundTraffic`; re-broadcasts
`OnAgentPhaseChanged` and `OnArrivalRefused` (a relay across a layer, as AirportOps already
does above it); owns `TMap<int32, TObjectPtr<ARoadAgentActor>>` and spawns or destroys on
the phase events (`Gone → *` spawns, `* → Gone` destroys); `Advance(Delta, SurfaceZ)`
forwards with the actor's network and then pushes each agent's `LastMotion` to its view.
Refactor contract applies: every `UE_LOG` survives, comment lines do not fall, one
composition test per forwarder.

---

## 9. Tests

All `Airside.Model.*` unless stated, `NewObject`, no world, reason strings, measured:

| Test | Measures |
|---|---|
| `Occupancy.Claims` | grant; refuse with holder id; preempt reserved, never occupied; `ReleaseBehind`; `HeldLengthOn` sums intervals |
| `Traffic.NodeYield` | two agents converge on a node in one frame: the vehicle's `StopWithin` reaches 0 before the node, the aircraft's never does; vehicle-to-aircraft distance ≥ vehicle footprint throughout |
| `Traffic.PriorityOverride` | same node, override lists the vehicle first: the aircraft yields |
| `Traffic.HoldShort` | chain held; aircraft stops within tolerance of the hold node at speed 0; chain released; it crosses |
| `Traffic.CarFollowing` | two aircraft one edge: gap never below `Footprint + Gap`; follower speed tracks the leader's |
| `Traffic.HeadOn` | bidirectional taxiway, nose to nose: both stop; all-aircraft cycle logged once; the later replans if a turn exists |
| `Traffic.DeadlockTriangle` | one-way triangle with one escape arm: detected within `StallSeconds + one tick`; exactly one agent replans and it is the highest id; all reach goals; no per-tick displacement above `Speed · Delta + tolerance` |
| `Traffic.GraphRebuild` | delete a segment ahead, add a bypass: handles live after rebuild; displacement across the rebuild frame ≤ one tick's travel; goal reached over the bypass. No bypass: stops at the last live node, logged |
| `Traffic.ArrivalRefusedRunwayOccupied` | dispatch while held refuses `RunwayOccupied` and the delegate fires |
| `RouteSearch.OccupancyCost` | held edge routed around; null occupancy yields the old plan bitwise |
| `Present.TrafficForwarders` | spawn the actor, tick: each `UAirsideTraffic` name reaches `UGroundTraffic`; views spawn and die on the events; both delegates re-broadcast |
| `Tool.HoldShort` | click sets, click clears, refused off-runway, undo restores, overlay emits a `HoldShort` intent |

The three §3.8 tests are `NodeYield`, `HoldShort`, `DeadlockTriangle`.

---

## 10. Logs

`LogAirsideTraffic`, one line each, keyed by agent id: stopped for a resource (kind, holder,
distance); resumed; hold-short reached; hold-short released; cycle detected (members); cycle
resolved (agent, old and new remaining length); unresolvable cycle; rebuild re-resolved N
and replanned M; arrival refused, runway occupied. The 71 existing `UE_LOG` in Airside
survive the move; the PR states the count before and after.

---

## 11. Out of M2

Reversing and pushback; time-windowed reservations; the build-tool warning for
all-aircraft layouts; turn bans; the `MinSegmentLength` constant; a Blueprint for the bar;
per-type aircraft length.

---

## 12. Amendments to the systems map

Recorded in `2026-09-05-game-systems-map-design.md` §3.8, dated 2026-09-06:

1. Hold-short is player-placed, not derived.
2. Aircraft routes are fixed against congestion; a rebuild that breaks the route replans
   the aircraft from its next node.
3. Unresolvable deadlock waiters retry on a cadence and on rebuild, not once.
4. Interval occupancy chosen (spec 5.7).
