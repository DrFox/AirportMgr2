#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Build/GroundCoverMask.h"
#include "Content/AirsideContent.h"
#include "Content/AirsidePrimitives.h"
#include "Content/AirsideSettings.h"
#include "Content/GroundCoverKit.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Present/AirsideGroundCoverActor.h"
#include "Model/LandGrid.h"
#include "Present/RoadEditFacade.h"
#include "Present/GroundCoverPresenter.h"
#include "Present/GroundCoverSubsystem.h"
#include "Present/RoadNetworkActor.h"
#include "Present/RoadSurfacePresenter.h"
#include "Solve/GroundCover.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr double GcCell = 3200.0;

	bool GcSameTufts(const TArray<GroundCover::FTuft>& A, const TArray<GroundCover::FTuft>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (A[Index].Position != B[Index].Position || A[Index].Variant != B[Index].Variant
				|| A[Index].YawDegrees != B[Index].YawDegrees || A[Index].Scale != B[Index].Scale)
			{
				return false;
			}
		}
		return true;
	}

	/** The test's OWN point-in-triangle, not the mask's: a composition test that asked the mask whether the mask worked would measure nothing. */
	bool GcOnTriangle(const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C)
	{
		auto Side = [](const FVector2D& U, const FVector2D& V, const FVector2D& W)
		{
			return (V.X - U.X) * (W.Y - U.Y) - (V.Y - U.Y) * (W.X - U.X);
		};
		const double D1 = Side(A, B, P), D2 = Side(B, C, P), D3 = Side(C, A, P);
		return !((D1 < 0 || D2 < 0 || D3 < 0) && (D1 > 0 || D2 > 0 || D3 > 0));
	}

	struct FGcSurfaceTriangles
	{
		TArray<FVector2D> Corners;

		explicit FGcSurfaceTriangles(const ARoadNetworkActor& Road)
		{
			Road.GetPresenter()->ForEachSurfaceTriangle([this](const FVector2D& A, const FVector2D& B, const FVector2D& C)
			{
				Corners.Add(A);
				Corners.Add(B);
				Corners.Add(C);
			});
		}

		bool Covers(const FVector2D& P) const
		{
			for (int32 Index = 0; Index + 2 < Corners.Num(); Index += 3)
			{
				if (GcOnTriangle(P, Corners[Index], Corners[Index + 1], Corners[Index + 2]))
				{
					return true;
				}
			}
			return false;
		}
	};

	/** One layer of 8 tufts/m² out to 40 m, 32 m cells, the engine cube as the only tuft - content-free. */
	FGroundCoverKit GcCubeKit()
	{
		FGroundCoverKit Kit;
		Kit.Tufts.Add(LoadObject<UStaticMesh>(nullptr, AirsidePrimitives::CubePath()));
		Kit.Layers.Add(GroundCover::FLayerSpec{ 8.0, 4000.0 });
		Kit.CellSizeUu = GcCell;
		return Kit;
	}

	/** StreamAround until the live set stops growing - the per-call build budget spreads it over calls. */
	void GcStreamFully(AAirsideGroundCoverActor& Grass, const FVector& Viewer)
	{
		for (int32 Pass = 0; Pass < 64; ++Pass)
		{
			const int32 Before = Grass.GetPresenter()->NumLiveCells();
			Grass.StreamAround(Viewer);
			if (Grass.GetPresenter()->NumLiveCells() == Before && Pass > 0)
			{
				return;
			}
		}
	}
}

