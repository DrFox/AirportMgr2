#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/TrafficRules.h"
#include "Model/Vehicle.h"
#include "Solve/VehicleSweep.h"

#if WITH_DEV_AUTOMATION_TESTS

// VEHICLE SIZE (spec 2026-09-23 §6). Figures measured from the .glb exports on 2026-09-24 and
// the hand figures below computed separately (Python, same geometry) - a test that called the
// production envelope to get its expected value would agree with itself.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVehicleBodyTest, "Airside.Model.VehicleBody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleBodyTest::RunTest(const FString& Parameters)
{
	const FVehicle Bowser = UAirsideSettings::ResolveDefaultVehicle();
	// Tied to the MESH through the footprint: Airside.Content.VehicleFootprintMatchesTheMesh
	// pins VehicleFootprint to the mesh's length, and the body must be that same length.
	TestEqual(TEXT("the bowser's body is as long as the footprint the mesh test pins"),
		Bowser.BodyFrontX - Bowser.BodyRearX, FTrafficRules().VehicleFootprint, 5.0);
	TestTrue(TEXT("and narrow enough for a 3 m Narrow lane, which is the point of the 6.2 m model"),
		Bowser.BodyWidth > 200.0 && Bowser.BodyWidth < 270.0);
	TestFalse(TEXT("a bowser is rigid"), Bowser.HasTrailer());

	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	TestTrue(TEXT("the rig pulls a trailer"), Rig.HasTrailer());
	TestTrue(TEXT("whose kingpin is a trailer-length ahead of its axle"), Rig.Trailer.KingpinToAxle > 1000.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVehicleSweepTest, "Airside.Solve.VehicleSweep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleSweepTest::RunTest(const FString& Parameters)
{
	VehicleSweep::FBody Rig;
	Rig.Wheelbase = 370.0;
	Rig.Width = 254.0;
	Rig.FrontX = 516.0;
	Rig.RearX = -78.0;
	Rig.KingpinX = 57.3;
	Rig.KingpinToAxle = 1029.5;
	Rig.TrailerFront = 166.0;
	Rig.TrailerRear = 121.5;
	Rig.TrailerWidth = 254.0;

	const VehicleSweep::FEnvelope At15 = VehicleSweep::Envelope(Rig, 1500.0);
	TestTrue(TEXT("the rig holds a 15 m turn"), At15.bHolds);
	TestEqual(TEXT("its trailer cuts 6 m inside the steered axle's path at 15 m"), At15.Inner, 599.13, 1.0);
	TestEqual(TEXT("and its cab swings 1.6 m outside it"), At15.Outer, 162.74, 1.0);

	const VehicleSweep::FEnvelope At20 = VehicleSweep::Envelope(Rig, 2000.0);
	TestEqual(TEXT("a wider turn cuts in less"), At20.Inner, 451.73, 1.0);

	TestFalse(TEXT("a turn tighter than the trailer can follow does not hold"),
		VehicleSweep::Envelope(Rig, 900.0).bHolds);

	VehicleSweep::FBody Bowser;
	Bowser.Wheelbase = 360.7;
	Bowser.Width = 237.4;
	Bowser.FrontX = 481.8;
	Bowser.RearX = -138.2;
	const VehicleSweep::FEnvelope B10 = VehicleSweep::Envelope(Bowser, 1000.0);
	TestTrue(TEXT("a rigid truck holds any turn wider than its wheelbase"), B10.bHolds);
	TestEqual(TEXT("its rear axle cuts inside"), B10.Inner, 186.02, 1.0);
	TestEqual(TEXT("its front corner swings out"), B10.Outer, 156.52, 1.0);
	TestFalse(TEXT("but no turn tighter than its wheelbase"), VehicleSweep::Envelope(Bowser, 300.0).bHolds);
	return true;
}

#endif
