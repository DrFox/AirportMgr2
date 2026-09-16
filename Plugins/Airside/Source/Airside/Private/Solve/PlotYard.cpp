#include "Solve/PlotYard.h"

#include "Solve/PlotFit.h"
#include "Solve/RoadGeom.h"

namespace
{
	/** The inward normal of the frontage: which way is INTO the plot. */
	FVector2D InwardOf(TArrayView<const FVector2D> Outline, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D Along = (B - A).GetSafeNormal();
		const FVector2D Left = RoadGeom::PerpCCW(Along);

		// Read off the polygon's winding rather than assumed counter-clockwise, exactly as
		// FitBays does: a plot stored the other way round would otherwise aim every module
		// out of the plot and across the road.
		return RoadGeom::PolygonArea(Outline) > 0.0 ? Left : -Left;
	}
}

void PlotYard::StandCorners(const FStand& Stand, const FFootprint& Footprint,
	TArray<FVector2D>& OutCorners)
{
	OutCorners.Reset();

	// INSET, and the inset is load-bearing rather than a fudge - see PlotFit::CornerInsetUu.
	// A stand flush against the outline puts its corner exactly ON the boundary, where a
	// containment test answers by floating-point coin flip and differently on another machine.
	const double HalfLength = FMath::Max(Footprint.LengthUu * 0.5 - PlotFit::CornerInsetUu, 0.0);
	const double HalfWidth = FMath::Max(Footprint.WidthUu * 0.5 - PlotFit::CornerInsetUu, 0.0);

	const FVector2D Forward(FMath::Cos(Stand.Heading), FMath::Sin(Stand.Heading));
	const FVector2D Side = RoadGeom::PerpCCW(Forward);

	OutCorners.Add(Stand.Centre + Forward * HalfLength + Side * HalfWidth);
	OutCorners.Add(Stand.Centre + Forward * HalfLength - Side * HalfWidth);
	OutCorners.Add(Stand.Centre - Forward * HalfLength - Side * HalfWidth);
	OutCorners.Add(Stand.Centre - Forward * HalfLength + Side * HalfWidth);
}

PlotYard::FYard PlotYard::LayOut(TArrayView<const FVector2D> Outline,
	FVector2D FrontageA, FVector2D FrontageB, FVector2D Gate,
	TArrayView<const FFootprint> Footprints, int32 Seed,
	const FFootprint& RoomForFootprint)
{
	FYard Yard;
	Yard.Stands.SetNum(Footprints.Num());

	if (Outline.Num() < 3)
	{
		return Yard;
	}

	const FVector2D Inward = InwardOf(Outline, FrontageA, FrontageB);
	const double InwardBearing = RoadGeom::Bearing(Inward);

	// The gate-fronting modules first: their pose is decided, not sampled, so they take
	// their ground before anything is allowed to sample into it.
	for (int32 Index = 0; Index < Footprints.Num(); ++Index)
	{
		if (!Footprints[Index].bFrontsTheGate)
		{
			continue;
		}

		FStand& Stand = Yard.Stands[Index];
		Stand.Heading = InwardBearing;
		Stand.Centre = Gate + Inward * (Footprints[Index].LengthUu * 0.5);
		Stand.bPlaced = true;
	}

	return Yard;
}
