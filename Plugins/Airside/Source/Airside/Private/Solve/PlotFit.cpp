#include "Solve/PlotFit.h"

#include "Solve/RoadGeom.h"

namespace
{
	/**
	 * Twice the signed area (the shoelace sum). Positive for a counter-clockwise polygon.
	 *
	 * Only its SIGN is used, so the factor of two is left in rather than divided out - a
	 * halving that no caller needs is a line that can only ever be wrong.
	 */
	double TwiceSignedArea(TArrayView<const FVector2D> Outline)
	{
		double Sum = 0.0;
		const int32 Count = Outline.Num();
		for (int32 I = 0; I < Count; ++I)
		{
			const FVector2D& A = Outline[I];
			const FVector2D& B = Outline[(I + 1) % Count];
			Sum += A.X * B.Y - B.X * A.Y;
		}
		return Sum;
	}

	/**
	 * Winding-number point-in-polygon.
	 *
	 * Winding number rather than the shorter even-odd ray cast, because a freeform gesture
	 * produces concave plots and even-odd disagrees with winding on self-touching outlines
	 * - and the draw tool only refuses edges that CROSS, not ones that meet at a point.
	 *
	 * UNDEFINED ON THE BOUNDARY, deliberately not papered over here. Callers probe from
	 * just inside; see PlotFit::CornerInsetUu for why that is the honest fix rather than an
	 * epsilon buried in this comparison.
	 */
	bool Contains(TArrayView<const FVector2D> Outline, const FVector2D& P)
	{
		int32 Winding = 0;
		const int32 Count = Outline.Num();
		for (int32 I = 0; I < Count; ++I)
		{
			const FVector2D& A = Outline[I];
			const FVector2D& B = Outline[(I + 1) % Count];

			// Which side of the directed edge A->B the point falls on. Positive is left.
			const double Side = (B.X - A.X) * (P.Y - A.Y) - (P.X - A.X) * (B.Y - A.Y);

			if (A.Y <= P.Y)
			{
				if (B.Y > P.Y && Side > 0.0) { ++Winding; }
			}
			else if (B.Y <= P.Y && Side < 0.0)
			{
				--Winding;
			}
		}
		return Winding != 0;
	}
}

PlotFit::FPlotGrid PlotFit::BuildGrid(FVector2D FrontageA, FVector2D FrontageB,
	int32 Width, int32 Depth)
{
	FPlotGrid Grid;
	if (Width <= 0 || Depth <= 0)
	{
		return Grid;
	}

	const FVector2D Along = FrontageB - FrontageA;
	const double Length = Along.Size();
	if (Length <= 0.0)
	{
		return Grid;
	}

	const FVector2D Unit = Along / Length;

	// Interior on the LEFT of A->B - the caller's convention, not a discovery. See the
	// header for why this differs from FitBays, which has to read it off a drawn polygon.
	const FVector2D Inward = RoadGeom::PerpCCW(Unit);
	const double Heading = RoadGeom::Bearing(Inward);

	Grid.Width = Width;
	Grid.Depth = Depth;
	Grid.Slots.Reserve(Width * Depth);

	// ROW-MAJOR, FRONT ROW FIRST. Row is the outer loop, so Slots[0 .. Width-1] is the row
	// on the road - the contract FPlotGrid states and the presenter relies on.
	for (int32 Row = 0; Row < Depth; ++Row)
	{
		for (int32 Bay = 0; Bay < Width; ++Bay)
		{
			FPlotBay Slot;
			Slot.Centre = FrontageA
				+ Unit * ((static_cast<double>(Bay) + 0.5) * BayWidthUu)
				+ Inward * ((static_cast<double>(Row) + 0.5) * BayDepthUu);
			Slot.Heading = Heading;
			Grid.Slots.Add(Slot);
		}
	}
	return Grid;
}

