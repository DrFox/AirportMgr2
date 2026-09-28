#pragma once

#include "CoreMinimal.h"

/**
 * Window placement geometry for UUiWindowHost - plain functions over FBox2D, no widgets, so every
 * rule is table-tested (UI library step 2, spec 2026-09-28 section 2). All values are in the
 * host's local units, which is what its canvas slots are positioned in.
 */
namespace WindowSnap
{
	/**
	 * Top-left for a window of Size dragged to Proposed: clamped inside Bounds, then each axis
	 * snapped onto the nearest edge within Distance - Bounds' own edges, and the edges of every
	 * Other that FACES the window (overlaps it, give or take Distance, on the other axis). A
	 * neighbour far above must not pull a window sideways onto a line nobody can see connecting them.
	 */
	AIRPORTMGR_API FVector2D Place(FVector2D Proposed, FVector2D Size, const FBox2D& Bounds,
		TConstArrayView<FBox2D> Others, double Distance);

	/**
	 * Size for a window at TopLeft resized to Proposed: at least MinSize, no further than Bounds
	 * (the minimum wins where they disagree), then its right and bottom edges snapped the same way.
	 */
	AIRPORTMGR_API FVector2D Resize(FVector2D TopLeft, FVector2D Proposed, FVector2D MinSize,
		const FBox2D& Bounds, TConstArrayView<FBox2D> Others, double Distance);
}
