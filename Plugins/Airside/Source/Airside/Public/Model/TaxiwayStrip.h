#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Solve/IcaoCode.h"

class URoadNetwork;

/**
 * The clearance strip beside every taxiway, as a keep-out question: does this footprint stand
 * where a taxiing wing sweeps? (taxiway clearance strip spec, 2026-09-28)
 *
 * ONE QUERY FOR EVERYTHING PLACED - stands now, roads and building plots in stage 3 - rather
 * than a rule per object type, so a road with buildings on it needs no special case: each
 * footprint asks the same question. World-free, so it is tested with a bare URoadNetwork.
 */
namespace TaxiwayStrip
{
	/** One taxiway whose strip a footprint enters, and how far. */
	struct FIntrusion
	{
		FRoadSegmentId Taxiway;
		/** The taxiway's letter - what set the strip. */
		EIcaoCode Letter = EIcaoCode::A;
		/** The strip's width, uu - IcaoCode::TaxiwayStripForWidth of the pavement. */
		double Required = 0.0;
		/** How far inside the strip's outer edge the footprint reaches, uu. */
		double Depth = 0.0;
	};

	/**
	 * How far a footprint may reach past a strip edge before it counts, uu. One centimetre,
	 * the facade's OverlapToleranceUu, for the same reason: a stand the tool placed exactly on
	 * the edge must not be refused by the sampled curve's float noise.
	 */
	inline constexpr double ToleranceUu = 1.0;

	/**
	 * Does this segment carry aircraft and nothing else? THE taxiway-profile rule, moved here
	 * from PlotGesture::IsTaxiway (which now forwards) so Model/ can ask it - Model may not
	 * include Tool/. A runway passes this too; HasStrip is the one that excludes it.
	 */
	AIRSIDE_API bool IsAircraftOnly(const URoadNetwork& Network, FRoadSegmentId Id);

	/** A taxiway with a strip: aircraft only, and not a runway (runways have their own rules). */
	AIRSIDE_API bool HasStrip(const URoadNetwork& Network, FRoadSegmentId Id);

	/** The strip each side of this segment, uu; 0 for anything HasStrip refuses. */
	AIRSIDE_API double StripWidthOf(const URoadNetwork& Network, FRoadSegmentId Id);

	/**
	 * The deepest strip intrusion of a closed footprint polygon (any winding), or unset when it
	 * is clear of every strip by ToleranceUu. Pavement counts as strip: a footprint over the
	 * taxiway itself intrudes by the whole strip and more.
	 *
	 * DEEPEST, NOT ALL: every caller so far reports one reason, and the deepest is the one to
	 * fix first. A caller that needs the list is the day this grows one.
	 */
	AIRSIDE_API TOptional<FIntrusion> WorstIntrusion(const URoadNetwork& Network, TConstArrayView<FVector2D> Footprint);
}
