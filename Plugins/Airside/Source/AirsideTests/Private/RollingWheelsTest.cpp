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
	FRunwayEnd End;
	End.Threshold = FVector2D(0.0, 0.0);
	End.Direction = FVector2D(1.0, 0.0);
	End.Length = 200000.0;
	FRoutePlan TaxiIn;
	if (!TestTrue(TEXT("the landing arms"),
		Agent.StartArrival(End, Airframe, 150000.0, TaxiIn)))
	{
		return false;
	}
	TestEqual(TEXT("it is Arriving"), Agent.Phase, EAgentPhase::Arriving);

	// ONE STEP INTO THE APPROACH, still miles from the threshold: DescribeMotion used to set
	// bAirborne only for a departure in the climb, so an arrival on final (or in the flare)
	// reported bAirborne=false - approach speed with the wheels shown down and turning, which
	// contradicts InspectFacts.cpp's "On final" status. Sampled here, before any rollout, so
	// this cannot be satisfied by the Rollout/Vacated branch of FLandingRun::IsOnGround.
	FAgentMotion ApproachMotion;
	EAgentEvent Event = EAgentEvent::None;
	Agent.Advance(1.0 / 30.0, ApproachMotion, Event);
	TestEqual(TEXT("still on the approach, not yet on the ground"),
		Agent.Arrival.Phase, ELandingPhase::Approach);
	TestTrue(TEXT("an aircraft on final is airborne"), ApproachMotion.bAirborne);

	// Fly it far enough to be ROLLING - Arrival.Phase == Rollout - rather than still on final.
	// 400 steps (13.3 s) was the OLD budget, and it is not enough: FinalAltitude/Glideslope
	// alone is ~5.8 s of approach before the flare even starts, and the old final assertion
	// below passed regardless, because bAirborne was hard-wired false for every Arriving
	// agent - the exact bug this test exists to catch. 6000 steps (200 s) comfortably covers
	// the approach and flare for any airframe these figures describe; touchdown is the thing
	// under test, not how long it takes to reach it.
	FAgentMotion Motion;
	for (int32 Step = 0; Step < 6000 && Agent.Arrival.Phase != ELandingPhase::Rollout
		&& Agent.Phase == EAgentPhase::Arriving; ++Step)
	{
		Agent.Advance(1.0 / 30.0, Motion, Event);
	}

	if (!TestEqual(TEXT("touched down within the step budget"),
		Agent.Arrival.Phase, ELandingPhase::Rollout))
	{
		return false;
	}

	// THE ASSERTION THE BUG WOULD FAIL. Before the fix this was Follower.Speed, which is
	// zero for the whole of a landing, and the wheels stood still under a rolling aeroplane.
	TestTrue(*FString::Printf(
		TEXT("a rolling arrival reports a ground speed (%.0f uu/s), so its wheels turn"),
		Motion.GroundSpeed), Motion.GroundSpeed > 0.0);

	// And it is the ARRIVAL's speed, not some other phase's that happens to be non-zero.
	TestTrue(TEXT("and it is the landing run's own speed"),
		FMath::IsNearlyEqual(Motion.GroundSpeed, Agent.Arrival.Speed, 0.01));

	// bAirborne is what the view gates the wheels on, and it must be false while any part of
	// the aeroplane is still rolling - otherwise the fix for the take-off half would stop the
	// wheels during the take-off ROLL, which is when they should be spinning fastest.
	TestFalse(TEXT("an aircraft on the runway is not airborne"), Motion.bAirborne);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * A PHASE THAT MOVES BACKWARDS REPORTS A NEGATIVE GROUND SPEED.
 *
 * REPORTED FROM PLAY, 2026-09-20: "in reverse the wheels animate as if the vehicle is still
 * going forward". The fuel truck is where it shows, because it reverses in plain view on
 * every service cycle, but a pushback has the same defect and always did - DescribeMotion's
 * own comment on the Manoeuvring case admitted it in writing: "the wheels turn under it -
 * backwards, but the view has no signed wheel rate and a tyre rolling the other way at
 * 1.5 m/s reads the same".
 *
 * IT IS THE VIEW'S ONLY SOURCE OF DIRECTION. UAirsideAgentAnim::WheelStepDegrees is
 * RadiansToDegrees(GroundSpeed / Radius) and reads nothing else, so an unsigned speed can
 * only ever spin a wheel forwards. Making the figure signed is what the sibling test in
 * Airside.Present.WheelsRollBackwardsAtNegativeSpeed then relies on.
 *
 * THE SIGN LIVES HERE, in the one place that knows the phase, rather than in FReverseRun and
 * FPushbackRun separately. Both of those keep a magnitude, as FRouteFollower does, so there
 * is one answer to "which way is this going" and not three that must agree.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBackwardsPhasesReportNegativeSpeedTest,
	"Airside.Model.BackwardsPhasesReportNegativeGroundSpeed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBackwardsPhasesReportNegativeSpeedTest::RunTest(const FString& Parameters)
{
	// DescribeMotion is a pure function of the agent's own state, so the phase and the one
	// field that phase reads are the whole fixture - no route, no world, no plan.
	FRoadAgent Agent;
	Agent.Id = 1;

	Agent.Phase = EAgentPhase::Reversing;
	Agent.Reverse.Speed = 100.0;
	const FAgentMotion Backing = Agent.DescribeMotion(FVector2D::ZeroVector, 0.0);
	TestEqual(TEXT("a reversing vehicle reports its speed as negative, so its wheels roll back"),
		Backing.GroundSpeed, -100.0, UE_DOUBLE_KINDA_SMALL_NUMBER);

	Agent.Phase = EAgentPhase::Manoeuvring;
	Agent.Pushback.Speed = 150.0;
	const FAgentMotion Pushed = Agent.DescribeMotion(FVector2D::ZeroVector, 0.0);
	TestEqual(TEXT("and so does an aircraft being pushed back off a stand"),
		Pushed.GroundSpeed, -150.0, UE_DOUBLE_KINDA_SMALL_NUMBER);

	// FORWARD IS UNTOUCHED, which is the half that stops this being a sign flip rather than a
	// fix: every other phase still reports a positive figure and the inspector still reads it.
	Agent.Phase = EAgentPhase::Taxiing;
	Agent.Follower.Speed = 400.0;
	TestEqual(TEXT("a taxiing vehicle is still positive"),
		Agent.DescribeMotion(FVector2D::ZeroVector, 0.0).GroundSpeed, 400.0,
		UE_DOUBLE_KINDA_SMALL_NUMBER);
	return true;
}

