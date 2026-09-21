#include "Solve/PlotYard.h"

#include "Solve/PlotFit.h"
#include "Solve/RoadGeom.h"

namespace
{
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

	/**
	 * The ground a plot has, and what has been stood on it so far.
	 *
	 * EXTRACTED FROM LayOut RATHER THAN COPIED, because Reserve needs the identical sampler.
	 * Two samplers would be two answers to "does this fit", and the preview would stop being
	 * the thing that gets built - which is the one property the reservation design rests on.
	 */
	struct FYardSpace
	{
		TArrayView<const FVector2D> Outline;
		FVector2D Gate = FVector2D::ZeroVector;
		FVector2D Inward = FVector2D::ZeroVector;
		FVector2D Across = FVector2D::ZeroVector;
		double InwardBearing = 0.0;
		double Deepest = 0.0;
		FVector2D Min = FVector2D::ZeroVector;
		FVector2D Max = FVector2D::ZeroVector;

		/** Corner rectangles of every stand already placed. Grows as we go. */
		TArray<TArray<FVector2D>> Taken;

		FRandomStream Stream;

		/**
		 * Grown by HALF of ClearanceUu on every side, so two padded footprints standing edge
		 * to edge land a full ClearanceUu apart - the gap is enforced by the same
		 * intersection test that stops them overlapping outright. Two modules that merely
		 * touch read as one building, which is the stamped look this file exists to remove.
		 *
		 * EVERY CALLER MUST PAD, or its contribution to the gap is zero rather than half:
		 * PlaceAgainstTheBackFence used to skip this and store an unpadded rectangle in Taken,
		 * so a shed standing next to a sampled module landed only ClearanceUu/2 away rather
		 * than ClearanceUu - see PlaceAgainstTheBackFence's own comment (issue #193).
		 */
		static PlotYard::FFootprint Padded(const PlotYard::FFootprint& Footprint)
		{
			PlotYard::FFootprint Out = Footprint;
			Out.LengthUu += PlotYard::ClearanceUu;
			Out.WidthUu += PlotYard::ClearanceUu;
			return Out;
		}

		/**
		 * Stand it on the gate's ray, as deep as its whole footprint fits.
		 *
		 * AGAINST THE BACK FENCE, not in the gateway. It stood at Gate + Inward * HalfLength
		 * until 2026-09-17, which put the shed squarely in the entrance - a depot whose only
		 * way in is blocked by the building you drive out of.
		 *
		 * AS DEEP AS ITS WHOLE FOOTPRINT FITS, not as deep as the plot's deepest POINT. The
		 * first version measured the deepest vertex and put the centre a half-length in
		 * front of it, which is right only when the back edge is square to the gate ray. The
		 * four-point gesture makes slanted backs the ordinary case, and PIE on 2026-09-17
		 * showed the shed "quite often sticks out of the back boundary of the plot".
		 *
		 * EVERY CORNER TESTED, the same question FitBays and the sampler ask, and inset by
		 * the same CornerInsetUu for the same reason: a corner exactly on the boundary
		 * answers a containment test by floating-point coin flip.
		 *
		 * THE FENCE TEST STAYS UNPADDED - "hard against the back fence" is the placement
		 * Airside.Solve.PlotYardStandsTheShedAtTheBack pins, and padding the containment
		 * search would hold the shed off the boundary it is supposed to touch. WHAT TAKEN
		 * REMEMBERS IS PADDED, though: a shed that stayed unpadded there until issue #193 gave
		 * whatever the sampler stood beside it only half of ClearanceUu rather than the whole
		 * of it, because only the sampled side was ever contributing its half.
		 */
		bool PlaceAgainstTheBackFence(const PlotYard::FFootprint& Footprint,
			PlotYard::FStand& OutStand)
		{
			const double HalfLength = Footprint.LengthUu * 0.5;

			// WRITTEN AS WE PROBE, not held in a local and copied out on success. A shed that
			// fits nowhere keeps its LAST CLAMPED pose rather than falling back to the origin,
			// and Airside.Solve.PlotYardStandsTheShedAtTheBack asserts exactly that on a plot
			// too shallow to hold it: "a shallow plot keeps the shed off the road". Writing
			// only on success moved a dropped shed to (0,0), which is out across the road.
			OutStand.Heading = InwardBearing;

			TArray<FVector2D> Corners;
			for (int32 Probe = 0; Probe < PlotYard::BackFenceProbes; ++Probe)
			{
				const double Alpha =
					static_cast<double>(Probe) / (PlotYard::BackFenceProbes - 1);
				const double Depth = FMath::Lerp(Deepest - HalfLength, HalfLength, Alpha);
				if (Depth < HalfLength)
				{
					continue;
				}

				OutStand.Centre = Gate + Inward * Depth;
				PlotYard::StandCorners(OutStand, Footprint, Corners);

				bool bInside = true;
				for (const FVector2D& Corner : Corners)
				{
					if (!RoadGeom::PointInPolygon(Outline, Corner))
					{
						bInside = false;
						break;
					}
				}
				if (bInside)
				{
					// TAKEN HERE, not by a second pass afterwards. LayOut used to collect
					// placed stands into Taken in a loop of its own; folding it in is what
					// lets Reserve interleave back-fence and sampled placement in one walk
					// without the two disagreeing about what ground is spoken for.
					OutStand.bPlaced = true;

					// PADDED FOR TAKEN ONLY: same centre and heading, corners recomputed at
					// the grown size purely for what a LATER sampled stand tests itself
					// against. Storing the unpadded Corners already computed above (as this
					// did before issue #193) is what left the sampled-vs-shed gap at half of
					// ClearanceUu.
					TArray<FVector2D> PaddedCorners;
					PlotYard::StandCorners(OutStand, Padded(Footprint), PaddedCorners);
					Taken.Add(PaddedCorners);
					return true;
				}
			}

			// NOT PLACED IS A REAL ANSWER. A plot with no room for the shed anywhere on the
			// gate's ray reports it dropped, exactly as a sampled module would. Forcing it in
			// would put a building through the fence.
			return false;
		}

