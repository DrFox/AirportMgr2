#include "CoreMinimal.h"
#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"
#include "Solve/PlotFit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** An axis-aligned rectangle, CCW, with its SOUTH edge (y = 0) as the frontage. */
	TArray<FVector2D> PlotRect(double Width, double Depth)
	{
		return { FVector2D(0.0, 0.0), FVector2D(Width, 0.0),
		         FVector2D(Width, Depth), FVector2D(0.0, Depth) };
	}

	/** The south edge of PlotRect(Width, _), which is the edge the road runs along. */
	void SouthEdge(double Width, FVector2D& OutA, FVector2D& OutB)
	{
		OutA = FVector2D(0.0, 0.0);
		OutB = FVector2D(Width, 0.0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotFitBaysTest,
	"Airside.Solve.PlotFitBays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotFitBaysTest::RunTest(const FString& Parameters)
{
	FVector2D A = FVector2D::ZeroVector;
	FVector2D B = FVector2D::ZeroVector;

	// 12 m of frontage is three 4 m bays exactly - the Tier 1 depot as drawn.
	//
	// NOTE this is the case where every bay corner lands exactly ON the plot boundary,
	// which is the hardest case for a point-in-polygon test, not the easiest. It is first
	// deliberately: a containment test that is undefined on the boundary passes or fails
	// this by coin flip, and would do so differently on another machine.
	{
		SouthEdge(1200.0, A, B);
		const PlotFit::FPlotFit Fit = PlotFit::FitBays(PlotRect(1200.0, 1200.0), A, B);
		TestTrue(TEXT("12 m of frontage fits"), Fit.bFits);
		TestEqual(TEXT("and gives exactly three bays"), Fit.Bays.Num(), 3);
	}

	// 11 m floors to two. The concept sheet's 11 m was a drawing, not a decision - a
	// solver that rounded up would silently give the player a bay they did not draw.
	{
		SouthEdge(1100.0, A, B);
		const PlotFit::FPlotFit Fit = PlotFit::FitBays(PlotRect(1100.0, 1200.0), A, B);
		TestEqual(TEXT("11 m floors to two bays, never rounds up"), Fit.Bays.Num(), 2);
	}

	// Under one bay is a refusal WITH A REASON, because the tool has to say which.
	{
		SouthEdge(300.0, A, B);
		const PlotFit::FPlotFit Fit = PlotFit::FitBays(PlotRect(300.0, 1200.0), A, B);
		TestFalse(TEXT("3 m of frontage does not fit"), Fit.bFits);
		TestEqual(TEXT("and says why, so the tool can tell the player"),
			static_cast<int32>(Fit.Why), static_cast<int32>(PlotFit::EPlotRefusal::TooSmall));
	}

	// Too SHALLOW is also TooSmall: a 12 m x 2 m strip has frontage but no room for a bay.
	// The frontage check alone would accept it, so this is the corner test earning its keep.
	{
		SouthEdge(1200.0, A, B);
		const PlotFit::FPlotFit Fit = PlotFit::FitBays(PlotRect(1200.0, 200.0), A, B);
		TestFalse(TEXT("a plot shallower than one bay does not fit"), Fit.bFits);
		TestEqual(TEXT("and says so rather than returning bays hanging over the edge"),
			static_cast<int32>(Fit.Why), static_cast<int32>(PlotFit::EPlotRefusal::TooSmall));
	}

	// A NOTCH cut out of the middle of the plot. The centre bay's corners fall in the
	// notch, so a centre-point-only containment test accepts it and the player watches a
	// shed stand on the grass. Non-convex plots are what a freeform gesture produces, so
	// this is the ordinary case, not an exotic one.
	{
		SouthEdge(1200.0, A, B);
		const TArray<FVector2D> Notched = {
			FVector2D(0.0, 0.0),   FVector2D(1200.0, 0.0),
			FVector2D(1200.0, 1200.0),
			FVector2D(800.0, 1200.0), FVector2D(800.0, 300.0),  // the notch, biting down
			FVector2D(400.0, 300.0), FVector2D(400.0, 1200.0),
			FVector2D(0.0, 1200.0) };
		const PlotFit::FPlotFit Fit = PlotFit::FitBays(Notched, A, B);
		TestEqual(TEXT("the bay under the notch is dropped, the outer two stand"),
			Fit.Bays.Num(), 2);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotFitFacesAwayFromRoadTest,
	"Airside.Solve.PlotFitFacesAwayFromRoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotFitFacesAwayFromRoadTest::RunTest(const FString& Parameters)
{
	// BuildFuelDepot: "+X FACES AWAY FROM THE ROAD... the truck drives out behind it",
	// and its own comment records that this was once written the wrong way round. Pin the
	// corrected statement: with the frontage on y = 0 and the plot to the north, every
	// bay's +X must point north, away from the road.
	FVector2D A = FVector2D::ZeroVector;
	FVector2D B = FVector2D::ZeroVector;
	SouthEdge(1200.0, A, B);

	const PlotFit::FPlotFit Fit = PlotFit::FitBays(PlotRect(1200.0, 1200.0), A, B);
	if (!TestTrue(TEXT("the plot fits"), Fit.bFits)) { return false; }

	for (const PlotFit::FPlotBay& Bay : Fit.Bays)
	{
		const FVector2D Forward(FMath::Cos(Bay.Heading), FMath::Sin(Bay.Heading));
		TestTrue(TEXT("the bay's +X points away from the frontage, not at it"),
			Forward.Y > 0.9);
		TestTrue(TEXT("and the bay sits inside the plot, not on its boundary"),
			Bay.Centre.Y > 0.0 && Bay.Centre.Y < 1200.0);
	}

	// THE SAME PLOT WOUND THE OTHER WAY ROUND. The inward normal is derived from the
	// outline's winding, so a plot whose points happen to run clockwise must still aim its
	// bays into the plot - not out of it, which would put every shed across the road.
	{
		TArray<FVector2D> Clockwise = PlotRect(1200.0, 1200.0);
		Algo::Reverse(Clockwise);
		// The south edge, traversed as this winding traverses it.
		const PlotFit::FPlotFit Flipped = PlotFit::FitBays(
			Clockwise, FVector2D(1200.0, 0.0), FVector2D(0.0, 0.0));
		if (TestTrue(TEXT("a clockwise plot still fits"), Flipped.bFits))
		{
			for (const PlotFit::FPlotBay& Bay : Flipped.Bays)
			{
				TestTrue(TEXT("and its bays are still inside the plot, not across the road"),
					Bay.Centre.Y > 0.0 && Bay.Centre.Y < 1200.0);
			}
		}
	}

	return true;
}

#endif
