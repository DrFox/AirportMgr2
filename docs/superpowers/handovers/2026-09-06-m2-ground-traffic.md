# Handover: Milestone 2 — Ground Traffic

Paste everything below the line into a fresh Claude Code session in `C:\repos\AirportMgr2`.

---

You are picking up AirportMgr (UE 5.8.2, C++) at the start of **Milestone 2: ground traffic
occupancy**. Read `CLAUDE.md` first; it is the project's rules and is not optional. Then
read, in this order:

1. `docs/superpowers/specs/2026-09-05-game-systems-map-design.md` — the systems map. §0 says
   nothing in it is law: change decisions for reasons, after discussion, and record the reason
   where the old decision was. §3.8 is the milestone you are building. §5.3 is the build order.
2. `docs/superpowers/specs/2026-08-29-ground-movement-model-design.md` — the guideline graph
   the occupancy model sits on. Spec 5.4 (class priority) and 5.5 (hold-short) are already in
   the model as `FGuidelineNode::PriorityOverride` and `HoldShortFor`.
3. `Plugins/Airside/Source/Airside/Public/Model/RouteFollower.h` — its header comment names
   the traffic model as the next thing owed: two aircraft pass straight through each other,
   hold-short nodes are not consulted, and the graph's right-of-way is ignored.
4. `docs/superpowers/plans/2026-09-05-m1-foundation.md` — the shape of a plan here: exact
   signatures, red-then-green, one build per task, `UE_LOG` counts before and after.

## State of the tree (all merged to `main`, 2026-09-06)

| PR | What |
|---|---|
| #50 | Systems map spec and GDD v2 |
| #51 | M1 foundation: `AirportOps` plugin (`USimClock`, `UOpsEvents`, `OpsSave`, `UOpsCatalog`/`UScenario`, `UOpsRuntime` + game-instance subsystem), Airside seam (agent ids, `OnAgentPhaseChanged`/`OnArrivalRefused`, `RedirectAgent`/`RetireAgent`, `AirsideCapability::Summarise`, `ARoadNetworkActor::SetSimTimeScale`). Also fixed a latent PIE bug: transient subobject pointers reset to the CDO on duplication (`PostInitProperties` re-points them). |
| #52 | Build bar HUD: `BuildActions()` is ONE registry driving key bindings, the banner and the bar's buttons; `EClickModifier`; `UBuildBarWidget` (code-built bottom bar, Blueprint optional). |
| #53 | Road builder: exact fillet fit with far-end-aware allowances, nodes FAIL loudly when no radius fits, folded ribbons never emitted, `TooShortForCorner` placement rule and `NodeCornersFit` for drags, ear-clipped junction rims when no fan apex sees the rim, junction snap claims its polygon not a circle. |

Baseline: **88 automation tests, 0 failed, 0 crashed** via `./Tools/Run-AirsideTests.ps1`
(default filter `Airside+AirportOps+AirportMgr`). `Tools/Check-Architecture.ps1` runs first
inside it and enforces include direction per layer, the cross-plugin rule (Airside never
references AirportOps), one log category per name per module, and doc-comment hygiene.

## What M2 is (spec §3.8, summarised — the spec wins where this differs)

Ground traffic lives in **Airside `Model/`**, world-free, `NewObject`-testable.

- **Mechanism vs policy.** Airside owns `FTrafficOccupancy`: who holds which guideline edge,
  node and runway surface, plus reservations a short way ahead. `URunwaySequencer` (M3,
  AirportOps) is policy and will ask occupancy for grants. Runway occupancy is one more
  surface in the same table.
- **Reservation window.** Each agent reserves the edges and nodes within its braking
  distance plus a class separation gap. `FSpeedProfile` already plans speed for the route;
  occupancy adds one input per tick, a stop-within distance to the first unreserved resource.
  Car-following on a taxiway falls out of it.
- **Nodes are the conflict points.** One agent at a time through a junction or crossing.
  Order: `PriorityOverride`, then class priority (aircraft over vehicles, already on the
  graph), then first-to-reserve. A hold-short node reserves the surface named by
  `HoldShortFor`.
