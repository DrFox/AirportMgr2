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

#endif
