#include "Build/PlotLayoutStrategy.h"

#include "Solve/RoadGeom.h"
#include "UObject/Package.h"

namespace
{
	/** The ground a stand claims: its object plus its apron, run width included. */
	PlotYard::FFootprint ClaimedBy(const PlotYard::FKitSpec& Kit, int32 RunLength)
	{
		PlotYard::FFootprint Out;
		Out.LengthUu = Kit.Footprint.LengthUu + Kit.ApronUu.X;
		Out.WidthUu = Kit.Footprint.WidthUu * RunLength + Kit.ApronUu.Y * 2.0;
		Out.bAgainstTheBackFence = Kit.Footprint.bAgainstTheBackFence;
		return Out;
	}

	/**
	 * The outline's lateral span at one depth, in ACROSS coordinates. False if the plot does
	 * not reach that deep.
	 *
	 * BECAUSE A PLOT IS NOT A RECTANGLE. The four-point gesture makes a skewed quad the
	 * ordinary case, and a single HalfSpan taken over the whole outline is the width at the
	 * plot's WIDEST depth - which for a plot that fans out towards the back is nowhere near
	 * the frontage. Placing a column at that offset put it outside the plot near the gate, so
	 * it failed containment on its first stand and the whole column vanished: PIE on
	 * 2026-09-20 drew a slightly off-square 45 m plot as 6 sheds, 0 tanks and 0 pumps.
	 */
	bool LateralSpanAt(TArrayView<const FVector2D> Outline, const FVector2D& Gate,
		const FVector2D& Inward, const FVector2D& Across, double Depth,
		double& OutLow, double& OutHigh)
	{
		OutLow = TNumericLimits<double>::Max();
		OutHigh = -TNumericLimits<double>::Max();

		for (int32 I = 0; I < Outline.Num(); ++I)
		{
			const FVector2D& A = Outline[I];
			const FVector2D& B = Outline[(I + 1) % Outline.Num()];

			const double DepthA = FVector2D::DotProduct(A - Gate, Inward);
			const double DepthB = FVector2D::DotProduct(B - Gate, Inward);

			// The edge has to STRADDLE this depth to contribute a crossing. An edge lying
			// exactly along it contributes both its ends, which the endpoint test below picks
			// up anyway.
			if ((DepthA < Depth && DepthB < Depth) || (DepthA > Depth && DepthB > Depth))
			{
				continue;
			}

			const double Span = DepthB - DepthA;
			const double T = FMath::IsNearlyZero(Span) ? 0.0 : (Depth - DepthA) / Span;
			const FVector2D At = A + (B - A) * FMath::Clamp(T, 0.0, 1.0);
			const double Lateral = FVector2D::DotProduct(At - Gate, Across);

			OutLow = FMath::Min(OutLow, Lateral);
			OutHigh = FMath::Max(OutHigh, Lateral);
		}

		return OutHigh > OutLow;
	}

	/**
	 * How deep the plot runs at one lateral offset. False if the plot is not there at all.
	 *
	 * THE TRANSPOSE OF LateralSpanAt, and it exists for the same reason. Deepest is the depth
	 * of the deepest CORNER of the whole outline; a back band placed at that depth stands
	 * there across its entire width, and the moment the back edge is not square to the gate
	 * most of that width is beyond the fence. Every stand is refused and the plot draws no
	 * sheds at all - PIE, 2026-09-20: "as soon as the back is no longer square you lose all of
	 * the sheds".
	 *
	 * A GLOBAL EXTREME IS NOT A LOCAL MEASUREMENT. That sentence covers every layout fault
	 * found in this file so far: HalfSpan put the columns outside a flared plot, HalfSpan
	 * again gave the shed band a window measured at the wrong depth, and Deepest does this.
	 * Ask the outline where IT is, at the place you are about to stand something.
	 */
	bool DepthSpanAt(TArrayView<const FVector2D> Outline, const FVector2D& Gate,
		const FVector2D& Inward, const FVector2D& Across, double Lateral,
		double& OutNear, double& OutFar)
	{
		OutNear = TNumericLimits<double>::Max();
		OutFar = -TNumericLimits<double>::Max();

		for (int32 I = 0; I < Outline.Num(); ++I)
		{
			const FVector2D& A = Outline[I];
			const FVector2D& B = Outline[(I + 1) % Outline.Num()];

			const double LateralA = FVector2D::DotProduct(A - Gate, Across);
			const double LateralB = FVector2D::DotProduct(B - Gate, Across);

			if ((LateralA < Lateral && LateralB < Lateral)
				|| (LateralA > Lateral && LateralB > Lateral))
			{
				continue;
			}

			const double Span = LateralB - LateralA;
			const double T = FMath::IsNearlyZero(Span) ? 0.0 : (Lateral - LateralA) / Span;
			const FVector2D At = A + (B - A) * FMath::Clamp(T, 0.0, 1.0);
			const double Depth = FVector2D::DotProduct(At - Gate, Inward);

			OutNear = FMath::Min(OutNear, Depth);
			OutFar = FMath::Max(OutFar, Depth);
		}

		return OutFar > OutNear;
	}

