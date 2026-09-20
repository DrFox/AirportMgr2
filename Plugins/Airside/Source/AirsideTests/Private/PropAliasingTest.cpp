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

	// NO DISPLAY CAP HERE: this test is entirely about the STEP guard against aliasing, a
	// property #107 item 7 left untouched - see FPropDisplayCapTest below for the RPM cap
	// this file gained alongside it. A cap this large never binds at the RPM used here.
	const float NoCap = 1.0e6f;

	// 110 and 120 are the rates measured to freeze and reverse this propeller in play; the
	// rest bracket what the game will actually run at, down to a hitching 24.
	const float Rates[] = { 24.0f, 30.0f, 60.0f, 75.0f, 90.0f, 100.0f, 110.0f, 120.0f, 144.0f, 240.0f };

	for (const float Fps : Rates)
	{
		const float Step = UAirsideAgentAnim::PropStepDegrees(2200.0f, 1.0f / Fps, Blades, Fraction, NoCap);

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
	const float Idle = UAirsideAgentAnim::PropStepDegrees(300.0f, 1.0f / 60.0f, Blades, Fraction, NoCap);
	TestTrue(*FString::Printf(TEXT("a slow propeller turns at its real rate (%.3f deg, wanted %.3f)"),
		Idle, 300.0f * 6.0f / 60.0f),
		FMath::IsNearlyEqual(Idle, 300.0f * 6.0f / 60.0f, 0.001f));

	// A STOPPED ONE STAYS STOPPED, rather than creeping forward on the clamp.
	TestEqual(TEXT("a stopped propeller does not turn"),
		UAirsideAgentAnim::PropStepDegrees(0.0f, 1.0f / 60.0f, Blades, Fraction, NoCap), 0.0f);

	// A two-blade propeller repeats every 180 degrees and may therefore turn further per
	// frame than a three-blade one. Pins that the limit comes from the BLADES, not a constant.
	const float Two = UAirsideAgentAnim::PropStepDegrees(2200.0f, 1.0f / 60.0f, 2, Fraction, NoCap);
	const float Three = UAirsideAgentAnim::PropStepDegrees(2200.0f, 1.0f / 60.0f, 3, Fraction, NoCap);
	TestTrue(*FString::Printf(TEXT("fewer blades may turn further (%.1f vs %.1f deg)"), Two, Three),
		Two > Three);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * THE PROPELLER'S APPARENT SPEED DOES NOT CHANGE WITH THE FRAME RATE (#107 item 7).
 *
 * CONFIRMED, 2026-09-12 review, the same review that measured FPropAliasingTest's aliasing
 * figures above. PropStepDegrees used to clamp only the per-frame STEP - a fixed
 * degrees-per-FRAME ceiling - so degrees-per-SECOND, the apparent speed, rose with the frame
 * rate whenever that ceiling bound: 200/400/800 apparent RPM at 30/60/120 fps for this
 * airframe's 2200 RPM cruise and three-blade prop. Reported from play as the propeller
 * changing speed with the camera, which it was - zooming in cost more to draw and landed the
 * frame rate on a different point along that ramp.
 *
 * THE FIX CAPS THE RATE INSTEAD (PropDisplayCapRPM), so the shown speed is the same number
 * at every frame rate the cap is chosen to cover - here, 60 fps and up, matching the
 * DEFAULT figure's own justification (400 RPM at 60 fps exactly fills PropMaxStepPerRepeat's
 * budget for a three-blade prop, so the step guard does not additionally bind above that
 * rate). Below it the guard binds again and the apparent speed sags - the accepted
 * hitch fallback this project has always had, not a regression.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPropDisplayCapTest,
	"Airside.Present.PropellerApparentSpeedIsFrameRateIndependent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPropDisplayCapTest::RunTest(const FString& Parameters)
{
	const int32 Blades = 3;
	const float Fraction = 0.333f;
	const float Cap = 400.0f;       // UAirsideAgentAnim::PropDisplayCapRPM's own default
	const float TrueRPM = 2200.0f;  // this airframe's cruise RPM (2026-09-12 measurement)

	auto ApparentRPM = [Blades, Fraction, Cap, TrueRPM](float Fps)
	{
		const float Step = UAirsideAgentAnim::PropStepDegrees(TrueRPM, 1.0f / Fps, Blades, Fraction, Cap);
		return Step * Fps / 6.0f;
	};

	const float At60 = ApparentRPM(60.0f);
	const float At90 = ApparentRPM(90.0f);
	const float At120 = ApparentRPM(120.0f);
	const float At240 = ApparentRPM(240.0f);

	// THE ASSERTION THE BUG WOULD FAIL: before the fix these four were 400/600/800/1600 -
	// each proportional to its own frame rate - rather than one shared number.
	TestTrue(*FString::Printf(TEXT("60 fps shows the capped rate (%.0f RPM, want %.0f)"), At60, Cap),
		FMath::IsNearlyEqual(At60, Cap, 0.5f));
	TestTrue(*FString::Printf(TEXT("90 fps shows the SAME apparent speed as 60 fps (%.0f vs %.0f)"), At90, At60),
		FMath::IsNearlyEqual(At90, At60, 0.5f));
	TestTrue(*FString::Printf(TEXT("120 fps too (%.0f vs %.0f)"), At120, At60),
		FMath::IsNearlyEqual(At120, At60, 0.5f));
	TestTrue(*FString::Printf(TEXT("and 240 fps (%.0f vs %.0f)"), At240, At60),
		FMath::IsNearlyEqual(At240, At60, 0.5f));

	// BELOW THE CHOSEN MINIMUM, THE GUARD FALLS BACK - a hitching frame rate shows a slower
	// prop, never a faster or a backwards one. This is the accepted degradation, not a bug:
	// PropMaxStepPerRepeat still binds here exactly as it always has.
	const float At30 = ApparentRPM(30.0f);
	TestTrue(*FString::Printf(
		TEXT("30 fps sags below the cap - the accepted hitch fallback, not frame independence (%.0f RPM)"),
		At30), At30 < Cap - 1.0f);

	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A WHEEL SPINS DOWN ONCE AIRBORNE, RATHER THAN STOPPING DEAD (#107 item 8).
 *
 * CONFIRMED, 2026-09-12 review. The wheel step used to be gated on !bAirborne outright, so a
 * ~12,000 deg/s wheel at rotation held its exact angle from the very next frame on - a snap
 * FAgentMotion::GroundSpeed's own header explicitly says real wheels do not do. The fix
 * decays WheelStepDegrees's own persistent rate toward zero over WheelSpinDownSeconds
 * instead of gating the integration outright.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWheelSpinDownTest,
	"Airside.Present.WheelSpinsDownRatherThanStopping",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWheelSpinDownTest::RunTest(const FString& Parameters)
{
	const float Radius = 21.0f;          // UAirsideAgentAnim::MainWheelRadius's own default
	const float SpinDown = 2.0f;         // UAirsideAgentAnim::WheelSpinDownSeconds's own default
	const float Dt = 1.0f / 60.0f;

	// ON THE GROUND: v = wr, read straight off ground speed every frame.
	float Rate = 0.0f;
	const float GroundStep = UAirsideAgentAnim::WheelStepDegrees(3000.0f, Radius, false, Dt, SpinDown, Rate);
	const float ExpectedRate = FMath::RadiansToDegrees(3000.0f / Radius);
	TestTrue(*FString::Printf(TEXT("on the ground the rate is v/r (%.1f deg/s, want %.1f)"), Rate, ExpectedRate),
		FMath::IsNearlyEqual(Rate, ExpectedRate, 0.1f));
	TestTrue(TEXT("and it actually turns"), GroundStep > 0.0f);

	// ROTATION: the wheel is turning fast, at ExpectedRate, when the aircraft leaves the
	// ground. THE ASSERTION THE BUG WOULD FAIL: gated outright, this next call returned 0 and
	// froze the wheel in one frame.
	const float FirstAirborneStep = UAirsideAgentAnim::WheelStepDegrees(4000.0f, Radius, true, Dt, SpinDown, Rate);
	TestTrue(*FString::Printf(
		TEXT("the first airborne frame still turns the wheel, rather than stopping it dead (%.3f deg)"),
		FirstAirborneStep), FirstAirborneStep > 0.0f);
	TestTrue(TEXT("and it is closer to a stop than to the ground rate, one frame later"),
		Rate < ExpectedRate);

	// GROUND SPEED IS IGNORED WHILE AIRBORNE: a climbing aircraft's ground speed does not
	// fall (FAgentMotion::GroundSpeed's own header), and feeding a rising figure in here must
	// not make the wheel speed back up.
	const float RateBeforeBogusSpeed = Rate;
	UAirsideAgentAnim::WheelStepDegrees(1.0e6f, Radius, true, Dt, SpinDown, Rate);
	TestTrue(*FString::Printf(
		TEXT("a huge ground speed while airborne does not raise the rate (%.3f vs %.3f)"),
		Rate, RateBeforeBogusSpeed), Rate <= RateBeforeBogusSpeed);

	// SEVERAL TIME CONSTANTS LATER: RELATIVE TO WHERE IT STARTED, not an absolute figure -
	// this airframe's wheel rate is itself in the thousands of degrees a second, so "close to
	// a stop" means a small FRACTION of RateBeforeBogusSpeed, not a small absolute number.
	// e^-5 is ~0.7%; five time constants (10 s at SpinDown=2 s) should clear that easily.
	const float RateBeforeDecay = RateBeforeBogusSpeed;
	for (int32 Step = 0; Step < 600; ++Step)  // 10 s at 60 fps, five time constants
	{
		UAirsideAgentAnim::WheelStepDegrees(4000.0f, Radius, true, Dt, SpinDown, Rate);
	}
	TestTrue(*FString::Printf(
		TEXT("five time constants later the wheel has essentially stopped (%.3f deg/s, was %.3f)"),
		Rate, RateBeforeDecay), Rate < RateBeforeDecay * 0.05f);

	// SpinDownSeconds <= 0 IS A CONFIGURATION CHOICE TO STOP DEAD, not a divide-by-a-tiny-
	// number: the old behaviour is still reachable for whoever wants it.
	float DeadRate = ExpectedRate;
	UAirsideAgentAnim::WheelStepDegrees(4000.0f, Radius, true, Dt, 0.0f, DeadRate);
	TestEqual(TEXT("SpinDownSeconds <= 0 stops the wheel on the very next frame"), DeadRate, 0.0f);

	// A ZERO RADIUS IS STILL GUARDED, exactly as it always was.
	float GuardRate = 999.0f;
	TestEqual(TEXT("a zero radius never turns the wheel"),
		UAirsideAgentAnim::WheelStepDegrees(3000.0f, 0.0f, false, Dt, SpinDown, GuardRate), 0.0f);
	TestEqual(TEXT("and clears the rate too"), GuardRate, 0.0f);

	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A WHEEL ROLLS BACKWARDS WHEN THE GROUND SPEED IS NEGATIVE.
 *
 * REPORTED FROM PLAY, 2026-09-20: "in reverse the wheels animate as if the vehicle is still
 * going forward". The model half is Airside.Model.BackwardsPhasesReportNegativeGroundSpeed -
 * a reversing agent now reports a negative figure. This is the view half, and it is a
 * separate test because the two were separately capable of being wrong: the arithmetic here
 * (v = wr) carried the sign through on the ground all along, and the SPIN-DOWN did not.
 *
 * THE DECAY WAS THE TRAP. It read FMath::Max(Rate - Rate/tau * dt, 0), which for a negative
 * rate is not a decay at all - Max against zero returns zero on the first airborne frame, so
 * the very snap the spin-down exists to prevent came back for anything rolling backwards.
 * An aircraft does not reverse into the air, so nothing in play would have shown it; it would
 * have waited for the first vehicle that did.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWheelsRollBackwardsTest,
	"Airside.Present.WheelsRollBackwardsAtNegativeSpeed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWheelsRollBackwardsTest::RunTest(const FString& Parameters)
{
	const float Radius = 21.0f;
	const float SpinDown = 2.0f;
	const float Dt = 1.0f / 60.0f;

	// ON THE GROUND, BACKING UP: v = wr with the sign kept, so the step is negative and the
	// bone turns the other way. THE ASSERTION THE BUG WOULD FAIL is upstream of here - this
	// pins that the view does not quietly discard a sign the model took trouble to send.
	float Rate = 0.0f;
	const float Step = UAirsideAgentAnim::WheelStepDegrees(-100.0f, Radius, false, Dt, SpinDown, Rate);
	TestTrue(*FString::Printf(TEXT("a backing vehicle turns its wheels backwards (%.3f deg)"), Step),
		Step < 0.0f);
	TestTrue(*FString::Printf(TEXT("at v/r in the other direction (%.1f deg/s)"), Rate),
		FMath::IsNearlyEqual(Rate, FMath::RadiansToDegrees(-100.0f / Radius), 0.1f));

	// AND IT IS SYMMETRIC: the same speed forwards turns the same amount the other way. A fix
	// that clamped instead of signing would pass the test above and fail this one.
	float Forward = 0.0f;
	const float ForwardStep =
		UAirsideAgentAnim::WheelStepDegrees(100.0f, Radius, false, Dt, SpinDown, Forward);
	TestTrue(TEXT("and forwards at the same speed is the same step mirrored"),
		FMath::IsNearlyEqual(ForwardStep, -Step, KINDA_SMALL_NUMBER));

	// THE SPIN-DOWN, FROM A NEGATIVE RATE. It must ease toward zero in MAGNITUDE and keep its
	// sign on the way - not jump to zero, and not cross through and start rolling forwards.
	float Decaying = FMath::RadiansToDegrees(-100.0f / Radius);
	const float Started = Decaying;
	const float FirstAirborne =
		UAirsideAgentAnim::WheelStepDegrees(0.0f, Radius, true, Dt, SpinDown, Decaying);
	TestTrue(*FString::Printf(
		TEXT("the first airborne frame still turns it backwards rather than stopping dead (%.4f deg)"),
		FirstAirborne), FirstAirborne < 0.0f);
	TestTrue(*FString::Printf(TEXT("and eases toward zero (%.2f from %.2f)"), Decaying, Started),
		Decaying > Started && Decaying < 0.0f);

	for (int32 At = 0; At < 600; ++At)   // 10 s at 60 fps, five time constants
	{
		UAirsideAgentAnim::WheelStepDegrees(0.0f, Radius, true, Dt, SpinDown, Decaying);
		if (!TestTrue(TEXT("it never crosses zero and starts rolling the wrong way"),
			Decaying <= 0.0f))
		{
			return false;
		}
	}
	TestTrue(*FString::Printf(TEXT("and essentially stops (%.4f, was %.2f)"), Decaying, Started),
		FMath::Abs(Decaying) < FMath::Abs(Started) * 0.05f);
	return true;
}

#endif
