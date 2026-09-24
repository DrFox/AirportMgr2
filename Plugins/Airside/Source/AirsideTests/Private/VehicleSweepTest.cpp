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
	TestTrue(TEXT("whose kingpin is a trailer-length ahead of its axle"), Rig.Tow.Num() == 1 && Rig.Tow[0].Length > 1000.0);
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
	Rig.Tow.Add({ /*HitchX*/ 57.3, /*Length*/ 1029.5, /*BodyFront*/ 166.0, /*BodyRear*/ 121.5, /*Width*/ 254.0 });

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

#if WITH_DEV_AUTOMATION_TESTS

namespace VehicleSweepTrace
{
	VehicleSweep::FBody Rig(double KingpinToAxle = 1029.5)
	{
		VehicleSweep::FBody Body;
		Body.Wheelbase = 370.0; Body.Width = 254.0; Body.FrontX = 516.0; Body.RearX = -78.0;
		Body.Tow.Add({ /*HitchX*/ 57.3, /*Length*/ KingpinToAxle, /*BodyFront*/ 166.0, /*BodyRear*/ 121.5, /*Width*/ 254.0 });
		return Body;
	}

	/** A right-angle quadratic turn, legs Leg: the shape a junction's turn path has. */
	TArray<FVector2D> RightAngle(double Leg)
	{
		TArray<FVector2D> Path;
		for (int32 I = 0; I <= 16; ++I)
		{
			const double T = I / 16.0;
			const FVector2D A(0.0, -Leg), C(0.0, 0.0), B(Leg, 0.0);
			Path.Add(A * FMath::Square(1.0 - T) + C * (2.0 * (1.0 - T) * T) + B * (T * T));
		}
		return Path;
	}

	double MaxOf(const TArray<double>& Values)
	{
		double Out = 0.0;
		for (const double V : Values) { Out = FMath::Max(Out, V); }
		return Out;
	}
}

// A REGULATION, not a figure of ours (EU Directive 96/53/EC): an articulated vehicle must turn
// a full circle within an outer radius of 12.5 m and an inner of 5.3 m. A rig with a 7.7 m
// trailer wheelbase and our cab passes it under the steady-state model - the check that the
// maths agrees with the road. (Our tankTrailer1 is 10.3 m kingpin to axles and does NOT pass;
// see the log line below and the spec's amendments - a model question, raised 2026-09-24.)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVehicleSweepEuCircleTest, "Airside.Solve.VehicleSweepEuCircle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleSweepEuCircleTest::RunTest(const FString& Parameters)
{
	auto InnerAtOuter = [](const VehicleSweep::FBody& Body, double Outer, bool& bHolds)
	{
		double Lo = Body.Wheelbase + 1.0, Hi = 4000.0;
		for (int32 I = 0; I < 60; ++I)
		{
			const double Mid = (Lo + Hi) * 0.5;
			const double R = Mid + VehicleSweep::Envelope(Body, Mid).Outer;
			(R > Outer ? Hi : Lo) = Mid;
		}
		const VehicleSweep::FEnvelope E = VehicleSweep::Envelope(Body, Lo);
		bHolds = E.bHolds;
		return Lo - E.Inner;
	};
	bool bHolds = false;
	const double Inner = InnerAtOuter(VehicleSweepTrace::Rig(770.0), 1250.0, bHolds);
	TestTrue(TEXT("a legal-length trailer holds the EU circle"), bHolds);
	TestTrue(TEXT("and clears its 5.3 m inner radius"), Inner >= 529.0);

	bool bOursHolds = false;
	const double Ours = InnerAtOuter(VehicleSweepTrace::Rig(), 1250.0, bOursHolds);
	AddInfo(FString::Printf(TEXT("tankTrailer1 (10.3 m wheelbase) on the EU circle: holds %d, inner %.0f uu (5.3 m required)"),
		bOursHolds ? 1 : 0, Ours));
	return true;
}

// THE TURN AS DRIVEN (review of 2026-09-24): the steered axle walks the path, the fixed axle
// and the trailer trail it, and the body's reach either side is recorded against the sample it
// is nearest. A 90 degree corner ends before the trailer settles, so it cuts in LESS than the
// steady-state figure - which is what made the first cut of the Wide corner 30 m.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVehicleSweepTraceTest, "Airside.Solve.VehicleSweepTrace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVehicleSweepTraceTest::RunTest(const FString& Parameters)
{
	using namespace VehicleSweepTrace;
	TArray<double> Inner, Outer;

	TArray<FVector2D> Straight;
	for (int32 I = 0; I <= 16; ++I) { Straight.Add(FVector2D(I * 200.0, 0.0)); }
	TestTrue(TEXT("a straight road is drivable"), VehicleSweep::Trace(Rig(), Straight, Inner, Outer));
	TestEqual(TEXT("one reading per sample"), Inner.Num(), Straight.Num());
	TestTrue(TEXT("and on it the body reaches half its width either side, no more"),
		MaxOf(Inner) <= 127.0 + 1.0 && MaxOf(Outer) <= 127.0 + 1.0);

	// Hand figures from the Python prototype of this simulation (2026-09-24).
	const TArray<FVector2D> Tight = RightAngle(800.0);   // tightest radius ~566
	TestTrue(TEXT("the rig makes a tight right angle - no jack-knife"), VehicleSweep::Trace(Rig(), Tight, Inner, Outer));
	TestFalse(TEXT("where the steady-state model said the turn could not be held at all"),
		VehicleSweep::Envelope(Rig(), 566.0).bHolds);
	TestEqual(TEXT("its trailer cuts 6.3 m inside"), MaxOf(Inner), 629.0, 15.0);

	const TArray<FVector2D> Wide = RightAngle(2000.0);  // tightest radius ~1414
	VehicleSweep::Trace(Rig(), Wide, Inner, Outer);
	TestEqual(TEXT("a wider corner, 4.5 m"), MaxOf(Inner), 453.0, 15.0);
	TestTrue(TEXT("less than the steady-state figure at the same radius"),
		MaxOf(Inner) < VehicleSweep::Envelope(Rig(), 1414.0).Inner);
	return true;
}

#endif
