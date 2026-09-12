#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Present/AirsideAgentAnim.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE PROPELLER READS AS TURNING FORWARDS AT EVERY FRAME RATE.
 *
 * REPORTED FROM PLAY (2026-09-12): "zooming out on an aircraft changes the animation on the
 * props, they are slower when further out and seem to spin faster when the camera is closer".
 * It was doing exactly that, and the camera was the cause only indirectly - a zoomed-in
 * aircraft fills more screen, costs more to draw, and lands the frame rate on a different
 * alias.
 *
 * A three-blade propeller at 2200 RPM turns 36.7 times a second and repeats every 120
 * degrees. Sampled at 90 fps it appears to turn forwards at 6.67 rev/s; at 110 fps it stands
 * DEAD STILL; at 120 and 144 it runs backwards. None of those is the real rotation, and no
 * frame rate this game can reach would show it - 36.7 rev/s needs thousands of samples a
 * second.
 *
 * ASSERTS THE SAMPLING PROPERTY, not a chosen speed. What matters is that a step never
 * crosses half a blade-repeat, because that is precisely the point at which a forward turn
 * becomes indistinguishable from a backward one. Checked across the whole range of frame
 * rates the game might run at, including the ones measured to fail before the fix.
 *
 * The model is deliberately NOT involved: FAgentMotion::EngineRPM keeps the true figure and
 * this is the view choosing a rotation it can show.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPropAliasingTest,
	"Airside.Present.PropellerNeverAliasesBackwards",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPropAliasingTest::RunTest(const FString& Parameters)
{
	const int32 Blades = 3;
	const float Fraction = 0.333f;
	const float Repeat = 360.0f / Blades;

	// 110 and 120 are the rates measured to freeze and reverse this propeller in play; the
	// rest bracket what the game will actually run at, down to a hitching 24.
	const float Rates[] = { 24.0f, 30.0f, 60.0f, 75.0f, 90.0f, 100.0f, 110.0f, 120.0f, 144.0f, 240.0f };

	for (const float Fps : Rates)
	{
		const float Step = UAirsideAgentAnim::PropStepDegrees(2200.0f, 1.0f / Fps, Blades, Fraction);

		// THE ASSERTION THE BUG WOULD FAIL. Unclamped, 2200 RPM at 60 fps steps 220 degrees -
		// well past the 60 that a 120 degree repeat can carry - and reads as backwards.
		TestTrue(*FString::Printf(
			TEXT("%.0f fps: the step stays inside half a blade repeat (%.1f deg, limit %.1f)"),
			Fps, Step, Repeat * 0.5f), Step < Repeat * 0.5f);

		TestTrue(*FString::Printf(TEXT("%.0f fps: and it still turns forwards (%.1f deg)"), Fps, Step),
			Step > 0.0f);
	}

	// A SLOW PROPELLER IS NOT TOUCHED. Shutting down or winding up, the blades are inside what
	// the frame rate can show and must turn at their real rate - a clamp that always bound
	// would make every propeller in the game turn at the same speed.
	const float Idle = UAirsideAgentAnim::PropStepDegrees(300.0f, 1.0f / 60.0f, Blades, Fraction);
	TestTrue(*FString::Printf(TEXT("a slow propeller turns at its real rate (%.3f deg, wanted %.3f)"),
		Idle, 300.0f * 6.0f / 60.0f),
		FMath::IsNearlyEqual(Idle, 300.0f * 6.0f / 60.0f, 0.001f));

	// A STOPPED ONE STAYS STOPPED, rather than creeping forward on the clamp.
	TestEqual(TEXT("a stopped propeller does not turn"),
		UAirsideAgentAnim::PropStepDegrees(0.0f, 1.0f / 60.0f, Blades, Fraction), 0.0f);

	// A two-blade propeller repeats every 180 degrees and may therefore turn further per
	// frame than a three-blade one. Pins that the limit comes from the BLADES, not a constant.
	const float Two = UAirsideAgentAnim::PropStepDegrees(2200.0f, 1.0f / 60.0f, 2, Fraction);
	const float Three = UAirsideAgentAnim::PropStepDegrees(2200.0f, 1.0f / 60.0f, 3, Fraction);
	TestTrue(*FString::Printf(TEXT("fewer blades may turn further (%.1f vs %.1f deg)"), Two, Three),
		Two > Three);
	return true;
}

#endif
