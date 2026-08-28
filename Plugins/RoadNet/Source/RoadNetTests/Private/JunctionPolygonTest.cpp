#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/JunctionSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FJunctionArm MakePolyArm(double Bearing, double HalfWidth, double Radius)
	{
		FJunctionArm Arm;
		Arm.Tangent = FVector2D(FMath::Cos(Bearing), FMath::Sin(Bearing));
		Arm.HalfWidthLeft = HalfWidth;
		Arm.HalfWidthRight = HalfWidth;
		Arm.FilletRadius = Radius;
		return Arm;
	}

	FJunctionResult SolveFull(const FJunctionInput& Input)
	{
		FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		FJunctionSolver::SolveBoundary(Input, Result);
		return Result;
	}

	/** The rim is the boundary minus the fan centre appended at the end. */
	TArray<FVector2D> RimOf(const FJunctionResult& Result)
	{
		TArray<FVector2D> Rim = Result.Boundary;
		if (Rim.Num() > 0)
		{
			Rim.Pop();
		}
		return Rim;
	}

	/**
	 * Every emitted triangle must wind counter-clockwise.
	 *
	 * PolygonArea(Rim) > 0 provably CANNOT detect an inverted fan. The shoelace sum over
	 * the rim and the sum of the fan's per-triangle signed areas are the same expression,
	 * so a fan in which some triangles wind clockwise still totals a positive area: the
	 * negative contributions are exactly the ones the shoelace already cancels. Only the
	 * per-triangle sign catches it, which is why this is checked here and not via area.
	 */
	bool AllTrianglesCCW(const FJunctionResult& Result)
	{
		for (int32 Slot = 0; Slot + 2 < Result.Triangles.Num(); Slot += 3)
		{
			const FVector2D& A = Result.Boundary[Result.Triangles[Slot]];
			const FVector2D& B = Result.Boundary[Result.Triangles[Slot + 1]];
			const FVector2D& C = Result.Boundary[Result.Triangles[Slot + 2]];

			const FVector2D Edge1 = B - A;
			const FVector2D Edge2 = C - A;
			if (Edge1.X * Edge2.Y - Edge1.Y * Edge2.X <= 0.0)
			{
				return false;
			}
		}
		return true;
	}

	bool ContainsExactly(const TArray<FVector2D>& Points, const FVector2D& Target)
	{
		for (const FVector2D& Point : Points)
		{
			// Bitwise, not Equals(). If this ever needs a tolerance the shared-truth
			// contract has been broken somewhere and the seams are back.
			if (Point.X == Target.X && Point.Y == Target.Y)
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadJunctionPolygonTest,
	"RoadNet.Solve.JunctionPolygon",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadJunctionPolygonTest::RunTest(const FString& Parameters)
{
	constexpr double W = 1150.0;
	constexpr double R = 1500.0;

	// The gallery: every configuration the solver must survive.
	const TArray<TArray<double>> Gallery = {
		{ 0.0, UE_DOUBLE_PI * (15.0 / 180.0) },                                     // 2-way, 15 deg
		{ 0.0, UE_DOUBLE_PI * 0.25 },                                               // 2-way, 45 deg
		{ 0.0, UE_DOUBLE_PI * 0.5 },                                                // 2-way, 90 deg
		{ 0.0, UE_DOUBLE_PI * (170.0 / 180.0) },                                    // 2-way, 170 deg
		{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI },                                  // 3-way T
		{ 0.0, UE_DOUBLE_PI * 0.6667, UE_DOUBLE_PI * 1.3333 },                      // 3-way Y
		{ 0.0, UE_DOUBLE_PI * 0.5, UE_DOUBLE_PI, UE_DOUBLE_PI * 1.5 },              // 4-way
		{ 0.0, UE_DOUBLE_PI * 0.4, UE_DOUBLE_PI * 0.8,
		  UE_DOUBLE_PI * 1.2, UE_DOUBLE_PI * 1.6 }                                  // 5-way
	};

	for (int32 CaseIndex = 0; CaseIndex < Gallery.Num(); ++CaseIndex)
	{
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.ArcSegments = 8;
		for (const double Bearing : Gallery[CaseIndex])
		{
			Input.Arms.Add(MakePolyArm(Bearing, W, R));
		}

		const FJunctionResult Result = SolveFull(Input);
		const FString Label = FString::Printf(TEXT("case %d"), CaseIndex);

		TestTrue(*(Label + TEXT(" solves")), Result.bValid);
		TestTrue(*(Label + TEXT(" has a boundary")), Result.Boundary.Num() >= 4);

		const TArray<FVector2D> Rim = RimOf(Result);

		TestTrue(*(Label + TEXT(" winding is CCW")), RoadGeom::PolygonArea(Rim) > 0.0);
		TestTrue(*(Label + TEXT(" polygon is simple")), RoadGeom::IsSimplePolygon(Rim));

		// Triangle indices are well formed and all reference the fan centre.
		TestEqual(*(Label + TEXT(" triangle count is a multiple of 3")),
			Result.Triangles.Num() % 3, 0);
		TestTrue(*(Label + TEXT(" triangles were emitted")), Result.Triangles.Num() > 0);

		// The fan apex must see the whole rim. A 2-way node whose arms are less than
		// ~28 degrees apart puts its own node OUTSIDE its boundary lobe, and the fan
		// then folds back on itself. Area cannot see that; per-triangle winding can.
		TestTrue(*(Label + TEXT(" every triangle winds CCW")), AllTrianglesCCW(Result));

		for (int32 Slot = 0; Slot < Result.Triangles.Num(); ++Slot)
		{
			const int32 VertexIndex = Result.Triangles[Slot];
			TestTrue(*(Label + TEXT(" index in range")),
				VertexIndex >= 0 && VertexIndex < Result.Boundary.Num());
			if (Slot % 3 == 0)
			{
				TestEqual(*(Label + TEXT(" fan apex is the centre")),
					VertexIndex, Result.Boundary.Num() - 1);
			}
		}

		// THE CONTRACT: every arm's cut vertices appear in the boundary bit-for-bit.
		// This is the whole reason Slice 1 exists. If it fails, the segment mesh and
		// the junction mesh will disagree at the seam and the cracks come back.
		for (int32 ArmIndex = 0; ArmIndex < Result.Arms.Num(); ++ArmIndex)
		{
			TestTrue(*(Label + TEXT(" left cut shared exactly")),
				ContainsExactly(Result.Boundary, Result.Arms[ArmIndex].LeftCut));
			TestTrue(*(Label + TEXT(" right cut shared exactly")),
				ContainsExactly(Result.Boundary, Result.Arms[ArmIndex].RightCut));
		}

		// No arm's cut may sit inside another arm's ribbon, or the two overlap.
		for (int32 ArmIndex = 0; ArmIndex < Result.Arms.Num(); ++ArmIndex)
		{
			const FVector2D CutCentre =
				Input.Arms[ArmIndex].Tangent * Result.Arms[ArmIndex].CutDistance;
			for (int32 Other = 0; Other < Result.Arms.Num(); ++Other)
			{
				if (Other == ArmIndex) { continue; }
				const double Lateral = FMath::Abs(FVector2D::DotProduct(
					CutCentre, RoadGeom::PerpCCW(Input.Arms[Other].Tangent)));
				const double Longitudinal =
					FVector2D::DotProduct(CutCentre, Input.Arms[Other].Tangent);
				const bool bOverlaps =
					Lateral < W - 1e-6 &&
					Longitudinal > Result.Arms[Other].CutDistance + 1e-6;
				TestFalse(*(Label + TEXT(" arms do not overlap")), bOverlaps);
			}
		}

		// Every rim vertex must be finite; one NaN poisons the whole mesh silently.
		for (const FVector2D& Point : Rim)
		{
			TestTrue(*(Label + TEXT(" rim vertex finite")),
				FMath::IsFinite(Point.X) && FMath::IsFinite(Point.Y));
		}
	}

	// --- A collinear node must produce a clean rectangle, with no faceting ---
	{
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.ArcSegments = 8;
		Input.Arms.Add(MakePolyArm(0.0, W, R));
		Input.Arms.Add(MakePolyArm(UE_DOUBLE_PI, W, R));

		const FJunctionResult Result = SolveFull(Input);
		TestTrue(TEXT("collinear solves"), Result.bValid);

		const TArray<FVector2D> Rim = RimOf(Result);

		// Both cuts are at zero and both corners are straight-through, so the two
		// cut lines coincide: the welded rim collapses to a single degenerate edge.
		// What matters is that no arc was emitted and nothing was trimmed.
		TestTrue(TEXT("collinear trims nothing"),
			FMath::IsNearlyEqual(Result.Arms[0].CutDistance, 0.0, 1e-9));
		TestTrue(TEXT("collinear emits no arc points"), Rim.Num() <= 4);

		// The ring close may drop an arc sample, never a cut vertex. Here arm 1's cut
		// line coincides with arm 0's to within the weld tolerance, so the old
		// unconditional Pop() discarded arm 1's LeftCut and a mesh builder searching the
		// rim for that segment end would not have found it.
		TestEqual(TEXT("collinear rim keeps its last cut vertex"), Rim.Num(), 3);
		TestTrue(TEXT("collinear keeps arm 1's left cut bitwise"),
			ContainsExactly(Rim, Result.Arms[1].LeftCut));
		TestTrue(TEXT("collinear keeps arm 1's right cut bitwise"),
			ContainsExactly(Rim, Result.Arms[1].RightCut));
		TestTrue(TEXT("collinear keeps arm 0's right cut bitwise"),
			ContainsExactly(Rim, Result.Arms[0].RightCut));
	}

	// --- Dead end: boundary exists, but there is no fan to build ---
	{
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.Arms.Add(MakePolyArm(0.0, W, R));

		const FJunctionResult Result = SolveFull(Input);
		TestTrue(TEXT("dead end solves"), Result.bValid);
		TestTrue(TEXT("dead end has cut vertices"),
			ContainsExactly(Result.Boundary, Result.Arms[0].LeftCut) &&
			ContainsExactly(Result.Boundary, Result.Arms[0].RightCut));
		TestEqual(TEXT("dead end emits no triangles"), Result.Triangles.Num(), 0);
	}

	// --- Continuity: sweeping one arm's bearing must not make the boundary flip ---
	//
	// Raw area is NOT a bounded-derivative function here: the fillet reach is
	// |R/tan(Theta/2)|, which genuinely diverges as the fork closes. At 2 degrees the
	// trim is ~86 km and the junction spans 6.8 km^2. That is correct geometry, not a
	// defect, so a flat "area changed less than X%" bound is the wrong instrument.
	//
	// Monotonicity is the right one. The reach falls strictly as Theta opens toward PI
	// and rises again beyond it, so the area must do the same. An inside-out flip or a
	// popped vertex breaks that ordering even where the absolute change is small.
	{
		double PreviousArea = -1.0;
		for (int32 Step = 1; Step < 180; ++Step)
		{
			const double Bearing = 2.0 * UE_DOUBLE_PI * static_cast<double>(Step) / 360.0;

			FJunctionInput Input;
			Input.Position = FVector2D::ZeroVector;
			Input.ArcSegments = 8;
			Input.Arms.Add(MakePolyArm(0.0, W, R));
			Input.Arms.Add(MakePolyArm(Bearing, W, R));

			const FJunctionResult Result = SolveFull(Input);
			TestTrue(TEXT("acute sweep solves"), Result.bValid);

			const TArray<FVector2D> Rim = RimOf(Result);
			const double Area = RoadGeom::PolygonArea(Rim);

			TestTrue(TEXT("acute sweep winding stays CCW"), Area > 0.0);
			TestTrue(TEXT("acute sweep stays simple"), RoadGeom::IsSimplePolygon(Rim));

			// This is the assertion that would have caught the inverted fan: below
			// ~28 degrees the node lies outside its own rim, so the apex must move to
			// the rim centroid or no triangles may be emitted at all.
			TestTrue(TEXT("acute sweep emits triangles"), Result.Triangles.Num() > 0);
			TestTrue(TEXT("acute sweep every triangle winds CCW"), AllTrianglesCCW(Result));

			if (PreviousArea > 0.0)
			{
				TestTrue(TEXT("area falls monotonically as the fork opens"),
					Area <= PreviousArea * (1.0 + 1e-6));
			}
			PreviousArea = Area;
		}
	}

	// Beyond PI the reach rises again, so the area must rise with it.
	{
		double PreviousArea = -1.0;
		for (int32 Step = 181; Step < 360; ++Step)
		{
			const double Bearing = 2.0 * UE_DOUBLE_PI * static_cast<double>(Step) / 360.0;

			FJunctionInput Input;
			Input.Position = FVector2D::ZeroVector;
			Input.ArcSegments = 8;
			Input.Arms.Add(MakePolyArm(0.0, W, R));
			Input.Arms.Add(MakePolyArm(Bearing, W, R));

			const FJunctionResult Result = SolveFull(Input);
			TestTrue(TEXT("reflex sweep solves"), Result.bValid);

			const TArray<FVector2D> Rim = RimOf(Result);
			const double Area = RoadGeom::PolygonArea(Rim);

			TestTrue(TEXT("reflex sweep winding stays CCW"), Area > 0.0);
			TestTrue(TEXT("reflex sweep stays simple"), RoadGeom::IsSimplePolygon(Rim));

			// The mirror of the acute case: past ~332 degrees the lobe closes up again.
			TestTrue(TEXT("reflex sweep emits triangles"), Result.Triangles.Num() > 0);
			TestTrue(TEXT("reflex sweep every triangle winds CCW"), AllTrianglesCCW(Result));

			if (PreviousArea > 0.0)
			{
				TestTrue(TEXT("area rises monotonically past PI"),
					Area >= PreviousArea * (1.0 - 1e-6));
			}
			PreviousArea = Area;
		}
	}

	// In the well-conditioned band the boundary really should move smoothly, so the
	// tight bound still applies where the reach is not diverging.
	{
		double PreviousArea = -1.0;
		for (int32 Step = 20; Step <= 160; ++Step)
		{
			const double Bearing = 2.0 * UE_DOUBLE_PI * static_cast<double>(Step) / 360.0;

			FJunctionInput Input;
			Input.Position = FVector2D::ZeroVector;
			Input.ArcSegments = 8;
			Input.Arms.Add(MakePolyArm(0.0, W, R));
			Input.Arms.Add(MakePolyArm(Bearing, W, R));

			const FJunctionResult Result = SolveFull(Input);
			const double Area = RoadGeom::PolygonArea(RimOf(Result));

			if (PreviousArea > 0.0)
			{
				const double Change = FMath::Abs(Area - PreviousArea) / PreviousArea;
				TestTrue(TEXT("well-conditioned band moves smoothly"), Change < 0.10);
			}
			PreviousArea = Area;
		}
	}

	// --- Translation invariance: the solve must not depend on world position ---
	{
		const FVector2D Offset(500000.0, -250000.0);

		FJunctionInput AtOrigin;
		FJunctionInput Moved;
		AtOrigin.Position = FVector2D::ZeroVector;
		Moved.Position = Offset;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const double Bearing = UE_DOUBLE_PI * 0.5 * Index;
			AtOrigin.Arms.Add(MakePolyArm(Bearing, W, R));
			Moved.Arms.Add(MakePolyArm(Bearing, W, R));
		}

		const FJunctionResult A = SolveFull(AtOrigin);
		const FJunctionResult B = SolveFull(Moved);

		TestEqual(TEXT("same vertex count when translated"),
			A.Boundary.Num(), B.Boundary.Num());
		for (int32 Index = 0; Index < A.Boundary.Num(); ++Index)
		{
			TestTrue(TEXT("boundary translates rigidly"),
				(A.Boundary[Index] + Offset).Equals(B.Boundary[Index], 1e-6));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
