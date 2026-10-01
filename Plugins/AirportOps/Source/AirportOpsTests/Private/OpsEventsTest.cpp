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
	Events->OnSaveSlot.AddDynamic(L, &UOpsEventsTestListener::OnSaveSlot);
	Events->OnPurchase.AddDynamic(L, &UOpsEventsTestListener::OnPurchase);

	Events->NotifyArrivalRefused(EArrivalRefusal::NoRunway);
	Events->NotifySaveSlot(EOpsSaveOutcome::SaveFailed, TEXT("hello"));
	FOpsPurchase Purchase;
	Purchase.Kind = EOpsPurchaseKind::ModulesRefunded;
	Purchase.Name = FText::FromString(TEXT("Sheds"));
	Purchase.Count = 2;
	Events->NotifyPurchase(Purchase);

	// Enum ordinals spelled out so a reordering of EArrivalRefusal fails HERE, where the string format is visible, rather than in a UI
	// that reads them. (The phase and speed faces were cut with their delegates, #445: nothing listened to either; the one-string
	// notification and warning faces were replaced by the typed save-slot and purchase faces, #445 item 7.)
	const TArray<FString> Expected = {
		TEXT("refused:1"), TEXT("save:SaveFailed:hello"), TEXT("buy:ModulesRefunded") };
	TestEqual(TEXT("each Notify reaches its bound listener with its arguments intact"), L->Seen, Expected);
	if (TestEqual(TEXT("the purchase arrives whole"), L->Purchases.Num(), 1))
	{
		TestEqual(TEXT("its count intact"), L->Purchases[0].Count, 2);
		TestEqual(TEXT("its noun intact"), L->Purchases[0].Name.ToString(), FString(TEXT("Sheds")));
	}
	return true;
}

#endif
