# Unstick an agent - design

2026-09-29. Branch `feature/unstick-agent`, from origin/main c67699dc.

## Why

Routing keeps getting better and agents still get stuck: deadlocks the resolver cannot break,
and `EAgentPhase::Stranded`, which is FINAL by design ("the honest answer is that the player
retires it" - GroundTrafficRebuild.cpp) while no UI can retire anything. A stranded service
vehicle also keeps its job for ever (`UJobBoard::OnAgentPhase` returns on `To != Parked`;
`DriveVehicleTo` treats Stranded as "finish the leg", and it never will).

The player needs an escape hatch on the inspector, in escalating order: **Replan**, **Send
home**, **Despawn**.

## Rulings (user, 2026-09-29)

- Aircraft have no depot; their middle action **re-seeks a stand**.
- UI is **one Unstick button** opening a small menu of the three actions.
- A despawned aircraft's flight ends **Cancelled**, a new terminal `EFlightPhase`, not Departed.
- Available for any selected agent, not only stranded ones (a deadlocked Taxiing agent is stuck
  too). Despawn is always allowed, even airborne or on the roll - it is the hatch of last resort.
  Decided by the assistant with the user's go-ahead; revisable.

## Actions

| Action | Vehicle | Aircraft |
|---|---|---|
| Replan | Taxiing: `ReplanAt` - held at a step: splice there, banning what refused it (the deadlock resolver's own ban); moving freely: splice at the next step, no ban. Stranded: rescue in place toward its OWN goal. | same |
| Send home / Find stand | Reopen its current + queued jobs, then `GoToFacility`. Stranded now drives home through the rescue instead of "finish the leg". | Stranded, or Parked awaiting a stand: `ChooseStand` a stand, rescue toward it. Otherwise refused with a reason (Taxiing: "moving - use Replan"; at a stand: "already on a stand"). |
| Despawn | Reopen its jobs, `RetireAgent`, vehicle Idle at home. | Flight -> `Cancelled` -> history, then `RetireAgent` (claims released by `ReleaseAll`; a parked aircraft's turnaround drops through the existing Parked -> Gone path). |

Refused in Arriving (on approach / roll-out), Departing and Manoeuvring for Replan and Home:
the runway and pushback own those, and neither is a route a replan can touch.

A refusal is never silent: `CanUnstick` returns the reason, the menu greys the entry with it as
tooltip, and the action logs it.

## Components

### Airside: `UGroundTraffic::RescueStranded(AgentId, Network, Goal)`

The one new movement primitive. Moves `RejoinNearby` (GroundTrafficRebuild.cpp, anonymous
namespace) onto `FPlanReResolver` so a second caller can use it, with the goal as a parameter
rather than read off the agent. Snaps a Stranded agent onto the nearest live edge running its way
within `RescueRejoinRadius`, routed to Goal (unset = its own goal), `RejoinTaxi`, and the same
aftermath as the split rejoin (reservations and guideline claims released, arbitration and stall
cleared). Goal moved through `ReleaseGoal` / `TakeGoal` so claims and departure arming stay on
the one path.

`RescueRejoinRadius = 1500 uu`: the stranding itself means no node within `ResolveRadius`, so the
rebuild radii (300, 1000) find nothing by construction; 15 m is a visible hop the player asked
for, and still short of the next road over. Beyond it: refused, "no pavement within 15 m running
its way" - Despawn is what is left.

This is the stranding comment's "choose a place to teleport it to", now answered because the
player chose it. That comment is updated to say so.

### AirportOps: `UAgentRescue`

New Model/ subobject of `UOpsRuntime` (which grows by forwarding - a pointer and a line in Attach).
Holds nothing of its own; composes GroundTraffic, JobBoard and FlightBoard:

```cpp
enum class EUnstickAction : uint8 { Replan, SendHome, Despawn };
struct FUnstickVerdict { bool bAllowed = false; FText Why; };

FUnstickVerdict CanUnstick(int32 AgentId, EUnstickAction) const;
FUnstickVerdict Unstick(int32 AgentId, EUnstickAction);   // bAllowed = it happened
```

Per-body rules in two private functions (vehicle, aircraft), not a strategy class: two bodies,
three actions, one table - a policy hierarchy would be six one-line classes.

Every call logs one `LogAirportOps` line: `Unstick: agent N (Vehicle|Aircraft, <phase>) <action> -> done|refused: <why>`.

### AirportOps: JobBoard

- `UJobBoard::RecallVehicleOfAgent(AgentId, bRetire)`: reopen `CurrentJob` and the queue, then
  `GoToFacility` or retire and set Idle at home. The explicit form of what `SyncFleet` does a
  tick late after an external retire.
- `DriveVehicleTo`: Stranded is no longer "finish the leg"; it goes through `RescueStranded`
  toward the goal, and retires where it stands if that refuses (the Parked branch's fallback).
- `OnAgentPhase`: `To == Stranded` for a vehicle reopens its jobs and recalls it - the follow-up
  from #399. Its own small fix, but it is the same seam.

### AirportOps: FlightBoard

- `EFlightPhase::Cancelled`, terminal, after `Expired`. Flight.h's comment ("Cancelled is
  deliberately ABSENT ... a phase nothing can enter is a lie") is amended: the player's despawn
  enters it now; the sequencer's future cancellations will share it.
- `UFlightBoard::CancelByAgent(AgentId, Now)`: Phase = Cancelled, clear ByAgent / AgentId,
  `MoveToHistory`. Called BEFORE `RetireAgent`, so the Gone that follows finds no flight and
  cannot book it Departed.
- The terminal lists in comments and `RearmSchedules` (FlightBoard.cpp:429) gain Cancelled.

### Game module: inspector

- `BuildActions()`: new row `selection.unstick`, found BY ID in `UInspectorWidget::EnsureSlots`.
- Button "Unstick", highlighted when the agent is Stranded or has stalled past
  `UnstickHighlightSeconds` (15 s, a UPROPERTY on the widget; `Rules.StallSeconds` is 3 s,
  which would light up every queue at a hold).
- Click opens a menu at the button (`UMenuAnchor`, like `UUiDropdown`): Replan / Send home (or
  Find stand) / Despawn. Disabled entries greyed, `Why` as tooltip.
- Despawn is two clicks: the first relabels it "Despawn - click to confirm"; closing the menu
  resets it (destructive gestures need a deliberate step).
- Controller: `ARoadBuildController::UnstickSelected(EUnstickAction)` and `CanUnstickSelected`,
  forwarding to `UOpsRuntimeSubsystem` -> `UAgentRescue`.

## Testing

World-free where the logic lives:

- Airside: `RescueStranded` - a stranded agent beside a live edge rejoins and reaches its goal;
  one with nothing within the radius is refused and unchanged; a new goal moves the claim.
- AirportOps `AgentRescue`:
  - Replan: a held Taxiing agent is replanned with the ban; a free-moving one with the same best
    route is refused "already on its best route".
  - Vehicle SendHome from ToJob and from Stranded: job Open again, vehicle ToFacility / home.
  - Vehicle Despawn: agent gone, job Open, vehicle Idle - in the same tick, not the next.
  - Aircraft FindStand from Stranded: redirected to a free stand.
  - Aircraft Despawn: flight in History as Cancelled, not Departed; runway claim released.
  - Refusals: Arriving/Departing refuse Replan and Home with a reason; Despawn is allowed.
- JobBoard: a vehicle going Stranded has its job reopened (the #399 follow-up).
- Composition (game module): the inspector's action list contains `selection.unstick` and
  `EnsureSlots` finds it (the "consumed, not declared" check).
- PIE, by hand: strand a truck and an aircraft (delete the pavement under them), each action in
  turn; the `Unstick:` log lines are the evidence.

## Out of scope

- Automatic unstick (the resolver already is one; this is the manual hatch).
- A reputation or money penalty for a cancelled flight - the phase is the hook.
- Re-seeking a stand for a MOVING aircraft (needs an aircraft-safe `RerouteAgent`; Replan covers
  an unreachable stand only if a route exists at all).
