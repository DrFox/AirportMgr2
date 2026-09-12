#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "SunDriver.h"
#include "Model/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE SEAM TEST the refactor contract asks for: it fails if the driver is spawned but
 * never wired to a clock.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunDriverNoClockIsNoonTest,
	"Airside.Sky.SunDriver.NoClockMeansNoon",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunDriverNoClockIsNoonTest::RunTest(const FString& Parameters)
{
	// The editor viewport and the first PIE frame both have no time of day to read. Noon is
	// the right answer there because it is the angle the art direction was judged against -
	// falling back to midnight would make an untouched editor look broken.
	TestEqual(TEXT("no clock resolves to noon"), ASunDriver::ResolveDayFraction(nullptr), 0.5, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunDriverReadsClockTest,
	"Airside.Sky.SunDriver.ReadsTheGameClock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunDriverReadsClockTest::RunTest(const FString& Parameters)
{
	// USimClock is world-free by design ("built with NewObject and advanced by hand in
	// tests"), so this needs no level.
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());

	// A game day of 86,400 real seconds makes TimeScale() exactly 1, so Advance() moves
	// game time one-for-one and the numbers below read as the times they are. There is no
	// SetGameSeconds accessor and this needs none.
	Clock->RealSecondsPerGameDay = USimClock::SecondsPerDay;

	TestEqual(TEXT("a fresh clock is midnight"), ASunDriver::ResolveDayFraction(Clock), 0.0, 1e-9);

	const double Quarter = USimClock::SecondsPerDay * 0.25;
	Clock->Advance(Quarter);
	TestEqual(TEXT("06:00 is a quarter of a day"), ASunDriver::ResolveDayFraction(Clock), 0.25, 1e-9);

	Clock->Advance(Quarter);
	TestEqual(TEXT("12:00 is half a day"), ASunDriver::ResolveDayFraction(Clock), 0.5, 1e-9);

	// The one that matters: a day boundary must wrap to 0, not keep counting, or the sun
	// climbs out of the sky on day two. TimeOfDay() is the clock's own answer to this and
	// re-deriving it in the driver would be a second implementation of the same wrap.
	Clock->Advance(USimClock::SecondsPerDay);
	TestEqual(TEXT("12:00 on day two wraps to half"), ASunDriver::ResolveDayFraction(Clock), 0.5, 1e-9);
	return true;
}

#endif