- **Routing.** `RouteSearch` gains an occupancy cost so vehicles route around jams at plan
  time. Aircraft routes are fixed at clearance and may replan only while stopped at a node —
  an aircraft rerouting mid-edge leaves the line the player was shown, which is the
  invariant the guideline graph exists to protect.
- **Deadlock.** Reservations form a wait-for graph; a cycle is a deadlock; the lowest-priority
  waiter releases and replans. An all-aircraft cycle is logged and the later arrival diverted.
- **Tests, world-free, measured not narrated:** two agents converge on a node and one yields;
  an aircraft holds short while the runway is occupied and proceeds on release; three
  vehicles in a cycle resolve without teleporting.

## How to work it

- **Brainstorm first, then spec, then plan, then execute.** This is architectural: use the
  brainstorming skill, ask one question at a time, propose 2–3 approaches with a
  recommendation, present the design in sections, write
  `docs/superpowers/specs/2026-09-0X-ground-traffic-design.md`, then the writing-plans skill
  for `docs/superpowers/plans/…`. Execute inline (each task ends in a full build and a cold
  test run; nothing parallelises; a subagent per task only re-reads context).
- **Branch `feature/m2-ground-traffic`, PR to `main`.** Fill the PR template: build line,
  test line, `UE_LOG` delta, seams and the test that fails if each is unwired, runtime
  evidence or "builds, unverified at runtime".
- **The editor must be closed to build.** The user has allowed you to close it yourself when
  nothing is unsaved: check the log for save lines and `git status Content/`, then
  `CloseMainWindow()` on the `UnrealEditor` process and wait; say you did. Live Coding
  (`python Tools/Mcp.py call LiveCodingToolset.LiveCodingToolset CompileLiveCoding`) covers
  function bodies only and does NOT survive an editor restart — the editor loads whatever is
  on disk. A Live Coding failure reports "see Live console"; do a full build instead.
- **Read `Saved/Logs/AirportMgr.log` before any hypothesis.** Rebuild the user's exact graph
  headlessly from the `Node N placed` / `Segment N connected` lines when a report is about
  geometry; a throwaway probe test that logs per-segment facts settled two bugs this week in
  one round trip each. Instrument the boundary, then ask for the repro.
- **Tests assert behaviour with a reason, and measure.** Winding measured against a
  reference the same builder produced; trims summed against the length; a duplicated actor's
  subobject outers compared. Red first: run the new test on the unfixed tree and quote the
  failure.
- **Every `UE_LOG` survives a refactor; comments say WHY and name the rejected alternative.**
  Count both before and after.

## Facts that will save you a round trip

- `EAgentPhase { Arriving, Taxiing, Departing, Parked, Gone }`. `Gone` doubles as "did not
  exist": spawn broadcasts `Gone -> Arriving/Taxiing`, removal broadcasts `<phase> -> Gone`.
- `UAirsideTraffic::Advance(DeltaSeconds, SurfaceZ, Network, Rules)` is the one tick; the
  actor scales `DeltaSeconds` by `SimTimeScale` (the x0..x8 multiplier only, never the day compression).
  `FRoadAgent::Advance` owns every handover; `FRouteFollower` walks `FRoutePlan::Polyline`
  from `GuidelineGeom::Sample` — the ONE sampled array the search costs, the overlay draws
  and the follower walks. Do not add a second evaluator.
- `URoadNetwork` guideline API: `GetGuidelineNodes/Edges()`, `GuidelineNodeIdAt(int)`,
  `GetOutgoingGuidelines(node, class)`, `AddGuidelineNode(pos, bDerived)`,
  `AddGuidelineEdge(FGuidelineEdge&&)`. Authored nodes (`bDerived=false`) survive rebuilds;
  derived ones are swept. Junction turn paths are one one-way edge per ordered arm pair with
  `DerivedFrom` unset.
- Unity build: two anonymous-namespace helpers of one name in two test files collide; prefix
  them. `DEFINE_LOG_CATEGORY_STATIC` twice for one name collides; test modules cannot link
  Airside's categories, so a test that logs defines its own.
