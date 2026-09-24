#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"

class URoadNetwork;

/**
 * The PAINT of a two-lane road: a dashed white centre line between its lanes (spec
 * 2026-09-23 §7), as quads for the white paint layer the runway markings use.
 *
 * A Build/ class beside FRunwayMarkingBuilder and FHoldingPositionMarkingBuilder, and not
 * part of FRoadMeshBuilder, for their reason: paint that landed on a road vertex must not
 * weld to it - the bitwise-welded surface is the road's, and the paint only lies on it.
 *
 * ON THE CENTRELINE, which the drive side does not move - so flipping sides repaints
 * nothing. Between the segment's own cut lines, so no dash is painted inside a junction.
 */
struct AIRSIDE_API FRoadLaneMarkingBuilder
{
	/** Line width, uu. Exaggerated from a real 0.1 m, like the holding bars, so it reads from the build camera. Unjudged. */
	static constexpr double LineWidth = 20.0;
	/** A dash and the gap after it, uu - 3 m on, 6 m off. First guesses, unjudged until seen. */
	static constexpr double DashLength = 300.0;
	static constexpr double DashGap = 600.0;

	/**
	 * Appends the centre line of every live, solved segment whose profile carries an A->B
	 * lane and a B->A lane. Returns how many dashes were painted; OutSegments, if given,
	 * receives how many segments carried them.
	 */
	static int32 Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out, int32* OutSegments = nullptr);
};
