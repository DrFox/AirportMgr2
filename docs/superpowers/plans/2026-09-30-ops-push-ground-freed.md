# Ops push-ground-freed plan: a blocked pushback wakes the job board by event, not every frame

Follow-up to ops batch 3 PR D (item 6 finding: `DepartTheReady`'s per-frame retry waits on `PushbackBlocked`, which no
event covered). Branch `feature/ops-push-ground-freed` (worktree `C:\repos\airportmgr2-ops-batch3`), stacked on
`feature/ops-per-frame` (#420). Baseline full suite: `1571 test(s) run, 0 failed, 0 crashed`.

Baseline UE_LOG / comment lines at `ab5e7a40`: GroundTraffic.h 0/775, GroundTraffic.cpp 34/616,
GroundTrafficRebuild.cpp 21/519, AirsideTraffic.h 0/188, AirsideTraffic.cpp 7/97, OpsEventBus.h 2/159,
OpsEventBus.cpp 8/9, JobBoard.h 0/447, JobBoard.cpp 23/197, ServiceJob.h 0/134, OpsRuntime.h 0/193, OpsRuntime.cpp 25/366.

## Premises checked against the code (2026-09-30)

- **The push route is NOT a fixed function of the parked aircraft.** `DepartAgent` builds it as
  `PlanAny(Network, GoalNode, Airframe, Class, &Occupancy)` -> straight-out test on `LastMotion.Heading` ->
  `PushbackPlanner::Plan(Network, GoalNode, Plan, ...)` -> `IsPushGroundFree(Id, Push.PushRoute, Push.PushRoute.Length)`.
  `PlanAny` RANKS runways free-before-held (`IsAnyHeld(RunwaySurfaces(End.Seed))`, the same question as
  `ArrivalPlanner::IsChainHeld`), and the push reverses onto the arm the chosen departure does NOT take. So a runway
  going held or free can move the push to the other arm. Re-asking a stored route alone would miss "runway A got busy,
  B is free and B's push arm is clear" - DepartAgent would say yes, the watch would not.
- **So the watch re-asks identically, cheaply.** The planning half of DepartAgent moves into one file-local function
  (`AskDeparture`) that DepartAgent and the diff both call - ONE copy of the decision. Its inputs are the network
  (object, `GetEditRevision`, `GetGuidelineRevision` - topology, facts through the facade (rule 35), and
  AreGuidelinesBehindRoad), the parked agent (goal node, airframe, class, heading: fixed while Parked), the rules, and
  the occupancy ONLY through (a) the held-runway set and (b) `IsPushGroundFree` on the push route. The entry stores the
  push route and that key; the diff re-asks `IsPushGroundFree(stored route)` while the key holds, and re-runs
  `AskDeparture` when it moves. Identical by construction: every input to the plan is either in the key or re-read.
- **"Freed" means "DepartAgent would no longer answer PushbackBlocked"** - ground free, now straight out, or refused for
  another reason (the graph moved). The last is a spurious wake that costs one Step and gets the new refusal logged
  once; missing it would strand the aircraft. Same trade DiffFreedom already makes for a renamed runway seed.
- **Rebuild: RE-DERIVE, not clear.** A rebuild moves the guideline revision, so every entry replans in the rebuild's
  own `DiffFreedom(Network, true)`. Clearing would make Airside's answer depend on ops re-registering the aircraft
  through a NetworkChanged-driven retry - a contract across the plugin boundary Airside must not know exists (and
  an Airside-only caller would lose the aircraft). Re-deriving costs one `AskDeparture` per watched aircraft per edit.
- **The job-board deadline covers a turnaround that becomes due unserved.** `ArmJobBoardDeadline` runs after every
  Step and books `NextDeadline`, which includes every turnaround's `TurnaroundEndsAt > Now`; turnarounds are opened in
  `OnAgentPhase(Parked)`, whose bus subscriber marks the JobBoard pass dirty, so the Step that follows books it. A
  turnaround still being served at its deadline departs from the Step that finishes its job (FinishServe/AssignOpenJobs
  inside Step, DropAircraft from a phase event that dirties the pass). Pinned below by the NoPushbackRoute quiet test's
  control (the deadline itself departs nothing blocked, but runs exactly one Step).
- **`FTurnaround::LastDepartureRefusal` is saved** (UPROPERTY). A load's MarkAllDirty runs the pass, whose Step retries
  the departure and re-registers a PushbackBlocked with the (unsaved, empty) watch. No extra load path needed.
- **Item 5 (vehicle "unresolved") - analysis first, tests second.** `StartNext` leaves `Idle && Queue.Num() > 0` only
  on (i) an open prerequisite - `FServiceJob::Prerequisites` is never populated outside tests - or (ii) "refused at home
  with a route there", which needs `DepotRoute(...).IsValid()` (Result == Found, so not SameNode) and
  `DispatchAgent` refusing it (`!IsDrivable()`: fewer than two polyline points or zero length) - a degenerate found
  route. It leaves `Serving && CurrentJob == 0` only on (i). Timed-and-due at the END of a Step is self-limiting: the
  next Step's due loop moves the vehicle on. Expected: no per-frame repeater reachable in production. Tests stage each
  part and assert the board goes quiet; if one repeats, it is fixed with the trigger that changes its answer.

## Files

| File | Change |
|---|---|
| `Airside/Public/Model/GroundTraffic.h` | `FOnPushGroundFreed OnPushGroundFreed` (int32 AgentId); `int32 PushWatchCountForTest() const`; private `FPushWatch` (route + key), `TMap<int32, FPushWatch> PushWatch`, `uint32 HeldRunwaysChanges` |
| `Airside/Private/Model/GroundTraffic.cpp` | `AskDeparture` (file-local, the planning half of DepartAgent); DepartAgent logs from it, registers/drops the watch; `DiffFreedom` re-asks the watch after stands, broadcasts last |
| `Airside/Public/Present/AirsideTraffic.h/.cpp` | relay `OnPushGroundFreed` |
| `AirportOps/Public/Model/OpsEventBus.h`, `Private/Model/OpsEventBus.cpp` | `FPushGroundFreedEvent { int32 AgentId; "PushGroundFreed" }` into `FOpsEvent` |
| `AirportOps/Public/Model/JobBoard.h`, `Private/Model/JobBoard.cpp` | `Step` drops `bDepartureWaiting`; `DepartedLastStep()`; `HasRefusedDeparture(Now)`; comments made true |
| `AirportOps/Public/Model/ServiceJob.h` | `LastDepartureRefusal`'s "runs every tick" comment |
| `AirportOps/Public/Present/OpsRuntime.h/.cpp` | bind/unbind + publish; `DirtyJobBoard()` (sets `bJobBoardCovered`) at every JobBoard dirtier; PushGroundFreed subscriber in WireBus; the net split into two wanted halves (queue, departures), one clock entry; `CancelSafetyNet()` on load/detach; the JobBoard pass logs the safety Warning |
| tests | new `AirsideTests/Private/PushGroundFreedTest.cpp`; new `AirportOpsTests/Private/PushGroundFreedPassTest.cpp`; `FuelServiceTest.cpp` gains the vehicle quiet tests |

## Interfaces

```cpp
// GroundTraffic.h (public)
DECLARE_MULTICAST_DELEGATE_OneParam(FOnPushGroundFreed, int32 /*AgentId*/);
FOnPushGroundFreed OnPushGroundFreed;
int32 PushWatchCountForTest() const { return PushWatch.Num(); }

// GroundTraffic.h (private)
struct FPushWatch
{
	FRoutePlan PushRoute;                             // what IsPushGroundFree is re-asked of
	TWeakObjectPtr<const URoadNetwork> Network;       // identity only
	uint32 EditRevision = 0, GuidelineRevision = 0;
	TSet<FRoadSegmentId> RunwaysHeld;                 // PlanAny's occupancy input, at plan time
};
TMap<int32, FPushWatch> PushWatch;

// GroundTraffic.cpp (file-local)
struct FDepartureAsk { FDeparturePlan Plan; bool bStraightOut; double OffDegrees; FPushbackPlan Push;
                       EDepartureRefusal Why; };   // Why == PushbackBlocked iff the push ground is held
FDepartureAsk AskDeparture(const UGroundTraffic&, const FRoadAgent&, const FAirframe&, const URoadNetwork&);

// OpsEventBus.h
struct AIRPORTOPS_API FPushGroundFreedEvent { int32 AgentId = 0; EventName "PushGroundFreed"; Describe "aircraft %d" };

// JobBoard.h
const TArray<int32>& DepartedLastStep() const;     // DepartTheReady's accepted departures, this Step
bool HasRefusedDeparture(double Now) const;         // due, unserved, LastDepartureRefusal != None

// OpsRuntime
void DirtyJobBoard();   // bJobBoardCovered = true; MarkDirty("JobBoard")
// net: bQueueNetWanted / bDepartNetWanted; one Clock->Every(SafetySeconds); CancelSafetyNet() on load/detach
```

Diff (end of DiffFreedom, after the stand baseline): for each watch entry, drop silently if the agent is gone or not
Parked; if the key moved (network object, either revision, `HeldRunways` != `RunwaysHeld`), `AskDeparture` and refresh
the entry (still blocked) or free it (anything else); else `IsPushGroundFree(Id, PushRoute, PushRoute.Length)`. Freed
ids are removed, THEN broadcast (baselines first, like the runways). Cost: one `IsAnyHeld` over ~2 x steps resources per
watched aircraft per call - a push is 2-4 steps, so ~8 table lookups; 0-2 aircraft watched in any test field
(2026-09-30). A replan (PlanAny: one route search per departing runway) only when the key moves.

## Tasks (TDD: test, build, run red, implement, green, commit)

1. **Plan** - this file. Commit.
2. **Airside watch.** Declare `OnPushGroundFreed` + `PushWatchCountForTest` (returns 0), new `PushGroundFreedTest.cpp`
   (two builds). `Airside.Model.Traffic.PushGroundFreed.TaxiingBlockerClears` (FPushbackGraph extended south: a second
   aircraft taxis up the push arm; refused PushbackBlocked; none while it is on the ground; exactly one on the frame it
   clears; DepartAgent then accepts), `.RetiredWatchedFiresNothing`, `.ReleaseOutsideAdvance` (a HoldStand on the push
   arm's node, ReleaseHold -> fires with no Advance), `.RunwayFlipReplans` if the fixture can show it (two runways,
   opposite arms), `.RebuildRederives`. Red: counts 0. Implement; green. Relay on UAirsideTraffic.
3. **Event + bridge.** `FPushGroundFreedEvent`; Attach binds, Detach unbinds. Test `AirportOps.Present.Bus.PushGroundFreedIsBridged`.
   `EveryEventHasASubscriber` red until task 4.
4. **Job board.** Subscriber (`DirtyJobBoard`), `bDepartureWaiting` removed, comments fixed, `DepartedLastStep`,
   `HasRefusedDeparture`, net extended, safety Warning. New `PushGroundFreedPassTest.cpp`:
   `AirportOps.Present.PushGroundFreed.DepartsTheFrameAfter` (composition, spy: no Warning),
   `.NoPushbackRouteIsQuiet` (0 Steps over 150 frames; red today: 150; drawing the arm departs it),
   `.SafetyNetDepartsAMissedOne` (blocker released by hand, no event -> the net departs it after 30 game s, one Warning),
   `.NetArmedAndCancelled` (armed by a refusal, cancelled by load and detach).
5. **Vehicle quiet tests** (item 5) in FuelServiceTest.cpp: `AirportOps.Fuel.QuietBoard.TimedStepDue`,
   `.IdleWithAQueue`, `.ServingWithNoJob` - each staged part settles within 2 Steps; report.
6. **Mutations**, each red, restored with cp + touch: watch diff off (DepartsTheFrameAfter via the Warning); bridge off
   (PushGroundFreedIsBridged, DepartsTheFrameAfter); dirtier off (DepartsTheFrameAfter); bDepartureWaiting back
   (NoPushbackRouteIsQuiet); safety Warning off (SafetyNetDepartsAMissedOne).
7. **Full suite**, counts, report.
