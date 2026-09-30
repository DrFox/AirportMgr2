# Ops batch 3 - PR D plan: derived runway and stand freedom; the arrival queue as a pass

Spec: `docs/superpowers/specs/2026-09-29-ops-batch3-design.md` §5, §7; bus spec
`2026-09-29-ops-event-bus-design.md` §2 (the TickQueue row, the safety pass). Branch `feature/ops-runway-freed`
(worktree `C:\repos\airportmgr2-ops-batch3`), stacked on PR C (#418). Baseline full suite: `1522 test(s) run, 0
failed, 0 crashed`.

Baseline UE_LOG / comment lines at `443bc853`: GroundTraffic.h 0/719, GroundTraffic.cpp 32/593,
GroundTrafficRebuild.cpp 19/513, ArrivalPlanner.h 0/197, ArrivalPlanner.cpp 1/299, AirsideTraffic.h 0/194,
AirsideTraffic.cpp 7/105, AirsideTestGraph.h 0/147, AirsideTestGraph.cpp 0/89, RunwayUseTest.cpp 0/57,
OpsEventBus.h 2/154, OpsEventBus.cpp 8/11, FlightBoard.h 0/385, FlightBoard.cpp 24/266, OpsRuntime.h 0/166,
OpsRuntime.cpp 24/345.

## Premises checked against the code (2026-09-30)

- **Crossings reach `IsChainHeld` through the claim pass - HOLDS.** `FClaimPass::BuildPending` (TrafficClaims.cpp
  ~515) raises every segment of `Agent.CrossingRunway`'s chain as an OCCUPIED surface claim while
  `CrossingPhase != None`; `UpdateCrossing` (~478) ends the crossing (`EndCrossing`), and `ApplyClaims` finishes with
  `Table.ReleaseExcept(Agent.Id, Wanted)` (~1360), so the next pass drops the surfaces. `IsChainHeld` is
  `Occupancy->IsAnyHeld(Network.RunwaySurfaces(Seed), 0, false)` over that same table. A diff at the end of
  `Advance` therefore sees the clear on the substep it happens. `OccupancyRevision` does NOT move for it
  (GroundTraffic.h's own comment) - the test pins that, since it is why the diff exists.
- **`IsChainHeld` is file-local** (ArrivalPlanner.cpp anonymous namespace). It moves to `ArrivalPlanner::IsChainHeld`
  (AIRSIDE_API) so the diff calls the SAME function `IsRunwayBusy` does, not a copy.
- **A flight going Inbound has no event.** `UFlightBoard::Enqueue` runs inside a `Clock.At` callback (and from
  `RearmSchedules` on a load). New `FFlightInboundEvent`, published by `Enqueue` (one site; the load's
  `DemoteRestoredMidFlight` is covered by the load's `MarkAllDirty`).
- **Attach does not MarkAllDirty today** (its catch-up is the first `NetworkChanged`). Added, as the spec says.
- **A paused `TickQueue` consumes its dirt.** It returns early while paused, and nothing re-dirties it on resume. So
  `FSpeedChangedEvent` (published by every speed/pause change, `ApplySpeed`) also dirties the pass. NOT in the spec's
  list - a deviation, reported.
- **FINDING - item 6's premise does not hold; STOPPED.** `DepartTheReady`'s per-frame retry (`Step` returning
  `bDepartureWaiting`) is never waiting on a runway: `DeparturePlanner::PlanAny` (DeparturePlanner.cpp ~185) ranks a
  held runway lower but never refuses for one, and `DepartAgent`'s only traffic refusal is `PushbackBlocked`
  (`IsPushGroundFree`, GroundTraffic.cpp ~1000) - the push ground, a taxiway, whose freeing raises no event (per-tick
  claim churn). Dirtying the JobBoard pass on `RunwayFreed` would retry nothing that changed, and removing the
  per-frame retry would strand a blocked pushback until the 30 s net, with a Warning each time. The retry, and the
  departure-ready half of the safety net, are left as they are, for the orchestrator's ruling.

**Spec deviation (ruled 2026-09-30):** §5's "JobBoard dirtied by RunwayFreed, replacing DepartTheReady's per-frame retry" is not done - the retry is kept and the safety net covers the arrival queue only (see the finding above and the ledger).

## Files

| File | Change |
|---|---|
| `Airside/Public/Model/ArrivalPlanner.h`, `Private/Model/ArrivalPlanner.cpp` | `IsChainHeld` public |
| `Airside/Public/Model/GroundTraffic.h` | `FOnRunwayFreed OnRunwayFreed` (`FRoadSegmentId Seed`), `FOnStandsFreed OnStandsFreed` (`const TArray<FGuidelineNodeId>&`), `int32 RunwayFreedCount() const`; private `DiffFreedom(const URoadNetwork&, bool bRebuilt)`, `RunwaySeeds`, `RunwaySeedsRevision`, `RunwaySeedsNetwork`, `HeldRunways`, `HeldStands`, `RunwayFreedTotal` |
| `Airside/Private/Model/GroundTraffic.cpp` | `Advance`: `DiffFreedom(*Network, false)` after the substeps; the function |
| `Airside/Private/Model/GroundTrafficRebuild.cpp` | `OnGraphRebuilt`: `DiffFreedom(Network, true)` at the end |
| `Airside/Public/Present/AirsideTraffic.h/.cpp` | relays `OnRunwayFreed` / `OnStandsFreed`, bound in `PostInitProperties` beside the two existing |
| `Airside/Public/Testing/AirsideTestGraph.h/.cpp` | `FTestTwoRunways` moved from RunwayUseTest.cpp's namespace (the ops two-runway test needs it) |
| `AirportOps/Public/Model/OpsEventBus.h`, `Private/Model/OpsEventBus.cpp` | `FRunwayFreedEvent`, `FStandsFreedEvent`, `FFlightInboundEvent` + `Describe`, into `FOpsEvent` |
| `AirportOps/Public/Model/FlightBoard.h/.cpp` | `FQueueTick TickQueue(...)` (was void); dead-stand re-reserve; `Enqueue` publishes `FFlightInboundEvent` |
| `AirportOps/Public/Present/OpsRuntime.h/.cpp` | Attach binds + publishes the two; `Detach` unbinds; `"ArrivalQueue"` pass + dirtiers in `WireBus`; one clearance a frame; safety net; `Tick` no longer calls `TickQueue`; Attach `MarkAllDirty`; `LoadFromSlot` cancels the net |
| `Tools/Check-Architecture.ps1` | rule 33 `queue-is-a-pass`: `TickQueue(` appears in OpsRuntime.cpp once, inside `RunArrivalQueue` |
| tests | new `AirsideTests/Private/RunwayFreedTest.cpp`; new `AirportOpsTests/Private/ArrivalQueuePassTest.cpp`; `ArrivalQueueTest.cpp` (dead stand); `OpsRuntimeTest.cpp` (`RuntimeTicksTheQueue` becomes the quiet test); `RunwayUseTest.cpp` uses the moved fixture |

## Interfaces

```cpp
// ArrivalPlanner.h
/** Is any segment of Seed's strip held ... ONE predicate for IsRunwayBusy, Plan and UGroundTraffic's freed diff. */
AIRSIDE_API bool IsChainHeld(const URoadNetwork& Network, FRoadSegmentId Seed, const FTrafficOccupancy* Occupancy);

// GroundTraffic.h
DECLARE_MULTICAST_DELEGATE_OneParam(FOnRunwayFreed, FRoadSegmentId /*Seed*/);
FOnRunwayFreed OnRunwayFreed;
DECLARE_MULTICAST_DELEGATE_OneParam(FOnStandsFreed, const TArray<FGuidelineNodeId>& /*PoseNodes*/);
FOnStandsFreed OnStandsFreed;
int32 RunwayFreedCount() const { return RunwayFreedTotal; }

// OpsEventBus.h
struct AIRPORTOPS_API FRunwayFreedEvent { FRoadSegmentId Seed; EventName "RunwayFreed"; Describe "runway seed %d" };
struct AIRPORTOPS_API FStandsFreedEvent { TArray<FGuidelineNodeId> PoseNodes; "StandsFreed"; "%d stand(s): ..." };
struct AIRPORTOPS_API FFlightInboundEvent { int32 FlightId; FName AirlineId; "FlightInbound" };

// FlightBoard.h
struct FQueueTick { int32 Waiting = 0; UFlight* Cleared = nullptr; bool bRetry = false; };
FQueueTick TickQueue(UGroundTraffic&, const URoadNetwork&, const USimClock&);
```

`DiffFreedom`: seeds = `SummariseRunways(Network)` ends' seeds, re-read when the network pointer or
`GetEditRevision()` changes or on a rebuild (chain membership is topology - FRunwayChainCache's own argument).
Held now = seeds with `IsChainHeld`. Every seed in `HeldRunways` not in held-now fires `OnRunwayFreed` and bumps the
counter. Stands: `IsStandCandidate()` entities whose `PoseNode` `IsStandHeld(Pose, 0)`; every old pose not held now
(or gone) is freed, one `OnStandsFreed` with the list. A rebuild runs the same diff at once - a vanished runway or
stand is not held now, so it is freed; a pose re-numbered by the rebuild reports freed spuriously, which costs one
pass. Cost: 2 runway checks + ~30 stand node lookups per Advance on the busiest test field (2026-09-30).

Runtime pass:

```cpp
Bus.RegisterPass(TEXT("ArrivalQueue"), [this]() { RunArrivalQueue(); });
// dirtiers (Sim): RunwayFreed, StandsFreed, OfferAccepted, FlightInbound, NetworkChanged, AirportStatusChanged,
// SpeedChanged, AgentPhase To==Arriving - each through DirtyArrivalQueue() (sets bQueueCovered)
void UOpsRuntime::RunArrivalQueue()
{
    if (QueueClearedFrame == DrainFrame) { Bus.MarkDirtyNextDrain(TEXT("ArrivalQueue")); return; } // one a frame
    const bool bSafetyOnly = bQueueSafetyDue && !bQueueCovered;
    bQueueSafetyDue = bQueueCovered = false;
    const FQueueTick Result = FlightBoard->TickQueue(*Model, *Target->Network, *Clock);
    if (Result.Cleared) { QueueClearedFrame = DrainFrame; if (bSafetyOnly) UE_LOG(LogOpsBus, Warning, "safety pass dispatched flight %d (%s) - no event covered it"); }
    if (Result.bRetry) { Bus.MarkDirtyNextDrain(TEXT("ArrivalQueue")); }
    ArmQueueSafetyNet(Result.Waiting - (Result.Cleared ? 1 : 0) > 0);
}
// net: Clock->Every(QueueSafetySeconds = 30, [this]{ bQueueSafetyDue = true; Bus.MarkDirty(TEXT("ArrivalQueue")); })
```

## Tasks (TDD: test, build, run red, implement, green, commit)

1. **Plan** - this file. Commit.
2. **Airside diff.** Declare the delegates/counter (no broadcast), new `RunwayFreedTest.cpp` (two builds). Tests
   `Airside.Model.Traffic.RunwayFreed.TakeOff` (DepartureReleasesWhenAirborne's field: none while rolling, one on
   release, counter 1), `.CrossingClears` (FCrossingFixture: none while crossing, one on release; OccupancyRevision
   unchanged across that tick - the no-revision case), `.DespawnOnRunway` (RetireAgent mid-crossing, next Advance),
   `.DeletedRunway` (runway segment removed + rebuild mid-crossing: fires in OnGraphRebuilt), `.StandsDiff`
   (FTestAirport 2 stands: hold -> nothing; release -> [pose]; parked agent retired -> [its pose]; deleted held
   stand + rebuild -> freed). Red: counts 0. Implement; green.
3. **Fixture move.** `FTestTwoRunways` into AirsideTestGraph; RunwayUseTest aliases it; `Airside.Model.RunwayUse`
   stays green (pure refactor).
4. **Events + bridge.** Three events; `Attach` binds `UAirsideTraffic::OnRunwayFreed/OnStandsFreed` (relayed from
   the model). Test `AirportOps.Present.Bus.RunwayAndStandFreedAreBridged` (broadcast on the model -> subscriber
   sees event after Tick; Detach -> nothing). `EveryEventHasASubscriber` goes red until task 5 subscribes.
5. **The pass.** `ArrivalQueuePassTest.cpp` (two builds):
   `AirportOps.Present.ArrivalQueue.CrossingClearDispatchesNextFrame` (world actor + FTestAirport + a crossing
   laid across the runway beyond the exit; a flight queued while a taxiing aircraft crosses; dispatched within one
   frame of the chain's release; unbuffered LogOpsBus spy sees NO safety Warning), `.SafetyNetCatchesAMissedEvent`
   (a claim released by hand, no Advance: the net dispatches after 30 game s and logs exactly one Warning),
   `.QuietQueueRunsNothing` (0 TickQueue calls over 300 frames), `.EachEventDirtiesIt` (each dirtier by name, +1
   call per event, + MarkAllDirty on load), `.SecondRunwayNextFrame` (FTestTwoRunways: two flights, dispatched on
   consecutive frames), `AirportOps.Model.ArrivalQueue.DeadStandReservesWhenOneFrees`,
   `AirportOps.Model.FlightBoard.EnqueuePublishesInbound`. `RuntimeTicksTheQueue` rewritten (sequencer wired; 2
   quiet ticks run 0). Implement; green.
6. **Rule 33** `queue-is-a-pass`; control (a second TickQueue call in Tick fails it).
7. **Mutations**, each red then `cp` + `touch` restore: no runway broadcast (CrossingClear's no-Warning test red),
   no stands broadcast, each dirtier, no Attach bind, no one-a-frame deferral (SecondRunwayNextFrame), no dead-stand
   branch.
8. **Full suite**, counts, report.

## Execution notes (2026-09-30)

Full suite: `1534 test(s) run, 0 failed, 0 crashed` (+12: 5 Airside, 8 ops, minus `AirportOps.Present.RuntimeTicksTheQueue`,
which pinned the per-frame call and is replaced by `QuietQueueRunsNothing`). Check-Architecture PASS with rule 33, rule-12
warnings 111 (unchanged).

Red lines before implementation: all five `Airside.Model.Traffic.RunwayFreed.*` ("the tick the crossing clears fires once"
expected 1, got 0, etc.); `FreedIsBridged` ("a freed runway is published onto the bus" 3, got 2); `EveryEventHasASubscriber`
(RunwayFreed/StandsFreed/FlightInbound). The pass tests were written with the pass; their reds are the mutations below.

Mutations (each red, restored with cp + touch, rebuilt green):

| Mutation | Red |
|---|---|
| runway diff broadcast never reached | RunwayFreed.TakeOff/CrossingClears/DespawnOnRunway/DeletedRunway; `CrossingClearDispatchesNextFrame` ("released 304, cleared 898"; "no safety Warning" expected 0, got 1) |
| stands diff broadcast never reached | RunwayFreed.StandsDiff |
| all eight dirtiers emptied | `EachEventDirtiesIt`, one named line each; `CrossingClearDispatchesNextFrame`; `SecondRunwayNextFrame` |
| Attach bridge removed | `FreedIsBridged` |
| one-a-frame deferral removed | `SecondRunwayNextFrame` ("one frame after" expected 1, got 0) |
| dead-stand branch removed | `DeadStandReservesWhenOneFrees` |
| Enqueue publish removed | `EnqueuePublishesInbound` |
| safety Warning removed | `SafetyNetCatchesAMissedEvent` ("said so, once" expected 1, got 0) |
| pass re-dirties itself every frame (the old poll) | `QuietQueueRunsNothing` (expected 5, got 305), `EachEventDirtiesIt` ("nothing: no run") |
| TickQueue call back in Tick | rule 33 `queue-is-a-pass` FAIL |

Not pinned: the load's `MarkAllDirty` for the queue (the load's own NetworkChanged also runs it, so no test can tell them
apart); the safety net's cancel on load/detach.

Findings for the orchestrator:

- **Item 6 stopped** (see Premises): `DepartTheReady` never waits on a runway; its one self-clearing refusal is
  `PushbackBlocked`, which no event covers. Per-frame retry kept; no RunwayFreed -> JobBoard dirtier; the safety net covers
  the queue only.
- **An in-play guideline rebuild drops every accepted flight's stand hold.** `FTrafficOccupancy::ReleaseGuidelineClaims`
  releases Node claims of any holder, flight holds included, and `UFlightBoard::OnGraphRebuilt` (Reapply) runs only from
  `LoadFromSlot`. The stands diff now reports those holds freed, truthfully. Pre-existing; not fixed here.
- **SpeedChanged dirties the queue** (not in the spec's list): a pass run while paused consumes its dirt.

## Review ledger (fresh review of PR D, 2026-09-30: 0 Critical, 2 Important)

Rulings are the orchestrator's. Each fix got a test that failed first (red line quoted) unless marked pin; pins went red under
their mutation. Full suite after: see the end of this section.

| # | Finding | Ruling / fix | Test (red line) |
|---|---|---|---|
| Item 6 | DepartTheReady's per-frame retry is not a runway wait | RULING: accepted as found. PlanAny never refuses for a held runway; the one transient refusal is PushbackBlocked, which no event covers. Retry kept, no RunwayFreed -> JobBoard dirtier, safety net covers the queue only. Spec deviation noted above | - |
| I1 | An in-play guideline rebuild dropped every accepted flight's stand hold (`ReleaseGuidelineClaims` takes every Node claim; Reapply ran only on load) - a later accept double-booked the stand. Pre-existing on main | `UGroundTraffic::OnGraphRebuilt` snapshots non-agent (negative-holder) Node claims on stand poses BY ENTITY before the release and re-holds each on that entity's pose after the agents' goal claims; a hold whose stand is gone is dropped with a Log line (the flight's dead Stand is HeldStandLost's evidence); a refused re-hold Warns | `AirportOps.Present.RuntimeEdit.KeepsAcceptedStandHold` ("after the edit the stand is still held, by the same flight" expected -1, got 0; "a second offer is refused" expected null); `Airside.Model.Traffic.RebuildKeepsStandHolds` ("stand 0 is still held, by its own holder" -7, got 0); `AirportOps.Model.StandAllocator.SurvivesAGraphRebuild` rewritten - it asserted the drop - ("the rebuild itself keeps the hold" -1, got 0) |
| I1 knock-on | ClearanceFor caches NoFreeStand on OccupancyRevision; a stand freed by claim churn moves no revision | Checked, no fix needed: the only flight that can be refused NoFreeStand is a stand-less (or dead-stand) one, and TickQueue re-reserves before it asks - HoldStand bumps the revision, invalidating the cache. Pinned | `AirportOps.Model.ArrivalQueue.StandFreedByChurnIsNotStale` (pin, green; mutation "HoldStand without its bump" -> "clears the flight - not the cached NoFreeStand" expected 1, got 0) |
| I2 | StandsDiff's deleted-stand block measured nothing (every hold read freed on a rebuild) | Both stands held, stand 0 deleted: exactly stand 0 reported, stand 1 still held by -9 | red pre-I1 ("naming the deleted stand 0 alone", "stand 1 is still held" -9, got 0); mutation "no RemoveEntity" -> "the rebuild reports one broadcast" expected 3, got 2 |
| M1 | False safety Warnings | (a) bRetry re-dirty keeps `bQueueCovered`; (b) both flags reset whenever the net is disarmed; (c) `DiffNow()` (DiffFreedom on the last diffed network, weak) at the end of RetireAgent, ReleaseHold, ClearAgents - the event is real, not suppressed | (a) `AirportOps.Present.ArrivalQueue.RetryStaysCovered` ("no safety Warning" expected 0, got 1); (c) `RetireFreesWithoutAdvance` (0, got 1), `RunwayFreed.DespawnOnRunway` ("frees the strip at once" 1, got 0), `RunwayFreed.ClearAgents` (1, got 0), `StandsDiff` ("the release itself fires" 1, got 0). (b) NOT pinned: a stale flag needs the net to fire and be disarmed between a clock callback and the drain that follows it, which no public path does |
| M2 | Stand baseline keyed by pose | `TMap<FEntityInstanceId, FGuidelineNodeId>`; the pose kept for the event payload | covered by StandsDiff/RebuildKeepsStandHolds |
| M3 | Rule 33 too narrow | Any `TickQueue\s*\(` bar the definition, `&X::TickQueue`, comments stripped, function re-read at every column-0 definition (free functions too) | control: a bare `TickQueue(...)` in `UFlightBoard::TickOffers` and `&UFlightBoard::TickQueue` in a free function after RunArrivalQueue -> 2 FAILs; a comment naming `TickQueue(` -> none |
| M4 | Catch-ups and net cancels unpinned | `FOpsEventBus::IsDirtyForTest` | `AirportOps.Present.ArrivalQueue.AttachAndLoadMarkItDirty` (mutation no Attach MarkAllDirty -> "an attach marks the queue pass dirty"; no load MarkAllDirty -> "a load marks..."); `NetCancelledOnLoadAndDetach` (mutation -> "a load cancels the net", "a detach cancels it") |
| M5 | Two ENFORCED BY lines overclaimed | IsChainHeld: reworded, no ENFORCED BY (no test can see a copy that agrees); "Airside never learns ops": ENFORCED BY Check-Architecture rule 1b (cross-plugin) | - |
| M6 | FRig lost the PlaceNode WHY | Restored ("a network, which a fresh actor lacks until its first edit...") | - |
| M7 | Cost comment omitted O(S^2) Contains | Gone with M2: TSet/TMap baselines; comment counts the lookups | - |

Renamed from the ruling's `AirportOps.Present.Runtime.EditKeepsAcceptedStandHold`: a dotted child under the existing bare
`AirportOps.Present.Runtime` test made the automation tree drop that test (run count 1541 where 1542 was due).

Full suite after the review fixes: `1542 test(s) run, 0 failed, 0 crashed` (+8). Check-Architecture PASS, rule-12 111.
