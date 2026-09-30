#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/FacilityPurchases.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OpsDefinition.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Templates/UniquePtr.h"

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
		int32 ReservedPumps = 1;
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
			// THE BOARD'S OWN LEDGER AND BUS, which the fleet's door posts to and publishes on (#443): a vehicle's charge, its
			// credit and its FleetChanged are the door's, no longer the shop's. UOpsRuntime wires the same two beside the shop's.
			Board->Ledger = Ledger;
			Board->Bus = &Bus;
			Shop = NewObject<UFacilityPurchases>(GetTransientPackage());
			Shop->ModuleOffers = GetDefault<UScenario>()->ModuleOffers;
			Shop->JobBoard = Board;
			Shop->Ledger = Ledger;
			Shop->Clock = Clock;
			Shop->Bus = &Bus;
			Shop->ReservedSlotsOf = [this](FEntityInstanceId, const FEntityInstance&, EDepotModule Module)
			{
				return Module == EDepotModule::Shed ? ReservedSheds : Module == EDepotModule::Pump ? ReservedPumps : 1;
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
	TestEqual(TEXT("the start kit's one shed is one bay"), F.Shop->VehicleSlotsOf(F.Depot, *F.Net->GetEntity(F.Depot)), 1);
	F.Net->AddEntityModule(F.Depot, EDepotModule::Tank);
	TestEqual(TEXT("a tank grants nothing - it has no offer"), F.Shop->VehicleSlotsOf(F.Depot, *F.Net->GetEntity(F.Depot)), 1);
	F.Net->AddEntityModule(F.Depot, EDepotModule::Shed);
	TestEqual(TEXT("a second shed is a second bay"), F.Shop->VehicleSlotsOf(F.Depot, *F.Net->GetEntity(F.Depot)), 2);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityDepotRemovedCreditsTest, "AirportOps.Model.Facility.DepotRemovalCreditsItsVehicles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityDepotRemovedCreditsTest::RunTest(const FString&)
{
	// RULED 2026-09-30: a vehicle withdrawn because its depot went (bulldoze, undo of the placement) is
	// credited its resale value, as a sale would have been. Before it, the player paid 90000 for a bowser
	// and removing the depot took it without a penny back.
	FFacilityFixture F;
	F.Board->Ledger = F.Ledger;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FPurchaseResult Bought = F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("FUEL"));
	if (!TestTrue(TEXT("setup: a vehicle is bought"), Bought.Succeeded())) { return false; }
	const double AfterBuy = F.Ledger->Balance();
	const int32 EntriesBefore = F.Ledger->Entries().Num();

	F.Net->RemoveEntity(F.Depot);
	F.Board->Step(*Traffic, *F.Net, *F.Clock);
	TestNull(TEXT("the vehicle is withdrawn with its depot"), F.Board->FindVehicle(Bought.VehicleId));
	TestEqual(TEXT("and credited Price x ResaleFraction"), F.Ledger->Balance(), AfterBuy + 45000.0, 1e-6);
	if (!TestEqual(TEXT("in exactly one entry"), F.Ledger->Entries().Num(), EntriesBefore + 1)) { return true; }
	TestEqual(TEXT("filed under Fleet, where its purchase was"),
		static_cast<int32>(F.Ledger->Entries().Last().Category), static_cast<int32>(ELedgerCategory::Fleet));
	F.Board->Step(*Traffic, *F.Net, *F.Clock);
	TestEqual(TEXT("and only once - the next sync finds nothing to credit"), F.Ledger->Entries().Num(), EntriesBefore + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityDepotRemovedPublishesTest, "AirportOps.Model.Fleet.DepotRemovalPublishesFleetChanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityDepotRemovedPublishesTest::RunTest(const FString&)
{
	// #443: A REMOVED DEPOT'S VEHICLES LEFT THE FLEET WITH THEIR CREDIT POSTED BY THE JOB BOARD AND NO EVENT, so a FleetChanged
	// subscriber - the feed's "Bowser #3 credited, depot removed" - heard of purchases and sales and missed this. The
	// fleet's door announces every way out.
	FFacilityFixture F;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	TArray<FFleetChangedEvent> Seen;
	F.Bus.BeginWiring();
	F.Bus.Subscribe<FFleetChangedEvent>(EOpsTier::Presentation, TEXT("test"), [&Seen](const FFleetChangedEvent& E) { Seen.Add(E); });
	F.Bus.EndWiring();
	const FPurchaseResult Bought = F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("FUEL"));
	if (!TestTrue(TEXT("setup: a vehicle is bought"), Bought.Succeeded())) { return false; }
	F.Bus.Drain();
	if (!TestEqual(TEXT("setup: the purchase was announced, from the door and not twice"), Seen.Num(), 1)) { return false; }
	TestEqual(TEXT("setup: as Bought"), static_cast<int32>(Seen[0].Change), static_cast<int32>(EFleetChange::Bought));
	Seen.Reset();

	F.Net->RemoveEntity(F.Depot);
	F.Board->Step(*Traffic, *F.Net, *F.Clock);
	F.Bus.Drain();
	if (!TestEqual(TEXT("the removal published exactly one FleetChanged"), Seen.Num(), 1)) { return false; }
	TestEqual(TEXT("saying the vehicle was Withdrawn"), static_cast<int32>(Seen[0].Change), static_cast<int32>(EFleetChange::Withdrawn));
	TestEqual(TEXT("which vehicle"), Seen[0].VehicleId, Bought.VehicleId);
	TestEqual(TEXT("of what kind"), Seen[0].TypeCode, FName(TEXT("FUEL")));
	TestEqual(TEXT("from which depot"), Seen[0].Depot, F.Depot.Index);
	TestEqual(TEXT("and what it was credited"), Seen[0].Amount, 45000.0, 1e-9);
	Seen.Reset();
	F.Board->Step(*Traffic, *F.Net, *F.Clock);
	F.Bus.Drain();
	TestEqual(TEXT("and only once - the next Step finds nothing to withdraw"), Seen.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityUnseatedTest, "AirportOps.Model.Facility.UnseatedModulesGrantNeitherBaysNorPumps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityUnseatedTest::RunTest(const FString&)
{
	// #443, RULED 2026-09-30 (#266): "a player should not be able to purchase upgrades that don't fit on the plot; the
	// modules that count are only the ones that are placed." FEntityInstance::Modules is what the player OWNS; the plot
	// seats min(owned, ceiling) of each kind and the presenter drops the rest. A module it could not seat grants no bay
	// and no pump - the depot card used to quote "2 bays" for a plot that stood one shed. The ceiling is the fixture's
	// hook, standing for UOpsRuntime::ReservedSlotsOf, and the board reads the SAME one (as Attach wires it).
	FFacilityFixture F;
	F.Board->ModuleCeilingOf = F.Shop->ReservedSlotsOf;
	auto Depot = [&F]() -> const FEntityInstance& { return *F.Net->GetEntity(F.Depot); };
	TestEqual(TEXT("control: the start kit's shed is seated, so one bay"), F.Shop->VehicleSlotsOf(F.Depot, Depot()), 1);
	TestTrue(TEXT("control: and its pump is seated, so the depot can fuel"), F.Board->HasWorkingPump(F.Depot, Depot()));
	TestEqual(TEXT("control: one pump"), F.Board->PumpsAt(F.Depot, Depot()), 1);

	// THE PLOT CANNOT SEAT THE SHED: owned, and granting nothing.
	F.ReservedSheds = 0;
	TestEqual(TEXT("an owned shed the plot cannot seat grants no bay"), F.Shop->VehicleSlotsOf(F.Depot, Depot()), 0);
	TestEqual(TEXT("and the quote says so"), F.Shop->Quote(*F.Net, F.Depot).Bays, 0);
	TestEqual(TEXT("so the vehicle purchase is refused for want of a bay - and the command agrees with the quote"),
		static_cast<int32>(F.Shop->BuyVehicle(*F.Net, F.Depot, TEXT("FUEL")).Refusal), static_cast<int32>(EPurchaseRefusal::NoFreeBay));

	// THE PLOT CANNOT SEAT THE PUMP: the depot cannot fuel.
	F.ReservedSheds = 3;
	F.ReservedPumps = 0;
	TestFalse(TEXT("an owned pump the plot cannot seat is no pump"), F.Board->HasWorkingPump(F.Depot, Depot()));
	TestEqual(TEXT("and the refill divisor stays floored at one"), F.Board->PumpsAt(F.Depot, Depot()), 1);

	// OVER-OWNED: more sheds owned than the plot holds. Seated up to the ceiling; the rest grant nothing, and buying another is
	// refused (NoSlotReserved), so nothing bought is ever left unseated.
	F.ReservedPumps = 1;
	F.ReservedSheds = 2;
	F.Net->AddEntityModule(F.Depot, EDepotModule::Shed);
	F.Net->AddEntityModule(F.Depot, EDepotModule::Shed);
	TestEqual(TEXT("three sheds owned, two seated: two bays"), F.Shop->VehicleSlotsOf(F.Depot, Depot()), 2);
	TestEqual(TEXT("a fourth shed is refused - it would not be seated"),
		static_cast<int32>(F.Shop->BuyModule(*F.Net, F.Depot, EDepotModule::Shed).Refusal), static_cast<int32>(EPurchaseRefusal::NoSlotReserved));

	// AND THE QUOTE IS THE COMMAND for what the plot can seat: at owned == ceiling the card lights nothing.
	for (const FModuleOfferQuote& Offer : F.Shop->Quote(*F.Net, F.Depot).Modules)
	{
		if (Offer.Module == EDepotModule::Shed)
		{
			TestEqual(TEXT("the shed's button is disabled for the same reason"),
				static_cast<int32>(Offer.Refusal), static_cast<int32>(EPurchaseRefusal::NoSlotReserved));
		}
	}
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
	// "no module hook": a shop the runtime never attached - the quote lit "Buy Shed" while the command
	// refused NotAFacility, until JudgeModule counted the unset hook (task-7 review finding 1).
	struct FCase { const TCHAR* Name; double Balance; int32 ReservedSheds; int32 Idle; int32 Busy; bool bHookless = false; };
	const FCase Cases[] = {
		{ TEXT("rich, room, empty"), 500000.0, 3, 0, 0 },
		{ TEXT("poor"), 1000.0, 3, 0, 0 },
		{ TEXT("no space"), 500000.0, 1, 0, 0 },
		{ TEXT("bays full"), 500000.0, 3, 1, 0 },
		{ TEXT("a busy vehicle"), 500000.0, 3, 0, 1 },
		{ TEXT("no module hook"), 500000.0, 3, 0, 0, true },
	};
	auto Make = [](const FCase& Case)
	{
		TUniquePtr<FFacilityFixture> F = MakeUnique<FFacilityFixture>(Case.Balance);
		F->ReservedSheds = Case.ReservedSheds;
		if (Case.bHookless) { F->Shop->ApplyModulePurchase = nullptr; }
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityNameFallbackTest, "AirportOps.Model.Facility.EmptyDisplayNameFallsBackToTypeCode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityNameFallbackTest::RunTest(const FString&)
{
	// A DESIGNER ROW WITH NO NAME still labels its button (FFuelVehicleSpec::DisplayName is optional data):
	// the type code stands in, rather than a blank "  90,000 . 10,000 L" nobody can read.
	FFacilityFixture F;
	F.Board->VehicleSpecs.FindChecked(TEXT("FUEL")).DisplayName = FText::GetEmpty();
	const FFacilityQuote Q = F.Shop->Quote(*F.Net, F.Depot);
	const FVehicleOfferQuote* Row = Q.VehicleOffers.FindByPredicate([](const FVehicleOfferQuote& Each) { return Each.TypeCode == TEXT("FUEL"); });
	if (!TestNotNull(TEXT("the bowser is offered"), Row)) { return false; }
	TestEqual(TEXT("an empty DisplayName reads as the type code"), Row->Name.ToString(), FString(TEXT("FUEL")));
	TestTrue(TEXT("and the label leads with it"), Row->Label.ToString().StartsWith(TEXT("FUEL ")));
	const FVehicleOfferQuote* Named = Q.VehicleOffers.FindByPredicate([](const FVehicleOfferQuote& Each) { return Each.TypeCode == TEXT("UTILITY"); });
	if (!TestNotNull(TEXT("the tow is offered"), Named)) { return false; }
	TestFalse(TEXT("a named row keeps its own name - the fallback is not the rule"), Named->Name.ToString() == TEXT("UTILITY"));
	return true;
}

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

/**
 * THE REPAIR, WORLD-FREE (#266, owner 2026-09-30, option b): a depot owning more of a kind than its plot seats has the
 * excess removed through the hook and refunded at the offer's price - one Refund line, one event per kind - and owned ==
 * seated afterwards, so upkeep never charges for a module that is not standing. With no removal hook nothing is removed
 * AND nothing is paid (a refund alone would be money for nothing); a plotless depot has nothing to be smaller than.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityRepairTest, "AirportOps.Model.Facility.RepairRemovesAndRefundsTheExcess",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityRepairTest::RunTest(const FString&)
{
	FFacilityFixture F;
	// Ceilings: 3 sheds, 1 pump, 1 tank. Owned: 5 sheds, 3 pumps, 2 tanks - two, two and one too many.
	for (int32 I = 0; I < 4; ++I) { F.Net->AddEntityModule(F.Depot, EDepotModule::Shed); }
	F.Net->AddEntityModule(F.Depot, EDepotModule::Pump);
	F.Net->AddEntityModule(F.Depot, EDepotModule::Pump);
	F.Net->AddEntityModule(F.Depot, EDepotModule::Tank);
	UEntityDefinition* Def = UEntityDefinition::MakeFuelDepotTransient();
	FEntityPlacement Plotless;
	Plotless.Definition = Def;
	Plotless.Anchors = Def->Anchors;
	Plotless.Position = FVector2D(40000.0, 0.0);
	Plotless.PoseRole = EServiceRole::Fuel;
	Plotless.Modules = { EDepotModule::Shed, EDepotModule::Shed, EDepotModule::Shed, EDepotModule::Shed, EDepotModule::Shed };
	const FEntityInstanceId Legacy = F.Net->PlaceEntity(Plotless);
	TArray<FModulesRefundedEvent> Seen;
	F.Bus.BeginWiring();
	F.Bus.Subscribe<FModulesRefundedEvent>(EOpsTier::Presentation, TEXT("test"), [&Seen](const FModulesRefundedEvent& E) { Seen.Add(E); });
	F.Bus.EndWiring();
	const double Balance = F.Ledger->Balance();
	auto Count = [&F](FEntityInstanceId Id, EDepotModule Module)
	{
		int32 Out = 0;
		for (const EDepotModule Each : F.Net->GetEntity(Id)->Modules) { Out += Each == Module ? 1 : 0; }
		return Out;
	};

	TestEqual(TEXT("with no removal hook nothing is removed"), F.Shop->RemoveUnseated(*F.Net), 0);
	TestEqual(TEXT("and nothing is paid - a refund with no removal is money for nothing"), F.Ledger->Balance(), Balance, 1e-9);
	TestEqual(TEXT("the sheds are all still owned"), F.Sheds(), 5);

	F.Shop->ApplyModuleRemoval = [&F](FEntityInstanceId Id, EDepotModule Module, int32 Many) { return F.Net->RemoveEntityModules(Id, Module, Many); };
	TestEqual(TEXT("the repair removes every module past its ceiling: 2 sheds, 2 pumps, 1 tank"), F.Shop->RemoveUnseated(*F.Net), 5);
	TestEqual(TEXT("owned == seated: three sheds"), F.Sheds(), 3);
	TestEqual(TEXT("one pump"), Count(F.Depot, EDepotModule::Pump), 1);
	TestEqual(TEXT("one tank"), Count(F.Depot, EDepotModule::Tank), 1);
	TestEqual(TEXT("the plotless depot keeps all five - no plot, nothing to be smaller than"), Count(Legacy, EDepotModule::Shed), 5);
	TestEqual(TEXT("the two sheds are refunded at the offer's price"), F.Ledger->Balance(), Balance + 2.0 * 40000.0, 1e-6);
	const FLedgerEntry& Line = F.Ledger->Entries().Last();
	TestEqual(TEXT("on ONE line"), Line.Amount, 80000.0, 1e-6);
	TestEqual(TEXT("in the Refund column"), static_cast<int32>(Line.Category), static_cast<int32>(ELedgerCategory::Refund));
	TestTrue(FString::Printf(TEXT("saying what and why (%s)"), *Line.What.ToString()), Line.What.ToString().Contains(TEXT("2 x Shed")));
	F.Bus.Drain();
	if (TestEqual(TEXT("one event per kind removed"), Seen.Num(), 3))
	{
		const FModulesRefundedEvent* Sheds = Seen.FindByPredicate([](const FModulesRefundedEvent& E) { return E.Module == EDepotModule::Shed; });
		const FModulesRefundedEvent* Pumps = Seen.FindByPredicate([](const FModulesRefundedEvent& E) { return E.Module == EDepotModule::Pump; });
		TestTrue(TEXT("the sheds' names the depot, the count and the credit"),
			Sheds != nullptr && Sheds->Entity == F.Depot.Index && Sheds->Count == 2 && FMath::IsNearlyEqual(Sheds->Amount, 80000.0));
		TestTrue(TEXT("a kind the shop does not sell is removed with nothing to refund"),
			Pumps != nullptr && Pumps->Count == 2 && Pumps->Amount == 0.0);
	}
	const int32 Lines = F.Ledger->Entries().Num();
	TestEqual(TEXT("run again, there is nothing left to repair"), F.Shop->RemoveUnseated(*F.Net), 0);
	TestEqual(TEXT("and nothing more is posted"), F.Ledger->Entries().Num(), Lines);
	TestEqual(TEXT("upkeep charges the three standing sheds and the plotless five - none past a plot's ceiling"), F.Shop->DailyUpkeep(*F.Net).Modules, (3.0 + 5.0) * 200.0, 1e-9);
	return true;
}

/**
 * THE KIT SHED IS REFUNDED AT THE SHOP'S PRICE (orchestrator ruling on #469, 2026-09-30): a depot owning only the shed it
 * was drawn with, on a plot that now seats no shed, loses the shed and is paid FModuleOffer::Price for it - the current
 * offer, since what it cost is not recorded per module. Its tank and pump seat, and stay.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFacilityKitShedRefundTest, "AirportOps.Model.Facility.RepairRefundsAKitShedAtTheShopPrice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFacilityKitShedRefundTest::RunTest(const FString&)
{
	FFacilityFixture F;
	F.ReservedSheds = 0;
	F.Shop->ApplyModuleRemoval = [&F](FEntityInstanceId Id, EDepotModule Module, int32 Many) { return F.Net->RemoveEntityModules(Id, Module, Many); };
	if (!TestEqual(TEXT("setup: the depot owns only its kit shed"), F.Sheds(), 1)) { return false; }
	const double Price = F.Shop->ModuleOffers.FindChecked(EDepotModule::Shed).Price;
	if (!TestTrue(TEXT("setup: the shop sells sheds"), Price > 0.0)) { return false; }
	const double Balance = F.Ledger->Balance();

	TestEqual(TEXT("the repair removes the one shed the plot cannot seat"), F.Shop->RemoveUnseated(*F.Net), 1);
	TestEqual(TEXT("so the depot owns none"), F.Sheds(), 0);
	TestEqual(TEXT("and is paid the shop's current price for it, though it came with the plot"), F.Ledger->Balance(), Balance + Price, 1e-6);
	const FLedgerEntry& Line = F.Ledger->Entries().Last();
	TestEqual(TEXT("on a Refund line"), static_cast<int32>(Line.Category), static_cast<int32>(ELedgerCategory::Refund));
	TestEqual(TEXT("of exactly one shed's price"), Line.Amount, Price, 1e-6);
	TestEqual(TEXT("its kit tank and pump seat, and stay"), F.Net->GetEntity(F.Depot)->Modules.Num(), 2);
	return true;
}

#endif
