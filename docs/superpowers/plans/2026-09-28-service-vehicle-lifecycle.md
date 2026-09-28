# Service Vehicle Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Execution note (2026-09-28):** the user asked for all stages unattended overnight, so this
> plan is executed natively by its author with one whole-branch review at the end. Headers and
> test assertions are given in full; function BODIES are specified by behaviour and by the
> existing `FuelService.cpp` code they are moved from (cited by function), because most of that
> code moves rather than being written anew and re-typing 1200 lines into a plan would be a
> second copy to drift.

**Goal:** Replace `UFuelService` with a vehicle-owned lifecycle, a per-role policy and a bidding
`UJobBoard`, in three stacked PRs.

**Architecture:** `FServiceVehicle` owns state, cargo and queue; `IServiceRolePolicy` supplies
the per-role rules as pure functions of (cargo, type, quantity); `ServiceBid::Finish` is a pure
simulation of a vehicle's queue used for every bid; `UJobBoard` owns vehicles, jobs and
turnarounds, routes Airside phase events to vehicles, and drives them through Airside's
existing dispatch/redirect/reroute/retire.

**Tech Stack:** UE 5.8 C++, AirportOps plugin (`Model/` world-free), UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-28-service-vehicle-lifecycle-design.md`

## Global Constraints

- AirportOps `Model/` may not include `Entities/` or `Content/` (Check-Architecture rule 1).
- One log category: `LogAirportOps`. Every existing `UE_LOG` in `FuelService.cpp` survives (count before/after); wording may change `truck`->`vehicle` except strings a test expects (`does not fit the road home`).
- WHY comments travel with their code; comment-line count in touched files must not fall.
- `ENFORCED BY:` names cited in comments must still exist after renames.
- Build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-service-vehicles\AirportMgr.uproject" -WaitMutex` (add `-NoHotReloadFromIDE` only if an editor is open on ANOTHER checkout).
- Test: `./Tools/Run-AirsideTests.ps1 -Project C:\repos\airportmgr2-service-vehicles\AirportMgr.uproject` - read the `N test(s) run, N failed, N crashed` line.
- A new test .cpp needs two builds (memory note); check the run count rises.
- No `Co-Authored-By` trailer on commits (user CLAUDE.md). PR bodies end with the Claude Code line.
- Unity build: anonymous-namespace helpers in the new .cpp files get distinct named namespaces.

## Review Focus

1. A job whose aircraft leaves while its vehicle is Underway/Serving: the vehicle must turn on the road (not teleport) and go on to its next job or home - pinned by the ported `AircraftLeaves` and `*RecalledMidRouteGetsHome` tests.
2. A vehicle chained stand-to-stand leaves a PARKED start whose route opens with a reverse leg: the tow's seeded reverse must be used for a job leg as for the home leg - new `Fuel.TowChainsWithoutTheDepot`-shaped test on the far-edge fixture (bowser case in `ChainsStandToStandWithoutTheDepot`).
3. A depot deleted with vehicles out: vehicles withdrawn, their jobs re-opened, no agent left stranded - `DepotDeletedWithdrawsItsVehicles`.
4. Dispatch refused (DispatchAgent returns 0): the vehicle stays Idle with its queue and retries next tick without a log line per tick - covered by the busy-wait counting test (no bids while idle).
5. Zero-litre aircraft and zero-capacity specs never produce a zero-trip loop - ported `NoLitresNoTruck`, `ZeroCapacitySpecStillFinishes`.

---

## File Structure

