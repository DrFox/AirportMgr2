#pragma once

#include "CoreMinimal.h"
#include "Model/BuildPurse.h"
#include "Model/RunwayFacts.h"

class URoadProfile;
class UEntityDefinition;
class URoadNetwork;
struct FRoadSegment;

/**
 * How much of it there is, at the authored rate. The Airside half of the money; UPricing in
 * AirportOps is the other half, and answers what it costs.
 *
 * THE ONE PLACE uu BECOME METRES FOR MONEY. Every rate in this codebase is per metre or per
 * square metre, and every quote divides by 100 (or 10 000 for an area) exactly once - here. A
 * second site doing the conversion is how a taxiway comes to cost a hundred times too much
 * with nothing to say which of the two was wrong.
 *
 * THE CHORD, NOT THE ARC. SegmentLengthUu measures straight between the two nodes even for a
 * curved segment. It is a few percent short on a bend, and it is used by BOTH the build quote
 * and the daily upkeep - which is the property that matters. A more accurate arc length used
 * in one place and not the other would mean a segment's upkeep quietly disagreed with what its
 * construction charged, and the player would have no way to see why.
 */
namespace BuildCost
{
	/** Straight between the segment's two nodes, uu. Zero if either end is not live. */
	AIRSIDE_API double SegmentLengthUu(const URoadNetwork& Network, const FRoadSegment& Segment);

	/** LengthUu of pavement at Profile's rate, one line on Surface - Pavement::RateFactor is
	 *  applied by FBuildLine::Amount, not here (the factor table and why it is a factor live
	 *  with it; #356's GrassRateFactor folded into it). A runway's Surface is FRunwayFacts',
	 *  not this parameter's default - QuoteForRunway passes Facts.Surface through explicitly,
	 *  so a caller that omits it is asking for tarmac on purpose, not by the runway's nature. */
	AIRSIDE_API FBuildQuote ForSegment(const URoadProfile& Profile, double LengthUu,
		EPavement Surface = EPavement::Tarmac);

	/** One placed thing - a stand, a depot - at its definition's rate. No pavement of its own -
	 *  see FBuildLine::Pavement. */
	AIRSIDE_API FBuildQuote ForEntity(const UEntityDefinition& Definition);

	/** The polygon's area at RatePerSquareMetre, on Surface if it has one. Winding-independent -
	 *  see the .cpp. */
	AIRSIDE_API FBuildQuote ForApron(TConstArrayView<FVector2D> Outline, double RatePerSquareMetre,
		TOptional<EPavement> Surface);

	/**
	 * Two quotes as one: A's lines then B's, appended - the shape PlaceEntityInPlot and
	 * QuoteStand both used to hand-sum before this existed (issue #193, fix round 1). What
	 * becomes "{A} + {B}".
	 */
	AIRSIDE_API FBuildQuote Combine(FBuildQuote A, const FBuildQuote& B);

	/**
	 * One day of owning everything currently standing, at the authored rates.
	 *
	 * WALKS THE WHOLE NETWORK, once a game day. That is cheap at a day's interval and it keeps
	 * the figure honest: a running total maintained by every mutator would be a second source
	 * of truth about what exists, and the mutator that forgot to update it would be invisible
	 * until the upkeep bill drifted away from the airport.
	 *
	 * A STAND IS TWO TERMS: its definition's flat UpkeepPerDay (the equipment) plus its pad's
	 * area at ApronRatePerSquareMetrePerDay times its pavement's factor (2026-09-27). A depot's
	 * plot is not a pad and bills no area term.
	 * ENFORCED BY: Airside.Build.BuildCostStandPadUpkeepByArea
	 */
	AIRSIDE_API double DailyUpkeep(const URoadNetwork& Network,
		double ApronRatePerSquareMetrePerDay);

	/** The polygon's area in square metres, winding-independent. Shared by the quote and the
	 *  upkeep so the two can never measure the same apron differently. */
	AIRSIDE_API double PolygonAreaSquareMetres(TConstArrayView<FVector2D> Outline);
}
