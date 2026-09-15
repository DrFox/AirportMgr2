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
		const PlotFit::FPlotFit Fit = PlotFit::FitBays(PlotRect(1200.0, 800.0), A, B);
		TestTrue(TEXT("12 m of frontage fits"), Fit.bFits);
		TestEqual(TEXT("and gives exactly three bays"), Fit.Bays.Num(), 3);
	}

	// 11 m floors to two. The concept sheet's 11 m was a drawing, not a decision - a
	// solver that rounded up would silently give the player a bay they did not draw.
	{
		SouthEdge(1100.0, A, B);
		const PlotFit::FPlotFit Fit = PlotFit::FitBays(PlotRect(1100.0, 800.0), A, B);
		TestEqual(TEXT("11 m floors to two bays, never rounds up"), Fit.Bays.Num(), 2);
	}

	// Under one bay is a refusal WITH A REASON, because the tool has to say which.
	{
		SouthEdge(300.0, A, B);
		const PlotFit::FPlotFit Fit = PlotFit::FitBays(PlotRect(300.0, 800.0), A, B);
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
			FVector2D(1200.0, 800.0),
			FVector2D(800.0, 800.0), FVector2D(800.0, 300.0),  // the notch, biting down
			FVector2D(400.0, 300.0), FVector2D(400.0, 800.0),
			FVector2D(0.0, 800.0) };
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

	const PlotFit::FPlotFit Fit = PlotFit::FitBays(PlotRect(1200.0, 800.0), A, B);
	if (!TestTrue(TEXT("the plot fits"), Fit.bFits)) { return false; }

	for (const PlotFit::FPlotBay& Bay : Fit.Bays)
	{
		const FVector2D Forward(FMath::Cos(Bay.Heading), FMath::Sin(Bay.Heading));
		TestTrue(TEXT("the bay's +X points away from the frontage, not at it"),
			Forward.Y > 0.9);
		TestTrue(TEXT("and the bay sits inside the plot, not on its boundary"),
			Bay.Centre.Y > 0.0 && Bay.Centre.Y < 800.0);
	}

	// THE SAME PLOT WOUND THE OTHER WAY ROUND. The inward normal is derived from the
	// outline's winding, so a plot whose points happen to run clockwise must still aim its
	// bays into the plot - not out of it, which would put every shed across the road.
	{
		TArray<FVector2D> Clockwise = PlotRect(1200.0, 800.0);
		Algo::Reverse(Clockwise);
		// The south edge, traversed as this winding traverses it.
		const PlotFit::FPlotFit Flipped = PlotFit::FitBays(
			Clockwise, FVector2D(1200.0, 0.0), FVector2D(0.0, 0.0));
		if (TestTrue(TEXT("a clockwise plot still fits"), Flipped.bFits))
		{
			for (const PlotFit::FPlotBay& Bay : Flipped.Bays)
			{
				TestTrue(TEXT("and its bays are still inside the plot, not across the road"),
					Bay.Centre.Y > 0.0 && Bay.Centre.Y < 800.0);
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlotGridSlotsTest,
	"Airside.Solve.PlotGridSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlotGridSlotsTest::RunTest(const FString& Parameters)
{
	// Frontage on y = 0 running east, so the interior is to the north.
	const FVector2D A(0.0, 0.0);
	const FVector2D B(1200.0, 0.0);

	// THREE WIDE, TWO DEEP is six slots. The first THREE are row 1 - the row that fronts the
	// road and takes the modules - and the ORDER is part of the contract, because the
	// presenter fills slots in order and a row-major slip would put a shed in the back yard
	// where no truck reaches it.
	{
		const PlotFit::FPlotGrid Grid = PlotFit::BuildGrid(A, B, 3, 2);
		TestEqual(TEXT("three by two is six slots"), Grid.Slots.Num(), 6);
		TestEqual(TEXT("and remembers its width"), Grid.Width, 3);
		TestEqual(TEXT("and its depth"), Grid.Depth, 2);

		for (int32 I = 0; I < 3; ++I)
		{
			TestTrue(TEXT("row 1 fronts the road"),
				FMath::IsNearlyEqual(Grid.Slots[I].Centre.Y, PlotFit::BayDepthUu * 0.5, 1.0));
		}
		for (int32 I = 3; I < 6; ++I)
		{
			TestTrue(TEXT("row 2 sits a full bay behind row 1"),
				FMath::IsNearlyEqual(Grid.Slots[I].Centre.Y, PlotFit::BayDepthUu * 1.5, 1.0));
		}
	}

	// EVERY SLOT FACES AWAY FROM THE ROAD, back rows included - a module in row 2 is still a
	// shed whose doors face the yard, not one turned round because it does not front the road.
	{
		const PlotFit::FPlotGrid Grid = PlotFit::BuildGrid(A, B, 2, 2);
		for (const PlotFit::FPlotBay& Slot : Grid.Slots)
		{
			const FVector2D Forward(FMath::Cos(Slot.Heading), FMath::Sin(Slot.Heading));
			TestTrue(TEXT("+X points away from the frontage"), Forward.Y > 0.9);
		}
	}

	// THE OUTLINE IS COUNTER-CLOCKWISE, because the pad goes through the same ear-clipper an
	// apron does and a clockwise one faces DOWN. That shipped on 2026-09-15 and rendered as
	// no concrete at all. Shoelace sign, positive for CCW.
	{
		const TArray<FVector2D> Outline = PlotFit::GridOutline(A, B, 3, 2);
		TestEqual(TEXT("a rectangle has four corners, not five"), Outline.Num(), 4);

		double Twice = 0.0;
		for (int32 I = 0; I < Outline.Num(); ++I)
		{
			const FVector2D& P = Outline[I];
			const FVector2D& Q = Outline[(I + 1) % Outline.Num()];
			Twice += P.X * Q.Y - Q.X * P.Y;
		}
		TestTrue(TEXT("and is wound counter-clockwise"), Twice > 0.0);

		// AND IT ENCLOSES EVERY SLOT. The grid and the boundary are derived separately from
		// the same edge, so nothing but a test makes them agree - a plot whose sheds stood
		// outside its own fence would look like a presenter bug for a long time.
		const PlotFit::FPlotGrid Grid = PlotFit::BuildGrid(A, B, 3, 2);
		for (const PlotFit::FPlotBay& Slot : Grid.Slots)
		{
			TestTrue(TEXT("every slot centre lies inside the plot boundary"),
				Slot.Centre.X > 0.0 && Slot.Centre.X < 1200.0
				&& Slot.Centre.Y > 0.0 && Slot.Centre.Y < 1600.0);
		}
	}

	// A degenerate size yields nothing, rather than a zero-area rectangle the player could
	// still press Build on.
	{
		TestEqual(TEXT("zero width is no slots"), PlotFit::BuildGrid(A, B, 0, 2).Slots.Num(), 0);
		TestEqual(TEXT("zero depth is no slots"), PlotFit::BuildGrid(A, B, 3, 0).Slots.Num(), 0);
		TestEqual(TEXT("and no outline either"), PlotFit::GridOutline(A, B, 0, 2).Num(), 0);
	}

	return true;
}

#endif
