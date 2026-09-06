#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/OpsCatalog.h"
#include "Model/OpsDefinition.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsCatalogTest,
	"AirportOps.Model.Catalog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsCatalogTest::RunTest(const FString& Parameters)
{
	UOpsCatalog* Catalog = NewObject<UOpsCatalog>();
	UScenario* Easy = NewObject<UScenario>(GetTransientPackage(), TEXT("Easy"));
	Easy->StartingBalance = 900000.0;
	UScenario* Hard = NewObject<UScenario>(GetTransientPackage(), TEXT("Hard"));
	Hard->StartingBalance = 100000.0;

	Catalog->Add(Easy);
	Catalog->Add(Hard);
	Catalog->Add(Hard);  // duplicates are ignored, not doubled
	Catalog->Add(nullptr);

	TestEqual(TEXT("All<UScenario> lists each definition once"), Catalog->All<UScenario>().Num(), 2);
	UScenario* Found = Catalog->Find<UScenario>(TEXT("Hard"));
	if (TestNotNull(TEXT("Find by asset name"), Found))
	{
		TestEqual(TEXT("returns the right one"), Found->StartingBalance, 100000.0, 1e-9);
	}
	TestNull(TEXT("an unknown name is null, not a crash"), Catalog->Find<UScenario>(TEXT("Nope")));

	TestEqual(TEXT("the primary asset type is the class name, so the Asset Manager can scan per type"),
		Easy->GetPrimaryAssetId().PrimaryAssetType.GetName(), FName(TEXT("Scenario")));
	TestEqual(TEXT("and the asset name is the id"), Easy->GetPrimaryAssetId().PrimaryAssetName, FName(TEXT("Easy")));
	return true;
}

#endif
