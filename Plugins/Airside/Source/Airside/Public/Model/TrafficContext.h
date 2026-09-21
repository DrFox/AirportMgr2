#pragma once

#include "CoreMinimal.h"
#include "Model/NodeReach.h"
#include "Model/RunwayQuery.h"
#include "Model/TrafficOccupancy.h"
#include "Model/TrafficRules.h"

class URoadNetwork;

/**
 * The graph, the rules, the occupancy table, and the two memoised caches over the graph -
 * bundled ONCE rather than re-listed at every signature that threads them together. Issue
 * #175: FDeadlockResolver::Resolve alone took seven parameters, four of them UGroundTraffic's
 * own members, in an order a caller had to get right from memory rather than from the type
 * system - the "missing parameter object" the issue names.
 *
 * NOT EVERY CONSUMER READS EVERY FIELD - FPlanReResolver never reads Chains, and neither
 * FClaimPass nor FPlanReResolver reads SimSeconds - and that is accepted rather than split
 * further into a bundle per consumer: what makes this ONE struct is that UGroundTraffic
 * itself holds exactly these five as its own members and hands them down together at every
 * call site (Arbitrate, AdvanceOnce, OnGraphRebuilt), so a caller already has the whole
 * bundle whether or not the callee reads all of it. A struct per USE would be the same
 * fields copied into three near-identical shapes - the drift "one struct per thing" exists
 * to prevent, applied to parameter lists instead of data members.
 *
 * REFERENCES, NOT COPIES, exactly as FClaimPass's own header already documented for three of
 * these five: a claim, a replan or a resolution raised through any consumer must land in the
 * SAME occupancy table, the same reach cache and the same chain cache every other one reads
 * this tick - UGroundTraffic constructs one FTrafficContext per call and hands it down, it
 * never holds one as a member.
 */
struct FTrafficContext
{
	const URoadNetwork& Network;
	const FTrafficRules& Rules;
	FTrafficOccupancy& Occupancy;
	FNodeReachCache& Reach;
	FRunwayChainCache& Chains;

	/** UGroundTraffic::GetSimSeconds() at the moment this context was built - the deadlock
	 *  resolver's retry clock. 0 for a caller with no clock of its own (a bare test). */
	double SimSeconds = 0.0;
};