| File | Responsibility |
|---|---|
| Create `Plugins/AirportOps/Source/AirportOps/Public/Model/ServiceJob.h` | `EServiceJobState`, `EServiceRefusal`, `FServiceJob`, `FTurnaround` |
| Create `.../Public/Model/ServiceVehicle.h` | `EServiceVehicleState`, `FServiceVehicleType`, `FServiceVehicle` |
| Create `.../Public/Model/ServiceRolePolicy.h` + `Private/Model/ServiceRolePolicy.cpp` | `EServiceStep`, `IServiceRolePolicy`, `FFuelRolePolicy` |
| Create `.../Public/Model/ServiceBid.h` + `Private/Model/ServiceBid.cpp` | pure queue simulation `ServiceBid::Finish` |
| Rename `FuelService.h/.cpp` -> `JobBoard.h/.cpp` | `UJobBoard` (lifecycle, events, tick, departures, describe) |
| Create `Private/Model/JobBoardDrive.cpp` | `UJobBoard::DriveVehicleTo` (moved `SendTruckHome`), `MayDriveUngated` |
| Create `Private/Model/JobBoardBid.cpp` | eligibility (moved `ChooseDepot` classification), bid assembly, `CouldServe`, stage-2 re-bid |
| Modify `OpsRuntime.h/.cpp`, `FlightBoard.h/.cpp`, `OpsSave.cpp`, `Source/AirportMgr/InspectorWidget.cpp`, `OfferViewModelsTest.cpp` | rename `UFuelService`->`UJobBoard`, `GetFuelService`->`GetJobBoard` |
| Create `AirportOpsTests/Private/ServiceBidTest.cpp` | pure bid tests (user's worked examples, transfer policy) |
| Modify `FuelServiceTest.cpp`, `FuelServiceWiredTest.cpp`, `FuelDescribeTest.cpp`, `OpsRuntimeTest.cpp`, `OpsSaveTest.cpp` | port to jobs/vehicles |

---

## Stage 1 (PR 1): vehicles, jobs, board, fuel policy, queued bids

### Task 1: Types, policy and the pure bid

**Files:** create `ServiceJob.h`, `ServiceVehicle.h`, `ServiceRolePolicy.h/.cpp`, `ServiceBid.h/.cpp`, `ServiceBidTest.cpp`.

**Interfaces (produces):**

```cpp
// ServiceJob.h
UENUM() enum class EServiceJobState : uint8 { Open, Queued, Underway, Serving, Done, Unserviceable };
UENUM() enum class EServiceRefusal : uint8 { None, NoDepot, NoRoad, StandUnjoined, NoRoute, TooNarrow, NoPump, VehicleTooLarge };
USTRUCT() struct AIRPORTOPS_API FServiceJob {
  int32 Id = 0; int32 FlightId = 0; int32 AircraftId = 0; EServiceRole Role = EServiceRole::Fuel;
  FEntityInstanceId Stand; EServiceJobState State = EServiceJobState::Open;
  double QuantityOwed = 0, QuantityDelivered = 0; int32 Trips = 0;
  int32 VehicleId = 0;           // Queued/Underway/Serving: whose; else 0
  double PromisedFinish = 0;     // the winning bid, game seconds
  double TankLitres = 0;         // capacity of the vehicle that took the latest trip (card's trip count)
  double TripQuantity = 0, TripStartedAt = 0, TripEndsAt = 0;   // Serving only
  EServiceRefusal Why = EServiceRefusal::None; uint32 RefusedAtRevision = 0;
  TArray<int32> Prerequisites;   // stage-less: stored, honoured by the board (none produced yet)
};
USTRUCT() struct AIRPORTOPS_API FTurnaround {
  int32 AircraftId = 0; int32 FlightId = 0; FEntityInstanceId Stand;
  double TurnaroundEndsAt = 0; EDepartureRefusal LastDepartureRefusal = EDepartureRefusal::None;
  TArray<int32> JobIds;          // the per-flight index
};
// ServiceVehicle.h
UENUM() enum class EServiceVehicleState : uint8 { Idle, ToJob, Serving, ToFacility, AtFacility };
USTRUCT() struct AIRPORTOPS_API FServiceVehicleType { FName TypeCode; EServiceRole Role; FVehicle Vehicle; double Capacity = 1; double RatePerMinute = 1; };
USTRUCT() struct AIRPORTOPS_API FServiceVehicle {
  int32 Id = 0; FName TypeCode; EServiceRole Role = EServiceRole::Fuel; FEntityInstanceId Home;
  EServiceVehicleState State = EServiceVehicleState::Idle; int32 AgentId = 0; double Cargo = 0;
  int32 CurrentJob = 0; TArray<int32> Queue; double StepStartedAt = 0, StepEndsAt = 0;
};
// ServiceRolePolicy.h
enum class EServiceStep : uint8 { Direct, ViaFacility };
class AIRPORTOPS_API IServiceRolePolicy {
public:
  virtual ~IServiceRolePolicy() = default;
  virtual EServiceRole Role() const = 0;
  virtual bool NeedsPumpAtHome() const = 0;
  virtual EServiceStep NextStep(double Cargo, const FServiceVehicleType&, double Owed) const = 0;
  virtual double TripQuantity(double Cargo, const FServiceVehicleType&, double Owed) const = 0;
  virtual double ServeSeconds(const FServiceVehicleType&, double Quantity) const = 0;
  virtual double CargoAfterServe(double Cargo, double Quantity) const = 0;
  virtual double FacilitySeconds(double Cargo, const FServiceVehicleType&, int32 Pumps) const = 0;
  virtual double CargoAfterFacility(double Cargo, const FServiceVehicleType&) const = 0;
};
class AIRPORTOPS_API FFuelRolePolicy final : public IServiceRolePolicy { public: double RefillLitresPerMinutePerPump = 500; /* overrides */ };
// ServiceBid.h
namespace ServiceBid {
  enum class EPlace : uint8 { Facility, Job };
  struct FTrip { int32 Node = 0; double Owed = 0; };            // node: an opaque id the drive callback understands
  struct FInput {
    double FreeAt = 0; double CargoWhenFree = 0; int32 NodeWhenFree = 0;   // where/when the current step leaves it
    int32 FacilityNode = 0; int32 Pumps = 1;
    TArray<FTrip> Queued;          // one trip each, in order
    FTrip Appended;                // served to completion alone
    const FServiceVehicleType* Type = nullptr; const IServiceRolePolicy* Policy = nullptr;
    TFunction<double(int32 From, int32 To)> DriveSeconds;   // game seconds; <0 = unreachable
  };
  struct FResult { double Finish = 0; int32 FacilityVisits = 0; int32 Trips = 0; bool bReachable = true; };
  AIRPORTOPS_API FResult Finish(const FInput& In, int32 MaxTrips = 64);
}
```

- [ ] **Step 1: Write `ServiceBidTest.cpp`** with these tests (drive = 180 s between any two different nodes, 0 same node; tow = UTILITY 1000 L @ 75; bowser = FUEL 10000 L @ 200; refill 500 L/min/pump, 1 pump; facility node 1, stands 10, 11, 12):
  - `AirportOps.Service.Bid.BusyBowserBeatsIdleTow`: 4000 L appended. Tow: FreeAt 0, cargo 1000 at facility. Bowser: FreeAt 300, cargo 9000 at node 11. Assert bowser.Finish < tow.Finish; tow.FacilityVisits == 3; tow.Trips == 4; bowser.FacilityVisits == 0.
  - `AirportOps.Service.Bid.QueueDepthDecides`: 600 L appended at node 12. Bowser FreeAt 1200 at node 11 cargo 9000. Tow A cargo 1000 at facility, one 300 L queued at node 10. Tow B same, two 300 L queued at 10, 11. Assert towA < bowser < towB; towA.FacilityVisits == 0; towB.FacilityVisits == 1 ("because the tank runs out").
  - `AirportOps.Service.Bid.FullTankIsDirect`: cargo 1000 == capacity, 4000 owed -> first step Direct (no visit before first trip): tow from facility with cargo 1000 has Trips 4, visits 3, not 4.
  - `AirportOps.Service.Bid.TransferPolicyAlwaysVisits`: a test-local `FTransferPolicy` (NextStep always ViaFacility, TripQuantity min(Capacity, Owed), FacilitySeconds 60, CargoAfterFacility Capacity, CargoAfterServe 0) with 250 owed, capacity 100: Trips 3, FacilityVisits 3.
  - `AirportOps.Service.Bid.UnreachableIsFlagged`: DriveSeconds returns -1 -> bReachable false.
- [ ] **Step 2: Build twice, run `-Filter AirportOps.Service`**; expect compile failure then fail.
- [ ] **Step 3: Implement.** `FFuelRolePolicy`: Capacity floored at 1, rate floored at 1. `NextStep`: Direct if `Cargo >= Owed - 0.5` or `Cargo >= Cap - 0.5`, else ViaFacility. `TripQuantity = min(max(Cargo,0), Owed)`. `ServeSeconds = Q / Rate * 60`. `FacilitySeconds = max(Cap - Cargo, 0) / (max(Pumps,1) * max(Refill,1)) * 60`. `ServiceBid::Finish`: T = FreeAt, cargo, node; for each queued trip then the appended job (loop while owed > 0.5 up to MaxTrips): if `NextStep == ViaFacility` and node != facility: T += drive(node, fac) + FacilitySeconds; cargo = after; node = fac; ++visits (at facility with ViaFacility: visit without drive). Then T += drive(node, trip node) + ServeSeconds(q); cargo after; owed -= q; ++Trips. Any drive < 0 -> bReachable false, return. A trip with q < 0.5 after a facility visit (zero capacity) breaks the loop.
- [ ] **Step 4: Run; expect 5 pass.** Commit `feat(service): vehicle/job types, fuel role policy, pure queue bid`.

### Task 2: `UJobBoard` replaces `UFuelService`

**Files:** `git mv` FuelService.h/.cpp -> JobBoard.h/.cpp; create JobBoardDrive.cpp, JobBoardBid.cpp; modify every file listed in File Structure.

**Interfaces (produces), `UJobBoard : UObject, IOpsPersistent`:** keeps every public member of `UFuelService` that callers use - `VehicleSpecs`, `FallbackSpec`, `RefillLitresPerMinutePerPump`, `SpecFor`, `LitresOwedFor`, `DefaultLitres`, `HasWorkingPump`, `PumpsAt`, `LetterCount`, `ResolveVehicles`, `VehiclesFor`, `DesignVehicleOf`, `DesignVehicleFor`, `VehicleFor`, `LetterOfStand`, `CouldServe`, `OnAgentPhase`, `Ledger`, `Pricing`, `PostServiceFee`, `Tick`, `DescribeAgent`, `MayDriveUngated`, `SaveBlobName` ("Fuel", unchanged), `OnBeforeRestore` - plus:

```cpp
const TArray<FServiceJob>& GetJobs() const;
const TArray<FServiceVehicle>& GetVehicles() const;
const TArray<FTurnaround>& GetTurnarounds() const;
const FServiceJob* JobForAircraft(int32 AircraftId, EServiceRole Role = EServiceRole::Fuel) const;
const FServiceVehicle* FindVehicle(int32 VehicleId) const;
const FServiceVehicle* VehicleForAgent(int32 AgentId) const;
int32 AgentForJob(const FServiceJob&) const;          // the assigned vehicle's agent, or 0
int32 TrucksGoingHomeForTest() const;                 // vehicles in ToFacility
int32 TrucksOutForTest(FEntityInstanceId Depot) const;// that depot's vehicles not Idle
int32 GetBidCallCountForTest() const; void ResetBidCallCountForTest();
FFuelDemand-free: FServiceJob& AddJobForTest(int32 AircraftId, EServiceJobState, EServiceRefusal, uint32 RefusedAtRevision);
ServiceBid::FResult BidForTest(const URoadNetwork&, const USimClock&, int32 VehicleId, int32 JobId) const;
static constexpr double RebidMarginSeconds = 120.0;   // stage 2
```

Behaviour (moved code cited):
- `SyncFleet(Network)` at the top of Tick and OnAgentPhase: placeholder fleet per spec §3.4; types from distinct `VehiclesByLetter` TypeCodes with `SpecFor` figures; new vehicles Idle, cargo = capacity, logged `Fleet: depot %d gains vehicle %d %s`. Vehicles whose home is not alive: agent retired, current/queued jobs back to Open, removed, logged; `++FleetRevision`.
- `OnAgentPhase`: aircraft leaving Parked -> drop its turnaround and jobs, recall the vehicles (moved from FuelService OnAgentPhase head; Queued jobs just leave the queue); vehicle agent Parked -> arrival (spec §2.1); aircraft Parked at a stand -> FTurnaround + one fuel job if litres > 0 (moved demand creation; "wants no fuel" line kept).
- `Tick`: SyncFleet; refills/serves that are due (moved Fuelling case: fee, Done log, "another trip needed" -> job Open); Unserviceable re-offer on revision (moved, per job); Open jobs bid (`AssignOpenJobs`); Idle vehicles with a queue `StartNext`; `DepartTheReady` over turnarounds (moved).
- `AssignOpenJobs`: per Open job, eligibility per vehicle (moved `ChooseDepot` walk and refusal chain, minus busy), `ServiceBid::Finish` per eligible vehicle using route lengths from the cache / cruise speed (spec §2.4 clock conversion); lowest finish wins; append; log `Bid:` line with runner-up; the "Fuel route:" polyline line moves to the dispatch in `DriveVehicleTo`. `++BidCallCountForTest` per job bid (replaces ChooseDepot's counter).
- `DriveVehicleTo(Vehicle, Goal, bToFacility)`: moved `SendTruckHome` generalised (no agent -> dispatch from home pose; Taxiing -> turn on the road; Parked -> seeded redirect; to facility only: ungated fallback with `MayDriveUngated`, else retire and `Idle` at home).
- `DescribeAgent`: same strings, from the job (Open/Queued -> "waiting for a truck", Underway -> "truck en route", Serving -> "fuelling").

- [ ] **Step 1: Port tests first** (they are the failing tests): mechanical renames `UFuelService`->`UJobBoard`, `FFuelDemand`->`FServiceJob`, `EFuelRefusal`->`EServiceRefusal`, `GetDemands`->`GetJobs`, `LitresOwed/Delivered`->`QuantityOwed/Delivered`, states Needed->Open, TruckEnRoute->Underway, Fuelling->Serving, `Demand.TruckId`->`Service->AgentForJob(Demand)`, `DwellEndsAt`->`TripEndsAt`, `TurnaroundEndsAt` read from `GetTurnarounds()`. Fixture sets `Service->VehicleSpecs = GetDefault<UScenario>()->FuelVehicles` so each type has its real figures. Semantic rewrites: `FuelQueuesOnABusyDepot` -> second job is Queued or Underway (never Unserviceable, Why None) and is served within the bound; `FuelBusyWaitSkipsChooseDepot` -> `FuelIdleTicksRunNoBids` (60 idle ticks, zero bids); `FuelBusyWaitReoffersOnce` -> `FuelQueuedJobServedWithoutRebids` (served in bound, bids == 1 for it in stage 1); `FuelChooseDepotCachesRouteFinds` -> `FuelBidCachesRouteFinds` (`BidForTest` x6, one Find... per distinct leg on first ask, none after); `FuelPerDemandRefusalRevision` -> same on `AddJobForTest`.
- [ ] **Step 2: New tests** in FuelServiceTest.cpp: `AirportOps.Fuel.ChainsStandToStandWithoutTheDepot` (bSecondStand, two aircraft parked before dispatch, the bowser takes both: same agent id serves both, no ToFacility state observed between the two Serving states); `AirportOps.Fuel.ShortTankGoesViaTheDepot` (bowser capacity set to 400 via VehicleSpecs, two 300 L aircraft: between serves the vehicle visits the depot); `AirportOps.Fuel.DepotDeletedWithdrawsItsVehicles` (vehicle out, `Net->RemoveEntity(Depot)`, next tick: no vehicles, the job Unserviceable NoDepot, the agent gone).
- [ ] **Step 3: Implement** the rename and the board. Build. Run all; fix until green.
- [ ] **Step 4: Architecture lint + full suite; count `UE_LOG(` in old FuelService.cpp (at base) vs new files; commit `feat(service)!: UJobBoard - vehicles own their state, jobs bid by finish time (replaces UFuelService)`; push; PR 1 to main.**

## Stage 2 (PR 2, based on PR 1's branch): re-bidding queued jobs

### Task 3: Re-bid with a margin

**Files:** JobBoardBid.cpp, JobBoard.h/.cpp, FuelServiceTest.cpp (or new `JobBoardRebidTest.cpp` using `AddVehicleForTest` + a drive override).

**Interfaces:** `TFunction<double(FGuidelineNodeId, FGuidelineNodeId, const FServiceVehicleType&)> DriveSecondsOverride;` (test seam, empty in production); `FServiceVehicle& AddVehicleForTest(FName TypeCode, FEntityInstanceId Home, double Cargo);` `void RebidQueued(const URoadNetwork&, const USimClock&)` private, run in Tick when `FleetRevision` or the guideline revision moved since the last pass.

- [ ] **Step 1: Tests** (`AirportOps.Service.Rebid.*`, board with override drive 180 s, no traffic movement needed - vehicles placed by AddVehicleForTest in Serving with StepEndsAt set): `MovesWhenMuchBetter` (job queued behind a long serve on vehicle A; vehicle B added (FleetRevision) idle -> job moves to B; log `Rebid:`); `StaysWithinMargin` (B only 60 s better -> stays); `UnderwayNeverMoves` (job Underway on A, B idle and faster -> stays); `QuietWithoutTrigger` (no revision change -> `GetBidCallCountForTest` unchanged over 60 ticks).
- [ ] **Step 2: Implement** `RebidQueued` (spec §2.5): for each Queued job, its finish on its current vehicle = `ServiceBid::Finish` of that vehicle's queue up to and including it; best alternative = appended bid on every other eligible vehicle; move if `best < current - RebidMarginSeconds`; bump `FleetRevision` on every vehicle step end (serve done, facility done, arrival).
- [ ] **Step 3: Full suite; commit `feat(service): re-bid queued jobs when the fleet or airport changes`; push; PR 2 based on PR 1.**

## Stage 3 (PR 3, based on PR 2): save/load and the inspector

### Task 4: Persist the fleet and jobs

- [ ] **Step 1: Investigate** what survives `UOpsRuntime::LoadFromSlot` (agents are cleared; do parked flights re-dispatch?). Save the FLEET (vehicle ids, types, home, cargo) always - purchase will make it the player's property. Save jobs only if their aircraft come back after a load; otherwise say so in the PR and drop them with the turnarounds as today.
- [ ] **Step 2: Test** `AirportOps.Save.FleetSurvivesALoad` in OpsSaveTest.cpp: a vehicle with 400 L cargo out on a job; save; load; the vehicle exists, Idle, at home, cargo 400, no agent; `Restore:` log line.
- [ ] **Step 3: Implement** (make `Vehicles` a saved UPROPERTY; `OnBeforeRestore` keeps clearing agents' ids; `OnAfterRestore`/equivalent normalises every vehicle to Idle, AgentId 0, empty queue; `SyncFleet` must not re-seed a depot whose vehicles came from the save).

### Task 5: Inspector vehicle line

- [ ] **Step 1: Test** `AirportOps.Service.DescribeVehicle`: `DescribeVehicleAgent(AgentId, Now)` for a vehicle ToJob with one queued -> `"Fuel truck FUEL · to stand %d · 9,700 L · 1 queued"` (exact format fixed in the test); Serving -> `"... · fuelling stand %d · ..."`.
- [ ] **Step 2: Implement** `UJobBoard::DescribeVehicleAgent`; `InspectorWidget.cpp` fills `F.Fuel` from it when the selected agent is a vehicle (read the widget's selection code first; the aircraft path stays as is).
- [ ] **Step 3: Full suite; commit; push; PR 3 based on PR 2.**

### Task 6: Whole-branch review

- [ ] Dispatch a reviewer (most capable model) over `origin/main...feature/service-vehicles-rebid-save` with the spec, this plan and the Global Constraints; fix Critical/Important; re-run the suite.
