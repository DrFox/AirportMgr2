#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/AirlineDefinition.h"
#include "Model/OpsCatalog.h"

#if WITH_DEV_AUTOMATION_TESTS

// (AirportOps.Content.AirlineDefinition.ReachesTheCatalog is gone, #462 #33: the catalog stores pointers, so "its fleet survived the
// round trip" read back the test's own object, and "offers something by default" read a constructor constant. That an airline is
// ENUMERABLE through UOpsCatalog is AirportOps.Model.Catalog's (the same All<T> for UScenario); that the asset manager SCANS the type
// - a DefaultGame.ini line this module cannot reach, see UAirlineDefinition's header for the silence that causes - is the test below.)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirlineAssetsAreScannedTest,
	"AirportOps.Content.AirlineDefinition.TheAssetManagerScansThem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirlineAssetsAreScannedTest::RunTest(const FString& Parameters)
{
	// THE ONE FAILURE NOTHING ELSE CATCHES. UOpsDefinition::GetPrimaryAssetId derives the
	// type from the class name minus its prefix, so a missing or misspelt
	// PrimaryAssetTypesToScan line in DefaultGame.ini means the catalog scans nothing, loads
	// nothing, and reports no airlines - with no error at any point. The game then runs
	// perfectly with an inbox that never fills, which reads as a broken generator.
	//
	// Content-dependent by design: it asserts the shipped assets are reachable, which is the
	// claim being made. If it fails after adding an airline, check the .ini before the code.
	UOpsCatalog* Catalog = NewObject<UOpsCatalog>();
	Catalog->LoadFromAssetManager();

	const TArray<UAirlineDefinition*> Airlines = Catalog->All<UAirlineDefinition>();
	TestTrue(TEXT("the asset manager scans and loads at least one airline"), Airlines.Num() > 0);

	for (const UAirlineDefinition* Airline : Airlines)
	{
		if (Airline == nullptr)
		{
			continue;
		}
		// An airline with an empty fleet offers nothing, for ever, and says nothing about it.
		TestTrue(TEXT("every shipped airline has a fleet"), Airline->Fleet.Num() > 0);
		TestTrue(TEXT("and asks for flights"),
			Airline->PeakOffersPerHour > 0.0 || Airline->FloorOffersPerHour > 0.0);
		// TWO GAME HOURS AT LEAST (2026-09-28, from play): landing, taxiing in, a fuel loop and
		// taxiing out take ~95 game minutes in daylight before any queueing.
		TestTrue(FString::Printf(TEXT("%s gives at least two game hours from accept to airborne"),
			*Airline->DisplayName.ToString()), Airline->ContractSeconds >= 2.0 * 3600.0);
		TestTrue(TEXT("its demand curve is flat (empty) or one weight per hour"),
			Airline->DemandCurve.Num() == 0 || Airline->DemandCurve.Num() == 24);
	}
	return true;
}

#endif
