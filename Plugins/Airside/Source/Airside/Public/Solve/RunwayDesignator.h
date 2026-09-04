#pragma once

#include "CoreMinimal.h"

/**
 * A runway's NAME, derived from the direction it points.
 *
 * A runway is not named, it is measured: the designator is the magnetic bearing of the
 * centreline in tens of degrees, so a strip on 093 is runway 09 and the same strip walked
 * the other way is 27. Both numbers are facts about the geometry, which is why nothing here
 * stores them - move a runway and its name follows, because there is nowhere for a stale one
 * to hide.
 *
 * NORTH IS +X, and variation is assumed zero. The project has no magnetic model, so "magnetic
 * bearing" and "true bearing" are the same number here; the day a variation is wanted it
 * subtracts in Designate and nothing else moves. Stated rather than implied because a
 * designator that silently used true north would be wrong at every real aerodrome and wrong
 * by an amount nobody would notice until they compared with a chart.
 *
 * PARALLEL RUNWAYS ARE NOT HANDLED. Two runways on the same bearing take L/C/R suffixes, and
 * deciding which is which needs to know about the other one - a question about an airport,
 * not about a direction. This namespace answers only what a single direction is called.
 *
 * Dependency-free like the rest of Solve/: a direction in, a number out.
 */
namespace RunwayDesignator
{
	/**
	 * The designator for departing along Direction, 1 to 36.
	 *
	 * Rounds to the nearest ten degrees, and reports 36 rather than 0 for anything that
	 * rounds to north - there is no runway 00. Returns 0 only for a direction with no
	 * bearing at all, which the caller must treat as "no runway here" rather than as north.
	 */
	AIRSIDE_API int32 Designate(const FVector2D& Direction);

	/** The number at the other threshold: the same strip, walked the other way. */
	AIRSIDE_API int32 Reciprocal(int32 Designator);

	/** Zero-padded, as it is painted: "09", "27", "36". Empty for an invalid designator. */
	AIRSIDE_API FString ToText(int32 Designator);

	/**
	 * Both ends, low number first, as a runway is spoken of: "09/27".
	 *
	 * Low first because that is the convention on charts and signage, and because it makes
	 * the name stable when the segment's stored direction is reversed by an edit - the same
	 * strip must not become "27/09" because a node was dragged.
	 */
	AIRSIDE_API FString ToPairText(const FVector2D& Direction);
}