		/** Sample up to MaxTries poses. False means dropped, which is a real answer. */
		bool TryPlace(const PlotYard::FFootprint& Footprint, PlotYard::FStand& OutStand)
		{
			const PlotYard::FFootprint Grown = Padded(Footprint);

			const double HalfLength = Grown.LengthUu * 0.5;
			const double HalfWidth = Grown.WidthUu * 0.5;

			TArray<FVector2D> Corners;
			for (int32 Try = 0; Try < PlotYard::MaxTries; ++Try)
			{
				PlotYard::FStand Candidate;

				// THE HEADING IS CHOSEN FIRST, so the centre can be drawn from the band that
				// heading can actually occupy. Sampling the centre across the whole bounding
				// box and rejecting afterwards put most candidates half through the fence
				// before any other test ran - on a narrow plot that is most of the draw
				// wasted.
				//
				// A QUARTER TURN PLUS A FEW DEGREES. Uniform over a circle reads as debris
				// after an explosion; this reads as something parked in a hurry.
				Candidate.Heading = InwardBearing
					+ Stream.RandRange(0, 3) * UE_DOUBLE_HALF_PI
					+ Stream.FRandRange(-PlotYard::HeadingJitterRadians,
						PlotYard::HeadingJitterRadians);

				// This heading's own axis-aligned reach - EXACT, not the half-diagonal. The
				// diagonal holds for every rotation and so refuses poses a 12 degree turn
				// allows, which quietly costs the yard modules it had room for.
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

				PlotYard::StandCorners(Candidate, Grown, Corners);

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
					// seed 3. It is also simply the wrong rule: the truck needs room to get
					// off the pad and turn, and once it is clear of the fence it can weave.
					const FVector2D FromGate = Corner - Gate;
					const double Into = FVector2D::DotProduct(FromGate, Inward);
					if (Into >= 0.0 && Into <= PlotYard::GateCorridorUu
						&& FMath::Abs(FVector2D::DotProduct(FromGate, Across))
							< PlotYard::GateCorridorUu * 0.5)
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
			// mesh through a mesh is the one failure no camera angle hides. The caller
			// reports it - "Modules 2 of 3" is already the readout's habit.
			return false;
		}
	};

