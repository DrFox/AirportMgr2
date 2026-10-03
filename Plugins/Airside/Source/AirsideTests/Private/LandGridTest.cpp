#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/LandGrid.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// The R3 start on a 600 m 8x8 grid with its origin at the world origin: column 0, rows 3-4.
	FLandGrid LgStart()
	{
		const FIntPoint Start[] = { FIntPoint(0, 3), FIntPoint(0, 4) };
		return FLandGrid::Make(FVector2D::ZeroVector, 60000.0, 8, 8, Start);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandGridDefaultOwnsEverything, "Airside.Model.LandGrid.DefaultOwnsEverything",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandGridDefaultOwnsEverything::RunTest(const FString&)
{
	// A NETWORK SAVED BEFORE LAND EXISTED loads a default grid. It must own everything, or every old map
	// and every old save would refuse every build.
	const FLandGrid Grid;
	TestFalse(TEXT("a default grid is invalid"), Grid.IsValid());
	TestTrue(TEXT("and owns a point anywhere"), Grid.IsOwned(FVector2D(1.0e7, -1.0e7)));
	TestEqual(TEXT("and clamps nothing"), Grid.ClampToOwned(FVector2D(1.0e7, 5.0)), FVector2D(1.0e7, 5.0));
	TestEqual(TEXT("and has no outline to wall"), Grid.Outline().Num(), 0);
	FLandGrid TooBig = Grid;
	TooBig.Columns = 9;
	TooBig.Rows = 8;
	TestFalse(TEXT("9x8 tiles do not fit a 64-bit mask, so it is invalid rather than wrapping bits"), TooBig.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandGridOwnership, "Airside.Model.LandGrid.Ownership",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandGridOwnership::RunTest(const FString&)
{
	const FLandGrid Grid = LgStart();
	TestTrue(TEXT("valid"), Grid.IsValid());
	TestEqual(TEXT("two tiles owned"), Grid.NumOwned(), 2);
	TestTrue(TEXT("inside tile (0,3)"), Grid.IsOwned(FVector2D(30000.0, 210000.0)));
	TestFalse(TEXT("tile (1,3) is not owned"), Grid.IsOwned(FVector2D(90000.0, 210000.0)));
	TestTrue(TEXT("the shared edge between two owned tiles is owned"), Grid.IsOwned(FVector2D(30000.0, 240000.0)));
	TestFalse(TEXT("the edge between owned and unowned is NOT - a tuft or a wall there leans over the cut"),
		Grid.IsOwned(FVector2D(60000.0, 210000.0)));
	TestFalse(TEXT("off the grid is not owned"), Grid.IsOwned(FVector2D(-1.0, 210000.0)));
	TestEqual(TEXT("TileAt"), Grid.TileAt(FVector2D(90000.0, 250000.0)), FIntPoint(1, 4));
	TestEqual(TEXT("mask word 0 holds bits 0-15: (0,3) is bit 24, so word 0 is empty"), int32(Grid.MaskWord(0)), 0);
	TestEqual(TEXT("word 1 holds bits 16-31: bit 24 is 1 << 8"), int32(Grid.MaskWord(1)), 1 << 8);
	TestEqual(TEXT("word 2 holds bits 32-47: bit 32 (0,4) is 1 << 0"), int32(Grid.MaskWord(2)), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandGridBuyable, "Airside.Model.LandGrid.Buyable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandGridBuyable::RunTest(const FString&)
{
	const FLandGrid Grid = LgStart();
	TestTrue(TEXT("edge-adjacent (1,3) is buyable"), Grid.IsBuyable(FIntPoint(1, 3)));
	TestTrue(TEXT("(0,2) beside (0,3) is buyable"), Grid.IsBuyable(FIntPoint(0, 2)));
	TestFalse(TEXT("diagonal (1,2) is not - R4"), Grid.IsBuyable(FIntPoint(1, 2)));
	TestFalse(TEXT("an owned tile is not"), Grid.IsBuyable(FIntPoint(0, 3)));
	TestFalse(TEXT("off the grid is not"), Grid.IsBuyable(FIntPoint(-1, 3)));
	TestFalse(TEXT("far away is not"), Grid.IsBuyable(FIntPoint(5, 5)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandGridOutlineAndClamp, "Airside.Model.LandGrid.OutlineAndClamp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandGridOutlineAndClamp::RunTest(const FString&)
{
	// AN L: (0,3), (0,4), (1,3). Six boundary runs - the 1x2's west and its two ends merge across the
	// shared edge, and the notch at (1,4) adds two.
	FLandGrid Grid = LgStart();
	Grid.SetTileOwned(FIntPoint(1, 3), true);
	const TArray<FLandEdgeRun> Runs = Grid.Outline();
	TestEqual(TEXT("an L has six runs"), Runs.Num(), 6);
	double Perimeter = 0.0;
	for (const FLandEdgeRun& Run : Runs)
	{
		Perimeter += FVector2D::Distance(Run.A, Run.B);
		const FVector2D Mid = (Run.A + Run.B) * 0.5;
		TestTrue(TEXT("one step inward from a run is owned"), Grid.IsOwned(Mid - Run.Outward * 100.0));
		TestFalse(TEXT("one step outward is not"), Grid.IsOwned(Mid + Run.Outward * 100.0));
	}
	TestEqual(TEXT("the L's perimeter is 8 tile sides"), Perimeter, 8.0 * 60000.0, 1e-6);

	// (100000, 290000) is over the notch (1,4). The bounding box would leave it there; the nearest owned
	// point is on (0,4)'s east edge, 400 m away, nearer than (1,3)'s north edge at 500 m.
	TestEqual(TEXT("a point over the notch clamps to the nearest owned point, not the bounding box"),
		Grid.ClampToOwned(FVector2D(100000.0, 290000.0)), FVector2D(60000.0, 290000.0));
	TestEqual(TEXT("an owned point is left alone"), Grid.ClampToOwned(FVector2D(30000.0, 200000.0)), FVector2D(30000.0, 200000.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandGridAreas, "Airside.Model.LandGrid.Areas",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandGridAreas::RunTest(const FString&)
{
	FLandGrid Grid = LgStart();
	Grid.SetTileOwned(FIntPoint(1, 3), true);   // the L: (0,3), (0,4), (1,3)
	TestTrue(TEXT("a strip inside one tile"), Grid.IsStripOwned(FVector2D(10000, 200000), FVector2D(50000, 200000), 1200));
	TestTrue(TEXT("EdgeTouchingIsOwned: a strip whose side lies ON the outer edge"),
		Grid.IsStripOwned(FVector2D(1200, 190000), FVector2D(1200, 290000), 1200));
	TestFalse(TEXT("ShoulderOverTheCutIsNot: centreline inside, shoulder 1 m over the west cut"),
		Grid.IsStripOwned(FVector2D(1100, 190000), FVector2D(1100, 290000), 1200));
	// NotchIsNotOwned: every vertex on owned tiles, but the strip crosses unowned (1,4).
	TestFalse(TEXT("a diagonal across the L's notch"), Grid.IsStripOwned(FVector2D(50000, 290000), FVector2D(110000, 230000), 500));
	TestTrue(TEXT("across the shared edge between two owned tiles"), Grid.IsStripOwned(FVector2D(30000, 200000), FVector2D(90000, 200000), 2000));
	const TArray<FVector2D> OffGrid = { FVector2D(-5000, 200000), FVector2D(5000, 200000), FVector2D(5000, 210000) };
	TestFalse(TEXT("off the grid"), Grid.IsAreaOwned(OffGrid));
	TestTrue(TEXT("an invalid grid owns any area"), FLandGrid().IsAreaOwned(OffGrid));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandGridOutsideText, "Airside.Model.LandGrid.OutsideText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FLandGridOutsideText::RunTest(const FString&)
{
	// THE ONE WORDING every build refusal past the edge uses (spec R7) - the readouts and the tests compare against it.
	TestEqual(TEXT("refusal text"), FLandGrid::OutsideText, FString(TEXT("Outside your land")));
	return true;
}

#endif
