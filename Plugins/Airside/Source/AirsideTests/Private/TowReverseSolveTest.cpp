#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"
#include "Solve/RoadGeom.h"
#include "Solve/TowReverse.h"
#include "Solve/VehicleSweep.h"

#if WITH_DEV_AUTOMATION_TESTS

// A TOW BACKING ALONG A LINE (spec 2026-09-26 §1). The two real chains - the rig's one-link
// semi and the utility's two-link drawbar - taken from the resolvers the game drives, so a
// change to either vehicle's figures is judged here too rather than against a copy.

namespace TowReverseTest
{
	/** A chain at rest facing +X, fixed axle at Fixed, laid dead straight behind. */
	TowReverse::FInput StraightInput(const FVehicle& Vehicle, const FVector2D& Fixed)
	{
		TowReverse::FInput In;
		In.Body = VehicleFit::BodyOf(Vehicle);
		In.MaxSteerRadians = FMath::DegreesToRadians(Vehicle.Chassis.Ground.MaxSteerDegrees);
		In.Fixed = Fixed;
		In.Heading = 0.0;
		VehicleSweep::LayChainStraight(In.Body, Fixed, FVector2D(1.0, 0.0), In.Axles);
		return In;
	}

	/**
	 * From Start travelling -X for Straight, a quarter circle of Radius turning toward +Y
	 * (Sign +1) or -Y (-1), then On straight - the shape a 90 degree bay asks for, 20 uu apart.
	 */
	TArray<FVector2D> BayLine(const FVector2D& Start, double Straight, double Radius, double Sign, double On)
	{
		TArray<FVector2D> Out;
		const double Spacing = 20.0;
		for (double D = 0.0; D < Straight; D += Spacing)
		{
			Out.Add(Start + FVector2D(-D, 0.0));
		}
		const FVector2D ArcStart = Start + FVector2D(-Straight, 0.0);
		const FVector2D Centre = ArcStart + FVector2D(0.0, Sign * Radius);
		const int32 ArcSteps = FMath::Max(8, FMath::CeilToInt32(Radius * UE_HALF_PI / Spacing));
		for (int32 K = 0; K < ArcSteps; ++K)
		{
			// Angle measured from the centre: starts pointing back at ArcStart (-Sign*Y).
			const double T = UE_HALF_PI * K / ArcSteps;
			Out.Add(Centre + FVector2D(-FMath::Sin(T) * Radius, -Sign * FMath::Cos(T) * Radius));
		}
		const FVector2D ArcEnd = Centre + FVector2D(-Radius, 0.0);
		for (double D = 0.0; D <= On; D += Spacing)
		{
			Out.Add(ArcEnd + FVector2D(0.0, Sign * D));
		}
		return Out;
	}

