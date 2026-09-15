#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
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

	/**
	 * The radius every corner of the ring is rounded to, uu.
	 *
	 * A BOX IS HOW A LANE IS DESCRIBED, NOT HOW IT IS DRIVEN. UEntityDefinition::ServiceLoop
	 * stays four points, but a corner where two straight sides meet is a vertex whose heading
	 * changes instantly, and FSpeedProfile calls one of those untakeable at any speed.
	 *
	 * NOT IcaoCode::RadiusForLetter, which is what a PAINTED taxi line is swept at, sized for
	 * the largest aircraft a stand admits - 2500 uu for a Code C, on a lane four metres wide
	 * whose longest side is 5250. 750 uu is the SERVICE ROAD's junction fillet
	 * (URoadProfile::MakeServiceRoadTransient, and DA_RoadProfile_ServiceRoad beside it): one
	 * decision, so a lane is never tighter than the road feeding it. It clears the 471 uu a
	 * truck's steering lock allows by 1.6x.
	 *
	 * PUBLIC because it MOVES THINGS A CALLER CAN SEE. A road no longer joins a lane at the
	 * square corner of the definition - the ring turns in before it and never reaches it - and
	 * a test that had to hardcode how far would be a second statement of this decision.
	 */
	static constexpr double LaneTurnRadius = 750.0;

	/**
	 * How far along the ring a line joining it should slide from the point it is nearest.
	 *
	 * A LINE JOINS A LANE ALONG IT, NOT ACROSS IT. Sliding the join this far and putting the
	 * curve's control back at the nearest point makes the first leg run down the ring, so the
	 * curve leaves tangentially and there is no turn at the junction to take. Gap is how far
	 * off the ring the other end is.
	 *
	 * SHARED BY SPURS AND ROAD LINKS, because the two are the same thing with a different
	 * far end - an anchor or a road. A link that crossed square-on while its spurs swept was
	 * the last tight turn left on a stand, and the one the router kept choosing.
	 */
	static double TangentRunFor(double Gap);

	/**
	 * Walk Distance along the RING from (Edge, Param) and report where it lands. Negative
	 * walks the other way; the sign is read against Edge's own A-to-B sense.
	 *
	 * ACROSS EDGE BOUNDARIES, which is the whole reason it exists. A ring side is cut by every
	 * spur and every link that joins it, so "800 uu along the lane" routinely lands on a
	 * different EDGE from the one a line is nearest - and a walk that stopped at the end of
	 * its own edge would report no room where the ring has plenty.
	 *
	 * ONLY THE RING. Spurs and links hang off it and are stepped over, so a ring node carries
	 * exactly two ring edges and the way onward is never ambiguous.
	 *
	 * OutForward says which way the walk was travelling when it stopped, in the LANDING edge's
	 * own sense - which a caller needs, because that edge may be parameterised the opposite
	 * way round from the one it started on.
	 */
	static bool WalkRing(const URoadNetwork& Network, FGuidelineEdgeId Edge, double Param,
		double Distance, FGuidelineEdgeId& OutEdge, double& OutParam, bool& OutForward);

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
