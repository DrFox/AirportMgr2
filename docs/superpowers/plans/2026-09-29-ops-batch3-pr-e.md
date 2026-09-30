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

## Execution notes (2026-09-30)

Full suite: `1563 test(s) run, 0 failed, 0 crashed` (baseline 1542; +21: Airside 4, AirportOps 4, AirportMgr 13 - the
count rose by exactly the tests added, so no bare parent was dropped). Check-Architecture PASS with rule 34, rule-12
warnings 111 (unchanged).

Red before the gates (counters in, gates not): `HeldTaxiOut.AsksOncePerEdit` ("asked once" expected 1, got 99);
`StandHolds.ChurnIsCounted` (not equal x3); `EmptyBoardCopiesNothing` (0, got 100); `Fuel.RevisionMovesOnEveryChange`
(all seven mutators "to be true"); `Fuel.LineSaysWhenItMovesWithTheClock` ("pumping: it moves with the clock");
`SimTimeScale.SetOnlyWhenItChanges` (0, got 100); `Inspector.Cache.QuietCardsDescribeOnce` (1, got 30, per card),
`.AircraftLookupsOnce` (1, got 30 x3), `.DepotDescribedOncePerMinute` (0, got 20); `LandPanelBuildsOnlyOnChange` (1, got
30). The refresh tests (`StandSeesAHold`, `...Churn`, `...ALine`, `RunwaySeesAMove`, `...ItsFacts`, `DepotSeesTheBoard`,
`TurnaroundTicksByTheMinute`, `FuelLineLiveWhilePumping`) and the two UI pins passed with no cache - their reds are the
mutations below.

Mutations (restored by copy + touch, rebuilt, full suite green after):

| Mutation | Red |
|---|---|
| taxi-out gate's `continue` off | AsksOncePerEdit ("asked once" 1, got 99) |
| `++StandHoldChanges` off | ChurnIsCounted (not equal x3); with the card cache on, `Cache.StandSeesChurn` ("the card names it ('Empty')") |
| JobBoard OnAgentPhase bump off | RevisionMovesOnEveryChange ("an agent's phase moves the revision") |
| live flag never set | LineSaysWhenItMovesWithTheClock; with the flag off the card's key, `FuelLineLiveWhilePumping` ("500 L are in ('... 2,900 L left ...')") |
| TickOffers early-out off | EmptyBoardCopiesNothing (0, got 100) |
| LoadFromSlot's ApplySpeed off | SetOnlyWhenItChanges ("a load: the saved speed reaches the actor" 2, got 4) |
| FocusOn's `bWatchingAgent = false` off | FocusOnLeavesWatch ("a Go leaves watch mode") |
| SelectAndFocus agent select off | GoToAnAgentSelectsIt ("the aircraft is selected" not equal) |
| Land gate off | LandPanelBuildsOnlyOnChange (1, got 30) |
| card reuse off | QuietCardsDescribeOnce (1, got 30 per card) |
| flight lookup gate off | AircraftLookupsOnce (1, got 30) |
| card key without EditRevision / GuidelineRevision / OccupancyRevision / StandHolds / JobRevision / Minute | RunwaySeesAMove; StandSeesALine + RunwaySeesItsFacts; StandSeesAHold; StandSeesChurn; DepotSeesTheBoard; DepotDescribedOncePerMinute ("the next minute: one describe" 1, got 0) |
| flight lookup without the board revision; fuel line without the job revision; without the live flag; turnaround without its minutes | AircraftLookupsOnce ("looked up again" 2, got 1; "asked again" 2, got 1); FuelLineLiveWhilePumping; TurnaroundTicksByTheMinute ("3 h left" unchanged) |
| Land key without Seed / EditRevision / GuidelineRevision | LandPanelBuildsOnlyOnChange ("onto runway B", "dragged longer", "turned to grass": 1, got 0) |
| `SetSimTimeScale` back in Tick | rule 34 `scale-on-change` FAIL |

Two refresh tests first compared the whole card, which a taxiing aircraft changes every frame anyway; under the
mutations they passed. They now read their own line (`FAircraftRig::Line`) and went red.

Not pinned: the network-object resets (the taxi-out gate's `TaxiOutGateNetwork`, the three pointers on
`FInspectorCardKey`, `FLandChoicesKey::Network`) - a new network also moves the revisions, and no test can hold every
revision equal across two objects; the Revision bumps in `ResolveVehicles` and `Serialize`.

