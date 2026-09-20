#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Solve/GuideArbiter.h"

class URoadNetwork;

/**
 * What to CALL a road in something the player reads, and which guide COLUMN it belongs to.
 *
 * ONE HOME for a classification the codebase had twice: URoadNetwork::IsRunwaySegment, and
 * FPlotPlaceTool's file-local IsServiceRoad, which asks the profile's guidelines whether
 * anything admits a ground vehicle. A guide label saying "parallel to the taxiway" over a
 * service road is the kind of wrong that survives review, because each reader assumes the
 * other's definition.
 *
 * ONE CLASSIFICATION AND NOT A THIRD COPY - which is what 2026-09-20 turned on. Splitting
 * EReference::Road into Taxiway and ServiceRoad asked four guide sources the same question
 * Describe had been answering all along, and the cheap move was for each to consult the
 * profile's guidelines itself. Describe is BUILT ON ReferenceOf instead, so the label a
 * player reads and the button that switches it off cannot disagree about one segment.
 *
 * NAMED BY WHAT IT ADMITS, not by an asset name: a URoadProfile has no display name at all
 * (only a MaterialSlot FName), and the thing the player is lining up with is a taxiway or a
 * service road regardless of which cross-section asset drew it.
 */
namespace RoadNaming
{
	/**
	 * Which guide column a segment belongs to - Runway, ServiceRoad or Taxiway. False, with Out
	 * untouched, for a segment that is not live.
	 *
	 * RETURNS BOOL rather than growing a "no idea" enum member, so a caller has to branch on
	 * it: CLAUDE.md's rule about honouring anything that fills an out-parameter. There is no
	 * honest EReference for a dead segment, and World or ThisGesture would each be a lie that
	 * some cell of the grid would happily accept.
	 *
	 * THE RUNWAY QUESTION IS ASKED FIRST inside, because a runway's cross-section may well
	 * admit a vehicle and the service-road test would then claim it.
	 */
	AIRSIDE_API bool ReferenceOf(const URoadNetwork& Network, FRoadSegmentId Segment,
		SnapGuide::EReference& Out);

	/**
	 * "runway 18/36", "the service road", "the taxiway". Empty for a segment that is not live.
	 *
	 * The runway form carries BOTH ends, low first, because that is how a runway is spoken of
	 * and because it stays stable when an edit reverses the segment's stored direction - see
	 * RunwayDesignator::ToPairText's own comment.
	 */
	AIRSIDE_API FString Describe(const URoadNetwork& Network, FRoadSegmentId Segment);
}
