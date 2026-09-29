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