// ---------------------------------------------------------------------------------------
/**
 * THE STEERING COMES FROM THE PHASE THAT IS DRIVING, LIKE THE SPEED ABOVE.
 *
 * REPORTED FROM PLAY, 2026-09-20: the fuel truck "straightened its wheels while still turning
 * and then slid around the last part of the reverse".
 *
 * DescribeMotion took SteerAngleDegrees from FRouteFollower "WHATEVER THE PHASE", and said so
 * in its own comment - the argument being that a landing rollout and a take-off roll steer on
 * the rudder, so the follower's zero is the right answer there rather than a missing one. It
 * is the right answer for those two. It is not an answer at all for Reversing, where the
 * follower is not running: SteerDegrees simply holds whatever it last computed before the
 * manoeuvre armed, which for a truck that has just parked its steered axle on a service point
 * is near enough straight.
 *
 * THIS IS THE THIRD FIELD IN ONE FUNCTION WITH THE SAME DEFECT. GroundSpeed was missing the
 * Arriving phase once (Airside.Model.GroundSpeedFollowsTheDrivingPhase) and reporting a cap
 * rather than a state for Reversing (Airside.Model.ReverseSpeedIsWhatItAchieved). The shape is
 * always "the struct that is actually moving the agent is not the struct being read", so the
 * test is always at this seam and never inside one of the run structs.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSteeringFollowsTheDrivingPhaseTest,
	"Airside.Model.SteeringFollowsTheDrivingPhase",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSteeringFollowsTheDrivingPhaseTest::RunTest(const FString& Parameters)
{
	FRoadAgent Agent;
	Agent.Id = 1;

	// A VALUE THE FOLLOWER COULD NOT HAVE MEANT, so a pass cannot come from the two agreeing
	// by accident: 33 degrees is the stale taxi steering, -12 is what the reverse is asking
	// for right now.
	Agent.Follower.SteerDegrees = 33.0;
	Agent.Reverse.SteerDegrees = -12.0;

	Agent.Phase = EAgentPhase::Reversing;
	TestEqual(TEXT("a reversing vehicle steers by its reverse manoeuvre, not the parked taxi"),
		Agent.DescribeMotion(FVector2D::ZeroVector, 0.0).SteerAngleDegrees, -12.0,
		UE_DOUBLE_KINDA_SMALL_NUMBER);

	// AND EVERY OTHER PHASE IS UNCHANGED, which is the half that keeps this from being a
	// rewrite: the follower's answer is still the right one for a taxi, a rollout and a roll.
	Agent.Phase = EAgentPhase::Taxiing;
	TestEqual(TEXT("a taxiing vehicle still steers by its follower"),
		Agent.DescribeMotion(FVector2D::ZeroVector, 0.0).SteerAngleDegrees, 33.0,
		UE_DOUBLE_KINDA_SMALL_NUMBER);

	Agent.Phase = EAgentPhase::Arriving;
	TestEqual(TEXT("and so does a landing rollout, which steers on the rudder"),
		Agent.DescribeMotion(FVector2D::ZeroVector, 0.0).SteerAngleDegrees, 33.0,
		UE_DOUBLE_KINDA_SMALL_NUMBER);
	return true;
}

#endif
