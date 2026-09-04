#pragma once

#include "CoreMinimal.h"

class URoadProfile;
class URoadMaterialSet;

/**
 * A profile's band boundaries expressed as positions along a cut line.
 *
 * Every array is parallel and ordered from the RIGHT edge to the LEFT, because that is
 * the direction FMath::Lerp(RightCut, LeftCut, Alpha) travels. Alphas always start at
 * exactly 0 and end at exactly 1, so the outermost boundaries reproduce the solver's
 * stored cut vertices without arithmetic - which is what lets them weld bitwise.
 */
struct AIRSIDE_API FRoadProfileBands
{
	/** Lerp parameter along the cut line, ascending, first exactly 0 and last exactly 1. */
	TArray<double> Alphas;

	/** Signed lateral offset in uu at each boundary: negative right, positive left. */
	TArray<float> Laterals;

	/**
	 * Material id per BAND, so Alphas.Num() - 1 entries, in the same right-to-left order.
	 * SlotIndices[i] skins the band between Alphas[i] and Alphas[i + 1].
	 *
	 * Always a valid index into the material set, never INDEX_NONE: an unresolved name
	 * falls back to 0 and is counted in UnresolvedSlots instead. An out-of-range id is not
	 * a diagnosable error downstream - the scene proxy silently discards the triangle.
	 */
	TArray<int32> SlotIndices;

	/**
	 * Bands whose MaterialSlot the set did not declare, and which therefore fell back to 0.
	 *
	 * A count rather than a log line, because a test asserting on the fallback should not
	 * have to parse output. The builder logs as well, for the human.
	 */
	int32 UnresolvedSlots = 0;

	/**
	 * Where the centreline sits between Alphas[0] and Alphas.Last(). Computed here, from
	 * the same half-widths the laterals use, so no caller re-derives it.
	 */
	double CentrelineAlpha = 0.5;

	/**
	 * Boundaries for a profile, or the degenerate two-boundary case for a null one.
	 *
	 * A null Materials is the supported single-material state, not an error: every band
	 * resolves to 0, which is what the network renders with today.
	 */
	static FRoadProfileBands FromProfile(const URoadProfile* Profile,
		const URoadMaterialSet* Materials = nullptr);

	/** The band containing Alpha, or INDEX_NONE when there are no bands. */
	int32 BandAt(double Alpha) const;

	/**
	 * The material id for a band, or 0 when this table carries no slots.
	 *
	 * Every emission site goes through here rather than indexing SlotIndices, so an id
	 * outside the material set cannot reach the mesh. FDynamicMeshSceneProxy counts a
	 * triangle whose id is out of range into NO render buffer: the triangle vanishes and
	 * nothing is logged, which is the least diagnosable failure available here.
	 */
	int32 SlotForBand(int32 BandIndex) const
	{
		return SlotIndices.IsValidIndex(BandIndex) ? SlotIndices[BandIndex] : 0;
	}
};
