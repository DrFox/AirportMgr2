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

/**
 * The same eight seconds, delivered as a x32 session's worth of frames.
 *
 * This is the assertion the spec asks for and the one a game-seconds - or a
 * Multiplier()-scaled - implementation FAILS. Half a real second of 60 Hz frames at x32
 * would be 16 game seconds and would expire an 8-second toast twice over; here it must not
 * expire at all, because the frames are real and the player has been looking at it for half
 * a second.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNotificationSpeedTest,
	"AirportMgr.UI.FeedIgnoresGameSpeed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNotificationSpeedTest::RunTest(const FString& Parameters)
{
	UNotificationCentre* Centre = NewObject<UNotificationCentre>(GetTransientPackage());
	Centre->FeedLifetimeRealSeconds = 8.0;
	Centre->PostFeed(FText::FromString(TEXT("Loaded 'quick'")));

	// 30 frames of 1/60 s: half a real second, whatever the clock is doing.
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		Centre->Advance(1.0 / 60.0);
	}
	TestEqual(TEXT("half a real second in, the toast is still up however fast the sim runs"),
		Centre->Entries().Num(), 1);
	TestTrue(TEXT("and only half a real second has passed on the centre's own clock"),
		Centre->Now() < 0.6);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNotificationAlertKeyTest,
	"AirportMgr.UI.AlertsAreKeyedBySource",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNotificationAlertKeyTest::RunTest(const FString& Parameters)
{
	UNotificationCentre* Centre = NewObject<UNotificationCentre>(GetTransientPackage());

	// A condition re-detected every tick must not stack eighty copies of itself.
	Centre->RaiseAlert(TEXT("Route.NoStand"), FText::FromString(TEXT("No route from runway 10")));
	Centre->RaiseAlert(TEXT("Route.NoStand"), FText::FromString(TEXT("No route from runway 10")));
	TestEqual(TEXT("raising the same source twice yields one entry"), Centre->Entries().Num(), 1);

	// And an alert does NOT expire on time - it lasts while the condition does.
	Centre->Advance(10000.0);
	TestEqual(TEXT("an alert does not expire on time"), Centre->Entries().Num(), 1);

	Centre->ClearAlert(TEXT("Route.NoStand"));
	TestEqual(TEXT("clearing removes it"), Centre->Entries().Num(), 0);
	return true;
}

/**
 * The bound drops FEED entries only. An alert buried by a burst of chatter would be a live
 * problem the player was told about once and then quietly stopped being told about.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNotificationAlertSurvivesChatterTest,
	"AirportMgr.UI.AlertsSurviveTheScrollbackBound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNotificationAlertSurvivesChatterTest::RunTest(const FString& Parameters)
{
	UNotificationCentre* Centre = NewObject<UNotificationCentre>(GetTransientPackage());
	Centre->FeedLifetimeRealSeconds = 10000.0;

	Centre->RaiseAlert(TEXT("Route.NoStand"), FText::FromString(TEXT("No route to any stand")));
	for (int32 I = 0; I < 200; ++I)
	{
		Centre->PostFeed(FText::FromString(FString::Printf(TEXT("chatter %d"), I)));
	}

	int32 Alerts = 0;
	for (const FNotificationEntry& Entry : Centre->Entries())
	{
		if (Entry.Kind == ENotificationKind::Alert)
		{
			++Alerts;
		}
	}
	TestEqual(TEXT("the alert is still there after 200 feed entries pushed past the bound"), Alerts, 1);
	return true;
}

#endif
