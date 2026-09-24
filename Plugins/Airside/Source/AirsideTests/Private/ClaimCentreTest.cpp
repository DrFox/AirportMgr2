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
	Agent.EditAirframeForTest().Chassis.Ground = TestAirframes::Piper().Chassis.Ground;
	Agent.EditAirframeForTest().Chassis.SteerAxleX = 0.0;
	Agent.EditAirframeForTest().Chassis.FixedAxleX = -454.3;
	Agent.EditAirframeForTest().Chassis.BodyCentreX = -629.3;   // (167.5 + -1426.1) / 2, plane2 as measured
	Agent.Follower.Travelled = 10000.0;

	TestEqual(TEXT("the centre is the body centre, not the steered axle"),
		FClaimPass::CentreOf(Agent), 10000.0 - 629.3, 0.01);

	// A DEVIATING ORIGIN is measured from the steered axle too, so both offsets appear: the
	// Piper's Travelled is its nose gear (SteerAxleX ahead of an origin at the main gear),
	// and its centre is behind that by the wheelbase and the body offset together.
	FRoadAgent Piper;
	Piper.EditAirframeForTest().Chassis.Ground = TestAirframes::Piper().Chassis.Ground;
	Piper.EditAirframeForTest().Chassis.SteerAxleX = 260.0;
	Piper.EditAirframeForTest().Chassis.FixedAxleX = 0.0;
	Piper.EditAirframeForTest().Chassis.BodyCentreX = -73.2;    // (385.1 + -531.5) / 2
	Piper.Follower.Travelled = 10000.0;

	TestEqual(TEXT("a deviating origin subtracts both offsets"),
		FClaimPass::CentreOf(Piper), 10000.0 - 260.0 - 73.2, 0.01);

	// AN UNMEASURED AGENT KEEPS TODAY'S ANSWER EXACTLY - every vehicle takes this path, and
	// a claim window that moved under them would be a traffic model quietly re-tuned.
	FRoadAgent Unmeasured;
	Unmeasured.EditAirframeForTest().Chassis.Ground = TestAirframes::Piper().Chassis.Ground;
	Unmeasured.Follower.Travelled = 10000.0;
	TestEqual(TEXT("an unmeasured agent's centre is still Travelled"),
		FClaimPass::CentreOf(Unmeasured), 10000.0, 0.0001);

	// A PUSHED BODY LEADS ITS NOSE GEAR, and the offset must reverse with it.
	//
	// BodyCentreX - SteerAxleX is NEGATIVE on a conforming airframe - the plan centre sits
	// AFT of the nose gear - so the form above puts the centre BEHIND the steered axle in
	// plan distance. That is right for a taxi, where the nose gear leads. During a push the
	// aeroplane is pulled out along a lead-in that runs away from the terminal it faces, so
	// the MAINS lead and the body is AHEAD of the nose gear in plan distance.
	//
	// Getting it wrong misplaces the claimed body by TWICE the offset - 12.6 m on plane2,
	// whose re-export about its nose gear is the reason CentreOf exists at all - and shows as
	// an aeroplane still holding the stand it has left while pushing into ground it has not
	// claimed. Neither is visible in any log, which is why it is a test.
	FRoadAgent Pushing;
	Pushing.EditAirframeForTest().Chassis.Ground = TestAirframes::Piper().Chassis.Ground;
	Pushing.EditAirframeForTest().Chassis.SteerAxleX = 0.0;
	Pushing.EditAirframeForTest().Chassis.FixedAxleX = -454.3;
	Pushing.EditAirframeForTest().Chassis.BodyCentreX = -629.3;   // plane2 as measured, as above
	Pushing.Phase = EAgentPhase::Manoeuvring;
	Pushing.Pushback.Travelled = 10000.0;

	TestEqual(TEXT("a pushed body's centre LEADS its nose gear"),
		FClaimPass::CentreOf(Pushing), 10000.0 + 629.3, 0.01);

	// AND THE SAME AGENT READ AS A TAXI IS UNCHANGED. The assertions at the top already pin
	// the taxi answer, but reading ONE agent both ways is what makes this a contrast rather
	// than two unrelated facts - and it is what fails if CentreOf stops asking the phase.
	Pushing.Phase = EAgentPhase::Taxiing;
	Pushing.Follower.Travelled = 10000.0;
	TestEqual(TEXT("a taxiing body's centre still trails it"),
		FClaimPass::CentreOf(Pushing), 10000.0 - 629.3, 0.01);

	// AND THE DISTANCE COMES FROM WHICHEVER STRUCT IS DRIVING. A manoeuvring agent whose
	// Follower still holds the taxi that brought it in must read the PUSH's distance: taking
	// the stale one would claim ground the aeroplane is nowhere near, and the two are
	// deliberately different numbers here so that mistake cannot pass.
	FRoadAgent Stale;
	Stale.EditAirframeForTest().Chassis.Ground = TestAirframes::Piper().Chassis.Ground;
	Stale.Phase = EAgentPhase::Manoeuvring;
	Stale.Follower.Travelled = 99000.0;   // where the taxi IN ended
	Stale.Pushback.Travelled = 1500.0;    // where the push has got to
	TestEqual(TEXT("a push reads its own distance, not the taxi that brought it in"),
		FClaimPass::CentreOf(Stale), 1500.0, 0.01);

	return true;
}

#endif
