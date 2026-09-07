# Ground Traffic — Design

**Status:** implemented 2026-09-06 (feature/m2-ground-traffic, PR #54). Milestone 2 of
`2026-09-05-game-systems-map-design.md` §3.8 and §5.3.
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
| Hold-short authoring | ~~Player-placed with a build-bar tool, never derived by the builder.~~ **Amended 2026-09-07:** runway-holding positions are derived at every taxiway end on a runway; the player places *intermediate* holding positions (see `2026-09-07-holding-positions-design.md`). The M2 decision confused the marking with the ATC instruction. | User decision. A hold bar is a piece of airport design, not a consequence of geometry. Nothing writes `HoldShortFor` today; spec 5.5's "the topology already exists" is true of the node, not the flag. |
| Hold-short tool scope | In M2. | Without it the runway rule exists only in tests. |
| Reservation window | Braking distance from current speed plus a per-class gap. | A fixed lookahead under-reserves a fast aircraft and over-reserves a stopped one. |
| Deadlock resolution | Lowest-ranked waiter replans at a node with the blocked edge banned. No reversing. | Every case reversing fixes is also fixed by the player adding an exit, which §5 picks up. Reversing needs the follower to walk backwards with a heading rule; deferred as a v2 option, not a v1 debt. |
| Unresolvable waiters | Stay waiters. Retry on a sim-time cadence and on every graph rebuild. | The player's fix must be picked up. "Logged once and left" was the first wording and was wrong. |
| Graph rebuild | Re-resolve every agent's steps by position; replan what fails; clear the table. | A road edit regenerates the guideline graph with new handles. Today agents survive only because the follower never reads a handle. A handle-keyed table would go stale on every edit. |
| Aircraft routes | Fixed at clearance against congestion. Replanned only while stopped at a node: deadlock, or a rebuild that broke the route. | Amendment to §3.8, which said replan "only while stopped at a node" without naming the rebuild case. Freezing an aircraft whose taxiway was deleted leaves it on air. |

Rejected outright: time-windowed reservations (resource × time interval, CBS-style). Correct
and heavy; the speed profile would have to be honoured in time as well as distance. Named so
it is not rediscovered.

### 1.1 Amendments made during execution

Design-level deviations found while implementing, each recorded in place at its section with
a dated paragraph; listed here so a reviewer does not have to diff the whole document:

- `FRoadAgent::CrossingRunway` — a runway chain stays occupied from a granted bar (or a
  landing's `Vacated` handover) until the agent's tail is geometrically clear of the strip,
  not just past the exit node. §3.1.
- The hold above is armed only when the step leaving a bar leads ONTO the strip; a bar on the
  exit side of a two-bar crossing arms nothing. §3.1.
- The hold follows the agent's BODY, not a node: `ECrossingPhase { None, Committed, OnStrip }`
  sampled at nose/centre/tail on the one guideline polyline. §3.1.
- `OnGraphRebuilt` clears edge and node claims only; surface claims (keyed on the surface
  model, not the guideline graph) survive a rebuild. §6.
- A rebuild that cannot replan an agent's route TRUNCATES it to the last live node instead of
  parking it where it stands; a goal node that no longer resolves keeps its old handle. §6.

Other execution-time findings (implementation nuances that did not change spec text, plan
corrections, and test-fixture decisions) are listed in the handover's Outcome section
(`docs/superpowers/handovers/2026-09-06-m2-ground-traffic.md`) and in PR #54's description;
the plan's working directory that held the raw ledger was retired once the branch was pushed.
Two PIE rounds on 2026-09-06 added the deadlock-resolver amendments in §5 and the airborne
release in §3.1.

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

*Amended 2026-09-06, final review — the block above is the design sketch; this is the API
that shipped.* Rank moved ONTO `FTrafficClaim` (`int32 Rank`) rather than travelling beside
it, because every caller had to hand the two over together and a rank that could be passed
for the wrong claim was a bug nothing would catch. `TryClaim(const FTrafficClaim& Claim,
FTrafficClaim& OutBlocker)` hands back the whole blocking CLAIM, not just its holder id: the
refusal has to answer "how far may I go", which needs the blocker's interval as well as its
owner. `ReleaseBehind` was never needed and does not exist; what the tick actually wants is
the complement, `ReleaseExcept(AgentId, Keep)` — release everything of mine that is not on
this pass's list — which keeps first-to-reserve across ticks in one call. Beside it:
`ReleaseReservations(AgentId)` (a replan gives back the line ahead, never the ground under
the body), `ReleaseGuidelineClaims()` (a graph rebuild: every edge and node claim, whoever
holds it, surfaces kept), `ReleaseGuidelineClaimsOf(AgentId)` (one stranded agent's edges and
nodes, its surfaces kept — the same distinction for one agent), `Release(AgentId, Resource)`
and `ReleaseAll(AgentId)`. `TakePreempted()` reports the agents a preemption evicted this
tick, which the sketch had no mechanism for at all.

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
  - *Amended 2026-09-07: node reach.* `Footprint/2` above is the floor, not the rule. A
    node's claim reaches along each incident edge as far as a body that distance down it
    and a body the same distance down another edge of the node are still within one
    footprint of each other, measured on the sampled polylines (`NodeReach::Compute`).
    That is exactly `Footprint/2` for a straight continuation, `Footprint/√2` at a right
    angle, and roughly `√(2·R·Footprint)` where a stand's sweep arc leaves its taxiway
    tangentially - 2454 of a 4058 uu arc on the two-stand fixture. Everywhere this section
    says "within `Footprint/2` of the node" read "within the node's reach along that
    edge"; the window asks for a node when it reaches the reach, not the node; and a
    refused node's stop point is a gap short of where the reach begins. Why: two edges
    that leave a node together are one resource for the length they run side by side,
    and holding only the node let a departing aircraft on the arc drive alongside an
    arrival on the taxiway 429 uu apart (`Traffic.DepartureMeetsArrivalOnTaxiway`). Why
    equal distances and not point-to-line: point-to-line makes every straight split a
    whole footprint long and a follower would brake for its leader's node.
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
    entering the runway edge until `Gone`. *Amended 2026-09-06 from the second PIE report:*
    until AIRBORNE - `FTakeoffRun` in its Climb phase - not until `Gone`. Gone is the top of
    the climb, 300 m up and most of a minute after lift-off, and every arrival asked for in
    that minute was refused "the runway is in use" over an empty strip. The strip is what the
    table protects; the next arrival joins its approach minutes out and cannot touch down
    under a climbing aircraft. Wake and separation between movements are M3's sequencer.
    `Airside.Model.Traffic.DepartureReleasesWhenAirborne` measures the release within one
    tick of lift-off.

`DispatchArrival` refuses with `RunwayOccupied` while the chain is held.

*Amended 2026-09-06 during Task 6 review — a fourth route to the same rule.* The three above
end exactly where the exposure begins: once an agent's tail passes the bar, the bar node
leaves its window and the surface claim with it, while the agent is standing on the
centreline; and a junction's turn paths carry no `DerivedFrom` (by design), so the crossing
edges never imply the runway either. A landing could then be cleared onto an aircraft
mid-crossing. So: **an agent that has passed a hold-short node, or has just vacated a
landing, holds that runway's chain OCCUPIED until its tail is geometrically clear of the
strip** — `FRoadAgent::CrossingRunway` names the chain's seed while it applies; it is set
when the agent's centre passes a granted bar (and at the Vacated handover, in place of the
immediate release), and cleared once the tail has passed a route node that lies outside the
runway's half-width of its centreline (`URoadNetwork::IsGuidelineNodeOnRunway`), or is a
full chain half-width past a node that lies ON the strip (the crossing node itself, which
is the last node before a long taxiway edge on most layouts - without this clause the hold
would run to the next node, kilometres away). Geometry
rather than a far-side bar, because a player may place one bar or none on the far side, and
a hold that waits for a bar that does not exist never ends. Rejected: holding until the next
node only — a runway node sits ON the strip, so that releases while the tail is still on it.

*Refined 2026-09-06 during Task 7:* a bar cannot tell a crossing being ENTERED from one being
LEFT — the far-side bar of a two-bar crossing is also "a hold-short node the agent has just
passed", and arming the hold there kept the runway occupied behind an aircraft that had
already crossed, for the whole exit leg. So the hold is armed only when the step leaving the
bar leads ONTO the strip: its end node lies on the runway (`IsGuidelineNodeOnRunway`), or
the agent's own centre already does. A bar whose step leads away from the strip is the
exit bar and arms nothing.

*Refined again 2026-09-06 during Task 8 review:* keyed on the BODY, not on nodes. A
hand-drawn bar-to-bar edge with no node on the strip (parent spec R10 guarantees one only
for generated crossings) armed ~half a footprint late and released with tail still on the
runway. So the hold has a phase, `ECrossingPhase { None, Committed, OnStrip }`: Committed
when the centre passes a bar whose step leads toward the strip (its end node on it, any
sampled vertex of the step on it, or the nose point on it); OnStrip once the centre point
is on it; released once OnStrip and the tail point is off it. Nose, centre and tail are
`GuidelineGeom::PointAtDistance` on the one sampled polyline at `T ± Footprint/2` - never a
second evaluator. The node-based clauses above survive only as the fallback for a polyline
too short to sample. A phase, not two bools, because Committed-but-not-yet-on and
On-but-tail-not-clear are the two states that must never be confused.

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

*Amended 2026-09-06, final review:* "their surface" in step 1 is `RunwayHeld` PLUS the chain
of any crossing still in progress — the surface the agent's BODY is standing on, not only the
one a landing or a take-off was granted. `RunwayHeld` is empty on anything that never landed,
so the narrower reading gave the strip back one tick after an aircraft whose plan died
mid-crossing parked on the centreline, and a landing could be cleared onto it. A parked agent
still holds no guideline edge or node: it blocks no taxiway. The hold ends when the agent
does — `RetireAgent`, or the tick that removes it.

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

*Amended 2026-09-06, final review — what shipped.* The function is
`ReplanAt(AgentId, Network, SpliceStep, BannedEdge)`: a STEP INDEX, not a node. Both callers
— the deadlock resolver and the graph rebuild — hold the step they failed on and would have
had to convert it to a node to call `ReplanFrom`, and the conversion is not injective: a
route that revisits a node (a van sent out and back through one junction) gives two answers
to "which step does this node leave from", and the wrong one splices behind the agent and
teleports it. The index also makes the precondition checkable — `SpliceStep` at or ahead of
`CurrentStep(Travelled)`, refused rather than clamped — which a node alone cannot express.
`BannedEdge` is unset for the rebuild caller: the edge that failed is not in the graph at
all, so no search could pick it.

---

## 5. Deadlock

**Detection.** Each tick, for every agent with `StalledSeconds > Rules.StallSeconds`: follow
`WaitingOn`. Reaching an agent already on the path is a cycle. A cycle is identified by its
lowest member id, so it is reported and resolved once per tick, not once per member.

**Resolver.** *Refined 2026-09-06 while planning.* A member can replan only if it is stopped
AT the node where the edge it was refused begins — within `Gap + Footprint/2` (plus the
node's reach beyond `Footprint/2`, amended 2026-09-07 with §3.1) short of that
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

*Amended 2026-09-06 from the first PIE deadlock (`samples/deadlock.png`, log 19:13-19:20):*
an arrival vacating toward a runway's end met two departures queued at that end's bar on the
same bidirectional taxiway. Three things the resolver got wrong, all fixed and pinned by
`Airside.Model.Traffic.HeadOnReplansRoundBarHolder`, which rebuilds that airport on the
real solver and builder:

- **The ban must be what refused the agent.** A node with an aircraft standing on it is a
  wall from every arm; banning only the edge the agent was about to take let the search
  loop round the runway's end and re-enter the same node from its other arm. The agent
  now records `BlockedResource`, and a node refusal bans the node (`FRouteQuery::BannedNode`).
- **A replan never taxis along a runway** (`FRouteQuery::bAvoidRunways`). The loop route
  used a runway-derived edge, re-reserved the strip, and starved the departure at the bar
  for the very surface it was waiting for. Crossings are turn paths and nodes, so they stay
  open.
- **A bar-holder is a candidate, and candidates are tried in order.** The hold-short refusal
  now names the step LEAVING the bar as `BlockedStep`, so an aircraft with its nose on the
  bar qualifies; and the resolver walks every qualifying member in rank order until one
  replan succeeds, refusing a "replan" that returns the route the agent already had. The
  departure with one way out fails fast and the arrival with two turns.

The route the player could see - the top taxiway, across at the crossing's bars, along the
bottom - was legal throughout; it lost on distance to a route through a node nobody could
enter. §4's "nodes are not costed" stands for routing at plan time; a stopped bar-holder is
handled by the ban, not by a cost.

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
   (§4). *Amended 2026-09-06 during Task 9 review:* a goal that no longer resolves keeps its
   old handle, and a route that cannot be replanned is TRUNCATED to the last live node - the
   agent drives there and parks, logged - rather than parking where it stands, which would
   leave it stopped mid-taxiway for ever. Only an agent whose current step itself is gone is
   stranded in place. The node behind the agent (the current step's from-node) is re-pointed
   too: the crossing arm, the tail-node claim, rank and a replan's start all read it.
3. Edge and node claims are cleared (`FTrafficOccupancy::ReleaseGuidelineClaims`); every
   agent re-claims on its next tick. *Amended 2026-09-06 during Task 9 review:* SURFACE claims
   are kept. They are keyed on segment ids, the surface model, which a guideline rebuild does
   not regenerate; clearing them left an aircraft mid-crossing or rolling out with no strip
   claim until the next tick, and `DispatchArrival` reads the table between ticks. Handles
   are generation-checked, so a stale edge or node claim can never name new pavement - the
   reason to drop them is only that their resources are gone.

*Amended 2026-09-06, final review:* the "stranded in place" rule above is now what the code
does. A failure at the step the agent is DRIVING ON strands it — it is not replanned, because
a splice at the current step re-reads the agent's own `Travelled` on new geometry and moves it
sideways (3310 uu, measured on `Airside.Model.Traffic.GraphRebuild` case 5, unbounded in
principle), and not truncated, because there is no node behind it on a line that survives. A
stranded agent gives back its EDGE and NODE claims only (`ReleaseGuidelineClaimsOf`) and keeps
any runway surface with the crossing fields that describe it: a rebuild deletes pavement, it
does not move aeroplanes, and `ArrivalPlanner::Plan` reads the table between ticks.

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
mark itself needs no re-resolution. *Noted 2026-09-06 during Task 10:* a mark whose
taxiway segment is later SPLIT (a new junction placed on it) is pruned and the bar is lost -
`SplitSegmentIn` retires the old segment handle by design, the same rule under which a
hand-drawn edge over a vanished segment is killed rather than guessed at. Accepted for v1;
re-attaching a bar across a split belongs with the turn-ban work the handover lists.

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
| `Traffic.DeadlockRing` | one-way square ring with one escape arm: detected within `StallSeconds + one tick`; exactly one agent replans and it is the highest id; all reach goals; no per-tick displacement above `Speed · Delta + tolerance`; never closer than a footprint. *Was a triangle until 2026-09-07:* at 60° a van waiting at the next box's entry is still inside the corner's reach, so a ring of 600 uu boxes and 500 uu vans is a true gridlock, and the triangle only ever "resolved" by driving vans 278 uu apart |
| `Traffic.DepartureMeetsArrivalOnTaxiway` | builder graph, two stands: a parked aircraft departs while the next arrival taxis in; never closer than a footprint; somebody waits. The 2026-09-07 play report |
| `NodeReach.StraightContinuation` / `RightAngle` / `TangentArc` / `CacheFollowsRevision` | reach is exactly `Footprint/2` straight on, `Footprint/√2` (+ one sample) at 90°, far past both on a tangent arc and equal from either edge; the cache re-measures after `AddGuidelineEdge` bumps the revision |
| `Traffic.GraphRebuild` | delete a segment ahead, add a bypass: handles live after rebuild; displacement across the rebuild frame ≤ one tick's travel; goal reached over the bypass. No bypass: stops at the last live node, logged |
| `Traffic.ArrivalRefusedRunwayOccupied` | dispatch while held refuses `RunwayOccupied` and the delegate fires |
| `RouteSearch.OccupancyCost` | held edge routed around; null occupancy yields the old plan bitwise |
| `Present.TrafficForwarders` | spawn the actor, tick: each `UAirsideTraffic` name reaches `UGroundTraffic`; views spawn and die on the events; both delegates re-broadcast |
| `Tool.HoldShort` | click sets, click clears, refused off-runway, undo restores, overlay emits a `HoldShort` intent |

The three §3.8 tests are `NodeYield`, `HoldShort`, `DeadlockRing` (`DeadlockTriangle` until 2026-09-07).

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
