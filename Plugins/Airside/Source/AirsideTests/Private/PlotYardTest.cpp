#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/PlotYard.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** An axis-aligned rectangle, CCW, with its SOUTH edge (y = 0) as the frontage.
	 *  Same shape PlotFitTest uses, so the two files describe the same world. */
	TArray<FVector2D> YardRect(double Width, double Depth)
	{
		return { FVector2D(0.0, 0.0), FVector2D(Width, 0.0),
		         FVector2D(Width, Depth), FVector2D(0.0, Depth) };
	}

	/** A shed-sized footprint, stood against the back fence. 8 m deep, 4 m wide. */
	PlotYard::FFootprint Shed()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 800.0;
		F.WidthUu = 400.0;
		F.bAgainstTheBackFence = true;
		return F;
	}

	/** A tank-sized footprint: 5 m x 5 m, and it does not front the gate. */
	PlotYard::FFootprint Tank()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 500.0;
		F.WidthUu = 500.0;
		F.bAgainstTheBackFence = false;
		return F;
	}

	/** A pump: 3 m x 2 m, low and small. */
	PlotYard::FFootprint Pump()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 300.0;
		F.WidthUu = 200.0;
		F.bAgainstTheBackFence = false;
		return F;
	}

	/** 16 m x 16 m. Big enough to actually compete with a phantom tank for room. */
	PlotYard::FFootprint Hangar()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 1600.0;
		F.WidthUu = 1600.0;
		F.bAgainstTheBackFence = false;
		return F;
	}

	/** True when two stands' corner rectangles intersect, by separating axis. */
	bool StandsOverlap(const PlotYard::FStand& A, const PlotYard::FFootprint& FA,
		const PlotYard::FStand& B, const PlotYard::FFootprint& FB)
	{
		TArray<FVector2D> CornersA;
		TArray<FVector2D> CornersB;
		PlotYard::StandCorners(A, FA, CornersA);
		PlotYard::StandCorners(B, FB, CornersB);

		// Four candidate axes - two per rectangle. Two convex shapes miss each other if and
		// only if some axis separates them, so finding one is proof of no overlap.
		const TArray<FVector2D> Axes = {
			(CornersA[1] - CornersA[0]).GetSafeNormal(),
			(CornersA[3] - CornersA[0]).GetSafeNormal(),
			(CornersB[1] - CornersB[0]).GetSafeNormal(),
			(CornersB[3] - CornersB[0]).GetSafeNormal() };

		for (const FVector2D& Axis : Axes)
		{
			double MinA = TNumericLimits<double>::Max();
			double MaxA = -TNumericLimits<double>::Max();
			double MinB = TNumericLimits<double>::Max();
			double MaxB = -TNumericLimits<double>::Max();
			for (const FVector2D& P : CornersA)
			{
				const double D = FVector2D::DotProduct(P, Axis);
				MinA = FMath::Min(MinA, D);
				MaxA = FMath::Max(MaxA, D);
			}
			for (const FVector2D& P : CornersB)
			{
				const double D = FVector2D::DotProduct(P, Axis);
				MinB = FMath::Min(MinB, D);
				MaxB = FMath::Max(MaxB, D);
			}
			if (MaxA < MinB || MaxB < MinA)
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardStandsTheShedAtTheBackTest,
	"Airside.Solve.PlotYardStandsTheShedAtTheBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardStandsTheShedAtTheBackTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(2400.0, 1600.0);
	const FVector2D FrontageA(0.0, 0.0);
	const FVector2D FrontageB(2400.0, 0.0);
	const FVector2D Gate(1200.0, 0.0);

	const PlotYard::FFootprint Footprints[] = { Shed() };

	const PlotYard::FYard Yard = PlotYard::LayOut(
		Outline, FrontageA, FrontageB, Gate, Footprints, /*Seed=*/1234, Shed());

	if (!TestEqual(TEXT("one footprint in, one stand out"), Yard.Stands.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("the shed was placed"), Yard.Stands[0].bPlaced);

	// SQUARE TO THE FRONTAGE, not jittered. The truck drives out of the shed, so its heading
	// is functional - the one module that may not be turned for looks. Interior is +Y here,
	// so the inward bearing is +90 degrees.
	TestEqual(TEXT("the shed faces away from the road, square"),
		Yard.Stands[0].Heading, UE_DOUBLE_HALF_PI);

	// AND IT STANDS AT THE BACK. It stood in the gateway until 2026-09-17 - a depot whose
	// only way in is blocked by the building you drive out of. The plot is 16 m deep and the
	// shed 8 m long, so its centre belongs at 12 m: hard against the back fence.
	TestTrue(*FString::Printf(TEXT("the shed is against the back fence, got y %.0f"),
		Yard.Stands[0].Centre.Y),
		FMath::IsNearlyEqual(Yard.Stands[0].Centre.Y, 1200.0, 1.0));

	// NOT IN THE GATEWAY, stated as its own claim: "deep in the yard" and "clear of the gate"
	// are different facts, and it was the second that failed.
	TestTrue(TEXT("and well clear of the gate it used to block"),
		FVector2D::Distance(Yard.Stands[0].Centre, Gate) > PlotYard::GateCorridorUu);

	// A PLOT TOO SHALLOW still gets it wholly inside the fence rather than hanging across
	// the road - the clamp, which no other case reaches.
	const PlotYard::FYard Shallow = PlotYard::LayOut(
		YardRect(2400.0, 600.0), FrontageA, FrontageB, Gate, Footprints, 1234, Shed());
	if (TestEqual(TEXT("still one stand"), Shallow.Stands.Num(), 1))
	{
		TestTrue(TEXT("a shallow plot keeps the shed off the road"),
			Shallow.Stands[0].Centre.Y >= 400.0 - 1.0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardKeepsModulesInsideThePlotTest,
	"Airside.Solve.PlotYardKeepsModulesInsideThePlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardKeepsModulesInsideThePlotTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(2400.0, 1600.0);
	const PlotYard::FFootprint Footprints[] = { Shed(), Tank(), Pump() };

	// SEVERAL SEEDS, not one. A sampler that happens to keep everything inside on seed 1234
	// and hangs a tank over the fence on 1235 is exactly the bug this guards, and a
	// single-seed test would ship it.
	for (int32 Seed = 1; Seed <= 8; ++Seed)
	{
		const PlotYard::FYard Yard = PlotYard::LayOut(
			Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0), FVector2D(1200.0, 0.0),
			Footprints, Seed, Tank());

		for (int32 Index = 0; Index < Yard.Stands.Num(); ++Index)
		{
			if (!Yard.Stands[Index].bPlaced)
			{
				continue;
			}

			TArray<FVector2D> Corners;
			PlotYard::StandCorners(Yard.Stands[Index], Footprints[Index], Corners);
			for (const FVector2D& Corner : Corners)
			{
				// EVERY CORNER, not the centre. A centre-only test accepts a module hanging
				// out of the plot and the player watches a tank stand on the grass.
				TestTrue(*FString::Printf(
					TEXT("seed %d: module %d corner (%.0f, %.0f) is inside the plot"),
					Seed, Index, Corner.X, Corner.Y),
					RoadGeom::PointInPolygon(Outline, Corner));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardDoesNotOverlapModulesTest,
	"Airside.Solve.PlotYardDoesNotOverlapModules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardDoesNotOverlapModulesTest::RunTest(const FString& Parameters)
{
	// TIGHT, BUT NOT SO TIGHT NOTHING FITS: on a large plot a naive sampler passes by luck,
	// and on too small a one there is no pair left to compare.
	//
	// It was 8 m x 16 m until 2026-09-17, when the shed moved from the gateway to the back
	// fence. The shed then took the back, the gate corridor took the front, and on a plot
	// only 8 m wide the strips either side of the corridor are too narrow for anything -
	// one module stood and this test's own guard caught it.
	const TArray<FVector2D> Outline = YardRect(1600.0, 2000.0);
	const PlotYard::FFootprint Footprints[] = { Shed(), Tank(), Pump() };

	for (int32 Seed = 1; Seed <= 8; ++Seed)
	{
		const PlotYard::FYard Yard = PlotYard::LayOut(
			Outline, FVector2D(0.0, 0.0), FVector2D(1600.0, 0.0), FVector2D(800.0, 0.0),
			Footprints, Seed, Tank());

		// AT LEAST TWO STANDING, or the loops below compare nothing and this test goes
		// green for the wrong reason. A tight plot is the right shape to catch collisions
		// and the wrong shape to trust blindly - some modules ARE expected to drop here,
		// which is exactly how a vacuous pass would hide.
		int32 Placed = 0;
		for (const PlotYard::FStand& Stand : Yard.Stands)
		{
			if (Stand.bPlaced) { ++Placed; }
		}
		TestTrue(*FString::Printf(
			TEXT("seed %d: at least two modules stand, so there is a pair to compare"), Seed),
			Placed >= 2);

		for (int32 A = 0; A < Yard.Stands.Num(); ++A)
		{
			for (int32 B = A + 1; B < Yard.Stands.Num(); ++B)
			{
				if (!Yard.Stands[A].bPlaced || !Yard.Stands[B].bPlaced)
				{
					continue;
				}
				// TWO MODULES IN ONE SPACE is the one failure that cannot be argued as
				// styling - it is a mesh through a mesh, and no camera angle hides it.
				TestFalse(*FString::Printf(TEXT("seed %d: module %d and %d do not intersect"),
					Seed, A, B),
					StandsOverlap(Yard.Stands[A], Footprints[A], Yard.Stands[B], Footprints[B]));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardLeavesTheGateClearTest,
	"Airside.Solve.PlotYardLeavesTheGateClear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardLeavesTheGateClearTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(2400.0, 1600.0);
	const FVector2D Gate(1200.0, 0.0);

	// NO SHED in this mix, so nothing is entitled to sit on the gate and every stand here
	// is one the sampler chose. With a shed present the shed IS on the gate by design, and
	// the test would be asserting the opposite of the rule it means to check.
	const PlotYard::FFootprint Footprints[] = { Tank(), Pump(), Pump() };

	for (int32 Seed = 1; Seed <= 8; ++Seed)
	{
		const PlotYard::FYard Yard = PlotYard::LayOut(
			Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0), Gate,
			Footprints, Seed, Tank());

		// EVERY ONE OF THEM PLACED. A plot this size has room for three small modules, so a
		// solver that placed nothing would otherwise slip past every loop below.
		TestEqual(*FString::Printf(TEXT("seed %d: all three modules found a spot"), Seed),
			Yard.DroppedCount(), 0);

		for (int32 Index = 0; Index < Yard.Stands.Num(); ++Index)
		{
			if (!Yard.Stands[Index].bPlaced)
			{
				continue;
			}
			TArray<FVector2D> Corners;
			PlotYard::StandCorners(Yard.Stands[Index], Footprints[Index], Corners);
			for (const FVector2D& Corner : Corners)
			{
				// The gate mouth: one truck length deep from the frontage, +Y here. BOUNDED
				// IN DEPTH on purpose - a lane running the whole yard would cut the plot in
				// two, and the truck only needs room to get off the pad and turn.
				const bool bInLane =
					FMath::Abs(Corner.X - Gate.X) < PlotYard::GateCorridorUu * 0.5
					&& Corner.Y >= 0.0 && Corner.Y <= PlotYard::GateCorridorUu;
				TestFalse(*FString::Printf(
					TEXT("seed %d: module %d keeps out of the truck's way"), Seed, Index),
					bInLane);
			}
		}
	}
	return true;
}

/**
 * ROOM TO GROW IS COUNTED, and counted as more than nothing.
 *
 * THE GAP THIS FILLS: DropsWhatWillNotFit asserts RoomForMore is ZERO on a tiny plot, and
 * IsDeterministic only compares one run's figure against another's. Both pass if the count
 * is always zero - which is exactly what it was, on every plot, until this test said so.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardCountsRoomToGrowTest,
	"Airside.Solve.PlotYardCountsRoomToGrow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardCountsRoomToGrowTest::RunTest(const FString& Parameters)
{
	// 24 m x 24 m and EMPTY: nothing placed, nothing in the way, so a solver that cannot
	// count room here cannot count it anywhere.
	const TArray<FVector2D> Outline = YardRect(2400.0, 2400.0);

	const PlotYard::FYard Bare = PlotYard::LayOut(
		Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0), FVector2D(1200.0, 0.0),
		TArrayView<const PlotYard::FFootprint>(), /*Seed=*/11, Tank());

	TestEqual(TEXT("no footprints in, no stands out"), Bare.Stands.Num(), 0);
	TestTrue(*FString::Printf(
		TEXT("an empty 24 m yard has room for several tanks, got %d"), Bare.RoomForMore),
		Bare.RoomForMore > 1);

	// THE SAME PLOT WITH A DEPOT IN IT still has room, and less of it. Filling a yard must
	// REDUCE what is left rather than leaving the figure untouched, which is how a count
	// that ignores what is standing would look.
	const PlotYard::FFootprint Footprints[] = { Shed(), Tank(), Pump() };
	const PlotYard::FYard Filled = PlotYard::LayOut(
		Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0), FVector2D(1200.0, 0.0),
		Footprints, /*Seed=*/11, Tank());

	TestEqual(TEXT("the mix all fits in a yard this size"), Filled.DroppedCount(), 0);
	TestTrue(*FString::Printf(TEXT("and there is still room to grow, got %d"),
		Filled.RoomForMore), Filled.RoomForMore > 0);

	// WHAT IS STANDING REDUCES WHAT IS LEFT - shown with something big enough to actually
	// compete for the space. The depot's own mix does NOT reduce it at this size (both come
	// back 4): the shed hugs the back fence and the tank and pump tuck into slack a 6 m
	// phantom could never have used, so "filling a yard leaves less room" is simply false
	// here. Asserting it anyway passed until the shed moved, and would have gone on passing
	// as <= while proving nothing.
	const PlotYard::FFootprint Big[] = { Hangar() };
	const PlotYard::FYard Blocked = PlotYard::LayOut(
		Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0), FVector2D(1200.0, 0.0),
		Big, /*Seed=*/11, Tank());

	TestEqual(TEXT("the hangar stands"), Blocked.DroppedCount(), 0);
	TestTrue(*FString::Printf(TEXT("a 16 m hangar eats the room: blocked %d, bare %d"),
		Blocked.RoomForMore, Bare.RoomForMore), Blocked.RoomForMore < Bare.RoomForMore);

	// A SHAPE THE GESTURE CAN ACTUALLY MAKE. This was 12 m wide until 2026-09-17 and the
	// gesture's minimum frontage is 15 m, so it was testing a plot no player could draw -
	// and once the shed moved to the back fence a plot that narrow had no usable middle at
	// all, which is how the staleness surfaced.
	const TArray<FVector2D> Drawn20 = YardRect(2000.0, 2400.0);
	const PlotYard::FYard Drawn = PlotYard::LayOut(
		Drawn20, FVector2D(0.0, 0.0), FVector2D(2000.0, 0.0), FVector2D(1000.0, 0.0),
		Footprints, /*Seed=*/11, Tank());

	TestEqual(TEXT("the mix fits a 20 m x 24 m plot"), Drawn.DroppedCount(), 0);
	TestTrue(*FString::Printf(
		TEXT("and it has room for another tank, got %d"), Drawn.RoomForMore),
		Drawn.RoomForMore > 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardIsDeterministicTest,
	"Airside.Solve.PlotYardIsDeterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardIsDeterministicTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(2400.0, 1600.0);
	const PlotYard::FFootprint Footprints[] = { Shed(), Tank(), Pump() };

	auto Lay = [&]()
	{
		return PlotYard::LayOut(Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0),
			FVector2D(1200.0, 0.0), Footprints, /*Seed=*/4242, Tank());
	};

	const PlotYard::FYard First = Lay();
	const PlotYard::FYard Second = Lay();

	if (!TestEqual(TEXT("both lay out the same number of stands"),
		First.Stands.Num(), Second.Stands.Num()))
	{
		return false;
	}

	for (int32 Index = 0; Index < First.Stands.Num(); ++Index)
	{
		// BITWISE, not nearly. UPlotPresenter::RebuildFrom clears and rebuilds on every
		// graph change, so "close enough" is a yard that shivers every time the player lays
		// a road somewhere else on the airport - and nothing on screen would explain why.
		TestTrue(*FString::Printf(TEXT("stand %d lands on exactly the same spot"), Index),
			First.Stands[Index].Centre == Second.Stands[Index].Centre);
		TestTrue(*FString::Printf(TEXT("stand %d takes exactly the same heading"), Index),
			First.Stands[Index].Heading == Second.Stands[Index].Heading);
		TestEqual(*FString::Printf(TEXT("stand %d agrees about being placed"), Index),
			First.Stands[Index].bPlaced, Second.Stands[Index].bPlaced);
	}
	TestEqual(TEXT("and both agree how much room is left"),
		First.RoomForMore, Second.RoomForMore);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardVariesWithSeedTest,
	"Airside.Solve.PlotYardVariesWithSeed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardVariesWithSeedTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Outline = YardRect(2400.0, 1600.0);
	const PlotYard::FFootprint Footprints[] = { Shed(), Tank(), Pump() };

	auto LaySeeded = [&](int32 Seed)
	{
		return PlotYard::LayOut(Outline, FVector2D(0.0, 0.0), FVector2D(2400.0, 0.0),
			FVector2D(1200.0, 0.0), Footprints, Seed, Tank());
	};

	const PlotYard::FYard A = LaySeeded(1);
	const PlotYard::FYard B = LaySeeded(2);

	// WITHOUT THIS, a solver that ignored the seed entirely - or one that quietly placed
	// everything on a grid again - would pass every other test in this file. Two depots
	// looking identical IS the complaint this whole feature answers.
	bool bAnyDifference = false;
	for (int32 Index = 0; Index < A.Stands.Num() && Index < B.Stands.Num(); ++Index)
	{
		if (A.Stands[Index].Centre != B.Stands[Index].Centre
			|| A.Stands[Index].Heading != B.Stands[Index].Heading)
		{
			bAnyDifference = true;
			break;
		}
	}
	TestTrue(TEXT("two seeds lay out two different yards"), bAnyDifference);

	// THE SHED IS THE EXCEPTION and must NOT vary: its pose is functional, not decorative.
	TestTrue(TEXT("but the shed still faces the gate in both"),
		A.Stands[0].Heading == B.Stands[0].Heading);
	TestTrue(TEXT("and stands in the same place in both"),
		A.Stands[0].Centre == B.Stands[0].Centre);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotYardDropsWhatWillNotFitTest,
	"Airside.Solve.PlotYardDropsWhatWillNotFit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardDropsWhatWillNotFitTest::RunTest(const FString& Parameters)
{
	// One bay wide and one row deep: the shed alone fills it.
	const TArray<FVector2D> Outline = YardRect(400.0, 800.0);
	const PlotYard::FFootprint Footprints[] = { Shed(), Tank(), Pump() };

	const PlotYard::FYard Yard = PlotYard::LayOut(
		Outline, FVector2D(0.0, 0.0), FVector2D(400.0, 0.0), FVector2D(200.0, 0.0),
		Footprints, /*Seed=*/7, Tank());

	// ONE ENTRY PER FOOTPRINT, IN ORDER, even for the ones that did not fit. A compacted
	// array would re-associate a pump's stand with a tank, and the depot would draw the
	// wrong box in the wrong place with nothing to say so.
	if (!TestEqual(TEXT("three footprints in, three stands out"), Yard.Stands.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("something had to be dropped from a one-bay plot"), Yard.DroppedCount() > 0);

	// THE SHED IS NOT ONE OF THEM. Its pose is decided rather than sampled, so a plot too
	// small loses the things that were looking for space - never the one the truck needs.
	TestTrue(TEXT("but the shed still stands"), Yard.Stands[0].bPlaced);

	TestEqual(TEXT("and there is no room for more"), Yard.RoomForMore, 0);

	return true;
}

#endif
