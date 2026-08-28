#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/JunctionSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FJunctionArm MakeArm(const FVector2D& Tangent, double HalfWidth, double Radius)
	{
		FJunctionArm Arm;
		Arm.Tangent = Tangent.GetSafeNormal();
		Arm.HalfWidthLeft = HalfWidth;
		Arm.HalfWidthRight = HalfWidth;
		Arm.FilletRadius = Radius;
		return Arm;
	}

	FJunctionArm MakeArmAtBearing(double Bearing, double HalfWidth, double Radius)
	{
		return MakeArm(FVector2D(FMath::Cos(Bearing), FMath::Sin(Bearing)), HalfWidth, Radius);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadJunctionCutTest,
	"RoadNet.Solve.JunctionCuts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadJunctionCutTest::RunTest(const FString& Parameters)
{
	constexpr double W = 1150.0;

	// --- Four-way, equal widths, arms sorted CCW from east ---
	{
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.Arms.Add(MakeArm(FVector2D( 1.0,  0.0), W, 1500.0));  // east    0
		Input.Arms.Add(MakeArm(FVector2D( 0.0,  1.0), W, 1500.0));  // north  90
		Input.Arms.Add(MakeArm(FVector2D(-1.0,  0.0), W, 1500.0));  // west  180
		Input.Arms.Add(MakeArm(FVector2D( 0.0, -1.0), W, 1500.0));  // south 270

		const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		TestTrue(TEXT("4-way solves"), Result.bValid);
		TestEqual(TEXT("4-way arm results"), Result.Arms.Num(), 4);
		TestEqual(TEXT("4-way corners"), Result.Corners.Num(), 4);

		// Every corner is a convex 90deg, so the corner point sits at distance W along
		// each arm and the fillet pushes the cut a further R back: cut = W + R.
		// The cut must clear the corner point, or adjacent arms overlap through the node.
		constexpr double ExpectedCut = W + 1500.0;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			TestTrue(TEXT("4-way cut clears the corner point"),
				FMath::IsNearlyEqual(Result.Arms[Index].CutDistance, ExpectedCut, 1e-6));
			TestTrue(TEXT("4-way cut is beyond the half-width"),
				Result.Arms[Index].CutDistance > W);
		}

		TestTrue(TEXT("east left cut"),
			Result.Arms[0].LeftCut.Equals(FVector2D(ExpectedCut,  W), 1e-6));
		TestTrue(TEXT("east right cut"),
			Result.Arms[0].RightCut.Equals(FVector2D(ExpectedCut, -W), 1e-6));

		// North arm: left is the -X side, right is the +X side.
		TestTrue(TEXT("north left cut"),
			Result.Arms[1].LeftCut.Equals(FVector2D(-W, ExpectedCut), 1e-6));
		TestTrue(TEXT("north right cut"),
			Result.Arms[1].RightCut.Equals(FVector2D( W, ExpectedCut), 1e-6));

		// THE overlap invariant: no arm's cut line may reach another arm's ribbon.
		// Arm 0 owns x >= cut with |y| <= W; arm 1 owns y >= cut with |x| <= W.
		// They are disjoint exactly when cut > W.
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const FJunctionArm& Arm = Input.Arms[Index];
			const double Along = Result.Arms[Index].CutDistance;
			for (int32 Other = 0; Other < 4; ++Other)
			{
				if (Other == Index) { continue; }
				const FJunctionArm& OtherArm = Input.Arms[Other];
				// Distance from this arm's cut centre to the other arm's centreline.
				const FVector2D CutCentre = Arm.Tangent * Along;
				const double Lateral = FMath::Abs(FVector2D::DotProduct(
					CutCentre, RoadGeom::PerpCCW(OtherArm.Tangent)));
				const double Longitudinal = FVector2D::DotProduct(CutCentre, OtherArm.Tangent);
				const bool bInsideOtherRibbon =
					Lateral < OtherArm.HalfWidthLeft - 1e-6 &&
					Longitudinal > Result.Arms[Other].CutDistance + 1e-6;
				TestFalse(TEXT("cut vertex does not fall inside another arm's ribbon"),
					bInsideOtherRibbon);
			}
		}
	}

	// --- Straight-through node: the COMMON case after R9 auto-subdivision ---
	{
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.Arms.Add(MakeArm(FVector2D( 1.0, 0.0), W, 1500.0));  // east
		Input.Arms.Add(MakeArm(FVector2D(-1.0, 0.0), W, 1500.0));  // west

		const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		TestTrue(TEXT("collinear solves"), Result.bValid);
		TestTrue(TEXT("corner 0 straight"), Result.Corners[0].bStraightThrough);
		TestTrue(TEXT("corner 1 straight"), Result.Corners[1].bStraightThrough);

		// Nothing is trimmed: a subdivided straight run must not facet.
		TestTrue(TEXT("east cut is zero"), FMath::IsNearlyEqual(Result.Arms[0].CutDistance, 0.0, 1e-6));
		TestTrue(TEXT("west cut is zero"), FMath::IsNearlyEqual(Result.Arms[1].CutDistance, 0.0, 1e-6));

		// The two arms' cut lines must coincide exactly, or a straight run shows a seam.
		TestTrue(TEXT("east left meets west right"),
			Result.Arms[0].LeftCut.Equals(Result.Arms[1].RightCut, 1e-9));
		TestTrue(TEXT("east right meets west left"),
			Result.Arms[0].RightCut.Equals(Result.Arms[1].LeftCut, 1e-9));
	}

	// --- Dead end ---
	{
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.Arms.Add(MakeArm(FVector2D(1.0, 0.0), W, 1500.0));

		const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		TestTrue(TEXT("dead end solves"), Result.bValid);
		TestEqual(TEXT("dead end arms"), Result.Arms.Num(), 1);
		TestEqual(TEXT("dead end has no corners"), Result.Corners.Num(), 0);
		TestTrue(TEXT("dead end cut"), FMath::IsNearlyEqual(Result.Arms[0].CutDistance, W, 1e-6));
	}

	// --- Mixed widths: a narrow taxiway meeting a wide runway (R10) ---
	{
		constexpr double RunwayHalf = 2250.0;   // 45 m runway
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.Arms.Add(MakeArm(FVector2D( 1.0, 0.0), RunwayHalf, 3000.0));  // runway east
		Input.Arms.Add(MakeArm(FVector2D( 0.0, 1.0), W,          1500.0));  // taxiway north
		Input.Arms.Add(MakeArm(FVector2D(-1.0, 0.0), RunwayHalf, 3000.0));  // runway west

		const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		TestTrue(TEXT("mixed width solves"), Result.bValid);
		for (const FJunctionArmResult& Arm : Result.Arms)
		{
			TestTrue(TEXT("mixed cut non-negative"), Arm.CutDistance >= -1e-9);
			TestTrue(TEXT("mixed cut finite"), FMath::IsFinite(Arm.CutDistance));
		}

		// Each arm keeps its OWN width across the cut: the runway must not narrow
		// to the taxiway's width just because they share a node.
		const double RunwayCutWidth = (Result.Arms[0].LeftCut - Result.Arms[0].RightCut).Length();
		const double TaxiwayCutWidth = (Result.Arms[1].LeftCut - Result.Arms[1].RightCut).Length();
		TestTrue(TEXT("runway keeps its width"), FMath::IsNearlyEqual(RunwayCutWidth, RunwayHalf * 2.0, 1e-6));
		TestTrue(TEXT("taxiway keeps its width"), FMath::IsNearlyEqual(TaxiwayCutWidth, W * 2.0, 1e-6));
	}

	// --- Every arm count and angle the gallery will exercise ---
	{
		const TArray<TArray<double>> BearingSets = {
			{ 0.0, UE_DOUBLE_PI * (15.0 / 180.0) },                                    // acute 2-way
			{ 0.0, UE_DOUBLE_PI * (170.0 / 180.0) },                                   // near-straight 2-way
			{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI },                                 // 3-way T
			{ 0.0, UE_DOUBLE_PI * 0.6667, UE_DOUBLE_PI * 1.3333 },                     // 3-way Y
			{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI, UE_DOUBLE_PI * 1.5 },             // 4-way
			{ 0.0, UE_DOUBLE_PI * 0.4, UE_DOUBLE_PI * 0.8,
			  UE_DOUBLE_PI * 1.2, UE_DOUBLE_PI * 1.6 }                                 // 5-way
		};

		for (int32 CaseIndex = 0; CaseIndex < BearingSets.Num(); ++CaseIndex)
		{
			FJunctionInput Input;
			Input.Position = FVector2D::ZeroVector;
			for (const double Bearing : BearingSets[CaseIndex])
			{
				Input.Arms.Add(MakeArmAtBearing(Bearing, W, 1500.0));
			}

			const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
			const FString Label = FString::Printf(TEXT("case %d"), CaseIndex);

			TestTrue(*(Label + TEXT(" solves")), Result.bValid);
			for (int32 ArmIndex = 0; ArmIndex < Result.Arms.Num(); ++ArmIndex)
			{
				const FJunctionArmResult& Arm = Result.Arms[ArmIndex];
				TestTrue(*(Label + TEXT(" cut non-negative")), Arm.CutDistance >= -1e-9);
				TestTrue(*(Label + TEXT(" cut finite")), FMath::IsFinite(Arm.CutDistance));

				// The cut line must stay exactly the arm's full width.
				TestTrue(*(Label + TEXT(" cut spans full width")),
					FMath::IsNearlyEqual((Arm.LeftCut - Arm.RightCut).Length(), W * 2.0, 1e-6));

				// THE INVARIANT the boundary walk depends on: the cut sits at or
				// beyond both tangent points on this arm's own edges.
				const int32 PrevIndex = (ArmIndex + Result.Corners.Num() - 1) % Result.Corners.Num();
				if (!Result.Corners[ArmIndex].bStraightThrough)
				{
					TestTrue(*(Label + TEXT(" cut >= left tangent")),
						Arm.CutDistance >= Result.Corners[ArmIndex].ParamA - 1e-6);
				}
				if (!Result.Corners[PrevIndex].bStraightThrough)
				{
					TestTrue(*(Label + TEXT(" cut >= right tangent")),
						Arm.CutDistance >= Result.Corners[PrevIndex].ParamB - 1e-6);
				}
			}
		}
	}

	// --- Position offset must translate the whole solve, nothing else ---
	{
		const FVector2D Offset(123456.0, -98765.0);

		FJunctionInput AtOrigin;
		AtOrigin.Position = FVector2D::ZeroVector;
		FJunctionInput Moved;
		Moved.Position = Offset;

		for (int32 Index = 0; Index < 3; ++Index)
		{
			const double Bearing = UE_DOUBLE_PI * 0.6 * Index;
			AtOrigin.Arms.Add(MakeArmAtBearing(Bearing, W, 1500.0));
			Moved.Arms.Add(MakeArmAtBearing(Bearing, W, 1500.0));
		}

		const FJunctionResult A = FJunctionSolver::SolveCuts(AtOrigin);
		const FJunctionResult B = FJunctionSolver::SolveCuts(Moved);

		TestTrue(TEXT("both solve"), A.bValid && B.bValid);
		for (int32 Index = 0; Index < A.Arms.Num(); ++Index)
		{
			TestTrue(TEXT("cut distance is translation invariant"),
				FMath::IsNearlyEqual(A.Arms[Index].CutDistance, B.Arms[Index].CutDistance, 1e-6));
			TestTrue(TEXT("left cut translates"),
				(A.Arms[Index].LeftCut + Offset).Equals(B.Arms[Index].LeftCut, 1e-6));
			TestTrue(TEXT("right cut translates"),
				(A.Arms[Index].RightCut + Offset).Equals(B.Arms[Index].RightCut, 1e-6));
		}
	}

	// --- Zero arms is not a solve ---
	{
		FJunctionInput Empty;
		const FJunctionResult Result = FJunctionSolver::SolveCuts(Empty);
		TestFalse(TEXT("empty junction is invalid"), Result.bValid);
		TestEqual(TEXT("empty junction has no arms"), Result.Arms.Num(), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
