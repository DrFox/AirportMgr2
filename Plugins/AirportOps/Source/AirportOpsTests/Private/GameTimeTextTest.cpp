#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/GameTimeText.h"
#include "Model/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

// GAME TIME AS THE PLAYER READS IT, IN ONE PLACE (#447). The depot card printed "+95 min" beside the aircraft card's "1 h 35 min"; the bar's clock and
// the ledger's rows each formatted "Day N  HH:MM" and the ledger re-derived the day from seconds. Every reader goes through GameTimeText now.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameTimeTextDurationTest, "AirportOps.Model.GameTimeText.DurationIsTheClocksWords",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGameTimeTextDurationTest::RunTest(const FString&)
{
	const auto Words = [](double Seconds) { return GameTimeText::Duration(Seconds).ToString(); };
	TestEqual(TEXT("nothing"), Words(0.0), FString(TEXT("0 min")));
	TestEqual(TEXT("a quarter of an hour"), Words(15.0 * 60.0), FString(TEXT("15 min")));
	TestEqual(TEXT("under the hour stays in minutes"), Words(59.0 * 60.0), FString(TEXT("59 min")));
	TestEqual(TEXT("an hour"), Words(3600.0), FString(TEXT("1 h")));
	TestEqual(TEXT("the depot card's 95 minutes are the aircraft card's 1 h 35 min"), Words(95.0 * 60.0), FString(TEXT("1 h 35 min")));
	TestEqual(TEXT("whole hours drop the minutes"), Words(2.0 * 3600.0), FString(TEXT("2 h")));
	TestEqual(TEXT("ROUNDED to the minute, as the turnaround key recomputes it: 29 s is none, 31 s is one"), Words(29.0), FString(TEXT("0 min")));
	TestEqual(TEXT("31 s rounds up"), Words(31.0), FString(TEXT("1 min")));
	TestEqual(TEXT("a negative span is never printed negative"), Words(-500.0), FString(TEXT("0 min")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameTimeTextStampTest, "AirportOps.Model.GameTimeText.StampIsDayAndClockFace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGameTimeTextStampTest::RunTest(const FString&)
{
	TestEqual(TEXT("the first instant is day 1, midnight"), GameTimeText::Stamp(0.0), FString(TEXT("Day 1  00:00")));
	TestEqual(TEXT("09:05 on day 2"), GameTimeText::Stamp(USimClock::SecondsPerDay + 9.0 * 3600.0 + 5.0 * 60.0), FString(TEXT("Day 2  09:05")));
	TestEqual(TEXT("truncated as a clock face is: 23:59:59 is 23:59, still day 1"), GameTimeText::Stamp(USimClock::SecondsPerDay - 1.0), FString(TEXT("Day 1  23:59")));
	TestEqual(TEXT("a time of day alone"), GameTimeText::TimeOfDay(14.0 * 3600.0 + 7.0 * 60.0 + 30.0), FString(TEXT("14:07")));

	// THE DAY IS THE CLOCK'S: a stamp of the clock's own Now never disagrees with USimClock::Day (the ledger re-derived it).
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	Clock->SetUniformDay(USimClock::SecondsPerDay);
	for (int32 Step = 0; Step < 6; ++Step)
	{
		TestTrue(*FString::Printf(TEXT("'%s' is day %d"), *GameTimeText::Stamp(Clock->Now()), Clock->Day() + 1),
			GameTimeText::Stamp(Clock->Now()).StartsWith(FString::Printf(TEXT("Day %d  "), Clock->Day() + 1)));
		Clock->Advance(USimClock::SecondsPerDay * 0.37);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimClockMovementTest, "AirportOps.Model.SimClock.MovementSecondsConvertAtTheBandsRate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FSimClockMovementTest::RunTest(const FString&)
{
	// MOVEMENT SECONDS (real x speed - what agents run on) AS GAME SECONDS: x the day's compression at the clock's time of day now. The hold line's
	// stall and a bid's drive were each converted by their own copy of this (#447); it is the clock's.
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	Clock->SetUniformDay(1200.0);   // 72 game seconds a real second, all day
	TestEqual(TEXT("80 movement seconds at a 1200 s day are 96 game minutes"), Clock->GameSecondsOfMovement(80.0), 5760.0);

	// TWO BANDS: the day's compression differs by hour, so the same span is worth more at night. 14 h of daylight in 2400 s = 21 game s per s;
	// 10 h of night in 480 s = 75.
	Clock->RealSecondsDaylight = 2400.0;
	Clock->RealSecondsNight = 480.0;
	Clock->StartAtHour(9.0);
	TestEqual(TEXT("09:00, in the day band"), Clock->GameSecondsOfMovement(10.0), 10.0 * 21.0);
	Clock->StartAtHour(3.0);
	TestEqual(TEXT("03:00, in the night band"), Clock->GameSecondsOfMovement(10.0), 10.0 * 75.0);
	TestEqual(TEXT("and it is linear in the span, so a bid's per-second rate is GameSecondsOfMovement(1)"),
		Clock->GameSecondsOfMovement(1.0) * 10.0, Clock->GameSecondsOfMovement(10.0));
	TestEqual(TEXT("it is the rate NOW, which is GameSecondsPerRealSecond at the time of day - not TimeScale, which carries the speed too"),
		Clock->GameSecondsOfMovement(1.0), Clock->GameSecondsPerRealSecond(Clock->TimeOfDay()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimClockBandEdgeTest, "AirportOps.Model.SimClock.BandEdgeIsWhereTheRateChanges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FSimClockBandEdgeTest::RunTest(const FString&)
{
	// #445: the next dawn or dusk, in game seconds - and it is where the rate REALLY changes, measured by advancing the clock across it rather
	// than by restating the formula: one second of real time either side of the edge is worth the day's rate and then the night's.
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	Clock->RealSecondsDaylight = 2400.0;
	Clock->RealSecondsNight = 480.0;
	Clock->DawnHour = 6.0;
	Clock->DuskHour = 20.0;
	Clock->StartAtHour(19.0);
	TestEqual(TEXT("an hour before dusk"), Clock->GameSecondsToBandEdge(), 3600.0);
	Clock->StartAtHour(21.0);
	TestEqual(TEXT("21:00: dawn is nine hours on, across midnight"), Clock->GameSecondsToBandEdge(), 9.0 * 3600.0);
	Clock->StartAtHour(3.0);
	TestEqual(TEXT("03:00: dawn is three hours on"), Clock->GameSecondsToBandEdge(), 3.0 * 3600.0);

	Clock->StartAtHour(19.0);
	const double Edge = Clock->GameSecondsToBandEdge();
	const double DayRate = Clock->GameSecondsOfMovement(1.0);
	Clock->Advance(Edge / DayRate + 1.0);
	TestTrue(TEXT("one real second past the edge, the clock is in the night band"), Clock->GameSecondsOfMovement(1.0) > DayRate * 3.0);
	TestTrue(TEXT("and the next edge is dawn, hours away - not the one just crossed"), Clock->GameSecondsToBandEdge() > 3600.0);
	return true;
}

#endif