Deviations from the spec (each argued at its site):

- **Taxi-out gate is `(network, GuidelineRevision)`, no `RunwayFreedCount`.** `PlanAny` ranks a held runway, never
  refuses one; both departure errands read no occupancy. Pinned by `BusyRunwayIsNoRefusal`.
- **Land key has no `RunwayFreedCount`, no airport status**: Build reads neither. Focus is reduced to the runway seed.
- **Depot card keys on a new `UJobBoard::Revision`, not `StepCount`**: OnAgentPhase and the recall change the board
  outside Step. The card now describes AT THE MINUTE (Now floored to 60 s): its "+N min" figures are the minute's,
  up to 59 game s behind the old per-frame ones, so the text is a function of the key.
- **Aircraft lookups key on the boards' revisions and the clock, not agent phase**: the phase is no input; the fuel
  line is asked every frame while pumping (its live flag), the turnaround when its printed minutes move.
- **`StandHoldChangeCount` added to UGroundTraffic**: OccupancyRevision and StandsFreed miss a body arriving on a pose.
- **The scale-set counter lives on UOpsRuntime** (`TimeScaleSetsForTest`): rule 19 forbids a new ForTest member on
  ARoadNetworkActor. Rule 34 added for the shape.

Finding: `URoadNetwork::SetRunwayFacts` bumps no revision. Every production write goes through the facade, whose
Topology rebuild moves `GetGuidelineRevision`; the three new gates rely on that, and `Cache.RunwaySeesItsFacts` pins
it. A future facts write that skips the facade would leave all three stale.

UE_LOG / comment lines, `f056d647` -> now (no file fell): FlightBoard.cpp 24/264 -> 24/267; JobBoard.cpp 22/185 -> 22/188;
OpsRuntime.cpp 25/355 -> 25/360; FlightBoard.h 0/388 -> 0/390; JobBoard.h 0/413 -> 0/429; OpsRuntime.h 0/190 -> 0/193;
GroundTraffic.cpp 34/595 -> 34/614; RoadAgent.cpp 15/533 -> 15/533; GroundTraffic.h 0/758 -> 0/778; RoadAgent.h
0/779 -> 0/786; RoadNetworkActor.h 0/899 -> 0/900; InspectorWidget.cpp 8/128 -> 8/148; InspectorWidget.h 0/138 -> 0/191;
LandAircraftPanelWidget.cpp 1/22 -> 1/24; LandAircraftPanelWidget.h 0/49 -> 0/60; LandChoices.cpp 0/11 -> 0/12;
LandChoices.h 0/32 -> 0/48.

Unverified in PIE: all of it - the cards, the Land panel and the held taxi out are measured headlessly only.

## Review ledger (fresh review of PR E, 2026-09-30: 0 Critical, 2 Important)

Rulings are the orchestrator's. Red lines are from each fix's test with the fix taken out (the fix and its test were
written together; the red is the mutation), unless marked control. Full suite after: `1566 test(s) run, 0 failed, 0
crashed` (+3). Check-Architecture PASS with rules 34 (hardened) and 35, rule-12 warnings 111.

