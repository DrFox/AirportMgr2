#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"

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
	Van.Ground.MinSteeringSpeed = 0.0;

	TestTrue(
		TEXT("a vehicle that can stop mid-turn still has usable ground performance"),
		Van.Ground.IsSet());

	return true;
}

#endif
