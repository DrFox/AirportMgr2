#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadAgent.h"
#include "Model/TrafficClaims.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClaimCentreTest,
	"Airside.Model.ClaimCentre",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FClaimCentreTest::RunTest(const FString& Parameters)
{
	// The claim window is T +/- F/2 and T is documented as the agent's CENTRE. With the
	// origin on the nose gear, Travelled is the NOSE GEAR's distance, so taking it for the
	// centre puts the whole window half a length too far forward - the agent reserves line
	// it is not standing on and leaves its own tail unclaimed, which is a collision nobody
	// would be able to explain from the logs.
	FRoadAgent Agent;
	Agent.Airframe.Ground = TestAirframes::Piper().Ground;
	Agent.Airframe.SteerAxleX = 0.0;
	Agent.Airframe.FixedAxleX = -454.3;
	Agent.Airframe.BodyCentreX = -629.3;   // (167.5 + -1426.1) / 2, plane2 as measured
	Agent.Follower.Travelled = 10000.0;

	TestEqual(TEXT("the centre is the body centre, not the steered axle"),
		FClaimPass::CentreOf(Agent), 10000.0 - 629.3, 0.01);

	// A DEVIATING ORIGIN is measured from the steered axle too, so both offsets appear: the
	// Piper's Travelled is its nose gear (SteerAxleX ahead of an origin at the main gear),
	// and its centre is behind that by the wheelbase and the body offset together.
	FRoadAgent Piper;
	Piper.Airframe.Ground = TestAirframes::Piper().Ground;
	Piper.Airframe.SteerAxleX = 260.0;
	Piper.Airframe.FixedAxleX = 0.0;
	Piper.Airframe.BodyCentreX = -73.2;    // (385.1 + -531.5) / 2
	Piper.Follower.Travelled = 10000.0;

	TestEqual(TEXT("a deviating origin subtracts both offsets"),
		FClaimPass::CentreOf(Piper), 10000.0 - 260.0 - 73.2, 0.01);

	// AN UNMEASURED AGENT KEEPS TODAY'S ANSWER EXACTLY - every vehicle takes this path, and
	// a claim window that moved under them would be a traffic model quietly re-tuned.
	FRoadAgent Unmeasured;
	Unmeasured.Airframe.Ground = TestAirframes::Piper().Ground;
	Unmeasured.Follower.Travelled = 10000.0;
	TestEqual(TEXT("an unmeasured agent's centre is still Travelled"),
		FClaimPass::CentreOf(Unmeasured), 10000.0, 0.0001);

	return true;
}

#endif
