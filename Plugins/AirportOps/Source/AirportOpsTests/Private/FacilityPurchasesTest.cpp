#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/FacilityPurchases.h"
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

#endif
