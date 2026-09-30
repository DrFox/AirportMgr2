#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "SunDriver.h"
#include "Model/OpsDefinition.h"
#include "Model/SimClock.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE SEAM TEST the refactor contract asks for: it fails if the driver is spawned but
 * never wired to a clock.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunDriverNoClockIsNoonTest,
	"AirportMgr.Sky.SunDriver.NoClockMeansNoon",
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
	"AirportMgr.Sky.SunDriver.ReadsTheGameClock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunDriverReadsClockTest::RunTest(const FString& Parameters)
{
	// USimClock is world-free by design ("built with NewObject and advanced by hand in
	// tests"), so this needs no level.
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());

	// A game day of 86,400 real seconds makes TimeScale() exactly 1, so Advance() moves
	// game time one-for-one and the numbers below read as the times they are. There is no
	// SetGameSeconds accessor and this needs none.
	Clock->SetUniformDay(USimClock::SecondsPerDay);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunDriverDuskIsTheClocksTest,
	"AirportMgr.Sky.SunDriver.DuskIsTheClocks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunDriverDuskIsTheClocksTest::RunTest(const FString& Parameters)
{
	// THE WIRING a test of FSunPath alone cannot see (#447): the driver hands the CLOCK's dawn and dusk to the path. Night is defined once - the
	// clock's, the scenario's - so a dusk of 20 leaves the field lit at 19:30 and dark at 20:30, and a scenario that moves DuskHour moves the sky.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	ASunDriver* Driver = TestWorld.World != nullptr ? TestWorld.World->SpawnActor<ASunDriver>() : nullptr;
	if (!TestNotNull(TEXT("a sun driver"), Driver)) { return false; }
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	const auto ElevationAt = [&](double Hour) { return -Driver->MakePath(Clock).At(Hour / 24.0).Rotation.Pitch; };
	const double Floor = Driver->MinElevationDegrees;

	Clock->DuskHour = 20.0;
	TestTrue(*FString::Printf(TEXT("dusk 20: 19:30 is above the floor (%.2f vs %.2f)"), ElevationAt(19.5), Floor), ElevationAt(19.5) > Floor + 0.5);
	TestTrue(*FString::Printf(TEXT("dusk 20: 20:30 is at the floor (%.2f)"), ElevationAt(20.5)), FMath::IsNearlyEqual(ElevationAt(20.5), Floor, 1e-6));

	Clock->DuskHour = 18.0;
	TestTrue(TEXT("a scenario that moves dusk to 18 moves the sky: 19:30 is now night"), FMath::IsNearlyEqual(ElevationAt(19.5), Floor, 1e-6));
	Clock->DuskHour = 22.0;
	TestTrue(TEXT("and to 22: 21:30 is day"), ElevationAt(21.5) > Floor + 0.5);

	Clock->DuskHour = 20.0;
	Clock->DawnHour = 8.0;
	TestTrue(TEXT("dawn follows too: 07:30 is night at dawn 8"), FMath::IsNearlyEqual(ElevationAt(7.5), Floor, 1e-6));

	// NO CLOCK (the editor): the scenario's own defaults, not a third set of numbers.
	double Dawn = -1.0;
	double Dusk = -1.0;
	ASunDriver::ResolveDaylightHours(nullptr, Dawn, Dusk);
	TestEqual(TEXT("no clock: the scenario's default dawn"), Dawn, GetDefault<UScenario>()->DawnHour);
	TestEqual(TEXT("no clock: the scenario's default dusk"), Dusk, GetDefault<UScenario>()->DuskHour);
	const FSunPath Editor = Driver->MakePath(nullptr);
	TestEqual(TEXT("and the path the driver builds with no clock carries them"), Editor.DuskHour, GetDefault<UScenario>()->DuskHour);
	return true;
}

#endif
