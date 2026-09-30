#include "CoreMinimal.h"
#include "Content/AirportOpsSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Model/OpsCatalog.h"
#include "Model/OpsDefinition.h"
#include "Testing/AirsideTestWorld.h"

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


/**
 * THE PROJECT'S OWN CONFIGURATION, measured - not the function, which the test above does with a settings
 * CDO it overrides. Spec 2026-09-29-ops-batch3 §2: until DA_Scenario_Default existed, Content/Ops did not,
 * and every figure a designer might tune was a constructor default nobody could see in the editor. This goes
 * red if the asset is deleted, renamed, moved out of the /Game/Ops scan, or the ini line is dropped.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirportOpsDefaultScenarioIsTheAssetTest,
	"AirportOps.Content.DefaultScenarioIsTheAsset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirportOpsDefaultScenarioIsTheAssetTest::RunTest(const FString& Parameters)
{
	// A FRESH CATALOG, loaded the one way the runtime loads one (UOpsRuntime::Attach).
	UOpsCatalog* Catalog = NewObject<UOpsCatalog>();
	Catalog->LoadFromAssetManager();
	const UScenario* Scenario = UAirportOpsSettings::ResolveDefaultScenario(*Catalog);
	if (!TestNotNull(TEXT("the default scenario resolves"), Scenario)) { return false; }
	TestTrue(TEXT("to an asset, not UScenario's CDO - the tuning is editable in the Details panel"), Scenario != GetDefault<UScenario>());
	TestEqual(TEXT("the asset build_scenario.py creates"), Scenario->GetName(), FString(TEXT("DA_Scenario_Default")));
	// NO FIGURE IS ASSERTED (review I3): the asset is the designer's to tune, and a check that its numbers
	// equal the CDO's would fail on the first tune while proving nothing today (an untouched asset IS the CDO's
	// values). build_scenario.py checks "creation changed nothing" once, at creation, where it is true.
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirportOpsResolvedLineTest,
	"AirportOps.Content.ResolvedScenarioIsLoggedPerAsset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirportOpsResolvedLineTest::RunTest(const FString& Parameters)
{
	// ONCE PER DISTINCT ASSET, not once per process (review M5): a once-per-process line names whichever
	// scenario was resolved first - in an editor session, possibly one a test configured - and is silent for
	// the one the game then runs on, which is the very question the line exists to answer.
	UOpsCatalog* Catalog = NewObject<UOpsCatalog>();
	UScenario* First = NewObject<UScenario>(GetTransientPackage(), TEXT("TestLoggedScenarioA"));
	UScenario* Second = NewObject<UScenario>(GetTransientPackage(), TEXT("TestLoggedScenarioB"));
	Catalog->Add(First);
	Catalog->Add(Second);
	UAirportOpsSettings* Settings = GetMutableDefault<UAirportOpsSettings>();
	const FPrimaryAssetId Configured = Settings->DefaultScenario;
	ON_SCOPE_EXIT { Settings->DefaultScenario = Configured; };

	FLogLineSpy Spy(FName(TEXT("LogAirportOps")));
	GLog->AddOutputDevice(&Spy);
	Settings->DefaultScenario = First->GetPrimaryAssetId();
	UAirportOpsSettings::ResolveDefaultScenario(*Catalog);
	UAirportOpsSettings::ResolveDefaultScenario(*Catalog);
	Settings->DefaultScenario = Second->GetPrimaryAssetId();
	UAirportOpsSettings::ResolveDefaultScenario(*Catalog);
	GLog->RemoveOutputDevice(&Spy);

	auto Count = [&Spy](const TCHAR* Name)
	{
		return Spy.CapturedLines.FilterByPredicate([Name](const FString& Line)
			{ return Line.Contains(TEXT("resolved to")) && Line.Contains(Name); }).Num();
	};
	TestEqual(TEXT("the first asset is named once, however often it is resolved"), Count(TEXT("TestLoggedScenarioA")), 1);
	TestEqual(TEXT("and a different asset is named when the setting moves to it"), Count(TEXT("TestLoggedScenarioB")), 1);
	return true;
}

#endif