| # | Finding | Ruling / fix | Test (red line) |
|---|---|---|---|
| I1 | The three gates lean on runway facts moving GuidelineRevision, which `URoadNetwork::SetRunwayFacts` never does; `AsksOncePerEdit` itself used that bypass | New Check-Architecture rule 35 `facts-through-facade`: the network's `SetRunwayFacts(` may be called only from RoadEditFacade*.cpp, Testing/ and the test modules (`Facade->` / `Target->` edit-target calls, declarations and definitions exempt; comments and string literals stripped). Cited in the ENFORCED BY of the taxi-out gate, FInspectorCardKey and FLandChoicesKey. `AsksOncePerEdit` now strands the hold by DELETING B, the only line onto the runway (a genuine NoRoute), and fixes it by drawing a line back - a real graph edit, the input the gate reads; no facts write at all | control: `Target->Network->SetRunwayFacts(...)` added to OpsRuntime.cpp -> `FAIL facts-through-facade: OpsRuntime.cpp:1225 calls SetRunwayFacts( on something other than the facade`; rewritten test under the gate-off mutation: "asked once on entering the hold" 1, got 4; "100 quiet substeps" 1, got 104 |
| I2 | Attach's `ApplySpeed` unpinned; the network-clear step trivial | `SetOnlyWhenItChanges` steps to x4 and attaches a second, fresh actor: its scale is the multiplier, not its default 1. The clear step now asserts the actor is OFF its default beforehand (x2) and that the runtime set nothing across the clear - so a clear that reset the scale reads wrong. The mutation that would show it (a reset in `ClearNetwork`) is itself a second writer of the scale, which rule 34 fails before any test runs (control below) - that is the honest limit of this step | mutation "no ApplySpeed in Attach": "attached to it at x4: it runs at the clock's multiplier" 4, got 1 |
| - | `RunwayFreedCount()` consumed by nothing | Deleted, with `RunwayFreedTotal`. Its two test uses (`RunwayFreed.TakeOff`, `.DespawnOnRunway`) already counted `OnRunwayFreed` broadcasts beside it; the counter lines were dropped, not replaced | - |
| M | Land gate missed the type count | `Types.Num() == JudgedTypeCount` on the gate; `SetTypeSourceForTest` | `AirportMgr.UI.LandPanelJudgesWhenTypesArrive` (mutation: "the types arrive: judged again" 2, got 1; "and their rows are there" 2, got 0) |
| M | Network-object resets unpinned | Two deterministic builds at equal revisions | `Airside.Model.Traffic.HeldTaxiOut.ANewNetworkAsksAgain` (mutation, reset off: "the other network, same number: asked again" 1, got 0); `AirportMgr.UI.LandChoicesKeyNamesTheNetwork` (mutation, Network off `==`: "and still two keys: the network is on it") |
| M | Raw `const void*` identity in the keys | `FWeakObjectPtr` (index + serial, no complete type needed in the header) | covered by the two tests above for the Land key; the card key's three identities are still not pinned by a card test |
| M | Rule 34 gaps | Direct `SimTimeScale =` writes fail; "which function" re-read at indented definitions and at every `namespace`; string literals and /* */ stripped; AirsideEditor scanned. The AirsideEditor control first PASSED: the stripper read `/*` inside a `//` comment (a path glob) as a block opening and blinded the rest of the file - now whichever opens first wins | controls: direct write in RoadNetworkActor.cpp -> FAIL "writes SimTimeScale directly"; anonymous-namespace `SneakScale` after ApplySpeed -> FAIL "from SneakScale"; indented `static void SneakScale2` right after an ApplySpeed body -> FAIL; a call in RoadBuildEdMode.cpp -> FAIL (after the fix); a TEXT("...SetSimTimeScale(1)...") log line and single- and multi-line /* */ calls -> PASS |
| M | `UJobBoard::Revision` overclaimed | "every mutator of Jobs/Vehicles/Turnarounds"; the Serialize and ResolveVehicles bumps named REDUNDANT (OnBeforeRestore runs immediately before the blob; the letter table is read by no Describe), not "unpinned" | - |
| M | Quiet cards measured with nothing moving | An aircraft taxis a line of its own through the 90 quiet frames; asserted to have moved > 1 m | green (describes still 1 per card) |
| M | Unmarked claims | ENFORCED BY added: RoadNetworkActor.h "when the speed changes" (rule 34 + SetOnlyWhenItChanges); JobBoard.h 3-arg DescribeAgent (LineSaysWhenItMovesWithTheClock, Cache.FuelLineLiveWhilePumping). GroundTraffic.h's "the Land panel reads no occupancy" reworded to the signature fact ("LandChoices::Build is given no traffic model at all") | - |
| M | Save slot left on disk | `ON_SCOPE_EXIT` deletes `AirportOpsTest_SimTimeScale` however the test ends | none on disk after the suite |

UE_LOG / comment lines, `f056d647` -> now (no file fell): JobBoard.h 0/413 -> 0/433; GroundTraffic.cpp 34/595 -> 34/616;
GroundTraffic.h 0/758 -> 0/775; RoadNetworkActor.h 0/899 -> 0/902; InspectorWidget.h 0/138 -> 0/195;
LandAircraftPanelWidget.h 0/49 -> 0/68; LandChoices.h 0/32 -> 0/51; the rest as in the execution notes.

## Whole-stack review ledger (origin/main...feature/ops-per-frame, 2026-09-30: 0 Critical, 1 Important)