TArray<FVector2D> PlotFit::GridOutline(FVector2D FrontageA, FVector2D FrontageB,
	int32 Width, int32 Depth)
{
	TArray<FVector2D> Outline;
	if (Width <= 0 || Depth <= 0)
	{
		return Outline;
	}

	const FVector2D Along = FrontageB - FrontageA;
	const double Length = Along.Size();
	if (Length <= 0.0)
	{
		return Outline;
	}

	const FVector2D Unit = Along / Length;
	const FVector2D Inward = RoadGeom::PerpCCW(Unit);

	// THE GRID'S OWN EXTENT, not the frontage edge that was passed in. A->B is the edge the
	// gesture snapped to and may be any length; the plot is exactly Width bays wide, so
	// measuring the boundary from the bay count is what keeps the outline and the slots
	// describing the same rectangle.
	const FVector2D Front = Unit * (static_cast<double>(Width) * BayWidthUu);
	const FVector2D Back = Inward * (static_cast<double>(Depth) * BayDepthUu);

	// A -> A+Front -> A+Front+Back -> A+Back. Walking the frontage first and THEN turning
	// inward is what makes this counter-clockwise, given Inward is the left normal.
	Outline.Add(FrontageA);
	Outline.Add(FrontageA + Front);
	Outline.Add(FrontageA + Front + Back);
	Outline.Add(FrontageA + Back);
	return Outline;
}

PlotFit::FPlotFit PlotFit::FitBays(TArrayView<const FVector2D> Outline,
	FVector2D FrontageA, FVector2D FrontageB)
{
	FPlotFit Result;

	if (Outline.Num() < 3)
	{
		Result.Why = EPlotRefusal::TooSmall;
		return Result;
	}

	const FVector2D Along = FrontageB - FrontageA;
	const double Length = Along.Size();
	if (Length < BayWidthUu)
	{
		// Not enough road frontage for a single bay. TooSmall rather than NoFrontage: an
		// edge WAS found, it is just too short, and telling the player "no road" when they
		// drew along one would send them looking for the wrong problem.
		Result.Why = EPlotRefusal::TooSmall;
		return Result;
	}

	const FVector2D Unit = Along / Length;

	// The interior is on the LEFT of a boundary edge walked in winding order - but only for
	// a counter-clockwise polygon. Derived from the signed area rather than assumed, so a
	// plot whose clicks ran clockwise aims its bays into the plot instead of across the road.
	const FVector2D LeftNormal = RoadGeom::PerpCCW(Unit);
	const FVector2D Inward = TwiceSignedArea(Outline) > 0.0 ? LeftNormal : -LeftNormal;

	// HEADING IS INWARD, not along the frontage: +X faces away from the road.
	const double Heading = RoadGeom::Bearing(Inward);

	const int32 BayCount = FMath::FloorToInt(Length / BayWidthUu);

	const FVector2D HalfAlong = Unit * (BayWidthUu * 0.5 - CornerInsetUu);
	const FVector2D HalfDeep = Inward * (BayDepthUu * 0.5 - CornerInsetUu);

	for (int32 I = 0; I < BayCount; ++I)
	{
		const double AlongOffset = (static_cast<double>(I) + 0.5) * BayWidthUu;
		const FVector2D Centre = FrontageA + Unit * AlongOffset + Inward * (BayDepthUu * 0.5);

		// EVERY CORNER, not merely the centre. A centre-only test accepts a bay hanging out
		// of a notch in a concave plot, and the player watches a shed stand on the grass.
		// Concave plots are what a freeform gesture produces, so this is the ordinary case.
		const bool bInside =
			Contains(Outline, Centre + HalfAlong + HalfDeep) &&
			Contains(Outline, Centre + HalfAlong - HalfDeep) &&
			Contains(Outline, Centre - HalfAlong + HalfDeep) &&
			Contains(Outline, Centre - HalfAlong - HalfDeep);

		if (!bInside)
		{
			// SKIPPED, not abandoned. A notch in the middle of a wide plot costs the bay
			// over it and leaves the ones to either side standing, which is what the player
			// drew; refusing the whole plot would be a solver overruling a legal shape.
			continue;
		}

		FPlotBay Bay;
		Bay.Centre = Centre;
		Bay.Heading = Heading;
		Result.Bays.Add(Bay);
	}

	Result.bFits = Result.Bays.Num() > 0;
	if (!Result.bFits)
	{
		// Frontage was long enough, so this is a plot too shallow for a bay to stand in, or
		// one notched away to nothing. Same refusal either way - the player's fix is the
		// same, which is to draw it bigger.
		Result.Why = EPlotRefusal::TooSmall;
	}
	return Result;
}
