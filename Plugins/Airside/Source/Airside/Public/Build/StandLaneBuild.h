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

	// TangentRunFor IS DELETED, 2026-09-16, and where its answer comes from now is the point.
	//
	// It sized the run of a tangential join from the GAP, capped at a preferred 800 uu, and that
	// figure was measured against an anchor spur's 1000-1400 uu gaps. A road link's gaps are
	// three to five times that, and the same 800 delivered 40 uu of radius at 4 m and 67 at 54
	// against the 699 a service vehicle's steering lock demands - a curve no truck can follow at
	// any speed, which is the defect this whole redesign exists to delete. A run cannot be a
	// constant tuned against one distance.
	//
	// GuidelineGeom::ShiftDeflectionFor answers it instead, by INVERTING the radius relation:
	// give it the radius that must be cleared and the gap to cross and it returns the deflection
	// and the run that deliver it. It lives beside CornerRunFor because it is that formula read
	// the other way round. FAnchorLink::Join is the caller.

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