Fixed at the tip, stacked branches untouched. Rulings are the orchestrator's; M5 is filed as an issue by the orchestrator and
not fixed here. Each test was written first and run red against the unfixed code (red lines below); each new gate was then
mutated out on its own and seen red again. Restores by copy + touch, rebuilt, suite green after. Full suite: `1570 test(s)
run, 0 failed, 0 crashed` (+4). Check-Architecture PASS, rule-12 warnings 111.

| # | Finding | Ruling / fix | Test (red line before the fix; mutation) |
|---|---|---|---|
| I1 | A closed airport admits no arrivals only at the entry doors (Accept, Land); the queue pass, the door onto the runway, cleared anyway | `UFlightBoard::TickQueue` asks the same `AdmitsArrivals` predicate as Accept, before the stand re-reserve; `FQueueTick::bClosed` tells the pass, which then keeps no safety net (reopening is an AirportStatusChanged, which dirties the pass). `LoadFromSlot`'s not-Open branch now cancels EVERY Accepted and Inbound flight, unscored (`CancelRequeued` widened and renamed `CancelUnarrivedAtLoad`: before RearmSchedules, so none is armed or held) | `AirportOps.Present.ArrivalQueue.ClosedAirportDispatchesNothing` (new; red: "closed: the holding flight is not cleared to land" not equal, "no aircraft was dispatched" 0, got 1; mutation "no net gate": "no safety net ticks for a queue that cannot move"); `AirportOps.Present.Airport.LoadRederivesWithoutCancelling` rewritten - an FTestAirport field the planted flight CAN land on, ETA 5 s after the load, ticked past it; "NOT cancelled" is now "no satisfaction change" (red: "nothing was dispatched" 0, got 1; "flight 1 is not landing"; "the planted flight is not coming (Landing)"; mutation "load cancels Inbound only": "not coming (Inbound)") |
| M1 | The Land panel ignored the status | `EAirportStatus` on `FLandChoicesKey`; `LandChoices::Build(..., Status)` refuses every type while not Open ("the airport is closed"); the panel reads it from the runtime (none - the editor mode - is open, the Land button's rule). PaintRows's contract holds: every greyed row is a click the game would refuse | `AirportMgr.UI.LandPanelGreysWhileClosed` (new; red: "the close alone re-judges the rows" 2, got 1; "every row greyed" 0, got 12; mutation "status off the key": same; mutation "Build ignores status": "every row greyed" 0, got 12) |
| M2 | The inbox's "Closed" slot kept 8 px of padding while hidden | The SizeBox itself is collapsed with its text (`DemandStatusBox`), so its slot's padding goes with it | `AirportMgr.UI.OfferInbox.StatusSwapKeepsHeight` (new, desired height by SlatePrepass on the widget's Slate tree - it works headless; red: "open and closed, the card is the same height" 30, got 38; mutation "box never collapsed": same) |
| M3 | The arrival callback re-queued whatever the flight had become | Only `Due->Phase == Accepted` joins the queue | `AirportOps.Model.FlightBoard.ArrivalRequeuesOnlyAnAccepted` (new, with an Accepted control that does join; red: "a cancelled one stays cancelled" not equal) |
| M4 | A departure never turned around (Depart from the fallback junction) scored nothing | RULING applied. `UJobBoard::EndTurnaround` is now THE one publisher of TurnaroundEnded (and poster of the part-fuelled fee): DropAircraft calls it for a real turnaround; OnAgentPhase's new sibling branch - Parked -> a departing phase, NO turnaround, aircraft ARMED for a runway - calls it with {Unfuelled, Delivered 0, Wanted}. WANTED HAS AN HONEST SOURCE: `LitresOwedFor(AgentId, Airframe)` (the flight's FuelLitres), else `DefaultLitres` - the very expression a turnaround's fuel job takes its load from. Cannot double: the branch runs only when TurnaroundFor finds nothing, and "armed" excludes the re-offer's Parked -> Taxiing to a stand | `AirportOps.Model.Bus.DepartFromFallbackReadsTaxiOut` extended with a roster wired as the runtime wires it (red: "one TurnaroundEnded for the departure" 1, got 0; "its airline minds: the whole shortfall penalty" 0.44, got 0.50); mutation "no armed test": `FallbackParkStaysTaxiIn` "no TurnaroundEnded was published for the junction" 0, got 1 |
| M6 | The airport button lit while closed | IsActive is always false: ACCENT MEANS ARMED AND NOTHING ELSE; the caption is the signal | `AirportMgr.Actions.AirportStatusCaption` (red: "closed: NOT lit - the caption says it" to be false; mutation restoring the old lambda: same) |

UE_LOG / comment lines, `04a732e4` -> now (no file fell): FlightBoard.cpp 24/267 -> 24/279; FlightBoard.h 0/390 -> 0/396;
JobBoard.cpp 22/188 -> 23/196; JobBoard.h 0/433 -> 0/440; OpsRuntime.cpp 25/360 -> 25/366; LandChoices.h 0/51 -> 0/54;
LandChoices.cpp 0/12 -> 0/14; LandAircraftPanelWidget.h 0/68 -> 0/72; LandAircraftPanelWidget.cpp 1/24 -> 1/25;
OfferInboxWidget.cpp 1/113 -> 1/116; OfferInboxWidget.h 0/98 -> 0/100; BuildActions.cpp 2/141 -> 2/144.

Unverified in PIE: all of the above.

### Whole-stack re-review (scoped, 2026-09-30: 0 Critical, 1 Important) - last round

Full suite after: `1571 test(s) run, 0 failed, 0 crashed` (+1: the split). Check-Architecture PASS, rule-12 warnings 111 (one
new claim, "the one door onto the runway", fell out of its ENFORCED BY window when m6 grew the comment - reworded). Red
lines marked "pin" are for behaviour the previous round already fixed: the test was written against it, so its red is the
mutation.

| # | Finding | Fix | Test (red line) |
|---|---|---|---|
| I-1 | LoadRederivesWithoutCancelling stopped pinning "a load publishes no status change", and its name no longer said what it asserted | Split, sharing one rig (`FAirportClosedLoadRig`: closed, saved with a landable Accepted flight due 5 s later, reopened, loaded, run past the ETA). `AirportOps.Present.Airport.LoadRederivesSilently` (renamed): the status re-derived closed, `UAirport::ChangeCountForTest()` unchanged across the load and the ticks, no satisfaction change. `AirportOps.Present.Airport.ClosedLoadCancelsTheUnarrived` (new): nothing dispatched, no flight Landing, the planted flight Cancelled, unscored. ENFORCED BY lines re-pointed (Airport.h, OpsRuntime.cpp x2, FlightBoard.cpp) | pin; mutation "the load's Reseat -> Refresh": "no status change was published" expected 2, got 3. The cancel half's reds are the previous round's I1 lines |
| m1 | Two copies of the owed-litres read | `UJobBoard::LitresWanted(AgentId, Airframe)` (private), used by the fuel-job site and the M4 site. DepartFromFallbackReadsTaxiOut wires `LitresOwedFor` to 1234 L (asserted not the default) and asserts Wanted equals it | pin; mutation "DefaultLitres at the M4 site": "owed what the flight was owed" 1234, got 317.8 |
| m2 | The M4 log line said "unfuelled" as a literal | Logs `FuelOutcomeOf(0, Wanted)` by name | - |
| m3 | Three docs still named DropAircraft the one publisher | OpsEventBus.h (FTurnaroundEndedEvent), JobBoard.h (DropAircraft), JobBoard.cpp (DepartTheReady's note) now name EndTurnaround and its two callers | - |
| m4 | FLandChoicesKey's ENFORCED BY missed the status | Adds AirportMgr.UI.LandPanelGreysWhileClosed | - |
| m5 | StatusSwapKeepsHeight never measured the card as built | Measures a fresh inbox before any ShowAirportStatus (Open's first call is a no-op, so only the build collapses the box) | pin; mutation "no build-time collapse": "and fresh from the build, the same again" 30, got 38 |
| m6 | TickQueue tested the pause before the closure, so a closed, paused airport armed a net | AdmitsArrivals asked first | ClosedAirportDispatchesNothing gains a paused-and-closed step - red before the fix: "paused and closed: no safety net either" to be false |

UE_LOG / comment lines, `1082a4db` -> now (none fell): FlightBoard.cpp 24/279 -> 24/280; JobBoard.cpp 23/196 -> 23/197;
JobBoard.h 0/440 -> 0/447; OpsEventBus.h 2/157 -> 2/159; LandChoices.h 0/54 -> 0/55; Airport.h 0/44 -> 0/44.
