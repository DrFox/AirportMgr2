#pragma once

#include "CoreMinimal.h"

class URoadProfile;

/**
 * A profile's band boundaries expressed as positions along a cut line.
 *
 * Every array is parallel and ordered from the RIGHT edge to the LEFT, because that is
 * the direction FMath::Lerp(RightCut, LeftCut, Alpha) travels. Alphas always start at
 * exactly 0 and end at exactly 1, so the outermost boundaries reproduce the solver's
 * stored cut vertices without arithmetic - which is what lets them weld bitwise.
 */
struct ROADNET_API FRoadProfileBands
{
	/** Lerp parameter along the cut line, ascending, first exactly 0 and last exactly 1. */
	TArray<double> Alphas;

	/** Signed lateral offset in uu at each boundary: negative right, positive left. */
	TArray<float> Laterals;

	/** Boundaries for a profile, or the degenerate two-boundary case for a null one. */
	static FRoadProfileBands FromProfile(const URoadProfile* Profile);
};
