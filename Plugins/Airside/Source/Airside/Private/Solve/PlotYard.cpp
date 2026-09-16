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

	/** The outline's axis-aligned bounds, which is where candidate points are drawn from. */
	void BoundsOf(TArrayView<const FVector2D> Outline, FVector2D& OutMin, FVector2D& OutMax)
	{
		OutMin = FVector2D(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
		OutMax = FVector2D(-TNumericLimits<double>::Max(), -TNumericLimits<double>::Max());
		for (const FVector2D& P : Outline)
		{
			OutMin.X = FMath::Min(OutMin.X, P.X);
			OutMin.Y = FMath::Min(OutMin.Y, P.Y);
			OutMax.X = FMath::Max(OutMax.X, P.X);
			OutMax.Y = FMath::Max(OutMax.Y, P.Y);
		}
	}

	/** Do these two convex quads intersect? Separating axis; four axes for two rectangles. */
	bool QuadsIntersect(TArrayView<const FVector2D> A, TArrayView<const FVector2D> B)
	{
		const FVector2D Axes[] = {
			(A[1] - A[0]).GetSafeNormal(), (A[3] - A[0]).GetSafeNormal(),
			(B[1] - B[0]).GetSafeNormal(), (B[3] - B[0]).GetSafeNormal() };

		for (const FVector2D& Axis : Axes)
		{
			double MinA = TNumericLimits<double>::Max();
			double MaxA = -TNumericLimits<double>::Max();
			double MinB = TNumericLimits<double>::Max();
			double MaxB = -TNumericLimits<double>::Max();
			for (const FVector2D& P : A)
			{
				const double D = FVector2D::DotProduct(P, Axis);
				MinA = FMath::Min(MinA, D);
				MaxA = FMath::Max(MaxA, D);
			}
			for (const FVector2D& P : B)
			{
				const double D = FVector2D::DotProduct(P, Axis);
				MinB = FMath::Min(MinB, D);
				MaxB = FMath::Max(MaxB, D);
			}
			if (MaxA < MinB || MaxB < MinA)
			{
				// AN AXIS SEPARATES THEM, which is proof rather than evidence: two convex
				// shapes miss each other if and only if one exists. A bounding-box test
				// would refuse legal poses and accept illegal ones by turns, now that these
				// rectangles are not axis-aligned.
				return false;
			}
		}
		return true;
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

	// PLACED FOOTPRINTS GROW AS WE GO, and a candidate is tested against every one already
	// standing - including the shed, which took its ground first for exactly this reason.
	TArray<TArray<FVector2D>> Taken;
	TArray<FVector2D> Corners;
	for (int32 Index = 0; Index < Footprints.Num(); ++Index)
	{
		if (Yard.Stands[Index].bPlaced)
		{
			StandCorners(Yard.Stands[Index], Footprints[Index], Corners);
			Taken.Add(Corners);
		}
	}

	FVector2D Min = FVector2D::ZeroVector;
	FVector2D Max = FVector2D::ZeroVector;
	BoundsOf(Outline, Min, Max);

	FRandomStream Stream(Seed);

	// Grown by ClearanceUu on every side, so the gap between two modules is enforced by the
	// same test that stops them intersecting. Two modules that merely touch read as one
	// building, which is the stamped look this file exists to remove.
	auto Padded = [](const FFootprint& Footprint)
	{
		FFootprint Out = Footprint;
		Out.LengthUu += ClearanceUu;
		Out.WidthUu += ClearanceUu;
		return Out;
	};

	const FVector2D Across = RoadGeom::PerpCCW(Inward);

	auto TryPlace = [&](const FFootprint& Footprint, FStand& OutStand) -> bool
	{
		const FFootprint Grown = Padded(Footprint);

		const double HalfLength = Grown.LengthUu * 0.5;
		const double HalfWidth = Grown.WidthUu * 0.5;

		for (int32 Try = 0; Try < MaxTries; ++Try)
		{
			FStand Candidate;

			// THE HEADING IS CHOSEN FIRST, so the centre can be drawn from the band that
			// heading can actually occupy. Sampling the centre across the whole bounding box
			// and rejecting afterwards put most candidates half through the fence before any
			// other test ran - on a narrow plot that is most of the draw wasted.
			//
			// A QUARTER TURN PLUS A FEW DEGREES. Uniform over a circle reads as debris after
			// an explosion; this reads as something parked in a hurry.
			Candidate.Heading = InwardBearing
				+ Stream.RandRange(0, 3) * UE_DOUBLE_HALF_PI
				+ Stream.FRandRange(-HeadingJitterRadians, HeadingJitterRadians);

			// This heading's own axis-aligned reach - EXACT, not the half-diagonal. The
			// diagonal holds for every rotation and so refuses poses a 12 degree turn allows,
			// which quietly costs the yard modules it had room for.
			const double Cos = FMath::Abs(FMath::Cos(Candidate.Heading));
			const double Sin = FMath::Abs(FMath::Sin(Candidate.Heading));
			const double ReachX = HalfLength * Cos + HalfWidth * Sin;
			const double ReachY = HalfLength * Sin + HalfWidth * Cos;

			if (Min.X + ReachX > Max.X - ReachX || Min.Y + ReachY > Max.Y - ReachY)
			{
				// No centre exists for this heading. Another turn may still fit, so this
				// costs a try rather than abandoning the module.
				continue;
			}

			Candidate.Centre = FVector2D(
				Stream.FRandRange(Min.X + ReachX, Max.X - ReachX),
				Stream.FRandRange(Min.Y + ReachY, Max.Y - ReachY));

			StandCorners(Candidate, Grown, Corners);

			bool bLegal = true;
			for (const FVector2D& Corner : Corners)
			{
				if (!RoadGeom::PointInPolygon(Outline, Corner))
				{
					bLegal = false;
					break;
				}

				// The corridor from the gate into the plot: a BOX at the gate mouth, one
				// truck length deep, not a lane running the full depth of the yard.
				//
				// THE DEPTH BOUND IS LOAD-BEARING, not tidiness. Unbounded, the lane cuts
				// the plot in two and forbids anything crossing the middle - which took
				// acceptance to roughly 8% a try, so 24 tries dropped a module about one
				// time in eight and Airside.Solve.PlotYardLeavesTheGateClear caught it on
				// seed 3. It is also simply the wrong rule: the truck needs room to get off
				// the pad and turn, and once it is clear of the fence it can weave.
				const FVector2D FromGate = Corner - Gate;
				const double Into = FVector2D::DotProduct(FromGate, Inward);
				if (Into >= 0.0 && Into <= GateCorridorUu
					&& FMath::Abs(FVector2D::DotProduct(FromGate, Across)) < GateCorridorUu * 0.5)
				{
					bLegal = false;
					break;
				}
			}
			if (!bLegal)
			{
				continue;
			}

			for (const TArray<FVector2D>& Other : Taken)
			{
				if (QuadsIntersect(Corners, Other))
				{
					bLegal = false;
					break;
				}
			}
			if (!bLegal)
			{
				continue;
			}

			Candidate.bPlaced = true;
			OutStand = Candidate;
			Taken.Add(Corners);
			return true;
		}

		// DROPPED, not forced. A module shoved in anyway would intersect something, and a
		// mesh through a mesh is the one failure no camera angle hides. The caller reports
		// it - "Modules 2 of 3" is already the readout's habit.
		return false;
	};

	// LARGEST FIRST, through an index order rather than by sorting Yard.Stands, whose order
	// is the caller's contract. A tank placed after four pumps have taken the middle has
	// nowhere left to go, and the player loses the biggest object rather than the smallest.
	TArray<int32> Order;
	for (int32 Index = 0; Index < Footprints.Num(); ++Index)
	{
		if (!Footprints[Index].bFrontsTheGate)
		{
			Order.Add(Index);
		}
	}
	Order.Sort([&Footprints](int32 A, int32 B)
	{
		return Footprints[A].LengthUu * Footprints[A].WidthUu
			> Footprints[B].LengthUu * Footprints[B].WidthUu;
	});

	for (const int32 Index : Order)
	{
		TryPlace(Footprints[Index], Yard.Stands[Index]);
	}

	// HOW MANY MORE WOULD FIT, from the same pass that places things - so the number the
	// player reads is produced by the code that would actually put the thing down. A
	// separate free-area calculation would be a second opinion about one question.
	//
	// Each phantom is added to Taken by TryPlace, which is what makes this terminate: it
	// occupies ground the next one cannot use. The cap is a backstop against a zero-area
	// footprint looping forever, not an expected limit.
	FStand Phantom;
	while (Yard.RoomForMore < 64 && TryPlace(RoomForFootprint, Phantom))
	{
		++Yard.RoomForMore;
	}

	return Yard;
}
