#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/RunwayFacts.h"

class URoadNetwork;

/**
 * The runway reads a runway network answers, as free functions rather than URoadNetwork
 * methods. URoadNetwork owns the graph (nodes, segments, guideline edges) and IsRunwaySegment/
 * ProfileFor - the one-line rule every one of these, and the traffic model, asks per segment -
 * stay there as repository methods. Everything below DERIVES a runway fact by reading the
 * graph through URoadNetwork's own public surface, which is what a query belongs beside
 * rather than inside the thing it queries; this issue's review found the repository carrying
 * ~450 lines of these across roughly a third of its interface, atop a header contract naming
 * an IRoadCommand that does not exist (see RoadNetwork.h's own class comment for what actually
 * gates mutation now).
 *
 * URoadNetwork forwards every one of these at its old name and signature, so none of the
 * dozen-plus existing call sites for RunwayChain/RunwayFactsFor/RunwayExtentAt change.
 */
namespace RunwayQuery
{
	/**
	 * Every segment continuous with Seed through nodes joining exactly two runway segments,
	 * returning the segments walked rather than the ends - RunwayExtentAt reads its
	 * thresholds off this chain. Empty when Seed is not a live runway. Includes Seed.
	 *
	 * This is what a runway IS to the occupancy table: a landing holds every segment of the
	 * chain, a holding-position names one, and the arbiter expands it here - so an exit added to a
	 * runway after the hold bar was placed still protects the whole strip.
	 */
	AIRSIDE_API TArray<FRoadSegmentId> RunwayChain(const URoadNetwork& Network, FRoadSegmentId Seed);

	/**
	 * RunwayChain(Seed), or a one-segment chain of just Seed when that comes back empty.
	 *
	 * Seed dropped to a taxiway under a claim still made or a bar still placed should
	 * protect the one segment named rather than nothing at all - one implementation
	 * instead of every caller spelling out the same fallback. See #86.
	 */
	AIRSIDE_API TArray<FRoadSegmentId> RunwayChainOrSeed(const URoadNetwork& Network, FRoadSegmentId Seed);

	/**
	 * The surface and approach class of the runway Seed belongs to.
	 *
	 * Reads Seed's OWN segment: URoadNetwork::SetRunwayFacts writes every member of the chain
	 * and the split copies them, so any member answers for the strip and no walk is needed
	 * here. A dead or non-runway seed reads the struct default - the same answer an
	 * unclassified runway gives, so a caller that must tell the two apart asks
	 * URoadNetwork::IsRunwaySegment first.
	 */
	AIRSIDE_API FRunwayFacts RunwayFactsFor(const URoadNetwork& Network, FRoadSegmentId Seed);

	/**
	 * Does this guideline node stand ON the strip of the runway chain seeded at Seed?
	 *
	 * The question spec §3.1's fourth route asks: an agent crossing a runway holds it until
	 * its TAIL is clear of the strip, and "clear" cannot be "past the next node" because a
	 * node in the middle of a crossing sits on the runway itself. Geometry answers it; a bar
	 * on the far side would not, because a player may place one bar or none.
	 *
	 * True when, for ANY segment of the chain, the node is within that segment's own profile
	 * half width of its centreline AND its projection falls inside the segment's A..B extent
	 * with that half width of slack at each end - a junction cut puts the node a little
	 * beyond the road node, and this is the slack that admits it anyway.
	 *
	 * OutChainHalfWidth, when given, reports the LARGEST half width in the chain whether or
	 * not the node is on it. That is how far past the last on-strip node a tail must travel
	 * to be clear of the widest part of the strip, which is the caller's next question when
	 * the answer here is true.
	 */
	AIRSIDE_API bool IsGuidelineNodeOnRunway(const URoadNetwork& Network, FGuidelineNodeId Node,
		FRoadSegmentId Seed, double* OutChainHalfWidth = nullptr);

	/**
	 * The same question about a bare POSITION, which is where the rule actually lives.
	 *
	 * IsGuidelineNodeOnRunway is this function applied to a node's position, and exists
	 * because most callers have a node. The one that does not is the crossing hold arming
	 * itself (spec §3.1, refined during Task 7): "is the agent's own centre already on the
	 * strip" is asked of an agent standing between two nodes, and there is no node to hand.
	 * One implementation so the two answers cannot drift - the second-evaluator rule.
	 */
	AIRSIDE_API bool IsPointOnRunway(const URoadNetwork& Network, const FVector2D& Position,
		FRoadSegmentId Seed, double* OutChainHalfWidth = nullptr);

