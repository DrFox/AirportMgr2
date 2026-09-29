# Facility Upgrades + Fleet Purchase Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A fuel depot starts with its kit and no vehicles; the player buys sheds (bays) and vehicles, sells idle vehicles, pays upkeep for both, and every button on the depot card is rendered from one quote the rules also judge by.

**Architecture:** One world-free purchase service, `UFacilityPurchases` (AirportOps `Model/`), owned by `UOpsRuntime`, judges every command through the same private `Judge*` functions its `Quote` uses. Vehicles enter and leave only through two new `UJobBoard` methods; a shed enters only through a new `URoadNetwork` mutator reached via a new `URoadEditFacade` door (Topology rebuild, then undo history cleared). World/presentation reach the model through two `TFunction` hooks set in `UOpsRuntime::Attach`. Two new bus events wake the job board pass; the inspector renders the quote and runs three new inspector-only `FBuildAction` rows.

**Tech Stack:** UE 5.8.2 C++, Airside + AirportOps plugins, AirportMgr game module, UE automation tests.

**Spec:** `docs/superpowers/specs/2026-09-29-facility-upgrades-and-fleet-purchase-design.md` (read §1 rulings and §6 deviations first). Also `CLAUDE.md` (binding), `docs/superpowers/specs/2026-09-29-ops-event-bus-design.md`, `docs/superpowers/specs/2026-09-28-service-vehicle-lifecycle-design.md` §3.4.

## Global Constraints

- Figures (spec §2 table): Shed price 40,000, upkeep 200/day, grants 1 vehicle slot. UTILITY price 25,000, upkeep 150/day, resale 50%. FUEL bowser price 90,000, upkeep 500/day, resale 50%. `ResaleFraction` default 0.5.
- R2: one shed = one vehicle bay (any type). R3: a NEW (player-drawn) depot has its start kit `{Shed, Tank, Pump}` and **no vehicles**.
- R5: only Idle vehicles (parked at home, empty queue, no current job, no agent) can be sold; sheds cannot be sold. R8: a shed purchase clears the build undo history; vehicle trades never touch undo. R9: a shed is buyable only into a free reserved slot. R10: the selected depot shows its ghost slots outside edit mode; every other depot does not.
- `EPurchaseRefusal`: `None, NotAFacility, CannotAfford, NoSlotReserved, NoFreeBay, UnknownType, VehicleBusy` - exactly these.
- Money is posted synchronously inside the command; a refused command posts nothing and publishes nothing. Sheds post `ELedgerCategory::Placement`; vehicle buys and sale credits post `ELedgerCategory::Fleet`.
- Log lines (`LogAirportOps`): `Purchase: depot 3 bought FUEL for 90000 (2/2 bays)`, `Purchase refused: depot 3 FUEL - NoFreeBay`, `Purchase: depot 3 sold vehicle 7 for 45000`, `Purchase: depot 3 bought Shed for 40000 (2/3 slots)`.
- Every bus subscription is in `UOpsRuntime::WireBus` (Check-Architecture rule 31). `AirportOps.Present.Bus.EveryEventHasASubscriber` and `AirportOps.Model.Bus.EveryEventDescribesItself` stay green.
- `Model/` in both plugins stays world-free and never includes `Build/ Tool/ Present/ Entities/ Content/` (rule 1). A comment stating a fact about other code carries `// ENFORCED BY:` naming a real test or rule (rule 12).
- No player saves exist (memory 2026-09-23): no save-compat shims; the PR notes the break.
- Build (worktree, editor may be open on the main checkout):
  `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-facility-upgrades\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE`
  Success = `Result: Succeeded`. **A task that adds a new `.cpp` builds TWICE** and checks the second run prints `Compile [x64] <NewFile>.cpp` (memory: the first run says Succeeded without compiling it). If it still does not, touch an existing `.cpp` in the same module and build again.
- Tests: `pwsh ./Tools/Run-AirsideTests.ps1 -Project "C:\repos\airportmgr2-facility-upgrades\AirportMgr.uproject" -Filter <Prefix>` (join prefixes with `+`). Read its `N test(s) run, N failed, N crashed` line; never trust the exit code. The script runs Check-Architecture first.
- Test names: leaves are distinct (`AirportOps.Model.Facility.BuyVehicleChargesAndAddsIdleFull`), never a bare parent beside dotted children. Never name a test local `TestWorld(...)` with arguments in AirsideTests/AirportOpsTests (rule 9 reads it as an assertion) - use `FAirsideTestWorld TestWorld;` or another name.
- Every `Test*` call names a reason first (`TEXT(...)` or `FString::Printf(...)`) - rule 9.

## Review Focus

1. **Existing maps' depots were saved with `Trucks = sheds`.** They keep that value and still get a starter fleet; only a depot drawn after this change starts empty. A starter fleet the player sells must not regrow after a save/load (SyncFleet re-seeds any depot missing from the transient `SeededDepots`). Pinned by `AirportOps.Model.Fleet.SoldStarterFleetIsNotReseededAfterLoad` (Task 2), fixed by saving `SeededDepots`. PIE verification must use a NEWLY DRAWN depot.
2. **The offer row's "Accept (no fuel)" stays stale after a purchase.** `FOfferVerdict` is cached on board/guideline/occupancy revisions only; a vehicle purchase moves none. Pinned by `AirportOps.Model.Fleet.OfferVerdictIsDatedByTheFleet` (Task 2); fixed by dating the verdict with `UJobBoard::GetFleetRevision()`.
3. **A refused job is terminal until the guideline revision moves** (`JobBoard.cpp:1029-1043`). Buying the first vehicle changes no guideline, so the aircraft would wait forever. Pinned by `AirportOps.Model.Fleet.PurchaseReopensRefusedJobs` (Task 2) and the composition `AirportOps.Present.Facility.PurchaseWakesTheBoard` (Task 5).
4. **The inspector re-asks the quote every tick**, and the quote's reserved-slot ceiling is a full plot solve. Pinned by `AirportOps.Present.Facility.QuoteSolvesOncePerDepot` (Task 7): the runtime memoises the ceiling per (network, depot).
5. **Selling or buying against state that moved since the card was drawn** (a vehicle dispatched between open and click, a depot deleted with its card open). Every command re-judges; pinned by `AirportOps.Model.Facility.RefusalsChargeAndPublishNothing` (VehicleBusy for a queued Idle vehicle; NotAFacility for a removed depot) in Task 4.

## Deviations from spec

(Recorded also as §6 of the spec - commit together.)

1. **Offers live on `UScenario`, not `UPlotModuleKit`** (spec §2). `UScenario`'s header says difficulty is its numbers; kits are `.uasset`s needing headless edits; AirportOps `Model/` may not include Airside `Content/`/`Entities/`. New `FModuleOffer` + `UScenario::ModuleOffers` (Shed only; Tank/Pump absent = not buyable). `FModuleOffer` and `FFuelVehicleSpec` also carry display names ("Shed"/"Sheds", "Utility tow", "Bowser") - the card's words are data, not code.
2. **`Trucks` is kept and re-meant, not retired** (spec §2 "Retired"). `FEntityInstance/FEntityPlacement/UEntityDefinition::Trucks` = the starter vehicles a placement came with; SyncFleet's placeholder seeding stays as the starter-fleet mechanism for plotless/test depots. `URoadNetwork::PlaceEntity` no longer derives it from sheds; the player's plotted depot passes 0. 36 files read it (mostly tests). `SeededDepots` becomes saved (Review Focus 1).
3. **`UFacilityPurchases` takes the `URoadNetwork` per call** instead of holding it (spec §3): `URoadEditFacade::ClearNetwork` and undo replace the network object, and `UJobBoard` already takes it per call for the same reason.
4. **No `UOpsEvents::OnFleetChanged` delegate.** Presentation is the bus's source log line plus a toast through `UOpsEvents::NotifyNotification`; a delegate nothing binds fails Check-Architecture rule 11a and the inspector polls the quote anyway.
5. **Daily upkeep is three described `Upkeep` entries** (base, "Facility upkeep", "Fleet upkeep") through a new line-list `ULedger::PostDailyUpkeep` overload; spec said two lines beside the existing base.
6. **New `EServiceRefusal::NoVehicles`** ("depot has no vehicles - buy one"): without it an empty depot fell through to `NoRoute` ("no road from depot").
7. **Buying a vehicle re-opens refused jobs**, and the offer verdict is dated by the fleet revision (Review Focus 2, 3).
8. **The three `selection.*` purchase rows are inspector-only** (`FBuildAction::bInspectorOnly`): not on the bar, no icon needed. Buy-vehicle and sell run as "choose/arm on the controller, then `TryRun` the row"; Sell needs a second click to confirm (destructive gesture - memory).
9. **Ghost reveal (R10) is driven from the controller's existing per-frame ghost gate**, reading the selection, not pushed by the inspector: `AAirsideBuildingsActor::ShowPlotGhosts(bVisible, Only)`; a change of `Only` rebuilds the plots once.
10. **Reserved-slot helper is `DepotKit::ReservationOf` in Airside `Build/`**, shared with `UPlotPresenter::RebuildFrom`, memoised per depot by `UOpsRuntime`. Hook signature carries the entity id: `int32(FEntityInstanceId, const FEntityInstance&, EDepotModule)`.
11. **Hover-highlight of the next slot is optional** (Task 10).

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsDefinition.h` | `FModuleOffer`, `UScenario::ModuleOffers`, vehicle prices on `FFuelVehicleSpec` | 1 |
| `Plugins/AirportOps/Source/AirportOps/Public/Model/Ledger.h`, `Private/Model/Ledger.cpp` | `ELedgerCategory::Fleet`, `CanPay`, `FUpkeepLine` overload | 1, 4, 6 |
| `Source/AirportMgr/LedgerViewModels.cpp` | word for `Fleet` | 1 |
| `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h`, `Public/Model/RoadNetwork.h`, `Private/Model/RoadNetwork.cpp`, `Public/Entities/EntityDefinition.h`, `Private/Entities/EntityDefinition.cpp`, `Private/Present/RoadEditFacadeSurfaces.cpp` | Trucks re-meant; player depot passes 0; `AddEntityModule` | 1, 3 |
| `Plugins/AirportOps/Source/AirportOps/Public/Model/ServiceJob.h`, `Private/Model/JobBoard.cpp`, `Private/Model/JobBoardBid.cpp`, `Public/Model/JobBoard.h` | `NoVehicles`; purchase add/remove; saved `SeededDepots`; `VehicleLine` | 1, 2 |
| `Plugins/AirportOps/Source/AirportOps/Public/Model/FlightBoard.h`, `Private/Model/FlightBoard.cpp` | verdict dated by fleet | 2 |
| `Plugins/Airside/Source/Airside/Public/Build/DepotKit.h`, `Private/Build/DepotKit.cpp` | `DepotKit::ReservationOf`, `RecoverFrontage` (moved) | 3 |
| `Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp`, `Public/Present/PlotPresenter.h` | uses the helper; ghost scope | 3, 9 |
| `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h`, `Private/Present/RoadEditFacade.cpp` | `AddEntityModule` door (rebuild + clear undo) | 3 |
| `Tools/Check-Architecture.ps1` | rule 4 row `PlotLayoutFor` | 3 |
| `Plugins/AirportOps/Source/AirportOps/Public/Model/FacilityPurchases.h`, `Private/Model/FacilityPurchases.cpp` (new) | the purchase service | 4, 6 |
| `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsEventBus.h`, `Private/Model/OpsEventBus.cpp` | two events | 5 |
| `Plugins/AirportOps/Source/AirportOps/Public/Present/OpsRuntime.h`, `Private/Present/OpsRuntime.cpp` | ownership, forwarders, WireBus, upkeep, hooks, memo | 5, 6, 7 |
| `Plugins/Airside/Source/Airside/Public/Present/AirsideBuildingsActor.h`, `Private/Present/AirsideBuildingsActor.cpp` | `ShowPlotGhosts` | 9 |
| `Source/AirportMgr/BuildActions.h/.cpp`, `BuildBarWidget.cpp`, `RoadBuildController.h/.cpp`, `InspectorWidget.h/.cpp` | UI | 8, 9 |
| Tests (new): `AirportOpsTests/Private/FacilityPurchasesTest.cpp`, `FleetPurchaseBoardTest.cpp`, `FacilityWiredTest.cpp`; `AirsideTests/Private/FacilityModuleTest.cpp` | | 1, 2, 3, 5 |
| Tests (edited): `AirsideTests/Private/PlotPlacementTest.cpp`, `PlotPresenterTest.cpp`; `AirportOpsTests/Private/FuelServiceTest.cpp`, `LedgerTest.cpp`; `Source/AirportMgr/BuildActionsTest.cpp`, `BuildBarWidgetTest.cpp`, `UIStyleTest.cpp`, `InspectorWidgetTest.cpp`, `PlayerTickGatesPlotGhostsTest.cpp` | | 1-9 |

---

### Task 1: Data - scenario offers, Fleet category, Trucks re-meant, NoVehicles refusal

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsDefinition.h:1-148`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/Ledger.h:21-34`
- Modify: `Source/AirportMgr/LedgerViewModels.cpp:19-32`
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp:1677-1703`
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadEntity.h:195-203, 295-309, 388-425`
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h:641-642`
- Modify: `Plugins/Airside/Source/Airside/Public/Entities/EntityDefinition.h:327-336`
- Modify: `Plugins/Airside/Source/Airside/Private/Entities/EntityDefinition.cpp:835-843`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacadeSurfaces.cpp:506-532` (PlaceEntityInPlot's FEntityPlacement)
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/ServiceJob.h:65-108` (EServiceRefusal)
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/JobBoard.h:209-216, 492-505`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/JobBoard.cpp:84-110` (RefusalText)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/JobBoardBid.cpp:67-196, 570-596`
- Create: `Plugins/AirportOps/Source/AirportOpsTests/Private/FacilityPurchasesTest.cpp`
- Modify: `Plugins/Airside/Source/AirsideTests/Private/PlotPlacementTest.cpp:79-130`
- Modify: `Plugins/AirportOps/Source/AirportOpsTests/Private/FuelServiceTest.cpp:112, 402-431` (+ one new test)

**Interfaces:**
- Produces: `struct FModuleOffer { FText DisplayName; FText PluralName; double Price; double UpkeepPerDay; int32 VehicleSlots; }`; `UScenario::ModuleOffers : TMap<EDepotModule, FModuleOffer>`; `FFuelVehicleSpec::{DisplayName, Price, UpkeepPerDay, ResaleFraction}` and ctor `FFuelVehicleSpec(double Capacity, double Flow, double Price, double UpkeepPerDay, FText DisplayName)`; `ELedgerCategory::Fleet`; `EServiceRefusal::NoVehicles`; `UJobBoard::FJudgement::FleetOnRoad`.

- [ ] **Step 1: Write the failing tests**

Create `Plugins/AirportOps/Source/AirportOpsTests/Private/FacilityPurchasesTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/OpsDefinition.h"
#include "Model/RoadEntity.h"

#if WITH_DEV_AUTOMATION_TESTS

// FACILITY UPGRADES AND FLEET PURCHASE (spec 2026-09-29-facility-upgrades-and-fleet-purchase).
// World-free: the scenario's catalogue, then (Task 4) UFacilityPurchases on NewObject fixtures.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityScenarioCatalogueTest, "AirportOps.Model.Facility.ScenarioPricesTheCatalogue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityScenarioCatalogueTest::RunTest(const FString&)
{
	// THE SPEC'S TABLE (§2), as data: a rebalance is an edit here and nowhere in code.
	const UScenario* Scenario = GetDefault<UScenario>();
	const FFuelVehicleSpec* Bowser = Scenario->FuelVehicles.Find(TEXT("FUEL"));
	const FFuelVehicleSpec* Tow = Scenario->FuelVehicles.Find(TEXT("UTILITY"));
	if (!TestNotNull(TEXT("the bowser row exists"), Bowser) || !TestNotNull(TEXT("the tow row exists"), Tow)) { return false; }
	TestEqual(TEXT("a bowser costs 90,000"), Bowser->Price, 90000.0, 1e-9);
	TestEqual(TEXT("and 500 a day to keep"), Bowser->UpkeepPerDay, 500.0, 1e-9);
	TestEqual(TEXT("and sells back for half"), Bowser->ResaleFraction, 0.5, 1e-9);
	TestEqual(TEXT("a utility tow costs 25,000"), Tow->Price, 25000.0, 1e-9);
	TestEqual(TEXT("and 150 a day to keep"), Tow->UpkeepPerDay, 150.0, 1e-9);
	TestEqual(TEXT("the capacities are unchanged by pricing"), Bowser->CapacityLitres, 10000.0, 1e-9);

	// ONLY THE SHED IS FOR SALE THIS SLICE (spec §1 out of scope: pumps, tanks).
	TestEqual(TEXT("one module offer"), Scenario->ModuleOffers.Num(), 1);
	const FModuleOffer* Shed = Scenario->ModuleOffers.Find(EDepotModule::Shed);
	if (!TestNotNull(TEXT("the shed is offered"), Shed)) { return false; }
	TestEqual(TEXT("a shed costs 40,000"), Shed->Price, 40000.0, 1e-9);
	TestEqual(TEXT("and 200 a day to keep"), Shed->UpkeepPerDay, 200.0, 1e-9);
	TestEqual(TEXT("and grants one vehicle bay (R2)"), Shed->VehicleSlots, 1);
	TestNull(TEXT("a tank is not for sale"), Scenario->ModuleOffers.Find(EDepotModule::Tank));
	TestNull(TEXT("nor a pump"), Scenario->ModuleOffers.Find(EDepotModule::Pump));
	return true;
}

#endif
```

In `Plugins/Airside/Source/AirsideTests/Private/PlotPlacementTest.cpp`, REPLACE the whole `FTrucksDerivedFromShedsTest` (lines 79-130) with:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStarterTrucksAreStatedTest,
	"Airside.Entities.StarterTrucksAreStated",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStarterTrucksAreStatedTest::RunTest(const FString& Parameters)
{
	// TRUCKS IS THE STARTER FLEET A PLACEMENT COMES WITH (facility-upgrades spec §6), never derived
	// from the sheds any more: sheds are BAYS the player fills by buying vehicles (R2, R3).
	URoadNetwork* Net = NewObject<URoadNetwork>();
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	if (!TestNotNull(TEXT("a depot definition"), Depot)) { return false; }

	auto PlaceWith = [&](const TArray<EDepotModule>& Modules, int32 Trucks, double X)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(X, 0.0);
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = ThreeBayPlot(X);
		Placement.Modules = Modules;
		Placement.Trucks = Trucks;
		return Net->GetEntity(Net->PlaceEntity(Placement));
	};

	{
		const FEntityInstance* TwoSheds = PlaceWith({ EDepotModule::Shed, EDepotModule::Shed, EDepotModule::Tank }, 0, 0.0);
		if (!TestNotNull(TEXT("two sheds placed"), TwoSheds)) { return false; }
		TestEqual(TEXT("two sheds stated with no starter trucks start with none - sheds are bays, not vehicles"), TwoSheds->Trucks, 0);
	}
	{
		const FEntityInstance* Stated = PlaceWith({ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, 2, 5000.0);
		if (!TestNotNull(TEXT("a plotted depot with a stated starter fleet places"), Stated)) { return false; }
		TestEqual(TEXT("keeps the count it was given, whatever its modules"), Stated->Trucks, 2);
	}
	{
		const FEntityInstance* Plain = Net->GetEntity(Net->PlaceEntity(
			Depot, Depot->Anchors, FVector2D(9000.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 3));
		if (!TestNotNull(TEXT("a plotless depot places"), Plain)) { return false; }
		TestEqual(TEXT("and a plotless caller keeps its count too"), Plain->Trucks, 3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayerDepotStartsWithNoTrucksTest,
	"Airside.Present.PlayerDepotStartsWithNoTrucks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlayerDepotStartsWithNoTrucksTest::RunTest(const FString& Parameters)
{
	// R3, AT THE GESTURE'S OWN DOOR: the depot the player draws has its kit and no vehicles, whatever
	// the definition's starter count says.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	TestEqual(TEXT("setup: the definition states a starter truck"), Actor->FuelDepotDefinition->Trucks, 1);

	const TArray<FVector2D> Plot = { FVector2D(0.0, 0.0), FVector2D(2000.0, 0.0), FVector2D(2000.0, 2400.0), FVector2D(0.0, 2400.0) };
	const int32 Index = Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1],
		{ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, EPlaceableEntity::FuelDepot);
	if (!TestNotEqual(TEXT("the plot is placed"), Index, int32(INDEX_NONE))) { return false; }
	const FEntityInstance& Placed = Actor->Network->GetEntities()[Index];
	TestEqual(TEXT("with no starter trucks (R3)"), Placed.Trucks, 0);
	TestEqual(TEXT("and its whole start kit"), Placed.Modules.Num(), 3);
	return true;
}
```

Add at the top of `PlotPlacementTest.cpp` (with the other includes) if absent: `#include "Present/RoadNetworkActor.h"`, `#include "Testing/AirsideTestWorld.h"`, `#include "Tool/RoadEditTarget.h"`.

In `Plugins/AirportOps/Source/AirportOpsTests/Private/FuelServiceTest.cpp`:
- In `struct FFuelFixture`, after `bool bDepotWithoutPump = false;` (line 112) add:

```cpp
		/** A depot placed with NO starter vehicles - the player's new depot (facility spec R3). Set before Build. */
		bool bEmptyDepot = false;
```
- In `FFuelFixture::Build`'s `bDepotWithoutPump` branch, after `Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank };` add:

```cpp
			// ITS STARTER TRUCK, STATED: sheds no longer imply vehicles (facility spec §6), and this case
			// is about the missing PUMP, so the depot must still have something to send.
			Placement.Trucks = DepotDef->Trucks;
```
- In the final `else` branch replace `UE_DOUBLE_PI * 0.5, 0.0, DepotDef->PoseRole, DepotDef->Trucks);` with `UE_DOUBLE_PI * 0.5, 0.0, DepotDef->PoseRole, bEmptyDepot ? 0 : DepotDef->Trucks);`
- Append before the final `#endif`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFuelEmptyDepotSaysNoVehiclesTest, "AirportOps.Fuel.EmptyDepotSaysNoVehicles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFuelEmptyDepotSaysNoVehiclesTest::RunTest(const FString& Parameters)
{
	// A DEPOT WITH NO VEHICLES (R3): not seeded, and the card names the fix. Before NoVehicles the chain
	// fell through to NoRoute - "no road from depot" about a depot sitting on a road with nothing in it.
	FFuelFixture Fixture;
	Fixture.bEmptyDepot = true;
	Fixture.Build(/*bWithRoad=*/true);
	Fixture.Advance(1.0 / 30.0);
	TestEqual(TEXT("a depot placed with no starter trucks is not seeded"), Fixture.Service->GetVehicles().Num(), 0);

	if (!TestTrue(TEXT("an aircraft parks"), Fixture.ParkAircraft() != 0)) { return false; }
	Fixture.Advance(0.2);
	if (!TestEqual(TEXT("one demand"), Fixture.Service->GetJobs().Num(), 1)) { return false; }
	TestEqual(TEXT("refused for the missing vehicle"),
		static_cast<int32>(Fixture.Service->GetJobs()[0].Why), static_cast<int32>(EServiceRefusal::NoVehicles));
	TestEqual(TEXT("and the card says what to do"),
		Fixture.Service->DescribeAgent(Fixture.Service->GetJobs()[0].AircraftId, 0.0),
		FString(TEXT("Fuel 300 L \u00B7 depot has no vehicles - buy one")));
	return true;
}
```

- [ ] **Step 2: Build twice and run to verify failure**

Run the Build command (Global Constraints) twice; the second run must show `Compile [x64] FacilityPurchasesTest.cpp`. Expected: compile errors (`ModuleOffers`, `Price`, `NoVehicles`, `FEntityPlacement::Trucks` read in the rewritten test compiles but the new fields do not). That is the red.

- [ ] **Step 3: Implement - scenario data**

In `OpsDefinition.h` add `#include "Model/RoadEntity.h"` after `#include "Engine/DataAsset.h"`. Replace `FFuelVehicleSpec` (lines 26-45) with:

```cpp
/**
 * What one KIND of fuel vehicle carries, how fast it pumps, and what it costs to own - keyed by
 * FVehicle::TypeCode.
 *
 * IN AirportOps, NOT ON FVehicle: Airside knows how a vehicle MOVES and must never learn what it
 * is FOR (UJobBoard's header). Spec 2026-09-28-fuel-litres section 1; prices from spec
 * 2026-09-29-facility-upgrades §2, where "a tanker later is a data row" (R4) is this struct.
 */
USTRUCT()
struct AIRPORTOPS_API FFuelVehicleSpec
{
	GENERATED_BODY()

	FFuelVehicleSpec() = default;
	FFuelVehicleSpec(double InCapacity, double InFlow) : CapacityLitres(InCapacity), FlowLitresPerMinute(InFlow) {}
	FFuelVehicleSpec(double InCapacity, double InFlow, double InPrice, double InUpkeep, FText InName)
		: CapacityLitres(InCapacity), FlowLitresPerMinute(InFlow), DisplayName(MoveTemp(InName)), Price(InPrice), UpkeepPerDay(InUpkeep) {}

	/** The vehicle's own tank - a load bigger than this takes more than one trip. */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "1.0")) double CapacityLitres = 1000.0;

	/** How fast it pumps into an aircraft, litres per GAME minute. */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "1.0")) double FlowLitresPerMinute = 75.0;

	/** What the depot card calls it - "Bowser". Empty falls back to the TypeCode. */
	UPROPERTY(EditAnywhere, Category = "Fleet") FText DisplayName;

	/** What buying one costs, charged to ELedgerCategory::Fleet. 0 is free, not "not for sale". */
	UPROPERTY(EditAnywhere, Category = "Fleet", meta = (ClampMin = "0.0")) double Price = 0.0;

	/** What owning one costs a day, whatever it does - part of the daily "Fleet upkeep" entry. */
	UPROPERTY(EditAnywhere, Category = "Fleet", meta = (ClampMin = "0.0")) double UpkeepPerDay = 0.0;

	/** The share of Price an idle one sells back for (R5). */
	UPROPERTY(EditAnywhere, Category = "Fleet", meta = (ClampMin = "0.0", ClampMax = "1.0")) double ResaleFraction = 0.5;
};

/**
 * One depot module the player can buy, and what it grants (spec 2026-09-29-facility-upgrades §2).
 *
 * ON UScenario, NOT ON UPlotModuleKit as the spec first said (§6 deviation 1): difficulty is the
 * scenario's numbers, a kit is a .uasset needing a headless edit per rebalance, and AirportOps'
 * Model/ may not include Airside's Content/. Rules code never names "shed": a module grants
 * VehicleSlots, and a second kind of bay later is a second row.
 */
USTRUCT()
struct AIRPORTOPS_API FModuleOffer
{
	GENERATED_BODY()

	FModuleOffer() = default;
	FModuleOffer(double InPrice, double InUpkeep, int32 InSlots, FText InName, FText InPlural)
		: DisplayName(MoveTemp(InName)), PluralName(MoveTemp(InPlural)), Price(InPrice), UpkeepPerDay(InUpkeep), VehicleSlots(InSlots) {}

	/** "Shed" - the buy button's word. */
	UPROPERTY(EditAnywhere, Category = "Facility") FText DisplayName;

	/** "Sheds" - the card's count line. Data, not an appended "s". */
	UPROPERTY(EditAnywhere, Category = "Facility") FText PluralName;

	/** Charged to ELedgerCategory::Placement - a module is building work. */
	UPROPERTY(EditAnywhere, Category = "Facility", meta = (ClampMin = "0.0")) double Price = 0.0;

	/** Per owned module per day, on every live depot - the daily "Facility upkeep" entry. */
	UPROPERTY(EditAnywhere, Category = "Facility", meta = (ClampMin = "0.0")) double UpkeepPerDay = 0.0;

	/** Vehicle bays each one grants (R2: a shed is one bay, any type). */
	UPROPERTY(EditAnywhere, Category = "Facility", meta = (ClampMin = "0")) int32 VehicleSlots = 0;
};
```

