#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "ReverseTurn.generated.h"

/**
 * "A VEHICLE MAY BACK FROM ONE ARM OF THIS JUNCTION INTO ANOTHER" (spec 2026-09-26 §3) - a bay, or
 * a hammerhead's stub. The model states the permission; FRoadGuidelineBuilder derives the reverse
 * leg (bReverseLeg edges) and the exit from it on every rebuild, as it derives turn paths from a
 * junction. A hand-drawn reverse edge would not survive the rebuild that re-derives the lanes it
 * joins - which is why this is a record and not an edge.
 *
 * ARMS BY THEIR FAR NODES, not by segment handle: "the arm from Node towards FromFar" names the
 * same road after an edit that re-slots its segment, and it is what a caller placing nodes has in
 * hand (ARigTestCourse::LayYard, 2026-09-26). The builder resolves each to the segment between
 * the two nodes, and drops the record, logged, once either arm has gone.
 *
 * WHAT IS LAID: from the lane end at FromFar on the lane running Node -> FromFar (where a vehicle
 * pulling past stops), back along that lane towards Node, round a fillet onto the lane of the
 * IntoFar arm that runs back towards Node, and on to near IntoFar - the vehicle backs in on the
 * lane it will drive out on, so the exit is straight. Opposite arms make a straight bay.
 * ENFORCED BY: Airside.Build.ReverseTurn.EdgeIsFlaggedAndStartsAtTheStop, .StraightBayIsOneLine
 */
USTRUCT()
struct AIRSIDE_API FReverseTurn
{
	GENERATED_BODY()

	/** The junction the vehicle backs through. */
	UPROPERTY() FRoadNodeId Node;

	/** The far end of the arm it pulls past along - the pull-past is that arm's whole length. */
	UPROPERTY() FRoadNodeId FromFar;

	/** The far end of the arm it backs into: the bay, or the hammerhead's stub. */
	UPROPERTY() FRoadNodeId IntoFar;
};
