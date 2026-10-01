#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "NotificationCentre.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE REGRESSION THIS EXISTS FOR. A toast is a piece of UI a human reads, so its lifetime is
 * REAL seconds - never game seconds, and never scaled by the speed multiplier either. Timed
 * on game seconds it would live 72x shorter at the day compression, and x32 now exists on
 * top of that; merely multiplied by Multiplier() it would live a quarter of a second at x32.
 * Either way the toast would be gone before the eye reached it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNotificationFeedExpiryTest,
	"AirportMgr.UI.FeedExpiresOnRealSeconds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNotificationFeedExpiryTest::RunTest(const FString& Parameters)
{
	UNotificationCentre* Centre = NewObject<UNotificationCentre>(GetTransientPackage());
	Centre->FeedLifetimeRealSeconds = 8.0;

	Centre->PostFeed(FText::FromString(TEXT("Arrival refused - runway too short")));
	TestEqual(TEXT("the entry is up"), Centre->Entries().Num(), 1);

	// Seven real seconds: still up.
	Centre->Advance(7.0);
	TestEqual(TEXT("still up just short of its lifetime"), Centre->Entries().Num(), 1);

	// Two more: gone.
	Centre->Advance(2.0);
	TestEqual(TEXT("expired after its real-seconds lifetime"), Centre->Entries().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNotificationScrollbackTest,
	"AirportMgr.UI.FeedScrollbackIsBounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNotificationScrollbackTest::RunTest(const FString& Parameters)
{
	UNotificationCentre* Centre = NewObject<UNotificationCentre>(GetTransientPackage());
	Centre->FeedLifetimeRealSeconds = 10000.0;   // nothing expires; the bound is what is under test

	for (int32 I = 0; I < 80; ++I)
	{
		Centre->PostFeed(FText::FromString(FString::Printf(TEXT("event %d"), I)));
	}

	// A long session must not grow this without bound, and the OLDEST is what goes.
	TestEqual(TEXT("scrollback is capped at 50"), Centre->Entries().Num(), 50);
	TestEqual(TEXT("the oldest survivor is event 30, so the oldest were dropped"),
		Centre->Entries()[0].Text.ToString(), FString(TEXT("event 30")));
	return true;
}

#endif
