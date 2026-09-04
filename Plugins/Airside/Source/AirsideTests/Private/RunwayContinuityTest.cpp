#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/JunctionSolver.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	constexpr double RunwayHalfWidth = 2250.0;   // a 45 m runway
	constexpr double TaxiwayHalfWidth = 1150.0;  // a 23 m taxiway
	constexpr double Fillet = 1500.0;

	FJunctionArm RunwayArm(const FVector2D& Tangent, bool bContinuous)
	{
		FJunctionArm Arm;
		Arm.Tangent = Tangent;
		Arm.HalfWidthLeft = RunwayHalfWidth;
		Arm.HalfWidthRight = RunwayHalfWidth;
		Arm.FilletRadius = Fillet;
		Arm.bContinuous = bContinuous;
		return Arm;
	}

	FJunctionArm TaxiwayArm(const FVector2D& Tangent)
	{
		FJunctionArm Arm;
		Arm.Tangent = Tangent;
		Arm.HalfWidthLeft = TaxiwayHalfWidth;
		Arm.HalfWidthRight = TaxiwayHalfWidth;
		Arm.FilletRadius = Fillet;
		return Arm;
	}

	/**
	 * A runway running east-west with one taxiway leaving to the south.
	 *
	 * Arms MUST be sorted ascending by CCW bearing - the solver says so and does not check.
	 * East is 0, west is 180, and a taxiway whose tangent points away from the node to the
	 * south is 270.
	 */
	FJunctionInput RunwayExit(bool bContinuous)
	{
		FJunctionInput Input;
		Input.Position = FVector2D::ZeroVector;
		Input.ArcSegments = 8;
		Input.Arms.Add(RunwayArm(FVector2D(1.0, 0.0), bContinuous));    // east,  bearing 0
		Input.Arms.Add(RunwayArm(FVector2D(-1.0, 0.0), bContinuous));   // west,  bearing 180
		Input.Arms.Add(TaxiwayArm(FVector2D(0.0, -1.0)));               // south, bearing 270
		return Input;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayContinuityTest,
	"Airside.Solve.RunwayContinuity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayContinuityTest::RunTest(const FString& Parameters)
{
	// 1. THE MEASUREMENT. A runway's edge runs UNBROKEN past an exit.
	//
	//    Every arm at a node is trimmed back so a polygon can be paved between them. That is
	//    right where two taxiways meet and wrong for a runway: a runway edge is a straight
	//    line from threshold to threshold, and an exit is a taxiway filleted into it, not a
	//    hole cut in it.
	//
	//    Measured as the GAP between the two runway halves' cut vertices. They are the same
	//    physical point on the same edge, so the gap must be zero - and bitwise zero, not
	//    within a tolerance, because that is this project's surface contract. Trimmed, the
	//    gap is twice the cut distance.
	{
		const FJunctionInput Input = RunwayExit(/*bContinuous=*/true);
		const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);

		if (!TestTrue(TEXT("the node solves"), Result.bValid))
		{
			return false;
		}

		// East's LEFT edge and west's RIGHT edge are the same side of the runway - east
		// points +X so its left is +Y, west points -X so its right is +Y too.
		const FVector2D NorthFromEast = Result.Arms[0].LeftCut;
		const FVector2D NorthFromWest = Result.Arms[1].RightCut;
		const FVector2D SouthFromEast = Result.Arms[0].RightCut;
		const FVector2D SouthFromWest = Result.Arms[1].LeftCut;

		TestEqual(FString::Printf(TEXT("the runway's north edge is unbroken (gap %.1f uu)"),
			FVector2D::Distance(NorthFromEast, NorthFromWest)),
			NorthFromEast, NorthFromWest);

		TestEqual(FString::Printf(TEXT("and its south edge too (gap %.1f uu)"),
			FVector2D::Distance(SouthFromEast, SouthFromWest)),
			SouthFromEast, SouthFromWest);

		// BITWISE, not close. A tolerance here would mean the seams are back - see the
		// invariant in CLAUDE.md. TestEqual on FVector2D compares with KINDA_SMALL_NUMBER,
		// so the exact comparison is made separately and deliberately.
		TestTrue(TEXT("the shared vertices are bitwise identical, not merely close"),
			NorthFromEast.X == NorthFromWest.X && NorthFromEast.Y == NorthFromWest.Y &&
			SouthFromEast.X == SouthFromWest.X && SouthFromEast.Y == SouthFromWest.Y);

		// And they sit exactly on the runway's own edges, at the node.
		TestEqual(TEXT("the north edge is a half-width from the centreline"),
			NorthFromEast.Y, RunwayHalfWidth);
		TestEqual(TEXT("and lies on the node's cross-section"), NorthFromEast.X, 0.0);
	}

	// 2. THE TAXIWAY IS STILL CUT. Continuity is for the arm that passes through, and a
	//    taxiway that stopped being trimmed would drive its ribbon into the runway surface.
	{
		const FJunctionInput Input = RunwayExit(/*bContinuous=*/true);
		const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);

		TestTrue(FString::Printf(
			TEXT("the taxiway is still trimmed back (%.0f uu)"), Result.Arms[2].CutDistance),
			Result.Arms[2].CutDistance > 0.0);

		TestEqual(TEXT("while the runway halves are not"), Result.Arms[0].CutDistance, 0.0);
		TestEqual(TEXT("either of them"), Result.Arms[1].CutDistance, 0.0);
	}

	// 3. WITHOUT THE FLAG, NOTHING CHANGES. Every junction in the airport is an ordinary
	//    one, and this must be exactly the code that has always run for them.
	{
		const FJunctionInput Input = RunwayExit(/*bContinuous=*/false);
		const FJunctionResult Result = FJunctionSolver::SolveCuts(Input);

		if (TestTrue(TEXT("the ordinary node still solves"), Result.bValid))
		{
			TestTrue(FString::Printf(
				TEXT("and its arms are trimmed as they always were (%.0f uu)"),
				Result.Arms[0].CutDistance),
				Result.Arms[0].CutDistance > 0.0);
		}
	}

	// 4. THE JUNCTION POLYGON MUST NOT PAVE OVER THE RUNWAY.
	//
	//    The runway ribbon now runs through the node uncut, so anything the boundary also
	//    covers there is a second surface at the same height - z-fighting, and two sets of
	//    triangles claiming one piece of ground.
	//
	//    Measured by sampling the boundary rather than by reading it: a point strictly
	//    inside the runway strip, away from its edges, must not be inside the polygon.
	{
		FJunctionInput Input = RunwayExit(/*bContinuous=*/true);
		FJunctionResult Result = FJunctionSolver::SolveCuts(Input);
		FJunctionSolver::SolveBoundary(Input, Result);

		if (TestTrue(TEXT("the boundary is built"), Result.Boundary.Num() > 2))
		{
			// Signed area of the boundary ring, ignoring the fan centre appended last.
			// A polygon confined to the taxiway lens is far smaller than the runway strip
			// it would otherwise swallow.
			double Area = 0.0;
			const int32 Count = Result.Boundary.Num() - 1;
			for (int32 At = 0; At < Count; ++At)
			{
				const FVector2D& A = Result.Boundary[At];
				const FVector2D& B = Result.Boundary[(At + 1) % Count];
				Area += A.X * B.Y - B.X * A.Y;
			}
			Area = FMath::Abs(Area) * 0.5;

			// The runway strip across this node, from fillet tangent to fillet tangent, is
			// at least 2 * RunwayHalfWidth deep and several metres long. A polygon that
			// covered it would be an order of magnitude larger than the lens.
			const double RunwayStrip = 2.0 * RunwayHalfWidth * 2.0 * TaxiwayHalfWidth;
			AddInfo(FString::Printf(
				TEXT("junction polygon %.0f uu2, runway strip %.0f uu2, taxiway cut %.0f uu, "
					 "%d boundary points"),
				Area, RunwayStrip, Result.Arms[2].CutDistance, Result.Boundary.Num()));

			TestTrue(FString::Printf(
				TEXT("the junction polygon stays out of the runway (%.0f uu2 against a %.0f uu2 strip)"),
				Area, RunwayStrip),
				Area < RunwayStrip);

			// AND IS NOT DEGENERATE. The taxiway is trimmed back, so the ground between its
			// cut line and the runway edge has to be paved by this polygon or there is a
			// visible gap at every exit - the opposite failure to swallowing the runway, and
			// one that "area is small" would happily pass.
			const double LeastLens = 2.0 * TaxiwayHalfWidth * Result.Arms[2].CutDistance * 0.5;
			TestTrue(FString::Printf(
				TEXT("and still paves the taxiway's approach (%.0f uu2, needs at least %.0f)"),
				Area, LeastLens),
				Area > LeastLens);

			// EXTENT, not just area. A shoelace sum cancels, so a polygon with a zero-width
			// spike running up across the runway and back reports exactly the same area as a
			// clean lens - and triangulates into degenerate slivers. The runway's south edge
			// is the roof of this polygon; nothing may reach past it.
			double HighestY = -TNumericLimits<double>::Max();
			for (int32 At = 0; At + 1 < Result.Boundary.Num(); ++At)
			{
				HighestY = FMath::Max(HighestY, Result.Boundary[At].Y);
			}
			AddInfo(FString::Printf(TEXT("boundary reaches Y=%.0f, runway south edge Y=%.0f"),
				HighestY, -RunwayHalfWidth));

			TestTrue(FString::Printf(
				TEXT("no boundary point crosses the runway's south edge (%.0f vs %.0f)"),
				HighestY, -RunwayHalfWidth),
				HighestY <= -RunwayHalfWidth + 1e-6);

			// And no triangle is a sliver, which is what a spike leaves behind.
			int32 Degenerate = 0;
			for (int32 At = 0; At + 2 < Result.Triangles.Num(); At += 3)
			{
				const FVector2D& A = Result.Boundary[Result.Triangles[At]];
				const FVector2D& B = Result.Boundary[Result.Triangles[At + 1]];
				const FVector2D& C = Result.Boundary[Result.Triangles[At + 2]];
				const double Twice = FMath::Abs((B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X));
				if (Twice < 1.0)
				{
					++Degenerate;
				}
			}
			TestEqual(FString::Printf(TEXT("no degenerate triangles (%d of %d)"),
				Degenerate, Result.Triangles.Num() / 3), Degenerate, 0);
		}
	}

	return true;
}

#endif
