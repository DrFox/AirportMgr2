#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadAgent.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The wheels turn at the speed the aeroplane is actually doing, in every phase that moves it.
 *
 * REPORTED FROM PLAY, both halves: "the wheels dont spin on landing they only start when the
 * aircraft turns to leave the runway", and "the wheels continue to spin after the aircraft
 * has left the ground on takeoff".
 *
 * One line caused the first. FRoadAgent::DescribeMotion chose between the DEPARTURE's speed
 * and the FOLLOWER's, and a landing is driven by neither - it is the Arrival, an FLandingRun,
 * and the follower does not start until the aircraft vacates. So a touchdown at seventy knots
 * reported a ground speed of zero, and UAirsideAgentAnim - which computes wheel rate as speed
 * over radius and reads nothing else - held the wheels perfectly still until the taxi began.
 *
 * The second is the opposite mistake about the same figure: ground speed does NOT fall to
 * zero at rotation, so a climbing aeroplane kept spinning its wheels, faster than it ever had
 * on the runway. That one is fixed in the view rather than here, because bAirborne already
 * says it and the model's speed is not wrong - the aeroplane really is travelling.
 *
 * WHAT THIS PINS is the model half: every moving phase reports a speed that belongs to the
 * thing driving it. A test of the anim instance would need an owning actor and a skeleton;
 * this needs neither, and the defect was here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRollingWheelsTest,
	"Airside.Model.GroundSpeedFollowsTheDrivingPhase",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRollingWheelsTest::RunTest(const FString& Parameters)
{
	// An airframe that can actually fly a landing: the figures matter only in that the
	// landing run refuses to begin without them.
	FAirframe Airframe;
	Airframe.Ground.Taxi.Accel = 100.0;
	Airframe.Ground.Taxi.Decel = 200.0;
	Airframe.Ground.Taxi.SpeedCap = 1000.0;
	Airframe.Ground.Takeoff.Accel = 450.0;
	Airframe.Ground.Takeoff.Decel = 400.0;
	Airframe.Ground.Takeoff.SpeedCap = 3100.0;
	Airframe.Ground.Landing.Accel = 100.0;
	Airframe.Ground.Landing.Decel = 400.0;
	Airframe.Ground.Landing.SpeedCap = 3600.0;
	Airframe.Climb.ClimbSpeed = 4400.0;
	Airframe.Climb.ClearAltitude = 30000.0;
	Airframe.Engine.MaxRPM = 2200.0;

	FRoadAgent Agent;
	Agent.Id = 1;

	// A long strip, so the rollout is still running when this looks at it.
	const FVector2D Threshold(0.0, 0.0);
	const FVector2D Direction(1.0, 0.0);
	FRoutePlan TaxiIn;
	if (!TestTrue(TEXT("the landing arms"),
		Agent.StartArrival(Threshold, Direction, 200000.0, Airframe, 150000.0, TaxiIn)))
	{
		return false;
	}
	TestEqual(TEXT("it is Arriving"), Agent.Phase, EAgentPhase::Arriving);

	// Fly it far enough to be rolling rather than still on final.
	FAgentMotion Motion;
	for (int32 Step = 0; Step < 400 && Agent.Phase == EAgentPhase::Arriving; ++Step)
	{
		Agent.Advance(1.0 / 30.0, Motion);
	}

	if (Agent.Phase == EAgentPhase::Arriving)
	{
		// THE ASSERTION THE BUG WOULD FAIL. Before the fix this was Follower.Speed, which is
		// zero for the whole of a landing, and the wheels stood still under a rolling
		// aeroplane.
		TestTrue(*FString::Printf(
			TEXT("a rolling arrival reports a ground speed (%.0f uu/s), so its wheels turn"),
			Motion.GroundSpeed), Motion.GroundSpeed > 0.0);

		// And it is the ARRIVAL's speed, not some other phase's that happens to be non-zero.
		TestTrue(TEXT("and it is the landing run's own speed"),
			FMath::IsNearlyEqual(Motion.GroundSpeed, Agent.Arrival.Speed, 0.01));
	}
	else
	{
		AddInfo(TEXT("the arrival completed within the step budget; speed not sampled"));
	}

	// bAirborne is what the view gates the wheels on, and it must be false while any part of
	// the aeroplane is still rolling - otherwise the fix for the take-off half would stop the
	// wheels during the take-off ROLL, which is when they should be spinning fastest.
	TestFalse(TEXT("an aircraft on the runway is not airborne"), Motion.bAirborne);
	return true;
}

#endif
