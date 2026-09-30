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

## Execution notes (2026-09-30)

Full suite: `1584 test(s) run, 0 failed, 0 crashed` (+13: 5 Airside, 5 ops composition, 3 fuel quiet guards). Check-Architecture
PASS, rule-12 warnings 111 (unchanged).

Red lines before implementation:

- `Airside.Model.Traffic.PushGroundFreed.*` (watch not registered): TaxiingBlockerClears "and it is watched" expected 1, got 0;
  "it fired once, when the ground cleared" 1, got 0; ReleaseOutsideAdvance "the release fires at once, with no Advance" 1, got 0;
  RunwayFlipReplans "runway 1 busy: ... fired" 1, got 0; RebuildRederives "and keeps the aircraft watched" 1, got 0.
  RetiredWatchedFiresNothing is a guard (its red is the mutation below).
- `AirportOps.Present.Bus.PushGroundFreedIsBridged`: "a freed push ground is published onto the bus" expected 4, got 3.
- `AirportOps.Present.PushGroundFreed.NoPushbackRouteIsQuiet`: "five quiet seconds run no job board step" expected 3, got 153.
- DepartsTheFrameAfter, SafetyNetDepartsAMissedOne, NetArmedAndCancelled: written with the pass; their reds are the mutations.
  (First run also showed the rig's own error: a hold on the taxiway's far end is off the push ground - the push is 12490 uu and
  ends at FAnchorLink's back tangent node, so the rig blocks that node, found by position.)

Mutations (each restored by copy + touch, rebuilt, green after):

| Mutation | Red |
|---|---|
| watch diff never frees (`FreedPushes.Add` off) | all four Airside firing tests; `DepartsTheFrameAfter` ("no safety Warning" expected 0, got 1; "freed -1, departed 930") |
| runway-held key off the watch | `RunwayFlipReplans` ("fired" 1, got 0) |
| rebuild clears the watch | `RebuildRederives` ("keeps the aircraft watched" 1, got 0) |
| a gone agent fires instead of dropping | `RetiredWatchedFiresNothing` ("nothing fired for the retired aircraft" 0, got 1) |
| Attach bridge off | `PushGroundFreedIsBridged` (4, got 3); `DepartsTheFrameAfter` ("freed 243, departed 930"; Warning 1) |
| PushGroundFreed subscriber a no-op | `DepartsTheFrameAfter` ("freed 243, departed 930"; Warning 1) |
| `bDepartureWaiting` back (`|| HasRefusedDeparture`) | `NoPushbackRouteIsQuiet` (3, got 153); `SafetyNetDepartsAMissedOne` (departed before the net; Warning 0) |
| safety Warning off | `SafetyNetDepartsAMissedOne` ("said so, once" 1, got 0) |
| CancelSafetyNet leaves the departure half | `NetArmedAndCancelled` ("a detach cancels it", "a load cancels the net") |
| departure half never wanted | `NetArmedAndCancelled` ("a refused departure arms the net"); `SafetyNetDepartsAMissedOne` (never departs, 1200 frames) |
| every Idle / Serving vehicle "waiting" | `QuietBoard.TimedStepDue`, `.IdleWithAQueue` (300 of 300) |
| StartNext never sends a jobless vehicle home | `QuietBoard.ServingWithNoJob` (300 of 300) |

Item 5 finding: no part of `bVehicleWaiting` repeats every frame in production. Timed-and-due is finished by the next Step's due
loop; Idle-with-queue settles (the job goes back to the board and is refused); Serving-with-no-job goes home on the next Step.
The only per-frame repeaters are an open prerequisite (`FServiceJob::Prerequisites` is populated nowhere outside tests) and
StartNext's "refused at home with a route there" (a Found route DispatchAgent will not drive - under two points or zero long;
Start == Goal is SameNode, not Found), neither stageable. The tests stay as guards; no fix.

Deviations: the push watch re-asks DepartAgent's whole planning (AskDeparture) when its key moves, not only the stored route -
PlanAny's runway ranking can move the push to the other arm (RunwayFlipReplans). The net's clock entry is shared by both halves
(`SafetyNetHandle`; `IsQueueSafetyNetArmedForTest` renamed `IsSafetyNetArmedForTest`, `QueueSafetySeconds` -> `SafetyNetSeconds`).
All JobBoard dirtiers go through `DirtyJobBoard` (the deadline and the Unstick command included), so a safety run knows it was
uncovered.

Finding (pre-existing, not fixed): an in-play rebuild leaves no taxiing body claims in the table until the next Advance; a
DepartAgent in that window (the NetworkChanged retry) reads push ground free that a taxiing aircraft stands on. The watch's
re-derive reads the same table and may wake an aircraft once, early. Unverified whether the runtime can drain between a rebuild
and the next Advance in PIE.

UE_LOG / comment lines, `ab5e7a40` -> now (none fell): GroundTraffic.h 0/775 -> 0/824; GroundTraffic.cpp 34/616 -> 35/633;
AirsideTraffic.h 0/188 -> 0/189; AirsideTraffic.cpp 7/97 -> 7/97; OpsEventBus.h 2/159 -> 2/165; OpsEventBus.cpp 8/9 -> 8/9;
JobBoard.h 0/447 -> 0/464; JobBoard.cpp 23/197 -> 23/202; ServiceJob.h 0/134 -> 0/136; OpsRuntime.h 0/193 -> 0/206;
OpsRuntime.cpp 25/366 -> 26/380.

