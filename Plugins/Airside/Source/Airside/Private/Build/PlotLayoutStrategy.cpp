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
	 * A COLUMN ANCHORS TO THE PLOT'S EDGE AT ITS OWN DEPTH. The global lateral extreme is the
	 * width at the plot's WIDEST depth, which on a flared quad is at the back and nowhere near
	 * the frontage where a column starts - so anchoring there put both columns outside the
	 * plot, their first stand failed containment, and the whole band vanished.
	 *
	 * IT WAS DELETED ONCE, on the grounds that nothing referenced it after the sheds were
	 * moved first, and the column code that should have been calling it was reaching for the
	 * global extreme instead. "Unused" was true of the text and false of the intent.
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
	 * ASKED OF THE OUTLINE, not taken from a global extreme. Deepest is the depth
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

	/**
	 * Would this stand park itself in another module's way in?
	 *
	 * AN APRON IS A DOOR, NOT A BUFFER. A module that declares one is entered from the front -
	 * a truck noses into a shed - so the ground beyond that apron is the only route it has.
	 * Clearance says nothing about this: a tank a metre in front of a shed clears the shed by
	 * the rules and walls it up completely.
	 *
	 * KEYED ON THE APRON rather than on kit 0 or on the back-fence flag, because the apron is
	 * the thing that means "entered from the front". A future kit that declares one gets the
	 * same protection with no edit here, and a shed that lost its apron would stop claiming a
	 * route it no longer needs.
	 */
	bool BlocksAWayIn(const PlotYard::FStand& Stand, const PlotYard::FFootprint& Claimed,
		const TArray<PlotYard::FReservedStand>& Placed,
		TArrayView<const PlotYard::FKitSpec> Kits)
	{
		for (const PlotYard::FReservedStand& Other : Placed)
		{
			const PlotYard::FKitSpec& Kit = Kits[Other.KitIndex];
			if (Kit.ApronUu.X <= 0.0)
			{
				continue;
			}

			const PlotYard::FFootprint Theirs = ClaimedBy(Kit, Other.RunLength);

			// FORWARD IS INWARD, so the way in is behind the module, between its apron and
			// the gate - one truck-corridor of it, spanning the whole run's width.
			const FVector2D Forward(FMath::Cos(Other.Heading), FMath::Sin(Other.Heading));

			PlotYard::FStand WayIn = Other;
			WayIn.Centre -= Forward * (Theirs.LengthUu + PlotYard::GateCorridorUu) * 0.5;

			PlotYard::FFootprint Ground;
			Ground.LengthUu = PlotYard::GateCorridorUu;
			Ground.WidthUu = Theirs.WidthUu;

			if (PlotYard::StandsOverlap(Stand, Claimed, WayIn, Ground))
			{
				return true;
			}
		}

		return false;
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

	// THE OUTLINE'S LATERAL REACH, used ONLY as the bound of a search - never as the place to
	// stand anything.
	//
	// THAT DISTINCTION IS THE WHOLE LESSON OF THIS FILE. Four PIE failures came from taking a
	// global extreme of the outline and standing a module at it: HalfSpan put both columns
	// outside a flared plot, HalfSpan again gave the shed band a window measured at the wrong
	// depth, and Deepest stood the shed row at the deepest CORNER's depth all the way across,
	// so a back edge a degree off square lost every shed. A global extreme is a fine place to
	// stop looking. It is never an answer to "how deep is the plot HERE".
	double LateralMin = TNumericLimits<double>::Max();
	double LateralMax = -TNumericLimits<double>::Max();
	for (const FVector2D& Point : Site.Outline)
	{
		const double Lateral = FVector2D::DotProduct(Point - Site.Gate, Across);
		LateralMin = FMath::Min(LateralMin, Lateral);
		LateralMax = FMath::Max(LateralMax, Lateral);
	}

	// --- Sheds, along the back --------------------------------------------------------
	//
	// SHEDS FIRST, AND THEY TAKE PRECEDENCE. A depot is trucks; the tanks and pumps serve
	// them. They were placed last until 2026-09-20 and the columns ate the yard: a 55 m plot
	// with a slanted back drew 0 sheds, 9 tanks and 32 pumps.
	//
	// IT ALSO DELETES TWO GLOBAL EXTREMES. With the sheds down first there is no ColumnLimit
	// to compute - a column stops before the sheds because it cannot overlap one - and no
	// window to measure, because the sheds are not squeezing between anything.
	{
		const int32 Kit = 0;

		// AT MOST HALF THE PLOT'S WIDTH, so precedence is not a monopoly. Without it a 15 m
		// plot takes a three-bay run across 12 of its 15 metres and leaves the tank and pump
		// two slivers - which is how the sheds came to be placed last in the first place.
		const double ShedBudget = (LateralMax - LateralMin) * 0.5;

		// CENTRED ON THE PLOT, not started at one edge. Scanning from LateralMin would spend
		// the whole budget on the left half and leave the sheds sitting on top of whichever
		// column takes that side, while the far half of the back fence stood empty.
		const double ShedFrom = (LateralMin + LateralMax) * 0.5 - ShedBudget * 0.5;

		for (int32 RunLength = FMath::Clamp(Kits[Kit].RunCap, 1, 64); RunLength >= 1;
			--RunLength)
		{
			const PlotYard::FFootprint Claimed = ClaimedBy(Kits[Kit], RunLength);
			const double Pitch = Claimed.WidthUu + PlotYard::ClearanceUu;
			const double HalfWidth = Claimed.WidthUu * 0.5;

			double Spent = 0.0;

			// SCANNED ACROSS THE PLOT, and an illegal position is SKIPPED rather than ending
			// the band. A row locked to the back edge cannot read as scattered - it is a row
			// with a gap where the plot is too shallow to stand in, which is what the ground
			// actually looks like. Ending at the first refusal was right when every stand sat
			// at one depth; it stops being right the moment the row follows the fence.
			for (double Lateral = ShedFrom + HalfWidth; Lateral <= LateralMax - HalfWidth;
				Lateral += Pitch)
			{
				if (Spent + Claimed.WidthUu > ShedBudget)
				{
					break;
				}

				// EACH STAND AT THE DEPTH ITS OWN POSITION HAS. The SHALLOWER of its two
				// lateral edges wins: a stand is only as deep as its shallowest corner allows,
				// and taking the middle would hang one end over the fence.
				double NearA = 0.0, FarA = 0.0, NearB = 0.0, FarB = 0.0;
				if (!DepthSpanAt(Site.Outline, Site.Gate, Inward, Across,
						Lateral - HalfWidth, NearA, FarA)
					|| !DepthSpanAt(Site.Outline, Site.Gate, Inward, Across,
						Lateral + HalfWidth, NearB, FarB))
				{
					continue;
				}

				PlotYard::FReservedStand Stand;
				Stand.KitIndex = Kit;
				Stand.RunLength = RunLength;
				Stand.Heading = InwardBearing;
				Stand.Centre = Site.Gate + Across * Lateral
					+ Inward * (FMath::Min(FarA, FarB) - Claimed.LengthUu * 0.5);
				Stand.bPlaced = true;

				if (!IsLegal(Stand, Claimed, Site.Outline, Reservation.Stands, Kits))
				{
					continue;
				}

				Reservation.Stands.Add(Stand);
				Spent += Pitch;
			}

			if (Reservation.Stands.Num() > 0)
			{
				break;
			}
		}
	}

	// --- Tanks down one side, pumps down the other ------------------------------------
	//
	// BOUNDED BY THE SHEDS THEMSELVES, not by a depth computed from the plot's deepest corner.
	// A column walks into the plot and stops when its next stand would overlap a shed, which
	// is the same rule whatever shape the back is - and unlike ColumnLimit it cannot be right
	// for one lateral offset and wrong for another.
	for (int32 Kit = 1; Kit < Kits.Num(); ++Kit)
	{
		const PlotYard::FFootprint Claimed = ClaimedBy(Kits[Kit], 1);
		const bool bLeft = (Kit == 1);
		const double Direction = bLeft ? -1.0 : 1.0;

		// THE PLOT'S EDGE AT THE COLUMN'S OWN DEPTH, both ends of the first stand measured and
		// the tighter one winning - never the plot's widest point, which on a flared quad is at
		// the back and would put the column outside the fence down here at the frontage.
		const double Near = PlotYard::GateCorridorUu;
		const double Far = Near + Claimed.LengthUu;

		double NearLow = 0.0, NearHigh = 0.0, FarLow = 0.0, FarHigh = 0.0;
		if (!LateralSpanAt(Site.Outline, Site.Gate, Inward, Across, Near, NearLow, NearHigh)
			|| !LateralSpanAt(Site.Outline, Site.Gate, Inward, Across, Far, FarLow, FarHigh))
		{
			continue;
		}

		const double Edge = bLeft
			? FMath::Min(NearHigh, FarHigh)
			: FMath::Max(NearLow, FarLow);

		// FILES, NOT A FILE. A column that has run out of DEPTH starts another one inward, so
		// a wide shallow plot buys tanks and pumps the way it already buys sheds.
		//
		// A TRUCK AISLE BETWEEN FILES, not a clearance: two files a metre apart are a wall
		// with air in it, and everything in the outer file is behind the inner one.
		const double FilePitch = Claimed.WidthUu + PlotYard::GateCorridorUu;
		const double MostOneBandMayTake = (LateralMax - LateralMin) * 0.25;

		double Taken = 0.0;
		for (int32 File = 0; File < 16; ++File)
		{
			// THE FIRST FILE IS NEVER REFUSED BY THE PROPORTION. A tank is 5 m wide and a
			// quarter of a 15 m plot is 3.75, so the rule that stops a band eating the yard
			// would otherwise deny the narrow plot its only tank.
			if (File > 0 && Taken + Claimed.WidthUu > MostOneBandMayTake)
			{
				break;
			}

			const double Lateral =
				Edge + Direction * (File * FilePitch + Claimed.WidthUu * 0.5);

			const int32 BeforeFile = Reservation.Stands.Num();

			// INTO THE PLOT FROM THE GATE, one truck-corridor in so the way out stays clear.
			for (int32 Index = 0; Index < 64; ++Index)
			{
				const double Depth = PlotYard::GateCorridorUu
					+ Claimed.LengthUu * 0.5
					+ Index * (Claimed.LengthUu + PlotYard::ClearanceUu);

				PlotYard::FReservedStand Stand;
				Stand.KitIndex = Kit;
				Stand.RunLength = 1;
				Stand.Heading = InwardBearing;
				Stand.Centre = Site.Gate + Across * Lateral + Inward * Depth;
				Stand.bPlaced = true;

				// ENDS AT ITS FIRST REFUSAL, unlike the shed row. A column marches away from
				// the gate, so the first thing it cannot pass - a shed, a shed's doorway, or
				// the fence - is also the last thing a truck could have driven past to reach
				// what lies beyond it.
				if (!IsLegal(Stand, Claimed, Site.Outline, Reservation.Stands, Kits)
					|| BlocksAWayIn(Stand, Claimed, Reservation.Stands, Kits))
				{
					break;
				}
				Reservation.Stands.Add(Stand);
			}

			// A FILE THAT PLACED NOTHING ENDS THE BAND. Skipping to the next would jump a gap
			// the plot's taper put there.
			if (Reservation.Stands.Num() == BeforeFile)
			{
				break;
			}
			Taken = File * FilePitch + Claimed.WidthUu + PlotYard::ClearanceUu;
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
