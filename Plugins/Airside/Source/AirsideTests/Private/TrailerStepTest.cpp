#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/VehicleSweep.h"

#if WITH_DEV_AUTOMATION_TESTS

// ONE TRAILER STEPPER (2026-09-24): VehicleSweep::Trace inlined this pursuit; Task 2's driving
// agent needs the same step, so it is pulled out here and Trace is made to call it, rather than
// have two evaluators of the one kinematic rule drift apart. See the guideline-graph "samples
// ONCE" invariant in CLAUDE.md for why a second copy is the danger, not just duplication.
//
// NO PARITY TEST AGAINST TRACE (Airside.Solve.TrailerStep.TraceUsesTheStepper, removed in #462): it held
// Trace's trailer corners against a 110-line hand copy of Trace's own loop routed through StepTrailer.
// Trace stopped using that loop when it began walking a chain (VehicleSweep::StepChain, cbe729d6), so the
// copy was a second evaluator kept ONLY to be compared with - the thing the invariant above forbids - and a
// comparison that fails when the copy drifts says nothing about the road. The pursuit's output is pinned by
// figures that do not come from this code: Airside.Solve.VehicleSweepTrace's hand figures from the Python
// prototype (a tight corner's trailer cutting 6.3 m inside, a wide one 4.5 m).
// ENFORCED BY: Airside.Solve.VehicleSweepTrace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrailerStepStraightTest, "Airside.Solve.TrailerStep.StraightStaysStraight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrailerStepStraightTest::RunTest(const FString& Parameters)
{
	const double KingpinToAxle = 1029.5;
	FVector2D Axle(-KingpinToAxle, 0.0);   // starts directly behind the kingpin, on the X axis
	FVector2D Kingpin(0.0, 0.0);
	const FVector2D CabHeading(1.0, 0.0);

	// A kingpin moving along +X in 10 uu steps: an axle that starts on the line and is pulled
	// straight toward it never leaves the X axis - the pursuit has no lateral component to induce.
	for (int32 Step = 0; Step < 50; ++Step)
	{
		Kingpin.X += 10.0;
		const bool bOk = VehicleSweep::StepTrailer(Kingpin, CabHeading, KingpinToAxle, Axle);
		TestTrue(TEXT("straight pursuit never jack-knifes"), bOk);
		TestEqual(TEXT("the axle stays on the X axis"), Axle.Y, 0.0, 0.001);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrailerStepCircleTest, "Airside.Solve.TrailerStep.SteadyCircleConverges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrailerStepCircleTest::RunTest(const FString& Parameters)
{
	// The analytic steady tractrix: a kingpin held on a circle of radius Rk settles the trailer
	// axle onto a concentric circle of radius sqrt(Rk^2 - L^2) (Envelope's own derivation, Solve/
	// VehicleSweep.cpp). Starting the axle away from that radius and driving many small steps
	// around the circle should converge onto it.
	const double L = 1029.5;
	const double Rk = 1500.0;
	const double ExpectedAxleRadius = FMath::Sqrt(Rk * Rk - L * L);

	FVector2D Axle(Rk - L, 0.0);   // deliberately off the steady radius
	// A finer step than Trace's own 10 uu road-sample step: StepTrailer's pull-toward-new-kingpin
	// rule is a first-order (one-step-lag) discretisation of the continuous tractrix, so its bias
	// against the closed-form steady radius shrinks with step size - this pins the analytic limit
	// the primitive approximates, not Trace's road-sampling resolution.
	const double Step = 1.0;
	const double Circumference = 2.0 * PI * Rk;
	const int32 NumSteps = FMath::CeilToInt32(2.0 * Circumference / Step);   // a couple of laps to settle

	double Angle = 0.0;
	for (int32 I = 0; I < NumSteps; ++I)
	{
		Angle += Step / Rk;
		const FVector2D Kingpin(Rk * FMath::Cos(Angle), Rk * FMath::Sin(Angle));
		const FVector2D CabHeading(-FMath::Sin(Angle), FMath::Cos(Angle));   // tangent, direction of travel
		const bool bOk = VehicleSweep::StepTrailer(Kingpin, CabHeading, L, Axle);
		TestTrue(TEXT("a held circle wider than the trailer never jack-knifes"), bOk);
	}

	TestEqual(TEXT("the axle settles on the steady-state radius"), Axle.Size(), ExpectedAxleRadius, 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrailerStepJackknifeTest, "Airside.Solve.TrailerStep.JackknifeDetected",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrailerStepJackknifeTest::RunTest(const FString& Parameters)
{
	const double KingpinToAxle = 1029.5;
	FVector2D Axle(-KingpinToAxle, 0.0);
	FVector2D Kingpin(0.0, 0.0);
	const FVector2D CabHeading(1.0, 0.0);

	// Settle the trailer straight behind the cab first, as Trace would after a lead-in.
	for (int32 Step = 0; Step < 20; ++Step)
	{
		Kingpin.X += 10.0;
		VehicleSweep::StepTrailer(Kingpin, CabHeading, KingpinToAxle, Axle);
	}
	TestEqual(TEXT("settled dead behind the cab"), Axle.X, Kingpin.X - KingpinToAxle, 0.01);

	// The kingpin reverses STRAIGHT BACK, past where the axle already sits, in one motion: the
	// hitch is now behind the axle it is meant to be pulling, on the cab's own nose line - the
	// axle cannot get there without the trailer swinging through square to the cab. A gradual
	// 10 uu-at-a-time retreat never triggers this (each step keeps the same L-Step lead the
	// pursuit rule preserves); the point of "reversing straight BACK OVER its axle" is the
	// hitch crossing to the far side, which is what a real reversing jack-knife is.
	const bool bOk = VehicleSweep::StepTrailer(FVector2D(Axle.X - 500.0, 0.0), CabHeading, KingpinToAxle, Axle);
	TestFalse(TEXT("a kingpin reversing straight back over its axle jack-knifes"), bOk);
	return true;
}

#endif
