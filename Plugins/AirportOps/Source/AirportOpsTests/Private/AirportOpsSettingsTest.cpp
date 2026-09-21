#include "CoreMinimal.h"
#include "Content/AirportOpsSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Model/OpsCatalog.h"
#include "Model/OpsDefinition.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * UAirportOpsSettings::ResolveDefaultScenario had no test of its own (issue #194) despite
 * being the one place #104 says a default scenario is named - and, per its own comment, the
 * one function standing between an unconfigured project and a clock silently opening at
 * midnight because a null scenario skipped every caller's `if (Scenario)`. All three branches
 * (unconfigured, configured-and-found, configured-but-missing) share that "never null" promise;
 * nothing measured any of them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirportOpsSettingsResolveDefaultScenarioTest,
	"AirportOps.Content.ResolveDefaultScenario",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirportOpsSettingsResolveDefaultScenarioTest::RunTest(const FString& Parameters)
{
	UOpsCatalog* Catalog = NewObject<UOpsCatalog>();
	UScenario* Named = NewObject<UScenario>(GetTransientPackage(), TEXT("TestNamedScenario"));
	Named->StartingBalance = 555000.0;
	Catalog->Add(Named);

	// THE PROJECT'S OWN CDO, temporarily overridden and restored - the same idiom
	// FuelDepotPlaceToolTest and ServiceRoadToolTest already use for a UDeveloperSettings CDO,
	// so this test measures the FUNCTION rather than whatever DefaultGame.ini happens to say
	// today.
	UAirportOpsSettings* Settings = GetMutableDefault<UAirportOpsSettings>();
	const FPrimaryAssetId Configured = Settings->DefaultScenario;
	ON_SCOPE_EXIT { Settings->DefaultScenario = Configured; };

	// 1. UNCONFIGURED: falls back to UScenario's own CDO, and it is never null - the specific
	// regression the function's own header comment describes.
	Settings->DefaultScenario = FPrimaryAssetId();
	const UScenario* Unconfigured = UAirportOpsSettings::ResolveDefaultScenario(*Catalog);
	if (TestNotNull(TEXT("unconfigured still resolves to something"), Unconfigured))
	{
		TestEqual(TEXT("unconfigured resolves to UScenario's own CDO"),
			Unconfigured, GetDefault<UScenario>());
	}

	// 2. CONFIGURED AND FOUND: the catalog's entry, not a second load of the same asset - #104's
	// whole point is that LoadFromAssetManager is the only loader.
	Settings->DefaultScenario = Named->GetPrimaryAssetId();
	const UScenario* Found = UAirportOpsSettings::ResolveDefaultScenario(*Catalog);
	if (TestNotNull(TEXT("a configured, catalogued name resolves"), Found))
	{
		TestEqual(TEXT("to exactly the catalog's entry"), Found, static_cast<const UScenario*>(Named));
		TestEqual(TEXT("carrying its own figures, not the CDO's"),
			Found->StartingBalance, 555000.0, 1e-6);
	}

	// 3. CONFIGURED BUT MISSING FROM THE CATALOG: falls back to the CDO rather than null or a
	// crash - a scenario asset that failed to scan into the Asset Manager must not silently
	// open the clock at midnight either. THE ERROR IS THE POINT (SteerLawTest's own idiom):
	// the function logs at Error for exactly this case, and the automation harness fails any
	// test that logs at Error unless it is declared expected - so expecting it here asserts
	// the log fires, rather than merely tolerating it.
	AddExpectedError(TEXT("is configured but not in the catalog"), EAutomationExpectedErrorFlags::Contains, 1);
	Settings->DefaultScenario = FPrimaryAssetId(FPrimaryAssetType(TEXT("Scenario")), FName(TEXT("NotInCatalog")));
	const UScenario* Missing = UAirportOpsSettings::ResolveDefaultScenario(*Catalog);
	if (TestNotNull(TEXT("a configured but uncatalogued name still resolves to something"), Missing))
	{
		TestEqual(TEXT("falling back to UScenario's own CDO, the same as unconfigured"),
			Missing, GetDefault<UScenario>());
	}

	return true;
}

#endif
