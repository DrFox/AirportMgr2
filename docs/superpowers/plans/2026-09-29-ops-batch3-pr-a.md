# Ops batch 3 - PR A plan: OfferAccepted, TurnaroundEnded, the scenario asset

Spec: `docs/superpowers/specs/2026-09-29-ops-batch3-design.md` §2, §7. Branch `feature/ops-batch3`
(worktree `C:\repos\airportmgr2-ops-batch3`, base main `c48fd130`). Baseline full suite on this tree:
`1468 test(s) run, 0 failed, 0 crashed` (2026-09-29).

Order: the two events first (C++ only, one build each), the asset last - it needs the tuning field to
exist when it is saved, and a headless commandlet against the built DLL.

## Files

| File | Change |
|---|---|
| `AirportOps/Public/Model/ServiceJob.h` | `UENUM() EFuelOutcome { Fuelled, PartFuelled, Unfuelled }` |
| `AirportOps/Public/Model/OpsEventBus.h` | `FOfferAcceptedEvent`, `FTurnaroundEndedEvent`; both appended to `FOpsEvent`; include `Model/ServiceJob.h` |
| `AirportOps/Private/Model/OpsEventBus.cpp` | their `Describe()` |
| `AirportOps/Private/Model/FlightBoard.cpp` | `Accept` publishes `FOfferAcceptedEvent` after the stand is held |
| `AirportOps/Public/Model/JobBoard.h` | `FOpsEventBus* Bus = nullptr;` beside `Ledger` |
| `AirportOps/Private/Model/JobBoard.cpp` | `DepartTheReady` publishes `FTurnaroundEndedEvent` only after `DepartAgent` returns `None` |
| `AirportOps/Public/Model/OpsDefinition.h` | `FAirlineSatisfactionTuning::ShortfallPenalty = 0.06`, commented unjudged (2026-09-29) |
| `AirportOps/Public/Model/AirlineRoster.h/.cpp` | `OnTurnaroundEnded(const FTurnaroundEndedEvent&, const UFlightBoard* Flights)` |
| `AirportOps/Private/Present/OpsRuntime.cpp` | `WireBus`: Airlines on TurnaroundEnded (Reaction), Alerts on OfferAccepted (Reaction, MarkDirty); `Attach`: `JobBoard->Bus = &Bus`; `Detach`: `JobBoard->Bus = nullptr` |
| `AirportOps/Private/Content/AirportOpsSettings.cpp` | one-shot Log line when the configured scenario IS found (the "asset's name" line the spec verifies by) |
| `Tools/Python/build_scenario.py` | new: creates `/Game/Ops/DA_Scenario_Default` from `UScenario`'s CDO, writes the ini line |
| `Config/DefaultGame.ini` | `[/Script/AirportOps.AirportOpsSettings] DefaultScenario=Scenario:DA_Scenario_Default` |
| `Content/Ops/DA_Scenario_Default.uasset` | new asset |
| tests | `FlightBoardEventsTest.cpp`, `AirlineRosterTest.cpp`, `FuelServiceTest.cpp`, `OpsRuntimeBusTest.cpp`, `AirportOpsSettingsTest.cpp` - all EXISTING files, so no two-build trap |

## Interfaces (checked against the live headers, 2026-09-29)

```cpp
// ServiceJob.h - UENUM so Describe() can print it through UEnum::GetValueAsString like every other event.
UENUM()
enum class EFuelOutcome : uint8 { Fuelled, PartFuelled, Unfuelled };

// OpsEventBus.h. FlightId, not the spec's "Flight": every flight event on the bus names the field FlightId.
struct AIRPORTOPS_API FOfferAcceptedEvent
{
	int32 FlightId = 0;
	FName AirlineId;
	FEntityInstanceId Stand;
	static const TCHAR* EventName() { return TEXT("OfferAccepted"); }
	FString Describe() const;
};

struct AIRPORTOPS_API FTurnaroundEndedEvent
{
	int32 AircraftAgentId = INDEX_NONE;
	FEntityInstanceId Stand;
	EFuelOutcome Outcome = EFuelOutcome::Fuelled;
	double Delivered = 0.0;
	double Wanted = 0.0;
	static const TCHAR* EventName() { return TEXT("TurnaroundEnded"); }
	FString Describe() const;
};
```

`UFlightBoard::FindByAgent` is PRIVATE; its public const face is `FlightForAgent(int32)` (what
`LitresOwedFor` already uses). The roster calls that. The board is PASSED to the handler rather than
held by the roster: WireBus owns both, and a held pointer would be one more thing Detach must clear.

