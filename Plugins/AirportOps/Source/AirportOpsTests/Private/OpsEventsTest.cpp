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
	Events->OnAgentPhaseChanged.AddDynamic(L, &UOpsEventsTestListener::OnPhase);
	Events->OnArrivalRefused.AddDynamic(L, &UOpsEventsTestListener::OnRefused);
	Events->OnSpeedChanged.AddDynamic(L, &UOpsEventsTestListener::OnSpeed);
	Events->OnNotification.AddDynamic(L, &UOpsEventsTestListener::OnNote);

	Events->NotifyAgentPhaseChanged(7, EAgentPhase::Taxiing, EAgentPhase::Parked);
	Events->NotifyArrivalRefused(EArrivalRefusal::NoRunway);
	Events->NotifySpeedChanged(ESimSpeed::X4);
	Events->NotifyNotification(TEXT("hello"));

	// Enum ordinals spelled out so a reordering of EAgentPhase or ESimSpeed fails HERE,
	// where the string format is visible, rather than in a UI that reads them.
	const TArray<FString> Expected = {
		TEXT("phase:7:1->3"), TEXT("refused:1"), TEXT("speed:3"), TEXT("note:hello") };
	TestEqual(TEXT("each Notify reaches its bound listener with its arguments intact"), L->Seen, Expected);
	return true;
}

#endif