	/** Is this stand wholly inside the outline, and clear of everything already placed? */
	bool IsLegal(const PlotYard::FStand& Stand, const PlotYard::FFootprint& Claimed,
		TArrayView<const FVector2D> Outline,
		const TArray<PlotYard::FReservedStand>& Placed,
		TArrayView<const PlotYard::FKitSpec> Kits)
	{
		TArray<FVector2D> Corners;
		PlotYard::StandCorners(Stand, Claimed, Corners);
		for (const FVector2D& Corner : Corners)
		{
			if (!RoadGeom::PointInPolygon(Outline, Corner)) { return false; }
		}
		for (const PlotYard::FReservedStand& Other : Placed)
		{
			if (PlotYard::StandsOverlap(Stand, Claimed, Other,
				ClaimedBy(Kits[Other.KitIndex], Other.RunLength)))
			{
				return false;
			}
		}
		return true;
	}
}

PlotYard::FReservation UScatterLayoutStrategy::Solve(
	const FPlotSite& Site, TArrayView<const PlotYard::FKitSpec> Kits) const
{
	return PlotYard::Reserve(
		Site.Outline, Site.FrontageA, Site.FrontageB, Site.Gate, Kits, Site.Seed);
}

PlotYard::FReservation UFuelYardBandsStrategy::Solve(
	const FPlotSite& Site, TArrayView<const PlotYard::FKitSpec> Kits) const
{
	PlotYard::FReservation Reservation;
	if (Site.Outline.Num() < 3 || Kits.Num() == 0)
	{
		return Reservation;
	}

	const FVector2D Inward = PlotYard::InwardOf(Site.Outline, Site.FrontageA, Site.FrontageB);
	const double InwardBearing = RoadGeom::Bearing(Inward);

	// ACROSS POINTS LEFT as you stand at the gate looking into the plot: PerpCCW of an inward
	// (0,1) is (-1,0). Tanks take +Across and pumps -Across, and FuelYardKeepsApronsClear
	// pins the arrangement so a sign flip here cannot pass unnoticed.
	const FVector2D Across = RoadGeom::PerpCCW(Inward);

	double Deepest = 0.0;
	double HalfSpan = 0.0;
	for (const FVector2D& Point : Site.Outline)
	{
		Deepest = FMath::Max(Deepest, FVector2D::DotProduct(Point - Site.Gate, Inward));
		HalfSpan = FMath::Max(HalfSpan,
			FMath::Abs(FVector2D::DotProduct(Point - Site.Gate, Across)));
	}

	// A band walks OUTWARD FROM ITS ORIGIN in one direction, so a wider or deeper plot only
	// ever adds stands to the END of a band and never re-places the ones already there. That
	// is what makes this layout monotonic where the sampler is not.
	//
	// PITCH IS PASSED, NOT DERIVED. A back band steps ACROSS the plot and its pitch is the
	// claimed WIDTH; a side band steps INTO the plot and its pitch is the claimed LENGTH.
	// Deriving it here would space the tanks by their width and overlap them.
	auto FillBand = [&](int32 Kit, const FVector2D& Origin, const FVector2D& Step,
		int32 RunLength, double Pitch, double DepthLimit)
	{
		const PlotYard::FFootprint Claimed = ClaimedBy(Kits[Kit], RunLength);

		for (int32 Index = 0; Index < 64; ++Index)
		{
			// STOPPED SHORT OF THE SHEDS - FROM THE SECOND ONE ON.
			//
			// THE FIRST IN A COLUMN IS REACHED STRAIGHT FROM THE GATE, so it may stand beside
			// the sheds; on the smallest legal plot the shed band takes the whole depth and
			// beside is the only place a tank can go at all. Every later one is approached
			// across the middle of the yard instead, and at the shed band's depth the middle
			// IS sheds - which is what walled in the back of both columns in PIE.
			//
			// A back band passes the plot's own depth as the limit and is bounded by
			// containment alone.
			if (Index > 0)
			{
				const FVector2D At = Origin + Step * (Pitch * Index);
				const double Far = FVector2D::DotProduct(At - Site.Gate, Inward)
					+ Claimed.LengthUu * 0.5;
				if (Far > DepthLimit)
				{
					return;
				}
			}

			PlotYard::FReservedStand Stand;
			Stand.KitIndex = Kit;
			Stand.RunLength = RunLength;
			Stand.Heading = InwardBearing;
			Stand.Centre = Origin + Step * (Pitch * Index);
			Stand.bPlaced = true;

			if (!IsLegal(Stand, Claimed, Site.Outline, Reservation.Stands, Kits))
			{
				// THE BAND ENDS AT ITS FIRST REFUSAL rather than skipping past it. A gap
				// jumped over would put a stand beyond the plot's taper, and the band would
				// read as scattered - which is the thing this layout exists to stop.
				return;
			}
			Reservation.Stands.Add(Stand);
		}
	};

	// WHERE THE SHED BAND'S GROUND BEGINS, computed BEFORE the columns are laid so they can
	// stop short of it.
	//
	// A COLUMN THAT RUNS PAST THIS IS A COLUMN NOBODY CAN REACH. The columns sit against the
	// plot edges, so a module is approached from the middle of the yard - and at the shed
	// band's depth the middle IS sheds. PIE on 2026-09-20 drew five tanks up the left edge
	// with the back ones walled in between the fence and the shed row: "some of the pumps and
	// the tanks are inaccessible as they are down the side of the sheds".
	//
	// THIS IS NOT A CIRCULATION SOLVER. Nothing here proves a route exists; it refuses the one
	// arrangement that provably has none. See the design doc section 3.
	const PlotYard::FFootprint ShedGround =
		ClaimedBy(Kits[0], FMath::Clamp(Kits[0].RunCap, 1, 64));
	const double ColumnLimit = Deepest - ShedGround.LengthUu - PlotYard::ClearanceUu;

	// --- Tanks down the left, pumps down the right -----------------------------------
	//
	// THE COLUMNS FIRST, and the order is load-bearing rather than tidy. They are anchored to
	// the plot's own EDGES, so they take the same ground whatever else happens; the shed band
	// is anchored to whatever is left. Run the other way round and the sheds - centred on the
	// gate, up to three bays wide - claim the middle AND the edges on a narrow plot, and the
	// tank and pump get nothing. On an 11 x 12 m site that left two 1.5 m strips.
	//
	// STARTED ONE TRUCK-CORRIDOR IN FROM THE GATE so a column never grows across the way out,
	// and stepping INTO the plot so the yard fills from the road backwards.

	// The window still free for the shed band is computed AFTER the columns, at the shed
	// band's OWN depth - see below. Deriving it here from the columns' lateral extents was the
	// same measure-at-the-wrong-depth bug that put the columns outside a skewed plot, left in
	// the one place it had not yet been fixed: the columns stand near the gate and the sheds
	// stand at the back, so on an angled plot the width at one is not the width at the other.
	// A shed band placed against a window measured at the frontage was refused outright, and a
	// 65 m plot south of a road drew 0 sheds, 1 tank and 16 pumps.

	for (int32 Kit = 1; Kit < Kits.Num(); ++Kit)
	{
		const PlotYard::FFootprint Claimed = ClaimedBy(Kits[Kit], 1);
		const bool bLeft = (Kit == 1);
		const FVector2D Edge = bLeft ? Across : -Across;

		const int32 Before = Reservation.Stands.Num();

		// AT THE COLUMN'S OWN DEPTH, not at the plot's widest. Both ends of the first stand
		// are measured and the TIGHTER edge wins, so a tapering plot cannot leave a corner of
		// the stand hanging outside - which on a convex quad is the whole of the problem.
		const double Near = PlotYard::GateCorridorUu;
		const double Far = Near + Claimed.LengthUu;

		double NearLow = 0.0, NearHigh = 0.0, FarLow = 0.0, FarHigh = 0.0;
		if (!LateralSpanAt(Site.Outline, Site.Gate, Inward, Across, Near, NearLow, NearHigh)
			|| !LateralSpanAt(Site.Outline, Site.Gate, Inward, Across, Far, FarLow, FarHigh))
		{
			continue;
		}

		const double EdgeAt = bLeft
			? FMath::Min(NearHigh, FarHigh)
			: -FMath::Max(NearLow, FarLow);

		// FILES, NOT A FILE. A column that has run out of DEPTH starts another one inward, so
		// a wide shallow plot buys tanks and pumps the way it already buys sheds. Before this,
		// frontage bought nothing but sheds: a 100 m plot and a 15 m one held the same one
		// tank, because tanks only ever ran into the plot.
		//
		// A TRUCK AISLE BETWEEN FILES, not a clearance. Two files a clearance apart are a
		// four-metre-thick wall with a metre of air in it, and everything in the outer file is
		// behind the inner one. GateCorridorUu is the width a truck already needs to get in at
		// the gate, and it is the width it needs to get between two rows of tanks.
		//
		// HALF THE HALF-SPAN, at most. Files march inward until they meet the middle, and the
		// middle is where the sheds go and where a truck turns. This is a composition rule -
		// how much of the yard one band may claim - and not a count: it says nothing about how
		// many tanks that ground holds.
		const double Pitch = Claimed.WidthUu + PlotYard::GateCorridorUu;
		const double MostOneBandMayTake = FMath::Abs(EdgeAt) * 0.5;

		double Taken = 0.0;
		for (int32 File = 0; File < 16; ++File)
		{
			// THE FIRST FILE IS NEVER REFUSED BY THE PROPORTION. A tank is 5 m wide and half
			// the half-span of a 15 m plot is 3.75, so the rule that stops a band eating the
			// yard would otherwise deny the narrow plot its only tank - and it did, until
			// FuelYardFitsTheConceptSheet said so. The rule governs how far a band GROWS, not
			// whether it exists.
			const double Offset = File * Pitch;
			if (File > 0 && Offset + Claimed.WidthUu > MostOneBandMayTake)
			{
				break;
			}

			const int32 BeforeFile = Reservation.Stands.Num();
			const double Side = EdgeAt - Offset - Claimed.WidthUu * 0.5;

			const FVector2D Centre = Site.Gate + Edge * Side
				+ Inward * (Near + Claimed.LengthUu * 0.5);

			FillBand(Kit, Centre, Inward, 1, Claimed.LengthUu + PlotYard::ClearanceUu,
				ColumnLimit);

			// A FILE THAT PLACED NOTHING ENDS THE BAND. Skipping to the next one would jump a
			// gap the plot's taper put there, and the band would read as scattered.
			if (Reservation.Stands.Num() == BeforeFile)
			{
				break;
			}
			Taken = Offset + Claimed.WidthUu + PlotYard::ClearanceUu;
		}

		(void)Before;
		(void)Taken;
	}

	// --- Sheds, across the back, between the columns ----------------------------------
	//
	// CENTRED ON WHAT IS LEFT, not on the gate. The free window is rarely symmetric about the
	// gate once a 5 m tank column and a 2 m pump column have taken opposite edges, and a band
	// centred on the gate would reach into one of them and be refused outright.
	{
		const int32 Kit = 0;

		// THE WINDOW, MEASURED AT THE SHED BAND'S OWN DEPTH. Both ends of the band's ground
		// are taken and the tighter edge wins, exactly as a column measures its own.
		const double ShedNear = Deepest - ShedGround.LengthUu;
		double NearLow = -HalfSpan, NearHigh = HalfSpan;
		double FarLow = -HalfSpan, FarHigh = HalfSpan;
		LateralSpanAt(Site.Outline, Site.Gate, Inward, Across, ShedNear, NearLow, NearHigh);
		LateralSpanAt(Site.Outline, Site.Gate, Inward, Across, Deepest - 1.0, FarLow, FarHigh);

		double WindowLow = FMath::Max(NearLow, FarLow);
		double WindowHigh = FMath::Min(NearHigh, FarHigh);

		// NARROWED ONLY BY WHAT ACTUALLY REACHES BACK HERE. A column stops short of the shed
		// band, so most of it is nowhere near this depth and narrowing for all of it would
		// hand the sheds a sliver of the plot. What does reach is each file's FIRST stand,
		// which is allowed beside the sheds because it is reached straight from the gate.
		for (const PlotYard::FReservedStand& Stand : Reservation.Stands)
		{
			const PlotYard::FFootprint Claimed =
				ClaimedBy(Kits[Stand.KitIndex], Stand.RunLength);
			const double Depth = FVector2D::DotProduct(Stand.Centre - Site.Gate, Inward);
			if (Depth + Claimed.LengthUu * 0.5 <= ShedNear)
			{
				continue;
			}

			const double Lateral = FVector2D::DotProduct(Stand.Centre - Site.Gate, Across);
			const double Half = Claimed.WidthUu * 0.5 + PlotYard::ClearanceUu;
			if (Lateral > 0.0)
			{
				WindowHigh = FMath::Min(WindowHigh, Lateral - Half);
			}
			else
			{
				WindowLow = FMath::Max(WindowLow, Lateral + Half);
			}
		}

		const double Middle = (WindowLow + WindowHigh) * 0.5;

		// AS LONG A RUN AS THE WINDOW TAKES, THEN SHORTER - the same shrink Reserve does. A
		// band that only ever tried RunCap gave a narrow plot no sheds at all rather than a
		// one-bay one.
		// EACH STAND AT THE DEPTH ITS OWN LATERAL POSITION HAS, so the row follows a slanted
		// back edge instead of standing at the deepest corner's depth all the way across. The
		// SHALLOWER of the stand's two lateral edges wins - a stand is only as deep as its
		// shallowest corner allows, and taking the middle would hang one end over the fence.
		auto BackBand = [&](int32 RunLength, double From, double Direction, double Pitch)
		{
			const PlotYard::FFootprint Claimed = ClaimedBy(Kits[Kit], RunLength);

			for (int32 Index = 0; Index < 64; ++Index)
			{
				const double Lateral = From + Direction * Pitch * Index;
				const double HalfWidth = Claimed.WidthUu * 0.5;

				double NearA = 0.0, FarA = 0.0, NearB = 0.0, FarB = 0.0;
				if (!DepthSpanAt(Site.Outline, Site.Gate, Inward, Across,
						Lateral - HalfWidth, NearA, FarA)
					|| !DepthSpanAt(Site.Outline, Site.Gate, Inward, Across,
						Lateral + HalfWidth, NearB, FarB))
				{
					return;
				}

				const double BackHere = FMath::Min(FarA, FarB);

				PlotYard::FReservedStand Stand;
				Stand.KitIndex = Kit;
				Stand.RunLength = RunLength;
				Stand.Heading = InwardBearing;
				Stand.Centre = Site.Gate + Across * Lateral
					+ Inward * (BackHere - Claimed.LengthUu * 0.5);
				Stand.bPlaced = true;

				if (!IsLegal(Stand, Claimed, Site.Outline, Reservation.Stands, Kits))
				{
					return;
				}
				Reservation.Stands.Add(Stand);
			}
		};

		// AS LONG A RUN AS THE WINDOW TAKES, THEN SHORTER - the same shrink Reserve does. A
		// band that only ever tried RunCap gave a narrow plot no sheds at all rather than a
		// one-bay one.
		for (int32 RunLength = FMath::Clamp(Kits[Kit].RunCap, 1, 64); RunLength >= 1;
			--RunLength)
		{
			const int32 Before = Reservation.Stands.Num();

			const PlotYard::FFootprint Claimed = ClaimedBy(Kits[Kit], RunLength);
			const double Pitch = Claimed.WidthUu + PlotYard::ClearanceUu;

			BackBand(RunLength, Middle, +1.0, Pitch);
			BackBand(RunLength, Middle - Pitch, -1.0, Pitch);

			if (Reservation.Stands.Num() > Before)
			{
				break;
			}
		}
	}

	return Reservation;
}

