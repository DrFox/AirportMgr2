#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

class URoadNetwork;

/**
 * How far a node's claim reaches along each edge that meets it.
 *
 * THE PROBLEM THIS SOLVES. The claim table reserves GRAPH resources - an edge interval, a
 * node, a runway surface - and two bodies on two different edges conflict only through the
 * node they share. A node used to be held while a body's centre was within half a footprint
 * of it. That is the right answer when the edges leave the node in different directions:
 * two bodies half a footprint down two diverging lines are already a footprint apart. It is
 * the wrong answer when the edges leave the node TOGETHER. A stand's sweep arc joins its
 * taxiway tangentially, and measured on the two-stand fixture it stays within one footprint
 * of the taxiway for 2454 of its 4058 uu. A departing aircraft was refused the join while an
 * arrival was on it, resumed the moment the arrival was 500 uu past, and drove the last
 * 2000 uu of the arc alongside the arrival's body - 429 uu apart at the closest, "through
 * each other" in play (2026-09-07). Airside.Model.Traffic.DepartureMeetsArrivalOnTaxiway
 * is that report as a test.
 *
 * THE RULE. The reach of node N along edge E is the distance s from N within which a body
 * s along E and a body s along some OTHER edge of N are within one footprint of each other
 * - measured by walking both sampled polylines outward from N, not modelled from angles.
 * Two bodies the same distance down a straight continuation are 2s apart, so a plain
 * node's reach is F/2 and nothing about an ordinary junction changes; a right-angle
 * junction gets F/sqrt(2); a tangent arc of radius R gets roughly sqrt(2 R F). Floored at
 * F/2, capped at the edge's length.
 *
 * WHY EQUAL DISTANCES AND NOT POINT-TO-LINE. Point-to-line makes every straight continuation
 * a whole footprint long - the nearest point of the edge BEHIND the node is the node itself
 * - and every follower through a split taxiway would then brake for a node its leader had
 * long cleared. Equal distance is exactly the geometry the F/2 rule was already assuming;
 * this is that rule with the assumption measured instead of taken on trust.
 *
 * WHY MODEL/ AND NOT A NUMBER STORED BY THE BUILDER. Hand-drawn guidelines
 * (URoadEditFacade::AddGuidelineEdge) meet derived nodes and need the same answer, and a
 * per-node figure written at build time would go stale the moment one was drawn. So it is
 * derived from the graph on demand and memoised against URoadNetwork::GetGuidelineRevision.
 *
 * SAMPLED THROUGH GuidelineGeom::Sample, the same evaluator the route polyline is built
 * from, so the reach is measured on the line the follower will actually walk.
 */
namespace NodeReach
{
	/**
	 * The reach of Node along Edge for a body Footprint long, in uu of edge distance from
	 * the node. F/2 when Edge does not meet Node, or meets nothing else there.
	 */
	AIRSIDE_API double Compute(const URoadNetwork& Network, FGuidelineNodeId Node,
		FGuidelineEdgeId Edge, double Footprint);
}

/**
 * NodeReach::Compute, memoised against the graph's revision.
 *
 * Owned by UGroundTraffic as a plain member: derived state any tick can rebuild from the
 * network, not simulation state, so it is neither a UPROPERTY nor saved. Keyed by slot
 * index rather than by generation-checked handle because a reused slot is a graph
 * mutation, and every mutation bumps the revision this whole table is dropped on.
 */
struct AIRSIDE_API FNodeReachCache
{
	double Get(const URoadNetwork& Network, FGuidelineNodeId Node, FGuidelineEdgeId Edge, double Footprint);

	/** Drop everything. OnGraphRebuilt calls it; the revision check would catch it anyway. */
	void Invalidate();

	/** Entries currently held. Test-facing. */
	int32 NumForTest() const { return Entries.Num(); }

private:
	struct FKey
	{
		int32 Node = INDEX_NONE;
		int32 Edge = INDEX_NONE;
		/** Rounded: the footprints are per class, a handful of round numbers. */
		int32 Footprint = 0;

		bool operator==(const FKey& Other) const
		{
			return Node == Other.Node && Edge == Other.Edge && Footprint == Other.Footprint;
		}
		friend uint32 GetTypeHash(const FKey& Key)
		{
			return HashCombine(HashCombine(::GetTypeHash(Key.Node), ::GetTypeHash(Key.Edge)), ::GetTypeHash(Key.Footprint));
		}
	};

	/** The network and revision the entries were computed against. */
	const URoadNetwork* For = nullptr;
	uint32 Revision = 0;
	TMap<FKey, double> Entries;
};
