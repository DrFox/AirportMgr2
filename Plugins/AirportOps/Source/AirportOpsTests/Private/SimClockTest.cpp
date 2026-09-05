#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimClockSpeedTest,
	"AirportOps.Model.SimClock.Speed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimClockSpeedTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	Clock->RealSecondsPerGameDay = 1200.0;  // 20 real minutes per day -> 72 game s per real s

	Clock->Advance(1.0);
	TestEqual(TEXT("x1 advances by the day compression alone"), Clock->Now(), 72.0, 1e-9);

	Clock->SetSpeed(ESimSpeed::X2);
	Clock->Advance(1.0);
	TestEqual(TEXT("x2 doubles the compressed rate"), Clock->Now(), 72.0 + 144.0, 1e-9);

	Clock->SetSpeed(ESimSpeed::Paused);
	Clock->Advance(10.0);
	TestEqual(TEXT("paused advances nothing however long the real step"), Clock->Now(), 216.0, 1e-9);

	TestEqual(TEXT("Multiplier is the speed table, not the compression"),
		USimClock::Multiplier(ESimSpeed::X8), 8.0, 1e-12);
	TestEqual(TEXT("paused multiplier is zero"), USimClock::Multiplier(ESimSpeed::Paused), 0.0, 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimClockDayTest,
	"AirportOps.Model.SimClock.Day",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimClockDayTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	Clock->RealSecondsPerGameDay = 1.0;  // one real second is one game day: makes the arithmetic readable

	Clock->Advance(1.5);
	TestEqual(TEXT("a day and a half is day 1"), Clock->Day(), 1);
	TestEqual(TEXT("time of day wraps to half a day"), Clock->TimeOfDay(), USimClock::SecondsPerDay * 0.5, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSimClockSchedulerTest,
	"AirportOps.Model.SimClock.Scheduler",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSimClockSchedulerTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>();
	Clock->RealSecondsPerGameDay = USimClock::SecondsPerDay;  // 1 real s == 1 game s, so steps read plainly

	TArray<FString> Fired;
	Clock->At(5.0, [&Fired]() { Fired.Add(TEXT("at5")); });
	Clock->Every(2.0, [&Fired]() { Fired.Add(TEXT("every2")); });
	const int32 Cancelled = Clock->At(3.0, [&Fired]() { Fired.Add(TEXT("cancelled")); });
	TestTrue(TEXT("a pending entry can be cancelled"), Clock->Cancel(Cancelled));
	TestFalse(TEXT("cancelling twice reports nothing to cancel"), Clock->Cancel(Cancelled));

	Clock->Advance(1.0);
	TestEqual(TEXT("nothing is due at t=1"), Fired.Num(), 0);

	// ONE big step. The point of the scheduler is that a large step drains every due entry
	// in TIME order, not that it fires whatever happens to be due at the end - a x8 frame
	// crossing several deliveries must post them all, oldest first.
	Clock->Advance(5.0);  // now t=6: every2 at 2,4,6; at5 at 5
	const TArray<FString> Expected = { TEXT("every2"), TEXT("every2"), TEXT("at5"), TEXT("every2") };
	TestEqual(TEXT("due entries fire in time order across one large step"), Fired, Expected);

	// An entry scheduled in the past fires on the next Advance rather than being lost.
	Clock->At(1.0, [&Fired]() { Fired.Add(TEXT("late")); });
	Clock->Advance(0.0);
	TestEqual(TEXT("a past-due entry fires on the next advance"), Fired.Last(), FString(TEXT("late")));

	TestEqual(TEXT("Every rejects a non-positive interval"), Clock->Every(0.0, []() {}), static_cast<int32>(INDEX_NONE));
	return true;
}

#endif
