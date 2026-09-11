#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/AirlineDefinition.h"
#include "Model/OpsCatalog.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirlineDefinitionCatalogTest,
	"AirportOps.Content.AirlineDefinition.ReachesTheCatalog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirlineDefinitionCatalogTest::RunTest(const FString& Parameters)
{
	// The catalog is how everything else finds airlines, so the type being ENUMERABLE is the
	// feature. Whether the asset manager scans the type at all is a DefaultGame.ini line
	// this cannot reach - see UAirlineDefinition's header for the silence that causes.
	UOpsCatalog* Catalog = NewObject<UOpsCatalog>();
	UAirlineDefinition* Airline = NewObject<UAirlineDefinition>();
	Airline->DisplayName = FText::FromString(TEXT("Meridian"));
	Airline->Fleet.Add(NewObject<UAircraftType>());
	Catalog->Add(Airline);

	const TArray<UAirlineDefinition*> Found = Catalog->All<UAirlineDefinition>();
	TestEqual(TEXT("the catalog returns the airline it was given"), Found.Num(), 1);
	if (Found.Num() > 0)
	{
		TestEqual(TEXT("and its fleet survived the round trip"), Found[0]->Fleet.Num(), 1);
		TestEqual(TEXT("an airline offers something by default, or the inbox stays empty"),
			Found[0]->OffersPerDay > 0.0, true);
	}
	return true;
}

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
		TestTrue(TEXT("and asks for flights"), Airline->OffersPerDay > 0.0);
	}
	return true;
}

#endif