// --- Solve ---------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverSameCellSameTufts, "Airside.Solve.GroundCover.SameCellSameTufts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverSameCellSameTufts::RunTest(const FString&)
{
	// DETERMINISM IS THE FEATURE: walk away and back, and the same grass is standing there.
	TArray<GroundCover::FTuft> First, Second;
	GroundCover::ScatterCell(FIntPoint(-3, 7), 0, GcCell, 8.0, 3, First);
	GroundCover::ScatterCell(FIntPoint(-3, 7), 0, GcCell, 8.0, 3, Second);
	TestTrue(TEXT("a cell has tufts"), First.Num() > 0);
	TestTrue(TEXT("the same cell and layer give identical tufts"), GcSameTufts(First, Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverLayersDiffer, "Airside.Solve.GroundCover.LayersDiffer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverLayersDiffer::RunTest(const FString&)
{
	// A NEARER LAYER ADDS NEW TUFTS: if layer 1 repeated layer 0, the "denser" ground near the
	// camera would be the same tufts drawn twice in the same places.
	TArray<GroundCover::FTuft> Zero, One, Neighbour;
	GroundCover::ScatterCell(FIntPoint(2, 2), 0, GcCell, 8.0, 3, Zero);
	GroundCover::ScatterCell(FIntPoint(2, 2), 1, GcCell, 8.0, 3, One);
	GroundCover::ScatterCell(FIntPoint(2, 3), 0, GcCell, 8.0, 3, Neighbour);
	TestFalse(TEXT("layer 1 of a cell is not layer 0"), GcSameTufts(Zero, One));
	TestTrue(TEXT("and its first tuft stands somewhere else"), Zero[0].Position != One[0].Position);
	TestTrue(TEXT("a neighbouring cell is not a translated copy"),
		(Zero[0].Position - FVector2D(2 * GcCell, 2 * GcCell)) != (Neighbour[0].Position - FVector2D(2 * GcCell, 3 * GcCell)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverDensityHolds, "Airside.Solve.GroundCover.DensityHolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverDensityHolds::RunTest(const FString&)
{
	// THE AUTHORED FIGURE IS WHAT IS DRAWN: 8 per m² over a 32 m cell is 8192 tufts, each cell.
	int32 Total = 0;
	for (int32 X = 0; X < 10; ++X)
	{
		for (int32 Y = 0; Y < 10; ++Y)
		{
			TArray<GroundCover::FTuft> Tufts;
			GroundCover::ScatterCell(FIntPoint(X, Y), 0, GcCell, 8.0, 3, Tufts);
			Total += Tufts.Num();
		}
	}
	const double Expected = 8.0 * 32.0 * 32.0 * 100.0;
	TestTrue(FString::Printf(TEXT("%d tufts over 100 cells is within 1%% of %.0f"), Total, Expected),
		FMath::Abs(Total - Expected) <= Expected * 0.01);
	TArray<GroundCover::FTuft> None;
	GroundCover::ScatterCell(FIntPoint(0, 0), 2, GcCell, 0.0, 3, None);
	TestEqual(TEXT("a zero-density layer scatters nothing"), None.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverTuftsStayInCell, "Airside.Solve.GroundCover.TuftsStayInCell",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverTuftsStayInCell::RunTest(const FString&)
{
	// A TUFT OUTSIDE ITS CELL would survive its cell's release and be drawn by a neighbour too.
	TArray<GroundCover::FTuft> Tufts;
	GroundCover::ScatterCell(FIntPoint(-1, -2), 0, GcCell, 8.0, 3, Tufts);
	int32 Outside = 0;
	int32 BadVariant = 0;
	for (const GroundCover::FTuft& Tuft : Tufts)
	{
		Outside += GroundCover::CellOf(Tuft.Position, GcCell) != FIntPoint(-1, -2) ? 1 : 0;
		BadVariant += (Tuft.Variant < 0 || Tuft.Variant >= 3) ? 1 : 0;
	}
	TestEqual(TEXT("every tuft lies in its own (negative) cell"), Outside, 0);
	TestEqual(TEXT("every variant names one of the three meshes"), BadVariant, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverNoCellsFromHigh, "Airside.Solve.GroundCover.NoCellsFromHigh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverNoCellsFromHigh::RunTest(const FString&)
{
	// "ONLY WHEN THE CAMERA IS LOW" HAS NO SWITCH: a viewer higher than the reach finds no cell.
	TArray<FIntPoint> Cells;
	GroundCover::CellsWithin(FVector(100.0, 100.0, 5000.0), 4000.0, GcCell, Cells);
	TestEqual(TEXT("a viewer 50 m up has no cells within 40 m"), Cells.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverCellsWithinRadius, "Airside.Solve.GroundCover.CellsWithinRadius",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverCellsWithinRadius::RunTest(const FString&)
{
	const FVector Viewer(1000.0, -500.0, 300.0);
	TArray<FIntPoint> Cells;
	GroundCover::CellsWithin(Viewer, 4000.0, GcCell, Cells);
	TestTrue(TEXT("the viewer's own cell is in"), Cells.Contains(GroundCover::CellOf(FVector2D(Viewer), GcCell)));
	int32 TooFar = 0;
	for (const FIntPoint& C : Cells)
	{
		TooFar += GroundCover::DistanceToCell(Viewer, C, GcCell) > 4000.0 ? 1 : 0;
	}
	TestEqual(TEXT("no returned cell is beyond the radius"), TooFar, 0);
	// A 40 m ring over 32 m cells: the 3x3 around the viewer at least, and nothing like a 9x9.
	TestTrue(FString::Printf(TEXT("%d cells is a handful, not a field"), Cells.Num()), Cells.Num() >= 4 && Cells.Num() <= 16);
	return true;
}

// --- Build ---------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverMaskTriangleCovers, "Airside.Build.GroundCoverMask.TriangleCovers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverMaskTriangleCovers::RunTest(const FString&)
{
	FGroundCoverMask Mask;
	TestFalse(TEXT("an empty mask covers nothing"), Mask.IsCovered(FVector2D(0.0, 0.0)));
	// CLOCKWISE on purpose: the builders' winding is not the mask's business.
	Mask.AddTriangle(FVector2D(0.0, 0.0), FVector2D(0.0, 1000.0), FVector2D(1000.0, 0.0));
	TestTrue(TEXT("inside is covered"), Mask.IsCovered(FVector2D(200.0, 200.0)));
	TestTrue(TEXT("a vertex is covered"), Mask.IsCovered(FVector2D(1000.0, 0.0)));
	TestFalse(TEXT("outside is not"), Mask.IsCovered(FVector2D(800.0, 800.0)));
	Mask.AddTriangle(FVector2D(5000.0, 0.0), FVector2D(6000.0, 0.0), FVector2D(7000.0, 0.0));
	TestEqual(TEXT("a zero-area triangle is dropped"), Mask.NumTriangles(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverMaskEdgeIsCovered, "Airside.Build.GroundCoverMask.EdgeIsCovered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverMaskEdgeIsCovered::RunTest(const FString&)
{
	// INCLUSIVE: a tuft centred exactly on a slab's edge would stand half on the slab.
	FGroundCoverMask Mask;
	Mask.AddTriangle(FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0), FVector2D(0.0, 1000.0));
	TestTrue(TEXT("the hypotenuse's midpoint is covered"), Mask.IsCovered(FVector2D(500.0, 500.0)));
	TestTrue(TEXT("a point on the base is covered"), Mask.IsCovered(FVector2D(300.0, 0.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverMaskOutsideLandIsCovered, "Airside.Build.GroundCoverMask.OutsideLandIsCovered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverMaskOutsideLandIsCovered::RunTest(const FString&)
{
	// THE DIORAMA EDGE (2026-10-02): a tuft past the cut stands on nothing and gives the edge away.
	FGroundCoverMask Mask;
	TestFalse(TEXT("with no land set, open ground far out is bare - every older map owns everything"),
		Mask.IsCovered(FVector2D(1.0e6, -1.0e6)));
	const FIntPoint Tiles[] = { FIntPoint(0, 3), FIntPoint(0, 4) };
	Mask.SetLand(FLandGrid::Make(FVector2D::ZeroVector, 60000.0, 8, 8, Tiles));
	TestFalse(TEXT("on owned land, open ground is bare"), Mask.IsCovered(FVector2D(30000.0, 210000.0)));
	TestTrue(TEXT("on an unowned tile it is covered"), Mask.IsCovered(FVector2D(90000.0, 210000.0)));
	TestTrue(TEXT("ON the cut is covered - a tuft centred there leans over it"), Mask.IsCovered(FVector2D(60000.0, 210000.0)));
	Mask.SetLand(FLandGrid());
	TestFalse(TEXT("an invalid grid owns everything again"), Mask.IsCovered(FVector2D(90000.0, 210000.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverMaskBucketBoundary, "Airside.Build.GroundCoverMask.BucketBoundary",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverMaskBucketBoundary::RunTest(const FString&)
{
	// A TRIANGLE ACROSS FOUR BUCKETS covers the corner they share, whichever bucket the query lands in.
	FGroundCoverMask Mask(1000.0);
	Mask.AddTriangle(FVector2D(-1500.0, -1500.0), FVector2D(1500.0, -1500.0), FVector2D(0.0, 1500.0));
	TestTrue(TEXT("the shared corner (0,0) is covered"), Mask.IsCovered(FVector2D(0.0, 0.0)));
	TestTrue(TEXT("a point on a bucket line is covered"), Mask.IsCovered(FVector2D(0.0, -1000.0)));
	TestTrue(TEXT("the triangle is filed in more than one bucket"), Mask.NumBuckets() >= 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverMaskPolygonMatchesTriangles, "Airside.Build.GroundCoverMask.PolygonMatchesTriangles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverMaskPolygonMatchesTriangles::RunTest(const FString&)
{
	// A PLOT OUTLINE AND THE SAME SQUARE AS TWO TRIANGLES keep the grass off the same ground.
	FGroundCoverMask ByOutline, ByTriangles;
	ByOutline.AddPolygon({ FVector2D(100.0, 100.0), FVector2D(2100.0, 100.0), FVector2D(2100.0, 2100.0), FVector2D(100.0, 2100.0) });
	ByTriangles.AddTriangle(FVector2D(100.0, 100.0), FVector2D(2100.0, 100.0), FVector2D(2100.0, 2100.0));
	ByTriangles.AddTriangle(FVector2D(100.0, 100.0), FVector2D(2100.0, 2100.0), FVector2D(100.0, 2100.0));
	int32 Disagree = 0;
	int32 Covered = 0;
	for (int32 X = 0; X < 50; ++X)
	{
		for (int32 Y = 0; Y < 50; ++Y)
		{
			// Sampled off the edges: the outline's winding test is not edge-exact (RoadGeom's own comment).
			const FVector2D P(X * 50.0 + 13.0, Y * 50.0 + 17.0);
			const bool bOutline = ByOutline.IsCovered(P);
			Covered += bOutline ? 1 : 0;
			Disagree += bOutline != ByTriangles.IsCovered(P) ? 1 : 0;
		}
	}
	TestTrue(TEXT("the square covers some samples"), Covered > 0);
	TestEqual(TEXT("outline and triangles agree everywhere"), Disagree, 0);
	return true;
}

// --- Content -------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverResolveConvertsMetres, "Airside.Content.ResolveGroundCoverConvertsMetres",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverResolveConvertsMetres::RunTest(const FString&)
{
	const UAirsideContent* Content = UAirsideSettings::GetContent();
	if (!TestNotNull(TEXT("the content set resolves"), Content))
	{
		return false;
	}
	const FGroundCoverKit Kit = UAirsideSettings::ResolveGroundCover();
	if (!TestEqual(TEXT("one resolved layer per authored layer"), Kit.Layers.Num(), Content->GroundCoverLayers.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < Kit.Layers.Num(); ++Index)
	{
		TestEqual(TEXT("ShowWithin is metres x 100"), Kit.Layers[Index].ShowWithinUu,
			double(Content->GroundCoverLayers[Index].ShowWithinMetres) * 100.0);
	}
	TestEqual(TEXT("the cell is metres x 100"), Kit.CellSizeUu, double(Content->GroundCoverCellMetres) * 100.0);
	return true;
}

// --- Present (through the composition) -----------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverTuftsBesideNotOnSurface, "Airside.Present.GroundCover.TuftsBesideNotOnSurface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverTuftsBesideNotOnSurface::RunTest(const FString&)
{
	FAirsideTestWorld Fixture;
	ARoadNetworkActor* Road = Fixture.Actor;
	const int32 W = Road->PlaceNode(FVector2D(0.0, 0.0));
	const int32 E = Road->PlaceNode(FVector2D(60000.0, 0.0));
	if (!TestTrue(TEXT("a taxiway is drawn"), Road->ConnectNodes(W, E, ERoadKind::Taxiway)))
	{
		return false;
	}
	AAirsideGroundCoverActor* Grass = Fixture.World->SpawnActor<AAirsideGroundCoverActor>();
	Grass->SetKit(GcCubeKit());
	Grass->BindTo(Road);
	GcStreamFully(*Grass, FVector(30000.0, 0.0, 300.0));

	const FGcSurfaceTriangles Surface(*Road);
	TestTrue(TEXT("the surface has triangles to keep off"), Surface.Corners.Num() > 0);
	int32 Count = 0, OnSurface = 0, NearEdge = 0;
	Grass->GetPresenter()->ForEachInstanceLocation([&](int32, const FVector& Location)
	{
		++Count;
		const FVector2D P(Location.X, Location.Y);
		if (Surface.Covers(P))
		{
			++OnSurface;
		}
		// WITHIN 1 M OF THE SLAB: the grass reaches the edge, which is what hides the lip.
		else if (Surface.Covers(P + FVector2D(0.0, 100.0)) || Surface.Covers(P - FVector2D(0.0, 100.0)))
		{
			++NearEdge;
		}
	});
	TestTrue(FString::Printf(TEXT("%d tufts stand beside the taxiway"), Count), Count > 1000);
	TestEqual(TEXT("NO tuft stands on the taxiway (checked against its own triangles, not the mask)"), OnSurface, 0);
	TestTrue(FString::Printf(TEXT("%d tufts stand within 1 m of its edge"), NearEdge), NearEdge > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverNewSurfaceClearsItsTufts, "Airside.Present.GroundCover.NewSurfaceClearsItsTufts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverNewSurfaceClearsItsTufts::RunTest(const FString&)
{
	// THE SEAM: OnNetworkChanged -> mask -> refill. Unbound, the live cells keep their old tufts
	// and this goes red - the road would be drawn with grass through it.
	FAirsideTestWorld Fixture;
	ARoadNetworkActor* Road = Fixture.Actor;
	AAirsideGroundCoverActor* Grass = Fixture.World->SpawnActor<AAirsideGroundCoverActor>();
	Grass->SetKit(GcCubeKit());
	Grass->BindTo(Road);
	GcStreamFully(*Grass, FVector(1000.0, 0.0, 300.0));
	TArray<FVector2D> Before;
	Grass->GetPresenter()->ForEachInstanceLocation([&](int32, const FVector& L) { Before.Add(FVector2D(L.X, L.Y)); });

	const int32 W = Road->PlaceNode(FVector2D(-3000.0, 0.0));
	const int32 E = Road->PlaceNode(FVector2D(5000.0, 0.0));
	if (!TestTrue(TEXT("a taxiway is drawn over the grass"), Road->ConnectNodes(W, E, ERoadKind::Taxiway)))
	{
		return false;
	}
	const FGcSurfaceTriangles Surface(*Road);
	int32 WasUnder = 0;
	for (const FVector2D& P : Before)
	{
		WasUnder += Surface.Covers(P) ? 1 : 0;
	}
	int32 StillUnder = 0;
	Grass->GetPresenter()->ForEachInstanceLocation([&](int32, const FVector& L)
	{
		StillUnder += Surface.Covers(FVector2D(L.X, L.Y)) ? 1 : 0;
	});
	TestTrue(FString::Printf(TEXT("%d tufts stood where the taxiway went"), WasUnder), WasUnder > 0);
	TestEqual(TEXT("none stands there after the change - with no further stream"), StillUnder, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverMaxLayersZeroDrawsNothing, "Airside.Present.GroundCover.MaxLayersZeroDrawsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverMaxLayersZeroDrawsNothing::RunTest(const FString&)
{
	// GRAPHICS LOW: no grass, and no work to have none.
	FAirsideTestWorld Fixture;
	AAirsideGroundCoverActor* Grass = Fixture.World->SpawnActor<AAirsideGroundCoverActor>();
	Grass->SetKit(GcCubeKit());
	Grass->BindTo(Fixture.Actor);
	GcStreamFully(*Grass, FVector(0.0, 0.0, 300.0));
	TestTrue(TEXT("at full quality cells are live"), Grass->GetPresenter()->NumLiveCells() > 0);
	Grass->GetPresenter()->SetMaxLayers(0);
	TestEqual(TEXT("quality Low releases every cell"), Grass->GetPresenter()->NumLiveCells(), 0);
	GcStreamFully(*Grass, FVector(0.0, 0.0, 300.0));
	TestEqual(TEXT("and streams none back"), Grass->GetPresenter()->NumLiveCells(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverNoTuftPastTheOwnedLand, "Airside.Present.GroundCover.NoTuftPastTheOwnedLand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverNoTuftPastTheOwnedLand::RunTest(const FString&)
{
	// THE SEAM, through the composition: the grass actor must read the airport's land and HEAR it change. Unwired,
	// the mask test above still passes and tufts float past the cut (the report that started this, 2026-10-02), or
	// a bought tile stays bald. Column 0 spans X [-30000, 30000), rows 3-4 span Y [-60000, 60000): the viewer stands ON
	// the east cut at X = 30000, so grass streams on both sides of it (it grows within ~40 m of the camera only - a
	// viewer at the origin, 300 m off, saw no cut at all and passed for nothing).
	FAirsideTestWorld Fixture;
	const FIntPoint Start[] = { FIntPoint(0, 3), FIntPoint(0, 4) };
	const FLandGrid Land = FLandGrid::Make(FVector2D(-30000.0, -240000.0), 60000.0, 8, 8, Start);
	Fixture.Actor->GetEditFacade()->AuthorOwnedLand(Land);

	AAirsideGroundCoverActor* Grass = Fixture.World->SpawnActor<AAirsideGroundCoverActor>();
	Grass->SetKit(GcCubeKit());
	Grass->BindTo(Fixture.Actor);
	GcStreamFully(*Grass, FVector(30000.0, 10000.0, 300.0));

	int32 Owned = 0, Past = 0;
	Grass->GetPresenter()->ForEachInstanceLocation([&](int32, const FVector& Location)
	{
		(Land.IsOwned(FVector2D(Location)) ? Owned : Past) += 1;
	});
	TestTrue(FString::Printf(TEXT("%d tufts grow on owned land"), Owned), Owned > 100);
	TestEqual(TEXT("NO tuft grows past the cut"), Past, 0);

	// A PURCHASE east of the cut: the grass hears it and grows there.
	FLandGrid Grown = Land;
	Grown.SetTileOwned(FIntPoint(1, 4), true);
	Fixture.Actor->GetEditFacade()->AuthorOwnedLand(Grown);
	GcStreamFully(*Grass, FVector(30000.0, 10000.0, 300.0));
	int32 East = 0;
	Grass->GetPresenter()->ForEachInstanceLocation([&](int32, const FVector& Location)
	{
		East += Location.X > 30000.0 && Location.Y > 0.0 ? 1 : 0;
	});
	TestTrue(FString::Printf(TEXT("%d tufts grow on the bought tile"), East), East > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverEmptyKitIsIdle, "Airside.Present.GroundCover.EmptyKitIsIdle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverEmptyKitIsIdle::RunTest(const FString&)
{
	// A CONTENT SET WITH NO TUFTS (a fresh checkout before build_grass_content.py) draws nothing, quietly.
	FAirsideTestWorld Fixture;
	AAirsideGroundCoverActor* Grass = Fixture.World->SpawnActor<AAirsideGroundCoverActor>();
	Grass->SetKit(FGroundCoverKit());
	Grass->BindTo(Fixture.Actor);
	GcStreamFully(*Grass, FVector(0.0, 0.0, 300.0));
	TestEqual(TEXT("no cells"), Grass->GetPresenter()->NumLiveCells(), 0);
	TestEqual(TEXT("no components made"), Grass->GetPresenter()->NumComponents(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGroundCoverSubsystemSpawnsOnBeginPlay, "Airside.Present.GroundCover.SubsystemSpawnsOnBeginPlay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FGroundCoverSubsystemSpawnsOnBeginPlay::RunTest(const FString&)
{
	// THE WIRING: nothing else creates the grass actor. A game world that begins play with an
	// airport gets one, bound to that airport; before begin play there is none.
	FAirsideTestWorld Fixture;
	UGroundCoverSubsystem* Subsystem = Fixture.World->GetSubsystem<UGroundCoverSubsystem>();
	if (!TestNotNull(TEXT("a game world has the subsystem"), Subsystem))
	{
		return false;
	}
	TestNull(TEXT("no grass actor before begin play"), UGroundCoverSubsystem::FindActor(Fixture.World));
	Subsystem->OnWorldBeginPlay(*Fixture.World);
	const AAirsideGroundCoverActor* Grass = UGroundCoverSubsystem::FindActor(Fixture.World);
	if (!TestNotNull(TEXT("begin play spawns one"), Grass))
	{
		return false;
	}
	TestTrue(TEXT("bound to the world's airport"), Grass->GetRoadNetwork() == Fixture.Actor);
	TestTrue(TEXT("and never saved with the level"), Grass->HasAnyFlags(RF_Transient));
	return true;
}

#endif
