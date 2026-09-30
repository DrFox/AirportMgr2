#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/OpsDefinition.h"
#include "Model/SimClock.h"
#include "SunPath.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathNoonTest,
	"AirportMgr.Sky.SunPath.NoonIsThePeak",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathNoonTest::RunTest(const FString& Parameters)
{
	const FSunPath Path;

	// Noon is the peak BY CONSTRUCTION: the hand-picked -42 degrees the art direction was
	// judged against is not a magic number any more, it is the top of this curve. If this
	// drifts, the sun angle every screenshot was approved at has silently moved.
	const FSunLighting Noon = Path.At(0.5);
	TestEqual(TEXT("noon pitch is minus MaxElevation"), Noon.Rotation.Pitch, -Path.MaxElevationDegrees, 1e-6);
	TestEqual(TEXT("noon temperature is the noon value"), Noon.TemperatureKelvin, Path.NoonTemperatureKelvin, 1e-3f);
	TestEqual(TEXT("noon intensity is the noon value"), Noon.Intensity, Path.NoonIntensity, 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathFloorTest,
	"AirportMgr.Sky.SunPath.NeverBelowTheFloor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathFloorTest::RunTest(const FString& Parameters)
{
	const FSunPath Path;

	// THE WHOLE POINT OF THE SLICE. A literal sun would put 40% of play time in the dark
	// with no runway lights and exposure locked, which is unreadable rather than
	// atmospheric. Sampling every 1/64 of a day catches a floor that holds at the obvious
	// hours and fails between them.
	for (int32 Step = 0; Step < 64; ++Step)
	{
		const double Fraction = Step / 64.0;
		const FSunLighting At = Path.At(Fraction);
		const double Elevation = -At.Rotation.Pitch;
		TestTrue(
			*FString::Printf(TEXT("elevation %.3f at fraction %.4f is at or above the floor %.3f"),
				Elevation, Fraction, Path.MinElevationDegrees),
			Elevation >= Path.MinElevationDegrees - 1e-6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathMidnightContinuityTest,
	"AirportMgr.Sky.SunPath.AzimuthIsContinuousAcrossMidnight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathMidnightContinuityTest::RunTest(const FString& Parameters)
{
	const FSunPath Path;

	// A jump here would read as the sun teleporting once per game day - at x8 that is every
	// 2.5 real minutes, so it would be seen. Compared on the NORMALISED delta because the
	// two yaws are 360 degrees apart in raw value and identical as directions.
	const FSunLighting Before = Path.At(0.9999);
	const FSunLighting After = Path.At(0.0);
	const double Delta = FMath::Abs(FRotator::NormalizeAxis(After.Rotation.Yaw - Before.Rotation.Yaw));
	TestTrue(*FString::Printf(TEXT("yaw step across midnight is %.4f degrees, expected under 1"), Delta),
		Delta < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathDawnRisesTest,
	"AirportMgr.Sky.SunPath.ElevationRisesFromDawnToNoon",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathDawnRisesTest::RunTest(const FString& Parameters)
{
	const FSunPath Path;

	// Monotonic, not merely higher at the ends: a curve that dipped in the middle of the
	// morning would read as weather rather than as time passing.
	double Previous = -Path.At(0.25).Rotation.Pitch;
	for (int32 Step = 1; Step <= 16; ++Step)
	{
		const double Fraction = 0.25 + (0.25 * Step) / 16.0;
		const double Elevation = -Path.At(Fraction).Rotation.Pitch;
		TestTrue(*FString::Printf(TEXT("elevation at %.4f (%.3f) is at or above the previous (%.3f)"),
			Fraction, Elevation, Previous), Elevation >= Previous - 1e-6);
		Previous = Elevation;
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathDuskWarmsTest,
	"AirportMgr.Sky.SunPath.DuskIsWarmerAndDimmer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathDuskWarmsTest::RunTest(const FString& Parameters)
{
	const FSunPath Path;

	// Warmth is most of what sells time of day; dimness is what a locked exposure then
	// shows honestly rather than compensating away.
	const FSunLighting Midnight = Path.At(0.0);
	TestEqual(TEXT("midnight temperature is the dusk value"), Midnight.TemperatureKelvin, Path.DuskTemperatureKelvin, 1e-3f);
	TestEqual(TEXT("midnight intensity is the dusk fraction of noon"), Midnight.Intensity,
		Path.NoonIntensity * Path.DuskIntensityFraction, 1e-3f);
	TestTrue(TEXT("dusk is dimmer than noon"), Midnight.Intensity < Path.At(0.5).Intensity);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathDuskIsTheClocksTest,
	"AirportMgr.Sky.SunPath.NightStartsAtTheClocksDusk",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathDuskIsTheClocksTest::RunTest(const FString& Parameters)
{
	// NIGHT IS DEFINED ONCE (#447): the clock's daylight is the scenario's DawnHour..DuskHour, 6..20 by default, and it is what the
	// time compression, the demand curve and the inbox's night shading all follow. The sky hard-coded 06:00-18:00, so for two
	// game hours a day the field was lit as night while everything else said day.
	const FSunPath Path;
	const double Floor = Path.MinElevationDegrees;
	const auto ElevationAt = [&Path](double Hour) { return -Path.At(Hour / 24.0).Rotation.Pitch; };
	TestTrue(*FString::Printf(TEXT("19:30 is still day: %.2f degrees, floor %.2f"), ElevationAt(19.5), Floor), ElevationAt(19.5) > Floor + 0.5);
	TestTrue(*FString::Printf(TEXT("20:30 is night: %.2f degrees, floor %.2f"), ElevationAt(20.5), Floor), FMath::IsNearlyEqual(ElevationAt(20.5), Floor, 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathHoursMoveTheSkyTest,
	"AirportMgr.Sky.SunPath.TheHoursAreThePaths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathHoursMoveTheSkyTest::RunTest(const FString& Parameters)
{
	// A SCENARIO THAT MOVES THE CLOCK'S DAWN OR DUSK MOVES THE SKY (#447): the path has the hours, not a sky-only 06:00 and 18:00.
	const auto ElevationAt = [](const FSunPath& Path, double Hour) { return -Path.At(Hour / 24.0).Rotation.Pitch; };

	FSunPath Late;
	Late.DuskHour = 22.0;
	TestTrue(TEXT("dusk at 22: 21:30 is still day"), ElevationAt(Late, 21.5) > Late.MinElevationDegrees + 0.5);
	TestTrue(TEXT("and 22:30 is night"), FMath::IsNearlyEqual(ElevationAt(Late, 22.5), Late.MinElevationDegrees, 1e-6));

	FSunPath Early;
	Early.DuskHour = 18.0;
	TestTrue(TEXT("dusk at 18 (the old sky): 17:30 is day"), ElevationAt(Early, 17.5) > Early.MinElevationDegrees + 0.5);
	TestTrue(TEXT("and 18:30 is night - the two hours the old sky could not tell from the clock's are the same now"),
		FMath::IsNearlyEqual(ElevationAt(Early, 18.5), Early.MinElevationDegrees, 1e-6));

	FSunPath Sleepy;
	Sleepy.DawnHour = 8.0;
	TestTrue(TEXT("dawn at 8: 07:30 is night"), FMath::IsNearlyEqual(ElevationAt(Sleepy, 7.5), Sleepy.MinElevationDegrees, 1e-6));
	TestTrue(TEXT("and 08:30 is day"), ElevationAt(Sleepy, 8.5) > Sleepy.MinElevationDegrees + 0.1);

	// NOON STAYS THE PEAK AT 12:00, whatever the hours, and the arc is continuous onto the floor at both ends (no snapping).
	for (const FSunPath* Path : { &Late, &Early, &Sleepy })
	{
		TestTrue(TEXT("noon is the peak"), FMath::IsNearlyEqual(ElevationAt(*Path, 12.0), Path->MaxElevationDegrees, 1e-6));
		TestTrue(TEXT("and dusk meets the floor"), FMath::IsNearlyEqual(ElevationAt(*Path, Path->DuskHour), Path->MinElevationDegrees, 1e-6));
		TestTrue(TEXT("and so does dawn"), FMath::IsNearlyEqual(ElevationAt(*Path, Path->DawnHour), Path->MinElevationDegrees, 1e-6));
	}

	// A DAY WITH NO ARC on a side (dusk at or before noon) sits at the floor rather than dividing by zero.
	FSunPath Degenerate;
	Degenerate.DuskHour = 12.0;
	TestTrue(TEXT("a dusk at noon gives no afternoon arc, and no NaN"), FMath::IsNearlyEqual(ElevationAt(Degenerate, 15.0), Degenerate.MinElevationDegrees, 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathDefaultsAreTheScenariosTest,
	"AirportMgr.Sky.SunPath.DefaultsAreTheScenarios",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathDefaultsAreTheScenariosTest::RunTest(const FString& Parameters)
{
	// THE BARE PATH'S HOURS ARE A COPY of the scenario's defaults (and the clock's own), which a struct that cannot include the clock must be -
	// so this is what keeps "night is defined once" true of the copy: change one default and this goes red, instead of the editor's sky
	// quietly using a day the game does not.
	const FSunPath Path;
	const UScenario* Scenario = GetDefault<UScenario>();
	const USimClock* Clock = GetDefault<USimClock>();
	TestEqual(TEXT("dawn: the path's default is the scenario's"), Path.DawnHour, Scenario->DawnHour);
	TestEqual(TEXT("dusk: the path's default is the scenario's"), Path.DuskHour, Scenario->DuskHour);
	TestEqual(TEXT("dawn: and the bare clock's"), Path.DawnHour, Clock->DawnHour);
	TestEqual(TEXT("dusk: and the bare clock's"), Path.DuskHour, Clock->DuskHour);
	return true;
}

#endif
