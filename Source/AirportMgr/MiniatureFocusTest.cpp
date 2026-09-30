#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MiniatureFocus.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMiniatureFocusConstantBlurTest,
	"AirportMgr.Sky.MiniatureFocus.SameBlurAtEveryZoom",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMiniatureFocusConstantBlurTest::RunTest(const FString& Parameters)
{
	const FMiniatureFocus Focus;
	const double Fov = 75.0;

	// THE REASON THE STRUCT EXISTS. A fixed lens blurs nothing at 600 m and everything at
	// 6 m; the scaled sensor must give the SAME background blur everywhere the effect is at
	// full strength (FullBlurDistanceUu to the 60000 uu limit - the close-zoom fade has its
	// own test below). Measured through the renderer's own formula, at twice the focus
	// distance where the depth term is exactly one half.
	for (const double FocusUu : {Focus.FullBlurDistanceUu, 15000.0, 30000.0, 60000.0})
	{
		const double Sensor = Focus.SensorWidthMm(FocusUu, Fov);
		TestTrue(*FString::Printf(TEXT("sensor positive at %.0f uu"), FocusUu), Sensor > 0.0);

		const double AtDouble = Focus.BlurFraction(Sensor, FocusUu, 2.0 * FocusUu, Fov);
		TestEqual(*FString::Printf(TEXT("blur at 2x focus is half BlurAtInfinity at %.0f uu"), FocusUu),
			AtDouble, 0.5 * Focus.BlurAtInfinity, 1e-9);

		const double AtFocus = Focus.BlurFraction(Sensor, FocusUu, FocusUu, Fov);
		TestEqual(*FString::Printf(TEXT("the focus point itself is sharp at %.0f uu"), FocusUu),
			AtFocus, 0.0, 1e-12);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMiniatureFocusCloseZoomFadeTest,
	"AirportMgr.Sky.MiniatureFocus.FadesOutAtCloseZoom",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMiniatureFocusCloseZoomFadeTest::RunTest(const FString& Parameters)
{
	const FMiniatureFocus Focus;
	const double Fov = 75.0;

	// samples/blur.png: at close zoom the subject is often not the focus point, so the lens
	// must be switched OFF there (a zero sensor), not merely weakened.
	TestEqual(TEXT("no lens at the closest zoom"), Focus.SensorWidthMm(600.0, Fov), 0.0);
	TestEqual(TEXT("no lens at the fade's near end"), Focus.SensorWidthMm(Focus.NoBlurDistanceUu, Fov), 0.0);

	// Monotonic through the fade: a zoom-out never sharpens the background.
	double Last = 0.0;
	for (int32 Step = 1; Step <= 10; ++Step)
	{
		const double D = FMath::Lerp(Focus.NoBlurDistanceUu, Focus.FullBlurDistanceUu, Step / 10.0);
		const double Blur = Focus.BlurAt(D);
		TestTrue(*FString::Printf(TEXT("blur at %.0f uu does not fall"), D), Blur >= Last);
		Last = Blur;
	}
	TestEqual(TEXT("full strength at the far end"), Last, Focus.BlurAtInfinity, 1e-12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMiniatureFocusDegenerateTest,
	"AirportMgr.Sky.MiniatureFocus.DegenerateInputBlursNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMiniatureFocusDegenerateTest::RunTest(const FString& Parameters)
{
	// Zero is the safe failure: the component treats a zero sensor as "leave DOF off", so a
	// bad FOV or a camera not yet placed can never smear the whole frame.
	FMiniatureFocus Focus;
	TestEqual(TEXT("zero focus distance"), Focus.SensorWidthMm(0.0, 75.0), 0.0);
	TestEqual(TEXT("zero FOV"), Focus.SensorWidthMm(8000.0, 0.0), 0.0);
	TestEqual(TEXT("180 FOV"), Focus.SensorWidthMm(8000.0, 180.0), 0.0);
	Focus.BlurAtInfinity = 0.0;
	TestEqual(TEXT("zero blur asked for"), Focus.SensorWidthMm(8000.0, 75.0), 0.0);
	return true;
}

#endif
