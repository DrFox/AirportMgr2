#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadEntity.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirframeAxlesTest,
	"Airside.Model.AirframeAxles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirframeAxlesTest::RunTest(const FString& Parameters)
{
	// 1. ZERO IS "NOT MEASURED", and it must mean the old law rather than a wheelbase of
	//    nothing - every vehicle in the game relies on that, so it is asserted first.
	const FAirframe Unmeasured;
	TestFalse(TEXT("an airframe nobody measured does not claim axles"), Unmeasured.HasAxles());
	TestEqual(TEXT("and its wheelbase is zero rather than a negative"),
		Unmeasured.Wheelbase(), 0.0);

	// 2. A conforming airframe: origin ON the steered axle, mains aft, so the wheelbase is
	//    the distance between them and is POSITIVE whichever way the figures are signed.
	FAirframe Conforming;
	Conforming.SteerAxleX = 0.0;
	Conforming.FixedAxleX = -454.3;
	TestTrue(TEXT("axle figures turn the geometric law on"), Conforming.HasAxles());
	TestEqual(TEXT("wheelbase is nose gear to main gear"), Conforming.Wheelbase(), 454.3, 0.01);

	// 3. The Piper's documented deviation - origin at the MAIN gear - measures the same
	//    wheelbase. If this ever fails, the two laws disagree about the same aeroplane.
	FAirframe Deviating;
	Deviating.SteerAxleX = 454.3;
	Deviating.FixedAxleX = 0.0;
	TestEqual(TEXT("a main-gear origin measures the same wheelbase"),
		Deviating.Wheelbase(), 454.3, 0.01);

	// 4. The figures survive the trip through an authored type, which is the only path the
	//    game uses - a field added to FAirframe and not copied in Airframe() is a figure
	//    that is authored and then silently dropped.
	UAircraftType* Type = NewObject<UAircraftType>();
	UAircraftType::BuildPiperMeridian(Type);
	Type->Footprint.NoseX = 385.1;
	Type->Footprint.TailX = -531.5;
	Type->SteerAxleX = 260.0;
	Type->FixedAxleX = 0.0;
	Type->Ground.MaxSteerDegrees = 55.0;
	Type->Ground.MaxLateralAccelUu = 200.0;

	const FAirframe Built = Type->Airframe();
	TestEqual(TEXT("SteerAxleX reaches the airframe"), Built.SteerAxleX, 260.0, 0.01);
	TestEqual(TEXT("FixedAxleX reaches the airframe"), Built.FixedAxleX, 0.0, 0.01);
	TestEqual(TEXT("the steering lock reaches the airframe"),
		Built.Ground.MaxSteerDegrees, 55.0, 0.01);
	TestEqual(TEXT("the lateral accel limit reaches the airframe"),
		Built.Ground.MaxLateralAccelUu, 200.0, 0.01);

	// BodyCentreX is DERIVED in Airframe() rather than authored, so it cannot disagree with
	// the footprint it comes from. Claims read it - see FClaimPass::CentreOf.
	TestEqual(TEXT("the body centre is derived from the footprint"),
		Built.BodyCentreX, (385.1 + -531.5) * 0.5, 0.01);

	return true;
}

#endif
