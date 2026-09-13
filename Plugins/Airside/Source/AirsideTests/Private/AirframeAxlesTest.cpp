#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Engine/SkeletalMesh.h"
#include "Model/RoadEntity.h"
#include "UObject/UObjectGlobals.h"

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

	// 5. THE PIPER IS MEASURED and keeps its main-gear origin: SteerAxleX is its wheelbase,
	//    FixedAxleX is zero. Measured off SK_PiperMeridian's reference pose - wheel_f at
	//    x = 237.8, wheel_rl and wheel_rr at x = 0 - rather than from the "about 2.6 m" the
	//    type's comment used to estimate, which was 9 per cent out.
	UAircraftType* Meridian = NewObject<UAircraftType>();
	UAircraftType::BuildPiperMeridian(Meridian);
	const FAirframe Piper = Meridian->Airframe();
	TestTrue(TEXT("the Piper steers geometrically"), Piper.HasAxles());
	TestEqual(TEXT("its wheelbase is the measured 2.378 m"), Piper.Wheelbase(), 237.8, 0.1);
	TestEqual(TEXT("and its fixed axle is the origin, which is its declared deviation"),
		Piper.FixedAxleX, 0.0, 0.01);
	TestTrue(TEXT("it has a steering lock to turn on"), Piper.Ground.MaxSteerDegrees > 0.0);

	// 6. THE AIRLINERS STAY ON THE PIVOT LAW, deliberately: no mesh to measure against, and
	//    a published wheelbase would be a figure nobody could check on screen. They are kept
	//    rather than deleted because EntityDefinition builds the A320 in production and the
	//    stand-geometry tests size against both - they are the only large footprints here.
	UAircraftType* Airbus = NewObject<UAircraftType>();
	UAircraftType::BuildA320(Airbus);
	TestFalse(TEXT("the A320 is not measured, so it keeps the flat rate"),
		Airbus->Airframe().HasAxles());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFootprintMatchesTheMeshTest,
	"Airside.Content.FootprintMatchesTheMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFootprintMatchesTheMeshTest::RunTest(const FString& Parameters)
{
	// THE HALF-RUN PIPELINE, pinned. build_plane2_type.py measures the export and writes the
	// DA; re-import the mesh without re-running it and the figures describe an aeroplane
	// that no longer exists. Nothing on screen shows that - the model looks right and its
	// clearance envelope is somewhere else - so it has to be a test.
	UAircraftType* Type = Cast<UAircraftType>(StaticLoadObject(
		UAircraftType::StaticClass(), nullptr, TEXT("/Game/Entities/DA_Aircraft_Plane2")));
	if (!TestNotNull(TEXT("DA_Aircraft_Plane2 loads"), Type))
	{
		return false;
	}

	USkeletalMesh* Mesh = Type->Mesh.LoadSynchronous();
	if (!TestNotNull(TEXT("and the mesh it names loads"), Mesh))
	{
		return false;
	}

	const FBoxSphereBounds Bounds = Mesh->GetBounds();
	const double MeshNoseX = Bounds.Origin.X + Bounds.BoxExtent.X;
	const double MeshTailX = Bounds.Origin.X - Bounds.BoxExtent.X;

	// A centimetre either way: both come from the same measurement, so anything larger is a
	// pipeline that was not re-run rather than a rounding difference.
	TestEqual(TEXT("the authored nose matches the mesh"), Type->Footprint.NoseX, MeshNoseX, 1.0);
	TestEqual(TEXT("the authored tail matches the mesh"), Type->Footprint.TailX, MeshTailX, 1.0);

	// AND THE ORIGIN IS THE NOSE GEAR, which is what makes the steered axle zero. A mesh
	// re-exported about some other point would pass both checks above and still steer from
	// the wrong end of the aeroplane.
	TestEqual(TEXT("plane2's steered axle is its origin, per the class convention"),
		Type->SteerAxleX, 0.0, 1.0);
	TestTrue(TEXT("and its mains are measured, aft of that origin"), Type->FixedAxleX < -100.0);

	return true;
}

#endif
