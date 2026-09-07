# Stand Occupancy — Design

**Status:** design, agreed in conversation 2026-09-07 after PR #61's PIE round ("I called in
2 aircraft, they both went to the same stand"). Depends on #61 (facts layer, DepartAgent,
Select tool).

**Parents:** `2026-09-06-ground-traffic-design.md` (occupancy table, rebuild rules),
`2026-09-07-entity-inspector-design.md` (facts, panel), `2026-09-05-game-systems-map-design.md`
§3.4 (M3's stand allocator sits ABOVE this).

## 0. Nothing here is law

As the systems map §0. Change it for a reason, after discussion, and record the reasoning
where the old decision was.

## 1. Why

`ArrivalPlanner::Plan` takes the occupancy table and asks it one question - is the runway
held - then loops every live stand and routes to each pose node with no occupancy question
at all. Two arrivals pick the same best stand. The mechanism exists; it is not consulted.

Deleting infrastructure under an inbound aircraft is handled today by the M2 rebuild rules,
which know how to re-resolve a route to the SAME goal. A stand deleted while its aircraft is
on final leaves a dead goal, so the rebuild truncates the taxi-in to the last surviving node
and the aircraft parks on a taxiway junction, holding it. A taxiway deleted so that no stand
is reachable does the same, or strands the aircraft at the runway exit for good.

## 2. Decisions taken

- **Occupancy lives in the traffic occupancy table, not on the stand.** The user's first
  instinct was the stand itself; rejected for three concrete reasons. (1) `URoadEditHistory`
  snapshots the whole `URoadNetwork` with `DuplicateObject`, so a field on `FEntityInstance`
  would be undone and redone with taxiway edits. (2) Stands are saved and agents are not, so
  a loaded game would carry ghosts until M3 saves flights. (3) The agent already records its
  goal; a second field must be kept in step by dispatch, redirect, retire, depart, deadlock
  replan and rebuild, and the facts layer already derives the occupant from the agent. The
  stand stays infrastructure, like the taxi lines.
- **Reserve at planning, not on arrival.** The pick is the moment to claim.
- **No free stand refuses the arrival**, as the planner refuses for no runway or no route.
  Nothing spawns with nowhere to go. M3 turns the refusal into hold-then-divert.
- **A dead goal retargets to another free stand.** Only when none is reachable does the
  aircraft wait.
- **Wait, don't strand.** Stranding-is-final was written for pavement vanishing under a body.
  A missing stand has an answer - the player builds one - so an aircraft with no stand waits
  where its route ends and is re-offered a stand on every rebuild and whenever one frees.
- **Warn at the delete, don't refuse it.** The removal preview labels a stand that is in use.
- **The M3 reservation layer sits on top.** A flight reserving a stand at acceptance is a
  flight fact in AirportOps; this spec is the physical layer it will read.

## 3. The claim

A stand is HELD by the agent whose goal is its pose node, from the moment the plan is made to
the moment the goal changes or the agent goes. The claim is `FTrafficResource::OfNode(PoseNode)`
in `FTrafficOccupancy` - the kind that already exists; nothing new is added to the table.

- **Derived from `FRoadAgent::GoalNode` every tick.** `UGroundTraffic::ClaimAhead` already
  rebuilds each agent's claims per tick from its state. For an agent in Arriving or Taxiing
  whose `GoalNode` is a live stand pose node, it adds the pose-node claim as a RESERVATION
  (`bOccupied = false`) beside the guideline claims ahead of it; for a Parked agent at a pose
  node the claim is OCCUPIED (`bOccupied = true`) and sits with the surface it holds. Nothing
  new is stored on the agent: the claim is a reading of a field that already exists.
- **And asserted at dispatch, between ticks.** `DispatchArrival` and `DispatchAgent` claim the
  goal pose node the moment the agent is admitted, so a second dispatch in the same frame
  (M3's sequencer will do this; two 7-presses cannot) sees it held. `RedirectAgent` releases
  the old goal claim before setting the new goal, for the same between-ticks reason.
- **Released** by the paths that already call `ReleaseAll` (retire, Gone) and by the redirect
  above. `DepartAgent` goes through `RedirectAgent`, so departing frees the stand.
- **Rank does not preempt a stand.** Only aircraft take pose-node goals, and equal rank means
  the first holder keeps it. A pose node is a dead end off the lead-in, so no through route
  is refused by the claim. Asserted by test, not assumed.
- A pose node whose stand has been deleted is a freed slot; the handle is generation-checked
  and matches nothing. The claim evaporates on the next tick's rebuild of the table.

## 4. The planner

`ArrivalPlanner::Plan` skips a stand whose pose node `IsHeld` by any agent other than the one
being planned for (0 at dispatch). The stand loop is factored into
`ArrivalPlanner::ChooseStand(Network, From, Airframe, Occupancy, ExcludingAgent)` returning
the best `FRoutePlan` (shortest admitted taxi, as today) so §5 can call it from a node that
is not a runway exit.

New refusal `EArrivalRefusal::NoFreeStand`: stands exist and at least one is reachable, but
every reachable one is held. Distinct from `NoRouteToStand` (none reachable at all), because
the two tell the player different things: build a taxiway, or wait. `Describe` names it.
`UAirsideTraffic::OnArrivalRefused` already relays refusals; the bar's notification shows it.

## 5. Goal death: retarget, then wait

`UGroundTraffic::ReResolvePlan` (rebuild) gains one step for an aircraft whose goal was a stand
pose node, before truncation: if the goal is dead or no route reaches it, `ChooseStand` from
the aircraft's current node - the runway exit for an Arriving agent, the current step's
from-node for a Taxiing one - excluding stands held by others. Found: the plan is replaced
(`Replanned`), the log says which stand it now goes to. Not found: the plan is truncated to
its surviving prefix as today, and the agent is marked awaiting a stand.

**Awaiting a stand.** `FRoadAgent::bAwaitingStand`. INTENT DATA, like `FDepartureOrder`, not
a phase: the agent is Taxiing to the end of its prefix and then Parked there, and either of
those may be true while it waits. Set by the rebuild path above and by nothing else in v1.
While set: the agent holds whatever node it stops at (the Parked claim rule, unchanged), the
status line reads `No stand - waiting`, and `Depart` remains available as the escape hatch.

**Re-offer.** `UGroundTraffic` keeps a `bStandsMayHaveFreed` flag, set by `OnGraphRebuilt`
and by every stand-claim release (redirect, retire, Gone). At the end of `Advance`, when the
flag is set and any agent is awaiting, `ChooseStand` runs for each awaiting agent from its
`GoalNode` (where it stopped); a found stand becomes a `RedirectAgent` and clears the flag on
that agent. One pass, then the flag clears. A flag rather than an event subscription because
the table is rebuilt per tick and the model is world-free; a per-frame check of one bool is
the cheapest correct thing.

**Airborne with no stand** (all stands deleted while on final): lands anyway in v1, the
taxi-in truncates to the exit node, the aircraft waits there off the strip. Go-around is out
of v1; divert is M3's `Diverted`, which plugs in exactly here. Recorded, not solved.

**Undo.** Undoing a stand deletion restores the network snapshot and fires a rebuild; the
awaiting aircraft is re-offered by §5's flag and goes. No special case.

## 6. Warn at the delete

`FStandPlaceTool`'s remove preview (Ctrl / Remove modifier over a stand) already emits a
Doomed marker. It adds a `Label` in `EPreviewStyle::Refused` style reading
`in use by aircraft N` when the stand's pose node is held, via `IRoadEditTarget::
GetGroundTraffic()` (PR #61). The click still deletes. Refusing was rejected: the player owns
the infrastructure, and a modal over a build tool is the one thing Cities never does.

## 7. Facts and panel

`FStandFacts::OccupantAgent` is now read from the claim holder rather than scanned from
agents, and gains `bOccupantParked`: the panel says `Occupied by aircraft N` when parked and
`Reserved for aircraft N` when inbound. `FAgentFacts::Status` gains `No stand - waiting`
ahead of the Parked/Taxiing lines. No layout change.

## 8. Tests

Model (`NewObject`, no world):
- Two arrivals, two stands: different stands. A third: `NoFreeStand`. With one stand deleted
  and the other free, the refusal is still `NoFreeStand` not `NoRouteToStand`.
- Dispatch claims the pose node between ticks; `IsHeld` true before any `Advance`.
- Redirect releases the old stand; `DepartAgent` frees the stand it left.
- A vehicle routed through a held pose node's neighbourhood is not refused by the stand claim.
- Rebuild: delete the reserved stand while Arriving → `Replanned` to the other stand; goal
  names it. Delete the only stand → truncated, `bAwaitingStand`, status line; place a stand
  and rebuild → redirected, flag clears. Depart from waiting works.
- Facts: `Reserved for` while inbound, `Occupied by` when parked, `No stand - waiting`.

Tool:
- Stand remove preview labels an in-use stand; a free one gets no label.

Composition (actor, ticked): land two aircraft on the starter-style fixture, assert two
distinct stands through `UAirsideTraffic`; delete one stand through the facade mid-final and
assert the retarget happened through the actor's rebuild path.

## 9. Out of scope, recorded

- Reservation before an aircraft exists (M3 `UStandAllocator`), first-fit by ICAO class,
  release at taxi-out rather than at departure.
- Divert and go-around for an airborne aircraft with no stand.
- Refusing deletion, or asking for confirmation.
- Vehicles and service anchors: anchors are not claimed; M3's job board owns them.
- A Remove verb on the inspector for a waiting aircraft (deferred in #61; still wanted).
