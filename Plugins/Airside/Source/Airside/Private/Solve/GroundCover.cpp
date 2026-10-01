#include "Solve/GroundCover.h"

namespace
{
	/**
	 * SplitMix64. NOT FRandomStream: Solve/ includes CoreMinimal.h alone (Check-Architecture
	 * rule 1), and a seedable 64-bit generator is eight lines - and its sequence is ours, so an
	 * engine change cannot reshuffle every field of grass in the game.
	 */
	uint64 Next(uint64& State)
	{
		uint64 Z = (State += 0x9E3779B97F4A7C15ull);
		Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
		Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
		return Z ^ (Z >> 31);
	}

	/** Uniform in [0, 1): the top 53 bits as a double's mantissa. */
	double Unit(uint64& State)
	{
		return double(Next(State) >> 11) * (1.0 / 9007199254740992.0);
	}

	/** One seed per (cell, layer). The layer term is odd-multiplied so layer 1 of a cell is not layer 0 of a neighbour. */
	uint64 SeedOf(const FIntPoint& Cell, int32 Layer)
	{
		const uint64 Packed = (uint64(uint32(Cell.X)) << 32) | uint64(uint32(Cell.Y));
		return Packed ^ (uint64(Layer + 1) * 0xD1B54A32D192ED03ull);
	}
}

FIntPoint GroundCover::CellOf(const FVector2D& Point, double CellSizeUu)
{
	return FIntPoint(FMath::FloorToInt32(Point.X / CellSizeUu), FMath::FloorToInt32(Point.Y / CellSizeUu));
}

double GroundCover::DistanceToCell(const FVector& Viewer, const FIntPoint& Cell, double CellSizeUu)
{
	const double MinX = Cell.X * CellSizeUu;
	const double MinY = Cell.Y * CellSizeUu;
	const double Dx = Viewer.X - FMath::Clamp(Viewer.X, MinX, MinX + CellSizeUu);
	const double Dy = Viewer.Y - FMath::Clamp(Viewer.Y, MinY, MinY + CellSizeUu);
	return FMath::Sqrt(Dx * Dx + Dy * Dy + Viewer.Z * Viewer.Z);
}

void GroundCover::CellsWithin(const FVector& Viewer, double RadiusUu, double CellSizeUu, TArray<FIntPoint>& Out)
{
	Out.Reset();
	const double Height = FMath::Abs(Viewer.Z);
	if (CellSizeUu <= 0.0 || RadiusUu <= Height)
	{
		return;
	}
	// The ground circle the sphere of RadiusUu cuts: no cell outside its bounding square can qualify.
	const double Reach = FMath::Sqrt(RadiusUu * RadiusUu - Height * Height);
	const FIntPoint Lo = CellOf(FVector2D(Viewer.X - Reach, Viewer.Y - Reach), CellSizeUu);
	const FIntPoint Hi = CellOf(FVector2D(Viewer.X + Reach, Viewer.Y + Reach), CellSizeUu);
	for (int32 X = Lo.X; X <= Hi.X; ++X)
	{
		for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
		{
			if (DistanceToCell(Viewer, FIntPoint(X, Y), CellSizeUu) <= RadiusUu)
			{
				Out.Add(FIntPoint(X, Y));
			}
		}
	}
}

void GroundCover::ScatterCell(const FIntPoint& Cell, int32 Layer, double CellSizeUu, double TuftsPerSquareMetre,
	int32 NumVariants, TArray<FTuft>& Out)
{
	const double SideMetres = CellSizeUu / 100.0;
	const int32 Count = FMath::RoundToInt32(FMath::Max(TuftsPerSquareMetre, 0.0) * SideMetres * SideMetres);
	if (Count <= 0 || NumVariants <= 0)
	{
		return;
	}
	uint64 State = SeedOf(Cell, Layer);
	const FVector2D Origin(Cell.X * CellSizeUu, Cell.Y * CellSizeUu);
	Out.Reserve(Out.Num() + Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FTuft Tuft;
		Tuft.Position = Origin + FVector2D(Unit(State) * CellSizeUu, Unit(State) * CellSizeUu);
		Tuft.YawDegrees = float(Unit(State) * 360.0);
		// +-20 %: enough that neighbours differ, not so much that a tuft reads as a shrub.
		Tuft.Scale = float(0.8 + Unit(State) * 0.4);
		Tuft.Variant = FMath::Min(int32(Unit(State) * NumVariants), NumVariants - 1);
		Out.Add(Tuft);
	}
}
