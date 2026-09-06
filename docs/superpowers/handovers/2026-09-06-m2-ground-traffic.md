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
- `UAirsideTraffic::Advance(DeltaSeconds, SurfaceZ)` is the one tick; the actor scales
  `DeltaSeconds` by `SimTimeScale` (the x0..x8 multiplier only, never the day compression).
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