	/**
	 * The same question against a chain ALREADY WALKED, for a caller asking it of many
	 * points on one chain (RunwayExitNodes, one per guideline node) - RunwayChain(Seed)
	 * walks the graph, and paying for that walk again per point would be asking the same
	 * question about the network a hundred times to answer it about a hundred positions.
	 */
	AIRSIDE_API bool IsPointOnRunway(const URoadNetwork& Network, const FVector2D& Position,
		const TArray<FRoadSegmentId>& Chain, double* OutChainHalfWidth = nullptr);

	/**
	 * If Near sits on a runway, reports the departure from the threshold nearest it.
	 *
	 * WALKS THE WHOLE RUNWAY, not the one segment it lands on. Adding an exit splits a runway,
	 * so by the time it is useful it is several segments - and a roll computed from one piece
	 * would refuse a departure the strip can easily take. The walk follows nodes joining
	 * exactly two continuous segments, which is what an uninterrupted runway looks like from
	 * the graph's point of view.
	 *
	 * A runway is recognised by its PROFILE - see URoadProfile::bContinuousThroughJunctions -
	 * so nothing here needs a runway type or a flag on the segment.
	 *
	 * OutEnd.Direction points from the near threshold toward the far one: the way you depart
	 * having backtracked to that end. OutEnd.Seed is the runway segment whose end was
	 * nearest Near. False when Near is not on a runway at all, and OutEnd is untouched.
	 */
	AIRSIDE_API bool RunwayExtentAt(const URoadNetwork& Network, const FVector2D& Near, FRunwayEnd& OutEnd);

	/**
	 * The runway threshold nearest a point, however far away it is.
	 *
	 * THE SAME SEARCH AS RunwayExtentAt WITHOUT THE PROXIMITY TEST, which is exactly the
	 * difference between the two questions. A departure asks "did my taxi end on a runway",
	 * and must hear no everywhere else - that test exists because without it every route
	 * armed a departure at the only runway on the field. An arrival asks "which runway am I
	 * landing on", of a click that is deliberately nowhere near one.
	 *
	 * The threshold returned is the end NEAREST the query and the direction runs away from
	 * it, so an aircraft lands toward the far end - the same convention as a departure, and
	 * the reason both can share the walk. OutEnd.Seed is the runway segment whose end was
	 * nearest Near.
	 */
	AIRSIDE_API bool NearestRunwayThreshold(const URoadNetwork& Network, const FVector2D& Near, FRunwayEnd& OutEnd);

	/**
	 * Guideline nodes lying on the runway chain Seed belongs to, ordered by distance from
	 * Threshold along Direction.
	 *
	 * THE EXITS, without needing an exit to be a thing. A runway is continuous through
	 * junctions, so a taxiway joining it already puts a guideline node on the centreline;
	 * asking which nodes lie along the strip therefore finds every way off it, including
	 * ones the player drew after the runway existed.
	 *
	 * A node qualifies by IsPointOnRunway(Node.Position, Seed) - the one evaluator of "on
	 * the strip", tested per SEGMENT of the chain against that segment's OWN width (#87).
	 * The two callers used to compute their own HalfWidth as the max over every continuous
	 * segment on the whole airport, so a 60 m runway anywhere widened the exit test on an
	 * 18 m strip.
	 *
	 * THRESHOLD AND DIRECTION ARE THE CALLER'S OWN, not re-derived from Seed here (fixed
	 * 2026-09-13): a seed has two ends and this function has no way to know which one the
	 * aircraft is actually at. Deriving them from Seed's own A node - a draw-direction
	 * artefact - silently reversed the ordering and the MinDistance filter on any strip not
	 * drawn threshold-first, which nothing forces a player to do. Seed still decides
	 * membership and width (IsPointOnRunway); Threshold/Direction decide direction.
	 *
	 * MinDistance is what makes the answer useful to an arrival: an exit before the aircraft
	 * can possibly have slowed down is not an exit it can take.
	 */
	AIRSIDE_API TArray<FGuidelineNodeId> RunwayExitNodes(const URoadNetwork& Network, FRoadSegmentId Seed,
		const FVector2D& Threshold, const FVector2D& Direction, double MinDistance);
}