Outcome, read in `DepartTheReady` before the departure (the existing locals): `bUnfuelled` (fuel job
Unserviceable) -> `PartFuelled` if `Delivered > 0`, else `Unfuelled`; otherwise `Fuelled`.
Score: `Fuelled` or `Wanted <= 0` -> nothing; else `-ShortfallPenalty * (1 - Clamp(Delivered / Wanted, 0, 1))`,
causes `left part-fuelled` / `left unfuelled`.

## Task 1 - FOfferAcceptedEvent

1. RED, model: `AirportOps.Model.FlightBoard.AcceptPublishesOfferAccepted` in `FlightBoardEventsTest.cpp`
   - fixture gains `TArray<FOfferAcceptedEvent> Accepted` + subscription; offer a Cumbria flight, Accept,
   Drain: exactly one event, its FlightId, AirlineId, and Stand == the flight's held stand; a refused
   Accept (flight no longer Offered) publishes nothing more. Build fails to compile until the struct
   exists - that is not the red; add the struct + Describe + variant entry first with NO publish, build,
   run, see `expected 1, got 0`.
2. RED, composition: `AirportOps.Present.Alerts.AcceptDirtiesAlerts` in `OpsRuntimeBusTest.cpp` - an
   attached runtime over a stand; tick to quiet; record `Alerts->RecomputeCountForTest()`; accept an offer
   through the board as the game module does (`OfferViewModels.cpp:212`); `Tick(0)`; the count rose by
   exactly one. (Red until WireBus subscribes. `EveryEventHasASubscriber` is also red until then.)
3. GREEN: publish in `Accept` after `++RevisionCount`; `Bus.Subscribe<FOfferAcceptedEvent>(Reaction, "Alerts", MarkDirty)`.
4. Mutation: delete the subscription -> AcceptDirtiesAlerts AND EveryEventHasASubscriber red; restore (cp + touch).

FINDING TO REPORT, not forced: the spec's mutation target is "the HeldStandLost test". No HeldStandLost
state can be produced BY an accept - `UStandAllocator::Reserve` only picks `IsStandCandidate()` stands with
a pose, and `HeldStandIsGone` is true only for a missing entity or pose - so an accept never raises or
clears one, and a HeldStandLost-shaped test would be green with the subscription removed. The composition
test measures what the subscription does (the pass runs), which is what goes red.

## Task 2 - FTurnaroundEndedEvent, scored

1. `ShortfallPenalty` on the tuning; `EFuelOutcome`; the event struct + Describe + variant entry;
   `UJobBoard::Bus`; `UAirlineRoster::OnTurnaroundEnded` declared with an EMPTY body; nothing published.
2. RED, roster (`AirlineRosterTest.cpp`, fixture subscribes TurnaroundEnded with a real `UFlightBoard`
   holding a flight of airline A flown by agent 7):
   - `AirportOps.Model.Airlines.TurnaroundPartFuelledScoresInProportion` - 1000 of 2500 L: 0.5 -> 0.5 - 0.06 * 0.6 = 0.464, cause `left part-fuelled`
   - `AirportOps.Model.Airlines.TurnaroundUnfuelledScoresTheFullPenalty` - 0 of 2500: 0.44, cause `left unfuelled`
   - `AirportOps.Model.Airlines.TurnaroundFuelledScoresNothing` - Fuelled 2500/2500, and Wanted 0: no change, no event
   - `AirportOps.Model.Airlines.TurnaroundOfNoFlightIsSkipped` - agent 99: nothing moves
3. RED, publisher (`FuelServiceTest.cpp`): `AirportOps.Fuel.RefusedDepartureEndsNoTurnaround` - the
   fixture's runway recipe moves into `FFuelFixture::LayRunway()` (Build calls it when `bWithRunway`),
   remembering the taxiway's north node. No runway: park, fuel, run past the deadline, 5 more seconds of
   refusals, drain a local bus: 0 events. `LayRunway()`: advance until one event; 5 more seconds: still
   exactly 1, Outcome Fuelled, Delivered 300 of 300, the agent id.
4. RED, composition: `AirportOps.Present.Bus.TurnaroundShortfallReachesAirline` - attached runtime, its
   job board's Bus IS the runtime's bus (the Attach line), a flight of a test airline added to the
   runtime's board with agent 7, publish an Unfuelled TurnaroundEnded, `Tick(0)`: satisfaction fell by
   the penalty.
5. GREEN: roster body; publish in `DepartTheReady` after the refusal `continue`; WireBus subscription;
   Attach/Detach bus lines.
6. Mutations: (a) drop the WireBus subscription -> TurnaroundShortfallReachesAirline + EveryEventHasASubscriber
   red; (b) move the publish ABOVE the refusal check -> RefusedDepartureEndsNoTurnaround red (many events);
   (c) drop `JobBoard->Bus = &Bus` in Attach -> TurnaroundShortfallReachesAirline red.

## Task 3 - DA_Scenario_Default

