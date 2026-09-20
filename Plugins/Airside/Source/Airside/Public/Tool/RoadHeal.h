#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RoadPlacement.h"

class URoadNetwork;

/**
 * Everything deleting one node would do, worked out before any of it happens.
 *
 * Computed rather than performed so the overlay can show it, and so a deletion that
 * cannot be healed legally can be refused whole instead of half-applied.
 */
struct FRoadDeletionPlan
{
	/** False means the deletion must be refused; Refusal says why. */
	bool bValid = false;

	ERoadPlacement Refusal = ERoadPlacement::Valid;

	/** The neighbour that could not be rejoined, when Refusal is set. */
	FRoadNodeId RefusedNeighbour;

	FRoadNodeId Target;

	/** Who the stranded arms rejoin. Unset when nothing needs rejoining. */
	FRoadNodeId Anchor;

	/** Neighbours to connect to Anchor, in a deterministic order. */
	TArray<FRoadNodeId> Rejoin;

	/** Segments the deletion removes. */
	TArray<FRoadSegmentId> Doomed;

	/** Neighbours left holding no road at all, which go with it. */
	TArray<FRoadNodeId> Swept;

	/**
	 * The cross-section the rejoins are laid with - the road being healed, not the level's
	 * default.
	 *
	 * ON THE PLAN BECAUSE THE PLAN IS JUDGED WITH IT. It was computed inside
	 * PlanNodeDeletion, used to validate each rejoin against a scratch graph, and then
	 * thrown away: URoadEditFacade::DeleteNode laid the real segment with
	 * ARoadNetworkActor::ResolveProfile() instead - the level's default taxiway. So a heal
	 * was approved for one cross-section and performed with another, which is the same
	 * two-answers shape URoadNetwork::SplitSegment's comment records about the ghost.
	 *
	 * Visible on any road that is not the default width; FATAL on a runway, whose relaid
	 * strip simply stopped being a runway - URoadNetwork::IsRunwaySegment reads the profile
	 * and nothing else. Found while fixing "no way to disconnect a taxiway from a runway",
	 * not by that report.
	 *
	 * Null when there is nothing to rejoin, and the facade falls back to its own default
	 * as before.
	 */
	TObjectPtr<URoadProfile> HealProfile;
};

/**
 * Deleting a node without leaving the roads that met there amputated.
 *
 * Two rules, because the two cases are genuinely different:
 *
 *   Degree 2 - ALWAYS heal. Removing a point from the middle of a road has to leave the
 *   road connected, whether or not its ends happen to be junctions. This is the classic
 *   case every road editor implements and there is nothing ambiguous about it.
 *
 *   Degree 3 or more - rejoin ONLY the neighbours the deletion would strand, to the
 *   neighbour that keeps the most roads. Reattaching a neighbour that still has roads of
 *   its own would not be healing; it would invent a road that never existed. It is also
 *   what would make one unlucky neighbour absorb every arm of a large junction, which is
 *   precisely the shape the fillet solver handles worst.
 *
 * Anything left with no road after all that is swept, because a node holding no geometry
 * is not something a deletion destroys - it is litter the deletion left behind.
 *
 * JUDGED AT THE REJOINING END ONLY, and deliberately not also at the anchor. Validating
 * the anchor as well is the obvious way to stop it accumulating arms at impossible angles,
 * and it was tried: it refuses the motivating case. Two stranded arms reattaching to one
 * anchor arrive from broadly the same direction almost by definition - the bug report's own
 * shape puts them about 16 degrees apart, inside a 25 degree limit - so the anchor check
 * turns the common heal into a refusal.
 *
 * The consequence, stated rather than hidden: an anchor CAN end up with arms closer together
 * than MinTurnDegrees, which is where the fillet solver clamps (spec section 12, K1). That is
 * visible in the preview before the click and undoable after it. What the rejoining-end check
 * does still catch is the damage that is not merely untidy - a rejoin shorter than the solver
 * can trim, a duplicate of a road already there, and a genuine hairpin where the rejoining
 * node keeps arms of its own.
 *
 * PLANNED AGAINST THE GRAPH AS IT WILL BE, not as it is. Validating a rejoin against the
 * live graph refuses almost every heal: the arm being judged at is the one about to be
 * deleted, it points nearly along the replacement, and the turn rule reads that as a
 * hairpin. So the plan is computed on a copy with the target already removed, and each
 * accepted rejoin is applied to that copy before the next is judged - otherwise two arms
 * landing on top of each other at the anchor is never noticed.
 */
namespace RoadHeal
{
	AIRSIDE_API FRoadDeletionPlan PlanNodeDeletion(const URoadNetwork& Network,
		FRoadNodeId Target, const FRoadPlacementLimits& Limits);

	/** How many roads Neighbour keeps once Target and its segments are gone. */
	AIRSIDE_API int32 DegreeWithout(const URoadNetwork& Network, FRoadNodeId Neighbour, FRoadNodeId Target);
}
