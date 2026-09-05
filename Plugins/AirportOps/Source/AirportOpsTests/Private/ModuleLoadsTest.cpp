#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirportOpsModuleLoadsTest,
	"AirportOps.Module.Loads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirportOpsModuleLoadsTest::RunTest(const FString& Parameters)
{
	// The filter 'AirportOps' must match at least one test or Run-AirsideTests.ps1 reports
	// "no tests matched" and fails the run. This is that test, and it also proves the
	// plugin is enabled in the .uproject - a disabled plugin's modules never load.
	TestTrue(TEXT("AirportOps runtime module is loaded when its tests run"),
		FModuleManager::Get().IsModuleLoaded(TEXT("AirportOps")));
	return true;
}

#endif
