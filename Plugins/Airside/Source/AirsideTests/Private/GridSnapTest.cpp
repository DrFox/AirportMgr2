#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/GridSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * ROUNDS TO NEAREST ON BOTH SIDES OF THE ORIGIN. An int cast truncates toward zero, and an
 * airport west or south of the origin would then sit one grid line out from the one drawn.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridSnapQuantiseTest,
	"Airside.Solve.GridSnap.Quantise",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridSnapQuantiseTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("1 m"), GridSnap::Quantise(FVector2D(149.0, 351.0), 100.0), FVector2D(100.0, 400.0));
	TestEqual(TEXT("5 m"), GridSnap::Quantise(FVector2D(1240.0, 1260.0), 500.0), FVector2D(1000.0, 1500.0));
	TestEqual(TEXT("10 m"), GridSnap::Quantise(FVector2D(1499.0, 1501.0), 1000.0), FVector2D(1000.0, 2000.0));
	TestEqual(TEXT("negative coordinates round to NEAREST, not toward zero"),
		GridSnap::Quantise(FVector2D(-149.0, -151.0), 100.0), FVector2D(-100.0, -200.0));

	// BITWISE, not nearly: grid off must be exactly today's behaviour.
	const FVector2D Odd(123.456789, -98.7654321);
	const FVector2D Off = GridSnap::Quantise(Odd, 0.0);
	TestTrue(TEXT("step 0 returns the point bitwise-unchanged"), Off.X == Odd.X && Off.Y == Odd.Y);
	return true;
}

