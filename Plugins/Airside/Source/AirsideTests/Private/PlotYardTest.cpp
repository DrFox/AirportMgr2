#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Solve/PlotYard.h"

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

#endif
