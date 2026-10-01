#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MiniatureFocus.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMiniatureFocusSharpNearSoftFarTest,
	"AirportMgr.Sky.MiniatureFocus.SharpNearSoftFar",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMiniatureFocusSharpNearSoftFarTest::RunTest(const FString& Parameters)
{
	const FMiniatureFocus Focus;
	const double Fov = 75.0;

	// THE OWNER'S PICTURE (2026-10-01): at close zoom, looking across the field, aircraft
	// within ~50 m are sharp and those further off go soft. Measured through the renderer's
	// own formula at the closest zoom (600 uu) and at the full-strength edge of the fade.
	for (const double ZoomUu : {600.0, Focus.FullBlurZoomUu})
	{
		const double Sensor = Focus.SensorWidthMm(ZoomUu, Fov);
		TestTrue(*FString::Printf(TEXT("a lens at zoom %.0f uu"), ZoomUu), Sensor > 0.0);

		const double S = Focus.SharpDistanceUu;
		TestEqual(*FString::Printf(TEXT("the sharp distance is in focus at zoom %.0f"), ZoomUu),
			Focus.BlurFraction(Sensor, S, S, Fov), 0.0, 1e-12);
		TestEqual(*FString::Printf(TEXT("twice the sharp distance gets half BlurAtInfinity at zoom %.0f"), ZoomUu),
			Focus.BlurFraction(Sensor, S, 2.0 * S, Fov), 0.5 * Focus.BlurAtInfinity, 1e-9);

		// Farther is softer: an aircraft across the field blurs more than one just past 50 m.
		const double Near = Focus.BlurFraction(Sensor, S, 1.2 * S, Fov);
		const double Far = Focus.BlurFraction(Sensor, S, 10.0 * S, Fov);
		TestTrue(*FString::Printf(TEXT("farther is softer at zoom %.0f"), ZoomUu), Far > Near && Near > 0.0);
	}

	// Nearer than the focus the formula returns a NON-zero blur, and that is the point of
	// r.DOF.Kernel.MaxForegroundRadius=0: the renderer clamps it, this struct cannot. If this
	// ever reads zero, the struct has grown a near-field rule and the ini cap may be stale.
	const double Sensor = Focus.SensorWidthMm(600.0, Fov);
	TestTrue(TEXT("the thin lens alone would blur the near field - the ini cap is load-bearing"),
		Focus.BlurFraction(Sensor, Focus.SharpDistanceUu, 0.5 * Focus.SharpDistanceUu, Fov) > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMiniatureFocusZoomOutFadeTest,
	"AirportMgr.Sky.MiniatureFocus.FadesOutAsTheViewClimbs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMiniatureFocusZoomOutFadeTest::RunTest(const FString& Parameters)
{
	const FMiniatureFocus Focus;
	const double Fov = 75.0;

	// THE 2026-09-30 BUG, pinned. At max zoom (60000 uu) the old lens blurred a ~20 px
	// aircraft by ~4 px; from the plan view up there must be NO lens at all - a zero sensor,
	// which the component turns into cleared overrides - not merely a weak one.
	for (const double ZoomUu : {Focus.NoBlurZoomUu, 8000.0, 15000.0, 60000.0})
	{
		TestEqual(*FString::Printf(TEXT("no lens at zoom %.0f uu"), ZoomUu), Focus.SensorWidthMm(ZoomUu, Fov), 0.0);
	}
	TestEqual(TEXT("full strength at the closest zoom"), Focus.BlurAt(600.0), Focus.BlurAtInfinity, 1e-12);

	// Monotonic through the fade: zooming out never makes the far field softer.
	double Last = Focus.BlurAt(Focus.FullBlurZoomUu);
	TestEqual(TEXT("full strength at the fade's near end"), Last, Focus.BlurAtInfinity, 1e-12);
	for (int32 Step = 1; Step <= 10; ++Step)
	{
		const double Z = FMath::Lerp(Focus.FullBlurZoomUu, Focus.NoBlurZoomUu, Step / 10.0);
		const double Blur = Focus.BlurAt(Z);
		TestTrue(*FString::Printf(TEXT("blur at zoom %.0f uu does not rise"), Z), Blur <= Last);
		Last = Blur;
	}
	TestEqual(TEXT("nothing at the fade's far end"), Last, 0.0, 1e-12);
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
	TestEqual(TEXT("zero FOV"), Focus.SensorWidthMm(600.0, 0.0), 0.0);
	TestEqual(TEXT("180 FOV"), Focus.SensorWidthMm(600.0, 180.0), 0.0);
	FMiniatureFocus NoFocus;
	NoFocus.SharpDistanceUu = 0.0;
	TestEqual(TEXT("zero sharp distance"), NoFocus.SensorWidthMm(600.0, 75.0), 0.0);
	Focus.BlurAtInfinity = 0.0;
	TestEqual(TEXT("zero blur asked for"), Focus.SensorWidthMm(600.0, 75.0), 0.0);
	return true;
}

#endif