In `UScenario`, replace the `FuelVehicles` initializer (lines 130-132) with:

```cpp
	TMap<FName, FFuelVehicleSpec> FuelVehicles = {
		{ FName(TEXT("UTILITY")), FFuelVehicleSpec(1000.0, 75.0, 25000.0, 150.0, NSLOCTEXT("Scenario", "UtilityTow", "Utility tow")) },
		{ FName(TEXT("FUEL")), FFuelVehicleSpec(10000.0, 200.0, 90000.0, 500.0, NSLOCTEXT("Scenario", "Bowser", "Bowser")) } };
```

and extend that property's comment's last paragraph with: `PRICES (spec 2026-09-29-facility-upgrades §2): first guesses against a 500k opening balance and a ~15k full bowser load; unjudged in play.`

After `DepotRefillLitresPerMinutePerPump` add:

```cpp
	/**
	 * The depot modules the player can buy, and what each grants. Copied into UFacilityPurchases at
	 * attach. THE SHED ONLY this slice (spec 2026-09-29-facility-upgrades §1: pumps and tanks are out of
	 * scope) - a module with no row here is not for sale, and its buy is refused UnknownType.
	 */
	UPROPERTY(EditAnywhere, Category = "Facilities")
	TMap<EDepotModule, FModuleOffer> ModuleOffers = {
		{ EDepotModule::Shed, FModuleOffer(40000.0, 200.0, 1, NSLOCTEXT("Scenario", "Shed", "Shed"), NSLOCTEXT("Scenario", "Sheds", "Sheds")) } };
```

- [ ] **Step 4: Implement - ledger category**