/**
 * THE CROSSING ALONG A LINE: what a single guide, a kerb and a stand's inward edge all snap by.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridSnapCrossingAlongTest,
	"Airside.Solve.GridSnap.CrossingAlong",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridSnapCrossingAlongTest::RunTest(const FString& Parameters)
{
	FVector2D Out(7.0, 7.0);

	// A line along X at Y = 130 is parallel to the Y = k lines: only X = k crossings exist, and
	// the point stays ON the line (Y = 130), which Quantise would have broken.
	TestTrue(TEXT("an X-axis line has crossings"),
		GridSnap::NearestCrossingAlong(FVector2D(0.0, 130.0), FVector2D(1.0, 0.0), FVector2D(740.0, 999.0), 500.0, Out));
	TestTrue(TEXT("on the X = 500 line, still on the guide"),
		FMath::IsNearlyEqual(Out.X, 500.0, 1e-6) && FMath::IsNearlyEqual(Out.Y, 130.0, 1e-6));

	// 45 degrees from (30, 0): crosses X = 100 at t = 70*sqrt2 and Y = 100 at t = 100*sqrt2. The
	// foot of (110, 80) is t = 80*sqrt2, nearer the X = 100 crossing - both families are candidates.
	TestTrue(TEXT("a diagonal line has crossings"),
		GridSnap::NearestCrossingAlong(FVector2D(30.0, 0.0), FVector2D(1.0, 1.0), FVector2D(110.0, 80.0), 100.0, Out));
	TestTrue(TEXT("the nearer of the two families"),
		FMath::IsNearlyEqual(Out.X, 100.0, 1e-6) && FMath::IsNearlyEqual(Out.Y, 70.0, 1e-6));

	const FVector2D Before = Out;
	TestFalse(TEXT("a zero direction is refused"),
		GridSnap::NearestCrossingAlong(FVector2D::ZeroVector, FVector2D::ZeroVector, FVector2D(5.0, 5.0), 100.0, Out));
	TestFalse(TEXT("a zero step is refused"),
		GridSnap::NearestCrossingAlong(FVector2D::ZeroVector, FVector2D(1.0, 0.0), FVector2D(5.0, 5.0), 0.0, Out));
	TestEqual(TEXT("a refusal leaves Out untouched"), Out, Before);

	TestTrue(TEXT("negative side"),
		GridSnap::NearestCrossingAlong(FVector2D(0.0, 0.0), FVector2D(-1.0, 0.0), FVector2D(-260.0, 0.0), 100.0, Out));
	TestTrue(TEXT("-260 rounds to -300, not -200"), FMath::IsNearlyEqual(Out.X, -300.0, 1e-6));
	return true;
}

/** THE RANGE-LIMITED CROSSING: an anchor on its segment, a frontage at least its floor. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridSnapCrossingInRangeTest,
	"Airside.Solve.GridSnap.CrossingInRange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridSnapCrossingInRangeTest::RunTest(const FString& Parameters)
{
	FVector2D Out(7.0, 7.0);
	// Foot at 9900 on [0, 9700] at a 10 m step: the unrestricted nearest is 10000, past the end.
	TestTrue(TEXT("a crossing inside the range exists"),
		GridSnap::NearestCrossingInRange(FVector2D::ZeroVector, FVector2D(1.0, 0.0), FVector2D(9900.0, 50.0), 1000.0, 0.0, 9700.0, Out));
	TestTrue(TEXT("the last line on the range, not the nearer one past it"), FMath::IsNearlyEqual(Out.X, 9000.0, 1e-6));

	// Foot at 200 with a floor of 1500: the nearest crossing at least 1500 along.
	TestTrue(TEXT("a floor"), GridSnap::NearestCrossingInRange(FVector2D(30.0, 0.0), FVector2D(1.0, 0.0), FVector2D(230.0, 0.0), 500.0, 1500.0, 1e12, Out));
	TestTrue(TEXT("first line at or beyond the floor (X = 1530 -> line 2000)"), FMath::IsNearlyEqual(Out.X, 2000.0, 1e-6));

	const FVector2D Before = Out;
	TestFalse(TEXT("a range between two lines holds none"),
		GridSnap::NearestCrossingInRange(FVector2D(100.0, 0.0), FVector2D(1.0, 0.0), FVector2D(400.0, 0.0), 1000.0, 0.0, 800.0, Out));
	TestEqual(TEXT("refusal leaves Out untouched"), Out, Before);
	return true;
}

/**
 * THE OVERLAY'S PIECES: inside the disc, one cell long at most, majors every fifth line.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridSnapPiecesInDiscTest,
	"Airside.Solve.GridSnap.PiecesInDisc",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridSnapPiecesInDiscTest::RunTest(const FString& Parameters)
{
	TArray<GridSnap::FPiece> Pieces;
	const FVector2D Centre(1234.0, -567.0);
	const double Radius = 2000.0;
	const double Step = 100.0;
	GridSnap::PiecesInDisc(Centre, Radius, Step, Pieces);

	TestTrue(TEXT("a 20 m disc at 1 m has pieces"), Pieces.Num() > 0);

	bool bAllInside = true;
	bool bAllShort = true;
	bool bMajorsOnFives = true;
	int32 Majors = 0;
	for (const GridSnap::FPiece& Piece : Pieces)
	{
		bAllInside &= FVector2D::Distance(Piece.From, Centre) <= Radius + 1e-6
			&& FVector2D::Distance(Piece.To, Centre) <= Radius + 1e-6;
		bAllShort &= FVector2D::Distance(Piece.From, Piece.To) <= Step + 1e-6;

		// The line's own coordinate: X for a vertical piece, Y for a horizontal one.
		const bool bVertical = Piece.From.X == Piece.To.X;
		const double At = bVertical ? Piece.From.X : Piece.From.Y;
		const bool bOnFive = FMath::IsNearlyEqual(FMath::Fmod(FMath::Abs(At), 5.0 * Step), 0.0, 1e-6);
		bMajorsOnFives &= Piece.bMajor == bOnFive;
		Majors += Piece.bMajor ? 1 : 0;
	}
	TestTrue(TEXT("every piece is inside the disc"), bAllInside);
	TestTrue(TEXT("no piece is longer than one step - see PiecesInDisc on the HUD's culling"), bAllShort);
	TestTrue(TEXT("major exactly on multiples of five steps"), bMajorsOnFives);
	TestTrue(TEXT("and there are some"), Majors > 0);

	// A disc of radius exactly one step, centred on a line's neighbour, is tangent to the lines
	// a step either side: they touch at one point and draw nothing.
	GridSnap::PiecesInDisc(FVector2D(50.0, 50.0), 50.0, 100.0, Pieces);
	TestEqual(TEXT("tangent lines contribute nothing"), Pieces.Num(), 0);

	GridSnap::PiecesInDisc(Centre, Radius, 0.0, Pieces);
	TestEqual(TEXT("step 0 draws nothing"), Pieces.Num(), 0);
	return true;
}

#endif