Unverified in PIE: all of it.

## Review ledger (fresh review, 2026-09-30: 0 Critical, 2 Important)

The rulings are the orchestrator's. Each fix got a test that failed first (red line quoted), unless it is marked pin; a pin went red
under its mutation. Restores used cp + touch and were rebuilt. Full suite after: `1589 test(s) run, 0 failed, 0 crashed` (+5).
Check-Architecture PASS with rule 36; rule-12 warnings 111.

| # | Finding | Ruling / fix | Test (red line; mutation) |
|---|---|---|---|
| I1 | The rebuild window: OnGraphRebuilt drops every guideline claim and re-makes only goals and stand holds, so a taxiing or pushing body is out of the table until the next AdvanceOnce. Paused, that never comes. The ops drain still runs, and the edit's NetworkChanged retry pushed an aircraft into its blocker. Also on main | CAUSE FIXED: at the end of OnGraphRebuilt, after the holds and before the freed diff, `Arbitrate(Network)` - the claim pass AdvanceOnce runs first (by rank, with its one re-pass), with no motion. One claim routine, not a copy. The fallback flag was not needed. PushWatch's "woken once, early" comment is rewritten | `Airside.Model.Traffic.PushGroundFreed.RebuildKeepsTaxiingBlocker` (red: "after the rebuild the table still holds the taxiing blocker" to be true; "so the rebuild fires nothing" 0, got 1); `AirportOps.Present.PushGroundFreed.PausedEditDepartsNothing` (red: "paused through the edit, the aircraft has not pushed back into the blocker" to be true); mutation "no Arbitrate in the rebuild" -> all three I1 tests red again |
| I1 (PR D's signals) | Does the rebuild's DiffFreedom(true) report a runway or stand FREED under a taxiing aircraft? STAND: YES, before the fix - an aircraft leaving a stand holds it by its body only (its goal is the runway), and the rebuild's diff reported the stand freed with the aircraft still on it. RUNWAY: NO - surface claims are not guideline claims, and ReleaseGuidelineClaims keeps them | Both covered by the same fix | `Airside.Model.Traffic.RunwayFreed.RebuildKeepsLeavingStandHeld` (red: "after the rebuild the stand is still held by the body on it"; "and the rebuild reported no stand freed" 0, got 1); `.RebuildKeepsCrossingHeld` (pin, green before the fix; mutation "rebuild Clear()s the table and runs no claim pass": "the strip is still held" to be true, "no runway freed" 0, got 1) |
| I2 | Raw JobBoard / ArrivalQueue dirtiers bypass the covered flag | Check-Architecture rule 36 `pass-dirtied-through-funnel`: `MarkDirty(TEXT("JobBoard"))` only in `UOpsRuntime::DirtyJobBoard`, and `MarkDirty(TEXT("ArrivalQueue"))` only in `UOpsRuntime::DirtyArrivalQueue` (the ArrivalQueue's funnel exists, from PR D); both are also allowed in `UOpsRuntime::ArmSafetyNet` (the net's entry). MarkDirtyNextDrain and MarkAllDirty are not matched. Comments are stripped. A missing funnel fails the rule. ENFORCED BY rule 36 added to both funnels' docs. **origin/main's #417 adds two raw JobBoard dirtiers (FleetChanged, FacilityUpgraded). This stack is not rebased, and rule 36 will fail them at rebase time: route both through DirtyJobBoard then** | control: raw `Bus.MarkDirty(TEXT("JobBoard"))` in the PushGroundFreed subscriber -> `FAIL ... OpsRuntime.cpp:246 marks the JobBoard pass dirty from UOpsRuntime::WireBus`; raw ArrivalQueue in OnStandsFreed -> `FAIL ... OpsRuntime.cpp:605 ... from UOpsRuntime::OnStandsFreed`; the same line's `// Bus.MarkDirty(TEXT("JobBoard"))` comment -> no failure. Restored -> PASS |
| M1 | "Strips read NOW" unpinned; relative ENFORCED BY name | `Airside.Model.Traffic.PushGroundFreed.RegisteredWithStripsHeldNow`: runway 1 is held after the last diff and released before the next, so the baseline never sees it. Registered from the baseline, the key reads unchanged and the stale J-B route is re-asked - a missed wake. ENFORCED BY now gives full names | pin; mutation "RunwaysHeld = HeldRunways" -> "runway 1 free again before any diff saw it held: the next diff replans and fires" 1, got 0 |
| M2 | `!bRebuilt` force unpinned | Called redundant in the comment: a rebuild follows a graph change, which has already moved GuidelineRevision. With the graph unchanged the stored route IS the route, so both paths answer alike and no test can see the difference | - |
| M3 | Watch not reset in ClearAgents | `PushWatch.Reset()` in ClearAgents. Agent ids CANNOT restart: `NextAgentId` only counts up within one UGroundTraffic. And ClearAgents' DiffNow already drops every entry as gone once a network has been diffed. So it is hygiene, and untested: the only observable case is a ClearAgents on a traffic model that has never diffed a network, which also has no entry to drop | - |
| M4 | (as reviewed) | Accepted as designed | - |

UE_LOG / comment lines, `6565c7d1` -> now (none fell): GroundTraffic.cpp 35/633 -> 35/640; GroundTrafficRebuild.cpp 21/519 -> 21/528;
OpsRuntime.h 0/206 -> 0/208; the rest unchanged.
