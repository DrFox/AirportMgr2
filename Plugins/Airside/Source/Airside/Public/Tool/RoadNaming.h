#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

class URoadNetwork;

/**
 * What to CALL a road in something the player reads.
 *
 * ONE HOME for a classification the codebase had twice: URoadNetwork::IsRunwaySegment, and
 * FPlotPlaceTool's file-local IsServiceRoad, which asks the profile's guidelines whether
 * anything admits a ground vehicle. A guide label saying "parallel to the taxiway" over a
 * service road is the kind of wrong that survives review, because each reader assumes the
 * other's definition.
 *
 * NAMED BY WHAT IT ADMITS, not by an asset name: a URoadProfile has no display name at all
 * (only a MaterialSlot FName), and the thing the player is lining up with is a taxiway or a
 * service road regardless of which cross-section asset drew it.
 */
namespace RoadNaming
{
	/**
	 * "runway 18/36", "the service road", "the taxiway". Empty for a segment that is not live.
	 *
	 * The runway form carries BOTH ends, low first, because that is how a runway is spoken of
	 * and because it stays stable when an edit reverses the segment's stored direction - see
	 * RunwayDesignator::ToPairText's own comment.
	 */
	AIRSIDE_API FString Describe(const URoadNetwork& Network, FRoadSegmentId Segment);
}