1. RED: `AirportOps.Content.DefaultScenarioIsTheAsset` in `AirportOpsSettingsTest.cpp` - a fresh
   `UOpsCatalog::LoadFromAssetManager()`, `ResolveDefaultScenario`: not the CDO, named
   `DA_Scenario_Default`, and its `ShortfallPenalty` equals the CDO's (built from the defaults). Red now:
   nothing configured, the CDO comes back.
2. GREEN: `Tools/Python/build_scenario.py` (headless, `-run=pythonscript`), creating the asset with
   `AssetTools.create_asset(..., unreal.Scenario, None)` - a `UPrimaryDataAsset` needs no factory beyond
   the DataAsset one; the script falls back to `DataAssetFactory` with `data_asset_class` set - saved
   with `save_asset(..., only_if_is_dirty=False)` (memory: save writes nothing unless forced), then
   verifies by `does_asset_exist` and the .uasset on disk, and writes the ini section idempotently.
   The ResolveDefaultScenario found-branch gains a one-shot Log line naming the asset.
3. Verify: the .uasset exists with a non-trivial size, `grep -a DA_Scenario_Default` finds it; the test log
   shows `DefaultScenario 'Scenario:DA_Scenario_Default' resolved` (new) and the Attach line
   `Scenario 'DA_Scenario_Default'` instead of `using UScenario's built-in defaults`.
4. Mutation: comment out the ini line -> DefaultScenarioIsTheAsset red; restore.

## Finish

Full suite, no filter; `UE_LOG` and comment-line counts of touched production files against the
baseline (74 / 1738); `Check-Architecture` verdict line; rule-12 warning count not risen (111).
Unverified in PIE: the inbox row `▼ left unfuelled (-0.06)` and the Details panel view of the asset.

## Review ledger (fresh review of PR A, 2026-09-29: 0 Critical, 3 Important)

Rulings are the orchestrator's; each fix got a test that failed first unless marked.

| # | Finding | Ruling / fix | Test (red line) |
|---|---|---|---|
| I1 | The inspector's manual Depart (`DepartSelected` -> `DepartAgent`) skipped `DepartTheReady`: no TurnaroundEnded, no part-fuelled fee - an exploit | ONE site: `UJobBoard::DropAircraft`, when the aircraft leaves Parked for a DEPARTING phase (Manoeuvring/Reversing/Taxiing/Departing), only if a turnaround existed. Gone (Unstick despawn) is not a turnaround end - PR B scores it as Cancelled. `DepartTheReady` no longer publishes or pays. Outcome from the figures (`FuelOutcomeOf`): Wanted<=0 or delivered within `FuelledWithinLitres` (0.5, FinishServe's Done threshold) -> Fuelled; 0 delivered -> Unfuelled; else PartFuelled. Part-fuelled fee posted at the same site; never twice, since FinishServe pays only a Done job, which reads Fuelled | `AirportOps.Fuel.ManualDepartEndsTurnaroundOnce` ("a manual depart ends the turnaround, once" expected 1, got 0); `AirportOps.Fuel.RetiredAircraftEndsNoTurnaround` (guard) |
| I2 | The publisher's figures were never pinned | Local bus in `PartFuelledPaysForWhatItGot` (PartFuelled 1000/2500) and `UnserviceableStillDeparts` (Unfuelled 0/300) | green on the old publisher (same figures); red when the new site's publish is removed |
| I3 | `DefaultScenarioIsTheAsset` asserted asset == CDO: tautological, breaks on the first tune | Dropped; not-CDO + name kept. "Creation changed nothing" asserted by `build_scenario.py` at creation (`check_matches_cdo`, all 10 UScenario properties); control: a tweaked in-memory asset fails it | script control run |
| M1 | Reaction-tier comment claimed tier settling | Reworded: publish order within the drain keeps the flight findable; `UnfuelledDepartureLowersAirline` (real depot-less departure through the runtime) named as ENFORCED BY | red when the publish site is removed |
| M2 | OfferAccepted->Alerts comment overclaimed | Reworded: nothing an accept changes is re-derived today; kept for the pass's correctness and PR D | - |
| M3 | AcceptDirtiesAlerts could be dirtied by the arrival clock | Lead time 1e7 s | - |
| M4 | Shortfall composition test true with a zero penalty | `TestTrue(Penalty > 0)` | - |
| M5 | "resolved" line once per process | Once per distinct resolved path | `AirportOps.Content.ResolvedScenarioIsLoggedPerAsset` (expected 1, got 0) |
| M6 | No accept-refused-by-Reserve case | Second offer with the only stand held: refused, nothing published | green (behaviour existed) |
| M7 | `Detach` left `Airlines->Bus` set | Nulled beside JobBoard's | `AirportOps.Present.Bus.DetachUnhooksEveryPublisher` ("and the airline roster's" expected null) |