- `UUserWidget::NativeOnInitialized` needs a player context; a world-only widget builds in an
  `Initialize()` override. `FInputChord` bindings take a parameterless handler.
- `UWidgetBlueprint::WidgetTree` is not scriptable from Python on 5.8; the bar is code-built
  and a Blueprint restyle is hand-authored from the seven slot names in `BuildBarWidget.h`.
- Keys in PIE: 1–6 tools, 7 land, C watch, G overlay, Backspace clear, Ctrl+Z/Y undo/redo,
  Comma/Period speed, P pause, K/L quick save/load. Every key is a button on the bar.

## Open items you may meet (not M2 scope, do not start them uninvited)

- **Turn bans.** A tool to remove one turn path (say A→F at a six-arm junction) needs a
  per-node `BannedTurns` list keyed by ordered segment-id pair that the guideline builder
  skips; `DisconnectGuideline` refuses derived edges by design. Splits must rewrite bans.
- **`MinSegmentLength` (250 uu) is now superseded in practice** by the corner-fit rule; a
  right-angle on the 400-wide default needs ~444 uu. Decide whether the constant should follow.
- **A Blueprint for the bar** does not exist yet; the code-built bar is the default look.
- **Systems-spec amendments** made during M1 are recorded in §2.0, §2.1, §2.3 of the systems
  map; read them rather than the original text.

## User preferences (from memory; honour them)

- OO veteran, new to Unreal C++: skip OO basics, explain UE machinery (subsystems, CDOs,
  duplication, reflection) when it bites.
- Rigorous architecture: name the pattern, justify every deviation UE forces.
- Extremely concise replies; sacrifice grammar for concision. Feature branches, PRs to
  `main`, no `Co-Authored-By` trailer.
- Bottom bar UI in sections, Cities Skylines style; never a vertical toolbar.
- Design decisions are revisable: discuss, change for the right reason, write the reason
  down where the old decision was.

Start by reading the four documents above, then open the brainstorm for M2 with your
classification and the first question.

---

## Outcome (2026-09-06)

M2 shipped on `feature/m2-ground-traffic` (31 commits over base `8a86491`, 56 files changed),
spec `docs/superpowers/specs/2026-09-06-ground-traffic-design.md`. Full detail is the
plan/rulings/progress ledger in `.superpowers/sdd/2026-09-06-m2-ground-traffic/`.

### What shipped, task by task

1. `FRouteFollower::Advance` gains a `StopWithin` cap; `FRouteStep::EndDistance`;
   `RouteSearch::Splice`.
2. `FTrafficOccupancy` — the one reservation table: `TryClaim`/`Release`/`ReleaseBehind`/
   `ReleaseAll`/`HeldLengthOn`/`IsHeld`/`Clear`.
3. `URoadNetwork` runway helpers — `RunwayChain`, `IsRunwaySegment`,
   `RunwayNearGuidelineNode`, `EArrivalRefusal::RunwayOccupied`.
4. `UGroundTraffic` (`Model/`) takes over the agent list and dispatch from `UAirsideTraffic`,
   which shrinks to a view registry plus forwarders; `IRoadEditTarget::DispatchAgent` gained
   a 3-arg overload with a 2-arg forwarder for the old call sites.
5. `UGroundTraffic::Advance` — the arbitration tick: rank ordering, window claims, the
   box-junction entry rule, car-following, node-yield, priority override, head-on stop.
6. Runway surface claims — hold-short refusal, `DispatchArrival` refuses `RunwayOccupied`,
   `FRoadAgent::CrossingRunway` holds a chain occupied through a crossing.
7. `RouteSearch` occupancy cost for routing; `UGroundTraffic::ReplanAt` splices a new route
   in without a position jump.
8. Deadlock detection (a wait-for cycle read off `WaitingOn`) and resolution (the lowest-
   ranked qualifying member replans with the blocked edge banned); the crossing hold became
   body-based (`ECrossingPhase`).
9. Graph rebuild — `OnGraphRebuilt` re-resolves each agent's steps by position, replans or
   truncates a broken route, clears guideline claims (surface claims survive).
