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

/**
 * GRID FRAMES (grid-follows-snap design section 1). A frame is a grid turned to a thing: one
 * line along the thing's centreline, the lines across it phased from the world origin.
 */
namespace GridFrameFixture
{
	bool SameBits(const FVector2D& A, const FVector2D& B)
	{
		return FMemory::Memcmp(&A, &B, sizeof(FVector2D)) == 0;
	}

	FVector2D Deg(double Degrees)
	{
		const double R = FMath::DegreesToRadians(Degrees);
		return FVector2D(FMath::Cos(R), FMath::Sin(R));
	}

	/** Q in Frame's own coordinates: along Axis, then along its left perpendicular. */
	FVector2D Local(const GridSnap::FGridFrame& Frame, const FVector2D& Q)
	{
		const FVector2D Perp(-Frame.Axis.Y, Frame.Axis.X);
		return FVector2D(FVector2D::DotProduct(Q - Frame.Origin, Frame.Axis), FVector2D::DotProduct(Q - Frame.Origin, Perp));
	}

	bool OnLine(double Value, double Step, double Tolerance = 1e-6)
	{
		return FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value / Step) * Step, Tolerance);
	}
}

/**
 * THE WORLD FRAME IS TODAY, BITWISE. Every figure the pre-frame functions gave on their own test
 * inputs, the frame overloads give with World(step) - compared as bits, not with a tolerance,
 * because World is what a player with Follow off gets and it promised no change at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFrameWorldIsTodayTest,
	"Airside.Solve.GridSnap.WorldFrameIsBitwiseToday",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFrameWorldIsTodayTest::RunTest(const FString& Parameters)
{
	using namespace GridFrameFixture;
	const FVector2D Points[] = { FVector2D(149.0, 351.0), FVector2D(-149.0, -151.0), FVector2D(-0.3, 0.2), FVector2D(1499.0, 1501.0) };
	for (const double Step : { 100.0, 500.0, 1000.0 })
	{
		const GridSnap::FGridFrame World = GridSnap::FGridFrame::World(Step);
		for (const FVector2D& P : Points)
		{
			TestTrue(*FString::Printf(TEXT("Quantise %s at %.0f"), *P.ToString(), Step),
				SameBits(GridSnap::Quantise(P, World), GridSnap::Quantise(P, Step)));

			FVector2D A(-1.0, -1.0), B(-1.0, -1.0);
			const bool bA = GridSnap::NearestCrossingAlong(FVector2D(30.0, 0.0), FVector2D(1.0, 1.0), P, World, A);
			const bool bB = GridSnap::NearestCrossingAlong(FVector2D(30.0, 0.0), FVector2D(1.0, 1.0), P, Step, B);
			TestTrue(TEXT("CrossingAlong agrees"), bA == bB && SameBits(A, B));

			FVector2D C(-1.0, -1.0), D(-1.0, -1.0);
			const bool bC = GridSnap::NearestCrossingInRange(FVector2D(100.0, 0.0), FVector2D(1.0, 0.0), P, World, 0.0, 800.0, C);
			const bool bD = GridSnap::NearestCrossingInRange(FVector2D(100.0, 0.0), FVector2D(1.0, 0.0), P, Step, 0.0, 800.0, D);
			TestTrue(TEXT("CrossingInRange agrees"), bC == bD && SameBits(C, D));
		}

		TArray<GridSnap::FPiece> Framed, Plain;
		GridSnap::PiecesInDisc(FVector2D(1234.0, -567.0), 2000.0, World, Framed);
		GridSnap::PiecesInDisc(FVector2D(1234.0, -567.0), 2000.0, Step, Plain);
		bool bSame = Framed.Num() == Plain.Num();
		for (int32 I = 0; bSame && I < Plain.Num(); ++I)
		{
			bSame = SameBits(Framed[I].From, Plain[I].From) && SameBits(Framed[I].To, Plain[I].To) && Framed[I].bMajor == Plain[I].bMajor;
		}
		TestTrue(*FString::Printf(TEXT("PiecesInDisc agrees at %.0f"), Step), bSame);
	}
	TestTrue(TEXT("World(step) is world-aligned"), GridSnap::FGridFrame::World(500.0).IsWorldAligned());
	return true;
}

/**
 * A SQUARE GRID IS THE SAME AFTER A QUARTER TURN, so a road drawn A->B and one drawn B->A, or
 * one at 30 and one at 120 degrees through the same point, get ONE grid, not four.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFrameAlongFoldsTest,
	"Airside.Solve.GridSnap.AlongFolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFrameAlongFoldsTest::RunTest(const FString& Parameters)
{
	using namespace GridFrameFixture;
	const FVector2D P(1234.0, 777.0);
	const GridSnap::FGridFrame Forward = GridSnap::FGridFrame::Along(P, Deg(30.0), 500.0);
	TestTrue(TEXT("reversed direction, same grid"), Forward.SameLines(GridSnap::FGridFrame::Along(P, -Deg(30.0), 500.0)));
	// SAME AXIS, NOT SAME LINES: the 120-degree line through P is a different line, so its grid is
	// laid along IT - the fold only promises the two are not turned against each other.
	TestTrue(TEXT("a quarter turn, same axis"), Forward.Axis == GridSnap::FGridFrame::Along(P, FVector2D(-Deg(30.0).Y, Deg(30.0).X), 500.0).Axis);
	TestTrue(TEXT("a line along the world X axis through y = 0 IS the world grid"),
		GridSnap::FGridFrame::Along(FVector2D(-20000.0, 0.0), FVector2D(-1.0, 0.0), 500.0).IsWorldAligned());
	TestTrue(TEXT("axis folded into [0, 90)"), FMath::IsNearlyEqual(Forward.AxisDegrees(), 30.0, 1e-9));
	TestTrue(TEXT("a zero direction is the world grid"), GridSnap::FGridFrame::Along(P, FVector2D::ZeroVector, 500.0).IsWorldAligned());
	TestEqual(TEXT("the step travels"), Forward.StepUu, 500.0);
	return true;
}

/**
 * ONE LINE ALONG THE THING, CROSS LINES FROM THE WORLD ORIGIN - design option 1. A point just off
 * the road quantises onto the road's own line; the world origin sits on a cross line.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFrameLineOnTheThingTest,
	"Airside.Solve.GridSnap.AlongPutsALineOnTheThing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFrameLineOnTheThingTest::RunTest(const FString& Parameters)
{
	using namespace GridFrameFixture;
	const FVector2D P(1234.0, 777.0);
	const FVector2D Dir = Deg(30.0);
	const GridSnap::FGridFrame Frame = GridSnap::FGridFrame::Along(P, Dir, 500.0);

	const FVector2D NearRoad = P + Dir * 1111.0 + FVector2D(-Dir.Y, Dir.X) * 40.0;
	const FVector2D Snapped = GridSnap::Quantise(NearRoad, Frame);
	const FVector2D ToRoad = Snapped - P;
	TestTrue(TEXT("40 uu off the road quantises onto the road's line"),
		FMath::Abs(ToRoad.X * Dir.Y - ToRoad.Y * Dir.X) < 1e-6);
	TestTrue(TEXT("and onto a cross line, phased from the world origin"),
		OnLine(FVector2D::DotProduct(Snapped, Dir), 500.0));
	TestTrue(TEXT("the result is a grid point of the frame"),
		OnLine(Local(Frame, Snapped).X, 500.0) && OnLine(Local(Frame, Snapped).Y, 500.0));

	FVector2D Out(0.0, 0.0);
	TestTrue(TEXT("a crossing along the road exists"), GridSnap::NearestCrossingAlong(P, Dir, P + Dir * 740.0, Frame, Out));
	TestTrue(TEXT("along the road, only the cross family - one step apart, world-phased"),
		OnLine(FVector2D::DotProduct(Out, Dir), 500.0) && FMath::Abs((Out - P).X * Dir.Y - (Out - P).Y * Dir.X) < 1e-6);
	return true;
}

/**
 * WHAT THE WORLD GRID FIXED, KEPT: two segments of one road share their cross lines (the
 * 2026-09-27 stand report), and two parallel roads a whole step apart share both families.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFrameSharedLinesTest,
	"Airside.Solve.GridSnap.CollinearAndParallelShareLines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFrameSharedLinesTest::RunTest(const FString& Parameters)
{
	using namespace GridFrameFixture;
	const FVector2D A(1234.0, 777.0);
	const FVector2D Dir = Deg(30.0);
	const FVector2D Perp(-Dir.Y, Dir.X);
	const GridSnap::FGridFrame First = GridSnap::FGridFrame::Along(A, Dir, 500.0);
	const GridSnap::FGridFrame Collinear = GridSnap::FGridFrame::Along(A + Dir * 777.0, Dir, 500.0);
	const GridSnap::FGridFrame Parallel = GridSnap::FGridFrame::Along(A + Perp * 500.0, Dir, 500.0);
	const GridSnap::FGridFrame Offset = GridSnap::FGridFrame::Along(A + Perp * 370.0, Dir, 500.0);

	const FVector2D Probe(3210.0, 4567.0);
	const FVector2D Q = GridSnap::Quantise(Probe, First);
	TestTrue(TEXT("collinear segment: same grid point"), FVector2D::Distance(Q, GridSnap::Quantise(Probe, Collinear)) < 1e-6);
	TestTrue(TEXT("parallel road one step over: same grid point"), FVector2D::Distance(Q, GridSnap::Quantise(Probe, Parallel)) < 1e-6);
	TestTrue(TEXT("control: 3.7 m over is a different grid"), FVector2D::Distance(Q, GridSnap::Quantise(Probe, Offset)) > 1.0);
	return true;
}

/** THE OVERLAY TURNS WITH THE FRAME: every piece along or across the axis, inside the disc. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGridFrameRotatedPiecesTest,
	"Airside.Solve.GridSnap.RotatedPiecesFollowAxis",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGridFrameRotatedPiecesTest::RunTest(const FString& Parameters)
{
	using namespace GridFrameFixture;
	const GridSnap::FGridFrame Frame = GridSnap::FGridFrame::Along(FVector2D(1234.0, 777.0), Deg(30.0), 500.0);
	const FVector2D Centre(-2345.0, 5432.0);
	TArray<GridSnap::FPiece> Pieces;
	GridSnap::PiecesInDisc(Centre, 6000.0, Frame, Pieces);
	if (!TestTrue(TEXT("pieces drawn"), Pieces.Num() > 0)) { return false; }

	bool bSquare = true;
	bool bInside = true;
	bool bMajorsOnFives = true;
	int32 Majors = 0;
	for (const GridSnap::FPiece& Piece : Pieces)
	{
		const FVector2D U = (Piece.To - Piece.From).GetSafeNormal();
		const double Dot = FMath::Abs(FVector2D::DotProduct(U, Frame.Axis));
		bSquare &= Dot < 1e-9 || FMath::Abs(Dot - 1.0) < 1e-9;
		bInside &= FVector2D::Distance(Piece.From, Centre) <= 6000.0 + 1e-6 && FVector2D::Distance(Piece.To, Centre) <= 6000.0 + 1e-6;
		// A piece ACROSS the axis lies on a line of constant local X; ALONG it, constant local Y.
		const FVector2D L = Local(Frame, Piece.From);
		const double At = Dot < 1e-9 ? L.X : L.Y;
		bMajorsOnFives &= Piece.bMajor == OnLine(At, 2500.0, 1e-3);
		Majors += Piece.bMajor ? 1 : 0;
	}
	TestTrue(TEXT("every piece along or across the axis"), bSquare);
	TestTrue(TEXT("every piece inside the disc"), bInside);
	TestTrue(TEXT("major every fifth line from the frame origin"), bMajorsOnFives && Majors > 0);
	return true;
}

#endif
