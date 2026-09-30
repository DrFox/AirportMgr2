# Ops batch 3 - PR E plan: per-frame fixes

Spec: `docs/superpowers/specs/2026-09-29-ops-batch3-design.md` §6, §7; survey
`docs/2026-09-29-ops-bus-event-candidates.md` §2. Branch `feature/ops-per-frame` (worktree
`C:\repos\airportmgr2-ops-batch3`), stacked on PR D (#419). Base `f056d647`. Baseline full suite:
`1542 test(s) run, 0 failed, 0 crashed`.

## Premises checked against the code (2026-09-30)

- **Item 1: a held taxi-out's failure reads NO occupancy.** `ReplanHeldTaxiOuts` fails when
  `RouteSearch::FindNearestNode` finds no node within 30 m, or `DeparturePlanner::PlanAny` is invalid.
  `PlanAny` uses Occupancy only to RANK a held runway below a free one (`bHeld` in its loop,
  DeparturePlanner.cpp ~187) - never to refuse; both departure errands (`DepartureToEntry`,
  `DepartureBacktrack`) are `EOccupancyUse::Never` (RoutePolicy.cpp), so the route search reads none either.
  Every other input is the network: guideline graph, runway facts (use, in-use end), segments. In play every
  one of those changes through the facade, whose Topology notify rebuilds the guideline graph
  (`URoadSurfacePresenter` -> `FRoadGuidelineBuilder::Build`, which removes and re-adds every derived edge), so
  `GetGuidelineRevision` moves for all of them; a drag (Geometry) leaves the guidelines behind the road, and
  `ReplanHeldTaxiOuts` returns before any attempt while `AreGuidelinesBehindRoad`. The agent's own pose does not
  move while it holds. **Key: `(network object, GuidelineRevision)`. `RunwayFreedCount` is NOT in it** - the
  spec's reason for it ("PlanAny is given occupancy, so a guideline-only gate would strand a hold behind a busy
  runway") does not hold, and a key input that cannot change the answer only costs retries. Deviation, pinned by
  a test that a fully held runway is still planned to.
- **Item 2: which card reads what.** `DescribeRunway`: segments, nodes (EditRevision), runway facts (facade ->
  Topology -> GuidelineRevision). `DescribeTaxiway`: profile (`SetSegmentProfile` bumps EditRevision), stored
  `RestrictedLetter` and live obstructions (entities, segments: facade Topology -> GuidelineRevision; node drags
  -> EditRevision). Neither reads occupancy. `DescribeStand`: entities (placed/removed through the facade ->
  GuidelineRevision), guideline `Incident` / `IsServiceNodeConnected` (GuidelineRevision), `StripClosure`
  (segments -> EditRevision), the pose node's holder (`IsHeld`) and the holder's phase. The holder is covered by
  OccupancyRevision for goal claims, HoldStand/ReleaseHold, every phase change (GroundTraffic.cpp ~1568,
  TakeGoal) - but NOT for a body claim made or dropped by the per-tick claim pass (PR D: "a stand freed by claim
  churn moves no revision"). `StandsFreed` alone would miss a body arriving; so the diff gains ONE counter,
  `StandHoldChangeCount`, bumped whenever the held-stand set differs in either direction.
- **Item 3: the depot card.** `DescribeDepot` reads vehicles, jobs, turnarounds (all private; every public
  mutator is `Step`, `OnAgentPhase`, `RecallVehicleOfAgent`, the restore pair, and the ForTest adders) and
  `Now`. `StepCount` alone misses `OnAgentPhase` and the recall, which run outside Step. So: `UJobBoard::Revision`
  bumped at each public mutator. `Now`: its minutes are `RoundToInt((PromisedFinish - Now) / 60)`, which move at
  half-minute offsets of Now, NOT at game-minute boundaries - a key on the game minute would leave the text up to
  a minute stale. The card therefore DESCRIBES AT THE MINUTE (`Now` floored to 60 s) and keys on that minute, so
  its text is a function of the key exactly.
- **Item 4: the aircraft lookups.** `FlightForAgent` reads `ByAgent` (FlightBoard revision). `DescribeTurnaround`
  reads the flight's `ContractSeconds`/`AcceptedAt` (revision) and Now through `DescribeDuration`'s rounded
  minutes. `JobBoard::DescribeAgent` reads the job board and, WHILE PUMPING only, Now (litres left, 10 L steps).
  Agent phase is not an input of any of them (the flight's phase change reaches the board as a revision). Keys:
  flight lookup on `(board, Revision, agent)`; turnaround text on `(flight, Revision, late?, |minutes shown|)`;
  fuel line on `(job board, Revision, agent)` plus Now while the last answer said it moves with the clock
  (`DescribeAgent(..., bool& bOutMovesWithClock)`). Deviation from the spec's "flight board revision + agent
  phase": the job board and the clock are inputs, the phase is not.
- **Item 5: Land.** `LandChoices::Build` reads `NearestRunwayThreshold(Focus)` (segments -> EditRevision) and
  `CheckArrival(seed, airframe)` (facts -> GuidelineRevision; length -> EditRevision); types are read once. It
  reads NO occupancy and NOT the airport status (PR B's status gates `aircraft.land`'s IsEnabled, not the panel).
  Key: `(network, EditRevision, GuidelineRevision, the nearest runway's seed or none)` - the focus reduced to what
  Build uses of it. `RunwayFreedCount` NOT in it (deviation, same reason as item 1).
- **Item 7: who resets the actor's scale.** `ARoadNetworkActor::SimTimeScale` is `UPROPERTY(Transient)`, 1.0
  by default, written only by `SetSimTimeScale`, whose only production callers are `UOpsRuntime::ApplySpeed` and
  `Tick`. A new actor (a level change, PIE duplication) is met by `Attach`, which ends in `ApplySpeed`.
  `ClearNetwork` replaces the network object, not the actor. `LoadFromSlot` already ends in `ApplySpeed`, and
  `OpsSave::Restore` always returns true (the only early returns precede the clock's restore). Every clock
  speed change goes through `ApplySpeed` (StepSpeed, TogglePause). So Tick's line is dropped and nothing else
  is added.

## Files

| File | Change |
|---|---|
| `Airside/Public/Model/RoadAgent.h`, `Private/Model/RoadAgent.cpp` | `TOptional<uint32> TaxiOutRefusedAt`, reset with `bTaxiOutHoldSaid` |
| `Airside/Public/Model/GroundTraffic.h`, `Private/Model/GroundTraffic.cpp` | gate in `ReplanHeldTaxiOuts`; `TaxiOutReplanAttemptsForTest`; `TaxiOutGateNetwork`; `StandHoldChangeCount` in `DiffFreedom` |
| `Airside/Public/Present/RoadNetworkActor.h` | `SimTimeScaleSetCountForTest` |
| `AirportOps/Public/Model/JobBoard.h`, `Private/Model/JobBoard.cpp` | `Revision()`; `DescribeAgent(..., bool& bOutMovesWithClock)` |
| `AirportOps/Public/Model/FlightBoard.h`, `Private/Model/FlightBoard.cpp` | `TickOffers` early-out; `OfferSnapshotCountForTest` |
| `AirportOps/Private/Present/OpsRuntime.cpp` | Tick no longer sets the scale |
| `Source/AirportMgr/InspectorWidget.h/.cpp` | `FInspectorCardKey`, card cache, aircraft lookup cache, counters, `RefreshWith(Runtime, ...)` seam |
| `Source/AirportMgr/LandAircraftPanelWidget.h/.cpp`, `LandChoices.h/.cpp` | `FLandChoicesKey LandChoices::KeyFor`; Build only on a new key; `BuildCountForTest`; `RefreshFor(C)` seam |
| tests | new `AirsideTests/Private/HeldTaxiOutGateTest.cpp`, new `AirportOpsTests/Private/PerFrameGateTest.cpp`, new `Source/AirportMgr/InspectorCacheTest.cpp`; `LandAircraftPanelTest.cpp`, `AlertsPanelTest.cpp`, `BuildCameraComponentTest.cpp` |

## Interfaces

```cpp
// RoadAgent.h
/** The guideline revision a replan from this hold was last refused at - UGroundTraffic::ReplanHeldTaxiOuts. */
TOptional<uint32> TaxiOutRefusedAt;

// GroundTraffic.h
int32 TaxiOutReplanAttemptsForTest() const { return TaxiOutReplanAttempts; }
uint32 StandHoldChangeCount() const { return StandHoldChanges; }

// RoadNetworkActor.h
int32 SimTimeScaleSetCountForTest() const { return SimTimeScaleSets; }

// JobBoard.h
uint32 Revision() const { return RevisionCount; }
FString DescribeAgent(int32 AgentId, double Now, bool& bOutMovesWithClock) const;

// FlightBoard.h
int32 OfferSnapshotCountForTest() const { return OfferSnapshots; }

// LandChoices.h
struct FLandChoicesKey { const URoadNetwork* Network; uint32 EditRevision, GuidelineRevision; bool bHasRunway; int32 Seed; operator== };
AIRPORTMGR_API FLandChoicesKey KeyFor(const URoadNetwork* Network, const FVector2D& Near);

// InspectorWidget.h
struct FInspectorCardKey { ESelectionKind Kind; int32 Id; const void* Network; uint32 EditRevision, GuidelineRevision,
	OccupancyRevision, StandHolds; const void* JobBoard; uint32 JobRevision; int64 Minute; operator== };
void RefreshWith(const UOpsRuntime* Runtime, const ARoadNetworkActor* Target, const FSelection&, const FAgentFacts* = nullptr);
int32 CardDescribeCountForTest() const; int32 DepotDescribeCountForTest() const;
int32 FlightLookupCountForTest() const; int32 FuelLookupCountForTest() const; int32 TurnaroundComposeCountForTest() const;
```

## Tasks (TDD: counters and tests first, build, run red, gates, green)

1. **Plan** - this file. Commit.
2. **Counters + tests, no gates.** The ForTest counters and `StandHoldChangeCount`, `UJobBoard::Revision`, the
   live flag, `KeyFor`, `RefreshWith`/`RefreshFor` seams - all behaviour-neutral. Tests:
   - `Airside.Model.Traffic.HeldTaxiOut.AsksOncePerEdit` (FPushbackGraph-shaped field: a taxiing departure past J,
     B removed and stranded: 1 attempt over 100 substeps; a line to a new runway node -> 2, and it taxis),
     `.BusyRunwayIsNoRefusal` (every surface of the strip claimed: PlanAny still valid),
     `.ANewNetworkAsksAgain` (same revision number, new network object -> asked).
   - `Airside.Model.Traffic.StandHolds.ChurnIsCounted` (a body asserted onto a pose + Advance: +1, revision flat;
     released + Advance: +1).
   - `AirportOps.Model.FlightBoard.EmptyBoardCopiesNothing` (0 snapshots over 100 TickOffers; 1 per tick with an offer).
   - `AirportOps.Fuel.RevisionMovesOnEveryChange` (Step, OnAgentPhase via a real parked aircraft, recall, adders).
   - `AirportOps.Fuel.LineSaysWhenItMovesWithTheClock` (Serving true; Underway/Done false).
   - `AirportOps.Present.SimTimeScale.SetOnlyWhenItChanges` (attach, 100 ticks: 0 sets; StepSpeed, load of an
     x2 save while at x4, ClearNetwork, TogglePause: the actor's scale is the multiplier each time).
   - `AirportMgr.Inspector.Cache.StandDescribedOnce` (N quiet frames with traffic advancing elsewhere: 1),
     `.RunwayAndTaxiwayDescribedOnce`, `.StandSeesAHold` (HoldStand -> "Reserved"), `.StandSeesChurn` (asserted
     body + Advance: "Reserved", no OccupancyRevision move), `.StandSeesALine` (a guideline edge onto the pose:
     "Reachable"), `.RunwaySeesAMove` (SetNodePosition: length), `.RunwaySeesItsFacts` (Actor->SetRunwayFacts:
     "Takes"), `.DepotDescribedOncePerMinute`, `.DepotSeesTheBoard` (AddJobForTest), `.AircraftLookupsOnce`
     (flight lookup and fuel line 1 over N quiet frames; board revision -> 2), `.FuelLineLiveWhilePumping`,
     `.TurnaroundTicksByTheMinute`.
   - `AirportMgr.UI.LandPanelBuildsOnlyOnChange` (N frames: 1; focus onto a second runway -> 2; focus along the
     same one -> 2; SetNodePosition on its end -> 3; facts via the actor -> 4).
   - `AirportMgr.UI.Alerts.GoToAnAgentSelectsIt`, `Airside.View.BuildCameraComponent.FocusOnLeavesWatch` (pins).
   Build twice (new .cpp files); run; record the reds.
3. **Gates.** ReplanHeldTaxiOuts, the inspector caches, Land, TickOffers, Tick. Green.
4. **Mutations**, each red then `cp` + `touch`: every gate removed; every key input dropped (network, edit rev,
   guideline rev, occupancy rev, stand holds, job revision, minute, flight revision, the live flag, the seed); the
   `StandHoldChanges` bump; LoadFromSlot's `ApplySpeed`; FocusOn's `bWatchingAgent = false`; SelectAndFocus's agent
   select.
5. **Full suite**, counts, report.