9b. `ClaimAhead` (~630 lines) split into file-local helpers; no behaviour change, all
   Traffic tests green before and after.
10. Hold-short marks stored by identity (`HoldShortMarks` + `FGuidelineEndRef`), survive a
    guideline rebuild by re-application, not by keeping the node; `URoadEditFacade::SetHoldShort`.
11. Hold-short build tool on key 8; the overlay draws the bar; both drivers list it.
12. Final fix wave (whole-branch review): `DispatchArrival` claims the runway synchronously,
    so two presses of `7` in one frame cannot clear two landings onto one strip; a stranded
    or dead-plan agent gives back its guidelines and KEEPS its runway surface
    (`FTrafficOccupancy::ReleaseGuidelineClaimsOf`); a rebuild that deletes the step under an
    agent strands it in place instead of teleporting it (spec §6.2); `FTrafficRules` became
    a saved `ARoadNetworkActor::TrafficRules` handed down the tick; the
    `RebuildMesh -> OnGraphRebuilt` seam gained a composition test (mutation-checked).

### Test and build

`118 test(s) run, 0 failed, 0 crashed.` (`./Tools/Run-AirsideTests.ps1`, cold editor start).
`Check-Architecture: PASS (include direction, cross-plugin, log categories, doc comments,
content default)`. Baseline at handover was 88/0/0.

### Log and comment deltas

`UE_LOG` in Airside: 71 -> 94 (97 at Task 12; the final fix wave dropped the three
`"released the runway"` lines that no longer describe anything - a stranded or dead-plan
agent keeps the strip its body is on). Comment lines across the split pair (`AirsideTraffic.h/.cpp` +
`GroundTraffic.h` + `GroundTraffic*.cpp`, the four files the arbitration/rebuild/deadlock
logic actually lives in): 179 -> 1411.

### Deviations from the plan (rulings)

One line each; full reasoning and cost-if-wrong is in `rulings.md`.

- No git worktree: built in the main checkout, editor closed per task (warm `Intermediate/`).
- `Airside.Model.RouteSearch` renamed to `...RouteSearch.Find`, and the bare
  `Airside.Model.Traffic` to `...Traffic.RightOfWay` — the UE automation tree silently drops
  a bare-named test once a dotted child exists under it.
- `TryClaim`'s same-agent update conflict-checks against other agents rather than replacing
  unconditionally — the brief's version would grant a follower into a window that grew onto
  the leader's occupied interval, the exact pass-through defect M2 fixes.
- `TryClaim`: a claimant that is itself occupied preempts any non-occupied holder regardless
  of rank (two occupants still conflict) — physical presence beats a reservation.
- `ClaimAhead` claims every occupied entry first, unconditionally; the first-refusal short-
  circuit applies to reservations only.
- Box-junction entry rule applies only to the first box step not yet entered this pass, not
  every consecutive short step chained forward — chaining it was traced by hand to deadlock
  harder on the three-vehicle triangle.
- NodeYield's van-yields assertion is "minimum speed while waiting on the aircraft < 50% of
  cap", not "StopWithin < 1" — a yielder that slows and lets the aircraft through has
  yielded; a full stop needs braking distance the fixture doesn't give.
- CarFollowing samples the gap only once the follower has moved (`Travelled > 0`).
- The reservation window head is nose-based (`T + Footprint/2 + Window`, per spec 3.2), not
  the brief's `T + Window` — without it a stopped waiter flickered granted/held every tick.
- A hold bar at the far end of a box step stops the nose ON the bar; the box-entry rule
  never overrides a player-placed bar.
- `CrossingRunway`/the crossing hold gained three refinements across review (spec 3.1,
  §1.1 of the spec lists them): a fourth surface rule so a crossing chain stays occupied
  until the tail is clear; armed only on a bar whose step leads onto the strip; finally
  keyed on the agent's body (`ECrossingPhase`) rather than nodes, for hand-drawn crossings
  with no on-strip node.
- `FTrafficOccupancy::Release` kept with no production caller — it's the table's per-
  resource unit-tested API; final review left it in.
