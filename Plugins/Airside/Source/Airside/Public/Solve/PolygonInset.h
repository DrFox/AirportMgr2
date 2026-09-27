#pragma once

#include "CoreMinimal.h"

/**
 * A simple polygon moved inward by a uniform distance, mitred at every corner.
 *
 * For painted bands inside a plot's outline - the fuel depot's hazard band is the ring
 * between an outline and its inset (FRoadMeshBuilder::AddApronRing). Refuses rather than
 * returning a wrong shape: a band that cannot fit is drawn as no band, never as a
 * self-crossing one.
 *
 * Dependency-free like the rest of Solve/: CoreMinimal.h only.
 */
namespace PolygonInset
{
	/**
	 * A corner whose mitre reaches further than this many Distances from the corner is too
	 * sharp to inset. 4 is a 29-degree corner; a drawn plot is a rectangle today
	 * (2026-09-27, PlotGesture), so this only guards a future freeform outline.
	 */
	inline constexpr double MaxMitreRatio = 4.0;

	/**
	 * Outer inset by Distance, one vertex per outer vertex, in the same order and winding -
	 * so edge i of the result is parallel to edge i of Outer and a ring can be built by
	 * pairing them. Either winding is accepted.
	 *
	 * False, OutInner left EMPTY, when Distance is not positive, Outer has fewer than three
	 * corners or no area, a corner is sharper than MaxMitreRatio allows, or the inset has
	 * collapsed (any edge reversed, or the area changed sign).
	 */
	AIRSIDE_API bool Inset(TConstArrayView<FVector2D> Outer, double Distance, TArray<FVector2D>& OutInner);

	/** Signed shoelace area; its sign is the winding. Shared so the two cannot disagree. */
	AIRSIDE_API double SignedArea(TConstArrayView<FVector2D> Polygon);
}