	double WorstHitch(const TowReverse::FSolution& Solution)
	{
		double Worst = 0.0;
		for (const TowReverse::FSample& Sample : Solution.Samples)
		{
			Worst = FMath::Max(Worst, FMath::Abs(Sample.HitchRadians));
		}
		return Worst;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseBodyTest, "Airside.Solve.TowReverse.ReverseBodyMergesTheTurntable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowReverseBodyTest::RunTest(const FString& Parameters)
{
	// The turntable lock: towbar (hitch -90.7, 114) and body (0, 221) are one link from the
	// utility's hitch to the trailer's rear axle, 335 uu - fuelTrailer1's towbar eye to its axle.
	const VehicleSweep::FBody Utility = TowReverse::ReverseBody(VehicleFit::BodyOf(UAirsideSettings::ResolveUtilityTowVehicle()));
	TestEqual(TEXT("utility backs as one link"), Utility.Tow.Num(), 1);
	if (Utility.Tow.Num() == 1)
	{
		TestNearlyEqual(TEXT("from the utility's own hitch"), Utility.Tow[0].HitchX, -90.7, 0.01);
		TestNearlyEqual(TEXT("to the trailer's rear axle"), Utility.Tow[0].Length, 335.0, 0.01);
	}
	// The rig has one joint already, so the lock changes nothing.
	const VehicleSweep::FBody RigFull = VehicleFit::BodyOf(UAirsideSettings::ResolveRigVehicle());
	const VehicleSweep::FBody Rig = TowReverse::ReverseBody(RigFull);
	TestTrue(TEXT("rig unchanged"), Rig.Tow.Num() == 1 && Rig.Tow[0].Length == RigFull.Tow[0].Length
		&& Rig.Tow[0].HitchX == RigFull.Tow[0].HitchX);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseSteadyTest, "Airside.Solve.TowReverse.SteadyStateHolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowReverseSteadyTest::RunTest(const FString& Parameters)
{
	// THE REFERENCE MUST BE A REAL EQUILIBRIUM of the chain the solver steps, or the controller
	// chases an angle the trailer cannot hold. Drive the tractor round its steady circle with the
	// chain started at SteadyHitchRadians and StepChain doing the rest: the angle must not drift.
	for (const FVehicle& Vehicle : { UAirsideSettings::ResolveRigVehicle(), UAirsideSettings::ResolveUtilityTowVehicle() })
	{
		const VehicleSweep::FBody Body = TowReverse::ReverseBody(VehicleFit::BodyOf(Vehicle));
		const double A = Body.Tow[0].HitchX;
		const double L = Body.Tow[0].Length;
		const double Rt = 2000.0;
		const double Rf = FMath::Sqrt(Rt * Rt + L * L - A * A);
		const double Phi = TowReverse::SteadyHitchRadians(Body, 1.0 / Rt);

		// Tractor on a circle of radius Rf about the origin, turning +theta (CCW), starting at
		// angle 0 on the circle facing +Y.
		FVector2D Fixed(Rf, 0.0);
		double Theta = UE_HALF_PI;
		const FVector2D Hitch = Fixed + FVector2D(FMath::Cos(Theta), FMath::Sin(Theta)) * A;
		const double Psi = Theta - Phi;
		TArray<FVector2D> Axles = { Hitch - FVector2D(FMath::Cos(Psi), FMath::Sin(Psi)) * L };
		const double StepAngle = 5.0 / Rf;
		double Worst = 0.0;
		for (int32 K = 0; K < FMath::CeilToInt32(2000.0 / 5.0); ++K)
		{
			const double Around = (K + 1) * StepAngle;
			Fixed = FVector2D(FMath::Cos(Around), FMath::Sin(Around)) * Rf;
			Theta = UE_HALF_PI + Around;
			int32 Folded = INDEX_NONE;
			double Fold = 0.0;
			VehicleSweep::StepChain(Body, Fixed, FVector2D(FMath::Cos(Theta), FMath::Sin(Theta)), Axles, Folded, Fold);
			const FVector2D HitchNow = Fixed + FVector2D(FMath::Cos(Theta), FMath::Sin(Theta)) * A;
			const FVector2D G = (HitchNow - Axles[0]).GetSafeNormal();
			const double PhiNow = FMath::UnwindRadians(Theta - FMath::Atan2(G.Y, G.X));
			Worst = FMath::Max(Worst, FMath::Abs(PhiNow - Phi));
		}
		TestTrue(FString::Printf(TEXT("%s: steady hitch %.2f deg holds (drift %.3f deg)"), *Vehicle.TypeCode.ToString(),
			FMath::RadiansToDegrees(Phi), FMath::RadiansToDegrees(Worst)), FMath::RadiansToDegrees(Worst) < 0.1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseStraightTest, "Airside.Solve.TowReverse.StraightIsExact",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowReverseStraightTest::RunTest(const FString& Parameters)
{
	// OPTION B, ruled 2026-09-24: a chain already in line backs straight in line. No steering,
	// no drift - the straight reverse is an equilibrium, so any error here is the solver's own.
	for (const FVehicle& Vehicle : { UAirsideSettings::ResolveRigVehicle(), UAirsideSettings::ResolveUtilityTowVehicle() })
	{
		TowReverse::FInput In = TowReverseTest::StraightInput(Vehicle, FVector2D(5000.0, 300.0));
		In.Line = { In.Axles.Last(), In.Axles.Last() + FVector2D(-3000.0, 0.0) };
		const TowReverse::FSolution Solution = TowReverse::Solve(In);
		const FString Name = Vehicle.TypeCode.ToString();
		TestTrue(*(Name + TEXT(": solved - ") + Solution.Describe()), Solution.IsValid());
		double WorstOff = 0.0;
		double WorstSteer = 0.0;
		for (const TowReverse::FSample& Sample : Solution.Samples)
		{
			for (const FVector2D& Axle : Sample.Axles)
			{
				WorstOff = FMath::Max(WorstOff, FMath::Abs(Axle.Y - 300.0));
			}
			WorstOff = FMath::Max(WorstOff, FMath::Abs(Sample.Fixed.Y - 300.0));
			WorstSteer = FMath::Max(WorstSteer, FMath::Abs(Sample.SteerDegrees));
		}
		TestTrue(FString::Printf(TEXT("%s: every axle stays on the line (worst %.4f uu)"), *Name, WorstOff), WorstOff < 0.01);
		TestTrue(FString::Printf(TEXT("%s: no steering (worst %.4f deg)"), *Name, WorstSteer), WorstSteer < 0.01);
		if (Solution.Samples.Num() > 0)
		{
			TestTrue(TEXT("ends on the line's end"), FVector2D::Distance(Solution.Samples.Last().Axles.Last(), In.Line.Last()) < 1.0);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseArcTest, "Airside.Solve.TowReverse.ArcTrackedWithinTolerance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowReverseArcTest::RunTest(const FString& Parameters)
{
	// A 90 DEGREE BAY, both ways round: the line retraces the approach, bends, and runs into the
	// bay. Radii are the ones a reverse turn lays for each (rig 1500, the utility's shorter link
	// 800). Tracked within the solver's own line limit and landed inside the end-pose limits.
	struct FCase { FVehicle Vehicle; double Radius; };
	for (const FCase& Case : { FCase{ UAirsideSettings::ResolveRigVehicle(), 1500.0 }, FCase{ UAirsideSettings::ResolveUtilityTowVehicle(), 800.0 } })
	{
		for (const double Sign : { 1.0, -1.0 })
		{
			TowReverse::FInput In = TowReverseTest::StraightInput(Case.Vehicle, FVector2D(0.0, 0.0));
			In.Line = TowReverseTest::BayLine(In.Axles.Last(), 2000.0, Case.Radius, Sign, 2500.0);
			const TowReverse::FSolution Solution = TowReverse::Solve(In);
			const FString Name = FString::Printf(TEXT("%s R%.0f %s"), *Case.Vehicle.TypeCode.ToString(), Case.Radius,
				Sign > 0.0 ? TEXT("+Y") : TEXT("-Y"));
			TestTrue(*(Name + TEXT(": ") + Solution.Describe()), Solution.IsValid());
			AddInfo(Name + TEXT(": ") + Solution.Describe());
			TestTrue(*(Name + TEXT(": within the line limit")), Solution.WorstLineError <= TowReverse::MaxLineError);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseTooTightTest, "Airside.Solve.TowReverse.TooTightIsRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowReverseTooTightTest::RunTest(const FString& Parameters)
{
	// A 10 m trailer cannot back round a 3 m radius: no playable solution, and the reason names
	// its figure so the log that refuses it is enough to find the bay that asked.
	TowReverse::FInput In = TowReverseTest::StraightInput(UAirsideSettings::ResolveRigVehicle(), FVector2D::ZeroVector);
	In.Line = TowReverseTest::BayLine(In.Axles.Last(), 2000.0, 300.0, 1.0, 2500.0);
	const TowReverse::FSolution Solution = TowReverse::Solve(In);
	TestFalse(TEXT("refused"), Solution.IsValid());
	TestTrue(TEXT("no samples to play"), Solution.Samples.Num() == 0);
	TestTrue(TEXT("jackknife or off the line: ") + Solution.Describe(),
		Solution.Refusal == TowReverse::ERefusal::Jackknife || Solution.Refusal == TowReverse::ERefusal::OffLine);
	TestTrue(TEXT("names a figure"), Solution.Describe().Contains(TEXT("(")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseBentTest, "Airside.Solve.TowReverse.BentTurntableIsRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowReverseBentTest::RunTest(const FString& Parameters)
{
	// THE LOCK ENGAGES ONLY NEAR STRAIGHT (ruling 2026-09-26). Bend the turntable 5 degrees -
	// the body link turned about the towbar's axle - and the solve refuses before moving anything.
	TowReverse::FInput In = TowReverseTest::StraightInput(UAirsideSettings::ResolveUtilityTowVehicle(), FVector2D::ZeroVector);
	const double Body = In.Body.Tow[1].Length;
	const double Bend = FMath::DegreesToRadians(5.0);
	In.Axles[1] = In.Axles[0] + FVector2D(-FMath::Cos(Bend), FMath::Sin(Bend)) * Body;
	In.Line = { In.Axles.Last(), In.Axles.Last() + FVector2D(-3000.0, 0.0) };
	const TowReverse::FSolution Solution = TowReverse::Solve(In);
	TestTrue(TEXT("turntable bent: ") + Solution.Describe(), Solution.Refusal == TowReverse::ERefusal::TurntableBent);
	TestNearlyEqual(TEXT("names the bend"), Solution.Figure, 5.0, 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseLockSnapTest, "Airside.Solve.TowReverse.LockSnapsWithinTolerance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowReverseLockSnapTest::RunTest(const FString& Parameters)
{
	// Just inside the lock (2.9 deg): accepted, and the snap to straight moves the towbar's axle
	// by no more than the lock tolerance allows - so the view never jumps by more than that.
	TowReverse::FInput In = TowReverseTest::StraightInput(UAirsideSettings::ResolveUtilityTowVehicle(), FVector2D::ZeroVector);
	const double Body = In.Body.Tow[1].Length;
	const double Towbar = In.Body.Tow[0].Length;
	const double Bend = FMath::DegreesToRadians(2.9);
	In.Axles[1] = In.Axles[0] + FVector2D(-FMath::Cos(Bend), FMath::Sin(Bend)) * Body;
	In.Line = { In.Axles.Last(), In.Axles.Last() + FVector2D(-3000.0, 0.0) };
	const TowReverse::FSolution Solution = TowReverse::Solve(In);
	TestTrue(TEXT("accepted: ") + Solution.Describe(), Solution.IsValid());
	if (Solution.Samples.Num() > 0)
	{
		const double Moved = FVector2D::Distance(Solution.Samples[0].Axles[0], In.Axles[0]);
		const double Allowed = (Towbar + Body) * FMath::Sin(FMath::DegreesToRadians(TowReverse::TurntableLockDegrees));
		TestTrue(FString::Printf(TEXT("towbar axle snapped %.1f uu (<= %.1f)"), Moved, Allowed), Moved <= Allowed);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseNoJackknifeTest, "Airside.Solve.TowReverse.NeverJackknives",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowReverseNoJackknifeTest::RunTest(const FString& Parameters)
{
	// Every playable solution keeps its hitch inside the critical angle at EVERY sample - the
	// check is per sample, not just the worst the solver reports about itself.
	for (const FVehicle& Vehicle : { UAirsideSettings::ResolveRigVehicle(), UAirsideSettings::ResolveUtilityTowVehicle() })
	{
		TowReverse::FInput In = TowReverseTest::StraightInput(Vehicle, FVector2D::ZeroVector);
		In.Line = TowReverseTest::BayLine(In.Axles.Last(), 2000.0, 1500.0, 1.0, 2500.0);
		const TowReverse::FSolution Solution = TowReverse::Solve(In);
		const double Critical = TowReverse::CriticalHitchRadians(TowReverse::ReverseBody(In.Body), In.MaxSteerRadians);
		TestTrue(*(Vehicle.TypeCode.ToString() + TEXT(": solved")), Solution.IsValid());
		TestTrue(FString::Printf(TEXT("%s: worst %.1f deg < critical %.1f deg"), *Vehicle.TypeCode.ToString(),
			FMath::RadiansToDegrees(TowReverseTest::WorstHitch(Solution)), FMath::RadiansToDegrees(Critical)),
			TowReverseTest::WorstHitch(Solution) < Critical);
	}
	return true;
}

#endif