- `ReplanAt` releases the agent's RESERVATIONS only, not every claim — the occupied chain,
  standing node and body interval survive so a `DispatchArrival` between a replan and the
  next tick still sees a crossing runway as held. Supersedes the brief's `ReleaseAll`.
- `URoadEditFacade::FindRoute` passes the model's occupancy and `Rules.CongestionWeight` for
  non-aircraft classes — spec 4's "vehicles always route with the table" had no production
  caller until this.
- Plan amendment: Task 9b ("split ClaimAhead") inserted after Task 9 — the function had
  grown to ~630 of the file's 1210 lines.
- `OnGraphRebuilt` clears edge and node claims only; surface claims (keyed on the surface
  model) survive — supersedes the brief/spec's `Occupancy.Clear()`.
- A goal node that no longer resolves keeps its old handle; a route that can't be replanned
  truncates to the last live node and the agent parks there, rather than parking mid-taxiway.

### Deferred minors worth a follow-up

**Crossing hold**
- The half-width release clause is conservative for anything but a centreline node (a full
  chain-width clause would be unconditionally safe). Some "free at some tick" test
  assertions are weaker than their prose claims.
- A bar-to-bar crossing with no on-strip node (hand-drawn, no generated crossing node) arms
  the hold only once the nose reaches the asphalt — a measured 250 uu committed-but-unheld
  window. Recommended fix: arm when any polyline span of the step leaving a bar intersects
  the strip slab. Also: a bar two-plus steps short of the asphalt never arms; only one
  runway's crossing can be held at a time (a second runway isn't armed mid-hold of the
  first); `IsPointOnRunway` re-walks `RunwayChain` per call and should be hoisted.
- ~~`Strand` (rebuild) still calls `ReleaseAll`~~ FIXED in the final wave: it calls
  `ReleaseGuidelineClaimsOf` and keeps the crossing fields, so an aircraft stranded
  mid-crossing goes on holding the strip. The `CrossingHoldsRunway` rebuild test is still a
  no-op rebuild (pins release only, not re-arming).

**Deadlock / arbitration**
- A preempted agent can be told to stop inside its own braking distance and halts abruptly
  (measured 1525 uu given vs 2500 needed) — the follower clamps rather than overshoots, so
  it's safe, just not smooth.
- An occupied edge interval spans the whole reservation window, so a moving low-ranked agent
  can preempt a higher-ranked agent's reservation on the same edge (the spec wanted this
  closed). A same-agent update could in principle overwrite an occupancy with a later
  reservation on a revisited node — unreachable with today's routes.

**Rebuild**
- ~~Case 5's 3310 uu teleport~~ FIXED in the final wave: a failure at the step the agent is
  DRIVING ON now strands it in place (spec §6.2) instead of splicing a replan under it, and
  case 5 asserts no per-tick displacement above one tick's travel. A stranded agent keeps the
  runway surface it is standing on (`ReleaseGuidelineClaimsOf`), so a landing cannot be
  cleared onto it.
- The truncation-backwards case (an agent truncated to a point behind its current position)
  is untested — and is now unreachable for a driving agent, since the only way to reach it
  was the current-step failure that strands instead.

**Tool**
- The hold-short tool's skip-when-absent path is untested (safe via the orphan sweep, traced
  by hand, not measured).

**Arbitration, as designed (M3 input, not a defect)**
- Parked agents keep the surface their body is on; other than that they claim nothing — spec
  §3.4. `ClaimAhead`'s non-Taxiing branch holds `RunwayHeld` PLUS the chain of any crossing
  still in progress, so an aeroplane abandoned ON a runway (a stranded agent that then parks)
  goes on holding it until the player retires it, and a landing offered meanwhile is refused
  `RunwayOccupied`. It holds no guideline edge or node: a parked agent blocks no taxiway.
  What SHOULD happen to an aeroplane stuck on a strip — tow it, refuse the stand, warn the
  player — is `URunwaySequencer`'s question in M3, which is the object that decides who may
  use a runway at all.

### Follow-up issues to file

Triaged by the final review; none blocks the merge, each is a separate piece of work.

