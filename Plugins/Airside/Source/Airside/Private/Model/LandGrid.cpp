#include "Model/LandGrid.h"

FLandGrid FLandGrid::Make(FVector2D InOrigin, double InTileSize, int32 InColumns, int32 InRows, TConstArrayView<FIntPoint> Start)
{
	FLandGrid Grid;
	Grid.Origin = InOrigin;
	Grid.TileSize = InTileSize;
	Grid.Columns = InColumns;
	Grid.Rows = InRows;
	for (const FIntPoint& Tile : Start)
	{
		Grid.SetTileOwned(Tile, true);
	}
	return Grid;
}

bool FLandGrid::IsValid() const
{
	return TileSize > 0.0 && Columns > 0 && Rows > 0 && Columns * Rows <= MaxTiles && Owned != 0;
}

bool FLandGrid::IsTileOwned(FIntPoint Tile) const
{
	return IsOnGrid(Tile) && ((Owned >> BitOf(Tile)) & 1ull) != 0;
}

void FLandGrid::SetTileOwned(FIntPoint Tile, bool bOwned)
{
	if (!IsOnGrid(Tile) || Columns * Rows > MaxTiles)
	{
		return;
	}
	const uint64 Bit = 1ull << BitOf(Tile);
	Owned = bOwned ? (Owned | Bit) : (Owned & ~Bit);
}

int32 FLandGrid::NumOwned() const
{
	return FMath::CountBits(Owned);
}

bool FLandGrid::IsBuyable(FIntPoint Tile) const
{
	if (!IsOnGrid(Tile) || IsTileOwned(Tile))
	{
		return false;
	}
	return IsTileOwned(Tile + FIntPoint(1, 0)) || IsTileOwned(Tile - FIntPoint(1, 0))
		|| IsTileOwned(Tile + FIntPoint(0, 1)) || IsTileOwned(Tile - FIntPoint(0, 1));
}

FIntPoint FLandGrid::TileAt(FVector2D Point) const
{
	return FIntPoint(
		FMath::FloorToInt32((Point.X - Origin.X) / TileSize),
		FMath::FloorToInt32((Point.Y - Origin.Y) / TileSize));
}

FBox2D FLandGrid::TileBox(FIntPoint Tile) const
{
	const FVector2D Min = Origin + FVector2D(Tile.X, Tile.Y) * TileSize;
	return FBox2D(Min, Min + FVector2D(TileSize, TileSize));
}

bool FLandGrid::IsOwned(FVector2D Point) const
{
	if (!IsValid())
	{
		return true;
	}
	// Every tile the point touches: one, or two on an edge, or four at a corner.
	const double FX = (Point.X - Origin.X) / TileSize;
	const double FY = (Point.Y - Origin.Y) / TileSize;
	const int32 X = FMath::FloorToInt32(FX);
	const int32 Y = FMath::FloorToInt32(FY);
	const bool bOnX = FX == double(X);
	const bool bOnY = FY == double(Y);
	for (int32 DX = bOnX ? -1 : 0; DX <= 0; ++DX)
	{
		for (int32 DY = bOnY ? -1 : 0; DY <= 0; ++DY)
		{
			if (!IsTileOwned(FIntPoint(X + DX, Y + DY)))
			{
				return false;
			}
		}
	}
	return true;
}

FVector2D FLandGrid::ClampToOwned(FVector2D Point) const
{
	if (!IsValid())
	{
		return Point;
	}
	// A linear walk of at most MaxTiles boxes - 64, fixed by the mask type (2026-10-02).
	FVector2D Best = Point;
	double BestDistSq = TNumericLimits<double>::Max();
	for (int32 Y = 0; Y < Rows; ++Y)
	{
		for (int32 X = 0; X < Columns; ++X)
		{
			if (!IsTileOwned(FIntPoint(X, Y)))
			{
				continue;
			}
			const FBox2D Box = TileBox(FIntPoint(X, Y));
			const FVector2D Clamped(FMath::Clamp(Point.X, Box.Min.X, Box.Max.X), FMath::Clamp(Point.Y, Box.Min.Y, Box.Max.Y));
			const double DistSq = FVector2D::DistSquared(Point, Clamped);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Best = Clamped;
			}
		}
	}
	return Best;
}

TArray<FLandEdgeRun> FLandGrid::Outline() const
{
	TArray<FLandEdgeRun> Runs;
	if (!IsValid())
	{
		return Runs;
	}
	// Unit edges first, keyed by (side, line, start) so collinear neighbours meet in order. Side 0..3 =
	// -X, +X, -Y, +Y; the line is the tile coordinate of the boundary, the start the tile along it.
	struct FUnit { int32 Side; int32 Line; int32 Along; };
	TArray<FUnit> Units;
	const FIntPoint Steps[4] = { FIntPoint(-1, 0), FIntPoint(1, 0), FIntPoint(0, -1), FIntPoint(0, 1) };
	for (int32 Y = 0; Y < Rows; ++Y)
	{
		for (int32 X = 0; X < Columns; ++X)
		{
			if (!IsTileOwned(FIntPoint(X, Y)))
			{
				continue;
			}
			for (int32 Side = 0; Side < 4; ++Side)
			{
				if (IsTileOwned(FIntPoint(X, Y) + Steps[Side]))
				{
					continue;
				}
				const bool bAlongY = Side < 2;   // an X-facing side runs along Y
				const int32 Line = bAlongY ? X + (Side == 1 ? 1 : 0) : Y + (Side == 3 ? 1 : 0);
				Units.Add({ Side, Line, bAlongY ? Y : X });
			}
		}
	}
	Units.Sort([](const FUnit& L, const FUnit& R)
	{
		return L.Side != R.Side ? L.Side < R.Side : L.Line != R.Line ? L.Line < R.Line : L.Along < R.Along;
	});
	for (int32 I = 0; I < Units.Num();)
	{
		int32 J = I + 1;
		while (J < Units.Num() && Units[J].Side == Units[I].Side && Units[J].Line == Units[I].Line
			&& Units[J].Along == Units[J - 1].Along + 1)
		{
			++J;
		}
		const FUnit& U = Units[I];
		const bool bAlongY = U.Side < 2;
		const int32 End = Units[J - 1].Along + 1;
		FLandEdgeRun Run;
		Run.A = Origin + (bAlongY ? FVector2D(U.Line, U.Along) : FVector2D(U.Along, U.Line)) * TileSize;
		Run.B = Origin + (bAlongY ? FVector2D(U.Line, End) : FVector2D(End, U.Line)) * TileSize;
		Run.Outward = FVector2D(Steps[U.Side].X, Steps[U.Side].Y);
		Runs.Add(Run);
		I = J;
	}
	return Runs;
}
