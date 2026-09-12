#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "SunPath.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named under "Airside." although the path lives in the game module, for the same reason
 * BuildCameraRigTest gives: Run-AirsideTests.ps1 filters on that prefix, and a test the
 * pre-commit run does not pick up is one found failing by the next person to touch it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathNoonTest,
	"Airside.Sky.SunPath.NoonIsThePeak",
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
	"Airside.Sky.SunPath.NeverBelowTheFloor",
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
	"Airside.Sky.SunPath.AzimuthIsContinuousAcrossMidnight",
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
	"Airside.Sky.SunPath.ElevationRisesFromDawnToNoon",
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
	"Airside.Sky.SunPath.DuskIsWarmerAndDimmer",
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

#endif
