#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"
#include "Model/RouteFollower.h"
#include "Model/SpeedProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSteeringFloorZeroStillTaxisTest,
	"Airside.Model.SteeringFloorZeroStillTaxis",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSteeringFloorZeroStillTaxisTest::RunTest(const FString& Parameters)
{
	// THE LANDMINE THE WHOLE SPLIT WALKS PAST. FGroundPerformance::IsSet() used to require
	// MinSteeringSpeed > 0, back when that number meant only "an aircraft cannot yaw without
	// rolling". A ground vehicle has no such limit - a truck really can stop mid-turn,
	// because nothing is redirecting thrust along a body axis - so the van is authored at
	// zero. Under the old clause that made the van answer IsSet() == false and FREEZE:
	// ArrivalPlanner and FRoadAgent both branch on exactly that call.
	//
	// Nothing else in the suite would have caught it. The van would simply never have moved,
	// which reads as a routing bug and would have been chased as one - which is the same
	// wrong-subsystem hunt that cost this project three sessions in September 2026.
	FAirframe Van = UAirsideSettings::ResolveDefaultVehicle();
	Van.Chassis.Ground.MinSteeringSpeed = 0.0;

	TestTrue(
		TEXT("a vehicle that can stop mid-turn still has usable ground performance"),
		Van.Chassis.Ground.IsSet());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSteeringFloorSharpVertexStillCreepsTest,
	"Airside.Model.SteeringFloorSharpVertexStillCreeps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSteeringFloorSharpVertexStillCreepsTest::RunTest(const FString& Parameters)
{
	// A PLAN MUST NEVER ASK AN AGENT TO STOP WHERE IT STILL HAS TO STEER. A vertex whose
	// heading changes instantly cannot be taken at any speed, and FSpeedProfile has always
	// answered that with "the slowest this thing can still steer at". For an aircraft that is
	// a real figure. For a ground vehicle it is now ZERO - and zero here is not a crawl, it
	// is a PERMANENT STOP: the backward pass brakes the agent to rest at the vertex, LimitAt
	// returns zero from then on, and the truck sits on the apron for ever.
	//
	// This is the second deadlock of the pair, and the worse one. The first - the crab loop
	// at FRouteFollower - at least needs a heading error to trigger. This one needs only a
	// corner.
	//
	// So both floors take the GREATER of the physical minimum and the solver's progress
	// epsilon. The aircraft keeps its physics; the van creeps.
	FAirframe Van = UAirsideSettings::ResolveDefaultVehicle();
	Van.Chassis.Ground.MinSteeringSpeed = 0.0;

	// A right-angle vertex at 1000 uu: due east, then due north. Nothing samples the corner,
	// so the heading changes instantly and FSpeedProfile calls it untakeable - which is
	// exactly the case under test, not an accident of the fixture.
	const TArray<FVector2D> Corner = {
		FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0), FVector2D(1000.0, 1000.0) };

	FSpeedProfile Profile;
	Profile.Build(Corner, Van.Chassis);

	TestTrue(
		FString::Printf(
			TEXT("a van planned through a sharp vertex is still asked to move (%.1f uu/s), ")
			TEXT("not to stop dead"),
			Profile.LimitAt(1000.0)),
		Profile.LimitAt(1000.0) >= FRouteFollower::ProgressEpsilon - KINDA_SMALL_NUMBER);

	// AND AN AIRCRAFT IS UNAFFECTED, which is what says the epsilon was added beside the
	// physical floor rather than in place of it. A Piper's own steering minimum is far above
	// the epsilon, so the epsilon must not be what governs it.
	const FAirframe Piper = UAirsideSettings::ResolveDefaultAirframe();
	if (Piper.Chassis.Ground.MinSteeringSpeed > FRouteFollower::ProgressEpsilon)
	{
		FSpeedProfile Aircraft;
		Aircraft.Build(Corner, Piper.Chassis);

		TestTrue(
			FString::Printf(
				TEXT("an aircraft still crawls its sharp vertex at its own steering minimum ")
				TEXT("%.0f, not at the solver epsilon"),
				Piper.Chassis.Ground.MinSteeringSpeed),
			Aircraft.LimitAt(1000.0) >= Piper.Chassis.Ground.MinSteeringSpeed - KINDA_SMALL_NUMBER);
	}

	return true;
}

#endif
