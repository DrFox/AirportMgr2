#include "Build/BuildCost.h"

// NO Content/ INCLUDE (issue #191/#78's shape): every rate this file prices with -
// Profile.CostPerMetre/UpkeepPerMetrePerDay, ForApron's RatePerSquareMetre, DailyUpkeep's
// ApronRatePerSquareMetrePerDay - already arrives as a PARAMETER, resolved by the caller
// (Present/RoadEditFacade.cpp's QuoteForApron; AirportOps' daily-upkeep poster). This file
// never called UAirsideSettings; the include was dead weight left over from before those
// callers existed.
#include "Entities/EntityDefinition.h"
#include "Model/RoadApron.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"

namespace
{
	/** uu to metres. One conversion, one place - see BuildCost's header. */
	constexpr double UuPerMetre = 100.0;

	double MetresFromUu(double Uu) { return Uu / UuPerMetre; }
}

double BuildCost::SegmentLengthUu(const URoadNetwork& Network, const FRoadSegment& Segment)
{
	const TArray<FRoadNode>& Nodes = Network.GetNodes();
	if (!Nodes.IsValidIndex(Segment.A.Index) || !Nodes.IsValidIndex(Segment.B.Index))
	{
		return 0.0;
	}
	const FRoadNode& A = Nodes[Segment.A.Index];
	const FRoadNode& B = Nodes[Segment.B.Index];
	if (!A.bAlive || !B.bAlive)
	{
		return 0.0;
	}
	return FVector2D::Distance(A.Position, B.Position);
}

FBuildQuote BuildCost::ForSegment(const URoadProfile& Profile, double LengthUu)
{
	FBuildQuote Quote;
	Quote.BaseAmount = FMath::Max(0.0, MetresFromUu(LengthUu)) * Profile.CostPerMetre;
	Quote.Source = &Profile;
	Quote.What = FText::Format(
		NSLOCTEXT("BuildCost", "PavementOf", "{0}, {1} m"),
		FText::FromString(Profile.GetName()),
		FText::AsNumber(FMath::RoundToInt(MetresFromUu(LengthUu))));
	return Quote;
}

FBuildQuote BuildCost::ForEntity(const UEntityDefinition& Definition)
{
	FBuildQuote Quote;
	Quote.BaseAmount = Definition.PlacementCost;
	Quote.Source = &Definition;
	Quote.What = FText::FromString(Definition.GetName());
	return Quote;
}

double BuildCost::PolygonAreaSquareMetres(TConstArrayView<FVector2D> Outline)
{
	if (Outline.Num() < 3)
	{
		return 0.0;
	}

	// The shoelace sum, in uu squared.
	double Twice = 0.0;
	for (int32 Index = 0; Index < Outline.Num(); ++Index)
	{
		const FVector2D& Current = Outline[Index];
		const FVector2D& Next = Outline[(Index + 1) % Outline.Num()];
		Twice += (Current.X * Next.Y) - (Next.X * Current.Y);
	}

	// ABSOLUTE, and this is not defensive tidying. A signed shoelace sum is NEGATIVE for one of
	// the two windings, and both genuinely occur here - CCW faces down in Unreal, so an outline
	// drawn either way round is a real apron. A negative area would quote a negative amount,
	// and a negative charge PAYS THE PLAYER TO BUILD.
	return FMath::Abs(Twice) * 0.5 / (UuPerMetre * UuPerMetre);
}

FBuildQuote BuildCost::ForApron(TConstArrayView<FVector2D> Outline, double RatePerSquareMetre)
{
	const double AreaSquareMetres = PolygonAreaSquareMetres(Outline);

	FBuildQuote Quote;
	Quote.BaseAmount = AreaSquareMetres * RatePerSquareMetre;

	// NO SOURCE ASSET, and that is forced rather than an oversight: FApronSurface is an outline
	// and a material slot name, because bands and lanes are meaningless for a polygon
	// (RoadApron.h). So an apron discount cannot key on an asset the way a taxiway's can, and
	// UPricing sees a null Source here. Named so nobody later reads the null as a bug.
	Quote.What = FText::Format(NSLOCTEXT("BuildCost", "ApronOf", "Apron, {0} m²"),
		FText::AsNumber(FMath::RoundToInt(AreaSquareMetres)));
	return Quote;
}

double BuildCost::DailyUpkeep(const URoadNetwork& Network, double ApronRatePerSquareMetrePerDay)
{
	double Total = 0.0;

	for (const FRoadSegment& Segment : Network.GetSegments())
	{
		if (!Segment.bAlive)
		{
			continue;
		}
		// ProfileFor, not Segment.Profile: a segment with none falls back to the network's
		// DefaultProfile, and reading the field directly would bill those at nothing - see
		// URoadNetwork::ProfileFor, which exists because two call sites made exactly that
		// mistake in different directions.
		if (const URoadProfile* Profile = Network.ProfileFor(Segment))
		{
			Total += MetresFromUu(SegmentLengthUu(Network, Segment)) * Profile->UpkeepPerMetrePerDay;
		}
	}

	for (const FEntityInstance& Entity : Network.GetEntities())
	{
		if (Entity.bAlive && Entity.Definition != nullptr)
		{
			Total += Entity.Definition->UpkeepPerDay;
		}
	}

	for (const FApronSurface& Apron : Network.GetAprons())
	{
		if (Apron.bAlive)
		{
			Total += PolygonAreaSquareMetres(Apron.Outline) * ApronRatePerSquareMetrePerDay;
		}
	}

	return Total;
}
