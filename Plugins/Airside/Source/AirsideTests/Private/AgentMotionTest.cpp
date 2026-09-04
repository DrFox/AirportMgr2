#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentMotionTest,
	"Airside.Present.AgentMotion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentMotionTest::RunTest(const FString& Parameters)
{
	// 1. THE MEASUREMENT. A STATIONARY AIRCRAFT IS NOT A STOPPED ENGINE.
	//
	//    "Is the engine running" was inferred from whether the aircraft had arrived, so the
	//    propeller stopped every time the aircraft did. An aircraft holding short with its
	//    engine idling is the commonest sight on an airport and it read as shut down.
	//
	//    The two are independent, which is the whole of this test: speed says nothing about
	//    the engine, and the engine says nothing about the speed.
	{
		FRoadAgent Agent;
		Agent.bEngineRunning = true;
		Agent.Follower.Speed = 0.0;

		const FAgentMotion Motion = Agent.DescribeMotion(FVector2D(100.0, 200.0), 1.0);

		TestTrue(TEXT("a stationary aircraft under power still has its engine running"),
			Motion.bEngineRunning);
		TestEqual(TEXT("while reporting no ground speed at all"), Motion.GroundSpeed, 0.0);
	}

	// 2. AND THE OTHER WAY ROUND, which the old code could not express: a shut-down aircraft
	//    reports its engine stopped no matter what else is true of it.
	{
		FRoadAgent Agent;
		Agent.bEngineRunning = false;
		Agent.Follower.Speed = 1000.0;

		const FAgentMotion Motion = Agent.DescribeMotion(FVector2D::ZeroVector, 0.0);

		TestFalse(TEXT("a shut-down aircraft has a stopped propeller"), Motion.bEngineRunning);
		TestEqual(TEXT("even while it is being pushed along"), Motion.GroundSpeed, 1000.0);
	}

	// 3. SPEED COMES FROM WHICHEVER PHASE IS DRIVING. The follower's speed is meaningless
	//    once a departure has taken over, and reading the wrong one would spin the wheels at
	//    taxi pace all the way down the runway.
	{
		FRoadAgent Agent;
		Agent.bEngineRunning = true;
		Agent.Follower.Speed = 800.0;
		Agent.Departure.Speed = 4400.0;

		TestEqual(TEXT("taxiing, the follower's speed is the one that counts"),
			Agent.DescribeMotion(FVector2D::ZeroVector, 0.0).GroundSpeed, 800.0);

		Agent.bDeparting = true;
		TestEqual(TEXT("departing, the take-off run's speed is"),
			Agent.DescribeMotion(FVector2D::ZeroVector, 0.0).GroundSpeed, 4400.0);
	}

	// 4. AIRBORNE MEANS THE WHEELS ARE UP, not that the altitude happens to be non-zero.
	//    During the rotation the aircraft is pitching with its mains still down, and gear
	//    retraction must not begin there.
	{
		FRoadAgent Agent;
		Agent.bEngineRunning = true;
		Agent.bDeparting = true;

		Agent.Departure.Phase = ETakeoffPhase::Roll;
		TestFalse(TEXT("rolling is not airborne"),
			Agent.DescribeMotion(FVector2D::ZeroVector, 0.0).bAirborne);

		Agent.Departure.Phase = ETakeoffPhase::Rotate;
		TestFalse(TEXT("nor is rotating, with the mains still down"),
			Agent.DescribeMotion(FVector2D::ZeroVector, 0.0).bAirborne);

		Agent.Departure.Phase = ETakeoffPhase::Climb;
		TestTrue(TEXT("climbing is"),
			Agent.DescribeMotion(FVector2D::ZeroVector, 0.0).bAirborne);
	}

	// 5. A TAXIING AIRCRAFT IS NEVER AIRBORNE, whatever the departure struct happens to hold
	//    - it is armed at dispatch and only takes over on arrival.
	{
		FRoadAgent Agent;
		Agent.bEngineRunning = true;
		Agent.bDeparting = false;
		Agent.Departure.Phase = ETakeoffPhase::Climb;

		TestFalse(TEXT("an armed but inactive departure leaves the aircraft on the ground"),
			Agent.DescribeMotion(FVector2D::ZeroVector, 0.0).bAirborne);
	}

	return true;
}

#endif