- **Bar-to-bar arming window.** A hand-drawn crossing with no on-strip node arms the hold
  only when the NOSE reaches the asphalt — a measured 250 uu committed-but-unheld window.
  Fix: arm when any polyline SPAN of the step leaving a bar intersects the runway SLAB,
  rather than testing points. Also in that family: a bar two-plus steps short of the asphalt
  never arms, and only one runway's crossing can be held at a time.
- **`RunwayChain` re-walk.** `URoadNetwork::IsPointOnRunway` re-walks the whole chain per
  call, and the claim pass calls it several times per agent per tick. Hoist it.
- **`RunSearch` running length.** The route search re-measures lengths it has already walked;
  carry the running length instead.
- **A preempted agent stops hard.** It can be told to stop inside its own braking distance
  (measured: 1525 uu given against 2500 needed). The follower clamps rather than overshoots,
  so it is safe — just not smooth.
- **`ConnectGuidelines`/`DisconnectGuideline` never `Commit()`.** Pre-existing, not M2: both
  open an `FRoadEditScope` and never commit it, so a hand-drawn guideline link cannot be
  undone.
- **The builder's skip-when-absent path is untested.** Safe via the orphan sweep, traced by
  hand, not measured.
- **"Free at some tick" assertions.** Several crossing-hold assertions are weaker than their
  prose claims: they check that a resource was free at SOME tick rather than at the tick the
  prose names. Tighten them to the frame in question.

### Pre-existing bug found (not M2, proposed GitHub issue)

**Title:** `ConnectGuidelines`/`DisconnectGuideline` never `Commit()` — hand-drawn links not
undoable. Both open an `FRoadEditScope` and never call `Commit()` on it; a hand-drawn
guideline link a player draws or removes cannot be undone. Found incidentally during Task 10
(hold-short marks); out of M2 scope.

### Runtime verification (not done this session — no editor, no MCP)

The editor was closed and no MCP server was reachable during Task 12, so this milestone is
**builds, tests green, unverified at runtime**. To verify in PIE:

1. Open the editor, PIE.
2. Key `6` (Runway): draw a runway. Key `5` (Guidelines): draw a taxiway guideline crossing
   it partway along. Key `3` (Stand): place two stands reachable from the taxiway.
3. Key `8` (Hold short): click the taxiway node right next to where it meets the runway — a
   hold bar should appear across the taxiway there.
4. Key `7` (Land): dispatch an arrival. While it is still rolling down the runway, press `7`
   again to dispatch a second arrival.
5. Grep `Saved/Logs/AirportMgr.log` for, in order:
   - `Arrival refused: the runway is in use.` — the second arrival refused while the first
     still holds the strip (or both land in sequence if the first vacated first).
   - `Agent N holding short at node` — exact format `"Agent %d holding short at node %d for
     runway segment %d held by agent %d"` — the second aircraft stopped at the bar.
   - `Agent N stops ... short of` — exact format `"Agent %d stops %.0f uu short of %s held by
     agent %d"` — any arbitration stop, at the bar or a junction.
   - `Agent N resumes` — exact format `"Agent %d resumes"` — it moves again once clear.
   - `released the runway` — exact format `"Agent %d released the runway"` — the first
     aircraft's vacate.
   - `Graph rebuilt:` — exact format `"Graph rebuilt: %d agents re-resolved, %d replanned,
     %d truncated, %d stranded"` — appears after any road edit once agents exist; delete or
     add a taxiway segment near an agent to trigger it, and check case-5's teleport minor
     above while there (an agent should not jump more than about one tick's travel).
   - `Hold short set at guideline node` — exact format `"Hold short %s at guideline node %d
     for segment %d"` (`%s` is `set` or `cleared`) — logged when the key-8 click commits.
   - `Deadlock among agents` — needs three-plus agents in a cycle (e.g. a one-way taxiway
     loop with no escape); not exercised by the two-aircraft recipe above.
6. Key `4` (Route) optionally dispatches a ground vehicle onto the taxiway to see it yield
   to an aircraft at the crossing (`NodeYield`/`PriorityOverride` in PIE, not just in tests).
