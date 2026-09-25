#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/VehicleSweep.h"

#if WITH_DEV_AUTOMATION_TESTS

// ONE TRAILER STEPPER (2026-09-24): VehicleSweep::Trace inlined this pursuit; Task 2's driving
// agent needs the same step, so it is pulled out here and Trace is made to call it, rather than
// have two evaluators of the one kinematic rule drift apart. See the guideline-graph "samples
// ONCE" invariant in CLAUDE.md for why a second copy is the danger, not just duplication.

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

namespace TrailerStepTraceMirror
{
	FVector2D Perp(const FVector2D& V) { return FVector2D(-V.Y, V.X); }

	/**
	 * A line-for-line mirror of VehicleSweep::Trace's own loop (Private/Solve/VehicleSweep.cpp),
	 * with the trailer pursuit routed through StepTrailer/TrailerHeading instead of inlined. If
	 * Trace and this mirror ever disagree, either Trace stopped calling the shared stepper or
	 * this mirror drifted from it - the point of the "samples ONCE" rule (CLAUDE.md) is that
	 * there is exactly one place the pursuit math lives, and this test is what would notice a
	 * second one creeping back in.
	 */
	bool MirrorTrace(const VehicleSweep::FBody& Body, TArrayView<const FVector2D> Path,
		TArray<double>& OutInner, TArray<double>& OutOuter)
	{
		OutInner.Init(0.0, Path.Num());
		OutOuter.Init(0.0, Path.Num());
		if (Path.Num() < 2) { return true; }

		constexpr double Step = 10.0;
		const FVector2D InTangent = (Path[1] - Path[0]).GetSafeNormal();
		const FVector2D OutTangent = (Path.Last() - Path[Path.Num() - 2]).GetSafeNormal();
		// ONE TRAILER, read off the chain's first link: since 2026-09-24 Trace walks a chain
		// (VehicleSweep::StepChain), and this mirror keeps the one-trailer loop it replaced, so
		// the comparison below also pins that a one-link chain IS the old semi-trailer.
		const VehicleSweep::FLink Trailer = Body.Tow.Num() > 0 ? Body.Tow[0] : VehicleSweep::FLink();
		const double Lead = Body.Wheelbase + Trailer.HitchX + Trailer.Length + Body.FrontX + 200.0;

		const FVector2D Mid = Path[Path.Num() / 2];
		const double Turn = FVector2D::CrossProduct(Mid - Path[0], Path.Last() - Mid);
		const double InwardSign = Turn >= 0.0 ? 1.0 : -1.0;

		TArray<FVector2D> Steps;
		for (double D = Lead; D > 0.0; D -= Step) { Steps.Add(Path[0] - InTangent * D); }
		for (int32 Index = 0; Index + 1 < Path.Num(); ++Index)
		{
			const FVector2D A = Path[Index];
			const FVector2D B = Path[Index + 1];
			const int32 Count = FMath::Max(1, FMath::FloorToInt32(FVector2D::Distance(A, B) / Step));
			for (int32 Sub = 0; Sub < Count; ++Sub)
			{
				Steps.Add(A + (B - A) * (static_cast<double>(Sub) / Count));
			}
		}
		for (double D = 0.0; D < Lead; D += Step) { Steps.Add(Path.Last() + OutTangent * D); }

		FVector2D Fixed = Steps[0] - InTangent * Body.Wheelbase;
		FVector2D Kingpin = Fixed + InTangent * Trailer.HitchX;
		FVector2D TrailerAxle = Kingpin - InTangent * Trailer.Length;
		const bool bTrailer = Trailer.Length > 0.0;

		TArray<FVector2D, TInlineAllocator<16>> Corners;
		for (const FVector2D& Steered : Steps)
		{
			FVector2D Heading = (Steered - Fixed).GetSafeNormal();
			Fixed = Steered - Heading * Body.Wheelbase;
			Heading = (Steered - Fixed).GetSafeNormal();
			const FVector2D Side = Perp(Heading) * (Body.Width * 0.5);

			Corners.Reset();
			for (const double X : { Body.FrontX, Body.RearX, 0.0 })
			{
				Corners.Add(Fixed + Heading * X + Side);
				Corners.Add(Fixed + Heading * X - Side);
			}
			if (bTrailer)
			{
				Kingpin = Fixed + Heading * Trailer.HitchX;
				if (!VehicleSweep::StepTrailer(Kingpin, Heading, Trailer.Length, TrailerAxle))
				{
					return false;
				}
				const FVector2D Trailing = VehicleSweep::TrailerHeading(Kingpin, TrailerAxle);
				const FVector2D TrailerSide = Perp(Trailing) * (Trailer.Width * 0.5);
				for (const double X : { Trailer.Length + Trailer.BodyFront, -Trailer.BodyRear, 0.0, Trailer.Length * 0.5 })
				{
					Corners.Add(TrailerAxle + Trailing * X + TrailerSide);
					Corners.Add(TrailerAxle + Trailing * X - TrailerSide);
				}
			}

			for (const FVector2D& Corner : Corners)
			{
				double Best = TNumericLimits<double>::Max();
				int32 BestSpan = 0;
				double BestT = 0.0;
				for (int32 Span = 0; Span + 1 < Path.Num(); ++Span)
				{
					const FVector2D AB = Path[Span + 1] - Path[Span];
					const double T = FMath::Clamp(FVector2D::DotProduct(Corner - Path[Span], AB)
						/ FMath::Max(AB.SizeSquared(), UE_DOUBLE_SMALL_NUMBER), 0.0, 1.0);
					const double Distance = FVector2D::Distance(Corner, Path[Span] + AB * T);
					if (Distance < Best)
					{
						Best = Distance;
						BestSpan = Span;
						BestT = T;
					}
				}
				if ((BestSpan == 0 && BestT <= 0.0) || (BestSpan == Path.Num() - 2 && BestT >= 1.0))
				{
					continue;
				}
				const FVector2D AB = Path[BestSpan + 1] - Path[BestSpan];
				const FVector2D Foot = Path[BestSpan] + AB * BestT;
				const double Lateral = FVector2D::DotProduct(Corner - Foot, Perp(AB.GetSafeNormal()) * InwardSign);
				const int32 Sample = BestT < 0.5 ? BestSpan : BestSpan + 1;
				if (Lateral > 0.0)
				{
					OutInner[Sample] = FMath::Max(OutInner[Sample], Lateral);
				}
				else
				{
					OutOuter[Sample] = FMath::Max(OutOuter[Sample], -Lateral);
				}
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrailerStepTraceUsesStepperTest, "Airside.Solve.TrailerStep.TraceUsesTheStepper",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrailerStepTraceUsesStepperTest::RunTest(const FString& Parameters)
{
	// Trace's trailer corners for a 90 degree path must equal a hand loop over StepTrailer, run
	// on the same samples Trace walks - ONE evaluator, not two that happen to agree today.
	VehicleSweep::FBody Body;
	Body.Wheelbase = 370.0; Body.Width = 254.0; Body.FrontX = 516.0; Body.RearX = -78.0;
	Body.Tow.Add({ /*HitchX*/ 57.3, /*Length*/ 1029.5, /*BodyFront*/ 166.0, /*BodyRear*/ 121.5, /*Width*/ 254.0 });

	TArray<FVector2D> Path;
	for (int32 I = 0; I <= 16; ++I)
	{
		const double T = I / 16.0;
		const FVector2D A(0.0, -800.0), C(0.0, 0.0), B(800.0, 0.0);
		Path.Add(A * FMath::Square(1.0 - T) + C * (2.0 * (1.0 - T) * T) + B * (T * T));
	}

	TArray<double> TraceInner, TraceOuter;
	TestTrue(TEXT("Trace holds the turn"), VehicleSweep::Trace(Body, Path, TraceInner, TraceOuter));

	TArray<double> MirrorInner, MirrorOuter;
	TestTrue(TEXT("the hand loop over StepTrailer holds it too"),
		TrailerStepTraceMirror::MirrorTrace(Body, Path, MirrorInner, MirrorOuter));

	TestEqual(TEXT("same number of inner samples"), MirrorInner.Num(), TraceInner.Num());
	TestEqual(TEXT("same number of outer samples"), MirrorOuter.Num(), TraceOuter.Num());
	bool bAllMatch = true;
	for (int32 I = 0; I < TraceInner.Num() && bAllMatch; ++I)
	{
		if (!FMath::IsNearlyEqual(MirrorInner[I], TraceInner[I], 0.01) ||
			!FMath::IsNearlyEqual(MirrorOuter[I], TraceOuter[I], 0.01))
		{
			bAllMatch = false;
		}
	}
	TestTrue(TEXT("Trace's own trailer corners exactly match a hand loop over StepTrailer, sample for sample"), bAllMatch);
	return true;
}

#endif
