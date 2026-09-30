#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/OpsEvents.h"
#include "OpsEventsTestListener.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOpsEventsTest,
	"AirportOps.Model.Events",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOpsEventsTest::RunTest(const FString& Parameters)
{
	UOpsEvents* Events = NewObject<UOpsEvents>();
	UOpsEventsTestListener* L = NewObject<UOpsEventsTestListener>();
	Events->OnArrivalRefused.AddDynamic(L, &UOpsEventsTestListener::OnRefused);
	Events->OnNotification.AddDynamic(L, &UOpsEventsTestListener::OnNote);

	Events->NotifyArrivalRefused(EArrivalRefusal::NoRunway);
	Events->NotifyNotification(TEXT("hello"));

	// Enum ordinals spelled out so a reordering of EArrivalRefusal fails HERE, where the string format is visible, rather than in a UI
	// that reads them. (The phase and speed faces were cut with their delegates, #445: nothing listened to either.)
	const TArray<FString> Expected = {
		TEXT("refused:1"), TEXT("note:hello") };
	TestEqual(TEXT("each Notify reaches its bound listener with its arguments intact"), L->Seen, Expected);
	return true;
}

#endif