In `Ledger.h`, after `BroughtForward` (line 33) add (APPENDED, so a saved entry's value keeps its meaning):

```cpp
	BroughtForward,
	/** A vehicle bought (negative) or sold (positive) - UFacilityPurchases. Appended, not inserted. */
	Fleet
```

(remove the old trailing `BroughtForward` line so the enum lists it once). In `Source/AirportMgr/LedgerViewModels.cpp` `WordFor`, add after the `BroughtForward` case:

```cpp
		case ELedgerCategory::Fleet:          return LOCTEXT("CatFleet", "Fleet");
```

- [ ] **Step 5: Implement - Trucks re-meant (Airside)**

In `RoadNetwork.cpp` replace lines 1677-1703 (the `// DERIVED FROM THE SHEDS` comment through the `else { Instance.Trucks = Placement.Trucks; }` block) with:

```cpp
	// THE STARTER FLEET, AS STATED - never derived from the sheds since 2026-09-29 (facility-upgrades
	// spec §6): a shed is a BAY the player fills by buying a vehicle (R2), so a count derived from the
	// sheds would hand out the vehicles the player is meant to buy. The player's plotted depot states 0
	// (URoadEditFacade::PlaceEntityInPlot, R3); plotless callers state their definition's count, which
	// UJobBoard's placeholder seeding turns into vehicles once.
	// ENFORCED BY: Airside.Entities.StarterTrucksAreStated, Airside.Present.PlayerDepotStartsWithNoTrucks
	Instance.Trucks = Placement.Trucks;
```

In `RoadEntity.h`:
- EDepotModule comment, replace the `Shed - the truck count. LIVE: ...` line with:
  ` *   Shed - a vehicle bay. LIVE: UFacilityPurchases counts bays from the offers' VehicleSlots (facility spec §2).`
- Replace the `FEntityInstance::Trucks` doc comment (the block ending at line 309) with:

```cpp
	/**
	 * The STARTER vehicles this installation came with - of each kind in UJobBoard::FleetTypes -
	 * turned into real vehicles once by UJobBoard's placeholder seeding. 0 on a stand, and 0 on a
	 * depot the player drew (facility-upgrades spec R3): the player BUYS its fleet. Kept, rather than
	 * retired as that spec first said (§6), because plotless and test depots still start with one.
	 *
	 * CAPTURED at placement for the Model/-must-not-see-Entities/ reason DesignWingspan is - and read
	 * from ANOTHER MODULE's Model/ (AirportOps' UJobBoard), which Check-Architecture also forbids from
	 * including Entities/.
	 */
	UPROPERTY() int32 Trucks = 0;
```
- In the `FEntityPlacement` struct comment replace the `TRUCKS IS STILL HERE ...` paragraph with:

```cpp
 * TRUCKS IS THE STARTER FLEET, stated by the caller and never derived from Modules (facility-upgrades
 * spec §6): the player's plotted depot states 0 and buys its vehicles; a plotless caller states its
 * definition's count.
```
  and the field comment `/** Only read when Modules is empty. See the struct comment. */` becomes `/** Starter vehicles - see FEntityInstance::Trucks. */`.

In `RoadNetwork.h` line 641-642 replace `Trucks is the third and last such capture - see FEntityInstance::Trucks for why a fourth would become a struct instead.` with `Trucks is the starter fleet - see FEntityInstance::Trucks.`

In `EntityDefinition.h` replace the `Trucks` comment (lines 327-335) with:

```cpp
	/**
	 * The STARTER vehicles of each kind a PLOTLESS placement of this definition comes with - see
	 * FEntityInstance::Trucks. A depot the player draws ignores it and starts empty (facility spec R3).
	 * 0 on a stand, where it means nothing.
	 */
```

In `EntityDefinition.cpp` replace the comment block above `Definition->Trucks = 1;` (lines 835-842) with:

```cpp
	// ONE STARTER TRUCK OF EACH KIND for a PLOTLESS placement - every pre-plot caller and the fuel
	// tests. A depot the player draws ignores it and starts with none (facility-upgrades spec R3,
	// URoadEditFacade::PlaceEntityInPlot). Zeroing this silently gave every plotless caller a depot
	// that could not dispatch once already; six fuel tests said so.
```

In `RoadEditFacadeSurfaces.cpp` `PlaceEntityInPlot`, directly after `Placement.Modules = Modules;` add:

```cpp
	// NO STARTER VEHICLES (facility-upgrades spec R3): the depot the player draws has its kit and an
	// empty yard; the fleet is bought from its inspector card. Stated rather than left to the default,
	// so the rule is visible where the placement is built.
	// ENFORCED BY: Airside.Present.PlayerDepotStartsWithNoTrucks
	Placement.Trucks = 0;
```

- [ ] **Step 6: Implement - NoVehicles refusal (AirportOps)**

`ServiceJob.h`: after `VehicleTooLarge` (keep its comment) append:

```cpp
	VehicleTooLarge,

	/**
	 * Depots are on a road but not one of them has a vehicle (facility-upgrades spec R3: a new depot
	 * starts empty). ITS OWN REFUSAL AND NOT NoRoute: "no road from depot" about an empty depot on a
	 * road sends the player to fix a road that is fine. APPENDED so no other value moves.
	 * ENFORCED BY: AirportOps.Fuel.EmptyDepotSaysNoVehicles
	 */
	NoVehicles
```

(remove the old bare `VehicleTooLarge` line so it appears once).

`JobBoard.cpp` `RefusalText`, add before `default:`:

```cpp
	// THE FIX, NAMED: the depot card has the buy button (facility-upgrades spec §4).
	case EServiceRefusal::NoVehicles:    return TEXT("depot has no vehicles - buy one");
```

`JobBoard.h` `FJudgement`: after `int32 DepotsOnRoad = 0;` add:

```cpp
		/** Real vehicles whose home depot is alive and on a road - NoVehicles when zero. */
		int32 FleetOnRoad = 0;
```

`JobBoardBid.cpp` `Judge`: after the depot-count loop (after `Out.DepotsOnRoad += ...` loop closes, ~line 90) add:

```cpp
	// THE FLEET COUNTS TOO, off the board's own vehicles and not the candidates: a stranded vehicle is
	// left out of the bidding (AssignOpenJobs) but is still a vehicle, and its job must keep saying what
	// it said before - only a depot with NOTHING in it is NoVehicles.
	for (const FServiceVehicle& Vehicle : Vehicles)
	{
		const FEntityInstance* Home = Network.GetEntity(Vehicle.Home);
		Out.FleetOnRoad += (Vehicle.Role == Role && Home != nullptr && Home->bAlive && Network.IsDepotJoined(*Home)) ? 1 : 0;
	}
```

`RefusalOf`: after the `NoRoad` return add:

```cpp
	if (Out.FleetOnRoad == 0)
	{
		// A DEPOT ON A ROAD WITH NOTHING IN IT: the next thing in the player's hand is the buy button on
		// its card, before anything about the stand or the graph.
		return EServiceRefusal::NoVehicles;
	}
```

`CouldServe` comment (lines 572-573) becomes:

```cpp
	// THE CANDIDATES A DEPOT HAS OR WILL HAVE: its vehicles once SyncFleet has seeded it, else the
	// STARTER fleet its Trucks would give it - an offer can be asked before the first tick. A depot the
	// player drew has Trucks 0 and, until a vehicle is bought, no candidate: its offers say "no fuel".
```

`JobBoard.h` `FleetTypes` comment (lines 209-215) becomes:

```cpp
	/**
	 * The kinds of vehicle a STARTER fleet gives a depot (spec §3.4): DefaultFleetTypes if set, else every
	 * distinct TypeCode in the letter table. A depot with Trucks = N gets N of each, once; the player's
	 * depot has Trucks 0 and buys its fleet instead (UFacilityPurchases, facility-upgrades spec).
	 */
```

- [ ] **Step 7: Build and run the tests**

Build (once is enough; the new file was picked up in Step 2). Run:
`pwsh ./Tools/Run-AirsideTests.ps1 -Project "C:\repos\airportmgr2-facility-upgrades\AirportMgr.uproject" -Filter "AirportOps.Model.Facility+Airside.Entities+Airside.Present.PlayerDepotStartsWithNoTrucks+AirportOps.Fuel"`
Expected: all pass. Then the full suite (no `-Filter`); expected `0 failed, 0 crashed`. Any test that now reads `NoVehicles` where it expected `NoRoute`: inspect it - if its depot truly has no vehicle, update the expectation to `NoVehicles` with a comment; otherwise the fixture needs `Trucks` stated.

- [ ] **Step 8: Commit**

```bash
git add -A Plugins/AirportOps Plugins/Airside Source/AirportMgr/LedgerViewModels.cpp
git commit -m "feat(facility): scenario prices, Fleet ledger category, Trucks = starter fleet, NoVehicles refusal"
```

---

### Task 2: UJobBoard - buy and remove vehicles; saved seeding; verdict dated by fleet

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/JobBoard.h:98-107, 326-332, 371-389, 594-596`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/JobBoard.cpp:48-82, 333-347, 1170-1188`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/FlightBoard.h:24-48`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/FlightBoard.cpp:120-140`
- Create: `Plugins/AirportOps/Source/AirportOpsTests/Private/FleetPurchaseBoardTest.cpp`

**Interfaces:**
- Consumes: `FFuelVehicleSpec` (Task 1).
- Produces (public on `UJobBoard`): `int32 AddPurchasedVehicle(FName TypeCode, FEntityInstanceId Home)` (0 on refusal); `bool CanRemoveVehicle(int32 VehicleId) const`; `bool RemoveVehicle(int32 VehicleId)`; `int32 VehiclesAt(FEntityInstanceId Depot) const`; `uint32 GetFleetRevision() const`; `FString VehicleLine(const FServiceVehicle& Vehicle) const`. `FOfferVerdict::FleetAt`.

- [ ] **Step 1: Write the failing tests**

Create `Plugins/AirportOps/Source/AirportOpsTests/Private/FleetPurchaseBoardTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/OpsDefinition.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"

#if WITH_DEV_AUTOMATION_TESTS

// UJobBoard'S HALF OF FLEET PURCHASE (facility-upgrades spec §3): the board owns Vehicles, so a bought
// vehicle enters and a sold one leaves only through these two doors.

namespace
{
	/** A board with the scenario's figures and a depot id to be home. Prefixed: unity build. */
	UJobBoard* FleetBoardWithSpecs()
	{
		UJobBoard* Board = NewObject<UJobBoard>(GetTransientPackage());
		Board->VehicleSpecs = GetDefault<UScenario>()->FuelVehicles;
		return Board;
	}

	FEntityInstanceId FleetTestDepotId()
	{
		FEntityInstanceId Id;
		Id.Index = 2;
		Id.Generation = 1;
		return Id;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetPurchasedIdleFullTest, "AirportOps.Model.Fleet.PurchasedVehicleIsIdleAndFull",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetPurchasedIdleFullTest::RunTest(const FString&)
{
	UJobBoard* Board = FleetBoardWithSpecs();
	const uint32 Before = Board->GetFleetRevision();
	const int32 Id = Board->AddPurchasedVehicle(TEXT("FUEL"), FleetTestDepotId());
	if (!TestTrue(TEXT("a purchase issues a vehicle id"), Id != 0)) { return false; }
	const FServiceVehicle* Vehicle = Board->FindVehicle(Id);
	if (!TestNotNull(TEXT("and the vehicle is on the board"), Vehicle)) { return false; }
	TestEqual(TEXT("idle at home - a vehicle at home is not on the road"),
		static_cast<int32>(Vehicle->State), static_cast<int32>(EServiceVehicleState::Idle));
	TestTrue(TEXT("at the depot it was bought for"), Vehicle->Home == FleetTestDepotId());
	TestEqual(TEXT("delivered full (spec §3)"), Vehicle->Cargo, 10000.0, 1e-9);
	TestEqual(TEXT("with no agent"), Vehicle->AgentId, 0);
	TestTrue(TEXT("the fleet revision moves, so a re-bid sees it"), Board->GetFleetRevision() != Before);
	TestEqual(TEXT("counted at its depot"), Board->VehiclesAt(FleetTestDepotId()), 1);
	TestEqual(TEXT("an unset home is refused"), Board->AddPurchasedVehicle(TEXT("FUEL"), FEntityInstanceId()), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetPurchaseReopensTest, "AirportOps.Model.Fleet.PurchaseReopensRefusedJobs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetPurchaseReopensTest::RunTest(const FString&)
{
	// A REFUSED JOB IS TERMINAL until the guideline revision moves - and buying the first vehicle moves
	// no guideline. Without this the aircraft that was refused NoVehicles waits out its turnaround.
	UJobBoard* Board = FleetBoardWithSpecs();
	FServiceJob& Job = Board->AddJobForTest(41, EServiceJobState::Unserviceable, EServiceRefusal::NoVehicles, 0);
	const int32 JobId = Job.Id;
	Board->AddPurchasedVehicle(TEXT("FUEL"), FleetTestDepotId());
	const FServiceJob* After = Board->GetJobs().FindByPredicate([JobId](const FServiceJob& J) { return J.Id == JobId; });
	if (!TestNotNull(TEXT("the job is still on the board"), After)) { return false; }
	TestEqual(TEXT("and open again, for the next pass to bid"),
		static_cast<int32>(After->State), static_cast<int32>(EServiceJobState::Open));
	TestEqual(TEXT("with its old reason cleared"), static_cast<int32>(After->Why), static_cast<int32>(EServiceRefusal::None));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetOnlyIdleLeavesTest, "AirportOps.Model.Fleet.OnlyAnIdleVehicleLeaves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetOnlyIdleLeavesTest::RunTest(const FString&)
{
	// R5: SOLD ONLY WHEN PARKED with nothing promised. A queued job is a promise another bid was judged
	// against; a vehicle out has an agent on the road.
	UJobBoard* Board = FleetBoardWithSpecs();
	const int32 Out = Board->AddVehicleForTest(TEXT("FUEL"), FleetTestDepotId(), EServiceVehicleState::ToJob, 0.0).Id;
	FServiceVehicle& Promised = Board->AddVehicleForTest(TEXT("FUEL"), FleetTestDepotId(), EServiceVehicleState::Idle, 0.0);
	Promised.Queue = { 99 };
	const int32 PromisedId = Promised.Id;
	const int32 Free = Board->AddVehicleForTest(TEXT("UTILITY"), FleetTestDepotId(), EServiceVehicleState::Idle, 0.0).Id;

	TestFalse(TEXT("a vehicle on its way to a job cannot leave"), Board->RemoveVehicle(Out));
	TestFalse(TEXT("nor an idle one with a queued job"), Board->RemoveVehicle(PromisedId));
	TestFalse(TEXT("and CanRemoveVehicle agrees with RemoveVehicle"), Board->CanRemoveVehicle(PromisedId));
	TestTrue(TEXT("an idle one with nothing queued can be asked"), Board->CanRemoveVehicle(Free));
	const uint32 Before = Board->GetFleetRevision();
	TestTrue(TEXT("and removed"), Board->RemoveVehicle(Free));
	TestNull(TEXT("it is gone from the board"), Board->FindVehicle(Free));
	TestTrue(TEXT("the fleet revision moves"), Board->GetFleetRevision() != Before);
	TestFalse(TEXT("an unknown id removes nothing"), Board->RemoveVehicle(12345));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetSoldStaysSoldTest, "AirportOps.Model.Fleet.SoldStarterFleetIsNotReseededAfterLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetSoldStaysSoldTest::RunTest(const FString&)
{
	// REVIEW FOCUS 1: a starter depot whose fleet the player sold must not regrow after a load. The
	// placeholder seeds any depot missing from SeededDepots, which used to be Transient and so empty
	// after every load.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UJobBoard* Board = FleetBoardWithSpecs();
	Board->DefaultFleetTypes = { TEXT("FUEL") };
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	Net->PlaceEntity(Depot, Depot->Anchors, FVector2D(0.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 1);

	Board->Tick(*Traffic, *Net, *Clock);
	if (!TestEqual(TEXT("setup: the starter truck is seeded"), Board->GetVehicles().Num(), 1)) { return false; }
	TestTrue(TEXT("setup: and sold"), Board->RemoveVehicle(Board->GetVehicles()[0].Id));

	FOpsSnapshot Snapshot;
	OpsSave::CaptureBlob(*Board, Snapshot);
	OpsSave::RestoreBlob(Snapshot, *Board);
	Board->Tick(*Traffic, *Net, *Clock);
	TestEqual(TEXT("the load does not seed the sold-out depot again"), Board->GetVehicles().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFleetVerdictDatedTest, "AirportOps.Model.Fleet.OfferVerdictIsDatedByTheFleet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFleetVerdictDatedTest::RunTest(const FString&)
{
	// REVIEW FOCUS 2: the offer row's "Accept (no fuel)" is cached; a purchase must re-date it or the
	// row goes on saying no fuel after the bowser arrived.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UFlightBoard* Flights = NewObject<UFlightBoard>(GetTransientPackage());
	Flights->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
	UJobBoard* Board = FleetBoardWithSpecs();
	Flights->Fuel = Board;
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->OfferWindowSeconds = 60.0;
	Flight->OfferSecondsLeft = 60.0;
	Flights->AddOffer(*Clock, Flight);

	const uint32 FirstAt = Flights->VerdictFor(*Traffic, *Net, *Flight).FleetAt;
	TestEqual(TEXT("the verdict is dated by the fleet it was judged against"), FirstAt, Board->GetFleetRevision());
	Board->AddPurchasedVehicle(TEXT("FUEL"), FleetTestDepotId());
	TestEqual(TEXT("a purchase re-dates it on the next ask"),
		Flights->VerdictFor(*Traffic, *Net, *Flight).FleetAt, Board->GetFleetRevision());
	TestTrue(TEXT("which is a different date"), FirstAt != Board->GetFleetRevision());
	return true;
}

#endif
```

- [ ] **Step 2: Build twice; verify red**

Build twice (new file). Expected: compile errors for `AddPurchasedVehicle`, `GetFleetRevision`, `RemoveVehicle`, `CanRemoveVehicle`, `VehiclesAt`, `FleetAt`.

- [ ] **Step 3: Implement the board methods**

`JobBoard.h` - after `DescribeDepot` (line 326) add:

```cpp
	/**
	 * THE DOOR A BOUGHT VEHICLE ENTERS BY (facility-upgrades spec §3): Idle at Home and full, its row
	 * resolved through TypeFor like a seeded one. Bumps FleetRevision and RE-OPENS every refused job of its
	 * role - a refused job is terminal until something changes, and a vehicle that did not exist is the
	 * change (see the re-offer pass in Step, which watches the guideline revision only). Returns the new
	 * id, or 0 (logged) for an unset Home or a None type. Money and bays are UFacilityPurchases', not this.
	 * ENFORCED BY: AirportOps.Model.Fleet.PurchasedVehicleIsIdleAndFull, .PurchaseReopensRefusedJobs
	 */
	int32 AddPurchasedVehicle(FName TypeCode, FEntityInstanceId Home);

	/** True when VehicleId is Idle, has no agent, no current job and an empty queue - the only vehicle
	 *  that may leave (R5). RemoveVehicle asks exactly this. */
	bool CanRemoveVehicle(int32 VehicleId) const;

	/**
	 * THE DOOR A SOLD VEHICLE LEAVES BY. False, nothing changed, unless CanRemoveVehicle. Bumps
	 * FleetRevision; leaves SeededDepots alone, so a sold starter fleet stays sold.
	 * ENFORCED BY: AirportOps.Model.Fleet.OnlyAnIdleVehicleLeaves
	 */
	bool RemoveVehicle(int32 VehicleId);

	/** Vehicles whose Home is Depot - counted off the vehicles, never stored on the depot. */
	int32 VehiclesAt(FEntityInstanceId Depot) const;

	/** "FUEL #7 · at depot 1 · 10,000 L" - the one line the depot card's backlog and its fleet rows share. */
	FString VehicleLine(const FServiceVehicle& Vehicle) const;
```

Replace `uint32 GetFleetRevisionForTest() const { return FleetRevision; }` (line 389) with:

```cpp
	/** See FleetRevision. Public since the offer verdict is dated by it (UFlightBoard::VerdictFor). */
	uint32 GetFleetRevision() const { return FleetRevision; }
	uint32 GetFleetRevisionForTest() const { return GetFleetRevision(); }
```

Replace the `SeededDepots` declaration (lines 594-596) with:

```cpp
	/**
	 * Depots whose STARTER fleet has been seeded, so a depot is seeded once and a vehicle that is out
	 * does not get a twin at home. SAVED since 2026-09-29 (facility-upgrades spec): a starter fleet the
	 * player sold must stay sold across a load, and a transient set came back empty and re-seeded it.
	 * ENFORCED BY: AirportOps.Model.Fleet.SoldStarterFleetIsNotReseededAfterLoad
	 */
	UPROPERTY() TSet<FEntityInstanceId> SeededDepots;
```

In the `OnBeforeRestore` header comment (lines 98-106) replace `The vehicles come back from the placeholder fleet on the next tick (SyncFleet), Idle at home, which is exactly where a truck that was out at save time should reappear.` with `The vehicles come back from the Fuel blob (Serialize normalises them Idle at home), and SeededDepots with them.`

`JobBoard.cpp` - in `OnBeforeRestore` replace the comment above `SeededDepots.Reset();` with `// CLEARED, THEN RESTORED FROM THE BLOB (it is saved): a snapshot without it re-seeds each depot once, which is the old behaviour.`

Add after `AddVehicleForTest` (after line 347):

```cpp
int32 UJobBoard::AddPurchasedVehicle(FName TypeCode, FEntityInstanceId Home)
{
	if (!Home.IsSet() || TypeCode.IsNone())
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Fleet: purchase of '%s' for depot %d refused - no depot or no type"),
			*TypeCode.ToString(), Home.Index);
		return 0;
	}
	const FServiceVehicleType Type = TypeFor(TypeCode);
	FServiceVehicle& Vehicle = Vehicles.AddDefaulted_GetRef();
	Vehicle.Id = NextVehicleId++;
	Vehicle.TypeCode = TypeCode;
	Vehicle.Role = Type.Role;
	Vehicle.Home = Home;
	Vehicle.State = EServiceVehicleState::Idle;
	Vehicle.Cargo = FFuelRolePolicy::CapacityOf(Type);
	const int32 Id = Vehicle.Id;
	++FleetRevision;

	// A NEW VEHICLE IS A CHANGE A REFUSED JOB CAN ANSWER DIFFERENTLY - see the header. Re-opened, not bid
	// here: the next Step bids it, in its one sequence (the bus's FleetChanged wakes that pass).
	int32 Reopened = 0;
	for (FServiceJob& Job : Jobs)
	{
		if (Job.State == EServiceJobState::Unserviceable && Job.Role == Type.Role)
		{
			Job.State = EServiceJobState::Open;
			Job.Why = EServiceRefusal::None;
			++Reopened;
		}
	}
	UE_LOG(LogAirportOps, Log, TEXT("Fleet: depot %d gains bought vehicle %d %s (%.0f L at %.0f L/min); %d refused job(s) ask again"),
		Home.Index, Id, *TypeCode.ToString(), FFuelRolePolicy::CapacityOf(Type), Type.RatePerMinute, Reopened);
	return Id;
}

bool UJobBoard::CanRemoveVehicle(int32 VehicleId) const
{
	const FServiceVehicle* Vehicle = FindVehicle(VehicleId);
	return Vehicle != nullptr && Vehicle->State == EServiceVehicleState::Idle && Vehicle->AgentId == 0
		&& Vehicle->CurrentJob == 0 && Vehicle->Queue.Num() == 0;
}

bool UJobBoard::RemoveVehicle(int32 VehicleId)
{
	if (!CanRemoveVehicle(VehicleId))
	{
		return false;
	}
	const int32 Index = Vehicles.IndexOfByPredicate([VehicleId](const FServiceVehicle& V) { return V.Id == VehicleId; });
	UE_LOG(LogAirportOps, Log, TEXT("Fleet: vehicle %d %s leaves depot %d"),
		VehicleId, *Vehicles[Index].TypeCode.ToString(), Vehicles[Index].Home.Index);
	Vehicles.RemoveAt(Index);
	++FleetRevision;
	return true;
}

int32 UJobBoard::VehiclesAt(FEntityInstanceId Depot) const
{
	int32 Count = 0;
	for (const FServiceVehicle& Vehicle : Vehicles)
	{
		Count += Vehicle.Home == Depot ? 1 : 0;
	}
	return Count;
}

FString UJobBoard::VehicleLine(const FServiceVehicle& Vehicle) const
{
	const FString Dot = TEXT(" · ");
	return FString::Printf(TEXT("%s #%d"), *Vehicle.TypeCode.ToString(), Vehicle.Id) + Dot + VehicleDoing(Vehicle)
		+ Dot + FText::AsNumber(FMath::RoundToInt(Vehicle.Cargo)).ToString() + TEXT(" L");
}
```

In `DescribeDepot`, replace the two-line `Lines.Add(FString::Printf(TEXT("%s #%d"), ... + Dot + Litres(Vehicle.Cargo));` with `Lines.Add(VehicleLine(Vehicle));` (same text - the shared line).

- [ ] **Step 4: Implement the verdict date**

`FlightBoard.h` `FOfferVerdict`: after `uint32 OccupancyAt = 0;` add:

```cpp
	/** UJobBoard::GetFleetRevision when judged - a vehicle bought or sold changes bFuelServable
	 *  (facility-upgrades spec). ENFORCED BY: AirportOps.Model.Fleet.OfferVerdictIsDatedByTheFleet */
	uint32 FleetAt = 0;
```

and in its comment change `CACHED ON THREE REVISIONS` to `CACHED ON FOUR REVISIONS` and add `, or the fleet (UJobBoard::GetFleetRevision - a vehicle bought or sold)` after the occupancy clause.

`FlightBoard.cpp` `VerdictFor`: after `const uint32 OccupancyNow = Traffic.OccupancyRevision();` add `const uint32 FleetNow = Fuel != nullptr ? Fuel->GetFleetRevision() : 0;`; add `|| Verdict.FleetAt != FleetNow` to the `if`; and `Verdict.FleetAt = FleetNow;` beside `Verdict.OccupancyAt = OccupancyNow;`.

- [ ] **Step 5: Build and test**

Build. Run `-Filter "AirportOps.Model.Fleet+AirportOps.Fuel+AirportOps.Model.Save+AirportOps.Model.Offers"`. Expected: all pass (the existing `AirportOps.Fuel.RestoredFleetIsNotReseeded` stays green - Serialize still adds restored homes).

- [ ] **Step 6: Commit**

```bash
git add -A Plugins/AirportOps
git commit -m "feat(fleet): job board buy/remove doors, saved starter seeding, verdict dated by fleet"
```

---

### Task 3: Airside - reserved-slot helper, network module mutator, facade door (rebuild + clear undo)

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Build/DepotKit.h:67-93`
- Modify: `Plugins/Airside/Source/Airside/Private/Build/DepotKit.cpp` (append)
- Modify: `Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp:217-257, 590-640`
- Modify: `Plugins/Airside/Source/Airside/Public/Model/RoadNetwork.h:828` (after SetEntityPoseRole)
- Modify: `Plugins/Airside/Source/Airside/Private/Model/RoadNetwork.cpp:2024` (after SetEntityPoseRole)
- Modify: `Plugins/Airside/Source/Airside/Public/Present/RoadEditFacade.h:214` (after SetDriveSide)
- Modify: `Plugins/Airside/Source/Airside/Private/Present/RoadEditFacade.cpp:1185` (after SetDriveSide)
- Modify: `Tools/Check-Architecture.ps1:~411` (rule 4 table, after the `DepotKitSpecs` row)
- Create: `Plugins/Airside/Source/AirsideTests/Private/FacilityModuleTest.cpp`

**Interfaces:**
- Produces: `DepotKit::ReservationOf(const FEntityInstance&, TArrayView<const PlotYard::FKitSpec>) -> TOptional<PlotYard::FReservation>`; `DepotKit::RecoverFrontage(const FEntityInstance&, FVector2D&, FVector2D&) -> bool`; `URoadNetwork::AddEntityModule(FEntityInstanceId, EDepotModule) -> bool`; `URoadEditFacade::AddEntityModule(FEntityInstanceId, EDepotModule) -> bool`.

- [ ] **Step 1: Write the failing tests**

Create `Plugins/Airside/Source/AirsideTests/Private/FacilityModuleTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Build/DepotKit.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadEditTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

// AIRSIDE'S HALF OF A SHED PURCHASE (facility-upgrades spec §3): the mutator, the one solve that says
// how many slots a placed plot reserves, and the facade door that relights the yard and clears undo.

namespace
{
	/** 50 x 24 m: room for more than the start kit's one shed under the band layout (memory: the 12 x 8 m
	 *  plot seats nothing). Prefixed: unity build. */
	TArray<FVector2D> FacilityModuleWidePlot()
	{
		return { FVector2D(0.0, 0.0), FVector2D(5000.0, 0.0), FVector2D(5000.0, 2400.0), FVector2D(0.0, 2400.0) };
	}

	FEntityPlacement FacilityModulePlacement(UEntityDefinition* Depot, const TArray<FVector2D>& Plot)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = (Plot[0] + Plot[1]) * 0.5;
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = Plot;
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		return Placement;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEntityModulesAddTest, "Airside.Model.EntityModules.AddAppendsToADepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FEntityModulesAddTest::RunTest(const FString&)
{
	URoadNetwork* Net = NewObject<URoadNetwork>();
	const TArray<FVector2D> Plot = FacilityModuleWidePlot();
	const FEntityInstanceId Depot = Net->PlaceEntity(FacilityModulePlacement(UEntityDefinition::MakeFuelDepotTransient(), Plot));
	TestTrue(TEXT("a shed is added to a live depot"), Net->AddEntityModule(Depot, EDepotModule::Shed));
	const FEntityInstance* After = Net->GetEntity(Depot);
	if (!TestNotNull(TEXT("the depot is still there"), After)) { return false; }
	TestEqual(TEXT("four modules now"), After->Modules.Num(), 4);
	TestEqual(TEXT("APPENDED - bay order is the fill order the presenter lights by"),
		static_cast<int32>(After->Modules.Last()), static_cast<int32>(EDepotModule::Shed));

	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(20000.0, 0.0), 0.0, 0.0, StandDef->PoseRole, 0);
	TestFalse(TEXT("a stand has no bays to add to"), Net->AddEntityModule(Stand, EDepotModule::Shed));
	TestFalse(TEXT("an unset handle takes nothing"), Net->AddEntityModule(FEntityInstanceId(), EDepotModule::Shed));
	Net->RemoveEntity(Depot);
	TestFalse(TEXT("a removed depot takes nothing"), Net->AddEntityModule(Depot, EDepotModule::Shed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDepotKitReservationOfTest, "Airside.Build.DepotKit.ReservationOfIsThePresentersSolve",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FDepotKitReservationOfTest::RunTest(const FString&)
{
	// ONE SOLVE: what the purchase rules call a free slot is exactly what the presenter draws ghosted.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	const TArray<FVector2D> Plot = FacilityModuleWidePlot();
	const FEntityInstanceId Depot = Actor->Network->PlaceEntity(FacilityModulePlacement(UEntityDefinition::MakeFuelDepotTransient(), Plot));
	Actor->RebuildMesh();

	const TOptional<PlotYard::FReservation> Reserved = DepotKit::ReservationOf(*Actor->Network->GetEntity(Depot), Actor->ResolveDepotKits());
	if (!TestTrue(TEXT("a plotted depot has a reservation"), Reserved.IsSet())) { return false; }
	int32 Bays = 0;
	for (const PlotYard::FReservedStand& Stand : Reserved->Stands) { Bays += Stand.RunLength; }
	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	TestEqual(TEXT("its bays are every bay the presenter drew, lit or ghosted"), Bays, Plots->GetModuleCount() + Plots->GetGhostCount());

	UEntityDefinition* Plain = UEntityDefinition::MakeFuelDepotTransient();
	const FEntityInstanceId Plotless = Actor->Network->PlaceEntity(Plain, Plain->Anchors, FVector2D(30000.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 1);
	TestFalse(TEXT("a plotless depot reserves nothing - there is no ground to solve"),
		DepotKit::ReservationOf(*Actor->Network->GetEntity(Plotless), Actor->ResolveDepotKits()).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityModuleFacadeTest, "Airside.Present.Facility.ModulePurchaseRelightsAndClearsUndo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityModuleFacadeTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
	const TArray<FVector2D> Plot = FacilityModuleWidePlot();
	const int32 Index = Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1],
		{ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, EPlaceableEntity::FuelDepot);
	if (!TestNotEqual(TEXT("setup: the depot is placed"), Index, int32(INDEX_NONE))) { return false; }
	const FEntityInstanceId Depot = Actor->Network->EntityIdAt(Index);
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestTrue(TEXT("setup: placing it is an undo step"), Facade->CanUndo())) { return false; }
	const TOptional<PlotYard::FReservation> Reserved = DepotKit::ReservationOf(*Actor->Network->GetEntity(Depot), Actor->ResolveDepotKits());
	if (!TestTrue(TEXT("setup: the plot reserves a second shed"),
		Reserved.IsSet() && Reserved->CeilingFor(static_cast<int32>(EDepotModule::Shed)) >= 2)) { return false; }

	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	const int32 Built = Plots->GetModuleCount();
	const int32 Ghosts = Plots->GetGhostCount();
	TestTrue(TEXT("the facade takes the shed"), Facade->AddEntityModule(Depot, EDepotModule::Shed));
	TestEqual(TEXT("one more bay is lit - the Topology rebuild reached the buildings actor"), Plots->GetModuleCount(), Built + 1);
	TestEqual(TEXT("and one fewer is ghosted"), Plots->GetGhostCount(), Ghosts - 1);
	TestFalse(TEXT("R8: the purchase is a checkpoint - an undo would drop the shed and keep the money"), Facade->CanUndo());
	TestFalse(TEXT("a stand-shaped id is refused at the door"), Facade->AddEntityModule(FEntityInstanceId(), EDepotModule::Shed));
	return true;
}

#endif
```

- [ ] **Step 2: Build twice; verify red**

Expected: compile errors for `DepotKit::ReservationOf`, `AddEntityModule`.

- [ ] **Step 3: Implement the helper (move RecoverFrontage)**

`DepotKit.h`: add `#include "Misc/Optional.h"` below the existing includes. Inside `namespace DepotKit` (after `ReportIncomplete`) add:

```cpp
	/**
	 * Which edge of a placed plot is its frontage, recovered from the entity alone. MOVED FROM
	 * PlotPresenter.cpp's anonymous namespace (facility-upgrades spec) with its WHY comment in the .cpp:
	 * the purchase rules need the same answer the presenter draws with.
	 */
	AIRSIDE_API bool RecoverFrontage(const FEntityInstance& Entity, FVector2D& OutA, FVector2D& OutB);

	/**
	 * Everything a PLACED plotted depot's plot has room for - the SAME solve UPlotPresenter::RebuildFrom
	 * draws from (its definition's layout, the recovered frontage, the pose as gate and seed). Unset for
	 * anything that is not a live plotted depot, or whose frontage cannot be recovered.
	 *
	 * ONE SOLVE, TWO READERS: the presenter's lit/ghosted bays and UFacilityPurchases' "free reserved
	 * slot" (R9) are one fact, so a Buy shed that lights nothing cannot happen. Modules play no part -
	 * what a plot HOLDS does not depend on what was bought.
	 * ENFORCED BY: Check-Architecture rule 4 row 'PlotLayoutFor'; Airside.Build.DepotKit.ReservationOfIsThePresentersSolve
	 */
	AIRSIDE_API TOptional<PlotYard::FReservation> ReservationOf(const FEntityInstance& Depot,
		TArrayView<const PlotYard::FKitSpec> Specs);
```

`DepotKit.cpp`: add includes `#include "Build/PlotLayoutStrategy.h"` and `#include "Entities/EntityDefinition.h"` (Build/ may include Entities/). Append:

```cpp
bool DepotKit::RecoverFrontage(const FEntityInstance& Entity, FVector2D& OutA, FVector2D& OutB)
{
	// <MOVE the whole WHY comment block that sat above RecoverFrontage in PlotPresenter.cpp here,
	//  verbatim - "Which edge of the plot is its frontage ... it stays decided.">
	double BestDistance = TNumericLimits<double>::Max();
	int32 BestEdge = INDEX_NONE;

	for (int32 I = 0; I < Entity.Outline.Num(); ++I)
	{
		const FVector2D& A = Entity.Outline[I];
		const FVector2D& B = Entity.Outline[(I + 1) % Entity.Outline.Num()];
		const double Distance = FVector2D::Distance((A + B) * 0.5, Entity.Position);
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			BestEdge = I;
		}
	}

	if (BestEdge == INDEX_NONE)
	{
		return false;
	}

	OutA = Entity.Outline[BestEdge];
	OutB = Entity.Outline[(BestEdge + 1) % Entity.Outline.Num()];
	return true;
}

TOptional<PlotYard::FReservation> DepotKit::ReservationOf(const FEntityInstance& Depot,
	TArrayView<const PlotYard::FKitSpec> Specs)
{
	if (!Depot.bAlive || !Depot.IsDepot() || !Depot.IsPlotted())
	{
		return {};
	}
	FVector2D FrontageA = FVector2D::ZeroVector;
	FVector2D FrontageB = FVector2D::ZeroVector;
	if (!RecoverFrontage(Depot, FrontageA, FrontageB))
	{
		return {};
	}
	// Depot.Outline OUTLIVES THE SOLVE - FPlotSite::Outline is a view (its own comment).
	FPlotSite Site;
	Site.Outline = Depot.Outline;
	Site.FrontageA = FrontageA;
	Site.FrontageB = FrontageB;
	Site.Gate = Depot.Position;
	Site.Seed = DepotYardSeed(Depot.Position);
	const EPlotLayout Layout = Depot.Definition != nullptr ? Depot.Definition->Layout : EPlotLayout::Scatter;
	return PlotLayoutFor(Layout)->Solve(Site, Specs);
}
```

`PlotPresenter.cpp`: delete `RecoverFrontage` and its comment from the anonymous namespace (lines ~217-257; the comment moved above). In `RebuildFrom`, replace the block from `FVector2D FrontageA = FVector2D::ZeroVector;` through `const PlotYard::FReservation Reservation = PlotLayoutFor(Layout)->Solve(PlotSite, Specs);` with (KEEP the WHY comments that sit between them - "THE SAME GEOMETRY THE PLACEMENT USED", "THE YARD, NOT A GRID ...", "RE-DERIVED, NEVER SAVED", "THE PLOT TYPE DECIDES" - above this call):

```cpp
		const TOptional<PlotYard::FReservation> Solved = DepotKit::ReservationOf(Entity, Specs);
		if (!Solved.IsSet())
		{
			continue;
		}
		++Plots;
		const PlotYard::FReservation& Reservation = *Solved;
```

(and delete the now-unused `++Plots;` that followed `RecoverFrontage`). `Build/PlotLayoutStrategy.h` stays included if anything else in the file uses it; otherwise remove the include. Refactor contract: count `UE_LOG(` and comment lines in PlotPresenter.cpp + DepotKit.cpp before/after - neither may fall.

- [ ] **Step 4: Implement the network mutator**

`RoadNetwork.h`, after `SetEntityPoseRole`'s declaration:

```cpp
	/**
	 * Append Module to a live DEPOT's Modules - the one write a module purchase makes (facility-upgrades
	 * spec §3). False, nothing changed, for a dead or unset handle or a non-depot. A pure data write: no
	 * rebuild, no undo, no money - URoadEditFacade::AddEntityModule is the door that adds those. No
	 * EditRevision bump: that clock is scoped to nodes and segments (see GetEditRevision).
	 * ENFORCED BY: Airside.Model.EntityModules.AddAppendsToADepot
	 */
	bool AddEntityModule(FEntityInstanceId Entity, EDepotModule Module);
```

`RoadNetwork.cpp`, after `SetEntityPoseRole`:

```cpp
bool URoadNetwork::AddEntityModule(FEntityInstanceId Entity, EDepotModule Module)
{
	FEntityInstance* Instance = RoadSlot::Get<FEntityInstanceId>(Entities, Entity);
	if (Instance == nullptr || !Instance->bAlive || !Instance->IsDepot())
	{
		return false;
	}
	Instance->Modules.Add(Module);
	return true;
}
```

- [ ] **Step 5: Implement the facade door**

`RoadEditFacade.h`, after `SetDriveSide`:

```cpp
	/**
	 * A module bought for a depot (facility-upgrades spec §3): the network write, a Topology rebuild so
	 * AAirsideBuildingsActor relights the slot, then the undo history CLEARED (R8). Undo is a whole-network
	 * Memento; an undo past this would drop the shed and keep the money, so a purchase is a checkpoint.
	 * NOT ON IRoadEditTarget: no tool buys - UOpsRuntime's hook does. NOT PRICED here: money is
	 * UFacilityPurchases', posted after this returns true. False, nothing changed, for a non-depot.
	 * ENFORCED BY: Airside.Present.Facility.ModulePurchaseRelightsAndClearsUndo
	 */
	bool AddEntityModule(FEntityInstanceId Entity, EDepotModule Module);
```

`RoadEditFacade.cpp`, after `SetDriveSide`:

```cpp
bool URoadEditFacade::AddEntityModule(FEntityInstanceId Entity, EDepotModule Module)
{
	URoadNetwork* Network = Actor().Network;
	// REFUSED BEFORE THE SCOPE, SetDriveSide's reason: there is no rollback, and a refused write inside a
	// scope would push an undo step that does nothing.
	const FEntityInstance* Instance = Network != nullptr ? Network->GetEntity(Entity) : nullptr;
	if (Instance == nullptr || !Instance->bAlive || !Instance->IsDepot())
	{
		UE_LOG(LogRoadMesh, Warning, TEXT("AddEntityModule refused: entity %d is not a live depot"), Entity.Index);
		return false;
	}
	{
		// A SCOPE AND CommitAndNotify, the one door every mutator notifies through - then closed, so its
		// destructor has pushed the step BEFORE the history is cleared below.
		FRoadEditScope Edit(HistoryForEdit(), Network, TEXT("buy module"));
		Network->AddEntityModule(Entity, Module);
		CommitAndNotify(Edit, EChangeKind::Topology);
	}
	ClearHistory();
	UE_LOG(LogRoadMesh, Log, TEXT("Depot %d: %s added; undo history cleared (a purchase is a checkpoint)"),
		Entity.Index, *UEnum::GetValueAsString(Module));
	return true;
}
```

- [ ] **Step 6: Add the Check-Architecture row**

In `Tools/Check-Architecture.ps1` `$AllowedCallers`, after the `DepotKitSpecs` row add:

```powershell
    @{
        # ONE SOLVE PER PLACED PLOT (facility-upgrades spec, 2026-09-29): the presenter's lit/ghosted bays
        # and the purchase rules' free slot read DepotKit::ReservationOf. A new production caller of
        # PlotLayoutFor is a second solve that can disagree - the Buy shed that lights nothing.
        # The tool and the facade solve an UNPLACED outline (the ghost, the commit), not a placed plot.
        Name        = 'PlotLayoutFor'
        Pattern     = '\bPlotLayoutFor\s*\('
        ProdAllowed = @('Public\Build\PlotLayoutStrategy.h', 'Private\Build\PlotLayoutStrategy.cpp', 'Private\Build\DepotKit.cpp', 'Private\Present\RoadEditFacadeSurfaces.cpp', 'Private\Tool\PlotPlaceTool.cpp')
        TestExempt  = $true
        ProdReason  = 'a placed plot is solved by DepotKit::ReservationOf; an unplaced outline by URoadEditFacade::ReserveForPlot or FPlotPlaceTool'
    },
```

Verify the row bites: temporarily add `PlotLayoutFor(EPlotLayout::Scatter);` to `PlotPresenter.cpp`, run `pwsh ./Tools/Check-Architecture.ps1`, expect a failure naming the row; remove it (memory: a green rule may measure nothing).

- [ ] **Step 7: Build and test**

Build. Run `-Filter "Airside.Model.EntityModules+Airside.Build.DepotKit+Airside.Present.Facility+Airside.Present.PlotPresenter+Airside.Present.StandPlot"`. Expected: pass. If `setup: the plot reserves a second shed` fails, widen `FacilityModuleWidePlot` (e.g. 7000 uu) until the band layout reserves two sheds, and note the figure in its comment with today's date.

- [ ] **Step 8: Commit**

```bash
git add -A Plugins/Airside Tools/Check-Architecture.ps1
git commit -m "feat(facility): DepotKit::ReservationOf shared solve, AddEntityModule mutator and facade checkpoint"
```

---

### Task 4: UFacilityPurchases - the model and its world-free tests

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/FacilityPurchases.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/Model/FacilityPurchases.cpp`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/Ledger.h:106-111`, `Private/Model/Ledger.cpp:157-176`
- Modify: `Plugins/AirportOps/Source/AirportOpsTests/Private/FacilityPurchasesTest.cpp`

**Interfaces:**
- Consumes: `UJobBoard::AddPurchasedVehicle/CanRemoveVehicle/RemoveVehicle/VehiclesAt/VehicleLine/SpecFor/VehicleSpecs/FindVehicle` (Task 2), `URoadNetwork::AddEntityModule` (Task 3), `FModuleOffer`, `FFuelVehicleSpec` prices (Task 1).
- Produces: `EPurchaseRefusal`; `FModuleOfferQuote`, `FVehicleOfferQuote`, `FFleetRowQuote`, `FFacilityQuote { EPurchaseRefusal Refusal; int32 Bays; int32 Vehicles; TArray<...> Modules, VehicleOffers, Fleet; bool IsFacility() const; }`, `FPurchaseResult { EPurchaseRefusal Refusal; double Amount; int32 VehicleId; bool Succeeded() const; }`, `FFacilityUpkeep { double Modules; double Fleet; }`; `UFacilityPurchases` with members `ModuleOffers, JobBoard, Ledger, Pricing, Clock, Bus, ReservedSlotsOf, ApplyModulePurchase` and methods `VehicleSlotsOf, Quote, BuyModule, BuyVehicle, SellVehicle, DailyUpkeep (Task 6), static RefusalText`; `ULedger::CanPay(double) const`.

- [ ] **Step 1: Write the failing tests**

In `FacilityPurchasesTest.cpp` add includes:

```cpp
#include "Entities/EntityDefinition.h"
#include "Model/FacilityPurchases.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Templates/UniquePtr.h"
```

Then, after the Task 1 test, add the fixture and tests:

```cpp
namespace
{
	/**
	 * A depot with the start kit on a NewObject network, a board with the scenario's vehicles, a ledger,
	 * an unwired bus, and the two hooks as world-free lambdas: the reserved-slot ceiling is a number the
	 * test sets, the module write is the network's own mutator. Heap-held (MakeUnique) where a test needs
	 * several: the hooks capture `this`.
	 */
	struct FFacilityFixture
	{
		URoadNetwork* Net = nullptr;
		UJobBoard* Board = nullptr;
		ULedger* Ledger = nullptr;
		USimClock* Clock = nullptr;
		UFacilityPurchases* Shop = nullptr;
		FOpsEventBus Bus;
		FEntityInstanceId Depot;
		int32 ReservedSheds = 3;
		int32 ApplyCalls = 0;

		explicit FFacilityFixture(double Balance = 500000.0)
		{
			Net = NewObject<URoadNetwork>(GetTransientPackage());
			Board = NewObject<UJobBoard>(GetTransientPackage());
			Board->VehicleSpecs = GetDefault<UScenario>()->FuelVehicles;
			Clock = NewObject<USimClock>(GetTransientPackage());
			Ledger = NewObject<ULedger>(GetTransientPackage());
			Ledger->Clock = Clock;
			Ledger->Open(Balance);
			Shop = NewObject<UFacilityPurchases>(GetTransientPackage());
			Shop->ModuleOffers = GetDefault<UScenario>()->ModuleOffers;
			Shop->JobBoard = Board;
			Shop->Ledger = Ledger;
			Shop->Clock = Clock;
			Shop->Bus = &Bus;
			Shop->ReservedSlotsOf = [this](FEntityInstanceId, const FEntityInstance&, EDepotModule Module)
			{
				return Module == EDepotModule::Shed ? ReservedSheds : 1;
			};
			Shop->ApplyModulePurchase = [this](FEntityInstanceId Id, EDepotModule Module)
			{
				++ApplyCalls;
				return Net->AddEntityModule(Id, Module);
			};

			UEntityDefinition* Def = UEntityDefinition::MakeFuelDepotTransient();
			FEntityPlacement Placement;
			Placement.Definition = Def;
			Placement.Anchors = Def->Anchors;
			Placement.Position = FVector2D(1500.0, 0.0);
			Placement.Heading = UE_DOUBLE_HALF_PI;
			Placement.PoseRole = EServiceRole::Fuel;
			Placement.Outline = { FVector2D(0.0, 0.0), FVector2D(3000.0, 0.0), FVector2D(3000.0, 2400.0), FVector2D(0.0, 2400.0) };
			Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
			Placement.Trucks = 0;
			Depot = Net->PlaceEntity(Placement);
		}

		/** Sheds on the depot; 0 once it is removed (GetEntity is null for a dead handle). */
		int32 Sheds() const
		{
			const FEntityInstance* Entity = Net->GetEntity(Depot);
			int32 Count = 0;
			for (const EDepotModule Module : Entity != nullptr ? Entity->Modules : TArray<EDepotModule>()) { Count += Module == EDepotModule::Shed ? 1 : 0; }
			return Count;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilitySlotsTest, "AirportOps.Model.Facility.SlotsDeriveFromModules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilitySlotsTest::RunTest(const FString&)
{
	// CAPACITY IS DERIVED, NEVER STORED (spec §2): the offers' VehicleSlots over the owned modules.
	FFacilityFixture F;
	TestEqual(TEXT("the start kit's one shed is one bay"), F.Shop->VehicleSlotsOf(*F.Net->GetEntity(F.Depot)), 1);
	F.Net->AddEntityModule(F.Depot, EDepotModule::Tank);
	TestEqual(TEXT("a tank grants nothing - it has no offer"), F.Shop->VehicleSlotsOf(*F.Net->GetEntity(F.Depot)), 1);
	F.Net->AddEntityModule(F.Depot, EDepotModule::Shed);
	TestEqual(TEXT("a second shed is a second bay"), F.Shop->VehicleSlotsOf(*F.Net->GetEntity(F.Depot)), 2);
	TestEqual(TEXT("and the quote reads the same count"), F.Shop->Quote(*F.Net, F.Depot).Bays, 2);
	TestEqual(TEXT("a new depot has no vehicles (R3)"), F.Shop->Quote(*F.Net, F.Depot).Vehicles, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityBuyVehicleTest, "AirportOps.Model.Facility.BuyVehicleChargesAndAddsIdleFull",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityBuyVehicleTest::RunTest(const FString&)
{
	FFacilityFixture F;
	const FPurchaseResult R = F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("FUEL"));
	if (!TestTrue(TEXT("a bay and the money: bought"), R.Succeeded())) { return false; }
	TestEqual(TEXT("for the bowser's price"), R.Amount, 90000.0, 1e-9);
	TestEqual(TEXT("taken from the balance, synchronously"), F.Ledger->Balance(), 410000.0, 1e-6);
	TestEqual(TEXT("as a Fleet entry"), static_cast<int32>(F.Ledger->Entries().Last().Category), static_cast<int32>(ELedgerCategory::Fleet));
	const FServiceVehicle* Vehicle = F.Board->FindVehicle(R.VehicleId);
	if (!TestNotNull(TEXT("the vehicle is on the board"), Vehicle)) { return false; }
	TestEqual(TEXT("idle"), static_cast<int32>(Vehicle->State), static_cast<int32>(EServiceVehicleState::Idle));
	TestEqual(TEXT("and full"), Vehicle->Cargo, 10000.0, 1e-9);
	TestEqual(TEXT("one FleetChanged published, nothing else"), F.Bus.QueuedCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityRefusalsTest, "AirportOps.Model.Facility.RefusalsChargeAndPublishNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityRefusalsTest::RunTest(const FString&)
{
	// EVERY REFUSAL: nothing charged, nothing published, nothing added (spec §3 "a refusal is never charged").
	auto Nothing = [this](const TCHAR* Case, const FFacilityFixture& F, const FPurchaseResult& R, EPurchaseRefusal Expected,
		double Balance, int32 Entries, int32 Queued, int32 Vehicles, int32 Sheds)
	{
		TestEqual(FString::Printf(TEXT("%s: refused for the named reason"), Case), static_cast<int32>(R.Refusal), static_cast<int32>(Expected));
		TestEqual(FString::Printf(TEXT("%s: no money moved"), Case), F.Ledger->Balance(), Balance, 1e-6);
		TestEqual(FString::Printf(TEXT("%s: no ledger entry"), Case), F.Ledger->Entries().Num(), Entries);
		TestEqual(FString::Printf(TEXT("%s: nothing published"), Case), F.Bus.QueuedCount(), Queued);
		TestEqual(FString::Printf(TEXT("%s: no vehicle added or removed"), Case), F.Board->GetVehicles().Num(), Vehicles);
		TestEqual(FString::Printf(TEXT("%s: no shed added"), Case), F.Sheds(), Sheds);
	};
	auto Snap = [](const FFacilityFixture& F, double& Balance, int32& Entries, int32& Queued, int32& Vehicles, int32& Sheds)
	{
		Balance = F.Ledger->Balance(); Entries = F.Ledger->Entries().Num(); Queued = F.Bus.QueuedCount();
		Vehicles = F.Board->GetVehicles().Num(); Sheds = F.Sheds();
	};
	double B; int32 E, Q, V, S;

	{
		FFacilityFixture F;
		F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("FUEL"));
		Snap(F, B, E, Q, V, S);
		Nothing(TEXT("NoFreeBay"), F, F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("UTILITY")), EPurchaseRefusal::NoFreeBay, B, E, Q, V, S);
	}
	{
		FFacilityFixture F(1000.0);
		Snap(F, B, E, Q, V, S);
		Nothing(TEXT("CannotAfford"), F, F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("FUEL")), EPurchaseRefusal::CannotAfford, B, E, Q, V, S);
		Nothing(TEXT("CannotAfford a shed"), F, F.Shop->BuyModule(*F.Net, F.Depot, EDepotModule::Shed), EPurchaseRefusal::CannotAfford, B, E, Q, V, S);
		TestEqual(TEXT("CannotAfford never reached the network write"), F.ApplyCalls, 0);
	}
	{
		FFacilityFixture F;
		F.ReservedSheds = 1;
		Snap(F, B, E, Q, V, S);
		Nothing(TEXT("NoSlotReserved"), F, F.Shop->BuyModule(*F.Net, F.Depot, EDepotModule::Shed), EPurchaseRefusal::NoSlotReserved, B, E, Q, V, S);
	}
	{
		FFacilityFixture F;
		Snap(F, B, E, Q, V, S);
		Nothing(TEXT("UnknownType vehicle"), F, F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("TANKER")), EPurchaseRefusal::UnknownType, B, E, Q, V, S);
		Nothing(TEXT("UnknownType module (no offer)"), F, F.Shop->BuyModule(*F.Net, F.Depot, EDepotModule::Tank), EPurchaseRefusal::UnknownType, B, E, Q, V, S);
	}
	{
		FFacilityFixture F;
		const int32 Out = F.Board->AddVehicleForTest(TEXT("FUEL"), F.Depot, EServiceVehicleState::ToJob, 0.0).Id;
		FServiceVehicle& Promised = F.Board->AddVehicleForTest(TEXT("FUEL"), F.Depot, EServiceVehicleState::Idle, 0.0);
		Promised.Queue = { 99 };
		const int32 PromisedId = Promised.Id;
		Snap(F, B, E, Q, V, S);
		Nothing(TEXT("VehicleBusy on the road"), F, F.Shop->SellVehicle(Out), EPurchaseRefusal::VehicleBusy, B, E, Q, V, S);
		Nothing(TEXT("VehicleBusy with a queued job"), F, F.Shop->SellVehicle(PromisedId), EPurchaseRefusal::VehicleBusy, B, E, Q, V, S);
	}
	{
		FFacilityFixture F;
		UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
		const FEntityInstanceId Stand = F.Net->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(20000.0, 0.0), 0.0, 0.0, StandDef->PoseRole, 0);
		Snap(F, B, E, Q, V, S);
		Nothing(TEXT("NotAFacility: a stand"), F, F.Shop->BuyVehicle(*F.Net, Stand, TEXT("FUEL")), EPurchaseRefusal::NotAFacility, B, E, Q, V, S);
		Nothing(TEXT("NotAFacility: no such vehicle"), F, F.Shop->SellVehicle(12345), EPurchaseRefusal::NotAFacility, B, E, Q, V, S);
		F.Shop->ApplyModulePurchase = nullptr;
		Nothing(TEXT("NotAFacility: no module hook"), F, F.Shop->BuyModule(*F.Net, F.Depot, EDepotModule::Shed), EPurchaseRefusal::NotAFacility, B, E, Q, V, S);
		F.Net->RemoveEntity(F.Depot);
		Nothing(TEXT("NotAFacility: the depot was removed with its card open"), F, F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("FUEL")), EPurchaseRefusal::NotAFacility, B, E, Q, V, 0);
	}
	{
		FFacilityFixture F;
		F.Shop->ReservedSlotsOf = nullptr;
		Snap(F, B, E, Q, V, S);
		Nothing(TEXT("a null ceiling hook is a ceiling of 0"), F, F.Shop->BuyModule(*F.Net, F.Depot, EDepotModule::Shed), EPurchaseRefusal::NoSlotReserved, B, E, Q, V, S);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilitySellTest, "AirportOps.Model.Facility.SellCreditsResale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilitySellTest::RunTest(const FString&)
{
	FFacilityFixture F;
	const FPurchaseResult Bought = F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("FUEL"));
	const double AfterBuy = F.Ledger->Balance();
	const int32 Queued = F.Bus.QueuedCount();
	const FPurchaseResult Sold = F.Shop->SellVehicle(Bought.VehicleId);
	if (!TestTrue(TEXT("an idle vehicle sells"), Sold.Succeeded())) { return false; }
	TestEqual(TEXT("for Price x ResaleFraction"), Sold.Amount, 45000.0, 1e-9);
	TestEqual(TEXT("credited at once"), F.Ledger->Balance(), AfterBuy + 45000.0, 1e-6);
	TestEqual(TEXT("as a Fleet entry"), static_cast<int32>(F.Ledger->Entries().Last().Category), static_cast<int32>(ELedgerCategory::Fleet));
	TestNull(TEXT("and it is gone"), F.Board->FindVehicle(Bought.VehicleId));
	TestEqual(TEXT("one FleetChanged published"), F.Bus.QueuedCount(), Queued + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityShedOpensBayTest, "AirportOps.Model.Facility.ShedMakesASecondVehicleBuyable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityShedOpensBayTest::RunTest(const FString&)
{
	FFacilityFixture F;
	TestTrue(TEXT("the first vehicle fills the start kit's bay"), F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("FUEL")).Succeeded());
	TestEqual(TEXT("a second is refused for want of a bay"),
		static_cast<int32>(F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("UTILITY")).Refusal), static_cast<int32>(EPurchaseRefusal::NoFreeBay));
	const double Before = F.Ledger->Balance();
	const int32 Queued = F.Bus.QueuedCount();
	const FPurchaseResult Shed = F.Shop->BuyModule(*F.Net, F.Depot, EDepotModule::Shed);
	if (!TestTrue(TEXT("a shed into a free reserved slot"), Shed.Succeeded())) { return false; }
	TestEqual(TEXT("costs 40,000"), F.Ledger->Balance(), Before - 40000.0, 1e-6);
	TestEqual(TEXT("posted as Placement - a module is building work"),
		static_cast<int32>(F.Ledger->Entries().Last().Category), static_cast<int32>(ELedgerCategory::Placement));
	TestEqual(TEXT("through the module hook exactly once"), F.ApplyCalls, 1);
	TestEqual(TEXT("one FacilityUpgraded published"), F.Bus.QueuedCount(), Queued + 1);
	TestTrue(TEXT("and now the second vehicle is buyable"), F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("UTILITY")).Succeeded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityOverCapacityTest, "AirportOps.Model.Facility.OverCapacityFleetSellsButIsNotReplaced",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityOverCapacityTest::RunTest(const FString&)
{
	// A LOAD KEEPS THE SAVED FLEET EVEN OVER ITS BAYS (spec §3 "Save"): excess can be sold, not replaced.
	// The fleet's survival itself is AirportOps.Model.Save.FleetSurvivesALoad.
	FFacilityFixture F;
	const int32 A = F.Board->AddVehicleForTest(TEXT("FUEL"), F.Depot, EServiceVehicleState::Idle, 10000.0).Id;
	const int32 B = F.Board->AddVehicleForTest(TEXT("UTILITY"), F.Depot, EServiceVehicleState::Idle, 1000.0).Id;
	const FFacilityQuote Q = F.Shop->Quote(*F.Net, F.Depot);
	TestEqual(TEXT("two vehicles"), Q.Vehicles, 2);
	TestEqual(TEXT("in one bay"), Q.Bays, 1);
	TestEqual(TEXT("a third is refused"), static_cast<int32>(F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("FUEL")).Refusal), static_cast<int32>(EPurchaseRefusal::NoFreeBay));
	TestTrue(TEXT("the excess sells"), F.Shop->SellVehicle(A).Succeeded());
	TestEqual(TEXT("and is not replaced while the bay is still full"),
		static_cast<int32>(F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("FUEL")).Refusal), static_cast<int32>(EPurchaseRefusal::NoFreeBay));
	TestTrue(TEXT("the last one sells too"), F.Shop->SellVehicle(B).Succeeded());
	TestTrue(TEXT("and then one may be bought"), F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("FUEL")).Succeeded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityQuoteAgreesTest, "AirportOps.Model.Facility.QuoteEqualsCommandForEveryOffer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityQuoteAgreesTest::RunTest(const FString&)
{
	// THE UI AND THE RULES CANNOT DISAGREE (spec §3): for every offer and fleet row the quote shows, the
	// command run on an identical airport refuses for exactly the quoted reason (or succeeds).
	struct FCase { const TCHAR* Name; double Balance; int32 ReservedSheds; int32 Idle; int32 Busy; };
	const FCase Cases[] = {
		{ TEXT("rich, room, empty"), 500000.0, 3, 0, 0 },
		{ TEXT("poor"), 1000.0, 3, 0, 0 },
		{ TEXT("no space"), 500000.0, 1, 0, 0 },
		{ TEXT("bays full"), 500000.0, 3, 1, 0 },
		{ TEXT("a busy vehicle"), 500000.0, 3, 0, 1 },
	};
	auto Make = [](const FCase& Case)
	{
		TUniquePtr<FFacilityFixture> F = MakeUnique<FFacilityFixture>(Case.Balance);
		F->ReservedSheds = Case.ReservedSheds;
		for (int32 I = 0; I < Case.Idle; ++I) { F->Board->AddVehicleForTest(TEXT("FUEL"), F->Depot, EServiceVehicleState::Idle, 10000.0); }
		for (int32 I = 0; I < Case.Busy; ++I) { F->Board->AddVehicleForTest(TEXT("FUEL"), F->Depot, EServiceVehicleState::ToJob, 0.0); }
		return F;
	};
	for (const FCase& Case : Cases)
	{
		const TUniquePtr<FFacilityFixture> Quoted = Make(Case);
		const FFacilityQuote Q = Quoted->Shop->Quote(*Quoted->Net, Quoted->Depot);
		TestTrue(FString::Printf(TEXT("%s: the depot is a facility"), Case.Name), Q.IsFacility());
		for (const FModuleOfferQuote& Offer : Q.Modules)
		{
			const TUniquePtr<FFacilityFixture> Run = Make(Case);
			TestEqual(FString::Printf(TEXT("%s: module %s quote and command agree"), Case.Name, *UEnum::GetValueAsString(Offer.Module)),
				static_cast<int32>(Run->Shop->BuyModule(*Run->Net, Run->Depot, Offer.Module).Refusal), static_cast<int32>(Offer.Refusal));
		}
		for (const FVehicleOfferQuote& Offer : Q.VehicleOffers)
		{
			const TUniquePtr<FFacilityFixture> Run = Make(Case);
			TestEqual(FString::Printf(TEXT("%s: vehicle %s quote and command agree"), Case.Name, *Offer.TypeCode.ToString()),
				static_cast<int32>(Run->Shop->BuyVehicle(*Run->Net, Run->Depot, Offer.TypeCode).Refusal), static_cast<int32>(Offer.Refusal));
		}
		for (const FFleetRowQuote& Row : Q.Fleet)
		{
			const TUniquePtr<FFacilityFixture> Run = Make(Case);
			TestEqual(FString::Printf(TEXT("%s: selling vehicle %d, quote and command agree"), Case.Name, Row.VehicleId),
				static_cast<int32>(Run->Shop->SellVehicle(Row.VehicleId).Refusal), static_cast<int32>(Row.Refusal));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityQuoteNotAFacilityTest, "AirportOps.Model.Facility.QuoteOfAStandIsNotAFacility",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityQuoteNotAFacilityTest::RunTest(const FString&)
{
	FFacilityFixture F;
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = F.Net->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(20000.0, 0.0), 0.0, 0.0, StandDef->PoseRole, 0);
	const FFacilityQuote Q = F.Shop->Quote(*F.Net, Stand);
	TestFalse(TEXT("a stand is no facility - the card shows no purchase rows"), Q.IsFacility());
	TestEqual(TEXT("and offers nothing"), Q.Modules.Num() + Q.VehicleOffers.Num() + Q.Fleet.Num(), 0);
	return true;
}
```

- [ ] **Step 2: Build twice (new FacilityPurchases.cpp comes in Step 3); verify red**

Build once now: expected compile error `Model/FacilityPurchases.h: No such file`.

- [ ] **Step 3: Implement `ULedger::CanPay`**

`Ledger.h`, after `Describe` (line 110):

```cpp
	/**
	 * The ONE affordability rule, for a price already known: free is always allowed, else Price <= Balance.
	 * CanAfford prices a quote and asks this; UFacilityPurchases asks it directly for a catalogue price -
	 * so a shed and a taxiway are refused under water by the same line.
	 */
	bool CanPay(double Price) const;
```

`Ledger.cpp`, replace the body of `CanAfford` after `const double Price = PriceOf(Quote);` - keep both WHY comments, moved into `CanPay`:

```cpp
bool ULedger::CanAfford(const FBuildQuote& Quote) const
{
	return CanPay(PriceOf(Quote));
}

bool ULedger::CanPay(double Price) const
{
	// <the two existing WHY comments from CanAfford, verbatim: "A FREE EDIT IS ALWAYS ALLOWED ..." and
	//  "THE GDD'S "A NEGATIVE BALANCE LOCKS PLACEMENT" FALLS OUT OF THIS ...">
	if (Price <= 0.0)
	{
		return true;
	}
	return Price <= Balance();
}
```

- [ ] **Step 4: Implement the header**

Create `Plugins/AirportOps/Source/AirportOps/Public/Model/FacilityPurchases.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Model/OpsDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"
#include "UObject/Object.h"
#include "FacilityPurchases.generated.h"

class FOpsEventBus;
class UJobBoard;
class ULedger;
class UPricing;
class URoadNetwork;
class USimClock;

/** Why a purchase or sale was refused (spec 2026-09-29-facility-upgrades §3). The UI's disabled reason. */
UENUM()
enum class EPurchaseRefusal : uint8
{
	None,
	/** Not a live facility: a stand, a removed depot, an unknown vehicle, or no world hook. */
	NotAFacility,
	CannotAfford,
	/** R9: no free reserved (ghost) slot for the module. */
	NoSlotReserved,
	/** R2: every bay has a vehicle. */
	NoFreeBay,
	/** No offer for it: a vehicle row absent from the scenario, or a module with no FModuleOffer. */
	UnknownType,
	/** R5: not idle at home with an empty queue. */
	VehicleBusy
};

/** One module offer as the card shows it. Label is "Buy Shed ¤40,000". */
struct FModuleOfferQuote
{
	EDepotModule Module = EDepotModule::Shed;
	FText Name;
	FText PluralName;
	double Price = 0.0;
	double UpkeepPerDay = 0.0;
	int32 Owned = 0;
	int32 Reserved = 0;
	EPurchaseRefusal Refusal = EPurchaseRefusal::None;
	FText Label;
};

/** One vehicle offer. Label is "Bowser ¤90,000 · 10,000 L". */
struct FVehicleOfferQuote
{
	FName TypeCode;
	FText Name;
	double Price = 0.0;
	double UpkeepPerDay = 0.0;
	double CapacityLitres = 0.0;
	EPurchaseRefusal Refusal = EPurchaseRefusal::None;
	FText Label;
};

/** One vehicle at the depot, and whether it can be sold. SellLabel is "Sell ¤45,000". */
struct FFleetRowQuote
{
	int32 VehicleId = 0;
	FName TypeCode;
	FString Line;
	double Refund = 0.0;
	EPurchaseRefusal Refusal = EPurchaseRefusal::None;
	FText SellLabel;
};

/**
 * Everything the depot card shows, and nothing it computes (spec §3: the UI renders ONLY the quote).
 * A PLAIN STRUCT, like FBuildQuote: never saved, never reflected, rebuilt on every ask.
 */
struct FFacilityQuote
{
	/** None for a live facility; NotAFacility otherwise, with every array empty. */
	EPurchaseRefusal Refusal = EPurchaseRefusal::NotAFacility;
	int32 Bays = 0;
	int32 Vehicles = 0;
	TArray<FModuleOfferQuote> Modules;
	TArray<FVehicleOfferQuote> VehicleOffers;
	TArray<FFleetRowQuote> Fleet;

	bool IsFacility() const { return Refusal == EPurchaseRefusal::None; }
};

/** What a command did. Amount is what was charged (buy) or credited (sell); 0 when refused. */
struct FPurchaseResult
{
	EPurchaseRefusal Refusal = EPurchaseRefusal::None;
	double Amount = 0.0;
	/** The bought vehicle's id; 0 otherwise. */
	int32 VehicleId = 0;

	bool Succeeded() const { return Refusal == EPurchaseRefusal::None; }
};

/** A day's facility upkeep, two figures so the ledger shows them as two lines (spec §3). */
struct FFacilityUpkeep
{
	double Modules = 0.0;
	double Fleet = 0.0;
};

/**
 * Buying a building's modules and vehicles, and selling its vehicles - one purchase service for every
 * building with modules and a fleet (spec 2026-09-29-facility-upgrades, R1/R7). Today: fuel depots.
 *
 * PATTERN: a Command service over three owners, none of which it duplicates. The network owns Modules
 * (written only through ApplyModulePurchase), UJobBoard owns Vehicles (through AddPurchasedVehicle and
 * RemoveVehicle), ULedger owns the money. Capacity is DERIVED on every ask - offers' VehicleSlots over
 * owned modules - never stored.
 *
 * QUOTE AND COMMAND SHARE THEIR JUDGEMENT: each command re-runs the private Judge* its quote row was
 * built from, so a button can only be lit for a command that will succeed.
 * ENFORCED BY: AirportOps.Model.Facility.QuoteEqualsCommandForEveryOffer
 *
 * WORLD-FREE (Model/): the reserved-slot ceiling (a plot solve in Airside Build/) and the module write
 * (a facade edit with a rebuild and an undo checkpoint) come in as TFunction hooks set by UOpsRuntime at
 * attach - UJobBoard::DesignVehicleOf's pattern. The network is passed PER CALL, not held: the actor's
 * network object is replaced by a clear, a load and every undo (§6 deviation 3).
 */
UCLASS()
class AIRPORTOPS_API UFacilityPurchases : public UObject
{
	GENERATED_BODY()

public:
	/** Copied from UScenario::ModuleOffers at attach. A module with no row is not for sale. */
	UPROPERTY() TMap<EDepotModule, FModuleOffer> ModuleOffers;

	/** Set by UOpsRuntime's constructor, like UAgentRescue's boards. Null refuses everything NotAFacility. */
	UPROPERTY() TObjectPtr<UJobBoard> JobBoard = nullptr;

	/** The money. Null in a test that does not care: then everything is free, IBuildPurse's rule. */
	UPROPERTY() TObjectPtr<ULedger> Ledger = nullptr;

	/** Formats prices for the labels; null formats plain numbers. */
	UPROPERTY() TObjectPtr<UPricing> Pricing = nullptr;

	/** Dates the ledger entries; null dates them 0, ULedger::NowOrZero's rule. */
	UPROPERTY() TObjectPtr<USimClock> Clock = nullptr;

	/** Published to on success only. Owned by UOpsRuntime, like UAirlineRoster::Bus. */
	FOpsEventBus* Bus = nullptr;

	/**
	 * How many of Module the placed plot of Depot can hold - PlotYard's reservation ceiling. UNSET (a bare
	 * NewObject) answers 0, so no module can be bought: refusing is the honest default for a question
	 * this layer cannot answer.
	 */
	TFunction<int32(FEntityInstanceId Id, const FEntityInstance& Depot, EDepotModule Module)> ReservedSlotsOf;

	/**
	 * Append Module to the depot, rebuild its yard and checkpoint undo - URoadEditFacade::AddEntityModule
	 * in production. UNSET or false refuses NotAFacility, uncharged.
	 */
	TFunction<bool(FEntityInstanceId Id, EDepotModule Module)> ApplyModulePurchase;

	/** Vehicle bays Depot's modules grant. */
	int32 VehicleSlotsOf(const FEntityInstance& Depot) const;

	FFacilityQuote Quote(const URoadNetwork& Network, FEntityInstanceId Entity) const;
	FPurchaseResult BuyModule(const URoadNetwork& Network, FEntityInstanceId Entity, EDepotModule Module);
	FPurchaseResult BuyVehicle(const URoadNetwork& Network, FEntityInstanceId Entity, FName TypeCode);
	FPurchaseResult SellVehicle(int32 VehicleId);

	/** "No space", "Can't afford" - the disabled button's reason. The wording is the contract. */
	static FText RefusalText(EPurchaseRefusal Why);

private:
	/** Entity if it is a live facility on Network, else null. */
	static const FEntityInstance* FacilityAt(const URoadNetwork& Network, FEntityInstanceId Entity);

	int32 ReservedOf(FEntityInstanceId Entity, const FEntityInstance& Facility, EDepotModule Module) const;
	static int32 OwnedOf(const FEntityInstance& Facility, EDepotModule Module);

	EPurchaseRefusal JudgeModule(const FEntityInstance* Facility, EDepotModule Module, int32 Reserved) const;
	EPurchaseRefusal JudgeVehicle(const FEntityInstance* Facility, FEntityInstanceId Entity, FName TypeCode) const;
	EPurchaseRefusal JudgeSale(int32 VehicleId) const;

	bool CanPay(double Price) const;
	double NowOrZero() const;
	FText Money(double Amount) const;
	FText VehicleName(FName TypeCode) const;
	double RefundOf(FName TypeCode) const;
	void LogRefused(int32 Depot, const FString& What, EPurchaseRefusal Why) const;
};
```

- [ ] **Step 5: Implement the .cpp**

Create `Plugins/AirportOps/Source/AirportOps/Private/Model/FacilityPurchases.cpp`:

```cpp
#include "Model/FacilityPurchases.h"

#include "AirportOpsLog.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"
#include "Model/Pricing.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"

#define LOCTEXT_NAMESPACE "FacilityPurchases"

FText UFacilityPurchases::RefusalText(EPurchaseRefusal Why)
{
	switch (Why)
	{
	case EPurchaseRefusal::None:           return FText::GetEmpty();
	case EPurchaseRefusal::NotAFacility:   return LOCTEXT("NotAFacility", "Not a facility");
	case EPurchaseRefusal::CannotAfford:   return LOCTEXT("CannotAfford", "Can't afford");
	case EPurchaseRefusal::NoSlotReserved: return LOCTEXT("NoSlotReserved", "No space");
	case EPurchaseRefusal::NoFreeBay:      return LOCTEXT("NoFreeBay", "No free bay - buy a shed");
	case EPurchaseRefusal::UnknownType:    return LOCTEXT("UnknownType", "Not for sale");
	case EPurchaseRefusal::VehicleBusy:    return LOCTEXT("VehicleBusy", "Busy");
	}
	return FText::GetEmpty();
}

const FEntityInstance* UFacilityPurchases::FacilityAt(const URoadNetwork& Network, FEntityInstanceId Entity)
{
	// A FUEL DEPOT IS THE ONE FACILITY TODAY (R1 is the generic shape; the fuel depot its first consumer).
	const FEntityInstance* Instance = Network.GetEntity(Entity);
	return Instance != nullptr && Instance->bAlive && Instance->IsDepot() ? Instance : nullptr;
}

int32 UFacilityPurchases::VehicleSlotsOf(const FEntityInstance& Depot) const
{
	int32 Slots = 0;
	for (const EDepotModule Module : Depot.Modules)
	{
		if (const FModuleOffer* Offer = ModuleOffers.Find(Module))
		{
			Slots += Offer->VehicleSlots;
		}
	}
	return Slots;
}

int32 UFacilityPurchases::OwnedOf(const FEntityInstance& Facility, EDepotModule Module)
{
	int32 Count = 0;
	for (const EDepotModule Each : Facility.Modules)
	{
		Count += Each == Module ? 1 : 0;
	}
	return Count;
}

int32 UFacilityPurchases::ReservedOf(FEntityInstanceId Entity, const FEntityInstance& Facility, EDepotModule Module) const
{
	return ReservedSlotsOf ? ReservedSlotsOf(Entity, Facility, Module) : 0;
}

bool UFacilityPurchases::CanPay(double Price) const
{
	return Ledger == nullptr || Ledger->CanPay(Price);
}

double UFacilityPurchases::NowOrZero() const
{
	return Clock != nullptr ? Clock->Now() : 0.0;
}

FText UFacilityPurchases::Money(double Amount) const
{
	return Pricing != nullptr ? Pricing->Format(Amount) : FText::AsNumber(FMath::RoundToInt(Amount));
}

FText UFacilityPurchases::VehicleName(FName TypeCode) const
{
	const FFuelVehicleSpec* Spec = JobBoard != nullptr ? JobBoard->VehicleSpecs.Find(TypeCode) : nullptr;
	return Spec != nullptr && !Spec->DisplayName.IsEmpty() ? Spec->DisplayName : FText::FromName(TypeCode);
}

double UFacilityPurchases::RefundOf(FName TypeCode) const
{
	const FFuelVehicleSpec* Spec = JobBoard != nullptr ? JobBoard->VehicleSpecs.Find(TypeCode) : nullptr;
	return Spec != nullptr ? Spec->Price * Spec->ResaleFraction : 0.0;
}

void UFacilityPurchases::LogRefused(int32 Depot, const FString& What, EPurchaseRefusal Why) const
{
	// THE BARE NAME ("NoFreeBay"), the spec's log wording - not GetValueAsString's "EPurchaseRefusal::".
	UE_LOG(LogAirportOps, Log, TEXT("Purchase refused: depot %d %s - %s"), Depot, *What,
		*StaticEnum<EPurchaseRefusal>()->GetNameStringByValue(static_cast<int64>(Why)));
}

// --- The judgements: the ONE place each rule is written. Quote and command both call these. --------
// ORDER: what the player cannot fix by waiting first (not a facility, not for sale, no room), money last -
// a button reads "No space" rather than "Can't afford" when both are true, because saving up would not help.

EPurchaseRefusal UFacilityPurchases::JudgeModule(const FEntityInstance* Facility, EDepotModule Module, int32 Reserved) const
{
	if (Facility == nullptr)
	{
		return EPurchaseRefusal::NotAFacility;
	}
	const FModuleOffer* Offer = ModuleOffers.Find(Module);
	if (Offer == nullptr)
	{
		return EPurchaseRefusal::UnknownType;
	}
	if (OwnedOf(*Facility, Module) >= Reserved)
	{
		return EPurchaseRefusal::NoSlotReserved;
	}
	return CanPay(Offer->Price) ? EPurchaseRefusal::None : EPurchaseRefusal::CannotAfford;
}

EPurchaseRefusal UFacilityPurchases::JudgeVehicle(const FEntityInstance* Facility, FEntityInstanceId Entity, FName TypeCode) const
{
	if (Facility == nullptr || JobBoard == nullptr)
	{
		return EPurchaseRefusal::NotAFacility;
	}
	const FFuelVehicleSpec* Spec = JobBoard->VehicleSpecs.Find(TypeCode);
	if (Spec == nullptr)
	{
		return EPurchaseRefusal::UnknownType;
	}
	if (JobBoard->VehiclesAt(Entity) >= VehicleSlotsOf(*Facility))
	{
		return EPurchaseRefusal::NoFreeBay;
	}
	return CanPay(Spec->Price) ? EPurchaseRefusal::None : EPurchaseRefusal::CannotAfford;
}

EPurchaseRefusal UFacilityPurchases::JudgeSale(int32 VehicleId) const
{
	if (JobBoard == nullptr || JobBoard->FindVehicle(VehicleId) == nullptr)
	{
		return EPurchaseRefusal::NotAFacility;
	}
	return JobBoard->CanRemoveVehicle(VehicleId) ? EPurchaseRefusal::None : EPurchaseRefusal::VehicleBusy;
}

FFacilityQuote UFacilityPurchases::Quote(const URoadNetwork& Network, FEntityInstanceId Entity) const
{
	FFacilityQuote Out;
	const FEntityInstance* Facility = FacilityAt(Network, Entity);
	if (Facility == nullptr || JobBoard == nullptr)
	{
		return Out;
	}
	Out.Refusal = EPurchaseRefusal::None;
	Out.Bays = VehicleSlotsOf(*Facility);
	Out.Vehicles = JobBoard->VehiclesAt(Entity);

	// SORTED, so the card's rows and a menu's line indices do not move between two asks.
	TArray<EDepotModule> Modules;
	ModuleOffers.GenerateKeyArray(Modules);
	Modules.Sort();
	for (const EDepotModule Module : Modules)
	{
		const FModuleOffer& Offer = ModuleOffers[Module];
		FModuleOfferQuote& Row = Out.Modules.AddDefaulted_GetRef();
		Row.Module = Module;
		Row.Name = Offer.DisplayName;
		Row.PluralName = Offer.PluralName;
		Row.Price = Offer.Price;
		Row.UpkeepPerDay = Offer.UpkeepPerDay;
		Row.Owned = OwnedOf(*Facility, Module);
		Row.Reserved = ReservedOf(Entity, *Facility, Module);
		Row.Refusal = JudgeModule(Facility, Module, Row.Reserved);
		Row.Label = FText::Format(LOCTEXT("ModuleLabel", "Buy {0} {1}"), Offer.DisplayName, Money(Offer.Price));
	}

	TArray<FName> Types;
	JobBoard->VehicleSpecs.GenerateKeyArray(Types);
	Types.Sort(FNameLexicalLess());
	for (const FName TypeCode : Types)
	{
		const FFuelVehicleSpec& Spec = JobBoard->VehicleSpecs[TypeCode];
		FVehicleOfferQuote& Row = Out.VehicleOffers.AddDefaulted_GetRef();
		Row.TypeCode = TypeCode;
		Row.Name = VehicleName(TypeCode);
		Row.Price = Spec.Price;
		Row.UpkeepPerDay = Spec.UpkeepPerDay;
		Row.CapacityLitres = Spec.CapacityLitres;
		Row.Refusal = JudgeVehicle(Facility, Entity, TypeCode);
		Row.Label = FText::Format(LOCTEXT("VehicleLabel", "{0} {1} \u00B7 {2} L"), Row.Name, Money(Spec.Price),
			FText::AsNumber(FMath::RoundToInt(Spec.CapacityLitres)));
	}

	for (const FServiceVehicle& Vehicle : JobBoard->GetVehicles())
	{
		if (Vehicle.Home != Entity)
		{
			continue;
		}
		FFleetRowQuote& Row = Out.Fleet.AddDefaulted_GetRef();
		Row.VehicleId = Vehicle.Id;
		Row.TypeCode = Vehicle.TypeCode;
		Row.Line = JobBoard->VehicleLine(Vehicle);
		Row.Refund = RefundOf(Vehicle.TypeCode);
		Row.Refusal = JudgeSale(Vehicle.Id);
		Row.SellLabel = FText::Format(LOCTEXT("SellLabel", "Sell {0}"), Money(Row.Refund));
	}
	return Out;
}

FPurchaseResult UFacilityPurchases::BuyModule(const URoadNetwork& Network, FEntityInstanceId Entity, EDepotModule Module)
{
	FPurchaseResult Result;
	const FEntityInstance* Facility = FacilityAt(Network, Entity);
	const int32 Reserved = Facility != nullptr ? ReservedOf(Entity, *Facility, Module) : 0;
	Result.Refusal = JudgeModule(Facility, Module, Reserved);
	const FString What = StaticEnum<EDepotModule>()->GetNameStringByValue(static_cast<int64>(Module));
	if (!Result.Succeeded())
	{
		LogRefused(Entity.Index, What, Result.Refusal);
		return Result;
	}
	if (!ApplyModulePurchase)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Purchase refused: depot %d %s - no module hook (UOpsRuntime not attached)"), Entity.Index, *What);
		Result.Refusal = EPurchaseRefusal::NotAFacility;
		return Result;
	}
	// READ BEFORE THE WRITE: the hook rebuilds the airport, and nothing here touches Facility after it.
	const FModuleOffer Offer = ModuleOffers[Module];
	const int32 Owned = OwnedOf(*Facility, Module) + 1;
	if (!ApplyModulePurchase(Entity, Module))
	{
		LogRefused(Entity.Index, What, EPurchaseRefusal::NotAFacility);
		Result.Refusal = EPurchaseRefusal::NotAFacility;
		return Result;
	}
	// CHARGED AFTER THE WRITE SUCCEEDED and in the same call - a refusal is never charged, and nothing can
	// move the balance between the judgement above and this line.
	Result.Amount = Offer.Price;
	if (Ledger != nullptr)
	{
		Ledger->Post(NowOrZero(), ELedgerCategory::Placement, -Offer.Price,
			FText::Format(LOCTEXT("BoughtModule", "Bought {0}"), Offer.DisplayName));
	}
	UE_LOG(LogAirportOps, Log, TEXT("Purchase: depot %d bought %s for %.0f (%d/%d slots)"),
		Entity.Index, *What, Offer.Price, Owned, Reserved);
	if (Bus != nullptr)
	{
		Bus->Publish(FFacilityUpgradedEvent{ Entity.Index, Module, Offer.Price });
	}
	return Result;
}

FPurchaseResult UFacilityPurchases::BuyVehicle(const URoadNetwork& Network, FEntityInstanceId Entity, FName TypeCode)
{
	FPurchaseResult Result;
	const FEntityInstance* Facility = FacilityAt(Network, Entity);
	Result.Refusal = JudgeVehicle(Facility, Entity, TypeCode);
	if (!Result.Succeeded())
	{
		LogRefused(Entity.Index, TypeCode.ToString(), Result.Refusal);
		return Result;
	}
	const double Price = JobBoard->VehicleSpecs[TypeCode].Price;
	Result.VehicleId = JobBoard->AddPurchasedVehicle(TypeCode, Entity);
	if (Result.VehicleId == 0)
	{
		Result.Refusal = EPurchaseRefusal::NotAFacility;
		LogRefused(Entity.Index, TypeCode.ToString(), Result.Refusal);
		return Result;
	}
	Result.Amount = Price;
	if (Ledger != nullptr)
	{
		Ledger->Post(NowOrZero(), ELedgerCategory::Fleet, -Price, FText::Format(LOCTEXT("BoughtVehicle", "Bought {0}"), VehicleName(TypeCode)));
	}
	UE_LOG(LogAirportOps, Log, TEXT("Purchase: depot %d bought %s for %.0f (%d/%d bays)"),
		Entity.Index, *TypeCode.ToString(), Price, JobBoard->VehiclesAt(Entity), VehicleSlotsOf(*Facility));
	if (Bus != nullptr)
	{
		Bus->Publish(FFleetChangedEvent{ Entity.Index, Result.VehicleId, TypeCode, EFleetChange::Bought, Price });
	}
	return Result;
}

FPurchaseResult UFacilityPurchases::SellVehicle(int32 VehicleId)
{
	FPurchaseResult Result;
	Result.Refusal = JudgeSale(VehicleId);
	const FServiceVehicle* Vehicle = JobBoard != nullptr ? JobBoard->FindVehicle(VehicleId) : nullptr;
	const int32 Depot = Vehicle != nullptr ? Vehicle->Home.Index : INDEX_NONE;
	if (!Result.Succeeded())
	{
		LogRefused(Depot, FString::Printf(TEXT("sell vehicle %d"), VehicleId), Result.Refusal);
		return Result;
	}
	// COPIED BEFORE THE REMOVE, which invalidates Vehicle.
	const FName TypeCode = Vehicle->TypeCode;
	const double Refund = RefundOf(TypeCode);
	JobBoard->RemoveVehicle(VehicleId);
	Result.Amount = Refund;
	if (Ledger != nullptr)
	{
		Ledger->Post(NowOrZero(), ELedgerCategory::Fleet, Refund,
			FText::Format(LOCTEXT("SoldVehicle", "Sold {0} #{1}"), VehicleName(TypeCode), FText::AsNumber(VehicleId)));
	}
	UE_LOG(LogAirportOps, Log, TEXT("Purchase: depot %d sold vehicle %d for %.0f"), Depot, VehicleId, Refund);
	if (Bus != nullptr)
	{
		Bus->Publish(FFleetChangedEvent{ Depot, VehicleId, TypeCode, EFleetChange::Sold, Refund });
	}
	return Result;
}

#undef LOCTEXT_NAMESPACE
```

NOTE: `FFacilityUpgradedEvent`, `FFleetChangedEvent` and `EFleetChange` are defined in Task 5. To keep Task 4 compiling and testable on its own, do Task 5 Step 3 (the two event structs, the variant entries and their `Describe`) as part of this step, then Task 5 adds the wiring. The world-free tests here only count `Bus.QueuedCount()`.

- [ ] **Step 6: Build twice; run**

Build twice (second run must compile `FacilityPurchases.cpp`). Run `-Filter "AirportOps.Model.Facility+AirportOps.Model.Ledger+AirportOps.Model.Bus"`. Expected: pass (EveryEventDescribesItself covers the two new events).

- [ ] **Step 7: Commit**

```bash
git add -A Plugins/AirportOps
git commit -m "feat(facility): UFacilityPurchases - quote, buy module/vehicle, sell; one judgement per rule"
```

---

### Task 5: Bus events, runtime ownership, forwarders, WireBus - and the board wakes on a purchase

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsEventBus.h:1-157`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Model/OpsEventBus.cpp:62-70`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Present/OpsRuntime.h`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp:28-53, 197-209, 405-419`
- Create: `Plugins/AirportOps/Source/AirportOpsTests/Private/FacilityWiredTest.cpp`

**Interfaces:**
- Consumes: `UFacilityPurchases` (Task 4).
- Produces: `enum class EFleetChange : uint8 { Bought, Sold }`; `FFacilityUpgradedEvent { int32 Entity; EDepotModule Module; double Amount; }`; `FFleetChangedEvent { int32 Depot; int32 VehicleId; FName TypeCode; EFleetChange Change; double Amount; }`; `UOpsRuntime::GetFacilityPurchases()`, `QuoteFacility(FEntityInstanceId) const`, `BuyModule(FEntityInstanceId, EDepotModule)`, `BuyVehicle(FEntityInstanceId, FName)`, `SellVehicle(int32)`.

- [ ] **Step 1: Write the failing composition test**

Create `Plugins/AirportOps/Source/AirportOpsTests/Private/FacilityWiredTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/FacilityPurchases.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/PlotPresenter.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/RoadEditTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

// THE COMPOSITION TESTS FOR FACILITY PURCHASES (facility-upgrades spec §5): each fails if UOpsRuntime
// leaves a seam unwired - the bus's wake-up, the module hook, the undo checkpoint, the ceiling memo.

namespace
{
	/** FuelServiceWiredTest's LayFuelLine, prefixed for the unity build. */
	void FacilityWiredLine(URoadNetwork& Net, const FVector2D& From, const FVector2D& To, ETraversalClass Class,
		FGuidelineNodeId& OutA, FGuidelineNodeId& OutB)
	{
		OutA = Net.AddGuidelineNode(From);
		OutB = Net.AddGuidelineNode(To);
		FGuidelineEdge Edge;
		Edge.A = OutA;
		Edge.B = OutB;
		Edge.Control = (From + To) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(Class);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	struct FFacilityFuelField
	{
		FEntityInstanceId Stand;
		FEntityInstanceId Depot;
		FGuidelineNodeId TaxiSouth;
	};

	/**
	 * FuelServiceWiredTest's airport - a taxiway, a Code C stand, a service road with a spur to the stand's
	 * far edge - with the depot placed as the PLAYER's would be: its start kit (one shed, one bay) and NO
	 * starter trucks (R3). Plotless, so no plot solve is involved in this seam.
	 */
	FFacilityFuelField FacilityFuelField(URoadNetwork& Net)
	{
		FFacilityFuelField Out;
		constexpr double RoadY = -4000.0;
		FGuidelineNodeId TaxiNorth;
		FacilityWiredLine(Net, FVector2D(-10000.0, -10000.0), FVector2D(-10000.0, 10000.0), ETraversalClass::Aircraft, Out.TaxiSouth, TaxiNorth);

		UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
		Out.Stand = Net.PlaceEntity(StandDef, StandDef->Anchors, FVector2D(0.0, 0.0), 0.0, 3600.0, StandDef->PoseRole, StandDef->Trucks);

		double FarEdge = -TNumericLimits<double>::Max();
		for (const FVector2D& Corner : Net.GetEntity(Out.Stand)->Outline)
		{
			FarEdge = FMath::Max(FarEdge, Corner.X);
		}
		const double SpurX = FarEdge + 420.0;
		FGuidelineNodeId SpurFoot, SpurHead;
		FacilityWiredLine(Net, FVector2D(SpurX, RoadY), FVector2D(SpurX, 10000.0), ETraversalClass::GroundVehicle, SpurFoot, SpurHead);
		auto Join = [&Net](FGuidelineNodeId A, FGuidelineNodeId B)
		{
			FGuidelineEdge Edge;
			Edge.A = A;
			Edge.B = B;
			Edge.Control = (Net.GetGuidelineNode(A)->Position + Net.GetGuidelineNode(B)->Position) * 0.5;
			Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
			Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
			Edge.Direction = EGuidelineDir::Bidirectional;
			Edge.Width = 600.0;
			Edge.bDerived = true;
			Net.AddGuidelineEdge(MoveTemp(Edge));
		};
		const FGuidelineNodeId RoadWest = Net.AddGuidelineNode(FVector2D(-20000.0, RoadY));
		const FGuidelineNodeId RoadEast = Net.AddGuidelineNode(FVector2D(20000.0, RoadY));
		Join(RoadWest, SpurFoot);
		Join(SpurFoot, RoadEast);

		UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
		FEntityPlacement Placement;
		Placement.Definition = DepotDef;
		Placement.Anchors = DepotDef->Anchors;
		Placement.Position = FVector2D(12000.0, RoadY + 4000.0);
		Placement.Heading = UE_DOUBLE_PI * 0.5;
		Placement.PoseRole = DepotDef->PoseRole;
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		Placement.Trucks = 0;
		Out.Depot = Net.PlaceEntity(Placement);
		FAnchorLink::Build(Net, UAirsideSettings::ResolveLargestServiceVehicle());
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityWakesBoardTest, "AirportOps.Present.Facility.PurchaseWakesTheBoard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityWakesBoardTest::RunTest(const FString&)
{
	// SPEC §5: a job waiting on an empty depot dispatches after buy + drain, with no tick polling. The seam
	// is WireBus's FleetChanged -> JobBoard pass; unwired, the job waits for an unrelated event.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;
	const FFacilityFuelField Field = FacilityFuelField(Net);

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	UJobBoard* Board = Runtime->GetJobBoard();

	const FRoutePlan Plan = TestGraph::Probe(Net, Field.TaxiSouth, Net.GetEntity(Field.Stand)->PoseNode, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("the aircraft routes to the stand"), Plan.IsValid())) { return false; }
	if (!TestTrue(TEXT("and dispatches"), Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultAirframe()))) { return false; }

	constexpr float Step = 1.0f / 30.0f;
	for (int32 Tick = 0; Tick < 20000; ++Tick)
	{
		Actor->Tick(Step);
		Runtime->Tick(Step);
		if (Board->GetJobs().Num() > 0 && Board->GetJobs()[0].State == EServiceJobState::Unserviceable) { break; }
	}
	if (!TestEqual(TEXT("parking made one job"), Board->GetJobs().Num(), 1)) { return false; }
	TestEqual(TEXT("refused: the depot has no vehicles (R3)"),
		static_cast<int32>(Board->GetJobs()[0].Why), static_cast<int32>(EServiceRefusal::NoVehicles));

	const int32 Quiet = Board->StepCountForTest();
	for (int32 Tick = 0; Tick < 60; ++Tick) { Actor->Tick(Step); Runtime->Tick(Step); }
	TestEqual(TEXT("a refused job is not polled - two quiet seconds run no step"), Board->StepCountForTest(), Quiet);

	const FName Kind = Board->VehiclesFor(EIcaoCode::C).TypeCode;
	const FPurchaseResult Bought = Runtime->BuyVehicle(Field.Depot, Kind);
	if (!TestTrue(TEXT("the depot's one bay takes a vehicle"), Bought.Succeeded())) { return false; }
	TestEqual(TEXT("nothing ran inside the command - the pass waits for the drain"), Board->StepCountForTest(), Quiet);

	Runtime->Tick(Step);
	TestTrue(TEXT("the drain ran the job board pass"), Board->StepCountForTest() > Quiet);
	const FServiceJob& Job = Board->GetJobs()[0];
	TestTrue(TEXT("the waiting job is now the bought vehicle's"),
		Job.VehicleId == Bought.VehicleId
		&& (Job.State == EServiceJobState::Queued || Job.State == EServiceJobState::Underway));
	return true;
}

#endif
```

- [ ] **Step 2: Build twice; verify red**

Expected: compile errors for `UOpsRuntime::BuyVehicle`.

- [ ] **Step 3: Implement the events** (if not already done in Task 4 Step 5)

`OpsEventBus.h`: add `#include "Model/RoadEntity.h"` with the other Model includes. After `FAirlineSatisfactionEvent`:

```cpp
/**
 * A facility bought a module (facility-upgrades spec §3). Published by UFacilityPurchases on success only.
 * Entity is the depot's INDEX - an id, never a pointer (spec 2026-09-29-ops-event-bus §4).
 */
struct AIRPORTOPS_API FFacilityUpgradedEvent
{
	int32 Entity = INDEX_NONE;
	EDepotModule Module = EDepotModule::Shed;
	double Amount = 0.0;
	static const TCHAR* EventName() { return TEXT("FacilityUpgraded"); }
	FString Describe() const;
};

/** Bought or sold. A plain enum - this header has no .generated.h for a UENUM (memory: UHT cannot see it). */
enum class EFleetChange : uint8
{
	Bought,
	Sold
};

/** A vehicle joined or left a depot's fleet. Published by UFacilityPurchases on success only. */
struct AIRPORTOPS_API FFleetChangedEvent
{
	int32 Depot = INDEX_NONE;
	int32 VehicleId = 0;
	FName TypeCode;
	EFleetChange Change = EFleetChange::Bought;
	double Amount = 0.0;
	static const TCHAR* EventName() { return TEXT("FleetChanged"); }
	FString Describe() const;
};
```

Append `, FFacilityUpgradedEvent, FFleetChangedEvent` to the `FOpsEvent` TVariant list (after `FNetworkChangedEvent`).

`OpsEventBus.cpp`, after `FNetworkChangedEvent::Describe`:

```cpp
FString FFacilityUpgradedEvent::Describe() const
{
	return FString::Printf(TEXT("depot %d, %s, %.0f"), Entity, *UEnum::GetValueAsString(Module), Amount);
}

FString FFleetChangedEvent::Describe() const
{
	return FString::Printf(TEXT("depot %d, vehicle %d %s, %s for %.0f"), Depot, VehicleId, *TypeCode.ToString(),
		Change == EFleetChange::Bought ? TEXT("bought") : TEXT("sold"), Amount);
}
```

- [ ] **Step 4: Implement runtime ownership and forwarders**

`OpsRuntime.h`: add `#include "Model/FacilityPurchases.h"` after `#include "Model/AgentRescue.h"`. After `GetAgentRescue()`:

```cpp
	/** Sheds and vehicles bought and sold. See UFacilityPurchases - this runtime owns it and wires its hooks. */
	UFacilityPurchases* GetFacilityPurchases() const { return FacilityPurchases; }

	/**
	 * FORWARDERS to UFacilityPurchases with this runtime's network - the one the driver does not hold, as
	 * CanUnstick supplies it. Refused NotAFacility when unattached. Logic lives in UFacilityPurchases.
	 * ENFORCED BY: AirportOps.Present.Facility.PurchaseWakesTheBoard
	 */
	FFacilityQuote QuoteFacility(FEntityInstanceId Entity) const;
	FPurchaseResult BuyModule(FEntityInstanceId Entity, EDepotModule Module);
	FPurchaseResult BuyVehicle(FEntityInstanceId Entity, FName TypeCode);
	FPurchaseResult SellVehicle(int32 VehicleId);
```

After `UPROPERTY() TObjectPtr<UAgentRescue> AgentRescue;` add `UPROPERTY() TObjectPtr<UFacilityPurchases> FacilityPurchases;`.

`OpsRuntime.cpp`: in the constructor, after the `AgentRescue` lines:

```cpp
	// PURCHASES, the same shape: a pointer, and the owners it commands - the board owns the fleet, the
	// ledger the money. The world hooks are Attach's (they need the actor).
	FacilityPurchases = CreateDefaultSubobject<UFacilityPurchases>(TEXT("FacilityPurchases"));
	FacilityPurchases->JobBoard = JobBoard;
	FacilityPurchases->Ledger = Ledger;
	FacilityPurchases->Pricing = Pricing;
	FacilityPurchases->Clock = Clock;
	FacilityPurchases->Bus = &Bus;
```

After `Unstick` (the function body) add:

```cpp
FFacilityQuote UOpsRuntime::QuoteFacility(FEntityInstanceId Entity) const
{
	return Target != nullptr && Target->Network != nullptr ? FacilityPurchases->Quote(*Target->Network, Entity) : FFacilityQuote();
}

FPurchaseResult UOpsRuntime::BuyModule(FEntityInstanceId Entity, EDepotModule Module)
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Purchase refused: depot %d - no airport attached"), Entity.Index);
		return FPurchaseResult{ EPurchaseRefusal::NotAFacility };
	}
	return FacilityPurchases->BuyModule(*Target->Network, Entity, Module);
}

FPurchaseResult UOpsRuntime::BuyVehicle(FEntityInstanceId Entity, FName TypeCode)
{
	if (Target == nullptr || Target->Network == nullptr)
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Purchase refused: depot %d - no airport attached"), Entity.Index);
		return FPurchaseResult{ EPurchaseRefusal::NotAFacility };
	}
	return FacilityPurchases->BuyVehicle(*Target->Network, Entity, TypeCode);
}

FPurchaseResult UOpsRuntime::SellVehicle(int32 VehicleId)
{
	return FacilityPurchases->SellVehicle(VehicleId);
}
```

In `Attach`, inside the scenario block after `JobBoard->RefillLitresPerMinutePerPump = ...;` add:

```cpp
		FacilityPurchases->ModuleOffers = Scenario->ModuleOffers;
```

- [ ] **Step 5: Wire the bus**

In `WireBus`, directly after the `FNetworkChangedEvent` JobBoard subscription:

```cpp
	// A VEHICLE BOUGHT OR SOLD, A MODULE BOUGHT: the board's candidates changed, and a job waiting on an
	// empty depot meets the new vehicle on this drain's pass - nothing polls (facility-upgrades spec §3).
	// ENFORCED BY: AirportOps.Present.Facility.PurchaseWakesTheBoard
	Bus.Subscribe<FFleetChangedEvent>(EOpsTier::Sim, TEXT("JobBoard"),
		[this](const FFleetChangedEvent&) { Bus.MarkDirty(TEXT("JobBoard")); });
	Bus.Subscribe<FFacilityUpgradedEvent>(EOpsTier::Sim, TEXT("JobBoard"),
		[this](const FFacilityUpgradedEvent&) { Bus.MarkDirty(TEXT("JobBoard")); });
```

and in the PRESENTATION block after the `FNotificationEvent` subscription:

```cpp
	// THE PURCHASE TOASTS - through the notification face every other toast uses, not a delegate of their
	// own (§6 deviation 4: nothing would bind one; the inspector re-reads the quote anyway).
	Bus.Subscribe<FFleetChangedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"), [this](const FFleetChangedEvent& E)
	{
		const FFuelVehicleSpec Spec = JobBoard->SpecFor(E.TypeCode);
		const FString Name = Spec.DisplayName.IsEmpty() ? E.TypeCode.ToString() : Spec.DisplayName.ToString();
		Events->NotifyNotification(FString::Printf(TEXT("%s %s \u2014 %s"),
			E.Change == EFleetChange::Bought ? TEXT("Bought") : TEXT("Sold"), *Name, *Pricing->Format(E.Amount).ToString()));
	});
	Bus.Subscribe<FFacilityUpgradedEvent>(EOpsTier::Presentation, TEXT("OpsEvents"), [this](const FFacilityUpgradedEvent& E)
	{
		const FModuleOffer* Offer = FacilityPurchases->ModuleOffers.Find(E.Module);
		const FString Name = Offer != nullptr ? Offer->DisplayName.ToString() : UEnum::GetValueAsString(E.Module);
		Events->NotifyNotification(FString::Printf(TEXT("Bought %s \u2014 %s"), *Name, *Pricing->Format(E.Amount).ToString()));
	});
```

- [ ] **Step 6: Build and test**

Build (twice if `FacilityWiredTest.cpp` did not compile on the first). Run `-Filter "AirportOps.Present.Facility+AirportOps.Present.Bus+AirportOps.Model.Bus+AirportOps.Model.Facility"`. Expected: all pass, including `EveryEventHasASubscriber` and `EveryEventDescribesItself`. Run `pwsh ./Tools/Check-Architecture.ps1`: rule 31 passes (every Subscribe is in OpsRuntime.cpp).

- [ ] **Step 7: Commit**

```bash
git add -A Plugins/AirportOps
git commit -m "feat(facility): bus events FacilityUpgraded/FleetChanged wake the job board; runtime owns purchases"
```

---

### Task 6: Upkeep - modules and fleet as described ledger lines

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/Ledger.h:156-173`, `Private/Model/Ledger.cpp:106-116`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/FacilityPurchases.h`, `Private/Model/FacilityPurchases.cpp`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp:629-661`
- Modify: `Plugins/AirportOps/Source/AirportOpsTests/Private/LedgerTest.cpp`, `FacilityPurchasesTest.cpp`

**Interfaces:**
- Produces: `struct FUpkeepLine { double Amount; FText What; }` (Ledger.h); `ULedger::PostDailyUpkeep(TConstArrayView<FUpkeepLine> Lines, double Now)`; `UFacilityPurchases::DailyUpkeep(const URoadNetwork&) const -> FFacilityUpkeep`.

- [ ] **Step 1: Write the failing tests**

Append to `LedgerTest.cpp` (before `#endif`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerUpkeepLinesTest,
	"AirportOps.Model.LedgerUpkeepLines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerUpkeepLinesTest::RunTest(const FString& Parameters)
{
	// ONE ENTRY PER DESCRIBED LINE (facility-upgrades spec §3): the finance screen shows where the money
	// went, so "Fleet upkeep" is its own row, not folded into "Upkeep". A zero line writes nothing.
	ULedger* Ledger = NewObject<ULedger>();
	Ledger->Open(10000.0);
	const FUpkeepLine Lines[] = {
		{ 50.0, FText::FromString(TEXT("Upkeep")) },
		{ 0.0, FText::FromString(TEXT("Facility upkeep")) },
		{ 650.0, FText::FromString(TEXT("Fleet upkeep")) } };
	Ledger->PostDailyUpkeep(Lines, 86400.0);
	TestEqual(TEXT("two entries - the zero line wrote nothing"), Ledger->Entries().Num(), 2);
	TestEqual(TEXT("both are Upkeep"), static_cast<int32>(Ledger->Entries().Last().Category), static_cast<int32>(ELedgerCategory::Upkeep));
	TestEqual(TEXT("each described"), Ledger->Entries().Last().What.ToString(), FString(TEXT("Fleet upkeep")));
	TestEqual(TEXT("and charged"), Ledger->Balance(), 10000.0 - 700.0, 1e-6);
	return true;
}
```

Append to `FacilityPurchasesTest.cpp` (before `#endif`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityUpkeepTest, "AirportOps.Model.Facility.UpkeepSumsModulesAndFleet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityUpkeepTest::RunTest(const FString&)
{
	// R6: sheds and vehicles both cost upkeep. Tanks and pumps have no offer and so no upkeep this slice.
	FFacilityFixture F;
	F.Board->AddVehicleForTest(TEXT("FUEL"), F.Depot, EServiceVehicleState::Idle, 0.0);
	F.Board->AddVehicleForTest(TEXT("UTILITY"), F.Depot, EServiceVehicleState::ToJob, 0.0);
	UEntityDefinition* Def = UEntityDefinition::MakeFuelDepotTransient();
	FEntityPlacement Gone;
	Gone.Definition = Def;
	Gone.Anchors = Def->Anchors;
	Gone.Position = FVector2D(40000.0, 0.0);
	Gone.PoseRole = EServiceRole::Fuel;
	Gone.Modules = { EDepotModule::Shed, EDepotModule::Shed };
	F.Net->RemoveEntity(F.Net->PlaceEntity(Gone));

	const FFacilityUpkeep Upkeep = F.Shop->DailyUpkeep(*F.Net);
	TestEqual(TEXT("one live shed at 200 - a removed depot's sheds cost nothing"), Upkeep.Modules, 200.0, 1e-9);
	TestEqual(TEXT("a bowser and a tow, busy or not, 500 + 150"), Upkeep.Fleet, 650.0, 1e-9);
	return true;
}
```

- [ ] **Step 2: Build; verify red** (compile errors: `FUpkeepLine`, `DailyUpkeep`).

- [ ] **Step 3: Implement**

`Ledger.h`, above `UCLASS() class ULedger`:

```cpp
/** One described upkeep entry. A plain struct: it lives for one PostDailyUpkeep call. */
struct FUpkeepLine
{
	/** Positive: what the day costs. <= 0 posts nothing. */
	double Amount = 0.0;
	FText What;
};
```

Replace the `PostDailyUpkeep` declaration (keep its whole comment) with:

```cpp
	void PostDailyUpkeep(double Base, double Now);

	/**
	 * The same beat with DESCRIBED lines - the airport's base upkeep, "Facility upkeep", "Fleet upkeep"
	 * (facility-upgrades spec §3) - one Upkeep entry per positive line, then the RollUp, unconditional for
	 * the reason above. The Base overload forwards here with one line, so the skip-if-zero rule is written
	 * once. ENFORCED BY: AirportOps.Model.LedgerUpkeepLines, AirportOps.Model.LedgerPostDailyUpkeep
	 */
	void PostDailyUpkeep(TConstArrayView<FUpkeepLine> Lines, double Now);
```

`Ledger.cpp`, replace `PostDailyUpkeep` with:

```cpp
void ULedger::PostDailyUpkeep(double Base, double Now)
{
	const FUpkeepLine Line{ Base, NSLOCTEXT("Ledger", "DailyUpkeep", "Upkeep") };
	PostDailyUpkeep(MakeArrayView(&Line, 1), Now);
}

void ULedger::PostDailyUpkeep(TConstArrayView<FUpkeepLine> Lines, double Now)
{
	for (const FUpkeepLine& Line : Lines)
	{
		if (Line.Amount > 0.0)
		{
			Post(Now, ELedgerCategory::Upkeep, -Line.Amount, Line.What);
		}
	}

	// UNCONDITIONAL - see this method's own header comment for why a Base of zero used to
	// (wrongly) skip this too.
	RollUp(Now);
}
```

`FacilityPurchases.h`, after `SellVehicle`:

```cpp
	/**
	 * One day's upkeep: every owned module with an offer on every live facility, and every vehicle on the
	 * board at its row's UpkeepPerDay (R6). Two figures, posted as two described lines by UOpsRuntime.
	 * ENFORCED BY: AirportOps.Model.Facility.UpkeepSumsModulesAndFleet
	 */
	FFacilityUpkeep DailyUpkeep(const URoadNetwork& Network) const;
```

`FacilityPurchases.cpp`, before `#undef`:

```cpp
FFacilityUpkeep UFacilityPurchases::DailyUpkeep(const URoadNetwork& Network) const
{
	FFacilityUpkeep Out;
	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		if (!Entity.bAlive || !Entity.IsDepot())
		{
			continue;
		}
		for (const EDepotModule Module : Entity.Modules)
		{
			if (const FModuleOffer* Offer = ModuleOffers.Find(Module))
			{
				Out.Modules += Offer->UpkeepPerDay;
			}
		}
	}
	if (JobBoard != nullptr)
	{
		for (const FServiceVehicle& Vehicle : JobBoard->GetVehicles())
		{
			Out.Fleet += JobBoard->SpecFor(Vehicle.TypeCode).UpkeepPerDay;
		}
	}
	return Out;
}
```

`OpsRuntime.cpp` `PostDailyUpkeep`: replace `Ledger->PostDailyUpkeep(Base, Clock->Now());` with:

```cpp
	// THE FACILITIES' SHARE, from the one service that knows what a module and a vehicle cost to keep
	// (facility-upgrades spec R6), as lines of their own so the finance screen can say where it went.
	const FFacilityUpkeep Facilities = FacilityPurchases->DailyUpkeep(*Target->Network);
	const FUpkeepLine Lines[] = {
		{ Base, NSLOCTEXT("Ledger", "DailyUpkeep", "Upkeep") },
		{ Facilities.Modules, NSLOCTEXT("Ledger", "FacilityUpkeep", "Facility upkeep") },
		{ Facilities.Fleet, NSLOCTEXT("Ledger", "FleetUpkeep", "Fleet upkeep") } };
	Ledger->PostDailyUpkeep(Lines, Clock->Now());
```

and the final log line becomes (same prefix, so a grep for `Upkeep day` still finds it):

```cpp
	UE_LOG(LogAirportOps, Log, TEXT("Upkeep day %d: %.0f (+%.0f facilities, +%.0f fleet); balance %.0f"),
		Clock->Day(), Base, Facilities.Modules, Facilities.Fleet, Ledger->Balance());
```

- [ ] **Step 4: Build and test**

Run `-Filter "AirportOps.Model.Ledger+AirportOps.Model.Facility+AirportOps.Present"`. Expected: pass (the existing `LedgerPostDailyUpkeep` stays green through the forwarder).

- [ ] **Step 5: Commit**

```bash
git add -A Plugins/AirportOps
git commit -m "feat(facility): daily upkeep for sheds and fleet, as described ledger lines"
```

---

### Task 7: Runtime hooks - reserved slots (memoised) and the module write - with composition tests

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Present/OpsRuntime.h`
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp` (includes, Attach, Detach, new method)
- Modify: `Plugins/AirportOps/Source/AirportOpsTests/Private/FacilityWiredTest.cpp`

**Interfaces:**
- Consumes: `DepotKit::ReservationOf` (Task 3), `URoadEditFacade::AddEntityModule` (Task 3), `ARoadNetworkActor::ResolveDepotKits` (existing, public).
- Produces: `UOpsRuntime::ReservationSolvesForTest() const -> int32`.

- [ ] **Step 1: Write the failing tests**

In `FacilityWiredTest.cpp`'s anonymous namespace add:

```cpp
	/** 50 x 24 m - Task 3's FacilityModuleWidePlot figure, which reserves a second shed. */
	TArray<FVector2D> FacilityWiredWidePlot()
	{
		return { FVector2D(0.0, 0.0), FVector2D(5000.0, 0.0), FVector2D(5000.0, 2400.0), FVector2D(0.0, 2400.0) };
	}

	/** An attached runtime, then the player's gesture: a plotted depot with its start kit, paid for. */
	FEntityInstanceId FacilityWiredDepot(FAirsideTestWorld& World, UOpsRuntime*& OutRuntime)
	{
		ARoadNetworkActor* Actor = World.Actor;
		Actor->PlaceNode(FVector2D(0.0, 40000.0));
		Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();
		OutRuntime = NewObject<UOpsRuntime>();
		OutRuntime->Attach(Actor);
		const TArray<FVector2D> Plot = FacilityWiredWidePlot();
		const int32 Index = Actor->PlaceEntityInPlot(Plot, Plot[0], Plot[1],
			{ EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump }, EPlaceableEntity::FuelDepot);
		return Index != INDEX_NONE ? Actor->Network->EntityIdAt(Index) : FEntityInstanceId();
	}
```

Append tests:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityShedRelightsTest, "AirportOps.Present.Facility.ShedPurchaseRelightsASlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityShedRelightsTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the depot is placed"), Depot.IsSet())) { return false; }

	const FFacilityQuote Before = Runtime->QuoteFacility(Depot);
	if (!TestTrue(TEXT("setup: the runtime's ceiling hook sees a second shed slot"),
		Before.Modules.Num() == 1 && Before.Modules[0].Reserved >= 2)) { return false; }
	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	const int32 Lit = Plots->GetModuleCount();

	const FPurchaseResult Shed = Runtime->BuyModule(Depot, EDepotModule::Shed);
	TestTrue(TEXT("the shed is bought through the runtime's hooks"), Shed.Succeeded());
	TestEqual(TEXT("and one more bay is lit - the module hook reached the facade and the rebuild"), Plots->GetModuleCount(), Lit + 1);
	TestEqual(TEXT("the quote now owns two"), Runtime->QuoteFacility(Depot).Modules[0].Owned, 2);
	TestEqual(TEXT("and has two bays"), Runtime->QuoteFacility(Depot).Bays, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityUndoCheckpointTest, "AirportOps.Present.Facility.ShedClearsUndoVehicleDoesNot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityUndoCheckpointTest::RunTest(const FString&)
{
	// R8: a shed is a checkpoint; a vehicle trade never touches undo.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	URoadEditFacade* Facade = TestWorld.Actor->GetEditFacade();
	if (!TestTrue(TEXT("setup: placing the depot is undoable"), Depot.IsSet() && Facade->CanUndo())) { return false; }

	TestTrue(TEXT("a vehicle is bought"), Runtime->BuyVehicle(Depot, TEXT("FUEL")).Succeeded());
	TestTrue(TEXT("and the build history is untouched"), Facade->CanUndo());
	TestTrue(TEXT("a shed is bought"), Runtime->BuyModule(Depot, EDepotModule::Shed).Succeeded());
	TestFalse(TEXT("and the history is gone - an undo would drop the shed and keep the money"), Facade->CanUndo());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityQuoteMemoTest, "AirportOps.Present.Facility.QuoteSolvesOncePerDepot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityQuoteMemoTest::RunTest(const FString&)
{
	// REVIEW FOCUS 4: the inspector asks every tick; the ceiling is a plot solve. Asked once per depot.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = nullptr;
	const FEntityInstanceId Depot = FacilityWiredDepot(TestWorld, Runtime);
	if (!TestTrue(TEXT("setup: the depot is placed"), Depot.IsSet())) { return false; }
	const int32 Before = Runtime->ReservationSolvesForTest();
	Runtime->QuoteFacility(Depot);
	Runtime->QuoteFacility(Depot);
	Runtime->BuyModule(Depot, EDepotModule::Shed);
	Runtime->QuoteFacility(Depot);
	TestEqual(TEXT("three quotes and a purchase solve the plot once - modules do not change what it holds"),
		Runtime->ReservationSolvesForTest(), Before + 1);
	return true;
}
```

- [ ] **Step 2: Build; verify red** (compile error `ReservationSolvesForTest`; and before the hooks, `Before.Modules[0].Reserved` would be 0).

- [ ] **Step 3: Implement**

`OpsRuntime.h` private section, after `FacilityPurchases`:

```cpp
	/**
	 * UFacilityPurchases::ReservedSlotsOf's production answer: DepotKit::ReservationOf's ceiling over the
	 * actor's one kit table, MEMOISED per (network object, depot) - the inspector re-quotes every tick and
	 * the ceiling is a plot solve. Modules do not change what a plot holds, and a depot never moves, so the
	 * only invalidations are a different network (clear, load, undo replace the object) and Detach.
	 * ENFORCED BY: AirportOps.Present.Facility.QuoteSolvesOncePerDepot
	 */
	int32 ReservedSlotsOf(FEntityInstanceId Id, const FEntityInstance& Depot, EDepotModule Module);
	TWeakObjectPtr<const URoadNetwork> ReservationMemoNetwork;
	TMap<FEntityInstanceId, TArray<int32>> ReservationMemo;
	int32 ReservationSolves = 0;
```

public section, after `OfferTicksForTest`:

```cpp
	/** How many plot solves ReservedSlotsOf has run - see its memo. */
	int32 ReservationSolvesForTest() const { return ReservationSolves; }
```

`OpsRuntime.cpp`: add `#include "Build/DepotKit.h"`. Add:

```cpp
int32 UOpsRuntime::ReservedSlotsOf(FEntityInstanceId Id, const FEntityInstance& Depot, EDepotModule Module)
{
	if (Target == nullptr)
	{
		return 0;
	}
	if (ReservationMemoNetwork.Get() != Target->Network)
	{
		ReservationMemo.Reset();
		ReservationMemoNetwork = Target->Network;
	}
	TArray<int32>* Ceilings = ReservationMemo.Find(Id);
	if (Ceilings == nullptr)
	{
		++ReservationSolves;
		Ceilings = &ReservationMemo.Add(Id);
		const TArray<PlotYard::FKitSpec> Specs = Target->ResolveDepotKits();
		const TOptional<PlotYard::FReservation> Reserved = DepotKit::ReservationOf(Depot, Specs);
		// INDEXED BY EDepotModule: DepotKitSpecs walks the enum, so a spec's index IS its module (its header).
		for (int32 Kit = 0; Kit < Specs.Num(); ++Kit)
		{
			Ceilings->Add(Reserved.IsSet() ? Reserved->CeilingFor(Kit) : 0);
		}
	}
	const int32 Kit = static_cast<int32>(Module);
	return Ceilings->IsValidIndex(Kit) ? (*Ceilings)[Kit] : 0;
}
```

In `Attach`, after `JobBoard->DesignVehicleOf = &UOpsRuntime::StandDesignVehicleOf;` add:

```cpp
	// THE PURCHASE SERVICE'S TWO WORLD HOOKS (facility-upgrades spec §3; UJobBoard::DesignVehicleOf's
	// pattern): the plot's ceiling from Airside's one solve, and the module write through the facade's one
	// door - which rebuilds the yard and checkpoints undo. `this` for the ceiling, which the runtime memoises
	// and outlives nothing; weak for the actor, the dispatcher's reason below.
	// ENFORCED BY: AirportOps.Present.Facility.ShedPurchaseRelightsASlot
	FacilityPurchases->ReservedSlotsOf = [this](FEntityInstanceId Id, const FEntityInstance& Depot, EDepotModule Module)
	{
		return ReservedSlotsOf(Id, Depot, Module);
	};
	{
		TWeakObjectPtr<ARoadNetworkActor> WeakActor = Target;
		FacilityPurchases->ApplyModulePurchase = [WeakActor](FEntityInstanceId Id, EDepotModule Module)
		{
			ARoadNetworkActor* Actor = WeakActor.Get();
			URoadEditFacade* Facade = Actor != nullptr ? Actor->GetEditFacade() : nullptr;
			return Facade != nullptr && Facade->AddEntityModule(Id, Module);
		};
	}
```

In `Detach`, beside `FlightBoard->Dispatcher = nullptr;` add:

```cpp
	// THE PURCHASE HOOKS GO WITH THE ACTOR, the dispatcher's reason: a hook that still answered after a
	// detach would build into a field this runtime no longer drives.
	FacilityPurchases->ReservedSlotsOf = nullptr;
	FacilityPurchases->ApplyModulePurchase = nullptr;
	ReservationMemo.Reset();
	ReservationMemoNetwork.Reset();
```

- [ ] **Step 4: Build and test**

Run `-Filter "AirportOps.Present.Facility+AirportOps.Present.Bus+AirportOps.Present.Ledger"` then the full suite. Expected: `0 failed, 0 crashed`.

- [ ] **Step 5: Commit**

```bash
git add -A Plugins/AirportOps
git commit -m "feat(facility): runtime hooks - memoised slot ceiling, module purchase through the facade"
```

---

### Task 8: Inspector depot card, three inspector-only actions, controller verbs

**Files:**
- Modify: `Source/AirportMgr/BuildActions.h:95-128`, `BuildActions.cpp:207-210`
- Modify: `Source/AirportMgr/BuildBarWidget.cpp:341-348`
- Modify: `Source/AirportMgr/RoadBuildController.h:520-531, ~876`, `RoadBuildController.cpp:788`
- Modify: `Source/AirportMgr/InspectorWidget.h`, `InspectorWidget.cpp`
- Modify tests: `Source/AirportMgr/BuildActionsTest.cpp`, `BuildBarWidgetTest.cpp:32-38`, `UIStyleTest.cpp:110-116`, `InspectorWidgetTest.cpp`

**Interfaces:**
- Consumes: `UOpsRuntime::QuoteFacility/BuyModule/BuyVehicle/SellVehicle` (Task 5), `FFacilityQuote` & friends, `UFacilityPurchases::RefusalText` (Task 4).
- Produces: `FBuildAction::bInspectorOnly`; action ids `selection.buy_module`, `selection.buy_vehicle`, `selection.sell_vehicle`; controller `SelectedFacility`, `QuoteSelectedFacility`, `CanBuySelectedModule/BuySelectedModule`, `ChooseVehicleToBuy/CanBuyChosenVehicle/BuyChosenVehicle`, `ArmSellVehicle/CanSellArmedVehicle/SellArmedVehicle`; inspector `ShowFacilityQuote`, `DepotStatus`, BindWidgetOptional `ShedsRow, ShedsText, BuyModuleButton, VehiclesRow, VehiclesText, BuyVehicleMenu, FleetList`; `UInspectorFleetRow`.

- [ ] **Step 1: Write the failing tests**

`BuildActionsTest.cpp` - append a test:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFacilityVerbsRegisteredTest,
	"AirportMgr.Actions.FacilityVerbsRegistered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFacilityVerbsRegisteredTest::RunTest(const FString& Parameters)
{
	// BY NAME (CLAUDE.md "lists that must agree"): the inspector finds these rows by id, so a rename here
	// would leave the depot card with dead buttons and nothing red.
	for (const TCHAR* Id : { TEXT("selection.buy_module"), TEXT("selection.buy_vehicle"), TEXT("selection.sell_vehicle") })
	{
		const FBuildAction* Action = FindAction(FName(Id));
		if (!TestNotNull(*FString::Printf(TEXT("%s is registered"), Id), Action)) { continue; }
		TestEqual(*FString::Printf(TEXT("%s is a Selection verb"), Id), Action->Section, EActionSection::Selection);
		TestFalse(*FString::Printf(TEXT("%s has no key - a key that spent money on whatever was selected is a misclick"), Id), Action->Key.IsValid());
		TestTrue(*FString::Printf(TEXT("%s is inspector-only - acting on a selection belongs to the inspector"), Id), Action->bInspectorOnly);
	}
	return true;
}
```

`BuildBarWidgetTest.cpp` (lines 32-38): change the Expected count to skip inspector-only rows:

```cpp
		for (const FBuildAction& A : BuildActions()) { if (A.Section == Section && !A.bInspectorOnly) { ++Expected; } }
```

and after the loop add:

```cpp
	// THE THREE PURCHASE VERBS ARE ROWS, NOT BUTTONS: registered, and still one bar button fewer each.
	int32 InspectorOnly = 0;
	for (const FBuildAction& A : BuildActions()) { InspectorOnly += A.bInspectorOnly ? 1 : 0; }
	TestEqual(TEXT("three inspector-only rows exist, and the per-section counts above drew none of them"), InspectorOnly, 3);
```

`UIStyleTest.cpp` (inside the loop, line ~110): add to the skip condition `|| Action.bInspectorOnly` with the comment `// INSPECTOR-ONLY rows draw no bar button, so they need no icon (facility spec §6 deviation 8).`

`InspectorWidgetTest.cpp` - add `#include "Model/FacilityPurchases.h"` and append:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorFacilityCardTest,
	"AirportMgr.Inspector.FacilityCardRendersTheQuote",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorFacilityCardTest::RunTest(const FString& Parameters)
{
	// THE CARD RENDERS ONLY THE QUOTE (spec §4): every caption, enabled state and reason below comes from
	// the struct handed in - nothing is computed here, so a button cannot disagree with the rules.
	FAirsideTestWorld Bare(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), Bare.World)) { return false; }
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(Bare.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panel is created with no asset"), Panel)) { return false; }
	TestNotNull(TEXT("the code-built card has a buy-module button"), Panel->BuyModuleButton.Get());
	TestNotNull(TEXT("and a buy-vehicle menu"), Panel->BuyVehicleMenu.Get());

	FFacilityQuote Quote;
	Quote.Refusal = EPurchaseRefusal::None;
	Quote.Bays = 1;
	Quote.Vehicles = 1;
	FModuleOfferQuote& Shed = Quote.Modules.AddDefaulted_GetRef();
	Shed.Module = EDepotModule::Shed;
	Shed.Name = FText::FromString(TEXT("Shed"));
	Shed.PluralName = FText::FromString(TEXT("Sheds"));
	Shed.Owned = 1;
	Shed.Reserved = 3;
	Shed.Refusal = EPurchaseRefusal::CannotAfford;
	Shed.Label = FText::FromString(TEXT("Buy Shed 40,000"));
	for (const TCHAR* Code : { TEXT("FUEL"), TEXT("UTILITY") })
	{
		FVehicleOfferQuote& Offer = Quote.VehicleOffers.AddDefaulted_GetRef();
		Offer.TypeCode = Code;
		Offer.Refusal = EPurchaseRefusal::NoFreeBay;
		Offer.Label = FText::FromString(Code);
	}
	FFleetRowQuote& Row = Quote.Fleet.AddDefaulted_GetRef();
	Row.VehicleId = 7;
	Row.Line = TEXT("FUEL #7 · to stand 2 · 10,000 L");
	Row.Refusal = EPurchaseRefusal::VehicleBusy;
	Row.SellLabel = FText::FromString(TEXT("Sell 45,000"));

	Panel->ShowFacilityQuote(Quote);
	TestTrue(TEXT("a facility's rows are shown"), Panel->AreFacilityRowsShownForTest());
	TestEqual(TEXT("the shed line counts owned against reserved"), Panel->ShedsTextForTest(), FString(TEXT("Sheds 1 / 3 space")));
	TestFalse(TEXT("an unaffordable shed is a disabled button"), Panel->IsBuyModuleEnabledForTest());
	TestTrue(TEXT("whose caption says why"), Panel->BuyModuleCaptionForTest().Contains(UFacilityPurchases::RefusalText(EPurchaseRefusal::CannotAfford).ToString()));
	TestEqual(TEXT("the vehicle line counts vehicles against bays"), Panel->VehiclesTextForTest(), FString(TEXT("Vehicles 1 / 1 bays")));
	const TArray<FUiMenuItem> Items = Panel->BuyVehicleItemsForTest();
	if (TestEqual(TEXT("one menu line per vehicle offer"), Items.Num(), 2))
	{
		TestFalse(TEXT("a full depot greys every line"), Items[0].bEnabled);
		TestTrue(TEXT("with the refusal as its reason"), Items[0].Why.EqualTo(UFacilityPurchases::RefusalText(EPurchaseRefusal::NoFreeBay)));
	}
	TestEqual(TEXT("one fleet row per vehicle"), Panel->FleetRowCountForTest(), 1);
	TestFalse(TEXT("a busy vehicle's Sell is disabled"), Panel->IsSellEnabledForTest(0));

	Panel->ShowFacilityQuote(FFacilityQuote());
	TestFalse(TEXT("a card that is no facility shows no purchase rows"), Panel->AreFacilityRowsShownForTest());

	FFacilityQuote Empty = Quote;
	Empty.Vehicles = 0;
	TestEqual(TEXT("an empty depot on a road says what to do"),
		UInspectorWidget::DepotStatus(Empty, /*bReachable=*/true, TEXT("No jobs")), FString(TEXT("No vehicles \u2014 buy one")));
	TestEqual(TEXT("off the road, the road is the fix it names"),
		UInspectorWidget::DepotStatus(Empty, /*bReachable=*/false, TEXT("Cannot dispatch")), FString(TEXT("Cannot dispatch")));
	TestEqual(TEXT("with a vehicle, the backlog stands"),
		UInspectorWidget::DepotStatus(Quote, /*bReachable=*/true, TEXT("No jobs")), FString(TEXT("No jobs")));
	return true;
}
```

- [ ] **Step 2: Build; verify red** (compile errors for every new name).

- [ ] **Step 3: Implement the action flag and rows**

`BuildActions.h` `FBuildAction`, after `DynamicLabel`:

```cpp
	/**
	 * Run from the inspector's card only - no bar button, so no icon (facility-upgrades spec §4: "nothing on
	 * the bottom bar: acting on a selection belongs to the inspector"). Still a row, so the card's clicks go
	 * through TryRun's one door and its log line.
	 * ENFORCED BY: AirportMgr.Actions.FacilityVerbsRegistered, AirportMgr.Actions.BarBuildsFromRegistry
	 */
	bool bInspectorOnly = false;
```

`BuildActions.cpp`, after the `selection.unstick` row:

```cpp
		// FACILITY PURCHASES (spec 2026-09-29-facility-upgrades §4): the depot card's three verbs, INSPECTOR
		// ONLY. Buy-vehicle and sell carry an argument a row cannot: the card CHOOSES the type / ARMS the
		// vehicle on the controller, then runs the row - so a sale takes two clicks (a destructive gesture
		// needs a deliberate second one - memory). Keyless: a key that spent money on whatever was selected
		// is a misclick.
		{
			FBuildAction Module = Make(TEXT("selection.buy_module"), EActionSection::Selection,
				LOCTEXT("BuyModule", "Buy module"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { Ctx.Controller.BuySelectedModule(); }, Never,
				[](const FBuildActionContext& Ctx) { return Ctx.Controller.CanBuySelectedModule(); });
			Module.bInspectorOnly = true;
			Out.Add(MoveTemp(Module));

			FBuildAction Vehicle = Make(TEXT("selection.buy_vehicle"), EActionSection::Selection,
				LOCTEXT("BuyVehicle", "Buy vehicle"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { Ctx.Controller.BuyChosenVehicle(); }, Never,
				[](const FBuildActionContext& Ctx) { return Ctx.Controller.CanBuyChosenVehicle(); });
			Vehicle.bInspectorOnly = true;
			Out.Add(MoveTemp(Vehicle));

			FBuildAction Sell = Make(TEXT("selection.sell_vehicle"), EActionSection::Selection,
				LOCTEXT("SellVehicle", "Sell vehicle"), EKeys::Invalid, false,
				[](FBuildActionContext& Ctx) { Ctx.Controller.SellArmedVehicle(); }, Never,
				[](const FBuildActionContext& Ctx) { return Ctx.Controller.CanSellArmedVehicle(); });
			Sell.bInspectorOnly = true;
			Out.Add(MoveTemp(Sell));
		}
```

`BuildBarWidget.cpp` `BuildButtons`, after `if (Panel == nullptr) { continue; }`:

```cpp
		// INSPECTOR-ONLY rows have no bar button - see FBuildAction::bInspectorOnly.
		if (Action.bInspectorOnly)
		{
			continue;
		}
```

- [ ] **Step 4: Implement the controller verbs**

`RoadBuildController.h`: add `#include "Model/FacilityPurchases.h"` beside the AgentRescue include. After `GetUnstickMenuRequests()`:

```cpp
	/**
	 * THE DEPOT CARD'S VERBS (facility-upgrades spec §4) - FORWARDERS to UOpsRuntime with the selected
	 * depot, the Unstick verbs' shape. The quote is asked fresh on every call, so an enabled check and the
	 * command it guards read the same state. Refused (logged) with no depot selected or no runtime.
	 */
	FEntityInstanceId SelectedFacility() const;
	FFacilityQuote QuoteSelectedFacility() const;
	/** The quote's FIRST module offer - the only one this slice (the shed). A second becomes a menu. */
	bool CanBuySelectedModule() const;
	void BuySelectedModule();
	/** The card's buy menu chooses, then runs selection.buy_vehicle - the row cannot carry the type. */
	void ChooseVehicleToBuy(FName TypeCode) { ChosenVehicleType = TypeCode; }
	bool CanBuyChosenVehicle() const;
	void BuyChosenVehicle();
	/** The fleet row's first click arms, its second runs selection.sell_vehicle. 0 disarms. */
	void ArmSellVehicle(int32 VehicleId) { ArmedSellVehicle = VehicleId; }
	int32 GetArmedSellVehicle() const { return ArmedSellVehicle; }
	bool CanSellArmedVehicle() const;
	void SellArmedVehicle();
```

private, after `UnstickMenuRequests`:

```cpp
	/** See ChooseVehicleToBuy / ArmSellVehicle. Session state, never saved. */
	FName ChosenVehicleType;
	int32 ArmedSellVehicle = 0;
```

`RoadBuildController.cpp`, after `UnstickSelected`:

```cpp
FEntityInstanceId ARoadBuildController::SelectedFacility() const
{
	const URoadNetwork* Net = Target != nullptr ? Target->GetNetwork() : nullptr;
	if (Net == nullptr || GetSelection().Kind != ESelectionKind::Stand)
	{
		return FEntityInstanceId();
	}
	const FEntityInstanceId Id = Net->EntityIdAt(GetSelection().Id);
	const FEntityInstance* Entity = Net->GetEntity(Id);
	return Entity != nullptr && Entity->bAlive && Entity->IsDepot() ? Id : FEntityInstanceId();
}

FFacilityQuote ARoadBuildController::QuoteSelectedFacility() const
{
	const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	const FEntityInstanceId Id = SelectedFacility();
	return Runtime != nullptr && Id.IsSet() ? Runtime->QuoteFacility(Id) : FFacilityQuote();
}

bool ARoadBuildController::CanBuySelectedModule() const
{
	const FFacilityQuote Quote = QuoteSelectedFacility();
	return Quote.Modules.Num() > 0 && Quote.Modules[0].Refusal == EPurchaseRefusal::None;
}

void ARoadBuildController::BuySelectedModule()
{
	UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	const FFacilityQuote Quote = QuoteSelectedFacility();
	if (Runtime == nullptr || Quote.Modules.Num() == 0)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("Buy module: no depot selected, or no ops runtime."));
		return;
	}
	// UFacilityPurchases logs the "Purchase: ..." line; this one says the click arrived.
	UE_LOG(LogRoadBuild, Log, TEXT("Buy module %s: depot %d"), *UEnum::GetValueAsString(Quote.Modules[0].Module), SelectedFacility().Index);
	Runtime->BuyModule(SelectedFacility(), Quote.Modules[0].Module);
}

bool ARoadBuildController::CanBuyChosenVehicle() const
{
	const FFacilityQuote Quote = QuoteSelectedFacility();
	const FName Chosen = ChosenVehicleType;
	const FVehicleOfferQuote* Offer = Quote.VehicleOffers.FindByPredicate([Chosen](const FVehicleOfferQuote& O) { return O.TypeCode == Chosen; });
	return Offer != nullptr && Offer->Refusal == EPurchaseRefusal::None;
}

void ARoadBuildController::BuyChosenVehicle()
{
	UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	if (Runtime == nullptr || !SelectedFacility().IsSet() || ChosenVehicleType.IsNone())
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("Buy vehicle: no depot selected, no type chosen, or no ops runtime."));
		return;
	}
	UE_LOG(LogRoadBuild, Log, TEXT("Buy vehicle %s: depot %d"), *ChosenVehicleType.ToString(), SelectedFacility().Index);
	Runtime->BuyVehicle(SelectedFacility(), ChosenVehicleType);
	ChosenVehicleType = NAME_None;
}

bool ARoadBuildController::CanSellArmedVehicle() const
{
	const FFacilityQuote Quote = QuoteSelectedFacility();
	const int32 Armed = ArmedSellVehicle;
	const FFleetRowQuote* Row = Quote.Fleet.FindByPredicate([Armed](const FFleetRowQuote& R) { return R.VehicleId == Armed; });
	return Row != nullptr && Row->Refusal == EPurchaseRefusal::None;
}

void ARoadBuildController::SellArmedVehicle()
{
	UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	if (Runtime == nullptr || ArmedSellVehicle == 0)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("Sell vehicle: nothing armed, or no ops runtime."));
		return;
	}
	UE_LOG(LogRoadBuild, Log, TEXT("Sell vehicle %d"), ArmedSellVehicle);
	Runtime->SellVehicle(ArmedSellVehicle);
	ArmedSellVehicle = 0;
}
```

- [ ] **Step 5: Implement the inspector**

`InspectorWidget.h`: add `#include "Model/FacilityPurchases.h"`; forward-declare `class UPanelWidget;` and `class UInspectorWidget;`. Before `UInspectorWidget`'s UCLASS add:

```cpp
/**
 * One fleet row's Sell click - UBuildBarEntry's reason: a dynamic delegate binds only to a UFUNCTION on a
 * UObject. The first click ARMS (the caption asks again), the second sells: selling cannot be undone
 * (memory: destructive gestures need a deliberate mode).
 */
UCLASS()
class AIRPORTMGR_API UInspectorFleetRow : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 VehicleId = 0;
	UPROPERTY() TObjectPtr<UTextBlock> Line;
	UPROPERTY() TObjectPtr<UUiButton> SellButton;
	UPROPERTY() TWeakObjectPtr<UInspectorWidget> Owner;
	bool bArmed = false;

	UFUNCTION() void HandleSell();
};
```

In `UInspectorWidget` public, after `UnstickMenu`:

```cpp
	/**
	 * THE DEPOT CARD'S PURCHASE ROWS (facility-upgrades spec §4), all filled from ShowFacilityQuote. The C++
	 * base builds them asset-free; a Blueprint restyles through these names.
	 */
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> ShedsRow;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ShedsText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UUiButton> BuyModuleButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> VehiclesRow;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> VehiclesText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UUiMenuButton> BuyVehicleMenu;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> FleetList;

	/**
	 * Render the purchase rows from Quote and NOTHING ELSE - the spec's rule that the card cannot disagree
	 * with the rules. A quote that is no facility collapses them. Public so a headless test (no runtime,
	 * no controller) can drive it; Refresh calls it every tick with the selected depot's quote.
	 * ENFORCED BY: AirportMgr.Inspector.FacilityCardRendersTheQuote
	 */
	void ShowFacilityQuote(const FFacilityQuote& Quote);

	/** The depot status line: "No vehicles — buy one" for an empty depot on a road, else Current. */
	static FString DepotStatus(const FFacilityQuote& Quote, bool bReachable, const FString& Current);

	/** A fleet row's click - see UInspectorFleetRow. */
	void OnFleetSell(UInspectorFleetRow& Row);

	bool AreFacilityRowsShownForTest() const;
	FString ShedsTextForTest() const;
	FString VehiclesTextForTest() const;
	bool IsBuyModuleEnabledForTest() const;
	FString BuyModuleCaptionForTest() const;
	TArray<FUiMenuItem> BuyVehicleItemsForTest() const { return BuyVehicleItems(); }
	int32 FleetRowCountForTest() const { return FleetRows.Num(); }
	bool IsSellEnabledForTest(int32 Row) const;
```

private, after `UnstickActionIndex`:

```cpp
	/** By id, RunwayActionIndex's rule - and caught BEFORE the positional Depart/Follow pair, or a new
	 *  Selection row would shift it. */
	int32 BuyModuleActionIndex = INDEX_NONE;
	int32 BuyVehicleActionIndex = INDEX_NONE;
	int32 SellVehicleActionIndex = INDEX_NONE;

	/** The quote the rows were last drawn from - the buy menu's lines are asked of it as it opens. */
	FFacilityQuote LastQuote;
	/** The fleet's vehicle ids last drawn; the rows are rebuilt only when this changes. */
	FString LastFleetKey;
	/** The type codes the open buy menu lists, by line - HandleBuyVehicleChosen reads the line's code. */
	mutable TArray<FName> ShownVehicleCodes;
	UPROPERTY() TArray<TObjectPtr<UInspectorFleetRow>> FleetRows;

	TArray<FUiMenuItem> BuyVehicleItems() const;
	void RebuildFleetRows();
	UFUNCTION() void HandleBuyModule();
	UFUNCTION() void HandleBuyVehicleChosen(int32 Index);
```

`InspectorWidget.cpp`:

(a) `EnsureSlots` loop - insert BEFORE the positional `if (SelectionSeen == 0)` line:

```cpp
		if (Actions[Index].Id == FName(TEXT("selection.buy_module"))) { BuyModuleActionIndex = Index; continue; }
		if (Actions[Index].Id == FName(TEXT("selection.buy_vehicle"))) { BuyVehicleActionIndex = Index; continue; }
		if (Actions[Index].Id == FName(TEXT("selection.sell_vehicle"))) { SellVehicleActionIndex = Index; continue; }
```

(b) end of `EnsureSlots` (after the Unstick warning):

```cpp
	// THE PURCHASE ROWS, built empty; ShowFacilityQuote fills them from the one quote.
	auto Panel = [&](TObjectPtr<UPanelWidget>& Field, UClass* Class, const TCHAR* Name)
	{
		if (Field != nullptr || Column == nullptr) { return; }
		Field = WidgetTree->ConstructWidget<UPanelWidget>(Class, Name);
		Column->AddChildToVerticalBox(Field)->SetPadding(FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		Field->SetVisibility(ESlateVisibility::Collapsed);
	};
	auto RowText = [&](TObjectPtr<UTextBlock>& Field, const TCHAR* Name, UPanelWidget* Into)
	{
		if (Field != nullptr) { return; }
		Field = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
		Style->ApplyText(*Field, EUITextRole::Body, Style->InkMuted);
		if (UHorizontalBox* Box = Cast<UHorizontalBox>(Into)) { Box->AddChildToHorizontalBox(Field)->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f)); }
	};
	Panel(ShedsRow, UHorizontalBox::StaticClass(), TEXT("ShedsRow"));
	RowText(ShedsText, TEXT("ShedsText"), ShedsRow);
	if (BuyModuleButton == nullptr && Actions.IsValidIndex(BuyModuleActionIndex))
	{
		BuyModuleButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), TEXT("BuyModuleButton"));
		BuyModuleButton->SetLabel(Actions[BuyModuleActionIndex].Label);
		BuyModuleButton->Build(*Style, EUiButtonKind::Secondary);
		if (UHorizontalBox* Box = Cast<UHorizontalBox>(ShedsRow)) { Box->AddChildToHorizontalBox(BuyModuleButton); }
	}
	Panel(VehiclesRow, UHorizontalBox::StaticClass(), TEXT("VehiclesRow"));
	RowText(VehiclesText, TEXT("VehiclesText"), VehiclesRow);
	if (BuyVehicleMenu == nullptr && Actions.IsValidIndex(BuyVehicleActionIndex))
	{
		BuyVehicleMenu = WidgetTree->ConstructWidget<UUiMenuButton>(UUiMenuButton::StaticClass(), TEXT("BuyVehicleMenu"));
		BuyVehicleMenu->Build(*Style, Actions[BuyVehicleActionIndex].Label);
		if (UHorizontalBox* Box = Cast<UHorizontalBox>(VehiclesRow)) { Box->AddChildToHorizontalBox(BuyVehicleMenu); }
	}
	Panel(FleetList, UVerticalBox::StaticClass(), TEXT("FleetList"));
	for (const int32 Found : { BuyModuleActionIndex, BuyVehicleActionIndex, SellVehicleActionIndex })
	{
		if (!Actions.IsValidIndex(Found))
		{
			UE_LOG(LogInspector, Warning, TEXT("A selection.buy_module/buy_vehicle/sell_vehicle row is missing from BuildActions(): the depot card cannot buy or sell"));
			break;
		}
	}
```

(c) `BuildOnce`, after the UnstickMenu binding:

```cpp
	if (BuyModuleButton != nullptr) { BuyModuleButton->OnClicked.AddDynamic(this, &UInspectorWidget::HandleBuyModule); }
	if (BuyVehicleMenu != nullptr)
	{
		TWeakObjectPtr<UInspectorWidget> WeakSelf(this);
		BuyVehicleMenu->Items = [WeakSelf]() { return WeakSelf.IsValid() ? WeakSelf->BuyVehicleItems() : TArray<FUiMenuItem>(); };
		BuyVehicleMenu->OnChosen.AddDynamic(this, &UInspectorWidget::HandleBuyVehicleChosen);
	}
```

(d) `Refresh`: in the `bNewSelection` block add `LastFleetKey = TEXT("!");` (forces the next depot's rows to rebuild, disarming any sale) and `if (BuyVehicleMenu != nullptr) { BuyVehicleMenu->Close(); }`. Declare `FFacilityQuote CardQuote;` beside `FString Title, Facts, Status;`. In the depot `else` branch, after the backlog block:

```cpp
			// THE PURCHASE ROWS AND "NO VEHICLES", from the one quote (facility-upgrades spec §4).
			if (const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld()))
			{
				CardQuote = Runtime->QuoteFacility(Target->GetNetwork()->EntityIdAt(Selection.Id));
			}
			Status = DepotStatus(CardQuote, S.bReachable, Status);
```

and just before `SetShown(true);` at the end:

```cpp
	// EVERY CARD SAYS whether it has purchase rows - a non-depot's default quote collapses them.
	ShowFacilityQuote(CardQuote);
```

(e) New functions:

```cpp
FString UInspectorWidget::DepotStatus(const FFacilityQuote& Quote, bool bReachable, const FString& Current)
{
	// OFF THE ROAD, THE ROAD IS THE FIX, and a depot with a vehicle keeps its backlog line.
	if (!bReachable || !Quote.IsFacility() || Quote.Vehicles > 0)
	{
		return Current;
	}
	return NSLOCTEXT("AirportMgr", "InspectorDepotNoVehicles", "No vehicles \u2014 buy one").ToString();
}

void UInspectorWidget::ShowFacilityQuote(const FFacilityQuote& Quote)
{
	LastQuote = Quote;
	const bool bCard = Quote.IsFacility();
	auto Show = [](UWidget* Widget, bool bShow)
	{
		const ESlateVisibility Wanted = bShow ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
		if (Widget != nullptr && Widget->GetVisibility() != Wanted) { Widget->SetVisibility(Wanted); }
	};
	// COMPARED FIRST: SetText has no early-out, and this runs every tick (Refresh's own gate, same reason).
	auto SetIfChanged = [](UTextBlock* Block, const FString& Text)
	{
		if (Block != nullptr && Block->GetText().ToString() != Text) { Block->SetText(FText::FromString(Text)); }
	};
	Show(ShedsRow, bCard && Quote.Modules.Num() > 0);
	Show(VehiclesRow, bCard);
	Show(FleetList, bCard && Quote.Fleet.Num() > 0);

	if (bCard && Quote.Modules.Num() > 0)
	{
		const FModuleOfferQuote& Module = Quote.Modules[0];
		SetIfChanged(ShedsText, FString::Printf(TEXT("%s %d / %d space"), *Module.PluralName.ToString(), Module.Owned, Module.Reserved));
		if (BuyModuleButton != nullptr)
		{
			const bool bCan = Module.Refusal == EPurchaseRefusal::None;
			const FText Caption = bCan ? Module.Label
				: FText::Format(NSLOCTEXT("AirportMgr", "InspectorRefusedCaption", "{0} - {1}"), Module.Label, UFacilityPurchases::RefusalText(Module.Refusal));
			const UTextBlock* Current = BuyModuleButton->GetLabel();
			if (Current == nullptr || !Current->GetText().EqualTo(Caption)) { BuyModuleButton->SetLabel(Caption); }
			BuyModuleButton->SetState(bCan, false);
		}
	}
	if (bCard)
	{
		SetIfChanged(VehiclesText, FString::Printf(TEXT("Vehicles %d / %d bays"), Quote.Vehicles, Quote.Bays));
		if (BuyVehicleMenu != nullptr && BuyVehicleMenu->GetButton() != nullptr)
		{
			BuyVehicleMenu->GetButton()->SetState(Quote.VehicleOffers.Num() > 0, false);
		}
	}

	FString Key;
	for (const FFleetRowQuote& Row : Quote.Fleet) { Key += FString::Printf(TEXT("%d,"), Row.VehicleId); }
	if (Key != LastFleetKey)
	{
		LastFleetKey = Key;
		RebuildFleetRows();
	}
	for (int32 Index = 0; Index < FleetRows.Num() && Index < Quote.Fleet.Num(); ++Index)
	{
		UInspectorFleetRow* Row = FleetRows[Index];
		const FFleetRowQuote& Facts = Quote.Fleet[Index];
		SetIfChanged(Row->Line, Facts.Line);
		const bool bCan = Facts.Refusal == EPurchaseRefusal::None;
		if (!bCan) { Row->bArmed = false; }
		const FText Caption = !bCan ? UFacilityPurchases::RefusalText(Facts.Refusal)
			: Row->bArmed ? FText::Format(NSLOCTEXT("AirportMgr", "InspectorSellConfirm", "{0} - click again"), Facts.SellLabel)
			: Facts.SellLabel;
		const UTextBlock* Current = Row->SellButton->GetLabel();
		if (Current == nullptr || !Current->GetText().EqualTo(Caption)) { Row->SellButton->SetLabel(Caption); }
		Row->SellButton->SetState(bCan, Row->bArmed);
	}
}

void UInspectorWidget::RebuildFleetRows()
{
	if (FleetList != nullptr) { FleetList->ClearChildren(); }
	FleetRows.Reset();
	// A REBUILD DISARMS: the armed vehicle may be the one that just left the list.
	if (ARoadBuildController* C = Controller()) { C->ArmSellVehicle(0); }
	if (FleetList == nullptr || PanelStyle == nullptr) { return; }
	for (const FFleetRowQuote& Facts : LastQuote.Fleet)
	{
		UInspectorFleetRow* Row = NewObject<UInspectorFleetRow>(this);
		Row->VehicleId = Facts.VehicleId;
		Row->Owner = this;
		UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Row->Line = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		PanelStyle->ApplyText(*Row->Line, EUITextRole::Body, PanelStyle->InkMuted);
		Row->Line->SetText(FText::FromString(Facts.Line));
		Box->AddChildToHorizontalBox(Row->Line)->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
		Row->SellButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
		Row->SellButton->SetLabel(Facts.SellLabel);
		Row->SellButton->Build(*PanelStyle, EUiButtonKind::Secondary);
		Row->SellButton->OnClicked.AddDynamic(Row, &UInspectorFleetRow::HandleSell);
		Box->AddChildToHorizontalBox(Row->SellButton);
		FleetList->AddChild(Box);
		FleetRows.Add(Row);
	}
}

void UInspectorFleetRow::HandleSell()
{
	if (UInspectorWidget* Panel = Owner.Get())
	{
		Panel->OnFleetSell(*this);
	}
}

void UInspectorWidget::OnFleetSell(UInspectorFleetRow& Row)
{
	ARoadBuildController* C = Controller();
	if (C == nullptr)
	{
		UE_LOG(LogInspector, Warning, TEXT("Sell click on vehicle %d ignored: no controller"), Row.VehicleId);
		return;
	}
	if (!Row.bArmed)
	{
		for (UInspectorFleetRow* Other : FleetRows) { if (Other != nullptr) { Other->bArmed = false; } }
		Row.bArmed = true;
		C->ArmSellVehicle(Row.VehicleId);
		return;
	}
	Row.bArmed = false;
	RunAction(SellVehicleActionIndex);
}

TArray<FUiMenuItem> UInspectorWidget::BuyVehicleItems() const
{
	TArray<FUiMenuItem> Out;
	ShownVehicleCodes.Reset();
	for (const FVehicleOfferQuote& Offer : LastQuote.VehicleOffers)
	{
		FUiMenuItem& Item = Out.AddDefaulted_GetRef();
		Item.Label = Offer.Label;
		Item.bEnabled = Offer.Refusal == EPurchaseRefusal::None;
		Item.Why = UFacilityPurchases::RefusalText(Offer.Refusal);
		ShownVehicleCodes.Add(Offer.TypeCode);
	}
	return Out;
}

void UInspectorWidget::HandleBuyVehicleChosen(int32 Index)
{
	ARoadBuildController* C = Controller();
	if (C == nullptr || !ShownVehicleCodes.IsValidIndex(Index))
	{
		UE_LOG(LogInspector, Warning, TEXT("Buy vehicle line %d ignored: no controller or no such line"), Index);
		return;
	}
	C->ChooseVehicleToBuy(ShownVehicleCodes[Index]);
	RunAction(BuyVehicleActionIndex);
}

void UInspectorWidget::HandleBuyModule() { RunAction(BuyModuleActionIndex); }

bool UInspectorWidget::AreFacilityRowsShownForTest() const
{
	return VehiclesRow != nullptr && VehiclesRow->GetVisibility() == ESlateVisibility::Visible;
}
FString UInspectorWidget::ShedsTextForTest() const { return ShedsText != nullptr ? ShedsText->GetText().ToString() : FString(); }
FString UInspectorWidget::VehiclesTextForTest() const { return VehiclesText != nullptr ? VehiclesText->GetText().ToString() : FString(); }
bool UInspectorWidget::IsBuyModuleEnabledForTest() const { return BuyModuleButton != nullptr && BuyModuleButton->GetIsEnabled(); }
FString UInspectorWidget::BuyModuleCaptionForTest() const
{
	const UTextBlock* Caption = BuyModuleButton != nullptr ? BuyModuleButton->GetLabel() : nullptr;
	return Caption != nullptr ? Caption->GetText().ToString() : FString();
}
bool UInspectorWidget::IsSellEnabledForTest(int32 Row) const
{
	return FleetRows.IsValidIndex(Row) && FleetRows[Row]->SellButton != nullptr && FleetRows[Row]->SellButton->GetIsEnabled();
}
```

(`UUiButton::SetState` sets IsEnabled - its header: "Enabled + selected -> fill, ink, IsEnabled".) Add `#include "Model/FacilityPurchases.h"` (via the header) and `#include "Components/PanelWidget.h"` to the .cpp.

- [ ] **Step 6: Build and test**

New UCLASS and UPROPERTYs: full build (not Live Coding). Run `-Filter "AirportMgr.Actions+AirportMgr.Inspector+AirportMgr.UI"`, then the full suite. Expected: `0 failed, 0 crashed`.

- [ ] **Step 7: Commit**

```bash
git add -A Source/AirportMgr
git commit -m "feat(ui): depot card buys sheds and vehicles, sells idle vehicles - rendered from the quote"
```

---

### Task 9: Ghost reveal - the selected depot shows its ghost slots outside edit mode (R10)

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h:137-148, 216`
- Modify: `Plugins/Airside/Source/Airside/Private/Present/PlotPresenter.cpp:377-458, 590-730`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/AirsideBuildingsActor.h:61-67`, `Private/Present/AirsideBuildingsActor.cpp` (append)
- Modify: `Source/AirportMgr/RoadBuildController.h`, `RoadBuildController.cpp:1300-1311`
- Modify tests: `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp`, `Source/AirportMgr/PlayerTickGatesPlotGhostsTest.cpp`

**Interfaces:**
- Produces: `UPlotPresenter::SetGhostScope(FEntityInstanceId Only) -> bool`, `GetGhostScope()`, `GetGhostInstanceCountForTest()`; `AAirsideBuildingsActor::ShowPlotGhosts(bool bVisible, FEntityInstanceId Only)`; `static ARoadBuildController::RevealedDepotFor(const ARoadNetworkActor*, const FSelection&) -> FEntityInstanceId`.

- [ ] **Step 1: Write the failing tests**

`PlotPresenterTest.cpp`, append:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterRevealTest,
	"Airside.Present.PlotPresenter.RevealDrawsOneDepotsGhosts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterRevealTest::RunTest(const FString& Parameters)
{
	// R10: outside edit mode only the SELECTED depot shows its ghost slots; zoomed out, every other depot's
	// cyan boxes would read as buildings (readability ruling 2026-09-27).
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	Actor->ClearNetwork();
	auto Place = [&](double X)
	{
		FEntityPlacement Placement;
		Placement.Definition = Depot;
		Placement.Anchors = Depot->Anchors;
		Placement.Position = FVector2D(X + 1500.0, 0.0);
		Placement.Heading = UE_DOUBLE_HALF_PI;
		Placement.PoseRole = EServiceRole::Fuel;
		Placement.Outline = SpareRoomPlotAt(X);
		Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
		return Actor->Network->PlaceEntity(Placement);
	};
	const FEntityInstanceId A = Place(0.0);
	Place(10000.0);
	Actor->RebuildMesh();
	const UPlotPresenter* Plots = TestWorld.Buildings->GetPlotPresenter();
	const int32 Every = Plots->GetGhostInstanceCountForTest();
	const int32 Capacity = Plots->GetGhostCount();
	if (!TestTrue(TEXT("setup: both yards have ghosts to draw"), Every > 0)) { return false; }

	TestWorld.Buildings->ShowPlotGhosts(/*bVisible=*/true, A);
	const int32 OnlyA = Plots->GetGhostInstanceCountForTest();
	TestTrue(TEXT("the revealed depot draws its ghosts"), OnlyA > 0);
	TestTrue(TEXT("and the other depot's are not drawn"), OnlyA < Every);
	TestTrue(TEXT("the ghost layer is visible for it"), Plots->AreGhostsVisible());
	TestEqual(TEXT("capacity is a fact about the yards, not about what is drawn"), Plots->GetGhostCount(), Capacity);

	TestWorld.Buildings->ShowPlotGhosts(/*bVisible=*/false, FEntityInstanceId());
	TestFalse(TEXT("with nothing revealed, outside edit mode, the ghosts are hidden"), Plots->AreGhostsVisible());
	TestEqual(TEXT("and every yard is instanced again, ready for edit mode to show"), Plots->GetGhostInstanceCountForTest(), Every);
	return true;
}
```

`PlayerTickGatesPlotGhostsTest.cpp`, add includes `#include "Entities/EntityDefinition.h"`, `#include "Model/RoadNetwork.h"`, `#include "Tool/Selection.h"` and append:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRevealedDepotFollowsSelectionTest,
	"AirportMgr.Actions.RevealedDepotFollowsTheSelection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRevealedDepotFollowsSelectionTest::RunTest(const FString& Parameters)
{
	// THE READ PlayerTick hands ShowPlotGhosts: a selected PLOTTED depot is revealed; anything else is not.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 40000.0));
	URoadNetwork& Net = *Actor->Network;
	UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
	FEntityPlacement Placement;
	Placement.Definition = DepotDef;
	Placement.Anchors = DepotDef->Anchors;
	Placement.Position = FVector2D(1000.0, 0.0);
	Placement.Heading = UE_DOUBLE_HALF_PI;
	Placement.PoseRole = EServiceRole::Fuel;
	Placement.Outline = { FVector2D(0.0, 0.0), FVector2D(2000.0, 0.0), FVector2D(2000.0, 2400.0), FVector2D(0.0, 2400.0) };
	Placement.Modules = { EDepotModule::Shed };
	const FEntityInstanceId Depot = Net.PlaceEntity(Placement);
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net.PlaceEntity(StandDef, StandDef->Anchors, FVector2D(20000.0, 0.0), 0.0, 0.0, StandDef->PoseRole, 0);

	FSelection Sel;
	Sel.Kind = ESelectionKind::Stand;
	Sel.Id = Depot.Index;
	TestTrue(TEXT("a selected plotted depot is revealed"), ARoadBuildController::RevealedDepotFor(Actor, Sel) == Depot);
	Sel.Id = Stand.Index;
	TestFalse(TEXT("a selected stand reveals nothing"), ARoadBuildController::RevealedDepotFor(Actor, Sel).IsSet());
	Sel.Kind = ESelectionKind::Aircraft;
	TestFalse(TEXT("an aircraft selection reveals nothing"), ARoadBuildController::RevealedDepotFor(Actor, Sel).IsSet());
	TestFalse(TEXT("and no target reveals nothing"), ARoadBuildController::RevealedDepotFor(nullptr, Sel).IsSet());
	return true;
}
```

- [ ] **Step 2: Build; verify red.**

- [ ] **Step 3: Implement the presenter scope**

`PlotPresenter.h`: add `#include "Model/RoadHandles.h"`. After `AreGhostsVisible`:

```cpp
	/**
	 * Whose unbought bays get INSTANCES: every plot (unset - the default, and edit mode's) or Only's
	 * (facility-upgrades spec R10: the selected depot's card is open outside edit mode). Returns true when it
	 * changed; the CALLER then rebuilds (AAirsideBuildingsActor::ShowPlotGhosts), because instances are made
	 * in RebuildFrom. Visibility stays SetGhostsVisible's; GetGhostCount stays every yard's capacity.
	 * ENFORCED BY: Airside.Present.PlotPresenter.RevealDrawsOneDepotsGhosts
	 */
	bool SetGhostScope(FEntityInstanceId Only);
	FEntityInstanceId GetGhostScope() const { return GhostScope; }

	/** For tests: ghost instances drawn, grey boxes and pooled meshes together. */
	int32 GetGhostInstanceCountForTest() const;
```

private, after `bGhostsVisible`: `/** See SetGhostScope. */ FEntityInstanceId GhostScope;`

Change `DrawMeshes`'s declaration to add `bool bDrawDark` as the last parameter, with the comment line `bDrawDark false lays the run out exactly as before but adds no ghost pieces (SetGhostScope).`

`PlotPresenter.cpp`:

```cpp
bool UPlotPresenter::SetGhostScope(FEntityInstanceId Only)
{
	if (Only == GhostScope)
	{
		return false;
	}
	GhostScope = Only;
	return true;
}

int32 UPlotPresenter::GetGhostInstanceCountForTest() const
{
	int32 Count = GhostBoxes != nullptr ? GhostBoxes->GetInstanceCount() : 0;
	for (const auto& Entry : GhostMeshPool)
	{
		Count += IsValid(Entry.Value) ? Entry.Value->GetInstanceCount() : 0;
	}
	return Count;
}
```

In `DrawMeshes` (signature `..., int32 Lit, int32 Dark, bool bDrawDark)`): Baked branch `if (Dark > 0 && GhostBoxes != nullptr)` becomes `if (Dark > 0 && GhostBoxes != nullptr && bDrawDark)`; Parts' `Piece` lambda guard `if (!bGhost || GhostBoxes != nullptr)` becomes `if (!bGhost || (GhostBoxes != nullptr && bDrawDark))` - the Cursor still advances, so the lit building stays where the whole run puts it.

In `RebuildFrom`, change the entity loop to an index loop so the id is known:

```cpp
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 EntityIndex = 0; EntityIndex < Entities.Num(); ++EntityIndex)
	{
		const FEntityInstance& Entity = Entities[EntityIndex];
		// ... existing skip test unchanged ...
		// R10: THIS YARD'S GHOSTS ARE INSTANCED when every yard's are, or it is the one revealed.
		const bool bDrawDark = !GhostScope.IsSet() || GhostScope == Network.EntityIdAt(EntityIndex);
```

pass `bDrawDark` to `DrawMeshes(*Look, Spec, RunCentre, Stand.Heading, Lit, Dark, bDrawDark)`, and the grey-box ghost `if (Dark > 0 && GhostBoxes != nullptr && !bMeshes)` becomes `if (Dark > 0 && GhostBoxes != nullptr && !bMeshes && bDrawDark)`. `Ghosts += Dark;` is UNCHANGED (capacity, every yard).

- [ ] **Step 4: Implement the buildings actor door**

`AirsideBuildingsActor.h` public, after `GetPlotPresenter`:

```cpp
	/**
	 * The ghost gate, both halves in one call (facility-upgrades spec R10): bVisible is edit mode OR a
	 * revealed depot; Only is that depot (unset = every yard). A change of Only rebuilds the plots once -
	 * instances are made in RebuildFrom - and a frame with no change costs two compares, so both drivers
	 * may call this every frame. ENFORCED BY: Airside.Present.PlotPresenter.RevealDrawsOneDepotsGhosts
	 */
	void ShowPlotGhosts(bool bVisible, FEntityInstanceId Only);
```

(add `#include "Model/RoadHandles.h"` to the header.) `.cpp`, append:

```cpp
void AAirsideBuildingsActor::ShowPlotGhosts(bool bVisible, FEntityInstanceId Only)
{
	if (Plots == nullptr)
	{
		return;
	}
	Plots->SetGhostsVisible(bVisible);
	if (!Plots->SetGhostScope(Only))
	{
		return;
	}
	UE_LOG(LogAirside, Log, TEXT("Plots: ghost bays drawn for %s"),
		Only.IsSet() ? *FString::Printf(TEXT("depot %d only"), Only.Index) : TEXT("every plot"));
	ARoadNetworkActor* Road = Bound.Get();
	if (Road != nullptr && Road->Network != nullptr)
	{
		Rebuild(*Road->Network);
	}
}
```

- [ ] **Step 5: Implement the controller gate**

`RoadBuildController.h` public (beside `SelectedFacility`):

```cpp
	/** The depot whose ghost slots the selection reveals (R10): a selected live PLOTTED depot, else unset. */
	static FEntityInstanceId RevealedDepotFor(const ARoadNetworkActor* InTarget, const FSelection& Selection);
```

`RoadBuildController.cpp`:

```cpp
FEntityInstanceId ARoadBuildController::RevealedDepotFor(const ARoadNetworkActor* InTarget, const FSelection& Selection)
{
	const URoadNetwork* Net = InTarget != nullptr ? InTarget->GetNetwork() : nullptr;
	if (Net == nullptr || Selection.Kind != ESelectionKind::Stand)
	{
		return FEntityInstanceId();
	}
	const FEntityInstanceId Id = Net->EntityIdAt(Selection.Id);
	const FEntityInstance* Entity = Net->GetEntity(Id);
	return Entity != nullptr && Entity->bAlive && Entity->IsDepot() && Entity->IsPlotted() ? Id : FEntityInstanceId();
}
```

In `PlayerTick`, replace `Plots->SetGhostsVisible(Session.WantsPlotGhostsDrawn());` (inside the `if (UPlotPresenter* Plots = ...)` block - replace that whole inner block) with:

```cpp
		// R10 (facility-upgrades spec): EDIT MODE shows every yard's ghosts, as before; outside it the
		// SELECTED depot's card reveals its own - read off the selection here, the one per-frame gate, rather
		// than pushed by the inspector (a push per change site is the shape this comment already refuses).
		const FEntityInstanceId Reveal = RevealedDepotFor(Target, GetSelection());
		const bool bEditing = Session.WantsPlotGhostsDrawn();
		Found->ShowPlotGhosts(bEditing || Reveal.IsSet(), bEditing ? FEntityInstanceId() : Reveal);
```

Keep the existing `GHOST BAYS FOLLOW THE LIT TOOL` comment above it. `AirportMgr.Actions.PlayerTickGatesPlotGhosts` must stay green (nothing selected = the old behaviour).

- [ ] **Step 6: Build and test**

Run `-Filter "Airside.Present.PlotPresenter+Airside.Present.Plot+AirportMgr.Actions+Airside.Tool"`, then the full suite.

- [ ] **Step 7: Commit**

```bash
git add -A Plugins/Airside Source/AirportMgr
git commit -m "feat(plots): selected depot reveals its ghost slots outside edit mode (R10)"
```

---

### Task 10 (OPTIONAL): Hovering "Buy shed" highlights the next slot to light

Visual change: iterate live (memory: edit, Live Coding, shot, look) - do not polish headlessly. Skip if time is short; the plan is complete without it.

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Present/PlotPresenter.h`, `Private/Present/PlotPresenter.cpp`
- Modify: `Plugins/Airside/Source/Airside/Public/Present/AirsideBuildingsActor.h/.cpp`
- Modify: `Source/AirportMgr/InspectorWidget.h/.cpp`, `RoadBuildController.h/.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/PlotPresenterTest.cpp`

**Interfaces:**
- Produces: `UPlotPresenter::SetHighlightNext(int32 KitIndex) -> bool` (INDEX_NONE clears; applies to the revealed depot only); `AAirsideBuildingsActor::HighlightNextSlot(int32 KitIndex)`; `ARoadBuildController::SetHoveredModuleOffer(bool)`.

- [ ] **Step 1: Failing test** (PlotPresenterTest.cpp):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotPresenterHighlightTest,
	"Airside.Present.PlotPresenter.HighlightAddsOneGhostBay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotPresenterHighlightTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
	Actor->ClearNetwork();
	UEntityDefinition* Depot = UEntityDefinition::MakeFuelDepotTransient();
	FEntityPlacement Placement;
	Placement.Definition = Depot;
	Placement.Anchors = Depot->Anchors;
	Placement.Position = FVector2D(1500.0, 0.0);
	Placement.Heading = UE_DOUBLE_HALF_PI;
	Placement.PoseRole = EServiceRole::Fuel;
	Placement.Outline = SpareRoomPlotAt(0.0);
	Placement.Modules = { EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };
	const FEntityInstanceId A = Actor->Network->PlaceEntity(Placement);
	Actor->RebuildMesh();
	TestWorld.Buildings->ShowPlotGhosts(true, A);
	const int32 Before = TestWorld.Buildings->GetPlotPresenter()->GetGhostInstanceCountForTest();
	TestWorld.Buildings->HighlightNextSlot(static_cast<int32>(EDepotModule::Shed));
	TestEqual(TEXT("the next shed slot is drawn once more over its ghost - denser, so it reads as 'this one'"),
		TestWorld.Buildings->GetPlotPresenter()->GetGhostInstanceCountForTest(), Before + 1);
	TestWorld.Buildings->HighlightNextSlot(INDEX_NONE);
	TestEqual(TEXT("and cleared when the hover ends"), TestWorld.Buildings->GetPlotPresenter()->GetGhostInstanceCountForTest(), Before);
	return true;
}
```

- [ ] **Step 2: Implement.** Presenter: `int32 HighlightKit = INDEX_NONE; bool SetHighlightNext(int32 KitIndex) { if (KitIndex == HighlightKit) return false; HighlightKit = KitIndex; return true; }`. In `RebuildFrom`, per entity keep `bool bHighlighted = false;`; after computing `Lit/Dark` for a stand, if `GhostScope.IsSet() && GhostScope == Network.EntityIdAt(EntityIndex) && !bHighlighted && Stand.KitIndex == HighlightKit && Dark > 0 && GhostBoxes != nullptr`, add ONE extra grey-box instance for the first dark bay:

```cpp
				const double BayWidth = One.WidthUu;
				const FVector2D FirstDark = RunCentre + Across * ((LitWidth - FullWidth) * 0.5 + LitWidth + BayWidth * 0.5 - LitWidth * 0.5);
				GhostBoxes->AddInstance(BoxAt(FirstDark, Stand.Heading, One.LengthUu, BayWidth, HeightUu), /*bWorldSpace=*/true);
				bHighlighted = true;
```

(check the centre on screen; the formula places it at the first dark bay's centre along the run - adjust live). Buildings actor: `void HighlightNextSlot(int32 KitIndex) { if (Plots && Plots->SetHighlightNext(KitIndex) && Bound.IsValid() && Bound->Network) Rebuild(*Bound->Network); }`. Inspector: bind `BuyModuleButton->OnHovered/OnUnhovered` (UButton dynamic delegates) to `HandleBuyModuleHovered/Unhovered` UFUNCTIONs calling `C->SetHoveredModuleOffer(true/false)`. Controller: `bool bHoveredModuleOffer`; in the PlayerTick ghost block after `ShowPlotGhosts`, `Found->HighlightNextSlot(bHoveredModuleOffer && Reveal.IsSet() ? static_cast<int32>(QuoteSelectedFacility().Modules.Num() > 0 ? QuoteSelectedFacility().Modules[0].Module : EDepotModule::Shed) : INDEX_NONE);` (hoist the quote into a local).

- [ ] **Step 3: Build, test, look** - run the filter, then PIE: select a depot, hover Buy shed, `python Tools/Mcp.py shot out.png editor`, look. Show the user before committing.

- [ ] **Step 4: Commit** `git commit -m "feat(plots): hovering Buy shed highlights the next slot (first cut, judge in play)"`

---

## PIE verification (after Task 9; the user's answer is the result)

1. Close the editor on the main checkout if it holds this branch's DLLs; open this worktree's editor (or merge first per the branch workflow memory). PIE on a map; confirm `Saved/Logs/AirportMgr.log` shows `Road building ready` and `OpsRuntime attached`.
2. **Draw a NEW depot** (existing maps' depots keep their saved starter trucks - Review Focus 1). Select it: the card reads `No vehicles — buy one`, `Sheds 1 / N space`, `Vehicles 0 / 1 bays`; the depot's ghost slots show outside edit mode, other depots' do not.
3. Park an aircraft on a joined stand: its card reads `Fuel ... · depot has no vehicles - buy one`.
4. Buy vehicle -> Bowser. Log: `Purchase: depot N bought FUEL for 90000 (1/1 bays)`, `Bus: + FleetChanged {...}`, `Notification: Bought Bowser — ¤90,000`, then the job dispatched (`Fuel:` bid line). Ledger panel: a `Fleet` row `-90,000`.
5. Buy shed: `Purchase: depot N bought Shed for 40000 (2/N slots)`, `Depot N: Shed added; undo history cleared`; a ghost slot lights; Undo is disabled.
6. When the bowser is back Idle, Sell (two clicks): `Purchase: depot N sold vehicle V for 45000`, ledger `Fleet +45,000`.
7. Next game day: `Upkeep day D: ... (+400 facilities, +... fleet)`.

## Self-review record

- Spec coverage: §1 R1-R10 -> Tasks 1 (R3, R4 data), 4 (R1, R2, R5, R7, R9 rules), 3/7 (R8), 6 (R6), 9 (R10); §2 data -> 1; §3 commands/events/logging/undo -> 3, 4, 5, 7; §4 UI -> 8, 9, 10; §5 tests -> each task's Step 1 (quote==command, refusals, upkeep, over-capacity, wake, relight, undo, reveal, registry names).
- Placeholder scan: the only elided text is "move the existing WHY comment verbatim" (Task 3 Step 3, Task 4 Step 3), deliberate - the comment exists in the tree and must move, not be retyped.
- Type consistency: hook signature `int32(FEntityInstanceId, const FEntityInstance&, EDepotModule)` used in Tasks 4 and 7; `FFleetChangedEvent{int32, int32, FName, EFleetChange, double}` in Tasks 4 and 5; `GetFleetRevision` in Task 2 and FlightBoard.
