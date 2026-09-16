#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

class URoadNetwork;

/**
 * Puts each placed entity's SERVICE LANE - the closed cycle its equipment boxes are painted
 * on - into the guideline graph.
 *
 * WHY A LANE AT ALL is UEntityDefinition::ServiceLane's business; this is only where it
 * reaches the graph. What matters here is that all of it is DERIVED, so it is swept and
 * remade by the ordinary rebuild, follows the stand when the stand moves, and goes when the
 * stand goes - the same lifecycle as any other derived edge, and the reason no instance
 * stores a lane and no saved level needs migrating.
 *
 * THE ANCHORS ARE ON THE LANE, not spurred to it, since 2026-09-16. The old ring ran outboard
 * of the wingtips with a stub from every service anchor into it, and a stub needs a 90 degree
 * turn: 989 uu of run on each of its two arms against the 990 uu of depth there was between
 * the ring and the box row. So the lane was moved INSIDE the wingtip, onto the row itself, and
 * an anchor is now a waypoint the lane passes straight THROUGH. Nothing turns into a box.
 *
 * RUNS BEFORE FAnchorLink, which joins each lane to a road. The lane has to exist before
 * anything can link it.
 *
 * IDEMPOTENT, because the two callers cannot promise the sweep ran in between: the presenter
 * always rebuilds first, and a test may call FAnchorLink twice to check a second pass adds
 * nothing. An entity that already owns live lane edges is left alone rather than given a
 * second lane, detected from FGuidelineEdge::StandGeometryOwner - the same mark the link
 * search uses to avoid joining a lane to itself.
 */
struct AIRSIDE_API FStandLaneBuild
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

	/**
	 * How far along the lane a line joining it should slide from the point it is nearest.
	 *
	 * A LINE JOINS A LANE ALONG IT, NOT ACROSS IT. Sliding the join this far and putting the
	 * curve's control back at the nearest point makes the first leg run down the lane, so the
	 * curve leaves tangentially and there is no turn at the junction to take. Gap is how far
	 * off the lane the other end is.
	 *
	 * ONE CALLER: FAnchorLink::Join, where a road's connector leaves a DECLARED ENTRY. It was
	 * shared with the anchor spurs until 2026-09-16 and they are gone - an anchor is a waypoint
	 * ON the lane now and has nothing to spur. The 800 uu and the measurements that settled it
	 * are on PreferredTangentRun in StandLaneBuild.cpp; the gaps they were measured against are
	 * a spur's 1000-1400 uu, which is SHORTER than a road link's, and Join records what the
	 * figure delivers at a road's distance.
	 */
	static double TangentRunFor(double Gap);

	/** What one pass laid, and what the link search needs to know about it. */
	struct FResult
	{
		/**
		 * Every node of every lane now in the graph.
		 *
		 * FAnchorLink excludes these as link TARGETS, exactly the way it excludes anchor
		 * nodes: a lane is itself a vehicle guideline, so without this a lane would join
		 * itself four metres away, every stand would read as connected, and no truck would
		 * ever route anywhere.
		 */
		TSet<FGuidelineNodeId> Nodes;

		/** Per entity, the edges of its lane - what the link search measures FROM. */
		TMap<FEntityInstanceId, TArray<FGuidelineEdgeId>> Lanes;

		/**
		 * Per entity, the node laid at each of its declared Entry waypoints, in the
		 * definition's own order.
		 *
		 * WHERE A ROAD MAY JOIN, said by the asset rather than measured off the graph. The
		 * ring used to be searched for its nearest approach to a road and cut wherever that
		 * fell, which put entrances on bends, clustered three of them along one edge, and
		 * needed a whole side-walking apparatus to undo. An Entry is authored on the side it
		 * belongs to, once.
		 *
		 * TWO NODES PER ENTRY on the shipping stand, because its entries are authored at
		 * CORNERS and a rounded corner's own point carries no node - the bend's control sits
		 * there and its two ends sit back along the two legs. Which of the pair a road should
		 * join depends on which side the road is, which is the linking pass's question.
		 *
		 * FILLED ON THE IDEMPOTENT SKIP PATH TOO, and that is load-bearing rather than tidy.
		 * Build re-gathers Lanes and Nodes from the graph for a lane it did not lay this pass;
		 * an Entries that was filled only by the laying path would make this whole result
		 * INCONSISTENT on the second of two passes, so a stand laid on one and linked on the
		 * next would never be joined and no log would say why. See RecoverEntries in the .cpp.
		 *
		 * NOTHING IN THIS PASS READS IT. Task 5 of the stand routing work is the consumer;
		 * it is recorded here because this is the only place that knows which node a given
		 * waypoint became.
		 *
		 * NO KEY AT ALL for a definition that declares no entry, rather than an empty array:
		 * "this stand has no entries" is a real and reportable state, and a caller's Find()
		 * is the question, so the map only answers when it has an answer.
		 */
		TMap<FEntityInstanceId, TArray<FGuidelineNodeId>> Entries;

		int32 LanesBuilt = 0;
	};

	static FResult Build(URoadNetwork& Network);
};
