# Entity Inspector — Design

**Status:** design, approved in brainstorm 2026-09-07. Retires the Route tool; adds a
Select tool, a facts layer, a Depart action and an inspector panel.

**Parents:** `2026-09-05-game-systems-map-design.md` (§5.1 names "flight and stand
panels" as M3 screens - this pulls them forward onto raw agents),
`2026-09-06-build-bar-hud-design.md` (the UMG recipe the panel copies),
`2026-09-06-ground-traffic-design.md` (agents, phases, redirect).

## 0. Nothing here is law

As the systems map §0: every decision below was the best call at the time of writing.
Change it for a reason, after discussion, and record the reasoning where the old
decision was.

## 1. Why

The Route tool was the only tool that built nothing: it asked the airport a question
(click a start, click a goal, watch it drive) and was also, by accident of history, the
ONLY way to make an aircraft depart. Arrivals moved to the bar's Land button; departures
never followed. Meanwhile nothing on screen says what an aircraft is doing - phase,
speed, who it is waiting on - which every PIE round of M2 needed and got from the log.

The game that is coming (systems map §3.2) has flights with phases, stands with
occupants and vehicles with jobs. The player-facing shape of all of them is the same:
click a thing, see its facts, press a verb. Building that shape now, on the raw
`FRoadAgent`, gives M3 a panel to fill rather than a panel to design.

## 2. Decisions taken in brainstorm

- **Select is the default state (Cities Skylines).** Registry index 0, so the session
  starts in it; cancelling out of an IDLE build tool returns to it; activating a build
  tool clears the selection. Alternatives rejected: Select as just another tool
  (must pick it before clicking anything); selection orthogonal to tools (every build
  click near a stand gets a competing meaning).
- **Depart chooses the runway automatically**: every runway that admits the airframe,
  shortest taxi wins. One button. M3's sequencer replaces the chooser, not the panel.
  Rejected: a button per runway (a second list to keep in step with the network); a
  button-then-click gesture (reintroduces the modal step being retired).
- **v1 actions are Depart and Follow only.** "Land here" on a stand and "Remove" on an
  aircraft were considered and deferred: the first needs a target-stand parameter on the
  arrival planner, the second is a debug verb.
- **Panel lives in the game module**, not `AirportOps/Present` as the systems map says.
  AirportOps has no `UFlight` yet; a panel there would describe Airside agents through a
  relay that adds nothing. The seam that survives into M3 is the FACTS STRUCT (§4), not
  the module the widget sits in.
- **Aircraft are picked in screen space**, not by collision. Roads carry none by design
  (procedural-road spec §6.2), the HUD already reasons in pixels, and a pixel radius is
  what keeps a distant aircraft clickable.

## 3. Selection and picking

### 3.1 `FSelection`

```
struct FSelection { ESelectionKind Kind = None; int32 Id = 0; }   // Aircraft: agent id. Stand: entity index.
```

Lives on `FBuildSession`. WRITTEN ONLY by the Select tool; read by the panel and the HUD.
Cleared when: a build tool becomes active (`FBuildSession::SelectTool` to any index but
0); the Select tool receives Cancel; the selected agent reaches `Gone`; the selected
stand is deleted. The last two are checked by the tool's `Tick` against the target, not
by subscribing to events - a tool has no delegate lifetime to manage and polls anyway.

An enum and an id, not two optional ids: an aircraft and a stand can never both be
selected (CLAUDE.md, "a phase is an enum, never a set of bools").

### 3.2 `FSelectTool : IBuildTool`

Registry entry `{ EKeys::Four, "Select", FSelectTool }` at INDEX 0. Everything else
shifts down one index; keys are unchanged because the registry names keys explicitly.

- `OnClick`: if `Context.HoverAgent != 0`, select that aircraft. Else
  `Target->FindEntityAt(Cursor, ToolPickRadius)`; if found, select that stand. Else
  clear. Aircraft beats stand because a parked aircraft covers its stand and the smaller
  target should win.
- `OnCancel`: clear the selection. `IsIdle()` is `Kind == None`, so a second cancel with
  nothing selected falls through to the controller (which does nothing in Select; in a
  build tool it now returns to Select - §3.4).
- `BuildPreview`: `Marker` at the hovered pickable in a new `EPreviewStyle::Hover`;
  `Marker` at the selection in `EPreviewStyle::Selected`; the selected agent's remaining
  route in the existing `EPreviewStyle::Route` (the one useful picture the Route tool
  drew). Styles name meanings, the HUD picks colours, as today.
- `Tick`: drops a selection whose target has gone (§3.1).

### 3.3 Hover agent: the controller projects, the tool decides

`FToolContext` gains `int32 HoverAgent = 0`. The controller fills it every frame before
`MakeToolContext` returns: project every live agent's actor location to the screen, take
the nearest within `AgentPickPixels` (a UPROPERTY, default 24) of the cursor.

The rule "nearest of N screen points within a radius" is a free function
`NearestWithin(TArrayView<FVector2D> Points, FVector2D Cursor, double Radius)` in
`Tool/ScreenPick.h`, world-free and tested. Only the projection call sits in the
controller. The tool itself never sees a camera, so it stays in the plugin.

Why not road-plane: an arrival on final is 2000 uu up and a plane-cursor lands on the
grass under it. Why not the agent's road-plane `Position`: same reason, plus the
follower's position is the nose gear, and clicking the tail should still work.

### 3.4 Cancel returns to Select

`ARoadBuildController::OnCancelGesture` (and the editor mode's Escape) today: if the
active tool is not idle, tool cancels; else nothing. New: else, if the active index is
not 0, `SelectTool(0)`. Two cancels from mid-gesture in a build tool reach Select; one
cancel from an idle build tool does. Same rule for both drivers.

## 4. Facts — `Model/InspectFacts.h`

Pure functions of `const UGroundTraffic&` + `const URoadNetwork&`. No widget reads
`FRoadAgent`; the widget reads these. When M3's `UFlight` exists it fills the same
struct (airline, scheduled off-block) and the panel does not change shape.

```
struct FAgentFacts {
    int32 Id; FString TypeName;          // Airframe's definition name, or the class
    EAgentPhase Phase;
    double HeadingDegrees;               // compass, from LastMotion.Heading
    double GroundSpeed;                  // uu/s; the panel formats m/s and kt
    double Altitude;                     // uu above the surface
    FString Destination;                 // "Stand 3", "Runway 09", or "Node 41"
    FString Status;                      // see below
    bool bEngineRunning;
    bool bCanDepart;                     // Phase == Parked
};
struct FStandFacts {
    int32 Index; FString SizeClass;      // from FAirsideCapability's stand summary
    double DesignWingspan;
    int32 OccupantAgent;                 // 0 when empty
    int32 AnchorCount;
    bool bReachable;                     // pose node has at least one edge
};
```

**Status line**, one of, first match wins: `Departure armed` (bDepartureArmed, Taxiing);
`Holding for aircraft N` (WaitingOn != 0); `Crossing runway` (CrossingPhase != None);
`Shutting down (Ns)` (Parked, ShutdownCountdown > 0); `Parked`; `On final` / `Landing
roll` (Arriving, by bAirborne); `Rolling` / `Climbing` (Departing, by bAirborne);
`Taxiing`. A string, not an enum: it is presentation of several orthogonal model facts
and no code branches on it.

**Destination**: the goal node, named. A stand when some `FEntityInstance::PoseNode`
equals it; a runway when `RunwayExtentAt(node position)` - designator from
`RunwayDesignator`; else the node index. `FindStandByPoseNode` is added to
`URoadNetwork` for the first case - a linear scan over entities, fine at these counts.

**Occupant**: the agent whose `GoalNode` is this stand's pose node and whose phase is
Parked or Taxiing (inbound). Reported, not stored: an occupancy FIELD on the entity
would be a second source of truth the traffic model would have to keep in step. M3's
`UStandAllocator` gets to decide whether reservation becomes a stored fact.

## 5. Depart

### 5.1 `DeparturePlanner::PlanAny`

```
FDeparturePlan PlanAny(const URoadNetwork&, FGuidelineNodeId From, const FAirframe&, ETraversalClass);
```

For each runway chain (one per seed: walk `RunwayChain` from every runway segment,
dedupe by seed), `Plan` toward the chain's threshold and keep the VALID plan with the
shortest `Route.Length`. When none is valid, return the first refusal so the log names
a reason ("NotAdmitted: grass strip, needs tarmac") rather than "no runway". Both
thresholds of a chain are candidates: `Plan` already picks entry and backtrack per
threshold, so this is two calls per chain.

### 5.2 `UGroundTraffic::DepartAgent`

```
EDepartureRefusal DepartAgent(int32 AgentId, const URoadNetwork& Network);
```

Refuses with a new `EDepartureRefusal::NotParked` (logged) unless `Phase == Parked`: a
taxiing aircraft has a plan, an arriving one is not on the ground, a departing one is
already going. A new value rather than reusing `NoRoute`, which would name a cause that
is not the cause. `PlanAny` from the
agent's `GoalNode` (the pose node it parked at). Then the existing `RedirectAgent`, which
already rewrites the follower and calls `ArmDepartureIfRunway` - no second arming path.
One `LogAirsideTraffic` line per attempt, success or refusal, with the designator chosen.

**Engine**: `FRoadAgent::StartTaxi` sets `bEngineRunning = true`. Today a parked
shutdown clears it and nothing on redirect sets it back - the Route tool never redirected
a parked aircraft, so it was never seen. The test in §8 pins it.

Forwarded as-is through `UAirsideTraffic::DepartAgent` and `ARoadNetworkActor::
DepartAgent`. NOT on `IRoadEditTarget`: that interface is what tools drive, and no tool
calls this - the panel does, through the actor the controller already holds.

## 6. Panel and actions (game module)

### 6.1 `UInspectorWidget : UUserWidget`

Same recipe as `UBuildBarWidget`: C++ base with `BindWidgetOptional` slots (`TitleText`,
`FactsText`, `StatusText`, `DepartButton`, `FollowButton`), a Widget Blueprint subclass
for layout and style, created by the controller in `BeginPlay` beside the bar. Docked
bottom-left above the bar by default; the Blueprint moves it.

Refreshes in `NativeTick` from the facts struct while `Selection.Kind != None`, hidden
otherwise. Polled, not event-driven: the bar's enabled states were event-driven once and
went stale; a panel showing speed needs every frame anyway.

Depart is visible only for an aircraft and enabled iff `bCanDepart`. Follow is visible
for an aircraft.

### 6.2 Actions

Two entries in the one `BuildActions()` table, new section `EActionSection::Selection`
between Aircraft and Game:

| Id | Key | Execute | IsEnabled |
|---|---|---|---|
| `Depart` | none (bar/panel only) | `Target->DepartAgent(Selection.Id)` | `Kind == Aircraft && Facts.bCanDepart` |
| `Follow` | C (moves from "watch newest") | `ToggleWatchAgent()`, retargeted | `Kind == Aircraft || HasAgent()` |

`ToggleWatchAgent` follows the SELECTED agent when one is selected, else the newest as
today. The panel's buttons execute the table entries; the bar shows them too (the
Selection section is empty-looking when nothing is selected, as Undo is greyed with
nothing to undo). One list, three consumers.

## 7. Retirement

Delete `Tool/RouteTool.h`, `Tool/RouteTool.cpp`, `RouteToolTest.cpp`. The route-tool cases in
`GuidelineOverlayTest` and `ToolCursorTest` are rewritten against the taxiway tool and the
session's context contract respectively, and dropped where they tested routing.

*Amended 2026-09-07 (Task 1):* `AirframeFor` is DELETED with the tool, not moved to
`Model/`. It includes `Entities/AircraftType.h` and `Content/AirsideSettings.h`, both of
which `Model/` is forbidden (Check-Architecture rule 1), and the Route tool was its only
caller. Its test (`Airside.Tool.RouteTool.DefaultAirframe`) pinned that the fallback
agreed with `UAirsideSettings::ResolveDefaultAirframe`; with one resolver and no second
caller there is nothing left to agree. Check-Architecture rule 4 (one Piper fallback site)
still holds.

`EPreviewStyle::Route` stays; the Select tool emits it for the selected agent's route.

**Accepted loss:** there is no way to taxi a vehicle between two arbitrary nodes any
more. Vehicles do not exist yet; M3's job board dispatches them with `DispatchAgent`
directly. If a debug driver is wanted before then, it is a console command, not a tool.

## 8. Tests

Model (`NewObject`, no world):
- `PlanAny` on a two-runway network where one refuses admission picks the other; on two
  admitting runways picks the shorter taxi; on none returns the first refusal.
- `DepartAgent`: refused for Arriving/Taxiing/Departing; a Parked agent ends Taxiing
  with `bDepartureArmed` and `bEngineRunning`; ticks on to Departing then Gone.
- `FAgentFacts` status for each phase and for WaitingOn / Crossing / armed; destination
  names a stand, a runway, a bare node.
- `FStandFacts` occupant: empty, inbound-taxiing, parked; not the agent that departed.

Tool:
- `ToolRegistry()[0]` is Select and a fresh session's active index is 0.
- `NearestWithin`: nearest wins, outside radius is none, empty is none.
- Select picks hover-agent over a stand under the cursor; picks the stand with no hover;
  clears on empty click and on cancel; selection cleared when another tool activates.
- Tick drops a selection whose agent is Gone.

Composition (spawn `ARoadNetworkActor`, tick it):
- Land, tick to Parked, `DepartAgent`, tick to Gone. Fails if forwarding is unwired.
- A real `UInspectorWidget` bound to the actor reports Depart disabled while Arriving and
  enabled at Parked. Fails if the panel reads nothing.

Pre-commit: `Run-AirsideTests.ps1` (its `N run, N failed, N crashed` line) and
`Check-Architecture.ps1`. The PR records the `UE_LOG(` count before and after the Route
tool's deletion, and names the lines that went with it.

## 9. Out of scope, recorded

- Vehicles as a selectable kind: `ESelectionKind::Vehicle` is added when one exists;
  the facts struct is the same shape.
- Multi-select, drag-select, hover tooltips.
- Selecting a runway, apron or taxiway segment. The build tools' Remove modifier still
  owns those.
- Stand actions ("Land here"), Remove agent.
