#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

class URoadNetwork;

/**
 * Puts each placed entity's SERVICE LOOP - and a spur from every service anchor to it - into
 * the guideline graph.
 *
 * WHY A LANE AT ALL is UEntityDefinition::ServiceLoop's business; this is only where it
 * reaches the graph. What matters here is that all of it is DERIVED, so it is swept and
 * remade by the ordinary rebuild, follows the stand when the stand moves, and goes when the
 * stand goes - the same lifecycle as any other derived edge, and the reason no instance
 * stores a loop and no saved level needs migrating.
 *
 * RUNS BEFORE FAnchorLink, which then joins each lane to a road. The lane has to exist before
 * anything can link it, and a service anchor spurred to the lane is already joined by the
 * time FAnchorLink asks - so it is skipped there rather than casting a ray at a road on the
 * far side of the aeroplane.
 *
 * IDEMPOTENT, because the two callers cannot promise the sweep ran in between: the presenter
 * always rebuilds first, and a test may call FAnchorLink twice to check a second pass adds
 * nothing. An entity that already owns live lane edges is left alone rather than given a
 * second lane, detected from FGuidelineEdge::ServiceLoopOwner - the same mark the link search
 * uses to avoid joining a lane to itself.
 */
struct AIRSIDE_API FServiceLoopBuild
{
	/**
	 * Physical width given to a lane nobody paints, uu.
	 *
	 * A number rather than a profile lookup because there is no surface here to read one
	 * from: the lane is invisible by design, and FGuidelineEdge::Width drives marking
	 * geometry and clearance, neither of which this has. Four metres is a service road's
	 * lane, which is what a lane round a stand is.
	 */
	static constexpr double LaneWidth = 400.0;

	/** What one pass laid, and what the link search needs to know about it. */
	struct FResult
	{
		/**
		 * Every node of every lane and spur now in the graph.
		 *
		 * FAnchorLink excludes these as link TARGETS, exactly the way it excludes anchor
		 * nodes: a lane is itself a vehicle guideline, so without this a lane would join
		 * itself four metres away, every stand would read as connected, and no truck would
		 * ever route anywhere.
		 */
		TSet<FGuidelineNodeId> Nodes;

		/** Per entity, the edges of its lane - what the link search measures FROM. */
		TMap<FEntityInstanceId, TArray<FGuidelineEdgeId>> Lanes;

		int32 LoopsBuilt = 0;
		int32 SpursBuilt = 0;
	};

	static FResult Build(URoadNetwork& Network);
};