const UPlotLayoutStrategy* PlotLayoutFor(EPlotLayout Layout)
{
	// ROOTED ON FIRST USE, so the GC cannot take a strategy the presenter is about to call.
	// Static locals rather than a registry: the set is closed at compile time, and a registry
	// would be a second list to keep in step with the enum.
	static UScatterLayoutStrategy* Scatter = []
	{
		UScatterLayoutStrategy* Made = NewObject<UScatterLayoutStrategy>(
			GetTransientPackage(), TEXT("ScatterLayout"));
		Made->AddToRoot();
		return Made;
	}();

	static UFuelYardBandsStrategy* Bands = []
	{
		UFuelYardBandsStrategy* Made = NewObject<UFuelYardBandsStrategy>(
			GetTransientPackage(), TEXT("FuelYardBandsLayout"));
		Made->AddToRoot();
		return Made;
	}();

	switch (Layout)
	{
	case EPlotLayout::Scatter: return Scatter;
	case EPlotLayout::FuelYardBands: return Bands;
	}

	// A LAYOUT ADDED TO THE ENUM WITH NO CASE gets the scatter rather than a null: an
	// unplanned yard is wrong, and an empty plot is wrong AND looks like a broken presenter.
	// Airside.Build.EveryPlotLayoutResolves fails either way.
	return Scatter;
}
