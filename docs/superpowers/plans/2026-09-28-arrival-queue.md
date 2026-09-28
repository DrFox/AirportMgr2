# Arrival Queue Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A busy runway queues an accepted flight instead of refusing or losing it, and the player can see the queue.

**Architecture:** `ArrivalPlanner::Plan` gains `ERunwayBusy::Queue` so acceptance ignores runway occupancy; the board puts a due flight into `Inbound` (holding, stand kept); a world-free `UArrivalSequencer` picks the first queued flight whose runway is free each frame; an ARRIVALS section under the offers shows every accepted flight until airborne.

**Tech Stack:** UE 5.8.2 C++, Airside + AirportOps plugins, AirportMgr UMG, UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-28-arrival-queue-design.md`

## Global Constraints

- Worktree `C:\repos\airportmgr2-offers-and-demand`, branch `feature/arrival-queue` (stacked on `feature/offers-and-demand`, PR #378). Builds add `-NoHotReloadFromIDE`; tests pass `-Project`.
- Model/ world-free; Airside may not include AirportOps.
- A mode is an enum (`ERunwayBusy`), not a bool.
- One list: the queue is DERIVED from flights in `Inbound`, never stored.
- Every new seam gets a test that fails if unwired; new test .cpp needs two builds.
- Commit messages concise, no Co-Authored-By.

## Review Focus

- **A dispatch refused for a non-runway reason** (graph mid-edit): flight must stay queued with a stand, retried - pinned in Task 2 (`FailedDispatchStaysQueued`).
- **Two runways, head waiting on the busy one**: the other runway's flight clears - Task 2 (`FreeRunwayJumpsTheQueue`).
- **Save with a flight holding**: comes back queued and clears - Task 2 (`HoldingSurvivesSave`).
- **Paused game**: nothing clears - Task 2 (`PausedClearsNothing`).
- **Runway busy AND no stand**: the row must say no stand, not "fine" - Task 1 (`QueueStillChecksStands`).

---

### Task 1: `ERunwayBusy` and `IsRunwayBusy` in ArrivalPlanner

**Files:** `Plugins/Airside/Source/Airside/Public/Model/ArrivalPlanner.h`, `Private/Model/ArrivalPlanner.cpp`; test `Plugins/Airside/Source/AirsideTests/Private/ArrivalQueuePlanTest.cpp` (new).

**Produces:**
```cpp
enum class ERunwayBusy : uint8 { Refuse, Queue };   // UENUM not needed: never reflected
namespace ArrivalPlanner {
  AIRSIDE_API FArrivalPlan Plan(const URoadNetwork&, const FVector2D& Near, const FAirframe&,
      const FTrafficOccupancy* Occupancy = nullptr, ERunwayBusy RunwayBusy = ERunwayBusy::Refuse);
  // True when the runway nearest Near is held by anyone. False with no runway or no occupancy.
  AIRSIDE_API bool IsRunwayBusy(const URoadNetwork&, const FVector2D& Near, const FTrafficOccupancy* Occupancy);
}
```
- [ ] Tests: `Airside.Model.ArrivalQueue.QueueIgnoresABusyRunway` (hold a runway surface under agent 99: Refuse -> RunwayOccupied, Queue -> None); `.QueueStillChecksStands` (same, plus the one stand held: Queue -> NoFreeStand); `.IsRunwayBusyAgreesWithPlan`.
- [ ] Build twice, run `-Filter Airside.Model.ArrivalQueue`: compile RED; implement (step 1b calls IsRunwayBusy, skipped under Queue); GREEN; commit `feat(arrival): a busy runway can be queued for, not refused`.

### Task 2: The queue on the board

**Files:** `Flight.h` (`HoldingSince`), new `Public/Model/ArrivalSequencer.h` + `Private/Model/ArrivalSequencer.cpp`, `FlightBoard.h/.cpp`, `OpsRuntime.cpp`; test `Plugins/AirportOps/Source/AirportOpsTests/Private/ArrivalQueueTest.cpp` (new), updates to `FlightBoardTest.cpp` / `FlightSaveTest.cpp` where they asserted dispatch-at-ETA.

**Produces:**
```cpp
UCLASS() class UArrivalSequencer : public UObject {   // world-free, no state today
  // First flight in queue order whose runway is free, or null.
  UFlight* Next(TArrayView<UFlight* const> Queue, const URoadNetwork&, const FTrafficOccupancy&) const; };
// UFlightBoard
UPROPERTY() TObjectPtr<UArrivalSequencer> Sequencer;          // CreateDefaultSubobject in runtime
TArray<UFlight*> Queue() const;                               // Inbound, by HoldingSince then Id
void TickQueue(UGroundTraffic&, const URoadNetwork&, const USimClock&);  // clears <= 1 per call
// UFlight: UPROPERTY() double HoldingSince = 0.0;
```
- Schedule callback and RearmSchedules' overdue branch call a new private `Enqueue(Flight, Now)` instead of `DispatchNow`.
- `DispatchNow` returns bool; on dispatcher refusal re-Reserve the stand and restore `Inbound`.
- WhyNotAcceptable passes `ERunwayBusy::Queue`.
- `OnGraphRebuilt` re-applies holds for `Accepted` AND `Inbound`.
- `TickQueue` no-op when paused; board with null Sequencer falls back to strict head-of-queue (tests that don't care).
- [ ] Tests (`AirportOps.Model.ArrivalQueue.*`): `AcceptWhileRunwayBusy`, `DueWhileBusyWaitsThenLands` (regression for lost flight), `ClearsInJoinOrderOnePerFrame`, `FreeRunwayJumpsTheQueue`, `FailedDispatchStaysQueued`, `PausedClearsNothing`, `HoldingSurvivesSave`, `OverdueOnLoadJoinsQueue`.
- [ ] RED (compile), implement, GREEN, full suite, mutation: make Schedule call DispatchNow again -> `DueWhileBusyWaitsThenLands` red. Commit `feat(arrival): due flights hold for the runway instead of being lost`.

### Task 3: ARRIVALS section

**Files:** `Source/AirportMgr/ArrivalViewModels.h/.cpp` (new), `OfferInboxWidget.h/.cpp`, test `Source/AirportMgr/ArrivalViewModelsTest.cpp` (new).

**Produces:** `UArrivalRowViewModel` (GetTitle "#1 CU 204  Saab 340B", GetStatus, GetDetail, IsLate), `UArrivalsViewModel::Refresh(Board, Clock)` + `GetRows()`; static `UArrivalRowViewModel::DescribeStatus(const UFlight&, double Now)` and `DescribeDetail(const UFlight&, double Now)`.
- [ ] Tests (`AirportMgr.Arrivals.*`): `OrderHoldingThenInboundThenGround`, `StatusText` (each phase), `LateText`, widget `SectionShowsCount`.
- [ ] RED, implement, GREEN, full suite, commit `feat(ui): arrivals section under the offers`.

### Task 4: PIE check, review, PR

- [ ] PIE on the user's M_Test copy: accept two offers back to back; expect `holding: #2 in queue` then `cleared to land after`; screenshot the ARRIVALS section.
- [ ] Opus whole-branch review; fix Critical/Important with TDD; PR against `feature/offers-and-demand`.