	/** Build the space from a plot. False for a degenerate outline. */
	bool MakeYardSpace(TArrayView<const FVector2D> Outline, FVector2D FrontageA,
		FVector2D FrontageB, FVector2D Gate, int32 Seed, FYardSpace& Out)
	{
		if (Outline.Num() < 3)
		{
			return false;
		}

		Out.Outline = Outline;
		Out.Gate = Gate;
		Out.Inward = PlotYard::InwardOf(Outline, FrontageA, FrontageB);
		Out.Across = RoadGeom::PerpCCW(Out.Inward);
		Out.InwardBearing = RoadGeom::Bearing(Out.Inward);

		// HOW DEEP THE PLOT RUNS on the gate's own ray, so a back-standing module can be
		// pushed as far from the road as the outline allows.
		Out.Deepest = 0.0;
		for (const FVector2D& Point : Outline)
		{
			Out.Deepest =
				FMath::Max(Out.Deepest, FVector2D::DotProduct(Point - Gate, Out.Inward));
		}

		BoundsOf(Outline, Out.Min, Out.Max);

		// SEEDED BEFORE ANY PLACEMENT, and nothing draws from it until the first TryPlace -
		// the back-fence pass decides its pose rather than sampling. So the draw sequence is
		// the one LayOut had when it built its stream after that pass.
		Out.Stream = FRandomStream(Seed);
		return true;
	}
}

FVector2D PlotYard::InwardOf(TArrayView<const FVector2D> Outline,
	FVector2D FrontageA, FVector2D FrontageB)
{
	const FVector2D Along = (FrontageB - FrontageA).GetSafeNormal();
	const FVector2D Left = RoadGeom::PerpCCW(Along);

	// Read off the polygon's winding rather than assumed counter-clockwise, exactly as
	// FitBays does: a plot stored the other way round would otherwise aim every module out of
	// the plot and across the road.
	return RoadGeom::PolygonArea(Outline) > 0.0 ? Left : -Left;
}

bool PlotYard::StandsOverlap(const FStand& A, const FFootprint& FootprintA,
	const FStand& B, const FFootprint& FootprintB)
{
	TArray<FVector2D> CornersA;
	TArray<FVector2D> CornersB;
	StandCorners(A, FootprintA, CornersA);
	StandCorners(B, FootprintB, CornersB);
	return QuadsIntersect(CornersA, CornersB);
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

	FYardSpace Space;
	if (!MakeYardSpace(Outline, FrontageA, FrontageB, Gate, Seed, Space))
	{
		return Yard;
	}

	// The back-standing modules first: their pose is decided, not sampled, so they take their
	// ground before anything is allowed to sample into it. PlaceAgainstTheBackFence adds each
	// one to Taken itself, which is why the separate collect-the-placed loop that used to sit
	// here is gone.
	for (int32 Index = 0; Index < Footprints.Num(); ++Index)
	{
		if (!Footprints[Index].bAgainstTheBackFence)
		{
			continue;
		}
		Space.PlaceAgainstTheBackFence(Footprints[Index], Yard.Stands[Index]);
	}

	// LARGEST FIRST, through an index order rather than by sorting Yard.Stands, whose order
	// is the caller's contract. A tank placed after four pumps have taken the middle has
	// nowhere left to go, and the player loses the biggest object rather than the smallest.
	TArray<int32> Order;
	for (int32 Index = 0; Index < Footprints.Num(); ++Index)
	{
		if (!Footprints[Index].bAgainstTheBackFence)
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
		Space.TryPlace(Footprints[Index], Yard.Stands[Index]);
	}

	// HOW MANY MORE WOULD FIT, from the same pass that places things - so the number the
	// player reads is produced by the code that would actually put the thing down. A
	// separate free-area calculation would be a second opinion about one question.
	//
	// Each phantom is added to Taken by TryPlace, which is what makes this terminate: it
	// occupies ground the next one cannot use. The cap is a backstop against a zero-area
	// footprint looping forever, not an expected limit.
	FStand Phantom;
	while (Yard.RoomForMore < 64 && Space.TryPlace(RoomForFootprint, Phantom))
	{
		++Yard.RoomForMore;
	}

	return Yard;
}

PlotYard::FReservation PlotYard::Reserve(TArrayView<const FVector2D> Outline,
	FVector2D FrontageA, FVector2D FrontageB, FVector2D Gate,
	TArrayView<const FKitSpec> Kits, int32 Seed)
{
	FReservation Reservation;

	FYardSpace Space;
	if (!MakeYardSpace(Outline, FrontageA, FrontageB, Gate, Seed, Space))
	{
		return Reservation;
	}

	// THE CYCLE, EXPANDED ONCE. Weight 3 means three offers per turn round the kits, and
	// building the order up front keeps the loop below a plain walk rather than three nested
	// counters that have to agree with each other.
	//
	// LARGEST FIRST WITHIN A CYCLE, which is LayOut's lesson kept rather than re-learnt: a
	// tank offered the yard after four pumps have taken the middle has nowhere left to go,
	// and the player loses the big object rather than the small one. Ties hold their kit
	// order so the cycle stays deterministic.
	TArray<int32> Cycle;
	for (int32 Kit = 0; Kit < Kits.Num(); ++Kit)
	{
		for (int32 N = 0; N < FMath::Max(Kits[Kit].ReserveWeight, 0); ++N)
		{
			Cycle.Add(Kit);
		}
	}
	Cycle.StableSort([&Kits](int32 A, int32 B)
	{
		return Kits[A].Footprint.LengthUu * Kits[A].Footprint.WidthUu
			> Kits[B].Footprint.LengthUu * Kits[B].Footprint.WidthUu;
	});

	if (Cycle.Num() == 0)
	{
		return Reservation;
	}

	// ONE BAY EACH BEFORE ANY KIT GROWS.
	//
	// THE FIRST VERSION OFFERED EVERY KIT ITS FULL RUN STRAIGHT AWAY, and on a small plot the
	// first kit ate the lot: the concept sheet's 12 x 8 m depot reserved a three-bay shed run
	// - 12 m wide by 8 m deep, the whole plot - and left nothing for the tank or the pump.
	// The depot the sheet draws is one shed, one tank, one pump, and it stopped fitting in
	// its own plot.
	//
	// DISTINCT KITS, NOT THE WEIGHTED CYCLE: a foothold pass that walked the cycle would give
	// a weight-3 shed three bays before the pump saw the yard at all, which is the same bug
	// one step smaller. Largest first within the pass, for the reason the cycle sorts.
	//
	// WHETHER THE RAY WAS OFFERED, NOT WHETHER IT WAS TAKEN. CeilingFor(Kit) == 0 used to
	// stand for "no attempt yet", but a back-fence placement that FAILS also leaves the
	// ceiling at zero forever - so every later offer of that kit was forced back onto the
	// same dead ray and it was never sampled anywhere else, contradicting "the rest are
	// sampled like anything else" below (issue #193). Tracked separately so a failed first
	// attempt still frees the rest of the cycle.
	TArray<bool> RayOffered;
	RayOffered.SetNumZeroed(Kits.Num());

	auto PlaceOne = [&](int32 Kit, int32 MaxRun) -> bool
	{
		FReservedStand Stand;
		Stand.KitIndex = Kit;

		// ONLY THE FIRST STAND OF A BACK-FENCE KIT TAKES THE RAY. The back-fence pass walks
		// depths along one line out of the gate, so a second stand offered that line either
		// lands on the first or is refused outright. The rest are sampled like anything else.
		const bool bTakesTheRay = Kits[Kit].Footprint.bAgainstTheBackFence && !RayOffered[Kit];
		if (Kits[Kit].Footprint.bAgainstTheBackFence)
		{
			RayOffered[Kit] = true;
		}

		// AS LONG AS IT CAN, THEN SHORTER. A plot with room for two bays should get a two-bay
		// run rather than nothing: refusing the whole run because the third bay does not fit
		// would leave ground empty that the player drew and paid for.
		const int32 Cap = FMath::Clamp(FMath::Min(Kits[Kit].RunCap, MaxRun), 1, 64);
		for (int32 Length = Cap; Length >= 1; --Length)
		{
			// THE RUN'S FOOTPRINT, not the module's. Bays share walls, so a run is N times as
			// wide and exactly as deep - no clearance between bays, because they are one
			// building. FKitSpec::Footprint stays one module's so that this multiplication
			// happens in exactly one place.
			FFootprint Run = Kits[Kit].Footprint;
			Run.WidthUu *= Length;

			Stand.RunLength = Length;
			const bool bPlaced = bTakesTheRay
				? Space.PlaceAgainstTheBackFence(Run, Stand)
				: Space.TryPlace(Run, Stand);

			if (bPlaced)
			{
				Reservation.Stands.Add(Stand);
				return true;
			}
		}
		return false;
	};

	TArray<int32> Footholds;
	for (int32 Kit = 0; Kit < Kits.Num(); ++Kit)
	{
		if (Kits[Kit].ReserveWeight > 0)
		{
			Footholds.Add(Kit);
		}
	}
	Footholds.StableSort([&Kits](int32 A, int32 B)
	{
		return Kits[A].Footprint.LengthUu * Kits[A].Footprint.WidthUu
			> Kits[B].Footprint.LengthUu * Kits[B].Footprint.WidthUu;
	});
	for (const int32 Kit : Footholds)
	{
		PlaceOne(Kit, /*MaxRun=*/1);
	}

	// A WHOLE CYCLE THAT PLACES NOTHING MEANS FULL, rather than one failure meaning full: a
	// plot with no room left for a shed may still take three pumps, and stopping at the shed
	// would waste the corner the player paid for.
	//
	// The cap is a backstop against a zero-area footprint looping forever, not an expected
	// limit - the same role the cap plays in LayOut's RoomForMore loop.
	bool bPlacedAny = true;
	while (bPlacedAny && Reservation.Stands.Num() < 256)
	{
		bPlacedAny = false;
		for (const int32 Kit : Cycle)
		{
			// FULL RUNS FROM HERE ON. Every kit already has its foothold, so a shed taking
			// three bays now costs the tank nothing it was owed.
			if (PlaceOne(Kit, Kits[Kit].RunCap))
			{
				bPlacedAny = true;
			}
		}
	}

	return Reservation;
}
