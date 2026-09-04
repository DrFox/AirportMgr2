#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirsideScaffoldTest,
	"Airside.Scaffold.HarnessRuns",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirsideScaffoldTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("harness arithmetic"), 2 + 2, 4);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
