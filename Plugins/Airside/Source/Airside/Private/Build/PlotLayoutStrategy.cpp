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
		int32 RunLength, double Pitch)
	{
		const PlotYard::FFootprint Claimed = ClaimedBy(Kits[Kit], RunLength);

		for (int32 Index = 0; Index < 64; ++Index)
		{
			// THE CAP IS WHAT A DEPOT IS, and it is checked against what this kit already
			// holds rather than against this band's own count: a shed kit fills two bands,
			// one either side of the centre, and capping each separately would give twice
			// the sheds asked for.
			const int32 Cap = Kits[Kit].MaxPerPlot;
			if (Cap > 0 && Reservation.CeilingFor(Kit) + RunLength > Cap)
			{
				return;
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
	// The window still free for the shed band, in ACROSS COORDINATES - signed distance from
	// the gate along Across, so High is the left edge and Low the right. Measured in that one
	// frame throughout: mixing it with world X is how the first version had the tank column
	// narrowing the wrong side of the plot.
	double WindowHigh = HalfSpan;
	double WindowLow = -HalfSpan;

	for (int32 Kit = 1; Kit < Kits.Num(); ++Kit)
	{
		const PlotYard::FFootprint Claimed = ClaimedBy(Kits[Kit], 1);
		const bool bLeft = (Kit == 1);
		const FVector2D Edge = bLeft ? Across : -Across;
		const double Side = HalfSpan - Claimed.WidthUu * 0.5;

		const int32 Before = Reservation.Stands.Num();

		const FVector2D Centre = Site.Gate + Edge * Side
			+ Inward * (PlotYard::GateCorridorUu + Claimed.LengthUu * 0.5);

		FillBand(Kit, Centre, Inward, 1, Claimed.LengthUu + PlotYard::ClearanceUu);

		// A COLUMN THAT PLACED NOTHING COSTS THE SHEDS NOTHING. Narrowing the window for a
		// tank that was never reserved would be paying for ground nobody took.
		if (Reservation.Stands.Num() > Before)
		{
			const double Inner = HalfSpan - Claimed.WidthUu - PlotYard::ClearanceUu;
			if (bLeft) { WindowHigh = Inner; } else { WindowLow = -Inner; }
		}
	}

	// --- Sheds, across the back, between the columns ----------------------------------
	//
	// CENTRED ON WHAT IS LEFT, not on the gate. The free window is rarely symmetric about the
	// gate once a 5 m tank column and a 2 m pump column have taken opposite edges, and a band
	// centred on the gate would reach into one of them and be refused outright.
	{
		const int32 Kit = 0;
		const double Middle = (WindowLow + WindowHigh) * 0.5;

		// AS LONG A RUN AS THE WINDOW TAKES, THEN SHORTER - the same shrink Reserve does. A
		// band that only ever tried RunCap gave a narrow plot no sheds at all rather than a
		// one-bay one.
		for (int32 RunLength = FMath::Clamp(Kits[Kit].RunCap, 1, 64); RunLength >= 1;
			--RunLength)
		{
			const int32 Before = Reservation.Stands.Num();

			const PlotYard::FFootprint Claimed = ClaimedBy(Kits[Kit], RunLength);
			const FVector2D Centre = Site.Gate + Across * Middle
				+ Inward * (Deepest - Claimed.LengthUu * 0.5);
			const double Pitch = Claimed.WidthUu + PlotYard::ClearanceUu;

			FillBand(Kit, Centre, Across, RunLength, Pitch);
			FillBand(Kit, Centre - Across * Pitch, -Across, RunLength, Pitch);

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
