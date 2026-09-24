#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/UTurnGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUTurnBalloonTest, "Airside.Solve.UTurnBalloon",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUTurnBalloonTest::RunTest(const FString& Parameters)
{
	// The two lane ends of a Narrow road at a dead end facing +Y, and the 8.5 m dispenser's
	// lock radius (699 uu, the figure URoadProfile::JunctionScalingMargin's comment quotes).
	const FVector2D In(-150.0, 0.0);
	const FVector2D Out(150.0, 0.0);
	const FVector2D Axis(0.0, 1.0);
	const double Needed = 699.0;

	double Radius = 0.0;
	const TArray<UTurnGeom::FPiece> Pieces = UTurnGeom::Balloon(In, Out, Axis, Needed, &Radius);
	if (!TestEqual(TEXT("six quadratics: reverse curve out, half circle, reverse curve back"), Pieces.Num(), 6))
	{
		return false;
	}
	TestTrue(TEXT("it ends exactly on the leaving lane, so the handle joins"), Pieces.Last().End == Out);

	FVector2D Prev = In;
	for (int32 Index = 0; Index < Pieces.Num(); ++Index)
	{
		const UTurnGeom::FPiece& Piece = Pieces[Index];
		TestTrue(FString::Printf(TEXT("piece %d is followable by the vehicle it was sized for"), Index),
			GuidelineGeom::TightestRadius(Prev, Piece.Control, Piece.End) >= Needed);
		if (Index + 1 < Pieces.Num())
		{
			const FVector2D Arriving = (Piece.End - Piece.Control).GetSafeNormal();
			const FVector2D Leaving = (Pieces[Index + 1].Control - Piece.End).GetSafeNormal();
			TestTrue(FString::Printf(TEXT("no heading kink at joint %d"), Index),
				FVector2D::DotProduct(Arriving, Leaving) > 0.9999);
		}
		Prev = Piece.End;
	}

	TestTrue(TEXT("it leaves the arriving lane straight on, along the axis"),
		FVector2D::DotProduct((Pieces[0].Control - In).GetSafeNormal(), Axis) > 0.9999);
	TestTrue(TEXT("and joins the leaving lane heading back down the road"),
		FVector2D::DotProduct((Out - Pieces.Last().Control).GetSafeNormal(), -Axis) > 0.9999);

	// Measured by the Python prototype, 2026-09-23: R = 1091 at 699 needed.
	TestTrue(TEXT("the radius the search settled on is the prototype's"), Radius > 1000.0 && Radius < 1200.0);

	TestEqual(TEXT("coincident ends give no balloon"), UTurnGeom::Balloon(In, In, Axis, Needed).Num(), 0);
	TestEqual(TEXT("a zero axis gives no balloon"), UTurnGeom::Balloon(In, Out, FVector2D::ZeroVector, Needed).Num(), 0);
	return true;
}

#endif
