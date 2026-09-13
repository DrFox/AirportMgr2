#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RouteFollower.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A LONG FRAME MUST NOT CARRY AN AGENT THROUGH ITS STOP POINT.
 *
 * FRouteFollower::Advance clamps Travelled to StopAt and caps Speed by the braking curve
 * to it, and its comment says that is deliberately for "a long frame - a hitch, or a
 * breakpoint". Nothing enforced it: the contract lived only in the comment, and deleting
 * the clamp would have broken holding short of a live runway with every test still green.
 *
 * It matters more the faster the game can be run. ARoadNetworkActor::SetSimTimeScale
 * multiplies every frame's delta before it reaches this code, and nothing clamps the delta
 * on the way in: UGameEngine::MaxDeltaTime defaults to 0, which its own comment documents
 * as unbound, and it is not applied in the editor at all. At a 32x scale a half-second
 * hitch therefore hands the model a SIXTEEN SECOND step. The deltas below are that case,
 * not a hypothetical.
 *
 * Deliberately asserts POSITION rather than merely that a stop happened: an agent that
 * halts a metre past the holding point has already crossed the bar the arbiter drew, and
 * "it stopped" would pass for that.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFollowerLongFrameTest,
	"Airside.Model.Follower.LongFrameNeverPassesTheStop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFollowerLongFrameTest::RunTest(const FString& Parameters)
{
	// 0.016 is a healthy frame; 0.53 is one frame of a 60 Hz game at x32; 16.0 is a half
	// second hitch at x32; 60.0 is an alt-tab or a shader compile stall at speed.
	const double Deltas[] = { 0.016, 0.53, 2.0, 16.0, 60.0 };

	for (const double Delta : Deltas)
	{
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = { FVector2D(0.0, 0.0), FVector2D(200000.0, 0.0) };
		Plan.Length = 200000.0;

		FAirframe Airframe;
		FRouteFollower Follower;
		Follower.Start(Plan, Airframe);

		FVector2D At;
		double Heading = 0.0;

		// Fixed SIM DURATION, not a fixed tick count: 64 ticks is a second at 0.016 and an
		// hour at 60, and a test whose coverage depends on the delta it is varying measures
		// nothing. Sixty seconds is comfortably past the five this airframe needs to brake
		// from cruise at its Decel of 200.
		const auto TicksFor = [Delta](double Seconds)
		{
			return FMath::Max(1, FMath::CeilToInt32(Seconds / Delta));
		};

		// Up to cruise with nothing ahead, using this frame length throughout so the agent
		// is genuinely moving at the speed a long frame would have given it.
		for (int32 Tick = 0; Tick < TicksFor(60.0); ++Tick)
		{
			Follower.Advance(Delta, Airframe, TNumericLimits<double>::Max(), At, Heading);
		}
		if (!TestTrue(*FString::Printf(TEXT("delta %.3f s: at cruise before the bar is set"), Delta),
			Follower.Speed > 990.0))
		{
			return false;
		}

		// A bar 500 uu ahead - five metres, tighter than one step at the larger deltas, so
		// the clamp is the only thing that can keep the agent behind it.
		const double StopAt = Follower.Travelled + 500.0;
		for (int32 Tick = 0; Tick < TicksFor(60.0); ++Tick)
		{
			const double StopWithin = FMath::Max(0.0, StopAt - Follower.Travelled);
			Follower.Advance(Delta, Airframe, StopWithin, At, Heading);

			TestTrue(*FString::Printf(
				TEXT("delta %.3f s: never passes the stop point (at %.3f, bar at %.3f)"),
				Delta, Follower.Travelled, StopAt),
				Follower.Travelled <= StopAt + 1e-6);
		}

		TestTrue(*FString::Printf(TEXT("delta %.3f s: comes to rest at the bar"), Delta),
			Follower.Speed < 1e-6);

		// The position on the line must be real too. A clamped Travelled with a stale
		// OutPosition would look correct in the number and wrong on screen.
		TestTrue(*FString::Printf(
			TEXT("delta %.3f s: reported position is at the bar, not past it (x %.1f)"),
			Delta, At.X),
			At.X <= StopAt + 1e-6);
	}
	return true;
}

#endif
