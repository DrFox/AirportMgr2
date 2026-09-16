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

	/** A shed-sized footprint that fronts the gate. 8 m deep, 4 m wide. */
	PlotYard::FFootprint Shed()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 800.0;
		F.WidthUu = 400.0;
		F.bFrontsTheGate = true;
		return F;
	}

	/** A tank-sized footprint: 5 m x 5 m, and it does not front the gate. */
	PlotYard::FFootprint Tank()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 500.0;
		F.WidthUu = 500.0;
		F.bFrontsTheGate = false;
		return F;
	}

	/** A pump: 3 m x 2 m, low and small. */
	PlotYard::FFootprint Pump()
	{
		PlotYard::FFootprint F;
		F.LengthUu = 300.0;
		F.WidthUu = 200.0;
		F.bFrontsTheGate = false;
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
	FPlotYardFrontsTheShedOnTheGateTest,
	"Airside.Solve.PlotYardFrontsTheShedOnTheGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotYardFrontsTheShedOnTheGateTest::RunTest(const FString& Parameters)
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

	// SQUARE TO THE FRONTAGE, not jittered. The truck drives out of the shed, so its
	// heading is functional - it is the one module that may not be turned for looks.
	// Interior is +Y here, so the inward bearing is +90 degrees.
	TestEqual(TEXT("the shed faces away from the road, square"),
		Yard.Stands[0].Heading, UE_DOUBLE_HALF_PI);

	// AND IT IS ON THE GATE. A shed behind the tank is a shed the truck cannot leave, and
	// it would look perfectly correct from every angle.
	TestTrue(TEXT("the shed sits at the gate, not deep in the yard"),
		FVector2D::Distance(Yard.Stands[0].Centre, Gate) < 800.0);

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
	// A TIGHT PLOT, deliberately: on a large one a naive sampler passes by luck. Two bays
	// wide and two deep has to hold a shed, a tank and a pump with little room to spare.
	const TArray<FVector2D> Outline = YardRect(800.0, 1600.0);
	const PlotYard::FFootprint Footprints[] = { Shed(), Tank(), Pump() };

	for (int32 Seed = 1; Seed <= 8; ++Seed)
	{
		const PlotYard::FYard Yard = PlotYard::LayOut(
			Outline, FVector2D(0.0, 0.0), FVector2D(800.0, 0.0), FVector2D(400.0, 0.0),
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

#endif
